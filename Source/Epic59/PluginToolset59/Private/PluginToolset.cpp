// Copyright Epic Games, Inc. All Rights Reserved.

#include "PluginToolset59.h"

#include "Containers/VersePath.h"
#include "Features/IPluginsEditorFeature.h"
#include "Features/EditorFeatures.h"
#include "Features/IModularFeatures.h"
#include "HAL/FileManager.h"
#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"
#include "ISourceControlProvider.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "PluginDescriptor.h"
#include "PluginDisallowedDescriptor.h"
#include "ProjectDescriptor.h"
#include "PluginReferenceDescriptor.h"
#include "PluginUtils.h"
#include "SourceControlOperations.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PluginToolset59)

// --- Helpers ---
namespace UE::PluginToolset::Private
{
	void RaiseError(const FString& Message)
	{
		UKismetSystemLibrary::RaiseScriptError(	FString::Printf(TEXT("PluginToolset: %s"), *Message));
	}

	// Mirrors SPluginTile::GetAuthoringButtonsVisibility: returns false for engine plugins in
	// installed engine builds, and for non-Mod plugins in any installed build.
	bool CanEditPluginDescriptor(const IPlugin& Plugin)
	{
		if (FApp::IsEngineInstalled() && Plugin.GetLoadedFrom() == EPluginLoadedFrom::Engine)
		{
			RaiseError(FString::Printf(TEXT("Cannot edit descriptor of engine plugin '%s' in an installed engine build."), *Plugin.GetName()));
			return false;
		}
		if (FApp::IsInstalled() && Plugin.GetType() != EPluginType::Mod)
		{
			RaiseError(FString::Printf(TEXT("Cannot edit descriptor of plugin '%s' in an installed build (only Mod plugins may be edited)."), *Plugin.GetName()));
			return false;
		}
		return true;
	}

	// Checks the file out via SCC when it is enabled and the file can be checked out. Unlike
	// GameProjectUtils::TryMakeProjectFileWriteable this never opens a dialog, so a read-only
	// file is left for the caller's write to fail on.
	void CheckOutFileIfPossible(const FString& FileName)
	{
		ISourceControlModule& SCCModule = ISourceControlModule::Get();
		if (SCCModule.IsEnabled())
		{
			ISourceControlProvider& Provider = SCCModule.GetProvider();
			TSharedPtr<ISourceControlState, ESPMode::ThreadSafe> State =
				Provider.GetState(FileName, EStateCacheUsage::ForceUpdate);
			if (State.IsValid() && State->CanCheckout())
			{
				Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), FileName);
			}
		}
	}

	// Checks out the file via SCC if available, then calls IPlugin::UpdateDescriptor.
	// Skips the write entirely if the serialized descriptor text is unchanged.
	// Returns true on success; calls RaiseError and returns false on failure.
	bool UpdateDescriptorInternal(IPlugin& Plugin, const FPluginDescriptor& NewDescriptor)
	{
		if (!CanEditPluginDescriptor(Plugin))
		{
			return false;
		}

		FString OldText;
		Plugin.GetDescriptor().Write(OldText);
		FString NewText;
		NewDescriptor.Write(NewText);

		if (OldText.Compare(NewText, ESearchCase::CaseSensitive) == 0)
		{
			return true;
		}

		CheckOutFileIfPossible(Plugin.GetDescriptorFileName());

		FText FailReason;
		if (!Plugin.UpdateDescriptor(NewDescriptor, FailReason))
		{
			RaiseError(FailReason.ToString());
			return false;
		}
		return true;
	}

	FPluginTemplateDescriptionToolsetInfo59 ToToolsetTemplateDescription(FPluginTemplateDescription& PluginTemplate)
	{
		FPluginTemplateDescriptionToolsetInfo59 ToolsetInfo;
		ToolsetInfo.Name = PluginTemplate.Name;
		ToolsetInfo.Description = PluginTemplate.Description;
		ToolsetInfo.OnDiskPath = PluginTemplate.OnDiskPath;
		PluginTemplate.UpdatePluginNameTextWhenTemplateSelected(ToolsetInfo.DefaultTemplateName);
		ToolsetInfo.bCanBePlacedInEngine = PluginTemplate.bCanBePlacedInEngine && !FApp::IsInstalled();
		return ToolsetInfo;
	}

	// OnDiskPath alone identifies a template; the display text is long and callers tend to abbreviate it.
	bool ToolsetTemplateDescriptionMatches(const FPluginTemplateDescriptionToolsetInfo59& ToolsetInfo,
		const FPluginTemplateDescription& PluginTemplate)
	{
		return !ToolsetInfo.OnDiskPath.IsEmpty() && ToolsetInfo.OnDiskPath == PluginTemplate.OnDiskPath;
	}

	// Mirrors SPluginPaths::CanModifyProjectPaths.
	bool CanModifyProjectPluginDirectories()
	{
		bool bCanModify = true;
		if (!GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanModifyProjectPluginDirectoriesFromBrowser"), bCanModify, GEditorIni))
		{
			GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanModifyPluginsFromBrowser"), bCanModify, GEditorIni);
		}
		if (!bCanModify)
		{
			RaiseError(TEXT("Modifying project plugin directories is disabled in Editor Settings."));
		}
		return bCanModify;
	}

	void UpdateAdditionalPluginDirectory(const FString& Directory, bool bAdd)
	{
		if (!CanModifyProjectPluginDirectories())
		{
			return;
		}

		const FString FullDirectory = FPaths::ConvertRelativePathToFull(Directory);
		if (bAdd && !FPaths::DirectoryExists(FullDirectory))
		{
			RaiseError(FString::Printf(TEXT("Directory does not exist: %s"), *FullDirectory));
			return;
		}

		IProjectManager& ProjectManager = IProjectManager::Get();
		const bool bIsListed = ProjectManager.GetAdditionalPluginDirectories().ContainsByPredicate(
			[&FullDirectory](const FString& Listed) { return FPaths::IsSamePath(Listed, FullDirectory); });
		if (bAdd && bIsListed)
		{
			return;
		}
		if (!bAdd && !bIsListed)
		{
			RaiseError(FString::Printf(TEXT("Directory is not an additional plugin directory: %s"), *FullDirectory));
			return;
		}

		// UpdateAdditionalPluginDirectory writes the project file without checking suppression, and there is no
		// other way to change the list, so honoring suppression means refusing the change.
		if (ProjectManager.IsSuppressingProjectFileWrite())
		{
			RaiseError(TEXT("Project file writes are suppressed, so additional plugin directories cannot be changed."));
			return;
		}

		CheckOutFileIfPossible(FPaths::GetProjectFilePath());
		if (!ProjectManager.UpdateAdditionalPluginDirectory(FullDirectory, bAdd))
		{
			return;
		}

		// UpdateAdditionalPluginDirectory saves too but discards the result, so save again to learn whether
		// the change reached the disk, and undo it in memory if it didn't.
		FText FailReason;
		if (!ProjectManager.SaveCurrentProjectToDisk(FailReason))
		{
			ProjectManager.UpdateAdditionalPluginDirectory(FullDirectory, !bAdd);
			RaiseError(FString::Printf(TEXT("Project file could not be saved: %s"), *FailReason.ToString()));
			return;
		}

		IPluginManager::Get().RefreshPluginsList();
	}
}


// --- Read Methods ---

TArray<FString> UPluginToolset59::ListEnabledPlugins()
{
	TArray<FString> Names;
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
	{
		Names.Add(Plugin->GetName());
	}
	Names.Sort();
	return Names;
}

TArray<FString> UPluginToolset59::ListDiscoveredPlugins()
{
	TArray<FString> Names;
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
	{
		Names.Add(Plugin->GetName());
	}
	Names.Sort();
	return Names;
}

FPluginToolsetInfo59 UPluginToolset59::GetPluginInfo(const FString& PluginName)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return FPluginToolsetInfo59();
	}

	const FPluginDescriptor& Desc = Plugin->GetDescriptor();

	FPluginToolsetInfo59 Info;
	Info.Description = Desc.Description;
	Info.Version = Desc.Version;
	Info.VersionName = Desc.VersionName;
	Info.BaseDir = Plugin->GetBaseDir();
	Info.ContentDir = Plugin->GetContentDir();
	Info.DescriptorPath = Plugin->GetDescriptorFileName();
	Info.MountedAssetPath = Plugin->GetMountedAssetPath();
	return Info;
}

TArray<FPluginDependencyToolsetInfo59> UPluginToolset59::GetPluginDependencies(const FString& PluginName)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return {};
	}

	TArray<FPluginDependencyToolsetInfo59> Result;
	for (const FPluginReferenceDescriptor& Ref : Plugin->GetDescriptor().Plugins)
	{
		FPluginDependencyToolsetInfo59& Entry = Result.AddDefaulted_GetRef();
		Entry.Name = Ref.Name;
		Entry.bOptional = Ref.bOptional;
		Entry.bEnabled = Ref.bEnabled;
		Entry.bActivate = Ref.bActivate;
	}
	return Result;
}

TArray<FString> UPluginToolset59::GetPluginDependents(const FString& PluginName)
{
	if (!IPluginManager::Get().FindPlugin(PluginName).IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return {};
	}

	TArray<FString> Result;
	for (const TSharedRef<IPlugin>& Candidate : IPluginManager::Get().GetDiscoveredPlugins())
	{
		for (const FPluginReferenceDescriptor& Ref : Candidate->GetDescriptor().Plugins)
		{
			if (Ref.Name == PluginName)
			{
				Result.Add(Candidate->GetName());
				break;
			}
		}
	}
	Result.Sort();
	return Result;
}

bool UPluginToolset59::IsEnabled(const FString& PluginName)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return false;
	}
	return Plugin->IsEnabled();
}

FString UPluginToolset59::GetPluginForAsset(const FString& AssetPath)
{
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPluginsWithContent())
	{
		const FString MountedPath =	Plugin->GetMountedAssetPath();
		if (!MountedPath.IsEmpty() && AssetPath.StartsWith(MountedPath))
		{
			return Plugin->GetName();
		}
	}
	UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("No plugin contains asset: %s"),*AssetPath));
	return FString();
}

// --- Plugin Templates  ---

TArray<FPluginTemplateDescriptionToolsetInfo59> UPluginToolset59::GetPluginTemplateDescriptions()
{
	TArray<FPluginTemplateDescriptionToolsetInfo59> ToolsetResult;
	if (IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
	{
		const IPluginsEditorFeature& PluginEditor = 
			IModularFeatures::Get().GetModularFeature<IPluginsEditorFeature>(EditorFeatures::PluginsEditor);
		const TArray<TSharedRef<FPluginTemplateDescription>>& DefaultDescriptions = PluginEditor.GetDefaultPluginTemplates();
		const TArray<TSharedRef<FPluginTemplateDescription>>& AddedDescriptions = PluginEditor.GetAddedPluginTemplates();

		ToolsetResult.Reserve(DefaultDescriptions.Num() + AddedDescriptions.Num());

		for (const TSharedRef<FPluginTemplateDescription>& Description : DefaultDescriptions)
		{
			ToolsetResult.Emplace(UE::PluginToolset::Private::ToToolsetTemplateDescription(*Description));
		}
		for (const TSharedRef<FPluginTemplateDescription>& Description : AddedDescriptions)
		{
			ToolsetResult.Emplace(UE::PluginToolset::Private::ToToolsetTemplateDescription(*Description));
		}
	}

	return ToolsetResult;
}

TSharedPtr<FPluginTemplateDescription> UPluginToolset59::FindPluginTemplateDescriptionForToolsetInfo(
	const FPluginTemplateDescriptionToolsetInfo59& PluginTemplateInfo)
{
	if (IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
	{
		const IPluginsEditorFeature& PluginEditor = 
			IModularFeatures::Get().GetModularFeature<IPluginsEditorFeature>(EditorFeatures::PluginsEditor);
		const TArray<TSharedRef<FPluginTemplateDescription>>& DefaultDescriptions = PluginEditor.GetDefaultPluginTemplates();
		for (const TSharedRef<FPluginTemplateDescription>& Description : DefaultDescriptions)
		{
			if (UE::PluginToolset::Private::ToolsetTemplateDescriptionMatches(PluginTemplateInfo, *Description))
			{
				return Description;
			}
		}
		const TArray<TSharedRef<FPluginTemplateDescription>>& AddedDescriptions = PluginEditor.GetAddedPluginTemplates();
		for (const TSharedRef<FPluginTemplateDescription>& Description : AddedDescriptions)
		{
			if (UE::PluginToolset::Private::ToolsetTemplateDescriptionMatches(PluginTemplateInfo, *Description))
			{
				return Description;
			}
		}
	}
	return TSharedPtr<FPluginTemplateDescription>();
}

// --- Plugin Creation ---

FString UPluginToolset59::GeneratePluginFolderPath(FPluginTemplateDescription& PluginTemplate, bool bPlaceInEngine)
{
	FString PluginFolderPath = bPlaceInEngine ?
		IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*FPaths::EnginePluginsDir()) :
		IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*FPaths::ProjectPluginsDir());
	FPaths::MakePlatformFilename(PluginFolderPath);
	PluginTemplate.UpdatePathWhenTemplateSelected(PluginFolderPath);

	return PluginFolderPath;
}

bool UPluginToolset59::ValidateNewPluginNameAndLocationInternal(
	const FString& PluginName,
	const FString& RelativePluginLocation,
	const bool bPlaceInEngine,
	FPluginTemplateDescription& PluginTemplate,
	FString& OutAbsolutePluginLocation)
{
	if (bPlaceInEngine && !PluginTemplate.bCanBePlacedInEngine)
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin template %s cannot be placed in engine as requested."), *PluginTemplate.Name.ToString()));
		return false;
	}

	OutAbsolutePluginLocation = GeneratePluginFolderPath(PluginTemplate, bPlaceInEngine) / RelativePluginLocation;

	FText FailReason;
	if (!FPluginUtils::ValidateNewPluginNameAndLocation(PluginName, OutAbsolutePluginLocation, &FailReason))
	{
		UE::PluginToolset::Private::RaiseError(FailReason.ToString());
		return false;
	}
	const FString PluginFilePath = FPluginUtils::GetPluginFilePath(OutAbsolutePluginLocation, PluginName);

	if (!PluginTemplate.ValidatePathForPlugin(PluginFilePath, FailReason))
	{
		UE::PluginToolset::Private::RaiseError(FailReason.ToString());
		return false;
	}
	return true;
}

bool UPluginToolset59::IsPluginCreationAllowed()
{
	bool bCreateEnabled = true;
	GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanCreatePluginsFromBrowser"), bCreateEnabled, GEditorIni);
	if (!bCreateEnabled)
	{
		UE::PluginToolset::Private::RaiseError(TEXT("Plugin creation is disabled in Editor Settings."));
		return false;
	}
	return true;
}

bool UPluginToolset59::ValidateNewPluginNameAndLocation(
	const FString& PluginName,
	const FString& RelativePluginLocation,
	const bool bPlaceInEngine,
	const FPluginTemplateDescriptionToolsetInfo59& TemplateInfo)
{
	TSharedPtr<FPluginTemplateDescription> Template = FindPluginTemplateDescriptionForToolsetInfo(TemplateInfo);
	if (!Template.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(TEXT("Could not find a plugin template matching the provided TemplateInfo."));
		return false;
	}

	FString AbsolutePluginLocationUnused;
	return ValidateNewPluginNameAndLocationInternal(PluginName, RelativePluginLocation,
		bPlaceInEngine, *Template, AbsolutePluginLocationUnused);
}

FString UPluginToolset59::CreatePlugin(
	const FString& PluginName,
#if UE59_COMPAT // see PluginToolset.h
	const TOptional<FString>& PluginVersePath,
#else
	const TOptional<UE::Core::FVersePath>& PluginVersePath,
#endif
	const FString& RelativePluginLocation,
	const bool bPlaceInEngine,
	const FPluginTemplateDescriptionToolsetInfo59& TemplateInfo,
	const FString& Description,
	const FString& PythonScriptAdditionalArgs)
{
	if (!IsPluginCreationAllowed())
	{
		return FString();
	}
	TSharedPtr<FPluginTemplateDescription> Template = FindPluginTemplateDescriptionForToolsetInfo(TemplateInfo);
	if (!Template.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(TEXT("Could not find a plugin template matching the provided TemplateInfo."));
		return FString();
	}

	FString AbsolutePluginLocation;
	if (!ValidateNewPluginNameAndLocationInternal(PluginName, RelativePluginLocation,
		bPlaceInEngine, *Template, AbsolutePluginLocation))
	{
		return FString();
	}

	FPluginUtils::FNewPluginParamsWithDescriptor CreationParams;
	CreationParams.TemplateFolders.Add(Template->OnDiskPath);
	CreationParams.Descriptor.bCanContainContent = Template->bCanContainContent;
	CreationParams.Descriptor.FriendlyName = PluginName;
	CreationParams.Descriptor.Version = 1;
	CreationParams.Descriptor.VersionName = TEXT("1.0");
	CreationParams.Descriptor.Category = TEXT("Other");
	CreationParams.Descriptor.Description = Description;
	CreationParams.PostCreatePythonScriptPath = Template->PostCreatePythonScriptPath;
	CreationParams.PostCreatePythonScriptArguments = Template->PostCreatePythonScriptArguments;
	CreationParams.PostCreatePythonScriptAdditionalArguments = PythonScriptAdditionalArgs;

	// An explicit Verse path (including an empty one) overrides the default, which (matching the New Plugin wizard) is the root Verse path
	// for the plugin's location (engine or project) followed by the plugin name, when that location has a root Verse path
	FString NewVersePath;
	if (PluginVersePath.IsSet())
	{
#if UE59_COMPAT
		NewVersePath = *PluginVersePath;
#else
		NewVersePath = PluginVersePath->ToString();
#endif
	}
	else
	{
		FString RootVersePath;
		GConfig->GetString(TEXT("PluginDescriptorSettings"), bPlaceInEngine ? TEXT("EngineRootVersePath") : TEXT("ProjectRootVersePath"), RootVersePath, GEditorIni);
		if (!RootVersePath.IsEmpty())
		{
			NewVersePath = RootVersePath / PluginName;
		}
	}

	if (!NewVersePath.IsEmpty())
	{
		FText VersePathError;
		if (!UE::Core::FVersePath::IsValidFullPath(*NewVersePath, &VersePathError))
		{
			UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Invalid Verse path '%s': %s"), *NewVersePath, *VersePathError.ToString()));
			return FString();
		}

		CreationParams.Descriptor.VersePath = NewVersePath;
	}

	Template->CustomizeDescriptorBeforeCreation(CreationParams.Descriptor);

	FText FailReason;
	FPluginUtils::FLoadPluginParams LoadParams;
	LoadParams.bEnablePluginInProject = true;
	LoadParams.bUpdateProjectPluginSearchPath = true;
	LoadParams.OutFailReason = &FailReason;

	TSharedPtr<IPlugin> NewPlugin = FPluginUtils::CreateAndLoadNewPlugin(
		PluginName, AbsolutePluginLocation, CreationParams, LoadParams);
	if (!NewPlugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FailReason.ToString());
		return FString();
	}

	FText TemplateError;
	Template->OnPluginCreated(NewPlugin, TemplateError); // UE 5.9 has UE6's overload with the error out-parameter
	if (!TemplateError.IsEmpty())
	{
		UE::PluginToolset::Private::RaiseError(TemplateError.ToString());
	}
	return NewPlugin->GetDescriptorFileName();
}

// --- Plugin Management ---

bool UPluginToolset59::IsPluginModificationAllowed()
{
	bool bValue = true;
	GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanModifyPluginsFromBrowser"), bValue, GEditorIni);
	if (!bValue)
	{
		UE::PluginToolset::Private::RaiseError(TEXT("Plugin modification is disabled in Editor Settings."));
		return false;
	}
	return true;
}

bool UPluginToolset59::SetPluginEnabled(const FString& PluginName, bool bEnabled)
{
	if (!IsPluginModificationAllowed())
	{
		return false;
	}
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return false;
	}

	IProjectManager& ProjectManager = IProjectManager::Get();

	// The project's own entry for the plugin, if it has one, so a failed save can put it back.
	TOptional<bool> PreviousReferenceEnabled;
	if (const FProjectDescriptor* Project = ProjectManager.GetCurrentProject())
	{
		if (const FPluginReferenceDescriptor* Reference = Project->Plugins.FindByPredicate(
			[&PluginName](const FPluginReferenceDescriptor& Entry) { return Entry.Name == PluginName; }))
		{
			PreviousReferenceEnabled = Reference->bEnabled;
		}

		// IProjectManager::SetPluginEnabled can drop or rewrite the reference (removing it when it matches the
		// default, refreshing its metadata), and there is no way to put the old one back. So refuse up front when
		// the project file can't be written, rather than relying on the rollback below.
		const bool bEnabledInProject = PreviousReferenceEnabled.IsSet()
			? PreviousReferenceEnabled.GetValue()
			: Plugin->IsEnabledByDefault(!Project->bDisableEnginePluginsByDefault);
		if (bEnabledInProject != bEnabled && !ProjectManager.IsSuppressingProjectFileWrite())
		{
			const FString ProjectFile = FPaths::GetProjectFilePath();
			UE::PluginToolset::Private::CheckOutFileIfPossible(ProjectFile);
			if (IFileManager::Get().IsReadOnly(*ProjectFile))
			{
				UE::PluginToolset::Private::RaiseError(FString::Printf(
					TEXT("Plugin '%s' was not changed because the project file is read-only and could not be checked out: %s"),
					*PluginName, *FPaths::ConvertRelativePathToFull(ProjectFile)));
				return false;
			}
		}
	}

	FText FailReason;
	if (!ProjectManager.SetPluginEnabled(PluginName, bEnabled, FailReason))
	{
		UE::PluginToolset::Private::RaiseError(FailReason.ToString());
		return false;
	}

	if (ProjectManager.IsCurrentProjectDirty() && !ProjectManager.IsSuppressingProjectFileWrite())
	{
		UE::PluginToolset::Private::CheckOutFileIfPossible(FPaths::GetProjectFilePath());
		if (!ProjectManager.SaveCurrentProjectToDisk(FailReason))
		{
			// Only the enabled state can be put back, so any other fields the reference had are lost here.
			FText RestoreFailReason;
			if (PreviousReferenceEnabled.IsSet())
			{
				ProjectManager.SetPluginEnabled(PluginName, PreviousReferenceEnabled.GetValue(), RestoreFailReason);
			}
			else
			{
				ProjectManager.RemovePluginReference(PluginName, RestoreFailReason);
			}
			UE::PluginToolset::Private::RaiseError(FString::Printf(
				TEXT("Plugin '%s' was not changed because the project file could not be saved: %s"),
				*PluginName, *FailReason.ToString()));
			return false;
		}
	}

	return Plugin->IsEnabled() != bEnabled;
}

// --- Plugin Descriptor Editing ---

FPluginDescriptorToolsetInfo59 UPluginToolset59::GetPluginDescriptor(const FString& PluginName)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return FPluginDescriptorToolsetInfo59();
	}

	const FPluginDescriptor& Desc = Plugin->GetDescriptor();

	FPluginDescriptorToolsetInfo59 Info;
	Info.FriendlyName = Desc.FriendlyName;
	Info.Description = Desc.Description;
	Info.Category = Desc.Category;
	Info.VersionName = Desc.VersionName;
	Info.Version = Desc.Version;
	Info.CreatedBy = Desc.CreatedBy;
	Info.CreatedByURL = Desc.CreatedByURL;
	Info.DocsURL = Desc.DocsURL;
	Info.MarketplaceURL = Desc.MarketplaceURL;
	Info.SupportURL = Desc.SupportURL;
	Info.bCanContainContent = Desc.bCanContainContent;
	Info.bIsBetaVersion = Desc.bIsBetaVersion;
	Info.bIsExperimentalVersion = Desc.bIsExperimentalVersion;
	Info.bIsSealed = Desc.bIsSealed;
	return Info;
}

void UPluginToolset59::UpdatePluginDescriptor(const FString& PluginName, const FPluginDescriptorToolsetInfo59& NewDescriptor)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return;
	}

	FPluginDescriptor UpdatedDescriptor = Plugin->GetDescriptor();
	UpdatedDescriptor.FriendlyName = NewDescriptor.FriendlyName;
	UpdatedDescriptor.Description = NewDescriptor.Description;
	UpdatedDescriptor.Category = NewDescriptor.Category;
	UpdatedDescriptor.VersionName = NewDescriptor.VersionName;
	UpdatedDescriptor.Version = NewDescriptor.Version;
	UpdatedDescriptor.CreatedBy = NewDescriptor.CreatedBy;
	UpdatedDescriptor.CreatedByURL = NewDescriptor.CreatedByURL;
	UpdatedDescriptor.DocsURL = NewDescriptor.DocsURL;
	UpdatedDescriptor.MarketplaceURL = NewDescriptor.MarketplaceURL;
	UpdatedDescriptor.SupportURL = NewDescriptor.SupportURL;
	UpdatedDescriptor.bCanContainContent = NewDescriptor.bCanContainContent;
	UpdatedDescriptor.bIsBetaVersion = NewDescriptor.bIsBetaVersion;
	UpdatedDescriptor.bIsExperimentalVersion = NewDescriptor.bIsExperimentalVersion;
	UpdatedDescriptor.bIsSealed = NewDescriptor.bIsSealed;

	UE::PluginToolset::Private::UpdateDescriptorInternal(*Plugin, UpdatedDescriptor);
}

void UPluginToolset59::AddPluginDependency(const FString& PluginName, const FString& DependencyName, bool bOptional, bool bEnabled, bool bActivate)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return;
	}

	FPluginDescriptor UpdatedDescriptor = Plugin->GetDescriptor();

	for (const FPluginReferenceDescriptor& Existing : UpdatedDescriptor.Plugins)
	{
		if (Existing.Name == DependencyName)
		{
			if (Existing.bOptional == bOptional && Existing.bEnabled == bEnabled && Existing.bActivate == bActivate)
			{
				return;
			}
			UE::PluginToolset::Private::RaiseError(FString::Printf(
				TEXT("Dependency '%s' already exists with different settings in plugin '%s'."),
				*DependencyName, *PluginName));
			return;
		}
	}

	FPluginReferenceDescriptor& NewDep = UpdatedDescriptor.Plugins.AddDefaulted_GetRef();
	NewDep.Name = DependencyName;
	NewDep.bOptional = bOptional;
	NewDep.bEnabled = bEnabled;
	NewDep.bActivate = bActivate;

	UE::PluginToolset::Private::UpdateDescriptorInternal(*Plugin, UpdatedDescriptor);
}

void UPluginToolset59::UpdatePluginDependency(const FString& PluginName, const FString& DependencyName,
	TOptional<bool> bOptional, TOptional<bool> bEnabled, TOptional<bool> bActivate)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return;
	}
	if (!bOptional.IsSet() && !bEnabled.IsSet() && !bActivate.IsSet())
	{
		UE::PluginToolset::Private::RaiseError(TEXT("Nothing to change: set at least one of bOptional, bEnabled or bActivate."));
		return;
	}

	FPluginDescriptor UpdatedDescriptor = Plugin->GetDescriptor();
	FPluginReferenceDescriptor* Dependency = UpdatedDescriptor.Plugins.FindByPredicate(
		[&DependencyName](const FPluginReferenceDescriptor& Ref) { return Ref.Name == DependencyName; });
	if (!Dependency)
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(
			TEXT("Dependency '%s' not found in plugin '%s'."), *DependencyName, *PluginName));
		return;
	}

	Dependency->bOptional = bOptional.Get(Dependency->bOptional);
	Dependency->bEnabled = bEnabled.Get(Dependency->bEnabled);
	Dependency->bActivate = bActivate.Get(Dependency->bActivate);

	UE::PluginToolset::Private::UpdateDescriptorInternal(*Plugin, UpdatedDescriptor);
}

void UPluginToolset59::RemovePluginDependency(const FString& PluginName, const FString& DependencyName)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return;
	}

	FPluginDescriptor UpdatedDescriptor = Plugin->GetDescriptor();

	const int32 RemovedCount = UpdatedDescriptor.Plugins.RemoveAll(
		[&DependencyName](const FPluginReferenceDescriptor& Ref) { return Ref.Name == DependencyName; });

	if (RemovedCount == 0)
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(
			TEXT("Dependency '%s' not found in plugin '%s'."), *DependencyName, *PluginName));
		return;
	}

	UE::PluginToolset::Private::UpdateDescriptorInternal(*Plugin, UpdatedDescriptor);
}

TArray<FPluginDisallowedToolsetInfo> UPluginToolset59::GetDisallowedPlugins(const FString& PluginName)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return {};
	}

	TArray<FPluginDisallowedToolsetInfo> Result;
	for (const FPluginDisallowedDescriptor& Disallowed : Plugin->GetDescriptor().DisallowedPlugins)
	{
		FPluginDisallowedToolsetInfo& Entry = Result.AddDefaulted_GetRef();
		Entry.Name = Disallowed.Name;
		Entry.Comment = Disallowed.Comment;
	}
	return Result;
}

void UPluginToolset59::AddDisallowedPlugin(const FString& PluginName, const FString& DisallowedPluginName, const FString& Comment)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return;
	}
	if (DisallowedPluginName.IsEmpty())
	{
		UE::PluginToolset::Private::RaiseError(TEXT("Disallowed plugin name cannot be empty."));
		return;
	}
	if (DisallowedPluginName == PluginName)
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin '%s' cannot disallow itself."), *PluginName));
		return;
	}

	FPluginDescriptor UpdatedDescriptor = Plugin->GetDescriptor();
	FPluginDisallowedDescriptor* Existing = UpdatedDescriptor.DisallowedPlugins.FindByPredicate(
		[&DisallowedPluginName](const FPluginDisallowedDescriptor& Disallowed) { return Disallowed.Name == DisallowedPluginName; });
	if (Existing)
	{
		Existing->Comment = Comment;
	}
	else
	{
		FPluginDisallowedDescriptor& NewEntry = UpdatedDescriptor.DisallowedPlugins.AddDefaulted_GetRef();
		NewEntry.Name = DisallowedPluginName;
		NewEntry.Comment = Comment;
	}

	UE::PluginToolset::Private::UpdateDescriptorInternal(*Plugin, UpdatedDescriptor);
}

void UPluginToolset59::RemoveDisallowedPlugin(const FString& PluginName, const FString& DisallowedPluginName)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(TEXT("Plugin could not be found: %s"), *PluginName));
		return;
	}

	FPluginDescriptor UpdatedDescriptor = Plugin->GetDescriptor();
	const int32 RemovedCount = UpdatedDescriptor.DisallowedPlugins.RemoveAll(
		[&DisallowedPluginName](const FPluginDisallowedDescriptor& Disallowed) { return Disallowed.Name == DisallowedPluginName; });
	if (RemovedCount == 0)
	{
		UE::PluginToolset::Private::RaiseError(FString::Printf(
			TEXT("Disallowed plugin '%s' not found in plugin '%s'."), *DisallowedPluginName, *PluginName));
		return;
	}

	UE::PluginToolset::Private::UpdateDescriptorInternal(*Plugin, UpdatedDescriptor);
}

// --- Project Plugin Directories ---

TArray<FString> UPluginToolset59::GetAdditionalPluginDirectories()
{
	return IProjectManager::Get().GetAdditionalPluginDirectories();
}

void UPluginToolset59::AddAdditionalPluginDirectory(const FString& Directory)
{
	UE::PluginToolset::Private::UpdateAdditionalPluginDirectory(Directory, /*bAdd=*/true);
}

void UPluginToolset59::RemoveAdditionalPluginDirectory(const FString& Directory)
{
	UE::PluginToolset::Private::UpdateAdditionalPluginDirectory(Directory, /*bAdd=*/false);
}

bool UPluginToolset59::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}
