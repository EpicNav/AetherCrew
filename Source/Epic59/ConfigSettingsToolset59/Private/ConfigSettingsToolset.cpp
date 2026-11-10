// Copyright Epic Games, Inc. All Rights Reserved.

#include "ConfigSettingsToolset59.h"

#include "Editor.h"
#include "Editor/Transactor.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IProjectManager.h"
#include "ISettingsCategory.h"
#include "ISettingsContainer.h"
#include "ISettingsModule.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/DataDrivenPlatformInfoRegistry.h"
#include "Misc/FileHelper.h"
#include "Misc/TransactionObjectEvent.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProjectDescriptor.h"
#include "ScopedTransaction.h" // UE 5.8: see SetSectionProperties
#include "SourceControlHelpers.h"
#include "ToolsetRegistry/ToolsetLibrary.h"
#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ConfigSettingsToolset59)

// --- Helpers ---
namespace UE::ConfigSettingsToolset::Private
{
	void RaiseError(const FString& Message)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("ConfigSettingsToolset: %s"), *Message));
	}

	ISettingsModule* GetSettingsModule()
	{
		return FModuleManager::GetModulePtr<ISettingsModule>("Settings");
	}

	// Checks a source-controlled file out when it isn't already open, raising with the provider's reason
	// if that fails. Returns false only on that failure; a file outside source control passes untouched.
	bool CheckOutIfSourceControlled(const FString& FilePath)
	{
		ISourceControlModule& SCModule = ISourceControlModule::Get();
		if (!SCModule.IsEnabled())
		{
			return true;
		}
		FSourceControlStatePtr State = SCModule.GetProvider().GetState(FilePath, EStateCacheUsage::ForceUpdate);
		if (State.IsValid() && State->IsSourceControlled() && !State->IsCheckedOut() && !State->IsAdded()
			&& !USourceControlHelpers::CheckOutOrAddFile(FilePath, /*bSilent=*/true))
		{
			RaiseError(FString::Printf(TEXT("'%s' could not be checked out of source control: %s"),
				*FPaths::GetCleanFilename(FilePath), *USourceControlHelpers::LastErrorMsg().ToString()));
			return false;
		}
		return true;
	}

	// Returns true if the edit can proceed. For CLASS_DefaultConfig objects, checks the backing
	// .ini out when source control is active, as the Settings editor does, and otherwise requires
	// it to be writable, raising a descriptive error and returning false when it can't be written.
	bool CheckDefaultConfigWritable(const UObject* SettingsObject)
	{
		if (!SettingsObject->GetClass()->HasAnyClassFlags(CLASS_DefaultConfig))
		{
			return true;
		}

		const FString ConfigPath = FPaths::ConvertRelativePathToFull(
			SettingsObject->GetDefaultConfigFilename());

		if (ISourceControlModule::Get().IsEnabled())
		{
			if (!CheckOutIfSourceControlled(ConfigPath))
			{
				return false;
			}
		}
		else if (IFileManager::Get().IsReadOnly(*ConfigPath))
		{
			RaiseError(FString::Printf(
				TEXT("'%s' is stored in a read-only default config file. "
					"Make '%s' writable before editing this setting."),
				*FPaths::GetCleanFilename(ConfigPath), *ConfigPath));
			return false;
		}

		return true;
	}

	FGuid GetNewestUndoTransactionId()
	{
		return GEditor && GEditor->Trans ? GEditor->Trans->GetUndoContext(/*bCheckWhetherUndoPossible=*/false).TransactionId : FGuid();
	}

	// Undoes the transaction SetObjectProperties recorded, which it keeps on failure, so a failed call leaves
	// nothing changed in memory. The transaction id check keeps an earlier, unrelated transaction from being
	// undone when the call recorded nothing. Undo only sends a property-less PostEditChange, which settings
	// classes ignore, so every property the transaction changed is notified again here for anything its change
	// notification updated (a CVar, for example). The transaction's list is used rather than the names
	// SetObjectProperties returns, which leave out a partly applied struct and can't name an array element.
	void UndoSetProperties(UObject& Object, const FGuid& PreviousUndoTransactionId)
	{
		const FGuid TransactionId = GetNewestUndoTransactionId();
		if (TransactionId == PreviousUndoTransactionId)
		{
			return;
		}
		TArray<FName> ChangedPropertyNames;
		if (const FTransaction* Transaction = GEditor->Trans->GetTransaction(GEditor->Trans->FindTransactionIndex(TransactionId)))
		{
			const FTransactionDiff Diff = Transaction->GenerateDiff();
			if (const TSharedPtr<FTransactionObjectEvent>* ObjectEvent = Diff.DiffMap.Find(FName(*Object.GetPathName())))
			{
				ChangedPropertyNames = (*ObjectEvent)->GetChangedProperties();
			}
		}
#if UE59_COMPAT // 5.8's UndoTransaction has no context argument
		GEditor->UndoTransaction(/*bCanRedo=*/false);
#else
		GEditor->UndoTransaction(/*bCanRedo=*/false, TEXT("ConfigSettingsToolset"));
#endif
		for (const FName& PropertyName : ChangedPropertyNames)
		{
			if (FProperty* Property = FindFProperty<FProperty>(Object.GetClass(), PropertyName))
			{
				FPropertyChangedEvent ChangedEvent(Property);
				Object.PostEditChangeProperty(ChangedEvent);
			}
		}
	}

	// True when every term appears, case-insensitively, in at least one of the fields.
	bool MatchesAllTerms(const TArray<FString>& Terms, std::initializer_list<const FString*> Fields)
	{
		for (const FString& Term : Terms)
		{
			bool bFound = false;
			for (const FString* Field : Fields)
			{
				if (Field->Contains(Term, ESearchCase::IgnoreCase))
				{
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				return false;
			}
		}
		return true;
	}

}


// --- Private ---

TSharedPtr<ISettingsSection> UConfigSettingsToolset59::FindSection(
	const FString& ContainerName,
	const FString& CategoryName,
	const FString& SectionName)
{
	ISettingsModule* SettingsModule = UE::ConfigSettingsToolset::Private::GetSettingsModule();
	if (!SettingsModule)
	{
		return nullptr;
	}
	TSharedPtr<ISettingsContainer> Container = SettingsModule->GetContainer(FName(*ContainerName));
	if (!Container.IsValid())
	{
		return nullptr;
	}
	TSharedPtr<ISettingsCategory> Category = Container->GetCategory(FName(*CategoryName));
	if (!Category.IsValid())
	{
		return nullptr;
	}
	return Category->GetSection(FName(*SectionName));
}


// --- Discovery ---

TArray<FString> UConfigSettingsToolset59::ListContainers()
{
	TArray<FName> Names;
	if (ISettingsModule* SettingsModule = UE::ConfigSettingsToolset::Private::GetSettingsModule())
	{
		SettingsModule->GetContainerNames(Names);
	}
	TArray<FString> Result;
	Result.Reserve(Names.Num());
	for (const FName& Name : Names)
	{
		Result.Add(Name.ToString());
	}
	Result.Sort();
	return Result;
}

TArray<FString> UConfigSettingsToolset59::ListCategories(const FString& ContainerName)
{
	ISettingsModule* SettingsModule = UE::ConfigSettingsToolset::Private::GetSettingsModule();
	if (!SettingsModule)
	{
		UE::ConfigSettingsToolset::Private::RaiseError(TEXT("Settings module is not available."));
		return {};
	}
	TSharedPtr<ISettingsContainer> Container = SettingsModule->GetContainer(FName(*ContainerName));
	if (!Container.IsValid())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Container not found: %s"), *ContainerName));
		return {};
	}
	TArray<TSharedPtr<ISettingsCategory>> Categories;
	Container->GetCategories(Categories);
	TArray<FString> Result;
	Result.Reserve(Categories.Num());
	for (const TSharedPtr<ISettingsCategory>& Category : Categories)
	{
		if (Category.IsValid())
		{
			Result.Add(Category->GetName().ToString());
		}
	}
	Result.Sort();
	return Result;
}

TArray<FString> UConfigSettingsToolset59::ListSections(
	const FString& ContainerName,
	const FString& CategoryName)
{
	ISettingsModule* SettingsModule = UE::ConfigSettingsToolset::Private::GetSettingsModule();
	if (!SettingsModule)
	{
		UE::ConfigSettingsToolset::Private::RaiseError(TEXT("Settings module is not available."));
		return {};
	}
	TSharedPtr<ISettingsContainer> Container = SettingsModule->GetContainer(FName(*ContainerName));
	if (!Container.IsValid())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Container not found: %s"), *ContainerName));
		return {};
	}
	TSharedPtr<ISettingsCategory> Category = Container->GetCategory(FName(*CategoryName));
	if (!Category.IsValid())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Category not found: %s/%s"), *ContainerName, *CategoryName));
		return {};
	}
	TArray<TSharedPtr<ISettingsSection>> Sections;
	Category->GetSections(Sections);
	TArray<FString> Result;
	Result.Reserve(Sections.Num());
	for (const TSharedPtr<ISettingsSection>& Section : Sections)
	{
		if (Section.IsValid())
		{
			Result.Add(Section->GetName().ToString());
		}
	}
	Result.Sort();
	return Result;
}

TArray<FConfigSettingsSearchResult> UConfigSettingsToolset59::SearchSettings(const FString& Query)
{
	TArray<FString> Terms;
	Query.ParseIntoArrayWS(Terms);
	if (Terms.IsEmpty())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(TEXT("Search query cannot be empty."));
		return {};
	}
	ISettingsModule* SettingsModule = UE::ConfigSettingsToolset::Private::GetSettingsModule();
	if (!SettingsModule)
	{
		UE::ConfigSettingsToolset::Private::RaiseError(TEXT("Settings module is not available."));
		return {};
	}

	TArray<FConfigSettingsSearchResult> Results;
	TArray<FName> ContainerNames;
	SettingsModule->GetContainerNames(ContainerNames);
	for (const FName& ContainerName : ContainerNames)
	{
		TSharedPtr<ISettingsContainer> Container = SettingsModule->GetContainer(ContainerName);
		if (!Container.IsValid())
		{
			continue;
		}
		TArray<TSharedPtr<ISettingsCategory>> Categories;
		Container->GetCategories(Categories);
		for (const TSharedPtr<ISettingsCategory>& Category : Categories)
		{
			if (!Category.IsValid())
			{
				continue;
			}
			TArray<TSharedPtr<ISettingsSection>> Sections;
			Category->GetSections(Sections);
			for (const TSharedPtr<ISettingsSection>& Section : Sections)
			{
				if (!Section.IsValid())
				{
					continue;
				}
				auto AddResult = [&Results, &ContainerName, &Category, &Section](const FString& PropertyName, const FString& DisplayName,
					const FString& ConsoleVariableName)
				{
					FConfigSettingsSearchResult& Result = Results.AddDefaulted_GetRef();
					Result.ContainerName = ContainerName.ToString();
					Result.CategoryName = Category->GetName().ToString();
					Result.SectionName = Section->GetName().ToString();
					Result.PropertyName = PropertyName;
					Result.DisplayName = DisplayName;
					Result.ConsoleVariableName = ConsoleVariableName;
				};

				const FString SectionName = Section->GetName().ToString();
				const FString SectionDisplayName = Section->GetDisplayName().ToString();
				if (UE::ConfigSettingsToolset::Private::MatchesAllTerms(Terms, { &SectionName, &SectionDisplayName }))
				{
					AddResult(FString(), SectionDisplayName, FString());
				}

				const UObject* SettingsObject = Section->GetSettingsObject().Get();
				if (!SettingsObject)
				{
					continue;
				}
				for (TFieldIterator<FProperty> It(SettingsObject->GetClass()); It; ++It)
				{
					const FProperty* Property = *It;
					if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Deprecated))
					{
						continue;
					}
					const FString PropertyName = Property->GetName();
					const FString DisplayName = Property->GetDisplayNameText().ToString();
					const FString ToolTip = Property->GetToolTipText().ToString();
					const FString ConsoleVariableName = Property->GetMetaData(TEXT("ConsoleVariable"));
					if (UE::ConfigSettingsToolset::Private::MatchesAllTerms(Terms, { &PropertyName, &DisplayName, &ToolTip, &ConsoleVariableName }))
					{
						AddResult(PropertyName, DisplayName, ConsoleVariableName);
					}
				}
			}
		}
	}

	Results.Sort([](const FConfigSettingsSearchResult& A, const FConfigSettingsSearchResult& B)
	{
		if (A.ContainerName != B.ContainerName)
		{
			return A.ContainerName < B.ContainerName;
		}
		if (A.CategoryName != B.CategoryName)
		{
			return A.CategoryName < B.CategoryName;
		}
		if (A.SectionName != B.SectionName)
		{
			return A.SectionName < B.SectionName;
		}
		return A.PropertyName < B.PropertyName;
	});
	return Results;
}


// --- Schema & Values ---

FString UConfigSettingsToolset59::GetSectionSchema(
	const FString& ContainerName,
	const FString& CategoryName,
	const FString& SectionName)
{
	TSharedPtr<ISettingsSection> Section = FindSection(ContainerName, CategoryName, SectionName);
	if (!Section.IsValid())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Section not found: %s/%s/%s"), *ContainerName, *CategoryName, *SectionName));
		return FString();
	}
	UObject* SettingsObject = Section->GetSettingsObject().Get();
	if (!SettingsObject)
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Section has no settings object (may use a custom widget): %s/%s/%s"),
				*ContainerName, *CategoryName, *SectionName));
		return FString();
	}
	return UToolsetLibrary::ListStructProperties(SettingsObject->GetClass());
}

FString UConfigSettingsToolset59::GetSectionPropertyValues(
	const FString& ContainerName,
	const FString& CategoryName,
	const FString& SectionName,
	const TArray<FString>& PropertyNames)
{
	TSharedPtr<ISettingsSection> Section = FindSection(ContainerName, CategoryName, SectionName);
	if (!Section.IsValid())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Section not found: %s/%s/%s"), *ContainerName, *CategoryName, *SectionName));
		return FString();
	}
	UObject* SettingsObject = Section->GetSettingsObject().Get();
	if (!SettingsObject)
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Section has no settings object (may use a custom widget): %s/%s/%s"),
				*ContainerName, *CategoryName, *SectionName));
		return FString();
	}
	TArray<FName> Names;
	Names.Reserve(PropertyNames.Num());
	for (const FString& Name : PropertyNames)
	{
		Names.Add(FName(*Name));
	}
	return UToolsetLibrary::GetObjectProperties(SettingsObject, Names);
}


// --- Editing ---

bool UConfigSettingsToolset59::SetSectionProperties(
	const FString& ContainerName,
	const FString& CategoryName,
	const FString& SectionName,
	const FString& PropertiesJson)
{
	TSharedPtr<ISettingsSection> Section = FindSection(ContainerName, CategoryName, SectionName);
	if (!Section.IsValid())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Section not found: %s/%s/%s"), *ContainerName, *CategoryName, *SectionName));
		return false;
	}
	if (!Section->CanEdit())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Section cannot be edited: %s/%s/%s"), *ContainerName, *CategoryName, *SectionName));
		return false;
	}
	UObject* SettingsObject = Section->GetSettingsObject().Get();
	if (!SettingsObject)
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Section has no settings object: %s/%s/%s"), *ContainerName, *CategoryName, *SectionName));
		return false;
	}
	// Settings objects aren't created transactional, and the rollback below relies on the edit being recorded.
	SettingsObject->SetFlags(RF_Transactional);
	const FGuid PreviousUndoTransactionId = UE::ConfigSettingsToolset::Private::GetNewestUndoTransactionId();
	TArray<FName> SetPropertyNames;
	bool bSetSuccess = false;
	{
		// UE 5.8: 5.9's SetObjectProperties makes the whole call one undo transaction, which the rollback below
		// undoes; 5.8's records none, so the call is wrapped in one here.
		const FScopedTransaction Transaction(INVTEXT("Set Section Properties"));
		bSetSuccess = UToolsetLibrary::SetObjectProperties(
			SettingsObject, PropertiesJson, SetPropertyNames, EBypassContainerCheck::No); // UE 5.8: no PropertyListHint argument
	}
	if (!bSetSuccess)
	{
		// SetObjectProperties has already raised the error, and since nothing is left changed there is nothing to save.
		UE::ConfigSettingsToolset::Private::UndoSetProperties(*SettingsObject, PreviousUndoTransactionId);
		return false;
	}
	if (SetPropertyNames.IsEmpty())
	{
		return true;
	}
	// Checked only now so a rejected call never checks the file out.
	if (!UE::ConfigSettingsToolset::Private::CheckDefaultConfigWritable(SettingsObject))
	{
		UE::ConfigSettingsToolset::Private::UndoSetProperties(*SettingsObject, PreviousUndoTransactionId);
		return false;
	}

	// Per-property config save (mirrors SSettingsEditor::HandleSettingsPropertyChanged).
	// Use UpdateSinglePropertyInConfigFile for individual DefaultConfig properties;
	// fall back to a full Section->Save() for collection types when the CVar is absent/0.
	const UClass* ObjectClass = SettingsObject->GetClass();
	const bool bIsDefaultConfig = ObjectClass->HasAnyClassFlags(CLASS_DefaultConfig);
	if(!bIsDefaultConfig)
	{
		if(!Section->Save())
		{
			UE::ConfigSettingsToolset::Private::RaiseError(
				FString::Printf(TEXT("Failed to save section: %s/%s/%s"), *ContainerName, *CategoryName, *SectionName));
			return false;			
		}
		return bSetSuccess;
	}

	if(!Section->NotifySectionOnPropertyModified())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Failed to notify section property modified: %s/%s/%s"), *ContainerName, *CategoryName, *SectionName));
		return false;
	}

	IConsoleVariable* NewPropertySavingCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ini.UseNewPropertySaving"));
	const bool bCanUsePerPropertySaveForContainers = NewPropertySavingCVar && NewPropertySavingCVar->GetInt() != 0;

	for (const FName& PropertyName : SetPropertyNames)
	{
		FProperty* Prop = FindFProperty<FProperty>(ObjectClass, PropertyName);
		if(ensureAlways(Prop))
		{
			if(bCanUsePerPropertySaveForContainers || 
				!(Prop->IsA<FArrayProperty>() || Prop->IsA<FSetProperty>() || Prop->IsA<FMapProperty>()))
			{
				SettingsObject->UpdateSinglePropertyInConfigFile(Prop, SettingsObject->GetDefaultConfigFilename());
			}
			else
			{
				// Once the whole section has been saved, we no longer need to iterate individual properties
				if (!Section->Save())
				{
					UE::ConfigSettingsToolset::Private::RaiseError(
						FString::Printf(TEXT("Failed to save section: %s/%s/%s"), *ContainerName, *CategoryName, *SectionName));
					return false;
				}
				return bSetSuccess;
			}
		}
	}
	return bSetSuccess;
}

bool UConfigSettingsToolset59::SaveSection(
	const FString& ContainerName,
	const FString& CategoryName,
	const FString& SectionName)
{
	TSharedPtr<ISettingsSection> Section = FindSection(ContainerName, CategoryName, SectionName);
	if (!Section.IsValid())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Section not found: %s/%s/%s"), *ContainerName, *CategoryName, *SectionName));
		return false;
	}
	if (!Section->CanSave())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Section does not support saving: %s/%s/%s"), *ContainerName, *CategoryName, *SectionName));
		return false;
	}
	const UObject* SettingsObject = Section->GetSettingsObject().Get();
	if (SettingsObject && !UE::ConfigSettingsToolset::Private::CheckDefaultConfigWritable(SettingsObject))
	{
		return false;
	}
	return Section->Save();
}

bool UConfigSettingsToolset59::ResetSectionToDefaults(
	const FString& ContainerName,
	const FString& CategoryName,
	const FString& SectionName)
{
	TSharedPtr<ISettingsSection> Section = FindSection(ContainerName, CategoryName, SectionName);
	if (!Section.IsValid())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Section not found: %s/%s/%s"), *ContainerName, *CategoryName, *SectionName));
		return false;
	}
	if (!Section->CanResetDefaults())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(
			FString::Printf(TEXT("Section does not support reset to defaults: %s/%s/%s"), *ContainerName, *CategoryName, *SectionName));
		return false;
	}
	return Section->ResetDefaults();
}

// --- Supported Platforms ---

TArray<FString> UConfigSettingsToolset59::ListAvailablePlatforms()
{
	TArray<FString> Result;
	for (const FDataDrivenPlatformInfo* PlatformInfo : FDataDrivenPlatformInfoRegistry::GetSortedPlatformInfos(EPlatformInfoType::TruePlatformsOnly))
	{
		Result.Add(PlatformInfo->IniPlatformName.ToString());
	}
	return Result;
}

TArray<FString> UConfigSettingsToolset59::GetSupportedPlatforms()
{
	const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
	if (!Project)
	{
		UE::ConfigSettingsToolset::Private::RaiseError(TEXT("No project is loaded."));
		return {};
	}
	TArray<FString> Result;
	for (const FName& Platform : Project->TargetPlatforms)
	{
		Result.Add(Platform.ToString());
	}
	return Result;
}

void UConfigSettingsToolset59::SetSupportedPlatforms(const TArray<FString>& Platforms)
{
	IProjectManager& ProjectManager = IProjectManager::Get();
	const FProjectDescriptor* Project = ProjectManager.GetCurrentProject();
	if (!Project)
	{
		UE::ConfigSettingsToolset::Private::RaiseError(TEXT("No project is loaded."));
		return;
	}

	const TArray<FName> OldPlatforms = Project->TargetPlatforms;

	// A project can already list platforms whose platform data isn't installed here, so those stay valid too.
	TArray<FString> KnownPlatforms = ListAvailablePlatforms();
	for (const FName& Platform : OldPlatforms)
	{
		KnownPlatforms.AddUnique(Platform.ToString());
	}

	TArray<FName> NewPlatforms;
	for (const FString& Platform : Platforms)
	{
		const FString* Match = KnownPlatforms.FindByPredicate(
			[&Platform](const FString& Known) { return Known.Equals(Platform, ESearchCase::IgnoreCase); });
		if (!Match)
		{
			UE::ConfigSettingsToolset::Private::RaiseError(FString::Printf(TEXT("Unknown platform '%s'. Available platforms: %s"),
				*Platform, *FString::Join(ListAvailablePlatforms(), TEXT(", "))));
			return;
		}
		NewPlatforms.AddUnique(FName(**Match));
	}
	const bool bUnchanged = OldPlatforms.Num() == NewPlatforms.Num() && !NewPlatforms.ContainsByPredicate(
		[&OldPlatforms](const FName& Platform) { return !OldPlatforms.Contains(Platform); });
	if (bUnchanged)
	{
		return;
	}

	// The project manager's platform functions write the project file without checking suppression, and there is
	// no other way to change the list, so honoring suppression means refusing the change.
	if (ProjectManager.IsSuppressingProjectFileWrite())
	{
		UE::ConfigSettingsToolset::Private::RaiseError(TEXT("Project file writes are suppressed, so supported platforms cannot be changed."));
		return;
	}

	if (!UE::ConfigSettingsToolset::Private::CheckOutIfSourceControlled(FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())))
	{
		return;
	}

	auto ApplyPlatforms = [&ProjectManager](const TArray<FName>& PlatformsToApply)
	{
		ProjectManager.ClearSupportedTargetPlatformsForCurrentProject();
		for (const FName& Platform : PlatformsToApply)
		{
			ProjectManager.UpdateSupportedTargetPlatformsForCurrentProject(Platform, /*bIsSupported=*/true);
		}
	};
	ApplyPlatforms(NewPlatforms);

	// Those functions save too but discard the result, so save again to learn whether the change reached the
	// disk, and put the old list back in memory if it didn't.
	FText FailReason;
	if (!ProjectManager.SaveCurrentProjectToDisk(FailReason))
	{
		ApplyPlatforms(OldPlatforms);
		UE::ConfigSettingsToolset::Private::RaiseError(FString::Printf(TEXT("Project file could not be saved: %s"), *FailReason.ToString()));
	}
}

bool UConfigSettingsToolset59::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}
