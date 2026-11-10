// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * Sandbox Mode: while it is on, everything saved in the editor lands in a UE File Sandbox HyperAI opened, not in the
 * project, until the user approves or discards all of it. Agents cannot approve or discard. Game thread only.
 */
namespace HyperAIStudio::Sandbox
{
	enum class EState : uint8
	{
		/** Sandbox Mode is off. */
		Off,
		/** The File Sandbox plugin is not loaded. */
		Unavailable,
		/** Waiting to open, for example until Play In Editor stops, or for a decision about an earlier sandbox. */
		Waiting,
		/** HyperAI's sandbox holds what is saved. */
		Active,
		/** Someone else's sandbox is active; HyperAI never takes it over, so agent edits are refused. */
		Foreign
	};

	struct FChange
	{
		/** The project file, as it will be written when approved. */
		FString Path;
		/** added, removed or edited. */
		FString Kind;
	};

	/** What source control did for one held file as it was last approved (FHyperAIStudioSourceControl::WriteHeldFiles). */
	struct FFileOutcome
	{
		/** Project-relative, as FChange::Path. */
		FString Path;
		FString Kind;
		/** checked_out, marked_for_add, marked_for_delete, locked, marked_dirty, written, blocked or failed. */
		FString Outcome;
		/** Who holds a blocked file. */
		FString Who;
		FString Detail;
		bool bWritten = false;
	};

	struct FStatus
	{
		EState State = EState::Off;
		FString SandboxName;
		FString Diagnostic;
		TArray<FChange> Changes;
	};

	/** Whether Approve All could write what HyperAI's sandbox holds right now; finding out saves and changes nothing. */
	struct FReadiness
	{
		/** HyperAI's sandbox is active and holds files. */
		bool bHolding = false;
		FString SandboxName;
		/** What it holds, as "kind path", as Activity lists it. */
		TArray<FString> Files;
		/** Some held files remove the project's own files when written. */
		bool bRemovals = false;
		/** Empty when Approve All would go ahead now; otherwise what it waits for, in the log's English. */
		FString WaitsFor;
	};

	HYPERAISTUDIO_API void Startup();
	HYPERAISTUDIO_API void Shutdown();
	/** Applies the Sandbox Mode setting after it changed. */
	HYPERAISTUDIO_API void OnModeChanged();
	HYPERAISTUDIO_API FStatus GetStatus(bool bWithChanges = true);
	/** True while HyperAI's sandbox holds saved edits for review. */
	HYPERAISTUDIO_API bool IsHoldingEdits();
	/** How many files HyperAI's sandbox holds, counted when they change, so it is cheap to show every frame. */
	HYPERAISTUDIO_API int32 GetHeldCount();
	/** Before an agent edit runs: true when Sandbox Mode is off or its sandbox is ready; false with the reason otherwise. */
	HYPERAISTUDIO_API bool CheckAgentEdit(FString& OutError);
	/**
	 * The user's decision: saves what agents left unsaved, writes every held file to the project, and starts afresh.
	 * Refused while other assets are unsaved: closing a sandbox reloads every unsaved asset from disk.
	 */
	HYPERAISTUDIO_API bool ApproveAll(FString& OutError);
	/**
	 * The user's decision for some held files (project-relative, as FChange::Path): each is checked out (or locked, in
	 * Lore on a server) and written to the project; new files are marked for add. A file a teammate holds stays held,
	 * naming them, and the rest go ahead. The sandbox stays open with whatever is left. Approve All writes every held file
	 * this way, and closes the sandbox only once all of them went. False with OutError when any stayed held.
	 */
	HYPERAISTUDIO_API bool ApproveFiles(const TArray<FString>& Paths, FString& OutError);
	/** What source control did for each file at the last approve, for the sandbox card. */
	HYPERAISTUDIO_API const TArray<FFileOutcome>& GetLastOutcomes();
	/**
	 * For the auto-approval the user turned on for Sandbox Mode's held saves: whether Approve All would go ahead now,
	 * without its saving step. It waits while any asset is unsaved, rather than saving it, and while Play In Editor or an
	 * agent edit runs.
	 */
	HYPERAISTUDIO_API FReadiness CheckApproveAll();
	/** The user's decision: throws away every held file and reloads what was open from the project, unsaved assets included. */
	HYPERAISTUDIO_API bool DiscardAll(FString& OutError);
	HYPERAISTUDIO_API FSimpleMulticastDelegate& OnChanged();
#if WITH_DEV_AUTOMATION_TESTS
	/** Overrides the setting for tests; unset restores it. */
	HYPERAISTUDIO_API void SetModeForTests(TOptional<bool> bMode);
#endif
}
