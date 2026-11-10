// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "GameplayTagsManager.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions

#include "GameplayTagsToolset59.generated.h"

/// Describes the properties of a gameplay tag.
USTRUCT(BlueprintType)
struct FGameplayTagInfo59
{
	GENERATED_BODY()

	/// The developer comment describing the tag's purpose.
	UPROPERTY(BlueprintReadWrite, Category = "GameplayTag")
	FString Comment;

	/// The name of the INI source file that defines this tag (e.g. "MyPluginGameplayTags.ini").
	UPROPERTY(BlueprintReadWrite, Category = "GameplayTag")
	FString Source;

	/// The fully-qualified names of this tag's immediate children.
	UPROPERTY(BlueprintReadWrite, Category = "GameplayTag")
	TArray<FString> Children;
};

/// Result of a MoveTags call describing which tags were moved and which failed.
USTRUCT(BlueprintType)
struct FGameplayTagsMoveResult
{
	GENERATED_BODY()

	/// The fully-qualified names of tags that were successfully moved to the target source.
	UPROPERTY(BlueprintReadWrite, Category = "GameplayTag")
	TArray<FString> TagsMoved;

	/// The fully-qualified names of tags the module declined to move.
	UPROPERTY(BlueprintReadWrite, Category = "GameplayTag")
	TArray<FString> FailedToMoveTags;
};

/// Describes a source that gameplay tags are loaded from and saved to.
USTRUCT(BlueprintType)
struct FGameplayTagSourceInfo
{
	GENERATED_BODY()

	/// The source's name, which is what TagSource arguments take (e.g. "MyPluginGameplayTags.ini").
	UPROPERTY(BlueprintReadWrite, Category = "GameplayTag")
	FString Name;

	/// Where the source's tags come from. Tags can be added to DefaultTagList and TagList sources.
	UPROPERTY(BlueprintReadWrite, Category = "GameplayTag")
	EGameplayTagSourceType SourceType = EGameplayTagSourceType::Invalid;

	/// The config file the source is saved to, or empty for sources not saved to one (Native, DataTable).
	UPROPERTY(BlueprintReadWrite, Category = "GameplayTag")
	FString ConfigFileName;
};

/// Provides tools for reading and managing gameplay tags in INI tag source files via the project's GameplayTagsManager.
UCLASS(BlueprintType, Hidden)
class UGameplayTagsToolset59 : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	/**
	 * Returns gameplay tags registered in the project.
	 * @param ParentTag If non-empty, only tags that are descendants of this tag are returned.
	 *   For example, passing "Character.State" returns "Character.State.Dead",
	 *   "Character.State.Stunned", etc. Pass an empty string to return all tags.
	 * @return A sorted list of fully-qualified tag names.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameplayTags")
	static TArray<FString> ListTags(const FString& ParentTag);

	/**
	 * Returns detailed information about a specific gameplay tag.
	 * @param TagName The fully-qualified name of the tag, e.g. "Character.State.Dead".
	 * @return The tag's developer comment, source INI file, and immediate children.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameplayTags")
	static FGameplayTagInfo59 GetTagInfo(const FString& TagName);

	/**
	 * Adds a new gameplay tag to the project.
	 * This should ONLY be called after getting explicit direction or permission from the user.
	 * @param TagName The fully-qualified name of the tag to add, e.g. "Character.State.Dead".
	 * @param DevComment An optional developer comment describing the tag's purpose.
	 * @param TagSource An existing INI source to add the tag to (see ListTagSources); use AddTagSource to create
	 *   a new one first. When empty, behavior is controlled by the Gameplay Tags Toolset project setting
	 *   "EmptySourcePolicy": either falls back to the GameplayTagsManager's default source, or rejects the call.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "GameplayTags")
	static void AddTag(const FString& TagName, const FString& DevComment, const FString& TagSource);

	/**
	 * Returns every gameplay tag source, sorted by name.
	 * @return Each source's name, type and config file.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameplayTags")
	static TArray<FGameplayTagSourceInfo> ListTagSources();

	/**
	 * Returns the directories a new tag source can be created in: the project's Config/Tags directory and
	 * each registered plugin's.
	 * @return Absolute directory paths, sorted.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameplayTags")
	static TArray<FString> ListTagSourceRoots();

	/**
	 * Creates a new tag list source that tags can then be added to. Its file is written when the first tag
	 * is added. No-ops if a tag list source with that name already exists in the same directory.
	 * This should ONLY be called after getting explicit direction or permission from the user.
	 * @param SourceName The source's name, e.g. "MyPluginGameplayTags.ini". ".ini" is appended if missing.
	 * @param TagRoot The directory to create the source in, from ListTagSourceRoots. When empty, the source is
	 *   created in the project's Config/Tags directory.
	 * @return The new source.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "GameplayTags")
	static FGameplayTagSourceInfo AddTagSource(const FString& SourceName, const FString& TagRoot);

	/**
	 * Removes a gameplay tag from the project.
	 * This should ONLY be called after getting explicit direction or permission from the user.
	 * @param TagName The fully-qualified name of the tag to remove, e.g. "Character.State.Dead".
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "GameplayTags")
	static void RemoveTag(const FString& TagName);

	/**
	 * Renames a gameplay tag, updating all references in the project.
	 * This should ONLY be called after getting explicit direction or permission from the user.
	 * @param OldTagName The fully-qualified name of the tag to rename.
	 * @param NewTagName The new fully-qualified name for the tag.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "GameplayTags")
	static void RenameTag(const FString& OldTagName, const FString& NewTagName);

	/**
	 * Moves a set of gameplay tags from their current INI source to a different one.
	 * This should ONLY be called after getting explicit direction or permission from the user.
	 * @param TagsToMove The fully-qualified names of the tags to move.
	 * @param TargetTagSource The INI source to move the tags into, e.g. "MyCoolPluginTags.ini".
	 * @return The tags the module reported as moved, and any it declined to move.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "GameplayTags")
	static FGameplayTagsMoveResult MoveTags(
		const TArray<FString>& TagsToMove, const FString& TargetTagSource);

	/**
	 * Returns all gameplay tags defined in a specific tag source.
	 * @param TagSource The INI source file name to list tags from, e.g. "MyPluginGameplayTags.ini".
	 * @return A sorted list of fully-qualified tag names explicitly defined in that source.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameplayTags")
	static TArray<FString> ListTagsInSource(const FString& TagSource);

	/**
	 * Returns assets that reference a gameplay tag.
	 * @param TagName The fully-qualified gameplay tag to search for, e.g. "Character.State.Dead".
	 * @return A list of full object paths to assets that reference the tag.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameplayTags")
	static TArray<FString> FindReferencersByTag(const FString& TagName);

	//~ Begin UToolsetDefinition Interface
	virtual TArray<FName> GetToolsToHide_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition Interface
};
