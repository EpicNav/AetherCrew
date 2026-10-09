// Games by Hyper 2026.

#include "HyperAIStudioExecutionQueue.h"

#include "Containers/Ticker.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioSandbox.h"
#include "HyperAIStudioSourceControl.h"
#include "HyperAIStudioTrustedExecutionInternal.h"
#include "Misc/PackageName.h"

namespace HyperAIStudio::ExecutionQueue::Private
{
	struct FQueuedEdit
	{
		FHyperAIStudioTrustedPreparedArtifact Prepared;
		FString OperationId;
		FHyperAIStudioTrustedArtifactRequest Request;
		/** Approved by the user: a click, or the auto-approval they turned on for its category. */
		bool bUserApproved = false;
		double QueuedSeconds = 0.0;
	};

	struct FFailure
	{
		FString OperationId;
		FString Status;
		FString Diagnostic;
	};

	constexpr int32 MaxRememberedFailures = 32;
	constexpr float DrainSeconds = 0.1f;

	TArray<FQueuedEdit>& Queue()
	{
		static TArray<FQueuedEdit> Items;
		return Items;
	}

	TArray<FFailure>& Failures()
	{
		static TArray<FFailure> Items;
		return Items;
	}

	FTSTicker::FDelegateHandle& DrainHandle()
	{
		static FTSTicker::FDelegateHandle Handle;
		return Handle;
	}

	TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe>& HostOverride()
	{
		static TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host;
		return Host;
	}

	TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> CurrentHost()
	{
		return HostOverride().IsValid() ? HostOverride() : HyperAIStudio::TrustedExecution::Private::GetCoreHost();
	}

	void RecordActivity(const FQueuedEdit& Item, const EHyperAIStudioActivityKind Kind, const FString& Status, const FString& Detail)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Kind;
		Entry.PackId = Item.Request.PackId;
		Entry.ToolName = Item.Request.ToolName;
		Entry.Target = Item.Request.EffectTarget;
		Entry.OperationId = Item.OperationId;
		Entry.StatusCode = Status;
		Entry.Detail = Detail;
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	/** Remembered so hyper_operation_status can say why an edit that nobody was waiting on never ran. */
	void Remember(const FString& OperationId, const FString& Status, const FString& Diagnostic)
	{
		TArray<FFailure>& Items = Failures();
		Items.RemoveAll([&OperationId](const FFailure& Failure) { return Failure.OperationId == OperationId; });
		if (Items.Num() >= MaxRememberedFailures)
		{
			Items.RemoveAt(0);
		}
		Items.Add({OperationId, Status, Diagnostic});
	}

	void Fail(const FQueuedEdit& Item, const FString& Status, const FString& Diagnostic)
	{
		Remember(Item.OperationId, Status, Diagnostic);
		RecordActivity(Item, EHyperAIStudioActivityKind::Failed, Status, Diagnostic);
	}

	/** Stages and submits on a free lane. A plan that waited past its seal is prepared again from the same inputs. */
	bool Start(FHyperAIStudioTrustedExecutionHost& Host, const FQueuedEdit& Item, FString& OutStatus, FString& OutError)
	{
		// Checked as the edit starts, not when it queued: the sandbox that will hold its save must be open now.
		if (!HyperAIStudio::Sandbox::CheckAgentEdit(OutError))
		{
			OutStatus = TEXT("sandbox_unavailable");
			return false;
		}
		// Checked out (or locked in Lore) before anything changes; one a teammate holds turns the edit away.
		if (!FHyperAIStudioSourceControl::MakeEditable({FPackageName::ObjectPathToPackageName(Item.Request.EffectTarget)}, OutError))
		{
			OutStatus = TEXT("source_control_blocked");
			return false;
		}
		FHyperAIStudioTypedArtifactStageReceipt Receipt;
		FHyperAIStudioTrustedExecutionDiagnostic Status;
		bool bStaged = Host.StageExact(Item.Prepared, Item.OperationId, Receipt, Status, OutError);
		if (!bStaged && (Status.StatusCode == TEXT("prerequisite_snapshot_stale")
			|| Status.StatusCode == TEXT("trusted_preparation_expired")))
		{
			FHyperAIStudioTrustedPreparedArtifact Fresh;
			bStaged = Host.Reprepare(Item.Prepared, Fresh, Status, OutError)
				&& Host.StageExact(Fresh, Item.OperationId, Receipt, Status, OutError);
		}
		if (!bStaged)
		{
			OutStatus = Status.StatusCode.IsEmpty() ? FString(TEXT("stage_failed")) : Status.StatusCode;
			OutError = OutError.IsEmpty() ? Status.Diagnostic : OutError;
			return false;
		}
		// Edit is tokenless. A risky plan's grant is issued only for a plan the user approved, now that it runs, by the
		// host that staged it (in the editor that is always the core host).
		FString AuthorizationToken;
		if (Item.bUserApproved && Item.Request.Safety != EHyperAIStudioDomainSafety::Edit
			&& !Host.IssueAuthorizationFromCoreUiApproval(Receipt, AuthorizationToken, OutError))
		{
			OutStatus = TEXT("authorization_not_issued");
			return false;
		}
		FHyperAIStudioTypedArtifactSubmissionReceipt Submission;
		if (!Host.SubmitExact(Receipt, AuthorizationToken, Submission, Status, OutError))
		{
			OutStatus = Status.StatusCode.IsEmpty() ? FString(TEXT("submit_failed")) : Status.StatusCode;
			OutError = OutError.IsEmpty() ? Status.Diagnostic : OutError;
			return false;
		}
		OutStatus = TEXT("submitted");
		return true;
	}

	bool Drain(float)
	{
		TArray<FQueuedEdit>& Items = Queue();
		const double Now = FPlatformTime::Seconds();
		for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
		{
			if (Now - Items[Index].QueuedSeconds > FHyperAIStudioExecutionQueue::MaxWaitSeconds)
			{
				const FQueuedEdit Item = MoveTemp(Items[Index]);
				Items.RemoveAt(Index);
				Fail(Item, TEXT("queue_timeout"), TEXT("The edit waited ten minutes for the edits ahead of it and was dropped; nothing ran."));
			}
		}
		const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host = CurrentHost();
		if (!Items.IsEmpty() && Host.IsValid() && !Host->IsLaneBusy())
		{
			const FQueuedEdit Item = MoveTemp(Items[0]);
			Items.RemoveAt(0);
			FString Status;
			FString Error;
			if (!Start(*Host, Item, Status, Error))
			{
				Fail(Item, Status, Error);
			}
		}
		if (Items.IsEmpty())
		{
			DrainHandle().Reset();
			return false;
		}
		return true;
	}

	bool Enqueue(
		const FHyperAIStudioTrustedPreparedArtifact& Prepared,
		const FString& OperationId,
		const bool bUserApproved,
		FString& OutStatus,
		FString& OutError)
	{
		OutStatus.Reset();
		OutError.Reset();
		if (!IsInGameThread())
		{
			OutStatus = TEXT("game_thread_required");
			OutError = TEXT("Edits are submitted on the game thread.");
			return false;
		}
		const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host = CurrentHost();
		FQueuedEdit Item;
		if (!Host.IsValid() || !Host->DescribePrepared(Prepared, Item.Request))
		{
			OutStatus = TEXT("host_unavailable");
			OutError = TEXT("The trusted execution host is unavailable, or the plan was not prepared by it.");
			return false;
		}
		TArray<FQueuedEdit>& Items = Queue();
		if (Items.ContainsByPredicate([&OperationId](const FQueuedEdit& Queued) { return Queued.OperationId == OperationId; }))
		{
			OutStatus = TEXT("queued");
			return true;
		}
		Failures().RemoveAll([&OperationId](const FFailure& Failure) { return Failure.OperationId == OperationId; });
		Item.Prepared = Prepared;
		Item.OperationId = OperationId;
		Item.bUserApproved = bUserApproved;
		Item.QueuedSeconds = FPlatformTime::Seconds();
		const bool bLaneBusy = Host->IsLaneBusy();
		if (Items.IsEmpty() && !bLaneBusy)
		{
			const bool bStarted = Start(*Host, Item, OutStatus, OutError);
			if (!bStarted && bUserApproved)
			{
				// The approval gate logs the failure; the agent waiting on it learns it from the operation status.
				Remember(OperationId, OutStatus, OutError);
			}
			return bStarted;
		}
		if (Items.Num() >= FHyperAIStudioExecutionQueue::MaxQueued)
		{
			OutStatus = TEXT("queue_full");
			OutError = FString::Printf(TEXT("%d edits are already waiting for the project; retry when some finish."), Items.Num());
			return false;
		}
		RecordActivity(Item, EHyperAIStudioActivityKind::Queued, TEXT("queued"),
			FString::Printf(TEXT("Waiting for %d edit(s) ahead of it."), Items.Num() + (bLaneBusy ? 1 : 0)));
		Items.Add(MoveTemp(Item));
		if (!DrainHandle().IsValid())
		{
			DrainHandle() = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Drain), DrainSeconds);
		}
		OutStatus = TEXT("queued");
		return true;
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void HyperAIStudio::TrustedExecution::Private::SetQueueHostForTests(
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe>& Host)
{
	HyperAIStudio::ExecutionQueue::Private::HostOverride() = Host;
}
#endif

bool HyperAIStudio::TrustedExecution::Private::DescribeQueuedSafety(
	const FHyperAIStudioTrustedPreparedArtifact& Prepared,
	EHyperAIStudioDomainSafety& OutSafety)
{
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host = HyperAIStudio::ExecutionQueue::Private::CurrentHost();
	FHyperAIStudioTrustedArtifactRequest Request;
	if (!IsInGameThread() || !Host.IsValid() || !Host->DescribePrepared(Prepared, Request))
	{
		return false;
	}
	OutSafety = Request.Safety;
	return true;
}

bool HyperAIStudio::TrustedExecution::Private::SubmitUserApproved(
	const FHyperAIStudioTrustedPreparedArtifact& Prepared,
	const FString& OperationId,
	FString& OutStatus,
	FString& OutError)
{
	return HyperAIStudio::ExecutionQueue::Private::Enqueue(Prepared, OperationId, true, OutStatus, OutError);
}

bool FHyperAIStudioExecutionQueue::Submit(
	const FHyperAIStudioTrustedPreparedArtifact& Prepared,
	const FString& OperationId,
	FString& OutStatus,
	FString& OutError)
{
	return HyperAIStudio::ExecutionQueue::Private::Enqueue(Prepared, OperationId, false, OutStatus, OutError);
}

bool FHyperAIStudioExecutionQueue::Query(const FString& OperationId, int32& OutPosition, FString& OutFailure, FString& OutDiagnostic)
{
	using namespace HyperAIStudio::ExecutionQueue::Private;
	OutPosition = 0;
	OutFailure.Reset();
	OutDiagnostic.Reset();
	const int32 Index = Queue().IndexOfByPredicate([&OperationId](const FQueuedEdit& Item) { return Item.OperationId == OperationId; });
	if (Index != INDEX_NONE)
	{
		OutPosition = Index + 1;
		return true;
	}
	for (const FFailure& Failure : Failures())
	{
		if (Failure.OperationId == OperationId)
		{
			OutFailure = Failure.Status;
			OutDiagnostic = Failure.Diagnostic;
			return true;
		}
	}
	return false;
}

int32 FHyperAIStudioExecutionQueue::Num()
{
	return HyperAIStudio::ExecutionQueue::Private::Queue().Num();
}

bool FHyperAIStudioExecutionQueue::IsRunning()
{
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host = HyperAIStudio::ExecutionQueue::Private::CurrentHost();
	return Host.IsValid() && Host->IsLaneBusy();
}

void FHyperAIStudioExecutionQueue::Clear()
{
	using namespace HyperAIStudio::ExecutionQueue::Private;
	if (DrainHandle().IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DrainHandle());
		DrainHandle().Reset();
	}
	// Held plans keep their pack's objects alive, so they go before the packs unload.
	Queue().Reset();
	Failures().Reset();
}
