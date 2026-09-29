// Games by Hyper 2026.

#include "HyperAIStudioUIDelegationMatrix.h"
#include "HyperAIStudioUIToolset.h"
#include "HyperAIStudioUIValueModel.h"

#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetNavigation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Internationalization/Text.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioTrustedExecution.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "MovieScene.h"
#include "ObjectTools.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "WidgetBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::UI::Tests
{
	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	FHyperAIUIRecord BlueprintRecord()
	{
		FHyperAIUIRecord Record;
		Record.Kind = TEXT("blueprint");
		Record.RecordKey = TEXT("blueprint:/Game/UI/W_Test.W_Test");
		Record.BlueprintPath = TEXT("/Game/UI/W_Test.W_Test");
		Record.StableId = Record.BlueprintPath;
		Record.Name = TEXT("W_Test");
		Record.ClassPath = TEXT("/Script/UMGEditor.WidgetBlueprint");
		return Record;
	}

	FHyperAIUIRecord RootWidgetRecord()
	{
		FHyperAIUIRecord Record;
		Record.Kind = TEXT("widget");
		Record.RecordKey = TEXT("widget:/Game/UI/W_Test.W_Test:name:Title");
		Record.BlueprintPath = TEXT("/Game/UI/W_Test.W_Test");
		Record.StableId = TEXT("name:Title");
		Record.Name = TEXT("Title");
		Record.ClassPath = TEXT("/Script/UMG.TextBlock");
		FHyperAIUIFieldValue& Text = Record.Fields.AddDefaulted_GetRef();
		Text.Id = TEXT("text"); Text.Type = TEXT("string"); Text.StringValue = TEXT("Old");
		FHyperAIUIFieldValue& Enabled = Record.Fields.AddDefaulted_GetRef();
		Enabled.Id = TEXT("is_enabled"); Enabled.Type = TEXT("bool"); Enabled.bBoolValue = true;
		FHyperAIUIFieldValue& Visibility = Record.Fields.AddDefaulted_GetRef();
		Visibility.Id = TEXT("visibility"); Visibility.Type = TEXT("enum");
		Visibility.StringValue = TEXT("visible");
		return Record;
	}

	FHyperAIStudioUIValueSnapshot BaseSnapshot()
	{
		FHyperAIStudioUIValueSnapshot Snapshot;
		Snapshot.Records = {BlueprintRecord(), RootWidgetRecord()};
		FString Error;
		FHyperAIStudioUIValueContracts::ComputeFingerprints(Snapshot, Error);
		return Snapshot;
	}

	TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe> Payload(
		const EHyperAIStudioUIPlanSafety Safety)
	{
		const TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe> Value =
			MakeShared<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe>();
		Value->TargetPath = TEXT("/Game/UI/W_Test.W_Test");
		Value->BasePersistedFingerprint = Hash(TEXT("base"));
		Value->DesiredPersistedFingerprint = Hash(TEXT("desired"));
		Value->ValidationPolicy = TEXT("structural");
		Value->Safety = Safety;
		FHyperAIStudioUIBackendOperation& Operation = Value->Operations.AddDefaulted_GetRef();
		Operation.Kind = Safety == EHyperAIStudioUIPlanSafety::Destructive
			? EHyperAIStudioUIOperationKind::AnimationDelete
			: EHyperAIStudioUIOperationKind::WidgetSetProperty;
		Operation.Safety = Safety;
		Operation.SubjectId = TEXT("subject");
		Operation.ExpectedElementFingerprint = Hash(TEXT("element"));
		if (Safety == EHyperAIStudioUIPlanSafety::Edit)
		{
			Operation.Name = TEXT("text");
			Operation.StringValue = TEXT("value");
		}
		Value->SemanticFingerprint =
			FHyperAIStudioUIContracts::ComputePayloadSemanticFingerprint(*Value);
		return Value;
	}

	bool Prepare(
		const TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe>& PayloadValue,
		FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError)
	{
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
			FHyperAIStudioUIContracts::GetPreparationDescriptor();
		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = FHyperAIStudioUIContracts::PackId;
		Binding.ToolName = TEXT("hyper_ui_apply_plan");
		Binding.VariantId = PayloadValue->Safety == EHyperAIStudioUIPlanSafety::Destructive
			? FHyperAIStudioUIContracts::DestructiveVariantId
			: FHyperAIStudioUIContracts::EditVariantId;
		Binding.ExpectedSafety = PayloadValue->Safety == EHyperAIStudioUIPlanSafety::Destructive
			? EHyperAIStudioDomainSafety::Destructive : EHyperAIStudioDomainSafety::Edit;
		Binding.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
		Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Binding.ExpectedAdapterGeneration = 1;
		Binding.ExpectedRegistryEpoch = 1;
		Binding.Prerequisites.PackId = Binding.PackId;
		Binding.Prerequisites.bPackEnabled = true;
		Binding.Prerequisites.Revision = 1;
		Binding.Prerequisites.Observations = {
			{TEXT("module.UMG"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("plugin.ModelViewViewModel"), EHyperAIStudioDomainPrerequisiteState::Available},
			{FHyperAIStudioUIContracts::LiveProbeId,
				EHyperAIStudioDomainPrerequisiteState::Missing}};
		Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
		Binding.Admission.PackId = Binding.PackId;
		Binding.Admission.Revision = 1;
		Binding.Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);
		FHyperAIStudioTypedArtifactContract Contract;
		Contract.Binding = Binding;
		Contract.ArtifactTypeId = PayloadValue->GetTypeId();
		Contract.ArtifactSchemaFingerprint = PayloadValue->GetSchemaFingerprint();
		Contract.ArtifactSemanticFingerprint = PayloadValue->GetSemanticFingerprint();
		Contract.EffectTarget = TEXT("widget_blueprint:test");
		Contract.DeadlineMs = 1000;
		Contract.MaxNativeOperations = 8;
		Contract.MaxGameThreadMs = 200;
		Contract.MaxOutputBytes = 8192;
		Contract.MaxResultBytes = 256;
		Contract.StageLifetimeMs = 15000;
		Contract.bCompileOnce = true;
		Contract.bSaveOnce = true;
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		return FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, OutPrepared, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUIManifestAndDelegationTest,
	"HyperAIStudio.NativeTools.UI.ManifestAndEpicDelegation46",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUIManifestAndDelegationTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Exact pack id"), FString(FHyperAIStudioUIContracts::PackId),
		FString(TEXT("ui_slate_mvvm")));
	TestEqual(TEXT("Exact plugin requirement group"),
		FString(FHyperAIStudioUIContracts::PluginRequirementGroupId), FString(TEXT("ui_plugin")));
	TestEqual(TEXT("Exact backend requirement group"),
		FString(FHyperAIStudioUIContracts::BackendRequirementGroupId), FString(TEXT("ui_backend")));
	TestEqual(TEXT("Exact probe id"), FString(FHyperAIStudioUIContracts::LiveProbeId),
		FString(TEXT("probe.widget_blueprint_editor")));
	TestEqual(TEXT("Exact zero-effect non-dry blocker"),
		FString(FHyperAIStudioUIContracts::NonDryCallableState),
		FString(TEXT("staged_backend_required")));
	TestEqual(TEXT("Exact cohort id"), FString(FHyperAIStudioUIContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudiouitoolset.v1")));
	TestEqual(TEXT("Central module type handoff"),
		FString(FHyperAIStudioUIContracts::RequiredModuleType), FString(TEXT("Editor")));
	TestEqual(TEXT("Central loading phase handoff"),
		FString(FHyperAIStudioUIContracts::RequiredLoadingPhase), FString(TEXT("None")));
	const TArray<FHyperAIStudioUIManifestEntry>& Manifest = FHyperAIStudioUIContracts::GetManifest();
	TestEqual(TEXT("Exactly three UI callables"), Manifest.Num(), 3);
	TestEqual(TEXT("Inspect name"), Manifest[0].Name, FString(TEXT("hyper_ui_inspect")));
	TestEqual(TEXT("Apply name"), Manifest[1].Name, FString(TEXT("hyper_ui_apply_plan")));
	TestEqual(TEXT("Validate name"), Manifest[2].Name, FString(TEXT("hyper_ui_validate")));
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioUIContracts::GetPreparationDescriptor();
	TestEqual(TEXT("Two separately typed apply safety variants"), Descriptor.Variants.Num(), 2);
	TestEqual(TEXT("Edit descriptor safety"), static_cast<uint8>(Descriptor.Variants[0].Safety),
		static_cast<uint8>(EHyperAIStudioDomainSafety::Edit));
	TestEqual(TEXT("Destructive descriptor safety"),
		static_cast<uint8>(Descriptor.Variants[1].Safety),
		static_cast<uint8>(EHyperAIStudioDomainSafety::Destructive));
	TestEqual(TEXT("Edit variant id is exact"), Descriptor.Variants[0].VariantId,
		FString(FHyperAIStudioUIContracts::EditVariantId));
	TestEqual(TEXT("Destructive variant id is exact"), Descriptor.Variants[1].VariantId,
		FString(FHyperAIStudioUIContracts::DestructiveVariantId));
	TestEqual(TEXT("Blocking UI groups are absent from non-blocking authority"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	const FString RequestHash = HyperAIStudio::UI::Tests::Hash(TEXT("request"));
	const FString PersistedHash = HyperAIStudio::UI::Tests::Hash(TEXT("persisted"));
	const FString Cursor = FHyperAIStudioUIContracts::EncodeCursor(
		23, RequestHash, PersistedHash);
	int32 CursorOffset = -1;
	TestTrue(TEXT("Cursor opens only for exact request/persisted identity"),
		FHyperAIStudioUIContracts::DecodeCursor(
			Cursor, RequestHash, PersistedHash, CursorOffset));
	TestEqual(TEXT("Cursor offset is exact"), CursorOffset, 23);
	TestFalse(TEXT("Cursor rejects persisted drift"),
		FHyperAIStudioUIContracts::DecodeCursor(
			Cursor, RequestHash, HyperAIStudio::UI::Tests::Hash(TEXT("drift")), CursorOffset));

	const TConstArrayView<FHyperAIStudioUIEpicDelegation> Matrix =
		FHyperAIStudioUIEpicDelegationMatrix::Get();
	TestEqual(TEXT("Exact 46/46 matrix"), Matrix.Num(), 46);
	int32 UMG = 0;
	int32 MVVM = 0;
	int32 Slate = 0;
	TSet<FString> Coordinates;
	for (const FHyperAIStudioUIEpicDelegation& Entry : Matrix)
	{
		const FString Source = Entry.SourceToolset;
		if (Source == TEXT("UMGToolSet")) ++UMG;
		else if (Source == TEXT("MVVMToolset")) ++MVVM;
		else if (Source == TEXT("SlateInspectorToolset")) ++Slate;
		const FString Coordinate = Source + TEXT(":") + Entry.CallableName;
		TestFalse(TEXT("No duplicate Epic coordinate"), Coordinates.Contains(Coordinate));
		Coordinates.Add(Coordinate);
		if (FString(Entry.CallableName) == TEXT("ListWidgetViewModels")
			|| FString(Entry.CallableName) == TEXT("ListWidgetViewBindings"))
		{
			TestEqual(TEXT("MVVM ListWidget lifecycle is edit"),
				FString(Entry.Lifecycle), FString(TEXT("edit")));
			TestEqual(TEXT("MVVM ListWidget access is edit"),
				FString(Entry.Access), FString(TEXT("edit")));
		}
	}
	TestEqual(TEXT("23 UMG delegates"), UMG, 23);
	TestEqual(TEXT("9 MVVM delegates"), MVVM, 9);
	TestEqual(TEXT("14 Slate delegates"), Slate, 14);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUISafetyShapeTest,
	"HyperAIStudio.NativeTools.UI.ClosedShapesAndSafetyAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUISafetyShapeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::UI::Tests;
	const TArray<FString> EditKinds = {TEXT("animation.create"), TEXT("animation.rename"),
		TEXT("widget.set_property"), TEXT("widget.set_style"), TEXT("binding.create_legacy"),
		TEXT("binding.update_legacy"), TEXT("binding.create_mvvm"), TEXT("binding.update_mvvm")};
	const TArray<FString> DestructiveKinds = {TEXT("animation.delete"),
		TEXT("binding.replace_legacy"), TEXT("binding.delete_legacy"),
		TEXT("binding.replace_mvvm"), TEXT("binding.delete_mvvm")};
	for (const FString& Kind : EditKinds)
	{
		EHyperAIStudioUIOperationKind Parsed;
		EHyperAIStudioUIPlanSafety Safety;
		TestTrue(TEXT("Edit kind parses"), FHyperAIStudioUIContracts::ClassifyOperation(Kind, Parsed, Safety));
		TestEqual(TEXT("Edit kind cannot become read/destructive"),
			static_cast<uint8>(Safety), static_cast<uint8>(EHyperAIStudioUIPlanSafety::Edit));
	}
	for (const FString& Kind : DestructiveKinds)
	{
		EHyperAIStudioUIOperationKind Parsed;
		EHyperAIStudioUIPlanSafety Safety;
		TestTrue(TEXT("Destructive kind parses"), FHyperAIStudioUIContracts::ClassifyOperation(Kind, Parsed, Safety));
		TestEqual(TEXT("Delete/replace derives destructive"),
			static_cast<uint8>(Safety), static_cast<uint8>(EHyperAIStudioUIPlanSafety::Destructive));
	}
	FHyperAIUIPlanOperation Delete;
	Delete.Kind = TEXT("animation.delete");
	Delete.SubjectId = TEXT("Intro");
	Delete.ExpectedElementFingerprint = Hash(TEXT("animation"));
	FHyperAIStudioUIBackendOperation Backend;
	FString Error;
	TestTrue(TEXT("Exact delete shape accepted"),
		FHyperAIStudioUIContracts::ValidateOperationShape(Delete, Backend, Error));
	TestEqual(TEXT("Delete backend remains destructive"), static_cast<uint8>(Backend.Safety),
		static_cast<uint8>(EHyperAIStudioUIPlanSafety::Destructive));
	Delete.ExpectedElementFingerprint.Reset();
	TestFalse(TEXT("Delete cannot omit per-element CAS"),
		FHyperAIStudioUIContracts::ValidateOperationShape(Delete, Backend, Error));

	FHyperAIUIPlanOperation Replace;
	Replace.Kind = TEXT("binding.replace_legacy");
	Replace.SubjectId = TEXT("binding_id");
	Replace.ExpectedElementFingerprint = Hash(TEXT("binding"));
	Replace.Name = TEXT("Title");
	Replace.SecondaryName = TEXT("Text");
	Replace.SourcePath = TEXT("GetTitle");
	TestTrue(TEXT("Exact replacement shape accepted"),
		FHyperAIStudioUIContracts::ValidateOperationShape(Replace, Backend, Error));
	TestEqual(TEXT("Replacement backend remains destructive"), static_cast<uint8>(Backend.Safety),
		static_cast<uint8>(EHyperAIStudioUIPlanSafety::Destructive));
	Replace.bHasBoolValue = true;
	TestFalse(TEXT("Replacement rejects unused scalar fields"),
		FHyperAIStudioUIContracts::ValidateOperationShape(Replace, Backend, Error));
	FHyperAIUIPlanOperation ArbitraryWidget;
	ArbitraryWidget.Kind = TEXT("widget.set_property");
	ArbitraryWidget.SubjectId = TEXT("arbitrary/path");
	ArbitraryWidget.ExpectedElementFingerprint = Hash(TEXT("widget"));
	ArbitraryWidget.Name = TEXT("text");
	ArbitraryWidget.StringValue = TEXT("value");
	TestFalse(TEXT("Widget edits require exact name:/guid: stable ids"),
		FHyperAIStudioUIContracts::ValidateOperationShape(ArbitraryWidget, Backend, Error));
	FHyperAIUIPlanOperation HiddenScalar = Delete;
	HiddenScalar.ExpectedElementFingerprint = Hash(TEXT("animation"));
	HiddenScalar.NumberValue = 0.5;
	TestFalse(TEXT("Inactive union members must retain exact defaults"),
		FHyperAIStudioUIContracts::ValidateOperationShape(HiddenScalar, Backend, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUIFingerprintsAndValidatorTest,
	"HyperAIStudio.NativeTools.UI.PersistedVolatileFingerprintsAndIndependentValidator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUIFingerprintsAndValidatorTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::UI::Tests;
	FHyperAIStudioUIValueSnapshot Snapshot = BaseSnapshot();
	FHyperAIUIRecord Volatile;
	Volatile.Kind = TEXT("blueprint");
	Volatile.RecordKey = TEXT("volatile_blueprint:/Game/UI/W_Test.W_Test");
	Volatile.BlueprintPath = TEXT("/Game/UI/W_Test.W_Test");
	Volatile.StableId = TEXT("volatile");
	Volatile.Name = TEXT("W_Test");
	Volatile.bPersisted = false;
	FHyperAIUIFieldValue& Dirty = Volatile.Fields.AddDefaulted_GetRef();
	Dirty.Id = TEXT("package_dirty"); Dirty.Type = TEXT("bool"); Dirty.bBoolValue = false;
	Snapshot.Records.Add(Volatile);
	FString Error;
	TestTrue(TEXT("Fingerprints seal"),
		FHyperAIStudioUIValueContracts::ComputeFingerprints(Snapshot, Error));
	const FString Persisted = Snapshot.PersistedFingerprint;
	const FString VolatileBefore = Snapshot.VolatileObservationFingerprint;
	Snapshot.Records.FindByPredicate([](const FHyperAIUIRecord& Record)
	{
		return !Record.bPersisted;
	})->Fields[0].bBoolValue = true;
	TestTrue(TEXT("Drifted fingerprints seal"),
		FHyperAIStudioUIValueContracts::ComputeFingerprints(Snapshot, Error));
	TestEqual(TEXT("Volatile drift does not poison persisted CAS"),
		Snapshot.PersistedFingerprint, Persisted);
	TestNotEqual(TEXT("Volatile drift changes observation identity"),
		Snapshot.VolatileObservationFingerprint, VolatileBefore);

	FHyperAIStudioUIValueSnapshot Cycle;
	Cycle.Records.Add(BlueprintRecord());
	FHyperAIUIRecord A = RootWidgetRecord();
	A.StableId = TEXT("a"); A.Name = TEXT("A"); A.RecordKey = TEXT("widget:a"); A.ParentStableId = TEXT("b");
	FHyperAIUIRecord B = RootWidgetRecord();
	B.StableId = TEXT("b"); B.Name = TEXT("B"); B.RecordKey = TEXT("widget:b"); B.ParentStableId = TEXT("a");
	Cycle.Records.Add(A); Cycle.Records.Add(B);
	FHyperAIStudioUIValidationOptions Options;
	bool bTruncated = false;
	const TArray<FHyperAIUIIssue> Issues =
		FHyperAIStudioUIValueValidator::Validate(Cycle, Options, bTruncated);
	TestFalse(TEXT("Cycle validation is bounded"), bTruncated);
	TestTrue(TEXT("Cycle is found from detached values"), Issues.ContainsByPredicate(
		[](const FHyperAIUIIssue& Issue) { return Issue.Code == TEXT("widget_parent_cycle"); }));

	FHyperAIStudioUIValueSnapshot OrphanMVVM = BaseSnapshot();
	FHyperAIUIRecord& Binding = OrphanMVVM.Records.AddDefaulted_GetRef();
	Binding.Kind = TEXT("mvvm_binding");
	Binding.RecordKey = TEXT("mvvm_binding:/Game/UI/W_Test.W_Test:11111111111111111111111111111111");
	Binding.BlueprintPath = TEXT("/Game/UI/W_Test.W_Test");
	Binding.StableId = TEXT("11111111111111111111111111111111");
	Binding.Name = Binding.StableId;
	auto AddString = [&](const TCHAR* Id, const TCHAR* Value)
	{
		FHyperAIUIFieldValue& Field = Binding.Fields.AddDefaulted_GetRef();
		Field.Id = Id; Field.Type = TEXT("string"); Field.StringValue = Value;
	};
	AddString(TEXT("source_endpoint"), TEXT("self"));
	AddString(TEXT("source_path"), TEXT("Title"));
	AddString(TEXT("destination_endpoint"), TEXT("widget:Title"));
	AddString(TEXT("destination_path"), TEXT("Text"));
	bTruncated = false;
	const TArray<FHyperAIUIIssue> OrphanIssues =
		FHyperAIStudioUIValueValidator::Validate(OrphanMVVM, Options, bTruncated);
	TestTrue(TEXT("MVVM values cannot fabricate an absent extension/view"),
		OrphanIssues.ContainsByPredicate([](const FHyperAIUIIssue& Issue)
		{
			return Issue.Code == TEXT("mvvm_view_inventory_missing");
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUIShadowClonePrepareTest,
	"HyperAIStudio.NativeTools.UI.ShadowReplayImmutableCloneAndPurePrepare",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUIShadowClonePrepareTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::UI::Tests;
	FHyperAIStudioUIValueSnapshot Base = BaseSnapshot();
	FHyperAIUIPlanOperation Operation;
	Operation.Kind = TEXT("widget.set_property");
	Operation.SubjectId = TEXT("name:Title");
	Operation.ExpectedElementFingerprint =
		FHyperAIStudioUIValueContracts::ComputeRecordFingerprint(Base.Records[1]);
	Operation.Name = TEXT("text");
	Operation.StringValue = TEXT("New");
	FHyperAIStudioUIBackendOperation Backend;
	FString Error;
	TestTrue(TEXT("Text operation has a closed shape"),
		FHyperAIStudioUIContracts::ValidateOperationShape(Operation, Backend, Error));
	FHyperAIStudioUIValueSnapshot Desired;
	FHyperAIUIPlanEffects Effects;
	TArray<FHyperAIUIIssue> Issues;
	TestTrue(TEXT("Shadow replay succeeds without UObject access"),
		FHyperAIStudioUIValueContracts::ReplayShadowPlan(
			Base, {Backend}, Desired, Effects, Issues, Error));
	TestNotEqual(TEXT("Desired fingerprint differs"),
		Desired.PersistedFingerprint, Base.PersistedFingerprint);
	TestEqual(TEXT("One property edit"), Effects.PropertyEdits, 1);
	TestTrue(TEXT("Lifecycle would compile once"), Effects.bWouldCompileOnce);
	TestTrue(TEXT("Lifecycle would save once"), Effects.bWouldSaveOnce);
	TestTrue(TEXT("Lifecycle would verify fresh once"), Effects.bWouldVerifyFreshOnce);

	const TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe> EditPayload =
		Payload(EHyperAIStudioUIPlanSafety::Edit);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		EditPayload->CloneImmutable();
	TestTrue(TEXT("Clone is detached"), &Clone.Get() != &EditPayload.Get());
	TestEqual(TEXT("Clone semantic identity"), Clone->GetSemanticFingerprint(),
		EditPayload->GetSemanticFingerprint());
	EditPayload->Operations[0].StringValue = TEXT("mutated-after-clone");
	TestNotEqual(TEXT("Clone does not alias nested operation strings"),
		static_cast<const FHyperAIStudioUIPlanPayload&>(Clone.Get()).Operations[0].StringValue,
		EditPayload->Operations[0].StringValue);

	FHyperAIStudioPreparedTypedArtifact EditPrepared;
	FString PrepareError;
	const TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe> PureEdit =
		Payload(EHyperAIStudioUIPlanSafety::Edit);
	TestTrue(TEXT("Public pure Prepare seals edit authority"),
		Prepare(PureEdit, EditPrepared, PrepareError));
	TestTrue(TEXT("Edit plan hash canonical"),
		FHyperAIStudioUIContracts::IsCanonicalSha256(EditPrepared.PlanHash));
	FHyperAIStudioPreparedTypedArtifact DestructivePrepared;
	const TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe> PureDestructive =
		Payload(EHyperAIStudioUIPlanSafety::Destructive);
	TestTrue(TEXT("Public pure Prepare seals destructive authority separately"),
		Prepare(PureDestructive, DestructivePrepared, PrepareError));
	TestNotEqual(TEXT("Edit and destructive authority hashes differ"),
		EditPrepared.AuthorizationPlanHash, DestructivePrepared.AuthorizationPlanHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUIWidgetAnimationTest,
	"HyperAIStudio.NativeTools.UI.WidgetAnimationEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUIWidgetAnimationTest::RunTest(const FString& Parameters)
{
	// A Widget Blueprint with a canvas and a border named Panel, built the way the widget blueprint factory does.
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString Name = TEXT("WBP_HyperAITest_") + Suffix;
	const FString Target = FString::Printf(TEXT("/Game/__HyperAIStudioTests/%s.%s"), *Name, *Name);
	UPackage* Package = CreatePackage(*(TEXT("/Game/__HyperAIStudioTests/") + Name));
	UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package,
		FName(*Name), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("the fixture Widget Blueprint exists"), Blueprint)) return false;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	const bool bOneShot = Settings->bAllowOneShotAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	Settings->bAllowOneShotAgentEdits = true;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bApproval;
		Settings->bAllowOneShotAgentEdits = bOneShot;
		ObjectTools::ForceDeleteObjects({Blueprint}, /*ShowConfirmation=*/false);
	};
	UCanvasPanel* Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Blueprint->WidgetTree->RootWidget = Root;
	UBorder* Panel = Blueprint->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Panel"));
	Root->AddChildToCanvas(Panel);
	FAssetRegistryModule::AssetCreated(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	FHyperAIUIPlanOperation Create;
	Create.Kind = TEXT("animation.create");
	Create.Name = TEXT("Pulse");
	Create.bHasNumberValue = true;
	Create.NumberValue = 1.0;
	FHyperAIUIPlanOperation Track;
	Track.Kind = TEXT("animation.add_float_track");
	Track.SubjectId = TEXT("Panel");
	Track.SecondaryName = TEXT("Pulse");
	Track.Name = TEXT("RenderOpacity");
	Track.StringValue = TEXT("0=0.3, 1=1");

	FHyperAIUIApplyPlanRequest Request;
	Request.TargetPath = Target;
	auto DryRun = [&](const TArray<FHyperAIUIPlanOperation>& Operations)
	{
		Request.Operations = Operations;
		return FHyperAIStudioUIContracts::BuildFittedPlan(Request);
	};
	auto Refuses = [&](const FHyperAIUIPlanOperation& Wrong, const TCHAR* Expected)
	{
		const FHyperAIUIApplyPlanReport Report = DryRun({Create, Wrong});
		TestTrue(*FString::Printf(TEXT("refused with '%s' (%s: %s)"), Expected, *Report.Status, *Report.Diagnostic),
			!Report.bOk && Report.Diagnostic.Contains(Expected));
	};
	FHyperAIUIPlanOperation Wrong = Track;
	Wrong.SubjectId = TEXT("Nope");
	Refuses(Wrong, TEXT("no widget named 'Nope'"));
	Wrong = Track;
	Wrong.Name = TEXT("ColorAndOpacity");
	Refuses(Wrong, TEXT("RenderOpacity"));
	Wrong = Track;
	Wrong.StringValue = TEXT("0=0.3, 2=1");
	Refuses(Wrong, TEXT("outside the animation"));
	Wrong = Track;
	Wrong.SecondaryName = TEXT("Missing");
	Refuses(Wrong, TEXT("no animation named"));
	const FHyperAIUIApplyPlanReport Planned = DryRun({Create, Track});
	TestEqual(*FString::Printf(TEXT("the plan dry-runs (%s)"), *Planned.Diagnostic), Planned.Status, FString(TEXT("planned")));

	// One call: no dry run, no hash.
	Request.Operations = {Create, Track};
	Request.bDryRun = false;
	Request.OperationId = TEXT("ui-animation-") + Suffix;
	const FHyperAIUIApplyPlanReport Submitted = FHyperAIStudioUIContracts::BuildFittedPlan(Request);
	if (!TestEqual(*FString::Printf(TEXT("the plan is submitted (%s)"), *Submitted.Diagnostic), Submitted.Status, FString(TEXT("submitted"))))
	{
		return false;
	}
	FHyperAIStudioTypedArtifactOperationStatus Status;
	FString StatusError;
	const double Deadline = FPlatformTime::Seconds() + 60.0;
	while (FPlatformTime::Seconds() < Deadline)
	{
		FTSTicker::GetCoreTicker().Tick(0.01f);
		if (FHyperAIStudioTrustedExecutionFacade::QueryStatus(Request.OperationId, Status, StatusError) && Status.bTerminal) break;
	}
	TestEqual(*FString::Printf(TEXT("the operation completes (%s)"), *Status.Diagnostic), Status.Status, FString(TEXT("completed")));

	UWidgetAnimation* Pulse = nullptr;
	for (UWidgetAnimation* Animation : Blueprint->Animations)
	{
		if (Animation && Animation->GetName() == TEXT("Pulse")) Pulse = Animation;
	}
	if (!TestNotNull(TEXT("the Pulse animation exists"), Pulse) || !TestNotNull(TEXT("with a movie scene"), Pulse->GetMovieScene())) return false;
	const UMovieScene* Scene = Pulse->GetMovieScene();
	const TRange<FFrameNumber> Range = Scene->GetPlaybackRange();
	const double Seconds = Scene->GetTickResolution().AsSeconds(FFrameTime(Range.GetUpperBoundValue() - Range.GetLowerBoundValue()));
	TestTrue(*FString::Printf(TEXT("it lasts a second (%.3f)"), Seconds), FMath::IsNearlyEqual(Seconds, 1.0, 0.01));
	const UMovieSceneFloatSection* Section = nullptr;
	for (const FWidgetAnimationBinding& Binding : Pulse->AnimationBindings)
	{
		if (Binding.WidgetName != TEXT("Panel")) continue;
		for (UMovieSceneTrack* Each : Scene->FindTracks(UMovieSceneFloatTrack::StaticClass(), Binding.AnimationGuid))
		{
			const UMovieSceneFloatTrack* Float = Cast<UMovieSceneFloatTrack>(Each);
			if (Float && Float->GetPropertyName() == TEXT("RenderOpacity") && !Float->GetAllSections().IsEmpty())
			{
				Section = Cast<UMovieSceneFloatSection>(Float->GetAllSections()[0]);
			}
		}
	}
	if (!TestNotNull(TEXT("Panel.RenderOpacity is animated"), Section)) return false;
	float Start = -1.f;
	float End = -1.f;
	Section->GetChannel().Evaluate(FFrameTime(Range.GetLowerBoundValue()), Start);
	Section->GetChannel().Evaluate(FFrameTime(Range.GetUpperBoundValue()), End);
	TestTrue(*FString::Printf(TEXT("opacity runs 0.3 to 1 (%.2f to %.2f)"), Start, End), FMath::IsNearlyEqual(Start, 0.3f, 0.01f) && FMath::IsNearlyEqual(End, 1.f, 0.01f));
	TestFalse(TEXT("the Widget Blueprint is saved"), Blueprint->GetOutermost()->IsDirty());
	Request.OperationId = TEXT("ui-animation-again-") + Suffix;
	TestEqual(TEXT("a second Pulse is refused"), FHyperAIStudioUIContracts::BuildFittedPlan(Request).Status, FString(TEXT("invalid_op")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUIWidgetTreeTest,
	"HyperAIStudio.NativeTools.UI.WidgetTreeLayoutNavigationEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUIWidgetTreeTest::RunTest(const FString& Parameters)
{
	// An empty Widget Blueprint, as CreateBlueprint leaves it: no root yet. One plan builds a main menu on it.
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString Name = TEXT("WBP_HyperAIMenu_") + Suffix;
	const FString Target = FString::Printf(TEXT("/Game/__HyperAIStudioTests/%s.%s"), *Name, *Name);
	UPackage* Package = CreatePackage(*(TEXT("/Game/__HyperAIStudioTests/") + Name));
	UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package,
		FName(*Name), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("the fixture Widget Blueprint exists"), Blueprint)) return false;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	const bool bOneShot = Settings->bAllowOneShotAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	Settings->bAllowOneShotAgentEdits = true;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bApproval;
		Settings->bAllowOneShotAgentEdits = bOneShot;
		ObjectTools::ForceDeleteObjects({Blueprint}, /*ShowConfirmation=*/false);
	};
	FAssetRegistryModule::AssetCreated(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	auto Op = [](const TCHAR* Kind, const TCHAR* Subject, const TCHAR* OpName = TEXT(""), const TCHAR* Value = TEXT(""), const TCHAR* Secondary = TEXT(""))
	{
		FHyperAIUIPlanOperation Result;
		Result.Kind = Kind;
		Result.SubjectId = Subject;
		Result.Name = OpName;
		Result.StringValue = Value;
		Result.SecondaryName = Secondary;
		return Result;
	};
	FHyperAIUIPlanOperation Intro = Op(TEXT("animation.create"), TEXT(""), TEXT("Intro"));
	Intro.bHasNumberValue = true;
	Intro.NumberValue = 0.5;
	const TArray<FHyperAIUIPlanOperation> Plan = {
		Op(TEXT("widget.add"), TEXT("Root"), TEXT(""), TEXT("CanvasPanel")),
		Op(TEXT("widget.add"), TEXT("Menu"), TEXT(""), TEXT("VerticalBox"), TEXT("Root")),
		Op(TEXT("widget.set_slot"), TEXT("Menu"), TEXT("anchors"), TEXT("center")),
		Op(TEXT("widget.set_slot"), TEXT("Menu"), TEXT("alignment"), TEXT("0.5,0.5")),
		Op(TEXT("widget.set_slot"), TEXT("Menu"), TEXT("auto_size"), TEXT("true")),
		Op(TEXT("widget.add"), TEXT("Title"), TEXT(""), TEXT("TextBlock"), TEXT("Menu")),
		Op(TEXT("widget.set_property"), TEXT("Title"), TEXT("text"), TEXT("AETHERFALL")),
		Op(TEXT("widget.set_property"), TEXT("Title"), TEXT("font_size"), TEXT("48")),
		Op(TEXT("widget.set_property"), TEXT("Title"), TEXT("justification"), TEXT("center")),
		Op(TEXT("widget.set_property"), TEXT("Title"), TEXT("visibility"), TEXT("hit_test_invisible")),
		Op(TEXT("widget.set_style"), TEXT("Title"), TEXT("color_and_opacity"), TEXT("#E8C170")),
		Op(TEXT("widget.add"), TEXT("PlayButton"), TEXT(""), TEXT("Button"), TEXT("Menu")),
		Op(TEXT("widget.set_slot"), TEXT("PlayButton"), TEXT("padding"), TEXT("0,12")),
		Op(TEXT("widget.set_style"), TEXT("PlayButton"), TEXT("background_color"), TEXT("0.1,0.1,0.12,1")),
		Op(TEXT("widget.add"), TEXT("PlayLabel"), TEXT(""), TEXT("TextBlock"), TEXT("PlayButton")),
		Op(TEXT("widget.set_property"), TEXT("PlayLabel"), TEXT("text"), TEXT("Play")),
		Op(TEXT("widget.add"), TEXT("QuitButton"), TEXT(""), TEXT("Button"), TEXT("Menu")),
		Op(TEXT("widget.set_slot"), TEXT("QuitButton"), TEXT("h_align"), TEXT("fill")),
		Op(TEXT("widget.add"), TEXT("QuitLabel"), TEXT(""), TEXT("TextBlock"), TEXT("QuitButton")),
		Op(TEXT("widget.set_property"), TEXT("QuitLabel"), TEXT("text"), TEXT("Quit")),
		Op(TEXT("widget.set_property"), TEXT("PlayButton"), TEXT("accessible_text"), TEXT("Start playing")),
		Op(TEXT("widget.set_property"), TEXT("QuitButton"), TEXT("accessible_text"), TEXT("Quit to the desktop")),
		Op(TEXT("widget.set_navigation"), TEXT("PlayButton"), TEXT("down"), TEXT("explicit"), TEXT("QuitButton")),
		Op(TEXT("widget.set_navigation"), TEXT("QuitButton"), TEXT("up"), TEXT("explicit"), TEXT("PlayButton")),
		Op(TEXT("widget.set_navigation"), TEXT("QuitButton"), TEXT("down"), TEXT("wrap")),
		Intro,
		Op(TEXT("animation.add_float_track"), TEXT("Menu"), TEXT("RenderOpacity"), TEXT("0=0, 0.5=1"), TEXT("Intro")),
		Op(TEXT("animation.add_transform_track"), TEXT("Menu"), TEXT("translation_y"), TEXT("0=40, 0.5=0"), TEXT("Intro"))};

	FHyperAIUIApplyPlanRequest Request;
	Request.TargetPath = Target;
	auto DryRun = [&](const TArray<FHyperAIUIPlanOperation>& Operations)
	{
		Request.Operations = Operations;
		return FHyperAIStudioUIContracts::BuildFittedPlan(Request);
	};
	auto Refuses = [&](const FHyperAIUIPlanOperation& Wrong, const TCHAR* Expected)
	{
		// The root, the menu, the title, the play button and its label, then the wrong op.
		TArray<FHyperAIUIPlanOperation> Operations = {Plan[0], Plan[1], Plan[5], Plan[11], Plan[14], Wrong};
		const FHyperAIUIApplyPlanReport Report = DryRun(Operations);
		TestTrue(*FString::Printf(TEXT("refused with '%s' (%s: %s)"), Expected, *Report.Status, *Report.Diagnostic),
			!Report.bOk && Report.Diagnostic.Contains(Expected));
	};
	Refuses(Op(TEXT("widget.add"), TEXT("Inner"), TEXT(""), TEXT("Image"), TEXT("Title")), TEXT("holds no children"));
	Refuses(Op(TEXT("widget.add"), TEXT("Second"), TEXT(""), TEXT("TextBlock"), TEXT("PlayButton")), TEXT("holds one child"));
	Refuses(Op(TEXT("widget.set_property"), TEXT("PlayButton"), TEXT("text"), TEXT("Go")), TEXT("TextBlock inside it"));
	Refuses(Op(TEXT("widget.set_slot"), TEXT("Title"), TEXT("anchors"), TEXT("center")), TEXT("CanvasPanel"));
	Refuses(Op(TEXT("widget.set_navigation"), TEXT("PlayButton"), TEXT("down"), TEXT("explicit"), TEXT("Nowhere")), TEXT("explicit navigation needs"));
	Refuses(Op(TEXT("widget.add"), TEXT("Typo"), TEXT(""), TEXT("Buton"), TEXT("Menu")), TEXT("is not a widget class"));
	Refuses(Op(TEXT("widget.set_property"), TEXT("Title"), TEXT("justification"), TEXT("middle")), TEXT("left, center or right"));
	Refuses(Op(TEXT("widget.add"), TEXT("Title"), TEXT(""), TEXT("TextBlock"), TEXT("Menu")), TEXT("already has a widget"));
	Refuses(Op(TEXT("animation.add_transform_track"), TEXT("Title"), TEXT("wobble"), TEXT("0=0"), TEXT("Nope")), TEXT("no animation named"));
	const FHyperAIUIApplyPlanReport Planned = DryRun(Plan);
	TestEqual(*FString::Printf(TEXT("the whole menu dry-runs (%s)"), *Planned.Diagnostic), Planned.Status, FString(TEXT("planned")));
	TestEqual(TEXT("with its widgets counted"), Planned.Effects.WidgetAdds, 7);
	TestEqual(TEXT("and its navigation"), Planned.Effects.NavigationEdits, 3);

	Request.Operations = Plan;
	Request.bDryRun = false;
	Request.OperationId = TEXT("ui-menu-") + Suffix;
	const FHyperAIUIApplyPlanReport Submitted = FHyperAIStudioUIContracts::BuildFittedPlan(Request);
	if (!TestEqual(*FString::Printf(TEXT("the plan is submitted (%s)"), *Submitted.Diagnostic), Submitted.Status, FString(TEXT("submitted"))))
	{
		return false;
	}
	FHyperAIStudioTypedArtifactOperationStatus Status;
	FString StatusError;
	const double Deadline = FPlatformTime::Seconds() + 60.0;
	while (FPlatformTime::Seconds() < Deadline)
	{
		FTSTicker::GetCoreTicker().Tick(0.01f);
		if (FHyperAIStudioTrustedExecutionFacade::QueryStatus(Request.OperationId, Status, StatusError) && Status.bTerminal) break;
	}
	if (!TestEqual(*FString::Printf(TEXT("the operation completes (%s)"), *Status.Diagnostic), Status.Status, FString(TEXT("completed"))))
	{
		return false;
	}

	UWidgetTree* Tree = Blueprint->WidgetTree;
	const UCanvasPanel* Root = Cast<UCanvasPanel>(Tree->RootWidget);
	const UVerticalBox* Menu = Cast<UVerticalBox>(Tree->FindWidget(TEXT("Menu")));
	const UCanvasPanelSlot* MenuSlot = Menu ? Cast<UCanvasPanelSlot>(Menu->Slot) : nullptr;
	TestNotNull(TEXT("the root is a CanvasPanel"), Root);
	TestTrue(TEXT("the menu sits on it, anchored and pivoted in the middle, sized to its content"), Menu && Menu->GetParent() == Root && MenuSlot
		&& MenuSlot->GetAnchors().Minimum.Equals(FVector2D(0.5, 0.5)) && MenuSlot->GetAlignment().Equals(FVector2D(0.5, 0.5)) && MenuSlot->GetAutoSize());
	const UTextBlock* Title = Cast<UTextBlock>(Tree->FindWidget(TEXT("Title")));
	if (!TestNotNull(TEXT("the title exists"), Title)) return false;
	TestEqual(TEXT("it reads AETHERFALL"), Title->GetText().ToString(), FString(TEXT("AETHERFALL")));
	TestTrue(TEXT("as localizable text keyed in the package"), !Title->GetText().IsCultureInvariant() && FTextInspector::GetKey(Title->GetText()).IsSet());
	TestTrue(TEXT("at 48 points"), FMath::IsNearlyEqual(Title->GetFont().Size, 48.f));
	const FByteProperty* Justification = FindFProperty<FByteProperty>(UTextBlock::StaticClass(), TEXT("Justification"));
	TestTrue(TEXT("centred"), Justification && Justification->GetPropertyValue_InContainer(Title) == ETextJustify::Center);
	TestTrue(TEXT("letting clicks through"), Title->GetVisibility() == ESlateVisibility::HitTestInvisible);
	TestTrue(TEXT("in the gold #E8C170"), Title->GetColorAndOpacity().GetSpecifiedColor().Equals(FLinearColor(FColor::FromHex(TEXT("E8C170"))), 0.001f));
	const UButton* Play = Cast<UButton>(Tree->FindWidget(TEXT("PlayButton")));
	const UButton* Quit = Cast<UButton>(Tree->FindWidget(TEXT("QuitButton")));
	const UVerticalBoxSlot* PlaySlot = Play ? Cast<UVerticalBoxSlot>(Play->Slot) : nullptr;
	const UVerticalBoxSlot* QuitSlot = Quit ? Cast<UVerticalBoxSlot>(Quit->Slot) : nullptr;
	if (!TestTrue(TEXT("both buttons are in the menu"), Play && Quit && PlaySlot && QuitSlot)) return false;
	TestTrue(TEXT("Play has vertical padding"), PlaySlot->GetPadding() == FMargin(0.f, 12.f));
	TestTrue(TEXT("and a dark background"), Play->GetBackgroundColor().Equals(FLinearColor(0.1f, 0.1f, 0.12f, 1.f), 0.001f));
	TestTrue(TEXT("Quit fills the menu's width"), QuitSlot->GetHorizontalAlignment() == EHorizontalAlignment::HAlign_Fill);
	const UTextBlock* PlayLabel = Cast<UTextBlock>(Tree->FindWidget(TEXT("PlayLabel")));
	TestTrue(TEXT("Play's label sits inside it"), PlayLabel && PlayLabel->GetParent() == Play && PlayLabel->GetText().ToString() == TEXT("Play"));
	TestTrue(TEXT("down from Play focuses Quit"), Play->Navigation && Play->Navigation->Down.Rule == EUINavigationRule::Explicit
		&& Play->Navigation->Down.WidgetToFocus == TEXT("QuitButton"));
	TestTrue(TEXT("up from Quit focuses Play, and down wraps"), Quit->Navigation && Quit->Navigation->Up.Rule == EUINavigationRule::Explicit
		&& Quit->Navigation->Up.WidgetToFocus == TEXT("PlayButton") && Quit->Navigation->Down.Rule == EUINavigationRule::Wrap);
	FHyperAIUIInspectRequest Look;
	Look.TargetPaths = {Target};
	const FHyperAIUIInspectReport Inspected = UHyperAIStudioUIToolset::hyper_ui_inspect(Look);
	const FHyperAIUIRecord* PlayRecord = Inspected.Records.FindByPredicate([](const FHyperAIUIRecord& Record)
	{
		return Record.Kind == TEXT("widget") && Record.Name == TEXT("PlayButton");
	});
	const FHyperAIUIFieldValue* Down = PlayRecord ? PlayRecord->Fields.FindByPredicate([](const FHyperAIUIFieldValue& Field)
	{
		return Field.Id == TEXT("navigation_down");
	}) : nullptr;
	TestTrue(*FString::Printf(TEXT("inspect reports Play's navigation (%s)"), Down ? *Down->StringValue : TEXT("none")),
		Down && Down->StringValue == TEXT("explicit:QuitButton"));
	// Validation reads the saved package from the asset registry, which learns of a save a tick later in the editor.
	IAssetRegistry::Get()->ScanFilesSynchronous({FPackageName::LongPackageNameToFilename(Blueprint->GetOutermost()->GetName(),
		FPackageName::GetAssetPackageExtension())}, /*bForceRescan=*/true);
	FHyperAIUIValidateRequest Check;
	Check.TargetPaths = {Target};
	const FHyperAIUIValidateReport Validated = UHyperAIStudioUIToolset::hyper_ui_validate(Check);
	FString Found;
	for (const FHyperAIUIIssue& Issue : Validated.Issues)
	{
		Found += Issue.Severity + TEXT(" ") + Issue.Code + TEXT(": ") + Issue.Message + TEXT(" | ");
	}
	TestTrue(*FString::Printf(TEXT("the menu validates with no errors or warnings (%s)"), *Found),
		Validated.bValid && Validated.ErrorCount == 0 && Validated.WarningCount == 0);

	UWidgetAnimation* IntroAnimation = nullptr;
	for (UWidgetAnimation* Animation : Blueprint->Animations)
	{
		if (Animation && Animation->GetName() == TEXT("Intro")) IntroAnimation = Animation;
	}
	const UMovieScene* Scene = IntroAnimation ? IntroAnimation->GetMovieScene() : nullptr;
	if (!TestNotNull(TEXT("the Intro animation exists"), Scene)) return false;
	const FGuid MenuGuid = [IntroAnimation]()
	{
		for (const FWidgetAnimationBinding& Binding : IntroAnimation->AnimationBindings)
		{
			if (Binding.WidgetName == TEXT("Menu")) return Binding.AnimationGuid;
		}
		return FGuid();
	}();
	const TRange<FFrameNumber> Range = Scene->GetPlaybackRange();
	const UMovieScene2DTransformSection* Slide = nullptr;
	for (UMovieSceneTrack* Track : Scene->FindTracks(UMovieScene2DTransformTrack::StaticClass(), MenuGuid))
	{
		if (Track && !Track->GetAllSections().IsEmpty()) Slide = Cast<UMovieScene2DTransformSection>(Track->GetAllSections()[0]);
	}
	if (!TestNotNull(TEXT("the menu has a transform track"), Slide)) return false;
	TestTrue(TEXT("animating only its vertical position"), Slide->GetMask().GetChannels() == EMovieScene2DTransformChannel::TranslationY);
	float From = 0.f;
	float To = -1.f;
	Slide->Translation[1].Evaluate(FFrameTime(Range.GetLowerBoundValue()), From);
	Slide->Translation[1].Evaluate(FFrameTime(Range.GetUpperBoundValue()), To);
	TestTrue(*FString::Printf(TEXT("it slides up from 40 to 0 (%.1f to %.1f)"), From, To), FMath::IsNearlyEqual(From, 40.f, 0.01f) && FMath::IsNearlyEqual(To, 0.f, 0.01f));
	bool bFades = false;
	for (UMovieSceneTrack* Track : Scene->FindTracks(UMovieSceneFloatTrack::StaticClass(), MenuGuid))
	{
		const UMovieSceneFloatTrack* Float = Cast<UMovieSceneFloatTrack>(Track);
		bFades |= Float && Float->GetPropertyName() == TEXT("RenderOpacity") && !Float->GetAllSections().IsEmpty();
	}
	TestTrue(TEXT("while it fades in"), bFades);
	TestTrue(TEXT("the Widget Blueprint compiles"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	TestFalse(TEXT("and is saved"), Blueprint->GetOutermost()->IsDirty());

	// The whole menu was one transaction: one undo takes it all away, one redo brings it back.
	if (!GEditor)
	{
		AddWarning(TEXT("No editor transaction buffer; undo not checked."));
		return true;
	}
	TestTrue(TEXT("one undo step"), GEditor->UndoTransaction());
	TestTrue(TEXT("undo empties the tree"), Blueprint->WidgetTree->RootWidget == nullptr && !Blueprint->WidgetTree->FindWidget(TEXT("QuitLabel")));
	TestTrue(TEXT("one redo step"), GEditor->RedoTransaction());
	TestTrue(TEXT("redo rebuilds it"), Blueprint->WidgetTree->RootWidget != nullptr && Blueprint->WidgetTree->FindWidget(TEXT("QuitLabel")) != nullptr);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
