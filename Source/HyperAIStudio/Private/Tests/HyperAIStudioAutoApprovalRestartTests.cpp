// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAutoApproval.h"
#include "HyperAIStudioAutoApprovalKeepOn.h"
#include "HyperAIStudioSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HyperAIStudioWidgetShot.h"
#include "SHyperAIStudioActivitySidebar.h"
#include "SHyperAIStudioAutoApprovalDialog.h"
#include "Tests/HyperAIStudioLogCapture.h"

namespace HyperAIStudio::AutoApprovalRestartTests
{
	using FAccess = FHyperAIStudioAutoApprovalTestAccess;
	using ECategory = EHyperAIStudioApprovalCategory;
	using EDuration = EHyperAIStudioAutoApprovalDuration;
	using HyperAIStudio::AutoApproval::KeepOn::ERefusal;
	using HyperAIStudio::Tests::FLogCapture;
	namespace KeepOn = HyperAIStudio::AutoApproval::KeepOn;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString NewId()
	{
		return TEXT("keepon-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	}

	FHyperAIStudioApprovalSummary SummaryFor(const ECategory Category)
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = TEXT("level_design");
		Summary.ToolName = TEXT("hyper_auto_approval_restart_test");
		Summary.Safety = Category == ECategory::ExternalEffects ? EHyperAIStudioDomainSafety::ExternalEffect : EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = TEXT("/Game/Tests/AutoApproval/Restart.Restart");
		Summary.Effects = {TEXT("test effect")};
		return Summary;
	}

	void RecordOutcome(const FString& OperationId, const bool bCompleted)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = bCompleted ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = TEXT("level_design");
		Entry.ToolName = TEXT("hyper_auto_approval_restart_test");
		Entry.OperationId = OperationId;
		Entry.StatusCode = bCompleted ? TEXT("completed") : TEXT("failed");
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	bool Hold(const FString& OperationId, const ECategory Category, TFunction<void()> Run)
	{
		FString Error;
		return FHyperAIStudioApprovalGate::RequestAction(OperationId, SummaryFor(Category), MoveTemp(Run), Error);
	}

	/** Holds a request in Category whose action finishes as bCompleted says, and lets the gate's sweep approve it. */
	bool Approve(const FString& OperationId, const ECategory Category, const bool bCompleted = true)
	{
		Hold(OperationId, Category, [OperationId, bCompleted]() { RecordOutcome(OperationId, bCompleted); });
		FAccess::PumpGate();
		return !FHyperAIStudioApprovalGate::HasPending(OperationId);
	}

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

	KeepOn::FContext ContextNow()
	{
		KeepOn::FContext Context;
		Context.NowUtc = FAccess::GetUtcNow();
		Context.ProjectPath = KeepOn::CurrentProject();
		Context.WindowsUser = KeepOn::CurrentUser();
		Context.SpendFingerprint = FAccess::GetSpendFingerprint();
		return Context;
	}

	/** A valid record for this editor, as one turned on Ago seconds before now would have written. */
	KeepOn::FRecord MakeRecord(const EDuration Duration, const double Ago)
	{
		const KeepOn::FContext Context = ContextNow();
		KeepOn::FRecord Record;
		Record.CategoryMask = 1u << static_cast<uint8>(ECategory::Edits);
		Record.Duration = Duration;
		Record.TurnedOnUtc = Context.NowUtc - FTimespan::FromSeconds(Ago);
		if (Duration == EDuration::OneHour)
		{
			Record.ExpiresUtc = Record.TurnedOnUtc + FTimespan::FromHours(1.0);
		}
		else if (Duration == EDuration::FourHours)
		{
			Record.ExpiresUtc = Record.TurnedOnUtc + FTimespan::FromHours(4.0);
		}
		else if (Duration == EDuration::FifteenMinutes)
		{
			Record.ExpiresUtc = Record.TurnedOnUtc + FTimespan::FromMinutes(15.0);
		}
		Record.SpendFingerprint = Context.SpendFingerprint;
		Record.ProjectPath = Context.ProjectPath;
		Record.WindowsUser = Context.WindowsUser;
		Record.WrittenUtc = Context.NowUtc;
		return Record;
	}

	/**
	 * Every test starts and ends off, with its own record path under the bench's Saved/HyperAIStudio/Tests; the user's
	 * own record (KeepOn::DefaultPath) is never written, read or deleted by a test, which the scope checks.
	 */
	struct FScope
	{
		FString Path;
		TArray<FString> Ids;
		bool bUserRecordBefore = false;
		FDateTime UserRecordStamp;
		FAutomationTestBase& Test;

		explicit FScope(FAutomationTestBase& InTest)
			: Test(InTest)
		{
			FAccess::Reset();
			FHyperAIStudioAgentScoreboard::SetStoragePathForTests(
				FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests/AgentScoreboard.json")));
			Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests/AutoApproval") / (NewId() + TEXT(".keepon")));
			FAccess::SetRecordPath(Path);
			bUserRecordBefore = IFileManager::Get().FileExists(*KeepOn::DefaultPath());
			UserRecordStamp = IFileManager::Get().GetTimeStamp(*KeepOn::DefaultPath());
		}

		~FScope()
		{
			FAccess::Reset();
			for (const FString& Id : Ids)
			{
				FHyperAIStudioApprovalGate::Reject(Id, TEXT("test over"));
			}
			IFileManager::Get().Delete(*Path, false, true, true);
			FHyperAIStudioAgentScoreboard::SetStoragePathForTests(FString());
			Test.TestTrue(TEXT("The user's own record was left as it was"),
				IFileManager::Get().FileExists(*KeepOn::DefaultPath()) == bUserRecordBefore
				&& IFileManager::Get().GetTimeStamp(*KeepOn::DefaultPath()) == UserRecordStamp);
		}

		FString Add()
		{
			return Ids.Add_GetRef(NewId());
		}

		TSet<FString> All() const
		{
			return TSet<FString>(Ids);
		}

		bool HasRecord() const
		{
			return IFileManager::Get().FileExists(*Path);
		}

		KeepOn::FRecord ReadRecord(ERefusal* OutRefusal = nullptr) const
		{
			KeepOn::FRecord Record;
			const ERefusal Refusal = KeepOn::Read(Path, ContextNow(), Record);
			if (OutRefusal)
			{
				*OutRefusal = Refusal;
			}
			return Record;
		}
	};

	/** Writes Text as the record file, sealed with this project's DPAPI entropy as HyperAI seals one. */
	bool SealText(const FString& Path, const FString& Text, const FString& ProjectPath)
	{
		const FTCHARToUTF8 Utf8(*Text);
		TArray<uint8> Plain;
		Plain.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		TArray<uint8> Sealed;
		FString Error;
		return KeepOn::Protect(Plain, ProjectPath, Sealed, Error) && FFileHelper::SaveArrayToFile(Sealed, *Path);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalRecordRoundTripTest,
	"HyperAIStudio.Chat.AutoApproval.Restart.RecordRoundTrip",
	HyperAIStudio::AutoApprovalRestartTests::Flags)

bool FHyperAIStudioAutoApprovalRecordRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalRestartTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope(*this);
	KeepOn::FRecord Record = MakeRecord(EDuration::FourHours, 600.0);
	Record.CategoryMask = (1u << static_cast<uint8>(ECategory::Edits)) | (1u << static_cast<uint8>(ECategory::ExternalEffects))
		| (1u << static_cast<uint8>(ECategory::SandboxSaves)) | (1u << static_cast<uint8>(ECategory::Destructive));
	Record.Approved = 12;
	Record.ExternalApproved = 3;
	Record.ConsecutiveFailures = 2;
	Record.SandboxWrites = 4;
	Record.ClosedUtc = Record.WrittenUtc;

	KeepOn::FRecord Parsed;
	TestTrue(TEXT("Its JSON reads back"), KeepOn::FromJson(KeepOn::ToJson(Record), Parsed));
	auto Same = [](const KeepOn::FRecord& A, const KeepOn::FRecord& B)
	{
		auto Ms = [](const FDateTime& Time) { return Time.GetTicks() / ETimespan::TicksPerMillisecond; };
		return A.CategoryMask == B.CategoryMask && A.Duration == B.Duration && Ms(A.TurnedOnUtc) == Ms(B.TurnedOnUtc)
			&& A.ExpiresUtc.IsSet() == B.ExpiresUtc.IsSet() && (!A.ExpiresUtc.IsSet() || Ms(*A.ExpiresUtc) == Ms(*B.ExpiresUtc))
			&& A.Approved == B.Approved && A.ExternalApproved == B.ExternalApproved && A.ConsecutiveFailures == B.ConsecutiveFailures
			&& A.SandboxWrites == B.SandboxWrites && A.SpendFingerprint == B.SpendFingerprint && A.ProjectPath == B.ProjectPath
			&& A.WindowsUser == B.WindowsUser && Ms(A.WrittenUtc) == Ms(B.WrittenUtc)
			&& A.ClosedUtc.IsSet() == B.ClosedUtc.IsSet() && (!A.ClosedUtc.IsSet() || Ms(*A.ClosedUtc) == Ms(*B.ClosedUtc));
	};
	TestTrue(TEXT("with every field: categories, duration, times, counts, fingerprint, project and user"), Same(Record, Parsed));
	TestTrue(TEXT("It holds the spend settings only as a fingerprint"), Record.SpendFingerprint.Len() == 40);

	FString Error;
	TestTrue(TEXT("It is written sealed"), KeepOn::Write(Scope.Path, Record, Error) && Error.IsEmpty());
	FString OnDisk;
	FFileHelper::LoadFileToString(OnDisk, *Scope.Path);
	TArray<uint8> Bytes;
	FFileHelper::LoadFileToArray(Bytes, *Scope.Path);
	const FTCHARToUTF8 Project(*Record.ProjectPath);
	bool bPlainOnDisk = false;
	for (int32 Index = 0; Index + Project.Length() <= Bytes.Num(); ++Index)
	{
		bPlainOnDisk |= FMemory::Memcmp(Bytes.GetData() + Index, Project.Get(), Project.Length()) == 0;
	}
	TestFalse(TEXT("so the file shows neither its JSON nor the project path in plain text"), OnDisk.Contains(TEXT("turnedOnUtc")) || bPlainOnDisk);
	ERefusal Refusal = ERefusal::NoRecord;
	const KeepOn::FRecord Read = Scope.ReadRecord(&Refusal);
	TestEqual(TEXT("and reads back, valid for this user and project"), static_cast<int32>(Refusal), static_cast<int32>(ERefusal::None));
	TestTrue(TEXT("unchanged"), Same(Record, Read));

	// "Until the editor closes" has no expiry; after a crash it has no close time either.
	KeepOn::FRecord UntilClose = MakeRecord(EDuration::UntilEditorCloses, 60.0);
	TestTrue(TEXT("An until-close record is written"), KeepOn::Write(Scope.Path, UntilClose, Error));
	const KeepOn::FRecord ReadUntilClose = Scope.ReadRecord(&Refusal);
	TestTrue(TEXT("and reads back with no expiry and no close time"), Refusal == ERefusal::None && !ReadUntilClose.ExpiresUtc.IsSet()
		&& !ReadUntilClose.ClosedUtc.IsSet() && ReadUntilClose.Duration == EDuration::UntilEditorCloses);

	// The record of a real activation, as the dialog writes it.
	TestTrue(TEXT("Turned on with Stay on ticked"), FAccess::TurnOn({ECategory::Edits, ECategory::Scripts}, Scope.All(), EDuration::FifteenMinutes, false, true));
	const KeepOn::FRecord Live = Scope.ReadRecord(&Refusal);
	TestTrue(TEXT("it writes its record at once"), Refusal == ERefusal::None && Live.Duration == EDuration::FifteenMinutes
		&& Live.CategoryMask == ((1u << static_cast<uint8>(ECategory::Edits)) | (1u << static_cast<uint8>(ECategory::Scripts)))
		&& Live.ExpiresUtc.IsSet() && FMath::IsNearlyEqual((*Live.ExpiresUtc - Live.TurnedOnUtc).GetTotalMinutes(), 15.0, 0.01)
		&& Live.Approved == 0 && !Live.ClosedUtc.IsSet());
	TestTrue(TEXT("and its status says it stays on after a restart"), FHyperAIStudioAutoApproval::GetStatus().bStaysOnAfterRestart
		&& !FHyperAIStudioAutoApproval::GetStatus().bRestored);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalRecordTamperTest,
	"HyperAIStudio.Chat.AutoApproval.Restart.TamperedRecordsAreRefused",
	HyperAIStudio::AutoApprovalRestartTests::Flags)

bool FHyperAIStudioAutoApprovalRecordTamperTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalRestartTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope(*this);
	FLogCapture Log(TEXT("LogHyperAIStudioAutoApproval"));
	const KeepOn::FRecord Record = MakeRecord(EDuration::OneHour, 60.0);
	FString Error;
	auto Refusal = [&Scope]() { ERefusal Out = ERefusal::NoRecord; Scope.ReadRecord(&Out); return Out; };

	TestTrue(TEXT("A valid record is written"), KeepOn::Write(Scope.Path, Record, Error));
	TArray<uint8> Bytes;
	FFileHelper::LoadFileToArray(Bytes, *Scope.Path);
	TestTrue(TEXT("and is valid"), Refusal() == ERefusal::None);
	// One flipped byte anywhere in the sealed data, as an edit by hand or by another program makes it.
	for (const int32 Index : {Bytes.Num() / 2, Bytes.Num() - 1})
	{
		TArray<uint8> Flipped = Bytes;
		Flipped[Index] ^= 0x01;
		FFileHelper::SaveArrayToFile(Flipped, *Scope.Path);
		TestTrue(FString::Printf(TEXT("A byte flipped at %d no longer decrypts"), Index), Refusal() == ERefusal::DoesNotDecrypt);
	}
	TestEqual(TEXT("Restoring it is refused"), static_cast<int32>(FAccess::Restore(Scope.Path, Scope.All())), static_cast<int32>(ERefusal::DoesNotDecrypt));
	TestFalse(TEXT("auto-approval stays off"), FHyperAIStudioAutoApproval::IsActive());
	TestFalse(TEXT("and the record is deleted"), Scope.HasRecord());
	TestTrue(TEXT("and the log says so"), Log.Count(TEXT("was refused and deleted")) >= 1);
	const TOptional<FText> Notice = FAccess::GetLastNotice();
	TestTrue(TEXT("and the notification says it was refused: ") + (Notice.IsSet() ? Notice->ToString() : FString()),
		Notice.IsSet() && Notice->ToString().Contains(TEXT("refused and deleted")));

	// The JSON a plain file write would put there, never sealed.
	FFileHelper::SaveStringToFile(KeepOn::ToJson(Record), *Scope.Path);
	TestTrue(TEXT("A plain JSON file does not decrypt"), Refusal() == ERefusal::DoesNotDecrypt);
	TestTrue(TEXT("and is refused and deleted"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::DoesNotDecrypt && !Scope.HasRecord()
		&& !FHyperAIStudioAutoApproval::IsActive());

	FFileHelper::SaveStringToFile(FString(), *Scope.Path);
	TestTrue(TEXT("An empty file is refused as unreadable"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::Unreadable && !Scope.HasRecord());

	TestTrue(TEXT("Sealed text that is no record"), SealText(Scope.Path, TEXT("{\"format\":\"something else\"}"), KeepOn::CurrentProject()));
	TestTrue(TEXT("is refused as unparsable"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::Unparsable && !Scope.HasRecord());

	// The fields contradicting each other: an expiry 10 hours after a one-hour activation began.
	KeepOn::FRecord Stretched = Record;
	Stretched.ExpiresUtc = Record.TurnedOnUtc + FTimespan::FromHours(10.0);
	TestTrue(TEXT("A record whose expiry its duration cannot give is written"), KeepOn::Write(Scope.Path, Stretched, Error));
	TestTrue(TEXT("and refused as inconsistent"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::Inconsistent && !Scope.HasRecord());
	KeepOn::FRecord NoCategory = Record;
	NoCategory.CategoryMask = 0;
	TestTrue(TEXT("So is one with no category"), KeepOn::Check(NoCategory, ContextNow()) == ERefusal::Inconsistent);
	KeepOn::FRecord Negative = Record;
	Negative.Approved = -40;
	TestTrue(TEXT("or a negative count"), KeepOn::Check(Negative, ContextNow()) == ERefusal::Inconsistent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalRecordOwnerTest,
	"HyperAIStudio.Chat.AutoApproval.Restart.WrongProjectOrUserIsRefused",
	HyperAIStudio::AutoApprovalRestartTests::Flags)

bool FHyperAIStudioAutoApprovalRecordOwnerTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalRestartTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope(*this);
	FString Error;
	const FString OtherProject = TEXT("e:/unreal projects/someoneelse/someoneelse.uproject");

	// Sealed for another project: its entropy differs, so DPAPI itself refuses it here.
	KeepOn::FRecord Elsewhere = MakeRecord(EDuration::OneHour, 60.0);
	Elsewhere.ProjectPath = OtherProject;
	TestTrue(TEXT("A record sealed for another project is written"), KeepOn::Write(Scope.Path, Elsewhere, Error));
	TestTrue(TEXT("and does not decrypt for this one"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::DoesNotDecrypt
		&& !Scope.HasRecord() && !FHyperAIStudioAutoApproval::IsActive());

	// Sealed with this project's entropy but naming another project inside.
	TestTrue(TEXT("A record naming another project inside"), SealText(Scope.Path, KeepOn::ToJson(Elsewhere), KeepOn::CurrentProject()));
	TestTrue(TEXT("is refused for the wrong project"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::WrongProject
		&& !Scope.HasRecord() && !FHyperAIStudioAutoApproval::IsActive());

	KeepOn::FRecord OtherUser = MakeRecord(EDuration::OneHour, 60.0);
	OtherUser.WindowsUser = TEXT("someone-else");
	TestTrue(TEXT("A record naming another Windows user"), KeepOn::Write(Scope.Path, OtherUser, Error));
	TestTrue(TEXT("is refused for the wrong user"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::WrongUser
		&& !Scope.HasRecord() && !FHyperAIStudioAutoApproval::IsActive());

	// The user and project compare as Windows compares them: without case.
	KeepOn::FRecord Upper = MakeRecord(EDuration::OneHour, 60.0);
	Upper.WindowsUser = Upper.WindowsUser.ToUpper();
	TestTrue(TEXT("The same user in other case is this user"), KeepOn::Check(Upper, ContextNow()) == ERefusal::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalRestartExpiryTest,
	"HyperAIStudio.Chat.AutoApproval.Restart.ExpiryCountsTimeClosed",
	HyperAIStudio::AutoApprovalRestartTests::Flags)

bool FHyperAIStudioAutoApprovalRestartExpiryTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalRestartTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope(*this);
	TestTrue(TEXT("It turns on for an hour, kept on"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::OneHour, false, true));
	FAccess::AdvanceClock(10.0 * 60.0);
	FAccess::CloseEditor();
	TestFalse(TEXT("The editor closing turns it off"), FHyperAIStudioAutoApproval::IsActive());
	TestTrue(TEXT("as the editor closing"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::EditorClosing);
	ERefusal Refusal = ERefusal::NoRecord;
	const KeepOn::FRecord Closed = Scope.ReadRecord(&Refusal);
	TestTrue(TEXT("but its record stays, with the close time"), Scope.HasRecord() && Refusal == ERefusal::None && Closed.ClosedUtc.IsSet());

	// 30 minutes closed: 20 of its 60 minutes are left, not 50.
	FAccess::AdvanceClock(30.0 * 60.0);
	TestTrue(TEXT("Opened again 30 minutes later, it comes back"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::None);
	const FHyperAIStudioAutoApprovalStatus Status = FHyperAIStudioAutoApproval::GetStatus();
	TestTrue(TEXT("on, and marked as restored"), Status.bActive && Status.bRestored && Status.bStaysOnAfterRestart);
	TestTrue(FString::Printf(TEXT("with the time the editor was closed counted: %.0f s left"), Status.SecondsLeft),
		Status.SecondsLeft > 19.0 * 60.0 && Status.SecondsLeft <= 20.0 * 60.0 + 1.0);
	TestTrue(TEXT("Its record loses the close time while it is open again"), !Scope.ReadRecord().ClosedUtc.IsSet());

	FAccess::CloseEditor();
	FAccess::AdvanceClock(21.0 * 60.0);
	FLogCapture Log(TEXT("LogHyperAIStudioAutoApproval"));
	TestTrue(TEXT("Opened again after its end, it is refused as expired"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::Expired);
	TestFalse(TEXT("stays off"), FHyperAIStudioAutoApproval::IsActive());
	TestFalse(TEXT("and its record is deleted"), Scope.HasRecord());
	TestTrue(TEXT("The log says why"), Log.Count(TEXT("did not come back")) >= 1 && Log.Count(TEXT("ran out")) >= 1);
	const TOptional<FText> Notice = FAccess::GetLastNotice();
	TestTrue(TEXT("and so does the notification: ") + (Notice.IsSet() ? Notice->ToString() : FString()),
		Notice.IsSet() && Notice->ToString().Contains(TEXT("did not come back on")) && Notice->ToString().Contains(TEXT("time ran out")));

	// The rule itself, at its edge.
	const KeepOn::FRecord Edge = MakeRecord(EDuration::FifteenMinutes, 15.0 * 60.0);
	TestTrue(TEXT("A timed record whose end is now is expired"), KeepOn::Check(Edge, ContextNow()) == ERefusal::Expired);
	const KeepOn::FRecord Inside = MakeRecord(EDuration::FifteenMinutes, 14.0 * 60.0);
	TestTrue(TEXT("one a minute short of it is not"), KeepOn::Check(Inside, ContextNow()) == ERefusal::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalRestartCapTest,
	"HyperAIStudio.Chat.AutoApproval.Restart.TwentyFourHourCap",
	HyperAIStudio::AutoApprovalRestartTests::Flags)

bool FHyperAIStudioAutoApprovalRestartCapTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalRestartTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope(*this);
	KeepOn::FRecord Old = MakeRecord(EDuration::UntilEditorCloses, 24.0 * 3600.0 + 1.0);
	Old.ClosedUtc = ContextNow().NowUtc - FTimespan::FromMinutes(1.0);
	Old.WrittenUtc = *Old.ClosedUtc;
	TestTrue(TEXT("Closed a minute ago but turned on over 24 hours ago, it is refused"), KeepOn::Check(Old, ContextNow()) == ERefusal::TooOld);
	KeepOn::FRecord Young = MakeRecord(EDuration::UntilEditorCloses, 24.0 * 3600.0 - 60.0);
	Young.ClosedUtc = ContextNow().NowUtc - FTimespan::FromMinutes(1.0);
	Young.WrittenUtc = *Young.ClosedUtc;
	TestTrue(TEXT("a minute under 24 hours, it is not"), KeepOn::Check(Young, ContextNow()) == ERefusal::None);

	// The cap counts from when the user turned it on, across every restart in between.
	TestTrue(TEXT("It turns on until the editor closes, kept on"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::UntilEditorCloses, false, true));
	FAccess::CloseEditor();
	FAccess::AdvanceClock(10.0 * 60.0);
	TestTrue(TEXT("It comes back after a restart"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::None && FHyperAIStudioAutoApproval::IsActive());
	FAccess::AdvanceClock(23.0 * 3600.0 + 50.0 * 60.0);
	FAccess::CloseEditor();
	FAccess::AdvanceClock(2.0 * 60.0);
	TestTrue(TEXT("but not a day after it was first turned on, however soon the editor opens again"),
		FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::TooOld && !FHyperAIStudioAutoApproval::IsActive() && !Scope.HasRecord());

	// A clock set back: times later than now are refused rather than trusted.
	KeepOn::FRecord Future = MakeRecord(EDuration::OneHour, -2.0 * 3600.0);
	Future.WrittenUtc = Future.TurnedOnUtc;
	TestTrue(TEXT("A record turned on hours from now is refused"), KeepOn::Check(Future, ContextNow()) == ERefusal::FromTheFuture);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalRestartCloseWindowTest,
	"HyperAIStudio.Chat.AutoApproval.Restart.UntilCloseNeedsQuickReopen",
	HyperAIStudio::AutoApprovalRestartTests::Flags)

bool FHyperAIStudioAutoApprovalRestartCloseWindowTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalRestartTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope(*this);
	TestEqual(TEXT("The window is the restart's own chat-resume window"), KeepOn::CloseWindowSeconds(), 30.0 * 60.0);
	TestTrue(TEXT("It turns on until the editor closes, kept on"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::UntilEditorCloses, false, true));
	FAccess::CloseEditor();
	FAccess::AdvanceClock(29.0 * 60.0);
	TestTrue(TEXT("Opened again 29 minutes after closing, it comes back"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::None
		&& FHyperAIStudioAutoApproval::IsActive());
	TestTrue(TEXT("still until the editor closes"), FHyperAIStudioAutoApproval::GetStatus().SecondsLeft < 0.0);
	FAccess::CloseEditor();
	FAccess::AdvanceClock(31.0 * 60.0);
	TestTrue(TEXT("Opened again 31 minutes after closing, it is refused"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::ClosedTooLong
		&& !FHyperAIStudioAutoApproval::IsActive() && !Scope.HasRecord());

	// After a crash nothing stamps the close; the record's last write stands for it.
	FAccess::Reset();
	FAccess::SetRecordPath(Scope.Path);
	TestTrue(TEXT("It turns on again"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::UntilEditorCloses, false, true));
	TArray<uint8> Crashed;
	FFileHelper::LoadFileToArray(Crashed, *Scope.Path);
	FAccess::Reset();
	TestFalse(TEXT("Ended any other way than the editor closing, its record is deleted"), Scope.HasRecord());
	// What a crash leaves: the record as last written, with no close time.
	FFileHelper::SaveArrayToFile(Crashed, *Scope.Path);
	FAccess::SetRecordPath(Scope.Path);
	FAccess::AdvanceClock(10.0 * 60.0);
	TestTrue(TEXT("A crash's record written 10 minutes ago comes back"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::None);
	FAccess::Reset();
	FFileHelper::SaveArrayToFile(Crashed, *Scope.Path);
	FAccess::SetRecordPath(Scope.Path);
	FAccess::AdvanceClock(40.0 * 60.0);
	TestTrue(TEXT("one written 40 minutes ago does not"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::ClosedTooLong && !Scope.HasRecord());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalRestartCountersTest,
	"HyperAIStudio.Chat.AutoApproval.Restart.LimitsCountAcrossRestarts",
	HyperAIStudio::AutoApprovalRestartTests::Flags)

bool FHyperAIStudioAutoApprovalRestartCountersTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalRestartTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	{
		FScope Scope(*this);
		for (int32 Index = 0; Index <= FHyperAIStudioAutoApproval::GetMaxApprovals(); ++Index)
		{
			Scope.Add();
		}
		TestTrue(TEXT("Approvals: it turns on, kept on"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::FourHours, false, true));
		int32 Next = 0;
		for (; Next < 30; ++Next)
		{
			Approve(Scope.Ids[Next], ECategory::Edits);
		}
		TestEqual(TEXT("Its record counts 30 approvals"), Scope.ReadRecord().Approved, 30);
		FAccess::CloseEditor();
		TestTrue(TEXT("It comes back after the restart"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::None);
		TestEqual(TEXT("with its 30 approvals"), FHyperAIStudioAutoApproval::GetStatus().Approved, 30);
		int32 Approved = 0;
		for (; Next < FHyperAIStudioAutoApproval::GetMaxApprovals() - 1; ++Next)
		{
			Approved += Approve(Scope.Ids[Next], ECategory::Edits) ? 1 : 0;
		}
		TestEqual(TEXT("It approves 19 more"), Approved, 19);
		TestTrue(TEXT("and is still on at 49"), FHyperAIStudioAutoApproval::IsActive());
		TestTrue(TEXT("The 50th in all is approved"), Approve(Scope.Ids[Next++], ECategory::Edits));
		TestFalse(TEXT("and turns it off, at the same total as without the restart"), FHyperAIStudioAutoApproval::IsActive());
		TestTrue(TEXT("for its limit"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::ApprovalLimit);
		TestFalse(TEXT("which deletes its record"), Scope.HasRecord());
		TestFalse(TEXT("The next request waits"), Approve(Scope.Ids[Next], ECategory::Edits));
	}
	{
		FScope Scope(*this);
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Scope.Add();
		}
		TestTrue(TEXT("Failures: it turns on, kept on"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::OneHour, false, true));
		Approve(Scope.Ids[0], ECategory::Edits, false);
		Approve(Scope.Ids[1], ECategory::Edits, false);
		TestEqual(TEXT("Its record counts two failures in a row"), Scope.ReadRecord().ConsecutiveFailures, 2);
		FAccess::CloseEditor();
		TestTrue(TEXT("It comes back"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::None);
		Approve(Scope.Ids[2], ECategory::Edits, false);
		TestFalse(TEXT("and the third failure in a row turns it off"), FHyperAIStudioAutoApproval::IsActive());
		TestTrue(TEXT("for that reason"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::RepeatedFailures);
		TestFalse(TEXT("deleting its record"), Scope.HasRecord());
	}
	{
		FScope Scope(*this);
		for (int32 Index = 0; Index < FHyperAIStudioAutoApproval::MaxExternalEffects; ++Index)
		{
			Scope.Add();
		}
		TestTrue(TEXT("External effects: it turns on, kept on"), FAccess::TurnOn({ECategory::ExternalEffects}, Scope.All(), EDuration::OneHour, false, true));
		for (int32 Index = 0; Index < FHyperAIStudioAutoApproval::MaxExternalEffects - 1; ++Index)
		{
			Approve(Scope.Ids[Index], ECategory::ExternalEffects);
		}
		const KeepOn::FRecord Record = Scope.ReadRecord();
		TestTrue(TEXT("Its record counts nine external effects"), Record.ExternalApproved == 9 && Record.Approved == 9);
		FAccess::CloseEditor();
		TestTrue(TEXT("It comes back"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::None);
		Approve(Scope.Ids.Last(), ECategory::ExternalEffects);
		TestFalse(TEXT("and the tenth in all turns it off"), FHyperAIStudioAutoApproval::IsActive());
		TestTrue(TEXT("for the external-effect limit"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::ExternalEffectLimit);
	}
	// A record already at a limit never comes back.
	KeepOn::FRecord Spent = MakeRecord(EDuration::OneHour, 60.0);
	Spent.Approved = FHyperAIStudioAutoApproval::GetMaxApprovals();
	TestTrue(TEXT("A record at its approval limit is refused"), KeepOn::Check(Spent, ContextNow()) == ERefusal::LimitReached);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalRestartSpendTest,
	"HyperAIStudio.Chat.AutoApproval.Restart.SpendSettingsChangeRefuses",
	HyperAIStudio::AutoApprovalRestartTests::Flags)

bool FHyperAIStudioAutoApprovalRestartSpendTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalRestartTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope(*this);
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const float Cap = Settings->ExternalServiceSessionSpendCap;
	ON_SCOPE_EXIT { Settings->ExternalServiceSessionSpendCap = Cap; };
	TestTrue(TEXT("External effects turn on, kept on"), FAccess::TurnOn({ECategory::ExternalEffects}, Scope.All(), EDuration::OneHour, false, true));
	FAccess::CloseEditor();
	// What any MCP client can do with Epic's settings toolset while the editor is closed or opening: raise the cap.
	Settings->ExternalServiceSessionSpendCap = Cap + 100.0f;
	TestTrue(TEXT("With the cap raised meanwhile, it is refused"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::SpendSettingsChanged);
	TestFalse(TEXT("stays off"), FHyperAIStudioAutoApproval::IsActive());
	TestFalse(TEXT("and its record is deleted"), Scope.HasRecord());

	// Unchanged settings, the same record comes back.
	Settings->ExternalServiceSessionSpendCap = Cap;
	FAccess::Reset();
	FAccess::SetRecordPath(Scope.Path);
	TestTrue(TEXT("It turns on again"), FAccess::TurnOn({ECategory::ExternalEffects}, Scope.All(), EDuration::OneHour, false, true));
	FAccess::CloseEditor();
	TestTrue(TEXT("and with the settings as they were, comes back"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::None);
	// A change while it is back on ends it as before, deleting the record.
	Settings->ExternalServiceSessionSpendCap = Cap + 50.0f;
	TestTrue(TEXT("Its one-second check runs"), FAccess::TickExpiry());
	TestTrue(TEXT("and a change now ends it"), !FHyperAIStudioAutoApproval::IsActive()
		&& FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::SpendSettingsChanged && !Scope.HasRecord());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalStayOnDialogTest,
	"HyperAIStudio.Chat.AutoApproval.Restart.DialogDefaultsAndRemembers",
	HyperAIStudio::AutoApprovalRestartTests::Flags)

bool FHyperAIStudioAutoApprovalStayOnDialogTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalRestartTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope(*this);
	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Fresh = FAccess::MakeDialog();
	TestFalse(TEXT("Stay on after the editor restarts is unticked by default"), FAccess::IsStayingOn(*Fresh));
	const FString Tooltip = FAccess::GetStayOnTooltip(*Fresh).ToString();
	for (const TCHAR* Says : {TEXT("ends when the editor closes"), TEXT("encrypted by Windows for your account"), TEXT("time the editor was closed counts"),
		TEXT("24 hours"), TEXT("30 minutes"), TEXT("spend cap"), TEXT("Only requests made after it comes back"), TEXT("delete the record")})
	{
		TestTrue(FString::Printf(TEXT("Its tooltip says what it does: \"%s\""), Says), Tooltip.Contains(Says));
	}
	TestFalse(TEXT("Unticked, the native confirmation does not mention it"),
		FAccess::GetConfirmation(*Fresh).ToString().Contains(TEXT("stays on after the editor restarts")));
	FAccess::SetStayOn(*Fresh, true);
	TestTrue(TEXT("Ticked, the native confirmation says it stays on"),
		FAccess::GetConfirmation(*Fresh).ToString().Contains(TEXT("It stays on after the editor restarts")));
	FAccess::SetDuration(*Fresh, EDuration::UntilEditorCloses);
	TestTrue(TEXT("and, until the editor closes, within 30 minutes and 24 hours"),
		FAccess::GetConfirmation(*Fresh).ToString().Contains(TEXT("within 30 minutes")) && FAccess::GetConfirmation(*Fresh).ToString().Contains(TEXT("24 hours")));

	// Under a test the dialog neither reads nor writes the user's choices, unless asked to remember.
	FAccess::RememberChoices(true);
	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Chosen = FAccess::MakeDialog();
	FAccess::SetTicked(*Chosen, ECategory::Destructive, true);
	FAccess::SetDuration(*Chosen, EDuration::FourHours);
	FAccess::SetAlsoApproveWaiting(*Chosen, false);
	FAccess::SetStayOn(*Chosen, true);
	FAccess::SetNativeAnswer(*Chosen, true);
	FAccess::PressTurnOn(*Chosen, Scope.All());
	TestTrue(TEXT("It turns on with those choices"), FHyperAIStudioAutoApproval::IsActive() && FHyperAIStudioAutoApproval::GetStatus().bStaysOnAfterRestart);
	FAccess::Reset();
	FAccess::RememberChoices(true);
	FAccess::SetRecordPath(Scope.Path);
	// Reset forgets them; turn on once more to see the next dialog remember.
	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Again = FAccess::MakeDialog();
	TestTrue(TEXT("After Reset a dialog starts from the defaults"), FAccess::IsTicked(*Again, ECategory::Edits) && !FAccess::IsTicked(*Again, ECategory::Destructive)
		&& FAccess::GetDuration(*Again) == EDuration::OneHour && FAccess::IsAlsoApprovingWaiting(*Again) && !FAccess::IsStayingOn(*Again));
	FAccess::SetTicked(*Again, ECategory::Destructive, true);
	FAccess::SetDuration(*Again, EDuration::FourHours);
	FAccess::SetAlsoApproveWaiting(*Again, false);
	FAccess::SetStayOn(*Again, true);
	FAccess::SetNativeAnswer(*Again, true);
	FAccess::PressTurnOn(*Again, Scope.All());
	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Remembered = FAccess::MakeDialog();
	TestTrue(TEXT("The next dialog opens with the last choices: categories"),
		FAccess::IsTicked(*Remembered, ECategory::Edits) && FAccess::IsTicked(*Remembered, ECategory::Destructive) && !FAccess::IsTicked(*Remembered, ECategory::Scripts));
	TestEqual(TEXT("duration"), FAccess::GetDuration(*Remembered), EDuration::FourHours);
	TestFalse(TEXT("the waiting option"), FAccess::IsAlsoApprovingWaiting(*Remembered));
	TestTrue(TEXT("and the stay-on box"), FAccess::IsStayingOn(*Remembered));
	FAccess::PressCancel(*Remembered);

	// Where the user's choices live: the per-user project settings, read and written as the editor does. What the file
	// held before is put back.
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
	TestTrue(TEXT("They live in the user's per-project settings: ") + Ini, Ini.Contains(TEXT("EditorPerProjectUserSettings")) && !Ini.Contains(TEXT("Default")));
	FHyperAIStudioAutoApprovalChoices Written;
	Written.Categories = {ECategory::Edits, ECategory::ExternalEffects, ECategory::SandboxSaves};
	Written.Duration = EDuration::UntilEditorCloses;
	Written.bAlsoApproveWaiting = false;
	Written.bStayOnAfterRestart = true;
	FHyperAIStudioAutoApproval::WriteChoices(Ini, Written);
	TestTrue(TEXT("and read back as written"), FHyperAIStudioAutoApproval::ReadChoices(Ini) == Written);
	GConfig->SetString(Section, TEXT("Categories"), TEXT("edits,everything"), Ini);
	GConfig->SetString(Section, TEXT("Duration"), TEXT("forever"), Ini);
	const FHyperAIStudioAutoApprovalChoices Garbled = FHyperAIStudioAutoApproval::ReadChoices(Ini);
	TestTrue(TEXT("Values it does not understand fall back to the defaults"),
		Garbled.Categories == TArray<ECategory>({ECategory::Edits}) && Garbled.Duration == EDuration::OneHour && Garbled.bStayOnAfterRestart);
	// Preferences never turn anything on: they say Stay on, but there is no sealed record.
	FAccess::Reset();
	FAccess::SetRecordPath(Scope.Path);
	TestTrue(TEXT("Choices saying Stay on restore nothing without a sealed record"),
		FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::NoRecord && !FHyperAIStudioAutoApproval::IsActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalRestoreNoticeTest,
	"HyperAIStudio.Chat.AutoApproval.Restart.RestoreNoticeAndActivity",
	HyperAIStudio::AutoApprovalRestartTests::Flags)

bool FHyperAIStudioAutoApprovalRestoreNoticeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalRestartTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope(*this);
	const FString Waiting = Scope.Add();
	const FString Fresh = Scope.Add();
	TestTrue(TEXT("It turns on until the editor closes, kept on"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::UntilEditorCloses, false, true));
	FAccess::CloseEditor();
	// A request that reaches the next editor before auto-approval is back, as an agent's might.
	bool bWaitingRan = false;
	TestTrue(TEXT("A request is held before it comes back"), Hold(Waiting, ECategory::Edits, [&bWaitingRan]() { bWaitingRan = true; }));
	FAccess::AdvanceClock(5.0 * 60.0);
	FLogCapture Log(TEXT("LogHyperAIStudioAutoApproval"));
	TestTrue(TEXT("It comes back"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::None);
	const int32 Pending = FHyperAIStudioApprovalGate::GetPending().Num();
	const FText Expected = FHyperAIStudioAutoApproval::DescribeRestored({ECategory::Edits}, {}, Pending);
	const TOptional<FText> Notice = FAccess::GetLastNotice();
	TestTrue(TEXT("Its notification says so: ") + (Notice.IsSet() ? Notice->ToString() : FString()), Notice.IsSet()
		&& Notice->ToString().StartsWith(TEXT("Auto-approval is back on from before the restart: Edits, until the editor closes."))
		&& Notice->ToString() == Expected.ToString());
	TestTrue(TEXT("and that requests waiting from before keep waiting"), Notice.IsSet() && Notice->ToString().Contains(TEXT("waiting from before")));
	const TArray<FText> Buttons = FAccess::GetLastNoticeButtons();
	TestTrue(TEXT("with Stop and Show"), Buttons.Num() == 2 && Buttons[0].ToString() == TEXT("Stop") && Buttons[1].ToString() == TEXT("Show"));
	TestTrue(TEXT("The log says it is back on"), Log.Count(TEXT("Auto-approval is back on from before the restart")) == 1);
	const TArray<FHyperAIStudioActivityEntry> Entries = FHyperAIStudioAgentActivityLog::GetSnapshot();
	TestTrue(TEXT("Activity records it"), !Entries.IsEmpty() && Entries[0].Kind == EHyperAIStudioActivityKind::AutoApproval
		&& Entries[0].StatusCode == TEXT("auto_approval_restored") && Entries[0].Detail.Contains(TEXT("kept on from before the restart")));
	TestTrue(TEXT("The banner's tooltip says it came back"), FHyperAIStudioAutoApproval::DescribeBannerTooltip().ToString().Contains(TEXT("came back on by itself")));

	// "Also approve the requests waiting now" is never repeated: only new requests.
	FAccess::PumpGate();
	TestTrue(TEXT("The request from before keeps waiting"), !bWaitingRan && FHyperAIStudioApprovalGate::HasPending(Waiting));
	TestTrue(TEXT("and the log can say why"), FHyperAIStudioAutoApproval::DescribeWhyNotCovered(ECategory::Edits, 0, Waiting).Contains(TEXT("already waiting")));
	TestTrue(TEXT("A new request is approved"), Approve(Fresh, ECategory::Edits));
	const TOptional<FHyperAIStudioActivityEntry> Approved = FindApproval(Fresh);
	TestTrue(TEXT("and Activity marks it kept on after restart"), Approved.IsSet() && Approved->bKeptOnAfterRestart
		&& Approved->Detail.StartsWith(TEXT("Auto-approved as edits (kept on after restart)")));
	const TOptional<FHyperAIStudioApprovalRecord> Record = FHyperAIStudioApprovalGate::FindApproval(Fresh);
	TestTrue(TEXT("as does the gate's record agents read"), Record.IsSet() && Record->bKeptOnAfterRestart);

	// The timed text names the time it ends, in the user's own time zone.
	const FDateTime Ends = FDateTime::UtcNow() + FTimespan::FromMinutes(20.0);
	const FString Timed = FHyperAIStudioAutoApproval::DescribeRestored({ECategory::Edits, ECategory::ExternalEffects}, Ends, 0).ToString();
	TestEqual(TEXT("A timed one says until when"), Timed,
		FString::Printf(TEXT("Auto-approval is back on from before the restart: Edits, External effects, until %s."), *FText::AsTime(Ends, EDateTimeStyle::Short).ToString()));

	// Its Stop button turns it off, as the banner's does, and the record goes.
	TestTrue(TEXT("Stop is pressed"), FAccess::PressLastNoticeButton(0));
	TestFalse(TEXT("and turns it off"), FHyperAIStudioAutoApproval::IsActive());
	TestTrue(TEXT("as stopped by the user"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::Stopped);
	TestFalse(TEXT("deleting its record"), Scope.HasRecord());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalNoRecordUntickedTest,
	"HyperAIStudio.Chat.AutoApproval.Restart.NothingKeptWhenUnticked",
	HyperAIStudio::AutoApprovalRestartTests::Flags)

bool FHyperAIStudioAutoApprovalNoRecordUntickedTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalRestartTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope(*this);
	TestTrue(TEXT("Turned on with the box unticked"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::OneHour));
	TestFalse(TEXT("it writes no record"), Scope.HasRecord());
	TestFalse(TEXT("and its status says it does not stay on"), FHyperAIStudioAutoApproval::GetStatus().bStaysOnAfterRestart);
	FAccess::CloseEditor();
	TestFalse(TEXT("The editor closing leaves none"), Scope.HasRecord());
	TestTrue(TEXT("so nothing comes back"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::NoRecord && !FHyperAIStudioAutoApproval::IsActive());
	TestFalse(TEXT("and nothing is shown"), FAccess::GetLastNotice().IsSet());

	// An older record is deleted when it turns on unticked, and when it turns off.
	FString Error;
	TestTrue(TEXT("An older record is there"), KeepOn::Write(Scope.Path, MakeRecord(EDuration::OneHour, 60.0), Error) && Scope.HasRecord());
	TestTrue(TEXT("Turned on unticked"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::OneHour));
	TestFalse(TEXT("it deletes the older record"), Scope.HasRecord());
	TestTrue(TEXT("An older record is there again"), KeepOn::Write(Scope.Path, MakeRecord(EDuration::OneHour, 60.0), Error));
	FHyperAIStudioAutoApproval::Stop(EHyperAIStudioAutoApprovalEnd::Stopped);
	TestFalse(TEXT("Turning it off deletes it too"), Scope.HasRecord());

	// Ticked, every end but the editor closing deletes the record: Stop, expiry and Turn Off.
	TestTrue(TEXT("Turned on ticked for 15 minutes"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::FifteenMinutes, false, true) && Scope.HasRecord());
	FAccess::AdvanceClock(16.0 * 60.0);
	TestTrue(TEXT("Its one-second check runs"), FAccess::TickExpiry());
	TestTrue(TEXT("and its expiry deletes the record"), !FHyperAIStudioAutoApproval::IsActive()
		&& FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::Expired && !Scope.HasRecord());
	TestTrue(TEXT("Turned on ticked again"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::OneHour, false, true) && Scope.HasRecord());
	TestTrue(TEXT("Turned on again unticked, replacing it"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::OneHour));
	TestFalse(TEXT("the record goes"), Scope.HasRecord());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutoApprovalKeptOnScreensTest,
	"HyperAIStudio.Screens.AutoApprovalKeptOn",
	HyperAIStudio::AutoApprovalRestartTests::Flags)

bool FHyperAIStudioAutoApprovalKeptOnScreensTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AutoApprovalRestartTests;
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	FScope Scope(*this);
	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Dialog = FAccess::MakeDialog();
	FAccess::SetStayOn(*Dialog, true);
	FAccess::SetDuration(*Dialog, EDuration::UntilEditorCloses);
	const FString DialogShot = HyperAIStudio::Tests::SaveWidgetShot(Dialog, FIntPoint(660, 1100), TEXT("auto-approval-dialog-stay-on"));
	const FString Approved = Scope.Add();
	TestTrue(TEXT("It turns on, kept on"), FAccess::TurnOn({ECategory::Edits}, Scope.All(), EDuration::UntilEditorCloses, false, true));
	FAccess::CloseEditor();
	TestTrue(TEXT("and comes back"), FAccess::Restore(Scope.Path, Scope.All()) == ERefusal::None);
	TestTrue(TEXT("A request is approved after it came back"), Approve(Approved, ECategory::Edits));
	const FString ActivityShot = HyperAIStudio::Tests::SaveWidgetShot(SNew(SHyperAIStudioChatActivitySidebar), FIntPoint(300, 560), TEXT("auto-approval-activity-kept-on"));
	if (DialogShot.IsEmpty() || ActivityShot.IsEmpty())
	{
		AddInfo(TEXT("This editor cannot render; no screenshots were taken."));
		return true;
	}
	AddInfo(TEXT("Screenshots: ") + DialogShot + TEXT(", ") + ActivityShot);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
