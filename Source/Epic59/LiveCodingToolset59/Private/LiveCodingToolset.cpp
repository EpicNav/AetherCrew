// Copyright Epic Games, Inc. All Rights Reserved.

#include "LiveCodingToolset59.h"

#include "AsyncFunctionResult/AsyncFunctionResultUtils.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Logging/LogMacros.h"
#include "Subsystems/SubsystemCollection.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#if WITH_LIVE_CODING
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "ILiveCodingModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogLiveCodingToolset, Log, All);

static bool bEnableLiveCodingToolset = true;

static void OnEnableLiveCodingToolsetChanged(IConsoleVariable* Variable)
{
	if (GEditor == nullptr)
	{
		return;
	}

	if (ULiveCodingToolset59* Toolset = GEditor->GetEditorSubsystem<ULiveCodingToolset59>())
	{
		Toolset->SetToolsetEnabled(Variable->GetBool());
	}
}

static FAutoConsoleVariableRef CVarEnableLiveCodingToolset(
	TEXT("LiveCodingToolset59.Enable"),
	bEnableLiveCodingToolset,
	TEXT("Enable or disable LiveCodingToolset registration. When disabled, its tools will not appear in the MCP tool list."),
	FConsoleVariableDelegate::CreateStatic(&OnEnableLiveCodingToolsetChanged),
	ECVF_Default);

#if WITH_LIVE_CODING
namespace
{
	/** Temporary output device that captures LogLiveCoding messages during compilation.
	 *  Live Coding runs its compile pipeline off the game thread and logs from worker
	 *  threads, so Serialize must be safe to call concurrently. Output mutations are
	 *  guarded by a critical section. */
	class FLiveCodingOutputCollector : public FOutputDevice
	{
	public:
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			static const FName LiveCodingCategory(TEXT("LogLiveCoding"));
			if (Category != LiveCodingCategory)
			{
				return;
			}
			FScopeLock Lock(&OutputCriticalSection);
			if (!Output.IsEmpty())
			{
				Output += TEXT("\n");
			}
			Output += V;
		}

		virtual bool CanBeUsedOnAnyThread() const override { return true; }

		FString ConsumeOutput()
		{
			FScopeLock Lock(&OutputCriticalSection);
			return MoveTemp(Output);
		}

	private:
		FCriticalSection OutputCriticalSection;
		FString Output;
	};

	FString CompileResultToString(ELiveCodingCompileResult Result)
	{
		switch (Result)
		{
		case ELiveCodingCompileResult::Success:            return TEXT("Success");
		case ELiveCodingCompileResult::NoChanges:          return TEXT("NoChanges");
		case ELiveCodingCompileResult::Failure:            return TEXT("Failure");
		case ELiveCodingCompileResult::CompileStillActive: return TEXT("CompileStillActive");
		case ELiveCodingCompileResult::NotStarted:         return TEXT("NotStarted");
		case ELiveCodingCompileResult::Cancelled:          return TEXT("Cancelled");
		case ELiveCodingCompileResult::InProgress:         return TEXT("InProgress");
		default:                                           return TEXT("Unknown");
		}
	}

	FString GetUBTLogPath()
	{
		return FPaths::Combine(FPaths::EngineDir(), TEXT("Programs"), TEXT("UnrealBuildTool"), TEXT("Log.txt"));
	}

	/** Extract lines containing MSVC diagnostics from UBT log text. */
	FString ExtractDiagnostics(const FString& LogText)
	{
		FString Diagnostics;
		TArray<FString> Lines;
		LogText.ParseIntoArrayLines(Lines);

		for (const FString& Line : Lines)
		{
			if (Line.Contains(TEXT("): error ")) || Line.Contains(TEXT("): warning ")) || Line.Contains(TEXT("): fatal error ")))
			{
				if (!Diagnostics.IsEmpty())
				{
					Diagnostics += TEXT("\n");
				}
				Diagnostics += Line;
			}
		}

		return Diagnostics;
	}

	// Generous on purpose: this only exists to reclaim a compile that never reports back at all
	// (e.g. the external console process dies mid-compile), not to catch ordinarily slow ones.
	constexpr float CompileTimeoutSeconds = 1800.0f;
}
#endif // WITH_LIVE_CODING

void ULiveCodingToolset59::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// Ensure UToolsetRegistrySubsystem is initialized before we call RegisterToolsetClass.
	// RegisterToolsetClass silently no-ops if the registry subsystem is not yet ready.
	Collection.InitializeDependency<UToolsetRegistrySubsystem>();
	SetToolsetEnabled(bEnableLiveCodingToolset);
}

void ULiveCodingToolset59::Deinitialize()
{
	SetToolsetEnabled(false);
	Super::Deinitialize();
}

void ULiveCodingToolset59::SetToolsetEnabled(bool bEnabled)
{
	// RegisterToolsetClass returns void and can bail without registering, so assuming success
	// here would latch the flag on and stop the guard below ever retrying.
	if (bEnabled && !bToolsetRegistered)
	{
		UToolsetRegistry::RegisterToolsetClass(ULiveCodingToolset59::StaticClass());
		bToolsetRegistered = UToolsetRegistry::IsToolsetClassRegistered(ULiveCodingToolset59::StaticClass());
	}
	else if (!bEnabled && bToolsetRegistered)
	{
		UToolsetRegistry::UnregisterToolsetClass(ULiveCodingToolset59::StaticClass());
		bToolsetRegistered = UToolsetRegistry::IsToolsetClassRegistered(ULiveCodingToolset59::StaticClass());
	}
}

bool ULiveCodingToolset59::IsLiveCodingReady()
{
#if WITH_LIVE_CODING
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	return LiveCoding && LiveCoding->IsEnabledForSession() && LiveCoding->IsReadyForReload();
#else
	return false;
#endif
}

void ULiveCodingToolset59::EnableLiveCoding()
{
#if WITH_LIVE_CODING
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (!LiveCoding)
	{
		UE_LOGF(LogLiveCodingToolset, Warning, "EnableLiveCoding: Live Coding module is not loaded");
		return;
	}

	LiveCoding->EnableForSession(true);
#endif
}

bool ULiveCodingToolset59::TryTimeout(UAsyncFunctionResultString& AsyncResult)
{
	if (AsyncResult.bIsComplete)
	{
		return false;
	}
	AsyncResult.SetError(TEXT("Error: Live Coding compile timed out without reporting a result."));
	return true;
}

UAsyncFunctionResultString* ULiveCodingToolset59::CompileLiveCoding()
{
	using namespace UE::AsyncFunctionResult;

#if WITH_LIVE_CODING
	UE_LOGF(LogLiveCodingToolset, Log, "CompileLiveCoding: entry");

	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (!LiveCoding)
	{
		UE_LOGF(LogLiveCodingToolset, Warning, "CompileLiveCoding: Live Coding module is not loaded");
		return MakeErrorResult<UAsyncFunctionResultString>(TEXT("Error: Live Coding module is not loaded. Ensure Live Coding is enabled in Editor Preferences."));
	}
	if (!LiveCoding->IsEnabledForSession())
	{
		UE_LOGF(LogLiveCodingToolset, Warning, "CompileLiveCoding: Live Coding is not enabled for this session");
		return MakeErrorResult<UAsyncFunctionResultString>(TEXT("Error: Live Coding is not enabled for this session. Enable it in Editor Preferences."));
	}

	// Snapshot the UBT Log.txt modification time before compile. UBT backs up and
	// rewrites Log.txt on each invocation (see EpicGames.Core Log.BackupLogFile),
	// so a changed timestamp is the signal that this compile actually produced a
	// fresh log. If the timestamp is unchanged, the prior log is stale and must
	// not leak into the result.
	const FString UBTLogPath = GetUBTLogPath();
	const FDateTime UBTLogTimestampBeforeCompile = IFileManager::Get().GetTimeStamp(*UBTLogPath);

	// Capture LogLiveCoding output during compilation (Live Coding hooks
	// surface compiler/linker output via this category). Held by TSharedRef so it
	// stays alive for the completion delegate below.
	TSharedRef<FLiveCodingOutputCollector> OutputCollector = MakeShared<FLiveCodingOutputCollector>();
	GLog->AddOutputDevice(&OutputCollector.Get());

	// Fire-and-forget: completion is driven by the delegate bound below, avoiding the
	// blocking wait and modal Slate dialog that WaitForCompletion would otherwise pump.
	ELiveCodingCompileResult ImmediateResult = ELiveCodingCompileResult::NotStarted;
	const bool bStarted = LiveCoding->Compile(ELiveCodingCompileFlags::None, &ImmediateResult);
	UAsyncFunctionResultString* AsyncResult = NewObject<UAsyncFunctionResultString>();
	if (!bStarted)
	{
		GLog->RemoveOutputDevice(&OutputCollector.Get());

		const FString ResultString = FString::Printf(TEXT("Result: %s"), *CompileResultToString(ImmediateResult));
		UE_LOGF(LogLiveCodingToolset, Log, "CompileLiveCoding: %ls", *ResultString);
		AsyncResult->SetValue(ResultString);
		return AsyncResult;
	}

	// Keep the async result alive until the delegate below completes it.
	TStrongObjectPtr<UAsyncFunctionResultString> ResultRef(AsyncResult);

	// Complete via GetOnCompileFinishedDelegate() rather than polling IsCompiling(), which can go
	// false a tick before the module finishes updating its own result.
	TSharedRef<FDelegateHandle> DelegateHandle = MakeShared<FDelegateHandle>();
	*DelegateHandle = LiveCoding->GetOnCompileFinishedDelegate().AddLambda(
		[ResultRef, OutputCollector, UBTLogPath, UBTLogTimestampBeforeCompile, DelegateHandle](ELiveCodingCompileResult Result)
		{
			// Deferred: this callback runs from inside Broadcast(), so unbinding here directly would
			// mutate the delegate's invocation list while it's still being iterated.
			AsyncTask(ENamedThreads::GameThread, [DelegateHandle]()
			{
				if (ILiveCodingModule* LiveCodingModule = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME))
				{
					LiveCodingModule->GetOnCompileFinishedDelegate().Remove(*DelegateHandle);
				}
			});

			GLog->RemoveOutputDevice(&OutputCollector.Get());

			FString ResultString = FString::Printf(TEXT("Result: %s"), *CompileResultToString(Result));

			const FString CapturedOutput = OutputCollector->ConsumeOutput();
			if (!CapturedOutput.IsEmpty())
			{
				ResultString += TEXT("\n\nCompile Output:\n");
				ResultString += CapturedOutput;
			}

			// Supplement hook output with any MSVC diagnostics that UBT wrote to its own
			// log during this compile. Only read the log when its timestamp has changed,
			// so a prior run's diagnostics cannot leak into the result.
			const FDateTime UBTLogTimestampAfterCompile = IFileManager::Get().GetTimeStamp(*UBTLogPath);
			if (UBTLogTimestampAfterCompile != FDateTime::MinValue() && UBTLogTimestampAfterCompile != UBTLogTimestampBeforeCompile)
			{
				FString LogContent;
				if (FFileHelper::LoadFileToString(LogContent, *UBTLogPath))
				{
					const FString Diagnostics = ExtractDiagnostics(LogContent);
					if (!Diagnostics.IsEmpty())
					{
						ResultString += TEXT("\n\nCompiler Diagnostics:\n");
						ResultString += Diagnostics;
					}
				}
			}

			UE_LOGF(LogLiveCodingToolset, Log, "CompileLiveCoding: %ls", *ResultString);
			ResultRef->SetValue(ResultString);
		});

	// Safety net for a compile that's started but never reports back through the delegate above.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[ResultRef, OutputCollector, DelegateHandle](float) -> bool
		{
			if (!ULiveCodingToolset59::TryTimeout(*ResultRef))
			{
				return false;
			}

			GLog->RemoveOutputDevice(&OutputCollector.Get());
			if (ILiveCodingModule* LiveCodingModule = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME))
			{
				LiveCodingModule->GetOnCompileFinishedDelegate().Remove(*DelegateHandle);
			}

			UE_LOGF(LogLiveCodingToolset, Warning, "CompileLiveCoding: timed out waiting for a result");
			return false;
		}), CompileTimeoutSeconds);

	return AsyncResult;
#else
	UE_LOGF(LogLiveCodingToolset, Warning, "CompileLiveCoding: Live Coding is not available in this build configuration");
	return MakeErrorResult<UAsyncFunctionResultString>(TEXT("Error: Live Coding is not available in this build configuration."));
#endif // WITH_LIVE_CODING
}

bool ULiveCodingToolset59::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}
