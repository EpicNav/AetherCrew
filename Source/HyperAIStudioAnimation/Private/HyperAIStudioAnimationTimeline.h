// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAnimationToolset.h"
#include "HyperAIStudioTrustedExecution.h"

/**
 * The executable slice for montages, sequences, composites and skeletons. A plan whose ops all edit one of those assets runs
 * through the trusted executor the way a blend space plan does: applied as one undo step, checked, saved and verified fresh.
 * Animation Blueprint and pose asset plans stay staged.
 */
namespace HyperAIStudio::Animation::Timeline
{
	/** Every op is of a kind that executes (blend space ops included), whatever assets they edit. */
	bool AreExecutableKinds(const TArray<FHyperAIStudioAnimationBackendOperation>& Operations);

	/** Every op edits the same montage, sequence, composite or skeleton. */
	bool IsExecutablePlan(const TArray<FHyperAIStudioAnimationBackendOperation>& Operations);

	/** The asset's captured revision: everything inspect reads of it, hashed; empty when it is not loaded. */
	FString ContentKey(const FHyperAIStudioAnimationTypedPayload& Payload);

	/** One op in a line, for the approval summary. */
	FString Describe(const FHyperAIStudioAnimationBackendOperation& Operation);

	FHyperAIStudioDomainAdapterResult ExecutePhase(EHyperAIStudioDomainExecutionActionKind Phase, const FHyperAIStudioAnimationTypedPayload& Payload);
}
