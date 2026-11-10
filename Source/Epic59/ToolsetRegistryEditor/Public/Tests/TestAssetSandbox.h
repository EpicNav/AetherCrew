// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/* RAII base for tests needing a writable, mounted content folder for fixture assets they create.
 * Fixtures live in /Automation/FolderName/, on the project's automation-transient dir, so a
 * toolset's ValidateAssetPath/CreateAsset checks resolve to a writable location. The folder is
 * deleted (via UEditorAssetSubsystem::DeleteDirectory, which re-resolves the directory's contents at
 * call time rather than holding a long-lived object list) on destruction. Nothing is saved unless a
 * test calls SaveDirtyPackages.
 *
 * /Automation/ is mounted on first use and never unmounted, since other tests share it. Content
 * under it belongs to the asset referencing AutomationDomain, which can reference and be referenced
 * by any other content, so fixtures pass asset pickers such as the Blueprint parent class picker.
 *
 * Subclasses add their own domain-specific creation/import helpers; every fixture asset they
 * create must live under Folder() so this base's directory delete on destruction actually reaches
 * it. This base only owns the mount/cleanup lifecycle.
 */
class TOOLSETREGISTRYEDITOR_API FTestAssetSandbox // UE 5.8: 5.9 UnrealEd class, carried by the compat layer
{
public:
	explicit FTestAssetSandbox(const TCHAR* FolderName);
	virtual ~FTestAssetSandbox();

	FTestAssetSandbox(const FTestAssetSandbox&) = delete;
	FTestAssetSandbox& operator=(const FTestAssetSandbox&) = delete;

	/** Folder, under /Automation/, that fixture assets live in. */
	FString Folder() const;

	/**
	 * Saves the dirty packages under Folder(), including external actor and folder packages,
	 * and no others. For fixtures that must exist on disk, e.g. a level another level instances.
	 * Validate-on-save is disabled for the duration, since no asset reference domain covers the mount.
	 */
	bool SaveDirtyPackages() const;

	/** A unique asset name so each fixture is isolated from the others. */
	static FString UniqueName(const TCHAR* Prefix);

private:
	/** Folder() with a trailing slash. */
	FString SandboxFolderPath;
};
