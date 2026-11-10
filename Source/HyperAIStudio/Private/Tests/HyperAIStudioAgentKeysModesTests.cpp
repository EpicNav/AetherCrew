// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioChatModes.h"
#include "HyperAIStudioCredentials.h"
#include "HyperAIStudioNativeChat.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "HyperAIStudioApiOptions.h"
#include "SHyperAIStudioQuickActionWindow.h"
#include "HyperAIStudioTerminalRawInput.h"
#include "STerminal.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSkills.h"
#include "HyperAIStudioWidgetShot.h"
#include "SHyperAIStudioKeysPanel.h"
#include "SHyperAIStudioModesPanel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace HyperAIStudio::KeysModesTests
{
	/** Credential Manager targets under a prefix of the test's own, removed when it ends; one agent and one provider slot. */
	struct FScopedCredentials
	{
		FString Prefix = FString::Printf(TEXT("HyperAIStudioTest-%s/"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		TArray<FString> Written;
		TMap<FString, FString> SavedEnvironment;

		FScopedCredentials()
		{
			// Never let a test use a real provider key inherited by the editor.
			for (const FHyperAIStudioKeySlot& Slot : Credentials::Slots())
			{
				if (!Slot.Agent.IsEmpty() && !SavedEnvironment.Contains(Slot.Variable))
				{
					SavedEnvironment.Add(Slot.Variable, FPlatformMisc::GetEnvironmentVariable(*Slot.Variable));
					FPlatformMisc::SetEnvironmentVar(*Slot.Variable, TEXT(""));
				}
			}
			FHyperAIStudioKeySlot Agent;
			Agent.Id = TEXT("agent/hyperai-test-agent/HYPERAI_TEST_SECRET");
			Agent.Agent = TEXT("HyperAI Test Agent");
			Agent.Label = TEXT("Test key");
			Agent.Variable = TEXT("HYPERAI_TEST_SECRET");
			Agent.Aliases = {TEXT("HYPERAI_TEST_SECRET_ALIAS")};
			FHyperAIStudioKeySlot Provider;
			Provider.Id = TEXT("provider/HYPERAI_TEST_PROVIDER_KEY");
			Provider.Label = TEXT("Test provider");
			Provider.Variable = TEXT("HYPERAI_TEST_PROVIDER_KEY");
			Credentials::SetTestScope(Prefix, {Agent, Provider});
		}

		bool Store(const FString& SlotId, const FString& Value)
		{
			FString Error;
			Written.Add(Credentials::TargetName(SlotId));
			return Credentials::Write(Credentials::TargetName(SlotId), Value, Error);
		}

		~FScopedCredentials()
		{
			for (const TPair<FString, FString>& Pair : SavedEnvironment)
			{
				FPlatformMisc::SetEnvironmentVar(*Pair.Key, *Pair.Value);
			}
			for (const FString& Target : Written)
			{
				Credentials::Remove(Target);
			}
			Credentials::SetTestScope(FString(), {});
		}
	};

	struct FScratchRoot
	{
		FString Path = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()),
			TEXT("KeysModesTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		~FScratchRoot() { IFileManager::Get().DeleteDirectory(*Path, /*RequireExists=*/false, /*Tree=*/true); }
	};

	/** Runs cmd.exe Params (with Extra in its environment when given) and returns what it printed. */
	FString RunCmd(const FString& Params, const TMap<FString, FString>* Extra, double TimeoutSeconds = 60.0)
	{
		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		FPlatformProcess::CreatePipe(ReadPipe, WritePipe);
		FProcHandle Process = Extra
			? Credentials::CreateProcWithEnvironment(TEXT("cmd.exe"), Params, FPaths::ProjectDir(), *Extra, WritePipe, nullptr)
			: FPlatformProcess::CreateProc(TEXT("cmd.exe"), *Params, false, true, true, nullptr, 0, nullptr, WritePipe, nullptr);
		FString Output;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (Process.IsValid() && FPlatformProcess::IsProcRunning(Process) && FPlatformTime::Seconds() < Deadline)
		{
			Output += FPlatformProcess::ReadPipe(ReadPipe);
			FPlatformProcess::Sleep(0.05f);
		}
		Output += FPlatformProcess::ReadPipe(ReadPipe);
		if (Process.IsValid())
		{
			if (FPlatformProcess::IsProcRunning(Process))
			{
				FPlatformProcess::TerminateProc(Process, true);
			}
			FPlatformProcess::CloseProc(Process);
		}
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		return Output;
	}

	const TCHAR* PresenceCommand = TEXT("if defined HYPERAI_TEST_SECRET (echo secret-present) else (echo secret-absent)");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioApiTerminalOptionsTest,
	"HyperAIStudio.AgentKeys.ApiTerminalOptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioApiTerminalOptionsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio;
	KeysModesTests::FScratchRoot Root;
	TestEqual(TEXT("Auto is the default"), ApiOptions::MaxOutputTokens(Root.Path), 0);
	TestTrue(TEXT("A custom project limit is saved"), ApiOptions::SetMaxOutputTokens(Root.Path, 64000));
	TestEqual(TEXT("Read back without an editor restart"), ApiOptions::MaxOutputTokens(Root.Path), 64000);
	TestFalse(TEXT("Negative limits are refused"), ApiOptions::SetMaxOutputTokens(Root.Path, -1));
	TestEqual(TEXT("Refusal preserves the existing setting"), ApiOptions::MaxOutputTokens(Root.Path), 64000);
	TestTrue(TEXT("Zero removes the HyperAI cap"), ApiOptions::SetMaxOutputTokens(Root.Path, 0));
	const FString Session = TEXT("12345678-1234-1234-1234-123456789abc");
	for (const TCHAR* Agent : {TEXT("Claude API"), TEXT("OpenAI API"), TEXT("Gemini API")})
	{
		const FString Command = FHyperAIStudioNativeChat::GetDirectApiLaunchCommand(Agent, Root.Path, FString(), Session, true, true);
		TestTrue(TEXT("Terminal starts the direct API worker"), Command.Contains(TEXT("hyperai_api_agent.py")));
		TestTrue(TEXT("Interactive presentation selected"), Command.Contains(TEXT("--terminal")));
		TestTrue(TEXT("Same saved conversation id"), Command.Contains(TEXT("--session ") + Session));
		TestTrue(TEXT("Read-only mode carried to terminal"), Command.Contains(TEXT("--read-only")));
		TestFalse(TEXT("Credentials never appear in command arguments"), Command.Contains(TEXT("API_KEY")));
		const FString Native = FHyperAIStudioNativeChat::GetDirectApiLaunchCommand(Agent, Root.Path, FString(), Session, false);
		TestFalse(TEXT("Native uses protocol presentation"), Native.Contains(TEXT("--terminal")));
		FHyperAIStudioStatus NotReady;
		const FString Bootstrap = SHyperAIStudioQuickActionWindow::BuildTerminalBootstrapCommandForStatus(NotReady, Agent, Root.Path, Command);
		TestTrue(TEXT("Direct API starts even without CLI setup readiness"), Bootstrap.Contains(Command));
		TestFalse(TEXT("No CLI readiness block"), Bootstrap.Contains(TEXT("Complete setup before")));
	}
	TestTrue(TEXT("Unsafe command path refused"), FHyperAIStudioNativeChat::GetDirectApiLaunchCommand(TEXT("Claude API"), TEXT("C:/bad&path"), FString(), Session, true).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCredentialsRoundTripTest,
	"HyperAIStudio.AgentKeys.CredentialManagerRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCredentialsRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio;
	KeysModesTests::FScopedCredentials Scope;
	const FString Target = Credentials::TargetName(TEXT("agent/hyperai-test-agent/HYPERAI_TEST_SECRET"));
	TestTrue(TEXT("Test targets carry the test's own prefix"), Target.StartsWith(Scope.Prefix));
	const FString Value = TEXT("test-value-é中-0123456789");
	TestTrue(TEXT("A value is written to Credential Manager"), Scope.Store(TEXT("agent/hyperai-test-agent/HYPERAI_TEST_SECRET"), Value));
	FString Read;
	TestTrue(TEXT("It reads back"), Credentials::Read(Target, Read));
	TestEqual(TEXT("Exactly, wide characters included"), Read, Value);
	TestEqual(TEXT("ReadSlot finds it by slot"), Credentials::ReadSlot(TEXT("agent/hyperai-test-agent/HYPERAI_TEST_SECRET")), Value);
	FString Error;
	TestFalse(TEXT("An empty value is refused"), Credentials::Write(Target, TEXT("   "), Error));
	TestTrue(TEXT("Remove deletes it"), Credentials::Remove(Target));
	TestFalse(TEXT("And it is gone"), Credentials::Read(Target, Read));
	TestTrue(TEXT("Removing what is not there is not an error"), Credentials::Remove(Target));

	// A provider key: the stored one first, then the environment variable.
	TestEqual(TEXT("Nothing stored or set"), Credentials::KeySource(TEXT("HYPERAI_TEST_PROVIDER_KEY")), FString());
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_PROVIDER_KEY"), TEXT("from-environment"));
	TestEqual(TEXT("The environment is the fallback"), Credentials::ReadKey(TEXT("HYPERAI_TEST_PROVIDER_KEY")), FString(TEXT("from-environment")));
	TestTrue(TEXT("A provider key is stored"), Scope.Store(TEXT("provider/HYPERAI_TEST_PROVIDER_KEY"), TEXT("from-credential-manager")));
	TestEqual(TEXT("The stored key wins"), Credentials::ReadKey(TEXT("HYPERAI_TEST_PROVIDER_KEY")), FString(TEXT("from-credential-manager")));
	TestEqual(TEXT("And says where it came from"), Credentials::KeySource(TEXT("HYPERAI_TEST_PROVIDER_KEY")), FString(TEXT("stored")));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_PROVIDER_KEY"), TEXT(""));

	// Outside a test's own scope, an automation run never reads the user's keys.
	Credentials::SetTestScope(FString(), {});
	TestEqual(TEXT("Automation runs do not see stored keys"), Credentials::ReadSlot(TEXT("provider/HYPERAI_TEST_PROVIDER_KEY")), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCredentialsMaskTest,
	"HyperAIStudio.AgentKeys.Mask",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCredentialsMaskTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Credentials;
	const FString Dots = TEXT("••••••••");
	TestEqual(TEXT("Nothing masks to nothing"), Mask(TEXT("")), FString());
	TestEqual(TEXT("A short value shows only dots"), Mask(TEXT("abc123")), Dots);
	TestEqual(TEXT("A medium one shows two characters each end"), Mask(TEXT("abcdefghijklmn")), TEXT("ab") + Dots + TEXT("mn"));
	const FString Key = TEXT("sk-ant-api03-ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
	const FString Masked = Mask(Key);
	TestEqual(TEXT("A long key shows four each end"), Masked, TEXT("sk-a") + Dots + TEXT("6789"));
	TestFalse(TEXT("The middle of the key is never shown"), Masked.Contains(TEXT("ABCDEFGH")));
	TestEqual(TEXT("The mask does not tell the length"), Mask(Key + Key).Len(), Masked.Len());
	TestEqual(TEXT("Whitespace around a pasted key is ignored"), Mask(TEXT("  ") + Key + TEXT("\n")), Masked);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCredentialsInjectionTest,
	"HyperAIStudio.AgentKeys.EnvironmentInjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCredentialsInjectionTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio;
	using namespace HyperAIStudio::KeysModesTests;
	FScopedCredentials Scope;
	const FString Secret = TEXT("injected-test-value-51b7");
	TestTrue(TEXT("The agent's key is stored"), Scope.Store(TEXT("agent/hyperai-test-agent/HYPERAI_TEST_SECRET"), Secret));

	const TMap<FString, FString> Environment = Credentials::EnvironmentFor(TEXT("HyperAI Test Agent"));
	TestEqual(TEXT("The agent gets its variable"), Environment.FindRef(TEXT("HYPERAI_TEST_SECRET")), Secret);
	TestEqual(TEXT("And its alias"), Environment.FindRef(TEXT("HYPERAI_TEST_SECRET_ALIAS")), Secret);
	TestTrue(TEXT("Another agent gets nothing of it"), Credentials::EnvironmentFor(TEXT("Codex")).IsEmpty());

	// The process HyperAI starts sees the variable; the editor's own environment never holds it.
	const FString Native = RunCmd(FString::Printf(TEXT("/d /s /c \"%s\""), PresenceCommand), &Environment);
	TestTrue(TEXT("A launched process has the key in its environment"), Native.Contains(TEXT("secret-present")));
	TestFalse(TEXT("Its output never needs the value"), Native.Contains(Secret));
	TestTrue(TEXT("The editor's environment does not hold it"), FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAI_TEST_SECRET")).IsEmpty());
	const FString Plain = RunCmd(FString::Printf(TEXT("/d /s /c \"%s\""), PresenceCommand), nullptr);
	TestTrue(TEXT("A process started without it does not have it"), Plain.Contains(TEXT("secret-absent")));

	// A terminal launch reads the key from Credential Manager itself: only its target name is on the line.
	const FString Wrapped = Credentials::WrapTerminalCommand(TEXT("HyperAI Test Agent"), PresenceCommand);
	TestTrue(TEXT("The terminal launch is wrapped"), Wrapped.StartsWith(TEXT("powershell.exe")));
	TestFalse(TEXT("The key is not on the command line"), Wrapped.Contains(Secret));
	TestEqual(TEXT("An agent with no keys launches as it was"), Credentials::WrapTerminalCommand(TEXT("Codex"), PresenceCommand), FString(PresenceCommand));
	const FString Terminal = RunCmd(FString::Printf(TEXT("/d /s /c \"%s\""), *Wrapped), nullptr, 120.0);
	TestTrue(TEXT("The wrapped command runs with the key in its environment"), Terminal.Contains(TEXT("secret-present")));
	TestFalse(TEXT("And never prints it"), Terminal.Contains(Secret));
	if (!Terminal.Contains(TEXT("secret-present")))
	{
		AddInfo(TEXT("Terminal output: ") + Terminal.Left(600));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCredentialsLaunchArgumentsTest,
	"HyperAIStudio.AgentKeys.LaunchArgumentsAndTests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCredentialsLaunchArgumentsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio;
	KeysModesTests::FScopedCredentials Scope;
	// Codex gets Unreal MCP on its command line, so an untrusted project still has it.
	const FString Plain = FHyperAIStudioService::GetCodexRootArguments();
	TestTrue(TEXT("Codex is given Unreal MCP by -c"), Plain.Contains(TEXT("-c mcp_servers.unreal-mcp.url=http")));
	TestFalse(TEXT("No provider without a stored key"), Plain.Contains(TEXT("model_provider=")));
	FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_API_KEY"), TEXT("test-env-key"));
	FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_BASE_URL"), TEXT("https://env.test/v1"));
	const FString FromEnvironment = FHyperAIStudioService::GetCodexRootArguments();
	TestTrue(TEXT("Environment keys also bypass ChatGPT login"), FromEnvironment.Contains(TEXT("model_provider=hyperai")));
	TestTrue(TEXT("The environment endpoint is respected"), FromEnvironment.Contains(TEXT("base_url=https://env.test/v1")));
	TestTrue(TEXT("Codex uses Responses with no subscription authentication"), FromEnvironment.Contains(TEXT("wire_api=responses"))
		&& FromEnvironment.Contains(TEXT("requires_openai_auth=false")));
	TestFalse(TEXT("An environment key never reaches command arguments"), FromEnvironment.Contains(TEXT("test-env-key")));
	const FString Key = TEXT("sk-test-0000000000000000000000");
	Scope.Store(TEXT("agent/codex/OPENAI_API_KEY"), Key);
	TestEqual(TEXT("The stored API account wins over the environment account"), Credentials::ReadAgentValue(TEXT("Codex"), TEXT("OPENAI_API_KEY")), Key);
	Scope.Store(TEXT("agent/codex/OPENAI_BASE_URL"), TEXT("https://openrouter.ai/api/v1"));
	const FString Keyed = FHyperAIStudioService::GetCodexRootArguments();
	TestTrue(TEXT("A stored key names HyperAI's provider"), Keyed.Contains(TEXT("-c model_provider=hyperai")));
	TestTrue(TEXT("Which reads OPENAI_API_KEY"), Keyed.Contains(TEXT("model_providers.hyperai.env_key=OPENAI_API_KEY")));
	TestTrue(TEXT("At the stored base URL"), Keyed.Contains(TEXT("model_providers.hyperai.base_url=https://openrouter.ai/api/v1")));
	TestFalse(TEXT("The key itself is never on the line"), Keyed.Contains(Key));
	Scope.Store(TEXT("agent/codex/OPENAI_BASE_URL"), TEXT("https://x.test/v1 & calc"));
	TestTrue(TEXT("An address with shell characters falls back to OpenAI's"),
		FHyperAIStudioService::GetCodexRootArguments().Contains(TEXT("base_url=https://api.openai.com/v1")));

	// Test builds a free authenticated read, and never for a slot without one.
	FHyperAIStudioKeySlot Slot;
	FHyperAIStudioKeyTestRequest Request;
	TestTrue(TEXT("Claude Code's key has a slot"), Credentials::FindSlot(TEXT("agent/claude-code/ANTHROPIC_API_KEY"), Slot));
	TestTrue(TEXT("Anthropic's model list"), Credentials::BuildTestRequest(Slot, TEXT("k"), FString(), Request)
		&& Request.Url == TEXT("https://api.anthropic.com/v1/models") && Request.Headers.FindRef(TEXT("x-api-key")) == TEXT("k")
		&& Request.Headers.Contains(TEXT("anthropic-version")));
	TestTrue(TEXT("At a gateway's address when one is stored"), Credentials::BuildTestRequest(Slot, TEXT("k"), TEXT("https://gw.test/"), Request)
		&& Request.Url == TEXT("https://gw.test/v1/models"));
	TestTrue(TEXT("Qwen Code's key has a slot"), Credentials::FindSlot(TEXT("agent/qwen-code/OPENAI_API_KEY"), Slot));
	TestTrue(TEXT("An OpenAI-compatible model list"), Credentials::BuildTestRequest(Slot, TEXT("k"), TEXT("https://openrouter.ai/api/v1"), Request)
		&& Request.Url == TEXT("https://openrouter.ai/api/v1/models") && Request.Headers.FindRef(TEXT("Authorization")) == TEXT("Bearer k"));
	TestTrue(TEXT("Meshy's key is a provider slot"), Credentials::FindSlot(TEXT("provider/MESHY_API_KEY"), Slot));
	TestTrue(TEXT("Its balance read"), Credentials::BuildTestRequest(Slot, TEXT("k"), FString(), Request) && Request.Url.Contains(TEXT("/balance")));
	TestTrue(TEXT("Cursor's key has a slot"), Credentials::FindSlot(TEXT("agent/cursor-agent/CURSOR_API_KEY"), Slot));
	TestFalse(TEXT("No free check is invented for it"), Credentials::BuildTestRequest(Slot, TEXT("k"), FString(), Request));
	TestTrue(TEXT("Gemini has an API key slot"), Credentials::FindSlot(TEXT("agent/gemini/GEMINI_API_KEY"), Slot));
	TestTrue(TEXT("Gemini checks the same gateway it will use"), Credentials::BuildTestRequest(Slot, TEXT("k"), TEXT("https://gemini.test/"), Request)
		&& Request.Url == TEXT("https://gemini.test/v1beta/models") && Request.Headers.FindRef(TEXT("x-goog-api-key")) == TEXT("k"));
	TestFalse(TEXT("The Gemini key is not in a URL"), Request.Url.Contains(TEXT("key=")));

	// Every slot has a distinct id, a variable and a label; the providers HyperAI's tools call are all there.
	TSet<FString> Ids;
	for (const FHyperAIStudioKeySlot& Each : Credentials::Slots())
	{
		TestFalse(*FString::Printf(TEXT("%s is listed once"), *Each.Id), Ids.Contains(Each.Id));
		Ids.Add(Each.Id);
		TestFalse(*FString::Printf(TEXT("%s names its variable"), *Each.Id), Each.Variable.IsEmpty() || Each.Label.IsEmpty());
	}
	for (const TCHAR* Variable : {TEXT("GEMINI_API_KEY"), TEXT("MESHY_API_KEY"), TEXT("TRIPO_API_KEY"), TEXT("HITEM3D_API_KEY"), TEXT("ELEVENLABS_API_KEY"), TEXT("STABILITY_API_KEY")})
	{
		TestTrue(*FString::Printf(TEXT("%s can be stored"), Variable), Ids.Contains(FString(TEXT("provider/")) + Variable));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioChatModesStorageTest,
	"HyperAIStudio.ChatModes.Storage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioChatModesStorageTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChatModes;
	HyperAIStudio::KeysModesTests::FScratchRoot Root;
	const TArray<FHyperAIStudioChatMode> Shipped = BuiltIns();
	TArray<FString> Ids;
	for (const FHyperAIStudioChatMode& Mode : Shipped)
	{
		Ids.Add(Mode.Id);
		FHyperAIStudioChatMode Back;
		FString Error;
		TestTrue(*FString::Printf(TEXT("%s round-trips through JSON"), *Mode.Id), FromJson(ToJson(Mode), Back, Error) && Back.Name == Mode.Name
			&& Back.Approval == Mode.Approval && Back.Packs == Mode.Packs && Back.Skills == Mode.Skills);
	}
	TestEqual(TEXT("Plan, Build, Review, Art pass and QA ship"), FString::Join(Ids, TEXT(",")), FString(TEXT("plan,build,review,art-pass,qa")));
	TestTrue(TEXT("Plan is read only"), Shipped[0].IsReadOnly());
	TestTrue(TEXT("Review is read only"), Shipped[2].IsReadOnly());
	TestFalse(TEXT("Build is not"), Shipped[1].IsReadOnly());
	TestEqual(TEXT("A project without modes lists the built-ins"), List(Root.Path).Num(), Shipped.Num());

	// The project's own mode, in .hyperai/modes as JSON with no secrets.
	FHyperAIStudioChatMode Blockout;
	Blockout.Id = TEXT("blockout");
	Blockout.Name = TEXT("Blockout");
	Blockout.Description = TEXT("Grey-box levels fast.");
	Blockout.Instructions = TEXT("Use simple shapes; no materials.");
	Blockout.Approval = EHyperAIStudioModeApproval::Ask;
	Blockout.Packs = {TEXT("level_design")};
	Blockout.Skills = {TEXT("unreal-level-design")};
	Blockout.Agent = TEXT("Claude Code");
	FString Error;
	TestTrue(TEXT("A project mode saves"), Save(Root.Path, Blockout, FString(), Error));
	TestTrue(TEXT("As .hyperai/modes/blockout.json"), FPaths::FileExists(FPaths::Combine(Root.Path, TEXT(".hyperai/modes/blockout.json"))));
	FHyperAIStudioChatMode Found;
	TestTrue(TEXT("It is listed"), Find(Root.Path, TEXT("blockout"), Found) && Found.Approval == EHyperAIStudioModeApproval::Ask
		&& Found.Agent == TEXT("Claude Code") && !Found.bBuiltIn);
	TestFalse(TEXT("A second mode cannot take its id"), Save(Root.Path, Blockout, FString(), Error));

	// A built-in mode changed for the project replaces it, and Delete restores it.
	FHyperAIStudioChatMode Plan = Shipped[0];
	Plan.Instructions = TEXT("Plan in five steps at most.");
	TestTrue(TEXT("A built-in mode can be changed for the project"), Save(Root.Path, Plan, Plan.Id, Error));
	TestTrue(TEXT("The project's version is used"), Find(Root.Path, TEXT("plan"), Found) && Found.Instructions == Plan.Instructions
		&& Found.bBuiltIn && !Found.File.IsEmpty());
	TestEqual(TEXT("And still listed once"), List(Root.Path).Num(), Shipped.Num() + 1);
	TestTrue(TEXT("Delete puts it back as shipped"), Delete(Root.Path, TEXT("plan"), Error) && Find(Root.Path, TEXT("plan"), Found)
		&& Found.Instructions == Shipped[0].Instructions && Found.File.IsEmpty());
	TestFalse(TEXT("A shipped mode cannot be deleted"), Delete(Root.Path, TEXT("plan"), Error));

	// Export and import.
	const FString Exported = FPaths::Combine(Root.Path, TEXT("out"), TEXT("blockout.json"));
	TestTrue(TEXT("A mode exports"), Export(Root.Path, TEXT("blockout"), Exported, Error));
	TestTrue(TEXT("Delete removes a project mode"), Delete(Root.Path, TEXT("blockout"), Error) && !Find(Root.Path, TEXT("blockout"), Found));
	FString Id;
	TestTrue(TEXT("And it imports back"), Import(Root.Path, Exported, Id, Error) && Id == TEXT("blockout") && Find(Root.Path, Id, Found));
	TestFalse(TEXT("Not twice"), Import(Root.Path, Exported, Id, Error));

	// What a mode file may not be.
	auto Refuses = [this](const TCHAR* What, const FString& Json, const TCHAR* Expected)
	{
		FHyperAIStudioChatMode Mode;
		FString Problem;
		const bool bRead = FromJson(Json, Mode, Problem);
		TestFalse(*FString::Printf(TEXT("%s is refused"), What), bRead);
		TestTrue(*FString::Printf(TEXT("%s says why (%s)"), What, *Problem), Problem.Contains(Expected));
	};
	Refuses(TEXT("A bad id"), TEXT("{\"id\":\"Bad Id\",\"name\":\"X\"}"), TEXT("not a valid mode id"));
	Refuses(TEXT("No name"), TEXT("{\"id\":\"x\"}"), TEXT("needs a name"));
	Refuses(TEXT("An unknown approval"), TEXT("{\"id\":\"x\",\"name\":\"X\",\"approval\":\"sometimes\"}"), TEXT("not an approval"));
	Refuses(TEXT("A key field"), TEXT("{\"id\":\"x\",\"name\":\"X\",\"apiKey\":\"abc\"}"), TEXT("Unknown field"));
	Refuses(TEXT("A key in the instructions"), TEXT("{\"id\":\"x\",\"name\":\"X\",\"instructions\":\"use sk-proj-abcdefghijklmnopqrstuvwx123\"}"), TEXT("looks like an API key"));
	Refuses(TEXT("Not JSON"), TEXT("plan: yes"), TEXT("not a JSON object"));
	const FString Bad = FPaths::Combine(Root.Path, TEXT("bad.json"));
	FFileHelper::SaveStringToFile(TEXT("{\"id\":\"x\",\"name\":\"X\",\"approval\":\"never\"}"), *Bad);
	TestFalse(TEXT("A bad file is not imported"), Import(Root.Path, Bad, Id, Error));
	TestTrue(TEXT("The error names the file"), Error.Contains(TEXT("bad.json")));
	TestFalse(TEXT("Plain words are not taken for a key"), LooksLikeSecret(TEXT("Use hyper_lighting_apply_plan and unreal-level-design for this.")));

	const FString Block = InstructionsBlock(Shipped[0]);
	TestTrue(TEXT("A read-only mode's block tells the agent edits are refused"), Block.Contains(TEXT("approval=\"read-only\"")) && Block.Contains(TEXT("refuses edit plans")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioChatModesReadOnlyTest,
	"HyperAIStudio.ChatModes.ReadOnlyEnforced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioChatModesReadOnlyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChatModes;
	ClearAllActiveForTests();
	ON_SCOPE_EXIT { ClearAllActiveForTests(); };
	const TArray<FHyperAIStudioChatMode> Shipped = BuiltIns();
	const int32 PlanChat = 1;
	const int32 BuildChat = 2;

	FHyperAIStudioApprovalSummary Summary;
	Summary.PackId = TEXT("test");
	Summary.ToolName = TEXT("hyper_test_edit");
	Summary.EffectTarget = TEXT("/Game/Test/Asset");
	Summary.Effects.Add(TEXT("test edit"));
	bool bRan = false;

	SetActive(&BuildChat, &Shipped[1], TEXT("HyperAI Chat 2"));
	TestTrue(TEXT("Build mode leaves edits as the user set them"), ReadOnlyHolder().IsEmpty() && !IsAskActive());

	SetActive(&PlanChat, &Shipped[0], TEXT("HyperAI Chat 1"));
	TestEqual(TEXT("The read-only chat is named"), ReadOnlyHolder(), FString(TEXT("HyperAI Chat 1 is in Plan mode")));
	TestTrue(TEXT("Every edit goes through the gate"), FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit));
	TestTrue(TEXT("Fast edit paths are closed"), FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(EHyperAIStudioDomainSafety::Edit));
	TestFalse(TEXT("Reads are untouched"), FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Read));
	const FString OperationId = TEXT("readonly-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString Error;
	TestFalse(TEXT("An edit plan is refused while a chat is read only"),
		FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [&bRan]() { bRan = true; }, Error));
	TestTrue(TEXT("With a clear message"), Error.Contains(TEXT("read-only")) && Error.Contains(TEXT("Plan mode")) && Error.Contains(TEXT("nothing was changed")));
	TestFalse(TEXT("Nothing waits for approval"), FHyperAIStudioApprovalGate::HasPending(OperationId));
	TestFalse(TEXT("Nothing ran"), bRan);

	// The chat leaves Plan mode: edits are allowed again.
	SetActive(&PlanChat, &Shipped[1], TEXT("HyperAI Chat 1"));
	TestTrue(TEXT("No chat is read only now"), ReadOnlyHolder().IsEmpty());
	TestTrue(TEXT("The same plan is held for approval"), FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [&bRan]() { bRan = true; }, Error));
	FHyperAIStudioApprovalGate::Reject(OperationId, TEXT("test"));
	TestFalse(TEXT("And rejected without running"), bRan);

	// Ask for edits: approval is required whatever Preferences say.
	SetActive(&PlanChat, &Shipped[3], TEXT("HyperAI Chat 1"));
	TestTrue(TEXT("Art pass asks for every edit"), IsAskActive() && FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit));
	ClearActive(&PlanChat);
	ClearActive(&BuildChat);
	TestFalse(TEXT("A closed chat holds no mode"), IsAskActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentKeysScreensTest,
	"HyperAIStudio.AgentKeys.Screens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentKeysScreensTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio;
	KeysModesTests::FScopedCredentials Scope;
	// A made-up key and gateway, under the test's own targets, so the list shows one masked and one address.
	Scope.Store(TEXT("agent/claude-code/ANTHROPIC_API_KEY"), TEXT("sk-ant-api03-EXAMPLEEXAMPLEEXAMPLEEXAMPLE-wxyz"));
	Scope.Store(TEXT("agent/qwen-code/OPENAI_BASE_URL"), TEXT("https://openrouter.ai/api/v1"));
	const FString KeysShot = Tests::SaveWidgetShot(SNew(SHyperAIStudioKeysPanel), FIntPoint(1000, 4200), TEXT("settings-agents-keys"));
	const FString ModesShot = Tests::SaveWidgetShot(SNew(SHyperAIStudioModesPanel), FIntPoint(760, 560), TEXT("chat-modes"));
	if (KeysShot.IsEmpty() || ModesShot.IsEmpty())
	{
		AddInfo(TEXT("This editor cannot render; no screenshots were taken."));
		return true;
	}
	AddInfo(TEXT("Screens: ") + KeysShot + TEXT(" ") + ModesShot);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSkillValidationTest,
	"HyperAIStudio.Skills.ValidationAndDiscovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSkillValidationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Skills;
	auto Check = [](const FString& Text, const FString& Folder, FString& OutError)
	{
		FString Name;
		FString Description;
		return ValidateSkill(Text, Folder, Name, Description, OutError);
	};
	FString Error;
	TestTrue(TEXT("A Claude-format skill is valid"), Check(TEXT("---\nname: door-setup\ndescription: Sets up doors. Use when adding a door.\n---\n\nBody\n"), TEXT("door-setup"), Error));
	TestFalse(TEXT("No frontmatter"), Check(TEXT("# Just a title\n"), FString(), Error));
	TestTrue(TEXT("says so"), Error.Contains(TEXT("frontmatter")));
	TestFalse(TEXT("Unclosed frontmatter"), Check(TEXT("---\nname: a\ndescription: b\n"), FString(), Error));
	TestTrue(TEXT("says so"), Error.Contains(TEXT("not closed")));
	TestFalse(TEXT("No description"), Check(TEXT("---\nname: a\n---\n"), FString(), Error));
	TestTrue(TEXT("says so"), Error.Contains(TEXT("description")));
	TestFalse(TEXT("Capitals in the name"), Check(TEXT("---\nname: Door Setup\ndescription: d\n---\n"), FString(), Error));
	TestTrue(TEXT("says so"), Error.Contains(TEXT("not valid")));
	TestFalse(TEXT("A reserved word"), Check(TEXT("---\nname: claude-helper\ndescription: d\n---\n"), FString(), Error));
	TestTrue(TEXT("says so"), Error.Contains(TEXT("reserved")));
	TestFalse(TEXT("A name that is not its folder's"), Check(TEXT("---\nname: door-setup\ndescription: d\n---\n"), TEXT("doors"), Error));
	TestTrue(TEXT("says so"), Error.Contains(TEXT("does not match")));
	TestFalse(TEXT("A description over 1024 characters"), Check(TEXT("---\nname: a\ndescription: ") + FString::ChrN(1025, TEXT('x')) + TEXT("\n---\n"), FString(), Error));
	TestFalse(TEXT("An XML tag"), Check(TEXT("---\nname: a\ndescription: Use <b>this</b>\n---\n"), FString(), Error));

	// Discovery: the project's .agents/skills (shared with Codex and mirrored for Claude Code), Claude-only skills, and invalid ones shown with why.
	HyperAIStudio::KeysModesTests::FScratchRoot Root;
	TestTrue(TEXT("New makes a valid project skill"), Create(Root.Path, TEXT("door-setup"), TEXT("Sets up doors. Use when adding a door."), Error));
	TestTrue(TEXT("In .agents/skills for Codex"), FPaths::FileExists(FPaths::Combine(CanonicalDirectory(Root.Path), TEXT("door-setup/SKILL.md"))));
	TestTrue(TEXT("Copied for Claude Code"), FPaths::FileExists(FPaths::Combine(ClaudeDirectory(Root.Path), TEXT("door-setup/SKILL.md"))));
	TestFalse(TEXT("Not twice"), Create(Root.Path, TEXT("door-setup"), TEXT("Again."), Error));
	TestFalse(TEXT("A bad name is refused"), Create(Root.Path, TEXT("Door Setup"), TEXT("d"), Error));
	FFileHelper::SaveStringToFile(TEXT("---\nname: local-only\ndescription: Only Claude Code has this one.\n---\n"),
		*FPaths::Combine(ClaudeDirectory(Root.Path), TEXT("local-only"), TEXT("SKILL.md")));
	FFileHelper::SaveStringToFile(TEXT("---\nname: broken\n---\n"), *FPaths::Combine(CanonicalDirectory(Root.Path), TEXT("broken"), TEXT("SKILL.md")));
	const TArray<FHyperAIStudioSkill> Usable = ListProjectSkills(Root.Path);
	TestTrue(TEXT("The project skill is listed"), Usable.ContainsByPredicate([](const FHyperAIStudioSkill& Skill) { return Skill.Name == TEXT("door-setup") && Skill.Scope == TEXT("project"); }));
	TestTrue(TEXT("The Claude-only one too"), Usable.ContainsByPredicate([](const FHyperAIStudioSkill& Skill) { return Skill.Name == TEXT("local-only") && Skill.Scope == TEXT("claude"); }));
	TestFalse(TEXT("An invalid one is not offered to agents"), Usable.ContainsByPredicate([](const FHyperAIStudioSkill& Skill) { return Skill.Name == TEXT("broken"); }));
	const TArray<FHyperAIStudioSkill> Shown = ListProjectSkills(Root.Path, /*bIncludeInvalid=*/true);
	const FHyperAIStudioSkill* Broken = Shown.FindByPredicate([](const FHyperAIStudioSkill& Skill) { return Skill.Name == TEXT("broken"); });
	TestTrue(TEXT("The Skills page shows the invalid one with why"), Broken && Broken->Problem.Contains(TEXT("description")));
	TestTrue(TEXT("Codex is offered the project skill"), ListForAgent(Root.Path, TEXT("Codex")).ContainsByPredicate([](const FHyperAIStudioSkill& Skill) { return Skill.Name == TEXT("door-setup"); }));

	// Import checks the skill as Claude Code would, and names what is wrong.
	const FString Source = FPaths::Combine(Root.Path, TEXT("incoming"), TEXT("Some Folder"));
	FFileHelper::SaveStringToFile(TEXT("---\nname: Not Valid\ndescription: d\n---\n"), *FPaths::Combine(Source, TEXT("SKILL.md")));
	TestTrue(TEXT("A bad skill is not imported"), Import(Root.Path, Source, Error).IsEmpty());
	TestTrue(TEXT("The error names the folder and the problem"), Error.Contains(TEXT("Some Folder")) && Error.Contains(TEXT("not valid")));
	FFileHelper::SaveStringToFile(TEXT("---\nname: crate-stack\ndescription: Stacks crates.\n---\n"), *FPaths::Combine(Source, TEXT("SKILL.md")));
	TestEqual(TEXT("A good one imports under its own name"), Import(Root.Path, Source, Error), FString(TEXT("crate-stack")));

	// HyperAI's built-in skills: at least 22, each one valid as Claude Code reads it.
	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *FPaths::Combine(BuiltInDirectory(), TEXT("*")), /*Files=*/false, /*Directories=*/true);
	int32 Valid = 0;
	for (const FString& Folder : Folders)
	{
		FString Text;
		FString Name;
		FString Description;
		if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(BuiltInDirectory(), Folder, TEXT("SKILL.md"))))
		{
			const bool bValid = ValidateSkill(Text, Folder, Name, Description, Error);
			TestTrue(*FString::Printf(TEXT("Built-in skill %s is valid (%s)"), *Folder, *Error), bValid);
			Valid += bValid ? 1 : 0;
		}
	}
	TestTrue(*FString::Printf(TEXT("At least 22 built-in skills (%d)"), Valid), Valid >= 22);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioApiAuthIsolationTest,
	"HyperAIStudio.AgentKeys.ApiAuthIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioApiAuthIsolationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio;
	KeysModesTests::FScopedCredentials Scope;
	const FString PreviousToken = FPlatformMisc::GetEnvironmentVariable(TEXT("ANTHROPIC_AUTH_TOKEN"));
	ON_SCOPE_EXIT { FPlatformMisc::SetEnvironmentVar(TEXT("ANTHROPIC_AUTH_TOKEN"), *PreviousToken); };
	FPlatformMisc::SetEnvironmentVar(TEXT("ANTHROPIC_AUTH_TOKEN"), TEXT("inherited-test-token"));
	TestTrue(TEXT("An Anthropic key is stored"), Scope.Store(TEXT("agent/claude-code/ANTHROPIC_API_KEY"), TEXT("fake-api-key")));
	const auto Environment = Credentials::EnvironmentFor(TEXT("Claude Code"));
	TestTrue(TEXT("API launch removes the inherited bearer token"), Environment.Contains(TEXT("ANTHROPIC_AUTH_TOKEN"))
		&& Environment.FindRef(TEXT("ANTHROPIC_AUTH_TOKEN")).IsEmpty());
	const FString Command = TEXT("if defined ANTHROPIC_AUTH_TOKEN (echo wrong-account) else (echo api-account)");
	TestTrue(TEXT("The native child does not retain the token"), KeysModesTests::RunCmd(FString::Printf(TEXT("/d /s /c \"%s\""), *Command), &Environment).Contains(TEXT("api-account")));
	const FString Wrapped = Credentials::WrapTerminalCommand(TEXT("Claude Code"), Command);
	TestTrue(TEXT("The terminal child also removes the token"), KeysModesTests::RunCmd(FString::Printf(TEXT("/d /s /c \"%s\""), *Wrapped), nullptr).Contains(TEXT("api-account")));
	TestEqual(TEXT("The editor's token is unchanged"), FPlatformMisc::GetEnvironmentVariable(TEXT("ANTHROPIC_AUTH_TOKEN")), FString(TEXT("inherited-test-token")));
	TestFalse(TEXT("Keys are not embedded in launch arguments"), Wrapped.Contains(TEXT("fake-api-key")));
	const FString LongCommand = FString::ChrN(5000, TEXT('x'));
	const FString Refused = Credentials::WrapTerminalCommand(TEXT("Claude Code"), LongCommand);
	TestFalse(TEXT("An oversized keyed launch cannot run without its credentials"), Refused.Contains(LongCommand));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGeminiApiHandshakeTest,
	"HyperAIStudio.AgentKeys.NativeApiHandshake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioGeminiApiHandshakeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio;
	KeysModesTests::FScopedCredentials Scope;
	KeysModesTests::FScratchRoot Scratch;
	IFileManager::Get().MakeDirectory(*Scratch.Path, true);
	const FString Script = Scratch.Path / TEXT("api_auth_agent.py");
	FFileHelper::SaveStringToFile(TEXT(R"PY(import json, os, sys
authed = False
for line in sys.stdin:
    m = json.loads(line); method = m.get('method'); ident = m.get('id'); p = m.get('params', {})
    def send(result=None, error=None):
        obj = {'jsonrpc': '2.0', 'id': ident}
        obj['error' if error else 'result'] = error if error else result
        print(json.dumps(obj), flush=True)
    if method == 'initialize':
        send({'protocolVersion': 1, 'agentCapabilities': {}, 'authMethods': [{'id': 'gemini-api-key', 'name': 'Gemini API key'}]})
    elif method == 'authenticate':
        if 'reject' in sys.argv:
            send(error={'code': -32000, 'message': 'invalid API key'})
        elif p == {'methodId': 'gemini-api-key'} and os.environ.get('GEMINI_API_KEY') == 'fake-gemini-key':
            authed = True; send({})
        else:
            send(error={'code': -32000, 'message': 'unexpected credential transport'})
    elif method == 'session/new':
        has_key = bool(os.environ.get('GEMINI_API_KEY'))
        if has_key and not authed:
            send(error={'code': -32000, 'message': 'auth required before session'})
        else:
            send({'sessionId': 'api-session' if authed else 'login-session'})
    elif method in ('thread/start', 'thread/resume'):
        send({'thread': {'id': p.get('modelProvider', 'login-provider')}})
)PY"), *Script, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	ON_SCOPE_EXIT { FHyperAIStudioNativeChat::SetCommandForTests(FString()); };
	auto Wait = [](const TFunction<bool()>& Done)
	{
		const double End = FPlatformTime::Seconds() + 10.0;
		while (!Done() && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.03f);
			FPlatformProcess::Sleep(0.02f);
		}
		return Done();
	};
	auto Start = [&](const bool bReject)
	{
		FHyperAIStudioNativeChat::SetCommandForTests(FString::Printf(TEXT("python -u \"%s\"%s"), *Script, bReject ? TEXT(" reject") : TEXT("")));
		auto Chat = FHyperAIStudioNativeChat::Create(TEXT("Gemini"), Scratch.Path, FString(), FString());
		Chat->SetHeadless(TEXT("Authentication test; no generation."));
		FString Error;
		TestTrue(TEXT("The protocol stand-in starts"), Chat->Start(Error));
		return Chat;
	};
	FPlatformMisc::SetEnvironmentVar(TEXT("GEMINI_API_KEY"), TEXT("fake-gemini-key"));
	auto Chat = Start(false);
	TestTrue(TEXT("An environment key authenticates before session creation"), Wait([&]() { return Chat->IsReady(); }) && Chat->GetSessionId() == TEXT("api-session"));
	Chat->Stop();
	FPlatformMisc::SetEnvironmentVar(TEXT("GEMINI_API_KEY"), TEXT(""));
	TestTrue(TEXT("A Gemini key is stored"), Scope.Store(TEXT("agent/gemini/GEMINI_API_KEY"), TEXT("fake-gemini-key")));
	Chat = Start(false);
	TestTrue(TEXT("A stored key also authenticates"), Wait([&]() { return Chat->IsReady(); }) && Chat->GetSessionId() == TEXT("api-session"));
	Chat->Stop();
	Chat = Start(true);
	TestTrue(TEXT("Rejected API auth reports an error without opening a login session"), Wait([&]()
	{
		return Chat->GetItems().ContainsByPredicate([](const TSharedPtr<FHyperAIStudioChatItem>& Item)
		{ return Item->Kind == FHyperAIStudioChatItem::EKind::Error; });
	}) && !Chat->IsReady() && Chat->GetSessionId().IsEmpty());
	Chat->Stop();
	Credentials::Remove(Credentials::TargetName(TEXT("agent/gemini/GEMINI_API_KEY")));
	Chat = Start(false);
	TestTrue(TEXT("No configured key leaves the login flow alone"), Wait([&]() { return Chat->IsReady(); }) && Chat->GetSessionId() == TEXT("login-session"));
	Chat->Stop();
	FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_API_KEY"), TEXT("fake-openai-key"));
	for (const FString& Resume : {FString(), FString(TEXT("previous-chatgpt-thread"))})
	{
		Chat = FHyperAIStudioNativeChat::Create(TEXT("Codex"), Scratch.Path, FString(), Resume);
		Chat->SetHeadless(TEXT("Authentication test; no generation."));
		FString Error;
		TestTrue(TEXT("Codex protocol stand-in starts"), Chat->Start(Error));
		TestTrue(TEXT("New and resumed Codex chats explicitly select the API provider"), Wait([&]() { return Chat->IsReady(); })
			&& Chat->GetSessionId() == TEXT("hyperai"));
		Chat->Stop();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDirectProviderEndpointsTest,
	"HyperAIStudio.AgentKeys.DirectProviderEndpoints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDirectProviderEndpointsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio;
	KeysModesTests::FScopedCredentials Scope;
	KeysModesTests::FScratchRoot Scratch;
	constexpr uint32 Port = 18337;
	IFileManager::Get().MakeDirectory(*Scratch.Path, true);
	const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, true);
	if (!TestTrue(TEXT("The local provider mock binds"), Router.IsValid())) { return false; }
	struct FCase { const TCHAR* Agent; const TCHAR* Slot; const TCHAR* Variable; const TCHAR* Path; const TCHAR* Reply; };
	const FCase Cases[] = {
		{TEXT("Claude API"), TEXT("claude-code"), TEXT("ANTHROPIC"), TEXT("/v1/messages"), TEXT("{\"content\":[{\"type\":\"text\",\"text\":\"Direct API answer\"}],\"stop_reason\":\"end_turn\"}")},
		{TEXT("OpenAI API"), TEXT("codex"), TEXT("OPENAI"), TEXT("/responses"), TEXT("{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"Direct API answer\"}]}]}")},
		{TEXT("Gemini API"), TEXT("gemini"), TEXT("GEMINI"), TEXT("/v1beta/models/gemini-3.8-flash:generateContent"), TEXT("{\"candidates\":[{\"finishReason\":\"STOP\",\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Direct API answer\"}]}}]}")}
	};
	FHyperAIStudioNativeChat::SetCommandForTests(FString());
	auto Wait = [](const TFunction<bool()>& Done)
	{
		const double End = FPlatformTime::Seconds() + 12.0;
		while (!Done() && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.03f);
			FPlatformProcess::Sleep(0.02f);
		}
		return Done();
	};
	for (const FCase& Case : Cases)
	{
		int32 Requests = 0;
		// Unreal treats ':' as a route parameter, including Gemini's literal ':generateContent' suffix.
		const FString RoutePath = FString(Case.Agent) == TEXT("Gemini API") ? TEXT("/v1beta/models/:model") : Case.Path;
		const FHttpRouteHandle Route = Router->BindRoute(FHttpPath(RoutePath), EHttpServerRequestVerbs::VERB_POST,
			FHttpRequestHandler::CreateLambda([this, &Requests, ExpectedPath = FString(Case.Path), Reply = FString(Case.Reply)](const FHttpServerRequest& Request, const FHttpResultCallback& Done)
			{
				++Requests;
				TestEqual(TEXT("The worker posts to the exact provider generation path"), Request.FullPath.GetPath(), ExpectedPath);
				Done(FHttpServerResponse::Create(Reply, TEXT("application/json")));
				return true;
			}));
		FHttpServerModule::Get().StartAllListeners();
		ON_SCOPE_EXIT { Router->UnbindRoute(Route); };
		const FString KeyVariable = FString(Case.Variable) + TEXT("_API_KEY");
		const FString BaseVariable = FString(Case.Agent) == TEXT("Gemini API") ? TEXT("GOOGLE_GEMINI_BASE_URL") : FString(Case.Variable) + TEXT("_BASE_URL");
		const FString Prefix = FString(TEXT("agent/")) + Case.Slot + TEXT("/");
		TestTrue(TEXT("A fake provider key is stored"), Scope.Store(Prefix + KeyVariable, TEXT("local-provider-test-key")));
		TestTrue(TEXT("The mock base URL is stored"), Scope.Store(Prefix + BaseVariable, FString::Printf(TEXT("http://127.0.0.1:%u"), Port)));
		TestTrue(TEXT("An API route is offered without CLI discovery"), FHyperAIStudioNativeChat::GetAvailableApiAgents().Contains(Case.Agent));
		auto Chat = FHyperAIStudioNativeChat::Create(Case.Agent, Scratch.Path, FString(), FString());
		Chat->SetHeadless(TEXT("Return the provider response; no tools or generation outside localhost."));
		FString Error;
		if (!TestTrue(TEXT("The real direct API worker starts"), Chat->Start(Error))) { AddError(Error); continue; }
		TestTrue(TEXT("API startup completes without a balance request"), Wait([&]() { return Chat->IsReady(); }) && Requests == 0);
		Chat->Send(TEXT("Hello, API"));
		TestTrue(*FString::Printf(TEXT("%s reaches its generation endpoint and returns to the chat"), Case.Agent), Wait([&]() { return !Chat->IsBusy(); })
			&& Requests == 1 && Chat->GetItems().ContainsByPredicate([](const TSharedPtr<FHyperAIStudioChatItem>& Item)
			{ return Item->Kind == FHyperAIStudioChatItem::EKind::Agent && Item->Text.Contains(TEXT("Direct API answer")); }));
		AddInfo(FString::Printf(TEXT("%s generation requests: %d"), Case.Agent, Requests));
		for (const TSharedPtr<FHyperAIStudioChatItem>& Item : Chat->GetItems())
		{
			if (Item->Kind == FHyperAIStudioChatItem::EKind::Error) { AddInfo(Item->Text); }
		}
		Chat->Stop();
	}
	// Project API sessions are visible and resumable without being interpreted as Claude Code/Codex histories.
	const FString Id = TEXT("12345678-1234-1234-1234-123456789abc");
	const FString Log = Scratch.Path / TEXT(".hyperai/runtime/api-chats/anthropic") / (Id + TEXT(".jsonl"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Log), true);
	FFileHelper::SaveStringToFile(TEXT("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"A direct API conversation\"}}\n"), *Log);
	const auto Sessions = ChatHistory::ListSessions(Scratch.Path);
	TestTrue(TEXT("API history is listed with its own route"), Sessions.ContainsByPredicate([&](const FHyperAIStudioChatSession& Session)
	{ return Session.AgentName == TEXT("Claude API") && Session.SessionId == Id && Session.Title == TEXT("A direct API conversation"); }));
	TestFalse(TEXT("The API conversation resolves to its own transcript"), ChatHistory::SessionFile(TEXT("Claude API"), Scratch.Path, Id).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioApiTerminalLiveTest,
	"HyperAIStudio.AgentKeys.ApiTerminalLive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioApiTerminalLiveTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio;
	KeysModesTests::FScopedCredentials Scope;
	KeysModesTests::FScratchRoot Scratch;
	IFileManager::Get().MakeDirectory(*Scratch.Path, true);
	constexpr uint32 Port = 18338;
	const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, true);
	if (!TestTrue(TEXT("Terminal mock server binds"), Router.IsValid())) { return false; }
	int32 Requests = 0;
	const FHttpRouteHandle Route = Router->BindRoute(FHttpPath(TEXT("/v1/messages")), EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([&Requests](const FHttpServerRequest& Request, const FHttpResultCallback& Done)
		{
			++Requests;
			Done(FHttpServerResponse::Create(TEXT("{\"id\":\"terminal-answer\",\"content\":[{\"type\":\"text\",\"text\":\"Embedded terminal API answer\"}],\"stop_reason\":\"end_turn\"}"), TEXT("application/json")));
			return true;
		}));
	ON_SCOPE_EXIT { Router->UnbindRoute(Route); };
	FHttpServerModule::Get().StartAllListeners();
	Scope.Store(TEXT("agent/claude-code/ANTHROPIC_API_KEY"), TEXT("terminal-local-fake-secret"));
	Scope.Store(TEXT("agent/claude-code/ANTHROPIC_BASE_URL"), FString::Printf(TEXT("http://127.0.0.1:%u"), Port));
	const TSharedRef<STerminal> Terminal = SNew(STerminal);
	if (Tests::SaveWidgetShot(Terminal, FIntPoint(1200, 760), TEXT("api-terminal-start")).IsEmpty())
	{
		AddInfo(TEXT("Rendering unavailable; terminal UI check skipped."));
		return true;
	}
	// STerminal consumes output with a Slate active timer, which requires a window in the Slate tree.
	const TSharedRef<SWindow> Window = SNew(SWindow).Title(FText::FromString(TEXT("API terminal test"))).ClientSize(FVector2D(1200, 760))[Terminal];
	FSlateApplication::Get().AddWindow(Window, true);
	ON_SCOPE_EXIT { FSlateApplication::Get().RequestDestroyWindow(Window); };
	auto Wait = [&Terminal](const TFunction<bool()>& Done)
	{
		const double End = FPlatformTime::Seconds() + 15.0;
		while (!Done() && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.03f);
			FSlateApplication::Get().Tick();
			FPlatformProcess::Sleep(0.02f);
		}
		return Done();
	};
	if (!TestTrue(TEXT("Embedded terminal session starts"), Wait([&]() { return Terminal->IsSessionRunning(); }))) { return false; }
	const FString Direct = FHyperAIStudioNativeChat::GetDirectApiLaunchCommand(TEXT("Claude API"), Scratch.Path, FString(), TEXT("87654321-4321-4321-4321-cba987654321"), true);
	const FString Wrapped = Credentials::WrapTerminalCommand(TEXT("Claude API"), Direct);
	FHyperAIStudioStatus NotReady;
	Terminal->ExecuteCommand(SHyperAIStudioQuickActionWindow::BuildTerminalBootstrapCommandForStatus(NotReady, TEXT("Claude API"), Scratch.Path, Wrapped));
	const bool bPrompt = Wait([&]() { return TerminalRawInput::ReadVisibleTail(*Terminal, 200).Contains(TEXT("You >")); });
	if (!TestTrue(TEXT("Direct API prompt appears inside embedded terminal"), bPrompt))
	{
		AddInfo(TerminalRawInput::ReadVisibleTail(*Terminal, 200));
		return false;
	}
	TestEqual(TEXT("Startup makes no provider requests"), Requests, 0);
	TestTrue(TEXT("A prompt reaches the terminal input"), TerminalRawInput::WriteText(*Terminal, TEXT("Hello from the embedded terminal"), true));
	TestTrue(TEXT("Generation returns a visible answer"), Wait([&]() { return TerminalRawInput::ReadVisibleTail(*Terminal, 200).Contains(TEXT("Embedded terminal API answer")); }));
	TestEqual(TEXT("Exactly one direct generation"), Requests, 1);
	TestFalse(TEXT("The saved key is never displayed"), TerminalRawInput::ReadVisibleTail(*Terminal, 200).Contains(TEXT("terminal-local-fake-secret")));
	AddInfo(TEXT("Terminal screenshot: ") + Tests::SaveWidgetShot(Terminal, FIntPoint(1200, 760), TEXT("api-terminal-answer")));
	TerminalRawInput::WriteText(*Terminal, TEXT("/exit"), true);
	Wait([&]() { return TerminalRawInput::ReadVisibleTail(*Terminal, 200).Contains(TEXT("Conversation saved:")); });
	return true;
}

#endif
