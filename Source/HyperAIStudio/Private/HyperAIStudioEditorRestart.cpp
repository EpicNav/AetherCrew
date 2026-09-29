// Games by Hyper 2026.

#include "HyperAIStudioEditorRestart.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioShell.h"
#include "Interfaces/IMainFrameModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SHyperAIStudioQuickActionWindow.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioRestart, Log, All);

namespace HyperAIStudio::EditorRestart::Private
{
	constexpr const TCHAR* PackId = TEXT("editor_lifecycle");
	constexpr const TCHAR* CohortId = TEXT("cohort.source.hyperaistudioeditorrestarttoolset.v1");
	constexpr const TCHAR* ToolName = TEXT("hyper_editor_restart");
	constexpr const TCHAR* QualifiedToolset = TEXT("HyperAIStudio.HyperAIStudioEditorRestartToolset");
	/** How long the helper waits for the editor to close before it gives up. */
	constexpr int32 CloseTimeoutSeconds = 120;
	/** A restart older than this is not resumed: a full rebuild can take hours, a forgotten plan should not come back. */
	constexpr double MaxResumeAgeSeconds = 6.0 * 60.0 * 60.0;
	/** An editor opened without the token takes a crash's chats only this soon after it: Send and Restart, or a quick reopen by hand. */
	constexpr double MaxCrashAgeWithoutTokenSeconds = 10.0 * 60.0;
	/** The editor waits this long before it closes, so the tool's answer reaches the agent first. */
	constexpr float CloseDelaySeconds = 2.0f;
	/** A close still not under way this long after it was asked for was cancelled. */
	constexpr double CancelAfterSeconds = 10.0;
	constexpr int32 MaxNoteCharacters = 4000;

	/** How often the crash watch looks for changed chats. */
	constexpr float WatchSeconds = 5.0f;

	struct FState
	{
		bool bRestarting = false;
		FProcHandle Helper;
		/** After a restart: the chats still waiting for their tab. */
		TArray<FHyperAIStudioRestartChat> Chats;
		/** After a restart: every restored chat's tab, the one told to continue last. */
		TArray<int32> Pages;
		TUniquePtr<FAutoConsoleCommand> Command;
		FDelegateHandle PostEngineInit;
		FDelegateHandle MainFrameReady;
		FDelegateHandle PreExit;
		bool bOwnsToolset = false;
		/** The crash watch: the plan the watchdog hands over if this editor crashes, and where it is kept. */
		TSharedPtr<FJsonObject> WatchPlan;
		FString WatchPath;
		FString WatchText;
		FTSTicker::FDelegateHandle WatchTicker;
	};

	FState& State()
	{
		static FState Instance;
		return Instance;
	}

	FString Script(const TCHAR* Name)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
		return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/Scripts") / Name) : FString();
	}

	FString HelperScript()
	{
		return Script(TEXT("HyperAIStudioRestart.ps1"));
	}

	/** Saved/HyperAIStudio/<Name>: Restart for a restart's plan, Recovery for the crash watch's. */
	FString StudioFolder(const TCHAR* Name)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio") / Name);
	}

	/** An editor a person works in: not a commandlet, a test run or a build machine, which nobody wants reopened. */
	bool IsInteractiveEditor()
	{
		return GIsEditor && !IsRunningCommandlet() && !FApp::IsUnattended() && !GIsAutomationTesting && !GIsBuildMachine;
	}

	FString Native(const FString& Path)
	{
		FString Out = FPaths::ConvertRelativePathToFull(Path);
		FPaths::MakePlatformFilename(Out);
		return Out;
	}

	/** The command line without an earlier restart's token, so tokens never pile up. */
	FString WithoutResumeSwitch(FString Line)
	{
		for (int32 At = Line.Find(TEXT("-HyperAIResume=")); At != INDEX_NONE; At = Line.Find(TEXT("-HyperAIResume=")))
		{
			int32 End = At;
			while (End < Line.Len() && !FChar::IsWhitespace(Line[End]))
			{
				++End;
			}
			Line.RemoveAt(At, End - At);
		}
		return Line.TrimStartAndEnd();
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

	TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Field, Found) && Found ? *Found : nullptr;
	}

	TArray<FString> StringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<FString> Values;
		if (Object.IsValid())
		{
			Object->TryGetStringArrayField(Field, Values);
		}
		return Values;
	}

	bool WriteJson(const FString& Path, const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		return FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text))
			&& FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	TSharedPtr<FJsonObject> ReadJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		return FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) ? Object : nullptr;
	}

	TSharedRef<FJsonObject> ChatToJson(const FHyperAIStudioRestartChat& Chat)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("tab"), Chat.TabIndex);
		Object->SetStringField(TEXT("agent"), Chat.Agent);
		Object->SetStringField(TEXT("sessionId"), Chat.SessionId);
		Object->SetStringField(TEXT("model"), Chat.Model);
		Object->SetStringField(TEXT("subagent"), Chat.Subagent);
		Object->SetStringField(TEXT("title"), Chat.Title);
		Object->SetStringField(TEXT("backgroundId"), Chat.BackgroundId);
		Object->SetBoolField(TEXT("working"), Chat.bWorking);
		return Object;
	}

	FHyperAIStudioRestartChat ChatFromJson(const TSharedPtr<FJsonObject>& Object)
	{
		FHyperAIStudioRestartChat Chat;
		double Tab = 1.0;
		if (Object.IsValid())
		{
			Object->TryGetNumberField(TEXT("tab"), Tab);
			Object->TryGetBoolField(TEXT("working"), Chat.bWorking);
		}
		Chat.TabIndex = FMath::Max(1, static_cast<int32>(Tab));
		Chat.Agent = StringField(Object, TEXT("agent"));
		Chat.SessionId = StringField(Object, TEXT("sessionId"));
		Chat.Model = StringField(Object, TEXT("model"));
		Chat.Subagent = StringField(Object, TEXT("subagent"));
		Chat.Title = StringField(Object, TEXT("title"));
		Chat.BackgroundId = StringField(Object, TEXT("backgroundId"));
		return Chat;
	}

	/** How this editor builds and opens again: the part of the plan a restart and a crash share. */
	struct FReopen
	{
		FString Target;
		FString ProjectFile;
		FString BuildLine;
		FString Executable;
		FString Arguments;
	};

	FReopen GetReopen(const FString& Token)
	{
		FReopen Out;
		Out.Target = FPlatformMisc::GetUBTTargetName();
		Out.ProjectFile = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		// The running editor's own target, platform and configuration, built as the bench and Build.bat users do.
		Out.BuildLine = FString::Printf(TEXT("\"%s\" %s %s %s \"-Project=%s\" -WaitMutex -NoHotReloadFromIDE"),
			*Native(FPaths::EngineDir() / TEXT("Build/BatchFiles/Build.bat")), *Out.Target, FPlatformMisc::GetUBTPlatform(),
			LexToString(FApp::GetBuildConfiguration()), *Native(Out.ProjectFile));
		// What FUnrealEdMisc::SpawnEditorInstance runs for File > Restart, plus the token.
		Out.Executable = FPlatformProcess::ExecutablePath();
		Out.Arguments = FString::Printf(TEXT("\"%s\" %s -HyperAIResume=%s"), *Out.ProjectFile, *WithoutResumeSwitch(FCommandLine::Get()), *Token);
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> ChatsToJson(const TArray<FHyperAIStudioRestartChat>& Chats)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FHyperAIStudioRestartChat& Chat : Chats)
		{
			Values.Add(MakeShared<FJsonValueObject>(ChatToJson(Chat)));
		}
		return Values;
	}

	/** The plan the helper script follows, with its build script written beside it in Saved/HyperAIStudio/<FolderName>. */
	TSharedRef<FJsonObject> MakePlan(const FString& Token, const FReopen& Reopen, const TCHAR* FolderName, const bool bBuild,
		const TArray<FHyperAIStudioRestartChat>& Chats)
	{
		const FString Folder = StudioFolder(FolderName);
		IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
		const FString BuildScript = Folder / TEXT("build.cmd");
		FFileHelper::SaveStringToFile(TEXT("@echo off\r\ncall ") + Reopen.BuildLine + TEXT("\r\nexit /b %ERRORLEVEL%\r\n"), *BuildScript);
		const TSharedRef<FJsonObject> Build = MakeShared<FJsonObject>();
		Build->SetBoolField(TEXT("enabled"), bBuild);
		Build->SetStringField(TEXT("target"), Reopen.Target);
		Build->SetStringField(TEXT("script"), Native(BuildScript));
		Build->SetStringField(TEXT("log"), Native(Folder / TEXT("build.log")));
		Build->SetStringField(TEXT("logRelative"), FString::Printf(TEXT("Saved/HyperAIStudio/%s/build.log"), FolderName));
		const TSharedRef<FJsonObject> Relaunch = MakeShared<FJsonObject>();
		Relaunch->SetStringField(TEXT("executable"), Native(Reopen.Executable));
		Relaunch->SetStringField(TEXT("arguments"), Reopen.Arguments);
		Relaunch->SetStringField(TEXT("workingDirectory"), Native(FPaths::ProjectDir()));
		const TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("token"), Token);
		Plan->SetStringField(TEXT("project"), FApp::GetProjectName());
		Plan->SetStringField(TEXT("projectFile"), Native(Reopen.ProjectFile));
		Plan->SetNumberField(TEXT("editorProcessId"), FPlatformProcess::GetCurrentProcessId());
		Plan->SetNumberField(TEXT("closeTimeoutSeconds"), CloseTimeoutSeconds);
		Plan->SetBoolField(TEXT("sendCrashReport"), GetDefault<UHyperAIStudioSettings>()->bSendCrashReportForMe);
		Plan->SetStringField(TEXT("requestedAt"), FDateTime::UtcNow().ToIso8601());
		Plan->SetObjectField(TEXT("build"), Build);
		Plan->SetObjectField(TEXT("relaunch"), Relaunch);
		Plan->SetArrayField(TEXT("chats"), ChatsToJson(Chats));
		return Plan;
	}

	FString DescribeChat(const FHyperAIStudioRestartChat& Chat)
	{
		const FString How = !Chat.BackgroundId.IsEmpty() ? FString(TEXT("attaches to its background session again"))
			: Chat.SessionId.IsEmpty() ? FString(TEXT("starts a new conversation, since its id is not known"))
			: FString::Printf(TEXT("resumes %s"), *Chat.SessionId);
		return FString::Printf(TEXT("Chat %d, %s: %s%s"), Chat.TabIndex, *Chat.Agent, *How, Chat.bWorking ? TEXT(", and is told to continue") : TEXT(""));
	}

	void EndRestart()
	{
		FState& Current = State();
		if (Current.Helper.IsValid())
		{
			FPlatformProcess::CloseProc(Current.Helper);
		}
		Current.Helper = FProcHandle();
		Current.bRestarting = false;
	}

	/** The editor did not close: stop the helper, so a later close is not followed by a build and a relaunch. */
	void CancelRestart()
	{
		if (State().Helper.IsValid() && FPlatformProcess::IsProcRunning(State().Helper))
		{
			FPlatformProcess::TerminateProc(State().Helper, /*KillTree=*/true);
		}
		EndRestart();
		UE_LOG(LogHyperAIStudioRestart, Warning, TEXT("The editor did not close (the close was cancelled), so the restart stopped."));
	}

	/** The way File > Restart closes the editor: it asks the editor's own questions, and a No keeps it open. */
	void CloseEditor()
	{
		if (!State().bRestarting || !GEngine)
		{
			return;
		}
		GEngine->DeferredCommands.Add(TEXT("CLOSE_SLATE_MAINFRAME"));
		const double Asked = FPlatformTime::Seconds();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Asked](float)
		{
			if (!State().bRestarting || IsEngineExitRequested())
			{
				return false;
			}
			if (FPlatformTime::Seconds() - Asked < CancelAfterSeconds)
			{
				return true;
			}
			CancelRestart();
			return false;
		}), 0.5f);
	}

	/**
	 * Shows every restored chat's tab: one the Studio layout did not bring back opens, and one it did comes forward,
	 * since a tab behind another page never ticks and its agent would never start. The one told to continue is last,
	 * so it ends in front.
	 */
	void ShowChats()
	{
		for (const int32 TabIndex : State().Pages)
		{
			const bool bShown = FHyperAIStudioShell::Open(SHyperAIStudioQuickActionWindow::GetPageId(TabIndex)).IsValid();
			UE_LOG(LogHyperAIStudioRestart, Display, TEXT("Chat tab %d %s after the restart."), TabIndex, bShown ? TEXT("shown") : TEXT("could not be opened"));
		}
		State().Pages.Reset();
		if (!State().Chats.IsEmpty())
		{
			UE_LOG(LogHyperAIStudioRestart, Warning, TEXT("%d chat(s) from before the restart found no tab and start fresh."), State().Chats.Num());
			State().Chats.Reset();
		}
	}

	/** Keeps the crash watch's chats current, writing the plan only when they change. */
	bool RefreshWatch(float)
	{
		FState& Current = State();
		if (!Current.WatchPlan.IsValid())
		{
			return false;
		}
		Current.WatchPlan->SetArrayField(TEXT("chats"), ChatsToJson(SHyperAIStudioQuickActionWindow::GetRestartChats()));
		FString Text;
		FJsonSerializer::Serialize(Current.WatchPlan.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
		if (Text != Current.WatchText && FFileHelper::SaveStringToFile(Text, *Current.WatchPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Current.WatchText = MoveTemp(Text);
		}
		return true;
	}

	/** A clean exit: the plan goes, so the watchdog knows there is nothing to reopen. */
	void EndCrashWatch()
	{
		FState& Current = State();
		if (Current.WatchTicker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Current.WatchTicker);
			Current.WatchTicker.Reset();
		}
		if (!Current.WatchPath.IsEmpty())
		{
			IFileManager::Get().Delete(*Current.WatchPath, false, true, true);
		}
		Current.WatchPlan.Reset();
		Current.WatchPath.Reset();
		Current.WatchText.Reset();
	}

	/**
	 * Once the editor is up: a hidden watchdog that outlives it, and the plan it follows if the editor crashes. The plan
	 * is a restart's (build, open the project with a token, give the chats back), kept current under Recovery.
	 */
	void StartCrashWatch()
	{
		FState& Current = State();
		const FString Watchdog = Script(TEXT("HyperAIStudioWatchdog.ps1"));
		if (!PLATFORM_WINDOWS || Current.WatchPlan.IsValid() || !IsInteractiveEditor() || !GetDefault<UHyperAIStudioSettings>()->bReopenEditorAfterCrash
			|| !FPaths::IsProjectFilePathSet() || !FPaths::FileExists(Watchdog))
		{
			return;
		}
		// An editor killed with its process tree (taskkill /T, as the benchmark stops one) takes its watchdog along and
		// leaves its plan behind.
		const FString Folder = StudioFolder(TEXT("Recovery"));
		TArray<FString> Plans;
		IFileManager::Get().FindFiles(Plans, *(Folder / TEXT("session-*.json")), /*Files=*/true, /*Directories=*/false);
		for (const FString& Name : Plans)
		{
			if (!FPlatformProcess::IsApplicationRunning(static_cast<uint32>(FCString::Atoi(*FPaths::GetBaseFilename(Name).Mid(8)))))
			{
				IFileManager::Get().Delete(*(Folder / Name), false, true, true);
			}
		}
		const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		// UBT decides what the crash left stale: C++ changed since the last build, or Live Coding patches, get built in.
		const TSharedRef<FJsonObject> Plan = MakePlan(Token, GetReopen(Token), TEXT("Recovery"), FPaths::DirectoryExists(FPaths::GameSourceDir()), {});
		Plan->SetStringField(TEXT("crashes"), Native(FPaths::ProjectSavedDir() / TEXT("Crashes")));
		Plan->SetStringField(TEXT("restartFolder"), Native(FHyperAIStudioEditorRestart::GetFolder()));
		Current.WatchPlan = Plan;
		Current.WatchPath = Folder / FString::Printf(TEXT("session-%u.json"), FPlatformProcess::GetCurrentProcessId());
		RefreshWatch(0.f);
		const FString Params = FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -Session \"%s\""), *Native(Watchdog), *Native(Current.WatchPath));
		FProcHandle Process = FPlatformProcess::CreateProc(TEXT("powershell.exe"), *Params, /*bLaunchDetached=*/true, /*bLaunchHidden=*/true,
			/*bLaunchReallyHidden=*/true, nullptr, 0, *FPaths::ProjectDir(), nullptr);
		if (!Process.IsValid())
		{
			EndCrashWatch();
			UE_LOG(LogHyperAIStudioRestart, Warning, TEXT("The crash watchdog did not start, so a crash will not reopen the editor."));
			return;
		}
		FPlatformProcess::CloseProc(Process);
		Current.WatchTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&RefreshWatch), WatchSeconds);
		UE_LOG(LogHyperAIStudioRestart, Display, TEXT("Watching for crashes: if this editor crashes, it opens again with its chats."));
	}

	void RegisterToolset()
	{
		FState& Current = State();
		if (Current.bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
		{
			return;
		}
		if (!FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, CohortId, {ToolName},
			FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled()))
		{
			UE_LOG(LogHyperAIStudioRestart, Verbose, TEXT("The editor restart tool is not admitted here (Extended Hyper Tools are off)."));
			return;
		}
		FString Error;
		Current.bOwnsToolset = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
			UHyperAIStudioEditorRestartToolset::StaticClass(), QualifiedToolset, Error);
		if (!Current.bOwnsToolset)
		{
			UE_LOG(LogHyperAIStudioRestart, Error, TEXT("The editor restart tool was not registered: %s"), *Error);
		}
	}

	/** HyperAIStudio.Restart [NoBuild] [NoSave] [DryRun] [Tab=<n>] [note...] */
	void RunCommand(const TArray<FString>& Args)
	{
		FHyperAIStudioEditorRestart::FOptions Options;
		TArray<FString> Note;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("NoBuild"), ESearchCase::IgnoreCase))
			{
				Options.bBuild = false;
			}
			else if (Arg.Equals(TEXT("NoSave"), ESearchCase::IgnoreCase))
			{
				Options.bSaveDirty = false;
			}
			else if (Arg.Equals(TEXT("DryRun"), ESearchCase::IgnoreCase))
			{
				Options.bDryRun = true;
			}
			else if (Arg.StartsWith(TEXT("Tab="), ESearchCase::IgnoreCase))
			{
				Options.NoteTab = FCString::Atoi(*Arg.Mid(4));
			}
			else
			{
				Note.Add(Arg);
			}
		}
		Options.Note = FString::Join(Note, TEXT(" "));
		const FHyperAIEditorRestartReport Report = FHyperAIStudioEditorRestart::Restart(Options);
		UE_LOG(LogHyperAIStudioRestart, Display, TEXT("Restart %s: %s"), *Report.Status, *Report.Message);
		for (const FString& Chat : Report.Chats)
		{
			UE_LOG(LogHyperAIStudioRestart, Display, TEXT("  %s"), *Chat);
		}
		UE_LOG(LogHyperAIStudioRestart, Display, TEXT("  Build: %s"), Report.BuildCommand.IsEmpty() ? TEXT("none") : *Report.BuildCommand);
		UE_LOG(LogHyperAIStudioRestart, Display, TEXT("  Reopens with: %s"), *Report.RelaunchCommand);
	}
}

FHyperAIEditorRestartReport UHyperAIStudioEditorRestartToolset::hyper_editor_restart(const FHyperAIEditorRestartRequest& Request)
{
	FHyperAIStudioEditorRestart::FOptions Options;
	Options.bBuild = Request.bBuild;
	Options.bSaveDirty = Request.bSaveDirty;
	Options.bDryRun = Request.bDryRun;
	Options.Note = Request.Note.Left(HyperAIStudio::EditorRestart::Private::MaxNoteCharacters);
	return FHyperAIStudioEditorRestart::Restart(Options);
}

FString FHyperAIStudioEditorRestart::GetFolder()
{
	return HyperAIStudio::EditorRestart::Private::StudioFolder(TEXT("Restart"));
}

bool FHyperAIStudioEditorRestart::IsRestarting()
{
	return HyperAIStudio::EditorRestart::Private::State().bRestarting;
}

FHyperAIEditorRestartReport FHyperAIStudioEditorRestart::Restart(const FOptions& Options)
{
	using namespace HyperAIStudio::EditorRestart::Private;
	FHyperAIEditorRestartReport Report;
	Report.Status = TEXT("refused");
	const FString Script = HelperScript();
	if (State().bRestarting)
	{
		Report.Message = TEXT("A restart is already underway.");
		return Report;
	}
	if (GIsAutomationTesting && !Options.bDryRun)
	{
		Report.Message = TEXT("Automation tests are running in this editor, so it does not restart under them.");
		return Report;
	}
	if (!GEditor || !FPaths::IsProjectFilePathSet())
	{
		Report.Message = TEXT("This editor has no project file to open again.");
		return Report;
	}
	if (Script.IsEmpty() || !FPaths::FileExists(Script))
	{
		Report.Message = TEXT("The restart helper is missing from the plugin: Resources/Scripts/HyperAIStudioRestart.ps1.");
		return Report;
	}
	if (GEditor->IsLightingBuildCurrentlyRunning())
	{
		Report.Message = TEXT("A lighting build is running; restart once it finishes.");
		return Report;
	}

	TArray<UPackage*> Dirty;
	TArray<UPackage*> DirtyContent;
	UEditorLoadingAndSavingUtils::GetDirtyMapPackages(Dirty);
	UEditorLoadingAndSavingUtils::GetDirtyContentPackages(DirtyContent);
	Dirty.Append(DirtyContent);
	for (const UPackage* Package : Dirty)
	{
		Report.DirtyPackages.Add(Package->GetName());
		if (Options.bSaveDirty && FPackageName::IsTempPackage(Package->GetName()))
		{
			Report.Message = FString::Printf(TEXT("%s is a new level that was never saved, so it cannot be saved without a name. Save it first, or restart with bSaveDirty off to be asked."),
				*Package->GetName());
			return Report;
		}
	}

	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TArray<FHyperAIStudioRestartChat> Chats = SHyperAIStudioQuickActionWindow::GetRestartChats();
	bool bAnyWorking = false;
	for (FHyperAIStudioRestartChat& Chat : Chats)
	{
		Chat.bWorking = Chat.bWorking || Chat.TabIndex == Options.NoteTab;
		bAnyWorking = bAnyWorking || Chat.bWorking;
		Report.Chats.Add(DescribeChat(Chat));
	}
	const FString Folder = GetFolder();
	const FReopen Reopen = GetReopen(Token);
	Report.BuildCommand = Options.bBuild ? Reopen.BuildLine : FString();
	Report.BuildLog = TEXT("Saved/HyperAIStudio/Restart/build.log");
	Report.RelaunchCommand = FString::Printf(TEXT("\"%s\" %s"), *Reopen.Executable, *Reopen.Arguments);
	const FString Outcome = FString::Printf(TEXT("%d chat tab(s) reopen%s."), Chats.Num(), bAnyWorking
		? TEXT("; the working one is told to continue")
		: TEXT(". None was working, so none is told to continue. From outside the editor, wait with .hyperai/scripts/Wait-HyperAIStudioMCP.ps1 -TimeoutSeconds 1800, then continue"));
	if (Options.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Message = FString::Printf(TEXT("Nothing happened; this is what a restart would do. %s"), *Outcome);
		return Report;
	}

	// Nothing is lost: play stops, and changed work is saved before anything else happens.
	if (GEditor->PlayWorld)
	{
		GEditor->EndPlayMap();
	}
	if (Options.bSaveDirty && !Dirty.IsEmpty() && !UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages=*/true, /*bSaveContentPackages=*/true))
	{
		Report.Message = TEXT("Saving the changed levels and assets failed, so the editor did not restart. Save them yourself, or restart with bSaveDirty off to be asked.");
		return Report;
	}

	IFileManager::Get().Delete(*(Folder / TEXT("result.json")), false, true, true);
	TArray<TSharedPtr<FJsonValue>> Saved;
	for (const FString& Name : Options.bSaveDirty ? Report.DirtyPackages : TArray<FString>())
	{
		Saved.Add(MakeShared<FJsonValueString>(Name));
	}
	const TSharedRef<FJsonObject> Plan = MakePlan(Token, Reopen, TEXT("Restart"), Options.bBuild, Chats);
	Plan->SetStringField(TEXT("note"), Options.Note);
	Plan->SetArrayField(TEXT("savedPackages"), Saved);
	const FString PlanPath = Folder / TEXT("request.json");
	if (!WriteJson(PlanPath, Plan))
	{
		Report.Message = TEXT("The restart plan could not be written under Saved/HyperAIStudio/Restart, so the editor did not restart.");
		return Report;
	}

	// Its own console window, so the build shows while the editor is closed; nothing ties it to the editor.
	const FString Params = FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -Request \"%s\""), *Native(Script), *Native(PlanPath));
	FProcHandle Helper = FPlatformProcess::CreateProc(TEXT("powershell.exe"), *Params, /*bLaunchDetached=*/false, /*bLaunchHidden=*/false,
		/*bLaunchReallyHidden=*/false, nullptr, 0, *FPaths::ProjectDir(), nullptr);
	if (!Helper.IsValid())
	{
		Report.Message = TEXT("The restart helper did not start, so the editor did not restart.");
		return Report;
	}
	State().bRestarting = true;
	State().Helper = Helper;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		CloseEditor();
		return false;
	}), CloseDelaySeconds);

	Report.bOk = true;
	Report.Status = TEXT("restarting");
	Report.Message = FString::Printf(TEXT("The editor closes in about %d s%s, then opens %s again. %s Reply in a line that the restart is underway; your turn ends when the editor closes."),
		FMath::RoundToInt(CloseDelaySeconds), Options.bBuild ? *FString::Printf(TEXT(", builds %s"), *Reopen.Target) : TEXT(""), FApp::GetProjectName(), *Outcome);
	UE_LOG(LogHyperAIStudioRestart, Display, TEXT("Restarting the editor: %s"), *Report.Message);
	return Report;
}

TOptional<FHyperAIStudioRestartChat> FHyperAIStudioEditorRestart::TakeChat(const int32 TabIndex)
{
	TArray<FHyperAIStudioRestartChat>& Chats = HyperAIStudio::EditorRestart::Private::State().Chats;
	const int32 Index = Chats.IndexOfByPredicate([TabIndex](const FHyperAIStudioRestartChat& Chat) { return Chat.TabIndex == TabIndex; });
	if (Index == INDEX_NONE)
	{
		return {};
	}
	FHyperAIStudioRestartChat Chat = Chats[Index];
	Chats.RemoveAt(Index);
	UE_LOG(LogHyperAIStudioRestart, Display, TEXT("Chat tab %d picks up %s%s%s."), TabIndex, *Chat.Agent,
		Chat.SessionId.IsEmpty() ? TEXT("") : *(TEXT(" ") + Chat.SessionId), Chat.Message.IsEmpty() ? TEXT("") : TEXT(", to be told it restarted"));
	return Chat;
}

FString FHyperAIStudioEditorRestart::DescribeRestart(const TSharedPtr<FJsonObject>& Plan, const TSharedPtr<FJsonObject>& Result)
{
	using namespace HyperAIStudio::EditorRestart::Private;
	// A crash's plan comes from the watchdog, with the crash report's error and where it happened.
	const TSharedPtr<FJsonObject> Crash = StringField(Plan, TEXT("reason")) == TEXT("crash") ? ObjectField(Plan, TEXT("crash")) : nullptr;
	TArray<FString> Lines = {Crash.IsValid()
		? FString(TEXT("The Unreal Editor crashed and was opened again; this chat resumed."))
		: FString(TEXT("The Unreal Editor restarted, as you asked with hyper_editor_restart, and this chat resumed.")), FString()};
	if (Crash.IsValid())
	{
		Lines.Add(TEXT("- Crash: ") + StringField(Crash, TEXT("error")));
		const TArray<FString> Frames = StringArray(Crash, TEXT("callstack"));
		if (!Frames.IsEmpty())
		{
			Lines.Add(TEXT("  Where:"));
			for (const FString& Frame : Frames)
			{
				Lines.Add(TEXT("      ") + Frame);
			}
		}
		Lines.Add(TEXT("  The report: ") + StringField(Crash, TEXT("report")));
	}
	const TSharedPtr<FJsonObject> PlanBuild = ObjectField(Plan, TEXT("build"));
	const TSharedPtr<FJsonObject> Build = ObjectField(Result, TEXT("build"));
	const FString Target = StringField(PlanBuild, TEXT("target"));
	bool bBuilt = false;
	double ExitCode = -1.0;
	double Seconds = 0.0;
	if (Build.IsValid())
	{
		Build->TryGetBoolField(TEXT("enabled"), bBuilt);
		Build->TryGetNumberField(TEXT("exitCode"), ExitCode);
		Build->TryGetNumberField(TEXT("seconds"), Seconds);
	}
	if (!bBuilt)
	{
		if (!Crash.IsValid())
		{
			Lines.Add(TEXT("- Build: none, as you asked."));
		}
	}
	else if (ExitCode == 0.0)
	{
		Lines.Add(FString::Printf(TEXT("- Build: %s built in %d s."), *Target, FMath::RoundToInt(Seconds)));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("- Build: %s failed (exit code %d) after %d s, so the editor opened on the binaries from before the build."),
			*Target, FMath::RoundToInt(ExitCode), FMath::RoundToInt(Seconds)));
		const TArray<FString> Errors = StringArray(Build, TEXT("errors"));
		if (!Errors.IsEmpty())
		{
			// Indented rather than fenced: the chat shows what the user sends as plain text.
			Lines.Add(TEXT("  The first errors:"));
			for (const FString& Error : Errors)
			{
				Lines.Add(TEXT("      ") + Error);
			}
		}
		Lines.Add(FString::Printf(TEXT("  The whole log: %s"), *StringField(PlanBuild, TEXT("logRelative"))));
	}
	const int32 Saved = StringArray(Plan, TEXT("savedPackages")).Num();
	if (Saved > 0)
	{
		Lines.Add(FString::Printf(TEXT("- Saved before it closed: %d %s."), Saved, Saved == 1 ? TEXT("level or asset") : TEXT("levels and assets")));
	}
	const FString Note = StringField(Plan, TEXT("note"));
	if (!Note.IsEmpty())
	{
		Lines.Add(TEXT("- Your note: ") + Note);
	}
	Lines.Add(FString());
	if (Crash.IsValid())
	{
		Lines.Add(TEXT("Work not saved before the crash is lost, unless the editor offered to restore it from an autosave. If an Unreal tool call of yours never got its answer, it may be what crashed the editor: check that call before you make it again."));
	}
	Lines.Add(TEXT("Unreal MCP is up again. Continue where you left off."));
	return FString::Join(Lines, TEXT("\n"));
}

FString FHyperAIStudioEditorRestart::ContinuePrompt()
{
	return TEXT("The Unreal Editor restarted. Read Saved/HyperAIStudio/Restart/continue.md for why, the build result and any note, then continue where you left off.");
}

bool FHyperAIStudioEditorRestart::LoadResume(const FString& Folder, const FString& Token, const double MaxAgeSeconds)
{
	using namespace HyperAIStudio::EditorRestart::Private;
	const FString PlanPath = Folder / TEXT("request.json");
	const FString ResultPath = Folder / TEXT("result.json");
	const TSharedPtr<FJsonObject> Plan = ReadJson(PlanPath);
	const TSharedPtr<FJsonObject> Result = ReadJson(ResultPath);
	FDateTime Requested;
	const double Age = FDateTime::ParseIso8601(*StringField(Plan, TEXT("requestedAt")), Requested) ? (FDateTime::UtcNow() - Requested).GetTotalSeconds() : -1.0;
	// Opened without the token (the crash reporter's Send and Restart, or by hand): only a fresh crash plan for this very
	// project resumes here. Anything else stays for the editor its helper opens.
	const bool bNoToken = Token.IsEmpty();
	if (bNoToken && !(StringField(Plan, TEXT("reason")) == TEXT("crash") && Age >= 0.0 && Age <= MaxCrashAgeWithoutTokenSeconds
		&& FPaths::IsSamePath(StringField(Plan, TEXT("projectFile")), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()))))
	{
		return false;
	}
	const FString Expected = bNoToken ? StringField(Plan, TEXT("token")) : Token;
	// A restart resumes once: whatever the files hold, they are used up now.
	IFileManager::Get().Delete(*PlanPath, false, true, true);
	IFileManager::Get().Delete(*ResultPath, false, true, true);
	// Without the token there may be no result yet: the helper writes one only for an editor it opened itself.
	if (Expected.IsEmpty() || StringField(Plan, TEXT("token")) != Expected || (!bNoToken && StringField(Result, TEXT("token")) != Expected)
		|| Age < 0.0 || Age > MaxAgeSeconds)
	{
		UE_LOG(LogHyperAIStudioRestart, Display, TEXT("This editor was opened by a restart whose plan is missing, stale or another restart's; chats start fresh."));
		return false;
	}
	const FString Message = DescribeRestart(Plan, Result);
	TArray<FHyperAIStudioRestartChat>& Chats = State().Chats;
	Chats.Reset();
	State().Pages.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Plan->TryGetArrayField(TEXT("chats"), Values) && Values)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FHyperAIStudioRestartChat Chat = ChatFromJson(Value.IsValid() ? Value->AsObject() : nullptr);
			Chat.Message = Chat.bWorking ? Message : FString();
			if (!Chat.Agent.IsEmpty())
			{
				Chats.Add(MoveTemp(Chat));
			}
		}
	}
	Chats.StableSort([](const FHyperAIStudioRestartChat& A, const FHyperAIStudioRestartChat& B) { return !A.bWorking && B.bWorking; });
	for (const FHyperAIStudioRestartChat& Chat : Chats)
	{
		State().Pages.AddUnique(Chat.TabIndex);
	}
	// A terminal chat gets one line that points here.
	if (Chats.ContainsByPredicate([](const FHyperAIStudioRestartChat& Chat) { return Chat.bWorking; }))
	{
		FFileHelper::SaveStringToFile(Message, *(Folder / TEXT("continue.md")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	UE_LOG(LogHyperAIStudioRestart, Display, TEXT("Opened by a restart: %d chat(s) resume."), Chats.Num());
	return !Chats.IsEmpty();
}

void FHyperAIStudioEditorRestart::Startup()
{
	using namespace HyperAIStudio::EditorRestart::Private;
	FState& Current = State();
	Current.PostEngineInit = FCoreDelegates::GetOnPostEngineInit().AddStatic(&RegisterToolset);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterToolset();
	}
	Current.Command = MakeUnique<FAutoConsoleCommand>(TEXT("HyperAIStudio.Restart"),
		TEXT("Restart the editor and resume its chats: HyperAIStudio.Restart [NoBuild] [NoSave] [DryRun] [Tab=<n>] [note]. Tab tells that chat to continue."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunCommand));

	Current.PreExit = FCoreDelegates::OnEnginePreExit.AddStatic(&EndCrashWatch);

	FString Token;
	FParse::Value(FCommandLine::Get(), TEXT("HyperAIResume="), Token);
	const bool bResume = IsInteractiveEditor() && LoadResume(GetFolder(), Token, MaxResumeAgeSeconds);
	// Once the main frame is up: the crash watch, and after a restart, every restored chat tab a moment later. Tabs the
	// Studio layout brings back take their chats as they open.
	auto WhenUp = [bResume]()
	{
		StartCrashWatch();
		if (bResume)
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				ShowChats();
				return false;
			}), 1.5f);
		}
	};
	IMainFrameModule* MainFrame = FModuleManager::LoadModulePtr<IMainFrameModule>(TEXT("MainFrame"));
	if (MainFrame && MainFrame->IsWindowInitialized())
	{
		WhenUp();
	}
	else if (MainFrame)
	{
		Current.MainFrameReady = MainFrame->OnMainFrameCreationFinished().AddLambda([WhenUp](TSharedPtr<SWindow>, bool) { WhenUp(); });
	}
}

void FHyperAIStudioEditorRestart::Shutdown()
{
	using namespace HyperAIStudio::EditorRestart::Private;
	FState& Current = State();
	EndCrashWatch();
	if (Current.PreExit.IsValid())
	{
		FCoreDelegates::OnEnginePreExit.Remove(Current.PreExit);
		Current.PreExit.Reset();
	}
	if (Current.PostEngineInit.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(Current.PostEngineInit);
		Current.PostEngineInit.Reset();
	}
	if (Current.MainFrameReady.IsValid())
	{
		if (IMainFrameModule* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>(TEXT("MainFrame")))
		{
			MainFrame->OnMainFrameCreationFinished().Remove(Current.MainFrameReady);
		}
		Current.MainFrameReady.Reset();
	}
	if (Current.bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioEditorRestartToolset::StaticClass(), QualifiedToolset, Error))
		{
			UE_LOG(LogHyperAIStudioRestart, Warning, TEXT("Could not unregister the editor restart tool: %s"), *Error);
		}
	}
	Current.bOwnsToolset = false;
	Current.Command.Reset();
	// The helper runs on by design: it is what reopens the editor.
	if (Current.Helper.IsValid())
	{
		FPlatformProcess::CloseProc(Current.Helper);
		Current.Helper = FProcHandle();
	}
}
