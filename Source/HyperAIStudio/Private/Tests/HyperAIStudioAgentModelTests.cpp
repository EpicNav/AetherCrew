// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HyperAIStudioLocalModels.h"
#include "HyperAIStudioSettings.h"
#include "IHttpRouter.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentModelTest,
	"HyperAIStudio.Chat.AgentModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentModelTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioAgentModelRoute Route;
	Route.AgentName = TEXT("Claude Code");
	Route.Models = { TEXT("opus"), TEXT("claude-fable-5"), TEXT("vendor/model@2026") };

	FString Argument;
	FString Error;
	auto Resolve = [&](const FString& Selected, const FString& Format)
	{
		Route.SelectedModel = Selected;
		Route.ModelFlagFormat = Format;
		return UHyperAIStudioSettings::TryResolveModelArgument(Route, Argument, Error);
	};

	TestTrue(TEXT("No selection resolves"), Resolve(FString(), TEXT("--model {0}")));
	TestTrue(TEXT("No selection passes no flag, so the agent keeps its own default"), Argument.IsEmpty());

	TestTrue(TEXT("A listed model resolves"), Resolve(TEXT("opus"), TEXT("--model {0}")));
	TestEqual(TEXT("The flag format is filled in"), Argument, FString(TEXT("--model opus")));
	TestTrue(TEXT("Ids with dots, slashes and @ are allowed"), Resolve(TEXT("vendor/model@2026"), TEXT("-m {0}")));
	TestEqual(TEXT("Alternate flag spelling"), Argument, FString(TEXT("-m vendor/model@2026")));

	// Everything below reaches a cmd.exe line, so each case must fail closed with no argument.
	auto ExpectRejected = [&](const TCHAR* What, const FString& Selected, const FString& Format)
	{
		const bool bResolved = Resolve(Selected, Format);
		TestFalse(What, bResolved);
		TestTrue(*FString::Printf(TEXT("%s: no argument is produced"), What), Argument.IsEmpty());
		TestFalse(*FString::Printf(TEXT("%s: an error explains why"), What), Error.IsEmpty());
	};

	ExpectRejected(TEXT("A model not on the curated list"), TEXT("sonnet"), TEXT("--model {0}"));
	Route.Models.Add(TEXT("opus & calc"));
	ExpectRejected(TEXT("A listed id with a command separator"), TEXT("opus & calc"), TEXT("--model {0}"));
	Route.Models.Add(TEXT("--dangerously-skip-permissions"));
	ExpectRejected(TEXT("A listed id that starts with a dash would be read as a flag"), TEXT("--dangerously-skip-permissions"), TEXT("--model {0}"));
	ExpectRejected(TEXT("A format without a placeholder"), TEXT("opus"), TEXT("--model"));
	ExpectRejected(TEXT("A format with two placeholders"), TEXT("opus"), TEXT("--model {0} {0}"));
	ExpectRejected(TEXT("A format with a shell pipe"), TEXT("opus"), TEXT("--model {0} | calc"));
	ExpectRejected(TEXT("A format with a quote"), TEXT("opus"), TEXT("--model \"{0}\""));
	ExpectRejected(TEXT("An empty format"), TEXT("opus"), FString());

	// NewObject copies the CDO, which HyperAI Chat may already have seeded; start genuinely empty.
	UHyperAIStudioSettings* Settings = NewObject<UHyperAIStudioSettings>(GetTransientPackage());
	Settings->AgentModelRoutes.Reset();
	TestTrue(TEXT("Defaults are added to empty settings"), Settings->EnsureDefaultAgentModelRoutes());
	TestFalse(TEXT("Adding defaults again changes nothing"), Settings->EnsureDefaultAgentModelRoutes());
	TestNotNull(TEXT("Codex has a route"), Settings->FindAgentModelRoute(TEXT("Codex")));
	TestNotNull(TEXT("Lookup ignores case"), Settings->FindAgentModelRoute(TEXT("claude code")));
	if (const FHyperAIStudioAgentModelRoute* Claude = Settings->FindAgentModelRoute(TEXT("Claude Code")))
	{
		TestTrue(TEXT("Claude Code is seeded with the aliases its --help documents"), Claude->Models.Contains(TEXT("opus")));
		TestTrue(TEXT("Seeded routes default to the agent's own model"), Claude->SelectedModel.IsEmpty());

		FHyperAIStudioAgentModelRoute Seeded = *Claude;
		Seeded.SelectedModel = TEXT("sonnet");
		TestTrue(TEXT("Every seeded route resolves with a seeded model"), UHyperAIStudioSettings::TryResolveModelArgument(Seeded, Argument, Error));
	}

	// Smart: plan with one model, build with another, through Claude Code's opusplan alias.
	FString Prefix;
	TestTrue(TEXT("Smart builds a launch"), UHyperAIStudioSettings::TryBuildSmartModelLaunch(
		TEXT("claude-fable-5-1"), TEXT("claude-opus-5"), Prefix, Argument, Error));
	TestEqual(TEXT("Smart re-points the plan and build aliases, quoted so cmd keeps no trailing space"), Prefix,
		FString(TEXT("set \"ANTHROPIC_DEFAULT_OPUS_MODEL=claude-fable-5-1\" && set \"ANTHROPIC_DEFAULT_SONNET_MODEL=claude-opus-5\" && ")));
	TestEqual(TEXT("Smart launches with opusplan"), Argument, FString(TEXT("--model opusplan")));
	for (const TCHAR* Unsafe : { TEXT("opus & calc"), TEXT("opus\" & calc"), TEXT("opus 5"), TEXT(""), TEXT("-opus") })
	{
		TestFalse(*FString::Printf(TEXT("Smart refuses the plan model '%s'"), Unsafe),
			UHyperAIStudioSettings::TryBuildSmartModelLaunch(Unsafe, TEXT("claude-opus-5"), Prefix, Argument, Error));
		TestTrue(TEXT("A refused Smart launch produces nothing"), Prefix.IsEmpty() && Argument.IsEmpty());
		TestFalse(*FString::Printf(TEXT("Smart refuses the build model '%s'"), Unsafe),
			UHyperAIStudioSettings::TryBuildSmartModelLaunch(TEXT("claude-fable-5-1"), Unsafe, Prefix, Argument, Error));
	}

	Settings->SmartPlanModel = TEXT("claude-fable-5-1");
	Settings->SmartBuildModel = TEXT("claude-opus-5");
	FHyperAIStudioAgentModelRoute SmartRoute;
	SmartRoute.AgentName = TEXT("Claude Code");
	SmartRoute.SelectedModel = UHyperAIStudioSettings::SmartModelId;
	TestTrue(TEXT("Smart resolves for Claude Code without being in its model list"),
		Settings->TryResolveModelLaunch(SmartRoute, Prefix, Argument, Error));
	TestEqual(TEXT("Smart resolution uses the settings' models"), Argument, FString(TEXT("--model opusplan")));
	SmartRoute.AgentName = TEXT("Codex");
	TestFalse(TEXT("Smart is refused for agents without opusplan"), Settings->TryResolveModelLaunch(SmartRoute, Prefix, Argument, Error));
	SmartRoute.AgentName = TEXT("Claude Code");
	SmartRoute.SelectedModel = TEXT("opus");
	SmartRoute.Models = { TEXT("opus") };
	TestTrue(TEXT("Ordinary models still resolve through the same entry point"), Settings->TryResolveModelLaunch(SmartRoute, Prefix, Argument, Error));
	TestTrue(TEXT("Ordinary models need no prefix"), Prefix.IsEmpty());
	TestEqual(TEXT("Ordinary models keep their flag"), Argument, FString(TEXT("--model opus")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLocalModelsTest,
	"HyperAIStudio.Chat.LocalModels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLocalModelsTest::RunTest(const FString& Parameters)
{
	// Claude Code on a model Ollama serves: its API, with every Claude alias pointed at the model, and no real key sent.
	FString Prefix;
	FString Argument;
	FString Error;
	TestTrue(TEXT("Claude Code launches on an Ollama model"),
		UHyperAIStudioSettings::TryBuildLocalModelLaunch(TEXT("Claude Code"), TEXT("ollama/qwen3.6:latest"), Prefix, Argument, Error));
	TestTrue(TEXT("through Ollama's Anthropic-compatible API, its aliases on the model and no Anthropic key"),
		Prefix.StartsWith(TEXT("set \"ANTHROPIC_BASE_URL=http://127.0.0.1:11434\" && ")) && Prefix.Contains(TEXT("set \"ANTHROPIC_API_KEY=\" && "))
		&& Prefix.Contains(TEXT("set \"ANTHROPIC_DEFAULT_HAIKU_MODEL=qwen3.6:latest\" && ")) && Prefix.EndsWith(TEXT("&& ")));
	TestEqual(TEXT("with the model by Ollama's name"), Argument, FString(TEXT("--model qwen3.6:latest")));
	TestTrue(TEXT("Codex launches on an LM Studio model"),
		UHyperAIStudioSettings::TryBuildLocalModelLaunch(TEXT("Codex"), TEXT("lmstudio/qwen/qwen3-coder-30b"), Prefix, Argument, Error));
	TestTrue(TEXT("through its own LM Studio provider"), Prefix.IsEmpty() && Argument == TEXT("--oss --local-provider lmstudio -m qwen/qwen3-coder-30b"));
	TestFalse(TEXT("A local name that could chain commands is refused"),
		UHyperAIStudioSettings::TryBuildLocalModelLaunch(TEXT("Claude Code"), TEXT("ollama/qwen & calc"), Prefix, Argument, Error));
	TestTrue(TEXT("and produces nothing"), Prefix.IsEmpty() && Argument.IsEmpty());

	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	FHyperAIStudioAgentModelRoute Route;
	Route.AgentName = TEXT("Claude Code");
	Route.SelectedModel = TEXT("ollama/qwen3.6:latest");
	TestTrue(TEXT("A local model resolves without being in the agent's list"), Settings->TryResolveModelLaunch(Route, Prefix, Argument, Error));
	Route.AgentName = TEXT("OpenCode");
	Route.SelectedModel = TEXT("ollama/qwen3.6");
	Route.Models = {TEXT("ollama/qwen3.6")};
	TestTrue(TEXT("For an agent with its own models, an Ollama id is one of them, passed as it is"),
		Settings->TryResolveModelLaunch(Route, Prefix, Argument, Error) && Prefix.IsEmpty() && Argument == TEXT("--model ollama/qwen3.6"));

	// Ollama and LM Studio, played by servers on this machine; models that call no tools, and embedding models, are left out.
	constexpr uint32 OllamaPort = 18331;
	constexpr uint32 LMStudioPort = 18332;
	const TSharedPtr<IHttpRouter> Ollama = FHttpServerModule::Get().GetHttpRouter(OllamaPort, /*bFailOnBindFailure=*/true);
	const TSharedPtr<IHttpRouter> LMStudio = FHttpServerModule::Get().GetHttpRouter(LMStudioPort, /*bFailOnBindFailure=*/true);
	if (!Ollama.IsValid() || !LMStudio.IsValid())
	{
		AddWarning(TEXT("Port 18331 or 18332 is taken, so the stand-ins for Ollama and LM Studio could not start."));
		return true;
	}
	auto Serve = [](const TCHAR* Json)
	{
		return FHttpRequestHandler::CreateLambda([Body = FString(Json)](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			OnComplete(FHttpServerResponse::Create(Body, TEXT("application/json")));
			return true;
		});
	};
	const FHttpRouteHandle Tags = Ollama->BindRoute(FHttpPath(TEXT("/api/tags")), EHttpServerRequestVerbs::VERB_GET,
		Serve(TEXT(R"({"models": [{"name": "qwen3.6:latest", "capabilities": ["vision", "completion", "tools", "thinking"]},
			{"name": "NitrAI/VibeThinker-3B:latest", "capabilities": ["completion"]}, {"name": "nomic-embed-text:latest", "capabilities": ["embedding"]}]})")));
	const FHttpRouteHandle Models = LMStudio->BindRoute(FHttpPath(TEXT("/v1/models")), EHttpServerRequestVerbs::VERB_GET,
		Serve(TEXT(R"({"object": "list", "data": [{"id": "qwen/qwen3-coder-30b", "object": "model"}, {"id": "text-embedding-nomic-embed-text-v1.5", "object": "model"}]})")));
	FHttpServerModule::Get().StartAllListeners();
	ON_SCOPE_EXIT
	{
		Ollama->UnbindRoute(Tags);
		LMStudio->UnbindRoute(Models);
		FHyperAIStudioLocalModels::SetServersForTests(FString(), FString());
	};
	auto Look = [this]()
	{
		bool bDone = false;
		FHyperAIStudioLocalModels::Refresh([&bDone]() { bDone = true; });
		const double End = FPlatformTime::Seconds() + 10.0;
		while (!bDone && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.02f);
			FPlatformProcess::Sleep(0.01f);
		}
		TestTrue(TEXT("Both servers answered or gave up"), bDone);
		return FHyperAIStudioLocalModels::Get();
	};
	FHyperAIStudioLocalModels::SetServersForTests(FString::Printf(TEXT("http://127.0.0.1:%u"), OllamaPort), FString::Printf(TEXT("http://127.0.0.1:%u"), LMStudioPort));
	const TArray<FString> Found = Look();
	AddInfo(TEXT("Found: ") + FString::Join(Found, TEXT(", ")));
	TestEqual(TEXT("Each server's chat models are found, named for the Model menu"), Found,
		TArray<FString>{TEXT("ollama/qwen3.6:latest"), TEXT("lmstudio/qwen/qwen3-coder-30b")});
	FHyperAIStudioLocalModels::SetServersForTests(TEXT("http://127.0.0.1:18339"), TEXT("http://127.0.0.1:18338"));
	TestTrue(TEXT("With neither server running, there are none"), Look().IsEmpty());
	return true;
}

#endif
