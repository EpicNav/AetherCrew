// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResult.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/Function.h"
#include "UObject/Object.h"

#include "AssetHelpers.generated.h"

class AActor;
class UEditorAssetSubsystem;
class UFactory;
enum class EInterchangeTranslatorAssetType : uint8;
struct FSoftObjectPath;

#define UE_API TOOLSETREGISTRYEDITOR_API

// Async tool call result that completes with the assets an import produced.
UCLASS(BlueprintType)
class UAsyncImportResult : public UAsyncFunctionResult
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Toolset Library")
	bool SetValue(const TArray<UObject*>& InValue)
	{
		TArray<TObjectPtr<UObject>> Converted;
		Converted.Reserve(InValue.Num());
		for (UObject* Object : InValue)
		{
			Converted.Add(Object);
		}
		return MaybeBroadcastSuccessfulCompletion(MoveTemp(Converted), Value);
	}

	UPROPERTY(BlueprintReadOnly, Category = "Toolset Library")
	TArray<TObjectPtr<UObject>> Value;
};

namespace UE::ToolsetRegistry
{
	/*
	 * Creates a new asset of AssetClass at FolderPath/AssetName using Factory.
	 *
	 * Raises if unable to create an asset.
	 *
	 * @param FolderPath The content-browser folder to create the asset in.
	 * @param AssetName The name of the new asset.
	 * @param AssetClass The class the created asset must be.
	 * @param Factory The factory to create the asset with.
	 * @return The created asset, or null if creation failed.
	 */
	UE_API UObject* CreateAsset(
		const FString& FolderPath, const FString& AssetName, UClass* AssetClass, UFactory* Factory);

	/*
	 * Imports a file from disk as one or more assets at FolderPath/AssetName.
	 *
	 * Genuinely async via Interchange when Factory is null; pinning a Factory (e.g. when the
	 * extension is ambiguous or factory-specific options must be honored) blocks until the
	 * import finishes before the returned result completes, since the legacy factory-based
	 * import path has no async entry point.
	 *
	 * @param FolderPath The content-browser folder to import into.
	 * @param AssetName The name to give the imported asset. When the import produces multiple
	 *        assets, additional names are derived from this base name.
	 * @param SourceFile The absolute path to the file on disk to import.
	 * @param Factory Optional factory to drive the import. Leave null to let the engine pick
	 *        (the Interchange path).
	 * @param Options Optional factory-specific options object (e.g. UFbxImportUI). When Factory
	 *        is null and this is a UInterchangePipelineStackOverride, its pipelines are applied
	 *        to the import too.
	 * @param bReplaceExisting Overwrite an existing asset at the destination instead of failing.
	 * @param bSave Save the imported assets through the normal editor save path, so save validators run.
	 * @param ReplaceableClass If set, only an existing asset of this class may be replaced.
	 * @param bCheckDestination Check FolderPath/AssetName for a conflicting folder or asset before
	 *        importing. Pass false when the importer chooses where assets land (e.g. an
	 *        Interchange pipeline that places them in per-file or per-type subfolders),
	 *        leaving conflicts to the importer as an editor import does.
	 * @return A pending result that completes with every asset the import produced, or an error
	 *         if the importer reported any error, even when it produced some assets. Warnings are ignored.
	 */
	UE_API UAsyncImportResult* ImportAsset(
		const FString& FolderPath, const FString& AssetName, const FString& SourceFile,
		UFactory* Factory = nullptr, UObject* Options = nullptr,
		bool bReplaceExisting = false, bool bSave = false, UClass* ReplaceableClass = nullptr,
		bool bCheckDestination = true);

	/*
	 * Returns whether SourceFile's extension is one Factory declares in its Formats list.
	 *
	 * Raises if not supported.
	 *
	 * @param SourceFile The absolute path to the file on disk to check.
	 * @param Factory The factory whose Formats list is checked against.
	 * @return True if supported.
	 */
	UE_API bool RequireFactorySupports(const FString& SourceFile, const UFactory* Factory);

	/*
	 * Returns whether Interchange can import SourceFile's extension as AssetType.
	 *
	 * Raises if not supported.
	 *
	 * @param SourceFile The absolute path to the file on disk to check.
	 * @param AssetType The kind of asset the file must be able to produce.
	 * @param AssetTypeName How the error names AssetType, e.g. "meshes".
	 * @return True if supported.
	 */
	UE_API bool RequireInterchangeSupports(
		const FString& SourceFile, EInterchangeTranslatorAssetType AssetType, const TCHAR* AssetTypeName);

	/*
	 * Returns whether an import may write FolderPath/AssetName. A folder there always blocks it;
	 * an asset there blocks it unless bReplaceExisting is set and the asset is a ReplaceableClass
	 * (when given).
	 *
	 * Raises if blocked.
	 *
	 * @param FolderPath The content-browser folder the import writes to.
	 * @param AssetName The name of the asset the import writes.
	 * @param bReplaceExisting Whether the import may overwrite an existing asset.
	 * @param ReplaceableClass If set, only an existing asset of this class may be replaced.
	 * @return True if the import may proceed.
	 */
	UE_API bool CheckImportDestination(
		const FString& FolderPath, const FString& AssetName, bool bReplaceExisting, const UClass* ReplaceableClass);

	/*
	 * Saves imported assets through the editor's normal save path, as AssetToolset.SaveAsset does,
	 * so save-time validators see them.
	 *
	 * @param Imported The assets to save.
	 * @return An error naming the assets that failed to save, or empty when all saved.
	 */
	UE_API FString SaveImportedAssets(const TArray<UObject*>& Imported);

	/** Returns the editor's asset subsystem. */
	UE_API UEditorAssetSubsystem* GetEditorAssetSubsystem();

	/**
	 * Normalizes a full object path (e.g. "/Game/Foo/Bar.Bar") to a package path
	 * ("/Game/Foo/Bar"). Tools like FindAssets return full object paths, but the AssetRegistry
	 * APIs expect package paths.
	 */
	UE_API FString ToPackagePath(const FString& Path);

	/** Returns whether an asset or folder exists at Path and is visible. */
	UE_API bool AssetExists(const FString& Path);

	/**
	 * Returns whether the creator can see what Path names (internal or virtual, object, package or folder path):
	 * an asset by its own visibility, anything else by its folder, a /Script/ package always.
	 */
	UE_API bool IsPathVisible(const FString& Path);

	/**
	 * Loads the asset at AssetPath (internal or virtual, object or package path) only if the creator can see it,
	 * null when missing or hidden. A /Script/ path loads directly: native classes are code, not content.
	 */
	UE_API UObject* LoadAssetIfVisible(const FSoftObjectPath& AssetPath);

	/**
	 * Returns whether the asset at AssetPath has unsaved changes (in-memory edits, or no on-disk
	 * package at all).
	 *
	 * Raises if the asset does not exist.
	 */
	UE_API bool AssetIsDirty(const FString& AssetPath);

	/**
	 * Loads AssetPath and requires it to be an instance of ExpectedClass.
	 * Raises if the asset cannot be loaded or is not of the expected type.
	 */
	UE_API UObject* LoadAssetForDiff(const FString& AssetPath, UClass* ExpectedClass);

	/**
	 * Returns a JSON object mapping each of Object's readable properties to its JSON-encoded
	 * value, for use as a building block of a toolset's diff canonicalization.
	 *
	 * Subobject paths and asset-name prefixes referring to Object itself are normalized to
	 * "<self>" so two objects with the same shape produce the same canonical value.
	 * Unreadable properties (e.g. editor-only properties whose getters are permission-denied)
	 * are skipped - both sides of a diff hit the same rejections, so omitting them doesn't mask
	 * changes.
	 */
	UE_API TSharedRef<FJsonObject> CanonicalProperties(UObject* Object);

	/**
	 * Returns a JSON-safe canonical form of an actor (or a Blueprint's CDO) with its class,
	 * properties, and components, for diffing. Actor must not be null.
	 */
	UE_API TSharedRef<FJsonObject> CanonicalActorStateJson(AActor* Actor);

	/**
	 * Serializes Value to text with every JSON object's keys sorted alphabetically at every
	 * level, so two independently-built canonical values with the same shape always produce
	 * byte-identical text regardless of the source container's iteration order.
	 */
	UE_API FString ToCanonicalJsonString(const TSharedPtr<FJsonValue>& Value);

	/**
	 * Returns a human-readable line diff between two canonicalized JSON values (see
	 * ToCanonicalJsonString). Empty when Old and New canonicalize to the same text.
	 */
	UE_API FString DiffCanonicalJson(const TSharedPtr<FJsonValue>& Old, const TSharedPtr<FJsonValue>& New);

	/**
	 * Loads OldAssetPath/NewAssetPath as T, canonicalizes each with Canonicalize, and returns
	 * their diff (see DiffCanonicalJson). Empty when either asset could not be loaded (already
	 * raised) or when the two canonicalize to the same value.
	 */
	template <typename T>
	FString DiffAssets(
		const FString& OldAssetPath, const FString& NewAssetPath,
		TFunctionRef<TSharedPtr<FJsonValue>(T*)> Canonicalize)
	{
		T* OldAsset = Cast<T>(LoadAssetForDiff(OldAssetPath, T::StaticClass()));
		if (!OldAsset)
		{
			return FString();
		}
		T* NewAsset = Cast<T>(LoadAssetForDiff(NewAssetPath, T::StaticClass()));
		if (!NewAsset)
		{
			return FString();
		}
		return DiffCanonicalJson(Canonicalize(OldAsset), Canonicalize(NewAsset));
	}
}

#undef UE_API
