// Games by Hyper 2026.

#include "HyperAIStudioSceneCost.h"

#include "Algo/Sort.h"
#include "Components/BrushComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HyperAIStudioSceneSpatialToolset.h"
#include "HyperAIStudioSpatialQueries.h"
#include "InstancedFoliageActor.h"
#include "LandscapeComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeProxy.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "RHIStats.h"
#include "Rendering/NaniteResourcesHelper.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshResources.h"
#include "UObject/UObjectHash.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHelpers.h"

namespace HyperAIStudio::SceneSpatial::Cost
{
	namespace
	{
		/** LOD0 triangles past which a mesh drawn without Nanite is called heavy. */
		constexpr int64 HeavyTriangles = 100000;
		/** Shadow casters smaller than this across (radius, cm) cost a shadow draw each for little shadow. */
		constexpr double SmallCasterRadius = 20.0;
		constexpr int32 ManySmallCasters = 500;
		/** Movable shadow-casting local lights on one primitive past which its shadow draws multiply noticeably. */
		constexpr int32 ManyShadowLights = 4;
		/** Sections an instanced mesh may have before each instance batch draws too often. */
		constexpr int32 ManySections = 4;

		/** A light that casts dynamic shadows: on everything when directional, inside its radius otherwise. */
		struct FShadowLight
		{
			bool bDirectional = false;
			FVector Center = FVector::ZeroVector;
			double RadiusSquared = 0.0;
		};

		struct FMeshTally
		{
			const UStaticMesh* Mesh = nullptr;
			int32 Components = 0;
			int64 Instances = 0;
			/** One instance's triangles: LOD0, or the Nanite source. */
			int64 Triangles = 0;
			int32 Sections = 0;
			int32 DrawCalls = 0;
			int32 Materials = 0;
			bool bNanite = false;
			bool bShadow = false;
		};

		struct FActorTally
		{
			const AActor* Actor = nullptr;
			/** Non-Nanite triangles: static LOD0 times instances, skeletal LOD0, landscape. */
			int64 Triangles = 0;
			int64 NaniteTriangles = 0;
			int64 Instances = 0;
			int32 DrawCalls = 0;
			int32 Sections = 0;
			int32 Materials = 0;
			bool bNanite = false;
			bool bShadow = false;
			bool bCounted = false;
		};

		int32 Narrow(const int64 Value)
		{
			return static_cast<int32>(FMath::Clamp<int64>(Value, 0, MAX_int32));
		}

		void AddFinding(FHyperAISceneCostReport& Out, const FString& Finding)
		{
			if (Out.Findings.Num() < MaxFindings) Out.Findings.AddUnique(Finding);
		}

		/** Editor helpers, debug drawing and collision that never draw in a game: trigger shapes, volumes, landscape collision. */
		bool IsHelper(const UPrimitiveComponent& Component)
		{
			return Spatial::IsHelperPrimitive(Component) || Component.IsA<UShapeComponent>() || Component.IsA<UBrushComponent>()
				|| Component.IsA<ULandscapeHeightfieldCollisionComponent>();
		}

		/** Instances of Component whose middle is inside Box. */
		int64 InstancesInside(const UInstancedStaticMeshComponent& Component, const FBox& Box)
		{
			const FTransform& ToWorld = Component.GetComponentTransform();
			int64 Inside = 0;
			for (const FInstancedStaticMeshInstanceData& Instance : Component.PerInstanceSMData)
			{
				Inside += Box.IsInside(ToWorld.TransformPosition(FVector(Instance.Transform.GetOrigin()))) ? 1 : 0;
			}
			return Inside;
		}

		FHyperAISceneCostEntry ActorEntry(const FActorTally& Tally)
		{
			FHyperAISceneCostEntry Entry;
			Entry.Name = Spatial::LabelOf(*Tally.Actor);
			Entry.Class = Tally.Actor->GetClass()->GetName();
			Entry.Triangles = Tally.Triangles;
			Entry.Instances = Narrow(Tally.Instances);
			Entry.TotalTriangles = Tally.Triangles + Tally.NaniteTriangles;
			Entry.DrawCalls = Tally.DrawCalls;
			Entry.Sections = Tally.Sections;
			Entry.bNanite = Tally.bNanite;
			Entry.bCastsShadow = Tally.bShadow;
			Entry.Materials = Tally.Materials;
			Entry.Location = Spatial::FormatVector(Tally.Actor->GetActorLocation());
			return Entry;
		}

		FHyperAISceneCostEntry MeshEntry(const FMeshTally& Tally)
		{
			FHyperAISceneCostEntry Entry;
			Entry.Name = Tally.Mesh->GetPathName();
			Entry.Class = Tally.Mesh->GetClass()->GetName();
			Entry.Triangles = Tally.Triangles;
			Entry.Instances = Narrow(Tally.Instances);
			Entry.TotalTriangles = Tally.Triangles * Tally.Instances;
			Entry.DrawCalls = Tally.DrawCalls;
			Entry.Sections = Tally.Sections;
			Entry.bNanite = Tally.bNanite;
			Entry.bCastsShadow = Tally.bShadow;
			Entry.Materials = Tally.Materials;
			Entry.Location = FString::Printf(TEXT("%d component(s)"), Tally.Components);
			return Entry;
		}
	}

	int64 EstimateBytes(const FHyperAISceneCostReport& Report)
	{
		auto Bytes = [](const FString& Value) { return 16 + 2ll * Value.Len(); };
		// About sixty numbers and flags with their names.
		int64 Size = 1600 + Bytes(Report.Status) + Bytes(Report.Diagnostic) + Bytes(Report.LevelPath) + Bytes(Report.World) + Bytes(Report.Scope)
			+ Bytes(Report.TruncatedReason);
		for (const TArray<FHyperAISceneCostEntry>* Entries : {&Report.HeaviestActors, &Report.HeaviestMeshes})
		{
			for (const FHyperAISceneCostEntry& Entry : *Entries)
			{
				Size += 200 + Bytes(Entry.Name) + Bytes(Entry.Class) + Bytes(Entry.Location);
			}
		}
		for (const TArray<FString>* Lines : {&Report.TranslucentList, &Report.OtherClasses, &Report.Findings})
		{
			for (const FString& Line : *Lines)
			{
				Size += Bytes(Line);
			}
		}
		return Size;
	}

	void Measure(UWorld& World, const FHyperAISceneCostRequest& Request, FHyperAISceneCostReport& Out)
	{
		const double Start = FPlatformTime::Seconds();
		const double Deadline = Start + FMath::Max(Request.DeadlineMs, 1) / 1000.0;
		Out.LevelPath = World.GetOutermost()->GetName();
		FString Scope = Request.Scope.TrimStartAndEnd().ToLower();
		if (Scope.IsEmpty()) Scope = TEXT("level");
		Out.Scope = Scope;
		auto Refuse = [&Out](const FString& Why)
		{
			Out.bOk = false;
			Out.Status = TEXT("invalid_request");
			Out.Diagnostic = Why;
		};
		const bool bBox = Scope == TEXT("box");
		const bool bNamed = Scope == TEXT("actors");
		if (!bBox && !bNamed && Scope != TEXT("level"))
		{
			Refuse(TEXT("Scope is level, box or actors."));
			return;
		}
		FBox Box(ForceInit);
		if (bBox)
		{
			const FVector& Min = Request.BoxMin;
			const FVector& Max = Request.BoxMax;
			if (Min.X >= Max.X || Min.Y >= Max.Y || Min.Z >= Max.Z)
			{
				Refuse(TEXT("Scope box needs BoxMin below BoxMax on every axis, cm."));
				return;
			}
			Box = FBox(Min, Max);
		}
		TSet<const AActor*> Named;
		if (bNamed)
		{
			if (Request.Actors.IsEmpty() || Request.Actors.Num() > FHyperAIStudioSceneSpatialContracts::MaxScopeActors)
			{
				Refuse(FString::Printf(TEXT("Scope actors needs Actors: 1 to %d labels or paths."), FHyperAIStudioSceneSpatialContracts::MaxScopeActors));
				return;
			}
			TArray<FString> Missing;
			for (const FString& Name : Request.Actors)
			{
				FString Error;
				if (const AActor* Actor = Spatial::FindActor(World, Name, Error)) Named.Add(Actor);
				else Missing.Add(Error);
			}
			if (!Missing.IsEmpty())
			{
				Refuse(FString::Join(Missing, TEXT(" ")));
				return;
			}
		}

		// Lights first: a primitive's shadow draws depend on every light that reaches it, whatever the scope.
		TArray<FShadowLight> ShadowLights;
		int32 LocalShadowLights = 0;
		ForEachObjectOfClass(ULightComponentBase::StaticClass(), [&](UObject* Object)
		{
			const ULightComponentBase* Light = static_cast<const ULightComponentBase*>(Object);
			if (!IsValid(Light) || Light->IsTemplate() || !Light->IsRegistered() || Light->GetWorld() != &World || !Light->IsVisible()) return;
			const AActor* Owner = Light->GetOwner();
			if (Owner && !Request.bIncludeHidden && Owner->IsHiddenEd()) return;
			const ULocalLightComponent* Local = Cast<ULocalLightComponent>(Light);
			const bool bDirectional = Light->IsA<UDirectionalLightComponent>();
			bool bInScope = true;
			if (bNamed)
			{
				bInScope = Owner && Named.Contains(Owner);
			}
			else if (bBox && Local)
			{
				bInScope = Box.ComputeSquaredDistanceToPoint(Local->GetComponentLocation()) <= FMath::Square(Local->AttenuationRadius);
			}
			else if (bBox && !bDirectional && !Light->IsA<USkyLightComponent>())
			{
				bInScope = Box.IsInside(Light->GetComponentLocation());
			}
			const ULightComponent* Typed = Cast<ULightComponent>(Light);
			const bool bDynamicShadows = Typed && Light->Mobility != EComponentMobility::Static && Light->CastShadows && Light->CastDynamicShadows && Light->bAffectsWorld;
			if (bInScope)
			{
				FHyperAISceneCostLights& Lights = Out.Lights;
				// A spot light is a point light too, so it is asked first.
				if (bDirectional) ++Lights.Directional;
				else if (Light->IsA<USpotLightComponent>()) ++Lights.Spot;
				else if (Light->IsA<URectLightComponent>()) ++Lights.Rect;
				else if (Light->IsA<UPointLightComponent>()) ++Lights.Point;
				else if (Light->IsA<USkyLightComponent>()) ++Lights.Sky;
				switch (Light->Mobility.GetValue())
				{
				case EComponentMobility::Movable: ++Lights.Movable; break;
				case EComponentMobility::Stationary: ++Lights.Stationary; break;
				default: ++Lights.Static; break;
				}
				Lights.ShadowCastingDynamic += bDynamicShadows ? 1 : 0;
				Lights.MegaLights += Typed && Typed->bAllowMegaLights ? 1 : 0;
			}
			if (!bDynamicShadows) return;
			if (bDirectional)
			{
				ShadowLights.Add({true, FVector::ZeroVector, 0.0});
			}
			else if (Local && ++LocalShadowLights <= MaxShadowLights)
			{
				ShadowLights.Add({false, Local->GetComponentLocation(), FMath::Square(static_cast<double>(Local->AttenuationRadius))});
			}
		});
		if (LocalShadowLights > MaxShadowLights)
		{
			ShadowLights.RemoveAll([](const FShadowLight& Light) { return !Light.bDirectional; });
			AddFinding(Out, FString::Printf(TEXT("many_lights: %d local lights cast dynamic shadows; shadow draws count directional lights only."), LocalShadowLights));
		}

		TMap<const UStaticMesh*, FMeshTally> Meshes;
		TArray<FActorTally> Actors;
		TSet<const UMaterialInterface*> NaniteBins;
		TMap<const UMaterialInterface*, int32> Translucent;
		TMap<FString, int32> Others;
		int64 Instances = 0;
		int64 FoliageInstances = 0;
		int64 PCGInstances = 0;
		const UClass* PCGPartitionClass = Spatial::PCGPartitionActorClass();
		int32 SmallCasters = 0;
		int32 MostLocalShadowLights = 0;
		TArray<FString> Stops;

		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor) || Actor->IsEditorOnly()) continue;
			if (!Request.bIncludeHidden && Actor->IsHiddenEd()) continue;
			if (bNamed && !Named.Contains(Actor)) continue;
			if (Out.ActorsScanned >= Request.MaxActors)
			{
				Stops.Add(TEXT("max_actors"));
				break;
			}
			if (Out.ActorsScanned % 128 == 0 && FPlatformTime::Seconds() > Deadline)
			{
				Stops.Add(TEXT("deadline_reached"));
				break;
			}
			++Out.ActorsScanned;
			FActorTally Tally;
			Tally.Actor = Actor;
			Tally.bCounted = !bBox;

			auto CountShadows = [&](const UPrimitiveComponent& Component, const int32 Sections, const bool bNanite)
			{
				if (!Component.CastShadow || !Component.bCastDynamicShadow) return;
				++Out.ShadowCastingPrimitives;
				Tally.bShadow = true;
				SmallCasters += Component.Bounds.SphereRadius < SmallCasterRadius ? 1 : 0;
				// Nanite shadows rasterize with the rest of Nanite, not as draws of their own.
				if (bNanite) return;
				const FBox Bounds = Component.Bounds.GetBox();
				int32 Reaching = 0;
				int32 Local = 0;
				for (const FShadowLight& Light : ShadowLights)
				{
					if (Light.bDirectional || Bounds.ComputeSquaredDistanceToPoint(Light.Center) <= Light.RadiusSquared)
					{
						++Reaching;
						Local += Light.bDirectional ? 0 : 1;
					}
				}
				MostLocalShadowLights = FMath::Max(MostLocalShadowLights, Local);
				Out.EstimatedShadowDrawCalls += Sections * Reaching;
			};
			// Translucent and masked materials, and Nanite's shading bins; returns how many materials it has.
			auto CountMaterials = [&](const UPrimitiveComponent& Component, const bool bNanite, bool& bOutTranslucent)
			{
				TArray<UMaterialInterface*> Used;
				Component.GetUsedMaterials(Used);
				bool bMasked = false;
				bOutTranslucent = false;
				for (const UMaterialInterface* Material : Used)
				{
					if (!Material) continue;
					if (bNanite) NaniteBins.Add(Material);
					if (IsTranslucentBlendMode(*Material))
					{
						++Translucent.FindOrAdd(Material);
						bOutTranslucent = true;
					}
					else if (Material->IsMasked())
					{
						bMasked = true;
					}
				}
				Out.TranslucentPrimitives += bOutTranslucent ? 1 : 0;
				Out.MaskedNanitePrimitives += bNanite && bMasked ? 1 : 0;
				return Used.Num();
			};

			Actor->ForEachComponent<UPrimitiveComponent>(/*bIncludeFromChildActors=*/false, [&](UPrimitiveComponent* Component)
			{
				if (!Component->IsRegistered() || IsHelper(*Component)) return;
				// Hidden actors were let in above when asked for; a hidden component never draws.
				if (Request.bIncludeHidden ? !Component->IsVisible() : !Component->ShouldRender()) return;
				if (bBox && !Component->Bounds.GetBox().Intersect(Box)) return;
				++Out.PrimitiveComponents;
				Tally.bCounted = true;

				if (const ULandscapeComponent* Landscape = Cast<ULandscapeComponent>(Component))
				{
					++Out.LandscapeComponents;
					const int64 Triangles = 2ll * Landscape->ComponentSizeQuads * Landscape->ComponentSizeQuads;
					Out.TrianglesLandscape += Triangles;
					Tally.Triangles += Triangles;
					const ALandscapeProxy* Proxy = Landscape->GetLandscapeProxy();
					const bool bNanite = Proxy && Proxy->IsNaniteEnabled();
					if (bNanite)
					{
						Out.bLandscapeNanite = true;
						++Out.NanitePrimitives;
					}
					else
					{
						// One draw per component is an estimate: sections and LOD batching vary.
						++Out.NonNanitePrimitives;
						++Out.EstimatedDrawCalls;
						++Tally.DrawCalls;
					}
					CountShadows(*Component, 1, bNanite);
					bool bTranslucent = false;
					Tally.Materials += CountMaterials(*Component, bNanite, bTranslucent);
					return;
				}

				if (const UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(Component))
				{
					const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(StaticMesh);
					// A Nanite landscape draws through a static mesh of its own, already counted with its landscape components.
					if (!Instanced && Actor->IsA<ALandscapeProxy>()) return;
					const UStaticMesh* Mesh = StaticMesh->GetStaticMesh();
					const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
					if (!RenderData || RenderData->LODResources.IsEmpty())
					{
						// Only what is already loaded is read; a mesh still compiling or missing has nothing to count.
						AddFinding(Out, FString::Printf(TEXT("no_render_data: %s on %s has no render data yet."),
							Mesh ? *Mesh->GetPathName() : TEXT("no mesh"), *Spatial::LabelOf(*Actor)));
						return;
					}
					int64 Count = 1;
					if (Instanced)
					{
						Count = Instanced->GetInstanceCount();
						if (bBox && !Box.IsInside(Component->Bounds.GetBox())) Count = InstancesInside(*Instanced, Box);
						if (Count == 0) return;
					}
					// Decided as the renderer decides it: the mesh's Nanite data, the component's overrides, the platform and its materials.
					const bool bNanite = Nanite::FNaniteResourcesHelper::ShouldCreateNaniteProxy(*StaticMesh, nullptr);
					const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
					int32 Sections = 0;
					for (const FStaticMeshSection& Section : LOD.Sections)
					{
						Sections += Section.NumTriangles > 0 ? 1 : 0;
					}
					const int64 Triangles = bNanite ? Mesh->GetNumNaniteTriangles() : LOD.GetNumTriangles();

					FMeshTally& MeshTally = Meshes.FindOrAdd(Mesh);
					if (!MeshTally.Mesh)
					{
						MeshTally.Mesh = Mesh;
						MeshTally.Sections = Sections;
						MeshTally.Materials = Mesh->GetStaticMaterials().Num();
					}
					MeshTally.Triangles = FMath::Max(MeshTally.Triangles, Triangles);
					MeshTally.Components += 1;
					MeshTally.Instances += Count;
					MeshTally.bNanite |= bNanite;

					Instances += Count;
					Tally.Instances += Count;
					if (Instanced) ++Out.InstancedComponents;
					else ++Out.StaticMeshComponents;
					if (bNanite)
					{
						++Out.NanitePrimitives;
						Out.TrianglesNaniteSource += Triangles * Count;
						Tally.NaniteTriangles += Triangles * Count;
						Tally.bNanite = true;
					}
					else
					{
						// Instances are batched, so an instanced component draws once per section like a single mesh.
						++Out.NonNanitePrimitives;
						Out.TrianglesNonNanite += Triangles * Count;
						Out.EstimatedDrawCalls += Sections;
						MeshTally.DrawCalls += Sections;
						Tally.Triangles += Triangles * Count;
						Tally.DrawCalls += Sections;
						Tally.Sections += Sections;
						if (Triangles > HeavyTriangles)
						{
							AddFinding(Out, FString::Printf(TEXT("non_nanite_heavy: %s draws %lld triangles at LOD0 without Nanite."), *Mesh->GetPathName(), Triangles));
						}
						if (Instanced && Sections > ManySections)
						{
							AddFinding(Out, FString::Printf(TEXT("many_sections: %s is instanced with %d sections, a draw each per batch; merge its materials."), *Mesh->GetPathName(), Sections));
						}
					}
					const AActor* Owner = Component->GetOwner();
					if (Owner && Owner->IsA<AInstancedFoliageActor>())
					{
						FoliageInstances += Count;
					}
					else if (Spatial::IsPCG(*Component, PCGPartitionClass))
					{
						PCGInstances += Count;
						++Out.PCGComponents;
					}
					CountShadows(*Component, Sections, bNanite);
					MeshTally.bShadow |= Component->CastShadow && Component->bCastDynamicShadow;
					bool bTranslucent = false;
					Tally.Materials += CountMaterials(*Component, bNanite, bTranslucent);
					if (bTranslucent && !bNanite && StaticMesh->HasValidNaniteData())
					{
						AddFinding(Out, FString::Printf(TEXT("translucent_on_nanite: %s has Nanite data, but a translucent material draws it through its fallback mesh."), *Mesh->GetPathName()));
					}
					return;
				}

				if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
				{
					const USkinnedAsset* Asset = Skinned->GetSkinnedAsset();
					const FSkeletalMeshRenderData* RenderData = Asset ? Asset->GetResourceForRendering() : nullptr;
					if (!RenderData || RenderData->LODRenderData.IsEmpty())
					{
						AddFinding(Out, FString::Printf(TEXT("no_render_data: %s on %s has no render data yet."),
							Asset ? *Asset->GetPathName() : TEXT("no mesh"), *Spatial::LabelOf(*Actor)));
						return;
					}
					const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
					const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Asset);
					const bool bNanite = Skeletal && Skeletal->IsNaniteEnabled();
					const int64 Triangles = LOD.GetTotalFaces();
					const int32 Sections = LOD.RenderSections.Num();
					++Out.SkeletalMeshComponents;
					Out.TrianglesSkeletal += Triangles;
					Tally.Triangles += Triangles;
					Instances += 1;
					Tally.Instances += 1;
					if (bNanite)
					{
						++Out.NanitePrimitives;
						Tally.bNanite = true;
					}
					else
					{
						++Out.NonNanitePrimitives;
						Out.EstimatedDrawCalls += Sections;
						Tally.DrawCalls += Sections;
						Tally.Sections += Sections;
					}
					CountShadows(*Component, Sections, bNanite);
					bool bTranslucent = false;
					Tally.Materials += CountMaterials(*Component, bNanite, bTranslucent);
					return;
				}

				// Anything else (text, procedural meshes, geometry collections, water...) is counted as one draw.
				++Out.OtherPrimitives;
				++Out.NonNanitePrimitives;
				++Others.FindOrAdd(Component->GetClass()->GetName());
				++Out.EstimatedDrawCalls;
				++Tally.DrawCalls;
				CountShadows(*Component, 1, false);
				bool bTranslucent = false;
				Tally.Materials += CountMaterials(*Component, false, bTranslucent);
			});
			if (Tally.bCounted)
			{
				++Out.Actors;
				if (Tally.Triangles > 0 || Tally.NaniteTriangles > 0 || Tally.DrawCalls > 0) Actors.Add(Tally);
			}
		}

		Out.Instances = Narrow(Instances);
		Out.FoliageInstances = Narrow(FoliageInstances);
		Out.PCGInstances = Narrow(PCGInstances);
		Out.UniqueMeshes = Meshes.Num();
		Out.NaniteMaterialBins = NaniteBins.Num();
		Out.TranslucentMaterials = Translucent.Num();

		TArray<TPair<const UMaterialInterface*, int32>> TranslucentSorted;
		for (const TPair<const UMaterialInterface*, int32>& Entry : Translucent)
		{
			TranslucentSorted.Add(Entry);
		}
		Algo::Sort(TranslucentSorted, [](const TPair<const UMaterialInterface*, int32>& A, const TPair<const UMaterialInterface*, int32>& B)
		{
			return A.Value != B.Value ? A.Value > B.Value : A.Key->GetPathName() < B.Key->GetPathName();
		});
		for (const TPair<const UMaterialInterface*, int32>& Entry : TranslucentSorted)
		{
			Out.TranslucentList.Add(FString::Printf(TEXT("%s | %d"), *Entry.Key->GetPathName(), Entry.Value));
		}
		TArray<TPair<FString, int32>> OthersSorted;
		for (const TPair<FString, int32>& Entry : Others)
		{
			OthersSorted.Add(Entry);
		}
		Algo::Sort(OthersSorted, [](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
		{
			return A.Value != B.Value ? A.Value > B.Value : A.Key < B.Key;
		});
		for (const TPair<FString, int32>& Entry : OthersSorted)
		{
			Out.OtherClasses.Add(FString::Printf(TEXT("%s x%d"), *Entry.Key, Entry.Value));
		}

		// Heaviest by what is drawn without Nanite, then by draws; meshes by every instance's triangles.
		Algo::Sort(Actors, [](const FActorTally& A, const FActorTally& B)
		{
			if (A.Triangles != B.Triangles) return A.Triangles > B.Triangles;
			if (A.DrawCalls != B.DrawCalls) return A.DrawCalls > B.DrawCalls;
			return A.NaniteTriangles > B.NaniteTriangles;
		});
		for (int32 Index = 0; Index < Actors.Num() && Index < Request.TopN; ++Index)
		{
			Out.HeaviestActors.Add(ActorEntry(Actors[Index]));
		}
		TArray<FMeshTally> MeshList;
		Meshes.GenerateValueArray(MeshList);
		Algo::Sort(MeshList, [](const FMeshTally& A, const FMeshTally& B)
		{
			const int64 TotalA = A.Triangles * A.Instances;
			const int64 TotalB = B.Triangles * B.Instances;
			return TotalA != TotalB ? TotalA > TotalB : A.Mesh->GetPathName() < B.Mesh->GetPathName();
		});
		for (int32 Index = 0; Index < MeshList.Num() && Index < Request.TopN; ++Index)
		{
			Out.HeaviestMeshes.Add(MeshEntry(MeshList[Index]));
		}

		if (UWorldPartition* Partition = World.GetWorldPartition())
		{
			Out.bWorldPartition = true;
			auto Count = [&Out](const FWorldPartitionActorDescInstance* Desc)
			{
				++Out.WPActorDescs;
				Out.WPLoadedActorDescs += Desc->IsLoaded() ? 1 : 0;
				return true;
			};
			if (bBox) FWorldPartitionHelpers::ForEachIntersectingActorDescInstance(Partition, Box, AActor::StaticClass(), Count);
			else FWorldPartitionHelpers::ForEachActorDescInstance(Partition, AActor::StaticClass(), Count);
			if (Out.WPActorDescs > Out.WPLoadedActorDescs)
			{
				AddFinding(Out, FString::Printf(TEXT("unloaded_actors: %d of %d World Partition actors%s are not loaded; these figures cover loaded actors only."),
					Out.WPActorDescs - Out.WPLoadedActorDescs, Out.WPActorDescs, bBox ? TEXT(" in the box") : TEXT("")));
			}
		}
		if (SmallCasters > ManySmallCasters)
		{
			AddFinding(Out, FString::Printf(TEXT("small_shadow_casters: %d shadow casters are under %.0f cm across; turn off Cast Shadow on the small ones."), SmallCasters, SmallCasterRadius * 2.0));
		}
		if (MostLocalShadowLights > ManyShadowLights)
		{
			AddFinding(Out, FString::Printf(TEXT("many_shadow_lights: up to %d movable shadow-casting local lights reach one primitive, each drawing its shadows again."), MostLocalShadowLights));
		}
		for (const FString& Stop : Stops)
		{
			AddFinding(Out, Stop == TEXT("max_actors")
				? FString::Printf(TEXT("max_actors: stopped after %d actors; raise MaxActors or narrow the scope."), Out.ActorsScanned)
				: FString::Printf(TEXT("deadline_reached: stopped after %d ms; raise DeadlineMs or narrow the scope."), Request.DeadlineMs));
		}

		// The last frame a viewport rendered, as stat unit reads it; nothing when no frame was drawn.
		Out.MeasuredLastFrameDrawCalls = GNumDrawCallsRHI[0] > 0 ? GNumDrawCallsRHI[0] : -1;
		Out.MeasuredLastFramePrimitives = GNumPrimitivesDrawnRHI[0] > 0 ? GNumPrimitivesDrawnRHI[0] : -1;

		Out.bOk = true;
		Out.Status = TEXT("completed");
		auto Describe = [&Out, &Stops]()
		{
			Out.bTruncated = !Stops.IsEmpty();
			Out.TruncatedReason = FString::Join(Stops, TEXT(", "));
			Out.Diagnostic = FString::Printf(TEXT("%d actor(s), %d primitive(s): %lld non-Nanite triangles, %lld Nanite source triangles, about %d draw calls and %d shadow draws%s. Draw calls are estimates; MeasuredLastFrameDrawCalls is what the last rendered frame drew."),
				Out.Actors, Out.PrimitiveComponents, Out.TrianglesNonNanite + Out.TrianglesSkeletal + Out.TrianglesLandscape, Out.TrianglesNaniteSource,
				Out.EstimatedDrawCalls, Out.EstimatedShadowDrawCalls, Out.bTruncated ? *FString::Printf(TEXT(" (truncated: %s)"), *Out.TruncatedReason) : TEXT(""));
		};
		Describe();
		// Cut lists from the tail, least telling first, until the answer fits; the first cut is said, so it is measured too.
		while (EstimateBytes(Out) > Request.MaxOutputBytes)
		{
			TArray<FString>* Strings = !Out.TranslucentList.IsEmpty() ? &Out.TranslucentList : !Out.OtherClasses.IsEmpty() ? &Out.OtherClasses : nullptr;
			if (Strings) Strings->Pop();
			else if (!Out.HeaviestMeshes.IsEmpty()) Out.HeaviestMeshes.Pop();
			else if (!Out.HeaviestActors.IsEmpty()) Out.HeaviestActors.Pop();
			else if (!Out.Findings.IsEmpty()) Out.Findings.Pop();
			else break;
			if (!Stops.Contains(TEXT("max_output_bytes")))
			{
				Stops.Add(TEXT("max_output_bytes"));
				Describe();
			}
		}
		Out.ElapsedMs = FMath::CeilToInt32((FPlatformTime::Seconds() - Start) * 1000.0);
	}
}
