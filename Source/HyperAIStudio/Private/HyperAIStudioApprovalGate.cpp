// Games by Hyper 2026.

#include "HyperAIStudioApprovalGate.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGateInternal.h"
#include "HyperAIStudioAutoApproval.h"
#include "HyperAIStudioChatModes.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioTrustedExecutionInternal.h"
#include "Layout/WidgetPath.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SViewport.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioApprovalGate"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioApproval, Log, All);

const TCHAR* LexToString(const EHyperAIStudioApprovalCategory Category)
{
	switch (Category)
	{
	case EHyperAIStudioApprovalCategory::Scripts: return TEXT("scripts");
	case EHyperAIStudioApprovalCategory::Destructive: return TEXT("destructive");
	case EHyperAIStudioApprovalCategory::ExternalEffects: return TEXT("external_effects");
	case EHyperAIStudioApprovalCategory::SandboxSaves: return TEXT("sandbox_saves");
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

	/** How often a sweep that keeps waiting says so again in the log; a new reason is logged at once. */
	constexpr double WaitLogSeconds = 15.0;

	/** The wait for a quiet tick the sweep is in, so the log and Activity can say what holds approvals back. */
	struct FWait
	{
		FBusy Busy;
		double Since = 0.0;
		double LastLogged = -1.0;
		int32 Ticks = 0;
	};

	FWait& Wait()
	{
		static FWait Value;
		return Value;
	}

	/** A scheduled approval waits another tick; logged when the reason changes and then at most every WaitLogSeconds. */
	void NoteWait(const FBusy& Busy, const int32 Scheduled)
	{
		FWait& State = Wait();
		const double Now = FPlatformTime::Seconds();
		if (State.Ticks == 0)
		{
			State.Since = Now;
		}
		++State.Ticks;
		const bool bNewReason = State.Busy.Log != Busy.Log;
		State.Busy = Busy;
		if (bNewReason || Now - State.LastLogged >= WaitLogSeconds)
		{
			State.LastLogged = Now;
			UE_LOG(LogHyperAIStudioApproval, Display, TEXT("Auto-approval is waiting for a quiet editor tick to approve %d request(s): %s (waiting %.1f s, %d tick(s))."),
				Scheduled, *Busy.Log, Now - State.Since, State.Ticks);
		}
	}

	/** The sweep went ahead after Ended, the wait it was in: a wait it logged says how it ended, with what it approved. */
	void LogWaitEnded(const FWait& Ended, const int32 Approved)
	{
		if (Ended.Ticks > 0 && Ended.LastLogged >= 0.0 && Approved > 0)
		{
			UE_LOG(LogHyperAIStudioApproval, Display, TEXT("The editor is quiet again after %.1f s; auto-approval approved %d request(s)."),
				FPlatformTime::Seconds() - Ended.Since, Approved);
		}
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
		Record.bKeptOnAfterRestart = AutoCategory.IsSet() && FHyperAIStudioAutoApproval::IsRestored();
		Record.ApprovedUtc = FDateTime::UtcNow();
	}

	bool IsCovered(const FHeldApproval& Item)
	{
		// A chat in a mode that asks for every edit, or a read-only one, leaves each plan to a click.
		return Item.Pending.Category.IsSet() && !HyperAIStudio::ChatModes::IsAskActive() && HyperAIStudio::ChatModes::ReadOnlyHolder().IsEmpty()
			&& FHyperAIStudioAutoApproval::Covers(*Item.Pending.Category, Item.Sequence, Item.Pending.OperationId);
	}

	const SWidget* PlayingGameViewport()
	{
		// Simulate In Editor, and a play session ejected to it, run in the level viewport the user navigates and drags in:
		// the same widget is the registered game viewport then, but a capture there is the user's.
		if (!GEditor || !GEditor->PlayWorld || GEditor->bIsSimulatingInEditor || GEditor->IsSimulateInEditorInProgress()
			|| !FSlateApplication::IsInitialized())
		{
			return nullptr;
		}
		return FSlateApplication::Get().GetGameViewport().Get();
	}

	FString DescribeCaptor(const SWidget& Captor, const SWidget* PlayingGame)
	{
		return &Captor == PlayingGame ? FString() : Captor.GetTypeAsString();
	}

	FBusy DescribeBusy()
	{
		FBusy Busy;
		if (GEditor && GEditor->IsTransactionActive())
		{
			const FText Name = GEditor->GetTransactionName();
			Busy.Log = FString::Printf(TEXT("an undo transaction is open (%s)"), Name.IsEmpty() ? TEXT("unnamed") : *Name.ToString());
			Busy.Text = FText::Format(LOCTEXT("BusyTransaction", "an undo step is still open ({0})"), Name);
		}
		else if (FSlateApplication::IsInitialized() && FSlateApplication::Get().HasAnyMouseCaptor())
		{
			// A game that holds the mouse while Play In Editor runs is no drag, and holds it for the whole session: it
			// does not count, or nothing would be approved until play stopped. Any other capture is named.
			const SWidget* Game = PlayingGameViewport();
			FString Captor;
			FSlateApplication::Get().ForEachUser([&Captor, Game](FSlateUser& User)
			{
				if (!Captor.IsEmpty() || !User.HasAnyCapture())
				{
					return;
				}
				const TArray<FWidgetPath> Paths = User.GetCaptorPaths();
				if (Paths.IsEmpty())
				{
					Captor = FString::Printf(TEXT("a widget of user %d"), User.GetUserIndex());
				}
				for (const FWidgetPath& Path : Paths)
				{
					if (!Path.IsValid())
					{
						Captor = FString::Printf(TEXT("a widget of user %d"), User.GetUserIndex());
						break;
					}
					Captor = DescribeCaptor(Path.GetLastWidget().Get(), Game);
					if (!Captor.IsEmpty())
					{
						break;
					}
				}
			}, /*bIncludeVirtualUsers=*/true);
			if (!Captor.IsEmpty())
			{
				Busy.Log = FString::Printf(TEXT("the mouse is captured by %s, as in a drag"), *Captor);
				Busy.Text = FText::Format(LOCTEXT("BusyCapture", "the mouse is captured ({0}), as in a drag"), FText::FromString(Captor));
			}
		}
		if (Busy.IsSet())
		{
			return Busy;
		}
		if (UE::IsSavingPackage())
		{
			Busy.Log = TEXT("a package is being saved");
			Busy.Text = LOCTEXT("BusySaving", "a package is being saved");
		}
		else if (IsGarbageCollecting())
		{
			Busy.Log = TEXT("garbage is being collected");
			Busy.Text = LOCTEXT("BusyGarbage", "garbage is being collected");
		}
		return Busy;
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
		// An offer HyperAI made by itself waits for a click: nothing files it under a category auto-approval covers.
		if (Summary.bClickOnly)
		{
			return {};
		}
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

	/** The category as the log names it. */
	FString CategoryForLog(const TOptional<EHyperAIStudioApprovalCategory>& Category)
	{
		return Category.IsSet() ? FString(::LexToString(*Category)) : FString(TEXT("no category (only a click approves it)"));
	}

	/** Holds a request; one the user's auto-approval covers is approved on the next quiet tick. */
	void Hold(FHeldApproval&& Item)
	{
		FHyperAIStudioAutoApproval::EndIfOver();
		Item.Sequence = NextSequence()++;
		Item.Pending.RequestedUtc = FDateTime::UtcNow();
		Item.bScheduled = IsCovered(Item);
		const bool bScheduled = Item.bScheduled;
		// What the user expects auto-approval to do with each request can be read back from the log.
		const FHyperAIStudioApprovalSummary& Summary = Item.Pending.Summary;
		if (bScheduled)
		{
			UE_LOG(LogHyperAIStudioApproval, Display, TEXT("Holding %s on %s (%s) as %s: auto-approval covers it and approves it on the next quiet editor tick."),
				*Summary.ToolName, *Summary.EffectTarget, *Item.Pending.OperationId, *CategoryForLog(Item.Pending.Category));
		}
		else if (FHyperAIStudioAutoApproval::IsActive())
		{
			UE_LOG(LogHyperAIStudioApproval, Display, TEXT("Holding %s on %s (%s) as %s for a click: auto-approval is on but does not cover it: %s."),
				*Summary.ToolName, *Summary.EffectTarget, *Item.Pending.OperationId, *CategoryForLog(Item.Pending.Category),
				*FHyperAIStudioAutoApproval::DescribeWhyNotCovered(Item.Pending.Category, Item.Sequence, Item.Pending.OperationId));
		}
		else
		{
			UE_LOG(LogHyperAIStudioApproval, Log, TEXT("Holding %s on %s (%s) as %s for a click."),
				*Summary.ToolName, *Summary.EffectTarget, *Item.Pending.OperationId, *CategoryForLog(Item.Pending.Category));
		}
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
		// A request that was waiting when auto-approval began was approved because the user chose it in its dialog, or
		// because the auto-approval this one replaced had approved it before it started.
		const bool bFromBefore = AutoCategory.IsSet() && FHyperAIStudioAutoApproval::IsFromBeforeStart(Item.Sequence);
		const bool bCarried = bFromBefore && FHyperAIStudioAutoApproval::WasCarriedOver(OperationId);
		const TCHAR* const FromBefore = !bFromBefore ? TEXT("")
			: bCarried ? TEXT(" (the auto-approval this one replaced had approved it before it started)")
			: TEXT(" (it was waiting when auto-approval was turned on, and was chosen in its dialog)");
		// An auto-approval that came back on by itself after a restart says so wherever its approvals show.
		const bool bKeptOn = AutoCategory.IsSet() && FHyperAIStudioAutoApproval::IsRestored();
		const FString Approved = AutoCategory.IsSet()
			? FString::Printf(TEXT("Auto-approved as %s%s%s; "), ::LexToString(*AutoCategory), bKeptOn ? TEXT(" (kept on after restart)") : TEXT(""), FromBefore)
			: Approver == EHyperAIStudioApprover::AgentJob ? FString(TEXT("Approved for a running agent job; "))
			: FString(TEXT("Approved; "));
		auto LogApproval = [&Item, &OperationId, &AutoCategory, Approver, bFromBefore, bCarried, bKeptOn]()
		{
			if (AutoCategory.IsSet())
			{
				UE_LOG(LogHyperAIStudioApproval, Display, TEXT("Auto-approved %s on %s (%s) as %s%s%s."),
					*Item.Pending.Summary.ToolName, *Item.Pending.Summary.EffectTarget, *OperationId, ::LexToString(*AutoCategory),
					bKeptOn ? TEXT(" by auto-approval kept on after the editor restarted") : TEXT(""),
					!bFromBefore ? TEXT("") : bCarried ? TEXT(", which the auto-approval it replaced had approved before it started")
					: TEXT(", which was waiting when auto-approval was turned on"));
			}
			else
			{
				UE_LOG(LogHyperAIStudioApproval, Log, TEXT("Approved %s on %s (%s)%s."),
					*Item.Pending.Summary.ToolName, *Item.Pending.Summary.EffectTarget, *OperationId,
					Approver == EHyperAIStudioApprover::AgentJob ? TEXT(" for a running agent job") : TEXT(""));
			}
		};
		auto Mark = [&AutoCategory, Approver, bKeptOn](FHyperAIStudioActivityEntry& Entry)
		{
			Entry.AutoApprovedAs = AutoCategory;
			Entry.bKeptOnAfterRestart = bKeptOn;
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
			{
				const FHyperAIStudioAgentWorkScope Work;
				Item.Action();
			}
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
		// What it no longer covers waits for a click from here on, before anything is counted or logged as about to be
		// approved: an auto-approval that ended while the editor was busy approves nothing when it is quiet again.
		bool bUnscheduled = false;
		int32 ScheduledCount = 0;
		for (FHeldApproval& Item : Held())
		{
			if (Item.bScheduled && !IsCovered(Item))
			{
				Item.bScheduled = false;
				bUnscheduled = true;
			}
			ScheduledCount += Item.bScheduled ? 1 : 0;
		}
		if (bUnscheduled)
		{
			FHyperAIStudioApprovalGate::OnChanged().Broadcast();
		}
		if (ScheduledCount == 0)
		{
			Wait() = FWait();
			return false;
		}
		if (const FBusy Busy = DescribeBusy(); Busy.IsSet())
		{
			NoteWait(Busy, ScheduledCount);
			return true;
		}
		// Cleared before anything runs: an approved action may ask what an approval waits for.
		const FWait Ended = Wait();
		Wait() = FWait();
		TArray<TPair<uint64, FString>> Scheduled;
		for (const FHeldApproval& Item : Held())
		{
			if (Item.bScheduled)
			{
				Scheduled.Emplace(Item.Sequence, Item.Pending.OperationId);
			}
		}
		Scheduled.Sort([](const TPair<uint64, FString>& A, const TPair<uint64, FString>& B) { return A.Key < B.Key; });
		bUnscheduled = false;
		int32 Approved = 0;
		for (const TPair<uint64, FString>& Next : Scheduled)
		{
			// An approved action runs in place and may have changed what is held, so each one is looked up again; an
			// approval may also have reached a limit, which ends the auto-approval for the rest.
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
			++Approved;
			if (ApproveHeld(Next.Value, EHyperAIStudioApprover::AutoApproval, Category, Error))
			{
				FHyperAIStudioAutoApproval::NoteApproved(Category);
			}
			else
			{
				UE_LOG(LogHyperAIStudioApproval, Warning, TEXT("Auto-approved %s, but it could not start: %s"), *Next.Value, *Error);
			}
		}
		LogWaitEnded(Ended, Approved);
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

	int32 ReturnUnstarted(const uint64 EndedActivation, TArray<FString>* OutReturned)
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
			if (OutReturned)
			{
				OutReturned->Add(Item.Pending.OperationId);
			}
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

	int32 ScheduleCovered()
	{
		int32 Scheduled = 0;
		for (FHeldApproval& Item : Held())
		{
			if (Item.bScheduled || !IsCovered(Item))
			{
				continue;
			}
			Item.bScheduled = true;
			++Scheduled;
			UE_LOG(LogHyperAIStudioApproval, Display, TEXT("Auto-approval will approve %s on %s (%s) as %s, %s."),
				*Item.Pending.Summary.ToolName, *Item.Pending.Summary.EffectTarget, *Item.Pending.OperationId, *CategoryForLog(Item.Pending.Category),
				FHyperAIStudioAutoApproval::WasCarriedOver(Item.Pending.OperationId)
					? TEXT("which the auto-approval it replaced had approved before it started")
					: TEXT("which was waiting when it was turned on and was chosen in its dialog"));
		}
		if (Scheduled > 0)
		{
			ScheduleSweep();
			FHyperAIStudioApprovalGate::OnChanged().Broadcast();
		}
		return Scheduled;
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
	// A chat mode that asks for edits, or refuses them, routes every edit through the gate, which then does so.
	if (HyperAIStudio::ChatModes::IsAskActive() || !HyperAIStudio::ChatModes::ReadOnlyHolder().IsEmpty())
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
	// A read-only chat mode is enforced here, where every edit plan arrives, not just asked of the agent.
	if (const FString Refusal = HyperAIStudio::ChatModes::ReadOnlyRefusal(); !Refusal.IsEmpty() && IndexOf(OperationId) == INDEX_NONE)
	{
		OutError = Refusal;
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
	if (const FString Refusal = HyperAIStudio::ChatModes::ReadOnlyRefusal(); !Refusal.IsEmpty() && IndexOf(OperationId) == INDEX_NONE)
	{
		OutError = Refusal;
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
	const FBusy& Busy = Wait().Busy;
	InOutStatus = TEXT("queued");
	InOutDiagnostic = FString::Printf(
		TEXT("Auto-approval covers this request (%s): it is approved on the next quiet editor tick; nobody needs to click.%s ")
		TEXT("Call hyper_operation_wait with this operation_id for its outcome."),
		Category.IsSet() ? ::LexToString(*Category) : TEXT("edits"),
		Busy.IsSet() ? *FString::Printf(TEXT(" It is waiting because %s."), *Busy.Log) : TEXT(""));
	return true;
}

FText FHyperAIStudioApprovalGate::DescribeAutoApprovalWait()
{
	return IsInGameThread() ? HyperAIStudio::Approval::Private::Wait().Busy.Text : FText::GetEmpty();
}

FString FHyperAIStudioApprovalGate::DescribeWhyNotAutoApproved(const FString& OperationId)
{
	using namespace HyperAIStudio::Approval::Private;
	const int32 Index = IsInGameThread() && FHyperAIStudioAutoApproval::IsActive() ? IndexOf(OperationId) : INDEX_NONE;
	if (Index == INDEX_NONE)
	{
		return FString();
	}
	const FHeldApproval& Item = Held()[Index];
	return FHyperAIStudioAutoApproval::DescribeWhyNotCovered(Item.Pending.Category, Item.Sequence, Item.Pending.OperationId);
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

namespace HyperAIStudio::Approval::Private
{
	int32& AgentWorkDepth()
	{
		static int32 Depth = 0;
		return Depth;
	}
}

FHyperAIStudioAgentWorkScope::FHyperAIStudioAgentWorkScope()
{
	++HyperAIStudio::Approval::Private::AgentWorkDepth();
}

FHyperAIStudioAgentWorkScope::~FHyperAIStudioAgentWorkScope()
{
	--HyperAIStudio::Approval::Private::AgentWorkDepth();
}

bool FHyperAIStudioAgentWorkScope::IsRunning()
{
	return HyperAIStudio::Approval::Private::AgentWorkDepth() > 0;
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
	Wait() = FWait();
	OnChanged().Broadcast();
}

FSimpleMulticastDelegate& FHyperAIStudioApprovalGate::OnChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

#undef LOCTEXT_NAMESPACE
