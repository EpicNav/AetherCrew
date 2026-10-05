// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioProjectAssetIndex.h"

#include "Blueprint/BlueprintSupport.h"
#include "Misc/AutomationTest.h"

namespace HyperAIStudio::ProjectAssetIndex::Tests
{
	/** A fixed folder tree standing in for the Asset Registry; it records every folder the index reads. */
	class FFakeAssetSource final : public IHyperAIProjectAssetSource
	{
	public:
		virtual bool IsAvailable() const override
		{
			return bAvailable;
		}

		virtual bool IsScanning() const override
		{
			return bScanning;
		}

		virtual TArray<FName> GetContentRoots() const override
		{
			return Roots;
		}

		virtual void GetChildFolders(const FName Folder, TArray<FName>& OutChildren) const override
		{
			ChildReads.Add(Folder);
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
			Children.FindOrAdd(FName(Parent)).Add(FName(Folder));
		}

		static FAssetData MakeAsset(
			const TCHAR* Folder,
			const TCHAR* Name,
			const TCHAR* ClassPath = TEXT("/Script/Engine.StaticMesh"),
			const TCHAR* ParentClassTag = nullptr)
		{
			FAssetDataTagMap Tags;
			if (ParentClassTag)
			{
				Tags.Add(FBlueprintTags::ParentClassPath, ParentClassTag);
			}
			const FString PackageName = FString(Folder) + TEXT("/") + Name;
			return FAssetData(FName(*PackageName), FName(Folder), FName(Name), FTopLevelAssetPath(ClassPath), MoveTemp(Tags));
		}

		FAssetData AddAsset(
			const TCHAR* Folder,
			const TCHAR* Name,
			const TCHAR* ClassPath = TEXT("/Script/Engine.StaticMesh"),
			const TCHAR* ParentClassTag = nullptr)
		{
			const FAssetData Asset = MakeAsset(Folder, Name, ClassPath, ParentClassTag);
			Assets.FindOrAdd(FName(Folder)).Add(Asset);
			return Asset;
		}

		FAssetData RemoveAsset(const TCHAR* Folder, const TCHAR* Name)
		{
			FAssetData Removed;
			if (TArray<FAssetData>* Found = Assets.Find(FName(Folder)))
			{
				const int32 Index = Found->IndexOfByPredicate([Name](const FAssetData& Asset)
				{
					return Asset.AssetName == FName(Name);
				});
				if (Index != INDEX_NONE)
				{
					Removed = (*Found)[Index];
					Found->RemoveAt(Index);
				}
			}
			return Removed;
		}

		TArray<FName> Roots = { FName(TEXT("/Game")) };
		TMap<FName, TArray<FName>> Children;
		TMap<FName, TArray<FAssetData>> Assets;
		bool bAvailable = true;
		bool bScanning = false;
		mutable TArray<FName> ChildReads;
		mutable TArray<FName> AssetReads;
	};

	using FIndexRef = TSharedRef<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe>;

	FHyperAIStudioProjectAssetIndex::FLimits TestLimits()
	{
		FHyperAIStudioProjectAssetIndex::FLimits Limits;
		Limits.bTickOnCoreTicker = false;
		Limits.PublishIntervalSeconds = 0.0;
		return Limits;
	}

	/**
	 * Five folders and five assets:
	 * /Game (BP_Hero, a Character Blueprint), /Game/Anim, /Game/Anim/Unarmed (AS_Unarmed_Idle),
	 * /Game/Env (SM_Rock, SM_Tree), /Game/Env/Cliffs (SM_Cliff).
	 */
	TUniquePtr<FFakeAssetSource> MakeTree()
	{
		TUniquePtr<FFakeAssetSource> Source = MakeUnique<FFakeAssetSource>();
		Source->AddFolder(TEXT("/Game"), TEXT("/Game/Env"));
		Source->AddFolder(TEXT("/Game"), TEXT("/Game/Anim"));
		Source->AddFolder(TEXT("/Game/Anim"), TEXT("/Game/Anim/Unarmed"));
		Source->AddFolder(TEXT("/Game/Env"), TEXT("/Game/Env/Cliffs"));
		Source->AddAsset(TEXT("/Game"), TEXT("BP_Hero"), TEXT("/Script/Engine.Blueprint"),
			TEXT("/Script/CoreUObject.Class'/Script/Engine.Character'"));
		Source->AddAsset(TEXT("/Game/Anim/Unarmed"), TEXT("AS_Unarmed_Idle"), TEXT("/Script/Engine.AnimSequence"));
		Source->AddAsset(TEXT("/Game/Env"), TEXT("SM_Tree"));
		Source->AddAsset(TEXT("/Game/Env"), TEXT("SM_Rock"));
		Source->AddAsset(TEXT("/Game/Env/Cliffs"), TEXT("SM_Cliff"));
		return Source;
	}

	FIndexRef MakeIndex(TUniquePtr<FFakeAssetSource> Source, const FHyperAIStudioProjectAssetIndex::FLimits& Limits = TestLimits())
	{
		return MakeShared<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe>(MoveTemp(Source), Limits);
	}

	/** Starts the index and steps it, unbounded per step, until the walk is done. */
	TSharedRef<const FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe> BuildAll(FHyperAIStudioProjectAssetIndex& Index)
	{
		Index.EnsureStarted();
		for (int32 Attempt = 0; Attempt < 64 && !Index.GetPublished()->bHasCompletedWalk; ++Attempt)
		{
			Index.Step(10.0, MAX_int32);
		}
		return Index.GetPublished();
	}

	const FHyperAIProjectAssetRow* FindRow(const FHyperAIProjectAssetIndexSnapshot& Snapshot, const TCHAR* ObjectPath)
	{
		for (const TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>& Folder : Snapshot.Folders)
		{
			for (const FHyperAIProjectAssetRow& Row : Folder->Rows)
			{
				if (Row.ObjectPath.ToString() == ObjectPath)
				{
					return &Row;
				}
			}
		}
		return nullptr;
	}

	bool HasDiagnostic(const FHyperAIProjectAssetIndexSnapshot& Snapshot, const TCHAR* Code)
	{
		return Snapshot.Diagnostics.ContainsByPredicate([Code](const FHyperAIReadDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIProjectAssetIndexWalkBoundedTest,
	"HyperAIStudio.NativeTools.ContextSearch.AssetIndexWalkBounded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIProjectAssetIndexWalkBoundedTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ProjectAssetIndex::Tests;
	FHyperAIStudioProjectAssetIndex::FLimits Limits = TestLimits();
	Limits.MaxFoldersPerStep = 2;
	TUniquePtr<FFakeAssetSource> Tree = MakeTree();
	FFakeAssetSource* Source = Tree.Get();
	const FIndexRef Index = MakeIndex(MoveTemp(Tree), Limits);
	TestEqual(TEXT("an index nobody started is uninitialized"), Index->GetPublished()->Status, FString(TEXT("uninitialized")));

	Index->EnsureStarted();
	TestEqual(TEXT("starting reads nothing yet"), Source->AssetReads.Num(), 0);
	Index->Step(10.0, Limits.MaxFoldersPerStep);
	const auto AfterOneStep = Index->GetPublished();
	TestEqual(TEXT("one step reads at most its folder bound"), Source->AssetReads.Num(), 2);
	TestTrue(TEXT("one step publishes at most two folders"), AfterOneStep->FolderCount <= 2);
	TestEqual(TEXT("the walk is still indexing"), AfterOneStep->Status, FString(TEXT("indexing")));
	TestFalse(TEXT("no walk has completed yet"), AfterOneStep->bHasCompletedWalk);

	for (int32 Attempt = 0; Attempt < 10 && Index->GetPublished()->Status == TEXT("indexing"); ++Attempt)
	{
		Index->Step(10.0, Limits.MaxFoldersPerStep);
	}
	const auto Ready = Index->GetPublished();
	TestEqual(TEXT("repeated steps finish the walk"), Ready->Status, FString(TEXT("ready")));
	TestTrue(TEXT("the finished walk is recorded"), Ready->bHasCompletedWalk);
	TestEqual(TEXT("every asset is indexed"), Ready->AssetCount, 5);
	TestEqual(TEXT("only folders with assets are kept"), Ready->FolderCount, 4);
	TestEqual(TEXT("each of the five folders was read once"), Source->AssetReads.Num(), 5);
	TestFalse(TEXT("a finished walk is not truncated"), Ready->bTruncated);
	TestTrue(TEXT("the snapshot has a canonical fingerprint"), Ready->Fingerprint.StartsWith(TEXT("sha256:")) && Ready->Fingerprint.Len() == 71);
	for (int32 FolderIndex = 1; FolderIndex < Ready->Folders.Num(); ++FolderIndex)
	{
		TestTrue(TEXT("folders are sorted by path"),
			Ready->Folders[FolderIndex - 1]->Path.ToString() < Ready->Folders[FolderIndex]->Path.ToString());
	}
	for (const TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>& Folder : Ready->Folders)
	{
		for (int32 RowIndex = 1; RowIndex < Folder->Rows.Num(); ++RowIndex)
		{
			TestTrue(TEXT("rows are sorted by object path"),
				Folder->Rows[RowIndex - 1].ObjectPath.ToString() < Folder->Rows[RowIndex].ObjectPath.ToString());
		}
	}
	const FHyperAIProjectAssetRow* Rock = FindRow(*Ready, TEXT("/Game/Env/SM_Rock.SM_Rock"));
	TestNotNull(TEXT("SM_Rock is indexed"), Rock);
	if (Rock)
	{
		TestEqual(TEXT("its class is kept"), Rock->ClassPath.ToString(), FString(TEXT("/Script/Engine.StaticMesh")));
		TestTrue(TEXT("its search text holds name, class and folder"),
			Rock->SearchTextLower.Contains(TEXT("sm_rock")) && Rock->SearchTextLower.Contains(TEXT("staticmesh"))
			&& Rock->SearchTextLower.Contains(TEXT("/game/env")));
	}
	const FHyperAIProjectAssetRow* Hero = FindRow(*Ready, TEXT("/Game/BP_Hero.BP_Hero"));
	TestNotNull(TEXT("BP_Hero is indexed"), Hero);
	if (Hero)
	{
		TestEqual(TEXT("a Blueprint's parent class is read from its tag"),
			Hero->ParentClassPath.ToString(), FString(TEXT("/Script/Engine.Character")));
		TestTrue(TEXT("the parent class is searchable"), Hero->SearchTextLower.Contains(TEXT("/script/engine.character")));
	}

	// Once the walk is done, a step with nothing queued reads nothing.
	const int32 ReadsBefore = Source->AssetReads.Num();
	Index->Step(10.0, Limits.MaxFoldersPerStep);
	TestEqual(TEXT("an idle step reads no folder"), Source->AssetReads.Num(), ReadsBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIProjectAssetIndexSkipsTest,
	"HyperAIStudio.NativeTools.ContextSearch.AssetIndexSkipsRedirectorsAndExternalPackages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIProjectAssetIndexSkipsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ProjectAssetIndex::Tests;
	TUniquePtr<FFakeAssetSource> Tree = MakeUnique<FFakeAssetSource>();
	FFakeAssetSource* Source = Tree.Get();
	Source->AddFolder(TEXT("/Game"), TEXT("/Game/Maps"));
	Source->AddFolder(TEXT("/Game/Maps"), TEXT("/Game/Maps/__ExternalActors__"));
	Source->AddFolder(TEXT("/Game/Maps"), TEXT("/Game/Maps/__ExternalObjects__"));
	Source->AddFolder(TEXT("/Game/Maps/__ExternalActors__"), TEXT("/Game/Maps/__ExternalActors__/Main"));
	Source->AddAsset(TEXT("/Game/Maps"), TEXT("Main"), TEXT("/Script/Engine.World"));
	Source->AddAsset(TEXT("/Game/Maps"), TEXT("OldMain"), TEXT("/Script/CoreUObject.ObjectRedirector"));
	Source->AddAsset(TEXT("/Game/Maps/__ExternalActors__/Main"), TEXT("ABC123"), TEXT("/Script/Engine.StaticMeshActor"));
	Source->AddAsset(TEXT("/Game/Maps/__ExternalObjects__"), TEXT("DEF456"), TEXT("/Script/Engine.DataLayerInstance"));
	const FIndexRef Index = MakeIndex(MoveTemp(Tree));
	const auto Built = BuildAll(*Index);

	TestEqual(TEXT("only the map is indexed"), Built->AssetCount, 1);
	TestNotNull(TEXT("the map is indexed"), FindRow(*Built, TEXT("/Game/Maps/Main.Main")));
	TestNull(TEXT("the redirector is not indexed"), FindRow(*Built, TEXT("/Game/Maps/OldMain.OldMain")));
	for (const TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>& Folder : Built->Folders)
	{
		TestFalse(TEXT("no external-package folder is published"), Folder->Path.ToString().Contains(TEXT("__External")));
		for (const FHyperAIProjectAssetRow& Row : Folder->Rows)
		{
			TestFalse(TEXT("no redirector row"), Row.ClassPath.ToString() == TEXT("/Script/CoreUObject.ObjectRedirector"));
		}
	}
	const auto IsExternal = [](const FName Folder)
	{
		return Folder.ToString().Contains(TEXT("__External"));
	};
	TestFalse(TEXT("external-package folders are never listed"), Source->ChildReads.ContainsByPredicate(IsExternal));
	TestFalse(TEXT("external-package folders are never read"), Source->AssetReads.ContainsByPredicate(IsExternal));

	// A change event inside an external-package folder does not queue it either.
	const FAssetData ExternalActor = FFakeAssetSource::MakeAsset(
		TEXT("/Game/Maps/__ExternalActors__/Main"), TEXT("GHI789"), TEXT("/Script/Engine.StaticMeshActor"));
	Index->NoteAssetsChanged(MakeArrayView(&ExternalActor, 1));
	const FAssetData Outside = FFakeAssetSource::MakeAsset(TEXT("/Engine/Basic"), TEXT("Cube"));
	Index->NoteAssetsChanged(MakeArrayView(&Outside, 1));
	Index->Step(10.0, MAX_int32);
	TestFalse(TEXT("an external-actor event reads no folder"), Source->AssetReads.ContainsByPredicate(IsExternal));
	TestFalse(TEXT("an event outside the content roots reads no folder"), Source->AssetReads.Contains(FName(TEXT("/Engine/Basic"))));
	TestTrue(TEXT("the helper spots external folders at any depth"),
		FHyperAIStudioProjectAssetIndex::IsExcludedFolder(TEXT("/Game/Maps/__ExternalObjects__/A/B")));
	TestFalse(TEXT("an ordinary folder is not excluded"),
		FHyperAIStudioProjectAssetIndex::IsExcludedFolder(TEXT("/Game/External/Props")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIProjectAssetIndexBoundTest,
	"HyperAIStudio.NativeTools.ContextSearch.AssetIndexBoundReached",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIProjectAssetIndexBoundTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ProjectAssetIndex::Tests;
	FHyperAIStudioProjectAssetIndex::FLimits Limits = TestLimits();
	Limits.MaxAssets = 3;
	Limits.MaxFolders = 4;
	const FIndexRef First = MakeIndex(MakeTree(), Limits);
	const auto Bounded = BuildAll(*First);
	TestEqual(TEXT("a bounded walk is partial"), Bounded->Status, FString(TEXT("partial")));
	TestTrue(TEXT("the snapshot says it was cut short"), Bounded->bTruncated);
	TestTrue(TEXT("the bound has a stable diagnostic"), HasDiagnostic(*Bounded, TEXT("asset_index_bound_reached")));
	TestEqual(TEXT("exactly the asset bound is kept"), Bounded->AssetCount, 3);
	TestNull(TEXT("the fifth folder was never queued"), FindRow(*Bounded, TEXT("/Game/Env/Cliffs/SM_Cliff.SM_Cliff")));

	const FIndexRef Second = MakeIndex(MakeTree(), Limits);
	const auto Again = BuildAll(*Second);
	TestEqual(TEXT("truncation is deterministic"), Again->Fingerprint, Bounded->Fingerprint);
	TestEqual(TEXT("the same assets survive"), Again->AssetCount, Bounded->AssetCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIProjectAssetIndexScanTest,
	"HyperAIStudio.NativeTools.ContextSearch.AssetIndexWaitsForRegistryScan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIProjectAssetIndexScanTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ProjectAssetIndex::Tests;
	TUniquePtr<FFakeAssetSource> Tree = MakeTree();
	FFakeAssetSource* Source = Tree.Get();
	Source->bScanning = true;
	const FIndexRef Index = MakeIndex(MoveTemp(Tree));
	Index->EnsureStarted();
	Index->Step(10.0, MAX_int32);
	const auto Waiting = Index->GetPublished();
	TestEqual(TEXT("a scanning registry is waited for"), Waiting->Status, FString(TEXT("waiting_for_registry")));
	TestEqual(TEXT("nothing is indexed while it scans"), Waiting->AssetCount, 0);
	TestEqual(TEXT("no folder is read while it scans"), Source->AssetReads.Num(), 0);
	TestTrue(TEXT("the scan is reported"), Waiting->bRegistryScanning);
	TestTrue(TEXT("the scan has a stable warning"), HasDiagnostic(*Waiting, TEXT("asset_registry_scanning")));
	TestTrue(TEXT("a first search does not wait on a scan it cannot finish"), Index->IsSettled());

	Source->bScanning = false;
	TestFalse(TEXT("once the scan ends the walk is owed"), Index->IsSettled());
	Index->Step(10.0, MAX_int32);
	const auto Ready = Index->GetPublished();
	TestEqual(TEXT("steps after the scan finish the walk"), Ready->Status, FString(TEXT("ready")));
	TestEqual(TEXT("every asset is indexed"), Ready->AssetCount, 5);
	TestFalse(TEXT("the scan warning is gone"), HasDiagnostic(*Ready, TEXT("asset_registry_scanning")));

	TUniquePtr<FFakeAssetSource> Missing = MakeTree();
	Missing->bAvailable = false;
	const FIndexRef Unavailable = MakeIndex(MoveTemp(Missing));
	Unavailable->EnsureStarted();
	TestEqual(TEXT("no registry means unavailable"), Unavailable->GetPublished()->Status, FString(TEXT("unavailable")));
	TestTrue(TEXT("unavailable has a stable error"), HasDiagnostic(*Unavailable->GetPublished(), TEXT("asset_registry_unavailable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIProjectAssetIndexEventsTest,
	"HyperAIStudio.NativeTools.ContextSearch.AssetIndexEventsAreIncremental",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIProjectAssetIndexEventsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ProjectAssetIndex::Tests;
	FHyperAIStudioProjectAssetIndex::FLimits Limits = TestLimits();
	Limits.MaxPendingFolders = 2;
	TUniquePtr<FFakeAssetSource> Tree = MakeTree();
	FFakeAssetSource* Source = Tree.Get();
	const FIndexRef Index = MakeIndex(MoveTemp(Tree), Limits);
	const auto Ready = BuildAll(*Index);
	TestEqual(TEXT("the walk is ready"), Ready->Status, FString(TEXT("ready")));

	// Added: exactly that folder is read again.
	Source->AssetReads.Reset();
	Source->ChildReads.Reset();
	const FAssetData Boulder = Source->AddAsset(TEXT("/Game/Env"), TEXT("SM_Boulder"));
	Index->NoteAssetsChanged(MakeArrayView(&Boulder, 1));
	Index->Step(10.0, MAX_int32);
	const auto Added = Index->GetPublished();
	TestEqual(TEXT("an added asset reads one folder"), Source->AssetReads.Num(), 1);
	TestTrue(TEXT("it is the asset's folder"), Source->AssetReads.Contains(FName(TEXT("/Game/Env"))));
	TestEqual(TEXT("an event lists no child folders"), Source->ChildReads.Num(), 0);
	TestTrue(TEXT("the generation moves"), Added->Generation > Ready->Generation);
	TestNotEqual(TEXT("the fingerprint moves"), Added->Fingerprint, Ready->Fingerprint);
	TestEqual(TEXT("the asset is counted"), Added->AssetCount, 6);
	TestNotNull(TEXT("the new asset is indexed"), FindRow(*Added, TEXT("/Game/Env/SM_Boulder.SM_Boulder")));
	TestEqual(TEXT("the folder is still ready"), Added->Status, FString(TEXT("ready")));
	TestTrue(TEXT("the unchanged folders are shared with the previous snapshot"),
		Added->Folders.ContainsByPredicate([&Ready](const TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>& Folder)
		{
			return Folder->Path == FName(TEXT("/Game/Env/Cliffs"))
				&& Ready->Folders.ContainsByPredicate([&Folder](const TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>& Previous)
				{
					return &Previous.Get() == &Folder.Get();
				});
		}));

	// Removed.
	const FAssetData Removed = Source->RemoveAsset(TEXT("/Game/Env"), TEXT("SM_Boulder"));
	Index->NoteAssetsChanged(MakeArrayView(&Removed, 1));
	Index->Step(10.0, MAX_int32);
	const auto AfterRemove = Index->GetPublished();
	TestNull(TEXT("a removed asset's row is gone"), FindRow(*AfterRemove, TEXT("/Game/Env/SM_Boulder.SM_Boulder")));
	TestEqual(TEXT("the count drops"), AfterRemove->AssetCount, 5);
	TestEqual(TEXT("removing it restores the earlier fingerprint"), AfterRemove->Fingerprint, Ready->Fingerprint);

	// Renamed from /Game/Env to /Game/Anim: both folders are read again.
	Source->AssetReads.Reset();
	Source->RemoveAsset(TEXT("/Game/Env"), TEXT("SM_Rock"));
	const FAssetData Moved = Source->AddAsset(TEXT("/Game/Anim"), TEXT("SM_Rock"));
	Index->NoteAssetRenamed(Moved, TEXT("/Game/Env/SM_Rock.SM_Rock"));
	Index->Step(10.0, MAX_int32);
	const auto AfterRename = Index->GetPublished();
	TestTrue(TEXT("a rename reads the old folder"), Source->AssetReads.Contains(FName(TEXT("/Game/Env"))));
	TestTrue(TEXT("a rename reads the new folder"), Source->AssetReads.Contains(FName(TEXT("/Game/Anim"))));
	TestNull(TEXT("the old folder loses the row"), FindRow(*AfterRename, TEXT("/Game/Env/SM_Rock.SM_Rock")));
	TestNotNull(TEXT("the new folder gains it"), FindRow(*AfterRename, TEXT("/Game/Anim/SM_Rock.SM_Rock")));
	TestEqual(TEXT("a rename keeps the count"), AfterRename->AssetCount, 5);

	// An added event for an asset that is not on disk yet (created, unsaved) indexes nothing.
	const FAssetData Unsaved = FFakeAssetSource::MakeAsset(TEXT("/Game/Env/Cliffs"), TEXT("SM_Unsaved"));
	Index->NoteAssetsChanged(MakeArrayView(&Unsaved, 1));
	Index->Step(10.0, MAX_int32);
	const auto AfterUnsaved = Index->GetPublished();
	TestNull(TEXT("an unsaved asset has no row"), FindRow(*AfterUnsaved, TEXT("/Game/Env/Cliffs/SM_Unsaved.SM_Unsaved")));
	TestEqual(TEXT("an unsaved asset changes nothing"), AfterUnsaved->Fingerprint, AfterRename->Fingerprint);

	// More queued folders than the bound: the index walks everything again, sweeping folders that are gone.
	Source->Children.FindOrAdd(FName(TEXT("/Game"))).Remove(FName(TEXT("/Game/Anim")));
	Source->ChildReads.Reset();
	const FAssetData Storm[] = {
		FFakeAssetSource::MakeAsset(TEXT("/Game/Storm1"), TEXT("A")),
		FFakeAssetSource::MakeAsset(TEXT("/Game/Storm2"), TEXT("B")),
		FFakeAssetSource::MakeAsset(TEXT("/Game/Storm3"), TEXT("C"))
	};
	Index->NoteAssetsChanged(MakeArrayView(Storm, UE_ARRAY_COUNT(Storm)));
	Index->Step(10.0, MAX_int32);
	const auto Rewalked = Index->GetPublished();
	TestTrue(TEXT("an overflow walks from the root again"), Source->ChildReads.Contains(FName(TEXT("/Game"))));
	TestEqual(TEXT("the rewalk finishes ready"), Rewalked->Status, FString(TEXT("ready")));
	TestNull(TEXT("the sweep drops a folder that is gone"), FindRow(*Rewalked, TEXT("/Game/Anim/SM_Rock.SM_Rock")));

	TUniquePtr<FFakeAssetSource> Copy = MakeUnique<FFakeAssetSource>(*Source);
	const FIndexRef Fresh = MakeIndex(MoveTemp(Copy), Limits);
	const auto FreshBuilt = BuildAll(*Fresh);
	TestEqual(TEXT("the rewalk matches a fresh build"), Rewalked->Fingerprint, FreshBuilt->Fingerprint);
	TestEqual(TEXT("and counts the same assets"), Rewalked->AssetCount, FreshBuilt->AssetCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIProjectAssetIndexFingerprintTest,
	"HyperAIStudio.NativeTools.ContextSearch.AssetIndexFingerprintStable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIProjectAssetIndexFingerprintTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ProjectAssetIndex::Tests;
	const FIndexRef First = MakeIndex(MakeTree());
	const FIndexRef Second = MakeIndex(MakeTree());
	const auto A = BuildAll(*First);
	const auto B = BuildAll(*Second);
	TestFalse(TEXT("the fingerprint is not empty"), A->Fingerprint.IsEmpty());
	TestEqual(TEXT("two builds of one tree agree"), A->Fingerprint, B->Fingerprint);

	TUniquePtr<FFakeAssetSource> Changed = MakeTree();
	Changed->RemoveAsset(TEXT("/Game/Env"), TEXT("SM_Tree"));
	Changed->AddAsset(TEXT("/Game/Env"), TEXT("SM_Tree"), TEXT("/Script/Engine.SkeletalMesh"));
	const FIndexRef Third = MakeIndex(MoveTemp(Changed));
	const auto C = BuildAll(*Third);
	TestNotEqual(TEXT("one changed class path changes it"), C->Fingerprint, A->Fingerprint);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
