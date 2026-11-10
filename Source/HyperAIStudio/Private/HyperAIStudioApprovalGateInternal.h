// Games by Hyper 2026.

#pragma once

#include "HyperAIStudioApprovalGate.h"

class SWidget;

namespace HyperAIStudio::Approval::Private
{
	/** The sequence the next held request gets; an auto-approval covers only requests from the one current at its start. */
	uint64 PeekNextSequence();

	/**
	 * What auto-approval files a request under. An escalated script is Scripts; otherwise the riskier of the declared
	 * and the sealed safety decides, since a pack may declare Edit for a plan whose variant deletes. Unset when the
	 * two are different risky classes, or the request is a read: only a click approves those.
	 */
	TOptional<EHyperAIStudioApprovalCategory> Categorize(
		const FHyperAIStudioApprovalSummary& Summary,
		TOptional<EHyperAIStudioDomainSafety> SealedSafety);

	/** Why the editor is not quiet enough for an approval nobody clicked; both empty when it is. */
	struct FBusy
	{
		/** In the log's English, as a clause: "an undo transaction is open (Move Actors)". */
		FString Log;
		/** The same for Activity. */
		FText Text;

		bool IsSet() const { return !Log.IsEmpty(); }
	};

	/**
	 * A click never lands mid-drag or mid-save, but a ticker can: an approved edit would nest its transaction in the
	 * user's open one, and a failed edit that undoes its transaction could undo the user's work. Such ticks wait.
	 */
	FBusy DescribeBusy();

	/**
	 * The game's viewport while Play In Editor runs: a game holds the mouse there for the whole session, which is no
	 * drag. Null otherwise, and in Simulate In Editor or a session ejected to it, where that viewport is the editor's own
	 * and holding the mouse there is the user's drag.
	 */
	const SWidget* PlayingGameViewport();

	/** The captor's type, for the log, when its mouse capture holds approvals back; empty for PlayingGame's capture. */
	FString DescribeCaptor(const SWidget& Captor, const SWidget* PlayingGame);

	/**
	 * Right after an auto-approval starts: marks the requests it covers that were already waiting (those the user chose
	 * in its dialog, and those the activation it replaced had approved) to be approved on the next quiet tick. Returns
	 * how many.
	 */
	int32 ScheduleCovered();

	/**
	 * When an auto-approval ends: plans it approved that still wait in the edit queue behind other edits are taken back
	 * before their grant is issued, and wait for a click again as they did before it. Returns how many, and adds their
	 * operation ids to OutReturned when given (an activation that replaces the one ending approves them again).
	 */
	int32 ReturnUnstarted(uint64 EndedActivation, TArray<FString>* OutReturned = nullptr);

#if WITH_DEV_AUTOMATION_TESTS
	/** One pass of the sweep its ticker runs, without ticking the core ticker. */
	void SweepForTests();
#endif
}
