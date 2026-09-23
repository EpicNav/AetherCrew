---
name: build-system
description: Build a gameplay system in this Unreal project with HyperAI's agent team. The gameplay-architect plans parts, builders make them in parallel, the playtester checks each part, failures go back to the builder for up to two rounds, and the reviewer reports. Use it for a request with several parts, such as "a coin pickup that adds to a score shown on the HUD".
---
# Build a gameplay system with the agent team

The team's agents live in `.hyperai/agents`: `gameplay-architect`, `blueprint-builder`, `asset-builder`, `level-builder`, `playtester` and `reviewer`. If one is missing, stop and ask the user to add the team: HyperAI workbench, Agents tab, *Add Gameplay Team*.

In Claude Code, the same flow runs as a workflow, `/hyperai-build-system <what to build>`. Prefer that when it is available.

1. **Plan.** Spawn `gameplay-architect` with the request. It returns the parts. Each part has an id, a builder, a task, the assets it owns, the parts it depends on, and acceptance checks.
2. **Build.** Start every part whose dependencies are done, all at once, each with its builder agent. Give the builder:
   - the part's task, assets and acceptance checks;
   - what the parts it depends on produced.
   Start a dependent part as soon as its own dependencies finish.
3. **Check.** When a builder finishes, spawn `playtester` with the part's acceptance checks and the builder's result.
4. **Fix.** If the playtester reports failures, give them to the part's builder with the task, then check again. After two fix rounds, mark the part failed and move on.
5. **Report.** Spawn `reviewer` with the request, the plan, and every part's result and checks. Give the user its report.

Rules:
- Two parts never touch the same asset. If two need one asset, the second depends on the first.
- Parallel agents share one editor. HyperAI edits wait their turn, shown as `queued`, and that is expected. Epic's editor tools run one call at a time.
- Tell the user as parts finish or fail. Don't go silent until the end.

How to spawn: in Codex, spawn each agent by name with your multi-agent tools. In Claude Code, use the Agent tool with the agent's name as its type.
