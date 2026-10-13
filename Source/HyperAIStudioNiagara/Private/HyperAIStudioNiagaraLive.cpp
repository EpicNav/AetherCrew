// Games by Hyper 2026.

#include "HyperAIStudioNiagaraLive.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HyperAIStudioRequestFit.h"
#include "Misc/Paths.h"
#include "NiagaraCommon.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

namespace HyperAIStudio::Niagara::Live
{
	namespace
	{
		FString StateName(const ENiagaraExecutionState State)
		{
			switch (State)
			{
			case ENiagaraExecutionState::Active: return TEXT("active");
			case ENiagaraExecutionState::Inactive: return TEXT("inactive");
			case ENiagaraExecutionState::InactiveClear: return TEXT("inactive_clear");
			case ENiagaraExecutionState::Complete: return TEXT("complete");
			case ENiagaraExecutionState::Disabled: return TEXT("disabled");
			default: return TEXT("unknown");
			}
		}

		bool Matches(const FString& Filter, const FString& A, const FString& B, const FString& C)
		{
			return Filter.IsEmpty() || A.Contains(Filter) || B.Contains(Filter) || C.Contains(Filter);
		}

		/** A script of the emitter that failed its last compile, by its role; empty when none did. */
		FString FailedScript(const FVersionedNiagaraEmitterData& Data)
		{
			const TPair<const TCHAR*, const UNiagaraScript*> Scripts[] = {
				{TEXT("particle spawn"), Data.SpawnScriptProps.Script}, {TEXT("particle update"), Data.UpdateScriptProps.Script},
				{TEXT("GPU compute"), Data.GetGPUComputeScript()}};
			for (const TPair<const TCHAR*, const UNiagaraScript*>& Script : Scripts)
			{
				if (Script.Value && Script.Value->GetLastCompileStatus() == ENiagaraScriptCompileStatus::NCS_Error)
				{
					return Script.Key;
				}
			}
			return FString();
		}

		FString OwnerLabel(const UNiagaraComponent& Component)
		{
			const AActor* Owner = Component.GetOwner();
			return Owner ? FString::Printf(TEXT("%s.%s"), *Owner->GetActorLabel(), *Component.GetName()) : Component.GetName();
		}
	}

	FString BoundsText(const FBox& Box)
	{
		if (!Box.IsValid)
		{
			return TEXT("empty");
		}
		return FString::Printf(TEXT("min (%.0f,%.0f,%.0f) max (%.0f,%.0f,%.0f)"), Box.Min.X, Box.Min.Y, Box.Min.Z, Box.Max.X, Box.Max.Y, Box.Max.Z);
	}

	void Read(UWorld& World, const FString& Filter, const bool bIncludeInactive, FHyperAINiagaraLiveReport& Out)
	{
		Out.Level = World.GetOutermost()->GetName();
		for (TObjectIterator<UNiagaraComponent> It; It; ++It)
		{
			UNiagaraComponent* Component = *It;
			if (!IsValid(Component) || Component->GetWorld() != &World || !Component->IsRegistered()) continue;
			UNiagaraSystem* Asset = Component->GetAsset();
			const FString AssetPath = Asset ? Asset->GetPathName() : FString(TEXT("(none)"));
			const FString Label = OwnerLabel(*Component);
			if (!Matches(Filter, AssetPath, Label, Component->GetPathName())) continue;
			++Out.Components;
			const FNiagaraSystemInstanceControllerConstPtr Controller = Component->GetSystemInstanceController();
			const FNiagaraSystemInstance* Instance = Controller.IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr;
			const bool bPlaying = Component->IsActive() && Instance && !Instance->IsComplete();
			if (bPlaying) ++Out.ActiveComponents;
			if (!bPlaying && !bIncludeInactive) continue;

			FHyperAINiagaraLiveSystem& System = Out.Systems.AddDefaulted_GetRef();
			System.System = AssetPath;
			System.Component = Label;
			System.ComponentPath = Component->GetPathName();
			System.State = !Instance ? FString(TEXT("not_playing"))
				: StateName(const_cast<FNiagaraSystemInstance*>(Instance)->GetActualExecutionState());
			System.BoundsCenter = Component->Bounds.Origin;
			System.BoundsSize = Component->Bounds.BoxExtent * 2.0;
			if (!Asset)
			{
				System.Issues.Add(TEXT("The component has no Niagara System asset."));
				continue;
			}
			System.bFixedBounds = Asset->bFixedBounds;
			if (!Asset->IsValid())
			{
				System.Issues.Add(TEXT("The system is not valid: a script failed to compile or an emitter is missing (hyper_niagara_validate says which)."));
			}
			if (!Instance)
			{
				continue;
			}
			System.AgeSeconds = Instance->GetAge();
			if (Instance->IsDisabled())
			{
				System.Issues.Add(TEXT("The system instance is disabled: it failed to initialise (invalid asset or data interface)."));
			}
			for (const FNiagaraEmitterInstanceRef& EmitterRef : Instance->GetEmitters())
			{
				const FNiagaraEmitterInstance& Emitter = EmitterRef.Get();
				FHyperAINiagaraLiveEmitter& Row = System.Emitters.AddDefaulted_GetRef();
				Row.Name = Emitter.GetEmitterHandle().GetName().ToString();
				Row.SimTarget = Emitter.AsStateless() ? TEXT("stateless") : Emitter.GetSimTarget() == ENiagaraSimTarget::GPUComputeSim ? TEXT("gpu") : TEXT("cpu");
				Row.State = StateName(Emitter.GetExecutionState());
				Row.Particles = FMath::Max(0, Emitter.GetNumParticles());
				Row.TotalSpawned = Emitter.GetTotalSpawnedParticles();
				Row.bDynamicBounds = Emitter.AreBoundsDynamic();
				Row.Bounds = BoundsText(Emitter.GetBounds());
				System.Particles += Row.Particles;
				Out.Particles += Row.Particles;
				if (!Emitter.GetEmitterHandle().GetIsEnabled())
				{
					Row.Issues.Add(TEXT("Turned off in the system (its emitter handle is disabled)."));
				}
				else if (Emitter.IsDisabled())
				{
					Row.Issues.Add(TEXT("Disabled: it failed to compile, its scalability or detail level turned it off, or it is isolated."));
				}
				if (const FVersionedNiagaraEmitterData* Data = Emitter.GetVersionedEmitter().GetEmitterData())
				{
					const FString Failed = FailedScript(*Data);
					if (!Failed.IsEmpty())
					{
						Row.Issues.Add(FString::Printf(TEXT("Its %s script has compile errors (hyper_niagara_validate lists them)."), *Failed));
					}
					else if (!Data->IsValid())
					{
						Row.Issues.Add(TEXT("Its emitter data is not valid: a script is missing or failed to compile."));
					}
				}
				if (Emitter.IsWaitingForPSOCaching())
				{
					Row.Issues.Add(TEXT("Waiting for its shaders (PSO caching); it draws once they are ready."));
				}
				if (Row.Particles > 0 && Row.bDynamicBounds && Row.Bounds == TEXT("empty"))
				{
					Row.Issues.Add(TEXT("Particles are alive but its bounds are empty, so it can be culled; give the system fixed bounds."));
				}
			}
		}
	}

	void ApplySample(const FHyperAINiagaraLiveReport& Before, const double SampleSeconds, FHyperAINiagaraLiveReport& After)
	{
		After.SampledSeconds = SampleSeconds;
		for (FHyperAINiagaraLiveSystem& System : After.Systems)
		{
			const FHyperAINiagaraLiveSystem* Earlier = Before.Systems.FindByPredicate([&System](const FHyperAINiagaraLiveSystem& Row)
			{
				return Row.ComponentPath == System.ComponentPath;
			});
			for (FHyperAINiagaraLiveEmitter& Emitter : System.Emitters)
			{
				const FHyperAINiagaraLiveEmitter* Then = Earlier ? Earlier->Emitters.FindByPredicate([&Emitter](const FHyperAINiagaraLiveEmitter& Row)
				{
					return Row.Name == Emitter.Name;
				}) : nullptr;
				if (!Then || Emitter.TotalSpawned < Then->TotalSpawned)
				{
					// New since the first read, or restarted in between: what it spawned since it began is what the sample saw.
					Emitter.SpawnedInSample = Emitter.TotalSpawned;
				}
				else
				{
					Emitter.SpawnedInSample = Emitter.TotalSpawned - Then->TotalSpawned;
				}
				Emitter.Spawning = Emitter.SpawnedInSample > 0 ? TEXT("yes") : TEXT("no");
			}
		}
	}

	void Finish(const int32 TopN, FHyperAINiagaraLiveReport& Out)
	{
		for (FHyperAINiagaraLiveSystem& System : Out.Systems)
		{
			for (FHyperAINiagaraLiveEmitter& Emitter : System.Emitters)
			{
				if (Emitter.State == TEXT("active") && Emitter.Particles == 0 && Emitter.Spawning == TEXT("no") && Emitter.Issues.IsEmpty())
				{
					Emitter.Issues.Add(FString::Printf(TEXT("Active, but nothing alive and nothing spawned in %.1f s: check its spawn rate or burst, spawn conditions and lifetime."),
						Out.SampledSeconds));
				}
			}
		}
		Out.Systems.StableSort([](const FHyperAINiagaraLiveSystem& A, const FHyperAINiagaraLiveSystem& B)
		{
			const int32 IssuesA = A.Issues.Num() + A.Emitters.FilterByPredicate([](const FHyperAINiagaraLiveEmitter& E) { return !E.Issues.IsEmpty(); }).Num();
			const int32 IssuesB = B.Issues.Num() + B.Emitters.FilterByPredicate([](const FHyperAINiagaraLiveEmitter& E) { return !E.Issues.IsEmpty(); }).Num();
			return IssuesA != IssuesB ? IssuesA > IssuesB : A.Particles > B.Particles;
		});
		if (Out.Systems.Num() > TopN)
		{
			Out.Notes.Add(FString::Printf(TEXT("%d more system(s) not listed; raise top_n or use filter."), Out.Systems.Num() - TopN));
			Out.Systems.SetNum(TopN);
		}
		Out.Errors.Reset();
		for (const FHyperAINiagaraLiveSystem& System : Out.Systems)
		{
			const FString Name = FString::Printf(TEXT("%s (%s)"), *FPaths::GetBaseFilename(System.System), *System.Component);
			for (const FString& Issue : System.Issues) Out.Errors.Add(Name + TEXT(": ") + Issue);
			for (const FHyperAINiagaraLiveEmitter& Emitter : System.Emitters)
			{
				for (const FString& Issue : Emitter.Issues) Out.Errors.Add(FString::Printf(TEXT("%s / %s: %s"), *Name, *Emitter.Name, *Issue));
			}
		}
		Out.Notes.Add(TEXT("GPU particle counts come from the last GPU readback, a frame or more old. Cost per system: hyper_profile_live."));
	}

	FHyperAINiagaraLiveReport ReadOnce(const FHyperAINiagaraLiveRequest& Request)
	{
		FHyperAINiagaraLiveReport Report;
		UWorld* World = GEditor ? GEditor->PlayWorld.Get() : nullptr;
		if (!IsInGameThread() || !World)
		{
			Report.Status = World ? TEXT("invalid_request") : TEXT("no_pie_world");
			Report.Diagnostic = World ? TEXT("hyper_niagara_live runs on the game thread.")
				: TEXT("Play In Editor is not running: this reads effects while the game plays. Start it (hyper_playtest_run, or Play) and ask again.");
			return Report;
		}
		Read(*World, Request.Filter.TrimStartAndEnd(), Request.bIncludeInactive, Report);
		Report.Notes.Add(TEXT("Read once: spawning is unknown without sample_seconds."));
		Finish(FMath::Clamp(Request.TopN, 1, 100), Report);
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%d of %d Niagara component(s) playing, %lld particles; %d issue(s)."),
			Report.ActiveComponents, Report.Components, Report.Particles, Report.Errors.Num());
		return Report;
	}
}

UHyperAINiagaraLiveResult* UHyperAIStudioNiagaraToolset::hyper_niagara_live(const FHyperAINiagaraLiveRequest& In)
{
	using namespace HyperAIStudio::Niagara;
	UHyperAINiagaraLiveResult* Result = NewObject<UHyperAINiagaraLiveResult>();
	FHyperAINiagaraLiveRequest Request = In;
	FHyperAINiagaraLiveReport Report;
	FHyperAIStudioRequestFit Fit;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("hyper_niagara_live runs on the game thread.");
		Result->SetValue(Report);
		return Result;
	}
	UWorld* World = GEditor ? GEditor->PlayWorld.Get() : nullptr;
	if (!World)
	{
		Report.Status = TEXT("no_pie_world");
		Report.Diagnostic = TEXT("Play In Editor is not running: this reads effects while the game plays. Start it (hyper_playtest_run, or Play) and ask again.");
		Result->SetValue(Report);
		return Result;
	}
	Fit.ClampOrDefault(Request.TopN, 1, 100, 20, TEXT("top_n"));
	if (Request.SampleSeconds < 0.f) Request.SampleSeconds = 0.f;
	Fit.Clamp(Request.SampleSeconds, 0.f, 5.f, TEXT("sample_seconds"));
	Request.Filter.TrimStartAndEndInline();
	Live::Read(*World, Request.Filter, Request.bIncludeInactive, Report);
	auto Complete = [](FHyperAINiagaraLiveReport& Done, const int32 TopN, const FString& FitNotes)
	{
		Live::Finish(TopN, Done);
		Done.bOk = true;
		Done.Status = TEXT("completed");
		Done.Diagnostic = FString::Printf(TEXT("%d of %d Niagara component(s) playing, %lld particles; %d issue(s)."),
			Done.ActiveComponents, Done.Components, Done.Particles, Done.Errors.Num());
		if (!FitNotes.IsEmpty()) Done.Diagnostic += TEXT(" ") + FitNotes;
	};
	const FString FitNotes = Fit.AppendTo(FString());
	if (Request.SampleSeconds <= 0.f)
	{
		Report.Notes.Add(TEXT("Read once: spawning is unknown without sample_seconds."));
		Complete(Report, Request.TopN, FitNotes);
		Result->SetValue(Report);
		return Result;
	}
	// Read again once the sample is over, while play goes on; the reply waits for it.
	const TStrongObjectPtr<UHyperAINiagaraLiveResult> Pending(Result);
	const double Started = FPlatformTime::Seconds();
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Pending, Request, Before = Report, Started, Complete, FitNotes](float)
	{
		FHyperAINiagaraLiveReport Done;
		UWorld* PlayWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
		if (!PlayWorld)
		{
			Done = Before;
			Done.Notes.Add(TEXT("Play stopped during the sample; this is the first read."));
			Complete(Done, Request.TopN, FitNotes);
			Pending->SetValue(Done);
			return false;
		}
		Live::Read(*PlayWorld, Request.Filter, Request.bIncludeInactive, Done);
		Live::ApplySample(Before, FPlatformTime::Seconds() - Started, Done);
		Complete(Done, Request.TopN, FitNotes);
		Pending->SetValue(Done);
		return false;
	}), Request.SampleSeconds);
	return Result;
}
