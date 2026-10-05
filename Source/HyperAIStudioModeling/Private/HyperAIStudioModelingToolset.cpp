// Games by Hyper 2026.

#include "HyperAIStudioModelingToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioModelingGate.h"
#include "HyperAIStudioSourceControl.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioModeling, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioModeling"

FString FHyperAIStudioModelingContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioModeling.HyperAIStudioModelingToolset");
}

TArray<FString> FHyperAIStudioModelingContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName, ValidateToolName};
}

bool FHyperAIStudioModelingContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::ModelingTools::Private
{
	using C = FHyperAIStudioModelingContracts;
	namespace Gate = HyperAIStudio::Modeling::Gate;

	constexpr int32 MaxListedAssets = 200;

	FString Folder(const FString& Requested)
	{
		FString Path = Requested.TrimStartAndEnd();
		if (Path.IsEmpty()) Path = C::DefaultFolder;
		while (Path.Len() > 1 && Path.EndsWith(TEXT("/"))) Path.LeftChopInline(1);
		return Path;
	}

	UStaticMesh* LoadMesh(const FString& Reference)
	{
		const FString Trimmed = Reference.TrimStartAndEnd();
		if (!Trimmed.StartsWith(TEXT("/"))) return nullptr;
		const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
		if (UStaticMesh* Found = FindObject<UStaticMesh>(nullptr, *ObjectPath)) return Found;
		// A path with nothing on disk is never loaded: the failed load would leave an empty package behind.
		if (!FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ObjectPath))) return nullptr;
		return LoadObject<UStaticMesh>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** A registry tag's value, or a dash when the asset was saved without it. */
	FString Tag(const FAssetData& Asset, const TCHAR* Name)
	{
		FString Value;
		return Asset.GetTagValue(FName(Name), Value) ? Value : FString(TEXT("-"));
	}

	bool IsPlaying()
	{
		return GEditor && GEditor->PlayWorld != nullptr;
	}

	FHyperAIModelPlanReport Execute(const FHyperAIModelPlanRequest& Request, const FString& OperationId, Gate::FPlan* Checked, Gate::FBuilt* Prebuilt)
	{
		FHyperAIModelPlanReport Report;
		Report.OperationId = OperationId;
		FString Error;
		if (IsPlaying())
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = TEXT("Play in Editor is running; stop it, then send the plan again.");
			return Report;
		}
		// After an approval the project may have moved on: the plan is checked and built again against it.
		Gate::FPlan Fresh;
		Gate::FPlan& Plan = Checked ? *Checked : Fresh;
		if (!Checked && !Gate::Check(Request.Operations, Fresh, Error))
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Error;
			return Report;
		}
		TUniquePtr<Gate::FBuilt> Own;
		Gate::FBuilt* Built = Prebuilt;
		if (!Built)
		{
			Own = MakeUnique<Gate::FBuilt>();
			Built = Own.Get();
			if (!Gate::Build(Plan, *Built, Error))
			{
				Report.Status = TEXT("operation_failed");
				Report.Diagnostic = Error;
				return Report;
			}
		}
		Report.PlanHash = Plan.Hash;
		Report.ExpandedOps = Plan.ExpandedLines;
		Report.BuildMs = Built->BuildMs;
		Gate::Describe(*Built, Report.Meshes);
		TArray<UStaticMesh*> Written;
		TArray<UStaticMesh*> Created;
		bool bWritten = false;
		{
			const FScopedTransaction Transaction(LOCTEXT("ModelPlan", "HyperAI: Model Plan"));
			bWritten = Gate::Write(Plan, *Built, Report.Assets, Written, Created, Report.Notes, Error);
		}
		if (!bWritten)
		{
			Gate::Discard(Created);
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Error + (Written.Num() > 0 ? TEXT(" New assets the plan made were deleted; replaced ones stay in the plan's undo step.")
				: TEXT(" Nothing was written."));
			Report.Assets.Reset();
			return Report;
		}
		if (!Gate::Verify(Written, Report.Assets, Error))
		{
			Gate::Discard(Created);
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Error + TEXT(" New assets the plan made were deleted; replaced ones stay in memory, unsaved, and undo takes them back.");
			Report.Assets.Reset();
			return Report;
		}
		for (UStaticMesh* Mesh : Written)
		{
			if (!FHyperAIStudioSourceControl::SaveAsset(*Mesh, Error))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = FString::Printf(TEXT("%s was written but not saved: %s"), *Mesh->GetPathName(), *Error);
				return Report;
			}
			Report.Saved.Add(Mesh->GetPathName());
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		TArray<FString> Lines;
		for (const FHyperAIModelAssetResult& Asset : Report.Assets)
		{
			Lines.Add(FString::Printf(TEXT("%s (%d triangles, %d slot(s), %s collision, %d shape(s)%s)"), *Asset.Path, Asset.Triangles, Asset.Slots, *Asset.Collision,
				Asset.CollisionShapes, Asset.bNanite ? TEXT(", Nanite") : TEXT("")));
		}
		Report.Diagnostic = FString::Printf(TEXT("%d asset(s) written and saved: %s. Undo takes back replaced assets; new ones stay until deleted."), Report.Assets.Num(),
			*FString::Join(Lines, TEXT("; ")));
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAIModelPlanReport& Done)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::PlanToolName;
		Entry.Target = Done.Saved.IsEmpty() ? FString() : Done.Saved[0].Left(200);
		Entry.OperationId = OperationId;
		Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Done.Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}
}

FHyperAIModelInspectReport FHyperAIStudioModelingContracts::Inspect(const FHyperAIModelInspectRequest& Request)
{
	using namespace HyperAIStudio::ModelingTools::Private;
	FHyperAIModelInspectReport Report;
	Report.Kinds = Gate::DescribeKinds();
	for (const HyperAIStudio::Modeling::Recipes::FInfo& Recipe : HyperAIStudio::Modeling::Recipes::List())
	{
		Report.Recipes.Add(FString::Printf(TEXT("%s (Size %g cm): %s."), Recipe.Name, Recipe.DefaultSize, Recipe.What));
	}
	Report.Limits = Gate::DescribeLimits();
	const FString Path = Folder(Request.Folder);
	if (!Path.StartsWith(TEXT("/")))
	{
		Report.Diagnostic = FString::Printf(TEXT("Folder '%s' is not a content path, like /Game/HyperAI/Models."), *Request.Folder);
		return Report;
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	Registry.GetAssetsByPath(FName(*Path), Assets, /*bRecursive=*/true);
	Assets.RemoveAll([](const FAssetData& Asset) { return Asset.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName(); });
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
	for (const FAssetData& Asset : Assets)
	{
		if (Report.Assets.Num() >= MaxListedAssets)
		{
			Report.bTruncated = true;
			break;
		}
		Report.Assets.Add(FString::Printf(TEXT("%s: %s triangles, %s cm, %s slot(s), %s collision shape(s), Nanite %s"), *Asset.PackageName.ToString(), *Tag(Asset, TEXT("Triangles")),
			*Tag(Asset, TEXT("ApproxSize")).Replace(TEXT("x"), TEXT(",")), *Tag(Asset, TEXT("Materials")), *Tag(Asset, TEXT("CollisionPrims")), *Tag(Asset, TEXT("NaniteEnabled"))));
	}
	if (!Request.Asset.TrimStartAndEnd().IsEmpty())
	{
		UStaticMesh* Mesh = LoadMesh(Request.Asset);
		if (!Mesh)
		{
			Report.Diagnostic = FString::Printf(TEXT("%s is not a static mesh."), *Request.Asset);
			return Report;
		}
		TArray<FHyperAIModelIssue> Ignored;
		FString Line;
		Gate::CheckAsset(*Mesh, /*bProduction=*/false, Ignored, Line);
		Report.Details.Add(Line);
		for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
		{
			const FStaticMaterial& Material = Mesh->GetStaticMaterials()[Slot];
			Report.Details.Add(FString::Printf(TEXT("Slot %d %s: %s"), Slot, *Material.MaterialSlotName.ToString(),
				Material.MaterialInterface ? *Material.MaterialInterface->GetPathName() : TEXT("(empty)")));
		}
		if (const UBodySetup* Body = Mesh->GetBodySetup())
		{
			Report.Details.Add(FString::Printf(TEXT("Collision: %d box(es), %d convex hull(s), %d sphere(s), %d capsule(s)%s"), Body->AggGeom.BoxElems.Num(), Body->AggGeom.ConvexElems.Num(),
				Body->AggGeom.SphereElems.Num(), Body->AggGeom.SphylElems.Num(), Body->CollisionTraceFlag == ECollisionTraceFlag::CTF_UseComplexAsSimple ? TEXT(", complex as simple") : TEXT("")));
		}
		const FString* Hash = Mesh->GetPackage()->GetMetaData().FindValue(Mesh, C::PlanHashKey);
		Report.Details.Add(Hash ? FString::Printf(TEXT("Made by hyper_model_apply_plan, plan %s."), **Hash) : FString(TEXT("Not made by hyper_model_apply_plan.")));
	}
	Report.bOk = true;
	Report.Diagnostic = FString::Printf(TEXT("%d op kinds, %d recipes; %d static mesh(es) under %s%s."), Report.Kinds.Num(), Report.Recipes.Num(), Report.Assets.Num(), *Path,
		Report.bTruncated ? TEXT(" (the first listed)") : TEXT(""));
	return Report;
}

FHyperAIModelPlanReport FHyperAIStudioModelingContracts::ApplyPlan(const FHyperAIModelPlanRequest& Request)
{
	using namespace HyperAIStudio::ModelingTools::Private;
	FHyperAIModelPlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("model-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	if (IsPlaying())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Play in Editor is running; stop it, then send the plan again.");
		return Report;
	}
	FString Error;
	Gate::FPlan Plan;
	if (!Gate::Check(Request.Operations, Plan, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	Report.PlanHash = Plan.Hash;
	Report.ExpandedOps = Plan.ExpandedLines;
	if (!Request.bDryRun && Plan.Writes == 0)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("The plan writes nothing: end it with write_asset, or send it with bDryRun true to see the meshes. Nothing was changed.");
		return Report;
	}
	// Built in memory before anything else: the dry run's answer, and the check that every op runs.
	TUniquePtr<Gate::FBuilt> Built = MakeUnique<Gate::FBuilt>();
	if (!Gate::Build(Plan, *Built, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	Report.BuildMs = Built->BuildMs;
	Gate::Describe(*Built, Report.Meshes);
	if (Request.bDryRun)
	{
		Gate::PreviewWrites(Plan, *Built, Report.Assets);
		FString Ignored;
		if (Plan.Writes > 0 && !Gate::VertexColorMaterial(/*bCreate=*/false, Ignored))
		{
			Report.Notes.Add(FString::Printf(TEXT("Writing makes the vertex-colour material %s first."), C::VertexColorMaterial));
		}
		Report.bOk = true;
		Report.Status = TEXT("checked");
		int32 Triangles = 0;
		for (const FHyperAIModelMeshStats& Mesh : Report.Meshes) Triangles += Mesh.Triangles;
		Report.Diagnostic = FString::Printf(TEXT("%d op(s) built %d mesh(es), %d triangles, in %d ms; %d asset(s) would be written. Nothing was changed."), Plan.Steps.Num(),
			Report.Meshes.Num(), Triangles, Built->BuildMs, Report.Assets.Num());
		return Report;
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		TArray<FHyperAIModelAssetResult> Preview;
		Gate::PreviewWrites(Plan, *Built, Preview);
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = PlanToolName;
		Summary.VariantId = TEXT("model.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.PlanHash = Plan.Hash;
		Summary.EffectTarget = Preview.IsEmpty() ? FString() : Preview[0].Path;
		Summary.Effects.Add(FString::Printf(TEXT("Model plan: %d op(s), %d asset(s) written and saved"), Plan.Steps.Num(), Preview.Num()));
		for (const FHyperAIModelAssetResult& Asset : Preview)
		{
			Summary.Effects.Add(FString::Printf(TEXT("| write_asset %s (%d triangles, Nanite %s, %s collision%s)"), *Asset.Path, Asset.Triangles, Asset.bNanite ? TEXT("on") : TEXT("off"),
				*Asset.Collision, Asset.bReplaced ? TEXT(", replacing it") : TEXT("")));
			Summary.Touches.AddUnique(Asset.Path);
		}
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				Record(OperationId, Execute(Request, OperationId, nullptr, nullptr));
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
	FHyperAIModelPlanReport Done = Execute(Request, Report.OperationId, &Plan, Built.Get());
	Record(Report.OperationId, Done);
	return Done;
}

FHyperAIModelValidateReport FHyperAIStudioModelingContracts::Validate(const FHyperAIModelValidateRequest& Request)
{
	using namespace HyperAIStudio::ModelingTools::Private;
	FHyperAIModelValidateReport Report;
	const FString Policy = Request.Policy.TrimStartAndEnd().ToLower();
	if (!Policy.IsEmpty() && Policy != TEXT("placeholder") && Policy != TEXT("production"))
	{
		Report.Diagnostic = TEXT("Policy is placeholder or production.");
		return Report;
	}
	if (Request.Assets.IsEmpty() || Request.Assets.Num() > 64)
	{
		Report.Diagnostic = TEXT("Give 1 to 64 static meshes to check.");
		return Report;
	}
	for (const FString& Path : Request.Assets)
	{
		UStaticMesh* Mesh = LoadMesh(Path);
		if (!Mesh)
		{
			FHyperAIModelIssue& Missing = Report.Issues.AddDefaulted_GetRef();
			Missing.Asset = Path;
			Missing.Severity = TEXT("error");
			Missing.Code = TEXT("not_a_static_mesh");
			Missing.Message = FString::Printf(TEXT("%s is not a static mesh."), *Path);
			continue;
		}
		FString Line;
		Gate::CheckAsset(*Mesh, Policy == TEXT("production"), Report.Issues, Line);
		Report.Assets.Add(Line);
	}
	const int32 Errors = Report.Issues.FilterByPredicate([](const FHyperAIModelIssue& Issue) { return Issue.Severity == TEXT("error"); }).Num();
	Report.bOk = Errors == 0;
	Report.Diagnostic = FString::Printf(TEXT("%d asset(s) checked as %s: %d error(s), %d warning(s)."), Request.Assets.Num(), Policy.IsEmpty() ? TEXT("placeholder") : *Policy, Errors,
		Report.Issues.Num() - Errors);
	return Report;
}

FHyperAIModelInspectReport UHyperAIStudioModelingToolset::hyper_model_inspect(const FHyperAIModelInspectRequest& Request)
{
	return FHyperAIStudioModelingContracts::Inspect(Request);
}

FHyperAIModelPlanReport UHyperAIStudioModelingToolset::hyper_model_apply_plan(const FHyperAIModelPlanRequest& Request)
{
	return FHyperAIStudioModelingContracts::ApplyPlan(Request);
}

FHyperAIModelValidateReport UHyperAIStudioModelingToolset::hyper_model_validate(const FHyperAIModelValidateRequest& Request)
{
	return FHyperAIStudioModelingContracts::Validate(Request);
}

void FHyperAIStudioModelingRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioModelingRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioModelingRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioModelingToolset::StaticClass(),
			FHyperAIStudioModelingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioModeling, Warning, TEXT("Could not unregister the modeling toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioModelingRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioModelingContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioModeling, Verbose, TEXT("The modeling source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioModelingToolset::StaticClass(),
		FHyperAIStudioModelingContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioModeling, Error, TEXT("Modeling cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
