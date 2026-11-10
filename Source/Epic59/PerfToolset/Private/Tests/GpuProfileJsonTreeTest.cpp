// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GpuProfilerToolset.h"
#include "Misc/AutomationTest.h"

BEGIN_DEFINE_SPEC(FBuildGpuProfileJsonTreeSpec,
	"AIAssistant.Toolsets.PerfToolset.BuildGpuProfileJsonTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	TSharedPtr<FJsonObject> AsObject(const TSharedPtr<FJsonValue>& Value)
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		return (Value.IsValid() && Value->TryGetObject(ObjPtr)) ? *ObjPtr : nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> AsArray(const TSharedPtr<FJsonValue>& Value)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		return (Value.IsValid() && Value->TryGetArray(Arr)) ? *Arr : TArray<TSharedPtr<FJsonValue>>{};
	}

	TArray<TSharedPtr<FJsonValue>> Children(const TSharedPtr<FJsonObject>& Obj)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		return (Obj.IsValid() && Obj->TryGetArrayField(TEXT("Children"), Arr)) ? *Arr : TArray<TSharedPtr<FJsonValue>>{};
	}

	FString NameOf(const TSharedPtr<FJsonValue>& Value)
	{
		TSharedPtr<FJsonObject> Obj = AsObject(Value);
		FString Out;
		if (Obj.IsValid())
		{
			Obj->TryGetStringField(TEXT("Name"), Out);
		}
		return Out;
	}

END_DEFINE_SPEC(FBuildGpuProfileJsonTreeSpec)

void FBuildGpuProfileJsonTreeSpec::Define()
{
	It("returns empty array for empty input", [this]()
	{
		TestEqual(TEXT("root count"), AsArray(UGpuProfilerToolset::BuildGpuProfileJsonTree(FString(), 0.f)).Num(), 0);
	});

	It("returns empty array for malformed JSON", [this]()
	{
		TestEqual(TEXT("root count"), AsArray(UGpuProfilerToolset::BuildGpuProfileJsonTree(TEXT("not json"), 0.f)).Num(), 0);
	});

	It("names the queue root from the Queue field, not EventName", [this]()
	{
		const FString Source = TEXT(R"json(
			[{"Depth":0, "Queue":"Graphics", "EventName":"_ignored_", "GPUDurationMs":0.0}]
		)json");

		TArray<TSharedPtr<FJsonValue>> Roots = AsArray(UGpuProfilerToolset::BuildGpuProfileJsonTree(Source, 0.f));
		TestEqual(TEXT("root count"), Roots.Num(), 1);
		TestEqual(TEXT("root Name"), NameOf(Roots[0]), TEXT("Graphics"));
	});

	It("omits the Children key entirely on leaves", [this]()
	{
		const FString Source = TEXT(R"json(
			[{"Depth":0, "Queue":"Graphics", "EventName":"_root_", "GPUDurationMs":0.0}]
		)json");

		TSharedPtr<FJsonObject> Root = AsObject(AsArray(UGpuProfilerToolset::BuildGpuProfileJsonTree(Source, 0.f))[0]);
		if (TestTrue(TEXT("root present"), Root.IsValid()))
		{
			TestFalse(TEXT("Children key absent on leaf"), Root->HasField(TEXT("Children")));
		}
	});

	It("names non-root nodes from EventName", [this]()
	{
		const FString Source = TEXT(R"json(
			[
				{"Depth":0, "Queue":"Graphics", "EventName":"_root_", "GPUDurationMs":10.0},
				{"Depth":1, "Queue":"Graphics", "EventName":"BasePass", "GPUDurationMs":5.0}
			]
		)json");

		TSharedPtr<FJsonObject> Root = AsObject(AsArray(UGpuProfilerToolset::BuildGpuProfileJsonTree(Source, 0.f))[0]);
		TArray<TSharedPtr<FJsonValue>> Kids = Children(Root);
		TestEqual(TEXT("child count"), Kids.Num(), 1);
		TestEqual(TEXT("child Name"), NameOf(Kids[0]), TEXT("BasePass"));
	});

	It("drops sub-threshold node along with its entire subtree", [this]()
	{
		// MinInclusiveMs = 5. Subpass(3) is below the floor, so it AND its
		// SubpassChild(100) — which would individually survive — are dropped.
		// PostProcessing(8) is a Depth-1 sibling, kept.
		const FString Source = TEXT(R"json(
			[
				{"Depth":0, "Queue":"Graphics", "EventName":"_root_", "GPUDurationMs":20.0},
				{"Depth":1, "Queue":"Graphics", "EventName":"BasePass", "GPUDurationMs":10.0},
				{"Depth":2, "Queue":"Graphics", "EventName":"Subpass", "GPUDurationMs":3.0},
				{"Depth":3, "Queue":"Graphics", "EventName":"SubpassChild", "GPUDurationMs":100.0},
				{"Depth":1, "Queue":"Graphics", "EventName":"PostProcessing", "GPUDurationMs":8.0}
			]
		)json");

		TSharedPtr<FJsonObject> Root = AsObject(AsArray(UGpuProfilerToolset::BuildGpuProfileJsonTree(Source, 5.f))[0]);
		TArray<TSharedPtr<FJsonValue>> Kids = Children(Root);

		TestEqual(TEXT("kept Depth-1 count"), Kids.Num(), 2);
		TestEqual(TEXT("first kept"), NameOf(Kids[0]), TEXT("BasePass"));
		TestEqual(TEXT("second kept"), NameOf(Kids[1]), TEXT("PostProcessing"));

		// BasePass should have no surviving descendants (Subpass + SubpassChild culled).
		TestFalse(TEXT("BasePass became a leaf"), AsObject(Kids[0])->HasField(TEXT("Children")));
	});

	It("preserves Depth-0 queue roots even when below the threshold", [this]()
	{
		// Queue roots are anchors — never culled regardless of MinInclusiveMs.
		const FString Source = TEXT(R"json(
			[{"Depth":0, "Queue":"Graphics", "EventName":"_root_", "GPUDurationMs":0.0}]
		)json");

		TestEqual(TEXT("root count"), AsArray(UGpuProfilerToolset::BuildGpuProfileJsonTree(Source, 100.f)).Num(), 1);
	});

	It("produces one root per source queue", [this]()
	{
		const FString Source = TEXT(R"json(
			[
				{"Depth":0, "Queue":"Graphics", "EventName":"_root_", "GPUDurationMs":10.0},
				{"Depth":1, "Queue":"Graphics", "EventName":"BasePass", "GPUDurationMs":8.0},
				{"Depth":0, "Queue":"Async Compute", "EventName":"_root_", "GPUDurationMs":3.0}
			]
		)json");

		TArray<TSharedPtr<FJsonValue>> Roots = AsArray(UGpuProfilerToolset::BuildGpuProfileJsonTree(Source, 0.f));
		TestEqual(TEXT("root count"), Roots.Num(), 2);
		TestEqual(TEXT("first queue"), NameOf(Roots[0]), TEXT("Graphics"));
		TestEqual(TEXT("second queue"), NameOf(Roots[1]), TEXT("Async Compute"));
	});

	It("rewinds the ancestor stack across multiple levels when depth jumps back", [this]()
	{
		// Depth path: 0 -> 1 -> 2 -> 3 -> 1. The trailing Depth-1 entry must
		// become a sibling of the first Depth-1 entry, not nested inside the
		// Depth-3 chain. Catches off-by-one stack-rewinding bugs.
		const FString Source = TEXT(R"json(
			[
				{"Depth":0, "Queue":"Q", "EventName":"_root_", "GPUDurationMs":10.0},
				{"Depth":1, "Queue":"Q", "EventName":"A", "GPUDurationMs":8.0},
				{"Depth":2, "Queue":"Q", "EventName":"AA", "GPUDurationMs":6.0},
				{"Depth":3, "Queue":"Q", "EventName":"AAA", "GPUDurationMs":4.0},
				{"Depth":1, "Queue":"Q", "EventName":"B", "GPUDurationMs":2.0}
			]
		)json");

		TSharedPtr<FJsonObject> Root = AsObject(AsArray(UGpuProfilerToolset::BuildGpuProfileJsonTree(Source, 0.f))[0]);
		TArray<TSharedPtr<FJsonValue>> Kids = Children(Root);

		TestEqual(TEXT("Depth-1 sibling count"), Kids.Num(), 2);
		TestEqual(TEXT("first sibling"), NameOf(Kids[0]), TEXT("A"));
		TestEqual(TEXT("second sibling"), NameOf(Kids[1]), TEXT("B"));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
