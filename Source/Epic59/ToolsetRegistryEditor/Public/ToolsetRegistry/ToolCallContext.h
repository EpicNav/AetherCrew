// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResultContext.h"

namespace UE::ToolsetRegistry
{
	/// Producer side of a tool call's cancellation flag, one per unit of work the user can
	/// stop. Shared so that every token derived from it keeps the flag alive; trip it with
	/// Source->Cancel().
	using FToolCancellationSource = UE::AsyncFunctionResult::FCancellationSource;

	/// Read-only handle a running tool polls for cancellation. Converts from a source.
	using FToolCancellationToken = UE::AsyncFunctionResult::FCancellationToken;

	/// What one tool call carries that belongs to the call rather than to the tool.
	///
	/// Supplied by whoever dispatches the call and threaded down to the tool, so a host can
	/// identify its calls, stop them, and capture their edits without any of that being
	/// process-wide state.
	///
	/// Every field is optional; a default-constructed context describes an unattributed call.
	using FToolCallContext = UE::AsyncFunctionResult::FAsyncFunctionResultContext;
}
