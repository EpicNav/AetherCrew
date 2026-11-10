// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameplayTagsToolset/GameplayTagsToolset59.h"

#include "GameplayTagsToolset/GameplayTagsToolsetSettings.h"

#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "Kismet/KismetSystemLibrary.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GameplayTagsToolset59)

namespace
{
	TArray<FString> CollectDescendantTagNames(const TSharedPtr<FGameplayTagNode>& ParentNode)
	{
		TArray<FString> TagNames;
		TArray<TSharedPtr<FGameplayTagNode>> NodesToTraverse = ParentNode->GetChildTagNodes();
		while (NodesToTraverse.Num() > 0)
		{
			TSharedPtr<FGameplayTagNode> Node = NodesToTraverse.Pop(EAllowShrinking::No);
			if (Node.IsValid())
			{
				TagNames.Add(Node->GetCompleteTagName().ToString());
				NodesToTraverse.Append(Node->GetChildTagNodes());
			}
		}
		return TagNames;
	}

	IGameplayTagsEditorModule& GetModule()
	{
		return FModuleManager::LoadModuleChecked<IGameplayTagsEditorModule>(
			TEXT("GameplayTagsEditor"));
	}

	void RaiseScriptErrorFromModule(const TArray<FText>& Errors, const FString& Summary)
	{
		TArray<FString> Parts;
		Parts.Reserve(Errors.Num() + 1);
		Parts.Add(Summary);
		for (const FText& Error : Errors)
		{
			Parts.Add(Error.ToString());
		}
		UKismetSystemLibrary::RaiseScriptError(FString::Join(Parts, TEXT("\n")));
	}

	FGameplayTagSourceInfo MakeSourceInfo(const FGameplayTagSource& Source)
	{
		FGameplayTagSourceInfo Info;
		Info.Name = Source.SourceName.ToString();
		Info.SourceType = Source.SourceType;
		Info.ConfigFileName = Source.GetConfigFileName();
		return Info;
	}

	// The sources the editor's Add New Gameplay Tag dialog offers for a non-restricted tag.
	TArray<FString> GetTagListSourceNames()
	{
		TArray<const FGameplayTagSource*> Sources;
		UGameplayTagsManager::Get().FindTagSourcesWithType(EGameplayTagSourceType::DefaultTagList, Sources);
		UGameplayTagsManager::Get().FindTagSourcesWithType(EGameplayTagSourceType::TagList, Sources);
		TArray<FString> Names;
		for (const FGameplayTagSource* Source : Sources)
		{
			Names.Add(Source->SourceName.ToString());
		}
		Names.Sort();
		return Names;
	}
}

TArray<FString> UGameplayTagsToolset59::ListTags(const FString& ParentTag)
{
	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	TArray<FString> TagNames;

	if (ParentTag.IsEmpty())
	{
		FGameplayTagContainer AllTags;
		TagManager.RequestAllGameplayTags(AllTags, false);
		for (const FGameplayTag& Tag : AllTags)
		{
			TagNames.Add(Tag.GetTagName().ToString());
		}
	}
	else
	{
		TSharedPtr<FGameplayTagNode> ParentNode = TagManager.FindTagNode(FName(*ParentTag));
		if (ParentNode.IsValid())
		{
			TagNames = CollectDescendantTagNames(ParentNode);
		}
	}

	TagNames.Sort();
	return TagNames;
}

FGameplayTagInfo59 UGameplayTagsToolset59::GetTagInfo(const FString& TagName)
{
	TSharedPtr<FGameplayTagNode> TagNode =
		UGameplayTagsManager::Get().FindTagNode(FName(*TagName));

	if (!TagNode.IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(
			FString::Printf(TEXT("Gameplay tag '%s' does not exist."), *TagName));
		return FGameplayTagInfo59();
	}

	FGameplayTagInfo59 Info;
#if WITH_EDITORONLY_DATA
	Info.Comment = TagNode->GetDevComment();
	Info.Source = TagNode->GetFirstSourceName().ToString();
#endif

	for (const TSharedPtr<FGameplayTagNode>& Child : TagNode->GetChildTagNodes())
	{
		if (Child.IsValid())
		{
			Info.Children.Add(Child->GetCompleteTagName().ToString());
		}
	}
	return Info;
}

void UGameplayTagsToolset59::AddTag(
	const FString& TagName, const FString& DevComment, const FString& TagSource)
{
	if (TagName.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Tag name cannot be empty."));
		return;
	}

	FName SourceName = NAME_None;
	if (TagSource.IsEmpty())
	{
		const UGameplayTagsToolsetSettings* Settings = GetDefault<UGameplayTagsToolsetSettings>();
		if (Settings && Settings->EmptySourcePolicy ==
			EGameplayTagsToolsetEmptySourcePolicy::RequireNonEmptySource)
		{
			UKismetSystemLibrary::RaiseScriptError(TEXT(
				"TagSource is required by the Gameplay Tags Toolset project settings "
				"(EmptySourcePolicy = RequireNonEmptySource)."));
			return;
		}
	}
	else
	{
		SourceName = FName(*TagSource);
		// AddNewGameplayTagToINI would create a missing source in the project's Config/Tags, which the editor's
		// Add New Gameplay Tag dialog never does: it only offers existing sources.
		if (!UGameplayTagsManager::Get().FindTagSource(SourceName))
		{
			FString Message = FString::Printf(TEXT("Tag source '%s' does not exist."), *TagSource);
			const FString WithIni = TagSource + TEXT(".ini");
			if (!TagSource.EndsWith(TEXT(".ini")) && UGameplayTagsManager::Get().FindTagSource(FName(*WithIni)))
			{
				Message += FString::Printf(TEXT(" Did you mean '%s'?"), *WithIni);
			}
			Message += FString::Printf(TEXT(" Existing tag sources: %s. Use AddTagSource to create a new one."),
				*FString::Join(GetTagListSourceNames(), TEXT(", ")));
			UKismetSystemLibrary::RaiseScriptError(Message);
			return;
		}
	}

	TArray<FText> Errors;
	if (!GetModule().AddNewGameplayTagToINI(TagName, DevComment, SourceName, false, true, &Errors))
	{
		RaiseScriptErrorFromModule(Errors,
			FString::Printf(TEXT("Failed to add gameplay tag '%s'."), *TagName));
	}
}

TArray<FGameplayTagSourceInfo> UGameplayTagsToolset59::ListTagSources()
{
	TArray<FGameplayTagSourceInfo> Result;
	for (const EGameplayTagSourceType SourceType : TEnumRange<EGameplayTagSourceType>())
	{
		TArray<const FGameplayTagSource*> Sources;
		UGameplayTagsManager::Get().FindTagSourcesWithType(SourceType, Sources);
		for (const FGameplayTagSource* Source : Sources)
		{
			Result.Add(MakeSourceInfo(*Source));
		}
	}
	Result.Sort([](const FGameplayTagSourceInfo& A, const FGameplayTagSourceInfo& B) { return A.Name < B.Name; });
	return Result;
}

TArray<FString> UGameplayTagsToolset59::ListTagSourceRoots()
{
	TArray<FString> Roots;
	UGameplayTagsManager::Get().GetTagSourceSearchPaths(Roots);
	for (FString& Root : Roots)
	{
		Root = FPaths::ConvertRelativePathToFull(Root);
	}
	Roots.Sort();
	return Roots;
}

FGameplayTagSourceInfo UGameplayTagsToolset59::AddTagSource(const FString& SourceName, const FString& TagRoot)
{
	if (SourceName.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SourceName cannot be empty."));
		return FGameplayTagSourceInfo();
	}

	UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
	FString RootDirectory;
	if (!TagRoot.IsEmpty())
	{
		// The editor's Add New Gameplay Tag Source dialog only offers the registered search paths.
		TArray<FString> Roots;
		Manager.GetTagSourceSearchPaths(Roots);
		const FString* MatchingRoot = Roots.FindByPredicate(
			[&TagRoot](const FString& Root) { return FPaths::IsSamePath(Root, TagRoot); });
		if (!MatchingRoot)
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("'%s' is not a tag source directory. Use one of: %s."),
				*TagRoot, *FString::Join(ListTagSourceRoots(), TEXT(", "))));
			return FGameplayTagSourceInfo();
		}
		RootDirectory = *MatchingRoot;
	}

	const FString SourceFileName = SourceName.EndsWith(TEXT(".ini")) ? SourceName : SourceName + TEXT(".ini");
	// The engine builds the source's file path as <root>/<name>, so a name with path components would escape the root.
	if (FPaths::GetCleanFilename(SourceFileName) != SourceFileName || SourceFileName.Contains(TEXT("..")))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("SourceName must be a file name, not a path: '%s'."), *SourceName));
		return FGameplayTagSourceInfo();
	}
	if (const FGameplayTagSource* Existing = Manager.FindTagSource(FName(*SourceFileName)))
	{
		const FString ExpectedDirectory = RootDirectory.IsEmpty() ? FPaths::SourceConfigDir() / TEXT("Tags") : RootDirectory;
		if (Existing->SourceType != EGameplayTagSourceType::TagList
			|| !FPaths::IsSamePath(FPaths::GetPath(Existing->GetConfigFileName()), ExpectedDirectory))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("A tag source named '%s' already exists (%s, '%s')."), *SourceFileName,
				*UEnum::GetDisplayValueAsText(Existing->SourceType).ToString(), *Existing->GetConfigFileName()));
			return FGameplayTagSourceInfo();
		}
		return MakeSourceInfo(*Existing);
	}

	if (!GetModule().AddNewGameplayTagSource(SourceFileName, RootDirectory))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to add tag source '%s'."), *SourceFileName));
		return FGameplayTagSourceInfo();
	}
	const FGameplayTagSource* Added = Manager.FindTagSource(FName(*SourceFileName));
	return Added ? MakeSourceInfo(*Added) : FGameplayTagSourceInfo();
}

void UGameplayTagsToolset59::RemoveTag(const FString& TagName)
{
	TSharedPtr<FGameplayTagNode> TagNode =
		UGameplayTagsManager::Get().FindTagNode(FName(*TagName));
	if (!TagNode.IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(
			FString::Printf(TEXT("Gameplay tag '%s' does not exist."), *TagName));
		return;
	}
	TArray<FText> Errors;
	if (!GetModule().DeleteTagFromINI(TagNode, &Errors))
	{
		RaiseScriptErrorFromModule(Errors,
			FString::Printf(TEXT("Failed to remove gameplay tag '%s'."), *TagName));
	}
}

void UGameplayTagsToolset59::RenameTag(const FString& OldTagName, const FString& NewTagName)
{
	if (OldTagName.IsEmpty() || NewTagName.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Tag names cannot be empty."));
		return;
	}

	if (!UGameplayTagsManager::Get().FindTagNode(FName(*OldTagName)).IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(
			FString::Printf(TEXT("Gameplay tag '%s' does not exist."), *OldTagName));
		return;
	}

	TArray<FText> Errors;
	if (!GetModule().RenameTagInINI(OldTagName, NewTagName, true, &Errors))
	{
		RaiseScriptErrorFromModule(Errors, FString::Printf(
			TEXT("Failed to rename gameplay tag '%s' to '%s'."), *OldTagName, *NewTagName));
	}
}

FGameplayTagsMoveResult UGameplayTagsToolset59::MoveTags(
	const TArray<FString>& TagsToMove, const FString& TargetTagSource)
{
	FGameplayTagsMoveResult Result;

	if (TagsToMove.Num() == 0)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("TagsToMove cannot be empty."));
		return Result;
	}

	if (TargetTagSource.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("TargetTagSource cannot be empty."));
		return Result;
	}

	TArray<FText> Errors;
	GetModule().MoveTagsBetweenINI(
		TagsToMove, FName(*TargetTagSource), Result.TagsMoved, Result.FailedToMoveTags, &Errors);

	if (Errors.Num() > 0)
	{
		RaiseScriptErrorFromModule(Errors, FString::Printf(
			TEXT("Failed to move one or more gameplay tags to '%s'."), *TargetTagSource));
	}
	return Result;
}

TArray<FString> UGameplayTagsToolset59::ListTagsInSource(const FString& TagSource)
{
	if (TagSource.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("TagSource cannot be empty."));
		return TArray<FString>();
	}

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	const FName SourceName(*TagSource);

	const FGameplayTagSource* Source = TagManager.FindTagSource(SourceName);
	if (!Source)
	{
		UKismetSystemLibrary::RaiseScriptError(
			FString::Printf(TEXT("Tag source '%s' does not exist."), *TagSource));
		return TArray<FString>();
	}

	TArray<FString> Result;
	if (Source->SourceTagList)
	{
		for (const FGameplayTagTableRow& Row : Source->SourceTagList->GameplayTagList)
		{
			Result.Add(Row.Tag.ToString());
		}
	}

	Result.Sort();
	return Result;
}

TArray<FString> UGameplayTagsToolset59::FindReferencersByTag(const FString& TagName)
{
	if (TagName.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Tag name cannot be empty."));
		return TArray<FString>();
	}

	const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagName), false);
	if (!Tag.IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(
			FString::Printf(TEXT("Gameplay tag '%s' does not exist."), *TagName));
		return TArray<FString>();
	}

	const FAssetIdentifier TagIdentifier(FGameplayTag::StaticStruct(), Tag.GetTagName());

	IAssetRegistry& AssetRegistry = *IAssetRegistry::Get();
	TArray<FAssetIdentifier> Referencers;
	AssetRegistry.GetReferencers(TagIdentifier, Referencers,
		UE::AssetRegistry::EDependencyCategory::SearchableName);

	// Expand each referencer's package name to the full object path of every asset
	// in that package. FAssetIdentifier doesn't carry an asset name, so we have to
	// look up the asset data per referencer. Referencers whose packages aren't in
	// the registry are skipped — the canonical-form contract has no meaningful
	// bare-path fallback.
	TArray<FString> Result;
	Result.Reserve(Referencers.Num());
	for (const FAssetIdentifier& Id : Referencers)
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(Id.PackageName, Assets);
		for (const FAssetData& Asset : Assets)
		{
			Result.Add(Asset.GetSoftObjectPath().ToString());
		}
	}
	return Result;
}

TArray<FName> UGameplayTagsToolset59::GetToolsToHide_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	if (Provider->IsRestricted())
	{
		// Each tag mutator rewrites a project tag-source INI, which is outside the content a restricted
		// host lets its user author. The reads stay available.
		return {
			GET_FUNCTION_NAME_CHECKED(UGameplayTagsToolset59, AddTag),
			GET_FUNCTION_NAME_CHECKED(UGameplayTagsToolset59, AddTagSource),
			GET_FUNCTION_NAME_CHECKED(UGameplayTagsToolset59, RemoveTag),
			GET_FUNCTION_NAME_CHECKED(UGameplayTagsToolset59, RenameTag),
			GET_FUNCTION_NAME_CHECKED(UGameplayTagsToolset59, MoveTags)
		};
	}

	return {};
}
