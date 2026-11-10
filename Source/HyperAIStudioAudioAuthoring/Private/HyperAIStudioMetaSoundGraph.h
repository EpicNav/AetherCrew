// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAudioAuthoringToolset.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendLiteral.h"

/**
 * MetaSound graphs from any registered node: the registry searched and described, values typed by their pins, and a
 * graph plan checked against the registry and built through Epic's MetaSound Builder.
 */
namespace HyperAIStudio::AudioAuthoring::Graph
{
	/** Namespace.Name.Variant (or Namespace.Name) as a class name; false when it has no namespace and name. */
	bool ParseClassName(const FString& Text, FMetasoundFrontendClassName& OutName);

	/** A class name as plans write it: Namespace.Name.Variant, or Namespace.Name when it has no variant. */
	FString ClassText(const FMetasoundFrontendClassName& Name);

	/** The registered class: MajorVersion 0 takes the highest. */
	bool FindClass(const FMetasoundFrontendClassName& Name, int32 MajorVersion, FMetasoundFrontendClass& OutClass);

	/** A class as a report gives it; bFull adds defaults, descriptions and enum values. */
	FHyperAIMetaSoundNodeClass Describe(const FMetasoundFrontendClass& Class, bool bFull);

	/** Text as a literal for a pin or member of DataType: numbers, true/false, enum names, asset paths, comma lists. */
	bool MakeLiteral(FName DataType, const FString& Text, FMetasoundFrontendLiteral& OutLiteral, FString& OutError);

	FHyperAIMetaSoundNodeSearchReport Search(const FHyperAIMetaSoundNodeSearchRequest& Request);
	FHyperAIMetaSoundNodeDescribeReport DescribeClasses(const FHyperAIMetaSoundNodeDescribeRequest& Request);

	struct FCheckedNode
	{
		FString Id;
		/** A node already in the MetaSound, by its GUID; Class is then empty. */
		FGuid ExistingId;
		FMetasoundFrontendClassName ClassName;
		int32 MajorVersion = 1;
		TArray<TPair<FName, FMetasoundFrontendLiteral>> Defaults;
		FIntPoint Cell = FIntPoint::ZeroValue;
	};

	struct FCheckedEndpoint
	{
		/** INDEX_NONE for the graph itself. */
		int32 Node = INDEX_NONE;
		FName Pin;
	};

	struct FCheckedPlan
	{
		FString Package;
		TWeakObjectPtr<UObject> Existing;
		bool bStereo = false;
		bool bOneShot = true;
		TArray<TPair<FName, FName>> Inputs;
		TArray<FMetasoundFrontendLiteral> InputDefaults;
		TArray<TPair<FName, FName>> Outputs;
		TArray<FCheckedNode> Nodes;
		TArray<TPair<FCheckedEndpoint, FCheckedEndpoint>> Connections;
	};

	/** Checks a plan against the registry and the MetaSound already at Path, and lays it out; Report gets its nodes and warnings. */
	bool Check(const FHyperAIMetaSoundGraphPlanRequest& Request, FCheckedPlan& OutPlan, FHyperAIMetaSoundGraphPlanReport& Report);

	/** Builds a checked plan into its MetaSound. Call inside a transaction; the asset is saved when bSave. */
	bool Build(const FCheckedPlan& Plan, bool bSave, FHyperAIMetaSoundGraphPlanReport& Report);

	/** A MetaSound's nodes and wires, as inspect lines: node <GUID> <Class> and wire <from> -> <to>. */
	TArray<FString> DescribeGraph(const UObject& MetaSound, int32 MaxLines = 200);
}
