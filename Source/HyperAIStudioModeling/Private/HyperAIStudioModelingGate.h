// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioModelingToolset.h"
#include "UObject/StrongObjectPtr.h"

class UDynamicMesh;
class UDynamicMeshPool;
class UMaterialInterface;
class UStaticMesh;

/** The only code that calls Geometry Script: model plans checked, built in memory, and written as static meshes. */
namespace HyperAIStudio::Modeling::Gate
{
	/** What part of a mesh an op works on. */
	struct FSelector
	{
		enum class EType : uint8 { All, Normal, Box, Sphere, Plane, Material };
		EType Type = EType::All;
		FVector A = FVector::ZeroVector;
		FVector B = FVector::ZeroVector;
		double Value = 0.0;
		bool bInvert = false;
		int32 Grow = 0;
		/** The op named a selection; without one, selection ops take the whole mesh. */
		bool bGiven = false;
	};

	struct FKindSpec;

	/** One op checked: every field parsed, nothing left to read from text. */
	struct FStep
	{
		/** Its place in the expanded plan, and the request op it came from. */
		int32 Index = 0;
		int32 From = 0;
		bool bFromRecipe = false;
		const FKindSpec* Spec = nullptr;
		FString Kind;
		FString Mesh;
		FString Source;
		FString Asset;
		FVector Location = FVector::ZeroVector;
		bool bLocation = false;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;
		FVector Size = FVector::ZeroVector;
		bool bSize = false;
		TArray<FVector2D> Points;
		TArray<FVector> Path;
		FSelector Select;
		/** Params by lower-case name, each checked against its kind's list. */
		TMap<FString, FString> Params;

		FTransform Transform() const { return FTransform(Rotation, Location, Scale); }
		bool Has(const TCHAR* Name) const;
		double Float(const TCHAR* Name, double Default) const;
		int32 Int(const TCHAR* Name, int32 Default) const;
		bool Bool(const TCHAR* Name, bool Default) const;
		FString Text(const TCHAR* Name, const TCHAR* Default) const;
		FVector Vector(const TCHAR* Name, const FVector& Default) const;
		FLinearColor Color(const TCHAR* Name, const FLinearColor& Default) const;
		/** "op 3 (bevel, mesh Rock)", and the recipe it came from. */
		FString Describe() const;
	};

	/** A plan checked: recipes expanded, every op parsed, triangles predicted under the limits. */
	struct FPlan
	{
		TArray<FStep> Steps;
		/** The expanded ops one per line, when a recipe expanded. */
		TArray<FString> ExpandedLines;
		TArray<FString> Notes;
		/** Hash of the expanded ops: the same ops and seeds build the same meshes. */
		FString Hash;
		int32 Writes = 0;
	};

	bool Check(const TArray<FHyperAIModelOp>& Ops, FPlan& OutPlan, FString& OutError);

	/** A plan's working meshes, in a transient pool freed with it. */
	class FBuilt : FNoncopyable
	{
	public:
		FBuilt();
		~FBuilt();
		UDynamicMesh* Find(const FString& Name) const;
		UDynamicMesh* FindOrAdd(const FString& Name);
		void Remove(const FString& Name);
		UDynamicMesh* RequestTemp();
		void ReturnTemp(UDynamicMesh* Mesh);
		/** A write_asset's mesh as it was at its op, so later ops on the working mesh (a drop too) leave the asset as planned. */
		void Keep(int32 StepIndex, const UDynamicMesh& Mesh);
		UDynamicMesh* Kept(int32 StepIndex) const;
		/** Names in the order they were made, as the plan wrote them. */
		TArray<FString> Names;
		int32 BuildMs = 0;

	private:
		TStrongObjectPtr<UDynamicMeshPool> Pool;
		TMap<FString, UDynamicMesh*> Meshes;
		TMap<int32, UDynamicMesh*> Writes;
	};

	/** Runs the plan's ops on meshes in memory; stops at the first op that fails or passes a limit, naming it. */
	bool Build(const FPlan& Plan, FBuilt& Built, FString& OutError);
	void Describe(const FBuilt& Built, TArray<FHyperAIModelMeshStats>& OutStats);
	/** What each write_asset would write: triangles, slots, Nanite and simple collision shapes. */
	void PreviewWrites(const FPlan& Plan, FBuilt& Built, TArray<FHyperAIModelAssetResult>& OutAssets);
	/**
	 * Writes each write_asset's static mesh: new assets made without a transaction of their own (undo cannot delete
	 * them), replaced ones changed inside the caller's. OutCreated lists the new ones, for cleanup on failure.
	 */
	bool Write(const FPlan& Plan, FBuilt& Built, TArray<FHyperAIModelAssetResult>& OutAssets, TArray<UStaticMesh*>& OutWritten,
		TArray<UStaticMesh*>& OutCreated, TArray<FString>& OutNotes, FString& OutError);
	/** New assets a failed plan made, deleted again: a plan never leaves one half-written. */
	void Discard(TArray<UStaticMesh*>& Created);
	/** Finished compiling: render data, LOD0 triangles near the plan's, the collision asked for. */
	bool Verify(const TArray<UStaticMesh*>& Written, const TArray<FHyperAIModelAssetResult>& Expected, FString& OutError);
	/** The vertex-colour material; made and saved on first use when bCreate. */
	UMaterialInterface* VertexColorMaterial(bool bCreate, FString& OutError);

	/** One static mesh as a prop: issues found, and a line describing it. */
	void CheckAsset(UStaticMesh& Mesh, bool bProduction, TArray<FHyperAIModelIssue>& OutIssues, FString& OutLine);
	/** One line per op kind and per recipe, for inspect. */
	TArray<FString> DescribeKinds();
	TArray<FString> DescribeLimits();

	/** "x,y,z" lists, "x,y; x,y" profiles and "Name=Value; ..." params, parsed; exposed for tests. */
	bool ParseNumbers(const FString& Text, TArray<double>& OutValues);
	bool ParseProfile(const FString& Text, TArray<FVector2D>& OutPoints, FString& OutError);
	bool ParseParams(const FString& Text, TArray<TPair<FString, FString>>& OutPairs, FString& OutError);
	bool ParseColor(const FString& Text, FLinearColor& OutColor);
}

/** Recipe macros: a whole prop as ordinary ops, so a dry run shows exactly what will run. */
namespace HyperAIStudio::Modeling::Recipes
{
	struct FInfo
	{
		const TCHAR* Name;
		/** Centimeters: across for rocks, bushes and clusters, tall for the rest. */
		double DefaultSize;
		const TCHAR* What;
	};

	TArrayView<const FInfo> List();
	const FInfo* Find(const FString& Name);

	struct FParams
	{
		FString Mesh;
		FString Shape;
		double Size = 0.0;
		int32 Seed = 0;
		/** 1 to 3: more steps, more noise, more parts. */
		int32 Detail = 2;
		FLinearColor Tint = FLinearColor::Transparent;
		FLinearColor Accent = FLinearColor::Transparent;
		/** Where the finished model goes, its pivot at its base; empty when it stays at the origin. */
		FString Location;
		FString Rotation;
		FString Scale;
	};

	/** The recipe's ops; parts are named Mesh__part and dropped at the end. */
	bool Expand(const FParams& Params, TArray<FHyperAIModelOp>& OutOps, FString& OutError);
}
