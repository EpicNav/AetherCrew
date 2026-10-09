// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HyperAIStudioCapabilityInventoryClient.h"
#include "HyperAIStudioEditorStartupGate.h"
#include "HyperAIStudioMcpSerialQueue.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSettings.h"
#include "IHttpRouter.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "SHyperAIStudioQuickActionWindow.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "SocketTypes.h"
#include "Sockets.h"

namespace HyperAIStudio::McpReadinessTests::Private
{
	/**
	 * A loopback listener that takes connections and never reads them: what an editor that has not ticked yet looks
	 * like from outside, since its listen socket is open but nothing serves it. The OS picks the port.
	 */
	class FSilentListener
	{
	public:
		~FSilentListener()
		{
			DropConnections();
			if (Socket)
			{
				Socket->Close();
				Subsystem->DestroySocket(Socket);
			}
		}

		bool Open()
		{
			Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			Socket = Subsystem
				? Subsystem->CreateSocket(NAME_Stream, TEXT("HyperAIStudio silent MCP stand-in"), FNetworkProtocolTypes::IPv4)
				: nullptr;
			if (!Socket)
			{
				return false;
			}
			const TSharedRef<FInternetAddr> Address = Subsystem->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
			Address->SetLoopbackAddress();
			Address->SetPort(0);
			if (!Socket->Bind(*Address) || !Socket->Listen(8))
			{
				return false;
			}
			Port = Socket->GetPortNo();
			return Port > 0;
		}

		FString Endpoint() const
		{
			return FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), Port);
		}

		/** Takes the connections waiting, still unread, and says how many have arrived in all. */
		int32 Connections()
		{
			bool bPending = false;
			while (Socket && Socket->HasPendingConnection(bPending) && bPending)
			{
				FSocket* Connection = Socket->Accept(TEXT("HyperAIStudio silent MCP connection"));
				if (!Connection)
				{
					break;
				}
				Taken.Add(Connection);
				++Arrived;
			}
			return Arrived;
		}

		/** Closes the connections taken, unanswered, so a client still waiting on one fails at once. */
		void DropConnections()
		{
			for (FSocket* Connection : Taken)
			{
				Connection->Close();
				Subsystem->DestroySocket(Connection);
			}
			Taken.Reset();
		}

	private:
		ISocketSubsystem* Subsystem = nullptr;
		FSocket* Socket = nullptr;
		TArray<FSocket*> Taken;
		int32 Arrived = 0;
		int32 Port = 0;
	};

	/** Ticks the core ticker, which runs both the HTTP client and the HTTP server, until Done or Seconds pass. */
	bool PumpUntil(const TFunctionRef<bool()> Done, const double Seconds)
	{
		const double End = FPlatformTime::Seconds() + Seconds;
		while (!Done() && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.02f);
			FPlatformProcess::Sleep(0.01f);
		}
		return Done();
	}

	/** Epic's server, played just far enough for the probe and an eager inventory: initialize, tools/list, the rest empty. */
	TUniquePtr<FHttpServerResponse> AnswerAsStandIn(const FHttpServerRequest& Request)
	{
		const FUTF8ToTCHAR Body(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		TSharedPtr<FJsonObject> Call;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Body.Length(), Body.Get())), Call);
		FString Method;
		if (Call.IsValid())
		{
			Call->TryGetStringField(TEXT("method"), Method);
		}
		if (Method == TEXT("initialize"))
		{
			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(FString(TEXT(
				R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25","capabilities":{"tools":{}},"serverInfo":{"name":"stand-in","version":"1.0.0"}}})")),
				TEXT("application/json"));
			Response->Headers.Add(TEXT("Mcp-Session-Id"), TArray<FString>{TEXT("stand-in-session")});
			return Response;
		}
		if (Method == TEXT("tools/list"))
		{
			return FHttpServerResponse::Create(FString(TEXT(
				R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"stand_in_read","inputSchema":{"type":"object"}},{"name":"stand_in_write","inputSchema":{"type":"object"}}]}})")),
				TEXT("application/json"));
		}
		// notifications/initialized and the closing DELETE: accepted, no body.
		return FHttpServerResponse::Ok();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMcpProbeTimeoutTest,
	"HyperAIStudio.NativeTools.McpTimeoutStaysOpen.Probe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioMcpProbeTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::McpReadinessTests::Private;
	FSilentListener Silent;
	if (!Silent.Open())
	{
		AddError(TEXT("The silent loopback listener could not open."));
		return false;
	}
	const FString Endpoint = Silent.Endpoint();
	// The restart that locked the chats: initialize went out, the editor had not ticked, the request timed out.
	AddExpectedMessagePlain(TEXT("HTTP request timed out"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	bool bDone = false;
	bool bReachable = true;
	FString Message;
	const double Started = FPlatformTime::Seconds();
	FHyperAIStudioService::ProbeOwnServerToolsListAsync(Endpoint, [&](const bool bInReachable, const FString& InMessage, int32)
	{
		bDone = true;
		bReachable = bInReachable;
		Message = InMessage;
	});
	PumpUntil([&]() { Silent.Connections(); return bDone; }, 15.0);
	TestTrue(TEXT("The probe gives up on a server that never answers"), bDone);
	TestTrue(TEXT("only after its 5 second timeout, so the initialize was out"), FPlatformTime::Seconds() - Started >= 4.5);
	TestEqual(TEXT("One connection carried the initialize"), Silent.Connections(), 1);
	TestFalse(TEXT("The server is reported unreachable"), bReachable);
	AddInfo(TEXT("Probe: ") + Message);
	TestTrue(TEXT("as an initialize that got no answer"), Message.Contains(TEXT("did not answer initialize")));
	TestTrue(TEXT("which the next probe asks again"), Message.Contains(TEXT("next probe asks again")));
	TestFalse(TEXT("A read with an unknown outcome does not lock the endpoint"), FHyperAIStudioMcpSerialQueue::IsAmbiguouslyLocked(Endpoint));
	TestFalse(TEXT("and leaves its queue free"), FHyperAIStudioMcpSerialQueue::IsBusy(Endpoint));

	bool bSecondDone = false;
	FString SecondMessage;
	FHyperAIStudioService::ProbeOwnServerToolsListAsync(Endpoint, [&](bool, const FString& InMessage, int32)
	{
		bSecondDone = true;
		SecondMessage = InMessage;
	});
	TestTrue(TEXT("The next probe is sent, not refused"), PumpUntil([&]() { return Silent.Connections() >= 2; }, 5.0));
	// Closing its connection unanswered ends it at once, again with an outcome the probe cannot know.
	Silent.DropConnections();
	TestTrue(TEXT("The next probe finishes"), PumpUntil([&]() { return bSecondDone; }, 10.0));
	AddInfo(TEXT("Next probe: ") + SecondMessage);
	TestFalse(TEXT("A second unknown outcome leaves the endpoint open too"), FHyperAIStudioMcpSerialQueue::IsAmbiguouslyLocked(Endpoint));
	TestFalse(TEXT("and its queue free"), FHyperAIStudioMcpSerialQueue::IsBusy(Endpoint));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMcpInventoryTimeoutTest,
	"HyperAIStudio.NativeTools.McpTimeoutStaysOpen.Inventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioMcpInventoryTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::McpReadinessTests::Private;
	FSilentListener Silent;
	if (!Silent.Open())
	{
		AddError(TEXT("The silent loopback listener could not open."));
		return false;
	}
	const FString Endpoint = Silent.Endpoint();
	ON_SCOPE_EXIT
	{
		FHyperAIStudioCapabilityInventoryClient::Invalidate(Endpoint);
	};
	AddExpectedMessagePlain(TEXT("HTTP request timed out"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	TOptional<FHyperAIStudioCapabilityInventoryResult> First;
	FHyperAIStudioCapabilityInventoryClient::RefreshAsync(Endpoint, /*bForce=*/true,
		[&First](const FHyperAIStudioCapabilityInventoryResult& Result) { First = Result; });
	PumpUntil([&]() { Silent.Connections(); return First.IsSet(); }, 15.0);
	if (!TestTrue(TEXT("The inventory gives up on a server that never answers"), First.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("One connection carried its initialize"), Silent.Connections(), 1);
	TestFalse(TEXT("The inventory fails"), First->bSuccess);
	AddInfo(TEXT("Inventory: ") + First->Message);
	TestTrue(TEXT("as an initialize that got no answer, asked again on the next refresh"),
		First->Message.Contains(TEXT("no answer to initialize")) && First->Message.Contains(TEXT("next refresh asks again")));
	TestFalse(TEXT("An inventory read with an unknown outcome does not lock the endpoint"), FHyperAIStudioMcpSerialQueue::IsAmbiguouslyLocked(Endpoint));
	TestFalse(TEXT("and leaves its queue free"), FHyperAIStudioMcpSerialQueue::IsBusy(Endpoint));

	TOptional<FHyperAIStudioCapabilityInventoryResult> Second;
	FHyperAIStudioCapabilityInventoryClient::RefreshAsync(Endpoint, /*bForce=*/true,
		[&Second](const FHyperAIStudioCapabilityInventoryResult& Result) { Second = Result; });
	TestTrue(TEXT("The next refresh is sent, not refused"), PumpUntil([&]() { return Silent.Connections() >= 2; }, 5.0));
	Silent.DropConnections();
	TestTrue(TEXT("The next refresh finishes"), PumpUntil([&]() { return Second.IsSet(); }, 10.0));
	if (Second.IsSet())
	{
		AddInfo(TEXT("Next refresh: ") + Second->Message);
		TestFalse(TEXT("It was not refused by a lock"), Second->Message.Contains(TEXT("fail-closed")));
	}
	TestFalse(TEXT("A second unknown outcome leaves the endpoint open too"), FHyperAIStudioMcpSerialQueue::IsAmbiguouslyLocked(Endpoint));
	TestFalse(TEXT("and its queue free"), FHyperAIStudioMcpSerialQueue::IsBusy(Endpoint));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMcpStartupGateTest,
	"HyperAIStudio.NativeTools.McpStartupGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioMcpStartupGateTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::McpReadinessTests::Private;
	TestTrue(TEXT("An editor running tests has finished starting"), FHyperAIStudioEditorStartupGate::IsEditorAnswering());

	FHyperAIStudioStatus Starting;
	Starting.bUnrealMCPModuleAvailable = true;
	Starting.bUnrealMCPSettingsConfigured = true;
	Starting.bServerRunning = true;
	Starting.bPortListening = true;
	Starting.bAgentFilesReady = true;
	Starting.ConfiguredAgentCount = 1;
	Starting.bProbeInProgress = true;
	Starting.bEditorStarting = true;
	TestTrue(TEXT("A chat waiting on the editor says so"),
		SHyperAIStudioQuickActionWindow::GetNextActionTextForStatus(Starting, false).ToString().Contains(TEXT("finish starting")));
	FHyperAIStudioStatus Unreachable = Starting;
	Unreachable.bProbeInProgress = false;
	Unreachable.bEditorStarting = false;
	Unreachable.bHasProbeRun = true;
	Unreachable.ProbeMessage = TEXT("Unreal MCP did not answer initialize (TimedOut); the next probe asks again.");
	TestTrue(TEXT("and one whose probe got no answer gives the probe's reason"),
		SHyperAIStudioQuickActionWindow::GetNextActionTextForStatus(Unreachable, false).ToString().Contains(TEXT("did not answer initialize")));

	// A stand-in for this editor's own server that counts every request reaching it.
	constexpr uint32 Port = 18341;
	const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
	if (!Router.IsValid())
	{
		AddWarning(TEXT("Port 18341 is taken, so the stand-in MCP server could not start."));
		return true;
	}
	int32 Requests = 0;
	const FHttpRouteHandle Route = Router->BindRoute(FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_POST | EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateLambda([&Requests](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			++Requests;
			OnComplete(AnswerAsStandIn(Request));
			return true;
		}));
	FHttpServerModule::Get().StartAllListeners();
	const FString Endpoint = FString::Printf(TEXT("http://127.0.0.1:%u/mcp"), Port);
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const uint32 SavedPort = Settings->UnrealMCPPort;
	ON_SCOPE_EXIT
	{
		FHyperAIStudioEditorStartupGate::SimulateStartingForTests(false);
		Settings->UnrealMCPPort = SavedPort;
		Router->UnbindRoute(Route);
		FHyperAIStudioCapabilityInventoryClient::Invalidate(Endpoint);
	};

	TestFalse(TEXT("Another port on loopback is not this editor's server"), FHyperAIStudioEditorStartupGate::IsOwnServerEndpoint(Endpoint));
	// The stand-in plays the server HyperAI is set to use, which the inventory then treats as this editor's own.
	Settings->UnrealMCPPort = Port;
	TestTrue(TEXT("The port HyperAI is set to is this editor's server"), FHyperAIStudioEditorStartupGate::IsOwnServerEndpoint(Endpoint));
	TestTrue(TEXT("under any loopback name and path"), FHyperAIStudioEditorStartupGate::IsOwnServerEndpoint(TEXT("http://localhost:18341/other")));
	TestFalse(TEXT("but not on another host"), FHyperAIStudioEditorStartupGate::IsOwnServerEndpoint(TEXT("http://example.com:18341/mcp")));
	TestFalse(TEXT("nor a malformed endpoint"), FHyperAIStudioEditorStartupGate::IsOwnServerEndpoint(TEXT("not an endpoint")));

	FHyperAIStudioEditorStartupGate::SimulateStartingForTests(true);
	TestFalse(TEXT("A starting editor does not answer yet"), FHyperAIStudioEditorStartupGate::IsEditorAnswering());
	bool bProbed = false;
	bool bReachable = false;
	int32 ToolCount = 0;
	FString Message;
	TestFalse(TEXT("The probe of its own server waits"), FHyperAIStudioService::ProbeOwnServerToolsListAsync(Endpoint,
		[&](const bool bInReachable, const FString& InMessage, const int32 InToolCount)
		{
			bProbed = true;
			bReachable = bInReachable;
			Message = InMessage;
			ToolCount = InToolCount;
		}));
	TOptional<FHyperAIStudioCapabilityInventoryResult> Inventory;
	FHyperAIStudioCapabilityInventoryClient::RefreshAsync(Endpoint, /*bForce=*/true,
		[&Inventory](const FHyperAIStudioCapabilityInventoryResult& Result) { Inventory = Result; });
	PumpUntil([]() { return false; }, 0.5);
	TestEqual(TEXT("Nothing reaches the server while the editor is starting"), Requests, 0);
	TestFalse(TEXT("and no probe result is made up"), bProbed);
	TestFalse(TEXT("nor an inventory one"), Inventory.IsSet());

	FHyperAIStudioEditorStartupGate::SimulateStartingForTests(false);
	TestTrue(TEXT("The editor answers once it has started"), FHyperAIStudioEditorStartupGate::IsEditorAnswering());
	TestTrue(TEXT("and what waited runs at once"), PumpUntil([&]() { return bProbed && Inventory.IsSet(); }, 15.0));
	AddInfo(TEXT("Probe: ") + Message);
	TestTrue(TEXT("The held probe reaches the server"), bReachable);
	TestEqual(TEXT("and reads its tools"), ToolCount, 2);
	TestTrue(TEXT("Requests reach the server now"), Requests > 0);
	if (Inventory.IsSet())
	{
		AddInfo(TEXT("Inventory: ") + Inventory->Message);
		TestTrue(TEXT("The held inventory succeeds"), Inventory->bSuccess);
	}
	return true;
}

#endif
