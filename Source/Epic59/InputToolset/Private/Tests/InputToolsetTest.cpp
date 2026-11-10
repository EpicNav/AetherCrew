// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "InputToolset.h"

#include "AsyncFunctionResult/AsyncFunctionResultVoid.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Misc/App.h"
#include "Tests/AutomationEditorCommon.h"
#endif

BEGIN_DEFINE_SPEC(FInputToolsetSpec,
	"AIAssistant.Toolsets.InputToolset.InputToolsetSpec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	// Builds a UInputToolset for validation-only tests.
	UInputToolset* MakeTestToolset()
	{
		// ULocalPlayer is Within=Engine, not the transient package.
		ULocalPlayer* FakeLocalPlayer = NewObject<ULocalPlayer>(GEngine);
		return NewObject<UInputToolset>(FakeLocalPlayer);
	}

	// Runs Call and asserts it raises a script error whose message contains ExpectedSubstring.
	void TestRaises(const TCHAR* WhatFailed, TFunction<void()> Call, const TCHAR* ExpectedSubstring)
	{
		UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
		Handler.CaptureErrorsIn(MoveTemp(Call));
		TestTrue(FString::Printf(TEXT("%s: raised an error"), WhatFailed), !Handler.GetException().IsEmpty());
		TestTrue(
			FString::Printf(TEXT("%s: error mentions '%s'"), WhatFailed, ExpectedSubstring),
			Handler.GetException().Contains(ExpectedSubstring));
	}

	// Asserts Result is non-null and already completed with an Error mentioning ExpectedSubstring.
	void TestResultErrors(const TCHAR* WhatFailed, UAsyncFunctionResultVoid* Result, const TCHAR* ExpectedSubstring)
	{
		if (!TestNotNull(FString::Printf(TEXT("%s: result returned"), WhatFailed), Result))
		{
			return;
		}
		TestTrue(FString::Printf(TEXT("%s: result is complete"), WhatFailed), Result->bIsComplete);
		TestTrue(
			FString::Printf(TEXT("%s: error mentions '%s'"), WhatFailed, ExpectedSubstring),
			Result->Error.Contains(ExpectedSubstring));
	}

#if WITH_EDITOR
	// Polls until PIE has a resolvable local player controller, or times out (reported via AddError).
	// Returns a flag, true only on success -- callers should bail out early if it's false, rather
	// than cascading into misleading failures against a session that never started.
	TSharedRef<bool> AddWaitForPIEReadyCommand()
	{
		TSharedRef<bool> bReady = MakeShared<bool>(false);
		const double StartTime = FPlatformTime::Seconds();
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(
			[this, StartTime, bReady]() -> bool
			{
				if (GEditor && GEditor->PlayWorld && GEditor->PlayWorld->GetFirstLocalPlayerFromController())
				{
					*bReady = true;
					return true;
				}
				if (FPlatformTime::Seconds() - StartTime > 30.0)
				{
					AddError(TEXT("Timed out waiting for PIE to start with a local player controller."));
					return true;
				}
				return false;
			}));
		return bReady;
	}
#endif

END_DEFINE_SPEC(FInputToolsetSpec)

void FInputToolsetSpec::Define()
{
	Describe(TEXT("Entry validation (Action/Key exactly one)"), [this]()
	{
		It(TEXT("ApplyInput errors when an entry sets neither Action nor Key"), [this]()
		{
			// ApplyInput is async: it reports failures via the returned result, not RaiseScriptError.
			TestResultErrors(TEXT("ApplyInput"),
				MakeTestToolset()->ApplyInput({ FInputToolsetEntry() }, 0.0f), TEXT("neither"));
		});

		It(TEXT("ApplyInput errors when an entry sets both Action and Key"), [this]()
		{
			UInputAction* TestAction = NewObject<UInputAction>(GetTransientPackage());
			FInputToolsetEntry Entry;
			Entry.Action = TestAction;
			Entry.Key = EKeys::SpaceBar;
			TestResultErrors(TEXT("ApplyInput"),
				MakeTestToolset()->ApplyInput({ Entry }, 0.0f), TEXT("both"));
		});

		It(TEXT("IsInputApplied raises for an invalid entry"), [this]()
		{
			UInputToolset* Toolset = MakeTestToolset();
			TestRaises(TEXT("IsInputApplied"),
				[Toolset]() { Toolset->IsInputApplied({ FInputToolsetEntry() }); },
				TEXT("neither"));
		});
	});

	Describe(TEXT("ApplyInput"), [this]()
	{
		It(TEXT("errors when Entries is empty"), [this]()
		{
			TestResultErrors(TEXT("ApplyInput"),
				MakeTestToolset()->ApplyInput({}, 0.0f), TEXT("must not be empty"));
		});

		It(TEXT("errors when DurationSeconds is negative"), [this]()
		{
			UInputAction* TestAction = NewObject<UInputAction>(GetTransientPackage());
			FInputToolsetEntry Entry;
			Entry.Action = TestAction;
			Entry.Value = FVector(1.0, 0.0, 0.0);
			TestResultErrors(TEXT("ApplyInput"),
				MakeTestToolset()->ApplyInput({ Entry }, -1.0f), TEXT("must not be negative"));
		});

		It(TEXT("errors when an Action entry has a zero Value"), [this]()
		{
			UInputAction* TestAction = NewObject<UInputAction>(GetTransientPackage());
			FInputToolsetEntry Entry;
			Entry.Action = TestAction;
			// Entry.Value left at its default (zero) -- a caller who forgot to set it.
			TestResultErrors(TEXT("ApplyInput"),
				MakeTestToolset()->ApplyInput({ Entry }, 0.0f), TEXT("zero Value"));
		});

		It(TEXT("errors when a Key entry has a zero Value"), [this]()
		{
			FInputToolsetEntry Entry;
			Entry.Key = EKeys::SpaceBar;
			Entry.Value = FVector(0.0, 5.0, 5.0); // X is what a Key reads; nonzero Y/Z shouldn't matter.
			TestResultErrors(TEXT("ApplyInput"),
				MakeTestToolset()->ApplyInput({ Entry }, 0.0f), TEXT("zero Value"));
		});

		It(TEXT("errors when an Axis1D Action entry has a zero X with nonzero Y"), [this]()
		{
			UInputAction* TestAction = NewObject<UInputAction>(GetTransientPackage());
			TestAction->ValueType = EInputActionValueType::Axis1D;
			FInputToolsetEntry Entry;
			Entry.Action = TestAction;
			Entry.Value = FVector(0.0, 5.0, 0.0); // Axis1D only reads X; Y is irrelevant to it.
			TestResultErrors(TEXT("ApplyInput"),
				MakeTestToolset()->ApplyInput({ Entry }, 0.0f), TEXT("zero Value"));
		});
	});

#if WITH_EDITOR
	Describe(TEXT("ApplyInput (live PIE)"), [this]()
	{
		It(TEXT("holds an action, reports it active, then releases it when the hold ends"), [this]()
		{
			if (!FApp::CanEverRender())
			{
				AddInfo(TEXT("Skipped: no rendering available on this run."));
				return;
			}
			if (GIsBuildMachine)
			{
				AddInfo(TEXT("Skipped: running on a build machine."));
				return;
			}
			if (GEditor && GEditor->PlayWorld)
			{
				AddInfo(TEXT("Skipped: PIE is already running from a prior test."));
				return;
			}

			// Known-clean map, so this doesn't pick up project-specific Blueprint errors from
			// whichever map the dev happens to have open (mirrors EditorAppToolset's own PIE tests).
			FAutomationEditorCommonUtils::LoadMap(TEXT("/Engine/Maps/Templates/Template_Default"));

			ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(/*bSimulateInEditor=*/false));
			TSharedRef<bool> bPIEReady = AddWaitForPIEReadyCommand();

			// A fresh, unmapped action: injection targets actions by identity, not via a mapping
			// context, so this exercises ApplyInput/IsInputApplied without needing any real input bindings.
			TStrongObjectPtr<UInputAction> TestAction(NewObject<UInputAction>(GetTransientPackage()));

			ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(
				[this, TestAction, bPIEReady]() -> bool
				{
					if (!*bPIEReady)
					{
						// Already reported via AddError in AddWaitForPIEReadyCommand.
						ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
						return true;
					}
					if (!GEditor || !GEditor->PlayWorld)
					{
						AddError(TEXT("PIE ended before ApplyInput could run."));
						ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
						return true;
					}

					// The real, subsystem-collection-managed instance for the live session's local
					// player -- not a throwaway MakeTestToolset(), since this exercises the actual
					// dispatch path a real tool call would use.
					ULocalPlayer* LocalPlayer = GEditor->PlayWorld->GetFirstLocalPlayerFromController();
					UInputToolset* Toolset = LocalPlayer ? LocalPlayer->GetSubsystem<UInputToolset>() : nullptr;
					if (!TestNotNull(TEXT("InputToolset subsystem"), Toolset))
					{
						ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
						return true;
					}

					FInputToolsetEntry Entry;
					Entry.Action = TestAction.Get();
					Entry.Value = FVector(1.0, 0.0, 0.0);
					// Held strongly by the test: nothing else keeps this result rooted once
					// FDelayedHoldWatcher itself lets go of it after completing.
					TStrongObjectPtr<UAsyncFunctionResultVoid> Result(Toolset->ApplyInput({ Entry }, 0.2f));
					if (!TestNotNull(TEXT("ApplyInput result"), Result.Get()))
					{
						ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
						return true;
					}

					FInputToolsetEntry Target;
					Target.Action = TestAction.Get();
					const TArray<bool> HeldAfterStart = Toolset->IsInputApplied({ Target });
					TestTrue(TEXT("Held immediately after ApplyInput"),
						HeldAfterStart.Num() == 1 && HeldAfterStart[0]);

					// Poll until the hold's own duration elapses and it releases itself. FEndPlayMapCommand
					// is enqueued only once this resolves, not up front -- it would otherwise sit ahead of
					// this dynamically-queued poll and tear PIE down before the hold ever completes.
					const double PollStartTime = FPlatformTime::Seconds();
					ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(
						[this, Result, TestAction, WeakToolset = TWeakObjectPtr<UInputToolset>(Toolset), PollStartTime]() -> bool
						{
							if (!Result->bIsComplete)
							{
								if (FPlatformTime::Seconds() - PollStartTime > 5.0)
								{
									AddError(TEXT("Timed out waiting for the 0.2s hold to complete."));
									ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
									return true;
								}
								return false;
							}

							// A non-empty Error here means Deinitialize cancelled the hold (e.g. PIE ended
							// early): the subsystem may already be torn down, so don't touch it below.
							if (!TestTrue(TEXT("Hold completed without error"), Result->Error.IsEmpty()))
							{
								ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
								return true;
							}

							UInputToolset* Toolset = WeakToolset.Get();
							if (!TestNotNull(TEXT("InputToolset subsystem still alive"), Toolset))
							{
								ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
								return true;
							}

							FInputToolsetEntry Target;
							Target.Action = TestAction.Get();
							const TArray<bool> HeldAfterTimeout = Toolset->IsInputApplied({ Target });
							TestTrue(TEXT("Not held once the hold completes"),
								HeldAfterTimeout.Num() == 1 && !HeldAfterTimeout[0]);

							ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
							return true;
						}));
					return true;
				}));
		});
	});
#endif // WITH_EDITOR
}

#endif // WITH_DEV_AUTOMATION_TESTS
