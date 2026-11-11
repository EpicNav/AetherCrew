// Games by Hyper 2026.

#include "HyperAIStudioAutoApproval.h"

#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioApprovalGateInternal.h"
#include "HyperAIStudioAutoApprovalKeepOn.h"
#include "HyperAIStudioSandbox.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioShell.h"
#include "Interfaces/IMainFrameModule.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "SHyperAIStudioAutoApprovalDialog.h"
#include "Styling/AppStyle.h"
#include "UObject/UnrealType.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

#define LOCTEXT_NAMESPACE "HyperAIStudioAutoApproval"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAutoApproval, Log, All);

namespace HyperAIStudio::AutoApproval
{
	double ClampCustomHours(const double Hours)
	{
		return FMath::IsFinite(Hours) ? FMath::Clamp(Hours, MinCustomHours, MaxCustomHours) : DefaultCustomHours;
	}

	double DurationSeconds(const EHyperAIStudioAutoApprovalDuration Duration, const double CustomHours)
	{
		switch (Duration)
		{
		case EHyperAIStudioAutoApprovalDuration::FifteenMinutes: return 15.0 * 60.0;
		case EHyperAIStudioAutoApprovalDuration::FourHours: return 4.0 * 3600.0;
		case EHyperAIStudioAutoApprovalDuration::EightHours: return 8.0 * 3600.0;
		case EHyperAIStudioAutoApprovalDuration::Custom: return ClampCustomHours(CustomHours) * 3600.0;
		case EHyperAIStudioAutoApprovalDuration::UntilEditorCloses: return 0.0;
		default: return 3600.0;
		}
	}
}

namespace HyperAIStudio::AutoApproval::Private
{
	/** The command, in its words: it may name hours after them, as "8" or "8h". */
	const TCHAR* const CommandVerb = TEXT("/activate");
	const TCHAR* const CommandName = TEXT("auto-hyper-approval");

	constexpr int32 MaxTracked = 256;
	constexpr EHyperAIStudioApprovalCategory AllCategories[] = {
		EHyperAIStudioApprovalCategory::Edits,
		EHyperAIStudioApprovalCategory::Scripts,
		EHyperAIStudioApprovalCategory::Destructive,
		EHyperAIStudioApprovalCategory::ExternalEffects,
		EHyperAIStudioApprovalCategory::SandboxSaves};
	/** How often it looks at what Sandbox Mode holds; checking for unsaved assets walks every package. */
	constexpr double SandboxCheckSeconds = 2.0;
	/** How often a sandbox write that keeps waiting says so again in the log; a new reason is logged at once. */
	constexpr double SandboxWaitLogSeconds = 30.0;
	/**
	 * How long a failed sandbox write waits before it is tried again while the held files stay as they were: what made
	 * it fail (a project file another program holds, a checkout) is fixed outside the sandbox, where nothing reports it.
	 */
	constexpr double SandboxRetrySeconds = 60.0;

	struct FActivation
	{
		/** Never 0, never reused: work it approved runs only while this is the current one. */
		uint64 Id = 0;
		uint8 CategoryMask = 0;
		EHyperAIStudioAutoApprovalDuration Duration = EHyperAIStudioAutoApprovalDuration::OneHour;
		double CustomHours = HyperAIStudio::AutoApproval::DefaultCustomHours;
		/** When it ends, on the monotonic clock; 0 while it lasts until the editor closes. */
		double EndsAt = 0.0;
		/** Requests held before it started keep waiting for a click, except those the user chose in its dialog. */
		uint64 FirstSequence = 0;
		/** Requests waiting as its dialog opened that the user chose there to approve as well. */
		TSet<FString> ChosenWaiting;
		/**
		 * Requests the activation it replaced had approved that went back to waiting because they had not started: the
		 * user approved them by turning that one on, and its dialog never listed them, so this one approves them again.
		 */
		TSet<FString> CarriedOver;
		/** The spend settings as the user confirmed it under; it ends if they change. */
		FString SpendSettings;
		int32 Approved = 0;
		int32 ExternalApproved = 0;
		/** Writes of Sandbox Mode's held saves, each also counted in Approved. */
		int32 SandboxWrites = 0;
		int32 ConsecutiveFailures = 0;
		/** Operations it approved whose outcome is not in yet, oldest first. */
		TArray<FString> Approving;
		/** "Stay on after the editor restarts" was ticked: its record under Saved/ is kept current until it ends. */
		bool bKeepOn = false;
		/** It came back on by itself as this editor started. */
		bool bRestored = false;
		/** When the user turned it on, in UTC; a restored one keeps the first time, for the 24-hour cap. */
		FDateTime TurnedOnUtc;
		/** When a timed one ends, in UTC, for the record: time the editor spends closed counts. */
		TOptional<FDateTime> ExpiresUtc;
		/** When its record was last written, on the monotonic clock, for the "until the editor closes" heartbeat. */
		double LastWritten = 0.0;
#if WITH_DEV_AUTOMATION_TESTS
		/** A test's activation covers only the requests that test made, and only while a test runs. */
		TOptional<TSet<FString>> OnlyOperations;
#endif
	};

	TOptional<FActivation>& Current()
	{
		static TOptional<FActivation> Value;
		return Value;
	}

	uint64& LastActivationId()
	{
		static uint64 Value = 0;
		return Value;
	}

	/** What the last Stop sent back to wait for a click: plans it approved that had not started. */
	TArray<FString>& ReturnedByStop()
	{
		static TArray<FString> Value;
		return Value;
	}

	/** A test's activation covers only that test's own requests; the user's covers whatever they ticked. */
	bool IsTestScoped(const FActivation& Activation)
	{
#if WITH_DEV_AUTOMATION_TESTS
		return Activation.OnlyOperations.IsSet();
#else
		return false;
#endif
	}

	/**
	 * What paid calls are checked against: the session spend cap, the external services (with their prices), the image
	 * models (with theirs) and the 3D task prices; and the approval limit. Any MCP client can write HyperAI's settings
	 * (Epic's settings toolset asks nobody), so an activation keeps them as the user confirmed it and ends when they
	 * change, rather than approving paid calls under a cap or a price an agent set afterwards, or more requests than the
	 * user agreed to.
	 */
	FString SpendSettings()
	{
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		FString Text;
		if (!Settings)
		{
			return Text;
		}
		const FName Names[] = {
			GET_MEMBER_NAME_CHECKED(UHyperAIStudioSettings, ExternalServiceSessionSpendCap),
			GET_MEMBER_NAME_CHECKED(UHyperAIStudioSettings, ExternalServices),
			GET_MEMBER_NAME_CHECKED(UHyperAIStudioSettings, ImageModels),
			GET_MEMBER_NAME_CHECKED(UHyperAIStudioSettings, MeshTaskPrices),
			GET_MEMBER_NAME_CHECKED(UHyperAIStudioSettings, AutoApprovalMaxApprovals)};
		for (const FName Name : Names)
		{
			if (const FProperty* Property = UHyperAIStudioSettings::StaticClass()->FindPropertyByName(Name))
			{
				Property->ExportText_InContainer(0, Text, Settings, nullptr, nullptr, PPF_None);
			}
			Text += TEXT("|");
		}
		return Text;
	}

	/** Category names in the log's and the activity record's English, whatever the editor's language. */
	const TCHAR* EnglishName(const EHyperAIStudioApprovalCategory Category)
	{
		switch (Category)
		{
		case EHyperAIStudioApprovalCategory::Scripts: return TEXT("Scripts");
		case EHyperAIStudioApprovalCategory::Destructive: return TEXT("Destructive");
		case EHyperAIStudioApprovalCategory::ExternalEffects: return TEXT("External effects");
		case EHyperAIStudioApprovalCategory::SandboxSaves: return TEXT("Sandbox saves");
		default: return TEXT("Edits");
		}
	}

	FTSTicker::FDelegateHandle& ExpiryHandle()
	{
		static FTSTicker::FDelegateHandle Handle;
		return Handle;
	}

	FDelegateHandle& ActivityHandle()
	{
		static FDelegateHandle Handle;
		return Handle;
	}

#if WITH_DEV_AUTOMATION_TESTS
	double& ClockOffset()
	{
		static double Value = 0.0;
		return Value;
	}

	TOptional<EHyperAIStudioAutoApprovalEnd>& LastEnd()
	{
		static TOptional<EHyperAIStudioAutoApprovalEnd> Value;
		return Value;
	}
#endif

	double Now()
	{
#if WITH_DEV_AUTOMATION_TESTS
		return FPlatformTime::Seconds() + ClockOffset();
#else
		return FPlatformTime::Seconds();
#endif
	}

	/** Wall-clock time, which a record is written and read in: a restart's downtime counts. Tests move it with Now(). */
	FDateTime UtcNow()
	{
#if WITH_DEV_AUTOMATION_TESTS
		return FDateTime::UtcNow() + FTimespan::FromSeconds(ClockOffset());
#else
		return FDateTime::UtcNow();
#endif
	}

	/** The activation that came back on after a restart in this editor; 0 when none did. */
	uint64& RestoredActivationId()
	{
		static uint64 Value = 0;
		return Value;
	}

	FTSTicker::FDelegateHandle& StartupHandle()
	{
		static FTSTicker::FDelegateHandle Handle;
		return Handle;
	}

	TWeakPtr<SNotificationItem>& RestoreNotice()
	{
		static TWeakPtr<SNotificationItem> Notice;
		return Notice;
	}

#if WITH_DEV_AUTOMATION_TESTS
	/** Where a test's kept-on activation keeps its record; empty, and a test's writes nothing. */
	FString& TestRecordPath()
	{
		static FString Value;
		return Value;
	}

	/** A dialog under test remembers its choices here, and only while a test asked it to. */
	struct FTestChoices
	{
		bool bRemember = false;
		TOptional<FHyperAIStudioAutoApprovalChoices> Saved;
	};

	FTestChoices& TestChoices()
	{
		static FTestChoices Value;
		return Value;
	}

	/** What the last restore or refusal would have shown; a test's is kept here instead of shown. */
	TOptional<FNotificationInfo>& LastNotice()
	{
		static TOptional<FNotificationInfo> Value;
		return Value;
	}
#endif

	uint8 Bit(const EHyperAIStudioApprovalCategory Category)
	{
		return static_cast<uint8>(1u << static_cast<uint8>(Category));
	}

	FString DurationName(const EHyperAIStudioAutoApprovalDuration Duration, const double CustomHours)
	{
		switch (Duration)
		{
		case EHyperAIStudioAutoApprovalDuration::FifteenMinutes: return TEXT("for 15 minutes");
		case EHyperAIStudioAutoApprovalDuration::FourHours: return TEXT("for 4 hours");
		case EHyperAIStudioAutoApprovalDuration::EightHours: return TEXT("for 8 hours");
		case EHyperAIStudioAutoApprovalDuration::Custom: return FString::Printf(TEXT("for %g hours"), ClampCustomHours(CustomHours));
		case EHyperAIStudioAutoApprovalDuration::UntilEditorCloses: return TEXT("until the editor closes");
		default: return TEXT("for 1 hour");
		}
	}

	FText DurationPhrase(const EHyperAIStudioAutoApprovalDuration Duration, const double CustomHours)
	{
		switch (Duration)
		{
		case EHyperAIStudioAutoApprovalDuration::FifteenMinutes: return LOCTEXT("ForFifteenMinutes", "for 15 minutes");
		case EHyperAIStudioAutoApprovalDuration::FourHours: return LOCTEXT("ForFourHours", "for 4 hours");
		case EHyperAIStudioAutoApprovalDuration::EightHours: return LOCTEXT("ForEightHours", "for 8 hours");
		case EHyperAIStudioAutoApprovalDuration::Custom:
			return FText::Format(LOCTEXT("ForCustomHours", "for {0} {0}|plural(one=hour,other=hours)"), ClampCustomHours(CustomHours));
		case EHyperAIStudioAutoApprovalDuration::UntilEditorCloses: return LOCTEXT("UntilClose", "until the editor closes");
		default: return LOCTEXT("ForOneHour", "for 1 hour");
		}
	}

	/** Hours as the command names them: "8", "2.5" or "8h". False for anything else, or past the Custom field's limits. */
	bool ParseCommandHours(FString Word, double& OutHours)
	{
		Word.RemoveFromEnd(TEXT("h"), ESearchCase::IgnoreCase);
		if (!Word.IsNumeric())
		{
			return false;
		}
		OutHours = FCString::Atod(*Word);
		return OutHours >= MinCustomHours && OutHours <= MaxCustomHours;
	}

	/** Why it ended, in the activity log's and the log's English. */
	FString EndReason(const EHyperAIStudioAutoApprovalEnd Why)
	{
		switch (Why)
		{
		case EHyperAIStudioAutoApprovalEnd::StoppedRemotely: return TEXT("stopped on the remote page");
		case EHyperAIStudioAutoApprovalEnd::Expired: return TEXT("its time ran out");
		case EHyperAIStudioAutoApprovalEnd::ApprovalLimit:
			return FString::Printf(TEXT("%d approvals reached"), FHyperAIStudioAutoApproval::GetMaxApprovals());
		case EHyperAIStudioAutoApprovalEnd::ExternalEffectLimit:
			return FString::Printf(TEXT("%d external effects reached"), FHyperAIStudioAutoApproval::MaxExternalEffects);
		case EHyperAIStudioAutoApprovalEnd::RepeatedFailures:
			return FString::Printf(TEXT("%d auto-approved requests in a row failed"), FHyperAIStudioAutoApproval::MaxConsecutiveFailures);
		case EHyperAIStudioAutoApprovalEnd::SpendSettingsChanged:
			return TEXT("the spend cap, an external service, an image model or the approval limit changed in HyperAI Studio's settings");
		case EHyperAIStudioAutoApprovalEnd::Replaced: return TEXT("replaced by a new auto-approval");
		case EHyperAIStudioAutoApprovalEnd::EditorClosing: return TEXT("the editor is closing");
		case EHyperAIStudioAutoApprovalEnd::TestFinished: return TEXT("the automation test that turned it on finished");
		default: return TEXT("stopped in the editor");
		}
	}

	TArray<EHyperAIStudioApprovalCategory> CategoriesOf(const uint8 Mask)
	{
		TArray<EHyperAIStudioApprovalCategory> Categories;
		for (const EHyperAIStudioApprovalCategory Category : AllCategories)
		{
			if (Mask & Bit(Category))
			{
				Categories.Add(Category);
			}
		}
		return Categories;
	}

	void Notify(const FText& Message, const bool bWarning)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bWarning ? 10.0f : 8.0f;
		Info.bFireAndForget = true;
		if (bWarning)
		{
			Info.Image = FAppStyle::GetBrush(TEXT("Icons.WarningWithColor"));
		}
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	/** Activity keeps when it went on and off, with no operation of its own. */
	void RecordAudit(const TCHAR* What, const FString& StatusCode, const FString& Detail)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = EHyperAIStudioActivityKind::AutoApproval;
		Entry.ToolName = What;
		Entry.StatusCode = StatusCode;
		Entry.Detail = Detail;
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	/** Where an activation's record lives: the user's under Saved/; a test's only where the test pointed it, else nowhere. */
	FString RecordPathFor(const FActivation& Activation)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (IsTestScoped(Activation))
		{
			return TestRecordPath();
		}
#endif
		return KeepOn::DefaultPath();
	}

	/** This editor, now, as a record is checked against. */
	KeepOn::FContext RecordContext()
	{
		KeepOn::FContext Context;
		Context.NowUtc = UtcNow();
		Context.ProjectPath = KeepOn::CurrentProject();
		Context.WindowsUser = KeepOn::CurrentUser();
		Context.SpendFingerprint = KeepOn::Fingerprint(SpendSettings());
		return Context;
	}

	/**
	 * Keeps a kept-on activation's record current: as it turns on, as its counts change and as the editor closes
	 * (bClosing stamps the close time). A record that cannot be written is deleted, never left with older counts.
	 */
	void WriteRecord(FActivation& Activation, const bool bClosing = false)
	{
		const FString Path = RecordPathFor(Activation);
		if (!Activation.bKeepOn || Path.IsEmpty())
		{
			return;
		}
		KeepOn::FRecord Record;
		Record.CategoryMask = Activation.CategoryMask;
		Record.Duration = Activation.Duration;
		Record.CustomHours = Activation.CustomHours;
		Record.TurnedOnUtc = Activation.TurnedOnUtc;
		Record.ExpiresUtc = Activation.ExpiresUtc;
		Record.Approved = Activation.Approved;
		Record.ExternalApproved = Activation.ExternalApproved;
		Record.ConsecutiveFailures = Activation.ConsecutiveFailures;
		Record.SandboxWrites = Activation.SandboxWrites;
		// The settings it was confirmed under, not today's: a change it has not noticed yet must still end it.
		Record.SpendFingerprint = KeepOn::Fingerprint(Activation.SpendSettings);
		Record.ProjectPath = KeepOn::CurrentProject();
		Record.WindowsUser = KeepOn::CurrentUser();
		Record.WrittenUtc = UtcNow();
		if (bClosing)
		{
			Record.ClosedUtc = Record.WrittenUtc;
		}
		Activation.LastWritten = FPlatformTime::Seconds();
		FString Error;
		if (!KeepOn::Write(Path, Record, Error))
		{
			UE_LOG(LogHyperAIStudioAutoApproval, Warning, TEXT("Auto-approval could not keep its record for after a restart, so it will not come back after one: %s"), *Error);
		}
	}

	void WriteCurrentRecord()
	{
		if (Current().IsSet())
		{
			WriteRecord(*Current());
		}
	}

	/** Auto-approval turning on or off without a record to keep: none is left from before. */
	void DeleteRecord(const FActivation& Activation)
	{
		const FString Path = RecordPathFor(Activation);
		if (KeepOn::Delete(Path))
		{
			UE_LOG(LogHyperAIStudioAutoApproval, Display, TEXT("Deleted the record of auto-approval kept on after a restart: %s"), *Path);
		}
	}

	/** Why the current activation does not cover a request; None when it does. */
	enum class ENotCovered : uint8
	{
		None,
		Off,
		NoCategory,
		NotChosen,
		Expired,
		ApprovalLimit,
		ExternalLimit,
		Failures,
		WaitingBefore,
		TestScope
	};

	ENotCovered Check(const TOptional<EHyperAIStudioApprovalCategory>& Category, const uint64 Sequence, const FString& OperationId)
	{
		const TOptional<FActivation>& Active = Current();
		if (!Active.IsSet())
		{
			return ENotCovered::Off;
		}
		// Sandbox Mode's held saves are no request: a request never reaches it by that category.
		if (!Category.IsSet() || *Category == EHyperAIStudioApprovalCategory::SandboxSaves)
		{
			return ENotCovered::NoCategory;
		}
		if (!(Active->CategoryMask & Bit(*Category)))
		{
			return ENotCovered::NotChosen;
		}
		if (Active->EndsAt > 0.0 && Now() >= Active->EndsAt)
		{
			return ENotCovered::Expired;
		}
		if (Active->Approved >= FHyperAIStudioAutoApproval::GetMaxApprovals())
		{
			return ENotCovered::ApprovalLimit;
		}
		if (*Category == EHyperAIStudioApprovalCategory::ExternalEffects && Active->ExternalApproved >= FHyperAIStudioAutoApproval::MaxExternalEffects)
		{
			return ENotCovered::ExternalLimit;
		}
		if (Active->ConsecutiveFailures >= FHyperAIStudioAutoApproval::MaxConsecutiveFailures)
		{
			return ENotCovered::Failures;
		}
		// What already waited as it began is covered only if the user chose it in its dialog, or the activation it replaced
		// had approved it.
		if (Sequence < Active->FirstSequence && !Active->ChosenWaiting.Contains(OperationId) && !Active->CarriedOver.Contains(OperationId))
		{
			return ENotCovered::WaitingBefore;
		}
#if WITH_DEV_AUTOMATION_TESTS
		if (Active->OnlyOperations.IsSet() && (!GIsAutomationTesting || !Active->OnlyOperations->Contains(OperationId)))
		{
			return ENotCovered::TestScope;
		}
#endif
		return ENotCovered::None;
	}

	/** Where writing Sandbox Mode's held saves stands, for the log and Activity's sandbox card. */
	struct FSandboxWait
	{
		/** What the write waits for, as a clause in the log's English; empty while nothing holds it back. */
		FString Log;
		FText Text;
		double Since = 0.0;
		double LastLogged = -1.0;
		/** It looks again no sooner than this, on the monotonic clock. */
		double NextCheck = 0.0;
		/** The held files left after a write failed: it is tried again when they change, or at RetryAt. */
		FString FailedOn;
		/** When a failed write is tried again with the same held files, on Now()'s clock. */
		double RetryAt = 0.0;
		/** The log said once, for this activation, that it does not write what the sandbox holds. */
		bool bToldNotCovered = false;
	};

	FSandboxWait& SandboxWait()
	{
		static FSandboxWait Value;
		return Value;
	}

	/** Logged when the reason changes and then at most every SandboxWaitLogSeconds. */
	void NoteSandboxWait(const FString& Log, const FText& Text, const int32 Files)
	{
		FSandboxWait& Wait = SandboxWait();
		const double Time = FPlatformTime::Seconds();
		if (Wait.Log.IsEmpty())
		{
			Wait.Since = Time;
		}
		const bool bNewReason = Wait.Log != Log;
		Wait.Log = Log;
		Wait.Text = Text;
		if (bNewReason || Time - Wait.LastLogged >= SandboxWaitLogSeconds)
		{
			Wait.LastLogged = Time;
			UE_LOG(LogHyperAIStudioAutoApproval, Display, TEXT("Auto-approval is waiting to write the %d file(s) Sandbox Mode holds: %s (waiting %.0f s)."),
				Files, *Log, Time - Wait.Since);
		}
	}

	void ClearSandboxWait()
	{
		FSandboxWait& Wait = SandboxWait();
		Wait.Log.Reset();
		Wait.Text = FText::GetEmpty();
		Wait.LastLogged = -1.0;
	}

	/** Whether the current activation reaches the sandbox named SandboxName at all; a test's only one it opened itself. */
	bool ReachesSandbox(const FActivation& Active, const FString& SandboxName)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (Active.OnlyOperations.IsSet())
		{
			return GIsAutomationTesting && Active.OnlyOperations->Contains(TEXT("sandbox:") + SandboxName);
		}
#endif
		return true;
	}

	/** Whether the current activation may write the sandbox named SandboxName: the user ticked its held saves. */
	bool MayWriteSandbox(const FActivation& Active, const FString& SandboxName)
	{
		return (Active.CategoryMask & Bit(EHyperAIStudioApprovalCategory::SandboxSaves)) && ReachesSandbox(Active, SandboxName);
	}

	/** HyperAI's own sandbox is open: its name; unset otherwise (off, waiting to open, or someone else's). */
	TOptional<FString> OurOpenSandbox()
	{
		HyperAIStudio::Sandbox::FStatus Status = HyperAIStudio::Sandbox::GetStatus(/*bWithChanges=*/false);
		return Status.State == HyperAIStudio::Sandbox::EState::Active ? TOptional<FString>(MoveTemp(Status.SandboxName)) : TOptional<FString>();
	}

	/** The held files as "kind path" lines, as CheckApproveAll lists them. */
	FString HeldNow()
	{
		TArray<FString> Files;
		for (const HyperAIStudio::Sandbox::FChange& Change : HyperAIStudio::Sandbox::GetStatus().Changes)
		{
			Files.Add(Change.Kind + TEXT(" ") + Change.Path);
		}
		return FString::Join(Files, TEXT("\n"));
	}

	/**
	 * When the user chose Sandbox Mode's held saves: writes what the sandbox holds to the project, as Approve All does,
	 * at a quiet moment. It never saves or reloads the user's unsaved work: it waits until nothing is unsaved, Play In
	 * Editor is not running and no agent edit runs. Removals delete project files, so they need Destructive too.
	 *
	 * Held saves are not only HyperAI's gated edits: whatever saves in the editor lands there, including edits from
	 * tools that never ask HyperAI for approval. In the reported session (10 Oct 2026) agents changed materials with
	 * editor Python sent through another MCP server, the Nwiro Integration Kit's execute_python on port 5353, so
	 * nothing reached HyperAI's gate and auto-approval had nothing to approve; Sandbox Mode was the only layer that
	 * caught those edits, and the dialog says that ticking this writes them unreviewed.
	 */
	void SweepSandbox()
	{
		namespace Sandbox = HyperAIStudio::Sandbox;
		using ECategory = EHyperAIStudioApprovalCategory;
		FSandboxWait& Wait = SandboxWait();
		{
			const TOptional<FActivation>& Active = Current();
			if (!Active.IsSet())
			{
				Wait = FSandboxWait();
				return;
			}
			if (!(Active->CategoryMask & Bit(ECategory::SandboxSaves)))
			{
				// Turned on without Sandbox Mode's held saves, so what the sandbox holds keeps waiting for Approve All:
				// said once per activation, or the Sandbox card's waiting files would read as auto-approval not working.
				if (!Wait.bToldNotCovered && Sandbox::GetHeldCount() > 0)
				{
					if (const TOptional<FString> Name = OurOpenSandbox(); Name.IsSet() && ReachesSandbox(*Active, *Name))
					{
						Wait.bToldNotCovered = true;
						UE_LOG(LogHyperAIStudioAutoApproval, Display,
							TEXT("Auto-approval is on but does not write the %d file(s) Sandbox Mode holds: Sandbox Mode's held saves was not ticked in its dialog. They wait for Approve All in Activity, saves by tools that never asked for approval included; turn auto-approval on again with that ticked to have them written."),
							Sandbox::GetHeldCount());
					}
				}
				return;
			}
			if (Active->Approved >= FHyperAIStudioAutoApproval::GetMaxApprovals()
				|| Active->ConsecutiveFailures >= FHyperAIStudioAutoApproval::MaxConsecutiveFailures)
			{
				return;
			}
		}
		const double Time = FPlatformTime::Seconds();
		if (Time < Wait.NextCheck)
		{
			return;
		}
		Wait.NextCheck = Time + SandboxCheckSeconds;
		const Sandbox::FReadiness Ready = Sandbox::CheckApproveAll();
		if (!Ready.bHolding)
		{
			ClearSandboxWait();
			Wait.FailedOn.Reset();
			return;
		}
		if (!MayWriteSandbox(*Current(), Ready.SandboxName))
		{
			return;
		}
		const FString Held = FString::Join(Ready.Files, TEXT("\n"));
		// After a failed write it waits: until the held files change, or for its back-off, since what made it fail is
		// usually fixed outside the sandbox (a project file another program holds, a checkout), where nothing reports it.
		if (!Wait.FailedOn.IsEmpty() && Wait.FailedOn == Held && Now() < Wait.RetryAt)
		{
			return;
		}
		Wait.FailedOn.Reset();
		const int32 Files = Ready.Files.Num();
		if (Ready.bRemovals && !(Current()->CategoryMask & Bit(ECategory::Destructive)))
		{
			NoteSandboxWait(TEXT("it holds removals, which delete the project's own files, and Destructive tools was not chosen; Approve All in Activity writes them"),
				LOCTEXT("SandboxWaitRemovals", "it holds removals, which delete the project's own files, and Destructive tools was not ticked; press Approve All to write them"),
				Files);
			return;
		}
		if (const HyperAIStudio::Approval::Private::FBusy Busy = HyperAIStudio::Approval::Private::DescribeBusy(); Busy.IsSet())
		{
			NoteSandboxWait(Busy.Log, Busy.Text, Files);
			return;
		}
		if (!Ready.WaitsFor.IsEmpty())
		{
			NoteSandboxWait(Ready.WaitsFor, FText::FromString(Ready.WaitsFor), Files);
			return;
		}

		const FString Listed = FString::Join(TArrayView<const FString>(Ready.Files).Left(10), TEXT(", "))
			+ (Files > 10 ? FString::Printf(TEXT(" and %d more"), Files - 10) : FString());
		FHyperAIStudioActivityEntry Entry;
		Entry.PackId = TEXT("sandbox");
		Entry.ToolName = TEXT("Sandbox Mode");
		Entry.AutoApprovedAs = ECategory::SandboxSaves;
		Entry.bKeptOnAfterRestart = Current()->bRestored;
		FString Error;
		// The same call as Approve All in Activity; only the activity entry and the log say nobody clicked. Each write is
		// one of its approvals, and a failed one one of its failures in a row, as the dialog says.
		if (Sandbox::ApproveAll(Error))
		{
			ClearSandboxWait();
			const int32 Write = Current().IsSet() ? ++Current()->SandboxWrites : 0;
			const int32 Approval = Current().IsSet() ? Current()->Approved + 1 : 0;
			Entry.Kind = EHyperAIStudioActivityKind::ApprovalApproved;
			Entry.StatusCode = TEXT("approved");
			Entry.Detail = FString::Printf(TEXT("Auto-approved as sandbox_saves; wrote the %d file(s) Sandbox Mode held to the project: %s. It is approval %d of %d."),
				Files, *Listed, Approval, FHyperAIStudioAutoApproval::GetMaxApprovals());
			UE_LOG(LogHyperAIStudioAutoApproval, Display, TEXT("Auto-approved Sandbox Mode's held saves: wrote %d file(s) to the project (%s); write %d, approval %d of %d."),
				Files, *Listed, Write, Approval, FHyperAIStudioAutoApproval::GetMaxApprovals());
			FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
			if (Current().IsSet())
			{
				Current()->ConsecutiveFailures = 0;
			}
			Wait.FailedOn.Reset();
			FHyperAIStudioAutoApproval::NoteApproved(ECategory::SandboxSaves);
			return;
		}
		// What is left held after the failure: a write that wrote some files changes the list, which is no new save.
		Wait.FailedOn = HeldNow();
		Wait.RetryAt = Now() + SandboxRetrySeconds;
		const int32 Failures = Current().IsSet() ? Current()->ConsecutiveFailures + 1 : 0;
		Entry.Kind = EHyperAIStudioActivityKind::Failed;
		Entry.StatusCode = TEXT("sandbox_write_failed");
		Entry.Detail = FString::Printf(TEXT("Auto-approved as sandbox_saves, but writing the %d held file(s) failed: %s It is tried again in %.0f s, or when the held files change; failure %d of %d in a row."),
			Files, *Error, SandboxRetrySeconds, Failures, FHyperAIStudioAutoApproval::MaxConsecutiveFailures);
		UE_LOG(LogHyperAIStudioAutoApproval, Warning, TEXT("Auto-approval could not write the %d file(s) Sandbox Mode holds: %s It tries again in %.0f s, or when they change (failure %d of %d in a row)."),
			Files, *Error, SandboxRetrySeconds, Failures, FHyperAIStudioAutoApproval::MaxConsecutiveFailures);
		NoteSandboxWait(FString::Printf(TEXT("writing them failed (%s); it tries again in a minute, or when the held files change"), *Error),
			FText::Format(LOCTEXT("SandboxWaitFailedRetry", "writing them failed ({0}); it tries again in a minute, or when the held files change"), FText::FromString(Error)),
			Files);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
		if (Current().IsSet() && ++Current()->ConsecutiveFailures >= FHyperAIStudioAutoApproval::MaxConsecutiveFailures)
		{
			FHyperAIStudioAutoApproval::Stop(EHyperAIStudioAutoApprovalEnd::RepeatedFailures);
		}
		else
		{
			WriteCurrentRecord();
		}
	}

	bool TickExpiry(float)
	{
		FHyperAIStudioAutoApproval::EndIfOver();
		if (!Current().IsSet())
		{
			ExpiryHandle().Reset();
			return false;
		}
		// "Until the editor closes" kept on: a crash records no close time, so the record is written again now and then
		// and its last write stands for the close.
		if (Current()->bKeepOn && Current()->Duration == EHyperAIStudioAutoApprovalDuration::UntilEditorCloses
			&& FPlatformTime::Seconds() - Current()->LastWritten >= KeepOn::HeartbeatSeconds)
		{
			WriteCurrentRecord();
		}
		SweepSandbox();
		return Current().IsSet();
	}

	/** Counts the outcomes of what it approved: a run of failures turns it off. */
	void OnActivity()
	{
		TOptional<FActivation>& Active = Current();
		if (!Active.IsSet() || Active->Approving.IsEmpty())
		{
			return;
		}
		// Record inserts at the front and then broadcasts, so the newest entry is the one that changed.
		const TArray<FHyperAIStudioActivityEntry> Entries = FHyperAIStudioAgentActivityLog::GetSnapshot();
		if (Entries.IsEmpty() || Entries[0].OperationId.IsEmpty() || !Active->Approving.Contains(Entries[0].OperationId))
		{
			return;
		}
		const FHyperAIStudioActivityEntry& Newest = Entries[0];
		if (Newest.Kind == EHyperAIStudioActivityKind::Completed || Newest.Kind == EHyperAIStudioActivityKind::JobFinished)
		{
			Active->Approving.Remove(Newest.OperationId);
			const bool bWasFailing = Active->ConsecutiveFailures > 0;
			Active->ConsecutiveFailures = 0;
			if (bWasFailing)
			{
				WriteRecord(*Active);
			}
		}
		else if (Newest.Kind == EHyperAIStudioActivityKind::Failed)
		{
			Active->Approving.Remove(Newest.OperationId);
			if (++Active->ConsecutiveFailures >= FHyperAIStudioAutoApproval::MaxConsecutiveFailures)
			{
				FHyperAIStudioAutoApproval::Stop(EHyperAIStudioAutoApprovalEnd::RepeatedFailures);
			}
			else
			{
				WriteRecord(*Active);
			}
		}
	}

	/** Shows a notification, or for a test's activation keeps it for the test to read; the user never sees a test's. */
	TSharedPtr<SNotificationItem> ShowNotice(FNotificationInfo Info, const bool bTest)
	{
#if WITH_DEV_AUTOMATION_TESTS
		LastNotice() = Info;
#endif
		if (bTest || !FSlateApplication::IsInitialized())
		{
			return nullptr;
		}
		const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(SNotificationItem::CS_None);
		}
		return Item;
	}

	void CloseRestoreNotice()
	{
		const TSharedPtr<SNotificationItem> Item = RestoreNotice().Pin();
		if (Item.IsValid() && FSlateApplication::IsInitialized())
		{
			Item->ExpireAndFadeout();
		}
		RestoreNotice().Reset();
	}

	/**
	 * Brings back the auto-approval kept on in the record at Path, once, as this editor starts: only if the record is
	 * valid for this user and project, unexpired (time the editor was closed counts), younger than 24 hours, under the
	 * same spend settings and, for "until the editor closes", the editor opened again within 30 minutes of closing.
	 * Anything else is refused, and the record deleted and logged. It covers only requests made from now on: what
	 * waits already keeps waiting, as "Also approve the requests waiting now" is never repeated. TestScope makes it a
	 * test's activation, covering only those ids.
	 */
	KeepOn::ERefusal RestoreFrom(const FString& Path, const TOptional<TSet<FString>>& TestScope)
	{
		using KeepOn::ERefusal;
		const bool bTest = TestScope.IsSet();
		if (Current().IsSet())
		{
			// What the user turned on since this editor started owns the record now.
			if (!IsTestScoped(*Current()) || !bTest)
			{
				return ERefusal::NoRecord;
			}
			FHyperAIStudioAutoApproval::Stop(EHyperAIStudioAutoApprovalEnd::TestFinished);
		}
		const KeepOn::FContext Context = RecordContext();
		KeepOn::FRecord Record;
		const ERefusal Refusal = KeepOn::Read(Path, Context, Record);
		if (Refusal == ERefusal::NoRecord)
		{
			return Refusal;
		}
		const TCHAR* const TestNote = bTest ? TEXT(" (automation test)") : TEXT("");
		if (Refusal != ERefusal::None)
		{
			KeepOn::Delete(Path);
			const FString Why = KeepOn::DescribeRefusal(Refusal, Record, Context);
			const bool bSuspicious = KeepOn::IsSuspicious(Refusal);
			if (bSuspicious)
			{
				UE_LOG(LogHyperAIStudioAutoApproval, Warning, TEXT("A record of auto-approval kept on after a restart was refused and deleted%s: %s (%s). Auto-approval stays off."),
					TestNote, *Why, *Path);
			}
			else
			{
				UE_LOG(LogHyperAIStudioAutoApproval, Display, TEXT("Auto-approval kept on from before the restart did not come back%s: %s. Its record was deleted; auto-approval is off."),
					TestNote, *Why);
			}
			RecordAudit(TEXT("not restored"), TEXT("auto_approval_not_restored"), Why + TestNote);
			FNotificationInfo Info(FText::Format(bSuspicious
				? LOCTEXT("RestoreRefusedToast", "A saved auto-approval was refused and deleted: {0}. Auto-approval is off.")
				: LOCTEXT("RestoreEndedToast", "Auto-approval did not come back on after the restart: {0}. It is off; turn it on again from Activity if you want it."),
				KeepOn::DescribeRefusalText(Refusal)));
			Info.ExpireDuration = 12.0f;
			Info.bFireAndForget = true;
			Info.WidthOverride = 480.0f;
			if (bSuspicious)
			{
				Info.Image = FAppStyle::GetBrush(TEXT("Icons.WarningWithColor"));
			}
			ShowNotice(MoveTemp(Info), bTest);
			return Refusal;
		}

		FActivation Activation;
		Activation.Id = ++LastActivationId();
		Activation.CategoryMask = Record.CategoryMask;
		Activation.Duration = Record.Duration;
		Activation.CustomHours = Record.CustomHours;
		Activation.TurnedOnUtc = Record.TurnedOnUtc;
		Activation.ExpiresUtc = Record.ExpiresUtc;
		Activation.EndsAt = Record.ExpiresUtc.IsSet() ? Now() + FMath::Max(0.0, (*Record.ExpiresUtc - Context.NowUtc).GetTotalSeconds()) : 0.0;
		// Only requests made from here on: whatever waits already was never chosen in a dialog in this editor.
		Activation.FirstSequence = HyperAIStudio::Approval::Private::PeekNextSequence();
		Activation.SpendSettings = SpendSettings();
		Activation.Approved = Record.Approved;
		Activation.ExternalApproved = Record.ExternalApproved;
		Activation.ConsecutiveFailures = Record.ConsecutiveFailures;
		Activation.SandboxWrites = Record.SandboxWrites;
		Activation.bKeepOn = true;
		Activation.bRestored = true;
#if WITH_DEV_AUTOMATION_TESTS
		Activation.OnlyOperations = TestScope;
#endif
		RestoredActivationId() = Activation.Id;
		Current() = MoveTemp(Activation);
		SandboxWait() = FSandboxWait();
		ExpiryHandle() = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickExpiry), 1.0f);
		ActivityHandle() = FHyperAIStudioAgentActivityLog::OnChanged().AddStatic(&OnActivity);
		// Open again: the record loses its close time, and keeps being written as before.
		WriteRecord(*Current());

		const TArray<EHyperAIStudioApprovalCategory> Categories = CategoriesOf(Record.CategoryMask);
		TArray<FString> Names;
		for (const EHyperAIStudioApprovalCategory Category : Categories)
		{
			Names.Add(EnglishName(Category));
		}
		const int32 Waiting = FHyperAIStudioApprovalGate::GetPending().Num();
		const FString Until = Record.ExpiresUtc.IsSet()
			? FString::Printf(TEXT("until %s UTC"), *Record.ExpiresUtc->ToString(TEXT("%Y-%m-%d %H:%M")))
			: FString(TEXT("until the editor closes"));
		const FString Detail = FString::Printf(
			TEXT("%s %s, kept on from before the restart (turned on %s UTC; %d of %d approvals, %d of %d external effects and %d of %d failures in a row used). It approves only requests made from now on%s"),
			*FString::Join(Names, TEXT(", ")), *Until, *Record.TurnedOnUtc.ToString(TEXT("%Y-%m-%d %H:%M")),
			Record.Approved, FHyperAIStudioAutoApproval::GetMaxApprovals(), Record.ExternalApproved, FHyperAIStudioAutoApproval::MaxExternalEffects,
			Record.ConsecutiveFailures, FHyperAIStudioAutoApproval::MaxConsecutiveFailures,
			Waiting > 0 ? *FString::Printf(TEXT("; the %d request(s) waiting from before keep waiting for a click"), Waiting) : TEXT(""));
		if (bTest)
		{
			UE_LOG(LogHyperAIStudioAutoApproval, Display, TEXT("Auto-approval is back on from before the restart, for an automation test's own requests: %s."), *Detail);
		}
		else
		{
			UE_LOG(LogHyperAIStudioAutoApproval, Warning,
				TEXT("Auto-approval is back on from before the restart: %s. It covers every agent and MCP client connected to this editor."), *Detail);
		}
		RecordAudit(TEXT("turned on"), TEXT("auto_approval_restored"), Detail + TestNote);

		FNotificationInfo Info(FHyperAIStudioAutoApproval::DescribeRestored(Categories, Record.ExpiresUtc, Waiting));
		Info.bFireAndForget = true;
		Info.ExpireDuration = 30.0f;
		Info.WidthOverride = 480.0f;
		Info.Image = FAppStyle::GetBrush(TEXT("Icons.WarningWithColor"));
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("RestoreStop", "Stop"),
			LOCTEXT("RestoreStopTooltip", "Turn auto-approval off now. Requests wait for your approval again."),
			FSimpleDelegate::CreateLambda([]()
			{
				FHyperAIStudioAutoApproval::Stop(EHyperAIStudioAutoApprovalEnd::Stopped);
				CloseRestoreNotice();
			}), SNotificationItem::CS_None));
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("RestoreShow", "Show"),
			LOCTEXT("RestoreShowTooltip", "Open Activity, where auto-approval's banner, what it approved and Stop are."),
			FSimpleDelegate::CreateLambda([]()
			{
				FHyperAIStudioShell::Open(TEXT("Activity"));
				CloseRestoreNotice();
			}), SNotificationItem::CS_None));
		RestoreNotice() = ShowNotice(MoveTemp(Info), bTest);

		FHyperAIStudioAutoApproval::OnChanged().Broadcast();
		FHyperAIStudioApprovalGate::OnChanged().Broadcast();
		return ERefusal::None;
	}

	/** Once the editor's main window is up (notifications can be seen), before anything could be auto-approved. */
	bool TickStartup(float)
	{
		const bool bInteractive = GIsEditor && !IsRunningCommandlet() && !FApp::IsUnattended() && !GIsAutomationTesting && !GIsBuildMachine;
		if (!bInteractive)
		{
			StartupHandle().Reset();
			return false;
		}
		const IMainFrameModule* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>(TEXT("MainFrame"));
		if (!FSlateApplication::IsInitialized() || !MainFrame || !MainFrame->IsWindowInitialized() || GFrameCounter == 0)
		{
			return true;
		}
		StartupHandle().Reset();
		RestoreFrom(KeepOn::DefaultPath(), {});
		return false;
	}
}

int32 FHyperAIStudioAutoApproval::GetMaxApprovals()
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	return FMath::Clamp(Settings ? Settings->AutoApprovalMaxApprovals : DefaultMaxApprovals, MinMaxApprovals, MaxMaxApprovals);
}

bool FHyperAIStudioAutoApproval::IsActive()
{
	return HyperAIStudio::AutoApproval::Private::Current().IsSet();
}

uint64 FHyperAIStudioAutoApproval::GetActivationId()
{
	const TOptional<HyperAIStudio::AutoApproval::Private::FActivation>& Active = HyperAIStudio::AutoApproval::Private::Current();
	return Active.IsSet() ? Active->Id : 0;
}

bool FHyperAIStudioAutoApproval::IsRestored()
{
	const TOptional<HyperAIStudio::AutoApproval::Private::FActivation>& Active = HyperAIStudio::AutoApproval::Private::Current();
	return Active.IsSet() && Active->bRestored;
}

bool FHyperAIStudioAutoApproval::WasRestored(const uint64 ActivationId)
{
	return ActivationId != 0 && ActivationId == HyperAIStudio::AutoApproval::Private::RestoredActivationId();
}

void FHyperAIStudioAutoApproval::Startup()
{
	using namespace HyperAIStudio::AutoApproval::Private;
	if (IsRunningCommandlet() || StartupHandle().IsValid())
	{
		return;
	}
	StartupHandle() = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickStartup), 0.25f);
}

void FHyperAIStudioAutoApproval::Shutdown()
{
	using namespace HyperAIStudio::AutoApproval::Private;
	if (StartupHandle().IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(StartupHandle());
		StartupHandle().Reset();
	}
	RestoreNotice().Reset();
}

FHyperAIStudioAutoApprovalStatus FHyperAIStudioAutoApproval::GetStatus()
{
	using namespace HyperAIStudio::AutoApproval::Private;
	FHyperAIStudioAutoApprovalStatus Status;
	const TOptional<FActivation>& Active = Current();
	if (!Active.IsSet())
	{
		return Status;
	}
	Status.bActive = true;
	Status.Categories = CategoriesOf(Active->CategoryMask);
	Status.Duration = Active->Duration;
	Status.CustomHours = Active->CustomHours;
	Status.SecondsLeft = Active->EndsAt > 0.0 ? FMath::Max(0.0, Active->EndsAt - Now()) : -1.0;
	Status.Approved = Active->Approved;
	Status.ExternalEffectsApproved = Active->ExternalApproved;
	Status.ChosenWaiting = Active->ChosenWaiting.Num();
	Status.SandboxWrites = Active->SandboxWrites;
	Status.bStaysOnAfterRestart = Active->bKeepOn;
	Status.bRestored = Active->bRestored;
	return Status;
}

bool FHyperAIStudioAutoApproval::Covers(const EHyperAIStudioApprovalCategory Category, const uint64 Sequence, const FString& OperationId)
{
	using namespace HyperAIStudio::AutoApproval::Private;
	return Check(Category, Sequence, OperationId) == ENotCovered::None;
}

FString FHyperAIStudioAutoApproval::DescribeWhyNotCovered(
	const TOptional<EHyperAIStudioApprovalCategory>& Category,
	const uint64 Sequence,
	const FString& OperationId)
{
	using namespace HyperAIStudio::AutoApproval::Private;
	switch (Check(Category, Sequence, OperationId))
	{
	case ENotCovered::None: return FString();
	case ENotCovered::Off: return TEXT("auto-approval is off");
	case ENotCovered::NoCategory:
		return TEXT("it fits no category (a read, or a plan whose declared and sealed safety are different risky kinds), so only a click approves it");
	case ENotCovered::NotChosen:
	{
		TArray<FString> Names;
		for (const EHyperAIStudioApprovalCategory Chosen : CategoriesOf(Current()->CategoryMask))
		{
			Names.Add(EnglishName(Chosen));
		}
		return FString::Printf(TEXT("%s was not chosen in its dialog (it covers %s)"), EnglishName(*Category), *FString::Join(Names, TEXT(", ")));
	}
	case ENotCovered::Expired: return TEXT("its time ran out");
	case ENotCovered::ApprovalLimit: return FString::Printf(TEXT("it reached its limit of %d approvals"), GetMaxApprovals());
	case ENotCovered::ExternalLimit: return FString::Printf(TEXT("it reached its limit of %d external effects"), MaxExternalEffects);
	case ENotCovered::Failures: return FString::Printf(TEXT("%d requests it approved failed in a row"), MaxConsecutiveFailures);
	case ENotCovered::WaitingBefore:
		return Current().IsSet() && Current()->bRestored
			? TEXT("it was already waiting when auto-approval came back on after the editor restarted, and that covers only new requests")
			: TEXT("it was already waiting when auto-approval was turned on, and was not chosen in its dialog to be approved too");
	default: return TEXT("an automation test's auto-approval covers only that test's own requests");
	}
}

bool FHyperAIStudioAutoApproval::IsFromBeforeStart(const uint64 Sequence)
{
	const TOptional<HyperAIStudio::AutoApproval::Private::FActivation>& Active = HyperAIStudio::AutoApproval::Private::Current();
	return Active.IsSet() && Sequence < Active->FirstSequence;
}

bool FHyperAIStudioAutoApproval::WasCarriedOver(const FString& OperationId)
{
	const TOptional<HyperAIStudio::AutoApproval::Private::FActivation>& Active = HyperAIStudio::AutoApproval::Private::Current();
	return Active.IsSet() && Active->CarriedOver.Contains(OperationId);
}

bool FHyperAIStudioAutoApproval::CoversSandboxSaves()
{
	using namespace HyperAIStudio::AutoApproval::Private;
	const TOptional<FActivation>& Active = Current();
	if (!Active.IsSet())
	{
		return false;
	}
	// It writes only HyperAI's own open sandbox: not one still waiting to open, and never someone else's.
	const TOptional<FString> Name = OurOpenSandbox();
	return Name.IsSet() && MayWriteSandbox(*Active, *Name);
}

bool FHyperAIStudioAutoApproval::LeavesSandboxSavesWaiting()
{
	using namespace HyperAIStudio::AutoApproval::Private;
	const TOptional<FActivation>& Active = Current();
	if (!Active.IsSet() || (Active->CategoryMask & Bit(EHyperAIStudioApprovalCategory::SandboxSaves)))
	{
		return false;
	}
	const TOptional<FString> Name = OurOpenSandbox();
	return Name.IsSet() && ReachesSandbox(*Active, *Name);
}

FText FHyperAIStudioAutoApproval::DescribeSandboxWait()
{
	using namespace HyperAIStudio::AutoApproval::Private;
	if (LeavesSandboxSavesWaiting())
	{
		return LOCTEXT("SandboxNotCovered",
			"Auto-approval is on but does not write these: Sandbox Mode's held saves was not ticked in its dialog. Press Approve All, or turn auto-approval on again with that ticked.");
	}
	if (!CoversSandboxSaves())
	{
		return FText::GetEmpty();
	}
	const FSandboxWait& Wait = SandboxWait();
	return Wait.Text.IsEmpty()
		? LOCTEXT("SandboxWriteSoon", "Auto-approval writes these to the project at the next quiet moment.")
		: FText::Format(LOCTEXT("SandboxWriteWaiting", "Auto-approval writes these to the project at the next quiet moment; it is waiting because {0}."), Wait.Text);
}

void FHyperAIStudioAutoApproval::EndIfOver()
{
	using namespace HyperAIStudio::AutoApproval::Private;
	const TOptional<FActivation>& Active = Current();
	if (!Active.IsSet())
	{
		return;
	}
	if (Active->EndsAt > 0.0 && Now() >= Active->EndsAt)
	{
		Stop(EHyperAIStudioAutoApprovalEnd::Expired);
	}
	else if (SpendSettings() != Active->SpendSettings)
	{
		Stop(EHyperAIStudioAutoApprovalEnd::SpendSettingsChanged);
	}
}

void FHyperAIStudioAutoApproval::BeginApproval(const FString& OperationId)
{
	using namespace HyperAIStudio::AutoApproval::Private;
	TOptional<FActivation>& Active = Current();
	if (!Active.IsSet())
	{
		return;
	}
	Active->Approving.Remove(OperationId);
	if (Active->Approving.Num() >= MaxTracked)
	{
		Active->Approving.RemoveAt(0);
	}
	Active->Approving.Add(OperationId);
}

void FHyperAIStudioAutoApproval::NoteApproved(const EHyperAIStudioApprovalCategory Category)
{
	using namespace HyperAIStudio::AutoApproval::Private;
	TOptional<FActivation>& Active = Current();
	if (!Active.IsSet())
	{
		return;
	}
	++Active->Approved;
	if (Category == EHyperAIStudioApprovalCategory::ExternalEffects)
	{
		++Active->ExternalApproved;
	}
	// Ended at once rather than at the next request, so the banners go as the limit is reached.
	if (Active->Approved >= GetMaxApprovals())
	{
		Stop(EHyperAIStudioAutoApprovalEnd::ApprovalLimit);
	}
	else if (Active->ExternalApproved >= MaxExternalEffects)
	{
		Stop(EHyperAIStudioAutoApprovalEnd::ExternalEffectLimit);
	}
	else
	{
		// Kept on: the record follows the counts, so the limits trip at the same totals after a restart.
		WriteRecord(*Active);
		OnChanged().Broadcast();
	}
}

bool FHyperAIStudioAutoApproval::Activate(
	const TArray<EHyperAIStudioApprovalCategory>& Categories,
	const EHyperAIStudioAutoApprovalDuration Duration,
	const double CustomHours,
	const TSet<FString>& ChosenWaiting,
	const bool bStayOnAfterRestart,
	FString& OutError,
	const bool bTyped)
{
	using namespace HyperAIStudio::AutoApproval::Private;
	OutError.Reset();
#if WITH_DEV_AUTOMATION_TESTS
	TOptional<TSet<FString>> TestScope = MoveTemp(TestScopeForNextActivation());
	TestScopeForNextActivation().Reset();
	// Under a test only a test's own requests are ever covered, however the dialog was answered.
	if (GIsAutomationTesting && !TestScope.IsSet())
	{
		TestScope.Emplace();
	}
#endif
	if (!IsInGameThread() || IsRunningCommandlet() || !FSlateApplication::IsInitialized())
	{
		OutError = TEXT("Auto-approval turns on only in the editor's own window.");
		return false;
	}
	if (Categories.IsEmpty())
	{
		OutError = TEXT("Choose at least one category.");
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	// A test may run in the user's own editor; it never takes the place of the auto-approval they turned on.
	if (TestScope.IsSet() && Current().IsSet() && !IsTestScoped(*Current()))
	{
		OutError = TEXT("The user's own auto-approval is on; a test leaves it alone.");
		return false;
	}
#endif
	FActivation Activation;
	for (const EHyperAIStudioApprovalCategory Category : Categories)
	{
		Activation.CategoryMask |= Bit(Category);
	}
#if WITH_DEV_AUTOMATION_TESTS
	Activation.OnlyOperations = MoveTemp(TestScope);
#endif
	const bool bTest = IsTestScoped(Activation);
	if (Current().IsSet())
	{
		// What the activation it replaces approved that had not started goes back to waiting as it stops. Its dialog
		// listed only what was held then, so the user never chose these there; they approved them by turning the one
		// before on, so those in this one's categories are approved again. A test's are never carried into the user's.
		const bool bCarry = !IsTestScoped(*Current()) || bTest;
		Stop(EHyperAIStudioAutoApprovalEnd::Replaced);
		if (bCarry && !ReturnedByStop().IsEmpty())
		{
			for (const FHyperAIStudioPendingApproval& Pending : FHyperAIStudioApprovalGate::GetPending())
			{
				if (ReturnedByStop().Contains(Pending.OperationId) && Pending.Category.IsSet() && (Activation.CategoryMask & Bit(*Pending.Category)))
				{
					Activation.CarriedOver.Add(Pending.OperationId);
				}
			}
		}
		ReturnedByStop().Reset();
	}

	Activation.Id = ++LastActivationId();
	Activation.Duration = Duration;
	Activation.CustomHours = HyperAIStudio::AutoApproval::ClampCustomHours(CustomHours);
	const double Seconds = HyperAIStudio::AutoApproval::DurationSeconds(Duration, CustomHours);
	Activation.EndsAt = Seconds > 0.0 ? Now() + Seconds : 0.0;
	Activation.FirstSequence = HyperAIStudio::Approval::Private::PeekNextSequence();
	Activation.ChosenWaiting = ChosenWaiting;
	Activation.SpendSettings = SpendSettings();
	Activation.TurnedOnUtc = UtcNow();
	if (Seconds > 0.0)
	{
		Activation.ExpiresUtc = Activation.TurnedOnUtc + FTimespan::FromSeconds(Seconds);
	}
	Activation.bKeepOn = bStayOnAfterRestart;
	const TSet<FString> CarriedOver = Activation.CarriedOver;
	Current() = MoveTemp(Activation);
	SandboxWait() = FSandboxWait();
	ExpiryHandle() = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickExpiry), 1.0f);
	ActivityHandle() = FHyperAIStudioAgentActivityLog::OnChanged().AddStatic(&OnActivity);
	// Kept on, its record follows it from now; otherwise no record is left from an earlier one.
	if (bStayOnAfterRestart)
	{
		WriteRecord(*Current());
	}
	else
	{
		DeleteRecord(*Current());
	}
	const TCHAR* const KeptOn = bStayOnAfterRestart ? TEXT("; it stays on after the editor restarts") : TEXT("");
	const FString Lasting = DurationName(Duration, CustomHours);
	const TCHAR* const Confirmed = bTyped ? TEXT("typed in a chat with the keyboard's Enter key") : TEXT("confirmed in its dialog");

	TArray<FString> Names;
	for (const EHyperAIStudioApprovalCategory Category : Categories)
	{
		Names.Add(EnglishName(Category));
	}
	const FString Covered = FString::Join(Names, TEXT(", "));
	// The requests already waiting that the user chose in the dialog, and those carried over from the activation this
	// replaced, are approved on the next quiet tick, as new ones are.
	const int32 Scheduled = HyperAIStudio::Approval::Private::ScheduleCovered();
	int32 Carried = 0;
	for (const FString& Id : CarriedOver)
	{
		Carried += FHyperAIStudioApprovalGate::IsAutoApproving(Id) ? 1 : 0;
	}
	const int32 Chosen = Scheduled - Carried;
	FString AlsoWaiting = Chosen > 0
		? FString::Printf(TEXT("; it also approves %d request(s) that were waiting, as %s"), Chosen,
			bTyped ? TEXT("last chosen in its dialog") : TEXT("chosen in its dialog")) : FString();
	if (Carried > 0)
	{
		AlsoWaiting += FString::Printf(TEXT("; it approves again %d request(s) the auto-approval it replaced had approved before they started"), Carried);
	}
	// Whether Sandbox Mode's Approve All, the other approval waiting in Activity, is covered too.
	if (HyperAIStudio::Sandbox::GetStatus(/*bWithChanges=*/false).State != HyperAIStudio::Sandbox::EState::Off)
	{
		const int32 HeldFiles = HyperAIStudio::Sandbox::GetHeldCount();
		AlsoWaiting += (Current()->CategoryMask & Bit(EHyperAIStudioApprovalCategory::SandboxSaves))
			? FString::Printf(TEXT("; Sandbox Mode is on, holding %d file(s), and its held saves are covered: they are written to the project at a quiet moment"), HeldFiles)
			: FString::Printf(TEXT("; Sandbox Mode is on, holding %d file(s), and its held saves are NOT covered (not ticked in its dialog): they wait for Approve All in Activity"), HeldFiles);
	}
	if (bTest)
	{
		// What a test turns on covers only its own requests, so it is recorded as such and tells the user nothing.
		UE_LOG(LogHyperAIStudioAutoApproval, Display, TEXT("Auto-approval on for an automation test's own requests: %s (%s)%s%s."),
			*Covered, *Lasting, *AlsoWaiting, KeptOn);
		RecordAudit(TEXT("turned on"), TEXT("auto_approval_on"),
			FString::Printf(TEXT("%s %s (automation test; it covers only the test's own requests%s%s)"), *Covered, *Lasting, *AlsoWaiting, KeptOn));
	}
	else
	{
		UE_LOG(LogHyperAIStudioAutoApproval, Warning,
			TEXT("Auto-approval ON for %s (%s), %s%s%s. It covers every agent and MCP client connected to this editor."),
			*Covered, *Lasting, Confirmed, *AlsoWaiting, KeptOn);
		RecordAudit(TEXT("turned on"), TEXT("auto_approval_on"),
			FString::Printf(TEXT("%s %s (%s%s%s)"), *Covered, *Lasting, Confirmed, *AlsoWaiting, KeptOn));
		Notify(FText::Format(bStayOnAfterRestart
			? LOCTEXT("OnToastKeptOn",
				"Auto-approval is on {0}: {1}. Requests in these categories now run without asking you, and it stays on if the editor restarts; Stop is in Activity and in each chat window.")
			: LOCTEXT("OnToast",
				"Auto-approval is on {0}: {1}. Requests in these categories now run without asking you; Stop is in Activity and in each chat window."),
			DurationPhrase(Duration, CustomHours), DescribeCategories(Categories)), /*bWarning=*/true);
	}
	OnChanged().Broadcast();
	FHyperAIStudioApprovalGate::OnChanged().Broadcast();
	return true;
}

bool FHyperAIStudioAutoApproval::IsTypedCommand(const FString& Text)
{
	using namespace HyperAIStudio::AutoApproval::Private;
	TArray<FString> Words;
	Text.ParseIntoArrayWS(Words);
	return Words.Num() >= 2 && Words[0].Equals(CommandVerb, ESearchCase::IgnoreCase) && Words[1].Equals(CommandName, ESearchCase::IgnoreCase);
}

bool FHyperAIStudioAutoApproval::RunTypedCommand(const FString& Text, const bool bEnterKeyIsReal, FText& OutMessage)
{
	using namespace HyperAIStudio::AutoApproval::Private;
	if (!IsTypedCommand(Text))
	{
		return false;
	}
	TArray<FString> Words;
	Text.ParseIntoArrayWS(Words);
	FHyperAIStudioAutoApprovalChoices Choices = LoadChoices();
	if (Words.Num() > 3 || (Words.Num() == 3 && !ParseCommandHours(Words[2], Choices.CustomHours)))
	{
		OutMessage = FText::Format(LOCTEXT("CommandHoursUsage", "Auto-approval is unchanged. After the command, name the hours from {0} to {1}, such as 8h, or nothing."),
			HyperAIStudio::AutoApproval::MinCustomHours, HyperAIStudio::AutoApproval::MaxCustomHours);
		return false;
	}
	if (Words.Num() == 3)
	{
		Choices.Duration = EHyperAIStudioAutoApprovalDuration::Custom;
	}
	if (!bEnterKeyIsReal)
	{
		UE_LOG(LogHyperAIStudioAutoApproval, Display, TEXT("The auto-approval command was refused: it did not come with the keyboard's Enter key held down."));
		RecordAudit(TEXT("command refused"), TEXT("auto_approval_command_refused"), TEXT("Not sent with the keyboard's Enter key"));
		OutMessage = LOCTEXT("CommandNeedsEnter", "Auto-approval is unchanged. This command works only with the Enter key on your keyboard: press Enter, holding it a moment if the editor is slow.");
		return false;
	}
	TArray<EHyperAIStudioApprovalCategory> Categories;
	const bool bSandboxOn = HyperAIStudio::Sandbox::GetStatus(/*bWithChanges=*/false).State != HyperAIStudio::Sandbox::EState::Off;
	for (const EHyperAIStudioApprovalCategory Category : Choices.Categories)
	{
		// As in the dialog, which offers Sandbox Mode's held saves only while Sandbox Mode is on.
		if (Category != EHyperAIStudioApprovalCategory::SandboxSaves || bSandboxOn)
		{
			Categories.Add(Category);
		}
	}
	TSet<FString> ChosenWaiting;
	if (Choices.bAlsoApproveWaiting)
	{
		for (const FHyperAIStudioPendingApproval& Pending : FHyperAIStudioApprovalGate::GetPending())
		{
			if (Pending.Category.IsSet() && *Pending.Category != EHyperAIStudioApprovalCategory::SandboxSaves && Categories.Contains(*Pending.Category))
			{
				ChosenWaiting.Add(Pending.OperationId);
			}
		}
	}
	FString Error;
	if (!Activate(Categories, Choices.Duration, Choices.CustomHours, ChosenWaiting, Choices.bStayOnAfterRestart, Error, /*bTyped=*/true))
	{
		OutMessage = FText::FromString(Error);
		return false;
	}
	OutMessage = FText::Format(LOCTEXT("CommandOn", "Auto-approval is on {0}: {1}. Stop is in Activity and above this chat."),
		DurationPhrase(Choices.Duration, Choices.CustomHours), DescribeCategories(Categories));
	return true;
}

bool FHyperAIStudioAutoApproval::IsEnterKeyDown()
{
#if PLATFORM_WINDOWS
	return (::GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
#else
	return false;
#endif
}

void FHyperAIStudioAutoApproval::Stop(const EHyperAIStudioAutoApprovalEnd Why)
{
	using namespace HyperAIStudio::AutoApproval::Private;
	if (!Current().IsSet())
	{
		return;
	}
	if (ExpiryHandle().IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ExpiryHandle());
		ExpiryHandle().Reset();
	}
	FHyperAIStudioAgentActivityLog::OnChanged().Remove(ActivityHandle());
	ActivityHandle().Reset();
	const uint64 Ended = Current()->Id;
	const bool bTest = IsTestScoped(*Current());
	// Kept on and the editor closing: its record stays, with the close time, for the next editor. Any other end deletes
	// it (Stop, expiry, a limit, a spend-setting change, a new activation), and so does the end of one not kept on.
	const bool bKeptForRestart = Why == EHyperAIStudioAutoApprovalEnd::EditorClosing && Current()->bKeepOn && !RecordPathFor(*Current()).IsEmpty();
	if (bKeptForRestart)
	{
		WriteRecord(*Current(), /*bClosing=*/true);
	}
	else
	{
		DeleteRecord(*Current());
	}
	Current().Reset();
	CloseRestoreNotice();
	SandboxWait() = FSandboxWait();
#if WITH_DEV_AUTOMATION_TESTS
	LastEnd() = Why;
#endif

	// Held requests are left as they are: they wait for a click again. So does what it approved that is still queued
	// behind other edits, with its grant not yet issued; work that has started goes on, as after a click.
	ReturnedByStop().Reset();
	const int32 Returned = HyperAIStudio::Approval::Private::ReturnUnstarted(Ended, &ReturnedByStop());
	FString Reason = EndReason(Why);
	if (bTest && Why != EHyperAIStudioAutoApprovalEnd::TestFinished)
	{
		Reason += TEXT(" (automation test)");
	}
	if (bKeptForRestart)
	{
		Reason += TEXT("; it was kept on after restarts, so its record stays for the next editor");
	}
	if (Returned > 0)
	{
		Reason += Why == EHyperAIStudioAutoApprovalEnd::Replaced
			? FString::Printf(TEXT("; %d approved request(s) that had not started went back to waiting, and the new auto-approval approves again those in its categories"), Returned)
			: FString::Printf(TEXT("; %d approved request(s) that had not started wait for a click again"), Returned);
	}
	UE_LOG(LogHyperAIStudioAutoApproval, Display, TEXT("Auto-approval OFF: %s."), *Reason);
	RecordAudit(TEXT("turned off"), TEXT("auto_approval_off"), Reason);
	if (bTest)
	{
		OnChanged().Broadcast();
		FHyperAIStudioApprovalGate::OnChanged().Broadcast();
		return;
	}
	switch (Why)
	{
	case EHyperAIStudioAutoApprovalEnd::Stopped:
	case EHyperAIStudioAutoApprovalEnd::StoppedRemotely:
		Notify(LOCTEXT("OffToast", "Auto-approval is off. Agents' requests wait for your approval again, including any it approved that had not started."), false);
		break;
	case EHyperAIStudioAutoApprovalEnd::SpendSettingsChanged:
		Notify(LOCTEXT("SpendSettingsToast",
			"Auto-approval turned itself off: the spend cap, an external service, an image model or the approval limit changed in HyperAI Studio's settings. Check them before turning it on again."), true);
		break;
	case EHyperAIStudioAutoApprovalEnd::Replaced:
	case EHyperAIStudioAutoApprovalEnd::EditorClosing:
	case EHyperAIStudioAutoApprovalEnd::TestFinished:
		// The new activation's toast, or the editor closing, says enough.
		break;
	case EHyperAIStudioAutoApprovalEnd::Expired:
		Notify(LOCTEXT("ExpiredToast", "Auto-approval ended: its time ran out. Agents' requests wait for your approval again."), false);
		break;
	case EHyperAIStudioAutoApprovalEnd::ApprovalLimit:
		Notify(FText::Format(LOCTEXT("ApprovalLimitToast",
			"Auto-approval turned itself off after {0} approvals. Turn it on again from Activity to go on."), GetMaxApprovals()), true);
		break;
	case EHyperAIStudioAutoApprovalEnd::ExternalEffectLimit:
		Notify(FText::Format(LOCTEXT("ExternalLimitToast",
			"Auto-approval turned itself off after {0} external effects. Turn it on again from Activity to go on."), MaxExternalEffects), true);
		break;
	case EHyperAIStudioAutoApprovalEnd::RepeatedFailures:
		Notify(FText::Format(LOCTEXT("FailuresToast",
			"Auto-approval turned itself off: the last {0} requests it approved failed. Check Activity before turning it on again."),
			MaxConsecutiveFailures), true);
		break;
	default:
		break;
	}
	OnChanged().Broadcast();
	FHyperAIStudioApprovalGate::OnChanged().Broadcast();
}

FSimpleMulticastDelegate& FHyperAIStudioAutoApproval::OnChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

void FHyperAIStudioAutoApproval::OpenDialog(const TSharedPtr<const SWidget>& Parent)
{
	if (!IsInGameThread() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("DialogTitle", "Turn on auto-approval?"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false);
	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Dialog = SNew(SHyperAIStudioAutoApprovalDialog).Window(Window);
	Window->SetContent(Dialog);
	Window->SetWidgetToFocusOnActivate(Dialog->FocusOnOpen());
	// Modal: while it is open the editor's main loop, and so every MCP request, waits.
	FSlateApplication::Get().AddModalWindow(Window, Parent);
}

FText FHyperAIStudioAutoApproval::GetCategoryName(const EHyperAIStudioApprovalCategory Category)
{
	switch (Category)
	{
	case EHyperAIStudioApprovalCategory::Scripts: return LOCTEXT("ScriptsShort", "Scripts");
	case EHyperAIStudioApprovalCategory::Destructive: return LOCTEXT("DestructiveShort", "Destructive");
	case EHyperAIStudioApprovalCategory::ExternalEffects: return LOCTEXT("ExternalShort", "External effects");
	case EHyperAIStudioApprovalCategory::SandboxSaves: return LOCTEXT("SandboxShort", "Sandbox saves");
	default: return LOCTEXT("EditsShort", "Edits");
	}
}

FText FHyperAIStudioAutoApproval::DescribeCategories(const TArray<EHyperAIStudioApprovalCategory>& Categories)
{
	TArray<FText> Names;
	for (const EHyperAIStudioApprovalCategory Category : Categories)
	{
		Names.Add(GetCategoryName(Category));
	}
	return FText::Join(LOCTEXT("CategorySeparator", ", "), Names);
}

FText FHyperAIStudioAutoApproval::DescribeTimeLeft()
{
	const FHyperAIStudioAutoApprovalStatus Status = GetStatus();
	if (Status.SecondsLeft < 0.0)
	{
		return LOCTEXT("LeftUntilClose", "until the editor closes");
	}
	const int32 Seconds = FMath::CeilToInt(Status.SecondsLeft);
	if (Seconds >= 3600)
	{
		return FText::Format(LOCTEXT("LeftHours", "{0} h {1} min left"), Seconds / 3600, (Seconds % 3600) / 60);
	}
	if (Seconds >= 60)
	{
		return FText::Format(LOCTEXT("LeftMinutes", "{0} min left"), FMath::DivideAndRoundUp(Seconds, 60));
	}
	return FText::Format(LOCTEXT("LeftSeconds", "{0} s left"), Seconds);
}

FText FHyperAIStudioAutoApproval::DescribeBanner()
{
	return FText::Format(LOCTEXT("Banner", "Auto-approval on: {0} ({1})"), DescribeCategories(GetStatus().Categories), DescribeTimeLeft());
}

FText FHyperAIStudioAutoApproval::DescribeBannerTooltip()
{
	const FHyperAIStudioAutoApprovalStatus Status = GetStatus();
	FText Tooltip;
	if (Status.Categories.Contains(EHyperAIStudioApprovalCategory::SandboxSaves))
	{
		// Each write of the held saves is one of the approvals, so a session of many saves reaches the limit sooner.
		Tooltip = FText::Format(LOCTEXT("BannerTooltipSandbox",
			"Agents' requests in these categories run without asking you, from any agent or MCP client connected to this editor. {0} approved so far, {2} of them writes of Sandbox Mode's held saves; it turns itself off after {1}. Requests in other categories still wait for you."),
			Status.Approved, GetMaxApprovals(), Status.SandboxWrites);
	}
	else
	{
		Tooltip = FText::Format(LOCTEXT("BannerTooltip",
			"Agents' requests in these categories run without asking you, from any agent or MCP client connected to this editor. {0} approved so far; it turns itself off after {1}. Requests in other categories still wait for you."),
			Status.Approved, GetMaxApprovals());
	}
	if (Status.bRestored)
	{
		return FText::Format(LOCTEXT("BannerTooltipRestored", "{0}\n\nIt came back on by itself when the editor restarted, as you chose with \"Stay on after the editor restarts\"; the counts carried over."), Tooltip);
	}
	if (Status.bStaysOnAfterRestart)
	{
		return FText::Format(LOCTEXT("BannerTooltipKeptOn", "{0}\n\nIt stays on if the editor restarts (\"Stay on after the editor restarts\" was ticked)."), Tooltip);
	}
	return Tooltip;
}

FText FHyperAIStudioAutoApproval::DescribeRestored(const TArray<EHyperAIStudioApprovalCategory>& Categories, const TOptional<FDateTime>& ExpiresUtc, const int32 Waiting)
{
	// AsTime shows a UTC time in the user's own time zone.
	const FText Until = ExpiresUtc.IsSet() ? FText::AsTime(*ExpiresUtc, EDateTimeStyle::Short) : LOCTEXT("RestoredUntilClose", "the editor closes");
	const FText Text = FText::Format(LOCTEXT("RestoredToast", "Auto-approval is back on from before the restart: {0}, until {1}."), DescribeCategories(Categories), Until);
	if (Waiting <= 0)
	{
		return Text;
	}
	return FText::Format(LOCTEXT("RestoredWaiting",
		"{0} It approves only new requests: the {1} {1}|plural(one=request,other=requests) waiting from before {1}|plural(one=keeps,other=keep) waiting for you."),
		Text, Waiting);
}

const TCHAR* FHyperAIStudioAutoApproval::GetChoicesSection()
{
	return TEXT("HyperAIStudio.AutoApprovalDialog");
}

FHyperAIStudioAutoApprovalChoices FHyperAIStudioAutoApproval::ReadChoices(const FString& IniFile)
{
	using namespace HyperAIStudio::AutoApproval::Private;
	FHyperAIStudioAutoApprovalChoices Choices;
	if (!GConfig || IniFile.IsEmpty())
	{
		return Choices;
	}
	const TCHAR* Section = GetChoicesSection();
	FString Text;
	if (GConfig->GetString(Section, TEXT("Categories"), Text, IniFile))
	{
		TArray<FString> Names;
		Text.ParseIntoArray(Names, TEXT(","));
		uint8 Mask = 0;
		bool bAllKnown = !Names.IsEmpty();
		for (FString& Name : Names)
		{
			Name.TrimStartAndEndInline();
			bool bKnown = false;
			for (const EHyperAIStudioApprovalCategory Category : AllCategories)
			{
				if (Name == LexToString(Category))
				{
					Mask |= Bit(Category);
					bKnown = true;
				}
			}
			bAllKnown &= bKnown;
		}
		// Anything not understood keeps the default rather than a guess.
		if (bAllKnown && Mask != 0)
		{
			Choices.Categories = CategoriesOf(Mask);
		}
	}
	if (GConfig->GetString(Section, TEXT("Duration"), Text, IniFile))
	{
		HyperAIStudio::AutoApproval::KeepOn::ParseDuration(Text.TrimStartAndEnd(), Choices.Duration);
	}
	double Hours = 0.0;
	if (GConfig->GetDouble(Section, TEXT("CustomHours"), Hours, IniFile))
	{
		Choices.CustomHours = HyperAIStudio::AutoApproval::ClampCustomHours(Hours);
	}
	bool bValue = false;
	if (GConfig->GetBool(Section, TEXT("AlsoApproveWaiting"), bValue, IniFile))
	{
		Choices.bAlsoApproveWaiting = bValue;
	}
	if (GConfig->GetBool(Section, TEXT("StayOnAfterRestart"), bValue, IniFile))
	{
		Choices.bStayOnAfterRestart = bValue;
	}
	return Choices;
}

void FHyperAIStudioAutoApproval::WriteChoices(const FString& IniFile, const FHyperAIStudioAutoApprovalChoices& Choices)
{
	if (!GConfig || IniFile.IsEmpty())
	{
		return;
	}
	TArray<FString> Names;
	for (const EHyperAIStudioApprovalCategory Category : Choices.Categories)
	{
		Names.Add(LexToString(Category));
	}
	const TCHAR* Section = GetChoicesSection();
	GConfig->SetString(Section, TEXT("Categories"), *FString::Join(Names, TEXT(",")), IniFile);
	GConfig->SetString(Section, TEXT("Duration"), HyperAIStudio::AutoApproval::KeepOn::DurationKey(Choices.Duration), IniFile);
	GConfig->SetDouble(Section, TEXT("CustomHours"), Choices.CustomHours, IniFile);
	GConfig->SetBool(Section, TEXT("AlsoApproveWaiting"), Choices.bAlsoApproveWaiting, IniFile);
	GConfig->SetBool(Section, TEXT("StayOnAfterRestart"), Choices.bStayOnAfterRestart, IniFile);
	GConfig->Flush(/*bRemoveFromCache=*/false, IniFile);
}

FHyperAIStudioAutoApprovalChoices FHyperAIStudioAutoApproval::LoadChoices()
{
#if WITH_DEV_AUTOMATION_TESTS
	// A dialog under test never reads the user's choices: it starts from the defaults unless a test asked it to remember.
	if (GIsAutomationTesting)
	{
		const HyperAIStudio::AutoApproval::Private::FTestChoices& Test = HyperAIStudio::AutoApproval::Private::TestChoices();
		return Test.bRemember && Test.Saved.IsSet() ? *Test.Saved : FHyperAIStudioAutoApprovalChoices();
	}
#endif
	return ReadChoices(GEditorPerProjectIni);
}

void FHyperAIStudioAutoApproval::SaveChoices(const FHyperAIStudioAutoApprovalChoices& Choices)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (GIsAutomationTesting)
	{
		HyperAIStudio::AutoApproval::Private::FTestChoices& Test = HyperAIStudio::AutoApproval::Private::TestChoices();
		if (Test.bRemember)
		{
			Test.Saved = Choices;
		}
		return;
	}
#endif
	WriteChoices(GEditorPerProjectIni, Choices);
}

#if WITH_DEV_AUTOMATION_TESTS
TOptional<TSet<FString>>& FHyperAIStudioAutoApproval::TestScopeForNextActivation()
{
	static TOptional<TSet<FString>> Scope;
	return Scope;
}

void FHyperAIStudioAutoApprovalTestAccess::AdvanceClock(const double Seconds)
{
	HyperAIStudio::AutoApproval::Private::ClockOffset() += Seconds;
}

bool FHyperAIStudioAutoApprovalTestAccess::SkipWhileUserActivationOn(FAutomationTestBase& Test)
{
	using namespace HyperAIStudio::AutoApproval::Private;
	if (!Current().IsSet() || IsTestScoped(*Current()))
	{
		return false;
	}
	Test.AddWarning(TEXT("The user's own auto-approval is on in this editor; this test leaves it alone and checks nothing."));
	return true;
}

void FHyperAIStudioAutoApprovalTestAccess::Reset()
{
	using namespace HyperAIStudio::AutoApproval::Private;
	if (Current().IsSet() && IsTestScoped(*Current()))
	{
		FHyperAIStudioAutoApproval::Stop(EHyperAIStudioAutoApprovalEnd::TestFinished);
	}
	FHyperAIStudioAutoApproval::TestScopeForNextActivation().Reset();
	ClockOffset() = 0.0;
	LastEnd().Reset();
	TestRecordPath().Reset();
	TestChoices() = FTestChoices();
	LastNotice().Reset();
}

bool FHyperAIStudioAutoApprovalTestAccess::RunCommand(const FString& Text, const bool bEnterKeyIsReal, const TSet<FString>& OperationIds, FText& OutMessage)
{
	FHyperAIStudioAutoApproval::TestScopeForNextActivation() = OperationIds;
	const bool bOn = FHyperAIStudioAutoApproval::RunTypedCommand(Text, bEnterKeyIsReal, OutMessage);
	FHyperAIStudioAutoApproval::TestScopeForNextActivation().Reset();
	return bOn;
}

void FHyperAIStudioAutoApprovalTestAccess::SetRecordPath(const FString& Path)
{
	HyperAIStudio::AutoApproval::Private::TestRecordPath() = Path;
}

void FHyperAIStudioAutoApprovalTestAccess::RememberChoices(const bool bRemember)
{
	HyperAIStudio::AutoApproval::Private::TestChoices().bRemember = bRemember;
}

void FHyperAIStudioAutoApprovalTestAccess::CloseEditor()
{
	using namespace HyperAIStudio::AutoApproval::Private;
	// Only a test's own activation: the user's own is never ended by a test.
	if (Current().IsSet() && IsTestScoped(*Current()))
	{
		FHyperAIStudioAutoApproval::Stop(EHyperAIStudioAutoApprovalEnd::EditorClosing);
	}
}

HyperAIStudio::AutoApproval::KeepOn::ERefusal FHyperAIStudioAutoApprovalTestAccess::Restore(const FString& Path, const TSet<FString>& OperationIds)
{
	return HyperAIStudio::AutoApproval::Private::RestoreFrom(Path, OperationIds);
}

TOptional<FText> FHyperAIStudioAutoApprovalTestAccess::GetLastNotice()
{
	const TOptional<FNotificationInfo>& Notice = HyperAIStudio::AutoApproval::Private::LastNotice();
	return Notice.IsSet() ? TOptional<FText>(Notice->Text.Get()) : TOptional<FText>();
}

TArray<FText> FHyperAIStudioAutoApprovalTestAccess::GetLastNoticeButtons()
{
	TArray<FText> Labels;
	if (const TOptional<FNotificationInfo>& Notice = HyperAIStudio::AutoApproval::Private::LastNotice(); Notice.IsSet())
	{
		for (const FNotificationButtonInfo& Button : Notice->ButtonDetails)
		{
			Labels.Add(Button.Text.Get());
		}
	}
	return Labels;
}

bool FHyperAIStudioAutoApprovalTestAccess::PressLastNoticeButton(const int32 Index)
{
	const TOptional<FNotificationInfo> Notice = HyperAIStudio::AutoApproval::Private::LastNotice();
	if (!Notice.IsSet() || !Notice->ButtonDetails.IsValidIndex(Index))
	{
		return false;
	}
	return Notice->ButtonDetails[Index].Callback.ExecuteIfBound();
}

FDateTime FHyperAIStudioAutoApprovalTestAccess::GetUtcNow()
{
	return HyperAIStudio::AutoApproval::Private::UtcNow();
}

FString FHyperAIStudioAutoApprovalTestAccess::GetSpendFingerprint()
{
	return HyperAIStudio::AutoApproval::KeepOn::Fingerprint(HyperAIStudio::AutoApproval::Private::SpendSettings());
}

void FHyperAIStudioAutoApprovalTestAccess::PumpGate(const int32 Ticks)
{
	for (int32 Tick = 0; Tick < Ticks; ++Tick)
	{
		TickExpiry();
		HyperAIStudio::Approval::Private::SweepForTests();
	}
}

void FHyperAIStudioAutoApprovalTestAccess::PumpSandbox()
{
	HyperAIStudio::AutoApproval::Private::SandboxWait().NextCheck = 0.0;
	HyperAIStudio::AutoApproval::Private::SweepSandbox();
}

double FHyperAIStudioAutoApprovalTestAccess::GetSandboxRetrySeconds()
{
	return HyperAIStudio::AutoApproval::Private::SandboxRetrySeconds;
}

bool FHyperAIStudioAutoApprovalTestAccess::TickExpiry()
{
	const bool bRegistered = HyperAIStudio::AutoApproval::Private::ExpiryHandle().IsValid();
	if (bRegistered)
	{
		FHyperAIStudioAutoApproval::EndIfOver();
	}
	return bRegistered;
}

TOptional<EHyperAIStudioAutoApprovalEnd> FHyperAIStudioAutoApprovalTestAccess::GetLastEnd()
{
	return HyperAIStudio::AutoApproval::Private::LastEnd();
}
#endif

#undef LOCTEXT_NAMESPACE
