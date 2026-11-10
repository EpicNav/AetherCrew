// Games by Hyper 2026.

#include "HyperAIStudioModelingBake.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "ContentStreaming.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshBakeFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshSamplingFunctions.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioModelingUV.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioResultPreview.h"
#include "HyperAIStudioSourceControl.h"
#include "ImageUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PreviewScene.h"
#include "RenderingThread.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace HyperAIStudio::Modeling::Bake
{
	namespace
	{
		/** Where HyperAI's baked-surface material lives, made on first use beside the modelling placeholders' material. */
		const TCHAR* const SurfaceMaterialPath = TEXT("/Game/HyperAI/Modeling/M_HyperAIBakedSurface");
		const TCHAR* const WhiteTexture = TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");
		const TCHAR* const FlatNormalTexture = TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal");

		struct FMapInfo
		{
			EMap Map;
			const TCHAR* Name;
			const TCHAR* Suffix;
		};

		const FMapInfo MapTable[] = {
			{EMap::Normal, TEXT("normal"), TEXT("N")},
			{EMap::ObjectNormal, TEXT("object_normal"), TEXT("ON")},
			{EMap::BentNormal, TEXT("bent_normal"), TEXT("BN")},
			{EMap::AO, TEXT("ao"), TEXT("AO")},
			{EMap::Curvature, TEXT("curvature"), TEXT("CV")},
			{EMap::Position, TEXT("position"), TEXT("P")},
			{EMap::Height, TEXT("height"), TEXT("H")},
			{EMap::VertexColor, TEXT("vertex_color"), TEXT("VC")},
			{EMap::Texture, TEXT("texture"), TEXT("BC")},
			{EMap::MultiTexture, TEXT("multi_texture"), TEXT("BC")},
			{EMap::Material, TEXT("material"), TEXT("BC")},
			{EMap::ORM, TEXT("orm"), TEXT("ORM")},
			{EMap::MaterialMRS, TEXT("material_mrs"), TEXT("MRS")},
		};

		FString PackageOf(const FString& Reference)
		{
			FString Package = Reference.TrimStartAndEnd();
			int32 Dot = INDEX_NONE;
			if (Package.FindChar(TEXT('.'), Dot)) Package.LeftInline(Dot);
			return Package;
		}

		template <typename ObjectType>
		ObjectType* FindOrLoad(const FString& Package, UObject*& OutOther)
		{
			OutOther = nullptr;
			if (!FHyperAIStudioNames::IsObjectPath(Package, TEXT("Path"))) return nullptr;
			const FString ObjectPath = Package + TEXT(".") + FPackageName::GetShortName(Package);
			UObject* Found = FindObject<UObject>(nullptr, *ObjectPath);
			if (!Found && FPackageName::DoesPackageExist(Package)) Found = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Found && FPackageName::DoesPackageExist(Package)) OutOther = LoadPackage(nullptr, *Package, LOAD_NoWarn | LOAD_Quiet);
			ObjectType* Typed = Cast<ObjectType>(Found);
			if (Found && !Typed) OutOther = Found;
			return Typed;
		}

		EGeometryScriptBakeResolution ResolutionOf(const int32 Pixels)
		{
			switch (Pixels)
			{
			case 16: return EGeometryScriptBakeResolution::Resolution16;
			case 32: return EGeometryScriptBakeResolution::Resolution32;
			case 64: return EGeometryScriptBakeResolution::Resolution64;
			case 128: return EGeometryScriptBakeResolution::Resolution128;
			case 256: return EGeometryScriptBakeResolution::Resolution256;
			case 512: return EGeometryScriptBakeResolution::Resolution512;
			case 2048: return EGeometryScriptBakeResolution::Resolution2048;
			case 4096: return EGeometryScriptBakeResolution::Resolution4096;
			case 8192: return EGeometryScriptBakeResolution::Resolution8192;
			default: return EGeometryScriptBakeResolution::Resolution1024;
			}
		}

		EGeometryScriptBakeSamplesPerPixel SamplesOf(const int32 Samples)
		{
			switch (Samples)
			{
			case 4: return EGeometryScriptBakeSamplesPerPixel::Sample4;
			case 16: return EGeometryScriptBakeSamplesPerPixel::Sample16;
			case 64: return EGeometryScriptBakeSamplesPerPixel::Sample64;
			case 256: return EGeometryScriptBakeSamplesPerPixel::Samples256;
			default: return EGeometryScriptBakeSamplesPerPixel::Sample1;
			}
		}

		FGeometryScriptBakeTypeOptions TypeOf(const FJob& Job, const EMap Map)
		{
			using UBake = UGeometryScriptLibrary_MeshBakeFunctions;
			switch (Map)
			{
			case EMap::Normal: return UBake::MakeBakeTypeTangentNormal();
			case EMap::ObjectNormal: return UBake::MakeBakeTypeObjectNormal();
			case EMap::BentNormal: return UBake::MakeBakeTypeBentNormal(Job.Rays, Job.OcclusionDistance, 180.f, EGeometryScriptBakeNormalSpace::Tangent);
			case EMap::AO: return UBake::MakeBakeTypeAmbientOcclusion(Job.Rays, Job.OcclusionDistance, 180.f, 15.f);
			case EMap::Curvature: return UBake::MakeBakeTypeCurvature();
			case EMap::Position: return UBake::MakeBakeTypePosition();
			case EMap::Height: return UBake::MakeBakeTypeHeight(EGeometryScriptBakeHeightRangeMode::Absolute, -Job.ProjectionDistance, Job.ProjectionDistance);
			case EMap::VertexColor: return UBake::MakeBakeTypeVertexColor();
			case EMap::Texture: return UBake::MakeBakeTypeTexture(Job.SourceTexture, Job.SourceChannel);
			case EMap::MultiTexture: return UBake::MakeBakeTypeMultiTexture(Job.SlotTextures, Job.SourceChannel);
			default: return FGeometryScriptBakeTypeOptions();
			}
		}

		/** A baked texture's source pixels, which Geometry Script fills as BGRA8 (sRGB-encoded for colour maps), FColor's layout. */
		bool ImageOf(UTexture2D* Texture, const EMap Map, FImage& Out, FString& OutError)
		{
			if (!Texture || !Texture->Source.IsValid() || Texture->Source.GetFormat() != TSF_BGRA8)
			{
				OutError = FString::Printf(TEXT("the %s bake gave no 8-bit image"), MapName(Map));
				return false;
			}
			TArray64<uint8> Bytes;
			const int32 Width = Texture->Source.GetSizeX();
			if (!Texture->Source.GetMipData(Bytes, 0) || Width <= 0 || Texture->Source.GetSizeY() != Width || Bytes.Num() != int64(Width) * Width * 4)
			{
				OutError = FString::Printf(TEXT("the %s bake's pixels could not be read"), MapName(Map));
				return false;
			}
			Out.Map = Map;
			Out.Size = Width;
			Out.Pixels.SetNumUninitialized(Width * Width);
			FMemory::Memcpy(Out.Pixels.GetData(), Bytes.GetData(), Bytes.Num());
			return true;
		}

		/**
		 * The source's own material carried across: the source drawn alone in a preview world of its own (the level is not
		 * touched), from cameras all round it, and the pictures baked into the target's UVs with Geometry Script's
		 * BakeTextureFromRenderCaptures: base colour, tangent-space normal, and metallic, roughness and specular packed.
		 */
		bool Capture(const FJob& Job, TArray<FImage>& OutImages, FString& OutError)
		{
			if (!CanCapture())
			{
				OutError = TEXT("material draws the source with render captures, and this editor runs without rendering (null RHI); bake texture from its base colour instead");
				return false;
			}
			if (!Job.SourceAsset)
			{
				OutError = TEXT("material needs the source static mesh");
				return false;
			}
			const TStrongObjectPtr<UGeometryScriptDebug> Debug(NewObject<UGeometryScriptDebug>(GetTransientPackage()));
			FPreviewScene Scene(FPreviewScene::ConstructionValues().SetForceMipsResident(true).SetCreatePhysicsScene(false).AllowAudioPlayback(false));
			UWorld* World = Scene.GetWorld();
			FActorSpawnParameters Spawn;
			Spawn.ObjectFlags = RF_Transient;
			AStaticMeshActor* Actor = World ? World->SpawnActor<AStaticMeshActor>(Spawn) : nullptr;
			if (!Actor || !Actor->GetStaticMeshComponent())
			{
				OutError = TEXT("the preview world to draw the source in could not be made");
				return false;
			}
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetStaticMesh(Job.SourceAsset);
			// Drawn finished: shaders compiled and every mip of its textures in memory.
			FAssetCompilingManager::Get().FinishAllCompilation();
			Component->MarkRenderStateDirty();
			IStreamingManager::Get().StreamAllResources(5.0f);
			FlushRenderingCommands();
			FGeometryScriptRenderCaptureCamerasForBoxOptions CameraOptions;
			CameraOptions.Resolution = FMath::Clamp(Job.Resolution, 256, 2048);
			CameraOptions.bViewFromBoxFaces = true;
			CameraOptions.bViewFromUpperCorners = true;
			CameraOptions.bViewFromLowerCorners = true;
			FGeometryScriptBakeRenderCaptureOptions Options;
			UGeometryScriptLibrary_MeshSamplingFunctions::ComputeRenderCaptureCamerasForBox(Options.Cameras, Job.SourceAsset->GetBoundingBox().ExpandBy(1.0), CameraOptions, Debug.Get());
			Options.Resolution = ResolutionOf(Job.Resolution);
			Options.SamplesPerPixel = SamplesOf(Job.Samples);
			Options.bBaseColorMap = true;
			Options.bNormalMap = true;
			Options.bPackedMRSMap = true;
			Options.bEmissiveMap = false;
			Options.bOpacityMap = false;
			Options.bSubsurfaceColorMap = false;
			FGeometryScriptBakeTargetMeshOptions Target;
			Target.TargetUVLayer = Job.TargetChannel;
			const FGeometryScriptRenderCaptureTextures Out = UGeometryScriptLibrary_MeshBakeFunctions::BakeTextureFromRenderCaptures(Job.Target, FTransform::Identity, Target,
				{Actor}, Options, Debug.Get());
			Actor->Destroy();
			for (const FGeometryScriptDebugMessage& Message : Debug->Messages)
			{
				if (Message.MessageType == EGeometryScriptDebugMessageType::ErrorMessage)
				{
					OutError = FString::Printf(TEXT("Geometry Script's render-capture bake failed: %s"), *Message.Message.ToString());
					return false;
				}
			}
			const TPair<UTexture2D*, EMap> Captured[] = {{Out.BaseColorMap, EMap::Texture}, {Out.NormalMap, EMap::Normal}, {Out.PackedMRSMap, EMap::MaterialMRS}};
			for (const TPair<UTexture2D*, EMap>& Entry : Captured)
			{
				FImage& Image = OutImages.AddDefaulted_GetRef();
				if (!ImageOf(Entry.Key, Entry.Value, Image, OutError))
				{
					OutError = FString::Printf(TEXT("the material capture drew nothing for %s (%s)"), MapName(Entry.Value), *OutError);
					return false;
				}
			}
			return true;
		}

		bool Bake(const FJob& Job, const TArray<EMap>& Maps, TArray<FImage>& OutImages, FString& OutError)
		{
			if (Maps.IsEmpty()) return true;
			const TStrongObjectPtr<UGeometryScriptDebug> Debug(NewObject<UGeometryScriptDebug>(GetTransientPackage()));
			FGeometryScriptBakeTargetMeshOptions Target;
			Target.TargetUVLayer = Job.TargetChannel;
			FGeometryScriptBakeSourceMeshOptions Source;
			const bool bCarriesNormals = Maps.Contains(EMap::Normal) || Maps.Contains(EMap::ObjectNormal);
			if (Job.SourceNormalMap && bCarriesNormals)
			{
				Source.SourceNormalMap = Job.SourceNormalMap;
				Source.SourceNormalUVLayer = Job.SourceNormalChannel;
				Source.SourceNormalSpace = EGeometryScriptBakeNormalSpace::Tangent;
			}
			TArray<FGeometryScriptBakeTypeOptions> Types;
			for (const EMap Map : Maps) Types.Add(TypeOf(Job, Map));
			FGeometryScriptBakeTextureOptions Options;
			Options.Resolution = ResolutionOf(Job.Resolution);
			Options.BitDepth = EGeometryScriptBakeBitDepth::ChannelBits8;
			Options.GutterSize = FMath::Max(0, Job.Padding);
			Options.SamplesPerPixel = SamplesOf(Job.Samples);
			Options.ProjectionDistance = Job.ProjectionDistance;
			Options.bProjectionInWorldSpace = false;
			// Transient textures in the transient package; their pixels are copied out and they are left to garbage collection.
			TArray<UTexture2D*> Textures = UGeometryScriptLibrary_MeshBakeFunctions::BakeTexture(Job.Target, FTransform::Identity, Target, Job.Source, FTransform::Identity, Source, Types,
				Options, Debug.Get());
			for (const FGeometryScriptDebugMessage& Message : Debug->Messages)
			{
				if (Message.MessageType == EGeometryScriptDebugMessageType::ErrorMessage)
				{
					OutError = FString::Printf(TEXT("Geometry Script's bake failed: %s"), *Message.Message.ToString());
					return false;
				}
			}
			if (Textures.Num() != Maps.Num())
			{
				OutError = FString::Printf(TEXT("Geometry Script's bake gave %d map(s) of %d"), Textures.Num(), Maps.Num());
				return false;
			}
			for (int32 Index = 0; Index < Maps.Num(); ++Index)
			{
				FImage& Image = OutImages.AddDefaulted_GetRef();
				if (!ImageOf(Textures[Index], Maps[Index], Image, OutError)) return false;
			}
			return true;
		}

		/** A texture parameter whose name says what it holds, best match first. */
		UTexture2D* TextureByName(const UMaterialInterface* Material, const TArray<const TCHAR*>& Words, const TArray<const TCHAR*>& Not)
		{
			if (!Material) return nullptr;
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Material->GetAllTextureParameterInfo(Infos, Ids);
			for (const TCHAR* Word : Words)
			{
				for (const FMaterialParameterInfo& Info : Infos)
				{
					const FString Name = Info.Name.ToString().ToLower().Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));
					if (!Name.Contains(Word)) continue;
					if (Not.ContainsByPredicate([&Name](const TCHAR* Skip) { return Name.Contains(Skip); })) continue;
					UTexture* Value = nullptr;
					if (Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Info), Value))
					{
						if (UTexture2D* Texture = Cast<UTexture2D>(Value)) return Texture;
					}
				}
			}
			return nullptr;
		}

		bool IsDefaultTexture(const UTexture* Texture)
		{
			return !Texture || Texture->GetPathName().StartsWith(TEXT("/Engine/"));
		}
	}

	bool ParseMap(const FString& Text, EMap& Out)
	{
		const FString Name = Text.TrimStartAndEnd().ToLower();
		for (const FMapInfo& Info : MapTable)
		{
			if (Info.Map != EMap::ORM && Info.Map != EMap::MaterialMRS && Name == Info.Name)
			{
				Out = Info.Map;
				return true;
			}
		}
		return false;
	}

	const TCHAR* MapName(const EMap Map)
	{
		for (const FMapInfo& Info : MapTable)
		{
			if (Info.Map == Map) return Info.Name;
		}
		return TEXT("?");
	}

	const TCHAR* MapSuffix(const EMap Map)
	{
		for (const FMapInfo& Info : MapTable)
		{
			if (Info.Map == Map) return Info.Suffix;
		}
		return TEXT("X");
	}

	FString MapNames()
	{
		TArray<FString> Names;
		for (const FMapInfo& Info : MapTable)
		{
			if (Info.Map != EMap::ORM && Info.Map != EMap::MaterialMRS) Names.Add(Info.Name);
		}
		return FString::Join(Names, TEXT(", "));
	}

	FTextureKind KindOf(const EMap Map)
	{
		FTextureKind Kind;
		switch (Map)
		{
		case EMap::Normal:
		case EMap::BentNormal:
			Kind.Compression = TC_Normalmap;
			Kind.CompressionName = TEXT("TC_Normalmap");
			Kind.Group = TEXTUREGROUP_WorldNormalMap;
			break;
		case EMap::AO:
		case EMap::Curvature:
		case EMap::ORM:
		case EMap::MaterialMRS:
			Kind.Compression = TC_Masks;
			Kind.CompressionName = TEXT("TC_Masks");
			break;
		case EMap::Height:
			Kind.Compression = TC_Grayscale;
			Kind.CompressionName = TEXT("TC_Grayscale");
			break;
		case EMap::Position:
		case EMap::ObjectNormal:
			Kind.Compression = TC_VectorDisplacementmap;
			Kind.CompressionName = TEXT("TC_VectorDisplacementmap");
			break;
		default:
			Kind.bSRGB = true;
			break;
		}
		return Kind;
	}

	bool IsResolution(const int32 Pixels)
	{
		return Pixels >= 16 && Pixels <= 8192 && FMath::IsPowerOfTwo(Pixels);
	}

	bool IsSampleCount(const int32 Samples)
	{
		return Samples == 1 || Samples == 4 || Samples == 16 || Samples == 64 || Samples == 256;
	}

	bool Run(const FJob& Job, TArray<FImage>& OutImages, FString& OutError)
	{
		OutImages.Reset();
		if (!Job.Target || !Job.Source)
		{
			OutError = TEXT("the bake has no target or source mesh");
			return false;
		}
		// Geometry Script samples one source texture per bake, so a texture transfer bakes on its own, and the material
		// is drawn and captured on its own.
		TArray<EMap> Together;
		TArray<EMap> Alone;
		for (const EMap Map : Job.Maps)
		{
			if (Map != EMap::Material) (Map == EMap::Texture ? Alone : Together).Add(Map);
		}
		TArray<FImage> First;
		TArray<FImage> Second;
		TArray<FImage> Captured;
		if (!Bake(Job, Together, First, OutError) || !Bake(Job, Alone, Second, OutError)) return false;
		if (Job.Maps.Contains(EMap::Material) && !Capture(Job, Captured, OutError)) return false;
		for (const EMap Map : Job.Maps)
		{
			if (Map == EMap::Material)
			{
				OutImages.Append(MoveTemp(Captured));
				continue;
			}
			TArray<FImage>& From = Map == EMap::Texture ? Second : First;
			const int32 Index = From.IndexOfByPredicate([Map](const FImage& Image) { return Image.Map == Map; });
			if (Index == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("the %s map was not baked"), MapName(Map));
				return false;
			}
			OutImages.Add(MoveTemp(From[Index]));
			From.RemoveAt(Index);
		}
		return true;
	}

	bool CanCapture()
	{
		return FApp::CanEverRender() && IsInGameThread();
	}

	FImage PackORM(const FImage* Occlusion, const FImage* Captured, const int32 Size, const float Roughness, const float Metallic)
	{
		FImage Out;
		Out.Map = EMap::ORM;
		Out.Size = Size;
		Out.Pixels.SetNumUninitialized(Size * Size);
		const uint8 Rough = uint8(FMath::Clamp(FMath::RoundToInt32(Roughness * 255.f), 0, 255));
		const uint8 Metal = uint8(FMath::Clamp(FMath::RoundToInt32(Metallic * 255.f), 0, 255));
		const bool bOcclusion = Occlusion && Occlusion->Pixels.Num() == Size * Size;
		const bool bCaptured = Captured && Captured->Pixels.Num() == Size * Size;
		for (int32 Index = 0; Index < Size * Size; ++Index)
		{
			// A capture packs metal in red, roughness in green.
			Out.Pixels[Index] = FColor(bOcclusion ? Occlusion->Pixels[Index].R : uint8(255), bCaptured ? Captured->Pixels[Index].G : Rough, bCaptured ? Captured->Pixels[Index].R : Metal, 255);
		}
		return Out;
	}

	void Stats(const FImage& Image, float& OutMin, float& OutMean, float& OutMax)
	{
		OutMin = 1.f;
		OutMax = 0.f;
		double Sum = 0.0;
		for (const FColor& Pixel : Image.Pixels)
		{
			const float Value = (0.299f * Pixel.R + 0.587f * Pixel.G + 0.114f * Pixel.B) / 255.f;
			OutMin = FMath::Min(OutMin, Value);
			OutMax = FMath::Max(OutMax, Value);
			Sum += Value;
		}
		OutMean = Image.Pixels.IsEmpty() ? 0.f : float(Sum / Image.Pixels.Num());
		if (Image.Pixels.IsEmpty()) OutMin = 0.f;
	}

	UTexture2D* BaseColorTexture(const UMaterialInterface* Material)
	{
		if (!Material) return nullptr;
		if (UTexture2D* Named = TextureByName(Material, {TEXT("basecolor"), TEXT("albedo"), TEXT("diffuse"), TEXT("color"), TEXT("colour")}, {TEXT("normal"), TEXT("emissive")}))
		{
			if (!IsDefaultTexture(Named)) return Named;
		}
		TArray<UTexture*> Used;
		Material->GetUsedTextures(Used);
		for (UTexture* Texture : Used)
		{
			UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
			if (Texture2D && !IsDefaultTexture(Texture2D) && Texture2D->SRGB && Texture2D->CompressionSettings != TC_Normalmap) return Texture2D;
		}
		return nullptr;
	}

	UTexture2D* NormalTexture(const UMaterialInterface* Material)
	{
		if (!Material) return nullptr;
		if (UTexture2D* Named = TextureByName(Material, {TEXT("normal")}, {}))
		{
			if (!IsDefaultTexture(Named)) return Named;
		}
		TArray<UTexture*> Used;
		Material->GetUsedTextures(Used);
		for (UTexture* Texture : Used)
		{
			UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
			if (Texture2D && !IsDefaultTexture(Texture2D) && Texture2D->CompressionSettings == TC_Normalmap) return Texture2D;
		}
		return nullptr;
	}

	FString WhyNotTexturePath(const FString& Path, const bool bReplace, bool& bOutExists)
	{
		bOutExists = false;
		const FString Package = PackageOf(Path);
		FString Reason;
		if (!FHyperAIStudioNames::IsObjectPath(Package, TEXT("Texture"), &Reason)) return Reason;
		const FString Folder = FPackageName::GetLongPackagePath(Package);
		const FString Refusal = UV::WhyNotWritableFolder(Folder);
		if (!Refusal.IsEmpty()) return Refusal;
		UObject* Other = nullptr;
		UTexture2D* Existing = FindOrLoad<UTexture2D>(Package, Other);
		if (Other) return FString::Printf(TEXT("%s is already something other than a texture."), *Package);
		bOutExists = Existing != nullptr;
		if (Existing && !bReplace) return FString::Printf(TEXT("%s already exists; send bReplace true to overwrite it (undo takes it back), or pick another Folder."), *Package);
		if (Existing && Existing->GetPackage()->HasAnyPackageFlags(PKG_Cooked)) return FString::Printf(TEXT("%s is cooked and cannot be overwritten."), *Package);
		return FString();
	}

	UTexture2D* WriteTexture(const FString& Path, const FImage& Image, const bool bReplace, bool& bOutCreated, FString& OutError)
	{
		bOutCreated = false;
		const FString Package = PackageOf(Path);
		const FTextureKind Kind = KindOf(Image.Map);
		auto Configure = [&Kind](UTexture2D& Texture)
		{
			Texture.CompressionSettings = Kind.Compression;
			Texture.SRGB = Kind.bSRGB;
			Texture.LODGroup = Kind.Group;
		};
		if (Image.Size <= 0 || Image.Pixels.Num() != Image.Size * Image.Size)
		{
			OutError = FString::Printf(TEXT("the %s map has no pixels"), MapName(Image.Map));
			return nullptr;
		}
		UObject* Other = nullptr;
		if (UTexture2D* Existing = FindOrLoad<UTexture2D>(Package, Other))
		{
			if (!bReplace)
			{
				OutError = FString::Printf(TEXT("%s already exists; send bReplace true to overwrite it."), *Package);
				return nullptr;
			}
			if (!FHyperAIStudioSourceControl::MakeEditable({Package}, OutError)) return nullptr;
			// In the caller's undo step: the texture's source pixels go into the transaction with it.
			Existing->Modify();
			Existing->PreEditChange(nullptr);
			Existing->Source.Init(Image.Size, Image.Size, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Image.Pixels.GetData()));
			Configure(*Existing);
			Existing->PostEditChange();
			Existing->MarkPackageDirty();
			return Existing;
		}
		if (Other)
		{
			OutError = FString::Printf(TEXT("%s is already something other than a texture."), *Package);
			return nullptr;
		}
		// Made outside the undo step, as Epic's asset creation does: undo cannot delete an asset, and would only blank it.
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		UPackage* Outer = CreatePackage(*Package);
		UTexture2D* Texture = NewObject<UTexture2D>(Outer, *FPackageName::GetShortName(Package), RF_Public | RF_Standalone);
		Texture->Source.Init(Image.Size, Image.Size, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Image.Pixels.GetData()));
		Configure(*Texture);
		Texture->PostEditChange();
		FAssetRegistryModule::AssetCreated(Texture);
		Texture->MarkPackageDirty();
		Texture->SetFlags(RF_Transactional);
		bOutCreated = true;
		return Texture;
	}

	UMaterialInterface* SurfaceMaterial(const bool bCreate, FString& OutError)
	{
		UObject* Other = nullptr;
		if (UMaterialInterface* Found = FindOrLoad<UMaterialInterface>(SurfaceMaterialPath, Other)) return Found;
		if (!bCreate) return nullptr;
		if (Other)
		{
			OutError = FString::Printf(TEXT("%s exists but is not a material; move it, and the baked-surface material is made again."), SurfaceMaterialPath);
			return nullptr;
		}
		UTexture* White = LoadObject<UTexture>(nullptr, WhiteTexture, nullptr, LOAD_NoWarn | LOAD_Quiet);
		UTexture* Flat = LoadObject<UTexture>(nullptr, FlatNormalTexture, nullptr, LOAD_NoWarn | LOAD_Quiet);
		// Made outside any plan's undo step: undo cannot delete an asset, and would only blank its graph.
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		UPackage* Package = CreatePackage(SurfaceMaterialPath);
		UMaterial* Material = NewObject<UMaterial>(Package, *FPackageName::GetShortName(SurfaceMaterialPath), RF_Public | RF_Standalone);
		auto Texture = [Material](const TCHAR* Name, UTexture* Default, const EMaterialSamplerType Sampler)
		{
			UMaterialExpressionTextureSampleParameter2D* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
			Sample->ParameterName = Name;
			Sample->Texture = Default;
			Sample->SamplerType = Sampler;
			return Sample;
		};
		auto Scalar = [Material](const TCHAR* Name, const float Default)
		{
			UMaterialExpressionScalarParameter* Value = NewObject<UMaterialExpressionScalarParameter>(Material);
			Value->ParameterName = Name;
			Value->DefaultValue = Default;
			return Value;
		};
		// BaseColor x BaseColorTint; Normal; ORM: red occlusion, green roughness x Roughness, blue metal x Metallic.
		UMaterialExpressionTextureSampleParameter2D* Color = Texture(TEXT("BaseColor"), White, SAMPLERTYPE_Color);
		UMaterialExpressionTextureSampleParameter2D* Normal = Texture(TEXT("Normal"), Flat, SAMPLERTYPE_Normal);
		UMaterialExpressionTextureSampleParameter2D* Packed = Texture(TEXT("ORM"), White, SAMPLERTYPE_Masks);
		UMaterialExpressionVectorParameter* Tint = NewObject<UMaterialExpressionVectorParameter>(Material);
		Tint->ParameterName = TEXT("BaseColorTint");
		Tint->DefaultValue = FLinearColor::White;
		UMaterialExpressionScalarParameter* Roughness = Scalar(TEXT("Roughness"), 0.8f);
		UMaterialExpressionScalarParameter* Metallic = Scalar(TEXT("Metallic"), 0.f);
		UMaterialExpressionMultiply* Tinted = NewObject<UMaterialExpressionMultiply>(Material);
		Tinted->A.Connect(0, Color);
		Tinted->B.Connect(0, Tint);
		UMaterialExpressionMultiply* Rough = NewObject<UMaterialExpressionMultiply>(Material);
		Rough->A.Connect(2, Packed);
		Rough->B.Connect(0, Roughness);
		UMaterialExpressionMultiply* Metal = NewObject<UMaterialExpressionMultiply>(Material);
		Metal->A.Connect(3, Packed);
		Metal->B.Connect(0, Metallic);
		int32 Row = 0;
		for (UMaterialExpression* Expression : TArray<UMaterialExpression*>{Color, Tint, Tinted, Normal, Packed, Roughness, Rough, Metallic, Metal})
		{
			Expression->MaterialExpressionEditorX = -600 + (Row % 3) * 220;
			Expression->MaterialExpressionEditorY = (Row / 3) * 260;
			++Row;
			Material->GetExpressionCollection().AddExpression(Expression);
		}
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Tinted);
		Material->GetEditorOnlyData()->Normal.Connect(0, Normal);
		Material->GetEditorOnlyData()->AmbientOcclusion.Connect(1, Packed);
		Material->GetEditorOnlyData()->Roughness.Connect(0, Rough);
		Material->GetEditorOnlyData()->Metallic.Connect(0, Metal);
		Material->PostEditChange();
		FAssetRegistryModule::AssetCreated(Material);
		Material->MarkPackageDirty();
		if (!FHyperAIStudioSourceControl::SaveAsset(*Material, OutError)) return nullptr;
		Material->SetFlags(RF_Transactional);
		return Material;
	}

	FString WhyNotMaterialPath(const FString& Path, bool& bOutExists)
	{
		bOutExists = false;
		const FString Package = PackageOf(Path);
		FString Reason;
		if (!FHyperAIStudioNames::IsObjectPath(Package, TEXT("Material"), &Reason)) return Reason;
		const FString Refusal = UV::WhyNotWritableFolder(FPackageName::GetLongPackagePath(Package));
		if (!Refusal.IsEmpty()) return Refusal;
		UObject* Other = nullptr;
		UMaterialInstanceConstant* Existing = FindOrLoad<UMaterialInstanceConstant>(Package, Other);
		if (Other) return FString::Printf(TEXT("%s is already something other than a material instance constant."), *Package);
		bOutExists = Existing != nullptr;
		return FString();
	}

	UMaterialInstanceConstant* WriteMaterial(const FString& Path, UTexture2D* BaseColor, UTexture2D* Normal, UTexture2D* ORM, bool& bOutCreated, TArray<FString>& OutNotes,
		FString& OutError)
	{
		bOutCreated = false;
		const FString Package = PackageOf(Path);
		auto Fill = [&](UMaterialInstanceConstant& Instance)
		{
			const TPair<const TCHAR*, UTexture2D*> Textures[] = {{TEXT("BaseColor"), BaseColor}, {TEXT("Normal"), Normal}, {TEXT("ORM"), ORM}};
			for (const TPair<const TCHAR*, UTexture2D*>& Entry : Textures)
			{
				if (!Entry.Value) continue;
				UTexture* Current = nullptr;
				if (!Instance.GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(FName(Entry.Key))), Current))
				{
					OutNotes.Add(FString::Printf(TEXT("%s's parent has no texture parameter %s, so %s does not show through it."), *Instance.GetPathName(), Entry.Key,
						*Entry.Value->GetPathName()));
				}
				Instance.SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Entry.Key), Entry.Value);
			}
			// The ORM texture holds roughness and metal: the scales stay at 1 so it shows as baked.
			if (ORM)
			{
				Instance.SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), 1.f);
				Instance.SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Metallic")), 1.f);
			}
		};
		UObject* Other = nullptr;
		if (UMaterialInstanceConstant* Existing = FindOrLoad<UMaterialInstanceConstant>(Package, Other))
		{
			if (!FHyperAIStudioSourceControl::MakeEditable({Package}, OutError)) return nullptr;
			Existing->Modify();
			Fill(*Existing);
			Existing->PostEditChange();
			Existing->MarkPackageDirty();
			return Existing;
		}
		if (Other)
		{
			OutError = FString::Printf(TEXT("%s is already something other than a material instance constant."), *Package);
			return nullptr;
		}
		UMaterialInterface* Parent = SurfaceMaterial(/*bCreate=*/true, OutError);
		if (!Parent) return nullptr;
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		UPackage* Outer = CreatePackage(*Package);
		UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Outer, *FPackageName::GetShortName(Package), RF_Public | RF_Standalone);
		Instance->SetParentEditorOnly(Parent);
		Fill(*Instance);
		Instance->PostEditChange();
		FAssetRegistryModule::AssetCreated(Instance);
		Instance->MarkPackageDirty();
		Instance->SetFlags(RF_Transactional);
		bOutCreated = true;
		return Instance;
	}

	bool DrawSheet(const TArray<FImage>& Images, const FString& Path, FString& OutError)
	{
		if (Images.IsEmpty())
		{
			OutError = TEXT("no maps to draw");
			return false;
		}
		constexpr int32 Tile = 256;
		constexpr int32 Gap = 4;
		const int32 Columns = FMath::Min(Images.Num(), 4);
		const int32 Rows = (Images.Num() + Columns - 1) / Columns;
		const int32 Width = Columns * Tile + (Columns + 1) * Gap;
		const int32 Height = Rows * Tile + (Rows + 1) * Gap;
		TArray<FColor> Pixels;
		Pixels.Init(FColor(18, 18, 20), Width * Height);
		for (int32 Index = 0; Index < Images.Num(); ++Index)
		{
			const FImage& Image = Images[Index];
			if (Image.Size <= 0) continue;
			const int32 Left = Gap + (Index % Columns) * (Tile + Gap);
			const int32 Top = Gap + (Index / Columns) * (Tile + Gap);
			for (int32 Y = 0; Y < Tile; ++Y)
			{
				for (int32 X = 0; X < Tile; ++X)
				{
					const int32 SX = FMath::Min(Image.Size - 1, X * Image.Size / Tile);
					const int32 SY = FMath::Min(Image.Size - 1, Y * Image.Size / Tile);
					FColor Pixel = Image.Pixels[SY * Image.Size + SX];
					Pixel.A = 255;
					Pixels[(Top + Y) * Width + Left + X] = Pixel;
				}
			}
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		if (Png.IsEmpty() || !FFileHelper::SaveArrayToFile(Png, *Path))
		{
			OutError = TEXT("the maps' PNG could not be written");
			return false;
		}
		return true;
	}

	FString SheetPathOf(const FString& AssetPath, const bool bDryRun)
	{
		const FString Package = PackageOf(AssetPath);
		return FHyperAIStudioResultPreview::GetAssetPreviewPath(Package + TEXT(".") + FPackageName::GetShortName(Package) + (bDryRun ? TEXT(" bake dry run") : TEXT(" bake")));
	}

	bool ReadForBake(UStaticMesh& Mesh, const int32 Lod, UDynamicMesh& Out, FString& OutError)
	{
		const TStrongObjectPtr<UGeometryScriptDebug> Debug(NewObject<UGeometryScriptDebug>(GetTransientPackage()));
		FGeometryScriptCopyMeshFromAssetOptions Options;
		Options.bApplyBuildSettings = true;
		Options.bRequestTangents = true;
		Options.bUseBuildScale = true;
		FGeometryScriptMeshReadLOD Read;
		Read.LODIndex = FMath::Max(0, Lod);
		Read.LODType = Lod < 0 ? EGeometryScriptLODType::HiResSourceModel : UV::SourceOf(Mesh, Lod) ? EGeometryScriptLODType::SourceModel : EGeometryScriptLODType::RenderData;
		EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
		UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(&Mesh, &Out, Options, Read, Outcome, /*bUseSectionMaterials=*/false, Debug.Get());
		if (Outcome != EGeometryScriptOutcomePins::Success || Out.GetTriangleCount() == 0)
		{
			OutError = FString::Printf(TEXT("%s could not be read%s"), *Mesh.GetPathName(), Debug->Messages.IsEmpty() ? TEXT("") : *(TEXT(": ") + Debug->Messages[0].Message.ToString()));
			return false;
		}
		// Tangent-space maps need tangents; a mesh saved without any gets MikkTSpace ones from UV channel 0, as the build makes them.
		bool bTangents = false;
		Out.ProcessMesh([&bTangents](const FDynamicMesh3& Mesh3) { bTangents = Mesh3.HasAttributes() && Mesh3.Attributes()->HasTangentSpace(); });
		if (!bTangents)
		{
			FGeometryScriptTangentsOptions Tangents;
			Tangents.UVLayer = 0;
			UGeometryScriptLibrary_MeshNormalsFunctions::ComputeTangents(&Out, Tangents, Debug.Get());
		}
		return true;
	}
}
