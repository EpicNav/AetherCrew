// Games by Hyper 2026.

#include "HyperAIStudioEditorStartupGate.h"

#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "HyperAIStudioMcpSerialQueue.h"
#include "HyperAIStudioSettings.h"
#include "IModelContextProtocolModule.h"
#include "ModelContextProtocolServer.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioStartupGate, Log, All);

namespace HyperAIStudio::EditorStartupGate::Private
{
	struct FState
	{
		bool bMainLoopRunning = false;
		bool bSimulatingStart = false;
		FTSTicker::FDelegateHandle Watch;
		TArray<TFunction<void()>> Waiting;
	};

	FState& State()
	{
		static FState Current;
		return Current;
	}

	bool IsOpen()
	{
		return State().bMainLoopRunning && !State().bSimulatingStart;
	}

	void RunWaiting()
	{
		if (!IsOpen() || State().Waiting.IsEmpty())
		{
			return;
		}
		// Taken first: held work may hold more work, which then waits for the next opening or runs at once.
		TArray<TFunction<void()>> Ready = MoveTemp(State().Waiting);
		State().Waiting.Reset();
		UE_LOG(LogHyperAIStudioStartupGate, Display,
			TEXT("The editor answers its own Unreal MCP requests now; %d held probes run."), Ready.Num());
		for (TFunction<void()>& Work : Ready)
		{
			Work();
		}
	}

	bool WatchMainLoop(float)
	{
		// GFrameCounter counts frames FEngineLoop::Tick has finished; it is still 0 through the whole first one.
		if (GFrameCounter == 0)
		{
			return true;
		}
		State().Watch.Reset();
		State().bMainLoopRunning = true;
		RunWaiting();
		return false;
	}
}

void FHyperAIStudioEditorStartupGate::Startup()
{
	using namespace HyperAIStudio::EditorStartupGate::Private;
	FState& Current = State();
	if (Current.bMainLoopRunning || Current.Watch.IsValid())
	{
		return;
	}
	// A commandlet never runs the main loop, so nothing would open the gate there; it keeps probing as before.
	if (IsRunningCommandlet() || GFrameCounter > 0)
	{
		Current.bMainLoopRunning = true;
		return;
	}
	Current.Watch = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&WatchMainLoop));
}

void FHyperAIStudioEditorStartupGate::Shutdown()
{
	using namespace HyperAIStudio::EditorStartupGate::Private;
	FState& Current = State();
	if (Current.Watch.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Current.Watch);
		Current.Watch.Reset();
	}
	Current.Waiting.Reset();
}

bool FHyperAIStudioEditorStartupGate::IsEditorAnswering()
{
	return HyperAIStudio::EditorStartupGate::Private::IsOpen();
}

bool FHyperAIStudioEditorStartupGate::RunWhenEditorAnswers(TFunction<void()> Work)
{
	using namespace HyperAIStudio::EditorStartupGate::Private;
	if (!Work)
	{
		return false;
	}
	if (IsOpen())
	{
		Work();
		return true;
	}
	if (State().Waiting.IsEmpty())
	{
		UE_LOG(LogHyperAIStudioStartupGate, Display,
			TEXT("The editor is still starting, so probes of its own Unreal MCP server wait until it can answer."));
	}
	State().Waiting.Add(MoveTemp(Work));
	return false;
}

bool FHyperAIStudioEditorStartupGate::IsOwnServerEndpoint(const FString& Endpoint)
{
	const FString Key = FHyperAIStudioMcpSerialQueue::CanonicalizeEndpointKey(Endpoint);
	if (Key.IsEmpty())
	{
		return false;
	}
	auto IsLoopbackOn = [&Key](const uint32 Port)
	{
		return Port > 0 && Key == FString::Printf(TEXT("http://127.0.0.1:%u"), Port);
	};
	if (const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		Settings && IsLoopbackOn(Settings->UnrealMCPPort))
	{
		return true;
	}
	if (IModelContextProtocolModule* Module =
		FModuleManager::Get().GetModulePtr<IModelContextProtocolModule>(TEXT("ModelContextProtocol")))
	{
		if (const FModelContextProtocolServer* Server = Module->GetServer();
			Server && Server->IsServerRunning() && IsLoopbackOn(Server->GetServerPort()))
		{
			return true;
		}
	}
	return false;
}

void FHyperAIStudioEditorStartupGate::SimulateStartingForTests(const bool bStarting)
{
	using namespace HyperAIStudio::EditorStartupGate::Private;
	State().bSimulatingStart = bStarting;
	if (!bStarting)
	{
		RunWaiting();
	}
}
