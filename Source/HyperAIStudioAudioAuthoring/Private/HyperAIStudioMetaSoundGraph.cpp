// Games by Hyper 2026.

#include "HyperAIStudioMetaSoundGraph.h"

#include "HyperAIStudioNames.h"
#include "HyperAIStudioSoundCueGraph.h"
#include "HyperAIStudioSourceControl.h"
#include "Interfaces/MetasoundFrontendSourceInterface.h"
#include "Interfaces/MetasoundOutputFormatInterfaces.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/PackageName.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendDataTypeRegistry.h"
#include "MetasoundFrontendNodeClassRegistry.h"
#include "MetasoundFrontendQuery.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundSource.h"

namespace HyperAIStudio::AudioAuthoring::Graph
{
	namespace
	{
		using C = FHyperAIStudioAudioAuthoringContracts;

		/** The interface members a plan names through node "graph", by the names people use for them. */
		FName GraphAlias(const FString& Pin)
		{
			using namespace Metasound;
			const FString Key = Pin.TrimStartAndEnd().Replace(TEXT(" "), TEXT("")).ToLower();
			if (Key == TEXT("onplay") || Key == TEXT("play")) return Frontend::SourceInterface::Inputs::OnPlay;
			if (Key == TEXT("onfinished") || Key == TEXT("finished")) return Frontend::SourceOneShotInterface::Outputs::OnFinished;
			if (Key == TEXT("outmono") || Key == TEXT("out") || Key == TEXT("mono") || Key == TEXT("audio")) return Engine::OutputFormatMonoInterface::Outputs::MonoOut;
			if (Key == TEXT("outleft") || Key == TEXT("left")) return Engine::OutputFormatStereoInterface::Outputs::LeftOut;
			if (Key == TEXT("outright") || Key == TEXT("right")) return Engine::OutputFormatStereoInterface::Outputs::RightOut;
			return FName(*Pin.TrimStartAndEnd());
		}

		FString JoinText(const TArray<FText>& Texts, const TCHAR* Separator)
		{
			return FString::JoinBy(Texts, Separator, [](const FText& Text) { return Text.ToString(); });
		}

		bool IsGraphNode(const FString& Id)
		{
			return Id.TrimStartAndEnd().Equals(TEXT("graph"), ESearchCase::IgnoreCase);
		}

		TArray<FString> EnumValues(const FName DataType)
		{
			TArray<FString> Out;
			const TSharedPtr<const Metasound::Frontend::IEnumDataTypeInterface> Enum = Metasound::Frontend::IDataTypeRegistry::Get().GetEnumInterfaceForDataType(DataType);
			if (Enum.IsValid())
			{
				for (const Metasound::Frontend::IEnumDataTypeInterface::FGenericInt32Entry& Entry : Enum->GetAllEntries())
				{
					FString Name = Entry.Name.ToString();
					int32 Colon = INDEX_NONE;
					if (Name.FindLastChar(TEXT(':'), Colon))
					{
						Name = Name.Mid(Colon + 1);
					}
					Out.Add(FString::Printf(TEXT("%s=%d"), *Name, Entry.Value));
				}
			}
			return Out;
		}

		/** One term of a search against a class: in its name, display name, category, keywords or description. */
		bool Matches(const Metasound::Frontend::FMetaSoundClassInfo& Info, const FString& Haystack, const TArray<FString>& Terms)
		{
			for (const FString& Term : Terms)
			{
				if (!Haystack.Contains(Term))
				{
					return false;
				}
			}
			return true;
		}

		bool HasPinType(const TArray<FMetaSoundClassVertexInfo>& Pins, const FString& Type)
		{
			return Type.IsEmpty() || Pins.ContainsByPredicate([&Type](const FMetaSoundClassVertexInfo& Pin) { return Pin.TypeName.ToString().Equals(Type, ESearchCase::IgnoreCase); });
		}

		FHyperAIMetaSoundPin MakePin(const FMetaSoundClassVertexInfo& Vertex)
		{
			FHyperAIMetaSoundPin Out;
			Out.Name = Vertex.Name.ToString();
			Out.Type = Vertex.TypeName.ToString();
			Out.bConstructor = Vertex.AccessType == EMetasoundFrontendVertexAccessType::Value;
			return Out;
		}

		struct FPinSpec
		{
			FName Type;
			bool bConstructor = false;
		};

		/** A plan node's pins, from its registered class or, for a node already there, from the document. */
		struct FNodePins
		{
			TMap<FName, FPinSpec> Inputs;
			TMap<FName, FPinSpec> Outputs;
		};

		bool ParseBool(const FString& Text, bool& bOut)
		{
			if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text == TEXT("1")) { bOut = true; return true; }
			if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Text == TEXT("0")) { bOut = false; return true; }
			return false;
		}

		bool ParseEnum(const FName DataType, const FString& Text, int32& OutValue)
		{
			const TSharedPtr<const Metasound::Frontend::IEnumDataTypeInterface> Enum = Metasound::Frontend::IDataTypeRegistry::Get().GetEnumInterfaceForDataType(DataType);
			if (!Enum.IsValid())
			{
				return false;
			}
			int32 Number = 0;
			const bool bNumber = FDefaultValueHelper::ParseInt(Text, Number);
			for (const Metasound::Frontend::IEnumDataTypeInterface::FGenericInt32Entry& Entry : Enum->GetAllEntries())
			{
				FString Name = Entry.Name.ToString();
				int32 Colon = INDEX_NONE;
				const FString Short = Name.FindLastChar(TEXT(':'), Colon) ? Name.Mid(Colon + 1) : Name;
				if ((bNumber && Entry.Value == Number) || Text.Equals(Name, ESearchCase::IgnoreCase) || Text.Equals(Short, ESearchCase::IgnoreCase)
#if WITH_EDITOR
					|| Text.Equals(Entry.DisplayName.ToString(), ESearchCase::IgnoreCase)
#endif
					)
				{
					OutValue = Entry.Value;
					return true;
				}
			}
			return false;
		}

		bool ParseElement(const FName DataType, const Metasound::ELiteralType Kind, const FString& Text, const bool bEnum, TFunctionRef<void(bool)> AddBool,
			TFunctionRef<void(int32)> AddInt, TFunctionRef<void(float)> AddFloat, TFunctionRef<void(const FString&)> AddString, TFunctionRef<void(UObject*)> AddObject,
			FString& OutError)
		{
			using Metasound::ELiteralType;
			switch (Kind)
			{
			case ELiteralType::Boolean:
			case ELiteralType::BooleanArray:
			{
				bool bValue = false;
				if (!ParseBool(Text, bValue)) { OutError = FString::Printf(TEXT("%s takes true or false, not %s."), *DataType.ToString(), *Text); return false; }
				AddBool(bValue);
				return true;
			}
			case ELiteralType::Integer:
			case ELiteralType::IntegerArray:
			{
				int32 Value = 0;
				if (bEnum ? !ParseEnum(DataType, Text, Value) : !FDefaultValueHelper::ParseInt(Text, Value))
				{
					OutError = bEnum ? FString::Printf(TEXT("%s is one of %s, not %s."), *DataType.ToString(), *FString::Join(EnumValues(DataType), TEXT(", ")), *Text)
						: FString::Printf(TEXT("%s takes a whole number, not %s."), *DataType.ToString(), *Text);
					return false;
				}
				AddInt(Value);
				return true;
			}
			case ELiteralType::Float:
			case ELiteralType::FloatArray:
			{
				float Value = 0.f;
				if (!FDefaultValueHelper::ParseFloat(Text, Value) || !FMath::IsFinite(Value)) { OutError = FString::Printf(TEXT("%s takes a number, not %s."), *DataType.ToString(), *Text); return false; }
				AddFloat(Value);
				return true;
			}
			case ELiteralType::String:
			case ELiteralType::StringArray:
				AddString(Text);
				return true;
			case ELiteralType::UObjectProxy:
			case ELiteralType::UObjectProxyArray:
			{
				if (Text.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					AddObject(nullptr);
					return true;
				}
				FString Reason;
				UObject* Object = CueGraph::LoadAsset(Text, *UObject::StaticClass(), &Reason);
				const FName Element = Kind == ELiteralType::UObjectProxyArray ? FName(*DataType.ToString().Replace(TEXT(":Array"), TEXT(""))) : DataType;
				if (!Object || !Metasound::Frontend::IDataTypeRegistry::Get().IsValidUObjectForDataType(Element, Object))
				{
					OutError = Object ? FString::Printf(TEXT("%s is a %s, which a %s pin does not take."), *Text, *Object->GetClass()->GetName(), *DataType.ToString())
						: (Reason.IsEmpty() ? FString::Printf(TEXT("%s is not in the project."), *Text) : Reason);
					return false;
				}
				AddObject(Object);
				return true;
			}
			default:
				OutError = FString::Printf(TEXT("A %s pin takes no value: wire it instead."), *DataType.ToString());
				return false;
			}
		}
	}

	bool ParseClassName(const FString& Text, FMetasoundFrontendClassName& OutName)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		int32 First = INDEX_NONE;
		if (!Trimmed.FindChar(TEXT('.'), First) || First == 0)
		{
			return false;
		}
		const FString Namespace = Trimmed.Left(First);
		FString Rest = Trimmed.Mid(First + 1);
		FString Variant;
		int32 Last = INDEX_NONE;
		// The variant follows the last dot, except in an array variant such as WaveAsset:Array, which holds none.
		if (Rest.FindLastChar(TEXT('.'), Last))
		{
			Variant = Rest.Mid(Last + 1);
			Rest = Rest.Left(Last);
		}
		if (Rest.IsEmpty())
		{
			return false;
		}
		OutName = FMetasoundFrontendClassName(FName(*Namespace), FName(*Rest), FName(*Variant));
		return true;
	}

	FString ClassText(const FMetasoundFrontendClassName& Name)
	{
		return Name.Variant.IsNone() ? FString::Printf(TEXT("%s.%s"), *Name.Namespace.ToString(), *Name.Name.ToString())
			: FString::Printf(TEXT("%s.%s.%s"), *Name.Namespace.ToString(), *Name.Name.ToString(), *Name.Variant.ToString());
	}

	bool FindClass(const FMetasoundFrontendClassName& Name, const int32 MajorVersion, FMetasoundFrontendClass& OutClass)
	{
		using namespace Metasound::Frontend;
		FMetaSoundClassInfo Info;
		const bool bFound = MajorVersion > 0 ? ISearchEngine::Get().FindRegisteredClass(Name, MajorVersion, Info) : ISearchEngine::Get().FindClassWithHighestVersion(Name, Info);
		return bFound && INodeClassRegistry::GetChecked().FindFrontendClassFromRegistered(Info.ToRegistryKey(), OutClass);
	}

	FHyperAIMetaSoundNodeClass Describe(const FMetasoundFrontendClass& Class, const bool bFull)
	{
		FHyperAIMetaSoundNodeClass Out;
		const FMetasoundFrontendClassMetadata& Metadata = Class.Metadata;
		Out.Class = ClassText(Metadata.GetClassName());
		Out.MajorVersion = Metadata.GetVersion().Major;
		Out.MinorVersion = Metadata.GetVersion().Minor;
#if WITH_EDITORONLY_DATA
		Out.DisplayName = Metadata.GetDisplayName().ToString();
		Out.Category = JoinText(Metadata.GetCategoryHierarchy(), TEXT(" > "));
		Out.Description = Metadata.GetDescription().ToString().Left(bFull ? 1000 : 200);
		Out.bDeprecated = Metadata.GetIsDeprecated();
#endif
		for (const FMetasoundFrontendClassInput& Input : Class.GetDefaultInterface().Inputs)
		{
			FHyperAIMetaSoundPin& Pin = Out.Inputs.AddDefaulted_GetRef();
			Pin.Name = Input.Name.ToString();
			Pin.Type = Input.TypeName.ToString();
			Pin.bConstructor = Input.AccessType == EMetasoundFrontendVertexAccessType::Value;
			if (bFull)
			{
				if (const FMetasoundFrontendLiteral* Default = Input.FindConstDefault(Metasound::Frontend::DefaultPageID))
				{
					if (Default->IsValid() && Default->GetType() != EMetasoundFrontendLiteralType::None)
					{
						Pin.Default = Default->ToString();
					}
				}
#if WITH_EDITORONLY_DATA
				Pin.Description = Input.Metadata.GetDescription().ToString().Left(300);
#endif
				Pin.EnumValues = EnumValues(Input.TypeName);
			}
		}
		for (const FMetasoundFrontendClassOutput& Output : Class.GetDefaultInterface().Outputs)
		{
			FHyperAIMetaSoundPin& Pin = Out.Outputs.AddDefaulted_GetRef();
			Pin.Name = Output.Name.ToString();
			Pin.Type = Output.TypeName.ToString();
			Pin.bConstructor = Output.AccessType == EMetasoundFrontendVertexAccessType::Value;
#if WITH_EDITORONLY_DATA
			if (bFull)
			{
				Pin.Description = Output.Metadata.GetDescription().ToString().Left(300);
			}
#endif
		}
		return Out;
	}

	bool MakeLiteral(const FName DataType, const FString& InText, FMetasoundFrontendLiteral& OutLiteral, FString& OutError)
	{
		using namespace Metasound;
		using namespace Metasound::Frontend;
		FDataTypeRegistryInfo Info;
		if (!IDataTypeRegistry::Get().GetDataTypeInfo(DataType, Info))
		{
			OutError = FString::Printf(TEXT("%s is not a registered MetaSound data type (hyper_metasound_node_describe with bListDataTypes lists them)."), *DataType.ToString());
			return false;
		}
		const FString Text = InText.TrimStartAndEnd();
		if (Text.IsEmpty())
		{
			OutLiteral = FMetasoundFrontendLiteral();
			OutLiteral.SetFromLiteral(IDataTypeRegistry::Get().CreateDefaultLiteral(DataType));
			return true;
		}
		const ELiteralType Kind = Info.PreferredLiteralType;
		const bool bArray = Kind == ELiteralType::BooleanArray || Kind == ELiteralType::IntegerArray || Kind == ELiteralType::FloatArray
			|| Kind == ELiteralType::StringArray || Kind == ELiteralType::UObjectProxyArray || Kind == ELiteralType::NoneArray;
		TArray<FString> Elements;
		if (bArray)
		{
			FString Inner = Text;
			Inner.RemoveFromStart(TEXT("["));
			Inner.RemoveFromEnd(TEXT("]"));
			Inner.ParseIntoArray(Elements, TEXT(","), /*bCullEmpty=*/true);
			for (FString& Element : Elements)
			{
				Element.TrimStartAndEndInline();
				Element.TrimCharInline(TEXT('"'), nullptr);
			}
		}
		else
		{
			Elements.Add(Text);
		}
		TArray<bool> Bools;
		TArray<int32> Ints;
		TArray<float> Floats;
		TArray<FString> Strings;
		TArray<UObject*> Objects;
		for (const FString& Element : Elements)
		{
			if (!ParseElement(DataType, Kind, Element, Info.bIsEnum, [&Bools](bool B) { Bools.Add(B); }, [&Ints](int32 I) { Ints.Add(I); },
				[&Floats](float F) { Floats.Add(F); }, [&Strings](const FString& S) { Strings.Add(S); }, [&Objects](UObject* O) { Objects.Add(O); }, OutError))
			{
				return false;
			}
		}
		OutLiteral = FMetasoundFrontendLiteral();
		switch (Kind)
		{
		case ELiteralType::Boolean: OutLiteral.Set(Bools[0]); break;
		case ELiteralType::Integer: OutLiteral.Set(Ints[0]); break;
		case ELiteralType::Float: OutLiteral.Set(Floats[0]); break;
		case ELiteralType::String: OutLiteral.Set(Strings[0]); break;
		case ELiteralType::UObjectProxy: OutLiteral.Set(Objects[0]); break;
		case ELiteralType::BooleanArray: OutLiteral.Set(Bools); break;
		case ELiteralType::IntegerArray: OutLiteral.Set(Ints); break;
		case ELiteralType::FloatArray: OutLiteral.Set(Floats); break;
		case ELiteralType::StringArray: OutLiteral.Set(Strings); break;
		case ELiteralType::UObjectProxyArray: OutLiteral.Set(Objects); break;
		default:
			OutError = FString::Printf(TEXT("A %s takes no value: wire it instead."), *DataType.ToString());
			return false;
		}
		return true;
	}

	FHyperAIMetaSoundNodeSearchReport Search(const FHyperAIMetaSoundNodeSearchRequest& Request)
	{
		using namespace Metasound::Frontend;
		FHyperAIMetaSoundNodeSearchReport Report;
		const int32 Max = FMath::Clamp(Request.MaxResults <= 0 ? 25 : Request.MaxResults, 1, 100);
		const FString Query = Request.Query.TrimStartAndEnd().ToLower();
		TArray<FString> Terms;
		Query.ParseIntoArrayWS(Terms);
		const FString Category = Request.Category.TrimStartAndEnd().ToLower();
		struct FHit
		{
			int32 Rank = 0;
			FString Sort;
			const FMetaSoundClassInfo* Info = nullptr;
		};
		const TArray<FMetaSoundClassInfo> All = ISearchEngine::Get().FindAllClasses(ISearchEngine::EResultVersion::Highest);
		TArray<FHit> Hits;
		for (const FMetaSoundClassInfo& Info : All)
		{
			if (Info.ClassType != EMetasoundFrontendClassType::External && Info.ClassType != EMetasoundFrontendClassType::Graph)
			{
				continue;
			}
			if (!Request.bIncludeDeprecated && EnumHasAnyFlags(Info.AccessFlags, EMetasoundFrontendClassAccessFlags::Deprecated))
			{
				continue;
			}
			const FMetaSoundClassSearchInfo& SearchInfo = Info.InterfaceInfo.SearchInfo;
			const FString Name = ClassText(Info.ClassName).ToLower();
			const FString Display = SearchInfo.ClassDisplayName.ToString().ToLower();
			const FString Path = JoinText(SearchInfo.Hierarchy, TEXT(" > ")).ToLower();
			if (!Category.IsEmpty() && !Path.Contains(Category))
			{
				continue;
			}
			if (!HasPinType(Info.InterfaceInfo.Inputs, Request.InputType.TrimStartAndEnd()) || !HasPinType(Info.InterfaceInfo.Outputs, Request.OutputType.TrimStartAndEnd()))
			{
				continue;
			}
			const FString Haystack = Name + TEXT(" ") + Display + TEXT(" ") + Path + TEXT(" ") + JoinText(SearchInfo.Keywords, TEXT(" ")).ToLower() + TEXT(" ")
				+ SearchInfo.ClassDescription.ToString().ToLower();
			if (!Matches(Info, Haystack, Terms))
			{
				continue;
			}
			FHit& Hit = Hits.AddDefaulted_GetRef();
			Hit.Info = &Info;
			Hit.Sort = Name;
			const FString Short = Info.ClassName.Name.ToString().ToLower();
			Hit.Rank = Query.IsEmpty() ? 3 : (Short == Query || Display == Query) ? 0 : (Short.StartsWith(Query) || Display.StartsWith(Query)) ? 1
				: Matches(Info, Name + TEXT(" ") + Display, Terms) ? 2 : 3;
		}
		Hits.Sort([](const FHit& A, const FHit& B) { return A.Rank != B.Rank ? A.Rank < B.Rank : A.Sort < B.Sort; });
		Report.TotalMatches = Hits.Num();
		const int32 First = FMath::Clamp(Request.FirstResult, 0, Hits.Num());
		for (int32 Index = First; Index < Hits.Num() && Report.Nodes.Num() < Max; ++Index)
		{
			const FMetaSoundClassInfo& Info = *Hits[Index].Info;
			FHyperAIMetaSoundNodeClass& Node = Report.Nodes.AddDefaulted_GetRef();
			Node.Class = ClassText(Info.ClassName);
			Node.MajorVersion = Info.Version.Major;
			Node.MinorVersion = Info.Version.Minor;
			Node.DisplayName = Info.InterfaceInfo.SearchInfo.ClassDisplayName.ToString();
			Node.Category = JoinText(Info.InterfaceInfo.SearchInfo.Hierarchy, TEXT(" > "));
			Node.Description = Info.InterfaceInfo.SearchInfo.ClassDescription.ToString().Left(160);
			Node.bDeprecated = EnumHasAnyFlags(Info.AccessFlags, EMetasoundFrontendClassAccessFlags::Deprecated);
			for (const FMetaSoundClassVertexInfo& Vertex : Info.InterfaceInfo.Inputs) Node.Inputs.Add(MakePin(Vertex));
			for (const FMetaSoundClassVertexInfo& Vertex : Info.InterfaceInfo.Outputs) Node.Outputs.Add(MakePin(Vertex));
		}
		Report.bTruncated = First + Report.Nodes.Num() < Hits.Num();
		Report.bOk = true;
		Report.Status = TEXT("ok");
		Report.Diagnostic = FString::Printf(TEXT("%d node class(es) match; %d shown from %d%s."), Report.TotalMatches, Report.Nodes.Num(), First,
			Report.bTruncated ? *FString::Printf(TEXT("; FirstResult %d reads on"), First + Report.Nodes.Num()) : TEXT(""));
		return Report;
	}

	FHyperAIMetaSoundNodeDescribeReport DescribeClasses(const FHyperAIMetaSoundNodeDescribeRequest& Request)
	{
		FHyperAIMetaSoundNodeDescribeReport Report;
		if (Request.Classes.Num() > 16)
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = TEXT("At most 16 classes at a time.");
			return Report;
		}
		if (Request.Classes.IsEmpty() && !Request.bListDataTypes)
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = TEXT("Give Classes as Namespace.Name.Variant (hyper_metasound_node_search finds them), or bListDataTypes.");
			return Report;
		}
		TArray<FString> Missing;
		for (const FString& Text : Request.Classes)
		{
			FMetasoundFrontendClassName Name;
			FMetasoundFrontendClass Class;
			if (!ParseClassName(Text, Name) || !FindClass(Name, 0, Class))
			{
				// A class without a variant is written Namespace.Name, which the parse would split as name and variant.
				const FString Trimmed = Text.TrimStartAndEnd();
				int32 Dot = INDEX_NONE;
				if (!Trimmed.FindChar(TEXT('.'), Dot) || !FindClass(FMetasoundFrontendClassName(FName(*Trimmed.Left(Dot)), FName(*Trimmed.Mid(Dot + 1)), NAME_None), 0, Class))
				{
					Missing.Add(Text);
					continue;
				}
			}
			Report.Nodes.Add(Describe(Class, /*bFull=*/true));
		}
		if (Request.bListDataTypes)
		{
			TArray<FName> Names;
			Metasound::Frontend::IDataTypeRegistry::Get().GetRegisteredDataTypeNames(Names);
			Names.Sort(FNameLexicalLess());
			for (const FName& Name : Names)
			{
				Report.DataTypes.Add(Name.ToString());
			}
		}
		Report.bOk = Missing.IsEmpty();
		Report.Status = Report.bOk ? TEXT("ok") : TEXT("not_found");
		Report.Diagnostic = FString::Printf(TEXT("%d class(es) described%s%s."), Report.Nodes.Num(),
			Missing.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; not registered: %s (search with hyper_metasound_node_search)"), *FString::Join(Missing, TEXT(", "))),
			Request.bListDataTypes ? *FString::Printf(TEXT(", %d data types"), Report.DataTypes.Num()) : TEXT(""));
		return Report;
	}

	bool Check(const FHyperAIMetaSoundGraphPlanRequest& Request, FCheckedPlan& Plan, FHyperAIMetaSoundGraphPlanReport& Report)
	{
		auto Refuse = [&Report](const FString& Why)
		{
			Report.bOk = false;
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Why;
			return false;
		};
		Plan.Package = CueGraph::PackageOf(Request.Path);
		FString Reason;
		if (!FHyperAIStudioNames::IsPackagePath(Plan.Package, TEXT("Path"), &Reason))
		{
			return Refuse(Reason.IsEmpty() ? FString(TEXT("Path is a content path, such as /Game/Audio/MS_Footsteps.")) : Reason);
		}
		Report.Path = Plan.Package + TEXT(".") + FPackageName::GetShortName(Plan.Package);
		const UMetaSoundSource* Existing = nullptr;
		if (!FHyperAIStudioNames::IsFreePackage(Plan.Package))
		{
			UObject* Found = CueGraph::LoadAsset(Plan.Package, *UObject::StaticClass());
			Existing = Cast<UMetaSoundSource>(Found);
			if (!Existing)
			{
				return Refuse(FString::Printf(TEXT("%s is %s, not a MetaSound Source."), *Plan.Package, Found ? *Found->GetClass()->GetName() : TEXT("an asset that does not load")));
			}
			Plan.Existing = const_cast<UMetaSoundSource*>(Existing);
		}
		Report.bCreates = Existing == nullptr;
		const FString Format = Request.Format.TrimStartAndEnd().ToLower();
		if (!Format.IsEmpty() && Format != TEXT("mono") && Format != TEXT("stereo"))
		{
			return Refuse(TEXT("Format is mono or stereo."));
		}
		Plan.bStereo = Format == TEXT("stereo");
		Plan.bOneShot = Request.bOneShot;
		if (Request.Nodes.Num() > C::MaxGraphNodes || Request.Connections.Num() > C::MaxGraphConnections || Request.Inputs.Num() > 64 || Request.Outputs.Num() > 64)
		{
			return Refuse(FString::Printf(TEXT("At most %d nodes, %d connections and 64 inputs and outputs a plan."), C::MaxGraphNodes, C::MaxGraphConnections));
		}
		if (Request.Nodes.IsEmpty() && Request.Inputs.IsEmpty() && Request.Outputs.IsEmpty() && Request.Connections.IsEmpty())
		{
			return Refuse(TEXT("The plan changes nothing: give Nodes, Inputs, Outputs or Connections."));
		}

		// The graph's own members: the source interface, what is already there, and the plan's inputs and outputs.
		TMap<FName, FPinSpec> GraphOut;
		TMap<FName, FPinSpec> GraphIn;
		const FName Trigger(TEXT("Trigger"));
		const FName AudioType(TEXT("Audio"));
		TMap<FGuid, FNodePins> ExistingNodes;
		if (Existing)
		{
			const FMetasoundFrontendDocument& Document = Cast<IMetaSoundDocumentInterface>(Plan.Existing.Get())->GetConstDocument();
			for (const FMetasoundFrontendClassInput& Input : Document.RootGraph.GetDefaultInterface().Inputs)
			{
				GraphOut.Add(Input.Name, {Input.TypeName, Input.AccessType == EMetasoundFrontendVertexAccessType::Value});
			}
			for (const FMetasoundFrontendClassOutput& Output : Document.RootGraph.GetDefaultInterface().Outputs)
			{
				GraphIn.Add(Output.Name, {Output.TypeName, Output.AccessType == EMetasoundFrontendVertexAccessType::Value});
			}
			for (const FMetasoundFrontendNode& Node : Document.RootGraph.GetConstDefaultGraph().Nodes)
			{
				FNodePins& Pins = ExistingNodes.Add(Node.GetID());
				for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Inputs) Pins.Inputs.Add(Vertex.Name, {Vertex.TypeName, false});
				for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Outputs) Pins.Outputs.Add(Vertex.Name, {Vertex.TypeName, false});
			}
		}
		else
		{
			using namespace Metasound;
			GraphOut.Add(Frontend::SourceInterface::Inputs::OnPlay, {Trigger, false});
			if (Plan.bOneShot) GraphIn.Add(Frontend::SourceOneShotInterface::Outputs::OnFinished, {Trigger, false});
			if (Plan.bStereo)
			{
				GraphIn.Add(Engine::OutputFormatStereoInterface::Outputs::LeftOut, {AudioType, false});
				GraphIn.Add(Engine::OutputFormatStereoInterface::Outputs::RightOut, {AudioType, false});
			}
			else
			{
				GraphIn.Add(Engine::OutputFormatMonoInterface::Outputs::MonoOut, {AudioType, false});
			}
		}
		for (const FHyperAIMetaSoundGraphMember& Member : Request.Inputs)
		{
			const FName Name(*Member.Name.TrimStartAndEnd());
			const FName Type(*Member.Type.TrimStartAndEnd());
			if (Name.IsNone() || GraphOut.Contains(Name))
			{
				return Refuse(FString::Printf(TEXT("The graph input %s needs a name of its own."), *Member.Name));
			}
			FMetasoundFrontendLiteral Literal;
			FString Error;
			if (!MakeLiteral(Type, Member.Default, Literal, Error))
			{
				return Refuse(FString::Printf(TEXT("Input %s: %s"), *Member.Name, *Error));
			}
			GraphOut.Add(Name, {Type, false});
			Plan.Inputs.Add({Name, Type});
			Plan.InputDefaults.Add(Literal);
		}
		for (const FHyperAIMetaSoundGraphMember& Member : Request.Outputs)
		{
			const FName Name(*Member.Name.TrimStartAndEnd());
			const FName Type(*Member.Type.TrimStartAndEnd());
			if (Name.IsNone() || GraphIn.Contains(Name) || !Metasound::Frontend::IDataTypeRegistry::Get().IsRegistered(Type))
			{
				return Refuse(FString::Printf(TEXT("The graph output %s needs a name of its own and a registered type (not %s)."), *Member.Name, *Member.Type));
			}
			GraphIn.Add(Name, {Type, false});
			Plan.Outputs.Add({Name, Type});
		}

		// Nodes.
		TMap<FString, int32> Ids;
		TArray<FNodePins> Pins;
		for (const FHyperAIMetaSoundGraphNode& In : Request.Nodes)
		{
			FCheckedNode& Node = Plan.Nodes.AddDefaulted_GetRef();
			Node.Id = In.Id.TrimStartAndEnd();
			if (Node.Id.IsEmpty() || IsGraphNode(Node.Id) || Ids.Contains(Node.Id.ToLower()))
			{
				return Refuse(FString::Printf(TEXT("Every node needs an Id of its own (not graph): %s."), *In.Id));
			}
			Ids.Add(Node.Id.ToLower(), Plan.Nodes.Num() - 1);
			FNodePins& NodePins = Pins.AddDefaulted_GetRef();
			if (In.Class.TrimStartAndEnd().IsEmpty())
			{
				FGuid Guid;
				if (!FGuid::Parse(Node.Id, Guid) || !ExistingNodes.Contains(Guid))
				{
					return Refuse(FString::Printf(TEXT("Node %s has no Class; only a node already in the MetaSound (its NodeId from hyper_sound_asset_inspect) may leave it empty."), *Node.Id));
				}
				Node.ExistingId = Guid;
				NodePins = ExistingNodes[Guid];
			}
			else
			{
				FMetasoundFrontendClass Class;
				bool bFound = ParseClassName(In.Class, Node.ClassName) && FindClass(Node.ClassName, In.MajorVersion, Class);
				if (!bFound)
				{
					const FString Trimmed = In.Class.TrimStartAndEnd();
					int32 Dot = INDEX_NONE;
					if (Trimmed.FindChar(TEXT('.'), Dot))
					{
						Node.ClassName = FMetasoundFrontendClassName(FName(*Trimmed.Left(Dot)), FName(*Trimmed.Mid(Dot + 1)), NAME_None);
						bFound = FindClass(Node.ClassName, In.MajorVersion, Class);
					}
				}
				if (!bFound)
				{
					return Refuse(FString::Printf(TEXT("Node %s: %s is not a registered MetaSound node class%s. Find one with hyper_metasound_node_search (Class is Namespace.Name.Variant, such as UE.Wave Player.Mono)."),
						*Node.Id, *In.Class, In.MajorVersion > 0 ? *FString::Printf(TEXT(" at version %d"), In.MajorVersion) : TEXT("")));
				}
				Node.ClassName = Class.Metadata.GetClassName();
				Node.MajorVersion = Class.Metadata.GetVersion().Major;
				for (const FMetasoundFrontendClassInput& Input : Class.GetDefaultInterface().Inputs)
				{
					NodePins.Inputs.Add(Input.Name, {Input.TypeName, Input.AccessType == EMetasoundFrontendVertexAccessType::Value});
				}
				for (const FMetasoundFrontendClassOutput& Output : Class.GetDefaultInterface().Outputs)
				{
					NodePins.Outputs.Add(Output.Name, {Output.TypeName, Output.AccessType == EMetasoundFrontendVertexAccessType::Value});
				}
			}
			for (const FHyperAIAudioSetting& Default : In.Defaults)
			{
				const FName PinName(*Default.Name.TrimStartAndEnd());
				const FPinSpec* Spec = NodePins.Inputs.Find(PinName);
				if (!Spec)
				{
					TArray<FName> Names;
					NodePins.Inputs.GetKeys(Names);
					return Refuse(FString::Printf(TEXT("Node %s has no input %s; its inputs are %s."), *Node.Id, *Default.Name,
						*FString::JoinBy(Names, TEXT(", "), [](const FName& Name) { return Name.ToString(); })));
				}
				FMetasoundFrontendLiteral Literal;
				FString Error;
				if (!MakeLiteral(Spec->Type, Default.Value, Literal, Error))
				{
					return Refuse(FString::Printf(TEXT("Node %s, input %s: %s"), *Node.Id, *Default.Name, *Error));
				}
				Node.Defaults.Add({PinName, Literal});
			}
		}

		// Connections: a known pin at each end, the same type, and no input fed twice.
		auto Resolve = [&](const FString& NodeId, const FString& PinText, const bool bOutput, FCheckedEndpoint& Out, FPinSpec& OutSpec, FString& OutError)
		{
			if (IsGraphNode(NodeId))
			{
				Out.Node = INDEX_NONE;
				Out.Pin = GraphAlias(PinText);
				const TMap<FName, FPinSpec>& Members = bOutput ? GraphOut : GraphIn;
				if (const FPinSpec* Spec = Members.Find(Out.Pin))
				{
					OutSpec = *Spec;
					return true;
				}
				TArray<FName> Names;
				Members.GetKeys(Names);
				OutError = FString::Printf(TEXT("The graph has no %s %s; it has %s."), bOutput ? TEXT("input") : TEXT("output"), *PinText,
					*FString::JoinBy(Names, TEXT(", "), [](const FName& Name) { return Name.ToString(); }));
				return false;
			}
			const int32* Index = Ids.Find(NodeId.TrimStartAndEnd().ToLower());
			if (!Index)
			{
				OutError = FString::Printf(TEXT("No node %s in the plan."), *NodeId);
				return false;
			}
			Out.Node = *Index;
			Out.Pin = FName(*PinText.TrimStartAndEnd());
			const TMap<FName, FPinSpec>& Map = bOutput ? Pins[*Index].Outputs : Pins[*Index].Inputs;
			if (const FPinSpec* Spec = Map.Find(Out.Pin))
			{
				OutSpec = *Spec;
				return true;
			}
			TArray<FName> Names;
			Map.GetKeys(Names);
			OutError = FString::Printf(TEXT("Node %s has no %s %s; its %ss are %s."), *NodeId, bOutput ? TEXT("output") : TEXT("input"), *PinText,
				bOutput ? TEXT("output") : TEXT("input"), *FString::JoinBy(Names, TEXT(", "), [](const FName& Name) { return Name.ToString(); }));
			return false;
		};
		TSet<FString> Fed;
		TArray<TArray<int32>> Parents;
		Parents.SetNum(Plan.Nodes.Num());
		for (const FHyperAIMetaSoundConnection& Wire : Request.Connections)
		{
			FCheckedEndpoint From;
			FCheckedEndpoint To;
			FPinSpec FromSpec;
			FPinSpec ToSpec;
			FString Error;
			if (!Resolve(Wire.FromNode, Wire.FromPin, true, From, FromSpec, Error) || !Resolve(Wire.ToNode, Wire.ToPin, false, To, ToSpec, Error))
			{
				return Refuse(Error);
			}
			const FString What = FString::Printf(TEXT("%s.%s -> %s.%s"), *Wire.FromNode, *Wire.FromPin, *Wire.ToNode, *Wire.ToPin);
			if (FromSpec.Type != ToSpec.Type)
			{
				return Refuse(FString::Printf(TEXT("%s joins a %s to a %s; MetaSound wires join one type (put a conversion node between, such as UE.Conversion with the two types)."),
					*What, *FromSpec.Type.ToString(), *ToSpec.Type.ToString()));
			}
			if (ToSpec.bConstructor && !FromSpec.bConstructor && From.Node != INDEX_NONE)
			{
				return Refuse(FString::Printf(TEXT("%s feeds a constructor input from a running output; a constructor input takes a graph input or its default."), *What));
			}
			const FString Key = FString::Printf(TEXT("%d|%s"), To.Node, *To.Pin.ToString());
			if (Fed.Contains(Key))
			{
				return Refuse(FString::Printf(TEXT("%s: that input is already fed; an input takes one wire (mix signals with a node first)."), *What));
			}
			Fed.Add(Key);
			if (From.Node != INDEX_NONE && To.Node != INDEX_NONE)
			{
				Parents[To.Node].AddUnique(From.Node);
			}
			Plan.Connections.Add({From, To});
		}

		// Layout: a node's column is one past the furthest node feeding it; rows count down each column.
		TArray<int32> Columns;
		Columns.Init(-1, Plan.Nodes.Num());
		TFunction<int32(int32, int32)> ColumnOf = [&](const int32 Node, const int32 Depth) -> int32
		{
			if (Columns[Node] >= 0) return Columns[Node];
			if (Depth > Plan.Nodes.Num()) return 0;
			int32 Column = 0;
			for (const int32 Parent : Parents[Node]) Column = FMath::Max(Column, ColumnOf(Parent, Depth + 1) + 1);
			return Columns[Node] = Column;
		};
		TMap<int32, int32> RowsUsed;
		const int32 RowOffset = Existing ? 8 : 0;
		for (int32 Index = 0; Index < Plan.Nodes.Num(); ++Index)
		{
			const FHyperAIMetaSoundGraphNode& In = Request.Nodes[Index];
			const int32 Column = In.Column >= 0 ? In.Column : ColumnOf(Index, 0) + 1;
			int32& Used = RowsUsed.FindOrAdd(Column);
			const int32 Row = In.Row >= 0 ? In.Row : Used;
			Used = FMath::Max(Used, Row + 1);
			Plan.Nodes[Index].Cell = FIntPoint(Column, Row + RowOffset);
			FHyperAIMetaSoundBuiltNode& Built = Report.Nodes.AddDefaulted_GetRef();
			Built.Id = Plan.Nodes[Index].Id;
			Built.Class = Plan.Nodes[Index].ExistingId.IsValid() ? FString() : ClassText(Plan.Nodes[Index].ClassName);
			Built.NodeId = Plan.Nodes[Index].ExistingId.IsValid() ? Plan.Nodes[Index].ExistingId.ToString(EGuidFormats::DigitsWithHyphens) : FString();
			Built.Column = Column;
			Built.Row = Row + RowOffset;
		}
		Report.ConnectionCount = Plan.Connections.Num();

		// What would leave the sound silent or endless.
		Report.Warnings.Reset();
		if (!Existing)
		{
			for (const TPair<FName, FPinSpec>& Member : GraphIn)
			{
				if (!Fed.Contains(FString::Printf(TEXT("%d|%s"), INDEX_NONE, *Member.Key.ToString())))
				{
					Report.Warnings.Add(Member.Value.Type == Trigger
						? FString::Printf(TEXT("Nothing triggers %s: a one-shot that never finishes plays until stopped (wire a player's On Finished to graph.On Finished)."), *Member.Key.ToString())
						: FString::Printf(TEXT("Nothing reaches the output %s: that channel is silent."), *Member.Key.ToString()));
				}
			}
		}
		for (int32 Index = 0; Index < Plan.Nodes.Num(); ++Index)
		{
			for (const TPair<FName, FPinSpec>& Input : Pins[Index].Inputs)
			{
				if (Input.Value.Type == Trigger && !Plan.Nodes[Index].ExistingId.IsValid() && !Fed.Contains(FString::Printf(TEXT("%d|%s"), Index, *Input.Key.ToString()))
					&& (Input.Key == TEXT("Play") || Input.Key == TEXT("Trigger") || Input.Key == TEXT("Next") || Input.Key == TEXT("Start") || Input.Key == TEXT("Trigger Attack")))
				{
					Report.Warnings.Add(FString::Printf(TEXT("Node %s's %s is not wired: it never starts."), *Plan.Nodes[Index].Id, *Input.Key.ToString()));
				}
			}
		}
		return true;
	}

	bool Build(const FCheckedPlan& Plan, const bool bSave, FHyperAIMetaSoundGraphPlanReport& Report)
	{
		FString Error;
		auto Fail = [&Report](const FString& Why)
		{
			Report.bOk = false;
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Why;
			return false;
		};
		UMetaSoundBuilderBase* Builder = nullptr;
		EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
		UObject* Asset = Plan.Existing.Get();
		if (Asset)
		{
			Asset->Modify();
			Builder = UMetaSoundEditorSubsystem::GetChecked().FindOrBeginBuilding(TScriptInterface<IMetaSoundDocumentInterface>(Asset), Result);
		}
		else
		{
			FMetaSoundBuilderNodeOutputHandle OnPlay;
			FMetaSoundBuilderNodeInputHandle OnFinished;
			TArray<FMetaSoundBuilderNodeInputHandle> Outs;
			const FName Name(*(TEXT("HyperAIGraph_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			Builder = UMetaSoundBuilderSubsystem::GetChecked().CreateSourceBuilder(Name, OnPlay, OnFinished, Outs, Result,
				Plan.bStereo ? EMetaSoundOutputAudioFormat::Stereo : EMetaSoundOutputAudioFormat::Mono, Plan.bOneShot);
		}
		if (!Builder || Result != EMetaSoundBuilderResult::Succeeded)
		{
			return Fail(TEXT("The MetaSound builder did not start."));
		}
		for (int32 Index = 0; Index < Plan.Inputs.Num(); ++Index)
		{
			Builder->AddGraphInputNode(Plan.Inputs[Index].Key, Plan.Inputs[Index].Value, Plan.InputDefaults[Index], Result);
			if (Result != EMetaSoundBuilderResult::Succeeded) return Fail(FString::Printf(TEXT("The graph input %s was refused."), *Plan.Inputs[Index].Key.ToString()));
		}
		for (const TPair<FName, FName>& Output : Plan.Outputs)
		{
			FMetasoundFrontendLiteral Literal;
			Literal.SetFromLiteral(Metasound::Frontend::IDataTypeRegistry::Get().CreateDefaultLiteral(Output.Value));
			Builder->AddGraphOutputNode(Output.Key, Output.Value, Literal, Result);
			if (Result != EMetaSoundBuilderResult::Succeeded) return Fail(FString::Printf(TEXT("The graph output %s was refused."), *Output.Key.ToString()));
		}
		TArray<FMetaSoundNodeHandle> Handles;
		for (int32 Index = 0; Index < Plan.Nodes.Num(); ++Index)
		{
			const FCheckedNode& Node = Plan.Nodes[Index];
			FMetaSoundNodeHandle Handle(Node.ExistingId);
			if (!Node.ExistingId.IsValid())
			{
				Handle = Builder->AddNodeByClassName(Node.ClassName, Result, Node.MajorVersion);
				if (Result != EMetaSoundBuilderResult::Succeeded) return Fail(FString::Printf(TEXT("Node %s (%s) could not be added."), *Node.Id, *ClassText(Node.ClassName)));
				Builder->SetNodeLocation(Handle, FVector2D(Node.Cell.X * 320.0, Node.Cell.Y * 170.0), Result);
				Report.Nodes[Index].NodeId = Handle.NodeID.ToString(EGuidFormats::DigitsWithHyphens);
			}
			for (const TPair<FName, FMetasoundFrontendLiteral>& Default : Node.Defaults)
			{
				const FMetaSoundBuilderNodeInputHandle Input = Builder->FindNodeInputByName(Handle, Default.Key, Result);
				if (Result == EMetaSoundBuilderResult::Succeeded)
				{
					Builder->SetNodeInputDefault(Input, Default.Value, Result);
				}
				if (Result != EMetaSoundBuilderResult::Succeeded) return Fail(FString::Printf(TEXT("Node %s's %s did not take its value."), *Node.Id, *Default.Key.ToString()));
			}
			Handles.Add(Handle);
		}
		for (const TPair<FCheckedEndpoint, FCheckedEndpoint>& Wire : Plan.Connections)
		{
			FMetaSoundBuilderNodeOutputHandle From;
			FMetaSoundBuilderNodeInputHandle To;
			FName DataType;
			if (Wire.Key.Node == INDEX_NONE)
			{
				Builder->FindGraphInputNode(Wire.Key.Pin, DataType, From, Result);
			}
			else
			{
				From = Builder->FindNodeOutputByName(Handles[Wire.Key.Node], Wire.Key.Pin, Result);
			}
			if (Result == EMetaSoundBuilderResult::Succeeded)
			{
				if (Wire.Value.Node == INDEX_NONE)
				{
					Builder->FindGraphOutputNode(Wire.Value.Pin, DataType, To, Result);
				}
				else
				{
					To = Builder->FindNodeInputByName(Handles[Wire.Value.Node], Wire.Value.Pin, Result);
				}
			}
			if (Result == EMetaSoundBuilderResult::Succeeded)
			{
				Builder->ConnectNodes(From, To, Result);
			}
			if (Result != EMetaSoundBuilderResult::Succeeded)
			{
				const auto Name = [&Plan](const FCheckedEndpoint& End) { return End.Node == INDEX_NONE ? FString(TEXT("graph")) : Plan.Nodes[End.Node].Id; };
				return Fail(FString::Printf(TEXT("The wire %s.%s -> %s.%s was refused."), *Name(Wire.Key), *Wire.Key.Pin.ToString(), *Name(Wire.Value), *Wire.Value.Pin.ToString()));
			}
		}
		if (!Asset)
		{
			if (!FHyperAIStudioSourceControl::MakeEditable({Plan.Package}, Error))
			{
				return Fail(Error);
			}
			const TScriptInterface<IMetaSoundDocumentInterface> Built = UMetaSoundEditorSubsystem::GetChecked().BuildToAsset(Builder, TEXT("HyperAI Studio"),
				FPackageName::GetShortName(Plan.Package), FPackageName::GetLongPackagePath(Plan.Package), Result);
			Asset = Built.GetObject();
			if (!Asset || Result != EMetaSoundBuilderResult::Succeeded)
			{
				return Fail(TEXT("The MetaSound Source could not be built into an asset."));
			}
		}
		else
		{
			Asset->PostEditChange();
			Asset->MarkPackageDirty();
		}
		Report.Path = Asset->GetPathName();
		if (bSave)
		{
			if (!FHyperAIStudioSourceControl::SaveAsset(*Asset, Error))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = Error;
				return false;
			}
			Report.Saved.Add(Asset->GetPathName());
		}
		return true;
	}

	TArray<FString> DescribeGraph(const UObject& MetaSound, const int32 MaxLines)
	{
		TArray<FString> Out;
		const IMetaSoundDocumentInterface* Document = Cast<IMetaSoundDocumentInterface>(const_cast<UObject*>(&MetaSound));
		if (!Document)
		{
			return Out;
		}
		const FMetasoundFrontendDocument& Doc = Document->GetConstDocument();
		const FMetasoundFrontendGraph& Graph = Doc.RootGraph.GetConstDefaultGraph();
		// Graph inputs and outputs read as graph.<name>; the editor's template (reroute) nodes are looked through.
		TMap<FGuid, FString> Members;
		TSet<FGuid> Templates;
		TMap<TPair<FGuid, FGuid>, FString> Vertices;
		for (const FMetasoundFrontendNode& Node : Graph.Nodes)
		{
			const FMetasoundFrontendClass* Class = Doc.Dependencies.FindByPredicate([&Node](const FMetasoundFrontendClass& Each) { return Each.ID == Node.ClassID; });
			const EMetasoundFrontendClassType Type = Class ? Class->Metadata.GetType() : EMetasoundFrontendClassType::Invalid;
			if (Type == EMetasoundFrontendClassType::Input || Type == EMetasoundFrontendClassType::Output)
			{
				Members.Add(Node.GetID(), Node.Name.ToString());
			}
			else if (Type == EMetasoundFrontendClassType::Template)
			{
				Templates.Add(Node.GetID());
			}
			for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Inputs) Vertices.Add({Node.GetID(), Vertex.VertexID}, Vertex.Name.ToString());
			for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Outputs) Vertices.Add({Node.GetID(), Vertex.VertexID}, Vertex.Name.ToString());
			if (!Members.Contains(Node.GetID()) && !Templates.Contains(Node.GetID()) && Out.Num() < MaxLines)
			{
				Out.Add(FString::Printf(TEXT("node %s %s"), *Node.GetID().ToString(EGuidFormats::DigitsWithHyphens), Class ? *ClassText(Class->Metadata.GetClassName()) : TEXT("(unknown class)")));
			}
		}
		const auto Name = [&](const FGuid& NodeId, const FGuid& VertexId)
		{
			if (const FString* Member = Members.Find(NodeId))
			{
				return TEXT("graph.") + *Member;
			}
			return NodeId.ToString(EGuidFormats::DigitsWithHyphens) + TEXT(".") + Vertices.FindRef({NodeId, VertexId});
		};
		// What feeds each template node, so a wire through one reads from its real source.
		TMap<FGuid, FString> Through;
		for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
		{
			if (Templates.Contains(Edge.ToNodeID))
			{
				Through.Add(Edge.ToNodeID, Name(Edge.FromNodeID, Edge.FromVertexID));
			}
		}
		for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
		{
			if (Out.Num() >= MaxLines) break;
			if (Templates.Contains(Edge.ToNodeID)) continue;
			const FString* Source = Through.Find(Edge.FromNodeID);
			Out.Add(FString::Printf(TEXT("wire %s -> %s"), Source ? **Source : *Name(Edge.FromNodeID, Edge.FromVertexID), *Name(Edge.ToNodeID, Edge.ToVertexID)));
		}
		return Out;
	}
}
