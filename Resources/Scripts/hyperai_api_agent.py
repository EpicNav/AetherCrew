"""HyperAI's direct API agent. Standard library only; no provider CLI or account/balance probes.

The editor-facing JSONL protocol reuses HyperAI's existing transcript/approval renderer.
Provider credentials are read only from the child environment and used only in HTTP headers.
"""
from __future__ import annotations

import argparse
import copy
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import uuid

PROVIDERS = {
    "anthropic": ("Claude API", "ANTHROPIC_API_KEY", "ANTHROPIC_BASE_URL", "https://api.anthropic.com", "claude-opus-5-5"),
    "openai": ("OpenAI API", "OPENAI_API_KEY", "OPENAI_BASE_URL", "https://api.openai.com/v1", "gpt-5.4"),
    "gemini": ("Gemini API", "GEMINI_API_KEY", "GOOGLE_GEMINI_BASE_URL", "https://generativelanguage.googleapis.com", "gemini-3.8-flash"),
}
MAX_BODY = 32 * 1024 * 1024
OPTIONS_FILE = ".hyperai/runtime/api-agent-options.json"
# Anthropic requires max_tokens even in Auto mode. Current documented model output maxima.
ANTHROPIC_OUTPUT_LIMITS = {name: 128000 for name in
    ("claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5", "claude-fable-5-1", "claude-opus-4-6")}
SYSTEM = """You are HyperAI's direct API agent inside Unreal Editor. Fulfil the user's task using actual tool results.
Use unreal_list_tools to discover Unreal MCP tools and their schemas, then unreal_call_tool to invoke them.
Unreal MCP's call_tool tool dispatches toolset_name/tool_name/arguments; use its discovery tools when necessary.
Read/write source files only through the project tools. Binary Unreal assets must be edited through Unreal MCP.
Before editing a project, read its root AGENTS.md through read_file when that file exists.
Changes and commands require user approval. A denial is final: report it instead of trying another route.
Never claim an action succeeded without a successful result. Do not request or print API keys.
If a capability is unavailable, say exactly what failed. API usage is billed directly to the configured provider key.
"""


class Cancelled(Exception):
    pass


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise RuntimeError("HTTP redirect refused; configure the final API endpoint explicitly.")


def validate_url(url, local_only=False):
    parsed = urllib.parse.urlsplit(url)
    local = parsed.hostname in ("localhost", "127.0.0.1", "::1")
    if (parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username
            or parsed.password or parsed.query or parsed.fragment or (local_only and not local)
            or (parsed.scheme == "http" and not local)):
        raise ValueError("Use an HTTPS endpoint, or HTTP on localhost, without embedded credentials or query parameters.")
    return url.rstrip("/")


def post_json(url, body, headers, timeout=180, stream_handler=None):
    request = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"),
                                     headers={"Content-Type": "application/json", **headers}, method="POST")
    try:
        with urllib.request.build_opener(NoRedirect).open(request, timeout=timeout) as response:
            if stream_handler and "text/event-stream" in response.headers.get("Content-Type", ""):
                return stream_handler(response), dict(response.headers)
            raw = response.read(MAX_BODY + 1)
            if len(raw) > MAX_BODY:
                raise RuntimeError("Response exceeds the 32 MiB limit.")
            if response.status == 202 or not raw.strip():
                return {}, dict(response.headers)
            if "text/event-stream" in response.headers.get("Content-Type", ""):
                # Unreal MCP may use SSE for the single JSON-RPC response.
                for event in re.split(r"\r?\n\r?\n", raw.decode("utf-8")):
                    data = "\n".join(line[5:].lstrip() for line in event.splitlines() if line.startswith("data:"))
                    if data and data != "[DONE]":
                        message = json.loads(data)
                        if message.get("id") == body.get("id"):
                            return message, dict(response.headers)
                raise RuntimeError("The MCP stream contained no matching response.")
            return json.loads(raw), dict(response.headers)
    except urllib.error.HTTPError as error:
        raw = error.read(16384).decode("utf-8", errors="replace")
        error.close()
        try:
            detail = json.loads(raw).get("error", {})
            detail = detail.get("message", str(detail)) if isinstance(detail, dict) else str(detail)
        except (ValueError, AttributeError):
            detail = "The endpoint rejected the request."
        raise RuntimeError(f"HTTP {error.code}: {detail[:2000]}") from None


def output_tokens(root):
    path = Path(root) / OPTIONS_FILE
    if not path.exists():
        return 0
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig")).get("max_output_tokens", 0)
        if type(value) is not int or value < 0:
            raise ValueError()
        return value
    except (ValueError, OSError, AttributeError):
        raise ValueError("Invalid API output settings. Set Max output tokens in Settings > Agents; 0 selects Auto.") from None


def set_output_tokens(root, value):
    if type(value) is not int or value < 0:
        raise ValueError("Output tokens must be a non-negative integer; 0 selects Auto.")
    path = Path(root) / OPTIONS_FILE
    current = json.loads(path.read_text(encoding="utf-8-sig")) if path.exists() else {}
    current["max_output_tokens"] = value
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(".tmp-" + uuid.uuid4().hex)
    temp.write_text(json.dumps(current), encoding="utf-8")
    temp.replace(path)


def anthropic_stream(response, agent):
    message, partial, completed, invalid_input = None, {}, False, False
    event_lines, total = [], 0
    for raw in response:
        agent.check_cancel()
        total += len(raw)
        if total > MAX_BODY:
            raise RuntimeError("API stream exceeds the 32 MiB limit.")
        line = raw.decode("utf-8").rstrip("\r\n")
        if line:
            if line.startswith("data:"):
                event_lines.append(line[5:].lstrip())
            continue
        if not event_lines:
            continue
        event = json.loads("\n".join(event_lines))
        event_lines = []
        kind = event.get("type")
        if kind == "error":
            raise RuntimeError(str(event.get("error", {}).get("message", "API streaming error")))
        if kind == "message_start":
            message = copy.deepcopy(event["message"])
            message.setdefault("content", [])
        elif kind == "content_block_start" and message is not None:
            index = event["index"]
            if index != len(message["content"]):
                raise RuntimeError("Invalid content-block order in API stream.")
            message["content"].append(copy.deepcopy(event["content_block"]))
        elif kind == "content_block_delta" and message is not None:
            index, delta = event["index"], event["delta"]
            block = message["content"][index]
            field = {"text_delta": "text", "thinking_delta": "thinking", "signature_delta": "signature"}.get(delta.get("type"))
            if field:
                block[field] = block.get(field, "") + delta.get(field, "")
            elif delta.get("type") == "input_json_delta":
                partial[index] = partial.get(index, "") + delta.get("partial_json", "")
            elif delta.get("type") == "citations_delta":
                block.setdefault("citations", []).append(delta["citation"])
        elif kind == "content_block_stop" and message is not None and event["index"] in partial:
            try:
                message["content"][event["index"]]["input"] = json.loads(partial.pop(event["index"]))
            except ValueError:
                invalid_input = True
        elif kind == "message_delta" and message is not None:
            message.update(event.get("delta", {}))
            message.setdefault("usage", {}).update(event.get("usage", {}))
        elif kind == "message_stop":
            completed = True
        agent.emit({"type": "stream_event", "event": event})
        if completed:
            break
    if not completed or message is None or partial:
        raise RuntimeError("API stream ended before the message was complete. No pending tools were run.")
    if invalid_input and message.get("stop_reason") != "max_tokens":
        raise RuntimeError("API stream returned incomplete tool arguments. No pending tools were run.")
    return message


class McpClient:
    def __init__(self, endpoint):
        self.endpoint = validate_url(endpoint, local_only=True) if endpoint else ""
        self.session = ""
        self.sequence = 0
        self.tools = None

    def rpc(self, method, params=None, notification=False, timeout=10):
        if not self.endpoint:
            raise RuntimeError("Unreal MCP is not configured for this chat.")
        self.sequence += 1
        body = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        if not notification:
            body["id"] = self.sequence
        headers = {"Accept": "application/json, text/event-stream", "MCP-Protocol-Version": "2025-03-26"}
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        result, response_headers = post_json(self.endpoint, body, headers, timeout)
        for name, value in response_headers.items():
            if name.lower() == "mcp-session-id":
                self.session = value
        if result.get("error"):
            raise RuntimeError(str(result["error"].get("message", "Unreal MCP request failed")))
        return result.get("result", {})

    def discover(self):
        if self.tools is not None:
            return self.tools
        self.rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                               "clientInfo": {"name": "hyperai-direct-api", "version": "1"}})
        self.rpc("notifications/initialized", notification=True)
        tools, cursor, seen = {}, None, set()
        for _ in range(100):
            result = self.rpc("tools/list", {"cursor": cursor} if cursor else {})
            for tool in result.get("tools", []):
                tools[tool["name"]] = tool
            cursor = result.get("nextCursor")
            if not cursor:
                self.tools = tools
                return tools
            if cursor in seen:
                break
            seen.add(cursor)
        raise RuntimeError("Unreal MCP tool pagination did not complete.")


def schema(properties, required=()):
    return {"type": "object", "properties": properties, "required": list(required), "additionalProperties": False}


TEXT = {"type": "string"}
TOOLS = [
    ("unreal_list_tools", "Find advertised Unreal MCP tool schemas by name/description. Use offset to page results.",
     schema({"query": TEXT, "offset": {"type": "integer", "minimum": 0}})),
    ("unreal_call_tool", "Invoke an advertised Unreal MCP tool. Mutating/unknown tools require approval.",
     schema({"name": TEXT, "arguments": {"type": "object"}}, ("name", "arguments"))),
    ("list_files", "List a project directory; returns at most 200 entries, without following links outside the project.",
     schema({"path": TEXT})),
    ("read_file", "Read a UTF-8 project source/text file, at most 256 KiB. Use Unreal MCP for binary assets.",
     schema({"path": TEXT}, ("path",))),
    ("write_file", "Write a UTF-8 project source/text file after explicit approval. Use Unreal MCP for binary assets.",
     schema({"path": TEXT, "content": TEXT}, ("path", "content"))),
    ("run_command", "Run a command from the project directory after explicit approval; timeout 120 seconds.",
     schema({"command": TEXT}, ("command",))),
]


class ApiAgent:
    def __init__(self, provider, root, model="", session="", mcp="", read_only=False, emit=None, approval=None,
                 max_output_tokens=None, ephemeral=None):
        self.provider = provider
        self.label, key_var, base_var, default_base, default_model = PROVIDERS[provider]
        self.key = os.environ.get(key_var, "").strip()
        if not self.key:
            raise ValueError(f"Set {key_var} in Settings > Agents before starting {self.label}.")
        self.base = validate_url(os.environ.get(base_var, "").strip() or default_base)
        self.root = Path(root).resolve()
        self.model = model or default_model
        self.session = session or str(uuid.uuid4())
        if not re.fullmatch(r"[A-Za-z0-9-]{1,64}", self.session):
            raise ValueError("Invalid API conversation id.")
        self.folder = self.root / ".hyperai" / "runtime" / "api-chats" / provider
        self.state_file = self.folder / (self.session + ".json")
        self.log_file = self.folder / (self.session + ".jsonl")
        self.history = []
        self.title = "API chat"
        self.read_only = read_only
        self.ephemeral = read_only if ephemeral is None else ephemeral
        if max_output_tokens is not None and (type(max_output_tokens) is not int or max_output_tokens < 0):
            raise ValueError("Output tokens must be non-negative; 0 selects Auto.")
        self.max_output_tokens = max_output_tokens
        self.model_output_limits = {}
        self.extra_system = ""
        self.cancel = threading.Event()
        self.mcp = McpClient(mcp)
        self.output = emit or (lambda value: print(json.dumps(value, ensure_ascii=False), flush=True))
        self.approval = approval or (lambda name, args, call_id: False)
        if self.state_file.exists():
            if self.state_file.stat().st_size > MAX_BODY:
                raise ValueError("Saved API conversation is too large to resume.")
            saved = json.loads(self.state_file.read_text(encoding="utf-8"))
            if saved.get("provider") != provider or saved.get("session_id") != self.session:
                raise ValueError("Saved conversation does not match this API provider.")
            self.history = saved["messages"]
            self.title = saved.get("title", self.title)
            if not model:
                self.model = saved.get("model", self.model)

    def redact(self, text):
        for variable in ("ANTHROPIC_API_KEY", "OPENAI_API_KEY", "GEMINI_API_KEY"):
            secret = os.environ.get(variable, "")
            if secret:
                text = text.replace(secret, "[API key redacted]")
        return text

    def emit(self, value, record=False):
        value = json.loads(self.redact(json.dumps({**value, "session_id": self.session}, ensure_ascii=False)))
        self.output(value)
        if record and not self.ephemeral:
            self.folder.mkdir(parents=True, exist_ok=True)
            with self.log_file.open("a", encoding="utf-8") as log:
                log.write(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n")

    def save(self):
        if self.ephemeral:
            return
        self.folder.mkdir(parents=True, exist_ok=True)
        state = {"provider": self.provider, "model": self.model, "session_id": self.session,
                 "title": self.title, "messages": self.history}
        text = self.redact(json.dumps(state, ensure_ascii=False))
        if len(text.encode("utf-8")) > MAX_BODY:
            raise RuntimeError("Conversation exceeds the 32 MiB history limit. Start a new chat.")
        temp = self.state_file.with_suffix(".tmp")
        temp.write_text(text, encoding="utf-8")
        temp.replace(self.state_file)

    def check_cancel(self):
        if self.cancel.is_set():
            raise Cancelled("Stopped by the user.")

    def request(self):
        self.check_cancel()
        system = SYSTEM + "\n" + self.extra_system
        if self.read_only:
            system += "\nThis session is read-only. Do not attempt edits or commands."
        limit = output_tokens(self.root) if self.max_output_tokens is None else self.max_output_tokens
        if self.provider == "anthropic":
            if limit == 0:
                limit = ANTHROPIC_OUTPUT_LIMITS.get(self.model) or self.model_output_limits.get(self.model)
                if not limit:
                    url = self.base + ("/models/" if self.base.endswith("/v1") else "/v1/models/") + urllib.parse.quote(self.model, safe="-")
                    request = urllib.request.Request(url, headers={"x-api-key": self.key, "anthropic-version": "2023-06-01"})
                    try:
                        with urllib.request.build_opener(NoRedirect).open(request, timeout=15) as response:
                            limit = json.loads(response.read(1024 * 1024)).get("max_tokens")
                        if type(limit) is not int or limit <= 0:
                            raise ValueError()
                        self.model_output_limits[self.model] = limit
                    except Exception:
                        raise ValueError("Auto could not determine this Claude model's output maximum. Set Max output tokens explicitly or use /tokens N in the terminal.") from None
            url = self.base + ("/messages" if self.base.endswith("/v1") else "/v1/messages")
            body = {"model": self.model, "max_tokens": limit, "stream": True, "system": system, "messages": self.history,
                    "tools": [{"name": n, "description": d, "input_schema": s} for n, d, s in TOOLS]}
            headers = {"x-api-key": self.key, "anthropic-version": "2023-06-01"}
        elif self.provider == "openai":
            url = self.base + "/responses"
            body = {"model": self.model, "instructions": system,
                    "input": self.history, "store": False, "include": ["reasoning.encrypted_content"],
                    "tools": [{"type": "function", "name": n, "description": d, "parameters": s, "strict": False} for n, d, s in TOOLS]}
            headers = {"Authorization": "Bearer " + self.key}
            if limit:
                body["max_output_tokens"] = limit
        else:
            base = self.base if self.base.endswith("/v1beta") else self.base + "/v1beta"
            url = base + "/models/" + urllib.parse.quote(self.model, safe="-") + ":generateContent"
            body = {"contents": self.history, "systemInstruction": {"parts": [{"text": system}]},
                    "tools": [{"functionDeclarations": [{"name": n, "description": d, "parametersJsonSchema": s} for n, d, s in TOOLS]}]}
            headers = {"x-goog-api-key": self.key}
            if limit:
                body["generationConfig"] = {"maxOutputTokens": limit}
        result, _ = post_json(url, body, headers, stream_handler=(lambda response: anthropic_stream(response, self)) if self.provider == "anthropic" else None)
        self.check_cancel()
        if result.get("error"):
            raise RuntimeError(str(result["error"]))
        return result

    def parse_response(self, result):
        blocks, calls = [], []
        if self.provider == "anthropic":
            raw = result.get("content", [])
            self.history.append({"role": "assistant", "content": raw})
            for item in raw:
                if item.get("type") == "text":
                    blocks.append(item)
                elif item.get("type") == "tool_use":
                    calls.append({"id": item["id"], "name": item["name"], "input": item["input"]})
        elif self.provider == "openai":
            if result.get("status") not in (None, "completed"):
                raise RuntimeError("OpenAI response did not complete: " + str(result.get("incomplete_details") or result.get("status")))
            self.history.extend(result.get("output", []))
            for item in result.get("output", []):
                if item.get("type") == "message":
                    for part in item.get("content", []):
                        if part.get("type") == "output_text":
                            blocks.append({"type": "text", "text": part["text"]})
                        elif part.get("type") == "refusal":
                            blocks.append({"type": "text", "text": part.get("refusal", "Request refused.")})
                elif item.get("type") == "function_call":
                    calls.append({"id": item["call_id"], "name": item["name"], "input": json.loads(item["arguments"])})
        else:
            candidates = result.get("candidates", [])
            if not candidates or candidates[0].get("finishReason") not in (None, "STOP"):
                raise RuntimeError("Gemini did not complete the response: " + str(result.get("promptFeedback") or candidates))
            content = candidates[0].get("content", {"role": "model", "parts": []})
            self.history.append(content)  # Keep thoughtSignature and function-call ids exactly as returned.
            for part in content.get("parts", []):
                if "text" in part and not part.get("thought"):
                    blocks.append({"type": "text", "text": part["text"]})
                if "functionCall" in part:
                    call = part["functionCall"]
                    calls.append({"id": call.get("id") or str(uuid.uuid4()), "name": call["name"],
                                  "input": call.get("args", {}), "provider_id": call.get("id")})
        blocks.extend({"type": "tool_use", "id": c["id"], "name": c["name"], "input": c["input"]} for c in calls)
        if not blocks:
            raise RuntimeError("The API returned no usable text or tool calls.")
        self.emit({"type": "assistant", "message": {"id": result.get("id", str(uuid.uuid4())),
                   "role": "assistant", "content": blocks}}, record=True)
        if self.provider == "anthropic" and result.get("stop_reason") == "max_tokens":
            if calls:
                reason = "Output-token limit reached; this tool was not run. Ask to continue."
                self.add_tool_results([(call, reason, True) for call in calls])
                self.emit({"type": "user", "message": {"role": "user", "content": [
                    {"type": "tool_result", "tool_use_id": call["id"], "content": reason, "is_error": True}
                    for call in calls]}}, record=True)
            raise RuntimeError("Reached the selected or model output-token limit. Partial text was retained. Change Max output tokens or ask to continue; incomplete tools were not run.")
        return calls

    def project_path(self, value):
        path = (self.root / value).resolve()
        if not path.is_relative_to(self.root):
            raise ValueError("File access is restricted to this project.")
        return path

    def tool(self, call):
        self.check_cancel()
        name, args = call["name"], call["input"]
        if name == "unreal_list_tools":
            query_text = str(args.get("query", "")).lower()
            tools = [t for t in self.mcp.discover().values() if query_text in (t["name"] + " " + t.get("description", "")).lower()]
            offset = max(0, int(args.get("offset", 0)))
            return {"tools": tools[offset:offset+12], "nextOffset": offset+12 if offset+12 < len(tools) else None}
        if name == "list_files":
            path = self.project_path(args.get("path", "."))
            return {"entries": [p.name + ("/" if p.is_dir() else "") for p in sorted(path.iterdir())[:200]]}
        if name == "read_file":
            path = self.project_path(args["path"])
            if path.stat().st_size > 256 * 1024:
                raise ValueError("File exceeds 256 KiB. Request a smaller source file or use Unreal's discovery tools.")
            return {"text": path.read_text(encoding="utf-8-sig")}
        if name not in ("write_file", "run_command", "unreal_call_tool"):
            raise ValueError("Unknown tool: " + name)
        readonly = False
        if name == "unreal_call_tool":
            advertised = self.mcp.discover().get(args["name"])
            if not advertised:
                raise ValueError("Tool was not advertised by Unreal MCP. Discover it first.")
            readonly = advertised.get("annotations", {}).get("readOnlyHint") is True
        if not readonly:
            if self.read_only:
                raise PermissionError("This API session is read-only; the action was not run.")
            if not self.approval(name, args, call["id"]):
                raise PermissionError("The user denied the action. It was not run; do not try another route.")
        self.check_cancel()
        if name == "unreal_call_tool":
            result = self.mcp.rpc("tools/call", {"name": args["name"], "arguments": args["arguments"]}, timeout=900)
            if result.get("isError"):
                raise RuntimeError(json.dumps(result, ensure_ascii=False))
            return result
        if name == "write_file":
            path = self.project_path(args["path"])
            if path.suffix.lower() in (".uasset", ".umap", ".dll", ".exe", ".pdb"):
                raise ValueError("Use Unreal MCP to edit assets; this tool writes source/text files only.")
            content = args["content"]
            if len(content.encode("utf-8")) > 256 * 1024:
                raise ValueError("Write exceeds 256 KiB.")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            return {"written": str(path.relative_to(self.root))}
        env = {k: v for k, v in os.environ.items() if not any(word in k.upper() for word in ("API_KEY", "AUTH_TOKEN", "OAUTH_TOKEN", "SESSION_AUTH"))}
        result = subprocess.run(args["command"], cwd=self.root, shell=True, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=120, env=env)
        return {"exitCode": result.returncode, "output": (result.stdout + result.stderr)[-16000:]}

    def add_tool_results(self, results):
        if self.provider == "anthropic":
            self.history.append({"role": "user", "content": [{"type": "tool_result", "tool_use_id": c["id"],
                                  "content": text, "is_error": error} for c, text, error in results]})
        elif self.provider == "openai":
            self.history.extend({"type": "function_call_output", "call_id": c["id"], "output": text} for c, text, _ in results)
        else:
            parts = []
            for call, text, error in results:
                response = {"name": call["name"], "response": {"error" if error else "result": text}}
                if call.get("provider_id"):
                    response["id"] = call["provider_id"]
                parts.append({"functionResponse": response})
            self.history.append({"role": "user", "parts": parts})

    def user_message(self, content):
        if isinstance(content, str):
            content = [{"type": "text", "text": content}]
        if self.provider == "anthropic":
            return {"role": "user", "content": content}
        parts = []
        for part in content:
            if part.get("type") == "text":
                parts.append({"type": "input_text", "text": part["text"]} if self.provider == "openai" else {"text": part["text"]})
            elif part.get("type") == "image":
                image = part["source"]
                parts.append({"type": "input_image", "image_url": f"data:{image['media_type']};base64,{image['data']}"}
                             if self.provider == "openai" else {"inlineData": {"mimeType": image["media_type"], "data": image["data"]}})
        return {"role": "user", "content" if self.provider == "openai" else "parts": parts}

    def turn(self, content):
        try:
            self.history.append(self.user_message(content))
            texts = [p.get("text", "") for p in content if p.get("type") == "text"] if isinstance(content, list) else [content]
            if self.title == "API chat":
                self.title = " ".join(texts).split("\n")[0][:90]
            self.emit({"type": "user", "message": {"role": "user", "content": content}}, record=True)
            self.save()
            last = {}
            for _ in range(24):
                last = self.request()
                calls = self.parse_response(last)
                if not calls:
                    self.save()
                    self.emit({"type": "result", "subtype": "success", "is_error": False,
                               "usage": last.get("usage", last.get("usageMetadata", {})), "result": ""}, record=True)
                    return
                results = []
                for call in calls:
                    try:
                        value = self.tool(call)
                        text, error = self.redact(json.dumps(value, ensure_ascii=False))[:60000], False
                    except (Exception, KeyboardInterrupt) as failure:
                        if isinstance(failure, KeyboardInterrupt):
                            self.cancel.set()
                            failure = Cancelled("Stopped by the user. This tool did not complete.")
                        text, error = self.redact(str(failure))[:60000], True
                    results.append((call, text, error))
                    self.emit({"type": "user", "message": {"role": "user", "content": [{"type": "tool_result",
                               "tool_use_id": call["id"], "content": text, "is_error": error}]}}, record=True)
                self.add_tool_results(results)
                self.save()
                self.check_cancel()
            raise RuntimeError("Stopped after 24 model/tool rounds. Review the results before continuing.")
        except Exception as error:
            try:
                self.save()
            except Exception:
                pass
            self.emit({"type": "result", "subtype": "error_during_execution", "is_error": True,
                       "result": self.redact(str(error))}, record=True)


def main():
    sys.stdin.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider", choices=PROVIDERS, required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--model", default="")
    parser.add_argument("--session", default="")
    parser.add_argument("--mcp", default="")
    parser.add_argument("--read-only", action="store_true")
    parser.add_argument("--max-output-tokens", type=int, default=None)
    parser.add_argument("--terminal", action="store_true")
    parser.add_argument("--prompt-file", default="")
    args = parser.parse_args()
    if args.terminal:
        from hyperai_api_terminal import run_terminal
        return run_terminal(args)
    lock = threading.Lock()
    pending = {}
    worker = None
    agent = None

    def emit(value):
        with lock:
            print(json.dumps(value, ensure_ascii=False), flush=True)

    def approve(name, inputs, call_id):
        event = queue.Queue(maxsize=1)
        request_id = "api-approval-" + call_id
        pending[request_id] = event
        tool_name = "Write" if name == "write_file" else "Bash" if name == "run_command" else "unreal_call_tool"
        display = {"file_path": str(agent.project_path(inputs["path"])), "content": inputs["content"]} if name == "write_file" else inputs
        agent.emit({"type": "control_request", "request_id": request_id,
                    "request": {"subtype": "can_use_tool", "tool_name": tool_name, "input": display,
                                "tool_use_id": call_id, "suppress_always_allow_rule": True}})
        try:
            while not agent.cancel.is_set():
                try:
                    response = event.get(timeout=0.2)
                    return response.get("behavior") == "allow"
                except queue.Empty:
                    continue
            agent.emit({"type": "control_cancel_request", "request_id": request_id})
            return False
        finally:
            pending.pop(request_id, None)

    for line in sys.stdin:
        try:
            message = json.loads(line)
            if message.get("type") == "control_response":
                reply = message.get("response", {})
                if reply.get("request_id") in pending:
                    pending[reply["request_id"]].put_nowait(reply.get("response", {}))
            elif message.get("type") == "control_request":
                request_id = message["request_id"]
                request = message.get("request", {})
                if request.get("subtype") == "initialize":
                    try:
                        agent = ApiAgent(args.provider, args.root, args.model, args.session, args.mcp,
                                         args.read_only, emit=emit, approval=approve, max_output_tokens=args.max_output_tokens)
                        agent.extra_system = request.get("systemPrompt", "")
                        emit({"type": "control_response", "response": {"subtype": "success", "request_id": request_id, "response": {}}})
                        agent.emit({"type": "system", "subtype": "init", "model": agent.model})
                    except Exception as error:
                        emit({"type": "control_response", "response": {"subtype": "error", "request_id": request_id, "error": str(error)}})
                elif request.get("subtype") == "interrupt" and agent:
                    agent.cancel.set()
                    emit({"type": "control_response", "response": {"subtype": "success", "request_id": request_id, "response": {}}})
            elif message.get("type") == "user" and agent:
                if worker and worker.is_alive():
                    agent.emit({"type": "result", "is_error": True, "result": "The previous API request is still stopping. Retry after it finishes."})
                    continue
                agent.cancel.clear()
                agent.read_only = args.read_only or bool(message.get("api_read_only", False))
                worker = threading.Thread(target=agent.turn, args=(message["message"]["content"],), daemon=True)
                worker.start()
        except (ValueError, KeyError, queue.Full):
            emit({"type": "result", "is_error": True, "result": "Invalid editor/agent message."})
    if agent:
        agent.cancel.set()


if __name__ == "__main__":
    sys.exit(main() or 0)
