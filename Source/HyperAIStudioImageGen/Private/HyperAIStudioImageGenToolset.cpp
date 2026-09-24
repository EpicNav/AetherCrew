// Games by Hyper 2026.

#include "HyperAIStudioImageGenToolset.h"

#include "Editor.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAgentImage.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioImageGenCookbook.h"
#include "HyperAIStudioImageGenGate.h"
#include "HyperAIStudioSettings.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioImageGen, Log, All);

namespace HyperAIStudio::ImageGen::Private
{
	namespace Gate = HyperAIStudio::ImageGen::Gate;

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

	int32 ClampBytes(const int64 Size)
	{
		return static_cast<int32>(FMath::Clamp<int64>(Size, 1, MAX_int32));
	}

	/**
	 * What a completed generation is verified against.
	 *
	 * The images arrive over the following minute, long after the executor's phases are done, so the
	 * postcondition is the commission rather than the pixels: this exact prompt, count and folder were sent
	 * to this exact model, and a batch for them exists. hyper_image_inspect reports the files as they land.
	 */
	FString ComputeCommissionKey(const FHyperAIStudioImageGenGeneratePayload& Payload)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.image_generation.commission.v1"));
		AppendToken(Canonical, Payload.BatchId);
		AppendToken(Canonical, Payload.Folder);
		AppendToken(Canonical, Payload.ModelId);
		AppendToken(Canonical, Payload.Prompt);
		AppendToken(Canonical, FString::FromInt(Payload.Count));
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FHyperAIStudioDomainAdapterResult ExecuteGeneratePhase(
		const EHyperAIStudioDomainExecutionActionKind Phase,
		const FHyperAIStudioImageGenGeneratePayload& Payload)
	{
		FHyperAIStudioDomainAdapterResult Result;
		const double Started = FPlatformTime::Seconds();
		auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status,
			const FString& Diagnostic, const TCHAR* ResultPhase = nullptr, const FString& ContentKey = FString())
		{
			UE_LOG(LogHyperAIStudioImageGen, Log, TEXT("Generate phase %d finished %s in %.1f ms. %s"),
				static_cast<int32>(Phase), *Status, (FPlatformTime::Seconds() - Started) * 1000.0, *Diagnostic);
			Result.Outcome = Outcome;
			Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
			Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
			if (Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				const TSharedRef<FHyperAIStudioImageGenGenerateResultPayload, ESPMode::ThreadSafe> Output =
					MakeShared<FHyperAIStudioImageGenGenerateResultPayload, ESPMode::ThreadSafe>();
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
			return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("Image generation starts on the game thread."));
		}

		if (bApply)
		{
			FHyperAIStudioImageModelSettings Model;
			if (!Gate::FindModel(Payload.ModelId, Model))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("model_not_found"),
					FString::Printf(TEXT("Model %s is no longer configured; nothing was sent."), *Payload.ModelId));
			}
			if (IFileManager::Get().DirectoryExists(*(Gate::OutputRootOnDisk() / Payload.BatchId)))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("batch_folder_exists"),
					TEXT("Something created the batch folder after planning; nothing was sent."));
			}
			double Cost = 0.0;
			FString SpendError;
			FString Agent;
			// An agent draws on the user's own plan, so there is no service and nothing to reserve against the cap.
			if (!Gate::IsAgentModel(Model, Agent) && !FHyperAIStudioExternalServices::Reserve(Model.ServiceId, Payload.Count, Cost, SpendError))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("spend_cap_reached"), SpendError);
			}
			FString StartError;
			if (!Gate::StartBatch(Payload.BatchId, Model, Payload.TemplateId, Payload.Prompt, Payload.Negative,
				Payload.Count, Payload.Width, Payload.Height, Payload.Seed, StartError))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("batch_not_started"), StartError);
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("generating"),
				FString::Printf(TEXT("%d image%s commissioned from %s into %s, about %.2f reserved."),
					Payload.Count, Payload.Count == 1 ? TEXT("") : TEXT("s"), *Payload.ModelId, *Payload.Folder, Cost),
				TEXT("generating"));
		}

		FHyperAIImageBatchInfo Batch;
		if (!Gate::FindBatch(Payload.BatchId, Batch))
		{
			return Finish(FailedOutcome, TEXT("batch_missing"), TEXT("The generation batch is gone."));
		}
		switch (Phase)
		{
		case EHyperAIStudioDomainExecutionActionKind::Validate:
			// A wrong url, a missing key or an unparsable request template fails on the first send, which is
			// usually within this tick; catching it here reports it as the operation's own failure.
			if (Batch.State == TEXT("failed"))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("generation_failed"), Batch.Diagnostic);
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"),
				FString::Printf(TEXT("The batch is %s: %s."), *Batch.State, *Batch.Progress), TEXT("validated"));
		case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"),
				TEXT("Commission captured for verification."), TEXT("completed"), ComputeCommissionKey(Payload));
		default:
			return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Unknown execution phase."));
		}
	}
}

FString FHyperAIStudioImageGenContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioImageGen.HyperAIStudioImageGenToolset");
}

TArray<FString> FHyperAIStudioImageGenContracts::GetToolNames()
{
	return {InspectToolName, GenerateToolName};
}

bool FHyperAIStudioImageGenContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

TArray<FString> FHyperAIStudioImageGenContracts::GetCapabilities()
{
	return {
		TEXT("image_models_described_in_settings_rather_than_shipped"),
		TEXT("json_request_templates_with_prompt_size_and_seed_placeholders"),
		TEXT("base64_images_read_from_a_dotted_response_path"),
		TEXT("prompt_templates_with_slots_that_drop_out_unfilled"),
		TEXT("png_jpeg_and_webp_written_into_the_project_with_a_json_sidecar"),
		TEXT("approval_and_a_session_spend_cap_before_anything_is_sent"),
		TEXT("unsupported:endpoints_that_queue_work_and_need_polling_such_as_comfyui_graphs"),
		TEXT("unsupported:image_links_in_the_reply_instead_of_base64"),
		TEXT("unsupported:importing_the_result_as_a_texture_asset")};
}

bool FHyperAIStudioImageGenContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"))) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

bool FHyperAIStudioImageGenContracts::IsCanonicalName(const FString& Value, const int32 MaxCharacters)
{
	if (Value.IsEmpty() || Value.Len() > MaxCharacters)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		const bool bAllowed = (Character >= TEXT('a') && Character <= TEXT('z'))
			|| (Character >= TEXT('A') && Character <= TEXT('Z'))
			|| (Character >= TEXT('0') && Character <= TEXT('9'))
			|| Character == TEXT('_');
		if (!bAllowed)
		{
			return false;
		}
	}
	return true;
}

FString FHyperAIStudioImageGenContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("image_generation.inspect.payload.v1|models|templates|generated|optional_batch|output_bytes"));
	return Value;
}

FString FHyperAIStudioImageGenContracts::GeneratePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("image_generation.render.payload.v1|batch|folder|model|template|composed_prompt|negative|count|size|seed|deep_clone|bounded"));
	return Value;
}

FString FHyperAIStudioImageGenContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("image_generation.inspect.result.v1|models_with_health|templates_with_slots|generated_with_sidecars|batches|spend|bounded"));
	return Value;
}

FString FHyperAIStudioImageGenContracts::GenerateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("image_generation.render.result.v1|phase|content_key|bounded"));
	return Value;
}

FString FHyperAIStudioImageGenInspectPayload::GetTypeId() const { return FHyperAIStudioImageGenContracts::InspectPayloadTypeId; }
FString FHyperAIStudioImageGenInspectPayload::GetSchemaFingerprint() const { return FHyperAIStudioImageGenContracts::InspectPayloadSchemaFingerprint(); }
int32 FHyperAIStudioImageGenInspectPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::ImageGen::Private::ClampBytes(64 + 2ll * Request.BatchId.Len());
}

FString FHyperAIStudioImageGenGeneratePayload::GetTypeId() const { return FHyperAIStudioImageGenContracts::GeneratePayloadTypeId; }
FString FHyperAIStudioImageGenGeneratePayload::GetSchemaFingerprint() const { return FHyperAIStudioImageGenContracts::GeneratePayloadSchemaFingerprint(); }
int32 FHyperAIStudioImageGenGeneratePayload::GetBoundedByteSize() const
{
	using HyperAIStudio::ImageGen::Private::EstimateBytes;
	return HyperAIStudio::ImageGen::Private::ClampBytes(160 + EstimateBytes(BatchId) + EstimateBytes(Folder)
		+ EstimateBytes(ModelId) + EstimateBytes(TemplateId) + EstimateBytes(Prompt) + EstimateBytes(Negative)
		+ EstimateBytes(SemanticFingerprint));
}
FString FHyperAIStudioImageGenGeneratePayload::GetSemanticFingerprint() const { return SemanticFingerprint; }
TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> FHyperAIStudioImageGenGeneratePayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioImageGenGeneratePayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioImageGenGeneratePayload, ESPMode::ThreadSafe>();
	Clone->BatchId = BatchId;
	Clone->Folder = Folder;
	Clone->ModelId = ModelId;
	Clone->TemplateId = TemplateId;
	Clone->Prompt = Prompt;
	Clone->Negative = Negative;
	Clone->Count = Count;
	Clone->Width = Width;
	Clone->Height = Height;
	Clone->Seed = Seed;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioImageGenInspectResultPayload::GetTypeId() const { return FHyperAIStudioImageGenContracts::InspectResultTypeId; }
FString FHyperAIStudioImageGenInspectResultPayload::GetSchemaFingerprint() const { return FHyperAIStudioImageGenContracts::InspectResultSchemaFingerprint(); }
int32 FHyperAIStudioImageGenInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::ImageGen::Private;
	int64 Size = 512 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.OutputFolder);
	for (const FHyperAIImageModelInfo& Model : Report.Models)
	{
		Size += 64 + EstimateBytes(Model.Id) + EstimateBytes(Model.Label) + EstimateBytes(Model.ServiceId)
			+ EstimateBytes(Model.Model) + EstimateBytes(Model.Health) + EstimateBytes(Model.Issue);
	}
	for (const FHyperAIImageTemplateInfo& Template : Report.Templates)
	{
		Size += 48 + EstimateBytes(Template.Id) + EstimateBytes(Template.Label) + EstimateBytes(Template.Template);
		for (const FString& Slot : Template.Slots) Size += EstimateBytes(Slot);
	}
	for (const FHyperAIImageRecord& Image : Report.Images)
	{
		Size += 64 + EstimateBytes(Image.File) + EstimateBytes(Image.BatchId) + EstimateBytes(Image.Prompt)
			+ EstimateBytes(Image.ModelId) + EstimateBytes(Image.GeneratedUtc);
	}
	for (const FHyperAIImageBatchInfo& Batch : Report.Batches)
	{
		Size += 48 + EstimateBytes(Batch.BatchId) + EstimateBytes(Batch.State) + EstimateBytes(Batch.Progress)
			+ EstimateBytes(Batch.Folder) + EstimateBytes(Batch.Diagnostic);
	}
	return ClampBytes(Size);
}

FString FHyperAIStudioImageGenGenerateResultPayload::GetTypeId() const { return FHyperAIStudioImageGenContracts::GenerateResultTypeId; }
FString FHyperAIStudioImageGenGenerateResultPayload::GetSchemaFingerprint() const { return FHyperAIStudioImageGenContracts::GenerateResultSchemaFingerprint(); }
int32 FHyperAIStudioImageGenGenerateResultPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::ImageGen::Private::ClampBytes(96 + 2ll * (Phase.Len() + ContentKey.Len()));
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioImageGenContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.image_generation.reference.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({InspectToolName, InspectVariantId, InspectPayloadTypeId, InspectPayloadSchemaFingerprint(),
			InspectResultTypeId, InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		// Spending the user's money is an external effect, and an external effect cannot run without a grant
		// that exists only as the product of a click. That is the whole reason for this safety class here.
		Value.Variants.Add({GenerateToolName, GenerateVariantId, GeneratePayloadTypeId, GeneratePayloadSchemaFingerprint(),
			GenerateResultTypeId, GenerateResultSchemaFingerprint(), EHyperAIStudioDomainSafety::ExternalEffect});
		Value.ContractFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FHyperAIImageInspectReport FHyperAIStudioImageGenContracts::Inspect(const FHyperAIImageInspectRequest& Request)
{
	using namespace HyperAIStudio::ImageGen::Private;
	FHyperAIImageInspectReport Report;
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
		return Reject(TEXT("game_thread_required"), TEXT("Image inspection runs on the game thread."));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes
		|| (!Request.BatchId.IsEmpty() && !IsCanonicalName(Request.BatchId)))
	{
		return Reject(TEXT("invalid_request"), TEXT("Pass max_output_bytes 16-128 KB, and a batch_id of letters, digits and underscore if you narrow to one."));
	}

	Report.OutputFolder = Gate::OutputFolder();
	if (const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>())
	{
		Report.SessionSpendCap = Settings->ExternalServiceSessionSpendCap;
	}
	Report.SessionSpend = static_cast<float>(FHyperAIStudioExternalServices::GetSessionSpend());
	if (Request.bIncludeModels)
	{
		Report.Models = Gate::ReadModels();
	}
	if (Request.bIncludeTemplates)
	{
		Report.Templates = Gate::ReadTemplates();
	}
	Report.Batches = Gate::ReadBatches(Request.BatchId);
	if (Request.bIncludeGenerated)
	{
		int64 Budget = 4096;
		for (const FHyperAIImageModelInfo& Model : Report.Models) Budget += 64 + EstimateBytes(Model.Id) + EstimateBytes(Model.Issue);
		for (const FHyperAIImageTemplateInfo& Template : Report.Templates) Budget += 48 + EstimateBytes(Template.Template);
		for (FHyperAIImageRecord& Image : Gate::ReadGenerated(Request.BatchId, /*Max=*/128))
		{
			const int64 Bytes = 64 + EstimateBytes(Image.File) + EstimateBytes(Image.Prompt) + EstimateBytes(Image.ModelId);
			if (Budget + Bytes > Request.MaxOutputBytes)
			{
				Report.bTruncated = true;
				break;
			}
			Budget += Bytes;
			Report.Images.Add(MoveTemp(Image));
		}
	}

	const int32 Usable = Report.Models.FilterByPredicate(
		[](const FHyperAIImageModelInfo& Model) { return Model.Issue.IsEmpty(); }).Num();
	Report.bOk = true;
	Report.Status = Report.bTruncated ? TEXT("truncated") : TEXT("captured");
	Report.Diagnostic = FString::Printf(TEXT("%d of %d models usable, %d templates, %d images generated, %d batches this session, %.2f of %.2f spent."),
		Usable, Report.Models.Num(), Report.Templates.Num(), Report.Images.Num(), Report.Batches.Num(),
		Report.SessionSpend, Report.SessionSpendCap);
	if (const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>(); Settings && Settings->ImageModels.IsEmpty())
	{
		Report.Diagnostic += TEXT(" Only the built-in agent generators are listed; .hyperai/docs/HyperAIStudio-ImageGenCookbook.md configures a service model.");
	}
	return Report;
}

FString FHyperAIStudioImageGenContracts::ComputeGenerateSemanticFingerprint(const FHyperAIStudioImageGenGeneratePayload& Payload)
{
	using HyperAIStudio::ImageGen::Private::AppendToken;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.image_generation.render-intent.v1"));
	AppendToken(Canonical, PackId);
	AppendToken(Canonical, GenerateToolName);
	AppendToken(Canonical, GenerateVariantId);
	AppendToken(Canonical, GeneratePayloadSchemaFingerprint());
	AppendToken(Canonical, GetAdapterDescriptor().AdapterFingerprint);
	AppendToken(Canonical, Payload.BatchId);
	AppendToken(Canonical, Payload.Folder);
	AppendToken(Canonical, Payload.ModelId);
	AppendToken(Canonical, Payload.TemplateId);
	AppendToken(Canonical, Payload.Prompt);
	AppendToken(Canonical, Payload.Negative);
	AppendToken(Canonical, FString::FromInt(Payload.Count));
	AppendToken(Canonical, FString::FromInt(Payload.Width));
	AppendToken(Canonical, FString::FromInt(Payload.Height));
	AppendToken(Canonical, FString::FromInt(Payload.Seed));
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAIImageGenerateReport FHyperAIStudioImageGenContracts::BuildPlan(const FHyperAIImageGenerateRequest& Request)
{
	using namespace HyperAIStudio::ImageGen::Private;
	FHyperAIImageGenerateReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	Report.ModelId = Request.ModelId;
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
		return Reject(TEXT("game_thread_required"), TEXT("Image generation planning runs on the game thread."));
	}
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"), TEXT("A dry run takes no operation_id or expected_plan_hash."));
	}
	if (!Request.bDryRun && (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId) || !FHyperAIStudioExtensionRuntime::IsValidSubmissionPlanHash(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_submission_fields"), FHyperAIStudioExtensionRuntime::InvalidSubmissionMessage());
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes is outside the closed bounds."));
	}
	if (!IsCanonicalName(Request.BatchName))
	{
		return Reject(TEXT("invalid_batch_name"), TEXT("Pass a batch_name of letters, digits and underscore, up to 48 characters; it names the folder."));
	}
	if (Request.Count < 1 || Request.Count > MaxImagesPerBatch)
	{
		return Reject(TEXT("invalid_count"), FString::Printf(TEXT("A batch is 1 to %d images."), MaxImagesPerBatch));
	}
	if (Request.Negative.Len() > MaxPromptCharacters)
	{
		return Reject(TEXT("invalid_negative"), TEXT("The negative prompt is past the prompt length limit."));
	}

	FHyperAIStudioImageModelSettings Model;
	if (!Gate::FindModel(Request.ModelId, Model))
	{
		return Reject(TEXT("model_not_found"), FString::Printf(
			TEXT("No image model with id %s. codex_image_gen is built in (Codex draws it on your own plan); hyper_image_inspect lists ")
			TEXT("every model, and .hyperai/docs/HyperAIStudio-ImageGenCookbook.md configures a service."), *Request.ModelId.Left(48)));
	}
	FString Agent;
	const bool bAgent = Gate::IsAgentModel(Model, Agent);
	const FString AgentLabel = Agent.Equals(TEXT("gemini"), ESearchCase::IgnoreCase) ? TEXT("Gemini") : TEXT("Codex");
	FHyperAIStudioExternalServiceSettings Service;
	if (bAgent)
	{
		FString Issue;
		if (!FHyperAIStudioAgentImage::CanDraw(Agent, Issue))
		{
			return Reject(TEXT("agent_unavailable"), Issue);
		}
	}
	else
	{
		if (!FHyperAIStudioExternalServices::Find(Model.ServiceId, Service))
		{
			return Reject(TEXT("service_not_found"), FString::Printf(
				TEXT("Model %s names external service %s, which is not configured."), *Model.Id, *Model.ServiceId));
		}
		if (!FHyperAIStudioExternalServices::HasApiKey(Service))
		{
			return Reject(TEXT("api_key_missing"), FString::Printf(
				TEXT("Set the %s environment variable and restart the editor; the key is never read from a config file."),
				*Service.ApiKeyVariable));
		}
		const FHyperAIStudioServiceStatus Health = FHyperAIStudioExternalServices::QueryHealth(Model.ServiceId);
		if (Health.Health == EHyperAIStudioServiceHealth::Unreachable)
		{
			return Reject(TEXT("service_unreachable"), Health.Diagnostic);
		}
	}

	const int32 Width = Request.Width > 0 ? Request.Width : Model.Width;
	const int32 Height = Request.Height > 0 ? Request.Height : Model.Height;
	if (Width < MinImageSize || Width > MaxImageSize || Height < MinImageSize || Height > MaxImageSize)
	{
		return Reject(TEXT("invalid_size"), FString::Printf(TEXT("Width and height are %d to %d, or 0 for the model's own size."),
			MinImageSize, MaxImageSize));
	}
	const int32 Seed = Request.Seed < 0 ? -1 : Request.Seed;

	FString Prompt;
	FString ComposeError;
	if (!Gate::ComposePrompt(Request.TemplateId, Request.Slots, Request.Prompt, Prompt, Report.UnfilledSlots, ComposeError))
	{
		return Reject(TEXT("invalid_prompt"), ComposeError);
	}
	Report.ComposedPrompt = Prompt;

	// Built here as well as when sending, so a request template that cannot produce valid JSON is caught on
	// the dry run rather than after the user has approved a spend. An agent takes the prompt as it is.
	FString Body;
	FString BodyError;
	if (!bAgent && !Gate::BuildRequestBody(Model, Prompt, Request.Negative, Width, Height, Seed, Body, BodyError))
	{
		return Reject(TEXT("invalid_request_template"), BodyError);
	}
	if (!bAgent && Model.ResponseImagePath.IsEmpty())
	{
		return Reject(TEXT("invalid_response_path"), FString::Printf(
			TEXT("Model %s has no response_image_path; see the cookbook."), *Model.Id));
	}

	const FString Folder = Gate::OutputFolder() / Request.BatchName;
	if (IFileManager::Get().DirectoryExists(*(Gate::OutputRootOnDisk() / Request.BatchName)))
	{
		return Reject(TEXT("batch_exists"), FString::Printf(
			TEXT("%s already exists. Pick another batch_name so the earlier reference is not confused with this one."), *Folder));
	}

	const TSharedRef<FHyperAIStudioImageGenGeneratePayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioImageGenGeneratePayload, ESPMode::ThreadSafe>();
	Payload->BatchId = Request.BatchName;
	Payload->Folder = Folder;
	Payload->ModelId = Model.Id;
	Payload->TemplateId = Request.TemplateId;
	Payload->Prompt = Prompt;
	Payload->Negative = Request.Negative;
	Payload->Count = Request.Count;
	Payload->Width = Width;
	Payload->Height = Height;
	Payload->Seed = Seed;
	Payload->SemanticFingerprint = ComputeGenerateSemanticFingerprint(*Payload);
	if (Payload->GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("payload_bound_exceeded"), TEXT("The prompt exceeds the bounded request size."));
	}

	Report.BatchId = Payload->BatchId;
	Report.Folder = Folder;
	Report.Count = Request.Count;
	Report.Width = Width;
	Report.Height = Height;
	Report.Seed = Seed;
	Report.EstimatedCost = Service.CostPerCall * Request.Count;
	Report.bRunsOnThisMachine = Model.bRunsOnThisMachine;

	FHyperAIStudioTrustedArtifactRequest Trusted;
	Trusted.PackId = PackId;
	Trusted.ToolName = GenerateToolName;
	Trusted.VariantId = GenerateVariantId;
	Trusted.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
	Trusted.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
	Trusted.EffectTarget = Folder;
	Trusted.DeadlineMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactDeadlineMs;
	Trusted.MaxNativeOperations = MaxImagesPerBatch + 8;
	Trusted.MaxGameThreadMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactGameThreadMs;
	Trusted.MaxOutputBytes = Request.MaxOutputBytes;
	Trusted.MaxResultBytes = 256;
	Trusted.StageLifetimeMs = StageLifetimeMs;
	Trusted.bCompileOnce = false;
	// Nothing here is a package, so there is nothing to save; the images are files and land as they arrive.
	Trusted.bSaveOnce = false;
	Trusted.bValidateOnce = true;
	Trusted.bVerifyFreshOnce = true;

	FHyperAIStudioTrustedPreparedArtifact Prepared;
	FHyperAIStudioTrustedPrepareReport Prepare;
	FString PrepareError;
	const TSharedRef<FHyperAIStudioImageGenFreshVerifier, ESPMode::ThreadSafe> Verifier =
		MakeShared<FHyperAIStudioImageGenFreshVerifier, ESPMode::ThreadSafe>();
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

	const FString SlowWarning = bAgent
		? FString::Printf(TEXT(" %s draws these on your own %s plan, about a minute each; it picks the nearest size it can make and takes no seed."),
			*AgentLabel, *AgentLabel)
		: Model.bRunsOnThisMachine
		? FString(TEXT(" This model runs on this machine, so it shares the graphics card with the editor and can take many times longer than it would alone; close the editor if it crawls."))
		: FString();
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("planned");
		Report.Diagnostic = FString::Printf(
			TEXT("Nothing was sent and nothing was spent. Read composed_prompt before resubmitting: %d image%s at %dx%d from %s, about %.2f.%s ")
			TEXT("To generate, resubmit with bDryRun false, a new operation_id, and expected_plan_hash set to plan_hash."),
			Request.Count, Request.Count == 1 ? TEXT("") : TEXT("s"), Width, Height, *Model.Id, Report.EstimatedCost, *SlowWarning);
		if (!Report.UnfilledSlots.IsEmpty())
		{
			Report.Diagnostic += FString::Printf(TEXT(" Unfilled template slots dropped out of the prompt: %s."),
				*FString::Join(Report.UnfilledSlots, TEXT(", ")));
		}
		return Report;
	}
	if (!FHyperAIStudioExtensionRuntime::IsAcceptedPlanHash(Request.ExpectedPlanHash, Payload->SemanticFingerprint))
	{
		return Reject(TEXT("plan_hash_mismatch"), TEXT("The plan changed since its dry run. Dry-run again and read the composed prompt."));
	}

	// Always: an external effect cannot be submitted without a grant, and the grant is the click.
	FHyperAIStudioApprovalSummary Summary;
	Summary.PackId = PackId;
	Summary.ToolName = GenerateToolName;
	Summary.VariantId = GenerateVariantId;
	Summary.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
	Summary.EffectTarget = Folder;
	Summary.PlanHash = Payload->SemanticFingerprint;
	Summary.Effects.Add(bAgent
		? FString::Printf(TEXT("generate %d image%s with %s, drawn by %s on your own plan"),
			Request.Count, Request.Count == 1 ? TEXT("") : TEXT("s"), *Model.Id, *AgentLabel)
		: FString::Printf(TEXT("generate %d image%s at %dx%d with %s, about %.2f"),
			Request.Count, Request.Count == 1 ? TEXT("") : TEXT("s"), Width, Height, *Model.Id, Report.EstimatedCost));
	Summary.Effects.Add(TEXT("prompt: ") + Prompt);
	if (!Request.Negative.IsEmpty()) Summary.Effects.Add(TEXT("negative: ") + Request.Negative);
	Summary.Effects.Add(FString::Printf(TEXT("write %d PNG files and their sidecars into %s"), Request.Count, *Folder));
	Summary.Touches.Add(Folder);
	FString ApprovalError;
	if (!FHyperAIStudioApprovalGate::Request(Prepared, Request.OperationId, Summary, ApprovalError))
	{
		return Reject(TEXT("approval_not_queued"), ApprovalError);
	}
	Report.bOk = true;
	Report.Status = TEXT("awaiting_user_approval");
	Report.Diagnostic = FString::Printf(
		TEXT("Waiting for approval in HyperAI Chat, Activity panel. Nothing has been sent or spent.%s ")
		TEXT("Call hyper_operation_wait with operation_id and bWaitThroughApproval true; once approved, watch the batch with hyper_image_inspect."), *SlowWarning);
	return Report;
}

FHyperAIImageInspectReport UHyperAIStudioImageGenToolset::hyper_image_inspect(const FHyperAIImageInspectRequest& Request)
{
	return FHyperAIStudioImageGenContracts::Inspect(Request);
}

FHyperAIImageGenerateReport UHyperAIStudioImageGenToolset::hyper_image_generate(const FHyperAIImageGenerateRequest& Request)
{
	return FHyperAIStudioImageGenContracts::BuildPlan(Request);
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioImageGenDomainAdapter::GetDescriptor() const
{
	return FHyperAIStudioImageGenContracts::GetAdapterDescriptor();
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioImageGenDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	using Contracts = FHyperAIStudioImageGenContracts;
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
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty() && Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0 || Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"), TEXT("Image generation adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Matches(Contracts::InspectToolName, Contracts::InspectVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::InspectPayloadTypeId, Contracts::InspectPayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioImageGenInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioImageGenInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Inspect(static_cast<const FHyperAIStudioImageGenInspectPayload&>(Payload).Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Matches(Contracts::GenerateToolName, Contracts::GenerateVariantId, EHyperAIStudioDomainSafety::ExternalEffect,
		Contracts::GeneratePayloadTypeId, Contracts::GeneratePayloadSchemaFingerprint()))
	{
		const FHyperAIStudioImageGenGeneratePayload& Typed = static_cast<const FHyperAIStudioImageGenGeneratePayload&>(Payload);
		if (Contracts::ComputeGenerateSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"), TEXT("The sealed generation semantic fingerprint drifted."));
		}
		return HyperAIStudio::ImageGen::Private::ExecuteGeneratePhase(Context.ActionKind, Typed);
	}
	return Reject(TEXT("typed_binding_mismatch"), TEXT("Image generation adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

FString FHyperAIStudioImageGenFreshVerifier::GetOwnerAdapterFingerprint() const
{
	return FHyperAIStudioImageGenContracts::GetAdapterDescriptor().AdapterFingerprint;
}

bool FHyperAIStudioImageGenFreshVerifier::ResolveCanonicalEffectTarget(
	const IHyperAIStudioTypedArtifactPayload& Request, FString& OutCanonicalEffectTarget, FString& OutError)
{
	using Contracts = FHyperAIStudioImageGenContracts;
	OutCanonicalEffectTarget.Reset();
	OutError.Reset();
	if (Request.GetTypeId() != Contracts::GeneratePayloadTypeId || Request.GetSchemaFingerprint() != Contracts::GeneratePayloadSchemaFingerprint())
	{
		OutError = TEXT("Fresh verifier received the wrong typed image generation request schema.");
		return false;
	}
	const FHyperAIStudioImageGenGeneratePayload& Typed = static_cast<const FHyperAIStudioImageGenGeneratePayload&>(Request);
	if (!Contracts::IsCanonicalName(Typed.BatchId) || Typed.Folder.IsEmpty()
		|| Typed.Folder.Len() > FHyperAIStudioTypedArtifactLimits::MaxEffectTargetChars
		|| Contracts::ComputeGenerateSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
	{
		OutError = TEXT("Fresh verifier rejected the canonical batch folder or semantic seal.");
		return false;
	}
	OutCanonicalEffectTarget = Typed.Folder;
	return true;
}

bool FHyperAIStudioImageGenFreshVerifier::VerifyFreshExact(
	const IHyperAIStudioTypedArtifactPayload& Request, const IHyperAIStudioDomainResultPayload& Result,
	FString& OutPostconditionHash, FString& OutError)
{
	using Contracts = FHyperAIStudioImageGenContracts;
	OutPostconditionHash.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Fresh image generation verification requires the game thread.");
		return false;
	}
	FString CanonicalTarget;
	if (!ResolveCanonicalEffectTarget(Request, CanonicalTarget, OutError)
		|| Result.GetTypeId() != Contracts::GenerateResultTypeId || Result.GetSchemaFingerprint() != Contracts::GenerateResultSchemaFingerprint())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Fresh verifier received the wrong result schema.");
		return false;
	}
	const FHyperAIStudioImageGenGeneratePayload& Typed = static_cast<const FHyperAIStudioImageGenGeneratePayload&>(Request);
	const FHyperAIStudioImageGenGenerateResultPayload& TypedResult = static_cast<const FHyperAIStudioImageGenGenerateResultPayload&>(Result);
	if (TypedResult.Phase != TEXT("completed") || !Contracts::IsCanonicalSha256(TypedResult.ContentKey))
	{
		OutError = TEXT("Only a completed fresh-capture result may be verified.");
		return false;
	}
	if (TypedResult.ContentKey != HyperAIStudio::ImageGen::Private::ComputeCommissionKey(Typed))
	{
		OutError = TEXT("The commission changed between fresh capture and verification.");
		return false;
	}
	FHyperAIImageBatchInfo Batch;
	if (!HyperAIStudio::ImageGen::Gate::FindBatch(Typed.BatchId, Batch) || Batch.Folder != Typed.Folder)
	{
		OutError = TEXT("No generation batch exists for the approved commission.");
		return false;
	}
	FString Canonical;
	HyperAIStudio::ImageGen::Private::AppendToken(Canonical, TEXT("hyperai.image_generation.render-postcondition.v1"));
	HyperAIStudio::ImageGen::Private::AppendToken(Canonical, Typed.SemanticFingerprint);
	HyperAIStudio::ImageGen::Private::AppendToken(Canonical, TypedResult.ContentKey);
	OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!Contracts::IsCanonicalSha256(OutPostconditionHash))
	{
		OutError = TEXT("Bounded postcondition hashing failed.");
		OutPostconditionHash.Reset();
		return false;
	}
	return true;
}

void FHyperAIStudioImageGenRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	// This pack ships no model, so the cookbook is the only thing standing between an agent and a tool that
	// looks broken. It exists as soon as the tools do.
	if (!HyperAIStudio::ImageGen::EnsureCookbook())
	{
		UE_LOG(LogHyperAIStudioImageGen, Warning, TEXT("Could not write %s."), *HyperAIStudio::ImageGen::GetCookbookPath());
	}
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioImageGenRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioImageGenRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioImageGenRegistration::IsRegistered() const
{
	return bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioImageGenToolset::StaticClass(), FHyperAIStudioImageGenContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioImageGenRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioImageGenRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable() || !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioImageGenContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioImageGen, Verbose, TEXT("Image generation source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	Adapter = MakeShared<FHyperAIStudioImageGenDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioImageGen, Error, TEXT("Image generation adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(AdapterHandle, FHyperAIStudioImageGenContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioImageGen, Error, TEXT("Image generation live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	// Ready means the tools can run, not that a model is configured: with none, hyper_image_inspect is the
	// thing that says so, and a probe that withheld the tools would leave nobody to say it.
	const auto Observe = []()
	{
		FHyperAIStudioTrustedProbeResult Observation;
		Observation.bReady = FModuleManager::Get().IsModuleLoaded(TEXT("HTTP")) && GEditor != nullptr;
		Observation.StatusCode = Observation.bReady ? TEXT("ready") : TEXT("http_unavailable");
		Observation.Diagnostic = Observation.bReady
			? TEXT("Image generation can reach configured services.")
			: TEXT("The HTTP module is not loaded, so no image service can be reached.");
		return Observation;
	};
	const FHyperAIStudioTrustedProbeResult First = Observe();
	if (!First.bReady || !ProbePublisher.Start(ProbeHandle, Observe, Error))
	{
		if (Error.IsEmpty()) Error = First.Diagnostic;
		UE_LOG(LogHyperAIStudioImageGen, Error, TEXT("Image generation live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioImageGenToolset::StaticClass(), FHyperAIStudioImageGenContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioImageGen, Error, TEXT("Image generation cohort registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioImageGenRegistration::RollBackRegistration()
{
	if (!IsInGameThread()) return;
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioImageGenToolset::StaticClass(), FHyperAIStudioImageGenContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioImageGen, Error, TEXT("Image generation owned-toolset rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioImageGen, Error, TEXT("Image generation probe rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioImageGen, Error, TEXT("Image generation adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}
