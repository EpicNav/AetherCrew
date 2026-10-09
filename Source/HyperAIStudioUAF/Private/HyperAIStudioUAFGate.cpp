// Games by Hyper 2026.

#include "HyperAIStudioUAFGate.h"

#include "AnimNextController.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Graph/AnimNextAnimationGraph.h"
#include "Graph/AnimNextAnimationGraphFactory.h"
#include "Graph/AnimNextAnimationGraph_EditorData.h"
#include "Graph/AnimNextGraphInstance.h"
#include "Graph/RigUnit_AnimNextGraphRoot.h"
#include "Graph/RigUnit_AnimNextTraitStack.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioNames.h"
#include "Misc/PackageName.h"
#include "Param/ParamType.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "TraitCore/Trait.h"
#include "TraitCore/TraitHandle.h"
#include "TraitCore/TraitMode.h"
#include "TraitCore/TraitRegistry.h"
#include "UAFCompilationScope.h"
#include "UncookedOnlyUtils.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Variables/IAnimNextRigVMVariableInterface.h"

namespace HyperAIStudio::UAF::Gate
{
	namespace
	{
		using namespace UE::UAF;

		constexpr int32 MaxNameCharacters = 64;
		constexpr int32 MaxValueCharacters = 512;
		constexpr int32 MaxNodesPerGraph = 128;

		/** The trait pin's first sub-pin is the hard-coded struct that parametrises the trait, not an authored pin. */
		constexpr int32 FirstAuthoredSubPin = 1;

		void Append(FString& Canonical, const FString& Value)
		{
			Canonical += FString::FromInt(Value.Len());
			Canonical += TEXT(":");
			Canonical += Value;
			Canonical += TEXT("\n");
		}

		bool IsIdentifier(const FString& Name)
		{
			if (Name.IsEmpty() || Name.Len() > MaxNameCharacters) return false;
			for (const TCHAR Character : Name)
			{
				const bool bAllowed = (Character >= TEXT('a') && Character <= TEXT('z'))
					|| (Character >= TEXT('A') && Character <= TEXT('Z'))
					|| (Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('_');
				if (!bAllowed) return false;
			}
			return !(Name[0] >= TEXT('0') && Name[0] <= TEXT('9'));
		}

		UUAFAnimGraph_EditorData* EditorDataOf(const UUAFAnimGraph& Graph)
		{
			return UncookedOnly::FUtils::GetEditorData<UUAFAnimGraph_EditorData>(const_cast<UUAFAnimGraph*>(&Graph));
		}

		// The editor data keeps its RigVM accessors protected, but exposes them all through IRigVMClientHost.
		IRigVMClientHost* HostOf(const UUAFAnimGraph& Graph)
		{
			return Cast<IRigVMClientHost>(EditorDataOf(Graph));
		}

		URigVMGraph* ModelOf(const UUAFAnimGraph& Graph)
		{
			IRigVMClientHost* Host = HostOf(Graph);
			FRigVMClient* Client = Host ? Host->GetRigVMClient() : nullptr;
			return Client ? Client->GetDefaultModel() : nullptr;
		}

		UAnimNextController* ControllerOf(const UUAFAnimGraph& Graph)
		{
			IRigVMClientHost* Host = HostOf(Graph);
			URigVMGraph* Model = ModelOf(Graph);
			return (Host && Model) ? Cast<UAnimNextController>(Host->GetController(Model)) : nullptr;
		}

		/** Registered traits keyed by their shared-data struct name, which is what a trait pin is called. */
		const TMap<FName, const FTrait*>& TraitsByPinName()
		{
			static TMap<FName, const FTrait*> Map;
			static int32 CachedCount = -1;
			const int32 Count = static_cast<int32>(FTraitRegistry::Get().GetNum());
			if (Count != CachedCount)
			{
				CachedCount = Count;
				Map.Reset();
				for (const FTrait* Trait : FTraitRegistry::Get().GetTraits())
				{
					if (const UScriptStruct* Struct = Trait ? Trait->GetTraitSharedDataStruct() : nullptr)
					{
						Map.Add(Struct->GetFName(), Trait);
					}
				}
			}
			return Map;
		}

		const FTrait* FindTrait(const FString& Name)
		{
			return Name.IsEmpty() || !FHyperAIStudioNames::IsName(Name, nullptr) ? nullptr : FTraitRegistry::Get().Find(FName(*Name));
		}

		FString TraitNameOfPin(const URigVMPin& Pin)
		{
			const FTrait* const* Trait = TraitsByPinName().Find(Pin.GetFName());
			return Trait && *Trait ? (*Trait)->GetTraitName() : Pin.GetName();
		}

		const TCHAR* ModeOf(const FTrait& Trait)
		{
			switch (Trait.GetTraitMode())
			{
			case ETraitMode::Base: return TEXT("base");
			case ETraitMode::Additive: return TEXT("additive");
			default: return TEXT("unknown");
			}
		}

		bool IsPoseProperty(const FProperty& Property)
		{
			const FStructProperty* StructProperty = CastField<FStructProperty>(&Property);
			return StructProperty && StructProperty->Struct == FAnimNextTraitHandle::StaticStruct();
		}

		FProperty* FindTraitProperty(const FTrait& Trait, const FString& PinName)
		{
			UScriptStruct* Struct = Trait.GetTraitSharedDataStruct();
			if (!Struct || PinName.IsEmpty()) return nullptr;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (It->GetName() == PinName) return *It;
			}
			return nullptr;
		}

		URigVMPin* FindTraitPin(URigVMNode& Node, const FTrait& Trait)
		{
			const UScriptStruct* Struct = Trait.GetTraitSharedDataStruct();
			return Struct ? Node.FindPin(Struct->GetName()) : nullptr;
		}

		URigVMPin* FindSubPin(URigVMPin& TraitPin, const FString& Name)
		{
			for (URigVMPin* SubPin : TraitPin.GetSubPins())
			{
				if (SubPin && SubPin->GetName() == Name) return SubPin;
			}
			return nullptr;
		}

		bool IsTraitStack(const URigVMNode& Node)
		{
			const URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(&Node);
			return UnitNode && UnitNode->GetScriptStruct() == FRigUnit_AnimNextTraitStack::StaticStruct();
		}

		TArray<URigVMNode*> StackNodes(const URigVMGraph& Model)
		{
			TArray<URigVMNode*> Nodes;
			for (URigVMNode* Node : Model.GetNodes())
			{
				if (Node && IsTraitStack(*Node)) Nodes.Add(Node);
			}
			Nodes.Sort([](const URigVMNode& Left, const URigVMNode& Right) { return Left.GetName() < Right.GetName(); });
			return Nodes;
		}

		URigVMNode* RootNode(const URigVMGraph& Model)
		{
			return Model.FindNodeByName(FRigUnit_AnimNextGraphRoot::StaticStruct()->GetFName());
		}

		URigVMPin* RootResultPin(const URigVMGraph& Model)
		{
			URigVMNode* Root = RootNode(Model);
			return Root ? Root->FindPin(TEXT("Result")) : nullptr;
		}

		TArray<URigVMPin*> TraitPinsOf(URigVMNode& Node)
		{
			TArray<URigVMPin*> Pins;
			for (URigVMPin* Pin : Node.GetPins())
			{
				if (Pin && Pin->IsTraitPin()) Pins.Add(Pin);
			}
			return Pins;
		}

		/** The node feeding a pin, following the link back to whichever node owns the source pin. */
		URigVMNode* SourceNodeOf(const URigVMPin& Pin)
		{
			const TArray<URigVMPin*> Sources = Pin.GetLinkedSourcePins(/*bRecursive=*/false);
			return Sources.IsEmpty() || !Sources[0] ? nullptr : Sources[0]->GetNode();
		}

		/** Trait-stack nodes whose pose reaches the graph's output, walking pose pins back from the root. */
		TSet<FString> ReachableFromOutput(const URigVMGraph& Model)
		{
			TSet<FString> Reached;
			URigVMPin* Result = RootResultPin(Model);
			URigVMNode* First = Result ? SourceNodeOf(*Result) : nullptr;
			TArray<URigVMNode*> Pending;
			if (First && IsTraitStack(*First)) Pending.Add(First);
			while (!Pending.IsEmpty())
			{
				URigVMNode* Node = Pending.Pop();
				if (!Node || Reached.Contains(Node->GetName())) continue;
				Reached.Add(Node->GetName());
				for (URigVMPin* TraitPin : TraitPinsOf(*Node))
				{
					for (URigVMPin* SubPin : TraitPin->GetSubPins())
					{
						URigVMNode* Source = SubPin ? SourceNodeOf(*SubPin) : nullptr;
						if (Source && IsTraitStack(*Source)) Pending.Add(Source);
					}
				}
			}
			return Reached;
		}

		FString TypeNameOf(const FProperty& Property)
		{
			return Property.GetCPPType();
		}

		/** CompilationState is protected on UUAFRigVMAsset; this reads that one named UPROPERTY and nothing else. */
		const FEnumProperty* CompilationStateProperty()
		{
			static const FEnumProperty* Property =
				FindFProperty<FEnumProperty>(UUAFRigVMAsset::StaticClass(), TEXT("CompilationState"));
			return Property;
		}

		void CollectCatalogPins(const FTrait& Trait, TArray<FHyperAIUAFPin>& OutPins)
		{
			UScriptStruct* Struct = Trait.GetTraitSharedDataStruct();
			if (!Struct) return;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				FHyperAIUAFPin& Pin = OutPins.AddDefaulted_GetRef();
				Pin.Name = It->GetName();
				Pin.Type = TypeNameOf(**It);
				Pin.bPose = IsPoseProperty(**It);
			}
		}

		EPropertyBagPropertyType VariableValueType(const FString& Type, const UObject*& OutTypeObject)
		{
			OutTypeObject = nullptr;
			if (Type == TEXT("bool")) return EPropertyBagPropertyType::Bool;
			if (Type == TEXT("float")) return EPropertyBagPropertyType::Float;
			if (Type == TEXT("int")) return EPropertyBagPropertyType::Int32;
			if (Type == TEXT("name")) return EPropertyBagPropertyType::Name;
			if (Type == TEXT("vector"))
			{
				OutTypeObject = TBaseStructure<FVector>::Get();
				return EPropertyBagPropertyType::Struct;
			}
			return EPropertyBagPropertyType::None;
		}

		FString VariableTypeName(const FAnimNextParamType& Type)
		{
			switch (Type.GetValueType())
			{
			case EPropertyBagPropertyType::Bool: return TEXT("bool");
			case EPropertyBagPropertyType::Float: return TEXT("float");
			case EPropertyBagPropertyType::Double: return TEXT("float");
			case EPropertyBagPropertyType::Int32: return TEXT("int");
			case EPropertyBagPropertyType::Name: return TEXT("name");
			case EPropertyBagPropertyType::Struct:
				return Type.GetValueTypeObject() == TBaseStructure<FVector>::Get() ? TEXT("vector") : TEXT("other");
			default: return TEXT("other");
			}
		}

		IUAFRigVMVariableInterface* FindVariableEntry(UUAFAnimGraph& Graph, const FString& Name)
		{
			UUAFRigVMAssetEntry* Entry = UUAFRigVMAssetLibrary::FindEntry(&Graph, FName(*Name));
			return Entry ? Cast<IUAFRigVMVariableInterface>(Entry) : nullptr;
		}

		TArray<FString> VariableNamesOf(UUAFAnimGraph& Graph)
		{
			TArray<FString> Names;
			for (const FRigVMExternalVariable& Variable : Graph.GetExternalVariables())
			{
				Names.AddUnique(Variable.GetName().ToString());
			}
			return Names;
		}

		/** Variable getters wired into the graph, by variable name. */
		TSet<FString> BoundVariables(const URigVMGraph& Model)
		{
			TSet<FString> Bound;
			for (URigVMNode* Node : Model.GetNodes())
			{
				const URigVMVariableNode* VariableNode = Cast<URigVMVariableNode>(Node);
				if (!VariableNode) continue;
				URigVMPin* Value = VariableNode->FindPin(TEXT("Value"));
				if (Value && !Value->GetLinkedTargetPins(/*bRecursive=*/false).IsEmpty())
				{
					Bound.Add(VariableNode->GetVariableName().ToString());
				}
			}
			return Bound;
		}
	}

	UUAFAnimGraph* Resolve(const FString& Path, const bool bLoad)
	{
		if (Path.IsEmpty()) return nullptr;
		if (UUAFAnimGraph* Loaded = FindObject<UUAFAnimGraph>(nullptr, *Path)) return Loaded;
		return bLoad ? LoadObject<UUAFAnimGraph>(nullptr, *Path) : nullptr;
	}

	bool IsOpenInEditor(const UUAFAnimGraph& Graph)
	{
		UAssetEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		return Subsystem && !Subsystem->FindEditorsForAsset(const_cast<UUAFAnimGraph*>(&Graph)).IsEmpty();
	}

	FString CompileStatusOf(const UUAFAnimGraph& Graph)
	{
		const FEnumProperty* Property = CompilationStateProperty();
		if (!Property) return TEXT("not_compiled");
		const void* Value = Property->ContainerPtrToValuePtr<void>(&Graph);
		switch (static_cast<EAnimNextRigVMAssetState>(Property->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value)))
		{
		case EAnimNextRigVMAssetState::CompiledWithErrors: return TEXT("errors");
		case EAnimNextRigVMAssetState::CompiledWithWarnings: return TEXT("warnings");
		case EAnimNextRigVMAssetState::CompiledWithSuccess: return TEXT("compiled");
		default: return TEXT("not_compiled");
		}
	}

	FString ComputeRevision(const UUAFAnimGraph& Graph)
	{
		FString Canonical;
		Append(Canonical, TEXT("hyperai.uaf.graph-revision.v1"));
		URigVMGraph* Model = ModelOf(Graph);
		if (!Model)
		{
			Append(Canonical, TEXT("no_model"));
			return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
		}
		TArray<FString> Variables = VariableNamesOf(const_cast<UUAFAnimGraph&>(Graph));
		Variables.Sort();
		Append(Canonical, FString::FromInt(Variables.Num()));
		for (const FString& Variable : Variables) Append(Canonical, Variable);
		URigVMPin* Result = RootResultPin(*Model);
		URigVMNode* OutputSource = Result ? SourceNodeOf(*Result) : nullptr;
		Append(Canonical, OutputSource ? OutputSource->GetName() : TEXT("no_output"));
		TArray<URigVMNode*> Nodes = Model->GetNodes();
		Nodes.Sort([](const URigVMNode& Left, const URigVMNode& Right) { return Left.GetName() < Right.GetName(); });
		Append(Canonical, FString::FromInt(Nodes.Num()));
		for (URigVMNode* Node : Nodes)
		{
			if (!Node) continue;
			Append(Canonical, Node->GetName());
			Append(Canonical, Node->GetNodeTitle());
			for (URigVMPin* Pin : Node->GetPins())
			{
				if (!Pin) continue;
				Append(Canonical, Pin->GetName());
				for (URigVMPin* SubPin : Pin->GetSubPins())
				{
					if (!SubPin) continue;
					Append(Canonical, SubPin->GetName());
					Append(Canonical, SubPin->GetDefaultValue());
					const URigVMNode* Source = SourceNodeOf(*SubPin);
					Append(Canonical, Source ? Source->GetName() : FString());
				}
				const URigVMNode* Source = SourceNodeOf(*Pin);
				Append(Canonical, Source ? Source->GetName() : FString());
			}
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	void Read(const UUAFAnimGraph& Graph, TArray<FHyperAIUAFVariable>& OutVariables, TArray<FHyperAIUAFNode>& OutNodes, FString& OutOutputNode)
	{
		OutVariables.Reset();
		OutNodes.Reset();
		OutOutputNode.Reset();
		URigVMGraph* Model = ModelOf(Graph);
		if (!Model) return;
		const TSet<FString> Bound = BoundVariables(*Model);
		for (const FString& Name : VariableNamesOf(const_cast<UUAFAnimGraph&>(Graph)))
		{
			FHyperAIUAFVariable& Variable = OutVariables.AddDefaulted_GetRef();
			Variable.Name = Name;
			const IUAFRigVMVariableInterface* Entry = FindVariableEntry(const_cast<UUAFAnimGraph&>(Graph), Name);
			Variable.Type = Entry ? VariableTypeName(Entry->GetType()) : TEXT("other");
			Variable.bUsed = Bound.Contains(Name);
		}
		URigVMPin* Result = RootResultPin(*Model);
		if (const URigVMNode* Source = Result ? SourceNodeOf(*Result) : nullptr)
		{
			OutOutputNode = Source->GetName();
		}
		const TSet<FString> Reached = ReachableFromOutput(*Model);
		for (URigVMNode* Node : StackNodes(*Model))
		{
			FHyperAIUAFNode& Entry = OutNodes.AddDefaulted_GetRef();
			Entry.Name = Node->GetName();
			Entry.bReachesOutput = Reached.Contains(Entry.Name);
			for (URigVMPin* TraitPin : TraitPinsOf(*Node))
			{
				FHyperAIUAFTrait& Trait = Entry.Traits.AddDefaulted_GetRef();
				Trait.Name = TraitNameOfPin(*TraitPin);
				const FTrait* Registered = FindTrait(Trait.Name);
				Trait.Mode = Registered ? ModeOf(*Registered) : TEXT("unknown");
				const TArray<URigVMPin*>& SubPins = TraitPin->GetSubPins();
				for (int32 Index = FirstAuthoredSubPin; Index < SubPins.Num(); ++Index)
				{
					URigVMPin* SubPin = SubPins[Index];
					if (!SubPin) continue;
					FHyperAIUAFPin& Pin = Trait.Pins.AddDefaulted_GetRef();
					Pin.Name = SubPin->GetName();
					Pin.Type = SubPin->GetCPPType();
					Pin.bPose = SubPin->GetCPPTypeObject() == FAnimNextTraitHandle::StaticStruct();
					const URigVMNode* Source = SourceNodeOf(*SubPin);
					Pin.bLinked = Source != nullptr;
					if (const URigVMVariableNode* VariableNode = Cast<URigVMVariableNode>(Source))
					{
						Pin.Value = VariableNode->GetVariableName().ToString();
					}
					else
					{
						Pin.Value = Source ? Source->GetName() : SubPin->GetDefaultValue().Left(MaxValueCharacters);
					}
				}
			}
		}
	}

	TArray<FHyperAIUAFTrait> ReadTraitCatalog(const FString& Filter)
	{
		TArray<FHyperAIUAFTrait> Traits;
		for (const FTrait* Trait : FTraitRegistry::Get().GetTraits())
		{
			if (!Trait) continue;
			const FString Name = Trait->GetTraitName();
			if (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase)) continue;
			FHyperAIUAFTrait& Entry = Traits.AddDefaulted_GetRef();
			Entry.Name = Name;
			Entry.Mode = ModeOf(*Trait);
			CollectCatalogPins(*Trait, Entry.Pins);
		}
		Traits.Sort([](const FHyperAIUAFTrait& Left, const FHyperAIUAFTrait& Right) { return Left.Name < Right.Name; });
		return Traits;
	}

	namespace
	{
		/**
		 * One pass over the ops. With bWrite false it only checks them against the graph as it would be at that
		 * point, so an invalid plan is rejected before anything changes; with bWrite true it applies them.
		 */
		bool RunOps(UUAFAnimGraph* Graph, const TArray<FHyperAIUAFOp>& Ops, const bool bWrite, FString& OutError)
		{
			auto Fail = [&OutError](const FString& Message)
			{
				OutError = Message;
				return false;
			};
			UAnimNextController* Controller = nullptr;
			URigVMGraph* Model = nullptr;
			if (bWrite)
			{
				if (!Graph) return Fail(TEXT("The animation graph went away before the ops ran."));
				Controller = ControllerOf(*Graph);
				Model = ModelOf(*Graph);
				if (!Controller || !Model) return Fail(TEXT("The animation graph has no editable RigVM model."));
			}

			// Simulated state, so a dry run rejects the same plans the write would.
			TMap<FString, TArray<FString>> Nodes;
			TSet<FString> Variables;
			bool bOutputLinked = false;
			if (Graph)
			{
				if (URigVMGraph* Existing = ModelOf(*Graph))
				{
					for (URigVMNode* Node : StackNodes(*Existing))
					{
						TArray<FString>& Traits = Nodes.Add(Node->GetName());
						for (URigVMPin* TraitPin : TraitPinsOf(*Node)) Traits.Add(TraitNameOfPin(*TraitPin));
					}
					URigVMPin* Result = RootResultPin(*Existing);
					bOutputLinked = Result && SourceNodeOf(*Result) != nullptr;
				}
				for (const FString& Name : VariableNamesOf(*Graph)) Variables.Add(Name);
			}

			int32 Index = 0;
			for (const FHyperAIUAFOp& Op : Ops)
			{
				const FString Where = FString::Printf(TEXT("Op %d (%s)"), Index++, *Op.Kind);
				if (Op.Value.Len() > MaxValueCharacters)
				{
					return Fail(FString::Printf(TEXT("%s: value is longer than %d characters."), *Where, MaxValueCharacters));
				}

				if (Op.Kind == TEXT("add_variable"))
				{
					if (!IsIdentifier(Op.Name)) return Fail(Where + TEXT(": name must be an identifier."));
					if (Variables.Contains(Op.Name)) return Fail(FString::Printf(TEXT("%s: variable %s already exists."), *Where, *Op.Name));
					const UObject* TypeObject = nullptr;
					const EPropertyBagPropertyType Type = VariableValueType(Op.Value, TypeObject);
					if (Type == EPropertyBagPropertyType::None)
					{
						return Fail(Where + TEXT(": value must be bool, float, int, name or vector."));
					}
					Variables.Add(Op.Name);
					if (bWrite && !UUAFRigVMAssetLibrary::AddVariable(Graph, FName(*Op.Name), Type,
						EPropertyBagContainerType::None, TypeObject, FString(), /*bSetupUndoRedo=*/true, /*bPrintPythonCommand=*/false))
					{
						return Fail(FString::Printf(TEXT("%s: could not add variable %s."), *Where, *Op.Name));
					}
					continue;
				}

				if (Op.Kind == TEXT("add_node"))
				{
					if (!IsIdentifier(Op.Node)) return Fail(Where + TEXT(": node must be an identifier."));
					if (Nodes.Contains(Op.Node)) return Fail(FString::Printf(TEXT("%s: node %s already exists."), *Where, *Op.Node));
					if (Nodes.Num() >= MaxNodesPerGraph)
					{
						return Fail(FString::Printf(TEXT("%s: a graph takes at most %d trait stacks."), *Where, MaxNodesPerGraph));
					}
					const int32 Position = Nodes.Num();
					Nodes.Add(Op.Node);
					if (bWrite)
					{
						const URigVMUnitNode* Added = Controller->AddUnitNode(FRigUnit_AnimNextTraitStack::StaticStruct(),
							FRigVMStruct::ExecuteName, FVector2D(-320.0 * (Position / 4 + 1), 220.0 * (Position % 4)), Op.Node, /*bSetupUndoRedo=*/true);
						if (!Added) return Fail(FString::Printf(TEXT("%s: could not add trait stack %s."), *Where, *Op.Node));
						if (Added->GetName() != Op.Node)
						{
							return Fail(FString::Printf(TEXT("%s: the graph already uses the name %s."), *Where, *Op.Node));
						}
					}
					continue;
				}

				if (Op.Kind == TEXT("add_trait"))
				{
					TArray<FString>* Traits = Nodes.Find(Op.Node);
					if (!Traits) return Fail(FString::Printf(TEXT("%s: no trait stack named %s."), *Where, *Op.Node));
					const FTrait* Trait = FindTrait(Op.Trait);
					if (!Trait) return Fail(FString::Printf(TEXT("%s: no registered trait named %s; inspect with include_trait_catalog for the list."), *Where, *Op.Trait));
					if (Traits->Contains(Op.Trait))
					{
						return Fail(FString::Printf(TEXT("%s: %s already has %s; one trait per stack."), *Where, *Op.Node, *Op.Trait));
					}
					if (Trait->GetTraitMode() == ETraitMode::Base)
					{
						for (const FString& Existing : *Traits)
						{
							const FTrait* Other = FindTrait(Existing);
							if (Other && Other->GetTraitMode() == ETraitMode::Base)
							{
								return Fail(FString::Printf(TEXT("%s: %s already has the base trait %s; a stack takes one."), *Where, *Op.Node, *Existing));
							}
						}
					}
					else if (Traits->IsEmpty())
					{
						return Fail(FString::Printf(TEXT("%s: %s is additive, so add a base trait to %s first."), *Where, *Op.Trait, *Op.Node));
					}
					Traits->Add(Op.Trait);
					if (bWrite && Controller->AddTraitByName(FName(*Op.Node), FName(*Op.Trait), INDEX_NONE,
						FString(), /*bSetupUndoRedo=*/true, /*bPrintPythonCommand=*/false).IsNone())
					{
						return Fail(FString::Printf(TEXT("%s: %s would not take the trait %s."), *Where, *Op.Node, *Op.Trait));
					}
					continue;
				}

				if (Op.Kind == TEXT("set_pin") || Op.Kind == TEXT("bind_pin"))
				{
					const bool bBind = Op.Kind == TEXT("bind_pin");
					const TArray<FString>* Traits = Nodes.Find(Op.Node);
					if (!Traits) return Fail(FString::Printf(TEXT("%s: no trait stack named %s."), *Where, *Op.Node));
					if (!Traits->Contains(Op.Trait))
					{
						return Fail(FString::Printf(TEXT("%s: %s does not have the trait %s."), *Where, *Op.Node, *Op.Trait));
					}
					const FTrait* Trait = FindTrait(Op.Trait);
					const FProperty* Property = Trait ? FindTraitProperty(*Trait, Op.Pin) : nullptr;
					if (!Property) return Fail(FString::Printf(TEXT("%s: %s has no pin named %s."), *Where, *Op.Trait, *Op.Pin));
					if (IsPoseProperty(*Property))
					{
						return Fail(FString::Printf(TEXT("%s: %s.%s is a pose pin; feed it with link."), *Where, *Op.Trait, *Op.Pin));
					}
					if (bBind && !Variables.Contains(Op.Value))
					{
						return Fail(FString::Printf(TEXT("%s: no variable named %s; add_variable first."), *Where, *Op.Value));
					}
					if (!bWrite) continue;
					URigVMNode* Node = Model->FindNodeByName(FName(*Op.Node));
					URigVMPin* TraitPin = Node ? FindTraitPin(*Node, *Trait) : nullptr;
					URigVMPin* SubPin = TraitPin ? FindSubPin(*TraitPin, Op.Pin) : nullptr;
					if (!SubPin) return Fail(FString::Printf(TEXT("%s: could not find the pin %s.%s."), *Where, *Op.Trait, *Op.Pin));
					if (!bBind)
					{
						if (!Controller->SetPinDefaultValue(SubPin->GetPinPath(), Op.Value, /*bResizeArrays=*/true, /*bSetupUndoRedo=*/true))
						{
							return Fail(FString::Printf(TEXT("%s: %s is not a value %s.%s accepts."), *Where, *Op.Value, *Op.Trait, *Op.Pin));
						}
						continue;
					}
					const IUAFRigVMVariableInterface* Variable = FindVariableEntry(*Graph, Op.Value);
					if (!Variable) return Fail(FString::Printf(TEXT("%s: variable %s is not on this graph."), *Where, *Op.Value));
					const FRigVMTemplateArgumentType Argument = Variable->GetType().ToRigVMTemplateArgument();
					const URigVMVariableNode* Getter = Controller->AddVariableNode(FName(*Op.Value), Argument.CPPType.ToString(),
						Argument.CPPTypeObject, /*bIsGetter=*/true, FString(), SubPin->GetNode()->GetPosition() - FVector2D(300.0, 0.0),
						FString(), /*bSetupUndoRedo=*/true);
					URigVMPin* Value = Getter ? Getter->FindPin(TEXT("Value")) : nullptr;
					if (!Value || !Controller->AddLink(Value->GetPinPath(), SubPin->GetPinPath(), /*bSetupUndoRedo=*/true))
					{
						return Fail(FString::Printf(TEXT("%s: %s does not fit %s.%s."), *Where, *Op.Value, *Op.Trait, *Op.Pin));
					}
					continue;
				}

				if (Op.Kind == TEXT("link"))
				{
					if (!Nodes.Contains(Op.Node)) return Fail(FString::Printf(TEXT("%s: no trait stack named %s."), *Where, *Op.Node));
					const TArray<FString>* Traits = Nodes.Find(Op.To);
					if (!Traits) return Fail(FString::Printf(TEXT("%s: no trait stack named %s."), *Where, *Op.To));
					if (Op.Node == Op.To) return Fail(Where + TEXT(": a trait stack cannot feed itself."));
					if (!Traits->Contains(Op.Trait))
					{
						return Fail(FString::Printf(TEXT("%s: %s does not have the trait %s."), *Where, *Op.To, *Op.Trait));
					}
					const FTrait* Trait = FindTrait(Op.Trait);
					const FProperty* Property = Trait ? FindTraitProperty(*Trait, Op.Pin) : nullptr;
					if (!Property || !IsPoseProperty(*Property))
					{
						return Fail(FString::Printf(TEXT("%s: %s.%s is not a pose pin."), *Where, *Op.Trait, *Op.Pin));
					}
					if (!bWrite) continue;
					URigVMNode* Source = Model->FindNodeByName(FName(*Op.Node));
					URigVMNode* Target = Model->FindNodeByName(FName(*Op.To));
					URigVMPin* Result = Source ? Source->FindPin(TEXT("Result")) : nullptr;
					URigVMPin* TraitPin = Target ? FindTraitPin(*Target, *Trait) : nullptr;
					URigVMPin* SubPin = TraitPin ? FindSubPin(*TraitPin, Op.Pin) : nullptr;
					if (!Result || !SubPin || !Controller->AddLink(Result->GetPinPath(), SubPin->GetPinPath(), /*bSetupUndoRedo=*/true))
					{
						return Fail(FString::Printf(TEXT("%s: could not feed %s into %s.%s.%s."), *Where, *Op.Node, *Op.To, *Op.Trait, *Op.Pin));
					}
					continue;
				}

				if (Op.Kind == TEXT("link_output"))
				{
					if (!Nodes.Contains(Op.Node)) return Fail(FString::Printf(TEXT("%s: no trait stack named %s."), *Where, *Op.Node));
					bOutputLinked = true;
					if (!bWrite) continue;
					URigVMNode* Source = Model->FindNodeByName(FName(*Op.Node));
					URigVMPin* Result = Source ? Source->FindPin(TEXT("Result")) : nullptr;
					URigVMPin* Output = RootResultPin(*Model);
					if (!Result || !Output) return Fail(Where + TEXT(": the graph has no output pose pin."));
					Controller->BreakAllLinks(Output, /*bAsInput=*/true, /*bSetupUndoRedo=*/true);
					if (!Controller->AddLink(Result->GetPinPath(), Output->GetPinPath(), /*bSetupUndoRedo=*/true))
					{
						return Fail(FString::Printf(TEXT("%s: could not feed %s to the output pose."), *Where, *Op.Node));
					}
					continue;
				}

				if (Op.Kind == TEXT("remove_node"))
				{
					if (!Nodes.Contains(Op.Node)) return Fail(FString::Printf(TEXT("%s: no trait stack named %s."), *Where, *Op.Node));
					Nodes.Remove(Op.Node);
					if (bWrite && !Controller->RemoveNodeByName(FName(*Op.Node), /*bSetupUndoRedo=*/true))
					{
						return Fail(FString::Printf(TEXT("%s: could not remove %s."), *Where, *Op.Node));
					}
					continue;
				}

				return Fail(FString::Printf(TEXT("%s: unknown op kind."), *Where));
			}
			(void)bOutputLinked;
			return true;
		}
	}

	bool ValidateOps(const UUAFAnimGraph* Graph, const TArray<FHyperAIUAFOp>& Ops, FString& OutError)
	{
		return RunOps(const_cast<UUAFAnimGraph*>(Graph), Ops, /*bWrite=*/false, OutError);
	}

	UUAFAnimGraph* Create(const FString& Path, FString& OutError)
	{
		UPackage* Package = CreatePackage(*FPackageName::ObjectPathToPackageName(Path));
		// FactoryCreateNew is private on the UAF factory but public on UFactory, so call it through the base.
		UFactory* Factory = NewObject<UUAFAnimGraphFactory>();
		UUAFAnimGraph* Graph = Package ? Cast<UUAFAnimGraph>(Factory->FactoryCreateNew(UUAFAnimGraph::StaticClass(), Package,
			FName(*FPackageName::ObjectPathToObjectName(Path)), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn)) : nullptr;
		if (!Graph)
		{
			OutError = TEXT("Could not create the UAF animation graph.");
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Graph);
		Package->MarkPackageDirty();
		return Graph;
	}

	bool ApplyOps(UUAFAnimGraph& Graph, const TArray<FHyperAIUAFOp>& Ops, FString& OutError)
	{
		if (!RunOps(&Graph, Ops, /*bWrite=*/false, OutError))
		{
			return false;
		}
		Graph.Modify();
		if (UUAFAnimGraph_EditorData* EditorData = EditorDataOf(Graph))
		{
			EditorData->Modify();
		}
		IRigVMClientHost* Host = HostOf(Graph);
		UAnimNextController* Controller = ControllerOf(Graph);
		const bool bAutoRecompile = Host && Host->GetAutoVMRecompile();
		// One compile after every op, rather than one per op.
		if (Host) Host->SetAutoVMRecompile(false);
		// Each op records a RigVM undo action; the bracket collapses them into the plan's single undo step.
		if (Controller) Controller->OpenUndoBracket(TEXT("HyperAI: Edit UAF Animation Graph"));
		const bool bApplied = RunOps(&Graph, Ops, /*bWrite=*/true, OutError);
		if (Controller)
		{
			// Cancelling rolls the partial plan back, so a failed op leaves the graph as it was.
			if (bApplied) Controller->CloseUndoBracket(); else Controller->CancelUndoBracket();
		}
		if (Host) Host->SetAutoVMRecompile(bAutoRecompile);
		return bApplied;
	}

	bool Compile(UUAFAnimGraph& Graph, FString& OutFirstError, int32& OutErrors, int32& OutWarnings)
	{
		OutErrors = 0;
		OutWarnings = 0;
		{
			UncookedOnly::FCompilationScope Scope(&Graph);
			UncookedOnly::Compilation::RequestAssetCompilation(&Graph);
			const FCompilerResultsLog& Results = UncookedOnly::FCompilationScope::GetLogForObject(&Graph);
			OutErrors = Results.NumErrors;
			OutWarnings = Results.NumWarnings;
			for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
			{
				if (Message->GetSeverity() == EMessageSeverity::Error)
				{
					OutFirstError = Message->ToText().ToString();
					break;
				}
			}
		}
		return OutErrors == 0 && CompileStatusOf(Graph) != TEXT("errors");
	}

	TArray<FHyperAIUAFIssue> Check(UUAFAnimGraph& Graph, bool& bOutAllocatesInstance)
	{
		TArray<FHyperAIUAFIssue> Issues;
		auto Add = [&Issues](const TCHAR* Code, const bool bError, const FString& Subject, const FString& Message)
		{
			Issues.Add({Code, bError ? TEXT("error") : TEXT("warning"), Subject, Message});
		};
		const FString CompileStatus = CompileStatusOf(Graph);
		if (CompileStatus == TEXT("errors"))
		{
			Add(TEXT("compile_error"), true, Graph.GetName(), TEXT("The last compile failed; fix the graph and apply again."));
		}
		bOutAllocatesInstance = Graph.AllocateInstance().IsValid();
		if (!bOutAllocatesInstance && CompileStatus != TEXT("errors"))
		{
			Add(TEXT("no_instance"), true, Graph.GetName(),
				TEXT("The graph compiles but does not allocate a runnable instance; its root trait did not resolve, so check the output pose and the base traits."));
		}
		URigVMGraph* Model = ModelOf(Graph);
		if (!Model)
		{
			Add(TEXT("no_model"), true, Graph.GetName(), TEXT("The asset has no editable graph."));
			return Issues;
		}
		URigVMPin* Result = RootResultPin(*Model);
		if (!Result || !SourceNodeOf(*Result))
		{
			Add(TEXT("no_output_pose"), true, Graph.GetName(),
				TEXT("Nothing feeds the graph's output pose; link a trait stack to it with link_output."));
		}
		const TSet<FString> Reached = ReachableFromOutput(*Model);
		for (URigVMNode* Node : StackNodes(*Model))
		{
			const FString Name = Node->GetName();
			const TArray<URigVMPin*> TraitPins = TraitPinsOf(*Node);
			if (!Reached.Contains(Name))
			{
				Add(TEXT("orphan_stack"), false, Name, TEXT("Nothing reaches this trait stack from the output pose, so it never evaluates."));
			}
			if (TraitPins.IsEmpty())
			{
				Add(TEXT("empty_stack"), true, Name, TEXT("This trait stack has no traits, so it produces no pose."));
				continue;
			}
			int32 BaseTraits = 0;
			for (URigVMPin* TraitPin : TraitPins)
			{
				const FTrait* Trait = FindTrait(TraitNameOfPin(*TraitPin));
				if (Trait && Trait->GetTraitMode() == ETraitMode::Base) ++BaseTraits;
				const TArray<URigVMPin*>& SubPins = TraitPin->GetSubPins();
				for (int32 Index = FirstAuthoredSubPin; Index < SubPins.Num(); ++Index)
				{
					URigVMPin* SubPin = SubPins[Index];
					if (!SubPin) continue;
					const FString Subject = Name + TEXT(".") + TraitPin->GetName() + TEXT(".") + SubPin->GetName();
					if (SubPin->GetCPPTypeObject() == FAnimNextTraitHandle::StaticStruct() && !SourceNodeOf(*SubPin))
					{
						Add(TEXT("unlinked_pose"), false, Subject,
							TEXT("This pose pin has nothing feeding it, so it evaluates to the reference pose."));
						continue;
					}
					const FString Default = SubPin->GetDefaultValue();
					if (SubPin->GetCPPType().StartsWith(TEXT("TObjectPtr")) && !Default.IsEmpty() && Default != TEXT("None")
						&& !SourceNodeOf(*SubPin) && !FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Default)))
					{
						Add(TEXT("missing_asset"), true, Subject, FString::Printf(TEXT("%s does not exist."), *Default));
					}
				}
			}
			if (BaseTraits == 0)
			{
				Add(TEXT("no_base_trait"), true, Name, TEXT("Every trait stack needs one base trait under its additive traits."));
			}
			else if (BaseTraits > 1)
			{
				Add(TEXT("multiple_base_traits"), false, Name, TEXT("A trait stack evaluates one base trait; the others are ignored."));
			}
		}
		const TSet<FString> Bound = BoundVariables(*Model);
		for (const FString& Variable : VariableNamesOf(Graph))
		{
			if (!Bound.Contains(Variable))
			{
				Add(TEXT("unused_variable"), false, Variable, TEXT("This variable is declared but no pin reads it."));
			}
		}
		return Issues;
	}
}
