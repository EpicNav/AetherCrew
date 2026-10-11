// Games by Hyper 2026.

#include "HyperAIStudioMcpBypassMonitor.h"

#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IMainFrameModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SHyperAIStudioMcpBypassNote.h"
#include "Styling/AppStyle.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMcpBypass, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioMcpBypass"

namespace HyperAIStudio::McpBypass::Monitor
{
	/** The editor's per-project user settings, as Lore's command path and the Studio's rail are kept. */
	const TCHAR* const ConfigSection = TEXT("HyperAIStudio.McpBypass");
	const TCHAR* const SilencedKey = TEXT("SilencedWarnings");
	constexpr float FirstCheckSeconds = 5.0f;
	constexpr float CheckSeconds = 30.0f;
	/** One notification names at most this many "Don't warn about <name> again" buttons. */
	constexpr int32 MaxSilenceButtons = 3;

	struct FState
	{
		bool bScanned = false;
		bool bStarted = false;
		/** The first check also warns, once the editor's window is up. */
		bool bStartupWarned = false;
		TArray<FHyperAIStudioMcpFinding> Findings;
		TArray<FString> Problems;
		/** What the files, the Python setting and the ports looked like at the last read. */
		FString WatchSignature;
		/** Everything about the findings, to tell whether a read changed them. */
		FString FindingsSignature;
		/** The bypass names AGENTS.md was last given; unset until the first check gives it some (or none). */
		TOptional<FString> InstructionNames;
		TSet<FString> Silenced;
		bool bSilencedLoaded = false;
		/** Keys (BuildWarningKeys) warned about this session. */
		TSet<FString> NotifiedKeys;
		TWeakPtr<SNotificationItem> Notice;
		TWeakPtr<SWindow> DetailsWindow;
		FTSTicker::FDelegateHandle FirstTicker;
		FTSTicker::FDelegateHandle Ticker;
		FSimpleMulticastDelegate Changed;
		FString ConfigFileOverride;
		TOptional<TArray<FHyperAIStudioMcpFinding>> TestFindings;
	};

	FState& State()
	{
		static FState Instance;
		return Instance;
	}

	FString ConfigFile()
	{
		return State().ConfigFileOverride.IsEmpty() ? GEditorPerProjectIni : State().ConfigFileOverride;
	}

	const TSet<FString>& Silenced()
	{
		FState& Current = State();
		if (!Current.bSilencedLoaded)
		{
			Current.Silenced = FHyperAIStudioMcpBypassMonitor::ReadSilenced(ConfigFile());
			Current.bSilencedLoaded = true;
		}
		return Current.Silenced;
	}

	void ApplySilenced(TArray<FHyperAIStudioMcpFinding>& Findings)
	{
		for (FHyperAIStudioMcpFinding& Finding : Findings)
		{
			Finding.bSilenced = Silenced().Contains(Finding.Id);
		}
	}

	FString DescribeAll(const TArray<FHyperAIStudioMcpFinding>& Findings)
	{
		FString Text;
		for (const FHyperAIStudioMcpFinding& Finding : Findings)
		{
			Text += FString::Printf(TEXT("%s|%d|%d|%d|%d|%d\n"), *Finding.Id, static_cast<int32>(Finding.Class), static_cast<int32>(Finding.Reason),
				Finding.Port, Finding.bSilenced ? 1 : 0, Finding.bWrittenByHyperAI ? 1 : 0);
			for (const FHyperAIStudioMcpServerSource& Source : Finding.Sources)
			{
				Text += FString::Printf(TEXT("  %d|%s|%s|%s|%s|%s|%s\n"), static_cast<int32>(Source.Kind), *Source.Client, *Source.Scope, *Source.File,
					*Source.Container, *Source.EntryName, *Source.Address);
			}
		}
		return Text;
	}

	/** Python's Remote Execution setting, read by reflection: its settings class is private to PythonScriptPlugin. */
	bool IsPythonRemoteExecutionOn()
	{
		// Looked up once: a class of a loaded module stays, and finding objects is not allowed while a package saves.
		static TWeakObjectPtr<UClass> Found;
		static bool bLookedUp = false;
		if (!bLookedUp && !UE::IsSavingPackage() && !IsGarbageCollecting())
		{
			Found = FindObject<UClass>(nullptr, TEXT("/Script/PythonScriptPlugin.PythonScriptPluginSettings"));
			// Before the first frame the Python plugin may not have loaded yet; look again later then.
			bLookedUp = Found.IsValid() || GFrameCounter > 1;
		}
		const UClass* Class = Found.Get();
		if (!Class)
		{
			return false;
		}
		const FBoolProperty* Property = FindFProperty<FBoolProperty>(Class, TEXT("bRemoteExecution"));
		const UObject* Defaults = Class->GetDefaultObject(false);
		return Property && Defaults && Property->GetPropertyValue_InContainer(Defaults);
	}

	/** Changes when a config file, the Python setting or the listening ports change: the cheap check before a reread. */
	FString BuildWatchSignature(const FHyperAIStudioMcpScanPaths& Paths)
	{
		FString Signature;
		for (const FString& File : HyperAIStudio::McpBypass::GetConfigFiles(Paths))
		{
			const FFileStatData Stat = IFileManager::Get().GetStatData(*File);
			Signature += Stat.bIsValid
				? FString::Printf(TEXT("%s|%lld|%lld\n"), *File, Stat.ModificationTime.GetTicks(), Stat.FileSize)
				: FString::Printf(TEXT("%s|-\n"), *File);
		}
		Signature += IsPythonRemoteExecutionOn() ? TEXT("python-remote|1\n") : TEXT("python-remote|0\n");
		TArray<FString> Ports;
		for (const int32 Port : FHyperAIStudioMcpBypassMonitor::GetListeningPorts())
		{
			Ports.Add(FString::FromInt(Port));
		}
		return Signature + TEXT("ports|") + FString::Join(Ports, TEXT(","));
	}

	bool CanWarn()
	{
		if (IsRunningCommandlet() || FApp::IsUnattended() || GIsAutomationTesting || !FSlateApplication::IsInitialized())
		{
			return false;
		}
		IMainFrameModule* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>(TEXT("MainFrame"));
		return MainFrame && MainFrame->IsWindowInitialized();
	}

	/** The notification's text and keys now: unsilenced bypasses only. */
	TArray<FHyperAIStudioMcpFinding> Unsilenced()
	{
		return HyperAIStudio::McpBypass::FilterBypass(FHyperAIStudioMcpBypassMonitor::GetFindings(), /*bUnsilencedOnly=*/true);
	}

	void CloseNotice()
	{
		if (const TSharedPtr<SNotificationItem> Notice = State().Notice.Pin())
		{
			Notice->SetCompletionState(SNotificationItem::CS_None);
			Notice->ExpireAndFadeout();
		}
		State().Notice.Reset();
	}

	/** AGENTS.md's Other MCP Servers block follows the bypass names, once Setup has written the file. */
	void RefreshInstructions()
	{
		if (State().TestFindings.IsSet())
		{
			// A test's findings never reach the project's AGENTS.md.
			return;
		}
		const FString Names = FString::Join(HyperAIStudio::McpBypass::GetBypassNames(State().Findings), TEXT("\n"));
		if ((!State().InstructionNames.IsSet() || Names != State().InstructionNames.GetValue()) && FHyperAIStudioService::RefreshMcpBypassInstructions())
		{
			State().InstructionNames = Names;
		}
	}

	bool Tick(float)
	{
		FState& Current = State();
		if (UE::IsSavingPackage() || IsGarbageCollecting())
		{
			return true;
		}
		const FString Watch = BuildWatchSignature(FHyperAIStudioMcpBypassMonitor::GetScanPaths());
		if (!Current.bScanned || Watch != Current.WatchSignature)
		{
			FHyperAIStudioMcpBypassMonitor::Rescan();
		}
		FHyperAIStudioMcpBypassMonitor::ShowNotificationIfNew();
		return true;
	}

	/** The first check: as soon as the editor ticks; warns once its window is up. */
	bool FirstTick(float DeltaTime)
	{
		FState& Current = State();
		if (!Current.bScanned)
		{
			FHyperAIStudioMcpBypassMonitor::Rescan();
		}
		RefreshInstructions();
		if (!CanWarn() && !IsRunningCommandlet() && !FApp::IsUnattended())
		{
			// Try again on the next pass; the main window is not up yet.
			return true;
		}
		Current.bStartupWarned = true;
		FHyperAIStudioMcpBypassMonitor::ShowNotificationIfNew();
		Current.FirstTicker.Reset();
		Current.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), CheckSeconds);
		return false;
	}
}

void FHyperAIStudioMcpBypassMonitor::Startup()
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	FState& Current = State();
	if (Current.bStarted || IsRunningCommandlet())
	{
		return;
	}
	Current.bStarted = true;
	Current.FirstTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&FirstTick), FirstCheckSeconds);
}

void FHyperAIStudioMcpBypassMonitor::Shutdown()
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	FState& Current = State();
	if (Current.FirstTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Current.FirstTicker);
		Current.FirstTicker.Reset();
	}
	if (Current.Ticker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Current.Ticker);
		Current.Ticker.Reset();
	}
	if (const TSharedPtr<SWindow> Window = Current.DetailsWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
	Current.Notice.Reset();
	Current.bStarted = false;
}

TArray<FHyperAIStudioMcpFinding> FHyperAIStudioMcpBypassMonitor::GetFindings()
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	FState& Current = State();
	if (Current.TestFindings.IsSet())
	{
		TArray<FHyperAIStudioMcpFinding> Findings = Current.TestFindings.GetValue();
		ApplySilenced(Findings);
		return Findings;
	}
	if (!Current.bScanned)
	{
		Rescan();
	}
	return Current.Findings;
}

TArray<FString> FHyperAIStudioMcpBypassMonitor::GetProblems()
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	return State().TestFindings.IsSet() ? TArray<FString>() : State().Problems;
}

bool FHyperAIStudioMcpBypassMonitor::Rescan()
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	FState& Current = State();
	if (!IsInGameThread())
	{
		return false;
	}
	const FHyperAIStudioMcpScanPaths Paths = GetScanPaths();
	Current.WatchSignature = BuildWatchSignature(Paths);
	TArray<FString> Problems;
	const TArray<FHyperAIStudioConfiguredMcpServer> Servers = HyperAIStudio::McpBypass::ReadConfiguredServers(Paths, Problems);
	TArray<FHyperAIStudioMcpFinding> Findings = HyperAIStudio::McpBypass::Classify(Servers, GatherEditorFacts(), Silenced());
	const bool bFirst = !Current.bScanned;
	Current.bScanned = true;
	const FString Signature = DescribeAll(Findings) + FString::Join(Problems, TEXT("\n"));
	if (!bFirst && Signature == Current.FindingsSignature)
	{
		return false;
	}
	Current.FindingsSignature = Signature;
	Current.Findings = MoveTemp(Findings);
	Current.Problems = MoveTemp(Problems);

	// The log line: what bypasses HyperAI and where, never a value from the files. A test's run logs it plainly.
	const FString Line = HyperAIStudio::McpBypass::BuildLogLine(Current.Findings);
	const int32 Others = Current.Findings.Num() - HyperAIStudio::McpBypass::FilterBypass(Current.Findings, false).Num();
	if (!Line.IsEmpty() && !GIsAutomationTesting)
	{
		UE_LOG(LogHyperAIStudioMcpBypass, Warning, TEXT("%s"), *Line);
	}
	else if (!Line.IsEmpty())
	{
		UE_LOG(LogHyperAIStudioMcpBypass, Display, TEXT("%s"), *Line);
	}
	else
	{
		UE_LOG(LogHyperAIStudioMcpBypass, Display, TEXT("No other MCP server or editor plugin changes this editor outside HyperAI's approval gate."));
	}
	if (Others > 0)
	{
		TArray<FString> Names;
		for (const FHyperAIStudioMcpFinding& Finding : Current.Findings)
		{
			if (!Finding.ChangesEditor())
			{
				Names.Add(Finding.DisplayName);
			}
		}
		UE_LOG(LogHyperAIStudioMcpBypass, Display, TEXT("Other MCP servers agents have, which do not reach this editor: %s."), *FString::Join(Names, TEXT(", ")));
	}
	for (const FString& Problem : Current.Problems)
	{
		UE_LOG(LogHyperAIStudioMcpBypass, Display, TEXT("%s"), *Problem);
	}
	if (!bFirst)
	{
		RefreshInstructions();
	}
	Current.Changed.Broadcast();
	return true;
}

FSimpleMulticastDelegate& FHyperAIStudioMcpBypassMonitor::OnChanged()
{
	return HyperAIStudio::McpBypass::Monitor::State().Changed;
}

TSet<FString> FHyperAIStudioMcpBypassMonitor::ReadSilenced(const FString& ConfigFile)
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	TSet<FString> Ids;
	if (!GConfig || ConfigFile.IsEmpty())
	{
		return Ids;
	}
	TArray<FString> Values;
	GConfig->GetArray(ConfigSection, SilencedKey, Values, ConfigFile);
	for (FString& Value : Values)
	{
		Value.TrimStartAndEndInline();
		if (!Value.IsEmpty())
		{
			Ids.Add(Value.ToLower());
		}
	}
	return Ids;
}

void FHyperAIStudioMcpBypassMonitor::WriteSilenced(const FString& ConfigFile, const TSet<FString>& Ids)
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	if (!GConfig || ConfigFile.IsEmpty())
	{
		return;
	}
	TArray<FString> Values = Ids.Array();
	Values.Sort();
	GConfig->SetArray(ConfigSection, SilencedKey, Values, ConfigFile);
	GConfig->Flush(/*bRemoveFromCache=*/false, ConfigFile);
}

void FHyperAIStudioMcpBypassMonitor::SetWarningSilenced(const FString& Id, const bool bSilenced)
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	FState& Current = State();
	const FString Key = Id.ToLower();
	TSet<FString> Ids = Silenced();
	if (bSilenced)
	{
		Ids.Add(Key);
	}
	else
	{
		Ids.Remove(Key);
	}
	WriteSilenced(ConfigFile(), Ids);
	Current.Silenced = MoveTemp(Ids);
	Current.bSilencedLoaded = true;
	ApplySilenced(Current.Findings);
	Current.FindingsSignature = DescribeAll(Current.Findings) + FString::Join(Current.Problems, TEXT("\n"));
	UE_LOG(LogHyperAIStudioMcpBypass, Display, TEXT("%s %s in this project."), bSilenced ? TEXT("No longer warning about") : TEXT("Warning again about"), *Key);
	if (bSilenced)
	{
		// What else it named was warned about already; it does not come back for those.
		CloseNotice();
	}
	Current.Changed.Broadcast();
}

void FHyperAIStudioMcpBypassMonitor::ShowNotificationIfNew()
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	FState& Current = State();
	const TArray<FHyperAIStudioMcpFinding> Findings = Unsilenced();
	const TArray<FString> Keys = HyperAIStudio::McpBypass::BuildWarningKeys(Findings);
	if (Keys.IsEmpty())
	{
		CloseNotice();
		return;
	}
	const bool bNew = Keys.ContainsByPredicate([&Current](const FString& Key) { return !Current.NotifiedKeys.Contains(Key); });
	if (!bNew || !CanWarn())
	{
		return;
	}
	Current.NotifiedKeys.Append(Keys);
	CloseNotice();

	FNotificationInfo Info(HyperAIStudio::McpBypass::BuildNotificationText(Findings));
	Info.bFireAndForget = false;
	Info.ExpireDuration = 0.0f;
	Info.WidthOverride = 520.0f;
	Info.Image = FAppStyle::GetBrush(TEXT("Icons.WarningWithColor"));
	Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("ShowDetails", "Show details"),
		LOCTEXT("ShowDetailsTooltip", "List every MCP server agents have here, where each is configured, and how to remove it."),
		FSimpleDelegate::CreateStatic(&FHyperAIStudioMcpBypassMonitor::OpenDetails), SNotificationItem::CS_None));
	for (int32 Index = 0; Index < FMath::Min(Findings.Num(), MaxSilenceButtons); ++Index)
	{
		const FString Id = Findings[Index].Id;
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			FText::Format(LOCTEXT("DontWarnAgain", "Don't warn about {0} again"), FText::FromString(Findings[Index].DisplayName)),
			LOCTEXT("DontWarnAgainTooltip", "Stop this warning in this project, for you. Agents can still use it; Settings > Agents still lists it, and can warn again."),
			FSimpleDelegate::CreateLambda([Id]() { FHyperAIStudioMcpBypassMonitor::SetWarningSilenced(Id, true); }), SNotificationItem::CS_None));
	}
	Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("DismissWarning", "Dismiss"),
		LOCTEXT("DismissWarningTooltip", "Close this warning. It comes back when another such server, or another place for one, appears."),
		FSimpleDelegate::CreateStatic(&CloseNotice), SNotificationItem::CS_None));
	Current.Notice = FSlateNotificationManager::Get().AddNotification(Info);
	if (const TSharedPtr<SNotificationItem> Notice = Current.Notice.Pin())
	{
		Notice->SetCompletionState(SNotificationItem::CS_None);
	}
}

void FHyperAIStudioMcpBypassMonitor::OpenDetails()
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	if (!IsInGameThread() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	if (const TSharedPtr<SWindow> Existing = State().DetailsWindow.Pin())
	{
		Existing->BringToFront();
		return;
	}
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("DetailsTitle", "Other MCP servers"))
		.ClientSize(FVector2D(760.0f, 560.0f))
		.SupportsMaximize(true)
		.SupportsMinimize(false);
	Window->SetContent(SNew(SHyperAIStudioMcpBypassDetails).Window(Window));
	State().DetailsWindow = Window;
	IMainFrameModule* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>(TEXT("MainFrame"));
	const TSharedPtr<SWindow> Parent = MainFrame ? MainFrame->GetParentWindow() : nullptr;
	if (Parent.IsValid())
	{
		FSlateApplication::Get().AddWindowAsNativeChild(Window, Parent.ToSharedRef());
	}
	else
	{
		FSlateApplication::Get().AddWindow(Window);
	}
}

FHyperAIStudioMcpScanPaths FHyperAIStudioMcpBypassMonitor::GetScanPaths()
{
	auto Env = [](const TCHAR* Name)
	{
		FString Value = FPlatformMisc::GetEnvironmentVariable(Name);
		Value.TrimStartAndEndInline();
		FPaths::NormalizeDirectoryName(Value);
		return Value;
	};
	FHyperAIStudioMcpScanPaths Paths;
	Paths.ProjectRoot = FHyperAIStudioService::GetProjectRoot();
	FPaths::NormalizeDirectoryName(Paths.ProjectRoot);
	Paths.Home = Env(TEXT("USERPROFILE"));
	if (Paths.Home.IsEmpty())
	{
		Paths.Home = Env(TEXT("HOME"));
	}
	const FString ClaudeConfigDir = Env(TEXT("CLAUDE_CONFIG_DIR"));
	Paths.ClaudeConfigDir = ClaudeConfigDir.IsEmpty() ? Paths.Home : ClaudeConfigDir;
	const FString CodexHome = Env(TEXT("CODEX_HOME"));
	Paths.CodexHome = !CodexHome.IsEmpty() ? CodexHome : Paths.Home.IsEmpty() ? FString() : Paths.Home / TEXT(".codex");
	const FString AppData = Env(TEXT("APPDATA"));
	Paths.VSCodeUserDir = AppData.IsEmpty() ? FString() : AppData / TEXT("Code/User");
	return Paths;
}

FHyperAIStudioMcpEditorFacts FHyperAIStudioMcpBypassMonitor::GatherEditorFacts()
{
	FHyperAIStudioMcpEditorFacts Facts;
	if (const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>())
	{
		Facts.HyperAIPort = static_cast<int32>(Settings->UnrealMCPPort);
		for (const FHyperAIStudioMCPServerEntry& Entry : Settings->ExtraServers)
		{
			if (!Entry.bEnabled)
			{
				continue;
			}
			// As HyperAI names the entries it writes: lower case, spaces, dots and slashes as dashes.
			FString Id;
			for (const TCHAR Character : (Entry.Id.IsEmpty() ? Entry.DisplayName : Entry.Id).ToLower())
			{
				if (FChar::IsAlnum(Character) || Character == TEXT('-') || Character == TEXT('_'))
				{
					Id.AppendChar(Character);
				}
				else if (Character == TEXT(' ') || Character == TEXT('.') || Character == TEXT('/'))
				{
					Id.AppendChar(TEXT('-'));
				}
			}
			if (!Id.IsEmpty())
			{
				Facts.HyperAIExtraServerIds.AddUnique(Id);
			}
		}
	}
	Facts.ListeningPorts = GetListeningPorts();
	Facts.bPythonRemoteExecution = HyperAIStudio::McpBypass::Monitor::IsPythonRemoteExecutionOn();
	const FHyperAIStudioMcpScanPaths Paths = GetScanPaths();
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
	{
		const FString Name = Plugin->GetName();
		const FString Friendly = Plugin->GetFriendlyName();
		// Only the plugins classification knows about: Nwiro, RiderLink, and any named for MCP.
		const bool bCandidate = Name.Contains(TEXT("nwiro")) || Friendly.Contains(TEXT("nwiro")) || Name.Equals(TEXT("RiderLink"), ESearchCase::IgnoreCase)
			|| Name.Contains(TEXT("mcp")) || Friendly.Contains(TEXT("mcp"));
		if (!bCandidate)
		{
			continue;
		}
		FHyperAIStudioMcpPluginFact& Fact = Facts.Plugins.AddDefaulted_GetRef();
		Fact.Name = Name;
		Fact.FriendlyName = Friendly;
		Fact.CreatedBy = Plugin->GetDescriptor().CreatedBy;
		Fact.BaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
		Fact.ShortBaseDir = HyperAIStudio::McpBypass::ShortenPath(Fact.BaseDir, Paths.ProjectRoot, Paths.Home);
	}
	return Facts;
}

TArray<int32> FHyperAIStudioMcpBypassMonitor::GetListeningPorts()
{
	TArray<int32> Ports;
#if PLATFORM_WINDOWS
	const DWORD ProcessId = static_cast<DWORD>(FPlatformProcess::GetCurrentProcessId());
	// The table's port is in network byte order in its low 16 bits.
	auto PortOf = [](const DWORD Raw) { return static_cast<int32>(((Raw & 0xFF) << 8) | ((Raw >> 8) & 0xFF)); };
	for (const ULONG Family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)})
	{
		TArray<uint8> Buffer;
		DWORD Size = 0;
		DWORD Result = ERROR_INSUFFICIENT_BUFFER;
		// The table can grow between the size query and the read; a few tries are plenty.
		for (int32 Attempt = 0; Attempt < 3 && Result == ERROR_INSUFFICIENT_BUFFER; ++Attempt)
		{
			Buffer.SetNumZeroed(FMath::Max<int32>(static_cast<int32>(Size), 1));
			Result = ::GetExtendedTcpTable(Buffer.GetData(), &Size, false, Family, TCP_TABLE_OWNER_PID_LISTENER, 0);
		}
		if (Result != NO_ERROR)
		{
			continue;
		}
		if (Family == AF_INET)
		{
			const MIB_TCPTABLE_OWNER_PID* Table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(Buffer.GetData());
			for (DWORD Index = 0; Index < Table->dwNumEntries; ++Index)
			{
				if (Table->table[Index].dwOwningPid == ProcessId)
				{
					Ports.AddUnique(PortOf(Table->table[Index].dwLocalPort));
				}
			}
		}
		else
		{
			const MIB_TCP6TABLE_OWNER_PID* Table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(Buffer.GetData());
			for (DWORD Index = 0; Index < Table->dwNumEntries; ++Index)
			{
				if (Table->table[Index].dwOwningPid == ProcessId)
				{
					Ports.AddUnique(PortOf(Table->table[Index].dwLocalPort));
				}
			}
		}
	}
	Ports.Sort();
#endif
	return Ports;
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioMcpBypassMonitor::SetFindingsForTest(const TOptional<TArray<FHyperAIStudioMcpFinding>>& Findings)
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	State().TestFindings = Findings;
	State().Changed.Broadcast();
}

void FHyperAIStudioMcpBypassMonitor::SetConfigFileForTest(const FString& ConfigFile)
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	State().ConfigFileOverride = ConfigFile;
	State().bSilencedLoaded = false;
	ApplySilenced(State().Findings);
}

FText FHyperAIStudioMcpBypassMonitor::GetPendingNotificationForTest(bool& bOutWouldShow)
{
	using namespace HyperAIStudio::McpBypass::Monitor;
	const TArray<FHyperAIStudioMcpFinding> Findings = Unsilenced();
	const TArray<FString> Keys = HyperAIStudio::McpBypass::BuildWarningKeys(Findings);
	bOutWouldShow = Keys.ContainsByPredicate([](const FString& Key) { return !State().NotifiedKeys.Contains(Key); });
	return HyperAIStudio::McpBypass::BuildNotificationText(Findings);
}

void FHyperAIStudioMcpBypassMonitor::ResetNotifiedForTest()
{
	HyperAIStudio::McpBypass::Monitor::State().NotifiedKeys.Reset();
}

FString FHyperAIStudioMcpBypassMonitor::GetConfigFileForTest()
{
	return HyperAIStudio::McpBypass::Monitor::ConfigFile();
}
#endif

#undef LOCTEXT_NAMESPACE
