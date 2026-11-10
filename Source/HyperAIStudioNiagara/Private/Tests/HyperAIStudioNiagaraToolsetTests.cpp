// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioNiagaraToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSettings.h"
#include "Misc/ScopeExit.h"
#include "HyperAIStudioNiagaraAssetsGate.h"
#include "HyperAIStudioNiagaraExternalEditGate.h"
#include "HyperAIStudioNiagaraGraphGate.h"
#include "HyperAIStudioNiagaraLive.h"
#include "NiagaraComponent.h"
#include "NiagaraGraph.h"
#include "NiagaraScriptSource.h"
#include "PreviewScene.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraScript.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraDataChannel.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannelVariable.h"
#include "NiagaraEffectType.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "ObjectTools.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/SoftObjectPath.h"

#include <type_traits>

namespace HyperAIStudio::Niagara::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter;

	FString HashOf(const TCHAR Character)
	{
		return TEXT("sha256:") + FString::ChrN(64, Character);
	}

	FHyperAIStudioDomainDispatchContext MakeMutationContext(const EHyperAIStudioDomainExecutionActionKind Phase)
	{
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
			FHyperAIStudioNiagaraContracts::GetAdapterDescriptor();
		FHyperAIStudioDomainDispatchContext Context;
		Context.Binding.PackId = FHyperAIStudioNiagaraContracts::PackId;
		Context.Binding.ToolName = TEXT("hyper_niagara_apply_plan");
		Context.Binding.VariantId = FHyperAIStudioNiagaraContracts::MutationVariantId;
		Context.Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Context.Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Context.Safety = EHyperAIStudioDomainSafety::Edit;
		Context.ActionKind = Phase;
		return Context;
	}

	FHyperAINiagaraEditOp MakeOp(const FString& Kind)
	{
		FHyperAINiagaraEditOp Op;
		Op.Kind = Kind;
		return Op;
	}

	const TCHAR* FountainPath = TEXT("/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain");

	/** An unsaved System under /Game with one emitter copied from Niagara's Fountain template (CPU, one sprite renderer). */
	UNiagaraSystem* MakeFountainSystem(const TCHAR* Prefix)
	{
		UNiagaraEmitter* Fountain = LoadObject<UNiagaraEmitter>(nullptr, FountainPath);
		if (!Fountain)
		{
			return nullptr;
		}
		const FString Name = FString(Prefix) + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
		UPackage* Package = CreatePackage(*(TEXT("/Game/__HyperAIStudioTests/") + Name));
		UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
		UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/true);
		FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Fountain, Fountain->GetExposedVersion().VersionGuid);
		System->RequestCompile(/*bForce=*/false);
		System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
		return System;
	}

	FString DescribeIssues(const HyperAIStudio::Niagara::ExternalEditGate::FDiagnostics& Diagnostics)
	{
		FString Text;
		for (const FHyperAINiagaraIssue& Issue : Diagnostics.Issues)
		{
			Text += FString::Printf(TEXT("[%s %s] %s | "), *Issue.Severity, *Issue.SourcePath, *Issue.Summary);
		}
		return Text.IsEmpty() ? FString(TEXT("none")) : Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraManifestTest,
	"HyperAIStudio.Niagara.Contracts.ExactThreeToolCohort",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraManifestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FHyperAIStudioNiagaraManifestEntry>& Manifest =
		FHyperAIStudioNiagaraContracts::GetManifest();
	// The generated catalog pins these six names; another tool would fail admission until it is regenerated.
	TestEqual(TEXT("exactly six Niagara callables"), Manifest.Num(), 6);
	TestEqual(TEXT("exact optional-module toolset identity"),
		FHyperAIStudioNiagaraContracts::GetQualifiedToolsetName(),
		FString(TEXT("HyperAIStudioNiagara.HyperAIStudioNiagaraToolset")));
	const TSet<FString> Expected = {
		TEXT("hyper_niagara_inspect"),
		TEXT("hyper_niagara_apply_plan"),
		TEXT("hyper_niagara_validate"),
		TEXT("hyper_niagara_live"),
		TEXT("hyper_niagara_discover"),
		TEXT("hyper_niagara_preview")};
	TSet<FString> Actual;
	for (const FHyperAIStudioNiagaraManifestEntry& Entry : Manifest)
	{
		Actual.Add(Entry.Name);
		TestEqual(TEXT("one exact reflected owner"), Entry.QualifiedToolset,
			FHyperAIStudioNiagaraContracts::GetQualifiedToolsetName());
		const UFunction* Function = UHyperAIStudioNiagaraToolset::StaticClass()->FindFunctionByName(
			FName(*Entry.Name), EIncludeSuperFlag::ExcludeSuper);
		TestNotNull(TEXT("manifest function exists"), Function);
		if (Function)
		{
			TestTrue(TEXT("manifest function is AICallable"),
				Function->HasMetaData(TEXT("AICallable")));
			// The registry turns this tooltip into the MCP tool description agents choose by.
			TestFalse(TEXT("manifest function has a description"),
				Function->GetMetaData(TEXT("ToolTip")).IsEmpty());
		}
	}
	bool bNamesExact = Actual.Num() == Expected.Num();
	for (const FString& Name : Expected) bNamesExact &= Actual.Contains(Name);
	TestTrue(TEXT("manifest names exact"), bNamesExact);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraDescriptorTest,
	"HyperAIStudio.Niagara.Contracts.AdapterDescriptorExact",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraDescriptorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioNiagaraContracts::GetAdapterDescriptor();
	TestEqual(TEXT("pack id"), Descriptor.PackId, FString(TEXT("niagara_vfx")));
	TestEqual(TEXT("optional module type contract"),
		FString(FHyperAIStudioNiagaraContracts::RequiredModuleType), FString(TEXT("Editor")));
	TestEqual(TEXT("explicit loading phase contract"),
		FString(FHyperAIStudioNiagaraContracts::RequiredLoadingPhase), FString(TEXT("None")));
	TestEqual(TEXT("source cohort"),
		FString(FHyperAIStudioNiagaraContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudioniagaratoolset.v1")));
	TestEqual(TEXT("exact live probe"),
		FString(FHyperAIStudioNiagaraContracts::LiveProbeId),
		FString(TEXT("probe.niagara_editor")));
	if (!TestEqual(TEXT("exact six variants"), Descriptor.Variants.Num(), 6)) return false;
	TestTrue(TEXT("contract fingerprint canonical"),
		FHyperAIStudioNiagaraContracts::IsCanonicalSha256(Descriptor.ContractFingerprint));
	TestTrue(TEXT("adapter fingerprint canonical"),
		FHyperAIStudioNiagaraContracts::IsCanonicalSha256(Descriptor.AdapterFingerprint));
	TestTrue(TEXT("all Niagara requirement groups are blocking"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.IsEmpty());
	TSet<FString> Variants;
	TSet<FString> RequestTypes;
	TSet<FString> ResultTypes;
	for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
	{
		Variants.Add(Variant.VariantId);
		RequestTypes.Add(Variant.RequestTypeId);
		ResultTypes.Add(Variant.ResultTypeId);
		TestTrue(TEXT("request schema canonical"),
			FHyperAIStudioNiagaraContracts::IsCanonicalSha256(Variant.RequestSchemaFingerprint));
		TestTrue(TEXT("result schema canonical"),
			FHyperAIStudioNiagaraContracts::IsCanonicalSha256(Variant.ResultSchemaFingerprint));
		TestTrue(TEXT("result type stays in the disjoint result namespace"),
			Variant.ResultTypeId.StartsWith(TEXT("hyperai.result.")));
	}
	TestEqual(TEXT("unique variants"), Variants.Num(), 6);
	TestEqual(TEXT("unique request DTOs"), RequestTypes.Num(), 6);
	TestEqual(TEXT("unique result DTOs"), ResultTypes.Num(), 6);
	// Safety classes are pinned by the generated catalog; changing one fails admission closed.
	TestEqual(TEXT("inspect read safety"), Descriptor.Variants[0].Safety, EHyperAIStudioDomainSafety::Read);
	TestEqual(TEXT("apply_plan edit safety"), Descriptor.Variants[1].Safety, EHyperAIStudioDomainSafety::Edit);
	TestEqual(TEXT("apply_plan runs batched edit ops"), Descriptor.Variants[1].VariantId,
		FString(TEXT("external_edit_ops.v1")));
	TestEqual(TEXT("validate read safety"), Descriptor.Variants[2].Safety, EHyperAIStudioDomainSafety::Read);
	TestEqual(TEXT("live read safety"), Descriptor.Variants[3].Safety, EHyperAIStudioDomainSafety::Read);
	TestEqual(TEXT("live reads hyper_niagara_live"), Descriptor.Variants[3].ToolName, FString(TEXT("hyper_niagara_live")));
	TestEqual(TEXT("discover read safety"), Descriptor.Variants[4].Safety, EHyperAIStudioDomainSafety::Read);
	TestEqual(TEXT("discover is hyper_niagara_discover"), Descriptor.Variants[4].ToolName, FString(TEXT("hyper_niagara_discover")));
	TestEqual(TEXT("preview read safety"), Descriptor.Variants[5].Safety, EHyperAIStudioDomainSafety::Read);
	TestEqual(TEXT("preview is hyper_niagara_preview"), Descriptor.Variants[5].ToolName, FString(TEXT("hyper_niagara_preview")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraDelegationTest,
	"HyperAIStudio.Niagara.Contracts.CapabilityMatrix",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraDelegationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FHyperAINiagaraCapabilityStatus> Matrix =
		FHyperAIStudioNiagaraContracts::GetCapabilityMatrix();
	TestEqual(TEXT("one explicit Niagara matrix row"), Matrix.Num(), 1);
	if (Matrix.IsEmpty()) return false;
	TSet<FString> Delegated;
	for (const FString& Name : Matrix[0].DelegatedEpicCallables) Delegated.Add(Name);
	TestEqual(TEXT("all 56 Epic Niagara callables still listed"), Matrix[0].DelegatedEpicCallables.Num(), 56);
	TestEqual(TEXT("no duplicate delegation entries"), Delegated.Num(), 56);
	// Agents read these flags from every Niagara report to decide whether apply_plan can mutate.
	TestTrue(TEXT("independent validation is implemented"), Matrix[0].bIndependentValidationImplemented);
	TestTrue(TEXT("mutation execution is implemented"), Matrix[0].bMutationExecutionImplemented);
	TestTrue(TEXT("batched edits are a supported case"),
		Matrix[0].SupportedCases.Contains(TEXT("batched_edit_ops_one_undo_step")));
	TestTrue(TEXT("open-editor hazard is machine-readable"),
		Matrix[0].UnsupportedCases.Contains(TEXT("target_open_in_niagara_editor")));
	TestFalse(TEXT("retired rename blocker is gone"),
		Matrix[0].UnsupportedCases.Contains(TEXT("full_reference_inventory_incomplete")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraEditOpValidationTest,
	"HyperAIStudio.Niagara.Mutation.EditOpValidation",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraEditOpValidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Niagara::Tests;
	namespace Gate = HyperAIStudio::Niagara::ExternalEditGate;
	FString Status;
	FString Diagnostic;
	auto Accepts = [&](const FHyperAINiagaraEditOp& Op) { return Gate::ValidateOp(Op, false, Status, Diagnostic); };

	FHyperAINiagaraEditOp Enable = MakeOp(TEXT("set_module_enabled"));
	Enable.EmitterName = TEXT("Sparks");
	Enable.ScriptName = TEXT("ParticleUpdateScript");
	Enable.ModuleName = TEXT("GravityForce");
	TestTrue(TEXT("emitter module toggle accepted"), Accepts(Enable));

	FHyperAINiagaraEditOp SystemScoped = Enable;
	SystemScoped.ScriptName = TEXT("SystemUpdateScript");
	TestFalse(TEXT("system script with an emitter name rejected"), Accepts(SystemScoped));
	SystemScoped.EmitterName.Reset();
	TestTrue(TEXT("system script without an emitter accepted"), Accepts(SystemScoped));

	FHyperAINiagaraEditOp NoEmitter = Enable;
	NoEmitter.EmitterName.Reset();
	TestFalse(TEXT("emitter script without an emitter rejected"), Accepts(NoEmitter));
	TestEqual(TEXT("address failure status"), Status, FString(TEXT("invalid_op_address")));

	FHyperAINiagaraEditOp BadScript = Enable;
	BadScript.ScriptName = TEXT("ParticleSimulationStage");
	TestFalse(TEXT("unknown script stack rejected"), Accepts(BadScript));

	FHyperAINiagaraEditOp ControlChars = Enable;
	ControlChars.ModuleName = TEXT("Gravity\nForce");
	TestFalse(TEXT("control characters rejected"), Accepts(ControlChars));
	TestEqual(TEXT("text failure status"), Status, FString(TEXT("invalid_op_text")));

	TestFalse(TEXT("unknown kind rejected"), Accepts(MakeOp(TEXT("rename_module"))));
	TestEqual(TEXT("as unsupported_op_kind"), Status, FString(TEXT("unsupported_op_kind")));
	TestFalse(TEXT("a removal without its address is refused"), Accepts(MakeOp(TEXT("remove_module"))));
	TestEqual(TEXT("as invalid_op_address"), Status, FString(TEXT("invalid_op_address")));

	FHyperAINiagaraEditOp SetValue = Enable;
	SetValue.Kind = TEXT("set_input_value");
	SetValue.InputNameStack = { TEXT("Gravity") };
	auto ValueAccepted = [&](const TCHAR* Type, const TCHAR* Value)
	{
		SetValue.ValueType = Type;
		SetValue.Value = Value;
		return Accepts(SetValue);
	};
	TestTrue(TEXT("float"), ValueAccepted(TEXT("float"), TEXT("-9.8")));
	TestTrue(TEXT("int32"), ValueAccepted(TEXT("int32"), TEXT("3")));
	TestTrue(TEXT("bool"), ValueAccepted(TEXT("bool"), TEXT("true")));
	TestTrue(TEXT("vector"), ValueAccepted(TEXT("vector"), TEXT("0, 0, -980")));
	TestTrue(TEXT("color"), ValueAccepted(TEXT("color"), TEXT("1,0.5,0,1")));
	TestFalse(TEXT("int32 rejects a fraction"), ValueAccepted(TEXT("int32"), TEXT("3.5")));
	TestFalse(TEXT("vector rejects the wrong arity"), ValueAccepted(TEXT("vector"), TEXT("1,2")));
	TestFalse(TEXT("float rejects exponent and words"), ValueAccepted(TEXT("float"), TEXT("nan")));
	TestFalse(TEXT("bool rejects anything but true or false"), ValueAccepted(TEXT("bool"), TEXT("1")));
	TestFalse(TEXT("unknown value type rejected"), ValueAccepted(TEXT("quat"), TEXT("0,0,0,1")));
	TestTrue(TEXT("vector2"), ValueAccepted(TEXT("vector2"), TEXT("1,2")));
	TestTrue(TEXT("vector4"), ValueAccepted(TEXT("vector4"), TEXT("1,2,3,4")));
	TestTrue(TEXT("position"), ValueAccepted(TEXT("position"), TEXT("0,0,100")));
	SetValue.ValueType = TEXT("float");
	SetValue.Value = TEXT("1");
	SetValue.InputNameStack.Reset();
	TestFalse(TEXT("set_input_value needs an input name"), Accepts(SetValue));

	FHyperAINiagaraEditOp Renderer = MakeOp(TEXT("add_renderer"));
	Renderer.EmitterName = TEXT("Sparks");
	Renderer.AssetPath = TEXT("/Script/Niagara.NiagaraSpriteRendererProperties");
	TestTrue(TEXT("renderer add accepted by shape"), Accepts(Renderer));
	TestTrue(TEXT("sprite renderer class resolves"), Gate::ValidateOp(Renderer, true, Status, Diagnostic));
	Renderer.AssetPath = TEXT("/Script/Niagara.NiagaraRendererProperties");
	TestFalse(TEXT("abstract renderer base rejected"), Gate::ValidateOp(Renderer, true, Status, Diagnostic));

	FHyperAINiagaraEditOp Fix = MakeOp(TEXT("apply_stack_issue_fix"));
	TestFalse(TEXT("fix without ids rejected"), Accepts(Fix));
	Fix.IssueId = TEXT("issue");
	Fix.FixId = TEXT("fix");
	TestTrue(TEXT("fix with ids accepted"), Accepts(Fix));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraCasBoundsTest,
	"HyperAIStudio.Niagara.Contracts.CasTriStatePathsAndBounds",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraCasBoundsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("AssetRegistry Exists"),
		FHyperAIStudioNiagaraContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Exists), FString(TEXT("exists")));
	TestEqual(TEXT("AssetRegistry DoesNotExist"),
		FHyperAIStudioNiagaraContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::DoesNotExist), FString(TEXT("does_not_exist")));
	TestEqual(TEXT("AssetRegistry Unknown stays unknown"),
		FHyperAIStudioNiagaraContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Unknown), FString(TEXT("unknown")));
	TestTrue(TEXT("canonical top-level object path"),
		FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(TEXT("/Game/VFX/NS_Test.NS_Test")));
	TestFalse(TEXT("folder path rejected"),
		FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(TEXT("/Game/VFX")));
	TestFalse(TEXT("subobject path rejected"),
		FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(TEXT("/Game/VFX/NS_Test.NS_Test:Emitter")));
	TestFalse(TEXT("wildcard rejected"),
		FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(TEXT("/Game/VFX/*.NS_Test")));
	TestTrue(TEXT("lowercase sha accepted"),
		FHyperAIStudioNiagaraContracts::IsCanonicalSha256(HyperAIStudio::Niagara::Tests::HashOf(TEXT('a'))));
	TestFalse(TEXT("uppercase sha rejected"),
		FHyperAIStudioNiagaraContracts::IsCanonicalSha256(HyperAIStudio::Niagara::Tests::HashOf(TEXT('A'))));
	TestTrue(TEXT("User namespace accepted"),
		FHyperAIStudioNiagaraContracts::IsUserParameterName(TEXT("User.SpawnRate")));
	TestFalse(TEXT("non-user namespace rejected"),
		FHyperAIStudioNiagaraContracts::IsUserParameterName(TEXT("System.SpawnRate")));
	TestEqual(TEXT("page bound"), FHyperAIStudioNiagaraContracts::MaxPageSize, 64);
	TestEqual(TEXT("op bound"), FHyperAIStudioNiagaraContracts::MaxOpsPerPlan, 96);

	FHyperAINiagaraApplyPlanRequest Request;
	Request.TargetPath = TEXT("/Game/VFX/NS_Test.NS_Test");
	Request.ExpectedRevision = HyperAIStudio::Niagara::Tests::HashOf(TEXT('1'));
	TestEqual(TEXT("an empty plan is rejected before any capture"),
		FHyperAIStudioNiagaraContracts::BuildPlan(Request).Status, FString(TEXT("invalid_op_count")));
	Request.bDryRun = false;
	TestEqual(TEXT("non-dry needs an operation id and plan hash"),
		FHyperAIStudioNiagaraContracts::BuildPlan(Request).Status, FString(TEXT("invalid_submission_fields")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraPayloadAndAdapterTest,
	"HyperAIStudio.Niagara.Mutation.PayloadSealAndAdapterPhases",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraPayloadAndAdapterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Niagara::Tests;
	const TSharedRef<FHyperAIStudioNiagaraEditOpsPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioNiagaraEditOpsPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = TEXT("/Game/__HyperAIStudioMissing/NS_Missing.NS_Missing");
	Payload->BaseRevision = HashOf(TEXT('1'));
	FHyperAINiagaraEditOp First = MakeOp(TEXT("set_module_enabled"));
	First.EmitterName = TEXT("A");
	First.ScriptName = TEXT("ParticleUpdateScript");
	First.ModuleName = TEXT("M1");
	FHyperAINiagaraEditOp Second = First;
	Second.ModuleName = TEXT("M2");
	Second.bEnabled = false;
	Payload->Ops = { First, Second };
	Payload->SemanticFingerprint = FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(*Payload);
	TestTrue(TEXT("fingerprint is canonical"),
		FHyperAIStudioNiagaraContracts::IsCanonicalSha256(Payload->SemanticFingerprint));
	TestEqual(TEXT("fingerprint is stable"), Payload->SemanticFingerprint,
		FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(*Payload));

	FHyperAIStudioNiagaraEditOpsPayload Reordered = *Payload;
	Reordered.Ops = { Second, First };
	TestNotEqual(TEXT("reordered ops are a different plan"), Payload->SemanticFingerprint,
		FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(Reordered));
	FHyperAIStudioNiagaraEditOpsPayload Unsaved = *Payload;
	Unsaved.bSave = false;
	TestNotEqual(TEXT("save choice is sealed"), Payload->SemanticFingerprint,
		FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(Unsaved));

	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone = Payload->CloneImmutable();
	TestTrue(TEXT("clone is detached"), &Clone.Get() != &Payload.Get());
	TestEqual(TEXT("clone seal preserved"), Clone->GetSemanticFingerprint(), Payload->GetSemanticFingerprint());
	TestEqual(TEXT("clone size preserved"), Clone->GetBoundedByteSize(), Payload->GetBoundedByteSize());

	FHyperAIStudioNiagaraDomainAdapter Adapter;
	const FHyperAIStudioDomainAdapterResult Apply = Adapter.Execute(
		MakeMutationContext(EHyperAIStudioDomainExecutionActionKind::Apply), *Clone);
	TestEqual(TEXT("apply on a missing target is rejected before effect"), Apply.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("missing target status"), Apply.StatusCode, FString(TEXT("target_not_loaded")));
	TestFalse(TEXT("rejected apply claims no result"), Apply.Payload.IsValid());

	// After Apply, the edit may already be in memory: a later phase failing must never look effect-free.
	const FHyperAIStudioDomainAdapterResult Save = Adapter.Execute(
		MakeMutationContext(EHyperAIStudioDomainExecutionActionKind::Save), *Clone);
	TestEqual(TEXT("post-apply failure reports a known effect"), Save.Outcome,
		EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect);

	Payload->Ops[0].ModuleName = TEXT("Drifted");
	const FHyperAIStudioDomainAdapterResult Drifted = Adapter.Execute(
		MakeMutationContext(EHyperAIStudioDomainExecutionActionKind::Apply), *Payload);
	TestEqual(TEXT("semantic drift rejected"), Drifted.StatusCode, FString(TEXT("typed_payload_drift")));

	FHyperAIStudioNiagaraFreshVerifier Verifier(Adapter.GetDescriptor().AdapterFingerprint);
	FHyperAIStudioNiagaraMutationResultPayload NotCompleted;
	NotCompleted.Phase = TEXT("saved");
	NotCompleted.Revision = HashOf(TEXT('4'));
	NotCompleted.bValid = true;
	FString Postcondition;
	FString VerifyError;
	TestFalse(TEXT("only a completed capture verifies"),
		Verifier.VerifyFreshExact(*Clone, NotCompleted, Postcondition, VerifyError));
	TestTrue(TEXT("failed verification emits no postcondition"), Postcondition.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraEndToEndTest,
	"HyperAIStudio.Niagara.Mutation.EndToEndToggleModule",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraEndToEndTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Planning accepts only a System loaded from disk, so this spans two editor sessions: the first saves a
	// duplicate fixture of a project System, the second loads that fixture fresh and edits it. The fixture lives
	// in Content/__HyperAIStudioTests; delete that folder once done. Nothing here edits an original asset.
	const FString FixtureFolder = TEXT("/Game/__HyperAIStudioTests");
	const FString FixtureName = TEXT("NS_HyperAIEditOpsE2E");
	const FString PackageName = FixtureFolder / FixtureName;
	const FString TargetPath = PackageName + TEXT(".") + FixtureName;

	if (!FPackageName::DoesPackageExist(PackageName))
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Candidates;
		Registry.GetAssetsByClass(UNiagaraSystem::StaticClass()->GetClassPathName(), Candidates);
		const FAssetData* Source = Candidates.FindByPredicate([&](const FAssetData& Asset)
		{
			const FString Package = Asset.PackageName.ToString();
			return Package.StartsWith(TEXT("/Game/")) && !Package.StartsWith(FixtureFolder);
		});
		if (!Source)
		{
			AddInfo(TEXT("Skipped: no Niagara System under /Game to duplicate."));
			return true;
		}
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UObject* Fixture = AssetTools.DuplicateAsset(FixtureName, FixtureFolder, Source->GetAsset());
		TestNotNull(TEXT("fixture duplicated"), Fixture);
		if (Fixture)
		{
			TestTrue(TEXT("fixture saved"), UEditorLoadingAndSavingUtils::SavePackages({ Fixture->GetOutermost() }, false));
		}
		AddWarning(FString::Printf(TEXT("Created fixture %s from %s. Run this test again in a new editor session to exercise the edit path."),
			*TargetPath, *Source->PackageName.ToString()));
		return true;
	}

	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(TargetPath).TryLoad());
	if (!System || !System->HasAnyFlags(RF_WasLoaded))
	{
		AddWarning(TEXT("The fixture was created in this session and is not loaded from disk yet; run again in a new editor session."));
		return true;
	}
	// A freshly loaded System may not have compiled yet; planning requires a finished compile.
	System->RequestCompile(/*bForce=*/false);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);

	FHyperAINiagaraInspectRequest InspectRequest;
	InspectRequest.TargetPath = TargetPath;
	InspectRequest.bIncludeTopology = true;
	InspectRequest.MaxOutputBytes = FHyperAIStudioNiagaraContracts::MaxOutputBytes;
	const FHyperAINiagaraInspectReport Inspect = FHyperAIStudioNiagaraContracts::Inspect(InspectRequest);
	FString CaptureCodes;
	for (const FHyperAINiagaraIssue& Issue : Inspect.Issues)
	{
		CaptureCodes += Issue.Code + TEXT(" ");
	}
	TestTrue(*FString::Printf(TEXT("fixture inspects cleanly (%s: %s)"), *Inspect.Status, *CaptureCodes), Inspect.bOk);
	TestFalse(TEXT("topology returned"), Inspect.Emitters.IsEmpty());

	// Late particle-update modules (colour, scale) can be disabled without breaking the stack; state modules cannot.
	FHyperAINiagaraEditOp Toggle = HyperAIStudio::Niagara::Tests::MakeOp(TEXT("set_module_enabled"));
	for (const FHyperAINiagaraEmitterTopology& Emitter : Inspect.Emitters)
	{
		for (const FHyperAINiagaraScriptStackTopology& Stack : Emitter.ScriptStacks)
		{
			if (Stack.ScriptName != TEXT("ParticleUpdateScript") || !Toggle.ModuleName.IsEmpty())
			{
				continue;
			}
			for (int32 Index = Stack.Modules.Num() - 1; Index >= 0; --Index)
			{
				const FHyperAINiagaraModuleTopology& Module = Stack.Modules[Index];
				if (!Module.ModuleName.Contains(TEXT("State")) && !Module.ModuleName.Contains(TEXT("Initialize")))
				{
					Toggle.EmitterName = Emitter.EmitterName;
					Toggle.ScriptName = Stack.ScriptName;
					Toggle.ModuleName = Module.ModuleName;
					Toggle.bEnabled = !Module.bEnabled;
					break;
				}
			}
		}
	}
	if (!Inspect.bOk || Toggle.ModuleName.IsEmpty())
	{
		AddError(TEXT("The fixture has no ParticleUpdateScript module suitable for toggling, or did not inspect cleanly."));
		return false;
	}

	// This test approves nothing by hand, so it runs with the approval gate off and puts it back after.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bPreviousApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT { Settings->bRequireApprovalForAgentEdits = bPreviousApproval; };

	FHyperAINiagaraApplyPlanRequest Plan;
	Plan.TargetPath = TargetPath;
	Plan.ExpectedRevision = Inspect.Revision;
	Plan.Ops = { Toggle };
	const FHyperAINiagaraApplyPlanReport DryRun = FHyperAIStudioNiagaraContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic),
		DryRun.bOk && DryRun.bTrustedPrepared);
	TestFalse(TEXT("dry run stages nothing"), DryRun.bStaged);
	if (!DryRun.bOk)
	{
		return false;
	}

	Plan.bDryRun = false;
	Plan.OperationId = TEXT("niagara-e2e-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	Plan.ExpectedPlanHash = DryRun.PlanHash;
	const FHyperAINiagaraApplyPlanReport Submitted = FHyperAIStudioNiagaraContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("submission accepted (%s: %s)"), *Submitted.Status, *Submitted.Diagnostic),
		Submitted.bExecutionSubmitted);

	FHyperAIStudioTypedArtifactOperationStatus OperationStatus;
	FString StatusError;
	for (int32 Tick = 0; Tick < 400 && Submitted.bExecutionSubmitted; ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.01f);
		if (FHyperAIStudioTrustedExecutionFacade::QueryStatus(Plan.OperationId, OperationStatus, StatusError)
			&& OperationStatus.bTerminal)
		{
			break;
		}
	}
	TestTrue(*FString::Printf(TEXT("operation reaches a terminal state (%s)"), *OperationStatus.Status), OperationStatus.bTerminal);
	TestEqual(*FString::Printf(TEXT("operation completed (%s)"), *OperationStatus.Diagnostic),
		OperationStatus.Status, FString(TEXT("completed")));

	FHyperAINiagaraInspectRequest After = InspectRequest;
	const FHyperAINiagaraInspectReport Final = FHyperAIStudioNiagaraContracts::Inspect(After);
	bool bFlipped = false;
	for (const FHyperAINiagaraEmitterTopology& Emitter : Final.Emitters)
	{
		for (const FHyperAINiagaraScriptStackTopology& Stack : Emitter.ScriptStacks)
		{
			for (const FHyperAINiagaraModuleTopology& Module : Stack.Modules)
			{
				bFlipped |= Emitter.EmitterName == Toggle.EmitterName && Stack.ScriptName == Toggle.ScriptName
					&& Module.ModuleName == Toggle.ModuleName && Module.bEnabled == Toggle.bEnabled;
			}
		}
	}
	TestTrue(*FString::Printf(TEXT("%s/%s/%s is now %s"), *Toggle.EmitterName, *Toggle.ScriptName, *Toggle.ModuleName,
		Toggle.bEnabled ? TEXT("enabled") : TEXT("disabled")), bFlipped);
	TestFalse(TEXT("the edit was saved"), System->GetOutermost()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraPublicApiAuditTest,
	"HyperAIStudio.Niagara.UE58.PublicApiSignatures",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraPublicApiAuditTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Experimental external-edit signatures are audited at compile time inside the gate's .cpp.
	using FExistingViewModelSignature = TSharedPtr<FNiagaraSystemViewModel>
		(FNiagaraEditorModule::*)(UNiagaraSystem*);
	FExistingViewModelSignature ExistingViewModelApi =
		&FNiagaraEditorModule::GetExistingViewModelForSystem;
	using FRequestCompileReturn = decltype(DeclVal<UNiagaraSystem&>().RequestCompile(
		false, nullptr, nullptr));
	using FPackageExistsReturn = decltype(DeclVal<IAssetRegistry&>().TryGetAssetPackageData(
		FName(), DeclVal<FAssetPackageData&>(), true));
	static_assert(std::is_same_v<FRequestCompileReturn, bool>,
		"UE 5.8 UNiagaraSystem::RequestCompile return type changed");
	static_assert(std::is_same_v<FPackageExistsReturn, UE::AssetRegistry::EExists>,
		"UE 5.8 non-blocking package tri-state changed");
	TestTrue(TEXT("public existing-ViewModel lookup API signature"), ExistingViewModelApi != nullptr);
	TestTrue(TEXT("external edit API present"), HyperAIStudio::Niagara::ExternalEditGate::IsApiAvailable());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraEffectTypeAndDataChannelTest,
	"HyperAIStudio.Niagara.Assets.EffectTypeAndDataChannel",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraEffectTypeAndDataChannelTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	namespace Assets = HyperAIStudio::Niagara::AssetsGate;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString EffectPath = FString::Printf(TEXT("/Game/__HyperAIStudioTests/ET_HyperAI_%s.ET_HyperAI_%s"), *Suffix, *Suffix);
	const FString ChannelPath = FString::Printf(TEXT("/Game/__HyperAIStudioTests/DC_HyperAI_%s.DC_HyperAI_%s"), *Suffix, *Suffix);
	const FString SystemPath = TEXT("/Game/__HyperAIStudioTests/NS_Unused.NS_Unused");
	auto Op = [](const TCHAR* Kind, const FString& AssetPath, const TCHAR* Name = TEXT(""), const TCHAR* Value = TEXT(""), const TCHAR* ValueType = TEXT(""))
	{
		FHyperAINiagaraEditOp Result = HyperAIStudio::Niagara::Tests::MakeOp(Kind);
		Result.AssetPath = AssetPath;
		Result.Name = Name;
		Result.Value = Value;
		Result.ValueType = ValueType;
		return Result;
	};
	FString Status;
	FString Diagnostic;
	TSet<FString> Planned;
	TestTrue(TEXT("a new effect type path validates"), Assets::ValidateAssetOp(Op(TEXT("create_effect_type"), EffectPath), SystemPath, Planned, Status, Diagnostic));
	Planned.Add(EffectPath);
	TestTrue(TEXT("settings on an effect type created earlier in the plan validate"),
		Assets::ValidateAssetOp(Op(TEXT("set_effect_type_setting"), EffectPath, TEXT("max_distance"), TEXT("5000")), SystemPath, Planned, Status, Diagnostic));
	TestFalse(TEXT("unknown settings are refused"),
		Assets::ValidateAssetOp(Op(TEXT("set_effect_type_setting"), EffectPath, TEXT("max_fun"), TEXT("1")), SystemPath, Planned, Status, Diagnostic));
	TestFalse(TEXT("enum settings outside their set are refused"),
		Assets::ValidateAssetOp(Op(TEXT("set_effect_type_setting"), EffectPath, TEXT("update_frequency"), TEXT("sometimes")), SystemPath, Planned, Status, Diagnostic));
	TestFalse(TEXT("an unknown channel type is refused"),
		Assets::ValidateAssetOp(Op(TEXT("create_data_channel"), ChannelPath, TEXT(""), TEXT("Energy:float"), TEXT("sideways")), SystemPath, Planned, Status, Diagnostic));
	TestFalse(TEXT("an unknown variable type is refused"),
		Assets::ValidateAssetOp(Op(TEXT("create_data_channel"), ChannelPath, TEXT(""), TEXT("Energy:double"), TEXT("islands")), SystemPath, Planned, Status, Diagnostic));
	TestFalse(TEXT("duplicate variables are refused"),
		Assets::ValidateAssetOp(Op(TEXT("create_data_channel"), ChannelPath, TEXT(""), TEXT("A:float,a:int32"), TEXT("global")), SystemPath, Planned, Status, Diagnostic));
	FHyperAINiagaraEditOp Addressed = Op(TEXT("create_effect_type"), ChannelPath);
	Addressed.EmitterName = TEXT("Sparks");
	TestFalse(TEXT("asset ops take no emitter address"), Assets::ValidateAssetOp(Addressed, SystemPath, Planned, Status, Diagnostic));
	TestFalse(TEXT("baking an unknown capture is refused"),
		Assets::ValidateAssetOp(Op(TEXT("bake_sim_cache"), ChannelPath, TEXT("capture-nope")), SystemPath, Planned, Status, Diagnostic));

	// The two private data channel properties are written by name; this fails when UE renames or retypes them.
	const FObjectProperty* ChannelProperty = FindFProperty<FObjectProperty>(UNiagaraDataChannelAsset::StaticClass(), TEXT("DataChannel"));
	const FArrayProperty* VariablesProperty = FindFProperty<FArrayProperty>(UNiagaraDataChannel::StaticClass(), TEXT("ChannelVariables"));
	TestNotNull(TEXT("UNiagaraDataChannelAsset::DataChannel still exists"), ChannelProperty);
	TestTrue(TEXT("UNiagaraDataChannel::ChannelVariables is still an array of FNiagaraDataChannelVariable"),
		VariablesProperty && CastField<FStructProperty>(VariablesProperty->Inner)
		&& CastField<FStructProperty>(VariablesProperty->Inner)->Struct == FNiagaraDataChannelVariable::StaticStruct());

	UNiagaraSystem* System = NewObject<UNiagaraSystem>(GetTransientPackage(), NAME_None, RF_Transient);
	TArray<UObject*> Created;
	ON_SCOPE_EXIT { if (!Created.IsEmpty()) ObjectTools::ForceDeleteObjects(Created, /*ShowConfirmation=*/false); };
	TestTrue(TEXT("create_effect_type applies: ") + Diagnostic, Assets::ApplyAssetOp(*System, Op(TEXT("create_effect_type"), EffectPath), Status, Diagnostic));
	UNiagaraEffectType* EffectType = Cast<UNiagaraEffectType>(FSoftObjectPath(EffectPath).ResolveObject());
	if (!TestNotNull(TEXT("the effect type exists"), EffectType)) return false;
	Created.Add(EffectType);
	TestTrue(TEXT("max_distance applies"), Assets::ApplyAssetOp(*System, Op(TEXT("set_effect_type_setting"), EffectPath, TEXT("max_distance"), TEXT("5000")), Status, Diagnostic));
	TestTrue(TEXT("update_frequency applies"), Assets::ApplyAssetOp(*System, Op(TEXT("set_effect_type_setting"), EffectPath, TEXT("update_frequency"), TEXT("low")), Status, Diagnostic));
	FHyperAINiagaraAssetRecord Record;
	TestTrue(TEXT("the effect type is described"), Assets::DescribeAsset(*EffectType, Record));
	TestEqual(TEXT("as an effect type"), Record.Kind, FString(TEXT("effect_type")));
	TestTrue(TEXT("with its distance budget"), Record.Details.ContainsByPredicate([](const FHyperAINiagaraKeyValue& Row)
	{
		return Row.Key == TEXT("max_distance") && Row.Value == TEXT("5000.0");
	}));
	TestEqual(TEXT("and update frequency"), EffectType->UpdateFrequency, ENiagaraScalabilityUpdateFrequency::Low);
	TestTrue(TEXT("set_effect_type assigns it"), Assets::ApplyAssetOp(*System, Op(TEXT("set_effect_type"), EffectPath), Status, Diagnostic));
	TestEqual(TEXT("the System reports it"), Assets::GetEffectTypePath(*System), EffectPath);
	TestTrue(TEXT("an empty path clears it"), Assets::ApplyAssetOp(*System, Op(TEXT("set_effect_type"), FString()), Status, Diagnostic));
	TestTrue(TEXT("and it is gone"), Assets::GetEffectTypePath(*System).IsEmpty());

	TestTrue(TEXT("create_data_channel applies: ") + Diagnostic, Assets::ApplyAssetOp(*System,
		Op(TEXT("create_data_channel"), ChannelPath, TEXT(""), TEXT("ImpactPosition:position,Energy:float,Tint:linear_color"), TEXT("islands")), Status, Diagnostic));
	UObject* Channel = FSoftObjectPath(ChannelPath).ResolveObject();
	if (!TestNotNull(TEXT("the data channel exists"), Channel)) return false;
	Created.Add(Channel);
	Record = {};
	TestTrue(TEXT("the data channel is described"), Assets::DescribeAsset(*Channel, Record));
	TestTrue(TEXT("as islands"), Record.Details.ContainsByPredicate([](const FHyperAINiagaraKeyValue& Row)
	{
		return Row.Key == TEXT("type") && Row.Value == TEXT("islands");
	}));
	TestEqual(TEXT("with its three variables"), Record.Details.FilterByPredicate([](const FHyperAINiagaraKeyValue& Row)
	{
		return Row.Key == TEXT("variable");
	}).Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraSimCacheTest,
	"HyperAIStudio.Niagara.Assets.SimCacheCaptureBakeAndRegression",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraSimCacheTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	namespace Assets = HyperAIStudio::Niagara::AssetsGate;
	// Captures need a CPU System under /Game. An unsaved copy of one of Niagara's own CPU templates serves.
	UNiagaraSystem* Template = nullptr;
	for (const TCHAR* Candidate : {TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst.DirectionalBurst"),
		TEXT("/Niagara/DefaultAssets/Templates/Systems/RadialBurst.RadialBurst"),
		TEXT("/Niagara/DefaultAssets/Templates/Systems/SimpleExplosion.SimpleExplosion")})
	{
		UNiagaraSystem* Loaded = Cast<UNiagaraSystem>(FSoftObjectPath(Candidate).TryLoad());
		if (Loaded && !Loaded->HasAnyGPUEmitters())
		{
			Template = Loaded;
			break;
		}
	}
	if (!Template)
	{
		AddInfo(TEXT("Skipped: no CPU Niagara template System is available to capture."));
		return true;
	}
	const FString FixtureName = TEXT("NS_HyperAICapture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	UPackage* FixturePackage = CreatePackage(*(TEXT("/Game/__HyperAIStudioTests/") + FixtureName));
	UNiagaraSystem* System = DuplicateObject<UNiagaraSystem>(Template, FixturePackage, FName(*FixtureName));
	if (!TestNotNull(TEXT("the capture fixture duplicates"), System)) return false;
	System->SetFlags(RF_Public | RF_Standalone);
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };
	const FString SystemPath = System->GetPathName();
	// A golden comparison only means something for a repeatable simulation, so the copy turns determinism on
	// for the System and every emitter.
	if (FBoolProperty* Determinism = FindFProperty<FBoolProperty>(UNiagaraSystem::StaticClass(), TEXT("bDeterminism")))
	{
		Determinism->SetPropertyValue_InContainer(System, true);
	}
	for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData()) Data->bDeterminism = true;
	}
	System->RequestCompile(/*bForce=*/true);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);

	auto Capture = [&](const FString& Golden, const float Delta = 1.f / 60.f)
	{
		FHyperAINiagaraValidateRequest Request;
		Request.TargetPath = SystemPath;
		Request.Policy = TEXT("sim_cache_capture");
		Request.CaptureFrames = 12;
		Request.CaptureDeltaSeconds = Delta;
		FHyperAINiagaraValidateReport Report = FHyperAIStudioNiagaraContracts::Validate(Request);
		TestEqual(*FString::Printf(TEXT("capture starts (%s)"), *Report.Diagnostic), Report.Status, FString(TEXT("capture_started")));
		Request.CaptureId = Report.SimCache.CaptureId;
		Request.GoldenSimCachePath = Golden;
		for (int32 Tick = 0; Tick < 500 && Report.SimCache.State == TEXT("capturing"); ++Tick)
		{
			FPlatformProcess::Sleep(0.01f);
			FTSTicker::GetCoreTicker().Tick(0.01f);
			Report = FHyperAIStudioNiagaraContracts::Validate(Request);
		}
		return Report;
	};

	const FHyperAINiagaraValidateReport First = Capture(FString());
	TestEqual(*FString::Printf(TEXT("the capture completes (%s)"), *First.SimCache.Error), First.SimCache.State, FString(TEXT("complete")));
	TestEqual(TEXT("every frame is recorded"), First.SimCache.FramesCaptured, 12);
	TestFalse(TEXT("emitters are listed"), First.SimCache.EmitterNames.IsEmpty());
	if (First.SimCache.State != TEXT("complete")) return false;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString GoldenPath = FString::Printf(TEXT("/Game/__HyperAIStudioTests/SC_HyperAI_%s.SC_HyperAI_%s"), *Suffix, *Suffix);
	FHyperAINiagaraEditOp Bake = HyperAIStudio::Niagara::Tests::MakeOp(TEXT("bake_sim_cache"));
	Bake.Name = First.SimCache.CaptureId;
	Bake.AssetPath = GoldenPath;
	FString Status;
	FString Diagnostic;
	TestTrue(TEXT("a fresh capture of this System can be baked: ") + Diagnostic,
		Assets::ValidateAssetOp(Bake, SystemPath, {}, Status, Diagnostic));
	TestTrue(TEXT("baking applies: ") + Diagnostic, Assets::ApplyAssetOp(*System, Bake, Status, Diagnostic));
	UObject* Golden = FSoftObjectPath(GoldenPath).ResolveObject();
	if (!TestNotNull(TEXT("the golden cache exists"), Golden)) return false;
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({Golden}, /*ShowConfirmation=*/false); };
	FHyperAINiagaraAssetRecord Record;
	TestTrue(TEXT("the golden cache is described"), Assets::DescribeAsset(*Golden, Record) && Record.Kind == TEXT("sim_cache"));
	TestFalse(TEXT("a baked capture cannot be baked twice"), Assets::ValidateAssetOp(Bake, SystemPath, {}, Status, Diagnostic));

	const FHyperAINiagaraValidateReport Second = Capture(GoldenPath);
	TestTrue(TEXT("the second capture is compared with the golden cache"), Second.SimCache.bCompared);
	TestTrue(TEXT("the copy counts as fully deterministic"), Second.SimCache.bDeterministic);
	TestTrue(TEXT("an unchanged deterministic System matches its golden cache: ") + FString::Join(Second.SimCache.Differences, TEXT(" | ")),
		Second.SimCache.bMatch);
	TestEqual(TEXT("and validate says so"), Second.Status, FString(TEXT("matches_golden")));
	const FHyperAINiagaraValidateReport Changed = Capture(GoldenPath, 1.f / 30.f);
	TestFalse(TEXT("a changed simulation no longer matches"), Changed.SimCache.bMatch);
	TestFalse(TEXT("and the differences are listed"), Changed.SimCache.Differences.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraRuntimeProbeTest,
	"HyperAIStudio.Niagara.Validate.RuntimeProbeCountsLiveParticles",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraRuntimeProbeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Two looping emitters from Niagara's own templates, in an unsaved System under /Game.
	UNiagaraEmitter* Fountain = LoadObject<UNiagaraEmitter>(nullptr, TEXT("/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain"));
	UNiagaraEmitter* Looping = LoadObject<UNiagaraEmitter>(nullptr,
		TEXT("/Niagara/DefaultAssets/Templates/Emitters/SingleLoopingParticle.SingleLoopingParticle"));
	if (!Fountain || !Looping)
	{
		AddInfo(TEXT("Skipped: Niagara's emitter templates are not available."));
		return true;
	}
	const FString FixtureName = TEXT("NS_HyperAIProbe_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	UPackage* FixturePackage = CreatePackage(*(TEXT("/Game/__HyperAIStudioTests/") + FixtureName));
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(FixturePackage, FName(*FixtureName), RF_Public | RF_Standalone);
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };
	UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/true);
	for (UNiagaraEmitter* Emitter : {Fountain, Looping})
	{
		FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Emitter, Emitter->GetExposedVersion().VersionGuid);
	}
	System->RequestCompile(/*bForce=*/true);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);

	FHyperAINiagaraValidateRequest Request;
	Request.TargetPath = System->GetPathName();
	Request.Policy = TEXT("runtime_probe");
	Request.ProbeFrames = 60;
	const FHyperAINiagaraValidateReport Report = FHyperAIStudioNiagaraContracts::Validate(Request);
	TestEqual(*FString::Printf(TEXT("both emitters are live (%s)"), *Report.Diagnostic), Report.Status, FString(TEXT("live")));
	TestEqual(TEXT("one second simulated"), Report.RuntimeProbe.SimulatedSeconds, 1.f, 0.001f);
	if (!TestEqual(TEXT("each emitter is reported"), Report.RuntimeProbe.Emitters.Num(), 2)) return false;
	for (const FHyperAINiagaraEmitterRuntime& Emitter : Report.RuntimeProbe.Emitters)
	{
		TestEqual(Emitter.Name + TEXT(" is active"), Emitter.ExecutionState, FString(TEXT("Active")));
		TestTrue(Emitter.Name + TEXT(" has live particles"), Emitter.NumParticles > 0 && Emitter.bMeetsMinimum);
		TestTrue(Emitter.Name + TEXT(" says where its count came from"), Emitter.CountSource == TEXT("cpu") || Emitter.CountSource == TEXT("gpu_estimate"));
	}

	// Named emitters are checked by name, and one the System lacks is reported rather than assumed.
	Request.ExpectEmitters = {Report.RuntimeProbe.Emitters[0].Name};
	TestEqual(TEXT("a named live emitter passes"), FHyperAIStudioNiagaraContracts::Validate(Request).Status, FString(TEXT("live")));
	Request.ExpectEmitters = {TEXT("NoSuchEmitter")};
	TestEqual(TEXT("a missing emitter fails the probe"), FHyperAIStudioNiagaraContracts::Validate(Request).Status, FString(TEXT("not_live")));
	Request.ExpectEmitters.Reset();
	Request.MinParticlesPerEmitter = 1000000;
	TestEqual(TEXT("an unmet minimum fails the probe"), FHyperAIStudioNiagaraContracts::Validate(Request).Status, FString(TEXT("not_live")));
	Request.ProbeFrames = 0;
	TestEqual(TEXT("bounds are enforced"), FHyperAIStudioNiagaraContracts::Validate(Request).Status, FString(TEXT("invalid_request")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraUnversionedHandleTest,
	"HyperAIStudio.Niagara.Contracts.TemplateEmittersWithoutVersionGuid",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraUnversionedHandleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Epic's CreateNiagaraSystem copies a template whose emitter handles carry no version guid, which the engine reads
	// as the emitter's only (or exposed) version. Capture used to call that an incomplete identity and refuse to edit.
	UNiagaraSystem* System = HyperAIStudio::Niagara::Tests::MakeFountainSystem(TEXT("NS_HyperAIUnversioned_"));
	if (!System)
	{
		AddInfo(TEXT("Skipped: Niagara's Fountain template is not available."));
		return true;
	}
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };
	FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[0];
	Handle.SetInstanceVersion(FGuid());
	TestFalse(TEXT("the handle has no version guid"), Handle.GetInstance().Version.IsValid());
	TestNotNull(TEXT("and still resolves its emitter data"), Handle.GetEmitterData());
	FHyperAIStudioNiagaraValueSnapshot Snapshot;
	FString Status;
	FString Diagnostic;
	FHyperAIStudioNiagaraContracts::CaptureExact(System->GetPathName(), FHyperAIStudioNiagaraContracts::MaxReadGameThreadMs, Snapshot, Status, Diagnostic);
	TestFalse(TEXT("capture accepts it as a complete emitter identity"), Snapshot.CaptureIssues.ContainsByPredicate([](const FHyperAINiagaraIssue& Issue)
	{
		return Issue.Code == TEXT("emitter_version_identity_incomplete");
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraGraphOpShapeTest,
	"HyperAIStudio.Niagara.Mutation.GraphOpValidation",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraGraphOpShapeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Niagara::Tests;
	namespace Gate = HyperAIStudio::Niagara::ExternalEditGate;
	FString Status;
	FString Diagnostic;
	auto Accepts = [&](const FHyperAINiagaraEditOp& Op) { return Gate::ValidateOp(Op, false, Status, Diagnostic); };

	FHyperAINiagaraEditOp Stage = MakeOp(TEXT("add_simulation_stage"));
	Stage.EmitterName = TEXT("Sparks");
	Stage.Name = TEXT("Relax");
	TestTrue(TEXT("a stage with a name is accepted"), Accepts(Stage));
	Stage.Value = TEXT("8");
	TestTrue(TEXT("with an iteration count"), Accepts(Stage));
	for (const TCHAR* Bad : {TEXT("0"), TEXT("65"), TEXT("2.5"), TEXT("many")})
	{
		Stage.Value = Bad;
		TestFalse(FString::Printf(TEXT("iterations '%s' are refused"), Bad), Accepts(Stage));
	}
	TestEqual(TEXT("as invalid_iterations"), Status, FString(TEXT("invalid_iterations")));
	Stage.Value.Reset();
	Stage.Name = TEXT("Relax Pass");
	TestFalse(TEXT("stage names are identifiers"), Accepts(Stage));

	FHyperAINiagaraEditOp Code = MakeOp(TEXT("add_hlsl_module"));
	Code.EmitterName = TEXT("Sparks");
	Code.ScriptName = TEXT("ParticleUpdateScript");
	Code.Name = TEXT("Drag");
	Code.Hlsl = TEXT("// Slow down.\r\n\tParticles.Velocity *= 1.0 - 0.5 * Engine.DeltaTime;\n");
	TestTrue(TEXT("an HLSL module with newlines and tabs is accepted"), Accepts(Code));
	FHyperAINiagaraEditOp Unbalanced = Code;
	Unbalanced.Hlsl = TEXT("if (Particles.Age > 1.0) { Particles.Velocity = float3(0, 0, 0);");
	TestFalse(TEXT("an unclosed brace is refused"), Accepts(Unbalanced));
	TestEqual(TEXT("as invalid_hlsl"), Status, FString(TEXT("invalid_hlsl")));
	Unbalanced.Hlsl = TEXT("/* a comment with ( in it */ float X = 1.0; // and ] here");
	TestTrue(TEXT("brackets inside comments do not count"), Accepts(Unbalanced));
	Unbalanced.Hlsl = TEXT("   ");
	TestFalse(TEXT("an empty body is refused"), Accepts(Unbalanced));
	Unbalanced.Hlsl = FString::ChrN(HyperAIStudio::Niagara::GraphGate::MaxHlslChars + 1, TEXT('x'));
	TestFalse(TEXT("an oversized body is refused"), Accepts(Unbalanced));
	FHyperAINiagaraEditOp SystemCode = Code;
	SystemCode.ScriptName = TEXT("SystemUpdateScript");
	TestFalse(TEXT("a System script with an emitter is refused"), Accepts(SystemCode));
	SystemCode.EmitterName.Reset();
	TestTrue(TEXT("and accepted without one"), Accepts(SystemCode));
	FHyperAINiagaraEditOp StageCode = Code;
	StageCode.ScriptName = TEXT("ParticleSimulationStageScript");
	TestFalse(TEXT("a stage stack needs its stage name"), Accepts(StageCode));
	StageCode.StageName = TEXT("Relax");
	TestTrue(TEXT("and is accepted with it"), Accepts(StageCode));
	StageCode.ScriptName.Reset();
	TestTrue(TEXT("a stage name alone addresses the stage"), Accepts(StageCode));

	FHyperAINiagaraEditOp StageInput = MakeOp(TEXT("set_input_value"));
	StageInput.EmitterName = TEXT("Sparks");
	StageInput.StageName = TEXT("Relax");
	StageInput.ModuleName = TEXT("Drag");
	StageInput.InputNameStack = {TEXT("Strength")};
	StageInput.ValueType = TEXT("float");
	StageInput.Value = TEXT("1");
	TestFalse(TEXT("stage module inputs are refused rather than set on another stack"), Accepts(StageInput));
	FHyperAINiagaraEditOp StrayCode = MakeOp(TEXT("apply_stack_issue_fix"));
	StrayCode.IssueId = TEXT("issue");
	StrayCode.FixId = TEXT("fix");
	StrayCode.Hlsl = TEXT("float X = 1.0;");
	TestFalse(TEXT("hlsl on another kind is refused"), Accepts(StrayCode));

	FHyperAINiagaraEditOp StageModule = MakeOp(TEXT("add_module"));
	StageModule.EmitterName = TEXT("Sparks");
	StageModule.StageName = TEXT("Relax");
	StageModule.AssetPath = TEXT("/Niagara/Modules/Update/Forces/Drag.Drag");
	TestTrue(TEXT("a module can target a stage"), Accepts(StageModule));
	StageModule.ScriptName = TEXT("ParticleUpdateScript");
	TestFalse(TEXT("but not a stage and another stack at once"), Accepts(StageModule));

	FHyperAINiagaraEditOp Property = MakeOp(TEXT("set_emitter_property"));
	Property.EmitterName = TEXT("Sparks");
	auto PropertyAccepted = [&](const TCHAR* Name, const TCHAR* Value)
	{
		Property.Name = Name;
		Property.Value = Value;
		return Accepts(Property);
	};
	TestTrue(TEXT("sim_target gpu"), PropertyAccepted(TEXT("sim_target"), TEXT("gpu")));
	TestTrue(TEXT("local_space"), PropertyAccepted(TEXT("local_space"), TEXT("true")));
	TestTrue(TEXT("random_seed"), PropertyAccepted(TEXT("random_seed"), TEXT("1234")));
	TestTrue(TEXT("fixed_bounds"), PropertyAccepted(TEXT("fixed_bounds"), TEXT("-100,-100,0,100,100,300")));
	TestTrue(TEXT("fixed_bounds back to dynamic"), PropertyAccepted(TEXT("fixed_bounds"), TEXT("dynamic")));
	TestFalse(TEXT("inverted bounds are refused"), PropertyAccepted(TEXT("fixed_bounds"), TEXT("100,0,0,-100,1,1")));
	TestFalse(TEXT("an unknown sim target is refused"), PropertyAccepted(TEXT("sim_target"), TEXT("quantum")));
	TestFalse(TEXT("an unknown property is refused"), PropertyAccepted(TEXT("max_particles"), TEXT("10")));
	TestEqual(TEXT("as invalid_emitter_property"), Status, FString(TEXT("invalid_emitter_property")));

	FHyperAINiagaraEditOp Renderer = MakeOp(TEXT("set_renderer_property"));
	Renderer.EmitterName = TEXT("Sparks");
	Renderer.Name = TEXT("Alignment");
	Renderer.Value = TEXT("VelocityAligned");
	TestFalse(TEXT("a renderer property needs renderer_index"), Accepts(Renderer));
	Renderer.RendererIndex = 0;
	TestTrue(TEXT("and is accepted with it"), Accepts(Renderer));
	Renderer.Value = FString::ChrN(300, TEXT('a'));
	TestTrue(TEXT("values may be as long as an asset path"), Accepts(Renderer));

	// New fields are part of the sealed plan: changing any of them is a different plan.
	FHyperAIStudioNiagaraEditOpsPayload Payload;
	Payload.TargetPath = TEXT("/Game/VFX/NS_Test.NS_Test");
	Payload.BaseRevision = HashOf(TEXT('1'));
	Payload.Ops = {Code};
	const FString Sealed = FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(Payload);
	Payload.Ops[0].Hlsl += TEXT(" ");
	TestNotEqual(TEXT("the HLSL body is sealed"), Sealed, FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(Payload));
	Payload.Ops = {Code};
	Payload.Ops[0].StageName = TEXT("Relax");
	TestNotEqual(TEXT("the stage is sealed"), Sealed, FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(Payload));
	Payload.Ops = {Code};
	Payload.Ops[0].RendererIndex = 3;
	TestNotEqual(TEXT("the renderer index is sealed"), Sealed, FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(Payload));
	Payload.Ops = {Code};
	TestTrue(TEXT("the HLSL counts toward the bounded size"), Payload.GetBoundedByteSize() > 2 * Code.Hlsl.Len());

	// The Custom HLSL body is written by name through reflection; this fails when UE renames or retypes it.
	TestNotNull(TEXT("UNiagaraNodeCustomHlsl::CustomHlsl is still a string property"),
		FindFProperty<FStrProperty>(UNiagaraNodeCustomHlsl::StaticClass(), TEXT("CustomHlsl")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraStagesAndPropertiesTest,
	"HyperAIStudio.Niagara.Mutation.StagesHlslPropertiesAndUndo",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraStagesAndPropertiesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Niagara::Tests;
	namespace Gate = HyperAIStudio::Niagara::ExternalEditGate;
	namespace GraphGate = HyperAIStudio::Niagara::GraphGate;
	UNiagaraSystem* System = MakeFountainSystem(TEXT("NS_HyperAIStages_"));
	if (!System)
	{
		AddInfo(TEXT("Skipped: Niagara's Fountain template is not available."));
		return true;
	}
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };
	const FString Emitter = System->GetEmitterHandles()[0].GetName().ToString();
	auto EmitterData = [System]() { return System->GetEmitterHandles()[0].GetEmitterData(); };
	UNiagaraSpriteRendererProperties* Sprite = EmitterData() && EmitterData()->GetRenderers().Num() > 0
		? Cast<UNiagaraSpriteRendererProperties>(EmitterData()->GetRenderers()[0]) : nullptr;
	if (!TestNotNull(TEXT("the Fountain copy renders sprites"), Sprite))
	{
		return false;
	}
	TestEqual(TEXT("and simulates on the CPU"), EmitterData()->SimTarget, ENiagaraSimTarget::CPUSim);
	const ENiagaraSpriteAlignment OriginalAlignment = Sprite->Alignment;
	const FVector2D OriginalGrid = Sprite->SubImageSize;

	auto Op = [&Emitter](const TCHAR* Kind)
	{
		FHyperAINiagaraEditOp Result = MakeOp(Kind);
		Result.EmitterName = Emitter;
		return Result;
	};
	FHyperAINiagaraEditOp ToGpu = Op(TEXT("set_emitter_property"));
	ToGpu.Name = TEXT("sim_target");
	ToGpu.Value = TEXT("gpu");
	FHyperAINiagaraEditOp Stage = Op(TEXT("add_simulation_stage"));
	Stage.Name = TEXT("Settle");
	Stage.Value = TEXT("2");
	FHyperAINiagaraEditOp StageCode = Op(TEXT("add_hlsl_module"));
	StageCode.ScriptName = TEXT("ParticleSimulationStageScript");
	StageCode.StageName = TEXT("Settle");
	StageCode.Name = TEXT("Damp");
	StageCode.Hlsl = TEXT("// Bleed off a little speed on every stage pass.\nParticles.Velocity = Particles.Velocity * 0.98;");
	FHyperAINiagaraEditOp UpdateCode = Op(TEXT("add_hlsl_module"));
	UpdateCode.ScriptName = TEXT("ParticleUpdateScript");
	UpdateCode.Name = TEXT("Swirl");
	UpdateCode.Hlsl = TEXT("float3 Offset = Particles.Position;\nParticles.Velocity += float3(-Offset.y, Offset.x, 0.0) * 0.5 * Engine.DeltaTime;");
	FHyperAINiagaraEditOp Align = Op(TEXT("set_renderer_property"));
	Align.RendererIndex = 0;
	Align.Name = TEXT("Alignment");
	Align.Value = TEXT("VelocityAligned");
	FHyperAINiagaraEditOp Grid = Align;
	Grid.Name = TEXT("SubImageSize");
	Grid.Value = TEXT("2,2");
	const TArray<FHyperAINiagaraEditOp> Plan = {ToGpu, Stage, StageCode, UpdateCode, Align, Grid};

	FString Status;
	FString Diagnostic;
	for (const FHyperAINiagaraEditOp& Each : Plan)
	{
		const bool bShaped = Gate::ValidateOp(Each, true, Status, Diagnostic);
		TestTrue(FString::Printf(TEXT("%s %s passes its shape check (%s)"), *Each.Kind, *Each.Name, *Diagnostic), bShaped);
	}
	TestFalse(TEXT("a stage on the CPU emitter is refused against the System"), GraphGate::ValidatePlanAgainstSystem(*System, {Stage}, Status, Diagnostic));
	TestEqual(TEXT("with the sim_target remedy"), Status, FString(TEXT("simulation_stage_needs_gpu")));
	const bool bPlanValid = GraphGate::ValidatePlanAgainstSystem(*System, Plan, Status, Diagnostic);
	TestTrue(FString::Printf(TEXT("switching to GPU first makes the plan valid (%s)"), *Diagnostic), bPlanValid);

	auto Refused = [&](const TArray<FHyperAINiagaraEditOp>& Ops, const TCHAR* Expected)
	{
		const bool bValid = GraphGate::ValidatePlanAgainstSystem(*System, Ops, Status, Diagnostic);
		TestFalse(FString::Printf(TEXT("%s is caught in the dry run"), Expected), bValid);
		TestEqual(FString::Printf(TEXT("as %s (%s)"), Expected, *Diagnostic), Status, FString(Expected));
	};
	FHyperAINiagaraEditOp BadIndex = Align;
	BadIndex.RendererIndex = 5;
	Refused({BadIndex}, TEXT("renderer_index_out_of_range"));
	FHyperAINiagaraEditOp Typo = Align;
	Typo.Name = TEXT("Alignmnt");
	Refused({Typo}, TEXT("renderer_property_not_found"));
	TestTrue(TEXT("and the typo's diagnostic suggests the real name"), Diagnostic.Contains(TEXT("Alignment")));
	FHyperAINiagaraEditOp BadValue = Align;
	BadValue.Value = TEXT("Sideways");
	Refused({BadValue}, TEXT("invalid_renderer_property_value"));
	FHyperAINiagaraEditOp Binding = Align;
	Binding.Name = TEXT("PositionBinding");
	Refused({Binding}, TEXT("renderer_property_not_settable"));
	FHyperAINiagaraEditOp NoStage = StageCode;
	NoStage.StageName = TEXT("Nope");
	Refused({ToGpu, Stage, NoStage}, TEXT("stage_not_found"));
	FHyperAINiagaraEditOp Twin = MakeOp(TEXT("add_emitter"));
	Twin.AssetPath = FountainPath;
	Twin.Name = Emitter;
	Refused({Twin}, TEXT("emitter_name_taken"));
	FHyperAINiagaraEditOp Ghost = Align;
	Ghost.EmitterName = TEXT("Nope");
	Refused({Ghost}, TEXT("emitter_not_found"));

	int32 Applied = 0;
	TArray<FString> PerOp;
	FString Error;
	const bool bApplied = Gate::ApplyOps(*System, Plan, Applied, PerOp, Error);
	TestTrue(FString::Printf(TEXT("the plan applies (%s)"), *Error), bApplied);
	TestEqual(TEXT("every op landed"), Applied, Plan.Num());
	AddInfo(TEXT("Per op: ") + FString::Join(PerOp, TEXT(", ")));
	if (!bApplied)
	{
		return false;
	}
	TestEqual(TEXT("the emitter simulates on the GPU"), EmitterData()->SimTarget, ENiagaraSimTarget::GPUComputeSim);
	TestEqual(TEXT("sprites align to velocity"), Sprite->Alignment, ENiagaraSpriteAlignment::VelocityAligned);
	TestEqual(TEXT("the flipbook grid is 2x2"), Sprite->SubImageSize, FVector2D(2.0, 2.0));

	TArray<FHyperAINiagaraEmitterTopology> Topology;
	bool bTruncated = false;
	FString TopologyError;
	TestTrue(FString::Printf(TEXT("topology reads (%s)"), *TopologyError), Gate::ReadTopology(*System, Topology, bTruncated, TopologyError));
	const FHyperAINiagaraSimStageTopology* Settle = Topology.Num() == 1
		? Topology[0].SimulationStages.FindByPredicate([](const FHyperAINiagaraSimStageTopology& Each) { return Each.StageName == TEXT("Settle"); })
		: nullptr;
	if (TestNotNull(TEXT("inspect lists the stage"), Settle))
	{
		TestEqual(TEXT("iterating particles"), Settle->IterationSource, FString(TEXT("particles")));
		TestEqual(TEXT("twice a frame"), Settle->Iterations, 2);
		TestTrue(TEXT("with the HLSL module in its stack"), Settle->Modules.Num() == 1 && Settle->Modules[0].ModuleName == TEXT("Damp"));
	}
	const FHyperAINiagaraScriptStackTopology* Update = Topology.Num() == 1
		? Topology[0].ScriptStacks.FindByPredicate([](const FHyperAINiagaraScriptStackTopology& Each) { return Each.ScriptName == TEXT("ParticleUpdateScript"); })
		: nullptr;
	TestTrue(TEXT("the update HLSL module runs last in Particle Update"),
		Update && Update->Modules.Num() > 0 && Update->Modules.Last().ModuleName == TEXT("Swirl"));
	const TArray<FString> ScratchPads = GraphGate::ReadScratchPadModules(*System);
	TestTrue(TEXT("both modules are scratch pads of the System"), ScratchPads.Contains(TEXT("Damp")) && ScratchPads.Contains(TEXT("Swirl")));

	System->RequestCompile(/*bForce=*/false);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
	HyperAIStudio::Niagara::ExternalEditGate::FDiagnostics Diagnostics;
	FString DiagnosticsError;
	TestTrue(FString::Printf(TEXT("diagnostics read (%s)"), *DiagnosticsError), Gate::ReadDiagnostics(*System, 64, Diagnostics, DiagnosticsError));
	TestFalse(TEXT("the System compiles without errors: ") + DescribeIssues(Diagnostics), Diagnostics.bCompileHasErrors);
	TestEqual(TEXT("and has no stack errors: ") + DescribeIssues(Diagnostics), Diagnostics.StackErrorCount, 0);
	bool bStageCompiled = false;
	if (const UNiagaraScript* Gpu = EmitterData()->GetGPUComputeScript())
	{
		for (const FSimulationStageMetaData& Meta : Gpu->GetSimulationStageMetaData())
		{
			bStageCompiled |= Meta.SimulationStageName == FName(TEXT("Settle"));
		}
	}
	TestTrue(TEXT("the GPU script compiled the Settle stage"), bStageCompiled);

	// The whole plan was one transaction: one undo takes all of it back, one redo restores it.
	if (!GEditor)
	{
		AddWarning(TEXT("No editor transaction buffer; undo not checked."));
		return true;
	}
	TestTrue(TEXT("one undo step"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo puts the emitter back on the CPU"), EmitterData()->SimTarget, ENiagaraSimTarget::CPUSim);
	TestEqual(TEXT("removes the stage"), EmitterData()->GetSimulationStages().Num(), 0);
	TestEqual(TEXT("restores the sprite alignment"), Sprite->Alignment, OriginalAlignment);
	TestEqual(TEXT("and the flipbook grid"), Sprite->SubImageSize, OriginalGrid);
	TestTrue(TEXT("and drops the scratch pads"), GraphGate::ReadScratchPadModules(*System).IsEmpty());
	TestTrue(TEXT("one redo step"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo brings the stage back"), EmitterData()->GetSimulationStages().Num(), 1);
	TestEqual(TEXT("and the alignment"), Sprite->Alignment, ENiagaraSpriteAlignment::VelocityAligned);
	TestEqual(TEXT("and both scratch pads"), GraphGate::ReadScratchPadModules(*System).Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraHlslRuntimeTest,
	"HyperAIStudio.Niagara.Mutation.HlslModuleChangesTheSimulation",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraHlslRuntimeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Niagara::Tests;
	namespace Gate = HyperAIStudio::Niagara::ExternalEditGate;
	UNiagaraSystem* System = MakeFountainSystem(TEXT("NS_HyperAIHlsl_"));
	if (!System)
	{
		AddInfo(TEXT("Skipped: Niagara's Fountain template is not available."));
		return true;
	}
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };
	FHyperAINiagaraValidateRequest Probe;
	Probe.TargetPath = System->GetPathName();
	Probe.Policy = TEXT("runtime_probe");
	Probe.ProbeFrames = 120;
	auto LiveParticles = [&Probe]()
	{
		const FHyperAINiagaraValidateReport Report = FHyperAIStudioNiagaraContracts::Validate(Probe);
		return Report.RuntimeProbe.Emitters.Num() == 1 ? Report.RuntimeProbe.Emitters[0].NumParticles : -1;
	};
	const int32 Before = LiveParticles();
	TestTrue(FString::Printf(TEXT("the Fountain has particles to begin with (%d)"), Before), Before > 10);

	// Age every particle past its lifetime at the end of Particle Update: Particle State kills it on the next frame,
	// so only particles born this frame are left. A count that collapses proves the HLSL ran in the simulation.
	FHyperAINiagaraEditOp Expire = MakeOp(TEXT("add_hlsl_module"));
	Expire.EmitterName = System->GetEmitterHandles()[0].GetName().ToString();
	Expire.ScriptName = TEXT("ParticleUpdateScript");
	Expire.Name = TEXT("ExpireNow");
	Expire.Hlsl = TEXT("Particles.Age = Particles.Lifetime + 1.0;");
	int32 Applied = 0;
	TArray<FString> PerOp;
	FString Error;
	TestTrue(FString::Printf(TEXT("the HLSL module applies (%s)"), *Error), Gate::ApplyOps(*System, {Expire}, Applied, PerOp, Error));
	System->RequestCompile(/*bForce=*/false);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
	HyperAIStudio::Niagara::ExternalEditGate::FDiagnostics Diagnostics;
	FString DiagnosticsError;
	Gate::ReadDiagnostics(*System, 64, Diagnostics, DiagnosticsError);
	TestFalse(TEXT("the CPU System compiles the HLSL without errors: ") + DescribeIssues(Diagnostics), Diagnostics.bCompileHasErrors);
	const int32 After = LiveParticles();
	TestTrue(FString::Printf(TEXT("the HLSL module ran: %d live particles before, %d after"), Before, After), After >= 0 && After * 4 < Before);
	return true;
}

namespace HyperAIStudio::Niagara::Tests
{
	/** The body of the scratch pad module's Custom HLSL node, read the way set_hlsl_module writes it. */
	FString ScratchHlsl(const UNiagaraSystem& System, const FString& Module)
	{
		const FStrProperty* Property = FindFProperty<FStrProperty>(UNiagaraNodeCustomHlsl::StaticClass(), TEXT("CustomHlsl"));
		for (const UNiagaraScript* Script : System.ScratchPadScripts)
		{
			const UNiagaraScriptSource* Source = Script && Script->GetName() == Module ? Cast<UNiagaraScriptSource>(Script->GetLatestSource()) : nullptr;
			const UNiagaraGraph* Graph = Source ? Source->NodeGraph.Get() : nullptr;
			for (const UEdGraphNode* Node : Graph ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
			{
				if (const UNiagaraNodeCustomHlsl* Code = Cast<UNiagaraNodeCustomHlsl>(Node))
				{
					return Property ? Property->GetPropertyValue_InContainer(Code) : FString();
				}
			}
		}
		return FString();
	}

	FHyperAINiagaraLiveEmitter LiveEmitter(const TCHAR* Name, const TCHAR* State, const int64 Particles, const int64 TotalSpawned)
	{
		FHyperAINiagaraLiveEmitter Out;
		Out.Name = Name;
		Out.State = State;
		Out.Particles = Particles;
		Out.TotalSpawned = TotalSpawned;
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraScratchPadEditTest,
	"HyperAIStudio.Niagara.Mutation.ScratchPadHlslEdit",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraScratchPadEditTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Niagara::Tests;
	namespace Gate = HyperAIStudio::Niagara::ExternalEditGate;
	namespace GraphGate = HyperAIStudio::Niagara::GraphGate;
	UNiagaraSystem* System = MakeFountainSystem(TEXT("NS_HyperAIScratch_"));
	if (!System)
	{
		AddInfo(TEXT("Skipped: Niagara's Fountain template is not available."));
		return true;
	}
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };
	const FString Emitter = System->GetEmitterHandles()[0].GetName().ToString();
	FHyperAINiagaraEditOp Add = MakeOp(TEXT("add_hlsl_module"));
	Add.EmitterName = Emitter;
	Add.ScriptName = TEXT("ParticleUpdateScript");
	Add.Name = TEXT("Drift");
	Add.Hlsl = TEXT("Particles.Velocity += float3(0.0, 0.0, 10.0) * Engine.DeltaTime;");
	FHyperAINiagaraEditOp Set = MakeOp(TEXT("set_hlsl_module"));
	Set.Name = TEXT("Drift");
	Set.Hlsl = TEXT("// Sideways now.\nParticles.Velocity += float3(25.0, 0.0, 0.0) * Engine.DeltaTime;");

	FString Status;
	FString Diagnostic;
	TestTrue(TEXT("set_hlsl_module passes its shape check: ") + Diagnostic, Gate::ValidateOp(Set, true, Status, Diagnostic));
	FHyperAINiagaraEditOp Addressed = Set;
	Addressed.EmitterName = Emitter;
	TestFalse(TEXT("with an emitter address it is refused"), Gate::ValidateOp(Addressed, true, Status, Diagnostic));
	TestEqual(TEXT("as invalid_op_address"), Status, FString(TEXT("invalid_op_address")));
	FHyperAINiagaraEditOp Empty = Set;
	Empty.Hlsl.Reset();
	TestFalse(TEXT("without hlsl it is refused"), Gate::ValidateOp(Empty, true, Status, Diagnostic));
	FHyperAINiagaraEditOp Ghost = Set;
	Ghost.Name = TEXT("Nope");
	TestFalse(TEXT("a module the System does not have is caught in the dry run"), GraphGate::ValidatePlanAgainstSystem(*System, {Ghost}, Status, Diagnostic));
	TestEqual(TEXT("as scratch_module_not_found"), Status, FString(TEXT("scratch_module_not_found")));
	TestTrue(TEXT("an edit of a module an earlier op adds is valid: ") + Diagnostic, GraphGate::ValidatePlanAgainstSystem(*System, {Add, Set}, Status, Diagnostic));

	int32 Applied = 0;
	TArray<FString> PerOp;
	FString Error;
	if (!TestTrue(TEXT("the module is added: ") + Error, Gate::ApplyOps(*System, {Add}, Applied, PerOp, Error)))
	{
		return false;
	}
	TestEqual(TEXT("with its first body"), ScratchHlsl(*System, TEXT("Drift")), Add.Hlsl);
	TestTrue(TEXT("the edit applies on its own plan: ") + Error, Gate::ApplyOps(*System, {Set}, Applied, PerOp, Error) && Applied == 1);
	TestEqual(TEXT("and replaces the body"), ScratchHlsl(*System, TEXT("Drift")), Set.Hlsl);
	TestTrue(TEXT("still one scratch pad"), GraphGate::ReadScratchPadModules(*System).Num() == 1);
	System->RequestCompile(/*bForce=*/false);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
	HyperAIStudio::Niagara::ExternalEditGate::FDiagnostics Diagnostics;
	FString DiagnosticsError;
	Gate::ReadDiagnostics(*System, 64, Diagnostics, DiagnosticsError);
	TestFalse(TEXT("the System compiles the new body: ") + DescribeIssues(Diagnostics), Diagnostics.bCompileHasErrors);
	if (!GEditor)
	{
		return true;
	}
	TestTrue(TEXT("one undo step"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo restores the first body"), ScratchHlsl(*System, TEXT("Drift")), Add.Hlsl);
	TestTrue(TEXT("one redo step"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo brings the edit back"), ScratchHlsl(*System, TEXT("Drift")), Set.Hlsl);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraLiveSampleTest,
	"HyperAIStudio.Niagara.Live.SampleAndIssues",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraLiveSampleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Niagara::Tests;
	namespace Live = HyperAIStudio::Niagara::Live;
	TestEqual(TEXT("an invalid box prints as empty"), Live::BoundsText(FBox(ForceInit)), FString(TEXT("empty")));
	TestEqual(TEXT("a box prints its corners"), Live::BoundsText(FBox(FVector(-1, -2, -3), FVector(4, 5, 6))), FString(TEXT("min (-1,-2,-3) max (4,5,6)")));

	FHyperAINiagaraLiveReport Before;
	FHyperAINiagaraLiveSystem& Fire = Before.Systems.AddDefaulted_GetRef();
	Fire.System = TEXT("/Game/FX/NS_Fire.NS_Fire");
	Fire.Component = TEXT("Torch.Fire");
	Fire.ComponentPath = TEXT("/Game/Map.Map:PersistentLevel.Torch.Fire");
	Fire.Emitters = {LiveEmitter(TEXT("Flames"), TEXT("active"), 40, 100), LiveEmitter(TEXT("Sparks"), TEXT("active"), 0, 50),
		LiveEmitter(TEXT("Smoke"), TEXT("active"), 10, 500)};
	FHyperAINiagaraLiveReport After = Before;
	After.Systems[0].Emitters[0].TotalSpawned = 160;
	// Smoke restarted between the reads: its counter went back.
	After.Systems[0].Emitters[2].TotalSpawned = 30;
	FHyperAINiagaraLiveSystem& Late = After.Systems.AddDefaulted_GetRef();
	Late.System = TEXT("/Game/FX/NS_Hit.NS_Hit");
	Late.Component = TEXT("Hit");
	Late.ComponentPath = TEXT("/Game/Map.Map:PersistentLevel.Hit");
	Late.Particles = 5;
	Late.Emitters = {LiveEmitter(TEXT("Burst"), TEXT("active"), 5, 5)};
	Live::ApplySample(Before, 0.5, After);
	const TArray<FHyperAINiagaraLiveEmitter>& Emitters = After.Systems[0].Emitters;
	TestTrue(TEXT("Flames spawned 60 in the sample"), Emitters[0].SpawnedInSample == 60 && Emitters[0].Spawning == TEXT("yes"));
	TestTrue(TEXT("Sparks spawned nothing"), Emitters[1].SpawnedInSample == 0 && Emitters[1].Spawning == TEXT("no"));
	TestTrue(TEXT("a restarted emitter counts what it spawned since"), Emitters[2].SpawnedInSample == 30 && Emitters[2].Spawning == TEXT("yes"));
	TestTrue(TEXT("a system that appeared in the sample counts all of it"), After.Systems[1].Emitters[0].SpawnedInSample == 5);
	TestTrue(TEXT("the sample length is kept"), FMath::IsNearlyEqual(After.SampledSeconds, 0.5));

	Live::Finish(1, After);
	if (!TestEqual(TEXT("top_n 1 keeps one system"), After.Systems.Num(), 1)) return false;
	TestEqual(TEXT("the one with issues first"), After.Systems[0].System, FString(TEXT("/Game/FX/NS_Fire.NS_Fire")));
	TestTrue(TEXT("and a note says one more was left out"), After.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("1 more")); }));
	TestEqual(TEXT("Sparks, active with nothing alive or spawning, is an issue"), After.Systems[0].Emitters[1].Issues.Num(), 1);
	TestEqual(TEXT("Flames is not"), After.Systems[0].Emitters[0].Issues.Num(), 0);
	TestTrue(TEXT("the flat list names system, component and emitter"), After.Errors.Num() == 1
		&& After.Errors[0].StartsWith(TEXT("NS_Fire (Torch.Fire) / Sparks:")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraLiveWorldTest,
	"HyperAIStudio.Niagara.Live.ReadsAPlayingSystem",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraLiveWorldTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Niagara::Tests;
	namespace Live = HyperAIStudio::Niagara::Live;
	if (!GEditor || !GEditor->PlayWorld)
	{
		const UHyperAINiagaraLiveResult* Refused = UHyperAIStudioNiagaraToolset::hyper_niagara_live(FHyperAINiagaraLiveRequest());
		TestTrue(TEXT("without Play In Editor the tool answers no_pie_world at once"), Refused && Refused->Value.Status == TEXT("no_pie_world"));
		FHyperAINiagaraLiveRequest Once;
		TestEqual(TEXT("and so does the adapter's single read"), Live::ReadOnce(Once).Status, FString(TEXT("no_pie_world")));
	}
	UNiagaraSystem* System = MakeFountainSystem(TEXT("NS_HyperAILive_"));
	if (!System)
	{
		AddInfo(TEXT("Skipped: Niagara's Fountain template is not available."));
		return true;
	}
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };
	// A world of its own with the Fountain playing in it, stepped by hand the way the runtime probe steps it.
	FPreviewScene Scene{FPreviewScene::ConstructionValues()};
	UNiagaraComponent* Component = NewObject<UNiagaraComponent>(GetTransientPackage(), NAME_None, RF_Transient);
	Component->SetAutoActivate(false);
	Component->SetAsset(System);
	Component->SetForceSolo(true);
	Scene.AddComponent(Component, FTransform::Identity);
	Component->Activate(/*bReset=*/true);
	Component->AdvanceSimulation(30, 1.f / 30.f);
	UWorld& World = *Scene.GetWorld();

	FHyperAINiagaraLiveReport First;
	Live::Read(World, FString(), /*bIncludeInactive=*/false, First);
	if (!TestEqual(TEXT("the playing Fountain is read"), First.Systems.Num(), 1)) return false;
	const FHyperAINiagaraLiveSystem& Read = First.Systems[0];
	TestEqual(TEXT("by its asset"), Read.System, System->GetPathName());
	TestEqual(TEXT("as active"), Read.State, FString(TEXT("active")));
	if (!TestEqual(TEXT("with its one emitter"), Read.Emitters.Num(), 1)) return false;
	TestEqual(TEXT("on the CPU"), Read.Emitters[0].SimTarget, FString(TEXT("cpu")));
	TestTrue(FString::Printf(TEXT("with particles alive (%lld) and spawned (%lld)"), Read.Emitters[0].Particles, Read.Emitters[0].TotalSpawned),
		Read.Emitters[0].Particles > 0 && Read.Emitters[0].TotalSpawned >= Read.Emitters[0].Particles);
	TestTrue(TEXT("and bounds around them"), Read.Emitters[0].Bounds != TEXT("empty") || !Read.Emitters[0].bDynamicBounds);
	TestEqual(TEXT("counted in the totals"), First.Particles, Read.Emitters[0].Particles);
	TestEqual(TEXT("one component playing"), First.ActiveComponents, 1);

	FHyperAINiagaraLiveReport Filtered;
	Live::Read(World, TEXT("NoSuchSystem"), false, Filtered);
	TestEqual(TEXT("a filter that matches nothing reads nothing"), Filtered.Systems.Num(), 0);

	Component->AdvanceSimulation(15, 1.f / 30.f);
	FHyperAINiagaraLiveReport Second;
	Live::Read(World, FString(), false, Second);
	Live::ApplySample(First, 0.5, Second);
	Live::Finish(20, Second);
	TestTrue(TEXT("between the reads the Fountain kept spawning"), Second.Systems.Num() == 1 && Second.Systems[0].Emitters.Num() == 1
		&& Second.Systems[0].Emitters[0].Spawning == TEXT("yes"));
	TestEqual(TEXT("and has nothing wrong: ") + FString::Join(Second.Errors, TEXT(" | ")), Second.Errors.Num(), 0);

	Component->Deactivate();
	Component->DeactivateImmediate();
	FHyperAINiagaraLiveReport Stopped;
	Live::Read(World, FString(), /*bIncludeInactive=*/true, Stopped);
	TestTrue(TEXT("a stopped component is listed only when asked"), Stopped.Systems.Num() == 1 && Stopped.ActiveComponents == 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
