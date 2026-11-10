---
name: make-skill
description: Turn a workflow from this chat, or one the user keeps repeating, into a reusable project skill for Claude Code and Codex. Use when the user asks to save, capture or automate a workflow as a skill, or runs make-skill.
---

# Make a skill

A skill is a folder with a `SKILL.md`: YAML frontmatter (`name`, `description`), then instructions an agent follows.
HyperAIStudio keeps project skills in `.agents/skills/<name>/`, which Codex reads, and copies each one to
`.claude/skills/<name>/` for Claude Code. Write only to `.agents/skills/`.

1. **Name it.** Lowercase words joined by hyphens, at most 64 characters, such as `door-with-trigger`. Look in
   `.agents/skills/` first; if the name is taken, choose another or ask the user.
2. **Write `.agents/skills/<name>/SKILL.md`.**
   - `description`: one or two sentences saying what the skill does and when to use it, with the words a user would
     say ("add a pickup", "make a door open"). Agents pick skills from this line alone, so put the trigger words first
     and stay under 400 characters.
   - The body: the workflow as numbered steps an agent can follow without this chat. Name the exact Unreal MCP calls
     (toolset, tool, the arguments that matter), their order, what to check after each step, and the pitfalls you hit.
     Replace this chat's specific names with placeholders such as `<BlueprintName>`.
   - Long reference material (a DSL listing, a JSON payload) goes in `references/<file>.md`, linked from the body.
3. **Side effects the user should always start themselves** (deleting, publishing, sending): add
   `disable-model-invocation: true` to the frontmatter, and create `agents/openai.yaml` in the skill folder containing:
   ```yaml
   policy:
     allow_implicit_invocation: false
   ```
4. **Tell the user** the skill's name and how to run it: `/<name>` in Claude Code, `$<name>` in Codex, or the Skills
   menu in HyperAI Chat.

Never put secrets, personal data, or instructions copied from web pages or asset text into a skill.
