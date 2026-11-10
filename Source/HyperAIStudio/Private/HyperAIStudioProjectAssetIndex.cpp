// Games by Hyper 2026.

#include "HyperAIStudioProjectAssetIndex.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/BlueprintSupport.h"
#include "Editor.h"
#include "HyperAIStudioSemanticSearch.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/ScopeLock.h"

namespace HyperAIStudio::ProjectAssetIndex::Private
{
	constexpr int32 MaxContentRoots = 64;

	/** Reads the live Asset Registry, one folder per call. */
	class FRegistryAssetSource final : public IHyperAIProjectAssetSource
	{
	public:
		virtual bool IsAvailable() const override
		{
			return IAssetRegistry::Get() != nullptr;
		}

		virtual bool IsScanning() const override
		{
			const IAssetRegistry* Registry = IAssetRegistry::Get();
			return Registry && Registry->IsLoadingAssets();
		}

		virtual TArray<FName> GetContentRoots() const override
		{
			// The project's own content: /Game and the content plugins it carries, not engine or marketplace content.
			TArray<FString> PluginRoots;
			for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPluginsWithContent())
			{
				if (Plugin->GetLoadedFrom() != EPluginLoadedFrom::Project)
				{
					continue;
				}
				FString Root = Plugin->GetMountedAssetPath();
				while (Root.EndsWith(TEXT("/")))
				{
					Root.LeftChopInline(1);
				}
				if (Root.Len() > 1 && Root != TEXT("/Game"))
				{
					PluginRoots.AddUnique(Root);
				}
			}
			PluginRoots.Sort();
			if (PluginRoots.Num() > MaxContentRoots - 1)
			{
				PluginRoots.SetNum(MaxContentRoots - 1);
			}
			TArray<FName> Roots = { FName(TEXT("/Game")) };
			for (const FString& Root : PluginRoots)
			{
				Roots.Add(FName(*Root));
			}
			Roots.Sort(FNameLexicalLess());
			return Roots;
		}

		virtual void GetChildFolders(const FName Folder, TArray<FName>& OutChildren) const override
		{
			if (const IAssetRegistry* Registry = IAssetRegistry::Get())
			{
				Registry->GetSubPaths(Folder, OutChildren, false);
			}
		}

		virtual void GetFolderAssets(const FName Folder, TArray<FAssetData>& OutAssets) const override
		{
			if (const IAssetRegistry* Registry = IAssetRegistry::Get())
			{
				// bIncludeOnlyOnDiskAssets must stay true. Without it the registry also looks for in-memory assets of
				// the path by iterating every loaded UObject, which makes each folder read cost the whole editor.
				Registry->GetAssetsByPath(Folder, OutAssets, false, true);
			}
		}

		virtual bool FollowsRegistryEvents() const override
		{
			return true;
		}
	};

	void AddDiagnostic(
		TArray<FHyperAIReadDiagnostic>& Diagnostics,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& Message)
	{
		FHyperAIReadDiagnostic& Diagnostic = Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Code = Code;
		Diagnostic.Severity = Severity;
		Diagnostic.Field = TEXT("asset_index");
		Diagnostic.Message = Message;
	}

	bool IsUnderRoot(const FStringView Folder, const FString& Root)
	{
		return Folder.Equals(Root, ESearchCase::IgnoreCase)
			|| (Folder.Len() > Root.Len()
				&& Folder.StartsWith(Root, ESearchCase::IgnoreCase)
				&& Folder[Root.Len()] == TEXT('/'));
	}
}

TSharedRef<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe> FHyperAIStudioProjectAssetIndex::MakeForRegistry()
{
	FLimits Limits;
	// Play In Editor gets the frame to itself; folders changed meanwhile stay queued and are read once it stops.
	Limits.ShouldPause = []()
	{
		return GEditor && (GEditor->PlayWorld != nullptr || GEditor->IsPlaySessionInProgress());
	};
	return MakeShared<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe>(
		MakeUnique<HyperAIStudio::ProjectAssetIndex::Private::FRegistryAssetSource>(),
		Limits);
}

FHyperAIStudioProjectAssetIndex::FHyperAIStudioProjectAssetIndex(
	TUniquePtr<IHyperAIProjectAssetSource> InSource,
	const FLimits& InLimits)
	: Source(MoveTemp(InSource))
	, Limits(InLimits)
	, Published(MakeShared<FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe>())
{
}

// The ticker and registry subscriptions hold only weak references, so an index dropped without Shutdown is inert.
FHyperAIStudioProjectAssetIndex::~FHyperAIStudioProjectAssetIndex() = default;

bool FHyperAIStudioProjectAssetIndex::IsExcludedFolder(const FStringView Folder)
{
	// World Partition's one-file-per-actor packages: thousands of them, and none is an asset anyone searches for.
	const FStringView ExternalActors(FPackagePath::GetExternalActorsFolderName());
	const FStringView ExternalObjects(FPackagePath::GetExternalObjectsFolderName());
	int32 SegmentStart = 0;
	for (int32 Index = 0; Index <= Folder.Len(); ++Index)
	{
		if (Index == Folder.Len() || Folder[Index] == TEXT('/'))
		{
			const FStringView Segment = Folder.Mid(SegmentStart, Index - SegmentStart);
			if (Segment.Equals(ExternalActors, ESearchCase::IgnoreCase)
				|| Segment.Equals(ExternalObjects, ESearchCase::IgnoreCase))
			{
				return true;
			}
			SegmentStart = Index + 1;
		}
	}
	return false;
}

void FHyperAIStudioProjectAssetIndex::EnsureStarted()
{
	if (bStarted || bShuttingDown || !IsInGameThread())
	{
		return;
	}
	bStarted = true;
	if (Limits.bTickOnCoreTicker)
	{
		const TWeakPtr<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe> WeakIndex = AsShared();
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakIndex](const float DeltaSeconds)
		{
			const TSharedPtr<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe> Pinned = WeakIndex.Pin();
			return Pinned.IsValid() && Pinned->Tick(DeltaSeconds);
		}));
	}
	bLastKnownAvailable = Source->IsAvailable();
	bLastKnownScanning = bLastKnownAvailable && Source->IsScanning();
	if (bLastKnownAvailable)
	{
		BeginWalk();
	}
	else
	{
		bNeedsFullRewalk = true;
	}
	Publish();
}

void FHyperAIStudioProjectAssetIndex::RequestFullRewalk()
{
	// A walk already under way reads everything; events cover folders it passed before they changed.
	if (bStarted && !bShuttingDown && !bWalkActive)
	{
		bNeedsFullRewalk = true;
	}
}

bool FHyperAIStudioProjectAssetIndex::Tick(float DeltaSeconds)
{
	if (bShuttingDown)
	{
		return false;
	}
	const bool bShouldPause = Limits.ShouldPause && Limits.ShouldPause();
	if (bShouldPause != bPaused)
	{
		bPaused = bShouldPause;
		bDirtySincePublish = true;
		Publish();
	}
	if (bPaused)
	{
		return true;
	}
	// Once a walk is done and nothing is queued, a step is a few flag reads.
	Step(Limits.MaxStepSeconds, Limits.MaxFoldersPerStep, false);
	return true;
}

void FHyperAIStudioProjectAssetIndex::BeginWalk()
{
	Roots = Source->GetContentRoots();
	{
		FScopeLock Lock(&PendingMutex);
		RootPrefixes.Reset();
		for (const FName Root : Roots)
		{
			RootPrefixes.Add(Root.ToString());
		}
		bPendingOverflow = false;
	}
	WalkQueue.Reset();
	WalkHead = 0;
	QueuedThisWalk.Reset();
	VisitedThisWalk.Reset();
	bFolderBoundReached = false;
	for (const FName Root : Roots)
	{
		if (WalkQueue.Num() >= Limits.MaxFolders)
		{
			bFolderBoundReached = true;
			break;
		}
		WalkQueue.Add(Root);
		QueuedThisWalk.Add(Root);
	}
	bWalkActive = true;
	bNeedsFullRewalk = false;
	bDirtySincePublish = true;
}

void FHyperAIStudioProjectAssetIndex::Subscribe()
{
	if (bSubscribed)
	{
		return;
	}
	if (!Source->FollowsRegistryEvents())
	{
		bSubscribed = true;
		return;
	}
	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return;
	}
	// Subscribing after the initial scan keeps the first gather's per-asset event storm away from the index.
	bSubscribed = true;
	const TWeakPtr<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe> WeakIndex = AsShared();
	const auto OnAssets = [WeakIndex](const TConstArrayView<FAssetData> Assets)
	{
		if (const TSharedPtr<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe> Pinned = WeakIndex.Pin())
		{
			Pinned->NoteAssetsChanged(Assets);
		}
	};
	AddedHandle = Registry->OnAssetsAdded().AddLambda(OnAssets);
	RemovedHandle = Registry->OnAssetsRemoved().AddLambda(OnAssets);
	UpdatedHandle = Registry->OnAssetsUpdated().AddLambda(OnAssets);
	UpdatedOnDiskHandle = Registry->OnAssetsUpdatedOnDisk().AddLambda(OnAssets);
	RenamedHandle = Registry->OnAssetRenamed().AddLambda([WeakIndex](const FAssetData& Asset, const FString& OldObjectPath)
	{
		if (const TSharedPtr<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe> Pinned = WeakIndex.Pin())
		{
			Pinned->NoteAssetRenamed(Asset, OldObjectPath);
		}
	});
}

void FHyperAIStudioProjectAssetIndex::NoteAssetsChanged(const TConstArrayView<FAssetData> Assets)
{
	FScopeLock Lock(&PendingMutex);
	if (bEventsClosed)
	{
		return;
	}
	for (const FAssetData& Asset : Assets)
	{
		NoteFolderLocked(Asset.PackagePath);
	}
}

void FHyperAIStudioProjectAssetIndex::NoteAssetRenamed(const FAssetData& Asset, const FString& OldObjectPath)
{
	FScopeLock Lock(&PendingMutex);
	if (bEventsClosed)
	{
		return;
	}
	NoteFolderLocked(Asset.PackagePath);
	const FString OldPackageName = FPackageName::ObjectPathToPackageName(OldObjectPath);
	if (!OldPackageName.IsEmpty())
	{
		NoteFolderLocked(FName(*FPackageName::GetLongPackagePath(OldPackageName)));
	}
}

void FHyperAIStudioProjectAssetIndex::NoteFolderLocked(const FName Folder)
{
	if (bPendingOverflow || Folder.IsNone() || PendingFolders.Contains(Folder))
	{
		return;
	}
	const FString FolderString = Folder.ToString();
	if (!RootPrefixes.ContainsByPredicate([&FolderString](const FString& Root)
		{
			return HyperAIStudio::ProjectAssetIndex::Private::IsUnderRoot(FolderString, Root);
		})
		|| IsExcludedFolder(FolderString))
	{
		return;
	}
	PendingFolders.Add(Folder);
	if (PendingFolders.Num() > Limits.MaxPendingFolders)
	{
		bPendingOverflow = true;
		PendingFolders.Empty();
	}
}

bool FHyperAIStudioProjectAssetIndex::PopPendingFolder(FName& OutFolder)
{
	FScopeLock Lock(&PendingMutex);
	if (PendingFolders.IsEmpty())
	{
		return false;
	}
	TSet<FName>::TIterator Iterator = PendingFolders.CreateIterator();
	OutFolder = *Iterator;
	Iterator.RemoveCurrent();
	return true;
}

void FHyperAIStudioProjectAssetIndex::Step(const double BudgetSeconds, const int32 MaxFolders, const bool bForcePublish)
{
	if (!bStarted || bShuttingDown || !IsInGameThread())
	{
		return;
	}
	const bool bAvailable = Source->IsAvailable();
	const bool bScanning = bAvailable && Source->IsScanning();
	if (bAvailable != bLastKnownAvailable || bScanning != bLastKnownScanning)
	{
		bLastKnownAvailable = bAvailable;
		bLastKnownScanning = bScanning;
		bDirtySincePublish = true;
	}
	if (bAvailable && !bScanning)
	{
		Subscribe();
		bool bOverflowed = false;
		{
			FScopeLock Lock(&PendingMutex);
			bOverflowed = bPendingOverflow;
			bPendingOverflow = false;
		}
		if (bNeedsFullRewalk || bOverflowed)
		{
			BeginWalk();
		}

		// Live edits first, then the walk; at least one folder per step, so a tiny budget still makes progress.
		const double StartSeconds = FPlatformTime::Seconds();
		const int32 FolderBound = FMath::Max(0, MaxFolders);
		int32 Visited = 0;
		while (Visited < FolderBound
			&& (Visited == 0 || FPlatformTime::Seconds() - StartSeconds < BudgetSeconds))
		{
			FName Folder;
			bool bWalkVisit = false;
			if (!PopPendingFolder(Folder))
			{
				if (!bWalkActive || WalkHead >= WalkQueue.Num())
				{
					break;
				}
				Folder = WalkQueue[WalkHead++];
				bWalkVisit = true;
			}
			VisitFolder(Folder, bWalkVisit);
			++Visited;
		}
		if (bWalkActive && WalkHead >= WalkQueue.Num())
		{
			FinishWalk();
		}
	}

	const bool bStatusChanged = PublishedStatus != ComputeStatus();
	if ((bDirtySincePublish || bStatusChanged)
		&& (bForcePublish || bStatusChanged
			|| FPlatformTime::Seconds() - LastPublishSeconds >= Limits.PublishIntervalSeconds))
	{
		Publish();
	}
}

void FHyperAIStudioProjectAssetIndex::VisitFolder(const FName Folder, const bool bWalkVisit)
{
	const FString FolderString = Folder.ToString();
	if (IsExcludedFolder(FolderString))
	{
		return;
	}
	if (bWalkActive)
	{
		VisitedThisWalk.Add(Folder);
	}
	if (bWalkVisit)
	{
		TArray<FName> Children;
		Source->GetChildFolders(Folder, Children);
		Children.Sort(FNameLexicalLess());
		for (const FName Child : Children)
		{
			if (Child.IsNone() || QueuedThisWalk.Contains(Child) || IsExcludedFolder(Child.ToString()))
			{
				continue;
			}
			if (WalkQueue.Num() >= Limits.MaxFolders)
			{
				bFolderBoundReached = true;
				break;
			}
			WalkQueue.Add(Child);
			QueuedThisWalk.Add(Child);
		}
	}

	TArray<FAssetData> Assets;
	Source->GetFolderAssets(Folder, Assets);
	struct FKeyedRow
	{
		FString Key;
		FHyperAIProjectAssetRow Row;
		HyperAIStudio::SemanticSearch::FDoc Doc;
	};
	TArray<FKeyedRow> KeyedRows;
	KeyedRows.Reserve(Assets.Num());
	for (const FAssetData& Asset : Assets)
	{
		// Only this folder's own assets: a source that answered recursively must not bypass the folder bounds.
		if (!Asset.IsValid() || Asset.IsRedirector() || Asset.PackagePath != Folder)
		{
			continue;
		}
		const FString PackageName = Asset.PackageName.ToString();
		if (IsExcludedFolder(PackageName))
		{
			continue;
		}
		FKeyedRow& Keyed = KeyedRows.AddDefaulted_GetRef();
		Keyed.Row.ObjectPath = Asset.GetSoftObjectPath();
		Keyed.Row.ClassPath = Asset.AssetClassPath;
		FString ParentTag;
		if (Asset.GetTagValue(FBlueprintTags::ParentClassPath, ParentTag) && !ParentTag.IsEmpty())
		{
			const FString Parent = FPackageName::ExportTextPathToObjectPath(ParentTag);
			if (!Parent.IsEmpty() && Parent.Len() < NAME_SIZE)
			{
				Keyed.Row.ParentClassPath = FName(*Parent);
			}
		}
		Keyed.Key = Keyed.Row.ObjectPath.ToString();
		Keyed.Row.SearchTextLower = Asset.AssetName.ToString() + TEXT(" ") + Keyed.Row.ClassPath.ToString()
			+ TEXT(" ") + PackageName;
		if (!Keyed.Row.ParentClassPath.IsNone())
		{
			Keyed.Row.SearchTextLower += TEXT(" ") + Keyed.Row.ParentClassPath.ToString();
		}
		Keyed.Row.SearchTextLower.ToLowerInline();

		// The meaning index reads what the registry already holds: name, class, folder, parent and short tag values.
		HyperAIStudio::SemanticSearch::FDocInput Input;
		Input.AssetName = Asset.AssetName.ToString();
		Input.ClassPath = Keyed.Row.ClassPath.ToString();
		Input.PackagePath = FolderString;
		Input.ParentClass = Keyed.Row.ParentClassPath.IsNone() ? FString() : Keyed.Row.ParentClassPath.ToString();
		for (const auto TagAndValue : Asset.TagsAndValues)
		{
			if (Input.Tags.Num() >= HyperAIStudio::SemanticSearch::MaxTagsPerAsset)
			{
				break;
			}
			const FString Value = TagAndValue.Value.AsString();
			if (TagAndValue.Key != FBlueprintTags::ParentClassPath && HyperAIStudio::SemanticSearch::IsUsefulTagValue(Value))
			{
				Input.Tags.Add(Value);
			}
		}
		Keyed.Doc = HyperAIStudio::SemanticSearch::BuildDoc(Input);
		Keyed.Row.DocLength = Keyed.Doc.Length;
		Keyed.Row.GramCount = static_cast<uint16>(FMath::Min(Keyed.Doc.GramCount, 65535));
	}
	KeyedRows.Sort([](const FKeyedRow& Left, const FKeyedRow& Right)
	{
		return Left.Key < Right.Key;
	});
	for (int32 Index = KeyedRows.Num() - 1; Index > 0; --Index)
	{
		if (KeyedRows[Index].Key == KeyedRows[Index - 1].Key)
		{
			KeyedRows.RemoveAt(Index);
		}
	}

	const TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>* Existing = Folders.Find(Folder);
	const bool bExisted = Existing != nullptr;
	const int32 OldRowCount = bExisted ? (*Existing)->Rows.Num() : 0;
	const bool bOldTruncated = bExisted && (*Existing)->bTruncated;
	const FString OldFingerprint = bExisted ? (*Existing)->Fingerprint : FString();
	const int32 Allowed = FMath::Max(0, Limits.MaxAssets - (AssetCount - OldRowCount));
	bool bTruncated = false;
	if (KeyedRows.Num() > Allowed)
	{
		KeyedRows.SetNum(Allowed);
		bTruncated = true;
	}
	if (KeyedRows.IsEmpty() && !bTruncated)
	{
		if (bExisted)
		{
			AssetCount -= OldRowCount;
			TruncatedFolderCount -= bOldTruncated ? 1 : 0;
			Folders.Remove(Folder);
			bDirtySincePublish = true;
		}
		return;
	}
	if (!bExisted && Folders.Num() >= Limits.MaxFolders)
	{
		bFolderBoundReached = true;
		bDirtySincePublish = true;
		return;
	}

	const TSharedRef<FHyperAIProjectAssetFolder, ESPMode::ThreadSafe> Chunk =
		MakeShared<FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>();
	Chunk->Path = Folder;
	Chunk->bTruncated = bTruncated;
	Chunk->Rows.Reserve(KeyedRows.Num());
	TArray<FString> Tokens;
	Tokens.Reserve(KeyedRows.Num() * 3 + 3);
	Tokens.Add(TEXT("project_asset_folder.v1"));
	Tokens.Add(FolderString);
	int64 Bytes = sizeof(FHyperAIProjectAssetFolder);
	for (FKeyedRow& Keyed : KeyedRows)
	{
		Tokens.Add(Keyed.Key);
		Tokens.Add(Keyed.Row.ClassPath.ToString());
		Tokens.Add(Keyed.Row.ParentClassPath.IsNone() ? FString() : Keyed.Row.ParentClassPath.ToString());
		// Tags feed the meaning index, so a changed tag re-reads the folder's postings.
		Tokens.Add(LexToString(FCrc::MemCrc32(Keyed.Doc.Features.GetData(),
			Keyed.Doc.Features.Num() * sizeof(HyperAIStudio::SemanticSearch::FFeatureWeight))));
		const int32 RowIndex = Chunk->Rows.Num();
		for (const HyperAIStudio::SemanticSearch::FFeatureWeight& Feature : Keyed.Doc.Features)
		{
			Chunk->Postings.FindOrAdd(Feature.Feature).Add({RowIndex, Feature.Weight});
		}
		Chunk->PostingCount += Keyed.Doc.Features.Num();
		Chunk->TotalDocLength += Keyed.Row.DocLength;
		Bytes += sizeof(FHyperAIProjectAssetRow) + Keyed.Row.SearchTextLower.GetAllocatedSize();
		Chunk->Rows.Add(MoveTemp(Keyed.Row));
	}
	Chunk->Postings.Compact();
	Bytes += Chunk->Postings.GetAllocatedSize()
		+ static_cast<int64>(Chunk->PostingCount) * sizeof(FHyperAIProjectAssetPosting)
		+ static_cast<int64>(Chunk->Postings.Num()) * sizeof(TArray<FHyperAIProjectAssetPosting>);
	Chunk->ApproxBytes = Bytes;
	Tokens.Add(bTruncated ? TEXT("truncated") : TEXT("complete"));
	Chunk->Fingerprint = FHyperAIStudioContextSearchContracts::HashTokens(Tokens);
	if (bExisted && OldFingerprint == Chunk->Fingerprint)
	{
		return;
	}
	AssetCount += Chunk->Rows.Num() - OldRowCount;
	TruncatedFolderCount += (bTruncated ? 1 : 0) - (bOldTruncated ? 1 : 0);
	Folders.Add(Folder, Chunk);
	bDirtySincePublish = true;
}

void FHyperAIStudioProjectAssetIndex::FinishWalk()
{
	// The sweep: folders the walk never reached are gone from disk (or now out of bounds).
	for (TMap<FName, TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>>::TIterator Iterator =
		Folders.CreateIterator(); Iterator; ++Iterator)
	{
		if (!VisitedThisWalk.Contains(Iterator.Key()))
		{
			AssetCount -= Iterator.Value()->Rows.Num();
			TruncatedFolderCount -= Iterator.Value()->bTruncated ? 1 : 0;
			Iterator.RemoveCurrent();
		}
	}
	bWalkActive = false;
	bHasCompletedWalk = true;
	CompletedUtc = FDateTime::UtcNow().ToIso8601();
	WalkQueue.Empty();
	WalkHead = 0;
	QueuedThisWalk.Empty();
	VisitedThisWalk.Empty();
	bDirtySincePublish = true;
}

bool FHyperAIStudioProjectAssetIndex::IsTruncated() const
{
	return bFolderBoundReached || TruncatedFolderCount > 0;
}

const TCHAR* FHyperAIStudioProjectAssetIndex::ComputeStatus() const
{
	if (!bStarted)
	{
		return TEXT("uninitialized");
	}
	if (!bLastKnownAvailable)
	{
		return TEXT("unavailable");
	}
	if (bLastKnownScanning && !bHasCompletedWalk)
	{
		return TEXT("waiting_for_registry");
	}
	if (bWalkActive || !bHasCompletedWalk)
	{
		return TEXT("indexing");
	}
	return IsTruncated() ? TEXT("partial") : TEXT("ready");
}

bool FHyperAIStudioProjectAssetIndex::IsSettled() const
{
	if (!bStarted || bShuttingDown || bHasCompletedWalk)
	{
		return true;
	}
	return !Source->IsAvailable() || Source->IsScanning();
}

void FHyperAIStudioProjectAssetIndex::Publish()
{
	using namespace HyperAIStudio::ProjectAssetIndex::Private;
	const TSharedRef<FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe> Snapshot =
		MakeShared<FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe>();
	Snapshot->Status = ComputeStatus();
	Snapshot->Generation = ++Generation;
	Snapshot->AssetCount = AssetCount;
	Snapshot->FolderCount = Folders.Num();
	{
		FScopeLock Lock(&PendingMutex);
		Snapshot->PendingFolderCount = PendingFolders.Num();
	}
	Snapshot->bTruncated = IsTruncated();
	Snapshot->bRegistryScanning = bLastKnownScanning;
	Snapshot->bHasCompletedWalk = bHasCompletedWalk;
	Snapshot->bPaused = bPaused;
	Snapshot->CompletedUtc = CompletedUtc;

	TArray<TPair<FString, TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>>> Sorted;
	Sorted.Reserve(Folders.Num());
	for (const TPair<FName, TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>>& Pair : Folders)
	{
		Sorted.Emplace(Pair.Key.ToString(), Pair.Value);
	}
	Sorted.Sort([](const auto& Left, const auto& Right)
	{
		return Left.Key < Right.Key;
	});
	TArray<FString> RootStrings;
	for (const FName Root : Roots)
	{
		RootStrings.Add(Root.ToString());
	}
	TArray<FString> Tokens;
	Tokens.Reserve(Sorted.Num() * 2 + 3);
	Tokens.Add(TEXT("project_asset_index.v1"));
	Tokens.Add(FString::Join(RootStrings, TEXT(",")));
	Snapshot->Folders.Reserve(Sorted.Num());
	for (TPair<FString, TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>>& Pair : Sorted)
	{
		Tokens.Add(Pair.Key);
		Tokens.Add(Pair.Value->Fingerprint);
		Snapshot->ApproxBytes += Pair.Value->ApproxBytes;
		Snapshot->PostingCount += Pair.Value->PostingCount;
		Snapshot->Folders.Add(Pair.Value);
	}
	Tokens.Add(Snapshot->bTruncated ? TEXT("truncated") : TEXT("complete"));
	Snapshot->Fingerprint = FHyperAIStudioContextSearchContracts::HashTokens(Tokens);

	if (Snapshot->Status == TEXT("unavailable"))
	{
		AddDiagnostic(Snapshot->Diagnostics, TEXT("asset_registry_unavailable"), TEXT("error"),
			TEXT("The Asset Registry is not available, so project assets cannot be indexed."));
	}
	if (Snapshot->bRegistryScanning)
	{
		AddDiagnostic(Snapshot->Diagnostics, TEXT("asset_registry_scanning"), TEXT("warning"),
			TEXT("The Asset Registry is still scanning the project; assets are indexed once it finishes, so a missing asset may not be absent."));
	}
	if (Snapshot->bTruncated)
	{
		AddDiagnostic(Snapshot->Diagnostics, TEXT("asset_index_bound_reached"), TEXT("warning"),
			FString::Printf(TEXT("The asset index reached its fixed bound of %d assets or %d folders; assets beyond it are not searched."),
				Limits.MaxAssets, Limits.MaxFolders));
	}

	LastPublishSeconds = FPlatformTime::Seconds();
	bDirtySincePublish = false;
	PublishedStatus = Snapshot->Status;
	FScopeLock Lock(&PublishMutex);
	Published = Snapshot;
}

TSharedRef<const FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe> FHyperAIStudioProjectAssetIndex::GetPublished() const
{
	FScopeLock Lock(&PublishMutex);
	return Published;
}

void FHyperAIStudioProjectAssetIndex::Shutdown()
{
	if (bShuttingDown)
	{
		return;
	}
	bShuttingDown = true;
	{
		FScopeLock Lock(&PendingMutex);
		bEventsClosed = true;
		PendingFolders.Empty();
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (bSubscribed && Source->FollowsRegistryEvents())
	{
		if (IAssetRegistry* Registry = IAssetRegistry::Get())
		{
			Registry->OnAssetsAdded().Remove(AddedHandle);
			Registry->OnAssetsRemoved().Remove(RemovedHandle);
			Registry->OnAssetsUpdated().Remove(UpdatedHandle);
			Registry->OnAssetsUpdatedOnDisk().Remove(UpdatedOnDiskHandle);
			Registry->OnAssetRenamed().Remove(RenamedHandle);
		}
	}
	AddedHandle.Reset();
	RemovedHandle.Reset();
	UpdatedHandle.Reset();
	UpdatedOnDiskHandle.Reset();
	RenamedHandle.Reset();
}
