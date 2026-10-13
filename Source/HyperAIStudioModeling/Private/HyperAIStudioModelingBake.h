// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureDefines.h"

class UDynamicMesh;
class UMaterialInstanceConstant;
class UMaterialInterface;
class UStaticMesh;
class UTexture2D;

/** Texture bakes with Geometry Script (UGeometryScriptLibrary_MeshBakeFunctions::BakeTexture), and the assets they become. */
namespace HyperAIStudio::Modeling::Bake
{
	/**
	 * What a bake makes. Material bakes with render captures into three images: base colour (Texture), Normal and
	 * MaterialMRS (metallic, roughness, specular), which goes into the ORM pack and is never written alone.
	 */
	enum class EMap : uint8 { Normal, ObjectNormal, BentNormal, AO, Curvature, Position, Height, VertexColor, Texture, MultiTexture, Material, ORM, MaterialMRS };
	bool ParseMap(const FString& Text, EMap& Out);
	const TCHAR* MapName(EMap Map);
	/** The texture name's suffix: N, ON, BN, AO, CV, P, H, VC, BC, ORM. */
	const TCHAR* MapSuffix(EMap Map);
	/** Every map name, for refusals. */
	FString MapNames();

	/** How a map's texture is set up. */
	struct FTextureKind
	{
		TextureCompressionSettings Compression = TC_Default;
		bool bSRGB = false;
		TextureGroup Group = TEXTUREGROUP_World;
		const TCHAR* CompressionName = TEXT("TC_Default");
	};
	/** Normal maps TC_Normalmap; occlusion, curvature and ORM TC_Masks; height TC_Grayscale; position and object normals TC_VectorDisplacementmap (kept exact); all of those linear. Colour maps sRGB. */
	FTextureKind KindOf(EMap Map);

	struct FJob
	{
		UDynamicMesh* Target = nullptr;
		int32 TargetChannel = 0;
		/** The same object as Target bakes the target from itself. */
		UDynamicMesh* Source = nullptr;
		TArray<EMap> Maps;
		int32 Resolution = 1024;
		int32 Samples = 1;
		int32 Padding = 4;
		int32 Rays = 16;
		float OcclusionDistance = 0.f;
		float ProjectionDistance = 3.f;
		UTexture2D* SourceTexture = nullptr;
		/** multi_texture: one per material slot of the source; null slots are left out. */
		TArray<UTexture2D*> SlotTextures;
		int32 SourceChannel = 0;
		/** normal and object_normal also carry this tangent-space normal map of the source across. */
		UTexture2D* SourceNormalMap = nullptr;
		int32 SourceNormalChannel = 0;
		/** material: the source static mesh, drawn alone in a preview world of its own. */
		UStaticMesh* SourceAsset = nullptr;
	};

	/** One baked map as the texture will hold it: BGRA8 rows, sRGB-encoded for colour maps. */
	struct FImage
	{
		EMap Map = EMap::Normal;
		int32 Size = 0;
		TArray<FColor> Pixels;
	};

	/**
	 * Bakes the maps, in Job.Maps order (material gives its three images in its place); false with the reason, Geometry
	 * Script's own message when it gives one.
	 */
	bool Run(const FJob& Job, TArray<FImage>& OutImages, FString& OutError);
	/** Whether material bakes can run here: they draw the source, which an editor without rendering cannot. */
	bool CanCapture();
	/** Red from the occlusion map (white without one); green and blue the captured roughness and metal, or Roughness and Metallic. */
	FImage PackORM(const FImage* Occlusion, const FImage* Captured, int32 Size, float Roughness, float Metallic);
	/** Brightness over the image, 0-1. */
	void Stats(const FImage& Image, float& OutMin, float& OutMean, float& OutMax);

	/** The texture a material samples for its base colour (by parameter name, else the first colour texture it uses); null when none. */
	UTexture2D* BaseColorTexture(const UMaterialInterface* Material);
	/** Its tangent-space normal map, by parameter name or compression; null when none. */
	UTexture2D* NormalTexture(const UMaterialInterface* Material);

	/** Pixels per side Geometry Script bakes at: a power of two from 16 to 8192. */
	bool IsResolution(int32 Pixels);
	bool IsSampleCount(int32 Samples);

	/**
	 * Writes Image as a texture at Path (/Game/Models/T_Rock_N). A new texture is made outside any undo step, as Epic's
	 * asset creation does; with bReplace one already there is overwritten inside the caller's transaction.
	 */
	UTexture2D* WriteTexture(const FString& Path, const FImage& Image, bool bReplace, bool& bOutCreated, FString& OutError);
	/** What WriteTexture would refuse at Path, or empty. */
	FString WhyNotTexturePath(const FString& Path, bool bReplace, bool& bOutExists);

	/** HyperAI's baked-surface material (BaseColor, Normal and ORM texture parameters, BaseColorTint, Roughness and Metallic scales), made and saved on first use. */
	UMaterialInterface* SurfaceMaterial(bool bCreate, FString& OutError);
	/**
	 * Makes a material instance of the surface material at Path, or updates the material instance constant there (inside
	 * the caller's transaction), with whichever textures are given; Notes say when its parent lacks a parameter.
	 */
	UMaterialInstanceConstant* WriteMaterial(const FString& Path, UTexture2D* BaseColor, UTexture2D* Normal, UTexture2D* ORM, bool& bOutCreated, TArray<FString>& OutNotes,
		FString& OutError);
	FString WhyNotMaterialPath(const FString& Path, bool& bOutExists);

	/** The maps side by side as one PNG at Path, in order. */
	bool DrawSheet(const TArray<FImage>& Images, const FString& Path, FString& OutError);
	FString SheetPathOf(const FString& AssetPath, bool bDryRun);

	/**
	 * A static mesh as the renderer sees it, for a bake: build settings applied, tangents made, material ids as slot
	 * numbers. Lod -1 reads the hi-res source; a LOD reduced from another is read from the built mesh.
	 */
	bool ReadForBake(UStaticMesh& Mesh, int32 Lod, UDynamicMesh& Out, FString& OutError);
}
