// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "GeometryCollection/ManagedArrayCollection.h"
#include "HyperAIStudioDestructionToolset.h"

class AActor;
class UGeometryCollection;
class UMaterialInterface;
class UPhysicalMaterial;
class UStaticMesh;

/**
 * The destruction plan's checks, the pieces it builds in memory, and what it writes. The pieces are built the way the
 * Dataflow fracture nodes build them: a managed-array collection from FGeometryCollectionEngineConversion, cut by the
 * Fracture plugin's FFractureEngineFracturing (or PlanarCut's CutMultipleWithPlanarCells under it), clustered by
 * FFractureEngineClustering, and written with UGeometryCollection::ResetFrom. Game thread only.
 */
namespace HyperAIStudio::Destruction::Gate
{
	enum class EParamType : uint8
	{
		Int,
		Float,
		Bool,
		/** One word from Choices. */
		Choice,
		/** "x,y,z". */
		Vector,
		/** Free text the op reads itself, such as a list of planes or thresholds. */
		List
	};

	struct FParamSpec
	{
		const TCHAR* Name = TEXT("");
		EParamType Type = EParamType::Float;
		double Min = 0.0;
		double Max = 0.0;
		/** Above Max and up to this, a number is fitted down to Max with a note; past it, refused. 0: Max is hard. */
		double FitMax = 0.0;
		/** The default as text; empty when the param is required or has no default. */
		const TCHAR* Default = TEXT("");
		/** Choice: the words, comma separated. */
		const TCHAR* Choices = TEXT("");
		const TCHAR* Help = TEXT("");
		bool bRequired = false;
	};

	struct FKindSpec
	{
		const TCHAR* Kind = TEXT("");
		const TCHAR* Summary = TEXT("");
		TArray<FParamSpec> Params;
	};

	/** Every op kind, in the order the docs list them. */
	const TArray<FKindSpec>& Kinds();
	const FKindSpec* FindKind(const FString& Kind);
	/** "Name: type, range, default - help" for each param of Spec. */
	TArray<FString> DescribeParams(const FKindSpec& Spec);

	/** A kind's Params, checked against its table, with every default filled in. */
	struct FParams
	{
		TMap<FString, FString> Values;

		bool Has(const TCHAR* Name) const;
		FString Text(const TCHAR* Name) const;
		double Number(const TCHAR* Name) const;
		int32 Int(const TCHAR* Name) const;
		bool Bool(const TCHAR* Name) const;
		FVector Vector(const TCHAR* Name) const;
	};

	/**
	 * Params "Name=Value; Name=Value" against Spec: unknown names, values of the wrong type or out of range are refused
	 * with what the param takes. A number past Max but within FitMax is fitted to Max and noted in OutFitted ("Sites 600
	 * -> 500"); one past FitMax is refused as absurd.
	 */
	bool ParseParams(const FKindSpec& Spec, const FString& Text, FParams& Out, TArray<FString>& OutFitted, FString& OutError);

	bool ParseVector(const FString& Text, FVector& Out);
	/** "500000, 50000, 5000": a threshold per cluster level, the root's first; each 0 or more. */
	bool ParseThresholds(const FString& Text, TArray<float>& Out, FString& OutError);
	/** "px,py,pz,nx,ny,nz | ...": planes through a point, by their normal. */
	bool ParsePlanes(const FString& Text, TArray<FTransform>& Out, FString& OutError);
	/** "30:sphere, 100:box": pieces up to 30 cm use spheres, up to 100 boxes. */
	bool ParseSizes(const FString& Text, TArray<TPair<float, FString>>& Out, FString& OutError);

	/** Count sites spread evenly through Bounds, the same for the same Seed. */
	TArray<FVector> UniformSites(const FBox& Bounds, int32 Count, int32 Seed);
	/** Clusters of PerCluster sites each, around centres spread through Bounds, within RadiusFraction of Bounds' size. */
	TArray<FVector> ClusteredSites(const FBox& Bounds, int32 Clusters, int32 PerCluster, double RadiusFraction, int32 Seed);
	/**
	 * Rings x AngularSteps sites in rings about Center in the plane of Normal, the outermost Radius out, each varied by up
	 * to Variability cm: Epic's radial scatter (FRadialScatterPointsDataflowNode_v2) with an even ring spacing.
	 */
	TArray<FVector> RadialSites(const FVector& Center, const FVector& Normal, double Radius, int32 Rings, int32 AngularSteps, double AngleOffsetDegrees,
		double Variability, int32 Seed);

	/** One mesh a make op takes in: a static mesh asset, or a static mesh component of a level actor. */
	struct FSource
	{
		TObjectPtr<UStaticMesh> Mesh = nullptr;
		TArray<TObjectPtr<UMaterialInterface>> Materials;
		/** Relative to the geometry collection's pivot. */
		FTransform Transform = FTransform::Identity;
		FString Name;
	};

	/** An op, checked. */
	struct FStep
	{
		int32 Index = 0;
		FString Kind;
		FHyperAIDestructionOp Op;
		FParams Params;
		FVector Location = FVector::ZeroVector;
		TObjectPtr<UMaterialInterface> Material = nullptr;
		TObjectPtr<UPhysicalMaterial> PhysicalMaterial = nullptr;
		TArray<TObjectPtr<UStaticMesh>> Proxies;

		FString Describe() const;
	};

	struct FPlan
	{
		/** Package path, /Game/... */
		FString AssetPath;
		/** The plan makes the asset (a make op); otherwise it changes Existing. */
		bool bMake = false;
		TObjectPtr<UGeometryCollection> Existing = nullptr;
		TArray<FSource> Sources;
		/** Level actors the sources came from. */
		TArray<TWeakObjectPtr<AActor>> SourceActors;
		/** replace, beside, at or none. */
		FString Place = TEXT("none");
		/** Where a placed actor goes. */
		FTransform PlaceTransform = FTransform::Identity;
		TArray<FStep> Steps;
		/** Params fitted into range: "Operation 1 (fracture.uniform): Sites 600 -> 500". */
		TArray<FString> Fitted;
		int32 SourceTriangles = 0;
	};

	/** The geometry collection an undone plan made at PackagePath, kept as garbage for redo; null when there is none. */
	UGeometryCollection* UndoneAt(const FString& PackagePath);

	/** The request, checked without building anything: names, paths, params, sources. */
	bool Check(const FHyperAIDestructionPlanRequest& Request, FPlan& OutPlan, FString& OutError);

	/** Settings the plan writes onto the asset and the actors it places; unset ones stay as they are. */
	struct FSettings
	{
		TOptional<TArray<float>> Thresholds;
		TOptional<bool> bSizeSpecificDamage;
		TOptional<bool> bDamageFromCollision;
		TOptional<FString> Implicit;
		TOptional<FString> CollisionType;
		TOptional<float> CollisionMargin;
		TArray<TPair<float, FString>> Sizes;
		bool bSizesSet = false;
		TOptional<float> Density;
		TOptional<float> MassKg;
		TObjectPtr<UPhysicalMaterial> PhysicalMaterial = nullptr;
		/** dynamic, kinematic, static or sleeping. */
		TOptional<FString> State;
		TOptional<bool> bNanite;
		/** default or root_proxy. */
		TOptional<FString> Renderer;
		TArray<TObjectPtr<UStaticMesh>> Proxies;
	};

	/** The pieces as the plan leaves them, built in memory. */
	struct FBuilt
	{
		FManagedArrayCollection Collection;
		TArray<TObjectPtr<UMaterialInterface>> Materials;
		FSettings Settings;
		TArray<FString> Notes;
		int32 BuildMs = 0;
	};

	/** Builds the pieces: the sources (or the existing asset's pieces), then every op in order. */
	bool Build(const FPlan& Plan, FBuilt& OutBuilt, FString& OutError);

	/** What a collection holds. Thresholds are the asset's (or the plan's), per cluster level. */
	struct FMeasure
	{
		int32 Bones = 0;
		int32 Geometry = 0;
		int32 Leaves = 0;
		int32 Clusters = 0;
		int32 Triangles = 0;
		int32 Anchored = 0;
		TArray<FHyperAIDestructionLevel> Levels;
		FBox Bounds = FBox(ForceInit);
		double VolumeCm3 = 0.0;
		float SmallestLeafCm = 0.f;
		int32 LeavesUnder5Cm = 0;
		FString RootState;
		TArray<FHyperAIDestructionPiece> Pieces;
	};

	FMeasure Measure(const FManagedArrayCollection& Collection, const TArray<float>& Thresholds, int32 MaxPieces = 0);
	/** kg, from the density the asset holds (kg/m3) or its total mass. */
	float MassOf(const FMeasure& Measure, bool bMassAsDensity, float Mass);
	/** What could go wrong in play. */
	TArray<FString> Warn(const FMeasure& Measure, const TArray<float>& Thresholds, bool bPlaced, int32 SitesAsked);
	FString VectorText(const FVector& Value);

	/** The asset as the plan leaves it, inside the plan's undo step. */
	void WriteAsset(UGeometryCollection& Asset, const FBuilt& Built, const TArray<FSource>& Sources);

	/** A hyper_verify_run request: damage at Center reaching Radius, then at least MinPieces pieces broken off Label. */
	FString VerifyCaseJson(const FString& Label, const FVector& Center, double Radius, int32 MinPieces);

	/** Draws the pieces pushed apart from their middle, through a temporary static mesh and the thumbnail renderer. */
	bool DrawExploded(const FManagedArrayCollection& Collection, const TArray<TObjectPtr<UMaterialInterface>>& Materials, const FString& PreviewName,
		FString& OutPath, FString& OutReason);
	/** Tests only: the dry runs and plans of the logic tests skip drawing. */
	void SetPreviewsOffForTests(bool bOff);
}
