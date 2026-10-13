// Games by Hyper 2026.

#include "HyperAIStudioEQSGate.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Services/BTService_RunEQS.h"
#include "BehaviorTree/Tasks/BTTask_RunEQSQuery.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "DataProviders/AIDataProvider.h"
#include "DataProviders/AIDataProvider_QueryParams.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EnvironmentQuery/Contexts/EnvQueryContext_Item.h"
#include "EnvironmentQuery/Contexts/EnvQueryContext_Querier.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_ActorsOfClass.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_BlueprintBase.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_Composite.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_Cone.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_CurrentLocation.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_Donut.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_OnCircle.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_PathingGrid.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_PerceivedActors.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_ProjectedPoints.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_SimpleGrid.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_ActorBase.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_Direction.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_VectorBase.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Distance.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Dot.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_GameplayTags.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Overlap.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Pathfinding.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_PathfindingBatch.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Project.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Random.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Trace.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Volume.h"
#include "GameplayTagContainer.h"
#include "HyperAIStudioNames.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::EQS::Gate
{
	using C = FHyperAIStudioEQSContracts;

	namespace
	{
		FString Lower(const FString& Text)
		{
			return Text.TrimStartAndEnd().ToLower();
		}

		FString Number(const double Value)
		{
			FString Text = FString::Printf(TEXT("%.3f"), Value);
			while (Text.Contains(TEXT(".")) && (Text.EndsWith(TEXT("0")) || Text.EndsWith(TEXT("."))))
			{
				Text.LeftChopInline(1);
			}
			return Text == TEXT("-0") ? FString(TEXT("0")) : Text;
		}

		FString Words(const TArray<FKindInfo>& Kinds)
		{
			TArray<FString> Out;
			for (const FKindInfo& Kind : Kinds) Out.Add(Kind.Kind);
			return FString::Join(Out, TEXT(", "));
		}

		/** Properties a setting may name: the ones the details panel shows. */
		bool IsSetting(const FProperty& Property)
		{
			return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_Deprecated | CPF_EditConst);
		}

		/** The short names the op documents, for test and generator settings. */
		FString Alias(const FString& Name)
		{
			static const TMap<FString, FString> Aliases = {
				{TEXT("purpose"), TEXT("TestPurpose")},
				{TEXT("filter"), TEXT("FilterType")},
				{TEXT("min"), TEXT("FloatValueMin")},
				{TEXT("max"), TEXT("FloatValueMax")},
				{TEXT("scoring"), TEXT("ScoringEquation")},
				{TEXT("factor"), TEXT("ScoringFactor")},
				{TEXT("bool"), TEXT("BoolValue")},
				{TEXT("comment"), TEXT("TestComment")},
				{TEXT("clampmin"), TEXT("ScoreClampMin")},
				{TEXT("clampmax"), TEXT("ScoreClampMax")},
				{TEXT("name"), TEXT("OptionName")},
				{TEXT("runmode"), TEXT("EQSRequest.RunMode")}};
			const FString* Found = Aliases.Find(Lower(Name));
			return Found ? *Found : Name.TrimStartAndEnd();
		}

		FProperty* FindSetting(const UStruct& Struct, const FString& Name)
		{
			for (TFieldIterator<FProperty> It(&Struct); It; ++It)
			{
				if (!IsSetting(**It)) continue;
				if (It->GetName().Equals(Name, ESearchCase::IgnoreCase)) return *It;
#if WITH_EDITOR
				if (It->GetDisplayNameText().ToString().Replace(TEXT(" "), TEXT("")).Equals(Name, ESearchCase::IgnoreCase)) return *It;
#endif
			}
			return nullptr;
		}

		TArray<FString> SettingNames(const UStruct& Struct)
		{
			TArray<FString> Out;
			for (TFieldIterator<FProperty> It(&Struct); It; ++It)
			{
				if (IsSetting(**It)) Out.Add(It->GetName());
			}
			return Out;
		}

		/** The first context setting of a class, for the Context short name. */
		FProperty* MainContext(const UStruct& Struct)
		{
			for (TFieldIterator<FProperty> It(&Struct); It; ++It)
			{
				const FClassProperty* Class = CastField<FClassProperty>(*It);
				if (Class && IsSetting(*Class) && Class->MetaClass && Class->MetaClass->IsChildOf(UEnvQueryContext::StaticClass())) return *It;
			}
			return nullptr;
		}

		UClass* LoadClassAt(const FString& Path, UClass* Base, const TCHAR* Field, FString& OutError)
		{
			FString Reason;
			if (!FHyperAIStudioNames::IsObjectPath(Path, Field, &Reason))
			{
				OutError = Reason;
				return nullptr;
			}
			UClass* Class = FindObject<UClass>(nullptr, *Path);
			if (!Class && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)))
			{
				Class = LoadObject<UClass>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
			if (!Class || (Base && !Class->IsChildOf(Base)))
			{
				OutError = FString::Printf(TEXT("%s is not a %s class (a Blueprint's class ends in _C: /Game/AI/BP_Thing.BP_Thing_C)."), *Path, Base ? *Base->GetName() : TEXT("loaded"));
				return nullptr;
			}
			return Class;
		}

		bool ParseBool(const FString& Value, bool& Out)
		{
			const FString Word = Lower(Value);
			if (Word == TEXT("true") || Word == TEXT("1") || Word == TEXT("yes") || Word == TEXT("on")) { Out = true; return true; }
			if (Word == TEXT("false") || Word == TEXT("0") || Word == TEXT("no") || Word == TEXT("off")) { Out = false; return true; }
			return false;
		}

		bool ParseEnum(const UEnum& Enum, const FString& Value, int64& Out, FString& OutError)
		{
			const FString Want = Value.TrimStartAndEnd().Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT(""));
			TArray<FString> Names;
			const int32 Count = Enum.ContainsExistingMax() ? Enum.NumEnums() - 1 : Enum.NumEnums();
			for (int32 Index = 0; Index < Count; ++Index)
			{
#if WITH_EDITOR
				if (Enum.HasMetaData(TEXT("Hidden"), Index)) continue;
#endif
				const FString Short = Enum.GetNameStringByIndex(Index);
				Names.Add(Short);
				const FString Display = Enum.GetDisplayNameTextByIndex(Index).ToString().Replace(TEXT(" "), TEXT(""));
				if (Short.Replace(TEXT("_"), TEXT("")).Equals(Want, ESearchCase::IgnoreCase) || Display.Replace(TEXT("_"), TEXT("")).Equals(Want, ESearchCase::IgnoreCase))
				{
					Out = Enum.GetValueByIndex(Index);
					return true;
				}
			}
			OutError = FString::Printf(TEXT("%s is not one of %s."), *Value, *FString::Join(Names, TEXT(", ")));
			return false;
		}

		FString EnumText(const UEnum& Enum, const int64 Value)
		{
			const int32 Index = Enum.GetIndexByValue(Value);
			return Index == INDEX_NONE ? FString::Printf(TEXT("%lld"), Value) : Enum.GetNameStringByIndex(Index);
		}

		bool ParseVector(const FString& Text, FVector& Out)
		{
			TArray<FString> Parts;
			Text.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() != 3) return false;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const FString Part = Parts[Axis].TrimStartAndEnd();
				if (!Part.IsNumeric()) return false;
				Out[Axis] = FCString::Atod(*Part);
			}
			return true;
		}

		bool ParseTagQuery(const FString& Text, FGameplayTagQuery& Out, FString& OutError)
		{
			FString Mode = TEXT("any");
			FString List = Text.TrimStartAndEnd();
			if (List.Contains(TEXT(":")))
			{
				List.Split(TEXT(":"), &Mode, &List);
				Mode = Lower(Mode);
			}
			if (Mode != TEXT("any") && Mode != TEXT("all") && Mode != TEXT("none"))
			{
				OutError = TEXT("A tag query is any:A.B,C.D, all:A.B,C.D or none:A.B.");
				return false;
			}
			TArray<FString> Names;
			List.ParseIntoArray(Names, TEXT(","));
			FGameplayTagContainer Tags;
			for (const FString& Name : Names)
			{
				const FString Clean = Name.TrimStartAndEnd();
				if (!FHyperAIStudioNames::IsName(Clean, TEXT("tag"), &OutError)) return false;
				const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Clean), /*ErrorIfNotFound=*/false);
				if (!Tag.IsValid())
				{
					OutError = FString::Printf(TEXT("%s is not a gameplay tag of this project; add it first (Epic's GameplayTagsToolset AddTag)."), *Clean);
					return false;
				}
				Tags.AddTag(Tag);
			}
			if (Tags.IsEmpty())
			{
				OutError = TEXT("A tag query names at least one tag.");
				return false;
			}
			Out = Mode == TEXT("all") ? FGameplayTagQuery::MakeQuery_MatchAllTags(Tags)
				: Mode == TEXT("none") ? FGameplayTagQuery::MakeQuery_MatchNoTags(Tags) : FGameplayTagQuery::MakeQuery_MatchAnyTags(Tags);
			return true;
		}

		/** A value onto one property of Owner (or of a struct inside it). */
		bool SetValue(UObject& Owner, FProperty& Property, void* ValuePtr, const FString& InValue, FString& OutError)
		{
			const FString Value = InValue.TrimStartAndEnd();
			if (FStructProperty* Struct = CastField<FStructProperty>(&Property))
			{
				if (Struct->Struct->IsChildOf(FAIDataProviderValue::StaticStruct()))
				{
					FAIDataProviderValue& Provider = *static_cast<FAIDataProviderValue*>(ValuePtr);
					if (Value.StartsWith(TEXT("param:"), ESearchCase::IgnoreCase))
					{
						const FString ParamName = Value.Mid(6).TrimStartAndEnd();
						if (ParamName.IsEmpty() || !FHyperAIStudioNames::IsName(ParamName, TEXT("param"), &OutError))
						{
							if (OutError.IsEmpty()) OutError = TEXT("param: needs a name, such as param:Range.");
							return false;
						}
						const FName Field = Struct->Struct->IsChildOf(FAIDataProviderFloatValue::StaticStruct()) ? FName(TEXT("FloatValue"))
							: Struct->Struct->IsChildOf(FAIDataProviderIntValue::StaticStruct()) ? FName(TEXT("IntValue"))
							: Struct->Struct->IsChildOf(FAIDataProviderBoolValue::StaticStruct()) ? FName(TEXT("BoolValue")) : NAME_None;
						if (Field.IsNone())
						{
							OutError = FString::Printf(TEXT("%s cannot be a query param."), *Property.GetName());
							return false;
						}
						UAIDataProvider_QueryParams* Binding = NewObject<UAIDataProvider_QueryParams>(&Owner, NAME_None, RF_Transactional);
						Binding->ParamName = FName(*ParamName);
						Provider.DataBinding = Binding;
						Provider.DataField = Field;
						return true;
					}
					FProperty* Default = Struct->Struct->FindPropertyByName(TEXT("DefaultValue"));
					if (!Default)
					{
						OutError = FString::Printf(TEXT("%s takes no plain value."), *Property.GetName());
						return false;
					}
					if (!SetValue(Owner, *Default, Default->ContainerPtrToValuePtr<void>(ValuePtr), Value, OutError)) return false;
					Provider.DataBinding = nullptr;
					Provider.DataField = NAME_None;
					return true;
				}
				if (Struct->Struct == TBaseStructure<FVector>::Get())
				{
					FVector Vector;
					if (!ParseVector(Value, Vector))
					{
						OutError = FString::Printf(TEXT("%s takes x,y,z."), *Property.GetName());
						return false;
					}
					*static_cast<FVector*>(ValuePtr) = Vector;
					return true;
				}
				if (Struct->Struct == FGameplayTagQuery::StaticStruct())
				{
					return ParseTagQuery(Value, *static_cast<FGameplayTagQuery*>(ValuePtr), OutError);
				}
			}
			if (FClassProperty* ClassProperty = CastField<FClassProperty>(&Property))
			{
				UClass* Meta = ClassProperty->MetaClass;
				UClass* Class = nullptr;
				const FString Word = Lower(Value);
				if (Word == TEXT("none") || Word.IsEmpty())
				{
					Class = nullptr;
				}
				else if (Meta && Meta->IsChildOf(UEnvQueryContext::StaticClass()) && !Value.Contains(TEXT("/")))
				{
					for (const FKindInfo& Context : Contexts())
					{
						if (Word == Context.Kind) Class = Context.Class;
					}
					if (!Class)
					{
						OutError = FString::Printf(TEXT("%s takes a context: %s, or a context class (/Game/AI/EQC_Player.EQC_Player_C)."), *Property.GetName(), *Words(Contexts()));
						return false;
					}
				}
				else if (Value.Contains(TEXT("/")))
				{
					Class = LoadClassAt(Value, Meta, *Property.GetName(), OutError);
					if (!Class) return false;
				}
				else
				{
					if (!FHyperAIStudioNames::IsName(Value, *Property.GetName(), &OutError)) return false;
					Class = FindFirstObject<UClass>(*Value, EFindFirstObjectOptions::NativeFirst);
					if (!Class || (Meta && !Class->IsChildOf(Meta)))
					{
						OutError = FString::Printf(TEXT("%s is not a %s class; give its path."), *Value, Meta ? *Meta->GetName() : TEXT("loaded"));
						return false;
					}
				}
				ClassProperty->SetObjectPropertyValue(ValuePtr, Class);
				return true;
			}
			if (FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
			{
				UObject* Found = nullptr;
				if (Lower(Value) != TEXT("none") && !Value.IsEmpty())
				{
					if (!FHyperAIStudioNames::IsObjectPath(Value, *Property.GetName(), &OutError)) return false;
					const FString Path = Value.Contains(TEXT(".")) ? Value : Value + TEXT(".") + FPackageName::GetShortName(Value);
					Found = FindObject<UObject>(nullptr, *Path);
					if (!Found && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)))
					{
						Found = LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
					}
					if (!Found || !Found->IsA(Object->PropertyClass))
					{
						OutError = FString::Printf(TEXT("%s is not a %s."), *Value, *Object->PropertyClass->GetName());
						return false;
					}
				}
				Object->SetObjectPropertyValue(ValuePtr, Found);
				return true;
			}
			if (FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
			{
				bool bValue = false;
				if (!ParseBool(Value, bValue))
				{
					OutError = FString::Printf(TEXT("%s takes true or false."), *Property.GetName());
					return false;
				}
				Bool->SetPropertyValue(ValuePtr, bValue);
				return true;
			}
			if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
			{
				int64 Parsed = 0;
				if (!ParseEnum(*EnumProperty->GetEnum(), Value, Parsed, OutError))
				{
					OutError = Property.GetName() + TEXT(": ") + OutError;
					return false;
				}
				EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, Parsed);
				return true;
			}
			if (FByteProperty* Byte = CastField<FByteProperty>(&Property); Byte && Byte->Enum)
			{
				int64 Parsed = 0;
				if (!ParseEnum(*Byte->Enum, Value, Parsed, OutError))
				{
					OutError = Property.GetName() + TEXT(": ") + OutError;
					return false;
				}
				Byte->SetPropertyValue(ValuePtr, static_cast<uint8>(Parsed));
				return true;
			}
			if (FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
			{
				if (!Value.IsNumeric())
				{
					OutError = FString::Printf(TEXT("%s takes a number, not %s."), *Property.GetName(), *Value);
					return false;
				}
				if (Numeric->IsFloatingPoint())
				{
					Numeric->SetFloatingPointPropertyValue(ValuePtr, FCString::Atod(*Value));
				}
				else
				{
					if (Value.Contains(TEXT(".")))
					{
						OutError = FString::Printf(TEXT("%s takes a whole number."), *Property.GetName());
						return false;
					}
					Numeric->SetIntPropertyValue(ValuePtr, FCString::Atoi64(*Value));
				}
				return true;
			}
			if (!FHyperAIStudioNames::CanImportText(Property, Value, *Property.GetName(), &OutError)) return false;
			if (!Property.ImportText_Direct(*Value, ValuePtr, &Owner, PPF_None))
			{
				OutError = FString::Printf(TEXT("%s does not take %s."), *Property.GetName(), *Value);
				return false;
			}
			return true;
		}

		FString ItemTypeText(const UClass* ItemType)
		{
			if (!ItemType) return TEXT("none");
			if (ItemType->IsChildOf(UEnvQueryItemType_ActorBase::StaticClass())) return TEXT("actor");
			if (ItemType->IsChildOf(UEnvQueryItemType_Direction::StaticClass())) return TEXT("direction");
			if (ItemType->IsChildOf(UEnvQueryItemType_VectorBase::StaticClass())) return TEXT("point");
			return ItemType->GetName();
		}

		FString PurposeText(const UEnvQueryTest& Test)
		{
			switch (Test.TestPurpose)
			{
			case EEnvTestPurpose::Filter: return TEXT("filter");
			case EEnvTestPurpose::Score: return TEXT("score");
			default: return TEXT("filter_and_score");
			}
		}

		FString Describe(const UEnvQueryNode& Node)
		{
			const FString Title = Node.GetDescriptionTitle().ToString();
			const FString Details = Node.GetDescriptionDetails().ToString().Replace(TEXT("\n"), TEXT("; "));
			return Details.IsEmpty() ? Title : Title + TEXT(": ") + Details;
		}

		int32 ResolveIndex(const int32 Index, const int32 Count)
		{
			return Index < 0 ? Count - 1 : Index;
		}

		UEnvQueryGenerator* MakeGenerator(UObject& Outer, UClass& Class, const FString& Params, const TArray<FString>& Children, FString& OutError)
		{
			UEnvQueryGenerator* Generator = NewObject<UEnvQueryGenerator>(&Outer, &Class, NAME_None, RF_Transactional);
			if (!ApplyParams(*Generator, Params, OutError)) return nullptr;
			if (UEnvQueryGenerator_Composite* Composite = Cast<UEnvQueryGenerator_Composite>(Generator))
			{
				if (Children.IsEmpty() || Children.Num() > C::MaxChildren)
				{
					OutError = FString::Printf(TEXT("composite needs 1 to %d Children, each \"kind\" or \"kind: Name=Value\"."), C::MaxChildren);
					return nullptr;
				}
				for (const FString& Child : Children)
				{
					FString Kind = Child.TrimStartAndEnd();
					FString ChildParams;
					if (Kind.Contains(TEXT(":")) && !Kind.StartsWith(TEXT("/")))
					{
						Child.Split(TEXT(":"), &Kind, &ChildParams);
						Kind.TrimStartAndEndInline();
					}
					UClass* ChildClass = ResolveGenerator(Kind, OutError);
					if (!ChildClass) return nullptr;
					if (ChildClass->IsChildOf(UEnvQueryGenerator_Composite::StaticClass()))
					{
						OutError = TEXT("A composite's children are not composites.");
						return nullptr;
					}
					UEnvQueryGenerator* Made = MakeGenerator(*Composite, *ChildClass, ChildParams, {}, OutError);
					if (!Made)
					{
						OutError = FString::Printf(TEXT("child %s: %s"), *Kind, *OutError);
						return nullptr;
					}
					Composite->Generators.Add(Made);
				}
				Composite->VerifyItemTypes();
			}
			else if (!Children.IsEmpty())
			{
				OutError = TEXT("Children are a composite's only.");
				return nullptr;
			}
			return Generator;
		}

		UBehaviorTreeGraph* EnsureGraph(UBehaviorTree& Tree)
		{
			if (UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Tree.BTGraph)) return Graph;
			// What the Behavior Tree editor does when it opens a tree that has no graph yet.
			Tree.Modify();
			const TSubclassOf<UEdGraphSchema> Schema = GetDefault<UBehaviorTreeGraph>()->Schema;
			Tree.BTGraph = FBlueprintEditorUtils::CreateNewGraph(&Tree, TEXT("Behavior Tree"), UBehaviorTreeGraph::StaticClass(), Schema);
			UBehaviorTreeGraph* Graph = CastChecked<UBehaviorTreeGraph>(Tree.BTGraph);
			Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
			Graph->OnCreated();
			Graph->Initialize();
			return Graph;
		}

		UBehaviorTreeGraphNode_Root* RootOf(UBehaviorTreeGraph& Graph)
		{
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (UBehaviorTreeGraphNode_Root* Root = Cast<UBehaviorTreeGraphNode_Root>(Node)) return Root;
			}
			return nullptr;
		}

		FString NodeName(const UAIGraphNode& Node)
		{
			const UBTNode* Instance = Cast<UBTNode>(Node.NodeInstance);
			return Instance ? Instance->GetNodeName() : Node.GetName();
		}

		/** The composite Parent names: root (the root's child, or the root when it has none), a node name, or #index. */
		UBehaviorTreeGraphNode* FindParent(UBehaviorTreeGraph& Graph, const FString& Parent, FString& OutError)
		{
			const FString Want = Parent.TrimStartAndEnd();
			if (Want.IsEmpty() || Want.Equals(TEXT("root"), ESearchCase::IgnoreCase))
			{
				UBehaviorTreeGraphNode_Root* Root = RootOf(Graph);
				if (!Root)
				{
					OutError = TEXT("The tree's graph has no root node.");
					return nullptr;
				}
				UEdGraphPin* Out = Root->GetOutputPin();
				if (Out && Out->LinkedTo.Num() > 0 && Out->LinkedTo[0])
				{
					return Cast<UBehaviorTreeGraphNode>(Out->LinkedTo[0]->GetOwningNode());
				}
				return Root;
			}
			TArray<UBehaviorTreeGraphNode*> Matches;
			TArray<FString> Names;
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				UBehaviorTreeGraphNode_Composite* Composite = Cast<UBehaviorTreeGraphNode_Composite>(Node);
				const UBTCompositeNode* Instance = Composite ? Cast<UBTCompositeNode>(Composite->NodeInstance) : nullptr;
				if (!Instance) continue;
				const FString Index = FString::Printf(TEXT("#%d"), Instance->GetExecutionIndex());
				Names.Add(Index + TEXT(" ") + Instance->GetNodeName());
				if (Want == Index || Instance->GetNodeName().Equals(Want, ESearchCase::IgnoreCase)) Matches.Add(Composite);
			}
			if (Matches.Num() == 1) return Matches[0];
			OutError = Matches.IsEmpty()
				? FString::Printf(TEXT("The tree has no composite %s; it has %s (or say root)."), *Want, Names.IsEmpty() ? TEXT("none") : *FString::Join(Names, TEXT(", ")))
				: FString::Printf(TEXT("%d composites are named %s; name one by #index: %s."), Matches.Num(), *Want, *FString::Join(Names, TEXT(", ")));
			return nullptr;
		}

		/** Places a new graph node under Parent, to the right of its other children, and links it. */
		template <typename TNode>
		TNode* SpawnChild(UBehaviorTreeGraph& Graph, UBehaviorTreeGraphNode& Parent, UClass& InstanceClass, FString& OutError)
		{
			Graph.Modify();
			Parent.Modify();
			UEdGraphPin* Out = Parent.GetOutputPin();
			int32 RightMost = Parent.NodePosX - 300;
			if (Out)
			{
				for (const UEdGraphPin* Linked : Out->LinkedTo)
				{
					if (Linked && Linked->GetOwningNode()) RightMost = FMath::Max(RightMost, Linked->GetOwningNode()->NodePosX);
				}
			}
			FGraphNodeCreator<TNode> Creator(Graph);
			TNode* Node = Creator.CreateNode(/*bSelectNewNode=*/false);
			Node->ClassData = FGraphNodeClassData(&InstanceClass, FString());
			Creator.Finalize();
			Node->SetFlags(RF_Transactional);
			Node->NodePosX = RightMost + 300;
			Node->NodePosY = Parent.NodePosY + 150;
			UEdGraphPin* In = Node->GetInputPin();
			if (!Out || !In || !Graph.GetSchema()->TryCreateConnection(Out, In))
			{
				OutError = FString::Printf(TEXT("%s cannot go under %s."), *InstanceClass.GetName(), *NodeName(Parent));
				return nullptr;
			}
			return Node;
		}

		FBlackboardKeySelector* KeySelectorOf(UObject& Node)
		{
			FStructProperty* Property = FindFProperty<FStructProperty>(Node.GetClass(), TEXT("BlackboardKey"));
			return Property && Property->Struct == FBlackboardKeySelector::StaticStruct() ? Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(&Node) : nullptr;
		}

		/** A Vector key, or an object key holding actors: what Run EQS writes. */
		bool CheckKey(const UBlackboardData& Blackboard, const FString& Key, FString& OutError)
		{
			TArray<FString> Fit;
			const FBlackboardEntry* Found = nullptr;
			for (const UBlackboardData* Each = &Blackboard; Each; Each = Each->Parent)
			{
				for (const FBlackboardEntry& Entry : Each->Keys)
				{
					const bool bVector = Entry.KeyType && Entry.KeyType->IsA<UBlackboardKeyType_Vector>();
					const UBlackboardKeyType_Object* Object = Cast<UBlackboardKeyType_Object>(Entry.KeyType);
					const bool bActor = Object && (!Object->BaseClass || Object->BaseClass->IsChildOf(AActor::StaticClass()) || AActor::StaticClass()->IsChildOf(Object->BaseClass));
					if (bVector || bActor) Fit.Add(Entry.EntryName.ToString());
					if (Entry.EntryName.ToString().Equals(Key.TrimStartAndEnd(), ESearchCase::IgnoreCase))
					{
						Found = &Entry;
						if (!bVector && !bActor)
						{
							OutError = FString::Printf(TEXT("Blackboard key %s is not a Vector or Actor key; Run EQS writes a location or an actor. Keys that fit: %s."), *Key,
								Fit.IsEmpty() ? TEXT("none (add one with hyper_gameplay_ai_apply_plan add_blackboard_key or in the Blackboard editor)") : *FString::Join(Fit, TEXT(", ")));
							return false;
						}
					}
				}
			}
			if (!Found)
			{
				OutError = FString::Printf(TEXT("The tree's Blackboard %s has no key %s; keys that fit: %s."), *Blackboard.GetName(), *Key,
					Fit.IsEmpty() ? TEXT("none (add a Vector or Actor key first)") : *FString::Join(Fit, TEXT(", ")));
				return false;
			}
			return true;
		}

		/** Points an EQS task or service at the query and the key. */
		bool ConfigureEQSNode(UBTNode& Node, FEQSParametrizedQueryExecutionRequest& EQS, UEnvQuery& Query, UBlackboardData* Blackboard, const FHyperAIEQSOp& Op, FString& OutError)
		{
			Node.Modify();
			EQS.QueryTemplate = &Query;
			EQS.bUseBBKeyForQueryTemplate = false;
			EQS.QueryConfig.Reset();
			Query.CollectQueryParams(Node, EQS.QueryConfig);
			if (FBlackboardKeySelector* Key = KeySelectorOf(Node))
			{
				Key->SelectedKeyName = FName(*Op.BlackboardKey.TrimStartAndEnd());
				if (Blackboard) Key->ResolveSelectedKey(*Blackboard);
			}
			return ApplyParams(Node, Op.Params, OutError);
		}
	}

	const TArray<FKindInfo>& Generators()
	{
		static const TArray<FKindInfo> Kinds = {
			{TEXT("grid"), UEnvQueryGenerator_SimpleGrid::StaticClass(), TEXT("Points on a square grid around a context (GridSize half-width, SpaceBetween).")},
			{TEXT("circle"), UEnvQueryGenerator_OnCircle::StaticClass(), TEXT("Points on a circle around a context (CircleRadius, SpaceBetween or NumberOfPoints, an arc).")},
			{TEXT("donut"), UEnvQueryGenerator_Donut::StaticClass(), TEXT("Points on rings between InnerRadius and OuterRadius around a context.")},
			{TEXT("cone"), UEnvQueryGenerator_Cone::StaticClass(), TEXT("Points in a cone in front of a context (ConeDegrees, Range, AngleStep).")},
			{TEXT("pathing_grid"), UEnvQueryGenerator_PathingGrid::StaticClass(), TEXT("Grid points the querier can walk to on the navmesh.")},
			{TEXT("actors_of_class"), UEnvQueryGenerator_ActorsOfClass::StaticClass(), TEXT("Every actor of SearchedActorClass, optionally within SearchRadius of a context.")},
			{TEXT("current_location"), UEnvQueryGenerator_CurrentLocation::StaticClass(), TEXT("The context's own location (one item per context).")},
			{TEXT("composite"), UEnvQueryGenerator_Composite::StaticClass(), TEXT("The items of several generators together (Children).")},
			{TEXT("perceived_actors"), UEnvQueryGenerator_PerceivedActors::StaticClass(), TEXT("Actors the querier's AI Perception has sensed.")}};
		return Kinds;
	}

	const TArray<FKindInfo>& Tests()
	{
		static const TArray<FKindInfo> Kinds = {
			{TEXT("distance"), UEnvQueryTest_Distance::StaticClass(), TEXT("Distance from each item to a context (3D, 2D, along Z).")},
			{TEXT("dot"), UEnvQueryTest_Dot::StaticClass(), TEXT("Dot product of two directions (LineA, LineB): is the item in front of, behind, facing...")},
			{TEXT("trace"), UEnvQueryTest_Trace::StaticClass(), TEXT("A line or shape trace between each item and a context: can it see, is it hidden.")},
			{TEXT("pathfinding"), UEnvQueryTest_Pathfinding::StaticClass(), TEXT("Whether, or how far, a path exists between each item and a context (needs a navmesh).")},
			{TEXT("pathfinding_batch"), UEnvQueryTest_PathfindingBatch::StaticClass(), TEXT("Path cost to each item in one batched search (needs a navmesh).")},
			{TEXT("overlap"), UEnvQueryTest_Overlap::StaticClass(), TEXT("Whether a shape at each item overlaps geometry.")},
			{TEXT("gameplay_tags"), UEnvQueryTest_GameplayTags::StaticClass(), TEXT("Whether each actor item's gameplay tags match TagQueryToMatch.")},
			{TEXT("project"), UEnvQueryTest_Project::StaticClass(), TEXT("Projects each point onto the navmesh or geometry, dropping those that miss.")},
			{TEXT("random"), UEnvQueryTest_Random::StaticClass(), TEXT("A random score, to break ties.")},
			{TEXT("volume"), UEnvQueryTest_Volume::StaticClass(), TEXT("Whether each item is inside a volume context's volumes.")}};
		return Kinds;
	}

	const TArray<FKindInfo>& Contexts()
	{
		static const TArray<FKindInfo> Kinds = {
			{TEXT("querier"), UEnvQueryContext_Querier::StaticClass(), TEXT("The actor running the query (the AI's pawn).")},
			{TEXT("item"), UEnvQueryContext_Item::StaticClass(), TEXT("The item being tested.")}};
		return Kinds;
	}

	FString KindOf(const UClass* Class)
	{
		if (!Class) return TEXT("none");
		for (const TArray<FKindInfo>* Table : {&Generators(), &Tests(), &Contexts()})
		{
			for (const FKindInfo& Kind : *Table)
			{
				if (Kind.Class == Class) return Kind.Kind;
			}
		}
		return Class->GetPathName();
	}

	UClass* ResolveGenerator(const FString& Type, FString& OutError)
	{
		const FString Word = Lower(Type);
		for (const FKindInfo& Kind : Generators())
		{
			if (Word == Kind.Kind) return Kind.Class;
		}
		if (Type.Contains(TEXT("/")))
		{
			UClass* Class = LoadClassAt(Type.TrimStartAndEnd(), UEnvQueryGenerator::StaticClass(), TEXT("Type"), OutError);
			if (Class && Class->HasAnyClassFlags(CLASS_Abstract))
			{
				OutError = FString::Printf(TEXT("%s is abstract."), *Type);
				return nullptr;
			}
			return Class;
		}
		OutError = FString::Printf(TEXT("%s is not a generator; generators are %s, or a Blueprint generator class (/Game/AI/EQG_Cover.EQG_Cover_C)."),
			Type.IsEmpty() ? TEXT("(empty Type)") : *Type, *Words(Generators()));
		return nullptr;
	}

	UClass* ResolveTest(const FString& Type, FString& OutError)
	{
		const FString Word = Lower(Type);
		for (const FKindInfo& Kind : Tests())
		{
			if (Word == Kind.Kind) return Kind.Class;
		}
		OutError = FString::Printf(TEXT("%s is not a test; tests are %s."), Type.IsEmpty() ? TEXT("(empty Type)") : *Type, *Words(Tests()));
		return nullptr;
	}

	bool SplitParams(const FString& Text, TArray<TPair<FString, FString>>& Out, FString& OutError)
	{
		Out.Reset();
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(";"));
		for (const FString& Part : Parts)
		{
			const FString Trimmed = Part.TrimStartAndEnd();
			if (Trimmed.IsEmpty()) continue;
			FString Name;
			FString Value;
			if (!Trimmed.Split(TEXT("="), &Name, &Value) || Name.TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("\"%s\" is not Name=Value; settings are \"Name=Value; Name=Value\"."), *Trimmed);
				return false;
			}
			if (!FHyperAIStudioNames::IsName(Name.TrimStartAndEnd(), TEXT("Params"), &OutError)) return false;
			Out.Emplace(Name.TrimStartAndEnd(), Value.TrimStartAndEnd());
		}
		return true;
	}

	bool SetParam(UObject& Node, const FString& InName, const FString& Value, FString& OutError)
	{
		// Clamp is the two clamp types at once, as the editor's score curve shows them.
		if (InName.Equals(TEXT("Clamp"), ESearchCase::IgnoreCase))
		{
			return SetParam(Node, TEXT("ClampMinType"), Value, OutError) && SetParam(Node, TEXT("ClampMaxType"), Value, OutError);
		}
		FString Name = Alias(InName);
		if (Name.Equals(TEXT("Context"), ESearchCase::IgnoreCase))
		{
			FProperty* Context = MainContext(*Node.GetClass());
			if (!Context)
			{
				OutError = FString::Printf(TEXT("%s has no single context; name its context setting (%s)."), *KindOf(Node.GetClass()), *FString::Join(SettingNames(*Node.GetClass()), TEXT(", ")));
				return false;
			}
			Name = Context->GetName();
		}
		TArray<FString> Path;
		Name.ParseIntoArray(Path, TEXT("."));
		const UStruct* Struct = Node.GetClass();
		void* Container = &Node;
		for (int32 Index = 0; Index < Path.Num(); ++Index)
		{
			FProperty* Property = FindSetting(*Struct, Path[Index]);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("%s has no setting %s; it has %s."), Index == 0 ? *KindOf(Node.GetClass()) : *Path[Index - 1], *Path[Index],
					*FString::Join(SettingNames(*Struct), TEXT(", ")));
				return false;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
			if (Index == Path.Num() - 1)
			{
				if (!SetValue(Node, *Property, ValuePtr, Value, OutError))
				{
					if (!OutError.Contains(Property->GetName())) OutError = Property->GetName() + TEXT(": ") + OutError;
					return false;
				}
				// A tag test made before tag queries reads its old fields unless told otherwise.
				if (Property->GetName() == TEXT("TagQueryToMatch"))
				{
					if (FBoolProperty* Updated = FindFProperty<FBoolProperty>(Node.GetClass(), TEXT("bUpdatedToUseQuery")))
					{
						Updated->SetPropertyValue_InContainer(&Node, true);
					}
				}
				return true;
			}
			FStructProperty* Inner = CastField<FStructProperty>(Property);
			if (!Inner || Inner->Struct->IsChildOf(FAIDataProviderValue::StaticStruct()))
			{
				OutError = FString::Printf(TEXT("%s has no settings inside it."), *Property->GetName());
				return false;
			}
			Struct = Inner->Struct;
			Container = ValuePtr;
		}
		OutError = TEXT("A setting needs a name.");
		return false;
	}

	bool ApplyParams(UObject& Node, const FString& Params, FString& OutError)
	{
		TArray<TPair<FString, FString>> Pairs;
		if (!SplitParams(Params, Pairs, OutError)) return false;
		for (const TPair<FString, FString>& Pair : Pairs)
		{
			if (!SetParam(Node, Pair.Key, Pair.Value, OutError)) return false;
		}
		return true;
	}

	FString ValueText(const FProperty& Property, const void* Value, const UObject* Owner)
	{
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
		{
			if (Struct->Struct->IsChildOf(FAIDataProviderValue::StaticStruct()))
			{
				const FAIDataProviderValue& Provider = *static_cast<const FAIDataProviderValue*>(Value);
				if (const UAIDataProvider_QueryParams* Binding = Cast<UAIDataProvider_QueryParams>(Provider.DataBinding))
				{
					return TEXT("param:") + Binding->ParamName.ToString();
				}
				if (const FProperty* Default = Struct->Struct->FindPropertyByName(TEXT("DefaultValue")))
				{
					return ValueText(*Default, Default->ContainerPtrToValuePtr<void>(Value), Owner);
				}
			}
			if (Struct->Struct == TBaseStructure<FVector>::Get()) return VectorText(*static_cast<const FVector*>(Value));
		}
		if (const FClassProperty* Class = CastField<FClassProperty>(&Property))
		{
			const UClass* Held = Cast<UClass>(Class->GetObjectPropertyValue(Value));
			return Held ? KindOf(Held) : FString(TEXT("none"));
		}
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
		{
			const UObject* Held = Object->GetObjectPropertyValue(Value);
			return Held ? Held->GetPathName() : FString(TEXT("none"));
		}
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property)) return Bool->GetPropertyValue(Value) ? TEXT("true") : TEXT("false");
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(&Property)) return EnumText(*Enum->GetEnum(), Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value));
		if (const FByteProperty* Byte = CastField<FByteProperty>(&Property); Byte && Byte->Enum) return EnumText(*Byte->Enum, Byte->GetPropertyValue(Value));
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
		{
			return Numeric->IsFloatingPoint() ? Number(Numeric->GetFloatingPointPropertyValue(Value)) : FString::Printf(TEXT("%lld"), Numeric->GetSignedIntPropertyValue(Value));
		}
		FString Text;
		// The value is its own delta, so values equal to the type's zero are still written.
		Property.ExportTextItem_Direct(Text, Value, Value, const_cast<UObject*>(Owner), PPF_None);
		return Text;
	}

	TArray<FString> DescribeSettings(const UObject& Node, const bool bOnlyChanged)
	{
		TArray<FString> Out;
		const UObject* Defaults = Node.GetClass()->GetDefaultObject();
		for (TFieldIterator<FProperty> It(Node.GetClass()); It; ++It)
		{
			if (!IsSetting(**It)) continue;
			const FString Text = ValueText(**It, It->ContainerPtrToValuePtr<void>(&Node), &Node);
			if (bOnlyChanged && Defaults && Text == ValueText(**It, It->ContainerPtrToValuePtr<void>(Defaults), Defaults)) continue;
			Out.Add(FString::Printf(TEXT("%s=%s"), *It->GetName(), *Text));
		}
		return Out;
	}

	TArray<FString> DescribeKindSettings(UClass* Class)
	{
		TArray<FString> Out;
		const UObject* Defaults = Class ? Class->GetDefaultObject() : nullptr;
		if (!Defaults) return Out;
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			if (!IsSetting(**It)) continue;
			FString Type = It->GetCPPType();
			if (const FStructProperty* Struct = CastField<FStructProperty>(*It); Struct && Struct->Struct->IsChildOf(FAIDataProviderValue::StaticStruct()))
			{
				Type = Struct->Struct->IsChildOf(FAIDataProviderBoolValue::StaticStruct()) ? TEXT("bool or param:Name")
					: Struct->Struct->IsChildOf(FAIDataProviderIntValue::StaticStruct()) ? TEXT("integer or param:Name") : TEXT("number or param:Name");
			}
			else if (const FClassProperty* ClassProperty = CastField<FClassProperty>(*It); ClassProperty && ClassProperty->MetaClass && ClassProperty->MetaClass->IsChildOf(UEnvQueryContext::StaticClass()))
			{
				Type = TEXT("context");
			}
			else if (const FByteProperty* Byte = CastField<FByteProperty>(*It); Byte && Byte->Enum)
			{
				Type = Byte->Enum->GetName();
			}
			Out.Add(FString::Printf(TEXT("%s: %s, default %s"), *It->GetName(), *Type, *ValueText(**It, It->ContainerPtrToValuePtr<void>(Defaults), Defaults)));
		}
		return Out;
	}

	TArray<FHyperAIEQSOptionView> Describe(const UEnvQuery& Query)
	{
		TArray<FHyperAIEQSOptionView> Out;
		for (int32 OptionIndex = 0; OptionIndex < Query.GetOptions().Num(); ++OptionIndex)
		{
			const UEnvQueryOption* Option = Query.GetOptions()[OptionIndex];
			FHyperAIEQSOptionView& View = Out.AddDefaulted_GetRef();
			View.Index = OptionIndex;
			if (!Option || !Option->Generator)
			{
				View.Generator = TEXT("none");
				View.Description = TEXT("An option without a generator (the query skips it).");
				continue;
			}
			View.Generator = KindOf(Option->Generator->GetClass());
			View.ItemType = ItemTypeText(Option->Generator->ItemType);
			View.Description = Describe(*Option->Generator);
			View.Settings = DescribeSettings(*Option->Generator, true);
			if (const UEnvQueryGenerator_Composite* Composite = Cast<UEnvQueryGenerator_Composite>(Option->Generator))
			{
				for (const UEnvQueryGenerator* Child : Composite->Generators)
				{
					if (!Child) continue;
					const TArray<FString> Settings = DescribeSettings(*Child, true);
					View.Children.Add(Settings.IsEmpty() ? KindOf(Child->GetClass()) : KindOf(Child->GetClass()) + TEXT(": ") + FString::Join(Settings, TEXT("; ")));
				}
			}
			for (int32 TestIndex = 0; TestIndex < Option->Tests.Num(); ++TestIndex)
			{
				const UEnvQueryTest* Test = Option->Tests[TestIndex];
				if (!Test) continue;
				FHyperAIEQSTestView& TestView = View.Tests.AddDefaulted_GetRef();
				TestView.Index = TestIndex;
				TestView.Kind = KindOf(Test->GetClass());
				TestView.Purpose = PurposeText(*Test);
				TestView.Description = Describe(*Test);
				TestView.Settings = DescribeSettings(*Test, true);
			}
		}
		return Out;
	}

	TArray<FString> QueryParams(const UEnvQuery& Query)
	{
		TArray<FAIDynamicParam> Params;
		Query.CollectQueryParams(*const_cast<UEnvQuery*>(&Query), Params);
		TArray<FString> Out;
		for (const FAIDynamicParam& Param : Params)
		{
			const TCHAR* Type = Param.ParamType == EAIParamType::Int ? TEXT("integer") : Param.ParamType == EAIParamType::Bool ? TEXT("bool") : TEXT("number");
			Out.AddUnique(FString::Printf(TEXT("%s (%s)"), *Param.ParamName.ToString(), Type));
		}
		return Out;
	}

	TArray<FString> Warn(const UEnvQuery& Query)
	{
		TArray<FString> Out;
		if (Query.GetOptions().IsEmpty()) Out.Add(TEXT("The query has no options, so it never returns an item; add one with option.add."));
		for (int32 OptionIndex = 0; OptionIndex < Query.GetOptions().Num(); ++OptionIndex)
		{
			const UEnvQueryOption* Option = Query.GetOptions()[OptionIndex];
			if (!Option || !Option->Generator) continue;
			const UClass* ItemType = Option->Generator->ItemType;
			bool bNavigation = Option->Generator->IsA<UEnvQueryGenerator_PathingGrid>();
			if (const UEnvQueryGenerator_ProjectedPoints* Projected = Cast<UEnvQueryGenerator_ProjectedPoints>(Option->Generator))
			{
				bNavigation |= Projected->ProjectionData.TraceMode == EEnvQueryTrace::Navigation || Projected->ProjectionData.TraceMode == EEnvQueryTrace::NavigationOverLedges;
			}
			for (int32 TestIndex = 0; TestIndex < Option->Tests.Num(); ++TestIndex)
			{
				const UEnvQueryTest* Test = Option->Tests[TestIndex];
				if (!Test) continue;
				bNavigation |= Test->IsA<UEnvQueryTest_Pathfinding>();
				if (ItemType && Test->ValidItemType && !ItemType->IsChildOf(Test->ValidItemType))
				{
					Out.Add(FString::Printf(TEXT("Option %d test %d (%s) cannot test %s items; it is skipped when the query runs."), OptionIndex, TestIndex, *KindOf(Test->GetClass()),
						*ItemTypeText(ItemType)));
				}
				if (Test->TestPurpose != EEnvTestPurpose::Score && Test->GetWorkOnFloatValues() && Test->FilterType == EEnvTestFilterType::Range
					&& Test->FloatValueMin.DataBinding == nullptr && Test->FloatValueMax.DataBinding == nullptr && Test->FloatValueMin.DefaultValue > Test->FloatValueMax.DefaultValue)
				{
					Out.Add(FString::Printf(TEXT("Option %d test %d filters a range whose Min is above its Max; nothing passes."), OptionIndex, TestIndex));
				}
			}
			if (bNavigation)
			{
				Out.Add(FString::Printf(TEXT("Option %d uses the navmesh (projection or pathfinding): where the level has none, its points are dropped. For a test without a navmesh set ProjectionData.TraceMode=None."), OptionIndex));
			}
			if (Option->Tests.IsEmpty() && Query.GetOptions().Num() > OptionIndex + 1)
			{
				Out.Add(FString::Printf(TEXT("Option %d has no tests, so it always wins when it makes items and the options after it never run."), OptionIndex));
			}
		}
		return Out;
	}

	bool ApplyQueryOps(UEnvQuery& Query, const TArray<FHyperAIEQSOp>& Ops, TArray<FString>& OutNotes, FString& OutError)
	{
		for (int32 OpIndex = 0; OpIndex < Ops.Num(); ++OpIndex)
		{
			const FHyperAIEQSOp& Op = Ops[OpIndex];
			const FString Kind = Lower(Op.Kind);
			const FString Prefix = FString::Printf(TEXT("Operation %d (%s): "), OpIndex, *Kind);
			if (Kind == TEXT("create") || Kind.StartsWith(TEXT("bt."))) continue;
			TArray<TObjectPtr<UEnvQueryOption>>& Options = Query.GetOptionsMutable();
			if (Kind == TEXT("option.add"))
			{
				if (Options.Num() >= C::MaxOptions)
				{
					OutError = Prefix + FString::Printf(TEXT("a query has at most %d options."), C::MaxOptions);
					return false;
				}
				UClass* Class = ResolveGenerator(Op.Type, OutError);
				if (!Class)
				{
					OutError = Prefix + OutError;
					return false;
				}
				Query.Modify();
				UEnvQueryOption* Option = NewObject<UEnvQueryOption>(&Query, NAME_None, RF_Transactional);
				UEnvQueryGenerator* Generator = MakeGenerator(Query, *Class, Op.Params, Op.Children, OutError);
				if (!Generator)
				{
					OutError = Prefix + OutError;
					return false;
				}
				Option->Generator = Generator;
				Options.Add(Option);
				OutNotes.Add(FString::Printf(TEXT("Option %d: %s."), Options.Num() - 1, *Describe(*Generator)));
				continue;
			}
			const int32 OptionIndex = ResolveIndex(Op.Option, Options.Num());
			if (!Options.IsValidIndex(OptionIndex) || !Options[OptionIndex] || !Options[OptionIndex]->Generator)
			{
				OutError = Prefix + FString::Printf(TEXT("there is no option %d; the query has %d (from 0, -1 for the last)."), Op.Option, Options.Num());
				return false;
			}
			UEnvQueryOption& Option = *Options[OptionIndex];
			if (Kind == TEXT("option.set"))
			{
				Option.Generator->Modify();
				if (!ApplyParams(*Option.Generator, Op.Params, OutError))
				{
					OutError = Prefix + OutError;
					return false;
				}
				if (!Op.Children.IsEmpty())
				{
					OutError = Prefix + TEXT("option.set changes settings only; to change a composite's children, remove the option and add it again.");
					return false;
				}
				OutNotes.Add(FString::Printf(TEXT("Option %d: %s."), OptionIndex, *Describe(*Option.Generator)));
			}
			else if (Kind == TEXT("option.remove"))
			{
				Query.Modify();
				Options.RemoveAt(OptionIndex);
				OutNotes.Add(FString::Printf(TEXT("Option %d removed; %d left."), OptionIndex, Options.Num()));
			}
			else if (Kind == TEXT("test.add"))
			{
				if (Option.Tests.Num() >= C::MaxTestsPerOption)
				{
					OutError = Prefix + FString::Printf(TEXT("an option has at most %d tests."), C::MaxTestsPerOption);
					return false;
				}
				UClass* Class = ResolveTest(Op.Type, OutError);
				if (!Class)
				{
					OutError = Prefix + OutError;
					return false;
				}
				Option.Modify();
				UEnvQueryTest* Test = NewObject<UEnvQueryTest>(&Query, Class, NAME_None, RF_Transactional);
				if (!ApplyParams(*Test, Op.Params, OutError))
				{
					OutError = Prefix + OutError;
					return false;
				}
				Test->TestOrder = Option.Tests.Num();
				Option.Tests.Add(Test);
				OutNotes.Add(FString::Printf(TEXT("Option %d test %d (%s): %s."), OptionIndex, Option.Tests.Num() - 1, *PurposeText(*Test), *Describe(*Test)));
			}
			else if (Kind == TEXT("test.set") || Kind == TEXT("test.remove"))
			{
				const int32 TestIndex = ResolveIndex(Op.Test, Option.Tests.Num());
				if (!Option.Tests.IsValidIndex(TestIndex) || !Option.Tests[TestIndex])
				{
					OutError = Prefix + FString::Printf(TEXT("option %d has no test %d; it has %d (from 0, -1 for the last)."), OptionIndex, Op.Test, Option.Tests.Num());
					return false;
				}
				if (Kind == TEXT("test.remove"))
				{
					Option.Modify();
					Option.Tests.RemoveAt(TestIndex);
					for (int32 Index = 0; Index < Option.Tests.Num(); ++Index)
					{
						if (Option.Tests[Index] && Option.Tests[Index]->TestOrder != Index)
						{
							Option.Tests[Index]->Modify();
							Option.Tests[Index]->TestOrder = Index;
						}
					}
					OutNotes.Add(FString::Printf(TEXT("Option %d test %d removed."), OptionIndex, TestIndex));
				}
				else
				{
					UEnvQueryTest& Test = *Option.Tests[TestIndex];
					Test.Modify();
					if (!ApplyParams(Test, Op.Params, OutError))
					{
						OutError = Prefix + OutError;
						return false;
					}
					OutNotes.Add(FString::Printf(TEXT("Option %d test %d (%s): %s."), OptionIndex, TestIndex, *PurposeText(Test), *Describe(Test)));
				}
			}
			else
			{
				OutError = Prefix + TEXT("the kinds are create, option.add, option.set, option.remove, test.add, test.set, test.remove, bt.add_composite, bt.run_eqs_task and bt.run_eqs_service.");
				return false;
			}
		}
		return true;
	}

	UBehaviorTree* LoadTree(const FString& InPath, FString& OutError)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			OutError = TEXT("bt.* ops need BehaviorTree, the tree's path (/Game/AI/BT_Guard).");
			return nullptr;
		}
		if (!FHyperAIStudioNames::IsObjectPath(Path, TEXT("BehaviorTree"), &OutError)) return nullptr;
		if (!Path.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(TEXT("%s is not project content: a plan changes Behavior Trees under /Game only."), *Path);
			return nullptr;
		}
		if (!Path.Contains(TEXT("."))) Path += TEXT(".") + FPackageName::GetShortName(Path);
		UBehaviorTree* Tree = FindObject<UBehaviorTree>(nullptr, *Path);
		if (!Tree && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)))
		{
			Tree = LoadObject<UBehaviorTree>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (!Tree || !IsValid(Tree))
		{
			OutError = FString::Printf(TEXT("%s is not a Behavior Tree."), *Path);
			return nullptr;
		}
		return Tree;
	}

	bool CheckTreeOps(const TArray<FHyperAIEQSOp>& Ops, FString& OutError)
	{
		TMap<UBehaviorTree*, TArray<FString>> Added;
		for (int32 OpIndex = 0; OpIndex < Ops.Num(); ++OpIndex)
		{
			const FHyperAIEQSOp& Op = Ops[OpIndex];
			const FString Kind = Lower(Op.Kind);
			if (!Kind.StartsWith(TEXT("bt."))) continue;
			const FString Prefix = FString::Printf(TEXT("Operation %d (%s): "), OpIndex, *Kind);
			if (Kind != TEXT("bt.add_composite") && Kind != TEXT("bt.run_eqs_task") && Kind != TEXT("bt.run_eqs_service"))
			{
				OutError = Prefix + TEXT("the tree kinds are bt.add_composite, bt.run_eqs_task and bt.run_eqs_service.");
				return false;
			}
			UBehaviorTree* Tree = LoadTree(Op.BehaviorTree, OutError);
			if (!Tree)
			{
				OutError = Prefix + OutError;
				return false;
			}
			TArray<FString>& Names = Added.FindOrAdd(Tree);
			const FString Parent = Op.Parent.TrimStartAndEnd();
			const bool bRoot = Parent.IsEmpty() || Parent.Equals(TEXT("root"), ESearchCase::IgnoreCase);
			const bool bNamedNew = Names.ContainsByPredicate([&Parent](const FString& Name) { return Name.Equals(Parent, ESearchCase::IgnoreCase); });
			if (UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Tree->BTGraph); Graph && !bNamedNew)
			{
				UBehaviorTreeGraphNode* Found = FindParent(*Graph, Parent, OutError);
				if (!Found)
				{
					OutError = Prefix + OutError;
					return false;
				}
				if (Found->IsA<UBehaviorTreeGraphNode_Root>() && Kind != TEXT("bt.add_composite") && Names.IsEmpty())
				{
					OutError = Prefix + TEXT("the tree's root has nothing under it yet; add a selector or sequence first (bt.add_composite with Parent root).");
					return false;
				}
			}
			else if (!Tree->BTGraph && !bNamedNew)
			{
				if (Tree->RootNode && !bRoot)
				{
					// Built in code and never opened: the graph is made from the runtime tree, so names are those of its composites.
					bool bFound = false;
					TArray<UBTCompositeNode*> Stack = {Tree->RootNode};
					while (!Stack.IsEmpty() && !bFound)
					{
						UBTCompositeNode* Each = Stack.Pop();
						if (!Each) continue;
						bFound = Each->GetNodeName().Equals(Parent, ESearchCase::IgnoreCase) || Parent == FString::Printf(TEXT("#%d"), Each->GetExecutionIndex());
						for (const FBTCompositeChild& Child : Each->Children) Stack.Add(Child.ChildComposite);
					}
					if (!bFound)
					{
						OutError = Prefix + FString::Printf(TEXT("the tree has no composite %s (hyper_eqs_inspect with BehaviorTree lists them)."), *Parent);
						return false;
					}
				}
				else if (!Tree->RootNode && Kind != TEXT("bt.add_composite"))
				{
					OutError = Prefix + TEXT("the tree is empty; add a selector or sequence first (bt.add_composite with Parent root).");
					return false;
				}
			}
			if (Kind == TEXT("bt.add_composite"))
			{
				const FString Type = Lower(Op.Type);
				if (Type != TEXT("selector") && Type != TEXT("sequence"))
				{
					OutError = Prefix + TEXT("Type is selector or sequence.");
					return false;
				}
				FString NodeName = Type == TEXT("selector") ? TEXT("Selector") : TEXT("Sequence");
				TArray<TPair<FString, FString>> Pairs;
				if (!SplitParams(Op.Params, Pairs, OutError))
				{
					OutError = Prefix + OutError;
					return false;
				}
				for (const TPair<FString, FString>& Pair : Pairs)
				{
					if (Pair.Key.Equals(TEXT("NodeName"), ESearchCase::IgnoreCase)) NodeName = Pair.Value;
				}
				Names.Add(NodeName);
				continue;
			}
			if (Op.BlackboardKey.TrimStartAndEnd().IsEmpty())
			{
				OutError = Prefix + TEXT("give BlackboardKey, the Vector or Actor key the result goes to.");
				return false;
			}
			if (!Tree->BlackboardAsset)
			{
				OutError = Prefix + FString::Printf(TEXT("%s has no Blackboard; set one first (hyper_gameplay_ai_apply_plan set_behavior_tree_blackboard, or in the tree's details)."), *Tree->GetName());
				return false;
			}
			if (!CheckKey(*Tree->BlackboardAsset, Op.BlackboardKey, OutError))
			{
				OutError = Prefix + OutError;
				return false;
			}
			// The node's settings, checked on a throwaway node of the same class.
			UBTNode* Probe = NewObject<UBTNode>(GetTransientPackage(), Kind == TEXT("bt.run_eqs_task") ? UBTTask_RunEQSQuery::StaticClass() : UBTService_RunEQS::StaticClass());
			const bool bParams = ApplyParams(*Probe, Op.Params, OutError);
			Probe->MarkAsGarbage();
			if (!bParams)
			{
				OutError = Prefix + OutError;
				return false;
			}
		}
		return true;
	}

	bool ApplyTreeOp(UBehaviorTree& Tree, const FHyperAIEQSOp& Op, UEnvQuery& Query, FString& OutNote, FString& OutError)
	{
		Tree.Modify();
		UBehaviorTreeGraph* Graph = EnsureGraph(Tree);
		Graph->Modify();
		UBehaviorTreeGraphNode* Parent = FindParent(*Graph, Op.Parent, OutError);
		if (!Parent) return false;
		const FString Kind = Lower(Op.Kind);
		FString Placed;
		if (Kind == TEXT("bt.add_composite"))
		{
			UClass* Class = Lower(Op.Type) == TEXT("selector") ? UBTComposite_Selector::StaticClass() : UBTComposite_Sequence::StaticClass();
			UBehaviorTreeGraphNode_Composite* Node = SpawnChild<UBehaviorTreeGraphNode_Composite>(*Graph, *Parent, *Class, OutError);
			if (!Node) return false;
			if (UObject* Instance = Node->NodeInstance)
			{
				Instance->Modify();
				if (!ApplyParams(*Instance, Op.Params, OutError)) return false;
			}
			Placed = NodeName(*Node);
		}
		else if (Kind == TEXT("bt.run_eqs_task"))
		{
			if (Parent->IsA<UBehaviorTreeGraphNode_Root>())
			{
				OutError = TEXT("A task does not go straight under the root; add a selector or sequence first.");
				return false;
			}
			UBehaviorTreeGraphNode_Task* Node = SpawnChild<UBehaviorTreeGraphNode_Task>(*Graph, *Parent, *UBTTask_RunEQSQuery::StaticClass(), OutError);
			UBTTask_RunEQSQuery* Task = Node ? Cast<UBTTask_RunEQSQuery>(Node->NodeInstance) : nullptr;
			if (!Task)
			{
				if (OutError.IsEmpty()) OutError = TEXT("The Run EQS Query task could not be made.");
				return false;
			}
			if (!ConfigureEQSNode(*Task, Task->EQSRequest, Query, Tree.BlackboardAsset, Op, OutError)) return false;
			Placed = NodeName(*Node);
		}
		else
		{
			if (Parent->IsA<UBehaviorTreeGraphNode_Root>())
			{
				OutError = TEXT("A service goes on a selector or sequence; add one first.");
				return false;
			}
			UBehaviorTreeGraphNode_Service* Node = NewObject<UBehaviorTreeGraphNode_Service>(Graph, NAME_None, RF_Transactional);
			Node->ClassData = FGraphNodeClassData(UBTService_RunEQS::StaticClass(), FString());
			// AddSubNode places it (PostPlacedNewNode makes its instance) and rebuilds the tree, inside its own nested step.
			Parent->AddSubNode(Node, Graph);
			UBTService_RunEQS* Service = Cast<UBTService_RunEQS>(Node->NodeInstance);
			if (!Service)
			{
				OutError = TEXT("The Run EQS service could not be made.");
				return false;
			}
			FStructProperty* Request = FindFProperty<FStructProperty>(UBTService_RunEQS::StaticClass(), TEXT("EQSRequest"));
			if (!Request || Request->Struct != FEQSParametrizedQueryExecutionRequest::StaticStruct())
			{
				OutError = TEXT("This engine's Run EQS service has no EQSRequest.");
				return false;
			}
			if (!ConfigureEQSNode(*Service, *Request->ContainerPtrToValuePtr<FEQSParametrizedQueryExecutionRequest>(Service), Query, Tree.BlackboardAsset, Op, OutError)) return false;
			Placed = NodeName(*Node);
		}
		Graph->UpdateAsset();
		Tree.MarkPackageDirty();
		OutNote = FString::Printf(TEXT("%s: %s under %s"), *Tree.GetPathName(), *Placed, *NodeName(*Parent));
		return true;
	}

	void DescribeTree(UBehaviorTree& Tree, TArray<FString>& OutComposites, TArray<FString>& OutKeys, TArray<FString>& OutEQSNodes)
	{
		TArray<TPair<UBTCompositeNode*, int32>> Stack;
		if (Tree.RootNode) Stack.Emplace(Tree.RootNode, 0);
		while (!Stack.IsEmpty())
		{
			const TPair<UBTCompositeNode*, int32> Each = Stack.Pop();
			UBTCompositeNode* Composite = Each.Key;
			if (!Composite) continue;
			OutComposites.Add(FString::Printf(TEXT("#%d %s (%s), %d children, depth %d"), Composite->GetExecutionIndex(), *Composite->GetNodeName(), *Composite->GetClass()->GetName(),
				Composite->Children.Num(), Each.Value));
			for (const UBTService* Service : Composite->Services)
			{
				if (const UBTService_RunEQS* RunEQS = Cast<UBTService_RunEQS>(Service))
				{
					OutEQSNodes.Add(FString::Printf(TEXT("#%d %s on %s"), RunEQS->GetExecutionIndex(), *RunEQS->GetNodeName(), *Composite->GetNodeName()));
				}
			}
			for (int32 Index = Composite->Children.Num() - 1; Index >= 0; --Index)
			{
				const FBTCompositeChild& Child = Composite->Children[Index];
				if (Child.ChildComposite) Stack.Emplace(Child.ChildComposite, Each.Value + 1);
				if (const UBTTask_RunEQSQuery* Task = Cast<UBTTask_RunEQSQuery>(Child.ChildTask))
				{
					OutEQSNodes.Add(FString::Printf(TEXT("#%d %s under %s: %s"), Task->GetExecutionIndex(), *Task->GetNodeName(), *Composite->GetNodeName(),
						Task->EQSRequest.QueryTemplate ? *Task->EQSRequest.QueryTemplate->GetPathName() : TEXT("no query")));
				}
			}
		}
		for (const UBlackboardData* Each = Tree.BlackboardAsset; Each; Each = Each->Parent)
		{
			for (const FBlackboardEntry& Entry : Each->Keys)
			{
				OutKeys.Add(FString::Printf(TEXT("%s: %s"), *Entry.EntryName.ToString(), Entry.KeyType ? *Entry.KeyType->GetClass()->GetName().Replace(TEXT("BlackboardKeyType_"), TEXT("")) : TEXT("none")));
			}
		}
	}

	FString VectorText(const FVector& Value)
	{
		return FString::Printf(TEXT("%s,%s,%s"), *Number(Value.X), *Number(Value.Y), *Number(Value.Z));
	}
}
