---
name: hyperai-memory
description: Use HyperAIStudio project memory - recall project notes, past tasks and recent agent changes before working in an unfamiliar area, and save lasting conventions, decisions, gotchas and user preferences without being asked. Use when starting a task, when the user says remember or forget, or when you learn something future chats need.
---

# Project memory

Every chat sees the notes' titles in AGENTS.md (Project Memory), and a message sent from HyperAI's chat carries the
notes and past tasks that match it. The tools below reach the rest. All three are on toolset
`HyperAIStudio.HyperAIStudioNativeReadToolset`, through `call_tool`.

- **Recall** before working in an unfamiliar part of the project, or when a task looks like one done before:
  `hyper_memory_recall` with `{"query": "door trigger", "limit": 10, "target": "all"}`. It returns matching notes, the
  past tasks that match (what was asked, how it ended) and what agents changed lately; set `target` to an asset path
  to see only that asset's changes.
- **Remember**, without asking, when you learn something lasting about this project: a convention ("Pickups live in
  /Game/Gameplay"), a decision and its reason, a gotcha that cost you time, or a preference the user states. Make the
  title the fact itself: every chat sees titles.
  `hyper_memory_remember` with `{"title": "one line", "text": "the fact and why", "type": "convention", "replaces": "new"}`.
  Types: `convention`, `decision`, `fact`, `gotcha`. To update a fact you saved, pass its name as `replaces`.
- **Forget** a fact you saved that turned out wrong: `hyper_memory_forget` with `{"name": "<name>"}`.

Rules:
- Do not save task progress, temporary state, or anything a file or Git already records.
- What you save applies to later chats at once. A note that reads like instructions to agents is held for the user,
  and anything shaped like a key is refused. Say what you saved.
- You cannot change or delete facts the user wrote. Ask them.
