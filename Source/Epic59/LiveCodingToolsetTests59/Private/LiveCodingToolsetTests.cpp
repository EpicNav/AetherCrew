// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Editor.h"
#include "LiveCodingToolset59.h"
#include "AsyncFunctionResult/AsyncFunctionResultString.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/StrongObjectPtr.h"

TEST_CLASS(LiveCodingToolsetTest, "AIAssistant.Toolsets.LiveCodingToolset")
{
	TEST_METHOD(Subsystem_IsAvailable)
	{
		ULiveCodingToolset59* Subsystem = GEditor->GetEditorSubsystem<ULiveCodingToolset59>();
		ASSERT_THAT(IsNotNull(Subsystem));
	}

	TEST_METHOD(Subsystem_RegistersToolsetByDefault)
	{
		// The subsystem registers itself in Initialize when the
		// LiveCodingToolset.Enable CVar is true (the default).
		ASSERT_THAT(IsTrue(UToolsetRegistry::IsToolsetClassRegistered(ULiveCodingToolset59::StaticClass())));
	}

	TEST_METHOD(CompileLiveCoding_EventuallyResolvesWithResult)
	{
		UAsyncFunctionResultString* Result = ULiveCodingToolset59::CompileLiveCoding();
		ASSERT_THAT(IsNotNull(Result));

		TStrongObjectPtr<UAsyncFunctionResultString> ResultRef(Result);
		TestCommandBuilder
			.StartWhen([ResultRef]() { return ResultRef->bIsComplete; }, FTimespan::FromSeconds(60))
			.Then([this, ResultRef]()
			{
				ASSERT_THAT(IsFalse(ResultRef->Error.IsEmpty() && ResultRef->Value.IsEmpty()));
			});
	}

	TEST_METHOD(TryTimeout_ResolvesErrorWhenStillPending)
	{
		TStrongObjectPtr<UAsyncFunctionResultString> ResultRef(NewObject<UAsyncFunctionResultString>());

		const bool bTimedOut = ULiveCodingToolset59::TryTimeout(*ResultRef);

		ASSERT_THAT(IsTrue(bTimedOut));
		ASSERT_THAT(IsTrue(ResultRef->bIsComplete));
		ASSERT_THAT(IsFalse(ResultRef->Error.IsEmpty()));
	}

	TEST_METHOD(TryTimeout_LeavesAlreadyCompletedResultUntouched)
	{
		TStrongObjectPtr<UAsyncFunctionResultString> ResultRef(NewObject<UAsyncFunctionResultString>());
		ResultRef->SetValue(TEXT("already done"));

		const bool bTimedOut = ULiveCodingToolset59::TryTimeout(*ResultRef);

		ASSERT_THAT(IsFalse(bTimedOut));
		ASSERT_THAT(IsTrue(ResultRef->Error.IsEmpty()));
		ASSERT_THAT(IsTrue(ResultRef->Value == TEXT("already done")));
	}
};
