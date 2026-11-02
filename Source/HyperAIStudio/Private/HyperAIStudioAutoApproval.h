// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioApprovalCategory.h"

class SWidget;

namespace HyperAIStudio::AutoApproval::KeepOn
{
	enum class ERefusal : uint8;
}

enum class EHyperAIStudioAutoApprovalDuration : uint8
{
	FifteenMinutes,
	OneHour,
	FourHours,
	EightHours,
	/** As many hours as the user typed, from MinCustomHours to MaxCustomHours. */
	Custom,
	UntilEditorCloses
};

namespace HyperAIStudio::AutoApproval
{
	constexpr double MinCustomHours = 0.25;
	/** A kept-on auto-approval never comes back 24 hours after it was turned on, so no duration outlasts that. */
	constexpr double MaxCustomHours = 24.0;
	constexpr double DefaultCustomHours = 10.0;

	/** Within the limits; DefaultCustomHours for anything that is not a number. */
	double ClampCustomHours(double Hours);
	/** How long a duration lasts; 0 for "until the editor closes". CustomHours counts only for Custom. */
	double DurationSeconds(EHyperAIStudioAutoApprovalDuration Duration, double CustomHours);
}

/** Why auto-approval turned off. */
enum class EHyperAIStudioAutoApprovalEnd : uint8
{
	Stopped,
	StoppedRemotely,
	Expired,
	ApprovalLimit,
	ExternalEffectLimit,
	RepeatedFailures,
	/** The spend cap, an external service or an image model changed in HyperAI's settings while it was on. */
	SpendSettingsChanged,
	/** A new activation took its place. */
	Replaced,
	EditorClosing,
	/** The automation test that turned it on finished; only a test's own activation ends this way. */
	TestFinished
};

struct FHyperAIStudioAutoApprovalStatus
{
	bool bActive = false;
	TArray<EHyperAIStudioApprovalCategory> Categories;
	EHyperAIStudioAutoApprovalDuration Duration = EHyperAIStudioAutoApprovalDuration::OneHour;
	/** The hours it was turned on for, when Duration is Custom. */
	double CustomHours = HyperAIStudio::AutoApproval::DefaultCustomHours;
	/** Negative while it lasts until the editor closes. */
	double SecondsLeft = -1.0;
	int32 Approved = 0;
	int32 ExternalEffectsApproved = 0;
	/** Requests that were waiting when it began which the user chose in its dialog to approve too. */
	int32 ChosenWaiting = 0;
	/** Writes of Sandbox Mode's held saves; each is also one of Approved. */
	int32 SandboxWrites = 0;
	/** "Stay on after the editor restarts" was ticked: a protected record under Saved/ follows it. */
	bool bStaysOnAfterRestart = false;
	/** It came back on by itself as this editor started, from before a restart. */
	bool bRestored = false;
};

/**
 * What the auto-approval dialog opens with: the choices the user last turned it on with, kept in the editor's per-user
 * project settings. UI preferences only: nothing reads them but the dialog, and they never turn anything on.
 */
struct FHyperAIStudioAutoApprovalChoices
{
	TArray<EHyperAIStudioApprovalCategory> Categories = {EHyperAIStudioApprovalCategory::Edits};
	EHyperAIStudioAutoApprovalDuration Duration = EHyperAIStudioAutoApprovalDuration::OneHour;
	/** What the Custom field holds, whichever duration is chosen. */
	double CustomHours = HyperAIStudio::AutoApproval::DefaultCustomHours;
	bool bAlsoApproveWaiting = true;
	bool bStayOnAfterRestart = false;

	bool operator==(const FHyperAIStudioAutoApprovalChoices& Other) const
	{
		return Categories == Other.Categories && Duration == Other.Duration && CustomHours == Other.CustomHours && bAlsoApproveWaiting == Other.bAlsoApproveWaiting
			&& bStayOnAfterRestart == Other.bStayOnAfterRestart;
	}
};

/**
 * For a while the user chose, requests in the categories they chose are approved as their click would approve them.
 *
 * Only two things turn it on: SHyperAIStudioAutoApprovalDialog, after its warning and a native confirmation, and the
 * command the user types in a chat's composer, sent with the Enter key held on the real keyboard (RunTypedCommand). No
 * agent is told of the command or sees it, and nothing reflected, configured, exported or registered as a console
 * command can turn it on. It lives in this process's memory, so it is off at every
 * editor start, unless the user ticked "Stay on after the editor restarts": then a record sealed with Windows DPAPI for
 * this user and project (HyperAIStudioAutoApprovalKeepOn.h) follows it under Saved/, and the next editor brings it back
 * once, if the record is valid, unexpired, younger than 24 hours, under the same spend settings and, for "until the
 * editor closes", the editor opened again within 30 minutes. Anything may turn it off; what it approved that has not
 * started by then waits for a click again. Game thread only.
 */
class FHyperAIStudioAutoApproval
{
public:
	static constexpr int32 MaxApprovals = 50;
	/** External-effect requests, not paid calls: one image request may make several. The spend cap bounds the money. */
	static constexpr int32 MaxExternalEffects = 10;
	static constexpr int32 MaxConsecutiveFailures = 3;

	/**
	 * Module lifetime. Startup waits for the editor's main window, so the notification can be seen, and then brings back
	 * an auto-approval kept on from before a restart; until then it is off, so nothing is auto-approved before it.
	 */
	static void Startup();
	static void Shutdown();

	static bool IsActive();
	/** Names the current activation, so work it approved runs only while it lasts; 0 while off. */
	static uint64 GetActivationId();
	/** The current activation came back on by itself after the editor restarted. */
	static bool IsRestored();
	/** That activation (an id from GetActivationId) came back on after a restart: what it approved is marked so. */
	static bool WasRestored(uint64 ActivationId);
	static FHyperAIStudioAutoApprovalStatus GetStatus();
	/**
	 * Would a request of this category, held at Sequence, be approved now? Reads only: an activation whose time ran out
	 * covers nothing, and EndIfOver turns it off.
	 */
	static bool Covers(EHyperAIStudioApprovalCategory Category, uint64 Sequence, const FString& OperationId);
	/** Why Covers says no, as a clause in the log's English ("destructive is not one of the categories it covers"); empty when it says yes. */
	static FString DescribeWhyNotCovered(const TOptional<EHyperAIStudioApprovalCategory>& Category, uint64 Sequence, const FString& OperationId);
	/**
	 * True for a request held before the current auto-approval began: it covers one only if the user chose it in its
	 * dialog, or if the auto-approval it replaced had approved it before it started (WasCarriedOver).
	 */
	static bool IsFromBeforeStart(uint64 Sequence);
	/**
	 * The auto-approval the current one replaced had approved this request, which went back to waiting because it had not
	 * started; the current one approves it again if its categories cover it.
	 */
	static bool WasCarriedOver(const FString& OperationId);
	/**
	 * Turns off an activation whose time ran out, or whose spend settings (the cap, the external services, the image
	 * models) changed since it started; the gate calls it before each approval decision.
	 */
	static void EndIfOver();
	/**
	 * The user chose Sandbox Mode's held saves in its dialog and HyperAI's own sandbox is open: what it holds is written
	 * at a quiet moment. False while the sandbox waits to open or someone else's is active, where nothing is written.
	 */
	static bool CoversSandboxSaves();
	/**
	 * It is on and could reach HyperAI's open sandbox, but Sandbox Mode's held saves was not ticked in its dialog, so
	 * what the sandbox holds keeps waiting for Approve All.
	 */
	static bool LeavesSandboxSavesWaiting();
	/**
	 * For Sandbox Mode's card in Activity while it is on: when the held saves are written or what that waits for, or that
	 * it does not write them. Empty while it says nothing about the sandbox.
	 */
	static FText DescribeSandboxWait();
	/** The gate is about to approve OperationId for it; its outcome counts toward the failure guard. */
	static void BeginApproval(const FString& OperationId);
	static void NoteApproved(EHyperAIStudioApprovalCategory Category);
	static void Stop(EHyperAIStudioAutoApprovalEnd Why);
	static FSimpleMulticastDelegate& OnChanged();
	/** Opens the warning dialog, modal to Parent's window. It turns on only if the person confirms there. */
	static void OpenDialog(const TSharedPtr<const SWidget>& Parent);
	/** The composer's text is the auto-approval command, well formed or not: it never goes to an agent. */
	static bool IsTypedCommand(const FString& Text);
	/**
	 * Runs the command: on with the choices the user last turned it on with, optionally for the hours it names. No dialog
	 * asks, so bEnterKeyIsReal stands in for the native confirmation: a client that synthesizes Slate input, as Epic's
	 * Slate tools can, does not hold down the keyboard's Enter key. It is no barrier to a program with input of its own
	 * at the OS level, as a native dialog is not. OutMessage says what happened; false when it stayed as it was.
	 */
	static bool RunTypedCommand(const FString& Text, bool bEnterKeyIsReal, FText& OutMessage);
	/** The keyboard's Enter key is down now, as Windows sees it. */
	static bool IsEnterKeyDown();

	/** Edits, Scripts, Destructive, External effects, Sandbox saves. */
	static FText GetCategoryName(EHyperAIStudioApprovalCategory Category);
	static FText DescribeCategories(const TArray<EHyperAIStudioApprovalCategory>& Categories);
	static FText DescribeTimeLeft();
	/** "Auto-approval on: Edits (52 min left)", for the banners. */
	static FText DescribeBanner();
	static FText DescribeBannerTooltip();
	/**
	 * "Auto-approval is back on from before the restart: Edits, until 14:35.", and when requests were already waiting,
	 * that it approves only new ones. ExpiresUtc is unset for "until the editor closes".
	 */
	static FText DescribeRestored(const TArray<EHyperAIStudioApprovalCategory>& Categories, const TOptional<FDateTime>& ExpiresUtc, int32 Waiting);

	/** The choices the dialog opens with: the last ones the user turned it on with, or the defaults. */
	static FHyperAIStudioAutoApprovalChoices LoadChoices();
	/** Section [HyperAIStudio.AutoApprovalDialog] of IniFile; the defaults for anything missing or not understood. */
	static FHyperAIStudioAutoApprovalChoices ReadChoices(const FString& IniFile);
	static void WriteChoices(const FString& IniFile, const FHyperAIStudioAutoApprovalChoices& Choices);
	static const TCHAR* GetChoicesSection();

private:
	friend class SHyperAIStudioAutoApprovalDialog;
	/**
	 * The dialog's confirm path, after the native confirmation. Replaces a running activation, except that a test's
	 * activation never replaces the user's own. ChosenWaiting names requests that were waiting as the dialog opened
	 * and that the user chose there to approve as well; it covers no other request held before it began. With
	 * bStayOnAfterRestart a protected record follows it under Saved/; without, any earlier record is deleted. bTyped says
	 * the typed command turned it on, for the log and Activity.
	 */
	static bool Activate(const TArray<EHyperAIStudioApprovalCategory>& Categories,
		EHyperAIStudioAutoApprovalDuration Duration, double CustomHours, const TSet<FString>& ChosenWaiting, bool bStayOnAfterRestart, FString& OutError,
		bool bTyped = false);
	/** The dialog's Turn On, once it turned on: what the next dialog opens with. */
	static void SaveChoices(const FHyperAIStudioAutoApprovalChoices& Choices);
#if WITH_DEV_AUTOMATION_TESTS
	friend struct FHyperAIStudioAutoApprovalTestAccess;
	/**
	 * Set by the dialog when a test answered its confirmation: the next activation covers only these operation ids,
	 * and only while an automation test runs.
	 */
	static TOptional<TSet<FString>>& TestScopeForNextActivation();
#endif
};

#if WITH_DEV_AUTOMATION_TESTS
class FAutomationTestBase;
struct FKey;
class SHyperAIStudioAutoApprovalBanner;
class SHyperAIStudioAutoApprovalDialog;

/**
 * The tests' way in. Not exported, so only this module's tests can link it. A test can run in the user's own editor
 * (Epic's automation toolset reaches it over MCP), so nothing here ends or replaces the user's own activation: tests
 * skip while one is on.
 */
struct FHyperAIStudioAutoApprovalTestAccess
{
	/** True, with a warning on Test, while the user's own auto-approval is on; the test should then return at once. */
	static bool SkipWhileUserActivationOn(FAutomationTestBase& Test);
	static TSharedRef<SHyperAIStudioAutoApprovalDialog> MakeDialog();
	/** Replaces the native confirmation with this answer. */
	static void SetNativeAnswer(SHyperAIStudioAutoApprovalDialog& Dialog, bool bOk);
	static void SetTicked(SHyperAIStudioAutoApprovalDialog& Dialog, EHyperAIStudioApprovalCategory Category, bool bTicked);
	static bool IsTicked(const SHyperAIStudioAutoApprovalDialog& Dialog, EHyperAIStudioApprovalCategory Category);
	static void SetDuration(SHyperAIStudioAutoApprovalDialog& Dialog, EHyperAIStudioAutoApprovalDuration Duration);
	static EHyperAIStudioAutoApprovalDuration GetDuration(const SHyperAIStudioAutoApprovalDialog& Dialog);
	/** Types hours into the Custom field, as committing it would, which also chooses Custom. */
	static void CommitCustomHours(SHyperAIStudioAutoApprovalDialog& Dialog, double Hours);
	static double GetCustomHours(const SHyperAIStudioAutoApprovalDialog& Dialog);
	/** The typed command as the composer runs it; what it turns on covers only OperationIds. */
	static bool RunCommand(const FString& Text, bool bEnterKeyIsReal, const TSet<FString>& OperationIds, FText& OutMessage);
	static bool CanTurnOn(const SHyperAIStudioAutoApprovalDialog& Dialog);
	/** Presses Turn On through its real handler; the activation covers only OperationIds. */
	static void PressTurnOn(SHyperAIStudioAutoApprovalDialog& Dialog, const TSet<FString>& OperationIds);
	static void PressCancel(SHyperAIStudioAutoApprovalDialog& Dialog);
	/** Sends the dialog a key press through its own OnKeyDown. */
	static void PressKey(SHyperAIStudioAutoApprovalDialog& Dialog, const FKey& Key);
	/** Whether the widget the window focuses on opening (what Enter presses) is the dialog's Cancel button. */
	static bool FocusesCancel(const SHyperAIStudioAutoApprovalDialog& Dialog);
	/** Clicks the banner's Stop button, as a person would. */
	static void PressStop(SHyperAIStudioAutoApprovalBanner& Banner);
	/**
	 * A fresh dialog with these categories and this duration, confirmed: the whole way a person turns it on. Unlike the
	 * dialog's default, it leaves "Also approve the requests waiting now" unticked unless asked, so a test's requests
	 * held before it keep waiting. "Stay on after the editor restarts" is ticked only when asked, and a test's record
	 * is written only where SetRecordPath points.
	 */
	static bool TurnOn(const TArray<EHyperAIStudioApprovalCategory>& Categories, const TSet<FString>& OperationIds,
		EHyperAIStudioAutoApprovalDuration Duration = EHyperAIStudioAutoApprovalDuration::OneHour, bool bAlsoApproveWaiting = false,
		bool bStayOnAfterRestart = false);
	static bool IsStayingOn(const SHyperAIStudioAutoApprovalDialog& Dialog);
	static void SetStayOn(SHyperAIStudioAutoApprovalDialog& Dialog, bool bStayOn);
	/** The tooltip of "Stay on after the editor restarts". */
	static FText GetStayOnTooltip(const SHyperAIStudioAutoApprovalDialog& Dialog);
	/**
	 * Where a test's kept-on activation writes its record and a test restore reads it; empty (the default) and a test's
	 * activation writes nothing. The user's own always use the real path, which tests never touch.
	 */
	static void SetRecordPath(const FString& Path);
	/**
	 * A dialog under test opens with the defaults and remembers nothing, so it never reads or writes the user's choices.
	 * With this on it remembers its Turn On's choices in memory, as the user's settings would, until Reset.
	 */
	static void RememberChoices(bool bRemember);
	/** What the editor does as it closes: a kept-on activation stamps its record's close time and stays on disk. */
	static void CloseEditor();
	/**
	 * What startup does with the record at Path, as of the clock now (AdvanceClock moves it): it comes back as a test's
	 * activation covering only OperationIds, or is refused, deleted and logged. Returns the refusal (None when it came back).
	 */
	static HyperAIStudio::AutoApproval::KeepOn::ERefusal Restore(const FString& Path, const TSet<FString>& OperationIds);
	/** The notification the last restore or refusal showed (a test's is kept here, not shown); unset when there was none. */
	static TOptional<FText> GetLastNotice();
	/** The labels of its buttons; pressing one runs it as a click would. */
	static TArray<FText> GetLastNoticeButtons();
	static bool PressLastNoticeButton(int32 Index);
	/** The clock a record is read against: now in UTC, moved by AdvanceClock. */
	static FDateTime GetUtcNow();
	/** The spend settings' fingerprint as a record holds it. */
	static FString GetSpendFingerprint();
	static bool WasClosed(const SHyperAIStudioAutoApprovalDialog& Dialog);
	static int32 GetPendingShown(const SHyperAIStudioAutoApprovalDialog& Dialog);
	/** "Also approve the requests waiting now": its tick, and how many waiting requests it would approve with the categories ticked. */
	static bool IsAlsoApprovingWaiting(const SHyperAIStudioAutoApprovalDialog& Dialog);
	static void SetAlsoApproveWaiting(SHyperAIStudioAutoApprovalDialog& Dialog, bool bAlso);
	static int32 GetWaitingChosen(const SHyperAIStudioAutoApprovalDialog& Dialog);
	/** Whether the dialog offers Sandbox Mode's held saves (only while Sandbox Mode is on). */
	static bool OffersSandboxSaves(const SHyperAIStudioAutoApprovalDialog& Dialog);
	/** The note under that row: what happens to what Sandbox Mode holds, ticked or not. */
	static FText GetSandboxNote(const SHyperAIStudioAutoApprovalDialog& Dialog);
	/** What the native confirmation would say. */
	static FText GetConfirmation(const SHyperAIStudioAutoApprovalDialog& Dialog);
	/** The line on other MCP servers that bypass approvals; empty while none is found. */
	static FText GetBypassLine(const SHyperAIStudioAutoApprovalDialog& Dialog);
	/** One pass of the sandbox sweep the one-second ticker runs, without waiting for its two-second spacing. */
	static void PumpSandbox();
	/** How long a failed sandbox write waits before it is tried again while the held files stay as they were. */
	static double GetSandboxRetrySeconds();
	static int32 GetNativeConfirmations(const SHyperAIStudioAutoApprovalDialog& Dialog);
	static void AdvanceClock(double Seconds);
	/** A test's activation off, with the clock back: what module startup sees. The user's own activation is left alone. */
	static void Reset();
	/**
	 * Runs the gate's sweep and the expiry check as their tickers would, without ticking the core ticker (which also
	 * serves MCP and the remote page).
	 */
	static void PumpGate(int32 Ticks = 4);
	/** Runs the one-second expiry check once; false when its ticker was not registered. */
	static bool TickExpiry();
	static TOptional<EHyperAIStudioAutoApprovalEnd> GetLastEnd();
};
#endif
