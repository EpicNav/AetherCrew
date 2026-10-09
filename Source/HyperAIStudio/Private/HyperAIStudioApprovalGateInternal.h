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
}
