"""Checks Resources/Python/hyperai_script_runner.py outside the editor. Stdlib only.

    python Tools/Scripting/check_script_runner.py

The runner never imports unreal itself, so its analysis, deadline, guard and result record behave the same here as
in the editor's Python.
"""
import json
import os
import subprocess
import sys
import tempfile

RUNNER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Resources", "Python", "hyperai_script_runner.py")


def main():
    work = tempfile.mkdtemp(prefix="hyperai-runner-")
    script = os.path.join(work, "script.py")
    out = os.path.join(work, "out.json")

    def call(*arguments):
        subprocess.run([sys.executable, RUNNER, *arguments], check=True, cwd=work)
        with open(out, encoding="utf-8") as handle:
            return json.load(handle)

    def analyze(code):
        with open(script, "w", encoding="utf-8") as handle:
            handle.write(code)
        return call("analyze", script, out)

    def run(code, timeout=5, guarded=True):
        with open(script, "w", encoding="utf-8") as handle:
            handle.write(code)
        return call("run", script, str(timeout), out, "1" if guarded else "0")

    report = analyze("import json\nimport unreal\ndef run():\n    return {}\n")
    assert report["ok"] and report["has_run"] and report["reasons"] == [], report
    report = analyze("import os\nimport unreal\ndef run():\n    unreal.EditorAssetLibrary.delete_asset('/Game/X')\n"
                     "    open('x.txt', 'w')\n    open('y.txt')\n    getattr(unreal, name)\n    return {}\n")
    assert report["reasons"] == ["imports os", "calls delete_asset: deletes an asset", "opens a file for writing",
                                 "getattr with a computed name"], report
    assert analyze("def run(:\n")["ok"] is False
    assert analyze("x = 1\n")["has_run"] is False

    report = run("def run():\n    print('hi')\n    return {'sum': 1 + 2}\n")
    assert report["ok"] and json.loads(report["result_json"]) == {"sum": 3} and "hi" in report["output"], report
    report = run("def run():\n    while True:\n        try:\n            pass\n        except Exception:\n            pass\n", timeout=1)
    assert report["timed_out"] and not report["ok"], report
    guarded = os.path.join(work, "guarded.txt")
    report = run(f"def run():\n    f = open\n    f({guarded!r}, 'w')\n")
    assert not report["ok"] and "PermissionError" in report["traceback"] and not os.path.exists(guarded), report
    unguarded = os.path.join(work, "unguarded.txt")
    report = run(f"def run():\n    f = open\n    f({unguarded!r}, 'w').close()\n    return {{}}\n", guarded=False)
    assert report["ok"] and os.path.exists(unguarded), report
    report = run("def run():\n    raise RuntimeError('boom')\n")
    assert not report["ok"] and "boom" in report["traceback"], report
    report = run("def run():\n    return {'obj': object()}\n")
    assert report["ok"] and json.loads(report["result_json"])["obj"].startswith("<object"), report
    print("script runner checks passed")


if __name__ == "__main__":
    main()
