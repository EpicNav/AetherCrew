// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioUIToolset.h"

class UWidgetBlueprint;

/**
 * Widget Blueprint authoring, the executable slice of ui_slate_mvvm. A plan of widget.add, widget.set_property,
 * widget.set_style, widget.set_slot, widget.set_navigation, animation.create, animation.add_float_track and
 * animation.add_transform_track runs through the trusted executor: apply as one undo step, compile, check each op
 * landed, save, verify fresh. Plans with renames, deletes or bindings keep the persisted-shadow route, staged-only.
 */
namespace HyperAIStudio::UI::Animation
{
	static constexpr const TCHAR* VariantId = TEXT("ui.widget_animation_edit.v1");

	/** Every op is one the executor runs, and no widget op carries the older per-element fingerprint. */
	bool IsExecutablePlan(const TArray<FHyperAIUIPlanOperation>& Operations);
	FHyperAIUIApplyPlanReport BuildPlan(const FHyperAIUIApplyPlanRequest& Request);
	/** The saved package and its dirtiness, the widget tree with slots and navigation, and every animation's tracks and keys, hashed. */
	FString ComputeRevision(const UWidgetBlueprint& Blueprint);
}

class FHyperAIStudioUIAnimationPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BaseRevision;
	TArray<FHyperAIUIPlanOperation> Operations;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

class FHyperAIStudioUIAnimationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString ContentKey;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

/** Inspect and validate reach the adapter too: a registered adapter must cover its whole cohort. */
class FHyperAIStudioUIInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIUIInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioUIValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIUIValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioUIInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIUIInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioUIValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIUIValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioUIRenderReviewPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIUIRenderReviewRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioUIRenderReviewResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIUIRenderReviewReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioUIAnimationAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	static const FHyperAIStudioDomainAdapterDescriptor& Describe();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override { return Describe(); }
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
};

class FHyperAIStudioUIAnimationFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
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
