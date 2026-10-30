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
#include "HyperAIStudioExternalService.h"
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
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonSerializer.h"

/**
 * The Tripo model creator: classification, Tripo v3 request bodies, prices, the checks on small GLB fixtures with known
 * defects, each free fix, the paid-fix ladder, the material build from PBR maps, a whole job against a stand-in Tripo
 * (never the real one) with its budget, round cap, refusals and cancel, the Tripo Studio watch, and the account read.
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
			}
			Settings->MeshImportFolder = TestFolder;
			Settings->ExternalServiceSessionSpendCap = 100.f;
			FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_TRIPO_KEY"), TEXT("tripo-creator-test-key"));
			FHyperAIStudioMeshGenContracts::SetPollSecondsForTests(0.1);
			FHyperAIStudioExternalServices::ResetSessionSpend();
		}

		~FStandIn()
		{
			UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
			Settings->ExternalServices = SavedServices;
			Settings->MeshImportFolder = SavedFolder;
			Settings->ExternalServiceSessionSpendCap = SavedCap;
			Settings->MeshTaskPrices = SavedPrices;
			FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_CREATOR_TRIPO_KEY"), TEXT(""));
			FHyperAIStudioMeshGenContracts::SetPollSecondsForTests(0.0);
			FHyperAIStudioExternalServices::ResetSessionSpend();
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
		{TEXT("an ornate golden throne"), TEXT("hero_prop"), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("hero_prop"), false, 500000, TEXT("extreme"), false, false, true},
		{TEXT("a wooden crate"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("P2"), TEXT("prop"), false, 20000, TEXT("standard"), false, false, false},
		{TEXT("a sci-fi soldier in armour"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("P2"), TEXT("character"), true, 20000, TEXT("detailed"), false, false, false},
		{TEXT("a sci-fi soldier in armour"), TEXT(""), TEXT("mobile"), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("P2"), TEXT("character"), true, 5000, TEXT("standard"), false, false, false},
		{TEXT("a mossy boulder"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("environment"), false, 200000, TEXT("detailed"), false, false, true},
		{TEXT("a stone wall section"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("P2"), TEXT("kit"), false, 30000, TEXT("standard"), false, false, false},
		{TEXT("a rusty pickup truck"), TEXT(""), TEXT("vr"), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("vehicle"), false, 75000, TEXT("detailed"), false, false, true},
		{TEXT("a low poly pine tree"), TEXT(""), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("P2"), TEXT("foliage"), false, 8000, TEXT("standard"), false, false, true},
		{TEXT("a low poly treasure chest"), TEXT("prop"), TEXT(""), TEXT("H3.1"), 0, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("prop"), false, 20000, TEXT("standard"), false, true, false},
		{TEXT("a robot with separate parts"), TEXT("prop"), TEXT(""), TEXT(""), 0, TEXT(""), TEXT(""), TEXT("H3.1"), TEXT("prop"), false, 20000, TEXT("standard"), true, false, false},
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
	Body = RigBody(TEXT("task_1"), TEXT("unreal"));
	TestTrue(TEXT("rig: v3.0 rig model, biped, spec unreal"), Body->GetStringField(TEXT("model")) == TEXT("v3.0-20260909") && Body->GetStringField(TEXT("spec")) == TEXT("unreal")
		&& Body->GetStringField(TEXT("rig_type")) == TEXT("biped"));

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
			OnComplete(FHttpServerResponse::Create(TEXT(R"({"code":0,"data":{"balance":1234.5,"frozen":40}})"), TEXT("application/json")));
			return true;
		})));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/tripo/v3/account/usage")), EHttpServerRequestVerbs::VERB_GET, FHttpRequestHandler::CreateLambda(
		[](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			OnComplete(FHttpServerResponse::Create(TEXT(R"({"code":0,"data":[{"task_id":"task_a","type":"text_to_model","credits_consumed":20,"created_at":"2026-10-30T10:00:00Z"},)")
				TEXT(R"({"task_id":"task_b","type":"mesh_decimate","credits_consumed":35,"created_at":"2026-10-30T10:05:00Z"}]})"), TEXT("application/json")));
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
			if (Service.Id == TEXT("tripo"))
			{
				Service.BaseUrl = Base + TEXT("/tripo/v3");
				Service.ApiKeyVariable = TEXT("TRIPO_API_KEY_HYPERAI_NONE");
				Studio->ExternalServices.Add(Service);
			}
		}
		RefreshAccount([&bDone, &Missing](const FHyperAIModelAccountReport& Report) { Missing = Report; bDone = true; });
		Studio->ExternalServices = Saved;
	}
	TestTrue(TEXT("Without a key it says where to add one"), bDone && Missing.Status == TEXT("missing_key") && Missing.Diagnostic.Contains(TEXT("Keys")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
