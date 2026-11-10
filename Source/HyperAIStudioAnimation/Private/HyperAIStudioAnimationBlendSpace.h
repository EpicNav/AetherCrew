// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAnimationToolset.h"
#include "HyperAIStudioTrustedExecution.h"

/**
 * The executable slice of animation_rigging. A plan made only of blend_space.create, set_axis and set_samples for one
 * blend space runs through the trusted executor: apply as one undo step, validate the samples, save, verify fresh.
 * Every other plan stays staged-only.
 */
namespace HyperAIStudio::Animation::BlendSpace
{
	bool IsExecutablePlan(const TArray<FHyperAIStudioAnimationBackendOperation>& Operations);

	/** The blend space the plan edits. */
	FString EffectTarget(const FHyperAIStudioAnimationTypedPayload& Payload);

	/** The blend space's class, axes and samples, hashed; empty when it is not loaded. */
	FString ContentKey(const FHyperAIStudioAnimationTypedPayload& Payload);

	FHyperAIStudioDomainAdapterResult ExecutePhase(EHyperAIStudioDomainExecutionActionKind Phase, const FHyperAIStudioAnimationTypedPayload& Payload);
}

class FHyperAIStudioAnimationFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
{
public:
	virtual FString GetOwnerAdapterFingerprint() const override;
	virtual bool ResolveCanonicalEffectTarget(
		const IHyperAIStudioTypedArtifactPayload& Request,
		FString& OutCanonicalEffectTarget,
		FString& OutError) override;
	virtual bool VerifyFreshExact(
		const IHyperAIStudioTypedArtifactPayload& Request,
		const IHyperAIStudioDomainResultPayload& Result,
		FString& OutPostconditionHash,
		FString& OutError) override;
};
