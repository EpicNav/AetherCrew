// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class FProperty;
class UScriptStruct;

/**
 * Whether a name or path an agent sent can become an FName, an object lookup or a new package without stopping the
 * editor. Unreal asserts on an FName of 1024 characters or more, and FindObject, LoadObject, FSoftObjectPath,
 * FTopLevelAssetPath and the asset registry each make one from the path or from each part of it. CreatePackage stops
 * on a double slash, and so does LoadObject when nothing is there to load, since it makes the package it looked for.
 * FTopLevelAssetPath ensures on a path that does not start with '/'. Tools check request fields with these before any
 * of that is built and refuse with invalid_request, handing OutReason to the agent.
 */
struct HYPERAISTUDIO_API FHyperAIStudioNames
{
	/**
	 * Longest name or path accepted. A path, a '.', its short name again (the main asset of a package path) and "_C" (a
	 * Blueprint's class) still fit in an FName's 1023 characters.
	 */
	static constexpr int32 MaxLength = 500;

	/** A name: a tag, a property, a bone, an Outliner folder, an object's own name. At most MaxLength characters; empty passes. */
	static bool IsName(FStringView Text, const TCHAR* Field, FString* OutReason = nullptr);

	/**
	 * An object or package to look up: /Game/A/B, /Game/A/B.B, /Game/Map.Map:PersistentLevel.Actor or
	 * /Script/Engine.StaticMeshActor. Its package part is a well-formed long package name (a root and a name, no "//",
	 * no trailing '/', no spaces or other characters packages cannot hold), there are no quotes, and it is at most
	 * MaxLength characters. Whether anything is there is the caller's question.
	 */
	static bool IsObjectPath(FStringView Path, const TCHAR* Field, FString* OutReason = nullptr);

	/** A package to create: an object path with no '.' or ':', under a mounted root that can be saved to (/Game/A/B). */
	static bool IsPackagePath(FStringView Path, const TCHAR* Field, FString* OutReason = nullptr);

	/** A content folder new packages go in: /Game or /Game/A/B, without a trailing slash, under a root that can be saved to. */
	static bool IsFolderPath(FStringView Path, const TCHAR* Field, FString* OutReason = nullptr);

	/**
	 * Nothing at the package path, on disk or in memory. An asset made, duplicated or renamed there and not saved yet
	 * lives only in memory, where DoesPackageExist never looks: an asset made over it replaces it in place, or, when it
	 * is of another class, stops the editor (NewObject's "Cannot replace existing object of a different class").
	 */
	static bool IsFreePackage(FStringView PackagePath);

	/**
	 * Every string of a request, through its structs, arrays, sets and maps, is a name (IsName) unless its field is free
	 * text: one named in FreeText, or Tooltip, Description, Comment, Notes, Note, Code, Script, Python, Prompt, Message,
	 * Markdown, Json or Text. OutReason names the field by its path, such as Operations[2].Name. A tool runs it first, so
	 * no field it looks up, or makes a name of, can be past an FName's length.
	 */
	static bool CheckRequest(const UScriptStruct& Struct, const void* Request, TConstArrayView<const TCHAR*> FreeText, FString* OutReason);

	template <typename TRequest>
	static bool CheckRequest(const TRequest& Request, FString* OutReason, TConstArrayView<const TCHAR*> FreeText = {})
	{
		return CheckRequest(*TRequest::StaticStruct(), &Request, FreeText, OutReason);
	}

	/**
	 * CheckRequest at a tool's front door: true when the request is refused, with Report's Status set to invalid_request,
	 * bOk to false and Diagnostic to the reason, for whichever of those fields it has.
	 */
	template <typename TRequest, typename TReport>
	static bool RefuseRequest(const TRequest& Request, TReport& Report, TConstArrayView<const TCHAR*> FreeText = {})
	{
		FString Reason;
		if (CheckRequest(Request, &Reason, FreeText)) return false;
		if constexpr (requires { Report.Status = FString(); }) Report.Status = TEXT("invalid_request");
		if constexpr (requires { Report.bOk = false; }) Report.bOk = false;
		if constexpr (requires { Report.Diagnostic = FString(); }) Report.Diagnostic = Reason;
		return true;
	}

	/**
	 * Whether Text can go into Property through Unreal's text import, which makes an FName of a name and looks an object
	 * up by its path (loading it, and making its package when nothing is there). A name property takes a name; an
	 * object, class or soft reference takes a path (Class'/Game/A.A' too), a relative name or None; any other property
	 * takes text whose unquoted parts are names and hold no double slash. Strings and texts take anything.
	 */
	static bool CanImportText(const FProperty& Property, FStringView Text, const TCHAR* Field, FString* OutReason = nullptr);
};
