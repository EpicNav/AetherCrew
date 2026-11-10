// Copyright Epic Games, Inc. All Rights Reserved.

#include "Tests/TestAssetSandbox.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Package.h"

namespace UE::TestAssetSandbox::Private
{
	// A single-segment root: a sandbox mounting its own root matches no asset referencing domain, and
	// a nested root such as /Automation/Name/ makes the asset registry drop the assets it discovers.
	constexpr const TCHAR* AutomationRoot = TEXT("/Automation/");

	// FolderPath, plus the folders world partition puts external actor and object packages in for
	// levels inside it. Those sit under the mount root (/Automation/__ExternalActors__/Name/...),
	// not under FolderPath. They come first because deleting an unloaded actor loads it, which
	// fails once its level has been deleted.
	TArray<FString> GetOwnedPaths(const FString& FolderPath)
	{
		const FString RelativeFolder = FolderPath.RightChop(FCString::Strlen(AutomationRoot));
		return {
			FString::Printf(TEXT("%s%s/%s"), AutomationRoot, FPackagePath::GetExternalActorsFolderName(), *RelativeFolder),
			FString::Printf(TEXT("%s%s/%s"), AutomationRoot, FPackagePath::GetExternalObjectsFolderName(), *RelativeFolder),
			FolderPath,
		};
	}

	// What UEditorAssetSubsystem::DeleteDirectory does, minus removing the folder from the asset
	// registry: for external package folders that fails, and logs a warning, even once every package
	// in them has been deleted.
	// UE 5.9: only directories under Saved/Automation are ever deleted.
	bool IsUnderAutomationTransientDir(const FString& Directory)
	{
		FString Root = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir());
		FString Full = FPaths::ConvertRelativePathToFull(Directory);
		FPaths::NormalizeDirectoryName(Root);
		FPaths::NormalizeDirectoryName(Full);
		return Full.StartsWith(Root + TEXT("/"));
	}

	void DeleteExternalPackages(const FString& FolderPath)
	{
		TArray<FAssetData> Assets;
		IAssetRegistry::GetChecked().GetAssetsByPath(FName(FolderPath.LeftChop(1)), Assets, /*bRecursive=*/true);
		TArray<UObject*> Objects;
		for (const FAssetData& Asset : Assets)
		{
			if (UObject* Object = Asset.GetAsset())
			{
				Objects.Add(Object);
			}
		}
		if (!Objects.IsEmpty())
		{
			ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
		}

		FString FolderDir;
		if (FPackageName::TryConvertLongPackageNameToFilename(FolderPath, FolderDir) && IsUnderAutomationTransientDir(FolderDir))
		{
			IFileManager::Get().DeleteDirectory(*FolderDir, /*RequireExists=*/false, /*Tree=*/true);
		}
	}

	// UE 5.9: the sandbox folder is one plain path segment, so the destructor can only delete
	// what is inside its own folder. Empty (and the sandbox inert) for an empty name, ".", ".." or a name with a separator.
	FString MakeSandboxFolderPath(const TCHAR* FolderName)
	{
		const FString Name(FolderName);
		if (!ensureMsgf(!Name.IsEmpty() && Name != TEXT(".") && Name != TEXT("..") && !Name.Contains(TEXT("/")) && !Name.Contains(TEXT("\\")),
			TEXT("FTestAssetSandbox needs a folder name of one path segment, not '%s'"), *Name))
		{
			return FString();
		}
		return FString::Printf(TEXT("%s%s/"), AutomationRoot, *Name);
	}
}

FTestAssetSandbox::FTestAssetSandbox(const TCHAR* FolderName)
	: SandboxFolderPath(UE::TestAssetSandbox::Private::MakeSandboxFolderPath(FolderName))
{
	using namespace UE::TestAssetSandbox::Private;

	if (SandboxFolderPath.IsEmpty())
	{
		return; // UE 5.9: an invalid name (MakeSandboxFolderPath)
	}

	// Never unmounted: unmounting with packages still live under the root makes later name
	// resolution fail for the rest of the run, and other tests mount and use it too.
	if (!FPackageName::MountPointExists(AutomationRoot))
	{
		FPackageName::RegisterMountPoint(AutomationRoot, FPaths::AutomationTransientDir());
	}

	FString FolderDir;
	if (FPackageName::TryConvertLongPackageNameToFilename(SandboxFolderPath, FolderDir))
	{
		IFileManager::Get().MakeDirectory(*FolderDir, /*Tree=*/true);
	}
}

FTestAssetSandbox::~FTestAssetSandbox()
{
	using namespace UE::TestAssetSandbox::Private;

	if (SandboxFolderPath.IsEmpty())
	{
		return; // UE 5.9: an invalid name owns nothing
	}

	// Re-resolves each folder's contents at call time (via IAssetRegistry::GetAssetsByPath
	// + LoadAssetFromData for each result, including in-memory/unsaved assets) rather than
	// deleting a long-held raw object list that GC could have invalidated out from under us.
	if (UEditorAssetSubsystem* Eas = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
	{
		// Keeps ForceDeleteObjects from prompting, as DeleteDirectory does.
		TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
		for (const FString& Path : GetOwnedPaths(SandboxFolderPath))
		{
			if (Path == SandboxFolderPath)
			{
				Eas->DeleteDirectory(Path);
			}
			else
			{
				DeleteExternalPackages(Path);
			}
		}
	}
	// Collect the deleted packages now rather than leaving them loaded for whichever test runs next.
	GEngine->ForceGarbageCollection(true);
}

FString FTestAssetSandbox::Folder() const
{
	return SandboxFolderPath.LeftChop(1);
}

bool FTestAssetSandbox::SaveDirtyPackages() const
{
	const TArray<FString> OwnedPaths = UE::TestAssetSandbox::Private::GetOwnedPaths(SandboxFolderPath);
	TArray<UPackage*> Dirty;
	FEditorFileUtils::GetDirtyWorldPackages(Dirty);
	FEditorFileUtils::GetDirtyContentPackages(Dirty);
	Dirty.RemoveAll([&OwnedPaths](const UPackage* Package)
	{
		const FString PackageName = Package->GetName();
		return !OwnedPaths.ContainsByPredicate([&PackageName](const FString& Path) { return PackageName.StartsWith(Path); });
	});
	return UEditorLoadingAndSavingUtils::SavePackages(Dirty, /*bOnlyDirty=*/true);
}

FString FTestAssetSandbox::UniqueName(const TCHAR* Prefix)
{
	return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}
