// Copyright Epic Games, Inc. All Rights Reserved.

// CQTest
#include "CQTest.h"

// RewindDebuggerToolset
#include "RewindDebuggerToolset.h"

TEST_CLASS(RewindDebuggerToolsetTest, "AIAssistant.Toolsets.RewindDebuggerToolset")
{
	// -- GetSessionInfo --

	TEST_METHOD(GetSessionInfo_ContainsAllExpectedFields)
	{
		const FString Result = URewindDebuggerToolset::GetSessionInfo();
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"IsActive\""))));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"ScrubTime\""))));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"RecordingDuration\""))));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"TraceRangeStart\""))));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"TraceRangeEnd\""))));
	}

	TEST_METHOD(GetSessionInfo_NoSession_IsActiveFalse)
	{
		const FString Result = URewindDebuggerToolset::GetSessionInfo();
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"IsActive\": false"))));
	}

	// -- ListObjects --

	TEST_METHOD(ListObjects_NoSession_ContainsObjectsField)
	{
		const FString Result = URewindDebuggerToolset::ListObjects();
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Objects\""))));
	}

	TEST_METHOD(ListObjects_NoSession_ReturnsEmptyArray)
	{
		const FString Result = URewindDebuggerToolset::ListObjects();
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Objects\": []"))));
	}

	TEST_METHOD(ListObjects_WithClassFilter_NoSession_ReturnsEmptyArray)
	{
		const FString Result = URewindDebuggerToolset::ListObjects(TEXT("SomeClass"));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Objects\": []"))));
	}

	// -- GetObjectEvents --

	TEST_METHOD(GetObjectEvents_NoSession_ContainsEventsField)
	{
		const FString Result = URewindDebuggerToolset::GetObjectEvents(0, 0.0, 1.0);
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Events\""))));
	}

	TEST_METHOD(GetObjectEvents_NoSession_ReturnsEmptyArray)
	{
		const FString Result = URewindDebuggerToolset::GetObjectEvents(0, 0.0, 1.0);
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Events\": []"))));
	}

	TEST_METHOD(GetObjectEvents_InvalidObjectId_ReturnsEmptyArray)
	{
		const FString Result = URewindDebuggerToolset::GetObjectEvents(-1, 0.0, 100.0);
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Events\": []"))));
	}

	// -- GetObjectProperties --

	TEST_METHOD(GetObjectProperties_NoSession_ReturnsEmptyObject)
	{
		const FString Result = URewindDebuggerToolset::GetObjectProperties(0, 0.0);
		ASSERT_THAT(IsTrue(Result == TEXT("{}")));
	}

	TEST_METHOD(GetObjectProperties_InvalidObjectId_ReturnsEmptyObject)
	{
		const FString Result = URewindDebuggerToolset::GetObjectProperties(-1, 0.0);
		ASSERT_THAT(IsTrue(Result == TEXT("{}")));
	}

	// -- GetSelectedObject --

	TEST_METHOD(GetSelectedObject_NoSession_ReturnsError)
	{
		const FString Result = URewindDebuggerToolset::GetSelectedObject();
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Error\""))));
	}

	// -- SelectObject --

	TEST_METHOD(SelectObject_NoSession_ReturnsError)
	{
		const FString Result = URewindDebuggerToolset::SelectObject(0);
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Error\""))));
	}

	// -- SetScrubTime --

	TEST_METHOD(SetScrubTime_NoSession_ReturnsError)
	{
		const FString Result = URewindDebuggerToolset::SetScrubTime(0.0);
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Error\""))));
	}

	TEST_METHOD(SetScrubTime_NaN_ReturnsError)
	{
		const FString Result = URewindDebuggerToolset::SetScrubTime(std::numeric_limits<double>::quiet_NaN());
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Error\""))));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("finite"))));
	}

	TEST_METHOD(SetScrubTime_PositiveInf_ReturnsError)
	{
		const FString Result = URewindDebuggerToolset::SetScrubTime(std::numeric_limits<double>::infinity());
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Error\""))));
	}

	// -- CenterView --

	TEST_METHOD(CenterView_NoSession_ReturnsError)
	{
		const FString Result = URewindDebuggerToolset::CenterView(0.0);
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Error\""))));
	}

	TEST_METHOD(CenterView_NaN_ReturnsError)
	{
		const FString Result = URewindDebuggerToolset::CenterView(std::numeric_limits<double>::quiet_NaN());
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Error\""))));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("finite"))));
	}

	// -- ListObjectTraceEvents --

	TEST_METHOD(ListObjectTraceEvents_NoSession_ReturnsEmptyArray)
	{
		const FString Result = URewindDebuggerToolset::ListObjectTraceEvents(0, 0.0, 1.0, TEXT(""), TEXT(""), TEXT(""), 100, 0);
		ASSERT_THAT(IsTrue(Result == TEXT("[]")));
	}

	// -- GetTraceEventDetails --

	TEST_METHOD(GetTraceEventDetails_EmptySource_ReturnsError)
	{
		const FString Result = URewindDebuggerToolset::GetTraceEventDetails(TEXT(""), TEXT("1"));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Error\""))));
	}

	TEST_METHOD(GetTraceEventDetails_EmptyEventId_ReturnsError)
	{
		const FString Result = URewindDebuggerToolset::GetTraceEventDetails(TEXT("ChatCompletion"), TEXT(""));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Error\""))));
	}

	TEST_METHOD(GetTraceEventDetails_MalformedEventId_ReturnsError)
	{
		const FString Result = URewindDebuggerToolset::GetTraceEventDetails(TEXT("ChatCompletion"), TEXT("not-a-number"));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Error\""))));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("not a non-negative decimal integer"))));
	}

	TEST_METHOD(GetTraceEventDetails_NegativeEventId_ReturnsError)
	{
		const FString Result = URewindDebuggerToolset::GetTraceEventDetails(TEXT("ChatCompletion"), TEXT("-1"));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Error\""))));
	}

	TEST_METHOD(GetTraceEventDetails_NoSession_ReturnsError)
	{
		const FString Result = URewindDebuggerToolset::GetTraceEventDetails(TEXT("ChatCompletion"), TEXT("42"));
		ASSERT_THAT(IsTrue(Result.Contains(TEXT("\"Error\""))));
	}
};
