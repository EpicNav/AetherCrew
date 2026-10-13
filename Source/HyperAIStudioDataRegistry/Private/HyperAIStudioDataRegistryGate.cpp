// Games by Hyper 2026.

#include "HyperAIStudioDataRegistryGate.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Curves/RealCurve.h"
#include "Curves/RichCurve.h"
#include "Curves/SimpleCurve.h"
#include "DataRegistry.h"
#include "DataRegistryId.h"
#include "DataRegistrySettings.h"
#include "DataRegistrySource.h"
#include "DataRegistrySource_CurveTable.h"
#include "DataRegistrySource_DataTable.h"
#include "DataRegistrySubsystem.h"
#include "Engine/AssetManager.h"
#include "Engine/CurveTable.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "GameplayTagsManager.h"
#include "HyperAIStudioNames.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::DataRegistry::Gate
{
	using C = FHyperAIStudioDataRegistryContracts;

	namespace
	{
		FString Lower(const FString& Text)
		{
			return Text.TrimStartAndEnd().ToLower();
		}

		/** Spaces and dots out, lower case: how setting names are compared. */
		FString Squash(const FString& Text)
		{
			FString Out = Text.ToLower();
			Out.ReplaceInline(TEXT(" "), TEXT(""));
			Out.ReplaceInline(TEXT("."), TEXT(""));
			Out.ReplaceInline(TEXT("_"), TEXT(""));
			return Out;
		}

		template <typename T>
		T* LoadQuiet(const FString& Reference, FString& OutReason)
		{
			const FString Trimmed = Reference.TrimStartAndEnd();
			if (!FHyperAIStudioNames::IsObjectPath(Trimmed, TEXT("Path"), &OutReason)) return nullptr;
			const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
			if (T* Found = FindObject<T>(nullptr, *ObjectPath)) return IsValid(Found) ? Found : nullptr;
			// A path with nothing on disk is never loaded: the failed load would leave an empty package behind.
			if (!FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ObjectPath))) return nullptr;
			return LoadObject<T>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		template <typename T>
		T* LoadSoft(const FSoftObjectPath& Path)
		{
			if (Path.IsNull()) return nullptr;
			FString Reason;
			return LoadQuiet<T>(Path.ToString(), Reason);
		}

		FString PackageOf(const FString& Reference)
		{
			FString Path = Reference.TrimStartAndEnd();
			int32 Dot = INDEX_NONE;
			if (Path.FindLastChar(TEXT('.'), Dot)) Path.LeftInline(Dot);
			return Path;
		}

		const UEnum* EnumOf(const FProperty& Property)
		{
			if (const FEnumProperty* Enum = CastField<FEnumProperty>(&Property)) return Enum->GetEnum();
			if (const FByteProperty* Byte = CastField<FByteProperty>(&Property)) return Byte->Enum;
			return nullptr;
		}

		/** The setting structs whose fields Params names directly. */
		bool IsFlattened(const UScriptStruct* Struct)
		{
			if (!Struct) return false;
			const FName Name = Struct->GetFName();
			return Name == TEXT("DataRegistryCachePolicy") || Name == TEXT("DataRegistrySource_DataTableRules") || Name == TEXT("AssetManagerSearchRules")
				|| Name == TEXT("DataRegistryIdFormat");
		}

		bool IsSettable(const FProperty& Property)
		{
			return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_Transient | CPF_EditConst | CPF_Deprecated)
				&& Property.ArrayDim == 1 && Property.GetFName() != TEXT("DataSources") && Property.GetFName() != TEXT("RuntimeSources");
		}

		FString TypeText(const FProperty& Property)
		{
			if (const UEnum* Enum = EnumOf(Property))
			{
				TArray<FString> Names;
				for (int32 Index = 0; Index < Enum->NumEnums() - (Enum->ContainsExistingMax() ? 1 : 0); ++Index) Names.Add(Enum->GetNameStringByIndex(Index));
				return TEXT("one of ") + FString::Join(Names, TEXT(", "));
			}
			if (Property.IsA<FBoolProperty>()) return TEXT("true or false");
			if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property)) return Numeric->IsFloatingPoint() ? TEXT("number") : TEXT("integer");
			if (const FStructProperty* Struct = CastField<FStructProperty>(&Property); Struct && Struct->Struct->GetFName() == TEXT("GameplayTag")) return TEXT("gameplay tag");
			if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property); Array && Array->Inner->IsA<FStrProperty>()) return TEXT("list \"a, b\"");
			if (const FClassProperty* Class = CastField<FClassProperty>(&Property)) return FString::Printf(TEXT("class (%s or a child)"), *GetNameSafe(Class->MetaClass));
			if (const FSoftClassProperty* Class = CastField<FSoftClassProperty>(&Property)) return FString::Printf(TEXT("class (%s or a child)"), *GetNameSafe(Class->MetaClass));
			if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
			{
				return Object->PropertyClass && Object->PropertyClass->IsChildOf(UScriptStruct::StaticClass()) ? FString(TEXT("struct path"))
					: FString::Printf(TEXT("%s path"), *GetNameSafe(Object->PropertyClass));
			}
			if (Property.IsA<FNameProperty>()) return TEXT("name");
			if (Property.IsA<FStrProperty>()) return TEXT("text");
			return Property.GetCPPType();
		}

		UClass* FindClass(const FString& Text)
		{
			if (Text.StartsWith(TEXT("/")))
			{
				FString Reason;
				if (UClass* Found = LoadQuiet<UClass>(Text, Reason)) return Found;
				return LoadQuiet<UClass>(Text + TEXT("_C"), Reason);
			}
			if (UClass* Found = FindFirstObject<UClass>(*Text, EFindFirstObjectOptions::NativeFirst)) return Found;
			if (Text.Len() > 1 && (Text[0] == TEXT('U') || Text[0] == TEXT('A'))) return FindFirstObject<UClass>(*Text.Mid(1), EFindFirstObjectOptions::NativeFirst);
			return nullptr;
		}

		/** Asset data of a data table whose rows are Item, as UMetaDataRegistrySource_DataTable::DoesAssetPassFilter checks it. */
		bool AssetRowsMatch(const FAssetData& Asset, const UScriptStruct* Item)
		{
			if (!Item) return false;
			if (const UDataTable* Loaded = Cast<UDataTable>(Asset.FastGetAsset(/*bLoad=*/false))) return RowsMatch(Loaded->GetRowStruct(), Item);
			FString Rows;
			if (!Asset.GetTagValue(FName(TEXT("RowStructure")), Rows)) return false;
			if (Rows == Item->GetName() || Rows == Item->GetStructPathName().ToString() || Rows == Item->GetPathName()) return true;
			const UScriptStruct* Found = Rows.StartsWith(TEXT("/")) ? FindObject<UScriptStruct>(nullptr, *Rows)
				: FindFirstObject<UScriptStruct>(*Rows, EFindFirstObjectOptions::ExactClass);
			return Found && Found->IsChildOf(Item);
		}

		FString ShortEnum(const FString& Text)
		{
			int32 Colons = Text.Find(TEXT("::"));
			return Colons == INDEX_NONE ? Text : Text.Mid(Colons + 2);
		}
	}

	const TArray<FString>& OpKinds()
	{
		static const TArray<FString> Kinds = {TEXT("create"), TEXT("set"), TEXT("source.add"), TEXT("source.set"), TEXT("source.remove")};
		return Kinds;
	}

	const TArray<FString>& SourceKinds()
	{
		static const TArray<FString> Kinds = {TEXT("data_table"), TEXT("curve_table"), TEXT("data_table_meta"), TEXT("curve_table_meta")};
		return Kinds;
	}

	UClass* SourceClassOf(const FString& Kind)
	{
		const FString Word = Lower(Kind);
		if (Word == TEXT("data_table")) return UDataRegistrySource_DataTable::StaticClass();
		if (Word == TEXT("curve_table")) return UDataRegistrySource_CurveTable::StaticClass();
		if (Word == TEXT("data_table_meta")) return UMetaDataRegistrySource_DataTable::StaticClass();
		if (Word == TEXT("curve_table_meta")) return UMetaDataRegistrySource_CurveTable::StaticClass();
		return nullptr;
	}

	FString SourceKindOf(const UDataRegistrySource* Source)
	{
		if (!Source) return TEXT("empty");
		// Most derived first: a game may subclass the engine's sources.
		if (Source->IsA<UMetaDataRegistrySource_DataTable>()) return TEXT("data_table_meta");
		if (Source->IsA<UMetaDataRegistrySource_CurveTable>()) return TEXT("curve_table_meta");
		if (Source->IsA<UDataRegistrySource_DataTable>()) return TEXT("data_table");
		if (Source->IsA<UDataRegistrySource_CurveTable>()) return TEXT("curve_table");
		return Source->GetClass()->GetName();
	}

	bool IsMetaKind(const FString& Kind)
	{
		return Kind.EndsWith(TEXT("_meta"));
	}

	TArray<FLeaf> Leaves(const UClass& Class)
	{
		TArray<FLeaf> Out;
		for (TFieldIterator<FProperty> It(&Class); It; ++It)
		{
			FProperty* Property = *It;
			if (!IsSettable(*Property)) continue;
			const FStructProperty* Struct = CastField<FStructProperty>(Property);
			if (Struct && IsFlattened(Struct->Struct))
			{
				for (TFieldIterator<FProperty> Child(Struct->Struct); Child; ++Child)
				{
					if (!IsSettable(**Child)) continue;
					FLeaf& Leaf = Out.AddDefaulted_GetRef();
					Leaf.Name = Child->GetName();
					Leaf.Path = Property->GetName() + TEXT(".") + Child->GetName();
					Leaf.Outer = Property;
					Leaf.Property = *Child;
				}
				continue;
			}
			FLeaf& Leaf = Out.AddDefaulted_GetRef();
			Leaf.Name = Property->GetName();
			Leaf.Path = Leaf.Name;
			Leaf.Property = Property;
		}
		return Out;
	}

	const FLeaf* FindLeaf(const TArray<FLeaf>& All, const FString& Name)
	{
		const FString Wanted = Squash(Name.TrimStartAndEnd());
		if (Wanted.IsEmpty()) return nullptr;
		for (const FLeaf& Leaf : All)
		{
			const FString Own = Squash(Leaf.Name);
			if (Own == Wanted || Squash(Leaf.Path) == Wanted) return &Leaf;
			// A bool's b is optional: PrecacheTable for bPrecacheTable.
			if (Leaf.Property->IsA<FBoolProperty>() && Own.StartsWith(TEXT("b")) && Own.Mid(1) == Wanted) return &Leaf;
#if WITH_EDITORONLY_DATA
			if (Squash(Leaf.Property->GetDisplayNameText().ToString()) == Wanted) return &Leaf;
#endif
		}
		return nullptr;
	}

	void* LeafValue(const FLeaf& Leaf, UObject& Object)
	{
		void* Container = &Object;
		if (Leaf.Outer) Container = Leaf.Outer->ContainerPtrToValuePtr<void>(Container);
		return Leaf.Property->ContainerPtrToValuePtr<void>(Container);
	}

	const void* LeafValue(const FLeaf& Leaf, const UObject& Object)
	{
		return LeafValue(Leaf, const_cast<UObject&>(Object));
	}

	FString LeafText(const FLeaf& Leaf, const UObject& Object)
	{
		const void* Value = LeafValue(Leaf, Object);
		FString Text;
		Leaf.Property->ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);
		if (EnumOf(*Leaf.Property)) return ShortEnum(Text);
		if (const FStructProperty* Struct = CastField<FStructProperty>(Leaf.Property); Struct && Struct->Struct->GetFName() == TEXT("GameplayTag"))
		{
			// (TagName="A.B") as A.B.
			FString Tag = Text;
			Tag.ReplaceInline(TEXT("(TagName=\""), TEXT(""));
			Tag.ReplaceInline(TEXT("\")"), TEXT(""));
			return Tag == TEXT("()") || Tag == TEXT("None") ? FString() : Tag;
		}
		return Text.Len() > 300 ? Text.Left(300) + TEXT("...") : Text;
	}

	TArray<FString> DescribeLeaves(const UClass& Class, const UObject* Values, const bool bCurrent)
	{
		const UObject* From = Values ? Values : Class.GetDefaultObject();
		TArray<FString> Out;
		for (const FLeaf& Leaf : Leaves(Class))
		{
			const FString Value = From ? LeafText(Leaf, *From) : FString();
			Out.Add(FString::Printf(TEXT("%s: %s, %s %s%s"), *Leaf.Name, *TypeText(*Leaf.Property), bCurrent ? TEXT("now") : TEXT("default"),
				Value.IsEmpty() ? TEXT("(none)") : *Value, Leaf.Outer ? *FString::Printf(TEXT(" (in %s)"), *Leaf.Outer->GetName()) : TEXT("")));
		}
		return Out;
	}

	bool SplitParams(const FString& Text, TArray<TPair<FString, FString>>& Out, FString& OutError)
	{
		Out.Reset();
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(";"), /*InCullEmpty=*/true);
		for (const FString& Part : Parts)
		{
			const FString Trimmed = Part.TrimStartAndEnd();
			if (Trimmed.IsEmpty()) continue;
			FString Name, Value;
			if (!Trimmed.Split(TEXT("="), &Name, &Value) || Name.TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not Name=Value; Params are \"Name=Value; Name=Value\"."), *Trimmed);
				return false;
			}
			Out.Emplace(Name.TrimStartAndEnd(), Value.TrimStartAndEnd());
		}
		return true;
	}

	UScriptStruct* FindStruct(const FString& Text, FString& OutReason)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutReason = TEXT("Give the item struct: /Script/Module.StructName (a C++ struct, its F left out) or a user struct /Game/Data/S_Item.");
			return nullptr;
		}
		if (Trimmed.StartsWith(TEXT("/")))
		{
			if (!FHyperAIStudioNames::IsObjectPath(Trimmed, TEXT("ItemStruct"), &OutReason)) return nullptr;
			FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
			if (UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *ObjectPath)) return Found;
			// /Script/MyGame.FMyRow: the C++ name, whose F Unreal leaves out.
			FString Package, Name;
			if (ObjectPath.Split(TEXT("."), &Package, &Name) && Name.Len() > 1 && Name[0] == TEXT('F'))
			{
				if (UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *(Package + TEXT(".") + Name.Mid(1)))) return Found;
			}
			if (!ObjectPath.StartsWith(TEXT("/Script/")))
			{
				FString Reason;
				if (UScriptStruct* Found = LoadQuiet<UScriptStruct>(ObjectPath, Reason)) return Found;
			}
			OutReason = FString::Printf(TEXT("%s is not a struct: give /Script/Module.StructName (F left out) or a user struct /Game/Data/S_Item."), *Trimmed);
			return nullptr;
		}
		if (!FHyperAIStudioNames::IsName(Trimmed, TEXT("ItemStruct"), &OutReason)) return nullptr;
		if (UScriptStruct* Found = FindFirstObject<UScriptStruct>(*Trimmed, EFindFirstObjectOptions::NativeFirst)) return Found;
		if (Trimmed.Len() > 1 && Trimmed[0] == TEXT('F'))
		{
			if (UScriptStruct* Found = FindFirstObject<UScriptStruct>(*Trimmed.Mid(1), EFindFirstObjectOptions::NativeFirst)) return Found;
		}
		OutReason = FString::Printf(TEXT("No struct is named %s; give its path, /Script/Module.StructName or /Game/Data/S_Item."), *Trimmed);
		return nullptr;
	}

	bool RowsMatch(const UScriptStruct* Rows, const UScriptStruct* Item)
	{
		return Rows && Item && Rows->IsChildOf(Item);
	}

	FString CurveMismatch(const UCurveTable& Table, const UScriptStruct* Item)
	{
		if (!Item || !Item->IsChildOf(FRealCurve::StaticStruct()))
		{
			return FString::Printf(TEXT("curve tables serve curve items only, and the registry's items are %s; make the item struct /Script/Engine.RealCurve (any curve table), RichCurve or SimpleCurve."),
				*GetNameSafe(Item));
		}
		if (Item->IsChildOf(FSimpleCurve::StaticStruct()) && Table.GetCurveTableMode() != ECurveTableMode::SimpleCurves)
		{
			return FString::Printf(TEXT("%s holds rich (cubic) curves and the registry's items are SimpleCurve; use RealCurve or RichCurve items, or a table of simple curves."), *Table.GetPathName());
		}
		if (Item->IsChildOf(FRichCurve::StaticStruct()) && Table.GetCurveTableMode() != ECurveTableMode::RichCurves)
		{
			return FString::Printf(TEXT("%s holds no rich curves and the registry's items are RichCurve; use RealCurve or SimpleCurve items, or a table of rich (cubic) curves."), *Table.GetPathName());
		}
		return FString();
	}

	bool CoerceValue(const FProperty& Property, const FString& Text, FAssign& InOut, FString& OutError)
	{
		FString Value = Text.TrimStartAndEnd();
		if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\""))) Value = Value.Mid(1, Value.Len() - 2);
		InOut.Resolved = nullptr;
		if (const UEnum* Enum = EnumOf(Property))
		{
			TArray<FString> Names;
			for (int32 Index = 0; Index < Enum->NumEnums() - (Enum->ContainsExistingMax() ? 1 : 0); ++Index)
			{
				const FString Short = Enum->GetNameStringByIndex(Index);
				if (Short.Equals(Value, ESearchCase::IgnoreCase) || Enum->GetDisplayNameTextByIndex(Index).ToString().Replace(TEXT(" "), TEXT("")).Equals(Value.Replace(TEXT(" "), TEXT("")), ESearchCase::IgnoreCase)
					|| ShortEnum(Value).Equals(Short, ESearchCase::IgnoreCase))
				{
					InOut.Import = Short;
					InOut.Shown = Short;
					return true;
				}
				Names.Add(Short);
			}
			OutError = FString::Printf(TEXT("takes one of %s"), *FString::Join(Names, TEXT(", ")));
			return false;
		}
		if (Property.IsA<FBoolProperty>())
		{
			const FString Word = Value.ToLower();
			const bool bTrue = Word == TEXT("true") || Word == TEXT("1") || Word == TEXT("yes") || Word == TEXT("on");
			const bool bFalse = Word == TEXT("false") || Word == TEXT("0") || Word == TEXT("no") || Word == TEXT("off");
			if (!bTrue && !bFalse)
			{
				OutError = TEXT("takes true or false");
				return false;
			}
			InOut.Import = bTrue ? TEXT("True") : TEXT("False");
			InOut.Shown = bTrue ? TEXT("true") : TEXT("false");
			return true;
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
		{
			double Number = 0.0;
			if (!LexTryParseString(Number, *Value) || !FMath::IsFinite(Number))
			{
				OutError = TEXT("takes a number");
				return false;
			}
			if (!Numeric->IsFloatingPoint() && (FMath::Frac(Number) != 0.0 || FMath::Abs(Number) > 2147483647.0))
			{
				OutError = TEXT("takes a whole number");
				return false;
			}
			InOut.Import = Numeric->IsFloatingPoint() ? FString::SanitizeFloat(Number) : FString::Printf(TEXT("%lld"), static_cast<int64>(Number));
			InOut.Shown = InOut.Import;
			return true;
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property); Struct && Struct->Struct->GetFName() == TEXT("GameplayTag"))
		{
			if (Value.IsEmpty() || Value.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				InOut.Import = TEXT("()");
				InOut.Shown = TEXT("");
				return true;
			}
			if (!FHyperAIStudioNames::IsName(Value, TEXT("Tag"), &OutError)) return false;
			if (!UGameplayTagsManager::Get().RequestGameplayTag(FName(*Value), /*ErrorIfNotFound=*/false).IsValid())
			{
				OutError = FString::Printf(TEXT("takes a gameplay tag the project has, and %s is not one; add it first (Project Settings > Gameplay Tags, or Epic's GameplayTagsToolset.AddTag)"), *Value);
				return false;
			}
			InOut.Import = FString::Printf(TEXT("(TagName=\"%s\")"), *Value);
			InOut.Shown = Value;
			return true;
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property); Array && Array->Inner->IsA<FStrProperty>())
		{
			TArray<FString> Items;
			Value.ParseIntoArray(Items, TEXT(","), /*InCullEmpty=*/true);
			TArray<FString> Quoted;
			for (FString& Item : Items)
			{
				Item.TrimStartAndEndInline();
				if (Item.IsEmpty()) continue;
				if (Item.Contains(TEXT("\"")) || Item.Contains(TEXT("(")) || Item.Contains(TEXT(")")))
				{
					OutError = TEXT("takes a list \"a, b\" without quotes or brackets");
					return false;
				}
				if (!FHyperAIStudioNames::IsName(Item, TEXT("Params"), &OutError)) return false;
				if (Property.GetFName() == TEXT("AssetScanPaths") && !Item.StartsWith(TEXT("/")))
				{
					OutError = FString::Printf(TEXT("takes folders or assets by path, /Game/Data/Items; %s is not one"), *Item);
					return false;
				}
				Quoted.Add(TEXT("\"") + Item + TEXT("\""));
			}
			InOut.Import = TEXT("(") + FString::Join(Quoted, TEXT(",")) + TEXT(")");
			InOut.Shown = FString::Join(Items, TEXT(", "));
			return true;
		}
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
		{
			if (Value.IsEmpty() || Value.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				InOut.Import = TEXT("None");
				InOut.Shown = TEXT("None");
				return true;
			}
			if (!FHyperAIStudioNames::CanImportText(Property, Value, TEXT("The value"), &OutError)) return false;
			UClass* Meta = nullptr;
			if (const FClassProperty* Class = CastField<FClassProperty>(&Property)) Meta = Class->MetaClass;
			else if (const FSoftClassProperty* SoftClass = CastField<FSoftClassProperty>(&Property)) Meta = SoftClass->MetaClass;
			if (Meta)
			{
				UClass* Found = FindClass(Value);
				if (!Found || !Found->IsChildOf(Meta))
				{
					OutError = FString::Printf(TEXT("takes a class that is %s or a child of it, and %s is not one"), *Meta->GetName(), *Value);
					return false;
				}
				InOut.Resolved = Found;
				InOut.Import = Found->GetPathName();
				InOut.Shown = Found->GetPathName();
				return true;
			}
			if (Object->PropertyClass && Object->PropertyClass->IsChildOf(UScriptStruct::StaticClass()))
			{
				UScriptStruct* Found = FindStruct(Value, OutError);
				if (!Found) return false;
				InOut.Resolved = Found;
				InOut.Import = Found->GetPathName();
				InOut.Shown = Found->GetPathName();
				return true;
			}
			FString Reason;
			UObject* Found = LoadQuiet<UObject>(Value, Reason);
			if (!Found || (Object->PropertyClass && !Found->IsA(Object->PropertyClass)))
			{
				OutError = Reason.IsEmpty() ? FString::Printf(TEXT("takes a %s by path, and %s is not one"), *GetNameSafe(Object->PropertyClass), *Value) : Reason;
				return false;
			}
			InOut.Resolved = Found;
			InOut.Import = Found->GetPathName();
			InOut.Shown = Found->GetPathName();
			return true;
		}
		// Names, strings and the rest: Unreal's own import, tried in scratch memory first.
		if (!FHyperAIStudioNames::CanImportText(Property, Value, TEXT("The value"), &OutError)) return false;
		void* Scratch = FMemory::Malloc(Property.GetSize(), Property.GetMinAlignment());
		Property.InitializeValue(Scratch);
		FStringOutputDevice Errors;
		const TCHAR* End = Property.ImportText_Direct(*Value, Scratch, nullptr, PPF_None, &Errors);
		Property.DestroyValue(Scratch);
		FMemory::Free(Scratch);
		if (!End)
		{
			OutError = FString::Printf(TEXT("does not take '%s' (%s)"), *Value, *TypeText(Property));
			return false;
		}
		InOut.Import = Value;
		InOut.Shown = Value;
		return true;
	}

	bool ParseAssigns(const UClass& Class, const FString& Params, TArray<FAssign>& Out, FString& OutError)
	{
		TArray<TPair<FString, FString>> Pairs;
		if (!SplitParams(Params, Pairs, OutError)) return false;
		const TArray<FLeaf> All = Leaves(Class);
		for (const TPair<FString, FString>& Pair : Pairs)
		{
			const FLeaf* Leaf = FindLeaf(All, Pair.Key);
			if (!Leaf)
			{
				TArray<FString> Names;
				for (const FLeaf& Each : All) Names.Add(Each.Name);
				OutError = FString::Printf(TEXT("%s has no setting '%s'; it takes %s."), *Class.GetName(), *Pair.Key, *FString::Join(Names, TEXT(", ")));
				return false;
			}
			FAssign Assign;
			Assign.Leaf = *Leaf;
			FString Why;
			if (!CoerceValue(*Leaf->Property, Pair.Value, Assign, Why))
			{
				OutError = FString::Printf(TEXT("%s %s."), *Leaf->Name, *Why);
				return false;
			}
			Out.Add(MoveTemp(Assign));
		}
		return true;
	}

	bool ApplyAssigns(UObject& Object, const TArray<FAssign>& Assigns, FString& OutError)
	{
		for (const FAssign& Assign : Assigns)
		{
			void* Value = LeafValue(Assign.Leaf, Object);
			FStringOutputDevice Errors;
			if (!Assign.Leaf.Property->ImportText_Direct(*Assign.Import, Value, &Object, PPF_None, &Errors))
			{
				OutError = FString::Printf(TEXT("%s could not take '%s' on %s: %s"), *Assign.Leaf.Name, *Assign.Shown, *Object.GetName(), *Errors);
				return false;
			}
		}
		return true;
	}

	FString FSimSource::Describe(const int32 Index) const
	{
		FString Line = FString::Printf(TEXT("%d %s"), Index, *Kind);
		if (!TablePath.IsEmpty()) Line += TEXT(" ") + TablePath;
		if (!Paths.IsEmpty()) Line += TEXT(" over ") + FString::Join(Paths, TEXT(", "));
		return Line;
	}

	UDataRegistry* UndoneAt(const FString& PackagePath)
	{
		if (FPackageName::DoesPackageExist(PackagePath)) return nullptr;
		UPackage* Package = FindPackage(nullptr, *PackagePath);
		UObject* There = Package ? StaticFindObjectFast(UObject::StaticClass(), Package, FName(*FPackageName::GetShortName(PackagePath)), EFindObjectFlags::None,
			RF_NoFlags, EInternalObjectFlags::None) : nullptr;
		UDataRegistry* Registry = Cast<UDataRegistry>(There);
		return Registry && !IsValid(Registry) ? Registry : nullptr;
	}

	FString TypeTakenBy(const FName Type, const FString& OwnPackage)
	{
		if (Type.IsNone()) return FString();
		// Loaded ones first: an unsaved registry is in memory only.
		for (TObjectIterator<UDataRegistry> It; It; ++It)
		{
			UDataRegistry* Other = *It;
			if (!IsValid(Other) || Other->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Other->GetPackage() == GetTransientPackage()) continue;
			if (Other->GetPackage()->GetName() == OwnPackage) continue;
			if (Other->GetRegistryType() == Type) return Other->GetPathName();
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(UDataRegistry::StaticClass()->GetClassPathName(), Assets);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.PackageName.ToString() == OwnPackage) continue;
			FString Value;
			if (!Asset.GetTagValue(FName(TEXT("RegistryType")), Value) || FName(*Value) != Type) continue;
			// An undone registry still listed is not there.
			if (UObject* Loaded = Asset.FastGetAsset(false); Loaded && !IsValid(Loaded)) continue;
			return Asset.GetObjectPathString();
		}
		return FString();
	}

	bool Check(const FHyperAIDataRegistryPlanRequest& Request, FPlan& OutPlan, FString& OutError)
	{
		OutPlan = FPlan();
		const FString Path = PackageOf(Request.Asset);
		if (Path.IsEmpty())
		{
			OutError = TEXT("Give Asset, the Data Registry to make or change, as /Game/Folder/DR_Name.");
			return false;
		}
		FString Reason;
		if (!Path.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(TEXT("%s is not project content: a plan makes and changes Data Registries under /Game only, never Engine or plugin content."), *Path);
			return false;
		}
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
		for (int32 Index = 1; Index < Request.Operations.Num(); ++Index)
		{
			if (Lower(Request.Operations[Index].Kind) == TEXT("create"))
			{
				OutError = FString::Printf(TEXT("Operation %d (create): create comes first, once: the ops after it work on what it made. Nothing was changed."), Index);
				return false;
			}
		}
		OutPlan.bMake = Lower(Request.Operations[0].Kind) == TEXT("create");
		if (OutPlan.bMake)
		{
			// Free in memory too: an unsaved asset of another class there would stop the editor when the registry is made. A
			// registry an undone plan made is still there, as garbage: the plan may make it again.
			if (!FHyperAIStudioNames::IsFreePackage(Path) && !UndoneAt(Path))
			{
				OutError = FString::Printf(TEXT("Operation 0 (create): %s exists already; leave out create to change it, or name a new Asset. Nothing was changed."), *Path);
				return false;
			}
		}
		else
		{
			OutPlan.Existing = LoadQuiet<UDataRegistry>(Path, Reason);
			if (!OutPlan.Existing)
			{
				FString Ignored;
				OutError = LoadQuiet<UObject>(Path, Ignored)
					? FString::Printf(TEXT("%s is not a Data Registry."), *Path)
					: FString::Printf(TEXT("There is no Data Registry at %s; start the plan with create to make one."), *Path);
				return false;
			}
			OutPlan.RegistryType = OutPlan.Existing->GetRegistryType();
			OutPlan.ItemStruct = OutPlan.Existing->GetItemStruct();
			OutPlan.Tag = OutPlan.Existing->GetIdFormat().BaseGameplayTag.IsValid() ? OutPlan.Existing->GetIdFormat().BaseGameplayTag.ToString() : FString();
			for (UDataRegistrySource* Source : OutPlan.Existing->GetDataSources())
			{
				FSimSource& Sim = OutPlan.Sources.AddDefaulted_GetRef();
				Sim.Existing = Source;
				Sim.Kind = SourceKindOf(Source);
				Sim.Class = Source ? Source->GetClass() : nullptr;
				if (const UDataRegistrySource_DataTable* Table = Cast<UDataRegistrySource_DataTable>(Source))
				{
					Sim.TablePath = Table->SourceTable.ToString();
					Sim.Table = LoadSoft<UDataTable>(Table->SourceTable.ToSoftObjectPath());
				}
				else if (const UDataRegistrySource_CurveTable* Curves = Cast<UDataRegistrySource_CurveTable>(Source))
				{
					Sim.TablePath = Curves->SourceTable.ToString();
					Sim.Table = LoadSoft<UCurveTable>(Curves->SourceTable.ToSoftObjectPath());
				}
				else if (const UMetaDataRegistrySource* Meta = Cast<UMetaDataRegistrySource>(Source))
				{
					Sim.Paths = Meta->SearchRules.AssetScanPaths;
				}
			}
		}
		const TArray<FLeaf> RegistryLeaves = Leaves(*UDataRegistry::StaticClass());
		bool bTypeChanged = OutPlan.bMake;
		bool bStructChanged = OutPlan.bMake;
		TSet<int32> Touched;
		auto AddRegistryAssign = [&](const TCHAR* LeafName, const FString& Value, FString& Why) -> bool
		{
			const FLeaf* Leaf = FindLeaf(RegistryLeaves, LeafName);
			if (!Leaf)
			{
				Why = FString::Printf(TEXT("the registry has no %s setting in this engine."), LeafName);
				return false;
			}
			FAssign Assign;
			Assign.Leaf = *Leaf;
			FString Error;
			if (!CoerceValue(*Leaf->Property, Value, Assign, Error))
			{
				Why = FString::Printf(TEXT("%s %s."), LeafName, *Error);
				return false;
			}
			OutPlan.RegistryAssigns.Add(MoveTemp(Assign));
			return true;
		};
		auto NoteRegistryAssigns = [&](int32 From)
		{
			for (int32 Each = From; Each < OutPlan.RegistryAssigns.Num(); ++Each)
			{
				const FAssign& Assign = OutPlan.RegistryAssigns[Each];
				const FName Name = Assign.Leaf.Property->GetFName();
				if (Name == TEXT("RegistryType"))
				{
					OutPlan.RegistryType = FName(*Assign.Shown);
					bTypeChanged = true;
				}
				else if (Name == TEXT("ItemStruct"))
				{
					OutPlan.ItemStruct = Cast<UScriptStruct>(Assign.Resolved);
					bStructChanged = true;
				}
				else if (Name == TEXT("BaseGameplayTag")) OutPlan.Tag = Assign.Shown;
			}
		};
		// A source's table and search folders as its settings leave them.
		auto NoteSourceAssigns = [](FSimSource& Sim)
		{
			for (const FAssign& Assign : Sim.Assigns)
			{
				const FName Name = Assign.Leaf.Property->GetFName();
				if (Name == TEXT("SourceTable"))
				{
					Sim.Table = Assign.Resolved;
					Sim.TablePath = Assign.Resolved ? Assign.Resolved->GetPathName() : FString();
				}
				else if (Name == TEXT("AssetScanPaths"))
				{
					Sim.Paths.Reset();
					Assign.Shown.ParseIntoArray(Sim.Paths, TEXT(","), true);
					for (FString& Each : Sim.Paths) Each.TrimStartAndEndInline();
				}
			}
		};

		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAIDataRegistryOp& Op = Request.Operations[Index];
			const FString Kind = Lower(Op.Kind);
			auto Fail = [&OutError, Index, &Kind](const FString& Message)
			{
				OutError = FString::Printf(TEXT("Operation %d (%s): %s Nothing was changed."), Index, *Kind, *Message);
				return false;
			};
			if (!OpKinds().Contains(Kind))
			{
				return Fail(FString::Printf(TEXT("'%s' is not an op kind; the kinds are %s."), *Op.Kind, *FString::Join(OpKinds(), TEXT(", "))));
			}
			const bool bCreate = Kind == TEXT("create");
			const bool bSource = Kind.StartsWith(TEXT("source."));
			if (!bCreate && (!Op.RegistryType.TrimStartAndEnd().IsEmpty() || !Op.ItemStruct.TrimStartAndEnd().IsEmpty() || !Op.Tag.TrimStartAndEnd().IsEmpty()))
			{
				return Fail(TEXT("Only create takes RegistryType, ItemStruct and Tag; set changes them with Params (RegistryType=..., ItemStruct=..., BaseGameplayTag=...)."));
			}
			if (Kind != TEXT("source.add") && !Op.SourceKind.TrimStartAndEnd().IsEmpty()) return Fail(TEXT("Only source.add takes SourceKind."));
			if (Kind != TEXT("source.add") && Kind != TEXT("source.set") && (!Op.Table.TrimStartAndEnd().IsEmpty() || !Op.Paths.IsEmpty()))
			{
				return Fail(TEXT("Only source.add and source.set take Table and Paths."));
			}
			if (!bSource && Op.Index != -1) return Fail(TEXT("Only the source ops take Index."));

			if (bCreate)
			{
				const FString Type = Op.RegistryType.TrimStartAndEnd();
				if (Type.IsEmpty() || Type.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return Fail(TEXT("Give RegistryType, the name the registry's ids start with (Items for Items:Sword)."));
				if (Type.Contains(TEXT(":")) || Type.Contains(TEXT(" "))) return Fail(TEXT("RegistryType is one word, without ':' or spaces."));
				if (FName(*Type) == FDataRegistryType::CustomContextType.GetName()) return Fail(TEXT("That RegistryType is the editor's placeholder for custom ids; name the registry's own type."));
				FString Why;
				if (!AddRegistryAssign(TEXT("RegistryType"), Type, Why)) return Fail(Why);
				if (!AddRegistryAssign(TEXT("ItemStruct"), Op.ItemStruct, Why)) return Fail(Why);
				if (!Op.Tag.TrimStartAndEnd().IsEmpty() && !AddRegistryAssign(TEXT("BaseGameplayTag"), Op.Tag, Why)) return Fail(Why);
				const int32 From = OutPlan.RegistryAssigns.Num();
				if (!ParseAssigns(*UDataRegistry::StaticClass(), Op.Params, OutPlan.RegistryAssigns, Why)) return Fail(Why);
				NoteRegistryAssigns(0);
				OutPlan.Notes.Add(FString::Printf(TEXT("Operation %d (create): make %s, type %s, items %s%s%s."), Index, *Path, *Type, *GetNameSafe(OutPlan.ItemStruct),
					OutPlan.Tag.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", ids under the tag %s"), *OutPlan.Tag),
					From < OutPlan.RegistryAssigns.Num() ? *FString::Printf(TEXT(", %d setting(s)"), OutPlan.RegistryAssigns.Num() - From) : TEXT("")));
				continue;
			}
			if (Kind == TEXT("set"))
			{
				if (Op.Params.TrimStartAndEnd().IsEmpty()) return Fail(TEXT("Give Params, \"Name=Value; Name=Value\"; hyper_data_registry_inspect lists the registry's settings."));
				const int32 From = OutPlan.RegistryAssigns.Num();
				FString Why;
				if (!ParseAssigns(*UDataRegistry::StaticClass(), Op.Params, OutPlan.RegistryAssigns, Why)) return Fail(Why);
				NoteRegistryAssigns(From);
				TArray<FString> Shown;
				for (int32 Each = From; Each < OutPlan.RegistryAssigns.Num(); ++Each) Shown.Add(OutPlan.RegistryAssigns[Each].Leaf.Name + TEXT("=") + OutPlan.RegistryAssigns[Each].Shown);
				OutPlan.Notes.Add(FString::Printf(TEXT("Operation %d (set): %s."), Index, *FString::Join(Shown, TEXT("; "))));
				continue;
			}

			// The source ops.
			OutPlan.bSourcesChanged = true;
			if (Kind == TEXT("source.remove"))
			{
				if (!OutPlan.Sources.IsValidIndex(Op.Index)) return Fail(FString::Printf(TEXT("Index %d is not a source; the registry has %d (0 to %d)."), Op.Index, OutPlan.Sources.Num(), OutPlan.Sources.Num() - 1));
				OutPlan.Notes.Add(FString::Printf(TEXT("Operation %d (source.remove): remove source %s."), Index, *OutPlan.Sources[Op.Index].Describe(Op.Index)));
				OutPlan.Sources.RemoveAt(Op.Index);
				TSet<int32> Shifted;
				for (const int32 Each : Touched) if (Each != Op.Index) Shifted.Add(Each > Op.Index ? Each - 1 : Each);
				Touched = MoveTemp(Shifted);
				continue;
			}
			FSimSource* Sim = nullptr;
			int32 At = Op.Index;
			FSimSource Added;
			if (Kind == TEXT("source.add"))
			{
				const FString SourceKind = Lower(Op.SourceKind);
				Added.Class = SourceClassOf(SourceKind);
				if (!Added.Class) return Fail(FString::Printf(TEXT("SourceKind '%s' is not a source kind; the kinds are %s."), *Op.SourceKind, *FString::Join(SourceKinds(), TEXT(", "))));
				if (OutPlan.Sources.Num() >= C::MaxSources) return Fail(FString::Printf(TEXT("a registry has at most %d sources."), C::MaxSources));
				if (At != -1 && (At < 0 || At > OutPlan.Sources.Num())) return Fail(FString::Printf(TEXT("Index %d is not a place in the list: -1 (the end) or 0 to %d."), At, OutPlan.Sources.Num()));
				Added.Kind = SourceKind;
				if (IsMetaKind(SourceKind))
				{
					// The engine's own default is NoAssets, which finds nothing; a meta source added here searches its Paths.
					const TArray<FLeaf> AddedLeaves = Leaves(*Added.Class);
					const FLeaf* Usage = FindLeaf(AddedLeaves, TEXT("AssetUsage"));
					FAssign Assign;
					FString Why;
					if (Usage)
					{
						Assign.Leaf = *Usage;
						if (CoerceValue(*Usage->Property, TEXT("SearchAssets"), Assign, Why)) Added.Assigns.Add(Assign);
					}
				}
				if (At == -1) At = OutPlan.Sources.Num();
				OutPlan.Sources.Insert(MoveTemp(Added), At);
				TSet<int32> Shifted;
				for (const int32 Each : Touched) Shifted.Add(Each >= At ? Each + 1 : Each);
				Touched = MoveTemp(Shifted);
			}
			else if (!OutPlan.Sources.IsValidIndex(At))
			{
				return Fail(FString::Printf(TEXT("Index %d is not a source; the registry has %d (0 to %d)."), At, OutPlan.Sources.Num(), OutPlan.Sources.Num() - 1));
			}
			Sim = &OutPlan.Sources[At];
			Touched.Add(At);
			if (!Sim->Class) return Fail(FString::Printf(TEXT("source %d is empty; remove it with source.remove."), At));
			const bool bMeta = Sim->Class->IsChildOf(UMetaDataRegistrySource::StaticClass());
			const bool bDirect = Sim->Class->IsChildOf(UDataRegistrySource_DataTable::StaticClass()) || Sim->Class->IsChildOf(UDataRegistrySource_CurveTable::StaticClass());
			if (Kind == TEXT("source.set") && Op.Table.TrimStartAndEnd().IsEmpty() && Op.Paths.IsEmpty() && Op.Params.TrimStartAndEnd().IsEmpty())
			{
				return Fail(TEXT("Give Table, Paths or Params to change."));
			}
			FString Why;
			if (!Op.Table.TrimStartAndEnd().IsEmpty())
			{
				if (!bDirect) return Fail(FString::Printf(TEXT("a %s source takes Paths, not a Table."), *Sim->Kind));
				const TArray<FLeaf> SourceLeaves = Leaves(*Sim->Class);
				const FLeaf* Leaf = FindLeaf(SourceLeaves, TEXT("SourceTable"));
				FAssign Assign;
				if (!Leaf) return Fail(TEXT("this source has no table setting."));
				Assign.Leaf = *Leaf;
				if (!CoerceValue(*Leaf->Property, Op.Table, Assign, Why) || !Assign.Resolved)
				{
					return Fail(FString::Printf(TEXT("Table %s is not a %s."), *Op.Table, Sim->Class->IsChildOf(UDataRegistrySource_CurveTable::StaticClass()) ? TEXT("curve table") : TEXT("data table")));
				}
				Sim->Assigns.Add(Assign);
			}
			else if (Kind == TEXT("source.add") && bDirect)
			{
				return Fail(FString::Printf(TEXT("a %s source needs Table, the table it reads (/Game/Data/DT_Items)."), *Sim->Kind));
			}
			if (!Op.Paths.IsEmpty())
			{
				if (!bMeta) return Fail(FString::Printf(TEXT("a %s source takes a Table, not Paths."), *Sim->Kind));
				const TArray<FLeaf> SourceLeaves = Leaves(*Sim->Class);
				const FLeaf* Leaf = FindLeaf(SourceLeaves, TEXT("AssetScanPaths"));
				if (!Leaf) return Fail(TEXT("this source has no search paths setting."));
				FAssign Assign;
				Assign.Leaf = *Leaf;
				if (!CoerceValue(*Leaf->Property, FString::Join(Op.Paths, TEXT(",")), Assign, Why)) return Fail(TEXT("Paths ") + Why + TEXT("."));
				Sim->Assigns.Add(Assign);
			}
			if (!ParseAssigns(*Sim->Class, Op.Params, Sim->Assigns, Why)) return Fail(Why);
			NoteSourceAssigns(*Sim);
			if (Kind == TEXT("source.add") && bMeta && Sim->Paths.IsEmpty())
			{
				return Fail(TEXT("a meta source needs Paths, the folders it searches for tables (/Game/Data/Items, or a game feature's /MyFeature/Data)."));
			}
			OutPlan.Notes.Add(FString::Printf(TEXT("Operation %d (%s): %s source %s."), Index, *Kind, Kind == TEXT("source.add") ? TEXT("add") : TEXT("change"), *Sim->Describe(At)));
		}

		// The registry as the plan leaves it.
		if (!OutPlan.ItemStruct)
		{
			OutError = TEXT("The registry has no item struct; set ItemStruct (create's ItemStruct, or set with Params ItemStruct=/Script/Module.Struct). Nothing was changed.");
			return false;
		}
		if (OutPlan.RegistryType.IsNone())
		{
			OutError = TEXT("The registry has no type; set RegistryType. Nothing was changed.");
			return false;
		}
		if (bTypeChanged)
		{
			const FString Taken = TypeTakenBy(OutPlan.RegistryType, Path);
			if (!Taken.IsEmpty())
			{
				OutError = FString::Printf(TEXT("The registry type %s is already %s's; each registry's type is unique in the project. Name another. Nothing was changed."),
					*OutPlan.RegistryType.ToString(), *Taken);
				return false;
			}
		}
		const UScriptStruct* Item = OutPlan.ItemStruct;
		const bool bCurveItems = Item->IsChildOf(FRealCurve::StaticStruct());
		for (int32 Index = 0; Index < OutPlan.Sources.Num(); ++Index)
		{
			const FSimSource& Sim = OutPlan.Sources[Index];
			// Sources the plan did not touch keep their own problems as warnings, unless the plan changed the item struct under them.
			const bool bCheck = Touched.Contains(Index) || bStructChanged;
			FString Problem;
			if (const UDataTable* Table = Cast<UDataTable>(Sim.Table))
			{
				if (!RowsMatch(Table->GetRowStruct(), Item))
				{
					Problem = FString::Printf(TEXT("source %d: %s's rows are %s and the registry's items are %s; a data table source's rows must be the item struct or a struct made from it."),
						Index, *Table->GetPathName(), *GetNameSafe(Table->GetRowStruct()), *Item->GetName());
				}
			}
			else if (const UCurveTable* Curves = Cast<UCurveTable>(Sim.Table))
			{
				const FString Why = CurveMismatch(*Curves, Item);
				if (!Why.IsEmpty()) Problem = FString::Printf(TEXT("source %d: %s"), Index, *Why);
			}
			else if (Sim.Kind == TEXT("curve_table_meta") && !bCurveItems)
			{
				Problem = FString::Printf(TEXT("source %d: a curve table meta source serves curve items only, and the registry's items are %s."), Index, *Item->GetName());
			}
			else if (Sim.Kind == TEXT("data_table_meta") && bCurveItems)
			{
				OutPlan.Warnings.Add(FString::Printf(TEXT("Source %d searches for data tables, but the registry's items are curves: it will find none whose rows match."), Index));
			}
			else if ((Sim.Kind == TEXT("data_table") || Sim.Kind == TEXT("curve_table")) && !Sim.TablePath.IsEmpty() && !Sim.Table)
			{
				OutPlan.Warnings.Add(FString::Printf(TEXT("Source %d's table %s is not there."), Index, *Sim.TablePath));
			}
			if (Problem.IsEmpty()) continue;
			if (bCheck)
			{
				OutError = Problem + TEXT(" Nothing was changed.");
				return false;
			}
			OutPlan.Warnings.Add(Problem);
		}
		if (OutPlan.Sources.IsEmpty()) OutPlan.Warnings.Add(TEXT("The registry has no sources, so it holds no items; add one with source.add."));
		if (!IsScanned(Path)) OutPlan.Warnings.Add(ScanWarning(Path));
		return true;
	}

	bool Write(UDataRegistry& Registry, const FPlan& Plan, FString& OutError)
	{
		// Type, struct and id format first: the sources check their tables against the item struct.
		if (!ApplyAssigns(Registry, Plan.RegistryAssigns, OutError)) return false;
		FArrayProperty* SourcesProperty = FindFProperty<FArrayProperty>(UDataRegistry::StaticClass(), TEXT("DataSources"));
		if (!SourcesProperty)
		{
			OutError = TEXT("UDataRegistry has no DataSources property in this engine.");
			return false;
		}
		TArray<TObjectPtr<UDataRegistrySource>>* List = SourcesProperty->ContainerPtrToValuePtr<TArray<TObjectPtr<UDataRegistrySource>>>(&Registry);
		if (Plan.bSourcesChanged || Plan.bMake)
		{
			TArray<TObjectPtr<UDataRegistrySource>> Made;
			for (const FSimSource& Sim : Plan.Sources)
			{
				UDataRegistrySource* Source = Sim.Existing;
				if (!Source && Sim.Class)
				{
					// Instanced inside the registry, as the Details panel adds one; the registry's array change is what undo takes back.
					Source = NewObject<UDataRegistrySource>(&Registry, Sim.Class, NAME_None, RF_Transactional);
				}
				else if (Source && !Sim.Assigns.IsEmpty())
				{
					Source->Modify();
				}
				if (Source && !ApplyAssigns(*Source, Sim.Assigns, OutError)) return false;
				Made.Add(Source);
			}
			*List = MoveTemp(Made);
		}
#if WITH_EDITOR
		for (UDataRegistrySource* Source : *List)
		{
			// Each direct source loads its table again and checks its rows against the item struct, as a Details edit does.
			if (Source) Source->EditorRefreshSource();
		}
#endif
		return true;
	}

	FString FResolvedTable::Describe() const
	{
		return FString::Printf(TEXT("source %d %s %s"), SourceIndex, *Kind, *Path);
	}

	TArray<FResolvedTable> ResolveTables(const UDataRegistry& Registry, TArray<FString>& OutNotes)
	{
		TArray<FResolvedTable> Out;
		const UScriptStruct* Item = Registry.GetItemStruct();
		const TArray<TObjectPtr<UDataRegistrySource>>& Sources = Registry.GetDataSources();
		IAssetRegistry& Assets = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		for (int32 Index = 0; Index < Sources.Num(); ++Index)
		{
			const UDataRegistrySource* Source = Sources[Index];
			const FString Kind = SourceKindOf(Source);
			if (!Source)
			{
				OutNotes.Add(FString::Printf(TEXT("Source %d is empty."), Index));
				continue;
			}
			if (const UDataRegistrySource_DataTable* Direct = Cast<UDataRegistrySource_DataTable>(Source))
			{
				UDataTable* Table = LoadSoft<UDataTable>(Direct->SourceTable.ToSoftObjectPath());
				if (!Table)
				{
					OutNotes.Add(FString::Printf(TEXT("Source %d's table %s is not there."), Index, Direct->SourceTable.IsNull() ? TEXT("(none)") : *Direct->SourceTable.ToString()));
					continue;
				}
				if (!RowsMatch(Table->GetRowStruct(), Item))
				{
					OutNotes.Add(FString::Printf(TEXT("Source %d's table %s has %s rows, not %s: the registry ignores it."), Index, *Table->GetPathName(), *GetNameSafe(Table->GetRowStruct()), *GetNameSafe(Item)));
					continue;
				}
				FResolvedTable& Entry = Out.AddDefaulted_GetRef();
				Entry.SourceIndex = Index;
				Entry.Kind = Kind;
				Entry.DataTable = Table;
				Entry.Path = Table->GetPathName();
				continue;
			}
			if (const UDataRegistrySource_CurveTable* Direct = Cast<UDataRegistrySource_CurveTable>(Source))
			{
				UCurveTable* Table = LoadSoft<UCurveTable>(Direct->SourceTable.ToSoftObjectPath());
				if (!Table)
				{
					OutNotes.Add(FString::Printf(TEXT("Source %d's table %s is not there."), Index, Direct->SourceTable.IsNull() ? TEXT("(none)") : *Direct->SourceTable.ToString()));
					continue;
				}
				const FString Why = CurveMismatch(*Table, Item);
				if (!Why.IsEmpty())
				{
					OutNotes.Add(FString::Printf(TEXT("Source %d: %s The registry ignores it."), Index, *Why));
					continue;
				}
				FResolvedTable& Entry = Out.AddDefaulted_GetRef();
				Entry.SourceIndex = Index;
				Entry.Kind = Kind;
				Entry.CurveTable = Table;
				Entry.Path = Table->GetPathName();
				continue;
			}
			const UMetaDataRegistrySource* Meta = Cast<UMetaDataRegistrySource>(Source);
			if (!Meta)
			{
				OutNotes.Add(FString::Printf(TEXT("Source %d is a %s, a game's own source: only the running registry reads it."), Index, *Kind));
				continue;
			}
			const bool bCurves = Kind == TEXT("curve_table_meta");
			if (!EnumHasAnyFlags(Meta->AssetUsage, EMetaDataRegistrySourceAssetUsage::SearchAssets))
			{
				OutNotes.Add(FString::Printf(TEXT("Source %d does not search (AssetUsage %s): it holds only tables registered with it at run time, such as a game feature's."), Index,
					*ShortEnum(StaticEnum<EMetaDataRegistrySourceAssetUsage>()->GetNameStringByValue(static_cast<int64>(Meta->AssetUsage)))));
				continue;
			}
			// The asset registry, as UMetaDataRegistrySource::DetermineRuntimeNames searches it (without the asset manager's own include rules).
			TArray<FString> Paths = Meta->SearchRules.AssetScanPaths;
			if (!Meta->SearchRules.bSkipVirtualPathExpansion)
			{
				if (const UAssetManager* Manager = UAssetManager::GetIfInitialized()) Manager->ExpandVirtualPaths(Paths);
			}
			FARFilter Folders, Packages;
			for (const FString& Each : Paths)
			{
				FString Trimmed = Each.TrimStartAndEnd();
				while (Trimmed.EndsWith(TEXT("/"))) Trimmed.LeftChopInline(1);
				if (Trimmed.IsEmpty() || !Trimmed.StartsWith(TEXT("/"))) continue;
				if (Trimmed.Contains(TEXT("."))) Packages.PackageNames.Add(FName(*PackageOf(Trimmed)));
				else Folders.PackagePaths.Add(FName(*Trimmed));
			}
			if (Folders.PackagePaths.IsEmpty() && Packages.PackageNames.IsEmpty())
			{
				OutNotes.Add(FString::Printf(TEXT("Source %d searches no folders."), Index));
				continue;
			}
			UClass* Base = Meta->SearchRules.AssetBaseClass ? Meta->SearchRules.AssetBaseClass.Get() : bCurves ? UCurveTable::StaticClass() : UDataTable::StaticClass();
			TArray<FAssetData> Found;
			for (FARFilter* Filter : {&Folders, &Packages})
			{
				if (Filter->PackagePaths.IsEmpty() && Filter->PackageNames.IsEmpty()) continue;
				Filter->bRecursivePaths = true;
				Filter->ClassPaths.Add(Base->GetClassPathName());
				Filter->bRecursiveClasses = true;
				Assets.GetAssets(*Filter, Found);
			}
			Found.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
			int32 Skipped = 0;
			for (const FAssetData& Asset : Found)
			{
				const FString Package = Asset.PackageName.ToString();
				if (!Meta->SearchRules.IncludePatterns.IsEmpty()
					&& !Meta->SearchRules.IncludePatterns.ContainsByPredicate([&Package](const FString& Pattern) { return Package.MatchesWildcard(Pattern); }))
				{
					continue;
				}
				if (Meta->SearchRules.ExcludePatterns.ContainsByPredicate([&Package](const FString& Pattern) { return Package.MatchesWildcard(Pattern); })) continue;
				if (!bCurves && !AssetRowsMatch(Asset, Item))
				{
					++Skipped;
					continue;
				}
				if (Out.Num() >= C::MaxResolvedTables)
				{
					OutNotes.Add(FString::Printf(TEXT("Only the first %d tables are read."), C::MaxResolvedTables));
					break;
				}
				FResolvedTable Entry;
				Entry.SourceIndex = Index;
				Entry.Kind = Kind;
				Entry.Path = Asset.GetObjectPathString();
				FString Reason;
				if (bCurves) Entry.CurveTable = LoadQuiet<UCurveTable>(Entry.Path, Reason);
				else Entry.DataTable = LoadQuiet<UDataTable>(Entry.Path, Reason);
				if (!Entry.CurveTable && !Entry.DataTable) continue;
				if (Entry.CurveTable && !CurveMismatch(*Entry.CurveTable, Item).IsEmpty())
				{
					++Skipped;
					continue;
				}
				Out.Add(MoveTemp(Entry));
			}
			if (Skipped > 0) OutNotes.Add(FString::Printf(TEXT("Source %d skips %d table(s) under its folders whose rows are not %s."), Index, Skipped, *GetNameSafe(Item)));
		}
		return Out;
	}

	TArray<FName> ItemNames(const TArray<FResolvedTable>& Tables)
	{
		TArray<FName> Out;
		TSet<FName> Seen;
		for (const FResolvedTable& Table : Tables)
		{
			TArray<FName> Names;
			if (Table.DataTable) Table.DataTable->GetRowMap().GetKeys(Names);
			else if (Table.CurveTable) Table.CurveTable->GetRowMap().GetKeys(Names);
			for (const FName& Name : Names)
			{
				bool bAlready = false;
				Seen.Add(Name, &bAlready);
				if (!bAlready) Out.Add(Name);
			}
		}
		return Out;
	}

	TArray<FString> ScanDirectories()
	{
		TArray<FString> Out;
		for (const FDirectoryPath& Directory : GetDefault<UDataRegistrySettings>()->DirectoriesToScan)
		{
			FString Path = Directory.Path.TrimStartAndEnd();
			if (Path.IsEmpty()) continue;
			if (!Path.StartsWith(TEXT("/"))) Path = TEXT("/Game/") + Path;
			Out.AddUnique(UAssetManager::GetNormalizedPackagePath(Path, false));
		}
		return Out;
	}

	bool IsScanned(const FString& Path)
	{
		if (GetDefault<UDataRegistrySettings>()->bInitializeAllLoadedRegistries) return true;
		// As UDataRegistrySubsystem::RefreshRegistryMap matches them.
		return ScanDirectories().ContainsByPredicate([&Path](const FString& Directory) { return Path.StartsWith(Directory); });
	}

	FString ScanWarning(const FString& PackagePath)
	{
		return FString::Printf(TEXT("%s is outside the folders Data Registry settings scan, so the Data Registry subsystem never loads it and games cannot look its items up. Add its folder (%s) to Project Settings > Game > Data Registry > Directories To Scan, or with Epic's ConfigSettingsToolset (the DataRegistrySettings section, DirectoriesToScan); HyperAI does not change project settings."),
			*PackagePath, *FPackageName::GetLongPackagePath(PackagePath));
	}

	bool IsRegistered(const UDataRegistry& Registry)
	{
		const UDataRegistrySubsystem* Subsystem = GEngine ? GEngine->GetEngineSubsystem<UDataRegistrySubsystem>() : nullptr;
		return Subsystem && Subsystem->AreRegistriesInitialized() && Subsystem->GetRegistryForType(Registry.GetRegistryType()) == &Registry;
	}

	void Refresh(UDataRegistry& Registry, TArray<FString>& OutNotes)
	{
		UDataRegistrySubsystem* Subsystem = GEngine ? GEngine->GetEngineSubsystem<UDataRegistrySubsystem>() : nullptr;
		// EditorRefreshRegistry logs an error when the subsystem has not started, so the registry's sources are refreshed
		// directly then.
		if (!Subsystem || !Subsystem->AreRegistriesInitialized() || Registry.GetRegistryType().IsNone()
			|| Registry.GetRegistryType() == FDataRegistryType::CustomContextType.GetName())
		{
#if WITH_EDITOR
			for (UDataRegistrySource* Source : Registry.GetDataSources())
			{
				if (Source) Source->EditorRefreshSource();
			}
#endif
			OutNotes.Add(TEXT("The Data Registry subsystem is not running in this editor (no folder in Data Registry settings' Directories To Scan, or it waits for the first Play In Editor), so the registry was not registered; hyper_data_registry_lookup reads its tables directly."));
			return;
		}
#if WITH_EDITOR
		Registry.EditorRefreshRegistry();
#endif
		OutNotes.Add(IsRegistered(Registry)
			? FString::Printf(TEXT("Refreshed: the Data Registry subsystem has %s for the type %s and reads its %d runtime source(s)."), *Registry.GetName(), *Registry.GetRegistryType().ToString(),
				Registry.GetRuntimeSources().Num())
			: FString::Printf(TEXT("Refreshed, but the Data Registry subsystem does not have %s for the type %s (outside the scanned folders, or another registry has the type)."),
				*Registry.GetName(), *Registry.GetRegistryType().ToString()));
	}

	TArray<FString> ExportFields(const UScriptStruct& Struct, const void* Memory)
	{
		TArray<FString> Out;
		for (TFieldIterator<FProperty> It(&Struct); It; ++It)
		{
			FString Text;
			// The item as its own delta: a null delta leaves out values equal to the type's zero.
			It->ExportText_InContainer(0, Text, Memory, Memory, nullptr, PPF_None);
			Out.Add(FString::Printf(TEXT("%s: %s"), *It->GetName(), *(Text.Len() > 1000 ? Text.Left(1000) + TEXT("...") : Text)));
		}
		return Out;
	}
}
