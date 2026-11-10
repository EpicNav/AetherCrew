// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "EditorSubsystem.h"
#include "ToolsetCore/ToolsetProvider.h"
#include "ToolsetRegistry/ToolsetPermissionProvider.h"

#include "RewindDebuggerToolset.generated.h"

class FSubsystemCollectionBase;

/**
 * Rewind Debugger recording inspection toolset.
 *
 * Exposes recorded session data — objects, events, property snapshots, and decoded trace events
 * from typed trace producers — to MCP clients, plus navigation entry points to drive the editor
 * UI to a specific recorded object and moment in time. Requires an active Rewind Debugger
 * analysis session (either a live PIE recording or a loaded trace file). Call GetSessionInfo
 * first to confirm a session is active before querying object data.
 *
 * Typical workflow:
 *   1. GetSessionInfo() to confirm a session is active and get the time range.
 *   2. GetSelectedObject() to check whether the user already has an object focused in the
 *      outliner. If not, call ListObjects() to find objects by class name and get their ObjectIds.
 *   3. GetObjectEvents() / ListObjectTraceEvents() to see what events fired on an object
 *      in a time range. GetObjectEvents reads the generic gameplay-event channel;
 *      ListObjectTraceEvents aggregates typed trace producers that register an
 *      IRewindDebuggerToolsetEventDecoder (e.g. ChatCompletion).
 *   4. GetObjectProperties() to inspect property state at a specific moment, or
 *      GetTraceEventDetails() to decode a specific typed event in full.
 *   5. SelectObject() / SetScrubTime() to surface a particular object and moment
 *      in the Rewind Debugger UI when reporting findings back to the user.
 *
 * Note: ObjectId parameters are typed `int64` because UFUNCTION reflection has no `uint64` binding,
 * but the underlying gameplay-provider object-id space is `uint64`. Values are bit-cast across the
 * boundary, so ids above 2^63 will round-trip as negative int64 through the MCP layer. In practice
 * engine-allocated ids stay well below that range.
 */
UCLASS(BlueprintType, MinimalAPI)
class URewindDebuggerToolset : public UEditorSubsystem, public IToolsetProvider
{
	GENERATED_BODY()

public:

	REWINDDEBUGGERTOOLSET_API virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	REWINDDEBUGGERTOOLSET_API virtual void Deinitialize() override;

	/** Register or unregister the toolset at runtime. */
	void SetToolsetEnabled(bool bEnabled);

	/** Returns the current Rewind Debugger session state.
	 * Call this first to confirm a session is active before querying object data.
	 * Always returns all fields; IsActive is false when no session is loaded.
	 * @return JSON object: {"IsActive", "ScrubTime", "RecordingDuration", "TraceRangeStart", "TraceRangeEnd"} */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "RewindDebuggerToolset")
	static REWINDDEBUGGERTOOLSET_API FString GetSessionInfo();

	/** Lists all objects recorded in the current session, optionally filtered by class name.
	 * Each entry includes an ObjectId for use with GetObjectEvents and GetObjectProperties.
	 * Returns {"Objects": []} when no session is active or no objects match the filter.
	 * @param ClassName  Case-insensitive substring filter on class name. Empty returns all objects.
	 * @return JSON object: {"Objects": [{"ObjectId", "Name", "ClassName", "LifetimeStart", "LifetimeEnd"}]} */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "RewindDebuggerToolset")
	static REWINDDEBUGGERTOOLSET_API FString ListObjects(const FString& ClassName = TEXT(""));

	/** Returns all events that fired on a recorded object within a time range.
	 * Events represent discrete occurrences such as Linguine prompt executions.
	 * Returns {"Events": []} when no session is active or no events exist in the range.
	 * @param ObjectId   Object identifier returned by ListObjects.
	 * @param StartTime  Start of the time range in seconds (trace time).
	 * @param EndTime    End of the time range in seconds (trace time).
	 * @return JSON object: {"Events": [{"Time", "Name"}]} */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "RewindDebuggerToolset")
	static REWINDDEBUGGERTOOLSET_API FString GetObjectEvents(int64 ObjectId, double StartTime, double EndTime);

	/** Returns the recorded property state of an object at or before a given time.
	 * Uses closest-before semantics: returns the most recent snapshot at or before Time.
	 * Returns {} when no session is active or no snapshot exists before Time.
	 * @param ObjectId  Object identifier returned by ListObjects.
	 * @param Time      Point in time to query in seconds (trace time).
	 * @return JSON object: {"ProfileTime", "ElapsedTime", "Properties": [{"Name", "Value", "ParentId"}]} */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "RewindDebuggerToolset")
	static REWINDDEBUGGERTOOLSET_API FString GetObjectProperties(int64 ObjectId, double Time);

	/** Returns the object currently selected in the Rewind Debugger outliner, if any.
	 * @return JSON object: {"ObjectId", "Name", "ClassName"} when an object is selected and
	 *         resolved successfully, or {"Error": "..."} when no session is active or the
	 *         object is not recorded. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "RewindDebuggerToolset")
	static REWINDDEBUGGERTOOLSET_API FString GetSelectedObject();

	/** Selects a recorded object in the Rewind Debugger panel and opens the details panel so
	 * its state and timelines are visible to the user.
	 * Use after ListObjects to surface a particular object when reporting findings.
	 * @param ObjectId  Object identifier returned by ListObjects.
	 * @return JSON object: {"ObjectId", "Name", "ClassName"} on success,
	 *         or {"Error": "..."} when no session is active or the object is not recorded. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "RewindDebuggerToolset")
	static REWINDDEBUGGERTOOLSET_API FString SelectObject(int64 ObjectId);

	/** Moves the Rewind Debugger scrub cursor to a specific recording time. The engine clamps
	 * to the recording's range. Disables auto-scroll-with-recording so the timeline stays put.
	 * Does not re-centre the visible view range; pair with CenterView to do that.
	 * @param Time  Recording time in seconds. Must be finite (NaN/Inf are rejected).
	 * @return JSON object: {"ScrubTime"} on success, or {"Error": "..."} on invalid input or no session. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "RewindDebuggerToolset")
	static REWINDDEBUGGERTOOLSET_API FString SetScrubTime(double Time);

	/** Re-centres the Rewind Debugger visible view range on Time, preserving the current view width.
	 * Independent of the scrub cursor: pair with SetScrubTime when you want to both move the cursor
	 * and bring it into the centre of the visible timeline.
	 * @param Time  Recording time in seconds to centre the view on. Must be finite.
	 * @return JSON object: {"ViewRangeStart","ViewRangeEnd"} on success, or {"Error": "..."} on invalid input or no session. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "RewindDebuggerToolset")
	static REWINDDEBUGGERTOOLSET_API FString CenterView(double Time);

	/** Lists trace events attributed to ObjectId within [StartTime, EndTime], aggregated across
	 * every registered IRewindDebuggerToolsetEventDecoder. Returns lightweight rows; call
	 * GetTraceEventDetails to decode a specific event in full.
	 * @param ObjectId            Object identifier returned by ListObjects.
	 * @param StartTime           Inclusive lower bound on recording time (seconds since recording start).
	 * @param EndTime             Inclusive upper bound on recording time.
	 * @param SourceFilter        Only events whose source name (case-insensitive) equals this are
	 *                            returned. Empty includes every registered source.
	 * @param IncludedNameFilter  Case-insensitive substring match on EventName. Empty matches everything.
	 * @param ExcludedNameFilter  Case-insensitive substring exclusion on EventName. Empty excludes nothing.
	 * @param Limit               Maximum number of rows after sorting. <= 0 means no cap. Recommended default: 100.
	 * @param Offset              Number of rows to skip before returning Limit rows. Negative values treated as 0.
	 * @return JSON array sorted by RecordingTime ascending. Each entry has "Source" (string),
	 *         "EventId" (string-encoded uint64), "EventName" (string), "RecordingTime" (double),
	 *         plus any decoder-specific additional fields merged at the top level. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "RewindDebuggerToolset")
	static REWINDDEBUGGERTOOLSET_API FString ListObjectTraceEvents(
		int64 ObjectId,
		double StartTime,
		double EndTime,
		const FString& SourceFilter,
		const FString& IncludedNameFilter,
		const FString& ExcludedNameFilter,
		int32 Limit,
		int32 Offset);

	/** Returns the full decoded JSON for a single trace event from a named source.
	 * @param Source   Source identifier from ListObjectTraceEvents (e.g. "ChatCompletion").
	 * @param EventId  Event identifier from ListObjectTraceEvents (string-encoded uint64).
	 * @return The decoder-defined JSON object verbatim, or {"Error": "..."} on lookup failure. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "RewindDebuggerToolset")
	static REWINDDEBUGGERTOOLSET_API FString GetTraceEventDetails(
		const FString& Source,
		const FString& EventId);

	//~ Begin IToolsetProvider Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End IToolsetProvider Interface

private:

	bool bToolsetRegistered = false;
};
