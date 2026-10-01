// Games by Hyper 2026.

#include "HyperAIStudioLightingToolset.h"
#include "HyperAIStudioOperationWait.h"
#include "HyperAIStudioAgentActivity.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioExecutionQueue.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioImageMatch.h"
#include "HyperAIStudioLightingCapture.h"
#include "HyperAIStudioLightingGate.h"
#include "HyperAIStudioMatchJobs.h"
#include "HyperAIStudioPreviewPanel.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioLighting, Log, All);

namespace HyperAIStudio::Lighting::Private
{
	namespace Gate = HyperAIStudio::Lighting::Gate;

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

	FText TwoDecimals(const float Value)
	{
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = 2;
		Options.MaximumFractionalDigits = 2;
		return FText::AsNumber(Value, &Options);
	}

	EHyperAIStudioPreviewTone ToneFor(const float Magnitude, const float Close, const float Far)
	{
		if (Magnitude <= Close) return EHyperAIStudioPreviewTone::Good;
		return Magnitude >= Far ? EHyperAIStudioPreviewTone::Warning : EHyperAIStudioPreviewTone::Neutral;
	}

	/** The panel's Lock button: the user's own click in the editor, applied as one undoable step. */
	void LockExposureNow()
	{
		UWorld* World = Gate::GetEditorWorld();
		if (!World)
		{
			return;
		}
		FHyperAILightingOp Op;
		Op.Kind = TEXT("lock_exposure");
		Op.Value = TEXT("0");
		TArray<AActor*> Touched;
		FString Error;
		if (!Gate::ApplyOps(*World, {Op}, Touched, Error))
		{
			UE_LOG(LogHyperAIStudioLighting, Warning, TEXT("Could not lock exposure from the preview panel: %s"), *Error);
		}
	}

	/** Fills the preview panel: the reference, a fresh viewport capture, and what separates them. */
	bool RefreshPreview(const FString& ReferenceImagePath, FHyperAIStudioPreviewComparison& Out, FString& OutError)
	{
		FHyperAILightingCompareRequest Request;
		Request.ReferenceImagePath = ReferenceImagePath;
		const FHyperAILightingCompareReport Report = FHyperAIStudioLightingContracts::Compare(Request);
		if (!Report.bOk)
		{
			OutError = Report.Diagnostic;
			return false;
		}
		Out.ImagePathA = ReferenceImagePath;
		Out.ImagePathB = Report.CurrentImageWrittenPath;
		Out.LabelA = NSLOCTEXT("HyperAIStudioLightingPreview", "Reference", "Reference");
		Out.LabelB = NSLOCTEXT("HyperAIStudioLightingPreview", "Viewport", "Level viewport");
		Out.HistogramA = Report.Reference.Histogram;
		Out.HistogramB = Report.Current.Histogram;

		auto Add = [&Out](const FText& Label, const FText& Value, const EHyperAIStudioPreviewTone Tone)
		{
			Out.Stats.Add({Label, Value, Tone});
		};
		Add(NSLOCTEXT("HyperAIStudioLightingPreview", "Distance", "Distance"), TwoDecimals(Report.HistogramDistance),
			Report.bMatched ? EHyperAIStudioPreviewTone::Good : EHyperAIStudioPreviewTone::Warning);
		Add(NSLOCTEXT("HyperAIStudioLightingPreview", "Exposure", "Exposure"),
			FText::Format(NSLOCTEXT("HyperAIStudioLightingPreview", "EV", "{0} EV"), TwoDecimals(Report.ExposureDeltaEV)),
			ToneFor(FMath::Abs(Report.ExposureDeltaEV), 0.1f, 0.35f));
		Add(NSLOCTEXT("HyperAIStudioLightingPreview", "Temperature", "Temperature"),
			FText::Format(NSLOCTEXT("HyperAIStudioLightingPreview", "Kelvin", "{0} K"),
				FText::AsNumber(FMath::RoundToInt(Report.ColorTemperatureDeltaK))),
			ToneFor(FMath::Abs(Report.ColorTemperatureDeltaK), 150.0f, 600.0f));
		Add(NSLOCTEXT("HyperAIStudioLightingPreview", "Contrast", "Contrast"), TwoDecimals(Report.ContrastRatioDelta),
			ToneFor(FMath::Abs(Report.ContrastRatioDelta), 0.2f, 1.0f));
		Add(NSLOCTEXT("HyperAIStudioLightingPreview", "Saturation", "Saturation"), TwoDecimals(Report.SaturationDelta),
			ToneFor(FMath::Abs(Report.SaturationDelta), 0.02f, 0.08f));
		if (!Report.Suggestions.IsEmpty())
		{
			Add(NSLOCTEXT("HyperAIStudioLightingPreview", "Next", "Next"), FText::FromString(Report.Suggestions[0]),
				EHyperAIStudioPreviewTone::Neutral);
		}

		if (!Report.bExposureLocked)
		{
			// Auto exposure re-brightens every change, so the numbers above describe the eye, not the lighting.
			Out.Warning = NSLOCTEXT("HyperAIStudioLightingPreview", "AutoExposure",
				"Auto exposure is on, so this comparison measures the eye adapting, not the lighting. Lock it before matching a reference.");
			Out.WarningActionLabel = NSLOCTEXT("HyperAIStudioLightingPreview", "LockExposure", "Lock exposure");
			Out.WarningAction = FSimpleDelegate::CreateStatic(&LockExposureNow);
		}
		return true;
	}

	FString FullProjectPath(const FString& Path)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::ProjectDir() / Path : Path);
	}

	/** The Image match source: a session's latest attempt against its reference, with every attempt as a strip. */
	bool RefreshMatchPreview(const FString& ReferenceImagePath, FHyperAIStudioPreviewComparison& Out, FString& OutError)
	{
		namespace Match = HyperAIStudio::Lighting::Match;
		const FString SessionId = Match::FindSessionFor(ReferenceImagePath);
		FString Reference;
		FString Mode = TEXT("look");
		TArray<FHyperAIImageIteration> History;
		float Target = 80.f;
		const bool bSession = !SessionId.IsEmpty() && Match::ReadSession(SessionId, Reference, Mode, History, Target) && !History.IsEmpty();
		FHyperAIImageCompareRequest Request;
		Request.ReferenceImagePath = ReferenceImagePath;
		Request.Mode = Mode;
		Request.TargetScore = Target;
		// Without a session, the level viewport is the attempt; with one, its latest candidate is.
		Request.CandidateImagePath = bSession ? History.Last().CandidateImagePath : FString();
		const FHyperAIImageCompareReport Report = Match::Compare(Request);
		if (!Report.bOk)
		{
			OutError = Report.Diagnostic;
			return false;
		}
		Out.ImagePathA = ReferenceImagePath;
		Out.ImagePathB = FullProjectPath(bSession ? History.Last().CandidateImagePath : Report.CandidateWrittenPath);
		Out.LabelA = NSLOCTEXT("HyperAIStudioLightingPreview", "MatchReference", "Reference");
		Out.LabelB = bSession
			? FText::Format(NSLOCTEXT("HyperAIStudioLightingPreview", "MatchIteration", "Iteration {0}"), FText::AsNumber(History.Last().Index))
			: NSLOCTEXT("HyperAIStudioLightingPreview", "MatchViewport", "Level viewport");
		auto Add = [&Out](const FText& Label, const FText& Value, const EHyperAIStudioPreviewTone Tone)
		{
			Out.Stats.Add({Label, Value, Tone});
		};
		auto ScoreTone = [Target](const float Score)
		{
			return Score >= Target ? EHyperAIStudioPreviewTone::Good : Score >= Target - 15.f ? EHyperAIStudioPreviewTone::Neutral : EHyperAIStudioPreviewTone::Warning;
		};
		Add(NSLOCTEXT("HyperAIStudioLightingPreview", "MatchScore", "Score"),
			FText::Format(NSLOCTEXT("HyperAIStudioLightingPreview", "MatchScoreValue", "{0} of {1}"), FText::AsNumber(FMath::RoundToInt(Report.Score)),
				FText::AsNumber(FMath::RoundToInt(Target))), ScoreTone(Report.Score));
		Add(NSLOCTEXT("HyperAIStudioLightingPreview", "MatchPalette", "Palette"), FText::AsNumber(FMath::RoundToInt(Report.PaletteScore)), ScoreTone(Report.PaletteScore));
		Add(NSLOCTEXT("HyperAIStudioLightingPreview", "MatchTone", "Tone"), FText::AsNumber(FMath::RoundToInt(Report.ToneScore)), ScoreTone(Report.ToneScore));
		Add(NSLOCTEXT("HyperAIStudioLightingPreview", "MatchLayout", "Layout"), FText::AsNumber(FMath::RoundToInt(Report.LayoutScore)), ScoreTone(Report.LayoutScore));
		Add(NSLOCTEXT("HyperAIStudioLightingPreview", "MatchDetail", "Detail"), FText::AsNumber(FMath::RoundToInt(Report.DetailScore)), ScoreTone(Report.DetailScore));
		if (Mode == TEXT("aligned"))
		{
			Add(NSLOCTEXT("HyperAIStudioLightingPreview", "MatchStructure", "Structure"), FText::AsNumber(FMath::RoundToInt(Report.StructureScore)),
				ScoreTone(Report.StructureScore));
		}
		if (!Report.Suggestions.IsEmpty())
		{
			Add(NSLOCTEXT("HyperAIStudioLightingPreview", "MatchNext", "Next"), FText::FromString(Report.Suggestions[0]), EHyperAIStudioPreviewTone::Neutral);
		}
		for (const FHyperAIImageIteration& Iteration : History)
		{
			FHyperAIStudioPreviewFrame& Frame = Out.Frames.AddDefaulted_GetRef();
			Frame.ImagePath = FullProjectPath(Iteration.CandidateImagePath);
			Frame.Label = FText::Format(NSLOCTEXT("HyperAIStudioLightingPreview", "MatchFrame", "#{0}: {1}"), FText::AsNumber(Iteration.Index),
				FText::AsNumber(FMath::RoundToInt(Iteration.Score)));
			Frame.Tooltip = FText::FromString(Iteration.Note);
			Frame.Tone = ScoreTone(Iteration.Score);
		}
		return true;
	}

	int64 EstimateMetricsBytes(const FHyperAILightingImageMetrics& Metrics)
	{
		return 160 + 8ll * Metrics.Histogram.Num();
	}

	int32 ClampBytes(const int64 Size)
	{
		return static_cast<int32>(FMath::Clamp<int64>(Size, 1, MAX_int32));
	}

	FString DescribeOp(const FHyperAILightingOp& Op)
	{
		FString Text = Op.Kind;
		if (!Op.Property.IsEmpty()) Text += TEXT(" ") + Op.Property;
		if (!Op.Value.IsEmpty()) Text += TEXT(" = ") + Op.Value;
		if (!Op.ActorLabel.IsEmpty()) Text += TEXT(" on ") + Op.ActorLabel;
		return Text;
	}

	FHyperAIStudioDomainAdapterResult ExecuteEditPhase(
		const EHyperAIStudioDomainExecutionActionKind Phase,
		const FHyperAIStudioLightingEditOpsPayload& Payload)
	{
		FHyperAIStudioDomainAdapterResult Result;
		auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status,
			const FString& Diagnostic, const TCHAR* ResultPhase = nullptr, const FString& ContentKey = FString())
		{
			UE_LOG(LogHyperAIStudioLighting, Log, TEXT("Edit phase %d finished %s. %s"), static_cast<int32>(Phase), *Status, *Diagnostic);
			Result.Outcome = Outcome;
			Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
			Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
			if (Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				const TSharedRef<FHyperAIStudioLightingMutationResultPayload, ESPMode::ThreadSafe> Output =
					MakeShared<FHyperAIStudioLightingMutationResultPayload, ESPMode::ThreadSafe>();
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
			return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("Lighting edits run on the game thread."));
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
					TEXT("The level's lighting changed after planning; nothing was applied. Inspect again."));
			}
			TArray<AActor*> Touched;
			FString Error;
			if (!Gate::ApplyOps(*World, Payload.Ops, Touched, Error))
			{
				return Finish(Touched.IsEmpty() ? EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect
					: EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("edit_op_failed"), Error);
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("applied"),
				FString::Printf(TEXT("%d ops applied to %d actors."), Payload.Ops.Num(), Touched.Num()), TEXT("applied"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Validate:
		{
			// Every op re-validated inside Apply; here the edited actors must still resolve.
			if (Gate::ResolveTargets(*World, Payload.Ops).IsEmpty())
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("targets_missing"),
					TEXT("The edited lighting actors no longer resolve; undo reverts the edit."));
			}
			FString MaterialError;
			if (!Gate::CheckMaterialOps(*World, Payload.Ops, MaterialError))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("material_not_assigned"),
					MaterialError + TEXT(" Undo reverts the edit."));
			}
		}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"), TEXT("Edited actors resolve."), TEXT("validated"));
		case EHyperAIStudioDomainExecutionActionKind::Save:
		{
			const TArray<UPackage*> Packages = Gate::PackagesOf(Gate::ResolveTargets(*World, Payload.Ops));
			if (Packages.IsEmpty() || !UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty=*/false))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("save_failed"),
					TEXT("The lighting changed but did not save; it remains in the open level."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("saved"),
				FString::Printf(TEXT("%d packages saved."), Packages.Num()), TEXT("saved"));
		}
		case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"),
				TEXT("Fresh lighting captured for verification."), TEXT("completed"), Gate::ComputeRevision(*World));
		default:
			return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Lighting plans have no compile phase."));
		}
	}
}

FString FHyperAIStudioLightingContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioLighting.HyperAIStudioLightingToolset");
}

TArray<FString> FHyperAIStudioLightingContracts::GetToolNames()
{
	return {InspectToolName, MutationToolName, CompareToolName, CaptureToolName, ImageCompareToolName};
}

bool FHyperAIStudioLightingContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

TArray<FString> FHyperAIStudioLightingContracts::GetCapabilities()
{
	return {
		TEXT("sun_sky_light_fog_sky_atmosphere_and_unbound_post_process_capture"),
		TEXT("exposure_lock_state"),
		TEXT("closed_lighting_edits_as_one_undo_step"),
		TEXT("missing_lighting_actors_added_on_demand"),
		TEXT("material_assignment_to_a_placed_actor_mesh_slot"),
		TEXT("materials_in_view_with_parameters_and_what_keeps_them_from_taking_light"),
		TEXT("material_parameters_tuned_on_instances_the_level_owns_for_every_slot_or_one_actor"),
		TEXT("mesh_lighting_flags_emissive_light_source_indirect_lighting_and_distance_fields"),
		TEXT("reflection_captures_listed_added_and_shaped"),
		TEXT("project_rendering_methods_gi_reflections_tracing_and_substrate"),
		TEXT("point_spot_and_rect_lights_listed_by_view_edited_added_aimed_and_switched_off"),
		TEXT("volumetric_fog_for_light_shafts"),
		TEXT("any_details_panel_setting_of_the_sun_sky_light_fog_atmosphere_clouds_and_post_process_by_name"),
		TEXT("volumetric_clouds_added_and_shaped_through_a_material_instance_the_level_owns"),
		TEXT("colour_grading_luts_fitted_to_a_reference_and_imported_as_lut_textures"),
		TEXT("shadows_switched_off_on_blockout_shapes_that_stand_for_foliage_cloth_or_glass"),
		TEXT("blueprint_skies_ultra_dynamic_sky_and_weather_sun_sky_and_sky_sphere_lit_through_their_variables"),
		TEXT("viewport_or_image_vs_reference_metrics"),
		TEXT("ranked_next_change_suggestions"),
		TEXT("view_captures_from_a_camera_or_transform_with_frame_to_frame_differences"),
		TEXT("image_match_score_regions_and_sessions_for_scenes_materials_effects_and_ui"),
		TEXT("unsaved_by_default_opt_in_save_of_touched_actors"),
		TEXT("unsupported:bounded_post_process_volumes_and_baked_lighting_builds")};
}

bool FHyperAIStudioLightingContracts::IsCanonicalSha256(const FString& Value)
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

FString FHyperAIStudioLightingContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("lighting.inspect.payload.v3|editor_world|output_bytes|camera_view|label_filter|local_light_limit|material_limit"));
	return Value;
}

FString FHyperAIStudioLightingContracts::EditOpsPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("lighting.edit_ops.payload.v4|level_path|base_revision|ordered_closed_ops|local_lights|details_by_name|clouds|lut|actor_shadow|material_parameters|actor_lighting|reflection_captures|save|deep_clone|bounded"));
	return Value;
}

FString FHyperAIStudioLightingContracts::ComparePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("lighting.compare.payload.v1|project_reference_image|optional_current_image|match_threshold"));
	return Value;
}

FString FHyperAIStudioLightingContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("lighting.inspect.result.v4|level|revision|exposure_lock|actors_properties|changed_details|clouds|local_lights_by_view|reflection_captures|materials_in_view|rendering|bounded"));
	return Value;
}

FString FHyperAIStudioLightingContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("lighting.edit_ops.result.v1|phase|content_key|bounded"));
	return Value;
}

FString FHyperAIStudioLightingContracts::CapturePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("lighting.capture.payload.v1|camera_transform_or_viewport|size|frames|world_or_material_time|warmup|diff_threshold|label"));
	return Value;
}

FString FHyperAIStudioLightingContracts::CaptureResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("lighting.capture.result.v1|batch|folder|view|frames_metrics|diffs|world_time_advanced|shaders|bounded"));
	return Value;
}

FHyperAICaptureViewReport FHyperAIStudioLightingContracts::CaptureNow(const FHyperAICaptureViewRequest& Request)
{
	return HyperAIStudio::Lighting::Capture::RunNow(Request);
}

FHyperAIImageCompareReport FHyperAIStudioLightingContracts::CompareImages(const FHyperAIImageCompareRequest& Request)
{
	return HyperAIStudio::Lighting::Match::Compare(Request);
}

FString FHyperAIStudioLightingContracts::ImageComparePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("lighting.image_compare.payload.v2|project_reference|optional_candidate|mode|target|session|note|grid|fit_lut"));
	return Value;
}

FString FHyperAIStudioLightingContracts::ImageCompareResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("lighting.image_compare.result.v2|score|measures|deltas|regions|suggestions|images|lut_path_score_side|session_history|bounded"));
	return Value;
}

FString FHyperAIStudioLightingImageComparePayload::GetTypeId() const
{
	return FHyperAIStudioLightingContracts::ImageComparePayloadTypeId;
}

FString FHyperAIStudioLightingImageComparePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLightingContracts::ImageComparePayloadSchemaFingerprint();
}

int32 FHyperAIStudioLightingImageComparePayload::GetBoundedByteSize() const
{
	return HyperAIStudio::Lighting::Private::ClampBytes(96 + 2ll * (Request.ReferenceImagePath.Len() + Request.CandidateImagePath.Len()
		+ Request.Mode.Len() + Request.SessionId.Len() + Request.Note.Len()));
}

FString FHyperAIStudioLightingImageCompareResultPayload::GetTypeId() const
{
	return FHyperAIStudioLightingContracts::ImageCompareResultTypeId;
}

FString FHyperAIStudioLightingImageCompareResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLightingContracts::ImageCompareResultSchemaFingerprint();
}

int32 FHyperAIStudioLightingImageCompareResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Lighting::Private;
	int64 Size = 512 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.SideBySidePath)
		+ EstimateBytes(Report.DiffPath) + EstimateBytes(Report.CandidateWrittenPath) + EstimateBytes(Report.LutPath)
		+ EstimateBytes(Report.LutSideBySidePath) + EstimateBytes(Report.LutComposedWith);
	for (const FHyperAIImageRegion& Region : Report.Regions) Size += 96 + EstimateBytes(Region.Name) + EstimateBytes(Region.Difference);
	for (const FString& Suggestion : Report.Suggestions) Size += EstimateBytes(Suggestion);
	for (const FHyperAIImageIteration& Iteration : Report.History)
	{
		Size += 48 + EstimateBytes(Iteration.Note) + EstimateBytes(Iteration.CandidateImagePath) + EstimateBytes(Iteration.SideBySidePath);
	}
	return ClampBytes(Size);
}

FString FHyperAIStudioLightingCapturePayload::GetTypeId() const
{
	return FHyperAIStudioLightingContracts::CapturePayloadTypeId;
}

FString FHyperAIStudioLightingCapturePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLightingContracts::CapturePayloadSchemaFingerprint();
}

int32 FHyperAIStudioLightingCapturePayload::GetBoundedByteSize() const
{
	return 256 + 2 * (Request.CameraActorLabel.Len() + Request.Label.Len() + Request.Source.Len() + Request.TimeMode.Len())
		+ 4 * Request.MaterialTimes.Num();
}

FString FHyperAIStudioLightingCaptureResultPayload::GetTypeId() const
{
	return FHyperAIStudioLightingContracts::CaptureResultTypeId;
}

FString FHyperAIStudioLightingCaptureResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLightingContracts::CaptureResultSchemaFingerprint();
}

int32 FHyperAIStudioLightingCaptureResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Lighting::Private;
	int64 Size = 512 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.Folder) + EstimateBytes(Report.BatchId);
	for (const FHyperAICaptureFrame& Frame : Report.Frames) Size += 64 + EstimateBytes(Frame.ImagePath) + EstimateMetricsBytes(Frame.Metrics);
	Size += 32 * Report.Diffs.Num();
	return ClampBytes(Size);
}

FString FHyperAIStudioLightingContracts::CompareResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("lighting.compare.result.v1|metrics_pair|deltas|match|suggestions|current_image_path|bounded"));
	return Value;
}

FString FHyperAIStudioLightingInspectPayload::GetTypeId() const
{
	return FHyperAIStudioLightingContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioLightingInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLightingContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioLightingInspectPayload::GetBoundedByteSize() const
{
	return 64 + Request.CameraActorLabel.Len() + Request.LabelFilter.Len();
}

FString FHyperAIStudioLightingComparePayload::GetTypeId() const
{
	return FHyperAIStudioLightingContracts::ComparePayloadTypeId;
}

FString FHyperAIStudioLightingComparePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLightingContracts::ComparePayloadSchemaFingerprint();
}

int32 FHyperAIStudioLightingComparePayload::GetBoundedByteSize() const
{
	return HyperAIStudio::Lighting::Private::ClampBytes(64 + 2ll * (Request.ReferenceImagePath.Len() + Request.CurrentImagePath.Len()));
}

FString FHyperAIStudioLightingEditOpsPayload::GetTypeId() const
{
	return FHyperAIStudioLightingContracts::EditOpsPayloadTypeId;
}

FString FHyperAIStudioLightingEditOpsPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLightingContracts::EditOpsPayloadSchemaFingerprint();
}

int32 FHyperAIStudioLightingEditOpsPayload::GetBoundedByteSize() const
{
	using HyperAIStudio::Lighting::Private::EstimateBytes;
	int64 Size = 128 + EstimateBytes(LevelPath) + EstimateBytes(BaseRevision) + EstimateBytes(SemanticFingerprint);
	for (const FHyperAILightingOp& Op : Ops)
	{
		Size += 32 + EstimateBytes(Op.Kind) + EstimateBytes(Op.ActorLabel) + EstimateBytes(Op.Property) + EstimateBytes(Op.Value);
	}
	return HyperAIStudio::Lighting::Private::ClampBytes(Size);
}

FString FHyperAIStudioLightingEditOpsPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> FHyperAIStudioLightingEditOpsPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioLightingEditOpsPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioLightingEditOpsPayload, ESPMode::ThreadSafe>();
	Clone->LevelPath = LevelPath;
	Clone->BaseRevision = BaseRevision;
	Clone->Ops = Ops;
	Clone->bSave = bSave;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioLightingInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioLightingContracts::InspectResultTypeId;
}

FString FHyperAIStudioLightingInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLightingContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioLightingInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Lighting::Private;
	int64 Size = 256 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.LevelPath) + EstimateBytes(Report.Revision);
	for (const FHyperAILightingActorRecord& Actor : Report.Actors)
	{
		Size += 32 + EstimateBytes(Actor.Kind) + EstimateBytes(Actor.Label) + EstimateBytes(Actor.Path);
		for (const FHyperAILightingProperty& Property : Actor.Properties) Size += EstimateBytes(Property.Key) + EstimateBytes(Property.Value);
	}
	for (const FHyperAILightingMaterialRecord& Material : Report.Materials)
	{
		Size += 64 + EstimateBytes(Material.Path) + EstimateBytes(Material.Kind) + EstimateBytes(Material.BaseMaterial) + EstimateBytes(Material.BlendMode)
			+ EstimateBytes(Material.ShadingModel) + EstimateBytes(Material.Slot);
		for (const FHyperAILightingProperty& Property : Material.Parameters) Size += EstimateBytes(Property.Key) + EstimateBytes(Property.Value);
		for (const FString& Text : Material.Actors) Size += EstimateBytes(Text);
		for (const FString& Text : Material.Notes) Size += EstimateBytes(Text);
	}
	for (const FHyperAILightingProperty& Fact : Report.Rendering) Size += EstimateBytes(Fact.Key) + EstimateBytes(Fact.Value);
	return ClampBytes(Size);
}

FString FHyperAIStudioLightingCompareResultPayload::GetTypeId() const
{
	return FHyperAIStudioLightingContracts::CompareResultTypeId;
}

FString FHyperAIStudioLightingCompareResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLightingContracts::CompareResultSchemaFingerprint();
}

int32 FHyperAIStudioLightingCompareResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Lighting::Private;
	int64 Size = 256 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.CurrentImageWrittenPath)
		+ EstimateMetricsBytes(Report.Reference) + EstimateMetricsBytes(Report.Current);
	for (const FString& Suggestion : Report.Suggestions) Size += EstimateBytes(Suggestion);
	return ClampBytes(Size);
}

FString FHyperAIStudioLightingMutationResultPayload::GetTypeId() const
{
	return FHyperAIStudioLightingContracts::MutationResultTypeId;
}

FString FHyperAIStudioLightingMutationResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLightingContracts::MutationResultSchemaFingerprint();
}

int32 FHyperAIStudioLightingMutationResultPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::Lighting::Private::ClampBytes(96 + 2ll * (Phase.Len() + ContentKey.Len()));
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioLightingContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.lighting.lookdev.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({InspectToolName, InspectVariantId, InspectPayloadTypeId, InspectPayloadSchemaFingerprint(),
			InspectResultTypeId, InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({MutationToolName, MutationVariantId, EditOpsPayloadTypeId, EditOpsPayloadSchemaFingerprint(),
			MutationResultTypeId, MutationResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({CompareToolName, CompareVariantId, ComparePayloadTypeId, ComparePayloadSchemaFingerprint(),
			CompareResultTypeId, CompareResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		// Read: a capture writes only under Saved/HyperAIStudio, like the compare tool's own capture.
		Value.Variants.Add({CaptureToolName, CaptureVariantId, CapturePayloadTypeId, CapturePayloadSchemaFingerprint(),
			CaptureResultTypeId, CaptureResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		// Read: its images and sessions go under Saved/HyperAIStudio/Match.
		Value.Variants.Add({ImageCompareToolName, ImageCompareVariantId, ImageComparePayloadTypeId, ImageComparePayloadSchemaFingerprint(),
			ImageCompareResultTypeId, ImageCompareResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FHyperAILightingInspectReport FHyperAIStudioLightingContracts::Inspect(const FHyperAILightingInspectRequest& InRequest)
{
	namespace Gate = HyperAIStudio::Lighting::Gate;
	FHyperAILightingInspectRequest Request = InRequest;
	FHyperAIStudioRequestFit Fit;
	Fit.ClampOrDefault(Request.MaxOutputBytes, MinOutputBytes, MaxOutputBytes, FHyperAILightingInspectRequest().MaxOutputBytes, TEXT("max_output_bytes"));
	Fit.ClampOrDefault(Request.MaxLocalLights, 1, 128, FHyperAILightingInspectRequest().MaxLocalLights, TEXT("max_local_lights"));
	// 0 asks for no materials, so it is kept rather than read as unset.
	Fit.Clamp(Request.MaxMaterials, 0, 48, TEXT("max_materials"));
	FHyperAILightingInspectReport Report;
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
		return Reject(TEXT("game_thread_required"), TEXT("Lighting inspection runs on the game thread."));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes is outside the closed output bounds."));
	}
	UWorld* World = Gate::GetEditorWorld();
	if (!World)
	{
		return Reject(TEXT("no_editor_world"), TEXT("No level is open in the editor."));
	}
	if (Request.MaxLocalLights < 0 || Request.MaxLocalLights > 128 || Request.MaxMaterials < 0 || Request.MaxMaterials > 48
		|| Request.CameraActorLabel.Len() > 128 || Request.LabelFilter.Len() > 128)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_local_lights is 0 to 128, max_materials 0 to 48, and camera_actor_label and label_filter are up to 128 characters."));
	}
	Report.LevelPath = World->GetPathName();
	Report.Revision = Gate::ComputeRevision(*World);
	Report.bExposureLocked = Gate::IsExposureLocked(*World);
	Report.Actors = Gate::ReadActors(*World);
	Report.Rendering = Gate::ReadRendering(*World);
	TArray<FHyperAILightingActorRecord> LocalLights = Gate::ReadLocalLights(*World, Request.CameraActorLabel, Request.LabelFilter,
		Request.MaxLocalLights, Report.LocalLightCount, Report.LightView);
	if (!Request.CameraActorLabel.IsEmpty() && Report.LightView != Request.CameraActorLabel)
	{
		return Reject(TEXT("camera_not_found"), FString::Printf(TEXT("No camera is labelled '%s'; leave camera_actor_label empty to use HyperAI_MatchCamera or the level viewport."),
			*Request.CameraActorLabel));
	}
	TArray<FHyperAILightingMaterialRecord> Materials = Gate::ReadMaterials(*World, Request.CameraActorLabel, Request.MaxMaterials, Report.MaterialCount);
	// Materials, then local lights, fill what the output bound leaves, in view order, so a level with hundreds of them still answers.
	auto Bytes = [](const FHyperAILightingActorRecord& Record)
	{
		int32 Size = 64 + Record.Kind.Len() + Record.Label.Len() + Record.Path.Len();
		for (const FHyperAILightingProperty& Property : Record.Properties)
		{
			Size += 24 + Property.Key.Len() + Property.Value.Len();
		}
		return Size;
	};
	auto MaterialBytes = [](const FHyperAILightingMaterialRecord& Record)
	{
		int32 Size = 160 + Record.Path.Len() + Record.Kind.Len() + Record.BaseMaterial.Len() + Record.BlendMode.Len() + Record.ShadingModel.Len() + Record.Slot.Len();
		for (const FHyperAILightingProperty& Property : Record.Parameters)
		{
			Size += 24 + Property.Key.Len() + Property.Value.Len();
		}
		for (const FString& Text : Record.Actors)
		{
			Size += 8 + Text.Len();
		}
		for (const FString& Text : Record.Notes)
		{
			Size += 8 + Text.Len();
		}
		return Size;
	};
	int32 Used = 2048;
	for (const FHyperAILightingActorRecord& Record : Report.Actors)
	{
		Used += Bytes(Record);
	}
	for (const FHyperAILightingProperty& Fact : Report.Rendering)
	{
		Used += 24 + Fact.Key.Len() + Fact.Value.Len();
	}
	for (FHyperAILightingMaterialRecord& Material : Materials)
	{
		Used += MaterialBytes(Material);
		if (Used > Request.MaxOutputBytes) break;
		Report.Materials.Add(MoveTemp(Material));
	}
	const int32 StockActors = Report.Actors.Num();
	int32 Listed = 0;
	for (FHyperAILightingActorRecord& Light : LocalLights)
	{
		Used += Bytes(Light);
		if (Used > Request.MaxOutputBytes) break;
		Report.Actors.Add(MoveTemp(Light));
		++Listed;
	}
	Report.bOk = true;
	Report.Status = TEXT("captured");
	// A sky that owns its lights has no stock actors to add, and must not get any.
	const FString Sky = Gate::LightOwningSky(*World);
	TArray<FString> Missing;
	for (const TCHAR* Kind : {TEXT("sun"), TEXT("sky_light"), TEXT("fog"), TEXT("sky_atmosphere"), TEXT("post_process")})
	{
		if ((Sky.IsEmpty() || FCString::Strcmp(Kind, TEXT("post_process")) == 0)
			&& !Report.Actors.ContainsByPredicate([Kind](const FHyperAILightingActorRecord& Actor) { return Actor.Kind == Kind; }))
		{
			Missing.Add(Kind);
		}
	}
	const FString Local = Report.LocalLightCount == 0 ? FString(TEXT(" No point, spot or rect lights; add_light places one."))
		: FString::Printf(TEXT(" %d point, spot and rect lights, %d listed%s%s."), Report.LocalLightCount, Listed,
			Report.LightView.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", those lighting the view of %s first"), *Report.LightView),
			Listed < Report.LocalLightCount ? TEXT("; label_filter or max_local_lights (up to 128) lists others") : TEXT(""));
	const FString Surfaces = Request.MaxMaterials == 0 ? FString()
		: Report.LightView.IsEmpty() ? FString(TEXT(" No view to list materials from: place HyperAI_MatchCamera or open a perspective level viewport."))
		: FString::Printf(TEXT(" %d materials in view, %d listed with their parameters, most of the frame first; set_material_parameter tunes one wherever the level shows it, without changing the material itself."),
			Report.MaterialCount, Report.Materials.Num());
	Report.Diagnostic = FString::Printf(TEXT("%d lighting actors.%s%s%s%s%s"), StockActors,
		Sky.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" The sky is %s: it owns the sun, sky light and fog, so light it with set_sky_system and its variables."), *Sky),
		Missing.IsEmpty() ? TEXT("") : *(TEXT(" None of: ") + FString::Join(Missing, TEXT(", ")) + TEXT("; the first op on one adds it.")),
		*Local, *Surfaces,
		Report.bExposureLocked ? TEXT("") : TEXT(" Auto exposure is on: lock_exposure before comparing (it also turns off a sky's own exposure)."));
	Report.Diagnostic = Fit.AppendTo(Report.Diagnostic);
	return Report;
}

FHyperAILightingCompareReport FHyperAIStudioLightingContracts::Compare(const FHyperAILightingCompareRequest& Request)
{
	namespace Gate = HyperAIStudio::Lighting::Gate;
	FHyperAILightingCompareReport Report;
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
		return Reject(TEXT("game_thread_required"), TEXT("Lighting comparison runs on the game thread."));
	}
	if (Request.ReferenceImagePath.IsEmpty() || Request.ReferenceImagePath.Len() > 1024 || Request.CurrentImagePath.Len() > 1024
		|| !(Request.MatchThreshold > 0.f && Request.MatchThreshold <= 1.f))
	{
		return Reject(TEXT("invalid_request"), TEXT("Pass reference_image_path (up to 1024 characters) and a match_threshold above 0 and at most 1."));
	}
	TArray<FColor> Pixels;
	int32 Width = 0, Height = 0;
	FString Error;
	if (!Gate::LoadProjectImage(Request.ReferenceImagePath, Pixels, Width, Height, Error))
	{
		return Reject(TEXT("reference_unreadable"), Error);
	}
	Report.Reference = Gate::MeasurePixels(Pixels, Width, Height);

	if (!Request.CurrentImagePath.IsEmpty())
	{
		if (!Gate::LoadProjectImage(Request.CurrentImagePath, Pixels, Width, Height, Error))
		{
			return Reject(TEXT("current_image_unreadable"), Error);
		}
		// A supplied image already has its exposure baked in.
		Report.bExposureLocked = true;
	}
	else
	{
		UWorld* World = Gate::GetEditorWorld();
		if (!World)
		{
			return Reject(TEXT("no_editor_world"), TEXT("No level is open in the editor."));
		}
		if (!Gate::CaptureActiveViewport(Pixels, Width, Height, Error))
		{
			return Reject(TEXT("viewport_unavailable"), Error);
		}
		Report.bExposureLocked = Gate::IsExposureLocked(*World);
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Previews/lighting_current.png"));
		if (Gate::WritePng(Path, Pixels, Width, Height))
		{
			Report.CurrentImageWrittenPath = Path;
		}
	}
	Report.Current = Gate::MeasurePixels(Pixels, Width, Height);
	if (!Report.Reference.bValid || !Report.Current.bValid)
	{
		return Reject(TEXT("measurement_failed"), TEXT("An image had no measurable pixels."));
	}
	Gate::CompareMetrics(Report.Reference, Report.Current, Report.bExposureLocked, Request.MatchThreshold, Report);
	Report.bOk = true;
	Report.Status = Report.bMatched ? TEXT("matched") : TEXT("differs");
	Report.Diagnostic = FString::Printf(TEXT("Histogram distance %.3f (match at %.3f), exposure %+.2f EV, color temperature %+.0f K."),
		Report.HistogramDistance, Request.MatchThreshold, Report.ExposureDeltaEV, Report.ColorTemperatureDeltaK);
	return Report;
}

FString FHyperAIStudioLightingContracts::ComputeEditOpsSemanticFingerprint(const FHyperAIStudioLightingEditOpsPayload& Payload)
{
	using HyperAIStudio::Lighting::Private::AppendToken;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.lighting.edit-ops-intent.v1"));
	AppendToken(Canonical, PackId);
	AppendToken(Canonical, MutationToolName);
	AppendToken(Canonical, MutationVariantId);
	AppendToken(Canonical, EditOpsPayloadSchemaFingerprint());
	AppendToken(Canonical, GetAdapterDescriptor().AdapterFingerprint);
	AppendToken(Canonical, Payload.LevelPath);
	AppendToken(Canonical, Payload.BaseRevision);
	AppendToken(Canonical, Payload.bSave ? TEXT("save") : TEXT("no_save"));
	AppendToken(Canonical, FString::FromInt(Payload.Ops.Num()));
	for (const FHyperAILightingOp& Op : Payload.Ops)
	{
		AppendToken(Canonical, Op.Kind);
		AppendToken(Canonical, Op.ActorLabel);
		AppendToken(Canonical, Op.Property);
		AppendToken(Canonical, Op.Value);
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAILightingApplyPlanReport FHyperAIStudioLightingContracts::BuildPlan(const FHyperAILightingApplyPlanRequest& InRequest)
{
	using namespace HyperAIStudio::Lighting::Private;
	FHyperAILightingApplyPlanRequest Request = InRequest;
	FHyperAIStudioRequestFit Fit;
	Fit.ClampOrDefault(Request.MaxOutputBytes, MinOutputBytes, MaxOutputBytes, FHyperAILightingApplyPlanRequest().MaxOutputBytes, TEXT("max_output_bytes"));
	FHyperAILightingApplyPlanReport Report;
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
		return Reject(TEXT("game_thread_required"), TEXT("Lighting edit planning runs on the game thread."));
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
		return Reject(TEXT("invalid_revision"), TEXT("Pass expected_revision from hyper_lighting_inspect."));
	}
	if (Request.Ops.IsEmpty() || Request.Ops.Num() > MaxOpsPerPlan)
	{
		return Reject(TEXT("invalid_op_count"), FString::Printf(TEXT("A plan needs 1 to %d ops."), MaxOpsPerPlan));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes is outside the closed output bounds."));
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
		return Reject(TEXT("stale_revision"), TEXT("The level's lighting changed after inspection; inspect again.") + FHyperAIStudioAgentActivityLog::DescribeLastChange(Report.LevelPath));
	}
	if (Request.bSave && World->GetOutermost()->GetName().StartsWith(TEXT("/Temp/")))
	{
		return Reject(TEXT("level_never_saved"), TEXT("This level has never been saved, so there is nowhere to save the lighting; save the level first or leave bSave off."));
	}
	FString OpError;
	if (!Gate::ValidateOps(*World, Request.Ops, OpError))
	{
		return Reject(TEXT("invalid_op"), OpError);
	}

	const TSharedRef<FHyperAIStudioLightingEditOpsPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioLightingEditOpsPayload, ESPMode::ThreadSafe>();
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
	const TSharedRef<FHyperAIStudioLightingFreshVerifier, ESPMode::ThreadSafe> Verifier =
		MakeShared<FHyperAIStudioLightingFreshVerifier, ESPMode::ThreadSafe>();
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
		Report.Diagnostic = Fit.AppendTo(TEXT("Nothing changed. To apply, resubmit with bDryRun false, a new operation_id, and expected_plan_hash set to plan_hash."));
		return Report;
	}
	if (!FHyperAIStudioExtensionRuntime::IsAcceptedPlanHash(Request.ExpectedPlanHash, Payload->SemanticFingerprint))
	{
		return Reject(TEXT("plan_hash_mismatch"), TEXT("The plan changed since its dry run (ops or lighting). Dry-run again and review it."));
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
		for (const FHyperAILightingOp& Op : Request.Ops)
		{
			Summary.Effects.Add(DescribeOp(Op));
		}
		if (Request.bSave)
		{
			Summary.Effects.Add(TEXT("save the changed lighting actors"));
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
	Report.Diagnostic = Fit.AppendTo(TEXT("The edit runs over the next editor ticks. Call hyper_operation_wait with operation_id, then hyper_lighting_compare."));
	return Report;
}

FHyperAILightingInspectReport UHyperAIStudioLightingToolset::hyper_lighting_inspect(const FHyperAILightingInspectRequest& Request)
{
	return FHyperAIStudioLightingContracts::Inspect(Request);
}

UHyperAILightingApplyPlanResult* UHyperAIStudioLightingToolset::hyper_lighting_apply_plan(const FHyperAILightingApplyPlanRequest& Request)
{
	return HyperAIStudio::OperationWait::ReplyWhenDone<UHyperAILightingApplyPlanResult>(FHyperAIStudioLightingContracts::BuildFittedPlan(Request));
}

FHyperAILightingApplyPlanReport FHyperAIStudioLightingContracts::BuildFittedPlan(const FHyperAILightingApplyPlanRequest& InRequest)
{
	FHyperAILightingApplyPlanRequest Request = InRequest;
	FHyperAIStudioRequestFit Fit;
	Fit.DryRun(Request.bDryRun, Request.OperationId, Request.ExpectedPlanHash);
	FHyperAILightingApplyPlanReport Report = Fit.FreshOperationId(Request, [](const auto& Planned) { return FHyperAIStudioLightingContracts::BuildPlan(Planned); });
	Report.Diagnostic = Fit.AppendTo(Report.Diagnostic);
	return Report;
}

FHyperAILightingCompareReport UHyperAIStudioLightingToolset::hyper_lighting_compare(const FHyperAILightingCompareRequest& Request)
{
	return FHyperAIStudioLightingContracts::Compare(Request);
}

UHyperAICaptureViewResult* UHyperAIStudioLightingToolset::hyper_capture_view(const FHyperAICaptureViewRequest& Request)
{
	return HyperAIStudio::Lighting::Capture::Start(Request);
}

FHyperAIImageCompareReport UHyperAIStudioLightingToolset::hyper_image_compare(const FHyperAIImageCompareRequest& Request)
{
	return FHyperAIStudioLightingContracts::CompareImages(Request);
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioLightingDomainAdapter::GetDescriptor() const
{
	return FHyperAIStudioLightingContracts::GetAdapterDescriptor();
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioLightingDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	using Contracts = FHyperAIStudioLightingContracts;
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
		return Reject(TEXT("typed_binding_mismatch"), TEXT("Lighting adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Matches(Contracts::InspectToolName, Contracts::InspectVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::InspectPayloadTypeId, Contracts::InspectPayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioLightingInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioLightingInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Inspect(static_cast<const FHyperAIStudioLightingInspectPayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::CompareToolName, Contracts::CompareVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::ComparePayloadTypeId, Contracts::ComparePayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioLightingCompareResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioLightingCompareResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Compare(static_cast<const FHyperAIStudioLightingComparePayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::CaptureToolName, Contracts::CaptureVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::CapturePayloadTypeId, Contracts::CapturePayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioLightingCaptureResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioLightingCaptureResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::CaptureNow(static_cast<const FHyperAIStudioLightingCapturePayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::ImageCompareToolName, Contracts::ImageCompareVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::ImageComparePayloadTypeId, Contracts::ImageComparePayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioLightingImageCompareResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioLightingImageCompareResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::CompareImages(static_cast<const FHyperAIStudioLightingImageComparePayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::MutationToolName, Contracts::MutationVariantId, EHyperAIStudioDomainSafety::Edit,
		Contracts::EditOpsPayloadTypeId, Contracts::EditOpsPayloadSchemaFingerprint()))
	{
		const FHyperAIStudioLightingEditOpsPayload& Typed = static_cast<const FHyperAIStudioLightingEditOpsPayload&>(Payload);
		if (Contracts::ComputeEditOpsSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"), TEXT("The sealed edit-ops semantic fingerprint drifted."));
		}
		return HyperAIStudio::Lighting::Private::ExecuteEditPhase(Context.ActionKind, Typed);
	}
	return Reject(TEXT("typed_binding_mismatch"), TEXT("Lighting adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

FString FHyperAIStudioLightingFreshVerifier::GetOwnerAdapterFingerprint() const
{
	return FHyperAIStudioLightingContracts::GetAdapterDescriptor().AdapterFingerprint;
}

bool FHyperAIStudioLightingFreshVerifier::ResolveCanonicalEffectTarget(
	const IHyperAIStudioTypedArtifactPayload& Request,
	FString& OutCanonicalEffectTarget,
	FString& OutError)
{
	using Contracts = FHyperAIStudioLightingContracts;
	OutCanonicalEffectTarget.Reset();
	OutError.Reset();
	if (Request.GetTypeId() != Contracts::EditOpsPayloadTypeId
		|| Request.GetSchemaFingerprint() != Contracts::EditOpsPayloadSchemaFingerprint())
	{
		OutError = TEXT("Fresh verifier received the wrong typed lighting request schema.");
		return false;
	}
	const FHyperAIStudioLightingEditOpsPayload& Typed = static_cast<const FHyperAIStudioLightingEditOpsPayload&>(Request);
	if (Typed.LevelPath.IsEmpty() || Contracts::ComputeEditOpsSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
	{
		OutError = TEXT("Fresh verifier rejected the level path or semantic seal.");
		return false;
	}
	OutCanonicalEffectTarget = Typed.LevelPath;
	return true;
}

bool FHyperAIStudioLightingFreshVerifier::VerifyFreshExact(
	const IHyperAIStudioTypedArtifactPayload& Request,
	const IHyperAIStudioDomainResultPayload& Result,
	FString& OutPostconditionHash,
	FString& OutError)
{
	namespace Gate = HyperAIStudio::Lighting::Gate;
	using Contracts = FHyperAIStudioLightingContracts;
	OutPostconditionHash.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Fresh lighting verification requires the game thread.");
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
	const FHyperAIStudioLightingEditOpsPayload& Typed = static_cast<const FHyperAIStudioLightingEditOpsPayload&>(Request);
	const FHyperAIStudioLightingMutationResultPayload& TypedResult = static_cast<const FHyperAIStudioLightingMutationResultPayload&>(Result);
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
		OutError = TEXT("The lighting changed between fresh capture and verification.");
		return false;
	}
	if (Typed.bSave)
	{
		for (const UPackage* Package : Gate::PackagesOf(Gate::ResolveTargets(*World, Typed.Ops)))
		{
			if (Package->IsDirty())
			{
				OutError = FString::Printf(TEXT("%s still has unsaved changes after the save phase."), *Package->GetName());
				return false;
			}
		}
	}
	FString Canonical;
	HyperAIStudio::Lighting::Private::AppendToken(Canonical, TEXT("hyperai.lighting.edit-ops-postcondition.v1"));
	HyperAIStudio::Lighting::Private::AppendToken(Canonical, Typed.SemanticFingerprint);
	HyperAIStudio::Lighting::Private::AppendToken(Canonical, ContentKey);
	OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!Contracts::IsCanonicalSha256(OutPostconditionHash))
	{
		OutError = TEXT("Bounded postcondition hashing failed.");
		OutPostconditionHash.Reset();
		return false;
	}
	return true;
}

void FHyperAIStudioLightingRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	{
		FHyperAIStudioPreviewPanel::FSource Source;
		Source.Id = FName(TEXT("lighting"));
		Source.Label = NSLOCTEXT("HyperAIStudioLightingPreview", "SourceLabel", "Lighting: reference vs viewport");
		Source.Tooltip = NSLOCTEXT("HyperAIStudioLightingPreview", "SourceTooltip",
			"Capture the level viewport and measure it against a reference image.");
		Source.bNeedsReference = true;
		Source.Refresh = FHyperAIStudioPreviewRefresh::CreateStatic(&HyperAIStudio::Lighting::Private::RefreshPreview);
		Source.AgentActions = HyperAIStudio::Lighting::MatchJobs::Actions();
		FHyperAIStudioPreviewPanel::RegisterSource(MoveTemp(Source));
	}
	{
		FHyperAIStudioPreviewPanel::FSource Source;
		Source.Id = FName(TEXT("image_match"));
		Source.Label = NSLOCTEXT("HyperAIStudioLightingPreview", "MatchSourceLabel", "Image match: reference vs attempts");
		Source.Tooltip = NSLOCTEXT("HyperAIStudioLightingPreview", "MatchSourceTooltip",
			"Score an attempt against a reference, for scenes, materials, effects and UI. With a hyper_image_compare session, every iteration shows as a strip.");
		Source.bNeedsReference = true;
		Source.Refresh = FHyperAIStudioPreviewRefresh::CreateStatic(&HyperAIStudio::Lighting::Private::RefreshMatchPreview);
		Source.AgentActions = HyperAIStudio::Lighting::MatchJobs::Actions();
		Source.bRefreshWhileAgentWorks = true;
		// For the Images page's Relight this view: the picture a reference is made from is the view the Match jobs work on.
		Source.CaptureView = &HyperAIStudio::Lighting::MatchJobs::CaptureMatchView;
		FHyperAIStudioPreviewPanel::RegisterSource(MoveTemp(Source));
	}
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioLightingRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioLightingRegistration::Shutdown()
{
	HyperAIStudio::Lighting::Capture::CancelAll();
	FHyperAIStudioPreviewPanel::UnregisterSource(FName(TEXT("lighting")));
	FHyperAIStudioPreviewPanel::UnregisterSource(FName(TEXT("image_match")));
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioLightingRegistration::IsRegistered() const
{
	return bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioLightingToolset::StaticClass(), FHyperAIStudioLightingContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioLightingRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioLightingRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioLightingContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioLighting, Verbose, TEXT("Lighting source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	Adapter = MakeShared<FHyperAIStudioLightingDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioLighting, Error, TEXT("Lighting adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(AdapterHandle, FHyperAIStudioLightingContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioLighting, Error, TEXT("Lighting live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	const auto Observe = []()
	{
		FHyperAIStudioTrustedProbeResult Observation;
		Observation.bReady = HyperAIStudio::Lighting::Gate::GetEditorWorld() != nullptr;
		Observation.StatusCode = Observation.bReady ? TEXT("ready") : TEXT("no_editor_world");
		Observation.Diagnostic = Observation.bReady ? TEXT("A level is open in the editor.") : TEXT("No level is open in the editor.");
		return Observation;
	};
	const FHyperAIStudioTrustedProbeResult First = Observe();
	if (!First.bReady || !ProbePublisher.Start(ProbeHandle, Observe, Error))
	{
		if (Error.IsEmpty()) Error = First.Diagnostic;
		UE_LOG(LogHyperAIStudioLighting, Error, TEXT("Lighting live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioLightingToolset::StaticClass(), FHyperAIStudioLightingContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioLighting, Error, TEXT("Lighting cohort registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioLightingRegistration::RollBackRegistration()
{
	if (!IsInGameThread())
	{
		return;
	}
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioLightingToolset::StaticClass(), FHyperAIStudioLightingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioLighting, Error, TEXT("Lighting owned-toolset rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioLighting, Error, TEXT("Lighting probe rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioLighting, Error, TEXT("Lighting adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}
