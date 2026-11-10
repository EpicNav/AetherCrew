// Games by Hyper 2026.

#include "HyperAIStudioAgentHost.h"

#include "CoreGlobals.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAgentHost, Log, All);

namespace HyperAIStudio::AgentHost::Private
{
	/** A heartbeat older than this is a host that is gone. */
	constexpr double HeartbeatSeconds = 5.0;

	struct FState
	{
		bool bExiting = false;
		FString TestRoot;
		int32 TestLingerSeconds = 3;
		double TestIdleMinutes = 30.0;
		FDelegateHandle PreExit;
		FDelegateHandle ShutdownSaved;
		TUniquePtr<FAutoConsoleCommand> StopCommand;
	};

	FState& State()
	{
		static FState Instance;
		return Instance;
	}

	FString Script()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
		return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/Scripts/HyperAIStudioAgentHost.ps1")) : FString();
	}

	FString Native(const FString& Path)
	{
		FString Out = FPaths::ConvertRelativePathToFull(Path);
		FPaths::MakePlatformFilename(Out);
		return Out;
	}

	FString Compact(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}

	TSharedPtr<FJsonObject> ReadJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		return FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) ? Object : nullptr;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	double NumberField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const double Default = 0.0)
	{
		double Value = Default;
		if (Object.IsValid())
		{
			Object->TryGetNumberField(Field, Value);
		}
		return Value;
	}

	bool IsPlainId(const FString& Id)
	{
		if (Id.IsEmpty() || Id.Len() > 64)
		{
			return false;
		}
		for (const TCHAR Char : Id)
		{
			if (!FChar::IsAlnum(Char) && Char != TEXT('-'))
			{
				return false;
			}
		}
		return true;
	}

	/** A request the host takes in name order; written whole, then renamed, so the host never reads half of one. */
	bool WriteRequest(const TSharedRef<FJsonObject>& Request)
	{
		const FString Folder = FHyperAIStudioAgentHost::GetRoot() / TEXT("requests");
		IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
		const FString Name = FString::Printf(TEXT("%020lld-%s"), FDateTime::UtcNow().GetTicks(), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FString Temp = Folder / (Name + TEXT(".tmp"));
		return FFileHelper::SaveStringToFile(Compact(Request), *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			&& IFileManager::Get().Move(*(Folder / (Name + TEXT(".json"))), *Temp, /*Replace=*/true, /*EvenIfReadOnly=*/true);
	}

	/** The editor attached to this host's agents: an agent idle with no editor to answer it is stopped after a while. */
	void WriteEditor()
	{
		const TSharedRef<FJsonObject> Editor = MakeShared<FJsonObject>();
		Editor->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
		Editor->SetStringField(TEXT("at"), FDateTime::UtcNow().ToIso8601());
		FFileHelper::SaveStringToFile(Compact(Editor), *(FHyperAIStudioAgentHost::GetRoot() / TEXT("editor.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	double SecondsSince(const FString& Iso8601)
	{
		FDateTime At;
		return !Iso8601.IsEmpty() && FDateTime::ParseIso8601(*Iso8601, At) ? (FDateTime::UtcNow() - At).GetTotalSeconds() : -1.0;
	}
}

bool FHyperAIStudioAgentHost::ShouldHost()
{
	using namespace HyperAIStudio::AgentHost::Private;
	if (!PLATFORM_WINDOWS)
	{
		return false;
	}
	if (!State().TestRoot.IsEmpty())
	{
		return true;
	}
	return GIsEditor && !IsRunningCommandlet() && !FApp::IsUnattended() && !GIsAutomationTesting
		&& GetDefault<UHyperAIStudioSettings>()->bKeepAgentsWorkingWhenEditorCloses && FPaths::FileExists(Script());
}

FString FHyperAIStudioAgentHost::GetRoot()
{
	using namespace HyperAIStudio::AgentHost::Private;
	return !State().TestRoot.IsEmpty() ? State().TestRoot
		: FPaths::ConvertRelativePathToFull(FHyperAIStudioService::GetProjectRoot() / TEXT(".hyperai/runtime/agent-host"));
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioAgentHost::SetRootForTests(const FString& Root, const int32 LingerSeconds, const double IdleMinutes)
{
	using namespace HyperAIStudio::AgentHost::Private;
	State().TestRoot = Root.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Root);
	State().TestLingerSeconds = LingerSeconds;
	State().TestIdleMinutes = IdleMinutes;
}
#endif

bool FHyperAIStudioAgentHost::IsHostRunning()
{
	using namespace HyperAIStudio::AgentHost::Private;
	const TSharedPtr<FJsonObject> Beat = ReadJson(GetRoot() / TEXT("host.json"));
	const double Age = SecondsSince(StringField(Beat, TEXT("heartbeatAt")));
	const uint32 Pid = static_cast<uint32>(NumberField(Beat, TEXT("pid")));
	return Age >= 0.0 && Age < HeartbeatSeconds && Pid != 0 && FPlatformProcess::IsApplicationRunning(Pid);
}

bool FHyperAIStudioAgentHost::EnsureRunning(FString& OutError, const double WaitSeconds)
{
	using namespace HyperAIStudio::AgentHost::Private;
	const FString Root = GetRoot();
	IFileManager::Get().MakeDirectory(*Root, /*Tree=*/true);
	WriteEditor();
	if (IsHostRunning())
	{
		return true;
	}
	const FString Path = Script();
	if (!FPaths::FileExists(Path))
	{
		OutError = TEXT("The agent host is missing from the plugin: Resources/Scripts/HyperAIStudioAgentHost.ps1.");
		return false;
	}
	const bool bTest = !State().TestRoot.IsEmpty();
	const double Idle = bTest ? State().TestIdleMinutes : static_cast<double>(GetDefault<UHyperAIStudioSettings>()->BackgroundAgentIdleMinutes);
	const int32 Linger = bTest ? State().TestLingerSeconds : 60;
	// Hidden and detached: nothing ties it to the editor, so it outlives a close or a crash.
	const FString Params = FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%s\" -Root \"%s\" -IdleMinutes %.3f -LingerSeconds %d"),
		*Native(Path), *Native(Root), Idle, Linger);
	FProcHandle Process = FPlatformProcess::CreateProc(TEXT("powershell.exe"), *Params, /*bLaunchDetached=*/true, /*bLaunchHidden=*/true,
		/*bLaunchReallyHidden=*/true, nullptr, 0, *Root, nullptr);
	if (!Process.IsValid())
	{
		OutError = TEXT("The agent host did not start.");
		return false;
	}
	FPlatformProcess::CloseProc(Process);
	const double End = FPlatformTime::Seconds() + WaitSeconds;
	while (FPlatformTime::Seconds() < End)
	{
		if (IsHostRunning())
		{
			UE_LOG(LogHyperAIStudioAgentHost, Display, TEXT("The agent host is running for %s."), *Root);
			return true;
		}
		FPlatformProcess::Sleep(0.05f);
	}
	OutError = FString::Printf(TEXT("The agent host did not answer within %.0f s."), WaitSeconds);
	return false;
}

FString FHyperAIStudioAgentHost::NewSessionId()
{
	return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
}

bool FHyperAIStudioAgentHost::StartSession(const FString& Id, const FString& Command, const FString& WorkingDirectory,
	const TArray<TPair<FString, FString>>& Targets, const TMap<FString, FString>& Environment, FString& OutError)
{
	using namespace HyperAIStudio::AgentHost::Private;
	if (!IsPlainId(Id))
	{
		OutError = TEXT("A hosted session needs a plain id.");
		return false;
	}
	IFileManager::Get().MakeDirectory(*(GetRoot() / TEXT("sessions") / Id), /*Tree=*/true);
	const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("op"), TEXT("start"));
	Request->SetStringField(TEXT("session"), Id);
	Request->SetStringField(TEXT("command"), Command);
	Request->SetStringField(TEXT("cwd"), Native(WorkingDirectory));
	const TSharedRef<FJsonObject> Env = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Environment)
	{
		Env->SetStringField(Pair.Key, Pair.Value);
	}
	Request->SetObjectField(TEXT("env"), Env);
	// Where the keys are, never the keys: the host reads them from Credential Manager itself.
	TArray<TSharedPtr<FJsonValue>> TargetValues;
	for (const TPair<FString, FString>& Pair : Targets)
	{
		const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("variable"), Pair.Key);
		Target->SetStringField(TEXT("target"), Pair.Value);
		TargetValues.Add(MakeShared<FJsonValueObject>(Target));
	}
	Request->SetArrayField(TEXT("targets"), TargetValues);
	if (!WriteRequest(Request))
	{
		OutError = TEXT("The request to the agent host could not be written.");
		return false;
	}
	return true;
}

void FHyperAIStudioAgentHost::StopSession(const FString& Id, const FString& Reason, const bool bForget)
{
	using namespace HyperAIStudio::AgentHost::Private;
	if (!IsPlainId(Id))
	{
		return;
	}
	const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("op"), TEXT("stop"));
	Request->SetStringField(TEXT("session"), Id);
	Request->SetStringField(TEXT("reason"), Reason);
	Request->SetBoolField(TEXT("forget"), bForget);
	WriteRequest(Request);
	FHyperAIStudioHostSessionMeta Meta;
	if (bForget && ReadMeta(Id, Meta))
	{
		// Also for a host that is gone: the next one never takes the chat up again.
		Meta.bClosed = true;
		WriteMeta(Meta);
	}
}

int32 FHyperAIStudioAgentHost::StopAll(const FString& Reason)
{
	using namespace HyperAIStudio::AgentHost::Private;
	const int32 Running = CountRunning();
	if (IsHostRunning())
	{
		const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("op"), TEXT("stop-all"));
		Request->SetStringField(TEXT("reason"), Reason);
		WriteRequest(Request);
	}
	UE_LOG(LogHyperAIStudioAgentHost, Display, TEXT("Asked the agent host to stop %d agent(s): %s"), Running, *Reason);
	return Running;
}

bool FHyperAIStudioAgentHost::Append(const FString& Id, const TCHAR Kind, const FString& Line)
{
	using namespace HyperAIStudio::AgentHost::Private;
	if (!IsPlainId(Id))
	{
		return false;
	}
	// One line: a newline inside would split it, and JSON never needs one.
	FString Text = FString::Printf(TEXT("%c\t%s"), Kind, *Line.Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(" ")));
	Text += TEXT("\n");
	const FTCHARToUTF8 Utf8(*Text);
	const FString Path = GetRoot() / TEXT("sessions") / Id / TEXT("input.jsonl");
#if PLATFORM_WINDOWS
	// Appended in one write with the file shared, as the host reads it while it grows.
	HANDLE File = ::CreateFileW(*Native(Path), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (File == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	DWORD Written = 0;
	const bool bOk = ::WriteFile(File, Utf8.Get(), static_cast<DWORD>(Utf8.Length()), &Written, nullptr) && Written == static_cast<DWORD>(Utf8.Length());
	::CloseHandle(File);
	return bOk;
#else
	return false;
#endif
}

bool FHyperAIStudioAgentHost::ReadState(const FString& Id, FHyperAIStudioHostSessionState& Out)
{
	using namespace HyperAIStudio::AgentHost::Private;
	const TSharedPtr<FJsonObject> Json = IsPlainId(Id) ? ReadJson(GetRoot() / TEXT("sessions") / Id / TEXT("state.json")) : nullptr;
	if (!Json.IsValid())
	{
		return false;
	}
	Out = FHyperAIStudioHostSessionState();
	Out.State = StringField(Json, TEXT("state"));
	Out.ProcessId = static_cast<int32>(NumberField(Json, TEXT("pid")));
	double Code = 0.0;
	Out.bHasExitCode = Json->TryGetNumberField(TEXT("exitCode"), Code);
	Out.ExitCode = static_cast<int32>(Code);
	Out.Reason = StringField(Json, TEXT("reason"));
	Out.LastActivityAt = StringField(Json, TEXT("lastActivityAt"));
	return true;
}

bool FHyperAIStudioAgentHost::ReadMeta(const FString& Id, FHyperAIStudioHostSessionMeta& Out)
{
	using namespace HyperAIStudio::AgentHost::Private;
	const TSharedPtr<FJsonObject> Json = IsPlainId(Id) ? ReadJson(GetRoot() / TEXT("sessions") / Id / TEXT("meta.json")) : nullptr;
	if (!Json.IsValid())
	{
		return false;
	}
	Out = FHyperAIStudioHostSessionMeta();
	Out.Id = Id;
	Out.Agent = StringField(Json, TEXT("agent"));
	Out.Model = StringField(Json, TEXT("model"));
	Out.SessionId = StringField(Json, TEXT("sessionId"));
	Out.ResumeSessionId = StringField(Json, TEXT("resumeSessionId"));
	Out.Subagent = StringField(Json, TEXT("subagent"));
	Out.Title = StringField(Json, TEXT("title"));
	Out.ProjectFile = StringField(Json, TEXT("projectFile"));
	Out.TabIndex = static_cast<int32>(NumberField(Json, TEXT("tab")));
	Out.EditorProcessId = static_cast<uint32>(NumberField(Json, TEXT("editorPid")));
	Json->TryGetBoolField(TEXT("closed"), Out.bClosed);
	Json->TryGetBoolField(TEXT("busyAtDetach"), Out.bBusyAtDetach);
	Out.DetachedAt = StringField(Json, TEXT("detachedAt"));
	return true;
}

bool FHyperAIStudioAgentHost::WriteMeta(const FHyperAIStudioHostSessionMeta& Meta)
{
	using namespace HyperAIStudio::AgentHost::Private;
	if (!IsPlainId(Meta.Id))
	{
		return false;
	}
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("agent"), Meta.Agent);
	Json->SetStringField(TEXT("model"), Meta.Model);
	Json->SetStringField(TEXT("sessionId"), Meta.SessionId);
	Json->SetStringField(TEXT("resumeSessionId"), Meta.ResumeSessionId);
	Json->SetStringField(TEXT("subagent"), Meta.Subagent);
	Json->SetStringField(TEXT("title"), Meta.Title);
	Json->SetStringField(TEXT("projectFile"), Meta.ProjectFile);
	Json->SetNumberField(TEXT("tab"), Meta.TabIndex);
	Json->SetNumberField(TEXT("editorPid"), Meta.EditorProcessId);
	Json->SetBoolField(TEXT("closed"), Meta.bClosed);
	Json->SetBoolField(TEXT("busyAtDetach"), Meta.bBusyAtDetach);
	Json->SetStringField(TEXT("detachedAt"), Meta.DetachedAt);
	const FString Folder = GetRoot() / TEXT("sessions") / Meta.Id;
	IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
	// Whole or not at all: the host and a later editor read it at any moment.
	const FString Temp = Folder / TEXT("meta.json.tmp");
	return FFileHelper::SaveStringToFile(Compact(Json), *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		&& IFileManager::Get().Move(*(Folder / TEXT("meta.json")), *Temp, /*Replace=*/true, /*EvenIfReadOnly=*/true);
}

TArray<FString> FHyperAIStudioAgentHost::ListSessions()
{
	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *(GetRoot() / TEXT("sessions") / TEXT("*")), /*Files=*/false, /*Directories=*/true);
	Names.Sort();
	return Names;
}

int32 FHyperAIStudioAgentHost::CountRunning()
{
	if (!IsHostRunning())
	{
		return 0;
	}
	int32 Running = 0;
	for (const FString& Id : ListSessions())
	{
		FHyperAIStudioHostSessionState Session;
		Running += ReadState(Id, Session) && Session.IsRunning() ? 1 : 0;
	}
	return Running;
}

FString FHyperAIStudioAgentHost::StreamPath(const FString& Id)
{
	return GetRoot() / TEXT("sessions") / Id / TEXT("stream.log");
}

bool FHyperAIStudioAgentHost::ParseLine(const FString& Line, FHyperAIStudioHostLine& Out)
{
	if (Line.Len() < 2 || Line[1] != TEXT('\t') || !FCString::Strchr(TEXT("oeiux"), Line[0]))
	{
		return false;
	}
	Out.Kind = Line[0];
	Out.Body = Line.Mid(2);
	return true;
}

bool FHyperAIStudioAgentHost::IsDetached(const FHyperAIStudioHostSessionMeta& Meta, const FHyperAIStudioHostSessionState& State, const bool bHasState,
	const FString& ProjectFile, const bool bOwnerRunning)
{
	// Another editor shows it, the chat was closed, it is another project's, or it never started.
	if (Meta.bClosed || Meta.Agent.IsEmpty() || bOwnerRunning || !bHasState || !FPaths::IsSamePath(Meta.ProjectFile, ProjectFile))
	{
		return false;
	}
	// Running, or ended while no editor showed it, so its end (and what came before) is still news.
	return State.IsRunning() || !Meta.DetachedAt.IsEmpty() || Meta.EditorProcessId != 0;
}

TArray<FHyperAIStudioHostSessionMeta> FHyperAIStudioAgentHost::FindDetachedSessions(const FString& ProjectFile, const TSet<FString>& Skip)
{
	TArray<FHyperAIStudioHostSessionMeta> Found;
	const uint32 Self = FPlatformProcess::GetCurrentProcessId();
	for (const FString& Id : ListSessions())
	{
		FHyperAIStudioHostSessionMeta Meta;
		FHyperAIStudioHostSessionState Session;
		if (Skip.Contains(Id) || !ReadMeta(Id, Meta))
		{
			continue;
		}
		const bool bHasState = ReadState(Id, Session);
		const bool bOwnerRunning = Meta.EditorProcessId != 0 && Meta.EditorProcessId != Self && FPlatformProcess::IsApplicationRunning(Meta.EditorProcessId);
		if (IsDetached(Meta, Session, bHasState, ProjectFile, bOwnerRunning))
		{
			Found.Add(Meta);
		}
	}
	Found.StableSort([](const FHyperAIStudioHostSessionMeta& A, const FHyperAIStudioHostSessionMeta& B) { return A.TabIndex < B.TabIndex; });
	return Found;
}

bool FHyperAIStudioAgentHost::IsEditorExiting()
{
	return HyperAIStudio::AgentHost::Private::State().bExiting || IsEngineExitRequested();
}

void FHyperAIStudioAgentHost::SetEditorExiting(const bool bExiting)
{
	HyperAIStudio::AgentHost::Private::State().bExiting = bExiting;
}

void FHyperAIStudioAgentHost::Startup()
{
	using namespace HyperAIStudio::AgentHost::Private;
	FState& Current = State();
	// Set before the chat tabs go: a chat that works when its tab is destroyed by the close detaches instead of stopping.
	Current.ShutdownSaved = FEditorDelegates::OnShutdownPostPackagesSaved.AddLambda([]() { SetEditorExiting(); });
	Current.PreExit = FCoreDelegates::OnEnginePreExit.AddLambda([]() { SetEditorExiting(); });
	Current.StopCommand = MakeUnique<FAutoConsoleCommand>(TEXT("HyperAIStudio.StopBackgroundAgents"),
		TEXT("Stop every agent HyperAI's background host runs for this project, as the chat menu's Stop Background Agents does."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			UE_LOG(LogHyperAIStudioAgentHost, Display, TEXT("%d background agent(s) asked to stop."), FHyperAIStudioAgentHost::StopAll(TEXT("stopped from the editor")));
		}));
	// A host already running for this project learns which editor is back, so it no longer counts its agents as unattended.
	if (ShouldHost() && FPaths::DirectoryExists(GetRoot()))
	{
		WriteEditor();
	}
}

void FHyperAIStudioAgentHost::Shutdown()
{
	using namespace HyperAIStudio::AgentHost::Private;
	FState& Current = State();
	if (Current.ShutdownSaved.IsValid())
	{
		FEditorDelegates::OnShutdownPostPackagesSaved.Remove(Current.ShutdownSaved);
		Current.ShutdownSaved.Reset();
	}
	if (Current.PreExit.IsValid())
	{
		FCoreDelegates::OnEnginePreExit.Remove(Current.PreExit);
		Current.PreExit.Reset();
	}
	Current.StopCommand.Reset();
}

// ---------------------------------------------------------------------------------------------------------------- reader

FHyperAIStudioHostStreamReader::FHyperAIStudioHostStreamReader(const FString& InPath)
	: Path(InPath)
{
}

FHyperAIStudioHostStreamReader::~FHyperAIStudioHostStreamReader()
{
#if PLATFORM_WINDOWS
	if (Handle)
	{
		::CloseHandle(static_cast<HANDLE>(Handle));
	}
#endif
}

void FHyperAIStudioHostStreamReader::Read(TArray<FString>& OutLines)
{
#if PLATFORM_WINDOWS
	if (!Handle)
	{
		// Shared every way: the host appends to it, and may remove a finished session's folder.
		FString NativePath = FPaths::ConvertRelativePathToFull(Path);
		FPaths::MakePlatformFilename(NativePath);
		const HANDLE File = ::CreateFileW(*NativePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (File == INVALID_HANDLE_VALUE)
		{
			return;
		}
		Handle = File;
	}
	uint8 Buffer[64 * 1024];
	DWORD Read = 0;
	// Reading on from where the last read ended; at the end of the file a read returns nothing until more is written.
	while (::ReadFile(static_cast<HANDLE>(Handle), Buffer, sizeof(Buffer), &Read, nullptr) && Read > 0)
	{
		Pending.Append(Buffer, Read);
	}
	int32 Start = 0;
	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		if (Pending[Index] != '\n')
		{
			continue;
		}
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Pending.GetData() + Start), Index - Start);
		FString Line(Converted.Length(), Converted.Get());
		Line.TrimEndInline();
		if (!Line.IsEmpty())
		{
			OutLines.Add(MoveTemp(Line));
		}
		Start = Index + 1;
	}
	if (Start > 0)
	{
		Pending.RemoveAt(0, Start, EAllowShrinking::No);
	}
#endif
}
