// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

/** Where an edit stands when the tool that submitted it replies. */
struct FHyperAIStudioEditOutcome
{
	/** False once the edit failed, was rolled back or rejected, or cannot be found; true while it still runs. */
	bool bOk = false;
	/** The operation's status: completed, failed, queued, running, awaiting_user_approval, ... */
	FString Status;
	/** What happened, then what to do next. */
	FString Diagnostic;
};

/**
 * An apply tool waits for the edit it submitted and replies with how it ended, so an edit costs the agent one call
 * instead of an apply and a hyper_operation_wait. The wait holds the agent's request open while the editor keeps
 * ticking, exactly as hyper_operation_wait does, and gives up after EditWaitMs with the edit still running.
 */
namespace HyperAIStudio::OperationWait
{
	constexpr int32 EditWaitMs = 60000;

	/** Calls Done on the game thread once OperationId finishes, waits for approval, or TimeoutMs passes. */
	HYPERAISTUDIO_API void WhenDone(const FString& OperationId, int32 TimeoutMs, TFunction<void(const FHyperAIStudioEditOutcome&)>&& Done);

	/**
	 * A TResult (a UToolCallAsyncResult with SetValue(TReport)) that completes with Report once the edit Report
	 * submitted, under Report.OperationId, has finished, its status and diagnostic replaced by the outcome. A dry run,
	 * a refusal or an edit waiting for approval completes at once with Report as it is.
	 */
	template <typename TResult, typename TReport>
	TResult* ReplyWhenDone(TReport Report)
	{
		TResult* Result = NewObject<TResult>();
		if (!Report.bOk || !(Report.Status == TEXT("submitted") || Report.Status == TEXT("queued")))
		{
			Result->SetValue(Report);
			return Result;
		}
		const FString OperationId = Report.OperationId;
		WhenDone(OperationId, EditWaitMs, [Keep = TStrongObjectPtr<TResult>(Result), Report](const FHyperAIStudioEditOutcome& Outcome) mutable
		{
			Report.bOk = Outcome.bOk;
			Report.Status = Outcome.Status;
			Report.Diagnostic = Outcome.Diagnostic;
			Keep->SetValue(Report);
		});
		return Result;
	}
}
