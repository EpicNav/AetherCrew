// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAutoApproval.h"
#include "HyperAIStudioNativeReadToolset.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioWidgetShot.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SHyperAIStudioActivitySidebar.h"
#include "SHyperAIStudioAutoApprovalBanner.h"
#include "SHyperAIStudioAutoApprovalDialog.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

namespace HyperAIStudio::AutoApprovalTests
{
	using FAccess = FHyperAIStudioAutoApprovalTestAccess;
	using ECategory = EHyperAIStudioApprovalCategory;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	constexpr ECategory AllCategories[] = {ECategory::Edits, ECategory::Scripts, ECategory::Destructive, ECategory::ExternalEffects};

	FString NewId()
	{
		return TEXT("autoapproval-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	}

	FHyperAIStudioApprovalSummary MakeSummary(const EHyperAIStudioDomainSafety Safety, const TCHAR* PackId = TEXT("level_design"))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = TEXT("hyper_auto_approval_test");
		Summary.Safety = Safety;
		Summary.EffectTarget = TEXT("/Game/Tests/AutoApproval/Target.Target");
		Summary.Effects = {TEXT("test effect")};
		return Summary;
	}

	/** What hyper_script_run holds for a script the runner could not prove undoable. */
	FHyperAIStudioApprovalSummary EscalatedScript()
	{
		FHyperAIStudioApprovalSummary Summary = MakeSummary(EHyperAIStudioDomainSafety::Destructive, TEXT("scripting"));
		Summary.ToolName = TEXT("hyper_script_run");
		Summary.bEscalatedScript = true;
		return Summary;
	}

	FHyperAIStudioApprovalSummary SummaryFor(const ECategory Category)
	{
		switch (Category)
		{
		case ECategory::Scripts: return EscalatedScript();
		case ECategory::Destructive: return MakeSummary(EHyperAIStudioDomainSafety::Destructive);
		case ECategory::ExternalEffects: return MakeSummary(EHyperAIStudioDomainSafety::ExternalEffect);
		default: return MakeSummary(EHyperAIStudioDomainSafety::Edit);
		}
	}

	/** The outcome an approved in-place edit records, as the packs' actions do. */
	void RecordOutcome(const FString& OperationId, const bool bCompleted)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = bCompleted ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = TEXT("level_design");
		Entry.ToolName = TEXT("hyper_auto_approval_test");
		Entry.OperationId = OperationId;
		Entry.StatusCode = bCompleted ? TEXT("completed") : TEXT("failed");
		Entry.Detail = bCompleted ? TEXT("The test edit finished.") : TEXT("The test edit failed.");
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	TOptional<FHyperAIStudioPendingApproval> FindPending(const FString& OperationId)
	{
		for (const FHyperAIStudioPendingApproval& Pending : FHyperAIStudioApprovalGate::GetPending())
		{
			if (Pending.OperationId == OperationId)
			{
				return Pending;
			}
		}
		return {};
	}

	/** The approval Activity recorded for an operation, if any. */
	TOptional<FHyperAIStudioActivityEntry> FindApproval(const FString& OperationId)
	{
		for (const FHyperAIStudioActivityEntry& Entry : FHyperAIStudioAgentActivityLog::GetSnapshot())
		{
			if (Entry.OperationId == OperationId && Entry.Kind == EHyperAIStudioActivityKind::ApprovalApproved)
			{
				return Entry;
			}
		}
		return {};
	}

	/**
	 * Every test starts and ends where module startup does: off, with nothing of its own left waiting. Tests may run in
	 * the user's own editor, so each first skips while the user's own auto-approval is on, and Reset ends only a
	 * test's activation. Gate-only tests drive the sweep through FAccess::PumpGate rather than the core ticker; those
	 * that wait on the edit queue or hyper_operation_wait tick the core ticker, which also serves MCP and the remote
	 * page, so it is the test scope (the test's own ids, and only under GIsAutomationTesting) that keeps a test's
	 * activation from covering anything else.
	 */
	struct FScope
	{
		TArray<FString> Ids;

		FScope()
		{
			FAccess::Reset();
			FHyperAIStudioAgentScoreboard::SetStoragePathForTests(
				FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests/AgentScoreboard.json")));
		}

		~FScope()
		{
			FAccess::Reset();
			for (const FString& Id : Ids)
			{
				FHyperAIStudioApprovalGate::Reject(Id, TEXT("test over"));
			}
			FHyperAIStudioAgentScoreboard::SetStoragePathForTests(FString());
		}

		FString Add()
		{
			return Ids.Add_GetRef(NewId());
		}

		TSet<FString> All() const
		{
			return TSet<FString>(Ids);
		}
	};

	bool Hold(const FString& OperationId, const FHyperAIStudioApprovalSummary& Summary, TFunction<void()> Run)
	{
		FString Error;
		return FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, MoveTemp(Run), Error);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalUnreachableTest,
	"HyperAIStudio.Chat.AutoApproval.OffAndUnreachable",
	HyperAIStudio::AutoApprovalTests::Flags)

bool FHyperAIStudioAutoApprovalUnreachableTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope;
	TestFalse(TEXT("A fresh editor has auto-approval off"), FHyperAIStudioAutoApproval::IsActive());
	TestFalse(TEXT("and its status says so"), FHyperAIStudioAutoApproval::GetStatus().bActive);
	TestTrue(TEXT("and no activation is current"), FHyperAIStudioAutoApproval::GetActivationId() == 0);

	auto Names = [](const FString& Name)
	{
		return Name.ToLower().Replace(TEXT("_"), TEXT("")).Contains(TEXT("autoapprov"));
	};
	for (TFieldIterator<FProperty> It(UHyperAIStudioSettings::StaticClass()); It; ++It)
	{
		TestFalse(FString::Printf(TEXT("No setting turns it on (%s)"), *It->GetName()), Names(It->GetName()));
	}
	int32 ConsoleObjects = 0;
	IConsoleManager::Get().ForEachConsoleObjectThatContains(
		FConsoleObjectVisitor::CreateLambda([&ConsoleObjects](const TCHAR*, IConsoleObject*) { ++ConsoleObjects; }), TEXT("autoapprov"));
	TestEqual(TEXT("No console command or variable names it"), ConsoleObjects, 0);
	// Tools, Python and Blueprints reach only what is reflected: nothing reflected names it.
	TArray<FString> Reflected;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (Names(It->GetName()))
		{
			Reflected.Add(It->GetName());
		}
		for (TFieldIterator<UFunction> Function(*It, EFieldIteratorFlags::ExcludeSuper); Function; ++Function)
		{
			if (Names(Function->GetName()))
			{
				Reflected.Add(It->GetName() + TEXT("::") + Function->GetName());
			}
		}
	}
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		if (Names(It->GetName()))
		{
			Reflected.Add(It->GetName());
		}
	}
	for (TObjectIterator<UEnum> It; It; ++It)
	{
		if (Names(It->GetName()))
		{
			Reflected.Add(It->GetName());
		}
	}
	TestTrue(TEXT("No reflected class, function, struct or enum names it: ") + FString::Join(Reflected, TEXT(", ")), Reflected.IsEmpty());

	// Even while on, nothing of it reaches a config file, so a restart starts with it off.
	TestTrue(TEXT("It turns on through the dialog"), FAccess::TurnOn({ECategory::Edits}, {Scope.Add()}));
	TArray<FString> Configured;
	for (const FString* Ini : {&GEditorPerProjectIni, &GEditorSettingsIni, &GEditorIni, &GEngineIni, &GGameIni})
	{
		TArray<FString> Sections;
		GConfig->GetSectionNames(*Ini, Sections);
		for (const FString& Section : Sections)
		{
			TArray<FString> Lines;
			GConfig->GetSection(*Section, Lines, *Ini);
			if (Names(Section) || Lines.ContainsByPredicate([&Names](const FString& Line) { return Names(Line); }))
			{
				Configured.Add(Section);
			}
		}
	}
	TestTrue(TEXT("No config section or key holds it: ") + FString::Join(Configured, TEXT(", ")), Configured.IsEmpty());
	FAccess::Reset();
	TestFalse(TEXT("What module startup sees is off"), FHyperAIStudioAutoApproval::IsActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalDialogTest,
	"HyperAIStudio.Chat.AutoApproval.DialogIsTheOnlyWayOn",
	HyperAIStudio::AutoApprovalTests::Flags)

bool FHyperAIStudioAutoApprovalDialogTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope;
	const FString Id = Scope.Add();

	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Dialog = FAccess::MakeDialog();
	TestTrue(TEXT("Edits are ticked by default"), FAccess::IsTicked(*Dialog, ECategory::Edits));
	TestFalse(TEXT("Scripts are not"), FAccess::IsTicked(*Dialog, ECategory::Scripts));
	TestFalse(TEXT("Destructive tools are not"), FAccess::IsTicked(*Dialog, ECategory::Destructive));
	TestFalse(TEXT("External effects are not"), FAccess::IsTicked(*Dialog, ECategory::ExternalEffects));
	TestEqual(TEXT("It lasts an hour by default"), FAccess::GetDuration(*Dialog), EHyperAIStudioAutoApprovalDuration::OneHour);

	// The real native confirmation answers no where nobody can answer it, as in this test run.
	FAccess::PressTurnOn(*Dialog, {Id});
	TestEqual(TEXT("Turn On asks the native confirmation"), FAccess::GetNativeConfirmations(*Dialog), 1);
	TestFalse(TEXT("which fails closed here, so it stays off"), FHyperAIStudioAutoApproval::IsActive());
	TestFalse(TEXT("and the dialog stays open"), FAccess::WasClosed(*Dialog));

	FAccess::SetNativeAnswer(*Dialog, false);
	FAccess::PressTurnOn(*Dialog, {Id});
	TestFalse(TEXT("Cancel in the native confirmation leaves it off"), FHyperAIStudioAutoApproval::IsActive());
	TestFalse(TEXT("with the dialog still open"), FAccess::WasClosed(*Dialog));

	FAccess::SetTicked(*Dialog, ECategory::Edits, false);
	FAccess::SetNativeAnswer(*Dialog, true);
	TestFalse(TEXT("With nothing ticked, Turn On is disabled"), FAccess::CanTurnOn(*Dialog));
	FAccess::PressTurnOn(*Dialog, {Id});
	TestFalse(TEXT("and does nothing"), FHyperAIStudioAutoApproval::IsActive());
	TestEqual(TEXT("not even ask"), FAccess::GetNativeConfirmations(*Dialog), 2);

	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Cancelled = FAccess::MakeDialog();
	FAccess::SetNativeAnswer(*Cancelled, true);
	FAccess::PressCancel(*Cancelled);
	TestTrue(TEXT("Cancel closes the dialog"), FAccess::WasClosed(*Cancelled));
	FAccess::PressTurnOn(*Cancelled, {Id});
	TestFalse(TEXT("and leaves it off, even if Turn On is pressed after"), FHyperAIStudioAutoApproval::IsActive());

	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Escaped = FAccess::MakeDialog();
	FAccess::SetNativeAnswer(*Escaped, true);
	TestTrue(TEXT("The window focuses Cancel as it opens, so Enter cancels"), FAccess::FocusesCancel(*Escaped));
	FAccess::PressKey(*Escaped, EKeys::Enter);
	TestFalse(TEXT("Enter reaching the dialog itself presses nothing"), FAccess::WasClosed(*Escaped) || FHyperAIStudioAutoApproval::IsActive());
	FAccess::PressKey(*Escaped, EKeys::Escape);
	TestTrue(TEXT("Escape closes the dialog"), FAccess::WasClosed(*Escaped));
	TestFalse(TEXT("and leaves it off"), FHyperAIStudioAutoApproval::IsActive());
	TestEqual(TEXT("without asking the native confirmation"), FAccess::GetNativeConfirmations(*Escaped), 0);

	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Confirmed = FAccess::MakeDialog();
	FAccess::SetTicked(*Confirmed, ECategory::ExternalEffects, true);
	FAccess::SetDuration(*Confirmed, EHyperAIStudioAutoApprovalDuration::FifteenMinutes);
	FAccess::SetNativeAnswer(*Confirmed, true);
	FAccess::PressTurnOn(*Confirmed, {Id});
	const FHyperAIStudioAutoApprovalStatus Status = FHyperAIStudioAutoApproval::GetStatus();
	TestTrue(TEXT("Confirmed in both, it turns on"), Status.bActive);
	TestTrue(TEXT("with exactly the ticked categories"),
		Status.Categories == TArray<ECategory>({ECategory::Edits, ECategory::ExternalEffects}));
	TestEqual(TEXT("for the chosen time"), Status.Duration, EHyperAIStudioAutoApprovalDuration::FifteenMinutes);
	TestTrue(TEXT("which counts down from 15 minutes"), Status.SecondsLeft > 14.0 * 60.0 && Status.SecondsLeft <= 15.0 * 60.0);
	TestTrue(TEXT("and the dialog closes"), FAccess::WasClosed(*Confirmed));
	const TArray<FHyperAIStudioActivityEntry> Entries = FHyperAIStudioAgentActivityLog::GetSnapshot();
	TestTrue(TEXT("Activity records that it turned on"), !Entries.IsEmpty()
		&& Entries[0].Kind == EHyperAIStudioActivityKind::AutoApproval && Entries[0].ToolName == TEXT("turned on")
		&& Entries[0].Detail.Contains(TEXT("External effects")) && Entries[0].Agent.IsEmpty());
	TestTrue(TEXT("as a test's, never as one a person confirmed: ") + (Entries.IsEmpty() ? FString() : Entries[0].Detail),
		!Entries.IsEmpty() && Entries[0].Detail.Contains(TEXT("automation test")) && !Entries[0].Detail.Contains(TEXT("confirmed")));

	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Again = FAccess::MakeDialog();
	TestTrue(TEXT("The next dialog starts from the defaults again"), FAccess::IsTicked(*Again, ECategory::Edits)
		&& !FAccess::IsTicked(*Again, ECategory::ExternalEffects)
		&& FAccess::GetDuration(*Again) == EHyperAIStudioAutoApprovalDuration::OneHour);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalCategoriesTest,
	"HyperAIStudio.Chat.AutoApproval.CategoriesApproveOrWait",
	HyperAIStudio::AutoApprovalTests::Flags)

bool FHyperAIStudioAutoApprovalCategoriesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	for (const ECategory Chosen : AllCategories)
	{
		FScope Scope;
		const FString Label = ::LexToString(Chosen);
		TMap<ECategory, FString> Ids;
		for (const ECategory Category : AllCategories)
		{
			Ids.Add(Category, Scope.Add());
		}
		TestTrue(Label + TEXT(": it turns on"), FAccess::TurnOn({Chosen}, Scope.All()));
		TSet<ECategory> Ran;
		for (const ECategory Category : AllCategories)
		{
			TestTrue(Label + TEXT(": a request is held"), Hold(Ids[Category], SummaryFor(Category), [&Ran, Category]() { Ran.Add(Category); }));
			const TOptional<FHyperAIStudioPendingApproval> Pending = FindPending(Ids[Category]);
			TestTrue(Label + TEXT(": it is filed under its category ") + ::LexToString(Category),
				Pending.IsSet() && Pending->Category.IsSet() && *Pending->Category == Category);
			TestEqual(Label + TEXT(": it is marked as about to be approved only when chosen"),
				Pending.IsSet() && Pending->bAutoApproving, Category == Chosen);
		}
		TestTrue(Label + TEXT(": nothing runs until the editor ticks"), Ran.IsEmpty());
		FAccess::PumpGate();
		for (const ECategory Category : AllCategories)
		{
			const FString What = Label + TEXT(" on, ") + ::LexToString(Category) + TEXT(" request: ");
			TestEqual(What + TEXT("runs only if chosen"), Ran.Contains(Category), Category == Chosen);
			TestEqual(What + TEXT("waits unless chosen"), FHyperAIStudioApprovalGate::HasPending(Ids[Category]), Category != Chosen);
		}
		const TOptional<FHyperAIStudioActivityEntry> Approved = FindApproval(Ids[Chosen]);
		TestTrue(Label + TEXT(": Activity marks it auto-approved, with its category"),
			Approved.IsSet() && Approved->AutoApprovedAs.IsSet() && *Approved->AutoApprovedAs == Chosen
			&& Approved->Detail.StartsWith(FString::Printf(TEXT("Auto-approved as %s"), ::LexToString(Chosen))));
		const TOptional<FHyperAIStudioApprovalRecord> Record = FHyperAIStudioApprovalGate::FindApproval(Ids[Chosen]);
		TestTrue(Label + TEXT(": the gate remembers who approved it"), Record.IsSet() && Record->AutoApprovedAs == Chosen);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalPendingTest,
	"HyperAIStudio.Chat.AutoApproval.PendingBeforeActivationKeepWaiting",
	HyperAIStudio::AutoApprovalTests::Flags)

bool FHyperAIStudioAutoApprovalPendingTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope;
	const FString Before = Scope.Add();
	const FString After = Scope.Add();
	bool bBeforeRan = false;
	bool bAfterRan = false;
	TestTrue(TEXT("A request waits before auto-approval is on"),
		Hold(Before, MakeSummary(EHyperAIStudioDomainSafety::Edit), [&bBeforeRan]() { bBeforeRan = true; }));

	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Dialog = FAccess::MakeDialog();
	TestTrue(TEXT("The dialog lists what already waits"), FAccess::GetPendingShown(*Dialog) >= 1
		&& FAccess::GetPendingShown(*Dialog) == FHyperAIStudioApprovalGate::GetPending().Num());
	FAccess::SetNativeAnswer(*Dialog, true);
	FAccess::PressTurnOn(*Dialog, Scope.All());
	TestTrue(TEXT("It turns on"), FHyperAIStudioAutoApproval::IsActive());
	FAccess::PumpGate();
	TestFalse(TEXT("The request made before keeps waiting for a click"), bBeforeRan);
	TestTrue(TEXT("and is still held"), FHyperAIStudioApprovalGate::HasPending(Before));
	const TOptional<FHyperAIStudioPendingApproval> Pending = FindPending(Before);
	TestTrue(TEXT("and is not marked as about to be approved"), Pending.IsSet() && !Pending->bAutoApproving);

	TestTrue(TEXT("A request made after"), Hold(After, MakeSummary(EHyperAIStudioDomainSafety::Edit), [&bAfterRan]() { bAfterRan = true; }));
	FAccess::PumpGate();
	TestTrue(TEXT("is approved"), bAfterRan && !FHyperAIStudioApprovalGate::HasPending(After));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalExpiryTest,
	"HyperAIStudio.Chat.AutoApproval.ExpiryAndStop",
	HyperAIStudio::AutoApprovalTests::Flags)

bool FHyperAIStudioAutoApprovalExpiryTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope;
	const FString Late = Scope.Add();
	const FString Stopped = Scope.Add();
	TestTrue(TEXT("It turns on for 15 minutes"),
		FAccess::TurnOn({ECategory::Edits}, Scope.All(), EHyperAIStudioAutoApprovalDuration::FifteenMinutes));
	FAccess::AdvanceClock(15.0 * 60.0 + 1.0);
	// Its own one-second ticker notices, with no request to prompt it.
	TestTrue(TEXT("Its expiry ticker is registered"), FAccess::TickExpiry());
	TestFalse(TEXT("When its time runs out it turns off"), FHyperAIStudioAutoApproval::IsActive());
	TestTrue(TEXT("because it expired"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::Expired);
	bool bLateRan = false;
	TestTrue(TEXT("A new request"), Hold(Late, MakeSummary(EHyperAIStudioDomainSafety::Edit), [&bLateRan]() { bLateRan = true; }));
	FAccess::PumpGate();
	TestTrue(TEXT("waits for a click again"), !bLateRan && FHyperAIStudioApprovalGate::HasPending(Late));
	const TArray<FHyperAIStudioActivityEntry> Entries = FHyperAIStudioAgentActivityLog::GetSnapshot();
	TestTrue(TEXT("Activity records that it turned off and why"), Entries.ContainsByPredicate([](const FHyperAIStudioActivityEntry& Entry)
	{
		return Entry.Kind == EHyperAIStudioActivityKind::AutoApproval && Entry.ToolName == TEXT("turned off")
			&& Entry.Detail.StartsWith(TEXT("its time ran out"));
	}));

	FAccess::Reset();
	TestTrue(TEXT("It turns on again"), FAccess::TurnOn({ECategory::Edits}, Scope.All()));
	int32 Changes = 0;
	const FDelegateHandle Handle = FHyperAIStudioAutoApproval::OnChanged().AddLambda([&Changes]() { ++Changes; });
	ON_SCOPE_EXIT { FHyperAIStudioAutoApproval::OnChanged().Remove(Handle); };
	bool bStoppedRan = false;
	TestTrue(TEXT("A covered request is held"), Hold(Stopped, MakeSummary(EHyperAIStudioDomainSafety::Edit), [&bStoppedRan]() { bStoppedRan = true; }));
	TestTrue(TEXT("and about to be approved"), FHyperAIStudioApprovalGate::IsAutoApproving(Stopped));
	// The banner's Stop button, as a person presses it.
	const TSharedRef<SHyperAIStudioAutoApprovalBanner> Banner = SNew(SHyperAIStudioAutoApprovalBanner);
	FAccess::PressStop(*Banner);
	TestFalse(TEXT("Stop turns it off at once"), FHyperAIStudioAutoApproval::IsActive());
	TestTrue(TEXT("as stopped by the user"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::Stopped);
	TestTrue(TEXT("and tells the banners"), Changes > 0);
	TestTrue(TEXT("The request it had not reached yet is no longer about to be approved"), !FHyperAIStudioApprovalGate::IsAutoApproving(Stopped));
	FAccess::PumpGate();
	TestTrue(TEXT("and waits for a click"), !bStoppedRan && FHyperAIStudioApprovalGate::HasPending(Stopped));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalGuardTest,
	"HyperAIStudio.Chat.AutoApproval.RunawayGuard",
	HyperAIStudio::AutoApprovalTests::Flags)

bool FHyperAIStudioAutoApprovalGuardTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	{
		FScope Scope;
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Scope.Add();
		}
		TestTrue(TEXT("Failures: it turns on"), FAccess::TurnOn({ECategory::Edits}, Scope.All()));
		int32 Next = 0;
		auto Run = [&](const bool bCompleted)
		{
			const FString Id = Scope.Ids[Next++];
			Hold(Id, SummaryFor(ECategory::Edits), [Id, bCompleted]() { RecordOutcome(Id, bCompleted); });
			FAccess::PumpGate();
		};
		Run(false);
		Run(false);
		TestTrue(TEXT("Two failures in a row leave it on"), FHyperAIStudioAutoApproval::IsActive());
		Run(true);
		Run(false);
		Run(false);
		TestTrue(TEXT("A completion between failures starts the count again"), FHyperAIStudioAutoApproval::IsActive());
		Run(false);
		TestFalse(TEXT("Three failures in a row turn it off"), FHyperAIStudioAutoApproval::IsActive());
		TestTrue(TEXT("for that reason"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::RepeatedFailures);
		const FString Waiting = Scope.Ids[Next];
		Hold(Waiting, SummaryFor(ECategory::Edits), []() {});
		FAccess::PumpGate();
		TestTrue(TEXT("and the next request waits"), FHyperAIStudioApprovalGate::HasPending(Waiting));
	}
	{
		FScope Scope;
		for (int32 Index = 0; Index <= FHyperAIStudioAutoApproval::MaxApprovals; ++Index)
		{
			Scope.Add();
		}
		TestTrue(TEXT("Approvals: it turns on"), FAccess::TurnOn({ECategory::Edits}, Scope.All()));
		int32 Approved = 0;
		for (int32 Index = 0; Index < FHyperAIStudioAutoApproval::MaxApprovals; ++Index)
		{
			const FString Id = Scope.Ids[Index];
			Hold(Id, SummaryFor(ECategory::Edits), [Id]() { RecordOutcome(Id, true); });
			FAccess::PumpGate();
			Approved += FHyperAIStudioApprovalGate::HasPending(Id) ? 0 : 1;
		}
		TestEqual(TEXT("It approves up to its limit"), Approved, FHyperAIStudioAutoApproval::MaxApprovals);
		TestFalse(TEXT("then turns itself off"), FHyperAIStudioAutoApproval::IsActive());
		TestTrue(TEXT("for that reason"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::ApprovalLimit);
		const FString Over = Scope.Ids.Last();
		Hold(Over, SummaryFor(ECategory::Edits), []() {});
		FAccess::PumpGate();
		TestTrue(TEXT("and the next request waits"), FHyperAIStudioApprovalGate::HasPending(Over));
	}
	{
		FScope Scope;
		for (int32 Index = 0; Index <= FHyperAIStudioAutoApproval::MaxExternalEffects; ++Index)
		{
			Scope.Add();
		}
		TestTrue(TEXT("External effects: it turns on"), FAccess::TurnOn({ECategory::ExternalEffects}, Scope.All()));
		for (int32 Index = 0; Index < FHyperAIStudioAutoApproval::MaxExternalEffects; ++Index)
		{
			const FString Id = Scope.Ids[Index];
			Hold(Id, SummaryFor(ECategory::ExternalEffects), [Id]() { RecordOutcome(Id, true); });
			FAccess::PumpGate();
		}
		TestFalse(TEXT("Ten external effects turn it off"), FHyperAIStudioAutoApproval::IsActive());
		TestTrue(TEXT("for that reason"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::ExternalEffectLimit);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalStatusTest,
	"HyperAIStudio.Chat.AutoApproval.AgentVisibleStatus",
	HyperAIStudio::AutoApprovalTests::Flags)

bool FHyperAIStudioAutoApprovalStatusTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope;
	const FString Auto = Scope.Add();
	const FString Waited = Scope.Add();
	const FString Clicked = NewId();
	Scope.Ids.Add(Clicked);
	// The click-approved request is left out of the activation, so only a click approves it.
	TestTrue(TEXT("It turns on"), FAccess::TurnOn({ECategory::Edits}, {Auto, Waited}));

	TestTrue(TEXT("A covered request is held"), Hold(Auto, MakeSummary(EHyperAIStudioDomainSafety::Edit), [Auto]() { RecordOutcome(Auto, true); }));
	FHyperAIOperationStatus Status = UHyperAIStudioNativeReadToolset::hyper_operation_status(Auto);
	TestEqual(TEXT("Before the editor ticks, an agent reads it as queued"), Status.Status, FString(TEXT("queued")));
	TestEqual(TEXT("scheduled by auto-approval"), Status.StatusCode, FString(TEXT("auto_approval_scheduled")));
	TestEqual(TEXT("and approved by it"), Status.ApprovedBy, FString(TEXT("auto_approval")));
	TestFalse(TEXT("not finished"), Status.bTerminal);
	// What a pack that replies at once (hyper_script_run, hyper_playtest_run, hyper_mesh_generate...) tells the agent.
	FString Reply = TEXT("awaiting_user_approval");
	FString Diagnostic = TEXT("Waiting in HyperAI Chat, Activity panel; nothing ran yet.");
	TestTrue(TEXT("A pack's reply learns that auto-approval covers it"), FHyperAIStudioApprovalGate::DescribeHeld(Auto, Reply, Diagnostic));
	TestEqual(TEXT("and says queued, not awaiting approval"), Reply, FString(TEXT("queued")));
	TestEqual(TEXT("with what hyper_operation_status says"), Diagnostic, Status.Diagnostic);
	FAccess::PumpGate();
	Status = UHyperAIStudioNativeReadToolset::hyper_operation_status(Auto);
	TestEqual(TEXT("After it, the edit's own outcome shows"), Status.Status, FString(TEXT("completed")));
	TestEqual(TEXT("still marked as auto-approved"), Status.ApprovedBy, FString(TEXT("auto_approval")));
	TestTrue(TEXT("and the diagnostic says nobody clicked: ") + Status.Diagnostic, Status.Diagnostic.StartsWith(TEXT("Auto-approved (edits)")));

	// hyper_operation_wait without bWaitThroughApproval waits through an auto-approval, as through the edit queue.
	TestTrue(TEXT("Another covered request is held"), Hold(Waited, MakeSummary(EHyperAIStudioDomainSafety::Edit), [Waited]() { RecordOutcome(Waited, true); }));
	const TStrongObjectPtr<UHyperAIOperationWaitResult> Wait(UHyperAIStudioNativeReadToolset::hyper_operation_wait(Waited, 10000, false));
	TestTrue(TEXT("The wait does not end at once"), Wait.IsValid() && Wait->Value.CompletedBecause.IsEmpty());
	for (int32 Tick = 0; Tick < 50 && Wait->Value.CompletedBecause.IsEmpty(); ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.25f);
	}
	TestEqual(TEXT("It ends when the edit does"), Wait->Value.CompletedBecause, FString(TEXT("terminal")));
	TestEqual(TEXT("with its outcome"), Wait->Value.Status.Status, FString(TEXT("completed")));

	TestTrue(TEXT("A request outside it is held"), Hold(Clicked, MakeSummary(EHyperAIStudioDomainSafety::Edit), [Clicked]() { RecordOutcome(Clicked, true); }));
	Status = UHyperAIStudioNativeReadToolset::hyper_operation_status(Clicked);
	TestEqual(TEXT("and reads as waiting for a person"), Status.Status, FString(TEXT("awaiting_user_approval")));
	Reply = TEXT("awaiting_user_approval");
	Diagnostic = TEXT("Waiting.");
	TestFalse(TEXT("A pack's reply for it is left as it was"), FHyperAIStudioApprovalGate::DescribeHeld(Clicked, Reply, Diagnostic));
	TestTrue(TEXT("awaiting approval"), Reply == TEXT("awaiting_user_approval") && Diagnostic == TEXT("Waiting."));
	FString Error;
	TestTrue(TEXT("A click approves it"), FHyperAIStudioApprovalGate::Approve(Clicked, Error));
	Status = UHyperAIStudioNativeReadToolset::hyper_operation_status(Clicked);
	TestEqual(TEXT("and the agent reads who approved it"), Status.ApprovedBy, FString(TEXT("user")));
	TestFalse(TEXT("with no auto-approval note"), Status.Diagnostic.StartsWith(TEXT("Auto-approved")));
	const TOptional<FHyperAIStudioActivityEntry> Approved = FindApproval(Clicked);
	TestTrue(TEXT("Activity shows a plain approval"), Approved.IsSet() && !Approved->AutoApprovedAs.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalBusyTest,
	"HyperAIStudio.Chat.AutoApproval.DefersWhileEditorBusy",
	HyperAIStudio::AutoApprovalTests::Flags)

bool FHyperAIStudioAutoApprovalBusyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	if (!GEditor)
	{
		AddInfo(TEXT("No editor to hold a transaction open."));
		return true;
	}
	FScope Scope;
	const FString Id = Scope.Add();
	TestTrue(TEXT("It turns on"), FAccess::TurnOn({ECategory::Edits}, Scope.All()));
	bool bRan = false;
	const int32 Transaction = GEditor->BeginTransaction(FText::FromString(TEXT("Auto-approval busy test")));
	bool bOpen = true;
	ON_SCOPE_EXIT
	{
		if (bOpen)
		{
			GEditor->CancelTransaction(Transaction);
		}
	};
	TestTrue(TEXT("A covered request is held"), Hold(Id, MakeSummary(EHyperAIStudioDomainSafety::Edit), [&bRan]() { bRan = true; }));
	FAccess::PumpGate();
	TestFalse(TEXT("It is not approved inside the user's open transaction"), bRan);
	TestTrue(TEXT("and stays scheduled"), FHyperAIStudioApprovalGate::IsAutoApproving(Id));
	GEditor->CancelTransaction(Transaction);
	bOpen = false;
	FAccess::PumpGate();
	TestTrue(TEXT("Once the transaction ends, it is approved"), bRan && !FHyperAIStudioApprovalGate::HasPending(Id));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalTestScopeTest,
	"HyperAIStudio.Chat.AutoApproval.TestActivationCoversOnlyItsOwnIds",
	HyperAIStudio::AutoApprovalTests::Flags)

bool FHyperAIStudioAutoApprovalTestScopeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope;
	const FString Own = Scope.Add();
	const FString Other = Scope.Add();
	// A test can run in the user's own editor (Epic's automation toolset), so its activation must never reach their requests.
	TestTrue(TEXT("It turns on for one id"), FAccess::TurnOn({ECategory::Edits}, {Own}));
	const TArray<FHyperAIStudioActivityEntry> Turned = FHyperAIStudioAgentActivityLog::GetSnapshot();
	TestTrue(TEXT("Activity records it as a test's, covering only the test's own requests"), !Turned.IsEmpty()
		&& Turned[0].ToolName == TEXT("turned on") && Turned[0].Detail.Contains(TEXT("automation test")));
	bool bOtherRan = false;
	bool bOwnRan = false;
	TestTrue(TEXT("Another request is held"), Hold(Other, MakeSummary(EHyperAIStudioDomainSafety::Edit), [&bOtherRan]() { bOtherRan = true; }));
	FAccess::PumpGate();
	TestTrue(TEXT("and waits"), !bOtherRan && FHyperAIStudioApprovalGate::HasPending(Other) && !FHyperAIStudioApprovalGate::IsAutoApproving(Other));
	TestTrue(TEXT("Its own request is held"), Hold(Own, MakeSummary(EHyperAIStudioDomainSafety::Edit), [&bOwnRan]() { bOwnRan = true; }));
	FAccess::PumpGate();
	TestTrue(TEXT("and approved"), bOwnRan);

	// An activation the dialog confirms in a test with no ids covers nothing at all.
	FAccess::Reset();
	TestTrue(TEXT("It turns on with no ids"), FAccess::TurnOn({ECategory::Edits}, {}));
	const FString Stray = Scope.Add();
	bool bStrayRan = false;
	Hold(Stray, MakeSummary(EHyperAIStudioDomainSafety::Edit), [&bStrayRan]() { bStrayRan = true; });
	FAccess::PumpGate();
	TestTrue(TEXT("and approves nothing"), !bStrayRan && FHyperAIStudioApprovalGate::HasPending(Stray));
	FAccess::Reset();
	TestFalse(TEXT("A test's own activation ends with the test"), FHyperAIStudioAutoApproval::IsActive());
	const TArray<FHyperAIStudioActivityEntry> Ended = FHyperAIStudioAgentActivityLog::GetSnapshot();
	TestTrue(TEXT("and Activity says a test ended it, not that the editor closed"), !Ended.IsEmpty()
		&& Ended[0].ToolName == TEXT("turned off") && Ended[0].Detail.Contains(TEXT("automation test"))
		&& !Ended[0].Detail.Contains(TEXT("editor is closing")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalSpendSettingsTest,
	"HyperAIStudio.Chat.AutoApproval.SpendSettingsChangeEndsIt",
	HyperAIStudio::AutoApprovalTests::Flags)

bool FHyperAIStudioAutoApprovalSpendSettingsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const float Cap = Settings->ExternalServiceSessionSpendCap;
	const TArray<FHyperAIStudioExternalServiceSettings> Services = Settings->ExternalServices;
	const TArray<FHyperAIStudioImageModelSettings> Models = Settings->ImageModels;
	ON_SCOPE_EXIT
	{
		Settings->ExternalServiceSessionSpendCap = Cap;
		Settings->ExternalServices = Services;
		Settings->ImageModels = Models;
	};
	const FString Paid = Scope.Add();
	TestTrue(TEXT("External-effects auto-approval turns on"), FAccess::TurnOn({ECategory::ExternalEffects, ECategory::Edits}, Scope.All()));
	FAccess::PumpGate();
	TestTrue(TEXT("While nothing changes, it stays on"), FHyperAIStudioAutoApproval::IsActive());

	// What Epic's settings toolset lets any MCP client do with no approval: raise the cap after the user confirmed.
	Settings->ExternalServiceSessionSpendCap = Cap + 100.0f;
	bool bRan = false;
	TestTrue(TEXT("A paid request is held"), Hold(Paid, SummaryFor(ECategory::ExternalEffects), [&bRan]() { bRan = true; }));
	FAccess::PumpGate();
	TestFalse(TEXT("A raised cap turns it off before it approves anything"), FHyperAIStudioAutoApproval::IsActive());
	TestTrue(TEXT("for that reason"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::SpendSettingsChanged);
	TestTrue(TEXT("and the paid request waits for a click"), !bRan && FHyperAIStudioApprovalGate::HasPending(Paid));
	const TArray<FHyperAIStudioActivityEntry> Entries = FHyperAIStudioAgentActivityLog::GetSnapshot();
	TestTrue(TEXT("Activity says why it turned off"), Entries.ContainsByPredicate([](const FHyperAIStudioActivityEntry& Entry)
	{
		return Entry.ToolName == TEXT("turned off") && Entry.Detail.Contains(TEXT("spend cap"));
	}));

	// A service made free, or a cheaper model, is the same thing the other way round; the one-second check notices.
	Settings->ExternalServiceSessionSpendCap = Cap;
	FAccess::Reset();
	TestTrue(TEXT("It turns on again"), FAccess::TurnOn({ECategory::ExternalEffects}, Scope.All()));
	FHyperAIStudioExternalServiceSettings& Service = Settings->ExternalServices.AddDefaulted_GetRef();
	Service.Id = TEXT("autoapproval-test-service");
	Service.CostPerCall = 0.0f;
	TestTrue(TEXT("Its one-second check runs"), FAccess::TickExpiry());
	TestFalse(TEXT("and turns it off for a changed service"), FHyperAIStudioAutoApproval::IsActive());
	TestTrue(TEXT("for that reason too"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::SpendSettingsChanged);

	Settings->ExternalServices = Services;
	FAccess::Reset();
	TestTrue(TEXT("It turns on once more"), FAccess::TurnOn({ECategory::ExternalEffects}, Scope.All()));
	Settings->ImageModels.AddDefaulted();
	TestTrue(TEXT("Its one-second check runs again"), FAccess::TickExpiry());
	TestFalse(TEXT("and turns it off for a changed image model"), FHyperAIStudioAutoApproval::IsActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalJobApprovalTest,
	"HyperAIStudio.Chat.AutoApproval.JobApprovalsSaySo",
	HyperAIStudio::AutoApprovalTests::Flags)

bool FHyperAIStudioAutoApprovalJobApprovalTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope;
	const FString Edit = Scope.Add();
	const FString Risky = Scope.Add();
	bool bRan = false;
	TestTrue(TEXT("An edit is held"), Hold(Edit, MakeSummary(EHyperAIStudioDomainSafety::Edit), [Edit, &bRan]()
	{
		bRan = true;
		RecordOutcome(Edit, true);
	}));
	const TOptional<FHyperAIStudioPendingApproval> Plain = FindPending(Edit);
	TestTrue(TEXT("A plain edit is one a running job may approve"), Plain.IsSet() && Plain->IsPlainEdit());
	FString Error;
	TestTrue(TEXT("A running job approves it: ") + Error, FHyperAIStudioApprovalGate::ApproveForJob(Edit, Error) && bRan);
	const TOptional<FHyperAIStudioApprovalRecord> Record = FHyperAIStudioApprovalGate::FindApproval(Edit);
	TestTrue(TEXT("The gate records the job as the approver"), Record.IsSet()
		&& Record->ApprovedBy == EHyperAIStudioApprover::AgentJob && !Record->AutoApprovedAs.IsSet());
	const FHyperAIOperationStatus Status = UHyperAIStudioNativeReadToolset::hyper_operation_status(Edit);
	TestEqual(TEXT("An agent reads that a job approved it, not a person"), Status.ApprovedBy, FString(TEXT("agent_job")));
	const TOptional<FHyperAIStudioActivityEntry> Approved = FindApproval(Edit);
	TestTrue(TEXT("Activity marks it approved for a job"), Approved.IsSet() && Approved->bApprovedForJob
		&& !Approved->AutoApprovedAs.IsSet() && Approved->Detail.StartsWith(TEXT("Approved for a running agent job")));

	TestTrue(TEXT("A destructive request is held"), Hold(Risky, MakeSummary(EHyperAIStudioDomainSafety::Destructive), []() {}));
	const TOptional<FHyperAIStudioPendingApproval> Destructive = FindPending(Risky);
	TestTrue(TEXT("and no running job may approve it"), Destructive.IsSet() && !Destructive->IsPlainEdit());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalScreensTest,
	"HyperAIStudio.Screens.AutoApprovalDialog",
	HyperAIStudio::AutoApprovalTests::Flags)

bool FHyperAIStudioAutoApprovalScreensTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope;
	const FString Waiting = Scope.Add();
	FHyperAIStudioApprovalSummary Summary = MakeSummary(EHyperAIStudioDomainSafety::Edit);
	Summary.ToolName = TEXT("hyper_material_apply_plan");
	Summary.EffectTarget = TEXT("/Game/Materials/M_Rock.M_Rock");
	TestTrue(TEXT("A request waits"), Hold(Waiting, Summary, []() {}));
	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Dialog = FAccess::MakeDialog();
	const FString DialogShot = HyperAIStudio::Tests::SaveWidgetShot(Dialog, FIntPoint(660, 1000), TEXT("auto-approval-dialog"));
	const FString Covered = Scope.Add();
	TestTrue(TEXT("It is on in the banner shot"), FAccess::TurnOn({ECategory::Edits, ECategory::ExternalEffects}, {Covered}));
	Summary.EffectTarget = TEXT("/Game/Maps/Arena.Arena");
	Summary.ToolName = TEXT("hyper_lighting_apply_plan");
	TestTrue(TEXT("A covered request is approved, for the Activity shot"), Hold(Covered, Summary, []() {}));
	FAccess::PumpGate();
	const FString BannerShot = HyperAIStudio::Tests::SaveWidgetShot(SNew(SHyperAIStudioAutoApprovalBanner), FIntPoint(560, 64), TEXT("auto-approval-banner"));
	const FString ActivityShot = HyperAIStudio::Tests::SaveWidgetShot(SNew(SHyperAIStudioChatActivitySidebar), FIntPoint(300, 560), TEXT("auto-approval-activity"));
	if (DialogShot.IsEmpty() || BannerShot.IsEmpty() || ActivityShot.IsEmpty())
	{
		AddInfo(TEXT("This editor cannot render; no screenshots were taken."));
		return true;
	}
	AddInfo(TEXT("Screenshots: ") + DialogShot + TEXT(", ") + BannerShot + TEXT(", ") + ActivityShot);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
