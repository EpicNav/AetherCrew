// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Layout/Margin.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "ToolsetRegistry/ToolCallAsyncResult.h"
#include "HyperAIStudioUIToolset.generated.h"

/** One closed scalar from a UI value snapshot. Type selects exactly one value field. */
USTRUCT(BlueprintType)
struct FHyperAIUIFieldValue
{
	GENERATED_BODY()

	/** Closed field id; no raw reflection handle or reflection path is accepted. */
	UPROPERTY()
	FString Id;

	/** string, bool, int, number, color, vector2, margin, enum, guid, or path. */
	UPROPERTY()
	FString Type;

	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	bool bBoolValue = false;

	UPROPERTY()
	int32 IntValue = 0;

	UPROPERTY()
	double NumberValue = 0.0;

	UPROPERTY()
	FLinearColor ColorValue = FLinearColor::Transparent;

	UPROPERTY()
	FVector2D Vector2Value = FVector2D::ZeroVector;

	UPROPERTY()
	FMargin MarginValue;
};

/** Stable, value-only row from a loaded Widget Blueprint snapshot. */
USTRUCT(BlueprintType)
struct FHyperAIUIRecord
{
	GENERATED_BODY()

	/** asset_state, blueprint, widget, slot, animation, animation_binding, legacy_binding, mvvm_view, mvvm_viewmodel, or mvvm_binding. */
	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString RecordKey;

	UPROPERTY()
	FString BlueprintPath;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString ParentStableId;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString ClassPath;

	UPROPERTY()
	int32 Index = -1;

	UPROPERTY()
	bool bPersisted = true;

	UPROPERTY()
	TArray<FHyperAIUIFieldValue> Fields;
};

USTRUCT(BlueprintType)
struct FHyperAIUIIssue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString BlueprintPath;

	UPROPERTY()
	FString RecordKey;

	UPROPERTY()
	FString Message;
};

USTRUCT(BlueprintType)
struct FHyperAIUIInspectRequest
{
	GENERATED_BODY()

	/** Canonical /Game object paths. Empty scans a bounded prefix of already-loaded Widget Blueprints. */
	UPROPERTY()
	TArray<FString> TargetPaths;

	UPROPERTY()
	bool bIncludeTree = true;

	UPROPERTY()
	bool bIncludeLayout = true;

	UPROPERTY()
	bool bIncludeAnimations = true;

	UPROPERTY()
	bool bIncludeLegacyBindings = true;

	UPROPERTY()
	bool bIncludeMVVM = true;

	/** Package dirtiness and compile status are volatile and deliberately make paging unavailable. */
	UPROPERTY()
	bool bIncludeVolatile = false;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 DeadlineMs = 750;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIUIInspectReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	UPROPERTY()
	FString PersistedFingerprint;

	UPROPERTY()
	FString VolatileObservationFingerprint;

	UPROPERTY()
	FString RequestFingerprint;

	UPROPERTY()
	bool bSnapshotComplete = false;

	UPROPERTY()
	bool bVolatileIncluded = false;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	int32 LoadedObjectsScanned = 0;

	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIUIRecord> Records;

	UPROPERTY()
	TArray<FHyperAIUIIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIUIValidateRequest
{
	GENERATED_BODY()

	/** Canonical exact object paths. Empty validates a bounded loaded-only scope. */
	UPROPERTY()
	TArray<FString> TargetPaths;

	UPROPERTY()
	bool bCheckLayout = true;

	UPROPERTY()
	bool bCheckAccessibility = true;

	UPROPERTY()
	bool bCheckAnimations = true;

	UPROPERTY()
	bool bCheckBindings = true;

	UPROPERTY()
	bool bCheckMVVM = true;

	UPROPERTY()
	int32 MaxIssues = 128;

	UPROPERTY()
	int32 DeadlineMs = 750;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIUIValidateReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bValid = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	UPROPERTY()
	FString PersistedFingerprint;

	UPROPERTY()
	FString VolatileObservationFingerprint;

	UPROPERTY()
	bool bSnapshotComplete = false;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	int32 LoadedObjectsScanned = 0;

	UPROPERTY()
	int32 ErrorCount = 0;

	UPROPERTY()
	int32 WarningCount = 0;

	UPROPERTY()
	int32 InfoCount = 0;

	UPROPERTY()
	TArray<FHyperAIUIIssue> Issues;
};

/** Closed operation; fields unused by Kind must retain their defaults. */
USTRUCT(BlueprintType)
struct FHyperAIUIPlanOperation
{
	GENERATED_BODY()

	/**
	 * Run for real: widget.add (subject_id = new name, secondary_name = parent panel or empty for the root,
	 * string_value = class: CanvasPanel, VerticalBox, Button, TextBlock, Image... or a Widget Blueprint path),
	 * widget.set_property (name: text, visibility, is_enabled, font_size, justification, image, percent, value,
	 * padding, render_opacity, render_scale, width_override...), widget.set_style (color_and_opacity, brush_color,
	 * background_color, fill_color_and_opacity...), widget.set_slot (padding, h_align, v_align, fill; in a canvas:
	 * anchors, offsets, position, size, alignment, auto_size, z_order), widget.set_navigation (name = up|down|left|
	 * right|next|previous|all, string_value = escape|stop|wrap|explicit, secondary_name = the widget to focus),
	 * animation.create, animation.add_float_track (name = a float property: RenderOpacity, Percent...),
	 * animation.add_transform_track (name = translation_x|translation_y|angle|scale_x|scale_y|shear_x|shear_y).
	 * Staged only: animation.rename|animation.delete, binding.create_legacy|update_legacy|replace_legacy|
	 * delete_legacy, binding.create_mvvm|update_mvvm|replace_mvvm|delete_mvvm.
	 */
	UPROPERTY()
	FString Kind;

	/** Widget name, animation name, or stable binding id depending on Kind. */
	UPROPERTY()
	FString SubjectId;

	/** Staged-only ops: the exact per-element fingerprint when updating, replacing, renaming, or deleting. Leave empty on ops that run. */
	UPROPERTY()
	FString ExpectedElementFingerprint;

	/** New animation name, property/style/slot id, navigation direction, float property or transform channel. */
	UPROPERTY()
	FString Name;

	/** Animation name for tracks, parent panel for widget.add, focus target for explicit navigation, legacy event/property, or MVVM metadata. */
	UPROPERTY()
	FString SecondaryName;

	UPROPERTY()
	FString SourceEndpoint;

	UPROPERTY()
	FString SourcePath;

	UPROPERTY()
	FString DestinationEndpoint;

	UPROPERTY()
	FString DestinationPath;

	/** The value as text: "Play", "center", "10,20", "#FFAA00", "0.5", an asset path, or animation keys "0=0, 0.3=1". */
	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	bool bHasBoolValue = false;

	UPROPERTY()
	bool bBoolValue = false;

	UPROPERTY()
	bool bHasNumberValue = false;

	UPROPERTY()
	double NumberValue = 0.0;

	UPROPERTY()
	bool bHasColorValue = false;

	UPROPERTY()
	FLinearColor ColorValue = FLinearColor::Transparent;

	UPROPERTY()
	int32 StartFrame = 0;

	UPROPERTY()
	int32 EndFrame = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIUIApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDryRun = true;

	/** Required for a non-dry retry envelope; no mutation or stage is submitted by this source candidate. */
	UPROPERTY()
	FString OperationId;

	/** Exact plan hash returned by dry-run. */
	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	FString TargetPath;

	/** Exact persisted snapshot CAS. */
	UPROPERTY()
	FString ExpectedPersistedFingerprint;

	/** structural, layout_accessibility, or compile_ready. */
	UPROPERTY()
	FString ValidationPolicy = TEXT("layout_accessibility");

	UPROPERTY()
	TArray<FHyperAIUIPlanOperation> Operations;

	UPROPERTY()
	int32 DeadlineMs = 1000;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIUIPlanEffects
{
	GENERATED_BODY()

	UPROPERTY()
	int32 OperationCount = 0;

	UPROPERTY()
	int32 AnimationCreates = 0;

	UPROPERTY()
	int32 AnimationEdits = 0;

	UPROPERTY()
	int32 AnimationDeletes = 0;

	UPROPERTY()
	int32 PropertyEdits = 0;

	UPROPERTY()
	int32 StyleEdits = 0;

	UPROPERTY()
	int32 BindingCreates = 0;

	UPROPERTY()
	int32 BindingEdits = 0;

	UPROPERTY()
	int32 BindingDeletesOrReplaces = 0;

	UPROPERTY()
	int32 WidgetAdds = 0;

	UPROPERTY()
	int32 SlotEdits = 0;

	UPROPERTY()
	int32 NavigationEdits = 0;

	UPROPERTY()
	bool bWouldSaveOnce = false;

	UPROPERTY()
	bool bWouldCompileOnce = false;

	UPROPERTY()
	bool bWouldValidateOnce = false;

	UPROPERTY()
	bool bWouldVerifyFreshOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIUIApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	bool bTypedPrepared = false;

	UPROPERTY()
	bool bStaged = false;

	UPROPERTY()
	bool bExecutionSubmitted = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString SafetyClass;

	UPROPERTY()
	FString VariantId;

	UPROPERTY()
	FString BasePersistedFingerprint;

	UPROPERTY()
	FString DesiredPersistedFingerprint;

	UPROPERTY()
	FString SemanticFingerprint;

	UPROPERTY()
	FString PlanHash;

	UPROPERTY()
	FString AuthorizationPlanHash;

	UPROPERTY()
	FString CapabilityHash;

	UPROPERTY()
	FString EffectFingerprint;

	UPROPERTY()
	FHyperAIUIPlanEffects Effects;

	UPROPERTY()
	TArray<FHyperAIUIIssue> Issues;
};

/** hyper_ui_apply_plan's reply, held open until the edit it submitted has finished: one call instead of an apply and a wait. */
UCLASS()
class UHyperAIUIApplyPlanResult final : public UToolCallAsyncResult
{
	GENERATED_BODY()

public:
	bool SetValue(const FHyperAIUIApplyPlanReport& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(FHyperAIUIApplyPlanReport(InValue), Value);
	}

	UPROPERTY()
	FHyperAIUIApplyPlanReport Value;
};

USTRUCT(BlueprintType)
struct FHyperAIUIRenderReviewRequest
{
	GENERATED_BODY()

	/** The Widget Blueprint, an object path such as /Game/UI/WBP_MainMenu.WBP_MainMenu. */
	UPROPERTY() FString TargetPath;
	/** Render the instance playing in Play In Editor (its live state) rather than a fresh design-time one. */
	UPROPERTY() bool bFromPlay = false;
	/** Screen sizes as "WxH": 1920x1080 by default; add 2560x1440 and 1280x720 to check scaling and small screens. At most 4. */
	UPROPERTY() TArray<FString> Resolutions;
	/** UI scale; 0 (the default) uses the project's DPI curve for each resolution, as the game would. */
	UPROPERTY() float DpiScale = 0.f;
	/** Share of each screen axis inside the title-safe area: 0.9 by default (TVs can crop the outer 5% each side); 1 checks only the screen edge. */
	UPROPERTY() float SafeZone = 0.9f;
	/** Text whose glyphs come out smaller than this many pixels tall is too small: 14 by default. */
	UPROPERTY() float MinTextPixels = 14.f;
	/** Least contrast between text and what is drawn behind it (WCAG ratio): 4.5 by default; large text needs 3. */
	UPROPERTY() float MinContrast = 4.5f;
	/** What fills the screen behind the widget, sRGB: a dark grey by default. Set it near the game's view behind this UI. */
	UPROPERTY() FLinearColor Backdrop = FLinearColor(0.12f, 0.12f, 0.12f, 1.f);
	/** A mock-up or concept image inside the project to compare the first render against (hyper_image_compare, aligned). */
	UPROPERTY() FString ReferenceImagePath;
	/** Score (0-100) the render must reach against the reference: 80 by default. */
	UPROPERTY() float TargetScore = 80.f;
	/** Keeps a polish loop's compares together, as hyper_image_compare's session_id: letters, digits, - and _. */
	UPROPERTY() FString SessionId;
	/** What changed since the last render, kept in the compare session's history. */
	UPROPERTY() FString Note;
	/** Issues listed: 64 by default, 1 to 256. */
	UPROPERTY() int32 MaxIssues = 64;
};

USTRUCT(BlueprintType)
struct FHyperAIUIReviewIssue
{
	GENERATED_BODY()

	/**
	 * text_clipped, text_too_small, low_contrast, offscreen, outside_safe_zone, clipped_by_parent, overlapping_interactive,
	 * not_focusable, broken_navigation or no_initial_focus.
	 */
	UPROPERTY() FString Code;
	/** error or warning. */
	UPROPERTY() FString Severity;
	/** The widget's path in the tree, such as Root/Menu/PlayButton. */
	UPROPERTY() FString Widget;
	/** The render it was seen in, "1920x1080"; empty for issues of the tree itself (focus, navigation). */
	UPROPERTY() FString Resolution;
	/** Where it is in that render, pixels: "x,y wxh". */
	UPROPERTY() FString Rect;
	UPROPERTY() FString Message;
};

USTRUCT(BlueprintType)
struct FHyperAIUIRender
{
	GENERATED_BODY()

	UPROPERTY() FString Resolution;
	UPROPERTY() float DpiScale = 1.f;
	/** The PNG, full size. Open it to look at it. */
	UPROPERTY() FString ImagePath;
	/** Widgets drawn and measured. */
	UPROPERTY() int32 Widgets = 0;
	UPROPERTY() int32 Issues = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIUIReferenceMatch
{
	GENERATED_BODY()

	UPROPERTY() bool bCompared = false;
	/** 0-100, hyper_image_compare's aligned score of the first render against the reference. */
	UPROPERTY() float Score = 0.f;
	UPROPERTY() float TargetScore = 0.f;
	UPROPERTY() bool bMatched = false;
	/** Where they differ, brightest where most: open it. */
	UPROPERTY() FString HeatMapPath;
	UPROPERTY() FString SideBySidePath;
	/** The cells that differ most, worst first: "top left: darker by 0.8 EV, less detail". */
	UPROPERTY() TArray<FString> WorstRegions;
	UPROPERTY() TArray<FString> Suggestions;
	UPROPERTY() FString SessionId;
	/** Why there is no compare, when there is none. */
	UPROPERTY() FString Diagnostic;
};

USTRUCT(BlueprintType)
struct FHyperAIUIRenderReviewReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** reviewed, invalid_request, not_found, not_compiled, cannot_render or no_pie_world. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	/** design_time (a fresh instance) or play (the instance in Play In Editor). */
	UPROPERTY() FString Source;
	UPROPERTY() TArray<FHyperAIUIRender> Renders;
	/** Errors first. */
	UPROPERTY() TArray<FHyperAIUIReviewIssue> Issues;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FHyperAIUIReferenceMatch Reference;
	UPROPERTY() TArray<FString> Notes;
};

/** The UI cohort: inspect, plan and validate Widget Blueprints, and render them full size to review them. */
UCLASS()
class HYPERAISTUDIOUI_API UHyperAIStudioUIToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Loaded Widget Blueprints: widget tree, layout, animations, legacy bindings and MVVM, with persisted fingerprints. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|UI")
	static FHyperAIUIInspectReport hyper_ui_inspect(const FHyperAIUIInspectRequest& Request);

	/**
	 * Build or change a Widget Blueprint in one undo step, compiled and saved: add widgets to the tree, set their
	 * properties, styles and slot layout, give them navigation, and animate them (animation.create, float and transform
	 * tracks). Submit with bDryRun false and an operation_id; the reply waits for the edit. Renames, deletes and
	 * bindings only dry-run.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|UI")
	static UHyperAIUIApplyPlanResult* hyper_ui_apply_plan(const FHyperAIUIApplyPlanRequest& Request);

	/** Layout, accessibility, animation, binding and MVVM checks of loaded Widget Blueprints. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|UI")
	static FHyperAIUIValidateReport hyper_ui_validate(const FHyperAIUIValidateRequest& Request);

	/**
	 * Render a Widget Blueprint off screen at full size (1920x1080 by default; also 2560x1440, 1280x720, at the project's
	 * DPI scale or a chosen one), or its instance playing in Play In Editor, write each as a PNG, and review the layout:
	 * clipped text (desired against allotted size), text too small or too low in contrast against what is drawn behind
	 * it, widgets off screen or outside the safe zone or cut off by a clipping parent, overlapping buttons, and buttons
	 * that cannot take focus or navigate. With reference_image_path, also scores the render against a mock-up and
	 * writes a heat map. Read-only: nothing in the project changes.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|UI")
	static FHyperAIUIRenderReviewReport hyper_ui_render_review(const FHyperAIUIRenderReviewRequest& Request);
};

enum class EHyperAIStudioUIPlanSafety : uint8
{
	Edit,
	Destructive
};

enum class EHyperAIStudioUIOperationKind : uint8
{
	AnimationCreate,
	AnimationRename,
	AnimationDelete,
	WidgetSetProperty,
	WidgetSetStyle,
	BindingCreateLegacy,
	BindingUpdateLegacy,
	BindingReplaceLegacy,
	BindingDeleteLegacy,
	BindingCreateMVVM,
	BindingUpdateMVVM,
	BindingReplaceMVVM,
	BindingDeleteMVVM
};

struct FHyperAIStudioUIBackendOperation
{
	EHyperAIStudioUIOperationKind Kind = EHyperAIStudioUIOperationKind::WidgetSetProperty;
	EHyperAIStudioUIPlanSafety Safety = EHyperAIStudioUIPlanSafety::Edit;
	FString SubjectId;
	FString ExpectedElementFingerprint;
	FString Name;
	FString SecondaryName;
	FString SourceEndpoint;
	FString SourcePath;
	FString DestinationEndpoint;
	FString DestinationPath;
	FString StringValue;
	bool bHasBoolValue = false;
	bool bBoolValue = false;
	bool bHasNumberValue = false;
	double NumberValue = 0.0;
	bool bHasColorValue = false;
	FLinearColor ColorValue = FLinearColor::Transparent;
	int32 StartFrame = 0;
	int32 EndFrame = 0;
};

/** Deep-copyable closed payload used only by the public pure typed-artifact sealer. */
class FHyperAIStudioUIPlanPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BasePersistedFingerprint;
	FString DesiredPersistedFingerprint;
	FString ValidationPolicy;
	FString SemanticFingerprint;
	EHyperAIStudioUIPlanSafety Safety = EHyperAIStudioUIPlanSafety::Edit;
	TArray<FHyperAIStudioUIBackendOperation> Operations;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override { return SemanticFingerprint; }
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

struct FHyperAIStudioUIManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioUIContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("ui_slate_mvvm");
	static constexpr const TCHAR* RequiredModuleType = TEXT("Editor");
	static constexpr const TCHAR* RequiredLoadingPhase = TEXT("None");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiouitoolset.v1");
	static constexpr const TCHAR* PluginRequirementGroupId = TEXT("ui_plugin");
	static constexpr const TCHAR* BackendRequirementGroupId = TEXT("ui_backend");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.widget_blueprint_editor");
	static constexpr const TCHAR* EditVariantId = TEXT("ui.apply_edit_plan.v1");
	static constexpr const TCHAR* DestructiveVariantId = TEXT("ui.apply_destructive_plan.v1");
	static constexpr const TCHAR* PayloadTypeId = TEXT("hyperai.payload.ui.compound-plan.v1");
	static constexpr const TCHAR* NonDryCallableState = TEXT("staged_backend_required");
	static constexpr int32 MaxTargetPaths = 32;
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxTextCharacters = 4096;
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxLoadedBlueprintsScanned = 256;
	static constexpr int32 MaxRawObjectSlotsScanned = 8192;
	static constexpr int32 MaxWidgetsPerBlueprint = 512;
	static constexpr int32 MaxChildrenPerPanel = 256;
	static constexpr int32 MaxNamedSlotBindings = 128;
	static constexpr int32 MaxAnimationsPerBlueprint = 128;
	static constexpr int32 MaxAnimationBindings = 256;
	static constexpr int32 MaxMovieSceneBindings = 256;
	static constexpr int32 MaxTracksPerBinding = 128;
	static constexpr int32 MaxLegacyBindings = 256;
	static constexpr int32 MaxMVVMViewModels = 128;
	static constexpr int32 MaxMVVMBindings = 256;
	static constexpr int32 MaxMVVMPathSegments = 32;
	static constexpr int32 MaxRecords = 4096;
	static constexpr int32 MaxIssues = 256;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxCursorCharacters = 256;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MaxCaptureCanonicalCharacters = 512 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioUIManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static FString ClassifyAssetRegistryExistence(int32 StateValue);
	static bool DecodeCursor(
		const FString& Cursor,
		const FString& RequestFingerprint,
		const FString& PersistedFingerprint,
		int32& OutOffset);
	static FString EncodeCursor(
		int32 Offset,
		const FString& RequestFingerprint,
		const FString& PersistedFingerprint);
	static bool ClassifyOperation(
		const FString& Kind,
		EHyperAIStudioUIOperationKind& OutKind,
		EHyperAIStudioUIPlanSafety& OutSafety);
	static bool ValidateOperationShape(
		const FHyperAIUIPlanOperation& Operation,
		FHyperAIStudioUIBackendOperation& OutOperation,
		FString& OutError);
	static FString PayloadSchemaFingerprint();
	static FString ComputePayloadSemanticFingerprint(const FHyperAIStudioUIPlanPayload& Payload);
	static const FHyperAIStudioDomainAdapterDescriptor& GetPreparationDescriptor();
	/** hyper_ui_apply_plan's plan with an agent's near misses fitted first; the tool then waits for the edit it submits. */
	static FHyperAIUIApplyPlanReport BuildFittedPlan(const FHyperAIUIApplyPlanRequest& Request);
	static FHyperAIUIApplyPlanReport BuildPlan(const FHyperAIUIApplyPlanRequest& Request);
};

class FHyperAIStudioUIAnimationAdapter;

/** The widget animation adapter and the widget blueprint editor probe. Best effort: the read tools work without them. */
class FHyperAIStudioUIAnimationBackend final
{
public:
	void Register();
	void Unregister();

private:
	TSharedPtr<FHyperAIStudioUIAnimationAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher ProbePublisher;
};

class FHyperAIStudioUIRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;
	bool HasLiveOwnership() const { return bOwnsToolset; }

private:
	void RegisterAfterEngineInit();
	void RollBackRegistration();

	FDelegateHandle PostEngineInitHandle;
	FHyperAIStudioUIAnimationBackend AnimationBackend;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
