// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioContextSearchToolsets.h"
#include "HyperAIStudioMentions.h"
#include "HyperAIStudioProjectAssetIndex.h"
#include "HyperAIStudioSemanticSearch.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioWidgetShot.h"
#include "SHyperAIStudioMentionPicker.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace HyperAIStudio::SemanticSearch::Tests
{
	/** A fixed folder tree standing in for the Asset Registry, with tags; it records the folders the index reads. */
	class FFixtureSource final : public IHyperAIProjectAssetSource
	{
	public:
		virtual bool IsAvailable() const override { return true; }
		virtual bool IsScanning() const override { return false; }
		virtual TArray<FName> GetContentRoots() const override { return { FName(TEXT("/Game")) }; }
		virtual void GetChildFolders(const FName Folder, TArray<FName>& OutChildren) const override
		{
			if (const TArray<FName>* Found = Children.Find(Folder))
			{
				OutChildren = *Found;
			}
		}
		virtual void GetFolderAssets(const FName Folder, TArray<FAssetData>& OutAssets) const override
		{
			AssetReads.Add(Folder);
			if (const TArray<FAssetData>* Found = Assets.Find(Folder))
			{
				OutAssets = *Found;
			}
		}

		void AddFolder(const TCHAR* Parent, const TCHAR* Folder)
		{
			Children.FindOrAdd(FName(Parent)).AddUnique(FName(Folder));
		}

		void AddAsset(const TCHAR* Folder, const TCHAR* Name, const TCHAR* ClassPath, const TCHAR* Description = nullptr)
		{
			FAssetDataTagMap Tags;
			if (Description)
			{
				Tags.Add(FName(TEXT("Description")), Description);
			}
			const FString PackageName = FString(Folder) + TEXT("/") + Name;
			Assets.FindOrAdd(FName(Folder)).Add(FAssetData(FName(*PackageName), FName(Folder), FName(Name), FTopLevelAssetPath(ClassPath), MoveTemp(Tags)));
		}

		TMap<FName, TArray<FName>> Children;
		TMap<FName, TArray<FAssetData>> Assets;
		mutable TArray<FName> AssetReads;
	};

	constexpr const TCHAR* MaterialInstance = TEXT("/Script/Engine.MaterialInstanceConstant");
	constexpr const TCHAR* Material = TEXT("/Script/Engine.Material");
	constexpr const TCHAR* StaticMesh = TEXT("/Script/Engine.StaticMesh");
	constexpr const TCHAR* Texture = TEXT("/Script/Engine.Texture2D");

	/**
	 * A small project with descriptive names and one tag: walls, props, a texture and a character.
	 * /Game/Env/Walls: MI_StoneWall_Moss, M_BrickWall_Clean, SM_Wall_Stone, MI_Cobble_Overgrown
	 * /Game/Env/Props: SM_Barrel_Wood, SM_Crate_Metal_Rusty, SM_Boulder_Lichen, SM_Crystal_A (tagged "glowing blue crystal")
	 * /Game/Env/Textures: T_Moss_D
	 */
	TUniquePtr<FFixtureSource> MakeFixture()
	{
		TUniquePtr<FFixtureSource> Source = MakeUnique<FFixtureSource>();
		Source->AddFolder(TEXT("/Game"), TEXT("/Game/Env"));
		Source->AddFolder(TEXT("/Game/Env"), TEXT("/Game/Env/Walls"));
		Source->AddFolder(TEXT("/Game/Env"), TEXT("/Game/Env/Props"));
		Source->AddFolder(TEXT("/Game/Env"), TEXT("/Game/Env/Textures"));
		Source->AddAsset(TEXT("/Game/Env/Walls"), TEXT("MI_StoneWall_Moss"), MaterialInstance);
		Source->AddAsset(TEXT("/Game/Env/Walls"), TEXT("M_BrickWall_Clean"), Material);
		Source->AddAsset(TEXT("/Game/Env/Walls"), TEXT("SM_Wall_Stone"), StaticMesh);
		Source->AddAsset(TEXT("/Game/Env/Walls"), TEXT("MI_Cobble_Overgrown"), MaterialInstance);
		Source->AddAsset(TEXT("/Game/Env/Props"), TEXT("SM_Barrel_Wood"), StaticMesh);
		Source->AddAsset(TEXT("/Game/Env/Props"), TEXT("SM_Crate_Metal_Rusty"), StaticMesh);
		Source->AddAsset(TEXT("/Game/Env/Props"), TEXT("SM_Boulder_Lichen"), StaticMesh);
		Source->AddAsset(TEXT("/Game/Env/Props"), TEXT("SM_Crystal_A"), StaticMesh, TEXT("glowing blue crystal"));
		Source->AddAsset(TEXT("/Game/Env/Textures"), TEXT("T_Moss_D"), Texture);
		return Source;
	}

	FHyperAIStudioProjectAssetIndex::FLimits Limits()
	{
		FHyperAIStudioProjectAssetIndex::FLimits Out;
		Out.bTickOnCoreTicker = false;
		Out.PublishIntervalSeconds = 0.0;
		return Out;
	}

	using FIndexRef = TSharedRef<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe>;

	FIndexRef Walk(TUniquePtr<FFixtureSource> Source, FFixtureSource*& OutSource)
	{
		OutSource = Source.Get();
		FIndexRef Index = MakeShared<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe>(MoveTemp(Source), Limits());
		Index->EnsureStarted();
		for (int32 Step = 0; Step < 32 && !Index->GetPublished()->bHasCompletedWalk; ++Step)
		{
			Index->Step(1.0, 64, /*bForcePublish=*/true);
		}
		return Index;
	}

	TArray<FString> Names(const TArray<FRanked>& Ranked)
	{
		TArray<FString> Out;
		for (const FRanked& Each : Ranked)
		{
			Out.Add(Each.Row->ObjectPath.GetAssetName());
		}
		return Out;
	}

	FString FindField(const FHyperAIReadRecord& Record, const TCHAR* Name)
	{
		for (const FHyperAIReadField& Field : Record.Fields)
		{
			if (Field.Name == Name)
			{
				return Field.Value;
			}
		}
		return FString();
	}

	/** Restores Search By Meaning however a test ends. */
	struct FMeaningSetting
	{
		bool bWas = GetDefault<UHyperAIStudioSettings>()->bSearchByMeaning;
		void Set(const bool bOn) { GetMutableDefault<UHyperAIStudioSettings>()->bSearchByMeaning = bOn; }
		~FMeaningSetting() { Set(bWas); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSemanticSearchWordsTest,
	"HyperAIStudio.SemanticSearch.Words",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSemanticSearchWordsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::SemanticSearch;
	TestEqual(TEXT("Names split at prefixes, case and digits"), FString::Join(SplitWords(TEXT("MI_StoneWall01_Moss")), TEXT(" ")), FString(TEXT("mi stone wall 01 moss")));
	TestEqual(TEXT("An acronym before a word"), FString::Join(SplitWords(TEXT("HUDWidget")), TEXT(" ")), FString(TEXT("hud widget")));
	TestEqual(TEXT("mossy -> moss"), Stem(TEXT("mossy")), FString(TEXT("moss")));
	TestEqual(TEXT("walls -> wall"), Stem(TEXT("walls")), FString(TEXT("wall")));
	TestEqual(TEXT("wooden -> wood"), Stem(TEXT("wooden")), FString(TEXT("wood")));
	TestEqual(TEXT("rusted -> rust"), Stem(TEXT("rusted")), FString(TEXT("rust")));
	TestEqual(TEXT("boxes -> box"), Stem(TEXT("boxes")), FString(TEXT("box")));
	TestEqual(TEXT("glass keeps its s"), Stem(TEXT("glass")), FString(TEXT("glass")));
	TestTrue(TEXT("Stop words are dropped from queries"), IsStopWord(TEXT("the")) && IsStopWord(TEXT("with")) && !IsStopWord(TEXT("stone")));

	TArray<int32> Moss;
	TArray<int32> Lichen;
	ConceptsOf(Stem(TEXT("mossy")), false, Moss);
	ConceptsOf(Stem(TEXT("lichen")), false, Lichen);
	TestTrue(TEXT("moss and lichen share a concept"), Moss.ContainsByPredicate([&Lichen](const int32 Id) { return Lichen.Contains(Id); }));
	TArray<int32> Prefix;
	TArray<int32> MaterialWord;
	ConceptsOf(TEXT("mi"), true, Prefix);
	ConceptsOf(TEXT("material"), false, MaterialWord);
	TestTrue(TEXT("The MI_ prefix means material"), Prefix.ContainsByPredicate([&MaterialWord](const int32 Id) { return MaterialWord.Contains(Id); }));
	TestTrue(TEXT("The vocabulary has its concepts"), ConceptCount() >= 80);

	TestTrue(TEXT("A short description is a useful tag"), IsUsefulTagValue(TEXT("glowing blue crystal")));
	TestFalse(TEXT("A pixel format is not"), IsUsefulTagValue(TEXT("PF_DXT1")));
	TestFalse(TEXT("Nor a number"), IsUsefulTagValue(TEXT("2048")));
	TestFalse(TEXT("Nor a path"), IsUsefulTagValue(TEXT("/Game/Env/T_Moss_D.T_Moss_D")));
	TestFalse(TEXT("Nor a GUID"), IsUsefulTagValue(TEXT("0123456789ABCDEF0123456789ABCDEF")));
	TestFalse(TEXT("Nor a bool"), IsUsefulTagValue(TEXT("True")));

	const FQuery Query = AnalyzeQuery(TEXT("the stone wall material with moss"));
	TestEqual(TEXT("Four meaningful words"), Query.Words.Num(), 4);
	TestTrue(TEXT("each with concepts"), Query.Words[0].Concepts.Num() > 0);
	const FQuery Plain = AnalyzeQuery(TEXT("the stone wall material with moss"), /*bConcepts=*/false);
	TestTrue(TEXT("The words-only fallback carries no concepts or trigrams"), Plain.Words.Num() == 4 && Plain.Words[0].Concepts.IsEmpty() && Plain.Words[0].Grams.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSemanticSearchRankingTest,
	"HyperAIStudio.SemanticSearch.Ranking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSemanticSearchRankingTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::SemanticSearch;
	using namespace HyperAIStudio::SemanticSearch::Tests;
	FFixtureSource* Source = nullptr;
	const FIndexRef Index = Walk(MakeFixture(), Source);
	const TSharedRef<const FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe> Snapshot = Index->GetPublished();
	TestEqual(TEXT("The fixture is indexed"), Snapshot->AssetCount, 9);
	TestTrue(TEXT("with postings and a size"), Snapshot->PostingCount > 0 && Snapshot->ApproxBytes > 0);

	FRankLimits RankLimits;
	bool bTruncated = false;
	const TArray<FRanked> Wall = Rank(*Snapshot, AnalyzeQuery(TEXT("the stone wall material with moss")), RankLimits, bTruncated);
	const TArray<FString> WallNames = Names(Wall);
	AddInfo(TEXT("stone wall material with moss: ") + FString::Join(WallNames, TEXT(", ")));
	TestFalse(TEXT("The fixture ranks within the budget"), bTruncated);
	TestTrue(TEXT("The asset named for it comes first"), WallNames.Num() >= 2 && WallNames[0] == TEXT("MI_StoneWall_Moss"));
	TestTrue(TEXT("by name"), Wall.Num() > 0 && Wall[0].Match == TEXT("name"));
	TestTrue(TEXT("An overgrown cobble material is found by meaning"), WallNames.Contains(TEXT("MI_Cobble_Overgrown")));
	const FRanked* Cobble = Wall.FindByPredicate([](const FRanked& Each) { return Each.Row->ObjectPath.GetAssetName() == TEXT("MI_Cobble_Overgrown"); });
	TestTrue(TEXT("and says so"), Cobble && Cobble->Match == TEXT("meaning"));
	TestFalse(TEXT("A clean brick wall does not match moss and stone"), WallNames.Contains(TEXT("M_BrickWall_Clean")));
	TestFalse(TEXT("Nor does a rusty crate"), WallNames.Contains(TEXT("SM_Crate_Metal_Rusty")));

	const TArray<FString> Mossy = Names(Rank(*Snapshot, AnalyzeQuery(TEXT("mossy rock")), RankLimits, bTruncated));
	AddInfo(TEXT("mossy rock: ") + FString::Join(Mossy, TEXT(", ")));
	TestTrue(TEXT("mossy rock finds the lichen boulder"), Mossy.Contains(TEXT("SM_Boulder_Lichen")));

	const TArray<FRanked> Barrel = Rank(*Snapshot, AnalyzeQuery(TEXT("barel")), RankLimits, bTruncated);
	TestTrue(TEXT("A misspelt word lands by spelling"), Barrel.Num() > 0 && Barrel[0].Row->ObjectPath.GetAssetName() == TEXT("SM_Barrel_Wood")
		&& Barrel[0].Match == TEXT("spelling"));

	const TArray<FString> Crystal = Names(Rank(*Snapshot, AnalyzeQuery(TEXT("glowing crystal")), RankLimits, bTruncated));
	TestTrue(TEXT("A tag's words are searched"), Crystal.Num() > 0 && Crystal[0] == TEXT("SM_Crystal_A"));

	// The fallback: words only, no vocabulary and no trigrams.
	const TArray<FRanked> Plain = Rank(*Snapshot, AnalyzeQuery(TEXT("stone wall moss"), false), RankLimits, bTruncated);
	const TArray<FString> PlainNames = Names(Plain);
	TestTrue(TEXT("Words alone still find the named asset"), PlainNames.Num() > 0 && PlainNames[0] == TEXT("MI_StoneWall_Moss"));
	TestFalse(TEXT("but not the one only its meaning matches"), PlainNames.Contains(TEXT("MI_Cobble_Overgrown")));
	TestTrue(TEXT("Nothing by spelling either"), Rank(*Snapshot, AnalyzeQuery(TEXT("barel"), false), RankLimits, bTruncated).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSemanticSearchProjectSearchTest,
	"HyperAIStudio.SemanticSearch.ProjectSearchModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSemanticSearchProjectSearchTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::SemanticSearch::Tests;
	FMeaningSetting Setting;
	Setting.Set(true);
	FFixtureSource* Source = nullptr;
	const FIndexRef Index = Walk(MakeFixture(), Source);
	const TSharedRef<const FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe> Assets = Index->GetPublished();
	FHyperAIProjectIndexValueSnapshot Cpp;
	Cpp.Status = TEXT("ready");

	FHyperAIProjectSearchRequest Request;
	Request.Kinds = { TEXT("asset") };
	Request.Query = TEXT("the stone wall material with moss");
	Request.Mode = TEXT("literal");
	const FHyperAIReadReport Literal = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, *Assets);
	TestEqual(TEXT("Literally, every word must be in the name: none is"), Literal.TotalRecords, 0);

	Request.Mode = FString();
	const FHyperAIReadReport Auto = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, *Assets);
	TestTrue(TEXT("auto ranks a description by meaning"), Auto.Records.Num() >= 2
		&& Auto.Records[0].RecordId == TEXT("/Game/Env/Walls/MI_StoneWall_Moss.MI_StoneWall_Moss"));
	TestTrue(TEXT("Ranked records say how they matched and score"), Auto.Records.Num() > 0
		&& FindField(Auto.Records[0], TEXT("match")) == TEXT("name") && !FindField(Auto.Records[0], TEXT("score")).IsEmpty());

	Request.Query = TEXT("moss");
	Request.Mode = TEXT("hybrid");
	const FHyperAIReadReport Hybrid = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, *Assets);
	TArray<FString> HybridIds;
	for (const FHyperAIReadRecord& Record : Hybrid.Records)
	{
		HybridIds.Add(FPaths::GetBaseFilename(Record.RecordId));
	}
	AddInfo(TEXT("hybrid moss: ") + FString::Join(HybridIds, TEXT(", ")));
	TestTrue(TEXT("hybrid: the literal matches first"), HybridIds.Num() >= 3 && TSet<FString>({HybridIds[0], HybridIds[1]}).Includes(TSet<FString>({TEXT("MI_StoneWall_Moss"), TEXT("T_Moss_D")})));
	TestTrue(TEXT("then what moss means"), HybridIds.Contains(TEXT("SM_Boulder_Lichen")) && HybridIds.Contains(TEXT("MI_Cobble_Overgrown")));

	Request.Mode = FString();
	const FHyperAIReadReport Short = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, *Assets);
	TestEqual(TEXT("auto keeps a one-word query that names assets literal"), Short.TotalRecords, 2);

	Request.Mode = TEXT("fuzzy");
	TestEqual(TEXT("An unknown mode is refused"), FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, *Assets).Status,
		FString(TEXT("invalid_request")));

	// Search By Meaning off: ranked by words only.
	Setting.Set(false);
	Request.Query = TEXT("the stone wall material with moss");
	Request.Mode = TEXT("semantic");
	const FHyperAIReadReport Fallback = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, *Assets);
	TArray<FString> FallbackIds;
	for (const FHyperAIReadRecord& Record : Fallback.Records)
	{
		FallbackIds.Add(FPaths::GetBaseFilename(Record.RecordId));
	}
	TestTrue(TEXT("The fallback still finds the named asset"), FallbackIds.Num() > 0 && FallbackIds[0] == TEXT("MI_StoneWall_Moss"));
	TestFalse(TEXT("but not by meaning"), FallbackIds.Contains(TEXT("MI_Cobble_Overgrown")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSemanticSearchIncrementalTest,
	"HyperAIStudio.SemanticSearch.IncrementalAndPause",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSemanticSearchIncrementalTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::SemanticSearch;
	using namespace HyperAIStudio::SemanticSearch::Tests;
	TUniquePtr<FFixtureSource> Made = MakeFixture();
	FFixtureSource* Source = Made.Get();
	bool bPause = false;
	FHyperAIStudioProjectAssetIndex::FLimits IndexLimits = Limits();
	IndexLimits.ShouldPause = [&bPause]() { return bPause; };
	const FIndexRef Index = MakeShared<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe>(MoveTemp(Made), IndexLimits);
	Index->EnsureStarted();
	for (int32 Step = 0; Step < 32 && !Index->GetPublished()->bHasCompletedWalk; ++Step)
	{
		Index->TickForTests(0.016f);
	}
	const TSharedRef<const FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe> Before = Index->GetPublished();
	TestTrue(TEXT("The first walk finishes"), Before->bHasCompletedWalk);
	FRankLimits RankLimits;
	bool bTruncated = false;
	TestTrue(TEXT("No lava yet"), Rank(*Before, AnalyzeQuery(TEXT("lava")), RankLimits, bTruncated).IsEmpty());
	const auto FolderOf = [](const FHyperAIProjectAssetIndexSnapshot& Snapshot, const TCHAR* Path)
	{
		const TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>* Found = Snapshot.Folders.FindByPredicate(
			[Path](const TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>& Folder) { return Folder->Path == FName(Path); });
		return Found ? &Found->Get() : nullptr;
	};

	// One asset is saved into Props: only that folder is read again, and its postings with it.
	Source->AddAsset(TEXT("/Game/Env/Props"), TEXT("SM_Magma_Chunk"), StaticMesh);
	FAssetData Changed(FName(TEXT("/Game/Env/Props/SM_Magma_Chunk")), FName(TEXT("/Game/Env/Props")), FName(TEXT("SM_Magma_Chunk")),
		FTopLevelAssetPath(StaticMesh));
	Source->AssetReads.Reset();
	Index->NoteAssetsChanged({Changed});

	// Play In Editor: the ticker reads nothing and says it is paused.
	bPause = true;
	Index->TickForTests(0.016f);
	TestTrue(TEXT("Paused while Play In Editor runs"), Index->GetPublished()->bPaused);
	TestTrue(TEXT("and reads no folder"), Source->AssetReads.IsEmpty());
	bPause = false;
	Index->TickForTests(0.016f);
	Index->Step(1.0, 64, /*bForcePublish=*/true);
	const TSharedRef<const FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe> After = Index->GetPublished();
	TestFalse(TEXT("Not paused once it stops"), After->bPaused);
	TestTrue(TEXT("Only the changed folder was read"), Source->AssetReads.Num() == 1 && Source->AssetReads[0] == FName(TEXT("/Game/Env/Props")));
	TestTrue(TEXT("Unchanged folders are shared, not rebuilt"), FolderOf(*Before, TEXT("/Game/Env/Walls")) == FolderOf(*After, TEXT("/Game/Env/Walls")));
	TestTrue(TEXT("The changed one is new"), FolderOf(*Before, TEXT("/Game/Env/Props")) != FolderOf(*After, TEXT("/Game/Env/Props")));
	TestEqual(TEXT("One more asset"), After->AssetCount, Before->AssetCount + 1);
	const TArray<FString> Lava = Names(Rank(*After, AnalyzeQuery(TEXT("lava")), RankLimits, bTruncated));
	TestTrue(TEXT("The new asset is found by meaning at once (magma is lava)"), Lava.Num() > 0 && Lava[0] == TEXT("SM_Magma_Chunk"));
	TestTrue(TEXT("The index grew"), After->PostingCount > Before->PostingCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMentionsTest,
	"HyperAIStudio.Mentions.PickerAndChips",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioMentionsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Mentions;
	const auto Make = [](const EHyperAIStudioMentionKind Kind, const TCHAR* Label, const TCHAR* Reference, const TCHAR* Detail = TEXT(""))
	{
		FHyperAIStudioMention Mention;
		Mention.Kind = Kind;
		Mention.Label = Label;
		Mention.Reference = Reference;
		Mention.Detail = Detail;
		return Mention;
	};
	const FHyperAIStudioMention Rock = Make(EHyperAIStudioMentionKind::Asset, TEXT("SM_Rock"), TEXT("/Game/Env/SM_Rock.SM_Rock"), TEXT("StaticMesh"));
	const FHyperAIStudioMention RockLarge = Make(EHyperAIStudioMentionKind::Asset, TEXT("SM_RockLarge_Mossy"), TEXT("/Game/Env/SM_RockLarge_Mossy.SM_RockLarge_Mossy"), TEXT("StaticMesh"));
	const FHyperAIStudioMention Rocky = Make(EHyperAIStudioMentionKind::Asset, TEXT("T_Rocky_Ground"), TEXT("/Game/Env/T_Rocky_Ground.T_Rocky_Ground"), TEXT("Texture2D"));
	const FHyperAIStudioMention Torch = Make(EHyperAIStudioMentionKind::Actor, TEXT("Torch 01"), TEXT("/Game/Maps/Keep.Keep:PersistentLevel.Torch_01"), TEXT("PointLight"));
	const FHyperAIStudioMention Readme = Make(EHyperAIStudioMentionKind::File, TEXT("README.md"), TEXT("E:/Proj/README.md"), TEXT("README.md"));
	const FHyperAIStudioMention Header = Make(EHyperAIStudioMentionKind::File, TEXT("Spawner.h"), TEXT("E:/Proj/Source/Game/Rocks/Spawner.h"), TEXT("Source/Game/Rocks/Spawner.h"));
	const FHyperAIStudioMention Skill = Make(EHyperAIStudioMentionKind::Skill, TEXT("image-match"), TEXT("E:/Proj/.agents/skills/image-match/SKILL.md"));
	const FHyperAIStudioMention Operation = Make(EHyperAIStudioMentionKind::Operation, TEXT("hyper_scene_apply_plan op-1234"), TEXT("op-1234abcd"), TEXT("Completed"));
	const TArray<FHyperAIStudioMention> All = {Readme, Torch, Rocky, RockLarge, Rock, Header, Skill, Operation};

	// Fuzzy scoring: letters in order, start and word starts first, shorter names first.
	TestTrue(TEXT("smro matches SM_Rock"), FuzzyScore(TEXT("smro"), TEXT("SM_Rock")) > 0);
	TestEqual(TEXT("letters out of order do not match"), FuzzyScore(TEXT("orms"), TEXT("SM_Rock")), -1);
	TestTrue(TEXT("a word start beats the middle of a word"), FuzzyScore(TEXT("rock"), TEXT("SM_Rock")) > FuzzyScore(TEXT("rock"), TEXT("SM_Brocken")));
	TestTrue(TEXT("the shorter of two equal matches wins"), FuzzyScore(TEXT("rock"), TEXT("SM_Rock")) > FuzzyScore(TEXT("rock"), TEXT("SM_RockLarge_Mossy")));

	const TArray<FHyperAIStudioMention> Rocks = Filter(All, TEXT("rock"));
	TestTrue(TEXT("rock: SM_Rock first"), Rocks.Num() >= 3 && Rocks[0] == Rock);
	TestTrue(TEXT("then the other rocks, and the file in a Rocks folder"), Rocks.Contains(RockLarge) && Rocks.Contains(Rocky) && Rocks.Contains(Header));
	TestFalse(TEXT("not the torch"), Rocks.Contains(Torch));
	const TArray<FHyperAIStudioMention> Images = Filter(All, TEXT("imgm"));
	TestTrue(TEXT("Skills are found by name"), Images.Num() == 1 && Images[0] == Skill);
	TestTrue(TEXT("Operations by id"), Filter(All, TEXT("op-1234")).Contains(Operation));
	TestTrue(TEXT("Files by their project path too"), Filter(All, TEXT("source/game")).Contains(Header));
	TestEqual(TEXT("At most Max"), Filter(All, TEXT(""), 3).Num(), 3);

	// The active query: an @ at a word start, up to the cursor.
	int32 Start = INDEX_NONE;
	FString Query;
	TestTrue(TEXT("@ at the start"), FindActiveQuery(TEXT("@ro"), 3, Start, Query) && Start == 0 && Query == TEXT("ro"));
	TestTrue(TEXT("after a space"), FindActiveQuery(TEXT("move @SM_Ro now"), 11, Start, Query) && Start == 5 && Query == TEXT("SM_Ro"));
	TestTrue(TEXT("after a new line"), FindActiveQuery(TEXT("hi\n@to"), 6, Start, Query) && Start == 3 && Query == TEXT("to"));
	TestTrue(TEXT("a bare @ opens with an empty query"), FindActiveQuery(TEXT("see @"), 5, Start, Query) && Query.IsEmpty());
	TestFalse(TEXT("an email address is not a mention"), FindActiveQuery(TEXT("mail me@example"), 15, Start, Query));
	TestFalse(TEXT("nor is a finished token"), FindActiveQuery(TEXT("@SM_Rock "), 9, Start, Query));

	// Choosing replaces the query with the chip's token.
	int32 Cursor = 0;
	const FString Replaced = ReplaceQuery(TEXT("move @SM_Ro now"), 5, 11, Rock, Cursor);
	TestEqual(TEXT("The query becomes the token"), Replaced, FString(TEXT("move @SM_Rock now")));
	TestEqual(TEXT("the cursor after it"), Cursor, 14);
	TestEqual(TEXT("Spaces in a label become underscores"), Token(Torch), FString(TEXT("@Torch_01")));

	// Chips whose token is gone are dropped; @SM_Rock is not @SM_RockLarge_Mossy.
	const TArray<FHyperAIStudioMention> Chips = {Rock, Torch, Skill};
	const TArray<FHyperAIStudioMention> Kept = StillReferenced(TEXT("Put @SM_RockLarge_Mossy by @Torch_01."), Chips);
	TestTrue(TEXT("Only the torch is still referenced"), Kept.Num() == 1 && Kept[0] == Torch);

	// What the agent gets.
	const FString Message = Serialize(TEXT("Put @SM_Rock next to @Torch_01 and follow @image-match"), Chips);
	const FString Expected = FString(TEXT("Put @SM_Rock next to @Torch_01 and follow @image-match\n\nReferenced with @ (exact references):\n"))
		+ TEXT("- @SM_Rock: asset /Game/Env/SM_Rock.SM_Rock (StaticMesh)\n")
		+ TEXT("- @Torch_01: level actor \"Torch 01\" /Game/Maps/Keep.Keep:PersistentLevel.Torch_01 (PointLight)\n")
		+ TEXT("- @image-match: skill E:/Proj/.agents/skills/image-match/SKILL.md");
	TestEqual(TEXT("Each chip becomes one exact reference line"), Message, Expected);
	TestEqual(TEXT("A message without chips goes as typed"), Serialize(TEXT("just text"), {}), FString(TEXT("just text")));
	FHyperAIStudioMention File = Readme;
	TestTrue(TEXT("A file is sent by its path"), Serialize(TEXT("read @README.md"), {File}).EndsWith(TEXT("- @README.md: file E:/Proj/README.md")));

	TestTrue(TEXT("Ordinary pasted text is not a chip"), FromPastedText(TEXT("make the rocks bigger")).IsEmpty());
	TestTrue(TEXT("Nor a path to nothing"), FromPastedText(TEXT("/Game/__NoSuchFolder__/Nothing.Nothing")).IsEmpty());

	// The picker's project files come from the project scan; here a fixed list.
	SetProjectFilesForTests(TArray<FString>{TEXT("E:/Proj/Source/Game/RockSpawner.h"), TEXT("E:/Proj/Config/DefaultGame.ini")});
	ON_SCOPE_EXIT { SetProjectFilesForTests(TOptional<TArray<FString>>()); };
	const TArray<FHyperAIStudioMention> Gathered = Gather(TEXT("defaultgame"));
	TestTrue(TEXT("Gather finds a project file"), Gathered.ContainsByPredicate([](const FHyperAIStudioMention& Each)
	{
		return Each.Kind == EHyperAIStudioMentionKind::File && Each.Reference == TEXT("E:/Proj/Config/DefaultGame.ini");
	}));
	TestTrue(TEXT("The footer says where the index is"), DescribeIndex().Contains(TEXT("on this computer")));
	const TSharedRef<SHyperAIStudioMentionPicker> Picker = SNew(SHyperAIStudioMentionPicker);
	Picker->SetItems(Filter(All, TEXT("rock")));
	Picker->MoveSelection(1);
	TestTrue(TEXT("Down moves the selection"), Picker->GetSelected() && *Picker->GetSelected() == Rocks[1]);
	Picker->MoveSelection(-2);
	TestTrue(TEXT("and Up wraps around"), Picker->GetSelected() && *Picker->GetSelected() == Rocks.Last());
	FHyperAIStudioMention Chosen;
	const TSharedRef<SHyperAIStudioMentionPicker> Choosing = SNew(SHyperAIStudioMentionPicker)
		.OnChosen_Lambda([&Chosen](const FHyperAIStudioMention& Mention) { Chosen = Mention; });
	Choosing->SetItems(Rocks);
	TestTrue(TEXT("Enter picks the selected one"), Choosing->ChooseSelected() && Chosen == Rock);
	const FString Shot = HyperAIStudio::Tests::SaveWidgetShot(Picker, FIntPoint(580, 360), TEXT("mention-picker"));
	AddInfo(TEXT("Mention picker: ") + (Shot.IsEmpty() ? FString(TEXT("not rendered here")) : Shot));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
