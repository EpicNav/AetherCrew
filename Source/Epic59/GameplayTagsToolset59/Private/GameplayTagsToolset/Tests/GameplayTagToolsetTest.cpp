// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameplayTagsToolset/Tests/GameplayTagToolsetTest59.h"

#include "Algo/IsSorted.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Containers/Array.h"
#include "CoreMinimal.h"
#include "EditorAssetLibrary.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SourceControlHelpers.h"
#include "Templates/UniquePtr.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"
#include "UObject/UObjectGlobals.h"

#include "GameplayTagsToolset/GameplayTagsToolset59.h"
#include "GameplayTagsToolset/GameplayTagsToolsetSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	const auto TestFlags =
		EAutomationTestFlags::EditorContext |
		EAutomationTestFlags::ProductFilter |
		EAutomationTestFlags::CriticalPriority;
}

BEGIN_DEFINE_SPEC(FGameplayTagsToolsetSpec, "AIAssistant.Toolsets.GameplayTagsToolset.GameplayTagsToolsetSpec",
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
		TestTrue(
			*FString::Printf(TEXT("Error contains '%s'"), *ErrorMessage),
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

	// Add a test tag that is cleaned up after the test case.
	// Returns the name of the new tag.
	FString AddTestTag(
		const FString& TagNamePostfix,
		const FString& Comment = DefaultTagComment)
	{
		check(ExceptionHandler);
		FString TagName = TestTagName + TagNamePostfix;
		ExceptionHandler->CaptureErrorsIn(
			[this, &TagName, &Comment]()
			{
				UGameplayTagsToolset59::AddTag(TagName, Comment, TestTagSourceName.ToString());
			});
		AddedTagNames.Add(TagName);
		return TagName;
	}

	static FString GetTestTagIniDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AutomationTests") / TEXT("GameplayTags"));
	}

	// Tag writes save these files and, with source control on, check them out or mark them for add.
	struct FGuardedFile
	{
		FString Path;
		FString Contents;
		bool bExisted = false;
		bool bWasOpen = false;
		bool bWasWritable = false;
	};

	static bool IsOpenInSourceControl(const FString& Path)
	{
		if (!USourceControlHelpers::IsAvailable())
		{
			return false;
		}
		const FSourceControlState State = USourceControlHelpers::QueryFileState(Path, /*bSilent=*/true);
		return State.bIsValid && (State.bIsCheckedOut || State.bIsAdded);
	}

	void GuardFile(const FString& Path)
	{
		FGuardedFile& Guarded = GuardedFiles.AddDefaulted_GetRef();
		Guarded.Path = FPaths::ConvertRelativePathToFull(Path);
		Guarded.bExisted = FFileHelper::LoadFileToString(Guarded.Contents, *Guarded.Path);
		Guarded.bWasOpen = IsOpenInSourceControl(Guarded.Path);
		Guarded.bWasWritable = Guarded.bExisted && !IFileManager::Get().IsReadOnly(*Guarded.Path);
	}

	// Leaves files the test changed as they were: reverts the ones it opened in source control, then puts
	// back any contents that differ, which keeps local edits made to a writable file before the test.
	void RestoreGuardedFiles()
	{
		for (const FGuardedFile& Guarded : GuardedFiles)
		{
			if (Guarded.bWasOpen)
			{
				continue;
			}
			if (IsOpenInSourceControl(Guarded.Path))
			{
				USourceControlHelpers::RevertFiles({ Guarded.Path }, /*bSilent=*/true);
			}
			FString Contents;
			const bool bExists = FFileHelper::LoadFileToString(Contents, *Guarded.Path);
			// Without source control the engine clears a read-only file's flag before writing it, so restore
			// both the contents and the flag.
			if (Guarded.bExisted && (!bExists || Contents != Guarded.Contents))
			{
				IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
				PlatformFile.SetReadOnly(*Guarded.Path, false);
				FFileHelper::SaveStringToFile(Guarded.Contents, *Guarded.Path);
				PlatformFile.SetReadOnly(*Guarded.Path, !Guarded.bWasWritable);
			}
		}
		GuardedFiles.Empty();
	}

private:
	FString TestTagName;
	FString RenamedTagName;
	TArray<FString> AddedTagNames;
	TArray<FGuardedFile> GuardedFiles;
	TUniquePtr<UE::ToolsetRegistry::FToolCallExceptionHandler> ExceptionHandler;
	bool bPreviousImportTagsFromConfig = false;

private:
	static const FString NonExistentTag;
	static const FString DefaultTagComment;
	// Tag source backed by the project saved directory so writes succeed on build machines,
	// where the project config directory is read-only. AddTagIniSearchPath registers it with
	// the full writable path so AddNewGameplayTagToINI can find and use it.
	static const FName TestTagSourceName;
	// A second writable tag source used as a destination for MoveTags tests.
	static const FName TestTagSourceName2;
END_DEFINE_SPEC(FGameplayTagsToolsetSpec)

const FString FGameplayTagsToolsetSpec::NonExistentTag = TEXT("ZZZ.NonExistent.XYZABC123456");
const FString FGameplayTagsToolsetSpec::DefaultTagComment = TEXT("Automated test tag");
const FName FGameplayTagsToolsetSpec::TestTagSourceName = FName(TEXT("GameplayTagsToolsetTest.ini"));
const FName FGameplayTagsToolsetSpec::TestTagSourceName2 = FName(TEXT("GameplayTagsToolsetTest2.ini"));

void FGameplayTagsToolsetSpec::Define()
{
	using namespace UE::ToolsetRegistry;

	BeforeEach([this]()
	{
		// Register a tag INI source in the project saved directory so that
		// AddNewGameplayTagToINI writes to a location that is writable on all machines,
		// including build machines where the project config directory is read-only.
		//
		// AddTagIniSearchPath discovers .ini files in the directory and records their full
		// paths. EditorRefreshGameplayTagTree then registers each file as a tag source with
		// its correct ConfigFileName (pointing into the saved directory), so that subsequent
		// AddTag calls write to the right writable location.
		const FString TestTagIniDir = GetTestTagIniDir();
		const FString TestTagIniPath = TestTagIniDir / TestTagSourceName.ToString();
		const FString TestTagIniPath2 = TestTagIniDir / TestTagSourceName2.ToString();

		IFileManager::Get().MakeDirectory(*TestTagIniDir, /*Tree=*/true);
		if (!FPaths::FileExists(TestTagIniPath))
		{
			FFileHelper::SaveStringToFile(TEXT(""), *TestTagIniPath);
		}
		if (!FPaths::FileExists(TestTagIniPath2))
		{
			FFileHelper::SaveStringToFile(TEXT(""), *TestTagIniPath2);
		}

		GuardFile(GetDefault<UGameplayTagsSettings>()->GetDefaultConfigFilename());
		GuardFile(TestTagIniPath);
		GuardFile(TestTagIniPath2);

		UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
		Manager.AddTagIniSearchPath(TestTagIniDir);
		Manager.EditorRefreshGameplayTagTree();

		UGameplayTagsSettings* Settings = GetMutableDefault<UGameplayTagsSettings>();
		bPreviousImportTagsFromConfig = Settings->ImportTagsFromConfig;
		Settings->ImportTagsFromConfig = true;

		ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
		TestTagName = FString::Printf(
			TEXT("ToolsetTest.AutoTest%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
		RenamedTagName = TestTagName + TEXT("Renamed");
		AddedTagNames.Add(RenamedTagName);  // Clean up the renamed tag if it exists.
	});

	AfterEach([this]()
	{
		if (ExceptionHandler.IsValid()) ExpectNoException();
		// Clean up test tags.
		UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
		for (const FString& TagName : AddedTagNames)
		{
			if (Manager.FindTagNode(FName(*TagName)).IsValid())
			{
				UGameplayTagsToolset59::RemoveTag(TagName);
			}
		}
		TestTagName.Empty();
		RenamedTagName.Empty();
		AddedTagNames.Empty();

		UGameplayTagsSettings* Settings = GetMutableDefault<UGameplayTagsSettings>();
		Settings->ImportTagsFromConfig = bPreviousImportTagsFromConfig;
		RestoreGuardedFiles();
	});

	Describe(TEXT("ListTags"), [this]()
	{
		It(TEXT("Returns a sorted list"), [this]()
		{
			AddTestTag(TEXT("foo"));
			AddTestTag(TEXT("foo.bar"));
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					TArray<FString> Tags = UGameplayTagsToolset59::ListTags(FString());
					TArray<FString> Sorted = Tags;
					Sorted.Sort();
					TestEqual("Sorted", Tags, Sorted);
				});
		});

		It(TEXT("Returns empty array for a non-existent parent tag"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					TArray<FString> Tags = UGameplayTagsToolset59::ListTags(NonExistentTag);
					TestEqual("Empty", Tags.Num(), 0);
				});
		});
	});

	Describe(TEXT("GetTagInfo"), [this]()
	{
		It(TEXT("Fails for a non-existent tag"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					FGameplayTagInfo59 Info = UGameplayTagsToolset59::GetTagInfo(NonExistentTag);
#if WITH_EDITORONLY_DATA
					TestTrue("Comment empty", Info.Comment.IsEmpty());
					TestTrue("Source empty", Info.Source.IsEmpty());
#endif  // WITH_EDITORONLY_DATA
					TestEqual("No children", Info.Children.Num(), 0);
				});
			ExpectException();
		});

		It(TEXT("Returns info for an existing tag"), [this]()
		{
			FString TagName = AddTestTag(TEXT(".foo"));
			TArray<FString> Children =
			{
				AddTestTag(TEXT(".foo.bish")),
				AddTestTag(TEXT(".foo.bosh")),
			};
			ExceptionHandler->CaptureErrorsIn([this, &TagName, &Children]()
				{
					FGameplayTagInfo59 Info = UGameplayTagsToolset59::GetTagInfo(TagName);
#if WITH_EDITORONLY_DATA
					TestEqual(TEXT("Comment"), Info.Comment, DefaultTagComment);
					TestEqual(TEXT("Source"), Info.Source, TestTagSourceName.ToString());
#endif  // WITH_EDITORONLY_DATA
					if (TestEqual(TEXT("NumChildren"), Info.Children.Num(), Children.Num()))
					{
						for (int i = 0; i < Children.Num(); ++i)
						{
							TestEqual(
								*FString::Printf(TEXT("Children[%d]"), i),
								Info.Children[i], Children[i]);
						}
					}
				});
			ExpectNoException();
		});
	});

	Describe(TEXT("AddTag and RemoveTag"), [this]()
	{
		It(TEXT("Can add a new tag"), [this]()
		{
			FString TagName = AddTestTag(TEXT("test"));
			ExpectNoException();
			TestTrue(TEXT("Exists in manager"),
				UGameplayTagsManager::Get().FindTagNode(FName(*TagName)).IsValid());
		});

		It(TEXT("Fails to add empty tag"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn(
				[this]()
				{
					UGameplayTagsToolset59::AddTag(FString(), FString(), FString());
				});
			ExpectException();
		});

		It(TEXT("Fails to add a tag to a Native source"), [this]()
		{
			// Native sources have no ConfigFileName, so a write would fall back to the class
			// default config file and pollute every later tag list save.
			const FString DefaultConfigPath =
				GetDefault<UGameplayTagsList>()->GetDefaultConfigFilename();
			const bool bDefaultConfigExisted = FPaths::FileExists(DefaultConfigPath);
			const FDateTime DefaultConfigTimeStamp =
				IFileManager::Get().GetTimeStamp(*DefaultConfigPath);

			const FName NativeSourceName = FGameplayTagSource::GetNativeName();
			AddedTagNames.Add(TestTagName);
			ExceptionHandler->CaptureErrorsIn([this, &NativeSourceName]()
				{
					UGameplayTagsToolset59::AddTag(
						TestTagName, DefaultTagComment, NativeSourceName.ToString());
				});
			ExpectExceptionContains(TEXT("not a writable tag source"));
			TestFalse(TEXT("Tag was not added to manager"),
				UGameplayTagsManager::Get().FindTagNode(FName(*TestTagName)).IsValid());
			TestTrue(TEXT("Default config file untouched"),
				IFileManager::Get().GetTimeStamp(*DefaultConfigPath) == DefaultConfigTimeStamp);

			// Don't let a regression leave the poisoned file behind in the workspace.
			if (!bDefaultConfigExisted && FPaths::FileExists(DefaultConfigPath))
			{
				IFileManager::Get().Delete(*DefaultConfigPath);
			}
		});

		It(TEXT("Fails to add a tag to a tag source that does not exist"), [this]()
		{
			const FString UnknownSource = TEXT("GameplayTagsToolsetNoSuchSource.ini");
			const FString ProjectSourcePath = FPaths::SourceConfigDir() / TEXT("Tags") / UnknownSource;
			AddedTagNames.Add(TestTagName);
			ExceptionHandler->CaptureErrorsIn([this, &UnknownSource]()
				{
					UGameplayTagsToolset59::AddTag(TestTagName, DefaultTagComment, UnknownSource);
				});
			ExpectExceptionContains(TEXT("does not exist"));
			TestFalse(TEXT("Tag was not added to manager"),
				UGameplayTagsManager::Get().FindTagNode(FName(*TestTagName)).IsValid());
			TestNull(TEXT("No source was created"), UGameplayTagsManager::Get().FindTagSource(FName(*UnknownSource)));
			TestFalse(TEXT("No project tag file was written"), FPaths::FileExists(ProjectSourcePath));
		});

		It(TEXT("Suggests the source name with .ini when it is missing"), [this]()
		{
			const FString SourceWithoutIni = FPaths::GetBaseFilename(TestTagSourceName.ToString());
			AddedTagNames.Add(TestTagName);
			ExceptionHandler->CaptureErrorsIn([this, &SourceWithoutIni]()
				{
					UGameplayTagsToolset59::AddTag(TestTagName, DefaultTagComment, SourceWithoutIni);
				});
			ExpectExceptionContains(FString::Printf(TEXT("Did you mean '%s'?"), *TestTagSourceName.ToString()));
		});

		It(TEXT("Can remove an existing tag"), [this]()
		{
			FString TagName = AddTestTag(TEXT(""));
			ExceptionHandler->CaptureErrorsIn(
				[this, &TagName]()
				{
					UGameplayTagsToolset59::RemoveTag(TagName);
				});
			ExpectNoException();
			TestFalse(TEXT("No longer in manager"),
				UGameplayTagsManager::Get().FindTagNode(FName(*TagName)).IsValid());
		});

		It(TEXT("Fails to remove a non-existent tag"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn(
				[this]()
				{
					UGameplayTagsToolset59::RemoveTag(NonExistentTag);
				});
			ExpectExceptionContains(NonExistentTag);
		});
	});

	Describe(TEXT("RenameTag"), [this]()
	{
		It(TEXT("Can rename an existing tag"), [this]()
		{
			FString TagName = AddTestTag(TEXT(""));
			ExceptionHandler->CaptureErrorsIn(
				[this, &TagName]()
				{
					UGameplayTagsToolset59::RenameTag(TagName, RenamedTagName);
				});
			ExpectNoException();
			TestTrue(TEXT("New name exists"),
				UGameplayTagsManager::Get().FindTagNode(FName(*RenamedTagName)).IsValid());
		});

		It(TEXT("Fails when source tag name does not exist"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn(
				[this]()
				{
					UGameplayTagsToolset59::RenameTag(NonExistentTag, RenamedTagName);
				});
			ExpectExceptionContains(NonExistentTag);
		});

		It(TEXT("Fails with an empty source tag name"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn(
				[this]()
				{
					UGameplayTagsToolset59::RenameTag(FString(), RenamedTagName);
				});
			ExpectException();
		});

		It(TEXT("Fails with an empty target tag name"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn(
				[this]()
				{
					UGameplayTagsToolset59::RenameTag(TestTagName, FString());
				});
			ExpectException();
		});
	});

	Describe(TEXT("AddTag empty TagSource policy"), [this]()
	{
		It(TEXT("RequireNonEmptySource rejects an empty TagSource"), [this]()
		{
			UGameplayTagsToolsetSettings* Settings =
				GetMutableDefault<UGameplayTagsToolsetSettings>();
			const EGameplayTagsToolsetEmptySourcePolicy Previous = Settings->EmptySourcePolicy;
			Settings->EmptySourcePolicy =
				EGameplayTagsToolsetEmptySourcePolicy::RequireNonEmptySource;

			ExceptionHandler->CaptureErrorsIn([this]()
				{
					UGameplayTagsToolset59::AddTag(TestTagName, DefaultTagComment, FString());
				});
			ExpectExceptionContains(TEXT("TagSource"));
			TestFalse(TEXT("Tag was not added to manager"),
				UGameplayTagsManager::Get().FindTagNode(FName(*TestTagName)).IsValid());

			Settings->EmptySourcePolicy = Previous;
		});

		It(TEXT("UseDefaultSource allows an empty TagSource through the policy gate"), [this]()
		{
			UGameplayTagsToolsetSettings* Settings =
				GetMutableDefault<UGameplayTagsToolsetSettings>();
			const EGameplayTagsToolsetEmptySourcePolicy Previous = Settings->EmptySourcePolicy;
			Settings->EmptySourcePolicy =
				EGameplayTagsToolsetEmptySourcePolicy::UseDefaultSource;

			// Track for cleanup regardless of whether the underlying write succeeds, since the
			// default INI source may or may not be writable in this test environment.
			AddedTagNames.Add(TestTagName);
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					UGameplayTagsToolset59::AddTag(TestTagName, DefaultTagComment, FString());
				});
			// The policy gate must not fire — any error here would come from the underlying
			// module, not from the toolset's empty-source check.
			TestFalse(TEXT("Policy gate did not reject"),
				ExceptionHandler->GetException().Contains(TEXT("RequireNonEmptySource")));
			ExceptionHandler.Reset();

			Settings->EmptySourcePolicy = Previous;
		});
	});

	Describe(TEXT("MoveTags"), [this]()
	{
		It(TEXT("Moves a tag from one INI source to another"), [this]()
		{
			FString TagName = AddTestTag(TEXT(".movable"));

			FGameplayTagsMoveResult Result;
			ExceptionHandler->CaptureErrorsIn([this, &TagName, &Result]()
				{
					Result = UGameplayTagsToolset59::MoveTags(
						{ TagName }, TestTagSourceName2.ToString());
				});
			ExpectNoException();

			TestEqual(TEXT("TagsMoved count"), Result.TagsMoved.Num(), 1);
			TestEqual(TEXT("FailedToMoveTags count"), Result.FailedToMoveTags.Num(), 0);
			if (Result.TagsMoved.Num() == 1)
			{
				TestEqual(TEXT("Moved tag name"), Result.TagsMoved[0], TagName);
			}

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([this, &TagName]()
				{
					FGameplayTagInfo59 Info = UGameplayTagsToolset59::GetTagInfo(TagName);
#if WITH_EDITORONLY_DATA
					TestEqual(TEXT("Tag source updated"),
						Info.Source, TestTagSourceName2.ToString());
#endif  // WITH_EDITORONLY_DATA
				});
			ExpectNoException();
		});

		It(TEXT("Raises an error and reports failure for non-existent tags"), [this]()
		{
			FGameplayTagsMoveResult Result;
			ExceptionHandler->CaptureErrorsIn([this, &Result]()
				{
					Result = UGameplayTagsToolset59::MoveTags(
						{ NonExistentTag }, TestTagSourceName2.ToString());
				});
			// The module surfaces the per-tag failure as an error message, which the toolset
			// forwards via RaiseScriptError. The result struct still records the failed tag.
			ExpectExceptionContains(NonExistentTag);

			TestEqual(TEXT("TagsMoved count"), Result.TagsMoved.Num(), 0);
			TestEqual(TEXT("FailedToMoveTags count"), Result.FailedToMoveTags.Num(), 1);
			if (Result.FailedToMoveTags.Num() == 1)
			{
				TestEqual(TEXT("Failed tag name"),
					Result.FailedToMoveTags[0], NonExistentTag);
			}
		});

		It(TEXT("Fails with an empty TagsToMove list"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					UGameplayTagsToolset59::MoveTags(
						TArray<FString>(), TestTagSourceName2.ToString());
				});
			ExpectExceptionContains(TEXT("TagsToMove"));
		});

		It(TEXT("Fails with an empty TargetTagSource"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					UGameplayTagsToolset59::MoveTags({ TestTagName }, FString());
				});
			ExpectExceptionContains(TEXT("TargetTagSource"));
		});

		It(TEXT("Fails when TargetTagSource is not a registered source"), [this]()
		{
			const FString UnknownSource = TEXT("ZZZ.NotRegistered.NoSuchSource.ini");
			ExceptionHandler->CaptureErrorsIn([this, &UnknownSource]()
				{
					UGameplayTagsToolset59::MoveTags({ TestTagName }, UnknownSource);
				});
			ExpectExceptionContains(UnknownSource);
		});
	});

	Describe(TEXT("Tag sources"), [this]()
	{
		It(TEXT("ListTagSources returns the test source as a sorted tag list"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					const TArray<FGameplayTagSourceInfo> Sources = UGameplayTagsToolset59::ListTagSources();
					const FGameplayTagSourceInfo* TestSource = Sources.FindByPredicate(
						[this](const FGameplayTagSourceInfo& Source) { return Source.Name == TestTagSourceName.ToString(); });
					if (TestNotNull(TEXT("Test source listed"), TestSource))
					{
						TestEqual(TEXT("Test source type"), TestSource->SourceType, EGameplayTagSourceType::TagList);
						TestTrue(TEXT("Test source config file"),
							FPaths::IsSamePath(FPaths::GetPath(TestSource->ConfigFileName), GetTestTagIniDir()));
					}
					TestTrue(TEXT("Sorted"), Algo::IsSorted(Sources,
						[](const FGameplayTagSourceInfo& A, const FGameplayTagSourceInfo& B) { return A.Name < B.Name; }));
				});
			ExpectNoException();
		});

		It(TEXT("ListTagSourceRoots includes the test directory"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					const TArray<FString> Roots = UGameplayTagsToolset59::ListTagSourceRoots();
					TestTrue(TEXT("Test directory listed"), Roots.ContainsByPredicate(
						[this](const FString& Root) { return FPaths::IsSamePath(Root, GetTestTagIniDir()); }));
				});
			ExpectNoException();
		});

		It(TEXT("AddTagSource creates a source in the chosen root that tags can be added to"), [this]()
		{
			const FString NewSourceBase = FString::Printf(
				TEXT("GameplayTagsToolsetNew%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
			const FString NewSourceName = NewSourceBase + TEXT(".ini");
			const FString NewSourcePath = GetTestTagIniDir() / NewSourceName;
			ON_SCOPE_EXIT
			{
				for (const FString& TagName : AddedTagNames)
				{
					if (UGameplayTagsManager::Get().FindTagNode(FName(*TagName)).IsValid())
					{
						UGameplayTagsToolset59::RemoveTag(TagName);
					}
				}
				AddedTagNames.Empty();
				// Writing the source marks the file for add when source control is on.
				if (USourceControlHelpers::IsAvailable())
				{
					USourceControlHelpers::RevertFiles({ NewSourcePath }, /*bSilent=*/true);
				}
				IFileManager::Get().Delete(*NewSourcePath);
				UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();
			};

			ExceptionHandler->CaptureErrorsIn([this, &NewSourceBase, &NewSourceName]()
				{
					const FGameplayTagSourceInfo Added = UGameplayTagsToolset59::AddTagSource(NewSourceBase, GetTestTagIniDir());
					TestEqual(TEXT(".ini appended"), Added.Name, NewSourceName);
					TestEqual(TEXT("New source type"), Added.SourceType, EGameplayTagSourceType::TagList);
					TestTrue(TEXT("Created in the chosen root"),
						FPaths::IsSamePath(FPaths::GetPath(Added.ConfigFileName), GetTestTagIniDir()));

					const FGameplayTagSourceInfo AddedAgain = UGameplayTagsToolset59::AddTagSource(NewSourceName, GetTestTagIniDir());
					TestEqual(TEXT("Adding again returns the same source"), AddedAgain.ConfigFileName, Added.ConfigFileName);

					UGameplayTagsToolset59::AddTag(TestTagName, DefaultTagComment, NewSourceName);
				});
			AddedTagNames.Add(TestTagName);
			ExpectNoException();
			TestTrue(TEXT("Tag added to the new source"),
				UGameplayTagsManager::Get().FindTagNode(FName(*TestTagName)).IsValid());
			TestTrue(TEXT("New source file written in the chosen root"), FPaths::FileExists(NewSourcePath));
		});

		It(TEXT("AddTagSource rejects a directory that is not a tag source root"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
				{
					UGameplayTagsToolset59::AddTagSource(TEXT("GameplayTagsToolsetUnused.ini"), FPaths::ProjectSavedDir());
				});
			ExpectExceptionContains(TEXT("is not a tag source directory"));
		});

		It(TEXT("AddTagSource rejects a name taken by another kind of source"), [this]()
		{
			const FName DefaultSourceName = FGameplayTagSource::GetDefaultName();
			if (!UGameplayTagsManager::Get().FindTagSource(DefaultSourceName))
			{
				AddInfo(TEXT("No default tag source registered; skipping."));
				ExceptionHandler.Reset();
				return;
			}
			ExceptionHandler->CaptureErrorsIn([&DefaultSourceName]()
				{
					UGameplayTagsToolset59::AddTagSource(DefaultSourceName.ToString(), FString());
				});
			ExpectExceptionContains(TEXT("already exists"));
		});

		It(TEXT("AddTagSource rejects a name with path components"), [this]()
		{
			for (const TCHAR* PathName : { TEXT("../GameplayTagsToolsetEscape.ini"), TEXT("Sub/GameplayTagsToolsetEscape.ini") })
			{
				ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
				ExceptionHandler->CaptureErrorsIn([PathName]()
					{
						UGameplayTagsToolset59::AddTagSource(PathName, GetTestTagIniDir());
					});
				ExpectExceptionContains(TEXT("must be a file name"));
				TestNull(TEXT("No source registered"), UGameplayTagsManager::Get().FindTagSource(FName(PathName)));
			}
		});

		It(TEXT("AddTagSource fails with an empty name"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
				{
					UGameplayTagsToolset59::AddTagSource(FString(), FString());
				});
			ExpectExceptionContains(TEXT("SourceName"));
		});
	});

	Describe(TEXT("ListTagsInSource"), [this]()
	{
		It(TEXT("Fails with an empty TagSource"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					UGameplayTagsToolset59::ListTagsInSource(FString());
				});
			ExpectExceptionContains(TEXT("TagSource"));
		});

		It(TEXT("Fails for a non-existent source"), [this]()
		{
			const FString UnknownSource = TEXT("ZZZ.NoSuchTagSource.ini");
			ExceptionHandler->CaptureErrorsIn([this, &UnknownSource]()
				{
					UGameplayTagsToolset59::ListTagsInSource(UnknownSource);
				});
			ExpectExceptionContains(UnknownSource);
		});

		It(TEXT("Returns empty list for a registered source with no tags"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					TArray<FString> Tags =
						UGameplayTagsToolset59::ListTagsInSource(TestTagSourceName2.ToString());
					TestEqual(TEXT("Empty"), Tags.Num(), 0);
				});
			ExpectNoException();
		});

		It(TEXT("Returns a sorted list"), [this]()
		{
			AddTestTag(TEXT(".z_last"));
			AddTestTag(TEXT(".a_first"));
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					TArray<FString> Tags =
						UGameplayTagsToolset59::ListTagsInSource(TestTagSourceName.ToString());
					TArray<FString> Sorted = Tags;
					Sorted.Sort();
					TestEqual(TEXT("Sorted"), Tags, Sorted);
				});
			ExpectNoException();
		});

		It(TEXT("Returns only tags defined in the given source"), [this]()
		{
			FString TagInSource1 = AddTestTag(TEXT(".in_s1"));
			FString TagInSource2 = TestTagName + TEXT(".in_s2");
			AddedTagNames.Add(TagInSource2);
			ExceptionHandler->CaptureErrorsIn([this, &TagInSource2]()
				{
					UGameplayTagsToolset59::AddTag(
						TagInSource2, DefaultTagComment, TestTagSourceName2.ToString());
				});
			ExpectNoException();

			ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
			ExceptionHandler->CaptureErrorsIn([this, &TagInSource1, &TagInSource2]()
				{
					TArray<FString> Tags =
						UGameplayTagsToolset59::ListTagsInSource(TestTagSourceName.ToString());
					TestTrue(TEXT("Contains source1 tag"), Tags.Contains(TagInSource1));
					TestFalse(TEXT("Excludes source2 tag"), Tags.Contains(TagInSource2));
				});
			ExpectNoException();
		});
	});

	Describe(TEXT("FindReferencersByTag"), [this]()
	{
		It(TEXT("Returns empty array for a tag with no referencers"), [this]()
		{
			FString TagName = AddTestTag(TEXT(".norefs"));
			ExceptionHandler->CaptureErrorsIn([this, &TagName]()
				{
					TArray<FString> Referencers =
						UGameplayTagsToolset59::FindReferencersByTag(TagName);
					TestEqual("Empty", Referencers.Num(), 0);
				});
			ExpectNoException();
		});

		It(TEXT("Fails for a non-existent tag"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					TArray<FString> Referencers =
						UGameplayTagsToolset59::FindReferencersByTag(NonExistentTag);
					TestEqual("Empty", Referencers.Num(), 0);
				});
			ExpectExceptionContains(NonExistentTag);
		});

		It(TEXT("Fails with an empty tag name"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
				{
					UGameplayTagsToolset59::FindReferencersByTag(FString());
				});
			ExpectException();
		});

		It(TEXT("Returns referencers for a tag used by a saved asset"), [this]()
		{
			FString TagName = AddTestTag(TEXT(".hasrefs"));

			// Mount a transient content path for test assets.
			const FString MountPoint = TEXT("/Automation/");
			FPackageName::RegisterMountPoint(
				*MountPoint, FPaths::AutomationTransientDir());

			// Create a minimal asset that contains our test tag via
			// the editor asset pipeline (avoids data validation issues
			// with raw UPackage::SavePackage).
			const FString AssetName = TEXT("TagRefTestAsset");
			const FString PackagePath = MountPoint + AssetName;
			IAssetTools& AssetTools =
				FAssetToolsModule::GetModule().Get();
			UObject* CreatedAsset = AssetTools.CreateAsset(
				AssetName, MountPoint,
				UGameplayTagTestAsset59::StaticClass(), nullptr);
			if (!TestNotNull(TEXT("Asset created"), CreatedAsset))
			{
				return;
			}

			UGameplayTagTestAsset59* TestAsset =
				CastChecked<UGameplayTagTestAsset59>(CreatedAsset);
			TestAsset->Tags.AddTag(
				UGameplayTagsManager::Get().RequestGameplayTag(
					FName(*TagName)));

			// Save through the editor library so the asset registry
			// picks up the searchable name.
			UEditorAssetLibrary::SaveAsset(PackagePath);

			// Ensure the asset registry has processed the saved file's
			// searchable name dependencies before we query.
			const FString FilePath =
				FPaths::AutomationTransientDir() / TEXT("TagRefTestAsset.uasset");
			IAssetRegistry& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
					TEXT("AssetRegistry")).Get();
			AssetRegistry.ScanFilesSynchronous({FilePath}, true /* bForceRescan */);

			// Query and verify. FindReferencersByTag returns full /Pkg.Asset object paths.
			const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
			ExceptionHandler->CaptureErrorsIn([this, &TagName, &ObjectPath]()
				{
					TArray<FString> Referencers =
						UGameplayTagsToolset59::FindReferencersByTag(TagName);
					TestTrue("Has referencers", Referencers.Num() > 0);
					TestTrue("Contains test asset",
						Referencers.Contains(ObjectPath));
				});
			ExpectNoException();

			// Cleanup.
			UEditorAssetLibrary::DeleteAsset(PackagePath);
			FPackageName::UnRegisterMountPoint(*MountPoint,
				FPaths::AutomationTransientDir());
		});
	});
}

#endif  // WITH_DEV_AUTOMATION_TESTS
