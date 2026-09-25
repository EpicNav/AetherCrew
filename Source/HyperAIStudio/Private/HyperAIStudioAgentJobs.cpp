// Games by Hyper 2026.

#include "HyperAIStudioAgentJobs.h"

#include "Containers/Ticker.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioShell.h"
#include "SHyperAIStudioQuickActionWindow.h"
#include "Widgets/Notifications/SNotificationList.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioJobs, Log, All);

namespace HyperAIStudio::AgentJobs
{
	/** A turn is over once the agent has been idle this long: between two tool calls it can look idle for a moment. */
	constexpr double IdleSecondsForTurnEnd = 2.0;
	/** A message the agent never starts on counts, after this long, as a turn in which nothing was done. */
	constexpr double StartTimeoutSeconds = 120.0;
	constexpr float TickSeconds = 0.5f;
	constexpr int32 MaxKeptJobs = 20;

	struct FJob
	{
		FHyperAIStudioAgentJobInfo Info;
		FHyperAIStudioAgentJobSpec Spec;
		/** The agent has been seen working since the last message; its going idle after that ends the turn. */
		bool bSawWorking = false;
		bool bStartedNoticed = false;
		double IdleSince = 0.0;
		double SentAt = 0.0;
	};

	TArray<FJob>& Jobs()
	{
		static TArray<FJob> Storage;
		return Storage;
	}

	FTSTicker::FDelegateHandle& TickerHandle()
	{
		static FTSTicker::FDelegateHandle Handle;
		return Handle;
	}

	int32& NextId()
	{
		static int32 Id = 1;
		return Id;
	}

	void Notify(const FString& Text, const bool bGood)
	{
		FNotificationInfo Info(FText::FromString(Text));
		Info.ExpireDuration = 8.0f;
		Info.bUseSuccessFailIcons = true;
		if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(bGood ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}

	void Finish(FJob& Job, const EHyperAIStudioAgentJobState State, const FString& Status)
	{
		Job.Info.State = State;
		Job.Info.Status = Status;
		UE_LOG(LogHyperAIStudioJobs, Display, TEXT("Job %d (%s) %s after %d turn(s): %s"), Job.Info.Id, *Job.Info.Title,
			State == EHyperAIStudioAgentJobState::Succeeded ? TEXT("succeeded") : State == EHyperAIStudioAgentJobState::Failed ? TEXT("failed") : TEXT("stopped"),
			Job.Info.Turns, *Status);
		Notify(FString::Printf(TEXT("%s: %s"), *Job.Info.Title, *Status), State == EHyperAIStudioAgentJobState::Succeeded);
	}

	/**
	 * The user asked for a job's work by starting it, so plain edits in its packs are approved for them as they come.
	 * ponytail: scoped by pack, not by chat (MCP calls say nothing of their caller), so another agent's edit in the same
	 * pack while the job runs is approved too; per-chat MCP endpoints would make it exact.
	 */
	void ApproveJobEdits()
	{
		TSet<FString> Packs;
		for (const FJob& Job : Jobs())
		{
			if (Job.Info.IsRunning())
			{
				Packs.Append(Job.Spec.AutoApprovePacks);
			}
		}
		if (Packs.IsEmpty())
		{
			return;
		}
		for (const FHyperAIStudioPendingApproval& Pending : FHyperAIStudioApprovalGate::GetPending())
		{
			if (Pending.Summary.Safety != EHyperAIStudioDomainSafety::Edit || !Packs.Contains(Pending.Summary.PackId))
			{
				continue;
			}
			FString Error;
			if (FHyperAIStudioApprovalGate::Approve(Pending.OperationId, Error))
			{
				UE_LOG(LogHyperAIStudioJobs, Display, TEXT("Approved %s (%s) for a running job."), *Pending.OperationId, *Pending.Summary.ToolName);
			}
			else
			{
				UE_LOG(LogHyperAIStudioJobs, Warning, TEXT("Could not approve %s for a running job: %s"), *Pending.OperationId, *Error);
			}
		}
	}

	/** A turn ends when the agent goes idle; the owner then says whether it goes back to work. */
	bool Tick(float)
	{
		ApproveJobEdits();
		bool bChanged = false;
		bool bAnyRunning = false;
		const double Now = FPlatformTime::Seconds();
		for (FJob& Job : Jobs())
		{
			if (!Job.Info.IsRunning())
			{
				continue;
			}
			bool bWorking = false;
			bool bStarted = false;
			if (!SHyperAIStudioQuickActionWindow::GetTabWork(Job.Info.TabIndex, bWorking, &bStarted))
			{
				Finish(Job, EHyperAIStudioAgentJobState::Failed, TEXT("The chat tab closed, so the job stopped."));
				bChanged = true;
				continue;
			}
			bAnyRunning = true;
			if (bStarted && !Job.bStartedNoticed)
			{
				Job.bStartedNoticed = true;
				if (Job.Spec.OnStarted)
				{
					Job.Spec.OnStarted();
				}
			}
			if (bWorking)
			{
				if (Job.Info.State != EHyperAIStudioAgentJobState::Working)
				{
					Job.Info.State = EHyperAIStudioAgentJobState::Working;
					bChanged = true;
				}
				Job.bSawWorking = true;
				Job.IdleSince = 0.0;
				continue;
			}
			if (!Job.bSawWorking && Now - Job.SentAt < StartTimeoutSeconds)
			{
				continue;
			}
			if (Job.IdleSince <= 0.0)
			{
				Job.IdleSince = Now;
				continue;
			}
			if (Now - Job.IdleSince < IdleSecondsForTurnEnd)
			{
				continue;
			}

			++Job.Info.Turns;
			bChanged = true;
			const FHyperAIStudioAgentJobVerdict Verdict = Job.Spec.OnTurnEnded ? Job.Spec.OnTurnEnded(Job.Info.Turns) : FHyperAIStudioAgentJobVerdict();
			Job.Info.Status = Verdict.Status;
			if (Verdict.bSucceeded)
			{
				Finish(Job, EHyperAIStudioAgentJobState::Succeeded, Verdict.Status.IsEmpty() ? FString(TEXT("Done.")) : Verdict.Status);
				continue;
			}
			if (Verdict.NextMessage.IsEmpty())
			{
				Finish(Job, EHyperAIStudioAgentJobState::Stopped, Verdict.Status.IsEmpty() ? FString(TEXT("Stopped after this turn.")) : Verdict.Status);
				continue;
			}
			if (Job.Info.Turns > Job.Spec.MaxFollowUps)
			{
				Finish(Job, EHyperAIStudioAgentJobState::Stopped,
					FString::Printf(TEXT("%s Stopped at the limit of %d rounds."), *Verdict.Status, Job.Spec.MaxFollowUps + 1).TrimStart());
				continue;
			}
			FString Message;
			if (!SHyperAIStudioQuickActionWindow::SendToChat(Job.Info.TabIndex, Verdict.NextMessage, Message))
			{
				Finish(Job, EHyperAIStudioAgentJobState::Failed, TEXT("The next round could not be sent: ") + Message);
				continue;
			}
			UE_LOG(LogHyperAIStudioJobs, Display, TEXT("Job %d, round %d: %s"), Job.Info.Id, Job.Info.Turns + 1, *Verdict.Status);
			Job.bSawWorking = false;
			Job.IdleSince = 0.0;
			Job.SentAt = Now;
		}
		if (bChanged)
		{
			FHyperAIStudioAgentJobs::OnChanged().Broadcast();
		}
		if (!bAnyRunning)
		{
			TickerHandle().Reset();
			return false;
		}
		return true;
	}
}

int32 FHyperAIStudioAgentJobs::Start(FHyperAIStudioAgentJobSpec Spec, FString& OutError)
{
	using namespace HyperAIStudio::AgentJobs;
	if (Spec.Prompt.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("A job needs a prompt.");
		return 0;
	}
	FString Agent;
	const int32 Tab = SHyperAIStudioQuickActionWindow::OpenJobChat(Spec.Category, Spec.Prompt, Spec.Images, Agent, OutError);
	if (Tab <= 0)
	{
		return 0;
	}
	FJob Job;
	Job.Info.Id = NextId()++;
	Job.Info.Title = Spec.Title.IsEmpty() ? FString(TEXT("Agent job")) : Spec.Title;
	Job.Info.Category = Spec.Category;
	Job.Info.Key = Spec.Key;
	Job.Info.TabIndex = Tab;
	Job.Info.Agent = Agent;
	Job.Info.Status = FString::Printf(TEXT("Chat %d, %s, is starting."), Tab, *Agent);
	Job.SentAt = FPlatformTime::Seconds();
	Job.Spec = MoveTemp(Spec);
	const int32 Id = Job.Info.Id;
	Jobs().Insert(MoveTemp(Job), 0);
	// Finished jobs are kept for the pages that show them, up to a point.
	for (int32 Index = Jobs().Num() - 1; Index >= 0 && Jobs().Num() > MaxKeptJobs; --Index)
	{
		if (!Jobs()[Index].Info.IsRunning())
		{
			Jobs().RemoveAt(Index);
		}
	}
	if (!TickerHandle().IsValid())
	{
		TickerHandle() = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), TickSeconds);
	}
	UE_LOG(LogHyperAIStudioJobs, Display, TEXT("Job %d (%s) started in chat %d with %s."), Id, *Jobs()[0].Info.Title, Tab, *Agent);
	OnChanged().Broadcast();
	return Id;
}

void FHyperAIStudioAgentJobs::Stop(const int32 Id, const FString& Why)
{
	using namespace HyperAIStudio::AgentJobs;
	for (FJob& Job : Jobs())
	{
		if (Job.Info.Id == Id && Job.Info.IsRunning())
		{
			SHyperAIStudioQuickActionWindow::InterruptTab(Job.Info.TabIndex);
			Finish(Job, EHyperAIStudioAgentJobState::Stopped, Why.IsEmpty() ? FString(TEXT("Stopped by you.")) : Why);
			OnChanged().Broadcast();
		}
	}
}

TArray<FHyperAIStudioAgentJobInfo> FHyperAIStudioAgentJobs::List()
{
	TArray<FHyperAIStudioAgentJobInfo> Infos;
	for (const HyperAIStudio::AgentJobs::FJob& Job : HyperAIStudio::AgentJobs::Jobs())
	{
		Infos.Add(Job.Info);
	}
	return Infos;
}

TOptional<FHyperAIStudioAgentJobInfo> FHyperAIStudioAgentJobs::FindByKey(const FString& Key)
{
	for (const HyperAIStudio::AgentJobs::FJob& Job : HyperAIStudio::AgentJobs::Jobs())
	{
		if (!Key.IsEmpty() && Job.Info.Key.Equals(Key, ESearchCase::IgnoreCase))
		{
			return Job.Info;
		}
	}
	return {};
}

void FHyperAIStudioAgentJobs::ShowChat(const int32 Id)
{
	for (const HyperAIStudio::AgentJobs::FJob& Job : HyperAIStudio::AgentJobs::Jobs())
	{
		if (Job.Info.Id == Id)
		{
			FHyperAIStudioShell::Open(SHyperAIStudioQuickActionWindow::GetPageId(Job.Info.TabIndex));
		}
	}
}

FSimpleMulticastDelegate& FHyperAIStudioAgentJobs::OnChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}
