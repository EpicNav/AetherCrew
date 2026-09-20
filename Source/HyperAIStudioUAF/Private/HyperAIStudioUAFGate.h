// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioUAFToolset.h"

class UUAFAnimGraph;

/** The only code that reads or writes UAF animation graphs; the toolset handles contracts and execution. */
namespace HyperAIStudio::UAF::Gate
{
	UUAFAnimGraph* Resolve(const FString& Path, bool bLoad);
	bool IsOpenInEditor(const UUAFAnimGraph& Graph);
	/** compiled, warnings, errors or not_compiled. */
	FString CompileStatusOf(const UUAFAnimGraph& Graph);
	/** Variables, trait stacks, their traits and pin values, and every link; not node positions. */
	FString ComputeRevision(const UUAFAnimGraph& Graph);
	void Read(const UUAFAnimGraph& Graph, TArray<FHyperAIUAFVariable>& OutVariables, TArray<FHyperAIUAFNode>& OutNodes, FString& OutOutputNode);
	/** Every registered trait and its pins, filtered by a case-insensitive substring of the trait name. */
	TArray<FHyperAIUAFTrait> ReadTraitCatalog(const FString& Filter);

	/** Checks every op against the graph as it would be at that point; Graph is null when the plan creates it. */
	bool ValidateOps(const UUAFAnimGraph* Graph, const TArray<FHyperAIUAFOp>& Ops, FString& OutError);
	UUAFAnimGraph* Create(const FString& Path, FString& OutError);
	/** Applies ops in order; the caller holds the transaction. */
	bool ApplyOps(UUAFAnimGraph& Graph, const TArray<FHyperAIUAFOp>& Ops, FString& OutError);
	bool Compile(UUAFAnimGraph& Graph, FString& OutFirstError, int32& OutErrors, int32& OutWarnings);

	/** Structural and quality checks, plus whether the graph allocates a runnable instance. */
	TArray<FHyperAIUAFIssue> Check(UUAFAnimGraph& Graph, bool& bOutAllocatesInstance);
}
