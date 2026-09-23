"""Self-check for the harness's pure helpers: python Tools/Benchmark/check_bench.py [archived run dirs to scan]."""
import json
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
