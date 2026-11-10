// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Containers/Ticker.h"
#include "HyperAIStudioContextSearchToolsets.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"

/** One on-disk asset as project search sees it: identity, class, and the lowercase text queries match against. */
struct FHyperAIProjectAssetRow
{
	FSoftObjectPath ObjectPath;
	FTopLevelAssetPath ClassPath;
	/** Blueprint parent class from the registry's ParentClassPath tag; None for other assets. */
	FName ParentClassPath;
	/** Asset name, class path, package name and parent class, lowercased. */
	FString SearchTextLower;
	/** Sum of the asset's word weights in the meaning index, for BM25's length normalisation. */
	float DocLength = 0.0f;
	/** Distinct trigrams of its name's words, for spelling matches. */
	uint16 GramCount = 0;
};

/** One asset under one feature of the meaning index: the row in its folder and the feature's weight there. */
struct FHyperAIProjectAssetPosting
{
	int32 Row = 0;
	float Weight = 0.0f;
};

/** One folder's assets, read in one non-recursive registry call. Immutable once published. */
struct FHyperAIProjectAssetFolder
{
	FName Path;
	/** Sorted by object path. */
	TArray<FHyperAIProjectAssetRow> Rows;
	FString Fingerprint;
	/** The project-wide asset bound cut this folder short. */
	bool bTruncated = false;
	/**
	 * The folder's part of the meaning index (HyperAIStudioSemanticSearch): feature id to the rows that have it. Built
	 * when the folder is read, so an edit re-indexes only its own folder.
	 */
	TMap<uint32, TArray<FHyperAIProjectAssetPosting>> Postings;
	float TotalDocLength = 0.0f;
	int32 PostingCount = 0;
	/** Rough memory this folder holds: rows, their text and postings. */
	int64 ApproxBytes = 0;
};

/** Immutable published asset index. Folders a refresh did not re-read are shared with the previous snapshot. */
struct FHyperAIProjectAssetIndexSnapshot
{
	/** uninitialized, waiting_for_registry, indexing, ready, partial, unavailable, or not_attached. */
	FString Status = TEXT("uninitialized");
	int64 Generation = 0;
	FString Fingerprint;
	int32 AssetCount = 0;
	int32 FolderCount = 0;
	int32 PendingFolderCount = 0;
	bool bTruncated = false;
	bool bRegistryScanning = false;
	/** A walk of every content root has finished at least once, so the folders cover the whole project. */
	bool bHasCompletedWalk = false;
	/** Background reading waits while Play In Editor runs; a search still reads what it needs. */
	bool bPaused = false;
	/** Rough memory the index holds, in editor memory only. */
	int64 ApproxBytes = 0;
	int32 PostingCount = 0;
	/** Sorted by path. */
	TArray<TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>> Folders;
	TArray<FHyperAIReadDiagnostic> Diagnostics;
	FString CompletedUtc;
};

/** Where the index reads folders and assets from: the live Asset Registry, or a fixed tree in tests. */
class IHyperAIProjectAssetSource
{
public:
	virtual ~IHyperAIProjectAssetSource() = default;
	virtual bool IsAvailable() const = 0;
	/** The registry's initial scan is still running, so folder reads would be incomplete. */
	virtual bool IsScanning() const = 0;
	/** Content roots to walk, such as /Game; sorted. */
	virtual TArray<FName> GetContentRoots() const = 0;
	/** Direct child folders only. */
	virtual void GetChildFolders(FName Folder, TArray<FName>& OutChildren) const = 0;
	/** One folder's on-disk assets only, never recursive. */
	virtual void GetFolderAssets(FName Folder, TArray<FAssetData>& OutAssets) const = 0;
	/** Whether the index follows the live Asset Registry's change events; test sources do not. */
	virtual bool FollowsRegistryEvents() const { return false; }
};

/**
 * Bounded, incremental cache of the project's on-disk assets for hyper_project_search. A game-thread ticker walks the
 * content roots folder by folder (direct children, then that one folder's on-disk assets), so no call enumerates the
 * whole registry. Registry change events only queue folders to re-read. Readers take immutable snapshots.
 */
class FHyperAIStudioProjectAssetIndex final : public TSharedFromThis<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe>
{
public:
	struct FLimits
	{
		int32 MaxAssets = FHyperAIStudioContextSearchContracts::MaxProjectAssets;
		int32 MaxFolders = FHyperAIStudioContextSearchContracts::MaxProjectAssetFolders;
		int32 MaxFoldersPerStep = 64;
		double MaxStepSeconds = 0.004;
		/** More queued folders than this means a storm of edits; the index walks everything again instead. */
		int32 MaxPendingFolders = 1024;
		double PublishIntervalSeconds = 0.5;
		/** Tests drive Step themselves. */
		bool bTickOnCoreTicker = true;
		/** The background ticker reads nothing while this says so (Play In Editor runs, in the editor); unset never pauses. */
		TFunction<bool()> ShouldPause;
	};

	/** The production index over the live Asset Registry. */
	static TSharedRef<FHyperAIStudioProjectAssetIndex, ESPMode::ThreadSafe> MakeForRegistry();

	FHyperAIStudioProjectAssetIndex(TUniquePtr<IHyperAIProjectAssetSource> InSource, const FLimits& InLimits);
	~FHyperAIStudioProjectAssetIndex();

	/** Game thread. Starts the first walk (and the ticker) once; later calls do nothing. */
	void EnsureStarted();
	bool IsStarted() const { return bStarted; }
	/** Walk every root again on the next step; the current folders stay published until the walk sweeps. */
	void RequestFullRewalk();
	/** Game thread. Reads queued and walked folders within the budget, then publishes when due or forced. */
	void Step(double BudgetSeconds, int32 MaxFolders, bool bForcePublish = false);
	/** Game thread. What the background ticker does each frame: a step, unless ShouldPause says to wait. */
	void TickForTests(float DeltaSeconds) { Tick(DeltaSeconds); }
	/** A first search can stop waiting: a walk has finished, or the registry is scanning or unavailable. */
	bool IsSettled() const;
	TSharedRef<const FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe> GetPublished() const;
	/** Game thread. Removes the ticker and registry subscriptions. */
	void Shutdown();

	/** Registry event handlers. They only queue folders; they never call back into the registry. */
	void NoteAssetsChanged(TConstArrayView<FAssetData> Assets);
	void NoteAssetRenamed(const FAssetData& Asset, const FString& OldObjectPath);

	static bool IsExcludedFolder(FStringView Folder);

private:
	bool Tick(float DeltaSeconds);
	void BeginWalk();
	void Subscribe();
	void VisitFolder(FName Folder, bool bWalkVisit);
	void FinishWalk();
	bool PopPendingFolder(FName& OutFolder);
	void NoteFolderLocked(FName Folder);
	bool IsTruncated() const;
	const TCHAR* ComputeStatus() const;
	void Publish();

	TUniquePtr<IHyperAIProjectAssetSource> Source;
	FLimits Limits;

	bool bStarted = false;
	bool bShuttingDown = false;
	bool bSubscribed = false;
	bool bWalkActive = false;
	bool bHasCompletedWalk = false;
	bool bNeedsFullRewalk = false;
	bool bFolderBoundReached = false;
	bool bDirtySincePublish = false;
	bool bLastKnownScanning = false;
	bool bLastKnownAvailable = true;
	bool bPaused = false;
	TArray<FName> Roots;
	TArray<FName> WalkQueue;
	int32 WalkHead = 0;
	TSet<FName> QueuedThisWalk;
	TSet<FName> VisitedThisWalk;
	TMap<FName, TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>> Folders;
	int32 AssetCount = 0;
	int32 TruncatedFolderCount = 0;
	int64 Generation = 0;
	double LastPublishSeconds = 0.0;
	FString CompletedUtc;
	FString PublishedStatus;

	/** Event handlers may run while a step is reading; the queue has its own lock. */
	mutable FCriticalSection PendingMutex;
	TSet<FName> PendingFolders;
	TArray<FString> RootPrefixes;
	bool bPendingOverflow = false;
	bool bEventsClosed = false;

	FDelegateHandle AddedHandle;
	FDelegateHandle RemovedHandle;
	FDelegateHandle UpdatedHandle;
	FDelegateHandle UpdatedOnDiskHandle;
	FDelegateHandle RenamedHandle;
	FTSTicker::FDelegateHandle TickerHandle;

	mutable FCriticalSection PublishMutex;
	TSharedRef<const FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe> Published;
};
