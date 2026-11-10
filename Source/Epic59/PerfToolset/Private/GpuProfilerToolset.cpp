// Copyright Epic Games, Inc. All Rights Reserved.

#include "GpuProfilerToolset.h"

#include "Async/Async.h"
#include "AsyncFunctionResult/AsyncFunctionResultUtils.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#if WITH_EDITOR
#include "Editor.h"
#endif
#include "GPUProfiler.h"
#include "HAL/IConsoleManager.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CoreDelegates.h"
#include "PerfToolsetUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Templates/SharedPointer.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GpuProfilerToolset)

// UE 5.8: 5.8's RHI has no UE::RHI::GPUProfiler::SetProfileCaptureCallback or ProfileNextFrame, so a frame
// cannot be captured as JSON; TriggerGPUProfile reports that instead of capturing.
#define UE58_GPU_PROFILE_CAPTURE 0

TSharedRef<FJsonObject> UGpuProfileResult::MakeJsonSchema() const
{
	TSharedRef<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));

	TSharedRef<FJsonObject> InclusiveProp = MakeShared<FJsonObject>();
	InclusiveProp->SetStringField(TEXT("type"), TEXT("number"));

	TSharedRef<FJsonObject> NodeRef = MakeShared<FJsonObject>();
	NodeRef->SetStringField(TEXT("$ref"), TEXT("#/definitions/Node"));

	TSharedRef<FJsonObject> ChildrenProp = MakeShared<FJsonObject>();
	ChildrenProp->SetStringField(TEXT("type"), TEXT("array"));
	ChildrenProp->SetObjectField(TEXT("items"), NodeRef);

	TSharedRef<FJsonObject> NodeProperties = MakeShared<FJsonObject>();
	NodeProperties->SetObjectField(TEXT("Name"), NameProp);
	NodeProperties->SetObjectField(TEXT("InclusiveMs"), InclusiveProp);
	NodeProperties->SetObjectField(TEXT("Children"), ChildrenProp);

	TArray<TSharedPtr<FJsonValue>> NodeRequired;
	NodeRequired.Add(MakeShared<FJsonValueString>(TEXT("Name")));
	NodeRequired.Add(MakeShared<FJsonValueString>(TEXT("InclusiveMs")));

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

TSharedPtr<FJsonValue> UGpuProfilerToolset::BuildGpuProfileJsonTree(
	const FString& SourceJson, float MinInclusiveMs)
{
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SourceJson);
	TArray<TSharedPtr<FJsonValue>> SourceEntries;
	if (!FJsonSerializer::Deserialize(Reader, SourceEntries))
	{
		return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
	}

	// First: linearize the surviving entries (post-cull) as (Depth, Name,
	// InclusiveMs). Separating this from the nesting pass keeps both
	// passes simple — the nesting walk uses "next entry's Depth > current"
	// to decide descent, which would entangle with culled-subtree skipping
	// if combined.
	struct FSurvivor
	{
		int32 Depth;
		FString Name;
		float InclusiveMs;
	};
	TArray<FSurvivor> Survivors;
	Survivors.Reserve(SourceEntries.Num());

	int32 CullingDepth = -1;
	for (const TSharedPtr<FJsonValue>& SourceEntry : SourceEntries)
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!SourceEntry.IsValid() || !SourceEntry->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
		{
			continue;
		}
		const FJsonObject& Obj = **ObjPtr;

		int32 Depth = 0;
		Obj.TryGetNumberField(TEXT("Depth"), Depth);

		if (CullingDepth >= 0)
		{
			if (Depth > CullingDepth)
			{
				continue;
			}
			CullingDepth = -1;
		}

		double DurationMs = 0.0;
		Obj.TryGetNumberField(TEXT("GPUDurationMs"), DurationMs);

		if (Depth >= 1 && DurationMs < MinInclusiveMs)
		{
			CullingDepth = Depth;
			continue;
		}

		// Queue roots come in with whatever EventName the renderer set on
		// the root node — sometimes uninformative — but the Queue field
		// is always the queue's display name. Use Queue as Name for
		// Depth==0 so each queue's block has an obvious anchor.
		FString Name;
		if (Depth == 0)
		{
			Obj.TryGetStringField(TEXT("Queue"), Name);
		}
		else
		{
			Obj.TryGetStringField(TEXT("EventName"), Name);
		}

		Survivors.Add({ Depth, MoveTemp(Name), static_cast<float>(DurationMs) });
	}

	// Second: re-shape the flat survivor list into a nested JSON tree by
	// walking left-to-right with an explicit stack of open ancestors.
	// A new entry at depth D closes every stack entry with depth >= D
	// (those branches can't gain more children), then becomes the
	// deepest open node. Closing a node attaches its accumulated children
	// to its JSON object and appends the object to its parent's child
	// list — or to Roots if it was at depth 0.
	struct FOpenNode
	{
		TSharedRef<FJsonObject> Object;
		TArray<TSharedPtr<FJsonValue>> Children;
		int32 Depth;
	};

	TArray<TSharedPtr<FJsonValue>> Roots;
	TArray<FOpenNode> Stack;

	auto CloseTop = [&Stack, &Roots]()
	{
		FOpenNode Closed = MoveTemp(Stack.Last());
		Stack.Pop();
		// Omit the key entirely for leaves so the format reflects "no
		// children" structurally rather than as an empty array — saves
		// a few tokens per leaf, which dominate typical captures.
		if (!Closed.Children.IsEmpty())
		{
			Closed.Object->SetArrayField(TEXT("Children"), Closed.Children);
		}
		TArray<TSharedPtr<FJsonValue>>& Parent = Stack.IsEmpty() ? Roots : Stack.Last().Children;
		Parent.Add(MakeShared<FJsonValueObject>(Closed.Object));
	};

	for (const FSurvivor& Entry : Survivors)
	{
		while (!Stack.IsEmpty() && Stack.Last().Depth >= Entry.Depth)
		{
			CloseTop();
		}

		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("Name"), Entry.Name);
		Object->SetField(TEXT("InclusiveMs"), UE::PerfToolset::MakeMsValue(Entry.InclusiveMs));
		Stack.Add(FOpenNode{ Object, {}, Entry.Depth });
	}

	while (!Stack.IsEmpty())
	{
		CloseTop();
	}

	return MakeShared<FJsonValueArray>(MoveTemp(Roots));
}

namespace
{
	// Scope guard that restores a console variable's int value when destroyed.
	// Used to suppress the ProfileGPU visualizer pop-up for the duration of an
	// agent-driven capture (r.ProfileGPU.ShowUI = 0) without permanently
	// changing the user's setting. Held by TSharedRef across the engine's
	// capture callback so the restore fires whether the callback runs,
	// gets replaced by a subsequent trigger, or never fires at all (e.g.
	// no Graphics queue available).
	//
	// The destructor defers the IConsoleVariable::Set call to the game thread
	// because the guard can be destroyed on whichever thread last drops its
	// shared ref — the RHI invokes our capture callback on a worker thread,
	// so the most common destruction site is off-thread, and CVar mutation
	// from a non-game thread isn't allowed.
	struct FConsoleVariableIntRestoreGuard
	{
		IConsoleVariable* CVar = nullptr;
		int32 SavedValue = 0;

		~FConsoleVariableIntRestoreGuard()
		{
			if (CVar)
			{
				IConsoleVariable* LocalCVar = CVar;
				const int32 LocalSavedValue = SavedValue;
				AsyncTask(ENamedThreads::GameThread,
					[LocalCVar, LocalSavedValue]()
					{
						LocalCVar->Set(*FString::FromInt(LocalSavedValue), ECVF_SetByCode);
					});
			}
		}
	};

#if WITH_PROFILEGPU && UE58_GPU_PROFILE_CAPTURE // UE 5.8: see UE58_GPU_PROFILE_CAPTURE
	// Number of rendered frames to wait before arming the next-frame capture.
	// An agent driving this tool can stall the game thread around the call
	// (LLM response, blocking IO in the assistant UI, etc.); deferring past
	// the stall means the captured frame reflects real gameplay rather than
	// the idle editor-only swap the renderer would otherwise flush.
	constexpr int32 GpuDeferRenderedFrames = 2;

	// Per-query lifetime: counts rendered frames on the game thread, arms a
	// one-shot RHI capture, resolves the async result. Self-managing —
	// deletes itself once the result is filled in.
	struct FGpuQueryState
	{
		TStrongObjectPtr<UGpuProfileResult> StrongResult;
		TSharedRef<FConsoleVariableIntRestoreGuard> ShowUIGuard;
		float MinInclusiveMs;
		int32 FramesUntilArm;
		FDelegateHandle EndFrameHandle;

		FGpuQueryState(TStrongObjectPtr<UGpuProfileResult> InResult,
			TSharedRef<FConsoleVariableIntRestoreGuard> InGuard, float InMinInclusiveMs)
			: StrongResult(MoveTemp(InResult))
			, ShowUIGuard(MoveTemp(InGuard))
			, MinInclusiveMs(InMinInclusiveMs)
			, FramesUntilArm(GpuDeferRenderedFrames)
		{
			EndFrameHandle = FCoreDelegates::OnEndFrame.AddRaw(this, &FGpuQueryState::OnEndFrame);
		}

		~FGpuQueryState()
		{
			if (EndFrameHandle.IsValid())
			{
				FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
			}
		}

		// Fired on the game thread at the end of each rendered frame. Counts
		// down then arms the RHI's next-frame capture; we unsubscribe before
		// arming so the delegate isn't re-entered while the capture is in
		// flight.
		//
		// Also kicks a redraw of the level editor viewports on every tick.
		// The agent UI panel keeps Slate busy invalidating itself, but the
		// level viewport's redraw isn't automatically scheduled in that
		// state — without this, the captured frame is reliably Slate-only
		// (no BasePass / scene render) even when realtime is enabled and
		// the viewport was animating normally before the agent's request.
		// This mirrors what happens when a user manually types `ProfileGPU`
		// in the console: the viewport has focus and is being repainted.
		void OnEndFrame()
		{
#if WITH_EDITOR
			if (GEditor)
			{
				GEditor->RedrawLevelEditingViewports(/*bInvalidateHitProxies=*/false);
			}
#endif

			if (--FramesUntilArm > 0)
			{
				return;
			}

			FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
			EndFrameHandle = FDelegateHandle();

			UE::RHI::GPUProfiler::SetProfileCaptureCallback(
				[this](FString&& Json) mutable
				{
					OnCaptureComplete(MoveTemp(Json));
				});
			UE::RHI::GPUProfiler::ProfileNextFrame();
		}

		// Fires on a worker thread when the RHI's capture finishes. Bounces
		// to the game thread to resolve the async result and tear down.
		void OnCaptureComplete(FString&& Json)
		{
			TSharedPtr<FJsonValue> Tree = UGpuProfilerToolset::BuildGpuProfileJsonTree(Json, MinInclusiveMs);

			AsyncTask(ENamedThreads::GameThread,
				[this, Tree = MoveTemp(Tree)]() mutable
				{
					StrongResult->SetValue(MoveTemp(Tree));
					StrongResult.Reset();
					delete this;
				});
		}
	};
#endif // WITH_PROFILEGPU
}

UGpuProfileResult* UGpuProfilerToolset::TriggerGPUProfile(float MinInclusiveMs)
{
#if !UE58_GPU_PROFILE_CAPTURE // UE 5.8: no JSON frame capture in 5.8's RHI
	return UE::AsyncFunctionResult::MakeErrorResult<UGpuProfileResult>(
		TEXT("TriggerGPUProfile needs the GPU profiler capture API of UE 5.9; on UE 5.8 run the ProfileGPU console command and read the log instead."));
#elif WITH_PROFILEGPU
	UGpuProfileResult* Result = NewObject<UGpuProfileResult>();
	TStrongObjectPtr<UGpuProfileResult> StrongResult(Result);

	// Suppress the ProfileGPU visualizer pop-up for our capture. The CVar is
	// restored when the guard's last reference dies — held by the query state
	// below, so it survives across deferred frames and retries.
	TSharedRef<FConsoleVariableIntRestoreGuard> ShowUIGuard = MakeShared<FConsoleVariableIntRestoreGuard>();
	if (IConsoleVariable* ShowUICVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ProfileGPU.ShowUI")))
	{
		ShowUIGuard->CVar = ShowUICVar;
		ShowUIGuard->SavedValue = ShowUICVar->GetInt();
		ShowUICVar->Set(TEXT("0"), ECVF_SetByCode);
	}

	// Self-managing: subscribes to OnEndFrame, defers arming the RHI capture
	// by a few rendered frames, then deletes itself once the async result
	// is filled in.
	new FGpuQueryState(MoveTemp(StrongResult), MoveTemp(ShowUIGuard), FMath::Max(0.f, MinInclusiveMs));

	return Result;
#else
	using namespace UE::AsyncFunctionResult;

	// Preserve the async-result contract: resolve on the next game-thread
	// tick rather than synchronously inside this call.
	return RunOnNextTick<UGpuProfileResult>(
		[](const TStrongObjectPtr<UGpuProfileResult>& StrongResult)
		{
			StrongResult->SetValue(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>()));
		});
#endif // WITH_PROFILEGPU
}
