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

	struct FStatus
	{
		EState State = EState::Off;
		FString SandboxName;
		FString Diagnostic;
		TArray<FChange> Changes;
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
	/** The user's decision: throws away every held file and reloads what was open from the project, unsaved assets included. */
	HYPERAISTUDIO_API bool DiscardAll(FString& OutError);
	HYPERAISTUDIO_API FSimpleMulticastDelegate& OnChanged();
#if WITH_DEV_AUTOMATION_TESTS
	/** Overrides the setting for tests; unset restores it. */
	HYPERAISTUDIO_API void SetModeForTests(TOptional<bool> bMode);
#endif
}
