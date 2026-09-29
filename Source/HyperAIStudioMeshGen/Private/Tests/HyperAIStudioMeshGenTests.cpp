// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioMeshGenToolset.h"
#include "HyperAIStudioSettings.h"
#include "IHttpRouter.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"

namespace HyperAIStudio::MeshGenTests
{
	/** The smallest model a provider could send: one triangle, as a GLB. */
	TArray<uint8> TriangleGlb()
	{
		FString Json = TEXT(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0,"name":"Crate"}],)"
			R"("meshes":[{"name":"Crate","primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"buffers":[{"byteLength":44}],)"
			R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],)"
			R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
			R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})");
		while (Json.Len() % 4 != 0)
		{
			Json.AppendChar(TEXT(' '));
		}
		const FTCHARToUTF8 JsonUtf8(*Json);
		const float Positions[9] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
		const uint16 Indices[4] = {0, 1, 2, 0};
		TArray<uint8> Glb;
		auto Word = [&Glb](const uint32 Value) { Glb.Append(reinterpret_cast<const uint8*>(&Value), 4); };
		Word(0x46546C67u);
		Word(2);
		Word(12 + 8 + JsonUtf8.Length() + 8 + 44);
		Word(JsonUtf8.Length());
		Word(0x4E4F534Au);
		Glb.Append(reinterpret_cast<const uint8*>(JsonUtf8.Get()), JsonUtf8.Length());
		Word(44);
		Word(0x004E4942u);
		Glb.Append(reinterpret_cast<const uint8*>(Positions), sizeof(Positions));
		Glb.Append(reinterpret_cast<const uint8*>(Indices), 8);
		return Glb;
	}

	/** What the stand-in services saw. */
	struct FSeen
	{
		TArray<FString> Calls;
		TMap<FString, FString> Auth;
		TArray<FString> Bodies;
		int32 Polls = 0;
	};

	FString BodyText(const FHttpServerRequest& Request)
	{
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		return FString(Text.Length(), Text.Get());
	}

	FString Header(const FHttpServerRequest& Request, const TCHAR* Name)
	{
		for (const TPair<FString, TArray<FString>>& Pair : Request.Headers)
		{
			if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase) && !Pair.Value.IsEmpty())
			{
				return Pair.Value[0];
			}
		}
		return FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMeshGenProvidersTest,
	"HyperAIStudio.MeshGen.Providers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioMeshGenProvidersTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::MeshGenTests;
	constexpr uint32 Port = 18351;
	const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
	if (!Router.IsValid())
	{
		AddWarning(TEXT("Port 18351 is taken, so the stand-ins for Tripo, Meshy and Hi3D could not start."));
		return true;
	}
	const FString Base = FString::Printf(TEXT("http://127.0.0.1:%u"), Port);
	const TSharedRef<FSeen> Seen = MakeShared<FSeen>();
	const TSharedRef<TArray<uint8>> Glb = MakeShared<TArray<uint8>>(TriangleGlb());
	TArray<FHttpRouteHandle> Routes;
	// Each answers as its provider documents: JSON with a task id, then a status that is running once and done after.
	auto Json = [](const FString& Text, const EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Text, TEXT("application/json"));
		Response->Code = Code;
		return Response;
	};
	auto Bind = [&](const TCHAR* Path, const EHttpServerRequestVerbs Verb, TFunction<TUniquePtr<FHttpServerResponse>(const FHttpServerRequest&)> Answer)
	{
		Routes.Add(Router->BindRoute(FHttpPath(Path), Verb, FHttpRequestHandler::CreateLambda(
			[Seen, Path = FString(Path), Answer](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				Seen->Calls.Add(Path);
				Seen->Auth.Add(Path, Header(Request, TEXT("Authorization")));
				Seen->Bodies.Add(BodyText(Request).Left(2000));
				OnComplete(Answer(Request));
				return true;
			})));
	};
	auto Poll = [Seen, Json, Base](const FString& Running, const FString& Done)
	{
		return [Seen, Json, Base, Running, Done](const FHttpServerRequest&)
		{
			return Json((Seen->Polls++ % 2 == 0 ? Running : Done).Replace(TEXT("{base}"), *Base));
		};
	};
	Bind(TEXT("/model.glb"), EHttpServerRequestVerbs::VERB_GET, [Glb](const FHttpServerRequest&)
	{
		return FHttpServerResponse::Create(TArray<uint8>(*Glb), TEXT("model/gltf-binary"));
	});
	Bind(TEXT("/tripo/v3/files"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"code":0,"data":{"file_token":"file_crate"}})")); });
	Bind(TEXT("/tripo/v3/generation/image-to-model"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"code":0,"data":{"task_id":"task_tripo"}})")); });
	Bind(TEXT("/tripo/v3/tasks/task_tripo"), EHttpServerRequestVerbs::VERB_GET, Poll(TEXT(R"({"code":0,"data":{"status":"running","progress":40}})"),
		TEXT(R"({"code":0,"data":{"status":"success","progress":100,"output":{"model_url":"{base}/model.glb"}}})")));
	Bind(TEXT("/meshy/openapi/v2/text-to-3d"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest& Request)
	{
		return Json(BodyText(Request).Contains(TEXT("\"refine\"")) ? TEXT(R"({"result":"meshy_refine"})") : TEXT(R"({"result":"meshy_preview"})"), EHttpServerResponseCodes::Accepted);
	});
	Bind(TEXT("/meshy/openapi/v2/text-to-3d/meshy_preview"), EHttpServerRequestVerbs::VERB_GET, Poll(TEXT(R"({"status":"IN_PROGRESS","progress":30})"),
		TEXT(R"({"status":"SUCCEEDED","progress":100,"model_urls":{}})")));
	Bind(TEXT("/meshy/openapi/v2/text-to-3d/meshy_refine"), EHttpServerRequestVerbs::VERB_GET, Poll(TEXT(R"({"status":"IN_PROGRESS","progress":70})"),
		TEXT(R"({"status":"SUCCEEDED","progress":100,"model_urls":{"glb":"{base}/model.glb"}})")));
	Bind(TEXT("/hi3d/open-api/v1/auth/token"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"code":200,"data":{"accessToken":"hi3d-access","tokenType":"Bearer"}})")); });
	Bind(TEXT("/hi3d/open-api/v1/submit-task"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"code":200,"data":{"task_id":"hi3d_task"},"msg":"success"})")); });
	Bind(TEXT("/hi3d/open-api/v1/query-task"), EHttpServerRequestVerbs::VERB_GET, Poll(TEXT(R"({"code":200,"data":{"task_id":"hi3d_task","state":"processing"}})"),
		TEXT(R"({"code":200,"data":{"task_id":"hi3d_task","state":"success","url":"{base}/model.glb"}})")));
	FHttpServerModule::Get().StartAllListeners();

	// The providers, pointed at the stand-ins, with keys only this test sets.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FHyperAIStudioExternalServiceSettings> SavedServices = Settings->ExternalServices;
	const FString SavedFolder = Settings->MeshImportFolder;
	for (FHyperAIStudioExternalServiceSettings Service : UHyperAIStudioSettings::GetMeshServices())
	{
		Service.BaseUrl = Base / Service.Id + (Service.Id == TEXT("tripo") ? TEXT("/v3") : TEXT(""));
		Service.ApiKeyVariable = TEXT("HYPERAI_TEST_") + Service.Id.ToUpper() + TEXT("_KEY");
		Settings->ExternalServices.Add(Service);
	}
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_TRIPO_KEY"), TEXT("tripo-test-key"));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_MESHY_KEY"), TEXT("meshy-test-key"));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_HI3D_KEY"), TEXT("client:secret"));
	Settings->MeshImportFolder = TEXT("/Game/HyperAITests/MeshGen");
	FHyperAIStudioMeshGenContracts::SetPollSecondsForTests(0.2);
	const FString Picture = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests/meshgen_crate.png"));
	const TArray<uint8> Png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0};
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Picture), /*Tree=*/true);
	FFileHelper::SaveArrayToFile(Png, *Picture);
	FHyperAIStudioExternalServices::ResetSessionSpend();
	TArray<FString> Imported;
	TArray<FString> Operations;
	ON_SCOPE_EXIT
	{
		for (const FHttpRouteHandle& Route : Routes)
		{
			Router->UnbindRoute(Route);
		}
		Settings->ExternalServices = SavedServices;
		Settings->MeshImportFolder = SavedFolder;
		for (const TCHAR* Variable : {TEXT("HYPERAI_TEST_TRIPO_KEY"), TEXT("HYPERAI_TEST_MESHY_KEY"), TEXT("HYPERAI_TEST_HI3D_KEY")})
		{
			FPlatformMisc::SetEnvironmentVar(Variable, TEXT(""));
		}
		FHyperAIStudioMeshGenContracts::SetPollSecondsForTests(0.0);
		FHyperAIStudioExternalServices::ResetSessionSpend();
		IFileManager::Get().Delete(*Picture, false, true, true);
		for (const FString& Operation : Operations)
		{
			IFileManager::Get().DeleteDirectory(*(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Models") / Operation), false, true);
		}
		TArray<UObject*> Objects;
		for (const FString& Path : Imported)
		{
			if (UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Path))
			{
				Objects.Add(Object);
			}
		}
		if (!Objects.IsEmpty())
		{
			ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
		}
	};

	// A dry run prices it and spends nothing; a provider that needs a picture, or a key, says so.
	FHyperAIMeshGenerateRequest Request;
	Request.Provider = TEXT("tripo");
	Request.Prompt = TEXT("a wooden crate with iron bands");
	const FHyperAIMeshGenerateReport Checked = FHyperAIStudioMeshGenContracts::Generate(Request);
	AddInfo(TEXT("Dry run: ") + Checked.Diagnostic);
	TestTrue(TEXT("A dry run prices the model and names where it lands"), Checked.bOk && Checked.Status == TEXT("checked")
		&& FMath::IsNearlyEqual(Checked.EstimatedCost, 0.30f) && Checked.Destination == TEXT("/Game/HyperAITests/MeshGen/SM_WoodenCrateWithIron"));
	Request.Provider = TEXT("hi3d");
	TestEqual(TEXT("Hi3D needs a picture"), FHyperAIStudioMeshGenContracts::Generate(Request).Status, FString(TEXT("needs_picture")));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_MESHY_KEY"), TEXT(""));
	Request.Provider = TEXT("meshy");
	const FHyperAIMeshGenerateReport NoKey = FHyperAIStudioMeshGenContracts::Generate(Request);
	TestTrue(TEXT("A provider without a key names the variable to set"), NoKey.Status == TEXT("missing_key") && NoKey.Diagnostic.Contains(TEXT("HYPERAI_TEST_MESHY_KEY")));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_MESHY_KEY"), TEXT("meshy-test-key"));

	// Each provider for real, through approval, to an imported static mesh.
	auto Run = [&](const TCHAR* Provider, const FString& Prompt, const FString& Image) -> FHyperAIMeshJob
	{
		FHyperAIMeshGenerateRequest Live;
		Live.Provider = Provider;
		Live.Prompt = Prompt;
		Live.ImagePath = Image;
		Live.Name = FString::Printf(TEXT("SM_Test_%s_%s"), Provider, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(6));
		Live.bDryRun = false;
		const FHyperAIMeshGenerateReport Asked = FHyperAIStudioMeshGenContracts::Generate(Live);
		Operations.Add(Asked.OperationId);
		if (!TestEqual(FString(Provider) + TEXT(" waits for approval: ") + Asked.Diagnostic, Asked.Status, FString(TEXT("awaiting_user_approval"))))
		{
			return FHyperAIMeshJob();
		}
		FString Error;
		TestTrue(FString(Provider) + TEXT(" is approved: ") + Error, FHyperAIStudioApprovalGate::Approve(Asked.OperationId, Error));
		FHyperAIMeshJob Job;
		const double End = FPlatformTime::Seconds() + 30.0;
		while (FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.02f);
			FPlatformProcess::Sleep(0.01f);
			const FHyperAIMeshStatusReport Status = FHyperAIStudioMeshGenContracts::Status(Asked.OperationId);
			Job = Status.Jobs.IsEmpty() ? FHyperAIMeshJob() : Status.Jobs[0];
			if (Job.State == TEXT("completed") || Job.State == TEXT("failed"))
			{
				break;
			}
		}
		AddInfo(FString::Printf(TEXT("%s: %s, %s"), Provider, *Job.State, *Job.Diagnostic));
		Imported.Append(Job.ImportedAssets);
		TestTrue(FString(Provider) + TEXT(" imports the model as a static mesh, in a folder of its own"), Job.State == TEXT("completed")
			&& Job.AssetPath.StartsWith(Asked.Destination + TEXT("/")) && StaticLoadObject(UObject::StaticClass(), nullptr, *Job.AssetPath) != nullptr);
		return Job;
	};
	const FString Relative = TEXT("Saved/HyperAIStudio/Tests/meshgen_crate.png");
	Run(TEXT("tripo"), FString(), Relative);
	TestTrue(TEXT("Tripo gets the picture uploaded, then made into a model from its file token, with the key as a bearer"),
		Seen->Calls.Contains(TEXT("/tripo/v3/files")) && Seen->Bodies.ContainsByPredicate([](const FString& Body) { return Body.Contains(TEXT("\"input\":\"file_crate\"")); })
		&& Seen->Auth.FindRef(TEXT("/tripo/v3/files")) == TEXT("Bearer tripo-test-key"));
	Run(TEXT("meshy"), TEXT("a wooden crate"), FString());
	TestTrue(TEXT("Meshy makes a model from words in two tasks, the mesh and then its texture"),
		Seen->Bodies.ContainsByPredicate([](const FString& Body) { return Body.Contains(TEXT("\"mode\":\"preview\"")); })
		&& Seen->Bodies.ContainsByPredicate([](const FString& Body) { return Body.Contains(TEXT("\"preview_task_id\":\"meshy_preview\"")); }));
	const FHyperAIMeshJob Hi3d = Run(TEXT("hi3d"), FString(), Relative);
	TestTrue(TEXT("Hi3D trades client_id:client_secret for a token, then takes the picture as a form, as a GLB"),
		Seen->Auth.FindRef(TEXT("/hi3d/open-api/v1/auth/token")) == TEXT("Basic ") + FBase64::Encode(TEXT("client:secret"))
		&& Seen->Auth.FindRef(TEXT("/hi3d/open-api/v1/submit-task")) == TEXT("Bearer hi3d-access")
		&& Seen->Bodies.ContainsByPredicate([](const FString& Body) { return Body.Contains(TEXT("name=\"request_type\"")) && Body.Contains(TEXT("name=\"images\"")); }));
	TestTrue(TEXT("and its credit line is kept"), Hi3d.Diagnostic.Contains(TEXT("Created with Hitem3D")));
	TestTrue(TEXT("Each accepted task is booked against the session's cap"), FMath::IsNearlyEqual(FHyperAIStudioExternalServices::GetSessionSpend(), 0.30 + 0.60 + 1.30, 0.001));

	// Turned down, nothing is sent.
	Request.Provider = TEXT("tripo");
	Request.bDryRun = false;
	const int32 CallsBefore = Seen->Calls.Num();
	const FHyperAIMeshGenerateReport Asked = FHyperAIStudioMeshGenContracts::Generate(Request);
	FHyperAIStudioApprovalGate::Reject(Asked.OperationId, TEXT("Not now."));
	const FHyperAIMeshStatusReport Rejected = FHyperAIStudioMeshGenContracts::Status(Asked.OperationId);
	TestTrue(TEXT("A rejected job shows as rejected and reaches no provider"), !Rejected.Jobs.IsEmpty() && Rejected.Jobs[0].State == TEXT("rejected")
		&& Seen->Calls.Num() == CallsBefore);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
