---
name: team
description: Run a request through HyperAI's team for this Unreal project, the way a studio would. The director plans it, then the programmer, level designer, lighting artist, technical artist and UI designer build their parts at once. The QA tester proves each part against its checks and quality bar, and failures go back to their builder for up to three rounds. Any new level gets a lighting pass, and the director reviews it all. Use it for a request with more than one part, or whenever the user asks for the team.
---
# Run a request through the team

The team's agents live in `.hyperai/agents`: `director`, `programmer`, `level-designer`, `lighting-artist`, `technical-artist`, `ui-designer`, `cinematographer` and `qa-tester`. Each carries its role's quality bar. If one is missing, stop and ask the user to add the team: HyperAI workbench, Agents tab, *Add the Team*.

In Claude Code, the same flow runs as a workflow, `/hyperai-team <request>`. Prefer that when it is available.

1. **Plan.** Spawn `director` to plan the request. It returns the parts. Each part has an id, a role, a task, the assets it owns, the parts it depends on, and acceptance checks.
2. **Build.** Start every part whose dependencies are done, all at once, each with its role's agent. Give the builder:
   - the part's task, assets and acceptance checks;
   - what the parts it depends on produced.
   Start a dependent part as soon as its own dependencies finish.
3. **Test.** When a part is built, spawn `qa-tester` with the part's checks, its role, and the builder's result.
4. **Fix.** If the QA tester reports failures, give them to the part's builder with the task, then test again. After three fix rounds, mark the part failed and move on.
5. **Light.** If any part built or changed a level and no part lit it, spawn `lighting-artist` to light it to its quality bar. Then spawn `qa-tester` to check the lighting, with the same fix rounds.
6. **Review.** Spawn `director` to review the request, the plan, and every part's result and evidence. Give the user its report.

Rules:
- Two parts never touch the same asset. If two need one asset, the second depends on the first.
- Parallel agents share one editor. HyperAI edits wait their turn, shown as `queued`, and that is expected. Epic's editor tools run one call at a time.
- Tell the user as parts finish or fail. Don't go silent until the end.

How to spawn: in Codex, spawn each agent by name with your multi-agent tools. In Claude Code, use the Agent tool with the agent's name as its type. An agent with neither plays each role in turn itself, reading that role's file in `.hyperai/agents` first.
