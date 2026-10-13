// Games by Hyper 2026.

#include "HyperAIStudioPhonePlaytest.h"

#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioPixelStreamingBackend.h"
#include "HyperAIStudioQrCode.h"
#include "Interfaces/IPluginManager.h"
#include "IPAddress.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPhonePlaytest, Log, All);

namespace
{
	TSharedPtr<IHyperAIStudioPixelStreamingBackend>& BackendSlot()
	{
		static TSharedPtr<IHyperAIStudioPixelStreamingBackend> Backend;
		return Backend;
	}
}

void FHyperAIStudioPixelStreamingBackends::Register(TSharedPtr<IHyperAIStudioPixelStreamingBackend> Backend)
{
	BackendSlot() = MoveTemp(Backend);
}

void FHyperAIStudioPixelStreamingBackends::Unregister()
{
	BackendSlot().Reset();
}

TSharedPtr<IHyperAIStudioPixelStreamingBackend> FHyperAIStudioPixelStreamingBackends::Get()
{
	return BackendSlot();
}

namespace HyperAIStudio::PhonePlaytest
{
	namespace
	{
		struct FSession
		{
			/** awaiting_user_approval, streaming or idle */
			FString State = TEXT("idle");
			FHyperAIPhonePlaytestRequest Request;
			FHyperAIPhonePlaytestReport Plan;
			FProcHandle Game;
			FString StreamerId;
		};

		FSession& Session()
		{
			static FSession Value;
			return Value;
		}

		TArray<FString>& TestAddresses()
		{
			static TArray<FString> Value;
			return Value;
		}

		bool ParseIpv4(const FString& Address, uint8 (&Out)[4])
		{
			TArray<FString> Parts;
			Address.TrimStartAndEnd().ParseIntoArray(Parts, TEXT("."), /*InCullEmpty=*/false);
			if (Parts.Num() != 4)
			{
				return false;
			}
			for (int32 Index = 0; Index < 4; ++Index)
			{
				if (Parts[Index].IsEmpty() || Parts[Index].Len() > 3 || !Parts[Index].IsNumeric() || Parts[Index].Contains(TEXT("-")) || Parts[Index].Contains(TEXT(".")))
				{
					return false;
				}
				const int32 Value = FCString::Atoi(*Parts[Index]);
				if (Value < 0 || Value > 255)
				{
					return false;
				}
				Out[Index] = static_cast<uint8>(Value);
			}
			return true;
		}

		FString TargetName(const EHyperAIPhonePlaytestTarget Target)
		{
			return Target == EHyperAIPhonePlaytestTarget::Standalone ? TEXT("Standalone") : TEXT("PlayInEditor");
		}

		FString QrPath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("HyperAIStudio"), TEXT("PhonePlaytest"), TEXT("qr.png")));
		}

		FString ExposureText(const FString& Lan, const int32 HttpPort, const int32 StreamerPort)
		{
			return FString::Printf(TEXT("Pixel Streaming's signalling server runs inside this editor on ports %d (the player page and viewers) and %d (the game's stream). ")
				TEXT("It listens on every network interface of this PC, because the engine's server has no setting to bind one address; this PC has no public ")
				TEXT("internet address, so only devices on its local network (%s) reach it, unless a router forwards these ports. Its websockets take only pages served ")
				TEXT("from http://%s:%d or this PC. Nothing is downloaded and no internet server is used."), HttpPort, StreamerPort, *Lan, *Lan, HttpPort);
		}

		/** Resolves Standalone's level: Request.Map or the open level, which must be saved to disk. */
		bool ResolveMap(const FHyperAIPhonePlaytestRequest& Request, FString& OutMap, TArray<FString>& OutNotes, FString& OutError)
		{
			OutMap = Request.Map.TrimStartAndEnd();
			const UWorld* Editor = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (OutMap.IsEmpty())
			{
				OutMap = Editor ? Editor->GetOutermost()->GetName() : FString();
				if (Editor && Editor->GetOutermost()->IsDirty())
				{
					OutNotes.Add(TEXT("The open level has unsaved changes: a Standalone game plays it as saved. Use Target PlayInEditor to stream the unsaved level."));
				}
			}
			if (!FPackageName::IsValidLongPackageName(OutMap) || !FPackageName::DoesPackageExist(OutMap))
			{
				OutError = FString::Printf(TEXT("%s is not a saved level; save it first, or pass map as a package path such as /Game/Maps/Arena."),
					OutMap.IsEmpty() ? TEXT("The open level") : *OutMap);
				return false;
			}
			return true;
		}
	}

	EAddressKind ClassifyIpv4(const FString& Address)
	{
		uint8 B[4];
		if (!ParseIpv4(Address, B))
		{
			return EAddressKind::Invalid;
		}
		if (B[0] == 127)
		{
			return EAddressKind::Loopback;
		}
		if (B[0] == 10 || (B[0] == 172 && B[1] >= 16 && B[1] <= 31) || (B[0] == 192 && B[1] == 168))
		{
			return EAddressKind::Private;
		}
		if (B[0] == 169 && B[1] == 254)
		{
			return EAddressKind::LinkLocal;
		}
		if (B[0] == 100 && B[1] >= 64 && B[1] <= 127)
		{
			return EAddressKind::CarrierNat;
		}
		if (B[0] == 0 || B[0] >= 224)
		{
			return EAddressKind::Invalid;
		}
		return EAddressKind::Public;
	}

	FString PickLanAddress(const TArray<FString>& Addresses, const FString& Requested, FString& OutError)
	{
		TArray<FString> Private = Addresses.FilterByPredicate([](const FString& Address) { return ClassifyIpv4(Address) == EAddressKind::Private; });
		if (!Requested.IsEmpty())
		{
			if (ClassifyIpv4(Requested) != EAddressKind::Private)
			{
				OutError = FString::Printf(TEXT("%s is not a private LAN address (10.x, 172.16-31.x or 192.168.x)."), *Requested);
				return FString();
			}
			if (!Private.Contains(Requested))
			{
				OutError = FString::Printf(TEXT("%s is not one of this PC's addresses (%s)."), *Requested, *FString::Join(Private, TEXT(", ")));
				return FString();
			}
			return Requested;
		}
		Private.StableSort([](const FString& A, const FString& B) { return A.StartsWith(TEXT("192.168.")) && !B.StartsWith(TEXT("192.168.")); });
		if (Private.IsEmpty())
		{
			OutError = TEXT("This PC has no private LAN address (10.x, 172.16-31.x or 192.168.x) for phones to reach; join it to the phones' Wi-Fi network.");
			return FString();
		}
		return Private[0];
	}

	FString AllowedOrigins(const FString& LanAddress, const int32 HttpPort)
	{
		// The game's own stream connects from this PC, its websocket naming 127.0.0.1 as its origin.
		return FString::Printf(TEXT("http://%s:%d,http://127.0.0.1:%d,http://localhost:%d,127.0.0.1"), *LanAddress, HttpPort, HttpPort, HttpPort);
	}

	FString PlayerUrl(const FString& LanAddress, const int32 HttpPort, const FString& StreamerId)
	{
		return FString::Printf(TEXT("http://%s:%d/?StreamerId=%s&AutoConnect=true&AutoPlayVideo=true&StartVideoMuted=true&TouchInput=true&HoveringMouse=true"),
			*LanAddress, HttpPort, *StreamerId);
	}

	FString GameArguments(const FHyperAIPhonePlaytestRequest& Request, const FString& Map, const FString& GameLogPath)
	{
		FString Args = FString::Printf(TEXT("%s -game -PixelStreamingConnectionURL=ws://127.0.0.1:%d -PixelStreamingID=%s -PixelStreamingInputController=%s ")
			TEXT("-PixelStreamingEncoderMaxSessions=%d -ResX=%d -ResY=%d %s -nosplash -NoLiveCoding -abslog=\"%s\""),
			*Map, Request.StreamerPort, StandaloneStreamerId, Request.bAllViewersControl ? TEXT("Any") : TEXT("Host"), Request.MaxViewers,
			Request.ResX, Request.ResY, Request.bOffscreen ? TEXT("-RenderOffScreen") : TEXT("-windowed"), *GameLogPath);
		return Args;
	}

	bool Validate(const FHyperAIPhonePlaytestRequest& Request, FString& OutError)
	{
		if (Request.HttpPort < MinPort || Request.HttpPort > MaxPort || Request.StreamerPort < MinPort || Request.StreamerPort > MaxPort)
		{
			OutError = FString::Printf(TEXT("httpPort and streamerPort are %d to %d (ports under 1024 need administrator rights)."), MinPort, MaxPort);
			return false;
		}
		if (Request.HttpPort == Request.StreamerPort)
		{
			OutError = TEXT("httpPort and streamerPort must differ.");
			return false;
		}
		if (Request.MaxViewers < 1 || Request.MaxViewers > MaxViewers)
		{
			OutError = FString::Printf(TEXT("maxViewers is 1 to %d."), MaxViewers);
			return false;
		}
		if (!StaticEnum<EHyperAIPhonePlaytestTarget>()->IsValidEnumValue(static_cast<int64>(Request.Target)))
		{
			OutError = TEXT("target is PlayInEditor or Standalone.");
			return false;
		}
		const bool bStandalone = Request.Target == EHyperAIPhonePlaytestTarget::Standalone;
		if (!bStandalone && (!Request.Map.IsEmpty() || Request.bOffscreen))
		{
			OutError = TEXT("map and bOffscreen are for target Standalone; PlayInEditor streams the PIE session.");
			return false;
		}
		if (Request.ResX < 320 || Request.ResX > 3840 || Request.ResY < 240 || Request.ResY > 2160)
		{
			OutError = TEXT("resX is 320 to 3840 and resY 240 to 2160.");
			return false;
		}
		if (Request.Map.Len() > 512 || Request.Address.Len() > 64 || Request.OperationId.Len() > 128)
		{
			OutError = TEXT("map, address or operationId is too long.");
			return false;
		}
		return true;
	}

	void SetAddressesForTests(const TArray<FString>& Addresses)
	{
		TestAddresses() = Addresses;
	}

	TArray<FString> LocalAddresses()
	{
		if (!TestAddresses().IsEmpty())
		{
			return TestAddresses();
		}
		TArray<FString> Out;
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		TArray<TSharedPtr<FInternetAddr>> Adapters;
		if (Sockets && Sockets->GetLocalAdapterAddresses(Adapters))
		{
			for (const TSharedPtr<FInternetAddr>& Adapter : Adapters)
			{
				const FString Text = Adapter.IsValid() ? Adapter->ToString(/*bAppendPort=*/false) : FString();
				if (ClassifyIpv4(Text) != EAddressKind::Invalid)
				{
					Out.AddUnique(Text);
				}
			}
		}
		return Out;
	}

	bool IsPortFree(const int32 Port)
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		FSocket* Probe = Sockets ? Sockets->CreateSocket(NAME_Stream, TEXT("HyperAIPortProbe"), false) : nullptr;
		if (!Probe)
		{
			return false;
		}
		const TSharedRef<FInternetAddr> Any = Sockets->CreateInternetAddr();
		Any->SetAnyAddress();
		Any->SetPort(Port);
		const bool bFree = Probe->Bind(*Any);
		Probe->Close();
		Sockets->DestroySocket(Probe);
		return bFree;
	}

	namespace
	{
		FHyperAIPhonePlaytestReport Refuse(FHyperAIPhonePlaytestReport Report, const FString& Diagnostic, const TCHAR* Status = TEXT("refused"))
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Diagnostic;
			return Report;
		}

		/** After approval: the server, then the game or PIE streaming. */
		void Begin(const FHyperAIPhonePlaytestRequest& Request, const FHyperAIPhonePlaytestReport& Plan)
		{
			FSession& Live = Session();
			const TSharedPtr<IHyperAIStudioPixelStreamingBackend> Backend = FHyperAIStudioPixelStreamingBackends::Get();
			FString Error;
			if (!Backend || !Backend->StartSignalling(Request.HttpPort, Request.StreamerPort, AllowedOrigins(Plan.Address, Request.HttpPort), Error))
			{
				UE_LOG(LogHyperAIStudioPhonePlaytest, Warning, TEXT("Phone playtest server did not start: %s"), *Error);
				Live = FSession();
				Live.Plan = Plan;
				Live.Plan.Status = TEXT("failed");
				Live.Plan.Diagnostic = TEXT("The signalling server did not start: ") + (Error.IsEmpty() ? FString(TEXT("Pixel Streaming 2 is not available")) : Error);
				return;
			}
			if (Request.Target == EHyperAIPhonePlaytestTarget::Standalone)
			{
				const FString Editor = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::EngineDir(), TEXT("Binaries"), TEXT("Win64"), TEXT("UnrealEditor.exe")));
				const FString Params = FString::Printf(TEXT("\"%s\" %s"), *FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()), *Plan.GameCommandLine);
				uint32 ProcessId = 0;
				Live.Game = FPlatformProcess::CreateProc(*Editor, *Params, /*bLaunchDetached=*/true, /*bLaunchHidden=*/false, /*bLaunchReallyHidden=*/false,
					&ProcessId, /*PriorityModifier=*/0, nullptr, nullptr);
				if (!Live.Game.IsValid())
				{
					Backend->StopSignalling();
					Live = FSession();
					Live.Plan = Plan;
					Live.Plan.Status = TEXT("failed");
					Live.Plan.Diagnostic = TEXT("The game process did not start: ") + Editor;
					return;
				}
				Live.StreamerId = StandaloneStreamerId;
			}
			else
			{
				Backend->SetPlayInEditorStreaming(true, Request.StreamerPort, Request.MaxViewers, Request.bAllViewersControl);
				Live.StreamerId = Backend->PlayInEditorStreamerId();
			}
			Live.State = TEXT("streaming");
			UE_LOG(LogHyperAIStudioPhonePlaytest, Log, TEXT("Phone playtest streaming at %s"), *Plan.Url);
		}
	}

	FHyperAIPhonePlaytestReport Start(const FHyperAIPhonePlaytestRequest& InRequest)
	{
		FHyperAIPhonePlaytestRequest Request = InRequest;
		FHyperAIPhonePlaytestReport Report;
		Report.Target = TargetName(Request.Target);
		Report.HttpPort = Request.HttpPort;
		Report.StreamerPort = Request.StreamerPort;
		Report.MaxViewers = Request.MaxViewers;
		Report.OperationId = Request.OperationId.IsEmpty() ? TEXT("phone-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower() : Request.OperationId;
		Request.OperationId = Report.OperationId;
		FString Error;
		if (!IsInGameThread())
		{
			return Refuse(Report, TEXT("hyper_phone_playtest_start runs on the game thread."));
		}
		if (!Validate(Request, Error))
		{
			return Refuse(Report, Error);
		}
		if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Report.OperationId))
		{
			return Refuse(Report, TEXT("operation_id must be a short journal-safe id, or empty to have one made."));
		}
		if (Session().State != TEXT("idle"))
		{
			return Refuse(Report, FString::Printf(TEXT("A phone playtest is already %s (%s); hyper_phone_playtest_stop it first."),
				*Session().State.Replace(TEXT("_"), TEXT(" ")), *Session().Plan.Url), TEXT("busy"));
		}
		if (!IPluginManager::Get().FindEnabledPlugin(TEXT("PixelStreaming2")).IsValid())
		{
			return Refuse(Report, TEXT("Phone playtests stream with Epic's Pixel Streaming 2 plugin, which this project has not enabled. Enable it in Edit > Plugins ")
				TEXT("(or the .uproject's Plugins list) and restart the editor, with hyper_editor_restart from an agent."));
		}
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("HyperAIStudioPixelStreaming")))
		{
			FModuleManager::Get().LoadModule(TEXT("HyperAIStudioPixelStreaming"));
		}
		const TSharedPtr<IHyperAIStudioPixelStreamingBackend> Backend = FHyperAIStudioPixelStreamingBackends::Get();
		if (!Backend)
		{
			return Refuse(Report, TEXT("HyperAI's Pixel Streaming module did not load; the plugin's modules may not be built for this engine."));
		}
		const FString Page = FPaths::Combine(Backend->PlayerPageFolder(), TEXT("player.html"));
		if (!IFileManager::Get().FileExists(*Page))
		{
			return Refuse(Report, FString::Printf(TEXT("The Pixel Streaming player page is missing (%s). Epic's get_ps_servers script in the plugin's Resources/WebServers ")
				TEXT("downloads it from GitHub; HyperAI does not download it for you. Run that script yourself if you accept the download."), *Page));
		}
		const TArray<FString> Addresses = LocalAddresses();
		for (const FString& Address : Addresses)
		{
			if (ClassifyIpv4(Address) == EAddressKind::Public)
			{
				return Refuse(Report, FString::Printf(TEXT("This PC has a public internet address (%s), so a server listening on all its interfaces would be reachable ")
					TEXT("from the internet. Phone playtests run only on a PC behind a router."), *Address));
			}
			if (ClassifyIpv4(Address) == EAddressKind::Private)
			{
				Report.LanAddresses.Add(Address);
			}
		}
		Report.Address = PickLanAddress(Addresses, Request.Address.TrimStartAndEnd(), Error);
		if (Report.Address.IsEmpty())
		{
			return Refuse(Report, Error);
		}
		for (const int32 Port : {Request.HttpPort, Request.StreamerPort})
		{
			if (!IsPortFree(Port))
			{
				return Refuse(Report, FString::Printf(TEXT("Port %d is in use on this PC; pass another httpPort or streamerPort."), Port));
			}
		}
		const bool bStandalone = Request.Target == EHyperAIPhonePlaytestTarget::Standalone;
		FString Map;
		if (bStandalone)
		{
			if (!ResolveMap(Request, Map, Report.Notes, Error))
			{
				return Refuse(Report, Error);
			}
			Report.GameLog = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("HyperAIStudio"), TEXT("PhonePlaytest"), TEXT("game.log")));
			Report.GameCommandLine = GameArguments(Request, Map, Report.GameLog);
		}
		const FString StreamerId = bStandalone ? FString(StandaloneStreamerId) : Backend->PlayInEditorStreamerId();
		Report.Url = PlayerUrl(Report.Address, Request.HttpPort, StreamerId);
		Report.Exposure = ExposureText(Report.Address, Request.HttpPort, Request.StreamerPort);
		FHyperAIStudioQrCode Code;
		if (FHyperAIStudioQrCode::Encode(Report.Url, Code, Error) && Code.SavePng(QrPath(), 8, Error))
		{
			Report.QrCodePng = QrPath();
		}
		Report.Notes.Add(Request.bAllViewersControl
			? FString(TEXT("Every viewer's touches, keys and mouse reach the game's first player; a phone's gamepad becomes a player of its own when the game adds local players."))
			: FString(TEXT("The first viewer controls the game; the others watch.")));
		Report.Notes.Add(bStandalone
			? FString::Printf(TEXT("Up to %d viewers: the GPU encodes for each, and past its hardware sessions (8 on GeForce) Pixel Streaming falls back to slower software encoding; the engine's server sets no cap of its own."), Request.MaxViewers)
			: FString::Printf(TEXT("Up to %d viewers; HyperAI turns away any more. Play In Editor sessions started while this runs stream their first player's view; hyper_playtest_run's bot and bRecordVideo work as usual."), Request.MaxViewers));
		Report.Notes.Add(TEXT("Windows may ask whether UnrealEditor may accept connections: allow it on private networks only."));
		if (Request.bDryRun)
		{
			Report.bOk = true;
			Report.Status = TEXT("checked");
			Report.Diagnostic = FString::Printf(TEXT("Would stream %s to %s for up to %d viewers. %s Nothing started; resubmit with bDryRun false to ask the user."),
				bStandalone ? *FString::Printf(TEXT("the saved level %s as a game process"), *Map) : TEXT("Play In Editor sessions"), *Report.Url, Request.MaxViewers, *Report.Exposure);
			return Report;
		}
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = TEXT("pie_runtime");
		Summary.ToolName = TEXT("hyper_phone_playtest_start");
		Summary.VariantId = TEXT("phone.playtest.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
		Summary.EffectTarget = FString::Printf(TEXT("%s:%d and :%d"), *Report.Address, Request.HttpPort, Request.StreamerPort);
		Summary.Effects = {
			FString::Printf(TEXT("Start a Pixel Streaming server in this editor on ports %d and %d, listening on this PC's network interfaces, for phones on the local network at %s"),
				Request.HttpPort, Request.StreamerPort, *Report.Url),
			bStandalone ? FString::Printf(TEXT("Launch the game process: UnrealEditor.exe %s"), *Report.GameCommandLine)
				: FString(TEXT("Stream every Play In Editor session started while it runs")),
			FString::Printf(TEXT("Up to %d viewers, %s"), Request.MaxViewers, Request.bAllViewersControl ? TEXT("all of them with control") : TEXT("the first with control")),
			TEXT("Not exposed to the internet: refused on a PC with a public address; websockets only from pages on this LAN address or this PC")};
		FString ApprovalError;
		const FHyperAIPhonePlaytestReport Plan = Report;
		if (!FHyperAIStudioApprovalGate::RequestAction(Report.OperationId, Summary, [Request, Plan]()
		{
			if (Session().State == TEXT("awaiting_user_approval") && Session().Plan.OperationId == Plan.OperationId)
			{
				Begin(Request, Plan);
			}
		}, ApprovalError))
		{
			return Refuse(Report, ApprovalError, TEXT("approval_not_queued"));
		}
		FSession& Live = Session();
		Live = FSession();
		Live.State = TEXT("awaiting_user_approval");
		Live.Request = Request;
		Live.Plan = Report;
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = FString::Printf(TEXT("Waiting for the user's approval in HyperAI Chat, Activity; nothing has started. Then phones on the same Wi-Fi open %s ")
			TEXT("or scan the QR code: hyper_operation_wait with operation_id and bWaitThroughApproval true, then hyper_phone_playtest_status."), *Report.Url);
		FHyperAIStudioApprovalGate::DescribeHeld(Report.OperationId, Report.Status, Report.Diagnostic);
		return Report;
	}

	FHyperAIPhonePlaytestReport Status()
	{
		FSession& Live = Session();
		FHyperAIPhonePlaytestReport Report = Live.Plan;
		if (Live.State == TEXT("idle"))
		{
			Report.bOk = true;
			if (Report.Status != TEXT("failed"))
			{
				Report = FHyperAIPhonePlaytestReport();
				Report.bOk = true;
				Report.Status = TEXT("idle");
				Report.Diagnostic = TEXT("No phone playtest is running; hyper_phone_playtest_start starts one.");
			}
			return Report;
		}
		if (Live.State == TEXT("awaiting_user_approval") && !FHyperAIStudioApprovalGate::HasPending(Live.Plan.OperationId))
		{
			// Rejected, or approved and begun this frame.
			if (Live.State == TEXT("awaiting_user_approval"))
			{
				Live = FSession();
				Report.Status = TEXT("stopped");
				Report.Diagnostic = TEXT("The user did not approve the phone playtest; nothing started.");
				return Report;
			}
		}
		const TSharedPtr<IHyperAIStudioPixelStreamingBackend> Backend = FHyperAIStudioPixelStreamingBackends::Get();
		Report.bOk = true;
		Report.Status = Live.State;
		Report.bServerReady = Backend && Backend->IsSignallingReady();
		Report.Viewers = Backend && Live.Request.Target == EHyperAIPhonePlaytestTarget::PlayInEditor ? Backend->NumViewers() : 0;
		Report.bGameRunning = Live.Game.IsValid() && FPlatformProcess::IsProcRunning(Live.Game);
		if (Live.State == TEXT("streaming"))
		{
			Report.Diagnostic = Live.Request.Target == EHyperAIPhonePlaytestTarget::Standalone
				? (Report.bGameRunning ? FString::Printf(TEXT("Streaming the game to %s."), *Report.Url)
					: FString::Printf(TEXT("The game process has ended; the server still runs at %s. Stop, then start again to relaunch."), *Report.Url))
				: FString::Printf(TEXT("Streaming Play In Editor to %s: start a PIE session (Play, or hyper_playtest_run) and phones see it. %d viewer(s) now."),
					*Report.Url, Report.Viewers);
		}
		return Report;
	}

	FHyperAIPhonePlaytestReport Stop()
	{
		FSession& Live = Session();
		FHyperAIPhonePlaytestReport Report = Live.Plan;
		if (Live.State == TEXT("awaiting_user_approval"))
		{
			FHyperAIStudioApprovalGate::Reject(Live.Plan.OperationId, TEXT("The phone playtest was stopped before it was approved."));
		}
		if (const TSharedPtr<IHyperAIStudioPixelStreamingBackend> Backend = FHyperAIStudioPixelStreamingBackends::Get())
		{
			Backend->SetPlayInEditorStreaming(false, 0, 0, false);
			Backend->StopSignalling();
		}
		if (Live.Game.IsValid())
		{
			if (FPlatformProcess::IsProcRunning(Live.Game))
			{
				FPlatformProcess::TerminateProc(Live.Game, /*KillTree=*/true);
			}
			FPlatformProcess::CloseProc(Live.Game);
		}
		const bool bWasRunning = Live.State != TEXT("idle");
		Live = FSession();
		Report.bOk = true;
		Report.Status = TEXT("stopped");
		Report.bServerReady = false;
		Report.bGameRunning = false;
		Report.Viewers = 0;
		Report.Diagnostic = bWasRunning ? TEXT("Stopped: the server is closed and nothing streams.") : TEXT("No phone playtest was running.");
		return Report;
	}
}
