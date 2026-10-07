// Games by Hyper 2026.

#include "HyperAIStudioSceneSpatialToolset.h"

#include "Editor.h"
#include "Engine/World.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioRequestFit.h"
#include "HyperAIStudioSceneCost.h"
#include "HyperAIStudioSpatialPlan.h"
#include "HyperAIStudioSpatialQueries.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioSceneSpatial, Log, All);

FString FHyperAIStudioSceneSpatialContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioSceneSpatial.HyperAIStudioSceneSpatialToolset");
}

TArray<FString> FHyperAIStudioSceneSpatialContracts::GetToolNames()
{
	return {CostToolName, QueryToolName, PlanToolName};
}

bool FHyperAIStudioSceneSpatialContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::SceneSpatial::Private
{
	using C = FHyperAIStudioSceneSpatialContracts;

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	/** The world a read asks about: the editor's (the default), or Play In Editor's while it runs. */
	UWorld* PickWorld(const FString& Name, FString& OutWorld, FString& OutStatus, FString& OutError)
	{
		const FString Key = Name.TrimStartAndEnd().ToLower();
		if (Key.IsEmpty() || Key == TEXT("editor"))
		{
			OutWorld = TEXT("editor");
			if (UWorld* World = EditorWorld()) return World;
			OutStatus = TEXT("no_editor_world");
			OutError = TEXT("No level is open in the editor.");
			return nullptr;
		}
		if (Key == TEXT("pie"))
		{
			OutWorld = TEXT("pie");
			if (UWorld* World = GEditor ? GEditor->PlayWorld.Get() : nullptr) return World;
			OutStatus = TEXT("no_pie_world");
			OutError = TEXT("Play In Editor is not running; start it, or ask about the editor world.");
			return nullptr;
		}
		OutStatus = TEXT("invalid_request");
		OutError = TEXT("World is editor or pie.");
		return nullptr;
	}

	void Record(const FString& OperationId, const FString& Target, const FHyperAISpatialPlanReport& Done)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::PlanToolName;
		Entry.Target = Target.Left(200);
		Entry.OperationId = OperationId;
		Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Done.Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}
}

FHyperAISceneCostReport FHyperAIStudioSceneSpatialContracts::CostReport(const FHyperAISceneCostRequest& In)
{
	using namespace HyperAIStudio::SceneSpatial;
	FHyperAISceneCostReport Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("hyper_scene_cost_report runs on the game thread.");
		return Report;
	}
	FHyperAISceneCostRequest Request = In;
	FHyperAIStudioRequestFit Fit;
	Fit.ClampOrDefault(Request.TopN, 1, 50, 15, TEXT("top_n"));
	Fit.ClampOrDefault(Request.MaxActors, 100, 100000, 20000, TEXT("max_actors"));
	Fit.ClampOrDefault(Request.DeadlineMs, 100, 5000, 1500, TEXT("deadline_ms"));
	Fit.ClampOrDefault(Request.MaxOutputBytes, 4096, 131072, 32768, TEXT("max_output_bytes"));
	FString Error;
	UWorld* World = Private::PickWorld(Request.World, Report.World, Report.Status, Error);
	if (!World)
	{
		Report.Diagnostic = Error;
		return Report;
	}
	Cost::Measure(*World, Request, Report);
	Report.Diagnostic = Fit.AppendTo(Report.Diagnostic);
	return Report;
}

FHyperAISpatialQueryReport FHyperAIStudioSceneSpatialContracts::Query(const FHyperAISpatialQueryRequest& In)
{
	using namespace HyperAIStudio::SceneSpatial;
	FHyperAISpatialQueryReport Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("hyper_spatial_query runs on the game thread.");
		return Report;
	}
	if (In.Queries.IsEmpty() || In.Queries.Num() > MaxQueries)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = FString::Printf(TEXT("Queries holds 1 to %d questions; split a longer list over several calls."), MaxQueries);
		return Report;
	}
	for (int32 Index = 0; Index < In.Queries.Num(); ++Index)
	{
		FString Why;
		if (!Spatial::CheckNames(In.Queries[Index], Why))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = FString::Printf(TEXT("Queries[%d]: %s"), Index, *Why);
			return Report;
		}
	}
	FHyperAISpatialQueryRequest Request = In;
	FHyperAIStudioRequestFit Fit;
	Fit.ClampOrDefault(Request.DeadlineMs, 100, 5000, 1000, TEXT("deadline_ms"));
	Fit.ClampOrDefault(Request.MaxOutputBytes, 4096, 131072, 32768, TEXT("max_output_bytes"));
	FString Error;
	UWorld* World = Private::PickWorld(Request.World, Report.World, Report.Status, Error);
	if (!World)
	{
		Report.Diagnostic = Error;
		return Report;
	}
	Spatial::RunAll(*World, Request, Report);
	Report.Diagnostic = Fit.AppendTo(Report.Diagnostic);
	return Report;
}

FHyperAISpatialPlanReport FHyperAIStudioSceneSpatialContracts::ApplyPlan(const FHyperAISpatialPlanRequest& Request)
{
	using namespace HyperAIStudio::SceneSpatial;
	FHyperAISpatialPlanReport Report;
	const FString Requested = Request.OperationId.TrimStartAndEnd();
	Report.OperationId = Requested.IsEmpty()
		? FString::Printf(TEXT("spatial-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Requested;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("hyper_spatial_apply_plan runs on the game thread.");
		return Report;
	}
	UWorld* World = Private::EditorWorld();
	if (!World)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("No level is open in the editor.");
		return Report;
	}
	if (GEditor && GEditor->PlayWorld)
	{
		Report.Status = TEXT("play_in_editor_running");
		Report.Diagnostic = TEXT("Stop Play In Editor first: edits made now would not reach the running game.");
		return Report;
	}
	TArray<Plan::FMove> Moves;
	if (!Plan::Check(*World, Request, Moves, Report))
	{
		return Report;
	}
	if (Request.bSave && World->GetOutermost()->GetName().StartsWith(TEXT("/Temp/")))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("This level has never been saved, so there is nowhere to save the moves; save the level first or leave bSave off.");
		return Report;
	}
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%d move(s) checked; nothing was changed. Moves says where each actor would go."), Moves.Num());
		return Report;
	}
	const FString LevelPath = World->GetOutermost()->GetName();
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = PlanToolName;
		Summary.VariantId = TEXT("spatial.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = LevelPath;
		const int32 Placed = Moves.FilterByPredicate([](const Plan::FMove& Move) { return Move.Kind == TEXT("place"); }).Num();
		Summary.Effects.Add(FString::Printf(TEXT("Spatial placement: %d actor(s) moved, %d placed, one undo step%s"), Moves.Num() - Placed, Placed,
			Request.bSave ? TEXT(", then the level saved") : TEXT("")));
		for (int32 Index = 0; Index < Report.Moves.Num() && Index < 24; ++Index)
		{
			Summary.Effects.Add(TEXT("| ") + Report.Moves[Index]);
		}
		for (const Plan::FMove& Move : Moves)
		{
			Summary.Touches.AddUnique(Move.Actor.IsValid() ? Move.Actor->GetPathName() : Move.Name);
		}
		const FString OperationId = Report.OperationId;
		FString Error;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, Moves, OperationId, LevelPath]()
			{
				UWorld* Target = Private::EditorWorld();
				FHyperAISpatialPlanReport Done;
				Done.OperationId = OperationId;
				Done.Status = TEXT("operation_failed");
				if (!Target) Done.Diagnostic = TEXT("No level is open in the editor.");
				else if (GEditor && GEditor->PlayWorld) Done.Diagnostic = TEXT("Play In Editor was running when the plan was approved; nothing was changed.");
				// A placement has no actor to recognise, so another level open by now would take it.
				else if (Target->GetOutermost()->GetName() != LevelPath)
				{
					Done.Diagnostic = FString::Printf(TEXT("The plan was checked in %s, but %s is open now; nothing was changed."), *LevelPath, *Target->GetOutermost()->GetName());
				}
				else Done = Plan::Execute(*Target, Request, Moves, OperationId);
				Private::Record(OperationId, LevelPath, Done);
			}, Error))
		{
			Report.Status = TEXT("approval_not_queued");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
		return Report;
	}
	// Checked a moment ago in this same call, so it is applied as checked rather than searched for again.
	FHyperAISpatialPlanReport Done = Plan::Execute(*World, Request, Moves, Report.OperationId, /*bRecheck=*/false);
	Done.Notes = Report.Notes;
	return Done;
}

FHyperAISceneCostReport UHyperAIStudioSceneSpatialToolset::hyper_scene_cost_report(const FHyperAISceneCostRequest& Request)
{
	return FHyperAIStudioSceneSpatialContracts::CostReport(Request);
}

FHyperAISpatialQueryReport UHyperAIStudioSceneSpatialToolset::hyper_spatial_query(const FHyperAISpatialQueryRequest& Request)
{
	return FHyperAIStudioSceneSpatialContracts::Query(Request);
}

FHyperAISpatialPlanReport UHyperAIStudioSceneSpatialToolset::hyper_spatial_apply_plan(const FHyperAISpatialPlanRequest& Request)
{
	return FHyperAIStudioSceneSpatialContracts::ApplyPlan(Request);
}

void FHyperAIStudioSceneSpatialRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioSceneSpatialRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioSceneSpatialRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioSceneSpatialToolset::StaticClass(),
			FHyperAIStudioSceneSpatialContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioSceneSpatial, Warning, TEXT("Could not unregister the scene spatial toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioSceneSpatialRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioSceneSpatialContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioSceneSpatial, Verbose, TEXT("The scene spatial source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioSceneSpatialToolset::StaticClass(),
		FHyperAIStudioSceneSpatialContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioSceneSpatial, Error, TEXT("Scene spatial cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}
