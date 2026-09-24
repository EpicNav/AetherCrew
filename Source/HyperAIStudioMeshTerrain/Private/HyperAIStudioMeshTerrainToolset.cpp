// Games by Hyper 2026.

#include "HyperAIStudioMeshTerrainToolset.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExecutionQueue.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioMeshTerrainGate.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMeshTerrain, Log, All);

namespace HyperAIStudio::MeshTerrain::Private
{
	namespace Gate = HyperAIStudio::MeshTerrain::Gate;

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

	int64 EstimateTerrainBytes(const FHyperAIMeshTerrain& Terrain)
	{
		return 48 + EstimateBytes(Terrain.Name) + EstimateBytes(Terrain.ActorPath) + EstimateBytes(Terrain.Definition)
			+ EstimateBytes(Terrain.Location) + EstimateBytes(Terrain.Size);
	}

	int64 EstimateModifierBytes(const FHyperAIMeshTerrainModifier& Modifier)
	{
		return 48 + EstimateBytes(Modifier.Name) + EstimateBytes(Modifier.Type) + EstimateBytes(Modifier.Terrain)
			+ EstimateBytes(Modifier.Location) + EstimateBytes(Modifier.Rotation) + EstimateBytes(Modifier.Scale)
			+ EstimateBytes(Modifier.Settings) + EstimateBytes(Modifier.Points);
	}

	int64 EstimateIssueBytes(const FHyperAIMeshTerrainIssue& Issue)
	{
		return 32 + EstimateBytes(Issue.Code) + EstimateBytes(Issue.Severity) + EstimateBytes(Issue.Subject) + EstimateBytes(Issue.Message);
	}

	int32 ClampBytes(const int64 Size)
	{
		return static_cast<int32>(FMath::Clamp<int64>(Size, 1, MAX_int32));
	}

	/** Every terrain and modifier a non-remove op names must exist after the plan ran. */
	bool NamedItemsExist(UWorld& World, const TArray<FHyperAIMeshTerrainOp>& Ops, FString& OutMissing)
	{
		TSet<FString> Remaining;
		for (const FHyperAIMeshTerrainOp& Op : Ops)
		{
			if (Op.Kind.Equals(TEXT("remove_modifier"), ESearchCase::IgnoreCase)) Remaining.Remove(Op.Name.ToLower());
			else Remaining.Add(Op.Name.ToLower());
		}
		TArray<FHyperAIMeshTerrain> Terrains;
		TArray<FHyperAIMeshTerrainModifier> Modifiers;
		Gate::Read(World, Terrains, Modifiers);
		for (const FHyperAIMeshTerrain& Terrain : Terrains) Remaining.Remove(Terrain.Name.ToLower());
		for (const FHyperAIMeshTerrainModifier& Modifier : Modifiers) Remaining.Remove(Modifier.Name.ToLower());
		OutMissing = FString::Join(Remaining.Array(), TEXT(", "));
		return Remaining.IsEmpty();
	}

	/** Watches the terrains rebuild after an edit, so hyper_operation_wait returns when the result can be seen. */
	void WatchRebuild(const FString& LevelPath, const FString& OperationId)
	{
		FHyperAIStudioAsyncJobRequest Job;
		Job.PackId = FHyperAIStudioMeshTerrainContracts::PackId;
		Job.ToolName = FHyperAIStudioMeshTerrainContracts::MutationToolName;
		Job.Target = LevelPath;
		Job.OperationId = OperationId;
		Job.DeadlineMs = FHyperAIStudioMeshTerrainContracts::RebuildDeadlineMs;
		Job.PollIntervalMs = 250;
		FString JobId;
		FString Error;
		// A rebuild can start a tick or two after the edit, so idle counts as done only after one was seen or time passed.
		const TSharedRef<bool> bSawBuild = MakeShared<bool>(false);
		const double Started = FPlatformTime::Seconds();
		FHyperAIStudioAsyncJobHost::Start(Job, [LevelPath, bSawBuild, Started](FString& OutProgress, FString& OutDiagnostic)
		{
			UWorld* World = Gate::GetEditorWorld();
			if (!World || World->GetPathName() != LevelPath)
			{
				OutDiagnostic = TEXT("The level closed while the terrain rebuilt.");
				return EHyperAIStudioAsyncJobPoll::Failed;
			}
			if (Gate::IsBuilding(*World))
			{
				*bSawBuild = true;
				OutProgress = TEXT("rebuilding preview sections");
				return EHyperAIStudioAsyncJobPoll::Running;
			}
			if (!*bSawBuild && FPlatformTime::Seconds() - Started < 1.5)
			{
				OutProgress = TEXT("waiting for the rebuild to start");
				return EHyperAIStudioAsyncJobPoll::Running;
			}
			OutDiagnostic = TEXT("The terrain rebuilt; hyper_mesh_terrain_validate checks it.");
			return EHyperAIStudioAsyncJobPoll::Completed;
		}, JobId, Error);
		if (!Error.IsEmpty())
		{
			UE_LOG(LogHyperAIStudioMeshTerrain, Warning, TEXT("No rebuild watch: %s"), *Error);
		}
	}

	FHyperAIStudioDomainAdapterResult ExecuteEditPhase(
		const FHyperAIStudioDomainDispatchContext& Context,
		const FHyperAIStudioMeshTerrainEditOpsPayload& Payload)
	{
		const EHyperAIStudioDomainExecutionActionKind Phase = Context.ActionKind;
		FHyperAIStudioDomainAdapterResult Result;
		auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status,
			const FString& Diagnostic, const TCHAR* ResultPhase = nullptr, const FString& ContentKey = FString())
		{
			UE_LOG(LogHyperAIStudioMeshTerrain, Log, TEXT("Edit phase %d finished %s. %s"), static_cast<int32>(Phase), *Status, *Diagnostic);
			Result.Outcome = Outcome;
			Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
			Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
			if (Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				const TSharedRef<FHyperAIStudioMeshTerrainMutationResultPayload, ESPMode::ThreadSafe> Output =
					MakeShared<FHyperAIStudioMeshTerrainMutationResultPayload, ESPMode::ThreadSafe>();
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
			return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("Terrain edits run on the game thread."));
		}
		UWorld* World = Gate::GetEditorWorld();
		if (!World || World->GetPathName() != Payload.LevelPath)
		{
			return Finish(FailedOutcome, TEXT("level_changed"), TEXT("A different level is open than the plan was made for; nothing more was changed."));
		}

		switch (Phase)
		{
		case EHyperAIStudioDomainExecutionActionKind::Apply:
		{
			if (GEditor && GEditor->PlayWorld)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("play_in_editor_running"),
					TEXT("Play In Editor started after planning; stop it and apply again."));
			}
			if (Gate::ComputeRevision(*World) != Payload.BaseRevision)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("stale_revision"),
					TEXT("The terrain changed after planning; nothing was applied. Inspect again."));
			}
			int32 Changed = 0;
			FString Error;
			if (!Gate::ApplyOps(*World, Payload.Ops, Changed, Error))
			{
				return Finish(Changed == 0 ? EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect
					: EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("edit_op_failed"), Error);
			}
			WatchRebuild(Payload.LevelPath, Context.OperationId);
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("applied"),
				FString::Printf(TEXT("%d ops applied; the terrain is rebuilding."), Changed), TEXT("applied"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Validate:
		{
			FString Missing;
			if (!NamedItemsExist(*World, Payload.Ops, Missing))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("items_missing"),
					TEXT("These are missing after the edit: ") + Missing + TEXT(". Undo reverts it."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"), TEXT("Every named terrain and modifier is in place."), TEXT("validated"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Save:
			if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages=*/true, /*bSaveContentPackages=*/false))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("save_failed"),
					TEXT("The terrain changed but the level did not save; it remains in the open level."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("saved"), TEXT("Level saved."), TEXT("saved"));
		case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"),
				TEXT("Fresh terrain state captured for verification."), TEXT("completed"), Gate::ComputeRevision(*World));
		default:
			return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Terrain plans have no compile phase."));
		}
	}
}

FString FHyperAIStudioMeshTerrainContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioMeshTerrain.HyperAIStudioMeshTerrainToolset");
}

TArray<FString> FHyperAIStudioMeshTerrainContracts::GetToolNames()
{
	return {InspectToolName, MutationToolName, ValidateToolName};
}

bool FHyperAIStudioMeshTerrainContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

TArray<FString> FHyperAIStudioMeshTerrainContracts::GetCapabilities()
{
	return {
		TEXT("create_flat_terrain_split_into_sections"),
		TEXT("noise_remesh_texture_patch_and_spline_modifiers_as_one_undo_step"),
		TEXT("modifier_settings_by_unreal_text_including_one_level_of_instanced_settings"),
		TEXT("rebuild_watched_to_completion"),
		TEXT("unsupported:sculpting_painting_boolean_lattice_and_heightmap_import")};
}

bool FHyperAIStudioMeshTerrainContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:")))
	{
		return false;
	}
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('a') && Character <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}

FString FHyperAIStudioMeshTerrainContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("mesh_terrain.inspect.payload.v1|editor_world|output_bytes"));
	return Value;
}

FString FHyperAIStudioMeshTerrainContracts::EditOpsPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("mesh_terrain.edit_ops.payload.v1|level_path|base_revision|ordered_closed_ops|save|deep_clone|bounded"));
	return Value;
}

FString FHyperAIStudioMeshTerrainContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("mesh_terrain.validate.payload.v1|editor_world|output_bytes"));
	return Value;
}

FString FHyperAIStudioMeshTerrainContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("mesh_terrain.inspect.result.v1|level|revision|terrains|modifiers|bounded"));
	return Value;
}

FString FHyperAIStudioMeshTerrainContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("mesh_terrain.edit_ops.result.v1|phase|content_key|bounded"));
	return Value;
}

FString FHyperAIStudioMeshTerrainContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("mesh_terrain.validate.result.v1|terrains|issues|building|bounded"));
	return Value;
}

FString FHyperAIStudioMeshTerrainInspectPayload::GetTypeId() const
{
	return FHyperAIStudioMeshTerrainContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioMeshTerrainInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioMeshTerrainContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioMeshTerrainInspectPayload::GetBoundedByteSize() const
{
	return 64;
}

FString FHyperAIStudioMeshTerrainValidatePayload::GetTypeId() const
{
	return FHyperAIStudioMeshTerrainContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioMeshTerrainValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioMeshTerrainContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioMeshTerrainValidatePayload::GetBoundedByteSize() const
{
	return 64;
}

FString FHyperAIStudioMeshTerrainEditOpsPayload::GetTypeId() const
{
	return FHyperAIStudioMeshTerrainContracts::EditOpsPayloadTypeId;
}

FString FHyperAIStudioMeshTerrainEditOpsPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioMeshTerrainContracts::EditOpsPayloadSchemaFingerprint();
}

int32 FHyperAIStudioMeshTerrainEditOpsPayload::GetBoundedByteSize() const
{
	using HyperAIStudio::MeshTerrain::Private::EstimateBytes;
	int64 Size = 160 + EstimateBytes(LevelPath) + EstimateBytes(BaseRevision) + EstimateBytes(SemanticFingerprint);
	for (const FHyperAIMeshTerrainOp& Op : Ops)
	{
		Size += 40 + EstimateBytes(Op.Kind) + EstimateBytes(Op.Name) + EstimateBytes(Op.Terrain) + EstimateBytes(Op.Type)
			+ EstimateBytes(Op.Location) + EstimateBytes(Op.Rotation) + EstimateBytes(Op.Scale) + EstimateBytes(Op.Size)
			+ EstimateBytes(Op.Properties) + EstimateBytes(Op.Points);
	}
	return HyperAIStudio::MeshTerrain::Private::ClampBytes(Size);
}

FString FHyperAIStudioMeshTerrainEditOpsPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> FHyperAIStudioMeshTerrainEditOpsPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioMeshTerrainEditOpsPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioMeshTerrainEditOpsPayload, ESPMode::ThreadSafe>();
	Clone->LevelPath = LevelPath;
	Clone->BaseRevision = BaseRevision;
	Clone->Ops = Ops;
	Clone->bSave = bSave;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioMeshTerrainInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioMeshTerrainContracts::InspectResultTypeId;
}

FString FHyperAIStudioMeshTerrainInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioMeshTerrainContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioMeshTerrainInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::MeshTerrain::Private;
	int64 Size = 512 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.LevelPath) + EstimateBytes(Report.Revision);
	for (const FHyperAIMeshTerrain& Terrain : Report.Terrains) Size += EstimateTerrainBytes(Terrain);
	for (const FHyperAIMeshTerrainModifier& Modifier : Report.Modifiers) Size += EstimateModifierBytes(Modifier);
	return ClampBytes(Size);
}

FString FHyperAIStudioMeshTerrainValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioMeshTerrainContracts::ValidateResultTypeId;
}

FString FHyperAIStudioMeshTerrainValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioMeshTerrainContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioMeshTerrainValidateResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::MeshTerrain::Private;
	int64 Size = 512 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.LevelPath);
	for (const FHyperAIMeshTerrainIssue& Issue : Report.Issues) Size += EstimateIssueBytes(Issue);
	return ClampBytes(Size);
}

FString FHyperAIStudioMeshTerrainMutationResultPayload::GetTypeId() const
{
	return FHyperAIStudioMeshTerrainContracts::MutationResultTypeId;
}

FString FHyperAIStudioMeshTerrainMutationResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioMeshTerrainContracts::MutationResultSchemaFingerprint();
}

int32 FHyperAIStudioMeshTerrainMutationResultPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::MeshTerrain::Private::ClampBytes(96 + 2ll * (Phase.Len() + ContentKey.Len()));
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioMeshTerrainContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.mesh_terrain.mesh_partition.ue58");
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

FHyperAIMeshTerrainInspectReport FHyperAIStudioMeshTerrainContracts::Inspect(const FHyperAIMeshTerrainInspectRequest& Request)
{
	using namespace HyperAIStudio::MeshTerrain::Private;
	FHyperAIMeshTerrainInspectReport Report;
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
		return Reject(TEXT("game_thread_required"), TEXT("Terrain inspection runs on the game thread."));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes must be 16-128 KB."));
	}
	UWorld* World = Gate::GetEditorWorld();
	if (!World)
	{
		return Reject(TEXT("no_editor_world"), TEXT("No level is open in the editor."));
	}
	Report.LevelPath = World->GetPathName();
	Report.Revision = Gate::ComputeRevision(*World);
	TArray<FHyperAIMeshTerrain> Terrains;
	TArray<FHyperAIMeshTerrainModifier> Modifiers;
	Gate::Read(*World, Terrains, Modifiers);
	int64 Budget = 2048;
	for (FHyperAIMeshTerrain& Terrain : Terrains)
	{
		Budget += EstimateTerrainBytes(Terrain);
		Report.Terrains.Add(MoveTemp(Terrain));
	}
	for (FHyperAIMeshTerrainModifier& Modifier : Modifiers)
	{
		const int64 Bytes = EstimateModifierBytes(Modifier);
		if (Budget + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		Budget += Bytes;
		Report.Modifiers.Add(MoveTemp(Modifier));
	}
	Report.bOk = true;
	Report.Status = Report.bTruncated ? TEXT("truncated") : TEXT("captured");
	Report.Diagnostic = Report.Terrains.IsEmpty()
		? FString(TEXT("The level has no Mesh Terrain; apply_plan's create_terrain makes one."))
		: FString::Printf(TEXT("%d terrains, %d modifiers."), Report.Terrains.Num(), Modifiers.Num());
	return Report;
}

FHyperAIMeshTerrainValidateReport FHyperAIStudioMeshTerrainContracts::Validate(const FHyperAIMeshTerrainValidateRequest& Request)
{
	using namespace HyperAIStudio::MeshTerrain::Private;
	FHyperAIMeshTerrainValidateReport Report;
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
		return Reject(TEXT("game_thread_required"), TEXT("Terrain validation runs on the game thread."));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes must be 16-128 KB."));
	}
	UWorld* World = Gate::GetEditorWorld();
	if (!World)
	{
		return Reject(TEXT("no_editor_world"), TEXT("No level is open in the editor."));
	}
	Report.LevelPath = World->GetPathName();
	Gate::Check(*World, Report);
	int64 Budget = 2048;
	int32 Kept = 0;
	for (; Kept < Report.Issues.Num(); ++Kept)
	{
		Budget += EstimateIssueBytes(Report.Issues[Kept]);
		if (Budget > Request.MaxOutputBytes) break;
	}
	if (Kept < Report.Issues.Num())
	{
		Report.Issues.SetNum(Kept);
		Report.bTruncated = true;
	}
	Report.bOk = true;
	Report.Status = Report.TerrainCount == 0 ? TEXT("no_terrain") : Report.ErrorCount > 0 ? TEXT("problems") : TEXT("valid");
	Report.Diagnostic = FString::Printf(TEXT("%d terrains: %d errors, %d warnings%s."), Report.TerrainCount, Report.ErrorCount,
		Report.WarningCount, Report.bBuilding ? TEXT(", still rebuilding") : TEXT(""));
	return Report;
}

FString FHyperAIStudioMeshTerrainContracts::ComputeEditOpsSemanticFingerprint(const FHyperAIStudioMeshTerrainEditOpsPayload& Payload)
{
	using HyperAIStudio::MeshTerrain::Private::AppendToken;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.mesh_terrain.edit-ops-intent.v1"));
	AppendToken(Canonical, PackId);
	AppendToken(Canonical, MutationToolName);
	AppendToken(Canonical, MutationVariantId);
	AppendToken(Canonical, EditOpsPayloadSchemaFingerprint());
	AppendToken(Canonical, GetAdapterDescriptor().AdapterFingerprint);
	AppendToken(Canonical, Payload.LevelPath);
	AppendToken(Canonical, Payload.BaseRevision);
	AppendToken(Canonical, Payload.bSave ? TEXT("save") : TEXT("no_save"));
	AppendToken(Canonical, FString::FromInt(Payload.Ops.Num()));
	for (const FHyperAIMeshTerrainOp& Op : Payload.Ops)
	{
		AppendToken(Canonical, Op.Kind);
		AppendToken(Canonical, Op.Name);
		AppendToken(Canonical, Op.Terrain);
		AppendToken(Canonical, Op.Type);
		AppendToken(Canonical, Op.Location);
		AppendToken(Canonical, Op.Rotation);
		AppendToken(Canonical, Op.Scale);
		AppendToken(Canonical, Op.Size);
		AppendToken(Canonical, FString::Printf(TEXT("%.3f"), Op.Spacing));
		AppendToken(Canonical, Op.Properties);
		AppendToken(Canonical, Op.Points);
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAIMeshTerrainApplyPlanReport FHyperAIStudioMeshTerrainContracts::BuildPlan(const FHyperAIMeshTerrainApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::MeshTerrain::Private;
	FHyperAIMeshTerrainApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
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
		return Reject(TEXT("game_thread_required"), TEXT("Terrain planning runs on the game thread."));
	}
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"), TEXT("A dry run takes no operation_id or expected_plan_hash."));
	}
	if (!Request.bDryRun && (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId)
		|| !FHyperAIStudioExtensionRuntime::IsValidSubmissionPlanHash(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_submission_fields"), FHyperAIStudioExtensionRuntime::InvalidSubmissionMessage());
	}
	if (!IsCanonicalSha256(Request.ExpectedRevision))
	{
		return Reject(TEXT("invalid_revision"), TEXT("Pass expected_revision from hyper_mesh_terrain_inspect."));
	}
	if (Request.Ops.IsEmpty() || Request.Ops.Num() > MaxOpsPerPlan)
	{
		return Reject(TEXT("invalid_op_count"), FString::Printf(TEXT("A plan needs 1 to %d ops."), MaxOpsPerPlan));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes must be 16-128 KB."));
	}
	UWorld* World = Gate::GetEditorWorld();
	if (!World)
	{
		return Reject(TEXT("no_editor_world"), TEXT("No level is open in the editor."));
	}
	if (GEditor && GEditor->PlayWorld)
	{
		return Reject(TEXT("play_in_editor_running"), TEXT("Stop Play In Editor first: edits made now would not reach the running game."));
	}
	Report.LevelPath = World->GetPathName();
	Report.BaseRevision = Gate::ComputeRevision(*World);
	if (Report.BaseRevision != Request.ExpectedRevision)
	{
		return Reject(TEXT("stale_revision"), TEXT("The terrain changed after inspection; inspect again.")
			+ FHyperAIStudioAgentActivityLog::DescribeLastChange(Report.LevelPath));
	}
	if (Request.bSave && World->GetOutermost()->GetName().StartsWith(TEXT("/Temp/")))
	{
		return Reject(TEXT("level_never_saved"), TEXT("This level has never been saved, so there is nowhere to save the terrain; save the level first or leave bSave off."));
	}
	FString Error;
	if (!Gate::ValidateOps(*World, Request.Ops, Report.Notes, Error))
	{
		return Reject(TEXT("invalid_op"), Error);
	}

	const TSharedRef<FHyperAIStudioMeshTerrainEditOpsPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioMeshTerrainEditOpsPayload, ESPMode::ThreadSafe>();
	Payload->LevelPath = Report.LevelPath;
	Payload->BaseRevision = Report.BaseRevision;
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
	Trusted.EffectTarget = Report.LevelPath;
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
	const TSharedRef<FHyperAIStudioMeshTerrainFreshVerifier, ESPMode::ThreadSafe> Verifier =
		MakeShared<FHyperAIStudioMeshTerrainFreshVerifier, ESPMode::ThreadSafe>();
	if (!FHyperAIStudioTrustedExecutionFacade::PrepareDryRun(Trusted, Payload, Verifier, Prepared, Prepare, PrepareError)
		|| !Prepare.bPrepared)
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
		return Reject(TEXT("plan_hash_mismatch"), TEXT("The plan changed since its dry run (ops or terrain). Dry-run again and review it."));
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = MutationToolName;
		Summary.VariantId = MutationVariantId;
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Report.LevelPath;
		Summary.PlanHash = Payload->SemanticFingerprint;
		for (const FHyperAIMeshTerrainOp& Op : Request.Ops)
		{
			Summary.Effects.Add(FString::Printf(TEXT("%s %s%s%s"), *Op.Kind, *Op.Name,
				Op.Type.IsEmpty() ? TEXT("") : *(TEXT(" (") + Op.Type + TEXT(")")),
				Op.Location.IsEmpty() ? TEXT("") : *(TEXT(" at ") + Op.Location)));
		}
		if (Request.bSave)
		{
			Summary.Effects.Add(TEXT("save the level, including any other unsaved changes in it"));
		}
		Summary.Touches.Add(Report.LevelPath);
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

	// Another agent's edit may own the project lane; then this one waits its turn.
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
	Report.Diagnostic = TEXT("The edit runs over the next editor ticks. Call hyper_operation_wait with operation_id: it returns when the terrain has rebuilt. Then hyper_mesh_terrain_validate.");
	return Report;
}

FHyperAIMeshTerrainInspectReport UHyperAIStudioMeshTerrainToolset::hyper_mesh_terrain_inspect(const FHyperAIMeshTerrainInspectRequest& Request)
{
	return FHyperAIStudioMeshTerrainContracts::Inspect(Request);
}

FHyperAIMeshTerrainApplyPlanReport UHyperAIStudioMeshTerrainToolset::hyper_mesh_terrain_apply_plan(const FHyperAIMeshTerrainApplyPlanRequest& Request)
{
	return FHyperAIStudioMeshTerrainContracts::BuildPlan(Request);
}

FHyperAIMeshTerrainValidateReport UHyperAIStudioMeshTerrainToolset::hyper_mesh_terrain_validate(const FHyperAIMeshTerrainValidateRequest& Request)
{
	return FHyperAIStudioMeshTerrainContracts::Validate(Request);
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioMeshTerrainDomainAdapter::GetDescriptor() const
{
	return FHyperAIStudioMeshTerrainContracts::GetAdapterDescriptor();
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioMeshTerrainDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	using Contracts = FHyperAIStudioMeshTerrainContracts;
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = GetDescriptor();
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Result.StatusCode = Status;
		Result.Diagnostic = Diagnostic;
		return Result;
	};
	auto Matches = [&](const TCHAR* Tool, const TCHAR* Variant, const EHyperAIStudioDomainSafety Safety,
		const TCHAR* TypeId, const FString& Schema)
	{
		return Context.Binding.ToolName == Tool && Context.Binding.VariantId == Variant && Context.Safety == Safety
			&& Payload.GetTypeId() == TypeId && Payload.GetSchemaFingerprint() == Schema;
	};
	auto Succeed = [&](const TSharedRef<IHyperAIStudioDomainResultPayload, ESPMode::ThreadSafe>& Output,
		const FString& Status, const FString& Diagnostic)
	{
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	};
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty()
			&& Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0
		|| Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"), TEXT("Mesh terrain adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Matches(Contracts::InspectToolName, Contracts::InspectVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::InspectPayloadTypeId, Contracts::InspectPayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioMeshTerrainInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioMeshTerrainInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Inspect(static_cast<const FHyperAIStudioMeshTerrainInspectPayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::ValidateToolName, Contracts::ValidateVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::ValidatePayloadTypeId, Contracts::ValidatePayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioMeshTerrainValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioMeshTerrainValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Validate(static_cast<const FHyperAIStudioMeshTerrainValidatePayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::MutationToolName, Contracts::MutationVariantId, EHyperAIStudioDomainSafety::Edit,
		Contracts::EditOpsPayloadTypeId, Contracts::EditOpsPayloadSchemaFingerprint()))
	{
		const FHyperAIStudioMeshTerrainEditOpsPayload& Typed = static_cast<const FHyperAIStudioMeshTerrainEditOpsPayload&>(Payload);
		if (Contracts::ComputeEditOpsSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"), TEXT("The sealed edit-ops semantic fingerprint drifted."));
		}
		return HyperAIStudio::MeshTerrain::Private::ExecuteEditPhase(Context, Typed);
	}
	return Reject(TEXT("typed_binding_mismatch"), TEXT("Mesh terrain adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

FString FHyperAIStudioMeshTerrainFreshVerifier::GetOwnerAdapterFingerprint() const
{
	return FHyperAIStudioMeshTerrainContracts::GetAdapterDescriptor().AdapterFingerprint;
}

bool FHyperAIStudioMeshTerrainFreshVerifier::ResolveCanonicalEffectTarget(
	const IHyperAIStudioTypedArtifactPayload& Request,
	FString& OutCanonicalEffectTarget,
	FString& OutError)
{
	using Contracts = FHyperAIStudioMeshTerrainContracts;
	OutCanonicalEffectTarget.Reset();
	OutError.Reset();
	if (Request.GetTypeId() != Contracts::EditOpsPayloadTypeId
		|| Request.GetSchemaFingerprint() != Contracts::EditOpsPayloadSchemaFingerprint())
	{
		OutError = TEXT("Fresh verifier received the wrong typed terrain request schema.");
		return false;
	}
	const FHyperAIStudioMeshTerrainEditOpsPayload& Typed = static_cast<const FHyperAIStudioMeshTerrainEditOpsPayload&>(Request);
	if (Typed.LevelPath.IsEmpty() || Contracts::ComputeEditOpsSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
	{
		OutError = TEXT("Fresh verifier rejected the level path or semantic seal.");
		return false;
	}
	OutCanonicalEffectTarget = Typed.LevelPath;
	return true;
}

bool FHyperAIStudioMeshTerrainFreshVerifier::VerifyFreshExact(
	const IHyperAIStudioTypedArtifactPayload& Request,
	const IHyperAIStudioDomainResultPayload& Result,
	FString& OutPostconditionHash,
	FString& OutError)
{
	namespace Gate = HyperAIStudio::MeshTerrain::Gate;
	using Contracts = FHyperAIStudioMeshTerrainContracts;
	OutPostconditionHash.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Fresh terrain verification requires the game thread.");
		return false;
	}
	FString CanonicalTarget;
	if (!ResolveCanonicalEffectTarget(Request, CanonicalTarget, OutError)
		|| Result.GetTypeId() != Contracts::MutationResultTypeId
		|| Result.GetSchemaFingerprint() != Contracts::MutationResultSchemaFingerprint())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Fresh verifier received the wrong result schema.");
		return false;
	}
	const FHyperAIStudioMeshTerrainEditOpsPayload& Typed = static_cast<const FHyperAIStudioMeshTerrainEditOpsPayload&>(Request);
	const FHyperAIStudioMeshTerrainMutationResultPayload& TypedResult = static_cast<const FHyperAIStudioMeshTerrainMutationResultPayload&>(Result);
	if (TypedResult.Phase != TEXT("completed") || !Contracts::IsCanonicalSha256(TypedResult.ContentKey))
	{
		OutError = TEXT("Only a completed fresh-capture result may be verified.");
		return false;
	}
	UWorld* World = Gate::GetEditorWorld();
	if (!World || World->GetPathName() != CanonicalTarget)
	{
		OutError = TEXT("The edited level is no longer open.");
		return false;
	}
	const FString ContentKey = Gate::ComputeRevision(*World);
	if (ContentKey != TypedResult.ContentKey)
	{
		OutError = TEXT("The terrain changed between fresh capture and verification.");
		return false;
	}
	if (Typed.bSave && World->GetOutermost()->IsDirty())
	{
		OutError = TEXT("The level still has unsaved changes after the save phase.");
		return false;
	}
	FString Canonical;
	HyperAIStudio::MeshTerrain::Private::AppendToken(Canonical, TEXT("hyperai.mesh_terrain.edit-ops-postcondition.v1"));
	HyperAIStudio::MeshTerrain::Private::AppendToken(Canonical, Typed.SemanticFingerprint);
	HyperAIStudio::MeshTerrain::Private::AppendToken(Canonical, ContentKey);
	OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!Contracts::IsCanonicalSha256(OutPostconditionHash))
	{
		OutError = TEXT("Bounded postcondition hashing failed.");
		OutPostconditionHash.Reset();
		return false;
	}
	return true;
}

void FHyperAIStudioMeshTerrainRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioMeshTerrainRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioMeshTerrainRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioMeshTerrainRegistration::IsRegistered() const
{
	return bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioMeshTerrainToolset::StaticClass(), FHyperAIStudioMeshTerrainContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioMeshTerrainRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioMeshTerrainRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioMeshTerrainContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioMeshTerrain, Verbose, TEXT("Mesh terrain source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	Adapter = MakeShared<FHyperAIStudioMeshTerrainDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioMeshTerrain, Error, TEXT("Mesh terrain adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(AdapterHandle, FHyperAIStudioMeshTerrainContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioMeshTerrain, Error, TEXT("Mesh terrain live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	const auto Observe = []()
	{
		FHyperAIStudioTrustedProbeResult Observation;
		const bool bModules = FModuleManager::Get().IsModuleLoaded(TEXT("MeshPartition"))
			&& FModuleManager::Get().IsModuleLoaded(TEXT("MeshPartitionEditor"));
		Observation.bReady = bModules && HyperAIStudio::MeshTerrain::Gate::GetEditorWorld() != nullptr;
		Observation.StatusCode = !bModules ? TEXT("mesh_partition_not_loaded") : Observation.bReady ? TEXT("ready") : TEXT("no_editor_world");
		Observation.Diagnostic = !bModules ? TEXT("The Mesh Partition plugin is not loaded.")
			: Observation.bReady ? TEXT("Mesh Partition is loaded and a level is open.") : TEXT("No level is open in the editor.");
		return Observation;
	};
	const FHyperAIStudioTrustedProbeResult First = Observe();
	if (!First.bReady || !ProbePublisher.Start(ProbeHandle, Observe, Error))
	{
		if (Error.IsEmpty()) Error = First.Diagnostic;
		UE_LOG(LogHyperAIStudioMeshTerrain, Error, TEXT("Mesh terrain live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioMeshTerrainToolset::StaticClass(), FHyperAIStudioMeshTerrainContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioMeshTerrain, Error, TEXT("Mesh terrain cohort registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioMeshTerrainRegistration::RollBackRegistration()
{
	if (!IsInGameThread())
	{
		return;
	}
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioMeshTerrainToolset::StaticClass(), FHyperAIStudioMeshTerrainContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioMeshTerrain, Error, TEXT("Mesh terrain owned-toolset rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioMeshTerrain, Error, TEXT("Mesh terrain probe rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioMeshTerrain, Error, TEXT("Mesh terrain adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}
