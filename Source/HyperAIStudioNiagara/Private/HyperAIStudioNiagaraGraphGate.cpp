// Games by Hyper 2026.

#include "HyperAIStudioNiagaraGraphGate.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "HyperAIStudioNames.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/OutputDeviceNull.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

static_assert(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8,
	"The Niagara graph gate mirrors UE 5.8's editor paths for simulation stages, scratch pad scripts and Custom HLSL "
	"nodes (NiagaraStackEmitterPropertiesGroup.cpp, NiagaraScratchPadViewModel.cpp, NiagaraScriptFactory.cpp). "
	"Re-audit them before raising this pin.");

namespace HyperAIStudio::Niagara::GraphGate
{
	namespace
	{
		constexpr int32 MaxIdentifierChars = 64;
		constexpr int32 MaxModulesPerStack = 256;
		constexpr int32 MaxSuggestions = 8;

		bool Fail(FString& OutStatus, FString& OutDiagnostic, const TCHAR* Status, const FString& Diagnostic)
		{
			OutStatus = Status;
			OutDiagnostic = Diagnostic;
			return false;
		}

		struct FScriptRow
		{
			const TCHAR* Name;
			ENiagaraScriptUsage Usage;
		};

		const FScriptRow ScriptRows[] = {
			{TEXT("SystemSpawnScript"), ENiagaraScriptUsage::SystemSpawnScript},
			{TEXT("SystemUpdateScript"), ENiagaraScriptUsage::SystemUpdateScript},
			{TEXT("EmitterSpawnScript"), ENiagaraScriptUsage::EmitterSpawnScript},
			{TEXT("EmitterUpdateScript"), ENiagaraScriptUsage::EmitterUpdateScript},
			{TEXT("ParticleSpawnScript"), ENiagaraScriptUsage::ParticleSpawnScript},
			{TEXT("ParticleUpdateScript"), ENiagaraScriptUsage::ParticleUpdateScript},
			{TEXT("ParticleSimulationStageScript"), ENiagaraScriptUsage::ParticleSimulationStageScript}};

		/** The stack an op addresses. A stage_name with no script_name means that stage's stack. */
		bool ParseStack(const FHyperAINiagaraEditOp& Op, ENiagaraScriptUsage& OutUsage)
		{
			if (Op.ScriptName.IsEmpty() && !Op.StageName.IsEmpty())
			{
				OutUsage = ENiagaraScriptUsage::ParticleSimulationStageScript;
				return true;
			}
			for (const FScriptRow& Row : ScriptRows)
			{
				if (Op.ScriptName == Row.Name)
				{
					OutUsage = Row.Usage;
					return true;
				}
			}
			return false;
		}

		bool IsSystemUsage(const ENiagaraScriptUsage Usage)
		{
			return Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript;
		}

		bool IsStageUsage(const ENiagaraScriptUsage Usage)
		{
			return Usage == ENiagaraScriptUsage::ParticleSimulationStageScript;
		}

		bool IsIdentifier(const FString& Value)
		{
			if (Value.IsEmpty() || Value.Len() > MaxIdentifierChars || !(FChar::IsAlpha(Value[0]) || Value[0] == TEXT('_')))
			{
				return false;
			}
			for (const TCHAR Char : Value)
			{
				if (!FChar::IsAlnum(Char) && Char != TEXT('_'))
				{
					return false;
				}
			}
			return true;
		}

		bool ParseBool(const FString& Value, bool& Out)
		{
			const FString Trimmed = Value.TrimStartAndEnd();
			if (Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase)) { Out = true; return true; }
			if (Trimmed.Equals(TEXT("false"), ESearchCase::IgnoreCase)) { Out = false; return true; }
			return false;
		}

		bool ParseInt(const FString& Value, int32& Out)
		{
			const FString Trimmed = Value.TrimStartAndEnd();
			return !Trimmed.IsEmpty() && Trimmed.IsNumeric() && !Trimmed.Contains(TEXT(".")) && LexTryParseString(Out, *Trimmed);
		}

		bool ParseDoubles(const FString& Value, const int32 Count, TArray<double>& Out)
		{
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(","), false);
			if (Parts.Num() != Count)
			{
				return false;
			}
			Out.Reset();
			for (const FString& Part : Parts)
			{
				double Parsed = 0.0;
				const FString Trimmed = Part.TrimStartAndEnd();
				if (Trimmed.IsEmpty() || !Trimmed.IsNumeric() || !LexTryParseString(Parsed, *Trimmed) || !FMath::IsFinite(Parsed))
				{
					return false;
				}
				Out.Add(Parsed);
			}
			return true;
		}

		bool ParseSimTarget(const FString& Value, ENiagaraSimTarget& Out)
		{
			const FString Trimmed = Value.TrimStartAndEnd();
			if (Trimmed.Equals(TEXT("gpu"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("GPUComputeSim"), ESearchCase::IgnoreCase))
			{
				Out = ENiagaraSimTarget::GPUComputeSim;
				return true;
			}
			if (Trimmed.Equals(TEXT("cpu"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("CPUSim"), ESearchCase::IgnoreCase))
			{
				Out = ENiagaraSimTarget::CPUSim;
				return true;
			}
			return false;
		}

		/**
		 * Unbalanced brackets are what a truncated or half-pasted body looks like; the compiler reports everything
		 * else through hyper_niagara_validate once the System compiles.
		 */
		bool CheckHlsl(const FString& Hlsl, FString& OutError)
		{
			if (Hlsl.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("hlsl is empty; give the module body, e.g. Particles.Velocity += float3(0, 0, 100) * Engine.DeltaTime;");
				return false;
			}
			if (Hlsl.Len() > MaxHlslChars)
			{
				OutError = FString::Printf(TEXT("hlsl is %d characters; the limit is %d."), Hlsl.Len(), MaxHlslChars);
				return false;
			}
			TArray<TCHAR> Open;
			bool bLineComment = false;
			bool bBlockComment = false;
			bool bString = false;
			for (int32 Index = 0; Index < Hlsl.Len(); ++Index)
			{
				const TCHAR Char = Hlsl[Index];
				const TCHAR Next = Index + 1 < Hlsl.Len() ? Hlsl[Index + 1] : TEXT('\0');
				if ((Char < TEXT(' ') && Char != TEXT('\t') && Char != TEXT('\n') && Char != TEXT('\r')) || Char == 0x7F)
				{
					OutError = FString::Printf(TEXT("hlsl has a control character at offset %d."), Index);
					return false;
				}
				if (bLineComment) { bLineComment = Char != TEXT('\n'); continue; }
				if (bBlockComment) { if (Char == TEXT('*') && Next == TEXT('/')) { bBlockComment = false; ++Index; } continue; }
				if (bString) { if (Char == TEXT('\\')) { ++Index; } else if (Char == TEXT('"')) { bString = false; } continue; }
				if (Char == TEXT('/') && Next == TEXT('/')) { bLineComment = true; ++Index; continue; }
				if (Char == TEXT('/') && Next == TEXT('*')) { bBlockComment = true; ++Index; continue; }
				if (Char == TEXT('"')) { bString = true; continue; }
				if (Char == TEXT('(') || Char == TEXT('[') || Char == TEXT('{'))
				{
					Open.Add(Char);
				}
				else if (Char == TEXT(')') || Char == TEXT(']') || Char == TEXT('}'))
				{
					const TCHAR Expected = Char == TEXT(')') ? TEXT('(') : Char == TEXT(']') ? TEXT('[') : TEXT('{');
					if (Open.IsEmpty() || Open.Last() != Expected)
					{
						OutError = FString::Printf(TEXT("hlsl has an unmatched '%c' at offset %d."), Char, Index);
						return false;
					}
					Open.Pop();
				}
			}
			if (!Open.IsEmpty() || bBlockComment || bString)
			{
				OutError = TEXT("hlsl ends inside an open bracket, comment or string; is it truncated?");
				return false;
			}
			return true;
		}

		FNiagaraEmitterHandle* FindHandle(UNiagaraSystem& System, const FString& Name)
		{
			const FName Wanted(*Name);
			for (FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
			{
				if (Handle.GetName() == Wanted)
				{
					return &Handle;
				}
			}
			return nullptr;
		}

		UNiagaraGraph* GraphOf(UNiagaraScriptSourceBase* Source)
		{
			const UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(Source);
			return ScriptSource ? ScriptSource->NodeGraph.Get() : nullptr;
		}

		UNiagaraSimulationStageBase* FindStage(const FVersionedNiagaraEmitterData& Data, const FString& Name)
		{
			const FName Wanted(*Name);
			for (UNiagaraSimulationStageBase* Stage : Data.GetSimulationStages())
			{
				if (Stage && Stage->SimulationStageName == Wanted)
				{
					return Stage;
				}
			}
			return nullptr;
		}

		FString IterationSourceName(const ENiagaraIterationSource Source)
		{
			switch (Source)
			{
			case ENiagaraIterationSource::Particles: return TEXT("particles");
			case ENiagaraIterationSource::DataInterface: return TEXT("data_interface");
			case ENiagaraIterationSource::DirectSet: return TEXT("direct_set");
			default: return TEXT("other");
			}
		}

		bool IsParameterMapPin(const UEdGraphPin* Pin)
		{
			return Pin && UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef();
		}

		UEdGraphPin* ParameterMapPin(UEdGraphNode& Node, const EEdGraphPinDirection Direction)
		{
			for (UEdGraphPin* Pin : Node.Pins)
			{
				if (Pin && Pin->Direction == Direction && IsParameterMapPin(Pin))
				{
					return Pin;
				}
			}
			return nullptr;
		}

		/**
		 * The modules feeding a stack's output node, first to last. The chain alternates module calls with the
		 * parameter-map-set nodes that carry their input overrides; only the calls are modules.
		 */
		TArray<UNiagaraNodeFunctionCall*> ModulesOf(UNiagaraNodeOutput& Output)
		{
			TArray<UNiagaraNodeFunctionCall*> Modules;
			TSet<const UEdGraphNode*> Seen;
			UEdGraphPin* Pin = ParameterMapPin(Output, EGPD_Input);
			while (Pin && Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0] && Seen.Num() < 4 * MaxModulesPerStack)
			{
				UEdGraphNode* Node = Pin->LinkedTo[0]->GetOwningNode();
				if (!Node || Seen.Contains(Node) || Node->IsA<UNiagaraNodeInput>())
				{
					break;
				}
				Seen.Add(Node);
				if (UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node))
				{
					Modules.Insert(Call, 0);
				}
				Pin = ParameterMapPin(*Node, EGPD_Input);
			}
			return Modules;
		}

		struct FStack
		{
			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
			UNiagaraGraph* Graph = nullptr;
			UNiagaraNodeOutput* Output = nullptr;
		};

		/** The output node of the stack an op addresses: an emitter or System script, or a simulation stage. */
		bool ResolveStack(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op, FStack& Out, FString& OutError)
		{
			if (!ParseStack(Op, Out.Usage))
			{
				OutError = FString::Printf(TEXT("Unknown script_name %s."), *Op.ScriptName);
				return false;
			}
			FGuid UsageId;
			if (IsSystemUsage(Out.Usage))
			{
				UNiagaraScript* Script = System.GetSystemSpawnScript();
				Out.Graph = Script ? GraphOf(Script->GetLatestSource()) : nullptr;
			}
			else
			{
				FNiagaraEmitterHandle* Handle = FindHandle(System, Op.EmitterName);
				FVersionedNiagaraEmitterData* Data = Handle ? Handle->GetEmitterData() : nullptr;
				if (!Data)
				{
					OutError = FString::Printf(TEXT("No emitter named %s."), *Op.EmitterName);
					return false;
				}
				Out.Graph = GraphOf(Data->GraphSource);
				if (IsStageUsage(Out.Usage))
				{
					const UNiagaraSimulationStageBase* Stage = FindStage(*Data, Op.StageName);
					if (!Stage || !Stage->Script)
					{
						OutError = FString::Printf(TEXT("%s has no simulation stage named %s."), *Op.EmitterName, *Op.StageName);
						return false;
					}
					UsageId = Stage->Script->GetUsageId();
				}
			}
			Out.Output = Out.Graph ? Out.Graph->FindEquivalentOutputNode(Out.Usage, UsageId) : nullptr;
			if (!Out.Output)
			{
				OutError = TEXT("The addressed stack has no output node in its graph.");
				return false;
			}
			return true;
		}

		/** What FNiagaraStackGraphUtilities::ResetGraphForOutput builds (it is not exported): an input map wired to a new output. */
		UNiagaraNodeOutput* CreateStackOutput(UNiagaraGraph& Graph, const ENiagaraScriptUsage Usage, const FGuid& UsageId)
		{
			Graph.Modify();
			int32 Bottom = 0;
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				Bottom = Node ? FMath::Max(Bottom, Node->NodePosY) : Bottom;
			}
			FGraphNodeCreator<UNiagaraNodeOutput> OutputCreator(Graph);
			UNiagaraNodeOutput* Output = OutputCreator.CreateNode(false);
			Output->SetUsage(Usage);
			Output->SetUsageId(UsageId);
			Output->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
			Output->NodePosX = 600;
			Output->NodePosY = Bottom + 400;
			OutputCreator.Finalize();

			FGraphNodeCreator<UNiagaraNodeInput> InputCreator(Graph);
			UNiagaraNodeInput* Input = InputCreator.CreateNode(false);
			Input->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
			Input->Usage = ENiagaraInputNodeUsage::Parameter;
			Input->NodePosX = 0;
			Input->NodePosY = Bottom + 400;
			InputCreator.Finalize();

			UEdGraphPin* From = ParameterMapPin(*Input, EGPD_Output);
			UEdGraphPin* To = ParameterMapPin(*Output, EGPD_Input);
			return From && To && Graph.GetSchema()->TryCreateConnection(From, To) ? Output : nullptr;
		}

		/** Usages a scratch module declares it supports; particle spawn can compile as its interpolated variant. */
		int32 UsageBits(const ENiagaraScriptUsage Usage)
		{
			int32 Bits = 1 << static_cast<int32>(Usage);
			if (Usage == ENiagaraScriptUsage::ParticleSpawnScript)
			{
				Bits |= 1 << static_cast<int32>(ENiagaraScriptUsage::ParticleSpawnScriptInterpolated);
			}
			return Bits;
		}

		/**
		 * A scratch pad module owned by the System, as the scratch pad view model makes one, whose graph is
		 * InputMap -> Custom HLSL (Map in, MapOut out) -> Output. The HLSL reads and writes attributes by name.
		 */
		UNiagaraScript* CreateHlslModule(UNiagaraSystem& System, const FString& Name, const FString& Hlsl,
			const ENiagaraScriptUsage TargetUsage, FString& OutError)
		{
			// The HLSL is a private property with no exported setter; the Details panel writes it through reflection,
			// and so does this, for this one name. The audit test fails if it is renamed or retyped.
			FStrProperty* HlslProperty = FindFProperty<FStrProperty>(UNiagaraNodeCustomHlsl::StaticClass(), TEXT("CustomHlsl"));
			if (!HlslProperty)
			{
				OutError = TEXT("This engine build changed the Custom HLSL node; HLSL modules cannot be created here.");
				return nullptr;
			}
			if (!System.HasAnyFlags(RF_Transactional))
			{
				System.SetFlags(RF_Transactional);
			}
			System.Modify();
			// The agent's name when it is free, so the stack module is called what the agent asked for.
			const FName Wanted(*Name);
			const FName ScriptName = StaticFindObjectFast(nullptr, &System, Wanted)
				? MakeUniqueObjectName(&System, UNiagaraScript::StaticClass(), Wanted) : Wanted;
			UNiagaraScript* Script = NewObject<UNiagaraScript>(&System, ScriptName, RF_Transactional);
			Script->SetUsage(ENiagaraScriptUsage::Module);
			UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Script, NAME_None, RF_Transactional);
			UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(Source, NAME_None, RF_Transactional);
			Source->NodeGraph = Graph;

			const FNiagaraTypeDefinition& MapType = FNiagaraTypeDefinition::GetParameterMapDef();
			FGraphNodeCreator<UNiagaraNodeOutput> OutputCreator(*Graph);
			UNiagaraNodeOutput* Output = OutputCreator.CreateNode(false);
			Output->SetUsage(ENiagaraScriptUsage::Module);
			Output->Outputs.Add(FNiagaraVariable(MapType, TEXT("Output")));
			Output->NodePosX = 800;
			OutputCreator.Finalize();

			FGraphNodeCreator<UNiagaraNodeInput> InputCreator(*Graph);
			UNiagaraNodeInput* Input = InputCreator.CreateNode(false);
			Input->Input = FNiagaraVariable(MapType, TEXT("MapIn"));
			Input->Usage = ENiagaraInputNodeUsage::Parameter;
			InputCreator.Finalize();

			// Pins come from the signature: RequestNewTypedPin, the editor's way, is not exported.
			FGraphNodeCreator<UNiagaraNodeCustomHlsl> CodeCreator(*Graph);
			UNiagaraNodeCustomHlsl* Code = CodeCreator.CreateNode(false);
			Code->Signature.Inputs.Add(FNiagaraVariable(MapType, TEXT("Map")));
			Code->Signature.Outputs.Add(FNiagaraVariableBase(MapType, TEXT("MapOut")));
			Code->NodePosX = 300;
			CodeCreator.Finalize();
			HlslProperty->SetPropertyValue_InContainer(Code, Hlsl);
			FPropertyChangedEvent HlslChanged(HlslProperty, EPropertyChangeType::ValueSet);
			Code->PostEditChangeProperty(HlslChanged);

			const UEdGraphSchema* Schema = Graph->GetSchema();
			UEdGraphPin* MapIn = ParameterMapPin(*Input, EGPD_Output);
			UEdGraphPin* CodeIn = ParameterMapPin(*Code, EGPD_Input);
			UEdGraphPin* CodeOut = ParameterMapPin(*Code, EGPD_Output);
			UEdGraphPin* MapOut = ParameterMapPin(*Output, EGPD_Input);
			if (!Schema || !MapIn || !CodeIn || !CodeOut || !MapOut
				|| !Schema->TryCreateConnection(MapIn, CodeIn) || !Schema->TryCreateConnection(CodeOut, MapOut))
			{
				OutError = TEXT("The HLSL module graph could not be wired.");
				return nullptr;
			}
			Script->SetLatestSource(Source);
			if (FVersionedNiagaraScriptData* ScriptData = Script->GetLatestScriptData())
			{
				ScriptData->ModuleUsageBitmask |= UsageBits(TargetUsage);
			}
			Script->ClearFlags(RF_Public | RF_Standalone);
			System.ScratchPadScripts.Add(Script);
			return Script;
		}

		bool AddSimulationStage(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op, FString& OutStatus, FString& OutError)
		{
			FNiagaraEmitterHandle* Handle = FindHandle(System, Op.EmitterName);
			const FVersionedNiagaraEmitter Instance = Handle ? Handle->GetInstance() : FVersionedNiagaraEmitter();
			FVersionedNiagaraEmitterData* Data = Instance.Emitter ? Instance.GetEmitterData() : nullptr;
			UNiagaraScriptSource* Source = Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
			UNiagaraGraph* Graph = Source ? Source->NodeGraph.Get() : nullptr;
			if (!Graph)
			{
				OutError = FString::Printf(TEXT("No emitter named %s with a script graph."), *Op.EmitterName);
				return false;
			}
			if (FindStage(*Data, Op.Name))
			{
				OutError = FString::Printf(TEXT("%s already has a simulation stage named %s."), *Op.EmitterName, *Op.Name);
				return false;
			}
			int32 Iterations = 1;
			if (!Op.Value.IsEmpty() && !ParseInt(Op.Value, Iterations))
			{
				OutError = TEXT("The iteration count no longer parses.");
				return false;
			}

			// NiagaraStackEmitterPropertiesGroup.cpp's add-stage path, step for step, except that the stage and its script
			// become transactional only once built. An object created transactional inside a transaction is recorded as
			// just constructed, so undo would briefly give the emitter a stage with no script, which Niagara logs as an
			// error while it serializes the emitter. Built first, undo simply drops the emitter's reference to it.
			Instance.Emitter->Modify();
			UNiagaraSimulationStageGeneric* Stage = NewObject<UNiagaraSimulationStageGeneric>(Instance.Emitter);
			Stage->SimulationStageName = FName(*Op.Name);
			Stage->Script = NewObject<UNiagaraScript>(Stage, MakeUniqueObjectName(Stage, UNiagaraScript::StaticClass(), TEXT("SimulationStage")));
			Stage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
			Stage->Script->SetUsageId(Stage->GetMergeId());
			Stage->Script->SetLatestSource(Source);
			if (Iterations != 1 && Stage->NumIterations.GetDefaultValueArray().Num() == sizeof(int32))
			{
				Stage->NumIterations.SetDefaultValueEditorOnly<int32>(Iterations);
			}
			Stage->Script->SetFlags(RF_Transactional);
			Stage->SetFlags(RF_Transactional);
			Instance.Emitter->AddSimulationStage(Stage, Instance.Version);
			if (!CreateStackOutput(*Graph, ENiagaraScriptUsage::ParticleSimulationStageScript, Stage->Script->GetUsageId()))
			{
				OutError = TEXT("The stage was added but its stack could not be wired.");
				return false;
			}
			OutStatus = FString::Printf(TEXT("added_simulation_stage:%s"), *Op.Name);
			return true;
		}

		bool AddHlslModule(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op, FString& OutStatus, FString& OutError)
		{
			FStack Stack;
			if (!ResolveStack(System, Op, Stack, OutError))
			{
				return false;
			}
			UNiagaraScript* Script = CreateHlslModule(System, Op.Name, Op.Hlsl, Stack.Usage, OutError);
			UNiagaraNodeFunctionCall* Call = Script
				? FNiagaraStackGraphUtilities::AddScriptModuleToStack(Script, *Stack.Output, INDEX_NONE, Op.Name) : nullptr;
			if (!Call)
			{
				if (OutError.IsEmpty()) OutError = TEXT("The HLSL module could not be added to the stack.");
				return false;
			}
			OutStatus = TEXT("added_hlsl_module:") + Call->GetFunctionName();
			return true;
		}

		bool AddStageModule(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op, FString& OutStatus, FString& OutError)
		{
			FStack Stack;
			if (!ResolveStack(System, Op, Stack, OutError))
			{
				return false;
			}
			UNiagaraScript* Module = Cast<UNiagaraScript>(FSoftObjectPath(Op.AssetPath).ResolveObject());
			const FVersionedNiagaraScriptData* ModuleData = Module ? Module->GetLatestScriptData() : nullptr;
			UNiagaraNodeFunctionCall* Call = Module ? FNiagaraStackGraphUtilities::AddScriptModuleToStack(Module, *Stack.Output,
				INDEX_NONE, FString(), ModuleData ? ModuleData->Version.VersionGuid : FGuid()) : nullptr;
			if (!Call)
			{
				OutError = FString::Printf(TEXT("The module %s is not loaded or could not be added."), *Op.AssetPath);
				return false;
			}
			OutStatus = TEXT("added_module:") + Call->GetFunctionName();
			return true;
		}

		bool SetStageModuleEnabled(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op, FString& OutStatus, FString& OutError)
		{
			FStack Stack;
			if (!ResolveStack(System, Op, Stack, OutError))
			{
				return false;
			}
			for (UNiagaraNodeFunctionCall* Call : ModulesOf(*Stack.Output))
			{
				if (Call->GetFunctionName() == Op.ModuleName)
				{
					FNiagaraStackGraphUtilities::SetModuleIsEnabled(*Call, Op.bEnabled);
					OutStatus = Op.bEnabled ? TEXT("enabled") : TEXT("disabled");
					return true;
				}
			}
			OutError = FString::Printf(TEXT("Stage %s has no module named %s."), *Op.StageName, *Op.ModuleName);
			return false;
		}

		const TCHAR* EmitterPropertyHelp()
		{
			return TEXT("name is sim_target (cpu|gpu), local_space (true|false), determinism (true|false), random_seed (an integer), "
				"fixed_bounds (minx,miny,minz,maxx,maxy,maxz, or dynamic) or enabled (true|false).");
		}

		/** Parses an emitter property value; FixedBounds stays unset for "dynamic". */
		bool ParseEmitterProperty(const FHyperAINiagaraEditOp& Op, ENiagaraSimTarget& OutTarget, bool& OutFlag, int32& OutInt,
			TOptional<FBox>& OutBounds)
		{
			if (Op.Name == TEXT("sim_target")) return ParseSimTarget(Op.Value, OutTarget);
			if (Op.Name == TEXT("local_space") || Op.Name == TEXT("determinism") || Op.Name == TEXT("enabled")) return ParseBool(Op.Value, OutFlag);
			if (Op.Name == TEXT("random_seed")) return ParseInt(Op.Value, OutInt);
			if (Op.Name == TEXT("fixed_bounds"))
			{
				if (Op.Value.TrimStartAndEnd().Equals(TEXT("dynamic"), ESearchCase::IgnoreCase))
				{
					OutBounds.Reset();
					return true;
				}
				TArray<double> Corners;
				if (!ParseDoubles(Op.Value, 6, Corners) || Corners[0] > Corners[3] || Corners[1] > Corners[4] || Corners[2] > Corners[5])
				{
					return false;
				}
				OutBounds = FBox(FVector(Corners[0], Corners[1], Corners[2]), FVector(Corners[3], Corners[4], Corners[5]));
				return true;
			}
			return false;
		}

		bool SetEmitterProperty(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op, FString& OutStatus, FString& OutError)
		{
			FNiagaraEmitterHandle* Handle = FindHandle(System, Op.EmitterName);
			const FVersionedNiagaraEmitter Instance = Handle ? Handle->GetInstance() : FVersionedNiagaraEmitter();
			FVersionedNiagaraEmitterData* Data = Instance.Emitter ? Instance.GetEmitterData() : nullptr;
			ENiagaraSimTarget Target = ENiagaraSimTarget::CPUSim;
			bool bFlag = false;
			int32 Number = 0;
			TOptional<FBox> Bounds;
			if (!Data)
			{
				OutError = FString::Printf(TEXT("No emitter named %s."), *Op.EmitterName);
				return false;
			}
			if (!ParseEmitterProperty(Op, Target, bFlag, Number, Bounds))
			{
				OutError = TEXT("The emitter property value no longer parses.");
				return false;
			}
			OutStatus = TEXT("emitter_property:") + Op.Name;
			if (Op.Name == TEXT("enabled"))
			{
				Handle->SetIsEnabled(bFlag, System, /*bRecompileIfChanged=*/true);
				return true;
			}
			// The Niagara stack's own Set Gpu Simulation fix: Modify, write, PostEditChangeVersionedProperty.
			Instance.Emitter->Modify();
			FName Changed;
			if (Op.Name == TEXT("sim_target")) { Data->SimTarget = Target; Changed = GET_MEMBER_NAME_CHECKED(FVersionedNiagaraEmitterData, SimTarget); }
			else if (Op.Name == TEXT("local_space")) { Data->bLocalSpace = bFlag; Changed = GET_MEMBER_NAME_CHECKED(FVersionedNiagaraEmitterData, bLocalSpace); }
			else if (Op.Name == TEXT("determinism")) { Data->bDeterminism = bFlag; Changed = GET_MEMBER_NAME_CHECKED(FVersionedNiagaraEmitterData, bDeterminism); }
			else if (Op.Name == TEXT("random_seed")) { Data->RandomSeed = Number; Changed = GET_MEMBER_NAME_CHECKED(FVersionedNiagaraEmitterData, RandomSeed); }
			else
			{
				Data->CalculateBoundsMode = Bounds.IsSet() ? ENiagaraEmitterCalculateBoundMode::Fixed : ENiagaraEmitterCalculateBoundMode::Dynamic;
				if (Bounds.IsSet())
				{
					Data->FixedBounds = Bounds.GetValue();
				}
				Changed = GET_MEMBER_NAME_CHECKED(FVersionedNiagaraEmitterData, CalculateBoundsMode);
			}
			FProperty* Property = FindFProperty<FProperty>(FVersionedNiagaraEmitterData::StaticStruct(), Changed);
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Instance.Emitter->PostEditChangeVersionedProperty(Event, Instance.Version);
			return true;
		}

		bool ContainsBinding(const FProperty* Property, const int32 Depth = 0)
		{
			if (!Property || Depth > 4)
			{
				return false;
			}
			if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
			{
				if (Struct->Struct->GetName().Contains(TEXT("Binding")))
				{
					return true;
				}
				for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
				{
					if (ContainsBinding(*It, Depth + 1))
					{
						return true;
					}
				}
				return false;
			}
			if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
			{
				return ContainsBinding(Array->Inner, Depth + 1);
			}
			return false;
		}

		bool IsWritableRendererProperty(const FProperty& Property)
		{
			return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated);
		}

		bool HoldsSubobjectsOrContainers(const FProperty& Property)
		{
			return Property.ArrayDim != 1 || Property.HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance)
				|| CastField<FMapProperty>(&Property) || CastField<FSetProperty>(&Property);
		}

		/** What set_renderer_property writes: editable values, not bindings, instanced subobjects, maps or sets. */
		bool IsSettableRendererProperty(const FProperty& Property)
		{
			return IsWritableRendererProperty(Property) && !HoldsSubobjectsOrContainers(Property) && !ContainsBinding(&Property);
		}

		int32 EditDistance(const FString& A, const FString& B)
		{
			TArray<int32> Row;
			Row.SetNumUninitialized(B.Len() + 1);
			for (int32 J = 0; J <= B.Len(); ++J) Row[J] = J;
			for (int32 I = 1; I <= A.Len(); ++I)
			{
				int32 Diagonal = Row[0];
				Row[0] = I;
				for (int32 J = 1; J <= B.Len(); ++J)
				{
					const int32 Above = Row[J];
					Row[J] = FMath::Min3(Row[J] + 1, Row[J - 1] + 1, Diagonal + (FChar::ToLower(A[I - 1]) == FChar::ToLower(B[J - 1]) ? 0 : 1));
					Diagonal = Above;
				}
			}
			return Row[B.Len()];
		}

		FString SuggestRendererProperties(const UClass& Class, const FString& Wanted)
		{
			TArray<TPair<int32, FString>> Scored;
			for (TFieldIterator<FProperty> It(&Class); It; ++It)
			{
				if (!IsSettableRendererProperty(**It)) continue;
				const FString Name = It->GetName();
				const bool bContains = Name.Contains(Wanted) || Wanted.Contains(Name);
				Scored.Add({bContains ? 0 : EditDistance(Name, Wanted), Name});
			}
			Scored.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key < B.Key || (A.Key == B.Key && A.Value < B.Value); });
			TArray<FString> Names;
			for (int32 Index = 0; Index < Scored.Num() && Index < MaxSuggestions; ++Index) Names.Add(Scored[Index].Value);
			return FString::Join(Names, TEXT(", "));
		}

		/** The renderer property set_renderer_property may write, or the reason it may not. */
		FProperty* FindRendererProperty(const UClass& Class, const FString& Name, FString& OutStatus, FString& OutError)
		{
			FProperty* Property = FindFProperty<FProperty>(&Class, FName(*Name));
			if (!Property)
			{
				OutStatus = TEXT("renderer_property_not_found");
				OutError = FString::Printf(TEXT("%s has no property %s. Closest editable: %s."), *Class.GetName(), *Name, *SuggestRendererProperties(Class, Name));
				return nullptr;
			}
			OutStatus = TEXT("renderer_property_not_settable");
			if (!IsWritableRendererProperty(*Property))
			{
				OutError = FString::Printf(TEXT("%s.%s is not editable."), *Class.GetName(), *Name);
				return nullptr;
			}
			if (HoldsSubobjectsOrContainers(*Property))
			{
				OutError = FString::Printf(TEXT("%s.%s holds instanced objects or a map or set; edit it in the Niagara editor."), *Class.GetName(), *Name);
				return nullptr;
			}
			if (ContainsBinding(Property))
			{
				OutError = FString::Printf(TEXT("%s.%s is an attribute binding; bind attributes in the Niagara editor's renderer panel."), *Class.GetName(), *Name);
				return nullptr;
			}
			OutStatus.Reset();
			return Property;
		}

		/** "4,4" for an FVector2D, "1,0,0" for an FLinearColor and the like, in Unreal's struct text form. */
		FString ExpandShorthand(const FProperty& Property, const FString& Value)
		{
			const FStructProperty* Struct = CastField<FStructProperty>(&Property);
			if (!Struct || Value.Contains(TEXT("(")) || !Value.Contains(TEXT(",")))
			{
				return Value;
			}
			static const TMap<FName, TArray<const TCHAR*>> Fields = {
				{TEXT("Vector2D"), {TEXT("X"), TEXT("Y")}}, {TEXT("Vector2f"), {TEXT("X"), TEXT("Y")}}, {TEXT("IntPoint"), {TEXT("X"), TEXT("Y")}},
				{TEXT("Vector"), {TEXT("X"), TEXT("Y"), TEXT("Z")}}, {TEXT("Vector3f"), {TEXT("X"), TEXT("Y"), TEXT("Z")}},
				{TEXT("IntVector"), {TEXT("X"), TEXT("Y"), TEXT("Z")}}, {TEXT("Rotator"), {TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")}},
				{TEXT("Vector4"), {TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")}}, {TEXT("Vector4f"), {TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")}},
				{TEXT("LinearColor"), {TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")}}, {TEXT("Color"), {TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")}}};
			const TArray<const TCHAR*>* Names = Fields.Find(Struct->Struct->GetFName());
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(","), false);
			if (!Names || (Parts.Num() != Names->Num() && !(Parts.Num() == 3 && Names->Num() == 4 && Struct->Struct->GetFName() == TEXT("LinearColor"))))
			{
				return Value;
			}
			TArray<FString> Assigned;
			for (int32 Index = 0; Index < Parts.Num(); ++Index)
			{
				Assigned.Add(FString::Printf(TEXT("%s=%s"), (*Names)[Index], *Parts[Index].TrimStartAndEnd()));
			}
			return TEXT("(") + FString::Join(Assigned, TEXT(",")) + TEXT(")");
		}

		/** Parses Value as Property's type into Destination; all of it must be consumed. */
		bool ImportValue(const FProperty& Property, const FString& Value, UObject* Owner, void* Destination, FString& OutError)
		{
			const FString Text = ExpandShorthand(Property, Value.TrimStartAndEnd());
			// The import makes names of the text, and loads an asset it names (making the package when nothing is there).
			if (!FHyperAIStudioNames::CanImportText(Property, Text, TEXT("value"), &OutError)) return false;
			FOutputDeviceNull Quiet;
			const TCHAR* End = Property.ImportText_Direct(*Text, Destination, Owner, PPF_None, &Quiet);
			while (End && FChar::IsWhitespace(*End))
			{
				++End;
			}
			if (!End || *End != TEXT('\0'))
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid %s for %s."), *Value.Left(96), *Property.GetCPPType(), *Property.GetName());
				return false;
			}
			return true;
		}

		/** Imports into a scratch value first, so a bad value never reaches the object. */
		bool ParseInto(const FProperty& Property, const FString& Value, UObject* Owner, void* OptionalDestination, FString& OutError)
		{
			void* Scratch = FMemory::Malloc(Property.GetSize(), Property.GetMinAlignment());
			Property.InitializeValue(Scratch);
			const bool bParsed = ImportValue(Property, Value, Owner, Scratch, OutError);
			if (bParsed && OptionalDestination)
			{
				Property.CopyCompleteValue(OptionalDestination, Scratch);
			}
			Property.DestroyValue(Scratch);
			FMemory::Free(Scratch);
			return bParsed;
		}

		bool SetRendererProperty(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op, FString& OutStatus, FString& OutError)
		{
			FNiagaraEmitterHandle* Handle = FindHandle(System, Op.EmitterName);
			const FVersionedNiagaraEmitterData* Data = Handle ? Handle->GetEmitterData() : nullptr;
			UNiagaraRendererProperties* Renderer = Data && Data->GetRenderers().IsValidIndex(Op.RendererIndex)
				? Data->GetRenderers()[Op.RendererIndex] : nullptr;
			if (!Renderer)
			{
				OutError = FString::Printf(TEXT("%s has no renderer %d."), *Op.EmitterName, Op.RendererIndex);
				return false;
			}
			FString Status;
			FProperty* Property = FindRendererProperty(*Renderer->GetClass(), Op.Name, Status, OutError);
			if (!Property)
			{
				return false;
			}
			// The Details panel's order: PreEditChange (which calls Modify), write, PostEditChangeProperty.
			Renderer->PreEditChange(Property);
			const bool bSet = ParseInto(*Property, Op.Value, Renderer, Property->ContainerPtrToValuePtr<void>(Renderer), OutError);
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Renderer->PostEditChangeProperty(Event);
			if (!bSet)
			{
				return false;
			}
			OutStatus = FString::Printf(TEXT("renderer_property:%d.%s"), Op.RendererIndex, *Op.Name);
			return true;
		}

		/** What validation knows about one emitter as the plan walks forward. */
		struct FPlannedEmitter
		{
			bool bGpu = false;
			TMap<FName, TSet<FName>> StageModules;
			TArray<const UClass*> Renderers;
		};

		void Describe(const FVersionedNiagaraEmitterData& Data, FPlannedEmitter& Out)
		{
			Out.bGpu = Data.SimTarget == ENiagaraSimTarget::GPUComputeSim;
			UNiagaraGraph* Graph = GraphOf(Data.GraphSource);
			for (const UNiagaraSimulationStageBase* Stage : Data.GetSimulationStages())
			{
				if (!Stage)
				{
					continue;
				}
				TSet<FName>& Modules = Out.StageModules.Add(Stage->SimulationStageName);
				UNiagaraNodeOutput* Output = Graph && Stage->Script
					? Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSimulationStageScript, Stage->Script->GetUsageId()) : nullptr;
				if (Output)
				{
					for (const UNiagaraNodeFunctionCall* Call : ModulesOf(*Output))
					{
						Modules.Add(FName(*Call->GetFunctionName()));
					}
				}
			}
			for (const UNiagaraRendererProperties* Renderer : Data.GetRenderers())
			{
				Out.Renderers.Add(Renderer ? Renderer->GetClass() : nullptr);
			}
		}

		FString JoinNames(const TArray<FName>& Names)
		{
			TArray<FString> Strings;
			for (int32 Index = 0; Index < Names.Num() && Index < 16; ++Index)
			{
				Strings.Add(Names[Index].ToString());
			}
			return Strings.IsEmpty() ? FString(TEXT("none")) : FString::Join(Strings, TEXT(", "));
		}
	}

	bool IsGraphOp(const FHyperAINiagaraEditOp& Op)
	{
		return Op.Kind == TEXT("add_simulation_stage") || Op.Kind == TEXT("add_hlsl_module")
			|| Op.Kind == TEXT("set_emitter_property") || Op.Kind == TEXT("set_renderer_property")
			|| ((Op.Kind == TEXT("add_module") || Op.Kind == TEXT("set_module_enabled")) && !Op.StageName.IsEmpty());
	}

	bool ValidateGraphOp(const FHyperAINiagaraEditOp& Op, const bool bResolveAssets, FString& OutStatus, FString& OutDiagnostic)
	{
		OutStatus.Reset();
		OutDiagnostic.Reset();
		if (!Op.Hlsl.IsEmpty() && Op.Kind != TEXT("add_hlsl_module"))
		{
			return Fail(OutStatus, OutDiagnostic, TEXT("invalid_op_address"), TEXT("Only add_hlsl_module takes hlsl."));
		}
		if (Op.Kind == TEXT("add_simulation_stage"))
		{
			int32 Iterations = 1;
			if (Op.EmitterName.IsEmpty() || !Op.ScriptName.IsEmpty() || !Op.StageName.IsEmpty() || !Op.ModuleName.IsEmpty() || !IsIdentifier(Op.Name))
			{
				return Fail(OutStatus, OutDiagnostic, TEXT("invalid_op_address"),
					TEXT("add_simulation_stage needs emitter_name and name (letters, digits and underscores), and no script, stage or module address."));
			}
			if (!Op.Value.IsEmpty() && (!ParseInt(Op.Value, Iterations) || Iterations < 1 || Iterations > MaxStageIterations))
			{
				return Fail(OutStatus, OutDiagnostic, TEXT("invalid_iterations"),
					FString::Printf(TEXT("value is the stage's iteration count, 1-%d, or empty for 1."), MaxStageIterations));
			}
			return true;
		}
		if (Op.Kind == TEXT("add_hlsl_module"))
		{
			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
			FString HlslError;
			if (!ParseStack(Op, Usage))
			{
				return Fail(OutStatus, OutDiagnostic, TEXT("invalid_script_name"),
					TEXT("script_name must be SystemSpawnScript, SystemUpdateScript, EmitterSpawnScript, EmitterUpdateScript, ParticleSpawnScript, ParticleUpdateScript or ParticleSimulationStageScript."));
			}
			if (IsSystemUsage(Usage) != Op.EmitterName.IsEmpty() || IsStageUsage(Usage) == Op.StageName.IsEmpty()
				|| !Op.ModuleName.IsEmpty() || !Op.AssetPath.IsEmpty() || !IsIdentifier(Op.Name))
			{
				return Fail(OutStatus, OutDiagnostic, TEXT("invalid_op_address"),
					TEXT("add_hlsl_module needs name (letters, digits and underscores), script_name, emitter_name for emitter scripts, and stage_name exactly when script_name is ParticleSimulationStageScript."));
			}
			if (!CheckHlsl(Op.Hlsl, HlslError))
			{
				return Fail(OutStatus, OutDiagnostic, TEXT("invalid_hlsl"), HlslError);
			}
			return true;
		}
		if (Op.Kind == TEXT("add_module") || Op.Kind == TEXT("set_module_enabled"))
		{
			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
			const bool bAdd = Op.Kind == TEXT("add_module");
			if (Op.EmitterName.IsEmpty() || !ParseStack(Op, Usage) || !IsStageUsage(Usage) || !Op.InputNameStack.IsEmpty()
				|| (bAdd ? Op.AssetPath.IsEmpty() : Op.ModuleName.IsEmpty()))
			{
				return Fail(OutStatus, OutDiagnostic, TEXT("invalid_op_address"), bAdd
					? TEXT("add_module into a stage needs emitter_name, stage_name and asset_path of a Niagara module script; script_name is ParticleSimulationStageScript or empty.")
					: TEXT("set_module_enabled in a stage needs emitter_name, stage_name and module_name; script_name is ParticleSimulationStageScript or empty."));
			}
			if (bAdd && bResolveAssets)
			{
				const UNiagaraScript* Module = Cast<UNiagaraScript>(FSoftObjectPath(Op.AssetPath).TryLoad());
				if (!Module || Module->GetUsage() != ENiagaraScriptUsage::Module)
				{
					return Fail(OutStatus, OutDiagnostic, TEXT("module_script_not_found"), FString::Printf(TEXT("No Niagara module script at %s."), *Op.AssetPath));
				}
			}
			return true;
		}
		if (Op.Kind == TEXT("set_emitter_property"))
		{
			ENiagaraSimTarget Target = ENiagaraSimTarget::CPUSim;
			bool bFlag = false;
			int32 Number = 0;
			TOptional<FBox> Bounds;
			if (Op.EmitterName.IsEmpty() || !Op.ScriptName.IsEmpty() || !Op.StageName.IsEmpty() || !Op.ModuleName.IsEmpty())
			{
				return Fail(OutStatus, OutDiagnostic, TEXT("invalid_op_address"), TEXT("set_emitter_property needs emitter_name and no script, stage or module address."));
			}
			if (!ParseEmitterProperty(Op, Target, bFlag, Number, Bounds))
			{
				return Fail(OutStatus, OutDiagnostic, TEXT("invalid_emitter_property"), EmitterPropertyHelp());
			}
			return true;
		}
		if (Op.Kind == TEXT("set_renderer_property"))
		{
			if (Op.EmitterName.IsEmpty() || Op.RendererIndex < 0 || !Op.ScriptName.IsEmpty() || !Op.StageName.IsEmpty() || !IsIdentifier(Op.Name))
			{
				return Fail(OutStatus, OutDiagnostic, TEXT("invalid_op_address"),
					TEXT("set_renderer_property needs emitter_name, renderer_index from inspect, and name, the renderer's property (e.g. Alignment, SubImageSize, Material)."));
			}
			if (Op.Value.TrimStartAndEnd().IsEmpty())
			{
				return Fail(OutStatus, OutDiagnostic, TEXT("invalid_renderer_property_value"), TEXT("value is required, in Unreal's text form: VelocityAligned, 2,2, true or /Game/Path/Asset.Asset."));
			}
			return true;
		}
		return Fail(OutStatus, OutDiagnostic, TEXT("unsupported_op_kind"), TEXT("Not a graph op."));
	}

	bool ValidatePlanAgainstSystem(UNiagaraSystem& System, const TArray<FHyperAINiagaraEditOp>& Ops, FString& OutStatus, FString& OutDiagnostic)
	{
		OutStatus.Reset();
		OutDiagnostic.Reset();
		TMap<FName, FPlannedEmitter> Emitters;
		for (FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
		{
			if (const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
			{
				Describe(*Data, Emitters.Add(Handle.GetName()));
			}
		}
		for (int32 Index = 0; Index < Ops.Num(); ++Index)
		{
			const FHyperAINiagaraEditOp& Op = Ops[Index];
			auto Reject = [&](const TCHAR* Status, const FString& Why)
			{
				return Fail(OutStatus, OutDiagnostic, Status, FString::Printf(TEXT("Op %d: %s"), Index, *Why));
			};
			if (Op.Kind == TEXT("add_emitter"))
			{
				if (Emitters.Contains(FName(*Op.Name)))
				{
					return Reject(TEXT("emitter_name_taken"), FString::Printf(TEXT("An emitter named %s exists already; the new one would be renamed and later ops would miss it. Pick another name."), *Op.Name));
				}
				FPlannedEmitter& Added = Emitters.Add(FName(*Op.Name));
				const UNiagaraEmitter* Template = Cast<UNiagaraEmitter>(FSoftObjectPath(Op.AssetPath).ResolveObject());
				if (const FVersionedNiagaraEmitterData* Data = Template ? Template->GetLatestEmitterData() : nullptr)
				{
					Describe(*Data, Added);
				}
				continue;
			}
			if (Op.EmitterName.IsEmpty())
			{
				continue;
			}
			FPlannedEmitter* Emitter = Emitters.Find(FName(*Op.EmitterName));
			if (!Emitter)
			{
				TArray<FName> Names;
				Emitters.GetKeys(Names);
				return Reject(TEXT("emitter_not_found"), FString::Printf(TEXT("No emitter named %s. Emitters: %s."), *Op.EmitterName, *JoinNames(Names)));
			}
			if (Op.Kind == TEXT("add_renderer"))
			{
				Emitter->Renderers.Add(FSoftClassPath(Op.AssetPath).ResolveClass());
				continue;
			}
			if (Op.Kind == TEXT("set_emitter_property") && Op.Name == TEXT("sim_target"))
			{
				ENiagaraSimTarget Target = ENiagaraSimTarget::CPUSim;
				Emitter->bGpu = ParseSimTarget(Op.Value, Target) && Target == ENiagaraSimTarget::GPUComputeSim;
				continue;
			}
			if (Op.Kind == TEXT("add_simulation_stage"))
			{
				if (Emitter->StageModules.Contains(FName(*Op.Name)))
				{
					return Reject(TEXT("stage_name_taken"), FString::Printf(TEXT("%s already has a simulation stage named %s."), *Op.EmitterName, *Op.Name));
				}
				if (!Emitter->bGpu)
				{
					return Reject(TEXT("simulation_stage_needs_gpu"), FString::Printf(TEXT("Simulation stages run only on GPU emitters in UE 5.8, and %s is CPU. Put a set_emitter_property op (name sim_target, value gpu) for it before this op."), *Op.EmitterName));
				}
				Emitter->StageModules.Add(FName(*Op.Name));
				continue;
			}
			if (!Op.StageName.IsEmpty())
			{
				TSet<FName>* Modules = Emitter->StageModules.Find(FName(*Op.StageName));
				if (!Modules)
				{
					TArray<FName> Names;
					Emitter->StageModules.GetKeys(Names);
					return Reject(TEXT("stage_not_found"), FString::Printf(TEXT("%s has no simulation stage named %s. Stages: %s."), *Op.EmitterName, *Op.StageName, *JoinNames(Names)));
				}
				if (Op.Kind == TEXT("set_module_enabled") && !Modules->Contains(FName(*Op.ModuleName)))
				{
					return Reject(TEXT("module_not_found"), FString::Printf(TEXT("Stage %s has no module named %s. Modules: %s."), *Op.StageName, *Op.ModuleName, *JoinNames(Modules->Array())));
				}
				if (Op.Kind == TEXT("add_hlsl_module"))
				{
					Modules->Add(FName(*Op.Name));
				}
				else if (Op.Kind == TEXT("add_module"))
				{
					const UObject* Module = FSoftObjectPath(Op.AssetPath).ResolveObject();
					Modules->Add(Module ? Module->GetFName() : NAME_None);
				}
				continue;
			}
			if (Op.Kind == TEXT("set_renderer_property"))
			{
				if (!Emitter->Renderers.IsValidIndex(Op.RendererIndex) || !Emitter->Renderers[Op.RendererIndex])
				{
					return Reject(TEXT("renderer_index_out_of_range"), FString::Printf(TEXT("%s has %d renderers; renderer_index %d is not one of them."),
						*Op.EmitterName, Emitter->Renderers.Num(), Op.RendererIndex));
				}
				const UClass& Class = *Emitter->Renderers[Op.RendererIndex];
				FString Status;
				FString Error;
				const FProperty* Property = FindRendererProperty(Class, Op.Name, Status, Error);
				if (!Property)
				{
					return Reject(*Status, Error);
				}
				if (!ParseInto(*Property, Op.Value, Class.GetDefaultObject(), nullptr, Error))
				{
					return Reject(TEXT("invalid_renderer_property_value"), Error);
				}
			}
		}
		return true;
	}

	bool ApplyGraphOp(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op, FString& OutStatus, FString& OutError)
	{
		OutStatus.Reset();
		OutError.Reset();
		bool bApplied = false;
		if (Op.Kind == TEXT("add_simulation_stage")) bApplied = AddSimulationStage(System, Op, OutStatus, OutError);
		else if (Op.Kind == TEXT("add_hlsl_module")) bApplied = AddHlslModule(System, Op, OutStatus, OutError);
		else if (Op.Kind == TEXT("add_module")) bApplied = AddStageModule(System, Op, OutStatus, OutError);
		else if (Op.Kind == TEXT("set_module_enabled")) bApplied = SetStageModuleEnabled(System, Op, OutStatus, OutError);
		else if (Op.Kind == TEXT("set_emitter_property")) bApplied = SetEmitterProperty(System, Op, OutStatus, OutError);
		else if (Op.Kind == TEXT("set_renderer_property")) bApplied = SetRendererProperty(System, Op, OutStatus, OutError);
		else OutError = TEXT("Unsupported graph op.");
		if (bApplied)
		{
			// What Epic's AddModule does after a stack edit: refresh the System's GPU flags, scalability and exposed parameters.
			FPropertyChangedEvent Changed(nullptr, EPropertyChangeType::ValueSet);
			System.PostEditChangeProperty(Changed);
		}
		return bApplied;
	}

	void ReadSimulationStages(UNiagaraSystem& System, const FString& EmitterName, TArray<FHyperAINiagaraSimStageTopology>& OutStages,
		int32& InOutModuleBudget, bool& bOutTruncated)
	{
		FNiagaraEmitterHandle* Handle = FindHandle(System, EmitterName);
		const FVersionedNiagaraEmitterData* Data = Handle ? Handle->GetEmitterData() : nullptr;
		if (!Data)
		{
			return;
		}
		UNiagaraGraph* Graph = GraphOf(Data->GraphSource);
		for (const UNiagaraSimulationStageBase* Stage : Data->GetSimulationStages())
		{
			if (!Stage)
			{
				continue;
			}
			FHyperAINiagaraSimStageTopology& Out = OutStages.AddDefaulted_GetRef();
			Out.StageName = Stage->SimulationStageName.ToString();
			Out.bEnabled = Stage->bEnabled;
			if (const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage))
			{
				Out.IterationSource = IterationSourceName(Generic->IterationSource);
				if (Generic->NumIterations.GetDefaultValueArray().Num() == sizeof(int32))
				{
					Out.Iterations = Generic->NumIterations.GetDefaultValue<int32>();
				}
			}
			UNiagaraNodeOutput* Output = Graph && Stage->Script
				? Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSimulationStageScript, Stage->Script->GetUsageId()) : nullptr;
			if (!Output)
			{
				continue;
			}
			for (const UNiagaraNodeFunctionCall* Call : ModulesOf(*Output))
			{
				if (InOutModuleBudget <= 0)
				{
					bOutTruncated = true;
					return;
				}
				--InOutModuleBudget;
				FHyperAINiagaraModuleTopology& Module = Out.Modules.AddDefaulted_GetRef();
				Module.ModuleName = Call->GetFunctionName();
				Module.bEnabled = Call->GetDesiredEnabledState() != ENodeEnabledState::Disabled;
				Module.bIsSetParametersModule = Call->IsA<UNiagaraNodeAssignment>();
				Module.ScriptAssetPath = Call->FunctionScript ? Call->FunctionScript->GetPathName() : FString();
			}
		}
	}

	TArray<FString> ReadScratchPadModules(const UNiagaraSystem& System)
	{
		TArray<FString> Names;
		for (const UNiagaraScript* Script : System.ScratchPadScripts)
		{
			if (Script)
			{
				Names.Add(Script->GetName());
			}
		}
		return Names;
	}
}
