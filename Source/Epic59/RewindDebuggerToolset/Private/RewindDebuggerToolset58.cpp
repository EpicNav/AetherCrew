// Games by Hyper 2026.
// UE 5.8: see RewindDebuggerToolset58.h.

#include "RewindDebuggerToolset58.h"

#include "RewindDebuggerToolset.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(RewindDebuggerToolset58)

FString URewindDebuggerToolset58::GetSessionInfo()
{
	return URewindDebuggerToolset::GetSessionInfo();
}

FString URewindDebuggerToolset58::ListObjects(const FString& ClassName)
{
	return URewindDebuggerToolset::ListObjects(ClassName);
}

FString URewindDebuggerToolset58::GetObjectEvents(int64 ObjectId, double StartTime, double EndTime)
{
	return URewindDebuggerToolset::GetObjectEvents(ObjectId, StartTime, EndTime);
}

FString URewindDebuggerToolset58::GetObjectProperties(int64 ObjectId, double Time)
{
	return URewindDebuggerToolset::GetObjectProperties(ObjectId, Time);
}

FString URewindDebuggerToolset58::GetSelectedObject()
{
	return URewindDebuggerToolset::GetSelectedObject();
}

FString URewindDebuggerToolset58::SelectObject(int64 ObjectId)
{
	return URewindDebuggerToolset::SelectObject(ObjectId);
}

FString URewindDebuggerToolset58::SetScrubTime(double Time)
{
	return URewindDebuggerToolset::SetScrubTime(Time);
}

FString URewindDebuggerToolset58::CenterView(double Time)
{
	return URewindDebuggerToolset::CenterView(Time);
}

FString URewindDebuggerToolset58::ListObjectTraceEvents(int64 ObjectId, double StartTime, double EndTime, const FString& SourceFilter, const FString& IncludedNameFilter, const FString& ExcludedNameFilter, int32 Limit, int32 Offset)
{
	return URewindDebuggerToolset::ListObjectTraceEvents(ObjectId, StartTime, EndTime, SourceFilter, IncludedNameFilter, ExcludedNameFilter, Limit, Offset);
}

FString URewindDebuggerToolset58::GetTraceEventDetails(const FString& Source, const FString& EventId)
{
	return URewindDebuggerToolset::GetTraceEventDetails(Source, EventId);
}
