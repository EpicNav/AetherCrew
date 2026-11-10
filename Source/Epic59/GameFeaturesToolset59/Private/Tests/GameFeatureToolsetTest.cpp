// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetRegistry/AssetRegistryModule.h"
#include "GameFeatureAction.h"
#include "GameFeatureData.h"
#include "GameFeaturesToolset59.h"
#include "GameFeaturesSubsystem.h"
#include "GameFeaturesSubsystemSettings.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "SourceControlHelpers.h"
#include "Templates/SharedPointer.h"
#include "Tests/GameFeaturePluginTestHelper.h"
#include "AsyncFunctionResult/AsyncFunctionResultBool.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	const EAutomationTestFlags TestFlags =
		EAutomationTestFlags::EditorContext |
		EAutomationTestFlags::ProductFilter |
		EAutomationTestFlags::CriticalPriority;
}

BEGIN_DEFINE_SPEC(FGameFeaturesToolsetSpec, "AIAssistant.Toolsets.GameFeatureToolset.GameFeaturesToolsetSpec",
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

// Ensure a transition tool handed back a result that has already failed, rather than a pending
// result for work that never started. The result is the only failure channel for these tools, so
// the reason has to arrive on it rather than as a raised script error.
void ExpectAlreadyFailed(UAsyncFunctionResultBool* Result, const FString& ExpectedError)
{
	if (!TestNotNull(TEXT("Result"), Result))
	{
		return;
	}
	TestTrue(TEXT("Complete"), Result->bIsComplete);
	TestTrue(
		*FString::Printf(TEXT("Error contains '%s'"), *ExpectedError),
		Result->Error.Contains(ExpectedError));
}

// Name of the throwaway plugin the transition tests drive.
static const TCHAR* SyntheticPluginName() { return TEXT("GFPToolsetTestTarget"); }

FString SyntheticPluginDir() const
{
	return FPaths::ProjectPluginsDir() / TEXT("GameFeatures") / SyntheticPluginName();
}

// Writes a content-free Game Feature Plugin to disk and registers it so the toolset can resolve it
// by name. Returns the plugin name, or an empty string when creation failed.
FString CreateSyntheticPlugin()
{
	UE::GameFeatures::Testing::FGameFeatureProperties Properties;
	Properties.PluginName = SyntheticPluginName();
	Properties.BuiltinAutoState = EGameFeatureTargetState::Installed;

	FString PluginURL;
	if (!UE::GameFeatures::Testing::CreateGameFeaturePlugin(Properties, PluginURL))
	{
		AddError(TEXT("Failed to write the synthetic Game Feature Plugin."));
		return FString();
	}

	IPluginManager& PluginManager = IPluginManager::Get();
	PluginManager.AddToPluginsList(SyntheticPluginDir() / (FString(SyntheticPluginName()) + TEXT(".uplugin")));
	PluginManager.RefreshPluginsList();

	// Registering with the plugin manager is not enough. The subsystem only learns a plugin's name
	// once it processes that plugin, so a transition has to be kicked off by URL before the toolset,
	// which takes names, can find it at all.
	UGameFeaturesSubsystem* Subsystem = GEngine ?
		GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
	if (!Subsystem)
	{
		AddError(TEXT("GameFeaturesSubsystem was not available."));
		return FString();
	}

	SyntheticPluginURL = PluginURL;
	Subsystem->ChangeGameFeatureTargetState(
		PluginURL,
		EGameFeatureTargetState::Installed,
		FGameFeaturePluginChangeStateComplete());

	return SyntheticPluginName();
}

// Whether the subsystem can resolve the synthetic plugin by name yet.
bool IsSyntheticPluginResolvableByName() const
{
	UGameFeaturesSubsystem* Subsystem = GEngine ?
		GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;

	FString UnusedURL;
	return Subsystem && Subsystem->GetPluginURLByName(SyntheticPluginName(), UnusedURL);
}

void RemoveSyntheticPluginFiles()
{
	IFileManager::Get().DeleteDirectory(*SyntheticPluginDir(), /*bRequireExists*/ false, /*Tree*/ true);
	IPluginManager::Get().RefreshPluginsList();
	SyntheticPluginURL.Reset();
}

// Takes the synthetic plugin back out of the subsystem before deleting its files. Terminating is the
// only thing that clears the subsystem's name mapping, so deleting the files first would leave the
// rest of the session able to resolve a plugin that no longer exists. Waiting for the name to stop
// resolving also guarantees no transition is still in flight over files about to be removed.
void TearDownSyntheticPlugin(const FDoneDelegate& Done)
{
	UGameFeaturesSubsystem* Subsystem = GEngine ?
		GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;

	if (!Subsystem || SyntheticPluginURL.IsEmpty())
	{
		RemoveSyntheticPluginFiles();
		Done.Execute();
		return;
	}

	Subsystem->TerminateGameFeaturePlugin(SyntheticPluginURL);

	const FDateTime Deadline = FDateTime::UtcNow() + FTimespan::FromSeconds(60.0);
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Done, Deadline]()
	{
		const bool bStillKnown = IsSyntheticPluginResolvableByName();
		if (bStillKnown && FDateTime::UtcNow() <= Deadline)
		{
			return false;
		}

		if (bStillKnown)
		{
			// Deleting now would strand a plugin that resolves to files which no longer exist, which
			// is the very state this teardown exists to avoid. Leaving them keeps the subsystem's view
			// honest, and the next CreateGameFeaturePlugin removes the directory anyway, so a stale
			// directory self-heals where a stranded name mapping would not.
			AddError(TEXT("The synthetic plugin was still known to the subsystem after terminating. "
				"Leaving its files in place rather than stranding a plugin that resolves to nothing."));
		}
		else
		{
			RemoveSyntheticPluginFiles();
		}

		Done.Execute();
		return true;
	}));
}

// Ensure a completed transition reported success rather than an error.
void ExpectTransitionSucceeded(const FString& What, UAsyncFunctionResultBool* Result)
{
	if (!TestNotNull(*FString::Printf(TEXT("%s result"), *What), Result))
	{
		return;
	}
	TestEqual(*FString::Printf(TEXT("%s error"), *What), Result->Error, FString());
	TestTrue(*FString::Printf(TEXT("%s value"), *What), Result->Value);
}

using FTransitionCall = TFunction<UAsyncFunctionResultBool*(const FString&)>;

// Runs two transitions in sequence against the synthetic plugin, asserting each reports success,
// then completes the test. A single latent command advances every step, so nothing is enqueued from
// inside another command and the enqueue stays owned by the LatentIt that started it.
void RunTransitionPair(
	const FDoneDelegate& Done,
	FTransitionCall First,
	const FString& FirstName,
	FTransitionCall Second,
	const FString& SecondName)
{
	const FString PluginName = CreateSyntheticPlugin();
	if (PluginName.IsEmpty())
	{
		Done.Execute();
		return;
	}

	struct FTransitionPairState
	{
		int32 Step = 0;
		TStrongObjectPtr<UAsyncFunctionResultBool> Result;
		FDateTime Deadline;
	};

	TSharedRef<FTransitionPairState> State = MakeShared<FTransitionPairState>();
	State->Deadline = FDateTime::UtcNow() + FTimespan::FromSeconds(75.0);

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(
		[this, Done, PluginName, First, FirstName, Second, SecondName, State]() -> bool
		{
			if (FDateTime::UtcNow() > State->Deadline)
			{
				AddError(TEXT("A transition did not complete before the deadline."));
				Done.Execute();
				return true;
			}

			switch (State->Step)
			{
			case 0:
				if (!IsSyntheticPluginResolvableByName())
				{
					return false;
				}
				++State->Step;
				return false;

			case 1:
				State->Result.Reset(First(PluginName));
				++State->Step;
				return false;

			case 2:
				if (!State->Result.IsValid() || !State->Result->bIsComplete)
				{
					return false;
				}
				ExpectTransitionSucceeded(FirstName, State->Result.Get());
				State->Result.Reset(Second(PluginName));
				++State->Step;
				return false;

			default:
				if (!State->Result.IsValid() || !State->Result->bIsComplete)
				{
					return false;
				}
				ExpectTransitionSucceeded(SecondName, State->Result.Get());
				Done.Execute();
				return true;
			}
		}));
}

// Returns false when the descriptor file is read-only and source control is not
// available to check it out -- typical of unattended CI runs where the workspace is
// synced read-only and no Perforce client is configured. Used to skip the descriptor
// write-path tests cleanly in that environment. Mirrors PluginToolsetTest's helper.
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

	// URL of the synthetic plugin, needed to terminate it during teardown.
	FString SyntheticPluginURL;

END_DEFINE_SPEC(FGameFeaturesToolsetSpec)

void FGameFeaturesToolsetSpec::Define()
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

	Describe(TEXT("ListEnabledGameFeaturePlugins"), [this]()
	{
		It(TEXT("returns a sorted list"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UGameFeaturesToolset59::ListEnabledGameFeaturePlugins();
				for (int32 i = 1; i < Names.Num(); ++i)
				{
					TestTrue(TEXT("Sorted"), Names[i - 1] <= Names[i]);
				}
			});
			ExpectNoException();
		});

		It(TEXT("does not include non-game features plugins"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UGameFeaturesToolset59::ListEnabledGameFeaturePlugins();
				// ToolsetRegistry is not a game feature plugin
				TestFalse(TEXT("ToolsetRegistry excluded"), Names.Contains(TEXT("ToolsetRegistry")));
			});
			ExpectNoException();
		});

		It(TEXT("only includes plugins that are enabled by the plugin manager"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UGameFeaturesToolset59::ListEnabledGameFeaturePlugins();
				TSet<FString> DiscoveredNames;
				for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
				{
					DiscoveredNames.Add(Plugin->GetName());
				}
				for (const FString& Name : Names)
				{
					TestTrue(
						FString::Printf(TEXT("'%s' is a discovered plugin"), *Name),
						DiscoveredNames.Contains(Name));
				}
			});
			ExpectNoException();
		});


		It(TEXT("only includes plugins known to game features subsystem"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UGameFeaturesSubsystem* Subsystem = GEngine ?
					GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
				if (TestNotNull(TEXT("game features subsystem"), Subsystem))
				{
					check(Subsystem); // Silence static analysis warning about Subsystem being null.
					const TArray<FString> Names = UGameFeaturesToolset59::ListEnabledGameFeaturePlugins();
					for (const FString& Name : Names)
					{
						FString PluginURL;
						TestTrue(
							FString::Printf(TEXT("GameFeaturesSubsystem GetPluginURLByName succeeded for '%s'"), *Name), 
							Subsystem->GetPluginURLByName(Name, PluginURL));
					}
				}
			});
			ExpectNoException();
		});
	});

	Describe(TEXT("ListDiscoveredGameFeaturePlugins"), [this]()
	{
		It(TEXT("returns a sorted list"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UGameFeaturesToolset59::ListDiscoveredGameFeaturePlugins();
				for (int32 i = 1; i < Names.Num(); ++i)
				{
					TestTrue(TEXT("Sorted"), Names[i - 1] <= Names[i]);
				}
			});
			ExpectNoException();
		});

		It(TEXT("does not include non-game features plugins"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UGameFeaturesToolset59::ListDiscoveredGameFeaturePlugins();
				// ToolsetRegistry is not a game feature plugin
				TestFalse(TEXT("ToolsetRegistry excluded"), Names.Contains(TEXT("ToolsetRegistry")));
			});
			ExpectNoException();
		});

		It(TEXT("only includes plugins that are discovered by the plugin manager"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FString> Names = UGameFeaturesToolset59::ListDiscoveredGameFeaturePlugins();
				TSet<FString> DiscoveredNames;
				for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
				{
					DiscoveredNames.Add(Plugin->GetName());
				}
				for (const FString& Name : Names)
				{
					TestTrue(
						FString::Printf(TEXT("'%s' is a discovered plugin"), *Name),
						DiscoveredNames.Contains(Name));
				}
			});
			ExpectNoException();
		});
	});

	Describe(TEXT("IsGameFeaturePlugin"), [this]()
	{
		It(TEXT("Nonexistent plugin produces an error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TestFalse(TEXT("returned state"),
					UGameFeaturesToolset59::IsGameFeaturePlugin(TEXT("UnknownTestPluginXYZ123")));
			});
			ExpectExceptionContains(TEXT("GameFeaturesToolset"));
		});

		It(TEXT("Non game feature plugin returns false"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TestFalse(TEXT("returned state"),
					UGameFeaturesToolset59::IsGameFeaturePlugin(TEXT("ToolsetRegistry")));
			});
			ExpectNoException();
		});

		It(TEXT("active game feature plugin produces true"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UGameFeaturesSubsystem* Subsystem = GEngine ?
					GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;

				if (TestNotNull(TEXT("game features subsystem"), Subsystem))
				{
					check(Subsystem); // Silence static analysis warning about Subsystem being null.
					Subsystem->ForEachActiveGameFeature<UGameFeatureData>(
						[this](const UGameFeatureData* Data)
					{
						if (TestNotNull(TEXT("game feature data"), Data))
						{
							FString PluginName;
							Data->GetPluginName(PluginName);
							TestTrue(TEXT("returned state"),
								UGameFeaturesToolset59::IsGameFeaturePlugin(PluginName));
						}
					}
					);
				}
			});
			ExpectNoException();
		});
	});

	Describe(TEXT("GetGameFeatureState"), [this]()
	{
		It(TEXT("Nonexistent plugin produces an error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TestEqual(TEXT("returned state"), EPluginToolsetGFPState59::Unknown,
					UGameFeaturesToolset59::GetGameFeatureState(TEXT("UnknownTestPluginXYZ123")));
			});
			ExpectExceptionContains(TEXT("GameFeaturesToolset"));
		});

		It(TEXT("Non game feature plugin produces an error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TestEqual(TEXT("returned state"), EPluginToolsetGFPState59::Unknown,
					UGameFeaturesToolset59::GetGameFeatureState(TEXT("ToolsetRegistry")));
			});
			ExpectExceptionContains(TEXT("GameFeaturesToolset"));
		});

		It(TEXT("active game feature plugin produces active result"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UGameFeaturesSubsystem* Subsystem = GEngine ?
					GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;

				if (TestNotNull(TEXT("game features subsystem"), Subsystem))
				{
					check(Subsystem); // Silence static analysis warning about Subsystem being null.
					Subsystem->ForEachActiveGameFeature<UGameFeatureData>(
						[this](const UGameFeatureData* Data)
					{
						if (TestNotNull(TEXT("game feature data"), Data))
						{
							FString PluginName;
							Data->GetPluginName(PluginName);
							TestEqual(TEXT("returned state"), EPluginToolsetGFPState59::Active,
								UGameFeaturesToolset59::GetGameFeatureState(PluginName));
							TestTrue(TEXT("returned active result"),
								UGameFeaturesToolset59::IsGameFeatureActive(PluginName));
						}
					}
					);
				}
			});
			ExpectNoException();
		});
	});

	// The transition tools hand back a pending result rather than a bool, so an unknown plugin has
	// to come back as a result that is already complete and carrying an error. Reporting success
	// for a transition that never started is the regression these guard.
	Describe(TEXT("RequestActivateGameFeature"), [this]()
	{
		It(TEXT("Nonexistent plugin produces an already-failed result"), [this]()
		{
			TStrongObjectPtr<UAsyncFunctionResultBool> Result;
			ExceptionHandler->CaptureErrorsIn([&Result]()
			{
				Result.Reset(UGameFeaturesToolset59::RequestActivateGameFeature(TEXT("UnknownTestPluginXYZ123")));
			});
			ExpectNoException();
			ExpectAlreadyFailed(Result.Get(), TEXT("GFP not found"));
		});
	});

	Describe(TEXT("RequestLoadGameFeature"), [this]()
	{
		It(TEXT("Nonexistent plugin produces an already-failed result"), [this]()
		{
			TStrongObjectPtr<UAsyncFunctionResultBool> Result;
			ExceptionHandler->CaptureErrorsIn([&Result]()
			{
				Result.Reset(UGameFeaturesToolset59::RequestLoadGameFeature(TEXT("UnknownTestPluginXYZ123")));
			});
			ExpectNoException();
			ExpectAlreadyFailed(Result.Get(), TEXT("GFP not found"));
		});
	});

	Describe(TEXT("RequestDeactivateGameFeature"), [this]()
	{
		It(TEXT("Nonexistent plugin produces an already-failed result"), [this]()
		{
			TStrongObjectPtr<UAsyncFunctionResultBool> Result;
			ExceptionHandler->CaptureErrorsIn([&Result]()
			{
				Result.Reset(UGameFeaturesToolset59::RequestDeactivateGameFeature(TEXT("UnknownTestPluginXYZ123")));
			});
			ExpectNoException();
			ExpectAlreadyFailed(Result.Get(), TEXT("GFP not found"));
		});
	});

	Describe(TEXT("RequestUnloadGameFeature"), [this]()
	{
		It(TEXT("Nonexistent plugin produces an already-failed result"), [this]()
		{
			TStrongObjectPtr<UAsyncFunctionResultBool> Result;
			ExceptionHandler->CaptureErrorsIn([&Result]()
			{
				Result.Reset(UGameFeaturesToolset59::RequestUnloadGameFeature(TEXT("UnknownTestPluginXYZ123")));
			});
			ExpectNoException();
			ExpectAlreadyFailed(Result.Get(), TEXT("GFP not found"));
		});

		It(TEXT("Nonexistent plugin produces an already-failed result when keeping it registered"), [this]()
		{
			TStrongObjectPtr<UAsyncFunctionResultBool> Result;
			ExceptionHandler->CaptureErrorsIn([&Result]()
			{
				Result.Reset(UGameFeaturesToolset59::RequestUnloadGameFeature(TEXT("UnknownTestPluginXYZ123"), /*bKeepRegistered=*/true));
			});
			ExpectNoException();
			ExpectAlreadyFailed(Result.Get(), TEXT("GFP not found"));
		});
	});

	// Success-path coverage needs a real plugin to drive, so these build a throwaway content-free GFP
	// and register it so the toolset can resolve it by name. Transitions are paired because the second
	// of each pair needs the state the first leaves behind.
	Describe(TEXT("Transitions against a synthetic Game Feature Plugin"), [this]()
	{
		LatentAfterEach(FTimespan::FromSeconds(90.0), [this](const FDoneDelegate& Done)
		{
			TearDownSyntheticPlugin(Done);
		});

		LatentIt(TEXT("Load then Unload both report success"), FTimespan::FromSeconds(120.0),
			[this](const FDoneDelegate& Done)
		{
			RunTransitionPair(
				Done,
				[](const FString& Name) { return UGameFeaturesToolset59::RequestLoadGameFeature(Name); },
				TEXT("Load"),
				[](const FString& Name) { return UGameFeaturesToolset59::RequestUnloadGameFeature(Name); },
				TEXT("Unload"));
		});

		LatentIt(TEXT("Activate then Deactivate both report success"), FTimespan::FromSeconds(120.0),
			[this](const FDoneDelegate& Done)
		{
			RunTransitionPair(
				Done,
				[](const FString& Name) { return UGameFeaturesToolset59::RequestActivateGameFeature(Name); },
				TEXT("Activate"),
				[](const FString& Name) { return UGameFeaturesToolset59::RequestDeactivateGameFeature(Name); },
				TEXT("Deactivate"));
		});
	});

	Describe(TEXT("GetGameFeaturePluginDetails"), [this]()
	{
		It(TEXT("Nonexistent plugin produces an error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UGameFeaturesToolset59::GetGameFeaturePluginDetails(TEXT("UnknownTestPluginXYZ123"));
			});
			ExpectExceptionContains(TEXT("GameFeaturesToolset"));
		});

		It(TEXT("Non game feature plugin produces an error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UGameFeaturesToolset59::GetGameFeaturePluginDetails(TEXT("ToolsetRegistry"));
			});
			ExpectExceptionContains(TEXT("GameFeaturesToolset"));
		});

		It(TEXT("active game feature plugin returns sensible details"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UGameFeaturesSubsystem* Subsystem = GEngine ?
					GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;

				if (TestNotNull(TEXT("game features subsystem"), Subsystem))
				{
					check(Subsystem);
					Subsystem->ForEachActiveGameFeature<UGameFeatureData>(
						[this](const UGameFeatureData* Data)
					{
						if (TestNotNull(TEXT("game feature data"), Data))
						{
							FString PluginName;
							Data->GetPluginName(PluginName);
							const FGameFeaturePluginDetailsToolsetInfo Info =
								UGameFeaturesToolset59::GetGameFeaturePluginDetails(PluginName);
							// An active GFP must have resolved to a non-Invalid state.
							TestNotEqual(TEXT("BuiltInAutoState resolved"),
								Info.BuiltInAutoState, EPluginToolsetGFPAutoState::Invalid);
							// AdditionalMetadata contains exactly the configured keys.
							const TArray<FString>& ConfiguredKeys =
								GetDefault<UGameFeaturesSubsystemSettings>()->AdditionalPluginMetadataKeys;
							TestEqual(TEXT("AdditionalMetadata size matches configured keys"),
								Info.AdditionalMetadata.Num(), ConfiguredKeys.Num());
							for (const FString& Key : ConfiguredKeys)
							{
								TestTrue(
									FString::Printf(TEXT("AdditionalMetadata contains '%s'"), *Key),
									Info.AdditionalMetadata.Contains(Key));
							}
						}
					}
					);
				}
			});
			ExpectNoException();
		});
	});

	Describe(TEXT("GetConfiguredAdditionalMetadataKeys"), [this]()
	{
		It(TEXT("matches the GameFeatures subsystem settings"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TArray<FString> Expected =
					GetDefault<UGameFeaturesSubsystemSettings>()->AdditionalPluginMetadataKeys;
				Expected.Sort();
				const TArray<FString> Actual =
					UGameFeaturesToolset59::GetConfiguredAdditionalMetadataKeys();
				TestEqual(TEXT("Count"), Actual.Num(), Expected.Num());
				for (int32 i = 0; i < Actual.Num() && i < Expected.Num(); ++i)
				{
					TestEqual(TEXT("Sorted key matches"), Actual[i], Expected[i]);
				}
			});
			ExpectNoException();
		});
	});

	Describe(TEXT("SetGameFeaturePluginBuiltInAutoState"), [this]()
	{
		It(TEXT("Invalid state produces an error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UGameFeaturesToolset59::SetGameFeaturePluginBuiltInAutoState(
					TEXT("UnknownTestPluginXYZ123"),
					EPluginToolsetGFPAutoState::Invalid);
			});
			ExpectExceptionContains(TEXT("Invalid"));
		});

		It(TEXT("Nonexistent plugin produces an error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UGameFeaturesToolset59::SetGameFeaturePluginBuiltInAutoState(
					TEXT("UnknownTestPluginXYZ123"),
					EPluginToolsetGFPAutoState::Loaded);
			});
			ExpectExceptionContains(TEXT("GameFeaturesToolset"));
		});

		It(TEXT("Non game feature plugin produces an error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UGameFeaturesToolset59::SetGameFeaturePluginBuiltInAutoState(
					TEXT("ToolsetRegistry"),
					EPluginToolsetGFPAutoState::Loaded);
			});
			ExpectExceptionContains(TEXT("GameFeaturesToolset"));
		});

		It(TEXT("round-trips a state change against an active GFP when the descriptor is modifiable"), [this]()
		{
			TSharedPtr<IPlugin> TargetPlugin;
			ExceptionHandler->CaptureErrorsIn([this, &TargetPlugin]()
			{
				UGameFeaturesSubsystem* Subsystem = GEngine ?
					GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
				if (!TestNotNull(TEXT("game features subsystem"), Subsystem))
				{
					return;
				}
				check(Subsystem);

				FString TargetName;
				Subsystem->ForEachActiveGameFeature<UGameFeatureData>(
					[&TargetName](const UGameFeatureData* Data)
				{
					if (Data && TargetName.IsEmpty())
					{
						Data->GetPluginName(TargetName);
					}
				});
				if (TargetName.IsEmpty())
				{
					return;
				}

				TargetPlugin = IPluginManager::Get().FindPlugin(TargetName);
				if (!TargetPlugin.IsValid() || !CanModifyPluginDescriptor(TargetPlugin.ToSharedRef()))
				{
					TargetPlugin.Reset();
					return;
				}

				// Match PluginToolsetTest: explicitly check out via SCC so the write-path is
				// exercised even though SetGameFeaturePluginBuiltInAutoState does not auto-checkout.
				const FString DescriptorFile = TargetPlugin->GetDescriptorFileName();
				if (USourceControlHelpers::IsAvailable())
				{
					USourceControlHelpers::CheckOutOrAddFile(DescriptorFile);
				}

				const FGameFeaturePluginDetailsToolsetInfo Before =
					UGameFeaturesToolset59::GetGameFeaturePluginDetails(TargetName);
				const EPluginToolsetGFPAutoState OriginalState = Before.BuiltInAutoState;

				// Choose a different valid state for the round-trip.
				const EPluginToolsetGFPAutoState SwappedState =
					(OriginalState == EPluginToolsetGFPAutoState::Loaded)
					? EPluginToolsetGFPAutoState::Active
					: EPluginToolsetGFPAutoState::Loaded;

				const FGameFeaturePluginDetailsToolsetInfo AfterSwap =
					UGameFeaturesToolset59::SetGameFeaturePluginBuiltInAutoState(TargetName, SwappedState);
				TestEqual(TEXT("BuiltInAutoState updated"),
					AfterSwap.BuiltInAutoState, SwappedState);

				if (OriginalState != EPluginToolsetGFPAutoState::Invalid)
				{
					const FGameFeaturePluginDetailsToolsetInfo Restored =
						UGameFeaturesToolset59::SetGameFeaturePluginBuiltInAutoState(TargetName, OriginalState);
					TestEqual(TEXT("BuiltInAutoState restored"),
						Restored.BuiltInAutoState, OriginalState);
				}
			});
			ExpectNoException();

			// Revert SCC changes so the test is non-destructive.
			if (TargetPlugin.IsValid() && USourceControlHelpers::IsAvailable())
			{
				USourceControlHelpers::RevertFiles({ TargetPlugin->GetDescriptorFileName() });
			}
		});
	});

	Describe(TEXT("SetGameFeaturePluginMetadataValue"), [this]()
	{
		It(TEXT("Unconfigured key produces an error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UGameFeaturesToolset59::SetGameFeaturePluginMetadataValue(
					TEXT("UnknownTestPluginXYZ123"),
					TEXT("ZZ_UnconfiguredKeyForToolsetTest"),
					TEXT("true"));
			});
			ExpectExceptionContains(TEXT("AdditionalPluginMetadataKeys"));
		});

		It(TEXT("Malformed JSON produces an error"), [this]()
		{
			const TArray<FString>& Keys =
				GetDefault<UGameFeaturesSubsystemSettings>()->AdditionalPluginMetadataKeys;
			if (Keys.Num() == 0)
			{
				return;  // Need at least one configured key to exercise the JSON parse path.
			}
			const FString SomeKey = Keys[0];
			ExceptionHandler->CaptureErrorsIn([this, &SomeKey]()
			{
				UGameFeaturesToolset59::SetGameFeaturePluginMetadataValue(
					TEXT("UnknownTestPluginXYZ123"),
					SomeKey,
					TEXT("{ not json"));
			});
			ExpectExceptionContains(TEXT("not valid JSON"));
		});

		It(TEXT("round-trips a value through a modifiable GFP descriptor"), [this]()
		{
			TSharedPtr<IPlugin> TargetPlugin;
			ExceptionHandler->CaptureErrorsIn([this, &TargetPlugin]()
			{
				const TArray<FString>& Keys =
					GetDefault<UGameFeaturesSubsystemSettings>()->AdditionalPluginMetadataKeys;
				if (Keys.Num() == 0)
				{
					return;  // Nothing configured to write.
				}

				UGameFeaturesSubsystem* Subsystem = GEngine ?
					GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
				if (!TestNotNull(TEXT("game features subsystem"), Subsystem))
				{
					return;
				}
				check(Subsystem);

				FString TargetName;
				Subsystem->ForEachActiveGameFeature<UGameFeatureData>(
					[&TargetName](const UGameFeatureData* Data)
				{
					if (Data && TargetName.IsEmpty())
					{
						Data->GetPluginName(TargetName);
					}
				});
				if (TargetName.IsEmpty())
				{
					return;
				}

				TargetPlugin = IPluginManager::Get().FindPlugin(TargetName);
				if (!TargetPlugin.IsValid() || !CanModifyPluginDescriptor(TargetPlugin.ToSharedRef()))
				{
					TargetPlugin.Reset();
					return;
				}

				const FString DescriptorFile = TargetPlugin->GetDescriptorFileName();
				if (USourceControlHelpers::IsAvailable())
				{
					USourceControlHelpers::CheckOutOrAddFile(DescriptorFile);
				}

				const FString TestKey = Keys[0];
				const FGameFeaturePluginDetailsToolsetInfo Before =
					UGameFeaturesToolset59::GetGameFeaturePluginDetails(TargetName);
				const FString OriginalValue = Before.AdditionalMetadata.FindRef(TestKey);

				const FGameFeaturePluginDetailsToolsetInfo AfterWrite =
					UGameFeaturesToolset59::SetGameFeaturePluginMetadataValue(
						TargetName, TestKey, TEXT("\"toolset-test-value\""));
				TestEqual(TEXT("string round-trip"),
					AfterWrite.AdditionalMetadata.FindRef(TestKey),
					FString(TEXT("\"toolset-test-value\"")));

				if (OriginalValue.IsEmpty() || OriginalValue == TEXT("\"\""))
				{
					UGameFeaturesToolset59::RemoveGameFeaturePluginMetadataValue(TargetName, TestKey);
				}
				else
				{
					UGameFeaturesToolset59::SetGameFeaturePluginMetadataValue(
						TargetName, TestKey, OriginalValue);
				}
			});
			ExpectNoException();

			if (TargetPlugin.IsValid() && USourceControlHelpers::IsAvailable())
			{
				USourceControlHelpers::RevertFiles({ TargetPlugin->GetDescriptorFileName() });
			}
		});
	});

	Describe(TEXT("RemoveGameFeaturePluginMetadataValue"), [this]()
	{
		It(TEXT("Unconfigured key produces an error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UGameFeaturesToolset59::RemoveGameFeaturePluginMetadataValue(
					TEXT("UnknownTestPluginXYZ123"),
					TEXT("ZZ_UnconfiguredKeyForToolsetTest"));
			});
			ExpectExceptionContains(TEXT("AdditionalPluginMetadataKeys"));
		});

		It(TEXT("round-trips a remove through a modifiable GFP descriptor"), [this]()
		{
			TSharedPtr<IPlugin> TargetPlugin;
			ExceptionHandler->CaptureErrorsIn([this, &TargetPlugin]()
			{
				const TArray<FString>& Keys =
					GetDefault<UGameFeaturesSubsystemSettings>()->AdditionalPluginMetadataKeys;
				if (Keys.Num() == 0)
				{
					AddInfo(TEXT("No AdditionalPluginMetadataKeys found. Skipping test."));
					return;
				}

				UGameFeaturesSubsystem* Subsystem = GEngine ?
					GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
				if (!TestNotNull(TEXT("game features subsystem"), Subsystem))
				{
					return;
				}
				check(Subsystem);

				FString TargetName;
				Subsystem->ForEachActiveGameFeature<UGameFeatureData>(
					[&TargetName](const UGameFeatureData* Data)
				{
					if (Data && TargetName.IsEmpty())
					{
						Data->GetPluginName(TargetName);
					}
				});
				if (TargetName.IsEmpty())
				{
					AddInfo(TEXT("No GameFeature plugins found. Skipping test."));
					return;
				}

				TargetPlugin = IPluginManager::Get().FindPlugin(TargetName);
				if (!TargetPlugin.IsValid() || !CanModifyPluginDescriptor(TargetPlugin.ToSharedRef()))
				{
					AddInfo(TEXT("Cannot modify plugin descriptors. Skipping test."));
					TargetPlugin.Reset();
					return;
				}

				const FString DescriptorFile = TargetPlugin->GetDescriptorFileName();
				if (USourceControlHelpers::IsAvailable())
				{
					USourceControlHelpers::CheckOutOrAddFile(DescriptorFile);
				}

				const FString TestKey = Keys[0];
				const FGameFeaturePluginDetailsToolsetInfo Before =
					UGameFeaturesToolset59::GetGameFeaturePluginDetails(TargetName);
				const FString OriginalValue = Before.AdditionalMetadata.FindRef(TestKey);

				// Stage a known value so the remove has something concrete to clear.
				UGameFeaturesToolset59::SetGameFeaturePluginMetadataValue(
					TargetName, TestKey, TEXT("\"toolset-pre-remove-value\""));

				const FGameFeaturePluginDetailsToolsetInfo AfterRemove =
					UGameFeaturesToolset59::RemoveGameFeaturePluginMetadataValue(TargetName, TestKey);
				// Removed keys round-trip as the empty string -- the engine parser's default
				// for missing keys, which BuildInfoFromDescriptor mirrors honestly.
				TestEqual(TEXT("removed key resolves to empty string"),
					AfterRemove.AdditionalMetadata.FindRef(TestKey),
					FString());

				// Restore original value if there was one.
				if (!OriginalValue.IsEmpty())
				{
					UGameFeaturesToolset59::SetGameFeaturePluginMetadataValue(
						TargetName, TestKey, OriginalValue);
				}
			});
			ExpectNoException();

			if (TargetPlugin.IsValid() && USourceControlHelpers::IsAvailable())
			{
				USourceControlHelpers::RevertFiles({ TargetPlugin->GetDescriptorFileName() });
			}
		});
	});
}

#endif  // WITH_DEV_AUTOMATION_TESTS
