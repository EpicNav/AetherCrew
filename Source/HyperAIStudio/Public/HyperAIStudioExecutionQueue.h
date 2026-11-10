// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioTrustedExecution.h"

/**
 * Agent edits take turns on the project's one execution lane. An edit that arrives while another runs waits
 * here, first in first out, instead of failing with project_execution_busy, so parallel agents can share one
 * editor. A plan that waited past its preparation seal is prepared again from the same inputs when its turn
 * comes; staging happens only then, since a staged artifact lives seconds. Game thread only.
 */
class HYPERAISTUDIO_API FHyperAIStudioExecutionQueue
{
public:
	static constexpr int32 MaxQueued = 32;
	static constexpr double MaxWaitSeconds = 600.0;

	/** Runs the edit now, or queues it. OutStatus is "submitted" or "queued"; on false it is the failure code. */
	static bool Submit(
		const FHyperAIStudioTrustedPreparedArtifact& Prepared,
		const FString& OperationId,
		FString& OutStatus,
		FString& OutError);

	/**
	 * True when the queue knows the operation: OutPosition is 1 for the next edit to run while it waits, or 0
	 * with OutFailure set when it could not start after waiting.
	 */
	static bool Query(const FString& OperationId, int32& OutPosition, FString& OutFailure, FString& OutDiagnostic);
	static int32 Num();
	/** True while an edit runs on the lane. */
	static bool IsRunning();
	/** Drops every waiting edit without running it. */
	static void Clear();
};
