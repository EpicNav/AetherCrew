// Games by Hyper 2026.

#include "HyperAIStudioDestructionGate.h"

#include "Chaos/Particle/ObjectState.h"
#include "Components/StaticMeshComponent.h"
#include "Dataflow/DataflowSelection.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FractureEngineClustering.h"
#include "FractureEngineEdit.h"
#include "FractureEngineFracturing.h"
#include "FractureEngineMaterials.h"
#include "FractureEngineUtility.h"
#include "GameFramework/Actor.h"
#include "GeometryCollection/Facades/CollectionAnchoringFacade.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/GeometryCollectionEngineConversion.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionProximityUtility.h"
#include "GeometryCollection/GeometryCollectionRootProxyRenderer.h"
#include "GeometryCollection/GeometryCollectionUtility.h"
#include "GeometryCollectionProxyData.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioResultPreview.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Math/RandomStream.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PlanarCut.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "Voronoi/Voronoi.h"

namespace HyperAIStudio::Destruction::Gate
{
	using C = FHyperAIStudioDestructionContracts;

	namespace
	{
		bool GPreviewsOff = false;

		/** Params every cutter takes: where it cuts, the grout between pieces and noise on the cut faces. */
		TArray<FParamSpec> CutParams()
		{
			return {
				{TEXT("Seed"), EParamType::Int, 0.0, 2147483647.0, 0.0, TEXT("1"), TEXT(""), TEXT("the same seed cuts the same pieces")},
				{TEXT("Level"), EParamType::Int, -1.0, 20.0, 0.0, TEXT("-1"), TEXT(""), TEXT("cut the pieces at this level; -1 cuts every piece that has not broken further")},
				{TEXT("Grout"), EParamType::Float, 0.0, 50.0, 0.0, TEXT("0"), TEXT(""), TEXT("cm of gap carved between the pieces")},
				{TEXT("Noise"), EParamType::Float, 0.0, 100.0, 0.0, TEXT("0"), TEXT(""), TEXT("cm the cut faces are pushed in and out by noise; 0 keeps them flat")},
				{TEXT("NoiseFrequency"), EParamType::Float, 0.0001, 10.0, 0.0, TEXT("0.1"), TEXT(""), TEXT("noise cycles per cm")},
				{TEXT("NoiseOctaves"), EParamType::Int, 1.0, 8.0, 0.0, TEXT("4"), TEXT(""), TEXT("layers of finer noise")},
				{TEXT("NoiseSpacing"), EParamType::Float, 1.0, 100.0, 0.0, TEXT("10"), TEXT(""), TEXT("cm between the vertices of a noisy cut face")},
				{TEXT("SplitIslands"), EParamType::Bool, 0.0, 0.0, 0.0, TEXT("true"), TEXT(""), TEXT("a cell that cuts a piece into parts that do not touch makes a piece of each")},
			};
		}

		TArray<FParamSpec> With(TArray<FParamSpec> Own, const TArray<FParamSpec>& More)
		{
			Own.Append(More);
			return Own;
		}

		TArray<FKindSpec> MakeKinds()
		{
			TArray<FKindSpec> Out;
			Out.Add({TEXT("make"), TEXT("Makes the geometry collection from Sources (static meshes by path, level actors by label); Place says where its actor goes."), {
				{TEXT("SplitIslands"), EParamType::Bool, 0.0, 0.0, 0.0, TEXT("false"), TEXT(""), TEXT("each mesh's parts that do not touch become pieces of their own")},
			}});
			Out.Add({TEXT("fracture.uniform"), TEXT("Voronoi fracture: Sites cells spread evenly through the pieces cut."), With({
				{TEXT("Sites"), EParamType::Int, 1.0, double(C::MaxSites), double(2 * C::MaxSites), TEXT(""), TEXT(""), TEXT("Voronoi cells; each makes a piece where it meets the mesh"), true},
			}, CutParams())});
			Out.Add({TEXT("fracture.clustered"), TEXT("Voronoi fracture with its sites in clumps: fine debris around Clusters impact points, large pieces between."), With({
				{TEXT("Clusters"), EParamType::Int, 1.0, 64.0, 128.0, TEXT("4"), TEXT(""), TEXT("clumps of sites")},
				{TEXT("SitesPerCluster"), EParamType::Int, 1.0, 100.0, 200.0, TEXT("8"), TEXT(""), TEXT("sites in each clump; Clusters x SitesPerCluster is at most 500")},
				{TEXT("ClusterRadius"), EParamType::Float, 0.01, 1.0, 0.0, TEXT("0.2"), TEXT(""), TEXT("how far a clump spreads, as a share of the pieces' size")},
			}, CutParams())});
			Out.Add({TEXT("fracture.radial"), TEXT("Voronoi fracture in rings about a centre (Location, from the middle of the pieces): an impact, a shattered window."), With({
				{TEXT("Normal"), EParamType::Vector, 0.0, 0.0, 0.0, TEXT("0,0,1"), TEXT(""), TEXT("the axis the rings turn about; across a wall, its facing")},
				{TEXT("Radius"), EParamType::Float, 0.0, 100000.0, 0.0, TEXT("0"), TEXT(""), TEXT("cm to the outer ring; 0 reaches the far edge")},
				{TEXT("Rings"), EParamType::Int, 1.0, 20.0, 40.0, TEXT("4"), TEXT(""), TEXT("rings of cells out from the centre")},
				{TEXT("AngularSteps"), EParamType::Int, 2.0, 64.0, 128.0, TEXT("8"), TEXT(""), TEXT("cells around each ring; Rings x AngularSteps is at most 500")},
				{TEXT("AngleOffset"), EParamType::Float, 0.0, 360.0, 0.0, TEXT("0"), TEXT(""), TEXT("degrees the first cell turns")},
				{TEXT("Variability"), EParamType::Float, 0.0, 1000.0, 0.0, TEXT("0"), TEXT(""), TEXT("cm each site wanders, so the rings look broken rather than drawn")},
			}, CutParams())});
			Out.Add({TEXT("fracture.planar"), TEXT("Cuts along planes: Planes you place, Count random ones through the pieces, or both."), With({
				{TEXT("Planes"), EParamType::List, 0.0, 0.0, 0.0, TEXT(""), TEXT(""), TEXT("\"px,py,pz,nx,ny,nz | ...\": a point on each plane and its normal, cm, in the collection's space")},
				{TEXT("Count"), EParamType::Int, 0.0, 64.0, 128.0, TEXT("0"), TEXT(""), TEXT("random planes through the pieces")},
			}, CutParams())});
			Out.Add({TEXT("fracture.slice"), TEXT("A grid of cuts: X, Y and Z cuts across each axis, tilted and shifted at random if asked."), With({
				{TEXT("X"), EParamType::Int, 0.0, 31.0, 0.0, TEXT("2"), TEXT(""), TEXT("cuts across X")},
				{TEXT("Y"), EParamType::Int, 0.0, 31.0, 0.0, TEXT("2"), TEXT(""), TEXT("cuts across Y")},
				{TEXT("Z"), EParamType::Int, 0.0, 31.0, 0.0, TEXT("0"), TEXT(""), TEXT("cuts across Z")},
				{TEXT("AngleVariation"), EParamType::Float, 0.0, 90.0, 0.0, TEXT("0"), TEXT(""), TEXT("degrees each cut may tilt")},
				{TEXT("OffsetVariation"), EParamType::Float, 0.0, 1.0, 0.0, TEXT("0"), TEXT(""), TEXT("how far each cut may shift, as a share of the gap between cuts")},
			}, CutParams())});
			Out.Add({TEXT("fracture.brick"), TEXT("Cuts into bricks laid in a bond; Location shifts the courses."), With({
				{TEXT("Length"), EParamType::Float, 1.0, 10000.0, 0.0, TEXT("40"), TEXT(""), TEXT("cm along a course")},
				{TEXT("Height"), EParamType::Float, 1.0, 10000.0, 0.0, TEXT("20"), TEXT(""), TEXT("cm of a course")},
				{TEXT("Depth"), EParamType::Float, 1.0, 10000.0, 0.0, TEXT("20"), TEXT(""), TEXT("cm through the wall")},
				{TEXT("Bond"), EParamType::Choice, 0.0, 0.0, 0.0, TEXT("stretcher"), TEXT("stretcher,stack,english,header,flemish"), TEXT("how the courses lie")},
			}, CutParams())});
			Out.Add({TEXT("cluster"), TEXT("Groups the pieces under each cluster at Level into Clusters clusters (or clusters about Size cm), adding a level: big chunks break off first, then crumble."), {
				{TEXT("Clusters"), EParamType::Int, 0.0, 256.0, 512.0, TEXT("0"), TEXT(""), TEXT("clusters made under each one at Level; 0 uses Size")},
				{TEXT("Size"), EParamType::Float, 0.0, 100000.0, 0.0, TEXT("0"), TEXT(""), TEXT("cm across a cluster, when Clusters is 0")},
				{TEXT("Level"), EParamType::Int, 0.0, 20.0, 0.0, TEXT("0"), TEXT(""), TEXT("the level whose clusters are grouped; 0 groups the whole object's pieces")},
				{TEXT("Connected"), EParamType::Bool, 0.0, 0.0, 0.0, TEXT("true"), TEXT(""), TEXT("a cluster holds only pieces that touch")},
			}});
			Out.Add({TEXT("cleanup"), TEXT("Pieces smaller than MinSize (the cube root of their volume, cm) are merged into a neighbour, or removed."), {
				{TEXT("MinSize"), EParamType::Float, 0.1, 1000.0, 0.0, TEXT(""), TEXT(""), TEXT("cm"), true},
				{TEXT("Mode"), EParamType::Choice, 0.0, 0.0, 0.0, TEXT("merge"), TEXT("merge,remove"), TEXT("merge into the largest neighbour, or remove")},
			}});
			Out.Add({TEXT("damage"), TEXT("How much strain breaks each level, and whether collisions do damage."), {
				{TEXT("Thresholds"), EParamType::List, 0.0, 0.0, 0.0, TEXT(""), TEXT(""), TEXT("\"500000, 50000, 5000\": the strain that breaks a cluster at each level, the whole object's first; deeper levels use the last")},
				{TEXT("SizeSpecific"), EParamType::Bool, 0.0, 0.0, 0.0, TEXT(""), TEXT(""), TEXT("thresholds by piece size (the collision op's Sizes) instead of by level")},
				{TEXT("Collision"), EParamType::Bool, 0.0, 0.0, 0.0, TEXT(""), TEXT(""), TEXT("collisions damage the placed actors (on by default in Unreal)")},
			}});
			Out.Add({TEXT("collision"), TEXT("The collision shape of each piece, and other shapes for small ones."), {
				{TEXT("Implicit"), EParamType::Choice, 0.0, 0.0, 0.0, TEXT(""), TEXT("convex,box,sphere,capsule,levelset,none"), TEXT("the shape pieces collide as")},
				{TEXT("Type"), EParamType::Choice, 0.0, 0.0, 0.0, TEXT(""), TEXT("volumetric,surface"), TEXT("volumetric: shape against shape; surface: particles on the surface against shapes")},
				{TEXT("Margin"), EParamType::Float, 0.0, 0.5, 0.0, TEXT(""), TEXT(""), TEXT("collision margin as a share of the piece's size")},
				{TEXT("Sizes"), EParamType::List, 0.0, 0.0, 0.0, TEXT(""), TEXT(""), TEXT("\"30:sphere, 100:box\": pieces up to 30 cm collide as spheres, up to 100 cm as boxes, larger ones as Implicit")},
			}});
			Out.Add({TEXT("mass"), TEXT("How heavy the pieces are: a density, a total mass, and the op's Material as their physical material."), {
				{TEXT("Density"), EParamType::Float, 1.0, 100000.0, 0.0, TEXT(""), TEXT(""), TEXT("kg per cubic meter: concrete 2400, wood 600, glass 2500")},
				{TEXT("Mass"), EParamType::Float, 0.01, 100000000.0, 0.0, TEXT(""), TEXT(""), TEXT("kg for the whole, instead of Density")},
			}});
			Out.Add({TEXT("state"), TEXT("How every piece starts in play: dynamic falls at once; kinematic and static hold still, and pieces a Damage field reaches turn dynamic and fall; sleeping waits for a touch. A base that holds while hit pieces fall is anchor's job."), {
				{TEXT("State"), EParamType::Choice, 0.0, 0.0, 0.0, TEXT(""), TEXT("dynamic,kinematic,static,sleeping"), TEXT("the initial state"), true},
			}});
			Out.Add({TEXT("anchor"), TEXT("Pins pieces in place: they never fall, even broken off. Ground anchors those touching the bottom, Box those inside it."), {
				{TEXT("Ground"), EParamType::Bool, 0.0, 0.0, 0.0, TEXT("true"), TEXT(""), TEXT("anchor the pieces within Height of the lowest point")},
				{TEXT("Height"), EParamType::Float, 0.0, 1000.0, 0.0, TEXT("5"), TEXT(""), TEXT("cm above the lowest point a piece's bottom may be")},
				{TEXT("Box"), EParamType::List, 0.0, 0.0, 0.0, TEXT(""), TEXT(""), TEXT("\"x0,y0,z0,x1,y1,z1\": anchor the pieces touching this box, cm in the collection's space, instead of Ground")},
				{TEXT("Clear"), EParamType::Bool, 0.0, 0.0, 0.0, TEXT("false"), TEXT(""), TEXT("remove every anchor first")},
			}});
			Out.Add({TEXT("material.internal"), TEXT("The op's Material on every face fracturing cut inside, the faces the original surface did not have."), {}});
			Out.Add({TEXT("rendering"), TEXT("Nanite, and proxy meshes (Sources, or the original meshes) drawn while the collection is whole."), {
				{TEXT("Nanite"), EParamType::Bool, 0.0, 0.0, 0.0, TEXT(""), TEXT(""), TEXT("render the pieces with Nanite")},
				{TEXT("Renderer"), EParamType::Choice, 0.0, 0.0, 0.0, TEXT(""), TEXT("default,root_proxy"), TEXT("root_proxy draws the proxy meshes until the first break")},
			}});
			return Out;
		}

		FString Lower(const FString& Text)
		{
			return Text.TrimStartAndEnd().ToLower();
		}

		bool ParseBool(const FString& Text, bool& Out)
		{
			const FString Word = Lower(Text);
			if (Word == TEXT("true") || Word == TEXT("1") || Word == TEXT("yes") || Word == TEXT("on")) { Out = true; return true; }
			if (Word == TEXT("false") || Word == TEXT("0") || Word == TEXT("no") || Word == TEXT("off")) { Out = false; return true; }
			return false;
		}

		FString Number(const double Value)
		{
			FString Text = FString::Printf(TEXT("%.3f"), Value);
			while (Text.Contains(TEXT(".")) && (Text.EndsWith(TEXT("0")) || Text.EndsWith(TEXT(".")))) Text.LeftChopInline(1);
			return Text;
		}

		/** The depth of each transform: 0 for a root. */
		TArray<int32> LevelsOf(const FGeometryCollection& Collection)
		{
			const int32 Count = Collection.NumElements(FGeometryCollection::TransformGroup);
			TArray<int32> Levels;
			Levels.Init(INDEX_NONE, Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				int32 Depth = 0;
				for (int32 Parent = Collection.Parent[Index]; Parent != INDEX_NONE && Depth <= Count; Parent = Collection.Parent[Parent])
				{
					++Depth;
				}
				Levels[Index] = Depth;
			}
			return Levels;
		}

		int32 RootOf(const FGeometryCollection& Collection)
		{
			const int32 Count = Collection.NumElements(FGeometryCollection::TransformGroup);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (Collection.Parent[Index] == INDEX_NONE) return Index;
			}
			return INDEX_NONE;
		}

		TArray<FTransform> GlobalsOf(const FGeometryCollection& Collection)
		{
			TArray<FTransform> Globals;
			GeometryCollectionAlgo::GlobalMatrices(Collection.Transform, Collection.Parent, Globals);
			return Globals;
		}

		/** The leaves under each transform of Selected (a leaf is its own), as the cutters cut them. */
		TArray<int32> LeavesUnder(const FGeometryCollection& Collection, const TArray<int32>& Selected)
		{
			TArray<int32> Out;
			TArray<int32> Stack = Selected;
			while (!Stack.IsEmpty())
			{
				const int32 Index = Stack.Pop(EAllowShrinking::No);
				if (!Collection.Children.IsValidIndex(Index)) continue;
				if (Collection.Children[Index].IsEmpty())
				{
					if (Collection.TransformToGeometryIndex[Index] != INDEX_NONE) Out.AddUnique(Index);
					continue;
				}
				for (const int32 Child : Collection.Children[Index])
				{
					Stack.Add(Child);
				}
			}
			Out.Sort();
			return Out;
		}

		/** The bounds of the pieces at Indices, in the collection's space. */
		FBox BoundsOf(const FGeometryCollection& Collection, const TArray<FTransform>& Globals, const TArray<int32>& Indices)
		{
			FBox Bounds(ForceInit);
			for (const int32 Index : Indices)
			{
				const int32 Geometry = Collection.TransformToGeometryIndex[Index];
				if (Geometry != INDEX_NONE && Collection.BoundingBox.IsValidIndex(Geometry))
				{
					Bounds += Collection.BoundingBox[Geometry].TransformBy(Globals[Index]);
				}
			}
			return Bounds;
		}

		/** The volume a geometry's faces enclose, cm3, in its own space. */
		double VolumeOf(const FGeometryCollection& Collection, const int32 Geometry)
		{
			double Volume = 0.0;
			const int32 First = Collection.FaceStart[Geometry];
			const int32 Last = First + Collection.FaceCount[Geometry];
			for (int32 Face = First; Face < Last && Face < Collection.Indices.Num(); ++Face)
			{
				const FIntVector& Tri = Collection.Indices[Face];
				const FVector A(Collection.Vertex[Tri.X]);
				const FVector B(Collection.Vertex[Tri.Y]);
				const FVector D(Collection.Vertex[Tri.Z]);
				Volume += FVector::DotProduct(A, FVector::CrossProduct(B, D)) / 6.0;
			}
			return FMath::Abs(Volume);
		}

		float SizeOf(const double Volume)
		{
			return static_cast<float>(FMath::Pow(FMath::Max(Volume, 0.0), 1.0 / 3.0));
		}

		/** The transforms the op cuts: the root for Level -1 (every leaf under it), else every transform at Level. */
		TArray<int32> Targets(const FGeometryCollection& Collection, const int32 Level)
		{
			TArray<int32> Out;
			const TArray<int32> Levels = LevelsOf(Collection);
			for (int32 Index = 0; Index < Levels.Num(); ++Index)
			{
				if ((Level < 0 && Levels[Index] == 0) || Levels[Index] == Level) Out.Add(Index);
			}
			return Out;
		}

		struct FCut
		{
			int32 Seed = 1;
			float Grout = 0.f;
			float Noise = 0.f;
			float Frequency = 0.1f;
			int32 Octaves = 4;
			float Spacing = 10.f;
			bool bSplitIslands = true;
		};

		FCut CutOf(const FParams& Params)
		{
			FCut Cut;
			Cut.Seed = Params.Int(TEXT("Seed"));
			Cut.Grout = static_cast<float>(Params.Number(TEXT("Grout")));
			Cut.Noise = static_cast<float>(Params.Number(TEXT("Noise")));
			Cut.Frequency = static_cast<float>(Params.Number(TEXT("NoiseFrequency")));
			Cut.Octaves = Params.Int(TEXT("NoiseOctaves"));
			Cut.Spacing = static_cast<float>(Params.Number(TEXT("NoiseSpacing")));
			Cut.bSplitIslands = Params.Bool(TEXT("SplitIslands"));
			return Cut;
		}

		FDataflowTransformSelection SelectionOf(const FManagedArrayCollection& Working, const TArray<int32>& Indices)
		{
			FDataflowTransformSelection Selection;
			Selection.InitializeFromCollection(Working, false);
			Selection.SetFromArray(Indices);
			return Selection;
		}

		/**
		 * A Voronoi cut of the leaves under Selected. With noise it is the Dataflow node's own call,
		 * FFractureEngineFracturing::VoronoiFracture; without, the call under it, PlanarCut's CutMultipleWithPlanarCells
		 * with flat cells (VoronoiFracture always meshes its cells for noise, which is slower and denser for flat cuts).
		 */
		int32 CutVoronoi(FManagedArrayCollection& Working, const TArray<int32>& Selected, TArray<FVector> Sites, const FCut& Cut)
		{
			if (Sites.IsEmpty()) return INDEX_NONE;
			if (Cut.Noise > 0.f)
			{
				return FFractureEngineFracturing::VoronoiFracture(Working, SelectionOf(Working, Selected), MoveTemp(Sites), FTransform::Identity, Cut.Seed, 1.f,
					FIslandSplitSettings(Cut.bSplitIslands), Cut.Grout, Cut.Noise, Cut.Frequency, 0.5f, 2.f, Cut.Octaves, Cut.Spacing, false, 0.f);
			}
			TUniquePtr<FGeometryCollection> Collection(Working.NewCopy<FGeometryCollection>());
			const TArray<int32> Leaves = LeavesUnder(*Collection, Selected);
			if (Leaves.IsEmpty()) return INDEX_NONE;
			FBox Bounds = BoundsOf(*Collection, GlobalsOf(*Collection), Leaves);
			Bounds += FBox(Sites);
			Bounds = Bounds.ExpandBy(Cut.Grout + UE_KINDA_SMALL_NUMBER);
			FVoronoiDiagram Voronoi(Sites, Bounds, .1f);
			FPlanarCells Cells(Sites, Voronoi);
			const int32 First = CutMultipleWithPlanarCells(Cells, *Collection, Leaves, Cut.Grout, 0.0, Cut.Seed, FTransform::Identity, true, true, nullptr,
				FVector::ZeroVector, FIslandSplitSettings(Cut.bSplitIslands));
			if (First != INDEX_NONE)
			{
				GeometryCollection::GenerateTemporaryGuids(Collection.Get(), First, true);
			}
			Working = static_cast<const FManagedArrayCollection&>(*Collection);
			return First;
		}

		int32 CountLeaves(const FManagedArrayCollection& Working)
		{
			TUniquePtr<FGeometryCollection> Collection(Working.NewCopy<FGeometryCollection>());
			int32 Leaves = 0;
			for (int32 Index = 0; Index < Collection->Children.Num(); ++Index)
			{
				if (Collection->Children[Index].IsEmpty() && Collection->TransformToGeometryIndex[Index] != INDEX_NONE) ++Leaves;
			}
			return Leaves;
		}

		/** Each transform with geometry. */
		TArray<int32> GeometryTransforms(const FGeometryCollection& Collection)
		{
			TArray<int32> Out;
			for (int32 Index = 0; Index < Collection.TransformToGeometryIndex.Num(); ++Index)
			{
				if (Collection.TransformToGeometryIndex[Index] != INDEX_NONE) Out.Add(Index);
			}
			return Out;
		}

		EImplicitTypeEnum ImplicitOf(const FString& Word)
		{
			if (Word == TEXT("box")) return EImplicitTypeEnum::Chaos_Implicit_Box;
			if (Word == TEXT("sphere")) return EImplicitTypeEnum::Chaos_Implicit_Sphere;
			if (Word == TEXT("capsule")) return EImplicitTypeEnum::Chaos_Implicit_Capsule;
			if (Word == TEXT("levelset")) return EImplicitTypeEnum::Chaos_Implicit_LevelSet;
			if (Word == TEXT("none")) return EImplicitTypeEnum::Chaos_Implicit_None;
			return EImplicitTypeEnum::Chaos_Implicit_Convex;
		}

		Chaos::EObjectStateType StateOf(const FString& Word)
		{
			if (Word == TEXT("kinematic")) return Chaos::EObjectStateType::Kinematic;
			if (Word == TEXT("static")) return Chaos::EObjectStateType::Static;
			if (Word == TEXT("sleeping")) return Chaos::EObjectStateType::Sleeping;
			return Chaos::EObjectStateType::Dynamic;
		}

		FString StateName(const int32 State)
		{
			switch (static_cast<Chaos::EObjectStateType>(State))
			{
			case Chaos::EObjectStateType::Sleeping: return TEXT("sleeping");
			case Chaos::EObjectStateType::Kinematic: return TEXT("kinematic");
			case Chaos::EObjectStateType::Static: return TEXT("static");
			case Chaos::EObjectStateType::Dynamic: return TEXT("dynamic");
			default: return TEXT("default");
			}
		}

		EFractureBrickBondEnum BondOf(const FString& Word)
		{
			if (Word == TEXT("stack")) return EFractureBrickBondEnum::Dataflow_FractureBrickBond_Stack;
			if (Word == TEXT("english")) return EFractureBrickBondEnum::Dataflow_FractureBrickBond_English;
			if (Word == TEXT("header")) return EFractureBrickBondEnum::Dataflow_FractureBrickBond_Header;
			if (Word == TEXT("flemish")) return EFractureBrickBondEnum::Dataflow_FractureBrickBond_Flemish;
			return EFractureBrickBondEnum::Dataflow_FractureBrickBond_Stretcher;
		}

		template <typename T>
		T* LoadQuiet(const FString& Reference, FString& OutReason)
		{
			const FString Trimmed = Reference.TrimStartAndEnd();
			if (!FHyperAIStudioNames::IsObjectPath(Trimmed, TEXT("Path"), &OutReason)) return nullptr;
			const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
			if (T* Found = FindObject<T>(nullptr, *ObjectPath)) return Found;
			// A path with nothing on disk is never loaded: the failed load would leave an empty package behind.
			if (!FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ObjectPath))) return nullptr;
			return LoadObject<T>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		UWorld* EditorWorld()
		{
			return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		}

		AActor* FindActor(UWorld& World, const FString& Label)
		{
			const FString Wanted = Label.TrimStartAndEnd();
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				if (IsValid(*It) && It->GetActorLabel().Equals(Wanted, ESearchCase::IgnoreCase)) return *It;
			}
			return nullptr;
		}

		FString PackageOf(const FString& Reference)
		{
			FString Path = Reference.TrimStartAndEnd();
			int32 Dot = INDEX_NONE;
			if (Path.FindLastChar(TEXT('.'), Dot)) Path.LeftInline(Dot);
			return Path;
		}
	}

	const TArray<FKindSpec>& Kinds()
	{
		static const TArray<FKindSpec> All = MakeKinds();
		return All;
	}

	const FKindSpec* FindKind(const FString& Kind)
	{
		const FString Wanted = Lower(Kind);
		return Kinds().FindByPredicate([&Wanted](const FKindSpec& Spec) { return Wanted == Spec.Kind; });
	}

	TArray<FString> DescribeParams(const FKindSpec& Spec)
	{
		TArray<FString> Out;
		for (const FParamSpec& Param : Spec.Params)
		{
			FString Type;
			switch (Param.Type)
			{
			case EParamType::Int: Type = FString::Printf(TEXT("integer %s to %s"), *Number(Param.Min), *Number(Param.Max)); break;
			case EParamType::Float: Type = FString::Printf(TEXT("number %s to %s"), *Number(Param.Min), *Number(Param.Max)); break;
			case EParamType::Bool: Type = TEXT("true or false"); break;
			case EParamType::Choice: Type = FString(Param.Choices).Replace(TEXT(","), TEXT(" | ")); break;
			case EParamType::Vector: Type = TEXT("x,y,z"); break;
			case EParamType::List: Type = TEXT("list"); break;
			}
			const FString Default = Param.bRequired ? FString(TEXT("required")) : FString(Param.Default).IsEmpty() ? FString(TEXT("unchanged when not given"))
				: FString::Printf(TEXT("default %s"), Param.Default);
			Out.Add(FString::Printf(TEXT("%s: %s, %s - %s"), Param.Name, *Type, *Default, Param.Help));
		}
		return Out;
	}

	bool FParams::Has(const TCHAR* Name) const
	{
		return Values.Contains(Name);
	}

	FString FParams::Text(const TCHAR* Name) const
	{
		const FString* Found = Values.Find(Name);
		return Found ? *Found : FString();
	}

	double FParams::Number(const TCHAR* Name) const
	{
		double Value = 0.0;
		LexTryParseString(Value, *Text(Name));
		return Value;
	}

	int32 FParams::Int(const TCHAR* Name) const
	{
		return static_cast<int32>(FMath::Clamp(FMath::RoundToDouble(Number(Name)), -2147483648.0, 2147483647.0));
	}

	bool FParams::Bool(const TCHAR* Name) const
	{
		bool Value = false;
		ParseBool(Text(Name), Value);
		return Value;
	}

	FVector FParams::Vector(const TCHAR* Name) const
	{
		FVector Value = FVector::ZeroVector;
		ParseVector(Text(Name), Value);
		return Value;
	}

	bool ParseVector(const FString& Text, FVector& Out)
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty=*/false);
		if (Parts.Num() != 3) return false;
		double Values[3];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (!LexTryParseString(Values[Axis], *Parts[Axis].TrimStartAndEnd()) || !FMath::IsFinite(Values[Axis]) || FMath::Abs(Values[Axis]) > 1.0e7) return false;
		}
		Out = FVector(Values[0], Values[1], Values[2]);
		return true;
	}

	bool ParseParams(const FKindSpec& Spec, const FString& Text, FParams& Out, TArray<FString>& OutFitted, FString& OutError)
	{
		Out.Values.Reset();
		TArray<FString> Pairs;
		Text.ParseIntoArray(Pairs, TEXT(";"), /*InCullEmpty=*/true);
		auto Known = [&Spec]()
		{
			TArray<FString> Names;
			for (const FParamSpec& Param : Spec.Params) Names.Add(Param.Name);
			return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT(", "));
		};
		for (const FString& Raw : Pairs)
		{
			const FString Pair = Raw.TrimStartAndEnd();
			if (Pair.IsEmpty()) continue;
			FString Name;
			FString Value;
			if (!Pair.Split(TEXT("="), &Name, &Value))
			{
				OutError = FString::Printf(TEXT("'%s' is not Name=Value; %s takes %s."), *Pair, Spec.Kind, *Known());
				return false;
			}
			Name.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			const FParamSpec* Param = Spec.Params.FindByPredicate([&Name](const FParamSpec& Each) { return Name.Equals(Each.Name, ESearchCase::IgnoreCase); });
			if (!Param)
			{
				OutError = FString::Printf(TEXT("%s takes no param '%s'; it takes %s."), Spec.Kind, *Name, *Known());
				return false;
			}
			if (Out.Values.Contains(Param->Name))
			{
				OutError = FString::Printf(TEXT("%s is given twice."), Param->Name);
				return false;
			}
			const FString Takes = DescribeParams({Spec.Kind, Spec.Summary, {*Param}})[0];
			switch (Param->Type)
			{
			case EParamType::Int:
			case EParamType::Float:
			{
				double Parsed = 0.0;
				if (!LexTryParseString(Parsed, *Value) || !FMath::IsFinite(Parsed) || (Param->Type == EParamType::Int && Parsed != FMath::RoundToDouble(Parsed)))
				{
					OutError = FString::Printf(TEXT("%s=%s is not a number; %s."), Param->Name, *Value, *Takes);
					return false;
				}
				if (Parsed < Param->Min || (Parsed > Param->Max && !(Param->FitMax > 0.0 && Parsed <= Param->FitMax)))
				{
					OutError = Param->FitMax > 0.0 && Parsed > Param->FitMax
						? FString::Printf(TEXT("%s=%s is far past the most, %s; %s."), Param->Name, *Value, *Number(Param->Max), *Takes)
						: FString::Printf(TEXT("%s=%s is out of range; %s."), Param->Name, *Value, *Takes);
					return false;
				}
				if (Parsed > Param->Max)
				{
					OutFitted.Add(FString::Printf(TEXT("%s %s -> %s"), Param->Name, *Value, *Number(Param->Max)));
					Parsed = Param->Max;
				}
				Value = Number(Parsed);
				break;
			}
			case EParamType::Bool:
			{
				bool Flag = false;
				if (!ParseBool(Value, Flag))
				{
					OutError = FString::Printf(TEXT("%s=%s is not true or false."), Param->Name, *Value);
					return false;
				}
				Value = Flag ? TEXT("true") : TEXT("false");
				break;
			}
			case EParamType::Choice:
			{
				TArray<FString> Choices;
				FString(Param->Choices).ParseIntoArray(Choices, TEXT(","));
				const FString Word = Lower(Value);
				if (!Choices.Contains(Word))
				{
					OutError = FString::Printf(TEXT("%s=%s is not one of %s."), Param->Name, *Value, *FString(Param->Choices).Replace(TEXT(","), TEXT(", ")));
					return false;
				}
				Value = Word;
				break;
			}
			case EParamType::Vector:
			{
				FVector Vector;
				if (!ParseVector(Value, Vector))
				{
					OutError = FString::Printf(TEXT("%s=%s is not \"x,y,z\"."), Param->Name, *Value);
					return false;
				}
				break;
			}
			case EParamType::List:
				break;
			}
			Out.Values.Add(Param->Name, Value);
		}
		for (const FParamSpec& Param : Spec.Params)
		{
			if (Out.Values.Contains(Param.Name)) continue;
			if (Param.bRequired)
			{
				OutError = FString::Printf(TEXT("%s needs %s; %s."), Spec.Kind, Param.Name, *DescribeParams({Spec.Kind, Spec.Summary, {Param}})[0]);
				return false;
			}
			if (FCString::Strlen(Param.Default) > 0) Out.Values.Add(Param.Name, Param.Default);
		}
		return true;
	}

	bool ParseThresholds(const FString& Text, TArray<float>& Out, FString& OutError)
	{
		Out.Reset();
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty=*/true);
		for (const FString& Part : Parts)
		{
			double Value = 0.0;
			if (!LexTryParseString(Value, *Part.TrimStartAndEnd()) || !FMath::IsFinite(Value) || Value < 0.0 || Value > 1.0e12)
			{
				OutError = FString::Printf(TEXT("Thresholds: '%s' is not a strain of 0 or more."), *Part.TrimStartAndEnd());
				return false;
			}
			Out.Add(static_cast<float>(Value));
		}
		if (Out.IsEmpty() || Out.Num() > 20)
		{
			OutError = TEXT("Thresholds lists 1 to 20 strains, the whole object's first: \"500000, 50000, 5000\".");
			return false;
		}
		return true;
	}

	bool ParsePlanes(const FString& Text, TArray<FTransform>& Out, FString& OutError)
	{
		Out.Reset();
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT("|"), /*InCullEmpty=*/true);
		for (const FString& Part : Parts)
		{
			TArray<FString> Numbers;
			Part.ParseIntoArray(Numbers, TEXT(","), /*InCullEmpty=*/false);
			double Values[6];
			bool bOk = Numbers.Num() == 6;
			for (int32 Index = 0; bOk && Index < 6; ++Index)
			{
				bOk = LexTryParseString(Values[Index], *Numbers[Index].TrimStartAndEnd()) && FMath::IsFinite(Values[Index]) && FMath::Abs(Values[Index]) < 1.0e7;
			}
			const FVector Normal = bOk ? FVector(Values[3], Values[4], Values[5]) : FVector::ZeroVector;
			if (!bOk || Normal.IsNearlyZero())
			{
				OutError = FString::Printf(TEXT("Planes: '%s' is not \"px,py,pz,nx,ny,nz\" with a normal that is not zero."), *Part.TrimStartAndEnd());
				return false;
			}
			Out.Add(FTransform(FRotationMatrix::MakeFromZ(Normal.GetSafeNormal()).ToQuat(), FVector(Values[0], Values[1], Values[2])));
		}
		if (Out.Num() > 64)
		{
			OutError = TEXT("Planes lists at most 64 planes.");
			return false;
		}
		return true;
	}

	bool ParseSizes(const FString& Text, TArray<TPair<float, FString>>& Out, FString& OutError)
	{
		Out.Reset();
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty=*/true);
		static const TArray<FString> Shapes = {TEXT("convex"), TEXT("box"), TEXT("sphere"), TEXT("capsule"), TEXT("levelset"), TEXT("none")};
		for (const FString& Part : Parts)
		{
			FString Size;
			FString Shape;
			double Value = 0.0;
			if (!Part.Split(TEXT(":"), &Size, &Shape) || !LexTryParseString(Value, *Size.TrimStartAndEnd()) || !(Value > 0.0 && Value < 1.0e6)
				|| !Shapes.Contains(Lower(Shape)))
			{
				OutError = FString::Printf(TEXT("Sizes: '%s' is not \"<cm>:<shape>\" with a shape of convex, box, sphere, capsule, levelset or none."), *Part.TrimStartAndEnd());
				return false;
			}
			Out.Emplace(static_cast<float>(Value), Lower(Shape));
		}
		if (Out.Num() > 8)
		{
			OutError = TEXT("Sizes lists at most 8 sizes.");
			return false;
		}
		Out.Sort([](const TPair<float, FString>& A, const TPair<float, FString>& B) { return A.Key < B.Key; });
		return true;
	}

	TArray<FVector> UniformSites(const FBox& Bounds, const int32 Count, const int32 Seed)
	{
		// As Epic's GenerateVoronoiSites draws them, with the count fixed rather than drawn from a range.
		FRandomStream Stream(Seed);
		const FVector Extent = Bounds.Max - Bounds.Min;
		TArray<FVector> Sites;
		Sites.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double X = Stream.FRand();
			const double Y = Stream.FRand();
			const double Z = Stream.FRand();
			Sites.Emplace(Bounds.Min + FVector(X, Y, Z) * Extent);
		}
		return Sites;
	}

	TArray<FVector> ClusteredSites(const FBox& Bounds, const int32 Clusters, const int32 PerCluster, const double RadiusFraction, const int32 Seed)
	{
		// As FClusterScatterPointsDataflowNode scatters them, with fixed counts.
		FRandomStream Stream(Seed);
		const FVector Extent = Bounds.Max - Bounds.Min;
		const double Reach = Bounds.GetExtent().GetAbsMax() * RadiusFraction;
		TArray<FVector> Sites;
		Sites.Reserve(Clusters * PerCluster);
		for (int32 Cluster = 0; Cluster < Clusters; ++Cluster)
		{
			const double X = Stream.FRand();
			const double Y = Stream.FRand();
			const double Z = Stream.FRand();
			const FVector Centre = Bounds.Min + FVector(X, Y, Z) * Extent;
			for (int32 Index = 0; Index < PerCluster; ++Index)
			{
				const FVector Direction = Stream.VRand().GetSafeNormal();
				Sites.Add(Centre + Direction * (Stream.FRand() * Reach));
			}
		}
		return Sites;
	}

	TArray<FVector> RadialSites(const FVector& Center, const FVector& Normal, const double Radius, const int32 Rings, const int32 AngularSteps, const double AngleOffsetDegrees,
		const double Variability, const int32 Seed)
	{
		// FRadialScatterPointsDataflowNode_v2 with an exponent of 1: the first ring half a step out, the rest a step apart.
		FRandomStream Stream(Seed);
		FVector Up = Normal.GetSafeNormal();
		if (Up.IsNearlyZero()) Up = FVector::UpVector;
		FVector BasisX;
		FVector BasisY;
		Up.FindBestAxisVectors(BasisX, BasisY);
		TArray<FVector> Sites;
		if (Rings < 1 || AngularSteps < 1 || !(Radius > 0.0)) return Sites;
		Sites.Reserve(Rings * AngularSteps);
		const double Step = Radius / (Rings - 0.5);
		const double AngularStep = 2.0 * UE_DOUBLE_PI / AngularSteps;
		// Voro++ fails on perfectly regular rings; Epic adds the same hair of noise.
		const double Wander = FMath::Max(Variability, Radius > 1.0 ? 0.0001 : 0.0);
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			const double Distance = (Ring + 0.5) * Step;
			double Angle = FMath::DegreesToRadians(AngleOffsetDegrees);
			for (int32 Index = 0; Index < AngularSteps; ++Index, Angle += AngularStep)
			{
				const double UseRadius = Distance + Stream.FRandRange(-1.0, 1.0) * Wander;
				const double UseAngle = Angle + (Distance > 0.0 ? Stream.FRandRange(-1.0, 1.0) * Wander / Distance : 0.0);
				Sites.Add(Center + UseRadius * (FMath::Cos(UseAngle) * BasisX + FMath::Sin(UseAngle) * BasisY));
			}
		}
		return Sites;
	}

	FString FStep::Describe() const
	{
		return FString::Printf(TEXT("Operation %d (%s)"), Index, *Kind);
	}

	UGeometryCollection* UndoneAt(const FString& PackagePath)
	{
		if (FPackageName::DoesPackageExist(PackagePath)) return nullptr;
		UPackage* Package = FindPackage(nullptr, *PackagePath);
		UObject* There = Package ? StaticFindObjectFast(UObject::StaticClass(), Package, FName(*FPackageName::GetShortName(PackagePath)), EFindObjectFlags::None,
			RF_NoFlags, EInternalObjectFlags::None) : nullptr;
		UGeometryCollection* Collection = Cast<UGeometryCollection>(There);
		return Collection && !IsValid(Collection) ? Collection : nullptr;
	}

	FString VectorText(const FVector& Value)
	{
		return FString::Printf(TEXT("%s,%s,%s"), *Number(Value.X), *Number(Value.Y), *Number(Value.Z));
	}

	bool Check(const FHyperAIDestructionPlanRequest& Request, FPlan& OutPlan, FString& OutError)
	{
		OutPlan = FPlan();
		const FString Path = PackageOf(Request.Asset);
		if (Path.IsEmpty())
		{
			OutError = TEXT("Give Asset, the geometry collection to make or change, as /Game/Folder/GC_Name.");
			return false;
		}
		FString Reason;
		if (!Path.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(TEXT("%s is not project content: a plan makes and changes geometry collections under /Game only, never Engine or plugin content."), *Path);
			return false;
		}
		if (!FHyperAIStudioNames::IsPackagePath(Path, TEXT("Asset"), &Reason))
		{
			OutError = Reason;
			return false;
		}
		OutPlan.AssetPath = Path;
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
			return false;
		}
		for (int32 Index = 1; Index < Request.Operations.Num(); ++Index)
		{
			if (Lower(Request.Operations[Index].Kind) == TEXT("make"))
			{
				OutError = FString::Printf(TEXT("Operation %d (make): make comes first, once: the ops after it work on what it made. Nothing was changed."), Index);
				return false;
			}
		}
		UWorld* World = EditorWorld();
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAIDestructionOp& Op = Request.Operations[Index];
			FStep& Step = OutPlan.Steps.AddDefaulted_GetRef();
			Step.Index = Index;
			Step.Op = Op;
			Step.Kind = Lower(Op.Kind);
			auto Fail = [&OutError, &Step](const FString& Message)
			{
				OutError = FString::Printf(TEXT("%s: %s Nothing was changed."), *Step.Describe(), *Message);
				return false;
			};
			const FKindSpec* Spec = FindKind(Step.Kind);
			if (!Spec)
			{
				TArray<FString> Names;
				for (const FKindSpec& Each : Kinds()) Names.Add(Each.Kind);
				return Fail(FString::Printf(TEXT("'%s' is not an op kind; the kinds are %s."), *Op.Kind, *FString::Join(Names, TEXT(", "))));
			}
			TArray<FString> Fitted;
			FString Why;
			if (!ParseParams(*Spec, Op.Params, Step.Params, Fitted, Why)) return Fail(Why);
			for (const FString& Fit : Fitted) OutPlan.Fitted.Add(FString::Printf(TEXT("%s: %s"), *Step.Describe(), *Fit));
			if (!Op.Location.TrimStartAndEnd().IsEmpty() && !ParseVector(Op.Location, Step.Location)) return Fail(FString::Printf(TEXT("Location '%s' is not \"x,y,z\"."), *Op.Location));
			const bool bSources = !Op.Sources.IsEmpty();
			const bool bMaterial = !Op.Material.TrimStartAndEnd().IsEmpty();
			if (bSources && Step.Kind != TEXT("make") && Step.Kind != TEXT("rendering")) return Fail(TEXT("Only make and rendering take Sources."));
			if (bMaterial && Step.Kind != TEXT("material.internal") && Step.Kind != TEXT("mass")) return Fail(TEXT("Only material.internal and mass take a Material."));
			if (!Op.Place.TrimStartAndEnd().IsEmpty() && Step.Kind != TEXT("make")) return Fail(TEXT("Only make takes Place."));

			if (Step.Kind == TEXT("make"))
			{
				if (Index != 0) return Fail(TEXT("make comes first, once: the ops after it work on what it made."));
				if (Op.Sources.IsEmpty() || Op.Sources.Num() > C::MaxSources) return Fail(FString::Printf(TEXT("Give 1 to %d Sources: static meshes by path or level actors by label."), C::MaxSources));
				// Free in memory too: an unsaved asset of another class there would stop the editor when the collection is made.
				// A collection an undone plan made is still there, as garbage: the plan may make it again.
				if (!FHyperAIStudioNames::IsFreePackage(Path) && !UndoneAt(Path))
				{
					return Fail(FString::Printf(TEXT("%s exists already; leave out make to change it, or name a new Asset."), *Path));
				}
				OutPlan.bMake = true;
				AActor* Pivot = nullptr;
				for (const FString& Source : Op.Sources)
				{
					const FString Trimmed = Source.TrimStartAndEnd();
					if (Trimmed.StartsWith(TEXT("/")))
					{
						UStaticMesh* Mesh = LoadQuiet<UStaticMesh>(Trimmed, Why);
						if (!Mesh) return Fail(Why.IsEmpty() ? FString::Printf(TEXT("%s is not a static mesh."), *Trimmed) : Why);
						FSource& Out = OutPlan.Sources.AddDefaulted_GetRef();
						Out.Mesh = Mesh;
						Out.Name = Mesh->GetName();
						for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials()) Out.Materials.Add(Slot.MaterialInterface);
						continue;
					}
					if (!FHyperAIStudioNames::IsName(Trimmed, TEXT("Sources"), &Why)) return Fail(Why);
					AActor* Actor = World ? FindActor(*World, Trimmed) : nullptr;
					if (!Actor) return Fail(FString::Printf(TEXT("No actor in the open level is labelled %s, and it is not a /Game path."), *Trimmed));
					if (OutPlan.SourceActors.ContainsByPredicate([Actor](const TWeakObjectPtr<AActor>& Each) { return Each.Get() == Actor; }))
					{
						return Fail(FString::Printf(TEXT("%s is named twice."), *Trimmed));
					}
					TArray<UStaticMeshComponent*> Components;
					Actor->GetComponents(Components);
					Components.RemoveAll([](const UStaticMeshComponent* Component) { return !Component || !Component->GetStaticMesh(); });
					if (Components.IsEmpty()) return Fail(FString::Printf(TEXT("%s has no static mesh to fracture."), *Trimmed));
					if (!Pivot) Pivot = Actor;
					OutPlan.SourceActors.Add(Actor);
					for (UStaticMeshComponent* Component : Components)
					{
						FSource& Out = OutPlan.Sources.AddDefaulted_GetRef();
						Out.Mesh = Component->GetStaticMesh();
						Out.Name = Actor->GetActorLabel();
						for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot) Out.Materials.Add(Component->GetMaterial(Slot));
						// As Fracture Mode's New does it: about the first actor's location, rotation and scale kept in the pieces.
						Out.Transform = Component->GetComponentTransform();
						Out.Transform.SetTranslation(Out.Transform.GetTranslation() - Pivot->GetActorLocation());
					}
				}
				for (const FSource& Source : OutPlan.Sources) OutPlan.SourceTriangles += Source.Mesh->GetNumTriangles(0);
				if (OutPlan.SourceTriangles > C::MaxSourceTriangles)
				{
					return Fail(FString::Printf(TEXT("the sources hold %d triangles and the most is %d; fracture a lighter mesh."), OutPlan.SourceTriangles, C::MaxSourceTriangles));
				}
				const FString Place = Lower(Op.Place);
				OutPlan.Place = !Place.IsEmpty() ? Place : Pivot ? FString(TEXT("replace")) : FString(TEXT("none"));
				if (OutPlan.Place != TEXT("replace") && OutPlan.Place != TEXT("beside") && OutPlan.Place != TEXT("at") && OutPlan.Place != TEXT("none"))
				{
					return Fail(TEXT("Place is replace, beside, at or none."));
				}
				if ((OutPlan.Place == TEXT("replace") || OutPlan.Place == TEXT("beside")) && !Pivot)
				{
					return Fail(FString::Printf(TEXT("Place=%s needs level actors among Sources; with meshes only, use at (with Location) or none."), *OutPlan.Place));
				}
				if (OutPlan.Place == TEXT("at") && Op.Location.TrimStartAndEnd().IsEmpty()) return Fail(TEXT("Place=at needs Location, \"x,y,z\" in the level."));
				if (OutPlan.Place != TEXT("none") && !World) return Fail(TEXT("No level is open in the editor to place the collection in."));
				if (OutPlan.Place == TEXT("at")) OutPlan.PlaceTransform = FTransform(Step.Location);
				else if (Pivot)
				{
					FBox Bounds(ForceInit);
					for (const TWeakObjectPtr<AActor>& Actor : OutPlan.SourceActors) Bounds += Actor->GetComponentsBoundingBox(/*bNonColliding=*/true);
					OutPlan.PlaceTransform = FTransform(Pivot->GetActorLocation());
					if (OutPlan.Place == TEXT("beside"))
					{
						// Beside them along Y, a meter clear of their bounds.
						OutPlan.PlaceTransform.AddToTranslation(FVector(0.0, Bounds.GetSize().Y + 100.0, 0.0));
					}
				}
				continue;
			}
			if (!OutPlan.bMake && !OutPlan.Existing)
			{
				UGeometryCollection* Existing = LoadQuiet<UGeometryCollection>(Path, Why);
				if (!Existing) return Fail(FString::Printf(TEXT("%s is not a geometry collection; start the plan with make to make it."), *Path));
				if (!Existing->GetGeometryCollection().IsValid()) return Fail(FString::Printf(TEXT("%s holds no collection."), *Path));
				OutPlan.Existing = Existing;
			}
			if (Step.Kind == TEXT("fracture.clustered"))
			{
				const int32 Sites = Step.Params.Int(TEXT("Clusters")) * Step.Params.Int(TEXT("SitesPerCluster"));
				if (Sites > 2 * C::MaxSites) return Fail(FString::Printf(TEXT("Clusters x SitesPerCluster is %d, far past the most, %d sites."), Sites, C::MaxSites));
				if (Sites > C::MaxSites)
				{
					const int32 Per = FMath::Max(1, C::MaxSites / Step.Params.Int(TEXT("Clusters")));
					OutPlan.Fitted.Add(FString::Printf(TEXT("%s: SitesPerCluster %d -> %d, for %d sites at most"), *Step.Describe(), Step.Params.Int(TEXT("SitesPerCluster")), Per, C::MaxSites));
					Step.Params.Values.Add(TEXT("SitesPerCluster"), FString::FromInt(Per));
				}
			}
			else if (Step.Kind == TEXT("fracture.radial"))
			{
				const int32 Sites = Step.Params.Int(TEXT("Rings")) * Step.Params.Int(TEXT("AngularSteps"));
				if (Sites > 2 * C::MaxSites) return Fail(FString::Printf(TEXT("Rings x AngularSteps is %d, far past the most, %d sites."), Sites, C::MaxSites));
				if (Sites > C::MaxSites)
				{
					const int32 Rings = FMath::Max(1, C::MaxSites / Step.Params.Int(TEXT("AngularSteps")));
					OutPlan.Fitted.Add(FString::Printf(TEXT("%s: Rings %d -> %d, for %d sites at most"), *Step.Describe(), Step.Params.Int(TEXT("Rings")), Rings, C::MaxSites));
					Step.Params.Values.Add(TEXT("Rings"), FString::FromInt(Rings));
				}
				if (Step.Params.Vector(TEXT("Normal")).IsNearlyZero()) return Fail(TEXT("Normal is not zero."));
			}
			else if (Step.Kind == TEXT("fracture.planar"))
			{
				TArray<FTransform> Planes;
				if (!ParsePlanes(Step.Params.Text(TEXT("Planes")), Planes, Why)) return Fail(Why);
				if (Planes.IsEmpty() && Step.Params.Int(TEXT("Count")) == 0) return Fail(TEXT("Give Planes, a Count of random planes, or both."));
			}
			else if (Step.Kind == TEXT("fracture.slice"))
			{
				const int64 Pieces = int64(Step.Params.Int(TEXT("X")) + 1) * (Step.Params.Int(TEXT("Y")) + 1) * (Step.Params.Int(TEXT("Z")) + 1);
				if (Pieces <= 1) return Fail(TEXT("Give at least one cut across X, Y or Z."));
				if (Pieces > C::MaxPieces) return Fail(FString::Printf(TEXT("the slices would cut each piece into %lld and the most is %d; fewer cuts."), Pieces, C::MaxPieces));
			}
			else if (Step.Kind == TEXT("cluster"))
			{
				if (Step.Params.Int(TEXT("Clusters")) == 0 && !(Step.Params.Number(TEXT("Size")) > 0.0)) return Fail(TEXT("Give Clusters (how many) or Size (cm across each)."));
				if (Step.Params.Int(TEXT("Clusters")) == 1) return Fail(TEXT("Clusters is 2 or more: one cluster adds nothing."));
			}
			else if (Step.Kind == TEXT("damage"))
			{
				if (!Step.Params.Has(TEXT("Thresholds")) && !Step.Params.Has(TEXT("SizeSpecific")) && !Step.Params.Has(TEXT("Collision")))
				{
					return Fail(TEXT("Give Thresholds, SizeSpecific or Collision."));
				}
				TArray<float> Thresholds;
				if (Step.Params.Has(TEXT("Thresholds")) && !ParseThresholds(Step.Params.Text(TEXT("Thresholds")), Thresholds, Why)) return Fail(Why);
			}
			else if (Step.Kind == TEXT("collision"))
			{
				if (Step.Params.Values.IsEmpty()) return Fail(TEXT("Give Implicit, Type, Margin or Sizes."));
				TArray<TPair<float, FString>> Sizes;
				if (Step.Params.Has(TEXT("Sizes")) && !ParseSizes(Step.Params.Text(TEXT("Sizes")), Sizes, Why)) return Fail(Why);
			}
			else if (Step.Kind == TEXT("mass"))
			{
				if (Step.Params.Has(TEXT("Density")) && Step.Params.Has(TEXT("Mass"))) return Fail(TEXT("Give Density or Mass, not both."));
				if (bMaterial)
				{
					Step.PhysicalMaterial = LoadQuiet<UPhysicalMaterial>(Op.Material, Why);
					if (!Step.PhysicalMaterial) return Fail(FString::Printf(TEXT("%s is not a physical material."), *Op.Material));
				}
				if (Step.Params.Values.IsEmpty() && !bMaterial) return Fail(TEXT("Give Density, Mass or a physical Material."));
			}
			else if (Step.Kind == TEXT("anchor"))
			{
				if (Step.Params.Has(TEXT("Box")))
				{
					TArray<FString> Numbers;
					Step.Params.Text(TEXT("Box")).ParseIntoArray(Numbers, TEXT(","));
					double Value = 0.0;
					if (Numbers.Num() != 6 || Numbers.ContainsByPredicate([&Value](const FString& Each) { return !LexTryParseString(Value, *Each.TrimStartAndEnd()); }))
					{
						return Fail(TEXT("Box is \"x0,y0,z0,x1,y1,z1\", cm in the collection's space."));
					}
				}
			}
			else if (Step.Kind == TEXT("material.internal"))
			{
				if (!bMaterial) return Fail(TEXT("Give the Material for the inside faces."));
				Step.Material = LoadQuiet<UMaterialInterface>(Op.Material, Why);
				if (!Step.Material) return Fail(FString::Printf(TEXT("%s is not a material."), *Op.Material));
			}
			else if (Step.Kind == TEXT("rendering"))
			{
				if (Step.Params.Values.IsEmpty() && Op.Sources.IsEmpty()) return Fail(TEXT("Give Nanite, Renderer or proxy Sources."));
				for (const FString& Source : Op.Sources)
				{
					UStaticMesh* Mesh = LoadQuiet<UStaticMesh>(Source, Why);
					if (!Mesh) return Fail(FString::Printf(TEXT("%s is not a static mesh to use as a proxy."), *Source));
					Step.Proxies.Add(Mesh);
				}
			}
		}
		return true;
	}

	bool Build(const FPlan& Plan, FBuilt& OutBuilt, FString& OutError)
	{
		const double Start = FPlatformTime::Seconds();
		OutBuilt = FBuilt();
		FManagedArrayCollection& Working = OutBuilt.Collection;
		if (Plan.bMake)
		{
			FGeometryCollection Made;
			const FStep& Make = Plan.Steps[0];
			const bool bSplit = Make.Params.Bool(TEXT("SplitIslands"));
			for (const FSource& Source : Plan.Sources)
			{
				const int32 FirstMaterial = OutBuilt.Materials.Num();
				OutBuilt.Materials.Append(Source.Materials);
				if (Source.Materials.IsEmpty()) OutBuilt.Materials.Add(nullptr);
				if (!FGeometryCollectionEngineConversion::AppendStaticMesh(Source.Mesh, FirstMaterial, Source.Transform, &Made, /*bReindexMaterials=*/false,
					/*bAddInternalMaterials=*/false, bSplit, /*bSetInternalFromMaterialIndex=*/false))
				{
					OutError = FString::Printf(TEXT("%s: %s has no mesh data to convert. Nothing was changed."), *Make.Describe(), *Source.Mesh->GetPathName());
					return false;
				}
			}
			Made.ReindexMaterials();
			if (FGeometryCollectionClusteringUtility::ContainsMultipleRootBones(&Made))
			{
				FGeometryCollectionClusteringUtility::ClusterAllBonesUnderNewRoot(&Made);
			}
			GeometryCollection::GenerateTemporaryGuids(&Made, 0, true);
			// Without the collision particles the conversion made: they cannot be copied (every cut copies the pieces), and
			// the asset makes its own when it is first simulated.
			if (Made.HasAttribute(FGeometryDynamicCollection::SimplicialsAttribute, FTransformCollection::TransformGroup))
			{
				Made.RemoveAttribute(FGeometryDynamicCollection::SimplicialsAttribute, FTransformCollection::TransformGroup);
			}
			Made.CopyTo(&Working);
			OutBuilt.Notes.Add(FString::Printf(TEXT("%s: made from %d mesh(es), %d triangle(s), %d piece(s)."), *Make.Describe(), Plan.Sources.Num(), Plan.SourceTriangles,
				CountLeaves(Working)));
		}
		else if (Plan.Existing)
		{
			Plan.Existing->GetGeometryCollection()->CopyTo(&Working, {}, {{FGeometryDynamicCollection::SimplicialsAttribute, FTransformCollection::TransformGroup}});
			OutBuilt.Materials = Plan.Existing->Materials;
		}
		TObjectPtr<UMaterialInterface> Internal = nullptr;
		for (const FStep& Step : Plan.Steps)
		{
			if (Step.Kind == TEXT("make")) continue;
			const FParams& P = Step.Params;
			auto Fail = [&OutError, &Step](const FString& Message)
			{
				OutError = FString::Printf(TEXT("%s: %s Nothing was changed."), *Step.Describe(), *Message);
				return false;
			};
			if (Step.Kind.StartsWith(TEXT("fracture.")))
			{
				const int32 Before = CountLeaves(Working);
				TUniquePtr<FGeometryCollection> Collection(Working.NewCopy<FGeometryCollection>());
				const TArray<int32> Selected = Targets(*Collection, P.Int(TEXT("Level")));
				const TArray<int32> Leaves = LeavesUnder(*Collection, Selected);
				if (Leaves.IsEmpty()) return Fail(FString::Printf(TEXT("no pieces at level %d to cut."), P.Int(TEXT("Level"))));
				const TArray<FTransform> Globals = GlobalsOf(*Collection);
				const FBox Bounds = BoundsOf(*Collection, Globals, Leaves);
				const FCut Cut = CutOf(P);
				int32 First = INDEX_NONE;
				int32 Sites = 0;
				if (Step.Kind == TEXT("fracture.uniform"))
				{
					Sites = P.Int(TEXT("Sites"));
					First = CutVoronoi(Working, Selected, UniformSites(Bounds, Sites, Cut.Seed), Cut);
				}
				else if (Step.Kind == TEXT("fracture.clustered"))
				{
					const TArray<FVector> Points = ClusteredSites(Bounds, P.Int(TEXT("Clusters")), P.Int(TEXT("SitesPerCluster")), P.Number(TEXT("ClusterRadius")), Cut.Seed);
					Sites = Points.Num();
					First = CutVoronoi(Working, Selected, Points, Cut);
				}
				else if (Step.Kind == TEXT("fracture.radial"))
				{
					const double Radius = P.Number(TEXT("Radius")) > 0.0 ? P.Number(TEXT("Radius")) : Bounds.GetExtent().GetMax();
					const TArray<FVector> Points = RadialSites(Bounds.GetCenter() + Step.Location, P.Vector(TEXT("Normal")), Radius, P.Int(TEXT("Rings")), P.Int(TEXT("AngularSteps")),
						P.Number(TEXT("AngleOffset")), P.Number(TEXT("Variability")), Cut.Seed);
					Sites = Points.Num();
					First = CutVoronoi(Working, Selected, Points, Cut);
				}
				else if (Step.Kind == TEXT("fracture.planar"))
				{
					TArray<FTransform> Planes;
					FString Ignored;
					ParsePlanes(P.Text(TEXT("Planes")), Planes, Ignored);
					First = FFractureEngineFracturing::PlaneCutter(Working, SelectionOf(Working, Selected), Bounds, FTransform::Identity, P.Int(TEXT("Count")), Cut.Seed, 1.f,
						FIslandSplitSettings(Cut.bSplitIslands), Cut.Grout, Cut.Noise, Cut.Frequency, 0.5f, 2.f, Cut.Octaves, Cut.Spacing, false, 0.f, Planes);
				}
				else if (Step.Kind == TEXT("fracture.slice"))
				{
					First = FFractureEngineFracturing::SliceCutter(Working, SelectionOf(Working, Selected), Bounds, P.Int(TEXT("X")), P.Int(TEXT("Y")), P.Int(TEXT("Z")),
						static_cast<float>(P.Number(TEXT("AngleVariation"))), static_cast<float>(P.Number(TEXT("OffsetVariation"))), Cut.Seed, 1.f,
						FIslandSplitSettings(Cut.bSplitIslands), Cut.Grout, Cut.Noise, Cut.Frequency, 0.5f, 2.f, Cut.Octaves, Cut.Spacing, false, 0.f);
				}
				else if (Step.Kind == TEXT("fracture.brick"))
				{
					const FVector Size = Bounds.GetSize();
					const double Length = P.Number(TEXT("Length"));
					const double Height = P.Number(TEXT("Height"));
					const double Depth = P.Number(TEXT("Depth"));
					const double Bricks = FMath::CeilToDouble(Size.X / Length + 1.0) * FMath::CeilToDouble(Size.Y / Depth + 1.0) * FMath::CeilToDouble(Size.Z / Height + 1.0);
					if (Bricks > C::MaxPieces)
					{
						return Fail(FString::Printf(TEXT("%.0f x %.0f x %.0f cm in bricks of %s x %s x %s cm is about %.0f bricks and the most is %d; make the bricks larger."),
							Size.X, Size.Y, Size.Z, *Number(Length), *Number(Depth), *Number(Height), Bricks, C::MaxPieces));
					}
					First = FFractureEngineFracturing::BrickCutter(Working, SelectionOf(Working, Selected), Bounds, FTransform(Step.Location), BondOf(P.Text(TEXT("Bond"))),
						static_cast<float>(Length), static_cast<float>(Height), static_cast<float>(Depth), Cut.Seed, 1.f, FIslandSplitSettings(Cut.bSplitIslands), Cut.Grout,
						Cut.Noise, Cut.Frequency, 0.5f, 2.f, Cut.Octaves, Cut.Spacing, false, 0.f);
				}
				FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Working, -1);
				const int32 After = CountLeaves(Working);
				if (After > C::MaxPieces)
				{
					return Fail(FString::Printf(TEXT("it made %d pieces and the most a collection holds is %d; fewer sites, or cut one level at a time."), After, C::MaxPieces));
				}
				FString Note = FString::Printf(TEXT("%s: %d piece(s) became %d"), *Step.Describe(), Before, After);
				if (Sites > 0) Note += FString::Printf(TEXT(" from %d site(s)"), Sites);
				if (First == INDEX_NONE) Note += TEXT("; it cut nothing (the cells or planes missed the pieces)");
				else if (Sites > 0 && After - Before + static_cast<int32>(Leaves.Num()) < Sites)
				{
					Note += TEXT("; fewer pieces than sites, as some cells fell outside the mesh");
				}
				if (Cut.Noise > 0.f) Note += FString::Printf(TEXT("; noise %s cm on the cuts"), *Number(Cut.Noise));
				OutBuilt.Notes.Add(Note + TEXT("."));
			}
			else if (Step.Kind == TEXT("cluster"))
			{
				TUniquePtr<FGeometryCollection> Collection(Working.NewCopy<FGeometryCollection>());
				const int32 Level = P.Int(TEXT("Level"));
				const TArray<int32> Levels = LevelsOf(*Collection);
				TArray<int32> Parents;
				for (int32 Index = 0; Index < Levels.Num(); ++Index)
				{
					if (Levels[Index] == Level && !Collection->Children[Index].IsEmpty()) Parents.Add(Index);
				}
				if (Parents.IsEmpty()) return Fail(FString::Printf(TEXT("there is no cluster at level %d to group; fracture first."), Level));
				const int32 Clusters = P.Int(TEXT("Clusters"));
				const EFractureEngineClusterSizeMethod Method = Clusters > 0 ? EFractureEngineClusterSizeMethod::ByNumber : EFractureEngineClusterSizeMethod::BySize;
				for (const int32 Parent : Parents)
				{
					FFractureEngineClustering::AutoCluster(*Collection, Parent, Method, static_cast<uint32>(FMath::Max(Clusters, 0)), 0.5f, static_cast<float>(P.Number(TEXT("Size"))),
						P.Bool(TEXT("Connected")), /*bAvoidIsolated=*/true, /*bEnforceSiteParameters=*/true);
				}
				FGeometryCollectionProximityUtility(Collection.Get()).UpdateProximity();
				Working = static_cast<const FManagedArrayCollection&>(*Collection);
				FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Working, -1);
				TUniquePtr<FGeometryCollection> After(Working.NewCopy<FGeometryCollection>());
				const TArray<int32> NewLevels = LevelsOf(*After);
				int32 Made = 0;
				for (int32 Index = 0; Index < NewLevels.Num(); ++Index)
				{
					if (NewLevels[Index] == Level + 1 && !After->Children[Index].IsEmpty()) ++Made;
				}
				OutBuilt.Notes.Add(FString::Printf(TEXT("%s: the pieces under %d cluster(s) at level %d were grouped into %d cluster(s) at level %d."), *Step.Describe(), Parents.Num(),
					Level, Made, Level + 1));
			}
			else if (Step.Kind == TEXT("cleanup"))
			{
				const int32 Before = CountLeaves(Working);
				const float MinSize = static_cast<float>(P.Number(TEXT("MinSize")));
				if (P.Text(TEXT("Mode")) == TEXT("remove"))
				{
					TUniquePtr<FGeometryCollection> Collection(Working.NewCopy<FGeometryCollection>());
					TArray<int32> Small;
					for (int32 Index = 0; Index < Collection->Children.Num(); ++Index)
					{
						const int32 Geometry = Collection->TransformToGeometryIndex[Index];
						if (Collection->Children[Index].IsEmpty() && Geometry != INDEX_NONE && Collection->Parent[Index] != INDEX_NONE
							&& SizeOf(VolumeOf(*Collection, Geometry)) < MinSize)
						{
							Small.Add(Index);
						}
					}
					if (!Small.IsEmpty())
					{
						FFractureEngineEdit::DeleteBranch(*Collection, Small);
						Working = static_cast<const FManagedArrayCollection&>(*Collection);
						FFractureEngineUtility::ValidateGeometryCollection(Working, /*RemoveUnreferencedGeometry=*/true, /*RemoveClustersOfOne=*/true, /*RemoveDanglingClusters=*/true);
					}
				}
				else
				{
					TUniquePtr<FGeometryCollection> Collection(Working.NewCopy<FGeometryCollection>());
					FFractureEngineUtility::FixTinyGeo(Working, SelectionOf(Working, GeometryTransforms(*Collection)), EFixTinyGeoMergeType::MergeGeometry, /*InOnFractureLevel=*/false,
						EFixTinyGeoGeometrySelectionMethod::VolumeCubeRoot, MinSize, 0.f, EFixTinyGeoUseBoneSelection::NoEffect, /*InOnlyClusters=*/false,
						EFixTinyGeoNeighborSelectionMethod::LargestNeighbor, /*InOnlyToConnected=*/true, /*InOnlySameParent=*/true);
				}
				FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Working, -1);
				const int32 After = CountLeaves(Working);
				OutBuilt.Notes.Add(FString::Printf(TEXT("%s: %d piece(s) under %s cm %s; %d piece(s) left."), *Step.Describe(), Before - After, *Number(MinSize),
					P.Text(TEXT("Mode")) == TEXT("remove") ? TEXT("removed") : TEXT("merged into a neighbour"), After));
			}
			else if (Step.Kind == TEXT("anchor"))
			{
				Chaos::Facades::FCollectionAnchoringFacade Anchoring(Working);
				if (!Anchoring.HasAnchoredAttribute()) Anchoring.AddAnchoredAttribute();
				TUniquePtr<FGeometryCollection> Collection(Working.NewCopy<FGeometryCollection>());
				const int32 Count = Collection->NumElements(FGeometryCollection::TransformGroup);
				if (P.Bool(TEXT("Clear")))
				{
					TArray<int32> All;
					for (int32 Index = 0; Index < Count; ++Index) All.Add(Index);
					Anchoring.SetAnchored(All, false);
				}
				const TArray<FTransform> Globals = GlobalsOf(*Collection);
				const TArray<int32> Leaves = LeavesUnder(*Collection, {RootOf(*Collection)});
				const FBox Whole = BoundsOf(*Collection, Globals, Leaves);
				FBox Box(ForceInit);
				if (P.Has(TEXT("Box")))
				{
					TArray<FString> Numbers;
					P.Text(TEXT("Box")).ParseIntoArray(Numbers, TEXT(","));
					double V[6] = {};
					for (int32 Index = 0; Index < 6; ++Index) LexTryParseString(V[Index], *Numbers[Index].TrimStartAndEnd());
					Box = FBox(FVector(FMath::Min(V[0], V[3]), FMath::Min(V[1], V[4]), FMath::Min(V[2], V[5])), FVector(FMath::Max(V[0], V[3]), FMath::Max(V[1], V[4]), FMath::Max(V[2], V[5])));
				}
				TArray<int32> Pinned;
				for (const int32 Leaf : Leaves)
				{
					const FBox Piece = BoundsOf(*Collection, Globals, {Leaf});
					const bool bHit = Box.IsValid ? Piece.Intersect(Box) : (P.Bool(TEXT("Ground")) && Piece.Min.Z <= Whole.Min.Z + P.Number(TEXT("Height")));
					if (bHit) Pinned.Add(Leaf);
				}
				Anchoring.SetAnchored(Pinned, true);
				OutBuilt.Notes.Add(FString::Printf(TEXT("%s: %d of %d piece(s) anchored%s."), *Step.Describe(), Pinned.Num(), Leaves.Num(),
					Box.IsValid ? TEXT(" inside the box") : *FString::Printf(TEXT(" within %s cm of the bottom"), *Number(P.Number(TEXT("Height"))))));
			}
			else if (Step.Kind == TEXT("state"))
			{
				// Applied after every cut, whatever the op's place in the plan, so the pieces cut later start the same way.
				OutBuilt.Settings.State = P.Text(TEXT("State"));
			}
			else if (Step.Kind == TEXT("damage"))
			{
				if (P.Has(TEXT("Thresholds")))
				{
					TArray<float> Thresholds;
					FString Ignored;
					ParseThresholds(P.Text(TEXT("Thresholds")), Thresholds, Ignored);
					OutBuilt.Settings.Thresholds = Thresholds;
				}
				if (P.Has(TEXT("SizeSpecific"))) OutBuilt.Settings.bSizeSpecificDamage = P.Bool(TEXT("SizeSpecific"));
				if (P.Has(TEXT("Collision"))) OutBuilt.Settings.bDamageFromCollision = P.Bool(TEXT("Collision"));
				OutBuilt.Notes.Add(FString::Printf(TEXT("%s: %s."), *Step.Describe(), *Step.Op.Params.TrimStartAndEnd()));
			}
			else if (Step.Kind == TEXT("collision"))
			{
				if (P.Has(TEXT("Implicit"))) OutBuilt.Settings.Implicit = P.Text(TEXT("Implicit"));
				if (P.Has(TEXT("Type"))) OutBuilt.Settings.CollisionType = P.Text(TEXT("Type"));
				if (P.Has(TEXT("Margin"))) OutBuilt.Settings.CollisionMargin = static_cast<float>(P.Number(TEXT("Margin")));
				if (P.Has(TEXT("Sizes")))
				{
					FString Ignored;
					ParseSizes(P.Text(TEXT("Sizes")), OutBuilt.Settings.Sizes, Ignored);
					OutBuilt.Settings.bSizesSet = true;
				}
				OutBuilt.Notes.Add(FString::Printf(TEXT("%s: %s."), *Step.Describe(), *Step.Op.Params.TrimStartAndEnd()));
			}
			else if (Step.Kind == TEXT("mass"))
			{
				if (P.Has(TEXT("Density"))) OutBuilt.Settings.Density = static_cast<float>(P.Number(TEXT("Density")));
				if (P.Has(TEXT("Mass"))) OutBuilt.Settings.MassKg = static_cast<float>(P.Number(TEXT("Mass")));
				if (Step.PhysicalMaterial) OutBuilt.Settings.PhysicalMaterial = Step.PhysicalMaterial;
				OutBuilt.Notes.Add(FString::Printf(TEXT("%s: %s%s."), *Step.Describe(), *Step.Op.Params.TrimStartAndEnd(),
					Step.PhysicalMaterial ? *FString::Printf(TEXT(" physical material %s"), *Step.PhysicalMaterial->GetName()) : TEXT("")));
			}
			else if (Step.Kind == TEXT("material.internal"))
			{
				// Applied after every cut, whatever the op's place in the plan: later cuts make inside faces too.
				Internal = Step.Material;
			}
			else if (Step.Kind == TEXT("rendering"))
			{
				if (P.Has(TEXT("Nanite"))) OutBuilt.Settings.bNanite = P.Bool(TEXT("Nanite"));
				if (P.Has(TEXT("Renderer"))) OutBuilt.Settings.Renderer = P.Text(TEXT("Renderer"));
				OutBuilt.Settings.Proxies.Append(Step.Proxies);
				if (!Step.Proxies.IsEmpty() && !OutBuilt.Settings.Renderer.IsSet()) OutBuilt.Settings.Renderer = FString(TEXT("root_proxy"));
				OutBuilt.Notes.Add(FString::Printf(TEXT("%s: %s%s."), *Step.Describe(), *Step.Op.Params.TrimStartAndEnd(),
					Step.Proxies.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" with %d proxy mesh(es)"), Step.Proxies.Num())));
			}
		}
		if (Internal)
		{
			int32 Slot = OutBuilt.Materials.Find(Internal);
			if (Slot == INDEX_NONE) Slot = OutBuilt.Materials.Add(Internal);
			TUniquePtr<FGeometryCollection> Collection(Working.NewCopy<FGeometryCollection>());
			// The leaves only: a cluster keeps its unbroken geometry, and with it in the selection the call would take the
			// cluster for the whole branch and leave its pieces alone.
			const TArray<int32> Leaves = LeavesUnder(*Collection, {RootOf(*Collection)});
			FFractureEngineMaterials::SetMaterial(Working, Leaves, FFractureEngineMaterials::ETargetFaces::InternalFaces, Slot);
			int32 Inside = 0;
			for (const int32 Leaf : Leaves)
			{
				const int32 Geometry = Collection->TransformToGeometryIndex[Leaf];
				for (int32 Face = Collection->FaceStart[Geometry]; Face < Collection->FaceStart[Geometry] + Collection->FaceCount[Geometry]; ++Face)
				{
					Inside += Collection->Internal[Face] ? 1 : 0;
				}
			}
			OutBuilt.Notes.Add(FString::Printf(TEXT("material.internal: %s on %d inside face(s)%s."), *Internal->GetName(), Inside,
				Inside == 0 ? TEXT(" (none yet: fracture first, and the cut faces take it)") : TEXT("")));
		}
		if (OutBuilt.Settings.State.IsSet())
		{
			// The Dataflow SetDynamicState node's call, on every piece: Chaos derives a cluster's state from its pieces', so
			// a kinematic or static whole needs kinematic or static pieces. A Damage field (or Epic's master field) turns the
			// pieces it reaches dynamic again; anchors are the way to hold a base while broken pieces fall.
			Chaos::Facades::FCollectionAnchoringFacade Anchoring(Working);
			TUniquePtr<FGeometryCollection> Collection(Working.NewCopy<FGeometryCollection>());
			TArray<int32> All;
			for (int32 Index = 0; Index < Collection->NumElements(FGeometryCollection::TransformGroup); ++Index) All.Add(Index);
			if (Anchoring.HasInitialDynamicStateAttribute()) Anchoring.SetInitialDynamicState(All, StateOf(OutBuilt.Settings.State.GetValue()));
			OutBuilt.Notes.Add(FString::Printf(TEXT("state: every piece starts %s%s."), *OutBuilt.Settings.State.GetValue(),
				OutBuilt.Settings.State.GetValue() == TEXT("dynamic") || OutBuilt.Settings.State.GetValue() == TEXT("sleeping") ? TEXT("")
				: TEXT("; pieces a Damage field reaches turn dynamic and fall")));
		}
		OutBuilt.BuildMs = static_cast<int32>((FPlatformTime::Seconds() - Start) * 1000.0);
		return true;
	}

	FMeasure Measure(const FManagedArrayCollection& Working, const TArray<float>& Thresholds, const int32 MaxPieces)
	{
		FMeasure Out;
		TUniquePtr<FGeometryCollection> Collection(Working.NewCopy<FGeometryCollection>());
		const int32 Count = Collection->NumElements(FGeometryCollection::TransformGroup);
		Out.Bones = Count;
		if (Count == 0) return Out;
		const TArray<int32> Levels = LevelsOf(*Collection);
		const TArray<FTransform> Globals = GlobalsOf(*Collection);
		const Chaos::Facades::FCollectionAnchoringFacade Anchoring(static_cast<const FManagedArrayCollection&>(*Collection));
		const bool bAnchors = Anchoring.HasAnchoredAttribute();
		Out.Triangles = Collection->Indices.Num();
		// Each transform's volume: its own geometry's, or the sum of its leaves'.
		TArray<double> Volumes;
		Volumes.Init(0.0, Count);
		float Smallest = TNumericLimits<float>::Max();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const int32 Geometry = Collection->TransformToGeometryIndex[Index];
			if (Geometry == INDEX_NONE) continue;
			++Out.Geometry;
			if (!Collection->Children[Index].IsEmpty()) continue;
			const double Volume = VolumeOf(*Collection, Geometry);
			Out.VolumeCm3 += Volume;
			Out.Bounds += Collection->BoundingBox[Geometry].TransformBy(Globals[Index]);
			for (int32 At = Index; At != INDEX_NONE; At = Collection->Parent[At]) Volumes[At] += Volume;
		}
		int32 Deepest = 0;
		for (const int32 Level : Levels) Deepest = FMath::Max(Deepest, Level);
		Out.Levels.SetNum(Deepest + 1);
		for (int32 Level = 0; Level <= Deepest; ++Level)
		{
			Out.Levels[Level].Level = Level;
			Out.Levels[Level].SmallestCm = TNumericLimits<float>::Max();
		}
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FHyperAIDestructionLevel& Level = Out.Levels[Levels[Index]];
			const bool bCluster = !Collection->Children[Index].IsEmpty();
			const float Size = SizeOf(Volumes[Index]);
			++Level.Pieces;
			++(bCluster ? Level.Clusters : Level.Leaves);
			++(bCluster ? Out.Clusters : Out.Leaves);
			Level.SmallestCm = FMath::Min(Level.SmallestCm, Size);
			Level.LargestCm = FMath::Max(Level.LargestCm, Size);
			if (!bCluster)
			{
				Smallest = FMath::Min(Smallest, Size);
				if (Size < 5.f) ++Out.LeavesUnder5Cm;
				if (bAnchors && Anchoring.IsAnchored(Index)) ++Out.Anchored;
			}
			if (Out.Pieces.Num() < MaxPieces)
			{
				FHyperAIDestructionPiece& Piece = Out.Pieces.AddDefaulted_GetRef();
				Piece.Index = Index;
				Piece.Name = Collection->BoneName.IsValidIndex(Index) ? Collection->BoneName[Index] : FString();
				Piece.Level = Levels[Index];
				Piece.Parent = Collection->Parent[Index];
				Piece.Children = Collection->Children[Index].Num();
				Piece.SizeCm = bCluster ? 0.f : Size;
				const int32 Geometry = Collection->TransformToGeometryIndex[Index];
				const FVector Centre = Geometry != INDEX_NONE ? Globals[Index].TransformPosition(Collection->BoundingBox[Geometry].GetCenter())
					: BoundsOf(*Collection, Globals, LeavesUnder(*Collection, {Index})).GetCenter();
				Piece.Center = VectorText(Centre);
				Piece.bAnchored = bAnchors && Anchoring.IsAnchored(Index);
			}
		}
		for (FHyperAIDestructionLevel& Level : Out.Levels)
		{
			if (Level.SmallestCm == TNumericLimits<float>::Max()) Level.SmallestCm = 0.f;
			Level.DamageThreshold = Level.Clusters > 0 && !Thresholds.IsEmpty() ? Thresholds[FMath::Min(Level.Level, Thresholds.Num() - 1)] : 0.f;
		}
		Out.SmallestLeafCm = Smallest == TNumericLimits<float>::Max() ? 0.f : Smallest;
		const int32 Root = RootOf(*Collection);
		Out.RootState = Anchoring.HasInitialDynamicStateAttribute() && Root != INDEX_NONE ? StateName(Collection->InitialDynamicState[Root]) : FString(TEXT("default"));
		return Out;
	}

	float MassOf(const FMeasure& Measure, const bool bMassAsDensity, const float Mass)
	{
		return bMassAsDensity ? static_cast<float>(Measure.VolumeCm3 / 1.0e6 * Mass) : Mass;
	}

	TArray<FString> Warn(const FMeasure& Measure, const TArray<float>& Thresholds, const bool bPlaced, const int32 SitesAsked)
	{
		TArray<FString> Out;
		if (Measure.Clusters == 0)
		{
			Out.Add(TEXT("Not fractured: one piece never breaks. Add a fracture op."));
		}
		if (Measure.Leaves > 1000)
		{
			Out.Add(FString::Printf(TEXT("%d pieces is heavy for physics; use fewer sites, or cluster them so they break off in chunks."), Measure.Leaves));
		}
		if (Measure.LeavesUnder5Cm > 0)
		{
			Out.Add(FString::Printf(TEXT("%d piece(s) are under 5 cm (the smallest %.1f cm): tiny pieces jitter and cost; cleanup MinSize=5 merges them."), Measure.LeavesUnder5Cm,
				Measure.SmallestLeafCm));
		}
		int32 ClusterLevels = 0;
		for (const FHyperAIDestructionLevel& Level : Measure.Levels)
		{
			if (Level.Clusters > 0) ++ClusterLevels;
		}
		if (ClusterLevels > Thresholds.Num() && !Thresholds.IsEmpty())
		{
			Out.Add(FString::Printf(TEXT("%d level(s) hold clusters but %d threshold(s) are set: the deeper ones use the last, %s."), ClusterLevels, Thresholds.Num(),
				*Number(Thresholds.Last())));
		}
		if (bPlaced && Measure.Anchored == 0 && (Measure.RootState == TEXT("default") || Measure.RootState == TEXT("dynamic")))
		{
			Out.Add(TEXT("Nothing is anchored and it starts dynamic: in play it falls or settles as one body until something breaks it. anchor Ground=true, or state static, holds it up."));
		}
		if (SitesAsked > 0 && Measure.Leaves < SitesAsked / 2)
		{
			Out.Add(FString::Printf(TEXT("%d sites made %d pieces: most cells missed the mesh, which is thinner or smaller than its bounds."), SitesAsked, Measure.Leaves));
		}
		return Out;
	}

	void WriteAsset(UGeometryCollection& Asset, const FBuilt& Built, const TArray<FSource>& Sources)
	{
		TArray<UMaterialInterface*> Materials;
		for (const TObjectPtr<UMaterialInterface>& Material : Built.Materials) Materials.Add(Material);
		// The Dataflow terminal node's write: the collection, its materials deduplicated, convex hulls where missing.
		Asset.ResetFrom(Built.Collection, Materials, /*bHasInternalMaterials=*/false);
		if (!Sources.IsEmpty())
		{
			Asset.GeometrySource.Reset();
			for (const FSource& Source : Sources)
			{
				Asset.GeometrySource.Emplace(FSoftObjectPath(Source.Mesh.Get()), Source.Transform, Source.Materials);
			}
			Asset.EnableNanite = Sources.ContainsByPredicate([](const FSource& Source) { return Source.Mesh && Source.Mesh->IsNaniteEnabled(); });
		}
		const FSettings& S = Built.Settings;
		if (S.Thresholds.IsSet())
		{
			Asset.DamageModel = EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold;
			Asset.DamageThreshold = S.Thresholds.GetValue();
		}
		if (S.bSizeSpecificDamage.IsSet()) Asset.bUseSizeSpecificDamageThreshold = S.bSizeSpecificDamage.GetValue();
		FGeometryCollectionSizeSpecificData& Default = Asset.GetDefaultSizeSpecificData();
		if (Default.CollisionShapes.IsEmpty()) Default.CollisionShapes.AddDefaulted();
		if (S.Implicit.IsSet()) Default.CollisionShapes[0].ImplicitType = ImplicitOf(S.Implicit.GetValue());
		if (S.CollisionType.IsSet())
		{
			Default.CollisionShapes[0].CollisionType = S.CollisionType.GetValue() == TEXT("surface") ? ECollisionTypeEnum::Chaos_Surface_Volumetric : ECollisionTypeEnum::Chaos_Volumetric;
		}
		if (S.CollisionMargin.IsSet()) Default.CollisionShapes[0].CollisionMarginFraction = S.CollisionMargin.GetValue();
		if (S.bSizesSet)
		{
			const FGeometryCollectionSizeSpecificData Kept = Default;
			Asset.SizeSpecificData.Reset();
			Asset.SizeSpecificData.Add(Kept);
			for (const TPair<float, FString>& Size : S.Sizes)
			{
				FGeometryCollectionSizeSpecificData Entry = Kept;
				Entry.MaxSize = Size.Key;
				Entry.CollisionShapes[0].ImplicitType = ImplicitOf(Size.Value);
				Asset.SizeSpecificData.Add(Entry);
			}
		}
		if (S.Density.IsSet())
		{
			Asset.bMassAsDensity = true;
			Asset.Mass = S.Density.GetValue();
		}
		if (S.MassKg.IsSet())
		{
			Asset.bMassAsDensity = false;
			Asset.Mass = S.MassKg.GetValue();
		}
		if (S.PhysicalMaterial) Asset.PhysicsMaterial = S.PhysicalMaterial;
		if (S.bNanite.IsSet()) Asset.EnableNanite = S.bNanite.GetValue();
		if (S.Renderer.IsSet())
		{
			if (S.Renderer.GetValue() == TEXT("root_proxy"))
			{
				Asset.CustomRendererType = UGeometryCollectionRootProxyRenderer::StaticClass();
				if (!S.Proxies.IsEmpty())
				{
					Asset.RootProxyData.Reset();
					for (const TObjectPtr<UStaticMesh>& Proxy : S.Proxies) Asset.RootProxyData.ProxyMeshes.Add(Proxy);
				}
				else
				{
					Asset.SetRootProxiesFromGeometrySources();
				}
			}
			else
			{
				Asset.CustomRendererType = nullptr;
				Asset.RootProxyData.Reset();
			}
		}
		Asset.InvalidateCollection();
		Asset.UpdateConvexGeometry();
		Asset.RebuildRenderData();
		Asset.PropagateTransformUpdateToComponents();
		Asset.PropagateMarkDirtyToComponents();
	}

	FString VerifyCaseJson(const FString& Label, const FVector& Center, const double Radius, const int32 MinPieces)
	{
		auto StepOf = [](const TCHAR* Action)
		{
			TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("action"), Action);
			return Step;
		};
		auto Location = [&Center]()
		{
			TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
			Point->SetNumberField(TEXT("x"), FMath::RoundToDouble(Center.X));
			Point->SetNumberField(TEXT("y"), FMath::RoundToDouble(Center.Y));
			Point->SetNumberField(TEXT("z"), FMath::RoundToDouble(Center.Z));
			return Point;
		};
		TArray<TSharedPtr<FJsonValue>> Steps;
		// In a World Partition level the collection streams in with its cell: wait for it, then let it settle.
		TSharedRef<FJsonObject> There = StepOf(TEXT("WaitUntil"));
		There->SetStringField(TEXT("until"), TEXT("AssertExists"));
		There->SetStringField(TEXT("target"), Label);
		There->SetNumberField(TEXT("timeoutSeconds"), 10);
		Steps.Add(MakeShared<FJsonValueObject>(There));
		TSharedRef<FJsonObject> Settle = StepOf(TEXT("Wait"));
		Settle->SetNumberField(TEXT("seconds"), 0.3);
		Steps.Add(MakeShared<FJsonValueObject>(Settle));
		TSharedRef<FJsonObject> Damage = StepOf(TEXT("Damage"));
		Damage->SetStringField(TEXT("target"), Label);
		Damage->SetObjectField(TEXT("location"), Location());
		Damage->SetNumberField(TEXT("radius"), FMath::RoundToDouble(Radius));
		Steps.Add(MakeShared<FJsonValueObject>(Damage));
		TSharedRef<FJsonObject> Until = StepOf(TEXT("WaitUntil"));
		Until->SetStringField(TEXT("until"), TEXT("AssertBroken"));
		Until->SetStringField(TEXT("target"), Label);
		Until->SetStringField(TEXT("compare"), TEXT(">="));
		Until->SetStringField(TEXT("value"), FString::FromInt(MinPieces));
		Until->SetNumberField(TEXT("timeoutSeconds"), 5);
		Steps.Add(MakeShared<FJsonValueObject>(Until));
		TSharedRef<FJsonObject> Fall = StepOf(TEXT("Wait"));
		Fall->SetNumberField(TEXT("seconds"), 1.5);
		Steps.Add(MakeShared<FJsonValueObject>(Fall));
		TSharedRef<FJsonObject> Count = StepOf(TEXT("AssertBroken"));
		Count->SetStringField(TEXT("target"), Label);
		Count->SetStringField(TEXT("compare"), TEXT(">="));
		Count->SetStringField(TEXT("value"), FString::FromInt(MinPieces));
		Steps.Add(MakeShared<FJsonValueObject>(Count));
		TSharedRef<FJsonObject> Case = MakeShared<FJsonObject>();
		Case->SetStringField(TEXT("name"), Label + TEXT("-breaks"));
		Case->SetNumberField(TEXT("timeoutSeconds"), 20);
		Case->SetArrayField(TEXT("steps"), Steps);
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Cases;
		Cases.Add(MakeShared<FJsonValueObject>(Case));
		Request->SetArrayField(TEXT("cases"), Cases);
		Request->SetNumberField(TEXT("deadlineSeconds"), 30);
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Request, Writer);
		return Out;
	}

	void SetPreviewsOffForTests(const bool bOff)
	{
		GPreviewsOff = bOff;
	}

	bool DrawExploded(const FManagedArrayCollection& Working, const TArray<TObjectPtr<UMaterialInterface>>& Materials, const FString& PreviewName, FString& OutPath,
		FString& OutReason)
	{
		OutPath.Reset();
		if (GPreviewsOff || !FApp::CanEverRender() || !IsInGameThread())
		{
			OutReason = TEXT("this editor runs without rendering (null RHI), so nothing can be drawn");
			return false;
		}
		TUniquePtr<FGeometryCollection> Collection(Working.NewCopy<FGeometryCollection>());
		const TArray<FTransform> Globals = GlobalsOf(*Collection);
		const TArray<int32> Leaves = LeavesUnder(*Collection, {RootOf(*Collection)});
		const FBox Whole = BoundsOf(*Collection, Globals, Leaves);
		if (Leaves.IsEmpty() || !Whole.IsValid)
		{
			OutReason = TEXT("the collection has no pieces to draw");
			return false;
		}
		// Each piece pushed out from the middle by half its distance from it, so every crack opens.
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		const int32 Slots = FMath::Max(1, Materials.Num());
		TArray<FPolygonGroupID> Groups;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			const FPolygonGroupID Group = Description.CreatePolygonGroup();
			SlotNames[Group] = FName(*FString::Printf(TEXT("Slot%d"), Slot));
			Groups.Add(Group);
		}
		const FVector Middle = Whole.GetCenter();
		for (const int32 Leaf : Leaves)
		{
			const int32 Geometry = Collection->TransformToGeometryIndex[Leaf];
			const FTransform& Global = Globals[Leaf];
			const FVector Push = (Global.TransformPosition(Collection->BoundingBox[Geometry].GetCenter()) - Middle) * 0.5;
			const int32 First = Collection->FaceStart[Geometry];
			const int32 Last = First + Collection->FaceCount[Geometry];
			for (int32 Face = First; Face < Last; ++Face)
			{
				const FIntVector& Tri = Collection->Indices[Face];
				const FVector Corners[3] = {Global.TransformPosition(FVector(Collection->Vertex[Tri.X])) + Push, Global.TransformPosition(FVector(Collection->Vertex[Tri.Y])) + Push,
					Global.TransformPosition(FVector(Collection->Vertex[Tri.Z])) + Push};
				const FVector3f Normal = FVector3f(FVector::CrossProduct(Corners[2] - Corners[0], Corners[1] - Corners[0]).GetSafeNormal());
				TArray<FVertexInstanceID> Instances;
				for (const FVector& Corner : Corners)
				{
					const FVertexID Vertex = Description.CreateVertex();
					Positions[Vertex] = FVector3f(Corner);
					const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
					Normals[Instance] = Normal;
					Instances.Add(Instance);
				}
				const int32 Slot = FMath::Clamp(Collection->MaterialID[Face], 0, Slots - 1);
				Description.CreateTriangle(Groups[Slot], Instances);
			}
		}
		// Built, drawn and let go outside any undo step: nothing of it outlives the call but the picture.
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		UStaticMesh* Temp = NewObject<UStaticMesh>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("HyperAIDestructionPreview")),
			RF_Transient);
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			UMaterialInterface* Material = Materials.IsValidIndex(Slot) && Materials[Slot] ? Materials[Slot].Get() : UMaterial::GetDefaultMaterial(MD_Surface);
			Temp->GetStaticMaterials().Add(FStaticMaterial(Material, SlotNames[Groups[Slot]], SlotNames[Groups[Slot]]));
		}
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bUseHashAsGuid = true;
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = false;
		Params.bFastBuild = true;
		if (!Temp->BuildFromMeshDescriptions({&Description}, Params))
		{
			OutReason = TEXT("the temporary mesh for the picture could not be built");
			return false;
		}
		FString Drawn;
		FString Error;
		if (!FHyperAIStudioResultPreview::WriteAssetPreview(*Temp, Drawn, Error))
		{
			OutReason = Error == TEXT("preview_write_failed") ? TEXT("the PNG could not be written (preview_write_failed)") : TEXT("the thumbnail renderer drew nothing (preview_render_failed)");
			return false;
		}
		const FString Named = FHyperAIStudioResultPreview::GetAssetPreviewPath(PreviewName);
		OutPath = IFileManager::Get().Move(*Named, *Drawn, /*bReplace=*/true) ? Named : Drawn;
		return true;
	}
}
