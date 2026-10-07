// Games by Hyper 2026.

#include "HyperAIStudioVerify.h"

#include "AI/NavigationSystemBase.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioNames.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioVerify, Log, All);

namespace HyperAIStudio::Verify
{
	namespace
	{
		/** No further case world starts with less free memory than this. */
		constexpr uint64 MinFreeBytes = 1024ull * 1024ull * 1024ull;
		constexpr int32 MaxStepsPerCase = 32;

		struct FCaseWorld
		{
			FCase Spec;
			FCaseOutcome Outcome;
			UGameInstance* Instance = nullptr;
			FWorldContext* Context = nullptr;
			UWorld* World = nullptr;
			TUniquePtr<FHyperAIBotExecutor> Bot;
			int32 NextStep = 0;
			bool bRunning = false;
		};

		/** What the Activity panel's job reads; the batch itself may be gone by then. */
		struct FJobState
		{
			FString Progress;
			bool bDone = false;
			bool bFailed = false;
			FString Diagnostic;
		};

		struct FBatch
		{
			TArray<TUniquePtr<FCaseWorld>> Cases;
			int32 Created = 0;
			double StartedAt = 0.0;
			double Deadline = 0.0;
			int32 Limit = 0;
			FString Map;
			FString Output;
			TFunction<void(const FBatchOutcome&)> OnDone;
			FTSTicker::FDelegateHandle Ticker;
			FDelegateHandle PreBeginPIE;
			FDelegateHandle Throttle;
			TSharedRef<FJobState> Job = MakeShared<FJobState>();
		};

		TUniquePtr<FBatch>& Current()
		{
			static TUniquePtr<FBatch> Batch;
			return Batch;
		}

		TFunction<void(UWorld&, const FString&)>& CaseSetup()
		{
			static TFunction<void(UWorld&, const FString&)> Setup;
			return Setup;
		}

		UWorld* EditorWorldOf()
		{
			return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		}

		FString CurrentMap()
		{
			const UWorld* World = EditorWorldOf();
			return World ? World->GetOutermost()->GetName() : FString();
		}

		/** Steps that need a game viewport or UI, which case worlds do not have. Captures render the player's camera instead. */
		bool NeedsViewport(const EHyperAIBotAction Action)
		{
			switch (Action)
			{
			case EHyperAIBotAction::AssertOnScreen:
			case EHyperAIBotAction::UiClick:
			case EHyperAIBotAction::UiType:
			case EHyperAIBotAction::UiAssert:
				return true;
			default:
				return false;
			}
		}

		FGameInstancePIEParameters PlayParameters()
		{
			FGameInstancePIEParameters Params;
			Params.bSimulateInEditor = false;
			Params.bAnyBlueprintErrors = false;
			Params.bStartInSpectatorMode = false;
			Params.bRunAsDedicated = false;
			Params.bIsPrimaryPIEClient = false;
			Params.NetMode = EPlayNetMode::PIE_Standalone;
			Params.EditorPlaySettings = GetMutableDefault<ULevelEditorPlaySettings>();
			return Params;
		}

		/** The project's own game instance class, as Play In Editor uses. */
		UClass* GameInstanceClass()
		{
			FString Path;
			GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameInstanceClass"), Path, GEngineIni);
			UClass* Class = Path.IsEmpty() ? nullptr : FSoftClassPath(Path).TryLoadClass<UGameInstance>();
			return Class ? Class : UGameInstance::StaticClass();
		}

		/**
		 * A copy of the open level through the editor's own play path, with a local player that has no window. It is
		 * a PIE world the editor loop leaves alone: the batch ticks it. GWorld is the play world while it starts, as
		 * for any PIE instance, and the editor world again after; GWorld is null right after the copy is made, so it is
		 * restored to the editor world, never to a saved value.
		 */
		bool CreateWorld(FCaseWorld& Case, const int32 Instance, const FString& Output, FString& OutError)
		{
			const FGameInstancePIEParameters Params = PlayParameters();
			Case.Instance = NewObject<UGameInstance>(GEngine, GameInstanceClass());
			Case.Instance->AddToRoot();
			const FGameInstancePIEResult Initialized = Case.Instance->InitializeForPlayInEditor(Instance, Params);
			Case.Context = Case.Instance->GetWorldContext();
			Case.World = Case.Context ? Case.Context->World() : nullptr;
			if (!Initialized.IsSuccess() || !Case.World)
			{
				OutError = TEXT("The level could not be copied: ") + Initialized.FailureReason.ToString();
				return false;
			}
			Case.World->SetShouldTick(false);
			SetPlayInEditorWorld(Case.World);
			UGameViewportClient* Viewport = NewObject<UGameViewportClient>(GEngine, GEngine->GameViewportClientClass);
			Viewport->Init(*Case.Context, Case.Instance, /*bCreateNewAudioDevice=*/false);
			Case.Context->GameViewport = Viewport;
			ULocalPlayer* Player = Viewport->SetupInitialLocalPlayer(OutError);
			const FGameInstancePIEResult Started = Player
				? Case.Instance->StartPlayInEditorGameInstance(Player, Params)
				: FGameInstancePIEResult::Failure(FText::FromString(TEXT("No local player: ") + OutError));
			GEngine->BlockTillLevelStreamingCompleted(Case.World);
			RestoreEditorWorld(GEditor->EditorWorld);
			if (!Started.IsSuccess())
			{
				OutError = TEXT("The game did not start: ") + Started.FailureReason.ToString();
				return false;
			}
			if (!Player->GetPlayerController(Case.World) || !Player->GetPlayerController(Case.World)->GetPawn())
			{
				OutError = TEXT("The game started without a player pawn; check the level's game mode and player start.");
				return false;
			}
			// Every frame the engine removes local players whose viewport client has no screen, pawns and all. The
			// copies have none, so once the game has started the player keeps its controller and input but no client.
			Player->ViewportClient = nullptr;
			Case.Context->GameViewport = nullptr;
			if (CaseSetup())
			{
				SetPlayInEditorWorld(Case.World);
				CaseSetup()(*Case.World, Case.Outcome.Name);
				RestoreEditorWorld(GEditor->EditorWorld);
			}
			Case.Bot = MakeUnique<FHyperAIBotExecutor>(*Case.World, Output / FString::FromInt(Instance));
			return true;
		}

		void End(FCaseWorld& Case, const TCHAR* Status, const FString& Diagnostic)
		{
			if (Case.Bot)
			{
				Case.Bot->Stop();
			}
			Case.bRunning = false;
			Case.Outcome.Status = Status;
			Case.Outcome.Diagnostic = Diagnostic.Left(1024);
		}

		/** Records every step that has finished and begins the next, until one is still running or the case ended. */
		void Settle(FCaseWorld& Case, FHyperAIBotResult Result)
		{
			while (Case.bRunning && Result.IsDone())
			{
				const bool bPassed = Result.State == FHyperAIBotResult::EState::Passed;
				FStepOutcome& Step = Case.Outcome.Steps.AddDefaulted_GetRef();
				Step.Action = FHyperAIBotExecutor::ActionName(Case.Spec.Steps[Case.NextStep].Action);
				Step.Status = bPassed ? TEXT("passed") : TEXT("failed");
				Step.Code = Result.Code;
				Step.Detail = Result.Detail.Left(1500);
				Step.File = Result.File;
				Step.AtSeconds = Case.Outcome.SimulatedSeconds;
				if (!bPassed)
				{
					Case.Outcome.FailedStep = Case.NextStep;
					End(Case, TEXT("failed"), FString::Printf(TEXT("Step %d (%s) failed with %s: %s"),
						Case.NextStep + 1, *Step.Action, *Result.Code, *Result.Detail.Left(400)));
					return;
				}
				if (++Case.NextStep >= Case.Spec.Steps.Num())
				{
					End(Case, TEXT("passed"), FString::Printf(TEXT("All %d steps passed in %.1f simulated seconds."),
						Case.Spec.Steps.Num(), Case.Outcome.SimulatedSeconds));
					return;
				}
				Result = Case.Bot->Begin(Case.Spec.Steps[Case.NextStep]);
			}
		}

		void Begin(FCaseWorld& Case)
		{
			Case.bRunning = true;
			SetPlayInEditorWorld(Case.World);
			Settle(Case, Case.Bot->Begin(Case.Spec.Steps[0]));
			RestoreEditorWorld(GEditor->EditorWorld);
		}

		/** One fixed step of the case's world, then its bot. */
		void Step(FCaseWorld& Case)
		{
			SetPlayInEditorWorld(Case.World);
			Case.World->Tick(LEVELTICK_All, FrameSeconds);
			Case.Outcome.SimulatedSeconds += FrameSeconds;
			Settle(Case, Case.Bot->Tick());
			RestoreEditorWorld(GEditor->EditorWorld);
			if (Case.bRunning && Case.Outcome.SimulatedSeconds >= Case.Spec.TimeoutSeconds)
			{
				End(Case, TEXT("timed_out"), FString::Printf(TEXT("The case used its %.0f simulated seconds during step %d (%s)."),
					Case.Spec.TimeoutSeconds, Case.NextStep + 1, *FHyperAIBotExecutor::ActionName(Case.Spec.Steps[Case.NextStep].Action)));
			}
		}

		/** The editor's own PIE teardown for each copy, then what its EndPlayMap does after it. */
		void TearDown(FBatch& Batch)
		{
			TArray<UWorld*> Worlds;
			for (const TUniquePtr<FCaseWorld>& Case : Batch.Cases)
			{
				Case->Bot.Reset();
				if (Case->Context && Case->World)
				{
					Worlds.Add(Case->World);
					GEditor->TeardownPlaySession(*Case->Context);
					GEngine->ShutdownWorldNetDriver(Case->World);
				}
			}
			for (const TUniquePtr<FCaseWorld>& Case : Batch.Cases)
			{
				if (Case->World)
				{
					GEngine->DestroyWorldContext(Case->World);
				}
				if (Case->Instance)
				{
					Case->Instance->RemoveFromRoot();
				}
				Case->World = nullptr;
				Case->Context = nullptr;
				Case->Instance = nullptr;
			}
			for (UWorld* World : Worlds)
			{
				World->RemoveFromRoot();
				World->MarkObjectsPendingKill();
			}
			for (FThreadSafeObjectIterator It; It; ++It)
			{
				if (It->GetOutermost()->HasAnyPackageFlags(PKG_PlayInEditor) && It->HasAnyFlags(RF_Standalone))
				{
					It->ClearFlags(RF_Standalone);
				}
			}
			if (GEditor->EditorWorld)
			{
				FNavigationSystem::OnPIEEnd(*GEditor->EditorWorld);
			}
			GEditor->EditorWorld = nullptr;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}

		FString CalibrationPath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio") / TEXT("VerifyCalibration.json"));
		}

		TSharedPtr<FJsonObject> LoadLevels()
		{
			FString Text;
			TSharedPtr<FJsonObject> Root;
			if (FFileHelper::LoadFileToString(Text, *CalibrationPath()))
			{
				FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root);
			}
			const TSharedPtr<FJsonObject>* Levels = nullptr;
			return Root.IsValid() && Root->TryGetObjectField(TEXT("levels"), Levels) ? *Levels : MakeShared<FJsonObject>();
		}

		void SaveLevel(const FString& Map, const FCalibration& Calibration)
		{
			const TSharedPtr<FJsonObject> Levels = LoadLevels();
			const TSharedRef<FJsonObject> Level = MakeShared<FJsonObject>();
			Level->SetNumberField(TEXT("limit"), Calibration.Limit);
			Level->SetStringField(TEXT("measuredUtc"), Calibration.MeasuredUtc);
			const TSharedRef<FJsonObject> Seconds = MakeShared<FJsonObject>();
			for (const TPair<int32, double>& Pair : Calibration.SecondsByCount)
			{
				Seconds->SetNumberField(FString::FromInt(Pair.Key), Pair.Value);
			}
			Level->SetObjectField(TEXT("secondsByCount"), Seconds);
			Levels->SetObjectField(Map, Level);
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetObjectField(TEXT("levels"), Levels);
			FString Text;
			FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
			FFileHelper::SaveStringToFile(Text, *CalibrationPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		FCalibration ReadCalibration(const FString& Map)
		{
			FCalibration Calibration;
			Calibration.Map = Map;
			Calibration.Status = TEXT("not_calibrated");
			// Held here: Level points into this object.
			const TSharedPtr<FJsonObject> Levels = LoadLevels();
			const TSharedPtr<FJsonObject>* Level = nullptr;
			if (!Levels->TryGetObjectField(Map, Level))
			{
				Calibration.Diagnostic = FString::Printf(TEXT("Not calibrated for this level yet; batches hold up to %d cases until hyper_verify_calibrate measures it."), DefaultLimit);
				return Calibration;
			}
			Calibration.Status = TEXT("calibrated");
			Calibration.Limit = FMath::Clamp(static_cast<int32>((*Level)->GetNumberField(TEXT("limit"))), 1, MaxCases);
			(*Level)->TryGetStringField(TEXT("measuredUtc"), Calibration.MeasuredUtc);
			const TSharedPtr<FJsonObject>* Seconds = nullptr;
			if ((*Level)->TryGetObjectField(TEXT("secondsByCount"), Seconds))
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Seconds)->Values)
				{
					Calibration.SecondsByCount.Add(FCString::Atoi(*Pair.Key), Pair.Value->AsNumber());
				}
			}
			return Calibration;
		}

		void Finish(const TCHAR* Status, const FString& Diagnostic)
		{
			// Taken out first, so OnDone can start the next batch.
			TUniquePtr<FBatch> Batch = MoveTemp(Current());
			if (!Batch)
			{
				return;
			}
			FTSTicker::GetCoreTicker().RemoveTicker(Batch->Ticker);
			FEditorDelegates::PreBeginPIE.Remove(Batch->PreBeginPIE);
			if (GEditor)
			{
				const FDelegateHandle Throttle = Batch->Throttle;
				GEditor->ShouldDisableCPUThrottlingDelegates.RemoveAll(
					[Throttle](const UEditorEngine::FShouldDisableCPUThrottling& Delegate) { return Delegate.GetHandle() == Throttle; });
			}
			const bool bDeadline = FCString::Strcmp(Status, TEXT("deadline")) == 0;
			int32 Cut = 0;
			for (const TUniquePtr<FCaseWorld>& Case : Batch->Cases)
			{
				if (Case->bRunning)
				{
					++Cut;
					End(*Case, bDeadline ? TEXT("timed_out") : TEXT("stopped"), bDeadline
						? FString::Printf(TEXT("Stopped at the batch deadline during step %d, after %.1f simulated seconds."), Case->NextStep + 1, Case->Outcome.SimulatedSeconds)
						: Diagnostic);
				}
				else if (Case->Outcome.Status.IsEmpty())
				{
					++Cut;
					Case->Outcome.Status = TEXT("not_started");
					Case->Outcome.Diagnostic = bDeadline ? FString(TEXT("The deadline came before it could start.")) : Diagnostic;
				}
			}
			TearDown(*Batch);

			FBatchOutcome Outcome;
			Outcome.Status = Status;
			Outcome.Diagnostic = Diagnostic;
			Outcome.Map = Batch->Map;
			Outcome.Limit = Batch->Limit;
			Outcome.WallSeconds = FPlatformTime::Seconds() - Batch->StartedAt;
			for (const TUniquePtr<FCaseWorld>& Case : Batch->Cases)
			{
				Outcome.Cases.Add(Case->Outcome);
			}
			// A batch the deadline cut short while its cases played slower than real time was more than this machine
			// runs in a minute, so the next ones get fewer. A case that never ends at full speed says nothing about that.
			double Simulated = 0.0;
			int32 Played = 0;
			for (const TUniquePtr<FCaseWorld>& Case : Batch->Cases)
			{
				Simulated += Case->Outcome.SimulatedSeconds;
				Played += Case->Outcome.SimulatedSeconds > 0.f;
			}
			const double Speed = Played > 0 && Outcome.WallSeconds > 0.0 ? Simulated / Played / Outcome.WallSeconds : 0.0;
			if (bDeadline && Cut > 0 && Batch->Limit > 1 && Speed < 1.0)
			{
				FCalibration Lowered = ReadCalibration(Batch->Map);
				Lowered.Limit = FMath::Max(1, Batch->Limit - 2);
				Lowered.MeasuredUtc = FDateTime::UtcNow().ToIso8601();
				SaveLevel(Batch->Map, Lowered);
				UE_LOG(LogHyperAIStudioVerify, Log, TEXT("A batch of %d missed its deadline at %.2fx real time; the limit for %s is now %d."),
					Batch->Cases.Num(), Speed, *Batch->Map, Lowered.Limit);
			}
			int32 Passed = 0;
			for (const FCaseOutcome& Case : Outcome.Cases)
			{
				Passed += Case.Status == TEXT("passed");
			}
			Batch->Job->Progress = FString::Printf(TEXT("%d of %d passed"), Passed, Outcome.Cases.Num());
			Batch->Job->Diagnostic = Diagnostic;
			Batch->Job->bFailed = !bDeadline && FCString::Strcmp(Status, TEXT("completed")) != 0;
			Batch->Job->bDone = true;
			if (Batch->OnDone)
			{
				Batch->OnDone(Outcome);
			}
		}

		bool TickBatch(float)
		{
			FBatch& Batch = *Current();
			if (FPlatformTime::Seconds() >= Batch.Deadline)
			{
				Finish(TEXT("deadline"), FString::Printf(TEXT("The batch reached its deadline after %.1f s."), FPlatformTime::Seconds() - Batch.StartedAt));
				return false;
			}
			// One new copy per frame, so a heavy level never stalls the editor for the whole batch at once.
			if (Batch.Created < Batch.Cases.Num())
			{
				FCaseWorld& Case = *Batch.Cases[Batch.Created];
				const int32 Instance = ++Batch.Created;
				FString Error;
				if (FPlatformMemory::GetStats().AvailablePhysical < MinFreeBytes)
				{
					End(Case, TEXT("not_started"), TEXT("The machine is low on free memory, so no further copy of the level started."));
				}
				else if (!CreateWorld(Case, Instance, Batch.Output, Error))
				{
					End(Case, TEXT("not_started"), Error);
				}
				else
				{
					Begin(Case);
				}
			}
			bool bAnyLeft = Batch.Created < Batch.Cases.Num();
			int32 Ended = 0;
			for (int32 Index = 0; Index < Batch.Created; ++Index)
			{
				FCaseWorld& Case = *Batch.Cases[Index];
				if (Case.bRunning)
				{
					Step(Case);
				}
				bAnyLeft |= Case.bRunning;
				Ended += !Case.bRunning;
			}
			Batch.Job->Progress = FString::Printf(TEXT("%d of %d cases done"), Ended, Batch.Cases.Num());
			if (!bAnyLeft)
			{
				Finish(TEXT("completed"), FString());
				return false;
			}
			return true;
		}

		struct FCalibrationRun
		{
			FString Map;
			TArray<int32> Counts;
			int32 Next = 0;
			TMap<int32, double> Seconds;
			TFunction<void(const FCalibration&)> OnDone;
		};

		void Conclude(const TSharedRef<FCalibrationRun>& Run, const FString& Diagnostic)
		{
			FCalibration Calibration;
			Calibration.Map = Run->Map;
			Calibration.SecondsByCount = Run->Seconds;
			Calibration.MeasuredUtc = FDateTime::UtcNow().ToIso8601();
			if (Run->Seconds.IsEmpty())
			{
				Calibration.Status = TEXT("failed");
				Calibration.Limit = ReadCalibration(Run->Map).Limit;
				Calibration.Diagnostic = Diagnostic;
			}
			else
			{
				Calibration.Status = TEXT("measured");
				Calibration.Limit = PickLimit(Run->Seconds, CalibrationBudgetSeconds);
				Calibration.Diagnostic = Diagnostic.IsEmpty()
					? FString::Printf(TEXT("Batches of up to %d cases finish within %.0f s on this machine and level."), Calibration.Limit, CalibrationBudgetSeconds)
					: Diagnostic;
				SaveLevel(Run->Map, Calibration);
			}
			Run->OnDone(Calibration);
		}

		void CalibrateNext(const TSharedRef<FCalibrationRun>& Run)
		{
			if (Run->Next >= Run->Counts.Num())
			{
				Conclude(Run, FString());
				return;
			}
			const int32 Count = Run->Counts[Run->Next++];
			TArray<FCase> Cases;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FCase& Case = Cases.AddDefaulted_GetRef();
				Case.Name = FString::Printf(TEXT("reference-%d"), Index + 1);
				FHyperAIBotStep Wait;
				Wait.Action = EHyperAIBotAction::Wait;
				Wait.Seconds = 10.f;
				Case.Steps.Add(Wait);
				Case.TimeoutSeconds = 12.f;
			}
			FString Error;
			if (!Start(MoveTemp(Cases), MaxDeadlineSeconds, [Run, Count](const FBatchOutcome& Outcome)
			{
				bool bAllRan = Outcome.Status == TEXT("completed");
				for (const FCaseOutcome& Case : Outcome.Cases)
				{
					bAllRan &= Case.Status == TEXT("passed");
				}
				// A batch that could not run to the end counts as over the budget.
				Run->Seconds.Add(Count, bAllRan ? Outcome.WallSeconds : CalibrationBudgetSeconds + Outcome.WallSeconds);
				if (!bAllRan || Outcome.WallSeconds > CalibrationBudgetSeconds)
				{
					Conclude(Run, FString::Printf(TEXT("A batch of %d took %.1f s%s, so larger ones were not tried."), Count, Outcome.WallSeconds,
						bAllRan ? TEXT("") : TEXT(" and did not finish every case")));
					return;
				}
				CalibrateNext(Run);
			}, Error))
			{
				Conclude(Run, TEXT("Calibration could not start a batch: ") + Error);
			}
		}
	}

	bool Admit(const TArray<FCase>& Cases, const float DeadlineSeconds, const int32 Limit, FString& OutError)
	{
		OutError.Reset();
		if (Cases.IsEmpty())
		{
			OutError = TEXT("Give at least one case.");
			return false;
		}
		if (Cases.Num() > Limit)
		{
			OutError = FString::Printf(TEXT("%d cases is more than this machine plays at once on this level (%d). Split them into batches, or run hyper_verify_calibrate to measure the limit again."),
				Cases.Num(), Limit);
			return false;
		}
		if (DeadlineSeconds < MinDeadlineSeconds || DeadlineSeconds > MaxDeadlineSeconds)
		{
			OutError = FString::Printf(TEXT("deadlineSeconds is %.0f to %.0f."), MinDeadlineSeconds, MaxDeadlineSeconds);
			return false;
		}
		for (int32 Index = 0; Index < Cases.Num(); ++Index)
		{
			const FCase& Case = Cases[Index];
			const FString Name = Case.Name.IsEmpty() ? FString::Printf(TEXT("case %d"), Index + 1) : Case.Name;
			if (Case.Steps.IsEmpty() || Case.Steps.Num() > MaxStepsPerCase)
			{
				OutError = FString::Printf(TEXT("%s: a case has 1 to %d steps."), *Name, MaxStepsPerCase);
				return false;
			}
			if (Case.TimeoutSeconds < 1.f || Case.TimeoutSeconds > DeadlineSeconds - ReserveSeconds)
			{
				OutError = FString::Printf(TEXT("%s: timeoutSeconds is 1 to %.0f, the deadline less %.0f s for starting and stopping."),
					*Name, DeadlineSeconds - ReserveSeconds, ReserveSeconds);
				return false;
			}
			for (int32 StepIndex = 0; StepIndex < Case.Steps.Num(); ++StepIndex)
			{
				const FHyperAIBotStep& Step = Case.Steps[StepIndex];
				FString StepError;
				if (NeedsViewport(Step.Action))
				{
					OutError = FString::Printf(TEXT("%s, step %d: %s needs a game viewport, which verification cases do not have. Check it with hyper_playtest_run."),
						*Name, StepIndex + 1, *FHyperAIBotExecutor::ActionName(Step.Action));
					return false;
				}
				// The bot makes FNames of its target, property and widget names, and loads an input action given by its path.
				if (!FHyperAIBotExecutor::Validate(Step, StepError)
					|| !FHyperAIStudioNames::IsName(Step.Target, TEXT("target"), &StepError) || !FHyperAIStudioNames::IsName(Step.Property, TEXT("property"), &StepError)
					|| (Step.InputAction.StartsWith(TEXT("/")) ? !FHyperAIStudioNames::IsObjectPath(Step.InputAction, TEXT("inputAction"), &StepError)
						: !FHyperAIStudioNames::IsName(Step.InputAction, TEXT("inputAction"), &StepError)))
				{
					OutError = FString::Printf(TEXT("%s, step %d: %s"), *Name, StepIndex + 1, *StepError);
					return false;
				}
			}
		}
		return true;
	}

	bool Start(TArray<FCase> Cases, const float DeadlineSeconds, TFunction<void(const FBatchOutcome&)> OnDone, FString& OutError)
	{
		OutError.Reset();
		if (!IsInGameThread() || !GEditor || !GEngine)
		{
			OutError = TEXT("Verification runs in the editor, on the game thread.");
			return false;
		}
		if (Current())
		{
			OutError = TEXT("A verification batch is already running; wait for it.");
			return false;
		}
		if (GEditor->IsPlaySessionInProgress() || GEditor->PlayWorld)
		{
			OutError = TEXT("Stop Play In Editor first: verification plays its own copies of the level.");
			return false;
		}
		UWorld* EditorWorld = EditorWorldOf();
		if (!EditorWorld || Cases.IsEmpty() || Cases.Num() > MaxCases)
		{
			OutError = FString::Printf(TEXT("A batch is 1 to %d cases, and needs an open level."), MaxCases);
			return false;
		}
		TUniquePtr<FBatch> Batch = MakeUnique<FBatch>();
		Batch->StartedAt = FPlatformTime::Seconds();
		Batch->Deadline = Batch->StartedAt + DeadlineSeconds;
		Batch->Map = EditorWorld->GetOutermost()->GetName();
		Batch->Limit = GetLimit();
		Batch->Output = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio") / TEXT("Verify")
			/ FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
		Batch->OnDone = MoveTemp(OnDone);
		for (int32 Index = 0; Index < Cases.Num(); ++Index)
		{
			TUniquePtr<FCaseWorld> Case = MakeUnique<FCaseWorld>();
			Case->Spec = MoveTemp(Cases[Index]);
			Case->Outcome.Name = Case->Spec.Name.IsEmpty() ? FString::Printf(TEXT("case %d"), Index + 1) : Case->Spec.Name;
			Batch->Cases.Add(MoveTemp(Case));
		}
		// What the editor sets up for a play session, and TearDown puts back.
		GEditor->EditorWorld = EditorWorld;
		FNavigationSystem::OnPIEStart(*EditorWorld);
		// Cases run one step per editor frame, so a background editor must not slow to a few frames a second meanwhile.
		UEditorEngine::FShouldDisableCPUThrottling Throttle = UEditorEngine::FShouldDisableCPUThrottling::CreateLambda([]() { return true; });
		Batch->Throttle = Throttle.GetHandle();
		GEditor->ShouldDisableCPUThrottlingDelegates.Add(MoveTemp(Throttle));
		// The user's own Play In Editor needs the play instances and the editor's play world: the batch gives way.
		Batch->PreBeginPIE = FEditorDelegates::PreBeginPIE.AddLambda([](bool)
		{
			Stop(TEXT("Stopped because Play In Editor started."));
		});
		Batch->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickBatch), 0.f);

		FHyperAIStudioAsyncJobRequest Job;
		Job.PackId = TEXT("pie_runtime");
		Job.ToolName = TEXT("hyper_verify_run");
		Job.Target = Batch->Map;
		Job.DeadlineMs = static_cast<int32>((DeadlineSeconds + 60.f) * 1000.f);
		Job.PollIntervalMs = 250;
		Job.OnCancel = []() { Stop(TEXT("Cancelled from the Activity panel.")); };
		const TSharedRef<FJobState> JobState = Batch->Job;
		JobState->Progress = FString::Printf(TEXT("0 of %d cases done"), Batch->Cases.Num());
		Current() = MoveTemp(Batch);
		FString JobId;
		FString JobError;
		FHyperAIStudioAsyncJobHost::Start(Job, [JobState](FString& OutProgress, FString& OutDiagnostic)
		{
			OutProgress = JobState->Progress;
			OutDiagnostic = JobState->Diagnostic;
			return !JobState->bDone ? EHyperAIStudioAsyncJobPoll::Running
				: JobState->bFailed ? EHyperAIStudioAsyncJobPoll::Failed : EHyperAIStudioAsyncJobPoll::Completed;
		}, JobId, JobError);
		return true;
	}

	void Stop(const FString& Reason)
	{
		if (Current())
		{
			Finish(TEXT("stopped"), Reason);
		}
	}

	bool IsRunning()
	{
		return Current().IsValid();
	}

	void SetCaseSetupForTests(TFunction<void(UWorld& World, const FString& CaseName)> Setup)
	{
		CaseSetup() = MoveTemp(Setup);
	}

	int32 GetLimit()
	{
		return ReadCalibration(CurrentMap()).Limit;
	}

	int32 PickLimit(const TMap<int32, double>& SecondsByCount, const double BudgetSeconds)
	{
		TArray<int32> Counts;
		SecondsByCount.GetKeys(Counts);
		Counts.Sort();
		int32 Best = 1;
		for (const int32 Count : Counts)
		{
			if (SecondsByCount[Count] > BudgetSeconds)
			{
				break;
			}
			Best = FMath::Clamp(Count, 1, MaxCases);
		}
		return Best;
	}

	FCalibration GetCalibration()
	{
		return ReadCalibration(CurrentMap());
	}

	bool Calibrate(const int32 MaxCount, TFunction<void(const FCalibration&)> OnDone, FString& OutError)
	{
		OutError.Reset();
		if (IsRunning())
		{
			OutError = TEXT("A verification batch is running; calibrate when it has finished.");
			return false;
		}
		const TSharedRef<FCalibrationRun> Run = MakeShared<FCalibrationRun>();
		Run->Map = CurrentMap();
		Run->OnDone = MoveTemp(OnDone);
		for (const int32 Count : {1, 2, 4, 6, 8, 10})
		{
			if (Count <= FMath::Clamp(MaxCount, 1, MaxCases))
			{
				Run->Counts.Add(Count);
			}
		}
		CalibrateNext(Run);
		return true;
	}
}
