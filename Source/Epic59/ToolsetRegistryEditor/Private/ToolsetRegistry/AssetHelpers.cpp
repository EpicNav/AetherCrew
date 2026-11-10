// Copyright Epic Games, Inc. All Rights Reserved.

#include "ToolsetRegistry/AssetHelpers.h"

#include "AssetCompilingManager.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetScriptingUtils.h"
#include "AssetToolsModule.h"
#include "AsyncFunctionResult/AsyncFunctionResultUtils.h"
#include "Blueprint/BlueprintSupport.h"
#include "BlueprintEditorLibrary.h"
#include "Components/ActorComponent.h"
#include "ContentBrowserDataFilter.h"
#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserItem.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Factories/Factory.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "IContentBrowserDataModule.h"
#include "InterchangeManager.h"
#include "InterchangeResult.h"
#include "InterchangeResultsContainer.h"
#include "InterchangeTranslatorBase.h"
#include "Internationalization/Regex.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/NamePermissionList.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "SubobjectData.h"
#include "SubobjectDataBlueprintFunctionLibrary.h"
#include "SubobjectDataHandle.h"
#include "SubobjectDataSubsystem.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"
#include "ToolsetRegistry/ToolsetLibrary.h"
#include "ToolsetRegistry/ToolsetLibrary59.h" // UE 5.8: 5.9 UToolsetLibrary additions
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectGlobals.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AssetHelpers)

namespace UE::ToolsetRegistry
{
	UObject* CreateAsset(
		const FString& FolderPath, const FString& AssetName, UClass* AssetClass, UFactory* Factory)
	{
		if (FolderPath.IsEmpty() || AssetName.IsEmpty())
		{
			UKismetSystemLibrary::RaiseScriptError(TEXT("FolderPath and AssetName must not be empty."));
			return nullptr;
		}

		if (!FToolsetLibrary59::ValidateAssetPath(FolderPath)) // UE 5.8: 5.9 UToolsetLibrary addition
		{
			return nullptr;
		}

		const FSoftObjectPath ObjectPath(
			FTopLevelAssetPath(FName(*(FolderPath / AssetName)), FName(*AssetName)));

		const FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		if (AssetRegistryModule.Get().GetAssetByObjectPath(ObjectPath).IsValid())
		{
			UKismetSystemLibrary::RaiseScriptError(
				FString::Printf(TEXT("An asset already exists at %s."), *ObjectPath.ToString()));
			return nullptr;
		}

		IAssetTools& AssetTools =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UObject* NewAsset = AssetTools.CreateAsset(AssetName, FolderPath, AssetClass, Factory);
		if (!NewAsset || !NewAsset->IsA(AssetClass))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("Failed to create a %s at %s."), *AssetClass->GetName(), *ObjectPath.ToString()));
			return nullptr;
		}
		return NewAsset;
	}

	bool RequireFactorySupports(const FString& SourceFile, const UFactory* Factory)
	{
		const FString Ext = FPaths::GetExtension(SourceFile).ToLower();
		TSet<FString> Allowed;
		// Factory->Formats (the raw declared list), not Factory->GetFormats(): some factories
		// (e.g. UFbxFactory) override GetFormats() to dynamically hide extensions its caller
		// currently defers to Interchange for, even though pinning that factory on the import
		// task (as ImportAsset does below) still imports that extension successfully through
		// the legacy path. This check is "does the factory declare this extension at all", not
		// "would auto-detection currently route here" -- Formats matches that intent.
		for (const FString& Entry : Factory->Formats)
		{
			int32 SemicolonIndex = INDEX_NONE;
			const FString FormatExt = Entry.FindChar(';', SemicolonIndex) ? Entry.Left(SemicolonIndex) : Entry;
			Allowed.Add(FormatExt.ToLower());
		}
		if (!Allowed.Contains(Ext))
		{
			TArray<FString> SortedAllowed = Allowed.Array();
			SortedAllowed.Sort();
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("%s does not support \".%s\" files. Allowed: %s."),
				*Factory->GetClass()->GetName(), *Ext, *FString::Join(SortedAllowed, TEXT(", "))));
			return false;
		}
		return true;
	}

	bool RequireInterchangeSupports(
		const FString& SourceFile, EInterchangeTranslatorAssetType AssetType, const TCHAR* AssetTypeName)
	{
		const FString Ext = FPaths::GetExtension(SourceFile).ToLower();
		TSet<FString> Allowed;
		for (const FString& Entry : UInterchangeManager::GetInterchangeManager().GetSupportedAssetTypeFormats(AssetType))
		{
			int32 SemicolonIndex = INDEX_NONE;
			const FString FormatExt = Entry.FindChar(';', SemicolonIndex) ? Entry.Left(SemicolonIndex) : Entry;
			Allowed.Add(FormatExt.ToLower());
		}
		if (!Allowed.Contains(Ext))
		{
			TArray<FString> SortedAllowed = Allowed.Array();
			SortedAllowed.Sort();
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("Interchange does not support importing \".%s\" files as %s. Allowed: %s."),
				*Ext, AssetTypeName, *FString::Join(SortedAllowed, TEXT(", "))));
			return false;
		}
		return true;
	}

	bool CheckImportDestination(
		const FString& FolderPath, const FString& AssetName, bool bReplaceExisting, const UClass* ReplaceableClass)
	{
		UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
		if (!EditorAssetSubsystem)
		{
			return true;
		}

		const FString DestinationAssetPath = FToolsetLibrary59::ResolveAssetPath(FolderPath) / AssetName; // UE 5.8: 5.9 UToolsetLibrary addition
		if (EditorAssetSubsystem->DoesDirectoryExist(DestinationAssetPath))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("%s at %s already exists as a folder."), *AssetName, *FolderPath));
			return false;
		}
		if (!EditorAssetSubsystem->DoesAssetExist(DestinationAssetPath))
		{
			return true;
		}
		if (!bReplaceExisting)
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("%s at %s already exists. Pass bReplaceExisting=true to overwrite it."),
				*AssetName, *FolderPath));
			return false;
		}
		if (ReplaceableClass)
		{
			const FAssetData Existing = EditorAssetSubsystem->FindAssetData(DestinationAssetPath);
			const UClass* ExistingClass = Existing.GetClass();
			if (!ExistingClass || !ExistingClass->IsChildOf(ReplaceableClass))
			{
				UKismetSystemLibrary::RaiseScriptError(FString::Printf(
					TEXT("%s at %s is a %s, not a %s, so this import cannot replace it."),
					*AssetName, *FolderPath, *Existing.AssetClassPath.GetAssetName().ToString(),
					*ReplaceableClass->GetName()));
				return false;
			}
		}
		return true;
	}

	FString SaveImportedAssets(const TArray<UObject*>& Imported)
	{
		UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
		TArray<FString> Failed;
		for (UObject* Object : Imported)
		{
			if (Object && !(EditorAssetSubsystem && EditorAssetSubsystem->SaveLoadedAsset(Object, /*bOnlyIfIsDirty=*/false)))
			{
				Failed.Add(Object->GetPathName());
			}
		}
		if (Failed.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(
			TEXT("Imported, but failed to save: %s. The assets are still in memory; retry with AssetToolset.SaveAsset."),
			*FString::Join(Failed, TEXT(", ")));
	}

	namespace
	{
		// Joins the errors, and optionally warnings, Interchange reported for an import, so a failure
		// says why (e.g. a class clash, or an existing asset it declined to overwrite).
		FString DescribeInterchangeProblems(const UE::Interchange::FImportResult& Done, bool bIncludeWarnings)
		{
			TArray<FString> Messages;
			if (const UInterchangeResultsContainer* Results = Done.GetResults())
			{
				for (const UInterchangeResult* Entry : Results->GetResults())
				{
					if (Entry && (Entry->GetResultType() == EInterchangeResultType::Error
						|| (bIncludeWarnings && Entry->GetResultType() == EInterchangeResultType::Warning)))
					{
						Messages.AddUnique(Entry->GetText().ToString());
					}
				}
			}
			return FString::Join(Messages, TEXT(" "));
		}

		// The legacy factory-based import path (no async entry point) - only called from
		// ImportAsset's Factory-pinned branch below, which converts its RaiseScriptError calls
		// into the returned UAsyncImportResult's Error via FToolCallExceptionHandler.
		TArray<UObject*> ImportAssetSynchronously(
			const FString& FolderPath, const FString& AssetName, const FString& SourceFile,
			UFactory* Factory, UObject* Options, bool bReplaceExisting, bool bSave,
			const UClass* ReplaceableClass, bool bCheckDestination)
		{
			if (!IFileManager::Get().FileExists(*SourceFile))
			{
				UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Source file not found: %s"), *SourceFile));
				return {};
			}
			if (!FToolsetLibrary59::ValidateAssetPath(FolderPath, /*bRequireWritable=*/true)) // UE 5.8: 5.9 UToolsetLibrary addition
			{
				return {};
			}
			if (Factory && !RequireFactorySupports(SourceFile, Factory))
			{
				return {};
			}

			const FString ResolvedFolderPath = FToolsetLibrary59::ResolveAssetPath(FolderPath); // UE 5.8: 5.9 UToolsetLibrary addition
			if (bCheckDestination && !CheckImportDestination(FolderPath, AssetName, bReplaceExisting, ReplaceableClass))
			{
				return {};
			}

			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = SourceFile;
			Task->DestinationPath = ResolvedFolderPath;
			Task->DestinationName = AssetName;
			Task->bReplaceExisting = bReplaceExisting;
			Task->bAutomated = true;
			// Saved below through SaveImportedAssets instead, so both import paths save the same way.
			Task->bSave = false;
			if (Factory)
			{
				Task->Factory = Factory;
			}
			if (Options)
			{
				Task->Options = Options;
			}

			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
			AssetTools.ImportAssetTasks({Task});

			if (Task->ImportedObjectPaths.IsEmpty())
			{
				UKismetSystemLibrary::RaiseScriptError(FString::Printf(
					TEXT("Import of \"%s\" produced no assets. The file format may not be supported or the file may be invalid."),
					*SourceFile));
				return {};
			}

			TArray<UObject*> Result;
			Result.Reserve(Task->ImportedObjectPaths.Num());
			for (const FString& ImportedObjectPath : Task->ImportedObjectPaths)
			{
				Result.Add(StaticLoadObject(UObject::StaticClass(), nullptr, *ImportedObjectPath));
			}

			// A factory can hand back an asset whose derived data is still building on another thread
			// (e.g. a SoundWave's streamed audio cache). Finish it, as the Interchange path does, so both
			// paths return complete assets.
			FAssetCompilingManager::Get().FinishCompilationForObjects(Result);

			if (bSave)
			{
				const FString SaveError = SaveImportedAssets(Result);
				if (!SaveError.IsEmpty())
				{
					UKismetSystemLibrary::RaiseScriptError(SaveError);
					return {};
				}
			}
			return Result;
		}
	} // anonymous namespace

	UAsyncImportResult* ImportAsset(
		const FString& FolderPath, const FString& AssetName, const FString& SourceFile,
		UFactory* Factory, UObject* Options, bool bReplaceExisting, bool bSave, UClass* ReplaceableClass,
		bool bCheckDestination)
	{
		using namespace UE::AsyncFunctionResult;

		if (Factory)
		{
			UAsyncImportResult* Result = NewObject<UAsyncImportResult>();

			// No async entry point for the legacy factory-based import path; import
			// synchronously and complete the result immediately.
			FToolCallExceptionHandler ExceptionHandler;
			TArray<UObject*> Imported;
			ExceptionHandler.CaptureErrorsIn(
				[&FolderPath, &AssetName, &SourceFile, Factory, Options, bReplaceExisting, bSave, ReplaceableClass, bCheckDestination, &Imported]()
				{
					Imported = ImportAssetSynchronously(
						FolderPath, AssetName, SourceFile, Factory, Options, bReplaceExisting, bSave, ReplaceableClass, bCheckDestination);
				});
			const FString Exception = ExceptionHandler.GetException();
			if (Exception.IsEmpty())
			{
				Result->SetValue(Imported);
			}
			else
			{
				Result->SetError(Exception);
			}
			return Result;
		}

		bool bValid = true;
		FString ResolvedFolderPath;
		FToolCallExceptionHandler ValidationHandler;
		ValidationHandler.CaptureErrorsIn([&FolderPath, &AssetName, &SourceFile, bReplaceExisting, ReplaceableClass, bCheckDestination, &bValid, &ResolvedFolderPath]()
		{
			if (!IFileManager::Get().FileExists(*SourceFile))
			{
				UKismetSystemLibrary::RaiseScriptError(
					FString::Printf(TEXT("Source file not found: %s"), *SourceFile));
				bValid = false;
				return;
			}
			if (!FToolsetLibrary59::ValidateAssetPath(FolderPath, /*bRequireWritable=*/true)) // UE 5.8: 5.9 UToolsetLibrary addition
			{
				bValid = false;
				return;
			}

			ResolvedFolderPath = FToolsetLibrary59::ResolveAssetPath(FolderPath); // UE 5.8: 5.9 UToolsetLibrary addition
			bValid = !bCheckDestination || CheckImportDestination(FolderPath, AssetName, bReplaceExisting, ReplaceableClass);
		});
		if (!bValid)
		{
			return MakeErrorResult<UAsyncImportResult>(ValidationHandler.GetException());
		}

		UInterchangeSourceData* SourceData =
			UInterchangeManager::GetInterchangeManager().CreateSourceData(SourceFile);
		FImportAssetParameters Params;
		Params.bIsAutomated = true;
		Params.bReplaceExisting = bReplaceExisting;
		Params.DestinationName = AssetName;
		if (const UInterchangePipelineStackOverride* PipelineOverride =
				Cast<UInterchangePipelineStackOverride>(Options))
		{
			Params.OverridePipelines = PipelineOverride->OverridePipelines;
		}

		const UE::Interchange::FAssetImportResultRef ImportResult =
			UInterchangeManager::GetInterchangeManager().ImportAssetAsync(ResolvedFolderPath, SourceData, Params);

		UAsyncImportResult* Result = NewObject<UAsyncImportResult>();
		// Keeps Result alive for the duration of the async import: nothing else references it
		// once this function returns, until the caller's own handling of the returned pointer
		// (e.g. the tool-call dispatcher) takes over - which only happens after OnDone fires.
		const TStrongObjectPtr<UAsyncImportResult> StrongResult(Result);
		ImportResult->OnDone([StrongResult, bSave](UE::Interchange::FImportResult& Done)
		{
			const TArray<UObject*>& Imported = Done.GetImportedObjects();
			if (Imported.IsEmpty())
			{
				// With nothing imported, warnings (e.g. an existing asset Interchange declined to
				// overwrite) may be the only explanation.
				const FString Reasons = DescribeInterchangeProblems(Done, /*bIncludeWarnings=*/true);
				StrongResult->SetError(Reasons.IsEmpty()
					? FString(TEXT("Import produced no assets. The file format may not be supported or the file may be invalid."))
					: FString::Printf(TEXT("Import produced no assets: %s"), *Reasons));
				return;
			}
			// An error means part of the file failed to import even though other assets succeeded.
			const FString Errors = DescribeInterchangeProblems(Done, /*bIncludeWarnings=*/false);
			if (!Errors.IsEmpty())
			{
				StrongResult->SetError(FString::Printf(
					TEXT("Import failed for part of the file: %s %d other assets were imported but not saved."),
					*Errors, Imported.Num()));
				return;
			}

			// Interchange hands off assets before derived-data compilation (e.g. texture
			// mip/platform-data build) finishes; without this, callers can observe a Texture2D
			// with GetSizeX()/GetSizeY() still at zero.
			FAssetCompilingManager::Get().FinishCompilationForObjects(Imported);
			if (bSave)
			{
				const FString SaveError = SaveImportedAssets(Imported);
				if (!SaveError.IsEmpty())
				{
					StrongResult->SetError(SaveError);
					return;
				}
			}
			StrongResult->SetValue(Imported);
		});
		return Result;
	}

	namespace
	{
		// Local helper for AssetExists/AssetIsDirty.

		bool IsOnDisk(const FString& PackagePath)
		{
			IAssetRegistry& Registry = *IAssetRegistry::Get();
			TArray<FAssetData> Found;
			Registry.GetAssetsByPackageName(FName(*PackagePath), Found, /*bIncludeOnlyOnDiskAssets=*/true);
			return !Found.IsEmpty();
		}

		// Local helpers for LoadAssetForDiff/CanonicalProperties/ToCanonicalJsonString.

		// Accept either object paths (/Game/Foo.Foo) or package paths (/Game/Foo). For the
		// latter, synthesize the asset name from the package leaf so downstream APIs that
		// require a complete object path can find the entry.
		FSoftObjectPath NormalizeAssetPath(const FString& InPath)
		{
			FSoftObjectPath Path(InPath);
			if (Path.IsNull() || !Path.GetAssetFName().IsNone())
			{
				return Path;
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(InPath);
			if (AssetName.IsEmpty())
			{
				return FSoftObjectPath();
			}
			return FSoftObjectPath(InPath + TEXT(".") + AssetName);
		}

		FAssetData FindAssetData(IAssetRegistry& AssetRegistry, const FString& RequestedPackageName, const FString& PackageName, const FString& AssetName, bool bScanUnregisteredPackage)
		{
			const FSoftObjectPath ResolvedPath(FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName));
			FAssetData Asset = AssetRegistry.GetAssetByObjectPath(AssetRegistry.GetRedirectedObjectPath(ResolvedPath));
			if (!Asset.IsValid())
			{
				// The requested package may be an alias folder, which only the Content Browser item knows about.
				// An alias item is keyed by its own object path (AliasFolder/Name_Hash.Name_Hash), not by the asset it stands for.
				if (const UContentBrowserDataSubsystem* ContentBrowserData = IContentBrowserDataModule::Get().GetSubsystem())
				{
					const FString RequestedItemPath = FString::Printf(TEXT("%s.%s"), *RequestedPackageName, *AssetName);
					for (const FString& ItemPath : { RequestedItemPath, AssetScriptingUtils::ConvertToVirtualPath(ResolvedPath.ToString()) })
					{
						FSoftObjectPath AliasedPath;
						if (ContentBrowserData->GetItemAtPath(FName(*ItemPath), EContentBrowserItemTypeFilter::IncludeFiles).GetItemAliasedPath(AliasedPath))
						{
							Asset = AssetRegistry.GetAssetByObjectPath(AliasedPath);
							break;
						}
					}
				}
			}
			// The package exists on disk but the registry has not scanned it yet.
			FString PackageFilename;
			if (bScanUnregisteredPackage && !Asset.IsValid() && FPackageName::DoesPackageExist(PackageName, &PackageFilename))
			{
				AssetRegistry.ScanFilesSynchronous({ PackageFilename });
				Asset = AssetRegistry.GetAssetByObjectPath(AssetRegistry.GetRedirectedObjectPath(ResolvedPath));
			}
			return Asset;
		}

		/** Whether the creator can see the asset found at PackageName, or the folder PackageName names when no asset is there. */
		bool IsVisible(IAssetTools& AssetTools, const FAssetData& Asset, const FString& PackageName)
		{
			if (Asset.IsValid() && Asset.GetOptionalOuterPathName().IsNone())
			{
				return AssetTools.IsAssetVisible(Asset);
			}
			// A folder decides for a path with no asset, and for an external object (an OFPA actor, a data layer instance),
			// which is not an asset type and so fails the class allow list IsAssetVisible checks first when one is set (UEFN).
			const FString Folder = Asset.IsValid() ? Asset.PackagePath.ToString() : PackageName;
			return AssetTools.GetFolderPermissionList()->PassesStartsWithFilter(Folder);
		}

		// Rebuilds Value with every JSON object's keys sorted alphabetically, recursively.
		TSharedPtr<FJsonValue> SortKeysRecursive(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid())
			{
				return MakeShared<FJsonValueNull>();
			}
			if (Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Src = Value->AsObject();
				TArray<TPair<FString, TSharedPtr<FJsonValue>>> Pairs;
				Pairs.Reserve(Src->Values.Num());
				for (const auto& Pair : Src->Values)
				{
					Pairs.Emplace(FString(Pair.Key.ToView()), Pair.Value);
				}
				Pairs.Sort([](const TPair<FString, TSharedPtr<FJsonValue>>& A,
					const TPair<FString, TSharedPtr<FJsonValue>>& B)
				{
					return A.Key < B.Key;
				});
				const TSharedRef<FJsonObject> Dst = MakeShared<FJsonObject>();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Pairs)
				{
					Dst->SetField(Pair.Key, SortKeysRecursive(Pair.Value));
				}
				return MakeShared<FJsonValueObject>(Dst);
			}
			if (Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Dst;
				for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
				{
					Dst.Add(SortKeysRecursive(Element));
				}
				return MakeShared<FJsonValueArray>(Dst);
			}
			return Value;
		}
	}

	UEditorAssetSubsystem* GetEditorAssetSubsystem()
	{
		UEditorAssetSubsystem* Assets = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		check(Assets);
		return Assets;
	}

	FString ToPackagePath(const FString& Path)
	{
		int32 DotIndex = INDEX_NONE;
		return Path.FindChar(TEXT('.'), DotIndex) ? Path.Left(DotIndex) : Path;
	}

	bool AssetExists(const FString& Path)
	{
		UEditorAssetSubsystem* Assets = GetEditorAssetSubsystem();
		// A trailing slash leaves no asset name for DoesAssetExist to check, which makes it log an
		// engine error rather than just returning false.
		const bool bHasAssetName = !Path.EndsWith(TEXT("/"));
		return (Assets->DoesDirectoryExist(Path) || (bHasAssetName && Assets->DoesAssetExist(Path))) && IsPathVisible(Path);
	}

	bool IsPathVisible(const FString& Path)
	{
		FString NormalizedPath;
		if (!AssetScriptingUtils::TryConvertToInternalPath(Path, NormalizedPath))
		{
			return false;
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(NormalizedPath);
		if (FPackageName::IsScriptPackage(PackageName))
		{
			return true;
		}
		// Path may be a package path ("/Game/Foo"), an object path ("/Game/Foo.Foo") or a folder: GetAssetsByPackageName matches
		// the package in every case, so a Blueprint class path (Foo.Foo_C) finds its Blueprint without special handling.
		TArray<FAssetData> Assets;
		IAssetRegistry::GetChecked().GetAssetsByPackageName(FName(*PackageName), Assets);
		return IsVisible(IAssetTools::Get(), Assets.IsEmpty() ? FAssetData() : Assets[0], PackageName);
	}

	UObject* LoadAssetIfVisible(const FSoftObjectPath& AssetPath)
	{
		if (AssetPath.IsNull())
		{
			return nullptr;
		}
		const FString RequestedPackageName = AssetPath.GetLongPackageName();
		FString PackageName;
		if (!AssetScriptingUtils::TryConvertToInternalPath(RequestedPackageName, PackageName))
		{
			return nullptr;
		}
		// 1. Script: Always allowed
		if (FPackageName::IsScriptPackage(PackageName))
		{
			return AssetPath.TryLoad();
		}

		IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
		const FString AssetName = AssetPath.GetAssetName().IsEmpty() ? FPackageName::GetShortName(PackageName) : AssetPath.GetAssetName();

		// A Blueprint class (Foo.Foo_C) is judged through its Blueprint, the only one of the two the permission lists know.
		FAssetData Asset;
		FSoftObjectPath GeneratedClassPath;
		const bool bIsClassName = AssetName.EndsWith(TEXT("_C"), ESearchCase::CaseSensitive);
		auto ResolveBlueprint = [&AssetRegistry, &RequestedPackageName, &PackageName, &AssetName, &Asset, &GeneratedClassPath]()
		{
			const FAssetData Blueprint = FindAssetData(AssetRegistry, RequestedPackageName, PackageName, AssetName.LeftChop(2), /*bScanUnregisteredPackage=*/ false);
			const FSoftObjectPath TaggedClassPath(FPackageName::ExportTextPathToObjectPath(Blueprint.GetTagValueRef<FString>(FBlueprintTags::GeneratedClassPath)));
			if (Blueprint.IsValid() && !TaggedClassPath.IsNull())
			{
				Asset = Blueprint;
				GeneratedClassPath = TaggedClassPath;
			}
		};
		if (bIsClassName)
		{
			ResolveBlueprint();
		}
		if (!Asset.IsValid())
		{
			Asset = FindAssetData(AssetRegistry, RequestedPackageName, PackageName, AssetName, /*bScanUnregisteredPackage=*/ true);
			// That scan registers a Blueprint package the registry had not seen, which the first probe could not find.
			if (!Asset.IsValid() && bIsClassName)
			{
				ResolveBlueprint();
			}
		}
		// 2. Missing or hidden: Refused, both the same way
		if (!Asset.IsValid() || !IsVisible(IAssetTools::Get(), Asset, PackageName))
		{
			return nullptr;
		}
		// 3. Visible Blueprint class: Loaded from the Blueprint that passed
		if (!GeneratedClassPath.IsNull())
		{
			return GeneratedClassPath.TryLoad();
		}
		// 4. Visible Asset
		UObject* Loaded = Asset.GetAsset();
		if (!Loaded || AssetPath.GetSubPathUtf8String().IsEmpty())
		{
			return Loaded;
		}
		// 5. Visible sub-object inside the asset
		return FSoftObjectPath(FSoftObjectPath(Loaded).GetAssetPath(), AssetPath.GetSubPathUtf8String()).ResolveObject();
	}

	bool AssetIsDirty(const FString& AssetPath)
	{
		if (!AssetExists(AssetPath))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Asset does not exist: %s"), *AssetPath));
			return false;
		}
		const FString PackagePath = ToPackagePath(AssetPath);
		UPackage* Package = FindPackage(nullptr, *PackagePath);
		const bool bInMemoryDirty = Package && Package->IsDirty(); // UE 5.8: no UEditorLoadingAndSavingUtils::IsPackageDirty
		return bInMemoryDirty || !IsOnDisk(PackagePath);
	}

	UObject* LoadAssetForDiff(const FString& AssetPath, UClass* ExpectedClass)
	{
		const FSoftObjectPath ObjectPath = NormalizeAssetPath(AssetPath);
		UObject* Asset = ObjectPath.IsValid() ? ObjectPath.TryLoad() : nullptr;
		if (!Asset)
		{
			UKismetSystemLibrary::RaiseScriptError(
				FString::Printf(TEXT("Could not load asset: %s"), *AssetPath));
			return nullptr;
		}
		if (ExpectedClass && !Asset->IsA(ExpectedClass))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("Not a %s: %s (found %s)"),
				*ExpectedClass->GetName(), *AssetPath, *Asset->GetClass()->GetName()));
			return nullptr;
		}
		return Asset;
	}

	TSharedRef<FJsonObject> CanonicalProperties(UObject* Object)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		const FString SchemaJson = UToolsetLibrary::ListStructProperties(Object->GetClass());
		if (SchemaJson.IsEmpty())
		{
			return Result;
		}
		TSharedPtr<FJsonObject> Schema;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(SchemaJson);
		if (!FJsonSerializer::Deserialize(Reader, Schema) || !Schema.IsValid())
		{
			return Result;
		}

		// Subobject paths and asset-name prefixes are normalized so two objects with the same
		// shape produce the same canonical value, mirroring canonical_properties in
		// toolset_registry.helpers.
		const FString SelfPath = Object->GetPathName();
		const FString SelfNamePrefix = FString::Printf(TEXT("<self>:%s"), *Object->GetName());

		for (const auto& Pair : Schema->Values)
		{
			const FString PropertyName(Pair.Key.ToView());

			// PropertyAccessUtil can reject the read (e.g. editor-only properties whose getters
			// return PermissionDenied). Skip those - both sides of a diff hit the same
			// rejections, so omitting the property doesn't mask changes.
			FToolCallExceptionHandler ExceptionHandler;
			FString Value;
			ExceptionHandler.CaptureErrorsIn([&]()
			{
				Value = UToolsetLibrary::GetObjectProperties(Object, {FName(*PropertyName)});
			});
			if (!ExceptionHandler.GetException().IsEmpty())
			{
				continue;
			}

			Value = Value.Replace(*SelfPath, TEXT("<self>"));
			Value = Value.Replace(*SelfNamePrefix, TEXT("<self>:"));
			Result->SetStringField(PropertyName, Value);
		}
		return Result;
	}

	FString ToCanonicalJsonString(const TSharedPtr<FJsonValue>& Value)
	{
		const TSharedPtr<FJsonValue> Sorted = SortKeysRecursive(Value);
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Sorted, FString(), Writer);
		return Out;
	}

	FString DiffCanonicalJson(const TSharedPtr<FJsonValue>& Old, const TSharedPtr<FJsonValue>& New)
	{
		const FString OldText = ToCanonicalJsonString(Old);
		const FString NewText = ToCanonicalJsonString(New);
		if (OldText == NewText)
		{
			return FString();
		}
		return UKismetStringLibrary::DiffString(OldText, NewText);
	}

	namespace
	{
		void GatherSubobjectHandles(AActor* Actor, TArray<FSubobjectDataHandle>& OutHandles)
		{
			OutHandles.Reset();
			if (USubobjectDataSubsystem* Subsystem = USubobjectDataSubsystem::Get())
			{
				Subsystem->GatherSubobjectData(Actor, OutHandles);
			}
		}

		// Every component. A Blueprint CDO goes through the subobject data so Simple Construction Script templates are included.
		TArray<UActorComponent*> EnumerateAllComponents(AActor* Actor)
		{
			UBlueprint* Blueprint = UBlueprintEditorLibrary::GetBlueprintAsset(Actor->GetOuter());
			if (!Blueprint)
			{
				return Actor->K2_GetComponentsByClass(UActorComponent::StaticClass());
			}
			TArray<FSubobjectDataHandle> Handles;
			GatherSubobjectHandles(Actor, Handles);
			TArray<UActorComponent*> Result;
			TSet<UActorComponent*> Seen;
			for (const FSubobjectDataHandle& Handle : Handles)
			{
				const FSubobjectData* Data = Handle.GetData();
				if (!Data || Data->IsActor())
				{
					continue;
				}
				UActorComponent* Obj = const_cast<UActorComponent*>(
					Cast<UActorComponent>(USubobjectDataBlueprintFunctionLibrary::GetAssociatedObject(*Data)));
				if (Obj && !Seen.Contains(Obj))
				{
					Seen.Add(Obj);
					Result.Add(Obj);
				}
			}
			return Result;
		}

		// Matches the trailing `_<digits>` on a reference to an actor-internal sub-object (e.g.
		// `<self>.MID_Foo_0`, `<actor>.SceneComponent_5`). Unreal appends `_N` to deduplicate names
		// when sub-objects are recreated (dynamic material instances, regenerated components, etc.),
		// so two semantically identical actors can produce differently-numbered references.
		FString StripAutoNameSuffix(const FString& Input)
		{
			static const FRegexPattern Pattern(TEXT("(<(?:self|actor)>[.:][A-Za-z_]\\w*)_\\d+(?=\\W|$)"));
			FRegexMatcher Matcher(Pattern, Input);
			FString Result;
			int32 LastEnd = 0;
			while (Matcher.FindNext())
			{
				const int32 MatchBegin = Matcher.GetMatchBeginning();
				const int32 MatchEnd = Matcher.GetMatchEnding();
				Result += Input.Mid(LastEnd, MatchBegin - LastEnd);
				Result += Matcher.GetCaptureGroup(1);
				LastEnd = MatchEnd;
			}
			Result += Input.Mid(LastEnd);
			return Result;
		}

		// Random GUIDs assigned per spawn - meaningful for engine bookkeeping but noise in a diff,
		// since two semantically identical actor instances will never share them.
		bool IsNondeterministicActorProperty(const FString& Name)
		{
			return Name == TEXT("actorGuid") || Name == TEXT("actorInstanceGuid");
		}
	}

	TSharedRef<FJsonObject> CanonicalActorStateJson(AActor* Actor)
	{
		const FString ActorPath = Actor->GetPathName();
		const FString ActorSelfPrefix = FString::Printf(TEXT("<self>:%s"), *Actor->GetName());

		UClass* ActorClass = Actor->GetClass();
		const bool bIsCdo = (Actor == ActorClass->GetDefaultObject());
		UBlueprint* Blueprint = bIsCdo ? UBlueprintEditorLibrary::GetBlueprintAsset(Actor->GetOuter()) : nullptr;

		FString ClassPath;
		TOptional<FString> GenClassPath;
		TOptional<FString> GenClassSelfPrefix;
		if (Blueprint)
		{
			// BP-generated CDO: report the parent native class and normalize the per-Blueprint
			// generated class path so structurally identical BPs match.
			UClass* ParentClass = UBlueprintEditorLibrary::GetBlueprintParentClass(Blueprint);
			ClassPath = ParentClass ? ParentClass->GetPathName() : ActorClass->GetPathName();
			GenClassPath = ActorClass->GetPathName();
			GenClassSelfPrefix = FString::Printf(TEXT("<self>:%s"), *ActorClass->GetName());
		}
		else
		{
			ClassPath = ActorClass->GetPathName();
		}

		auto Normalize = [&ActorPath, &ActorSelfPrefix, &GenClassPath, &GenClassSelfPrefix](FString Value) -> FString
		{
			Value = Value.Replace(*ActorPath, TEXT("<actor>")).Replace(*ActorSelfPrefix, TEXT("<actor>:"));
			if (GenClassPath.IsSet())
			{
				Value = Value.Replace(**GenClassPath, TEXT("<class>")).Replace(**GenClassSelfPrefix, TEXT("<class>:"));
			}
			return StripAutoNameSuffix(Value);
		};

		// Editor-only components (e.g. billboard icons auto-added for empty actors or lights) are
		// visualization helpers, not saved state. Drop them so they don't show up as spurious diffs.
		TArray<UActorComponent*> AllComponents = EnumerateAllComponents(Actor);
		AllComponents.Sort([](const UActorComponent& A, const UActorComponent& B)
		{
			return A.GetName() < B.GetName();
		});

		const TSharedRef<FJsonObject> Components = MakeShared<FJsonObject>();
		for (UActorComponent* Comp : AllComponents)
		{
			if (!Comp || Comp->IsEditorOnly())
			{
				continue;
			}
			const TSharedRef<FJsonObject> Props = UE::ToolsetRegistry::CanonicalProperties(Comp);
			const TSharedRef<FJsonObject> NormalizedProps = MakeShared<FJsonObject>();
			for (const auto& Pair : Props->Values)
			{
				NormalizedProps->SetStringField(Pair.Key, Normalize(Pair.Value->AsString()));
			}
			const TSharedRef<FJsonObject> CompEntry = MakeShared<FJsonObject>();
			CompEntry->SetStringField(TEXT("class"), Comp->GetClass()->GetPathName());
			CompEntry->SetField(TEXT("properties"), MakeShared<FJsonValueObject>(NormalizedProps));
			Components->SetField(Comp->GetName(), MakeShared<FJsonValueObject>(CompEntry));
		}

		const TSharedRef<FJsonObject> ActorProps = UE::ToolsetRegistry::CanonicalProperties(Actor);
		const TSharedRef<FJsonObject> NormalizedActorProps = MakeShared<FJsonObject>();
		for (const auto& Pair : ActorProps->Values)
		{
			if (IsNondeterministicActorProperty(FString(Pair.Key.ToView())))
			{
				continue;
			}
			NormalizedActorProps->SetStringField(Pair.Key, Normalize(Pair.Value->AsString()));
		}

		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("class"), ClassPath);
		Result->SetField(TEXT("properties"), MakeShared<FJsonValueObject>(NormalizedActorProps));
		Result->SetField(TEXT("components"), MakeShared<FJsonValueObject>(Components));
		return Result;
	}
}
