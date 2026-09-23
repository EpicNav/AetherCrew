"""HyperAIStudio script runner, written out by the HyperAIStudioScripting module and run in ExecuteFile mode.

    runner.py analyze <script.py> <out.json>
    runner.py run <script.py> <timeout_seconds> <out.json> <guarded 0|1>

analyze reports why a script cannot run as one undoable step. run executes it with a deadline checked between
Python lines and, when guarded, an audit hook that refuses file writes, processes and sockets. Both are
best-effort guards for an agent's own work, not a sandbox.
"""
import ast
import contextlib
import io
import json
import sys
import time
import traceback
import types

# Imports that reach the file system, processes, the network or the interpreter itself.
ESCALATING_MODULES = {
    "os", "sys", "subprocess", "socket", "shutil", "ctypes", "importlib", "http", "urllib", "ftplib", "smtplib",
    "multiprocessing", "threading", "concurrent", "asyncio", "signal", "pathlib", "tempfile", "glob", "io",
    "builtins", "gc", "inspect", "pickle", "marshal", "shelve", "code", "codeop", "runpy", "zipfile", "tarfile",
    "webbrowser", "winreg", "_winapi", "msvcrt", "nt", "posix", "pty", "requests", "sqlite3", "mmap",
}

# Calls an editor undo cannot take back, or that end or replace the transaction the run lives in.
ESCALATING_CALLS = {
    "delete_asset": "deletes an asset",
    "delete_loaded_asset": "deletes an asset",
    "delete_loaded_assets": "deletes assets",
    "delete_directory": "deletes a folder",
    "rename_asset": "renames an asset",
    "rename_loaded_asset": "renames an asset",
    "rename_directory": "renames a folder",
    "consolidate_assets": "consolidates assets",
    "delete_actor": "deletes an actor",
    "destroy_actor": "destroys an actor",
    "destroy_actors": "destroys actors",
    "destroy_component": "destroys a component",
    "import_asset_tasks": "imports files from disk",
    "import_assets_automated": "imports files from disk",
    "execute_console_command": "runs a console command",
    "execute_python_command": "runs more Python",
    "exec": "runs generated code",
    "eval": "runs generated code",
    "compile": "compiles generated code",
    "__import__": "imports by name",
    "load_level": "loads a level",
    "load_map": "loads a level",
    "load_level_instance": "loads a level",
    "new_level": "replaces the open level",
    "new_map": "replaces the open level",
    "begin_transaction": "manages undo itself",
    "end_transaction": "manages undo itself",
    "cancel_transaction": "manages undo itself",
    "undo": "undoes editor history",
    "undo_transaction": "undoes editor history",
    "redo": "redoes editor history",
    "redo_transaction": "redoes editor history",
    "request_exit": "closes the editor",
    "quit_editor": "closes the editor",
}

# Attribute names used to climb out of plain code into the interpreter's internals.
ESCAPE_ATTRIBUTES = {
    "__globals__", "__subclasses__", "__builtins__", "__code__", "__closure__", "__import__", "__loader__",
    "__spec__", "__mro__", "__bases__", "f_globals", "f_locals", "f_back", "f_builtins", "tb_frame",
    "gi_frame", "cr_frame", "ag_frame",
}

ESCAPE_NAMES = {"__builtins__", "globals", "locals", "vars", "breakpoint"}

# Audit events an undoable run may not raise; file opens are checked by mode separately.
BLOCKED_EVENT_PREFIXES = ("subprocess.", "socket.", "ctypes.", "shutil.", "webbrowser.", "winreg.", "_winapi.")
BLOCKED_EVENTS = {
    "os.system", "os.remove", "os.unlink", "os.rename", "os.replace", "os.rmdir", "os.mkdir", "os.chmod",
    "os.kill", "os.startfile", "os.exec", "os.spawn", "os.posix_spawn", "os.truncate", "os.symlink", "os.link",
    "os.putenv", "os.unsetenv",
}


class ScriptTimeout(BaseException):
    """Not an Exception, so a script's own except Exception cannot swallow its deadline."""


def _call_name(node):
    if isinstance(node.func, ast.Attribute):
        return node.func.attr
    if isinstance(node.func, ast.Name):
        return node.func.id
    return None


def _opens_for_writing(node):
    mode = node.args[1] if len(node.args) > 1 else next((k.value for k in node.keywords if k.arg == "mode"), None)
    if mode is None:
        return False
    if isinstance(mode, ast.Constant) and isinstance(mode.value, str):
        return any(flag in mode.value for flag in "wax+")
    return True


def analyze(code):
    """Returns (defines run(), reasons it needs approval). Raises SyntaxError."""
    tree = ast.parse(code)
    reasons = []

    def add(reason):
        if reason not in reasons:
            reasons.append(reason)

    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                top = alias.name.split(".")[0]
                if top in ESCALATING_MODULES:
                    add(f"imports {top}")
        elif isinstance(node, ast.ImportFrom):
            if node.level:
                add("uses a relative import")
            top = (node.module or "").split(".")[0]
            if top in ESCALATING_MODULES:
                add(f"imports from {top}")
        elif isinstance(node, ast.Call):
            name = _call_name(node)
            if name in ESCALATING_CALLS:
                add(f"calls {name}: {ESCALATING_CALLS[name]}")
            if name == "open" and _opens_for_writing(node):
                add("opens a file for writing")
            if name in ("getattr", "setattr", "delattr") and len(node.args) > 1:
                attribute = node.args[1]
                if not (isinstance(attribute, ast.Constant) and isinstance(attribute.value, str)):
                    add(f"{name} with a computed name")
                elif attribute.value in ESCALATING_CALLS or attribute.value in ESCAPE_ATTRIBUTES:
                    add(f"{name} of {attribute.value}")
        elif isinstance(node, ast.Attribute) and node.attr in ESCAPE_ATTRIBUTES:
            add(f"reaches {node.attr}")
        elif isinstance(node, ast.Name) and node.id in ESCAPE_NAMES:
            add(f"uses {node.id}")
    has_run = any(isinstance(item, ast.FunctionDef) and item.name == "run" for item in tree.body)
    return has_run, reasons


def _guard():
    """One audit hook for the editor's lifetime; hooks cannot be removed, so it acts only while active."""
    guard = sys.modules.get("hyperai_script_guard")
    if guard is not None:
        return guard
    guard = types.ModuleType("hyperai_script_guard")
    guard.active = False

    def hook(event, args):
        if not guard.active:
            return
        if event == "open":
            mode = args[1] if len(args) > 1 else None
            flags = args[2] if len(args) > 2 else 0
            writes = (isinstance(mode, str) and any(flag in mode for flag in "wax+")) or (
                mode is None and isinstance(flags, int) and flags & 0o3 != 0)
            if writes:
                raise PermissionError(f"Writing {args[0]!r} needs the user's approval; this script ran as undoable.")
        elif event in BLOCKED_EVENTS or event.startswith(BLOCKED_EVENT_PREFIXES):
            raise PermissionError(f"{event} needs the user's approval; this script ran as undoable.")

    sys.addaudithook(hook)
    sys.modules["hyperai_script_guard"] = guard
    return guard


def run(script_path, timeout, out_path, guarded):
    with open(script_path, "r", encoding="utf-8") as handle:
        code = handle.read()
    record = {"ok": False, "result_json": "", "output": "", "traceback": "", "timed_out": False, "seconds": 0.0}
    output = io.StringIO()
    namespace = {"__name__": "hyperai_script", "__file__": script_path}
    guard = _guard()
    started = time.monotonic()
    deadline = started + timeout

    def tracer(frame, event, arg):
        if time.monotonic() > deadline:
            raise ScriptTimeout(f"The script ran past {timeout:g} s.")
        return tracer

    guard.active = guarded
    try:
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            sys.settrace(tracer)
            try:
                exec(compile(code, script_path, "exec"), namespace)
                entry = namespace.get("run")
                if not callable(entry):
                    raise ValueError("Define run() and return a JSON-serializable dict.")
                value = entry()
            finally:
                sys.settrace(None)
        record["result_json"] = json.dumps(value, default=str)
        record["ok"] = True
    except ScriptTimeout:
        record["timed_out"] = True
        record["traceback"] = traceback.format_exc(limit=6)
    except BaseException:  # the agent's script may raise anything, SystemExit included
        record["traceback"] = traceback.format_exc(limit=8)
    finally:
        guard.active = False
        record["output"] = output.getvalue()[-20000:]
        record["seconds"] = round(time.monotonic() - started, 3)
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(record, handle)


def main(argv):
    if len(argv) >= 4 and argv[1] == "analyze":
        with open(argv[2], "r", encoding="utf-8") as handle:
            code = handle.read()
        try:
            has_run, reasons = analyze(code)
            record = {"ok": True, "has_run": has_run, "reasons": reasons}
        except SyntaxError as error:
            record = {"ok": False, "syntax_error": f"line {error.lineno}: {error.msg}"}
        with open(argv[3], "w", encoding="utf-8") as handle:
            json.dump(record, handle)
    elif len(argv) >= 6 and argv[1] == "run":
        run(argv[2], float(argv[3]), argv[4], argv[5] == "1")


main(sys.argv)
