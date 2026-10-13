// Games by Hyper 2026.

#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
#include "HyperAIStudioPixelStreamingBackend.h"
#include "Interfaces/IPluginManager.h"
#include "IPixelStreaming2EditorModule.h"
#include "IPixelStreaming2Module.h"
#include "IPixelStreaming2Streamer.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PixelStreaming2Delegates.h"
#include "PixelStreaming2Servers.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPixelStreaming, Log, All);

namespace
{
	void SetConsoleVariable(const TCHAR* Name, const FString& Value)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Variable->Set(*Value, ECVF_SetByCode);
		}
		else
		{
			UE_LOG(LogHyperAIStudioPixelStreaming, Warning, TEXT("Pixel Streaming 2 has no console variable %s in this engine."), Name);
		}
	}

	class FBackend final : public IHyperAIStudioPixelStreamingBackend
	{
	public:
		virtual ~FBackend() override
		{
			SetPlayInEditorStreaming(false, 0, 0, false);
			StopSignalling();
		}

		virtual bool StartSignalling(const int32 HttpPort, const int32 StreamerPort, const FString& AllowedOrigins, FString& OutError) override
		{
			StopSignalling();
			Server = UE::PixelStreaming2Servers::MakeSignallingServer();
			if (!Server)
			{
				OutError = TEXT("Pixel Streaming 2 made no signalling server.");
				return false;
			}
			bReady = false;
			Server->OnReady.AddLambda([this](const UE::PixelStreaming2Servers::FEndpoints&) { bReady = true; });
			UE::PixelStreaming2Servers::FLaunchArgs Args;
			// The plugin's own server in this process; IPixelStreaming2EditorModule::StartSignalling is not used, because it
			// runs get_ps_servers first, which downloads from GitHub.
			Args.ProcessArgs = FString::Printf(TEXT("--HttpPort=%d --StreamerPort=%d --ServeHttps=false --EnableIPv6=false --AllowedOrigins=%s"),
				HttpPort, StreamerPort, *AllowedOrigins);
			if (!Server->Launch(Args))
			{
				Server.Reset();
				OutError = FString::Printf(TEXT("The signalling server could not listen on ports %d and %d."), HttpPort, StreamerPort);
				return false;
			}
			UE_LOG(LogHyperAIStudioPixelStreaming, Log, TEXT("Signalling server on ports %d (viewers) and %d (streamer); origins %s"), HttpPort, StreamerPort, *AllowedOrigins);
			return true;
		}

		virtual void StopSignalling() override
		{
			if (Server)
			{
				Server->Stop();
				Server.Reset();
			}
			bReady = false;
		}

		virtual bool IsSignallingReady() const override
		{
			return Server && (bReady || Server->IsReady());
		}

		virtual FString PlayerPageFolder() const override
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PixelStreaming2"));
			return Plugin ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("WebServers"), TEXT("SignallingWebServer"), TEXT("www")) : FString();
		}

		virtual void SetPlayInEditorStreaming(const bool bOn, const int32 StreamerPort, const int32 InMaxViewers, const bool bAllViewersControl) override
		{
			MaxViewers = InMaxViewers;
			// The PIE streamer connects to HyperAI's server and never starts the editor module's own, which would download.
			SetConsoleVariable(TEXT("PixelStreaming2.Editor.UseRemoteSignallingServer"), TEXT("true"));
			SetConsoleVariable(TEXT("PixelStreaming2.Editor.AutoStreamPIE"), bOn ? TEXT("true") : TEXT("false"));
			if (bOn)
			{
				SetConsoleVariable(TEXT("PixelStreaming2.InputController"), bAllViewersControl ? TEXT("Any") : TEXT("Host"));
				SetConsoleVariable(TEXT("PixelStreaming2.Encoder.MaxSessions"), FString::FromInt(InMaxViewers));
				if (IPixelStreaming2EditorModule::IsAvailable())
				{
					IPixelStreaming2EditorModule::Get().SetSignallingDomain(TEXT("ws://127.0.0.1"));
					IPixelStreaming2EditorModule::Get().SetStreamerPort(StreamerPort);
				}
				if (!NewConnection.IsValid())
				{
					if (UPixelStreaming2Delegates* Delegates = UPixelStreaming2Delegates::Get())
					{
						// Viewers past the cap are turned away as they connect.
						NewConnection = Delegates->OnNewConnectionNative.AddLambda([this](FString StreamerId, FString PeerId)
						{
							AsyncTask(ENamedThreads::GameThread, [this, StreamerId, PeerId]()
							{
								const TSharedPtr<IPixelStreaming2Streamer> Streamer = IPixelStreaming2Module::Get().FindStreamer(StreamerId);
								if (Streamer && Streamer->GetConnectedPlayers().Num() > MaxViewers)
								{
									UE_LOG(LogHyperAIStudioPixelStreaming, Log, TEXT("Turned away viewer %s: %d watch already."), *PeerId, MaxViewers);
									Streamer->KickPlayer(PeerId);
								}
							});
						});
					}
				}
			}
			else if (NewConnection.IsValid())
			{
				if (UPixelStreaming2Delegates* Delegates = UPixelStreaming2Delegates::Get())
				{
					Delegates->OnNewConnectionNative.Remove(NewConnection);
				}
				NewConnection.Reset();
			}
		}

		virtual FString PlayInEditorStreamerId() const override
		{
			return IPixelStreaming2Module::IsAvailable() ? IPixelStreaming2Module::Get().GetDefaultStreamerID() : FString(TEXT("DefaultStreamer"));
		}

		virtual int32 NumViewers() const override
		{
			if (!IPixelStreaming2Module::IsAvailable())
			{
				return 0;
			}
			const TSharedPtr<IPixelStreaming2Streamer> Streamer = IPixelStreaming2Module::Get().FindStreamer(PlayInEditorStreamerId());
			return Streamer ? Streamer->GetConnectedPlayers().Num() : 0;
		}

	private:
		TSharedPtr<UE::PixelStreaming2Servers::IServer> Server;
		bool bReady = false;
		int32 MaxViewers = 8;
		FDelegateHandle NewConnection;
	};
}

class FHyperAIStudioPixelStreamingModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Until a phone playtest starts, PIE is not streamed, and the plugin's editor module never starts its own server
		// (which runs get_ps_servers, downloading from GitHub, every time).
		SetConsoleVariable(TEXT("PixelStreaming2.Editor.UseRemoteSignallingServer"), TEXT("true"));
		SetConsoleVariable(TEXT("PixelStreaming2.Editor.AutoStreamPIE"), TEXT("false"));
		Backend = MakeShared<FBackend>();
		FHyperAIStudioPixelStreamingBackends::Register(Backend);
	}

	virtual void ShutdownModule() override
	{
		FHyperAIStudioPixelStreamingBackends::Unregister();
		Backend.Reset();
	}

private:
	TSharedPtr<FBackend> Backend;
};

IMPLEMENT_MODULE(FHyperAIStudioPixelStreamingModule, HyperAIStudioPixelStreaming)
