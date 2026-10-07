// Games by Hyper 2026.

#include "HyperAIStudioNames.h"

#include "Misc/PackageName.h"
#include "UObject/EnumProperty.h"
#include "UObject/Package.h"
#include "UObject/PropertyOptional.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::Names::Private
{
	/** The value as a reply quotes it: a pasted blob is cut, so the diagnostic stays readable. */
	FString Shown(const FStringView Text)
	{
		constexpr int32 Most = 80;
		return Text.Len() <= Most ? FString(Text) : FString(Text.Left(Most)) + TEXT("...");
	}

	bool Fail(FString* OutReason, const FString& Reason)
	{
		if (OutReason) *OutReason = Reason;
		return false;
	}

	const TCHAR* FieldOf(const TCHAR* Field)
	{
		return Field && *Field ? Field : TEXT("The path");
	}

	FString Describe(const FPackageName::EErrorCode Code)
	{
		using EErrorCode = FPackageName::EErrorCode;
		switch (Code)
		{
		case EErrorCode::PackageNameEmptyPath: return TEXT("it is empty");
		case EErrorCode::PackageNamePathNotMounted: return TEXT("it is not under a content root this project can save to, like /Game");
		case EErrorCode::PackageNamePathIsMemoryOnly: return TEXT("it is under a memory-only root");
		case EErrorCode::PackageNameSpacesNotAllowed: return TEXT("it has a space");
		case EErrorCode::PackageNameContainsInvalidCharacters: return FString::Printf(TEXT("it has one of %s or a space"), TEXT("\\ : * ? \" < > | ' , . & ! ~ @ #"));
		case EErrorCode::LongPackageNames_PathTooShort: return TEXT("it has no root and name, like /Game/Name");
		case EErrorCode::LongPackageNames_PathWithNoStartingSlash: return TEXT("it does not start with '/', like /Game/Folder/Asset");
		case EErrorCode::LongPackageNames_PathWithTrailingSlash: return TEXT("it ends with '/'");
		case EErrorCode::LongPackageNames_PathWithDoubleSlash: return TEXT("it has a double slash");
		default: return TEXT("it is not a package name");
		}
	}

	bool FitsLength(const FStringView Text, const TCHAR* Field, FString* OutReason)
	{
		if (Text.Len() <= FHyperAIStudioNames::MaxLength) return true;
		return Fail(OutReason, FString::Printf(TEXT("%s is %d characters long; a name or path is at most %d."), FieldOf(Field), Text.Len(), FHyperAIStudioNames::MaxLength));
	}

	/** Fields every tool's requests use for free text, which nothing makes a name of. */
	const TCHAR* const FreeTextFields[] = {TEXT("Tooltip"), TEXT("Description"), TEXT("Comment"), TEXT("Notes"), TEXT("Note"), TEXT("Code"), TEXT("Script"),
		TEXT("Python"), TEXT("Prompt"), TEXT("Message"), TEXT("Markdown"), TEXT("Json"), TEXT("Text")};

	/** A request's strings, field by field, through its structs and containers. */
	struct FRequestWalk
	{
		TConstArrayView<const TCHAR*> FreeText;
		FString* OutReason = nullptr;

		bool IsFree(const FProperty& Property) const
		{
			const FString Name = Property.GetName();
			for (const TCHAR* Free : FreeTextFields)
			{
				if (Name.Equals(Free, ESearchCase::IgnoreCase)) return true;
			}
			for (const TCHAR* Free : FreeText)
			{
				if (Name.Equals(Free, ESearchCase::IgnoreCase)) return true;
			}
			return false;
		}

		bool Value(const FProperty& Property, const void* Data, const FString& Path) const
		{
			if (const FStrProperty* String = CastField<FStrProperty>(&Property))
			{
				return FHyperAIStudioNames::IsName(String->GetPropertyValue(Data), *Path, OutReason);
			}
			if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
			{
				return Fields(*Struct->Struct, Data, Path + TEXT("."));
			}
			if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
			{
				FScriptArrayHelper Helper(Array, Data);
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					if (!Value(*Array->Inner, Helper.GetRawPtr(Index), FString::Printf(TEXT("%s[%d]"), *Path, Index))) return false;
				}
				return true;
			}
			if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
			{
				FScriptSetHelper Helper(Set, Data);
				for (FScriptSetHelper::FIterator It(Helper); It; ++It)
				{
					if (!Value(*Set->ElementProp, Helper.GetElementPtr(It), Path + TEXT("[]"))) return false;
				}
				return true;
			}
			if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
			{
				FScriptMapHelper Helper(Map, Data);
				for (FScriptMapHelper::FIterator It(Helper); It; ++It)
				{
					if (!Value(*Map->KeyProp, Helper.GetKeyPtr(It), Path + TEXT("[key]")) || !Value(*Map->ValueProp, Helper.GetValuePtr(It), Path + TEXT("[value]"))) return false;
				}
				return true;
			}
			return true;
		}

		bool Fields(const UStruct& Struct, const void* Data, const FString& Prefix) const
		{
			for (TFieldIterator<FProperty> It(&Struct); It; ++It)
			{
				if (IsFree(**It)) continue;
				for (int32 Index = 0; Index < It->ArrayDim; ++Index)
				{
					if (!Value(**It, It->ContainerPtrToValuePtr<void>(Data, Index), Prefix + It->GetName())) return false;
				}
			}
			return true;
		}
	};

	/** Text's parts outside quotes, split where import text splits them: a struct's (A=1,B=/Game/X.X), a list's (a,b). */
	TArray<FStringView> UnquotedParts(const FStringView Text)
	{
		TArray<FStringView> Parts;
		int32 Start = 0;
		bool bQuoted = false;
		auto Cut = [&](const int32 End)
		{
			if (End > Start) Parts.Add(Text.Mid(Start, End - Start));
			Start = End + 1;
		};
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR Char = Text[Index];
			if (Char == TEXT('"'))
			{
				if (!bQuoted) Cut(Index);
				else Start = Index + 1;
				bQuoted = !bQuoted;
			}
			else if (!bQuoted && (Char == TEXT('(') || Char == TEXT(')') || Char == TEXT(',') || Char == TEXT('=') || FChar::IsWhitespace(Char)))
			{
				Cut(Index);
			}
		}
		if (!bQuoted) Cut(Text.Len());
		return Parts;
	}
}

bool FHyperAIStudioNames::IsName(const FStringView Text, const TCHAR* Field, FString* OutReason)
{
	using namespace HyperAIStudio::Names::Private;
	return FitsLength(Text, Field ? Field : TEXT("The name"), OutReason);
}

bool FHyperAIStudioNames::IsObjectPath(const FStringView Path, const TCHAR* Field, FString* OutReason)
{
	using namespace HyperAIStudio::Names::Private;
	if (Path.IsEmpty())
	{
		return Fail(OutReason, FString::Printf(TEXT("%s is empty; give a path like /Game/Folder/Asset."), FieldOf(Field)));
	}
	if (!FitsLength(Path, Field, OutReason)) return false;
	int32 Quote = INDEX_NONE;
	if (Path.FindChar(TEXT('\''), Quote) || Path.FindChar(TEXT('"'), Quote))
	{
		return Fail(OutReason, FString::Printf(TEXT("%s '%s' has a quote; give the path alone, like /Game/Folder/Asset.Asset."), FieldOf(Field), *Shown(Path)));
	}
	// Everything before the first '.' or ':' is the package, which LoadObject makes when nothing is there to load.
	int32 End = Path.Len();
	for (int32 Index = 0; Index < Path.Len(); ++Index)
	{
		if (Path[Index] == TEXT('.') || Path[Index] == TEXT(':'))
		{
			End = Index;
			break;
		}
	}
	FPackageName::EErrorCode Code = FPackageName::EErrorCode::PackageNameUnknown;
	if (!FPackageName::IsValidTextForLongPackageName(Path.Left(End), &Code))
	{
		return Fail(OutReason, FString::Printf(TEXT("%s '%s' is not a path: %s."), FieldOf(Field), *Shown(Path), *Describe(Code)));
	}
	return true;
}

bool FHyperAIStudioNames::IsPackagePath(const FStringView Path, const TCHAR* Field, FString* OutReason)
{
	using namespace HyperAIStudio::Names::Private;
	if (!IsObjectPath(Path, Field, OutReason)) return false;
	FPackageName::EErrorCode Code = FPackageName::EErrorCode::PackageNameUnknown;
	if (!FPackageName::IsValidLongPackageName(Path, /*bIncludeReadOnlyRoots=*/false, &Code))
	{
		return Fail(OutReason, FString::Printf(TEXT("%s '%s' cannot be a package: %s."), FieldOf(Field), *Shown(Path), *Describe(Code)));
	}
	return true;
}

bool FHyperAIStudioNames::IsFolderPath(const FStringView Path, const TCHAR* Field, FString* OutReason)
{
	using namespace HyperAIStudio::Names::Private;
	if (Path.IsEmpty())
	{
		return Fail(OutReason, FString::Printf(TEXT("%s is empty; give a folder like /Game/Folder."), FieldOf(Field)));
	}
	if (!FitsLength(Path, Field, OutReason)) return false;
	if (Path.EndsWith(TEXT('/')))
	{
		return Fail(OutReason, FString::Printf(TEXT("%s '%s' ends with '/'; give the folder without it, like /Game/Folder."), FieldOf(Field), *Shown(Path)));
	}
	// A root alone (/Game) is a folder but not a package, so the folder is checked as the package a name in it makes.
	FPackageName::EErrorCode Code = FPackageName::EErrorCode::PackageNameUnknown;
	if (!FPackageName::IsValidLongPackageName(FString(Path) + TEXT("/A"), /*bIncludeReadOnlyRoots=*/false, &Code))
	{
		return Fail(OutReason, FString::Printf(TEXT("%s '%s' cannot hold packages: %s."), FieldOf(Field), *Shown(Path), *Describe(Code)));
	}
	return true;
}

bool FHyperAIStudioNames::IsFreePackage(const FStringView PackagePath)
{
	const FString Path(PackagePath);
	if (!IsObjectPath(Path, nullptr) || FPackageName::DoesPackageExist(Path)) return false;
	UPackage* Package = FindPackage(nullptr, *Path);
	return !Package || !FindObject<UObject>(Package, *FPackageName::GetShortName(Path));
}

bool FHyperAIStudioNames::CheckRequest(const UScriptStruct& Struct, const void* Request, const TConstArrayView<const TCHAR*> FreeText, FString* OutReason)
{
	using namespace HyperAIStudio::Names::Private;
	FRequestWalk Walk;
	Walk.FreeText = FreeText;
	Walk.OutReason = OutReason;
	return !Request || Walk.Fields(Struct, Request, FString());
}

bool FHyperAIStudioNames::CanImportText(const FProperty& Property, const FStringView Text, const TCHAR* Field, FString* OutReason)
{
	using namespace HyperAIStudio::Names::Private;
	if (Property.IsA<FStrProperty>() || Property.IsA<FTextProperty>()) return true;
	if (Property.IsA<FNameProperty>()) return IsName(Text, Field, OutReason);
	if (Property.IsA<FObjectPropertyBase>() || Property.IsA<FInterfaceProperty>())
	{
		FString Reference(Text);
		Reference.TrimStartAndEndInline();
		if (Reference.IsEmpty() || Reference.Equals(TEXT("None"))) return true;
		// Class'/Game/A.A' names the object in quotes.
		if (Reference.EndsWith(TEXT("'"))) Reference = FPackageName::ExportTextPathToObjectPath(Reference);
		if (Reference.StartsWith(TEXT("/"))) return IsObjectPath(Reference, Field, OutReason);
		// A name relative to the object it goes in, such as a sibling component's.
		if (Reference.Contains(TEXT("//")))
		{
			return Fail(OutReason, FString::Printf(TEXT("%s '%s' has a double slash."), FieldOf(Field), *Shown(Reference)));
		}
		return IsName(Reference, Field, OutReason);
	}
	// A struct, a container, an enum or a number: what it references sits between its delimiters.
	for (const FStringView Part : UnquotedParts(Text))
	{
		if (Part.Contains(TEXT("//")))
		{
			return Fail(OutReason, FString::Printf(TEXT("%s '%s' has a double slash."), FieldOf(Field), *Shown(Part)));
		}
		if (!IsName(Part, Field, OutReason)) return false;
	}
	return true;
}
