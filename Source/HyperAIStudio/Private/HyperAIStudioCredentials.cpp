// Games by Hyper 2026.

#include "HyperAIStudioCredentials.h"

#include "CoreGlobals.h"
#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioSettings.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <wincred.h>
THIRD_PARTY_INCLUDES_END
// The attribute macro needs TRUE and FALSE, which only exist between these includes.
static const DWORD_PTR HyperAIStudioHandleListAttribute = PROC_THREAD_ATTRIBUTE_HANDLE_LIST;
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioCredentials, Log, All);

namespace HyperAIStudio::Credentials
{
namespace
{
	const TCHAR* TargetPrefix = TEXT("HyperAIStudio/");

	FString& TestPrefix()
	{
		static FString Prefix;
		return Prefix;
	}

	TArray<FHyperAIStudioKeySlot>& TestSlots()
	{
		static TArray<FHyperAIStudioKeySlot> Slots;
		return Slots;
	}

	/** Lower case, words joined by hyphens: "Claude Code" is claude-code. */
	FString AgentKey(const FString& AgentName)
	{
		FString Key;
		for (const TCHAR C : AgentName.ToLower())
		{
			Key.AppendChar(FChar::IsAlnum(C) ? C : TEXT('-'));
		}
		while (Key.ReplaceInline(TEXT("--"), TEXT("-")) > 0) {}
		Key.RemoveFromStart(TEXT("-"));
		Key.RemoveFromEnd(TEXT("-"));
		return Key;
	}

	FHyperAIStudioKeySlot AgentSlot(const TCHAR* Agent, const TCHAR* Label, const TCHAR* Variable, const bool bSecret, const TCHAR* Test,
		const TCHAR* Note, const bool bVerified = true, TArray<FString> Aliases = {})
	{
		FHyperAIStudioKeySlot Slot;
		Slot.Agent = Agent;
		Slot.Id = FString::Printf(TEXT("agent/%s/%s"), *AgentKey(Agent), Variable);
		Slot.Label = Label;
		Slot.Variable = Variable;
		Slot.bSecret = bSecret;
		Slot.Test = Test;
		Slot.Note = Note;
		Slot.bVerified = bVerified;
		Slot.Aliases = MoveTemp(Aliases);
		return Slot;
	}

	FHyperAIStudioKeySlot ProviderSlot(const FString& Label, const FString& Variable, const TCHAR* Test, const FString& Note)
	{
		FHyperAIStudioKeySlot Slot;
		Slot.Id = TEXT("provider/") + Variable;
		Slot.Label = Label;
		Slot.Variable = Variable;
		Slot.Test = Test;
		Slot.Note = Note;
		return Slot;
	}

	/**
	 * Each agent's variables, checked against its own documentation (13 October 2026). OpenCode's documentation names
	 * no provider variables (its providers read them through models.dev), so those are marked unverified. Codex's own
	 * provider does not read OPENAI_API_KEY; HyperAI names a provider that does (GetCodexRootArguments). Cline takes
	 * keys only through `cline auth`, so it has none here.
	 */
	TArray<FHyperAIStudioKeySlot> AgentSlots()
	{
		return {
			AgentSlot(TEXT("Claude Code"), TEXT("Anthropic API key"), TEXT("ANTHROPIC_API_KEY"), true, TEXT("anthropic"),
				TEXT("Billed to your Anthropic Console account instead of a Claude subscription. Claude Code asks once to use it.")),
			AgentSlot(TEXT("Claude Code"), TEXT("Anthropic-compatible base URL"), TEXT("ANTHROPIC_BASE_URL"), false, TEXT(""),
				TEXT("A gateway or proxy that speaks Anthropic's API. Leave empty for Anthropic itself.")),
			AgentSlot(TEXT("Codex"), TEXT("OpenAI API key"), TEXT("OPENAI_API_KEY"), true, TEXT("openai"),
				TEXT("Codex uses it through a provider HyperAI names on its command line, instead of your ChatGPT sign-in.")),
			AgentSlot(TEXT("Codex"), TEXT("OpenAI-compatible base URL"), TEXT("OPENAI_BASE_URL"), false, TEXT(""),
				TEXT("Any OpenAI-compatible endpoint (OpenRouter, a gateway, a local server). Leave empty for OpenAI.")),
			AgentSlot(TEXT("Gemini"), TEXT("Gemini API key"), TEXT("GEMINI_API_KEY"), true, TEXT("gemini"),
				TEXT("From Google AI Studio. Gemini CLI's nanobanana extension gets it too."), true, {TEXT("NANOBANANA_API_KEY")}),
			AgentSlot(TEXT("Gemini"), TEXT("Gemini API base URL"), TEXT("GOOGLE_GEMINI_BASE_URL"), false, TEXT(""),
				TEXT("A proxy for the Gemini API, used with an API key; https unless it is on this computer.")),
			AgentSlot(TEXT("GitHub Copilot"), TEXT("GitHub token"), TEXT("COPILOT_GITHUB_TOKEN"), true, TEXT("github"),
				TEXT("A fine-grained personal access token with the Copilot Requests permission.")),
			AgentSlot(TEXT("Cursor Agent"), TEXT("Cursor API key"), TEXT("CURSOR_API_KEY"), true, TEXT(""),
				TEXT("From your Cursor dashboard's integrations page.")),
			AgentSlot(TEXT("OpenCode"), TEXT("OpenRouter API key"), TEXT("OPENROUTER_API_KEY"), true, TEXT("openrouter"),
				TEXT("OpenCode's OpenRouter provider; pick an openrouter/ model in the Model menu."), false),
			AgentSlot(TEXT("OpenCode"), TEXT("OpenAI API key"), TEXT("OPENAI_API_KEY"), true, TEXT("openai"),
				TEXT("OpenCode's OpenAI provider."), false),
			AgentSlot(TEXT("OpenCode"), TEXT("Anthropic API key"), TEXT("ANTHROPIC_API_KEY"), true, TEXT("anthropic"),
				TEXT("OpenCode's Anthropic provider."), false),
			AgentSlot(TEXT("Goose"), TEXT("OpenRouter API key"), TEXT("OPENROUTER_API_KEY"), true, TEXT("openrouter"),
				TEXT("Goose's OpenRouter provider (GOOSE_PROVIDER=openrouter in its settings).")),
			AgentSlot(TEXT("Goose"), TEXT("OpenAI API key"), TEXT("OPENAI_API_KEY"), true, TEXT("openai"),
				TEXT("Goose's OpenAI provider, or any OpenAI-compatible one with the host below.")),
			AgentSlot(TEXT("Goose"), TEXT("OpenAI-compatible host"), TEXT("OPENAI_HOST"), false, TEXT(""),
				TEXT("The endpoint's host, such as https://openrouter.ai; Goose adds its own path.")),
			AgentSlot(TEXT("Qwen Code"), TEXT("OpenAI-compatible API key"), TEXT("OPENAI_API_KEY"), true, TEXT("openai"),
				TEXT("For DashScope, OpenRouter or any OpenAI-compatible endpoint, with the base URL below.")),
			AgentSlot(TEXT("Qwen Code"), TEXT("OpenAI-compatible base URL"), TEXT("OPENAI_BASE_URL"), false, TEXT(""),
				TEXT("Such as https://openrouter.ai/api/v1. Leave empty for OpenAI.")),
			AgentSlot(TEXT("Kimi"), TEXT("Kimi API key"), TEXT("KIMI_API_KEY"), true, TEXT(""),
				TEXT("For Kimi CLI's kimi provider; it overrides the key in its config file.")),
			AgentSlot(TEXT("Kimi"), TEXT("Kimi base URL"), TEXT("KIMI_BASE_URL"), false, TEXT(""),
				TEXT("Another address for the kimi provider. Leave empty for Moonshot's own.")),
			AgentSlot(TEXT("Auggie"), TEXT("Augment session token"), TEXT("AUGMENT_SESSION_AUTH"), true, TEXT(""),
				TEXT("What `auggie token print` shows after signing in."))};
	}
}

FString TargetName(const FString& SlotId)
{
	return (TestPrefix().IsEmpty() ? FString(TargetPrefix) : TestPrefix()) + SlotId;
}

bool Write(const FString& Target, const FString& Secret, FString& OutError)
{
	OutError.Reset();
	const FString Value = Secret.TrimStartAndEnd();
	if (Value.IsEmpty())
	{
		OutError = TEXT("Nothing to store: the value is empty. Remove clears a stored value.");
		return false;
	}
#if PLATFORM_WINDOWS
	// UTF-16 like every other generic credential, so PowerShell and cmdkey read it back the same way.
	const int32 Bytes = Value.Len() * sizeof(TCHAR);
	if (Bytes > CRED_MAX_CREDENTIAL_BLOB_SIZE)
	{
		OutError = FString::Printf(TEXT("That value is too long for Credential Manager (%d characters at most)."), CRED_MAX_CREDENTIAL_BLOB_SIZE / 2);
		return false;
	}
	CREDENTIALW Credential = {};
	Credential.Type = CRED_TYPE_GENERIC;
	Credential.TargetName = const_cast<LPWSTR>(*Target);
	Credential.Comment = const_cast<LPWSTR>(TEXT("HyperAI Studio: a key for an agent or provider, set in Settings > Agents."));
	Credential.CredentialBlobSize = static_cast<DWORD>(Bytes);
	Credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<TCHAR*>(*Value));
	Credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	Credential.UserName = const_cast<LPWSTR>(TEXT("HyperAIStudio"));
	if (!::CredWriteW(&Credential, 0))
	{
		OutError = FString::Printf(TEXT("Windows Credential Manager refused the value (error %u)."), ::GetLastError());
		return false;
	}
	return true;
#else
	OutError = TEXT("Stored keys need Windows Credential Manager; set the environment variable instead.");
	return false;
#endif
}

bool Read(const FString& Target, FString& OutSecret)
{
	OutSecret.Reset();
#if PLATFORM_WINDOWS
	PCREDENTIALW Credential = nullptr;
	if (!::CredReadW(*Target, CRED_TYPE_GENERIC, 0, &Credential) || !Credential)
	{
		return false;
	}
	if (Credential->CredentialBlob && Credential->CredentialBlobSize >= sizeof(TCHAR))
	{
		OutSecret = FString(static_cast<int32>(Credential->CredentialBlobSize / sizeof(TCHAR)), reinterpret_cast<const TCHAR*>(Credential->CredentialBlob));
	}
	::CredFree(Credential);
	return !OutSecret.IsEmpty();
#else
	return false;
#endif
}

bool Remove(const FString& Target)
{
#if PLATFORM_WINDOWS
	return ::CredDeleteW(*Target, CRED_TYPE_GENERIC, 0) || ::GetLastError() == ERROR_NOT_FOUND;
#else
	return true;
#endif
}

FString Mask(const FString& Secret)
{
	const FString Value = Secret.TrimStartAndEnd();
	if (Value.IsEmpty())
	{
		return FString();
	}
	// The same eight dots whatever the length, so the mask does not tell how long the key is.
	const FString Dots = TEXT("\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022");
	if (Value.Len() < 12)
	{
		return Dots;
	}
	const int32 Shown = Value.Len() >= 20 ? 4 : 2;
	return Value.Left(Shown) + Dots + Value.Right(Shown);
}

TArray<FHyperAIStudioKeySlot> Slots()
{
	TArray<FHyperAIStudioKeySlot> Out = AgentSlots();
	// The providers HyperAI's own tools call, each key once whatever number of services share it.
	TSet<FString> Seen;
	auto AddProvider = [&Out, &Seen](const FHyperAIStudioExternalServiceSettings& Service, const TCHAR* Test)
	{
		if (Service.ApiKeyVariable.IsEmpty() || Seen.Contains(Service.ApiKeyVariable))
		{
			return;
		}
		Seen.Add(Service.ApiKeyVariable);
		Out.Add(ProviderSlot(Service.Label, Service.ApiKeyVariable, Test,
			FString::Printf(TEXT("HyperAI's own tools call %s with it; %s in the environment is used when nothing is stored."), *Service.Label, *Service.ApiKeyVariable)));
	};
	AddProvider(UHyperAIStudioSettings::GetGeminiService(), TEXT("gemini"));
	for (const FHyperAIStudioExternalServiceSettings& Service : UHyperAIStudioSettings::GetMeshServices())
	{
		AddProvider(Service, Service.ApiKeyVariable == TEXT("MESHY_API_KEY") ? TEXT("meshy") : Service.ApiKeyVariable == TEXT("TRIPO_API_KEY") ? TEXT("tripo") : TEXT(""));
	}
	for (const FHyperAIStudioExternalServiceSettings& Service : UHyperAIStudioSettings::GetAudioServices())
	{
		AddProvider(Service, Service.ApiKeyVariable == TEXT("ELEVENLABS_API_KEY") ? TEXT("elevenlabs") : Service.ApiKeyVariable == TEXT("STABILITY_API_KEY") ? TEXT("stability") : TEXT(""));
	}
	for (const FHyperAIStudioExternalServiceSettings& Service : FHyperAIStudioExternalServices::GetServices())
	{
		AddProvider(Service, TEXT(""));
	}
	Out.Append(TestSlots());
	return Out;
}

bool FindSlot(const FString& SlotId, FHyperAIStudioKeySlot& OutSlot)
{
	for (FHyperAIStudioKeySlot& Slot : Slots())
	{
		if (Slot.Id == SlotId)
		{
			OutSlot = MoveTemp(Slot);
			return true;
		}
	}
	return false;
}

FString ReadSlot(const FString& SlotId)
{
	// An automation run never sees the user's own keys: only what a test stored under its own prefix.
	if (GIsAutomationTesting && TestPrefix().IsEmpty())
	{
		return FString();
	}
	FString Value;
	Read(TargetName(SlotId), Value);
	return Value;
}

TMap<FString, FString> EnvironmentFor(const FString& AgentName)
{
	TMap<FString, FString> Environment;
	if (AgentName.IsEmpty())
	{
		return Environment;
	}
	for (const FHyperAIStudioKeySlot& Slot : Slots())
	{
		if (!Slot.Agent.Equals(AgentName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		const FString Value = ReadSlot(Slot.Id).TrimStartAndEnd();
		if (Value.IsEmpty())
		{
			continue;
		}
		Environment.Add(Slot.Variable, Value);
		for (const FString& Alias : Slot.Aliases)
		{
			Environment.Add(Alias, Value);
		}
	}
	return Environment;
}

FString ReadKey(const FString& Variable)
{
	if (Variable.IsEmpty())
	{
		return FString();
	}
	const FString Stored = ReadSlot(TEXT("provider/") + Variable).TrimStartAndEnd();
	return Stored.IsEmpty() ? FPlatformMisc::GetEnvironmentVariable(*Variable).TrimStartAndEnd() : Stored;
}

FString KeySource(const FString& Variable)
{
	if (Variable.IsEmpty())
	{
		return FString();
	}
	if (!ReadSlot(TEXT("provider/") + Variable).TrimStartAndEnd().IsEmpty())
	{
		return TEXT("stored");
	}
	return FPlatformMisc::GetEnvironmentVariable(*Variable).TrimStartAndEnd().IsEmpty() ? FString() : FString(TEXT("environment"));
}

FProcHandle CreateProcWithEnvironment(const FString& Executable, const FString& Params, const FString& WorkingDirectory,
	const TMap<FString, FString>& Extra, void* PipeWriteChild, void* PipeReadChild, uint32* OutProcessId)
{
#if PLATFORM_WINDOWS
	// A copy of this editor's environment with Extra on top, sorted by name without case as Windows wants it.
	TArray<TPair<FString, FString>> Entries;
	if (TCHAR* Block = ::GetEnvironmentStringsW())
	{
		for (const TCHAR* Entry = Block; *Entry; Entry += FCString::Strlen(Entry) + 1)
		{
			const FString Line(Entry);
			// Names may start with '=' (the per-drive directories, "=C:=C:\..."), so the separator is searched after it.
			const int32 Equals = Line.Find(TEXT("="), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
			if (Equals > 0)
			{
				Entries.Emplace(Line.Left(Equals), Line.Mid(Equals + 1));
			}
		}
		::FreeEnvironmentStringsW(Block);
	}
	for (const TPair<FString, FString>& Pair : Extra)
	{
		Entries.RemoveAll([&Pair](const TPair<FString, FString>& Each) { return Each.Key.Equals(Pair.Key, ESearchCase::IgnoreCase); });
		Entries.Emplace(Pair.Key, Pair.Value);
	}
	Entries.StableSort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B)
	{
		return FCString::Stricmp(*A.Key, *B.Key) < 0;
	});
	TArray<TCHAR> Environment;
	for (const TPair<FString, FString>& Entry : Entries)
	{
		const FString Line = Entry.Key + TEXT("=") + Entry.Value;
		Environment.Append(*Line, Line.Len());
		Environment.Add(TEXT('\0'));
	}
	Environment.Add(TEXT('\0'));

	// As FPlatformProcess::CreateProc starts a hidden process with these pipes, stderr joined to stdout.
	HANDLE Handles[2];
	uint32 HandleCount = 0;
	if (PipeWriteChild)
	{
		Handles[HandleCount++] = HANDLE(PipeWriteChild);
	}
	if (PipeReadChild && PipeReadChild != PipeWriteChild)
	{
		Handles[HandleCount++] = HANDLE(PipeReadChild);
	}
	DWORD CreationFlags = CREATE_NEW_PROCESS_GROUP | NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT;
	DWORD StartFlags = STARTF_USESHOWWINDOW;
	TArray<uint8> AttributeStorage;
	LPPROC_THREAD_ATTRIBUTE_LIST AttributeList = nullptr;
	if (HandleCount > 0)
	{
		SIZE_T Size = 0;
		::InitializeProcThreadAttributeList(nullptr, 1, 0, &Size);
		AttributeStorage.SetNumZeroed(static_cast<int32>(Size));
		AttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttributeStorage.GetData());
		if (!::InitializeProcThreadAttributeList(AttributeList, 1, 0, &Size))
		{
			return FProcHandle();
		}
		if (!::UpdateProcThreadAttribute(AttributeList, 0, HyperAIStudioHandleListAttribute, Handles, HandleCount * sizeof(HANDLE), nullptr, nullptr))
		{
			::DeleteProcThreadAttributeList(AttributeList);
			return FProcHandle();
		}
		CreationFlags |= EXTENDED_STARTUPINFO_PRESENT;
		StartFlags |= STARTF_USESTDHANDLES;
	}
	STARTUPINFOEXW StartupInfo = {};
	StartupInfo.StartupInfo.cb = sizeof(STARTUPINFOEXW);
	StartupInfo.StartupInfo.dwFlags = StartFlags;
	StartupInfo.StartupInfo.wShowWindow = SW_HIDE;
	StartupInfo.StartupInfo.hStdInput = HANDLE(PipeReadChild);
	StartupInfo.StartupInfo.hStdOutput = HANDLE(PipeWriteChild);
	StartupInfo.StartupInfo.hStdError = HANDLE(PipeWriteChild);
	StartupInfo.lpAttributeList = AttributeList;
	FString CommandLine = FString::Printf(TEXT("\"%s\" %s"), *Executable, *Params);
	PROCESS_INFORMATION ProcessInformation = {};
	const BOOL bStarted = ::CreateProcessW(nullptr, CommandLine.GetCharArray().GetData(), nullptr, nullptr, AttributeList != nullptr, CreationFlags,
		Environment.GetData(), WorkingDirectory.IsEmpty() ? nullptr : *WorkingDirectory, &StartupInfo.StartupInfo, &ProcessInformation);
	if (AttributeList)
	{
		::DeleteProcThreadAttributeList(AttributeList);
	}
	// The block held keys: it is cleared before it goes, rather than left in freed memory.
	FMemory::Memzero(Environment.GetData(), Environment.Num() * sizeof(TCHAR));
	if (!bStarted)
	{
		UE_LOG(LogHyperAIStudioCredentials, Warning, TEXT("Could not start %s (error %u)."), *Executable, ::GetLastError());
		return FProcHandle();
	}
	::CloseHandle(ProcessInformation.hThread);
	if (OutProcessId)
	{
		*OutProcessId = ProcessInformation.dwProcessId;
	}
	return FProcHandle(ProcessInformation.hProcess);
#else
	if (!Extra.IsEmpty())
	{
		UE_LOG(LogHyperAIStudioCredentials, Warning, TEXT("Stored keys are passed to agents on Windows only; %s starts without them."), *Executable);
	}
	return FPlatformProcess::CreateProc(*Executable, *Params, false, true, true, OutProcessId, 0, WorkingDirectory.IsEmpty() ? nullptr : *WorkingDirectory,
		PipeWriteChild, PipeReadChild);
#endif
}

FString WrapCommandWithTargets(const TArray<TPair<FString, FString>>& VariableTargets, const FString& Command)
{
	if (VariableTargets.IsEmpty() || Command.IsEmpty())
	{
		return Command;
	}
	// PowerShell reads each credential through CredReadW and sets it in its own environment only; the agent's command
	// then runs from cmd, as the terminal would have run it, and inherits them. Nothing here holds a key.
	auto Quote = [](const FString& Text)
	{
		FString Out = Text;
		// PowerShell takes the typographic single quotes as quotes too; doubling any of them keeps it literal.
		for (const TCHAR* QuoteChar : {TEXT("'"), TEXT("\u2018"), TEXT("\u2019"), TEXT("\u201A"), TEXT("\u201B")})
		{
			Out.ReplaceInline(QuoteChar, *(FString(QuoteChar) + QuoteChar), ESearchCase::CaseSensitive);
		}
		return TEXT("'") + Out + TEXT("'");
	};
	FString Script;
	Script += TEXT("$s = @'\n");
	Script += TEXT("using System; using System.Runtime.InteropServices;\n");
	Script += TEXT("public static class HyperAIStudioCredential {\n");
	Script += TEXT("  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct C { public int Flags; public int Type; public IntPtr Target; public IntPtr Comment; public long Written; public int Size; public IntPtr Blob; public int Persist; public int Count; public IntPtr Attributes; public IntPtr Alias; public IntPtr User; }\n");
	Script += TEXT("  [DllImport(\"advapi32.dll\", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CredReadW(string target, int type, int flags, out IntPtr credential);\n");
	Script += TEXT("  [DllImport(\"advapi32.dll\")] static extern void CredFree(IntPtr credential);\n");
	Script += TEXT("  public static string Read(string target) { IntPtr p; if (!CredReadW(target, 1, 0, out p)) return null; try { C c = (C)Marshal.PtrToStructure(p, typeof(C)); return c.Size > 0 ? Marshal.PtrToStringUni(c.Blob, c.Size / 2) : null; } finally { CredFree(p); } }\n");
	Script += TEXT("}\n");
	Script += TEXT("'@\n");
	Script += TEXT("try {\n");
	Script += TEXT("  Add-Type -TypeDefinition $s\n");
	for (const TPair<FString, FString>& Pair : VariableTargets)
	{
		Script += FString::Printf(TEXT("  $v = [HyperAIStudioCredential]::Read(%s); if ($v) { [Environment]::SetEnvironmentVariable(%s, $v) }\n"),
			*Quote(Pair.Value), *Quote(Pair.Key));
	}
	Script += TEXT("  $v = $null\n");
	Script += TEXT("} catch { Write-Host 'HyperAI could not read the stored keys, so the agent starts without them.' }\n");
	Script += TEXT("$env:HYPERAI_AGENT_LAUNCH = ") + Quote(Command) + TEXT("\n");
	Script += TEXT("$shell = if ($env:ComSpec) { $env:ComSpec } else { 'cmd.exe' }\n");
	Script += TEXT("& $shell /d /c '%HYPERAI_AGENT_LAUNCH%'\n");
	Script += TEXT("exit $LASTEXITCODE\n");
	TArray<uint8> Bytes;
	Bytes.Reserve(Script.Len() * 2);
	for (const TCHAR C : Script)
	{
		const uint16 Unit = static_cast<uint16>(C);
		Bytes.Add(static_cast<uint8>(Unit & 0xFF));
		Bytes.Add(static_cast<uint8>(Unit >> 8));
	}
	const FString Wrapped = TEXT("powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -EncodedCommand ") + FBase64::Encode(Bytes);
	// cmd.exe takes 8191 characters on a line, with the bootstrap around it.
	if (Wrapped.Len() > 7600)
	{
		UE_LOG(LogHyperAIStudioCredentials, Warning, TEXT("The agent's launch is too long to pass stored keys with; it starts without them."));
		return Command;
	}
	return Wrapped;
}

FString WrapTerminalCommand(const FString& AgentName, const FString& Command)
{
	return WrapCommandWithTargets(TargetsFor(AgentName), Command);
}

TArray<TPair<FString, FString>> TargetsFor(const FString& AgentName)
{
	TArray<TPair<FString, FString>> Targets;
	for (const FHyperAIStudioKeySlot& Slot : Slots())
	{
		if (!Slot.Agent.Equals(AgentName, ESearchCase::IgnoreCase) || ReadSlot(Slot.Id).TrimStartAndEnd().IsEmpty())
		{
			continue;
		}
		Targets.Emplace(Slot.Variable, TargetName(Slot.Id));
		for (const FString& Alias : Slot.Aliases)
		{
			Targets.Emplace(Alias, TargetName(Slot.Id));
		}
	}
	return Targets;
}

bool BuildTestRequest(const FHyperAIStudioKeySlot& Slot, const FString& Key, const FString& BaseUrl, FHyperAIStudioKeyTestRequest& OutRequest)
{
	OutRequest = FHyperAIStudioKeyTestRequest();
	if (Key.IsEmpty() || Slot.Test.IsEmpty())
	{
		return false;
	}
	FString Base = BaseUrl.TrimStartAndEnd();
	Base.RemoveFromEnd(TEXT("/"));
	if (Slot.Test == TEXT("anthropic"))
	{
		OutRequest.Url = (Base.IsEmpty() ? FString(TEXT("https://api.anthropic.com")) : Base) + TEXT("/v1/models");
		OutRequest.Headers.Add(TEXT("x-api-key"), Key);
		OutRequest.Headers.Add(TEXT("anthropic-version"), TEXT("2023-06-01"));
	}
	else if (Slot.Test == TEXT("openai"))
	{
		OutRequest.Url = (Base.IsEmpty() ? FString(TEXT("https://api.openai.com/v1")) : Base) + TEXT("/models");
		OutRequest.Headers.Add(TEXT("Authorization"), TEXT("Bearer ") + Key);
	}
	else if (Slot.Test == TEXT("gemini"))
	{
		OutRequest.Url = TEXT("https://generativelanguage.googleapis.com/v1beta/models");
		OutRequest.Headers.Add(TEXT("x-goog-api-key"), Key);
	}
	else if (Slot.Test == TEXT("openrouter"))
	{
		OutRequest.Url = TEXT("https://openrouter.ai/api/v1/key");
		OutRequest.Headers.Add(TEXT("Authorization"), TEXT("Bearer ") + Key);
	}
	else if (Slot.Test == TEXT("github"))
	{
		OutRequest.Url = TEXT("https://api.github.com/user");
		OutRequest.Headers.Add(TEXT("Authorization"), TEXT("Bearer ") + Key);
		OutRequest.Headers.Add(TEXT("Accept"), TEXT("application/vnd.github+json"));
	}
	else if (Slot.Test == TEXT("elevenlabs"))
	{
		OutRequest.Url = TEXT("https://api.elevenlabs.io/v1/user");
		OutRequest.Headers.Add(TEXT("xi-api-key"), Key);
	}
	else if (Slot.Test == TEXT("meshy"))
	{
		OutRequest.Url = TEXT("https://api.meshy.ai/openapi/v1/balance");
		OutRequest.Headers.Add(TEXT("Authorization"), TEXT("Bearer ") + Key);
	}
	else if (Slot.Test == TEXT("tripo"))
	{
		// The same address generation uses (a project's own tripo entry wins). openapi.tripo3d.com is the China site,
		// which refuses an international key with 401.
		FHyperAIStudioExternalServiceSettings Tripo;
		FString TripoBase = FHyperAIStudioExternalServices::Find(TEXT("tripo"), Tripo) ? Tripo.BaseUrl.TrimStartAndEnd() : FString();
		if (TripoBase.IsEmpty())
		{
			TripoBase = TEXT("https://openapi.tripo3d.ai/v3");
		}
		TripoBase.RemoveFromEnd(TEXT("/"));
		OutRequest.Url = TripoBase + TEXT("/account/balance");
		OutRequest.Headers.Add(TEXT("Authorization"), TEXT("Bearer ") + Key);
	}
	else if (Slot.Test == TEXT("stability"))
	{
		// Stability's account read; its documentation page did not load when this was checked, but the address answers.
		OutRequest.Url = TEXT("https://api.stability.ai/v1/user/account");
		OutRequest.Headers.Add(TEXT("Authorization"), TEXT("Bearer ") + Key);
	}
	else
	{
		return false;
	}
	return true;
}

void TestSlot(const FString& SlotId, TFunction<void(bool bOk, const FString& Message)> OnDone)
{
	FHyperAIStudioKeySlot Slot;
	if (!FindSlot(SlotId, Slot))
	{
		OnDone(false, TEXT("There is no such key."));
		return;
	}
	FString Key = ReadSlot(Slot.Id).TrimStartAndEnd();
	if (Key.IsEmpty())
	{
		Key = FPlatformMisc::GetEnvironmentVariable(*Slot.Variable).TrimStartAndEnd();
	}
	if (Key.IsEmpty())
	{
		OnDone(false, FString::Printf(TEXT("Nothing to test: no key is stored, and %s is not set."), *Slot.Variable));
		return;
	}
	// The same agent's base URL, when it has one stored, is where its key is meant for.
	FString BaseUrl;
	for (const FHyperAIStudioKeySlot& Other : Slots())
	{
		if (!Other.bSecret && !Slot.Agent.IsEmpty() && Other.Agent == Slot.Agent)
		{
			BaseUrl = ReadSlot(Other.Id);
		}
	}
	FHyperAIStudioKeyTestRequest Built;
	if (!BuildTestRequest(Slot, Key, Slot.Variable.Contains(TEXT("HOST")) ? FString() : BaseUrl, Built))
	{
		OnDone(false, FString::Printf(TEXT("%s has no free check HyperAI knows of; the agent will tell you if the key is wrong."), *Slot.Label));
		return;
	}
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetVerb(TEXT("GET"));
	Request->SetURL(Built.Url);
	for (const TPair<FString, FString>& Header : Built.Headers)
	{
		Request->SetHeader(Header.Key, Header.Value);
	}
	Request->SetHeader(TEXT("User-Agent"), TEXT("HyperAIStudio"));
	Request->SetTimeout(15.0f);
	const FString Label = Slot.Label;
	Request->OnProcessRequestComplete().BindLambda([OnDone, Label](FHttpRequestPtr, FHttpResponsePtr Response, const bool bConnected)
	{
		if (!bConnected || !Response.IsValid())
		{
			OnDone(false, FString::Printf(TEXT("%s: no answer from the service. Check the address and your connection."), *Label));
			return;
		}
		const int32 Code = Response->GetResponseCode();
		if (Code >= 200 && Code < 300)
		{
			OnDone(true, FString::Printf(TEXT("%s works (HTTP %d)."), *Label, Code));
		}
		else if (Code == 401 || Code == 403)
		{
			OnDone(false, FString::Printf(TEXT("%s was refused (HTTP %d): the key is wrong, expired or lacks access."), *Label, Code));
		}
		else
		{
			OnDone(false, FString::Printf(TEXT("%s: the service answered HTTP %d."), *Label, Code));
		}
	});
	if (!Request->ProcessRequest())
	{
		OnDone(false, TEXT("The check could not be sent."));
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void SetTestScope(const FString& Prefix, const TArray<FHyperAIStudioKeySlot>& Extra)
{
	TestPrefix() = Prefix;
	TestSlots() = Extra;
}
#endif
}
