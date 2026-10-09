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
	/** A new activation took its place. */
	Replaced,
	EditorClosing
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
 * every editor start and never saved. Anything may turn it off. Game thread only.
 */
class FHyperAIStudioAutoApproval
{
public:
	static constexpr int32 MaxApprovals = 50;
	static constexpr int32 MaxExternalEffects = 10;
	static constexpr int32 MaxConsecutiveFailures = 3;

	static bool IsActive();
	static FHyperAIStudioAutoApprovalStatus GetStatus();
	/**
	 * Would a request of this category, held at Sequence, be approved now? Reads only: an activation whose time ran out
	 * covers nothing, and EndIfExpired turns it off.
	 */
	static bool Covers(EHyperAIStudioApprovalCategory Category, uint64 Sequence, const FString& OperationId);
	/** Turns off an activation whose time ran out; the gate calls it before each approval decision. */
	static void EndIfExpired();
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
	/** The dialog's confirm path, after the native confirmation. Replaces a running activation. */
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
class SHyperAIStudioAutoApprovalDialog;

/** The tests' way in. Not exported, so only this module's tests can link it. */
struct FHyperAIStudioAutoApprovalTestAccess
{
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
	/** A fresh dialog with these categories and this duration, confirmed: the whole way a person turns it on. */
	static bool TurnOn(const TArray<EHyperAIStudioApprovalCategory>& Categories, const TSet<FString>& OperationIds,
		EHyperAIStudioAutoApprovalDuration Duration = EHyperAIStudioAutoApprovalDuration::OneHour);
	static bool WasClosed(const SHyperAIStudioAutoApprovalDialog& Dialog);
	static int32 GetPendingShown(const SHyperAIStudioAutoApprovalDialog& Dialog);
	static int32 GetNativeConfirmations(const SHyperAIStudioAutoApprovalDialog& Dialog);
	static void AdvanceClock(double Seconds);
	/** Off, with the clock back: what module startup sees. */
	static void Reset();
	/** Ticks the core ticker the gate's sweep runs on. */
	static void PumpGate(int32 Ticks = 4);
	static TOptional<EHyperAIStudioAutoApprovalEnd> GetLastEnd();
};
#endif
