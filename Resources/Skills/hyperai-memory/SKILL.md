---
name: hyperai-memory
description: Use HyperAIStudio project memory - recall project facts and recent agent changes before working in an unfamiliar area, and save lasting conventions, decisions and gotchas. Use when starting a task, when the user says remember or forget, or when you learn something future chats need.
---

# Project memory

Every chat already sees the saved facts in AGENTS.md (Project Memory). The tools below reach the full text and the
change log. All three are on toolset `HyperAIStudio.HyperAIStudioNativeReadToolset`, through `call_tool`.

- **Recall** before working in an unfamiliar part of the project:
  `hyper_memory_recall` with `{"query": "door trigger", "limit": 10, "target": "all"}`. It returns matching facts and
  what agents changed lately; set `target` to an asset path to see only that asset's changes.
- **Remember** when you learn something lasting about this project: a convention ("Pickups live in /Game/Gameplay"),
  a decision and its reason, or a gotcha that cost you time.
  `hyper_memory_remember` with `{"title": "one line", "text": "the fact and why", "type": "convention", "replaces": "new"}`.
  Types: `convention`, `decision`, `fact`, `gotcha`. To update a fact you saved, pass its name as `replaces`.
- **Forget** a fact you saved that turned out wrong: `hyper_memory_forget` with `{"name": "<name>"}`.

Rules:
- Do not save task progress, temporary state, or anything a file or Git already records.
- The user reviews what agents save; until they confirm it, other chats may see only the title. Say what you saved.
- You cannot change or delete facts the user wrote. Ask them.
