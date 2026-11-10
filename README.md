# HyperAIStudio

HyperAIStudio is a UE 5.8 editor-only plugin (Win64) that puts AI coding agents to work inside Unreal: Claude Code,
Codex and Gemini in one chat, a six-role team, and supervised jobs that finish what they start, all through Epic's
Unreal MCP.

What it does:

- Sets up Epic Unreal MCP in one step, verifies it, and writes the agent files: `AGENTS.md`, `CLAUDE.md`, and MCP
  configs for Claude Code, Codex, Gemini, Cursor and VS Code/Copilot.
- Hosts Claude Code, Codex and Gemini in its own chat view, with approvals, reports, pictures and up to four chats.
- Includes Claude API, OpenAI API and Gemini API agents that send generation requests directly to the provider,
  with a tool loop for Unreal MCP, project files and approved commands. No provider CLI or balance probe is required.
  Native and terminal views share conversations; output limits are configurable (0 = Auto, provider limits apply).
  Keys stay in Windows Credential Manager. Existing CLI routes also support API-key authentication.
  See [API setup](Docs/api-authentication.md).
- Adds 212 tools in 42 capability packs (Blueprints, levels, lighting, materials, Niagara, animation, GAS, UI, PCG
  and more), every edit planned, approved, journaled and undoable.
- Runs a team: director, programmer, level designer, lighting artist, technical artist and QA tester.
- Matches a reference image: the preferred agent lights or builds the level, and HyperAI scores every attempt and
  sends it back until it matches. Ultra Dynamic Sky included.
- Generates reference images from presets, remembers the project on its own, and names the skills a task needs.

What it does not do:

- It does not proxy MCP traffic or replace Epic Unreal MCP.
- It does not include Epic `NoRedist` plugin files, provider CLIs or accounts; CLI routes need their agent installed,
  while direct API routes use Unreal's bundled Python runtime and your provider API key.
- It never saves API keys in project files. Keys can be stored in Windows Credential Manager or inherited from environment variables.

Open it with `Ctrl+Alt+H` or `Tools > HyperAI Studio` after enabling the plugin, then run Settings > Setup > Start Setup.

Documentation: open `Docs/index.html` in a browser (it works offline), or see
https://gamesbyhyper.com/docs/project-foundations/hyper-ai-studio/
