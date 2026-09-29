// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioRequestFit.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioRequestFitTest,
	"HyperAIStudio.NativeTools.RequestFit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioRequestFitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FHyperAIStudioRequestFit Fit;
	int32 PageSize = 0;
	Fit.ClampOrDefault(PageSize, 1, 64, 32, TEXT("page_size"));
	TestEqual(TEXT("0 means the default"), PageSize, 32);
	TestTrue(TEXT("without a note"), Fit.Notes.IsEmpty());
	TestEqual(TEXT("nothing fitted leaves the diagnostic as it was"), Fit.AppendTo(TEXT("Done.")), FString(TEXT("Done.")));
	int32 Budget = 500;
	Fit.ClampOrDefault(Budget, 1, 150, 100, TEXT("max_game_thread_ms"));
	float Tolerance = 0.25f;
	Fit.Clamp(Tolerance, 1.f, 1000.f, TEXT("symmetry_tolerance"));
	TestEqual(TEXT("a limit past its range takes the nearest end"), Budget, 150);
	TestEqual(TEXT("and the reply says so"), Fit.AppendTo(TEXT("Done.")),
		FString(TEXT("Done. Adjusted to fit: max_game_thread_ms 500 -> 150; symmetry_tolerance 0.25 -> 1.0.")));

	struct FRequest
	{
		bool bDryRun = true;
		FString OperationId = TEXT("beacon-probe-001");
		FString ExpectedPlanHash;
	};
	struct FReport
	{
		FString Status;
		FString OperationId;
	};
	FRequest Request;
	FHyperAIStudioRequestFit DryRun;
	DryRun.DryRun(Request.bDryRun, Request.OperationId, Request.ExpectedPlanHash);
	TestTrue(TEXT("a dry run drops the submission fields"), Request.OperationId.IsEmpty() && DryRun.Notes.Num() == 1);

	// An id an earlier session used: the edit runs under the next free one, and a second try of it replays that one.
	Request.bDryRun = false;
	Request.OperationId = TEXT("beacon-probe-001");
	TMap<FString, FString> Bound = {{TEXT("beacon-probe-001"), TEXT("old plan")}, {TEXT("beacon-probe-001-2"), TEXT("older plan")}};
	auto Submit = [&Bound](const FRequest& Planned)
	{
		const FString* Plan = Bound.Find(Planned.OperationId);
		if (Plan && *Plan != TEXT("new plan")) return FReport{TEXT("operation_id_conflict")};
		const bool bReplay = Plan != nullptr;
		Bound.Add(Planned.OperationId, TEXT("new plan"));
		return FReport{bReplay ? TEXT("replayed") : TEXT("submitted")};
	};
	FHyperAIStudioRequestFit Fresh;
	const FReport Submitted = Fresh.FreshOperationId(Request, Submit);
	TestEqual(TEXT("a conflicting id runs under the next free one"), Submitted.Status, FString(TEXT("submitted")));
	TestEqual(TEXT("which is"), Request.OperationId, FString(TEXT("beacon-probe-001-3")));
	TestEqual(TEXT("and the reply carries it"), Submitted.OperationId, FString(TEXT("beacon-probe-001-3")));
	TestTrue(TEXT("and the reply names it: ") + Fresh.AppendTo(FString()), Fresh.AppendTo(FString()).Contains(TEXT("beacon-probe-001-3")));
	Request.OperationId = TEXT("beacon-probe-001");
	TestEqual(TEXT("the same plan again replays"), FHyperAIStudioRequestFit().FreshOperationId(Request, Submit).Status, FString(TEXT("replayed")));
	TestEqual(TEXT("under that id"), Request.OperationId, FString(TEXT("beacon-probe-001-3")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
