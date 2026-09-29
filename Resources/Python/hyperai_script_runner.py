"""HyperAIStudio script runner, written out by the HyperAIStudioScripting module and run in ExecuteFile mode.

    runner.py analyze <script.py> <out.json> [levels]
    runner.py run <script.py> <timeout_seconds> <out.json> <guarded 0|1>

analyze reports why a script cannot run as one undoable step, and which level changes it makes. run executes it
with a deadline checked between Python lines and, when guarded, an audit hook that refuses processes, sockets and
file writes outside the script's own folder and Saved/HyperAIStudio/Captures, and attribute lookups by a computed
name that reach what the check would have escalated. Both are best-effort guards for an agent's own work, not a
sandbox.
"""
import ast
import builtins
import contextlib
import difflib
import io
import json
import os
import re
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
    "load_level_instance": "loads a level",
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

# Calls that replace the open world. The editor refuses to overwrite a level, so while the open one has nothing unsaved
# a change loses nothing and needs no approval; the run's undo step just ends where the level changes.
LEVEL_CALLS = {"new_level", "new_map", "load_level", "load_map"}

# Console commands that only take a screenshot, allowed when a literal command starts with one of them.
SAFE_CONSOLE_COMMANDS = {"shot", "highresshot", "screenshot"}

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


def _literal(node, index, keyword):
    """A call's argument when it is a literal string, else None."""
    value = node.args[index] if len(node.args) > index else next((k.value for k in node.keywords if k.arg == keyword), None)
    return value.value if isinstance(value, ast.Constant) and isinstance(value.value, str) else None


def _is_safe_console_command(command):
    """A literal screenshot command with nothing chained after it."""
    words = (command or "").split()
    return bool(words) and words[0].lower() in SAFE_CONSOLE_COMMANDS and not any(mark in command for mark in "|;,\r\n")


def _write_roots(script_path):
    """Where an undoable script may write: its own folder, and Captures beside the Scripts folder holding it."""
    folder = os.path.dirname(os.path.abspath(script_path))
    scripts = folder
    while os.path.basename(scripts).lower() != "scripts" and os.path.dirname(scripts) != scripts:
        scripts = os.path.dirname(scripts)
    return [os.path.normcase(folder), os.path.normcase(os.path.join(os.path.dirname(scripts), "Captures"))]


def _is_under(path, roots):
    try:
        full = os.path.normcase(os.path.abspath(os.fsdecode(path)))
    except (TypeError, ValueError):
        return False
    return any(full == root or full.startswith(root + os.sep) for root in roots)


def analyze(code, write_roots):
    """Returns (defines run(), reasons it needs approval, level-changing calls). Raises SyntaxError."""
    tree = ast.parse(code)
    reasons = []
    level_changes = []

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
            if name in LEVEL_CALLS:
                if name not in level_changes:
                    level_changes.append(name)
            elif name in ESCALATING_CALLS and not (
                    name == "execute_console_command" and _is_safe_console_command(_literal(node, 1, "command"))):
                add(f"calls {name}: {ESCALATING_CALLS[name]}")
            if name == "open" and _opens_for_writing(node):
                # A literal path elsewhere asks first; a computed one is checked when the script runs.
                path = _literal(node, 0, "file")
                if path is not None and not _is_under(path, write_roots):
                    add("opens a file for writing")
            if name in ("getattr", "setattr", "delattr") and len(node.args) > 1:
                # A computed name is checked when the script runs.
                attribute = _literal(node, 1, None)
                if attribute in ESCALATING_CALLS or attribute in LEVEL_CALLS or attribute in ESCAPE_ATTRIBUTES:
                    add(f"{name} of {attribute}")
        elif isinstance(node, ast.Attribute) and node.attr in ESCAPE_ATTRIBUTES:
            add(f"reaches {node.attr}")
        elif isinstance(node, ast.Attribute) and node.attr in LEVEL_CALLS:
            # Also an alias such as f = subsystem.new_level, which the call check alone would miss.
            if node.attr not in level_changes:
                level_changes.append(node.attr)
        elif isinstance(node, ast.Name) and node.id in ESCAPE_NAMES:
            add(f"uses {node.id}")
    has_run = any(isinstance(item, ast.FunctionDef) and item.name == "run" for item in tree.body)
    return has_run, reasons, level_changes


def _guard():
    """One audit hook for the editor's lifetime; hooks cannot be removed, so it acts only while active."""
    guard = sys.modules.get("hyperai_script_guard")
    if guard is not None:
        return guard
    guard = types.ModuleType("hyperai_script_guard")
    guard.active = False
    guard.write_roots = []

    def hook(event, args):
        if not guard.active:
            return
        if event == "open":
            mode = args[1] if len(args) > 1 else None
            flags = args[2] if len(args) > 2 else 0
            writes = (isinstance(mode, str) and any(flag in mode for flag in "wax+")) or (
                mode is None and isinstance(flags, int) and flags & 0o3 != 0)
            if writes and not _is_under(args[0], guard.write_roots):
                raise PermissionError(
                    f"Writing {args[0]!r} needs the user's approval: an undoable script writes only under its own "
                    "folder or Saved/HyperAIStudio/Captures. Write the path out as a literal so the run asks first.")
        elif event in BLOCKED_EVENTS or event.startswith(BLOCKED_EVENT_PREFIXES):
            raise PermissionError(f"{event} needs the user's approval; this script ran as undoable.")

    sys.addaudithook(hook)
    sys.modules["hyperai_script_guard"] = guard
    return guard


def _checked_builtins():
    """Builtins whose attribute functions refuse, by a computed name, what the check escalates when written out."""
    blocked = set(ESCALATING_CALLS) | LEVEL_CALLS | ESCAPE_ATTRIBUTES

    def check(name):
        if isinstance(name, str) and name in blocked:
            raise PermissionError(f"Reaching {name} by a computed name needs the user's approval; write it out so the run asks first.")

    def checked_getattr(target, name, *default):
        check(name)
        return getattr(target, name, *default)

    def checked_setattr(target, name, value):
        check(name)
        setattr(target, name, value)

    def checked_delattr(target, name):
        check(name)
        delattr(target, name)

    table = dict(vars(builtins))
    table.update(getattr=checked_getattr, setattr=checked_setattr, delattr=checked_delattr)
    return table


def _attribute_hint(error):
    """Names on the object an AttributeError missed that look like the one asked for, so the next try needs no dir() probe."""
    target, name = getattr(error, "obj", None), getattr(error, "name", None)
    if target is None or not isinstance(name, str):
        return ""
    try:
        available = [a for a in dir(target) if not a.startswith("_")]
    except Exception:  # an object's own __dir__ may raise
        return ""
    words = [w for w in re.findall(r"[a-z]+|[A-Z][a-z]*", name.replace("_", " ")) if len(w) > 2 and w.lower() not in ("get", "set")]
    scored = [(sum(w.lower() in a.lower() for w in words), a) for a in available]
    related = [a for score, a in sorted(scored, key=lambda item: (-item[0], len(item[1]))) if score][:12]
    close = difflib.get_close_matches(name, available, n=4, cutoff=0.6)
    names = list(dict.fromkeys(close + related))
    if not names:
        return ""
    owner = target.__name__ if isinstance(target, (type, types.ModuleType)) else type(target).__name__
    return f" Close names on {owner}: {', '.join(names)}."


def run(script_path, timeout, out_path, guarded):
    with open(script_path, "r", encoding="utf-8") as handle:
        code = handle.read()
    record = {"ok": False, "result_json": "", "output": "", "traceback": "", "timed_out": False, "seconds": 0.0}
    output = io.StringIO()
    namespace = {"__name__": "hyperai_script", "__file__": script_path}
    if guarded:
        namespace["__builtins__"] = _checked_builtins()
    guard = _guard()
    guard.write_roots = _write_roots(script_path)
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
    except BaseException as error:  # the agent's script may raise anything, SystemExit included
        record["traceback"] = traceback.format_exc(limit=8).rstrip() + (
            _attribute_hint(error) if isinstance(error, AttributeError) else "")
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
            has_run, reasons, level_changes = analyze(code, _write_roots(argv[2]))
            if argv[4:5] != ["levels"]:
                # A caller built before level changes were checked for unsaved levels gets them as reasons, as before.
                reasons += [f"calls {name}: changes the open level" for name in level_changes]
                level_changes = []
            record = {"ok": True, "has_run": has_run, "reasons": reasons, "level_changes": level_changes}
        except SyntaxError as error:
            record = {"ok": False, "syntax_error": f"line {error.lineno}: {error.msg}"}
        with open(argv[3], "w", encoding="utf-8") as handle:
            json.dump(record, handle)
    elif len(argv) >= 6 and argv[1] == "run":
        run(argv[2], float(argv[3]), argv[4], argv[5] == "1")


main(sys.argv)
