// Games by Hyper 2026.

#include "HyperAIStudioEditorRestart.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioShell.h"
#include "InterchangeManager.h"
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
#include "TerminalSettings.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioEditorRestart"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioRestart, Log, All);

namespace HyperAIStudio::EditorRestart::Private
{
	constexpr const TCHAR* PackId = TEXT("editor_lifecycle");
	constexpr const TCHAR* CohortId = TEXT("cohort.source.hyperaistudioeditorrestarttoolset.v1");
	constexpr const TCHAR* ToolName = TEXT("hyper_editor_restart");
	constexpr const TCHAR* QualifiedToolset = TEXT("HyperAIStudio.HyperAIStudioEditorRestartToolset");
	/**
	 * How long the helper waits for the editor to close; 0 is for as long as the editor runs. The editor's questions
	 * before closing wait on a person, and a close the person turns down withdraws the plan and stops the helper.
	 */
	constexpr int32 CloseTimeoutSeconds = 0;
	/** A restart older than this is not resumed: a full rebuild can take hours, a forgotten plan should not come back. */
	constexpr double MaxResumeAgeSeconds = 6.0 * 60.0 * 60.0;
	/** An editor opened without the token takes a crash's chats only this soon after it: Send and Restart, or a quick reopen by hand. */
	constexpr double MaxCrashAgeWithoutTokenSeconds = 10.0 * 60.0;
	/**
	 * An editor opened by hand takes the chats of a restart that closed the editor and never opened it again by itself
	 * this soon after the close; later, up to MaxResumeAgeSeconds, it offers them.
	 */
	constexpr double MaxRestartAgeWithoutTokenSeconds = 30.0 * 60.0;
	/** The editor waits this long before it closes, so the tool's answer reaches the agent first. */
	constexpr float CloseDelaySeconds = 2.0f;
	/**
	 * Times the close is asked for in all when the editor stays open without asking anything (an import running, say),
	 * each again at least FHyperAIStudioEditorRestart::CloseRetrySeconds later and once nothing keeps it busy.
	 */
	constexpr int32 MaxCloseTries = 3;
	/** A test's answer to the close's question: a switch on the command line, never passed on to an editor opened again. */
	constexpr const TCHAR* TestAnswerSwitch = TEXT("-HyperAITestRestartAnswer=");
	constexpr int32 MaxNoteCharacters = 4000;
	constexpr const TCHAR* CloseCommand = TEXT("CLOSE_SLATE_MAINFRAME");

	/** How often the crash watch looks for changed chats. */
	constexpr float WatchSeconds = 5.0f;

	struct FState
	{
		bool bRestarting = false;
		FProcHandle Helper;
		/** The restart's plan, which an editor opened by hand reads too once the close went ahead. */
		FString PlanPath;
		/** The close: asked for (Tries), with a question on screen while it ran (bCloseAsked), gone ahead (bShutDown). */
		int32 CloseTries = 0;
		bool bCloseAsked = false;
		bool bShutDown = false;
		/** When the editor was seen to stay open without asking anything; -1 while the close stands or runs. */
		double RefusedAt = -1.0;
		FTSTicker::FDelegateHandle CloseWatch;
		FDelegateHandle ModalTick;
		FDelegateHandle ShutdownSaved;
		/** The Terminal plugin's close question, held off while a restart closes the editor; its value to put back. */
		TOptional<bool> TerminalQuestion;
		/** A restart an editor opened by hand offers to resume: its plan and result, and the notification asking. */
		TSharedPtr<FJsonObject> OfferPlan;
		TSharedPtr<FJsonObject> OfferResult;
		FString OfferFolder;
		TWeakPtr<SNotificationItem> OfferNotice;
		/** After a restart: the chats still waiting for their tab. */
		TArray<FHyperAIStudioRestartChat> Chats;
		/** After a restart: every restored chat's tab, the one told to continue last. */
		TArray<int32> Pages;
		TUniquePtr<FAutoConsoleCommand> Command;
		TUniquePtr<FAutoConsoleCommand> ResumeCommand;
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

	/** Line without Switch (such as -HyperAIResume=) and its value, quoted or not, wherever it appears. */
	FString WithoutSwitch(FString Line, const TCHAR* Switch)
	{
		for (int32 At = Line.Find(Switch); At != INDEX_NONE; At = Line.Find(Switch))
		{
			// "-Switch=a b" is one argument too.
			At = At > 0 && Line[At - 1] == TEXT('"') ? At - 1 : At;
			int32 End = At;
			bool bQuoted = false;
			while (End < Line.Len() && (bQuoted || !FChar::IsWhitespace(Line[End])))
			{
				bQuoted = Line[End] == TEXT('"') ? !bQuoted : bQuoted;
				++End;
			}
			Line.RemoveAt(At, End - At);
		}
		return Line.TrimStartAndEnd();
	}

	/** The command line an editor opens again with: no earlier restart's token, so tokens never pile up, and no test's answer. */
	FString WithoutResumeSwitch(FString Line)
	{
		return WithoutSwitch(WithoutSwitch(MoveTemp(Line), TEXT("-HyperAIResume=")), TestAnswerSwitch);
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

	/** Seconds since an ISO 8601 time; -1 when there is none. */
	double SecondsSince(const FString& Iso8601)
	{
		FDateTime At;
		return !Iso8601.IsEmpty() && FDateTime::ParseIso8601(*Iso8601, At) ? (FDateTime::UtcNow() - At).GetTotalSeconds() : -1.0;
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

	/**
	 * The Terminal plugin asks before the editor closes while a terminal shows output, and the agent that asked for the
	 * restart is still writing its answer in its tab. HyperAI's chat terminals come back on their own conversations, so
	 * while every terminal open is one of them, the restart's close does not ask about them. A terminal of your own
	 * keeps the question. Only the setting in this editor's memory changes, and a called-off restart puts it back.
	 */
	void HoldTerminalQuestion()
	{
		UTerminalSettings* Settings = GetMutableDefault<UTerminalSettings>();
		if (State().TerminalQuestion.IsSet() || !Settings || !Settings->bPreventCloseDuringActivity || !SHyperAIStudioQuickActionWindow::OwnsEveryTerminal())
		{
			return;
		}
		State().TerminalQuestion = Settings->bPreventCloseDuringActivity;
		Settings->bPreventCloseDuringActivity = false;
		UE_LOG(LogHyperAIStudioRestart, Display, TEXT("Every terminal open is a HyperAI chat that reopens after the restart, so the editor does not ask about them as it closes."));
	}

	void RestoreTerminalQuestion()
	{
		if (State().TerminalQuestion.IsSet())
		{
			GetMutableDefault<UTerminalSettings>()->bPreventCloseDuringActivity = State().TerminalQuestion.GetValue();
		}
		State().TerminalQuestion.Reset();
	}

	void StopWatchingClose()
	{
		FState& Current = State();
		if (Current.CloseWatch.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Current.CloseWatch);
			Current.CloseWatch.Reset();
		}
		if (Current.ModalTick.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().GetOnModalLoopTickEvent().Remove(Current.ModalTick);
		}
		Current.ModalTick.Reset();
	}

	void EndRestart()
	{
		FState& Current = State();
		StopWatchingClose();
		if (Current.Helper.IsValid())
		{
			FPlatformProcess::CloseProc(Current.Helper);
		}
		Current.Helper = FProcHandle();
		Current.bRestarting = false;
		Current.CloseTries = 0;
		Current.bCloseAsked = false;
		Current.bShutDown = false;
		Current.RefusedAt = -1.0;
	}

	/**
	 * The editor stays open, so the restart is off: the helper stops and the plan goes, so neither a later close nor an
	 * editor opened later builds, reopens or resumes anything. Said in the log and in a notification.
	 */
	void CancelRestart(const FText& Why)
	{
		FState& Current = State();
		if (Current.Helper.IsValid() && FPlatformProcess::IsProcRunning(Current.Helper))
		{
			FPlatformProcess::TerminateProc(Current.Helper, /*KillTree=*/true);
		}
		// The helper also stops by itself once the plan is gone.
		if (!Current.PlanPath.IsEmpty())
		{
			IFileManager::Get().Delete(*Current.PlanPath, false, true, true);
		}
		RestoreTerminalQuestion();
		EndRestart();
		UE_LOG(LogHyperAIStudioRestart, Warning, TEXT("The restart was called off, and the editor stays open: %s"), *Why.ToString());
		if (FSlateApplication::IsInitialized())
		{
			FNotificationInfo Info(FText::Format(LOCTEXT("RestartCalledOff", "The editor restart was called off: {0}"), Why));
			Info.ExpireDuration = 10.0f;
			Info.bUseSuccessFailIcons = true;
			if (const TSharedPtr<SNotificationItem> Notice = FSlateNotificationManager::Get().AddNotification(Info))
			{
				Notice->SetCompletionState(SNotificationItem::CS_Fail);
			}
		}
	}

	/**
	 * The editor shuts down for the restart (FMainFrameHandler::ShutDownEditor, after every question was answered). The
	 * plan says so, for an editor opened by hand should the helper never open one.
	 */
	void OnShutDown()
	{
		FState& Current = State();
		if (!Current.bRestarting || Current.bShutDown)
		{
			return;
		}
		Current.bShutDown = true;
		if (const TSharedPtr<FJsonObject> Plan = ReadJson(Current.PlanPath))
		{
			Plan->SetStringField(TEXT("closedAt"), FDateTime::UtcNow().ToIso8601());
			WriteJson(Current.PlanPath, Plan.ToSharedRef());
		}
		// The helper opens it again now; a crash on the way out must not have the watchdog open a second one.
		EndCrashWatch();
		UE_LOG(LogHyperAIStudioRestart, Display, TEXT("The editor closes for the restart; the helper builds and opens it again once it is gone."));
	}

	void OnPreExit()
	{
		// Closed some other way than the main frame's close (QUIT_EDITOR, say): the restart still goes ahead.
		OnShutDown();
		EndCrashWatch();
	}

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
	/**
	 * For live tests of a restart, which span a real close and relaunch: an editor started with
	 * -HyperAITestRestartAnswer="<button>@<seconds>" presses the button labelled <button> on a question the restart's
	 * close puts on screen, once it has been up that long, as a person would. Only that switch arms it: nothing in the
	 * environment does, and the editor the helper opens again is started without it.
	 */
	struct FTestAnswer
	{
		FString Label;
		double Seconds = 0.0;
		double Since = -1.0;
	};

	FTestAnswer& TestAnswer()
	{
		static FTestAnswer Answer;
		return Answer;
	}

	bool ShowsText(const TSharedRef<SWidget>& Widget, const FString& Label)
	{
		if (Widget->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString() == Label)
		{
			return true;
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			if (ShowsText(Children->GetChildAt(Index), Label))
			{
				return true;
			}
		}
		return false;
	}

	TSharedPtr<SButton> FindButton(const TSharedRef<SWidget>& Widget, const FString& Label)
	{
		const FName Type = Widget->GetType();
		if ((Type == FName(TEXT("SButton")) || Type == FName(TEXT("SPrimaryButton"))) && ShowsText(Widget, Label))
		{
			return StaticCastSharedRef<SButton>(Widget);
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			if (TSharedPtr<SButton> Found = FindButton(Children->GetChildAt(Index), Label))
			{
				return Found;
			}
		}
		return nullptr;
	}

	void ReadTestAnswer()
	{
		FString Value;
		FString Label;
		FString Seconds;
		TestAnswer() = FTestAnswer();
		// FParse::Value takes the name without its leading dash.
		if (FParse::Value(FCommandLine::Get(), TestAnswerSwitch + 1, Value, /*bShouldStopOnSeparator=*/false)
			&& Value.Split(TEXT("@"), &Label, &Seconds, ESearchCase::IgnoreCase, ESearchDir::FromEnd) && !Label.IsEmpty())
		{
			TestAnswer().Label = Label;
			TestAnswer().Seconds = FCString::Atod(*Seconds);
		}
	}

	void AnswerForTests()
	{
		FTestAnswer& Answer = TestAnswer();
		const TSharedPtr<SWindow> Modal = Answer.Label.IsEmpty() ? nullptr : FSlateApplication::Get().GetActiveModalWindow();
		if (!Modal.IsValid())
		{
			return;
		}
		const double Now = FPlatformTime::Seconds();
		if (Answer.Since < 0.0)
		{
			Answer.Since = Now;
			UE_LOG(LogHyperAIStudioRestart, Display, TEXT("Test: '%s' asks before closing; it is answered %s in %.0f s."), *Modal->GetTitle().ToString(), *Answer.Label, Answer.Seconds);
		}
		if (Now - Answer.Since < Answer.Seconds)
		{
			return;
		}
		if (const TSharedPtr<SButton> Button = FindButton(Modal.ToSharedRef(), Answer.Label))
		{
			UE_LOG(LogHyperAIStudioRestart, Display, TEXT("Test: answered '%s' with %s after %.0f s."), *Modal->GetTitle().ToString(), *Answer.Label, Now - Answer.Since);
			Answer.Since = -1.0;
			Button->SimulateClick();
		}
	}
#endif

	void AskToClose();

	/**
	 * What keeps the editor from closing without a question: an import (Interchange says so in a toast and stays open),
	 * a lighting build, or saving, loading and slow tasks, which put the close back for later.
	 */
	bool IsEditorBusy()
	{
		return GIsSlowTask || IsLoading() || UE::IsSavingPackage() || IsGarbageCollecting()
			|| UInterchangeManager::GetInterchangeManager().IsInterchangeActive() || (GEditor && GEditor->IsLightingBuildCurrentlyRunning());
	}

	/** Each frame once the close was asked for, until the editor shuts down or the restart is off. */
	bool WatchClose(float)
	{
		using ECloseState = FHyperAIStudioEditorRestart::ECloseState;
		FState& Current = State();
		if (!Current.bRestarting)
		{
			Current.CloseWatch.Reset();
			return false;
		}
		const bool bShutDown = Current.bShutDown || IsEngineExitRequested();
		const bool bQueued = GEngine && GEngine->DeferredCommands.Contains(CloseCommand);
		const double Now = FPlatformTime::Seconds();
		if (!bShutDown && !bQueued && !Current.bCloseAsked && Current.RefusedAt < 0.0)
		{
			// The close ran and the editor stayed open without asking anything: it is asked again later, not next frame.
			Current.RefusedAt = Now;
			if (Current.CloseTries < MaxCloseTries)
			{
				UE_LOG(LogHyperAIStudioRestart, Display, TEXT("The editor stayed open without asking anything; the restart asks it to close again in %.0f s, once nothing keeps it busy%s."),
					FHyperAIStudioEditorRestart::CloseRetrySeconds, UInterchangeManager::GetInterchangeManager().IsInterchangeActive() ? TEXT(" (an import is running)") : TEXT(""));
			}
		}
		const double SinceRefused = Current.RefusedAt < 0.0 ? 0.0 : Now - Current.RefusedAt;
		// Only asked about once the editor refused: a closing editor's import manager may be gone.
		const bool bBusy = Current.RefusedAt >= 0.0 && !bShutDown && IsEditorBusy();
		switch (FHyperAIStudioEditorRestart::JudgeClose(bShutDown, bQueued, Current.bCloseAsked, Current.CloseTries, SinceRefused, bBusy))
		{
		case ECloseState::Waiting:
			return true;
		case ECloseState::Closing:
			OnShutDown();
			Current.CloseWatch.Reset();
			StopWatchingClose();
			return false;
		case ECloseState::TryAgain:
			UE_LOG(LogHyperAIStudioRestart, Display, TEXT("The restart asks the editor to close again (request %d of %d)."), Current.CloseTries + 1, MaxCloseTries);
			AskToClose();
			return true;
		case ECloseState::CalledOff:
		default:
			Current.CloseWatch.Reset();
			CancelRestart(Current.bCloseAsked
				? LOCTEXT("RestartKeptOpen", "the editor asked before closing and the answer kept it open, so nothing was built or opened again.")
				: FText::Format(LOCTEXT("RestartNeverClosed", "the editor stayed open after {0} requests to close, without asking anything. Close it yourself, or restart again."),
					FText::AsNumber(MaxCloseTries)));
			return false;
		}
	}

	/**
	 * The way File > Restart closes the editor, so it asks its own questions (save your work, close anyway), which wait
	 * for an answer however long it takes. What comes of it is judged by WatchClose from what happened, never by time.
	 */
	void AskToClose()
	{
		FState& Current = State();
		if (!Current.bRestarting || !GEngine)
		{
			return;
		}
		++Current.CloseTries;
		Current.bCloseAsked = false;
		Current.RefusedAt = -1.0;
		HoldTerminalQuestion();
		if (!Current.ModalTick.IsValid() && FSlateApplication::IsInitialized())
		{
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
			ReadTestAnswer();
#endif
			// A modal loop runs only while a question is on screen: the save prompt, close anyway, are you sure.
			Current.ModalTick = FSlateApplication::Get().GetOnModalLoopTickEvent().AddLambda([](float)
			{
				State().bCloseAsked = true;
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
				AnswerForTests();
#endif
			});
		}
		GEngine->DeferredCommands.AddUnique(CloseCommand);
		if (!Current.CloseWatch.IsValid())
		{
			Current.CloseWatch = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&WatchClose), 0.0f);
		}
	}

	/** A restart's chats in State for their tabs, the working ones told what happened; continue.md for a terminal chat. */
	bool AdoptChats(const FString& Folder, const TSharedPtr<FJsonObject>& Plan, const TSharedPtr<FJsonObject>& Result)
	{
		const FString Message = FHyperAIStudioEditorRestart::DescribeRestart(Plan, Result);
		TArray<FHyperAIStudioRestartChat>& Chats = State().Chats;
		Chats.Reset();
		State().Pages.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Plan.IsValid() && Plan->TryGetArrayField(TEXT("chats"), Values) && Values)
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

	int32 CountChats(const TSharedPtr<FJsonObject>& Plan)
	{
		int32 Count = 0;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Plan.IsValid() && Plan->TryGetArrayField(TEXT("chats"), Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Count += ChatFromJson(Value.IsValid() ? Value->AsObject() : nullptr).Agent.IsEmpty() ? 0 : 1;
			}
		}
		return Count;
	}

	void EndOffer()
	{
		FState& Current = State();
		Current.OfferPlan.Reset();
		Current.OfferResult.Reset();
		Current.OfferFolder.Reset();
		if (const TSharedPtr<SNotificationItem> Notice = Current.OfferNotice.Pin())
		{
			// The buttons go at once: a notice stays up a while as it fades, and a second press must find nothing.
			Notice->SetCompletionState(SNotificationItem::CS_Success);
			Notice->ExpireAndFadeout();
		}
		Current.OfferNotice.Reset();
	}

	/** The offered restart's files, if they are still that restart's: another editor may have taken them, or a newer restart replaced them. */
	bool OfferStillThere()
	{
		const FState& Current = State();
		return Current.OfferPlan.IsValid()
			&& StringField(ReadJson(Current.OfferFolder / TEXT("request.json")), TEXT("token")) == StringField(Current.OfferPlan, TEXT("token"));
	}

	/** Once the main frame is up: asks whether to resume a restart that closed the editor long ago and never opened it again. */
	void OfferResume()
	{
		FState& Current = State();
		if (!Current.OfferPlan.IsValid() || !FSlateApplication::IsInitialized())
		{
			return;
		}
		const int32 Count = CountChats(Current.OfferPlan);
		FNotificationInfo Info(FText::Format(LOCTEXT("RestartResumeOffer",
			"The editor closed for a HyperAI restart and did not open again by itself. Resume the {0} {0}|plural(one=chat,other=chats) it was running, where {0}|plural(one=it,other=they) left off?"),
			Count));
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("ResumeRestartChats", "Resume Chats"),
			LOCTEXT("ResumeRestartChatsTooltip", "Reopen the restart's chats on their conversations, and tell the chat that asked for the restart to continue. An open tab whose chat is idle switches to its restart chat (the chat it had stays in Chats); a tab that is working, or whose chat view has messages, keeps its chat, and the restart's opens in a free tab."),
			FSimpleDelegate::CreateLambda([]() { FHyperAIStudioEditorRestart::AcceptResumeOffer(); }), SNotificationItem::CS_None));
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("DismissRestartChats", "Dismiss"),
			LOCTEXT("DismissRestartChatsTooltip", "Leave the chat tabs as they are. This restart is not offered again."),
			FSimpleDelegate::CreateLambda([]() { FHyperAIStudioEditorRestart::DeclineResumeOffer(); }), SNotificationItem::CS_None));
		Current.OfferNotice = FSlateNotificationManager::Get().AddNotification(Info);
		UE_LOG(LogHyperAIStudioRestart, Display, TEXT("Offering to resume %d chat(s) from a restart that closed the editor and never opened it again."), Count);
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
	State().PlanPath = PlanPath;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		AskToClose();
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
	// The editor closed for the restart, and no helper reported opening it again: someone opened it by hand.
	const bool bByHand = !Crash.IsValid() && !StringField(Plan, TEXT("closedAt")).IsEmpty() && StringField(Result, TEXT("state")) != TEXT("relaunched");
	TArray<FString> Lines = {Crash.IsValid()
		? FString(TEXT("The Unreal Editor crashed and was opened again; this chat resumed."))
		: bByHand
		? FString(TEXT("The Unreal Editor closed for the restart you asked for with hyper_editor_restart, but did not open again by itself. It was opened again by hand, and this chat resumed."))
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
	bool bBuildPlanned = false;
	if (PlanBuild.IsValid())
	{
		PlanBuild->TryGetBoolField(TEXT("enabled"), bBuildPlanned);
	}
	if (!Build.IsValid() && bBuildPlanned && !Crash.IsValid())
	{
		Lines.Add(FString::Printf(TEXT("- Build: no result. The restart's helper stopped before it reported a build of %s, so the editor may be running the binaries from before your changes; if they are missing, restart again with bBuild."),
			*Target));
	}
	else if (!bBuilt)
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
	TSharedPtr<FJsonObject> Result = ReadJson(ResultPath);
	double Age = SecondsSince(StringField(Plan, TEXT("requestedAt")));
	// Opened without the token (the crash reporter's Send and Restart, or by hand): only a plan for this very project, and
	// then a fresh crash's, or a restart's whose editor really closed for it and that nothing opened again. Anything else
	// stays for the editor its helper opens.
	const bool bNoToken = Token.IsEmpty();
	if (bNoToken)
	{
		const bool bThisProject = FPaths::IsSamePath(StringField(Plan, TEXT("projectFile")), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		const bool bCrash = StringField(Plan, TEXT("reason")) == TEXT("crash");
		// A restart counts from its close: the editor's questions before closing may have waited a long time.
		const double Closed = bCrash ? -1.0 : SecondsSince(StringField(Plan, TEXT("closedAt")));
		if (!bThisProject || (bCrash ? !(Age >= 0.0 && Age <= MaxCrashAgeWithoutTokenSeconds) : Closed < 0.0))
		{
			return false;
		}
		if (!bCrash)
		{
			// A restart with no chat running (one asked for from outside the editor, say) has nothing to give back or
			// offer: its files are used up below, as a recent one's are.
			const int32 Count = CountChats(Plan);
			if (Count > 0 && Closed > MaxRestartAgeWithoutTokenSeconds)
			{
				// Too long ago to tell an agent to continue unasked: offered instead, and the files stay until then.
				if (Closed <= MaxAgeSeconds)
				{
					State().OfferPlan = Plan;
					State().OfferResult = StringField(Result, TEXT("token")) == StringField(Plan, TEXT("token")) ? Result : nullptr;
					State().OfferFolder = Folder;
				}
				return false;
			}
			UE_LOG(LogHyperAIStudioRestart, Display, TEXT("The editor closed for a restart %d min ago and nothing opened it again%s"), FMath::RoundToInt(Closed / 60.0),
				Count > 0 ? TEXT(", so this editor takes its chats.") : TEXT("; it ran no chats, so there is nothing to resume."));
			Age = Count > 0 ? Closed : 0.0;
		}
		// A result is this restart's only when it carries the same token; the helper may never have written one.
		if (StringField(Result, TEXT("token")) != StringField(Plan, TEXT("token")))
		{
			Result.Reset();
		}
	}
	const FString Expected = bNoToken ? StringField(Plan, TEXT("token")) : Token;
	// A restart resumes once: whatever the files hold, they are used up now.
	IFileManager::Get().Delete(*PlanPath, false, true, true);
	IFileManager::Get().Delete(*ResultPath, false, true, true);
	if (Expected.IsEmpty() || StringField(Plan, TEXT("token")) != Expected || (!bNoToken && StringField(Result, TEXT("token")) != Expected)
		|| Age < 0.0 || Age > MaxAgeSeconds)
	{
		UE_LOG(LogHyperAIStudioRestart, Display, TEXT("This editor was opened by a restart whose plan is missing, stale or another restart's; chats start fresh."));
		return false;
	}
	return AdoptChats(Folder, Plan, Result);
}

FHyperAIStudioEditorRestart::ECloseState FHyperAIStudioEditorRestart::JudgeClose(const bool bShutDown, const bool bCloseQueued, const bool bAsked, const int32 Tries,
	const double SecondsSinceRefused, const bool bBusy)
{
	if (bShutDown)
	{
		return ECloseState::Closing;
	}
	if (bCloseQueued)
	{
		return ECloseState::Waiting;
	}
	if (bAsked || Tries >= HyperAIStudio::EditorRestart::Private::MaxCloseTries)
	{
		return ECloseState::CalledOff;
	}
	// Asked again only once what kept it open had time to pass: the next frame would meet the same refusal.
	return bBusy || SecondsSinceRefused < CloseRetrySeconds ? ECloseState::Waiting : ECloseState::TryAgain;
}

bool FHyperAIStudioEditorRestart::HasResumeOffer()
{
	return HyperAIStudio::EditorRestart::Private::State().OfferPlan.IsValid();
}

bool FHyperAIStudioEditorRestart::AcceptResumeOffer()
{
	using namespace HyperAIStudio::EditorRestart::Private;
	FState& Current = State();
	const TSharedPtr<FJsonObject> Plan = Current.OfferPlan;
	const TSharedPtr<FJsonObject> Result = Current.OfferResult;
	const FString Folder = Current.OfferFolder;
	const bool bThere = OfferStillThere();
	EndOffer();
	if (!Plan.IsValid() || !bThere)
	{
		UE_LOG(LogHyperAIStudioRestart, Display, TEXT("The restart offered for resuming is gone (another editor took it, or a newer restart replaced it)."));
		return false;
	}
	IFileManager::Get().Delete(*(Folder / TEXT("request.json")), false, true, true);
	IFileManager::Get().Delete(*(Folder / TEXT("result.json")), false, true, true);
	if (!AdoptChats(Folder, Plan, Result))
	{
		return false;
	}
	// The tabs are up and running chats of their own by now: an idle one picks its restart chat up at once, one in use
	// keeps its chat and the restart's goes to a free tab; then every restored chat's tab shows, the one told to
	// continue in front.
	SHyperAIStudioQuickActionWindow::TakeRestartChats();
	ShowChats();
	return true;
}

TArray<int32> FHyperAIStudioEditorRestart::PlaceChats(const TArray<int32>& ChatTabs, const TArray<FHyperAIStudioRestartTab>& Tabs)
{
	auto Find = [&Tabs](const int32 TabIndex) { return Tabs.FindByPredicate([TabIndex](const FHyperAIStudioRestartTab& Tab) { return Tab.TabIndex == TabIndex; }); };
	TArray<int32> Out;
	TSet<int32> Taken;
	// First every chat whose own tab is free (closed, or open with nothing in use), so a moved chat never takes it.
	for (const int32 Own : ChatTabs)
	{
		const FHyperAIStudioRestartTab* Tab = Find(Own);
		const bool bFree = !Taken.Contains(Own) && (!Tab || !Tab->bOpen || !Tab->bInUse);
		Out.Add(bFree ? Own : 0);
		if (bFree)
		{
			Taken.Add(Own);
		}
	}
	// Then a chat whose tab is in use: a closed tab first, which opens on it, else an open one with nothing in use.
	for (int32 Index = 0; Index < ChatTabs.Num(); ++Index)
	{
		for (int32 Pass = 0; Pass < 2 && Out[Index] == 0; ++Pass)
		{
			for (const FHyperAIStudioRestartTab& Tab : Tabs)
			{
				if (!Taken.Contains(Tab.TabIndex) && !ChatTabs.Contains(Tab.TabIndex) && (Pass == 0 ? !Tab.bOpen : !Tab.bInUse))
				{
					Out[Index] = Tab.TabIndex;
					Taken.Add(Tab.TabIndex);
					break;
				}
			}
		}
	}
	return Out;
}

TArray<int32> FHyperAIStudioEditorRestart::GetWaitingChatTabs()
{
	TArray<int32> Tabs;
	for (const FHyperAIStudioRestartChat& Chat : HyperAIStudio::EditorRestart::Private::State().Chats)
	{
		Tabs.Add(Chat.TabIndex);
	}
	return Tabs;
}

TArray<FHyperAIStudioRestartChat> FHyperAIStudioEditorRestart::PlaceWaitingChats(const TArray<int32>& Tabs)
{
	using namespace HyperAIStudio::EditorRestart::Private;
	TArray<FHyperAIStudioRestartChat>& Chats = State().Chats;
	TArray<FHyperAIStudioRestartChat> Dropped;
	for (int32 Index = Chats.Num() - 1; Index >= 0; --Index)
	{
		const int32 To = Tabs.IsValidIndex(Index) ? Tabs[Index] : Chats[Index].TabIndex;
		if (To <= 0)
		{
			Dropped.Insert(Chats[Index], 0);
			Chats.RemoveAt(Index);
		}
		else if (To != Chats[Index].TabIndex)
		{
			UE_LOG(LogHyperAIStudioRestart, Display, TEXT("Chat tab %d is in use, so its restart chat (%s) opens in chat tab %d."), Chats[Index].TabIndex, *Chats[Index].Agent, To);
			Chats[Index].TabIndex = To;
		}
	}
	// The tabs shown after a restart, in the chats' order: the one told to continue last, so it ends in front.
	State().Pages.Reset();
	for (const FHyperAIStudioRestartChat& Chat : Chats)
	{
		State().Pages.AddUnique(Chat.TabIndex);
	}
	return Dropped;
}

void FHyperAIStudioEditorRestart::DeclineResumeOffer()
{
	using namespace HyperAIStudio::EditorRestart::Private;
	const FString Folder = State().OfferFolder;
	if (OfferStillThere())
	{
		IFileManager::Get().Delete(*(Folder / TEXT("request.json")), false, true, true);
		IFileManager::Get().Delete(*(Folder / TEXT("result.json")), false, true, true);
		UE_LOG(LogHyperAIStudioRestart, Display, TEXT("The restart's chats were not resumed, as chosen; it is not offered again."));
	}
	EndOffer();
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
	Current.ResumeCommand = MakeUnique<FAutoConsoleCommand>(TEXT("HyperAIStudio.ResumeChats"),
		TEXT("Resume the chats of a restart that closed the editor and never opened it again, as its notification's Resume Chats does."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			UE_LOG(LogHyperAIStudioRestart, Display, TEXT("%s"), FHyperAIStudioEditorRestart::AcceptResumeOffer()
				? TEXT("The restart's chats resume.") : TEXT("No restart waits to be resumed."));
		}));

	Current.PreExit = FCoreDelegates::OnEnginePreExit.AddStatic(&OnPreExit);
	// The one place the main frame's close goes ahead, once every question before it was answered.
	Current.ShutdownSaved = FEditorDelegates::OnShutdownPostPackagesSaved.AddStatic(&OnShutDown);

	FString Token;
	FParse::Value(FCommandLine::Get(), TEXT("HyperAIResume="), Token);
	const bool bResume = IsInteractiveEditor() && LoadResume(GetFolder(), Token, MaxResumeAgeSeconds);
	// Once the main frame is up: the crash watch, and after a restart, every restored chat tab a moment later. Tabs the
	// Studio layout brings back take their chats as they open. A restart too old to resume by itself is offered.
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
		OfferResume();
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
	StopWatchingClose();
	if (Current.PreExit.IsValid())
	{
		FCoreDelegates::OnEnginePreExit.Remove(Current.PreExit);
		Current.PreExit.Reset();
	}
	if (Current.ShutdownSaved.IsValid())
	{
		FEditorDelegates::OnShutdownPostPackagesSaved.Remove(Current.ShutdownSaved);
		Current.ShutdownSaved.Reset();
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
	Current.ResumeCommand.Reset();
	// The helper runs on by design: it is what reopens the editor.
	if (Current.Helper.IsValid())
	{
		FPlatformProcess::CloseProc(Current.Helper);
		Current.Helper = FProcHandle();
	}
}

#undef LOCTEXT_NAMESPACE
