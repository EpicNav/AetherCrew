// Games by Hyper 2026.

#include "HyperAIStudioDestructionToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorUndoClient.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionActor.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionRootProxyRenderer.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioDestructionGate.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDestruction, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioDestruction"

FString FHyperAIStudioDestructionContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioDestruction.HyperAIStudioDestructionToolset");
}

TArray<FString> FHyperAIStudioDestructionContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName};
}

bool FHyperAIStudioDestructionContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::DestructionTools::Private
{
	using C = FHyperAIStudioDestructionContracts;
	namespace Gate = HyperAIStudio::Destruction::Gate;

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	/**
	 * Geometry collections a plan made, so undo and redo can keep the Content Browser in step: the transaction marks a
	 * made asset garbage on undo and alive on redo, and this tells the asset registry so.
	 */
	class FMadeAssets final : public FSelfRegisteringEditorUndoClient
	{
	public:
		void Add(UGeometryCollection* Asset)
		{
			Made.RemoveAll([](const TWeakObjectPtr<UGeometryCollection>& Each) { return Each.IsStale(/*bIncludingGarbage=*/false); });
			Made.Add(Asset);
			Listed.Add(Asset, true);
		}

		virtual void PostUndo(bool bSuccess) override { Sync(); }
		virtual void PostRedo(bool bSuccess) override { Sync(); }

	private:
		void Sync()
		{
			for (int32 Index = Made.Num() - 1; Index >= 0; --Index)
			{
				UGeometryCollection* Asset = Made[Index].Get(/*bEvenIfGarbage=*/true);
				if (!Asset)
				{
					Made.RemoveAt(Index);
					continue;
				}
				const bool bAlive = IsValid(Asset);
				bool* Known = Listed.Find(Asset);
				if (Known && *Known == bAlive) continue;
				// Undone, nothing of it is left to save; redone, it is unsaved work again.
				if (bAlive)
				{
					FAssetRegistryModule::AssetCreated(Asset);
					Asset->MarkPackageDirty();
				}
				else
				{
					FAssetRegistryModule::AssetDeleted(Asset);
					Asset->GetPackage()->SetDirtyFlag(false);
				}
				Listed.Add(Asset, bAlive);
			}
		}

		TArray<TWeakObjectPtr<UGeometryCollection>> Made;
		TMap<TWeakObjectPtr<UGeometryCollection>, bool> Listed;
	};

	/** Made on first use and let go at module shutdown, while the editor it registered with is still there. */
	TUniquePtr<FMadeAssets> GMadeAssets;

	FMadeAssets& MadeAssets()
	{
		if (!GMadeAssets) GMadeAssets = MakeUnique<FMadeAssets>();
		return *GMadeAssets;
	}

	FString ImplicitName(const EImplicitTypeEnum Type)
	{
		switch (Type)
		{
		case EImplicitTypeEnum::Chaos_Implicit_Box: return TEXT("box");
		case EImplicitTypeEnum::Chaos_Implicit_Sphere: return TEXT("sphere");
		case EImplicitTypeEnum::Chaos_Implicit_Capsule: return TEXT("capsule");
		case EImplicitTypeEnum::Chaos_Implicit_LevelSet: return TEXT("levelset");
		case EImplicitTypeEnum::Chaos_Implicit_None: return TEXT("none");
		default: return TEXT("convex");
		}
	}

	FString CollisionLine(const UGeometryCollection& Asset)
	{
		TArray<FString> Parts;
		for (const FGeometryCollectionSizeSpecificData& Data : Asset.SizeSpecificData)
		{
			if (Data.CollisionShapes.IsEmpty()) continue;
			const FGeometryCollectionCollisionTypeData& Shape = Data.CollisionShapes[0];
			const FString Size = Data.MaxSize >= 1.0e6f ? FString(TEXT("default")) : FString::Printf(TEXT("up to %.0f cm"), Data.MaxSize);
			Parts.Add(FString::Printf(TEXT("%s %s %s"), *Size, *ImplicitName(Shape.ImplicitType),
				Shape.CollisionType == ECollisionTypeEnum::Chaos_Surface_Volumetric ? TEXT("surface") : TEXT("volumetric")));
		}
		return FString::Join(Parts, TEXT(", "));
	}

	TArray<UGeometryCollectionComponent*> ComponentsUsing(UWorld& World, const UGeometryCollection& Asset)
	{
		TArray<UGeometryCollectionComponent*> Out;
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			TInlineComponentArray<UGeometryCollectionComponent*> Components(*It);
			for (UGeometryCollectionComponent* Component : Components)
			{
				if (Component && Component->GetRestCollection() == &Asset) Out.Add(Component);
			}
		}
		return Out;
	}

	FString ObjectTypeName(const EObjectStateTypeEnum Type)
	{
		switch (Type)
		{
		case EObjectStateTypeEnum::Chaos_Object_Sleeping: return TEXT("sleeping");
		case EObjectStateTypeEnum::Chaos_Object_Kinematic: return TEXT("kinematic");
		case EObjectStateTypeEnum::Chaos_Object_Static: return TEXT("static");
		case EObjectStateTypeEnum::Chaos_Object_UserDefined: return TEXT("user defined");
		default: return TEXT("dynamic");
		}
	}

	/** The plan for real: the asset and actors in one undo step, then saved if asked, then the picture. */
	FHyperAIDestructionPlanReport Execute(const FHyperAIDestructionPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIDestructionPlanReport Report;
		Report.OperationId = OperationId;
		Gate::FPlan Plan;
		FString Error;
		if (!Gate::Check(Request, Plan, Error))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Error;
			return Report;
		}
		Gate::FBuilt Built;
		if (!Gate::Build(Plan, Built, Error))
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.BuildMs = Built.BuildMs;
		Report.Notes = Built.Notes;
		UWorld* World = EditorWorld();
		UGeometryCollection* Asset = nullptr;
		TArray<AActor*> Placed;
		bool bLevelChanged = false;
		{
			const FScopedTransaction Transaction(LOCTEXT("DestructionPlan", "HyperAI: Destruction Plan"));
			if (Plan.bMake)
			{
				// What an undone plan made here is out of the way first: this plan's step ends the redo that kept it.
				if (UGeometryCollection* Undone = Gate::UndoneAt(Plan.AssetPath))
				{
					Undone->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
				}
				UPackage* Package = CreatePackage(*Plan.AssetPath);
				Asset = NewObject<UGeometryCollection>(Package, *FPackageName::GetShortName(Plan.AssetPath), RF_Public | RF_Standalone | RF_Transactional);
				// Recorded as made in this undo step: garbage when the step began, alive when it ended. Undo marks it garbage
				// again (the asset goes, as an undone actor does) and redo brings it back.
				Asset->MarkAsGarbage();
				Asset->Modify();
				Asset->ClearGarbage();
				FAssetRegistryModule::AssetCreated(Asset);
				MadeAssets().Add(Asset);
			}
			else
			{
				Asset = Plan.Existing;
				Asset->Modify();
			}
			Gate::WriteAsset(*Asset, Built, Plan.Sources);
			Asset->MarkPackageDirty();
			// Actors already using it take the asset's damage settings, as a newly placed one does.
			if (World)
			{
				for (UGeometryCollectionComponent* Component : ComponentsUsing(*World, *Asset))
				{
					Component->Modify();
					Component->ApplyAssetDefaults();
					if (Built.Settings.bDamageFromCollision.IsSet()) Component->bEnableDamageFromCollision = Built.Settings.bDamageFromCollision.GetValue();
					if (Built.Settings.State.IsSet()) Component->ObjectType = EObjectStateTypeEnum::Chaos_Object_Dynamic;
					Component->MarkRenderStateDirty();
					bLevelChanged = true;
				}
			}
			if (Plan.Place != TEXT("none") && World)
			{
				FActorSpawnParameters Spawn;
				Spawn.OverrideLevel = World->GetCurrentLevel();
				Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				Spawn.ObjectFlags = RF_Transactional;
				AGeometryCollectionActor* Actor = World->SpawnActor<AGeometryCollectionActor>(AGeometryCollectionActor::StaticClass(), Plan.PlaceTransform, Spawn);
				UGeometryCollectionComponent* Component = Actor ? Actor->GetGeometryCollectionComponent() : nullptr;
				if (!Component)
				{
					Report.Status = TEXT("operation_failed");
					Report.Diagnostic = TEXT("The geometry collection actor could not be placed; the asset is made, unsaved, and Edit > Undo takes the plan back.");
					return Report;
				}
				Component->SetRestCollection(Asset);
				if (!Asset->PhysicsMaterial && GEngine) Component->SetPhysMaterialOverride(GEngine->DefaultDestructiblePhysMaterial);
				// Dynamic, so the root's own initial state (the state op) decides how it starts, as the Dataflow node's does.
				Component->ObjectType = EObjectStateTypeEnum::Chaos_Object_Dynamic;
				if (Built.Settings.bDamageFromCollision.IsSet()) Component->bEnableDamageFromCollision = Built.Settings.bDamageFromCollision.GetValue();
				FActorLabelUtilities::SetActorLabelUnique(Actor, FPackageName::GetShortName(Plan.AssetPath));
				Actor->PostEditMove(true);
				Placed.Add(Actor);
				Report.Actors.Add(Actor->GetActorLabel());
				bLevelChanged = true;
				if (Plan.Place == TEXT("replace"))
				{
					for (const TWeakObjectPtr<AActor>& Source : Plan.SourceActors)
					{
						if (AActor* Gone = Source.Get())
						{
							Gone->Modify();
							World->EditorDestroyActor(Gone, /*bShouldModifyLevel=*/true);
						}
					}
				}
			}
		}
		Report.Asset = Asset->GetPathName();
		const Gate::FMeasure Measure = Gate::Measure(*Asset->GetGeometryCollection(), Asset->DamageThreshold);
		Report.Bones = Measure.Bones;
		Report.Geometry = Measure.Geometry;
		Report.Leaves = Measure.Leaves;
		Report.Clusters = Measure.Clusters;
		Report.Levels = Measure.Levels;
		Report.Triangles = Measure.Triangles;
		Report.Anchored = Measure.Anchored;
		Report.MassKg = Gate::MassOf(Measure, Asset->bMassAsDensity, Asset->Mass);
		Report.Size = Gate::VectorText(Measure.Bounds.GetSize());
		Report.Min = Gate::VectorText(Measure.Bounds.Min);
		Report.Max = Gate::VectorText(Measure.Bounds.Max);
		Report.InitialState = Measure.RootState;
		Report.Collision = CollisionLine(*Asset);
		int32 Sites = 0;
		for (const Gate::FStep& Step : Plan.Steps)
		{
			if (Step.Kind == TEXT("fracture.uniform")) Sites += Step.Params.Int(TEXT("Sites"));
		}
		Report.Warnings = Gate::Warn(Measure, Asset->DamageThreshold, !Placed.IsEmpty(), Sites);
		for (const FString& Fit : Plan.Fitted) Report.Notes.Add(TEXT("Fitted ") + Fit + TEXT("."));
		if (!Placed.IsEmpty())
		{
			const FBox Bounds = Measure.Bounds.TransformBy(Placed[0]->GetActorTransform());
			Report.VerifyCase = Gate::VerifyCaseJson(Report.Actors[0], Bounds.GetCenter(), FMath::Max(100.0, Bounds.GetExtent().GetMax() * 1.5),
				FMath::Max(1, Measure.Leaves / 2));
			if (Placed[0]->GetWorld() && Placed[0]->GetWorld()->IsPartitionedWorld())
			{
				// A verification copy streams a partitioned level's actors from their saved packages, so an unsaved new one is
				// not in it; Play In Editor readies the level first and has them.
				Report.Notes.Add(TEXT("This level uses World Partition: save the level and asset before hyper_verify_run, whose copies stream only saved actors, or run the steps with hyper_playtest_run."));
			}
		}
		if (Request.bSave)
		{
			if (!FHyperAIStudioSourceControl::SaveAsset(*Asset, Error))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = Error;
				return Report;
			}
			Report.Saved.Add(Asset->GetPathName());
			if (bLevelChanged && World)
			{
				if (World->GetOutermost()->GetName().StartsWith(TEXT("/Temp/")) || !UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages=*/true, /*bSaveContentPackages=*/false))
				{
					Report.Status = TEXT("save_failed");
					Report.Diagnostic = TEXT("The geometry collection is saved but the level could not be (a level never saved has nowhere to go); save it yourself.");
					return Report;
				}
				Report.Saved.Add(World->GetOutermost()->GetName());
			}
		}
		if (Request.bPreview)
		{
			FString Reason;
			if (!Gate::DrawExploded(*Asset->GetGeometryCollection(), Asset->Materials, Asset->GetPathName() + TEXT(" exploded"), Report.PreviewPath, Reason))
			{
				Report.Notes.Add(TEXT("No exploded preview: ") + Reason + TEXT("."));
			}
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%s: %d piece(s) in %d level(s), %d cluster(s), %.0f kg%s%s; %s."), *Report.Asset, Report.Leaves, Report.Levels.Num(), Report.Clusters,
			Report.MassKg, Placed.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", placed as %s"), *Report.Actors[0]),
			Plan.Place == TEXT("replace") ? TEXT(" in place of its sources") : TEXT(""),
			Request.bSave ? TEXT("saved") : TEXT("unsaved, and one Edit > Undo takes the whole plan back"));
		if (!Report.VerifyCase.IsEmpty()) Report.Diagnostic += TEXT(" Prove it breaks: hyper_verify_run with VerifyCase.");
		return Report;
	}

	FHyperAIDestructionPlanReport DryRun(const FHyperAIDestructionPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIDestructionPlanReport Report;
		Report.OperationId = OperationId;
		Gate::FPlan Plan;
		Gate::FBuilt Built;
		FString Error;
		if (!Gate::Check(Request, Plan, Error))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Error;
			return Report;
		}
		if (!Gate::Build(Plan, Built, Error))
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Error;
			return Report;
		}
		const TArray<float> Thresholds = Built.Settings.Thresholds.IsSet() ? Built.Settings.Thresholds.GetValue()
			: Plan.Existing ? Plan.Existing->DamageThreshold : TArray<float>({500000.f, 50000.f, 5000.f});
		const Gate::FMeasure Measure = Gate::Measure(Built.Collection, Thresholds);
		Report.Asset = Plan.AssetPath;
		Report.Bones = Measure.Bones;
		Report.Geometry = Measure.Geometry;
		Report.Leaves = Measure.Leaves;
		Report.Clusters = Measure.Clusters;
		Report.Levels = Measure.Levels;
		Report.Triangles = Measure.Triangles;
		Report.Anchored = Measure.Anchored;
		const bool bDensity = Built.Settings.MassKg.IsSet() ? false : Built.Settings.Density.IsSet() ? true : Plan.Existing ? Plan.Existing->bMassAsDensity : true;
		const float Mass = Built.Settings.MassKg.IsSet() ? Built.Settings.MassKg.GetValue() : Built.Settings.Density.IsSet() ? Built.Settings.Density.GetValue()
			: Plan.Existing ? Plan.Existing->Mass : 2500.f;
		Report.MassKg = Gate::MassOf(Measure, bDensity, Mass);
		Report.Size = Gate::VectorText(Measure.Bounds.GetSize());
		Report.Min = Gate::VectorText(Measure.Bounds.Min);
		Report.Max = Gate::VectorText(Measure.Bounds.Max);
		Report.InitialState = Measure.RootState;
		Report.BuildMs = Built.BuildMs;
		Report.Notes = Built.Notes;
		for (const FString& Fit : Plan.Fitted) Report.Notes.Add(TEXT("Fitted ") + Fit + TEXT("."));
		int32 Sites = 0;
		for (const Gate::FStep& Step : Plan.Steps)
		{
			if (Step.Kind == TEXT("fracture.uniform")) Sites += Step.Params.Int(TEXT("Sites"));
		}
		Report.Warnings = Gate::Warn(Measure, Thresholds, Plan.Place != TEXT("none"), Sites);
		if (Request.bPreview)
		{
			FString Reason;
			if (!Gate::DrawExploded(Built.Collection, Built.Materials, Plan.AssetPath + TEXT(" dry run exploded"), Report.PreviewPath, Reason))
			{
				Report.Notes.Add(TEXT("No exploded preview: ") + Reason + TEXT("."));
			}
		}
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) checked and built in memory in %d ms: %d piece(s) in %d level(s), %d cluster(s), %.0f kg; nothing was changed."),
			Request.Operations.Num(), Built.BuildMs, Report.Leaves, Report.Levels.Num(), Report.Clusters, Report.MassKg);
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAIDestructionPlanReport& Done)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::PlanToolName;
		Entry.Target = Done.Asset;
		Entry.OperationId = OperationId;
		Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Done.Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}
}

FHyperAIDestructionInspectReport FHyperAIStudioDestructionContracts::Inspect(const FHyperAIDestructionInspectRequest& Request)
{
	using namespace HyperAIStudio::DestructionTools::Private;
	FHyperAIDestructionInspectReport Report;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	UWorld* World = EditorWorld();
	UGeometryCollection* Asset = nullptr;
	if (!Request.Actor.TrimStartAndEnd().IsEmpty())
	{
		if (!World)
		{
			Report.Diagnostic = TEXT("No level is open in the editor.");
			return Report;
		}
		for (TActorIterator<AActor> It(World); It && !Asset; ++It)
		{
			if (!It->GetActorLabel().Equals(Request.Actor.TrimStartAndEnd(), ESearchCase::IgnoreCase)) continue;
			TInlineComponentArray<UGeometryCollectionComponent*> Components(*It);
			if (Components.IsEmpty() || !Components[0]->GetRestCollection())
			{
				Report.Diagnostic = FString::Printf(TEXT("%s has no geometry collection."), *Request.Actor);
				return Report;
			}
			Asset = const_cast<UGeometryCollection*>(Components[0]->GetRestCollection());
		}
		if (!Asset)
		{
			Report.Diagnostic = FString::Printf(TEXT("No actor in the open level is labelled %s."), *Request.Actor);
			return Report;
		}
	}
	else if (!Request.Asset.TrimStartAndEnd().IsEmpty())
	{
		FString Reason;
		const FString Path = Request.Asset.TrimStartAndEnd();
		if (!FHyperAIStudioNames::IsObjectPath(Path, TEXT("Asset"), &Reason))
		{
			Report.Diagnostic = Reason;
			return Report;
		}
		const FString ObjectPath = Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
		Asset = FindObject<UGeometryCollection>(nullptr, *ObjectPath);
		if (!Asset && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ObjectPath)))
		{
			Asset = LoadObject<UGeometryCollection>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (!Asset)
		{
			Report.Diagnostic = FString::Printf(TEXT("%s is not a geometry collection."), *Path);
			return Report;
		}
	}
	if (!Asset)
	{
		for (const HyperAIStudio::Destruction::Gate::FKindSpec& Spec : HyperAIStudio::Destruction::Gate::Kinds())
		{
			FHyperAIDestructionKind& Kind = Report.Kinds.AddDefaulted_GetRef();
			Kind.Kind = Spec.Kind;
			Kind.Summary = Spec.Summary;
			Kind.Params = HyperAIStudio::Destruction::Gate::DescribeParams(Spec);
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(UGeometryCollection::StaticClass()->GetClassPathName(), Assets);
		for (const FAssetData& Each : Assets)
		{
			const FString Path = Each.GetObjectPathString();
			if (Path.StartsWith(TEXT("/Game/")) && Report.Assets.Num() < 200) Report.Assets.Add(Path);
		}
		Report.Assets.Sort();
		Report.bOk = true;
		Report.Diagnostic = FString::Printf(TEXT("%d op kind(s) hyper_destruction_apply_plan takes, and %d geometry collection(s) under /Game; give Asset or Actor to read one."),
			Report.Kinds.Num(), Report.Assets.Num());
		return Report;
	}
	if (!Asset->GetGeometryCollection().IsValid())
	{
		Report.Diagnostic = FString::Printf(TEXT("%s holds no collection."), *Asset->GetPathName());
		return Report;
	}
	namespace Gate = HyperAIStudio::Destruction::Gate;
	const int32 Listed = FMath::Clamp(Request.MaxPieces, 0, MaxListedPieces);
	const Gate::FMeasure Measure = Gate::Measure(*Asset->GetGeometryCollection(), Asset->DamageThreshold, Listed);
	Report.Asset = Asset->GetPathName();
	Report.Bones = Measure.Bones;
	Report.Geometry = Measure.Geometry;
	Report.Leaves = Measure.Leaves;
	Report.Clusters = Measure.Clusters;
	Report.Levels = Measure.Levels;
	Report.Triangles = Measure.Triangles;
	Report.MassKg = Gate::MassOf(Measure, Asset->bMassAsDensity, Asset->Mass);
	Report.Size = Gate::VectorText(Measure.Bounds.GetSize());
	Report.DamageModel = Asset->DamageModel == EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold ? TEXT("thresholds per level")
		: TEXT("material strength and connectivity");
	Report.DamageThresholds = Asset->DamageThreshold;
	Report.bSizeSpecificDamage = Asset->bUseSizeSpecificDamageThreshold;
	for (const FGeometryCollectionSizeSpecificData& Data : Asset->SizeSpecificData)
	{
		if (Data.CollisionShapes.IsEmpty()) continue;
		FHyperAIDestructionCollision& Entry = Report.Collision.AddDefaulted_GetRef();
		Entry.MaxSize = Data.MaxSize;
		Entry.Type = Data.CollisionShapes[0].CollisionType == ECollisionTypeEnum::Chaos_Surface_Volumetric ? TEXT("surface") : TEXT("volumetric");
		Entry.Implicit = ImplicitName(Data.CollisionShapes[0].ImplicitType);
		Entry.DamageThreshold = Data.DamageThreshold;
	}
	const FGeometryCollection& Collection = *Asset->GetGeometryCollection();
	for (int32 Slot = 0; Slot < Asset->Materials.Num(); ++Slot)
	{
		FHyperAIDestructionMaterialSlot& Entry = Report.Materials.AddDefaulted_GetRef();
		Entry.Slot = Slot;
		Entry.Material = Asset->Materials[Slot] ? Asset->Materials[Slot]->GetPathName() : FString(TEXT("None"));
	}
	for (int32 Face = 0; Face < Collection.MaterialID.Num(); ++Face)
	{
		if (!Report.Materials.IsValidIndex(Collection.MaterialID[Face])) continue;
		++(Collection.Internal[Face] ? Report.Materials[Collection.MaterialID[Face]].InnerFaces : Report.Materials[Collection.MaterialID[Face]].OuterFaces);
	}
	// The bone-selection material Unreal appends to every collection is not the user's; it is dropped when no face uses it.
	Report.Materials.RemoveAll([](const FHyperAIDestructionMaterialSlot& Slot) { return Slot.OuterFaces == 0 && Slot.InnerFaces == 0 && Slot.Material.Contains(TEXT("BoneSelected")); });
	Report.Anchored = Measure.Anchored;
	Report.InitialState = Measure.RootState;
	Report.bNanite = Asset->EnableNanite;
	Report.Renderer = Asset->CustomRendererType == UGeometryCollectionRootProxyRenderer::StaticClass() ? TEXT("root_proxy")
		: Asset->CustomRendererType ? Asset->CustomRendererType->GetName() : TEXT("default");
	for (const TObjectPtr<UStaticMesh>& Proxy : Asset->RootProxyData.ProxyMeshes)
	{
		if (Proxy) Report.ProxyMeshes.Add(Proxy->GetPathName());
	}
	if (World)
	{
		for (UGeometryCollectionComponent* Component : ComponentsUsing(*World, *Asset))
		{
			Report.Actors.Add(FString::Printf(TEXT("%s (%s, damage from collisions %s)"), Component->GetOwner() ? *Component->GetOwner()->GetActorLabel() : *Component->GetName(),
				*ObjectTypeName(Component->ObjectType), Component->bEnableDamageFromCollision ? TEXT("on") : TEXT("off")));
		}
	}
	Report.Pieces = Measure.Pieces;
	Report.Warnings = Gate::Warn(Measure, Asset->DamageThreshold, !Report.Actors.IsEmpty(), 0);
	Report.bOk = true;
	Report.Diagnostic = FString::Printf(TEXT("%s: %d piece(s) in %d level(s), %d cluster(s), %d bone(s), %.0f kg, %d anchored, starts %s; collision %s."), *Report.Asset, Report.Leaves,
		Report.Levels.Num(), Report.Clusters, Report.Bones, Report.MassKg, Report.Anchored, *Report.InitialState, *CollisionLine(*Asset));
	return Report;
}

FHyperAIDestructionPlanReport FHyperAIStudioDestructionContracts::ApplyPlan(const FHyperAIDestructionPlanRequest& Request)
{
	using namespace HyperAIStudio::DestructionTools::Private;
	FHyperAIDestructionPlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("destruction-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report, {TEXT("Params")})) return Report;
	if (GEditor && GEditor->PlayWorld)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Play In Editor is running; stop it first, then make or change the geometry collection.");
		return Report;
	}
	HyperAIStudio::Destruction::Gate::FPlan Plan;
	FString Error;
	if (!HyperAIStudio::Destruction::Gate::Check(Request, Plan, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	if (Request.bDryRun) return DryRun(Request, Report.OperationId);
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = PlanToolName;
		Summary.VariantId = TEXT("destruction.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Plan.AssetPath;
		Summary.Effects.Add(FString::Printf(TEXT("%s the geometry collection %s: %d operation(s), one undo step%s"), Plan.bMake ? TEXT("Make") : TEXT("Change"), *Plan.AssetPath,
			Request.Operations.Num(), Request.bSave ? TEXT(", then saved") : TEXT("")));
		for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
		{
			const FHyperAIDestructionOp& Op = Request.Operations[Index];
			Summary.Effects.Add(FString::Printf(TEXT("| %s %s"), *Op.Kind, *Op.Params.Left(120)));
		}
		if (Plan.Place == TEXT("replace")) Summary.Effects.Add(FString::Printf(TEXT("Replace %d level actor(s) with the geometry collection"), Plan.SourceActors.Num()));
		Summary.Touches.Add(Plan.AssetPath);
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				// The editor may have moved on while the plan waited (an asset made at the path, an actor deleted), so
				// Execute checks the plan again before anything changes.
				Record(OperationId, Execute(Request, OperationId));
			}, Error))
		{
			Report.Status = TEXT("approval_not_queued");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Asset = Plan.AssetPath;
		Report.Diagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
		FHyperAIStudioApprovalGate::DescribeHeld(OperationId, Report.Status, Report.Diagnostic);
		return Report;
	}
	Report = Execute(Request, Report.OperationId);
	Record(Report.OperationId, Report);
	return Report;
}

FHyperAIDestructionInspectReport UHyperAIStudioDestructionToolset::hyper_destruction_inspect(const FHyperAIDestructionInspectRequest& Request)
{
	return FHyperAIStudioDestructionContracts::Inspect(Request);
}

FHyperAIDestructionPlanReport UHyperAIStudioDestructionToolset::hyper_destruction_apply_plan(const FHyperAIDestructionPlanRequest& Request)
{
	return FHyperAIStudioDestructionContracts::ApplyPlan(Request);
}

void FHyperAIStudioDestructionRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioDestructionRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioDestructionRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioDestructionToolset::StaticClass(),
			FHyperAIStudioDestructionContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioDestruction, Warning, TEXT("Could not unregister the destruction toolset: %s"), *Error);
		}
	}
	HyperAIStudio::DestructionTools::Private::GMadeAssets.Reset();
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioDestructionRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioDestructionContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioDestruction, Verbose, TEXT("The destruction source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioDestructionToolset::StaticClass(),
		FHyperAIStudioDestructionContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioDestruction, Error, TEXT("Destruction cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
