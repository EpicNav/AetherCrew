// Copyright Epic Games, Inc. All Rights Reserved.

#include "StatsToolset.h"

#include "Async/Async.h"
#include "AsyncFunctionResult/AsyncFunctionResultUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#if WITH_EDITOR
#include "Editor.h"
#endif
#include "HAL/PlatformTime.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/CoreMiscDefines.h"
#include "PerfToolsetUtils.h"
#include "RHI.h"
#include "RenderTimer.h"
#include "Stats/Stats.h"
#include "Stats/StatsData.h"
#include "Misc/Optional.h"
#include "Templates/Function.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StatsToolset)

TSharedRef<FJsonObject> UStatsFrameTreeResult::MakeJsonSchema() const
{
	TSharedRef<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));

	TSharedRef<FJsonObject> InclusiveProp = MakeShared<FJsonObject>();
	InclusiveProp->SetStringField(TEXT("type"), TEXT("number"));

	TSharedRef<FJsonObject> ExclusiveProp = MakeShared<FJsonObject>();
	ExclusiveProp->SetStringField(TEXT("type"), TEXT("number"));

	TSharedRef<FJsonObject> NodeRef = MakeShared<FJsonObject>();
	NodeRef->SetStringField(TEXT("$ref"), TEXT("#/definitions/Node"));

	TSharedRef<FJsonObject> ChildrenProp = MakeShared<FJsonObject>();
	ChildrenProp->SetStringField(TEXT("type"), TEXT("array"));
	ChildrenProp->SetObjectField(TEXT("items"), NodeRef);

	TSharedRef<FJsonObject> TruncatedProp = MakeShared<FJsonObject>();
	TruncatedProp->SetStringField(TEXT("type"), TEXT("boolean"));

	TSharedRef<FJsonObject> NodeProperties = MakeShared<FJsonObject>();
	NodeProperties->SetObjectField(TEXT("Name"), NameProp);
	NodeProperties->SetObjectField(TEXT("InclusiveMs"), InclusiveProp);
	NodeProperties->SetObjectField(TEXT("ExclusiveMs"), ExclusiveProp);
	NodeProperties->SetObjectField(TEXT("Children"), ChildrenProp);
	NodeProperties->SetObjectField(TEXT("Truncated"), TruncatedProp);

	TArray<TSharedPtr<FJsonValue>> NodeRequired;
	NodeRequired.Add(MakeShared<FJsonValueString>(TEXT("Name")));
	NodeRequired.Add(MakeShared<FJsonValueString>(TEXT("InclusiveMs")));
	NodeRequired.Add(MakeShared<FJsonValueString>(TEXT("ExclusiveMs")));

	TSharedRef<FJsonObject> NodeSchema = MakeShared<FJsonObject>();
	NodeSchema->SetStringField(TEXT("type"), TEXT("object"));
	NodeSchema->SetObjectField(TEXT("properties"), NodeProperties);
	NodeSchema->SetArrayField(TEXT("required"), NodeRequired);

	TSharedRef<FJsonObject> Definitions = MakeShared<FJsonObject>();
	Definitions->SetObjectField(TEXT("Node"), NodeSchema);

	TSharedRef<FJsonObject> RootItemRef = MakeShared<FJsonObject>();
	RootItemRef->SetStringField(TEXT("$ref"), TEXT("#/definitions/Node"));

	TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("array"));
	Schema->SetObjectField(TEXT("items"), RootItemRef);
	Schema->SetObjectField(TEXT("definitions"), Definitions);
	return Schema;
}

TSharedRef<FJsonObject> UStatCountersResult::MakeJsonSchema() const
{
	TSharedRef<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));

	TSharedRef<FJsonObject> ValueProp = MakeShared<FJsonObject>();
	ValueProp->SetStringField(TEXT("type"), TEXT("number"));

	TArray<TSharedPtr<FJsonValue>> UnitEnum;
	UnitEnum.Add(MakeShared<FJsonValueString>(TEXT("count")));
	UnitEnum.Add(MakeShared<FJsonValueString>(TEXT("bytes")));
	UnitEnum.Add(MakeShared<FJsonValueString>(TEXT("float")));

	TSharedRef<FJsonObject> UnitProp = MakeShared<FJsonObject>();
	UnitProp->SetStringField(TEXT("type"), TEXT("string"));
	UnitProp->SetArrayField(TEXT("enum"), UnitEnum);

	TSharedRef<FJsonObject> RowProperties = MakeShared<FJsonObject>();
	RowProperties->SetObjectField(TEXT("Name"), NameProp);
	RowProperties->SetObjectField(TEXT("Value"), ValueProp);
	RowProperties->SetObjectField(TEXT("Unit"), UnitProp);

	TArray<TSharedPtr<FJsonValue>> RowRequired;
	RowRequired.Add(MakeShared<FJsonValueString>(TEXT("Name")));
	RowRequired.Add(MakeShared<FJsonValueString>(TEXT("Value")));
	RowRequired.Add(MakeShared<FJsonValueString>(TEXT("Unit")));

	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("type"), TEXT("object"));
	Row->SetObjectField(TEXT("properties"), RowProperties);
	Row->SetArrayField(TEXT("required"), RowRequired);

	TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("array"));
	Schema->SetObjectField(TEXT("items"), Row);
	return Schema;
}

namespace
{
	using namespace UE::PerfToolset;

#if STATS
	// The synthetic child injected by FRawStatStackNode::AddSelf carries the
	// exclusive (self) time for its parent. We split it out at conversion
	// time so it doesn't appear as a regular descendant in the emitted
	// tree, and surface its value as the parent's "ExclusiveMs" field.
	const FName NAME_SelfChild = NAME_Self;

	struct FChild
	{
		const FRawStatStackNode* Node;
		double InclusiveMs;
	};

	// Collects Node's children sorted by descending InclusiveMs, skipping
	// the synthetic NAME_Self entry and dropping any child below the
	// threshold.
	TArray<FChild> GatherSurvivingChildren(const FRawStatStackNode& Node, float CullInclusiveMs)
	{
		TArray<FChild> Out;
		Out.Reserve(Node.Children.Num());

		for (TMap<FName, FRawStatStackNode*>::TConstIterator It(Node.Children); It; ++It)
		{
			if (It.Key() == NAME_SelfChild)
			{
				continue;
			}
			const double Inclusive = FPlatformTime::ToMilliseconds64(
				static_cast<uint64>(It.Value()->Meta.GetValue_Duration()));
			if (Inclusive < CullInclusiveMs)
			{
				continue;
			}
			Out.Add({ It.Value(), Inclusive });
		}
		Out.Sort([](const FChild& A, const FChild& B) { return A.InclusiveMs > B.InclusiveMs; });
		return Out;
	}

	// Emits Name/InclusiveMs/ExclusiveMs for a single node. AddSelf injects a
	// NAME_Self synthetic child on non-leaf nodes carrying the parent's
	// exclusive time; true leaves have no such child, but self == inclusive
	// there by definition, so we can pull ExclusiveMs uniformly.
	TSharedRef<FJsonObject> BuildNodeFields(const FRawStatStackNode& Node)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("Name"), Node.Meta.NameAndInfo.GetShortName().ToString());
		Out->SetField(TEXT("InclusiveMs"), MakeMsValue(FPlatformTime::ToMilliseconds64(
			static_cast<uint64>(Node.Meta.GetValue_Duration()))));

		const FRawStatStackNode* Self = Node.Children.FindRef(NAME_SelfChild);
		const int64 ExclusiveCycles = Self
			? Self->Meta.GetValue_Duration()
			: Node.Meta.GetValue_Duration();
		Out->SetField(TEXT("ExclusiveMs"), MakeMsValue(FPlatformTime::ToMilliseconds64(
			static_cast<uint64>(ExclusiveCycles))));
		return Out;
	}

	// Emits Node and every descendant, bounded by MinInclusiveMs and
	// RemainingDepth. When depth runs out with surviving children below,
	// "Truncated": true marks the boundary.
	TSharedRef<FJsonObject> EmitFullSubtree(const FRawStatStackNode& Node, float CullInclusiveMs, int32 RemainingDepth)
	{
		TSharedRef<FJsonObject> Out = BuildNodeFields(Node);

		const TArray<FChild> SortedChildren = GatherSurvivingChildren(Node, CullInclusiveMs);
		if (!SortedChildren.IsEmpty())
		{
			if (RemainingDepth > 1)
			{
				TArray<TSharedPtr<FJsonValue>> ChildrenArr;
				ChildrenArr.Reserve(SortedChildren.Num());
				for (const FChild& Child : SortedChildren)
				{
					ChildrenArr.Add(MakeShared<FJsonValueObject>(EmitFullSubtree(*Child.Node, CullInclusiveMs, RemainingDepth - 1)));
				}
				Out->SetArrayField(TEXT("Children"), ChildrenArr);
			}
			else
			{
				Out->SetBoolField(TEXT("Truncated"), true);
			}
		}

		return Out;
	}

	// Searches under Node for descendants whose full dotted path contains
	// PathFilter. On a self-match, the whole subtree is emitted. If a
	// descendant matches, Node is returned wrapping the matched children as
	// structural context. Returns null when no match is found under Node.
	// An empty PathFilter self-matches immediately, degenerating to a full
	// EmitFullSubtree of every root.
	TSharedPtr<FJsonObject> SearchAndEmit(const FRawStatStackNode& Node, const FString& ParentPath, float CullInclusiveMs, int32 RemainingDepth, const FString& PathFilter)
	{
		const FString NodeName = Node.Meta.NameAndInfo.GetShortName().ToString();
		const FString FullPath = ParentPath.IsEmpty() ? NodeName : ParentPath + TEXT(".") + NodeName;

		if (PathFilter.IsEmpty() || FullPath.Contains(PathFilter, ESearchCase::IgnoreCase))
		{
			return EmitFullSubtree(Node, CullInclusiveMs, RemainingDepth);
		}
		if (RemainingDepth <= 1)
		{
			return nullptr;
		}

		TArray<TSharedPtr<FJsonValue>> ChildrenArr;
		for (const FChild& Child : GatherSurvivingChildren(Node, CullInclusiveMs))
		{
			if (TSharedPtr<FJsonObject> Built = SearchAndEmit(*Child.Node, FullPath, CullInclusiveMs, RemainingDepth - 1, PathFilter))
			{
				ChildrenArr.Add(MakeShared<FJsonValueObject>(Built.ToSharedRef()));
			}
		}
		if (ChildrenArr.IsEmpty())
		{
			return nullptr;
		}

		TSharedRef<FJsonObject> Out = BuildNodeFields(Node);
		Out->SetArrayField(TEXT("Children"), ChildrenArr);
		return Out;
	}

	TSharedRef<FJsonValue> BuildFrameJson(const FRawStatStackNode& FrameTree, float CullInclusiveMs, int32 MaxDepth, const FString& PathFilter)
	{
		const TArray<FChild> SortedRoots = GatherSurvivingChildren(FrameTree, CullInclusiveMs);
		TArray<TSharedPtr<FJsonValue>> RootArr;
		RootArr.Reserve(SortedRoots.Num());
		for (const FChild& Root : SortedRoots)
		{
			if (TSharedPtr<FJsonObject> Built = SearchAndEmit(*Root.Node, FString(), CullInclusiveMs, MaxDepth, PathFilter))
			{
				RootArr.Add(MakeShared<FJsonValueObject>(Built.ToSharedRef()));
			}
		}
		return MakeShared<FJsonValueArray>(MoveTemp(RootArr));
	}

	enum class ECounterUnitOrder : int32
	{
		Bytes = 0,
		Count = 1,
		Float = 2,
	};

	struct FCounterRow
	{
		ECounterUnitOrder UnitOrder;
		double PerUnitSort;
		TSharedPtr<FJsonObject> Object;
	};

	// Extracts one counter stat as a completed JSON object plus sort keys.
	// Returns an empty TOptional for stats that don't represent a numeric
	// counter, or when the value is zero and zeros are being dropped.
	TOptional<FCounterRow> ExtractCounterRow(const FStatMessage& Msg, bool bIncludeZero)
	{
		const EStatDataType::Type Type = Msg.NameAndInfo.GetField<EStatDataType>();
		const bool bIsMemory = Msg.NameAndInfo.GetFlag(EStatMetaFlags::IsMemory);

		FCounterRow Row;
		
		if (Type == EStatDataType::ST_int64)
		{
			const int64 V = Msg.GetValue_int64();
			if (V == 0 && !bIncludeZero)
			{
				return {};
			}
			Row.PerUnitSort = static_cast<double>(V);
			Row.UnitOrder = bIsMemory ? ECounterUnitOrder::Bytes : ECounterUnitOrder::Count;
			Row.Object = MakeShared<FJsonObject>();
			Row.Object->SetField(TEXT("Value"), MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), V)));
			Row.Object->SetStringField(TEXT("Unit"), bIsMemory ? TEXT("bytes") : TEXT("count"));
		}
		else if (Type == EStatDataType::ST_double)
		{
			const double V = Msg.GetValue_double();
			if (V == 0.0 && !bIncludeZero)
			{
				return {};
			}
			Row.PerUnitSort = V;
			Row.UnitOrder = ECounterUnitOrder::Float;
			Row.Object = MakeShared<FJsonObject>();
			Row.Object->SetField(TEXT("Value"), MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%.4g"), V)));
			Row.Object->SetStringField(TEXT("Unit"), TEXT("float"));
		}
		else
		{
			return {};
		}

		Row.Object->SetStringField(TEXT("Name"), Msg.NameAndInfo.GetShortName().ToString());
		return Row;
	}

	TSharedRef<FJsonValue> BuildCountersJson(const TArray<FStatMessage>& CounterStats, FName GroupName, bool bIncludeZero)
	{
		TArray<FCounterRow> Pending;
		for (const FStatMessage& Msg : CounterStats)
		{
			if (Msg.NameAndInfo.GetGroupName() != GroupName)
			{
				continue;
			}
			if (TOptional<FCounterRow> Row = ExtractCounterRow(Msg, bIncludeZero))
			{
				Pending.Add(MoveTemp(Row.GetValue()));
			}
		}

		Pending.Sort([](const FCounterRow& A, const FCounterRow& B)
		{
			if (A.UnitOrder != B.UnitOrder)
			{
				return A.UnitOrder < B.UnitOrder;
			}
			return A.PerUnitSort > B.PerUnitSort;
		});

		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Pending.Num());
		for (const FCounterRow& P : Pending)
		{
			Rows.Add(MakeShared<FJsonValueObject>(P.Object));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Rows));
	}

	// Number of rendered frames to wait through before subscribing to the
	// stats pipe. Serves two purposes:
	//   1. An agent driving this tool can briefly stall the game thread
	//      around the call (waiting on an LLM response, blocking IO in
	//      the assistant UI, etc.); the defer gives the game thread time
	//      to resume normal ticking so the sample reflects real gameplay
	//      rather than the tail of the stall.
	//   2. When a stat group was just enabled by us, off-thread emitters
	//      (physics task workers, async loaders, audio thread) need a
	//      few frames of headroom before their SET_DWORD_STAT calls land
	//      in the stats pipe. Six frames is ~100ms at 60fps, enough for
	//      multiple physics ticks and async task completions to flush.
	constexpr int32 StatsDeferRenderedFrames = 6;


	struct FQueryState
	{
		using FJsonBuilder = TFunction<TSharedPtr<FJsonValue>(const FStatsThreadState&, int64 Frame)>;
		using FCompletion = TFunction<void(TSharedPtr<FJsonValue>)>;

		const FStatsThreadState& Stats;
		FJsonBuilder JsonBuilder;
		FCompletion OnDone;
		int32 FramesUntilSubscribe;
		FDelegateHandle EndFrameHandle;
		FDelegateHandle NewFrameHandle;
		FName GroupToDisableOnDestroy;
		bool bFired = false;

		FQueryState(FJsonBuilder&& InJsonBuilder, FCompletion&& InOnDone, FName InGroupToDisableOnDestroy = NAME_None)
			: Stats(FStatsThreadState::GetLocalState())
			, JsonBuilder(MoveTemp(InJsonBuilder))
			, OnDone(MoveTemp(InOnDone))
			, FramesUntilSubscribe(StatsDeferRenderedFrames)
			, GroupToDisableOnDestroy(InGroupToDisableOnDestroy)
		{
			StatsPrimaryEnableAdd();
			EndFrameHandle = FCoreDelegates::OnEndFrame.AddRaw(this, &FQueryState::OnEndFrame);
		}

		~FQueryState()
		{
			if (EndFrameHandle.IsValid())
			{
				FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
			}
			if (NewFrameHandle.IsValid())
			{
				Stats.NewFrameDelegate.Remove(NewFrameHandle);
			}
			if (!GroupToDisableOnDestroy.IsNone())
			{
				IStatGroupEnableManager::Get().SetHighPerformanceEnableForGroup(GroupToDisableOnDestroy, false);
			}
			StatsPrimaryEnableSubtract();
		}

		// Fired on the game thread at end-of-frame. Kicks a level editor
		// viewport redraw each frame so the sampled stats frame reflects
		// real viewport work, not a Slate-only tick driven by the agent
		// UI panel invalidating itself. Counts down, then subscribes to
		// the stats pipe — but stays subscribed to OnEndFrame so the
		// viewport keeps being kicked while we wait for the stats thread
		// to deliver a completed frame (which may be several game-thread
		// ticks later). Both delegates are torn down in the destructor.
		void OnEndFrame()
		{
#if WITH_EDITOR
			if (GEditor)
			{
				GEditor->RedrawLevelEditingViewports(/*bInvalidateHitProxies=*/false);
			}
#endif

			if (NewFrameHandle.IsValid())
			{
				return;
			}
			if (--FramesUntilSubscribe > 0)
			{
				return;
			}
			NewFrameHandle = Stats.NewFrameDelegate.AddRaw(this, &FQueryState::OnNewFrame);
		}

		// Fired on the stats pipe once a complete frame is available.
		void OnNewFrame(int64 Frame)
		{
			if (bFired)
			{
				return;
			}
			bFired = true;

			Stats.NewFrameDelegate.Remove(NewFrameHandle);
			NewFrameHandle = FDelegateHandle();

			TSharedPtr<FJsonValue> Value = JsonBuilder(Stats, Frame);

			AsyncTask(ENamedThreads::GameThread,
				[OnDone = MoveTemp(OnDone), Value = MoveTemp(Value), Self = this]() mutable
				{
					OnDone(MoveTemp(Value));
					delete Self;
				});
		}
	};
#endif // STATS
}

FFrameTimings UStatsToolset::GetFrameTimings()
{
	FFrameTimings Out;
	Out.FrameMs        = static_cast<float>((FApp::GetCurrentTime() - FApp::GetLastTime()) * 1000.0);
	Out.GameThreadMs   = FPlatformTime::ToMilliseconds(GGameThreadTime);
	Out.RenderThreadMs = FPlatformTime::ToMilliseconds(GRenderThreadTime);
	Out.RhiThreadMs    = FPlatformTime::ToMilliseconds(GRHIThreadTime);
	Out.GpuMs          = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
	return Out;
}

UStatsFrameTreeResult* UStatsToolset::GetFrameTree(float MinInclusiveMs, int32 MaxDepth, const FString& PathFilter)
{
#if STATS
	UStatsFrameTreeResult* Result = NewObject<UStatsFrameTreeResult>();
	TStrongObjectPtr<UStatsFrameTreeResult> StrongResult(Result);
	const float ClampedMinMs = FMath::Max(0.f, MinInclusiveMs);
	// Each stat level adds two JSON containers (node object plus Children
	// array); orjson refuses values nested past 254, so 127 stat levels is
	// the hard ceiling. Cap the request comfortably below it.
	const int32 ClampedMaxDepth = FMath::Clamp(MaxDepth, 1, 120);

	// Self-managing: enables stats collection, defers by a few rendered
	// frames, subscribes to NewFrameDelegate for the next completed frame,
	// then deletes itself after firing the callback.
	new FQueryState(
		[ClampedMinMs, ClampedMaxDepth, PathFilter](const FStatsThreadState& Stats, int64 Frame) -> TSharedPtr<FJsonValue>
		{
			FRawStatStackNode FrameTree;
			Stats.UncondenseStackStats(Frame, FrameTree);
			FrameTree.AddSelf();
			return BuildFrameJson(FrameTree, ClampedMinMs, ClampedMaxDepth, PathFilter);
		},
		[StrongResult](TSharedPtr<FJsonValue> Tree) mutable
		{
			StrongResult->SetValue(MoveTemp(Tree));
			StrongResult.Reset();
		});

	return Result;
#else
	using namespace UE::AsyncFunctionResult;

	(void)MinInclusiveMs;
	(void)MaxDepth;
	(void)PathFilter;
	return MakeErrorResult<UStatsFrameTreeResult>(TEXT("Frame tree capture requires the STATS system, which is not compiled in this build."));
#endif // STATS
}

TArray<FString> UStatsToolset::ListStatGroups()
{
	TArray<FString> Out;
#if STATS
	const TSet<FName>& StatGroupNames = FStatGroupGameThreadNotifier::Get().StatGroupNames;
	Out.Reserve(StatGroupNames.Num());
	for (const FName& GroupName : StatGroupNames)
	{
		Out.Add(GroupName.ToString());
	}
	Out.Sort();
#else
	UKismetSystemLibrary::RaiseScriptError(TEXT("Stat group enumeration requires the STATS system, which is not compiled in this build."));
#endif
	return Out;
}

UStatCountersResult* UStatsToolset::GetStatCounters(const FString& GroupName, bool bIncludeZero)
{
#if STATS
	UStatCountersResult* Result = NewObject<UStatCountersResult>();
	TStrongObjectPtr<UStatCountersResult> StrongResult(Result);
	const FName GroupFName(*GroupName);
	// Enable the group so stats registered against it start emitting.
	// The defer window before FQueryState subscribes gives newly enabled
	// stats time to land in the pipe. FQueryState turns the group back off
	// in its destructor after the sample so we don't leak collection cost.
	// A caller who had manually toggled the group on via `stat GroupName`
	// will see it flipped off after this call and can re-toggle if needed.
	IStatGroupEnableManager::Get().SetHighPerformanceEnableForGroup(GroupFName, true);

	new FQueryState(
		[GroupFName, bIncludeZero](const FStatsThreadState& Stats, int64 Frame) -> TSharedPtr<FJsonValue>
		{
			// Physics/task-worker counters use SET_DWORD_STAT (ClearEveryFrame),
			// so a stat only appears in a specific frame's condensed data if it
			// emitted THAT frame. If our sample lands on a frame between physics
			// ticks we miss the value entirely. Walk backwards through recent
			// history and take the newest value seen per short-name -- gives us
			// "each counter's most recent known value within the window", which
			// matches what `stat GroupName` displays on-screen.
			constexpr int32 CounterHistoryFrames = 8;

			TArray<FStatMessage> CounterStats;
			TSet<FName> Seen;
			for (int32 Offset = 0; Offset < CounterHistoryFrames; ++Offset)
			{
				const int64 FrameIdx = Frame - Offset;
				if (!Stats.IsFrameValid(FrameIdx))
				{
					break;
				}
				FRawStatStackNode Unused;
				TArray<FStatMessage> FrameStats;
				Stats.UncondenseStackStats(FrameIdx, Unused, nullptr, &FrameStats);
				for (const FStatMessage& Msg : FrameStats)
				{
					if (Msg.NameAndInfo.GetGroupName() != GroupFName)
					{
						continue;
					}
					bool bAlreadyIn = false;
					Seen.Add(Msg.NameAndInfo.GetShortName(), &bAlreadyIn);
					if (!bAlreadyIn)
					{
						CounterStats.Add(Msg);
					}
				}
			}

			// Fallback for anything still missing: non-cleared accumulator
			// stats (memory pools, one-time-set counters) live in
			// NotClearedEveryFrame and persist across frames independent of
			// the sample window.
			for (const TPair<FName, FStatMessage>& Pair : Stats.NotClearedEveryFrame)
			{
				const FStatMessage& Msg = Pair.Value;
				if (Msg.NameAndInfo.GetGroupName() != GroupFName)
				{
					continue;
				}
				bool bAlreadyIn = false;
				Seen.Add(Msg.NameAndInfo.GetShortName(), &bAlreadyIn);
				if (!bAlreadyIn)
				{
					CounterStats.Add(Msg);
				}
			}

			return BuildCountersJson(CounterStats, GroupFName, bIncludeZero);
		},
		[StrongResult](TSharedPtr<FJsonValue> Rows) mutable
		{
			StrongResult->SetValue(MoveTemp(Rows));
			StrongResult.Reset();
		},
		GroupFName);

	return Result;
#else
	using namespace UE::AsyncFunctionResult;

	(void)GroupName;
	(void)bIncludeZero;
	return MakeErrorResult<UStatCountersResult>(TEXT("Stat counter sampling requires the STATS system, which is not compiled in this build."));
#endif // STATS
}
