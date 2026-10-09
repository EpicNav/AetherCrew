// Games by Hyper 2026.

#include "HyperAIStudioApprovalGate.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGateInternal.h"
#include "HyperAIStudioAutoApproval.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioTrustedExecutionInternal.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioApproval, Log, All);

const TCHAR* LexToString(const EHyperAIStudioApprovalCategory Category)
{
	switch (Category)
	{
	case EHyperAIStudioApprovalCategory::Scripts: return TEXT("scripts");
	case EHyperAIStudioApprovalCategory::Destructive: return TEXT("destructive");
	case EHyperAIStudioApprovalCategory::ExternalEffects: return TEXT("external_effects");
	default: return TEXT("edits");
	}
}

namespace HyperAIStudio::Approval::Private
{
	struct FHeldApproval
	{
		FHyperAIStudioPendingApproval Pending;
		FHyperAIStudioTrustedPreparedArtifact Prepared;
		/** Set instead of Prepared for work outside the typed executor. */
		TFunction<void()> Action;
		/** Order of arrival: an auto-approval covers only requests from the one current when it started. */
		uint64 Sequence = 0;
		/** Auto-approval covered it as it arrived; the sweep approves it on the next quiet tick. */
		bool bScheduled = false;
	};

	/** A typed plan an auto-approval approved that went into the edit queue behind other edits. */
	struct FAwaitingLane
	{
		FHyperAIStudioPendingApproval Pending;
		uint64 Sequence = 0;
		uint64 Activation = 0;
	};

	constexpr int32 MaxRecords = 64;

	TArray<FHeldApproval>& Held()
	{
		static TArray<FHeldApproval> Storage;
		return Storage;
	}

	/** Remembered until the auto-approval that approved them ends, so whatever has not started then can go back. */
	TArray<FAwaitingLane>& AwaitingLane()
	{
		static TArray<FAwaitingLane> Storage;
		return Storage;
	}

	uint64& NextSequence()
	{
		static uint64 Value = 1;
		return Value;
	}

	TArray<FHyperAIStudioApprovalRecord>& Records()
	{
		static TArray<FHyperAIStudioApprovalRecord> Storage;
		return Storage;
	}

	FTSTicker::FDelegateHandle& SweepHandle()
	{
		static FTSTicker::FDelegateHandle Handle;
		return Handle;
	}

	int32 IndexOf(const FString& OperationId)
	{
		return Held().IndexOfByPredicate([&OperationId](const FHeldApproval& Item)
		{
			return Item.Pending.OperationId == OperationId;
		});
	}

	FHyperAIStudioActivityEntry MakeEntry(
		const EHyperAIStudioActivityKind Kind,
		const FHyperAIStudioApprovalSummary& Summary,
		const FString& OperationId)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Kind;
		Entry.PackId = Summary.PackId;
		Entry.ToolName = Summary.ToolName;
		Entry.Target = Summary.EffectTarget;
		Entry.OperationId = OperationId;
		return Entry;
	}

	void ForgetApproval(const FString& OperationId)
	{
		Records().RemoveAll([&OperationId](const FHyperAIStudioApprovalRecord& Record) { return Record.OperationId == OperationId; });
	}

	void RecordApproval(const FString& OperationId, const EHyperAIStudioApprover Approver, const TOptional<EHyperAIStudioApprovalCategory>& AutoCategory)
	{
		TArray<FHyperAIStudioApprovalRecord>& Items = Records();
		ForgetApproval(OperationId);
		if (Items.Num() >= MaxRecords)
		{
			Items.RemoveAt(0);
		}
		FHyperAIStudioApprovalRecord& Record = Items.AddDefaulted_GetRef();
		Record.OperationId = OperationId;
		Record.ApprovedBy = Approver;
		Record.AutoApprovedAs = AutoCategory;
		Record.ApprovedUtc = FDateTime::UtcNow();
	}

	bool IsCovered(const FHeldApproval& Item)
	{
		return Item.Pending.Category.IsSet()
			&& FHyperAIStudioAutoApproval::Covers(*Item.Pending.Category, Item.Sequence, Item.Pending.OperationId);
	}

	/**
	 * A click never lands mid-drag or mid-save, but a ticker can: an approved edit would nest its transaction in the
	 * user's open one, and a failed edit that undoes its transaction could undo the user's work. Such ticks wait.
	 */
	bool IsEditorBusy()
	{
		return (GEditor && GEditor->IsTransactionActive())
			|| (FSlateApplication::IsInitialized() && FSlateApplication::Get().HasAnyMouseCaptor())
			|| UE::IsSavingPackage()
			|| IsGarbageCollecting();
	}

	uint64 PeekNextSequence()
	{
		return NextSequence();
	}

	TOptional<EHyperAIStudioApprovalCategory> Categorize(
		const FHyperAIStudioApprovalSummary& Summary,
		const TOptional<EHyperAIStudioDomainSafety> SealedSafety)
	{
		using ESafety = EHyperAIStudioDomainSafety;
		// Escalated scripts run without the undo step and the guard, whatever safety their summary carries.
		if (Summary.bEscalatedScript || (Summary.PackId == TEXT("scripting") && Summary.Safety != ESafety::Edit))
		{
			return EHyperAIStudioApprovalCategory::Scripts;
		}
		const ESafety Declared = Summary.Safety;
		const ESafety Sealed = SealedSafety.Get(Declared);
		if (Declared == ESafety::Read || Sealed == ESafety::Read)
		{
			return {};
		}
		const bool bDestructive = Declared == ESafety::Destructive || Sealed == ESafety::Destructive;
		const bool bExternal = Declared == ESafety::ExternalEffect || Sealed == ESafety::ExternalEffect;
		if (bDestructive && bExternal)
		{
			return {};
		}
		return bDestructive ? EHyperAIStudioApprovalCategory::Destructive
			: bExternal ? EHyperAIStudioApprovalCategory::ExternalEffects
			: EHyperAIStudioApprovalCategory::Edits;
	}

	bool Sweep(float);

	void ScheduleSweep()
	{
		if (!SweepHandle().IsValid())
		{
			SweepHandle() = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Sweep), 0.0f);
		}
	}

	/** Holds a request; one the user's auto-approval covers is approved on the next quiet tick. */
	void Hold(FHeldApproval&& Item)
	{
		FHyperAIStudioAutoApproval::EndIfOver();
		Item.Sequence = NextSequence()++;
		Item.Pending.RequestedUtc = FDateTime::UtcNow();
		Item.bScheduled = IsCovered(Item);
		const bool bScheduled = Item.bScheduled;
		FHyperAIStudioActivityEntry Entry = MakeEntry(EHyperAIStudioActivityKind::ApprovalRequested, Item.Pending.Summary, Item.Pending.OperationId);
		Entry.StatusCode = TEXT("awaiting_user_approval");
		Entry.Detail = Item.Pending.Summary.Effects.Num() > 0 ? Item.Pending.Summary.Effects[0] : FString();
		Held().Add(MoveTemp(Item));
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
		if (bScheduled)
		{
			ScheduleSweep();
		}
		FHyperAIStudioApprovalGate::OnChanged().Broadcast();
	}

	/**
	 * A click, an auto-approval and a running job all approve here; only the activity entry, the log and the record say
	 * which. AutoCategory is set exactly when Approver is AutoApproval.
	 */
	bool ApproveHeld(
		const FString& OperationId,
		const EHyperAIStudioApprover Approver,
		const TOptional<EHyperAIStudioApprovalCategory>& AutoCategory,
		FString& OutError)
	{
		OutError.Reset();
		if (!IsInGameThread())
		{
			OutError = TEXT("Approvals are granted on the game thread.");
			return false;
		}
		const int32 Index = IndexOf(OperationId);
		if (Index == INDEX_NONE)
		{
			OutError = TEXT("That plan is no longer waiting for approval.");
			return false;
		}
		const FHeldApproval Item = Held()[Index];
		Held().RemoveAt(Index);
		RecordApproval(OperationId, Approver, AutoCategory);
		const FString Approved = AutoCategory.IsSet() ? FString::Printf(TEXT("Auto-approved as %s; "), ::LexToString(*AutoCategory))
			: Approver == EHyperAIStudioApprover::AgentJob ? FString(TEXT("Approved for a running agent job; "))
			: FString(TEXT("Approved; "));
		auto LogApproval = [&Item, &OperationId, &AutoCategory, Approver]()
		{
			if (AutoCategory.IsSet())
			{
				UE_LOG(LogHyperAIStudioApproval, Display, TEXT("Auto-approved %s on %s (%s) as %s."),
					*Item.Pending.Summary.ToolName, *Item.Pending.Summary.EffectTarget, *OperationId, ::LexToString(*AutoCategory));
			}
			else
			{
				UE_LOG(LogHyperAIStudioApproval, Log, TEXT("Approved %s on %s (%s)%s."),
					*Item.Pending.Summary.ToolName, *Item.Pending.Summary.EffectTarget, *OperationId,
					Approver == EHyperAIStudioApprover::AgentJob ? TEXT(" for a running agent job") : TEXT(""));
			}
		};
		auto Mark = [&AutoCategory, Approver](FHyperAIStudioActivityEntry& Entry)
		{
			Entry.AutoApprovedAs = AutoCategory;
			Entry.bApprovedForJob = Approver == EHyperAIStudioApprover::AgentJob;
		};
		if (Item.Action)
		{
			FHyperAIStudioActivityEntry Entry = MakeEntry(EHyperAIStudioActivityKind::ApprovalApproved, Item.Pending.Summary, OperationId);
			Entry.StatusCode = TEXT("approved");
			Entry.Detail = Approved + TEXT("running.");
			Mark(Entry);
			FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
			LogApproval();
			FHyperAIStudioApprovalGate::OnChanged().Broadcast();
			Item.Action();
			return true;
		}

		// A plan that cannot start still says who approved it, so a failure nobody clicked for reads as such.
		auto Fail = [&Item, &OperationId, &Approved, &Mark, Approver](const FString& Status, const FString& Diagnostic)
		{
			FHyperAIStudioActivityEntry Entry = MakeEntry(EHyperAIStudioActivityKind::Failed, Item.Pending.Summary, OperationId);
			Entry.StatusCode = Status;
			Entry.Detail = Approver == EHyperAIStudioApprover::User ? Diagnostic : Approved + TEXT("it could not start: ") + Diagnostic;
			Mark(Entry);
			FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
			FHyperAIStudioApprovalGate::OnChanged().Broadcast();
		};

		// Staging happens when the plan reaches the lane, not when the agent asked, so the stage lifetime covers only
		// the run. A plan the user took longer than its seal to approve is prepared again from the same inputs, and a
		// risky plan's grant is issued then, for this approval only: for an auto-approval, only while it lasts.
		TOptional<HyperAIStudio::TrustedExecution::Private::FAutoApprovedBy> AutoApproved;
		if (AutoCategory.IsSet())
		{
			AutoApproved.Emplace();
			AutoApproved->Activation = FHyperAIStudioAutoApproval::GetActivationId();
			AutoApproved->Category = *AutoCategory;
		}
		FString Status;
		if (!HyperAIStudio::TrustedExecution::Private::SubmitUserApproved(Item.Prepared, OperationId, Status, OutError, AutoApproved))
		{
			Fail(Status, OutError);
			return false;
		}
		if (AutoApproved.IsSet() && Status == TEXT("queued"))
		{
			AwaitingLane().Add({Item.Pending, Item.Sequence, AutoApproved->Activation});
		}

		FHyperAIStudioActivityEntry Entry = MakeEntry(EHyperAIStudioActivityKind::ApprovalApproved, Item.Pending.Summary, OperationId);
		Entry.StatusCode = Status;
		Entry.Detail = Approved + (Status == TEXT("queued")
			? TEXT("it runs when the edits ahead of it finish.") : TEXT("the edit is running."));
		Mark(Entry);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
		LogApproval();
		FHyperAIStudioApprovalGate::OnChanged().Broadcast();
		return true;
	}

	/**
	 * Approves, on the game thread, what auto-approval covered as it arrived, by the code a click runs. Requests it no
	 * longer covers (it was stopped, ran out or reached a limit) wait for a click again. True while more remain.
	 */
	bool SweepOnce()
	{
		FHyperAIStudioAutoApproval::EndIfOver();
		auto IsScheduled = [](const FHeldApproval& Item) { return Item.bScheduled; };
		if (!Held().ContainsByPredicate(IsScheduled))
		{
			return false;
		}
		if (IsEditorBusy())
		{
			return true;
		}
		TArray<TPair<uint64, FString>> Scheduled;
		for (const FHeldApproval& Item : Held())
		{
			if (Item.bScheduled)
			{
				Scheduled.Emplace(Item.Sequence, Item.Pending.OperationId);
			}
		}
		Scheduled.Sort([](const TPair<uint64, FString>& A, const TPair<uint64, FString>& B) { return A.Key < B.Key; });
		bool bUnscheduled = false;
		for (const TPair<uint64, FString>& Next : Scheduled)
		{
			// An approved action runs in place and may have changed what is held, so each one is looked up again.
			const int32 Index = IndexOf(Next.Value);
			if (Index == INDEX_NONE || !Held()[Index].bScheduled)
			{
				continue;
			}
			if (!IsCovered(Held()[Index]))
			{
				Held()[Index].bScheduled = false;
				bUnscheduled = true;
				continue;
			}
			const EHyperAIStudioApprovalCategory Category = *Held()[Index].Pending.Category;
			FHyperAIStudioAutoApproval::BeginApproval(Next.Value);
			FString Error;
			if (ApproveHeld(Next.Value, EHyperAIStudioApprover::AutoApproval, Category, Error))
			{
				FHyperAIStudioAutoApproval::NoteApproved(Category);
			}
			else
			{
				UE_LOG(LogHyperAIStudioApproval, Warning, TEXT("Auto-approved %s, but it could not start: %s"), *Next.Value, *Error);
			}
		}
		if (bUnscheduled)
		{
			FHyperAIStudioApprovalGate::OnChanged().Broadcast();
		}
		// What an approved action asked for in turn is approved on the next tick.
		return Held().ContainsByPredicate(IsScheduled);
	}

	bool Sweep(float)
	{
		const bool bMore = SweepOnce();
		if (!bMore)
		{
			SweepHandle().Reset();
		}
		return bMore;
	}

	int32 ReturnUnstarted(const uint64 EndedActivation)
	{
		TArray<FAwaitingLane> Awaiting = MoveTemp(AwaitingLane());
		AwaitingLane().Reset();
		int32 Returned = 0;
		for (FAwaitingLane& Entry : Awaiting)
		{
			FHeldApproval Item;
			if (Entry.Activation != EndedActivation || IndexOf(Entry.Pending.OperationId) != INDEX_NONE
				|| !HyperAIStudio::TrustedExecution::Private::WithdrawAutoApproved(Entry.Pending.OperationId, Entry.Activation, Item.Prepared))
			{
				// It started, failed, or another activation's; either way nothing of it is still waiting on this one.
				continue;
			}
			// Its place among held requests is the one it had, so no later auto-approval counts it as made after it began.
			Item.Pending = MoveTemp(Entry.Pending);
			Item.Pending.bAutoApproving = false;
			Item.Sequence = Entry.Sequence;
			ForgetApproval(Item.Pending.OperationId);
			FHyperAIStudioActivityEntry Activity = MakeEntry(EHyperAIStudioActivityKind::ApprovalRequested, Item.Pending.Summary, Item.Pending.OperationId);
			Activity.StatusCode = TEXT("awaiting_user_approval");
			Activity.Detail = TEXT("Auto-approval ended before this reached the edit lane; nothing ran. It waits for your approval again.");
			UE_LOG(LogHyperAIStudioApproval, Display, TEXT("%s (%s) waits for a click again: the auto-approval that approved it ended before it started."),
				*Item.Pending.Summary.ToolName, *Item.Pending.OperationId);
			Held().Add(MoveTemp(Item));
			FHyperAIStudioAgentActivityLog::Record(MoveTemp(Activity));
			++Returned;
		}
		if (Returned > 0)
		{
			FHyperAIStudioApprovalGate::OnChanged().Broadcast();
		}
		return Returned;
	}

#if WITH_DEV_AUTOMATION_TESTS
	void SweepForTests()
	{
		if (!SweepOnce() && SweepHandle().IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(SweepHandle());
			SweepHandle().Reset();
		}
	}
#endif
}

bool FHyperAIStudioApprovalGate::IsApprovalRequired(const EHyperAIStudioDomainSafety Safety)
{
	if (Safety == EHyperAIStudioDomainSafety::Read)
	{
		return false;
	}
	// Destructive and external-effect variants need a server-issued grant, and that grant exists only as the
	// product of the user's approval: a click, or the auto-approval they turned on for that category.
	if (Safety != EHyperAIStudioDomainSafety::Edit)
	{
		return true;
	}
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	return !Settings || Settings->bRequireApprovalForAgentEdits;
}

bool FHyperAIStudioApprovalGate::Request(
	const FHyperAIStudioTrustedPreparedArtifact& Prepared,
	const FString& OperationId,
	const FHyperAIStudioApprovalSummary& Summary,
	FString& OutError)
{
	using namespace HyperAIStudio::Approval::Private;
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Approval requests are queued on the game thread.");
		return false;
	}
	if (!Prepared.IsValid() || OperationId.IsEmpty() || Summary.EffectTarget.IsEmpty())
	{
		OutError = TEXT("An approval request needs a prepared plan, an operation id and an effect target.");
		return false;
	}
	if (IndexOf(OperationId) != INDEX_NONE)
	{
		// A resubmitted operation id is the same intent; the queued plan already covers it.
		return true;
	}
	if (Held().Num() >= MaxPending)
	{
		OutError = TEXT("Too many plans are already waiting for approval; approve or reject some first.");
		return false;
	}

	FHeldApproval Item;
	Item.Pending.OperationId = OperationId;
	Item.Pending.Summary = Summary;
	Item.Prepared = Prepared;
	// The grant follows the sealed variant, so auto-approval does too; a plan whose seal cannot be read waits for a click.
	EHyperAIStudioDomainSafety Sealed = EHyperAIStudioDomainSafety::Edit;
	if (HyperAIStudio::TrustedExecution::Private::DescribeQueuedSafety(Prepared, Sealed))
	{
		Item.Pending.Category = Categorize(Summary, Sealed);
	}
	Hold(MoveTemp(Item));
	return true;
}

bool FHyperAIStudioApprovalGate::RequestAction(
	const FString& OperationId,
	const FHyperAIStudioApprovalSummary& Summary,
	TFunction<void()> Run,
	FString& OutError)
{
	using namespace HyperAIStudio::Approval::Private;
	OutError.Reset();
	if (!IsInGameThread() || !Run || OperationId.IsEmpty() || Summary.EffectTarget.IsEmpty())
	{
		OutError = TEXT("An approval request needs the game thread, an action, an operation id and an effect target.");
		return false;
	}
	if (IndexOf(OperationId) != INDEX_NONE)
	{
		return true;
	}
	if (Held().Num() >= MaxPending)
	{
		OutError = TEXT("Too many plans are already waiting for approval; approve or reject some first.");
		return false;
	}
	FHeldApproval Item;
	Item.Pending.OperationId = OperationId;
	Item.Pending.Summary = Summary;
	Item.Pending.Category = Categorize(Summary, {});
	Item.Action = MoveTemp(Run);
	Hold(MoveTemp(Item));
	return true;
}

TArray<FHyperAIStudioPendingApproval> FHyperAIStudioApprovalGate::GetPending()
{
	using namespace HyperAIStudio::Approval::Private;
	TArray<FHyperAIStudioPendingApproval> Pending;
	for (const FHeldApproval& Item : Held())
	{
		FHyperAIStudioPendingApproval& Out = Pending.Add_GetRef(Item.Pending);
		Out.bAutoApproving = Item.bScheduled && IsCovered(Item);
	}
	return Pending;
}

bool FHyperAIStudioApprovalGate::HasPending(const FString& OperationId)
{
	return HyperAIStudio::Approval::Private::IndexOf(OperationId) != INDEX_NONE;
}

bool FHyperAIStudioApprovalGate::IsAutoApproving(const FString& OperationId)
{
	using namespace HyperAIStudio::Approval::Private;
	const int32 Index = IsInGameThread() ? IndexOf(OperationId) : INDEX_NONE;
	return Index != INDEX_NONE && Held()[Index].bScheduled && IsCovered(Held()[Index]);
}

bool FHyperAIStudioApprovalGate::DescribeHeld(const FString& OperationId, FString& InOutStatus, FString& InOutDiagnostic)
{
	using namespace HyperAIStudio::Approval::Private;
	if (!IsAutoApproving(OperationId))
	{
		return false;
	}
	const TOptional<EHyperAIStudioApprovalCategory>& Category = Held()[IndexOf(OperationId)].Pending.Category;
	InOutStatus = TEXT("queued");
	InOutDiagnostic = FString::Printf(
		TEXT("Auto-approval covers this request (%s): it is approved on the next quiet editor tick; nobody needs to click. ")
		TEXT("Call hyper_operation_wait with this operation_id for its outcome."),
		Category.IsSet() ? ::LexToString(*Category) : TEXT("edits"));
	return true;
}

TOptional<FHyperAIStudioApprovalRecord> FHyperAIStudioApprovalGate::FindApproval(const FString& OperationId)
{
	if (!IsInGameThread())
	{
		return {};
	}
	const FHyperAIStudioApprovalRecord* Record = HyperAIStudio::Approval::Private::Records().FindByPredicate(
		[&OperationId](const FHyperAIStudioApprovalRecord& Item) { return Item.OperationId == OperationId; });
	return Record ? TOptional<FHyperAIStudioApprovalRecord>(*Record) : TOptional<FHyperAIStudioApprovalRecord>();
}

bool FHyperAIStudioApprovalGate::Approve(const FString& OperationId, FString& OutError)
{
	return HyperAIStudio::Approval::Private::ApproveHeld(OperationId, EHyperAIStudioApprover::User, {}, OutError);
}

bool FHyperAIStudioApprovalGate::ApproveForJob(const FString& OperationId, FString& OutError)
{
	return HyperAIStudio::Approval::Private::ApproveHeld(OperationId, EHyperAIStudioApprover::AgentJob, {}, OutError);
}

void FHyperAIStudioApprovalGate::Reject(const FString& OperationId, const FString& Reason)
{
	using namespace HyperAIStudio::Approval::Private;
	if (!IsInGameThread())
	{
		return;
	}
	const int32 Index = IndexOf(OperationId);
	if (Index == INDEX_NONE)
	{
		return;
	}
	const FHyperAIStudioApprovalSummary Summary = Held()[Index].Pending.Summary;
	Held().RemoveAt(Index);
	FHyperAIStudioActivityEntry Entry = MakeEntry(EHyperAIStudioActivityKind::ApprovalRejected, Summary, OperationId);
	Entry.StatusCode = TEXT("rejected_by_user");
	Entry.Detail = Reason;
	FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	OnChanged().Broadcast();
}

void FHyperAIStudioApprovalGate::Clear()
{
	using namespace HyperAIStudio::Approval::Private;
	if (SweepHandle().IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(SweepHandle());
		SweepHandle().Reset();
	}
	Held().Reset();
	AwaitingLane().Reset();
	OnChanged().Broadcast();
}

FSimpleMulticastDelegate& FHyperAIStudioApprovalGate::OnChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}
