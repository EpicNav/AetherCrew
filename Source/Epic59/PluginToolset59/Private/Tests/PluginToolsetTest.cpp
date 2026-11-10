// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Features/EditorFeatures.h"
#include "Features/IPluginsEditorFeature.h"
#include "Features/IModularFeatures.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PluginDescriptor.h"
#include "ProjectDescriptor.h"
#include "PluginReferenceDescriptor.h"
#include "SourceControlHelpers.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"
#include "PluginUtils.h"

#include "PluginToolset59.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
const EAutomationTestFlags TestFlags =
	EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter |
	EAutomationTestFlags::CriticalPriority;


// Test plugin template (mostly exists so it can be marked as explicitly loaded)
struct FPluginToolsetTestPluginTemplate : public FPluginTemplateDescription
{
	FPluginToolsetTestPluginTemplate()
		: FPluginTemplateDescription(FText::FromString(TEXT("PluginToolsetTestPluginTemplate")),
			FText::FromString(TEXT("Plugin template for testing creating plugins")),
			// UE 5.8: the template ships with this port, not with 5.8's PluginToolset plugin.
			IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"))->GetBaseDir() / TEXT("Source/Epic59/PluginToolset59/TestPluginTemplate"),
			false, EHostType::Editor)
	{
	}

	virtual void CustomizeDescriptorBeforeCreation(FPluginDescriptor& Descriptor) override
	{
		Descriptor.bExplicitlyLoaded = true;
	}
};

// Snapshots the project descriptor and, on destruction, saves any pending changes to a throwaway project file and
// reloads the snapshot, so a test leaves neither changes nor unsaved changes behind. With ERedirect::ForScope the
// project file path points at the throwaway file for the whole scope so the test's own saves land there too; plugin
// discovery and registration go by that path, so tests that create a plugin use ERedirect::AtCleanup. Callers should
// skip when the project already has unsaved changes, since only the descriptor comes back, not that flag.
class FScopedDisposableProject
{
public:
	UE_NONCOPYABLE(FScopedDisposableProject);

	enum class ERedirect
	{
		ForScope,
		AtCleanup,
	};

	FScopedDisposableProject(FAutomationTestBase& InTest, ERedirect Redirect)
		: Test(InTest)
		, OriginalProjectFilename(FPaths::GetProjectFilePath())
		, Directory(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::RootDir(), TEXT(".sandbox"), TEXT("PluginToolset"), FGuid::NewGuid().ToString())))
		, ProjectFilename(FPaths::Combine(Directory, TEXT("Persistence.uproject")))
		, SnapshotFilename(FPaths::Combine(Directory, TEXT("Original.uproject")))
	{
		const FProjectDescriptor* CurrentProject = IProjectManager::Get().GetCurrentProject();
		if (!Test.TestNotNull(TEXT("project loaded"), CurrentProject)
			|| !Test.TestTrue(TEXT("test directory created"), IFileManager::Get().MakeDirectory(*Directory, true)))
		{
			return;
		}
		bDirectoryCreated = true;
		FText FailureReason;
		FProjectDescriptor Snapshot = *CurrentProject;
		if (!Test.TestTrue(TEXT("snapshot saves"), Snapshot.Save(SnapshotFilename, FailureReason)))
		{
			return;
		}
		bSnapshotSaved = true;
		if (Redirect == ERedirect::ForScope)
		{
			FPaths::SetProjectFilePath(ProjectFilename);
		}
	}

	~FScopedDisposableProject()
	{
		if (bSnapshotSaved)
		{
			IProjectManager& ProjectManager = IProjectManager::Get();
			FPaths::SetProjectFilePath(ProjectFilename);
			if (ProjectManager.IsCurrentProjectDirty())
			{
				FText FailureReason;
				Test.TestTrue(TEXT("test project changes saved during cleanup"), ProjectManager.SaveCurrentProjectToDisk(FailureReason));
			}
			Test.TestTrue(TEXT("original project descriptor restored"), ProjectManager.LoadProjectFile(SnapshotFilename));
			FPaths::SetProjectFilePath(OriginalProjectFilename);
		}
		if (bDirectoryCreated)
		{
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
		}
	}

	bool IsValid() const
	{
		return bSnapshotSaved;
	}

	const FString& GetDirectory() const
	{
		return Directory;
	}

	const FString& GetProjectFilename() const
	{
		return ProjectFilename;
	}

private:
	FAutomationTestBase& Test;
	const FString OriginalProjectFilename;
	const FString Directory;
	const FString ProjectFilename;
	const FString SnapshotFilename;
	bool bDirectoryCreated = false;
	bool bSnapshotSaved = false;
};
}

BEGIN_DEFINE_SPEC(
	FPluginToolsetSpec59,
	TEXT("AIAssistant.Toolsets.PluginToolset"),
	TestFlags)

// Ensure no blueprint script exceptions were caught and clear the handler.
void ExpectNoException()
{
	check(ExceptionHandler.IsValid());
	TestEqual(TEXT("Error"), *ExceptionHandler->GetException(), TEXT(""));
	ExceptionHandler.Reset();
}

// Ensure blueprint script exception was caught and contains the error message, then clear the
// handler.
void ExpectExceptionContains(const FString& ErrorMessage)
{
	check(ExceptionHandler.IsValid());
	TestTrue(*FString::Printf(TEXT("Error contains '%s'"), *ErrorMessage),
		ExceptionHandler->GetException().Contains(ErrorMessage));
	ExceptionHandler.Reset();
}

// Ensure blueprint script exception was caught and clear the handler.
void ExpectException()
{
	check(ExceptionHandler.IsValid());
	TestNotEqual(TEXT("Has Error"), *ExceptionHandler->GetException(), TEXT(""));
	ExceptionHandler.Reset();
}

// Add a test template so at least one template exists
void AddTestTemplateDescription()
{
	if (IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
	{
		if (!TestTemplate.IsValid())
		{
			TestTemplate = MakeShareable(new FPluginToolsetTestPluginTemplate());
		}
		IPluginsEditorFeature& PluginEditor =
			IModularFeatures::Get().GetModularFeature<IPluginsEditorFeature>(EditorFeatures::PluginsEditor);
		PluginEditor.RegisterPluginTemplate(TestTemplate.ToSharedRef());
	}
}

void RemoveTestTemplateDescription()
{
	if (IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
	{
		if (ensure(TestTemplate.IsValid()))
		{
			IPluginsEditorFeature& PluginEditor =
				IModularFeatures::Get().GetModularFeature<IPluginsEditorFeature>(EditorFeatures::PluginsEditor);
				PluginEditor.UnregisterPluginTemplate(TestTemplate.ToSharedRef());
		}
	}
}

// Returns false when the descriptor file is read-only and source control is not available
// to check it out -- typical of unattended CI runs (e.g. Horde) where the workspace is
// synced read-only and no Perforce client is configured. Used to skip the descriptor
// write-path tests cleanly in that environment.
bool CanModifyPluginDescriptor(const TSharedRef<IPlugin>& Plugin)
{
	if (!IFileManager::Get().IsReadOnly(*Plugin->GetDescriptorFileName()))
	{
		return true;
	}
	return USourceControlHelpers::IsAvailable();
}

private:
	TUniquePtr<UE::ToolsetRegistry::FToolCallExceptionHandler> ExceptionHandler;
	TSharedPtr<FPluginTemplateDescription> TestTemplate;

END_DEFINE_SPEC(FPluginToolsetSpec59)

void FPluginToolsetSpec59::Define()
{
	BeforeEach([this]()
	{
		ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
	});

	AfterEach([this]()
	{
		if (ExceptionHandler.IsValid())
		{
			ExpectNoException();
		}
	});


	Describe(TEXT("ListEnabledPlugins"), [this]()
	{
		It(TEXT("returns a sorted list"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UPluginToolset59::ListEnabledPlugins();
				for (int32 i = 1; i < Names.Num(); ++i)
				{
					TestTrue(TEXT("Sorted"), Names[i - 1] <= Names[i]);
				}
			});
			ExpectNoException();
		});

		It(TEXT("Contains ToolsetRegistry"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UPluginToolset59::ListEnabledPlugins();
				TestTrue(TEXT("Contains ToolsetRegistry"), Names.Contains(TEXT("ToolsetRegistry")));
			});
			ExpectNoException();
		});

		It(TEXT("all listed plugins are enabled according to the plugin manager"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UPluginToolset59::ListEnabledPlugins();
				for (const FString& Name : Names)
				{
					TestTrue(TEXT("Found plugin with name"), IPluginManager::Get().FindEnabledPlugin(Name).IsValid());
				}
			});
			ExpectNoException();
		});
	});

	Describe(TEXT("ListDiscoveredPlugins"), [this]()
	{
		It(TEXT("returns a sorted list"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UPluginToolset59::ListDiscoveredPlugins();
				for (int32 i = 1; i < Names.Num(); ++i)
				{
					TestTrue(TEXT("Sorted"), Names[i - 1] <= Names[i]);
				}
			});
			ExpectNoException();
		});

		It(TEXT("Contains ToolsetRegistry"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UPluginToolset59::ListDiscoveredPlugins();
				TestTrue(TEXT("Contains ToolsetRegistry"), Names.Contains(TEXT("ToolsetRegistry")));
			});
			ExpectNoException();
		});

		It(TEXT("all listed plugins are discovered according to the plugin manager"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UPluginToolset59::ListDiscoveredPlugins();
				for (const FString& Name : Names)
				{
					TestTrue(TEXT("Found plugin with name"), IPluginManager::Get().FindPlugin(Name).IsValid());
				}
			});
			ExpectNoException();
		});
	});

	Describe(TEXT("GetPluginInfo"), [this]()
	{
		It(TEXT("Returns metadata for enabled plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FPluginToolsetInfo59 Info = UPluginToolset59::GetPluginInfo(TEXT("ToolsetRegistry"));
				TestFalse(TEXT("Has base dir"), Info.BaseDir.IsEmpty());
			});
			ExpectNoException();
		});

		It(TEXT("Works for code-only plugins"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FPluginToolsetInfo59 Info = UPluginToolset59::GetPluginInfo(TEXT("PythonScriptPlugin"));
				TestFalse(TEXT("Has base dir"), Info.BaseDir.IsEmpty());
			});
			ExpectNoException();
		});

		It(TEXT("Raises error for nonexistent plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FPluginToolsetInfo59 Info = UPluginToolset59::GetPluginInfo(TEXT("NonexistentXYZ12345"));
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});
	});

	Describe(TEXT("IsEnabled"), [this]()
	{
		It(TEXT("Returns true for ToolsetRegistry"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TestTrue(TEXT("Enabled"), UPluginToolset59::IsEnabled(TEXT("ToolsetRegistry")));
			});
			ExpectNoException();
		});

		It(TEXT("matches plugin's enabled status"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
				{
					TestEqual(TEXT("Enabled"), Plugin->IsEnabled(), UPluginToolset59::IsEnabled(Plugin->GetName()));
				}
			});
			ExpectNoException();
		});

		It(TEXT("raise an exception for non-existent plugins"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UPluginToolset59::IsEnabled(TEXT("NonexistentPluginXYZ12345"));
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});
	});

	Describe(TEXT("GetPluginForAsset"), [this]()
	{
		It(TEXT("Returns plugin name for known asset"),
			[this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				FString Result = UPluginToolset59::GetPluginForAsset(TEXT("/ToolsetRegistry/"));
				TestEqual(TEXT("Plugin name"), Result, FString(TEXT("ToolsetRegistry")));
			});
			ExpectNoException();
		});

		It(TEXT("Raises error for unknown asset"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				FString Result = UPluginToolset59::GetPluginForAsset(TEXT("/Game/NonexistentAsset12345"));
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});
	});



	Describe(TEXT("PluginTemplateDescriptions"), [this]()
	{
		It(TEXT("Returns a list of plugin template descriptions"),
			[this]()
		{
			if (!IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
			{
				AddInfo(TEXT("PluginsEditor modular feature not available. Skipping test."));
				return;
			}
			// Ensure at least one template description is available.
			AddTestTemplateDescription();
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TArray<FPluginTemplateDescriptionToolsetInfo59> Result = UPluginToolset59::GetPluginTemplateDescriptions();
				TestTrue(TEXT("Non-empty result"), !Result.IsEmpty());
			});
			ExpectNoException();
			RemoveTestTemplateDescription();
		});

		It(TEXT("all results can be found by FindPluginTemplateDescriptionForToolsetInfo"), [this]()
		{
			if (!IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
			{
				AddInfo(TEXT("PluginsEditor modular feature not available. Skipping test."));
				return;
			}
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FPluginTemplateDescriptionToolsetInfo59> Infos = UPluginToolset59::GetPluginTemplateDescriptions();
				for (const FPluginTemplateDescriptionToolsetInfo59& Info : Infos)
				{
					TestTrue(TEXT("Found template"), UPluginToolset59::FindPluginTemplateDescriptionForToolsetInfo(Info).IsValid());
				}
			});
			ExpectNoException();
		});

		It(TEXT("finds a template from its OnDiskPath when the display text differs"), [this]()
		{
			if (!IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
			{
				AddInfo(TEXT("PluginsEditor modular feature not available. Skipping test."));
				return;
			}
			AddTestTemplateDescription();
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				FPluginTemplateDescriptionToolsetInfo59 Abbreviated;
				Abbreviated.OnDiskPath = TestTemplate->OnDiskPath;
				Abbreviated.Name = FText::FromString(TEXT("Abbreviated"));
				Abbreviated.Description = FText::FromString(TEXT("Abbreviated"));
				TestTrue(TEXT("Found template"),
					UPluginToolset59::FindPluginTemplateDescriptionForToolsetInfo(Abbreviated).IsValid());
			});
			ExpectNoException();
			RemoveTestTemplateDescription();
		});

		It(TEXT("empty info produces null result from FindPluginTemplateDescriptionForToolsetInfo"), [this]()
		{
			if (!IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
			{
				AddInfo(TEXT("PluginsEditor modular feature not available. Skipping test."));
				return;
			}
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TestFalse(TEXT("Null result"),
					UPluginToolset59::FindPluginTemplateDescriptionForToolsetInfo(FPluginTemplateDescriptionToolsetInfo59{}).IsValid());
			});
			ExpectNoException();
		});
	});

	Describe(TEXT("IsPluginCreationAllowed"), [this]()
	{
		It(TEXT("matches GEditorIni bCanCreatePluginsFromBrowser"), [this]()
		{
			bool bExpected = true;
			GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanCreatePluginsFromBrowser"), bExpected, GEditorIni);

			bool bResult = false;
			ExceptionHandler->CaptureErrorsIn([this, &bResult]()
			{
				bResult = UPluginToolset59::IsPluginCreationAllowed();
			});

			TestEqual(TEXT("Result matches config"), bResult, bExpected);
			if (bExpected)
			{
				ExpectNoException();
			}
			else
			{
				ExpectExceptionContains(TEXT("PluginToolset"));
			}
		});
	});

	Describe(TEXT("ValidateNewPluginNameAndLocation"), [this]()
	{
		It(TEXT("Raises error for unresolvable template"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TestFalse(TEXT("Valid"), UPluginToolset59::ValidateNewPluginNameAndLocation(
					TEXT("TestPluginXYZ12345Valid"), TEXT(""), false, FPluginTemplateDescriptionToolsetInfo59{}));
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});

		It(TEXT("Returns true for valid name and location"), [this]()
		{
			if (!IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
			{
				AddInfo(TEXT("PluginsEditor modular feature not available. Skipping test."));
				return;
			}
			// Ensure at least one template description is available.
			AddTestTemplateDescription();
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FPluginTemplateDescriptionToolsetInfo59> Infos = UPluginToolset59::GetPluginTemplateDescriptions();
				if (TestTrue(TEXT("Non-empty template descriptions."), !Infos.IsEmpty()))
				{
					TestTrue(TEXT("Valid"), UPluginToolset59::ValidateNewPluginNameAndLocation(
						TEXT("TestPluginXYZ12345Valid"), TEXT(""), false, Infos[0]));
				}
			});
			ExpectNoException();
			RemoveTestTemplateDescription();
		});

		It(TEXT("Raises error for taken plugin name"), [this]()
		{
			if (!IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
			{
				AddInfo(TEXT("PluginsEditor modular feature not available. Skipping test."));
				return;
			}

			// Ensure at least one template description is available.
			AddTestTemplateDescription();
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FPluginTemplateDescriptionToolsetInfo59> Infos = UPluginToolset59::GetPluginTemplateDescriptions();
				if (TestTrue(TEXT("Non-empty template descriptions."), !Infos.IsEmpty()))
				{
					TestFalse(TEXT("Valid"), UPluginToolset59::ValidateNewPluginNameAndLocation(
						TEXT("ToolsetRegistry"), TEXT(""), false, Infos[0]));
				}
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
			RemoveTestTemplateDescription();
		});

		It(TEXT("Raises error for empty name"), [this]()
		{
			if (!IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
			{
				AddInfo(TEXT("PluginsEditor modular feature not available. Skipping test."));
				return;
			}

			// Ensure at least one template description is available.
			AddTestTemplateDescription();
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FPluginTemplateDescriptionToolsetInfo59> Infos = UPluginToolset59::GetPluginTemplateDescriptions();
				if (TestTrue(TEXT("Non-empty template descriptions."), !Infos.IsEmpty()))
				{
					TestFalse(TEXT("Valid"), UPluginToolset59::ValidateNewPluginNameAndLocation(
						TEXT(""), TEXT(""), false, Infos[0]));
				}
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
			RemoveTestTemplateDescription();
		});
	});

	Describe(TEXT("CreatePlugin"), [this]()
	{
		It(TEXT("Raises error for taken plugin name"), [this]()
		{
			if (!IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
			{
				AddInfo(TEXT("PluginsEditor modular feature not available. Skipping test."));
				return;
			}

			// Ensure at least one template description is available.
			AddTestTemplateDescription();
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FPluginTemplateDescriptionToolsetInfo59> Infos = UPluginToolset59::GetPluginTemplateDescriptions();
				if (TestTrue(TEXT("Non-empty template descriptions."), !Infos.IsEmpty()))
				{
					const FString Result = UPluginToolset59::CreatePlugin(
						TEXT("ToolsetRegistry"), {}, TEXT(""), false, Infos[0], TEXT(""), TEXT(""));
					TestTrue(TEXT("Empty result"), Result.IsEmpty());
				}
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
			RemoveTestTemplateDescription();
		});

		It(TEXT("Raises error for unresolvable template"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FString Result = UPluginToolset59::CreatePlugin(
					TEXT("TestPluginXYZ12345Valid"), {}, TEXT(""), false,
					FPluginTemplateDescriptionToolsetInfo59{}, TEXT(""), TEXT(""));
				TestTrue(TEXT("Empty result"), Result.IsEmpty());
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});

		It(TEXT("Successfully creates a plugin"), [this]()
		{
			bool bCreationAllowed = true;
			GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanCreatePluginsFromBrowser"), bCreationAllowed, GEditorIni);
			if (!bCreationAllowed)
			{
				AddInfo(TEXT("Plugin creation is not allowed in this project. Skipping test."));
				ExceptionHandler.Reset();
				return;
			}
			if (!IModularFeatures::Get().IsModularFeatureAvailable(EditorFeatures::PluginsEditor))
			{
				AddInfo(TEXT("PluginsEditor modular feature not available. Skipping test."));
				return;
			}
			// Creating and cleaning up the plugin changes the project's plugin list in memory, which would otherwise
			// leave the project with unsaved changes for every spec that follows.
			if (IProjectManager::Get().IsCurrentProjectDirty())
			{
				AddInfo(TEXT("Plugin creation test needs a project without unsaved descriptor changes; skipping."));
				ExceptionHandler.Reset();
				return;
			}
			const FScopedDisposableProject DisposableProject(*this, FScopedDisposableProject::ERedirect::AtCleanup);
			if (!DisposableProject.IsValid())
			{
				return;
			}

			// Add test template to plugin editor
			AddTestTemplateDescription();

			// Create our info for our actual test.
			FPluginTemplateDescriptionToolsetInfo59 Info;
			Info.Name = TestTemplate->Name;
			Info.Description = TestTemplate->Description;
			Info.OnDiskPath = TestTemplate->OnDiskPath;
			Info.bCanBePlacedInEngine = false;
			Info.DefaultTemplateName = FText();

			const FString PluginName = TEXT("PluginToolsetTestXYZ12345");
			const FString PluginRelativeLocation = TEXT("Nested");
			const FString ExpectedPluginAbsoluteLocation = FPaths::ProjectPluginsDir() / PluginRelativeLocation;
			const FString ExpectedPluginPath = FPluginUtils::GetPluginFilePath(ExpectedPluginAbsoluteLocation, PluginName, /*bFullPath*/ true);
			FString PluginAbsoluteLocation, PluginPath;
			ExceptionHandler->CaptureErrorsIn([this, Info, &PluginName, &PluginRelativeLocation, &ExpectedPluginPath, &ExpectedPluginAbsoluteLocation, &PluginAbsoluteLocation, &PluginPath]()
			{
				PluginAbsoluteLocation = UPluginToolset59::GeneratePluginFolderPath(*TestTemplate, false) / PluginRelativeLocation;
				TestTrue(TEXT("Result plugin folder path matches"), FPaths::IsSamePath(ExpectedPluginAbsoluteLocation, PluginAbsoluteLocation));

				PluginPath = UPluginToolset59::CreatePlugin(PluginName, {}, PluginRelativeLocation, false, Info, TEXT("Test plugin"), TEXT(""));
				TestTrue(TEXT("Result path matches"),
					FPaths::IsSamePath(ExpectedPluginPath, PluginPath));
			});


			// Now clean up
			// Remove from project config before unloading so that RefreshPluginsList (called
			// internally by UnloadPlugin) doesn't re-enable the plugin when it re-discovers it.
			FText DisableReason;
			IProjectManager::Get().SetPluginEnabled(PluginName, false, DisableReason);

			FText UnloadFailReason;
			FPluginUtils::UnloadPlugin(PluginName, &UnloadFailReason);

			// RemoveFromPluginsList expects the .uplugin file path, not the plugin name.
			FText RemoveFailReason;
			TestTrue(TEXT("PluginRemoveSuccess"), IPluginManager::Get().RemoveFromPluginsList(PluginPath, &RemoveFailReason));

			if (USourceControlHelpers::IsAvailable())
			{
				// Revert the pending p4 add operations for all files in the plugin directory.
				TArray<FString> FilesToRevert;
				IFileManager::Get().FindFilesRecursive(FilesToRevert, *(PluginAbsoluteLocation / PluginName), TEXT("*"), true, false);
				if (FilesToRevert.Num() > 0)
				{
					USourceControlHelpers::RevertFiles(FilesToRevert);
				}
			}
			IFileManager::Get().DeleteDirectory(*(PluginAbsoluteLocation / PluginName), false, true);

			RemoveTestTemplateDescription();
			ExpectNoException();
		});
	});

	Describe(TEXT("IsPluginModificationAllowed"), [this]()
	{
		It(TEXT("matches GEditorIni bCanModifyPluginsFromBrowser"), [this]()
		{
			bool bExpected = true;
			GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanModifyPluginsFromBrowser"), bExpected, GEditorIni);

			bool bResult = false;
			ExceptionHandler->CaptureErrorsIn([this, &bResult]()
			{
				bResult = UPluginToolset59::IsPluginModificationAllowed();
			});

			TestEqual(TEXT("Result matches config"), bResult, bExpected);
			if (bExpected)
			{
				ExpectNoException();
			}
			else
			{
				ExpectExceptionContains(TEXT("PluginToolset"));
			}
		});
	});

	Describe(TEXT("GetPluginDependencies"), [this]()
	{
		It(TEXT("Returns dependency entries for a discovered plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				// PluginToolset declares ToolsetRegistry and PluginUtils as dependencies
				const TArray<FPluginDependencyToolsetInfo59> Deps =
					UPluginToolset59::GetPluginDependencies(TEXT("PluginToolset"));
				const bool bHasToolsetRegistry = Deps.ContainsByPredicate(
					[](const FPluginDependencyToolsetInfo59& D) { return D.Name == TEXT("ToolsetRegistry"); });
				TestTrue(TEXT("Contains ToolsetRegistry dependency"), bHasToolsetRegistry);
			});
			ExpectNoException();
		});

		It(TEXT("Raises error for nonexistent plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UPluginToolset59::GetPluginDependencies(TEXT("NonexistentXYZ12345"));
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});
	});

	Describe(TEXT("GetPluginDependents"), [this]()
	{
		It(TEXT("Returns plugins that depend on a given plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				// PluginToolset declares ToolsetRegistry as a dependency
				const TArray<FString> Dependents =
					UPluginToolset59::GetPluginDependents(TEXT("ToolsetRegistry"));
				TestTrue(TEXT("Contains PluginToolset"), Dependents.Contains(TEXT("PluginToolset")));
			});
			ExpectNoException();
		});

		It(TEXT("Returns a sorted array"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Dependents =
					UPluginToolset59::GetPluginDependents(TEXT("ToolsetRegistry"));
				for (int32 i = 1; i < Dependents.Num(); ++i)
				{
					TestTrue(TEXT("Sorted"), Dependents[i - 1] <= Dependents[i]);
				}
			});
			ExpectNoException();
		});

		It(TEXT("Raises error for nonexistent plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UPluginToolset59::GetPluginDependents(TEXT("NonexistentXYZ12345"));
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});
	});

	Describe(TEXT("GetPluginDescriptor"), [this]()
	{
		It(TEXT("Returns descriptor for a discovered plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FPluginDescriptorToolsetInfo59 Info = UPluginToolset59::GetPluginDescriptor(TEXT("PluginToolset"));
				TestFalse(TEXT("Has FriendlyName"), Info.FriendlyName.IsEmpty());
			});
			ExpectNoException();
		});

		It(TEXT("Descriptor fields match IPlugin::GetDescriptor()"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ToolsetRegistry"));
				if (!TestTrue(TEXT("Plugin found"), Plugin.IsValid()))
				{
					return;
				}
				const FPluginDescriptor& Desc = Plugin->GetDescriptor();
				const FPluginDescriptorToolsetInfo59 Info = UPluginToolset59::GetPluginDescriptor(TEXT("ToolsetRegistry"));
				TestEqual(TEXT("Description matches"), Info.Description, Desc.Description);
				TestEqual(TEXT("Version matches"), Info.Version, Desc.Version);
				TestEqual(TEXT("VersionName matches"), Info.VersionName, Desc.VersionName);
				TestEqual(TEXT("FriendlyName matches"), Info.FriendlyName, Desc.FriendlyName);
			});
			ExpectNoException();
		});

		It(TEXT("Raises error for nonexistent plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UPluginToolset59::GetPluginDescriptor(TEXT("NonexistentXYZ12345"));
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});
	});

	Describe(TEXT("UpdatePluginDescriptor"), [this]()
	{
		It(TEXT("Raises error for nonexistent plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UPluginToolset59::UpdatePluginDescriptor(TEXT("NonexistentXYZ12345"), FPluginDescriptorToolsetInfo59{});
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});

		It(TEXT("Round-trips a descriptor field change"), [this]()
		{
			if (FApp::IsEngineInstalled() || FApp::IsInstalled())
			{
				AddInfo(TEXT("Cannot edit non-mod plugins in an installed build. Skipping test."));
				ExceptionHandler.Reset();
				return;
			}

			// Round-trip against a throwaway descriptor under Saved/ - mutating a
			// real engine .uplugin fails on build machines where the sync is
			// read-only.
			const FString TestPluginName = TEXT("PluginToolsetDescriptorRoundTripTest");
			const FString TestPluginDir = FPaths::ProjectSavedDir() / TEXT("Temp") / TEXT("PluginToolsetTest");
			const FString TestDescriptorFileName = TestPluginDir / TestPluginName + TEXT(".uplugin");

			FText FailReason;
			const FPluginDescriptor TestDescriptor;
			if (!TestTrue(TEXT("Test descriptor written"), TestDescriptor.Save(TestDescriptorFileName, FailReason)))
			{
				ExceptionHandler.Reset();
				return;
			}
			if (!TestTrue(TEXT("Test plugin registered"), IPluginManager::Get().AddToPluginsList(TestDescriptorFileName, &FailReason)))
			{
				IFileManager::Get().DeleteDirectory(*TestPluginDir, /*RequireExists=*/false, /*Tree=*/true);
				ExceptionHandler.Reset();
				return;
			}

			FPluginDescriptorToolsetInfo59 Original;
			ExceptionHandler->CaptureErrorsIn([&TestPluginName, &Original]()
			{
				Original = UPluginToolset59::GetPluginDescriptor(TestPluginName);
			});
			ExpectNoException();

			FPluginDescriptorToolsetInfo59 Modified = Original;
			Modified.Description = TEXT("PluginToolsetTestDescriptionXYZ12345");

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([&TestPluginName, &Modified]()
			{
				UPluginToolset59::UpdatePluginDescriptor(TestPluginName, Modified);
			});
			ExpectNoException();

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([this, &TestPluginName]()
			{
				const FPluginDescriptorToolsetInfo59 Updated = UPluginToolset59::GetPluginDescriptor(TestPluginName);
				TestEqual(TEXT("Description updated"), Updated.Description, FString(TEXT("PluginToolsetTestDescriptionXYZ12345")));
			});
			ExpectNoException();

			// Restore
			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([&TestPluginName, &Original]()
			{
				UPluginToolset59::UpdatePluginDescriptor(TestPluginName, Original);
			});
			ExpectNoException();

			// Unregister and delete the throwaway plugin
			IPluginManager::Get().RemoveFromPluginsList(TestDescriptorFileName);
			IFileManager::Get().DeleteDirectory(*TestPluginDir, /*RequireExists=*/false, /*Tree=*/true);
		});

		It(TEXT("No-op when descriptor is unchanged"), [this]()
		{
			const TSharedPtr<IPlugin> PluginToolsetPlugin = IPluginManager::Get().FindPlugin(TEXT("PluginToolset"));
			if (PluginToolsetPlugin.IsValid())
			{
				if (FApp::IsEngineInstalled() && PluginToolsetPlugin->GetLoadedFrom() == EPluginLoadedFrom::Engine)
				{
					AddInfo(TEXT("PluginToolset is an engine plugin in an installed engine build. Skipping test."));
					ExceptionHandler.Reset();
					return;
				}
				if (FApp::IsInstalled() && PluginToolsetPlugin->GetType() != EPluginType::Mod)
				{
					AddInfo(TEXT("Cannot edit non-mod plugins in an installed build. Skipping test."));
					ExceptionHandler.Reset();
					return;
				}
			}

			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PluginToolset"));
			if (!TestTrue(TEXT("Plugin found"), Plugin.IsValid()))
			{
				ExceptionHandler.Reset();
				return;
			}

			const FString DescriptorFileName = Plugin->GetDescriptorFileName();
			const FDateTime MtimeBefore = IFileManager::Get().GetTimeStamp(*DescriptorFileName);

			FPluginDescriptorToolsetInfo59 Original;
			ExceptionHandler->CaptureErrorsIn([this, &Original]()
			{
				Original = UPluginToolset59::GetPluginDescriptor(TEXT("PluginToolset"));
			});
			ExpectNoException();

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([this, &Original]()
			{
				UPluginToolset59::UpdatePluginDescriptor(TEXT("PluginToolset"), Original);
			});
			ExpectNoException();

			const FDateTime MtimeAfter = IFileManager::Get().GetTimeStamp(*DescriptorFileName);
			TestEqual(TEXT("File not modified"), MtimeBefore, MtimeAfter);
		});
	});

	Describe(TEXT("AddPluginDependency"), [this]()
	{
		It(TEXT("Raises error for nonexistent plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UPluginToolset59::AddPluginDependency(TEXT("NonexistentXYZ12345"), TEXT("ToolsetRegistry"), false, true, false);
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});

	});

	Describe(TEXT("RemovePluginDependency"), [this]()
	{
		It(TEXT("Raises error for nonexistent plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UPluginToolset59::RemovePluginDependency(TEXT("NonexistentXYZ12345"), TEXT("SomeDep"));
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});

		It(TEXT("Raises error when dependency not found"), [this]()
		{
			const TSharedPtr<IPlugin> PluginToolsetPlugin = IPluginManager::Get().FindPlugin(TEXT("PluginToolset"));
			if (PluginToolsetPlugin.IsValid())
			{
				if (FApp::IsEngineInstalled() && PluginToolsetPlugin->GetLoadedFrom() == EPluginLoadedFrom::Engine)
				{
					AddInfo(TEXT("PluginToolset is an engine plugin in an installed engine build. Skipping test."));
					ExceptionHandler.Reset();
					return;
				}
				if (FApp::IsInstalled() && PluginToolsetPlugin->GetType() != EPluginType::Mod)
				{
					AddInfo(TEXT("Cannot edit non-mod plugins in an installed build. Skipping test."));
					ExceptionHandler.Reset();
					return;
				}
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UPluginToolset59::RemovePluginDependency(TEXT("PluginToolset"), TEXT("NonexistentDepXYZ12345"));
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});

		It(TEXT("Add/remove dependency round-trip"), [this]()
		{
			if (FApp::IsEngineInstalled() || FApp::IsInstalled())
			{
				AddInfo(TEXT("Cannot edit non-mod plugins in an installed build. Skipping test."));
				ExceptionHandler.Reset();
				return;
			}

			// Round-trip against a throwaway descriptor under Saved/ - mutating a
			// real engine .uplugin fails on build machines where the sync is
			// read-only.
			const FString TestPluginName = TEXT("PluginToolsetRoundTripTest");
			const FString TestPluginDir = FPaths::ProjectSavedDir() / TEXT("Temp") / TEXT("PluginToolsetTest");
			const FString TestDescriptorFileName = TestPluginDir / TestPluginName + TEXT(".uplugin");

			FText FailReason;
			const FPluginDescriptor TestDescriptor;
			if (!TestTrue(TEXT("Test descriptor written"), TestDescriptor.Save(TestDescriptorFileName, FailReason)))
			{
				ExceptionHandler.Reset();
				return;
			}
			if (!TestTrue(TEXT("Test plugin registered"), IPluginManager::Get().AddToPluginsList(TestDescriptorFileName, &FailReason)))
			{
				IFileManager::Get().DeleteDirectory(*TestPluginDir, /*RequireExists=*/false, /*Tree=*/true);
				ExceptionHandler.Reset();
				return;
			}

			// Add a dependency that is not already present
			ExceptionHandler->CaptureErrorsIn([&TestPluginName]()
			{
				UPluginToolset59::AddPluginDependency(TestPluginName, TEXT("DataflowCore"), true, false, false);
			});
			ExpectNoException();

			// Verify the dependency appears in the in-memory descriptor
			{
				const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TestPluginName);
				if (TestTrue(TEXT("Plugin found"), Plugin.IsValid()))
				{
					const bool bFound = Plugin->GetDescriptor().Plugins.ContainsByPredicate(
						[](const FPluginReferenceDescriptor& Ref) { return Ref.Name == TEXT("DataflowCore"); });
					TestTrue(TEXT("Dependency present"), bFound);
				}
			}

			// Remove it
			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([&TestPluginName]()
			{
				UPluginToolset59::RemovePluginDependency(TestPluginName, TEXT("DataflowCore"));
			});
			ExpectNoException();

			// Verify it is gone
			{
				const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TestPluginName);
				if (TestTrue(TEXT("Plugin found"), Plugin.IsValid()))
				{
					const bool bFound = Plugin->GetDescriptor().Plugins.ContainsByPredicate(
						[](const FPluginReferenceDescriptor& Ref) { return Ref.Name == TEXT("DataflowCore"); });
					TestFalse(TEXT("Dependency removed"), bFound);
				}
			}

			// Unregister and delete the throwaway plugin
			IPluginManager::Get().RemoveFromPluginsList(TestDescriptorFileName);
			IFileManager::Get().DeleteDirectory(*TestPluginDir, /*RequireExists=*/false, /*Tree=*/true);
		});
	});

	Describe(TEXT("UpdatePluginDependency"), [this]()
	{
		It(TEXT("Raises error for nonexistent plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UPluginToolset59::UpdatePluginDependency(TEXT("NonexistentXYZ12345"), TEXT("ToolsetRegistry"), false, true, false);
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});

		It(TEXT("Activate flag round-trip keeps fields the editor doesn't show"), [this]()
		{
			if (FApp::IsEngineInstalled() || FApp::IsInstalled())
			{
				AddInfo(TEXT("Cannot edit non-mod plugins in an installed build. Skipping test."));
				ExceptionHandler.Reset();
				return;
			}

			const FString TestPluginName = TEXT("PluginToolsetActivateTest");
			const FString TestPluginDir = FPaths::ProjectSavedDir() / TEXT("Temp") / TEXT("PluginToolsetTest") / FGuid::NewGuid().ToString();
			const FString TestDescriptorFileName = TestPluginDir / TestPluginName + TEXT(".uplugin");

			FText FailReason;
			FPluginDescriptor TestDescriptor;
			FPluginReferenceDescriptor& Restricted = TestDescriptor.Plugins.Emplace_GetRef(TEXT("DataflowCore"), true);
			Restricted.PlatformAllowList.Add(TEXT("Win64"));
			if (!TestTrue(TEXT("Test descriptor written"), TestDescriptor.Save(TestDescriptorFileName, FailReason)))
			{
				ExceptionHandler.Reset();
				return;
			}
			if (!TestTrue(TEXT("Test plugin registered"), IPluginManager::Get().AddToPluginsList(TestDescriptorFileName, &FailReason)))
			{
				IFileManager::Get().DeleteDirectory(*TestPluginDir, /*RequireExists=*/false, /*Tree=*/true);
				ExceptionHandler.Reset();
				return;
			}
			ON_SCOPE_EXIT
			{
				IPluginManager::Get().RemoveFromPluginsList(TestDescriptorFileName);
				IFileManager::Get().DeleteDirectory(*TestPluginDir, /*RequireExists=*/false, /*Tree=*/true);
			};

			const auto FindDependency = [this, &TestPluginName](const TCHAR* DependencyName)
			{
				const TArray<FPluginDependencyToolsetInfo59> Dependencies = UPluginToolset59::GetPluginDependencies(TestPluginName);
				const FPluginDependencyToolsetInfo59* Found = Dependencies.FindByPredicate(
					[DependencyName](const FPluginDependencyToolsetInfo59& Entry) { return Entry.Name == DependencyName; });
				return Found ? TOptional<FPluginDependencyToolsetInfo59>(*Found) : TOptional<FPluginDependencyToolsetInfo59>();
			};

			ExceptionHandler->CaptureErrorsIn([this, &TestPluginName, &FindDependency]()
			{
				UPluginToolset59::AddPluginDependency(TestPluginName, TEXT("ToolsetRegistry"), false, true, true);
				const TOptional<FPluginDependencyToolsetInfo59> Added = FindDependency(TEXT("ToolsetRegistry"));
				TestTrue(TEXT("Added dependency reads back as activating"), Added.IsSet() && Added->bActivate);

				UPluginToolset59::UpdatePluginDependency(TestPluginName, TEXT("DataflowCore"), NullOpt, NullOpt, true);
				const TOptional<FPluginDependencyToolsetInfo59> Activated = FindDependency(TEXT("DataflowCore"));
				TestTrue(TEXT("Only Activate changed"), Activated.IsSet() && Activated->bActivate && !Activated->bOptional && Activated->bEnabled);

				UPluginToolset59::UpdatePluginDependency(TestPluginName, TEXT("DataflowCore"), true, NullOpt, NullOpt);
				const TOptional<FPluginDependencyToolsetInfo59> MadeOptional = FindDependency(TEXT("DataflowCore"));
				TestTrue(TEXT("Only Optional changed"), MadeOptional.IsSet() && MadeOptional->bOptional && MadeOptional->bActivate && MadeOptional->bEnabled);
			});
			ExpectNoException();

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([&TestPluginName]()
			{
				UPluginToolset59::UpdatePluginDependency(TestPluginName, TEXT("DataflowCore"), NullOpt, NullOpt, NullOpt);
			});
			ExpectExceptionContains(TEXT("Nothing to change"));

			FPluginDescriptor SavedDescriptor;
			if (TestTrue(TEXT("Saved descriptor loads"), SavedDescriptor.Load(TestDescriptorFileName, FailReason)))
			{
				const FPluginReferenceDescriptor* SavedRestricted = SavedDescriptor.Plugins.FindByPredicate(
					[](const FPluginReferenceDescriptor& Ref) { return Ref.Name == TEXT("DataflowCore"); });
				TestTrue(TEXT("Activate saved to the .uplugin"), SavedRestricted && SavedRestricted->bActivate);
				TestTrue(TEXT("Platform allow list kept"), SavedRestricted && SavedRestricted->PlatformAllowList == TArray<FString>{ TEXT("Win64") });
			}

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([&TestPluginName]()
			{
				UPluginToolset59::AddPluginDependency(TestPluginName, TEXT("ToolsetRegistry"), false, true, false);
			});
			ExpectExceptionContains(TEXT("already exists with different settings"));

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([&TestPluginName]()
			{
				UPluginToolset59::UpdatePluginDependency(TestPluginName, TEXT("NotADependencyXYZ12345"), false, true, false);
			});
			ExpectExceptionContains(TEXT("not found"));
		});
	});

	Describe(TEXT("DisallowedPlugins"), [this]()
	{
		It(TEXT("Raises error for nonexistent plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UPluginToolset59::GetDisallowedPlugins(TEXT("NonexistentXYZ12345"));
			});
			ExpectExceptionContains(TEXT("PluginToolset"));

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UPluginToolset59::AddDisallowedPlugin(TEXT("NonexistentXYZ12345"), TEXT("ToolsetRegistry"), TEXT(""));
			});
			ExpectExceptionContains(TEXT("PluginToolset"));

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UPluginToolset59::RemoveDisallowedPlugin(TEXT("NonexistentXYZ12345"), TEXT("ToolsetRegistry"));
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});

		It(TEXT("Add/update/remove round-trip"), [this]()
		{
			if (FApp::IsEngineInstalled() || FApp::IsInstalled())
			{
				AddInfo(TEXT("Cannot edit non-mod plugins in an installed build. Skipping test."));
				ExceptionHandler.Reset();
				return;
			}

			// Round-trip against a throwaway descriptor under Saved/ - mutating a
			// real engine .uplugin fails on build machines where the sync is
			// read-only.
			const FString TestPluginName = TEXT("PluginToolsetDisallowedTest");
			const FString TestPluginDir = FPaths::ProjectSavedDir() / TEXT("Temp") / TEXT("PluginToolsetTest");
			const FString TestDescriptorFileName = TestPluginDir / TestPluginName + TEXT(".uplugin");
			const FString DisallowedName = TEXT("DataflowCore");

			FText FailReason;
			const FPluginDescriptor TestDescriptor;
			if (!TestTrue(TEXT("Test descriptor written"), TestDescriptor.Save(TestDescriptorFileName, FailReason)))
			{
				ExceptionHandler.Reset();
				return;
			}
			if (!TestTrue(TEXT("Test plugin registered"), IPluginManager::Get().AddToPluginsList(TestDescriptorFileName, &FailReason)))
			{
				IFileManager::Get().DeleteDirectory(*TestPluginDir, /*RequireExists=*/false, /*Tree=*/true);
				ExceptionHandler.Reset();
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this, &TestPluginName, &DisallowedName]()
			{
				UPluginToolset59::AddDisallowedPlugin(TestPluginName, DisallowedName, TEXT("First"));
				UPluginToolset59::AddDisallowedPlugin(TestPluginName, DisallowedName, TEXT("Second"));

				const TArray<FPluginDisallowedToolsetInfo> Entries = UPluginToolset59::GetDisallowedPlugins(TestPluginName);
				if (TestEqual(TEXT("One entry after adding the same name twice"), Entries.Num(), 1))
				{
					TestEqual(TEXT("Name"), Entries[0].Name, DisallowedName);
					TestEqual(TEXT("Comment updated"), Entries[0].Comment, FString(TEXT("Second")));
				}

				UPluginToolset59::RemoveDisallowedPlugin(TestPluginName, DisallowedName);
				TestTrue(TEXT("Entry removed"), UPluginToolset59::GetDisallowedPlugins(TestPluginName).IsEmpty());
			});
			ExpectNoException();

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([&TestPluginName, &DisallowedName]()
			{
				UPluginToolset59::RemoveDisallowedPlugin(TestPluginName, DisallowedName);
			});
			ExpectExceptionContains(TEXT("not found"));

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([&TestPluginName]()
			{
				UPluginToolset59::AddDisallowedPlugin(TestPluginName, TestPluginName, TEXT(""));
			});
			ExpectExceptionContains(TEXT("cannot disallow itself"));

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([&TestPluginName]()
			{
				UPluginToolset59::AddDisallowedPlugin(TestPluginName, TEXT(""), TEXT(""));
			});
			ExpectExceptionContains(TEXT("cannot be empty"));

			// Unregister and delete the throwaway plugin
			IPluginManager::Get().RemoveFromPluginsList(TestDescriptorFileName);
			IFileManager::Get().DeleteDirectory(*TestPluginDir, /*RequireExists=*/false, /*Tree=*/true);
		});
	});

	Describe(TEXT("AdditionalPluginDirectories"), [this]()
	{
		It(TEXT("Matches the project manager's list"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TestTrue(TEXT("Same list"),
					UPluginToolset59::GetAdditionalPluginDirectories() == IProjectManager::Get().GetAdditionalPluginDirectories());
			});
			ExpectNoException();
		});

		It(TEXT("Raises error when adding a directory that does not exist"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UPluginToolset59::AddAdditionalPluginDirectory(FPaths::ProjectSavedDir() / TEXT("NonexistentDirXYZ12345"));
			});
			ExpectException();
		});

		It(TEXT("Raises error when removing a directory that is not listed"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UPluginToolset59::RemoveAdditionalPluginDirectory(FPaths::ProjectSavedDir() / TEXT("NonexistentDirXYZ12345"));
			});
			ExpectException();
		});

		It(TEXT("Add/remove round-trip"), [this]()
		{
			bool bCanModify = true;
			if (!GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanModifyProjectPluginDirectoriesFromBrowser"), bCanModify, GEditorIni))
			{
				GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanModifyPluginsFromBrowser"), bCanModify, GEditorIni);
			}
			const FString ProjectFile = FPaths::GetProjectFilePath();
			const bool bWasReadOnly = IFileManager::Get().IsReadOnly(*ProjectFile);
			if (!bCanModify || (bWasReadOnly && !USourceControlHelpers::IsAvailable()))
			{
				AddInfo(TEXT("Project plugin directories cannot be modified here. Skipping test."));
				ExceptionHandler.Reset();
				return;
			}

			// A writable project file that source control hasn't opened may hold local edits, and the add would
			// check it out and rewrite it.
			ISourceControlModule& SCModule = ISourceControlModule::Get();
			const FSourceControlStatePtr ProjectState = SCModule.IsEnabled()
				? SCModule.GetProvider().GetState(ProjectFile, EStateCacheUsage::ForceUpdate) : nullptr;
			if (!bWasReadOnly && ProjectState.IsValid() && ProjectState->IsSourceControlled()
				&& !ProjectState->IsCheckedOut() && !ProjectState->IsAdded())
			{
				AddInfo(TEXT("Project file is writable without being checked out and may hold local edits. Skipping test."));
				ExceptionHandler.Reset();
				return;
			}
			// Each change saves the whole descriptor, which would write any unrelated unsaved changes too.
			if (IProjectManager::Get().IsCurrentProjectDirty())
			{
				AddInfo(TEXT("Additional plugin directory test needs a project without unsaved descriptor changes; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			// A fresh directory per run, so it can't be one the user already has or lists.
			const FString TestDir = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir() / TEXT("Temp") / TEXT("PluginToolsetTest") / FGuid::NewGuid().ToString());
			IFileManager::Get().MakeDirectory(*TestDir, /*Tree=*/true);
			auto IsListed = [&TestDir]()
			{
				return IProjectManager::Get().GetAdditionalPluginDirectories().ContainsByPredicate(
					[&TestDir](const FString& Listed) { return FPaths::IsSamePath(Listed, TestDir); });
			};

			ExceptionHandler->CaptureErrorsIn([this, &TestDir, &IsListed]()
			{
				UPluginToolset59::AddAdditionalPluginDirectory(TestDir);
				TestTrue(TEXT("Listed after add"), IsListed());

				UPluginToolset59::AddAdditionalPluginDirectory(TestDir);
				TestTrue(TEXT("Adding twice is a no-op"), IsListed());

				UPluginToolset59::RemoveAdditionalPluginDirectory(TestDir);
				TestFalse(TEXT("Not listed after remove"), IsListed());
			});
			ExpectNoException();

			if (IsListed())
			{
				IProjectManager::Get().UpdateAdditionalPluginDirectory(TestDir, false);
			}
			if (bWasReadOnly)
			{
				USourceControlHelpers::RevertFiles({ ProjectFile });
			}
			IFileManager::Get().DeleteDirectory(*TestDir, /*RequireExists=*/false, /*Tree=*/true);
		});

		It(TEXT("Raises error and writes nothing while project file writes are suppressed"), [this]()
		{
			bool bCanModify = true;
			if (!GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanModifyProjectPluginDirectoriesFromBrowser"), bCanModify, GEditorIni))
			{
				GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanModifyPluginsFromBrowser"), bCanModify, GEditorIni);
			}
			if (!bCanModify)
			{
				AddInfo(TEXT("Project plugin directories cannot be modified here. Skipping test."));
				ExceptionHandler.Reset();
				return;
			}

			const FString ProjectFile = FPaths::GetProjectFilePath();
			const FDateTime TimeStampBefore = IFileManager::Get().GetTimeStamp(*ProjectFile);
			const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
			const FName WriteSuppressionName(TEXT("PluginToolsetSpec"));
			IProjectManager::Get().AddSuppressProjectFileWrite(WriteSuppressionName);
			ON_SCOPE_EXIT
			{
				IProjectManager::Get().RemoveSuppressProjectFileWrite(WriteSuppressionName);
			};

			ExceptionHandler->CaptureErrorsIn([&Directory]()
			{
				UPluginToolset59::AddAdditionalPluginDirectory(Directory);
			});
			ExpectExceptionContains(TEXT("suppressed"));

			TestFalse(TEXT("Directory not added"),
				IProjectManager::Get().GetAdditionalPluginDirectories().ContainsByPredicate([&Directory](const FString& Listed)
				{
					return FPaths::IsSamePath(Listed, Directory);
				}));
			TestEqual(TEXT("Project file not written"), IFileManager::Get().GetTimeStamp(*ProjectFile), TimeStampBefore);
		});

		It(TEXT("Raises error when the project file cannot be written"), [this]()
		{
			bool bCanModify = true;
			if (!GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanModifyProjectPluginDirectoriesFromBrowser"), bCanModify, GEditorIni))
			{
				GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanModifyPluginsFromBrowser"), bCanModify, GEditorIni);
			}
			// Only a read-only project file with no source control to check it out reliably fails, as on CI.
			if (!bCanModify || !IFileManager::Get().IsReadOnly(*FPaths::GetProjectFilePath()) || USourceControlHelpers::IsAvailable())
			{
				AddInfo(TEXT("Project file is writable or can be checked out. Skipping test."));
				ExceptionHandler.Reset();
				return;
			}

			ExceptionHandler->CaptureErrorsIn([]()
			{
				UPluginToolset59::AddAdditionalPluginDirectory(FPaths::ProjectSavedDir());
			});
			ExpectExceptionContains(TEXT("could not be saved"));
			TestFalse(TEXT("Failed add is undone in memory"),
				IProjectManager::Get().GetAdditionalPluginDirectories().ContainsByPredicate([](const FString& Listed)
				{
					return FPaths::IsSamePath(Listed, FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));
				}));
		});
	});

	Describe(TEXT("SetPluginEnabled"), [this]()
	{
		It(TEXT("Raises error for nonexistent plugin"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UPluginToolset59::SetPluginEnabled(TEXT("NonexistentXYZ12345"), true);
			});
			ExpectExceptionContains(TEXT("PluginToolset"));
		});

		It(TEXT("Persists plugin enablement and reports save failures in a disposable project"), [this]()
		{
			bool bModificationAllowed = true;
			GConfig->GetBool(TEXT("EditorSettings"), TEXT("bCanModifyPluginsFromBrowser"), bModificationAllowed, GEditorIni);
			if (!bModificationAllowed)
			{
				ExceptionHandler.Reset();
				return;
			}
			IProjectManager& ProjectManager = IProjectManager::Get();
			const FProjectDescriptor* CurrentProject = ProjectManager.GetCurrentProject();
			if (CurrentProject == nullptr)
			{
				AddError(TEXT("No project is loaded."));
				return;
			}
			if (ProjectManager.IsCurrentProjectDirty())
			{
				AddInfo(TEXT("Plugin persistence test needs a project without unsaved descriptor changes; skipping."));
				ExceptionHandler.Reset();
				return;
			}
			const FScopedDisposableProject DisposableProject(*this, FScopedDisposableProject::ERedirect::ForScope);
			if (!DisposableProject.IsValid())
			{
				return;
			}
			const FString& TestDirectory = DisposableProject.GetDirectory();
			const FString& TestProjectFilename = DisposableProject.GetProjectFilename();
			FText FailureReason;
			const TSharedPtr<IPlugin> TestPlugin = IPluginManager::Get().FindPlugin(TEXT("ToolsetRegistry"));
			if (!TestTrue(TEXT("test plugin exists"), TestPlugin.IsValid()))
			{
				return;
			}
			const auto IsSavedPluginEnabled = [&TestPlugin](const FProjectDescriptor& Descriptor)
			{
				const FPluginReferenceDescriptor* Reference = Descriptor.Plugins.FindByPredicate([](const FPluginReferenceDescriptor& Entry) { return Entry.Name == TEXT("ToolsetRegistry"); });
				return Reference ? Reference->bEnabled : TestPlugin->IsEnabledByDefault(!Descriptor.bDisableEnginePluginsByDefault);
			};

			bool bDisableNeedsRestart = false;
			ExceptionHandler->CaptureErrorsIn([&bDisableNeedsRestart]()
			{
				bDisableNeedsRestart = UPluginToolset59::SetPluginEnabled(TEXT("ToolsetRegistry"), false);
			});
			ExpectNoException();
			FProjectDescriptor SavedDescriptor;
			TestTrue(TEXT("disabled descriptor loads"), SavedDescriptor.Load(TestProjectFilename, FailureReason));
			TestFalse(TEXT("disabled state persists"), IsSavedPluginEnabled(SavedDescriptor));

			bool bEnableNeedsRestart = true;
			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([&bEnableNeedsRestart]()
			{
				bEnableNeedsRestart = UPluginToolset59::SetPluginEnabled(TEXT("ToolsetRegistry"), true);
			});
			ExpectNoException();
			TestTrue(TEXT("enabled descriptor loads"), SavedDescriptor.Load(TestProjectFilename, FailureReason));
			TestTrue(TEXT("enabled state persists"), IsSavedPluginEnabled(SavedDescriptor));
			TestFalse(TEXT("project changes saved"), ProjectManager.IsCurrentProjectDirty());
			TestTrue(TEXT("Disabling a running plugin needs a restart"), bDisableNeedsRestart);
			TestFalse(TEXT("Restoring the running state needs no restart"), bEnableNeedsRestart);
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			if (TestTrue(TEXT("test project made read-only"), PlatformFile.SetReadOnly(*TestProjectFilename, true)))
			{
				ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
				ExceptionHandler->CaptureErrorsIn([]() { UPluginToolset59::SetPluginEnabled(TEXT("ToolsetRegistry"), false); });
				ExpectExceptionContains(TEXT("read-only"));
				TestFalse(TEXT("read-only project file refused before anything changed"), ProjectManager.IsCurrentProjectDirty());
				TestTrue(TEXT("read-only project file leaves the plugin enabled"), IsSavedPluginEnabled(*ProjectManager.GetCurrentProject()));
				PlatformFile.SetReadOnly(*TestProjectFilename, false);
			}

			FPaths::SetProjectFilePath(TestDirectory);
			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([]() { UPluginToolset59::SetPluginEnabled(TEXT("ToolsetRegistry"), false); });
			ExpectExceptionContains(TEXT("could not be saved"));
			TestTrue(TEXT("failed change undone in memory"), IsSavedPluginEnabled(*ProjectManager.GetCurrentProject()));
			FPaths::SetProjectFilePath(TestProjectFilename);
			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([]() { UPluginToolset59::SetPluginEnabled(TEXT("ToolsetRegistry"), true); });
			ExpectNoException();
		});
	});
}
#endif
