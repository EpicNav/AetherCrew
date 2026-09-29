// Games by Hyper 2026.

#include "HyperAIStudioUAFToolset.h"
#include "HyperAIStudioRequestFit.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "FileHelpers.h"
#include "Graph/AnimNextAnimationGraph.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioExecutionQueue.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioUAFCookbook.h"
#include "HyperAIStudioUAFGate.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioUAF, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioUAF"

namespace HyperAIStudio::UAF::Private
{
	namespace Gate = HyperAIStudio::UAF::Gate;

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += FString::FromInt(Value.Len());
		Canonical += TEXT(":");
		Canonical += Value;
		Canonical += TEXT("\n");
	}

	int64 EstimateBytes(const FString& Value)
	{
		return 16 + 2ll * Value.Len();
	}

	int64 EstimateTraitBytes(const FHyperAIUAFTrait& Trait)
	{
		int64 Size = 32 + EstimateBytes(Trait.Name) + EstimateBytes(Trait.Mode);
		for (const FHyperAIUAFPin& Pin : Trait.Pins)
		{
			Size += 24 + EstimateBytes(Pin.Name) + EstimateBytes(Pin.Type) + EstimateBytes(Pin.Value);
		}
		return Size;
	}

	int64 EstimateNodeBytes(const FHyperAIUAFNode& Node)
	{
		int64 Size = 32 + EstimateBytes(Node.Name);
		for (const FHyperAIUAFTrait& Trait : Node.Traits) Size += EstimateTraitBytes(Trait);
		return Size;
	}

	int64 EstimateIssueBytes(const FHyperAIUAFIssue& Issue)
	{
		return 32 + EstimateBytes(Issue.Code) + EstimateBytes(Issue.Severity) + EstimateBytes(Issue.Subject) + EstimateBytes(Issue.Message);
	}

	int32 ClampBytes(const int64 Size)
	{
		return static_cast<int32>(FMath::Clamp<int64>(Size, 1, MAX_int32));
	}

	FHyperAIStudioDomainAdapterResult ExecuteEditPhase(
		const EHyperAIStudioDomainExecutionActionKind Phase,
		const FHyperAIStudioUAFEditOpsPayload& Payload)
	{
		FHyperAIStudioDomainAdapterResult Result;
		const double Started = FPlatformTime::Seconds();
		auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status,
			const FString& Diagnostic, const TCHAR* ResultPhase = nullptr, const FString& ContentKey = FString())
		{
			UE_LOG(LogHyperAIStudioUAF, Log, TEXT("Edit phase %d finished %s in %.1f ms. %s"), static_cast<int32>(Phase), *Status,
				(FPlatformTime::Seconds() - Started) * 1000.0, *Diagnostic);
			Result.Outcome = Outcome;
			Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
			Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
			if (Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				const TSharedRef<FHyperAIStudioUAFMutationResultPayload, ESPMode::ThreadSafe> Output =
					MakeShared<FHyperAIStudioUAFMutationResultPayload, ESPMode::ThreadSafe>();
				Output->Phase = ResultPhase;
				Output->ContentKey = ContentKey;
				Result.Payload = Output;
			}
			return Result;
		};
		const bool bApply = Phase == EHyperAIStudioDomainExecutionActionKind::Apply;
		const EHyperAIStudioDomainDispatchOutcome FailedOutcome = bApply
			? EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect
			: EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect;
		if (!IsInGameThread())
		{
			return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("UAF graph edits run on the game thread."));
		}
		UUAFAnimGraph* Graph = Gate::Resolve(Payload.TargetPath, /*bLoad=*/false);
		if (bApply)
		{
			FString Error;
			if (Payload.bCreate)
			{
				if (Graph || FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Payload.TargetPath)))
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("create_precondition_changed"),
						TEXT("An asset appeared at target_path after planning; nothing was created."));
				}
			}
			else if (!Graph || Gate::ComputeRevision(*Graph) != Payload.BaseRevision)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("stale_revision"),
					TEXT("The animation graph changed or unloaded after planning; nothing was applied."));
			}
			else if (Gate::IsOpenInEditor(*Graph))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("target_open_in_editor"),
					TEXT("The animation graph was opened in its editor after planning; nothing was applied."));
			}
			{
				const FScopedTransaction Transaction(Payload.bCreate
					? LOCTEXT("CreateUAFGraph", "HyperAI: Create UAF Animation Graph")
					: LOCTEXT("EditUAFGraph", "HyperAI: Edit UAF Animation Graph"));
				if (Payload.bCreate)
				{
					Graph = Gate::Create(Payload.TargetPath, Error);
					if (!Graph)
					{
						return Finish(EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect, TEXT("create_failed"), Error);
					}
				}
				if (!Gate::ApplyOps(*Graph, Payload.Ops, Error))
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("edit_op_failed"), Error);
				}
			}
			// Compiled here, outside the undo step: this phase has the budget for it, and the executor schedules a
			// separate compile phase only for plans that also save.
			FString FirstError;
			int32 Errors = 0;
			int32 Warnings = 0;
			if (!Gate::Compile(*Graph, FirstError, Errors, Warnings))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("compile_failed"),
					FString::Printf(TEXT("%d compile errors, first: %s The edit is in memory and was not saved; undo reverts it."), Errors, *FirstError));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("applied"),
				FString::Printf(TEXT("%s%d ops applied, compiled with %d warnings."), Payload.bCreate ? TEXT("Created; ") : TEXT(""),
					Payload.Ops.Num(), Warnings), TEXT("applied"));
		}
		if (!Graph)
		{
			return Finish(FailedOutcome, TEXT("target_not_loaded"), TEXT("The animation graph is no longer loaded."));
		}

		switch (Phase)
		{
		case EHyperAIStudioDomainExecutionActionKind::Validate:
			if (Gate::CompileStatusOf(*Graph) == TEXT("errors"))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("compile_error"),
					TEXT("The graph is in an error state after the edit; undo reverts it."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"), TEXT("The graph compiles."), TEXT("validated"));
		case EHyperAIStudioDomainExecutionActionKind::Save:
			if (!UEditorLoadingAndSavingUtils::SavePackages({Graph->GetOutermost()}, /*bOnlyDirty=*/false))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("save_failed"),
					TEXT("The edit applied but the package did not save; it remains in memory."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("saved"), TEXT("Package saved."), TEXT("saved"));
		case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"),
				TEXT("Fresh graph captured for verification."), TEXT("completed"), Gate::ComputeRevision(*Graph));
		default:
			return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Unknown execution phase."));
		}
	}
}

FString FHyperAIStudioUAFContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioUAF.HyperAIStudioUAFToolset");
}

TArray<FString> FHyperAIStudioUAFContracts::GetToolNames()
{
	return {InspectToolName, MutationToolName, ValidateToolName};
}

bool FHyperAIStudioUAFContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

TArray<FString> FHyperAIStudioUAFContracts::GetCapabilities()
{
	return {
		TEXT("create_uaf_animation_graphs"),
		TEXT("bool_float_int_name_and_vector_variables"),
		TEXT("trait_stacks_from_the_live_trait_registry"),
		TEXT("trait_pin_values_and_pins_bound_to_variables"),
		TEXT("pose_links_between_stacks_and_to_the_output_pose"),
		TEXT("compile_with_errors_reported_as_one_undo_step"),
		TEXT("checks_output_pose_base_traits_pose_pins_orphans_assets_and_instance_allocation"),
		TEXT("unsupported:modules_event_graphs_workspaces_and_binding_a_graph_to_a_character")};
}

bool FHyperAIStudioUAFContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT("*")) || Path.Contains(TEXT("?")) || Path.Contains(TEXT(":")))
	{
		return false;
	}
	for (const TCHAR Character : Path)
	{
		if (Character < 0x20 || Character == 0x7F) return false;
	}
	FText Reason;
	if (!FPackageName::IsValidObjectPath(Path, &Reason)) return false;
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	const FString ObjectName = FPackageName::ObjectPathToObjectName(Path);
	return !PackageName.IsEmpty() && !ObjectName.IsEmpty() && FPackageName::GetLongPackageAssetName(PackageName) == ObjectName;
}

bool FHyperAIStudioUAFContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"))) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

FString FHyperAIStudioUAFContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("uaf_graph.inspect.payload.v1|optional_exact_target|optional_load|trait_catalog|filter|output_bytes"));
	return Value;
}

FString FHyperAIStudioUAFContracts::EditOpsPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("uaf_graph.edit_ops.payload.v1|one_target|base_revision_or_create|ordered_closed_ops|compile|save|deep_clone|bounded"));
	return Value;
}

FString FHyperAIStudioUAFContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("uaf_graph.validate.payload.v1|exact_target|output_bytes"));
	return Value;
}

FString FHyperAIStudioUAFContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("uaf_graph.inspect.result.v1|revision|compile_status|output_node|variables|stacks_traits_pins_links|trait_catalog|bounded"));
	return Value;
}

FString FHyperAIStudioUAFContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("uaf_graph.edit_ops.result.v1|phase|content_key|bounded"));
	return Value;
}

FString FHyperAIStudioUAFContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("uaf_graph.validate.result.v1|compile_status|instance|issues|counts|bounded"));
	return Value;
}

FString FHyperAIStudioUAFInspectPayload::GetTypeId() const { return FHyperAIStudioUAFContracts::InspectPayloadTypeId; }
FString FHyperAIStudioUAFInspectPayload::GetSchemaFingerprint() const { return FHyperAIStudioUAFContracts::InspectPayloadSchemaFingerprint(); }
int32 FHyperAIStudioUAFInspectPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::UAF::Private::ClampBytes(64 + 2ll * (Request.TargetPath.Len() + Request.TraitFilter.Len()));
}

FString FHyperAIStudioUAFValidatePayload::GetTypeId() const { return FHyperAIStudioUAFContracts::ValidatePayloadTypeId; }
FString FHyperAIStudioUAFValidatePayload::GetSchemaFingerprint() const { return FHyperAIStudioUAFContracts::ValidatePayloadSchemaFingerprint(); }
int32 FHyperAIStudioUAFValidatePayload::GetBoundedByteSize() const
{
	return HyperAIStudio::UAF::Private::ClampBytes(64 + 2ll * Request.TargetPath.Len());
}

FString FHyperAIStudioUAFEditOpsPayload::GetTypeId() const { return FHyperAIStudioUAFContracts::EditOpsPayloadTypeId; }
FString FHyperAIStudioUAFEditOpsPayload::GetSchemaFingerprint() const { return FHyperAIStudioUAFContracts::EditOpsPayloadSchemaFingerprint(); }
int32 FHyperAIStudioUAFEditOpsPayload::GetBoundedByteSize() const
{
	using HyperAIStudio::UAF::Private::EstimateBytes;
	int64 Size = 160 + EstimateBytes(TargetPath) + EstimateBytes(BaseRevision) + EstimateBytes(SemanticFingerprint);
	for (const FHyperAIUAFOp& Op : Ops)
	{
		Size += 32 + EstimateBytes(Op.Kind) + EstimateBytes(Op.Node) + EstimateBytes(Op.Trait) + EstimateBytes(Op.Pin)
			+ EstimateBytes(Op.To) + EstimateBytes(Op.Name) + EstimateBytes(Op.Value);
	}
	return HyperAIStudio::UAF::Private::ClampBytes(Size);
}
FString FHyperAIStudioUAFEditOpsPayload::GetSemanticFingerprint() const { return SemanticFingerprint; }
TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> FHyperAIStudioUAFEditOpsPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioUAFEditOpsPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioUAFEditOpsPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BaseRevision = BaseRevision;
	Clone->bCreate = bCreate;
	Clone->Ops = Ops;
	Clone->bSave = bSave;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioUAFInspectResultPayload::GetTypeId() const { return FHyperAIStudioUAFContracts::InspectResultTypeId; }
FString FHyperAIStudioUAFInspectResultPayload::GetSchemaFingerprint() const { return FHyperAIStudioUAFContracts::InspectResultSchemaFingerprint(); }
int32 FHyperAIStudioUAFInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::UAF::Private;
	int64 Size = 512 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.Revision);
	for (const FHyperAIUAFVariable& Variable : Report.Variables) Size += 32 + EstimateBytes(Variable.Name) + EstimateBytes(Variable.Type);
	for (const FHyperAIUAFNode& Node : Report.Nodes) Size += EstimateNodeBytes(Node);
	for (const FHyperAIUAFTrait& Trait : Report.TraitCatalog) Size += EstimateTraitBytes(Trait);
	return ClampBytes(Size);
}

FString FHyperAIStudioUAFValidateResultPayload::GetTypeId() const { return FHyperAIStudioUAFContracts::ValidateResultTypeId; }
FString FHyperAIStudioUAFValidateResultPayload::GetSchemaFingerprint() const { return FHyperAIStudioUAFContracts::ValidateResultSchemaFingerprint(); }
int32 FHyperAIStudioUAFValidateResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::UAF::Private;
	int64 Size = 256 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic);
	for (const FHyperAIUAFIssue& Issue : Report.Issues) Size += EstimateIssueBytes(Issue);
	return ClampBytes(Size);
}

FString FHyperAIStudioUAFMutationResultPayload::GetTypeId() const { return FHyperAIStudioUAFContracts::MutationResultTypeId; }
FString FHyperAIStudioUAFMutationResultPayload::GetSchemaFingerprint() const { return FHyperAIStudioUAFContracts::MutationResultSchemaFingerprint(); }
int32 FHyperAIStudioUAFMutationResultPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::UAF::Private::ClampBytes(96 + 2ll * (Phase.Len() + ContentKey.Len()));
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioUAFContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.uaf_graph.animation.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({InspectToolName, InspectVariantId, InspectPayloadTypeId, InspectPayloadSchemaFingerprint(),
			InspectResultTypeId, InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({MutationToolName, MutationVariantId, EditOpsPayloadTypeId, EditOpsPayloadSchemaFingerprint(),
			MutationResultTypeId, MutationResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({ValidateToolName, ValidateVariantId, ValidatePayloadTypeId, ValidatePayloadSchemaFingerprint(),
			ValidateResultTypeId, ValidateResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FHyperAIUAFInspectReport FHyperAIStudioUAFContracts::Inspect(const FHyperAIUAFInspectRequest& Request)
{
	using namespace HyperAIStudio::UAF::Private;
	FHyperAIUAFInspectReport Report;
	Report.Capabilities = GetCapabilities();
	Report.TargetPath = Request.TargetPath;
	auto Reject = [&](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"), TEXT("UAF graph inspection runs on the game thread."));
	}
	const bool bCatalogOnly = Request.TargetPath.IsEmpty();
	if ((!bCatalogOnly && !IsCanonicalProjectObjectPath(Request.TargetPath))
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes
		|| Request.TraitFilter.Len() > 64)
	{
		return Reject(TEXT("invalid_request"), TEXT("Pass one canonical /Game object path (or none for the trait catalog), a trait_filter up to 64 characters, and max_output_bytes 16-128 KB."));
	}
	if (bCatalogOnly && !Request.bIncludeTraitCatalog)
	{
		return Reject(TEXT("invalid_request"), TEXT("With no target_path, set include_trait_catalog to get the trait vocabulary."));
	}

	int64 Budget = 2048;
	if (!bCatalogOnly)
	{
		const UUAFAnimGraph* Graph = Gate::Resolve(Request.TargetPath, Request.bLoad);
		if (!Graph)
		{
			const bool bExists = FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Request.TargetPath));
			return Reject(bExists ? TEXT("target_not_loaded") : TEXT("target_not_found"), bExists
				? FString(TEXT("The asset exists but is not a loaded UAF animation graph; inspect with bLoad."))
				: FString(TEXT("No UAF animation graph exists there; create one with apply_plan bCreate.")));
		}
		Report.Revision = Gate::ComputeRevision(*Graph);
		Report.CompileStatus = Gate::CompileStatusOf(*Graph);
		Report.bOpenInEditor = Gate::IsOpenInEditor(*Graph);
		Report.bPackageDirty = Graph->GetOutermost()->IsDirty();
		TArray<FHyperAIUAFNode> Nodes;
		Gate::Read(*Graph, Report.Variables, Nodes, Report.OutputNode);
		for (const FHyperAIUAFVariable& Variable : Report.Variables) Budget += 32 + EstimateBytes(Variable.Name) + EstimateBytes(Variable.Type);
		for (FHyperAIUAFNode& Node : Nodes)
		{
			const int64 Bytes = EstimateNodeBytes(Node);
			if (Budget + Bytes > Request.MaxOutputBytes)
			{
				Report.bTruncated = true;
				break;
			}
			Budget += Bytes;
			Report.Nodes.Add(MoveTemp(Node));
		}
	}
	if (Request.bIncludeTraitCatalog)
	{
		for (FHyperAIUAFTrait& Trait : Gate::ReadTraitCatalog(Request.TraitFilter))
		{
			const int64 Bytes = EstimateTraitBytes(Trait);
			if (Budget + Bytes > Request.MaxOutputBytes)
			{
				Report.bTruncated = true;
				break;
			}
			Budget += Bytes;
			Report.TraitCatalog.Add(MoveTemp(Trait));
		}
	}
	Report.bOk = true;
	Report.Status = Report.bTruncated ? TEXT("truncated") : TEXT("captured");
	Report.Diagnostic = bCatalogOnly
		? FString::Printf(TEXT("%d traits%s."), Report.TraitCatalog.Num(), Report.bTruncated ? TEXT(", truncated; narrow it with trait_filter") : TEXT(""))
		: FString::Printf(TEXT("%d variables, %d trait stacks, output pose from %s, compile status %s%s."),
			Report.Variables.Num(), Report.Nodes.Num(), Report.OutputNode.IsEmpty() ? TEXT("nothing") : *Report.OutputNode,
			*Report.CompileStatus, Report.bOpenInEditor ? TEXT("; open in its editor, so close it before applying edits") : TEXT(""));
	return Report;
}

FHyperAIUAFValidateReport FHyperAIStudioUAFContracts::Validate(const FHyperAIUAFValidateRequest& Request)
{
	using namespace HyperAIStudio::UAF::Private;
	FHyperAIUAFValidateReport Report;
	Report.Capabilities = GetCapabilities();
	auto Reject = [&](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"), TEXT("UAF graph validation runs on the game thread."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath) || Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_request"), TEXT("Pass one canonical /Game object path and max_output_bytes 16-128 KB."));
	}
	UUAFAnimGraph* Graph = Gate::Resolve(Request.TargetPath, /*bLoad=*/false);
	if (!Graph)
	{
		return Reject(TEXT("target_not_loaded"), TEXT("The animation graph is not loaded; inspect it with bLoad first."));
	}
	Report.CompileStatus = Gate::CompileStatusOf(*Graph);
	int64 Budget = 1024;
	for (const FHyperAIUAFIssue& Issue : Gate::Check(*Graph, Report.bAllocatesInstance))
	{
		(Issue.Severity == TEXT("error") ? Report.ErrorCount : Report.WarningCount) += 1;
		const int64 Bytes = EstimateIssueBytes(Issue);
		if (Budget + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			continue;
		}
		Budget += Bytes;
		Report.Issues.Add(Issue);
	}
	Report.bOk = true;
	Report.Status = Report.ErrorCount > 0 ? TEXT("invalid") : Report.WarningCount > 0 ? TEXT("valid_with_warnings") : TEXT("valid");
	Report.Diagnostic = FString::Printf(TEXT("%d errors, %d warnings; compile status %s; %s."), Report.ErrorCount, Report.WarningCount,
		*Report.CompileStatus, Report.bAllocatesInstance ? TEXT("the graph allocates a runnable instance") : TEXT("the graph does not allocate a runnable instance"));
	return Report;
}

FString FHyperAIStudioUAFContracts::ComputeEditOpsSemanticFingerprint(const FHyperAIStudioUAFEditOpsPayload& Payload)
{
	using HyperAIStudio::UAF::Private::AppendToken;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.uaf_graph.edit-ops-intent.v1"));
	AppendToken(Canonical, PackId);
	AppendToken(Canonical, MutationToolName);
	AppendToken(Canonical, MutationVariantId);
	AppendToken(Canonical, EditOpsPayloadSchemaFingerprint());
	AppendToken(Canonical, GetAdapterDescriptor().AdapterFingerprint);
	AppendToken(Canonical, Payload.TargetPath);
	AppendToken(Canonical, Payload.BaseRevision);
	AppendToken(Canonical, Payload.bCreate ? TEXT("create") : TEXT("edit"));
	AppendToken(Canonical, Payload.bSave ? TEXT("save") : TEXT("no_save"));
	AppendToken(Canonical, FString::FromInt(Payload.Ops.Num()));
	for (const FHyperAIUAFOp& Op : Payload.Ops)
	{
		for (const FString* Field : {&Op.Kind, &Op.Node, &Op.Trait, &Op.Pin, &Op.To, &Op.Name, &Op.Value})
		{
			AppendToken(Canonical, *Field);
		}
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAIUAFApplyPlanReport FHyperAIStudioUAFContracts::BuildPlan(const FHyperAIUAFApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::UAF::Private;
	FHyperAIUAFApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	Report.TargetPath = Request.TargetPath;
	Report.Capabilities = GetCapabilities();
	auto Reject = [&](const FString& Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.bStaged = false;
		Report.bExecutionSubmitted = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"), TEXT("UAF graph planning runs on the game thread."));
	}
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"), TEXT("A dry run takes no operation_id or expected_plan_hash."));
	}
	if (!Request.bDryRun && (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId) || !FHyperAIStudioExtensionRuntime::IsValidSubmissionPlanHash(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_submission_fields"), FHyperAIStudioExtensionRuntime::InvalidSubmissionMessage());
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| (Request.bCreate ? !Request.ExpectedRevision.IsEmpty() : !IsCanonicalSha256(Request.ExpectedRevision)))
	{
		return Reject(TEXT("invalid_target_or_revision"), TEXT("Pass a canonical /Game object path, plus the revision from inspect when editing, or none when bCreate."));
	}
	if ((Request.Ops.IsEmpty() && !Request.bCreate) || Request.Ops.Num() > MaxOpsPerPlan)
	{
		return Reject(TEXT("invalid_op_count"), FString::Printf(TEXT("A plan needs up to %d ops, and at least one unless it creates."), MaxOpsPerPlan));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes is outside the closed bounds."));
	}

	const UUAFAnimGraph* Graph = Gate::Resolve(Request.TargetPath, /*bLoad=*/false);
	FString Error;
	if (Request.bCreate)
	{
		if (Graph || FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Request.TargetPath)))
		{
			return Reject(TEXT("target_exists"), TEXT("An asset already exists at target_path; inspect and edit it instead."));
		}
	}
	else
	{
		if (!Graph)
		{
			return Reject(TEXT("target_not_loaded"), TEXT("The animation graph is not loaded; inspect it with bLoad first."));
		}
		Report.BaseRevision = Gate::ComputeRevision(*Graph);
		if (Report.BaseRevision != Request.ExpectedRevision)
		{
			return Reject(TEXT("stale_revision"), TEXT("The animation graph changed after inspection; inspect it again.")
				+ FHyperAIStudioAgentActivityLog::DescribeLastChange(Request.TargetPath));
		}
		if (Gate::IsOpenInEditor(*Graph))
		{
			return Reject(TEXT("target_open_in_editor"), TEXT("Close the graph's editor tab first, so it changes in one place."));
		}
	}
	if (!Gate::ValidateOps(Graph, Request.Ops, Error))
	{
		return Reject(TEXT("invalid_op"), Error);
	}

	const TSharedRef<FHyperAIStudioUAFEditOpsPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioUAFEditOpsPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Request.TargetPath;
	Payload->BaseRevision = Report.BaseRevision;
	Payload->bCreate = Request.bCreate;
	Payload->Ops = Request.Ops;
	Payload->bSave = Request.bSave;
	Payload->SemanticFingerprint = ComputeEditOpsSemanticFingerprint(*Payload);
	if (Payload->GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("payload_bound_exceeded"), TEXT("The ops exceed the bounded request size; split them into smaller plans."));
	}

	FHyperAIStudioTrustedArtifactRequest Trusted;
	Trusted.PackId = PackId;
	Trusted.ToolName = MutationToolName;
	Trusted.VariantId = MutationVariantId;
	Trusted.Safety = EHyperAIStudioDomainSafety::Edit;
	Trusted.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
	Trusted.EffectTarget = Request.TargetPath;
	Trusted.DeadlineMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactDeadlineMs;
	Trusted.MaxNativeOperations = MaxOpsPerPlan + 8;
	Trusted.MaxGameThreadMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactGameThreadMs;
	Trusted.MaxOutputBytes = Request.MaxOutputBytes;
	Trusted.MaxResultBytes = 256;
	Trusted.StageLifetimeMs = StageLifetimeMs;
	Trusted.bCompileOnce = false;
	Trusted.bSaveOnce = Request.bSave;
	Trusted.bValidateOnce = true;
	Trusted.bVerifyFreshOnce = true;

	FHyperAIStudioTrustedPreparedArtifact Prepared;
	FHyperAIStudioTrustedPrepareReport Prepare;
	FString PrepareError;
	const TSharedRef<FHyperAIStudioUAFFreshVerifier, ESPMode::ThreadSafe> Verifier =
		MakeShared<FHyperAIStudioUAFFreshVerifier, ESPMode::ThreadSafe>();
	if (!FHyperAIStudioTrustedExecutionFacade::PrepareDryRun(Trusted, Payload, Verifier, Prepared, Prepare, PrepareError) || !Prepare.bPrepared)
	{
		FString Diagnostic = PrepareError.IsEmpty() ? Prepare.Status.Diagnostic : PrepareError;
		if (!Prepare.Status.BlockingPrerequisiteIds.IsEmpty())
		{
			Diagnostic += TEXT(" Blocking: ") + FString::Join(Prepare.Status.BlockingPrerequisiteIds, TEXT(", "));
		}
		return Reject(Prepare.Status.StatusCode.IsEmpty() ? FString(TEXT("prepare_failed")) : Prepare.Status.StatusCode, Diagnostic);
	}
	Report.bTrustedPrepared = true;
	Report.PlanHash = Payload->SemanticFingerprint;
	Report.AuthorizationPlanHash = Prepare.PlanHash;

	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("planned");
		Report.Diagnostic = TEXT("Nothing changed. To apply, resubmit with bDryRun false, a new operation_id, and expected_plan_hash set to plan_hash.");
		return Report;
	}
	if (!FHyperAIStudioExtensionRuntime::IsAcceptedPlanHash(Request.ExpectedPlanHash, Payload->SemanticFingerprint))
	{
		return Reject(TEXT("plan_hash_mismatch"), TEXT("The plan changed since its dry run (ops or graph state). Dry-run again and review it."));
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = MutationToolName;
		Summary.VariantId = MutationVariantId;
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Request.TargetPath;
		Summary.PlanHash = Payload->SemanticFingerprint;
		if (Request.bCreate) Summary.Effects.Add(TEXT("create the UAF animation graph"));
		for (const FHyperAIUAFOp& Op : Request.Ops)
		{
			TArray<FString> Parts = {Op.Kind, Op.Node, Op.Trait, Op.Name};
			if (!Op.Pin.IsEmpty()) Parts.Add(TEXT("pin ") + Op.Pin);
			if (!Op.To.IsEmpty()) Parts.Add(TEXT("-> ") + Op.To);
			if (!Op.Value.IsEmpty()) Parts.Add(Op.Value);
			Summary.Effects.Add(FString::Join(Parts.FilterByPredicate([](const FString& Part) { return !Part.IsEmpty(); }), TEXT(" ")));
		}
		Summary.Effects.Add(Request.bSave ? TEXT("compile and save") : TEXT("compile"));
		Summary.Touches.Add(Request.TargetPath);
		FString ApprovalError;
		if (!FHyperAIStudioApprovalGate::Request(Prepared, Request.OperationId, Summary, ApprovalError))
		{
			return Reject(TEXT("approval_not_queued"), ApprovalError);
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = TEXT("Waiting for approval in HyperAI Chat, Activity panel. Nothing changed yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true: it starts once approved.");
		return Report;
	}

	// Another agent's edit may own the project lane; then this one waits its turn, and hyper_operation_wait
	// waits through the queue as well as the edit.
	FString QueueStatus;
	FString QueueError;
	if (!FHyperAIStudioExecutionQueue::Submit(Prepared, Request.OperationId, QueueStatus, QueueError))
	{
		return Reject(*QueueStatus, QueueError);
	}
	Report.bStaged = QueueStatus == TEXT("submitted");
	Report.bExecutionSubmitted = Report.bStaged;
	Report.bOk = true;
	Report.Status = QueueStatus;
	Report.Diagnostic = TEXT("The edit and compile run over the next editor ticks. Call hyper_operation_wait with operation_id, then hyper_uaf_validate.");
	return Report;
}

FHyperAIUAFInspectReport UHyperAIStudioUAFToolset::hyper_uaf_inspect(const FHyperAIUAFInspectRequest& Request)
{
	return FHyperAIStudioUAFContracts::Inspect(Request);
}

FHyperAIUAFApplyPlanReport UHyperAIStudioUAFToolset::hyper_uaf_apply_plan(const FHyperAIUAFApplyPlanRequest& InRequest)
{
	FHyperAIUAFApplyPlanRequest Request = InRequest;
	FHyperAIStudioRequestFit Fit;
	Fit.DryRun(Request.bDryRun, Request.OperationId, Request.ExpectedPlanHash);
	FHyperAIUAFApplyPlanReport Report = Fit.FreshOperationId(Request, [](const auto& Planned) { return FHyperAIStudioUAFContracts::BuildPlan(Planned); });
	Report.Diagnostic = Fit.AppendTo(Report.Diagnostic);
	return Report;
}

FHyperAIUAFValidateReport UHyperAIStudioUAFToolset::hyper_uaf_validate(const FHyperAIUAFValidateRequest& Request)
{
	return FHyperAIStudioUAFContracts::Validate(Request);
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioUAFDomainAdapter::GetDescriptor() const
{
	return FHyperAIStudioUAFContracts::GetAdapterDescriptor();
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioUAFDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	using Contracts = FHyperAIStudioUAFContracts;
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = GetDescriptor();
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Result.StatusCode = Status;
		Result.Diagnostic = Diagnostic;
		return Result;
	};
	auto Matches = [&](const TCHAR* Tool, const TCHAR* Variant, const EHyperAIStudioDomainSafety Safety, const TCHAR* TypeId, const FString& Schema)
	{
		return Context.Binding.ToolName == Tool && Context.Binding.VariantId == Variant && Context.Safety == Safety
			&& Payload.GetTypeId() == TypeId && Payload.GetSchemaFingerprint() == Schema;
	};
	auto Succeed = [&](const TSharedRef<IHyperAIStudioDomainResultPayload, ESPMode::ThreadSafe>& Output, const FString& Status, const FString& Diagnostic)
	{
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	};
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty() && Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0 || Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"), TEXT("UAF adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Matches(Contracts::InspectToolName, Contracts::InspectVariantId, EHyperAIStudioDomainSafety::Read, Contracts::InspectPayloadTypeId, Contracts::InspectPayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioUAFInspectResultPayload, ESPMode::ThreadSafe> Output = MakeShared<FHyperAIStudioUAFInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Inspect(static_cast<const FHyperAIStudioUAFInspectPayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::ValidateToolName, Contracts::ValidateVariantId, EHyperAIStudioDomainSafety::Read, Contracts::ValidatePayloadTypeId, Contracts::ValidatePayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioUAFValidateResultPayload, ESPMode::ThreadSafe> Output = MakeShared<FHyperAIStudioUAFValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Validate(static_cast<const FHyperAIStudioUAFValidatePayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::MutationToolName, Contracts::MutationVariantId, EHyperAIStudioDomainSafety::Edit, Contracts::EditOpsPayloadTypeId, Contracts::EditOpsPayloadSchemaFingerprint()))
	{
		const FHyperAIStudioUAFEditOpsPayload& Typed = static_cast<const FHyperAIStudioUAFEditOpsPayload&>(Payload);
		if (Contracts::ComputeEditOpsSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"), TEXT("The sealed edit-ops semantic fingerprint drifted."));
		}
		return HyperAIStudio::UAF::Private::ExecuteEditPhase(Context.ActionKind, Typed);
	}
	return Reject(TEXT("typed_binding_mismatch"), TEXT("UAF adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

FString FHyperAIStudioUAFFreshVerifier::GetOwnerAdapterFingerprint() const
{
	return FHyperAIStudioUAFContracts::GetAdapterDescriptor().AdapterFingerprint;
}

bool FHyperAIStudioUAFFreshVerifier::ResolveCanonicalEffectTarget(
	const IHyperAIStudioTypedArtifactPayload& Request, FString& OutCanonicalEffectTarget, FString& OutError)
{
	using Contracts = FHyperAIStudioUAFContracts;
	OutCanonicalEffectTarget.Reset();
	OutError.Reset();
	if (Request.GetTypeId() != Contracts::EditOpsPayloadTypeId || Request.GetSchemaFingerprint() != Contracts::EditOpsPayloadSchemaFingerprint())
	{
		OutError = TEXT("Fresh verifier received the wrong typed UAF request schema.");
		return false;
	}
	const FHyperAIStudioUAFEditOpsPayload& Typed = static_cast<const FHyperAIStudioUAFEditOpsPayload&>(Request);
	if (!Contracts::IsCanonicalProjectObjectPath(Typed.TargetPath) || Contracts::ComputeEditOpsSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
	{
		OutError = TEXT("Fresh verifier rejected the canonical target or semantic seal.");
		return false;
	}
	OutCanonicalEffectTarget = Typed.TargetPath;
	return true;
}

bool FHyperAIStudioUAFFreshVerifier::VerifyFreshExact(
	const IHyperAIStudioTypedArtifactPayload& Request, const IHyperAIStudioDomainResultPayload& Result, FString& OutPostconditionHash, FString& OutError)
{
	using Contracts = FHyperAIStudioUAFContracts;
	OutPostconditionHash.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Fresh UAF graph verification requires the game thread.");
		return false;
	}
	FString CanonicalTarget;
	if (!ResolveCanonicalEffectTarget(Request, CanonicalTarget, OutError)
		|| Result.GetTypeId() != Contracts::MutationResultTypeId || Result.GetSchemaFingerprint() != Contracts::MutationResultSchemaFingerprint())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Fresh verifier received the wrong result schema.");
		return false;
	}
	const FHyperAIStudioUAFEditOpsPayload& Typed = static_cast<const FHyperAIStudioUAFEditOpsPayload&>(Request);
	const FHyperAIStudioUAFMutationResultPayload& TypedResult = static_cast<const FHyperAIStudioUAFMutationResultPayload&>(Result);
	if (TypedResult.Phase != TEXT("completed") || !Contracts::IsCanonicalSha256(TypedResult.ContentKey))
	{
		OutError = TEXT("Only a completed fresh-capture result may be verified.");
		return false;
	}
	const UUAFAnimGraph* Graph = HyperAIStudio::UAF::Gate::Resolve(CanonicalTarget, /*bLoad=*/false);
	if (!Graph)
	{
		OutError = TEXT("The edited animation graph is no longer loaded.");
		return false;
	}
	const FString ContentKey = HyperAIStudio::UAF::Gate::ComputeRevision(*Graph);
	if (ContentKey != TypedResult.ContentKey)
	{
		OutError = TEXT("The animation graph changed between fresh capture and verification.");
		return false;
	}
	if (Typed.bSave && Graph->GetOutermost()->IsDirty())
	{
		OutError = TEXT("The animation graph still has unsaved changes after the save phase.");
		return false;
	}
	FString Canonical;
	HyperAIStudio::UAF::Private::AppendToken(Canonical, TEXT("hyperai.uaf_graph.edit-ops-postcondition.v1"));
	HyperAIStudio::UAF::Private::AppendToken(Canonical, Typed.SemanticFingerprint);
	HyperAIStudio::UAF::Private::AppendToken(Canonical, ContentKey);
	OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!Contracts::IsCanonicalSha256(OutPostconditionHash))
	{
		OutError = TEXT("Bounded postcondition hashing failed.");
		OutPostconditionHash.Reset();
		return false;
	}
	return true;
}

void FHyperAIStudioUAFRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	// The apply tool tells agents to read this first, so it exists as soon as the tools can.
	if (!HyperAIStudio::UAF::EnsureCookbook())
	{
		UE_LOG(LogHyperAIStudioUAF, Warning, TEXT("Could not write %s."), *HyperAIStudio::UAF::GetCookbookPath());
	}
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioUAFRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioUAFRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioUAFRegistration::IsRegistered() const
{
	return bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(UHyperAIStudioUAFToolset::StaticClass(), FHyperAIStudioUAFContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioUAFRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioUAFRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable() || !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioUAFContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioUAF, Verbose, TEXT("UAF source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	Adapter = MakeShared<FHyperAIStudioUAFDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioUAF, Error, TEXT("UAF adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(AdapterHandle, FHyperAIStudioUAFContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioUAF, Error, TEXT("UAF live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	const auto Observe = []()
	{
		FHyperAIStudioTrustedProbeResult Observation;
		const bool bModules = FModuleManager::Get().IsModuleLoaded(TEXT("UAFAnimGraph"))
			&& FModuleManager::Get().IsModuleLoaded(TEXT("UAFAnimGraphUncookedOnly"));
		Observation.bReady = bModules && GEditor != nullptr;
		Observation.StatusCode = Observation.bReady ? TEXT("ready") : TEXT("uaf_unavailable");
		Observation.Diagnostic = Observation.bReady
			? TEXT("UAF animation graph editing is available.")
			: TEXT("The UAF animation graph modules are not loaded; enable the UAF and UAFAnimGraph plugins.");
		return Observation;
	};
	const FHyperAIStudioTrustedProbeResult First = Observe();
	if (!First.bReady || !ProbePublisher.Start(ProbeHandle, Observe, Error))
	{
		if (Error.IsEmpty()) Error = First.Diagnostic;
		UE_LOG(LogHyperAIStudioUAF, Error, TEXT("UAF live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioUAFToolset::StaticClass(), FHyperAIStudioUAFContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioUAF, Error, TEXT("UAF cohort registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioUAFRegistration::RollBackRegistration()
{
	if (!IsInGameThread()) return;
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioUAFToolset::StaticClass(), FHyperAIStudioUAFContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioUAF, Error, TEXT("UAF owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		bOwnsToolset = false;
	}
	ProbePublisher.Stop();
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
		{
			UE_LOG(LogHyperAIStudioUAF, Error, TEXT("UAF probe rollback failed closed: %s"), *Error);
			return;
		}
		ProbeHandle = {};
	}
	if (AdapterHandle.IsValid())
	{
		FString Error;
		const EHyperAIStudioDomainUnregisterResult Outcome = FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(AdapterHandle, Error);
		if (Outcome != EHyperAIStudioDomainUnregisterResult::Removed && Outcome != EHyperAIStudioDomainUnregisterResult::NotFound)
		{
			UE_LOG(LogHyperAIStudioUAF, Error, TEXT("UAF adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}

#undef LOCTEXT_NAMESPACE
