// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: the UToolsetLibrary functions UE 5.9 added, copied from 5.9's
// ToolsetRegistryEditor/Private/ToolsetRegistry/ToolsetLibrary.cpp (see ToolsetLibrary59.h), plus
// UE::PropertyEditor::IsClassAllowed from 5.9's PropertyEditor/Private/PropertyAccessCheck.cpp.

#include "ToolsetRegistry/ToolsetLibrary59.h"

#include "AssetScriptingUtils.h"
#include "ClassViewerFilter.h"
#include "ClassViewerModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "IAssetTools.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PropertyAccessCheck.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ToolsetRegistry/AssetHelpers.h"
#include "ToolsetRegistry/ToolsetJsonConverter.h"
#include "ToolsetRegistry/ToolsetLibrary.h"
#include "ToolsetRegistry/ToolsetPropertyListing59.h"

namespace UE::PropertyEditor
{
	bool IsClassAllowed(const UClass* Class)
	{
		if (!IsValid(Class))
		{
			return false;
		}

#if WITH_EDITOR
		FClassViewerModule* ClassViewerModule = FModuleManager::GetModulePtr<FClassViewerModule>("ClassViewer");
		if (!ClassViewerModule)
		{
			return true;
		}

		// The global filter is registered by hosts that restrict which classes are available. When
		// nothing is registered (a normal editor) there is no filtering and every class is allowed.
		const TSharedPtr<IClassViewerFilter>& GlobalFilter = ClassViewerModule->GetGlobalClassViewerFilter();
		if (!GlobalFilter.IsValid())
		{
			return true;
		}

		return GlobalFilter->IsClassAllowed(
			FClassViewerInitializationOptions(), Class, ClassViewerModule->CreateFilterFuncs());
#else
		return true;
#endif
	}
}

namespace
{
	// Raises the shared script error for an entry point handed an object that has been destroyed.
	void RaiseDestroyedError(const TCHAR* ToolName, const UObject* Object)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("%s: '%s' has been destroyed; query for a current one instead of reusing this reference."),
			ToolName, *Object->GetPathName()));
	}

	// UE 5.8: 5.9's Internal::JsonToString is private to its ToolsetRegistry; the same condensed writer.
	FString JsonToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	FString JsonToString(const TSharedRef<FJsonValue>& Value)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Value, FString(), Writer);
		return Out;
	}

	// UE 5.8: 5.8's ToolsetJson pipeline is private, but FToolsetJsonConverter exposes it to subclasses.
	struct FToolsetJsonAccess : public UE::ToolsetRegistry::FToolsetJsonConverter
	{
		using FToolsetJsonConverter::ToolsetPropertyToJsonData;
	};

	// A caller-supplied path may be a Content Browser virtual path or contain "..", neither of
	// which the root/prefix/visibility checks below understand directly.
	bool TryNormalizeVirtualPath(const FString& Path, FString& OutNormalizedPath)
	{
		return AssetScriptingUtils::TryConvertToInternalPath(Path, OutNormalizedPath);
	}
}

FString FToolsetLibrary59::ListStructProperties(
	const UStruct* Struct, bool bUserVisiblePropertiesOnly, const FString& RootPath, int32 MaxDepth)
{
	// UE 5.8: 5.8's ListStructProperties gives the whole schema; narrowing and pruning are applied to it.
	const FString Whole = UToolsetLibrary::ListStructProperties(Struct, bUserVisiblePropertiesOnly);
	if (Whole.IsEmpty() || (RootPath.IsEmpty() && MaxDepth <= 0))
	{
		return Whole;
	}
	TSharedPtr<FJsonObject> Properties;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Whole), Properties) || !Properties.IsValid())
	{
		return Whole;
	}
	TValueOrError<TSharedPtr<FJsonObject>, FString> Subtree =
		UE::ToolsetRegistry::Epic59::PropertyListing::ExtractSchemaSubtree(
			Properties.ToSharedRef(), Struct, RootPath, MaxDepth);
	if (Subtree.HasError())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("ListStructProperties on '%s' at '%s': %s"),
			*Struct->GetName(), *RootPath, *Subtree.GetError()));
		return FString();
	}
	return JsonToString(Subtree.GetValue().ToSharedRef());
}

TArray<FString> FToolsetLibrary59::ListObjectPropertyNames(
	const UObject* Object, bool bUserVisiblePropertiesOnly, const FString& RootPath, const FString& NameFilter, int32 MaxDepth)
{
	if (!Object)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Null object provided"));
		return {};
	}
	if (!IsValid(Object))
	{
		RaiseDestroyedError(TEXT("ListObjectPropertyNames"), Object);
		return {};
	}

	// A UStruct argument names a type rather than an instance, so the walk gets no memory and
	// containers report unknown occupancy.
	const UStruct* Struct = Cast<UStruct>(Object);
	const void* Container = Struct ? nullptr : static_cast<const void*>(Object);
	if (!Struct)
	{
		Struct = Object->GetClass();
	}

	TValueOrError<TArray<FString>, FString> Listing = UE::ToolsetRegistry::Epic59::PropertyListing::RenderInstanceNames(
		Struct, Container, bUserVisiblePropertiesOnly, RootPath, NameFilter, MaxDepth);
	if (Listing.HasError())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("ListObjectPropertyNames on '%s' (%s): %s"),
			*Object->GetPathName(), *Object->GetClass()->GetName(), *Listing.GetError()));
		return {};
	}
	return Listing.StealValue();
}

FString FToolsetLibrary59::StructToJson(const FInstancedStruct& Value)
{
	const UScriptStruct* Struct = Value.GetScriptStruct();
	if (!Struct)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("StructToJson: the value holds no struct"));
		return FString();
	}
	// Converted as a property of the struct's type, so a converter registered for the type, such as
	// FTransform's, converts it whole, as it would a tool parameter. Heap-allocated because an FField
	// gets its class from its operator new.
	const TUniquePtr<FStructProperty> Property = MakeUnique<FStructProperty>(FFieldVariant(), NAME_None);
	Property->Struct = const_cast<UScriptStruct*>(Struct);
	Property->SetElementSize(Struct->GetStructureSize());
	const TSharedPtr<FJsonValue> Json = FToolsetJsonAccess::ToolsetPropertyToJsonData(Property.Get(), Value.GetMemory());
	if (!Json.IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("StructToJson: could not convert a %s"), *Struct->GetName()));
		return FString();
	}
	return JsonToString(Json.ToSharedRef());
}

bool FToolsetLibrary59::IsClassAllowed(const UClass* Class)
{
	return UE::PropertyEditor::IsClassAllowed(Class);
}

TArray<FString> FToolsetLibrary59::GetPluginContentPaths(bool bIncludeEngine)
{
	TArray<FString> Paths;
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
	{
		// An enabled but explicitly loaded plugin is not mounted until something loads it, so its asset path
		// does not map to a root yet.
		if (!Plugin->CanContainContent() || !Plugin->IsMounted())
		{
			continue;
		}
		// GetLoadedFrom() classifies by origin, not by on-disk location, so plugins pulled in
		// via AdditionalPluginDirectories still classify correctly.
		const bool bUnderProject = Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project;
		const bool bUnderEngine = bIncludeEngine && Plugin->GetLoadedFrom() == EPluginLoadedFrom::Engine;
		if (!bUnderProject && !bUnderEngine)
		{
			continue;
		}
		const FString Mount = Plugin->GetMountedAssetPath();
		if (!Mount.IsEmpty())
		{
			Paths.Add(Mount);
		}
	}
	return Paths;
}

TArray<FString> FToolsetLibrary59::GetAssetContentRoots(bool bRequireVisible)
{
	TArray<FString> Roots;
	Roots.Add(FString::Printf(TEXT("/%s/"), *UEditorAssetLibrary::GetProjectRootAssetDirectory()));
	Roots.Append(GetPluginContentPaths(/*bIncludeEngine=*/true));
	if (bRequireVisible)
	{
		Roots.RemoveAll([](const FString& Root) { return !IsPathVisible(Root); });
	}
	return Roots;
}

bool FToolsetLibrary59::ValidateAssetPath(const FString& FolderPath, bool bRequireWritable)
{
	FString Error;
	if (!TryValidateAssetPath(FolderPath, bRequireWritable, Error))
	{
		UKismetSystemLibrary::RaiseScriptError(Error);
		return false;
	}
	return true;
}

bool FToolsetLibrary59::TryValidateAssetPath(const FString& FolderPath, bool bRequireWritable, FString& OutError)
{
	OutError.Reset();
	FString NormalizedPath;
	if (!TryNormalizeVirtualPath(FolderPath, NormalizedPath))
	{
		OutError = FString::Printf(
			TEXT("FolderPath \"%s\" contains an unresolvable \"..\"."), *FolderPath);
		return false;
	}

	// Test fixtures mount their own sandbox paths, which GetAssetContentRoots doesn't know about.
	FString ResolvedDir;
	if (FPackageName::TryConvertLongPackageNameToFilename(NormalizedPath, ResolvedDir) &&
		FPaths::ConvertRelativePathToFull(ResolvedDir).StartsWith(
			FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir())))
	{
		return true;
	}

	const TArray<FString> ValidRoots = GetAssetContentRoots();
	bool bUnderValidRoot = false;
	for (FString Root : ValidRoots)
	{
		if (Root.EndsWith(TEXT("/")))
		{
			Root.LeftChopInline(1);
		}
		if (NormalizedPath.Equals(Root, ESearchCase::IgnoreCase) ||
			NormalizedPath.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase))
		{
			bUnderValidRoot = true;
			break;
		}
	}

	if (!bUnderValidRoot)
	{
		// A project can have well over a hundred valid roots (one per content plugin), so listing
		// all of them makes this message unreadable. Show a handful and note how many were omitted.
		constexpr int32 MaxRootsToList = 15;
		FString RootsSummary;
		if (ValidRoots.Num() <= MaxRootsToList)
		{
			RootsSummary = FString::Join(ValidRoots, TEXT(", "));
		}
		else
		{
			TArray<FString> ShownRoots;
			ShownRoots.Append(ValidRoots.GetData(), MaxRootsToList);
			RootsSummary = FString::Printf(
				TEXT("%s, and %d more; look up the writable paths for the full list"),
				*FString::Join(ShownRoots, TEXT(", ")), ValidRoots.Num() - MaxRootsToList);
		}
		OutError = FString::Printf(
			TEXT("FolderPath \"%s\" is outside this project's valid asset roots (%s)."),
			*NormalizedPath, *RootsSummary);
		return false;
	}

	// A project can narrow writability further than "under some project plugin's root"; this is a
	// no-op when nothing has populated the list, which is the case for a plain .uproject.
	if (bRequireWritable && !IAssetTools::Get().AllPassWritableFolderFilter({ NormalizedPath }))
	{
		OutError = FString::Printf(
			TEXT("FolderPath \"%s\" is not writable in this project."), *NormalizedPath);
		return false;
	}

	return true;
}

bool FToolsetLibrary59::IsPathVisible(const FString& Path)
{
	return UE::ToolsetRegistry::IsPathVisible(Path);
}

bool FToolsetLibrary59::IsPathWritable(const FString& Path)
{
	FString NormalizedPath;
	if (!TryNormalizeVirtualPath(Path, NormalizedPath))
	{
		return false;
	}
	return IAssetTools::Get().AllPassWritableFolderFilter({ NormalizedPath });
}

FString FToolsetLibrary59::ResolveAssetPath(const FString& Path)
{
	FString NormalizedPath;
	return TryNormalizeVirtualPath(Path, NormalizedPath) ? NormalizedPath : Path;
}

FString FToolsetLibrary59::ConvertToVirtualPath(const FString& Path)
{
	return AssetScriptingUtils::ConvertToVirtualPath(Path);
}
