// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioApprovalCategory.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTrustedExecution.h"

/** What the chat panel shows the user before an agent edit runs. */
struct FHyperAIStudioApprovalSummary
{
	FString PackId;
	FString ToolName;
	FString VariantId;
	EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Edit;
	/** Object path of the asset the plan changes. */
	FString EffectTarget;
	/** The plan hash the agent echoed back, shown so a reviewed plan is the one that runs. */
	FString PlanHash;
	/** One line per effect, e.g. "set_module_enabled on Fire/ParticleUpdateScript/Colour". */
	TArray<FString> Effects;
	/** Assets the plan overwrites or creates, if any. */
	TArray<FString> Touches;
	/** hyper_script_run's escalated scripts. Auto-approval files them under Scripts whatever Safety says. */
	bool bEscalatedScript = false;
};

struct FHyperAIStudioPendingApproval
{
	FString OperationId;
	FHyperAIStudioApprovalSummary Summary;
	FDateTime RequestedUtc = FDateTime::MinValue();
	/** What auto-approval files it under; unset when only a click can approve it. */
	TOptional<EHyperAIStudioApprovalCategory> Category;
	/** The auto-approval the user turned on covers it and approves it on the next quiet editor tick. */
	bool bAutoApproving = false;

	/** An undoable edit by its sealed variant as well as its declared safety: what a running agent job may approve. */
	bool IsPlainEdit() const
	{
		return Summary.Safety == EHyperAIStudioDomainSafety::Edit && Category == EHyperAIStudioApprovalCategory::Edits;
	}
};

/** Who approved a held request. */
enum class EHyperAIStudioApprover : uint8
{
	/** A person's click: the Activity card, the remote page, a tool form, or a panel's own Generate button. */
	User,
	/** The auto-approval the user turned on for the request's category. */
	AutoApproval,
	/** A running agent job the user started, for an edit in one of its packs. */
	AgentJob
};

/** Who approved an operation, and as what. */
struct FHyperAIStudioApprovalRecord
{
	FString OperationId;
	EHyperAIStudioApprover ApprovedBy = EHyperAIStudioApprover::User;
	/** Set when auto-approval approved it; the category that covered it. */
	TOptional<EHyperAIStudioApprovalCategory> AutoApprovedAs;
	FDateTime ApprovedUtc = FDateTime::MinValue();
};

/**
 * Holds an agent's prepared mutation until the user approves it in the chat panel.
 *
 * The plan is held prepared, not staged: a staged artifact expires in seconds, while a person may
 * take minutes. Staging and submitting happen on approval, so the reviewed plan is the one that
 * runs, and a risky variant's server grant is issued only from that approval: the user's click, or
 * the auto-approval the user turned on for its category in Activity.
 *
 * Game thread only.
 */
class HYPERAISTUDIO_API FHyperAIStudioApprovalGate
{
public:
	static constexpr int32 MaxPending = 16;

	/**
	 * True when this safety class must be approved before it runs, per the user's settings. Auto-approval does not
	 * change this: covered requests still pass through the gate, so they are recorded and approved on the same path
	 * as a click.
	 */
	static bool IsApprovalRequired(EHyperAIStudioDomainSafety Safety);

	/**
	 * Queue a prepared plan for approval. The pack should report awaiting_user_approval to the agent
	 * and stop; the gate submits it if and when the user approves.
	 */
	static bool Request(
		const FHyperAIStudioTrustedPreparedArtifact& Prepared,
		const FString& OperationId,
		const FHyperAIStudioApprovalSummary& Summary,
		FString& OutError);
	/**
	 * Queue work that is not a typed artifact, such as an agent's script, for approval. Approve runs it on the
	 * game thread; the action records its own Completed or Failed activity entry.
	 */
	static bool RequestAction(
		const FString& OperationId,
		const FHyperAIStudioApprovalSummary& Summary,
		TFunction<void()> Run,
		FString& OutError);

	static TArray<FHyperAIStudioPendingApproval> GetPending();
	static bool HasPending(const FString& OperationId);
	/** True while OperationId waits and the user's auto-approval will approve it on the next quiet editor tick. */
	static bool IsAutoApproving(const FString& OperationId);
	/**
	 * For a pack's reply right after Request or RequestAction. When the user's auto-approval will approve OperationId,
	 * sets Status to queued and Diagnostic to what hyper_operation_status says of it, and returns true; otherwise leaves
	 * both as they are (awaiting_user_approval).
	 */
	static bool DescribeHeld(const FString& OperationId, FString& InOutStatus, FString& InOutDiagnostic);
	/** How OperationId was approved, among the last 64 approvals of this editor session. */
	static TOptional<FHyperAIStudioApprovalRecord> FindApproval(const FString& OperationId);

	/**
	 * Run the held plan, or queue it behind the edit that owns the project lane. On failure the request is
	 * dropped and OutError explains why. This is the click; auto-approval runs the same code.
	 */
	static bool Approve(const FString& OperationId, FString& OutError);
	/** Approve, by a running agent job the user started rather than a click; recorded and reported as such. */
	static bool ApproveForJob(const FString& OperationId, FString& OutError);
	static void Reject(const FString& OperationId, const FString& Reason);
	static void Clear();

	static FSimpleMulticastDelegate& OnChanged();
};
