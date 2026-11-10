// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "Features/IModularFeature.h"
#include "HAL/Platform.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

namespace TraceServices { class IAnalysisSession; }
class IGameplayProvider;
class FJsonObject;

/**
 * Lightweight summary of a single trace event, produced by an IRewindDebuggerToolsetEventDecoder.
 * Each row identifies the event and carries enough information to list it in a UI or MCP response
 * (time, name, decoder-specific fields). For the full payload, the toolset calls back into the
 * decoder via DecodeEvent with the same EventId.
 */
struct FRewindDebuggerToolsetEventSummary
{
	/** Opaque event identifier; defined by the decoder and round-tripped back to it for details. */
	uint64 EventId = 0;

	/** Human-readable event name suitable for substring filtering. */
	FString EventName;

	/** Recording time in seconds since the recording started. */
	double RecordingTime = 0.0;

	/** Optional decoder-specific fields (e.g. Status, IsStructured). Merged into the row JSON.
	 * Ownership transfers to the toolset: the toolset may mutate this object in place (e.g. to overlay
	 * identity fields) and the decoder must not retain or share the TSharedPtr across rows. Decoders
	 * should allocate a fresh FJsonObject per visited event. */
	TSharedPtr<FJsonObject> AdditionalFields;
};

/**
 * Extension point for typed trace producers (ChatCompletion, animation notifies, etc.) to expose
 * their events through the generic RewindDebuggerToolset MCP tools (ListObjectTraceEvents,
 * GetTraceEventDetails). Implementations register via the modular-feature system on module startup:
 *
 *     IModularFeatures::Get().RegisterModularFeature(
 *         IRewindDebuggerToolsetEventDecoder::GetModularFeatureName(), &MyDecoder);
 *
 * The toolset enumerates registered decoders, filters by SourceFilter when supplied, applies
 * IncludedNameFilter / ExcludedNameFilter on EventName, sorts by RecordingTime, and paginates.
 */
class IRewindDebuggerToolsetEventDecoder : public IModularFeature
{
public:
	virtual ~IRewindDebuggerToolsetEventDecoder() = default;

	static FName GetModularFeatureName() { return TEXT("RewindDebuggerToolsetEventDecoder"); }

	/** Source identifier surfaced through the MCP tool surface (e.g. "ChatCompletion"). */
	virtual FName GetSourceName() const = 0;

	/**
	 * Visit events attributed to ObjectId within [StartTime, EndTime] (inclusive, recording time).
	 * Attribution semantics are decoder-defined (direct ownership, outer-chain walk, controller
	 * possession, etc.). The decoder is responsible for acquiring any provider-level read locks
	 * before reading its trace data.
	 */
	virtual void EnumerateObjectEvents(
		const TraceServices::IAnalysisSession& Session,
		const IGameplayProvider& GameplayProvider,
		uint64 ObjectId,
		double StartTime,
		double EndTime,
		TFunctionRef<void(const FRewindDebuggerToolsetEventSummary& Row)> Visitor) const = 0;

	/**
	 * Return a fully decoded JSON object describing the given event, or nullptr if EventId is
	 * unknown to this decoder. The JSON shape is decoder-defined and surfaced verbatim through
	 * GetTraceEventDetails.
	 */
	virtual TSharedPtr<FJsonObject> DecodeEvent(
		const TraceServices::IAnalysisSession& Session,
		const IGameplayProvider& GameplayProvider,
		uint64 EventId) const = 0;
};
