// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: the path functions of UE 5.9's AssetTools/Public/AssetScriptingUtils.h, which 5.8 does not have.
// Copied from 5.9 into the compat ToolsetRegistryEditor module (exported from it, not from AssetTools).
// 5.9's access checks (GetAssetAccess, GetNewAssetAccess, ResolveNewAssetPath, LoadAccessibleObject,
// GetOwningAsset, PassesAssetReferenceFilter) are not here; Tools/Epic59/port_toolset.py flags their use.

#pragma once

#include "Containers/UnrealString.h"
#include "HAL/Platform.h"

class UObject;

namespace AssetScriptingUtils
{
	/**
	 * Converts a path a user gave, which may be a content browser virtual path, to an internal path. Collapses ".." and
	 * duplicate slashes first, so that they cannot hide a mount point from the conversion or a permission list.
	 * @return False if a ".." cannot be collapsed; OutInternalPath is then the path as far as it was normalized.
	 */
	TOOLSETREGISTRYEDITOR_API bool TryConvertToInternalPath(const FString& Path, FString& OutInternalPath);

	/** The content browser virtual path of an internal package or object path, or the path itself when it has none. */
	TOOLSETREGISTRYEDITOR_API FString ConvertToVirtualPath(const FString& InternalPath);

	/** The content browser virtual path of the package an asset is in, e.g. for a message to the user. */
	TOOLSETREGISTRYEDITOR_API FString ConvertToVirtualPath(const UObject& Asset);

	/**
	 * Text (e.g. an exported property value) with the paths of its existing packages converted to virtual paths. Class and
	 * transient object paths are left as they are.
	 */
	TOOLSETREGISTRYEDITOR_API FString ConvertToVirtualPathsInText(const FString& Text);

	/** Text (e.g. a property value to import) with its virtual paths converted to internal paths, which importing understands. */
	TOOLSETREGISTRYEDITOR_API FString ConvertToInternalPathsInText(const FString& Text);
}
