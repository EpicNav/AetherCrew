// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioNiagaraToolset.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioNiagaraDiscover.h"
#include "HyperAIStudioNiagaraExternalEditGate.h"
#include "HyperAIStudioNiagaraGraphGate.h"
#include "HyperAIStudioNiagaraPreview.h"
#include "HyperAIStudioNiagaraRecipes.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioTrustedExecution.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "ObjectTools.h"
#include "UObject/SoftObjectPath.h"

namespace HyperAIStudio::Niagara::AuthoringTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	const TCHAR* const FountainPath = TEXT("/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain");
	const TCHAR* const ExplosionPath = TEXT("/Niagara/DefaultAssets/Templates/Systems/SimpleExplosion.SimpleExplosion");
	const TCHAR* const RandomRangeFloat = TEXT("/Niagara/DynamicInputs/UniformRange/V2/RandomRangeFloat.RandomRangeFloat");
	const TCHAR* const ColorFromCurve = TEXT("/Niagara/DynamicInputs/ValueFromCurve/ColorFromCurve.ColorFromCurve");

	FString NewPath(const TCHAR* Prefix)
	{
		const FString Name = FString(Prefix) + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
		return FString::Printf(TEXT("/Game/__HyperAIStudioTests/%s.%s"), *Name, *Name);
	}

	/** An unsaved System under /Game with one emitter copied from Niagara's Fountain template. */
	UNiagaraSystem* MakeFountain(const TCHAR* Prefix)
	{
		UNiagaraEmitter* Fountain = LoadObject<UNiagaraEmitter>(nullptr, FountainPath);
		if (!Fountain)
		{
			return nullptr;
		}
		const FString Path = NewPath(Prefix);
		UPackage* Package = CreatePackage(*FPackageName::ObjectPathToPackageName(Path));
		UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, FName(*FPackageName::ObjectPathToObjectName(Path)), RF_Public | RF_Standalone | RF_Transactional);
		UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/true);
		FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Fountain, Fountain->GetExposedVersion().VersionGuid);
		System->RequestCompile(/*bForce=*/false);
		System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
		return System;
	}

	FHyperAINiagaraEditOp Op(const TCHAR* Kind, const FString& Emitter = FString(), const TCHAR* Script = TEXT(""), const FString& Module = FString())
	{
		FHyperAINiagaraEditOp Out;
		Out.Kind = Kind;
		Out.EmitterName = Emitter;
		Out.ScriptName = Script;
		Out.ModuleName = Module;
		return Out;
	}

	FHyperAINiagaraEditOp Input(const FString& Emitter, const TCHAR* Script, const FString& Module, const TArray<FString>& Stack, const TCHAR* Type,
		const FString& Value, const FString& Asset = FString())
	{
		FHyperAINiagaraEditOp Out = Op(TEXT("set_input_value"), Emitter, Script, Module);
		Out.InputNameStack = Stack;
		Out.ValueType = Type;
		Out.Value = Value;
		Out.AssetPath = Asset;
		return Out;
	}

	const FHyperAINiagaraModuleTopology* FindModule(const TArray<FHyperAINiagaraEmitterTopology>& Topology, const FString& Script, const TCHAR* AssetContains,
		FString* OutEmitter = nullptr)
	{
		for (const FHyperAINiagaraEmitterTopology& Emitter : Topology)
		{
			for (const FHyperAINiagaraScriptStackTopology& Stack : Emitter.ScriptStacks)
			{
				for (const FHyperAINiagaraModuleTopology& Module : Stack.Modules)
				{
					if (Stack.ScriptName == Script && Module.ScriptAssetPath.Contains(AssetContains))
					{
						if (OutEmitter) *OutEmitter = Emitter.EmitterName;
						return &Module;
					}
				}
			}
		}
		return nullptr;
	}

	FString ValueOf(const FHyperAINiagaraModuleTopology& Module, const FString& Input)
	{
		for (const FString& Line : Module.InputValues)
		{
			if (Line.StartsWith(Input + TEXT(" = ")))
			{
				return Line.Mid(Input.Len() + 3);
			}
		}
		return FString();
	}

	bool ReadTopology(UNiagaraSystem& System, TArray<FHyperAINiagaraEmitterTopology>& Out, bool bValues = true)
	{
		bool bTruncated = false;
		FString Error;
		return ExternalEditGate::ReadTopology(System, Out, bTruncated, Error, bValues);
	}

	int32 LiveParticlesAfter(UNiagaraSystem& System, float Seconds)
	{
		FHyperAINiagaraValidateRequest Probe;
		Probe.TargetPath = System.GetPathName();
		Probe.Policy = TEXT("runtime_probe");
		Probe.ProbeFrames = FMath::Max(1, FMath::RoundToInt(Seconds * 60.f));
		Probe.MinParticlesPerEmitter = 1;
		const FHyperAINiagaraValidateReport Report = FHyperAIStudioNiagaraContracts::Validate(Probe);
		int32 Count = 0;
		for (const FHyperAINiagaraEmitterRuntime& Emitter : Report.RuntimeProbe.Emitters)
		{
			Count += Emitter.NumParticles;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioNiagaraRichValueShapeTest, "HyperAIStudio.Niagara.Authoring.RichInputValueShapes",
	HyperAIStudio::Niagara::AuthoringTests::Flags)

bool FHyperAIStudioNiagaraRichValueShapeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Niagara::AuthoringTests;
	namespace Gate = HyperAIStudio::Niagara::ExternalEditGate;
	FString Status;
	FString Diagnostic;
	auto Accepts = [&](const FHyperAINiagaraEditOp& Each) { return Gate::ValidateOp(Each, true, Status, Diagnostic); };
	auto Value = [](const TCHAR* Type, const FString& Text, const FString& Asset = FString())
	{
		return Input(TEXT("Fire"), TEXT("ParticleUpdateScript"), TEXT("ScaleColor"), {TEXT("Scale RGBA")}, Type, Text, Asset);
	};
	TestTrue(TEXT("enum by name"), Accepts(Value(TEXT("enum"), TEXT("Once"))));
	TestTrue(TEXT("linked to an attribute"), Accepts(Value(TEXT("linked"), TEXT("Particles.NormalizedAge"))));
	TestFalse(TEXT("a link needs a namespaced name"), Accepts(Value(TEXT("linked"), TEXT("NormalizedAge"))));
	TestTrue(TEXT("an hlsl expression"), Accepts(Value(TEXT("hlsl_expression"), TEXT("float4(1, 0.5, 0, 1) * (1 - Particles.NormalizedAge)"))));
	TestFalse(TEXT("an expression is one rvalue, not statements"), Accepts(Value(TEXT("hlsl_expression"), TEXT("float X = 1; return X;"))));
	TestFalse(TEXT("an expression with unbalanced brackets"), Accepts(Value(TEXT("hlsl_expression"), TEXT("sin(Engine.Time"))));
	TestTrue(TEXT("a dynamic input by path"), Accepts(Value(TEXT("dynamic_input"), FString(), RandomRangeFloat)));
	TestFalse(TEXT("a module is not a dynamic input"), Accepts(Value(TEXT("dynamic_input"), FString(), TEXT("/Niagara/Modules/Update/Color/ScaleColor.ScaleColor"))));
	TestTrue(TEXT("a data interface as JSON"), Accepts(Value(TEXT("data_interface"), TEXT("{\n  \"bUseLUT\": true\n}"))));
	TestFalse(TEXT("data interface JSON must be an object"), Accepts(Value(TEXT("data_interface"), TEXT("[1, 2]"))));
	TestTrue(TEXT("float curve keys"), Accepts(Value(TEXT("curve"), TEXT("0:1; 0.5:0.6; 1:0"))));
	TestTrue(TEXT("float curve keys, comma separated"), Accepts(Value(TEXT("curve"), TEXT("0:0, 1:1"))));
	TestTrue(TEXT("colour curve keys"), Accepts(Value(TEXT("color_curve"), TEXT("0:1,0.8,0.3,1; 1:0.2,0,0,0"))));
	TestTrue(TEXT("colour keys may leave alpha out"), Accepts(Value(TEXT("color_curve"), TEXT("0:1,1,1; 1:0,0,0"))));
	TestFalse(TEXT("curve times must increase"), Accepts(Value(TEXT("curve"), TEXT("0.5:1; 0.2:0"))));
	TestFalse(TEXT("a vector curve needs three values a key"), Accepts(Value(TEXT("vector_curve"), TEXT("0:1,2"))));
	FHyperAINiagaraEditOp Stray = Value(TEXT("float"), TEXT("1"), RandomRangeFloat);
	TestFalse(TEXT("asset_path goes only with dynamic_input"), Accepts(Stray));
	TestEqual(TEXT("as invalid_op_address"), Status, FString(TEXT("invalid_op_address")));
	FHyperAINiagaraEditOp Long = Value(TEXT("data_interface"), TEXT("{\"Curve\": \"") + FString::ChrN(2000, TEXT('x')) + TEXT("\"}"));
	TestTrue(TEXT("long values are allowed for JSON"), Accepts(Long));
	Long.ValueType = TEXT("float");
	TestFalse(TEXT("but not for plain values"), Accepts(Long));

	FString Json;
	FString Error;
	FHyperAINiagaraEditOp Create = Op(TEXT("create_system"));
	Create.AssetPath = ExplosionPath;
	Create.Value = TEXT("Nope");
	TestTrue(TEXT("create_system's shape is accepted"), Accepts(Create));
	TestFalse(TEXT("keeping an emitter the template lacks is caught in the walk"),
		HyperAIStudio::Niagara::GraphGate::ValidatePlanAgainstSystem(nullptr, {Create}, Status, Diagnostic));
	Create.AssetPath = TEXT("/Niagara/DefaultAssets/Templates/Systems/NoSuchSystem.NoSuchSystem");
	TestFalse(TEXT("a missing template is refused"), Accepts(Create));
	TestEqual(TEXT("as system_template_not_found"), Status, FString(TEXT("system_template_not_found")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioNiagaraRichValueApplyTest, "HyperAIStudio.Niagara.Authoring.RichInputValuesApplyAndReadBack",
	HyperAIStudio::Niagara::AuthoringTests::Flags)

bool FHyperAIStudioNiagaraRichValueApplyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Niagara::AuthoringTests;
	namespace Gate = HyperAIStudio::Niagara::ExternalEditGate;
	UNiagaraSystem* System = MakeFountain(TEXT("NS_HyperAIRich_"));
	if (!System)
	{
		AddInfo(TEXT("Skipped: Niagara's Fountain template is not available."));
		return true;
	}
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };

	TArray<FHyperAINiagaraEmitterTopology> Topology;
	TestTrue(TEXT("the Fountain's topology and values read"), ReadTopology(*System, Topology));
	FString Emitter;
	const FHyperAINiagaraModuleTopology* SpawnRate = FindModule(Topology, TEXT("EmitterUpdateScript"), TEXT("SpawnRate"), &Emitter);
	const FHyperAINiagaraModuleTopology* Initialize = FindModule(Topology, TEXT("ParticleSpawnScript"), TEXT("InitializeParticle"));
	const FHyperAINiagaraModuleTopology* State = FindModule(Topology, TEXT("EmitterUpdateScript"), TEXT("EmitterState"));
	if (!TestNotNull(TEXT("a SpawnRate module"), SpawnRate) || !TestNotNull(TEXT("an Initialize Particle module"), Initialize)
		|| !TestNotNull(TEXT("an Emitter State module"), State))
	{
		return false;
	}
	TestEqual(TEXT("every input has a value_type"), SpawnRate->InputTypes.Num(), SpawnRate->InputNames.Num());
	TestTrue(TEXT("SpawnRate's own input is a float"), SpawnRate->InputNames.Contains(TEXT("SpawnRate"))
		&& SpawnRate->InputTypes[SpawnRate->InputNames.IndexOfByKey(TEXT("SpawnRate"))] == TEXT("float"));
	TestTrue(TEXT("and reads back as a local float: ") + ValueOf(*SpawnRate, TEXT("SpawnRate")), ValueOf(*SpawnRate, TEXT("SpawnRate")).StartsWith(TEXT("float ")));
	const int32 ColorIndex = Initialize->InputNames.IndexOfByKey(TEXT("Color"));
	if (!TestTrue(TEXT("Initialize Particle has a Color input"), ColorIndex != INDEX_NONE))
	{
		return false;
	}
	TestEqual(TEXT("which takes a color"), Initialize->InputTypes[ColorIndex], FString(TEXT("color")));
	int32 EnumIndex = INDEX_NONE;
	for (int32 Index = 0; Index < State->InputTypes.Num() && EnumIndex == INDEX_NONE; ++Index)
	{
		if (State->InputTypes[Index].StartsWith(TEXT("enum:")))
		{
			EnumIndex = Index;
		}
	}
	if (!TestTrue(TEXT("Emitter State has an enum input"), EnumIndex != INDEX_NONE))
	{
		return false;
	}
	const UEnum* Enum = FindFirstObject<UEnum>(*State->InputTypes[EnumIndex].Mid(5), EFindFirstObjectOptions::NativeFirst);
	if (!TestNotNull(TEXT("its UEnum is found by name"), Enum) || !TestTrue(TEXT("with two entries at least"), Enum->NumEnums() > 2))
	{
		return false;
	}
	const FString EnumInput = State->InputNames[EnumIndex];
	const FString Wanted = Enum->GetDisplayNameTextByIndex(1).ToString();

	// The Color From Curve dynamic input's own inputs, read from the asset: a colour curve and the time it samples.
	TArray<FHyperAINiagaraInputSchema> CurveInputs;
	TArray<FString> CurveOutputs;
	FString SchemaError;
	const UNiagaraScript* CurveScript = LoadObject<UNiagaraScript>(nullptr, ColorFromCurve);
	TestTrue(TEXT("Color From Curve's schema reads: ") + SchemaError, CurveScript && Gate::ReadScriptSchema(*CurveScript, CurveInputs, CurveOutputs, SchemaError));
	const FHyperAINiagaraInputSchema* CurveInput = CurveInputs.FindByPredicate([](const FHyperAINiagaraInputSchema& Each) { return Each.ValueType.StartsWith(TEXT("data_interface:")); });
	const FHyperAINiagaraInputSchema* TimeInput = CurveInputs.FindByPredicate([](const FHyperAINiagaraInputSchema& Each) { return Each.ValueType == TEXT("float"); });
	if (!TestNotNull(TEXT("with a curve data interface input"), CurveInput) || !TestNotNull(TEXT("and a float input"), TimeInput))
	{
		return false;
	}

	const TArray<FHyperAINiagaraEditOp> Plan = {
		Input(Emitter, TEXT("EmitterUpdateScript"), SpawnRate->ModuleName, {TEXT("SpawnRate")}, TEXT("dynamic_input"), FString(), RandomRangeFloat),
		Input(Emitter, TEXT("EmitterUpdateScript"), SpawnRate->ModuleName, {TEXT("SpawnRate"), TEXT("Minimum")}, TEXT("float"), TEXT("20")),
		Input(Emitter, TEXT("EmitterUpdateScript"), SpawnRate->ModuleName, {TEXT("SpawnRate"), TEXT("Maximum")}, TEXT("float"), TEXT("40")),
		Input(Emitter, TEXT("ParticleSpawnScript"), Initialize->ModuleName, {TEXT("Color")}, TEXT("dynamic_input"), FString(), ColorFromCurve),
		Input(Emitter, TEXT("ParticleSpawnScript"), Initialize->ModuleName, {TEXT("Color"), CurveInput->Name}, TEXT("color_curve"), TEXT("0:1,0.5,0,1; 1:0.1,0,0,0")),
		Input(Emitter, TEXT("ParticleSpawnScript"), Initialize->ModuleName, {TEXT("Color"), TimeInput->Name}, TEXT("linked"), TEXT("Emitter.NormalizedLoopAge")),
		Input(Emitter, TEXT("EmitterUpdateScript"), State->ModuleName, {EnumInput}, TEXT("enum"), Wanted)};
	FString Status;
	FString Diagnostic;
	for (const FHyperAINiagaraEditOp& Each : Plan)
	{
		TestTrue(FString::Printf(TEXT("%s %s passes its shape check (%s)"), *Each.ValueType, *FString::Join(Each.InputNameStack, TEXT(".")), *Diagnostic),
			Gate::ValidateOp(Each, true, Status, Diagnostic));
	}
	int32 Applied = 0;
	TArray<FString> PerOp;
	FString Error;
	const bool bApplied = Gate::ApplyOps(*System, Plan, Applied, PerOp, Error);
	TestTrue(FString::Printf(TEXT("the rich values apply (%s)"), *Error), bApplied);
	AddInfo(TEXT("Per op: ") + FString::Join(PerOp, TEXT(", ")));
	if (!bApplied)
	{
		return false;
	}

	TestTrue(TEXT("topology reads again"), ReadTopology(*System, Topology));
	SpawnRate = FindModule(Topology, TEXT("EmitterUpdateScript"), TEXT("SpawnRate"));
	Initialize = FindModule(Topology, TEXT("ParticleSpawnScript"), TEXT("InitializeParticle"));
	State = FindModule(Topology, TEXT("EmitterUpdateScript"), TEXT("EmitterState"));
	const FString Rate = SpawnRate ? ValueOf(*SpawnRate, TEXT("SpawnRate")) : FString();
	TestTrue(TEXT("the spawn rate reads back as the random range with both bounds: ") + Rate,
		Rate.StartsWith(TEXT("dynamic_input ")) && Rate.Contains(TEXT("RandomRangeFloat")) && Rate.Contains(TEXT("Minimum = float 20")) && Rate.Contains(TEXT("Maximum = float 40")));
	const FString Color = Initialize ? ValueOf(*Initialize, TEXT("Color")) : FString();
	TestTrue(TEXT("the colour reads back as the curve with its keys and its link: ") + Color,
		Color.Contains(TEXT("ColorFromCurve")) && Color.Contains(TEXT("RedCurve 0:1, 1:0.1")) && Color.Contains(TEXT("AlphaCurve 0:1, 1:0"))
		&& Color.Contains(TEXT("linked Emitter.NormalizedLoopAge")));
	const FString Mode = State ? ValueOf(*State, EnumInput) : FString();
	TestTrue(FString::Printf(TEXT("%s reads back as %s: %s"), *EnumInput, *Wanted, *Mode), Mode.StartsWith(TEXT("enum ")) && Mode.Contains(Enum->GetNameByIndex(1).ToString()));

	System->RequestCompile(/*bForce=*/false);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
	Gate::FDiagnostics Diagnostics;
	FString DiagnosticsError;
	Gate::ReadDiagnostics(*System, 64, Diagnostics, DiagnosticsError);
	TestFalse(TEXT("the System compiles"), Diagnostics.bCompileHasErrors);

	// Errors name what would work.
	FHyperAINiagaraEditOp BadEnum = Plan.Last();
	BadEnum.Value = TEXT("Sideways");
	TestFalse(TEXT("an unknown enum entry fails"), Gate::ApplyOps(*System, {BadEnum}, Applied, PerOp, Error));
	TestTrue(TEXT("listing the entries: ") + Error, Error.Contains(Enum->GetNameStringByIndex(0)));
	FHyperAINiagaraEditOp CurveOnFloat = Input(Emitter, TEXT("EmitterUpdateScript"), SpawnRate ? SpawnRate->ModuleName : FString(), {TEXT("SpawnRate"), TEXT("Minimum")}, TEXT("curve"), TEXT("0:1; 1:0"));
	TestFalse(TEXT("a curve on a float input fails"), Gate::ApplyOps(*System, {CurveOnFloat}, Applied, PerOp, Error));
	TestTrue(TEXT("pointing at the curve dynamic inputs: ") + Error, Error.Contains(TEXT("From Curve")));

	if (GEditor)
	{
		// The value plan was one transaction (the two failures above applied nothing): undo it in one step.
		TestTrue(TEXT("one undo step"), GEditor->UndoTransaction());
		TestTrue(TEXT("topology reads after undo"), ReadTopology(*System, Topology));
		SpawnRate = FindModule(Topology, TEXT("EmitterUpdateScript"), TEXT("SpawnRate"));
		TestTrue(TEXT("undo puts the spawn rate back to a local float"), SpawnRate && ValueOf(*SpawnRate, TEXT("SpawnRate")).StartsWith(TEXT("float ")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioNiagaraCreateRemoveTest, "HyperAIStudio.Niagara.Authoring.CreateRemoveBindAndParameters",
	HyperAIStudio::Niagara::AuthoringTests::Flags)

bool FHyperAIStudioNiagaraCreateRemoveTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Niagara::AuthoringTests;
	namespace Gate = HyperAIStudio::Niagara::ExternalEditGate;
	namespace GraphGate = HyperAIStudio::Niagara::GraphGate;
	UNiagaraSystem* Template = LoadObject<UNiagaraSystem>(nullptr, ExplosionPath);
	if (!Template || Template->GetEmitterHandles().Num() < 2)
	{
		AddInfo(TEXT("Skipped: Niagara's SimpleExplosion template is not available."));
		return true;
	}
	const FString Keep = Template->GetEmitterHandles()[0].GetName().ToString();
	const FString Path = NewPath(TEXT("NS_HyperAICreate_"));
	FHyperAINiagaraEditOp Create = Op(TEXT("create_system"));
	Create.AssetPath = ExplosionPath;
	Create.Value = Keep;

	FHyperAINiagaraEditOp AddSparks = Op(TEXT("add_emitter"));
	AddSparks.AssetPath = FountainPath;
	AddSparks.Name = TEXT("Sparks");
	FHyperAINiagaraEditOp DropRenderer = Op(TEXT("remove_renderer"), TEXT("Sparks"));
	DropRenderer.RendererIndex = 0;
	FHyperAINiagaraEditOp AddSprite = Op(TEXT("add_renderer"), TEXT("Sparks"));
	AddSprite.AssetPath = TEXT("/Script/Niagara.NiagaraSpriteRendererProperties");
	FHyperAINiagaraEditOp Bind = Op(TEXT("set_renderer_binding"), TEXT("Sparks"));
	Bind.RendererIndex = 0;
	Bind.Name = TEXT("DynamicMaterialBinding");
	Bind.Value = TEXT("Particles.DynamicMaterialParameter1");
	FHyperAINiagaraEditOp Tint = Op(TEXT("add_user_parameter"));
	Tint.Name = TEXT("Tint");
	Tint.ValueType = TEXT("color");
	Tint.Value = TEXT("1,0.4,0.1,1");
	FHyperAINiagaraEditOp Scale = Op(TEXT("add_user_parameter"));
	Scale.Name = TEXT("User.Scale");
	Scale.ValueType = TEXT("float");
	Scale.Value = TEXT("2");
	FHyperAINiagaraEditOp DropScale = Op(TEXT("remove_user_parameter"));
	DropScale.Name = TEXT("User.Scale");
	DropScale.ValueType = TEXT("float");
	FHyperAINiagaraEditOp Wobble = Op(TEXT("add_set_parameters"), TEXT("Sparks"), TEXT("ParticleSpawnScript"));
	Wobble.Name = TEXT("Particles.Wobble");
	Wobble.ValueType = TEXT("float");
	Wobble.Value = TEXT("0.5");
	FHyperAINiagaraEditOp DropDrag = Op(TEXT("remove_module"), TEXT("Sparks"), TEXT("ParticleUpdateScript"), TEXT("Drag"));
	const TArray<FHyperAINiagaraEditOp> Plan = {Create, AddSparks, DropRenderer, AddSprite, Bind, Tint, Scale, DropScale, Wobble, DropDrag};

	FString Status;
	FString Diagnostic;
	for (const FHyperAINiagaraEditOp& Each : Plan)
	{
		TestTrue(FString::Printf(TEXT("%s passes its shape check (%s)"), *Each.Kind, *Diagnostic), Gate::ValidateOp(Each, true, Status, Diagnostic));
	}
	TestTrue(TEXT("the plan walks from the template's kept emitter: ") + Diagnostic, GraphGate::ValidatePlanAgainstSystem(nullptr, Plan, Status, Diagnostic));
	FHyperAINiagaraEditOp BadBinding = Bind;
	BadBinding.Name = TEXT("GlowBinding");
	TestFalse(TEXT("an unknown binding is caught in the walk"), GraphGate::ValidatePlanAgainstSystem(nullptr, {Create, AddSparks, BadBinding}, Status, Diagnostic));
	TestTrue(TEXT("naming the bindings there are: ") + Diagnostic, Status == TEXT("renderer_binding_not_found") && Diagnostic.Contains(TEXT("ColorBinding")));
	FHyperAINiagaraEditOp TooFar = DropRenderer;
	TooFar.RendererIndex = 1;
	TestFalse(TEXT("removing past the last renderer is caught"), GraphGate::ValidatePlanAgainstSystem(nullptr, {Create, AddSparks, DropRenderer, TooFar}, Status, Diagnostic));
	FHyperAINiagaraEditOp Gone = Op(TEXT("remove_emitter"), TEXT("Sparks"));
	FHyperAINiagaraEditOp AfterGone = Bind;
	TestFalse(TEXT("an op on a removed emitter is caught"), GraphGate::ValidatePlanAgainstSystem(nullptr, {Create, AddSparks, Gone, AfterGone}, Status, Diagnostic));
	TestEqual(TEXT("as emitter_not_found"), Status, FString(TEXT("emitter_not_found")));
	TestFalse(TEXT("create_system later in a plan is refused"), GraphGate::ValidatePlanAgainstSystem(nullptr, {AddSparks, Create}, Status, Diagnostic));

	FString Error;
	UNiagaraSystem* System = Gate::CreateSystem(Path, Create, Error);
	if (!TestTrue(TEXT("the System is made from the template: ") + Error, System && Error.IsEmpty()))
	{
		return false;
	}
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };
	TestEqual(TEXT("at the target path"), System->GetPathName(), Path);
	TestEqual(TEXT("keeping only the chosen emitter"), System->GetEmitterHandles().Num(), 1);
	TestEqual(TEXT("by name"), System->GetEmitterHandles()[0].GetName().ToString(), Keep);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);

	int32 Applied = 0;
	TArray<FString> PerOp;
	const bool bApplied = Gate::ApplyOps(*System, Plan, Applied, PerOp, Error);
	TestTrue(FString::Printf(TEXT("the plan applies (%s)"), *Error), bApplied);
	AddInfo(TEXT("Per op: ") + FString::Join(PerOp, TEXT(", ")));
	if (!bApplied)
	{
		return false;
	}
	const FNiagaraEmitterHandle* Sparks = System->GetEmitterHandles().FindByPredicate([](const FNiagaraEmitterHandle& Handle) { return Handle.GetName() == FName(TEXT("Sparks")); });
	const FVersionedNiagaraEmitterData* Data = Sparks ? Sparks->GetEmitterData() : nullptr;
	if (!TestNotNull(TEXT("Sparks was added"), Data))
	{
		return false;
	}
	TestEqual(TEXT("with one renderer, the new sprite"), Data->GetRenderers().Num(), 1);
	const UNiagaraSpriteRendererProperties* Sprite = Data->GetRenderers().Num() == 1 ? Cast<UNiagaraSpriteRendererProperties>(Data->GetRenderers()[0]) : nullptr;
	TestTrue(TEXT("whose dynamic material binding reads DynamicMaterialParameter1"),
		Sprite && Sprite->DynamicMaterialBinding.GetParamMapBindableVariable().GetName() == FName(TEXT("Particles.DynamicMaterialParameter1")));
	TArray<FNiagaraVariable> UserParameters;
	System->GetExposedParameters().GetParameters(UserParameters);
	TestTrue(TEXT("User.Tint was added"), UserParameters.ContainsByPredicate([](const FNiagaraVariable& Each) { return Each.GetName() == FName(TEXT("User.Tint")); }));
	TestFalse(TEXT("and User.Scale added then removed"), UserParameters.ContainsByPredicate([](const FNiagaraVariable& Each) { return Each.GetName() == FName(TEXT("User.Scale")); }));
	TArray<FHyperAINiagaraEmitterTopology> Topology;
	TestTrue(TEXT("topology reads"), ReadTopology(*System, Topology, /*bValues=*/false));
	bool bSetParameters = false;
	bool bDrag = false;
	for (const FHyperAINiagaraEmitterTopology& Emitter : Topology)
	{
		for (const FHyperAINiagaraScriptStackTopology& Stack : Emitter.ScriptStacks)
		{
			for (const FHyperAINiagaraModuleTopology& Module : Stack.Modules)
			{
				bSetParameters |= Emitter.EmitterName == TEXT("Sparks") && Stack.ScriptName == TEXT("ParticleSpawnScript") && Module.bIsSetParametersModule
					&& Module.InputNames.ContainsByPredicate([](const FString& Name) { return Name.Contains(TEXT("Wobble")); });
				bDrag |= Emitter.EmitterName == TEXT("Sparks") && Module.ModuleName == TEXT("Drag");
			}
		}
	}
	TestTrue(TEXT("a Set Parameters module writes Particles.Wobble"), bSetParameters);
	TestFalse(TEXT("Drag is gone from Sparks"), bDrag);
	System->RequestCompile(/*bForce=*/false);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
	Gate::FDiagnostics Diagnostics;
	FString DiagnosticsError;
	Gate::ReadDiagnostics(*System, 64, Diagnostics, DiagnosticsError);
	TestFalse(TEXT("and the System compiles"), Diagnostics.bCompileHasErrors);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioNiagaraCreatePlanTest, "HyperAIStudio.Niagara.Authoring.CreatePlanEndToEnd",
	HyperAIStudio::Niagara::AuthoringTests::Flags)

bool FHyperAIStudioNiagaraCreatePlanTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Niagara::AuthoringTests;
	if (!LoadObject<UNiagaraEmitter>(nullptr, FountainPath))
	{
		AddInfo(TEXT("Skipped: Niagara's Fountain template is not available."));
		return true;
	}
	const FString Path = NewPath(TEXT("NS_HyperAIPlanned_"));
	FHyperAINiagaraEditOp Create = Op(TEXT("create_system"));
	FHyperAINiagaraEditOp Fountain = Op(TEXT("add_emitter"));
	Fountain.AssetPath = FountainPath;
	Fountain.Name = TEXT("Fountain");
	FHyperAINiagaraEditOp Rate = Input(TEXT("Fountain"), TEXT("EmitterUpdateScript"), TEXT("SpawnRate"), {TEXT("SpawnRate")}, TEXT("float"), TEXT("80"));

	FHyperAINiagaraApplyPlanRequest Plan;
	Plan.TargetPath = Path;
	Plan.Ops = {Create, Fountain, Rate};
	const FHyperAINiagaraApplyPlanReport DryRun = FHyperAIStudioNiagaraContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("a plan that makes its System dry-runs with no revision (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic),
		DryRun.bOk && DryRun.bTrustedPrepared);
	TestTrue(TEXT("on the target's absence"), FHyperAIStudioNiagaraContracts::IsCanonicalSha256(DryRun.BaseRevision));
	TestFalse(TEXT("and makes nothing"), FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)) || FSoftObjectPath(Path).ResolveObject() != nullptr);
	FHyperAINiagaraApplyPlanRequest Revisioned = Plan;
	Revisioned.ExpectedRevision = TEXT("sha256:") + FString::ChrN(64, TEXT('1'));
	TestEqual(TEXT("a stray revision is refused"), FHyperAIStudioNiagaraContracts::BuildPlan(Revisioned).Status, FString(TEXT("invalid_target_or_revision")));
	if (!DryRun.bOk)
	{
		return false;
	}

	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bPreviousApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT { Settings->bRequireApprovalForAgentEdits = bPreviousApproval; };
	Plan.bDryRun = false;
	Plan.OperationId = TEXT("niagara-create-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	Plan.ExpectedPlanHash = DryRun.PlanHash;
	const FHyperAINiagaraApplyPlanReport Submitted = FHyperAIStudioNiagaraContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("submission accepted (%s: %s)"), *Submitted.Status, *Submitted.Diagnostic), Submitted.bExecutionSubmitted);
	FHyperAIStudioTypedArtifactOperationStatus OperationStatus;
	FString StatusError;
	for (int32 Tick = 0; Tick < 600 && Submitted.bExecutionSubmitted; ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.01f);
		if (FHyperAIStudioTrustedExecutionFacade::QueryStatus(Plan.OperationId, OperationStatus, StatusError) && OperationStatus.bTerminal)
		{
			break;
		}
	}
	TestEqual(*FString::Printf(TEXT("the operation completed (%s)"), *OperationStatus.Diagnostic), OperationStatus.Status, FString(TEXT("completed")));
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(Path).ResolveObject());
	if (!TestNotNull(TEXT("the System exists"), System))
	{
		return false;
	}
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };
	TestTrue(TEXT("saved to disk"), FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)) && !System->GetOutermost()->IsDirty());
	TestEqual(TEXT("with the Fountain emitter"), System->GetEmitterHandles().Num(), 1);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
	TestTrue(TEXT("and it spawns"), LiveParticlesAfter(*System, 1.f) > 0);
	FHyperAINiagaraApplyPlanRequest Again = Plan;
	Again.bDryRun = true;
	Again.OperationId.Reset();
	Again.ExpectedPlanHash.Reset();
	TestEqual(TEXT("planning the same path again is refused"), FHyperAIStudioNiagaraContracts::BuildPlan(Again).Status, FString(TEXT("target_exists")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioNiagaraDiscoverTest, "HyperAIStudio.Niagara.Authoring.Discover",
	HyperAIStudio::Niagara::AuthoringTests::Flags)

bool FHyperAIStudioNiagaraDiscoverTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Niagara;
	auto Run = [](const TCHAR* Kind, const FString& Query = FString(), const FString& Asset = FString(), const FString& ValueType = FString())
	{
		FHyperAINiagaraDiscoverRequest Request;
		Request.Kind = Kind;
		Request.Query = Query;
		Request.AssetPath = Asset;
		Request.ValueType = ValueType;
		Request.MaxResults = 200;
		return Discover::Run(Request);
	};
	auto Has = [](const FHyperAINiagaraDiscoverReport& Report, const TCHAR* PathContains)
	{
		return Report.Entries.ContainsByPredicate([PathContains](const FHyperAINiagaraDiscoverEntry& Entry) { return Entry.Path.Contains(PathContains); });
	};
	const FHyperAINiagaraDiscoverReport Templates = Run(TEXT("templates"));
	TestTrue(TEXT("templates list: ") + Templates.Diagnostic, Templates.bOk && Has(Templates, TEXT("Emitters/Fountain.Fountain")) && Has(Templates, TEXT("Systems/SimpleExplosion")));
	TestFalse(TEXT("without lightweight emitters"), Has(Templates, TEXT("Lightweight")));
	TestTrue(TEXT("templates say what they are"), Templates.Entries.ContainsByPredicate([](const FHyperAINiagaraDiscoverEntry& Entry) { return Entry.Kind == TEXT("system_template"); }));
	const FHyperAINiagaraDiscoverReport Burst = Run(TEXT("templates"), TEXT("burst"));
	TestTrue(TEXT("a query narrows them"), Burst.Entries.Num() > 0 && Burst.Entries.Num() < Templates.Entries.Num() && !Has(Burst, TEXT("Fountain")));
	const FHyperAINiagaraDiscoverReport Modules = Run(TEXT("modules"), TEXT("spawn rate"));
	TestTrue(TEXT("modules list the library's Spawn Rate"), Has(Modules, TEXT("/Niagara/Modules/Emitter/SpawnRate")));
	const FHyperAINiagaraDiscoverReport Floats = Run(TEXT("dynamic_inputs"), FString(), FString(), TEXT("float"));
	TestTrue(TEXT("float dynamic inputs include a random range: ") + Floats.Diagnostic, Floats.bOk && Has(Floats, TEXT("RandomRangeFloat")));
	TestFalse(TEXT("an unknown type is refused"), Run(TEXT("dynamic_inputs"), FString(), FString(), TEXT("matrix3")).bOk);
	const FHyperAINiagaraDiscoverReport Schema = Run(TEXT("script_schema"), FString(), TEXT("/Niagara/Modules/Spawn/Initialization/V2/InitializeParticle.InitializeParticle"));
	TestTrue(TEXT("Initialize Particle's schema lists Color as a color: ") + Schema.Diagnostic, Schema.Inputs.ContainsByPredicate([](const FHyperAINiagaraInputSchema& Input)
	{
		return Input.Name == TEXT("Color") && Input.ValueType == TEXT("color");
	}));
	const FHyperAINiagaraDiscoverReport Renderers = Run(TEXT("renderers"));
	TestTrue(TEXT("renderers include sprites and ribbons"), Has(Renderers, TEXT("NiagaraSpriteRendererProperties")) && Has(Renderers, TEXT("NiagaraRibbonRendererProperties")));
	const FHyperAINiagaraDiscoverReport Sprite = Run(TEXT("renderer_schema"), FString(), TEXT("SpriteRenderer"));
	TestTrue(TEXT("a short renderer name resolves: ") + Sprite.Diagnostic, Sprite.bOk);
	const FHyperAINiagaraPropertyInfo* ColorBinding = Sprite.Properties.FindByPredicate([](const FHyperAINiagaraPropertyInfo& Each) { return Each.Name == TEXT("ColorBinding"); });
	TestTrue(TEXT("ColorBinding is a binding that reads Particles.Color by default"), ColorBinding && ColorBinding->Type == TEXT("binding") && ColorBinding->Default == TEXT("Particles.Color"));
	TestTrue(TEXT("SubImageSize is a settable property"), Sprite.Properties.ContainsByPredicate([](const FHyperAINiagaraPropertyInfo& Each) { return Each.Name == TEXT("SubImageSize"); }));
	const FHyperAINiagaraDiscoverReport Curve = Run(TEXT("data_interface_schema"), FString(), TEXT("/Script/Niagara.NiagaraDataInterfaceColorCurve"));
	TestTrue(TEXT("the colour curve data interface lists its channels"), Curve.Properties.ContainsByPredicate([](const FHyperAINiagaraPropertyInfo& Each) { return Each.Name == TEXT("RedCurve"); }));
	TestFalse(TEXT("an unknown kind is refused"), Run(TEXT("everything")).bOk);
	const FHyperAINiagaraDiscoverReport Recipes = Run(TEXT("recipes"));
	TestEqual(TEXT("six effect recipes"), Recipes.Entries.Num(), 6);
	TestTrue(TEXT("with their settings"), Recipes.Properties.ContainsByPredicate([](const FHyperAINiagaraPropertyInfo& Each) { return Each.Name == TEXT("fire.color"); }));
	FHyperAINiagaraDiscoverRequest Fire;
	Fire.Kind = TEXT("recipe");
	Fire.Recipe = TEXT("fire");
	Fire.TargetPath = TEXT("/Game/VFX/NS_Campfire.NS_Campfire");
	const FHyperAINiagaraDiscoverReport FireOps = Discover::Run(Fire);
	TestTrue(TEXT("a recipe compiles into ops starting with create_system: ") + FireOps.Diagnostic,
		FireOps.bOk && FireOps.Ops.Num() > 3 && FireOps.Ops[0].Kind == TEXT("create_system"));
	Fire.RecipeSettings = {TEXT("heat=9")};
	TestEqual(TEXT("an unknown setting is refused"), Discover::Run(Fire).Status, FString(TEXT("invalid_recipe")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioNiagaraPreviewTest, "HyperAIStudio.Niagara.Authoring.PreviewContactSheet",
	HyperAIStudio::Niagara::AuthoringTests::Flags)

bool FHyperAIStudioNiagaraPreviewTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Niagara::AuthoringTests;
	TArray<FColor> Plate;
	Plate.Init(FColor(40, 40, 40, 255), 16);
	TArray<FColor> Pixels = Plate;
	Pixels[5] = FColor(250, 120, 20, 255);
	Pixels[6] = FColor(240, 110, 10, 255);
	FHyperAINiagaraPreviewFrame Measured;
	HyperAIStudio::Niagara::Preview::Measure(Pixels, Plate, 4, 4, Measured);
	TestEqual(TEXT("two of sixteen pixels differ from the plate"), Measured.Coverage, 2.f / 16.f);
	TestTrue(TEXT("their mean colour is orange"), Measured.MeanColor.R > 0.9f && Measured.MeanColor.G < 0.5f && Measured.MeanColor.B < 0.1f);
	TestEqual(TEXT("in the second row"), Measured.ScreenBox.Y, 0.25);

	FHyperAINiagaraPreviewRequest Bad;
	Bad.TargetPath = TEXT("/Game/__HyperAIStudioTests/NS_None.NS_None");
	Bad.Ages = {1.f, 0.5f};
	TestEqual(TEXT("ages must ascend"), HyperAIStudio::Niagara::Preview::Render(Bad).Status, FString(TEXT("invalid_request")));
	Bad.Ages = {0.5f};
	TestEqual(TEXT("a missing System is named"), HyperAIStudio::Niagara::Preview::Render(Bad).Status, FString(TEXT("target_not_found")));

	UNiagaraSystem* System = MakeFountain(TEXT("NS_HyperAIPreview_"));
	if (!System)
	{
		AddInfo(TEXT("Skipped: Niagara's Fountain template is not available."));
		return true;
	}
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };
	FHyperAINiagaraPreviewRequest Request;
	Request.TargetPath = System->GetPathName();
	Request.Ages = {0.f, 0.5f, 1.5f};
	Request.TileSize = 192;
	const FHyperAINiagaraPreviewReport Report = HyperAIStudio::Niagara::Preview::Render(Request);
	TestTrue(TEXT("the preview renders: ") + Report.Diagnostic, Report.bOk);
	if (!Report.bOk)
	{
		return false;
	}
	TestTrue(TEXT("into a PNG on disk under Saved"), IFileManager::Get().FileExists(*Report.ImagePath) && Report.ImagePath.Contains(TEXT("HyperAIStudio/Captures/Niagara")));
	TestEqual(TEXT("one tile per age, side by side"), Report.Width, 3 * 192);
	TestEqual(TEXT("one tile high"), Report.Height, 192);
	if (TestEqual(TEXT("a frame per age"), Report.Frames.Num(), 3))
	{
		TestTrue(TEXT("the Fountain has particles by 1.5 s"), Report.Frames[2].Particles > 0);
		TestTrue(FString::Printf(TEXT("and they show (%.3f covered at 1.5 s, %.3f at 0 s)"), Report.Frames[2].Coverage, Report.Frames[0].Coverage),
			Report.Frames[2].Coverage > 0.001f && Report.Frames[2].Coverage > Report.Frames[0].Coverage);
	}
	TestTrue(TEXT("the emitter is reported"), Report.Emitters.Num() == 1 && Report.Emitters[0].NumParticles > 0);
	FHyperAINiagaraPreviewRequest Grey = Request;
	Grey.Background = TEXT("grey");
	Grey.View = TEXT("three_quarter");
	const FHyperAINiagaraPreviewReport GreyReport = HyperAIStudio::Niagara::Preview::Render(Grey);
	TestTrue(TEXT("a grey backdrop and another view render too: ") + GreyReport.Diagnostic, GreyReport.bOk && GreyReport.Frames.Num() == 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioNiagaraRecipesTest, "HyperAIStudio.Niagara.Authoring.EffectRecipesBuildAndRender",
	HyperAIStudio::Niagara::AuthoringTests::Flags)

bool FHyperAIStudioNiagaraRecipesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Niagara::AuthoringTests;
	namespace Gate = HyperAIStudio::Niagara::ExternalEditGate;
	namespace Recipes = HyperAIStudio::Niagara::Recipes;
	if (!LoadObject<UNiagaraEmitter>(nullptr, FountainPath))
	{
		AddInfo(TEXT("Skipped: Niagara's templates are not available."));
		return true;
	}
	TestEqual(TEXT("six recipes"), Recipes::List().Num(), 6);
	TArray<FHyperAINiagaraEditOp> Ops;
	TArray<FString> Materials;
	TArray<FString> Notes;
	FString Error;
	TestFalse(TEXT("an unknown recipe is refused"), Recipes::Compile(TEXT("tornado"), {}, NewPath(TEXT("NS_X_")), Ops, Materials, Notes, Error));
	TestTrue(TEXT("naming the recipes there are"), Error.Contains(TEXT("explosion")));
	TestFalse(TEXT("a setting out of range is refused"), Recipes::Compile(TEXT("fire"), {TEXT("scale=500")}, NewPath(TEXT("NS_X_")), Ops, Materials, Notes, Error));
	TestFalse(TEXT("a malformed colour is refused"), Recipes::Compile(TEXT("fire"), {TEXT("color=red")}, NewPath(TEXT("NS_X_")), Ops, Materials, Notes, Error));

	for (const Recipes::FRecipeInfo& Info : Recipes::List())
	{
		const FString Path = NewPath(*(TEXT("NS_HyperAIRecipe_") + Info.Id + TEXT("_")));
		const bool bCompiled = Recipes::Compile(Info.Id, {TEXT("scale=1")}, Path, Ops, Materials, Notes, Error);
		TestTrue(FString::Printf(TEXT("%s compiles: %s"), *Info.Id, *Error), bCompiled && Ops.Num() > 1 && Ops[0].Kind == TEXT("create_system"));
		if (!bCompiled)
		{
			continue;
		}
		FString Status;
		FString Diagnostic;
		bool bShaped = true;
		for (const FHyperAINiagaraEditOp& Each : Ops)
		{
			bShaped &= Gate::ValidateOp(Each, true, Status, Diagnostic);
			if (!bShaped)
			{
				AddError(FString::Printf(TEXT("%s: %s op fails its shape check: %s"), *Info.Id, *Each.Kind, *Diagnostic));
				break;
			}
		}
		TestTrue(FString::Printf(TEXT("%s walks clean against its own templates: %s"), *Info.Id, *Diagnostic),
			bShaped && HyperAIStudio::Niagara::GraphGate::ValidatePlanAgainstSystem(nullptr, Ops, Status, Diagnostic));
		UNiagaraSystem* System = Gate::CreateSystem(Path, Ops[0], Error);
		if (!TestNotNull(FString::Printf(TEXT("%s's System is made: %s"), *Info.Id, *Error), System))
		{
			continue;
		}
		ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };
		int32 Applied = 0;
		TArray<FString> PerOp;
		bool bApplied = Gate::ApplyOps(*System, Ops, Applied, PerOp, Error);
		TestTrue(FString::Printf(TEXT("%s's %d ops apply (%s)"), *Info.Id, Ops.Num(), *Error), bApplied);
		if (bApplied && Info.Settings.ContainsByPredicate([](const Recipes::FRecipeSetting& Setting) { return Setting.Key == TEXT("part"); }))
		{
			// A recipe in two parts: the second adds to the System the first made, with no create_system of its own.
			TArray<FHyperAINiagaraEditOp> Second;
			TestTrue(FString::Printf(TEXT("%s part 2 compiles: %s"), *Info.Id, *Error),
				Recipes::Compile(Info.Id, {TEXT("part=2")}, Path, Second, Materials, Notes, Error) && Second.Num() > 1 && Second[0].Kind != TEXT("create_system"));
			bApplied = Gate::ApplyOps(*System, Second, Applied, PerOp, Error);
			TestTrue(FString::Printf(TEXT("%s part 2's %d ops apply (%s)"), *Info.Id, Second.Num(), *Error), bApplied);
		}
		if (!bApplied)
		{
			continue;
		}
		System->RequestCompile(/*bForce=*/false);
		System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
		Gate::FDiagnostics Diagnostics;
		FString DiagnosticsError;
		Gate::ReadDiagnostics(*System, 64, Diagnostics, DiagnosticsError);
		FString Issues;
		for (const FHyperAINiagaraIssue& Issue : Diagnostics.Issues)
		{
			if (Issue.Severity == TEXT("error")) Issues += FString::Printf(TEXT("[%s] %s | "), *Issue.SourcePath, *Issue.Summary);
		}
		TestFalse(FString::Printf(TEXT("%s compiles without errors: %s"), *Info.Id, *Issues), Diagnostics.bCompileHasErrors);
		TestEqual(FString::Printf(TEXT("%s has no stack errors: %s"), *Info.Id, *Issues), Diagnostics.StackErrorCount, 0);

		FHyperAINiagaraPreviewRequest Preview;
		Preview.TargetPath = Path;
		Preview.Ages = {0.1f, 0.35f, 0.8f, 1.6f};
		Preview.TileSize = 256;
		Preview.Background = Info.Id == TEXT("smoke_plume") ? TEXT("grey") : TEXT("black");
		const FHyperAINiagaraPreviewReport Report = HyperAIStudio::Niagara::Preview::Render(Preview);
		TestTrue(FString::Printf(TEXT("%s previews: %s"), *Info.Id, *Report.Diagnostic), Report.bOk);
		float MostCovered = 0.f;
		for (const FHyperAINiagaraPreviewFrame& Frame : Report.Frames)
		{
			MostCovered = FMath::Max(MostCovered, Frame.Coverage);
		}
		TestTrue(FString::Printf(TEXT("%s shows on screen (%.3f of a tile at most)"), *Info.Id, MostCovered), MostCovered > 0.005f);
		AddInfo(FString::Printf(TEXT("%s preview: %s (%s)"), *Info.Id, *Report.ImagePath, *FString::Join(Report.Notes, TEXT(" "))));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
