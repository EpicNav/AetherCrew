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
#include "JsonObjectConverter.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "PCGCommon.h"
#include "Data/PCGBasePointData.h"
#include "Elements/PCGAttributeFilter.h"
#include "Elements/PCGAttributeNoise.h"
#include "Elements/PCGBoundsModifier.h"
#include "Elements/PCGCopyPoints.h"
#include "Elements/PCGCreatePointsGrid.h"
#include "Elements/PCGDataFromActor.h"
#include "Elements/PCGDensityFilter.h"
#include "Elements/PCGDifferenceElement.h"
#include "Elements/PCGExecuteBlueprint.h"
#include "Elements/PCGNormalToDensity.h"
#include "Elements/PCGProjectionElement.h"
#include "Elements/PCGSelfPruning.h"
#include "Elements/PCGSpatialNoise.h"
#include "Elements/PCGStaticMeshSpawner.h"
#include "Elements/PCGSurfaceSampler.h"
#include "Elements/PCGTransformPoints.h"
#include "Elements/PCGTypedGetter.h"
#include "Grid/PCGPartitionActor.h"
#include "Helpers/PCGHelpers.h"
#include "MeshSelectors/PCGMeshSelectorWeighted.h"
#include "Metadata/PCGMetadataTypesConstantStruct.h"
#include "PCGComponent.h"
#include "PCGManagedResource.h"
#include "PCGData.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGVolume.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "StructUtils/PropertyBag.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

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

	/** A landscape or one of its streaming proxies, by label. */
	ALandscapeProxy* FindLandscape(UWorld& World, const FString& Label)
	{
		for (TActorIterator<ALandscapeProxy> It(&World); It; ++It)
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

	/** How an ecosystem graph is told from others: the graph's category, which the content browser shows too. */
	const TCHAR* const EcosystemCategory = TEXT("HyperAI Ecosystem");

	bool IsEcosystemGraph(const UPCGGraph& Graph)
	{
		return Graph.Category.ToString() == EcosystemCategory;
	}

	/**
	 * A graph this tool built: the scatter chain's nodes and nothing else, one of each. Ecosystems and marketplace
	 * generators use the same kinds of node, many times over, and are not scatter graphs for graph.set to reshape.
	 */
	bool IsScatterGraph(const UPCGGraph& Graph)
	{
		if (IsEcosystemGraph(Graph) || Graph.GetNodes().Num() != static_cast<int32>(EStep::Count)) return false;
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
		// Samples lean with the ground: an upright instance takes its turn as a world rotation instead of adding it to that.
		TOptional<bool> Align;
		ParseFlag(Op.AlignToGround, Align);
		if (!Align.IsSet() && bNew) Align = false;
		if (Align.IsSet() && !SetProperty(*Transform, TEXT("bAbsoluteRotation"), Align.GetValue() ? TEXT("False") : TEXT("True"), OutError)) return false;
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
		Lines.Add(TEXT("Rotation up to ") + ReadProperty(*Transform, TEXT("RotationMax"))
			+ (ReadProperty(*Transform, TEXT("bAbsoluteRotation")) == TEXT("True") ? TEXT(", upright") : TEXT(", leaning with the ground")));
		Lines.Add(TEXT("Seed ") + ReadProperty(*Sampler, TEXT("Seed")));
		if (const FObjectPropertyBase* Parameters = CastField<FObjectPropertyBase>(Spawner->GetClass()->FindPropertyByName(TEXT("MeshSelectorParameters"))))
		{
			if (const UObject* Selector = Parameters->GetObjectPropertyValue_InContainer(Spawner)) Lines.Add(TEXT("Meshes ") + ReadProperty(*Selector, TEXT("MeshEntries")).Left(1500));
		}
		return Lines;
	}

	// --- Ecosystems -----------------------------------------------------------------------------------------------

	/**
	 * The noise brightness that leaves Coverage of the ground in stands. PCG's Perlin2D (4 iterations) is centered on 0.5
	 * with a spread of about 0.125; a point stays where brightness + noise >= 0.5, at any contrast. Measured on UE 5.8's
	 * noise over a 2 km square; the test checks the share.
	 */
	double BrightnessForCoverage(const double Coverage)
	{
		static const TPair<double, double> Table[] = {{0.05, -0.205}, {0.1, -0.166}, {0.2, -0.114}, {0.3, -0.074}, {0.4, -0.037},
			{0.5, -0.002}, {0.6, 0.032}, {0.7, 0.066}, {0.8, 0.107}, {0.9, 0.160}, {0.95, 0.2}};
		const double C = FMath::Clamp(Coverage, Table[0].Key, Table[UE_ARRAY_COUNT(Table) - 1].Key);
		for (int32 Index = 1; Index < UE_ARRAY_COUNT(Table); ++Index)
		{
			if (C <= Table[Index].Key)
			{
				const double Alpha = (C - Table[Index - 1].Key) / (Table[Index].Key - Table[Index - 1].Key);
				return FMath::Lerp(Table[Index - 1].Value, Table[Index].Value, Alpha);
			}
		}
		return Table[UE_ARRAY_COUNT(Table) - 1].Value;
	}

	/** A species' values with its defaults filled in. */
	struct FSpeciesPlan
	{
		const FHyperAIPCGSpecies* Species = nullptr;
		float Footprint = 1.f;
		float SlopeMax = 0.f;
		FVector2D Scale = FVector2D(0.8, 1.2);
		int32 Cluster = 0;
		float ClusterRadius = 0.f;
	};

	FSpeciesPlan PlanSpecies(const FHyperAIPCGBiome& Biome, const FHyperAIPCGSpecies& Species)
	{
		FSpeciesPlan Plan;
		Plan.Species = &Species;
		Plan.Footprint = Species.Footprint > 0.f ? Species.Footprint : 1.f;
		Plan.SlopeMax = Species.SlopeMax > 0.f ? Species.SlopeMax : Biome.SlopeMax;
		Plan.Scale = Species.Scale.X > 0.0 && Species.Scale.Y > 0.0 ? Species.Scale : FVector2D(0.8, 1.2);
		Plan.Cluster = Species.Cluster > 1 ? Species.Cluster : 0;
		Plan.ClusterRadius = Plan.Cluster > 0 ? (Species.ClusterRadius > 0.f ? Species.ClusterRadius : Plan.Footprint * 2.f) : 0.f;
		return Plan;
	}

	/** Builds an ecosystem graph: one shared landscape read, then one chain per species, left to right. */
	class FEcosystemBuilder
	{
	public:
		FEcosystemBuilder(UPCGGraph& InGraph, const FHyperAIPCGScatterOp& InOp) : Graph(InGraph), Op(InOp) {}

		bool Build(FString& OutError)
		{
			if (!Add<UPCGGetLandscapeSettings>(-600, 0, Landscape)) return Fail(TEXT("This engine's PCG has no Get Landscape Data node."), OutError);
			Landscape->NodeTitle = FName(TEXT("Landscape"));
			// Actors kept clear of: one point per tagged actor, its bounds widened by the margin.
			for (int32 Index = 0; Index < Op.AvoidTags.Num(); ++Index)
			{
				UPCGNode* Get = nullptr;
				UPCGDataFromActorSettings* Actors = Add<UPCGDataFromActorSettings>(-600, 300 + Index * 200, &Get);
				UPCGNode* Widen = nullptr;
				UPCGBoundsModifierSettings* Bounds = Add<UPCGBoundsModifierSettings>(-300, 300 + Index * 200, &Widen);
				if (!Actors || !Bounds) return Fail(TEXT("This engine's PCG lacks the actor nodes avoiding tags needs."), OutError);
				Actors->ActorSelector.ActorFilter = EPCGActorFilter::AllWorldActors;
				Actors->ActorSelector.ActorSelection = EPCGActorSelection::ByTag;
				Actors->ActorSelector.ActorSelectionTag = FName(*Op.AvoidTags[Index].TrimStartAndEnd());
				Actors->ActorSelector.bSelectMultiple = true;
				Actors->Mode = EPCGGetDataFromActorMode::GetSinglePoint;
				const double Margin = FMath::Max(Op.AvoidMargin, 0.f) * 100.0;
				Bounds->Mode = EPCGBoundsModifierMode::Translate;
				Bounds->BoundsMin = FVector(-Margin, -Margin, -Margin);
				Bounds->BoundsMax = FVector(Margin, Margin, Margin);
				Touched(*Actors);
				Touched(*Bounds);
				Get->NodeTitle = FName(*FString::Printf(TEXT("Avoid tag %s"), *Op.AvoidTags[Index]));
				if (!Link(Get, PCGPinConstants::DefaultOutputLabel, Widen, PCGPinConstants::DefaultInputLabel, OutError)) return false;
				Avoided.Add(Widen);
			}
			int32 Row = 0;
			for (const FHyperAIPCGBiome& Biome : Op.Biomes)
			{
				TMap<FString, UPCGNode*> Footprints;
				TArray<FString> Order;
				for (const FHyperAIPCGSpecies& Species : Biome.Species)
				{
					UPCGNode* Footprint = nullptr;
					if (!BuildSpecies(Biome, Species, Row++, Footprints, Order, Footprint, OutError)) return false;
					Footprints.Add(Species.Name.TrimStartAndEnd().ToLower(), Footprint);
					Order.Add(Species.Name.TrimStartAndEnd().ToLower());
				}
			}
			return true;
		}

	private:
		UPCGGraph& Graph;
		const FHyperAIPCGScatterOp& Op;
		UPCGNode* Landscape = nullptr;
		TArray<UPCGNode*> Avoided;

		static bool Fail(const FString& Message, FString& OutError)
		{
			OutError = Message;
			return false;
		}

		/** Settings changed in code are announced as the details panel would, so the graph rebuilds its caches. */
		static void Touched(UPCGSettings& Settings)
		{
			static_cast<UObject&>(Settings).PostEditChange();
		}

		template <typename T>
		T* Add(const int32 X, const int32 Y, UPCGNode** OutNode = nullptr)
		{
			UPCGSettings* Settings = nullptr;
			UPCGNode* Node = Graph.AddNodeOfType(T::StaticClass(), Settings);
			if (!Node) return nullptr;
			Node->SetNodePosition(X, Y);
			if (OutNode) *OutNode = Node;
			return Cast<T>(Settings);
		}

		template <typename T>
		T* Add(const int32 X, const int32 Y, UPCGNode*& OutNode)
		{
			return Add<T>(X, Y, &OutNode);
		}

		bool Link(UPCGNode* From, const FName FromPin, UPCGNode* To, const FName ToPin, FString& OutError)
		{
			if (From && To && Graph.AddEdge(From, FromPin, To, ToPin)) return true;
			OutError = FString::Printf(TEXT("The ecosystem's %s -> %s link would not connect (%s -> %s)."), From ? *From->GetName() : TEXT("?"),
				To ? *To->GetName() : TEXT("?"), *FromPin.ToString(), *ToPin.ToString());
			return false;
		}

		/** A filter keeping points whose Attribute compares to Value; In Filter out. */
		UPCGNode* AttributeFilter(const int32 X, const int32 Y, const FString& Attribute, const EPCGAttributeFilterOperator Operator, const float Value)
		{
			UPCGNode* Node = nullptr;
			UPCGAttributeFilteringSettings* Filter = Add<UPCGAttributeFilteringSettings>(X, Y, Node);
			if (!Filter) return nullptr;
			Filter->Operator = Operator;
			Filter->TargetAttribute.SetAttributeName(FName(*Attribute));
			Filter->bUseConstantThreshold = true;
			Filter->AttributeTypes.Type = EPCGMetadataTypes::Float;
			Filter->AttributeTypes.FloatValue = Value;
			Touched(*Filter);
			return Node;
		}

		bool BuildSpecies(const FHyperAIPCGBiome& Biome, const FHyperAIPCGSpecies& Species, const int32 Row, const TMap<FString, UPCGNode*>& Footprints,
			const TArray<FString>& Earlier, UPCGNode*& OutFootprint, FString& OutError)
		{
			const FSpeciesPlan Plan = PlanSpecies(Biome, Species);
			const int32 Y = Row * 460;
			int32 X = 0;
			auto Next = [&X]() { return X += 280; };
			const uint32 Seed = HashCombine(HashCombine(GetTypeHash(Biome.Name.ToLower()), GetTypeHash(Species.Name.ToLower())), static_cast<uint32>(Op.Seed));
			const int32 NodeSeed = static_cast<int32>(Seed & 0x3fffffff);
			const FName In = PCGPinConstants::DefaultInputLabel;
			const FName Out = PCGPinConstants::DefaultOutputLabel;
			const FName Kept = PCGPinConstants::DefaultInFilterLabel;

			// A clump is Side x Side cells, thinned to Cluster members; members stand a cell apart, give or take a fifth of one.
			const bool bClumps = Plan.Cluster > 0;
			const int32 Side = bClumps ? FMath::Max(2, FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Plan.Cluster)))) : 1;
			const double Cell = FMath::Max(2.0 * Plan.ClusterRadius * 100.0 / Side, 1.25 * Plan.Footprint * 100.0 * Plan.Scale.Y);
			const double Jitter = Cell * 0.2;
			// Sample the ground inside the volume, its up axis the ground's normal. The sampler gives each point a cell of its
			// own at least as wide as its box: the footprint, or for clumps the whole clump, so clumps never overlap.
			UPCGNode* Sample = nullptr;
			UPCGSurfaceSamplerSettings* Sampler = Add<UPCGSurfaceSamplerSettings>(X, Y, Sample);
			if (!Sampler) return Fail(TEXT("This engine's PCG has no Surface Sampler."), OutError);
			Sampler->PointsPerSquaredMeter = Species.Density;
			const double Half = Plan.Footprint * 50.0;
			const FVector Extents(Half, Half, FMath::Max(Half, 50.0));
			const double ClumpHalf = Side * Cell * 0.5 + Jitter;
			Sampler->PointExtents = bClumps ? FVector(ClumpHalf, ClumpHalf, Extents.Z) : Extents;
			Sampler->Seed = NodeSeed;
			Touched(*Sampler);
			Sample->NodeTitle = FName(*FString::Printf(TEXT("%s: %s"), *Biome.Name, *Species.Name));
			if (!Link(Graph.GetInputNode(), In, Sample, TEXT("Bounding Shape"), OutError) || !Link(Landscape, Out, Sample, TEXT("Surface"), OutError)) return false;
			UPCGNode* Last = Sample;
			FName LastPin = Out;
			auto Chain = [&](UPCGNode* Node, const FName InPin, const FName OutPin) -> bool
			{
				if (!Node) return Fail(TEXT("A node of the ecosystem could not be made in this engine."), OutError);
				if (!Link(Last, LastPin, Node, InPin, OutError)) return false;
				Last = Node;
				LastPin = OutPin;
				return true;
			};
			// Clumps come before the masks, so each member is kept or dropped where it lands: a small grid stamped on each
			// point, thinned to Cluster members, moved off the grid and dropped back onto the ground, which also reads the
			// ground's normal and paint layers there.
			if (bClumps)
			{
				UPCGNode* Grid = nullptr;
				UPCGCreatePointsGridSettings* Stamp = Add<UPCGCreatePointsGridSettings>(X + 140, Y + 200, Grid);
				UPCGNode* Copy = nullptr;
				UPCGCopyPointsSettings* Copies = Add<UPCGCopyPointsSettings>(Next(), Y, Copy);
				if (!Stamp || !Copies) return Fail(TEXT("This engine's PCG lacks the clump nodes."), OutError);
				// Side x Side cells about the origin, which Copy Points centers on each point; the extra centimeter keeps the
				// cell count from rounding down.
				Stamp->GridExtents = FVector(Side * Cell * 0.5 + 1.0, Side * Cell * 0.5 + 1.0, 0.0);
				Stamp->CellSize = FVector(Cell, Cell, 100.0);
				Stamp->CoordinateSpace = EPCGCoordinateSpace::World;
				Stamp->bCullPointsOutsideVolume = false;
				Touched(*Stamp);
				Touched(*Copies);
				if (!Link(Grid, Out, Copy, TEXT("Source"), OutError) || !Chain(Copy, TEXT("Target"), Out)) return false;
				const int32 Members = Side * Side;
				if (Plan.Cluster < Members)
				{
					// A random density per member, kept above 1 - Cluster / Members: Cluster members per clump on average.
					UPCGNode* Dice = nullptr;
					UPCGAttributeNoiseSettings* Random = Add<UPCGAttributeNoiseSettings>(Next(), Y, Dice);
					UPCGNode* Thin = nullptr;
					UPCGDensityFilterSettings* Keep = Add<UPCGDensityFilterSettings>(Next(), Y, Thin);
					if (!Random || !Keep) return Fail(TEXT("This engine's PCG lacks the clump nodes."), OutError);
					Random->Mode = EPCGAttributeNoiseMode::Set;
					Random->NoiseMin = 0.f;
					Random->NoiseMax = 1.f;
					Random->Seed = NodeSeed ^ 0x51ed;
					Keep->LowerBound = 1.f - static_cast<float>(Plan.Cluster) / Members;
					Keep->UpperBound = 1.f;
					Touched(*Random);
					Touched(*Keep);
					if (!Chain(Dice, In, Out) || !Chain(Thin, In, Out)) return false;
				}
				// Members wander off the grid, so a clump does not read as one.
				UPCGNode* Scatter = nullptr;
				UPCGTransformPointsSettings* Spread = Add<UPCGTransformPointsSettings>(Next(), Y, Scatter);
				UPCGNode* Project = nullptr;
				UPCGProjectionSettings* Projection = Add<UPCGProjectionSettings>(Next(), Y, Project);
				UPCGNode* Boxed = nullptr;
				UPCGBoundsModifierSettings* Box = Add<UPCGBoundsModifierSettings>(Next(), Y, Boxed);
				if (!Spread || !Projection || !Box) return Fail(TEXT("This engine's PCG lacks the clump nodes."), OutError);
				Spread->OffsetMin = FVector(-Jitter, -Jitter, 0.0);
				Spread->OffsetMax = FVector(Jitter, Jitter, 0.0);
				Spread->bAbsoluteOffset = true;
				Spread->Seed = NodeSeed ^ 0x6a09;
				Projection->ProjectionParams.bProjectPositions = true;
				Projection->ProjectionParams.bProjectRotations = true;
				Projection->ProjectionParams.bProjectScales = false;
				// Each member's box is the footprint again, not the grid's cell.
				Box->Mode = EPCGBoundsModifierMode::Set;
				Box->BoundsMin = -Extents;
				Box->BoundsMax = Extents;
				Touched(*Spread);
				Touched(*Projection);
				Touched(*Box);
				Project->NodeTitle = FName(TEXT("Clump onto the ground"));
				if (!Chain(Scatter, In, Out) || !Chain(Project, In, Out) || !Link(Landscape, Out, Project, PCGProjectionConstants::ProjectionTargetLabel, OutError)
					|| !Chain(Boxed, In, Out))
				{
					return false;
				}
			}
			// The biome's paint layer, and the layers nothing grows on.
			if (!Biome.Layer.TrimStartAndEnd().IsEmpty()
				&& !Chain(AttributeFilter(Next(), Y, Biome.Layer.TrimStartAndEnd(), EPCGAttributeFilterOperator::GreaterOrEqual, Biome.LayerMin), In, Kept))
			{
				return false;
			}
			for (const FString& Avoid : Op.AvoidLayers)
			{
				if (!Chain(AttributeFilter(Next(), Y, Avoid.TrimStartAndEnd(), EPCGAttributeFilterOperator::Lesser, 0.5f), In, Kept)) return false;
			}
			if (Biome.Height.Y > Biome.Height.X)
			{
				UPCGNode* Node = nullptr;
				UPCGAttributeFilteringRangeSettings* Range = Add<UPCGAttributeFilteringRangeSettings>(Next(), Y, Node);
				if (!Range) return Fail(TEXT("This engine's PCG has no range filter."), OutError);
				Range->TargetAttribute.Update(TEXT("$Position.Z"));
				Range->MinThreshold.bUseConstantThreshold = true;
				Range->MinThreshold.AttributeTypes.Type = EPCGMetadataTypes::Double;
				Range->MinThreshold.AttributeTypes.DoubleValue = Biome.Height.X * 100.0;
				Range->MaxThreshold.bUseConstantThreshold = true;
				Range->MaxThreshold.AttributeTypes.Type = EPCGMetadataTypes::Double;
				Range->MaxThreshold.AttributeTypes.DoubleValue = Biome.Height.Y * 100.0;
				Touched(*Range);
				if (!Chain(Node, In, Kept)) return false;
			}
			// Off steep ground: the normal's height is the density, and a slope of S degrees leaves cos(S).
			if (Plan.SlopeMax > 0.f)
			{
				UPCGNode* Normal = nullptr;
				UPCGNormalToDensitySettings* ToDensity = Add<UPCGNormalToDensitySettings>(Next(), Y, Normal);
				UPCGNode* Steep = nullptr;
				UPCGDensityFilterSettings* Filter = Add<UPCGDensityFilterSettings>(Next(), Y, Steep);
				if (!ToDensity || !Filter) return Fail(TEXT("This engine's PCG lacks the slope nodes."), OutError);
				ToDensity->Normal = FVector::UpVector;
				Filter->LowerBound = static_cast<float>(FMath::Cos(FMath::DegreesToRadians(static_cast<double>(Plan.SlopeMax))));
				Filter->UpperBound = 1.f;
				Touched(*ToDensity);
				Touched(*Filter);
				if (!Chain(Normal, In, Out) || !Chain(Steep, In, Out)) return false;
			}
			// Stands and clearings: noise as density, kept where it reaches 0.5 on Coverage of the ground.
			if (Species.Patch > 0.f && Species.Coverage < 0.999f)
			{
				UPCGNode* Noise = nullptr;
				UPCGSpatialNoiseSettings* Spatial = Add<UPCGSpatialNoiseSettings>(Next(), Y, Noise);
				UPCGNode* Stands = nullptr;
				UPCGDensityFilterSettings* Filter = Add<UPCGDensityFilterSettings>(Next(), Y, Stands);
				if (!Spatial || !Filter) return Fail(TEXT("This engine's PCG lacks the noise nodes."), OutError);
				Spatial->Mode = PCGSpatialNoiseMode::Perlin2D;
				Spatial->Iterations = 4;
				// Noise coordinates are centimeters x scale x 0.0001, so one feature spans 100 / scale meters.
				const double NoiseScale = 100.0 / FMath::Max(Species.Patch, 1.f);
				Spatial->Transform = FTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector(NoiseScale));
				Spatial->Brightness = static_cast<float>(BrightnessForCoverage(Species.Coverage));
				Spatial->Contrast = 3.f;
				Spatial->Seed = NodeSeed ^ 0x2545;
				Filter->LowerBound = 0.5f;
				Filter->UpperBound = 1.f;
				Touched(*Spatial);
				Touched(*Filter);
				Noise->NodeTitle = FName(*FString::Printf(TEXT("Stands %.0f m, %.0f%%"), Species.Patch, Species.Coverage * 100.f));
				if (!Chain(Noise, In, Out) || !Chain(Stands, In, Out)) return false;
			}
			// Sunk, leaned, turned and scaled. Samples lean with the ground: rocks and debris add their turn to it, while
			// trees and bushes take a world rotation and stand upright.
			{
				UPCGNode* Place = nullptr;
				UPCGTransformPointsSettings* Transform = Add<UPCGTransformPointsSettings>(Next(), Y, Place);
				if (!Transform) return Fail(TEXT("This engine's PCG has no Transform Points node."), OutError);
				Transform->OffsetMin = FVector(0.0, 0.0, -FMath::Max(Species.Sink, 0.f));
				Transform->OffsetMax = FVector(0.0, 0.0, -FMath::Max(Species.Sink, 0.f) * 0.5);
				Transform->bAbsoluteOffset = true;
				Transform->RotationMin = FRotator(-Species.Tilt, 0.0, -Species.Tilt);
				Transform->RotationMax = FRotator(Species.Tilt, 360.0, Species.Tilt);
				Transform->bAbsoluteRotation = !Species.bAlignToGround;
				Transform->bUniformScale = true;
				Transform->ScaleMin = FVector(Plan.Scale.X);
				Transform->ScaleMax = FVector(Plan.Scale.Y);
				Transform->Seed = NodeSeed ^ 0x3c6e;
				Touched(*Transform);
				if (!Chain(Place, In, Out)) return false;
			}
			// Spaced: boxes of the footprint, grown with the instance's scale, never overlap. Clump members stand close by
			// design, and the sampler already kept clumps apart.
			if (!bClumps)
			{
				UPCGNode* Prune = nullptr;
				UPCGSelfPruningSettings* Pruning = Add<UPCGSelfPruningSettings>(Next(), Y, Prune);
				if (!Pruning) return Fail(TEXT("This engine's PCG has no Self Pruning node."), OutError);
				Pruning->Seed = NodeSeed ^ 0x7f4a;
				Touched(*Pruning);
				if (!Chain(Prune, In, Out)) return false;
			}
			// Out of the room earlier species take, and clear of tagged actors.
			TArray<UPCGNode*> Exclusions = Avoided;
			const bool bNone = Species.KeepOutOf.ContainsByPredicate([](const FString& Name) { return Name.TrimStartAndEnd().Equals(TEXT("none"), ESearchCase::IgnoreCase); });
			if (!bNone)
			{
				const TArray<FString> Names = Species.KeepOutOf.IsEmpty() ? Earlier : Species.KeepOutOf;
				for (const FString& Name : Names)
				{
					if (UPCGNode* const* Footprint = Footprints.Find(Name.TrimStartAndEnd().ToLower())) Exclusions.Add(*Footprint);
				}
			}
			if (!Exclusions.IsEmpty())
			{
				UPCGNode* Minus = nullptr;
				UPCGDifferenceSettings* Difference = Add<UPCGDifferenceSettings>(Next(), Y, Minus);
				if (!Difference) return Fail(TEXT("This engine's PCG has no Difference node."), OutError);
				Difference->DensityFunction = EPCGDifferenceDensityFunction::Binary;
				Touched(*Difference);
				if (!Chain(Minus, PCGDifferenceConstants::SourceLabel, Out)) return false;
				for (UPCGNode* Exclusion : Exclusions)
				{
					if (!Link(Exclusion, Out, Minus, PCGDifferenceConstants::DifferencesLabel, OutError)) return false;
				}
			}
			// The meshes, with their cull distances.
			UPCGNode* Spawn = nullptr;
			UPCGStaticMeshSpawnerSettings* Spawner = Add<UPCGStaticMeshSpawnerSettings>(Next(), Y, Spawn);
			if (!Spawner) return Fail(TEXT("This engine's PCG has no Static Mesh Spawner."), OutError);
			// The points leave with their footprint, not the mesh's bounds: the room later species keep out of is the footprint's.
			Spawner->bApplyMeshBoundsToPoints = false;
			Spawner->SetMeshSelectorType(UPCGMeshSelectorWeighted::StaticClass());
			UPCGMeshSelectorWeighted* Weighted = Cast<UPCGMeshSelectorWeighted>(Spawner->MeshSelectorParameters);
			if (!Weighted) return Fail(TEXT("The mesh spawner would not pick by weight in this engine."), OutError);
			Weighted->MeshEntries.Reset();
			for (const FHyperAIPCGScatterMesh& Mesh : Species.Meshes)
			{
				const UStaticMesh* Asset = Load<UStaticMesh>(Mesh.Mesh);
				FPCGMeshSelectorWeightedEntry Entry(TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(Asset)), FMath::Max(Mesh.Weight, 1));
				const float Cull = Mesh.CullDistance > 0.f ? Mesh.CullDistance : Species.CullDistance;
				if (Cull > 0.f)
				{
					Entry.Descriptor.InstanceEndCullDistance = FMath::RoundToInt(Cull * 100.f);
					Entry.Descriptor.InstanceStartCullDistance = FMath::RoundToInt(Cull * 80.f);
				}
				Weighted->MeshEntries.Add(MoveTemp(Entry));
			}
			Touched(*Spawner);
			Spawn->NodeTitle = FName(*FString::Printf(TEXT("%s: %s"), *Biome.Name, *Species.Name));
			if (!Chain(Spawn, In, Out) || !Link(Spawn, Out, Graph.GetOutputNode(), Out, OutError)) return false;
			// The room this species takes, for the species after it: its boxes widened by Clearance.
			UPCGNode* Room = nullptr;
			UPCGBoundsModifierSettings* Bounds = Add<UPCGBoundsModifierSettings>(X, Y + 220, Room);
			if (!Bounds) return Fail(TEXT("This engine's PCG has no Bounds Modifier."), OutError);
			const double Clearance = FMath::Max(Species.Clearance, 0.f);
			Bounds->Mode = EPCGBoundsModifierMode::Scale;
			Bounds->BoundsMin = FVector(Clearance, Clearance, 1.0);
			Bounds->BoundsMax = FVector(Clearance, Clearance, 1.0);
			Touched(*Bounds);
			Room->NodeTitle = FName(*FString::Printf(TEXT("Room of %s"), *Species.Name));
			if (!Link(Spawn, Out, Room, In, OutError)) return false;
			OutFootprint = Room;
			return true;
		}
	};

	/** A short account of an ecosystem, kept as the graph's description. */
	FString DescribeEcosystem(const FHyperAIPCGScatterOp& Op)
	{
		TArray<FString> Biomes;
		for (const FHyperAIPCGBiome& Biome : Op.Biomes)
		{
			TArray<FString> Species;
			for (const FHyperAIPCGSpecies& Each : Biome.Species)
			{
				Species.Add(FString::Printf(TEXT("%s %g/m2"), *Each.Name, Each.Density));
			}
			Biomes.Add(FString::Printf(TEXT("%s%s: %s"), *Biome.Name, Biome.Layer.IsEmpty() ? TEXT("") : *(TEXT(" on ") + Biome.Layer), *FString::Join(Species, TEXT(", "))));
		}
		return FString::Printf(TEXT("HyperAI ecosystem. %s.%s%s"), *FString::Join(Biomes, TEXT("; ")),
			Op.AvoidLayers.IsEmpty() ? TEXT("") : *(TEXT(" Avoids layers ") + FString::Join(Op.AvoidLayers, TEXT(", ")) + TEXT(".")),
			Op.AvoidTags.IsEmpty() ? TEXT("") : *(TEXT(" Keeps clear of tags ") + FString::Join(Op.AvoidTags, TEXT(", ")) + TEXT(".")));
	}

	/** Where an ecosystem graph keeps the op it was built from: its package's metadata, saved with the asset. */
	const TCHAR* const EcosystemSpecKey = TEXT("HyperAI.Ecosystem");

	/** The op's ecosystem fields as ecosystem.set takes them, named as the tool's arguments are. */
	FString EcosystemSpec(const UPCGGraph& Graph, const FHyperAIPCGScatterOp& Op)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		auto Key = [](const TCHAR* Name) { return FJsonObjectConverter::StandardizeCase(Name); };
		Json->SetStringField(Key(TEXT("Kind")), TEXT("ecosystem.set"));
		Json->SetStringField(Key(TEXT("Graph")), Graph.GetPathName());
		TArray<TSharedPtr<FJsonValue>> Biomes;
		for (const FHyperAIPCGBiome& Biome : Op.Biomes)
		{
			const TSharedRef<FJsonObject> Each = MakeShared<FJsonObject>();
			if (FJsonObjectConverter::UStructToJsonObject(FHyperAIPCGBiome::StaticStruct(), &Biome, Each)) Biomes.Add(MakeShared<FJsonValueObject>(Each));
		}
		Json->SetArrayField(Key(TEXT("Biomes")), Biomes);
		auto Strings = [](const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FString& Value : Values) Out.Add(MakeShared<FJsonValueString>(Value));
			return Out;
		};
		Json->SetArrayField(Key(TEXT("AvoidLayers")), Strings(Op.AvoidLayers));
		Json->SetArrayField(Key(TEXT("AvoidTags")), Strings(Op.AvoidTags));
		Json->SetNumberField(Key(TEXT("AvoidMargin")), Op.AvoidMargin);
		Json->SetNumberField(Key(TEXT("Seed")), Op.Seed);
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Json, Writer);
		return Out;
	}

	/** An ecosystem graph built from scratch, or rebuilt: every node but the graph's input and output goes first. */
	bool BuildEcosystem(UPCGGraph& Graph, const FHyperAIPCGScatterOp& Op, FString& OutError)
	{
		TArray<UPCGNode*> Old;
		for (UPCGNode* Node : Graph.GetNodes())
		{
			if (Node && Node != Graph.GetInputNode() && Node != Graph.GetOutputNode()) Old.Add(Node);
		}
		if (!Old.IsEmpty()) Graph.RemoveNodes(Old);
		Graph.Category = FText::FromString(EcosystemCategory);
		Graph.Description = FText::FromString(DescribeEcosystem(Op));
		FEcosystemBuilder Builder(Graph, Op);
		if (!Builder.Build(OutError)) return false;
		Graph.GetPackage()->GetMetaData().SetValue(&Graph, EcosystemSpecKey, *EcosystemSpec(Graph, Op));
		return true;
	}

	/** An ecosystem op's biomes and species checked before anything changes. */
	bool CheckEcosystem(const FHyperAIPCGScatterOp& Op, FString& OutError)
	{
		auto Fail = [&OutError](const FString& Message)
		{
			OutError = Message;
			return false;
		};
		if (Op.Biomes.IsEmpty() || Op.Biomes.Num() > C::MaxBiomes) return Fail(FString::Printf(TEXT("An ecosystem has 1 to %d biomes."), C::MaxBiomes));
		if (Op.AvoidMargin < 0.f || Op.AvoidMargin > 100.f) return Fail(TEXT("AvoidMargin is 0 to 100 meters."));
		int32 Total = 0;
		TSet<FString> BiomeNames;
		for (const FHyperAIPCGBiome& Biome : Op.Biomes)
		{
			const FString BiomeName = Biome.Name.TrimStartAndEnd();
			if (BiomeName.IsEmpty() || BiomeNames.Contains(BiomeName.ToLower())) return Fail(TEXT("Name each biome, each name once."));
			BiomeNames.Add(BiomeName.ToLower());
			if (Biome.Species.IsEmpty() || Biome.Species.Num() > C::MaxSpeciesPerBiome)
			{
				return Fail(FString::Printf(TEXT("Biome %s has 1 to %d species."), *BiomeName, C::MaxSpeciesPerBiome));
			}
			if (Biome.LayerMin < 0.f || Biome.LayerMin > 1.f) return Fail(FString::Printf(TEXT("Biome %s: LayerMin is 0 to 1."), *BiomeName));
			if ((Biome.Height.X != 0.0 || Biome.Height.Y != 0.0) && Biome.Height.Y <= Biome.Height.X)
			{
				return Fail(FString::Printf(TEXT("Biome %s: Height runs from a Min (X) to a higher Max (Y), in meters."), *BiomeName));
			}
			if (Biome.SlopeMax < 0.f || Biome.SlopeMax > 90.f) return Fail(FString::Printf(TEXT("Biome %s: SlopeMax is 0 to 90 degrees."), *BiomeName));
			TSet<FString> Seen;
			for (const FHyperAIPCGSpecies& Species : Biome.Species)
			{
				const FString Name = Species.Name.TrimStartAndEnd();
				auto Bad = [&](const FString& Message) { return Fail(FString::Printf(TEXT("%s, species %s: %s"), *BiomeName, Name.IsEmpty() ? TEXT("(unnamed)") : *Name, *Message)); };
				if (Name.IsEmpty() || Name.Equals(TEXT("none"), ESearchCase::IgnoreCase) || Seen.Contains(Name.ToLower())) return Bad(TEXT("name each species, each name once in its biome."));
				if (Species.Meshes.IsEmpty()) return Bad(TEXT("give its Meshes."));
				for (const FHyperAIPCGScatterMesh& Mesh : Species.Meshes)
				{
					if (!Load<UStaticMesh>(Mesh.Mesh)) return Bad(FString::Printf(TEXT("%s is not a static mesh."), *Mesh.Mesh));
					if (Mesh.CullDistance < 0.f || Mesh.CullDistance > 100000.f) return Bad(TEXT("a mesh's CullDistance is 0 to 100000 meters."));
				}
				if (Species.Density <= 0.f || Species.Density > C::MaxDensity) return Bad(FString::Printf(TEXT("Density is above 0 and at most %.0f points per square meter."), C::MaxDensity));
				if (Species.Footprint < 0.f || Species.Footprint > 100.f) return Bad(TEXT("Footprint is 0 (1 m) to 100 meters."));
				if (Species.SlopeMax < 0.f || Species.SlopeMax > 90.f) return Bad(TEXT("SlopeMax is 0 to 90 degrees."));
				if (Species.Patch < 0.f || (Species.Patch > 0.f && Species.Patch < 2.f) || Species.Patch > 5000.f) return Bad(TEXT("Patch is 0 (none) or 2 to 5000 meters."));
				if (Species.Coverage < 0.05f || Species.Coverage > 1.f) return Bad(TEXT("Coverage is 0.05 to 1."));
				if (Species.Cluster < 0 || Species.Cluster > 36) return Bad(TEXT("Cluster is 0 to 36 instances."));
				if (Species.ClusterRadius < 0.f || Species.ClusterRadius > 100.f) return Bad(TEXT("ClusterRadius is 0 to 100 meters."));
				if ((Species.Scale.X != 0.0 || Species.Scale.Y != 0.0) && !(Species.Scale.X > 0.0 && Species.Scale.Y >= Species.Scale.X)) return Bad(TEXT("Scale runs from a Min (X) above 0 to a Max (Y) at least as big."));
				if (Species.Sink < 0.f || Species.Sink > 1000.f) return Bad(TEXT("Sink is 0 to 1000 cm."));
				if (Species.Tilt < 0.f || Species.Tilt > 45.f) return Bad(TEXT("Tilt is 0 to 45 degrees."));
				if (Species.CullDistance < 0.f || Species.CullDistance > 100000.f) return Bad(TEXT("CullDistance is 0 to 100000 meters."));
				if (Species.Clearance < 0.f || Species.Clearance > 10.f) return Bad(TEXT("Clearance is 0 to 10."));
				for (const FString& Other : Species.KeepOutOf)
				{
					const FString Key = Other.TrimStartAndEnd().ToLower();
					if (Key != TEXT("none") && !Seen.Contains(Key)) return Bad(FString::Printf(TEXT("KeepOutOf names %s, which is not an earlier species of the biome."), *Other));
				}
				Seen.Add(Name.ToLower());
				++Total;
			}
		}
		if (Total > C::MaxSpecies) return Fail(FString::Printf(TEXT("An ecosystem has at most %d species in all."), C::MaxSpecies));
		for (const FString& Layer : Op.AvoidLayers)
		{
			if (Layer.TrimStartAndEnd().IsEmpty()) return Fail(TEXT("AvoidLayers names paint layers."));
		}
		for (const FString& Tag : Op.AvoidTags)
		{
			if (Tag.TrimStartAndEnd().IsEmpty()) return Fail(TEXT("AvoidTags names actor tags."));
		}
		return true;
	}

	// --- Reading any graph ----------------------------------------------------------------------------------------

	/** Lines and characters an inspect returns at most, so a 1000-node generator still answers in one call. */
	constexpr int32 MaxReadLines = 400;
	constexpr int32 MaxReadChars = 120000;
	constexpr int32 MaxRecipeDepth = 18;

	/** Settings every node has, or bookkeeping, that say nothing about what the node does to the points. */
	bool IsNoisyProperty(const FName Name)
	{
		static const TSet<FName> Noisy = {TEXT("CachedOverridableParams"), TEXT("DebugSettings"), TEXT("DeterminismSettings"),
			TEXT("bEnabled"), TEXT("bDisplayDebuggingProperties"), TEXT("bExposeToLibrary"), TEXT("Category"), TEXT("Description"),
			TEXT("ExecutionDependency"), TEXT("MeshSelectorParameters"), TEXT("BlueprintElementInstance"), TEXT("BlueprintElementType"),
			TEXT("bUseLegacyGridCreationMethod"), TEXT("TargetActor"), TEXT("bExecuteOnGPU"), TEXT("bDumpCookedHLSL"),
			TEXT("bDumpDataDescriptions"), TEXT("bPrintShaderDebugValues"), TEXT("DebugBufferSize"), TEXT("bCachePinsOverride")};
		return Noisy.Contains(Name);
	}

	/** Unreal's exported text, shorter: no script paths, and 1.000000 written 1. */
	FString Compact(const FString& Text, const int32 MaxChars)
	{
		FString In = Text.Replace(TEXT("/Script/PCG."), TEXT("")).Replace(TEXT("/Script/Engine."), TEXT("")).Replace(TEXT("/Script/CoreUObject."), TEXT(""));
		FString Out;
		Out.Reserve(In.Len());
		for (int32 Index = 0; Index < In.Len(); ++Index)
		{
			Out.AppendChar(In[Index]);
			// After a number's decimal point, drop the zeros it ends in, and the point itself when nothing is left.
			if (In[Index] == TEXT('.') && Index > 0 && FChar::IsDigit(In[Index - 1]) && Index + 1 < In.Len() && FChar::IsDigit(In[Index + 1]))
			{
				int32 End = Index + 1;
				while (End < In.Len() && FChar::IsDigit(In[End])) ++End;
				FString Fraction = In.Mid(Index + 1, End - Index - 1);
				while (Fraction.EndsWith(TEXT("0"))) Fraction.LeftChopInline(1);
				if (Fraction.IsEmpty()) Out.LeftChopInline(1);
				else Out += Fraction;
				Index = End - 1;
			}
		}
		return Out.Len() > MaxChars ? Out.Left(MaxChars - 3) + TEXT("...") : Out;
	}

	/** What a node does, by its settings class: SurfaceSampler, DensityFilter, or Blueprint:ScaleByDensity. */
	FString KindOf(const UPCGNode& Node)
	{
		const UPCGSettings* Settings = Node.GetSettings();
		if (!Settings) return TEXT("Node");
		if (const UPCGBlueprintSettings* Blueprint = Cast<UPCGBlueprintSettings>(Settings))
		{
			const UClass* Element = Blueprint->GetElementType();
			return TEXT("Blueprint:") + (Element ? Element->GetName().Replace(TEXT("_C"), TEXT("")) : FString(TEXT("none")));
		}
		FString Kind = Settings->GetClass()->GetName();
		Kind.RemoveFromStart(TEXT("PCG"));
		Kind.RemoveFromEnd(TEXT("Settings"));
		return Kind;
	}

	/**
	 * A value as text. PCG's own structs show only the fields that differ from their defaults, and a typed constant only
	 * its type and value; other values are written whole, their own delta, so nothing equal to a zero is dropped.
	 */
	FString ExportCompact(const FProperty& Property, const void* Value, const void* Default, const int32 Depth = 0)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(&Property);
		const UScriptStruct* Struct = StructProperty ? StructProperty->Struct.Get() : nullptr;
		if (Struct == FPCGMetadataTypesConstantStruct::StaticStruct())
		{
			const FPCGMetadataTypesConstantStruct& Constant = *static_cast<const FPCGMetadataTypesConstantStruct*>(Value);
			return Constant.TypeToString() + TEXT(" ") + Constant.ToString();
		}
		if (Struct && Default && Depth < 3 && !(Struct->StructFlags & STRUCT_ExportTextItemNative) && Struct->GetOutermost()->GetName() == TEXT("/Script/PCG"))
		{
			TArray<FString> Fields;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient) || It->Identical_InContainer(Value, Default)) continue;
				Fields.Add(It->GetName() + TEXT("=") + ExportCompact(**It, It->ContainerPtrToValuePtr<void>(Value), It->ContainerPtrToValuePtr<void>(Default), Depth + 1));
			}
			return TEXT("(") + FString::Join(Fields, TEXT(",")) + TEXT(")");
		}
		FString Text;
		Property.ExportTextItem_Direct(Text, Value, Value, nullptr, PPF_None);
		return Text;
	}

	/** The settings that differ from the node's defaults, as Name=Value. */
	FString ChangedSettings(const UPCGSettings& Settings, const int32 MaxChars)
	{
		const UObject* Defaults = Settings.GetClass()->GetDefaultObject();
		TArray<FString> Parts;
		for (TFieldIterator<FProperty> It(Settings.GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)
				|| IsNoisyProperty(Property->GetFName()) || (Property->GetFName() == TEXT("Seed") && !Settings.UseSeed())
				|| Property->Identical_InContainer(&Settings, Defaults))
			{
				continue;
			}
			const FString Value = ExportCompact(*Property, Property->ContainerPtrToValuePtr<void>(&Settings), Property->ContainerPtrToValuePtr<void>(Defaults));
			Parts.Add(Property->GetName() + TEXT("=") + Compact(Value, 200));
		}
		return Compact(FString::Join(Parts, TEXT("; ")), MaxChars);
	}

	/** A mesh spawner's meshes: name, weight and cull distance, or what picks them when no weights do. */
	FString MeshesOf(const UPCGSettings& Settings)
	{
		const UPCGStaticMeshSpawnerSettings* Spawner = Cast<UPCGStaticMeshSpawnerSettings>(&Settings);
		if (!Spawner) return FString();
		const UPCGMeshSelectorWeighted* Weighted = Cast<UPCGMeshSelectorWeighted>(Spawner->MeshSelectorParameters);
		if (!Weighted)
		{
			return FString::Printf(TEXT("meshes picked by %s"), Spawner->MeshSelectorParameters ? *Spawner->MeshSelectorParameters->GetClass()->GetName() : TEXT("nothing"));
		}
		TArray<FString> Entries;
		for (const FPCGMeshSelectorWeightedEntry& Entry : Weighted->MeshEntries)
		{
			FString Line = FString::Printf(TEXT("%s x%d"), Entry.Descriptor.StaticMesh.IsNull() ? TEXT("(none)") : *Entry.Descriptor.StaticMesh.GetAssetName(), Entry.Weight);
			if (Entry.Descriptor.InstanceEndCullDistance > 0)
			{
				Line += FString::Printf(TEXT(" cull %d-%d m"), Entry.Descriptor.InstanceStartCullDistance / 100, Entry.Descriptor.InstanceEndCullDistance / 100);
			}
			Entries.Add(Line);
		}
		return Entries.IsEmpty() ? FString(TEXT("no meshes")) : FString::Join(Entries, TEXT(", "));
	}

	/** The node's title as authored; the editor's list view would capitalize every word of it. */
	FString TitleOf(const UPCGNode& Node)
	{
		return Node.HasAuthoredTitle() ? Node.NodeTitle.ToString() : FString();
	}

	/** One node as a recipe step: kind, title, and its changed settings in parentheses. */
	FString StepOf(const UPCGNode& Node, const int32 MaxChars)
	{
		const FString Title = TitleOf(Node);
		const FString Changed = Node.GetSettings() ? ChangedSettings(*Node.GetSettings(), MaxChars) : FString();
		return KindOf(Node) + (Title.IsEmpty() ? FString() : TEXT(" '") + Title + TEXT("'")) + (Changed.IsEmpty() ? FString() : TEXT(" (") + Changed + TEXT(")"));
	}

	struct FInputLink
	{
		FName ToPin;
		const UPCGNode* From = nullptr;
		FName FromPin;
	};

	/** Each node's inputs: which node and pin feed each of its input pins. */
	TMap<const UPCGNode*, TArray<FInputLink>> InputsOf(const TArray<const UPCGNode*>& Nodes)
	{
		TMap<const UPCGNode*, TArray<FInputLink>> Inputs;
		for (const UPCGNode* Node : Nodes)
		{
			for (const UPCGPin* Pin : Node->GetInputPins())
			{
				if (!Pin) continue;
				for (const UPCGEdge* Edge : Pin->Edges)
				{
					const UPCGPin* Other = Edge ? Edge->InputPin.Get() : nullptr;
					if (Other && Other->Node) Inputs.FindOrAdd(Node).Add({Pin->Properties.Label, Other->Node, Other->Properties.Label});
				}
			}
		}
		return Inputs;
	}

	/** The input the points come down through: Target for copies (Source is the stamp), else In, Source or the first. */
	int32 MainInput(const UPCGNode& Node, const TArray<FInputLink>& Links)
	{
		const bool bCopy = KindOf(Node) == TEXT("CopyPoints");
		for (const TCHAR* Preferred : {bCopy ? TEXT("Target") : TEXT("In"), TEXT("In"), TEXT("Source"), TEXT("Surface")})
		{
			const int32 Found = Links.IndexOfByPredicate([Preferred](const FInputLink& Link) { return Link.ToPin == FName(Preferred); });
			if (Found != INDEX_NONE) return Found;
		}
		return Links.IsEmpty() ? INDEX_NONE : 0;
	}

	FString NameOf(const UPCGNode& Node)
	{
		const FString Title = TitleOf(Node);
		return Title.IsEmpty() ? KindOf(Node) : Title;
	}

	/** A spawner's chain upstream: each step, its other inputs in brackets, until the graph's input or a step seen above. */
	FString Recipe(const UPCGNode& Spawner, const TMap<const UPCGNode*, TArray<FInputLink>>& Inputs)
	{
		TArray<FString> Steps;
		TSet<const UPCGNode*> Seen;
		const UPCGNode* Node = &Spawner;
		for (int32 Depth = 0; Node && Depth < MaxRecipeDepth; ++Depth)
		{
			if (Seen.Contains(Node))
			{
				Steps.Add(NameOf(*Node) + TEXT(" (seen above)"));
				break;
			}
			Seen.Add(Node);
			FString Step = Depth == 0 ? FString() : StepOf(*Node, 220);
			const TArray<FInputLink>* Links = Inputs.Find(Node);
			const int32 Main = Links ? MainInput(*Node, *Links) : INDEX_NONE;
			if (Links)
			{
				TArray<FString> Others;
				for (int32 Index = 0; Index < Links->Num(); ++Index)
				{
					if (Index != Main) Others.Add((*Links)[Index].ToPin.ToString() + TEXT(": ") + NameOf(*(*Links)[Index].From));
				}
				if (!Others.IsEmpty()) Step += (Step.IsEmpty() ? TEXT("[") : TEXT(" [")) + FString::Join(Others, TEXT("; ")) + TEXT("]");
			}
			if (!Step.IsEmpty()) Steps.Add(Step);
			Node = Main != INDEX_NONE ? (*Links)[Main].From : nullptr;
		}
		const FString Title = TitleOf(Spawner);
		return FString::Printf(TEXT("%s%s [%s] <- %s"), *KindOf(Spawner), Title.IsEmpty() ? TEXT("") : *(TEXT(" '") + Title + TEXT("'")),
			Spawner.GetSettings() ? *MeshesOf(*Spawner.GetSettings()) : TEXT(""), *FString::Join(Steps, TEXT(" <- ")));
	}

	/** Adds lines while the answer stays small; says what was left out. */
	struct FLineBudget
	{
		int32 Chars = 0;
		bool bTruncated = false;

		bool Add(TArray<FString>& Lines, const FString& Line, const FString& Filter)
		{
			if (!Filter.IsEmpty() && !Line.Contains(Filter, ESearchCase::IgnoreCase)) return true;
			if (Lines.Num() >= MaxReadLines || Chars + Line.Len() > MaxReadChars)
			{
				bTruncated = true;
				return false;
			}
			Chars += Line.Len();
			Lines.Add(Line);
			return true;
		}
	};

	/** Any graph read for an agent: recipes always, parameters, and with Detail every node and edge. */
	void ReadGraph(const UPCGGraph& Graph, const FString& Detail, const FString& Filter, FHyperAIPCGScatterInspectReport& Report)
	{
		TArray<const UPCGNode*> Nodes;
		for (const UPCGNode* Node : Graph.GetNodes())
		{
			if (Node) Nodes.Add(Node);
		}
		for (const UPCGNode* Special : {Graph.GetInputNode(), Graph.GetOutputNode()})
		{
			if (Special && !Nodes.Contains(Special)) Nodes.Add(Special);
		}
		const TMap<const UPCGNode*, TArray<FInputLink>> Inputs = InputsOf(Nodes);
		FLineBudget Budget;
		int32 Spawners = 0;
		int32 EdgeCount = 0;
		for (const TPair<const UPCGNode*, TArray<FInputLink>>& Pair : Inputs) EdgeCount += Pair.Value.Num();
		for (const UPCGNode* Node : Nodes)
		{
			if (!Cast<UPCGStaticMeshSpawnerSettings>(Node->GetSettings())) continue;
			++Spawners;
			Budget.Add(Report.Recipes, Recipe(*Node, Inputs), Filter);
		}
		if (const FInstancedPropertyBag* Parameters = Graph.GetUserParametersStruct())
		{
			if (const UPropertyBag* Bag = Parameters->GetPropertyBagStruct())
			{
				for (const FPropertyBagPropertyDesc& Desc : Bag->GetPropertyDescs())
				{
					const TValueOrError<FString, EPropertyBagResult> Value = Parameters->GetValueSerializedString(Desc.Name);
					Report.Parameters.Add(FString::Printf(TEXT("%s (%s) = %s"), *Desc.Name.ToString(),
						*StaticEnum<EPropertyBagPropertyType>()->GetNameStringByValue(static_cast<int64>(Desc.ValueType)), Value.HasValue() ? *Compact(Value.GetValue(), 200) : TEXT("?")));
				}
			}
		}
		const FString Level = Detail.TrimStartAndEnd().ToLower();
		if (Level == TEXT("nodes") || Level == TEXT("full"))
		{
			for (const UPCGNode* Node : Nodes)
			{
				FString Line = Node->GetName() + TEXT(" | ") + StepOf(*Node, 900);
				const FString Meshes = Node->GetSettings() ? MeshesOf(*Node->GetSettings()) : FString();
				if (!Meshes.IsEmpty()) Line += TEXT(" | ") + Meshes;
				if (!Budget.Add(Report.Nodes, Line, Filter)) break;
			}
		}
		if (Level == TEXT("full"))
		{
			for (const UPCGNode* Node : Nodes)
			{
				const TArray<FInputLink>* Links = Inputs.Find(Node);
				for (int32 Index = 0; Links && Index < Links->Num(); ++Index)
				{
					const FInputLink& Link = (*Links)[Index];
					if (!Budget.Add(Report.Edges, FString::Printf(TEXT("%s.%s -> %s.%s"), *Link.From->GetName(), *Link.FromPin.ToString(), *Node->GetName(), *Link.ToPin.ToString()), Filter)) break;
				}
			}
		}
		Report.bTruncated = Budget.bTruncated;
		Report.Diagnostic = FString::Printf(TEXT("%s: %d node(s), %d edge(s), %d mesh spawner(s), %d parameter(s)%s%s."), *Graph.GetName(), Nodes.Num(), EdgeCount, Spawners,
			Report.Parameters.Num(), IsScatterGraph(Graph) ? TEXT("; a scatter graph this tool built")
				: IsEcosystemGraph(Graph) ? TEXT("; an ecosystem this tool built, its op in Ecosystem") : TEXT(""),
			Budget.bTruncated ? TEXT("; some lines were left out, narrow them with Filter") : TEXT(""));
	}

	/**
	 * The instanced mesh components a volume's generation made: its own and, when partitioned, those of its local
	 * components. Partition actors are shared by every partitioned volume on the same grid, so only what PCG manages for
	 * this volume counts, never everything on the actor.
	 */
	TArray<const UInstancedStaticMeshComponent*> GeneratedComponents(const APCGVolume& Volume)
	{
		TArray<const UInstancedStaticMeshComponent*> Out;
		const UPCGComponent* Component = Volume.PCGComponent;
		if (!Component) return Out;
		TArray<const UPCGComponent*> Sources = {Component};
		UWorld* World = Volume.GetWorld();
		if (World && Component->IsPartitioned())
		{
			for (TActorIterator<APCGPartitionActor> It(World); It; ++It)
			{
				if (const UPCGComponent* Local = It->GetLocalComponent(Component)) Sources.Add(Local);
			}
		}
		for (const UPCGComponent* Source : Sources)
		{
			Source->ForEachConstManagedResource([&Out](const UPCGManagedResource* Resource)
			{
				const UPCGManagedISMComponent* Managed = Cast<UPCGManagedISMComponent>(Resource);
				if (const UInstancedStaticMeshComponent* Instanced = Managed ? Managed->GetComponent() : nullptr) Out.AddUnique(Instanced);
			});
		}
		return Out;
	}

	/** Instances the volume's generation made; per mesh when asked. */
	int32 CountInstances(const APCGVolume& Volume, int32* OutMeshes = nullptr, TMap<FString, int32>* OutPerMesh = nullptr)
	{
		int32 Instances = 0;
		TSet<const UStaticMesh*> Meshes;
		for (const UInstancedStaticMeshComponent* Component : GeneratedComponents(Volume))
		{
			if (!Component || Component->GetInstanceCount() == 0) continue;
			Instances += Component->GetInstanceCount();
			const UStaticMesh* Mesh = Component->GetStaticMesh();
			Meshes.Add(Mesh);
			if (OutPerMesh) OutPerMesh->FindOrAdd(Mesh ? Mesh->GetName() : FString(TEXT("(none)"))) += Component->GetInstanceCount();
		}
		if (OutMeshes) *OutMeshes = Meshes.Num();
		return Instances;
	}

	/** Generating, cleaning up or about to refresh, here or in any of its partition cells. */
	bool IsBusy(const APCGVolume& Volume)
	{
		auto Busy = [](const UPCGComponent& Component) { return Component.IsGenerating() || Component.IsCleaningUp() || Component.IsRefreshInProgress(); };
		const UPCGComponent* Component = Volume.PCGComponent;
		if (!Component) return false;
		if (Busy(*Component)) return true;
		UWorld* World = Volume.GetWorld();
		if (World && Component->IsPartitioned())
		{
			for (TActorIterator<APCGPartitionActor> It(World); It; ++It)
			{
				const UPCGComponent* Local = It->GetLocalComponent(Component);
				if (Local && Busy(*Local)) return true;
			}
		}
		return false;
	}

	/**
	 * What a generation job hears from PCG. PCG can cancel a generation it has started, when a change it tracks lands, and
	 * run it again from a refresh a frame later; the job ends when PCG says the volume generated, or after a quiet spell.
	 */
	struct FGenerationWatch
	{
		TWeakObjectPtr<UPCGComponent> Component;
		FDelegateHandle Generated;
		FDelegateHandle Cancelled;
		bool bGenerated = false;
		bool bCancelled = false;
		double QuietSince = 0.0;

		void Stop()
		{
			if (UPCGComponent* Pinned = Component.Get())
			{
				Pinned->OnPCGGraphGeneratedDelegate.Remove(Generated);
				Pinned->OnPCGGraphCancelledDelegate.Remove(Cancelled);
			}
			Generated.Reset();
			Cancelled.Reset();
		}
	};

	/** Seconds a volume may sit idle without PCG saying it generated before the job takes what is there. */
	constexpr double GenerateQuietSeconds = 3.0;

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
		const TSharedRef<FGenerationWatch> Watch = MakeShared<FGenerationWatch>();
		Watch->Component = Component;
		Watch->Generated = Component->OnPCGGraphGeneratedDelegate.AddLambda([Watch](UPCGComponent*) { Watch->bGenerated = true; });
		Watch->Cancelled = Component->OnPCGGraphCancelledDelegate.AddLambda([Watch](UPCGComponent*) { Watch->bCancelled = true; });
		// PCG declines quietly (no graph, inactive, no subsystem): catch that before a job is shown.
		if (Component->GenerateLocalGetTaskId(/*bForce=*/true) == InvalidPCGTaskId)
		{
			Watch->Stop();
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
		// Cancelled from the Activity panel, or past the deadline: PCG stops too.
		Request.OnCancel = [Watch]()
		{
			if (UPCGComponent* Pinned = Watch->Component.Get()) Pinned->CancelGeneration();
			Watch->Stop();
		};
		FString JobId;
		if (!FHyperAIStudioAsyncJobHost::Start(Request, [Weak, Watch](FString& OutProgress, FString& OutDiagnostic)
			{
				const APCGVolume* Pinned = Weak.Get();
				if (!Pinned || !Pinned->PCGComponent)
				{
					Watch->Stop();
					OutDiagnostic = TEXT("The volume is gone.");
					return EHyperAIStudioAsyncJobPoll::Failed;
				}
				if (IsBusy(*Pinned))
				{
					Watch->QuietSince = 0.0;
					OutProgress = TEXT("generating");
					return EHyperAIStudioAsyncJobPoll::Running;
				}
				if (!Watch->bGenerated)
				{
					const double Now = FPlatformTime::Seconds();
					if (Watch->QuietSince == 0.0) Watch->QuietSince = Now;
					if (Now - Watch->QuietSince < GenerateQuietSeconds)
					{
						OutProgress = Watch->bCancelled ? TEXT("PCG restarted the generation") : TEXT("waiting for PCG");
						return EHyperAIStudioAsyncJobPoll::Running;
					}
				}
				Watch->Stop();
				int32 Meshes = 0;
				TMap<FString, int32> PerMesh;
				const int32 Instances = CountInstances(*Pinned, &Meshes, &PerMesh);
				// The points that reached the graph's end tell an empty sample from an empty spawn.
				int32 Points = 0;
				for (const FPCGTaggedData& Data : Pinned->PCGComponent->GetGeneratedGraphOutput().TaggedData)
				{
					if (const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(Data.Data)) Points += PointData->GetNumPoints();
				}
				FString Why;
				if (Watch->bCancelled && !Watch->bGenerated)
				{
					Why = TEXT(": PCG cancelled the generation and did not run it again, so generate the volume again (volume.generate)");
				}
				else if (Instances == 0 && Points == 0)
				{
					const TArray<FString> Unseen = UnseenLandscapes(*Pinned);
					Why = Unseen.IsEmpty()
						? FString(TEXT(": no point survived, so check that a landscape lies inside the volume, its ground is within the slope limit, and any paint layer the graph needs is painted"))
						: FString::Printf(TEXT(": PCG passed over %s, kept whole rather than split into streaming proxies; World Partition shows PCG such a landscape only once the level is saved, so save it (bSave) and generate again"),
							*FString::Join(Unseen, TEXT(", ")));
				}
				PerMesh.ValueSort([](const int32 A, const int32 B) { return A > B; });
				TArray<FString> Counts;
				for (const TPair<FString, int32>& Pair : PerMesh)
				{
					if (Counts.Num() >= 16) break;
					Counts.Add(FString::Printf(TEXT("%s %d"), *Pair.Key, Pair.Value));
				}
				OutDiagnostic = FString::Printf(TEXT("%s generated %d instance(s) of %d mesh(es)%s%s%s."), *Pinned->GetActorLabel(), Instances, Meshes,
					Points > 0 ? *FString::Printf(TEXT(" from %d point(s)"), Points) : TEXT(""), *Why,
					Counts.IsEmpty() ? TEXT("") : *(TEXT("; by mesh: ") + FString::Join(Counts, TEXT(", "))));
				return EHyperAIStudioAsyncJobPoll::Completed;
			}, JobId, OutError))
		{
			Watch->Stop();
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
		/** Graphs the plan makes, and whether each is an ecosystem. */
		TMap<FString, bool> NewGraphs;
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
			const bool* PlannedEcosystem = NewGraphs.Find(Path.ToLower());
			const bool bPlannedGraph = PlannedEcosystem != nullptr;
			if (Op.Density < 0.f || Op.Density > C::MaxDensity) return Fail(FString::Printf(TEXT("Density is 0 to %.0f points per square meter."), C::MaxDensity));
			if (Op.SlopeMax > 90.f) return Fail(TEXT("SlopeMax is 0 to 90 degrees."));
			if ((Op.Scale.X != 0.0 || Op.Scale.Y != 0.0) && !(Op.Scale.X > 0.0 && Op.Scale.Y >= Op.Scale.X)) return Fail(TEXT("Scale runs from a Min (X) above 0 to a Max (Y) at least as big."));
			TOptional<bool> Flag;
			if (!ParseFlag(Op.RandomYaw, Flag)) return Fail(TEXT("RandomYaw is true or false."));
			if (!ParseFlag(Op.AlignToGround, Flag)) return Fail(TEXT("AlignToGround is true or false."));
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
				NewGraphs.Add(Path.ToLower(), false);
			}
			else if (Kind == TEXT("graph.set"))
			{
				const UPCGGraph* Graph = Load<UPCGGraph>(Path);
				if (!Graph && !bPlannedGraph) return Fail(FString::Printf(TEXT("%s is not a PCG graph."), *Op.Graph));
				if ((Graph && !IsScatterGraph(*Graph)) || (PlannedEcosystem && *PlannedEcosystem)) return Fail(FString::Printf(TEXT("%s is not a scatter graph this tool built."), *Op.Graph));
			}
			else if (Kind == TEXT("ecosystem.create") || Kind == TEXT("ecosystem.set"))
			{
				if (Kind == TEXT("ecosystem.create"))
				{
					if (!Path.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Path) || FPackageName::DoesPackageExist(Path) || bPlannedGraph)
					{
						return Fail(TEXT("Name the new graph as a free path under /Game, like /Game/PCG/PCG_Forest."));
					}
					NewGraphs.Add(Path.ToLower(), true);
				}
				else
				{
					const UPCGGraph* Graph = Load<UPCGGraph>(Path);
					if (!Graph && !bPlannedGraph) return Fail(FString::Printf(TEXT("%s is not a PCG graph."), *Op.Graph));
					if ((Graph && !IsEcosystemGraph(*Graph)) || (PlannedEcosystem && !*PlannedEcosystem))
					{
						return Fail(FString::Printf(TEXT("%s is not an ecosystem graph this tool built; read it with hyper_pcg_scatter_inspect."), *Op.Graph));
					}
				}
				FString Problem;
				if (!CheckEcosystem(Op, Problem)) return Fail(Problem);
			}
			else if (Kind == TEXT("volume.create"))
			{
				if (Op.Volume.TrimStartAndEnd().IsEmpty()) return Fail(TEXT("Name the Volume."));
				if (FindVolume(World, Op.Volume) || NewVolumes.Contains(Op.Volume.ToLower())) return Fail(FString::Printf(TEXT("A PCG volume is labelled %s already."), *Op.Volume));
				if (!Op.Cover.TrimStartAndEnd().IsEmpty())
				{
					if (!FindLandscape(World, Op.Cover)) return Fail(FString::Printf(TEXT("No landscape is labelled %s."), *Op.Cover));
				}
				else if (!(Op.Size.X > 0.0 && Op.Size.Y > 0.0))
				{
					return Fail(TEXT("Give the volume's Size in meters, or a landscape to Cover."));
				}
				TOptional<bool> Partitioned;
				if (!ParseFlag(Op.Partitioned, Partitioned)) return Fail(TEXT("Partitioned is true or false."));
				if (Partitioned.Get(false) && !World.GetWorldPartition()) return Fail(TEXT("Only a World Partition level generates in partitions; leave Partitioned off here."));
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
			if (!FHyperAIStudioSourceControl::SaveAsset(*Graph, OutError)) return false;
			Report.Saved.Add(Graph->GetPathName());
			Report.Notes.Add(FString::Printf(TEXT("Graph %s: %s."), *Graph->GetName(), *FString::Join(DescribeGraph(*Graph), TEXT("; ")).Left(800)));
			return true;
		}
		if (Kind == TEXT("ecosystem.create") || Kind == TEXT("ecosystem.set"))
		{
			UPCGGraph* Graph = nullptr;
			if (Kind == TEXT("ecosystem.create"))
			{
				UPackage* Package = CreatePackage(*Path);
				Graph = NewObject<UPCGGraph>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
				FAssetRegistryModule::AssetCreated(Graph);
			}
			else
			{
				Graph = Load<UPCGGraph>(Path);
				Graph->Modify();
			}
			if (!BuildEcosystem(*Graph, Op, OutError)) return false;
			Graph->MarkPackageDirty();
			if (!FHyperAIStudioSourceControl::SaveAsset(*Graph, OutError)) return false;
			Report.Saved.Add(Graph->GetPathName());
			int32 Species = 0;
			for (const FHyperAIPCGBiome& Biome : Op.Biomes) Species += Biome.Species.Num();
			Report.Notes.Add(FString::Printf(TEXT("Ecosystem %s: %d biome(s), %d species, %d node(s). %s"), *Graph->GetName(), Op.Biomes.Num(), Species,
				Graph->GetNodes().Num(), *Graph->Description.ToString().Left(600)));
			return true;
		}
		if (Kind == TEXT("volume.create"))
		{
			UPCGGraph* Graph = Load<UPCGGraph>(Path);
			FVector Center = Op.Center;
			FVector2D Size = Op.Size;
			float Height = Op.Height > 0.f ? Op.Height : DefaultHeight;
			if (!Op.Cover.TrimStartAndEnd().IsEmpty())
			{
				// The ground as PCG sees it, with room above and below for what grows on it.
				const ALandscapeProxy* Ground = FindLandscape(World, Op.Cover);
				FBox Bounds = Ground ? PCGHelpers::GetLandscapeBounds(Ground) : FBox(EForceInit::ForceInit);
				// World Partition knows a landscape's whole bounds once it is saved; until then its loaded parts have theirs.
				ULandscapeInfo* Info = Ground && !Bounds.IsValid ? Ground->GetLandscapeInfo() : nullptr;
				if (Info)
				{
					Info->ForEachLandscapeProxy([&Bounds](ALandscapeProxy* Proxy)
					{
						Bounds += Proxy->GetComponentsBoundingBox(/*bNonColliding=*/true);
						return true;
					});
				}
				if (!Bounds.IsValid)
				{
					OutError = FString::Printf(TEXT("%s has no bounds PCG can see; save the level so World Partition knows it, then try again."), *Op.Cover);
					return false;
				}
				Center = Bounds.GetCenter();
				Size = FVector2D(Bounds.GetSize().X / 100.0, Bounds.GetSize().Y / 100.0);
				Height = FMath::Max(Height, static_cast<float>(Bounds.GetSize().Z / 100.0) + 100.f);
			}
			FActorSpawnParameters Parameters;
			Parameters.ObjectFlags |= RF_Transactional;
			APCGVolume* Volume = Graph ? World.SpawnActor<APCGVolume>(Center, FRotator::ZeroRotator, Parameters) : nullptr;
			if (!Volume)
			{
				OutError = TEXT("The level refused a new PCG volume.");
				return false;
			}
			UCubeBuilder* Box = NewObject<UCubeBuilder>();
			Box->X = static_cast<float>(Size.X * 100.0);
			Box->Y = static_cast<float>(Size.Y * 100.0);
			Box->Z = Height * 100.f;
			UActorFactory::CreateBrushForVolumeActor(Volume, Box);
			Volume->SetActorLabel(Op.Volume.TrimStartAndEnd());
			Volume->SetFolderPath(TEXT("PCG"));
			Volume->PCGComponent->SetGraph(Graph);
			TOptional<bool> Partitioned;
			ParseFlag(Op.Partitioned, Partitioned);
			if (Partitioned.Get(false)) Volume->PCGComponent->SetIsPartitioned(true);
			Report.Notes.Add(FString::Printf(TEXT("%s placed: %.0f x %.0f m at (%.0f, %.0f), running %s%s."), *Volume->GetActorLabel(), Size.X, Size.Y, Center.X, Center.Y,
				*Graph->GetName(), Partitioned.Get(false) ? TEXT(", generated in World Partition cells") : TEXT("")));
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
			Report.Diagnostic = FString::Printf(TEXT("%s is not a PCG graph; give its path, like /Game/PCG/PCG_Forest."), *Request.Graph);
			return Report;
		}
		const FString Detail = Request.Detail.TrimStartAndEnd().ToLower();
		if (!Detail.IsEmpty() && Detail != TEXT("summary") && Detail != TEXT("nodes") && Detail != TEXT("full"))
		{
			Report.Diagnostic = TEXT("Detail is summary, nodes or full.");
			return Report;
		}
		if (IsScatterGraph(*Graph)) Report.Settings = DescribeGraph(*Graph);
		if (IsEcosystemGraph(*Graph))
		{
			Report.Settings.Add(Graph->Description.ToString());
			if (const FString* Spec = Graph->GetPackage()->GetMetaData().FindValue(Graph, EcosystemSpecKey)) Report.Ecosystem = *Spec;
		}
		ReadGraph(*Graph, Detail, Request.Filter.TrimStartAndEnd(), Report);
		Report.bOk = true;
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
