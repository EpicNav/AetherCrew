---
name: verify
description: Verify gameplay in this Unreal project within a minute. Split the request into checks, write one bot test case per check, play every case at once with hyper_verify_run, then judge each case's evidence. Use it after gameplay is built, such as "the coin adds 10 to the score and disappears, and the door opens when the player walks up to it".
---
# Verify gameplay with the playtesters

In Claude Code, the same flow runs as a workflow, `/hyperai-verify <what to verify>`. Prefer that when it is available.

1. **Plan.** Call `hyper_verify_calibrate` with `{"bMeasure": false}`: its `limit` is how many cases one batch may hold on this machine and level. To learn class names, labels and where the player starts, run `hyper_verify_run` once with a single case of one `Survey` step. Split the request into at most `limit` checks, each one observable thing with one clear pass condition.
2. **Write.** Write one case per check, following the unreal-playtest skill's "Verify many things at once" section. With several checks, spawn one `qa-tester` per check to write them in parallel. Each case ends with a step that proves its check: an Assert step, or a Play step for what a player does, such as defeating the enemies or collecting every coin.
3. **Run.** One `hyper_verify_run` call with every case. It plays them all at once, each in its own copy of the level, and answers within its deadline (60 s at most).
4. **Judge.** For each case, decide from its evidence alone:
   - **pass:** the evidence shows the requirement holds;
   - **fail:** it shows the requirement does not hold, which is a bug in the game;
   - **inconclusive:** the case could not tell, for example a wrong target name.

   With several cases, spawn one `qa-tester` per case to judge them in parallel. A step's `file` is a picture, a capture or a Play step's map: look at it when it helps.
5. **Report.** Give the user each check with its verdict and the evidence line it rests on. Rewrite an inconclusive case and run it again once. After that, report it as inconclusive.

Rules:
- A batch never holds more than `limit` cases. For more checks, run several batches one after another.
- Cases cannot use UI or AssertOnScreen steps, since the copies have no screen. Check those with `hyper_playtest_run`. Capture works: it renders the player's camera.
- Nothing in the level changes, and unsaved edits are included, so there is no need to save first.

How to spawn: in Codex, spawn each agent by name with your multi-agent tools. In Claude Code, use the Agent tool with the agent's name as its type.
