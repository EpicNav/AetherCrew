// Games by Hyper 2026.

#include "HyperAIStudioSandbox.h"

#include "Algo/Transform.h"
#include "Containers/Ticker.h"
#include "Data/SandboxMetaData.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioExecutionQueue.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSettings.h"
#include "IFileSandboxCoreModule.h"
#include "ISandboxInstance.h"
#include "ISandboxManager.h"
#include "ISandboxRepository.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Templates/ValueOrError.h"
#include "Types/EBreakBehavior.h"
#include "Types/Manager/DeleteSandboxByDirectoryArgs.h"
#include "Types/Manager/DeleteSandboxResult.h"
#include "Types/Manager/LeaveSandboxArgs.h"
#include "Types/Manager/LeaveSandboxResult.h"
#include "Types/Manager/LoadSandboxByDirectoryArgs.h"
#include "Types/Manager/LoadSandboxError.h"
#include "Types/Manager/NewSandboxArgs.h"
#include "Types/Manager/NewSandboxError.h"
#include "Types/Manager/SandboxCreationResult.h"
#include "Types/Sandbox/RevertResult.h"
#include "Types/SandboxFileChange.h"
#include "Types/SandboxMetaInfo.h"
#include "Types/SandboxedFileChangeInfo.h"
#include "UObject/Package.h"
#include "Utils/SandboxDirectoryUtils.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioSandbox"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioSandbox, Log, All);

namespace HyperAIStudio::Sandbox
{
	namespace
	{
		namespace FSC = UE::FileSandboxCore;

		const FName HyperAITag(TEXT("HyperAI"));
		/** Epic's Sandboxes browser and status bar adopt sandboxes with this tag. Its header is private, hence the literal. */
		const FName SandboxedEditingTag(TEXT("SandboxedEditingTag"));

		struct FStateData
		{
			bool bStarted = false;
			TOptional<bool> ModeOverride;
			/** When HyperAI's sandbox opened; agent edits after this are the ones Approve and Discard save first. */
			FDateTime OpenedUtc = FDateTime::MinValue();
			/** HyperAI is leaving its own sandbox, so a shutdown event is not someone else closing it. */
			bool bLeaving = false;
			/** A sandbox from an earlier session waits for the user to resume or discard it. */
			bool bAwaitingDecision = false;
			int32 HeldCount = 0;
			FDelegateHandle PostStartup;
			FDelegateHandle PreShutdown;
			FDelegateHandle PostShutdown;
			FDelegateHandle EndPIE;
			FDelegateHandle FilesChanged;
			FSC::ISandboxInstance* Watched = nullptr;
			TWeakPtr<SNotificationItem> LeftoverNotice;
		};

		FStateData& State()
		{
			static FStateData Data;
			return Data;
		}

		FSimpleMulticastDelegate& Changed()
		{
			static FSimpleMulticastDelegate Delegate;
			return Delegate;
		}

		void Notify(const FText& Text)
		{
			FNotificationInfo Info(Text);
			Info.ExpireDuration = 8.0f;
			FSlateNotificationManager::Get().AddNotification(Info);
		}

		FSC::ISandboxManager* Manager()
		{
			return FSC::IFileSandboxCoreModule::IsAvailable() ? &FSC::IFileSandboxCoreModule::Get().GetSandboxManager() : nullptr;
		}

		bool IsOurs(const FSC::ISandboxInstance& Instance)
		{
			return Instance.GetInitialMetaData().Tags.Contains(HyperAITag);
		}

		FSC::ISandboxInstance* OurInstance()
		{
			FSC::ISandboxManager* SandboxManager = Manager();
			FSC::ISandboxInstance* Active = SandboxManager ? SandboxManager->GetActiveSandboxInstance() : nullptr;
			return Active && IsOurs(*Active) ? Active : nullptr;
		}

		bool IsModeOn()
		{
			const FStateData& Data = State();
			if (Data.ModeOverride.IsSet())
			{
				return Data.ModeOverride.GetValue();
			}
			const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
			return Settings && Settings->bSandboxMode;
		}

		bool IsPlaying()
		{
			return GEditor && (GEditor->PlayWorld != nullptr || GEditor->IsPlaySessionInProgress());
		}

		FString KindOf(const FSC::ESandboxFileChange Change)
		{
			switch (Change)
			{
			case FSC::ESandboxFileChange::Added: return TEXT("added");
			case FSC::ESandboxFileChange::Removed: return TEXT("removed");
			case FSC::ESandboxFileChange::Edited: return TEXT("edited");
			default: return TEXT("changed");
			}
		}

		TArray<FChange> ChangesOf(const FSC::ISandboxInstance& Instance)
		{
			TArray<FChange> Out;
			const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			Instance.EnumerateFileChanges([&Out, &ProjectDir](const FSC::FSandboxedFileChangeInfo& Info)
			{
				FString Path = FPaths::ConvertRelativePathToFull(Info.Path);
				FPaths::MakePathRelativeTo(Path, *ProjectDir);
				Out.Add({Path, KindOf(Info.Action)});
				return FSC::EBreakBehavior::Continue;
			}, FSC::EFileEnumerationFlags::None);
			Out.Sort([](const FChange& A, const FChange& B) { return A.Path < B.Path; });
			return Out;
		}

		void Broadcast()
		{
			const FSC::ISandboxInstance* Ours = OurInstance();
			State().HeldCount = Ours ? ChangesOf(*Ours).Num() : 0;
			Changed().Broadcast();
		}

		void Watch(FSC::ISandboxInstance& Instance)
		{
			FStateData& Data = State();
			if (Data.Watched == &Instance)
			{
				return;
			}
			if (Data.Watched && Data.FilesChanged.IsValid())
			{
				Data.Watched->OnSandboxedFilesChanged().Remove(Data.FilesChanged);
			}
			Data.Watched = &Instance;
			Data.FilesChanged = Instance.OnSandboxedFilesChanged().AddLambda([]() { Broadcast(); });
		}

		void Unwatch(FSC::ISandboxInstance& Instance)
		{
			FStateData& Data = State();
			if (Data.Watched == &Instance)
			{
				Instance.OnSandboxedFilesChanged().Remove(Data.FilesChanged);
				Data.FilesChanged.Reset();
				Data.Watched = nullptr;
			}
		}

		/** Opens HyperAI's sandbox, or confirms it is the active one. */
		bool Open(FString& OutError)
		{
			OutError.Reset();
			FSC::ISandboxManager* SandboxManager = Manager();
			if (!SandboxManager)
			{
				OutError = TEXT("The File Sandbox plugin is not loaded, so Sandbox Mode cannot hold edits.");
				return false;
			}
			if (FSC::ISandboxInstance* Active = SandboxManager->GetActiveSandboxInstance())
			{
				if (IsOurs(*Active))
				{
					return true;
				}
				OutError = FString::Printf(TEXT("Another sandbox (%s) is active. HyperAI never takes over someone else's sandbox; leave it first."),
					*Active->GetInitialMetaData().Name);
				return false;
			}
			if (IsPlaying())
			{
				OutError = TEXT("Play In Editor is running; Sandbox Mode opens its sandbox when it stops.");
				return false;
			}
			if (State().bAwaitingDecision)
			{
				OutError = TEXT("A HyperAI sandbox from an earlier session is waiting: resume or discard it first.");
				return false;
			}
			// The name is the sandbox's folder, so it must be new; approving and reopening can happen within a second.
			const FString Stamp = FString::Printf(TEXT("HyperAI %s"), *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H.%M.%S")));
			FString Name = Stamp;
			for (int32 Suffix = 2; IFileManager::Get().DirectoryExists(*FPaths::Combine(FSC::GetBaseSandboxDirectory(), Name)); ++Suffix)
			{
				Name = FString::Printf(TEXT("%s-%d"), *Stamp, Suffix);
			}
			FFileSandboxCore_SandboxMetaData MetaData(Name,
				TEXT("Everything saved while HyperAI's Sandbox Mode is on, held for review in HyperAI's Activity panel."));
			MetaData.Tags = {SandboxedEditingTag, HyperAITag};
			const FSC::FNewSandboxResult Result = SandboxManager->CreateNewSandbox(FSC::FNewSandboxArgs(MoveTemp(MetaData)));
			if (!Result.HasValue())
			{
				OutError = TEXT("The File Sandbox could not create a sandbox; the Output Log says why.");
				return false;
			}
			State().OpenedUtc = FDateTime::UtcNow();
			// Not the result's own pointer conversion: in UE 5.8 it does not compile.
			UE_LOG(LogHyperAIStudioSandbox, Log, TEXT("Sandbox Mode opened %s"), Result.GetValue()->GetRootDirectory());
			Broadcast();
			return true;
		}

		void OpenNextTick()
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				FString Error;
				if (IsModeOn() && !Open(Error))
				{
					UE_LOG(LogHyperAIStudioSandbox, Verbose, TEXT("Sandbox Mode is waiting: %s"), *Error);
				}
				return false;
			}));
		}

		/** Leaves HyperAI's sandbox, removing its folder when nothing is left in it. Someone else's is never left. */
		bool LeaveAndRemove(FString& OutError)
		{
			FSC::ISandboxManager* SandboxManager = Manager();
			FSC::ISandboxInstance* Active = OurInstance();
			if (!Active)
			{
				return true;
			}
			const FString Root = Active->GetRootDirectory();
			const bool bEmpty = !Active->HasFileChanges();
			TGuardValue<bool> Leaving(State().bLeaving, true);
			if (!SandboxManager->LeaveSandbox())
			{
				OutError = TEXT("The sandbox is locked by something else and cannot be left right now.");
				return false;
			}
			if (bEmpty)
			{
				SandboxManager->DeleteSandbox(FSC::FDeleteSandboxByDirectoryArgs(Root));
			}
			return true;
		}

		/**
		 * Unsaved agent edits are not in the sandbox yet, so they are saved into it before a decision. Only packages agents
		 * touched: the user's own unsaved work stays as it is.
		 */
		void SaveAgentEdits()
		{
			TArray<UPackage*> Content;
			bool bLevels = false;
			for (const FHyperAIStudioActivityEntry& Entry : FHyperAIStudioAgentActivityLog::GetSnapshot())
			{
				if (Entry.Utc < State().OpenedUtc || Entry.Target.IsEmpty())
				{
					continue;
				}
				const FString PackageName = FPackageName::ObjectPathToPackageName(Entry.Target);
				// A target is a tool's text, an agent's at times: FindPackage makes an FName of it.
				UPackage* Package = FHyperAIStudioNames::IsObjectPath(PackageName, nullptr) && FPackageName::IsValidLongPackageName(PackageName)
					? FindPackage(nullptr, *PackageName) : nullptr;
				if (!Package)
				{
					continue;
				}
				if (Package->ContainsMap())
				{
					bLevels = true;
				}
				else if (Package->IsDirty())
				{
					Content.AddUnique(Package);
				}
			}
			if (!Content.IsEmpty())
			{
				UEditorLoadingAndSavingUtils::SavePackages(Content, /*bOnlyDirty=*/true);
			}
			// A level edit under One File Per Actor dirties actor packages that only a map save reaches.
			if (bLevels)
			{
				UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages=*/true, /*bSaveContentPackages=*/false);
			}
		}

		/**
		 * Closing a sandbox reloads every unsaved asset from disk (the File Sandbox's design, so nothing built on
		 * sandboxed files lingers), which would silently drop unsaved work. HyperAI never closes one over it.
		 */
		TArray<FString> UnsavedPackageNames()
		{
			TArray<UPackage*> Packages;
			UEditorLoadingAndSavingUtils::GetDirtyMapPackages(Packages);
			UEditorLoadingAndSavingUtils::GetDirtyContentPackages(Packages);
			TArray<FString> Names;
			for (const UPackage* Package : Packages)
			{
				if (Package)
				{
					Names.AddUnique(Package->GetName());
				}
			}
			return Names;
		}

		FString NameList(const TArray<FString>& Names)
		{
			constexpr int32 Shown = 5;
			FString List = FString::Join(TArrayView<const FString>(Names).Left(Shown), TEXT(", "));
			if (Names.Num() > Shown)
			{
				List += FString::Printf(TEXT(" and %d more"), Names.Num() - Shown);
			}
			return List;
		}

		bool CanDecide(FSC::ISandboxInstance*& OutOurs, FString& OutError)
		{
			OutOurs = OurInstance();
			if (!OutOurs)
			{
				OutError = TEXT("HyperAI's sandbox is not active.");
				return false;
			}
			if (IsPlaying())
			{
				OutError = TEXT("Stop Play In Editor first: the game is running on the sandboxed files.");
				return false;
			}
			if (FHyperAIStudioExecutionQueue::Num() > 0 || FHyperAIStudioExecutionQueue::IsRunning())
			{
				OutError = TEXT("Agent edits are running or waiting to run; decide once they have finished.");
				return false;
			}
			return true;
		}

		/** A sandbox from an earlier session still holds changes: the user resumes or discards it. */
		void OfferLeftovers()
		{
			FSC::ISandboxManager* SandboxManager = Manager();
			if (!SandboxManager)
			{
				return;
			}
			const FSC::ISandboxInstance* Active = SandboxManager->GetActiveSandboxInstance();
			const FString ActiveRoot = Active ? FString(Active->GetRootDirectory()) : FString();
			struct FLeftover
			{
				FString Root;
				FDateTime Modified;
			};
			TArray<FLeftover> Leftovers;
			FSC::IFileSandboxCoreModule::Get().GetDefaultSandboxRepository().ForEachSandbox(
				[&Leftovers, &ActiveRoot](const FString& Root, const FSC::FSandboxMetaInfo& Info)
			{
				if (Info.UserMetaData.Tags.Contains(HyperAITag) && Root != ActiveRoot)
				{
					Leftovers.Add({Root, Info.LastModified});
				}
				return FSC::EBreakBehavior::Continue;
			});
			int32 Changes = 0;
			for (int32 Index = Leftovers.Num() - 1; Index >= 0; --Index)
			{
				int32 Count = 0;
				SandboxManager->EnumerateFileChanges(Leftovers[Index].Root, [&Count](const FSC::FSandboxedFileChangeInfo&)
				{
					++Count;
					return FSC::EBreakBehavior::Continue;
				});
				if (Count == 0)
				{
					SandboxManager->DeleteSandbox(FSC::FDeleteSandboxByDirectoryArgs(Leftovers[Index].Root));
					Leftovers.RemoveAt(Index);
				}
				Changes += Count;
			}
			if (Leftovers.IsEmpty() || !FSlateApplication::IsInitialized())
			{
				return;
			}
			Leftovers.Sort([](const FLeftover& A, const FLeftover& B) { return A.Modified > B.Modified; });
			State().bAwaitingDecision = true;
			const FString Newest = Leftovers[0].Root;
			TArray<FString> Roots;
			for (const FLeftover& Leftover : Leftovers)
			{
				Roots.Add(Leftover.Root);
			}
			FNotificationInfo Info(FText::Format(LOCTEXT("LeftoverSandbox",
				"HyperAI's Sandbox Mode is still holding {0} {0}|plural(one=change,other=changes) from an earlier session. Resume Sandbox to keep reviewing them, or discard them. Your chats are not affected."),
				Changes));
			Info.bFireAndForget = false;
			Info.ExpireDuration = 0.0f;
			// Named for the sandbox: after a restart it sits beside chats that resume, and a bare Resume read as theirs.
			Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("ResumeSandbox", "Resume Sandbox"),
				LOCTEXT("ResumeSandboxTooltip", "Open that sandbox again; its changes show in HyperAI's Activity panel."),
				FSimpleDelegate::CreateLambda([Newest]()
			{
				State().bAwaitingDecision = false;
				if (FSC::ISandboxManager* Loader = Manager(); Loader && !Loader->HasActiveSandbox())
				{
					if (Loader->LoadSandbox(FSC::FLoadSandboxByDirectoryArgs(Newest)).HasValue())
					{
						State().OpenedUtc = FDateTime::UtcNow();
					}
				}
				if (const TSharedPtr<SNotificationItem> Notice = State().LeftoverNotice.Pin())
				{
					Notice->ExpireAndFadeout();
				}
				Broadcast();
			}), SNotificationItem::CS_None));
			Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("DiscardSandbox", "Discard"),
				LOCTEXT("DiscardSandboxTooltip", "Throw those held changes away; the project keeps its files as they are."),
				FSimpleDelegate::CreateLambda([Roots]()
			{
				State().bAwaitingDecision = false;
				if (FSC::ISandboxManager* Deleter = Manager())
				{
					for (const FString& Root : Roots)
					{
						Deleter->DeleteSandbox(FSC::FDeleteSandboxByDirectoryArgs(Root));
					}
				}
				if (const TSharedPtr<SNotificationItem> Notice = State().LeftoverNotice.Pin())
				{
					Notice->ExpireAndFadeout();
				}
				OpenNextTick();
				Broadcast();
			}), SNotificationItem::CS_None));
			State().LeftoverNotice = FSlateNotificationManager::Get().AddNotification(Info);
		}
	}

	void Startup()
	{
		FStateData& Data = State();
		if (Data.bStarted)
		{
			return;
		}
		Data.bStarted = true;
		if (FSC::ISandboxManager* SandboxManager = Manager())
		{
			Data.PostStartup = SandboxManager->OnPostSandboxStartup().AddLambda([](FSC::ISandboxInstance& Instance)
			{
				Watch(Instance);
				Broadcast();
			});
			Data.PreShutdown = SandboxManager->OnPreSandboxShutdown().AddLambda([](FSC::ISandboxInstance& Instance)
			{
				Unwatch(Instance);
			});
			Data.PostShutdown = SandboxManager->OnPostSandboxShutdown().AddLambda([]()
			{
				Broadcast();
				// Someone else closed HyperAI's sandbox (Epic's Persist All, say): keep holding edits in a fresh one.
				if (!State().bLeaving && IsModeOn())
				{
					OpenNextTick();
				}
			});
			if (FSC::ISandboxInstance* Active = SandboxManager->GetActiveSandboxInstance())
			{
				Watch(*Active);
			}
		}
		Data.EndPIE = FEditorDelegates::EndPIE.AddLambda([](bool)
		{
			if (IsModeOn())
			{
				OpenNextTick();
			}
		});
		OfferLeftovers();
		if (IsModeOn())
		{
			FString Error;
			if (!Open(Error))
			{
				UE_LOG(LogHyperAIStudioSandbox, Log, TEXT("Sandbox Mode is waiting: %s"), *Error);
			}
		}
	}

	void Shutdown()
	{
		FStateData& Data = State();
		if (!Data.bStarted)
		{
			return;
		}
		Data.bStarted = false;
		FEditorDelegates::EndPIE.Remove(Data.EndPIE);
		if (FSC::ISandboxManager* SandboxManager = Manager())
		{
			SandboxManager->OnPostSandboxStartup().Remove(Data.PostStartup);
			SandboxManager->OnPreSandboxShutdown().Remove(Data.PreShutdown);
			SandboxManager->OnPostSandboxShutdown().Remove(Data.PostShutdown);
			if (FSC::ISandboxInstance* Active = SandboxManager->GetActiveSandboxInstance())
			{
				Unwatch(*Active);
			}
		}
		// A sandbox still holding changes stays on disk and is offered again at the next start.
	}

	void OnModeChanged()
	{
		if (IsModeOn())
		{
			FString Error;
			if (!Open(Error))
			{
				Notify(FText::FromString(Error));
			}
			Broadcast();
			return;
		}
		FSC::ISandboxInstance* Ours = OurInstance();
		const bool bHolding = Ours && Ours->HasFileChanges();
		if (Ours && !State().ModeOverride.IsSet() && (bHolding || !UnsavedPackageNames().IsEmpty()))
		{
			// Turning the mode off never decides for the user, and never closes the sandbox over unsaved work.
			UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
			Settings->bSandboxMode = true;
			Settings->SaveConfig();
			Notify(bHolding
				? LOCTEXT("ModeStaysOn", "Sandbox Mode stays on until its held changes are approved or discarded in HyperAI's Activity panel.")
				: LOCTEXT("ModeStaysOnUnsaved", "Sandbox Mode stays on while assets have unsaved changes: closing the sandbox would reload them from disk. Save them (they are held for review) or revert them, then turn it off."));
			Broadcast();
			return;
		}
		FString Error;
		LeaveAndRemove(Error);
		Broadcast();
	}

	FStatus GetStatus(const bool bWithChanges)
	{
		FStatus Status;
		FSC::ISandboxManager* SandboxManager = Manager();
		FSC::ISandboxInstance* Active = SandboxManager ? SandboxManager->GetActiveSandboxInstance() : nullptr;
		if (Active && !IsOurs(*Active))
		{
			Status.State = EState::Foreign;
			Status.SandboxName = Active->GetInitialMetaData().Name;
			Status.Diagnostic = IsModeOn()
				? TEXT("Another sandbox is active, so HyperAI refuses agent edits until it is left.")
				: TEXT("Another sandbox is active; what is saved goes there.");
			return Status;
		}
		if (Active)
		{
			Status.State = EState::Active;
			Status.SandboxName = Active->GetInitialMetaData().Name;
			if (bWithChanges)
			{
				Status.Changes = ChangesOf(*Active);
			}
			Status.Diagnostic = TEXT("Everything saved in the editor is held here until it is approved or discarded, your own saves included. "
				"Files written outside the editor, such as C++ source and Config, are not held.");
			return Status;
		}
		if (!IsModeOn())
		{
			return Status;
		}
		if (!SandboxManager)
		{
			Status.State = EState::Unavailable;
			Status.Diagnostic = TEXT("The File Sandbox plugin is not loaded.");
			return Status;
		}
		Status.State = EState::Waiting;
		Status.Diagnostic = IsPlaying() ? TEXT("Play In Editor is running; the sandbox opens when it stops.")
			: State().bAwaitingDecision ? TEXT("A sandbox from an earlier session waits for Resume or Discard.")
			: TEXT("The sandbox opens at the next agent edit.");
		return Status;
	}

	bool IsHoldingEdits()
	{
		return OurInstance() != nullptr;
	}

	int32 GetHeldCount()
	{
		return State().HeldCount;
	}

	bool CheckAgentEdit(FString& OutError)
	{
		OutError.Reset();
		return !IsModeOn() || Open(OutError);
	}

	bool ApproveAll(FString& OutError)
	{
		OutError.Reset();
		FSC::ISandboxInstance* Ours = nullptr;
		if (!CanDecide(Ours, OutError))
		{
			return false;
		}
		SaveAgentEdits();
		if (const TArray<FString> Unsaved = UnsavedPackageNames(); !Unsaved.IsEmpty())
		{
			OutError = TEXT("Save or revert these unsaved assets first; closing the sandbox would reload them from disk: ") + NameList(Unsaved);
			return false;
		}
		if (!Ours->PersistAll())
		{
			OutError = TEXT("Some files could not be written to the project, for example because they are read-only or checked out elsewhere. Everything stays in the sandbox; the Output Log names the files.");
			return false;
		}
		if (!LeaveAndRemove(OutError))
		{
			return false;
		}
		if (IsModeOn())
		{
			FString Ignored;
			Open(Ignored);
		}
		Broadcast();
		return true;
	}

	bool DiscardAll(FString& OutError)
	{
		OutError.Reset();
		FSC::ISandboxInstance* Ours = nullptr;
		if (!CanDecide(Ours, OutError))
		{
			return false;
		}
		// Unsaved agent edits go into the sandbox first, so they are reverted with it instead of lingering in memory.
		SaveAgentEdits();
		const FSC::FRevertResult Reverted = Ours->RevertAll();
		const auto Names = [](const TArray<FName>& Packages)
		{
			TArray<FString> Out;
			Algo::Transform(Packages, Out, [](const FName Package) { return Package.ToString(); });
			return NameList(Out);
		};
		UE_LOG(LogHyperAIStudioSandbox, Log, TEXT("Sandbox discarded. Reloaded: %s. Purged: %s."),
			*Names(Reverted.PackagesPendingHotReload), *Names(Reverted.PackagesPendingPurge));
		if (!LeaveAndRemove(OutError))
		{
			return false;
		}
		if (IsModeOn())
		{
			FString Ignored;
			Open(Ignored);
		}
		Broadcast();
		return true;
	}

	FSimpleMulticastDelegate& OnChanged()
	{
		return Changed();
	}

#if WITH_DEV_AUTOMATION_TESTS
	void SetModeForTests(const TOptional<bool> bMode)
	{
		State().ModeOverride = bMode;
	}
#endif
}

#undef LOCTEXT_NAMESPACE
