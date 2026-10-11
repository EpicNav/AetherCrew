// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioApprovalCategory.h"

class SWidget;

enum class EHyperAIStudioAutoApprovalDuration : uint8
{
	FifteenMinutes,
	OneHour,
	FourHours,
	UntilEditorCloses
};

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
	/** Negative while it lasts until the editor closes. */
	double SecondsLeft = -1.0;
	int32 Approved = 0;
	int32 ExternalEffectsApproved = 0;
	/** Requests that were waiting when it began which the user chose in its dialog to approve too. */
	int32 ChosenWaiting = 0;
	/** Writes of Sandbox Mode's held saves; each is also one of Approved. */
	int32 SandboxWrites = 0;
};

/**
 * For a while the user chose, requests in the categories they chose are approved as their click would approve them.
 *
 * Only SHyperAIStudioAutoApprovalDialog turns it on, after its warning and a native confirmation: nothing reflected,
 * configured, exported or registered as a console command can. It lives in this process's memory only, so it is off at
 * every editor start and never saved. Anything may turn it off; what it approved that has not started by then waits
 * for a click again. Game thread only.
 */
class FHyperAIStudioAutoApproval
{
public:
	static constexpr int32 MaxApprovals = 50;
	/** External-effect requests, not paid calls: one image request may make several. The spend cap bounds the money. */
	static constexpr int32 MaxExternalEffects = 10;
	static constexpr int32 MaxConsecutiveFailures = 3;

	static bool IsActive();
	/** Names the current activation, so work it approved runs only while it lasts; 0 while off. */
	static uint64 GetActivationId();
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

	/** Edits, Scripts, Destructive, External effects, Sandbox saves. */
	static FText GetCategoryName(EHyperAIStudioApprovalCategory Category);
	static FText DescribeCategories(const TArray<EHyperAIStudioApprovalCategory>& Categories);
	static FText DescribeTimeLeft();
	/** "Auto-approval on: Edits (52 min left)", for the banners. */
	static FText DescribeBanner();
	static FText DescribeBannerTooltip();

private:
	friend class SHyperAIStudioAutoApprovalDialog;
	/**
	 * The dialog's confirm path, after the native confirmation. Replaces a running activation, except that a test's
	 * activation never replaces the user's own. ChosenWaiting names requests that were waiting as the dialog opened
	 * and that the user chose there to approve as well; it covers no other request held before it began.
	 */
	static bool Activate(const TArray<EHyperAIStudioApprovalCategory>& Categories,
		EHyperAIStudioAutoApprovalDuration Duration, const TSet<FString>& ChosenWaiting, FString& OutError);
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
	 * held before it keep waiting.
	 */
	static bool TurnOn(const TArray<EHyperAIStudioApprovalCategory>& Categories, const TSet<FString>& OperationIds,
		EHyperAIStudioAutoApprovalDuration Duration = EHyperAIStudioAutoApprovalDuration::OneHour, bool bAlsoApproveWaiting = false);
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
