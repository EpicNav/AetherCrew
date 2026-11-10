// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

namespace UE::Tasks {

class FCancellationToken;

}

class UTransactor;

namespace UE::AsyncFunctionResult
{
	/// Producer side of a call's cancellation flag, one per unit of work the user can stop.
	/// Shared so that every token derived from it keeps the flag alive; trip it with
	/// Source->Cancel().
	using FCancellationSource = TSharedRef<UE::Tasks::FCancellationToken>;

	/// Read-only handle a running call polls for cancellation. Converts from a source.
	using FCancellationToken = TSharedPtr<const UE::Tasks::FCancellationToken>;

	/// What one asynchronous call carries that belongs to the call rather than to the function.
	///
	/// Supplied by whoever dispatches the call and threaded down to the callee, so a host can
	/// identify its calls, stop them, and capture their edits without any of that being
	/// process-wide state.
	///
	/// Every field is optional; a default-constructed context describes an unattributed call.
	struct FAsyncFunctionResultContext
	{
		/// Identifies the call to its dispatcher. Empty when the dispatcher does not track calls.
		FString ToolCallId;

		/// Polled by the callee to stop early. Null when the call cannot be stopped.
		FCancellationToken CancellationToken;

		/// Undo buffer this call's edits are captured into: the editor's undo buffer is redirected
		/// to it during each capture step. Null when the host is not capturing.
		TWeakObjectPtr<UTransactor> CaptureBuffer;
	};
}
