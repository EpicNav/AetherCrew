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
