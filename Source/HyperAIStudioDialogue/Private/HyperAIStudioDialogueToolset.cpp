// Games by Hyper 2026.

#include "HyperAIStudioDialogueToolset.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "FileHelpers.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioDialogueCookbook.h"
#include "HyperAIStudioDialogueGate.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioSettings.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDialogueToolset, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioDialogue"

namespace HyperAIStudio::Dialogue::Private
{
	namespace Gate = HyperAIStudio::Dialogue::Gate;

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

	int64 EstimateIssueBytes(const FHyperAIDialogueIssue& Issue)
	{
		return 32 + EstimateBytes(Issue.Code) + EstimateBytes(Issue.Severity) + EstimateBytes(Issue.Subject) + EstimateBytes(Issue.Message);
	}

	int32 ClampBytes(const int64 Size)
	{
		return static_cast<int32>(FMath::Clamp<int64>(Size, 1, MAX_int32));
	}

	/** What the project holds for this import now, so the executor can prove the effect landed. */
	FString ComputeImportContentKey(const FHyperAIStudioDialogueImportPayload& Payload)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.dialogue.import-content.v1"));
		AppendToken(Canonical, Payload.RenderId);
		AppendToken(Canonical, Payload.TargetFolder);
		for (const FString& Path : Gate::PlanImportPaths(Payload.RenderId, Payload.TargetFolder, Payload.bCreateDialogueWaves))
		{
			AppendToken(Canonical, Path);
			AppendToken(Canonical, FindObject<UObject>(nullptr, *Path) ? TEXT("present") : TEXT("absent"));
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FHyperAIStudioDomainAdapterResult ExecuteImportPhase(
		const EHyperAIStudioDomainExecutionActionKind Phase,
		const FHyperAIStudioDialogueImportPayload& Payload)
	{
		FHyperAIStudioDomainAdapterResult Result;
		const double Started = FPlatformTime::Seconds();
		auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status,
			const FString& Diagnostic, const TCHAR* ResultPhase = nullptr, const FString& ContentKey = FString())
		{
			UE_LOG(LogHyperAIStudioDialogueToolset, Log, TEXT("Import phase %d finished %s in %.1f ms. %s"),
				static_cast<int32>(Phase), *Status, (FPlatformTime::Seconds() - Started) * 1000.0, *Diagnostic);
			Result.Outcome = Outcome;
			Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
			Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
			if (Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				const TSharedRef<FHyperAIStudioDialogueMutationResultPayload, ESPMode::ThreadSafe> Output =
					MakeShared<FHyperAIStudioDialogueMutationResultPayload, ESPMode::ThreadSafe>();
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
			return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("Dialogue imports run on the game thread."));
		}

		if (bApply)
		{
			FHyperAIDialogueRenderRecord Record;
			if (!Gate::FindRender(Payload.RenderId, Record))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("render_not_found"),
					FString::Printf(TEXT("No render called %s; it may have been from an earlier editor session."), *Payload.RenderId));
			}
			if (Record.State != TEXT("complete"))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("render_not_complete"),
					FString::Printf(TEXT("That render is %s: %s."), *Record.State, *Record.Progress));
			}
			if (Record.RenderKey != Payload.RenderKey)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("stale_render"),
					TEXT("The rendered audio changed after planning; inspect the render again."));
			}
			FString Error;
			TArray<UObject*> Created;
			{
				const FScopedTransaction Transaction(LOCTEXT("ImportDialogue", "HyperAI: Import Dialogue"));
				if (!Gate::ImportRender(Payload.RenderId, Payload.TargetFolder, Payload.bCreateDialogueWaves, Created, Error))
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("import_failed"), Error);
				}
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("imported"),
				FString::Printf(TEXT("%d assets created in %s."), Created.Num(), *Payload.TargetFolder), TEXT("applied"));
		}

		switch (Phase)
		{
		case EHyperAIStudioDomainExecutionActionKind::Validate:
			for (const FString& Path : Gate::PlanImportPaths(Payload.RenderId, Payload.TargetFolder, Payload.bCreateDialogueWaves))
			{
				if (!FindObject<UObject>(nullptr, *Path))
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("asset_missing"),
						FString::Printf(TEXT("%s was not created; undo reverts the rest."), *Path));
				}
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"),
				TEXT("Every planned asset is there."), TEXT("validated"));
		case EHyperAIStudioDomainExecutionActionKind::Save:
		{
			TArray<UObject*> Imported;
			for (const FString& Path : Gate::PlanImportPaths(Payload.RenderId, Payload.TargetFolder, Payload.bCreateDialogueWaves))
			{
				if (UObject* Object = FindObject<UObject>(nullptr, *Path))
				{
					Imported.Add(Object);
				}
			}
			if (!UEditorLoadingAndSavingUtils::SavePackages(Gate::PackagesOf(Imported), /*bOnlyDirty=*/false))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("save_failed"),
					TEXT("The assets were created but did not save; they remain in memory."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("saved"), TEXT("Packages saved."), TEXT("saved"));
		}
		case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"),
				TEXT("Fresh assets captured for verification."), TEXT("completed"), ComputeImportContentKey(Payload));
		default:
			return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Unknown execution phase."));
		}
	}
}

FString FHyperAIStudioDialogueContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioDialogue.HyperAIStudioDialogueToolset");
}

TArray<FString> FHyperAIStudioDialogueContracts::GetToolNames()
{
	return {InspectToolName, ValidateToolName, RenderToolName, MutationToolName};
}

bool FHyperAIStudioDialogueContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

TArray<FString> FHyperAIStudioDialogueContracts::GetCapabilities()
{
	return {
		TEXT("render_lines_to_speech_on_the_configured_service"),
		TEXT("voice_assets_holding_a_reference_clip_description_and_seed"),
		TEXT("directions_performed_rather_than_spoken"),
		TEXT("one_wav_per_line_so_every_character_stays_separate"),
		TEXT("import_as_sound_waves_and_dialogue_waves_as_one_undo_step"),
		TEXT("checks_lines_before_spending_a_render"),
		TEXT("unsupported:writing_the_dialogue_itself_non_english_voices_and_decibel_sessions")};
}

bool FHyperAIStudioDialogueContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT("*")) || Path.Contains(TEXT("?")))
	{
		return false;
	}
	FText Reason;
	return FPackageName::IsValidObjectPath(Path, &Reason);
}

bool FHyperAIStudioDialogueContracts::IsCanonicalProjectFolder(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.EndsWith(TEXT("/")) || Path.Contains(TEXT("//")) || Path.Contains(TEXT("*"))
		|| Path.Contains(TEXT("?")) || Path.Contains(TEXT(".")))
	{
		return false;
	}
	for (const TCHAR Character : Path)
	{
		if (Character < 0x20 || Character == 0x7F) return false;
	}
	return true;
}

bool FHyperAIStudioDialogueContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"))) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

FString FHyperAIStudioDialogueContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("dialogue.inspect.payload.v1|output_bytes"));
	return Value;
}

FString FHyperAIStudioDialogueContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("dialogue.validate.payload.v1|lines|output_bytes"));
	return Value;
}

FString FHyperAIStudioDialogueContracts::RenderPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("dialogue.render.payload.v1|scene|lines|output_bytes"));
	return Value;
}

FString FHyperAIStudioDialogueContracts::ImportPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("dialogue.import.payload.v1|render_id|render_key|target_folder|dialogue_waves|save|deep_clone|bounded"));
	return Value;
}

FString FHyperAIStudioDialogueContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("dialogue.inspect.result.v1|service_health|voices|clips|renders|bounded"));
	return Value;
}

FString FHyperAIStudioDialogueContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("dialogue.validate.result.v1|issues|counts|estimate|sample_prompt|bounded"));
	return Value;
}

FString FHyperAIStudioDialogueContracts::RenderResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("dialogue.render.result.v1|render_id|folder|line_count|estimate|bounded"));
	return Value;
}

FString FHyperAIStudioDialogueContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("dialogue.import.result.v1|phase|content_key|bounded"));
	return Value;
}

FString FHyperAIStudioDialogueInspectPayload::GetTypeId() const { return FHyperAIStudioDialogueContracts::InspectPayloadTypeId; }
FString FHyperAIStudioDialogueInspectPayload::GetSchemaFingerprint() const { return FHyperAIStudioDialogueContracts::InspectPayloadSchemaFingerprint(); }
int32 FHyperAIStudioDialogueInspectPayload::GetBoundedByteSize() const { return 64; }

FString FHyperAIStudioDialogueValidatePayload::GetTypeId() const { return FHyperAIStudioDialogueContracts::ValidatePayloadTypeId; }
FString FHyperAIStudioDialogueValidatePayload::GetSchemaFingerprint() const { return FHyperAIStudioDialogueContracts::ValidatePayloadSchemaFingerprint(); }
int32 FHyperAIStudioDialogueValidatePayload::GetBoundedByteSize() const
{
	using HyperAIStudio::Dialogue::Private::EstimateBytes;
	int64 Size = 64;
	for (const FHyperAIDialogueLine& Line : Request.Lines)
	{
		Size += 32 + EstimateBytes(Line.Voice) + EstimateBytes(Line.Text) + EstimateBytes(Line.Direction) + EstimateBytes(Line.Name);
	}
	return HyperAIStudio::Dialogue::Private::ClampBytes(Size);
}

FString FHyperAIStudioDialogueRenderPayload::GetTypeId() const { return FHyperAIStudioDialogueContracts::RenderPayloadTypeId; }
FString FHyperAIStudioDialogueRenderPayload::GetSchemaFingerprint() const { return FHyperAIStudioDialogueContracts::RenderPayloadSchemaFingerprint(); }
int32 FHyperAIStudioDialogueRenderPayload::GetBoundedByteSize() const
{
	using HyperAIStudio::Dialogue::Private::EstimateBytes;
	int64 Size = 64 + EstimateBytes(Request.SceneName);
	for (const FHyperAIDialogueLine& Line : Request.Lines)
	{
		Size += 32 + EstimateBytes(Line.Voice) + EstimateBytes(Line.Text) + EstimateBytes(Line.Direction) + EstimateBytes(Line.Name);
	}
	return HyperAIStudio::Dialogue::Private::ClampBytes(Size);
}

FString FHyperAIStudioDialogueImportPayload::GetTypeId() const { return FHyperAIStudioDialogueContracts::ImportPayloadTypeId; }
FString FHyperAIStudioDialogueImportPayload::GetSchemaFingerprint() const { return FHyperAIStudioDialogueContracts::ImportPayloadSchemaFingerprint(); }
int32 FHyperAIStudioDialogueImportPayload::GetBoundedByteSize() const
{
	using HyperAIStudio::Dialogue::Private::EstimateBytes;
	return HyperAIStudio::Dialogue::Private::ClampBytes(
		128 + EstimateBytes(RenderId) + EstimateBytes(RenderKey) + EstimateBytes(TargetFolder) + EstimateBytes(SemanticFingerprint));
}
FString FHyperAIStudioDialogueImportPayload::GetSemanticFingerprint() const { return SemanticFingerprint; }
TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> FHyperAIStudioDialogueImportPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioDialogueImportPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioDialogueImportPayload, ESPMode::ThreadSafe>();
	Clone->RenderId = RenderId;
	Clone->RenderKey = RenderKey;
	Clone->TargetFolder = TargetFolder;
	Clone->bCreateDialogueWaves = bCreateDialogueWaves;
	Clone->bSave = bSave;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioDialogueInspectResultPayload::GetTypeId() const { return FHyperAIStudioDialogueContracts::InspectResultTypeId; }
FString FHyperAIStudioDialogueInspectResultPayload::GetSchemaFingerprint() const { return FHyperAIStudioDialogueContracts::InspectResultSchemaFingerprint(); }
int32 FHyperAIStudioDialogueInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Dialogue::Private;
	int64 Size = 512 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.ServiceDiagnostic);
	for (const FHyperAIDialogueVoiceRecord& Voice : Report.Voices)
	{
		Size += 48 + EstimateBytes(Voice.Path) + EstimateBytes(Voice.ReferenceClip) + EstimateBytes(Voice.SpeakerDescription);
	}
	for (const FString& Clip : Report.AvailableClips) Size += EstimateBytes(Clip);
	for (const FHyperAIDialogueRenderRecord& Render : Report.Renders)
	{
		Size += 64 + EstimateBytes(Render.RenderId) + EstimateBytes(Render.Progress) + EstimateBytes(Render.Folder) + EstimateBytes(Render.RenderKey);
	}
	return ClampBytes(Size);
}

FString FHyperAIStudioDialogueValidateResultPayload::GetTypeId() const { return FHyperAIStudioDialogueContracts::ValidateResultTypeId; }
FString FHyperAIStudioDialogueValidateResultPayload::GetSchemaFingerprint() const { return FHyperAIStudioDialogueContracts::ValidateResultSchemaFingerprint(); }
int32 FHyperAIStudioDialogueValidateResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Dialogue::Private;
	int64 Size = 256 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.SamplePrompt);
	for (const FHyperAIDialogueIssue& Issue : Report.Issues) Size += EstimateIssueBytes(Issue);
	return ClampBytes(Size);
}

FString FHyperAIStudioDialogueRenderResultPayload::GetTypeId() const { return FHyperAIStudioDialogueContracts::RenderResultTypeId; }
FString FHyperAIStudioDialogueRenderResultPayload::GetSchemaFingerprint() const { return FHyperAIStudioDialogueContracts::RenderResultSchemaFingerprint(); }
int32 FHyperAIStudioDialogueRenderResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Dialogue::Private;
	return ClampBytes(256 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic)
		+ EstimateBytes(Report.RenderId) + EstimateBytes(Report.Folder));
}

FString FHyperAIStudioDialogueMutationResultPayload::GetTypeId() const { return FHyperAIStudioDialogueContracts::MutationResultTypeId; }
FString FHyperAIStudioDialogueMutationResultPayload::GetSchemaFingerprint() const { return FHyperAIStudioDialogueContracts::MutationResultSchemaFingerprint(); }
int32 FHyperAIStudioDialogueMutationResultPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::Dialogue::Private::ClampBytes(96 + 2ll * (Phase.Len() + ContentKey.Len()));
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioDialogueContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.dialogue.speech.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({InspectToolName, InspectVariantId, InspectPayloadTypeId, InspectPayloadSchemaFingerprint(),
			InspectResultTypeId, InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({ValidateToolName, ValidateVariantId, ValidatePayloadTypeId, ValidatePayloadSchemaFingerprint(),
			ValidateResultTypeId, ValidateResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({RenderToolName, RenderVariantId, RenderPayloadTypeId, RenderPayloadSchemaFingerprint(),
			RenderResultTypeId, RenderResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({MutationToolName, MutationVariantId, ImportPayloadTypeId, ImportPayloadSchemaFingerprint(),
			MutationResultTypeId, MutationResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Edit});
		Value.ContractFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FHyperAIDialogueInspectReport FHyperAIStudioDialogueContracts::Inspect(const FHyperAIDialogueInspectRequest& Request)
{
	using namespace HyperAIStudio::Dialogue::Private;
	FHyperAIDialogueInspectReport Report;
	Report.Capabilities = GetCapabilities();
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Dialogue inspection runs on the game thread.");
		return Report;
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Pass max_output_bytes 16-128 KB.");
		return Report;
	}
	const FHyperAIStudioServiceStatus Status = FHyperAIStudioExternalServices::QueryHealth(ServiceId);
	Report.ServiceHealth = FHyperAIStudioExternalServices::LexToString(Status.Health);
	Report.ServiceDiagnostic = Status.Diagnostic;
	Report.VoiceReferenceFolder = GetDefault<UHyperAIStudioSettings>()->VoiceReferenceFolder;
	Report.Voices = Gate::ReadVoices();
	Report.AvailableClips = Gate::ReadAvailableClips();
	Report.Renders = Gate::ReadRenders();
	Report.bOk = true;
	Report.Status = TEXT("captured");
	Report.Diagnostic = FString::Printf(TEXT("Speech service %s. %d voices, %d clips, %d renders."),
		*Report.ServiceHealth, Report.Voices.Num(), Report.AvailableClips.Num(), Report.Renders.Num());
	return Report;
}

FHyperAIDialogueValidateReport FHyperAIStudioDialogueContracts::Validate(const FHyperAIDialogueValidateRequest& Request)
{
	using namespace HyperAIStudio::Dialogue::Private;
	FHyperAIDialogueValidateReport Report;
	Report.Capabilities = GetCapabilities();
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Dialogue validation runs on the game thread.");
		return Report;
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Pass max_output_bytes 16-128 KB.");
		return Report;
	}
	int64 Budget = 1024;
	for (const FHyperAIDialogueIssue& Issue : Gate::CheckLines(Request.Lines))
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
	Report.EstimatedRenderSeconds = Gate::EstimateRenderSeconds(Request.Lines);
	if (!Request.Lines.IsEmpty())
	{
		const UHyperAIStudioVoice* Voice = Gate::ResolveVoice(Request.Lines[0].Voice);
		Report.SamplePrompt = Gate::BuildPrompt(Voice ? Voice->SpeakerDescription : FString(), {Request.Lines[0]});
	}
	Report.bOk = true;
	Report.Status = Report.ErrorCount > 0 ? TEXT("invalid") : Report.WarningCount > 0 ? TEXT("valid_with_warnings") : TEXT("valid");
	Report.Diagnostic = FString::Printf(TEXT("%d errors, %d warnings; about %.0f seconds to render %d lines."),
		Report.ErrorCount, Report.WarningCount, Report.EstimatedRenderSeconds, Request.Lines.Num());
	return Report;
}

FHyperAIDialogueRenderReport FHyperAIStudioDialogueContracts::Render(const FHyperAIDialogueRenderRequest& Request)
{
	using namespace HyperAIStudio::Dialogue::Private;
	FHyperAIDialogueRenderReport Report;
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
		return Reject(TEXT("game_thread_required"), TEXT("Dialogue rendering starts on the game thread."));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_request"), TEXT("Pass max_output_bytes 16-128 KB."));
	}
	if (Request.SceneName.IsEmpty() || Request.SceneName.Len() > 64)
	{
		return Reject(TEXT("invalid_scene"), TEXT("Pass a scene_name of up to 64 characters to name the render folder."));
	}
	const float Estimate = Gate::EstimateRenderSeconds(Request.Lines);
	if (Estimate > MaxEstimatedRenderSeconds)
	{
		return Reject(TEXT("batch_too_long"), FString::Printf(
			TEXT("These %d lines are about %.0f seconds of rendering, past the %.0f one render takes. Split the scene."),
			Request.Lines.Num(), Estimate, MaxEstimatedRenderSeconds));
	}
	const TArray<FHyperAIDialogueIssue> Issues = Gate::CheckLines(Request.Lines);
	for (const FHyperAIDialogueIssue& Issue : Issues)
	{
		if (Issue.Severity == TEXT("error"))
		{
			return Reject(TEXT("invalid_lines"),
				FString::Printf(TEXT("%s: %s Run hyper_dialogue_validate for the rest."), *Issue.Subject, *Issue.Message));
		}
	}
	const FHyperAIStudioServiceStatus Status = FHyperAIStudioExternalServices::QueryHealth(ServiceId);
	if (Status.Health == EHyperAIStudioServiceHealth::Unreachable)
	{
		return Reject(TEXT("service_unreachable"), Status.Diagnostic);
	}
	double Cost = 0.0;
	FString SpendError;
	if (!FHyperAIStudioExternalServices::Reserve(ServiceId, Request.Lines.Num(), Cost, SpendError))
	{
		return Reject(TEXT("spend_cap_reached"), SpendError);
	}

	FString RenderId;
	FString Folder;
	FString Error;
	if (!Gate::StartRender(Request.SceneName, Request.Lines, RenderId, Folder, Error))
	{
		return Reject(TEXT("render_not_started"), Error);
	}
	Report.bOk = true;
	Report.Status = TEXT("rendering");
	Report.RenderId = RenderId;
	Report.Folder = Folder;
	Report.LineCount = Request.Lines.Num();
	Report.EstimatedRenderSeconds = Gate::EstimateRenderSeconds(Request.Lines);
	Report.Diagnostic = FString::Printf(
		TEXT("Rendering %d lines, about %.0f seconds, longer while the editor is using the same graphics card. ")
		TEXT("Watch it with hyper_dialogue_inspect, then import with hyper_dialogue_apply_plan."),
		Report.LineCount, Report.EstimatedRenderSeconds);
	return Report;
}

FString FHyperAIStudioDialogueContracts::ComputeImportSemanticFingerprint(const FHyperAIStudioDialogueImportPayload& Payload)
{
	using HyperAIStudio::Dialogue::Private::AppendToken;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.dialogue.import-intent.v1"));
	AppendToken(Canonical, PackId);
	AppendToken(Canonical, MutationToolName);
	AppendToken(Canonical, MutationVariantId);
	AppendToken(Canonical, ImportPayloadSchemaFingerprint());
	AppendToken(Canonical, GetAdapterDescriptor().AdapterFingerprint);
	AppendToken(Canonical, Payload.RenderId);
	AppendToken(Canonical, Payload.RenderKey);
	AppendToken(Canonical, Payload.TargetFolder);
	AppendToken(Canonical, Payload.bCreateDialogueWaves ? TEXT("dialogue_waves") : TEXT("sound_waves_only"));
	AppendToken(Canonical, Payload.bSave ? TEXT("save") : TEXT("no_save"));
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAIDialogueApplyPlanReport FHyperAIStudioDialogueContracts::BuildPlan(const FHyperAIDialogueApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Dialogue::Private;
	FHyperAIDialogueApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	Report.TargetFolder = Request.TargetFolder;
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
		return Reject(TEXT("game_thread_required"), TEXT("Dialogue import planning runs on the game thread."));
	}
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"), TEXT("A dry run takes no operation_id or expected_plan_hash."));
	}
	if (!Request.bDryRun && (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId) || !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_submission_fields"), TEXT("A non-dry submission needs a valid operation_id and the plan_hash its dry run returned."));
	}
	if (!IsCanonicalProjectFolder(Request.TargetFolder))
	{
		return Reject(TEXT("invalid_target_folder"), TEXT("Pass a /Game folder with no trailing slash, e.g. /Game/Dialogue/Act1."));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes is outside the closed bounds."));
	}
	FHyperAIDialogueRenderRecord Record;
	if (!Gate::FindRender(Request.RenderId, Record))
	{
		return Reject(TEXT("render_not_found"), FString::Printf(TEXT("No render called %s."), *Request.RenderId));
	}
	if (Record.State != TEXT("complete"))
	{
		return Reject(TEXT("render_not_complete"), FString::Printf(TEXT("That render is %s: %s."), *Record.State, *Record.Progress));
	}
	Report.RenderKey = Record.RenderKey;
	if (Record.RenderKey != Request.ExpectedRenderKey)
	{
		return Reject(TEXT("stale_render"), TEXT("The rendered audio changed since it was inspected; inspect it again."));
	}

	const TSharedRef<FHyperAIStudioDialogueImportPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioDialogueImportPayload, ESPMode::ThreadSafe>();
	Payload->RenderId = Request.RenderId;
	Payload->RenderKey = Record.RenderKey;
	Payload->TargetFolder = Request.TargetFolder;
	Payload->bCreateDialogueWaves = Request.bCreateDialogueWaves;
	Payload->bSave = Request.bSave;
	Payload->SemanticFingerprint = ComputeImportSemanticFingerprint(*Payload);

	FHyperAIStudioTrustedArtifactRequest Trusted;
	Trusted.PackId = PackId;
	Trusted.ToolName = MutationToolName;
	Trusted.VariantId = MutationVariantId;
	Trusted.Safety = EHyperAIStudioDomainSafety::Edit;
	Trusted.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
	Trusted.EffectTarget = Request.TargetFolder;
	Trusted.DeadlineMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactDeadlineMs;
	Trusted.MaxNativeOperations = MaxLinesPerRender * 3 + 8;
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
	const TSharedRef<FHyperAIStudioDialogueFreshVerifier, ESPMode::ThreadSafe> Verifier =
		MakeShared<FHyperAIStudioDialogueFreshVerifier, ESPMode::ThreadSafe>();
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
		Report.Diagnostic = FString::Printf(
			TEXT("%d lines would be imported into %s. Nothing changed. To apply, resubmit with bDryRun false, a new operation_id, and expected_plan_hash set to plan_hash."),
			Record.LinesTotal, *Request.TargetFolder);
		return Report;
	}
	if (Request.ExpectedPlanHash != Payload->SemanticFingerprint)
	{
		return Reject(TEXT("plan_hash_mismatch"), TEXT("The plan changed since its dry run. Dry-run again and review it."));
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = MutationToolName;
		Summary.VariantId = MutationVariantId;
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Request.TargetFolder;
		Summary.PlanHash = Payload->SemanticFingerprint;
		Summary.Effects.Add(FString::Printf(TEXT("import %d rendered lines (%.1f seconds of audio) as sound waves"),
			Record.LinesTotal, Record.SecondsOfAudio));
		if (Request.bCreateDialogueWaves)
		{
			Summary.Effects.Add(TEXT("and Dialogue Waves carrying each speaker and its spoken text"));
		}
		Summary.Effects.Add(Request.bSave ? TEXT("save the new packages") : TEXT("leave them unsaved"));
		for (const FString& Path : Gate::PlanImportPaths(Request.RenderId, Request.TargetFolder, Request.bCreateDialogueWaves))
		{
			Summary.Touches.Add(Path);
		}
		FString ApprovalError;
		if (!FHyperAIStudioApprovalGate::Request(Prepared, Request.OperationId, Summary, ApprovalError))
		{
			return Reject(TEXT("approval_not_queued"), ApprovalError);
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = TEXT("Waiting for approval in HyperAI Chat, Activity panel. Nothing changed yet. Poll hyper_operation_status with operation_id: it starts once approved.");
		return Report;
	}

	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	FHyperAIStudioTrustedExecutionDiagnostic StageStatus;
	FString StageError;
	if (!FHyperAIStudioTrustedExecutionFacade::StageExact(Prepared, Request.OperationId, Receipt, StageStatus, StageError))
	{
		return Reject(StageStatus.StatusCode.IsEmpty() ? FString(TEXT("stage_failed")) : StageStatus.StatusCode,
			StageError.IsEmpty() ? StageStatus.Diagnostic : StageError);
	}
	Report.bStaged = true;
	FHyperAIStudioTypedArtifactSubmissionReceipt Submission;
	FHyperAIStudioTrustedExecutionDiagnostic SubmitStatus;
	FString SubmitError;
	if (!FHyperAIStudioTrustedExecutionFacade::SubmitExact(Receipt, FString(), Submission, SubmitStatus, SubmitError))
	{
		Report.Status = SubmitStatus.StatusCode.IsEmpty() ? FString(TEXT("submit_failed")) : SubmitStatus.StatusCode;
		Report.Diagnostic = SubmitError.IsEmpty() ? SubmitStatus.Diagnostic : SubmitError;
		return Report;
	}
	Report.bExecutionSubmitted = true;
	Report.bOk = true;
	Report.Status = TEXT("submitted");
	Report.Diagnostic = TEXT("The import runs over the next editor ticks. Poll hyper_operation_status until it is terminal.");
	return Report;
}

FHyperAIDialogueInspectReport UHyperAIStudioDialogueToolset::hyper_dialogue_inspect(const FHyperAIDialogueInspectRequest& Request)
{
	return FHyperAIStudioDialogueContracts::Inspect(Request);
}

FHyperAIDialogueValidateReport UHyperAIStudioDialogueToolset::hyper_dialogue_validate(const FHyperAIDialogueValidateRequest& Request)
{
	return FHyperAIStudioDialogueContracts::Validate(Request);
}

FHyperAIDialogueRenderReport UHyperAIStudioDialogueToolset::hyper_dialogue_render(const FHyperAIDialogueRenderRequest& Request)
{
	return FHyperAIStudioDialogueContracts::Render(Request);
}

FHyperAIDialogueApplyPlanReport UHyperAIStudioDialogueToolset::hyper_dialogue_apply_plan(const FHyperAIDialogueApplyPlanRequest& Request)
{
	return FHyperAIStudioDialogueContracts::BuildPlan(Request);
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioDialogueDomainAdapter::GetDescriptor() const
{
	return FHyperAIStudioDialogueContracts::GetAdapterDescriptor();
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioDialogueDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	using Contracts = FHyperAIStudioDialogueContracts;
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
		return Reject(TEXT("typed_binding_mismatch"), TEXT("Dialogue adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Matches(Contracts::InspectToolName, Contracts::InspectVariantId, EHyperAIStudioDomainSafety::Read, Contracts::InspectPayloadTypeId, Contracts::InspectPayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioDialogueInspectResultPayload, ESPMode::ThreadSafe> Output = MakeShared<FHyperAIStudioDialogueInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Inspect(static_cast<const FHyperAIStudioDialogueInspectPayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::ValidateToolName, Contracts::ValidateVariantId, EHyperAIStudioDomainSafety::Read, Contracts::ValidatePayloadTypeId, Contracts::ValidatePayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioDialogueValidateResultPayload, ESPMode::ThreadSafe> Output = MakeShared<FHyperAIStudioDialogueValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Validate(static_cast<const FHyperAIStudioDialogueValidatePayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::RenderToolName, Contracts::RenderVariantId, EHyperAIStudioDomainSafety::Read, Contracts::RenderPayloadTypeId, Contracts::RenderPayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioDialogueRenderResultPayload, ESPMode::ThreadSafe> Output = MakeShared<FHyperAIStudioDialogueRenderResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Render(static_cast<const FHyperAIStudioDialogueRenderPayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::MutationToolName, Contracts::MutationVariantId, EHyperAIStudioDomainSafety::Edit, Contracts::ImportPayloadTypeId, Contracts::ImportPayloadSchemaFingerprint()))
	{
		const FHyperAIStudioDialogueImportPayload& Typed = static_cast<const FHyperAIStudioDialogueImportPayload&>(Payload);
		if (Contracts::ComputeImportSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"), TEXT("The sealed import semantic fingerprint drifted."));
		}
		return HyperAIStudio::Dialogue::Private::ExecuteImportPhase(Context.ActionKind, Typed);
	}
	return Reject(TEXT("typed_binding_mismatch"), TEXT("Dialogue adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

FString FHyperAIStudioDialogueFreshVerifier::GetOwnerAdapterFingerprint() const
{
	return FHyperAIStudioDialogueContracts::GetAdapterDescriptor().AdapterFingerprint;
}

bool FHyperAIStudioDialogueFreshVerifier::ResolveCanonicalEffectTarget(
	const IHyperAIStudioTypedArtifactPayload& Request, FString& OutCanonicalEffectTarget, FString& OutError)
{
	using Contracts = FHyperAIStudioDialogueContracts;
	OutCanonicalEffectTarget.Reset();
	OutError.Reset();
	if (Request.GetTypeId() != Contracts::ImportPayloadTypeId || Request.GetSchemaFingerprint() != Contracts::ImportPayloadSchemaFingerprint())
	{
		OutError = TEXT("Fresh verifier received the wrong typed dialogue request schema.");
		return false;
	}
	const FHyperAIStudioDialogueImportPayload& Typed = static_cast<const FHyperAIStudioDialogueImportPayload&>(Request);
	if (!Contracts::IsCanonicalProjectFolder(Typed.TargetFolder) || Contracts::ComputeImportSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
	{
		OutError = TEXT("Fresh verifier rejected the canonical target or semantic seal.");
		return false;
	}
	OutCanonicalEffectTarget = Typed.TargetFolder;
	return true;
}

bool FHyperAIStudioDialogueFreshVerifier::VerifyFreshExact(
	const IHyperAIStudioTypedArtifactPayload& Request, const IHyperAIStudioDomainResultPayload& Result, FString& OutPostconditionHash, FString& OutError)
{
	using Contracts = FHyperAIStudioDialogueContracts;
	OutPostconditionHash.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Fresh dialogue verification requires the game thread.");
		return false;
	}
	FString CanonicalTarget;
	if (!ResolveCanonicalEffectTarget(Request, CanonicalTarget, OutError)
		|| Result.GetTypeId() != Contracts::MutationResultTypeId || Result.GetSchemaFingerprint() != Contracts::MutationResultSchemaFingerprint())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Fresh verifier received the wrong result schema.");
		return false;
	}
	const FHyperAIStudioDialogueImportPayload& Typed = static_cast<const FHyperAIStudioDialogueImportPayload&>(Request);
	const FHyperAIStudioDialogueMutationResultPayload& TypedResult = static_cast<const FHyperAIStudioDialogueMutationResultPayload&>(Result);
	if (TypedResult.Phase != TEXT("completed") || !Contracts::IsCanonicalSha256(TypedResult.ContentKey))
	{
		OutError = TEXT("Only a completed fresh-capture result may be verified.");
		return false;
	}
	const FString ContentKey = HyperAIStudio::Dialogue::Private::ComputeImportContentKey(Typed);
	if (ContentKey != TypedResult.ContentKey)
	{
		OutError = TEXT("The imported assets changed between fresh capture and verification.");
		return false;
	}
	FString Canonical;
	HyperAIStudio::Dialogue::Private::AppendToken(Canonical, TEXT("hyperai.dialogue.import-postcondition.v1"));
	HyperAIStudio::Dialogue::Private::AppendToken(Canonical, Typed.SemanticFingerprint);
	HyperAIStudio::Dialogue::Private::AppendToken(Canonical, ContentKey);
	OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!Contracts::IsCanonicalSha256(OutPostconditionHash))
	{
		OutError = TEXT("Bounded postcondition hashing failed.");
		OutPostconditionHash.Reset();
		return false;
	}
	return true;
}

void FHyperAIStudioDialogueRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	if (!HyperAIStudio::Dialogue::EnsureCookbook())
	{
		UE_LOG(LogHyperAIStudioDialogueToolset, Warning, TEXT("Could not write %s."), *HyperAIStudio::Dialogue::GetCookbookPath());
	}
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioDialogueRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioDialogueRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	HyperAIStudio::Dialogue::Gate::Shutdown();
	bStarted = false;
}

bool FHyperAIStudioDialogueRegistration::IsRegistered() const
{
	return bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(UHyperAIStudioDialogueToolset::StaticClass(), FHyperAIStudioDialogueContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioDialogueRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioDialogueRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable() || !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioDialogueContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioDialogueToolset, Verbose, TEXT("Dialogue source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	Adapter = MakeShared<FHyperAIStudioDialogueDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioDialogueToolset, Error, TEXT("Dialogue adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(AdapterHandle, FHyperAIStudioDialogueContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioDialogueToolset, Error, TEXT("Dialogue live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	const auto Observe = []()
	{
		FHyperAIStudioTrustedProbeResult Observation;
		// The editor side is what this probe guards. A speech service that is down is reported by the tools
		// themselves, with what to start, rather than withdrawing the whole pack.
		Observation.bReady = GEditor != nullptr && FModuleManager::Get().IsModuleLoaded(TEXT("AssetTools"));
		Observation.StatusCode = Observation.bReady ? TEXT("ready") : TEXT("editor_unavailable");
		Observation.Diagnostic = Observation.bReady
			? TEXT("Dialogue rendering and import are available.")
			: TEXT("The editor asset tools are not loaded.");
		return Observation;
	};
	const FHyperAIStudioTrustedProbeResult First = Observe();
	if (!First.bReady || !ProbePublisher.Start(ProbeHandle, Observe, Error))
	{
		if (Error.IsEmpty()) Error = First.Diagnostic;
		UE_LOG(LogHyperAIStudioDialogueToolset, Error, TEXT("Dialogue live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioDialogueToolset::StaticClass(), FHyperAIStudioDialogueContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioDialogueToolset, Error, TEXT("Dialogue cohort registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioDialogueRegistration::RollBackRegistration()
{
	if (!IsInGameThread()) return;
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioDialogueToolset::StaticClass(), FHyperAIStudioDialogueContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioDialogueToolset, Error, TEXT("Dialogue owned-toolset rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioDialogueToolset, Error, TEXT("Dialogue probe rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioDialogueToolset, Error, TEXT("Dialogue adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}

#undef LOCTEXT_NAMESPACE
