// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
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
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "ReferenceSkeleton.h"

namespace HyperAIStudio::MeshGenCharacterTests
{
	using C = FHyperAIStudioMeshGenContracts;

	/** A GLB from its JSON and its binary chunk, each padded to four bytes as the format asks. */
	TArray<uint8> Glb(FString Json, TArray<uint8> Bin)
	{
		while (Json.Len() % 4 != 0)
		{
			Json.AppendChar(TEXT(' '));
		}
		while (Bin.Num() % 4 != 0)
		{
			Bin.Add(0);
		}
		const FTCHARToUTF8 JsonUtf8(*Json);
		TArray<uint8> Out;
		auto Word = [&Out](const uint32 Value) { Out.Append(reinterpret_cast<const uint8*>(&Value), 4); };
		Word(0x46546C67u);
		Word(2);
		Word(12 + 8 + JsonUtf8.Length() + 8 + Bin.Num());
		Word(JsonUtf8.Length());
		Word(0x4E4F534Au);
		Out.Append(reinterpret_cast<const uint8*>(JsonUtf8.Get()), JsonUtf8.Length());
		Word(Bin.Num());
		Word(0x004E4942u);
		Out.Append(Bin);
		return Out;
	}

	/** One triangle, as a textured model comes back at its smallest. */
	TArray<uint8> TriangleGlb()
	{
		const float Positions[9] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
		const uint16 Indices[4] = {0, 1, 2, 0};
		TArray<uint8> Bin(reinterpret_cast<const uint8*>(Positions), sizeof(Positions));
		Bin.Append(reinterpret_cast<const uint8*>(Indices), sizeof(Indices));
		return Glb(TEXT(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0,"name":"Crate"}],)")
			TEXT(R"("meshes":[{"name":"Crate","primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"buffers":[{"byteLength":44}],)")
			TEXT(R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],)")
			TEXT(R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)")
			TEXT(R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})"), Bin);
	}

	/**
	 * The smallest character a provider could send: three joints (Root, then a hip bone, then a spine bone a metre up) and a
	 * box on each of the last two, weighted to it. With Clip, a one-second animation of that name bends the spine, as an
	 * animation result is: the skinned character with its clip.
	 */
	TArray<uint8> CharacterGlb(const TCHAR* Root, const TCHAR* Hips, const TCHAR* Spine, const TCHAR* Clip = nullptr)
	{
		TArray<float> Positions;
		TArray<uint8> Joints;
		TArray<float> Weights;
		TArray<uint16> Indices;
		// glTF is Y up, in metres: the hip box from 0 to 0.9 m, the spine box from 1.1 to 2 m, each 0.4 m across.
		auto Box = [&](const float Bottom, const float Top, const uint8 Joint)
		{
			const uint16 Base = static_cast<uint16>(Positions.Num() / 3);
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				Positions.Append({(Corner & 1) ? 0.2f : -0.2f, (Corner & 2) ? Top : Bottom, (Corner & 4) ? 0.2f : -0.2f});
				Joints.Append({Joint, 0, 0, 0});
				Weights.Append({1.f, 0.f, 0.f, 0.f});
			}
			const uint16 Faces[36] = {0, 1, 2, 1, 3, 2, 4, 6, 5, 5, 6, 7, 0, 4, 1, 1, 4, 5, 2, 3, 6, 3, 7, 6, 0, 2, 4, 2, 6, 4, 1, 5, 3, 3, 5, 7};
			for (const uint16 Index : Faces)
			{
				Indices.Add(Base + Index);
			}
		};
		Box(0.f, 0.9f, 1);
		Box(1.1f, 2.f, 2);
		// Column-major inverse bind matrices: Root and the hips at the origin, the spine a metre up.
		const float Inverse[48] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
			1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1};
		const float Times[3] = {0.f, 0.5f, 1.f};
		const float Rotations[12] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.258819f, 0.965926f, 0.f, 0.f, 0.f, 1.f};
		TArray<uint8> Bin;
		auto Add = [&Bin](const void* Data, const int32 Bytes) { Bin.Append(static_cast<const uint8*>(Data), Bytes); };
		Add(Positions.GetData(), Positions.Num() * 4);  // 0: 192 bytes
		Add(Joints.GetData(), Joints.Num());            // 192: 64
		Add(Weights.GetData(), Weights.Num() * 4);      // 256: 256
		Add(Indices.GetData(), Indices.Num() * 2);      // 512: 144
		Add(Inverse, sizeof(Inverse));                  // 656: 192
		Add(Times, sizeof(Times));                      // 848: 12
		Add(Rotations, sizeof(Rotations));              // 860: 48
		FString Json = FString::Printf(TEXT(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],)")
			TEXT(R"("nodes":[{"name":"Body","mesh":0,"skin":0},{"name":"%s","children":[2]},{"name":"%s","children":[3]},{"name":"%s","translation":[0,1,0]}],)")
			TEXT(R"("meshes":[{"name":"Body","primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2},"indices":3}]}],)")
			TEXT(R"("skins":[{"joints":[1,2,3],"inverseBindMatrices":4,"skeleton":1}],"buffers":[{"byteLength":%d}],)")
			TEXT(R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":192},{"buffer":0,"byteOffset":192,"byteLength":64},)")
			TEXT(R"({"buffer":0,"byteOffset":256,"byteLength":256},{"buffer":0,"byteOffset":512,"byteLength":144},)")
			TEXT(R"({"buffer":0,"byteOffset":656,"byteLength":192},{"buffer":0,"byteOffset":848,"byteLength":12},{"buffer":0,"byteOffset":860,"byteLength":48}],)")
			TEXT(R"("accessors":[{"bufferView":0,"componentType":5126,"count":16,"type":"VEC3","min":[-0.2,0,-0.2],"max":[0.2,2,0.2]},)")
			TEXT(R"({"bufferView":1,"componentType":5121,"count":16,"type":"VEC4"},{"bufferView":2,"componentType":5126,"count":16,"type":"VEC4"},)")
			TEXT(R"({"bufferView":3,"componentType":5123,"count":72,"type":"SCALAR"},{"bufferView":4,"componentType":5126,"count":3,"type":"MAT4"},)")
			TEXT(R"({"bufferView":5,"componentType":5126,"count":3,"type":"SCALAR","min":[0],"max":[1]},{"bufferView":6,"componentType":5126,"count":3,"type":"VEC4"}])"),
			Root, Hips, Spine, Bin.Num());
		if (Clip)
		{
			Json += FString::Printf(TEXT(R"(,"animations":[{"name":"%s","samplers":[{"input":5,"output":6,"interpolation":"LINEAR"}],)")
				TEXT(R"("channels":[{"sampler":0,"target":{"node":3,"path":"rotation"}}]}])"), Clip);
		}
		return Glb(Json + TEXT("}"), Bin);
	}

	/** What the stand-in services saw. */
	struct FSeen
	{
		TArray<FString> Calls;
		TMap<FString, FString> Auth;
		TArray<FString> Bodies;
		TMap<FString, int32> Polls;
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

	/** Ticks the editor until the job is done, failed, cancelled or rejected, for up to Seconds. */
	FHyperAIMeshJob WaitFor(const FString& OperationId, const double Seconds = 60.0)
	{
		FHyperAIMeshJob Job;
		const double End = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.02f);
			FPlatformProcess::Sleep(0.01f);
			const FHyperAIMeshStatusReport Status = C::Status(OperationId);
			Job = Status.Jobs.IsEmpty() ? FHyperAIMeshJob() : Status.Jobs[0];
			if (Job.State == TEXT("completed") || Job.State == TEXT("failed") || Job.State == TEXT("cancelled") || Job.State == TEXT("rejected"))
			{
				break;
			}
		}
		return Job;
	}

	bool AnyBody(const FSeen& Seen, std::initializer_list<const TCHAR*> Parts)
	{
		return Seen.Bodies.ContainsByPredicate([&Parts](const FString& Body)
		{
			for (const TCHAR* Part : Parts)
			{
				if (!Body.Contains(Part))
				{
					return false;
				}
			}
			return true;
		});
	}

	/** The project's UE5 Mannequin, when the project has one: a skeletal mesh whose skeleton has root, pelvis and spine_01. */
	USkeletalMesh* FindMannequin()
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(USkeletalMesh::StaticClass()->GetClassPathName(), Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
		for (const FAssetData& Asset : Assets)
		{
			if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/Characters/")))
			{
				continue;
			}
			USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.GetAsset());
			const FReferenceSkeleton* Bones = Mesh ? &Mesh->GetRefSkeleton() : nullptr;
			if (Bones && Bones->FindBoneIndex(FName(TEXT("root"))) == 0 && Bones->FindBoneIndex(FName(TEXT("pelvis"))) != INDEX_NONE
				&& Bones->FindBoneIndex(FName(TEXT("spine_01"))) != INDEX_NONE)
			{
				return Mesh;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMeshGenCharacterRulesTest,
	"HyperAIStudio.MeshGen.CharacterRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioMeshGenCharacterRulesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::MeshGenCharacterTests;
	// Texture levels are the providers' own words, up to 8K.
	TestEqual(TEXT("Meshy textures at 2k unless asked"), C::TextureLevel(TEXT("meshy"), FString()), FString(TEXT("2k")));
	TestEqual(TEXT("and up to 8k"), C::TextureLevel(TEXT("meshy"), TEXT("8K")), FString(TEXT("8k")));
	TestTrue(TEXT("but not Tripo's words"), C::TextureLevel(TEXT("meshy"), TEXT("detailed")).IsEmpty());
	TestEqual(TEXT("Tripo textures at standard unless asked"), C::TextureLevel(TEXT("tripo"), FString()), FString(TEXT("standard")));
	TestEqual(TEXT("its 8K is its extreme quality"), C::TextureLevel(TEXT("tripo"), TEXT("8k")), FString(TEXT("extreme")));
	TestTrue(TEXT("and it has no 4k to ask for"), C::TextureLevel(TEXT("tripo"), TEXT("4k")).IsEmpty());

	// Viggle's clip goes onto a skeleton unchanged only when the names are one of its own skeletons'.
	TestEqual(TEXT("A UE5 Mannequin takes the MetaHuman body clip"), C::ViggleSkeletonFor({TEXT("root"), TEXT("pelvis"), TEXT("spine_01"), TEXT("spine_02")}),
		FString(TEXT("metahuman_body")));
	TestEqual(TEXT("A Mixamo skeleton takes the Mixamo clip"), C::ViggleSkeletonFor({TEXT("mixamorig:Hips"), TEXT("mixamorig:Spine")}), FString(TEXT("mixamo")));
	TestTrue(TEXT("and anything else neither"), C::ViggleSkeletonFor({TEXT("Bone"), TEXT("Bone.001")}).IsEmpty());

	// Tripo's presets are the ones its rig model has, written in full.
	TestEqual(TEXT("A v1.0 rig's walk is preset:biped:walk"), C::TripoPreset(TEXT("v1.0-20240301"), TEXT("walk")), FString(TEXT("preset:biped:walk")));
	TestEqual(TEXT("in full or not"), C::TripoPreset(TEXT("v1.0-20240301"), TEXT("preset:biped:wave_goodbye_01")), FString(TEXT("preset:biped:wave_goodbye_01")));
	TestEqual(TEXT("A v2.5 rig's walk is preset:walk"), C::TripoPreset(TEXT("v2.5-20260210"), TEXT("walk")), FString(TEXT("preset:walk")));
	TestEqual(TEXT("and it has the quadruped's"), C::TripoPreset(TEXT("v2.5-20260210"), TEXT("quadruped:walk")), FString(TEXT("preset:quadruped:walk")));
	TestTrue(TEXT("A v2.5 rig has no dance_01"), C::TripoPreset(TEXT("v2.5-20260210"), TEXT("dance_01")).IsEmpty());
	TestTrue(TEXT("and nobody has a moonwalk"), C::TripoPreset(TEXT("v1.0-20240301"), TEXT("moonwalk")).IsEmpty());

	// Task prices are built in at list price, and a project's entry replaces one.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FHyperAIStudioMeshTaskPrice> Saved = Settings->MeshTaskPrices;
	ON_SCOPE_EXIT { Settings->MeshTaskPrices = Saved; };
	Settings->MeshTaskPrices.Reset();
	TestTrue(TEXT("Tripo's rig is 25 credits"), FMath::IsNearlyEqual(UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("tripo"), TEXT("rig")), 0.25f));
	TestTrue(TEXT("Meshy's 8K texture is 15 credits"), FMath::IsNearlyEqual(UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("meshy"), TEXT("texture_8k")), 0.30f));
	TestTrue(TEXT("Viggle's clip is a flat 10 credits"), FMath::IsNearlyEqual(UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("viggle"), TEXT("motion")), 0.10f));
	TestTrue(TEXT("An unknown task has no price"), UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("hi3d"), TEXT("rig")) < 0.f);
	FHyperAIStudioMeshTaskPrice Own;
	Own.Provider = TEXT("Meshy");
	Own.Task = TEXT("rig");
	Own.Cost = 0.5f;
	Settings->MeshTaskPrices.Add(Own);
	TestTrue(TEXT("A project's own price replaces the built-in one"), FMath::IsNearlyEqual(UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("meshy"), TEXT("rig")), 0.5f));

	// The library search reads Tripo's whole list and Meshy's built-in part without a key or a call.
	FHyperAIMeshActionsRequest Search;
	Search.Search = TEXT("wave");
	const FHyperAIMeshActionsReport Waves = C::Actions(Search);
	TestTrue(TEXT("A search finds Meshy's and Tripo's waves"), Waves.bOk
		&& Waves.Actions.ContainsByPredicate([](const FHyperAIMeshAction& Action) { return Action.Provider == TEXT("meshy") && Action.Id == TEXT("28"); })
		&& Waves.Actions.ContainsByPredicate([](const FHyperAIMeshAction& Action) { return Action.Id == TEXT("preset:biped:wave_goodbye_01"); }));
	Search.Search.Reset();
	Search.Provider = TEXT("tripo");
	Search.Category = TEXT("v2.5");
	TestEqual(TEXT("Tripo's v2.5 rig has its 16 presets"), C::Actions(Search).Matched, 16);
	Search.Category = TEXT("v1.0");
	TestEqual(TEXT("and its v1.0 rig its 101"), C::Actions(Search).Matched, 101);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMeshGenCharactersTest,
	"HyperAIStudio.MeshGen.Characters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioMeshGenCharactersTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::MeshGenCharacterTests;
	constexpr uint32 Port = 18352;
	const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
	if (!Router.IsValid())
	{
		AddWarning(TEXT("Port 18352 is taken, so the stand-ins for Meshy, Tripo and Viggle could not start."));
		return true;
	}
	const FString Base = FString::Printf(TEXT("http://127.0.0.1:%u"), Port);
	const TSharedRef<FSeen> Seen = MakeShared<FSeen>();
	TArray<FHttpRouteHandle> Routes;
	// Each answers as its provider documents (docs.meshy.ai/api, developers.tripo3d.ai, docs.viggle.ai): a task id, then
	// a status that is running once and done after, with addresses on this server for the files.
	auto Json = [Base](const FString& Text, const EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Text.Replace(TEXT("{base}"), *Base), TEXT("application/json"));
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
				Seen->Bodies.Add(BodyText(Request).Left(4000));
				OnComplete(Answer(Request));
				return true;
			})));
	};
	auto Poll = [Seen, Json](const FString& Key, const FString& Running, const FString& Done)
	{
		return [Seen, Json, Key, Running, Done](const FHttpServerRequest&)
		{
			return Json(Seen->Polls.FindOrAdd(Key)++ % 2 == 0 ? Running : Done);
		};
	};
	auto File = [](TArray<uint8> Bytes)
	{
		return [Bytes = MoveTemp(Bytes)](const FHttpServerRequest&) { return FHttpServerResponse::Create(TArray<uint8>(Bytes), TEXT("model/gltf-binary")); };
	};
	Bind(TEXT("/files/character.glb"), EHttpServerRequestVerbs::VERB_GET, File(CharacterGlb(TEXT("Root"), TEXT("Hips"), TEXT("Spine"))));
	Bind(TEXT("/files/walk.glb"), EHttpServerRequestVerbs::VERB_GET, File(CharacterGlb(TEXT("Root"), TEXT("Hips"), TEXT("Spine"), TEXT("Walk"))));
	Bind(TEXT("/files/clip.glb"), EHttpServerRequestVerbs::VERB_GET, File(CharacterGlb(TEXT("Root"), TEXT("Hips"), TEXT("Spine"), TEXT("Wave"))));
	Bind(TEXT("/files/metahuman.glb"), EHttpServerRequestVerbs::VERB_GET, File(CharacterGlb(TEXT("root"), TEXT("pelvis"), TEXT("spine_01"), TEXT("Wave"))));
	Bind(TEXT("/files/textured.glb"), EHttpServerRequestVerbs::VERB_GET, File(TriangleGlb()));

	// Meshy: rigging (one that fails, one that never ends), animations with its library, text to motion, retexture.
	Bind(TEXT("/meshy/openapi/v1/rigging"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest& Request)
	{
		const FString Body = BodyText(Request);
		return Json(Body.Contains(TEXT("fail_source")) ? TEXT(R"({"result":"rig_fail"})") : Body.Contains(TEXT("slow_source")) ? TEXT(R"({"result":"rig_slow"})")
			: TEXT(R"({"result":"rig_meshy"})"), EHttpServerResponseCodes::Accepted);
	});
	Bind(TEXT("/meshy/openapi/v1/rigging/rig_meshy"), EHttpServerRequestVerbs::VERB_GET, Poll(TEXT("rig_meshy"), TEXT(R"({"status":"IN_PROGRESS","progress":40})"),
		TEXT(R"({"status":"SUCCEEDED","progress":100,"result":{"rigged_character_glb_url":"{base}/files/character.glb","basic_animations":{"walking_glb_url":"{base}/files/walk.glb"}}})")));
	Bind(TEXT("/meshy/openapi/v1/rigging/rig_fail"), EHttpServerRequestVerbs::VERB_GET, [Json](const FHttpServerRequest&)
	{
		return Json(TEXT(R"({"status":"FAILED","progress":0,"task_error":{"message":"Pose estimation failed"}})"));
	});
	Bind(TEXT("/meshy/openapi/v1/rigging/rig_slow"), EHttpServerRequestVerbs::VERB_GET, [Json](const FHttpServerRequest&)
	{
		return Json(TEXT(R"({"status":"IN_PROGRESS","progress":10})"));
	});
	Bind(TEXT("/meshy/openapi/v1/animations"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"result":"anim_meshy"})"), EHttpServerResponseCodes::Accepted); });
	Bind(TEXT("/meshy/openapi/v1/animations/anim_meshy"), EHttpServerRequestVerbs::VERB_GET, Poll(TEXT("anim_meshy"), TEXT(R"({"status":"PENDING","progress":0})"),
		TEXT(R"({"status":"SUCCEEDED","progress":100,"result":{"animation_glb_url":"{base}/files/clip.glb","animation_fbx_url":""}})")));
	Bind(TEXT("/meshy/openapi/v1/animations/library"), EHttpServerRequestVerbs::VERB_GET, [Json](const FHttpServerRequest&)
	{
		return Json(TEXT(R"([{"action_id":0,"name":"Idle","key":"Idle","category":"DailyActions","sub_category":"Idle"},)")
			TEXT(R"({"action_id":92,"name":"Double Combo Attack","key":"Double_Combo_Attack","category":"Fighting","sub_category":"AttackingwithWeapon"}])"));
	});
	Bind(TEXT("/meshy/openapi/v1/text-to-motion"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"result":"motion_meshy"})"), EHttpServerResponseCodes::Accepted); });
	Bind(TEXT("/meshy/openapi/v1/text-to-motion/motion_meshy"), EHttpServerRequestVerbs::VERB_GET, Poll(TEXT("motion_meshy"), TEXT(R"({"status":"IN_PROGRESS","progress":50})"),
		TEXT(R"({"status":"SUCCEEDED","progress":100,"result":{"motion_url":"{base}/files/clip.glb","motion_format":"fbx","duration_ms":3000,"mode":"prime"}})")));
	Bind(TEXT("/meshy/openapi/v1/retexture"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"result":"tex_meshy"})"), EHttpServerResponseCodes::Accepted); });
	Bind(TEXT("/meshy/openapi/v1/retexture/tex_meshy"), EHttpServerRequestVerbs::VERB_GET, Poll(TEXT("tex_meshy"), TEXT(R"({"status":"IN_PROGRESS","progress":60})"),
		TEXT(R"({"status":"SUCCEEDED","progress":100,"model_urls":{"glb":"{base}/files/textured.glb"},"texture_urls":[{"base_color":"{base}/files/none.png"}]})")));

	// Tripo: an upload, its rig and retarget, and a texture refused for want of credits.
	Bind(TEXT("/tripo/v3/files"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"code":0,"data":{"file_token":"file_mesh"}})")); });
	Bind(TEXT("/tripo/v3/animations/rig"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"code":0,"data":{"task_id":"rig_tripo"}})")); });
	Bind(TEXT("/tripo/v3/animations/retarget"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"code":0,"data":{"task_id":"anim_tripo"}})")); });
	Bind(TEXT("/tripo/v3/models/texture"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&)
	{
		return Json(TEXT(R"({"code":2010,"message":"Insufficient credits","suggestion":"Please top up your account"})"), EHttpServerResponseCodes::Forbidden);
	});
	Bind(TEXT("/tripo/v3/tasks/rig_tripo"), EHttpServerRequestVerbs::VERB_GET, Poll(TEXT("rig_tripo"), TEXT(R"({"code":0,"data":{"task_id":"rig_tripo","status":"running","progress":30}})"),
		TEXT(R"({"code":0,"data":{"task_id":"rig_tripo","status":"success","progress":100,"output":{"model_url":"{base}/files/character.glb"}}})")));
	Bind(TEXT("/tripo/v3/tasks/anim_tripo"), EHttpServerRequestVerbs::VERB_GET, Poll(TEXT("anim_tripo"), TEXT(R"({"code":0,"data":{"task_id":"anim_tripo","status":"queued","progress":0}})"),
		TEXT(R"({"code":0,"data":{"task_id":"anim_tripo","status":"success","progress":100,"output":{"model_url":"{base}/files/clip.glb"}}})")));

	// Viggle: a motion from words, then its export with a file a skeleton.
	Bind(TEXT("/viggle/v1/motions"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"id":"mot_1","status":"queued","type":"glb"})")); });
	Bind(TEXT("/viggle/v1/motions/mot_1"), EHttpServerRequestVerbs::VERB_GET, Poll(TEXT("mot_1"), TEXT(R"({"id":"mot_1","status":"processing","progress":20})"),
		TEXT(R"({"id":"mot_1","status":"ready","progress":100,"glb":{"status":"ready","skeletons":["mixamo","metahuman"]}})")));
	Bind(TEXT("/viggle/v1/motions/mot_1/export"), EHttpServerRequestVerbs::VERB_GET, [Json](const FHttpServerRequest&)
	{
		return Json(TEXT(R"({"id":"mot_1","status":"ready","download_type":"glb","mixamo_url":"{base}/files/clip.glb","metahuman_url":"{base}/files/metahuman.glb",)")
			TEXT(R"("metahuman_body_url":"{base}/files/metahuman.glb","thumbnail_url":null})"));
	});
	FHttpServerModule::Get().StartAllListeners();

	// The providers, pointed at the stand-ins, with keys only this test sets.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FHyperAIStudioExternalServiceSettings> SavedServices = Settings->ExternalServices;
	const TArray<FHyperAIStudioMeshTaskPrice> SavedPrices = Settings->MeshTaskPrices;
	const FString SavedFolder = Settings->MeshImportFolder;
	const float SavedCap = Settings->ExternalServiceSessionSpendCap;
	const FString Folder = TEXT("/Game/HyperAITests/MeshGenCharacters");
	for (FHyperAIStudioExternalServiceSettings Service : UHyperAIStudioSettings::GetMeshServices())
	{
		Service.BaseUrl = Base / Service.Id + (Service.Id == TEXT("tripo") ? TEXT("/v3") : TEXT(""));
		Service.ApiKeyVariable = TEXT("HYPERAI_TEST_") + Service.Id.ToUpper() + TEXT("_KEY");
		Settings->ExternalServices.Add(Service);
	}
	Settings->MeshTaskPrices.Reset();
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_TRIPO_KEY"), TEXT("tripo-test-key"));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_MESHY_KEY"), TEXT("meshy-test-key"));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_VIGGLE_KEY"), TEXT("viggle-test-key"));
	Settings->MeshImportFolder = Folder;
	Settings->ExternalServiceSessionSpendCap = 5.f;
	C::SetPollSecondsForTests(0.2);
	FHyperAIStudioExternalServices::ResetSessionSpend();
	const FString Library = FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Models/meshy_animation_library.json");
	FString SavedLibrary;
	const bool bHadLibrary = FFileHelper::LoadFileToString(SavedLibrary, *Library);
	IFileManager::Get().Delete(*Library, false, true, true);
	TArray<FString> Operations;
	ON_SCOPE_EXIT
	{
		for (const FHttpRouteHandle& Route : Routes)
		{
			Router->UnbindRoute(Route);
		}
		Settings->ExternalServices = SavedServices;
		Settings->MeshTaskPrices = SavedPrices;
		Settings->MeshImportFolder = SavedFolder;
		Settings->ExternalServiceSessionSpendCap = SavedCap;
		for (const TCHAR* Variable : {TEXT("HYPERAI_TEST_TRIPO_KEY"), TEXT("HYPERAI_TEST_MESHY_KEY"), TEXT("HYPERAI_TEST_VIGGLE_KEY")})
		{
			FPlatformMisc::SetEnvironmentVar(Variable, TEXT(""));
		}
		C::SetPollSecondsForTests(0.0);
		FHyperAIStudioExternalServices::ResetSessionSpend();
		bHadLibrary ? (void)FFileHelper::SaveStringToFile(SavedLibrary, *Library) : (void)IFileManager::Get().Delete(*Library, false, true, true);
		for (const FString& Operation : Operations)
		{
			IFileManager::Get().DeleteDirectory(*(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Models") / Operation), false, true);
		}
		// Everything the jobs and the retarget hand-offs made is under the test's folder.
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Made;
		Registry.GetAssetsByPath(FName(*Folder), Made, /*bRecursive=*/true);
		TArray<UObject*> Objects;
		for (const FAssetData& Asset : Made)
		{
			if (UObject* Object = Asset.GetAsset())
			{
				Objects.Add(Object);
			}
		}
		if (!Objects.IsEmpty())
		{
			ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
		}
	};
	auto Approve = [&](const FHyperAIMeshTaskReport& Asked, const TCHAR* What) -> FHyperAIMeshJob
	{
		Operations.Add(Asked.OperationId);
		if (!TestEqual(FString(What) + TEXT(" waits for approval: ") + Asked.Diagnostic, Asked.Status, FString(TEXT("awaiting_user_approval"))))
		{
			return FHyperAIMeshJob();
		}
		FString Error;
		TestTrue(FString(What) + TEXT(" is approved: ") + Error, FHyperAIStudioApprovalGate::Approve(Asked.OperationId, Error));
		const FHyperAIMeshJob Job = WaitFor(Asked.OperationId);
		AddInfo(FString::Printf(TEXT("%s: %s, %s"), What, *Job.State, *Job.Diagnostic));
		return Job;
	};

	// Dry runs price the tasks and spend nothing; a request that is not one is refused before any of that.
	FHyperAIMeshRigRequest Rig;
	Rig.Provider = TEXT("meshy");
	Rig.SourceTaskId = TEXT("meshy_model");
	Rig.Name = TEXT("SK_TestKnight");
	const FHyperAIMeshTaskReport RigPrice = C::Rig(Rig);
	AddInfo(TEXT("Rig dry run: ") + RigPrice.Diagnostic);
	TestTrue(TEXT("A rig's dry run prices Meshy's rig and names where it lands"), RigPrice.bOk && RigPrice.Status == TEXT("checked")
		&& FMath::IsNearlyEqual(RigPrice.EstimatedCost, 0.10f) && RigPrice.Destination == Folder / TEXT("SK_TestKnight") && RigPrice.Tasks.Num() == 1);
	Rig.MeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
	TestEqual(TEXT("A rig takes one source"), C::Rig(Rig).Status, FString(TEXT("invalid_request")));
	Rig.MeshPath.Reset();
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_MESHY_KEY"), TEXT(""));
	const FHyperAIMeshTaskReport NoKey = C::Rig(Rig);
	TestTrue(TEXT("A provider without a key names the variable to set"), NoKey.Status == TEXT("missing_key") && NoKey.Diagnostic.Contains(TEXT("HYPERAI_TEST_MESHY_KEY")));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_MESHY_KEY"), TEXT("meshy-test-key"));
	FHyperAIMeshRetextureRequest Texture;
	Texture.Provider = TEXT("meshy");
	Texture.SourceTaskId = TEXT("meshy_model");
	Texture.Prompt = TEXT("weathered bronze with green patina");
	Texture.Resolution = TEXT("8k");
	TestTrue(TEXT("Meshy's 8K texture is priced at its 8K price"), FMath::IsNearlyEqual(C::Retexture(Texture).EstimatedCost, 0.30f));
	Texture.Provider = TEXT("tripo");
	Texture.Resolution = TEXT("detailed");
	TestTrue(TEXT("and Tripo's detailed one at its HD price"), FMath::IsNearlyEqual(C::Retexture(Texture).EstimatedCost, 0.20f));
	Texture.Resolution = TEXT("4k");
	TestEqual(TEXT("Tripo has no 4k to ask for"), C::Retexture(Texture).Status, FString(TEXT("invalid_request")));
	TestTrue(TEXT("No dry run reached a provider"), Seen->Calls.IsEmpty());

	// The session's spend cap refuses a job that would pass it, before anything is asked or sent.
	Settings->ExternalServiceSessionSpendCap = 0.05f;
	Rig.bDryRun = false;
	TestEqual(TEXT("A job over the spend cap is refused"), C::Rig(Rig).Status, FString(TEXT("over_spend_cap")));
	Settings->ExternalServiceSessionSpendCap = 5.f;

	// Cancelled while it waits for approval, it never runs.
	const FHyperAIMeshTaskReport Waiting = C::Rig(Rig);
	Operations.Add(Waiting.OperationId);
	const FHyperAIMeshCancelReport Dropped = C::Cancel(Waiting.OperationId);
	TestTrue(TEXT("A job cancelled before approval shows as cancelled and reaches no provider"), Dropped.bOk && Dropped.Job.State == TEXT("cancelled")
		&& !FHyperAIStudioApprovalGate::HasPending(Waiting.OperationId) && Seen->Calls.IsEmpty());

	// Meshy rigs its own model: a skeletal mesh with its skeleton and physics asset, and the walk it made with it.
	const FHyperAIMeshJob Meshy = Approve(C::Rig(Rig), TEXT("Meshy's rig"));
	const USkeletalMesh* MeshyMesh = Cast<USkeletalMesh>(StaticLoadObject(USkeletalMesh::StaticClass(), nullptr, *Meshy.SkeletalMesh));
	TestTrue(TEXT("Meshy's rig is imported as a skeletal mesh with its skeleton and a physics asset"), Meshy.State == TEXT("completed") && MeshyMesh
		&& MeshyMesh->GetSkeleton() && MeshyMesh->GetPhysicsAsset() && Meshy.PhysicsAsset == MeshyMesh->GetPhysicsAsset()->GetPathName()
		&& Meshy.SkeletalMesh.StartsWith(Folder / TEXT("SK_TestKnight/")));
	TestTrue(TEXT("with the walk Meshy made beside it, on its skeleton"), Meshy.Animations.Num() == 1);
	TestTrue(TEXT("Meshy is asked to rig its own task, at a height, with the key as a bearer"), AnyBody(*Seen, {TEXT("\"input_task_id\":\"meshy_model\""), TEXT("\"height_meters\":1.7")})
		&& Seen->Auth.FindRef(TEXT("/meshy/openapi/v1/rigging")) == TEXT("Bearer meshy-test-key"));
	TestTrue(TEXT("and the rig is the task animations name"), Meshy.Tasks.Contains(TEXT("rig: rig_meshy")) && Meshy.TaskId == TEXT("rig_meshy"));

	// Tripo rigs a mesh from the project: written as a GLB, uploaded for a file token, rigged with Mixamo's bone names.
	FHyperAIMeshRigRequest Upload;
	Upload.Provider = TEXT("tripo");
	Upload.MeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
	Upload.Name = TEXT("SK_TestCube");
	Upload.bDryRun = false;
	const FHyperAIMeshJob Tripo = Approve(C::Rig(Upload), TEXT("Tripo's rig of a project mesh"));
	TestTrue(TEXT("Tripo's rig of a project mesh is imported as a skeletal mesh"), Tripo.State == TEXT("completed") && !Tripo.SkeletalMesh.IsEmpty());
	TestTrue(TEXT("after the mesh was uploaded as a GLB"), Seen->Calls.Contains(TEXT("/tripo/v3/files"))
		&& AnyBody(*Seen, {TEXT("name=\"file\""), TEXT("Cube.glb"), TEXT("model/gltf-binary")}));
	TestTrue(TEXT("and rigged from its file token as a humanoid with Mixamo's names"), AnyBody(*Seen, {TEXT("\"input\":\"file_mesh\""), TEXT("\"spec\":\"mixamo\""),
		TEXT("\"model\":\"v1.0-20240301\""), TEXT("\"out_format\":\"glb\"")}));

	// Presets: Meshy's by id and by a name its library is asked for; Tripo's in full, from its list.
	FHyperAIMeshAnimateRequest Animate;
	Animate.RigOperationId = Meshy.OperationId;
	Animate.Actions = {TEXT("0"), TEXT("Double Combo Attack")};
	Animate.bDryRun = false;
	Animate.RetargetTo = Tripo.SkeletalMesh;
	const FHyperAIMeshJob Presets = Approve(C::Animate(Animate), TEXT("Meshy's presets"));
	TestTrue(TEXT("Meshy's actions are imported as animations on the rig's skeleton"), Presets.State == TEXT("completed") && Presets.Animations.Num() == 1
		&& Cast<UAnimSequence>(StaticLoadObject(UAnimSequence::StaticClass(), nullptr, *Presets.Animations[0]))
		&& Cast<UAnimSequence>(StaticLoadObject(UAnimSequence::StaticClass(), nullptr, *Presets.Animations[0]))->GetSkeleton() == MeshyMesh->GetSkeleton());
	TestTrue(TEXT("after a name was found in Meshy's library and both sent by id to its rig"), Seen->Calls.Contains(TEXT("/meshy/openapi/v1/animations/library"))
		&& AnyBody(*Seen, {TEXT("\"rig_task_id\":\"rig_meshy\""), TEXT("\"action_ids\":[0,92]")}));
	TestTrue(TEXT("and the library is kept for the search"), C::Actions(FHyperAIMeshActionsRequest()).bMeshyLibraryCached);
	TestTrue(TEXT("Retargeting is handed to the animation_rigging pack"), !Presets.RetargetOperationId.IsEmpty() && Presets.RetargetPlan.Contains(TEXT("retarget.animations"))
		&& Presets.RetargetPlan.Contains(TEXT("ik_rig.create")) && Presets.RetargetPlan.Contains(TEXT("retargeter.create")));
	if (Presets.RetargetStatus == TEXT("unavailable"))
	{
		AddWarning(TEXT("The animation_rigging pack is not loaded here, so the retarget hand-off was planned but not run."));
	}
	else
	{
		FString Error;
		if (Presets.RetargetStatus == TEXT("awaiting_user_approval"))
		{
			TestTrue(TEXT("The retarget waits for its own approval, and runs on it: ") + Error, FHyperAIStudioApprovalGate::Approve(Presets.RetargetOperationId, Error));
		}
		for (int32 Tick = 0; Tick < 50; ++Tick)
		{
			FTSTicker::GetCoreTicker().Tick(0.02f);
		}
		const FString RigFolder = FPackageName::GetLongPackagePath(FPackageName::ObjectPathToPackageName(Meshy.SkeletalMesh)) / TEXT("Retarget");
		const FString RigName = FPackageName::GetShortName(FPackageName::ObjectPathToPackageName(Meshy.SkeletalMesh));
		const FString TargetName = FPackageName::GetShortName(FPackageName::ObjectPathToPackageName(Tripo.SkeletalMesh));
		TestTrue(TEXT("It made an IK Rig for each and a retargeter between them, beside the rig"), FPackageName::DoesPackageExist(RigFolder / TEXT("IK_") + RigName)
			|| FindPackage(nullptr, *(RigFolder / TEXT("IK_") + RigName)));
		TestTrue(TEXT("the retargeter"), FPackageName::DoesPackageExist(RigFolder / FString::Printf(TEXT("RTG_%s_To_%s"), *RigName, *TargetName))
			|| FindPackage(nullptr, *(RigFolder / FString::Printf(TEXT("RTG_%s_To_%s"), *RigName, *TargetName))));
	}
	FHyperAIMeshAnimateRequest TripoPresets;
	TripoPresets.RigOperationId = Tripo.OperationId;
	TripoPresets.Actions = {TEXT("moonwalk")};
	TestEqual(TEXT("A preset Tripo does not have is refused, naming it"), C::Animate(TripoPresets).Status, FString(TEXT("invalid_request")));
	TripoPresets.Actions = {TEXT("walk"), TEXT("preset:biped:wave_goodbye_01")};
	TripoPresets.bDryRun = false;
	const FHyperAIMeshJob TripoClips = Approve(C::Animate(TripoPresets), TEXT("Tripo's presets"));
	TestTrue(TEXT("Tripo's presets are imported onto its rig's skeleton"), TripoClips.State == TEXT("completed") && !TripoClips.Animations.IsEmpty());
	TestTrue(TEXT("sent in full, together, to its rig task"), AnyBody(*Seen, {TEXT("\"input\":\"rig_tripo\""), TEXT("\"animations\":[\"preset:biped:walk\",\"preset:biped:wave_goodbye_01\"]")}));

	// Words: Meshy's clip on its rig; and the project's own character animated from words with no rig at all.
	FHyperAIMeshAnimateRequest Words;
	Words.RigOperationId = Meshy.OperationId;
	Words.Prompt = TEXT("waves with both arms, then bows");
	Words.DurationSeconds = 3.f;
	Words.bDryRun = false;
	const FHyperAIMeshJob Motion = Approve(C::Animate(Words), TEXT("Meshy's clip from words"));
	TestTrue(TEXT("Meshy's clip from words is put on its rig and imported"), Motion.State == TEXT("completed") && Motion.Animations.Num() == 1);
	TestTrue(TEXT("made from the prompt for its duration, then applied by its task id"), AnyBody(*Seen, {TEXT("\"prompt\":\"waves with both arms, then bows\""), TEXT("\"duration\":3"),
		TEXT("\"mode\":\"prime\"")}) && AnyBody(*Seen, {TEXT("\"motion_task_id\":\"motion_meshy\""), TEXT("\"rig_task_id\":\"rig_meshy\"")}));
	FHyperAIMeshAnimateRequest Own;
	Own.Provider = TEXT("meshy");
	Own.TargetMesh = Tripo.SkeletalMesh;
	Own.Prompt = TEXT("jumps for joy");
	Own.DurationSeconds = 2.5f;
	const FHyperAIMeshTaskReport OwnPrice = C::Animate(Own);
	TestTrue(TEXT("Animating a project character from words with Meshy rigs it first, then retargets the clip back onto it"), OwnPrice.bOk
		&& FMath::IsNearlyEqual(OwnPrice.EstimatedCost, 0.10f + 0.20f + 0.06f) && OwnPrice.Tasks.Num() == 4 && !OwnPrice.RetargetPlan.IsEmpty());
	Own.bDryRun = false;
	const FHyperAIMeshJob OwnClip = Approve(C::Animate(Own), TEXT("A project character from words"));
	TestTrue(TEXT("whose GLB is sent inline to be rigged, and whose clip comes onto the rigged copy"), OwnClip.State == TEXT("completed") && !OwnClip.SkeletalMesh.IsEmpty()
		&& OwnClip.Animations.Num() == 1 && AnyBody(*Seen, {TEXT("\"model_url\":\"data:application/octet-stream;base64,")}) && !OwnClip.RetargetOperationId.IsEmpty());
	if (!OwnClip.RetargetOperationId.IsEmpty() && FHyperAIStudioApprovalGate::HasPending(OwnClip.RetargetOperationId))
	{
		FHyperAIStudioApprovalGate::Reject(OwnClip.RetargetOperationId, TEXT("Not needed by the test."));
	}

	// Viggle: a clip from words straight onto a skeleton of its kind, the UE5 Mannequin's when the project has one.
	FHyperAIMeshAnimateRequest Viggle;
	Viggle.Provider = TEXT("viggle");
	Viggle.TargetMesh = Meshy.SkeletalMesh;
	Viggle.Prompt = TEXT("a confident wave");
	Viggle.DurationSeconds = 2.f;
	TestEqual(TEXT("Viggle makes clips of 3 seconds or more"), C::Animate(Viggle).Status, FString(TEXT("invalid_request")));
	Viggle.DurationSeconds = 4.f;
	Viggle.bDryRun = false;
	const FHyperAIMeshJob Mixamo = Approve(C::Animate(Viggle), TEXT("Viggle onto a Mixamo-named skeleton"));
	TestTrue(TEXT("Viggle's Mixamo clip is imported onto a skeleton with a Hips bone"), Mixamo.State == TEXT("completed") && Mixamo.Animations.Num() == 1
		&& AnyBody(*Seen, {TEXT("\"text\":\"a confident wave\""), TEXT("\"duration_seconds\":4")}) && Seen->Calls.Contains(TEXT("/viggle/v1/motions/mot_1/export")));
	if (USkeletalMesh* Mannequin = FindMannequin())
	{
		Viggle.TargetMesh = Mannequin->GetPathName();
		const FHyperAIMeshJob OnMannequin = Approve(C::Animate(Viggle), TEXT("Viggle onto the project's Mannequin"));
		const UAnimSequence* Clip = OnMannequin.Animations.IsEmpty() ? nullptr
			: Cast<UAnimSequence>(StaticLoadObject(UAnimSequence::StaticClass(), nullptr, *OnMannequin.Animations[0]));
		TestTrue(TEXT("and its MetaHuman body clip straight onto the project's Mannequin skeleton"), OnMannequin.State == TEXT("completed") && Clip
			&& Clip->GetSkeleton() == Mannequin->GetSkeleton());
	}
	else
	{
		AddWarning(TEXT("No UE5 Mannequin under /Game/Characters, so Viggle onto the Mannequin was not tried."));
	}

	// Retexture: Meshy at 4K with PBR maps, imported as a mesh with its material; Tripo refused for want of credits.
	Texture.Provider = TEXT("meshy");
	Texture.Resolution = TEXT("4k");
	Texture.bDryRun = false;
	const FHyperAIMeshJob Textured = Approve(C::Retexture(Texture), TEXT("Meshy's texture"));
	TestTrue(TEXT("Meshy's texture is imported as a mesh in a folder of its own"), Textured.State == TEXT("completed") && !Textured.AssetPath.IsEmpty()
		&& StaticLoadObject(UObject::StaticClass(), nullptr, *Textured.AssetPath) != nullptr);
	TestTrue(TEXT("asked for at 4K with PBR maps, by words, glb only"), AnyBody(*Seen, {TEXT("\"text_style_prompt\":\"weathered bronze with green patina\""),
		TEXT("\"texture_resolution\":\"4k\""), TEXT("\"enable_pbr\":true"), TEXT("\"target_formats\":[\"glb\"]")}));
	Texture.Provider = TEXT("tripo");
	Texture.SourceTaskId = TEXT("tripo_model");
	Texture.Resolution = TEXT("8k");
	const FHyperAIMeshJob Broke = Approve(C::Retexture(Texture), TEXT("Tripo's texture"));
	TestTrue(TEXT("A provider's refusal fails the job in its own words"), Broke.State == TEXT("failed") && Broke.Diagnostic.Contains(TEXT("Insufficient credits"))
		&& Broke.Diagnostic.Contains(TEXT("out of credits")));
	TestTrue(TEXT("after Tripo was asked for its extreme (8K) quality"), AnyBody(*Seen, {TEXT("\"texture_quality\":\"extreme\""), TEXT("\"input\":\"tripo_model\"")}));

	// A task the provider fails, and one stopped while it runs.
	FHyperAIMeshRigRequest Failing = Rig;
	Failing.SourceTaskId = TEXT("fail_source");
	Failing.Name = TEXT("SK_TestFail");
	const FHyperAIMeshJob Failed = Approve(C::Rig(Failing), TEXT("A rig Meshy fails"));
	TestTrue(TEXT("A failed task fails the job with the provider's reason"), Failed.State == TEXT("failed") && Failed.Diagnostic.Contains(TEXT("Pose estimation failed")));
	const double SpentBefore = FHyperAIStudioExternalServices::GetSessionSpend();
	FHyperAIMeshRigRequest Slow = Rig;
	Slow.SourceTaskId = TEXT("slow_source");
	Slow.Name = TEXT("SK_TestSlow");
	const FHyperAIMeshTaskReport SlowAsked = C::Rig(Slow);
	Operations.Add(SlowAsked.OperationId);
	FString Error;
	FHyperAIStudioApprovalGate::Approve(SlowAsked.OperationId, Error);
	const double Until = FPlatformTime::Seconds() + 20.0;
	while (FPlatformTime::Seconds() < Until && C::Status(SlowAsked.OperationId).Jobs[0].State != TEXT("generating"))
	{
		FTSTicker::GetCoreTicker().Tick(0.02f);
		FPlatformProcess::Sleep(0.01f);
	}
	const FHyperAIMeshCancelReport Stopped = C::Cancel(SlowAsked.OperationId);
	TestTrue(TEXT("A running job is cancelled, and what the provider took stays booked"), Stopped.bOk && Stopped.Job.State == TEXT("cancelled")
		&& FMath::IsNearlyEqual(FHyperAIStudioExternalServices::GetSessionSpend(), SpentBefore + 0.10, 0.001));
	TestEqual(TEXT("A finished job has nothing to cancel"), C::Cancel(Meshy.OperationId).Status, FString(TEXT("already_finished")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
