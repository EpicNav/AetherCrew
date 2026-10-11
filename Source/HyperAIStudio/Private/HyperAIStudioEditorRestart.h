// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioEditorRestart.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIEditorRestartRequest
{
	GENERATED_BODY()

	/** Build the editor (the project's C++ and plugins) while it is closed. Off only closes and reopens it. */
	UPROPERTY() bool bBuild = true;
	/** Save every changed level and asset first, without asking. Off leaves the editor's own save prompt to the user. */
	UPROPERTY() bool bSaveDirty = true;
	/** For you after the restart: what you were doing and what comes next. It comes back in the message that resumes your chat. */
	UPROPERTY() FString Note;
	/** Only report what would happen: what is saved and built, the command lines, and which chats reopen. */
	UPROPERTY() bool bDryRun = false;
};

USTRUCT(BlueprintType)
struct FHyperAIEditorRestartReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** restarting, checked (a dry run) or refused. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Message;
	/** Levels and assets with unsaved changes, saved first unless bSaveDirty is off. */
	UPROPERTY() TArray<FString> DirtyPackages;
	/** The chat tabs that reopen, and which of them is told to continue. */
	UPROPERTY() TArray<FString> Chats;
	/** The build that runs while the editor is closed; empty when bBuild is off. */
	UPROPERTY() FString BuildCommand;
	/** Where the build's output goes, relative to the project. */
	UPROPERTY() FString BuildLog;
	/** How the project opens again. */
	UPROPERTY() FString RelaunchCommand;
};

/** Restarts the editor and gives its chats back. */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioEditorRestartToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Restarts the Unreal Editor, for example after you change C++ or a plugin setting. It saves changed levels and
	 * assets, closes the editor, builds it (bBuild), opens the same project again and reopens the HyperAI chat tabs,
	 * each on its own conversation. The chat that was working when you called this, yours, is then told that the
	 * editor restarted, with the build result, any errors and your note. The editor closes about two seconds after
	 * this returns and your turn ends there, so reply in a line that the restart is underway. Called from outside the
	 * editor, wait for it with .hyperai/scripts/Wait-HyperAIStudioMCP.ps1 -TimeoutSeconds 1800 instead.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Editor")
	static FHyperAIEditorRestartReport hyper_editor_restart(const FHyperAIEditorRestartRequest& Request);
};

/** One chat tab a restart reopens. */
struct FHyperAIStudioRestartChat
{
	int32 TabIndex = 1;
	FString Agent;
	FString SessionId;
	FString Model;
	FString Subagent;
	FString Title;
	/** A Claude Code chat running in the background outlives the editor; its tab attaches to it again. */
	FString BackgroundId;
	/** Its turn was running when the restart began. MCP calls carry no caller, so this is the chat that asked. */
	bool bWorking = false;
	/** After the restart: what this chat is told first. Empty for a chat that did not ask. */
	FString Message;
};

/**
 * Restart and resume. The editor saves, writes the plan under Saved/HyperAIStudio/Restart, hands the build and the
 * relaunch to a helper script that outlives it, and closes. The editor the helper opens (with -HyperAIResume=<token>)
 * gives each chat tab its conversation back. After a crash the same happens: a watchdog started with the editor finds
 * the crash report and hands the helper a plan the editor kept current under Saved/HyperAIStudio/Recovery. Game thread only.
 */
class HYPERAISTUDIO_API FHyperAIStudioEditorRestart
{
public:
	struct FOptions
	{
		bool bBuild = true;
		bool bSaveDirty = true;
		bool bDryRun = false;
		FString Note;
		/** A tab to tell to continue even when its turn is not running; 0 for none. */
		int32 NoteTab = 0;
	};

	static FHyperAIEditorRestartReport Restart(const FOptions& Options);
	/** True from the moment a restart starts closing the editor: the close asks nothing more. */
	static bool IsRestarting();
	/** The chat a restart left for this tab, handed over once. */
	static TOptional<FHyperAIStudioRestartChat> TakeChat(int32 TabIndex);

	/** Where a restart's close stands, judged each frame once the editor was asked to close. */
	enum class ECloseState : uint8
	{
		/** The close has not run yet, or the editor put it back for later (it was saving or loading). */
		Waiting,
		/** The editor shuts down: the helper builds and opens it again once it is gone. */
		Closing,
		/** The close ran and the editor stayed open without asking anything: ask it again. */
		TryAgain,
		/** The editor stayed open: a question before closing was answered No or Cancel, or it kept refusing. */
		CalledOff,
	};
	/**
	 * Judged from what happened, never from how long it took: the editor's questions (save your work, close anyway)
	 * wait for as long as the person answering takes, and a close that goes ahead only ends the editor a frame later.
	 */
	static ECloseState JudgeClose(bool bShutDown, bool bCloseQueued, bool bAsked, int32 Tries);

	/** An editor opened by hand long after a restart that closed it and never opened it again: its chats wait to be resumed. */
	static bool HasResumeOffer();
	/** Resumes the offered restart's chats: open tabs pick them up at once and the others open. False when there was none. */
	static bool AcceptResumeOffer();
	/** Drops the offered restart: its chats stay as they are. */
	static void DeclineResumeOffer();

	/** Module lifetime: the restart tool, the console command, and resuming after a restart. */
	static void Startup();
	static void Shutdown();

	/** What the chat that asked is told: from the plan the editor wrote and the result the helper wrote. */
	static FString DescribeRestart(const TSharedPtr<FJsonObject>& Plan, const TSharedPtr<FJsonObject>& Result);
	/** The same, for a terminal chat: one line a command line carries safely, pointing at continue.md. */
	static FString ContinuePrompt();
	/**
	 * Takes a restart's chats from Folder when both of its files carry Token and it is fresh; the files are used up.
	 * Without the token (the editor opened by hand, or by the crash reporter) it takes a fresh crash's chats, and a
	 * restart's whose editor really closed but never opened again: by itself when that was recent, and otherwise, up
	 * to MaxAgeSeconds, as an offer (HasResumeOffer) the person accepts or declines.
	 */
	static bool LoadResume(const FString& Folder, const FString& Token, double MaxAgeSeconds);
	static FString GetFolder();
};
