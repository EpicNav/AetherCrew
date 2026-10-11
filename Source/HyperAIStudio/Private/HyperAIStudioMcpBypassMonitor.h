// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioMcpBypass.h"

/**
 * The editor side of HyperAIStudioMcpBypass.h: reads this machine's agent configs and this editor's plugins and settings,
 * keeps the findings, and warns.
 *
 * - At editor start, and whenever a new bypass (or a new place for one) appears, a notification names it, with
 *   "Show details" and "Don't warn about <name> again" (a per-project choice kept in the user's
 *   EditorPerProjectUserSettings.ini, which only silences the warning).
 * - Activity, Settings > Setup and Settings > Agents, the auto-approval dialog, the capability report and AGENTS.md read
 *   the findings from here; each change is logged.
 *
 * It checks again every 30 seconds, rereading only when a config file, Python Remote Execution or this editor's
 * listening ports changed. Never writes the agents' config files. Game thread only.
 */
class FHyperAIStudioMcpBypassMonitor
{
public:
	static void Startup();
	static void Shutdown();

	/** The latest findings, silenced ones marked; the first call reads the machine if nothing has yet. */
	static TArray<FHyperAIStudioMcpFinding> GetFindings();
	/** Config files that could not be read on the last check (names and reasons, never contents). */
	static TArray<FString> GetProblems();
	/** Reads everything again now; true when the findings changed. */
	static bool Rescan();
	/** Broadcast when the findings change, or a warning is silenced or restored. */
	static FSimpleMulticastDelegate& OnChanged();

	/** "Don't warn about <name> again" (true) or "Warn again" (false), for this project and this user. */
	static void SetWarningSilenced(const FString& Id, bool bSilenced);
	/** The ids silenced in this project, from ConfigFile (the editor's per-project user settings by default). */
	static TSet<FString> ReadSilenced(const FString& ConfigFile);
	static void WriteSilenced(const FString& ConfigFile, const TSet<FString>& Ids);

	/** The window listing every finding, where it is configured and how to remove it. */
	static void OpenDetails();
	/** Shows the notification when an unsilenced bypass, or a place for one, has not been warned about this session. */
	static void ShowNotificationIfNew();

	/** Where this machine's agent configs are. */
	static FHyperAIStudioMcpScanPaths GetScanPaths();
	/** This editor's port, plugins, Python Remote Execution and listening ports, and HyperAI's extra servers. */
	static FHyperAIStudioMcpEditorFacts GatherEditorFacts();
	/** TCP ports this editor process listens on (Windows; empty elsewhere). Asks the OS, connects to nothing. */
	static TArray<int32> GetListeningPorts();

#if WITH_DEV_AUTOMATION_TESTS
	/** Replaces the machine's findings with these until reset (unset), so tests never read the user's files. */
	static void SetFindingsForTest(const TOptional<TArray<FHyperAIStudioMcpFinding>>& Findings);
	/** Keeps the "Don't warn again" choices in this file instead of the editor's; empty restores it. */
	static void SetConfigFileForTest(const FString& ConfigFile);
	/** What the notification would say now, and whether it would show (a key not warned about yet). */
	static FText GetPendingNotificationForTest(bool& bOutWouldShow);
	/** Forgets what was warned about this session. */
	static void ResetNotifiedForTest();
	/** Where "Don't warn again" is kept now. */
	static FString GetConfigFileForTest();
#endif
};
