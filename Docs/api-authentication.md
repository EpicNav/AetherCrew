# Direct provider APIs and CLI authentication

For direct generation using your API account, save the key in **Settings > Agents > API keys & authentication**, then choose **Claude API**, **OpenAI API**, or **Gemini API** in the chat's agent menu. These are built-in API agents; they do not launch Claude Code, Codex, or Gemini CLI. Their Python runtime ships with Unreal, so no provider CLI or SDK installation is needed.

| Chat route | Generation endpoint | Default model |
| --- | --- | --- |
| Claude API | `POST https://api.anthropic.com/v1/messages`, `x-api-key`, `anthropic-version: 2023-06-01` | `claude-opus-5-5` |
| OpenAI API | `POST https://api.openai.com/v1/responses`, bearer API key | `gpt-5.4` |
| Gemini API | `POST https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent`, `x-goog-api-key` | `gemini-3.8-flash` |

Use the Model menu to choose an exact model your key can access. The routes share the corresponding Claude Code, Codex and Gemini key/base-URL fields below, avoiding duplicate secrets. A custom endpoint must support that provider's request/response format; OpenAI gateways need the Responses API. HTTPS is required except on localhost. Redirects are refused rather than forwarding credentials to another host.

Sending a message starts a generation request immediately. Startup checks only that a key/runtime exists; it does **not** call balance or model-list endpoints. **Check key** is a separate optional authenticated read and does not generate a reply.

The built-in agent loops over provider tool calls and tool results until a final answer. It discovers Unreal MCP tools, preserves model conversation state, accepts image attachments, and offers project text-file reads/writes and approved commands. Writes, commands and MCP tools without a read-only annotation require approval in either view; Unreal's own plan/approval checks still apply. File tools stay inside the project. Read-only/headless sessions refuse mutations. Claude streams text as it arrives; OpenAI and Gemini show each generation when it completes.

### Output limit and terminal view

Set **Settings > Agents > API keys & authentication > Max output tokens** to **0 (Auto)** to remove HyperAI's former 4,096-token cap. Auto uses Claude's model output maximum because Anthropic requires `max_tokens`; for OpenAI and Gemini it leaves the output setting to the provider. Zero never means an unlimited model. A positive value sets a custom per-request maximum, subject to the selected model's limits. [Anthropic model limits](https://platform.claude.com/docs/en/models/overview).

This project setting is stored in `.hyperai/runtime/api-agent-options.json` as `max_output_tokens` and read on the next request, in either view. Unknown Claude model IDs resolve their maximum with an authenticated model-info read on their first Auto request; if unavailable, enter an explicit limit. This is not a balance check. Context-window limits and the 24-round tool-loop guard are separate from output tokens.

Select **Claude API**, **OpenAI API**, or **Gemini API**, then uncheck **Native View** for an interactive terminal using the same direct API engine, keys and saved conversation. It does not launch a provider CLI. Finish or stop the current request before switching views: closing its view stops the in-flight worker; saved messages resume in the other view.

Terminal commands:

- `/tokens 0` selects Auto; `/tokens 24000` sets a custom project limit; `/tokens` shows it.
- `/history` shows recent messages; `/model ID` changes the model for this terminal conversation.
- `/attach PATH` adds a PNG/JPEG/WebP image to the next prompt; `/paste` enters multiple lines, ending with a line containing only `.`.
- `/exit` saves and closes the agent; Ctrl+C stops its current request. Tools ask `Allow this action? [y/N]` before mutations.

API conversation history is separate from CLI histories under `.hyperai/runtime/api-chats/{provider}/`; the Chats menu can resume it. Provider keys are never serialized into requests on disk or conversation logs. API errors appear in the transcript, with no CLI-login fallback. Cancelling prevents subsequent tool calls; a request already accepted by a provider may still be billed until it stops/returns.

Claude Code Remote Control, CLI-specific multi-agent orchestration and CLI-specific commands remain features of their respective CLI routes.

## Existing CLI routes

Open **Settings > Agents > API keys & authentication**. Install and enable the agent, select **Get key**, paste it with **Set**, then start a new chat. Existing chats keep the credentials of their running process. An inherited environment key also works; restart Unreal after changing Windows environment variables.

The routes in the following table use each agent's CLI and its own protocols. Installing that CLI is required for these routes. Direct API routes above supply their own agent/tool loop instead.

| Agent | Credential | Selection |
| --- | --- | --- |
| Claude Code | `ANTHROPIC_API_KEY` from the Claude Console | Native chat uses the API key. Terminal chat prompts once to approve it; if previously declined, `/config` > **Use custom API key**. HyperAI clears inherited bearer tokens and cloud-provider switches in this child process when an API key is configured. |
| Codex | `OPENAI_API_KEY` from OpenAI Platform | HyperAI selects a Responses API provider for both terminal and native chat, without changing the CLI's saved ChatGPT login. `OPENAI_BASE_URL` can select a compatible gateway; it must support Responses and tool calls. |
| Gemini CLI | `GEMINI_API_KEY` from Google AI Studio | Native chat sends ACP `authenticate` with `gemini-api-key` before creating/resuming a session. Gemini remembers this auth choice in its own settings. Terminal chat: `/auth` > **Gemini API key**. |
| Cursor Agent CLI | `CURSOR_API_KEY` from the Cursor dashboard | Cursor account key. It is separate from Anthropic/OpenAI keys and does not transfer their credits into Cursor. |
| GitHub Copilot CLI | `COPILOT_GITHUB_TOKEN` | A GitHub token with the required Copilot permission and account entitlement. It does not use Anthropic/OpenAI billing. |
| OpenCode | OpenRouter, OpenAI or Anthropic key | Choose the corresponding provider/model in OpenCode. HyperAI passes the configured variables; entries marked unverified in the tooltip depend on the installed agent's provider configuration. |
| Goose | OpenRouter or OpenAI key | Select the matching provider in Goose's settings. |
| Qwen Code | `OPENAI_API_KEY`, optional `OPENAI_BASE_URL` | Configure the agent for its OpenAI-compatible provider and choose a model that endpoint supports. |
| Kimi | `KIMI_API_KEY`, optional `KIMI_BASE_URL` | Uses the agent's configured Kimi provider. |
| Auggie | `AUGMENT_SESSION_AUTH` | Augment session credential, obtained through its own authentication flow. |
| Cline | Agent-managed credentials | Authenticate using `cline auth`; HyperAI does not invent an environment-key interface. |

**Desktop editors:** In VS Code, run **Chat: Manage Language Models**, then add a model/provider and its key. In Cursor, open **Settings > Models** and configure provider keys there. HyperAI configures their Unreal MCP connection; their own secret storage owns model credentials. Launching an already-running editor with an environment variable does not reliably configure its provider.

## Credits, billing and checks

Use a key issued by the account/workspace that holds your promotional API credits. A Claude subscription login and Claude Console API billing are separate. A key does not establish the amount, expiry or permitted uses of a promotion: check the provider's console. HyperAI cannot confirm whether a specific $200 promotion covers your requests.

**Test** sends an authenticated model-list/account read without generating content. Success confirms that read was accepted; it does not prove generation/model access, available credit or promotional eligibility. The test uses the same stored/environment base URL as the agent, where supported.

Agent usage is billed by the provider/agent. HyperAI's **External Service Session Spend Cap** covers its own image/audio/3D calls, not the independent agent CLI's API requests. Claude Remote Control has its own subscription requirements; an API key does not grant that feature.

Stored values override the same environment variable. Keys go into the agent's child-process environment, not command arguments, shared project files or host request files. If a stored credential becomes unreadable, the launcher stops instead of silently falling back to another account. Provider keys for HyperAI's image/audio/3D tools remain separate from agent keys.

## Official references

- [Claude Messages API](https://platform.claude.com/docs/en/api/messages/create)
- [OpenAI function calling through Responses](https://developers.openai.com/api/docs/guides/function-calling)
- [Gemini generateContent API](https://ai.google.dev/api/generate-content)
- [Claude Code authentication and precedence](https://code.claude.com/docs/en/authentication)
- [Codex authentication](https://developers.openai.com/codex/auth/)
- [Gemini CLI authentication](https://geminicli.com/docs/get-started/authentication/)
- [Cursor CLI authentication](https://cursor.com/docs/cli/reference/authentication)
- [VS Code model and API-key configuration](https://code.visualstudio.com/docs/agent-customization/language-models)
