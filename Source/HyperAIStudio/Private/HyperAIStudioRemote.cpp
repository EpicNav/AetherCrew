// Games by Hyper 2026.

#include "HyperAIStudioRemote.h"

#include "CoreGlobals.h"
#include "Dialog/SCustomDialog.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAutoApproval.h"
#include "HyperAIStudioEditorRestart.h"
#include "HyperAIStudioNativeChat.h"
#include "HyperAIStudioSettings.h"
#include "IHttpRouter.h"
#include "IPAddress.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "SHyperAIStudioQuickActionWindow.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioRemote"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioRemote, Log, All);

namespace HyperAIStudio::Remote::Private
{
	constexpr double CodeSeconds = 600.0;
	constexpr int32 MaxWrongCodes = 5;
	/** The newest items of each chat the page shows. */
	constexpr int32 MaxItems = 40;
	constexpr int32 MaxItemCharacters = 4000;
	constexpr const TCHAR* CookieName = TEXT("hyperai_remote");
	/**
	 * The page sends it with every call. A page on another site cannot add a header to a request here without the
	 * browser asking this server first, which it never agrees to.
	 */
	constexpr const TCHAR* CallHeader = TEXT("X-HyperAI");
	constexpr const TCHAR* ListenersSection = TEXT("HTTPServer.Listeners");
	constexpr const TCHAR* ListenersKey = TEXT("ListenerOverrides");

	struct FState
	{
		TSharedPtr<IHttpRouter> Router;
		TArray<FHttpRouteHandle> Routes;
		uint32 Port = 0;
		/** Whether the listener took every address or this computer's only; fixed once it binds. */
		bool bOnNetwork = false;
		/** What a paired browser holds; empty until the first one pairs. */
		FString Key;
		FString Code;
		double CodeUntil = 0.0;
		int32 WrongCodes = 0;
		FString Page;
	};

	FState& State()
	{
		static FState Instance;
		return Instance;
	}

	FString Header(const FHttpServerRequest& Request, const TCHAR* Name)
	{
		for (const TPair<FString, TArray<FString>>& Pair : Request.Headers)
		{
			if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase))
			{
				return FString::Join(Pair.Value, TEXT(";"));
			}
		}
		return FString();
	}

	FString CookieValue(const FString& Cookies)
	{
		TArray<FString> Parts;
		Cookies.ParseIntoArray(Parts, TEXT(";"));
		for (const FString& Part : Parts)
		{
			FString Name;
			FString Value;
			if (Part.Split(TEXT("="), &Name, &Value) && Name.TrimStartAndEnd() == CookieName)
			{
				return Value.TrimStartAndEnd();
			}
		}
		return FString();
	}

	/**
	 * Only names that mean this computer: a site that points its own name at this address (DNS rebinding) is turned
	 * away, since the browser would send its name.
	 */
	bool IsOwnHost(FString Host)
	{
		if (Host.StartsWith(TEXT("[")))
		{
			int32 Close = INDEX_NONE;
			Host = Host.FindChar(TEXT(']'), Close) ? Host.Mid(1, Close - 1) : FString();
		}
		else
		{
			int32 Colon = INDEX_NONE;
			if (Host.FindLastChar(TEXT(':'), Colon))
			{
				Host.LeftInline(Colon);
			}
		}
		if (Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase) || Host == TEXT("::1")
			|| Host.Equals(FPlatformProcess::ComputerName(), ESearchCase::IgnoreCase))
		{
			return true;
		}
		TArray<FString> Parts;
		Host.ParseIntoArray(Parts, TEXT("."), /*InCullEmpty=*/false);
		return Parts.Num() == 4 && !Parts.ContainsByPredicate([](const FString& Part) { return Part.IsEmpty() || Part.Len() > 3 || !Part.IsNumeric(); });
	}

	TSharedPtr<FJsonObject> Body(const FHttpServerRequest& Request)
	{
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		TSharedPtr<FJsonObject> Json;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get())), Json);
		return Json.IsValid() ? Json : MakeShared<FJsonObject>();
	}

	void Reply(const FHttpResultCallback& OnComplete, const EHttpServerResponseCodes Code, const TSharedRef<FJsonObject>& Json, const FString& Cookie = FString())
	{
		FString Text;
		FJsonSerializer::Serialize(Json, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Text, TEXT("application/json"));
		Response->Code = Code;
		Response->Headers.Add(TEXT("Cache-Control"), {TEXT("no-store")});
		if (!Cookie.IsEmpty())
		{
			Response->Headers.Add(TEXT("Set-Cookie"), {Cookie});
		}
		OnComplete(MoveTemp(Response));
	}

	void Fail(const FHttpResultCallback& OnComplete, const EHttpServerResponseCodes Code, const FString& Error)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("error"), Error);
		Reply(OnComplete, Code, Json);
	}

	void Done(const FHttpResultCallback& OnComplete, const bool bOk, const FString& Message)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("ok"), bOk);
		Json->SetStringField(TEXT("message"), Message);
		Reply(OnComplete, EHttpServerResponseCodes::Ok, Json);
	}

	/** Why a call is turned away; empty when it may go on. The page itself needs only an own host name. */
	FString Refusal(const FHttpServerRequest& Request, const bool bCall, const bool bPaired)
	{
		if (!IsOwnHost(Header(Request, TEXT("Host"))))
		{
			return TEXT("unknown_host");
		}
		if (bCall && Header(Request, CallHeader).IsEmpty())
		{
			return TEXT("missing_header");
		}
		if (bPaired && (State().Key.IsEmpty() || CookieValue(Header(Request, TEXT("Cookie"))) != State().Key))
		{
			return TEXT("not_paired");
		}
		return FString();
	}

	const TCHAR* KindName(const FHyperAIStudioChatItem::EKind Kind)
	{
		switch (Kind)
		{
		case FHyperAIStudioChatItem::EKind::User: return TEXT("user");
		case FHyperAIStudioChatItem::EKind::Agent: return TEXT("agent");
		case FHyperAIStudioChatItem::EKind::Reasoning: return TEXT("reasoning");
		case FHyperAIStudioChatItem::EKind::Tool: return TEXT("tool");
		case FHyperAIStudioChatItem::EKind::Command: return TEXT("command");
		case FHyperAIStudioChatItem::EKind::FileChange: return TEXT("file_change");
		case FHyperAIStudioChatItem::EKind::Approval: return TEXT("question");
		case FHyperAIStudioChatItem::EKind::Error: return TEXT("error");
		case FHyperAIStudioChatItem::EKind::Image: return TEXT("image");
		default: return TEXT("notice");
		}
	}

	const TCHAR* SafetyName(const EHyperAIStudioDomainSafety Safety)
	{
		switch (Safety)
		{
		case EHyperAIStudioDomainSafety::Read: return TEXT("read");
		case EHyperAIStudioDomainSafety::Destructive: return TEXT("destructive");
		case EHyperAIStudioDomainSafety::ExternalEffect: return TEXT("external_effect");
		default: return TEXT("edit");
		}
	}

	TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value.Left(MaxItemCharacters)));
		}
		return Out;
	}

	/** What the page shows: every chat with an agent running, its newest items, and the edits waiting for approval. */
	TSharedRef<FJsonObject> Snapshot()
	{
		TArray<TSharedPtr<FJsonValue>> Chats;
		for (const FHyperAIStudioRestartChat& Tab : SHyperAIStudioQuickActionWindow::GetRestartChats())
		{
			const TSharedRef<FJsonObject> Chat = MakeShared<FJsonObject>();
			Chat->SetNumberField(TEXT("tab"), Tab.TabIndex);
			Chat->SetStringField(TEXT("agent"), Tab.Agent);
			Chat->SetStringField(TEXT("model"), Tab.Model);
			Chat->SetStringField(TEXT("title"), Tab.Title);
			Chat->SetBoolField(TEXT("working"), Tab.bWorking);
			TArray<TSharedPtr<FJsonValue>> Items;
			const TSharedPtr<FHyperAIStudioNativeChat> Native = SHyperAIStudioQuickActionWindow::GetTabChat(Tab.TabIndex);
			Chat->SetBoolField(TEXT("native"), Native.IsValid());
			if (Native.IsValid())
			{
				const TArray<TSharedPtr<FHyperAIStudioChatItem>>& All = Native->GetItems();
				for (int32 Index = FMath::Max(0, All.Num() - MaxItems); Index < All.Num(); ++Index)
				{
					const FHyperAIStudioChatItem& Source = *All[Index];
					const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("id"), Source.Id);
					Item->SetStringField(TEXT("kind"), KindName(Source.Kind));
					Item->SetStringField(TEXT("title"), Source.Title.Left(MaxItemCharacters));
					// The newest part of a long text is the part that matters on a phone.
					Item->SetStringField(TEXT("text"), Source.Text.Right(MaxItemCharacters));
					Item->SetStringField(TEXT("status"), Source.Status);
					if (Source.Kind == FHyperAIStudioChatItem::EKind::Approval && !Source.bAnswered)
					{
						Item->SetArrayField(TEXT("choices"), Strings(Source.Choices));
					}
					Items.Add(MakeShared<FJsonValueObject>(Item));
				}
				Chat->SetBoolField(TEXT("busy"), Native->IsBusy());
			}
			Chat->SetArrayField(TEXT("items"), Items);
			Chats.Add(MakeShared<FJsonValueObject>(Chat));
		}
		TArray<TSharedPtr<FJsonValue>> Approvals;
		for (const FHyperAIStudioPendingApproval& Pending : FHyperAIStudioApprovalGate::GetPending())
		{
			const TSharedRef<FJsonObject> Approval = MakeShared<FJsonObject>();
			Approval->SetStringField(TEXT("id"), Pending.OperationId);
			Approval->SetStringField(TEXT("tool"), Pending.Summary.ToolName);
			Approval->SetStringField(TEXT("target"), Pending.Summary.EffectTarget);
			Approval->SetStringField(TEXT("safety"), SafetyName(Pending.Summary.Safety));
			Approval->SetArrayField(TEXT("effects"), Strings(Pending.Summary.Effects));
			Approval->SetArrayField(TEXT("touches"), Strings(Pending.Summary.Touches));
			Approval->SetStringField(TEXT("requestedAt"), Pending.RequestedUtc.ToIso8601());
			Approval->SetBoolField(TEXT("autoApproving"), Pending.bAutoApproving);
			Approvals.Add(MakeShared<FJsonValueObject>(Approval));
		}
		// The page shows auto-approval and can stop it; nothing here turns it on.
		const FHyperAIStudioAutoApprovalStatus Auto = FHyperAIStudioAutoApproval::GetStatus();
		const TSharedRef<FJsonObject> AutoApproval = MakeShared<FJsonObject>();
		AutoApproval->SetBoolField(TEXT("on"), Auto.bActive);
		TArray<TSharedPtr<FJsonValue>> Categories;
		for (const EHyperAIStudioApprovalCategory Category : Auto.Categories)
		{
			Categories.Add(MakeShared<FJsonValueString>(::LexToString(Category)));
		}
		AutoApproval->SetArrayField(TEXT("categories"), Categories);
		AutoApproval->SetNumberField(TEXT("secondsLeft"), FMath::FloorToDouble(Auto.SecondsLeft));
		AutoApproval->SetNumberField(TEXT("approved"), Auto.Approved);
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("project"), FApp::GetProjectName());
		Root->SetArrayField(TEXT("chats"), Chats);
		Root->SetArrayField(TEXT("approvals"), Approvals);
		Root->SetObjectField(TEXT("autoApproval"), AutoApproval);
		return Root;
	}

	void Pair(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
	{
		FState& Current = State();
		FString Code;
		Body(Request)->TryGetStringField(TEXT("code"), Code);
		const bool bOffered = !Current.Code.IsEmpty() && FPlatformTime::Seconds() < Current.CodeUntil;
		if (!bOffered || Code.TrimStartAndEnd() != Current.Code)
		{
			if (bOffered && ++Current.WrongCodes >= MaxWrongCodes)
			{
				// Only the editor offers another: guessing cannot go on.
				Current.Code.Reset();
				UE_LOG(LogHyperAIStudioRemote, Warning, TEXT("Five wrong pairing codes: the remote page takes none until New Code in the editor."));
			}
			Fail(OnComplete, EHttpServerResponseCodes::Forbidden, bOffered ? TEXT("wrong_code") : TEXT("no_code"));
			return;
		}
		Current.Code.Reset();
		if (Current.Key.IsEmpty())
		{
			Current.Key = (FGuid::NewGuid().ToString(EGuidFormats::Digits) + FGuid::NewGuid().ToString(EGuidFormats::Digits)).ToLower();
		}
		UE_LOG(LogHyperAIStudioRemote, Display, TEXT("A browser paired with the remote page."));
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("ok"), true);
		// Strict: the browser sends it only from this page, never with a request another site starts.
		Reply(OnComplete, EHttpServerResponseCodes::Ok, Json,
			FString::Printf(TEXT("%s=%s; Path=/remote; HttpOnly; SameSite=Strict; Max-Age=2592000"), CookieName, *Current.Key));
	}

	int32 Tab(const TSharedPtr<FJsonObject>& Json)
	{
		double Value = 0.0;
		Json->TryGetNumberField(TEXT("tab"), Value);
		return static_cast<int32>(Value);
	}

	/** A paired browser's action, on the game thread where the HTTP server calls this. */
	void Act(const FString& Action, const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
	{
		const TSharedPtr<FJsonObject> Json = Body(Request);
		FString Id;
		Json->TryGetStringField(TEXT("id"), Id);
		FString Message;
		if (Action == TEXT("approve"))
		{
			const bool bOk = FHyperAIStudioApprovalGate::HasPending(Id) && FHyperAIStudioApprovalGate::Approve(Id, Message);
			Done(OnComplete, bOk, bOk ? FString(TEXT("Approved.")) : Message.IsEmpty() ? FString(TEXT("Nothing waits under that id.")) : Message);
		}
		else if (Action == TEXT("reject"))
		{
			const bool bOk = FHyperAIStudioApprovalGate::HasPending(Id);
			if (bOk)
			{
				FHyperAIStudioApprovalGate::Reject(Id, TEXT("Rejected on the remote page."));
			}
			Done(OnComplete, bOk, bOk ? TEXT("Rejected.") : TEXT("Nothing waits under that id."));
		}
		else if (Action == TEXT("answer"))
		{
			double Choice = -1.0;
			Json->TryGetNumberField(TEXT("choice"), Choice);
			const TSharedPtr<FHyperAIStudioNativeChat> Chat = SHyperAIStudioQuickActionWindow::GetTabChat(Tab(Json));
			const TSharedPtr<FHyperAIStudioChatItem>* Item = Chat.IsValid() ? Chat->GetItems().FindByPredicate([&Id](const TSharedPtr<FHyperAIStudioChatItem>& Entry)
			{
				return Entry->Id == Id && Entry->Kind == FHyperAIStudioChatItem::EKind::Approval && !Entry->bAnswered;
			}) : nullptr;
			const bool bOk = Item && Choice >= 0.0 && Choice < (*Item)->Choices.Num();
			if (bOk)
			{
				Chat->Answer(Id, static_cast<int32>(Choice));
			}
			Done(OnComplete, bOk, bOk ? TEXT("Answered.") : TEXT("That question is no longer open."));
		}
		else if (Action == TEXT("send"))
		{
			FString Text;
			Json->TryGetStringField(TEXT("text"), Text);
			const bool bOk = SHyperAIStudioQuickActionWindow::SendToChat(Tab(Json), Text, Message);
			Done(OnComplete, bOk, Message);
		}
		else if (Action == TEXT("stop"))
		{
			SHyperAIStudioQuickActionWindow::InterruptTab(Tab(Json));
			Done(OnComplete, true, TEXT("Asked to stop."));
		}
		else if (Action == TEXT("stop_auto_approval"))
		{
			FHyperAIStudioAutoApproval::Stop(EHyperAIStudioAutoApprovalEnd::StoppedRemotely);
			Done(OnComplete, true, TEXT("Auto-approval is off."));
		}
		else
		{
			Fail(OnComplete, EHttpServerResponseCodes::NotFound, TEXT("unknown_action"));
		}
	}

	/**
	 * Unreal's server reads a listener's address once, as it binds: this computer's only, or every address when other
	 * devices may open the page. The override lives in the config just for that moment, so none of it is saved.
	 */
	TSharedPtr<IHttpRouter> Bind(const uint32 Port, const bool bOnNetwork)
	{
		TArray<FString> Before;
		const bool bHadOverrides = GConfig->GetArray(ListenersSection, ListenersKey, Before, GEngineIni) > 0;
		TArray<FString> Overrides = Before;
		Overrides.RemoveAll([Port](const FString& Entry)
		{
			uint32 EntryPort = 0;
			return FParse::Value(*Entry.Replace(TEXT("("), TEXT("")), TEXT("Port="), EntryPort) && EntryPort == Port;
		});
		Overrides.Add(FString::Printf(TEXT("(Port=%u, BindAddress=%s)"), Port, bOnNetwork ? TEXT("any") : TEXT("localhost")));
		GConfig->SetArray(ListenersSection, ListenersKey, Overrides, GEngineIni);
		FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, {ListenersSection});
		TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
		if (bHadOverrides)
		{
			GConfig->SetArray(ListenersSection, ListenersKey, Before, GEngineIni);
		}
		else
		{
			GConfig->RemoveKey(ListenersSection, ListenersKey, GEngineIni);
		}
		FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, {ListenersSection});
		return Router;
	}
}

bool FHyperAIStudioRemote::Start(FString& OutError)
{
	using namespace HyperAIStudio::Remote::Private;
	FState& Current = State();
	if (IsRunning())
	{
		return true;
	}
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
	if (!Plugin.IsValid() || !FFileHelper::LoadFileToString(Current.Page, *(Plugin->GetBaseDir() / TEXT("Resources/Remote/index.html"))))
	{
		OutError = TEXT("The remote page is missing from the plugin: Resources/Remote/index.html.");
		return false;
	}
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	if (!Current.Router.IsValid())
	{
		Current.Router = Bind(Settings->RemotePagePort, Settings->bRemotePageOnNetwork);
		if (!Current.Router.IsValid())
		{
			OutError = FString::Printf(TEXT("Port %d is taken by another program; choose another Remote Page Port in the settings."), Settings->RemotePagePort);
			return false;
		}
		Current.Port = Settings->RemotePagePort;
		Current.bOnNetwork = Settings->bRemotePageOnNetwork;
	}
	const TSharedRef<IHttpRouter> Router = Current.Router.ToSharedRef();
	Current.Routes.Add(Router->BindRoute(FHttpPath(TEXT("/remote")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			const FString Refused = Refusal(Request, /*bCall=*/false, /*bPaired=*/false);
			if (!Refused.IsEmpty())
			{
				Fail(OnComplete, EHttpServerResponseCodes::Forbidden, Refused);
				return true;
			}
			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(State().Page, TEXT("text/html; charset=utf-8"));
			Response->Headers.Add(TEXT("Cache-Control"), {TEXT("no-store")});
			// The page runs its own script and nothing from elsewhere, and no other site may frame it.
			Response->Headers.Add(TEXT("Content-Security-Policy"), {TEXT("default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'")});
			OnComplete(MoveTemp(Response));
			return true;
		})));
	Current.Routes.Add(Router->BindRoute(FHttpPath(TEXT("/remote/api/pair")), EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			const FString Refused = Refusal(Request, /*bCall=*/true, /*bPaired=*/false);
			Refused.IsEmpty() ? Pair(Request, OnComplete) : Fail(OnComplete, EHttpServerResponseCodes::Forbidden, Refused);
			return true;
		})));
	Current.Routes.Add(Router->BindRoute(FHttpPath(TEXT("/remote/api/state")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			const FString Refused = Refusal(Request, /*bCall=*/true, /*bPaired=*/true);
			Refused.IsEmpty() ? Reply(OnComplete, EHttpServerResponseCodes::Ok, Snapshot())
				: Fail(OnComplete, Refused == TEXT("not_paired") ? EHttpServerResponseCodes::Denied : EHttpServerResponseCodes::Forbidden, Refused);
			return true;
		})));
	for (const TCHAR* Action : {TEXT("approve"), TEXT("reject"), TEXT("answer"), TEXT("send"), TEXT("stop"), TEXT("stop_auto_approval")})
	{
		const FString Name = Action;
		Current.Routes.Add(Router->BindRoute(FHttpPath(TEXT("/remote/api/") + Name), EHttpServerRequestVerbs::VERB_POST,
			FHttpRequestHandler::CreateLambda([Name](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				const FString Refused = Refusal(Request, /*bCall=*/true, /*bPaired=*/true);
				Refused.IsEmpty() ? Act(Name, Request, OnComplete)
					: Fail(OnComplete, Refused == TEXT("not_paired") ? EHttpServerResponseCodes::Denied : EHttpServerResponseCodes::Forbidden, Refused);
				return true;
			})));
	}
	FHttpServerModule::Get().StartAllListeners();
	UE_LOG(LogHyperAIStudioRemote, Display, TEXT("The remote page is up on port %u, for %s."), Current.Port,
		Current.bOnNetwork ? TEXT("this computer and this network") : TEXT("this computer only"));
	return true;
}

void FHyperAIStudioRemote::Stop()
{
	using namespace HyperAIStudio::Remote::Private;
	FState& Current = State();
	if (Current.Router.IsValid())
	{
		for (const FHttpRouteHandle& Route : Current.Routes)
		{
			Current.Router->UnbindRoute(Route);
		}
	}
	Current.Routes.Reset();
	Current.Key.Reset();
	Current.Code.Reset();
}

bool FHyperAIStudioRemote::IsRunning()
{
	return !HyperAIStudio::Remote::Private::State().Routes.IsEmpty();
}

FString FHyperAIStudioRemote::NewCode()
{
	using namespace HyperAIStudio::Remote::Private;
	FState& Current = State();
	// A GUID's bits come from the system's random source.
	Current.Code = FString::Printf(TEXT("%06u"), FGuid::NewGuid().A % 1000000u);
	Current.CodeUntil = FPlatformTime::Seconds() + CodeSeconds;
	Current.WrongCodes = 0;
	return Current.Code;
}

FString FHyperAIStudioRemote::GetCode()
{
	using namespace HyperAIStudio::Remote::Private;
	return FPlatformTime::Seconds() < State().CodeUntil ? State().Code : FString();
}

void FHyperAIStudioRemote::ForgetBrowsers()
{
	HyperAIStudio::Remote::Private::State().Key.Reset();
}

TArray<FString> FHyperAIStudioRemote::GetAddresses()
{
	using namespace HyperAIStudio::Remote::Private;
	const FState& Current = State();
	const uint32 Port = Current.Port ? Current.Port : static_cast<uint32>(GetDefault<UHyperAIStudioSettings>()->RemotePagePort);
	TArray<FString> Addresses = {FString::Printf(TEXT("http://localhost:%u/remote"), Port)};
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TArray<TSharedPtr<FInternetAddr>> Adapters;
	if (Current.bOnNetwork && Sockets && Sockets->GetLocalAdapterAddresses(Adapters))
	{
		for (const TSharedPtr<FInternetAddr>& Adapter : Adapters)
		{
			const FString Ip = Adapter.IsValid() && Adapter->GetProtocolType() == FNetworkProtocolTypes::IPv4 ? Adapter->ToString(false) : FString();
			if (!Ip.IsEmpty() && !Ip.StartsWith(TEXT("127.")) && !Ip.StartsWith(TEXT("169.254.")))
			{
				Addresses.AddUnique(FString::Printf(TEXT("http://%s:%u/remote"), *Ip, Port));
			}
		}
	}
	return Addresses;
}

void FHyperAIStudioRemote::Startup()
{
	if (!GetDefault<UHyperAIStudioSettings>()->bRemotePage || IsRunningCommandlet() || FApp::IsUnattended())
	{
		return;
	}
	FString Error;
	if (!Start(Error))
	{
		UE_LOG(LogHyperAIStudioRemote, Warning, TEXT("The remote page did not start: %s"), *Error);
	}
}

void FHyperAIStudioRemote::Shutdown()
{
	Stop();
	HyperAIStudio::Remote::Private::State().Router.Reset();
}

void FHyperAIStudioRemote::OpenDialog()
{
	using namespace HyperAIStudio::Remote::Private;
	auto SetPage = [](const bool bOn)
	{
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		Settings->bRemotePage = bOn;
		Settings->SaveConfig();
		FString Error;
		if (!bOn)
		{
			Stop();
		}
		else if (Start(Error))
		{
			NewCode();
		}
		else
		{
			UE_LOG(LogHyperAIStudioRemote, Warning, TEXT("The remote page did not start: %s"), *Error);
		}
	};
	if (IsRunning() && GetCode().IsEmpty())
	{
		NewCode();
	}
	const TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(STextBlock).AutoWrapText(true)
			.Text(LOCTEXT("Intro", "Follow the chats, answer their questions and approve edits from a browser: on this computer, or on your phone when other devices may open the page."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([]() { return IsRunning() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([SetPage](const ECheckBoxState State) { SetPage(State == ECheckBoxState::Checked); })
			[
				SNew(STextBlock).Text(LOCTEXT("Serve", "Serve the remote page"))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([]() { return GetDefault<UHyperAIStudioSettings>()->bRemotePageOnNetwork ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([](const ECheckBoxState State)
			{
				UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
				Settings->bRemotePageOnNetwork = State == ECheckBoxState::Checked;
				Settings->SaveConfig();
			})
			[
				SNew(STextBlock).Text(LOCTEXT("Network", "Other devices on this network can open it"))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Visibility_Lambda([]()
			{
				return GetDefault<UHyperAIStudioSettings>()->bRemotePageOnNetwork != State().bOnNetwork && State().Router.IsValid()
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			.Text(LOCTEXT("NetworkRestart", "The page keeps the addresses it started with until the editor restarts."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(STextBlock).AutoWrapText(true)
			.Visibility_Lambda([]() { return IsRunning() ? EVisibility::Visible : EVisibility::Collapsed; })
			.Text_Lambda([]() { return FText::Format(LOCTEXT("Addresses", "Open:\n{0}"), FText::FromString(FString::Join(GetAddresses(), TEXT("\n")))); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(STextBlock)
			.Visibility_Lambda([]() { return IsRunning() ? EVisibility::Visible : EVisibility::Collapsed; })
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			.Text_Lambda([]()
			{
				const FString Code = GetCode();
				return Code.IsEmpty() ? LOCTEXT("NoCode", "No pairing code on offer. New Code makes one.")
					: FText::Format(LOCTEXT("Code", "Pairing code: {0}  (ten minutes, one browser)"), FText::FromString(Code));
			})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(SHorizontalBox)
			.Visibility_Lambda([]() { return IsRunning() ? EVisibility::Visible : EVisibility::Collapsed; })
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SButton).Text(LOCTEXT("NewCode", "New Code")).OnClicked_Lambda([]() { NewCode(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SButton).Text(LOCTEXT("CopyLink", "Copy Link")).OnClicked_Lambda([]()
				{
					const TArray<FString> Addresses = GetAddresses();
					FPlatformApplicationMisc::ClipboardCopy(*Addresses.Last());
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).Text(LOCTEXT("Forget", "Forget Paired Browsers")).OnClicked_Lambda([]() { ForgetBrowsers(); return FReply::Handled(); })
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("Warning", "A paired browser can approve edits. The page is not encrypted, so let other devices open it only on a network you trust; away from it, reach this computer through a VPN such as Tailscale. Browsers pair again after the editor restarts."))
		];
	const TSharedRef<SCustomDialog> Dialog = SNew(SCustomDialog)
		.Title(LOCTEXT("Title", "Open On Another Device"))
		.Content()
		[
			SNew(SBox).WidthOverride(560.0f)[ Body ]
		]
		.Buttons({SCustomDialog::FButton(LOCTEXT("Close", "Close"))});
	// Not modal: the editor, and the page's server with it, keeps running while a browser pairs.
	Dialog->Show();
}

#undef LOCTEXT_NAMESPACE
