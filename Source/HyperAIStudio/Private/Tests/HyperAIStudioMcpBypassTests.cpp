// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreGlobals.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAutoApproval.h"
#include "HyperAIStudioMcpBypass.h"
#include "HyperAIStudioMcpBypassMonitor.h"
#include "HyperAIStudioNativeReadToolset.h"
#include "HyperAIStudioService.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SHyperAIStudioAutoApprovalDialog.h"
#include "HyperAIStudioWidgetShot.h"
#include "SHyperAIStudioMcpBypassNote.h"

namespace HyperAIStudio::McpBypassTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	using namespace HyperAIStudio::McpBypass;

	/** A folder of its own under the bench's Saved folder; the user's real configs are never read here. */
	FString TempFolder(const TCHAR* Label)
	{
		const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAITests") / FString::Printf(TEXT("%s-%s"), Label,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12)));
		IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
		return Folder;
	}

	void Write(const FString& Path, const FString& Text)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	/** Every value in these fixtures that a real config would keep secret has SECRET in it, and none may come out. */
	const TCHAR* const ProjectMcpJson = TEXT(R"JSON({
  "mcpServers": {
    "unreal-mcp": { "type": "http", "url": "http://127.0.0.1:8000/mcp" },
    "nwiro": { "type": "http", "url": "http://localhost:5354/mcp", "headers": { "Authorization": "Bearer SECRET-HEADER-TOKEN" } },
    "shadcn": { "command": "npx", "args": ["shadcn@latest", "mcp"], "env": { "API_KEY": "SECRET-ENV-VALUE" } },
    "proxy": { "command": "C:\\tools\\npx.cmd", "args": ["-y", "mcp-remote", "http://agent:SECRET-URL-PASSWORD@localhost:5353/mcp?token=SECRET-QUERY"] }
  }
})JSON");

	/** ~/.claude.json: user servers, this project's local servers under another slash spelling, another project's, and an account token. */
	const TCHAR* const ClaudeUserJson = TEXT(R"JSON({
  "oauthAccount": { "accessToken": "SECRET-OAUTH" },
  "mcpServers": {
    "rider": { "type": "http", "url": "http://127.0.0.1:64482/sse" },
    "files": { "command": "npx", "args": ["@modelcontextprotocol/server-filesystem", "E:/Unreal Projects/Game", "C:/EpicGames/UE_5.8", "SECRET-ARG"] }
  },
  "projects": {
    "@PROJECT@": {
      "mcpServers": { "local-unreal": { "command": "uvx", "args": ["unreal-mcp"], "env": { "TOKEN": "SECRET-LOCAL-ENV" } } },
      "disabledMcpjsonServers": ["shadcn"]
    },
    "C:/Elsewhere/Other": { "mcpServers": { "elsewhere": { "url": "http://localhost:9999/mcp" } } }
  }
})JSON");

	const TCHAR* const UserCodexToml = TEXT(R"TOML(model = "gpt-5"
notes = """
A multi-line string with what looks like a table, which is not one:
[mcp_servers.fake]
url = "http://localhost:1/SECRET-MULTILINE"
"""

[projects.'e:\unreal projects\game']
trust_level = "trusted"

[mcp_servers.node_repl]
command = 'C:\Program Files\Codex\node_repl.exe'
args = [
  "--serve", # a comment inside an array
  "SECRET-REPL-ARG",
]
startup_timeout_sec = 30

[mcp_servers.node_repl.env]
CODEX_TOKEN = "SECRET-TOML-ENV"

[mcp_servers.blender]
url = "http://localhost:9191"
enabled = true

[mcp_servers.nwiro.tools.execute_python]
approval_mode = "approve"

[desktop.external-agent-import-sync-item-types ]
items = ["a"]

[[skills.config]]
name = "x"

[mcp_servers.nwiro]
url = "http://127.0.0.1:5354/mcp"
http_headers = { Authorization = "Bearer SECRET-TOML-HEADER" }
tool_timeout_sec = 600

[mcp_servers.off]
url = "http://localhost:4444/mcp"
enabled = false

[mcp_servers."quoted name"]
command = "uvx"
args = ["some-tool"]
)TOML");

	const TCHAR* const ProjectCodexToml = TEXT(R"TOML([mcp_servers]
unreal-mcp = { url = "http://127.0.0.1:8000/mcp" }
local-tool = { command = "C:/bin/tool.exe", args = ["--token", "SECRET-INLINE"] }
)TOML");

	const TCHAR* const ProjectGeminiJson = TEXT(R"JSON({
  "apiKey": "SECRET-GEMINI-ROOT",
  "mcpServers": { "gem-ue": { "httpUrl": "http://localhost:30010/remote", "headers": { "X-Key": "SECRET-GEMINI" } } }
})JSON");

	/** VS Code's mcp.json allows comments and trailing commas. */
	const TCHAR* const ProjectVSCodeJson = TEXT(R"JSON({
  // the team's servers
  "servers": {
    "figma": { "type": "http", "url": "https://mcp.figma.com/mcp", /* remote */ },
  },
})JSON");

	struct FFixture
	{
		FHyperAIStudioMcpScanPaths Paths;
		FString Root;
	};

	FFixture MakeFixture(const bool bBrokenCursor = true)
	{
		FFixture Fixture;
		Fixture.Root = TempFolder(TEXT("McpBypass"));
		Fixture.Paths.ProjectRoot = Fixture.Root / TEXT("Project");
		Fixture.Paths.Home = Fixture.Root / TEXT("Home");
		Fixture.Paths.ClaudeConfigDir = Fixture.Paths.Home;
		Fixture.Paths.CodexHome = Fixture.Paths.Home / TEXT(".codex");
		const FString& Project = Fixture.Paths.ProjectRoot;
		Write(Project / TEXT(".mcp.json"), ProjectMcpJson);
		Write(Project / TEXT(".codex/config.toml"), ProjectCodexToml);
		Write(Project / TEXT(".gemini/settings.json"), ProjectGeminiJson);
		Write(Project / TEXT(".vscode/mcp.json"), ProjectVSCodeJson);
		Write(Project / TEXT(".claude/settings.local.json"), TEXT(R"({"disabledMcpjsonServers": ["not-here"], "permissions": {"allow": ["SECRET-PERMISSION"]}})"));
		if (bBrokenCursor)
		{
			Write(Project / TEXT(".cursor/mcp.json"), TEXT("{ \"mcpServers\": { SECRET-BROKEN"));
		}
		// This project, spelled with backslashes as Claude Code sometimes keys it.
		FString Key = Project;
		Key.ReplaceInline(TEXT("/"), TEXT("\\\\"));
		Write(Fixture.Paths.Home / TEXT(".claude.json"), FString(ClaudeUserJson).Replace(TEXT("@PROJECT@"), *Key));
		Write(Fixture.Paths.CodexHome / TEXT("config.toml"), UserCodexToml);
		return Fixture;
	}

	const FHyperAIStudioConfiguredMcpServer* FindServer(const TArray<FHyperAIStudioConfiguredMcpServer>& Servers, const FString& Name, const FString& Client)
	{
		return Servers.FindByPredicate([&Name, &Client](const FHyperAIStudioConfiguredMcpServer& Server)
		{
			return Server.Name == Name && Server.Source.Client == Client;
		});
	}

	const FHyperAIStudioMcpFinding* FindFinding(const TArray<FHyperAIStudioMcpFinding>& Findings, const FString& Id)
	{
		return Findings.FindByPredicate([&Id](const FHyperAIStudioMcpFinding& Finding) { return Finding.Id == Id; });
	}

	/** This editor as the classification tests see it: Nwiro and RiderLink enabled, ports 8000 and 5353 and 30010 its own. */
	FHyperAIStudioMcpEditorFacts EditorWithEverything()
	{
		FHyperAIStudioMcpEditorFacts Facts;
		Facts.HyperAIPort = 8000;
		Facts.ListeningPorts = {5353, 8000, 30010};
		Facts.Plugins = {
			{TEXT("NwiroIntegrationKit"), TEXT("Nwiro Integration Kit"), TEXT("Nwiro"), TEXT("E:/P/Plugins/NwiroIK_1.0.3_5.7_Built"), TEXT("Plugins/NwiroIK_1.0.3_5.7_Built")},
			{TEXT("RiderLink"), TEXT("RiderLink"), TEXT("JetBrains"), TEXT("E:/P/Plugins/Developer/RiderLink"), TEXT("Plugins/Developer/RiderLink")},
			{TEXT("UnrealMCP"), TEXT("Unreal MCP"), TEXT("A. Developer"), TEXT("E:/P/Plugins/UnrealMCP"), TEXT("Plugins/UnrealMCP")},
			{TEXT("ModelContextProtocol"), TEXT("Model Context Protocol"), TEXT("Epic Games, Inc."), TEXT("E:/UE/Plugins/MCP"), TEXT("E:/UE/Plugins/MCP")},
			{TEXT("HyperAIStudio"), TEXT("HyperAIStudio MCP"), TEXT("Games by Hyper"), TEXT("E:/P/Plugins/HyperAI"), TEXT("Plugins/HyperAI")}};
		Facts.bPythonRemoteExecution = true;
		Facts.HyperAIExtraServerIds = {TEXT("blender")};
		return Facts;
	}

	FHyperAIStudioMcpServerSource ConfigSource(const TCHAR* Client, const TCHAR* Scope, const TCHAR* ShortFile, const TCHAR* Entry,
		const EHyperAIStudioMcpSourceKind Kind = EHyperAIStudioMcpSourceKind::JsonEntry)
	{
		FHyperAIStudioMcpServerSource Source;
		Source.Kind = Kind;
		Source.Client = Client;
		Source.Scope = Scope;
		Source.File = FString(TEXT("E:/P/")) + ShortFile;
		Source.ShortFile = ShortFile;
		Source.Container = Kind == EHyperAIStudioMcpSourceKind::TomlTable ? TEXT("mcp_servers") : TEXT("mcpServers");
		Source.EntryName = Entry;
		Source.Address = TEXT("http://localhost:5354");
		return Source;
	}

	/** nwiro in .mcp.json, Codex's user config and the plugin; rider in ~/.claude.json; shadcn, other tools. */
	TArray<FHyperAIStudioMcpFinding> SampleFindings()
	{
		FHyperAIStudioMcpFinding Nwiro;
		Nwiro.Id = TEXT("nwiro");
		Nwiro.DisplayName = TEXT("nwiro");
		Nwiro.Class = EHyperAIStudioMcpServerClass::ChangesEditor;
		Nwiro.Reason = EHyperAIStudioMcpReason::Nwiro;
		Nwiro.Sources.Add(ConfigSource(TEXT("Claude Code"), TEXT("project"), TEXT(".mcp.json"), TEXT("nwiro")));
		Nwiro.Sources.Add(ConfigSource(TEXT("Codex"), TEXT("user"), TEXT("~/.codex/config.toml"), TEXT("nwiro"), EHyperAIStudioMcpSourceKind::TomlTable));
		FHyperAIStudioMcpServerSource Plugin;
		Plugin.Kind = EHyperAIStudioMcpSourceKind::Plugin;
		Plugin.EntryName = TEXT("Nwiro Integration Kit");
		Plugin.File = TEXT("E:/P/Plugins/NwiroIK_1.0.3_5.7_Built");
		Plugin.ShortFile = TEXT("Plugins/NwiroIK_1.0.3_5.7_Built");
		Plugin.Address = TEXT("port 5353");
		Nwiro.Sources.Add(Plugin);

		FHyperAIStudioMcpFinding Rider;
		Rider.Id = TEXT("rider");
		Rider.DisplayName = TEXT("rider");
		Rider.Class = EHyperAIStudioMcpServerClass::ChangesEditor;
		Rider.Reason = EHyperAIStudioMcpReason::Rider;
		Rider.Sources.Add(ConfigSource(TEXT("Claude Code"), TEXT("user"), TEXT("~/.claude.json"), TEXT("rider")));

		FHyperAIStudioMcpFinding Shadcn;
		Shadcn.Id = TEXT("shadcn");
		Shadcn.DisplayName = TEXT("shadcn");
		Shadcn.Sources.Add(ConfigSource(TEXT("Codex"), TEXT("user"), TEXT("~/.codex/config.toml"), TEXT("shadcn"), EHyperAIStudioMcpSourceKind::TomlTable));
		return {Nwiro, Rider, Shadcn};
	}

	/** Restores the monitor's real findings and config file whatever a test did. */
	struct FMonitorScope
	{
		FMonitorScope()
		{
			FHyperAIStudioMcpBypassMonitor::ResetNotifiedForTest();
		}
		~FMonitorScope()
		{
			FHyperAIStudioMcpBypassMonitor::SetFindingsForTest({});
			FHyperAIStudioMcpBypassMonitor::SetConfigFileForTest(FString());
			FHyperAIStudioMcpBypassMonitor::ResetNotifiedForTest();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMcpBypassParseTest,
	"HyperAIStudio.McpBypass.ParsesConfigsWithoutSecrets",
	HyperAIStudio::McpBypassTests::Flags)

bool FHyperAIStudioMcpBypassParseTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::McpBypassTests;
	const FFixture Fixture = MakeFixture();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Fixture.Root, /*RequireExists=*/false, /*Tree=*/true); };

	TArray<FString> Problems;
	const TArray<FHyperAIStudioConfiguredMcpServer> Servers = ReadConfiguredServers(Fixture.Paths, Problems);

	// Claude Code's project file.
	const FHyperAIStudioConfiguredMcpServer* Nwiro = FindServer(Servers, TEXT("nwiro"), TEXT("Claude Code"));
	TestTrue(TEXT(".mcp.json's nwiro is read"), Nwiro && Nwiro->Source.Scope == TEXT("project") && Nwiro->Source.ShortFile == TEXT(".mcp.json")
		&& Nwiro->Host == TEXT("localhost") && Nwiro->Port == 5354 && Nwiro->Source.Address == TEXT("http://localhost:5354"));
	const FHyperAIStudioConfiguredMcpServer* Proxy = FindServer(Servers, TEXT("proxy"), TEXT("Claude Code"));
	TestTrue(TEXT("A command proxying to a loopback URL keeps where it goes, without its user name or query"), Proxy && Proxy->CommandName == TEXT("npx")
		&& Proxy->Host == TEXT("localhost") && Proxy->Port == 5353 && Proxy->Source.Address == TEXT("command npx -> http://localhost:5353"));
	const FHyperAIStudioConfiguredMcpServer* Shadcn = FindServer(Servers, TEXT("shadcn"), TEXT("Claude Code"));
	TestTrue(TEXT("A .mcp.json server this project turned off in ~/.claude.json is off"), Shadcn && !Shadcn->bEnabled);
	TestTrue(TEXT("HyperAI's own entry is read too (classification leaves it out)"), FindServer(Servers, TEXT("unreal-mcp"), TEXT("Claude Code")) != nullptr);

	// ~/.claude.json: user servers, this project's local ones (any slash spelling), never another project's.
	const FHyperAIStudioConfiguredMcpServer* Rider = FindServer(Servers, TEXT("rider"), TEXT("Claude Code"));
	TestTrue(TEXT("~/.claude.json's user rider is read"), Rider && Rider->Source.Scope == TEXT("user") && Rider->Source.ShortFile == TEXT("~/.claude.json")
		&& Rider->Port == 64482);
	const FHyperAIStudioConfiguredMcpServer* Local = FindServer(Servers, TEXT("local-unreal"), TEXT("Claude Code"));
	TestTrue(TEXT("this project's local server is read, its key spelled with backslashes"), Local && Local->Source.Kind == EHyperAIStudioMcpSourceKind::ClaudeLocalEntry
		&& Local->Source.Scope == TEXT("local") && Local->CommandName == TEXT("uvx") && Local->Words.Contains(TEXT("unreal")));
	TestNull(TEXT("another project's server is not"), FindServer(Servers, TEXT("elsewhere"), TEXT("Claude Code")));
	const FHyperAIStudioConfiguredMcpServer* Files = FindServer(Servers, TEXT("files"), TEXT("Claude Code"));
	TestTrue(TEXT("a project or engine folder among the arguments does not make a server an Unreal one"), Files && !Files->Words.Contains(TEXT("unreal")));

	// Codex: the user's TOML with tables in any order, multi-line strings and arrays, and the project's inline tables.
	const FHyperAIStudioConfiguredMcpServer* CodexNwiro = FindServer(Servers, TEXT("nwiro"), TEXT("Codex"));
	TestTrue(TEXT("Codex's nwiro is read with the tools its config names"), CodexNwiro && CodexNwiro->Source.Kind == EHyperAIStudioMcpSourceKind::TomlTable
		&& CodexNwiro->Source.Scope == TEXT("user") && CodexNwiro->Source.ShortFile == TEXT("~/.codex/config.toml") && CodexNwiro->Port == 5354
		&& CodexNwiro->ToolNames.Contains(TEXT("execute_python")));
	const FHyperAIStudioConfiguredMcpServer* Repl = FindServer(Servers, TEXT("node_repl"), TEXT("Codex"));
	TestTrue(TEXT("a command's file name, from a literal string with backslashes"), Repl && Repl->CommandName == TEXT("node_repl"));
	const FHyperAIStudioConfiguredMcpServer* Off = FindServer(Servers, TEXT("off"), TEXT("Codex"));
	TestTrue(TEXT("enabled = false is off"), Off && !Off->bEnabled);
	TestNotNull(TEXT("a quoted table name is read"), FindServer(Servers, TEXT("quoted name"), TEXT("Codex")));
	TestNull(TEXT("a table inside a multi-line string is not a table"), FindServer(Servers, TEXT("fake"), TEXT("Codex")));
	const FHyperAIStudioConfiguredMcpServer* Inline = FindServer(Servers, TEXT("local-tool"), TEXT("Codex"));
	TestTrue(TEXT("the project's inline table is read"), Inline && Inline->Source.Scope == TEXT("project") && Inline->CommandName == TEXT("tool"));
	TestTrue(TEXT("Gemini's httpUrl is read"), FindServer(Servers, TEXT("gem-ue"), TEXT("Gemini")) && FindServer(Servers, TEXT("gem-ue"), TEXT("Gemini"))->Port == 30010);
	const FHyperAIStudioConfiguredMcpServer* Figma = FindServer(Servers, TEXT("figma"), TEXT("VS Code"));
	TestTrue(TEXT("VS Code's servers, with comments and trailing commas"), Figma && Figma->Source.Container == TEXT("servers") && Figma->Port == 443);

	// A file that cannot be read is named, never shown.
	TestTrue(TEXT("The broken Cursor file is reported: ") + FString::Join(Problems, TEXT(" | ")),
		Problems.Num() == 1 && Problems[0].Contains(TEXT(".cursor/mcp.json")));

	// Nothing a config keeps secret comes out of any of it: parsed fields, findings, and every text built from them.
	FString Everything = FString::Join(Problems, TEXT("\n"));
	for (const FHyperAIStudioConfiguredMcpServer& Server : Servers)
	{
		Everything += Server.Name + Server.Scheme + Server.Host + Server.CommandName + FString::Join(Server.Words.Array(), TEXT(","))
			+ FString::Join(Server.ToolNames, TEXT(",")) + Server.Source.Client + Server.Source.Scope + Server.Source.File + Server.Source.ShortFile
			+ Server.Source.Container + Server.Source.EntryName + Server.Source.Address + TEXT("\n");
	}
	const TArray<FHyperAIStudioMcpFinding> Findings = Classify(Servers, EditorWithEverything(), {});
	for (const FHyperAIStudioMcpFinding& Finding : Findings)
	{
		Everything += DescribeWhy(Finding).ToString();
		for (const FHyperAIStudioMcpServerSource& Source : Finding.Sources)
		{
			Everything += DescribeSource(Source).ToString() + DescribeRemoval(Source, Finding.bWrittenByHyperAI).ToString() + DescribeSourcePlain(Source);
		}
	}
	Everything += BuildDetailsMarkdown(Findings, Problems) + BuildLogLine(Findings) + BuildInstructionSection(Findings)
		+ BuildNotificationText(Findings).ToString() + BuildAutoApprovalLine(Findings).ToString() + BuildActivityNote(Findings).ToString()
		+ FString::Join(BuildWarningKeys(Findings), TEXT("\n"));
	TestFalse(TEXT("No secret from the fixtures comes out"), Everything.Contains(TEXT("SECRET")));
	TestFalse(TEXT("nor a URL's user name"), Everything.Contains(TEXT("agent:")));
	TestFalse(TEXT("nor a URL's path or query"), Everything.Contains(TEXT("/mcp?")) || Everything.Contains(TEXT("token=")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMcpBypassClassifyTest,
	"HyperAIStudio.McpBypass.Classification",
	HyperAIStudio::McpBypassTests::Flags)

bool FHyperAIStudioMcpBypassClassifyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::McpBypassTests;
	using EClass = EHyperAIStudioMcpServerClass;
	using EReason = EHyperAIStudioMcpReason;
	const FFixture Fixture = MakeFixture(/*bBrokenCursor=*/false);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Fixture.Root, /*RequireExists=*/false, /*Tree=*/true); };
	TArray<FString> Problems;
	const TArray<FHyperAIStudioConfiguredMcpServer> Servers = ReadConfiguredServers(Fixture.Paths, Problems);

	const TArray<FHyperAIStudioMcpFinding> Findings = Classify(Servers, EditorWithEverything(), {TEXT("rider")});
	TestNull(TEXT("HyperAI's own endpoint is never listed"), FindFinding(Findings, TEXT("unreal-mcp")));
	const FHyperAIStudioMcpFinding* Nwiro = FindFinding(Findings, TEXT("nwiro"));
	TestTrue(TEXT("nwiro changes this editor: the plugin is here"), Nwiro && Nwiro->Class == EClass::ChangesEditor && Nwiro->Reason == EReason::Nwiro && Nwiro->Port == 5353);
	if (Nwiro)
	{
		TestTrue(TEXT("its places are every config entry, the proxy to its port, and the plugin"),
			Nwiro->Sources.ContainsByPredicate([](const FHyperAIStudioMcpServerSource& Source) { return Source.ShortFile == TEXT(".mcp.json") && Source.EntryName == TEXT("nwiro"); })
			&& Nwiro->Sources.ContainsByPredicate([](const FHyperAIStudioMcpServerSource& Source) { return Source.EntryName == TEXT("proxy"); })
			&& Nwiro->Sources.ContainsByPredicate([](const FHyperAIStudioMcpServerSource& Source) { return Source.ShortFile == TEXT("~/.codex/config.toml"); })
			&& Nwiro->Sources.ContainsByPredicate([](const FHyperAIStudioMcpServerSource& Source)
			{
				return Source.Kind == EHyperAIStudioMcpSourceKind::Plugin && Source.EntryName == TEXT("Nwiro Integration Kit") && Source.Address == TEXT("port 5353");
			}));
	}
	const FHyperAIStudioMcpFinding* Rider = FindFinding(Findings, TEXT("rider"));
	TestTrue(TEXT("rider changes this editor through RiderLink, and keeps its silence"), Rider && Rider->Class == EClass::ChangesEditor && Rider->Reason == EReason::Rider
		&& Rider->bSilenced && Rider->Sources.ContainsByPredicate([](const FHyperAIStudioMcpServerSource& Source) { return Source.EntryName == TEXT("RiderLink"); }));
	const FHyperAIStudioMcpFinding* Local = FindFinding(Findings, TEXT("local-unreal"));
	TestTrue(TEXT("a local command named for Unreal changes this editor"), Local && Local->Class == EClass::ChangesEditor && Local->Reason == EReason::UnrealName);
	const FHyperAIStudioMcpFinding* Gemini = FindFinding(Findings, TEXT("gem-ue"));
	TestTrue(TEXT("so does Gemini's ue server on this computer"), Gemini && Gemini->ChangesEditor());
	const FHyperAIStudioMcpFinding* Blender = FindFinding(Findings, TEXT("blender"));
	TestTrue(TEXT("blender is another tool, and HyperAI wrote it"), Blender && Blender->Class == EClass::OtherTools && Blender->Reason == EReason::NotUnreal && Blender->bWrittenByHyperAI);
	const FHyperAIStudioMcpFinding* Figma = FindFinding(Findings, TEXT("figma"));
	TestTrue(TEXT("figma is another tool"), Figma && !Figma->ChangesEditor());
	const FHyperAIStudioMcpFinding* Files = FindFinding(Findings, TEXT("files"));
	TestTrue(TEXT("a file server given the engine's folder is another tool"), Files && !Files->ChangesEditor());
	TestNull(TEXT("a server turned off is not listed"), FindFinding(Findings, TEXT("off")));
	TestNull(TEXT("nor one this project turned off"), FindFinding(Findings, TEXT("shadcn")));
	const FHyperAIStudioMcpFinding* Plugin = FindFinding(Findings, TEXT("unrealmcp"));
	TestTrue(TEXT("a third-party plugin named for MCP changes this editor"), Plugin && Plugin->Class == EClass::ChangesEditor && Plugin->Reason == EReason::McpPlugin);
	TestNull(TEXT("Epic's own MCP plugin is not listed"), FindFinding(Findings, TEXT("modelcontextprotocol")));
	TestNull(TEXT("nor HyperAI"), FindFinding(Findings, TEXT("hyperaistudio")));
	const FHyperAIStudioMcpFinding* Python = FindFinding(Findings, TEXT("python-remote-execution"));
	TestTrue(TEXT("Python Remote Execution changes this editor"), Python && Python->ChangesEditor() && Python->DisplayName == TEXT("Python remote execution"));
	bool bSorted = true;
	for (int32 Index = 1; Index < Findings.Num(); ++Index)
	{
		bSorted &= !(Findings[Index].ChangesEditor() && !Findings[Index - 1].ChangesEditor());
	}
	TestTrue(TEXT("Bypasses come first"), bSorted);

	// The same configs in an editor without the plugins, the setting or the ports.
	FHyperAIStudioMcpEditorFacts Plain;
	Plain.HyperAIPort = 8000;
	const TArray<FHyperAIStudioMcpFinding> Elsewhere = Classify(Servers, Plain, {});
	const FHyperAIStudioMcpFinding* NwiroAway = FindFinding(Elsewhere, TEXT("nwiro"));
	TestTrue(TEXT("Without the Nwiro plugin, nwiro cannot reach this editor"), NwiroAway && NwiroAway->Class == EClass::OtherTools && NwiroAway->Reason == EReason::NwiroNotHere);
	const FHyperAIStudioMcpFinding* RiderAway = FindFinding(Elsewhere, TEXT("rider"));
	TestTrue(TEXT("Without RiderLink, rider cannot either"), RiderAway && RiderAway->Class == EClass::OtherTools && RiderAway->Reason == EReason::RiderNoLink);
	TestNull(TEXT("and no setting is listed"), FindFinding(Elsewhere, TEXT("python-remote-execution")));

	// Hand-made servers for the remaining rules.
	auto Server = [](const TCHAR* Name, const TCHAR* Url)
	{
		FHyperAIStudioConfiguredMcpServer Made;
		Made.Name = Name;
		Made.Source.Client = TEXT("Codex");
		Made.Source.EntryName = Name;
		Made.bHasUrl = ParseUrl(Url, Made.Scheme, Made.Host, Made.Port);
		TArray<FHyperAIStudioConfiguredMcpServer> One;
		One.Add(Made);
		return One;
	};
	TArray<FHyperAIStudioConfiguredMcpServer> Hand = Server(TEXT("custom"), TEXT("http://127.0.0.1:30010/x"));
	Hand.Append(Server(TEXT("unreal-cloud"), TEXT("https://unreal.example.com/mcp")));
	Hand.Append(Server(TEXT("image-provider"), TEXT("http://127.0.0.1:7001/mcp")));
	Hand[1].Words.Add(TEXT("unreal"));
	FHyperAIStudioMcpEditorFacts Ports;
	Ports.ListeningPorts = {30010};
	const TArray<FHyperAIStudioMcpFinding> HandFindings = Classify(Hand, Ports, {});
	const FHyperAIStudioMcpFinding* Custom = FindFinding(HandFindings, TEXT("custom"));
	TestTrue(TEXT("A loopback server on a port this editor listens on is this editor's"), Custom && Custom->Reason == EReason::EditorPort && Custom->Port == 30010
		&& DescribeWhy(*Custom).ToString().Contains(TEXT("30010")));
	const FHyperAIStudioMcpFinding* Cloud = FindFinding(HandFindings, TEXT("unreal-cloud"));
	TestTrue(TEXT("An Unreal server on another computer does not reach this editor"), Cloud && Cloud->Class == EClass::OtherTools && Cloud->Reason == EReason::UnrealRemote);
	TestTrue(TEXT("\"provider\" is not rider"), FindFinding(HandFindings, TEXT("image-provider")) && !FindFinding(HandFindings, TEXT("rider")));

	// RiderLink with no Rider server configured is listed, not alarming.
	FHyperAIStudioMcpEditorFacts LinkOnly;
	LinkOnly.Plugins = {{TEXT("RiderLink"), TEXT("RiderLink"), TEXT("JetBrains"), TEXT("E:/P/Plugins/Developer/RiderLink"), TEXT("Plugins/Developer/RiderLink")}};
	const TArray<FHyperAIStudioMcpFinding> LinkFindings = Classify({}, LinkOnly, {});
	TestTrue(TEXT("RiderLink alone is another tool"), LinkFindings.Num() == 1 && LinkFindings[0].Id == TEXT("riderlink") && LinkFindings[0].Reason == EReason::RiderLinkOnly
		&& !LinkFindings[0].ChangesEditor());

	// URLs: user info, IPv6, defaults, and what is not one.
	FString Scheme;
	FString Host;
	int32 Port = 0;
	TestTrue(TEXT("user info is dropped"), ParseUrl(TEXT("http://user:pw@LocalHost:5353/mcp?x=1"), Scheme, Host, Port) && Host == TEXT("localhost") && Port == 5353);
	TestTrue(TEXT("IPv6 loopback"), ParseUrl(TEXT("http://[::1]:8080/"), Scheme, Host, Port) && IsLoopbackHost(Host) && Port == 8080);
	TestTrue(TEXT("https defaults to 443"), ParseUrl(TEXT("https://example.com"), Scheme, Host, Port) && Port == 443 && !IsLoopbackHost(Host));
	TestFalse(TEXT("an environment placeholder is not a URL"), ParseUrl(TEXT("${NWIRO_URL}"), Scheme, Host, Port));
	TestEqual(TEXT("paths shorten to the project and ~/"), ShortenPath(TEXT("C:/Users/me/.codex/config.toml"), TEXT("E:/P/"), TEXT("C:\\Users\\me")),
		FString(TEXT("~/.codex/config.toml")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMcpBypassSilenceTest,
	"HyperAIStudio.McpBypass.DontWarnAgainIsPerProject",
	HyperAIStudio::McpBypassTests::Flags)

bool FHyperAIStudioMcpBypassSilenceTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::McpBypassTests;
	FMonitorScope Scope;
	TestEqual(TEXT("The choice lives in the editor's per-project user settings"), FHyperAIStudioMcpBypassMonitor::GetConfigFileForTest(), GEditorPerProjectIni);
	TestTrue(TEXT("which is the user's Saved config, not the project's checked-in Default*.ini: ") + GEditorPerProjectIni,
		GEditorPerProjectIni.Contains(TEXT("Saved")) && GEditorPerProjectIni.Contains(TEXT("EditorPerProjectUserSettings")) && !GEditorPerProjectIni.Contains(TEXT("Default")));

	// The bench's own per-user settings file, as the editor uses it; what it held before is put back afterwards.
	const FString Ini = GEditorPerProjectIni;
	const TSet<FString> Before = FHyperAIStudioMcpBypassMonitor::ReadSilenced(Ini);
	ON_SCOPE_EXIT
	{
		FHyperAIStudioMcpBypassMonitor::WriteSilenced(Ini, Before);
		FHyperAIStudioMcpBypassMonitor::SetConfigFileForTest(FString());
	};
	TSet<FString> Start = Before;
	Start.Remove(TEXT("nwiro"));
	Start.Remove(TEXT("rider"));
	FHyperAIStudioMcpBypassMonitor::WriteSilenced(Ini, Start);
	// Reads the choices again.
	FHyperAIStudioMcpBypassMonitor::SetConfigFileForTest(FString());
	FHyperAIStudioMcpBypassMonitor::SetFindingsForTest(SampleFindings());

	bool bWouldShow = false;
	FText Pending = FHyperAIStudioMcpBypassMonitor::GetPendingNotificationForTest(bWouldShow);
	TestTrue(TEXT("Both bypasses would be warned about: ") + Pending.ToString(), bWouldShow && Pending.ToString().Contains(TEXT("nwiro (")) && Pending.ToString().Contains(TEXT("rider (")));
	TestFalse(TEXT("but not the other tool"), Pending.ToString().Contains(TEXT("shadcn")));

	FHyperAIStudioMcpBypassMonitor::SetWarningSilenced(TEXT("nwiro"), true);
	TestTrue(TEXT("nwiro is marked silenced"), FindFinding(FHyperAIStudioMcpBypassMonitor::GetFindings(), TEXT("nwiro"))->bSilenced);
	TestTrue(TEXT("and remembered in the file"), FHyperAIStudioMcpBypassMonitor::ReadSilenced(Ini).Contains(TEXT("nwiro")));
	FString Written;
	FFileHelper::LoadFileToString(Written, *Ini);
	TestTrue(TEXT("as written to disk: ") + Written, Written.Contains(TEXT("SilencedWarnings=nwiro")));
	Pending = FHyperAIStudioMcpBypassMonitor::GetPendingNotificationForTest(bWouldShow);
	TestTrue(TEXT("The warning no longer names it, only rider: ") + Pending.ToString(), !Pending.ToString().Contains(TEXT("nwiro")) && Pending.ToString().Contains(TEXT("rider")));
	TestTrue(TEXT("Activity's note leaves it out too"), []()
	{
		const TSharedRef<SHyperAIStudioMcpBypassNote> Note = SNew(SHyperAIStudioMcpBypassNote);
		return !Note->GetText().ToString().Contains(TEXT("nwiro")) && Note->GetText().ToString().Contains(TEXT("rider"));
	}());
	TestTrue(TEXT("The auto-approval line still names it: a silenced warning is no approval"),
		BuildAutoApprovalLine(FHyperAIStudioMcpBypassMonitor::GetFindings()).ToString().Contains(TEXT("nwiro")));
	TestTrue(TEXT("and so does the agent rule"), BuildInstructionSection(FHyperAIStudioMcpBypassMonitor::GetFindings()).Contains(TEXT("`nwiro`")));

	// Another project's settings file is its own: it does not have the choice.
	{
		const FString Folder = TempFolder(TEXT("McpBypassOtherProject"));
		const FString Other = FPaths::ConvertRelativePathToFull(Folder / TEXT("EditorPerProjectUserSettings.ini"));
		Write(Other, TEXT("[HyperAIStudio.McpBypass]\n"));
		TestFalse(TEXT("Another project's file does not have it"), FHyperAIStudioMcpBypassMonitor::ReadSilenced(Other).Contains(TEXT("nwiro")));
		GConfig->UnloadFile(Other);
		IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists=*/false, /*Tree=*/true);
	}

	// What was warned about once is not again; a new place for a bypass is.
	FHyperAIStudioMcpBypassMonitor::ResetNotifiedForTest();
	TArray<FHyperAIStudioMcpFinding> Findings = SampleFindings();
	const TArray<FString> KeysBefore = BuildWarningKeys(Findings);
	Findings[1].Sources.Add(ConfigSource(TEXT("Codex"), TEXT("user"), TEXT("~/.codex/config.toml"), TEXT("rider"), EHyperAIStudioMcpSourceKind::TomlTable));
	const TArray<FString> After = BuildWarningKeys(Findings);
	TestTrue(TEXT("A new place for rider is a new key"), After.Num() == KeysBefore.Num() + 1 && After.ContainsByPredicate([&KeysBefore](const FString& Key) { return !KeysBefore.Contains(Key); }));
	Findings[0].bSilenced = true;
	TestFalse(TEXT("A silenced bypass has no keys"), BuildWarningKeys(Findings).ContainsByPredicate([](const FString& Key) { return Key.StartsWith(TEXT("nwiro=")); }));

	FHyperAIStudioMcpBypassMonitor::SetWarningSilenced(TEXT("nwiro"), false);
	TestFalse(TEXT("Warn Again forgets it"), FHyperAIStudioMcpBypassMonitor::ReadSilenced(Ini).Contains(TEXT("nwiro")));
	Pending = FHyperAIStudioMcpBypassMonitor::GetPendingNotificationForTest(bWouldShow);
	TestTrue(TEXT("and the warning names it again"), bWouldShow && Pending.ToString().Contains(TEXT("nwiro")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMcpBypassInstructionsTest,
	"HyperAIStudio.McpBypass.InstructionOnlyWithFindings",
	HyperAIStudio::McpBypassTests::Flags)

bool FHyperAIStudioMcpBypassInstructionsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::McpBypassTests;
	const TArray<FHyperAIStudioMcpFinding> Findings = SampleFindings();
	const FString Rule = TEXT("- Use HyperAI's tools for changes to this editor. Other servers configured here (`nwiro`, `rider`) bypass HyperAI's approvals, undo and checks; use them for edits only if the user asks.");
	TestTrue(TEXT("With bypasses, the rule names them"), BuildInstructionSection(Findings).Contains(Rule));
	TestTrue(TEXT("No findings, no rule"), BuildInstructionSection({}).IsEmpty());
	TestTrue(TEXT("Only other tools, no rule"), BuildInstructionSection({Findings[2]}).IsEmpty());

	const FString Folder = TempFolder(TEXT("McpBypassAgents"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists=*/false, /*Tree=*/true); };
	const FString Agents = Folder / TEXT("AGENTS.md");
	Write(Agents, TEXT("# Mine\n"));
	TestFalse(TEXT("A file Setup did not make is left alone"), FHyperAIStudioService::WriteMcpBypassInstructions(Agents, Findings));
	FString Text;
	FFileHelper::LoadFileToString(Text, *Agents);
	TestEqual(TEXT("untouched"), Text, FString(TEXT("# Mine\n")));

	const FString Main = TEXT("# Mine\r\n\r\n<!-- BEGIN HYPERAISTUDIO MANAGED BLOCK -->\r\n# HyperAIStudio Agent Instructions\r\n<!-- END HYPERAISTUDIO MANAGED BLOCK -->\r\n");
	Write(Agents, Main);
	TestTrue(TEXT("With findings the block is written"), FHyperAIStudioService::WriteMcpBypassInstructions(Agents, Findings));
	FFileHelper::LoadFileToString(Text, *Agents);
	TestTrue(TEXT("beside the main block, with its own markers and the rule"), Text.StartsWith(Main) && Text.Contains(TEXT("<!-- BEGIN HYPERAISTUDIO OTHER MCP SERVERS -->"))
		&& Text.Contains(TEXT("## Other MCP Servers")) && Text.Contains(Rule));
	TestTrue(TEXT("Writing it again changes nothing"), FHyperAIStudioService::WriteMcpBypassInstructions(Agents, Findings));
	FString Again;
	FFileHelper::LoadFileToString(Again, *Agents);
	TestEqual(TEXT("same text"), Again, Text);
	TestTrue(TEXT("Without findings the block goes"), FHyperAIStudioService::WriteMcpBypassInstructions(Agents, {Findings[2]}));
	FFileHelper::LoadFileToString(Text, *Agents);
	TestTrue(TEXT("and the rest stays: ") + Text, !Text.Contains(TEXT("OTHER MCP SERVERS")) && !Text.Contains(TEXT("bypass HyperAI's approvals"))
		&& Text.Contains(TEXT("<!-- BEGIN HYPERAISTUDIO MANAGED BLOCK -->")) && Text.StartsWith(TEXT("# Mine")));

	// The main block the generator writes does not depend on this machine's servers.
	const FString Generated = FHyperAIStudioService::BuildAgentsInstructionText();
	TestFalse(TEXT("The generator's main block never carries the rule"), Generated.Contains(TEXT("bypass HyperAI's approvals")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMcpBypassTextsTest,
	"HyperAIStudio.McpBypass.WarningTexts",
	HyperAIStudio::McpBypassTests::Flags)

bool FHyperAIStudioMcpBypassTextsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::McpBypassTests;
	FMonitorScope Scope;
	const TArray<FHyperAIStudioMcpFinding> Findings = SampleFindings();

	TestEqual(TEXT("The notification names each bypass and where it is"), BuildNotificationText({Findings[0]}).ToString(),
		FString(TEXT("Agents can change this editor without HyperAI's approval through nwiro (.mcp.json, ~/.codex/config.toml, the Nwiro Integration Kit plugin). There they can edit assets and run Python with no HyperAI approval, no auto-approval, no undo grouping and no HyperAI checks.")));
	TestTrue(TEXT("Several are listed"), BuildNotificationText(Findings).ToString().Contains(TEXT("nwiro (.mcp.json, ~/.codex/config.toml, the Nwiro Integration Kit plugin); rider (~/.claude.json)")));
	TestTrue(TEXT("Other tools alone warn about nothing"), BuildNotificationText({Findings[2]}).IsEmpty());

	TestEqual(TEXT("The auto-approval line, for one"), BuildAutoApprovalLine({Findings[0]}).ToString(),
		FString(TEXT("Agents can also use nwiro, which bypasses approvals entirely; auto-approval does not cover it.")));
	TestEqual(TEXT("and for two"), BuildAutoApprovalLine(Findings).ToString(),
		FString(TEXT("Agents can also use nwiro and rider, which bypass approvals entirely; auto-approval does not cover them.")));
	TestTrue(TEXT("and none for other tools"), BuildAutoApprovalLine({Findings[2]}).IsEmpty());

	// The dialog shows it while a bypass is found, and not otherwise.
	FHyperAIStudioMcpBypassMonitor::SetFindingsForTest(TArray<FHyperAIStudioMcpFinding>({Findings[0]}));
	{
		const TSharedRef<SHyperAIStudioAutoApprovalDialog> Dialog = FHyperAIStudioAutoApprovalTestAccess::MakeDialog();
		TestEqual(TEXT("The auto-approval dialog says agents can also use nwiro"), FHyperAIStudioAutoApprovalTestAccess::GetBypassLine(*Dialog).ToString(),
			FString(TEXT("Agents can also use nwiro, which bypasses approvals entirely; auto-approval does not cover it.")));
		HyperAIStudio::Tests::SaveWidgetShot(Dialog, FIntPoint(660, 1000), TEXT("mcp-bypass-auto-approval-dialog"));
		FHyperAIStudioAutoApprovalTestAccess::PressCancel(*Dialog);
	}
	FHyperAIStudioMcpBypassMonitor::SetFindingsForTest(TArray<FHyperAIStudioMcpFinding>({Findings[2]}));
	{
		const TSharedRef<SHyperAIStudioAutoApprovalDialog> Dialog = FHyperAIStudioAutoApprovalTestAccess::MakeDialog();
		TestTrue(TEXT("and nothing without a bypass"), FHyperAIStudioAutoApprovalTestAccess::GetBypassLine(*Dialog).IsEmpty());
		FHyperAIStudioAutoApprovalTestAccess::PressCancel(*Dialog);
	}

	TestEqual(TEXT("Activity's note"), BuildActivityNote(Findings).ToString(),
		FString(TEXT("Not covered here: agents can also change this editor through nwiro and rider, with no HyperAI approval, auto-approval, undo grouping or checks.")));

	// How to remove each place, as text.
	const FString Details = BuildDetailsMarkdown(Findings, {});
	TestTrue(TEXT("Details say how to remove the .mcp.json entry"), Details.Contains(TEXT("Delete the \"nwiro\" entry under \"mcpServers\" in E:/P/.mcp.json"))
		&& Details.Contains(TEXT("claude mcp remove nwiro -s project")));
	TestTrue(TEXT("the Codex table"), Details.Contains(TEXT("Delete the \\[mcp\\_servers.nwiro\\] table")) && Details.Contains(TEXT("codex mcp remove nwiro")));
	TestTrue(TEXT("the plugin"), Details.Contains(TEXT("Disable the Nwiro Integration Kit plugin in Edit > Plugins and restart the editor")));
	TestTrue(TEXT("the user-level Claude entry"), Details.Contains(TEXT("claude mcp remove rider -s user")));
	TestTrue(TEXT("Other tools are listed under their own heading, without removal steps"), Details.Contains(TEXT("## Other tools")) && !Details.Contains(TEXT("remove shadcn")));
	TestTrue(TEXT("and it says HyperAI never edits those files"), Details.Contains(TEXT("never changes them")));
	FHyperAIStudioMcpFinding Written = Findings[0];
	Written.bWrittenByHyperAI = true;
	TestTrue(TEXT("An entry HyperAI writes says to remove it in HyperAI's settings"),
		DescribeRemoval(Written.Sources[0], true).ToString().Contains(TEXT("Settings > MCP servers")));

	const FString Log = BuildLogLine(Findings);
	TestTrue(TEXT("The log line names each bypass and its places: ") + Log, Log.Contains(TEXT("nwiro [Claude Code project .mcp.json entry \"nwiro\" (http://localhost:5354)"))
		&& Log.Contains(TEXT("plugin Nwiro Integration Kit (Plugins/NwiroIK_1.0.3_5.7_Built, port 5353)")) && Log.Contains(TEXT("rider [")) && !Log.Contains(TEXT("shadcn")));
	TestTrue(TEXT("No bypass, no log line"), BuildLogLine({Findings[2]}).IsEmpty());

	// Activity's note and the Agents panel read the monitor.
	FHyperAIStudioMcpBypassMonitor::SetFindingsForTest(Findings);
	const TSharedRef<SHyperAIStudioMcpBypassNote> Note = SNew(SHyperAIStudioMcpBypassNote);
	TestEqual(TEXT("Activity shows the note"), Note->GetText().ToString(), BuildActivityNote(Findings).ToString());
	const TSharedRef<SHyperAIStudioMcpBypassPanel> Panel = SNew(SHyperAIStudioMcpBypassPanel);
	// Pictures for the report, from the sample findings only.
	HyperAIStudio::Tests::SaveWidgetShot(Note, FIntPoint(460, 110), TEXT("mcp-bypass-activity-note"));
	HyperAIStudio::Tests::SaveWidgetShot(Panel, FIntPoint(980, 640), TEXT("mcp-bypass-agents-panel"));
	HyperAIStudio::Tests::SaveWidgetShot(SNew(SHyperAIStudioMcpBypassDetails), FIntPoint(760, 900), TEXT("mcp-bypass-details"));
	FHyperAIStudioMcpBypassMonitor::SetFindingsForTest(TArray<FHyperAIStudioMcpFinding>());
	TestTrue(TEXT("and hides it when there is nothing to say"), Note->GetText().IsEmpty());

	// The capability report carries the same findings for agents.
	FHyperAIStudioMcpBypassMonitor::SetFindingsForTest(Findings);
	const FHyperAICapabilityReport Report = UHyperAIStudioNativeReadToolset::hyper_capability_report(1, {});
	TestEqual(TEXT("The capability report counts the bypasses"), Report.BypassMcpServerCount, 2);
	TestTrue(TEXT("and lists every server with its class and places"), Report.OtherMcpServers.Num() == 3
		&& Report.OtherMcpServers[0].Name == TEXT("nwiro") && Report.OtherMcpServers[0].Classification == TEXT("changes_editor_outside_hyperai")
		&& Report.OtherMcpServers[0].ConfiguredIn.Num() == 3 && Report.OtherMcpServers[0].HowToRemove.Num() == 3
		&& Report.OtherMcpServers[2].Classification == TEXT("other_tools") && Report.OtherMcpServers[2].HowToRemove.IsEmpty());
	TestTrue(TEXT("with a summary: ") + Report.OtherMcpServersSummary, Report.OtherMcpServersSummary.StartsWith(TEXT("nwiro, rider can change this editor outside HyperAI")));
	return true;
}

#endif
