// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "HyperAIStudioPoseSearchToolset.h"
#include "HyperAIStudioSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Tests/AutomationCommon.h"
#include "UObject/Package.h"

/** Deletes the test's assets a few frames on, once the engine's index manager has let go of the ones it built. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FHyperAIPoseSearchDeleteLater, TArray<FString>, ObjectPaths);

bool FHyperAIPoseSearchDeleteLater::Update()
{
	TArray<UObject*> Live;
	for (const FString& Path : ObjectPaths)
	{
		UObject* Object = FSoftObjectPath(Path).ResolveObject();
		if (IsValid(Object))
		{
			Live.Add(Object);
		}
	}
	if (!Live.IsEmpty())
	{
		ObjectTools::ForceDeleteObjects(Live, /*ShowConfirmation=*/false);
	}
	return true;
}

namespace HyperAIStudio::PoseSearchTests
{
	using C = FHyperAIStudioPoseSearchContracts;

	FString ObjectPathOf(const FString& Package)
	{
		return Package + TEXT(".") + FPackageName::GetShortName(Package);
	}

	FHyperAIPoseSearchOp Op(const TCHAR* Kind, const FString& Asset, TArray<FHyperAIPoseSearchSetting> Settings = {})
	{
		FHyperAIPoseSearchOp Out;
		Out.Kind = Kind;
		Out.Asset = Asset;
		Out.Settings = MoveTemp(Settings);
		return Out;
	}

	FHyperAIPoseSearchSetting Set(const TCHAR* Name, const FString& Value)
	{
		FHyperAIPoseSearchSetting Out;
		Out.Name = Name;
		Out.Value = Value;
		return Out;
	}

	FHyperAIPoseSearchPlanReport Run(TArray<FHyperAIPoseSearchOp> Ops, const bool bDryRun = false)
	{
		FHyperAIPoseSearchPlanRequest Plan;
		Plan.Operations = MoveTemp(Ops);
		Plan.bDryRun = bDryRun;
		return C::ApplyPlan(Plan);
	}

	FHyperAIPoseSearchAssetInfo Inspect(const FString& Path)
	{
		FHyperAIPoseSearchInspectRequest Look;
		Look.Paths = {Path};
		const FHyperAIPoseSearchInspectReport Report = C::Inspect(Look);
		return Report.bOk && Report.Assets.Num() == 1 ? Report.Assets[0] : FHyperAIPoseSearchAssetInfo();
	}

	/** The pose count out of a Built line or an Index state: "...built: 42 poses from ...". */
	int32 PosesIn(const FString& Line)
	{
		FString After;
		if (!Line.Split(TEXT("built: "), nullptr, &After))
		{
			return -1;
		}
		return FCString::Atoi(*After);
	}

	/** Two non-additive sequences of a second or more on one skeleton under /Game; the Third Person template's Manny has them. */
	TArray<UAnimSequence*> FindClips()
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(UAnimSequence::StaticClass()->GetClassPathName(), Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
		TMap<const USkeleton*, TArray<UAnimSequence*>> BySkeleton;
		for (const FAssetData& Asset : Assets)
		{
			const FString Package = Asset.PackageName.ToString();
			if (!Package.StartsWith(TEXT("/Game/")) || Package.StartsWith(TEXT("/Game/__")))
			{
				continue;
			}
			UAnimSequence* Clip = Cast<UAnimSequence>(Asset.GetAsset());
			if (!Clip || Clip->IsValidAdditive() || !Clip->GetSkeleton() || Clip->GetPlayLength() < 1.f)
			{
				continue;
			}
			TArray<UAnimSequence*>& Found = BySkeleton.FindOrAdd(Clip->GetSkeleton());
			Found.Add(Clip);
			if (Found.Num() == 2)
			{
				return Found;
			}
		}
		return {};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPoseSearchEndToEndTest,
	"HyperAIStudio.NativeTools.AnimationRigging.PoseSearchEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPoseSearchEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PoseSearchTests;
	const TArray<UAnimSequence*> Clips = FindClips();
	if (Clips.Num() < 2)
	{
		AddInfo(TEXT("Skipped: no skeleton under /Game has two non-additive Animation Sequences of a second or more."));
		return true;
	}
	USkeleton* Skeleton = Clips[0]->GetSkeleton();
	const FString Folder = TEXT("/Game/__HyperAIStudioTests/PoseSearch_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString SchemaPath = Folder + TEXT("/PSS_Locomotion");
	const FString DatabasePath = Folder + TEXT("/PSD_Locomotion");
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bApproval;
		const TArray<FString> Made = {ObjectPathOf(DatabasePath), ObjectPathOf(SchemaPath)};
		ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(1.f));
		ADD_LATENT_AUTOMATION_COMMAND(FHyperAIPoseSearchDeleteLater(Made));
	};

	FHyperAIPoseSearchOp MakeSchema = Op(TEXT("schema.create"), SchemaPath, {Set(TEXT("SampleRate"), TEXT("30"))});
	MakeSchema.Skeleton = Skeleton->GetPathName();
	FHyperAIPoseSearchOp MakeDatabase = Op(TEXT("database.create"), DatabasePath, {Set(TEXT("Tags"), TEXT("(\"Locomotion\")"))});
	MakeDatabase.Schema = SchemaPath;
	FHyperAIPoseSearchOp AddClips = Op(TEXT("database.add_animations"), DatabasePath, {Set(TEXT("MirrorOption"), TEXT("UnmirroredOnly"))});
	AddClips.Animations = {Clips[0]->GetPathName(), Clips[1]->GetPathName()};
	const FHyperAIPoseSearchOp Build = Op(TEXT("database.build"), DatabasePath);

	// Refused before anything changes: a setting the schema lacks, a value that is not one, a channel type that does not exist.
	FHyperAIPoseSearchOp Nope = MakeSchema;
	Nope.Settings = {Set(TEXT("Nope"), TEXT("1"))};
	const FHyperAIPoseSearchPlanReport NoSuchSetting = Run({Nope});
	TestTrue(TEXT("A setting the schema lacks is refused by name, with the ones it has: ") + NoSuchSetting.Diagnostic, !NoSuchSetting.bOk
		&& NoSuchSetting.Status == TEXT("invalid_request") && NoSuchSetting.Diagnostic.Contains(TEXT("Nope")) && NoSuchSetting.Diagnostic.Contains(TEXT("SampleRate")));
	FHyperAIPoseSearchOp BadValue = MakeSchema;
	BadValue.Settings = {Set(TEXT("SampleRate"), TEXT("fast"))};
	TestFalse(TEXT("so is a value that is not one"), Run({BadValue}, /*bDryRun=*/true).bOk);
	FHyperAIPoseSearchOp Telepathy = Op(TEXT("schema.add_channel"), SchemaPath);
	Telepathy.Channel = TEXT("Telepathy");
	const FHyperAIPoseSearchPlanReport NoSuchChannel = Run({MakeSchema, Telepathy}, /*bDryRun=*/true);
	TestTrue(TEXT("and a channel type that does not exist, with the types there are: ") + NoSuchChannel.Diagnostic, !NoSuchChannel.bOk
		&& NoSuchChannel.Diagnostic.Contains(TEXT("Trajectory")) && !FindObject<UObject>(nullptr, *ObjectPathOf(SchemaPath)));

	// One plan: a schema with Epic's default channels, a database of two clips, and its index built.
	const FHyperAIPoseSearchPlanReport Done = Run({MakeSchema, MakeDatabase, AddClips, Build});
	if (!TestTrue(TEXT("Schema, database and index in one plan: ") + Done.Diagnostic + TEXT(" | ") + FString::Join(Done.Notes, TEXT(" | ")),
		Done.bOk && Done.Status == TEXT("completed") && Done.Saved.Num() == 2 && Done.Built.Num() == 1))
	{
		return false;
	}
	const int32 PosesBefore = PosesIn(Done.Built[0]);
	TestTrue(TEXT("The index has poses: ") + Done.Built[0], PosesBefore > 0);
	const FHyperAIPoseSearchAssetInfo Schema = Inspect(SchemaPath);
	TestTrue(TEXT("The schema reads its skeleton, the default trajectory and pose channels, and its sample rate: ") + FString::Join(Schema.Channels, TEXT(" | ")),
		Schema.Kind == TEXT("schema") && Schema.Skeletons.Num() == 1 && Schema.Skeletons[0].StartsWith(Skeleton->GetPathName())
		&& Schema.Channels.Num() == 2 && Schema.Channels[0].StartsWith(TEXT("0 Trajectory:")) && Schema.Channels[1].StartsWith(TEXT("1 Pose:"))
		&& Schema.Settings.Contains(TEXT("SampleRate = 30")));
	const FHyperAIPoseSearchAssetInfo Database = Inspect(DatabasePath);
	TestTrue(TEXT("The database reads its schema, two clips, its tag and a built index: ") + Database.Index, Database.Kind == TEXT("database")
		&& Database.Schema == ObjectPathOf(SchemaPath) && Database.Animations.Num() == 2 && PosesIn(Database.Index) == PosesBefore
		&& Database.Settings.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("Tags = ")) && Line.Contains(TEXT("Locomotion")); }));

	// A second plan: a velocity channel added, the pose channel reweighted, brute-force search, one clip trimmed, the other removed.
	const FString Bone = Skeleton->GetReferenceSkeleton().GetBoneName(FMath::Min(1, Skeleton->GetReferenceSkeleton().GetNum() - 1)).ToString();
	FHyperAIPoseSearchOp AddVelocity = Op(TEXT("schema.add_channel"), SchemaPath,
		{Set(TEXT("Bone"), FString::Printf(TEXT("(BoneName=\"%s\")"), *Bone)), Set(TEXT("Weight"), TEXT("0.5"))});
	AddVelocity.Channel = TEXT("Velocity");
	FHyperAIPoseSearchOp Heavier = Op(TEXT("schema.set_channel"), SchemaPath, {Set(TEXT("Weight"), TEXT("2"))});
	Heavier.ChannelIndex = 1;
	const FHyperAIPoseSearchOp BruteForce = Op(TEXT("database.set"), DatabasePath, {Set(TEXT("PoseSearchMode"), TEXT("BruteForce"))});
	FHyperAIPoseSearchOp Trim = Op(TEXT("database.set_animations"), DatabasePath, {Set(TEXT("SamplingRange"), TEXT("(Min=0,Max=0.5)"))});
	Trim.Animations = {Clips[0]->GetPathName()};
	FHyperAIPoseSearchOp Drop = Op(TEXT("database.remove_animations"), DatabasePath);
	Drop.Animations = {Clips[1]->GetPathName()};
	FHyperAIPoseSearchOp DropTwice = Drop;
	TestFalse(TEXT("Removing a clip twice is refused"), Run({Drop, DropTwice}, /*bDryRun=*/true).bOk);
	FHyperAIPoseSearchOp NoSuchChannelIndex = Heavier;
	NoSuchChannelIndex.ChannelIndex = 9;
	TestFalse(TEXT("and so is a channel the schema does not have"), Run({NoSuchChannelIndex}, /*bDryRun=*/true).bOk);

	const FHyperAIPoseSearchPlanReport Edited = Run({AddVelocity, Heavier, BruteForce, Trim, Drop, Build});
	TestTrue(TEXT("The edit plan applies and rebuilds: ") + Edited.Diagnostic, Edited.bOk && Edited.Applied == 6 && Edited.Built.Num() == 1);
	const FHyperAIPoseSearchAssetInfo SchemaAfter = Inspect(SchemaPath);
	TestTrue(TEXT("The schema has the velocity channel on its bone and the reweighted pose channel: ") + FString::Join(SchemaAfter.Channels, TEXT(" | ")),
		SchemaAfter.Channels.Num() == 3 && SchemaAfter.Channels[2].StartsWith(TEXT("2 Velocity:")) && SchemaAfter.Channels[2].Contains(Bone)
		&& SchemaAfter.Channels[1].Contains(TEXT("Weight = 2.0")));
	const FHyperAIPoseSearchAssetInfo DatabaseAfter = Inspect(DatabasePath);
	const int32 PosesAfter = Edited.Built.IsEmpty() ? -1 : PosesIn(Edited.Built[0]);
	TestTrue(FString::Printf(TEXT("The database searches half a second of one clip, by brute force: %d poses, from %d; %s"), PosesAfter, PosesBefore,
		*FString::Join(DatabaseAfter.Animations, TEXT(" | "))), DatabaseAfter.Animations.Num() == 1 && DatabaseAfter.Animations[0].Contains(TEXT("Max=0.5"))
		&& DatabaseAfter.Settings.Contains(TEXT("PoseSearchMode = BruteForce")) && PosesAfter > 0 && PosesAfter < PosesBefore);
	if (GEditor)
	{
		GEditor->UndoTransaction();
		TestEqual(TEXT("Edit > Undo takes the whole edit plan back"), Inspect(SchemaPath).Channels.Num(), 2);
		TestEqual(TEXT("clips and all"), Inspect(DatabasePath).Animations.Num(), 2);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
