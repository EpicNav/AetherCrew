// Games by Hyper 2026.

#include "HyperAIStudioModelCreator.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshBoundaryLoops.h"
#include "MeshConversionOptions.h"
#include "MeshDescription.h"
#include "MeshConstraintsUtil.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "MeshSimplification.h"
#include "DynamicMeshToMeshDescription.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Generators/MinimalBoxMeshGenerator.h"
#include "Operations/MeshBoolean.h"
#include "Operations/MinimalHoleFiller.h"
#include "Operations/SimpleHoleFiller.h"
#include "Parameterization/DynamicMeshUVEditor.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"
#include "Engine/StaticMeshSocket.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "XAtlasWrapper.h"

/**
 * The in-editor half of the model creator: a mesh measured (budget, topology, manifoldness, holes, winding, floating
 * bits, size, pivot, UVs, texel density, textures, PBR maps, Nanite, collision, a visual compare), the free fixes run on
 * it, and its PBR maps made into a material instance of HyperAI's master material.
 */
namespace HyperAIStudio::MeshGen::Creator
{
	using namespace UE::Geometry;

	namespace InspectDetail
	{
		const TCHAR* const MasterPath = TEXT("/Game/HyperAI/Materials/M_HyperAIGeneratedMaster");
		const TCHAR* const WhiteTexture = TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");
		const TCHAR* const FlatNormalTexture = TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal");
		/** The ORM's default: a linear white mask of HyperAI's own. The engine's white square is sRGB colour, which a Masks sampler refuses. */
		const TCHAR* const WhiteMaskPath = TEXT("/Game/HyperAI/Materials/T_HyperAIWhiteMask");
		/** A hole with more edges than this is a missing part, not a hole: Tripo's completion fixes those. */
		constexpr int32 MaxFillEdges = 256;

		// ------------------------------------------------------------ reading a mesh

		FMeshDescription* SourceOf(UObject& Object)
		{
			if (UStaticMesh* Static = Cast<UStaticMesh>(&Object))
			{
				return Static->GetNumSourceModels() > 0 && Static->IsMeshDescriptionValid(0) ? Static->GetMeshDescription(0) : nullptr;
			}
			if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(&Object))
			{
				return Skeletal->HasMeshDescription(0) ? Skeletal->GetMeshDescription(0) : nullptr;
			}
			return nullptr;
		}

		/** Vertices welded by position, so a UV seam is not an edge of the surface. */
		struct FWelded
		{
			TArray<int32> Tids;
			TArray<FIntVector> Tris;
			TArray<FVector3d> Points;
			double Diagonal = 0.0;
		};

		FWelded Weld(const FDynamicMesh3& Mesh)
		{
			FWelded Out;
			const FAxisAlignedBox3d Bounds = Mesh.GetBounds();
			Out.Diagonal = Bounds.IsEmpty() ? 0.0 : Bounds.DiagonalLength();
			const double Cell = FMath::Max(Out.Diagonal * 1e-6, 1e-6);
			TMap<FIntVector, int32> Ids;
			TArray<int32> Map;
			Map.Init(INDEX_NONE, Mesh.MaxVertexID());
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				const FVector3d P = Mesh.GetVertex(Vid);
				const FIntVector Key(FMath::RoundToInt(P.X / Cell), FMath::RoundToInt(P.Y / Cell), FMath::RoundToInt(P.Z / Cell));
				int32* Found = Ids.Find(Key);
				if (!Found)
				{
					Found = &Ids.Add(Key, Out.Points.Add(P));
				}
				Map[Vid] = *Found;
			}
			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				const FIndex3i T = Mesh.GetTriangle(Tid);
				Out.Tids.Add(Tid);
				Out.Tris.Add(FIntVector(Map[T.A], Map[T.B], Map[T.C]));
			}
			return Out;
		}

		struct FUnion
		{
			TArray<int32> Parent;
			explicit FUnion(const int32 Count) { Parent.SetNumUninitialized(Count); for (int32 I = 0; I < Count; ++I) Parent[I] = I; }
			int32 Find(int32 X) { while (Parent[X] != X) { Parent[X] = Parent[Parent[X]]; X = Parent[X]; } return X; }
			void Join(const int32 A, const int32 B) { const int32 RA = Find(A), RB = Find(B); if (RA != RB) Parent[RA] = RB; }
		};

		/** The surface's topology: edges counted and oriented, components, and which triangles are wound against their neighbours. */
		struct FTopology
		{
			int32 BoundaryEdges = 0;
			int32 NonManifoldEdges = 0;
			int32 Holes = 0;
			int32 LargestHole = 0;
			int32 Degenerate = 0;
			/** Index into FWelded::Tris; the component each triangle is in, and how big each is. */
			TArray<int32> Component;
			TArray<int32> ComponentTris;
			TArray<double> ComponentDiagonal;
			/** Triangles wound against the rest of their component, and whole closed components turned inside out. */
			TArray<int32> Flipped;
			int32 InsideOut = 0;
		};

		FTopology Analyse(const FWelded& Welded)
		{
			FTopology Out;
			const int32 Count = Welded.Tris.Num();
			struct FEdge { int32 Count = 0; int32 TriA = INDEX_NONE; int32 TriB = INDEX_NONE; bool bSameDirection = false; int32 FirstDir = 0; };
			TMap<uint64, FEdge> Edges;
			Edges.Reserve(Count * 2);
			const double AreaFloor = FMath::Square(Welded.Diagonal) * 1e-12;
			for (int32 T = 0; T < Count; ++T)
			{
				const FIntVector& Tri = Welded.Tris[T];
				const FVector3d A = Welded.Points[Tri.X], B = Welded.Points[Tri.Y], C = Welded.Points[Tri.Z];
				if (Tri.X == Tri.Y || Tri.Y == Tri.Z || Tri.X == Tri.Z || 0.5 * ((B - A).Cross(C - A)).Length() <= AreaFloor)
				{
					++Out.Degenerate;
				}
				const int32 V[3] = {Tri.X, Tri.Y, Tri.Z};
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const int32 From = V[Corner], To = V[(Corner + 1) % 3];
					if (From == To)
					{
						continue;
					}
					const uint64 Key = (uint64(FMath::Min(From, To)) << 32) | uint64(FMath::Max(From, To));
					const int32 Dir = From < To ? 1 : -1;
					FEdge& Edge = Edges.FindOrAdd(Key);
					if (Edge.Count == 0)
					{
						Edge.TriA = T;
						Edge.FirstDir = Dir;
					}
					else if (Edge.Count == 1)
					{
						Edge.TriB = T;
						Edge.bSameDirection = Dir == Edge.FirstDir;
					}
					++Edge.Count;
				}
			}
			// Components over shared positions; adjacency over manifold edges for winding.
			FUnion Union(Welded.Points.Num());
			for (const FIntVector& Tri : Welded.Tris)
			{
				Union.Join(Tri.X, Tri.Y);
				Union.Join(Tri.Y, Tri.Z);
			}
			TArray<TArray<TPair<int32, bool>>> Neighbours;
			Neighbours.SetNum(Count);
			FUnion Loops(Welded.Points.Num());
			TSet<int32> BoundaryVertices;
			for (const TPair<uint64, FEdge>& Pair : Edges)
			{
				const FEdge& Edge = Pair.Value;
				if (Edge.Count == 1)
				{
					++Out.BoundaryEdges;
					const int32 A = int32(Pair.Key >> 32), B = int32(Pair.Key & 0xffffffffu);
					Loops.Join(A, B);
					BoundaryVertices.Add(A);
					BoundaryVertices.Add(B);
				}
				else if (Edge.Count > 2)
				{
					++Out.NonManifoldEdges;
				}
				else
				{
					Neighbours[Edge.TriA].Add({Edge.TriB, Edge.bSameDirection});
					Neighbours[Edge.TriB].Add({Edge.TriA, Edge.bSameDirection});
				}
			}
			TMap<int32, int32> LoopSizes;
			for (const int32 Vertex : BoundaryVertices)
			{
				++LoopSizes.FindOrAdd(Loops.Find(Vertex));
			}
			Out.Holes = LoopSizes.Num();
			for (const TPair<int32, int32>& Loop : LoopSizes)
			{
				Out.LargestHole = FMath::Max(Out.LargestHole, Loop.Value);
			}
			TMap<int32, int32> ComponentIds;
			Out.Component.SetNumUninitialized(Count);
			TArray<FBox> ComponentBounds;
			for (int32 T = 0; T < Count; ++T)
			{
				const int32 Root = Union.Find(Welded.Tris[T].X);
				int32* Id = ComponentIds.Find(Root);
				if (!Id)
				{
					Id = &ComponentIds.Add(Root, Out.ComponentTris.Add(0));
					ComponentBounds.Add(FBox(ForceInit));
				}
				Out.Component[T] = *Id;
				++Out.ComponentTris[*Id];
				for (const int32 V : {Welded.Tris[T].X, Welded.Tris[T].Y, Welded.Tris[T].Z})
				{
					ComponentBounds[*Id] += FVector(Welded.Points[V]);
				}
			}
			for (const FBox& Box : ComponentBounds)
			{
				Out.ComponentDiagonal.Add(Box.IsValid ? (Box.Max - Box.Min).Size() : 0.0);
			}
			// Winding: walk each component, flipping a neighbour's parity when the shared edge runs the same way in both.
			TArray<int8> Parity;
			Parity.Init(-1, Count);
			for (int32 Seed = 0; Seed < Count; ++Seed)
			{
				if (Parity[Seed] != -1)
				{
					continue;
				}
				TArray<int32> Members;
				TArray<int32> Stack = {Seed};
				Parity[Seed] = 0;
				while (!Stack.IsEmpty())
				{
					const int32 T = Stack.Pop(EAllowShrinking::No);
					Members.Add(T);
					for (const TPair<int32, bool>& Next : Neighbours[T])
					{
						if (Parity[Next.Key] == -1)
						{
							Parity[Next.Key] = Next.Value ? int8(1 - Parity[T]) : Parity[T];
							Stack.Add(Next.Key);
						}
					}
				}
				int32 Odd = 0;
				for (const int32 T : Members)
				{
					Odd += Parity[T];
				}
				// The minority is the flipped one; then a closed patch whose volume has the wrong sign is inside out.
				const int8 Bad = Odd * 2 > Members.Num() ? 0 : 1;
				double Volume = 0.0;
				bool bClosed = true;
				for (const int32 T : Members)
				{
					const FIntVector& Tri = Welded.Tris[T];
					FVector3d A = Welded.Points[Tri.X], B = Welded.Points[Tri.Y], C = Welded.Points[Tri.Z];
					if (Parity[T] == Bad)
					{
						Swap(B, C);
					}
					Volume += A.Dot(B.Cross(C)) / 6.0;
				}
				for (const int32 T : Members)
				{
					bClosed &= Neighbours[T].Num() == 3;
				}
				// Unreal winds outward faces the other way round (its normal is (C-A)x(B-A), see VectorUtil::Normal), so an
				// outward closed part comes out with a negative volume by this sum: a positive one is inside out.
				if (bClosed && Volume > 0.0)
				{
					++Out.InsideOut;
					for (const int32 T : Members)
					{
						if (Parity[T] != Bad)
						{
							Out.Flipped.Add(T);
						}
					}
				}
				else
				{
					for (const int32 T : Members)
					{
						if (Parity[T] == Bad)
						{
							Out.Flipped.Add(T);
						}
					}
				}
			}
			return Out;
		}

		/** Components small enough to be stray bits: under 2% of the triangles and a tenth of the model's size. */
		TArray<int32> SmallComponents(const FTopology& Topology, const int32 Triangles, const double Diagonal)
		{
			TArray<int32> Out;
			int32 Largest = 0;
			for (int32 Id = 0; Id < Topology.ComponentTris.Num(); ++Id)
			{
				Largest = Topology.ComponentTris[Id] > Topology.ComponentTris[Largest] ? Id : Largest;
			}
			for (int32 Id = 0; Id < Topology.ComponentTris.Num(); ++Id)
			{
				if (Id != Largest && Topology.ComponentTris[Id] < FMath::Max(1, Triangles / 50) && Topology.ComponentDiagonal[Id] < Diagonal * 0.1)
				{
					Out.Add(Id);
				}
			}
			return Out;
		}

		// ------------------------------------------------------------ textures

		enum class ERole : uint8 { BaseColor, Normal, Roughness, Metallic, AO, MetalRough, ORM, Emissive, Other };

		const TCHAR* RoleName(const ERole Role)
		{
			switch (Role)
			{
			case ERole::BaseColor: return TEXT("base_color");
			case ERole::Normal: return TEXT("normal");
			case ERole::Roughness: return TEXT("roughness");
			case ERole::Metallic: return TEXT("metallic");
			case ERole::AO: return TEXT("occlusion");
			case ERole::MetalRough: return TEXT("metallic_roughness");
			case ERole::ORM: return TEXT("orm");
			case ERole::Emissive: return TEXT("emissive");
			default: return TEXT("other");
			}
		}

		ERole RoleOf(const FString& Name)
		{
			const FString Text = Name.ToLower().Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));
			// glTF's extension maps (Interchange names them ClearCoatRoughnessTexture, SpecularColorTexture...) are not the PBR set.
			if (Text.Contains(TEXT("clearcoat")) || Text.Contains(TEXT("specular")) || Text.Contains(TEXT("sheen")) || Text.Contains(TEXT("transmission"))
				|| Text.Contains(TEXT("iridescence")) || Text.Contains(TEXT("anisotropy")) || Text.Contains(TEXT("thickness"))) return ERole::Other;
			if (Text.Contains(TEXT("orm")) && !Text.Contains(TEXT("normal")) && !Text.Contains(TEXT("form"))) return ERole::ORM;
			if (Text.Contains(TEXT("occlusionroughnessmetal"))) return ERole::ORM;
			if (Text.Contains(TEXT("metallicroughness")) || Text.Contains(TEXT("metalrough")) || Text.Contains(TEXT("roughnessmetal"))) return ERole::MetalRough;
			if (Text.Contains(TEXT("normal")) || Text.EndsWith(TEXT("nrm"))) return Text.Contains(TEXT("detail")) ? ERole::Other : ERole::Normal;
			if (Text.Contains(TEXT("basecolor")) || Text.Contains(TEXT("albedo")) || Text.Contains(TEXT("diffuse")) || Text.Contains(TEXT("color")) || Text.EndsWith(TEXT("bc"))) return ERole::BaseColor;
			if (Text.Contains(TEXT("rough"))) return ERole::Roughness;
			if (Text.Contains(TEXT("metal"))) return ERole::Metallic;
			if (Text.Contains(TEXT("occlusion")) || Text.EndsWith(TEXT("ao"))) return ERole::AO;
			if (Text.Contains(TEXT("emissive")) || Text.Contains(TEXT("emission"))) return ERole::Emissive;
			return ERole::Other;
		}

		struct FSlot
		{
			FName Name;
			UMaterialInterface* Material = nullptr;
			TMap<ERole, UTexture2D*> Maps;
			bool bOurs = false;
		};

		TArray<FSlot> SlotsOf(UObject& Mesh)
		{
			TArray<FSlot> Out;
			auto Read = [](FSlot& Slot)
			{
				if (!Slot.Material)
				{
					return;
				}
				const UMaterialInterface* Base = Slot.Material->GetMaterial();
				Slot.bOurs = Base && (Base->GetPathName().StartsWith(MasterPath) || Base->GetPathName().StartsWith(TEXT("/Game/HyperAI/Materials/M_HyperAI_BladeSteel_Clean")));
				if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Slot.Material))
				{
					for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
					{
						UTexture2D* Texture = Cast<UTexture2D>(Value.ParameterValue);
						const ERole Role = RoleOf(Value.ParameterInfo.Name.ToString());
						if (Texture && Role != ERole::Other && !Slot.Maps.Contains(Role))
						{
							Slot.Maps.Add(Role, Texture);
						}
					}
				}
				if (Slot.Maps.IsEmpty())
				{
					TArray<UTexture*> Used;
					Slot.Material->GetUsedTextures(Used);
					for (UTexture* Texture : Used)
					{
						UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
						if (!Texture2D || Texture2D->GetPathName().StartsWith(TEXT("/Engine/")))
						{
							continue;
						}
						const ERole Role = RoleOf(Texture2D->GetName());
						if (Role != ERole::Other && !Slot.Maps.Contains(Role))
						{
							Slot.Maps.Add(Role, Texture2D);
						}
					}
				}
			};
			if (UStaticMesh* Static = Cast<UStaticMesh>(&Mesh))
			{
				for (const FStaticMaterial& Material : Static->GetStaticMaterials())
				{
					FSlot& Slot = Out.AddDefaulted_GetRef();
					Slot.Name = Material.MaterialSlotName;
					Slot.Material = Material.MaterialInterface;
					Read(Slot);
				}
			}
			else if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(&Mesh))
			{
				for (const FSkeletalMaterial& Material : Skeletal->GetMaterials())
				{
					FSlot& Slot = Out.AddDefaulted_GetRef();
					Slot.Name = Material.MaterialSlotName;
					Slot.Material = Material.MaterialInterface;
					Read(Slot);
				}
			}
			return Out;
		}

		/** A texture's top mip as raw 8-bit BGRA, the bytes as stored, whatever its sRGB flag says; resized when asked. */
		bool RawPixels(UTexture2D& Texture, FImage& Out, const int32 Width = 0, const int32 Height = 0)
		{
			FImage Source;
			if (!Texture.Source.IsValid() || !Texture.Source.GetMipImage(Source, 0))
			{
				return false;
			}
			Source.GammaSpace = EGammaSpace::Linear;
			if (Width > 0 && Height > 0 && (Source.SizeX != Width || Source.SizeY != Height))
			{
				Source.ResizeTo(Out, Width, Height, ERawImageFormat::BGRA8, EGammaSpace::Linear);
			}
			else
			{
				Source.CopyTo(Out, ERawImageFormat::BGRA8, EGammaSpace::Linear);
			}
			return Out.RawData.Num() >= int64(Out.SizeX) * Out.SizeY * 4;
		}

		struct FStats
		{
			/** Mean of each channel, 0-1, in R G B order. */
			FVector3d Mean = FVector3d::ZeroVector;
			/** The share of pixels with each channel between 0.25 and 0.75. */
			FVector3d Mid = FVector3d::ZeroVector;
			FVector3d Min = FVector3d(1.0);
			FVector3d Max = FVector3d::ZeroVector;
		};

		bool StatsOf(UTexture2D& Texture, FStats& Out)
		{
			FImage Small;
			if (!RawPixels(Texture, Small, 128, 128))
			{
				return false;
			}
			const int64 Pixels = int64(Small.SizeX) * Small.SizeY;
			const uint8* Data = Small.RawData.GetData();
			for (int64 Index = 0; Index < Pixels; ++Index)
			{
				const FVector3d Value(Data[Index * 4 + 2] / 255.0, Data[Index * 4 + 1] / 255.0, Data[Index * 4 + 0] / 255.0);
				Out.Mean += Value;
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					Out.Mid[Axis] += Value[Axis] > 0.25 && Value[Axis] < 0.75 ? 1.0 : 0.0;
					Out.Min[Axis] = FMath::Min(Out.Min[Axis], Value[Axis]);
					Out.Max[Axis] = FMath::Max(Out.Max[Axis], Value[Axis]);
				}
			}
			Out.Mean /= double(FMath::Max<int64>(Pixels, 1));
			Out.Mid /= double(FMath::Max<int64>(Pixels, 1));
			return true;
		}

		FHyperAIModelCheck& Add(FHyperAIModelCheckReport& Report, const TCHAR* Id, const TCHAR* Result, const FString& Value, const FString& Expected,
			const FString& Detail, const TCHAR* LocalFix = TEXT(""), const TCHAR* PaidFix = TEXT(""))
		{
			FHyperAIModelCheck& Check = Report.Checks.AddDefaulted_GetRef();
			Check.Id = Id;
			Check.Result = Result;
			Check.Value = Value;
			Check.Expected = Expected;
			Check.Detail = Detail;
			Check.LocalFix = LocalFix;
			Check.PaidFix = PaidFix;
			return Check;
		}

		bool Upright(const FString& Use)
		{
			return Use == TEXT("character") || Use == TEXT("creature") || Use == TEXT("foliage");
		}

		// ------------------------------------------------------------ the visual check

		void SetString(const UStruct& Struct, void* Data, const TCHAR* Name, const FString& Value)
		{
			if (const FStrProperty* Property = CastField<FStrProperty>(Struct.FindPropertyByName(Name)))
			{
				Property->SetPropertyValue_InContainer(Data, Value);
			}
		}

		/** hyper_image_compare, reached through reflection: the lighting pack that owns it is optional. -1 when it is not there. */
		float CompareImages(const FString& Reference, const FString& Candidate, FString& OutNote)
		{
			UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/HyperAIStudioLighting.HyperAIStudioLightingToolset"));
			UFunction* Function = Class ? Class->FindFunctionByName(TEXT("hyper_image_compare")) : nullptr;
			const FStructProperty* RequestProperty = Function ? CastField<FStructProperty>(Function->FindPropertyByName(TEXT("Request"))) : nullptr;
			const FStructProperty* ReturnProperty = Function ? CastField<FStructProperty>(Function->GetReturnProperty()) : nullptr;
			if (!RequestProperty || !ReturnProperty)
			{
				OutNote = TEXT("hyper_image_compare (the lighting pack) is not loaded");
				return -1.f;
			}
			TArray<uint8> Parameters;
			Parameters.SetNumZeroed(Function->ParmsSize);
			Function->InitializeStruct(Parameters.GetData());
			void* Request = RequestProperty->ContainerPtrToValuePtr<void>(Parameters.GetData());
			SetString(*RequestProperty->Struct, Request, TEXT("ReferenceImagePath"), Reference);
			SetString(*RequestProperty->Struct, Request, TEXT("CandidateImagePath"), Candidate);
			SetString(*RequestProperty->Struct, Request, TEXT("Mode"), TEXT("look"));
			Class->GetDefaultObject()->ProcessEvent(Function, Parameters.GetData());
			const void* Result = ReturnProperty->ContainerPtrToValuePtr<void>(Parameters.GetData());
			float Score = -1.f;
			const FBoolProperty* Ok = CastField<FBoolProperty>(ReturnProperty->Struct->FindPropertyByName(TEXT("bOk")));
			const FFloatProperty* ScoreProperty = CastField<FFloatProperty>(ReturnProperty->Struct->FindPropertyByName(TEXT("Score")));
			const FStrProperty* Diagnostic = CastField<FStrProperty>(ReturnProperty->Struct->FindPropertyByName(TEXT("Diagnostic")));
			if (Ok && ScoreProperty && Ok->GetPropertyValue_InContainer(Result))
			{
				Score = ScoreProperty->GetPropertyValue_InContainer(Result);
			}
			else if (Diagnostic)
			{
				OutNote = Diagnostic->GetPropertyValue_InContainer(Result).Left(200);
			}
			Function->DestroyStruct(Parameters.GetData());
			return Score;
		}

		/**
		 * The editor's thumbnail renderer draws the asset, textured, into a PNG. A Nanite mesh is drawn from a transient copy
		 * built without Nanite: a thumbnail of the Nanite mesh itself showed its coarse fallback or first streamed pages and,
		 * until its materials were compiled for Nanite, the default grey, so the visual check compared a grey, blocky clay
		 * model with a textured reference. Every texture and shader is compiled and streamed in first (AlwaysFlush).
		 */
		bool Capture(UObject& Mesh, const FString& File)
		{
			if (!FApp::CanEverRender())
			{
				return false;
			}
			FAssetCompilingManager::Get().FinishAllCompilation();
			// A material whose shaders are not compiled yet renders as the default grey one in the thumbnail.
			for (const FSlot& Slot : SlotsOf(Mesh))
			{
				if (Slot.Material)
				{
					Slot.Material->EnsureIsComplete();
				}
			}
			// A fresh texture shows at a low mip until it streams in: every mip held resident for the capture, then let go.
			TArray<UTexture2D*> Held;
			for (const FSlot& Slot : SlotsOf(Mesh))
			{
				for (const TPair<ERole, UTexture2D*>& Map : Slot.Maps)
				{
					if (Map.Value && !Map.Value->bForceMiplevelsToBeResident)
					{
						Map.Value->bForceMiplevelsToBeResident = true;
						Map.Value->WaitForStreaming();
						Held.Add(Map.Value);
					}
				}
			}
			ON_SCOPE_EXIT
			{
				for (UTexture2D* Texture : Held)
				{
					Texture->bForceMiplevelsToBeResident = false;
				}
			};
			UObject* Drawn = &Mesh;
			TStrongObjectPtr<UStaticMesh> Copy;
			const UStaticMesh* Static = Cast<UStaticMesh>(&Mesh);
			if (Static && Static->GetNaniteSettings().bEnabled && Static->IsMeshDescriptionValid(0))
			{
				Copy.Reset(DuplicateObject<UStaticMesh>(Static, GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("HyperAICapture"))));
				FMeshNaniteSettings Nanite = Copy->GetNaniteSettings();
				Nanite.bEnabled = false;
				Copy->SetNaniteSettings(Nanite);
				Copy->Build(/*bInSilent=*/true);
				FAssetCompilingManager::Get().FinishAllCompilation();
				Drawn = Copy.Get();
			}
			FObjectThumbnail Thumbnail;
			ThumbnailTools::RenderThumbnail(Drawn, 512, 512, ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush, nullptr, &Thumbnail);
			if (Copy.IsValid())
			{
				Copy->ClearFlags(RF_Standalone | RF_Public);
				Copy->MarkAsGarbage();
				Copy.Reset();
			}
			const TArray<uint8>& Data = Thumbnail.GetUncompressedImageData();
			const int32 Width = Thumbnail.GetImageWidth(), Height = Thumbnail.GetImageHeight();
			if (Width <= 0 || Height <= 0 || Data.Num() < Width * Height * 4)
			{
				return false;
			}
			TArray<uint8> Opaque = Data;
			for (int32 Index = 3; Index < Opaque.Num(); Index += 4)
			{
				Opaque[Index] = 255;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
			return FImageUtils::SaveImageByExtension(*File, FImageView(Opaque.GetData(), Width, Height, ERawImageFormat::BGRA8));
		}

		bool IsPicture(const FString& File)
		{
			const FString Extension = FPaths::GetExtension(File).ToLower();
			return (Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg")) && IFileManager::Get().FileSize(*File) > 0;
		}

		// ------------------------------------------------------------ writing a mesh

		bool Read(UStaticMesh& Mesh, FDynamicMesh3& Out, FString& OutError)
		{
			const FMeshDescription* Source = Mesh.GetNumSourceModels() > 0 && Mesh.IsMeshDescriptionValid(0) ? Mesh.GetMeshDescription(0) : nullptr;
			if (!Source)
			{
				OutError = TEXT("it has no LOD 0 source mesh to change");
				return false;
			}
			FMeshDescriptionToDynamicMesh Converter;
			Converter.Convert(Source, Out, /*bCopyTangents=*/false);
			return Out.TriangleCount() > 0;
		}

		/** As the modelling pack writes a LOD: the conversion rebuilds polygon groups from material ids, so slot names come back. */
		bool Write(UStaticMesh& Mesh, const FDynamicMesh3& Edited, const TMap<int32, FName>& ExtraSlots, FString& OutError)
		{
			FMeshDescription* Target = Mesh.GetMeshDescription(0);
			if (!Target)
			{
				OutError = TEXT("LOD 0 has no source mesh to write");
				return false;
			}
			TMap<int32, FName> SlotNames = ExtraSlots;
			{
				FStaticMeshConstAttributes Attributes(*Target);
				const TPolygonGroupAttributesConstRef<FName> Names = Attributes.GetPolygonGroupMaterialSlotNames();
				for (const FPolygonGroupID Group : Target->PolygonGroups().GetElementIDs())
				{
					if (!SlotNames.Contains(Group.GetValue()))
					{
						SlotNames.Add(Group.GetValue(), Names.IsValid() ? Names[Group] : NAME_None);
					}
				}
			}
			FMeshDescription Out = *Target;
			FConversionToMeshDescriptionOptions Options;
			FDynamicMeshToMeshDescription Converter(Options);
			Converter.Convert(&Edited, Out, /*bCopyTangents=*/false);
			{
				FStaticMeshAttributes Attributes(Out);
				TPolygonGroupAttributesRef<FName> Names = Attributes.GetPolygonGroupMaterialSlotNames();
				if (Names.IsValid())
				{
					for (const FPolygonGroupID Group : Out.PolygonGroups().GetElementIDs())
					{
						if (const FName* Name = SlotNames.Find(Group.GetValue()))
						{
							Names[Group] = *Name;
						}
					}
				}
			}
			if (Out.Triangles().Num() != Edited.TriangleCount())
			{
				OutError = FString::Printf(TEXT("the mesh came out with %d triangles, not %d"), Out.Triangles().Num(), Edited.TriangleCount());
				return false;
			}
			Mesh.ModifyMeshDescription(0);
			*Target = MoveTemp(Out);
			Mesh.CommitMeshDescription(0);
			return true;
		}

		/**
		 * Quadric simplification to a triangle count: boundaries kept from flipping, bowties split first, and UV seams never
		 * collapsed. A provider's texture atlas is hundreds of small islands; collapsing across their seams (the editor's
		 * standard simplify does) smeared a Tripo sword's dark leather grip white. Seams may slide along themselves only, so
		 * a mesh of many islands may stop above the count. Free, in the editor.
		 */
		template <typename SimplifierType>
		void SimplifyMeshTo(FDynamicMesh3& Mesh, const int32 Triangles)
		{
			// A GLB splits its vertices along UV seams: welded first, so the surface is one piece and the seams are attribute
			// seams the constraints can hold.
			FMergeCoincidentMeshEdges Merge(&Mesh);
			Merge.MergeVertexTolerance = FMath::Max(Mesh.GetBounds().DiagonalLength() * 1e-6, 1e-6);
			Merge.Apply();
			if (Mesh.HasAttributes())
			{
				Mesh.Attributes()->SplitAllBowties();
			}
			SimplifierType Simplifier(&Mesh);
			Simplifier.DEBUG_CHECK_LEVEL = 0;
			Simplifier.bAllowSeamCollapse = false;
			Simplifier.SetEdgeFlipTolerance(1.e-5);
			FMeshConstraints Constraints;
			FMeshConstraintsUtil::ConstrainAllBoundariesAndSeams(Constraints, Mesh, EEdgeRefineFlags::NoFlip, EEdgeRefineFlags::NoConstraint, EEdgeRefineFlags::NoConstraint,
				/*bAllowSeamSplits=*/false, /*bAllowSeamSmoothing=*/true, /*bAllowSeamCollapse=*/false);
			Simplifier.SetExternalConstraints(MoveTemp(Constraints));
			Simplifier.SimplifyToTriangleCount(Triangles);
		}

		/**
		 * How far a mesh's texture mapping is from even, as the mean of |log| of each triangle's UV area over its surface area,
		 * against the whole mesh's ratio. 0 when there is no UV layer. A reduction that smears textures raises it.
		 */
		double UVDistortion(const FDynamicMesh3& Mesh)
		{
			const FDynamicMeshUVOverlay* UVs = Mesh.HasAttributes() && Mesh.Attributes()->NumUVLayers() > 0 ? Mesh.Attributes()->PrimaryUV() : nullptr;
			if (!UVs)
			{
				return 0.0;
			}
			TArray<TPair<double, double>> Areas;
			double UVTotal = 0.0, SurfaceTotal = 0.0;
			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				if (!UVs->IsSetTriangle(Tid))
				{
					continue;
				}
				FVector2f A, B, C;
				UVs->GetTriElements(Tid, A, B, C);
				const double UVArea = 0.5 * FMath::Abs(double((B.X - A.X) * (C.Y - A.Y) - (C.X - A.X) * (B.Y - A.Y)));
				const double Surface = Mesh.GetTriArea(Tid);
				if (UVArea > 0.0 && Surface > 0.0)
				{
					Areas.Add({UVArea, Surface});
					UVTotal += UVArea;
					SurfaceTotal += Surface;
				}
			}
			if (Areas.IsEmpty() || UVTotal <= 0.0 || SurfaceTotal <= 0.0)
			{
				return 0.0;
			}
			const double Ratio = UVTotal / SurfaceTotal;
			double Sum = 0.0, Weight = 0.0;
			for (const TPair<double, double>& Each : Areas)
			{
				Sum += FMath::Abs(FMath::Loge((Each.Key / Each.Value) / Ratio)) * Each.Value;
				Weight += Each.Value;
			}
			return Sum / Weight;
		}

		/**
		 * Reduces to Triangles keeping UV seams, then checks the textures still fit: when the mapping's distortion grew by
		 * more than a quarter, the reduction is undone and the mesh kept as it was. Notes say which. True when reduced.
		 */
		bool ReduceKeepingTextures(FDynamicMesh3& Mesh, const int32 Triangles, const TCHAR* Label, TArray<FString>& OutNotes)
		{
			const int32 From = Mesh.TriangleCount();
			const FDynamicMesh3 Kept = Mesh;
			const double Before = UVDistortion(Mesh);
			if (Mesh.HasAttributes() && Mesh.Attributes()->PrimaryNormals())
			{
				SimplifyMeshTo<FAttrMeshSimplification>(Mesh, Triangles);
			}
			else
			{
				SimplifyMeshTo<FQEMSimplification>(Mesh, Triangles);
			}
			const double After = UVDistortion(Mesh);
			if (After > Before + 0.25)
			{
				Mesh = Kept;
				OutNotes.Add(FString::Printf(TEXT("%s: a reduction from %d to %d triangles stretched the texture mapping (distortion %.2f, from %.2f), so it was undone; the mesh keeps %d triangles."),
					Label, From, Triangles, After, Before, From));
				return false;
			}
			OutNotes.Add(FString::Printf(TEXT("%s: reduced from %d to %d triangles in the editor, UV seams kept (texture mapping distortion %.2f, was %.2f), free."), Label, From, Mesh.TriangleCount(),
				After, Before));
			return true;
		}

		/** Every triangle of the given welded components, as dynamic-mesh triangle ids. */
		TArray<int32> TrianglesOf(const FWelded& Welded, const FTopology& Topology, const TSet<int32>& Components)
		{
			TArray<int32> Out;
			for (int32 Index = 0; Index < Welded.Tids.Num(); ++Index)
			{
				if (Components.Contains(Topology.Component[Index]))
				{
					Out.Add(Welded.Tids[Index]);
				}
			}
			return Out;
		}

		int32 FillHoles(FDynamicMesh3& Mesh, int32& OutLeft)
		{
			OutLeft = 0;
			// Weld the seams first, so only real openings are boundaries; the attribute seams stay.
			FMergeCoincidentMeshEdges Merge(&Mesh);
			Merge.MergeVertexTolerance = FMath::Max(Mesh.GetBounds().DiagonalLength() * 1e-6, 1e-6);
			Merge.Apply();
			FMeshBoundaryLoops Loops(&Mesh, /*bAutoCompute=*/true);
			int32 Filled = 0;
			FDynamicMeshUVEditor UVs(&Mesh, 0, /*bCreateIfMissing=*/false);
			for (const FEdgeLoop& Loop : Loops.Loops)
			{
				if (Loop.Edges.Num() > MaxFillEdges)
				{
					++OutLeft;
					continue;
				}
				// The material of the triangles around the hole.
				int32 Material = 0;
				if (Mesh.HasAttributes() && Mesh.Attributes()->HasMaterialID() && !Loop.Edges.IsEmpty())
				{
					const FIndex2i EdgeTris = Mesh.GetEdgeT(Loop.Edges[0]);
					Material = EdgeTris.A >= 0 ? Mesh.Attributes()->GetMaterialID()->GetValue(EdgeTris.A) : 0;
				}
				// A fan from the hole's centre: no sliver triangles along straight edges, as ear clipping makes.
				FSimpleHoleFiller Filler(&Mesh, Loop, FSimpleHoleFiller::EFillType::TriangleFan);
				if (Filler.Validate() != EOperationValidationResult::Ok || !Filler.Fill())
				{
					++OutLeft;
					continue;
				}
				++Filled;
				if (Mesh.HasAttributes())
				{
					if (Mesh.Attributes()->HasMaterialID())
					{
						for (const int32 Tid : Filler.NewTriangles)
						{
							Mesh.Attributes()->GetMaterialID()->SetValue(Tid, Material);
						}
					}
					if (FDynamicMeshNormalOverlay* Normals = Mesh.Attributes()->PrimaryNormals())
					{
						FMeshNormals::InitializeOverlayRegionToPerVertexNormals(Normals, Filler.NewTriangles);
					}
					if (Mesh.Attributes()->NumUVLayers() > 0)
					{
						// A plane through the hole; the cap is small, so a planar projection is enough.
						FVector3d Centre = FVector3d::Zero();
						FVector3d Normal = FVector3d::Zero();
						FAxisAlignedBox3d Box = FAxisAlignedBox3d::Empty();
						for (const int32 Vid : Loop.Vertices)
						{
							Centre += Mesh.GetVertex(Vid);
							Box.Contain(Mesh.GetVertex(Vid));
						}
						Centre /= double(FMath::Max(1, Loop.Vertices.Num()));
						for (const int32 Tid : Filler.NewTriangles)
						{
							Normal += Mesh.GetTriNormal(Tid);
						}
						const FFrame3d Frame(Centre, Normal.IsNearlyZero() ? FVector3d::UnitZ() : Normal.GetSafeNormal());
						const double Extent = FMath::Max(Box.MaxDim(), 1e-3);
						UVs.SetTriangleUVsFromPlanarProjection(Filler.NewTriangles, [](const FVector3d& P) { return P; }, Frame, FVector2d(Extent, Extent));
						// The cap's island goes into a corner a texel or two wide, so it does not lie over the model's own islands.
						FDynamicMeshUVOverlay* Overlay = Mesh.Attributes()->PrimaryUV();
						TSet<int32> Elements;
						for (const int32 Tid : Filler.NewTriangles)
						{
							if (Overlay->IsSetTriangle(Tid))
							{
								const FIndex3i E = Overlay->GetTriangle(Tid);
								Elements.Append({E.A, E.B, E.C});
							}
						}
						FVector2f Min(FLT_MAX), Max(-FLT_MAX);
						for (const int32 Element : Elements)
						{
							Min = FVector2f::Min(Min, Overlay->GetElement(Element));
							Max = FVector2f::Max(Max, Overlay->GetElement(Element));
						}
						const float Span = FMath::Max(FMath::Max(Max.X - Min.X, Max.Y - Min.Y), 1e-6f);
						for (const int32 Element : Elements)
						{
							Overlay->SetElement(Element, (Overlay->GetElement(Element) - Min) / Span * (1.f / 512.f));
						}
					}
				}
			}
			return Filled;
		}

		/** One UV layout for the whole mesh, from xatlas: only for a mesh with no textures to lose. */
		bool AutoUV(FDynamicMesh3& Mesh, FString& OutError)
		{
			if (!Mesh.HasAttributes())
			{
				Mesh.EnableAttributes();
			}
			if (Mesh.Attributes()->NumUVLayers() == 0)
			{
				Mesh.Attributes()->SetNumUVLayers(1);
			}
			TArray<int32> VertexIndex;
			VertexIndex.Init(INDEX_NONE, Mesh.MaxVertexID());
			TArray<FVector3f> Vertices;
			TArray<int32> Indices;
			TArray<int32> Tids;
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				VertexIndex[Vid] = Vertices.Add(FVector3f(Mesh.GetVertex(Vid)));
			}
			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				const FIndex3i T = Mesh.GetTriangle(Tid);
				Indices.Append({VertexIndex[T.A], VertexIndex[T.B], VertexIndex[T.C]});
				Tids.Add(Tid);
			}
			XAtlasWrapper::XAtlasChartOptions Charts;
			XAtlasWrapper::XAtlasPackOptions Pack;
			Pack.Resolution = 1024;
			Pack.Padding = 2;
			TArray<FVector2D> UVVertices;
			TArray<int32> UVIndices;
			TArray<int32> Remap;
			if (!XAtlasWrapper::ComputeUVs(Indices, Vertices, Charts, Pack, UVVertices, UVIndices, Remap) || UVIndices.Num() != Indices.Num())
			{
				OutError = TEXT("xatlas could not unwrap it");
				return false;
			}
			FDynamicMeshUVOverlay* Overlay = Mesh.Attributes()->PrimaryUV();
			Overlay->ClearElements();
			TArray<int32> Elements;
			for (const FVector2D& UV : UVVertices)
			{
				Elements.Add(Overlay->AppendElement(FVector2f(UV)));
			}
			for (int32 Index = 0; Index < Tids.Num(); ++Index)
			{
				Overlay->SetTriangle(Tids[Index], FIndex3i(Elements[UVIndices[Index * 3]], Elements[UVIndices[Index * 3 + 1]], Elements[UVIndices[Index * 3 + 2]]));
			}
			return true;
		}

		// ------------------------------------------------------------ materials

		template <typename T>
		T* FindOrLoad(const FString& Package, UObject*& OutOther)
		{
			OutOther = nullptr;
			const FString ObjectPath = Package + TEXT(".") + FPackageName::GetShortName(Package);
			UObject* Found = FindObject<UObject>(nullptr, *ObjectPath);
			if (!Found && FPackageName::DoesPackageExist(Package))
			{
				Found = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
			T* Typed = Cast<T>(Found);
			if (Found && !Typed)
			{
				OutOther = Found;
			}
			return Typed;
		}

		/** A 4x4 linear white mask, made once beside the master: occlusion, roughness and metal all 1 until an ORM is set. */
		UTexture2D* WhiteMask(FString& OutError)
		{
			UObject* Other = nullptr;
			if (UTexture2D* Found = FindOrLoad<UTexture2D>(WhiteMaskPath, Other))
			{
				return Found;
			}
			if (Other)
			{
				OutError = FString::Printf(TEXT("%s exists but is not a texture."), WhiteMaskPath);
				return nullptr;
			}
			TArray<uint8> Pixels;
			Pixels.Init(255, 4 * 4 * 4);
			UTexture2D* Texture = NewObject<UTexture2D>(CreatePackage(WhiteMaskPath), *FPackageName::GetShortName(WhiteMaskPath), RF_Public | RF_Standalone);
			Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, Pixels.GetData());
			Texture->SRGB = false;
			Texture->CompressionSettings = TC_Masks;
			Texture->PostEditChange();
			FAssetRegistryModule::AssetCreated(Texture);
			Texture->MarkPackageDirty();
			return FHyperAIStudioSourceControl::SaveAsset(*Texture, OutError) ? Texture : nullptr;
		}

		/**
		 * A master made before 31 October 2026 gave its ORM sampler (Masks) the engine's sRGB white square: the material does
		 * not compile, and every instance of it draws as the default grey material. Its default becomes the linear white mask.
		 */
		void RepairMaster(UMaterial& Material)
		{
			UMaterialExpressionTextureSampleParameter2D* Broken = nullptr;
			for (UMaterialExpression* Expression : Material.GetExpressions())
			{
				UMaterialExpressionTextureSampleParameter2D* Sample = Cast<UMaterialExpressionTextureSampleParameter2D>(Expression);
				if (Sample && Sample->SamplerType == SAMPLERTYPE_Masks && Sample->Texture && Sample->Texture->SRGB)
				{
					Broken = Sample;
				}
			}
			FString Error;
			UTexture2D* Mask = Broken ? WhiteMask(Error) : nullptr;
			if (!Mask)
			{
				return;
			}
			Material.Modify();
			Broken->Modify();
			Broken->Texture = Mask;
			Material.PostEditChange();
			Material.MarkPackageDirty();
			FHyperAIStudioSourceControl::SaveAsset(Material, Error);
		}

		UMaterialInterface* Master(FString& OutError)
		{
			UObject* Other = nullptr;
			if (UMaterialInterface* Found = FindOrLoad<UMaterialInterface>(MasterPath, Other))
			{
				if (UMaterial* Base = Cast<UMaterial>(Found))
				{
					RepairMaster(*Base);
				}
				return Found;
			}
			if (Other)
			{
				OutError = FString::Printf(TEXT("%s exists but is not a material; move it, and the master is made again."), MasterPath);
				return nullptr;
			}
			UTexture2D* Mask = WhiteMask(OutError);
			if (!Mask)
			{
				return nullptr;
			}
			UTexture* White = LoadObject<UTexture>(nullptr, WhiteTexture, nullptr, LOAD_NoWarn | LOAD_Quiet);
			UTexture* Flat = LoadObject<UTexture>(nullptr, FlatNormalTexture, nullptr, LOAD_NoWarn | LOAD_Quiet);
			TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
			UPackage* Package = CreatePackage(MasterPath);
			UMaterial* Material = NewObject<UMaterial>(Package, *FPackageName::GetShortName(MasterPath), RF_Public | RF_Standalone);
			TArray<UMaterialExpression*> All;
			auto Scalar = [&](const TCHAR* Name, const float Default)
			{
				UMaterialExpressionScalarParameter* Value = NewObject<UMaterialExpressionScalarParameter>(Material);
				Value->ParameterName = Name;
				Value->DefaultValue = Default;
				All.Add(Value);
				return Value;
			};
			auto Multiply = [&](UMaterialExpression* A, const int32 OutA, UMaterialExpression* B, const int32 OutB)
			{
				UMaterialExpressionMultiply* Product = NewObject<UMaterialExpressionMultiply>(Material);
				Product->A.Connect(OutA, A);
				Product->B.Connect(OutB, B);
				All.Add(Product);
				return Product;
			};
			auto Texture = [&](const TCHAR* Name, UTexture* Default, const EMaterialSamplerType Sampler, UMaterialExpression* Coordinates)
			{
				UMaterialExpressionTextureSampleParameter2D* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
				Sample->ParameterName = Name;
				Sample->Texture = Default;
				Sample->SamplerType = Sampler;
				Sample->Coordinates.Connect(0, Coordinates);
				All.Add(Sample);
				return Sample;
			};
			UMaterialExpressionTextureCoordinate* Coordinates = NewObject<UMaterialExpressionTextureCoordinate>(Material);
			All.Add(Coordinates);
			// Tiling scales the UVs (1 for a baked, unique layout); the detail normal tiles on its own.
			UMaterialExpression* Tiled = Multiply(Coordinates, 0, Scalar(TEXT("Tiling"), 1.f), 0);
			UMaterialExpression* DetailTiled = Multiply(Coordinates, 0, Scalar(TEXT("DetailNormalTiling"), 8.f), 0);
			UMaterialExpressionTextureSampleParameter2D* Color = Texture(TEXT("BaseColor"), White, SAMPLERTYPE_Color, Tiled);
			UMaterialExpressionVectorParameter* Tint = NewObject<UMaterialExpressionVectorParameter>(Material);
			Tint->ParameterName = TEXT("BaseColorTint");
			Tint->DefaultValue = FLinearColor::White;
			All.Add(Tint);
			UMaterialExpressionTextureSampleParameter2D* Normal = Texture(TEXT("Normal"), Flat, SAMPLERTYPE_Normal, Tiled);
			UMaterialExpressionTextureSampleParameter2D* Detail = Texture(TEXT("DetailNormal"), Flat, SAMPLERTYPE_Normal, DetailTiled);
			UMaterialExpressionTextureSampleParameter2D* Packed = Texture(TEXT("ORM"), Mask, SAMPLERTYPE_Masks, Tiled);
			// The detail's slope added to the base normal's (a whiteout blend), by DetailNormalStrength (0: off).
			UMaterialExpressionComponentMask* DetailXY = NewObject<UMaterialExpressionComponentMask>(Material);
			DetailXY->R = true;
			DetailXY->G = true;
			DetailXY->Input.Connect(0, Detail);
			All.Add(DetailXY);
			UMaterialExpression* Scaled = Multiply(DetailXY, 0, Scalar(TEXT("DetailNormalStrength"), 0.f), 0);
			UMaterialExpressionConstant* Zero = NewObject<UMaterialExpressionConstant>(Material);
			Zero->R = 0.f;
			All.Add(Zero);
			UMaterialExpressionAppendVector* Slope = NewObject<UMaterialExpressionAppendVector>(Material);
			Slope->A.Connect(0, Scaled);
			Slope->B.Connect(0, Zero);
			All.Add(Slope);
			UMaterialExpressionComponentMask* BaseXYZ = NewObject<UMaterialExpressionComponentMask>(Material);
			BaseXYZ->R = true;
			BaseXYZ->G = true;
			BaseXYZ->B = true;
			BaseXYZ->Input.Connect(0, Normal);
			All.Add(BaseXYZ);
			UMaterialExpressionAdd* Sum = NewObject<UMaterialExpressionAdd>(Material);
			Sum->A.Connect(0, BaseXYZ);
			Sum->B.Connect(0, Slope);
			All.Add(Sum);
			UMaterialExpressionNormalize* Blended = NewObject<UMaterialExpressionNormalize>(Material);
			Blended->VectorInput.Connect(0, Sum);
			All.Add(Blended);
			UMaterialExpression* Tinted = Multiply(Color, 0, Tint, 0);
			// ORM: red occlusion, green roughness, blue metal, each scaled (MetallicScale 0 for a model with no metal map).
			UMaterialExpression* Rough = Multiply(Packed, 2, Scalar(TEXT("RoughnessScale"), 1.f), 0);
			UMaterialExpression* Metal = Multiply(Packed, 3, Scalar(TEXT("MetallicScale"), 1.f), 0);
			int32 Row = 0;
			for (UMaterialExpression* Expression : All)
			{
				Expression->MaterialExpressionEditorX = -900 + (Row % 4) * 220;
				Expression->MaterialExpressionEditorY = (Row / 4) * 200;
				++Row;
				Material->GetExpressionCollection().AddExpression(Expression);
			}
			Material->GetEditorOnlyData()->BaseColor.Connect(0, Tinted);
			Material->GetEditorOnlyData()->Normal.Connect(0, Blended);
			Material->GetEditorOnlyData()->AmbientOcclusion.Connect(1, Packed);
			Material->GetEditorOnlyData()->Roughness.Connect(0, Rough);
			Material->GetEditorOnlyData()->Metallic.Connect(0, Metal);
			Material->PostEditChange();
			FAssetRegistryModule::AssetCreated(Material);
			Material->MarkPackageDirty();
			if (!FHyperAIStudioSourceControl::SaveAsset(*Material, OutError))
			{
				return nullptr;
			}
			return Material;
		}

		/**
		 * A normal map in OpenGL's convention, +Y: glTF's (Tripo's NormalGL maps) by its name or the file it came from.
		 * Unreal reads -Y, so its green must be flipped; Interchange's glTF import does that, other routes may not.
		 */
		bool IsOpenGLNormal(const UTexture2D& Texture)
		{
			const FString Name = Texture.GetName().ToLower();
			if (Name.Contains(TEXT("normalgl")) || Name.EndsWith(TEXT("_gl")))
			{
				return true;
			}
			const FString File = Texture.AssetImportData ? Texture.AssetImportData->GetFirstFilename() : FString();
			const FString Extension = FPaths::GetExtension(File).ToLower();
			return Extension == TEXT("glb") || Extension == TEXT("gltf");
		}

		/** Sets a texture up for what it holds; true when anything changed. */
		bool SetUp(UTexture2D& Texture, const ERole Role)
		{
			const bool bColor = Role == ERole::BaseColor || Role == ERole::Emissive;
			const TextureCompressionSettings Compression = Role == ERole::Normal ? TC_Normalmap : bColor ? TC_Default : TC_Masks;
			const bool bFlip = Role == ERole::Normal ? Texture.bFlipGreenChannel || IsOpenGLNormal(Texture) : Texture.bFlipGreenChannel;
			if (Texture.SRGB == bColor && Texture.CompressionSettings == Compression && Texture.bFlipGreenChannel == bFlip)
			{
				return false;
			}
			Texture.Modify();
			Texture.SRGB = bColor;
			Texture.CompressionSettings = Compression;
			Texture.bFlipGreenChannel = bFlip;
			Texture.PostEditChange();
			return true;
		}

		/** Occlusion in red, roughness in green, metal in blue, from separate maps or glTF's packed metal-roughness. */
		UTexture2D* PackORM(const FSlot& Slot, const FString& Package, FString& OutError)
		{
			UTexture2D* const* Rough = Slot.Maps.Find(ERole::Roughness);
			UTexture2D* const* Metal = Slot.Maps.Find(ERole::Metallic);
			UTexture2D* const* AO = Slot.Maps.Find(ERole::AO);
			UTexture2D* const* Packed = Slot.Maps.Find(ERole::MetalRough);
			if (!Rough && !Metal && !Packed)
			{
				return nullptr;
			}
			int32 Size = 0;
			for (UTexture2D* const* Texture : {Rough, Metal, AO, Packed})
			{
				if (Texture && *Texture)
				{
					Size = FMath::Max(Size, FMath::Max((*Texture)->Source.GetSizeX(), (*Texture)->Source.GetSizeY()));
				}
			}
			Size = FMath::Clamp(Size, 16, 4096);
			FImage RoughImage, MetalImage, AOImage, PackedImage;
			const bool bRough = Rough && *Rough && RawPixels(**Rough, RoughImage, Size, Size);
			const bool bMetal = Metal && *Metal && RawPixels(**Metal, MetalImage, Size, Size);
			const bool bAO = AO && *AO && RawPixels(**AO, AOImage, Size, Size);
			const bool bPacked = Packed && *Packed && RawPixels(**Packed, PackedImage, Size, Size);
			TArray<uint8> Pixels;
			Pixels.SetNumUninitialized(Size * Size * 4);
			for (int32 Index = 0; Index < Size * Size; ++Index)
			{
				// BGRA bytes: blue metal, green roughness, red occlusion. glTF packs roughness in green and metal in blue.
				const uint8 RoughValue = bRough ? RoughImage.RawData[Index * 4 + 2] : bPacked ? PackedImage.RawData[Index * 4 + 1] : 204;
				const uint8 MetalValue = bMetal ? MetalImage.RawData[Index * 4 + 2] : bPacked ? PackedImage.RawData[Index * 4 + 0] : 0;
				const uint8 AOValue = bAO ? AOImage.RawData[Index * 4 + 2] : 255;
				Pixels[Index * 4 + 0] = MetalValue;
				Pixels[Index * 4 + 1] = RoughValue;
				Pixels[Index * 4 + 2] = AOValue;
				Pixels[Index * 4 + 3] = 255;
			}
			UObject* Other = nullptr;
			UTexture2D* Texture = FindOrLoad<UTexture2D>(Package, Other);
			if (Other)
			{
				OutError = FString::Printf(TEXT("%s is already something other than a texture"), *Package);
				return nullptr;
			}
			const bool bNew = Texture == nullptr;
			if (bNew)
			{
				Texture = NewObject<UTexture2D>(CreatePackage(*Package), *FPackageName::GetShortName(Package), RF_Public | RF_Standalone);
			}
			else
			{
				Texture->Modify();
			}
			Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, Pixels.GetData());
			Texture->SRGB = false;
			Texture->CompressionSettings = TC_Masks;
			Texture->PostEditChange();
			if (bNew)
			{
				FAssetRegistryModule::AssetCreated(Texture);
			}
			Texture->MarkPackageDirty();
			return FHyperAIStudioSourceControl::SaveAsset(*Texture, OutError) ? Texture : nullptr;
		}

		/** Weapon textures capped as the convention says (2K colour and normal, a 1K mask by default); the sources keep their size. */
		void CapWeaponTextures(UObject& Mesh, const FWeaponFinish& Finish, TArray<FString>& OutNotes)
		{
			int32 Capped = 0;
			for (const FSlot& Slot : SlotsOf(Mesh))
			{
				for (const TPair<ERole, UTexture2D*>& Map : Slot.Maps)
				{
					if (!Map.Value || Map.Value->GetPathName().StartsWith(TEXT("/Engine/")))
					{
						continue;
					}
					const int32 Cap = Map.Key == ERole::BaseColor || Map.Key == ERole::Emissive ? Finish.ColorSize : Map.Key == ERole::Normal ? Finish.NormalSize : Finish.MaskSize;
					if (Cap > 0 && Map.Value->MaxTextureSize != Cap)
					{
						Map.Value->Modify();
						Map.Value->MaxTextureSize = Cap;
						Map.Value->PostEditChange();
						FString Error;
						FHyperAIStudioSourceControl::SaveAsset(*Map.Value, Error);
						++Capped;
					}
				}
			}
			if (Capped > 0)
			{
				OutNotes.Add(FString::Printf(TEXT("weapon_finish: %d texture(s) capped at colour %d, normal %d, mask %d (their sources kept)."), Capped, Finish.ColorSize, Finish.NormalSize,
					Finish.MaskSize));
			}
		}

		FString CleanName(const FString& Name);

		// ------------------------------------------------------------ AAA swords: the asset work

		const TCHAR* const BladeSteelPath = TEXT("/Game/HyperAI/Materials/M_HyperAI_BladeSteel_Clean");

		/**
		 * Clean blade steel: steel's albedo (0.56, 0.57, 0.58), metal, and roughness from an edge mask (frac(2U), U being
		 * 0.01-0.49 on the front face and 0.51-0.99 on the back, raised to the 8th) between FlatRoughness 0.28 and
		 * EdgeRoughness 0.10, so the edges catch the light. No noise texture: the engine's 64 x 64 one read as blocky bands.
		 */
		UMaterialInterface* BladeSteel(FString& OutError)
		{
			UObject* Other = nullptr;
			if (UMaterialInterface* Found = FindOrLoad<UMaterialInterface>(BladeSteelPath, Other))
			{
				return Found;
			}
			if (Other)
			{
				OutError = FString::Printf(TEXT("%s exists but is not a material."), BladeSteelPath);
				return nullptr;
			}
			TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
			UMaterial* Material = NewObject<UMaterial>(CreatePackage(BladeSteelPath), *FPackageName::GetShortName(BladeSteelPath), RF_Public | RF_Standalone);
			TArray<UMaterialExpression*> All;
			auto Make = [&](auto* Expression) { All.Add(Expression); return Expression; };
			UMaterialExpressionVectorParameter* Color = Make(NewObject<UMaterialExpressionVectorParameter>(Material));
			Color->ParameterName = TEXT("SteelColor");
			Color->DefaultValue = FLinearColor(0.56f, 0.57f, 0.58f, 1.f);
			UMaterialExpressionConstant* Metal = Make(NewObject<UMaterialExpressionConstant>(Material));
			Metal->R = 1.f;
			UMaterialExpressionTextureCoordinate* Coordinates = Make(NewObject<UMaterialExpressionTextureCoordinate>(Material));
			UMaterialExpressionComponentMask* U = Make(NewObject<UMaterialExpressionComponentMask>(Material));
			U->R = true;
			U->Input.Connect(0, Coordinates);
			UMaterialExpressionMultiply* Twice = Make(NewObject<UMaterialExpressionMultiply>(Material));
			Twice->A.Connect(0, U);
			Twice->ConstB = 2.f;
			UMaterialExpressionFrac* Frac = Make(NewObject<UMaterialExpressionFrac>(Material));
			Frac->Input.Connect(0, Twice);
			UMaterialExpressionSubtract* Half = Make(NewObject<UMaterialExpressionSubtract>(Material));
			Half->A.Connect(0, Frac);
			Half->ConstB = 0.5f;
			UMaterialExpressionAbs* Abs = Make(NewObject<UMaterialExpressionAbs>(Material));
			Abs->Input.Connect(0, Half);
			UMaterialExpressionMultiply* Edge = Make(NewObject<UMaterialExpressionMultiply>(Material));
			Edge->A.Connect(0, Abs);
			Edge->ConstB = 2.08f;
			UMaterialExpressionPower* Sharp = Make(NewObject<UMaterialExpressionPower>(Material));
			Sharp->Base.Connect(0, Edge);
			Sharp->ConstExponent = 8.f;
			UMaterialExpressionScalarParameter* Flat = Make(NewObject<UMaterialExpressionScalarParameter>(Material));
			Flat->ParameterName = TEXT("FlatRoughness");
			Flat->DefaultValue = 0.28f;
			UMaterialExpressionScalarParameter* EdgeRough = Make(NewObject<UMaterialExpressionScalarParameter>(Material));
			EdgeRough->ParameterName = TEXT("EdgeRoughness");
			EdgeRough->DefaultValue = 0.10f;
			UMaterialExpressionLinearInterpolate* Rough = Make(NewObject<UMaterialExpressionLinearInterpolate>(Material));
			Rough->A.Connect(0, Flat);
			Rough->B.Connect(0, EdgeRough);
			Rough->Alpha.Connect(0, Sharp);
			int32 Row = 0;
			for (UMaterialExpression* Expression : All)
			{
				Expression->MaterialExpressionEditorX = -900 + (Row % 4) * 220;
				Expression->MaterialExpressionEditorY = (Row / 4) * 200;
				++Row;
				Material->GetExpressionCollection().AddExpression(Expression);
			}
			Material->GetEditorOnlyData()->BaseColor.Connect(0, Color);
			Material->GetEditorOnlyData()->Metallic.Connect(0, Metal);
			Material->GetEditorOnlyData()->Roughness.Connect(0, Rough);
			Material->PostEditChange();
			FAssetRegistryModule::AssetCreated(Material);
			Material->MarkPackageDirty();
			return FHyperAIStudioSourceControl::SaveAsset(*Material, OutError) ? Material : nullptr;
		}

		/** A texture written in place (a fresh one, or the same asset's source replaced): never a re-import over stale data. */
		UTexture2D* WriteTexture(const FString& Package, const TArray<FColor>& Pixels, const int32 Width, const int32 Height, const bool bSRGB, const TextureCompressionSettings Compression,
			const int32 MaxSize, FString& OutError)
		{
			UObject* Other = nullptr;
			UTexture2D* Texture = FindOrLoad<UTexture2D>(Package, Other);
			if (Other)
			{
				OutError = FString::Printf(TEXT("%s is already something other than a texture"), *Package);
				return nullptr;
			}
			const bool bNew = Texture == nullptr;
			if (bNew)
			{
				Texture = NewObject<UTexture2D>(CreatePackage(*Package), *FPackageName::GetShortName(Package), RF_Public | RF_Standalone);
			}
			else
			{
				Texture->Modify();
			}
			Texture->Source.Init(Width, Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
			Texture->SRGB = bSRGB;
			Texture->CompressionSettings = Compression;
			Texture->MaxTextureSize = MaxSize;
			Texture->VirtualTextureStreaming = false;
			Texture->PostEditChange();
			if (bNew)
			{
				FAssetRegistryModule::AssetCreated(Texture);
			}
			Texture->MarkPackageDirty();
			return FHyperAIStudioSourceControl::SaveAsset(*Texture, OutError) ? Texture : nullptr;
		}

		/** Interchange's _nonVT copy (made in projects with virtual textures) stands for its original: the original is used. */
		UTexture2D* Original(UTexture2D* Texture)
		{
			if (!Texture || !Texture->GetName().EndsWith(TEXT("_nonVT")))
			{
				return Texture;
			}
			const FString Package = Texture->GetOutermost()->GetName().LeftChop(6);
			UObject* Other = nullptr;
			UTexture2D* Found = FindOrLoad<UTexture2D>(Package, Other);
			return Found ? Found : Texture;
		}

		/**
		 * The blade's seat: a box cut from the hilt where the generated blade's root was (a boolean subtract, its holes
		 * filled), so the guard's underside is a closed, flat face the new blade's ricasso enters. The cut's new faces take
		 * the guard's material and a texel of solid metal from its atlas.
		 */
		void SeatBlade(FDynamicMesh3& Mesh, const FSwordSpec& Spec, const FHiltFrame& Frame, const TArray<FMaterialClass>& Palette, const FAtlasResult& Atlas,
			TArray<FString>& OutNotes)
		{
			const double Low = Frame.GuardBottom - Spec.SeatBelow;
			const double High = Frame.GuardBottom + Spec.SeatAbove;
			const FVector3d Middle(Frame.Centre.X, (Low + High) * 0.5, Frame.Centre.Y);
			const FVector3d Half(Spec.SeatHalfWidth, (High - Low) * 0.5, Spec.SeatHalfThickness);
			FMinimalBoxMeshGenerator Box;
			Box.Box = FOrientedBox3d(FFrame3d(Middle), Half);
			FDynamicMesh3 Tool(&Box.Generate());
			// The tool carries the hilt's attribute layout, so the boolean keeps the hilt's own.
			Tool.EnableMatchingAttributes(Mesh);
			int32 HiltMaterial = 0;
			{
				TMap<int32, int32> Counts;
				for (const int32 Tid : Mesh.TriangleIndicesItr())
				{
					++Counts.FindOrAdd(Mesh.Attributes()->GetMaterialID()->GetValue(Tid));
				}
				int32 Most = -1;
				for (const TPair<int32, int32>& Each : Counts)
				{
					if (Each.Value > Most)
					{
						Most = Each.Value;
						HiltMaterial = Each.Key;
					}
				}
			}
			FDynamicMesh3 Result;
			FMeshBoolean Boolean(&Mesh, &Tool, &Result, FMeshBoolean::EBooleanOp::Difference);
			Boolean.bPutResultInInputSpace = true;
			Boolean.bSimplifyAlongNewEdges = false;
			Boolean.Compute();
			if (Result.TriangleCount() == 0)
			{
				OutNotes.Add(TEXT("sword_finish: the blade's seat could not be cut; the guard is left whole."));
				return;
			}
			TSet<int32> Filled;
			if (!Boolean.CreatedBoundaryEdges.IsEmpty())
			{
				FMeshBoundaryLoops Open(&Result, false);
				const TSet<int32> Consider(Boolean.CreatedBoundaryEdges);
				Open.EdgeFilterFunc = [&Consider](const int32 Eid) { return Consider.Contains(Eid); };
				Open.Compute();
				for (const FEdgeLoop& Loop : Open.Loops)
				{
					FMinimalHoleFiller Filler(&Result, Loop);
					if (Filler.Fill())
					{
						Filled.Append(Filler.NewTriangles);
					}
				}
			}
			const FAxisAlignedBox3d Near(Middle - Half - FVector3d(0.5), Middle + Half + FVector3d(0.5));
			// Whatever is still open about the seat (a piece of the cut's face the boolean dropped): each loop filled.
			{
				FMeshBoundaryLoops Remaining(&Result, false);
				Remaining.EdgeFilterFunc = [&Result, &Near](const int32 Eid)
				{
					const FIndex2i Ab = Result.GetEdgeV(Eid);
					return Near.Contains(Result.GetVertex(Ab.A)) && Near.Contains(Result.GetVertex(Ab.B));
				};
				Remaining.Compute();
				for (const FEdgeLoop& Loop : Remaining.Loops)
				{
					FMinimalHoleFiller Filler(&Result, Loop);
					if (Filler.Fill())
					{
						Filled.Append(Filler.NewTriangles);
						continue;
					}
					FSimpleHoleFiller Fan(&Result, Loop, FSimpleHoleFiller::EFillType::TriangleFan);
					if (Fan.Fill())
					{
						Filled.Append(Fan.NewTriangles);
					}
				}
			}
			// The cap: faces lying on the box's planes, and what filled its holes.
			FVector2f Metal(0.5f, 0.5f);
			const bool bMetal = SolidMetalTexel(Atlas, Palette, 25, Metal);
			FDynamicMeshAttributeSet& Attributes = *Result.Attributes();
			auto OnPlane = [&](const FVector3d& A, const FVector3d& B, const FVector3d& C)
			{
				for (int32 Axis : {0, 1, 2})
				{
					for (const double Plane : {Middle[Axis] - Half[Axis], Middle[Axis] + Half[Axis]})
					{
						if (FMath::Abs(A[Axis] - Plane) < 0.02 && FMath::Abs(B[Axis] - Plane) < 0.02 && FMath::Abs(C[Axis] - Plane) < 0.02)
						{
							return true;
						}
					}
				}
				return false;
			};
			int32 Capped = 0;
			for (const int32 Tid : Result.TriangleIndicesItr())
			{
				FVector3d A, B, C;
				Result.GetTriVertices(Tid, A, B, C);
				if (!Filled.Contains(Tid) && !OnPlane(A, B, C))
				{
					continue;
				}
				++Capped;
				Attributes.GetMaterialID()->SetValue(Tid, HiltMaterial);
				for (int32 Layer = 0; Layer < Attributes.NumUVLayers(); ++Layer)
				{
					FDynamicMeshUVOverlay* UVs = Attributes.GetUVLayer(Layer);
					const int32 Element = UVs->AppendElement(Metal);
					UVs->SetTriangle(Tid, FIndex3i(Element, Element, Element));
				}
				if (FDynamicMeshNormalOverlay* Normals = Attributes.PrimaryNormals())
				{
					// Flat, so the cap's edges stay crisp.
					const int32 Element = Normals->AppendElement(FVector3f(Result.GetTriNormal(Tid)));
					Normals->SetTriangle(Tid, FIndex3i(Element, Element, Element));
				}
			}
			int32 Left = 0;
			for (const int32 Eid : Result.BoundaryEdgeIndicesItr())
			{
				const FIndex2i Ab = Result.GetEdgeV(Eid);
				Left += Near.Contains(Result.GetVertex(Ab.A)) && Near.Contains(Result.GetVertex(Ab.B));
			}
			Mesh = MoveTemp(Result);
			Mesh.CompactInPlace();
			OutNotes.Add(FString::Printf(TEXT("sword_finish: the blade's seat cut from the guard as a sealed box (%.1f x %.1f cm, from %.1f cm below the guard to %.1f into it), %d open edge(s) left there (%d filled); %d cap face(s) %s."),
				Half.X * 2.0, Half.Z * 2.0, Spec.SeatBelow, Spec.SeatAbove, Left, Filled.Num(), Capped,
				bMetal ? *FString::Printf(TEXT("in the guard's solid metal (UV %.3f, %.3f)"), Metal.X, Metal.Y) : TEXT("in the guard's material (no solid metal found in its atlas)")));
		}

		/** What the sword's geometry stage leaves for the asset stage. */
		struct FSwordWork
		{
			bool bDone = false;
			int32 BladeMaterial = INDEX_NONE;
			FHiltFrame Frame;
			FSwordBalance Balance;
			FAtlasResult Atlas;
			UTexture2D* Normal = nullptr;
			UTexture2D* SourceColor = nullptr;
			double TipY = 0.0;
		};

		/**
		 * The sword's geometry, after ShapeWeapon has turned and slimmed it: the hilt's atlas read into classes, the generated
		 * blade and its root cut away, the hilt reduced (seams kept) to what the blade leaves of the budget, the pommel
		 * scaled for balance, smooth normals, then the exact blade appended as a material of its own.
		 */
		bool FinishSwordGeometry(FDynamicMesh3& Mesh, UObject& Object, const FCheckTargets& Targets, TArray<FString>& OutNotes, FSwordWork& Work)
		{
			const FSwordSpec Spec = SwordSpecFor(Targets.Weapon.LengthCm);
			// The hilt's atlas, read into material classes: the brief's words and the reference picture name them.
			const TArray<FSlot> Slots = SlotsOf(Object);
			Work.SourceColor = Slots.IsEmpty() ? nullptr : Original(Slots[0].Maps.FindRef(ERole::BaseColor));
			Work.Normal = Slots.IsEmpty() ? nullptr : Original(Slots[0].Maps.FindRef(ERole::Normal));
			TArray<FColor> Picture;
			FImage Reference;
			if (IsPicture(Targets.ReferenceImage) && FImageUtils::LoadImage(*Targets.ReferenceImage, Reference))
			{
				FImage Small;
				Reference.ResizeTo(Small, FMath::Min(Reference.SizeX, 512), FMath::Min(Reference.SizeY, 512), ERawImageFormat::BGRA8, EGammaSpace::sRGB);
				Picture.Append(reinterpret_cast<const FColor*>(Small.RawData.GetData()), Small.SizeX * Small.SizeY);
			}
			TArray<FString> PaletteNotes;
			const TArray<FMaterialClass> Palette = MaterialPalette(Targets.Weapon.Palette, Picture, PaletteNotes);
			OutNotes.Add(TEXT("sword_finish: ") + FString::Join(PaletteNotes, TEXT("; ")) + TEXT("."));
			FImage Colour;
			if (Work.SourceColor && RawPixels(*Work.SourceColor, Colour))
			{
				TArray<FColor> Pixels;
				Pixels.Append(reinterpret_cast<const FColor*>(Colour.RawData.GetData()), Colour.SizeX * Colour.SizeY);
				Work.Atlas = CorrectAtlas(Pixels, Colour.SizeX, Colour.SizeY, Palette);
				TArray<FString> Shares;
				for (int32 Class = 0; Class < Palette.Num(); ++Class)
				{
					Shares.Add(FString::Printf(TEXT("%s %.0f%%"), *Palette[Class].Name, Work.Atlas.Shares[Class] * 100.f));
				}
				OutNotes.Add(FString::Printf(TEXT("sword_finish: the hilt's %d x %d atlas made physically based by texel: %s."), Colour.SizeX, Colour.SizeY, *FString::Join(Shares, TEXT(", "))));
			}
			else
			{
				OutNotes.Add(TEXT("sword_finish: no base colour texture on the hilt; its textures are left as they are."));
			}

			TArray<FVector3d> Points;
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				Points.Add(Mesh.GetVertex(Vid));
			}
			Work.Frame = MeasureHilt(Points);
			if (!Work.Frame.bValid)
			{
				OutNotes.Add(TEXT("sword_finish: no guard and grip found; the generated blade is kept."));
				return false;
			}
			const FHiltFrame& Frame = Work.Frame;
			const double Bottom = Frame.GuardBottom;
			// The generated blade below the guard: everything under its lower face, and up to 4 mm into it where the blade's
			// seat will be cut (the guard's own underside beyond the seat stays, or the guard would be left open). Its root
			// inside the guard is not torn out triangle by triangle (that left a shredded underside and a hole above the new
			// blade): a sealed box cut takes it below, once the normals are set.
			auto InSeat = [&Frame, &Spec](const FVector3d& P)
			{
				return FMath::Abs(P.X - Frame.Centre.X) <= Spec.SeatHalfWidth && FMath::Abs(P.Z - Frame.Centre.Y) <= Spec.SeatHalfThickness;
			};
			TArray<int32> Cut;
			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				FVector3d A, B, C;
				Mesh.GetTriVertices(Tid, A, B, C);
				const double Top = FMath::Max3(A.Y, B.Y, C.Y);
				if (Top < Bottom - 0.01 || (Top < Bottom + 0.4 && InSeat(A) && InSeat(B) && InSeat(C)))
				{
					Cut.Add(Tid);
				}
			}
			for (const int32 Tid : Cut)
			{
				Mesh.RemoveTriangle(Tid, /*bRemoveIsolatedVertices=*/true, /*bPreserveManifold=*/false);
			}
			OutNotes.Add(FString::Printf(TEXT("sword_finish: the generated blade cut away below the guard (%d triangles)."), Cut.Num()));

			// The exact blade, built now to know what of the budget it leaves the hilt.
			const FBladeMesh Blade = BuildBlade(Spec, Bottom, Frame.Centre);
			const int32 HiltBudget = FMath::Max(Targets.Weapon.TargetTris - Blade.Triangles.Num(), 1000);
			if (Mesh.TriangleCount() > HiltBudget)
			{
				ReduceKeepingTextures(Mesh, HiltBudget, TEXT("sword_finish"), OutNotes);
			}
			else
			{
				// Welded, so the smooth normals below run across the atlas's seams.
				FMergeCoincidentMeshEdges Merge(&Mesh);
				Merge.MergeVertexTolerance = FMath::Max(Mesh.GetBounds().DiagonalLength() * 1e-6, 1e-6);
				Merge.Apply();
			}
			Mesh.CompactInPlace();

			// Balance: the pommel scaled (0.75-1.3) toward a point of balance 10-13 cm in front of the guard.
			TArray<FVector3d> Hilt;
			TArray<FIntVector> Triangles;
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				Hilt.Add(Mesh.GetVertex(Vid));
			}
			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				const FIndex3i T = Mesh.GetTriangle(Tid);
				Triangles.Add(FIntVector(T.A, T.B, T.C));
			}
			Work.Balance = BalanceSword(Spec, Hilt, Triangles, Frame);
			ScalePommel(Hilt, Frame, Work.Balance.PommelScale);
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				Mesh.SetVertex(Vid, Hilt[Vid]);
			}
			OutNotes.Add(FString::Printf(TEXT("sword_balance: %.0f g (blade %.0f g, hilt %.0f g), the point of balance %.1f cm in front of the guard, the pommel scaled %.2f%s."),
				Work.Balance.MassG, Work.Balance.BladeG, Work.Balance.HiltG, Work.Balance.BalanceCm, Work.Balance.PommelScale,
				Work.Balance.bInWindow ? TEXT("") : *FString::Printf(TEXT("; %.0f-%.0f cm was out of reach: the hilt's guard is too heavy or the pommel too light for the pommel's scale alone to move it"),
					Spec.BalanceMin, Spec.BalanceMax)));

			// Smooth normals per vertex over the hilt, before the seat is cut, so the cut's edges stay crisp.
			if (!Mesh.HasAttributes())
			{
				Mesh.EnableAttributes();
			}
			if (Mesh.Attributes()->PrimaryNormals())
			{
				FMeshNormals::InitializeOverlayToPerVertexNormals(Mesh.Attributes()->PrimaryNormals(), /*bUseMeshVertexNormalsIfAvailable=*/false);
			}
			if (!Mesh.Attributes()->HasMaterialID())
			{
				Mesh.Attributes()->EnableMaterialID();
			}
			if (Mesh.Attributes()->NumUVLayers() == 0)
			{
				Mesh.Attributes()->SetNumUVLayers(1);
			}
			SeatBlade(Mesh, Spec, Frame, Palette, Work.Atlas, OutNotes);
			FDynamicMeshAttributeSet& Attributes = *Mesh.Attributes();
			int32 Highest = 0;
			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				Highest = FMath::Max(Highest, Attributes.GetMaterialID()->GetValue(Tid));
			}
			Work.BladeMaterial = Highest + 1;
			if (Attributes.NumUVLayers() == 0)
			{
				Attributes.SetNumUVLayers(1);
			}
			TArray<int32> Vertices;
			for (const FVector3d& P : Blade.Positions)
			{
				Vertices.Add(Mesh.AppendVertex(P));
			}
			TArray<TArray<int32>> UVElements;
			UVElements.SetNum(Attributes.NumUVLayers());
			for (int32 Layer = 0; Layer < Attributes.NumUVLayers(); ++Layer)
			{
				for (const FVector2f& UV : Blade.UVs)
				{
					UVElements[Layer].Add(Attributes.GetUVLayer(Layer)->AppendElement(UV));
				}
			}
			TArray<int32> NormalElements;
			FDynamicMeshNormalOverlay* Normals = Attributes.PrimaryNormals();
			if (Normals)
			{
				for (const FVector3f& Normal : Blade.Normals)
				{
					NormalElements.Add(Normals->AppendElement(Normal));
				}
			}
			int32 Added = 0;
			Work.TipY = TNumericLimits<double>::Max();
			for (const FIntVector& Tri : Blade.Triangles)
			{
				const int32 Tid = Mesh.AppendTriangle(Vertices[Tri.X], Vertices[Tri.Y], Vertices[Tri.Z]);
				if (Tid < 0)
				{
					continue;
				}
				++Added;
				Attributes.GetMaterialID()->SetValue(Tid, Work.BladeMaterial);
				for (int32 Layer = 0; Layer < Attributes.NumUVLayers(); ++Layer)
				{
					Attributes.GetUVLayer(Layer)->SetTriangle(Tid, FIndex3i(UVElements[Layer][Tri.X], UVElements[Layer][Tri.Y], UVElements[Layer][Tri.Z]));
				}
				if (Normals)
				{
					Normals->SetTriangle(Tid, FIndex3i(NormalElements[Tri.X], NormalElements[Tri.Y], NormalElements[Tri.Z]));
				}
			}
			for (const FVector3d& P : Blade.Positions)
			{
				Work.TipY = FMath::Min(Work.TipY, P.Y);
			}
			OutNotes.Add(FString::Printf(TEXT("sword_finish: an exact blade, %.0f cm from the guard (%d triangles): %.1f cm wide tapering to %.1f, %.2f cm thick to %.2f at the point, a fuller %.1f cm wide and %.2f cm deep, a %.1f cm ricasso into the guard."),
				Spec.BladeLength, Added, Spec.BaseWidth, Spec.PointWidth, Spec.BaseThickness, Spec.TipThickness, Spec.FullerWidth, Spec.FullerDepth, Spec.Ricasso));
			Work.bDone = Added > 0;
			return Work.bDone;
		}

		/**
		 * The sword's asset work: the hilt's corrected textures written (not virtual textures; colour and normal 4K, the ORM
		 * 2K linear masks), the normal map's OpenGL green flipped, Interchange's _nonVT copies dropped, the hilt and the
		 * blade given their materials, and the gameplay sockets placed along Y.
		 */
		void FinishSwordAsset(UStaticMesh& Static, const FCheckTargets& Targets, const FSwordWork& Work, TArray<FString>& OutNotes)
		{
			FString Error;
			UMaterialInterface* Parent = Master(Error);
			UMaterialInterface* Steel = BladeSteel(Error);
			const FString Folder = FPackageName::GetLongPackagePath(Static.GetOutermost()->GetName());
			const FString MeshName = CleanName(Static.GetName());
			UTexture2D* Color = Work.SourceColor;
			UTexture2D* ORM = nullptr;
			if (Work.Atlas.Width > 0)
			{
				Color = WriteTexture(Folder / FString::Printf(TEXT("T_%s_Hilt_BC"), *MeshName), Work.Atlas.BaseColor, Work.Atlas.Width, Work.Atlas.Height, /*bSRGB=*/true, TC_Default,
					Targets.Weapon.ColorSize, Error);
				ORM = WriteTexture(Folder / FString::Printf(TEXT("T_%s_Hilt_ORM"), *MeshName), Work.Atlas.ORM, Work.Atlas.Width, Work.Atlas.Height, /*bSRGB=*/false, TC_Masks,
					Targets.Weapon.MaskSize, Error);
			}
			if (UTexture2D* Normal = Work.Normal)
			{
				Normal->Modify();
				Normal->VirtualTextureStreaming = false;
				Normal->SRGB = false;
				Normal->CompressionSettings = TC_Normalmap;
				Normal->bFlipGreenChannel = Normal->bFlipGreenChannel || IsOpenGLNormal(*Normal);
				Normal->MaxTextureSize = Targets.Weapon.NormalSize;
				Normal->PostEditChange();
				FHyperAIStudioSourceControl::SaveAsset(*Normal, Error);
			}
			// The hilt: an instance of HyperAI's master with the corrected maps, on every slot but the blade's.
			UMaterialInstanceConstant* HiltMaterial = nullptr;
			if (Parent)
			{
				const FString Package = Folder / FString::Printf(TEXT("MI_%s_Hilt"), *MeshName);
				UObject* Other = nullptr;
				HiltMaterial = FindOrLoad<UMaterialInstanceConstant>(Package, Other);
				if (!HiltMaterial && !Other)
				{
					HiltMaterial = NewObject<UMaterialInstanceConstant>(CreatePackage(*Package), *FPackageName::GetShortName(Package), RF_Public | RF_Standalone);
					FAssetRegistryModule::AssetCreated(HiltMaterial);
				}
				if (HiltMaterial)
				{
					HiltMaterial->Modify();
					HiltMaterial->SetParentEditorOnly(Parent);
					if (Color)
					{
						HiltMaterial->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BaseColor")), Color);
					}
					if (Work.Normal)
					{
						HiltMaterial->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Normal")), Work.Normal);
					}
					if (ORM)
					{
						HiltMaterial->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("ORM")), ORM);
					}
					HiltMaterial->PostEditChange();
					HiltMaterial->MarkPackageDirty();
					FHyperAIStudioSourceControl::SaveAsset(*HiltMaterial, Error);
				}
			}
			UMaterialInstanceConstant* BladeMaterial = nullptr;
			if (Steel)
			{
				const FString Package = Folder / FString::Printf(TEXT("MI_%s_Blade"), *MeshName);
				UObject* Other = nullptr;
				BladeMaterial = FindOrLoad<UMaterialInstanceConstant>(Package, Other);
				if (!BladeMaterial && !Other)
				{
					BladeMaterial = NewObject<UMaterialInstanceConstant>(CreatePackage(*Package), *FPackageName::GetShortName(Package), RF_Public | RF_Standalone);
					FAssetRegistryModule::AssetCreated(BladeMaterial);
				}
				if (BladeMaterial)
				{
					BladeMaterial->Modify();
					BladeMaterial->SetParentEditorOnly(Steel);
					BladeMaterial->PostEditChange();
					BladeMaterial->MarkPackageDirty();
					FHyperAIStudioSourceControl::SaveAsset(*BladeMaterial, Error);
				}
			}
			static const FName BladeSlot(TEXT("Blade"));
			TArray<FStaticMaterial>& Materials = Static.GetStaticMaterials();
			if (!Materials.ContainsByPredicate([](const FStaticMaterial& Each) { return Each.ImportedMaterialSlotName == BladeSlot; }))
			{
				Materials.Add(FStaticMaterial(BladeMaterial, BladeSlot, BladeSlot));
			}
			for (FStaticMaterial& Each : Materials)
			{
				if (Each.ImportedMaterialSlotName == BladeSlot)
				{
					Each.MaterialInterface = BladeMaterial;
				}
				else if (HiltMaterial)
				{
					Each.MaterialInterface = HiltMaterial;
				}
			}
			// Interchange's _nonVT copies beside the originals now used: dropped.
			TArray<UObject*> Copies;
			IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			TArray<FAssetData> Found;
			Registry.GetAssetsByPath(FName(*FPackageName::GetLongPackagePath(Folder)), Found, /*bRecursive=*/true);
			for (const FAssetData& Asset : Found)
			{
				const FString Name = Asset.AssetName.ToString();
				if (Name.EndsWith(TEXT("_nonVT")) && FPackageName::DoesPackageExist(Asset.PackageName.ToString().LeftChop(6)))
				{
					if (UObject* Copy = Asset.GetAsset())
					{
						Copies.Add(Copy);
					}
				}
			}
			if (!Copies.IsEmpty())
			{
				FAssetCompilingManager::Get().FinishAllCompilation();
				ObjectTools::ForceDeleteObjects(Copies, /*ShowConfirmation=*/false);
			}
			// Gameplay sockets along Y: the grip at the origin, the trail from the guard to the point, the point of balance.
			const double Guard = Work.Frame.GuardBottom;
			const TPair<const TCHAR*, double> Sockets[] = {{TEXT("Grip"), 0.0}, {TEXT("Trail_Start"), Guard}, {TEXT("Trail_End"), Work.TipY + 1.0},
				{TEXT("BalancePoint"), Guard - Work.Balance.BalanceCm}};
			for (const TPair<const TCHAR*, double>& Each : Sockets)
			{
				if (UStaticMeshSocket* Old = Static.FindSocket(Each.Key))
				{
					Static.RemoveSocket(Old);
				}
				UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(&Static);
				Socket->SocketName = Each.Key;
				Socket->RelativeLocation = FVector(0.0, Each.Value, 0.0);
				Static.AddSocket(Socket);
			}
			OutNotes.Add(FString::Printf(TEXT("sword_finish: the hilt's textures %s (colour %d, normal %d with its OpenGL green flipped, ORM %d as linear masks, no virtual texturing)%s; the blade in clean steel; sockets Grip, Trail_Start, Trail_End and BalancePoint."),
				Work.Atlas.Width > 0 ? TEXT("rebuilt") : TEXT("kept"), Targets.Weapon.ColorSize, Targets.Weapon.NormalSize, Targets.Weapon.MaskSize,
				Copies.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; %d _nonVT cop(ies) dropped"), Copies.Num())));
		}

		FString CleanName(const FString& Name)
		{
			FString Out;
			for (const TCHAR Char : Name)
			{
				Out.AppendChar(FChar::IsAlnum(Char) ? Char : TEXT('_'));
			}
			return Out.IsEmpty() ? FString(TEXT("Slot")) : Out.Left(40);
		}
	}

	using namespace InspectDetail;

	FString MasterMaterialPath()
	{
		return MasterPath;
	}

	void CapWeaponTextureSizes(UObject& Mesh, const FWeaponFinish& Finish, TArray<FString>& OutNotes)
	{
		CapWeaponTextures(Mesh, Finish, OutNotes);
	}

	FCheckTargets TargetsFromPlan(const FHyperAIModelPlan& Plan)
	{
		FCheckTargets Targets;
		Targets.Use = Plan.Use;
		Targets.FaceLimit = Plan.FaceLimit;
		Targets.MinFaces = Plan.MinFaces;
		Targets.bQuad = Plan.bQuad;
		Targets.SizeMeters = Plan.SizeMeters;
		Targets.TexelDensity = Plan.TexelDensity;
		Targets.bTextures = Plan.TextureQuality != TEXT("none");
		Targets.bPbr = Plan.bPbr;
		Targets.bNanite = Plan.bNanite;
		Targets.Collision = Plan.Collision;
		Targets.bLocalReduce = Plan.bLocalReduce;
		// A weapon (not quads, which the finishing's reduction would undo) is finished to the project's convention.
		Targets.bWeapon = Plan.Use == TEXT("weapon") && !Plan.bQuad;
		if (Targets.bWeapon)
		{
			Targets.Weapon = WeaponFinishFor(Plan.SizeMeters, Plan.FaceLimit);
			Targets.Weapon.bNanite = Plan.bNanite;
			if (Plan.bSword)
			{
				MakeSwordFinish(Targets.Weapon, /*bKeepBudget=*/true);
				Targets.Weapon.Palette = Plan.Palette;
			}
		}
		return Targets;
	}

	bool Inspect(UObject& Object, const FCheckTargets& Targets, FHyperAIModelCheckReport& Out, FString& OutError)
	{
		Out = FHyperAIModelCheckReport();
		Out.AssetPath = Object.GetPathName();
		UStaticMesh* Static = Cast<UStaticMesh>(&Object);
		USkeletalMesh* Skeletal = Cast<USkeletalMesh>(&Object);
		const FMeshDescription* Source = SourceOf(Object);
		if (!Source)
		{
			OutError = FString::Printf(TEXT("%s is not a static or skeletal mesh with a source mesh to measure."), *Out.AssetPath);
			return false;
		}
		FDynamicMesh3 Mesh;
		{
			FMeshDescriptionToDynamicMesh Converter;
			Converter.Convert(Source, Mesh, /*bCopyTangents=*/false);
		}
		for (const FPolygonID Polygon : Source->Polygons().GetElementIDs())
		{
			Out.Quads += Source->GetNumPolygonVertices(Polygon) == 4;
		}
		const int32 Polygons = Source->Polygons().Num();
		Out.Triangles = Mesh.TriangleCount();
		const FWelded Welded = Weld(Mesh);
		Out.Vertices = Welded.Points.Num();
		const FTopology Topology = Analyse(Welded);
		Out.Components = Topology.ComponentTris.Num();
		const FAxisAlignedBox3d Bounds = Mesh.GetBounds();
		Out.Size = Bounds.IsEmpty() ? FVector::ZeroVector : FVector(Bounds.Max - Bounds.Min);
		const TArray<FSlot> Slots = SlotsOf(Object);
		Out.MaterialSlots = Slots.Num();
		auto Static_ = [&](const TCHAR* Fix) { return Static ? Fix : TEXT(""); };

		// Budget and topology.
		{
			const int32 Have = Targets.bQuad ? Out.Quads : Out.Triangles;
			const FString Value = FString::Printf(TEXT("%d triangles, %d quads"), Out.Triangles, Out.Quads);
			const FString Expected = Targets.FaceLimit > 0 ? FString::Printf(TEXT("%d-%d %s"), Targets.MinFaces, Targets.FaceLimit, Targets.bQuad ? TEXT("quads") : TEXT("triangles"))
				: FString(TEXT("no budget given"));
			if (Targets.FaceLimit <= 0)
			{
				Add(Out, TEXT("faces"), TEXT("pass"), Value, Expected, TEXT("No face budget to hold it to."));
			}
			else if (Targets.bQuad && (Out.Quads == 0 || Out.Quads * 10 < Polygons * 8))
			{
				Add(Out, TEXT("faces"), TEXT("fail"), Value, Expected, FString::Printf(TEXT("Wanted quads, but %d of %d polygons are quads."), Out.Quads, Polygons), TEXT(""), TEXT("retopo"));
			}
			else if (Have > Targets.FaceLimit * 1.05)
			{
				// A model made denser than its budget on purpose (Hi3D's 100,000-face floor) is reduced in the editor first, for free.
				Add(Out, TEXT("faces"), TEXT("fail"), Value, Expected, FString::Printf(TEXT("%d over the budget of %d."), Have - Targets.FaceLimit, Targets.FaceLimit),
					Targets.bWeapon && Static && !Targets.bQuad ? TEXT("weapon_finish") : Targets.bLocalReduce && Static && !Targets.bQuad ? TEXT("simplify") : TEXT(""), TEXT("retopo"));
			}
			else if (Targets.MinFaces > 0 && Have < Targets.MinFaces)
			{
				Add(Out, TEXT("faces"), TEXT("warn"), Value, Expected, TEXT("Under the least the budget allows; the shape may be too coarse."));
			}
			else
			{
				Add(Out, TEXT("faces"), TEXT("pass"), Value, Expected, TEXT("Within the budget."));
			}
		}

		// The surface. Generated meshes nearly always have a few of these defects; on a static mesh that only renders (no
		// skinning, no complex collision) a handful are a warning the free fixes try, never a reason to pay for a retopology
		// that throws the model's detail away. A character or creature to be rigged, or a mesh walked on, keeps them failing.
		const bool bOnlyRenders = Static && Targets.Use != TEXT("character") && Targets.Use != TEXT("creature") && Targets.Collision != TEXT("complex");
		const bool bMinor = bOnlyRenders && Topology.LargestHole <= MaxFillEdges && Topology.NonManifoldEdges <= FMath::Max(50, Out.Triangles / 200)
			&& Topology.Holes <= FMath::Max(50, Out.Triangles / 500);
		const FString MinorNote = bMinor ? FString(TEXT(" A handful on a static mesh that only renders: a warning, not worth a paid fix.")) : FString();
		Add(Out, TEXT("non_manifold"), Topology.NonManifoldEdges == 0 ? TEXT("pass") : bMinor ? TEXT("warn") : TEXT("fail"), FString::FromInt(Topology.NonManifoldEdges), TEXT("0"),
			Topology.NonManifoldEdges > 0 ? TEXT("Edges shared by more than two faces: they break baking, boolean and physics cooking.") + MinorNote : FString(TEXT("Every edge joins at most two faces.")),
			TEXT(""), TEXT("retopo"));
		const bool bOpenOk = Targets.Use == TEXT("foliage") || Targets.Use == TEXT("background") || bMinor;
		const TCHAR* OpenResult = Topology.BoundaryEdges == 0 ? TEXT("pass") : bOpenOk ? TEXT("warn") : TEXT("fail");
		Add(Out, TEXT("open_edges"), OpenResult, FString::FromInt(Topology.BoundaryEdges), TEXT("0"),
			Topology.BoundaryEdges == 0 ? TEXT("Closed: no open edges.") : TEXT("Open edges where the surface stops."), Static_(TEXT("fill_holes")), TEXT("retopo"));
		Add(Out, TEXT("holes"), Topology.Holes == 0 ? TEXT("pass") : bOpenOk ? TEXT("warn") : TEXT("fail"), FString::FromInt(Topology.Holes), TEXT("0"),
			Topology.Holes == 0 ? TEXT("No holes.") : FString::Printf(TEXT("%d hole(s), the largest with %d boundary vertices%s."), Topology.Holes, Topology.LargestHole,
				Topology.LargestHole > MaxFillEdges ? TEXT(": too big to cap in the editor, a missing part") : TEXT("")),
			Static_(TEXT("fill_holes")), Topology.LargestHole > MaxFillEdges ? TEXT("complete") : TEXT("quick_cap"));
		const int32 Flipped = Topology.Flipped.Num();
		Add(Out, TEXT("flipped"), Flipped > 0 ? TEXT("fail") : TEXT("pass"), FString::FromInt(Flipped), TEXT("0"),
			Flipped == 0 ? TEXT("Every face is wound the same way, outward.") : FString::Printf(TEXT("%d face(s) wound against their neighbours%s."), Flipped,
				Topology.InsideOut > 0 ? *FString::Printf(TEXT(", %d closed part(s) inside out"), Topology.InsideOut) : TEXT("")),
			Static_(TEXT("orient,normals")), TEXT("retopo"));
		{
			// Shading normals against the winding: a face whose stored normals point into it renders black or inside out.
			int32 Against = 0, Measured = 0;
			if (Mesh.HasAttributes() && Mesh.Attributes()->PrimaryNormals())
			{
				const FDynamicMeshNormalOverlay* Normals = Mesh.Attributes()->PrimaryNormals();
				for (const int32 Tid : Mesh.TriangleIndicesItr())
				{
					if (!Normals->IsSetTriangle(Tid))
					{
						continue;
					}
					const FIndex3i Elements = Normals->GetTriangle(Tid);
					const FVector3d Stored = FVector3d(Normals->GetElement(Elements.A) + Normals->GetElement(Elements.B) + Normals->GetElement(Elements.C));
					++Measured;
					Against += Stored.Dot(Mesh.GetTriNormal(Tid)) < 0.0;
				}
			}
			const bool bBad = Measured > 0 && Against * 20 > Measured;
			Add(Out, TEXT("normals"), bBad ? TEXT("fail") : TEXT("pass"), FString::Printf(TEXT("%d of %d"), Against, Measured), TEXT("under 5%"),
				bBad ? TEXT("Stored normals point against the faces' winding.") : TEXT("Stored normals agree with the winding."), Static_(TEXT("normals")));
		}
		Add(Out, TEXT("degenerate"), Topology.Degenerate == 0 ? TEXT("pass") : Topology.Degenerate * 100 > Out.Triangles ? TEXT("fail") : TEXT("warn"),
			FString::FromInt(Topology.Degenerate), TEXT("0"), Topology.Degenerate == 0 ? TEXT("No zero-area faces.") : TEXT("Faces with no area."), Static_(TEXT("degenerate")));
		{
			const TArray<int32> Small = SmallComponents(Topology, Out.Triangles, Welded.Diagonal);
			int32 SmallTris = 0;
			for (const int32 Id : Small)
			{
				SmallTris += Topology.ComponentTris[Id];
			}
			Add(Out, TEXT("floating"), Small.IsEmpty() ? (Out.Components > 1 ? TEXT("warn") : TEXT("pass")) : TEXT("fail"),
				FString::Printf(TEXT("%d stray bit(s), %d triangles; %d part(s) in all"), Small.Num(), SmallTris, Out.Components), TEXT("0 stray bits"),
				!Small.IsEmpty() ? TEXT("Small disconnected pieces, usually generation debris.") : Out.Components > 1 ? TEXT("Several large disconnected parts; fine for a parts model.")
				: TEXT("One connected piece."), Static_(TEXT("remove_bits")), TEXT("retopo"));
		}

		// Size, pivot, orientation.
		const double Height = Out.Size.Z;
		const double Longest = Out.Size.GetMax();
		if (Targets.SizeMeters > 0.f)
		{
			const double Measure = Upright(Targets.Use) ? Height : Longest;
			const double Want = Targets.SizeMeters * 100.0;
			const bool bOk = Measure > 0.0 && FMath::Abs(Measure - Want) <= Want * 0.15;
			FHyperAIModelCheck& Check = Add(Out, TEXT("scale"), bOk ? TEXT("pass") : TEXT("fail"), FString::Printf(TEXT("%.1f"), Measure), FString::Printf(TEXT("%.1f"), Want),
				FString::Printf(TEXT("%s is %.1f cm; %.1f cm wanted (within 15%%)."), Upright(Targets.Use) ? TEXT("Height") : TEXT("Longest side"), Measure, Want),
				Targets.bWeapon ? Static_(TEXT("weapon_finish")) : Static_(TEXT("scale")), Static ? TEXT("") : TEXT("convert"));
			(void)Check;
		}
		else
		{
			Add(Out, TEXT("scale"), TEXT("skip"), FString::Printf(TEXT("%.1f"), Longest), TEXT(""), TEXT("No real-world size to hold it to."));
		}
		if (Targets.bWeapon && Static)
		{
			Add(Out, TEXT("pivot"), TEXT("skip"), FString(), TEXT("in the grip"), TEXT("A weapon's pivot is in its grip, not at its bottom: weapon_axis checks it."));
		}
		else
		{
			const FVector3d Centre = Bounds.IsEmpty() ? FVector3d::Zero() : Bounds.Center();
			const double Tolerance = FMath::Max(Longest * 0.02, 0.5);
			const bool bBottom = !Bounds.IsEmpty() && FMath::Abs(Bounds.Min.Z) <= FMath::Max(Height * 0.01, 0.5) && FMath::Abs(Centre.X) <= Tolerance && FMath::Abs(Centre.Y) <= Tolerance;
			Add(Out, TEXT("pivot"), bBottom ? TEXT("pass") : TEXT("fail"),
				Bounds.IsEmpty() ? FString() : FString::Printf(TEXT("bottom centre at (%.1f, %.1f, %.1f)"), Centre.X, Centre.Y, Bounds.Min.Z), TEXT("(0, 0, 0)"),
				bBottom ? TEXT("The pivot is at the bottom centre: it stands on the floor where it is placed.") : TEXT("The pivot is not at the bottom centre."),
				Static_(TEXT("pivot")), Static ? TEXT("") : TEXT("convert"));
		}
		if (Upright(Targets.Use))
		{
			const bool bUp = Height >= FMath::Max(Out.Size.X, Out.Size.Y) * 0.6;
			Add(Out, TEXT("orientation"), bUp ? TEXT("pass") : TEXT("fail"), FString::Printf(TEXT("%.0f x %.0f x %.0f cm"), Out.Size.X, Out.Size.Y, Out.Size.Z), TEXT("standing up (Z)"),
				bUp ? TEXT("It stands up along Z.") : TEXT("It looks to be lying down: its height is small for what it is."), Static_(TEXT("upright")));
		}
		else
		{
			Add(Out, TEXT("orientation"), TEXT("pass"), FString::Printf(TEXT("%.0f x %.0f x %.0f cm"), Out.Size.X, Out.Size.Y, Out.Size.Z), TEXT("Z up"),
				TEXT("Imported Z up; Tripo models face +X, as Unreal's forward."));
		}
		if (Targets.bWeapon && Targets.Weapon.bSword && Static)
		{
			// The AAA stage leaves its sockets: their absence says it has not run on this geometry.
			const UStaticMeshSocket* Balance = Static->FindSocket(TEXT("BalancePoint"));
			const UStaticMeshSocket* Trail = Static->FindSocket(TEXT("Trail_Start"));
			const double Point = Balance && Trail ? Trail->RelativeLocation.Y - Balance->RelativeLocation.Y : 0.0;
			Add(Out, TEXT("sword_finish"), !Balance || !Trail ? TEXT("fail") : Point >= 10.0 && Point <= 13.0 ? TEXT("pass") : TEXT("warn"),
				Balance && Trail ? FString::Printf(TEXT("balance %.1f cm in front of the guard"), Point) : FString(TEXT("not finished")), TEXT("an exact blade, balanced 10-13 cm in front of the guard"),
				!Balance || !Trail ? TEXT("The AAA sword finishing has not run: the generated blade is still on it.") : Point >= 10.0 && Point <= 13.0 ? TEXT("Finished and balanced.")
				: TEXT("Finished; the hilt's weight keeps the balance outside 10-13 cm."), !Balance || !Trail ? TEXT("weapon_finish") : TEXT(""));
		}
		if (Targets.bWeapon && Static)
		{
			// A weapon's convention and proportions: generated ones come out lying any way and inflated.
			TArray<FVector3d> Points;
			Points.Reserve(Mesh.VertexCount());
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				Points.Add(Mesh.GetVertex(Vid));
			}
			const FWeaponShape Shape = MeasureWeapon(Points);
			const TCHAR* const Finish = TEXT("weapon_finish");
			if (!Shape.bValid)
			{
				Add(Out, TEXT("weapon_axis"), TEXT("warn"), FString(), TEXT("a weapon"), TEXT("It does not read as a weapon: no handle beside its widest section."));
			}
			else
			{
				const bool bPivot = Targets.Weapon.PivotFraction > 0.f ? FMath::Abs(Shape.PivotFraction - Targets.Weapon.PivotFraction) <= 0.05f : Shape.bPivotInGrip;
				const bool bAxis = Shape.Axis == 1 && Shape.bHandlePositive && Shape.ThicknessAxis == 2 && bPivot;
				Add(Out, TEXT("weapon_axis"), bAxis ? TEXT("pass") : TEXT("fail"),
					FString::Printf(TEXT("length on %s, handle toward %s, thickness on %s, pivot %.0f%% from the handle's end%s"), Shape.Axis == 0 ? TEXT("X") : Shape.Axis == 1 ? TEXT("Y") : TEXT("Z"),
						Shape.bHandlePositive ? TEXT("+") : TEXT("-"), Shape.ThicknessAxis == 0 ? TEXT("X") : Shape.ThicknessAxis == 1 ? TEXT("Y") : TEXT("Z"), Shape.PivotFraction * 100.f,
						Shape.bPivotInGrip ? TEXT(" (in the grip)") : TEXT("")),
					Targets.Weapon.PivotFraction > 0.f ? FString::Printf(TEXT("length on Y, head toward -Y, flat across X, pivot %.0f%% from the handle's end"), Targets.Weapon.PivotFraction * 100.f)
						: FString(TEXT("length on Y, head toward -Y, flat across X, pivot in the grip")),
					bAxis ? FString(TEXT("It follows the convention: ")) + Targets.Weapon.Source + TEXT(".") : FString(TEXT("It does not lie as the project's weapons do: ")) + Targets.Weapon.Source + TEXT("."), Finish);
				if (!Shape.bHeaded)
				{
					Add(Out, TEXT("weapon_blade"), Shape.BladeRatio > 0.3f ? TEXT("fail") : TEXT("pass"), FString::Printf(TEXT("%.1f cm thick, %.1f cm wide (%.2f)"), Shape.BladeThickness, Shape.BladeWidth, Shape.BladeRatio),
						TEXT("thickness under 0.3 of the width"), Shape.BladeRatio > 0.3f ? TEXT("A bulky blade: real ones are a fifth as thick as they are wide, or less.") : TEXT("A blade of real proportions."), Finish);
				}
				Add(Out, TEXT("weapon_grip"), Shape.GripCm > 5.f ? TEXT("fail") : TEXT("pass"), FString::Printf(TEXT("%.1f cm"), Shape.GripCm), TEXT("5 cm across at most"),
					Shape.GripCm > 5.f ? TEXT("Too fat for a hand to hold.") : TEXT("A hand holds it."), Finish);
				if (!Shape.bHeaded && Shape.Length < 130.f)
				{
					Add(Out, TEXT("weapon_guard"), Shape.GuardDepthCm > 4.f ? TEXT("fail") : TEXT("pass"), FString::Printf(TEXT("%.1f cm deep"), Shape.GuardDepthCm), TEXT("4 cm deep at most"),
						Shape.GuardDepthCm > 4.f ? TEXT("A guard like a brick: a one-handed weapon's is a few centimetres deep.") : TEXT("A guard of real depth."), Finish);
				}
			}
		}

		// UVs.
		double UVArea = 0.0, SurfaceArea = 0.0;
		{
			const FDynamicMeshUVOverlay* UVs = Mesh.HasAttributes() && Mesh.Attributes()->NumUVLayers() > 0 ? Mesh.Attributes()->PrimaryUV() : nullptr;
			int32 Set = 0, OutOfRange = 0, Corners = 0;
			constexpr int32 Grid = 256;
			TArray<uint8> Coverage;
			Coverage.SetNumZeroed(Grid * Grid);
			FBox2D UVBounds(ForceInit);
			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				FVector3d A, B, C;
				Mesh.GetTriVertices(Tid, A, B, C);
				SurfaceArea += 0.5 * (B - A).Cross(C - A).Length();
				if (!UVs || !UVs->IsSetTriangle(Tid))
				{
					continue;
				}
				++Set;
				const FIndex3i E = UVs->GetTriangle(Tid);
				const FVector2d P[3] = {FVector2d(UVs->GetElement(E.A)), FVector2d(UVs->GetElement(E.B)), FVector2d(UVs->GetElement(E.C))};
				const double Area = 0.5 * FMath::Abs((P[1].X - P[0].X) * (P[2].Y - P[0].Y) - (P[2].X - P[0].X) * (P[1].Y - P[0].Y));
				UVArea += Area;
				for (const FVector2d& Corner : P)
				{
					++Corners;
					OutOfRange += Corner.X < -0.001 || Corner.X > 1.001 || Corner.Y < -0.001 || Corner.Y > 1.001;
					UVBounds += FVector2D(Corner);
				}
				if (Area <= 0.0)
				{
					continue;
				}
				// Each grid cell whose centre the UV triangle covers; a cell covered twice is overlap.
				const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P[0].X, P[1].X, P[2].X) * Grid), 0, Grid - 1);
				const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P[0].X, P[1].X, P[2].X) * Grid), 0, Grid - 1);
				const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P[0].Y, P[1].Y, P[2].Y) * Grid), 0, Grid - 1);
				const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P[0].Y, P[1].Y, P[2].Y) * Grid), 0, Grid - 1);
				const double Sign = (P[1].X - P[0].X) * (P[2].Y - P[0].Y) - (P[2].X - P[0].X) * (P[1].Y - P[0].Y) > 0 ? 1.0 : -1.0;
				for (int32 Y = MinY; Y <= MaxY; ++Y)
				{
					for (int32 X = MinX; X <= MaxX; ++X)
					{
						const FVector2d Q((X + 0.5) / Grid, (Y + 0.5) / Grid);
						bool bInside = true;
						for (int32 Edge = 0; Edge < 3 && bInside; ++Edge)
						{
							const FVector2d& U = P[Edge];
							const FVector2d& V = P[(Edge + 1) % 3];
							bInside = Sign * ((V.X - U.X) * (Q.Y - U.Y) - (Q.X - U.X) * (V.Y - U.Y)) > 0.0;
						}
						if (bInside && Coverage[Y * Grid + X] < 255)
						{
							++Coverage[Y * Grid + X];
						}
					}
				}
			}
			int32 Covered = 0, Overlapped = 0;
			for (const uint8 Count : Coverage)
			{
				Covered += Count > 0;
				Overlapped += Count > 1;
			}
			const bool bPresent = UVs && Set == Mesh.TriangleCount() && UVArea > 0.0;
			const TCHAR* UVLocal = Static && !Targets.bTextures ? TEXT("auto_uv") : TEXT("");
			Add(Out, TEXT("uv_present"), bPresent ? TEXT("pass") : TEXT("fail"), FString::Printf(TEXT("%d of %d faces mapped"), Set, Mesh.TriangleCount()), TEXT("all"),
				bPresent ? TEXT("UV channel 0 covers every face.") : TEXT("UV channel 0 is missing or empty."), UVLocal, TEXT("smart_uv"));
			const double Overlap = Covered > 0 ? double(Overlapped) / Covered : 0.0;
			Add(Out, TEXT("uv_overlap"), !bPresent ? TEXT("skip") : Overlap > 0.05 ? TEXT("fail") : Overlap > 0.01 ? TEXT("warn") : TEXT("pass"),
				FString::Printf(TEXT("%.1f%%"), Overlap * 100.0), TEXT("under 1%"),
				Overlap > 0.05 ? TEXT("UV islands lie on top of each other: a baked, unique texture shows the same pixels in two places.") : TEXT("UV islands do not overlap."),
				UVLocal, TEXT("smart_uv"));
			Add(Out, TEXT("uv_range"), !bPresent ? TEXT("skip") : OutOfRange * 20 > Corners ? TEXT("warn") : TEXT("pass"), FString::Printf(TEXT("%d of %d corners outside 0-1"), OutOfRange, Corners),
				TEXT("inside 0-1"), TEXT("A baked texture covers 0-1; UVs outside it tile."), UVLocal, TEXT("smart_uv"));
		}

		// Textures and PBR.
		UTexture2D* BaseColor = nullptr;
		UTexture2D* Normal = nullptr;
		UTexture2D* RoughSource = nullptr;
		UTexture2D* MetalSource = nullptr;
		bool bPackedMR = false, bORM = false, bAllOurs = !Slots.IsEmpty();
		for (const FSlot& Slot : Slots)
		{
			bAllOurs &= Slot.bOurs;
			BaseColor = BaseColor ? BaseColor : Slot.Maps.FindRef(ERole::BaseColor);
			Normal = Normal ? Normal : Slot.Maps.FindRef(ERole::Normal);
			if (UTexture2D* Packed = Slot.Maps.FindRef(ERole::ORM))
			{
				RoughSource = MetalSource = Packed;
				bORM = true;
			}
			else if (UTexture2D* MR = Slot.Maps.FindRef(ERole::MetalRough))
			{
				RoughSource = MetalSource = MR;
				bPackedMR = true;
			}
			RoughSource = RoughSource ? RoughSource : Slot.Maps.FindRef(ERole::Roughness);
			MetalSource = MetalSource ? MetalSource : Slot.Maps.FindRef(ERole::Metallic);
		}
		const int32 Resolution = BaseColor ? FMath::Max(BaseColor->Source.GetSizeX(), BaseColor->Source.GetSizeY()) : 0;
		if (!Targets.bTextures)
		{
			Add(Out, TEXT("textures"), TEXT("skip"), BaseColor ? BaseColor->GetName() : FString(), TEXT(""), TEXT("Made without textures."));
		}
		else
		{
			const int32 Want = Targets.TexelDensity >= 10.f ? 2048 : 1024;
			Add(Out, TEXT("textures"), !BaseColor ? TEXT("fail") : Resolution < Want / 2 ? TEXT("warn") : TEXT("pass"),
				BaseColor ? FString::Printf(TEXT("%s, %d px"), *BaseColor->GetName(), Resolution) : FString(TEXT("none")), FString::Printf(TEXT("a base colour map, %d px or more"), Want),
				!BaseColor ? TEXT("No base colour texture on any material slot.") : Resolution < Want / 2 ? TEXT("The base colour map is small for this use.") : TEXT("Textured."),
				TEXT(""), TEXT("texture"));
			if (BaseColor && !BaseColor->SRGB)
			{
				Add(Out, TEXT("base_color_srgb"), TEXT("fail"), TEXT("linear"), TEXT("sRGB"), TEXT("The base colour map is read as linear data; it renders washed out."), TEXT("materials"));
			}
		}
		if (Targets.bTextures && Targets.bPbr)
		{
			const bool bPbr = Normal && RoughSource && MetalSource;
			Add(Out, TEXT("pbr"), bPbr ? TEXT("pass") : TEXT("fail"),
				FString::Printf(TEXT("normal %s, roughness %s, metal %s%s"), Normal ? TEXT("yes") : TEXT("no"), RoughSource ? TEXT("yes") : TEXT("no"), MetalSource ? TEXT("yes") : TEXT("no"),
					bORM ? TEXT(" (ORM)") : bPackedMR ? TEXT(" (glTF metallic-roughness, packed)") : TEXT("")),
				TEXT("normal, roughness and metal maps"), bPbr ? TEXT("Every PBR map is there.") : TEXT("PBR maps are missing."), TEXT(""), TEXT("texture"));
			if (Normal)
			{
				FStats Stats;
				const bool bRead = StatsOf(*Normal, Stats);
				// Tangent-space normals average near (0.5, 0.5, 1): mostly blue, slopes either way.
				const double Z = Stats.Mean.Z * 2.0 - 1.0;
				const bool bFormed = !bRead || (Z > 0.3 && FMath::Abs(Stats.Mean.X - 0.5) < 0.25 && FMath::Abs(Stats.Mean.Y - 0.5) < 0.25);
				const bool bCompressed = Normal->CompressionSettings == TC_Normalmap && !Normal->SRGB;
				// glTF's (and Tripo's NormalGL) maps are +Y; read unflipped, every bump lights from the wrong side.
				const bool bGreen = !Targets.bFromGltf || !IsOpenGLNormal(*Normal) || Normal->bFlipGreenChannel;
				Add(Out, TEXT("normal_map"), !bFormed || !bCompressed || !bGreen ? TEXT("fail") : TEXT("pass"),
					FString::Printf(TEXT("%s, mean (%.2f, %.2f, %.2f)%s"), bCompressed ? TEXT("normal-map compression") : TEXT("not set up as a normal map"), Stats.Mean.X, Stats.Mean.Y, Stats.Mean.Z,
						bGreen ? TEXT("") : TEXT(", green not flipped")),
					TEXT("normal-map compression, a tangent-space map (mostly blue), an OpenGL (+Y) map's green flipped"),
					!bFormed ? TEXT("It does not read as a tangent-space normal map.") : !bCompressed ? TEXT("Compressed as a colour; it must be a normal map.")
					: !bGreen ? TEXT("An OpenGL (+Y) normal map, glTF's convention, read without its green flipped: Unreal's is -Y, so bumps light from the wrong side.")
					: TEXT("A well-formed tangent-space normal map. glTF stores +Y (OpenGL) normals; its green is flipped to Unreal's -Y."),
					!bCompressed || !bGreen ? TEXT("materials") : TEXT(""), !bFormed ? TEXT("texture") : TEXT(""));
			}
			if (RoughSource)
			{
				FStats Stats;
				if (StatsOf(*RoughSource, Stats))
				{
					// ORM: green roughness, blue metal; glTF packed: green roughness, blue metal; separate maps: red.
					const bool bPackedSource = bORM || bPackedMR;
					const double Rough = bPackedSource ? Stats.Mean.Y : Stats.Mean.X;
					double MetalMid = 0.0;
					FStats MetalStats = Stats;
					if (!bPackedSource && MetalSource && MetalSource != RoughSource)
					{
						StatsOf(*MetalSource, MetalStats);
					}
					MetalMid = bPackedSource ? MetalStats.Mid.Z : MetalStats.Mid.X;
					const bool bLinear = !RoughSource->SRGB && (!MetalSource || !MetalSource->SRGB);
					const bool bRange = Rough > 0.02 && Rough < 0.99;
					Add(Out, TEXT("roughness_metal"), !bLinear ? TEXT("fail") : !bRange ? TEXT("warn") : MetalMid > 0.35 ? TEXT("warn") : TEXT("pass"),
						FString::Printf(TEXT("roughness mean %.2f, metal %.0f%% in between, %s"), Rough, MetalMid * 100.0, bLinear ? TEXT("linear") : TEXT("sRGB")),
						TEXT("linear data; roughness within 0-1 and not flat; metal mostly 0 or 1"),
						!bLinear ? TEXT("Roughness or metal is read as sRGB colour; it must be linear data.")
						: !bRange ? TEXT("Roughness is flat at an extreme: everything mirror-like or chalky.")
						: MetalMid > 0.35 ? TEXT("Much of the metal map is neither metal nor not: real materials are mostly 0 or 1.") : TEXT("Roughness and metal are in range."),
						!bLinear ? TEXT("materials") : TEXT(""), !bRange || MetalMid > 0.35 ? TEXT("texture") : TEXT(""));
				}
			}
			Add(Out, TEXT("material_setup"), bAllOurs ? TEXT("pass") : TEXT("warn"), bAllOurs ? TEXT("HyperAI master") : TEXT("as imported"),
				TEXT("a material instance of HyperAI's master on every slot"),
				bAllOurs ? TEXT("Each slot has an instance with ORM, tiling and a detail normal.") : TEXT("Imported materials: no ORM packing, tiling or detail normal."), TEXT("materials"));
		}
		if (Resolution > 0 && SurfaceArea > 0.0 && UVArea > 0.0 && Targets.TexelDensity > 0.f)
		{
			const double Density = Resolution * FMath::Sqrt(UVArea / SurfaceArea);
			Add(Out, TEXT("texel_density"), Density < Targets.TexelDensity * 0.5 ? TEXT("warn") : TEXT("pass"), FString::Printf(TEXT("%.2f px/cm"), Density),
				FString::Printf(TEXT("%.2f px/cm"), Targets.TexelDensity),
				Density < Targets.TexelDensity * 0.5 ? TEXT("Under half the density this use wants: blurry up close.") : TEXT("Enough texture for its size."), TEXT(""), TEXT("texture"));
		}

		// Nanite, collision, lightmap UVs: static meshes only.
		if (Static)
		{
			const bool bNanite = Static->GetNaniteSettings().bEnabled;
			bool bTranslucent = false;
			for (const FSlot& Slot : Slots)
			{
				bTranslucent |= Slot.Material && IsTranslucentBlendMode(Slot.Material->GetBlendMode());
			}
			const bool bSuits = !bTranslucent && Out.Triangles >= 5000;
			if (Targets.bWeapon && bNanite && !Targets.Weapon.bNanite)
			{
				Add(Out, TEXT("nanite"), TEXT("warn"), TEXT("on"), TEXT("off"), TEXT("Nanite is on; the project's weapons are drawn without it (one LOD)."), TEXT("weapon_finish"));
			}
			else
			{
				Add(Out, TEXT("nanite"), Targets.bNanite && bSuits && !bNanite ? TEXT("warn") : bNanite && bTranslucent ? TEXT("warn") : TEXT("pass"),
					bNanite ? TEXT("on") : TEXT("off"), Targets.bNanite ? TEXT("on") : TEXT("either"),
					bTranslucent ? TEXT("A translucent material: Nanite does not draw it.") : Targets.bNanite && bSuits && !bNanite ? TEXT("Dense and opaque: Nanite renders it for less.")
					: TEXT("Nanite is set sensibly."), Targets.bNanite && bSuits && !bNanite ? TEXT("nanite") : TEXT(""));
			}
			const UBodySetup* Body = Static->GetBodySetup();
			const bool bCollision = Body && (Body->AggGeom.GetElementCount() > 0 || Body->CollisionTraceFlag == CTF_UseComplexAsSimple);
			const bool bWanted = !Targets.Collision.IsEmpty() && Targets.Collision != TEXT("none");
			Add(Out, TEXT("collision"), !bWanted ? TEXT("pass") : bCollision ? TEXT("pass") : TEXT("fail"), bCollision ? TEXT("set") : TEXT("none"),
				bWanted ? Targets.Collision : FString(TEXT("none needed")), bWanted && !bCollision ? TEXT("Nothing collides with it.") : TEXT("Collision suits its use."),
				bWanted && !bCollision ? TEXT("collision") : TEXT(""));
			const bool bLightmap = Static->GetNumSourceModels() > 0 && (Static->GetSourceModel(0).BuildSettings.bGenerateLightmapUVs || Static->GetLightMapCoordinateIndex() > 0);
			Add(Out, TEXT("lightmap_uv"), bLightmap ? TEXT("pass") : TEXT("warn"), bLightmap ? TEXT("set") : TEXT("none"), TEXT("a lightmap channel"),
				bLightmap ? TEXT("It has lightmap UVs for baked lighting.") : TEXT("No lightmap UVs: fine under Lumen, needed for baked lighting."), TEXT("lightmap_uv"));
		}
		else if (Skeletal)
		{
			Add(Out, TEXT("collision"), Skeletal->GetPhysicsAsset() ? TEXT("pass") : TEXT("warn"), Skeletal->GetPhysicsAsset() ? TEXT("physics asset") : TEXT("none"),
				TEXT("a physics asset"), Skeletal->GetPhysicsAsset() ? TEXT("It has a physics asset.") : TEXT("No physics asset."));
		}

		// The visual check: Tripo's own render, the editor's capture, and the reference.
		if (!Targets.CaptureFolder.IsEmpty())
		{
			const FString Capture_ = FPaths::ConvertRelativePathToFull(Targets.CaptureFolder / FString::Printf(TEXT("capture_%s.png"), *Object.GetName()));
			if (Capture(Object, Capture_))
			{
				Out.Images.Add(Capture_);
				FString Note;
				if (IsPicture(Targets.ProviderRender))
				{
					Out.Images.Add(Targets.ProviderRender);
				}
				if (IsPicture(Targets.ReferenceImage))
				{
					Out.Images.Add(Targets.ReferenceImage);
					Out.VisualScore = CompareImages(Targets.ReferenceImage, Capture_, Note);
					Add(Out, TEXT("visual"), Out.VisualScore < 0.f ? TEXT("skip") : Out.VisualScore < 40.f ? TEXT("fail") : Out.VisualScore < 55.f ? TEXT("warn") : TEXT("pass"),
						Out.VisualScore < 0.f ? Note : FString::Printf(TEXT("%.0f"), Out.VisualScore), TEXT("55 or more"),
						Out.VisualScore < 0.f ? TEXT("Could not compare.") : Out.VisualScore < 40.f ? TEXT("The capture looks little like the reference: the shape is likely wrong.")
						: TEXT("The capture against the reference."), TEXT(""), TEXT("regenerate"));
				}
				else if (IsPicture(Targets.ProviderRender))
				{
					const float Score = CompareImages(Targets.ProviderRender, Capture_, Note);
					Add(Out, TEXT("visual"), Score < 0.f ? TEXT("skip") : Score < 35.f ? TEXT("warn") : TEXT("pass"), Score < 0.f ? Note : FString::Printf(TEXT("%.0f"), Score),
						TEXT("close to Tripo's render"),
						Score >= 0.f && Score < 35.f ? TEXT("The editor's capture differs from Tripo's own render: check the import (textures, orientation).")
						: TEXT("The editor's capture against Tripo's own render (a different camera, so only the look counts)."));
				}
				else
				{
					Add(Out, TEXT("visual"), TEXT("skip"), Capture_, TEXT(""), TEXT("Captured; there is no reference or Tripo render to compare with."));
				}
			}
			else
			{
				Add(Out, TEXT("visual"), TEXT("skip"), TEXT(""), TEXT(""), TEXT("The editor could not render a capture (no renderer in this session)."));
			}
		}

		TArray<FString> FailedIds, WarnedIds;
		for (const FHyperAIModelCheck& Check : Out.Checks)
		{
			Out.Failed += Check.Result == TEXT("fail");
			Out.Warnings += Check.Result == TEXT("warn");
			if (Check.Result == TEXT("fail"))
			{
				FailedIds.Add(Check.Id);
			}
			else if (Check.Result == TEXT("warn"))
			{
				WarnedIds.Add(Check.Id);
			}
		}
		Out.bPassed = Out.Failed == 0;
		Out.Summary = FString::Printf(TEXT("%s: %d failed%s, %d warnings%s; %d triangles, %d quads, %d part(s), %.0f x %.0f x %.0f cm."), Out.bPassed ? TEXT("passes") : TEXT("fails"),
			Out.Failed, FailedIds.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *FString::Join(FailedIds, TEXT(", "))), Out.Warnings,
			WarnedIds.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *FString::Join(WarnedIds, TEXT(", "))), Out.Triangles, Out.Quads, Out.Components, Out.Size.X, Out.Size.Y, Out.Size.Z);
		return true;
	}

	bool ApplyLocalFixes(UObject& Object, const FCheckTargets& Targets, const TArray<FString>& Fixes, TArray<FString>& OutNotes, FString& OutError)
	{
		UStaticMesh* Static = Cast<UStaticMesh>(&Object);
		static const TSet<FString> GeometryFixes = {TEXT("remove_bits"), TEXT("weapon_finish"), TEXT("simplify"), TEXT("orient"), TEXT("fill_holes"), TEXT("normals"), TEXT("degenerate"), TEXT("auto_uv"),
			TEXT("scale"), TEXT("pivot"), TEXT("upright")};
		bool bGeometry = false;
		for (const FString& Fix : Fixes)
		{
			bGeometry |= GeometryFixes.Contains(Fix);
		}
		if (bGeometry && !Static)
		{
			OutNotes.Add(TEXT("Geometry fixes are not made on a skeletal mesh in the editor (its skin weights would not follow); Tripo's convert can move its pivot and scale."));
			bGeometry = false;
		}
		// What an AAA sword's geometry leaves for its asset work further down.
		FSwordWork Sword;
		if (bGeometry)
		{
			FDynamicMesh3 Mesh;
			if (!Read(*Static, Mesh, OutError))
			{
				return false;
			}
			const int32 Before = Mesh.TriangleCount();
			if (Fixes.Contains(TEXT("remove_bits")))
			{
				const FWelded Welded = Weld(Mesh);
				const FTopology Topology = Analyse(Welded);
				const TArray<int32> Small = SmallComponents(Topology, Mesh.TriangleCount(), Welded.Diagonal);
				const TArray<int32> Doomed = TrianglesOf(Welded, Topology, TSet<int32>(Small));
				for (const int32 Tid : Doomed)
				{
					Mesh.RemoveTriangle(Tid, /*bRemoveIsolatedVertices=*/true, /*bPreserveManifold=*/false);
				}
				OutNotes.Add(FString::Printf(TEXT("remove_bits: removed %d stray bit(s), %d triangles."), Small.Num(), Doomed.Num()));
			}
			// A weapon is oriented, placed, scaled and thinned first; it is reduced to its budget last, after any holes are
			// capped, with UV seams kept so its textures still fit.
			bool bWeaponReduce = Fixes.Contains(TEXT("weapon_finish")) && Targets.bWeapon && ShapeWeapon(Mesh, Targets.Weapon, OutNotes);
			// A sword finished to AAA: its blade made exactly, its hilt reduced with the budget the blade leaves, so it is
			// not reduced again below.
			if (bWeaponReduce && Targets.Weapon.bSword && FinishSwordGeometry(Mesh, Object, Targets, OutNotes, Sword))
			{
				bWeaponReduce = false;
			}
			if (Fixes.Contains(TEXT("simplify")) && Targets.FaceLimit > 0 && Mesh.TriangleCount() > Targets.FaceLimit)
			{
				ReduceKeepingTextures(Mesh, Targets.FaceLimit, TEXT("simplify"), OutNotes);
			}
			if (Fixes.Contains(TEXT("degenerate")))
			{
				const double Floor = FMath::Square(Mesh.GetBounds().DiagonalLength()) * 1e-12;
				int32 Removed = 0;
				TArray<int32> Tids;
				for (const int32 Tid : Mesh.TriangleIndicesItr())
				{
					Tids.Add(Tid);
				}
				for (const int32 Tid : Tids)
				{
					if (Mesh.GetTriArea(Tid) <= Floor && Mesh.RemoveTriangle(Tid, true, false) == EMeshResult::Ok)
					{
						++Removed;
					}
				}
				OutNotes.Add(FString::Printf(TEXT("degenerate: removed %d zero-area face(s)."), Removed));
			}
			if (Fixes.Contains(TEXT("orient")))
			{
				const FWelded Welded = Weld(Mesh);
				const FTopology Topology = Analyse(Welded);
				int32 Turned = 0;
				for (const int32 Index : Topology.Flipped)
				{
					Turned += Mesh.ReverseTriOrientation(Welded.Tids[Index]) == EMeshResult::Ok;
				}
				OutNotes.Add(FString::Printf(TEXT("orient: turned %d face(s) to wind with their neighbours, outward."), Turned));
			}
			if (Fixes.Contains(TEXT("fill_holes")))
			{
				int32 Left = 0;
				const int32 Filled = FillHoles(Mesh, Left);
				OutNotes.Add(FString::Printf(TEXT("fill_holes: capped %d hole(s)%s."), Filled, Left > 0 ? *FString::Printf(TEXT("; %d too large or awkward, left for Tripo's completion"), Left) : TEXT("")));
			}
			if (Fixes.Contains(TEXT("auto_uv")))
			{
				FString Error;
				OutNotes.Add(AutoUV(Mesh, Error) ? FString(TEXT("auto_uv: unwrapped one UV layout with xatlas (there were no textures to lose).")) : TEXT("auto_uv: ") + Error);
			}
			if (Fixes.Contains(TEXT("upright")))
			{
				const FAxisAlignedBox3d Box = Mesh.GetBounds();
				const FVector3d Size = Box.Max - Box.Min;
				// The longest flat axis becomes up: a figure lying along X or Y is stood on its feet.
				const bool bAlongX = Size.X >= Size.Y;
				for (const int32 Vid : Mesh.VertexIndicesItr())
				{
					const FVector3d P = Mesh.GetVertex(Vid);
					Mesh.SetVertex(Vid, bAlongX ? FVector3d(-P.Z, P.Y, P.X) : FVector3d(P.X, -P.Z, P.Y));
				}
				OutNotes.Add(FString::Printf(TEXT("upright: stood it up, its %s axis now Z."), bAlongX ? TEXT("X") : TEXT("Y")));
			}
			if (Fixes.Contains(TEXT("normals")) || Fixes.Contains(TEXT("orient")) || Fixes.Contains(TEXT("upright")) || Fixes.Contains(TEXT("fill_holes")))
			{
				if (Mesh.HasAttributes() && Mesh.Attributes()->PrimaryNormals())
				{
					FMeshNormals::QuickRecomputeOverlayNormals(Mesh);
					OutNotes.Add(TEXT("normals: recomputed from the faces, keeping hard edges."));
				}
			}
			if (Fixes.Contains(TEXT("scale")) && Targets.SizeMeters > 0.f)
			{
				const FAxisAlignedBox3d Box = Mesh.GetBounds();
				const double Measure = Upright(Targets.Use) ? Box.Height() : Box.MaxDim();
				if (Measure > KINDA_SMALL_NUMBER)
				{
					const double Factor = Targets.SizeMeters * 100.0 / Measure;
					for (const int32 Vid : Mesh.VertexIndicesItr())
					{
						Mesh.SetVertex(Vid, Mesh.GetVertex(Vid) * Factor);
					}
					OutNotes.Add(FString::Printf(TEXT("scale: scaled by %.3f to %.0f cm."), Factor, Targets.SizeMeters * 100.0));
				}
			}
			if (Fixes.Contains(TEXT("pivot")) || Fixes.Contains(TEXT("scale")) || Fixes.Contains(TEXT("upright")))
			{
				const FAxisAlignedBox3d Box = Mesh.GetBounds();
				const FVector3d Offset(-Box.Center().X, -Box.Center().Y, -Box.Min.Z);
				for (const int32 Vid : Mesh.VertexIndicesItr())
				{
					Mesh.SetVertex(Vid, Mesh.GetVertex(Vid) + Offset);
				}
				OutNotes.Add(TEXT("pivot: moved to the bottom centre."));
			}
			if (bWeaponReduce && Targets.Weapon.TargetTris > 0 && Mesh.TriangleCount() > Targets.Weapon.TargetTris)
			{
				ReduceKeepingTextures(Mesh, Targets.Weapon.TargetTris, TEXT("weapon_finish"), OutNotes);
			}
			Mesh.CompactInPlace();
			Static->Modify();
			TMap<int32, FName> NewSlots;
			if (Sword.bDone)
			{
				NewSlots.Add(Sword.BladeMaterial, FName(TEXT("Blade")));
			}
			if (!Write(*Static, Mesh, NewSlots, OutError))
			{
				return false;
			}
			OutNotes.Add(FString::Printf(TEXT("geometry written: %d triangles, from %d."), Mesh.TriangleCount(), Before));
		}
		if (Static)
		{
			bool bChanged = bGeometry;
			if (Fixes.Contains(TEXT("collision")) && !Targets.Collision.IsEmpty() && Targets.Collision != TEXT("none"))
			{
				Static->Modify();
				Static->CreateBodySetup();
				UBodySetup* Body = Static->GetBodySetup();
				Body->Modify();
				if (Targets.Collision == TEXT("complex"))
				{
					Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
					OutNotes.Add(TEXT("collision: uses the mesh itself as its collision (complex as simple), to walk on."));
				}
				else
				{
					Body->AggGeom.EmptyElements();
					const FBox Box = Static->GetBoundingBox();
					FKBoxElem Elem(Box.GetSize().X, Box.GetSize().Y, Box.GetSize().Z);
					Elem.Center = Box.GetCenter();
					Body->AggGeom.BoxElems.Add(Elem);
					Body->CollisionTraceFlag = CTF_UseDefault;
					OutNotes.Add(TEXT("collision: a box around it."));
				}
				Body->InvalidatePhysicsData();
				Body->CreatePhysicsMeshes();
				bChanged = true;
			}
			if (Fixes.Contains(TEXT("lightmap_uv")) && Static->GetNumSourceModels() > 0)
			{
				Static->Modify();
				FStaticMeshSourceModel& Model = Static->GetSourceModel(0);
				Model.BuildSettings.bGenerateLightmapUVs = true;
				Model.BuildSettings.SrcLightmapIndex = 0;
				Model.BuildSettings.DstLightmapIndex = 1;
				Model.BuildSettings.MinLightmapResolution = 64;
				Static->SetLightMapCoordinateIndex(1);
				Static->SetLightMapResolution(64);
				OutNotes.Add(TEXT("lightmap_uv: generates lightmap UVs into channel 1."));
				bChanged = true;
			}
			if (Fixes.Contains(TEXT("nanite")))
			{
				Static->Modify();
				FMeshNaniteSettings Nanite = Static->GetNaniteSettings();
				Nanite.bEnabled = true;
				Static->SetNaniteSettings(Nanite);
				OutNotes.Add(TEXT("nanite: on."));
				bChanged = true;
			}
			if (Fixes.Contains(TEXT("weapon_finish")) && Targets.bWeapon)
			{
				// As the project's weapons are drawn: one LOD, Nanite as the convention says (off unless told), and the
				// render data rebuilt from it (PostEditChange below), or Nanite's coarse fallback would stay.
				Static->Modify();
				FMeshNaniteSettings Nanite = Static->GetNaniteSettings();
				Nanite.bEnabled = Targets.Weapon.bNanite;
				Static->SetNaniteSettings(Nanite);
				if (Static->GetNumSourceModels() > 1)
				{
					Static->SetNumSourceModels(1);
				}
				OutNotes.Add(FString::Printf(TEXT("weapon_finish: one LOD, Nanite %s."), Targets.Weapon.bNanite ? TEXT("on") : TEXT("off")));
				if (Sword.bDone)
				{
					FinishSwordAsset(*Static, Targets, Sword, OutNotes);
				}
				CapWeaponTextures(*Static, Targets.Weapon, OutNotes);
				bChanged = true;
			}
			if (bChanged)
			{
				Static->PostEditChange();
				Static->MarkPackageDirty();
				if (!FHyperAIStudioSourceControl::SaveAsset(*Static, OutError))
				{
					return false;
				}
			}
		}
		if (Fixes.Contains(TEXT("materials")))
		{
			FMaterialResult Materials;
			if (!BuildMaterials(Object, false, Materials, OutError))
			{
				return false;
			}
			OutNotes.Append(Materials.Notes);
		}
		return true;
	}

	bool BuildMaterials(UObject& Object, const bool bPerPartSlots, FMaterialResult& Out, FString& OutError)
	{
		UStaticMesh* Static = Cast<UStaticMesh>(&Object);
		USkeletalMesh* Skeletal = Cast<USkeletalMesh>(&Object);
		if (!Static && !Skeletal)
		{
			OutError = TEXT("materials are built for a static or skeletal mesh");
			return false;
		}
		UMaterialInterface* Parent = Master(OutError);
		if (!Parent)
		{
			return false;
		}
		const FString Folder = FPackageName::GetLongPackagePath(Object.GetOutermost()->GetName());
		const FString MeshName = CleanName(Object.GetName());
		TArray<FSlot> Slots = SlotsOf(Object);

		// A parts model in one slot gets a slot a part, each with its own instance of the same maps, to tint or swap alone.
		if (bPerPartSlots && Static && Slots.Num() == 1)
		{
			FDynamicMesh3 Mesh;
			if (Read(*Static, Mesh, OutError))
			{
				const FWelded Welded = Weld(Mesh);
				const FTopology Topology = Analyse(Welded);
				const int32 Parts = Topology.ComponentTris.Num();
				if (Parts >= 2 && Parts <= 32 && Mesh.HasAttributes())
				{
					if (!Mesh.Attributes()->HasMaterialID())
					{
						Mesh.Attributes()->EnableMaterialID();
					}
					for (int32 Index = 0; Index < Welded.Tids.Num(); ++Index)
					{
						Mesh.Attributes()->GetMaterialID()->SetValue(Welded.Tids[Index], Topology.Component[Index]);
					}
					TMap<int32, FName> Names;
					for (int32 Part = 0; Part < Parts; ++Part)
					{
						Names.Add(Part, FName(*FString::Printf(TEXT("Part_%02d"), Part)));
					}
					Static->Modify();
					if (Write(*Static, Mesh, Names, OutError))
					{
						UMaterialInterface* Shared = Slots[0].Material;
						Static->GetStaticMaterials().Reset();
						for (int32 Part = 0; Part < Parts; ++Part)
						{
							Static->GetStaticMaterials().Add(FStaticMaterial(Shared, Names[Part], Names[Part]));
						}
						Static->PostEditChange();
						const TMap<ERole, UTexture2D*> Maps = Slots[0].Maps;
						Slots = SlotsOf(Object);
						for (FSlot& Slot : Slots)
						{
							Slot.Maps = Maps;
						}
						Out.Notes.Add(FString::Printf(TEXT("materials: one slot a part, %d parts."), Parts));
					}
				}
			}
		}

		for (int32 Index = 0; Index < Slots.Num(); ++Index)
		{
			FSlot& Slot = Slots[Index];
			const UMaterialInterface* SlotBase = Slot.Material ? Slot.Material->GetMaterial() : nullptr;
			if (SlotBase && SlotBase->GetPathName().StartsWith(BladeSteelPath))
			{
				// An AAA sword's blade: its clean steel is its material already.
				continue;
			}
			int32 Changed = 0;
			for (const TPair<ERole, UTexture2D*>& Map : Slot.Maps)
			{
				if (Map.Value && !Map.Value->GetPathName().StartsWith(TEXT("/Engine/")) && SetUp(*Map.Value, Map.Key))
				{
					++Changed;
					FString Error;
					FHyperAIStudioSourceControl::SaveAsset(*Map.Value, Error);
				}
				Out.Textures.AddUnique(Map.Value ? Map.Value->GetPathName() : FString());
			}
			const FString SlotName = CleanName(Slot.Name.IsNone() ? FString::Printf(TEXT("Slot%d"), Index) : Slot.Name.ToString());
			UTexture2D* ORM = Slot.Maps.FindRef(ERole::ORM);
			if (!ORM)
			{
				FString Error;
				ORM = PackORM(Slot, Folder / FString::Printf(TEXT("T_%s_%s_ORM"), *MeshName, *SlotName), Error);
				if (ORM)
				{
					Out.Textures.AddUnique(ORM->GetPathName());
				}
				else if (!Error.IsEmpty())
				{
					Out.Notes.Add(TEXT("materials: no ORM: ") + Error);
				}
			}
			const FString InstancePackage = Folder / FString::Printf(TEXT("MI_%s_%s"), *MeshName, *SlotName);
			UObject* Other = nullptr;
			UMaterialInstanceConstant* Instance = FindOrLoad<UMaterialInstanceConstant>(InstancePackage, Other);
			if (Other)
			{
				Out.Notes.Add(FString::Printf(TEXT("materials: %s is something else; slot %s kept its material."), *InstancePackage, *SlotName));
				continue;
			}
			const bool bNew = Instance == nullptr;
			if (bNew)
			{
				Instance = NewObject<UMaterialInstanceConstant>(CreatePackage(*InstancePackage), *FPackageName::GetShortName(InstancePackage), RF_Public | RF_Standalone);
			}
			else
			{
				Instance->Modify();
			}
			Instance->SetParentEditorOnly(Parent);
			if (UTexture2D* Color = Slot.Maps.FindRef(ERole::BaseColor))
			{
				Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BaseColor")), Color);
			}
			if (UTexture2D* NormalMap = Slot.Maps.FindRef(ERole::Normal))
			{
				Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Normal")), NormalMap);
			}
			if (ORM)
			{
				Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("ORM")), ORM);
			}
			else
			{
				// No maps: a plain, non-metal surface.
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("RoughnessScale")), 0.8f);
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MetallicScale")), 0.f);
			}
			Instance->PostEditChange();
			if (bNew)
			{
				FAssetRegistryModule::AssetCreated(Instance);
			}
			Instance->MarkPackageDirty();
			if (!FHyperAIStudioSourceControl::SaveAsset(*Instance, OutError))
			{
				return false;
			}
			Out.Materials.Add(Instance->GetPathName());
			if (Static)
			{
				Static->SetMaterial(Index, Instance);
			}
			else
			{
				Skeletal->Modify();
				Skeletal->GetMaterials()[Index].MaterialInterface = Instance;
			}
			Out.Notes.Add(FString::Printf(TEXT("materials: slot %s uses %s (%d map(s) set up%s)."), *SlotName, *Instance->GetName(), Changed, ORM ? TEXT(", ORM packed") : TEXT("")));
		}
		Out.Textures.Remove(FString());
		if (Skeletal)
		{
			Skeletal->PostEditChange();
		}
		else
		{
			Static->PostEditChange();
		}
		Object.MarkPackageDirty();
		return FHyperAIStudioSourceControl::SaveAsset(Object, OutError);
	}
}
