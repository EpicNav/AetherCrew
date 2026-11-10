// Games by Hyper 2026.

#include "HyperAIStudioChooserGate.h"

#include "BoolColumn.h"
#include "Chooser.h"
#include "ChooserPropertyAccess.h"
#include "Engine/Blueprint.h"
#include "EnumColumn.h"
#include "FloatDistanceColumn.h"
#include "FloatRangeColumn.h"
#include "GameplayTagColumn.h"
#include "GameplayTagContainer.h"
#include "GameplayTagQueryColumn.h"
#include "HyperAIStudioNames.h"
#include "IChooserParameterEnum.h"
#include "Misc/PackageName.h"
#include "MultiEnumColumn.h"
#include "NameColumn.h"
#include "ObjectChooser_Asset.h"
#include "ObjectChooser_Class.h"
#include "ObjectClassColumn.h"
#include "ObjectColumn.h"
#include "OutputBoolColumn.h"
#include "OutputEnumColumn.h"
#include "OutputFloatColumn.h"
#include "OutputGameplayTagColumn.h"
#include "OutputNameColumn.h"
#include "OutputObjectColumn.h"
#include "RandomizeColumn.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::Chooser::Gate
{
	using C = FHyperAIStudioChooserContracts;

	namespace
	{
		FString Lower(const FString& Text)
		{
			return Text.TrimStartAndEnd().ToLower();
		}

		bool IsAny(const FString& Text)
		{
			const FString Trimmed = Text.TrimStartAndEnd();
			return Trimmed.IsEmpty() || Trimmed == TEXT("*") || Trimmed.Equals(TEXT("any"), ESearchCase::IgnoreCase);
		}

		FString Number(const double Value)
		{
			return FString::SanitizeFloat(Value, 0);
		}

		/** /Game/A/CT_B from /Game/A/CT_B or /Game/A/CT_B.CT_B. */
		FString PackageOf(const FString& Path)
		{
			FString Trimmed = Path.TrimStartAndEnd();
			int32 Dot = INDEX_NONE;
			if (Trimmed.FindChar(TEXT('.'), Dot)) Trimmed.LeftInline(Dot);
			return Trimmed;
		}

		/** An object by path, found in memory or loaded quietly when its package exists on disk. */
		UObject* LoadQuiet(const FString& InPath, const TCHAR* Field, FString& OutError)
		{
			const FString Path = InPath.TrimStartAndEnd();
			if (!FHyperAIStudioNames::IsObjectPath(Path, Field, &OutError)) return nullptr;
			const FString ObjectPath = Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
			UObject* Found = FindObject<UObject>(nullptr, *ObjectPath);
			if (!Found && !ObjectPath.StartsWith(TEXT("/Script/")) && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ObjectPath)))
			{
				Found = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
			if (!Found) OutError = FString::Printf(TEXT("Nothing is at %s."), *Path);
			return Found;
		}

		/** A class by path: /Script/Engine.Character, or a Blueprint (/Game/BP_Hero, /Game/BP_Hero.BP_Hero_C). */
		UClass* LoadClassQuiet(const FString& Path, const TCHAR* Field, FString& OutError)
		{
			UObject* Found = LoadQuiet(Path, Field, OutError);
			if (UClass* Class = Cast<UClass>(Found)) return Class;
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(Found))
			{
				if (Blueprint->GeneratedClass) return Blueprint->GeneratedClass;
				OutError = FString::Printf(TEXT("%s is a Blueprint that has not been compiled."), *Path);
				return nullptr;
			}
			if (Found) OutError = FString::Printf(TEXT("%s is a %s, not a class."), *Path, *Found->GetClass()->GetName());
			else if (!Path.EndsWith(TEXT("_C")) && !Path.StartsWith(TEXT("/Script/")))
			{
				FString Ignored;
				const FString Package = PackageOf(Path);
				if (UClass* Class = Cast<UClass>(LoadQuiet(Package + TEXT(".") + FPackageName::GetShortName(Package) + TEXT("_C"), Field, Ignored))) return Class;
			}
			return nullptr;
		}

		UScriptStruct* LoadStructQuiet(const FString& Path, FString& OutError)
		{
			UObject* Found = LoadQuiet(Path, TEXT("ContextStructs"), OutError);
			if (UScriptStruct* Struct = Cast<UScriptStruct>(Found)) return Struct;
			if (Found) OutError = FString::Printf(TEXT("%s is a %s, not a struct."), *Path, *Found->GetClass()->GetName());
			return nullptr;
		}

		const UStruct* ParameterType(const FInstancedStruct& Parameter)
		{
			if (const FContextObjectTypeClass* Class = Parameter.GetPtr<FContextObjectTypeClass>()) return Class->Class;
			if (const FContextObjectTypeStruct* Struct = Parameter.GetPtr<FContextObjectTypeStruct>()) return Struct->Struct;
			return nullptr;
		}

		FString CppName(const UStruct& Struct)
		{
			return FString(Struct.GetPrefixCPP()) + Struct.GetName();
		}

		/** The properties of Type a binding of TypeFilter can take, for a refusal. */
		FString BindableNames(const UStruct& Type, const FString& TypeFilter)
		{
			TArray<FString> Names;
			for (TFieldIterator<FProperty> It(&Type); It && Names.Num() < 100; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_BlueprintVisible) && CanBind(**It, TypeFilter)) Names.Add(It->GetName());
			}
			return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT(", "));
		}

		FChooserPropertyBinding* MutableBindingOf(FInstancedStruct& Input)
		{
			if (!Input.IsValid()) return nullptr;
			FStructProperty* Property = BindingPropertyOf(Input.GetScriptStruct());
			return Property ? Property->ContainerPtrToValuePtr<FChooserPropertyBinding>(Input.GetMutableMemory()) : nullptr;
		}

		bool IsBound(const FChooserPropertyBinding& Binding)
		{
			return Binding.IsBoundToRoot || !Binding.PropertyBindingChain.IsEmpty();
		}

		bool ParseTags(const FString& Text, FGameplayTagContainer& Out, FString& OutError)
		{
			Out.Reset();
			TArray<FString> Parts;
			Text.ParseIntoArray(Parts, TEXT(","), true);
			for (FString Part : Parts)
			{
				Part.TrimStartAndEndInline();
				if (Part.IsEmpty()) continue;
				if (!FHyperAIStudioNames::IsName(Part, TEXT("Cells"), &OutError)) return false;
				const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Part), /*ErrorIfNotFound=*/false);
				if (!Tag.IsValid())
				{
					OutError = FString::Printf(TEXT("%s is not a gameplay tag in this project (Project Settings > Gameplay Tags)."), *Part);
					return false;
				}
				Out.AddTag(Tag);
			}
			return true;
		}

		FString TagsText(const FGameplayTagContainer& Tags)
		{
			TArray<FString> Names;
			for (const FGameplayTag& Tag : Tags) Names.Add(Tag.ToString());
			return FString::Join(Names, TEXT(", "));
		}

		bool ParseQuery(const FString& Text, FGameplayTagQuery& Out, FString& OutError)
		{
			const FString Trimmed = Text.TrimStartAndEnd();
			if (IsAny(Trimmed))
			{
				Out = FGameplayTagQuery();
				return true;
			}
			FString Mode;
			FString Rest;
			if (!Trimmed.Split(TEXT(":"), &Mode, &Rest))
			{
				OutError = FString::Printf(TEXT("'%s' is not a tag query; write any:Tags, all:Tags or none:Tags, the tags comma separated."), *Trimmed);
				return false;
			}
			FGameplayTagContainer Tags;
			if (!ParseTags(Rest, Tags, OutError)) return false;
			Mode = Lower(Mode);
			if (Mode == TEXT("any")) Out = FGameplayTagQuery::MakeQuery_MatchAnyTags(Tags);
			else if (Mode == TEXT("all")) Out = FGameplayTagQuery::MakeQuery_MatchAllTags(Tags);
			else if (Mode == TEXT("none")) Out = FGameplayTagQuery::MakeQuery_MatchNoTags(Tags);
			else
			{
				OutError = FString::Printf(TEXT("'%s' is not a tag query mode; the modes are any, all and none."), *Mode);
				return false;
			}
			return true;
		}

		FString StructText(const UScriptStruct& Struct, const void* Value)
		{
			FString Out;
			Struct.ExportText(Out, Value, Value, nullptr, PPF_None, nullptr);
			return Out;
		}

		/** A Result as the rows hold it. Object/class tables check it against OutputObjectType. */
		bool ParseResult(const UChooserTable& Table, const FString& InText, const TCHAR* Field, FInstancedStruct& Out, FString& OutError)
		{
			const FString Text = InText.TrimStartAndEnd();
			const UChooserTable* Owner = Table.GetContextOwner();
			const EObjectChooserResultType ResultType = Owner->ResultType;
			UClass* OutputType = Owner->OutputObjectType ? Owner->OutputObjectType.Get() : UObject::StaticClass();
			if (ResultType == EObjectChooserResultType::NoPrimaryResult)
			{
				if (!Text.IsEmpty() && !Text.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					OutError = FString::Printf(TEXT("this table has no result (ResultType none): its rows only write output columns; leave %s empty."), Field);
					return false;
				}
				// As the Chooser editor does: a placeholder result, since a row without one never writes its outputs.
				Out.InitializeAs(FClassChooser::StaticStruct());
				Out.GetMutable<FClassChooser>().Class = UClass::StaticClass();
				return true;
			}
			if (Text.IsEmpty())
			{
				OutError = FString::Printf(TEXT("give %s: an asset path, soft:path, a class, chooser:path or nested:Name."), Field);
				return false;
			}
			if (Text.StartsWith(TEXT("chooser:"), ESearchCase::IgnoreCase))
			{
				FString Why;
				UChooserTable* Other = LoadTable(Text.Mid(8), Why);
				if (!Other)
				{
					OutError = Why;
					return false;
				}
				if (Other == &Table || Other == Table.GetRootChooser())
				{
					OutError = TEXT("a row cannot evaluate the table it is in.");
					return false;
				}
				Out.InitializeAs(FEvaluateChooser::StaticStruct());
				Out.GetMutable<FEvaluateChooser>().Chooser = Other;
				return true;
			}
			if (Text.StartsWith(TEXT("nested:"), ESearchCase::IgnoreCase))
			{
				const FString Name = Text.Mid(7).TrimStartAndEnd();
				TArray<FString> Names;
				for (const TObjectPtr<UObject>& Nested : Table.GetRootChooser()->NestedObjects)
				{
					UChooserTable* NestedTable = Cast<UChooserTable>(Nested);
					if (!NestedTable) continue;
					if (NestedTable->GetName().Equals(Name, ESearchCase::IgnoreCase))
					{
						Out.InitializeAs(FNestedChooser::StaticStruct());
						Out.GetMutable<FNestedChooser>().Chooser = NestedTable;
						return true;
					}
					Names.Add(NestedTable->GetName());
				}
				OutError = FString::Printf(TEXT("no table nested in this one is named %s; the nested tables are %s (make nested tables in the Chooser editor)."), *Name,
					Names.IsEmpty() ? TEXT("none") : *FString::Join(Names, TEXT(", ")));
				return false;
			}
			if (ResultType == EObjectChooserResultType::ClassResult)
			{
				UClass* Class = LoadClassQuiet(Text, Field, OutError);
				if (!Class) return false;
				if (!Class->IsChildOf(OutputType))
				{
					OutError = FString::Printf(TEXT("%s is not a subclass of %s, the table's result class."), *Class->GetPathName(), *OutputType->GetName());
					return false;
				}
				Out.InitializeAs(FClassChooser::StaticStruct());
				Out.GetMutable<FClassChooser>().Class = Class;
				return true;
			}
			const bool bSoft = Text.StartsWith(TEXT("soft:"), ESearchCase::IgnoreCase);
			const FString Path = bSoft ? Text.Mid(5).TrimStartAndEnd() : Text;
			UObject* Asset = LoadQuiet(Path, Field, OutError);
			if (!Asset) return false;
			if (!Asset->IsA(OutputType))
			{
				OutError = FString::Printf(TEXT("%s is a %s, not a %s, the table's result class."), *Asset->GetPathName(), *Asset->GetClass()->GetName(), *OutputType->GetName());
				return false;
			}
			if (bSoft)
			{
				Out.InitializeAs(FSoftAssetChooser::StaticStruct());
				Out.GetMutable<FSoftAssetChooser>().Asset = TSoftObjectPtr<UObject>(Asset);
			}
			else
			{
				Out.InitializeAs(FAssetChooser::StaticStruct());
				Out.GetMutable<FAssetChooser>().Asset = Asset;
			}
			return true;
		}

		const UEnum* EnumOfColumn(const FInstancedStruct& Column)
		{
			const FInstancedStruct* Input = const_cast<FInstancedStruct&>(Column).GetMutable<FChooserColumnBase>().GetInputValuePtr();
			const FChooserParameterEnumBase* Enum = Input && Input->IsValid() ? Input->GetPtr<FChooserParameterEnumBase>() : nullptr;
			return Enum ? Enum->GetEnum() : nullptr;
		}

		bool ResolveEnumCell(const FInstancedStruct& Column, const FString& Text, int32& OutIndex, const UEnum*& OutEnum, FString& OutError)
		{
			OutEnum = EnumOfColumn(Column);
			if (!OutEnum)
			{
				OutError = TEXT("the column is not bound to an enum property yet; give it a Binding first.");
				return false;
			}
			OutIndex = FindEnumIndex(*OutEnum, Text);
			if (OutIndex == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("'%s' is not a value of %s; its values are %s."), *Text, *OutEnum->GetName(), *EnumWords(*OutEnum));
				return false;
			}
			return true;
		}

		/** A setting by name: its own name in any case, without a leading b, or its display name without spaces. */
		FProperty* FindSetting(const UScriptStruct& Struct, const FString& Name, FName RowValues)
		{
			const FString Wanted = Name.TrimStartAndEnd().Replace(TEXT(" "), TEXT(""));
			for (TFieldIterator<FProperty> It(&Struct); It; ++It)
			{
				FProperty* Property = *It;
				const FString Own = Property->GetName();
				const FString Plain = Own.StartsWith(TEXT("b"), ESearchCase::CaseSensitive) && Own.Len() > 1 && FChar::IsUpper(Own[1]) ? Own.Mid(1) : Own;
				const bool bNamed = Own.Equals(Wanted, ESearchCase::IgnoreCase) || Plain.Equals(Wanted, ESearchCase::IgnoreCase)
					|| Property->GetDisplayNameText().ToString().Replace(TEXT(" "), TEXT("")).Equals(Wanted, ESearchCase::IgnoreCase);
				if (!bNamed) continue;
				const bool bSetting = Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated)
					&& Property->GetFName() != RowValues && !Own.EndsWith(TEXT("_DEPRECATED")) && Own != TEXT("DefaultRowValue") && Own != TEXT("EditorColumnWidth")
					&& !(CastField<FStructProperty>(Property) && CastField<FStructProperty>(Property)->Struct == FInstancedStruct::StaticStruct());
				return bSetting ? Property : nullptr;
			}
			return nullptr;
		}

		bool IsSetting(const FProperty& Property, FName RowValues)
		{
			const FString Own = Property.GetName();
			return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated) && Property.GetFName() != RowValues
				&& !Own.EndsWith(TEXT("_DEPRECATED")) && Own != TEXT("DefaultRowValue") && Own != TEXT("EditorColumnWidth")
				&& !(CastField<FStructProperty>(&Property) && CastField<FStructProperty>(&Property)->Struct == FInstancedStruct::StaticStruct());
		}

		FName RowValuesName(const FInstancedStruct& Column)
		{
			return const_cast<FInstancedStruct&>(Column).GetMutable<FChooserColumnBase>().RowValuesPropertyName();
		}

		FString SettingNames(const UScriptStruct& Struct, FName RowValues)
		{
			TArray<FString> Names;
			for (TFieldIterator<FProperty> It(&Struct); It; ++It)
			{
				if (IsSetting(**It, RowValues)) Names.Add(It->GetName());
			}
			return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT(", "));
		}

		/** Params onto a column, by reflection, each value checked before it is written. */
		bool ApplyParams(FInstancedStruct& Column, const FString& Params, UObject* Owner, TArray<FString>& OutDone, FString& OutError)
		{
			TArray<TPair<FString, FString>> Pairs;
			if (!ParseSettings(Params, Pairs, OutError)) return false;
			const UScriptStruct& Struct = *Column.GetScriptStruct();
			const FName RowValues = RowValuesName(Column);
			for (const TPair<FString, FString>& Pair : Pairs)
			{
				FProperty* Property = FindSetting(Struct, Pair.Key, RowValues);
				if (!Property)
				{
					OutError = FString::Printf(TEXT("%s has no setting '%s'; its settings are %s."), *Struct.GetName(), *Pair.Key, *SettingNames(Struct, RowValues));
					return false;
				}
				FString Value = Pair.Value;
				if (CastField<FBoolProperty>(Property))
				{
					const FString Word = Lower(Value);
					if (Word == TEXT("true") || Word == TEXT("yes") || Word == TEXT("on") || Word == TEXT("1")) Value = TEXT("True");
					else if (Word == TEXT("false") || Word == TEXT("no") || Word == TEXT("off") || Word == TEXT("0")) Value = TEXT("False");
					else
					{
						OutError = FString::Printf(TEXT("%s is true or false, not '%s'."), *Property->GetName(), *Pair.Value);
						return false;
					}
				}
				const UEnum* Enum = CastField<FEnumProperty>(Property) ? CastField<FEnumProperty>(Property)->GetEnum()
					: CastField<FByteProperty>(Property) ? CastField<FByteProperty>(Property)->Enum.Get() : nullptr;
				if (Enum)
				{
					const int32 Index = FindEnumIndex(*Enum, Value);
					if (Index == INDEX_NONE)
					{
						OutError = FString::Printf(TEXT("'%s' is not a %s; the values are %s."), *Pair.Value, *Property->GetName(), *EnumWords(*Enum));
						return false;
					}
					Value = Enum->GetNameStringByIndex(Index);
				}
				// The import makes names of the text, and loads an asset it names (making the package when nothing is there).
				if (!FHyperAIStudioNames::CanImportText(*Property, Value, *Pair.Key, &OutError)) return false;
				void* Container = Column.GetMutableMemory();
				const TCHAR* Rest = Property->ImportText_InContainer(*Value, Container, Owner, PPF_None);
				if (!Rest || !FString(Rest).TrimStartAndEnd().IsEmpty())
				{
					OutError = FString::Printf(TEXT("'%s' is not a value for %s (%s)."), *Pair.Value, *Property->GetName(), *Property->GetCPPType());
					return false;
				}
				FString Now;
				Property->ExportText_InContainer(0, Now, Container, Container, Owner, PPF_None);
				OutDone.Add(FString::Printf(TEXT("%s = %s"), *Property->GetName(), *Now));
			}
			return true;
		}

		/** Each column's per-row array the same length as the rows, as the Chooser editor keeps them. */
		void SyncRows(UChooserTable& Table)
		{
			const int32 Rows = Table.ResultsStructs.Num();
			Table.DisabledRows.SetNum(Rows);
			for (FInstancedStruct& Column : Table.ColumnsStructs)
			{
				if (Column.IsValid()) Column.GetMutable<FChooserColumnBase>().SetNumRows(Rows);
			}
		}

		bool IsPoseMatch(const FInstancedStruct& Column)
		{
			return Column.GetScriptStruct() && Column.GetScriptStruct()->GetName() == TEXT("PoseSearchColumn");
		}

		/** Writes one cell. Row is a row index, or ChooserColumn_SpecialIndex_Fallback for an output column's fallback value. */
		bool SetCell(UChooserTable& Table, FInstancedStruct& Column, const int32 Row, const FString& InText, FString& OutError)
		{
			const FString Text = InText.TrimStartAndEnd();
			const bool bFallback = Row == ChooserColumn_SpecialIndex_Fallback;
			FChooserColumnBase& Base = Column.GetMutable<FChooserColumnBase>();
			if (bFallback && (!Base.HasOutputs() || Base.HasFilters()))
			{
				if (Text.IsEmpty()) return true;
				OutError = TEXT("only output columns hold a fallback value; leave a filter column's fallback cell empty.");
				return false;
			}
			if (IsPoseMatch(Column))
			{
				if (Text.IsEmpty()) return true;
				OutError = TEXT("a pose_match column's cells are the pose search databases the Chooser editor keeps inside the table; leave them empty and pick each row's database in the Chooser editor.");
				return false;
			}
			if (FBoolColumn* Bool = Column.GetMutablePtr<FBoolColumn>())
			{
				uint8 Value = 2;
				if (!ParseBoolCell(Text, Value, OutError)) return false;
				Bool->RowValuesWithAny[Row] = static_cast<EBoolColumnCellValue>(Value);
				return true;
			}
			if (FFloatRangeColumn* Range = Column.GetMutablePtr<FFloatRangeColumn>())
			{
				double Min = 0.0, Max = 0.0;
				bool bNoMin = true, bNoMax = true;
				if (!ParseRange(Text, Min, Max, bNoMin, bNoMax, OutError)) return false;
				if (!bNoMin && !bNoMax && Min > Max && !Range->bWrapInput)
				{
					OutError = FString::Printf(TEXT("'%s' runs backwards; the smaller number comes first (a backwards range, across the wrap, is for a column with WrapInput=true, such as angles)."), *Text);
					return false;
				}
				FChooserFloatRangeRowData& Cell = Range->RowValues[Row];
				Cell.Min = static_cast<float>(Min);
				Cell.Max = static_cast<float>(Max);
				Cell.bNoMin = bNoMin;
				Cell.bNoMax = bNoMax;
				return true;
			}
			if (FFloatDistanceColumn* Distance = Column.GetMutablePtr<FFloatDistanceColumn>())
			{
				double Value = 0.0;
				if (!Text.IsEmpty() && !ParseNumber(Text, Value))
				{
					OutError = FString::Printf(TEXT("'%s' is not a number."), *Text);
					return false;
				}
				Distance->RowValues[Row].Value = static_cast<float>(Value);
				return true;
			}
			if (FEnumColumn* Enum = Column.GetMutablePtr<FEnumColumn>())
			{
				FString Value;
				const int32 Mode = ParseComparison(Text, Value);
				FChooserEnumRowData& Cell = Enum->RowValues[Row];
				if (Mode == 2)
				{
					Cell.Comparison = EEnumColumnCellValueComparison::MatchAny;
					return true;
				}
				int32 Index = INDEX_NONE;
				const UEnum* Type = nullptr;
				if (!ResolveEnumCell(Column, Value, Index, Type, OutError)) return false;
				Cell.Comparison = Mode == 1 ? EEnumColumnCellValueComparison::MatchNotEqual : EEnumColumnCellValueComparison::MatchEqual;
				Cell.Value = static_cast<uint8>(Type->GetValueByIndex(Index));
				Cell.ValueName = Type->GetNameByIndex(Index);
				return true;
			}
			if (FMultiEnumColumn* Multi = Column.GetMutablePtr<FMultiEnumColumn>())
			{
				uint32 Mask = 0;
				if (!IsAny(Text))
				{
					TArray<FString> Parts;
					Text.ParseIntoArray(Parts, TEXT("|"), true);
					for (const FString& Part : Parts)
					{
						int32 Index = INDEX_NONE;
						const UEnum* Type = nullptr;
						if (!ResolveEnumCell(Column, Part.TrimStartAndEnd(), Index, Type, OutError)) return false;
						const int64 Value = Type->GetValueByIndex(Index);
						if (Value < 0 || Value > 31)
						{
							OutError = FString::Printf(TEXT("%s has the value %lld; an Enum (Or) column holds values 0 to 31."), *Part, static_cast<long long>(Value));
							return false;
						}
						Mask |= 1u << static_cast<uint32>(Value);
					}
				}
				Multi->RowValues[Row].Value = Mask;
				return true;
			}
			if (FGameplayTagColumn* Tags = Column.GetMutablePtr<FGameplayTagColumn>())
			{
				return ParseTags(IsAny(Text) ? FString() : Text, Tags->RowValues[Row], OutError);
			}
			if (FGameplayTagQueryColumn* Query = Column.GetMutablePtr<FGameplayTagQueryColumn>())
			{
				return ParseQuery(Text, Query->RowValues[Row], OutError);
			}
			if (FObjectColumn* Object = Column.GetMutablePtr<FObjectColumn>())
			{
				FString Value;
				const int32 Mode = ParseComparison(Text, Value);
				FChooserObjectRowData& Cell = Object->RowValues[Row];
				if (Mode == 2)
				{
					Cell.Comparison = EObjectColumnCellValueComparison::MatchAny;
					Cell.Value = nullptr;
					return true;
				}
				UObject* Found = LoadQuiet(Value, TEXT("Cells"), OutError);
				if (!Found) return false;
				Cell.Comparison = Mode == 1 ? EObjectColumnCellValueComparison::MatchNotEqual : EObjectColumnCellValueComparison::MatchEqual;
				Cell.Value = TSoftObjectPtr<UObject>(Found);
				return true;
			}
			if (FObjectClassColumn* Class = Column.GetMutablePtr<FObjectClassColumn>())
			{
				FChooserObjectClassRowData& Cell = Class->RowValues[Row];
				if (IsAny(Text))
				{
					Cell.Comparison = EObjectClassColumnCellValueComparison::Any;
					Cell.Value = nullptr;
					return true;
				}
				FString Path = Text;
				EObjectClassColumnCellValueComparison Mode = EObjectClassColumnCellValueComparison::SubClassOf;
				if (Path.StartsWith(TEXT("!="))) { Mode = EObjectClassColumnCellValueComparison::NotEqual; Path.RightChopInline(2); }
				else if (Path.StartsWith(TEXT("="))) { Mode = EObjectClassColumnCellValueComparison::Equal; Path.RightChopInline(1); }
				else if (Path.StartsWith(TEXT("!"))) { Mode = EObjectClassColumnCellValueComparison::NotSubClassOf; Path.RightChopInline(1); }
				UClass* Found = LoadClassQuiet(Path.TrimStartAndEnd(), TEXT("Cells"), OutError);
				if (!Found) return false;
				Cell.Comparison = Mode;
				Cell.Value = Found;
				return true;
			}
			if (FChooserNameColumn* Name = Column.GetMutablePtr<FChooserNameColumn>())
			{
				FString Value;
				const int32 Mode = ParseComparison(Text, Value);
				FChooserNameRowData& Cell = Name->RowValues[Row];
				if (Mode == 2)
				{
					Cell.Comparison = ENameColumnCellValueComparison::MatchAny;
					return true;
				}
				if (!FHyperAIStudioNames::IsName(Value, TEXT("Cells"), &OutError)) return false;
				Cell.Comparison = Mode == 1 ? ENameColumnCellValueComparison::MatchNotEqual : ENameColumnCellValueComparison::MatchEqual;
				Cell.Value = FName(*Value);
				return true;
			}
			if (FRandomizeColumn* Random = Column.GetMutablePtr<FRandomizeColumn>())
			{
				double Weight = 1.0;
				if (!Text.IsEmpty() && (!ParseNumber(Text, Weight) || Weight < 0.0))
				{
					OutError = FString::Printf(TEXT("'%s' is not a weight (0 or more)."), *Text);
					return false;
				}
				Random->RowValues[Row] = static_cast<float>(Weight);
				return true;
			}
			if (FOutputFloatColumn* OutFloat = Column.GetMutablePtr<FOutputFloatColumn>())
			{
				double Value = 0.0;
				if (!Text.IsEmpty() && !ParseNumber(Text, Value))
				{
					OutError = FString::Printf(TEXT("'%s' is not a number."), *Text);
					return false;
				}
				OutFloat->GetValueForIndex(Row) = Value;
				return true;
			}
			if (FOutputBoolColumn* OutBool = Column.GetMutablePtr<FOutputBoolColumn>())
			{
				uint8 Value = 0;
				if (!Text.IsEmpty() && (!ParseBoolCell(Text, Value, OutError) || Value == 2))
				{
					OutError = FString::Printf(TEXT("an output_bool cell is true or false, not '%s'."), *Text);
					return false;
				}
				OutBool->GetValueForIndex(Row) = Value == 1;
				return true;
			}
			if (FOutputEnumColumn* OutEnum = Column.GetMutablePtr<FOutputEnumColumn>())
			{
				FChooserOutputEnumRowData& Cell = OutEnum->GetValueForIndex(Row);
				if (Text.IsEmpty())
				{
					Cell.Value = 0;
					Cell.ValueName = NAME_None;
					return true;
				}
				int32 Index = INDEX_NONE;
				const UEnum* Type = nullptr;
				if (!ResolveEnumCell(Column, Text, Index, Type, OutError)) return false;
				Cell.Value = static_cast<uint8>(Type->GetValueByIndex(Index));
				Cell.ValueName = Type->GetNameByIndex(Index);
				return true;
			}
			if (FOutputObjectColumn* OutObject = Column.GetMutablePtr<FOutputObjectColumn>())
			{
				FChooserOutputObjectRowData& Cell = OutObject->GetValueForIndex(Row);
				if (Text.IsEmpty() || Text.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					Cell.Value.Reset();
					return true;
				}
				// The value is an object chooser, as a row's Result is; a no-result table still takes assets here.
				FInstancedStruct Value;
				if (Text.StartsWith(TEXT("chooser:"), ESearchCase::IgnoreCase) || Text.StartsWith(TEXT("nested:"), ESearchCase::IgnoreCase)
					|| Table.GetContextOwner()->ResultType != EObjectChooserResultType::NoPrimaryResult)
				{
					if (!ParseResult(Table, Text, TEXT("Cells"), Value, OutError)) return false;
				}
				else
				{
					const bool bSoft = Text.StartsWith(TEXT("soft:"), ESearchCase::IgnoreCase);
					UObject* Asset = LoadQuiet(bSoft ? Text.Mid(5) : Text, TEXT("Cells"), OutError);
					if (!Asset) return false;
					if (bSoft)
					{
						Value.InitializeAs(FSoftAssetChooser::StaticStruct());
						Value.GetMutable<FSoftAssetChooser>().Asset = TSoftObjectPtr<UObject>(Asset);
					}
					else
					{
						Value.InitializeAs(FAssetChooser::StaticStruct());
						Value.GetMutable<FAssetChooser>().Asset = Asset;
					}
				}
				Cell.Value = MoveTemp(Value);
				return true;
			}
			if (FChooserOutputNameColumn* OutName = Column.GetMutablePtr<FChooserOutputNameColumn>())
			{
				if (!FHyperAIStudioNames::IsName(Text, TEXT("Cells"), &OutError)) return false;
				OutName->GetValueForIndex(Row) = Text.IsEmpty() ? NAME_None : FName(*Text);
				return true;
			}
			if (FOutputGameplayTagColumn* OutTags = Column.GetMutablePtr<FOutputGameplayTagColumn>())
			{
				return ParseTags(Text, OutTags->GetValueForIndex(Row), OutError);
			}
			if (Text.IsEmpty()) return true;
			OutError = FString::Printf(TEXT("the plan does not write cells of a %s column; leave its cells empty and edit them in the Chooser editor."), *Column.GetScriptStruct()->GetName());
			return false;
		}

		/** Binds a column's input (its InputValue) to a property path, as the Chooser editor's binding menu does. */
		bool BindColumn(UChooserTable& Table, FInstancedStruct& Column, const FString& Path, FString& OutText, FString& OutError)
		{
			FChooserColumnBase& Base = Column.GetMutable<FChooserColumnBase>();
			FInstancedStruct* Input = Base.GetInputValuePtr();
			if (!Input)
			{
				OutError = FString::Printf(TEXT("a %s column takes no Binding."), *Column.GetScriptStruct()->GetName());
				return false;
			}
			if (!Input->IsValid() || !BindingPropertyOf(Input->GetScriptStruct()))
			{
				// An input not made by the column's constructor (Pose Match's pose history): the base type, or the first
				// type under it that has a binding, as the Chooser editor's input type picker offers them.
				const UScriptStruct* Type = Base.GetInputBaseType();
				if (Type && !BindingPropertyOf(Type))
				{
					const UScriptStruct* Found = nullptr;
					for (TObjectIterator<UScriptStruct> It; It && !Found; ++It)
					{
						if (*It != Type && It->IsChildOf(Type) && BindingPropertyOf(*It)) Found = *It;
					}
					Type = Found;
				}
				if (!Type)
				{
					OutError = FString::Printf(TEXT("a %s column has no input type with a binding."), *Column.GetScriptStruct()->GetName());
					return false;
				}
				Base.SetInputType(Type);
			}
			FStructProperty* Property = BindingPropertyOf(Input->GetScriptStruct());
			const FString TypeFilter = Property->GetMetaData(TEXT("BindingType"));
			FBindingPath Resolved;
			if (!ResolveBinding(Table, Path, TypeFilter, Resolved, OutError)) return false;
			FChooserPropertyBinding* Binding = Property->ContainerPtrToValuePtr<FChooserPropertyBinding>(Input->GetMutableMemory());
			Binding->PropertyBindingChain = Resolved.Chain;
			Binding->ContextIndex = Resolved.ContextIndex;
			Binding->IsBoundToRoot = Resolved.bRoot;
			// The name the Chooser editor shows on the column, made as SPropertyAccessChainWidget::SetPropertyBinding makes it.
			Binding->DisplayName = FString();
			const UStruct* ParameterStruct = ParameterType(Table.GetContextData()[Resolved.ContextIndex]);
			if (Resolved.bRoot)
			{
				if (ParameterStruct) Binding->DisplayName = ParameterStruct->GetDisplayNameText().ToString();
			}
			else if (Resolved.Leaf)
			{
				Binding->DisplayName = Resolved.Leaf->GetDisplayNameText().ToString();
				if (Binding->DisplayName.Len() < 5 && Resolved.Chain.Num() > 1) Binding->DisplayName = Resolved.Chain[Resolved.Chain.Num() - 2].ToString() + TEXT(".") + Binding->DisplayName;
			}
			Binding->SetPropertyData(Table.GetRootChooser(), const_cast<FProperty*>(Resolved.Leaf));
			Base.InputValueChanged();
			OutText = Resolved.Text;
			return true;
		}

		/** The parameter a binding reads from: input, output or both, for a warning when an output writes to an input. */
		uint8 DirectionOf(const UChooserTable& Table, const int32 ContextIndex)
		{
			TConstArrayView<FInstancedStruct> Context = Table.GetContextData();
			const FContextObjectTypeBase* Parameter = Context.IsValidIndex(ContextIndex) ? Context[ContextIndex].GetPtr<FContextObjectTypeBase>() : nullptr;
			return Parameter ? static_cast<uint8>(Parameter->Direction) : static_cast<uint8>(EContextObjectDirection::Read);
		}
	}

	const TArray<FColumnKindSpec>& ColumnKinds()
	{
		static const TArray<FColumnKindSpec> All = {
			{TEXT("bool"), TEXT("/Script/Chooser.BoolColumn"), TEXT("true, false, or any (empty)"), TEXT("filter")},
			{TEXT("enum"), TEXT("/Script/Chooser.EnumColumn"), TEXT("a value (Walking), !value for any other, or any (empty)"), TEXT("filter")},
			{TEXT("multi_enum"), TEXT("/Script/Chooser.MultiEnumColumn"), TEXT("values joined by | (Walking|Running): passes when the input is one of them; any (empty)"), TEXT("filter")},
			{TEXT("float_range"), TEXT("/Script/Chooser.FloatRangeColumn"), TEXT("min..max (0..300), ..max, min.., or any (empty)"), TEXT("filter")},
			{TEXT("float_distance"), TEXT("/Script/Chooser.FloatDistanceColumn"), TEXT("a number: the rows nearest the input score best"), TEXT("scoring")},
			{TEXT("gameplay_tag"), TEXT("/Script/Chooser.GameplayTagColumn"), TEXT("tags, comma separated (State.Crouch, State.Armed); empty for none"), TEXT("filter")},
			{TEXT("gameplay_tag_query"), TEXT("/Script/Chooser.GameplayTagQueryColumn"), TEXT("any:Tags, all:Tags or none:Tags, the tags comma separated; empty matches anything"), TEXT("filter")},
			{TEXT("object"), TEXT("/Script/Chooser.ObjectColumn"), TEXT("an object path, !path for any other, or any (empty)"), TEXT("filter")},
			{TEXT("object_class"), TEXT("/Script/Chooser.ObjectClassColumn"), TEXT("a class (the object is one or a subclass), =class (exactly it), !=class (anything else), !class (not one), or any (empty)"), TEXT("filter")},
			{TEXT("name"), TEXT("/Script/Chooser.ChooserNameColumn"), TEXT("a name, !name for any other, or any (empty)"), TEXT("filter")},
			{TEXT("randomize"), TEXT("/Script/Chooser.RandomizeColumn"), TEXT("a weight, 1 when empty: picks randomly among the rows that passed, a weight of 2 twice as often"), TEXT("random")},
			{TEXT("output_float"), TEXT("/Script/Chooser.OutputFloatColumn"), TEXT("a number written to the bound property"), TEXT("output")},
			{TEXT("output_bool"), TEXT("/Script/Chooser.OutputBoolColumn"), TEXT("true or false"), TEXT("output")},
			{TEXT("output_enum"), TEXT("/Script/Chooser.OutputEnumColumn"), TEXT("a value of the bound enum"), TEXT("output")},
			{TEXT("output_object"), TEXT("/Script/Chooser.OutputObjectColumn"), TEXT("an asset path, soft:path, chooser:path or nested:Name"), TEXT("output")},
			{TEXT("output_name"), TEXT("/Script/Chooser.ChooserOutputNameColumn"), TEXT("a name"), TEXT("output")},
			{TEXT("output_gameplay_tag"), TEXT("/Script/Chooser.OutputGameplayTagColumn"), TEXT("tags, comma separated"), TEXT("output")},
			{TEXT("pose_match"), TEXT("/Script/PoseSearch.PoseSearchColumn"), TEXT("left empty: motion matching picks among the rows' animations, through databases the Chooser editor keeps in the table"), TEXT("scoring")},
		};
		return All;
	}

	const FColumnKindSpec* FindColumnKind(const FString& Kind)
	{
		const FString Wanted = Lower(Kind);
		return ColumnKinds().FindByPredicate([&Wanted](const FColumnKindSpec& Spec) { return Wanted == Spec.Kind; });
	}

	const FColumnKindSpec* KindOfStruct(const UScriptStruct* Struct)
	{
		if (!Struct) return nullptr;
		const FString Path = Struct->GetStructPathName().ToString();
		return ColumnKinds().FindByPredicate([&Path](const FColumnKindSpec& Spec) { return Path == Spec.Struct; });
	}

	TArray<UScriptStruct*> AllColumnStructs()
	{
		TArray<UScriptStruct*> Out;
		const UScriptStruct* Base = FChooserColumnBase::StaticStruct();
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			if (*It != Base && It->IsChildOf(Base) && !It->HasMetaData(TEXT("Hidden"))) Out.Add(*It);
		}
		Out.Sort([](const UScriptStruct& A, const UScriptStruct& B) { return A.GetName() < B.GetName(); });
		return Out;
	}

	UScriptStruct* ColumnStruct(const FColumnKindSpec& Spec)
	{
		for (UScriptStruct* Struct : AllColumnStructs())
		{
			if (Struct->GetStructPathName().ToString() == Spec.Struct) return Struct;
		}
		return nullptr;
	}

	const TArray<FString>& OpKinds()
	{
		static const TArray<FString> All = {TEXT("create"), TEXT("column.add"), TEXT("column.set"), TEXT("column.remove"), TEXT("row.add"), TEXT("row.set"),
			TEXT("row.remove"), TEXT("fallback.set")};
		return All;
	}

	TArray<FString> DescribeOps()
	{
		return {
			TEXT("create: ResultType (object, class or none), OutputObjectType (a class), ContextClasses and ContextStructs (the parameters, Context0.. classes first), ContextDirections (input, output or both each). First, once."),
			TEXT("column.add: ColumnKind, Binding (the property it reads or writes: Speed, Context1.Speed), Params (settings), Cells (one per existing row). Filters go after the filters, outputs after them, randomize last."),
			TEXT("column.set: Column, and any of Binding, Params, Cells (one per row)."),
			TEXT("column.remove: Column."),
			TEXT("row.add: Result, Cells (one per column in column order, empty for any), Row (where; -1 at the end), Disabled."),
			TEXT("row.set: Row, and any of Result, Cells, Disabled."),
			TEXT("row.remove: Row."),
			TEXT("fallback.set: Result (none clears it), Cells (output columns' fallback values; filter columns empty)."),
		};
	}

	bool ParseSettings(const FString& Text, TArray<TPair<FString, FString>>& Out, FString& OutError)
	{
		Out.Reset();
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(";"), true);
		for (const FString& Part : Parts)
		{
			const FString Trimmed = Part.TrimStartAndEnd();
			if (Trimmed.IsEmpty()) continue;
			FString Name, Value;
			if (!Trimmed.Split(TEXT("="), &Name, &Value) || Name.TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not Name=Value; write \"Name=Value; Name=Value\"."), *Trimmed);
				return false;
			}
			Out.Emplace(Name.TrimStartAndEnd(), Value.TrimStartAndEnd());
		}
		return true;
	}

	bool ParseNumber(const FString& Text, double& Out)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !FCString::IsNumeric(*Trimmed)) return false;
		Out = FCString::Atod(*Trimmed);
		return FMath::IsFinite(Out);
	}

	bool ParseBoolCell(const FString& Text, uint8& Out, FString& OutError)
	{
		const FString Word = Lower(Text);
		if (IsAny(Word)) Out = 2;
		else if (Word == TEXT("true") || Word == TEXT("yes") || Word == TEXT("on") || Word == TEXT("1")) Out = 1;
		else if (Word == TEXT("false") || Word == TEXT("no") || Word == TEXT("off") || Word == TEXT("0")) Out = 0;
		else
		{
			OutError = FString::Printf(TEXT("'%s' is not a bool cell; write true, false or any."), *Text);
			return false;
		}
		return true;
	}

	bool ParseRange(const FString& Text, double& OutMin, double& OutMax, bool& bOutNoMin, bool& bOutNoMax, FString& OutError)
	{
		OutMin = OutMax = 0.0;
		bOutNoMin = bOutNoMax = true;
		const FString Trimmed = Text.TrimStartAndEnd();
		if (IsAny(Trimmed)) return true;
		FString Left, Right;
		if (!Trimmed.Split(TEXT(".."), &Left, &Right))
		{
			OutError = FString::Printf(TEXT("'%s' is not a range; write min..max (0..300), ..max, min.. or any."), *Trimmed);
			return false;
		}
		Left.TrimStartAndEndInline();
		Right.TrimStartAndEndInline();
		if (Left.IsEmpty() && Right.IsEmpty()) return true;
		if (!Left.IsEmpty())
		{
			if (!ParseNumber(Left, OutMin))
			{
				OutError = FString::Printf(TEXT("'%s' in '%s' is not a number."), *Left, *Trimmed);
				return false;
			}
			bOutNoMin = false;
		}
		if (!Right.IsEmpty())
		{
			if (!ParseNumber(Right, OutMax))
			{
				OutError = FString::Printf(TEXT("'%s' in '%s' is not a number."), *Right, *Trimmed);
				return false;
			}
			bOutNoMax = false;
		}
		// The open end takes the closed end's number: a wrapped column reads Max < Min as a range across the wrap.
		if (bOutNoMax && !bOutNoMin) OutMax = OutMin;
		if (bOutNoMin && !bOutNoMax) OutMin = OutMax;
		return true;
	}

	FString FormatRange(const double Min, const double Max, const bool bNoMin, const bool bNoMax)
	{
		if (bNoMin && bNoMax) return TEXT("any");
		return FString::Printf(TEXT("%s..%s"), bNoMin ? TEXT("") : *Number(Min), bNoMax ? TEXT("") : *Number(Max));
	}

	int32 ParseComparison(const FString& Text, FString& OutValue)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (IsAny(Trimmed))
		{
			OutValue.Reset();
			return 2;
		}
		if (Trimmed.StartsWith(TEXT("!")))
		{
			OutValue = Trimmed.Mid(1).TrimStartAndEnd();
			return 1;
		}
		OutValue = Trimmed;
		return 0;
	}

	int32 FindEnumIndex(const UEnum& Enum, const FString& Text)
	{
		const FString Wanted = Text.TrimStartAndEnd();
		if (Wanted.IsEmpty()) return INDEX_NONE;
		const int32 Count = Enum.NumEnums() - (Enum.ContainsExistingMax() ? 1 : 0);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Enum.GetNameStringByIndex(Index).Equals(Wanted, ESearchCase::IgnoreCase) || Enum.GetNameByIndex(Index).ToString().Equals(Wanted, ESearchCase::IgnoreCase)
				|| Enum.GetDisplayNameTextByIndex(Index).ToString().Replace(TEXT(" "), TEXT("")).Equals(Wanted.Replace(TEXT(" "), TEXT("")), ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		// MOVE_Walking also as Walking: a prefix the enum's values share is dropped.
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FString Short = Enum.GetNameStringByIndex(Index);
			int32 Underscore = INDEX_NONE;
			if (Short.FindChar(TEXT('_'), Underscore) && Short.Mid(Underscore + 1).Equals(Wanted, ESearchCase::IgnoreCase)) return Index;
		}
		return INDEX_NONE;
	}

	FString EnumWords(const UEnum& Enum)
	{
		TArray<FString> Names;
		const int32 Count = Enum.NumEnums() - (Enum.ContainsExistingMax() ? 1 : 0);
		for (int32 Index = 0; Index < Count && Names.Num() < 64; ++Index) Names.Add(Enum.GetNameStringByIndex(Index));
		return FString::Join(Names, TEXT(", "));
	}

	FStructProperty* BindingPropertyOf(const UScriptStruct* InputStruct)
	{
		if (!InputStruct) return nullptr;
		FStructProperty* Property = FindFProperty<FStructProperty>(InputStruct, TEXT("Binding"));
		return Property && Property->Struct && Property->Struct->IsChildOf(FChooserPropertyBinding::StaticStruct()) ? Property : nullptr;
	}

	const FChooserPropertyBinding* BindingOf(const FInstancedStruct& Column)
	{
		if (!Column.IsValid()) return nullptr;
		FInstancedStruct* Input = const_cast<FInstancedStruct&>(Column).GetMutable<FChooserColumnBase>().GetInputValuePtr();
		return Input ? MutableBindingOf(*Input) : nullptr;
	}

	FString BindingTypeOf(const FInstancedStruct& Column)
	{
		if (!Column.IsValid()) return FString();
		FChooserColumnBase& Base = const_cast<FInstancedStruct&>(Column).GetMutable<FChooserColumnBase>();
		const FInstancedStruct* Input = Base.GetInputValuePtr();
		const UScriptStruct* Type = Input && Input->IsValid() ? Input->GetScriptStruct() : Base.GetInputBaseType();
		const FStructProperty* Property = BindingPropertyOf(Type);
		return Property ? Property->GetMetaData(TEXT("BindingType")) : FString();
	}

	bool CanBind(const FProperty& Property, const FString& TypeFilter)
	{
		if (TypeFilter.IsEmpty()) return true;
		if (TypeFilter == TEXT("struct")) return CastField<FStructProperty>(&Property) != nullptr;
		if (TypeFilter == TEXT("object")) return CastField<FObjectPropertyBase>(&Property) != nullptr;
		if (TypeFilter == TEXT("double"))
		{
			const FString Type = Property.GetCPPType();
			return Type == TEXT("float") || Type == TEXT("double") || Type == TEXT("int32");
		}
		if (TypeFilter == TEXT("enum"))
		{
			if (CastField<FEnumProperty>(&Property)) return true;
			const FByteProperty* Byte = CastField<FByteProperty>(&Property);
			return Byte && Byte->Enum;
		}
		if (TypeFilter == TEXT("bool")) return CastField<FBoolProperty>(&Property) != nullptr;
		const FString Type = Property.GetCPPType();
		const FString Alternate = TypeFilter.EndsWith(TEXT("*")) ? FString::Printf(TEXT("TObjectPtr<%s>"), *TypeFilter.LeftChop(1)) : FString();
		return Type == TypeFilter || (!Alternate.IsEmpty() && Type == Alternate);
	}

	bool ResolveBinding(const UChooserTable& Table, const FString& Path, const FString& TypeFilter, FBindingPath& Out, FString& OutError)
	{
		Out = FBindingPath();
		TConstArrayView<FInstancedStruct> Context = Table.GetContextData();
		TArray<FString> Parts;
		Path.TrimStartAndEnd().ParseIntoArray(Parts, TEXT("."), true);
		for (FString& Part : Parts) Part.TrimStartAndEndInline();
		if (Parts.IsEmpty())
		{
			OutError = TEXT("give Binding, the property the column reads or writes: Speed, or Context1.Speed.");
			return false;
		}
		for (const FString& Part : Parts)
		{
			if (!FHyperAIStudioNames::IsName(Part, TEXT("Binding"), &OutError)) return false;
		}
		if (Context.IsEmpty())
		{
			OutError = TEXT("the table has no parameters to bind to; a table made with create takes ContextClasses or ContextStructs.");
			return false;
		}
		auto Listing = [&Context, &TypeFilter]()
		{
			TArray<FString> Lines;
			for (int32 Index = 0; Index < Context.Num(); ++Index)
			{
				const UStruct* Type = ParameterType(Context[Index]);
				if (Type) Lines.Add(FString::Printf(TEXT("Context%d %s (%s)"), Index, *Type->GetName(), *BindableNames(*Type, TypeFilter)));
			}
			return FString::Join(Lines, TEXT("; "));
		};
		int32 First = 0;
		Out.ContextIndex = INDEX_NONE;
		if (Parts[0].StartsWith(TEXT("Context"), ESearchCase::IgnoreCase) && Parts[0].Len() > 7 && Parts[0].Mid(7).IsNumeric())
		{
			Out.ContextIndex = FCString::Atoi(*Parts[0].Mid(7));
			if (!Context.IsValidIndex(Out.ContextIndex) || !ParameterType(Context[Out.ContextIndex]))
			{
				OutError = FString::Printf(TEXT("%s is not a parameter of the table; the parameters are %s."), *Parts[0], *Listing());
				return false;
			}
			First = 1;
		}
		else
		{
			for (int32 Index = 0; Index < Context.Num() && Out.ContextIndex == INDEX_NONE; ++Index)
			{
				const UStruct* Type = ParameterType(Context[Index]);
				if (Type && Parts.Num() > 1 && (Type->GetName().Equals(Parts[0], ESearchCase::IgnoreCase) || CppName(*Type).Equals(Parts[0], ESearchCase::IgnoreCase)))
				{
					Out.ContextIndex = Index;
					First = 1;
				}
			}
			for (int32 Index = 0; Index < Context.Num() && Out.ContextIndex == INDEX_NONE; ++Index)
			{
				const UStruct* Type = ParameterType(Context[Index]);
				if (Type && FindFProperty<FProperty>(Type, *Parts[0])) Out.ContextIndex = Index;
			}
			if (Out.ContextIndex == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("no parameter has a property '%s'; the parameters and the properties this column can bind are %s."), *Parts[0], *Listing());
				return false;
			}
		}
		const UStruct* Type = ParameterType(Context[Out.ContextIndex]);
		Out.Text = FString::Printf(TEXT("Context%d"), Out.ContextIndex);
		if (First == Parts.Num())
		{
			const bool bStruct = Context[Out.ContextIndex].GetPtr<FContextObjectTypeStruct>() != nullptr;
			if (!bStruct || !(TypeFilter == TEXT("struct") || CppName(*Type) == TypeFilter))
			{
				OutError = FString::Printf(TEXT("%s is bound to a property, not a whole parameter: write %s.Property (%s)."), *Out.Text, *Out.Text, *BindableNames(*Type, TypeFilter));
				return false;
			}
			Out.bRoot = true;
			return true;
		}
		for (int32 Index = First; Index < Parts.Num(); ++Index)
		{
			const FProperty* Property = FindFProperty<FProperty>(Type, *Parts[Index]);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("%s has no property '%s'; the ones this column can bind are %s."), *Type->GetName(), *Parts[Index], *BindableNames(*Type, TypeFilter));
				return false;
			}
			if (!Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
			{
				OutError = FString::Printf(TEXT("%s.%s is not visible to Blueprints, and the Chooser binds only Blueprint-visible properties."), *Type->GetName(), *Property->GetName());
				return false;
			}
			Out.Chain.Add(Property->GetFName());
			Out.Text += TEXT(".") + Property->GetName();
			if (Index < Parts.Num() - 1)
			{
				const FStructProperty* Struct = CastField<FStructProperty>(Property);
				if (!Struct)
				{
					OutError = FString::Printf(TEXT("%s is not a struct: a binding goes through struct members only, as the Chooser editor allows (object references are not followed)."), *Property->GetName());
					return false;
				}
				Type = Struct->Struct;
				continue;
			}
			if (!CanBind(*Property, TypeFilter))
			{
				OutError = FString::Printf(TEXT("%s is a %s, and this column binds a %s property; on %s those are %s."), *Out.Text, *Property->GetCPPType(), *TypeFilter, *Type->GetName(),
					*BindableNames(*Type, TypeFilter));
				return false;
			}
			Out.Leaf = Property;
		}
		return true;
	}

	FString DescribeBinding(const FChooserPropertyBinding& Binding)
	{
		if (!IsBound(Binding)) return FString();
		FString Text = FString::Printf(TEXT("Context%d"), Binding.ContextIndex);
		for (const FName& Name : Binding.PropertyBindingChain) Text += TEXT(".") + Name.ToString();
		return Text;
	}

	FString BindingProblem(const UChooserTable& Table, const FInstancedStruct& Column)
	{
		const FChooserPropertyBinding* Binding = BindingOf(Column);
		if (!Binding || !IsBound(*Binding)) return FString();
		FBindingPath Ignored;
		FString Error;
		return ResolveBinding(Table, DescribeBinding(*Binding), BindingTypeOf(Column), Ignored, Error) ? FString() : Error;
	}

	FString KindOf(const FInstancedStruct& Column)
	{
		if (!Column.IsValid()) return TEXT("none");
		const FColumnKindSpec* Spec = KindOfStruct(Column.GetScriptStruct());
		return Spec ? FString(Spec->Kind) : Column.GetScriptStruct()->GetName();
	}

	FString RoleOf(const FInstancedStruct& Column)
	{
		if (!Column.IsValid()) return TEXT("none");
		const FChooserColumnBase& Base = Column.Get<FChooserColumnBase>();
		if (Base.IsRandomizeColumn()) return TEXT("random");
		if (Base.HasCosts()) return TEXT("scoring");
		if (Base.HasOutputs() && !Base.HasFilters()) return TEXT("output");
		return TEXT("filter");
	}

	FString CellText(const FInstancedStruct& Column, const int32 Row)
	{
		if (!Column.IsValid()) return FString();
		const bool bFallback = Row == ChooserColumn_SpecialIndex_Fallback;
		auto Valid = [Row, bFallback](const auto& Array) { return bFallback || Array.IsValidIndex(Row); };
		if (const FBoolColumn* Bool = Column.GetPtr<FBoolColumn>())
		{
			if (bFallback || !Bool->RowValuesWithAny.IsValidIndex(Row)) return FString();
			const EBoolColumnCellValue Value = Bool->RowValuesWithAny[Row];
			return Value == EBoolColumnCellValue::MatchAny ? TEXT("any") : Value == EBoolColumnCellValue::MatchTrue ? TEXT("true") : TEXT("false");
		}
		if (const FFloatRangeColumn* Range = Column.GetPtr<FFloatRangeColumn>())
		{
			if (bFallback || !Range->RowValues.IsValidIndex(Row)) return FString();
			const FChooserFloatRangeRowData& Cell = Range->RowValues[Row];
			return FormatRange(Cell.Min, Cell.Max, Cell.bNoMin, Cell.bNoMax);
		}
		if (const FFloatDistanceColumn* Distance = Column.GetPtr<FFloatDistanceColumn>())
		{
			return !bFallback && Distance->RowValues.IsValidIndex(Row) ? Number(Distance->RowValues[Row].Value) : FString();
		}
		const UEnum* Enum = EnumOfColumn(Column);
		auto EnumName = [Enum](const uint8 Value, const FName Name)
		{
			if (Enum)
			{
				const int32 Index = Enum->GetIndexByValue(Value);
				if (Index != INDEX_NONE) return Enum->GetNameStringByIndex(Index);
			}
			return Name.IsNone() ? FString::FromInt(Value) : Name.ToString();
		};
		if (const FEnumColumn* EnumColumn = Column.GetPtr<FEnumColumn>())
		{
			if (bFallback || !EnumColumn->RowValues.IsValidIndex(Row)) return FString();
			const FChooserEnumRowData& Cell = EnumColumn->RowValues[Row];
			if (Cell.Comparison == EEnumColumnCellValueComparison::MatchAny) return TEXT("any");
			return (Cell.Comparison == EEnumColumnCellValueComparison::MatchNotEqual ? TEXT("!") : TEXT("")) + EnumName(Cell.Value, Cell.ValueName);
		}
		if (const FMultiEnumColumn* Multi = Column.GetPtr<FMultiEnumColumn>())
		{
			if (bFallback || !Multi->RowValues.IsValidIndex(Row)) return FString();
			const uint32 Mask = Multi->RowValues[Row].Value;
			if (Mask == 0) return TEXT("any");
			TArray<FString> Names;
			for (uint32 Bit = 0; Bit < 32; ++Bit)
			{
				if (Mask & (1u << Bit)) Names.Add(EnumName(static_cast<uint8>(Bit), NAME_None));
			}
			return FString::Join(Names, TEXT("|"));
		}
		if (const FGameplayTagColumn* Tags = Column.GetPtr<FGameplayTagColumn>())
		{
			return !bFallback && Tags->RowValues.IsValidIndex(Row) ? TagsText(Tags->RowValues[Row]) : FString();
		}
		if (const FGameplayTagQueryColumn* Query = Column.GetPtr<FGameplayTagQueryColumn>())
		{
			if (bFallback || !Query->RowValues.IsValidIndex(Row)) return FString();
			const FGameplayTagQuery& Cell = Query->RowValues[Row];
			if (Cell.IsEmpty()) return TEXT("any");
			return !Cell.GetDescription().IsEmpty() ? Cell.GetDescription() : StructText(*FGameplayTagQuery::StaticStruct(), &Cell);
		}
		if (const FObjectColumn* Object = Column.GetPtr<FObjectColumn>())
		{
			if (bFallback || !Object->RowValues.IsValidIndex(Row)) return FString();
			const FChooserObjectRowData& Cell = Object->RowValues[Row];
			if (Cell.Comparison == EObjectColumnCellValueComparison::MatchAny) return TEXT("any");
			return (Cell.Comparison == EObjectColumnCellValueComparison::MatchNotEqual ? TEXT("!") : TEXT("")) + Cell.Value.ToString();
		}
		if (const FObjectClassColumn* Class = Column.GetPtr<FObjectClassColumn>())
		{
			if (bFallback || !Class->RowValues.IsValidIndex(Row)) return FString();
			const FChooserObjectClassRowData& Cell = Class->RowValues[Row];
			const FString Path = Cell.Value ? Cell.Value->GetPathName() : FString(TEXT("None"));
			switch (Cell.Comparison)
			{
			case EObjectClassColumnCellValueComparison::Equal: return TEXT("=") + Path;
			case EObjectClassColumnCellValueComparison::NotEqual: return TEXT("!=") + Path;
			case EObjectClassColumnCellValueComparison::NotSubClassOf: return TEXT("!") + Path;
			case EObjectClassColumnCellValueComparison::Any: return TEXT("any");
			default: return Path;
			}
		}
		if (const FChooserNameColumn* Name = Column.GetPtr<FChooserNameColumn>())
		{
			if (bFallback || !Name->RowValues.IsValidIndex(Row)) return FString();
			const FChooserNameRowData& Cell = Name->RowValues[Row];
			if (Cell.Comparison == ENameColumnCellValueComparison::MatchAny) return TEXT("any");
			return (Cell.Comparison == ENameColumnCellValueComparison::MatchNotEqual ? TEXT("!") : TEXT("")) + Cell.Value.ToString();
		}
		if (const FRandomizeColumn* Random = Column.GetPtr<FRandomizeColumn>())
		{
			return !bFallback && Random->RowValues.IsValidIndex(Row) ? Number(Random->RowValues[Row]) : FString();
		}
		if (const FOutputFloatColumn* OutFloat = Column.GetPtr<FOutputFloatColumn>())
		{
			return Valid(OutFloat->RowValues) ? Number(OutFloat->GetValueForIndex(Row)) : FString();
		}
		if (const FOutputBoolColumn* OutBool = Column.GetPtr<FOutputBoolColumn>())
		{
			return Valid(OutBool->RowValues) ? FString(OutBool->GetValueForIndex(Row) ? TEXT("true") : TEXT("false")) : FString();
		}
		if (const FOutputEnumColumn* OutEnum = Column.GetPtr<FOutputEnumColumn>())
		{
			if (!Valid(OutEnum->RowValues)) return FString();
			const FChooserOutputEnumRowData& Cell = OutEnum->GetValueForIndex(Row);
			return EnumName(Cell.Value, Cell.ValueName);
		}
		if (const FOutputObjectColumn* OutObject = Column.GetPtr<FOutputObjectColumn>())
		{
			if (!Valid(OutObject->RowValues)) return FString();
			const FInstancedStruct& Value = OutObject->GetValueForIndex(Row).Value;
			return Value.IsValid() ? ResultText(Value) : FString(TEXT("none"));
		}
		if (const FChooserOutputNameColumn* OutName = Column.GetPtr<FChooserOutputNameColumn>())
		{
			return Valid(OutName->RowValues) ? OutName->GetValueForIndex(Row).ToString() : FString();
		}
		if (const FOutputGameplayTagColumn* OutTags = Column.GetPtr<FOutputGameplayTagColumn>())
		{
			return Valid(OutTags->RowValues) ? TagsText(OutTags->GetValueForIndex(Row)) : FString();
		}
		if (bFallback) return FString();
		// Any other column (Pose Match, Output Struct...): its row array's element in Unreal's text form.
		const FArrayProperty* Array = FindFProperty<FArrayProperty>(Column.GetScriptStruct(), RowValuesName(Column));
		if (!Array) return FString();
		FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Column.GetMemory()));
		if (!Helper.IsValidIndex(Row)) return FString();
		const uint8* Element = Helper.GetRawPtr(Row);
		if (const FStructProperty* Inner = CastField<FStructProperty>(Array->Inner))
		{
			// A Pose Match cell: the database the row is matched in.
			if (const FObjectPropertyBase* Database = FindFProperty<FObjectPropertyBase>(Inner->Struct, TEXT("Database")))
			{
				const UObject* Value = Database->GetObjectPropertyValue_InContainer(Element);
				return Value ? Value->GetPathName() : FString();
			}
		}
		FString Out;
		Array->Inner->ExportText_Direct(Out, Element, Element, nullptr, PPF_None);
		return Out.Left(300);
	}

	FString ResultText(const FInstancedStruct& Result)
	{
		if (!Result.IsValid()) return TEXT("none");
		if (const FAssetChooser* Asset = Result.GetPtr<FAssetChooser>()) return TEXT("asset ") + (Asset->Asset ? Asset->Asset->GetPathName() : FString(TEXT("None")));
		if (const FSoftAssetChooser* Soft = Result.GetPtr<FSoftAssetChooser>()) return TEXT("soft asset ") + Soft->Asset.ToString();
		if (const FClassChooser* Class = Result.GetPtr<FClassChooser>()) return TEXT("class ") + (Class->Class ? Class->Class->GetPathName() : FString(TEXT("None")));
		if (const FEvaluateChooser* Evaluate = Result.GetPtr<FEvaluateChooser>()) return TEXT("chooser ") + (Evaluate->Chooser ? Evaluate->Chooser->GetPathName() : FString(TEXT("None")));
		if (const FNestedChooser* Nested = Result.GetPtr<FNestedChooser>()) return TEXT("nested ") + (Nested->Chooser ? Nested->Chooser->GetName() : FString(TEXT("None")));
		FString Name;
		Result.Get<FObjectChooserBase>().GetDebugName(Name);
		return Result.GetScriptStruct()->GetName() + (Name.IsEmpty() ? FString() : TEXT(" ") + Name);
	}

	TArray<FString> SettingsOf(const FInstancedStruct& Column, UObject* Owner)
	{
		TArray<FString> Lines;
		if (!Column.IsValid()) return Lines;
		const FName RowValues = RowValuesName(Column);
		for (TFieldIterator<FProperty> It(Column.GetScriptStruct()); It; ++It)
		{
			if (!IsSetting(**It, RowValues)) continue;
			FString Value;
			// The container as its own delta: against null, a value equal to its type's zero exports as nothing.
			It->ExportText_InContainer(0, Value, Column.GetMemory(), Column.GetMemory(), Owner, PPF_None);
			Lines.Add(FString::Printf(TEXT("%s = %s"), *It->GetName(), *Value.Left(200)));
		}
		return Lines;
	}

	TArray<FString> DescribeSettings(const UScriptStruct& Struct)
	{
		FInstancedStruct Default;
		Default.InitializeAs(&Struct);
		TArray<FString> Lines;
		const FName RowValues = RowValuesName(Default);
		for (TFieldIterator<FProperty> It(&Struct); It; ++It)
		{
			if (!IsSetting(**It, RowValues)) continue;
			FString Value;
			It->ExportText_InContainer(0, Value, Default.GetMemory(), Default.GetMemory(), nullptr, PPF_None);
			Lines.Add(FString::Printf(TEXT("%s: %s, default %s"), *It->GetName(), *It->GetCPPType(), *Value.Left(120)));
		}
		return Lines;
	}

	FString DirectionText(const uint8 Direction)
	{
		switch (static_cast<EContextObjectDirection>(Direction))
		{
		case EContextObjectDirection::Write: return TEXT("output");
		case EContextObjectDirection::ReadWrite: return TEXT("both");
		default: return TEXT("input");
		}
	}

	TArray<FString> Warn(const UChooserTable& Table)
	{
		TArray<FString> Out;
		bool bFilters = false;
		for (int32 Index = 0; Index < Table.ColumnsStructs.Num(); ++Index)
		{
			const FInstancedStruct& Column = Table.ColumnsStructs[Index];
			if (!Column.IsValid())
			{
				Out.Add(FString::Printf(TEXT("Column %d holds nothing; remove it."), Index));
				continue;
			}
			const FChooserColumnBase& Base = Column.Get<FChooserColumnBase>();
			if (Base.bDisabled) continue;
			const FString Role = RoleOf(Column);
			if (Role == TEXT("filter") || Role == TEXT("scoring")) bFilters = true;
			const FChooserPropertyBinding* Binding = BindingOf(Column);
			if (Binding && !IsBound(*Binding) && !Base.IsRandomizeColumn())
			{
				Out.Add(FString::Printf(TEXT("Column %d (%s) is not bound: %s."), Index, *KindOf(Column),
					Role == TEXT("output") ? TEXT("it writes nothing") : TEXT("it lets every row through")));
				continue;
			}
			const FString Problem = BindingProblem(Table, Column);
			if (!Problem.IsEmpty()) Out.Add(FString::Printf(TEXT("Column %d (%s): %s"), Index, *KindOf(Column), *Problem));
			if (Binding && IsBound(*Binding))
			{
				const EContextObjectDirection Direction = static_cast<EContextObjectDirection>(DirectionOf(Table, Binding->ContextIndex));
				if (Role == TEXT("output") && Direction == EContextObjectDirection::Read)
				{
					Out.Add(FString::Printf(TEXT("Column %d (%s) writes to Context%d, an input parameter; make it output or both so callers read the value back."), Index,
						*KindOf(Column), Binding->ContextIndex));
				}
			}
		}
		if (bFilters && !Table.FallbackResult.IsValid() && Table.GetContextOwner()->ResultType != EObjectChooserResultType::NoPrimaryResult)
		{
			Out.Add(TEXT("No fallback: when no row passes, the table gives nothing (fallback.set gives it one)."));
		}
		for (int32 Row = 0; Row < Table.ResultsStructs.Num(); ++Row)
		{
			const FInstancedStruct& Result = Table.ResultsStructs[Row];
			const FAssetChooser* Asset = Result.GetPtr<FAssetChooser>();
			const FEvaluateChooser* Evaluate = Result.GetPtr<FEvaluateChooser>();
			if (!Result.IsValid() || (Asset && !Asset->Asset) || (Evaluate && !Evaluate->Chooser))
			{
				Out.Add(FString::Printf(TEXT("Row %d gives nothing: the table moves on to the next row that passes."), Row));
			}
		}
		if (Table.ResultsStructs.IsEmpty()) Out.Add(TEXT("The table has no rows yet (row.add)."));
		return Out;
	}

	void CompileTable(UChooserTable& Table)
	{
		IHasContextClass* Owner = Table.GetRootChooser();
		for (FInstancedStruct& Column : Table.ColumnsStructs)
		{
			if (!Column.IsValid()) continue;
			// An input left unbound (a Randomize column without its optional context) would log an error for nothing.
			const FChooserPropertyBinding* Binding = BindingOf(Column);
			if (Binding && !IsBound(*Binding)) continue;
			Column.GetMutable<FChooserColumnBase>().Compile(Owner, /*bForce=*/true);
		}
		for (FInstancedStruct& Result : Table.ResultsStructs)
		{
			if (Result.IsValid()) Result.GetMutable<FObjectChooserBase>().Compile(Owner, true);
		}
		if (Table.FallbackResult.IsValid()) Table.FallbackResult.GetMutable<FObjectChooserBase>().Compile(Owner, true);
		if (&Table == Table.GetRootChooser())
		{
			for (const TObjectPtr<UObject>& Nested : Table.NestedObjects)
			{
				if (UChooserTable* NestedTable = Cast<UChooserTable>(Nested.Get())) CompileTable(*NestedTable);
			}
		}
	}

	UChooserTable* UndoneAt(const FString& PackagePath)
	{
		if (FPackageName::DoesPackageExist(PackagePath)) return nullptr;
		UPackage* Package = FindPackage(nullptr, *PackagePath);
		UObject* There = Package ? StaticFindObjectFast(UObject::StaticClass(), Package, FName(*FPackageName::GetShortName(PackagePath)), EFindObjectFlags::None,
			RF_NoFlags, EInternalObjectFlags::None) : nullptr;
		UChooserTable* Table = Cast<UChooserTable>(There);
		return Table && !IsValid(Table) ? Table : nullptr;
	}

	UChooserTable* LoadTable(const FString& Path, FString& OutError)
	{
		UObject* Found = LoadQuiet(Path, TEXT("Asset"), OutError);
		UChooserTable* Table = Cast<UChooserTable>(Found);
		if (Found && !Table) OutError = FString::Printf(TEXT("%s is a %s, not a chooser table."), *Path.TrimStartAndEnd(), *Found->GetClass()->GetName());
		else if (!Found) OutError = FString::Printf(TEXT("%s is not a chooser table."), *Path.TrimStartAndEnd());
		return Table && IsValid(Table) ? Table : nullptr;
	}

	bool Apply(UChooserTable& Table, const TArray<FHyperAIChooserOp>& Operations, const bool bCreate, const bool bReal, TArray<FString>& OutNotes, FString& OutError)
	{
		if (bReal) Table.Modify();
		for (int32 Index = 0; Index < Operations.Num(); ++Index)
		{
			const FHyperAIChooserOp& Op = Operations[Index];
			const FString Kind = Lower(Op.Kind);
			auto Fail = [&OutError, Index, &Kind](const FString& Message)
			{
				OutError = FString::Printf(TEXT("Operation %d (%s): %s"), Index, *Kind, *Message);
				return false;
			};
			if (!OpKinds().Contains(Kind))
			{
				return Fail(FString::Printf(TEXT("'%s' is not an op kind; the kinds are %s."), *Op.Kind, *FString::Join(OpKinds(), TEXT(", "))));
			}
			const bool bCreateFields = !Op.ResultType.IsEmpty() || !Op.OutputObjectType.IsEmpty() || !Op.ContextClasses.IsEmpty() || !Op.ContextStructs.IsEmpty()
				|| !Op.ContextDirections.IsEmpty();
			if (bCreateFields && Kind != TEXT("create")) return Fail(TEXT("only create takes ResultType, OutputObjectType and the Context fields."));
			if ((!Op.ColumnKind.IsEmpty() || !Op.Binding.IsEmpty() || !Op.Params.IsEmpty()) && Kind != TEXT("column.add") && Kind != TEXT("column.set"))
			{
				return Fail(TEXT("only column.add and column.set take ColumnKind, Binding and Params."));
			}
			if (!Op.Result.IsEmpty() && Kind != TEXT("row.add") && Kind != TEXT("row.set") && Kind != TEXT("fallback.set")) return Fail(TEXT("only row.add, row.set and fallback.set take Result."));
			if (!Op.Disabled.IsEmpty() && Kind != TEXT("row.add") && Kind != TEXT("row.set")) return Fail(TEXT("only row.add and row.set take Disabled; a column is disabled with Params Disabled=true."));
			if (!Op.Cells.IsEmpty() && (Kind == TEXT("create") || Kind == TEXT("column.remove") || Kind == TEXT("row.remove"))) return Fail(TEXT("this op takes no Cells."));
			bool bDisabled = false;
			if (!Op.Disabled.IsEmpty())
			{
				uint8 Value = 2;
				FString Ignored;
				if (!ParseBoolCell(Op.Disabled, Value, Ignored) || Value == 2) return Fail(TEXT("Disabled is true or false."));
				bDisabled = Value == 1;
			}
			const int32 Rows = Table.ResultsStructs.Num();
			const int32 Columns = Table.ColumnsStructs.Num();
			FString Why;
			auto ColumnList = [&Table]()
			{
				TArray<FString> Names;
				for (int32 Each = 0; Each < Table.ColumnsStructs.Num(); ++Each) Names.Add(FString::Printf(TEXT("%d %s"), Each, *KindOf(Table.ColumnsStructs[Each])));
				return Names.IsEmpty() ? FString(TEXT("no columns")) : FString::Join(Names, TEXT(", "));
			};

			if (Kind == TEXT("create"))
			{
				if (Index != 0 || !bCreate) return Fail(TEXT("create comes first, once, and only for a table that is not there yet."));
				const FString ResultType = Lower(Op.ResultType);
				EObjectChooserResultType Type = EObjectChooserResultType::ObjectResult;
				if (ResultType == TEXT("class")) Type = EObjectChooserResultType::ClassResult;
				else if (ResultType == TEXT("none")) Type = EObjectChooserResultType::NoPrimaryResult;
				else if (!ResultType.IsEmpty() && ResultType != TEXT("object")) return Fail(TEXT("ResultType is object, class or none."));
				UClass* Output = UObject::StaticClass();
				if (!Op.OutputObjectType.TrimStartAndEnd().IsEmpty())
				{
					Output = LoadClassQuiet(Op.OutputObjectType, TEXT("OutputObjectType"), Why);
					if (!Output) return Fail(Why.IsEmpty() ? FString::Printf(TEXT("%s is not a class."), *Op.OutputObjectType) : Why);
				}
				if (Type == EObjectChooserResultType::NoPrimaryResult) Output = UClass::StaticClass();
				const int32 Parameters = Op.ContextClasses.Num() + Op.ContextStructs.Num();
				if (Parameters > C::MaxParameters || Op.ContextClasses.Num() > C::MaxObjectParameters)
				{
					return Fail(FString::Printf(TEXT("a table takes at most %d parameters, %d of them classes."), C::MaxParameters, C::MaxObjectParameters));
				}
				if (Op.ContextDirections.Num() > Parameters) return Fail(TEXT("ContextDirections has more entries than there are parameters."));
				TArray<EContextObjectDirection> Directions;
				for (int32 Each = 0; Each < Parameters; ++Each)
				{
					const FString Word = Op.ContextDirections.IsValidIndex(Each) ? Lower(Op.ContextDirections[Each]) : FString();
					if (Word.IsEmpty() || Word == TEXT("input")) Directions.Add(EContextObjectDirection::Read);
					else if (Word == TEXT("output")) Directions.Add(EContextObjectDirection::Write);
					else if (Word == TEXT("both")) Directions.Add(EContextObjectDirection::ReadWrite);
					else return Fail(FString::Printf(TEXT("ContextDirections[%d] '%s' is input, output or both."), Each, *Op.ContextDirections[Each]));
				}
				TArray<FInstancedStruct> Context;
				TArray<FString> Described;
				for (const FString& Path : Op.ContextClasses)
				{
					UClass* Class = LoadClassQuiet(Path, TEXT("ContextClasses"), Why);
					if (!Class) return Fail(Why.IsEmpty() ? FString::Printf(TEXT("%s is not a class."), *Path) : Why);
					FInstancedStruct& Entry = Context.AddDefaulted_GetRef();
					Entry.InitializeAs(FContextObjectTypeClass::StaticStruct());
					Entry.GetMutable<FContextObjectTypeClass>().Class = Class;
					Entry.GetMutable<FContextObjectTypeClass>().Direction = Directions[Context.Num() - 1];
					Described.Add(FString::Printf(TEXT("Context%d class %s (%s)"), Context.Num() - 1, *Class->GetName(), *DirectionText(static_cast<uint8>(Directions[Context.Num() - 1]))));
				}
				for (const FString& Path : Op.ContextStructs)
				{
					UScriptStruct* Struct = LoadStructQuiet(Path, Why);
					if (!Struct) return Fail(Why.IsEmpty() ? FString::Printf(TEXT("%s is not a struct."), *Path) : Why);
					FInstancedStruct& Entry = Context.AddDefaulted_GetRef();
					Entry.InitializeAs(FContextObjectTypeStruct::StaticStruct());
					Entry.GetMutable<FContextObjectTypeStruct>().Struct = Struct;
					Entry.GetMutable<FContextObjectTypeStruct>().Direction = Directions[Context.Num() - 1];
					Described.Add(FString::Printf(TEXT("Context%d struct %s (%s)"), Context.Num() - 1, *Struct->GetName(), *DirectionText(static_cast<uint8>(Directions[Context.Num() - 1]))));
				}
				Table.ResultType = Type;
				Table.OutputObjectType = Output;
				Table.ContextData = MoveTemp(Context);
				Table.Version = UChooserTable::CurrentVersion;
				OutNotes.Add(FString::Printf(TEXT("Operation %d (create): a table giving %s, with %s."), Index,
					Type == EObjectChooserResultType::NoPrimaryResult ? TEXT("no result, only outputs")
					: *FString::Printf(TEXT("%s %s"), Type == EObjectChooserResultType::ClassResult ? TEXT("subclasses of") : TEXT("objects of class"), *Output->GetName()),
					Described.IsEmpty() ? TEXT("no parameters") : *FString::Join(Described, TEXT(", "))));
				continue;
			}
			if (Index == 0 && bCreate) return Fail(TEXT("a new table starts with create."));
			SyncRows(Table);

			if (Kind == TEXT("column.add"))
			{
				const FColumnKindSpec* Spec = FindColumnKind(Op.ColumnKind);
				if (!Spec)
				{
					TArray<FString> Names;
					for (const FColumnKindSpec& Each : ColumnKinds()) Names.Add(Each.Kind);
					return Fail(FString::Printf(TEXT("'%s' is not a column kind; the kinds are %s."), *Op.ColumnKind, *FString::Join(Names, TEXT(", "))));
				}
				UScriptStruct* Type = ColumnStruct(*Spec);
				if (!Type)
				{
					return Fail(FCString::Strcmp(Spec->Kind, TEXT("pose_match")) == 0
						? FString(TEXT("pose_match needs the PoseSearch plugin (Pose Search), which is not enabled in this project; enable it and restart the editor."))
						: FString::Printf(TEXT("this editor has no %s column type (%s)."), Spec->Kind, Spec->Struct));
				}
				if (Columns >= C::MaxColumns) return Fail(FString::Printf(TEXT("a table takes at most %d columns."), C::MaxColumns));
				FInstancedStruct NewColumn;
				NewColumn.InitializeAs(Type);
				FChooserColumnBase& Base = NewColumn.GetMutable<FChooserColumnBase>();
				Base.Initialize(&Table);
				FString Bound;
				if (!Op.Binding.TrimStartAndEnd().IsEmpty())
				{
					if (!BindColumn(Table, NewColumn, Op.Binding, Bound, Why)) return Fail(Why);
				}
				else if (!Base.IsRandomizeColumn() && FCString::Strcmp(Spec->Kind, TEXT("pose_match")) != 0)
				{
					FBindingPath Ignored;
					ResolveBinding(Table, TEXT("?"), BindingTypeOf(NewColumn), Ignored, Why);
					return Fail(FString::Printf(TEXT("a %s column needs Binding, the property it %s: %s"), Spec->Kind, Base.HasFilters() ? TEXT("reads") : TEXT("writes"), *Why));
				}
				TArray<FString> Done;
				if (!ApplyParams(NewColumn, Op.Params, &Table, Done, Why)) return Fail(Why);
				// Where the Chooser editor puts a new column: randomize last (one per table), outputs before it, filters after the filters.
				int32 At = 0;
				if (Base.IsRandomizeColumn())
				{
					for (int32 Each = 0; Each < Columns; ++Each)
					{
						if (Table.ColumnsStructs[Each].IsValid() && Table.ColumnsStructs[Each].Get<FChooserColumnBase>().IsRandomizeColumn())
						{
							return Fail(FString::Printf(TEXT("the table has a Randomize column already (column %d); a table takes one, last."), Each));
						}
					}
					At = Columns;
				}
				else if (Base.HasOutputs() && !Base.HasFilters())
				{
					At = Columns;
					if (At > 0 && Table.ColumnsStructs[At - 1].IsValid() && Table.ColumnsStructs[At - 1].Get<FChooserColumnBase>().IsRandomizeColumn()) --At;
				}
				else
				{
					while (At < Columns && Table.ColumnsStructs[At].IsValid())
					{
						const FChooserColumnBase& Each = Table.ColumnsStructs[At].Get<FChooserColumnBase>();
						if ((Each.HasOutputs() && !Each.HasFilters()) || Each.IsRandomizeColumn()) break;
						++At;
					}
				}
				if (!Op.Cells.IsEmpty() && Op.Cells.Num() != Rows)
				{
					return Fail(FString::Printf(TEXT("Cells has %d value(s) for %d row(s); give one per row, in row order, or none."), Op.Cells.Num(), Rows));
				}
				Table.ColumnsStructs.Insert(MoveTemp(NewColumn), At);
				FInstancedStruct& Added = Table.ColumnsStructs[At];
				Added.GetMutable<FChooserColumnBase>().SetNumRows(Rows);
				for (int32 Row = 0; Row < Rows; ++Row)
				{
					if (!SetCell(Table, Added, Row, Op.Cells.IsValidIndex(Row) ? Op.Cells[Row] : FString(), Why)) return Fail(FString::Printf(TEXT("row %d: %s"), Row, *Why));
				}
				FString Note = FString::Printf(TEXT("Operation %d (column.add): %s column at index %d"), Index, Spec->Kind, At);
				if (!Bound.IsEmpty()) Note += TEXT(", bound to ") + Bound;
				if (!Done.IsEmpty()) Note += TEXT(", ") + FString::Join(Done, TEXT(", "));
				if (At < Columns) Note += FString::Printf(TEXT("; the columns from %d on moved one right"), At);
				OutNotes.Add(Note + TEXT("."));
				continue;
			}

			if (Kind == TEXT("column.set") || Kind == TEXT("column.remove"))
			{
				if (!Table.ColumnsStructs.IsValidIndex(Op.Column))
				{
					return Fail(FString::Printf(TEXT("Column %d is not a column; the columns are %s."), Op.Column, *ColumnList()));
				}
				FInstancedStruct& Column = Table.ColumnsStructs[Op.Column];
				if (Kind == TEXT("column.remove"))
				{
					OutNotes.Add(FString::Printf(TEXT("Operation %d (column.remove): column %d (%s) removed; the columns after it moved one left."), Index, Op.Column, *KindOf(Column)));
					Table.ColumnsStructs.RemoveAt(Op.Column);
					continue;
				}
				if (!Column.IsValid()) return Fail(TEXT("the column holds nothing; remove it."));
				FString Bound;
				if (!Op.Binding.TrimStartAndEnd().IsEmpty() && !BindColumn(Table, Column, Op.Binding, Bound, Why)) return Fail(Why);
				TArray<FString> Done;
				if (!ApplyParams(Column, Op.Params, &Table, Done, Why)) return Fail(Why);
				if (!Op.Cells.IsEmpty() && Op.Cells.Num() != Rows)
				{
					return Fail(FString::Printf(TEXT("Cells has %d value(s) for %d row(s); give one per row, in row order, or none."), Op.Cells.Num(), Rows));
				}
				for (int32 Row = 0; Row < Op.Cells.Num(); ++Row)
				{
					if (!SetCell(Table, Column, Row, Op.Cells[Row], Why)) return Fail(FString::Printf(TEXT("row %d: %s"), Row, *Why));
				}
				TArray<FString> Parts;
				if (!Bound.IsEmpty()) Parts.Add(TEXT("bound to ") + Bound);
				Parts.Append(Done);
				if (!Op.Cells.IsEmpty()) Parts.Add(FString::Printf(TEXT("%d cell(s) set"), Op.Cells.Num()));
				if (Parts.IsEmpty()) return Fail(TEXT("give Binding, Params or Cells to change."));
				OutNotes.Add(FString::Printf(TEXT("Operation %d (column.set): column %d (%s) %s."), Index, Op.Column, *KindOf(Column), *FString::Join(Parts, TEXT(", "))));
				continue;
			}

			auto CheckCells = [&]() -> bool
			{
				if (Op.Cells.IsEmpty() || Op.Cells.Num() == Columns) return true;
				Why = FString::Printf(TEXT("Cells has %d value(s) for %d column(s); give one per column in column order (%s), empty for any."), Op.Cells.Num(), Columns, *ColumnList());
				return false;
			};

			if (Kind == TEXT("row.add"))
			{
				if (Rows >= C::MaxRows) return Fail(FString::Printf(TEXT("a table takes at most %d rows."), C::MaxRows));
				const int32 At = Op.Row < 0 ? Rows : Op.Row;
				if (At > Rows) return Fail(FString::Printf(TEXT("Row %d is past the end; the table has %d row(s) (-1 adds at the end)."), Op.Row, Rows));
				if (!CheckCells()) return Fail(Why);
				FInstancedStruct Result;
				if (!ParseResult(Table, Op.Result, TEXT("Result"), Result, Why)) return Fail(Why);
				Table.ResultsStructs.Insert(MoveTemp(Result), At);
				Table.DisabledRows.Insert(bDisabled, At);
				for (FInstancedStruct& Column : Table.ColumnsStructs)
				{
					if (Column.IsValid()) Column.GetMutable<FChooserColumnBase>().InsertRows(At, 1);
				}
				for (int32 Each = 0; Each < Table.ColumnsStructs.Num(); ++Each)
				{
					if (!Table.ColumnsStructs[Each].IsValid()) continue;
					if (!SetCell(Table, Table.ColumnsStructs[Each], At, Op.Cells.IsValidIndex(Each) ? Op.Cells[Each] : FString(), Why))
					{
						return Fail(FString::Printf(TEXT("column %d (%s): %s"), Each, *KindOf(Table.ColumnsStructs[Each]), *Why));
					}
				}
				OutNotes.Add(FString::Printf(TEXT("Operation %d (row.add): row %d gives %s%s."), Index, At, *ResultText(Table.ResultsStructs[At]), bDisabled ? TEXT(", disabled") : TEXT("")));
				continue;
			}

			if (Kind == TEXT("row.set") || Kind == TEXT("row.remove"))
			{
				if (!Table.ResultsStructs.IsValidIndex(Op.Row)) return Fail(FString::Printf(TEXT("Row %d is not a row; the table has %d row(s), from 0."), Op.Row, Rows));
				if (Kind == TEXT("row.remove"))
				{
					TArray<int32> Gone = {Op.Row};
					for (FInstancedStruct& Column : Table.ColumnsStructs)
					{
						if (Column.IsValid()) Column.GetMutable<FChooserColumnBase>().DeleteRows(Gone);
					}
					OutNotes.Add(FString::Printf(TEXT("Operation %d (row.remove): row %d (%s) removed; the rows after it moved up one."), Index, Op.Row, *ResultText(Table.ResultsStructs[Op.Row])));
					Table.ResultsStructs.RemoveAt(Op.Row);
					Table.DisabledRows.RemoveAt(Op.Row);
					continue;
				}
				if (!CheckCells()) return Fail(Why);
				if (Op.Result.TrimStartAndEnd().IsEmpty() && Op.Cells.IsEmpty() && Op.Disabled.IsEmpty()) return Fail(TEXT("give Result, Cells or Disabled to change."));
				if (!Op.Result.TrimStartAndEnd().IsEmpty())
				{
					FInstancedStruct Result;
					if (!ParseResult(Table, Op.Result, TEXT("Result"), Result, Why)) return Fail(Why);
					Table.ResultsStructs[Op.Row] = MoveTemp(Result);
				}
				for (int32 Each = 0; Each < Op.Cells.Num(); ++Each)
				{
					if (!Table.ColumnsStructs[Each].IsValid()) continue;
					if (!SetCell(Table, Table.ColumnsStructs[Each], Op.Row, Op.Cells[Each], Why))
					{
						return Fail(FString::Printf(TEXT("column %d (%s): %s"), Each, *KindOf(Table.ColumnsStructs[Each]), *Why));
					}
				}
				if (!Op.Disabled.IsEmpty()) Table.DisabledRows[Op.Row] = bDisabled;
				OutNotes.Add(FString::Printf(TEXT("Operation %d (row.set): row %d gives %s%s%s."), Index, Op.Row, *ResultText(Table.ResultsStructs[Op.Row]),
					Op.Cells.IsEmpty() ? TEXT("") : TEXT(", cells set"), Op.Disabled.IsEmpty() ? TEXT("") : bDisabled ? TEXT(", disabled") : TEXT(", enabled")));
				continue;
			}

			if (Kind == TEXT("fallback.set"))
			{
				if (!CheckCells()) return Fail(Why);
				const FString Result = Op.Result.TrimStartAndEnd();
				if (Result.Equals(TEXT("none"), ESearchCase::IgnoreCase) && Table.GetContextOwner()->ResultType != EObjectChooserResultType::NoPrimaryResult)
				{
					Table.FallbackResult.Reset();
				}
				else if (!Result.IsEmpty() || Table.GetContextOwner()->ResultType == EObjectChooserResultType::NoPrimaryResult || !Table.FallbackResult.IsValid())
				{
					FInstancedStruct Fallback;
					if (!ParseResult(Table, Result, TEXT("Result"), Fallback, Why)) return Fail(Why);
					Table.FallbackResult = MoveTemp(Fallback);
				}
				for (int32 Each = 0; Each < Op.Cells.Num(); ++Each)
				{
					if (!Table.ColumnsStructs[Each].IsValid()) continue;
					if (!SetCell(Table, Table.ColumnsStructs[Each], ChooserColumn_SpecialIndex_Fallback, Op.Cells[Each], Why))
					{
						return Fail(FString::Printf(TEXT("column %d (%s): %s"), Each, *KindOf(Table.ColumnsStructs[Each]), *Why));
					}
				}
				OutNotes.Add(FString::Printf(TEXT("Operation %d (fallback.set): when no row passes the table gives %s%s."), Index, *ResultText(Table.FallbackResult),
					Op.Cells.IsEmpty() ? TEXT("") : TEXT(", with the output columns' fallback values set")));
				continue;
			}
		}
		SyncRows(Table);
		return true;
	}

	bool Check(const FHyperAIChooserPlanRequest& Request, FPlan& OutPlan, FString& OutError)
	{
		OutPlan = FPlan();
		const FString Path = PackageOf(Request.Asset);
		if (Path.IsEmpty())
		{
			OutError = TEXT("Give Asset, the chooser table to make or change, as /Game/Folder/CT_Name.");
			return false;
		}
		if (!Path.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(TEXT("%s is not project content: a plan makes and changes chooser tables under /Game only, never Engine or plugin content."), *Path);
			return false;
		}
		FString Reason;
		if (!FHyperAIStudioNames::IsPackagePath(Path, TEXT("Asset"), &Reason))
		{
			OutError = Reason;
			return false;
		}
		OutPlan.AssetPath = Path;
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
			return false;
		}
		OutPlan.bCreate = Lower(Request.Operations[0].Kind) == TEXT("create");
		UChooserTable* Copy = nullptr;
		if (OutPlan.bCreate)
		{
			// Free in memory too: an unsaved asset of another class there would stop the editor when the table is made.
			// A table an undone plan made is still there, as garbage: the plan may make it again.
			if (!FHyperAIStudioNames::IsFreePackage(Path) && !UndoneAt(Path))
			{
				OutError = FString::Printf(TEXT("%s exists already; leave out create to change it, or name a new Asset."), *Path);
				return false;
			}
			Copy = NewObject<UChooserTable>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UChooserTable::StaticClass(), FName(TEXT("HyperAIChooserCheck"))), RF_Transient);
		}
		else
		{
			FString Why;
			OutPlan.Existing = LoadTable(Path, Why);
			if (!OutPlan.Existing)
			{
				OutError = FString::Printf(TEXT("%s is not a chooser table; start the plan with create to make one."), *Path);
				return false;
			}
			if (OutPlan.Existing->RootChooser)
			{
				OutError = FString::Printf(TEXT("%s is nested in another table; change the table it is in."), *Path);
				return false;
			}
			// The ops run on a copy first, so a refusal halfway leaves the asset as it was.
			Copy = DuplicateObject<UChooserTable>(OutPlan.Existing, GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UChooserTable::StaticClass(), FName(TEXT("HyperAIChooserCheck"))));
			Copy->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
			Copy->SetFlags(RF_Transient);
		}
		const bool bApplied = Apply(*Copy, Request.Operations, OutPlan.bCreate, /*bReal=*/false, OutPlan.Notes, OutError);
		if (bApplied)
		{
			OutPlan.Warnings = Warn(*Copy);
			OutPlan.Columns = Copy->ColumnsStructs.Num();
			OutPlan.Rows = Copy->ResultsStructs.Num();
		}
		else
		{
			OutError += TEXT(" Nothing was changed.");
		}
		Copy->MarkAsGarbage();
		return bApplied;
	}
}
