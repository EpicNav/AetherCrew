"""Interactive terminal presentation for HyperAI's direct provider API engine."""
import base64
import json
from pathlib import Path
import sys

from hyperai_api_agent import ApiAgent, output_tokens, set_output_tokens


class TerminalOutput:
    def __init__(self, write=None):
        self.write = write or (lambda text: print(text, end="", flush=True))
        self.streamed = set()
        self.message_id = ""
        self.prefix = False

    def __call__(self, message):
        kind = message.get("type")
        if kind == "stream_event":
            event = message["event"]
            if event.get("type") == "message_start":
                self.message_id = event["message"]["id"]
                self.prefix = False
            elif event.get("type") == "content_block_delta" and event.get("delta", {}).get("type") == "text_delta":
                if not self.prefix:
                    self.write("\nAssistant:\n")
                    self.prefix = True
                self.streamed.add(self.message_id)
                self.write(event["delta"].get("text", ""))
        elif kind == "assistant":
            body = message["message"]
            for block in body.get("content", []):
                if block.get("type") == "text" and body.get("id") not in self.streamed:
                    self.write("\nAssistant:\n" + block["text"] + "\n")
                elif block.get("type") == "tool_use":
                    self.write("\n[tool] " + block["name"] + " " + json.dumps(block.get("input", {}), ensure_ascii=False)[:3000] + "\n")
        elif kind == "user":
            content = message.get("message", {}).get("content", [])
            if isinstance(content, list):
                for block in content:
                    if block.get("type") == "tool_result":
                        self.write(("[tool failed] " if block.get("is_error") else "[tool result] ") + str(block.get("content", ""))[:4000] + "\n")
        elif kind == "result":
            if message.get("is_error"):
                self.write("\n[error] " + message.get("result", "Request failed.") + "\n")
            self.write("\n")
            self.streamed.clear()


def show_history(agent, write=print):
    if not agent.log_file.exists():
        return
    with agent.log_file.open("rb") as log:
        size = log.seek(0, 2)
        log.seek(max(0, size - 2 * 1024 * 1024))
        lines = log.read().decode("utf-8", errors="replace").splitlines()
    records = []
    for line in lines:
        try:
            event = json.loads(line)
            if event.get("type") not in ("user", "assistant"):
                continue
            content = event.get("message", {}).get("content", [])
            texts = [content] if isinstance(content, str) else [p["text"] for p in content if p.get("type") == "text"]
            if texts:
                records.append(("You" if event["type"] == "user" else "Assistant", "\n".join(texts)))
        except (ValueError, KeyError, TypeError):
            continue
    for role, text in records[-12:]:
        write(f"\n{role}:\n{agent.redact(text[:6000])}")


def attachment(path, root):
    file = Path(path.strip().strip('"'))
    if not file.is_absolute():
        file = root / file
    mime = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".webp": "image/webp"}.get(file.suffix.lower())
    if not mime or file.stat().st_size > 20 * 1024 * 1024:
        raise ValueError("Attach a PNG, JPEG or WebP image of at most 20 MiB.")
    return {"type": "image", "source": {"type": "base64", "media_type": mime, "data": base64.b64encode(file.read_bytes()).decode("ascii")}}


def run_terminal(args, read=input, write=print):
    output = TerminalOutput(lambda text: write(text, end="", flush=True)) if write is print else TerminalOutput(lambda text: write(text))
    agent = None

    def approve(name, inputs, call_id):
        write("\nApproval required: " + name)
        write(agent.redact(json.dumps(inputs, ensure_ascii=False))[:6000])
        try:
            return read("Allow this action? [y/N] ").strip().lower() in ("y", "yes")
        except (EOFError, KeyboardInterrupt):
            agent.cancel.set()
            return False

    try:
        agent = ApiAgent(args.provider, args.root, args.model, args.session, args.mcp, args.read_only,
                         emit=output, approval=approve, max_output_tokens=args.max_output_tokens, ephemeral=False)
    except Exception as error:
        write("Cannot start API terminal: " + str(error))
        return 1
    if write is print and sys.stdout.isatty():
        # The embedded shell echoes its credential-loader command before starting Python.
        write("\x1b[2J\x1b[H", end="", flush=True)
    write(f"{agent.label} | {agent.model} | direct API billing")
    write(f"Conversation: {agent.session}")
    write("/tokens 0 = Auto (no HyperAI output cap); /tokens N = custom limit.")
    write("/help  /history  /model ID  /attach PATH  /paste  /exit. Ctrl+C stops a request.")
    if agent.read_only:
        write("Read-only mode: writes, commands and mutating tools are refused.")
    show_history(agent, write)
    images = []
    initial = None
    if args.prompt_file:
        try:
            source = Path(args.prompt_file)
            initial = source.read_text(encoding="utf-8-sig")
            if source.suffix.lower() == ".json":
                opening = json.loads(initial)
                images = [attachment(path, agent.root) for path in opening.get("images", [])]
                initial = opening.get("text", "") or ("Describe the attached images." if images else "")
        except (OSError, ValueError, AttributeError) as error:
            write("[error] Opening prompt could not be loaded: " + agent.redact(str(error)))
            initial, images = None, []
    while True:
        try:
            line = initial if initial is not None else read("\nYou > ")
            initial = None
            line = line.strip()
            if not line:
                continue
            if line in ("/exit", "/quit"):
                break
            if line == "/help":
                write("Type a prompt and press Enter. /paste accepts multiple lines until a line containing only a dot.")
                write("/tokens [0|N] changes the shared project output limit; 0 selects Auto, provider limits still apply.")
                write("/model ID selects a model for this conversation. /attach PATH adds an image to your next prompt.")
                write("/history shows recent messages. /exit saves the conversation. Ctrl+C stops the current request.")
                continue
            if line == "/history":
                show_history(agent, write)
                continue
            if line == "/tokens" or line.startswith("/tokens "):
                parts = line.split(maxsplit=1)
                if len(parts) > 1:
                    set_output_tokens(agent.root, int(parts[1]))
                    agent.max_output_tokens = None
                limit = output_tokens(agent.root) if agent.max_output_tokens is None else agent.max_output_tokens
                write("Max output tokens: " + (str(limit) if limit else "Auto (Claude model maximum; other provider defaults)"))
                continue
            if line.startswith("/model "):
                model = line[7:].strip()
                if not model:
                    raise ValueError("Provide an API model id.")
                agent.model = model
                write("Model: " + agent.model)
                continue
            if line.startswith("/attach "):
                images.append(attachment(line[8:], agent.root))
                write("Image attached to the next prompt.")
                continue
            if line == "/paste":
                write("Enter text; finish with a line containing only a dot:")
                lines = []
                while True:
                    part = read("")
                    if part == ".":
                        break
                    lines.append(part)
                line = "\n".join(lines)
            elif line.startswith("/"):
                write("Unknown command. Use /help.")
                continue
            agent.cancel.clear()
            content = [*images, {"type": "text", "text": line}]
            images = []
            agent.turn(content)
        except EOFError:
            break
        except KeyboardInterrupt:
            agent.cancel.set()
            agent.save()
            write("\nStopped. Completed conversation history is saved.")
        except Exception as error:
            write("[error] " + agent.redact(str(error)))
    agent.save()
    write("Conversation saved: " + agent.session)
    return 0
