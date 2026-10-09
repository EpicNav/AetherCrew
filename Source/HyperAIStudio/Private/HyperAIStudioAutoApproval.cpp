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
#include "HyperAIStudioSettings.h"
#include "SHyperAIStudioAutoApprovalDialog.h"
#include "Styling/AppStyle.h"
#include "UObject/UnrealType.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

#define LOCTEXT_NAMESPACE "HyperAIStudioAutoApproval"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAutoApproval, Log, All);

namespace HyperAIStudio::AutoApproval::Private
{
	constexpr int32 MaxTracked = 256;
	constexpr EHyperAIStudioApprovalCategory AllCategories[] = {
		EHyperAIStudioApprovalCategory::Edits,
		EHyperAIStudioApprovalCategory::Scripts,
		EHyperAIStudioApprovalCategory::Destructive,
		EHyperAIStudioApprovalCategory::ExternalEffects};

	struct FActivation
	{
		/** Never 0, never reused: work it approved runs only while this is the current one. */
		uint64 Id = 0;
		uint8 CategoryMask = 0;
		EHyperAIStudioAutoApprovalDuration Duration = EHyperAIStudioAutoApprovalDuration::OneHour;
		/** When it ends, on the monotonic clock; 0 while it lasts until the editor closes. */
		double EndsAt = 0.0;
		/** Requests held before it started keep waiting for a click. */
		uint64 FirstSequence = 0;
		/** The spend settings as the user confirmed it under; it ends if they change. */
		FString SpendSettings;
		int32 Approved = 0;
		int32 ExternalApproved = 0;
		int32 ConsecutiveFailures = 0;
		/** Operations it approved whose outcome is not in yet, oldest first. */
		TArray<FString> Approving;
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
	 * What paid calls are checked against: the session spend cap, the external services (with their prices) and the
	 * image models (with theirs). Any MCP client can write HyperAI's settings (Epic's settings toolset asks nobody), so an
	 * activation keeps them as the user confirmed it and ends when they change, rather than approving paid calls under a
	 * cap or a price an agent set afterwards.
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
			GET_MEMBER_NAME_CHECKED(UHyperAIStudioSettings, ImageModels)};
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

	uint8 Bit(const EHyperAIStudioApprovalCategory Category)
	{
		return static_cast<uint8>(1u << static_cast<uint8>(Category));
	}

	double DurationSeconds(const EHyperAIStudioAutoApprovalDuration Duration)
	{
		switch (Duration)
		{
		case EHyperAIStudioAutoApprovalDuration::FifteenMinutes: return 15.0 * 60.0;
		case EHyperAIStudioAutoApprovalDuration::FourHours: return 4.0 * 3600.0;
		case EHyperAIStudioAutoApprovalDuration::UntilEditorCloses: return 0.0;
		default: return 3600.0;
		}
	}

	const TCHAR* DurationName(const EHyperAIStudioAutoApprovalDuration Duration)
	{
		switch (Duration)
		{
		case EHyperAIStudioAutoApprovalDuration::FifteenMinutes: return TEXT("for 15 minutes");
		case EHyperAIStudioAutoApprovalDuration::FourHours: return TEXT("for 4 hours");
		case EHyperAIStudioAutoApprovalDuration::UntilEditorCloses: return TEXT("until the editor closes");
		default: return TEXT("for 1 hour");
		}
	}

	FText DurationPhrase(const EHyperAIStudioAutoApprovalDuration Duration)
	{
		switch (Duration)
		{
		case EHyperAIStudioAutoApprovalDuration::FifteenMinutes: return LOCTEXT("ForFifteenMinutes", "for 15 minutes");
		case EHyperAIStudioAutoApprovalDuration::FourHours: return LOCTEXT("ForFourHours", "for 4 hours");
		case EHyperAIStudioAutoApprovalDuration::UntilEditorCloses: return LOCTEXT("UntilClose", "until the editor closes");
		default: return LOCTEXT("ForOneHour", "for 1 hour");
		}
	}

	/** Why it ended, in the activity log's and the log's English. */
	FString EndReason(const EHyperAIStudioAutoApprovalEnd Why)
	{
		switch (Why)
		{
		case EHyperAIStudioAutoApprovalEnd::StoppedRemotely: return TEXT("stopped on the remote page");
		case EHyperAIStudioAutoApprovalEnd::Expired: return TEXT("its time ran out");
		case EHyperAIStudioAutoApprovalEnd::ApprovalLimit:
			return FString::Printf(TEXT("%d approvals reached"), FHyperAIStudioAutoApproval::MaxApprovals);
		case EHyperAIStudioAutoApprovalEnd::ExternalEffectLimit:
			return FString::Printf(TEXT("%d external effects reached"), FHyperAIStudioAutoApproval::MaxExternalEffects);
		case EHyperAIStudioAutoApprovalEnd::RepeatedFailures:
			return FString::Printf(TEXT("%d auto-approved requests in a row failed"), FHyperAIStudioAutoApproval::MaxConsecutiveFailures);
		case EHyperAIStudioAutoApprovalEnd::SpendSettingsChanged:
			return TEXT("the spend cap, an external service or an image model changed in HyperAI Studio's settings");
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

	bool TickExpiry(float)
	{
		FHyperAIStudioAutoApproval::EndIfOver();
		if (!Current().IsSet())
		{
			ExpiryHandle().Reset();
			return false;
		}
		return true;
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
			Active->ConsecutiveFailures = 0;
		}
		else if (Newest.Kind == EHyperAIStudioActivityKind::Failed)
		{
			Active->Approving.Remove(Newest.OperationId);
			if (++Active->ConsecutiveFailures >= FHyperAIStudioAutoApproval::MaxConsecutiveFailures)
			{
				FHyperAIStudioAutoApproval::Stop(EHyperAIStudioAutoApprovalEnd::RepeatedFailures);
			}
		}
	}
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
	Status.SecondsLeft = Active->EndsAt > 0.0 ? FMath::Max(0.0, Active->EndsAt - Now()) : -1.0;
	Status.Approved = Active->Approved;
	Status.ExternalEffectsApproved = Active->ExternalApproved;
	return Status;
}

bool FHyperAIStudioAutoApproval::Covers(const EHyperAIStudioApprovalCategory Category, const uint64 Sequence, const FString& OperationId)
{
	using namespace HyperAIStudio::AutoApproval::Private;
	const TOptional<FActivation>& Active = Current();
	if (!Active.IsSet() || !(Active->CategoryMask & Bit(Category)))
	{
		return false;
	}
	if ((Active->EndsAt > 0.0 && Now() >= Active->EndsAt)
		|| Active->Approved >= MaxApprovals
		|| (Category == EHyperAIStudioApprovalCategory::ExternalEffects && Active->ExternalApproved >= MaxExternalEffects)
		|| Active->ConsecutiveFailures >= MaxConsecutiveFailures)
	{
		return false;
	}
	if (Sequence < Active->FirstSequence)
	{
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (Active->OnlyOperations.IsSet() && (!GIsAutomationTesting || !Active->OnlyOperations->Contains(OperationId)))
	{
		return false;
	}
#endif
	return true;
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
	if (Active->Approved >= MaxApprovals)
	{
		Stop(EHyperAIStudioAutoApprovalEnd::ApprovalLimit);
	}
	else if (Active->ExternalApproved >= MaxExternalEffects)
	{
		Stop(EHyperAIStudioAutoApprovalEnd::ExternalEffectLimit);
	}
	else
	{
		OnChanged().Broadcast();
	}
}

bool FHyperAIStudioAutoApproval::Activate(
	const TArray<EHyperAIStudioApprovalCategory>& Categories,
	const EHyperAIStudioAutoApprovalDuration Duration,
	FString& OutError)
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
	if (Current().IsSet())
	{
		Stop(EHyperAIStudioAutoApprovalEnd::Replaced);
	}

	FActivation Activation;
	Activation.Id = ++LastActivationId();
	for (const EHyperAIStudioApprovalCategory Category : Categories)
	{
		Activation.CategoryMask |= Bit(Category);
	}
	Activation.Duration = Duration;
	const double Seconds = DurationSeconds(Duration);
	Activation.EndsAt = Seconds > 0.0 ? Now() + Seconds : 0.0;
	Activation.FirstSequence = HyperAIStudio::Approval::Private::PeekNextSequence();
	Activation.SpendSettings = SpendSettings();
#if WITH_DEV_AUTOMATION_TESTS
	Activation.OnlyOperations = MoveTemp(TestScope);
#endif
	const bool bTest = IsTestScoped(Activation);
	Current() = MoveTemp(Activation);
	ExpiryHandle() = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickExpiry), 1.0f);
	ActivityHandle() = FHyperAIStudioAgentActivityLog::OnChanged().AddStatic(&OnActivity);

	TArray<FString> Names;
	for (const EHyperAIStudioApprovalCategory Category : Categories)
	{
		Names.Add(EnglishName(Category));
	}
	const FString Covered = FString::Join(Names, TEXT(", "));
	if (bTest)
	{
		// What a test turns on covers only its own requests, so it is recorded as such and tells the user nothing.
		UE_LOG(LogHyperAIStudioAutoApproval, Display, TEXT("Auto-approval on for an automation test's own requests: %s (%s)."),
			*Covered, DurationName(Duration));
		RecordAudit(TEXT("turned on"), TEXT("auto_approval_on"),
			FString::Printf(TEXT("%s %s (automation test; it covers only the test's own requests)"), *Covered, DurationName(Duration)));
	}
	else
	{
		UE_LOG(LogHyperAIStudioAutoApproval, Warning,
			TEXT("Auto-approval ON for %s (%s), confirmed in its dialog. It covers every agent and MCP client connected to this editor."),
			*Covered, DurationName(Duration));
		RecordAudit(TEXT("turned on"), TEXT("auto_approval_on"),
			FString::Printf(TEXT("%s %s (confirmed in its dialog)"), *Covered, DurationName(Duration)));
		Notify(FText::Format(LOCTEXT("OnToast",
			"Auto-approval is on {0}: {1}. Requests in these categories now run without asking you; Stop is in Activity and in each chat window."),
			DurationPhrase(Duration), DescribeCategories(Categories)), /*bWarning=*/true);
	}
	OnChanged().Broadcast();
	FHyperAIStudioApprovalGate::OnChanged().Broadcast();
	return true;
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
	Current().Reset();
#if WITH_DEV_AUTOMATION_TESTS
	LastEnd() = Why;
#endif

	// Held requests are left as they are: they wait for a click again. So does what it approved that is still queued
	// behind other edits, with its grant not yet issued; work that has started goes on, as after a click.
	const int32 Returned = HyperAIStudio::Approval::Private::ReturnUnstarted(Ended);
	FString Reason = EndReason(Why);
	if (bTest && Why != EHyperAIStudioAutoApprovalEnd::TestFinished)
	{
		Reason += TEXT(" (automation test)");
	}
	if (Returned > 0)
	{
		Reason += FString::Printf(TEXT("; %d approved request(s) that had not started wait for a click again"), Returned);
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
			"Auto-approval turned itself off: the spend cap, an external service or an image model changed in HyperAI Studio's settings. Check them before turning it on again."), true);
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
			"Auto-approval turned itself off after {0} approvals. Turn it on again from Activity to go on."), MaxApprovals), true);
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
	return FText::Format(LOCTEXT("BannerTooltip",
		"Agents' requests in these categories run without asking you, from any agent or MCP client connected to this editor. {0} approved so far; it turns itself off after {1}. Requests in other categories still wait for you."),
		GetStatus().Approved, MaxApprovals);
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
}

void FHyperAIStudioAutoApprovalTestAccess::PumpGate(const int32 Ticks)
{
	for (int32 Tick = 0; Tick < Ticks; ++Tick)
	{
		TickExpiry();
		HyperAIStudio::Approval::Private::SweepForTests();
	}
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
