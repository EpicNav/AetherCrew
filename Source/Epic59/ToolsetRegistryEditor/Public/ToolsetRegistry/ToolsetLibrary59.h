// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: the functions UE 5.9 added to UToolsetLibrary (ToolsetRegistry/ToolsetLibrary.h). 5.8's
// UToolsetLibrary is an engine class and cannot gain members, so they live here as plain C++ statics
// (not exposed to Blueprint or Python). Tools/Epic59/port_toolset.py rewrites
// UToolsetLibrary::<added function> to FToolsetLibrary59::<added function>, and the 4-argument
// ListStructProperties to FToolsetLibrary59::ListStructProperties. See Source/Epic59/PORTING.md for
// where these differ from 5.9.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"

#define UE_API TOOLSETREGISTRYEDITOR_API

struct FToolsetLibrary59
{
	/*
	 * 5.9's ListStructProperties with RootPath and MaxDepth. With neither given it is 5.8's
	 * UToolsetLibrary::ListStructProperties. Otherwise it walks 5.8's schema to RootPath (struct members
	 * and container element types; a subscript is refused, as in 5.9) and cuts it MaxDepth levels below.
	 */
	static UE_API FString ListStructProperties(
		const UStruct* Struct, bool bUserVisiblePropertiesOnly = true,
		const FString& RootPath = TEXT(""), int32 MaxDepth = 0);

	/*
	 * Returns the property names of an object as dotted paths relative to RootPath, parents before
	 * children. A struct cut by MaxDepth ends in " {...}". Container brackets: an index range for arrays,
	 * the keys for maps, a bare count for sets, empty brackets for an empty container, "[?]" when no
	 * instance reaches the position. RootPath may subscript arrays ("Arr[2]") and maps ("Map[Key]").
	 * NameFilter is a case-insensitive regular expression searched in a property's name and Keywords
	 * metadata. MaxDepth zero or less expands every level.
	 */
	static UE_API TArray<FString> ListObjectPropertyNames(
		const UObject* Object, bool bUserVisiblePropertiesOnly = true,
		const FString& RootPath = TEXT(""), const FString& NameFilter = TEXT(""), int32 MaxDepth = 0);

	/* Returns a struct as JSON, in the form a tool takes for a parameter of its type. */
	static UE_API FString StructToJson(const FInstancedStruct& Value);

	/* Whether Class passes the global Class Viewer filter (permissive when none is registered). */
	static UE_API bool IsClassAllowed(const UClass* Class);

	/* The root content paths of enabled, mounted plugins with content, e.g. ["/PluginName/"]. */
	static UE_API TArray<FString> GetPluginContentPaths(bool bIncludeEngine = false);

	/* The project's own asset root plus every content plugin's mount, e.g. ["/Game/", "/MyPlugin/"]. */
	static UE_API TArray<FString> GetAssetContentRoots(bool bRequireVisible = false);

	/* Whether FolderPath is a legitimate content location; raises a script error listing the roots if not. */
	static UE_API bool ValidateAssetPath(const FString& FolderPath, bool bRequireWritable = true);

	/* ValidateAssetPath without publishing a script error. */
	static UE_API bool TryValidateAssetPath(const FString& FolderPath, bool bRequireWritable, FString& OutError);

	/* Whether Path passes the asset- and folder-visibility permission lists. */
	static UE_API bool IsPathVisible(const FString& Path);

	/* Whether Path passes the writable-folder filter. */
	static UE_API bool IsPathWritable(const FString& Path);

	/* Path with ".." collapsed and a Content Browser virtual path translated to its internal path. */
	static UE_API FString ResolveAssetPath(const FString& Path);

	/* The Content Browser virtual path of an internal content path (for presentation only). */
	static UE_API FString ConvertToVirtualPath(const FString& Path);
};

#undef UE_API
