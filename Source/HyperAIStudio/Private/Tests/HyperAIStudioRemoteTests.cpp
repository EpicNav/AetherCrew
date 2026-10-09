// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAutoApproval.h"
#include "HyperAIStudioRemote.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace HyperAIStudio::RemoteTests
{
	struct FAnswer
	{
		int32 Code = 0;
		FString Body;
		FString SetCookie;
	};

	/** One request to the page, as a browser (or someone else) sends it, answered before this returns. */
	FAnswer Call(const uint32 Port, const TCHAR* Verb, const FString& Path, const FString& Body = FString(),
		const TMap<FString, FString>& Headers = {})
	{
		const TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(FString::Printf(TEXT("http://127.0.0.1:%u%s"), Port, *Path));
		Request->SetVerb(Verb);
		Request->SetTimeout(10.0f);
		for (const TPair<FString, FString>& Header : Headers)
		{
			Request->SetHeader(Header.Key, Header.Value);
		}
		if (!Body.IsEmpty())
		{
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Request->SetContentAsString(Body);
		}
		const TSharedRef<FAnswer> Answer = MakeShared<FAnswer>();
		const TSharedRef<bool> bDone = MakeShared<bool>(false);
		Request->OnProcessRequestComplete().BindLambda([Answer, bDone](FHttpRequestPtr, FHttpResponsePtr Response, bool)
		{
			if (Response.IsValid())
			{
				Answer->Code = Response->GetResponseCode();
				Answer->Body = Response->GetContentAsString();
				Answer->SetCookie = Response->GetHeader(TEXT("Set-Cookie"));
			}
			*bDone = true;
		});
		Request->ProcessRequest();
		const double End = FPlatformTime::Seconds() + 10.0;
		while (!*bDone && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.01f);
			FPlatformProcess::Sleep(0.005f);
		}
		return *Answer;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioRemotePageTest,
	"HyperAIStudio.Remote.Page",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioRemotePageTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::RemoteTests;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const int32 SavedPort = Settings->RemotePagePort;
	const bool bSavedNetwork = Settings->bRemotePageOnNetwork;
	constexpr uint32 Port = 18341;
	Settings->RemotePagePort = Port;
	Settings->bRemotePageOnNetwork = false;
	ON_SCOPE_EXIT
	{
		FHyperAIStudioRemote::Stop();
		Settings->RemotePagePort = SavedPort;
		Settings->bRemotePageOnNetwork = bSavedNetwork;
	};
	FString Error;
	if (!TestTrue(TEXT("The page starts: ") + Error, FHyperAIStudioRemote::Start(Error)))
	{
		return false;
	}
	TestTrue(TEXT("It opens on this computer"), FHyperAIStudioRemote::GetAddresses() == TArray<FString>{FString::Printf(TEXT("http://localhost:%u/remote"), Port)});

	const TMap<FString, FString> Page = {{TEXT("X-HyperAI"), TEXT("1")}};
	const FAnswer Html = Call(Port, TEXT("GET"), TEXT("/remote"));
	TestTrue(TEXT("The page itself needs no pairing"), Html.Code == 200 && Html.Body.Contains(TEXT("HyperAI Studio Remote")));
	TestEqual(TEXT("A site that points its own name here is turned away"),
		Call(Port, TEXT("GET"), TEXT("/remote"), FString(), {{TEXT("Host"), FString::Printf(TEXT("evil.example:%u"), Port)}}).Code, 403);
	TestEqual(TEXT("A call without the page's header is refused, so other sites cannot post here"),
		Call(Port, TEXT("GET"), TEXT("/remote/api/state")).Code, 403);
	TestEqual(TEXT("An unpaired browser sees nothing"), Call(Port, TEXT("GET"), TEXT("/remote/api/state"), FString(), Page).Code, 401);

	// Pairing: no code until the editor offers one, five wrong tries void it, and the right one pairs one browser.
	TestTrue(TEXT("With no code on offer, nothing pairs"), Call(Port, TEXT("POST"), TEXT("/remote/api/pair"), TEXT(R"({"code": "123456"})"), Page).Body.Contains(TEXT("no_code")));
	FString Code = FHyperAIStudioRemote::NewCode();
	const FString Wrong = Code == TEXT("000000") ? TEXT("000001") : TEXT("000000");
	for (int32 Try = 0; Try < 5; ++Try)
	{
		TestEqual(TEXT("A wrong code is refused"), Call(Port, TEXT("POST"), TEXT("/remote/api/pair"), FString::Printf(TEXT(R"({"code": "%s"})"), *Wrong), Page).Code, 403);
	}
	TestTrue(TEXT("After five wrong tries even the right code is void"), FHyperAIStudioRemote::GetCode().IsEmpty()
		&& Call(Port, TEXT("POST"), TEXT("/remote/api/pair"), FString::Printf(TEXT(R"({"code": "%s"})"), *Code), Page).Body.Contains(TEXT("no_code")));
	Code = FHyperAIStudioRemote::NewCode();
	const FAnswer Paired = Call(Port, TEXT("POST"), TEXT("/remote/api/pair"), FString::Printf(TEXT(R"({"code": "%s"})"), *Code), Page);
	AddInfo(TEXT("Cookie: ") + Paired.SetCookie.Left(40) + TEXT("..."));
	TestTrue(TEXT("The right code pairs, with a cookie only this page's requests carry"), Paired.Code == 200 && Paired.SetCookie.StartsWith(TEXT("hyperai_remote="))
		&& Paired.SetCookie.Contains(TEXT("HttpOnly")) && Paired.SetCookie.Contains(TEXT("SameSite=Strict")) && Paired.SetCookie.Contains(TEXT("Path=/remote")));
	TestTrue(TEXT("and is used up"), FHyperAIStudioRemote::GetCode().IsEmpty());
	TMap<FString, FString> Browser = Page;
	Browser.Add(TEXT("Cookie"), Paired.SetCookie.Left(Paired.SetCookie.Find(TEXT(";"))));

	// An edit waits; the paired browser sees it, approves one and rejects another.
	bool bRan = false;
	FHyperAIStudioApprovalSummary Summary;
	Summary.PackId = TEXT("level_design");
	Summary.ToolName = TEXT("hyper_level_apply_plan");
	Summary.EffectTarget = TEXT("/Game/Maps/Arena");
	Summary.Effects = {TEXT("spawn_actor PointLight above PlayerStart")};
	const FString First = TEXT("remote-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(10).ToLower();
	const FString Second = TEXT("remote-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(10).ToLower();
	TestTrue(TEXT("An edit waits for approval"), FHyperAIStudioApprovalGate::RequestAction(First, Summary, [&bRan]() { bRan = true; }, Error)
		&& FHyperAIStudioApprovalGate::RequestAction(Second, Summary, []() {}, Error));
	ON_SCOPE_EXIT
	{
		FHyperAIStudioApprovalGate::Reject(First, TEXT("test over"));
		FHyperAIStudioApprovalGate::Reject(Second, TEXT("test over"));
	};
	const FAnswer State = Call(Port, TEXT("GET"), TEXT("/remote/api/state"), FString(), Browser);
	TestTrue(TEXT("The paired browser sees the edits waiting, with what they do"), State.Code == 200 && State.Body.Contains(First)
		&& State.Body.Contains(TEXT("spawn_actor PointLight above PlayerStart")) && State.Body.Contains(TEXT("\"chats\"")));
	TestTrue(TEXT("It approves one, which runs"), Call(Port, TEXT("POST"), TEXT("/remote/api/approve"), FString::Printf(TEXT(R"({"id": "%s"})"), *First), Browser)
		.Body.Contains(TEXT("\"ok\":true")) && bRan && !FHyperAIStudioApprovalGate::HasPending(First));
	TestTrue(TEXT("and rejects the other"), Call(Port, TEXT("POST"), TEXT("/remote/api/reject"), FString::Printf(TEXT(R"({"id": "%s"})"), *Second), Browser)
		.Body.Contains(TEXT("\"ok\":true")) && !FHyperAIStudioApprovalGate::HasPending(Second));
	TestTrue(TEXT("An edit nobody holds cannot be approved"), Call(Port, TEXT("POST"), TEXT("/remote/api/approve"), FString::Printf(TEXT(R"({"id": "%s"})"), *First), Browser)
		.Body.Contains(TEXT("\"ok\":false")));
	TestTrue(TEXT("Each waiting edit says whether auto-approval is about to approve it"), State.Body.Contains(TEXT("\"autoApproving\":false")));

	// Auto-approval: the page shows it and can stop it; nothing on it turns it on.
	FHyperAIStudioAutoApprovalTestAccess::Reset();
	ON_SCOPE_EXIT { FHyperAIStudioAutoApprovalTestAccess::Reset(); };
	auto AutoApprovalOf = [](const FAnswer& Answer)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedPtr<FJsonObject>* Auto = nullptr;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Answer.Body), Root) && Root.IsValid()
			&& Root->TryGetObjectField(TEXT("autoApproval"), Auto) && Auto ? *Auto : TSharedPtr<FJsonObject>();
	};
	TSharedPtr<FJsonObject> Auto = AutoApprovalOf(Call(Port, TEXT("GET"), TEXT("/remote/api/state"), FString(), Browser));
	TestTrue(TEXT("The page sees auto-approval off"), Auto.IsValid() && !Auto->GetBoolField(TEXT("on")));
	for (const TCHAR* Action : {TEXT("auto_approval"), TEXT("start_auto_approval"), TEXT("enable_auto_approval"), TEXT("turn_on_auto_approval")})
	{
		Call(Port, TEXT("POST"), FString(TEXT("/remote/api/")) + Action, TEXT("{}"), Browser);
		TestFalse(FString::Printf(TEXT("No page action turns it on (%s)"), Action), FHyperAIStudioAutoApproval::IsActive());
	}
	TestTrue(TEXT("Auto-approval turns on in the editor"), FHyperAIStudioAutoApprovalTestAccess::TurnOn({EHyperAIStudioApprovalCategory::Edits}, {}));
	Auto = AutoApprovalOf(Call(Port, TEXT("GET"), TEXT("/remote/api/state"), FString(), Browser));
	TestTrue(TEXT("The page sees it on, with what it covers"), Auto.IsValid() && Auto->GetBoolField(TEXT("on"))
		&& Auto->GetArrayField(TEXT("categories")).Num() == 1 && Auto->GetArrayField(TEXT("categories"))[0]->AsString() == TEXT("edits"));
	TestTrue(TEXT("The page can stop it"), Call(Port, TEXT("POST"), TEXT("/remote/api/stop_auto_approval"), TEXT("{}"), Browser)
		.Body.Contains(TEXT("\"ok\":true")));
	TestFalse(TEXT("and it is off"), FHyperAIStudioAutoApproval::IsActive());
	TestTrue(TEXT("stopped remotely"), FHyperAIStudioAutoApprovalTestAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::StoppedRemotely);

	FHyperAIStudioRemote::ForgetBrowsers();
	TestEqual(TEXT("A forgotten browser must pair again"), Call(Port, TEXT("GET"), TEXT("/remote/api/state"), FString(), Browser).Code, 401);
	FHyperAIStudioRemote::Stop();
	TestFalse(TEXT("A stopped page stops answering"), Call(Port, TEXT("GET"), TEXT("/remote"), FString()).Body.Contains(TEXT("HyperAI Studio Remote")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
