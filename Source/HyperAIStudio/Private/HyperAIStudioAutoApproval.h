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
	/**
	 * Turns off an activation whose time ran out, or whose spend settings (the cap, the external services, the image
	 * models) changed since it started; the gate calls it before each approval decision.
	 */
	static void EndIfOver();
	/** The gate is about to approve OperationId for it; its outcome counts toward the failure guard. */
	static void BeginApproval(const FString& OperationId);
	static void NoteApproved(EHyperAIStudioApprovalCategory Category);
	static void Stop(EHyperAIStudioAutoApprovalEnd Why);
	static FSimpleMulticastDelegate& OnChanged();
	/** Opens the warning dialog, modal to Parent's window. It turns on only if the person confirms there. */
	static void OpenDialog(const TSharedPtr<const SWidget>& Parent);

	/** Edits, Scripts, Destructive, External effects. */
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
	 * activation never replaces the user's own.
	 */
	static bool Activate(const TArray<EHyperAIStudioApprovalCategory>& Categories,
		EHyperAIStudioAutoApprovalDuration Duration, FString& OutError);
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
	/** A fresh dialog with these categories and this duration, confirmed: the whole way a person turns it on. */
	static bool TurnOn(const TArray<EHyperAIStudioApprovalCategory>& Categories, const TSet<FString>& OperationIds,
		EHyperAIStudioAutoApprovalDuration Duration = EHyperAIStudioAutoApprovalDuration::OneHour);
	static bool WasClosed(const SHyperAIStudioAutoApprovalDialog& Dialog);
	static int32 GetPendingShown(const SHyperAIStudioAutoApprovalDialog& Dialog);
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
