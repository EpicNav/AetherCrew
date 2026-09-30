// Games by Hyper 2026.

#include "HyperAIStudioPCGScatterToolset.h"

#include "ActorFactories/ActorFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSourceControl.h"
#include "LandscapeProxy.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "PCGCommon.h"
#include "Data/PCGBasePointData.h"
#include "Helpers/PCGHelpers.h"
#include "PCGComponent.h"
#include "PCGData.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "PCGVolume.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPCGScatter, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioPCGScatter"

FString FHyperAIStudioPCGScatterContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioPCGScatter.HyperAIStudioPCGScatterToolset");
}

TArray<FString> FHyperAIStudioPCGScatterContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName};
}

bool FHyperAIStudioPCGScatterContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::PCGScatterTools::Private
{
	using C = FHyperAIStudioPCGScatterContracts;

	/** The scatter graph's nodes, in the order points flow through them. */
	enum class EStep : uint8 { Landscape, Sampler, Normal, Filter, Prune, Transform, Spawner, Count };

	const TCHAR* StepClass(const EStep Step)
	{
		switch (Step)
		{
		case EStep::Landscape: return TEXT("/Script/PCG.PCGGetLandscapeSettings");
		case EStep::Sampler: return TEXT("/Script/PCG.PCGSurfaceSamplerSettings");
		case EStep::Normal: return TEXT("/Script/PCG.PCGNormalToDensitySettings");
		case EStep::Filter: return TEXT("/Script/PCG.PCGDensityFilterSettings");
		case EStep::Prune: return TEXT("/Script/PCG.PCGSelfPruningSettings");
		case EStep::Transform: return TEXT("/Script/PCG.PCGTransformPointsSettings");
		case EStep::Spawner: return TEXT("/Script/PCG.PCGStaticMeshSpawnerSettings");
		default: return TEXT("");
		}
	}

	/** Volumes stand this tall unless the op says otherwise, meters. */
	constexpr float DefaultHeight = 500.f;
	constexpr int32 GenerateDeadlineMs = 5 * 60 * 1000;

	FString KindOf(const FHyperAIPCGScatterOp& Op)
	{
		return Op.Kind.TrimStartAndEnd().ToLower();
	}

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	template <typename T>
	T* Load(const FString& Path)
	{
		const FString Trimmed = Path.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !Trimmed.StartsWith(TEXT("/"))) return nullptr;
		const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
		return LoadObject<T>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	FString PackageOf(const FString& Reference)
	{
		FString Path = Reference.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Dot)) Path.LeftInline(Dot);
		return Path;
	}

	APCGVolume* FindVolume(UWorld& World, const FString& Label)
	{
		for (TActorIterator<APCGVolume> It(&World); It; ++It)
		{
			if (It->GetActorLabel().Equals(Label.TrimStartAndEnd(), ESearchCase::IgnoreCase)) return *It;
		}
		return nullptr;
	}

	bool ParseFlag(const FString& Text, TOptional<bool>& Out)
	{
		const FString Value = Text.TrimStartAndEnd().ToLower();
		if (Value.IsEmpty()) return true;
		if (Value == TEXT("true") || Value == TEXT("yes") || Value == TEXT("1")) Out = true;
		else if (Value == TEXT("false") || Value == TEXT("no") || Value == TEXT("0")) Out = false;
		else return false;
		return true;
	}

	/** One property in Unreal's text form, as the details panel would write it, so the graph hears of the change. */
	bool SetProperty(UObject& Object, const TCHAR* Name, const FString& Value, FString& OutError)
	{
		FProperty* Property = Object.GetClass()->FindPropertyByName(Name);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("%s has no property %s in this engine."), *Object.GetClass()->GetName(), Name);
			return false;
		}
		Object.PreEditChange(Property);
		const TCHAR* Rest = Property->ImportText_InContainer(*Value, &Object, &Object, PPF_None);
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Object.PostEditChangeProperty(Event);
		if (!Rest)
		{
			OutError = FString::Printf(TEXT("'%s' is not a value for %s.%s."), *Value, *Object.GetClass()->GetName(), Name);
			return false;
		}
		return true;
	}

	FString ReadProperty(const UObject& Object, const TCHAR* Name)
	{
		const FProperty* Property = Object.GetClass()->FindPropertyByName(Name);
		FString Value;
		if (Property) Property->ExportText_InContainer(0, Value, &Object, &Object, const_cast<UObject*>(&Object), PPF_None);
		return Value;
	}

	/** The graph's node for a step, when the graph is one this tool built. */
	UPCGSettings* SettingsFor(const UPCGGraph& Graph, const EStep Step)
	{
		const UClass* Class = FindObject<UClass>(nullptr, StepClass(Step));
		for (UPCGNode* Node : Graph.GetNodes())
		{
			UPCGSettings* Settings = Node ? Node->GetSettings() : nullptr;
			if (Settings && Class && Settings->GetClass() == Class) return Settings;
		}
		return nullptr;
	}

	bool IsScatterGraph(const UPCGGraph& Graph)
	{
		for (int32 Step = 0; Step < static_cast<int32>(EStep::Count); ++Step)
		{
			if (!SettingsFor(Graph, static_cast<EStep>(Step))) return false;
		}
		return true;
	}

	/**
	 * The scatter chain: the landscape is sampled inside the volume, points steeper than the slope are dropped by
	 * turning each normal's height into a density and filtering it, crowded points are pruned, then the rest are scaled,
	 * turned and spawned as meshes.
	 */
	bool BuildGraph(UPCGGraph& Graph, FString& OutError)
	{
		UPCGNode* Nodes[static_cast<int32>(EStep::Count)] = {};
		for (int32 Step = 0; Step < static_cast<int32>(EStep::Count); ++Step)
		{
			UClass* Class = FindObject<UClass>(nullptr, StepClass(static_cast<EStep>(Step)));
			UPCGSettings* Settings = nullptr;
			Nodes[Step] = Class ? Graph.AddNodeOfType(Class, Settings) : nullptr;
			if (!Nodes[Step])
			{
				OutError = FString::Printf(TEXT("This engine's PCG has no %s node."), StepClass(static_cast<EStep>(Step)));
				return false;
			}
			// Laid out left to right in the graph editor, the landscape getter above the sampler.
			Nodes[Step]->SetNodePosition(Step == 0 ? 0 : Step * 300, Step == 0 ? -200 : 0);
		}
		const FName In = PCGPinConstants::DefaultInputLabel;
		const FName Out = PCGPinConstants::DefaultOutputLabel;
		auto Node = [&Nodes](const EStep Step) { return Nodes[static_cast<int32>(Step)]; };
		bool bLinked = Graph.AddEdge(Graph.GetInputNode(), In, Node(EStep::Sampler), TEXT("Bounding Shape")) != nullptr;
		bLinked &= Graph.AddEdge(Node(EStep::Landscape), Out, Node(EStep::Sampler), TEXT("Surface")) != nullptr;
		for (int32 Step = static_cast<int32>(EStep::Sampler); Step < static_cast<int32>(EStep::Spawner); ++Step)
		{
			bLinked &= Graph.AddEdge(Nodes[Step], Out, Nodes[Step + 1], In) != nullptr;
		}
		bLinked &= Graph.AddEdge(Node(EStep::Spawner), Out, Graph.GetOutputNode(), Out) != nullptr;
		if (!bLinked) OutError = TEXT("The scatter graph's nodes would not connect.");
		return bLinked;
	}

	/** The op's values onto a scatter graph; zero and negative values leave a value as it is. */
	bool ShapeGraph(UPCGGraph& Graph, const FHyperAIPCGScatterOp& Op, const bool bNew, FString& OutError)
	{
		UPCGSettings* Sampler = SettingsFor(Graph, EStep::Sampler);
		UPCGSettings* Normal = SettingsFor(Graph, EStep::Normal);
		UPCGSettings* Filter = SettingsFor(Graph, EStep::Filter);
		UPCGSettings* Transform = SettingsFor(Graph, EStep::Transform);
		UPCGSettings* Spawner = SettingsFor(Graph, EStep::Spawner);
		if (!Sampler || !Normal || !Filter || !Transform || !Spawner)
		{
			OutError = FString::Printf(TEXT("%s is not a scatter graph this tool built; edit it in the PCG graph editor or with Epic's PCG tools."), *Graph.GetName());
			return false;
		}
		const float Density = Op.Density > 0.f ? Op.Density : (bNew ? 0.1f : 0.f);
		if (Density > 0.f && !SetProperty(*Sampler, TEXT("PointsPerSquaredMeter"), FString::SanitizeFloat(Density), OutError)) return false;
		const float Spacing = Op.Spacing >= 0.f ? Op.Spacing : (bNew ? 0.f : -1.f);
		if (Spacing >= 0.f)
		{
			// A point's box is its footprint: pruning keeps boxes from overlapping, so half the spacing each way spaces them.
			const double Half = FMath::Max(Spacing * 50.0, 0.5);
			if (!SetProperty(*Sampler, TEXT("PointExtents"), FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), Half, Half, Half), OutError)) return false;
		}
		if (Op.Seed != 0 && !SetProperty(*Sampler, TEXT("Seed"), FString::FromInt(Op.Seed), OutError)) return false;
		if (bNew && !SetProperty(*Normal, TEXT("Normal"), TEXT("(X=0,Y=0,Z=1)"), OutError)) return false;
		const float Slope = Op.SlopeMax >= 0.f ? Op.SlopeMax : (bNew ? 0.f : -1.f);
		if (Slope >= 0.f)
		{
			// The density is the normal's height: a slope of S degrees leaves cos(S).
			const double Lowest = Slope > 0.f ? FMath::Cos(FMath::DegreesToRadians(static_cast<double>(Slope))) : 0.0;
			if (!SetProperty(*Filter, TEXT("LowerBound"), FString::SanitizeFloat(Lowest), OutError) || !SetProperty(*Filter, TEXT("UpperBound"), TEXT("1.0"), OutError)) return false;
		}
		const FVector2D Scale = Op.Scale.X > 0.0 && Op.Scale.Y > 0.0 ? Op.Scale : (bNew ? FVector2D(0.8, 1.2) : FVector2D::ZeroVector);
		if (Scale.X > 0.0)
		{
			if (!SetProperty(*Transform, TEXT("bUniformScale"), TEXT("True"), OutError)
				|| !SetProperty(*Transform, TEXT("ScaleMin"), FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), Scale.X, Scale.X, Scale.X), OutError)
				|| !SetProperty(*Transform, TEXT("ScaleMax"), FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), Scale.Y, Scale.Y, Scale.Y), OutError))
			{
				return false;
			}
		}
		TOptional<bool> Yaw;
		ParseFlag(Op.RandomYaw, Yaw);
		if (!Yaw.IsSet() && bNew) Yaw = true;
		if (Yaw.IsSet() && !SetProperty(*Transform, TEXT("RotationMax"), Yaw.GetValue() ? TEXT("(Pitch=0,Yaw=360,Roll=0)") : TEXT("(Pitch=0,Yaw=0,Roll=0)"), OutError)) return false;
		if (!Op.Meshes.IsEmpty())
		{
			UObject* Selector = nullptr;
			if (const FObjectPropertyBase* Parameters = CastField<FObjectPropertyBase>(Spawner->GetClass()->FindPropertyByName(TEXT("MeshSelectorParameters"))))
			{
				Selector = Parameters->GetObjectPropertyValue_InContainer(Spawner);
			}
			if (!Selector || !Selector->GetClass()->FindPropertyByName(TEXT("MeshEntries")))
			{
				OutError = TEXT("The mesh spawner does not pick by weight; set its meshes in the PCG graph editor.");
				return false;
			}
			TArray<FString> Entries;
			for (const FHyperAIPCGScatterMesh& Mesh : Op.Meshes)
			{
				const UStaticMesh* Asset = Load<UStaticMesh>(Mesh.Mesh);
				Entries.Add(FString::Printf(TEXT("(Descriptor=(StaticMesh=\"%s\"),Weight=%d)"), Asset ? *Asset->GetPathName() : *Mesh.Mesh, FMath::Max(Mesh.Weight, 1)));
			}
			Spawner->Modify();
			if (!SetProperty(*Selector, TEXT("MeshEntries"), TEXT("(") + FString::Join(Entries, TEXT(",")) + TEXT(")"), OutError)) return false;
			// UPCGSettings keeps its override protected; UObject's is public and reaches it.
			FPropertyChangedEvent Event(Spawner->GetClass()->FindPropertyByName(TEXT("MeshSelectorParameters")), EPropertyChangeType::ValueSet);
			static_cast<UObject*>(Spawner)->PostEditChangeProperty(Event);
		}
		return true;
	}

	TArray<FString> DescribeGraph(const UPCGGraph& Graph)
	{
		TArray<FString> Lines;
		const UPCGSettings* Sampler = SettingsFor(Graph, EStep::Sampler);
		const UPCGSettings* Filter = SettingsFor(Graph, EStep::Filter);
		const UPCGSettings* Transform = SettingsFor(Graph, EStep::Transform);
		const UPCGSettings* Spawner = SettingsFor(Graph, EStep::Spawner);
		if (!Sampler || !Filter || !Transform || !Spawner)
		{
			Lines.Add(FString::Printf(TEXT("%s is not a scatter graph this tool built: %d node(s)."), *Graph.GetName(), Graph.GetNodes().Num()));
			return Lines;
		}
		const double Lowest = FCString::Atod(*ReadProperty(*Filter, TEXT("LowerBound")));
		Lines.Add(TEXT("Density ") + ReadProperty(*Sampler, TEXT("PointsPerSquaredMeter")) + TEXT(" per m2"));
		Lines.Add(TEXT("Point extents ") + ReadProperty(*Sampler, TEXT("PointExtents")) + TEXT(" cm (spacing is twice them)"));
		Lines.Add(Lowest > 0.0 ? FString::Printf(TEXT("Slope up to %.0f degrees"), FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Lowest, 0.0, 1.0)))) : FString(TEXT("Any slope")));
		Lines.Add(TEXT("Scale ") + ReadProperty(*Transform, TEXT("ScaleMin")) + TEXT(" to ") + ReadProperty(*Transform, TEXT("ScaleMax")));
		Lines.Add(TEXT("Rotation up to ") + ReadProperty(*Transform, TEXT("RotationMax")));
		Lines.Add(TEXT("Seed ") + ReadProperty(*Sampler, TEXT("Seed")));
		if (const FObjectPropertyBase* Parameters = CastField<FObjectPropertyBase>(Spawner->GetClass()->FindPropertyByName(TEXT("MeshSelectorParameters"))))
		{
			if (const UObject* Selector = Parameters->GetObjectPropertyValue_InContainer(Spawner)) Lines.Add(TEXT("Meshes ") + ReadProperty(*Selector, TEXT("MeshEntries")).Left(1500));
		}
		return Lines;
	}

	/** Instances the volume's own generation made, over every instanced mesh component it owns. */
	int32 CountInstances(const APCGVolume& Volume, int32* OutMeshes = nullptr)
	{
		TArray<UInstancedStaticMeshComponent*> Components;
		Volume.GetComponents<UInstancedStaticMeshComponent>(Components);
		int32 Instances = 0;
		int32 Meshes = 0;
		for (const UInstancedStaticMeshComponent* Component : Components)
		{
			if (!Component || Component->GetInstanceCount() == 0) continue;
			Instances += Component->GetInstanceCount();
			++Meshes;
		}
		if (OutMeshes) *OutMeshes = Meshes;
		return Instances;
	}

	/**
	 * Landscapes under the volume that PCG passes over. A landscape split into streaming proxies is measured from its
	 * loaded parts, but one kept whole is measured in a World Partition level from what the level has saved of it.
	 */
	TArray<FString> UnseenLandscapes(const APCGVolume& Volume)
	{
		TArray<FString> Labels;
		UWorld* World = Volume.GetWorld();
		const FBox Bounds = Volume.GetBrushComponent() ? Volume.GetBrushComponent()->Bounds.GetBox() : FBox(EForceInit::ForceInit);
		for (TActorIterator<ALandscapeProxy> It(World); World && Bounds.IsValid && It; ++It)
		{
			const FBox Ground = It->GetProxyBounds();
			if (!Ground.IsValid || !Ground.Intersect(Bounds)) continue;
			const FBox Seen = PCGHelpers::GetLandscapeBounds(*It);
			if (!Seen.IsValid || !Seen.Intersect(Bounds)) Labels.Add(It->GetActorLabel());
		}
		return Labels;
	}

	/** A generation started, followed as an Activity panel job that hyper_operation_wait waits on. */
	bool StartGenerating(APCGVolume& Volume, const FString& OperationId, FHyperAIPCGScatterPlanReport& Report, FString& OutError)
	{
		UPCGComponent* Component = Volume.PCGComponent;
		if (!Component || !Component->GetGraph())
		{
			OutError = FString::Printf(TEXT("%s runs no graph."), *Volume.GetActorLabel());
			return false;
		}
		// PCG declines quietly (no graph, inactive, no subsystem): catch that before a job is shown.
		if (Component->GenerateLocalGetTaskId(/*bForce=*/true) == InvalidPCGTaskId)
		{
			OutError = FString::Printf(TEXT("PCG would not start generating %s; check that its component is active and the PCG plugin is running."), *Volume.GetActorLabel());
			return false;
		}
		TWeakObjectPtr<APCGVolume> Weak(&Volume);
		FHyperAIStudioAsyncJobRequest Request;
		Request.PackId = C::PackId;
		Request.ToolName = C::PlanToolName;
		Request.Target = Volume.GetActorLabel();
		Request.OperationId = OperationId;
		Request.DeadlineMs = GenerateDeadlineMs;
		Request.PollIntervalMs = 250;
		FString JobId;
		if (!FHyperAIStudioAsyncJobHost::Start(Request, [Weak](FString& OutProgress, FString& OutDiagnostic)
			{
				const APCGVolume* Pinned = Weak.Get();
				if (!Pinned || !Pinned->PCGComponent)
				{
					OutDiagnostic = TEXT("The volume is gone.");
					return EHyperAIStudioAsyncJobPoll::Failed;
				}
				if (Pinned->PCGComponent->IsGenerating())
				{
					OutProgress = TEXT("generating");
					return EHyperAIStudioAsyncJobPoll::Running;
				}
				int32 Meshes = 0;
				const int32 Instances = CountInstances(*Pinned, &Meshes);
				// The points that reached the graph's end tell an empty sample from an empty spawn.
				int32 Points = 0;
				for (const FPCGTaggedData& Data : Pinned->PCGComponent->GetGeneratedGraphOutput().TaggedData)
				{
					if (const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(Data.Data)) Points += PointData->GetNumPoints();
				}
				FString Why;
				if (Points == 0)
				{
					const TArray<FString> Unseen = UnseenLandscapes(*Pinned);
					Why = Unseen.IsEmpty()
						? FString(TEXT(": no point survived, so check that a landscape lies inside the volume and its ground is within the slope limit"))
						: FString::Printf(TEXT(": PCG passed over %s, kept whole rather than split into streaming proxies; World Partition shows PCG such a landscape only once the level is saved, so save it (bSave) and generate again"),
							*FString::Join(Unseen, TEXT(", ")));
				}
				OutDiagnostic = FString::Printf(TEXT("%s generated %d instance(s) of %d mesh(es) from %d point(s)%s."), *Pinned->GetActorLabel(), Instances, Meshes, Points, *Why);
				return EHyperAIStudioAsyncJobPoll::Completed;
			}, JobId, OutError))
		{
			return false;
		}
		Report.Notes.Add(FString::Printf(TEXT("%s is generating %s."), *Volume.GetActorLabel(), *Component->GetGraph()->GetName()));
		return true;
	}

	// --- Plan -----------------------------------------------------------------------------------------------------

	bool Check(UWorld& World, const FHyperAIPCGScatterPlanRequest& Request, FString& OutError)
	{
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
			return false;
		}
		TSet<FString> NewGraphs;
		TSet<FString> NewVolumes;
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAIPCGScatterOp& Op = Request.Operations[Index];
			const FString Kind = KindOf(Op);
			auto Fail = [&OutError, &Op, Index](const FString& Message)
			{
				OutError = FString::Printf(TEXT("Operation %d (%s): %s Nothing was changed."), Index, *Op.Kind, *Message);
				return false;
			};
			const FString Path = PackageOf(Op.Graph);
			const bool bPlannedGraph = NewGraphs.Contains(Path.ToLower());
			if (Op.Density < 0.f || Op.Density > C::MaxDensity) return Fail(FString::Printf(TEXT("Density is 0 to %.0f points per square meter."), C::MaxDensity));
			if (Op.SlopeMax > 90.f) return Fail(TEXT("SlopeMax is 0 to 90 degrees."));
			if ((Op.Scale.X != 0.0 || Op.Scale.Y != 0.0) && !(Op.Scale.X > 0.0 && Op.Scale.Y >= Op.Scale.X)) return Fail(TEXT("Scale runs from a Min (X) above 0 to a Max (Y) at least as big."));
			TOptional<bool> Yaw;
			if (!ParseFlag(Op.RandomYaw, Yaw)) return Fail(TEXT("RandomYaw is true or false."));
			for (const FHyperAIPCGScatterMesh& Mesh : Op.Meshes)
			{
				if (!Load<UStaticMesh>(Mesh.Mesh)) return Fail(FString::Printf(TEXT("%s is not a static mesh."), *Mesh.Mesh));
			}
			if (Kind == TEXT("graph.create"))
			{
				if (!Path.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Path) || FPackageName::DoesPackageExist(Path) || bPlannedGraph)
				{
					return Fail(TEXT("Name the new graph as a free path under /Game, like /Game/PCG/PCG_Forest."));
				}
				if (Op.Meshes.IsEmpty()) return Fail(TEXT("Give the Meshes to scatter."));
				NewGraphs.Add(Path.ToLower());
			}
			else if (Kind == TEXT("graph.set"))
			{
				const UPCGGraph* Graph = Load<UPCGGraph>(Path);
				if (!Graph && !bPlannedGraph) return Fail(FString::Printf(TEXT("%s is not a PCG graph."), *Op.Graph));
				if (Graph && !IsScatterGraph(*Graph)) return Fail(FString::Printf(TEXT("%s is not a scatter graph this tool built."), *Op.Graph));
			}
			else if (Kind == TEXT("volume.create"))
			{
				if (Op.Volume.TrimStartAndEnd().IsEmpty()) return Fail(TEXT("Name the Volume."));
				if (FindVolume(World, Op.Volume) || NewVolumes.Contains(Op.Volume.ToLower())) return Fail(FString::Printf(TEXT("A PCG volume is labelled %s already."), *Op.Volume));
				if (!(Op.Size.X > 0.0 && Op.Size.Y > 0.0)) return Fail(TEXT("Give the volume's Size in meters."));
				if (Op.Height < 0.f) return Fail(TEXT("Height is 0 (500 m) or more, in meters."));
				if (!Load<UPCGGraph>(Path) && !bPlannedGraph) return Fail(FString::Printf(TEXT("%s is not a PCG graph."), *Op.Graph));
				if (Index != Request.Operations.Num() - 1) return Fail(TEXT("A plan generates once, as its last op."));
				NewVolumes.Add(Op.Volume.ToLower());
			}
			else if (Kind == TEXT("volume.generate") || Kind == TEXT("volume.clear"))
			{
				const APCGVolume* Volume = FindVolume(World, Op.Volume);
				if (!Volume) return Fail(FString::Printf(TEXT("No PCG volume is labelled %s."), *Op.Volume));
				if (Kind == TEXT("volume.generate") && Index != Request.Operations.Num() - 1) return Fail(TEXT("A plan generates once, as its last op."));
				if (Kind == TEXT("volume.generate") && Volume->PCGComponent && Volume->PCGComponent->IsGenerating()) return Fail(TEXT("It is generating already."));
			}
			else
			{
				return Fail(TEXT("Unknown op; see hyper_pcg_scatter_apply_plan's description."));
			}
		}
		return true;
	}

	bool SaveAsset(UObject& Asset, FString& OutError)
	{
		UPackage* Package = Asset.GetOutermost();
		if (!FHyperAIStudioSourceControl::MakeEditable({Package->GetName()}, OutError)) return false;
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(Package, &Asset, *File, Args))
		{
			OutError = FString::Printf(TEXT("%s could not be saved."), *Package->GetName());
			return false;
		}
		return true;
	}

	/** One op; graphs are saved as they change, and a volume to generate is handed back to start once the plan is done. */
	bool Apply(UWorld& World, const FHyperAIPCGScatterOp& Op, FHyperAIPCGScatterPlanReport& Report, APCGVolume*& OutGenerate, FString& OutError)
	{
		const FString Kind = KindOf(Op);
		const FString Path = PackageOf(Op.Graph);
		if (Kind == TEXT("graph.create") || Kind == TEXT("graph.set"))
		{
			UPCGGraph* Graph = nullptr;
			const bool bNew = Kind == TEXT("graph.create");
			if (bNew)
			{
				UPackage* Package = CreatePackage(*Path);
				Graph = NewObject<UPCGGraph>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
				if (!BuildGraph(*Graph, OutError)) return false;
				FAssetRegistryModule::AssetCreated(Graph);
			}
			else
			{
				Graph = Load<UPCGGraph>(Path);
				Graph->Modify();
			}
			if (!ShapeGraph(*Graph, Op, bNew, OutError)) return false;
			Graph->MarkPackageDirty();
			if (!SaveAsset(*Graph, OutError)) return false;
			Report.Saved.Add(Graph->GetPathName());
			Report.Notes.Add(FString::Printf(TEXT("Graph %s: %s."), *Graph->GetName(), *FString::Join(DescribeGraph(*Graph), TEXT("; ")).Left(800)));
			return true;
		}
		if (Kind == TEXT("volume.create"))
		{
			UPCGGraph* Graph = Load<UPCGGraph>(Path);
			FActorSpawnParameters Parameters;
			Parameters.ObjectFlags |= RF_Transactional;
			APCGVolume* Volume = Graph ? World.SpawnActor<APCGVolume>(Op.Center, FRotator::ZeroRotator, Parameters) : nullptr;
			if (!Volume)
			{
				OutError = TEXT("The level refused a new PCG volume.");
				return false;
			}
			UCubeBuilder* Box = NewObject<UCubeBuilder>();
			Box->X = static_cast<float>(Op.Size.X * 100.0);
			Box->Y = static_cast<float>(Op.Size.Y * 100.0);
			Box->Z = (Op.Height > 0.f ? Op.Height : DefaultHeight) * 100.f;
			UActorFactory::CreateBrushForVolumeActor(Volume, Box);
			Volume->SetActorLabel(Op.Volume.TrimStartAndEnd());
			Volume->SetFolderPath(TEXT("PCG"));
			Volume->PCGComponent->SetGraph(Graph);
			Report.Notes.Add(FString::Printf(TEXT("%s placed: %g x %g m at (%.0f, %.0f), running %s."), *Volume->GetActorLabel(), Op.Size.X, Op.Size.Y, Op.Center.X, Op.Center.Y, *Graph->GetName()));
			OutGenerate = Volume;
			return true;
		}
		APCGVolume* Volume = FindVolume(World, Op.Volume);
		if (!Volume || !Volume->PCGComponent)
		{
			OutError = FString::Printf(TEXT("No PCG volume is labelled %s."), *Op.Volume);
			return false;
		}
		if (Kind == TEXT("volume.clear"))
		{
			const int32 Before = CountInstances(*Volume);
			Volume->PCGComponent->CleanupLocal(/*bRemoveComponents=*/true);
			Report.Notes.Add(FString::Printf(TEXT("%s cleared of %d instance(s)."), *Volume->GetActorLabel(), Before));
			return true;
		}
		OutGenerate = Volume;
		return true;
	}

	FHyperAIPCGScatterPlanReport Execute(const FHyperAIPCGScatterPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIPCGScatterPlanReport Report;
		Report.OperationId = OperationId;
		UWorld* World = EditorWorld();
		if (!World)
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = TEXT("No level is open in the editor.");
			return Report;
		}
		APCGVolume* Generate = nullptr;
		{
			const FScopedTransaction Transaction(LOCTEXT("ScatterPlan", "HyperAI: PCG Scatter"));
			for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
			{
				FString Error;
				if (!Apply(*World, Request.Operations[Index], Report, Generate, Error))
				{
					Report.Status = TEXT("operation_failed");
					Report.Diagnostic = FString::Printf(TEXT("Operation %d (%s): %s%s"), Index, *Request.Operations[Index].Kind, *Error,
						Report.Applied > 0 ? TEXT(" The operations before it stay applied; graphs are saved.") : TEXT(""));
					return Report;
				}
				++Report.Applied;
			}
		}
		// Saved before generating: a landscape kept whole reaches PCG in a World Partition level only once it is saved.
		if (Request.bSave)
		{
			if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages=*/true, /*bSaveContentPackages=*/false))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = Generate
					? TEXT("The level could not be saved, so nothing was generated; the plan's changes stay in memory.")
					: TEXT("The level could not be saved; the plan's changes stay in memory.");
				return Report;
			}
			Report.Saved.Add(World->GetOutermost()->GetName());
		}
		if (Generate)
		{
			FString Error;
			if (!StartGenerating(*Generate, OperationId, Report, Error))
			{
				Report.Status = TEXT("operation_failed");
				Report.Diagnostic = FString::Printf(TEXT("%d operation(s) applied, but generation did not start: %s"), Report.Applied, *Error);
				return Report;
			}
		}
		const bool bGenerating = Generate != nullptr;
		Report.bOk = true;
		Report.Status = bGenerating ? TEXT("generating") : TEXT("completed");
		Report.Diagnostic = bGenerating
			? FString::Printf(TEXT("%d operation(s) applied and generation started; call hyper_operation_wait with operation_id %s for the instances."), Report.Applied, *OperationId)
			: FString::Printf(TEXT("%d operation(s) applied."), Report.Applied);
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAIPCGScatterPlanReport& Done)
	{
		// A generation's own job reports when it finishes.
		if (Done.Status == TEXT("generating")) return;
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

FHyperAIPCGScatterInspectReport FHyperAIStudioPCGScatterContracts::Inspect(const FHyperAIPCGScatterInspectRequest& Request)
{
	using namespace HyperAIStudio::PCGScatterTools::Private;
	FHyperAIPCGScatterInspectReport Report;
	if (!Request.Graph.TrimStartAndEnd().IsEmpty())
	{
		const UPCGGraph* Graph = Load<UPCGGraph>(Request.Graph);
		if (!Graph)
		{
			Report.Diagnostic = FString::Printf(TEXT("%s is not a PCG graph."), *Request.Graph);
			return Report;
		}
		Report.Settings = DescribeGraph(*Graph);
		Report.bOk = true;
		Report.Diagnostic = FString::Printf(TEXT("%s: %s."), *Graph->GetName(), IsScatterGraph(*Graph) ? TEXT("a scatter graph") : TEXT("not a scatter graph this tool built"));
		return Report;
	}
	if (UWorld* World = EditorWorld())
	{
		for (TActorIterator<APCGVolume> It(World); It; ++It)
		{
			const UPCGComponent* Component = It->PCGComponent;
			const FBox Bounds = It->GetComponentsBoundingBox(/*bNonColliding=*/true);
			Report.Volumes.Add(FString::Printf(TEXT("%s: %s, %.0f x %.0f m, %d instance(s)%s"), *It->GetActorLabel(),
				Component && Component->GetGraph() ? *Component->GetGraph()->GetPathName() : TEXT("no graph"), Bounds.GetSize().X / 100.0, Bounds.GetSize().Y / 100.0,
				CountInstances(**It), Component && Component->IsGenerating() ? TEXT(", generating") : TEXT("")));
		}
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	Registry.GetAssetsByClass(UPCGGraph::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/false);
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.PackageName.ToString().StartsWith(TEXT("/Game/")) && Report.Graphs.Num() < 100) Report.Graphs.Add(Asset.GetObjectPathString());
	}
	Report.Graphs.Sort();
	Report.bOk = true;
	Report.Diagnostic = FString::Printf(TEXT("%d PCG volume(s) in the level, %d PCG graph(s) under /Game."), Report.Volumes.Num(), Report.Graphs.Num());
	return Report;
}

FHyperAIPCGScatterPlanReport FHyperAIStudioPCGScatterContracts::ApplyPlan(const FHyperAIPCGScatterPlanRequest& Request)
{
	using namespace HyperAIStudio::PCGScatterTools::Private;
	FHyperAIPCGScatterPlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("scatter-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	UWorld* World = EditorWorld();
	FString Error;
	if (!World)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("No level is open in the editor.");
		return Report;
	}
	if (!Check(*World, Request, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	if (Request.bSave && World->GetOutermost()->GetName().StartsWith(TEXT("/Temp/")))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("This level has never been saved, so there is nowhere to save the volume; save the level first or leave bSave off.");
		return Report;
	}
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) checked; nothing was changed."), Request.Operations.Num());
		return Report;
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = PlanToolName;
		Summary.VariantId = TEXT("pcg_scatter.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Request.Operations[0].Graph.IsEmpty() ? Request.Operations[0].Volume : Request.Operations[0].Graph;
		Summary.Effects.Add(FString::Printf(TEXT("PCG scatter: %d operation(s)%s"), Request.Operations.Num(), Request.bSave ? TEXT(", then the level saved") : TEXT("")));
		for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
		{
			const FHyperAIPCGScatterOp& Op = Request.Operations[Index];
			Summary.Effects.Add(FString::Printf(TEXT("| %s %s"), *Op.Kind, Op.Volume.IsEmpty() ? *Op.Graph : *Op.Volume));
			Summary.Touches.AddUnique(Op.Volume.IsEmpty() ? Op.Graph : Op.Volume);
		}
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				Record(OperationId, Execute(Request, OperationId));
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
	return Execute(Request, Report.OperationId);
}

FHyperAIPCGScatterInspectReport UHyperAIStudioPCGScatterToolset::hyper_pcg_scatter_inspect(const FHyperAIPCGScatterInspectRequest& Request)
{
	return FHyperAIStudioPCGScatterContracts::Inspect(Request);
}

FHyperAIPCGScatterPlanReport UHyperAIStudioPCGScatterToolset::hyper_pcg_scatter_apply_plan(const FHyperAIPCGScatterPlanRequest& Request)
{
	return FHyperAIStudioPCGScatterContracts::ApplyPlan(Request);
}

void FHyperAIStudioPCGScatterRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioPCGScatterRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioPCGScatterRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioPCGScatterToolset::StaticClass(),
			FHyperAIStudioPCGScatterContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioPCGScatter, Warning, TEXT("Could not unregister the PCG scatter toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioPCGScatterRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioPCGScatterContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioPCGScatter, Verbose, TEXT("The PCG scatter source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioPCGScatterToolset::StaticClass(),
		FHyperAIStudioPCGScatterContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioPCGScatter, Error, TEXT("PCG scatter cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
