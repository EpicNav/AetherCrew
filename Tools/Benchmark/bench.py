"""Benchmark harness: five Unreal tasks, an unattended agent, strict verification, request accounting.

Modelled on NeoStack's public UE 5.8 pilot (15 minutes and 45 state-changing requests per task; a pass
needs saved state, validation, runtime proof where it applies, and a visual check), so HyperAIStudio can
be measured the same way instead of guessed at.

Runs happen in a dedicated project, never in a game project: every run wipes /Game/Bench and kills the
editor, which is exactly what makes "saved" provable, and exactly what must never touch real work.

    python Tools/Benchmark/bench.py setup            create or refresh the bench project and build it
    python Tools/Benchmark/bench.py editor start     launch the bench editor and wait for MCP
    python Tools/Benchmark/bench.py editor stop
    python Tools/Benchmark/bench.py run --agent codex --task all
    python Tools/Benchmark/bench.py report

Standard library only.
"""

import argparse
import filecmp
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

PLUGIN_ROOT = Path(__file__).resolve().parents[2]
PLUGIN_FOLDER = PLUGIN_ROOT.name
ENGINE_ROOT = Path(os.environ.get("HYPERAI_BENCH_ENGINE", r"E:\EpicGames\UE_5.8"))
BENCH_ROOT = Path(os.environ.get("HYPERAI_BENCH_PROJECT", r"E:\Unreal Projects\HyperAIBench"))
BENCH_NAME = BENCH_ROOT.name
MCP_PORT = int(os.environ.get("HYPERAI_BENCH_PORT", "8010"))
MCP_URL = f"http://127.0.0.1:{MCP_PORT}/mcp"
TEMPLATE = "TP_ThirdPersonBP"
# The template's content packs, mounted under /Game/<name> exactly as the new-project wizard does.
CONTENT_PACKS = ("LevelPrototyping", "Characters", "Input")
RUNS_DIR = Path(__file__).resolve().parent / "runs"
TASKS_DIR = Path(__file__).resolve().parent / "tasks"
# Task id, automation test that verifies it, and the /Game/Bench folder it owns.
TASKS = (
    ("01-niagara", "HyperAIBench.Verify.01Niagara", "Niagara"),
    ("02-material", "HyperAIBench.Verify.02Material", "Material"),
    ("03-level", "HyperAIBench.Verify.03Level", "Level"),
    ("04-umg", "HyperAIBench.Verify.04UMG", "UI"),
    ("05-animbp", "HyperAIBench.Verify.05AnimBP", "Anim"),
)

# HyperAIStudio lists these as optional plugin dependencies; a project has to enable them itself, or the
# modules that need them cannot build. The toolset plugins are Epic's, which HyperAIStudio builds on.
REQUIRED_PLUGINS = (
    "ActorModifier", "ActorModifierCore", "DynamicMaterial", "EnhancedInput", "GameFeatures",
    "GameplayAbilities", "GeometryScripting", "Interchange", "MassAI", "MassGameplay", "Metasound",
    "ModelViewViewModel", "MovieRenderPipeline", "Niagara", "PCG", "Paper2D", "PropertyAnimator",
    "PropertyAnimatorCore", "RigVM", "TextureGraph", "UAF", "UAFAnimGraph", "WorldConditions",
    "ModelContextProtocol", "ToolsetRegistry", "EditorToolset", "Terminal",
    "AIModuleToolset", "AnimationAssistantToolset", "AutomationTestToolset", "ConfigSettingsToolset",
    "ConversationToolset", "DataRegistryToolset", "GameFeaturesToolset", "GameplayTagsToolset",
    "GASToolsets", "LiveCodingToolset", "NiagaraToolsets", "PCGToolset", "PhysicsToolsets",
    "PluginToolset", "SlateInspectorToolset", "StateTreeToolset", "UMGToolSet",
    "WorldConditionsToolset",
)

HYPERAI_SETTINGS = {
    "bShowWelcomeOnStartup": "False",
    "bResumeSetupAfterRestart": "False",
    "UnrealMCPPort": str(MCP_PORT),
    "UnrealMCPUrlPath": "/mcp",
    "bAutoStartUnrealMCP": "True",
    "bEnableToolSearch": "True",
    "NativeExecutionMode": "Fast",
    "NativeToolChannel": "Preview",
    "CapabilityPreset": "Everything",
    "bEnablePythonScripting": "True",
    "bGenerateCodexConfig": "True",
    "bGenerateClaudeConfig": "True",
    "bGenerateClaudeMarkdown": "True",
    "bEnableCodexAgent": "True",
    "bEnableClaudeAgent": "True",
    # Nobody is at the desk to click Approve. Destructive and external-effect tools still need a click
    # whatever this says, so a run that reaches one is recorded as blocked, which is itself a finding.
    "bRequireApprovalForAgentEdits": "False",
}
MCP_SETTINGS = {
    "ServerUrlPath": "/mcp",
    "ServerPortNumber": str(MCP_PORT),
    "bAutoStartServer": "True",
    "bEnableToolSearch": "True",
}


def log(message):
    print(f"[bench] {message}", flush=True)


# ---------------------------------------------------------------------------------------------- ini


def set_ini_section(path, section, values):
    """Sets simple keys in one section, keeping everything else in the file as it was."""
    lines = path.read_text(encoding="utf-8-sig").splitlines() if path.exists() else []
    header = f"[{section}]"
    start = next((i for i, line in enumerate(lines) if line.strip() == header), None)
    if start is None:
        if lines and lines[-1].strip():
            lines.append("")
        lines.append(header)
        lines.extend(f"{key}={value}" for key, value in values.items())
    else:
        end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("[")), len(lines))
        body = lines[start + 1:end]
        remaining = dict(values)
        for i, line in enumerate(body):
            key = line.split("=", 1)[0].strip()
            if key in remaining:
                body[i] = f"{key}={remaining.pop(key)}"
        insert_at = len(body)
        while insert_at > 0 and not body[insert_at - 1].strip():
            insert_at -= 1
        body[insert_at:insert_at] = [f"{key}={value}" for key, value in remaining.items()]
        lines[start + 1:end] = body
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# ------------------------------------------------------------------------------------------- setup


def uproject_path():
    return BENCH_ROOT / f"{BENCH_NAME}.uproject"


def create_project():
    template = ENGINE_ROOT / "Templates" / TEMPLATE
    if not template.is_dir():
        raise SystemExit(f"No template at {template}.")
    log(f"Creating {BENCH_ROOT} from {TEMPLATE}.")
    shutil.copytree(template / "Config", BENCH_ROOT / "Config",
                    ignore=shutil.ignore_patterns("TemplateDefs.ini"))
    shutil.copytree(template / "Content", BENCH_ROOT / "Content")
    for pack in CONTENT_PACKS:
        source = ENGINE_ROOT / "Templates" / "TemplateResources" / "High" / pack / "Content"
        shutil.copytree(source, BENCH_ROOT / "Content" / pack)
    template_project = json.loads((template / f"{TEMPLATE}.uproject").read_text(encoding="utf-8-sig"))
    project = {
        "FileVersion": 3,
        "EngineAssociation": "5.8",
        "Category": "",
        "Description": "HyperAIStudio benchmark project. Disposable: every run wipes /Game/Bench.",
        "Plugins": template_project.get("Plugins", []),
    }
    uproject_path().write_text(json.dumps(project, indent="\t") + "\n", encoding="utf-8")


def ensure_plugins():
    project = json.loads(uproject_path().read_text(encoding="utf-8-sig"))
    plugins = project.setdefault("Plugins", [])
    enabled = {plugin["Name"] for plugin in plugins}
    added = [name for name in REQUIRED_PLUGINS if name not in enabled]
    plugins.extend({"Name": name, "Enabled": True} for name in added)
    if added:
        uproject_path().write_text(json.dumps(project, indent="\t") + "\n", encoding="utf-8")
        log(f"Enabled {len(added)} plugins: {', '.join(added)}")


def ensure_settings():
    ini = BENCH_ROOT / "Config" / "DefaultEditorPerProjectUserSettings.ini"
    set_ini_section(ini, "/Script/HyperAIStudio.HyperAIStudioSettings", HYPERAI_SETTINGS)
    set_ini_section(ini, "/Script/ModelContextProtocolEngine.ModelContextProtocolSettings", MCP_SETTINGS)
    # The per-user copy wins over the defaults once the editor has written one, so keep it in step.
    saved = BENCH_ROOT / "Saved" / "Config" / "WindowsEditor" / "EditorPerProjectUserSettings.ini"
    if saved.exists():
        set_ini_section(saved, "/Script/HyperAIStudio.HyperAIStudioSettings", HYPERAI_SETTINGS)
        set_ini_section(saved, "/Script/ModelContextProtocolEngine.ModelContextProtocolSettings", MCP_SETTINGS)


def source_commit(source):
    result = subprocess.run(["git", "-C", str(source), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    dirty = subprocess.run(["git", "-C", str(source), "status", "--porcelain"], capture_output=True, text=True)
    return result.stdout.strip() + ("+dirty" if dirty.stdout.strip() else "")


def mirror_plugin(source=PLUGIN_ROOT):
    target = BENCH_ROOT / "Plugins" / PLUGIN_FOLDER
    log(f"Mirroring {source} into {target}.")
    # A copy rather than a junction: two projects building one plugin folder would overwrite each
    # other's binaries on every build. Files are compared by content and copied with a fresh timestamp:
    # a copy that kept its source's older time would look older than the objects UBT built from the
    # previous copy, so UBT would skip it (robocopy /MIR did exactly that).
    skip = {"Binaries", "Intermediate", ".git", "Saved", "runs", "__pycache__"}
    kept, copied, removed = set(), 0, 0
    for root, dirs, files in os.walk(source):
        dirs[:] = [d for d in dirs if d not in skip]
        for name in files:
            src = Path(root) / name
            dst = target / src.relative_to(source)
            kept.add(dst)
            if dst.is_file() and filecmp.cmp(src, dst, shallow=False):
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
            copied += 1
    for root, dirs, files in os.walk(target):
        dirs[:] = [d for d in dirs if d not in skip]
        for name in files:
            if Path(root) / name not in kept:
                (Path(root) / name).unlink()
                removed += 1
    log(f"Mirror: {copied} files copied, {removed} removed.")
    # Runs record which source was measured, since a bench can be built from any checkout.
    marker = BENCH_ROOT / "Saved" / "HyperAIBench"
    marker.mkdir(parents=True, exist_ok=True)
    (marker / "source.json").write_text(json.dumps({"path": str(source), "commit": source_commit(source)}), encoding="utf-8")


def build_editor():
    build = ENGINE_ROOT / "Engine" / "Build" / "BatchFiles" / "Build.bat"
    log("Building the bench editor (UnrealEditor target with the bench project's plugins).")
    # Below normal, so a build never slows an agent run on another bench (compilers inherit the class).
    result = subprocess.run(
        [str(build), "UnrealEditor", "Win64", "Development", f"-Project={uproject_path()}", "-WaitMutex",
         "-NoHotReloadFromIDE"],
        capture_output=True, text=True, creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
    tail = "\n".join((result.stdout + result.stderr).splitlines()[-40:])
    if result.returncode != 0:
        raise SystemExit(f"Build failed ({result.returncode}).\n{tail}")
    compiled = [line for line in result.stdout.splitlines() if "HyperAIStudio" in line and ("Compile" in line or "Link" in line)]
    log(f"Build succeeded; {len(compiled)} HyperAIStudio compile/link actions.")


def cmd_setup(args):
    if editor_running():
        raise SystemExit("The bench editor is running; stop it first (bench.py editor stop).")
    if not uproject_path().exists():
        create_project()
    ensure_plugins()
    ensure_settings()
    mirror_plugin(Path(args.source) if args.source else PLUGIN_ROOT)
    if not args.skip_build:
        build_editor()
    log("Setup done.")


# ------------------------------------------------------------------------------------------- MCP


class McpClient:
    """The minimum of streamable-HTTP MCP the harness needs: a session, tools/list, tools/call."""

    def __init__(self, url=MCP_URL, timeout=30):
        self.url = url
        self.timeout = timeout
        self.session = None
        self.next_id = 1

    def _post(self, payload, timeout=None):
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        request = urllib.request.Request(self.url, data=json.dumps(payload).encode("utf-8"), headers=headers, method="POST")
        with urllib.request.urlopen(request, timeout=timeout or self.timeout) as response:
            session = response.headers.get("Mcp-Session-Id")
            if session:
                self.session = session
            body = response.read().decode("utf-8")
        text = body.strip()
        if text.startswith(("event:", "data:")):
            text = "\n".join(line[5:].strip() for line in text.splitlines() if line.startswith("data:"))
        return json.loads(text) if text else None

    def request(self, method, params=None, timeout=None):
        payload = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}}
        self.next_id += 1
        reply = self._post(payload, timeout)
        if reply and "error" in reply:
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply["result"] if reply else None

    def open(self):
        self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                    "clientInfo": {"name": "HyperAIStudio.Bench", "version": "1.0.0"}})
        self._post({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return self

    def call(self, name, arguments=None, timeout=None):
        result = self.request("tools/call", {"name": name, "arguments": arguments or {}}, timeout)
        texts = [item.get("text", "") for item in result.get("content", []) if item.get("type") == "text"]
        return result, "\n".join(texts)

    def close(self):
        if not self.session:
            return
        try:
            request = urllib.request.Request(self.url, headers={"Mcp-Session-Id": self.session}, method="DELETE")
            urllib.request.urlopen(request, timeout=5).close()
        except (urllib.error.URLError, OSError):
            pass
        self.session = None


def mcp_ready(require_hyperai=True):
    """True once the editor answers MCP and, when asked, HyperAIStudio's toolsets are registered."""
    client = McpClient(timeout=10)
    try:
        client.open()
        tools = [tool["name"] for tool in client.request("tools/list").get("tools", [])]
        if not require_hyperai:
            return True, f"{len(tools)} top-level tools"
        if "list_toolsets" not in tools:
            return False, f"tool search is off ({len(tools)} tools); expected list_toolsets"
        _, text = client.call("list_toolsets", {}, timeout=30)
        hyper = sorted({token.strip('",') for token in text.replace("\n", " ").split() if "HyperAIStudio" in token})
        if not hyper:
            return False, "MCP is up but no HyperAIStudio toolsets are registered yet"
        return True, f"{len(hyper)} HyperAIStudio toolset references"
    except (urllib.error.URLError, OSError, RuntimeError, ValueError, KeyError) as error:
        return False, str(error)
    finally:
        client.close()


# ------------------------------------------------------------------------------------------ editor


def editor_exe():
    return ENGINE_ROOT / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"


def editor_pids():
    """PIDs of editors whose command line names the bench project; other editors are left alone."""
    query = ("Get-CimInstance Win32_Process -Filter \"Name='UnrealEditor.exe'\" | "
             "Where-Object { $_.CommandLine -like '*" + BENCH_NAME + ".uproject*' } | "
             "ForEach-Object { $_.ProcessId }")
    result = subprocess.run(["powershell", "-NoProfile", "-Command", query], capture_output=True, text=True)
    return [int(token) for token in result.stdout.split() if token.isdigit()]


def editor_running():
    return bool(editor_pids())


def clear_restore_prompt():
    # A killed editor leaves autosave restore data, and the next start would wait on a restore dialog.
    shutil.rmtree(BENCH_ROOT / "Saved" / "Autosaves", ignore_errors=True)


def start_editor(extra_args=(), timeout=300):
    if editor_running():
        log("Bench editor already running.")
    else:
        clear_restore_prompt()
        RUNS_DIR.mkdir(exist_ok=True)
        args = [str(editor_exe()), str(uproject_path()), "-nosplash", "-NoLiveCoding", *extra_args]
        log("Starting the bench editor.")
        subprocess.Popen(args, cwd=str(BENCH_ROOT),
                         creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + timeout
    message = ""
    while time.time() < deadline:
        ready, message = mcp_ready()
        if ready:
            log(f"MCP ready at {MCP_URL}: {message}.")
            return True
        if not editor_running() and time.time() > deadline - timeout + 20:
            raise SystemExit("The bench editor exited during startup; see its log in Saved/Logs.")
        time.sleep(3)
    raise SystemExit(f"Timed out waiting for MCP: {message}")


def stop_editor():
    pids = editor_pids()
    for pid in pids:
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
    for _ in range(30):
        if not editor_running():
            break
        time.sleep(1)
    clear_restore_prompt()
    if pids:
        log(f"Stopped bench editor ({', '.join(map(str, pids))}).")


def cmd_editor(args):
    if args.action == "start":
        start_editor(args.extra or ())
    elif args.action == "stop":
        stop_editor()
    else:
        running = editor_running()
        ready, message = mcp_ready() if running else (False, "editor not running")
        log(f"running={running} ready={ready} ({message})")


# ------------------------------------------------------------------------------------------ verify


def select_tasks(spec):
    if spec in (None, "", "all"):
        return list(TASKS)
    wanted = {part.strip() for part in spec.split(",")}
    chosen = [task for task in TASKS if task[0] in wanted or task[0].split("-")[0] in wanted]
    if not chosen:
        raise SystemExit(f"No task matches {spec!r}; tasks are {', '.join(t[0] for t in TASKS)}.")
    return chosen


def copy_evidence(task_id, out_dir):
    """The verifier's captures from the required views, kept beside the record."""
    for image in (BENCH_ROOT / "Saved" / "HyperAIBench" / "verify").glob(f"{task_id}_*.png"):
        shutil.copy2(image, out_dir / image.name)


def verify(tasks, log_path, timeout=900):
    """Runs the verification tests in a fresh editor, which only knows what was saved to disk."""
    if editor_running():
        raise SystemExit("Stop the bench editor before verifying: verification must load only saved state.")
    report_dir = BENCH_ROOT / "Saved" / "HyperAIBench" / "verify"
    shutil.rmtree(report_dir, ignore_errors=True)
    clear_restore_prompt()
    tests = "+".join(test for _, test, _ in tasks)
    command = [
        str(ENGINE_ROOT / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"), str(uproject_path()),
        f"-ExecCmds=Module Load HyperAIStudioBenchmark,Automation RunTests {tests}",
        "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-nosplash", "-nosound",
        f"-abslog={log_path}",
    ]
    try:
        subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        log("Verification timed out.")
    results = {}
    for task_id, test, _ in tasks:
        path = report_dir / f"{task_id}.json"
        if path.exists():
            results[task_id] = json.loads(path.read_text(encoding="utf-8"))
        else:
            results[task_id] = {"task": task_id, "pass": False, "checks": [],
                                "error": f"{test} produced no report; see {log_path}"}
    return results


def cmd_verify(args):
    RUNS_DIR.mkdir(exist_ok=True)
    tasks = select_tasks(args.task)
    results = verify(tasks, RUNS_DIR / "verify-adhoc.log")
    for task_id, result in results.items():
        failed = [c for c in result.get("checks", []) if c["required"] and not c["pass"]]
        log(f"{task_id}: {'PASS' if result.get('pass') else 'FAIL'}"
            + (f" - {result['error']}" if result.get("error") else "")
            + "".join(f"\n    x {c['id']}: {c['detail']}" for c in failed[:6]))


# ------------------------------------------------------------------------------------ accounting

TASK_SECONDS = 15 * 60
MAX_STATE_CHANGING = 45
# Epic's tool names carry their effect in the leading verb; anything not listed here is counted as a read.
MUTATING_VERBS = {
    "add", "apply", "append", "assign", "attach", "bake", "bind", "build", "clear", "compile", "connect",
    "create", "delete", "destroy", "detach", "disconnect", "duplicate", "execute", "fix", "generate", "import",
    "insert", "link", "load", "make", "move", "new", "paint", "place", "play", "refresh", "remove", "rename",
    "reparent", "repair", "replace", "reset", "run", "save", "set", "spawn", "start", "stop", "toggle",
    "unlink", "update", "wrap", "write",
}


def load_catalog_safety():
    catalog = json.loads((PLUGIN_ROOT / "Tools" / "Catalog" / "catalog_source.json").read_text(encoding="utf-8"))
    return {tool["name"]: tool.get("safety", []) for pack in catalog["packs"] for tool in pack.get("tools", [])}


def leading_verb(name):
    snake = "".join(f"_{c.lower()}" if c.isupper() else c for c in name).lstrip("_")
    return snake.split("_", 1)[0]


def find_dry_run(value):
    """True when any dry-run flag in the arguments is set, wherever the tool nests its request."""
    if isinstance(value, dict):
        for key, item in value.items():
            if key.lower().replace("_", "") in ("bdryrun", "dryrun") and item is True:
                return True
            if find_dry_run(item):
                return True
    elif isinstance(value, list):
        return any(find_dry_run(item) for item in value)
    return False


class Accounting:
    """Counts one agent's Unreal requests from its event stream, whichever agent it is."""

    def __init__(self, safety):
        self.safety = safety
        self.requests = 0
        self.state_changing = 0
        self.failed = 0
        self.inner = {}
        self.foreign = []
        self.local_tools = {}
        self.pending = {}

    def classify(self, tool, arguments):
        if tool != "call_tool":
            return tool, False
        arguments = arguments or {}
        inner = str(arguments.get("tool_name") or "?")
        inner_args = arguments.get("arguments") or {}
        if inner in self.safety:
            mutating = any(s != "Read" for s in self.safety[inner])
        else:
            mutating = leading_verb(inner) in MUTATING_VERBS
        return inner, mutating and not find_dry_run(inner_args)

    def begin(self, call_id, server, tool, arguments):
        if server != "unreal-mcp":
            self.foreign.append(f"{server}.{tool}")
            return
        inner, mutating = self.classify(tool, arguments)
        self.requests += 1
        self.state_changing += 1 if mutating else 0
        self.inner[inner] = self.inner.get(inner, 0) + 1
        self.pending[call_id] = inner

    def end(self, call_id, is_error, texts):
        if call_id not in self.pending:
            return
        self.pending.pop(call_id)
        failed = bool(is_error)
        for text in texts:
            try:
                parsed = json.loads(text)
            except (TypeError, ValueError):
                continue
            if isinstance(parsed, dict) and any(parsed.get(key) is False for key in ("bOk", "ok", "success")):
                failed = True
        self.failed += 1 if failed else 0

    def local(self, name):
        self.local_tools[name] = self.local_tools.get(name, 0) + 1

    def summary(self):
        return {"requests": self.requests, "state_changing": self.state_changing, "failed": self.failed,
                "by_tool": dict(sorted(self.inner.items(), key=lambda kv: -kv[1])),
                "foreign_tool_calls": self.foreign, "local_tools": self.local_tools}


def result_texts(content):
    if isinstance(content, str):
        return [content]
    if isinstance(content, list):
        return [item.get("text", "") for item in content if isinstance(item, dict) and item.get("type") == "text"]
    return []


def parse_claude(event, acct, state):
    kind = event.get("type")
    if kind == "system" and event.get("subtype") == "init":
        state["model"] = event.get("model")
        state["mcp"] = event.get("mcp_servers")
    elif kind == "assistant":
        for block in event.get("message", {}).get("content", []):
            if block.get("type") == "tool_use":
                name = block.get("name", "")
                if name.startswith("mcp__"):
                    _, server, tool = name.split("__", 2)
                    acct.begin(block.get("id"), server, tool, block.get("input"))
                else:
                    acct.local(name)
            elif block.get("type") == "text":
                state["last_text"] = block.get("text", "")
    elif kind == "user":
        content = event.get("message", {}).get("content", [])
        for block in content if isinstance(content, list) else []:
            if block.get("type") == "tool_result":
                acct.end(block.get("tool_use_id"), block.get("is_error"), result_texts(block.get("content")))
    elif kind == "result":
        state["final"] = event.get("result") or state.get("last_text", "")
        state["turns"] = event.get("num_turns")
        state["cost_usd"] = event.get("total_cost_usd")
        state["agent_status"] = event.get("subtype")
        state["is_error"] = bool(event.get("is_error"))


def parse_codex(event, acct, state):
    kind = event.get("type")
    item = event.get("item") or {}
    if kind == "item.started" and item.get("type") == "mcp_tool_call":
        acct.begin(item.get("id"), item.get("server"), item.get("tool"), item.get("arguments"))
    elif kind == "item.completed":
        if item.get("type") == "mcp_tool_call":
            result = item.get("result") or {}
            acct.end(item.get("id"), bool(item.get("error")) or bool(result.get("isError")), result_texts(result.get("content")))
        elif item.get("type") == "agent_message":
            state["final"] = item.get("text", "")
        elif item.get("type") in ("command_execution", "file_change", "web_search"):
            acct.local(item.get("type"))
    elif kind == "turn.completed":
        state["usage"] = event.get("usage")
    elif kind in ("turn.failed", "error"):
        state["agent_status"] = kind
        state["error"] = json.dumps(event)[:500]


# ------------------------------------------------------------------------------------------ agents


def codex_command(prompt, model, effort):
    command = ["codex", "exec", "--json", "--skip-git-repo-check", "-C", str(BENCH_ROOT), "-s", "read-only",
               "-c", 'approval_policy="never"', "-c", 'web_search="disabled"',
               "--disable", "computer_use", "--disable", "browser_use", "--disable", "in_app_browser",
               "-c", f'mcp_servers.unreal-mcp.url="{MCP_URL}"', "-c", "mcp_servers.unreal-mcp.tool_timeout_sec=300"]
    for tool in ("list_toolsets", "describe_toolset", "call_tool"):
        command += ["-c", f'mcp_servers.unreal-mcp.tools.{tool}.approval_mode="approve"']
    # The user's own servers would change what the agent can reach; only the bench editor is allowed.
    try:
        import tomllib
        config = tomllib.loads((Path.home() / ".codex" / "config.toml").read_text(encoding="utf-8"))
        for name in config.get("mcp_servers", {}):
            if name != "unreal-mcp":
                command += ["-c", f"mcp_servers.{name}.enabled=false"]
    except (OSError, ValueError):
        pass
    if model:
        command += ["-m", model]
    if effort:
        command += ["-c", f'model_reasoning_effort="{effort}"']
    return command + [prompt]


def claude_executable():
    """The native claude.exe behind npm's shim. Going through claude.CMD hands arguments to cmd.exe,
    which cuts a multi-line argument at its first newline and silently drops every flag after it."""
    shim = shutil.which("claude")
    if shim and shim.lower().endswith(".cmd"):
        native = Path(shim).parent / "node_modules" / "@anthropic-ai" / "claude-code" / "bin" / "claude.exe"
        if native.exists():
            return str(native)
    return shim or "claude"


def claude_command(prompt, model, effort):
    # The prompt goes on stdin, never argv, so no shell ever gets to parse it.
    command = [claude_executable(), "-p", "--output-format", "stream-json", "--verbose",
               "--strict-mcp-config", "--mcp-config", str(BENCH_ROOT / ".mcp.json"),
               "--allowedTools", "mcp__unreal-mcp__list_toolsets,mcp__unreal-mcp__describe_toolset,mcp__unreal-mcp__call_tool,Read,Glob,Grep",
               "--permission-prompts", "none"]
    if model:
        command += ["--model", model]
    if effort:
        command += ["--effort", effort]
    return command


def kill_tree(process):
    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True)


def run_agent(agent, prompt, out_dir, args):
    """Runs one agent on one task, counting as it goes, and stops it at either limit."""
    import queue
    import threading

    safety = load_catalog_safety()
    acct = Accounting(safety)
    state = {"agent": agent}
    if agent == "codex":
        command = codex_command(prompt, args.codex_model, args.codex_effort)
        parse = parse_codex
    else:
        command = claude_command(prompt, args.claude_model, args.claude_effort)
        parse = parse_claude
    executable = shutil.which(command[0]) or command[0]
    transcript = open(out_dir / "transcript.jsonl", "w", encoding="utf-8")
    errors = open(out_dir / "stderr.txt", "w", encoding="utf-8")
    (out_dir / "command.txt").write_text(json.dumps([executable] + command[1:], indent=1), encoding="utf-8")
    started = time.time()
    process = subprocess.Popen([executable] + command[1:], cwd=str(BENCH_ROOT),
                               stdin=subprocess.PIPE if agent == "claude" else subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=errors, text=True, encoding="utf-8", errors="replace",
                               creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    if agent == "claude":
        process.stdin.write(prompt)
        process.stdin.close()
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in process.stdout] + [lines.put(None)], daemon=True).start()
    stop_reason = "completed"
    while True:
        if time.time() - started > TASK_SECONDS:
            stop_reason = "time_limit"
            kill_tree(process)
            break
        try:
            line = lines.get(timeout=1.0)
        except queue.Empty:
            continue
        if line is None:
            break
        transcript.write(line)
        transcript.flush()
        try:
            parse(json.loads(line), acct, state)
        except ValueError:
            continue
        if acct.state_changing > MAX_STATE_CHANGING:
            stop_reason = "request_cap"
            kill_tree(process)
            break
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        kill_tree(process)
    transcript.close()
    errors.close()
    state.update({"stop_reason": stop_reason, "exit_code": process.returncode, "seconds": round(time.time() - started, 1)})
    state["accounting"] = acct.summary()
    return state


# --------------------------------------------------------------------------------------------- run


def wipe_bench_content():
    content = BENCH_ROOT / "Content"
    for folder in (content / "Bench", content / "__ExternalActors__" / "Bench", content / "__ExternalObjects__" / "Bench"):
        shutil.rmtree(folder, ignore_errors=True)


def archive_bench_content(out_dir):
    content = BENCH_ROOT / "Content"
    for relative in ("Bench", "__ExternalActors__/Bench", "__ExternalObjects__/Bench"):
        source = content / relative
        if source.is_dir():
            shutil.copytree(source, out_dir / "content" / relative, dirs_exist_ok=True)


def restore_bench_content(out_dir):
    wipe_bench_content()
    archived = out_dir / "content"
    if archived.is_dir():
        shutil.copytree(archived, BENCH_ROOT / "Content", dirs_exist_ok=True)


def editor_log_tool_lines(offset):
    path = BENCH_ROOT / "Saved" / "Logs" / f"{BENCH_NAME}.log"
    try:
        with open(path, "rb") as handle:
            handle.seek(offset)
            text = handle.read().decode("utf-8", errors="replace")
    except OSError:
        return 0
    return sum(1 for line in text.splitlines() if "Running tool:" in line)


def editor_log_size():
    try:
        return (BENCH_ROOT / "Saved" / "Logs" / f"{BENCH_NAME}.log").stat().st_size
    except OSError:
        return 0


def git_commit():
    result = subprocess.run(["git", "-C", str(PLUGIN_ROOT), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    dirty = subprocess.run(["git", "-C", str(PLUGIN_ROOT), "status", "--porcelain"], capture_output=True, text=True)
    return result.stdout.strip() + ("+dirty" if dirty.stdout.strip() else "")


# Both agents get the same framing. Without it Codex reads its read-only sandbox as "I cannot create
# assets" and stops before calling a single editing tool.
PROMPT_PREAMBLE = (
    "You are working in a running Unreal Editor through the unreal-mcp tools. Your own file access is "
    "read-only by design: the MCP tools create, edit and save assets inside the editor, so make every "
    "change through them."
)


# A plan's usage limit can end a run early through no fault of the tools. Such a run is set aside, unverified, and
# run again once the limit lifts, so the scorecard only counts runs that had their full chance.
USAGE_LIMIT_PATTERN = re.compile(r"usage limit|hit your (?:\w+ )?limit|limit reached|out of extra usage", re.IGNORECASE)
USAGE_LIMIT_MAX_WAIT = 6 * 3600


def usage_limit_message(agent, state, out_dir):
    """The plan's usage-limit message when that is what ended the run, else ''."""
    if agent == "codex":
        text = state.get("error", "") if state.get("agent_status") in ("turn.failed", "error") else ""
    else:
        # Claude prints the limit as its whole result: an error, or a line far shorter than any real summary.
        final = state.get("final") or ""
        text = final if state.get("is_error") or len(final) < 200 else ""
    try:
        text += "\n" + (out_dir / "stderr.txt").read_text(encoding="utf-8", errors="replace")[-2000:]
    except OSError:
        pass
    return text.strip()[:500] if USAGE_LIMIT_PATTERN.search(text) else ""


def usage_reset_time(message, now=None):
    """When the limit lifts, read from its message (a Unix time, or a clock time such as "3:27 PM"), plus two
    minutes; half an hour from now when the message gives neither."""
    now = time.time() if now is None else now
    stamp = re.search(r"\|(\d{10})\b", message)
    if stamp:
        return int(stamp.group(1)) + 120
    clock = re.search(r"(?:try again at|resets?(?: at)?)\s+(\d{1,2})(?::(\d{2}))?\s*([ap])\.?m\b", message, re.IGNORECASE)
    if not clock:
        return now + 1800
    hour = int(clock.group(1)) % 12 + (12 if clock.group(3).lower() == "p" else 0)
    local = time.localtime(now)
    target = time.mktime((local.tm_year, local.tm_mon, local.tm_mday, hour, int(clock.group(2) or 0), 0, 0, 0, -1))
    return (target if target > now else target + 86400) + 120


def run_one(agent, task, out_dir, args):
    """One run; when a usage limit cut it short, the attempt is kept beside it and the run starts over later."""
    waited = 0.0
    attempt = 1
    while True:
        record = run_attempt(agent, task, out_dir, args)
        if record["stop_reason"] != "usage_limit":
            return record
        resume = usage_reset_time(record["usage_limit"])
        if waited + max(0.0, resume - time.time()) > USAGE_LIMIT_MAX_WAIT:
            log(f"{agent} / {task[0]}: the usage limit lasts past the {USAGE_LIMIT_MAX_WAIT // 3600} h wait cap; recorded as stopped by it.")
            return record
        shelved = out_dir.with_name(f"{out_dir.name}-usage-limit-{attempt}")
        shutil.rmtree(shelved, ignore_errors=True)
        out_dir.rename(shelved)
        log(f"{agent} / {task[0]}: waiting for the {agent} usage limit to lift; retrying at "
            f"{time.strftime('%H:%M', time.localtime(resume))}.")
        pause = max(0.0, resume - time.time())
        time.sleep(pause)
        waited += pause
        attempt += 1


def run_attempt(agent, task, out_dir, args):
    task_id, test, _ = task
    out_dir.mkdir(parents=True, exist_ok=True)
    prompt = PROMPT_PREAMBLE + "\n\n" + (TASKS_DIR / f"{task_id}.md").read_text(encoding="utf-8").strip()
    stop_editor()
    wipe_bench_content()
    start_editor(timeout=900)
    offset = editor_log_size()
    log(f"{agent} / {task_id}: running (limit {TASK_SECONDS // 60} min, {MAX_STATE_CHANGING} state-changing requests).")
    record = run_agent(agent, prompt, out_dir, args)
    record["editor_log_tool_calls"] = editor_log_tool_lines(offset)
    limit = usage_limit_message(agent, record, out_dir)
    if limit:
        stop_editor()
        record.update({"task": task_id, "pass": False, "stop_reason": "usage_limit", "usage_limit": limit,
                       "verification": {"error": "stopped by the agent's plan usage limit"}, "out_dir": str(out_dir)})
        (out_dir / "record.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
        log(f"{agent} / {task_id}: stopped by a usage limit after {record['seconds']:.0f}s: {limit[:200]}")
        return record
    # Work the agent submitted just before exiting still gets its editor ticks to land and save.
    time.sleep(10)
    stop_editor()
    archive_bench_content(out_dir)
    verification = verify([task], out_dir / "verify.log")[task_id]
    copy_evidence(task_id, out_dir)
    record.update({"task": task_id, "pass": bool(verification.get("pass")), "verification": verification, "out_dir": str(out_dir)})
    (out_dir / "record.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    acct = record["accounting"]
    log(f"{agent} / {task_id}: {'PASS' if record['pass'] else 'FAIL'} in {record['seconds']:.0f}s, "
        f"{acct['requests']} requests ({acct['state_changing']} state-changing, {acct['failed']} failed), stop={record['stop_reason']}")
    return record


def cmd_run(args):
    agents = ["codex", "claude"] if args.agent == "both" else [args.agent]
    tasks = select_tasks(args.task)
    run_id = time.strftime("%Y%m%d-%H%M%S") + (f"-{args.label}" if args.label else "")
    run_dir = RUNS_DIR / run_id
    run_dir.mkdir(parents=True, exist_ok=True)
    try:
        measured = json.loads((BENCH_ROOT / "Saved" / "HyperAIBench" / "source.json").read_text(encoding="utf-8"))["commit"]
    except (OSError, ValueError, KeyError):
        measured = git_commit()
    meta = {"run_id": run_id, "plugin_commit": measured, "started": time.strftime("%Y-%m-%d %H:%M:%S"),
            "agents": agents, "tasks": [t[0] for t in tasks], "repeats": args.repeats,
            "codex_model": args.codex_model, "codex_effort": args.codex_effort,
            "claude_model": args.claude_model, "claude_effort": args.claude_effort,
            "limits": {"seconds": TASK_SECONDS, "state_changing": MAX_STATE_CHANGING}}
    records = []
    for repeat in range(args.repeats):
        for agent in agents:
            for task in tasks:
                suffix = f"-r{repeat + 1}" if args.repeats > 1 else ""
                records.append(run_one(agent, task, run_dir / agent / f"{task[0]}{suffix}", args))
                (run_dir / "results.json").write_text(json.dumps({"meta": meta, "records": records}, indent=2), encoding="utf-8")
    write_report(run_dir)
    log(f"Run {run_id} done: {run_dir}")


def cmd_reverify(args):
    """Re-scores archived runs against the current verifier, which grows stricter as proof tools land."""
    run_dir = RUNS_DIR / args.run_id
    results = json.loads((run_dir / "results.json").read_text(encoding="utf-8"))
    stop_editor()
    for record in results["records"]:
        task = next(t for t in TASKS if t[0] == record["task"])
        out_dir = Path(record["out_dir"])
        restore_bench_content(out_dir)
        verification = verify([task], out_dir / "reverify.log")[record["task"]]
        copy_evidence(record["task"], out_dir)
        record["verification"] = verification
        record["pass"] = bool(verification.get("pass"))
        log(f"{record['agent']} / {record['task']}: {'PASS' if record['pass'] else 'FAIL'}")
    wipe_bench_content()
    (run_dir / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    write_report(run_dir)


# ------------------------------------------------------------------------------------------ report


def write_report(run_dir):
    """A scorecard in the benchmark's own shape: tasks passed, requests, median per task, time, failures."""
    import html
    import statistics

    results = json.loads((run_dir / "results.json").read_text(encoding="utf-8"))
    meta, records = results["meta"], results["records"]
    agents = sorted({r["agent"] for r in records})
    summary = {}
    rows = []
    for agent in agents:
        mine = [r for r in records if r["agent"] == agent]
        requests = [r["accounting"]["requests"] for r in mine]
        summary[agent] = {
            "passed": sum(r["pass"] for r in mine), "tasks": len(mine),
            "requests": sum(requests), "median_requests": statistics.median(requests) if requests else 0,
            "failed_requests": sum(r["accounting"]["failed"] for r in mine),
            "minutes": round(sum(r["seconds"] for r in mine) / 60, 1),
        }
        s = summary[agent]
        rows.append(f"<tr><td>{html.escape(agent)}</td><td>{s['passed']} / {s['tasks']}</td><td>{s['requests']}</td>"
                    f"<td>{s['median_requests']}</td><td>{s['failed_requests']}</td><td>{s['minutes']} min</td></tr>")
    details = []
    for r in records:
        acct = r["accounting"]
        checks = r["verification"].get("checks", [])
        failing = [c for c in checks if c["required"] and not c["pass"]]
        why = "; ".join(f"{c['id']}: {c['detail']}" for c in failing[:4]) or r["verification"].get("error", "")
        details.append(
            f"<tr class='{'pass' if r['pass'] else 'fail'}'><td>{html.escape(r['agent'])}</td><td>{html.escape(r['task'])}</td>"
            f"<td>{'Pass' if r['pass'] else 'Fail'}</td><td>{acct['requests']}</td><td>{acct['state_changing']}</td>"
            f"<td>{acct['failed']}</td><td>{r['seconds'] / 60:.1f} min</td><td>{html.escape(r['stop_reason'])}</td>"
            f"<td>{html.escape(why)}</td></tr>")
    page = f"""<!doctype html><html><head><meta charset="utf-8"><title>HyperAIStudio benchmark {html.escape(meta['run_id'])}</title>
<style>body{{font:14px system-ui,sans-serif;margin:24px;max-width:1200px}}table{{border-collapse:collapse;width:100%;margin:12px 0}}
td,th{{border-bottom:1px solid #ddd;padding:6px 8px;text-align:left;vertical-align:top}}tr.pass td:nth-child(3){{color:#137333;font-weight:600}}
tr.fail td:nth-child(3){{color:#b3261e;font-weight:600}}code{{background:#f3f3f3;padding:1px 4px}}</style></head><body>
<h1>HyperAIStudio benchmark</h1>
<p>Run <code>{html.escape(meta['run_id'])}</code>, plugin <code>{html.escape(meta['plugin_commit'])}</code>, started {html.escape(meta['started'])}.
Limits: {meta['limits']['seconds'] // 60} minutes and {meta['limits']['state_changing']} state-changing requests per task.
Models: Codex <code>{html.escape(str(meta['codex_model']))}</code> ({html.escape(str(meta['codex_effort']))}),
Claude <code>{html.escape(str(meta['claude_model']))}</code> ({html.escape(str(meta['claude_effort']))}).</p>
<table><tr><th>Agent</th><th>Tasks passed</th><th>Total requests</th><th>Median / task</th><th>Failed requests</th><th>Total time</th></tr>
{''.join(rows)}</table>
<h2>Per task</h2>
<table><tr><th>Agent</th><th>Task</th><th>Result</th><th>Requests</th><th>State-changing</th><th>Failed</th><th>Time</th><th>Stopped</th><th>Why</th></tr>
{''.join(details)}</table>
<p>NeoStack's published pilot, for reference: NeoStack 3/5 (145 requests, median 27), Epic MCP 1/5 (375, 66), Nwiro 0/5 (320, 64).
Their tasks and verifier are not public; these tasks follow their descriptions, and this verifier checks saved state in a fresh editor.</p>
</body></html>"""
    (run_dir / "scorecard.html").write_text(page, encoding="utf-8")
    (run_dir / "summary.json").write_text(json.dumps({"meta": meta, "summary": summary}, indent=2), encoding="utf-8")
    return summary


def cmd_report(args):
    run_dir = RUNS_DIR / args.run_id if args.run_id else max((p for p in RUNS_DIR.iterdir() if (p / "results.json").exists()), key=lambda p: p.stat().st_mtime)
    summary = write_report(run_dir)
    for agent, s in summary.items():
        log(f"{agent}: {s['passed']}/{s['tasks']} passed, {s['requests']} requests (median {s['median_requests']}), "
            f"{s['failed_requests']} failed, {s['minutes']} min")
    log(f"Scorecard: {run_dir / 'scorecard.html'}")


# -------------------------------------------------------------------------------------------- main


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    setup = sub.add_parser("setup", help="create or refresh the bench project, mirror the plugin, build")
    setup.add_argument("--skip-build", action="store_true")
    setup.add_argument("--source", help="plugin checkout to mirror (default: this one)")
    setup.set_defaults(func=cmd_setup)

    editor = sub.add_parser("editor", help="start, stop, or check the bench editor")
    editor.add_argument("action", choices=("start", "stop", "status"))
    editor.add_argument("extra", nargs="*", help="extra editor arguments for start")
    editor.set_defaults(func=cmd_editor)

    check = sub.add_parser("verify", help="verify the bench project's current saved state")
    check.add_argument("--task", default="all", help="all, or a comma list such as 01,03")
    check.set_defaults(func=cmd_verify)

    run = sub.add_parser("run", help="run agents on tasks, verify, and write a scorecard")
    run.add_argument("--agent", choices=("codex", "claude", "both"), default="both")
    run.add_argument("--task", default="all", help="all, or a comma list such as 01,03")
    run.add_argument("--repeats", type=int, default=1)
    run.add_argument("--label", default="")
    run.add_argument("--codex-model", default="gpt-6-sol")
    run.add_argument("--codex-effort", default="medium")
    run.add_argument("--claude-model", default="claude-opus-5-5")
    run.add_argument("--claude-effort", default="xhigh")
    run.set_defaults(func=cmd_run)

    reverify = sub.add_parser("reverify", help="re-score an archived run with the current verifier")
    reverify.add_argument("run_id")
    reverify.set_defaults(func=cmd_reverify)

    report = sub.add_parser("report", help="rebuild a run's scorecard (latest run by default)")
    report.add_argument("run_id", nargs="?")
    report.set_defaults(func=cmd_report)

    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
