// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "ISettingsCategory.h"
#include "ISettingsContainer.h"
#include "ISettingsModule.h"
#include "ISettingsSection.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "HAL/FileManager.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/Paths.h"
#include "ProjectDescriptor.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "SourceControlHelpers.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"

#include "ConfigSettingsToolset59.h"
#include "Tests/ConfigSettingsToolsetTestObjects59.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
const EAutomationTestFlags TestFlags =
	EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter |
	EAutomationTestFlags::CriticalPriority;

// Known-stable test coordinates: always registered by ProjectSettingsViewerModule.
const FString TestContainer = TEXT("Project");
const FString TestCategory  = TEXT("Project");
const FString TestSection   = TEXT("General");
}

BEGIN_DEFINE_SPEC(
	FConfigSettingsToolsetSpec59,
	TEXT("AIAssistant.Toolsets.ConfigSettingsToolset"),
	TestFlags)

// Searches all registered sections for one matching Predicate.
// Returns false if none found. Uses ISettingsModule directly to avoid
// going through the toolset exception handler during discovery.
bool FindSectionWhere(
	TFunctionRef<bool(ISettingsSection&)> Predicate,
	FString& OutContainer, FString& OutCategory, FString& OutSection)
{
	ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
	if (!SettingsModule)
	{
		return false;
	}
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
				if (Section.IsValid() && Predicate(*Section))
				{
					OutContainer = ContainerName.ToString();
					OutCategory  = Category->GetName().ToString();
					OutSection   = Section->GetName().ToString();
					return true;
				}
			}
		}
	}
	return false;
}

void ExpectNoException()
{
	check(ExceptionHandler.IsValid());
	TestEqual(TEXT("Error"), *ExceptionHandler->GetException(), TEXT(""));
	ExceptionHandler.Reset();
}

void ExpectExceptionContains(const FString& ErrorMessage)
{
	check(ExceptionHandler.IsValid());
	TestTrue(*FString::Printf(TEXT("Error contains '%s'"), *ErrorMessage),
		ExceptionHandler->GetException().Contains(ErrorMessage));
	ExceptionHandler.Reset();
}

private:
	TUniquePtr<UE::ToolsetRegistry::FToolCallExceptionHandler> ExceptionHandler;
END_DEFINE_SPEC(FConfigSettingsToolsetSpec59)

void FConfigSettingsToolsetSpec59::Define()
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


	Describe(TEXT("ListContainers"), [this]()
	{
		It(TEXT("returns a sorted list"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UConfigSettingsToolset59::ListContainers();
				for (int32 i = 1; i < Names.Num(); ++i)
				{
					TestTrue(TEXT("Sorted"), Names[i - 1] <= Names[i]);
				}
			});
			ExpectNoException();
		});

		It(TEXT("contains 'Project'"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UConfigSettingsToolset59::ListContainers();
				TestTrue(TEXT("Contains Project"), Names.Contains(TestContainer));
			});
			ExpectNoException();
		});
	});

	Describe(TEXT("ListCategories"), [this]()
	{
		It(TEXT("returns categories for 'Project'"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UConfigSettingsToolset59::ListCategories(TestContainer);
				TestFalse(TEXT("Non-empty"), Names.IsEmpty());
			});
			ExpectNoException();
		});

		It(TEXT("raises error for nonexistent container"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UConfigSettingsToolset59::ListCategories(TEXT("NonexistentContainerXYZ12345"));
			});
			ExpectExceptionContains(TEXT("ConfigSettingsToolset"));
		});
	});

	Describe(TEXT("ListSections"), [this]()
	{
		It(TEXT("returns sections for 'Project'/'Project'"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UConfigSettingsToolset59::ListSections(TestContainer, TestCategory);
				TestFalse(TEXT("Non-empty"), Names.IsEmpty());
			});
			ExpectNoException();
		});

		It(TEXT("raises error for nonexistent category"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UConfigSettingsToolset59::ListSections(TestContainer, TEXT("NonexistentCategoryXYZ12345"));
			});
			ExpectExceptionContains(TEXT("ConfigSettingsToolset"));
		});
	});

	Describe(TEXT("GetSectionSchema"), [this]()
	{
		It(TEXT("returns schema for 'Project'/'Project'/'General'"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FString Schema =
					UConfigSettingsToolset59::GetSectionSchema(TestContainer, TestCategory, TestSection);
				TestFalse(TEXT("Non-empty"), Schema.IsEmpty());
			});
			ExpectNoException();
		});

		It(TEXT("raises error for nonexistent section"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UConfigSettingsToolset59::GetSectionSchema(
					TestContainer, TestCategory, TEXT("NonexistentSectionXYZ12345"));
			});
			ExpectExceptionContains(TEXT("ConfigSettingsToolset"));
		});
	});

	Describe(TEXT("GetSectionPropertyValues"), [this]()
	{
		It(TEXT("returns values for 'Project'/'Project'/'General'"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FString Values = UConfigSettingsToolset59::GetSectionPropertyValues(
					TestContainer, TestCategory, TestSection,
					{TEXT("bShouldWindowPreserveAspectRatio")});
				TestFalse(TEXT("Non-empty"), Values.IsEmpty());
			});
			ExpectNoException();
		});

		It(TEXT("raises error for nonexistent section"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UConfigSettingsToolset59::GetSectionPropertyValues(
					TestContainer, TestCategory, TEXT("NonexistentSectionXYZ12345"),
					{TEXT("bShouldWindowPreserveAspectRatio")});
			});
			ExpectExceptionContains(TEXT("ConfigSettingsToolset"));
		});
	});

	Describe(TEXT("SupportedPlatforms"), [this]()
	{
		// A list that differs from the project's current one by a single available platform. It never drops a
		// listed platform whose data isn't installed here, since that one couldn't be set back afterwards.
		auto MakeDifferentPlatforms = []()
		{
			TArray<FString> Platforms = UConfigSettingsToolset59::GetSupportedPlatforms();
			if (Platforms.IsEmpty())
			{
				return TArray<FString>{ TEXT("Windows") };
			}
			const TArray<FString> Available = UConfigSettingsToolset59::ListAvailablePlatforms();
			for (const FString& Platform : Available)
			{
				if (!Platforms.Contains(Platform))
				{
					Platforms.Add(Platform);
					return Platforms;
				}
			}
			Platforms.Remove(Available[0]);
			return Platforms;
		};

		It(TEXT("lists available platforms"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TestTrue(TEXT("Contains Windows"), UConfigSettingsToolset59::ListAvailablePlatforms().Contains(TEXT("Windows")));
			});
			ExpectNoException();
		});

		It(TEXT("matches the project descriptor"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TArray<FString> Expected;
				for (const FName& Platform : IProjectManager::Get().GetCurrentProject()->TargetPlatforms)
				{
					Expected.Add(Platform.ToString());
				}
				TestTrue(TEXT("Same list"), UConfigSettingsToolset59::GetSupportedPlatforms() == Expected);
			});
			ExpectNoException();
		});

		It(TEXT("raises error for an unknown platform"), [this]()
		{
			const TArray<FString> Before = UConfigSettingsToolset59::GetSupportedPlatforms();
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UConfigSettingsToolset59::SetSupportedPlatforms({ TEXT("NoSuchPlatformXYZ12345") });
			});
			ExpectExceptionContains(TEXT("Unknown platform"));
			TestTrue(TEXT("List unchanged"), UConfigSettingsToolset59::GetSupportedPlatforms() == Before);
		});

		It(TEXT("raises error and writes nothing while project file writes are suppressed"), [this, MakeDifferentPlatforms]()
		{
			const FString ProjectFile = FPaths::GetProjectFilePath();
			const FDateTime TimeStampBefore = IFileManager::Get().GetTimeStamp(*ProjectFile);
			const TArray<FString> Before = UConfigSettingsToolset59::GetSupportedPlatforms();
			const TArray<FString> Different = MakeDifferentPlatforms();

			const FName WriteSuppressionName(TEXT("ConfigSettingsToolsetSpec"));
			IProjectManager::Get().AddSuppressProjectFileWrite(WriteSuppressionName);
			ON_SCOPE_EXIT
			{
				IProjectManager::Get().RemoveSuppressProjectFileWrite(WriteSuppressionName);
			};

			ExceptionHandler->CaptureErrorsIn([&Different]()
			{
				UConfigSettingsToolset59::SetSupportedPlatforms(Different);
			});
			ExpectExceptionContains(TEXT("suppressed"));
			TestTrue(TEXT("List unchanged"), UConfigSettingsToolset59::GetSupportedPlatforms() == Before);
			TestEqual(TEXT("Project file not written"), IFileManager::Get().GetTimeStamp(*ProjectFile), TimeStampBefore);
		});

		It(TEXT("set/restore round-trip"), [this, MakeDifferentPlatforms]()
		{
			const FString ProjectFile = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
			const bool bWasReadOnly = IFileManager::Get().IsReadOnly(*ProjectFile);
			if (bWasReadOnly && !USourceControlHelpers::IsAvailable())
			{
				AddInfo(TEXT("Project file cannot be written here; skipping."));
				ExceptionHandler.Reset();
				return;
			}
			// A writable project file that source control hasn't opened may hold local edits, and the set would
			// check it out and rewrite it.
			ISourceControlModule& SCModule = ISourceControlModule::Get();
			const FSourceControlStatePtr ProjectState = SCModule.IsEnabled()
				? SCModule.GetProvider().GetState(ProjectFile, EStateCacheUsage::ForceUpdate) : nullptr;
			if (!bWasReadOnly && ProjectState.IsValid() && ProjectState->IsSourceControlled()
				&& !ProjectState->IsCheckedOut() && !ProjectState->IsAdded())
			{
				AddInfo(TEXT("Project file is writable without being checked out and may hold local edits; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			const TArray<FString> Original = UConfigSettingsToolset59::GetSupportedPlatforms();
			const TArray<FString> Different = MakeDifferentPlatforms();
			ON_SCOPE_EXIT
			{
				if (UConfigSettingsToolset59::GetSupportedPlatforms() != Original)
				{
					UConfigSettingsToolset59::SetSupportedPlatforms(Original);
				}
				if (bWasReadOnly)
				{
					USourceControlHelpers::RevertFiles({ ProjectFile }, /*bSilent=*/true);
				}
			};

			ExceptionHandler->CaptureErrorsIn([this, &Original, &Different]()
			{
				UConfigSettingsToolset59::SetSupportedPlatforms(Different);
				TestTrue(TEXT("Changed list applied"), UConfigSettingsToolset59::GetSupportedPlatforms() == Different);

				UConfigSettingsToolset59::SetSupportedPlatforms(Original);
				TestTrue(TEXT("Original list restored"), UConfigSettingsToolset59::GetSupportedPlatforms() == Original);
			});
			ExpectNoException();
		});

		It(TEXT("raises error when the project file cannot be written"), [this, MakeDifferentPlatforms]()
		{
			// Only a read-only project file with no source control to check it out reliably fails, as on CI.
			if (!IFileManager::Get().IsReadOnly(*FPaths::GetProjectFilePath()) || USourceControlHelpers::IsAvailable())
			{
				AddInfo(TEXT("Project file is writable or can be checked out; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			const TArray<FString> Before = UConfigSettingsToolset59::GetSupportedPlatforms();
			const TArray<FString> Different = MakeDifferentPlatforms();
			ExceptionHandler->CaptureErrorsIn([&Different]()
			{
				UConfigSettingsToolset59::SetSupportedPlatforms(Different);
			});
			ExpectExceptionContains(TEXT("could not be saved"));
			TestTrue(TEXT("Failed change is undone in memory"), UConfigSettingsToolset59::GetSupportedPlatforms() == Before);
		});
	});

	Describe(TEXT("SearchSettings"), [this]()
	{
		It(TEXT("finds a property from words in its label"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				// "Should Window Preserve Aspect Ratio": two words, out of order, in a different case.
				const TArray<FConfigSettingsSearchResult> Results = UConfigSettingsToolset59::SearchSettings(TEXT("ASPECT window"));
				const bool bFound = Results.ContainsByPredicate([](const FConfigSettingsSearchResult& Result)
				{
					return Result.ContainerName == TestContainer && Result.CategoryName == TestCategory
						&& Result.SectionName == TestSection && Result.PropertyName == TEXT("bShouldWindowPreserveAspectRatio");
				});
				TestTrue(TEXT("Found bShouldWindowPreserveAspectRatio in Project/Project/General"), bFound);
			});
			ExpectNoException();
		});

		It(TEXT("finds a property from the console variable it is bound to"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FString ConsoleVariable = TEXT("gc.TimeBetweenPurgingPendingKillObjects");
				const TArray<FConfigSettingsSearchResult> Results = UConfigSettingsToolset59::SearchSettings(ConsoleVariable);
				const FConfigSettingsSearchResult* Found = Results.FindByPredicate([](const FConfigSettingsSearchResult& Result)
				{
					return Result.PropertyName == TEXT("TimeBetweenPurgingPendingKillObjects");
				});
				if (TestNotNull(TEXT("Found the Garbage Collection setting"), Found))
				{
					TestEqual(TEXT("Console variable reported"), Found->ConsoleVariableName, ConsoleVariable);
				}
			});
			ExpectNoException();
		});

		It(TEXT("returns sorted results"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FConfigSettingsSearchResult> Results = UConfigSettingsToolset59::SearchSettings(TEXT("window"));
				TestFalse(TEXT("Non-empty"), Results.IsEmpty());
				for (int32 i = 1; i < Results.Num(); ++i)
				{
					const FConfigSettingsSearchResult& Previous = Results[i - 1];
					const FConfigSettingsSearchResult& Current = Results[i];
					const TArray<const FString*> PreviousFields = { &Previous.ContainerName, &Previous.CategoryName, &Previous.SectionName, &Previous.PropertyName };
					const TArray<const FString*> CurrentFields = { &Current.ContainerName, &Current.CategoryName, &Current.SectionName, &Current.PropertyName };
					bool bInOrder = true;
					for (int32 Field = 0; Field < PreviousFields.Num(); ++Field)
					{
						if (*PreviousFields[Field] != *CurrentFields[Field])
						{
							bInOrder = *PreviousFields[Field] < *CurrentFields[Field];
							break;
						}
					}
					TestTrue(TEXT("Sorted"), bInOrder);
				}
			});
			ExpectNoException();
		});

		It(TEXT("returns nothing when no setting matches"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TestTrue(TEXT("Empty"), UConfigSettingsToolset59::SearchSettings(TEXT("NoSuchSettingXYZ12345")).IsEmpty());
			});
			ExpectNoException();
		});

		It(TEXT("raises error for an empty query"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UConfigSettingsToolset59::SearchSettings(TEXT("   "));
			});
			ExpectExceptionContains(TEXT("ConfigSettingsToolset"));
		});
	});

	Describe(TEXT("SetSectionProperties"), [this]()
	{
		It(TEXT("raises read-only error for 'Project'/'Project'/'General' when default config file is not writable"), [this]()
		{
			// This test verifies the error path when the backing DefaultConfig .ini is
			// physically read-only and source control is not active (e.g. Horde build agents).
			// It skips when the file is writable or SC is active, since those paths are
			// covered by the separate writable-section round-trip test.
			ISettingsModule* SM = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
			TSharedPtr<ISettingsContainer> Container = SM ? SM->GetContainer(FName(*TestContainer)) : nullptr;
			TSharedPtr<ISettingsCategory> Category = Container.IsValid() ? Container->GetCategory(FName(*TestCategory)) : nullptr;
			TSharedPtr<ISettingsSection> Section = Category.IsValid() ? Category->GetSection(FName(*TestSection)) : nullptr;
			UObject* SettingsObject = Section.IsValid() ? Section->GetSettingsObject().Get() : nullptr;

			if (!SettingsObject || !SettingsObject->GetClass()->HasAnyClassFlags(CLASS_DefaultConfig))
			{
				AddInfo(TEXT("Expected DefaultConfig settings object not found; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			const FString ConfigPath = FPaths::ConvertRelativePathToFull(SettingsObject->GetDefaultConfigFilename());

			if (ISourceControlModule::Get().IsEnabled() || !IFileManager::Get().IsReadOnly(*ConfigPath))
			{
				AddInfo(TEXT("Default config file is writable or source control is active; read-only error case not exercisable here."));
				ExceptionHandler.Reset();
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UConfigSettingsToolset59::SetSectionProperties(
					TestContainer, TestCategory, TestSection,
					TEXT("{\"bShouldWindowPreserveAspectRatio\": true}"));
			});
			ExpectExceptionContains(TEXT("read-only default config file"));
		});

		It(TEXT("checks out the default config file for 'Project'/'Project'/'General' when source control is active"), [this]()
		{
			// SC active and the DefaultConfig .ini not checked out: the set checks it out, then the test reverts it.
			// Skips when SC is inactive (covered by the read-only test) or file is already checked out.
			ISettingsModule* SM = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
			TSharedPtr<ISettingsContainer> Container = SM ? SM->GetContainer(FName(*TestContainer)) : nullptr;
			TSharedPtr<ISettingsCategory> Category = Container.IsValid() ? Container->GetCategory(FName(*TestCategory)) : nullptr;
			TSharedPtr<ISettingsSection> Section = Category.IsValid() ? Category->GetSection(FName(*TestSection)) : nullptr;
			UObject* SettingsObject = Section.IsValid() ? Section->GetSettingsObject().Get() : nullptr;

			if (!SettingsObject || !SettingsObject->GetClass()->HasAnyClassFlags(CLASS_DefaultConfig))
			{
				AddInfo(TEXT("Expected DefaultConfig settings object not found; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			ISourceControlModule& SCModule = ISourceControlModule::Get();
			if (!SCModule.IsEnabled())
			{
				AddInfo(TEXT("Source control is not active; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			const FString ConfigPath = FPaths::ConvertRelativePathToFull(SettingsObject->GetDefaultConfigFilename());
			FSourceControlStatePtr State = SCModule.GetProvider().GetState(ConfigPath, EStateCacheUsage::ForceUpdate);
			if (!State.IsValid() || !State->IsSourceControlled())
			{
				AddInfo(TEXT("Default config file is not source controlled; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			if (State->IsCheckedOut() || State->IsAdded())
			{
				AddInfo(TEXT("Default config file is already checked out; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			// The cleanup reverts the file, which would also discard offline edits in a writable, unopened copy.
			if (!IFileManager::Get().IsReadOnly(*ConfigPath))
			{
				AddInfo(TEXT("Default config file is writable without being checked out and may hold local edits; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			ON_SCOPE_EXIT
			{
				USourceControlHelpers::RevertFiles({ ConfigPath }, /*bSilent=*/true);
			};

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				// Writing the current value back leaves the setting unchanged for the rest of the session.
				const FString Current = UConfigSettingsToolset59::GetSectionPropertyValues(
					TestContainer, TestCategory, TestSection, {TEXT("bShouldWindowPreserveAspectRatio")});
				UConfigSettingsToolset59::SetSectionProperties(TestContainer, TestCategory, TestSection, Current);
			});
			ExpectNoException();

			State = SCModule.GetProvider().GetState(ConfigPath, EStateCacheUsage::ForceUpdate);
			TestTrue(TEXT("Default config file checked out"), State.IsValid() && State->IsCheckedOut());
		});

		It(TEXT("applies nothing when any value is rejected"), [this]()
		{
			ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
			if (!SettingsModule)
			{
				AddInfo(TEXT("Settings module not available; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			const FString WritableContainer = TEXT("ConfigSettingsToolsetTest");
			const FString WritableCategory  = TEXT("Test");
			const FString WritableSection   = TEXT("Atomic");

			UConfigSettingsToolsetTestSettings59* TestObj = NewObject<UConfigSettingsToolsetTestSettings59>();
			TestObj->AddToRoot();
			SettingsModule->RegisterSettings(
				FName(*WritableContainer), FName(*WritableCategory), FName(*WritableSection),
				INVTEXT("Test Section"), INVTEXT("Automation test settings"),
				TestObj);
			ON_SCOPE_EXIT
			{
				SettingsModule->UnregisterSettings(
					FName(*WritableContainer), FName(*WritableCategory), FName(*WritableSection));
				TestObj->RemoveFromRoot();
			};

			const bool bOriginal = TestObj->bTestBoolProperty;
			bool bResult = true;
			ExceptionHandler->CaptureErrorsIn([&bResult, &WritableContainer, &WritableCategory, &WritableSection, bOriginal]()
			{
				// The bool alone would apply; the unknown property is rejected, so the bool must be rolled back.
				bResult = UConfigSettingsToolset59::SetSectionProperties(
					WritableContainer, WritableCategory, WritableSection,
					FString::Printf(TEXT("{\"bTestBoolProperty\": %s, \"NonexistentPropertyXYZ12345\": true}"),
						bOriginal ? TEXT("false") : TEXT("true")));
			});
			ExpectExceptionContains(TEXT("could not be set"));

			TestFalse(TEXT("Call reports failure"), bResult);
			TestEqual(TEXT("Accepted value rolled back"), TestObj->bTestBoolProperty, bOriginal);
		});

		It(TEXT("notifies a partly applied struct after rolling it back"), [this]()
		{
			ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
			if (!SettingsModule)
			{
				AddInfo(TEXT("Settings module not available; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			const FString WritableContainer = TEXT("ConfigSettingsToolsetTest");
			const FString WritableCategory  = TEXT("Test");
			const FString WritableSection   = TEXT("PartialStruct");

			UConfigSettingsToolsetTestSettings59* TestObj = NewObject<UConfigSettingsToolsetTestSettings59>();
			TestObj->AddToRoot();
			SettingsModule->RegisterSettings(
				FName(*WritableContainer), FName(*WritableCategory), FName(*WritableSection),
				INVTEXT("Test Section"), INVTEXT("Automation test settings"),
				TestObj);
			ON_SCOPE_EXIT
			{
				SettingsModule->UnregisterSettings(
					FName(*WritableContainer), FName(*WritableCategory), FName(*WritableSection));
				TestObj->RemoveFromRoot();
			};

			const int32 OriginalFirst = TestObj->TestStruct.First;
			ExceptionHandler->CaptureErrorsIn([&WritableContainer, &WritableCategory, &WritableSection, OriginalFirst]()
			{
				UConfigSettingsToolset59::SetSectionProperties(
					WritableContainer, WritableCategory, WritableSection,
					FString::Printf(TEXT("{\"TestStruct\": {\"First\": %d, \"NonexistentMemberXYZ12345\": 1}}"), OriginalFirst + 1));
			});
			ExpectExceptionContains(TEXT("could not be set"));

			TestEqual(TEXT("Struct value rolled back"), TestObj->TestStruct.First, OriginalFirst);
			// The partial value was applied and notified, then the rollback notified the original value again.
			TestTrue(TEXT("Struct notified with the partial value, then the rolled-back value"),
				TestObj->NotifiedTestStructFirst == TArray<int32>{ OriginalFirst + 1, OriginalFirst });
		});

		It(TEXT("leaves an earlier transaction alone when a rejected call changed nothing"), [this]()
		{
			ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
			if (!SettingsModule || !GEditor || !GEditor->Trans)
			{
				AddInfo(TEXT("Settings module or transaction buffer not available; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			const FString WritableContainer = TEXT("ConfigSettingsToolsetTest");
			const FString WritableCategory  = TEXT("Test");
			const FString WritableSection   = TEXT("EarlierTransaction");

			UConfigSettingsToolsetTestSettings59* TestObj = NewObject<UConfigSettingsToolsetTestSettings59>(
				GetTransientPackage(), NAME_None, RF_Transactional);
			TestObj->AddToRoot();
			SettingsModule->RegisterSettings(
				FName(*WritableContainer), FName(*WritableCategory), FName(*WritableSection),
				INVTEXT("Test Section"), INVTEXT("Automation test settings"),
				TestObj);
			ON_SCOPE_EXIT
			{
				SettingsModule->UnregisterSettings(
					FName(*WritableContainer), FName(*WritableCategory), FName(*WritableSection));
				TestObj->RemoveFromRoot();
			};

			const bool bEarlierValue = !TestObj->bTestBoolProperty;
			{
				const FScopedTransaction EarlierTransaction(INVTEXT("Earlier edit"));
				TestObj->Modify();
				TestObj->bTestBoolProperty = bEarlierValue;
			}
			const FGuid EarlierTransactionId = GEditor->Trans->GetUndoContext(false).TransactionId;

			ExceptionHandler->CaptureErrorsIn([&WritableContainer, &WritableCategory, &WritableSection]()
			{
				UConfigSettingsToolset59::SetSectionProperties(
					WritableContainer, WritableCategory, WritableSection,
					TEXT("{\"NonexistentPropertyXYZ12345\": true}"));
			});
			ExpectExceptionContains(TEXT("could not be set"));

			TestEqual(TEXT("Earlier edit kept"), TestObj->bTestBoolProperty, bEarlierValue);
			if (TestEqual(TEXT("Earlier transaction still newest"),
				GEditor->Trans->GetUndoContext(false).TransactionId, EarlierTransactionId))
			{
				GEditor->UndoTransaction(/*bCanRedo=*/false);
			}
		});

		It(TEXT("round-trips a property on a registered writable section"), [this]()
		{
			// Uses a custom non-DefaultConfig settings object so the write path always
			// succeeds regardless of source control state or file permissions.
			ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
			if (!SettingsModule)
			{
				AddInfo(TEXT("Settings module not available; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			const FString WritableContainer = TEXT("ConfigSettingsToolsetTest");
			const FString WritableCategory  = TEXT("Test");
			const FString WritableSection   = TEXT("Properties");

			UConfigSettingsToolsetTestSettings59* TestObj = NewObject<UConfigSettingsToolsetTestSettings59>();
			TestObj->AddToRoot();

			SettingsModule->RegisterSettings(
				FName(*WritableContainer), FName(*WritableCategory), FName(*WritableSection),
				INVTEXT("Test Section"), INVTEXT("Automation test settings"),
				TestObj);

			ON_SCOPE_EXIT
			{
				SettingsModule->UnregisterSettings(
					FName(*WritableContainer), FName(*WritableCategory), FName(*WritableSection));
				TestObj->RemoveFromRoot();
			};

			ExceptionHandler->CaptureErrorsIn([this, &WritableContainer, &WritableCategory, &WritableSection]()
			{
				const FString Original = UConfigSettingsToolset59::GetSectionPropertyValues(
					WritableContainer, WritableCategory, WritableSection,
					{TEXT("bTestBoolProperty")});

				const bool bSet = UConfigSettingsToolset59::SetSectionProperties(
					WritableContainer, WritableCategory, WritableSection, Original);
				TestTrue(TEXT("Set succeeded"), bSet);

				const FString After = UConfigSettingsToolset59::GetSectionPropertyValues(
					WritableContainer, WritableCategory, WritableSection,
					{TEXT("bTestBoolProperty")});
				TestEqual(TEXT("Value unchanged after round-trip"), After, Original);
			});
			ExpectNoException();
		});

		It(TEXT("raises error for nonexistent section"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UConfigSettingsToolset59::SetSectionProperties(
					TestContainer, TestCategory, TEXT("NonexistentSectionXYZ12345"),
					TEXT("{\"bShouldWindowPreserveAspectRatio\": true}"));
			});
			ExpectExceptionContains(TEXT("ConfigSettingsToolset"));
		});

		It(TEXT("raises error for nonexistent property"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UConfigSettingsToolset59::SetSectionProperties(
					TestContainer, TestCategory, TestSection,
					TEXT("{\"NonexistentPropertyXYZ12345\": true}"));
			});
			ExpectExceptionContains(TEXT("could not be set"));
		});

		It(TEXT("raises error for unparseable value"), [this]()
		{
			// UE 5.8: 5.8's UToolsetLibrary::SetObjectProperties accepts a JSON object for a bool without an error
			// (5.9 fixed it, UE-402853), so there is nothing for the toolset to report.
			AddInfo(TEXT("UE 5.8's SetObjectProperties does not reject an object for a bool; skipped."));
			ExceptionHandler.Reset();
#if 0 // UE 5.8: skipped, see above
			// Uses the test settings object so a value that is wrongly accepted can't write the project's
			// DefaultGame.ini. A JSON object can't convert to a bool; a string can be coerced (UE-402853).
			ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
			if (!SettingsModule)
			{
				AddInfo(TEXT("Settings module not available; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			const FString WritableContainer = TEXT("ConfigSettingsToolsetTest");
			const FString WritableCategory  = TEXT("Test");
			const FString WritableSection   = TEXT("Unparseable");

			UConfigSettingsToolsetTestSettings59* TestObj = NewObject<UConfigSettingsToolsetTestSettings59>();
			TestObj->AddToRoot();
			SettingsModule->RegisterSettings(
				FName(*WritableContainer), FName(*WritableCategory), FName(*WritableSection),
				INVTEXT("Test Section"), INVTEXT("Automation test settings"),
				TestObj);
			ON_SCOPE_EXIT
			{
				SettingsModule->UnregisterSettings(
					FName(*WritableContainer), FName(*WritableCategory), FName(*WritableSection));
				TestObj->RemoveFromRoot();
			};

			ExceptionHandler->CaptureErrorsIn([&WritableContainer, &WritableCategory, &WritableSection]()
			{
				UConfigSettingsToolset59::SetSectionProperties(
					WritableContainer, WritableCategory, WritableSection,
					TEXT("{\"bTestBoolProperty\": {\"NotABool\": 1}}"));
			});
			ExpectExceptionContains(TEXT("could not be set"));
#endif // UE 5.8
		});
	});

	Describe(TEXT("SaveSection"), [this]()
	{
		It(TEXT("saves 'Project'/'Project'/'General' without error"), [this]()
		{
			// Saving writes the whole section into the project's DefaultGame.ini; revert it if this test
			// is what checked it out, so a dev workspace is left as it was.
			ISettingsModule* SM = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
			TSharedPtr<ISettingsContainer> Container = SM ? SM->GetContainer(FName(*TestContainer)) : nullptr;
			TSharedPtr<ISettingsCategory> Category = Container.IsValid() ? Container->GetCategory(FName(*TestCategory)) : nullptr;
			TSharedPtr<ISettingsSection> Section = Category.IsValid() ? Category->GetSection(FName(*TestSection)) : nullptr;
			const UObject* SettingsObject = Section.IsValid() ? Section->GetSettingsObject().Get() : nullptr;

			ISourceControlModule& SCModule = ISourceControlModule::Get();
			const FString ConfigPath = SettingsObject ? FPaths::ConvertRelativePathToFull(SettingsObject->GetDefaultConfigFilename()) : FString();

			// Build machines sync the file read-only with source control off, so there the save must be refused.
			if (!SCModule.IsEnabled() && !ConfigPath.IsEmpty() && IFileManager::Get().IsReadOnly(*ConfigPath))
			{
				bool bSaved = true;
				ExceptionHandler->CaptureErrorsIn([&bSaved]()
				{
					bSaved = UConfigSettingsToolset59::SaveSection(TestContainer, TestCategory, TestSection);
				});
				ExpectExceptionContains(TEXT("read-only default config file"));
				TestFalse(TEXT("Saved"), bSaved);
				return;
			}

			FSourceControlStatePtr StateBefore = (SCModule.IsEnabled() && !ConfigPath.IsEmpty())
				? SCModule.GetProvider().GetState(ConfigPath, EStateCacheUsage::ForceUpdate) : nullptr;
			const bool bRevertAfter = StateBefore.IsValid() && StateBefore->IsSourceControlled()
				&& !StateBefore->IsCheckedOut() && !StateBefore->IsAdded();
			// The cleanup reverts the file, which would also discard offline edits in a writable, unopened copy.
			if (bRevertAfter && !IFileManager::Get().IsReadOnly(*ConfigPath))
			{
				AddInfo(TEXT("Default config file is writable without being checked out and may hold local edits; skipping."));
				ExceptionHandler.Reset();
				return;
			}
			ON_SCOPE_EXIT
			{
				if (bRevertAfter)
				{
					USourceControlHelpers::RevertFiles({ ConfigPath }, /*bSilent=*/true);
				}
			};

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const bool bSaved = UConfigSettingsToolset59::SaveSection(TestContainer, TestCategory, TestSection);
				TestTrue(TEXT("Saved"), bSaved);
			});
			ExpectNoException();
		});

		It(TEXT("saves a registered writable section"), [this]()
		{
			// The test settings object saves to Saved/Config, which is writable on every machine, so the save
			// path runs even where the project's Default*.ini files are read-only.
			ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
			if (!SettingsModule)
			{
				AddInfo(TEXT("Settings module not available; skipping."));
				ExceptionHandler.Reset();
				return;
			}

			const FString WritableContainer = TEXT("ConfigSettingsToolsetTest");
			const FString WritableCategory  = TEXT("Test");
			const FString WritableSection   = TEXT("Save");

			UConfigSettingsToolsetTestSettings59* TestObj = NewObject<UConfigSettingsToolsetTestSettings59>();
			TestObj->AddToRoot();
			SettingsModule->RegisterSettings(
				FName(*WritableContainer), FName(*WritableCategory), FName(*WritableSection),
				INVTEXT("Test Section"), INVTEXT("Automation test settings"),
				TestObj);
			ON_SCOPE_EXIT
			{
				SettingsModule->UnregisterSettings(
					FName(*WritableContainer), FName(*WritableCategory), FName(*WritableSection));
				TestObj->RemoveFromRoot();
			};

			bool bSaved = false;
			ExceptionHandler->CaptureErrorsIn([&bSaved, &WritableContainer, &WritableCategory, &WritableSection]()
			{
				bSaved = UConfigSettingsToolset59::SaveSection(WritableContainer, WritableCategory, WritableSection);
			});
			ExpectNoException();
			TestTrue(TEXT("Saved"), bSaved);
		});

		It(TEXT("raises error when section does not support saving"), [this]()
		{
			FString Container, Category, Section;
			if (!FindSectionWhere([](const ISettingsSection& S){ return !S.CanSave(); },
				Container, Category, Section))
			{
				AddInfo(TEXT("No section with CanSave=false found; skipping assertion."));
				ExceptionHandler.Reset();
				return;
			}
			ExceptionHandler->CaptureErrorsIn([&]()
			{
				UConfigSettingsToolset59::SaveSection(Container, Category, Section);
			});
			ExpectExceptionContains(TEXT("ConfigSettingsToolset"));
		});

		It(TEXT("raises error for nonexistent section"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UConfigSettingsToolset59::SaveSection(
					TestContainer, TestCategory, TEXT("NonexistentSectionXYZ12345"));
			});
			ExpectExceptionContains(TEXT("ConfigSettingsToolset"));
		});
	});

	Describe(TEXT("ResetSectionToDefaults"), [this]()
	{
		It(TEXT("resets a resettable section to defaults without error"), [this]()
		{
			FString Container, Category, Section;
			if (!FindSectionWhere([](ISettingsSection& S) 
			{ 
				// Don't run this test on the sections with OnResetDefaults bound 
				// because it might expect interaction
				return S.CanResetDefaults() && !S.OnResetDefaults().IsBound(); 
			},
				Container, Category, Section))
			{
				AddInfo(TEXT("No section with CanResetDefaults=true found; skipping assertion."));
				ExceptionHandler.Reset();
				return;
			}
			ExceptionHandler->CaptureErrorsIn([&]()
			{
				const bool bReset = UConfigSettingsToolset59::ResetSectionToDefaults(Container, Category, Section);
				TestTrue(TEXT("Reset"), bReset);
			});
			ExpectNoException();
		});

		It(TEXT("raises error when section does not support reset"), [this]()
		{
			FString Container, Category, Section;
			if (!FindSectionWhere([](const ISettingsSection& S){ return !S.CanResetDefaults(); },
				Container, Category, Section))
			{
				AddInfo(TEXT("No section with CanResetDefaults=false found; skipping assertion."));
				ExceptionHandler.Reset();
				return;
			}
			ExceptionHandler->CaptureErrorsIn([&]()
			{
				UConfigSettingsToolset59::ResetSectionToDefaults(Container, Category, Section);
			});
			ExpectExceptionContains(TEXT("ConfigSettingsToolset"));
		});

		It(TEXT("raises error for nonexistent section"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UConfigSettingsToolset59::ResetSectionToDefaults(
					TestContainer, TestCategory, TEXT("NonexistentSectionXYZ12345"));
			});
			ExpectExceptionContains(TEXT("ConfigSettingsToolset"));
		});
	});
}
#endif
