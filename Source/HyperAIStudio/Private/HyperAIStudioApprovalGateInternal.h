// Games by Hyper 2026.

#pragma once

#include "HyperAIStudioApprovalGate.h"

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

	/**
	 * When an auto-approval ends: plans it approved that still wait in the edit queue behind other edits are taken back
	 * before their grant is issued, and wait for a click again as they did before it. Returns how many.
	 */
	int32 ReturnUnstarted(uint64 EndedActivation);

#if WITH_DEV_AUTOMATION_TESTS
	/** One pass of the sweep its ticker runs, without ticking the core ticker. */
	void SweepForTests();
#endif
}
