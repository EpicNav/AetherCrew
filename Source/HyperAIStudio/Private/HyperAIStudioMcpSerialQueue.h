// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * Process-local endpoint queue for complete MCP sessions. A task owns the endpoint until it
 * invokes its idempotent completion callback. This prevents status probes and deep inventory from
 * overlapping Unreal game-thread tool calls.
 *
 * AmbiguousTransport is for work that changes the editor: when such a request may have reached the
 * server but no answer came back, more work could act twice or on top of it, so the endpoint stays
 * locked until a confirmed server restart. Read-only work never reports it. The readiness probe and
 * the capability inventory send only initialize, notifications/initialized, tools/list and the
 * list_toolsets/describe_toolset meta-tools, which change nothing, so asking again is always safe.
 * A stale exchange cannot feed a later one either: each opens its own session with its own
 * initialize, its later requests carry that session's Mcp-Session-Id and check their JSON-RPC ids,
 * and UE's HTTP client hands each response only to the request that sent it. A late initialize
 * leaves at most an unused session on the server. Locking on them once left a restarted editor's
 * chats waiting for good: the first probe timed out before the editor ticked, and only a server
 * restart could clear the lock.
 */
class FHyperAIStudioMcpSerialQueue
{
public:
	enum class ECompletionDisposition : uint8
	{
		SafeToRelease,
		AmbiguousTransport
	};

	using FComplete = TFunction<void(ECompletionDisposition)>;
	using FWork = TFunction<void(FComplete)>;
	using FRejected = TFunction<void(const FString& Reason)>;

	/**
	 * Returns a stable HTTP(S) origin identity or an empty string for malformed input.
	 * Path, query and fragment aliases share the same queue because they reach the same MCP
	 * listener. Loopback aliases localhost, 127.0.0.1 and [::1] also share one key.
	 */
	static FString CanonicalizeEndpointKey(const FString& Endpoint);

	/**
	 * Returns false and invokes Rejected when the bounded queue cannot accept the work.
	 * Rejected is also invoked for work that was waiting when the active request becomes
	 * ambiguous, so no caller remains parked indefinitely.
	 */
	static bool Enqueue(const FString& Endpoint, FWork Work, FRejected Rejected = FRejected());

	/**
	 * Clears endpoint state only after the caller has positively established that the old
	 * server/session epoch ended (for example, Epic's in-process server is confirmed stopped).
	 * This is deliberately not part of cache invalidation or an elapsed-time policy.
	 */
	static bool ResetAfterConfirmedServerStop(const FString& Endpoint);

	static bool IsBusy(const FString& Endpoint);
	static bool IsAmbiguouslyLocked(const FString& Endpoint);
};
