// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * Other MCP servers: the ones agents HyperAI launches can use besides HyperAI's own Unreal MCP endpoint, and the editor
 * plugins and settings that let a program change this editor from outside. What changes the editor that way never
 * reaches HyperAI's approval gate, auto-approval, undo grouping or checks, so HyperAI warns about it (found 11 Oct 2026:
 * agents edited materials through the Nwiro Integration Kit's execute_python while auto-approval "approved nothing").
 *
 * The core here is pure: parsers take file text, classification takes facts about the editor, and the text builders
 * take findings. Only FHyperAIStudioMcpBypass::ReadConfiguredServers reads files, from the paths it is given.
 *
 * Config files can hold tokens (env values, headers, arguments, URLs with a user name). Nothing here keeps or shows
 * them: a parsed server holds its name, its URL's scheme, host and port, its command's file name, the names of the
 * tools its config lists, and words from a fixed vocabulary (nwiro, unreal, rider, ...) found in its arguments.
 * Nothing here ever writes those files.
 */

/** How a configured or hosted server relates to this editor. */
enum class EHyperAIStudioMcpServerClass : uint8
{
	/** Changes this editor outside HyperAI: no HyperAI approval, auto-approval, undo grouping or checks. */
	ChangesEditor,
	/** Other tools that do not reach this editor, as far as HyperAI can tell: listed, not alarming. */
	OtherTools
};

/** Why a finding is in its class; the text builders word each one. */
enum class EHyperAIStudioMcpReason : uint8
{
	/** The Nwiro Integration Kit's server, which runs inside this editor and has execute_python. */
	Nwiro,
	/** A Nwiro server, but the Nwiro Integration Kit is not enabled in this editor. */
	NwiroNotHere,
	/** JetBrains Rider's server, whose Unreal tools (ue_execute_python, spawn_actor, ue_play) reach this editor through RiderLink. */
	Rider,
	/** Rider's server without RiderLink in this editor. */
	RiderNoLink,
	/** RiderLink is enabled, but no agent here has Rider's server. */
	RiderLinkOnly,
	/** Named for Unreal, on this computer. */
	UnrealName,
	/** Named for Unreal, but on another computer. */
	UnrealRemote,
	/** Its address is a port this editor listens on. */
	EditorPort,
	/** An enabled plugin that names itself an MCP server. */
	McpPlugin,
	/** Python's Remote Execution is on in this editor. */
	PythonRemoteExecution,
	/** Nothing ties it to Unreal. */
	NotUnreal
};

/** Where a server's entry is, so the removal text can say what to delete. */
enum class EHyperAIStudioMcpSourceKind : uint8
{
	/** An entry under a JSON object (Container) in File. */
	JsonEntry,
	/** An entry under projects["<project>"].mcpServers in Claude Code's ~/.claude.json. */
	ClaudeLocalEntry,
	/** A [mcp_servers.<name>] table in a Codex config.toml. */
	TomlTable,
	/** A plugin enabled in this editor. */
	Plugin,
	/** A setting of this editor (Python Remote Execution). */
	EditorSetting
};

/** One place a server is configured or hosted. Names, addresses and paths only. */
struct FHyperAIStudioMcpServerSource
{
	EHyperAIStudioMcpSourceKind Kind = EHyperAIStudioMcpSourceKind::JsonEntry;
	/** Claude Code, Codex, Gemini, Cursor, VS Code; empty for this editor's own plugins and settings. */
	FString Client;
	/** project, user, or local (Claude Code's entry for this project in ~/.claude.json). */
	FString Scope;
	/** The config file, absolute; for a plugin, its folder. Empty for a setting. */
	FString File;
	/** File as people know it: .mcp.json, ~/.codex/config.toml, Plugins/NwiroIK_1.0.3_5.7_Built. */
	FString ShortFile;
	/** mcpServers, servers or mcp_servers. */
	FString Container;
	/** The entry's name as configured, or the plugin's friendly name. */
	FString EntryName;
	/** scheme://host:port, "command <name>" or "port <n>"; never a path, query, user name or argument. */
	FString Address;

	bool operator==(const FHyperAIStudioMcpServerSource& Other) const
	{
		return Kind == Other.Kind && Client == Other.Client && Scope == Other.Scope && File == Other.File
			&& Container == Other.Container && EntryName == Other.EntryName && Address == Other.Address;
	}
};

/** A server as a config file lists it, reduced to what classification needs. */
struct FHyperAIStudioConfiguredMcpServer
{
	FString Name;
	FHyperAIStudioMcpServerSource Source;
	/** The URL's scheme, host (lower case) and port, or those of a loopback URL among a command's arguments (a proxy). */
	FString Scheme;
	FString Host;
	int32 Port = 0;
	bool bHasUrl = false;
	/** The command's file name without folder or extension, lower case; empty for a URL server. */
	FString CommandName;
	/** Lower-case words of the fixed vocabulary found in the name, arguments, URL path and tool names. */
	TSet<FString> Words;
	/** Tools the config names for this server ([mcp_servers.x.tools.<tool>], enabled_tools). */
	TArray<FString> ToolNames;
	/** False when the config turns it off (Codex enabled = false, "disabled": true, Claude's disabledMcpjsonServers). */
	bool bEnabled = true;
};

/** An enabled plugin in this editor, as classification sees it. */
struct FHyperAIStudioMcpPluginFact
{
	FString Name;
	FString FriendlyName;
	FString CreatedBy;
	/** Its folder, absolute, and as people know it (Plugins/<folder>, or the engine's). */
	FString BaseDir;
	FString ShortBaseDir;
};

/** What classification needs to know about this editor. */
struct FHyperAIStudioMcpEditorFacts
{
	/** HyperAI's own Unreal MCP endpoint's port: a loopback server there is HyperAI's and is never listed. */
	int32 HyperAIPort = 8000;
	/** TCP ports this editor process listens on, when known (Windows); a loopback server on one of them is this editor's. */
	TArray<int32> ListeningPorts;
	/** Enabled plugins; classification picks the ones it knows (Nwiro, RiderLink, MCP servers by name). */
	TArray<FHyperAIStudioMcpPluginFact> Plugins;
	/** Python's Remote Execution setting. */
	bool bPythonRemoteExecution = false;
	/** Ids (lower case) of the extra servers HyperAI writes into agent configs from Settings > MCP servers. */
	TArray<FString> HyperAIExtraServerIds;
};

/** One server, or one family of entries (every nwiro entry, say), and what it means for this editor. */
struct FHyperAIStudioMcpFinding
{
	/** Lower case: nwiro, rider, python-remote-execution, or the server's own name. What "Don't warn again" keeps. */
	FString Id;
	FString DisplayName;
	EHyperAIStudioMcpServerClass Class = EHyperAIStudioMcpServerClass::OtherTools;
	EHyperAIStudioMcpReason Reason = EHyperAIStudioMcpReason::NotUnreal;
	/** The editor port it was found on, for EditorPort, or the port the Nwiro plugin listens on; 0 when none. */
	int32 Port = 0;
	TArray<FHyperAIStudioMcpServerSource> Sources;
	/** The user chose "Don't warn about <name> again" in this project. */
	bool bSilenced = false;
	/** An entry with this name is one HyperAI writes from Settings > MCP servers. */
	bool bWrittenByHyperAI = false;

	bool ChangesEditor() const { return Class == EHyperAIStudioMcpServerClass::ChangesEditor; }
};

/** The files ReadConfiguredServers reads. Tests point them at fixtures. */
struct FHyperAIStudioMcpScanPaths
{
	/** The project's root folder: .mcp.json, .codex/config.toml, .gemini/settings.json, .cursor/mcp.json, .vscode/mcp.json, .claude/settings*.json. */
	FString ProjectRoot;
	/** The user's home: ~/.gemini/settings.json and ~/.cursor/mcp.json, and how paths are shortened to ~/. */
	FString Home;
	/** Where Claude Code keeps .claude.json: CLAUDE_CONFIG_DIR, or Home. */
	FString ClaudeConfigDir;
	/** CODEX_HOME, or Home/.codex. */
	FString CodexHome;
	/** VS Code's user folder (%APPDATA%/Code/User); empty to skip. */
	FString VSCodeUserDir;
};

namespace HyperAIStudio::McpBypass
{
	/** The configured servers in a JSON config's Container object (mcpServers, servers). False when it is not JSON. */
	bool ParseJsonServers(const FString& JsonText, const FString& Container, const FHyperAIStudioMcpServerSource& Template,
		TArray<FHyperAIStudioConfiguredMcpServer>& OutServers, FString& OutError);

	/**
	 * Claude Code's ~/.claude.json: its user servers (top-level mcpServers), this project's local servers
	 * (projects["<root>"].mcpServers, any spelling of the root), and the .mcp.json servers this project turned off.
	 */
	bool ParseClaudeUserConfig(const FString& JsonText, const FString& ProjectRoot, const FString& File, const FString& ShortFile,
		TArray<FHyperAIStudioConfiguredMcpServer>& OutServers, TArray<FString>& OutDisabledProjectServers, FString& OutError);

	/** disabledMcpjsonServers from a Claude Code settings file (.claude/settings.json or settings.local.json). */
	void ParseClaudeDisabledServers(const FString& JsonText, TArray<FString>& OutDisabled);

	/** The [mcp_servers.<name>] tables of a Codex config.toml, read with a small TOML subset reader. False when it cannot be read safely. */
	bool ParseCodexToml(const FString& TomlText, const FHyperAIStudioMcpServerSource& Template,
		TArray<FHyperAIStudioConfiguredMcpServer>& OutServers, FString& OutError);

	/** Splits a URL into lower-case scheme, host and port (the scheme's default when absent); never keeps user info, path or query. */
	bool ParseUrl(const FString& Url, FString& OutScheme, FString& OutHost, int32& OutPort);

	/** localhost, 127.x.x.x, ::1, 0.0.0.0. */
	bool IsLoopbackHost(const FString& Host);

	/** Path as people know it: relative to the project, under ~/, or as it is. Forward slashes. */
	FString ShortenPath(const FString& Path, const FString& ProjectRoot, const FString& Home);

	/** Reads every config file the agents HyperAI launches use, from Paths. OutProblems names files that could not be read (never their content). */
	TArray<FHyperAIStudioConfiguredMcpServer> ReadConfiguredServers(const FHyperAIStudioMcpScanPaths& Paths, TArray<FString>& OutProblems);

	/** Every file ReadConfiguredServers may read, so a watcher can notice a change. */
	TArray<FString> GetConfigFiles(const FHyperAIStudioMcpScanPaths& Paths);

	/**
	 * Classifies servers and this editor's plugins and settings. HyperAI's own endpoint and disabled entries are left
	 * out; entries of one family (every nwiro, say) become one finding. Bypasses first, then by name.
	 */
	TArray<FHyperAIStudioMcpFinding> Classify(const TArray<FHyperAIStudioConfiguredMcpServer>& Servers,
		const FHyperAIStudioMcpEditorFacts& Facts, const TSet<FString>& Silenced);

	/** The findings that change this editor outside HyperAI; with bUnsilencedOnly, only those still warned about. */
	TArray<FHyperAIStudioMcpFinding> FilterBypass(const TArray<FHyperAIStudioMcpFinding>& Findings, bool bUnsilencedOnly);

	/** The names of the findings that change this editor, as agents and the user know them: nwiro, rider, Python remote execution. */
	TArray<FString> GetBypassNames(const TArray<FHyperAIStudioMcpFinding>& Findings);

	/**
	 * What a warning would name, one key per place of each unsilenced bypass ("nwiro=<kind>|<file>|<container>|<entry>"),
	 * sorted. A key not warned about before is a new bypass, or a new place for one.
	 */
	TArray<FString> BuildWarningKeys(const TArray<FHyperAIStudioMcpFinding>& Findings);

	/** Why a finding is in its class, in a sentence. */
	FText DescribeWhy(const FHyperAIStudioMcpFinding& Finding);

	/** Where a source is: "Claude Code (project): .mcp.json, entry "nwiro", http://localhost:5354". */
	FText DescribeSource(const FHyperAIStudioMcpServerSource& Source);

	/** How to remove a source by hand: which file, which entry; or which plugin or setting to turn off. */
	FText DescribeRemoval(const FHyperAIStudioMcpServerSource& Source, bool bWrittenByHyperAI);

	/** What agents can do through a bypass, which the notification, Activity and the details repeat. */
	FText DescribeConsequence();

	/** The editor-start notification for these findings (the unsilenced bypasses); empty when there are none. */
	FText BuildNotificationText(const TArray<FHyperAIStudioMcpFinding>& Findings);

	/** The auto-approval dialog's line for these findings (every bypass); empty when there are none. */
	FText BuildAutoApprovalLine(const TArray<FHyperAIStudioMcpFinding>& Findings);

	/** Activity's note for these findings (the unsilenced bypasses); empty when there are none. */
	FText BuildActivityNote(const TArray<FHyperAIStudioMcpFinding>& Findings);

	/** Every finding with where it is configured and how to remove it, as Markdown. */
	FString BuildDetailsMarkdown(const TArray<FHyperAIStudioMcpFinding>& Findings, const TArray<FString>& Problems);

	/** The agent instruction section for these findings' bypasses; empty when there are none, so none is written. */
	FString BuildInstructionSection(const TArray<FHyperAIStudioMcpFinding>& Findings);

	/** The log line for these findings; empty when there are none. English, for the log. */
	FString BuildLogLine(const TArray<FHyperAIStudioMcpFinding>& Findings);

	/** The classification as the capability report words it: changes_editor_outside_hyperai or other_tools. */
	FString ClassificationId(EHyperAIStudioMcpServerClass Class);

	/** A source in plain English for tools and the log: "Claude Code project .mcp.json entry nwiro (http://localhost:5354)". */
	FString DescribeSourcePlain(const FHyperAIStudioMcpServerSource& Source);
}
