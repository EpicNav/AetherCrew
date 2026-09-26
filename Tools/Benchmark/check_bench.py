"""Self-check for the harness's pure helpers: python Tools/Benchmark/check_bench.py [archived run dirs to scan]."""
import json
import re
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402


def at(hour, minute, now):
    local = time.localtime(now)
    return time.mktime((local.tm_year, local.tm_mon, local.tm_mday, hour, minute, 0, 0, 0, -1))


def main():
    today = time.localtime()
    now = time.mktime((today.tm_year, today.tm_mon, today.tm_mday, 13, 52, 0, 0, 0, -1))
    codex_limit = ("You’ve hit your usage limit. Upgrade to Pro (https://chatgpt.com/explore/pro), visit "
                   "https://chatgpt.com/codex/settings/usage to purchase more credits or try again at 3:27 PM.")
    assert bench.usage_reset_time(codex_limit, now) == at(15, 27, now) + 120
    assert bench.usage_reset_time(codex_limit, at(16, 0, now)) == at(15, 27, now) + 86400 + 120, "a past clock time means tomorrow"
    assert bench.usage_reset_time("Claude AI usage limit reached|1790190000", now) == 1790190120
    assert bench.usage_reset_time("You've hit your limit · resets 3pm (Europe/London)", now) == at(15, 0, now) + 120
    assert bench.usage_reset_time("try again at 12:05 AM", now) == at(0, 5, now) + 86400 + 120
    assert bench.usage_reset_time("limit reached, no time given", now) == now + 1800

    with tempfile.TemporaryDirectory() as scratch:
        out = Path(scratch)
        (out / "stderr.txt").write_text("", encoding="utf-8")
        failed = {"agent_status": "turn.failed", "error": json.dumps({"message": codex_limit})}
        assert bench.usage_limit_message("codex", failed, out)
        assert not bench.usage_limit_message("codex", {"final": "I stopped before the usage limit."}, out), "only failures count"
        assert bench.usage_limit_message("claude", {"final": "Claude AI usage limit reached|1790190000", "is_error": True}, out)
        summary = "Done. " + "The level is lit and saved; the courtyard is symmetric. " * 5 + "No limit reached."
        assert not bench.usage_limit_message("claude", {"final": summary, "is_error": False}, out), "a real summary is not a limit"

    # A failed request is any recorded failure: a tool error, or a refusal inside HyperAI's return value.
    acct = bench.Accounting({})
    for call, body in (("a", {"returnValue": {"bOk": False, "diagnostic": "warmup_frames is 0-8."}}), ("b", {"bOk": False}),
                       ("c", {"returnValue": {"bOk": True}}), ("d", {"ok": True})):
        acct.begin(call, "unreal-mcp", "call_tool", {"tool_name": "hyper_capture_view", "arguments": {}})
        acct.end(call, False, [json.dumps(body)])
    assert acct.failed == 2, acct.summary()

    # Task sets: "all" stays the five comparison tasks; the Blueprint set has a three-minute target.
    assert [t[0] for t in bench.select_tasks("all")] == ["01-niagara", "02-material", "03-level", "04-umg", "05-animbp"]
    assert [t[0] for t in bench.select_tasks("bp")] == ["06-bp-pickup", "07-bp-door", "08-bp-health"]
    assert [t[0] for t in bench.select_tasks("01,06")] == ["01-niagara", "06-bp-pickup"]
    assert bench.task_seconds("06-bp-pickup") == 360 and bench.task_seconds("01-niagara") == bench.TASK_SECONDS
    assert all((bench.TASKS_DIR / f"{t[0]}.md").is_file() for t in bench.TASKS), "every task has its prompt"
    assert "features.memories=false" in bench.codex_command("p", None, None), "Codex runs without the user's memories"
    assert bench.claude_memory_dir().parent.name == re.sub(r"[^A-Za-z0-9]", "-", str(bench.BENCH_ROOT))
    with tempfile.TemporaryDirectory() as scratch:
        run = Path(scratch)
        def record(task, ok, seconds):
            row = {"agent": "claude", "task": task, "pass": ok, "seconds": seconds, "stop_reason": "completed",
                   "accounting": {"requests": 10, "state_changing": 2, "failed": 0}, "verification": {"checks": []}}
            if task in bench.TARGET_SECONDS:
                row.update(target_seconds=180, within_target=ok and seconds <= 180)
            return row
        meta = {"run_id": "check", "plugin_commit": "0", "started": "now", "limits": {"seconds": 900, "state_changing": 45},
                "codex_model": "m", "codex_effort": "e", "claude_model": "m", "claude_effort": "e"}
        records = [record("06-bp-pickup", True, 150), record("06-bp-pickup", True, 200), record("07-bp-door", False, 100),
                   record("01-niagara", True, 500)]
        (run / "results.json").write_text(json.dumps({"meta": meta, "records": records}), encoding="utf-8")
        summary = bench.write_report(run)["claude"]
        assert summary["targeted"] == 3 and summary["within_target"] == 1, summary
        assert summary["median_seconds"] == 175, summary

    # The control arm: HyperAIStudio off in the project file and back on, keeping every other listing.
    with tempfile.TemporaryDirectory() as scratch:
        uproject = Path(scratch) / "Bench.uproject"
        uproject.write_text(json.dumps({"Plugins": [{"Name": "Niagara", "Enabled": True}]}), encoding="utf-8")
        assert not bench.clear_plugin_off("HyperAIStudio", uproject), "nothing to clear on a plain project"
        bench.set_plugin_off("HyperAIStudio", uproject)
        bench.set_plugin_off("HyperAIStudio", uproject)
        plugins = json.loads(uproject.read_text(encoding="utf-8"))["Plugins"]
        assert plugins == [{"Name": "Niagara", "Enabled": True}, {"Name": "HyperAIStudio", "Enabled": False}], plugins
        assert bench.clear_plugin_off("HyperAIStudio", uproject)
        assert json.loads(uproject.read_text(encoding="utf-8"))["Plugins"] == [{"Name": "Niagara", "Enabled": True}]
    engine = bench.hyperai_engine_plugins()
    assert {"ModelContextProtocol", "EditorToolset", "PythonScriptPlugin"} <= set(engine) and "Niagara" not in engine, engine

    # Its agent sees none of HyperAI's files; afterwards the originals come back and the control's own leftovers go.
    with tempfile.TemporaryDirectory() as scratch:
        root, store = Path(scratch) / "Bench", Path(scratch) / "Bench" / "Saved" / "hidden"
        (root / ".claude" / "skills").mkdir(parents=True)
        (root / ".claude" / "skills" / "SKILL.md").write_text("hyperai", encoding="utf-8")
        (root / "AGENTS.md").write_text("hyperai", encoding="utf-8")
        (root / "Content").mkdir()
        assert bench.hide_agent_files(root, store) == ["AGENTS.md", ".claude"]
        assert not (root / "AGENTS.md").exists() and not (root / ".claude").exists() and (root / "Content").is_dir()
        (root / ".claude").mkdir()
        (root / ".claude" / "settings.local.json").write_text("control", encoding="utf-8")
        assert sorted(bench.restore_agent_files(root, store)) == [".claude", "AGENTS.md"]
        assert (root / ".claude" / "skills" / "SKILL.md").read_text(encoding="utf-8") == "hyperai"
        assert not (root / ".claude" / "settings.local.json").exists() and not store.exists()
        assert bench.restore_agent_files(root, store) == [], "restoring twice is harmless"

    # The control's environment restores HyperAI even when the run inside it fails.
    calls = []
    saved = {name: getattr(bench, name) for name in ("ensure_plugins", "set_plugin_off", "hide_agent_files", "stop_editor", "restore_hyperai")}
    try:
        bench.ensure_plugins = lambda source=None: calls.append("ensure")
        bench.set_plugin_off = lambda name, uproject=None: calls.append(f"off:{name}")
        bench.hide_agent_files = lambda *a: calls.append("hide") or []
        bench.stop_editor = lambda: calls.append("stop")
        bench.restore_hyperai = lambda: calls.append("restore")
        with bench.arm_environment("hyperai"):
            pass
        assert calls == [], "the product arm changes nothing"
        try:
            with bench.arm_environment("epic"):
                calls.append("run")
                raise RuntimeError("agent crashed")
        except RuntimeError:
            pass
        assert calls == ["ensure", "off:HyperAIStudio", "hide", "run", "stop", "restore"], calls
    finally:
        for name, value in saved.items():
            setattr(bench, name, value)
    run = Path("runs") / "x"
    assert bench.run_dir_for(run, "codex", "01-niagara", "epic", 0, 1) == run / "codex" / "01-niagara-epic"
    assert bench.run_dir_for(run, "codex", "01-niagara", "hyperai", 1, 3) == run / "codex" / "01-niagara-r2"

    # NeoStack's published figures agree with themselves, and the scorecard compares both arms against them.
    passes = {}
    for task, winners in bench.PUBLISHED["passes"].items():
        for product, (requests, _) in winners.items():
            passes[product] = passes.get(product, 0) + 1
            assert requests <= bench.PUBLISHED["products"][product]["requests"]
    assert all(passes.get(product, 0) == p["passed"] for product, p in bench.PUBLISHED["products"].items()), passes
    assert bench.published_uplift() == {"tasks": 2, "requests": 0.39, "failed": 0.41}
    with tempfile.TemporaryDirectory() as scratch:
        run = Path(scratch)
        def row(task, ok, requests, failed, arm):
            return {"agent": "claude", "arm": arm, "task": task, "pass": ok, "seconds": 300, "stop_reason": "completed",
                    "accounting": {"requests": requests, "state_changing": 5, "failed": failed}, "verification": {"checks": []}}
        records = [row("01-niagara", True, 30, 2, "hyperai"), row("04-umg", False, 30, 3, "hyperai"),
                   row("01-niagara", False, 100, 10, "epic"), row("04-umg", False, 100, 10, "epic")]
        (run / "results.json").write_text(json.dumps({"meta": meta, "records": records}), encoding="utf-8")
        summary = bench.write_report(run)
        assert summary["claude"]["passed"] == 1 and summary["claude+epic"]["passed"] == 0, summary
        assert bench.uplift(summary["claude"], summary["claude+epic"]) == {"tasks": 1, "requests": 0.3, "failed": 0.25}
        page = (run / "scorecard.html").read_text(encoding="utf-8")
        assert "Uplift over the control" in page and "none of the five passed" in page and "NeoStack 50 requests, 13.3 min" in page

    # Archived runs: replay each transcript through the parser and report what the check would have said.
    for run_dir in map(Path, sys.argv[1:]):
        for transcript in sorted(run_dir.glob("*/*/transcript.jsonl")):
            agent = transcript.parent.parent.name
            state, acct = {"agent": agent}, bench.Accounting(bench.load_catalog_safety())
            parse = bench.parse_codex if agent == "codex" else bench.parse_claude
            for line in transcript.read_text(encoding="utf-8", errors="replace").splitlines():
                try:
                    parse(json.loads(line), acct, state)
                except ValueError:
                    continue
            limit = bench.usage_limit_message(agent, state, transcript.parent)
            print(f"{transcript.parent.relative_to(run_dir)}: {'USAGE LIMIT: ' + limit[:80] if limit else 'ran normally'}")
    print("check_bench: ok")


if __name__ == "__main__":
    main()
