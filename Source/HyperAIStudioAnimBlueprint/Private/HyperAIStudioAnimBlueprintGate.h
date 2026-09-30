// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAnimBlueprintToolset.h"

class UAnimBlueprint;
class USkeleton;

/** The only code that reads or writes Animation Blueprint graphs; the toolset handles contracts and execution. */
namespace HyperAIStudio::AnimBlueprint::Gate
{
	UAnimBlueprint* Resolve(const FString& Path, bool bLoad);
	USkeleton* ResolveSkeleton(const FString& Path);
	/** Empty path is AnimInstance; otherwise an AnimInstance subclass, or null with OutError. */
	UClass* ResolveParentClass(const FString& Path, FString& OutError);
	bool IsOpenInEditor(const UAnimBlueprint& Blueprint);
	/** up_to_date, warnings, dirty or error. */
	FString CompileStatusOf(const UAnimBlueprint& Blueprint);
	/** Skeleton, parent, variables and every graph's nodes, pins, links and node settings; not node positions. */
	FString ComputeRevision(const UAnimBlueprint& Blueprint);
	/** The text ComputeRevision hashes, line by line, for telling two revisions apart. */
	FString RevisionText(const UAnimBlueprint& Blueprint);
	void Read(const UAnimBlueprint& Blueprint, TArray<FHyperAIAnimBlueprintVariable>& OutVariables, TArray<FHyperAIAnimBlueprintMachine>& OutMachines);
	/** What feeds the output pose, from the output back: inertialization, slots, layered blends, cached poses, the state machine. */
	TArray<FString> OutputChain(const UAnimBlueprint& Blueprint);
	/** The Animation Layer Interfaces it implements, and each layer it has (its own or an interface's) with what plays in it. */
	void ReadLayers(const UAnimBlueprint& Blueprint, TArray<FString>& OutInterfaces, TArray<FString>& OutLayers);

	/** True for /Script/Engine.AnimLayerInterface: a plan creating with it makes an Animation Layer Interface. */
	bool IsLayerInterfaceClass(const FString& ParentClassPath);

	/** Checks every op against the graph as it would be at that point; Blueprint is null when the plan creates it. */
	bool ValidateOps(const UAnimBlueprint* Blueprint, const USkeleton* Skeleton, const TArray<FHyperAIAnimBlueprintOp>& Ops, FString& OutError,
		bool bCreatingInterface = false);
	UAnimBlueprint* Create(const FString& Path, USkeleton& Skeleton, UClass& ParentClass, FString& OutError);
	/** An Animation Layer Interface: for a skeleton, or for any when Skeleton is null. */
	UAnimBlueprint* CreateLayerInterface(const FString& Path, USkeleton* Skeleton, FString& OutError);
	/** Applies ops in order; the caller holds the transaction. */
	bool ApplyOps(UAnimBlueprint& Blueprint, const TArray<FHyperAIAnimBlueprintOp>& Ops, FString& OutError);
	bool Compile(UAnimBlueprint& Blueprint, FString& OutFirstError, int32& OutErrors, int32& OutWarnings);

	/** Structural and quality checks that do not need a compile. */
	TArray<FHyperAIAnimBlueprintIssue> Check(const UAnimBlueprint& Blueprint);
}
