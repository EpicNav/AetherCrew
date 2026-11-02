// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAutoApproval.h"
#include "HyperAIStudioAutoApprovalKeepOn.h"
#include "HyperAIStudioNativeChat.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SHyperAIStudioAutoApprovalDialog.h"
#include "SHyperAIStudioNativeChatView.h"

namespace HyperAIStudio::AutoApprovalCommandTests
{
	using FAccess = FHyperAIStudioAutoApprovalTestAccess;
	using ECategory = EHyperAIStudioApprovalCategory;
	using EDuration = EHyperAIStudioAutoApprovalDuration;
	using HyperAIStudio::AutoApproval::KeepOn::ERefusal;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	const TCHAR* const Command = TEXT("/activate auto-hyper-approval");

	FString NewId()
	{
		return TEXT("command-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	}

	bool HoldEdit(const FString& OperationId)
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = TEXT("level_design");
		Summary.ToolName = TEXT("hyper_auto_approval_command_test");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = TEXT("/Game/Tests/AutoApproval/Command.Command");
		Summary.Effects = {TEXT("test effect")};
		FString Error;
		return FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, []() {}, Error);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalLongDurationsTest,
	"HyperAIStudio.Chat.AutoApproval.Command.LongAndCustomDurations",
	HyperAIStudio::AutoApprovalCommandTests::Flags)

bool FHyperAIStudioAutoApprovalLongDurationsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalCommandTests;
	using namespace HyperAIStudio::AutoApproval;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	ON_SCOPE_EXIT { FAccess::Reset(); };
	TestEqual(TEXT("8 hours"), DurationSeconds(EDuration::EightHours, 3.0), 8.0 * 3600.0);
	TestEqual(TEXT("custom hours"), DurationSeconds(EDuration::Custom, 2.5), 2.5 * 3600.0);
	TestEqual(TEXT("custom never passes the top"), DurationSeconds(EDuration::Custom, 500.0), MaxCustomHours * 3600.0);
	TestEqual(TEXT("or the bottom"), DurationSeconds(EDuration::Custom, -1.0), MinCustomHours * 3600.0);
	TestEqual(TEXT("and not-a-number is the default"), ClampCustomHours(FMath::Sqrt(-1.0)), DefaultCustomHours);

	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Dialog = FAccess::MakeDialog();
	TestEqual(TEXT("The Custom field starts at 10 hours"), FAccess::GetCustomHours(*Dialog), DefaultCustomHours);
	TestEqual(TEXT("without choosing Custom"), FAccess::GetDuration(*Dialog), EDuration::OneHour);
	FAccess::CommitCustomHours(*Dialog, 10.0);
	TestEqual(TEXT("Committing the field chooses Custom"), FAccess::GetDuration(*Dialog), EDuration::Custom);
	TestTrue(TEXT("The native confirmation names the hours"), FAccess::GetConfirmation(*Dialog).ToString().Contains(TEXT("For the next 10 hours")));
	FAccess::CommitCustomHours(*Dialog, 1.0);
	TestTrue(TEXT("in the singular too"), FAccess::GetConfirmation(*Dialog).ToString().Contains(TEXT("For the next 1 hour,")));
	FAccess::CommitCustomHours(*Dialog, 90.0);
	TestEqual(TEXT("Too many hours stop at the most"), FAccess::GetCustomHours(*Dialog), MaxCustomHours);
	FAccess::SetDuration(*Dialog, EDuration::EightHours);
	TestTrue(TEXT("8 hours is named too"), FAccess::GetConfirmation(*Dialog).ToString().Contains(TEXT("For the next 8 hours")));

	// Through the record: a custom time survives a restart, with its hours.
	const FString Path = FPaths::CreateTempFilename(*FPaths::AutomationTransientDir(), TEXT("AutoApprovalCustom"), TEXT(".keepon"));
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Path); };
	FAccess::SetRecordPath(Path);
	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Timed = FAccess::MakeDialog();
	FAccess::CommitCustomHours(*Timed, 10.0);
	FAccess::SetStayOn(*Timed, true);
	FAccess::SetNativeAnswer(*Timed, true);
	const TSet<FString> Ids = {NewId()};
	FAccess::PressTurnOn(*Timed, Ids);
	FHyperAIStudioAutoApprovalStatus Status = FHyperAIStudioAutoApproval::GetStatus();
	TestTrue(TEXT("It turns on for those hours"), Status.bActive && Status.Duration == EDuration::Custom && Status.CustomHours == 10.0
		&& Status.SecondsLeft > 9.9 * 3600.0 && Status.SecondsLeft <= 10.0 * 3600.0);
	FAccess::CloseEditor();
	FAccess::Reset();
	FAccess::SetRecordPath(Path);
	TestTrue(TEXT("and comes back after a restart"), FAccess::Restore(Path, Ids) == ERefusal::None);
	Status = FHyperAIStudioAutoApproval::GetStatus();
	TestTrue(TEXT("with the hours and the time left"), Status.bActive && Status.Duration == EDuration::Custom && Status.CustomHours == 10.0
		&& Status.SecondsLeft > 9.9 * 3600.0 && Status.SecondsLeft <= 10.0 * 3600.0);

	namespace KeepOn = HyperAIStudio::AutoApproval::KeepOn;
	EDuration Parsed = EDuration::OneHour;
	TestTrue(TEXT("The new durations have names"), KeepOn::ParseDuration(TEXT("eight_hours"), Parsed) && Parsed == EDuration::EightHours
		&& KeepOn::ParseDuration(TEXT("custom"), Parsed) && Parsed == EDuration::Custom);

	// The saved choices keep the field's hours, and clamp what someone wrote by hand.
	const FString Ini = GEditorPerProjectIni;
	const TCHAR* Section = FHyperAIStudioAutoApproval::GetChoicesSection();
	const bool bHadSection = GConfig->DoesSectionExist(Section, Ini);
	const FHyperAIStudioAutoApprovalChoices Before = FHyperAIStudioAutoApproval::ReadChoices(Ini);
	ON_SCOPE_EXIT
	{
		if (bHadSection)
		{
			FHyperAIStudioAutoApproval::WriteChoices(Ini, Before);
		}
		else
		{
			GConfig->EmptySection(Section, Ini);
			GConfig->Flush(false, Ini);
		}
	};
	FHyperAIStudioAutoApprovalChoices Written;
	Written.Duration = EDuration::Custom;
	Written.CustomHours = 6.5;
	FHyperAIStudioAutoApproval::WriteChoices(Ini, Written);
	TestTrue(TEXT("Choices read back as written"), FHyperAIStudioAutoApproval::ReadChoices(Ini) == Written);
	GConfig->SetDouble(Section, TEXT("CustomHours"), 1000.0, Ini);
	TestEqual(TEXT("Hours past the limit are clamped"), FHyperAIStudioAutoApproval::ReadChoices(Ini).CustomHours, MaxCustomHours);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalTypedCommandTest,
	"HyperAIStudio.Chat.AutoApproval.Command.TypedCommand",
	HyperAIStudio::AutoApprovalCommandTests::Flags)

bool FHyperAIStudioAutoApprovalTypedCommandTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalCommandTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	ON_SCOPE_EXIT { FAccess::Reset(); };
	TestTrue(TEXT("The command is recognized"), FHyperAIStudioAutoApproval::IsTypedCommand(Command));
	TestTrue(TEXT("in any case and spacing"), FHyperAIStudioAutoApproval::IsTypedCommand(TEXT("  /Activate   AUTO-hyper-approval  8h ")));
	TestFalse(TEXT("Other words are not it"), FHyperAIStudioAutoApproval::IsTypedCommand(TEXT("/activate something")));
	TestFalse(TEXT("nor a sentence that mentions it"), FHyperAIStudioAutoApproval::IsTypedCommand(TEXT("what does /activate auto-hyper-approval do?")));

	FText Message;
	const TSet<FString> Nobody;
	TestFalse(TEXT("Without the real Enter key it refuses"), FAccess::RunCommand(Command, false, Nobody, Message));
	TestFalse(TEXT("and stays off"), FHyperAIStudioAutoApproval::IsActive());
	TestTrue(TEXT("saying why: ") + Message.ToString(), Message.ToString().Contains(TEXT("Enter key")));
	for (const TCHAR* Bad : {TEXT("/activate auto-hyper-approval forever"), TEXT("/activate auto-hyper-approval 25"),
		TEXT("/activate auto-hyper-approval 0"), TEXT("/activate auto-hyper-approval -3"), TEXT("/activate auto-hyper-approval 8h please")})
	{
		TestFalse(FString(TEXT("Refused: ")) + Bad, FAccess::RunCommand(Bad, true, Nobody, Message) || FHyperAIStudioAutoApproval::IsActive());
		TestTrue(TEXT("naming the hours: ") + Message.ToString(), Message.ToString().Contains(TEXT("hours")));
	}

	TestTrue(TEXT("With the real Enter key it turns on"), FAccess::RunCommand(Command, true, Nobody, Message));
	FHyperAIStudioAutoApprovalStatus Status = FHyperAIStudioAutoApproval::GetStatus();
	TestTrue(TEXT("with the last choices, here the defaults: Edits, for an hour"), Status.bActive
		&& Status.Categories == TArray<ECategory>({ECategory::Edits}) && Status.Duration == EDuration::OneHour);
	TestTrue(TEXT("and says so: ") + Message.ToString(), Message.ToString().Contains(TEXT("for 1 hour")) && Message.ToString().Contains(TEXT("Edits")));

	TestTrue(TEXT("Naming hours sets the time"), FAccess::RunCommand(TEXT("/activate auto-hyper-approval 8h"), true, Nobody, Message));
	Status = FHyperAIStudioAutoApproval::GetStatus();
	TestTrue(TEXT("to those hours"), Status.Duration == EDuration::Custom && Status.CustomHours == 8.0 && Status.SecondsLeft > 7.9 * 3600.0);
	TestTrue(TEXT("and says so: ") + Message.ToString(), Message.ToString().Contains(TEXT("for 8 hours")));
	FAccess::Reset();

	// What already waits is approved as the dialog would, since the default choice is to approve it.
	const FString Id = NewId();
	TestTrue(TEXT("A request waits"), HoldEdit(Id) && FHyperAIStudioApprovalGate::HasPending(Id));
	ON_SCOPE_EXIT
	{
		if (FHyperAIStudioApprovalGate::HasPending(Id))
		{
			FHyperAIStudioApprovalGate::Reject(Id, TEXT("test finished"));
		}
	};
	TestTrue(TEXT("The command turns on"), FAccess::RunCommand(Command, true, {Id}, Message));
	FAccess::PumpGate();
	TestFalse(TEXT("and approves it"), FHyperAIStudioApprovalGate::HasPending(Id));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalCommandNeverReachesAgentTest,
	"HyperAIStudio.Chat.AutoApproval.Command.NeverReachesAgent",
	HyperAIStudio::AutoApprovalCommandTests::Flags)

bool FHyperAIStudioAutoApprovalCommandNeverReachesAgentTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalCommandTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	ON_SCOPE_EXIT { FAccess::Reset(); };
	const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("Claude Code"), FPaths::AutomationTransientDir(), FString(), FString());
	if (!TestTrue(TEXT("A chat that never starts an agent"), Chat.IsValid()))
	{
		return false;
	}
	const TSharedRef<SHyperAIStudioNativeChatView> View = SNew(SHyperAIStudioNativeChatView).Chat(Chat);
	// The Send button: what a client synthesizing Slate input could press. The Enter key's real state is never read here.
	View->SubmitForTest(Command);
	TestFalse(TEXT("The Send button does not turn it on"), FHyperAIStudioAutoApproval::IsActive());
	TestEqual(TEXT("nothing is queued for the agent"), Chat->NumQueued(), 0);
	TestFalse(TEXT("and the transcript does not hold it"), Chat->GetItems().ContainsByPredicate([](const TSharedPtr<FHyperAIStudioChatItem>& Item)
	{
		return Item.IsValid() && Item->Kind == FHyperAIStudioChatItem::EKind::User;
	}));
	TestTrue(TEXT("The notice says to press Enter: ") + View->GetNotice(), View->GetNotice().Contains(TEXT("Enter key")));
	TestEqual(TEXT("and the text stays to press it with"), View->GetComposerText(), FString(Command));
	return true;
}

#endif
