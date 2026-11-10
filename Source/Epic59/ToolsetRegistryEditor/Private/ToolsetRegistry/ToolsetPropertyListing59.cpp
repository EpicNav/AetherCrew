// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: adapted from UE 5.9's ToolsetRegistryEditor/Private/ToolsetRegistry/ToolsetPropertyListing.cpp.
// Differences from 5.9, each because the 5.9 piece is private to 5.9's ToolsetRegistry or missing in 5.8:
// - readability is 5.8's PropertyAccessUtil::CanGetPropertyValue, not 5.9's
//   UE::PropertyEditor::CheckCanGetProperty (no property-editor permission list in 5.8);
// - a struct with a custom JSON converter (5.9's ToolsetJson::HasCustomImporter) still expands into its
//   fields: 5.8's converter registry is private;
// - RootPath is parsed here (5.9's PropertyPath::Parse/ApplySubscript are private): segments are
//   "Name", "Name[3]" and "Name[Key]" or "Name[\"Key\"]";
// - an unknown segment lists the names that are there instead of 5.9's similarity summary.

#include "ToolsetRegistry/ToolsetPropertyListing59.h"

#include "Editor.h"
#include "Internationalization/Regex.h"
#include "JsonObjectConverter.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/UnrealType.h"

namespace UE::ToolsetRegistry::Epic59::PropertyListing
{
	FString ToWireKey(const FProperty* Property)
	{
		return Property ? FJsonObjectConverter::StandardizeCase(Property->GetAuthoredName()) : FString();
	}

	namespace
	{
		constexpr int32 MapKeyCap = 8;

		FProperty* FindPropertyByName(const UStruct* Struct, FStringView Name)
		{
			if (!Struct)
			{
				return nullptr;
			}
			if (FProperty* Property = PropertyAccessUtil::FindPropertyByName(FName(Name, FNAME_Find), Struct))
			{
				return Property;
			}
			for (FProperty* Property : TFieldRange<FProperty>(Struct))
			{
				if (Name.Equals(ToWireKey(Property), ESearchCase::IgnoreCase))
				{
					return Property;
				}
			}
			return nullptr;
		}

		/** The names a listing would show on Container, for an unknown-segment message. */
		FString ListWireNames(const UStruct* Container)
		{
			TArray<FString> Names;
			if (Container)
			{
				for (TFieldIterator<FProperty> It(Container); It; ++It)
				{
					if (IsPropertyListable(Container, *It, /*bUserVisiblePropertiesOnly=*/true))
					{
						Names.Add(ToWireKey(*It));
					}
				}
			}
			constexpr int32 MaxNames = 15;
			if (Names.Num() > MaxNames)
			{
				const int32 More = Names.Num() - MaxNames;
				Names.SetNum(MaxNames);
				return FString::Printf(TEXT("%s (+%d more)"), *FString::Join(Names, TEXT(", ")), More);
			}
			return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT(", "));
		}

		FString DescribeUnknownSegment(const FString& ContainerName, const FString& Candidates,
			const FString& SegmentName, int32 SegmentIndex, const FString& FullPath)
		{
			FString Message = FString::Printf(TEXT("no property named '%s'"), *SegmentName);
			if (SegmentIndex != INDEX_NONE && !FullPath.IsEmpty() && FullPath != SegmentName)
			{
				Message += FString::Printf(TEXT(" (segment %d of '%s')"), SegmentIndex + 1, *FullPath);
			}
			if (!ContainerName.IsEmpty())
			{
				Message += FString::Printf(TEXT(" on '%s'"), *ContainerName);
			}
			return Message + FString::Printf(TEXT("; properties there: %s"), *Candidates);
		}

		enum class ESubscriptKind : uint8 { None, Index, Key };

		struct FSegment
		{
			FString Name;
			ESubscriptKind Subscript = ESubscriptKind::None;
			int32 Index = INDEX_NONE;
			FString Key;
		};

		/** Splits "A.B[2].C[\"x.y\"]" into segments. */
		TValueOrError<TArray<FSegment>, FString> ParsePath(const FString& Path)
		{
			TArray<FSegment> Segments;
			int32 Pos = 0;
			const int32 Len = Path.Len();
			while (Pos < Len)
			{
				FSegment Segment;
				const int32 NameStart = Pos;
				while (Pos < Len && Path[Pos] != TEXT('.') && Path[Pos] != TEXT('['))
				{
					++Pos;
				}
				Segment.Name = Path.Mid(NameStart, Pos - NameStart).TrimStartAndEnd();
				if (Segment.Name.IsEmpty())
				{
					return MakeError(FString::Printf(TEXT("RootPath '%s' has an empty segment"), *Path));
				}
				if (Pos < Len && Path[Pos] == TEXT('['))
				{
					++Pos;
					FString Inner;
					if (Pos < Len && Path[Pos] == TEXT('"'))
					{
						++Pos;
						while (Pos < Len && Path[Pos] != TEXT('"'))
						{
							if (Path[Pos] == TEXT('\\') && Pos + 1 < Len)
							{
								++Pos;
							}
							Inner.AppendChar(Path[Pos++]);
						}
						if (Pos >= Len)
						{
							return MakeError(FString::Printf(TEXT("RootPath '%s' has an unterminated quoted key"), *Path));
						}
						++Pos; // closing quote
						Segment.Subscript = ESubscriptKind::Key;
						Segment.Key = Inner;
					}
					else
					{
						while (Pos < Len && Path[Pos] != TEXT(']'))
						{
							Inner.AppendChar(Path[Pos++]);
						}
						Inner.TrimStartAndEndInline();
						if (!Inner.IsEmpty() && Inner.IsNumeric() && !Inner.Contains(TEXT(".")) && !Inner.StartsWith(TEXT("-")))
						{
							Segment.Subscript = ESubscriptKind::Index;
							Segment.Index = FCString::Atoi(*Inner);
						}
						else
						{
							Segment.Subscript = ESubscriptKind::Key;
							Segment.Key = Inner;
						}
					}
					if (Pos >= Len || Path[Pos] != TEXT(']'))
					{
						return MakeError(FString::Printf(TEXT("RootPath '%s' has an unclosed '['"), *Path));
					}
					++Pos;
				}
				Segments.Add(MoveTemp(Segment));
				if (Pos < Len)
				{
					if (Path[Pos] != TEXT('.'))
					{
						return MakeError(FString::Printf(TEXT("RootPath '%s': expected '.' at position %d"), *Path, Pos));
					}
					++Pos;
					if (Pos >= Len)
					{
						return MakeError(FString::Printf(TEXT("RootPath '%s' ends in '.'"), *Path));
					}
				}
			}
			return MakeValue(MoveTemp(Segments));
		}

		/** True when the walk must stop here rather than descend into fields. */
		bool IsLeaf(const FProperty* Property)
		{
			return Property->IsA<FObjectPropertyBase>();
		}

		bool PassesNameFilter(const FString& WireKey, const FProperty* Property, const FRegexPattern* NameFilter)
		{
			if (!NameFilter || FRegexMatcher(*NameFilter, WireKey).FindNext())
			{
				return true;
			}
#if WITH_METADATA
			const FString& Keywords = Property->GetMetaData(TEXT("Keywords"));
			return !Keywords.IsEmpty() && FRegexMatcher(*NameFilter, Keywords).FindNext();
#else
			return false;
#endif
		}

		FString RenderCountBracket(int32 Num)
		{
			if (Num <= 0)
			{
				return TEXT("[]");
			}
			return Num == 1 ? TEXT("[0]") : FString::Printf(TEXT("[0..%d]"), Num - 1);
		}

		FString EscapeMapKey(const FString& Key)
		{
			FString Escaped = Key.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
			Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
			return Escaped;
		}

		FString RenderBracket(const FProperty* Property, const void* ValuePtr)
		{
			if (Property->ArrayDim > 1)
			{
				return RenderCountBracket(Property->ArrayDim);
			}
			const bool bIsContainer = Property->IsA<FArrayProperty>() ||
				Property->IsA<FSetProperty>() || Property->IsA<FMapProperty>();
			if (!bIsContainer)
			{
				return FString();
			}
			if (!ValuePtr)
			{
				return TEXT("[?]");
			}
			if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
			{
				FScriptMapHelper Helper(MapProperty, ValuePtr);
				TArray<FString> Keys;
				for (int32 Index = 0; Index < Helper.GetMaxIndex() && Keys.Num() < MapKeyCap; ++Index)
				{
					if (!Helper.IsValidIndex(Index))
					{
						continue;
					}
					FString Key;
					MapProperty->KeyProp->ExportTextItem_Direct(Key, Helper.GetKeyPtr(Index), nullptr, nullptr, PPF_None);
					Keys.Add(FString::Printf(TEXT("\"%s\""), *EscapeMapKey(Key)));
				}
				if (Keys.IsEmpty())
				{
					return TEXT("[]");
				}
				const int32 Withheld = Helper.Num() - Keys.Num();
				return Withheld > 0
					? FString::Printf(TEXT("[%s, ... +%d more]"), *FString::Join(Keys, TEXT(", ")), Withheld)
					: FString::Printf(TEXT("[%s]"), *FString::Join(Keys, TEXT(", ")));
			}
			if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
			{
				const int32 Num = FScriptSetHelper(SetProperty, ValuePtr).Num();
				return Num <= 0 ? TEXT("[]") : FString::Printf(TEXT("[%d]"), Num);
			}
			const FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(Property);
			return RenderCountBracket(FScriptArrayHelper(ArrayProperty, ValuePtr).Num());
		}

		TArray<FString> RenderProperty(const FProperty* Property, const void* ValuePtr, bool bUserVisiblePropertiesOnly,
			const FRegexPattern* NameFilter, int32 RemainingDepth, TSet<const UStruct*>& StructsOnStack, const FString& ParentPath);

		TOptional<TArray<FString>> RenderChildren(
			const FProperty* Property, const void* ValuePtr, bool bUserVisiblePropertiesOnly,
			const FRegexPattern* NameFilter, int32 RemainingDepth, TSet<const UStruct*>& StructsOnStack, const FString& ParentPath)
		{
			TArray<FString> Children;
			if (IsLeaf(Property) || Property->IsA<FSetProperty>())
			{
				return Children;
			}
			const FProperty* InnerProperty = Property;
			const void* InnerValuePtr = ValuePtr;
			if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				InnerProperty = ArrayProperty->Inner;
				InnerValuePtr = nullptr;
			}
			else if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
			{
				InnerProperty = MapProperty->ValueProp;
				InnerValuePtr = nullptr;
			}
			else if (Property->ArrayDim > 1)
			{
				InnerValuePtr = nullptr;
			}

			const FStructProperty* StructProperty = CastField<FStructProperty>(InnerProperty);
			if (!StructProperty || IsLeaf(StructProperty))
			{
				return Children;
			}
			if (StructsOnStack.Contains(StructProperty->Struct))
			{
				return NullOpt;
			}
			if (--RemainingDepth <= 0)
			{
				return NullOpt;
			}

			StructsOnStack.Add(StructProperty->Struct);
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				if (!IsPropertyListable(StructProperty->Struct, *It, bUserVisiblePropertiesOnly))
				{
					continue;
				}
				const void* ChildValuePtr = InnerValuePtr ? It->ContainerPtrToValuePtr<void>(InnerValuePtr) : nullptr;
				Children.Append(RenderProperty(
					*It, ChildValuePtr, bUserVisiblePropertiesOnly, NameFilter, RemainingDepth, StructsOnStack, ParentPath));
			}
			StructsOnStack.Remove(StructProperty->Struct);
			return Children;
		}

		TArray<FString> RenderProperty(
			const FProperty* Property, const void* ValuePtr, bool bUserVisiblePropertiesOnly,
			const FRegexPattern* NameFilter, int32 RemainingDepth,
			TSet<const UStruct*>& StructsOnStack, const FString& ParentPath)
		{
			const FString WireKey = ToWireKey(Property);
			const FString Path = ParentPath.IsEmpty() ? WireKey : ParentPath + TEXT(".") + WireKey;
			const bool bPassesFilter = PassesNameFilter(WireKey, Property, NameFilter);
			TOptional<TArray<FString>> Descendants = RenderChildren(Property, ValuePtr, bUserVisiblePropertiesOnly,
				bPassesFilter ? nullptr : NameFilter, RemainingDepth, StructsOnStack, Path);
			const bool bHasDescendants = Descendants.IsSet() && !Descendants->IsEmpty();
			if (!bPassesFilter && !bHasDescendants)
			{
				return {};
			}

			TArray<FString> Lines;
			if (!Descendants.IsSet())
			{
				Lines.Add(Path + RenderBracket(Property, ValuePtr) + TEXT(" {...}"));
				return Lines;
			}
			if (bPassesFilter)
			{
				Lines.Add(Path + RenderBracket(Property, ValuePtr));
			}
			Lines.Append(MoveTemp(*Descendants));
			return Lines;
		}

		FString DescribeLegalRange(int32 Num)
		{
			if (Num <= 0)
			{
				return TEXT("no legal index (the container is empty)");
			}
			return Num == 1 ? TEXT("0") : FString::Printf(TEXT("0..%d"), Num - 1);
		}

		/** Applies Segment's subscript to Property at ValuePtr; the element's property and memory, or an error. */
		TValueOrError<TPair<const FProperty*, const void*>, FString> ApplySubscript(
			const FSegment& Segment, const FProperty* Property, const void* ValuePtr)
		{
			if (Segment.Subscript == ESubscriptKind::None)
			{
				return MakeValue(TPair<const FProperty*, const void*>(Property, ValuePtr));
			}
			const bool bIndex = Segment.Subscript == ESubscriptKind::Index;
			if (Property->IsA<FSetProperty>())
			{
				return MakeError(FString::Printf(TEXT("'%s' does not support index access"), *Segment.Name));
			}
			const bool bAddressable = Property->ArrayDim > 1 || Property->IsA<FArrayProperty>() || Property->IsA<FMapProperty>();
			if (!bAddressable)
			{
				return MakeError(FString::Printf(TEXT("'%s' does not support %s access"),
					*Segment.Name, bIndex ? TEXT("index") : TEXT("key")));
			}
			if (!ValuePtr)
			{
				return MakeError(FString::Printf(
					TEXT("cannot %s '%s': no instance was listed; list an object, or omit the "
						"subscript to describe the element type"),
					bIndex ? TEXT("index") : TEXT("key"), *Segment.Name));
			}
			if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
			{
				FString KeyText = bIndex ? FString::FromInt(Segment.Index) : Segment.Key;
				FScriptMapHelper Helper(MapProperty, ValuePtr);
				for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
				{
					if (!Helper.IsValidIndex(Index))
					{
						continue;
					}
					FString Key;
					MapProperty->KeyProp->ExportTextItem_Direct(Key, Helper.GetKeyPtr(Index), nullptr, nullptr, PPF_None);
					if (Key.Equals(KeyText, ESearchCase::CaseSensitive))
					{
						return MakeValue(TPair<const FProperty*, const void*>(MapProperty->ValueProp, Helper.GetValuePtr(Index)));
					}
				}
				return MakeError(FString::Printf(TEXT("no key \"%s\" in '%s'"), *EscapeMapKey(KeyText), *Segment.Name));
			}
			if (!bIndex)
			{
				return MakeError(FString::Printf(TEXT("'%s' does not support key access"), *Segment.Name));
			}
			if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
				if (!Helper.IsValidIndex(Segment.Index))
				{
					return MakeError(FString::Printf(TEXT("index %d out of range for '%s' (valid range %s)"),
						Segment.Index, *Segment.Name, *DescribeLegalRange(Helper.Num())));
				}
				return MakeValue(TPair<const FProperty*, const void*>(ArrayProperty->Inner, Helper.GetRawPtr(Segment.Index)));
			}
			// A static C-array.
			if (Segment.Index < 0 || Segment.Index >= Property->ArrayDim)
			{
				return MakeError(FString::Printf(TEXT("index %d out of range for '%s' (valid range %s)"),
					Segment.Index, *Segment.Name, *DescribeLegalRange(Property->ArrayDim)));
			}
			return MakeValue(TPair<const FProperty*, const void*>(
				Property, static_cast<const uint8*>(ValuePtr) + Segment.Index * Property->GetElementSize()));
		}

		TSharedPtr<FJsonObject> FindPropertiesOwner(const TSharedRef<FJsonObject>& Node)
		{
			if (Node->HasField(TEXT("properties")))
			{
				return Node;
			}
			const TSharedPtr<FJsonObject>* Wrapped = nullptr;
			if ((Node->TryGetObjectField(TEXT("items"), Wrapped) ||
					Node->TryGetObjectField(TEXT("additionalProperties"), Wrapped)) &&
				Wrapped && Wrapped->IsValid() && (*Wrapped)->HasField(TEXT("properties")))
			{
				return *Wrapped;
			}
			return nullptr;
		}

		void PruneBelowDepth(const TSharedRef<FJsonObject>& Node, int32 RemainingDepth)
		{
			TSharedPtr<FJsonObject> Owner = FindPropertiesOwner(Node);
			if (!Owner)
			{
				return;
			}
			if (RemainingDepth <= 0)
			{
				Owner->RemoveField(TEXT("properties"));
				Owner->RemoveField(TEXT("required"));
				return;
			}
			const TSharedPtr<FJsonObject>* NestedProperties = nullptr;
			if (!Owner->TryGetObjectField(TEXT("properties"), NestedProperties))
			{
				return;
			}
			for (const auto& Pair : (*NestedProperties)->Values)
			{
				if (const TSharedPtr<FJsonObject> ChildNode = Pair.Value->AsObject())
				{
					PruneBelowDepth(ChildNode.ToSharedRef(), RemainingDepth - 1);
				}
			}
		}
	}

	bool IsPropertyListable(const UStruct* Owner, const FProperty* Property, bool bUserVisiblePropertiesOnly)
	{
		if (!Property || Property->HasAnyPropertyFlags(CPF_Deprecated))
		{
			return false;
		}
		if (!bUserVisiblePropertiesOnly)
		{
			return true;
		}
		// 5.8's ToolsetJson::GetCheckFlags(true), which 5.8's schema generator uses.
		EPropertyFlags CheckFlags = CPF_BlueprintVisible | CPF_BlueprintAssignable;
		if (GEditor)
		{
			CheckFlags |= CPF_Edit;
		}
		if (!Property->HasAnyPropertyFlags(CheckFlags))
		{
			return false;
		}
		return !EnumHasAnyFlags(PropertyAccessUtil::CanGetPropertyValue(Property), EPropertyAccessResultFlags::PermissionDenied);
	}

	TValueOrError<TArray<FString>, FString> RenderInstanceNames(
		const UStruct* Struct, const void* Container, bool bUserVisiblePropertiesOnly,
		const FString& RootPath, const FString& NameFilter, int32 MaxDepth)
	{
		TOptional<FRegexPattern> NameFilterPattern;
		if (!NameFilter.IsEmpty())
		{
			NameFilterPattern.Emplace(NameFilter, ERegexPatternFlags::CaseInsensitive);
		}

		const UStruct* CurrentStruct = Struct;
		const void* CurrentContainer = Container;

		if (!RootPath.IsEmpty())
		{
			TValueOrError<TArray<FSegment>, FString> Parsed = ParsePath(RootPath);
			if (Parsed.HasError())
			{
				return MakeError(Parsed.GetError());
			}
			const TArray<FSegment>& Segments = Parsed.GetValue();
			for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
			{
				const FSegment& Segment = Segments[SegmentIndex];
				FProperty* Property = FindPropertyByName(CurrentStruct, Segment.Name);
				if (!Property || !IsPropertyListable(CurrentStruct, Property, bUserVisiblePropertiesOnly))
				{
					return MakeError(DescribeUnknownSegment(CurrentStruct ? CurrentStruct->GetName() : FString(),
						ListWireNames(CurrentStruct), Segment.Name, SegmentIndex, RootPath));
				}
				const void* ValuePtr = CurrentContainer ? Property->ContainerPtrToValuePtr<void>(CurrentContainer) : nullptr;
				TValueOrError<TPair<const FProperty*, const void*>, FString> Subscripted = ApplySubscript(Segment, Property, ValuePtr);
				if (Subscripted.HasError())
				{
					return MakeError(Subscripted.GetError());
				}
				const FProperty* ResolvedProperty = Subscripted.GetValue().Key;
				const void* ResolvedAddress = Subscripted.GetValue().Value;

				if (Segment.Subscript == ESubscriptKind::None)
				{
					if (ResolvedProperty->IsA<FSetProperty>())
					{
						return MakeError(FString::Printf(
							TEXT("'%s' is a set: its elements cannot be addressed individually, "
								"so there is nothing to list under it"),
							*Segment.Name));
					}
					if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(ResolvedProperty))
					{
						ResolvedProperty = ArrayProperty->Inner;
						ResolvedAddress = nullptr;
					}
					else if (const FMapProperty* MapProperty = CastField<FMapProperty>(ResolvedProperty))
					{
						ResolvedProperty = MapProperty->ValueProp;
						ResolvedAddress = nullptr;
					}
					else if (ResolvedProperty->ArrayDim > 1)
					{
						ResolvedAddress = nullptr;
					}
				}

				const FStructProperty* StructProperty = CastField<FStructProperty>(ResolvedProperty);
				if (!StructProperty || IsLeaf(StructProperty))
				{
					return MakeError(FString::Printf(TEXT("'%s' does not contain properties to list"), *Segment.Name));
				}
				CurrentStruct = StructProperty->Struct;
				CurrentContainer = ResolvedAddress;
			}
		}

		const int32 RemainingDepth = MaxDepth <= 0 ? MAX_int32 : MaxDepth;
		TSet<const UStruct*> StructsOnStack{ CurrentStruct };
		TArray<FString> Lines;
		for (TFieldIterator<FProperty> It(CurrentStruct); It; ++It)
		{
			if (!IsPropertyListable(CurrentStruct, *It, bUserVisiblePropertiesOnly))
			{
				continue;
			}
			const void* ValuePtr = CurrentContainer ? It->ContainerPtrToValuePtr<void>(CurrentContainer) : nullptr;
			Lines.Append(RenderProperty(
				*It, ValuePtr, bUserVisiblePropertiesOnly, NameFilterPattern.GetPtrOrNull(), RemainingDepth, StructsOnStack, FString()));
		}
		return MakeValue(MoveTemp(Lines));
	}

	TValueOrError<TSharedPtr<FJsonObject>, FString> ExtractSchemaSubtree(
		const TSharedRef<FJsonObject>& PropertiesSchema, const UStruct* Struct,
		const FString& RootPath, int32 MaxDepth)
	{
		TSharedPtr<FJsonObject> CurrentProperties = PropertiesSchema;

		if (!RootPath.IsEmpty())
		{
			TValueOrError<TArray<FSegment>, FString> Parsed = ParsePath(RootPath);
			if (Parsed.HasError())
			{
				return MakeError(Parsed.GetError());
			}
			const TArray<FSegment>& Segments = Parsed.GetValue();
			for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
			{
				const FSegment& Segment = Segments[SegmentIndex];
				TSharedPtr<FJsonObject> ResolvedNode;
				TArray<FString> Siblings;
				for (const auto& Pair : CurrentProperties->Values)
				{
					const FString Key(Pair.Key.ToView());
					Siblings.Add(Key);
					if (!ResolvedNode && Key.Equals(Segment.Name, ESearchCase::IgnoreCase))
					{
						ResolvedNode = Pair.Value->AsObject();
					}
				}
				if (!ResolvedNode)
				{
					return MakeError(DescribeUnknownSegment(
						SegmentIndex == 0 && Struct ? Struct->GetName() : FString(),
						Siblings.IsEmpty() ? FString(TEXT("none")) : FString::Join(Siblings, TEXT(", ")),
						Segment.Name, SegmentIndex, RootPath));
				}
				if (Segment.Subscript != ESubscriptKind::None)
				{
					return MakeError(FString::Printf(
						TEXT("cannot subscript '%s': a schema describes types, so there is no element "
							"to address; omit the subscript to describe the element type"),
						*Segment.Name));
				}

				const TSharedPtr<FJsonObject>* Wrapped = nullptr;
				const bool bHasItems = ResolvedNode->TryGetObjectField(TEXT("items"), Wrapped);
				const bool bIsMapLike = !bHasItems && ResolvedNode->TryGetObjectField(TEXT("additionalProperties"), Wrapped);
				bool bUniqueItems = false;
				ResolvedNode->TryGetBoolField(TEXT("uniqueItems"), bUniqueItems);
				const bool bIsSet = bHasItems && bUniqueItems;
				if (bIsSet)
				{
					return MakeError(FString::Printf(
						TEXT("'%s' is a set: its elements cannot be addressed individually, "
							"so there is nothing to list under it"),
						*Segment.Name));
				}
				if ((bHasItems || bIsMapLike) && Wrapped && Wrapped->IsValid())
				{
					ResolvedNode = *Wrapped;
				}

				const TSharedPtr<FJsonObject>* NestedProperties = nullptr;
				if (!ResolvedNode || !ResolvedNode->TryGetObjectField(TEXT("properties"), NestedProperties))
				{
					return MakeError(FString::Printf(TEXT("'%s' does not contain properties to list"), *Segment.Name));
				}
				CurrentProperties = *NestedProperties;
			}
		}

		if (MaxDepth > 0)
		{
			for (const auto& Pair : CurrentProperties->Values)
			{
				if (const TSharedPtr<FJsonObject> ChildNode = Pair.Value->AsObject())
				{
					PruneBelowDepth(ChildNode.ToSharedRef(), MaxDepth - 1);
				}
			}
		}
		return MakeValue(CurrentProperties);
	}
}
