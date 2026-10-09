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

	constexpr int32 MaxRecords = 64;

	TArray<FHeldApproval>& Held()
	{
		static TArray<FHeldApproval> Storage;
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

	void RecordApproval(const FString& OperationId, const TOptional<EHyperAIStudioApprovalCategory>& AutoCategory)
	{
		TArray<FHyperAIStudioApprovalRecord>& Items = Records();
		Items.RemoveAll([&OperationId](const FHyperAIStudioApprovalRecord& Record) { return Record.OperationId == OperationId; });
		if (Items.Num() >= MaxRecords)
		{
			Items.RemoveAt(0);
		}
		FHyperAIStudioApprovalRecord& Record = Items.AddDefaulted_GetRef();
		Record.OperationId = OperationId;
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
		FHyperAIStudioAutoApproval::EndIfExpired();
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

	/** A click and an auto-approval both approve here; only the activity entry, the log and the record say which. */
	bool ApproveHeld(const FString& OperationId, const TOptional<EHyperAIStudioApprovalCategory>& AutoCategory, FString& OutError)
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
		RecordApproval(OperationId, AutoCategory);
		const FString Approved = AutoCategory.IsSet()
			? FString::Printf(TEXT("Auto-approved as %s; "), ::LexToString(*AutoCategory)) : FString(TEXT("Approved; "));
		auto LogApproval = [&Item, &OperationId, &AutoCategory]()
		{
			if (AutoCategory.IsSet())
			{
				UE_LOG(LogHyperAIStudioApproval, Display, TEXT("Auto-approved %s on %s (%s) as %s."),
					*Item.Pending.Summary.ToolName, *Item.Pending.Summary.EffectTarget, *OperationId, ::LexToString(*AutoCategory));
			}
			else
			{
				UE_LOG(LogHyperAIStudioApproval, Log, TEXT("Approved %s on %s (%s)."),
					*Item.Pending.Summary.ToolName, *Item.Pending.Summary.EffectTarget, *OperationId);
			}
		};
		if (Item.Action)
		{
			FHyperAIStudioActivityEntry Entry = MakeEntry(EHyperAIStudioActivityKind::ApprovalApproved, Item.Pending.Summary, OperationId);
			Entry.StatusCode = TEXT("approved");
			Entry.Detail = Approved + TEXT("running.");
			Entry.AutoApprovedAs = AutoCategory;
			FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
			LogApproval();
			FHyperAIStudioApprovalGate::OnChanged().Broadcast();
			Item.Action();
			return true;
		}

		auto Fail = [&Item, &OperationId](const FString& Status, const FString& Diagnostic)
		{
			FHyperAIStudioActivityEntry Entry = MakeEntry(EHyperAIStudioActivityKind::Failed, Item.Pending.Summary, OperationId);
			Entry.StatusCode = Status;
			Entry.Detail = Diagnostic;
			FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
			FHyperAIStudioApprovalGate::OnChanged().Broadcast();
		};

		// Staging happens when the plan reaches the lane, not when the agent asked, so the stage lifetime covers only
		// the run. A plan the user took longer than its seal to approve is prepared again from the same inputs, and a
		// risky plan's grant is issued then, for this approval only.
		FString Status;
		if (!HyperAIStudio::TrustedExecution::Private::SubmitUserApproved(Item.Prepared, OperationId, Status, OutError))
		{
			Fail(Status, OutError);
			return false;
		}

		FHyperAIStudioActivityEntry Entry = MakeEntry(EHyperAIStudioActivityKind::ApprovalApproved, Item.Pending.Summary, OperationId);
		Entry.StatusCode = Status;
		Entry.Detail = Approved + (Status == TEXT("queued")
			? TEXT("it runs when the edits ahead of it finish.") : TEXT("the edit is running."));
		Entry.AutoApprovedAs = AutoCategory;
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
		LogApproval();
		FHyperAIStudioApprovalGate::OnChanged().Broadcast();
		return true;
	}

	/**
	 * Approves, on the game thread, what auto-approval covered as it arrived, by the code a click runs. Requests it no
	 * longer covers (it was stopped, ran out or reached a limit) wait for a click again.
	 */
	bool Sweep(float)
	{
		FHyperAIStudioAutoApproval::EndIfExpired();
		auto IsScheduled = [](const FHeldApproval& Item) { return Item.bScheduled; };
		if (!Held().ContainsByPredicate(IsScheduled))
		{
			SweepHandle().Reset();
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
			if (ApproveHeld(Next.Value, Category, Error))
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
		const bool bMore = Held().ContainsByPredicate(IsScheduled);
		if (!bMore)
		{
			SweepHandle().Reset();
		}
		return bMore;
	}
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
	return HyperAIStudio::Approval::Private::ApproveHeld(OperationId, {}, OutError);
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
	OnChanged().Broadcast();
}

FSimpleMulticastDelegate& FHyperAIStudioApprovalGate::OnChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}
