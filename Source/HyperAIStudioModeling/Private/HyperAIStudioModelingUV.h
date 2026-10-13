// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioModelingSurfaceTypes.h"

struct FMeshDescription;
class UDynamicMesh;
class UStaticMesh;

namespace UE::Geometry { class FDynamicMesh3; }

/**
 * UV layouts measured, drawn and made: the UV check, AutoUV, and reading and writing a static mesh's source LODs
 * without touching anything but their UVs.
 */
namespace HyperAIStudio::Modeling::UV
{
	/** Triangles as the check reads them: three corners each, position in cm and UV. */
	struct FSoup
	{
		TArray<FVector3d> Positions;
		TArray<FVector2d> UVs;
		/** Per triangle: it has UVs in the channel. */
		TArray<bool> HasUV;

		int32 Num() const { return HasUV.Num(); }
		void Add(const FVector3d& A, const FVector3d& B, const FVector3d& C, const FVector2d& UA, const FVector2d& UB, const FVector2d& UC, bool bHasUV = true);
	};

	struct FIslandPair
	{
		int32 A = 0;
		int32 B = 0;
		int64 Pixels = 0;
	};

	/** What Analyze found; Islands numbers islands from 1, as the PNG draws them. */
	struct FMetrics
	{
		int32 Triangles = 0;
		int32 Islands = 0;
		double SeamLength = 0.0;
		double OverlapPercent = 0.0;
		/** Overlapping island pairs, most pixels first; A == B is an island folded over itself. */
		TArray<FIslandPair> Overlaps;
		int32 Flipped = 0;
		int32 Degenerate = 0;
		double OutsidePercent = 0.0;
		FBox2d Bounds = FBox2d(ForceInit);
		double CoveragePercent = 0.0;
		double UVArea = 0.0;
		double DensityMin = 0.0;
		double DensityMedian = 0.0;
		double DensityMax = 0.0;
		double DensitySpread = 0.0;
		/** Per triangle: its island (from 1; 0 for one left out), and what is wrong with it. */
		TArray<int32> IslandOf;
		enum EFlag : uint8 { FlagNone = 0, FlagFlipped = 1, FlagDegenerate = 2, FlagOverlapping = 4, FlagOutside = 8, FlagSkipped = 16 };
		TArray<uint8> Flags;
		/** Per triangle, bit e: edge e (corner e to e+1) has no neighbour whose UVs agree, so it outlines its island. */
		TArray<uint8> Border;
	};

	struct FOptions
	{
		/** Texture size texel density is counted at. */
		int32 TextureSize = 1024;
		/** Pixels along the longest side of the UV bounds when overlaps are counted. */
		int32 RasterSize = 1024;
	};

	FMetrics Analyze(const FSoup& Soup, const FOptions& Options = FOptions());

	struct FLightmapMetrics
	{
		int32 Resolution = 0;
		double SharedPercent = 0.0;
		TArray<FIslandPair> TooClose;
		double OutsidePercent = 0.0;
		bool bSuitable = false;
	};
	/** The layout counted in texels at Resolution over 0-1: texels islands share, and islands closer than Padding empty texels. */
	FLightmapMetrics AnalyzeLightmap(const FSoup& Soup, const FMetrics& Metrics, int32 Resolution, int32 Padding);

	/** good, usable, bad or missing, and the problems behind it, errors first. */
	void Judge(const FMetrics& Metrics, FString& OutVerdict, TArray<FString>& OutProblems);
	/** How bad a verdict is: missing 3, bad 2, usable 1, good 0. */
	int32 VerdictRank(const FString& Verdict);
	void ToStruct(const FMetrics& Metrics, int32 Channel, FHyperAIModelUVMetrics& Out);
	void ToStruct(const FLightmapMetrics& Metrics, int32 Channel, bool bPresent, bool bGenerated, FHyperAIModelLightmapCheck& Out);
	/** "bad: 412 islands, 23.1% overlap, 4 flipped" */
	FString Describe(const FString& Verdict, const FMetrics& Metrics);

	/** Draws the 0-1 square and the layout, problems coloured, as a Size-pixel PNG at Path. */
	bool DrawLayout(const FSoup& Soup, const FMetrics& Metrics, const FString& Path, FString& OutError, int32 Size = 512);
	/** Where an asset's layout PNG goes: the same file every time, so a reply can name it before it is drawn. */
	FString LayoutPathOf(const FString& AssetPath, int32 Lod, int32 Channel, bool bDryRun);

	/** False when the channel is not there. Positions are scaled by Scale (the LOD's build scale). */
	bool SoupFromDescription(const FMeshDescription& Mesh, int32 Channel, const FVector& Scale, FSoup& Out);
	bool SoupFromDynamic(const UE::Geometry::FDynamicMesh3& Mesh, int32 Layer, FSoup& Out);
	/** The built mesh, which holds channels made at build time (generated lightmap UVs). */
	bool SoupFromRenderData(UStaticMesh& Mesh, int32 Lod, int32 Channel, FSoup& Out, FString& OutError);

	/** A source LOD's mesh description: -1 is the hi-res source. Null when that LOD has none of its own. */
	const FMeshDescription* SourceOf(const UStaticMesh& Mesh, int32 Lod);
	/** UV channels a source LOD has; 0 when it has no mesh description. */
	int32 ChannelsOf(const UStaticMesh& Mesh, int32 Lod);
	FVector BuildScaleOf(const UStaticMesh& Mesh, int32 Lod);

	/** A source LOD as a dynamic mesh: material ids are polygon group ids, so Write puts every triangle back in its section. */
	bool Read(const UStaticMesh& Mesh, int32 Lod, UE::Geometry::FDynamicMesh3& Out, FString& OutError);
	/**
	 * Writes the dynamic mesh back as the LOD's source, inside the caller's transaction (its mesh description is
	 * recorded first): positions, normals, tangents, colours and polygroups come through as read, and the polygon groups
	 * keep their ids and material slot names, so every section keeps its material. The caller rebuilds the mesh.
	 */
	bool Write(UStaticMesh& Mesh, int32 Lod, const UE::Geometry::FDynamicMesh3& Edited, FString& OutError);

	/** The methods hyper_model_uv_apply takes. */
	enum class EMethod : uint8 { XAtlas, Patch, Box, Planar, Cylinder, Repack, Lightmap };
	bool ParseMethod(const FString& Text, EMethod& Out);
	const TCHAR* MethodName(EMethod Method);

	struct FUnwrap
	{
		EMethod Method = EMethod::XAtlas;
		int32 Channel = 0;
		int32 Resolution = 1024;
		int32 Padding = 4;
		bool bRepack = true;
		double TileSize = 100.0;
	};
	/** Lays out Channel again (adding it when it is one past the last); every other channel stays. Not for lightmap. */
	bool Unwrap(UDynamicMesh& Mesh, const FUnwrap& Settings, FString& OutError);
	/** Packs the channel's islands into 0-1, Padding texels apart at Resolution (Unreal's packer, at Resolution / Padding). */
	bool Pack(UE::Geometry::FDynamicMesh3& Mesh, int32 Channel, int32 Resolution, int32 Padding, FString& OutError);

	/** The lightmap channel Unreal's build would make from SourceChannel into Channel at Resolution, laid out in a copy of Mesh. */
	bool PreviewLightmap(const UStaticMesh& Asset, const FMeshDescription& Mesh, int32 SourceChannel, int32 Channel, int32 Resolution, FSoup& Out, FString& OutError);

	/**
	 * Why the agent may not change this mesh, or empty: Engine or plugin content, cooked, without a source mesh, or
	 * read-only on disk with no source control to check it out.
	 */
	FString WhyNotWritable(const UStaticMesh& Mesh);
	/** Where a texture or material made for the mesh may go: a /Game or project-plugin folder; empty when it may. */
	FString WhyNotWritableFolder(const FString& Folder);
}
