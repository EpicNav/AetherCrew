// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Editor.h"
#include "LiveCodingToolset59.h"
#include "AsyncFunctionResult/AsyncFunctionResultString.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/StrongObjectPtr.h"

TEST_CLASS(LiveCodingToolsetTest59, "AIAssistant.Toolsets.LiveCodingToolset") // UE 5.8: renamed ...59; 5.8 registers a test of the 5.9 name
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
		// UE 5.8: the subsystem registers ULiveCodingToolsetTools59 (5.8's registry needs a UToolsetDefinition).
		ASSERT_THAT(IsTrue(UToolsetRegistry::IsToolsetClassRegistered(ULiveCodingToolsetTools59::StaticClass())));
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
