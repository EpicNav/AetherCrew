// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetImportTask.h"
#include "AssetCompilingManager.h"
#include "AssetToolsModule.h"
#include "Containers/Ticker.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCredentials.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioMeshGenJob.h"
#include "HyperAIStudioMeshGenToolset.h"
#include "HyperAIStudioModelCreator.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioTripoSettings.h"
#include "IAssetTools.h"
#include "IHttpRouter.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "JsonObjectConverter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonSerializer.h"

/**
 * The Tripo model creator: classification, Tripo v3 request bodies, prices, the checks on small GLB fixtures with known
 * defects, each free fix, the paid-fix ladder, the material build from PBR maps, a whole job against a stand-in Tripo
 * (never the real one) with its budget, round cap, refusals and cancel, the Tripo Studio watch, and the account read.
 * Then Hi3D: its rules, form, prices, error codes and provider choice, a whole job against a stand-in Hi3D (the token
 * flow, the 100,000-face floor reduced for free, texture only, a refunded task tried again, no key anywhere), and the
 * reference picture first, on the user's own sword brief, against a stand-in Tripo.
 */
namespace HyperAIStudio::ModelCreatorTests
{
	using namespace HyperAIStudio::MeshGen::Creator;

	// ---------------------------------------------------------------- GLB fixtures

	struct FFixture
	{
		/** Edge length in metres, and the cube's grid: faces * Grid * Grid * 2 triangles. */
		float Size = 1.f;
		int32 Grid = 8;
		bool bHole = false;
		bool bFlipOne = false;
		bool bFloating = false;
		bool bOverlapUV = false;
		bool bTextured = false;
		/** glTF metres, Y up: an offset moves the pivot off the bottom centre. */
		FVector3f Offset = FVector3f::ZeroVector;
	};

	TArray<uint8> Png(const int32 Size, TFunctionRef<FColor(int32, int32)> Pixel)
	{
		TArray<FColor> Pixels;
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				Pixels.Add(Pixel(X, Y));
			}
		}
		TArray64<uint8> Out;
		FImageUtils::CompressImage(Out, TEXT("png"), FImageView(Pixels.GetData(), Size, Size));
		return TArray<uint8>(Out.GetData(), static_cast<int32>(Out.Num()));
	}

	/** A cube of Size metres made of a Grid x Grid of quads a face (two triangles each), with the defects asked for. */
	TArray<uint8> CubeGlb(const FFixture& F)
	{
		TArray<FVector3f> P, N;
		TArray<FVector2f> UV;
		TArray<uint32> I;
		const FVector3f Axes[6][3] = {
			{FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1)}, {FVector3f(-1, 0, 0), FVector3f(0, 0, 1), FVector3f(0, 1, 0)},
			{FVector3f(0, 1, 0), FVector3f(0, 0, 1), FVector3f(1, 0, 0)}, {FVector3f(0, -1, 0), FVector3f(1, 0, 0), FVector3f(0, 0, 1)},
			{FVector3f(0, 0, 1), FVector3f(1, 0, 0), FVector3f(0, 1, 0)}, {FVector3f(0, 0, -1), FVector3f(0, 1, 0), FVector3f(1, 0, 0)}};
		auto Cube = [&](const FVector3f& Centre, const float Half, const int32 Grid, const bool bMain)
		{
			for (int32 Face = 0; Face < 6; ++Face)
			{
				if (bMain && F.bHole && Face == 0)
				{
					continue;
				}
				const FVector3f& Normal = Axes[Face][0];
				const FVector3f& U = Axes[Face][1];
				const FVector3f& V = Axes[Face][2];
				const uint32 Base = P.Num();
				const FVector2f Cell(float(Face % 3) / 3.f, float(Face / 3) / 2.f);
				for (int32 B = 0; B <= Grid; ++B)
				{
					for (int32 A = 0; A <= Grid; ++A)
					{
						const float FA = float(A) / Grid, FB = float(B) / Grid;
						P.Add(Centre + Normal * Half + U * (FA * 2.f * Half - Half) + V * (FB * 2.f * Half - Half));
						N.Add(Normal);
						UV.Add(F.bOverlapUV ? FVector2f(FA, FB) : Cell + FVector2f(0.01f + FA * 0.313f, 0.01f + FB * 0.48f));
					}
				}
				for (int32 B = 0; B < Grid; ++B)
				{
					for (int32 A = 0; A < Grid; ++A)
					{
						const uint32 V00 = Base + B * (Grid + 1) + A, V10 = V00 + 1, V01 = V00 + Grid + 1, V11 = V01 + 1;
						const bool bFlip = bMain && F.bFlipOne && Face == 2 && A == 0 && B == 0;
						I.Append(bFlip ? TArray<uint32>{V00, V11, V10} : TArray<uint32>{V00, V10, V11});
						I.Append({V00, V11, V01});
					}
				}
			}
		};
		Cube(FVector3f(0.f, F.Size * 0.5f, 0.f) + F.Offset, F.Size * 0.5f, F.Grid, true);
		if (F.bFloating)
		{
			Cube(FVector3f(F.Size * 0.3f, F.Size * 0.5f, 0.f) + F.Offset + FVector3f(0.f, 0.f, F.Size * 0.9f), F.Size * 0.02f, 1, false);
		}

		TArray<uint8> Bin;
		auto Align = [&Bin]() { while (Bin.Num() % 4) Bin.Add(0); };
		auto Append = [&Bin, &Align](const void* Data, const int32 Bytes) { Align(); const int32 Offset = Bin.Num(); Bin.Append(static_cast<const uint8*>(Data), Bytes); return Offset; };
		const int32 PO = Append(P.GetData(), P.Num() * 12), NO = Append(N.GetData(), N.Num() * 12), UO = Append(UV.GetData(), UV.Num() * 8), IO = Append(I.GetData(), I.Num() * 4);
		FVector3f Min(FLT_MAX), Max(-FLT_MAX);
		for (const FVector3f& X : P)
		{
			Min = FVector3f::Min(Min, X);
			Max = FVector3f::Max(Max, X);
		}
		FString Views = FString::Printf(TEXT(R"({"buffer":0,"byteOffset":%d,"byteLength":%d,"target":34962},{"buffer":0,"byteOffset":%d,"byteLength":%d,"target":34962},)")
			TEXT(R"({"buffer":0,"byteOffset":%d,"byteLength":%d,"target":34962},{"buffer":0,"byteOffset":%d,"byteLength":%d,"target":34963})"), PO, P.Num() * 12, NO, N.Num() * 12, UO, UV.Num() * 8, IO, I.Num() * 4);
		FString Images, Material, Textures;
		if (F.bTextured)
		{
			const TArray<uint8> Color = Png(64, [](int32 X, int32 Y) { return ((X / 8 + Y / 8) % 2) ? FColor(180, 120, 60) : FColor(90, 60, 30); });
			// glTF packs roughness in green and metal in blue: rough 200, no metal.
			const TArray<uint8> MR = Png(64, [](int32, int32) { return FColor(0, 200, 0); });
			const TArray<uint8> Normal = Png(64, [](int32, int32) { return FColor(128, 128, 255); });
			const int32 CO = Append(Color.GetData(), Color.Num()), MO = Append(MR.GetData(), MR.Num()), NMO = Append(Normal.GetData(), Normal.Num());
			Views += FString::Printf(TEXT(R"(,{"buffer":0,"byteOffset":%d,"byteLength":%d},{"buffer":0,"byteOffset":%d,"byteLength":%d},{"buffer":0,"byteOffset":%d,"byteLength":%d})"),
				CO, Color.Num(), MO, MR.Num(), NMO, Normal.Num());
			Images = TEXT(R"(,"images":[{"bufferView":4,"mimeType":"image/png","name":"T_BaseColor"},{"bufferView":5,"mimeType":"image/png","name":"T_MetallicRoughness"},{"bufferView":6,"mimeType":"image/png","name":"T_Normal"}])");
			Textures = TEXT(R"(,"textures":[{"source":0},{"source":1},{"source":2}])");
			Material = TEXT(R"(,"materials":[{"name":"M_Fixture","pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":1}},"normalTexture":{"index":2}}])");
		}
		Align();
		FString Json = FString::Printf(TEXT(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0,"name":"Fixture"}],)")
			TEXT(R"("meshes":[{"name":"Fixture","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3%s}]}],)")
			TEXT(R"("buffers":[{"byteLength":%d}],"bufferViews":[%s],)")
			TEXT(R"("accessors":[{"bufferView":0,"componentType":5126,"count":%d,"type":"VEC3","min":[%f,%f,%f],"max":[%f,%f,%f]},)")
			TEXT(R"({"bufferView":1,"componentType":5126,"count":%d,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":%d,"type":"VEC2"},)")
			TEXT(R"({"bufferView":3,"componentType":5125,"count":%d,"type":"SCALAR"}]%s%s%s})"),
			F.bTextured ? TEXT(R"(,"material":0)") : TEXT(""), Bin.Num(), *Views, P.Num(), Min.X, Min.Y, Min.Z, Max.X, Max.Y, Max.Z, N.Num(), UV.Num(), I.Num(), *Material, *Textures, *Images);
		while (Json.Len() % 4 != 0)
		{
			Json.AppendChar(TEXT(' '));
		}
		const FTCHARToUTF8 Text(*Json);
		TArray<uint8> Glb;
		auto Word = [&Glb](const uint32 Value) { Glb.Append(reinterpret_cast<const uint8*>(&Value), 4); };
		Word(0x46546C67u);
		Word(2);
		Word(12 + 8 + Text.Length() + 8 + Bin.Num());
		Word(Text.Length());
		Word(0x4E4F534Au);
		Glb.Append(reinterpret_cast<const uint8*>(Text.Get()), Text.Length());
		Word(Bin.Num());
		Word(0x004E4942u);
		Glb.Append(Bin);
		return Glb;
	}

	const TCHAR* const TestFolder = TEXT("/Game/HyperAITests/Creator");

	/** Imports a fixture as HyperAI's own jobs do; the first static mesh it makes. */
	UStaticMesh* Import(const TArray<uint8>& Glb, const FString& Name, TArray<FString>& OutImported, const FString& Folder = TestFolder, const bool bOwn = true)
	{
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests/Creator") / Name + TEXT(".glb"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
		FFileHelper::SaveArrayToFile(Glb, *File);
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = Folder;
		Task->DestinationName = Name;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = false;
		{
			TOptional<FOwnImport> Own;
			if (bOwn)
			{
				Own.Emplace();
			}
			FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks({Task});
		}
		OutImported.Append(Task->ImportedObjectPaths);
		for (const FString& Path : Task->ImportedObjectPaths)
		{
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(StaticLoadObject(UObject::StaticClass(), nullptr, *Path)))
			{
				return Mesh;
			}
		}
		return nullptr;
	}

	const FHyperAIModelCheck* Find(const FHyperAIModelCheckReport& Report, const TCHAR* Id)
	{
		return Report.Checks.FindByPredicate([Id](const FHyperAIModelCheck& Check) { return Check.Id == Id; });
	}

	FString ResultOf(const FHyperAIModelCheckReport& Report, const TCHAR* Id)
	{
		const FHyperAIModelCheck* Check = Find(Report, Id);
		return Check ? Check->Result : FString(TEXT("missing"));
	}

	void DeleteAll(const TArray<FString>& Paths)
	{
		// Meshes and materials first; then, with every compile finished, the textures they used. Deleting a texture
		// a material still points at re-queues its compile mid-delete, which the engine's compile manager trips on.
		TArray<UObject*> Objects;
		TArray<UObject*> Textures;
		FAssetCompilingManager::Get().FinishAllCompilation();
		for (const FString& Path : Paths)
		{
			if (UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				(Cast<UTexture>(Object) ? Textures : Objects).AddUnique(Object);
			}
		}
		if (!Objects.IsEmpty())
		{
			ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
		}
		FAssetCompilingManager::Get().FinishAllCompilation();
		if (!Textures.IsEmpty())
		{
			ObjectTools::ForceDeleteObjects(Textures, /*ShowConfirmation=*/false);
		}
	}

	FString BodyText(const FHttpServerRequest& Request)
	{
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		return FString(Text.Length(), Text.Get());
	}

	TSharedPtr<FJsonObject> Parse(const FString& Text)
	{
		TSharedPtr<FJsonObject> Json;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json);
		return Json;
	}

	/** Points Tripo at the stand-in and gives it a key only the test sets; undone when it goes out of scope. */
	struct FStandIn
	{
		TArray<FHyperAIStudioExternalServiceSettings> SavedServices;
		FString SavedFolder;
		float SavedCap = 0.f;
		TArray<FHyperAIStudioMeshTaskPrice> SavedPrices;

		explicit FStandIn(const FString& Base)
		{
			UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
			SavedServices = Settings->ExternalServices;
			SavedFolder = Settings->MeshImportFolder;
			SavedCap = Settings->ExternalServiceSessionSpendCap;
			SavedPrices = Settings->MeshTaskPrices;
			for (FHyperAIStudioExternalServiceSettings Service : UHyperAIStudioSettings::GetMeshServices())
			{
				if (Service.Id == TEXT("tripo"))
				{
					Service.BaseUrl = Base + TEXT("/tripo/v3");
					Service.ApiKeyVariable = TEXT("HYPERAI_TEST_CREATOR_TRIPO_KEY");
					Settings->ExternalServices.Add(Service);
				}
				// Hi3D at the stand-in too, with a key only a Hi3D test sets: no test reaches the real Hi3D.
				if (Service.Id == TEXT("hi3d"))
				{
					Service.BaseUrl = Base + TEXT("/hi3d");
					Service.ApiKeyVariable = TEXT("HYPERAI_TEST_CREATOR_HI3D_KEY");
					Settings->ExternalServices.Add(Service);
				}
			}
			Settings->MeshImportFolder = TestFolder;
			Settings->ExternalServiceSessionSpendCap = 100.f;
			FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_TRIPO_KEY"), TEXT("tripo-creator-test-key"));
			FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_HI3D_KEY"), TEXT(""));
			FHyperAIStudioMeshGenContracts::SetPollSecondsForTests(0.1);
			FHyperAIStudioExternalServices::ResetSessionSpend();
			HyperAIStudio::MeshGen::Private::RetryScale() = 0.01;
			ForgetHi3DTokenForTests();
		}

		~FStandIn()
		{
			UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
			Settings->ExternalServices = SavedServices;
			Settings->MeshImportFolder = SavedFolder;
			Settings->ExternalServiceSessionSpendCap = SavedCap;
			Settings->MeshTaskPrices = SavedPrices;
			FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_TRIPO_KEY"), TEXT(""));
			FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_HI3D_KEY"), TEXT(""));
			FHyperAIStudioMeshGenContracts::SetPollSecondsForTests(0.0);
			FHyperAIStudioExternalServices::ResetSessionSpend();
			HyperAIStudio::MeshGen::Private::RetryScale() = 1.0;
			ForgetHi3DTokenForTests();
		}
	};

	/** Ticks the editor until Done says so, or Seconds pass. */
	bool TickUntil(TFunctionRef<bool()> Done, const double Seconds = 30.0)
	{
		const double End = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.02f);
			TickJobs();
			FPlatformProcess::Sleep(0.01f);
			if (Done())
			{
				return true;
			}
		}
		return false;
	}

	FHyperAIModelJob JobOf(const FString& OperationId)
	{
		const FHyperAIModelStatusReport Status = HyperAIStudio::MeshGen::Creator::Status(OperationId);
		return Status.Jobs.IsEmpty() ? FHyperAIModelJob() : Status.Jobs[0];
	}
}

// ---------------------------------------------------------------- classification

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelCreatorClassifyTest, "HyperAIStudio.MeshGen.Creator.Classify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioModelCreatorClassifyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelCreatorTests;
	struct FRow
	{
		const TCHAR* Prompt;
		const TCHAR* Use;
		const TCHAR* Platform;
		const TCHAR* Model;
		int32 Faces;
		const TCHAR* Topology;
		const TCHAR* Parts;
		// Expected.
		const TCHAR* Family;
		const TCHAR* UseOut;
		bool bQuad;
		int32 FaceLimit;
		const TCHAR* Texture;
		bool bParts;
		bool bSmartLowPoly;
		bool bNanite;
	};
	const FRow Rows[] = {
		{TEXT("an ornate golden throne"), TEXT("hero_prop"), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("hero_prop"), false, 300000, TEXT("extreme"), false, false, true},
		{TEXT("a wooden crate"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("P2"), TEXT("prop"), false, 20000, TEXT("standard"), false, false, false},
		{TEXT("a sci-fi soldier in armour"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("P2"), TEXT("character"), true, 20000, TEXT("detailed"), false, false, false},
		{TEXT("a sci-fi soldier in armour"), TEXT(""), TEXT("mobile"), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("P2"), TEXT("character"), true, 5000, TEXT("standard"), false, false, false},
		{TEXT("a mossy boulder"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("environment"), false, 200000, TEXT("detailed"), false, false, true},
		{TEXT("a stone wall section"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("P2"), TEXT("kit"), false, 30000, TEXT("standard"), false, false, false},
		{TEXT("a rusty pickup truck"), TEXT(""), TEXT("vr"), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("vehicle"), false, 75000, TEXT("detailed"), false, false, true},
		{TEXT("a low poly pine tree"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("P2"), TEXT("foliage"), false, 8000, TEXT("standard"), false, false, true},
		{TEXT("a low poly treasure chest"), TEXT("prop"), TEXT(""), TEXT("H3.1"), 0, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("prop"), false, 20000, TEXT("standard"), false, true, false},
		{TEXT("a robot with separate parts"), TEXT("prop"), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("prop"), false, 20000, TEXT("standard"), true, false, false},
		// Detail on a light use is four times its budget, not a flat 500K; a dense static weapon turns Nanite on.
		{TEXT("an ornate engraved longsword"), TEXT("weapon"), TEXT(""), TEXT("H3.1"), 0, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("weapon"), false, 60000, TEXT("extreme"), false, false, false},
		{TEXT("a longsword"), TEXT("weapon"), TEXT(""), TEXT("H3.1"), 200000, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("weapon"), false, 200000, TEXT("detailed"), false, false, true},
		{TEXT("a medieval sword"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT("quads"), TEXT(""), TEXT("P2"), TEXT("weapon"), true, 7500, TEXT("detailed"), false, false, false},
		{TEXT("a distant mountain"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("P2"), TEXT("background"), false, 5000, TEXT("standard"), false, false, false},
		{TEXT("a gem"), TEXT(""), TEXT(""), TEXT("P2"), 1200, TEXT(""), TEXT(""), TEXT("P2"), TEXT("miniature"), false, 1200, TEXT("standard"), false, false, false},
		{TEXT("a huge cathedral"), TEXT("environment"), TEXT(""), TEXT("H3.1"), 1800000, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("environment"), false, 1800000, TEXT("detailed"), false, false, true},
		// The user's caps: H3.1 quads stop at 50,000, P2 quads at 25,000.
		{TEXT("a creature"), TEXT("creature"), TEXT(""), TEXT("H3.1"), 0, TEXT("quads"), TEXT(""), TEXT("H3.1"), TEXT("creature"), true, 25000, TEXT("detailed"), false, false, false},
	};
	for (const FRow& Row : Rows)
	{
		FHyperAIModelBrief Brief;
		Brief.Prompt = Row.Prompt;
		Brief.Use = Row.Use;
		Brief.Platform = Row.Platform;
		Brief.Model = Row.Model;
		Brief.FaceLimit = Row.Faces;
		Brief.Topology = Row.Topology;
		Brief.Parts = Row.Parts;
		FHyperAIModelPlan Plan;
		FString Error;
		const FString Name = FString::Printf(TEXT("\"%s\" (%s)"), Row.Prompt, *FString::Join(TArray<FString>{Row.Use, Row.Platform, Row.Model}, TEXT(",")));
		if (!TestTrue(Name + TEXT(" classifies: ") + Error, Classify(Brief, Plan, Error)))
		{
			continue;
		}
		TestEqual(Name + TEXT(" family"), Plan.Family, FString(Row.Family));
		TestEqual(Name + TEXT(" use"), Plan.Use, FString(Row.UseOut));
		TestEqual(Name + TEXT(" quads"), Plan.bQuad, Row.bQuad);
		TestEqual(Name + TEXT(" faces"), Plan.FaceLimit, Row.FaceLimit);
		TestEqual(Name + TEXT(" texture tier"), Plan.TextureQuality, FString(Row.Texture));
		TestEqual(Name + TEXT(" parts"), Plan.bParts, Row.bParts);
		TestEqual(Name + TEXT(" smart low poly"), Plan.bSmartLowPoly, Row.bSmartLowPoly);
		TestEqual(Name + TEXT(" Nanite"), Plan.bNanite, Row.bNanite);
		TestTrue(Name + TEXT(" explains every choice"), Plan.Reasons.Num() >= 6 && Plan.Model == (Plan.Family == TEXT("P2") ? FString(P2Model) : FString(H31Model)));
		TestTrue(Name + TEXT(" stays in its family's range"), Plan.FaceLimit >= Plan.MinFaces && Plan.FaceLimit <= Plan.MaxFaces);
	}
	FHyperAIModelBrief Ultra;
	Ultra.Prompt = TEXT("a cathedral");
	Ultra.Model = TEXT("H3.1");
	Ultra.FaceLimit = 1800000;
	FHyperAIModelPlan Plan;
	FString Error;
	TestTrue(TEXT("Above 1.5M triangles H3.1 needs detailed geometry, so it is chosen"), Classify(Ultra, Plan, Error) && Plan.GeometryQuality == TEXT("detailed"));
	TestTrue(TEXT("Parts are made untextured, their texture task priced in the plan"), [&]()
	{
		FHyperAIModelBrief Parts;
		Parts.Prompt = TEXT("a robot");
		Parts.Parts = TEXT("on");
		FHyperAIModelPlan Out;
		FString Why;
		return Classify(Parts, Out, Why) && Out.bParts && Out.Family == TEXT("H3.1") && Out.PriceLines.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("texture task")); })
			&& Out.RequestJson.Contains(TEXT("\"generate_parts\":true")) && Out.RequestJson.Contains(TEXT("\"texture\":false"));
	}());

	// Overrides that cannot be honoured are refused, each with a reason.
	struct FRefusal
	{
		const TCHAR* Model;
		const TCHAR* Parts;
		const TCHAR* LowPoly;
		const TCHAR* Geometry;
		int32 Faces;
		const TCHAR* Topology;
		const TCHAR* Texture;
		const TCHAR* Pbr;
		const TCHAR* Use;
		const TCHAR* Contains;
	};
	const FRefusal Refusals[] = {
		{TEXT("P2"), TEXT("on"), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT("H3.1 options")},
		{TEXT("P2"), TEXT(""), TEXT("on"), TEXT(""), 0, TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT("H3.1 options")},
		{TEXT("P2"), TEXT(""), TEXT(""), TEXT("detailed"), 0, TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT("Ultra")},
		{TEXT("P2"), TEXT(""), TEXT(""), TEXT(""), 60000, TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT("outside")},
		{TEXT("P2"), TEXT(""), TEXT(""), TEXT(""), 30000, TEXT("quads"), TEXT(""), TEXT(""), TEXT(""), TEXT("outside")},
		{TEXT("H3.1"), TEXT(""), TEXT(""), TEXT(""), 60000, TEXT("quads"), TEXT(""), TEXT(""), TEXT(""), TEXT("outside")},
		{TEXT("H3.1"), TEXT(""), TEXT(""), TEXT(""), 400, TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT("outside")},
		{TEXT("H3.1"), TEXT(""), TEXT(""), TEXT("standard"), 1800000, TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT("geometry_quality detailed")},
		{TEXT("H4"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT("model is")},
		{TEXT(""), TEXT(""), TEXT(""), TEXT(""), 0, TEXT("ngons"), TEXT(""), TEXT(""), TEXT(""), TEXT("topology")},
		{TEXT(""), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT("none"), TEXT("on"), TEXT(""), TEXT("pbr needs textures")},
		{TEXT(""), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT("ultra"), TEXT(""), TEXT(""), TEXT("texture_quality")},
		{TEXT(""), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT(""), TEXT("spaceship_interior"), TEXT("use is")},
	};
	for (const FRefusal& Row : Refusals)
	{
		FHyperAIModelBrief Brief;
		Brief.Prompt = TEXT("a thing");
		Brief.Model = Row.Model;
		Brief.Parts = Row.Parts;
		Brief.SmartLowPoly = Row.LowPoly;
		Brief.GeometryQuality = Row.Geometry;
		Brief.FaceLimit = Row.Faces;
		Brief.Topology = Row.Topology;
		Brief.TextureQuality = Row.Texture;
		Brief.Pbr = Row.Pbr;
		Brief.Use = Row.Use;
		FHyperAIModelPlan Out;
		FString Why;
		TestTrue(FString::Printf(TEXT("Refused (%s): %s"), Row.Contains, *Why), !Classify(Brief, Out, Why) && Why.Contains(Row.Contains));
	}
	FHyperAIModelBrief Empty;
	TestFalse(TEXT("A brief with no words and no picture is refused"), Classify(Empty, Plan, Error));
	return true;
}

// ---------------------------------------------------------------- request shapes and prices

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelCreatorRequestsTest, "HyperAIStudio.MeshGen.Creator.RequestsAndPrices",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioModelCreatorRequestsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelCreatorTests;
	FHyperAIModelBrief Brief;
	Brief.Prompt = TEXT("a wooden crate with iron bands");
	Brief.ReferenceImage = TEXT("off");
	Brief.Model = TEXT("H3.1");
	Brief.Use = TEXT("prop");
	Brief.Topology = TEXT("quads");
	Brief.Seed = 42;
	FHyperAIModelPlan Plan;
	FString Error;
	TestTrue(TEXT("H3.1 quads classifies"), Classify(Brief, Plan, Error));
	TSharedRef<FJsonObject> Body = GenerationBody(Plan, Brief, {});
	TestEqual(TEXT("text-to-model goes to its own v3 endpoint"), Plan.Endpoint, FString(TEXT("/generation/text-to-model")));
	TestEqual(TEXT("with the model id"), Body->GetStringField(TEXT("model")), FString(TEXT("v3.1-20260211")));
	TestEqual(TEXT("the prompt"), Body->GetStringField(TEXT("prompt")), Brief.Prompt);
	TestTrue(TEXT("a negative prompt for game assets"), Body->GetStringField(TEXT("negative_prompt")).Contains(TEXT("floating parts")));
	TestTrue(TEXT("quad, face_limit, PBR, texture version, geometry quality, seed, auto size, UVs"), Body->GetBoolField(TEXT("quad")) && Body->GetIntegerField(TEXT("face_limit")) == Plan.FaceLimit
		&& Body->GetBoolField(TEXT("pbr")) && Body->GetStringField(TEXT("texture_version")) == TEXT("v3.5-20260815") && Body->GetStringField(TEXT("geometry_quality")) == TEXT("standard")
		&& Body->GetIntegerField(TEXT("model_seed")) == 42 && Body->GetBoolField(TEXT("auto_size")) && Body->GetBoolField(TEXT("export_uv")) && !Body->HasField(TEXT("type")));

	FHyperAIModelBrief Picture;
	Picture.ImagePath = TEXT("Saved/x.png");
	Picture.Model = TEXT("P2");
	TestTrue(TEXT("P2 from a picture classifies"), Classify(Picture, Plan, Error));
	Body = GenerationBody(Plan, Picture, {TEXT("file_abc")});
	TestTrue(TEXT("image-to-model takes the file token as input, keeps the picture's colours, and sends no H3.1-only field"),
		Plan.Endpoint == TEXT("/generation/image-to-model") && Body->GetStringField(TEXT("input")) == TEXT("file_abc") && Body->GetStringField(TEXT("texture_alignment")) == TEXT("original_image")
		&& Body->GetStringField(TEXT("model")) == TEXT("P2-20260801") && !Body->HasField(TEXT("geometry_quality")) && !Body->HasField(TEXT("smart_low_poly")) && !Body->HasField(TEXT("generate_parts"))
		&& !Body->HasField(TEXT("prompt")));

	FHyperAIModelBrief Views;
	Views.MultiviewImages = {TEXT("Saved/f.png"), TEXT(""), TEXT("Saved/b.png"), TEXT("Saved/r.png")};
	TestTrue(TEXT("Multiview classifies"), Classify(Views, Plan, Error));
	Body = GenerationBody(Plan, Views, {TEXT("tf"), TEXT(""), TEXT("tb"), TEXT("tr")});
	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	TestTrue(TEXT("multiview-to-model takes view-key inputs, leaving the skipped view out"), Plan.Endpoint == TEXT("/generation/multiview-to-model")
		&& Body->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs->Num() == 3 && (*Inputs)[0]->AsObject()->GetStringField(TEXT("front")) == TEXT("tf")
		&& (*Inputs)[1]->AsObject()->GetStringField(TEXT("back")) == TEXT("tb"));
	Views.MultiviewImages = {TEXT(""), TEXT("Saved/l.png")};
	TestFalse(TEXT("Multiview without its front view is refused"), Classify(Views, Plan, Error));

	// Edited views: four views made from the picture, edited, then the model made from them.
	FHyperAIModelBrief Edited;
	Edited.ImagePath = TEXT("Saved/x.png");
	Edited.Model = TEXT("H3.1");
	Edited.ViewEdits = {TEXT("back: add a quiver of arrows"), TEXT("front: red cloak")};
	TestTrue(TEXT("View edits on a picture classify as multiview, views made first: ") + Error, Classify(Edited, Plan, Error) && Plan.Input == TEXT("multiview")
		&& Plan.bMakeViews && Plan.ViewEdits.Num() == 2 && Plan.Endpoint == TEXT("/generation/multiview-to-model")
		&& Plan.PriceLines.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("image-to-multiview")); })
		&& Plan.PriceLines.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("edit-multiview")); }));
	FHyperAIModelBrief FromTask = Edited;
	FromTask.ImagePath.Reset();
	FromTask.MultiviewTaskId = TEXT("mv_7");
	TestTrue(TEXT("View edits on a multiview task edit it, no views made"), Classify(FromTask, Plan, Error) && !Plan.bMakeViews && Plan.ViewEdits.Num() == 2);
	Body = GenerationBody(Plan, FromTask, {});
	TestTrue(TEXT("and the model is made from that task's views"), Body->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs->Num() == 1
		&& (*Inputs)[0]->AsObject()->GetStringField(TEXT("task_id")) == TEXT("mv_7"));
	FHyperAIModelBrief NoSource;
	NoSource.Prompt = TEXT("a knight");
	NoSource.ViewEdits = {TEXT("back: a cape")};
	TestFalse(TEXT("View edits with nothing to edit are refused"), Classify(NoSource, Plan, Error));
	Edited.ViewEdits = {TEXT("top: a hat")};
	TestFalse(TEXT("A view that is not front, left, back or right is refused"), Classify(Edited, Plan, Error));
	Edited.ViewEdits = {TEXT("back: a"), TEXT("back: b")};
	TestFalse(TEXT("The same view twice is refused"), Classify(Edited, Plan, Error));

	Body = RetopoBody(TEXT("task_1"), 30000, false, true);
	TestTrue(TEXT("retopology: /mesh/decimate v3.0, held to 20,000 triangles, smart UV and bake"), Body->GetStringField(TEXT("model")) == TEXT("v3.0")
		&& Body->GetIntegerField(TEXT("face_limit")) == 20000 && Body->GetBoolField(TEXT("smart_uv")) && Body->GetBoolField(TEXT("bake")) && !Body->GetBoolField(TEXT("quad")));
	Body = RetopoBody(TEXT("task_1"), 30000, true, false);
	TestEqual(TEXT("retopology in quads is held to 10,000"), Body->GetIntegerField(TEXT("face_limit")), 10000);
	Body = SmartUvBody(TEXT("task_1"));
	TestTrue(TEXT("smart UV: /mesh/uv with bake"), Body->GetStringField(TEXT("input")) == TEXT("task_1") && Body->GetBoolField(TEXT("bake")) && Body->GetStringField(TEXT("model")) == TEXT("default"));
	Body = TextureBody(TEXT("task_1"), TEXT("detailed"), true, {TEXT("tripo_part_2")}, TEXT("worn leather"));
	const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
	TestTrue(TEXT("texture: part_names, quality, pbr, texture model, prompt"), Body->TryGetArrayField(TEXT("part_names"), Parts) && Parts->Num() == 1
		&& Body->GetStringField(TEXT("texture_quality")) == TEXT("detailed") && Body->GetBoolField(TEXT("pbr")) && Body->GetStringField(TEXT("model")) == TEXT("v3.5-20260815")
		&& Body->GetObjectField(TEXT("texture_prompt"))->GetStringField(TEXT("text")) == TEXT("worn leather"));
	FConvertOptions Options;
	Options.bPivotToCenterBottom = true;
	Options.bFlattenBottom = true;
	Options.ScaleFactor = 0.5f;
	Options.bQuad = true;
	Options.FaceLimit = 8000;
	Body = ConvertBody(TEXT("task_1"), Options);
	TestTrue(TEXT("convert: FBX for quads, pivot, flatten, scale, face limit, bake_scale preset"), Body->GetStringField(TEXT("format")) == TEXT("FBX") && Body->GetBoolField(TEXT("quad"))
		&& Body->GetBoolField(TEXT("pivot_to_center_bottom")) && Body->GetBoolField(TEXT("flatten_bottom")) && FMath::IsNearlyEqual(Body->GetNumberField(TEXT("scale_factor")), 0.5)
		&& Body->GetIntegerField(TEXT("face_limit")) == 8000 && Body->GetStringField(TEXT("fbx_preset")) == TEXT("bake_scale"));
	Body = SegmentBody(TEXT("task_1"));
	TestTrue(TEXT("segment: v2.0 semantic"), Body->GetStringField(TEXT("model")) == TEXT("v2.0-20260430") && Body->GetStringField(TEXT("segmentation_granularity")) == TEXT("balanced"));
	Body = CompleteBody(TEXT("task_seg"), {}, true);
	TestTrue(TEXT("complete: the segmentation's task, quick cap"), Body->GetStringField(TEXT("input")) == TEXT("task_seg") && Body->GetStringField(TEXT("completion_mode")) == TEXT("quick_cap"));
	Body = CompleteBody(TEXT("task_seg"), {TEXT("tripo_part_0")}, false);
	TestTrue(TEXT("complete: AI completion of named parts"), Body->GetStringField(TEXT("completion_mode")) == TEXT("ai_completion") && Body->HasField(TEXT("part_names")));
	Body = EditMultiviewBody(TEXT("mv_1"), {{TEXT("front"), TEXT("red shirt")}, {TEXT("back"), TEXT("add a logo")}});
	const TArray<TSharedPtr<FJsonValue>>* Prompts = nullptr;
	TestTrue(TEXT("edit-multiview: {prompt, view} edits on the multiview task"), Body->GetStringField(TEXT("input")) == TEXT("mv_1") && Body->TryGetArrayField(TEXT("prompts"), Prompts)
		&& Prompts->Num() == 2 && (*Prompts)[1]->AsObject()->GetStringField(TEXT("view")) == TEXT("back"));
	TestEqual(TEXT("image-to-multiview: one input"), ImageToMultiviewBody(TEXT("file_x"))->GetStringField(TEXT("input")), FString(TEXT("file_x")));
	FString RigModel, RigSpec;
	RigChoice(TEXT("biped"), RigModel, RigSpec);
	Body = RigBody(TEXT("task_1"), RigModel, TEXT("biped"), RigSpec);
	TestTrue(TEXT("rig: a biped gets the v3.0 rig model and spec unreal"), Body->GetStringField(TEXT("model")) == TEXT("v3.0-20260909") && Body->GetStringField(TEXT("spec")) == TEXT("unreal")
		&& Body->GetStringField(TEXT("rig_type")) == TEXT("biped"));
	RigChoice(TEXT("quadruped"), RigModel, RigSpec);
	Body = RigBody(TEXT("task_1"), RigModel, TEXT("quadruped"), RigSpec);
	TestTrue(TEXT("any other body v2.5 with its rig_type and no spec"), Body->GetStringField(TEXT("model")) == TEXT("v2.5-20260210") && !Body->HasField(TEXT("spec"))
		&& Body->GetStringField(TEXT("rig_type")) == TEXT("quadruped"));
	TestEqual(TEXT("the rig check takes the input alone"), RigCheckBody(TEXT("task_1"))->GetStringField(TEXT("input")), FString(TEXT("task_1")));

	// Prices, from MeshTaskPrices, which the user can correct.
	auto PriceOf = [&](const TCHAR* Model, const TCHAR* Prompt, const TCHAR* Image, const TCHAR* Tier, const TCHAR* Topology, const TCHAR* Geometry, const TCHAR* Parts, const TCHAR* LowPoly)
	{
		FHyperAIModelBrief B;
		B.Prompt = Prompt;
		B.ImagePath = Image;
		B.Model = Model;
		B.TextureQuality = Tier;
		B.Topology = Topology;
		B.GeometryQuality = Geometry;
		B.Parts = Parts;
		B.SmartLowPoly = LowPoly;
		B.Use = TEXT("prop");
		B.ReferenceImage = TEXT("off");
		FHyperAIModelPlan Out;
		FString Why;
		return Classify(B, Out, Why) ? Out.Credits : -1;
	};
	TestEqual(TEXT("H3.1 text, no texture: 10"), PriceOf(TEXT("H3.1"), TEXT("a cup"), TEXT(""), TEXT("none"), TEXT(""), TEXT(""), TEXT(""), TEXT("")), 10);
	TestEqual(TEXT("H3.1 text, standard: 20"), PriceOf(TEXT("H3.1"), TEXT("a cup"), TEXT(""), TEXT("standard"), TEXT(""), TEXT(""), TEXT(""), TEXT("")), 20);
	TestEqual(TEXT("H3.1 text, fast costs as standard: 20"), PriceOf(TEXT("H3.1"), TEXT("a cup"), TEXT(""), TEXT("fast"), TEXT(""), TEXT(""), TEXT(""), TEXT("")), 20);
	TestEqual(TEXT("H3.1 text, extreme: 40"), PriceOf(TEXT("H3.1"), TEXT("a cup"), TEXT(""), TEXT("extreme"), TEXT(""), TEXT(""), TEXT(""), TEXT("")), 40);
	TestEqual(TEXT("H3.1 picture, detailed: 40"), PriceOf(TEXT("H3.1"), TEXT(""), TEXT("Saved/x.png"), TEXT("detailed"), TEXT(""), TEXT(""), TEXT(""), TEXT("")), 40);
	TestEqual(TEXT("H3.1 picture, standard + quads (5) + detailed geometry (20): 55"),
		PriceOf(TEXT("H3.1"), TEXT(""), TEXT("Saved/x.png"), TEXT("standard"), TEXT("quads"), TEXT("detailed"), TEXT(""), TEXT("")), 55);
	TestEqual(TEXT("H3.1 text + smart low poly (10): 30"), PriceOf(TEXT("H3.1"), TEXT("a cup"), TEXT(""), TEXT("standard"), TEXT(""), TEXT(""), TEXT(""), TEXT("on")), 30);
	TestEqual(TEXT("H3.1 text parts: untextured 10 + parts 20 + texture task 10: 40"), PriceOf(TEXT("H3.1"), TEXT("a cup"), TEXT(""), TEXT("standard"), TEXT(""), TEXT(""), TEXT("on"), TEXT("")), 40);
	TestEqual(TEXT("P2, none: 100"), PriceOf(TEXT("P2"), TEXT("a cup"), TEXT(""), TEXT("none"), TEXT(""), TEXT(""), TEXT(""), TEXT("")), 100);
	TestEqual(TEXT("P2, extreme: 130"), PriceOf(TEXT("P2"), TEXT("a cup"), TEXT(""), TEXT("extreme"), TEXT(""), TEXT(""), TEXT(""), TEXT("")), 130);
	TestEqual(TEXT("Paid steps: retopo 35, quads 40, smart UV 20, segment 40, complete 50, quick cap 30, convert 5/10, rig 25"),
		FString::Printf(TEXT("%d %d %d %d %d %d %d %d %d"), Credits(Price(TEXT("retopo"))), Credits(Price(TEXT("retopo_quad"))), Credits(Price(TEXT("smart_uv"))), Credits(Price(TEXT("segment"))),
			Credits(Price(TEXT("complete"))), Credits(Price(TEXT("complete_quick"))), Credits(Price(TEXT("convert"))), Credits(Price(TEXT("convert_advanced"))), Credits(Price(TEXT("rig")))),
		FString(TEXT("35 40 20 40 50 30 5 10 25")));
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FHyperAIStudioMeshTaskPrice> Saved = Settings->MeshTaskPrices;
	FHyperAIStudioMeshTaskPrice Own;
	Own.Provider = TEXT("tripo");
	Own.Task = TEXT("p2");
	Own.Cost = 0.90f;
	Settings->MeshTaskPrices.Add(Own);
	TestEqual(TEXT("A price set in MeshTaskPrices replaces the built-in one"), PriceOf(TEXT("P2"), TEXT("a cup"), TEXT(""), TEXT("none"), TEXT(""), TEXT(""), TEXT(""), TEXT("")), 90);
	Settings->MeshTaskPrices = Saved;

	// Settings > Keys checks a Tripo key where generation sends it, not at the China site (which answered 401).
	FHyperAIStudioKeySlot TripoSlot;
	TripoSlot.Test = TEXT("tripo");
	FHyperAIStudioKeyTestRequest KeyCheck;
	TestTrue(TEXT("A Tripo key check is built"), HyperAIStudio::Credentials::BuildTestRequest(TripoSlot, TEXT("tsk_test"), FString(), KeyCheck));
	FHyperAIStudioExternalServiceSettings Tripo;
	TestTrue(TEXT("The tripo service is there"), FHyperAIStudioExternalServices::Find(TEXT("tripo"), Tripo));
	FString TripoBase = Tripo.BaseUrl;
	TripoBase.RemoveFromEnd(TEXT("/"));
	TestEqual(TEXT("The key check reads the balance from the generation address"), KeyCheck.Url, TripoBase + TEXT("/account/balance"));
	TestFalse(TEXT("The key check never goes to the China site"), KeyCheck.Url.Contains(TEXT("tripo3d.com")));
	TestEqual(TEXT("The key goes as a Bearer token"), KeyCheck.Headers.FindRef(TEXT("Authorization")), FString(TEXT("Bearer tsk_test")));
	return true;
}

// ---------------------------------------------------------------- the ladder

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelCreatorLadderTest, "HyperAIStudio.MeshGen.Creator.Ladder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioModelCreatorLadderTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelCreatorTests;
	auto Report = [](std::initializer_list<const TCHAR*> Failing)
	{
		FHyperAIModelCheckReport Out;
		Out.Triangles = 30000;
		for (const TCHAR* Id : Failing)
		{
			FHyperAIModelCheck& Check = Out.Checks.AddDefaulted_GetRef();
			Check.Id = Id;
			Check.Result = TEXT("fail");
		}
		FHyperAIModelCheck& Textures = Out.Checks.AddDefaulted_GetRef();
		Textures.Id = TEXT("textures_ok");
		Textures.Result = TEXT("pass");
		return Out;
	};
	FFixContext Context;
	Context.InputTaskId = TEXT("task_gen");
	Context.bHasTextures = true;
	Context.RemainingUSD = 5.f;
	Context.bCanRegenerate = true;
	FHyperAIModelBrief Brief;
	Brief.Prompt = TEXT("a crate");
	FString Error;
	Classify(Brief, Context.Plan, Error);

	TArray<FPaidFix> Ladder = PaidLadder(Report({TEXT("uv_overlap"), TEXT("textures")}), Context);
	TestTrue(TEXT("Cheapest first: texture (10) before smart UV (20)"), Ladder.Num() == 2 && Ladder[0].Id == TEXT("texture") && Ladder[1].Id == TEXT("smart_uv"));
	TestTrue(TEXT("and each one says what it clears and where it goes"), Ladder[1].Endpoints == TArray<FString>{TEXT("/mesh/uv")} && Ladder[1].Clears.Contains(TEXT("uv_overlap")));
	Ladder = PaidLadder(Report({TEXT("faces"), TEXT("textures")}), Context);
	TestTrue(TEXT("Over the face budget, retopology comes first: nothing cheaper fixes faces"), !Ladder.IsEmpty() && Ladder[0].Id == TEXT("retopo")
		&& Ladder[0].Bodies[0]->GetStringField(TEXT("input")) == TEXT("task_gen"));
	Ladder = PaidLadder(Report({TEXT("holes")}), Context);
	TestTrue(TEXT("A hole the free fill left goes to retopology first"), Ladder.Num() == 1 && Ladder[0].Id == TEXT("retopo"));
	Context.Tried.Add(TEXT("retopo"), 1);
	Ladder = PaidLadder(Report({TEXT("holes")}), Context);
	TestTrue(TEXT("then, retopology tried, segmentation and a quick cap (70), the cap's input the segment task"), Ladder.Num() == 1 && Ladder[0].Id == TEXT("quick_cap")
		&& Ladder[0].Endpoints.Num() == 2 && Credits(Ladder[0].PriceUSD) == 70 && Ladder[0].Bodies[1]->GetStringField(TEXT("input")) == TEXT("{segment_task}"));
	Context.Tried.Add(TEXT("quick_cap"), 1);
	Ladder = PaidLadder(Report({TEXT("holes")}), Context);
	TestTrue(TEXT("then AI completion (90)"), Ladder.Num() == 1 && Ladder[0].Id == TEXT("complete") && Credits(Ladder[0].PriceUSD) == 90);
	Context.Tried.Reset();
	Ladder = PaidLadder(Report({TEXT("visual"), TEXT("textures")}), Context);
	TestTrue(TEXT("A wrong shape needs a new generation, the dearest, so last"), Ladder.Num() == 2 && Ladder.Last().Id == TEXT("regenerate"));
	TArray<FString> Over;
	Context.RemainingUSD = 0.15f;
	Ladder = PaidLadder(Report({TEXT("uv_overlap"), TEXT("textures")}), Context, &Over);
	TestTrue(TEXT("What the budget cannot pay for is left out, and said"), Ladder.Num() == 1 && Ladder[0].Id == TEXT("texture") && Over.Num() == 1 && Over[0].Contains(TEXT("Smart UV")));
	Context.RemainingUSD = 5.f;
	Context.InputTaskId.Reset();
	Context.InputToken = TEXT("{input}");
	Ladder = PaidLadder(Report({TEXT("textures")}), Context);
	TestTrue(TEXT("A model Tripo did not make (or one a paid fix changed) goes as an uploaded file"), Ladder.Num() == 1 && Ladder[0].Bodies[0]->GetStringField(TEXT("input")) == TEXT("{input}"));
	Context.bHasTextures = false;
	Ladder = PaidLadder(Report({TEXT("uv_overlap")}), Context);
	TestTrue(TEXT("UVs with no texture on them are the free unwrap's, not Tripo's"), Ladder.IsEmpty());
	Context.bSkeletal = true;
	FHyperAIModelCheckReport Scale = Report({TEXT("pivot"), TEXT("scale")});
	Scale.Checks[1].Value = TEXT("400");
	Scale.Checks[1].Expected = TEXT("180");
	Ladder = PaidLadder(Scale, Context);
	TestTrue(TEXT("A skeletal mesh's pivot and scale go to Tripo's convert"), Ladder.Num() == 1 && Ladder[0].Id == TEXT("convert")
		&& Ladder[0].Bodies[0]->GetBoolField(TEXT("pivot_to_center_bottom")) && FMath::IsNearlyEqual(Ladder[0].Bodies[0]->GetNumberField(TEXT("scale_factor")), 0.45, 0.01));

	FHyperAIModelCheckReport Local = Report({TEXT("pivot")});
	Local.Checks[0].LocalFix = TEXT("pivot");
	FHyperAIModelCheck& Bits = Local.Checks.AddDefaulted_GetRef();
	Bits.Id = TEXT("floating");
	Bits.Result = TEXT("fail");
	Bits.LocalFix = TEXT("remove_bits");
	FHyperAIModelCheck& Wind = Local.Checks.AddDefaulted_GetRef();
	Wind.Id = TEXT("flipped");
	Wind.Result = TEXT("fail");
	Wind.LocalFix = TEXT("orient,normals");
	TestEqual(TEXT("Free fixes run in order: bits, winding, normals, then pivot"), FString::Join(LocalFixesFor(Local), TEXT(",")), FString(TEXT("remove_bits,orient,normals,pivot")));
	return true;
}

// ---------------------------------------------------------------- checks and free fixes on fixtures

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelCreatorChecksTest, "HyperAIStudio.MeshGen.Creator.ChecksAndLocalFixes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioModelCreatorChecksTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelCreatorTests;
	TArray<FString> Imported;
	ON_SCOPE_EXIT { DeleteAll(Imported); };
	FCheckTargets Targets = TargetsForUse(TEXT("prop"), TEXT("pc"), 1.f, 2000, TEXT("tris"));
	Targets.bTextures = false;
	Targets.bPbr = false;
	Targets.Collision = TEXT("box");

	FFixture Clean;
	UStaticMesh* Mesh = Import(CubeGlb(Clean), TEXT("SM_CreatorClean"), Imported);
	if (!TestNotNull(TEXT("The clean fixture imports"), Mesh))
	{
		return false;
	}
	// The import may give it a collision of its own; this one starts without.
	Mesh->CreateBodySetup();
	Mesh->GetBodySetup()->AggGeom.EmptyElements();
	Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseDefault;
	FHyperAIModelCheckReport Report;
	FString Error;
	TestTrue(TEXT("It measures"), Inspect(*Mesh, Targets, Report, Error));
	AddInfo(TEXT("Clean: ") + Report.Summary);
	for (const TCHAR* Id : {TEXT("faces"), TEXT("non_manifold"), TEXT("open_edges"), TEXT("holes"), TEXT("flipped"), TEXT("normals"), TEXT("floating"), TEXT("scale"), TEXT("pivot"), TEXT("uv_present"), TEXT("uv_overlap")})
	{
		TestEqual(FString::Printf(TEXT("The clean cube passes %s"), Id), ResultOf(Report, Id), FString(TEXT("pass")));
	}
	TestTrue(TEXT("It is 768 triangles, 100 cm"), Report.Triangles == 768 && FMath::IsNearlyEqual(Report.Size.Z, 100.0, 1.0));
	TestEqual(TEXT("Over a 500-triangle budget, faces fail and point to retopology"), [&]()
	{
		FCheckTargets Tight = Targets;
		Tight.FaceLimit = 500;
		FHyperAIModelCheckReport Out;
		FString Why;
		Inspect(*Mesh, Tight, Out, Why);
		const FHyperAIModelCheck* Faces = Find(Out, TEXT("faces"));
		return Faces ? Faces->Result + TEXT(" ") + Faces->PaidFix : FString();
	}(), FString(TEXT("fail retopo")));
	TestEqual(TEXT("Without collision, collision fails"), ResultOf(Report, TEXT("collision")), FString(TEXT("fail")));

	struct FCase
	{
		const TCHAR* Name;
		FFixture Fixture;
		const TCHAR* Check;
		const TCHAR* Fix;
	};
	FFixture Hole; Hole.bHole = true;
	FFixture Flip; Flip.bFlipOne = true;
	FFixture Float; Float.bFloating = true;
	FFixture Small; Small.Size = 0.25f;
	FFixture Overlap; Overlap.bOverlapUV = true;
	FFixture Off; Off.Offset = FVector3f(0.4f, 0.3f, -0.2f);
	const FCase Cases[] = {
		{TEXT("SM_CreatorHole"), Hole, TEXT("holes"), TEXT("fill_holes")},
		{TEXT("SM_CreatorFlip"), Flip, TEXT("flipped"), TEXT("orient")},
		{TEXT("SM_CreatorFloat"), Float, TEXT("floating"), TEXT("remove_bits")},
		{TEXT("SM_CreatorSmall"), Small, TEXT("scale"), TEXT("scale")},
		{TEXT("SM_CreatorOverlap"), Overlap, TEXT("uv_overlap"), TEXT("auto_uv")},
		{TEXT("SM_CreatorOffset"), Off, TEXT("pivot"), TEXT("pivot")},
	};
	for (const FCase& Case : Cases)
	{
		UStaticMesh* Defective = Import(CubeGlb(Case.Fixture), Case.Name, Imported);
		if (!TestNotNull(FString(Case.Name) + TEXT(" imports"), Defective))
		{
			continue;
		}
		FHyperAIModelCheckReport Before;
		Inspect(*Defective, Targets, Before, Error);
		const FHyperAIModelCheck* Check = Find(Before, Case.Check);
		AddInfo(FString::Printf(TEXT("%s: %s (%s)"), Case.Name, *Before.Summary, Check ? *Check->Detail : TEXT("")));
		TestEqual(FString::Printf(TEXT("%s fails %s"), Case.Name, Case.Check), ResultOf(Before, Case.Check), FString(TEXT("fail")));
		TestTrue(FString::Printf(TEXT("%s names the free fix %s"), Case.Name, Case.Fix), Check && Check->LocalFix.Contains(Case.Fix) && LocalFixesFor(Before).Contains(Case.Fix));
		TArray<FString> Notes;
		TestTrue(FString::Printf(TEXT("%s: the free fixes run: %s"), Case.Name, *Error), ApplyLocalFixes(*Defective, Targets, LocalFixesFor(Before), Notes, Error));
		FHyperAIModelCheckReport After;
		Inspect(*Defective, Targets, After, Error);
		AddInfo(FString::Printf(TEXT("%s fixed: %s; %s"), Case.Name, *After.Summary, *FString::Join(Notes, TEXT(" "))));
		TestEqual(FString::Printf(TEXT("%s passes %s after %s"), Case.Name, Case.Check, Case.Fix), ResultOf(After, Case.Check), FString(TEXT("pass")));
		TestEqual(FString::Printf(TEXT("%s has collision after the fixes"), Case.Name), ResultOf(After, TEXT("collision")), FString(TEXT("pass")));
		TestEqual(FString::Printf(TEXT("%s keeps its pivot at the bottom centre"), Case.Name), ResultOf(After, TEXT("pivot")), FString(TEXT("pass")));
		TestTrue(FString::Printf(TEXT("%s passes every check after its free fixes: %s"), Case.Name, *After.Summary), After.bPassed);
	}

	// The fixes that need no defect to try: lightmap UVs, Nanite, normals.
	UStaticMesh* Settled = Import(CubeGlb(Clean), TEXT("SM_CreatorSettings"), Imported);
	TArray<FString> Notes;
	FCheckTargets Dense = Targets;
	Dense.bNanite = true;
	TestTrue(TEXT("Lightmap UVs, Nanite and normals apply"), Settled && ApplyLocalFixes(*Settled, Dense, {TEXT("lightmap_uv"), TEXT("nanite"), TEXT("normals"), TEXT("collision")}, Notes, Error));
	if (Settled)
	{
		TestTrue(TEXT("lightmap UVs into channel 1"), Settled->GetSourceModel(0).BuildSettings.bGenerateLightmapUVs && Settled->GetLightMapCoordinateIndex() == 1);
		TestTrue(TEXT("Nanite on"), Settled->GetNaniteSettings().bEnabled);
		TestTrue(TEXT("a box collision"), Settled->GetBodySetup() && Settled->GetBodySetup()->AggGeom.BoxElems.Num() == 1);
		FHyperAIModelCheckReport After;
		Inspect(*Settled, Dense, After, Error);
		TestEqual(TEXT("normals agree with the winding"), ResultOf(After, TEXT("normals")), FString(TEXT("pass")));
	}

	// Missing textures fail when textures are wanted, and point to Tripo's texture task.
	FCheckTargets Textured = Targets;
	Textured.bTextures = true;
	Textured.bPbr = true;
	Inspect(*Mesh, Textured, Report, Error);
	const FHyperAIModelCheck* Textures = Find(Report, TEXT("textures"));
	TestTrue(TEXT("No texture: textures fail, PBR fails, Tripo's texture is the fix"), Textures && Textures->Result == TEXT("fail") && Textures->PaidFix == TEXT("texture")
		&& ResultOf(Report, TEXT("pbr")) == TEXT("fail"));

	// A skeletal mesh is not reshaped.
	TArray<FString> SkeletalNotes;
	return true;
}

// ---------------------------------------------------------------- materials from PBR maps

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelCreatorMaterialsTest, "HyperAIStudio.MeshGen.Creator.Materials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioModelCreatorMaterialsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelCreatorTests;
	TArray<FString> Imported;
	ON_SCOPE_EXIT { DeleteAll(Imported); };
	FFixture Fixture;
	Fixture.bTextured = true;
	Fixture.Grid = 2;
	UStaticMesh* Mesh = Import(CubeGlb(Fixture), TEXT("SM_CreatorTextured"), Imported);
	if (!TestNotNull(TEXT("The textured fixture imports"), Mesh))
	{
		return false;
	}
	FCheckTargets Targets = TargetsForUse(TEXT("prop"), TEXT("pc"), 1.f, 2000, TEXT("tris"));
	FHyperAIModelCheckReport Report;
	FString Error;
	Inspect(*Mesh, Targets, Report, Error);
	AddInfo(TEXT("Imported: ") + Report.Summary);
	TestEqual(TEXT("The glTF's maps are found: base colour"), ResultOf(Report, TEXT("textures")) == TEXT("fail") ? FString(TEXT("fail")) : FString(TEXT("found")), FString(TEXT("found")));
	TestEqual(TEXT("and every PBR map (glTF's packed metallic-roughness)"), ResultOf(Report, TEXT("pbr")), FString(TEXT("pass")));
	TestEqual(TEXT("The imported material is not HyperAI's master yet"), ResultOf(Report, TEXT("material_setup")), FString(TEXT("warn")));
	FMaterialResult Result;
	TestTrue(TEXT("The material build runs: ") + Error, BuildMaterials(*Mesh, false, Result, Error));
	for (const FString& Note : Result.Notes)
	{
		AddInfo(Note);
	}
	UMaterialInstanceConstant* Instance = Result.Materials.IsEmpty() ? nullptr : LoadObject<UMaterialInstanceConstant>(nullptr, *Result.Materials[0]);
	TestTrue(TEXT("A material instance of HyperAI's master on the slot"), Instance && Instance->Parent && Instance->Parent->GetPathName().StartsWith(MasterMaterialPath())
		&& Mesh->GetStaticMaterials()[0].MaterialInterface == Instance);
	Imported.Append(Result.Materials);
	Imported.Add(MasterMaterialPath());
	UTexture* BaseColor = nullptr;
	UTexture* Normal = nullptr;
	UTexture* ORM = nullptr;
	if (Instance)
	{
		Instance->GetTextureParameterValue(FMaterialParameterInfo(TEXT("BaseColor")), BaseColor);
		Instance->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Normal")), Normal);
		Instance->GetTextureParameterValue(FMaterialParameterInfo(TEXT("ORM")), ORM);
	}
	TestTrue(TEXT("Base colour is sRGB colour"), BaseColor && BaseColor->SRGB && BaseColor->CompressionSettings == TC_Default);
	TestTrue(TEXT("The normal map has normal-map compression, linear"), Normal && Normal->CompressionSettings == TC_Normalmap && !Normal->SRGB);
	UTexture2D* Packed = Cast<UTexture2D>(ORM);
	TestTrue(TEXT("An ORM is packed: linear masks"), Packed && !Packed->SRGB && Packed->CompressionSettings == TC_Masks);
	if (Packed)
	{
		Imported.Add(Packed->GetPathName());
		FImage Image;
		Packed->Source.GetMipImage(Image, 0);
		const uint8* Pixel = Image.RawData.GetData();
		// BGRA: blue metal 0, green roughness 200 from glTF's green, red occlusion 255 (none given).
		TestTrue(FString::Printf(TEXT("ORM channels: occlusion 255, roughness 200, metal 0 (got %d %d %d)"), Pixel[2], Pixel[1], Pixel[0]),
			Image.Format == ERawImageFormat::BGRA8 && Pixel[2] == 255 && FMath::Abs(int32(Pixel[1]) - 200) <= 2 && Pixel[0] == 0);
	}
	Inspect(*Mesh, Targets, Report, Error);
	TestTrue(TEXT("After the build: the master on every slot, the normal map well formed, roughness and metal linear"), ResultOf(Report, TEXT("material_setup")) == TEXT("pass")
		&& ResultOf(Report, TEXT("normal_map")) == TEXT("pass") && ResultOf(Report, TEXT("roughness_metal")) == TEXT("pass") && ResultOf(Report, TEXT("pbr")) == TEXT("pass"));

	// A parts model: one slot a part.
	FFixture PartsFixture;
	PartsFixture.bFloating = true;
	PartsFixture.Grid = 2;
	PartsFixture.bTextured = true;
	UStaticMesh* Parts = Import(CubeGlb(PartsFixture), TEXT("SM_CreatorParts"), Imported);
	FMaterialResult PartsResult;
	TestTrue(TEXT("A parts model gets a material slot a part"), Parts && BuildMaterials(*Parts, true, PartsResult, Error) && Parts->GetStaticMaterials().Num() == 2 && PartsResult.Materials.Num() == 2);
	Imported.Append(PartsResult.Materials);
	Imported.Append(PartsResult.Textures);
	return true;
}

// ---------------------------------------------------------------- a whole job against a stand-in Tripo

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelCreatorJobTest, "HyperAIStudio.MeshGen.Creator.Job",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioModelCreatorJobTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelCreatorTests;
	constexpr uint32 Port = 18353;
	const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
	if (!Router.IsValid())
	{
		AddWarning(TEXT("Port 18353 is taken, so the stand-in for Tripo could not start."));
		return true;
	}
	const FString Base = FString::Printf(TEXT("http://127.0.0.1:%u"), Port);
	struct FSeen
	{
		TArray<FString> Calls;
		TMap<FString, FString> Bodies;
		TArray<FString> Auth;
		int32 Polls = 0;
		bool bFixRepairs = true;
	};
	const TSharedRef<FSeen> Seen = MakeShared<FSeen>();
	FFixture Generated;
	Generated.Size = 1.f;
	Generated.Grid = 8;
	Generated.bFloating = true;
	const TSharedRef<TArray<uint8>> Dense = MakeShared<TArray<uint8>>(CubeGlb(Generated));
	FFixture Light;
	Light.Grid = 4;
	Light.Size = 0.8f;
	const TSharedRef<TArray<uint8>> Retopologised = MakeShared<TArray<uint8>>(CubeGlb(Light));
	const TSharedRef<TArray<uint8>> Render = MakeShared<TArray<uint8>>(Png(32, [](int32, int32) { return FColor(120, 90, 60); }));
	TArray<FHttpRouteHandle> Routes;
	auto Bind = [&](const TCHAR* Path, const EHttpServerRequestVerbs Verb, TFunction<TUniquePtr<FHttpServerResponse>(const FHttpServerRequest&)> Answer)
	{
		Routes.Add(Router->BindRoute(FHttpPath(Path), Verb, FHttpRequestHandler::CreateLambda([Seen, Path = FString(Path), Answer](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			Seen->Calls.Add(Path);
			Seen->Bodies.Add(Path, BodyText(Request).Left(4000));
			for (const TPair<FString, TArray<FString>>& Header : Request.Headers)
			{
				if (Header.Key.Equals(TEXT("Authorization"), ESearchCase::IgnoreCase) && !Header.Value.IsEmpty())
				{
					Seen->Auth.Add(Header.Value[0]);
				}
			}
			OnComplete(Answer(Request));
			return true;
		})));
	};
	auto Json = [](const FString& Text) { return FHttpServerResponse::Create(Text, TEXT("application/json")); };
	Bind(TEXT("/tripo/v3/generation/text-to-model"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"code":0,"data":{"task_id":"task_gen"}})")); });
	Bind(TEXT("/tripo/v3/mesh/decimate"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"code":0,"data":{"task_id":"task_retopo"}})")); });
	Bind(TEXT("/tripo/v3/files"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&) { return Json(TEXT(R"({"code":0,"data":{"file_token":"file_model"}})")); });
	auto Task = [Seen, Json, Base](const FString& Model, const float Credits)
	{
		return [Seen, Json, Base, Model, Credits](const FHttpServerRequest&)
		{
			if (Seen->Polls++ % 2 == 0)
			{
				return Json(TEXT(R"({"code":0,"data":{"status":"running","progress":50}})"));
			}
			return Json(FString::Printf(TEXT(R"({"code":0,"data":{"status":"success","progress":100,"credits_consumed":%.2f,"output":{"model_url":"%s%s","rendered_image_url":"%s/render.png"}}})"),
				Credits, *Base, *Model, *Base));
		};
	};
	Bind(TEXT("/tripo/v3/tasks/task_gen"), EHttpServerRequestVerbs::VERB_GET, Task(TEXT("/dense.glb"), 10.f));
	Bind(TEXT("/tripo/v3/tasks/task_retopo"), EHttpServerRequestVerbs::VERB_GET, Task(TEXT("/retopo.glb"), 35.f));
	Bind(TEXT("/dense.glb"), EHttpServerRequestVerbs::VERB_GET, [Dense](const FHttpServerRequest&) { return FHttpServerResponse::Create(TArray<uint8>(*Dense), TEXT("model/gltf-binary")); });
	Bind(TEXT("/retopo.glb"), EHttpServerRequestVerbs::VERB_GET, [Seen, Dense, Retopologised](const FHttpServerRequest&)
	{
		return FHttpServerResponse::Create(TArray<uint8>(Seen->bFixRepairs ? *Retopologised : *Dense), TEXT("model/gltf-binary"));
	});
	Bind(TEXT("/render.png"), EHttpServerRequestVerbs::VERB_GET, [Render](const FHttpServerRequest&) { return FHttpServerResponse::Create(TArray<uint8>(*Render), TEXT("image/png")); });
	FHttpServerModule::Get().StartAllListeners();

	FStandIn StandIn(Base);
	TArray<FString> Operations;
	TArray<FString> Cleanup;
	ON_SCOPE_EXIT
	{
		for (const FHttpRouteHandle& Route : Routes)
		{
			Router->UnbindRoute(Route);
		}
		for (const FString& Operation : Operations)
		{
			IFileManager::Get().DeleteDirectory(*(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Models") / Operation), false, true);
		}
		DeleteAll(Cleanup);
	};

	FHyperAIModelCreateRequest Request;
	Request.Brief.Prompt = TEXT("a wooden crate");
	Request.Brief.ReferenceImage = TEXT("off");
	Request.Brief.Use = TEXT("prop");
	Request.Brief.Model = TEXT("H3.1");
	Request.Brief.FaceLimit = 600;
	Request.Brief.TextureQuality = TEXT("none");
	Request.Brief.SizeMeters = 0.8f;
	Request.FixBudgetUSD = 1.f;
	Request.MaxRounds = 3;

	// A dry run classifies and prices; nothing is sent.
	FHyperAIModelCreateReport Dry = Create(Request);
	TestTrue(TEXT("A dry run plans and prices: ") + Dry.Diagnostic, Dry.bOk && Dry.Status == TEXT("checked") && Dry.Plan.Credits == 10 && Seen->Calls.IsEmpty());

	// Refusals.
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_TRIPO_KEY"), TEXT(""));
	Request.bDryRun = false;
	FHyperAIModelCreateReport NoKey = Create(Request);
	TestTrue(TEXT("Without the API key it is refused, and says Studio credits cannot be used"), NoKey.Status == TEXT("missing_key") && NoKey.Diagnostic.Contains(TEXT("Tripo Studio")));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_TRIPO_KEY"), TEXT("tripo-creator-test-key"));
	FHyperAIModelCreateRequest Bad = Request;
	Bad.Brief.Model = TEXT("P2");
	Bad.Brief.Parts = TEXT("on");
	TestEqual(TEXT("P2 with parts is refused"), Create(Bad).Status, FString(TEXT("invalid_request")));
	Bad = Request;
	Bad.Brief.Prompt.Reset();
	Bad.Brief.ImagePath = TEXT("C:/Windows/win.ini");
	TestEqual(TEXT("A picture outside the project is refused"), Create(Bad).Status, FString(TEXT("invalid_image")));
	Bad = Request;
	Bad.Folder = TEXT("/Engine/Stuff");
	TestEqual(TEXT("A folder outside /Game is refused"), Create(Bad).Status, FString(TEXT("invalid_request")));
	TestTrue(TEXT("No refusal reached Tripo"), Seen->Calls.IsEmpty());

	// The job: generate (approved), import, check, free fixes, then retopology offered, priced, approved, copied onto the asset.
	Request.OperationId = TEXT("model-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	FHyperAIModelCreateReport Asked = Create(Request);
	Operations.Add(Request.OperationId);
	if (!TestEqual(TEXT("It waits for approval: ") + Asked.Diagnostic, Asked.Status, FString(TEXT("awaiting_user_approval"))))
	{
		return false;
	}
	FString Error;
	TestTrue(TEXT("Approved"), FHyperAIStudioApprovalGate::Approve(Asked.OperationId, Error));
	FHyperAIModelJob Job;
	TickUntil([&]() { Job = JobOf(Asked.OperationId); return Job.State == TEXT("awaiting_fix_approval") || Job.State == TEXT("completed") || Job.State == TEXT("stopped") || Job.State == TEXT("failed"); });
	AddInfo(FString::Printf(TEXT("After generation: %s, %s"), *Job.State, *Job.Diagnostic));
	for (const FHyperAIModelStep& Step : Job.Steps)
	{
		AddInfo(FString::Printf(TEXT("  round %d %s %s: %s (%s)"), Step.Round, *Step.Kind, *Step.Status, *Step.What.Left(160), *Step.Detail.Left(300)));
	}
	Cleanup.Add(Job.AssetPath);
	const TSharedPtr<FJsonObject> Sent = Parse(Seen->Bodies.FindRef(TEXT("/tripo/v3/generation/text-to-model")));
	TestTrue(TEXT("The generation went to /generation/text-to-model with the plan's fields"), Sent.IsValid() && Sent->GetStringField(TEXT("model")) == TEXT("v3.1-20260211")
		&& Sent->GetIntegerField(TEXT("face_limit")) == 600 && !Sent->GetBoolField(TEXT("texture")) && Sent->GetStringField(TEXT("prompt")) == Request.Brief.Prompt);
	TestTrue(TEXT("with the key as a bearer, and nowhere else"), Seen->Auth.Contains(TEXT("Bearer tripo-creator-test-key")) && !Job.Diagnostic.Contains(TEXT("tripo-creator-test-key")));
	TestTrue(TEXT("The stray bit and the scale were fixed for free"), Job.Steps.ContainsByPredicate([](const FHyperAIModelStep& Step)
	{
		return Step.Kind == TEXT("local_fix") && Step.What.Contains(TEXT("remove_bits")) && Step.What.Contains(TEXT("scale")) && Step.Status == TEXT("done");
	}));
	TestEqual(TEXT("Still over the 600-triangle budget: retopology is offered"), Job.State, FString(TEXT("awaiting_fix_approval")));
	TestEqual(TEXT("The generation's own check is round 0"), Job.Round, 0);
	const FHyperAIModelStep* Offer = Job.Steps.FindByPredicate([](const FHyperAIModelStep& Step) { return Step.Kind == TEXT("paid_fix") && Step.Status == TEXT("awaiting_user_approval"); });
	TestTrue(TEXT("priced at 35 credits, approved on its own"), Offer && Offer->Credits == 35 && Offer->OperationId == Job.CurrentOperationId && FHyperAIStudioApprovalGate::HasPending(Job.CurrentOperationId));
	TestTrue(TEXT("Tripo's render came down for the visual check"), !Job.PreviewImages.IsEmpty());
	TestTrue(TEXT("The journal is written"), IFileManager::Get().FileSize(*(FPaths::ProjectDir() / Job.Journal)) > 0);
	TestTrue(TEXT("Approved"), FHyperAIStudioApprovalGate::Approve(Job.CurrentOperationId, Error));
	TickUntil([&]() { Job = JobOf(Asked.OperationId); return Job.State == TEXT("completed") || Job.State == TEXT("stopped") || Job.State == TEXT("failed"); });
	AddInfo(FString::Printf(TEXT("After the fix: %s, %s"), *Job.State, *Job.Diagnostic));
	const TSharedPtr<FJsonObject> Retopo = Parse(Seen->Bodies.FindRef(TEXT("/tripo/v3/mesh/decimate")));
	TestTrue(TEXT("Retopology took the generation's task as input, the budget, v3.0 and smart UV"), Retopo.IsValid() && Retopo->GetStringField(TEXT("input")) == TEXT("task_gen")
		&& Retopo->GetIntegerField(TEXT("face_limit")) == 600 && Retopo->GetStringField(TEXT("model")) == TEXT("v3.0") && Retopo->GetBoolField(TEXT("smart_uv")));
	TestEqual(TEXT("and the job ends with every check passing"), Job.StopReason, FString(TEXT("checks_passed")));
	TestTrue(TEXT("on the same asset, now light"), Job.LastCheck.Triangles <= 600 && Job.LastCheck.AssetPath == Job.AssetPath);
	TestTrue(TEXT("Spent: the generation and the fix, with Tripo's own credit count"), FMath::IsNearlyEqual(Job.SpentUSD, 0.45f, 0.001f) && FMath::IsNearlyEqual(Job.CreditsConsumed, 45.f, 0.01f));
	for (const FHyperAIModelStep& Step : Job.Steps)
	{
		if (Step.Kind == TEXT("paid_fix") || Step.Kind == TEXT("generate"))
		{
			Cleanup.Add(FPaths::GetBaseFilename(Step.Detail));
		}
	}
	if (const UStaticMesh* Fixed = LoadObject<UStaticMesh>(nullptr, *Job.AssetPath))
	{
		Cleanup.Add(FPackageName::GetLongPackagePath(Fixed->GetOutermost()->GetName()) / TEXT("Fixes"));
	}

	// The budget: a fix that costs more than what is left is not offered; the job stops and says why.
	Seen->Polls = 0;
	FHyperAIModelCreateRequest Poor = Request;
	Poor.OperationId = TEXT("model-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	Poor.FixBudgetUSD = 0.2f;
	Poor.Name = TEXT("SM_CreatorPoor");
	Operations.Add(Poor.OperationId);
	FHyperAIModelCreateReport PoorAsked = Create(Poor);
	FHyperAIStudioApprovalGate::Approve(PoorAsked.OperationId, Error);
	TickUntil([&]() { Job = JobOf(Poor.OperationId); return Job.State == TEXT("completed") || Job.State == TEXT("stopped") || Job.State == TEXT("failed") || Job.State == TEXT("awaiting_fix_approval"); });
	Cleanup.Add(Job.AssetPath);
	TestTrue(TEXT("Over budget it stops, budget_used, with what it would have done"), Job.State == TEXT("stopped") && Job.StopReason == TEXT("budget_used")
		&& Job.Steps.ContainsByPredicate([](const FHyperAIModelStep& Step) { return Step.Kind == TEXT("paid_fix") && Step.Status == TEXT("skipped"); }));

	// The round cap: a fix that does not help, then no rounds left.
	Seen->Polls = 0;
	Seen->bFixRepairs = false;
	FHyperAIModelCreateRequest Stubborn = Request;
	Stubborn.OperationId = TEXT("model-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	Stubborn.MaxRounds = 1;
	Stubborn.FixBudgetUSD = 5.f;
	Stubborn.Name = TEXT("SM_CreatorStubborn");
	Operations.Add(Stubborn.OperationId);
	FHyperAIModelCreateReport StubbornAsked = Create(Stubborn);
	FHyperAIStudioApprovalGate::Approve(StubbornAsked.OperationId, Error);
	TickUntil([&]() { Job = JobOf(Stubborn.OperationId); return Job.State == TEXT("awaiting_fix_approval") || Job.State == TEXT("stopped") || Job.State == TEXT("failed"); });
	Cleanup.Add(Job.AssetPath);
	FHyperAIStudioApprovalGate::Approve(Job.CurrentOperationId, Error);
	TickUntil([&]() { Job = JobOf(Stubborn.OperationId); return Job.State == TEXT("completed") || Job.State == TEXT("stopped") || Job.State == TEXT("failed"); });
	TestTrue(TEXT("After its one round (one paid fix, which did not help) it stops, rounds_used"), Job.State == TEXT("stopped") && Job.StopReason == TEXT("rounds_used") && Job.Round == 1
		&& Job.Steps.FilterByPredicate([](const FHyperAIModelStep& Step) { return Step.Kind == TEXT("paid_fix") && Step.Status == TEXT("done"); }).Num() == 1);

	// Declined: the user turns the paid fix down; the job stops and keeps what it has.
	Seen->Polls = 0;
	FHyperAIModelCreateRequest Declined = Request;
	Declined.OperationId = TEXT("model-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	Declined.Name = TEXT("SM_CreatorDeclined");
	Operations.Add(Declined.OperationId);
	FHyperAIModelCreateReport DeclinedAsked = Create(Declined);
	FHyperAIStudioApprovalGate::Approve(DeclinedAsked.OperationId, Error);
	TickUntil([&]() { Job = JobOf(Declined.OperationId); return Job.State == TEXT("awaiting_fix_approval") || Job.State == TEXT("stopped") || Job.State == TEXT("failed"); });
	Cleanup.Add(Job.AssetPath);
	FHyperAIStudioApprovalGate::Reject(Job.CurrentOperationId, TEXT("Not now."));
	TickUntil([&]() { Job = JobOf(Declined.OperationId); return Job.State == TEXT("stopped"); }, 5.0);
	TestEqual(TEXT("A declined fix stops the job, fix_declined"), Job.StopReason, FString(TEXT("fix_declined")));

	// Cancel before approval: nothing reaches Tripo.
	const int32 CallsBefore = Seen->Calls.Num();
	FHyperAIModelCreateRequest Cancelled = Request;
	Cancelled.OperationId = TEXT("model-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	Operations.Add(Cancelled.OperationId);
	Create(Cancelled);
	FHyperAIMeshCancelRequest Cancel;
	Cancel.OperationId = Cancelled.OperationId;
	const FHyperAIMeshCancelReport Stopped = UHyperAIStudioMeshGenToolset::hyper_mesh_cancel(Cancel);
	TickJobs();
	TestTrue(TEXT("hyper_mesh_cancel stops a creator job before it is approved; nothing reaches Tripo"), Stopped.bOk && JobOf(Cancelled.OperationId).State == TEXT("cancelled")
		&& !FHyperAIStudioApprovalGate::HasPending(Cancelled.OperationId) && Seen->Calls.Num() == CallsBefore);
	TestEqual(TEXT("Cancelling a finished job says so"), UHyperAIStudioMeshGenToolset::hyper_mesh_cancel(Cancel).Status, FString(TEXT("already_finished")));

	// Resume after a restart: the generation Tripo already took is asked after again, never paid for twice.
	Seen->Polls = 0;
	Seen->bFixRepairs = true;
	FHyperAIModelCreateRequest Restarted = Request;
	Restarted.OperationId = TEXT("model-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	Restarted.Name = TEXT("SM_CreatorRestarted");
	Operations.Add(Restarted.OperationId);
	auto Generations = [Seen]() { return Seen->Calls.FilterByPredicate([](const FString& Call) { return Call == TEXT("/tripo/v3/generation/text-to-model"); }).Num(); };
	const int32 GenerationsBefore = Generations();
	FHyperAIModelCreateReport RestartedAsked = Create(Restarted);
	FHyperAIStudioApprovalGate::Approve(RestartedAsked.OperationId, Error);
	TickUntil([&]() { Job = JobOf(Restarted.OperationId); return Job.Steps.ContainsByPredicate([](const FHyperAIModelStep& Step) { return Step.Kind == TEXT("generate") && !Step.TaskId.IsEmpty(); }); }, 10.0);
	ForgetForTests(Restarted.OperationId);
	TestTrue(TEXT("Forgotten, the job is still in its journal"), JobOf(Restarted.OperationId).Diagnostic.Contains(TEXT("journal")));
	FHyperAIModelCreateRequest Pick;
	Pick.ResumeOperationId = Restarted.OperationId;
	const FHyperAIModelCreateReport Picked = Create(Pick);
	TestTrue(TEXT("Resumed: ") + Picked.Diagnostic, Picked.bOk && Picked.Status == TEXT("resumed") && Picked.Diagnostic.Contains(TEXT("task_gen")));
	TickUntil([&]() { Job = JobOf(Restarted.OperationId); return Job.State == TEXT("awaiting_fix_approval") || Job.State == TEXT("completed") || Job.State == TEXT("stopped") || Job.State == TEXT("failed"); });
	Cleanup.Add(Job.AssetPath);
	TestTrue(TEXT("It imports and checks what Tripo made, with one generation request in all: ") + Job.Diagnostic, !Job.AssetPath.IsEmpty() && Job.State == TEXT("awaiting_fix_approval")
		&& Generations() == GenerationsBefore + 1);
	FHyperAIStudioApprovalGate::Reject(Job.CurrentOperationId, TEXT("Enough."));

	// Resume: a finished job reports itself from its journal; nothing is redone.
	FHyperAIModelCreateRequest Resume;
	Resume.ResumeOperationId = Asked.OperationId;
	TestTrue(TEXT("Resuming a job in this session points to its status"), Create(Resume).bOk);
	return true;
}

// ---------------------------------------------------------------- Tripo Studio's DCC Bridge, and the account

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelCreatorStudioTest, "HyperAIStudio.MeshGen.Creator.StudioWatchAndAccount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioModelCreatorStudioTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelCreatorTests;
	const TArray<FString> Folders = {TEXT("/Game/Tripo")};
	const TArray<FString> Markers = {TEXT("tripo")};
	TestTrue(TEXT("A mesh in the bridge's folder is Tripo's"), IsTripoImport(TEXT("StaticMesh"), TEXT("/Game/Tripo/Chair/SM_Chair.SM_Chair"), FString(), Folders, Markers));
	TestTrue(TEXT("So is one named for Tripo"), IsTripoImport(TEXT("SkeletalMesh"), TEXT("/Game/Models/tripo_convert_abc.tripo_convert_abc"), FString(), Folders, Markers));
	TestTrue(TEXT("Or one from a file in a Tripo folder"), IsTripoImport(TEXT("StaticMesh"), TEXT("/Game/Props/SM_Lamp.SM_Lamp"), TEXT("C:/Users/x/AppData/Local/Tripo/bridge/lamp.fbx"), Folders, Markers));
	TestFalse(TEXT("Not an ordinary mesh elsewhere"), IsTripoImport(TEXT("StaticMesh"), TEXT("/Game/Props/SM_Lamp.SM_Lamp"), TEXT("D:/Art/lamp.fbx"), Folders, Markers));
	TestFalse(TEXT("Not a texture from the bridge's folder"), IsTripoImport(TEXT("Texture2D"), TEXT("/Game/Tripo/T_Chair.T_Chair"), FString(), Folders, Markers));
	TestFalse(TEXT("Not /Game/TripoX for /Game/Tripo"), IsTripoImport(TEXT("StaticMesh"), TEXT("/Game/TripoX/SM_A.SM_A"), FString(), Folders, {}));

	// A real import into a watched folder, as the bridge's would be: offered, approved, checked and fixed for free.
	UHyperAIStudioTripoSettings* Settings = GetMutableDefault<UHyperAIStudioTripoSettings>();
	const TArray<FString> SavedFolders = Settings->TripoBridgeFolders;
	const bool bSavedWatch = Settings->bWatchTripoBridgeImports;
	const bool bSavedNotify = Settings->bNotifyTripoBridgeImports;
	Settings->TripoBridgeFolders = {TEXT("/Game/HyperAITests/TripoBridge")};
	Settings->bWatchTripoBridgeImports = true;
	Settings->bNotifyTripoBridgeImports = false;
	TArray<FString> Imported;
	ON_SCOPE_EXIT
	{
		Settings->TripoBridgeFolders = SavedFolders;
		Settings->bWatchTripoBridgeImports = bSavedWatch;
		Settings->bNotifyTripoBridgeImports = bSavedNotify;
		DeleteAll(Imported);
	};
	StartWatch();
	const int32 OffersBefore = OfferedAssets().Num();
	FFixture Sent;
	Sent.bFloating = true;
	Sent.Offset = FVector3f(0.5f, 0.2f, 0.f);
	UStaticMesh* Mesh = Import(CubeGlb(Sent), TEXT("SM_FromStudio"), Imported, TEXT("/Game/HyperAITests/TripoBridge"), /*bOwn=*/false);
	TestNotNull(TEXT("The Studio model imports"), Mesh);
	TickUntil([&]() { return OfferedAssets().Num() > OffersBefore; }, 5.0);
	TestTrue(TEXT("The watch offers to check it"), OfferedAssets().Num() == OffersBefore + 1 && Mesh && OfferedAssets().Last() == Mesh->GetPathName());
	const TArray<FHyperAIStudioPendingApproval> Pending = FHyperAIStudioApprovalGate::GetPending();
	const FHyperAIStudioPendingApproval* Offer = Pending.FindByPredicate([](const FHyperAIStudioPendingApproval& Item) { return Item.Summary.VariantId == TEXT("model.tripo_studio.v1"); });
	TestTrue(TEXT("as an approval in Activity, an edit, free"), Offer && Offer->Summary.Safety == EHyperAIStudioDomainSafety::Edit);
	FString Error;
	if (Offer)
	{
		FHyperAIStudioApprovalGate::Approve(Offer->OperationId, Error);
		FHyperAIModelJob Job;
		TickUntil([&]() { Job = JobOf(Offer->OperationId + TEXT("-run")); return Job.State == TEXT("completed") || Job.State == TEXT("stopped") || Job.State == TEXT("failed"); });
		AddInfo(FString::Printf(TEXT("Studio model: %s, %s"), *Job.State, *Job.Diagnostic));
		TestTrue(TEXT("Approved, it is checked and fixed for free: the stray bit and the pivot"), Job.Steps.ContainsByPredicate([](const FHyperAIModelStep& Step)
		{
			return Step.Kind == TEXT("local_fix") && Step.What.Contains(TEXT("remove_bits")) && Step.What.Contains(TEXT("pivot"));
		}) && ResultOf(Job.LastCheck, TEXT("floating")) == TEXT("pass") && ResultOf(Job.LastCheck, TEXT("pivot")) == TEXT("pass"));
		TestEqual(TEXT("and no paid fix is offered without a key"), Job.Steps.FilterByPredicate([](const FHyperAIModelStep& Step) { return Step.Kind == TEXT("paid_fix"); }).Num(), 0);
	}
	// HyperAI's own imports are not offered.
	const int32 OffersAfter = OfferedAssets().Num();
	Import(CubeGlb(Sent), TEXT("SM_OwnImport"), Imported, TEXT("/Game/HyperAITests/TripoBridge"), /*bOwn=*/true);
	TickUntil([]() { return false; }, 1.5);
	TestEqual(TEXT("HyperAI's own imports into the folder are not offered"), OfferedAssets().Num(), OffersAfter);

	// hyper_model_check_and_fix on any mesh: a check only, then the free fixes after one approval.
	FHyperAIModelCheckFixRequest Check;
	Check.AssetPath = Imported.IsEmpty() ? FString() : Imported.Last();
	Check.Use = TEXT("prop");
	Check.SizeMeters = 1.f;
	const FHyperAIModelCheckFixReport Checked = HyperAIStudio::MeshGen::Creator::CheckAndFix(Check);
	TestTrue(TEXT("A check lists the free fixes and changes nothing"), Checked.bOk && Checked.Status == TEXT("checked") && Checked.LocalFixes.Contains(TEXT("remove_bits")));
	Check.bCheckOnly = false;
	const FHyperAIModelCheckFixReport Asked = HyperAIStudio::MeshGen::Creator::CheckAndFix(Check);
	TestEqual(TEXT("The free fixes wait for one approval"), Asked.Status, FString(TEXT("awaiting_user_approval")));
	FHyperAIStudioApprovalGate::Reject(Asked.OperationId, TEXT("Leave it."));
	TickUntil([&]() { return JobOf(Asked.OperationId).State == TEXT("stopped"); }, 3.0);
	TestEqual(TEXT("Turned down, it stops and changes nothing"), JobOf(Asked.OperationId).StopReason, FString(TEXT("fix_declined")));
	Check.bCheckOnly = true;
	Check.AssetPath = TEXT("/Game/Nope/SM_Missing.SM_Missing");
	TestEqual(TEXT("Not a mesh: refused"), HyperAIStudio::MeshGen::Creator::CheckAndFix(Check).Status, FString(TEXT("invalid_request")));

	// The account: balance and usage from the stand-in, the key nowhere in the report.
	constexpr uint32 Port = 18354;
	const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
	if (!Router.IsValid())
	{
		AddWarning(TEXT("Port 18354 is taken; the account check is skipped."));
		return true;
	}
	const FString Base = FString::Printf(TEXT("http://127.0.0.1:%u"), Port);
	TArray<FHttpRouteHandle> Routes;
	TSharedRef<FString> Auth = MakeShared<FString>();
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/tripo/v3/account/balance")), EHttpServerRequestVerbs::VERB_GET, FHttpRequestHandler::CreateLambda(
		[Auth](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			for (const TPair<FString, TArray<FString>>& Header : Request.Headers)
			{
				if (Header.Key.Equals(TEXT("Authorization"), ESearchCase::IgnoreCase) && !Header.Value.IsEmpty())
				{
					*Auth = Header.Value[0];
				}
			}
			OnComplete(FHttpServerResponse::Create(FString(TEXT(R"({"code":0,"data":{"balance":1234.5,"frozen":40}})")), TEXT("application/json")));
			return true;
		})));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/tripo/v3/account/usage")), EHttpServerRequestVerbs::VERB_GET, FHttpRequestHandler::CreateLambda(
		[](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			OnComplete(FHttpServerResponse::Create(FString(TEXT(R"({"code":0,"data":[{"task_id":"task_a","type":"text_to_model","credits_consumed":20,"created_at":"2026-10-30T10:00:00Z"},)")
				TEXT(R"({"task_id":"task_b","type":"mesh_decimate","credits_consumed":35,"created_at":"2026-10-30T10:05:00Z"}]})")), TEXT("application/json")));
			return true;
		})));
	FHttpServerModule::Get().StartAllListeners();
	ON_SCOPE_EXIT
	{
		for (const FHttpRouteHandle& Route : Routes)
		{
			Router->UnbindRoute(Route);
		}
	};
	{
		FStandIn StandIn(Base);
		bool bDone = false;
		FHyperAIModelAccountReport Read;
		RefreshAccount([&bDone, &Read](const FHyperAIModelAccountReport& Report) { Read = Report; bDone = true; });
		TickUntil([&bDone]() { return bDone; }, 10.0);
		TestTrue(TEXT("The balance is read from /v3/account/balance"), Read.Status == TEXT("ok") && FMath::IsNearlyEqual(Read.Balance, 1234.5f) && FMath::IsNearlyEqual(Read.Frozen, 40.f));
		TestTrue(TEXT("and the usage from /v3/account/usage"), Read.Usage.Num() == 2 && Read.Usage[1].Type == TEXT("mesh_decimate") && FMath::IsNearlyEqual(Read.Usage[1].Credits, 35.f));
		TestEqual(TEXT("with the key as a bearer"), *Auth, FString(TEXT("Bearer tripo-creator-test-key")));
		FHyperAIModelAccountRequest Limit;
		Limit.Limit = 1;
		const FHyperAIModelAccountReport Tool = UHyperAIStudioMeshGenToolset::hyper_tripo_account(Limit);
		FString Serialized;
		FJsonObjectConverter::UStructToJsonObjectString(Tool, Serialized);
		TestTrue(TEXT("hyper_tripo_account returns it, limited, and never the key"), Tool.Status == TEXT("ok") && Tool.Usage.Num() == 1 && !Serialized.Contains(TEXT("tripo-creator-test-key")));
	}
	bool bDone = false;
	FHyperAIModelAccountReport Missing;
	FPlatformMisc::SetEnvironmentVar(TEXT("TRIPO_API_KEY_HYPERAI_NONE"), TEXT(""));
	{
		UHyperAIStudioSettings* Studio = GetMutableDefault<UHyperAIStudioSettings>();
		const TArray<FHyperAIStudioExternalServiceSettings> Saved = Studio->ExternalServices;
		for (FHyperAIStudioExternalServiceSettings Service : UHyperAIStudioSettings::GetMeshServices())
		{
			if (Service.Id == TEXT("tripo") || Service.Id == TEXT("hi3d"))
			{
				Service.BaseUrl = Base + (Service.Id == TEXT("tripo") ? TEXT("/tripo/v3") : TEXT("/hi3d"));
				Service.ApiKeyVariable = TEXT("TRIPO_API_KEY_HYPERAI_NONE");
				Studio->ExternalServices.Add(Service);
			}
		}
		RefreshAccount([&bDone, &Missing](const FHyperAIModelAccountReport& Report) { Missing = Report; bDone = true; });
		Studio->ExternalServices = Saved;
	}
	TestTrue(TEXT("Without a key it says where to add one"), bDone && Missing.Status == TEXT("missing_key") && Missing.Diagnostic.Contains(TEXT("Keys")));
	TestTrue(TEXT("and so does Hi3D's balance"), Missing.Hi3DStatus == TEXT("missing_key") && Missing.Hi3DDiagnostic.Contains(TEXT("client_id:client_secret")));
	return true;
}


// ---------------------------------------------------------------- Hi3D and the reference picture

namespace HyperAIStudio::ModelCreatorTests
{
	/** Everything the log says while a test runs, to show no key or token reaches it. */
	struct FHi3DLogCatch : public FOutputDevice
	{
		FString Text;
		FCriticalSection Lock;
		virtual void Serialize(const TCHAR* Value, ELogVerbosity::Type, const FName&) override
		{
			FScopeLock Guard(&Lock);
			Text += Value;
			Text += TEXT("\n");
		}
	};

	/** What a stand-in saw: each call by path, its body (multipart as text), its Authorization header. */
	struct FHi3DSeen
	{
		TArray<FString> Calls;
		TMap<FString, TArray<FString>> Bodies;
		TArray<FString> Auth;
		int32 Tokens = 0;
		int32 Submits = 0;
		TMap<FString, int32> Polls;
	};

	FString AuthOf(const FHttpServerRequest& Request)
	{
		for (const TPair<FString, TArray<FString>>& Header : Request.Headers)
		{
			if (Header.Key.Equals(TEXT("Authorization"), ESearchCase::IgnoreCase) && !Header.Value.IsEmpty())
			{
				return Header.Value[0];
			}
		}
		return FString();
	}

	TUniquePtr<FHttpServerResponse> JsonReply(const FString& Text, const EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
	{
		TUniquePtr<FHttpServerResponse> Reply = FHttpServerResponse::Create(Text, TEXT("application/json"));
		Reply->Code = Code;
		return Reply;
	}

	/** A form field's value in a multipart body, as the stand-in received it. */
	FString FormField(const FString& Body, const FString& Name)
	{
		const FString Marker = FString::Printf(TEXT("name=\"%s\"\r\n\r\n"), *Name);
		const int32 At = Body.Find(Marker);
		if (At == INDEX_NONE)
		{
			return FString();
		}
		const int32 Start = At + Marker.Len();
		const int32 End = Body.Find(TEXT("\r\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		return Body.Mid(Start, End == INDEX_NONE ? MAX_int32 : End - Start);
	}

	FString WritePicture(const FString& Name)
	{
		const FString Relative = TEXT("Saved/HyperAIStudio/Tests/Creator/") + Name;
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Relative);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
		FFileHelper::SaveArrayToFile(Png(64, [](int32 X, int32 Y) { return (X - 32) * (X - 32) + (Y - 32) * (Y - 32) < 400 ? FColor(150, 110, 70) : FColor(240, 240, 240); }), *File);
		return Relative;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelCreatorHi3DPlanTest, "HyperAIStudio.MeshGen.Creator.Hi3D.PlanAndRequests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioModelCreatorHi3DPlanTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelCreatorTests;
	using HyperAIStudio::MeshGen::Private::RetryDelaySeconds;
	FString Error;

	// The model, resolution and face rules.
	FHi3DRequest Request;
	Request.Images = {TEXT("C:/x/front.png")};
	TestTrue(TEXT("v3.0 at 2048quality, 2,000,000 faces, one picture: valid"), Hi3DValidate(Request, Error));
	Request.Resolution = TEXT("1536pro");
	TestFalse(TEXT("v3.0 does not take 1536pro"), Hi3DValidate(Request, Error));
	Request.Resolution = TEXT("2048quality");
	Request.Faces = 99999;
	TestFalse(TEXT("under 100,000 faces is refused"), Hi3DValidate(Request, Error));
	Request.Faces = 5000000;
	TestFalse(TEXT("5,000,000 faces only at 2048master"), Hi3DValidate(Request, Error));
	Request.Resolution = TEXT("2048master");
	TestTrue(TEXT("5,000,000 at 2048master is valid"), Hi3DValidate(Request, Error));
	Request.Faces = 5000001;
	TestFalse(TEXT("but not more"), Hi3DValidate(Request, Error));
	FHi3DRequest Old;
	Old.Model = TEXT("hitem3dv1.5");
	Old.Resolution = TEXT("512");
	Old.Faces = 500000;
	Old.Images = {TEXT("C:/x/a.png")};
	TestFalse(TEXT("v1.5 has no PBR"), Hi3DValidate(Old, Error));
	Old.bPbr = false;
	TestTrue(TEXT("v1.5 at 512 without PBR"), Hi3DValidate(Old, Error));
	Old.Model = TEXT("hitem3dv9");
	TestFalse(TEXT("an unknown model is refused"), Hi3DValidate(Old, Error));
	FHi3DRequest Texture;
	Texture.RequestType = 2;
	Texture.Model = TEXT("hitem3dv2.1");
	Texture.Images = {TEXT("C:/x/a.png")};
	Texture.Mesh = TEXT("C:/x/m.glb");
	TestFalse(TEXT("texture only is offered on v3.0 and v1.5 alone"), Hi3DValidate(Texture, Error));
	Texture.Model = TEXT("hi3dv3.0");
	TestTrue(TEXT("on v3.0 it is valid"), Hi3DValidate(Texture, Error));
	Texture.Mesh = TEXT("C:/x/m.fbx");
	TestFalse(TEXT("its mesh is a GLB"), Hi3DValidate(Texture, Error));
	TestEqual(TEXT("Recommended faces by resolution"), FString::Printf(TEXT("%d %d %d %d %d"), Hi3DRecommendedFaces(TEXT("512")), Hi3DRecommendedFaces(TEXT("1024")),
		Hi3DRecommendedFaces(TEXT("1536fast")), Hi3DRecommendedFaces(TEXT("2048quality")), Hi3DRecommendedFaces(TEXT("2048master"))), FString(TEXT("500000 1000000 2000000 2000000 5000000")));
	TestEqual(TEXT("portrait v2.1 takes 1536fast and 1536pro (the docs' 1536profast)"), FString::Join(Hi3DResolutions(TEXT("scene-portraitv2.1")), TEXT(",")), FString(TEXT("1536fast,1536pro")));

	// The multipart fields.
	TArray<TPair<FString, FString>> Text, Files;
	FHi3DRequest Whole;
	Whole.Faces = 100000;
	Whole.Images = {TEXT("C:/x/front.png")};
	Hi3DFields(Whole, Text, Files);
	auto Value = [&Text](const TCHAR* Name) { const TPair<FString, FString>* Found = Text.FindByPredicate([Name](const TPair<FString, FString>& Pair) { return Pair.Key == Name; }); return Found ? Found->Value : FString(TEXT("-")); };
	TestEqual(TEXT("Fields: request_type, model, resolution, face, pbr, shading, rmbg, format"), FString::Printf(TEXT("%s %s %s %s %s %s %s %s"), *Value(TEXT("request_type")), *Value(TEXT("model")),
		*Value(TEXT("resolution")), *Value(TEXT("face")), *Value(TEXT("pbr")), *Value(TEXT("shading")), *Value(TEXT("rmbg")), *Value(TEXT("format"))),
		FString(TEXT("3 hi3dv3.0 2048quality 100000 1 0.5 1 2")));
	TestTrue(TEXT("one picture goes as images"), Files.Num() == 1 && Files[0].Key == TEXT("images") && Value(TEXT("multi_images_bit")) == TEXT("-"));
	Text.Reset();
	Files.Reset();
	FHi3DRequest Views = Whole;
	Views.bMultiview = true;
	Views.Images = Hi3DViewOrder({TEXT("C:/f.png"), TEXT(""), TEXT("C:/b.png"), TEXT("C:/r.png")});
	Hi3DFields(Views, Text, Files);
	TestTrue(TEXT("views go as multi_images in Hi3D's order (front, back, left, right), multi_images_bit counting them"), Value(TEXT("multi_images_bit")) == TEXT("1101")
		&& Files.Num() == 3 && Files[0].Value == TEXT("C:/f.png") && Files[1].Value == TEXT("C:/b.png") && Files[2].Value == TEXT("C:/r.png") && Files[2].Key == TEXT("multi_images"));
	Text.Reset();
	Files.Reset();
	Texture.Mesh = TEXT("C:/x/m.glb");
	Hi3DFields(Texture, Text, Files);
	TestTrue(TEXT("texture only: request_type 2, the mesh as a file, no face or resolution"), Value(TEXT("request_type")) == TEXT("2") && Value(TEXT("face")) == TEXT("-")
		&& Value(TEXT("resolution")) == TEXT("-") && Files.ContainsByPredicate([](const TPair<FString, FString>& Pair) { return Pair.Key == TEXT("mesh"); }));
	TestEqual(TEXT("The token call's header is Basic base64(client_id:client_secret)"), Hi3DBasicAuth(TEXT(" ak_123:sk_456 ")), TEXT("Basic ") + FBase64::Encode(TEXT("ak_123:sk_456")));

	// Every documented error code says what it means.
	for (const TCHAR* Code : {TEXT("30010000"), TEXT("10031001"), TEXT("10031002"), TEXT("10031003"), TEXT("10031005"), TEXT("10031006"), TEXT("10031007"), TEXT("10031008"),
		TEXT("10031009"), TEXT("10031011"), TEXT("10031016"), TEXT("10031017"), TEXT("10000000"), TEXT("50010001"), TEXT("40010000")})
	{
		TestFalse(FString::Printf(TEXT("code %s is put into words"), Code), Hi3DCodeMessage(Code).IsEmpty());
	}
	TestTrue(TEXT("balance, faces, texture and refund say so"), Hi3DCodeMessage(TEXT("30010000")).Contains(TEXT("balance")) && Hi3DCodeMessage(TEXT("10031002")).Contains(TEXT("face"))
		&& Hi3DCodeMessage(TEXT("10031017")).Contains(TEXT("hi3dv3.0")) && Hi3DCodeMessage(TEXT("50010001")).Contains(TEXT("refunded")));
	TestTrue(TEXT("A 401 or a 4001 code asks for a new token"), Hi3DIsAuthError(401, TEXT("")) && Hi3DIsAuthError(200, TEXT("40010001")) && !Hi3DIsAuthError(200, TEXT("10031003")));

	// Prices, in Hi3D credits at $0.02, from MeshTaskPrices.
	TestEqual(TEXT("Hi3D credits: v3.0 45 / 145, v2.1 fast 25, v2.0 1536 35, v1.5 512 15 and 1536pro 40, portrait v1.5 30"),
		FString::Printf(TEXT("%d %d %d %d %d %d %d"), Hi3DCredits(Hi3DPrice(TEXT("hi3dv3.0"), TEXT("2048quality"), 3, true)), Hi3DCredits(Hi3DPrice(TEXT("hi3dv3.0"), TEXT("2048master"), 3, true)),
			Hi3DCredits(Hi3DPrice(TEXT("hitem3dv2.1"), TEXT("1536fast"), 3, true)), Hi3DCredits(Hi3DPrice(TEXT("hitem3dv2.0"), TEXT("1536"), 3, true)),
			Hi3DCredits(Hi3DPrice(TEXT("hitem3dv1.5"), TEXT("512"), 3, false)), Hi3DCredits(Hi3DPrice(TEXT("hitem3dv1.5"), TEXT("1536pro"), 3, false)),
			Hi3DCredits(Hi3DPrice(TEXT("scene-portraitv1.5"), TEXT("1536"), 3, false))),
		FString(TEXT("45 145 25 35 15 40 30")));
	TestEqual(TEXT("Geometry alone: v3.0 30, master 130; without PBR 40; texture only 10, with PBR 15"),
		FString::Printf(TEXT("%d %d %d %d %d"), Hi3DCredits(Hi3DPrice(TEXT("hi3dv3.0"), TEXT("2048quality"), 1, false)), Hi3DCredits(Hi3DPrice(TEXT("hi3dv3.0"), TEXT("2048master"), 1, false)),
			Hi3DCredits(Hi3DPrice(TEXT("hi3dv3.0"), TEXT("2048quality"), 3, false)), Hi3DCredits(Hi3DTexturePrice(false)), Hi3DCredits(Hi3DTexturePrice(true))),
		FString(TEXT("30 130 40 10 15")));
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FHyperAIStudioMeshTaskPrice> SavedPrices = Settings->MeshTaskPrices;
	FHyperAIStudioMeshTaskPrice Own;
	Own.Provider = TEXT("hi3d");
	Own.Task = TEXT("hi3dv3.0_2048quality");
	Own.Cost = 1.00f;
	Settings->MeshTaskPrices.Add(Own);
	TestEqual(TEXT("A Hi3D price set in MeshTaskPrices replaces the built-in one"), Hi3DCredits(Hi3DPrice(TEXT("hi3dv3.0"), TEXT("2048quality"), 3, true)), 50);
	Settings->MeshTaskPrices = SavedPrices;

	// The provider, chosen per brief.
	const FString Picture = WritePicture(TEXT("hi3d_plan.png"));
	struct FChoice
	{
		const TCHAR* Prompt;
		const TCHAR* Use;
		bool bPicture;
		const TCHAR* Topology;
		const TCHAR* Platform;
		int32 Faces;
		bool bRig;
		const TCHAR* Expected;
	};
	const FChoice Choices[] = {
		{TEXT("a carved stone altar"), TEXT("hero_prop"), true, TEXT(""), TEXT(""), 0, false, TEXT("hi3d")},
		{TEXT("a mossy cliff"), TEXT("environment"), true, TEXT(""), TEXT(""), 0, false, TEXT("hi3d")},
		{TEXT("a rusty pickup truck"), TEXT(""), true, TEXT(""), TEXT(""), 0, false, TEXT("hi3d")},
		{TEXT("a photoreal leather boot"), TEXT("prop"), true, TEXT(""), TEXT(""), 0, false, TEXT("hi3d")},
		{TEXT("a marble bust of an emperor"), TEXT("prop"), true, TEXT(""), TEXT(""), 0, false, TEXT("hi3d")},
		{TEXT("a carved stone altar"), TEXT("hero_prop"), false, TEXT(""), TEXT(""), 0, false, TEXT("tripo")},
		{TEXT("a wooden crate"), TEXT("prop"), true, TEXT(""), TEXT(""), 0, false, TEXT("tripo")},
		{TEXT("a carved stone altar"), TEXT("hero_prop"), true, TEXT("quads"), TEXT(""), 0, false, TEXT("tripo")},
		{TEXT("a carved stone altar"), TEXT("hero_prop"), true, TEXT(""), TEXT("mobile"), 0, false, TEXT("tripo")},
		{TEXT("a carved stone altar"), TEXT("hero_prop"), true, TEXT(""), TEXT(""), 20000, false, TEXT("tripo")},
		{TEXT("a photoreal knight"), TEXT("character"), true, TEXT(""), TEXT(""), 0, false, TEXT("tripo")},
		{TEXT("a carved stone altar"), TEXT("hero_prop"), true, TEXT(""), TEXT(""), 0, true, TEXT("tripo")},
	};
	for (const FChoice& Row : Choices)
	{
		FHyperAIModelBrief Brief;
		Brief.Prompt = Row.Prompt;
		Brief.Use = Row.Use;
		Brief.ImagePath = Row.bPicture ? Picture : FString();
		Brief.Topology = Row.Topology;
		Brief.Platform = Row.Platform;
		Brief.FaceLimit = Row.Faces;
		FString Why;
		TestEqual(FString::Printf(TEXT("\"%s\" (%s%s%s%s) goes to"), Row.Prompt, Row.Use, Row.bPicture ? TEXT(", picture") : TEXT(", words"), Row.Topology, Row.Platform),
			ChooseProvider(Brief, Row.bRig, Why, Error), FString(Row.Expected));
	}

	// Hi3D plans.
	FHyperAIModelBrief Hero;
	Hero.Prompt = TEXT("a carved stone altar");
	Hero.Use = TEXT("hero_prop");
	Hero.ImagePath = Picture;
	FHyperAIModelPlan Plan;
	TestTrue(TEXT("A picture of a hero prop classifies to Hi3D: ") + Error, Classify(Hero, Plan, Error));
	TestTrue(TEXT("v3.0 at 2048quality, Hi3D's 2,000,000 faces kept dense with Nanite, PBR, 45 credits"), Plan.Provider == TEXT("hi3d") && Plan.Model == TEXT("hi3dv3.0")
		&& Plan.Resolution == TEXT("2048quality") && Plan.ProviderFaces == 2000000 && Plan.FaceLimit == 2000000 && Plan.bNanite && !Plan.bLocalReduce && Plan.bPbr && Plan.Credits == 45
		&& Plan.Endpoint == Hi3DSubmitPath && Plan.RequestJson.Contains(TEXT("\"face\":\"2000000\"")));
	FHyperAIModelBrief Master = Hero;
	Master.Prompt = TEXT("an extreme detail carved altar, master quality");
	TestTrue(TEXT("An extreme or master brief gets 2048master (145)"), Classify(Master, Plan, Error) && Plan.Resolution == TEXT("2048master") && Plan.Credits == 145);
	FHyperAIModelBrief Prop = Hero;
	Prop.Provider = TEXT("hi3d");
	Prop.Use = TEXT("prop");
	Prop.FaceLimit = 20000;
	TestTrue(TEXT("Under 100,000: made at 100,000, then reduced in the editor to the budget"), Classify(Prop, Plan, Error) && Plan.ProviderFaces == 100000 && Plan.FaceLimit == 20000
		&& Plan.bLocalReduce && TargetsFromPlan(Plan).bLocalReduce);
	FHyperAIModelBrief Bust = Hero;
	Bust.Prompt = TEXT("a bronze bust of a general");
	Bust.Use = TEXT("prop");
	TestTrue(TEXT("A bust from a picture: Hi3D's portrait model"), Classify(Bust, Plan, Error) && Plan.Model == TEXT("scene-portraitv2.1") && Plan.Resolution == TEXT("1536pro"));
	FHyperAIModelBrief Quads = Prop;
	Quads.Topology = TEXT("quads");
	TestFalse(TEXT("Hi3D with quads is refused"), Classify(Quads, Plan, Error));
	FHyperAIModelBrief Wrong = Prop;
	Wrong.Resolution = TEXT("1536fast");
	TestFalse(TEXT("A resolution its model does not take is refused"), Classify(Wrong, Plan, Error));
	FHyperAIModelBrief Words;
	Words.Prompt = TEXT("an ornate brass lantern");
	Words.Provider = TEXT("hi3d");
	TestTrue(TEXT("Hi3D from words: a reference picture first, then Hi3D"), Classify(Words, Plan, Error) && Plan.Provider == TEXT("hi3d") && Plan.bReferenceFirst
		&& Plan.ReferenceSource == TEXT("tripo") && Plan.PriceLines.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("reference picture")); }));
	UseImageGenReference(Plan);
	TestTrue(TEXT("and without a Tripo key HyperAI's image generation makes it"), Plan.ReferenceSource == TEXT("imagegen") && !Plan.ImageModel.IsEmpty());
	Words.ReferenceImage = TEXT("off");
	TestFalse(TEXT("Hi3D from words with the picture off is refused"), Classify(Words, Plan, Error));

	// The ladder for a Hi3D model.
	FFixContext Context;
	Context.Provider = TEXT("hi3d");
	Context.bTripo = false;
	Context.InputToken = TEXT("{input}");
	Context.RemainingUSD = 5.f;
	Context.bCanRegenerate = true;
	Context.Hi3DImages = {TEXT("C:/x/front.png")};
	Classify(Hero, Context.Plan, Error);
	FHyperAIModelCheckReport Failing;
	for (const TCHAR* Id : {TEXT("textures"), TEXT("visual"), TEXT("faces")})
	{
		FHyperAIModelCheck& Check = Failing.Checks.AddDefaulted_GetRef();
		Check.Id = Id;
		Check.Result = TEXT("fail");
	}
	TArray<FPaidFix> Ladder = PaidLadder(Failing, Context);
	TestTrue(TEXT("Hi3D's fixes: texture only (15) before a resolution up (145); no Tripo fix without its key"), Ladder.Num() == 2 && Ladder[0].Id == TEXT("hi3d_texture")
		&& Ladder[0].Hi3D.RequestType == 2 && Hi3DCredits(Ladder[0].PriceUSD) == 15 && Ladder[1].Id == TEXT("hi3d_regenerate") && Ladder[1].Hi3D.Resolution == TEXT("2048master"));
	Context.bTripo = true;
	Ladder = PaidLadder(Failing, Context);
	TestTrue(TEXT("With Tripo's key, Tripo's retopology on the uploaded GLB is offered too, first for faces"), !Ladder.IsEmpty() && Ladder[0].Id == TEXT("retopo")
		&& Ladder[0].Bodies[0]->GetStringField(TEXT("input")) == TEXT("{input}") && !Ladder.ContainsByPredicate([](const FPaidFix& Fix) { return Fix.Id == TEXT("regenerate"); }));

	// The reference picture: names, models, sizes, prices.
	FString Id, Note;
	TestTrue(TEXT("gpt-image-2.5-sunburn is chat_image_2.5_sunburst, and says so"), ImageModelFromName(TEXT("gpt-image-2.5-sunburn"), Id, Note) && Id == TEXT("chat_image_2.5_sunburst")
		&& Note.Contains(TEXT("gpt-image-2.5-sunburn")));
	TestTrue(TEXT("nano banana pro is banana_pro"), ImageModelFromName(TEXT("Nano Banana Pro"), Id, Note) && Id == TEXT("banana_pro"));
	TestTrue(TEXT("nano banana 2 is banana2"), ImageModelFromName(TEXT("nano-banana-2"), Id, Note) && Id == TEXT("banana2"));
	TestTrue(TEXT("seedream 5 is seedream_v5"), ImageModelFromName(TEXT("Seedream 5"), Id, Note) && Id == TEXT("seedream_v5"));
	TestTrue(TEXT("chatgpt image with no version is the newest, flare"), ImageModelFromName(TEXT("chatgpt image"), Id, Note) && Id == TEXT("chat_image_2.5_flare"));
	TestTrue(TEXT("a tier misspelt within two letters: flair is flare"), ImageModelFromName(TEXT("chat_image_2.5_flair"), Id, Note) && Id == TEXT("chat_image_2.5_flare"));
	TestFalse(TEXT("chat_image_1 is retired"), ImageModelFromName(TEXT("chat_image_1"), Id, Note));
	TestTrue(TEXT("chat_image_1.5 is taken with a warning"), ImageModelFromName(TEXT("chat_image_1.5"), Id, Note) && Note.Contains(TEXT("retires")));
	TestTrue(TEXT("anything else is refused with the list"), !ImageModelFromName(TEXT("dall-e-3"), Id, Note) && Note.Contains(TEXT("chat_image_2.5_sunburst")));
	TestTrue(TEXT("quality: chat_image_2 low/medium/high, 2.5 also xhigh and max, others none"), ImageModelTakesQuality(TEXT("chat_image_2"), TEXT("high"))
		&& !ImageModelTakesQuality(TEXT("chat_image_2"), TEXT("xhigh")) && ImageModelTakesQuality(TEXT("chat_image_2.5_flare"), TEXT("max")) && !ImageModelTakesQuality(TEXT("banana2"), TEXT("high")));
	TestTrue(TEXT("sizes: presets, custom WxH in multiples of 16 within the pixel range, seedream's K sizes"), ImageSizeValid(TEXT("chat_image_2.5_flare"), TEXT("1024x1024"), Error)
		&& ImageSizeValid(TEXT("chat_image_2.5_flare"), TEXT("1280x720"), Error) && !ImageSizeValid(TEXT("chat_image_2.5_flare"), TEXT("1000x1000"), Error)
		&& !ImageSizeValid(TEXT("chat_image_2.5_flare"), TEXT("3840x1024"), Error) && ImageSizeValid(TEXT("seedream_v5"), TEXT("3K"), Error) && !ImageSizeValid(TEXT("seedream_v5"), TEXT("4K"), Error));
	TestEqual(TEXT("Picture prices: sunburst high 20 / 25 / 40, xhigh 2K 30, max 4K 50, auto bills 2K, seedream 5, banana_pro 4K 20"),
		FString::Printf(TEXT("%d %d %d %d %d %d %d %d"), Credits(ImagePrice(TEXT("chat_image_2.5_sunburst"), TEXT("high"), TEXT("1024x1024"))),
			Credits(ImagePrice(TEXT("chat_image_2.5_sunburst"), TEXT("high"), TEXT("2048x2048"))), Credits(ImagePrice(TEXT("chat_image_2.5_sunburst"), TEXT("high"), TEXT("3840x2160"))),
			Credits(ImagePrice(TEXT("chat_image_2.5_flare"), TEXT("xhigh"), TEXT("2048x1152"))), Credits(ImagePrice(TEXT("chat_image_2.5_flare"), TEXT("max"), TEXT("3840x2160"))),
			Credits(ImagePrice(TEXT("chat_image_2.5_flare"), TEXT("high"), TEXT("auto"))), Credits(ImagePrice(TEXT("seedream_v5"), TEXT(""), TEXT("2K"))),
			Credits(ImagePrice(TEXT("banana_pro"), TEXT(""), TEXT("4K")))),
		FString(TEXT("20 25 40 30 50 25 5 20")));
	FHyperAIModelBrief Lamp;
	Lamp.Prompt = TEXT("a brass oil lamp");
	Lamp.Use = TEXT("prop");
	Lamp.ImageModel = TEXT("banana2");
	TestTrue(TEXT("A picture from banana2: no quality or background sent, a plain grey background asked for in the words"), Classify(Lamp, Plan, Error) && [&]()
	{
		const TSharedRef<FJsonObject> Body = TextToImageBody(Plan);
		return !Body->HasField(TEXT("quality")) && !Body->HasField(TEXT("background")) && !Body->HasField(TEXT("size")) && Body->GetStringField(TEXT("prompt")).Contains(TEXT("light-grey"));
	}());
	Lamp.ImageQuality = TEXT("high");
	TestFalse(TEXT("quality on a model that takes none is refused"), Classify(Lamp, Plan, Error));

	// Tripo's fixes, prices, errors and retries.
	FHyperAIModelBrief Edited;
	Edited.ImagePath = Picture;
	Edited.Model = TEXT("H3.1");
	Edited.ViewEdits = {TEXT("back: a quiver"), TEXT("front: a red cloak"), TEXT("left: a scar")};
	TestTrue(TEXT("Edit-multiview costs 5 credits an edited view: 3 edits, 15"), Classify(Edited, Plan, Error) && Plan.PriceLines.ContainsByPredicate([](const FString& Line)
	{
		return Line.Contains(TEXT("edit-multiview, 3")) && Line.Contains(TEXT(": 15 credits"));
	}));
	TSharedRef<FJsonObject> Body = TextureBody(TEXT("task_1"), TEXT("standard"), true, {}, TEXT("ignored"), TEXT("task_ref"), 77, TEXT("original_image"));
	TestTrue(TEXT("texture: the reference as texture_prompt.image, texture_seed, texture_alignment"), Body->GetObjectField(TEXT("texture_prompt"))->GetStringField(TEXT("image")) == TEXT("task_ref")
		&& !Body->GetObjectField(TEXT("texture_prompt"))->HasField(TEXT("text")) && Body->GetIntegerField(TEXT("texture_seed")) == 77 && Body->GetStringField(TEXT("texture_alignment")) == TEXT("original_image"));
	FFixContext Tripo;
	Tripo.InputTaskId = TEXT("task_gen");
	Tripo.RemainingUSD = 5.f;
	Tripo.ReferenceInput = TEXT("task_ref");
	FHyperAIModelBrief Crate;
	Crate.Prompt = TEXT("a crate");
	Classify(Crate, Tripo.Plan, Error);
	FHyperAIModelCheckReport NoTexture;
	FHyperAIModelCheck& Missing = NoTexture.Checks.AddDefaulted_GetRef();
	Missing.Id = TEXT("textures");
	Missing.Result = TEXT("fail");
	Tripo.Tried.Add(TEXT("texture"), 1);
	Ladder = PaidLadder(NoTexture, Tripo);
	TestTrue(TEXT("The first texture retry keeps the tier and price and changes the seed"), Ladder.Num() == 1 && Credits(Ladder[0].PriceUSD) == 10 && Ladder[0].Bodies[0]->HasField(TEXT("texture_seed"))
		&& Ladder[0].Bodies[0]->GetObjectField(TEXT("texture_prompt"))->GetStringField(TEXT("image")) == TEXT("task_ref"));
	Tripo.Tried.Add(TEXT("texture"), 2);
	Ladder = PaidLadder(NoTexture, Tripo);
	TestTrue(TEXT("the next goes a tier up"), Ladder.Num() == 1 && Credits(Ladder[0].PriceUSD) == 20);
	HyperAIStudio::MeshGen::Private::FJob Job;
	Job.Service.Label = TEXT("Tripo 3D");
	auto Says = [&Job](const int32 Code, const TCHAR* Json) { return HyperAIStudio::MeshGen::Private::ProviderError(Job, Code, HyperAIStudio::MeshGen::Private::Utf8(Json)); };
	TestTrue(TEXT("Tripo's codes: 2010 credits, 2008 content policy, 2015 retired, 2018 too complex"), Says(403, TEXT(R"({"code":2010,"message":"no"})")).Contains(TEXT("out of credits"))
		&& Says(400, TEXT(R"({"code":2008})")).Contains(TEXT("content policy")) && Says(400, TEXT(R"({"code":2015})")).Contains(TEXT("retired"))
		&& Says(400, TEXT(R"({"code":2018})")).Contains(TEXT("decimate")) && Says(400, TEXT(R"({"code":1004,"message":"face_limit too large"})")).Contains(TEXT("face_limit too large")));
	const double Scale = HyperAIStudio::MeshGen::Private::RetryScale();
	HyperAIStudio::MeshGen::Private::RetryScale() = 1.0;
	TestEqual(TEXT("Retries back off 1, 2, 4, 8, 16, 32, 32 seconds"), FString::Printf(TEXT("%.0f %.0f %.0f %.0f %.0f %.0f %.0f"), RetryDelaySeconds(0, TEXT(""), TEXT("")),
		RetryDelaySeconds(1, TEXT(""), TEXT("")), RetryDelaySeconds(2, TEXT(""), TEXT("")), RetryDelaySeconds(3, TEXT(""), TEXT("")), RetryDelaySeconds(4, TEXT(""), TEXT("")),
		RetryDelaySeconds(5, TEXT(""), TEXT("")), RetryDelaySeconds(9, TEXT(""), TEXT(""))), FString(TEXT("1 2 4 8 16 32 32")));
	TestTrue(TEXT("and honour Retry-After and X-RateLimit-Reset"), FMath::IsNearlyEqual(RetryDelaySeconds(0, TEXT("7"), TEXT("")), 7.0)
		&& FMath::IsNearlyEqual(RetryDelaySeconds(0, TEXT(""), TEXT("3")), 3.0));
	HyperAIStudio::MeshGen::Private::RetryScale() = Scale;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelCreatorHi3DJobTest, "HyperAIStudio.MeshGen.Creator.Hi3D.Job",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioModelCreatorHi3DJobTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelCreatorTests;
	constexpr uint32 Port = 18355;
	const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
	if (!Router.IsValid())
	{
		AddWarning(TEXT("Port 18355 is taken, so the stand-in for Hi3D could not start."));
		return true;
	}
	const FString Base = FString::Printf(TEXT("http://127.0.0.1:%u"), Port);
	const FString Secret = TEXT("hi3d-client-id:hi3d-client-secret");
	const TSharedRef<FHi3DSeen> Seen = MakeShared<FHi3DSeen>();
	FFixture Dense;
	Dense.Grid = 12;
	const TSharedRef<TArray<uint8>> Generated = MakeShared<TArray<uint8>>(CubeGlb(Dense));
	FFixture Painted;
	Painted.Grid = 6;
	Painted.bTextured = true;
	const TSharedRef<TArray<uint8>> Textured = MakeShared<TArray<uint8>>(CubeGlb(Painted));
	const TSharedRef<TArray<uint8>> Cover = MakeShared<TArray<uint8>>(Png(32, [](int32, int32) { return FColor(150, 110, 70); }));
	TArray<FHttpRouteHandle> Routes;
	auto Bind = [&](const TCHAR* Path, const EHttpServerRequestVerbs Verb, TFunction<TUniquePtr<FHttpServerResponse>(const FHttpServerRequest&)> Answer)
	{
		Routes.Add(Router->BindRoute(FHttpPath(Path), Verb, FHttpRequestHandler::CreateLambda([Seen, Path = FString(Path), Answer](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			Seen->Calls.Add(Path);
			Seen->Bodies.FindOrAdd(Path).Add(BodyText(Request).Left(6000));
			Seen->Auth.Add(Path + TEXT(" ") + AuthOf(Request));
			OnComplete(Answer(Request));
			return true;
		})));
	};
	Bind(TEXT("/hi3d/open-api/v1/auth/token"), EHttpServerRequestVerbs::VERB_POST, [Seen](const FHttpServerRequest&)
	{
		++Seen->Tokens;
		return JsonReply(FString::Printf(TEXT(R"({"code":200,"data":{"accessToken":"hi3d-token-%d","tokenType":"Bearer","nonce":"n"}})"), Seen->Tokens));
	});
	Bind(TEXT("/hi3d/open-api/v1/submit-task"), EHttpServerRequestVerbs::VERB_POST, [Seen](const FHttpServerRequest&)
	{
		++Seen->Submits;
		const TCHAR* const Ids[] = {TEXT("t_gen"), TEXT("t_tex1"), TEXT("t_tex2"), TEXT("t_more")};
		return JsonReply(FString::Printf(TEXT(R"({"code":200,"data":{"task_id":"%s"},"msg":"success"})"), Ids[FMath::Min(Seen->Submits - 1, 3)]));
	});
	Bind(TEXT("/hi3d/open-api/v1/query-task"), EHttpServerRequestVerbs::VERB_GET, [Seen, Base](const FHttpServerRequest& Request)
	{
		const FString Task = Request.QueryParams.FindRef(TEXT("task_id"));
		const int32 Poll = Seen->Polls.FindOrAdd(Task)++;
		// The first token "expires" before the generation is asked after: a 401, a new token, the same question again.
		if (AuthOf(Request) == TEXT("Bearer hi3d-token-1") && Task == TEXT("t_gen"))
		{
			return JsonReply(TEXT(R"({"code":401,"msg":"token expired"})"), EHttpServerResponseCodes::Denied);
		}
		if (Task == TEXT("t_tex1"))
		{
			return JsonReply(TEXT(R"({"code":50010001,"msg":"generate failed"})"));
		}
		if (Poll == 0)
		{
			return JsonReply(FString::Printf(TEXT(R"({"code":200,"data":{"task_id":"%s","state":"processing"}})"), *Task));
		}
		return JsonReply(FString::Printf(TEXT(R"({"code":200,"data":{"task_id":"%s","state":"success","url":"%s/hi3d/files/%s.glb","cover_url":"%s/hi3d/files/cover.png"}})"),
			*Task, *Base, Task == TEXT("t_gen") ? TEXT("gen") : TEXT("tex"), *Base));
	});
	Bind(TEXT("/hi3d/open-api/v1/balance"), EHttpServerRequestVerbs::VERB_GET, [](const FHttpServerRequest&) { return JsonReply(TEXT(R"({"code":200,"data":{"totalBalance":512}})")); });
	Bind(TEXT("/hi3d/files/gen.glb"), EHttpServerRequestVerbs::VERB_GET, [Generated](const FHttpServerRequest&) { return FHttpServerResponse::Create(TArray<uint8>(*Generated), TEXT("model/gltf-binary")); });
	Bind(TEXT("/hi3d/files/tex.glb"), EHttpServerRequestVerbs::VERB_GET, [Textured](const FHttpServerRequest&) { return FHttpServerResponse::Create(TArray<uint8>(*Textured), TEXT("model/gltf-binary")); });
	Bind(TEXT("/hi3d/files/cover.png"), EHttpServerRequestVerbs::VERB_GET, [Cover](const FHttpServerRequest&) { return FHttpServerResponse::Create(TArray<uint8>(*Cover), TEXT("image/png")); });
	// Tripo's text-to-image, for Hi3D from words.
	Bind(TEXT("/tripo/v3/generation/text-to-image"), EHttpServerRequestVerbs::VERB_POST, [](const FHttpServerRequest&) { return JsonReply(TEXT(R"({"code":0,"data":{"task_id":"task_lantern"}})")); });
	Bind(TEXT("/tripo/v3/tasks/task_lantern"), EHttpServerRequestVerbs::VERB_GET, [Base](const FHttpServerRequest&)
	{
		return JsonReply(FString::Printf(TEXT(R"({"code":0,"data":{"status":"success","progress":100,"credits_consumed":20,"output":{"generated_image_url":"%s/hi3d/files/cover.png"}}})"), *Base));
	});
	FHttpServerModule::Get().StartAllListeners();

	FStandIn StandIn(Base);
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_HI3D_KEY"), *Secret);
	// Only Hi3D's key: Tripo's fixes are not offered on its model.
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_TRIPO_KEY"), TEXT(""));
	FHi3DLogCatch Logged;
	GLog->AddOutputDevice(&Logged);
	TArray<FString> Operations;
	TArray<FString> Cleanup;
	ON_SCOPE_EXIT
	{
		GLog->RemoveOutputDevice(&Logged);
		for (const FHttpRouteHandle& Route : Routes)
		{
			Router->UnbindRoute(Route);
		}
		for (const FString& Operation : Operations)
		{
			IFileManager::Get().DeleteDirectory(*(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Models") / Operation), false, true);
		}
		DeleteAll(Cleanup);
	};

	FHyperAIModelCreateRequest Request;
	Request.Brief.Prompt = TEXT("a carved wooden chest");
	Request.Brief.Provider = TEXT("hi3d");
	Request.Brief.Use = TEXT("prop");
	Request.Brief.ImagePath = WritePicture(TEXT("hi3d_chest.png"));
	Request.Brief.FaceLimit = 600;
	Request.Brief.SizeMeters = 1.f;
	Request.FixBudgetUSD = 2.f;
	Request.MaxRounds = 3;
	Request.bDryRun = false;
	Request.OperationId = TEXT("model-hi3d-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	Operations.Add(Request.OperationId);
	const FHyperAIModelCreateReport Asked = Create(Request);
	if (!TestEqual(TEXT("A Hi3D job waits for approval, priced in Hi3D credits: ") + Asked.Diagnostic, Asked.Status, FString(TEXT("awaiting_user_approval"))))
	{
		return false;
	}
	TestTrue(TEXT("its plan: Hi3D v3.0, made at 100,000 faces and reduced to 600 in the editor, 45 credits"), Asked.Plan.Provider == TEXT("hi3d") && Asked.Plan.ProviderFaces == 100000
		&& Asked.Plan.FaceLimit == 600 && Asked.Plan.bLocalReduce && Asked.Plan.Credits == 45 && Asked.Diagnostic.Contains(TEXT("Hi3D credits")));
	FString Error;
	TestTrue(TEXT("Approved"), FHyperAIStudioApprovalGate::Approve(Asked.OperationId, Error));
	FHyperAIModelJob Job;
	TickUntil([&]() { Job = JobOf(Asked.OperationId); return Job.State == TEXT("awaiting_fix_approval") || Job.State == TEXT("completed") || Job.State == TEXT("stopped") || Job.State == TEXT("failed"); }, 60.0);
	AddInfo(FString::Printf(TEXT("After generation: %s, %s"), *Job.State, *Job.Diagnostic));
	for (const FHyperAIModelStep& Step : Job.Steps)
	{
		AddInfo(FString::Printf(TEXT("  round %d %s %s: %s (%s)"), Step.Round, *Step.Kind, *Step.Status, *Step.What.Left(160), *Step.Detail.Left(300)));
	}
	Cleanup.Add(Job.AssetPath);

	// The token flow: Basic with the key, then Bearer; a refused token traded for a new one.
	TestTrue(TEXT("The token call sent Basic base64(client_id:client_secret)"), Seen->Auth.Contains(TEXT("/hi3d/open-api/v1/auth/token Basic ") + FBase64::Encode(Secret)));
	TestTrue(TEXT("the submit sent the token as a Bearer"), Seen->Auth.Contains(TEXT("/hi3d/open-api/v1/submit-task Bearer hi3d-token-1")));
	TestTrue(TEXT("and after the 401 a new token was asked for and used"), Seen->Tokens == 2 && Seen->Auth.Contains(TEXT("/hi3d/open-api/v1/query-task Bearer hi3d-token-2")));
	const FString Form = Seen->Bodies.FindRef(TEXT("/hi3d/open-api/v1/submit-task")).IsEmpty() ? FString() : Seen->Bodies.FindRef(TEXT("/hi3d/open-api/v1/submit-task"))[0];
	TestEqual(TEXT("The multipart form: request_type 3, hi3dv3.0, 2048quality, 100,000 faces, PBR, glb"), FString::Printf(TEXT("%s %s %s %s %s %s"), *FormField(Form, TEXT("request_type")),
		*FormField(Form, TEXT("model")), *FormField(Form, TEXT("resolution")), *FormField(Form, TEXT("face")), *FormField(Form, TEXT("pbr")), *FormField(Form, TEXT("format"))),
		FString(TEXT("3 hi3dv3.0 2048quality 100000 1 2")));
	TestTrue(TEXT("with the picture as images"), Form.Contains(TEXT("name=\"images\"; filename=\"hi3d_chest.png\"")) && !Form.Contains(TEXT("multi_images")));

	// Polled, downloaded, imported, checked; the 100,000 faces reduced for free to the budget.
	TestTrue(TEXT("Imported from Hi3D's GLB, with its cover for the visual check"), !Job.AssetPath.IsEmpty() && !Job.PreviewImages.IsEmpty() && Job.Provider == TEXT("hi3d"));
	TestTrue(TEXT("The budget was met by the free simplify"), Job.Steps.ContainsByPredicate([](const FHyperAIModelStep& Step)
	{
		return Step.Kind == TEXT("local_fix") && Step.What.Contains(TEXT("simplify")) && Step.Status == TEXT("done");
	}) && Job.LastCheck.Triangles <= 630 && ResultOf(Job.LastCheck, TEXT("faces")) != TEXT("fail") && Job.FirstCheck.Triangles > 1000);
	TestTrue(TEXT("and it stays closed: the seams were welded before the reduction"), ResultOf(Job.LastCheck, TEXT("holes")) != TEXT("fail")
		&& ResultOf(Job.LastCheck, TEXT("open_edges")) != TEXT("fail"));

	// Untextured: Hi3D's texture-only fix, priced in Hi3D credits.
	const FHyperAIModelStep* Offer = Job.Steps.FindByPredicate([](const FHyperAIModelStep& Step) { return Step.Kind == TEXT("paid_fix") && Step.Status == TEXT("awaiting_user_approval"); });
	TestTrue(TEXT("Missing textures: Hi3D's texture-only fix is offered at 15 Hi3D credits, and nothing of Tripo's"), Job.State == TEXT("awaiting_fix_approval") && Offer
		&& Offer->What.Contains(TEXT("Hi3D")) && Offer->Credits == 15 && Offer->Endpoint == Hi3DSubmitPath);
	if (!Offer)
	{
		return false;
	}
	TestTrue(TEXT("Approved"), FHyperAIStudioApprovalGate::Approve(Job.CurrentOperationId, Error));
	// The texture task fails with 50010001 (refunded): a retry is offered, priced and approved.
	TickUntil([&]() { Job = JobOf(Asked.OperationId); return Job.Steps.ContainsByPredicate([](const FHyperAIModelStep& Step) { return Step.What.StartsWith(TEXT("Try again")); })
		|| Job.State == TEXT("completed") || Job.State == TEXT("stopped") || Job.State == TEXT("failed"); }, 60.0);
	const FHyperAIModelStep* Retry = Job.Steps.FindByPredicate([](const FHyperAIModelStep& Step) { return Step.What.StartsWith(TEXT("Try again")); });
	TestTrue(TEXT("50010001: refunded, and a retry is offered at the same price"), Retry && Retry->Status == TEXT("awaiting_user_approval") && Retry->Credits == 15
		&& Retry->Detail.Contains(TEXT("refunded")) && FHyperAIStudioApprovalGate::HasPending(Job.CurrentOperationId));
	const TArray<FString>& Forms = Seen->Bodies.FindRef(TEXT("/hi3d/open-api/v1/submit-task"));
	const FString TextureForm = Forms.Num() > 1 ? Forms[1] : FString();
	TestTrue(TEXT("The texture-only form: request_type 2, v3.0, the current mesh as a GLB, no face count"), FormField(TextureForm, TEXT("request_type")) == TEXT("2")
		&& FormField(TextureForm, TEXT("model")) == TEXT("hi3dv3.0") && TextureForm.Contains(TEXT("name=\"mesh\"; filename=")) && TextureForm.Contains(TEXT(".glb\""))
		&& FormField(TextureForm, TEXT("face")).IsEmpty() && TextureForm.Contains(TEXT("name=\"images\"")));
	FHyperAIStudioApprovalGate::Approve(Job.CurrentOperationId, Error);
	TickUntil([&]() { Job = JobOf(Asked.OperationId); return Job.State == TEXT("completed") || Job.State == TEXT("stopped") || Job.State == TEXT("failed") || (Job.State == TEXT("awaiting_fix_approval")
		&& !Job.Steps.Last().What.StartsWith(TEXT("Try again"))); }, 60.0);
	AddInfo(FString::Printf(TEXT("After the retexture: %s, %s"), *Job.State, *Job.Diagnostic));
	for (const FHyperAIModelStep& Step : Job.Steps)
	{
		AddInfo(FString::Printf(TEXT("  round %d %s %s: %s (%s)"), Step.Round, *Step.Kind, *Step.Status, *Step.What.Left(160), *Step.Detail.Left(300)));
	}
	TestTrue(TEXT("The retried texture came in: the model now has its textures"), ResultOf(Job.LastCheck, TEXT("textures")) != TEXT("fail") && Seen->Submits == 3);
	if (FHyperAIStudioApprovalGate::HasPending(Job.CurrentOperationId))
	{
		FHyperAIStudioApprovalGate::Reject(Job.CurrentOperationId, TEXT("Enough."));
	}
	for (const FHyperAIModelStep& Step : Job.Steps)
	{
		if (Step.Kind == TEXT("paid_fix"))
		{
			Cleanup.Add(FPaths::GetBaseFilename(Step.Detail));
		}
	}
	if (const UStaticMesh* Fixed = LoadObject<UStaticMesh>(nullptr, *Job.AssetPath))
	{
		Cleanup.Add(FPackageName::GetLongPackagePath(Fixed->GetOutermost()->GetName()) / TEXT("Fixes"));
	}

	// The balance, with the token, beside Tripo's.
	bool bRead = false;
	FHyperAIModelAccountReport Account;
	RefreshAccount([&bRead, &Account](const FHyperAIModelAccountReport& Report) { Account = Report; bRead = true; });
	TickUntil([&bRead]() { return bRead; }, 10.0);
	TestTrue(TEXT("Hi3D's balance is read (data.totalBalance) beside Tripo's"), Account.Hi3DStatus == TEXT("ok") && FMath::IsNearlyEqual(Account.Hi3DBalance, 512.f));

	// Nowhere: the key, its base64, the tokens.
	FString Output;
	FJsonObjectConverter::UStructToJsonObjectString(HyperAIStudio::MeshGen::Creator::Status(Asked.OperationId), Output);
	FString AccountJson;
	FJsonObjectConverter::UStructToJsonObjectString(UHyperAIStudioMeshGenToolset::hyper_tripo_account(FHyperAIModelAccountRequest()), AccountJson);
	FString Journal;
	FFileHelper::LoadFileToString(Journal, *(FPaths::ProjectDir() / Job.Journal));
	for (const FString& Secretive : {Secret, FBase64::Encode(Secret), FString(TEXT("hi3d-client-secret")), FString(TEXT("hi3d-token-1")), FString(TEXT("hi3d-token-2"))})
	{
		TestFalse(TEXT("Never in the log: ") + Secretive.Left(10), Logged.Text.Contains(Secretive));
		TestFalse(TEXT("Never in hyper_model_status or the account: ") + Secretive.Left(10), Output.Contains(Secretive) || AccountJson.Contains(Secretive));
		TestFalse(TEXT("Never in the journal: ") + Secretive.Left(10), Journal.Contains(Secretive));
	}
	TestTrue(TEXT("The token went to Hi3D alone: every Bearer call was the stand-in's Hi3D"), !Seen->Auth.ContainsByPredicate([](const FString& Line)
	{
		return Line.Contains(TEXT("hi3d-token")) && !Line.StartsWith(TEXT("/hi3d/open-api/"));
	}));

	// Hi3D from words: Tripo's text-to-image makes the picture first (one approval for both), then Hi3D makes the model from it.
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_TRIPO_KEY"), TEXT("tripo-creator-test-key"));
	FHyperAIModelCreateRequest Lantern;
	Lantern.Brief.Prompt = TEXT("an ornate brass lantern");
	Lantern.Brief.Provider = TEXT("hi3d");
	Lantern.Brief.Use = TEXT("prop");
	Lantern.Brief.ReviewReference = TEXT("off");
	Lantern.Brief.TextureQuality = TEXT("none");
	Lantern.bLocalFixesOnly = true;
	Lantern.bDryRun = false;
	Lantern.Name = TEXT("SM_Hi3DLantern");
	Lantern.OperationId = TEXT("model-hi3d-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	Operations.Add(Lantern.OperationId);
	const FHyperAIModelCreateReport LanternAsked = Create(Lantern);
	TestTrue(TEXT("Hi3D from words plans Tripo's picture first: ") + LanternAsked.Diagnostic, LanternAsked.Status == TEXT("awaiting_user_approval")
		&& LanternAsked.Plan.bReferenceFirst && LanternAsked.Plan.ReferenceSource == TEXT("tripo"));
	FHyperAIStudioApprovalGate::Approve(LanternAsked.OperationId, Error);
	const int32 SubmitsBefore = Seen->Submits;
	TickUntil([&]() { Job = JobOf(Lantern.OperationId); return Job.State == TEXT("completed") || Job.State == TEXT("stopped") || Job.State == TEXT("failed"); }, 60.0);
	AddInfo(FString::Printf(TEXT("Lantern: %s, %s"), *Job.State, *Job.Diagnostic));
	Cleanup.Add(Job.AssetPath);
	const TArray<FString>& LanternForms = Seen->Bodies.FindRef(TEXT("/hi3d/open-api/v1/submit-task"));
	TestTrue(TEXT("one approval: the picture, then Hi3D from it (geometry only), the picture as images"), Seen->Calls.Contains(TEXT("/tripo/v3/generation/text-to-image"))
		&& Seen->Submits == SubmitsBefore + 1 && !LanternForms.IsEmpty() && LanternForms.Last().Contains(TEXT("filename=\"reference.png\""))
		&& FormField(LanternForms.Last(), TEXT("request_type")) == TEXT("1") && !Job.AssetPath.IsEmpty());
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_TRIPO_KEY"), TEXT(""));

	// Auto: a picture of a hero prop goes to Hi3D only with its key; without it, Tripo.
	FHyperAIModelCreateRequest Auto;
	Auto.Brief.Prompt = TEXT("a carved stone altar");
	Auto.Brief.Use = TEXT("hero_prop");
	Auto.Brief.ImagePath = Request.Brief.ImagePath;
	TestEqual(TEXT("With Hi3D's key, auto chooses Hi3D"), Create(Auto).Plan.Provider, FString(TEXT("hi3d")));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_HI3D_KEY"), TEXT(""));
	const FHyperAIModelCreateReport Without = Create(Auto);
	TestTrue(TEXT("Without it, Tripo, saying why"), Without.Plan.Provider == TEXT("tripo") && Without.Plan.Reasons[0].Contains(TEXT("key")));
	Auto.Brief.Provider = TEXT("hi3d");
	Auto.bDryRun = false;
	const FHyperAIModelCreateReport NoKey = Create(Auto);
	TestTrue(TEXT("Asked for, Hi3D without its key is refused, saying where the AK:SK goes"), NoKey.Status == TEXT("missing_key") && NoKey.Diagnostic.Contains(TEXT("client_id:client_secret")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelCreatorReferenceSwordTest, "HyperAIStudio.MeshGen.Creator.ReferenceSword",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioModelCreatorReferenceSwordTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelCreatorTests;
	constexpr uint32 Port = 18356;
	const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
	if (!Router.IsValid())
	{
		AddWarning(TEXT("Port 18356 is taken, so the stand-in for Tripo could not start."));
		return true;
	}
	const FString Base = FString::Printf(TEXT("http://127.0.0.1:%u"), Port);
	const TSharedRef<FHi3DSeen> Seen = MakeShared<FHi3DSeen>();
	FFixture Blade;
	Blade.Grid = 4;
	Blade.Size = 1.f;
	Blade.bTextured = true;
	const TSharedRef<TArray<uint8>> Sword = MakeShared<TArray<uint8>>(CubeGlb(Blade));
	const TSharedRef<TArray<uint8>> Picture = MakeShared<TArray<uint8>>(Png(64, [](int32 X, int32 Y) { return FMath::Abs(X - 32) < 4 ? FColor(200, 200, 210) : FColor(240, 240, 240); }));
	TArray<FHttpRouteHandle> Routes;
	auto Bind = [&](const TCHAR* Path, const EHttpServerRequestVerbs Verb, TFunction<TUniquePtr<FHttpServerResponse>(const FHttpServerRequest&)> Answer)
	{
		Routes.Add(Router->BindRoute(FHttpPath(Path), Verb, FHttpRequestHandler::CreateLambda([Seen, Path = FString(Path), Answer](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			Seen->Calls.Add(Path);
			Seen->Bodies.FindOrAdd(Path).Add(BodyText(Request).Left(6000));
			OnComplete(Answer(Request));
			return true;
		})));
	};
	Bind(TEXT("/tripo/v3/generation/text-to-image"), EHttpServerRequestVerbs::VERB_POST, [Seen](const FHttpServerRequest&)
	{
		// Busy the first time (rate limited): sent again after the back-off.
		if (++Seen->Submits == 1)
		{
			TUniquePtr<FHttpServerResponse> Busy = JsonReply(TEXT(R"({"code":1007,"message":"rate limited"})"), EHttpServerResponseCodes::TooManyRequests);
			Busy->Headers.Add(TEXT("Retry-After"), {TEXT("1")});
			return Busy;
		}
		return JsonReply(TEXT(R"({"code":0,"data":{"task_id":"task_picture"}})"));
	});
	Bind(TEXT("/tripo/v3/generation/image-to-model"), EHttpServerRequestVerbs::VERB_POST, [](const FHttpServerRequest&) { return JsonReply(TEXT(R"({"code":0,"data":{"task_id":"task_sword"}})")); });
	Bind(TEXT("/tripo/v3/tasks/task_picture"), EHttpServerRequestVerbs::VERB_GET, [Seen, Base](const FHttpServerRequest&)
	{
		// "unknown" once: polled on.
		const int32 Poll = Seen->Polls.FindOrAdd(TEXT("picture"))++;
		if (Poll == 0)
		{
			return JsonReply(TEXT(R"({"code":0,"data":{"status":"unknown"}})"));
		}
		return JsonReply(FString::Printf(TEXT(R"({"code":0,"data":{"status":"success","progress":100,"credits_consumed":20,"output":{"generated_image_url":"%s/files/sword.png"}}})"), *Base));
	});
	Bind(TEXT("/tripo/v3/tasks/task_sword"), EHttpServerRequestVerbs::VERB_GET, [Seen, Base](const FHttpServerRequest&)
	{
		if (Seen->Polls.FindOrAdd(TEXT("sword"))++ == 0)
		{
			return JsonReply(TEXT(R"({"code":0,"data":{"status":"running","progress":40}})"));
		}
		return JsonReply(FString::Printf(TEXT(R"({"code":0,"data":{"status":"success","progress":100,"credits_consumed":60,"output":{"model_url":"%s/files/sword.glb"}}})"), *Base));
	});
	Bind(TEXT("/files/sword.png"), EHttpServerRequestVerbs::VERB_GET, [Picture](const FHttpServerRequest&) { return FHttpServerResponse::Create(TArray<uint8>(*Picture), TEXT("image/png")); });
	Bind(TEXT("/files/sword.glb"), EHttpServerRequestVerbs::VERB_GET, [Sword](const FHttpServerRequest&) { return FHttpServerResponse::Create(TArray<uint8>(*Sword), TEXT("model/gltf-binary")); });
	FHttpServerModule::Get().StartAllListeners();

	FStandIn StandIn(Base);
	TArray<FString> Operations;
	TArray<FString> Cleanup;
	ON_SCOPE_EXIT
	{
		for (const FHttpRouteHandle& Route : Routes)
		{
			Router->UnbindRoute(Route);
		}
		for (const FString& Operation : Operations)
		{
			IFileManager::Get().DeleteDirectory(*(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Models") / Operation), false, true);
		}
		DeleteAll(Cleanup);
	};

	// The user's first run: a sword, a weapon, its picture from chat_image_2.5_sunburst at high quality on a transparent
	// background (asked for as people say it), then H3.1 detailed from that picture.
	FHyperAIModelCreateRequest Request;
	Request.Brief.Prompt = TEXT("a sword");
	Request.Brief.Use = TEXT("weapon");
	Request.Brief.ImageModel = TEXT("gpt-image-2.5-sunburn");
	Request.Brief.ImageQuality = TEXT("high");
	Request.Brief.ImageBackground = TEXT("transparent");
	Request.Brief.Model = TEXT("H3.1");
	Request.Brief.GeometryQuality = TEXT("detailed");
	Request.Brief.TextureQuality = TEXT("detailed");
	Request.FixBudgetUSD = 1.f;
	Request.MaxRounds = 2;
	const FHyperAIModelCreateReport Dry = Create(Request);
	const FHyperAIModelPlan& Plan = Dry.Plan;
	TestTrue(TEXT("The plan: a picture first, chat_image_2.5_sunburst (read from the name), high, 1024, transparent, asset extraction, reviewed: ") + Dry.Diagnostic,
		Dry.bOk && Plan.bReferenceFirst && Plan.ReferenceSource == TEXT("tripo") && Plan.ImageModel == TEXT("chat_image_2.5_sunburst") && Plan.ImageQuality == TEXT("high")
		&& Plan.ImageSize == TEXT("1024x1024") && Plan.ImageBackground == TEXT("transparent") && Plan.ImageTemplate == TEXT("asset_extraction") && Plan.bReviewReference
		&& Plan.Reasons.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("gpt-image-2.5-sunburn")); }));
	TestTrue(TEXT("then H3.1 detailed from the picture's task: image-to-model"), Plan.Family == TEXT("H3.1") && Plan.GeometryQuality == TEXT("detailed") && Plan.Endpoint == TEXT("/generation/image-to-model")
		&& Plan.Input == TEXT("image") && Plan.Provider == TEXT("tripo"));
	TestTrue(TEXT("Priced: the picture 20, H3.1 from a picture detailed 40, detailed geometry 20: 80 credits"), Plan.Credits == 80
		&& Plan.PriceLines.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("reference picture (chat_image_2.5_sunburst high, up to 1K): 20 credits")); }));
	TestTrue(TEXT("Nothing was sent by the dry run"), Seen->Calls.IsEmpty());

	Request.bDryRun = false;
	Request.OperationId = TEXT("model-sword-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	Operations.Add(Request.OperationId);
	const FHyperAIModelCreateReport Asked = Create(Request);
	if (!TestEqual(TEXT("The picture is asked for first: ") + Asked.Diagnostic, Asked.Status, FString(TEXT("awaiting_user_approval"))))
	{
		return false;
	}
	FHyperAIModelJob Job = JobOf(Asked.OperationId);
	TestTrue(TEXT("its step is the picture alone, 20 credits"), !Job.Steps.IsEmpty() && Job.Steps.Last().Kind == TEXT("reference") && Job.Steps.Last().Credits == 20);
	FString Error;
	TestTrue(TEXT("Approved"), FHyperAIStudioApprovalGate::Approve(Asked.OperationId, Error));
	TickUntil([&]() { Job = JobOf(Asked.OperationId); return Job.State == TEXT("awaiting_reference_review") || Job.State == TEXT("failed") || Job.State == TEXT("stopped"); }, 30.0);
	AddInfo(FString::Printf(TEXT("After the picture: %s, %s"), *Job.State, *Job.Diagnostic));
	const TSharedPtr<FJsonObject> Sent = Parse(Seen->Bodies.FindRef(TEXT("/tripo/v3/generation/text-to-image")).IsEmpty() ? FString() : Seen->Bodies.FindRef(TEXT("/tripo/v3/generation/text-to-image")).Last());
	TestTrue(TEXT("Text-to-image: sunburst, high, 1024x1024, transparent, PNG, asset_extraction, negatives after --no"), Sent.IsValid() && Sent->GetStringField(TEXT("model")) == TEXT("chat_image_2.5_sunburst")
		&& Sent->GetStringField(TEXT("quality")) == TEXT("high") && Sent->GetStringField(TEXT("size")) == TEXT("1024x1024") && Sent->GetStringField(TEXT("background")) == TEXT("transparent")
		&& Sent->GetStringField(TEXT("output_format")) == TEXT("png") && Sent->GetStringField(TEXT("template")) == TEXT("asset_extraction")
		&& Sent->GetStringField(TEXT("prompt")).StartsWith(TEXT("a sword, single isolated object")) && Sent->GetStringField(TEXT("prompt")).Contains(TEXT(" --no ")));
	TestEqual(TEXT("The 429 was sent again after Retry-After"), Seen->Calls.FilterByPredicate([](const FString& Call) { return Call == TEXT("/tripo/v3/generation/text-to-image"); }).Num(), 2);
	TestTrue(TEXT("Polled through an unknown status"), Seen->Polls.FindRef(TEXT("picture")) >= 2);
	TestEqual(TEXT("The job holds for the review"), Job.State, FString(TEXT("awaiting_reference_review")));
	TestTrue(TEXT("with the picture downloaded into its folder and shown"), Job.PreviewImages.ContainsByPredicate([](const FString& Image) { return Image.EndsWith(TEXT("reference.png")); })
		&& IFileManager::Get().FileSize(*(FPaths::ProjectDir() / Job.PreviewImages[0])) > 0);
	TestTrue(TEXT("and the 3D generation waits for its own approval, 60 credits"), FHyperAIStudioApprovalGate::HasPending(Job.CurrentOperationId) && Job.Steps.Last().Kind == TEXT("generate")
		&& Job.Steps.Last().Credits == 60 && !Seen->Calls.Contains(TEXT("/tripo/v3/generation/image-to-model")));
	TestTrue(TEXT("Approved"), FHyperAIStudioApprovalGate::Approve(Job.CurrentOperationId, Error));
	TickUntil([&]() { Job = JobOf(Asked.OperationId); return Job.State == TEXT("awaiting_fix_approval") || Job.State == TEXT("completed") || Job.State == TEXT("stopped") || Job.State == TEXT("failed"); }, 60.0);
	AddInfo(FString::Printf(TEXT("After the sword: %s, %s"), *Job.State, *Job.Diagnostic));
	for (const FHyperAIModelStep& Step : Job.Steps)
	{
		AddInfo(FString::Printf(TEXT("  round %d %s %s: %s (%s)"), Step.Round, *Step.Kind, *Step.Status, *Step.What.Left(160), *Step.Detail.Left(300)));
	}
	Cleanup.Add(Job.AssetPath);
	const TSharedPtr<FJsonObject> Model = Parse(Seen->Bodies.FindRef(TEXT("/tripo/v3/generation/image-to-model")).IsEmpty() ? FString() : Seen->Bodies.FindRef(TEXT("/tripo/v3/generation/image-to-model"))[0]);
	TestTrue(TEXT("Image-to-model took the picture's task as input, H3.1, detailed geometry and textures, the picture's colours"), Model.IsValid() && Model->GetStringField(TEXT("input")) == TEXT("task_picture")
		&& Model->GetStringField(TEXT("model")) == TEXT("v3.1-20260211") && Model->GetStringField(TEXT("geometry_quality")) == TEXT("detailed")
		&& Model->GetStringField(TEXT("texture_quality")) == TEXT("detailed") && Model->GetStringField(TEXT("texture_alignment")) == TEXT("original_image"));
	TestTrue(TEXT("Nothing was uploaded: no picture went to /files"), !Seen->Calls.Contains(TEXT("/tripo/v3/files")));
	TestTrue(TEXT("The sword is imported and checked, the picture its visual reference"), !Job.AssetPath.IsEmpty() && Job.FirstCheck.Checks.Num() > 10
		&& Job.Steps.ContainsByPredicate([](const FHyperAIModelStep& Step) { return Step.Kind == TEXT("check"); }));
	TestTrue(TEXT("Spent: the picture and the model, with Tripo's own count (20 + 60)"), FMath::IsNearlyEqual(Job.SpentUSD, 0.80f, 0.001f) && FMath::IsNearlyEqual(Job.CreditsConsumed, 80.f, 0.01f));
	if (FHyperAIStudioApprovalGate::HasPending(Job.CurrentOperationId))
	{
		FHyperAIStudioApprovalGate::Reject(Job.CurrentOperationId, TEXT("Enough."));
	}

	// Seen and turned down: the review declines the 3D, nothing more is paid.
	Seen->Polls.Reset();
	FHyperAIModelCreateRequest Again = Request;
	Again.OperationId = TEXT("model-sword-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	Again.Name = TEXT("SM_SwordDeclined");
	Operations.Add(Again.OperationId);
	const FHyperAIModelCreateReport Second = Create(Again);
	FHyperAIStudioApprovalGate::Approve(Second.OperationId, Error);
	TickUntil([&]() { Job = JobOf(Second.OperationId); return Job.State == TEXT("awaiting_reference_review") || Job.State == TEXT("failed"); }, 30.0);
	const int32 Models = Seen->Calls.FilterByPredicate([](const FString& Call) { return Call == TEXT("/tripo/v3/generation/image-to-model"); }).Num();
	FHyperAIStudioApprovalGate::Reject(Job.CurrentOperationId, TEXT("Not that sword."));
	TickUntil([&]() { Job = JobOf(Second.OperationId); return Job.State == TEXT("stopped"); }, 5.0);
	TestTrue(TEXT("Declining at the review stops the job, fix_declined, and no 3D is asked of Tripo"), Job.StopReason == TEXT("fix_declined")
		&& Seen->Calls.FilterByPredicate([](const FString& Call) { return Call == TEXT("/tripo/v3/generation/image-to-model"); }).Num() == Models);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
