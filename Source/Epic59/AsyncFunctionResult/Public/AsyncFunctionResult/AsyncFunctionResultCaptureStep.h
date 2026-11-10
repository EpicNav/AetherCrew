// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Features/IModularFeature.h"
#include "Templates/UniquePtr.h"
#include "UObject/NameTypes.h"

class UTransactor;

#define UE_API ASYNCFUNCTIONRESULT_API

namespace UE::AsyncFunctionResult
{
	/// Handle to an open capture step. Destroying it ends the step.
	class ICaptureStep
	{
	public:
		virtual ~ICaptureStep() = default;
	};

	/// Extension point that opens capture steps, implemented by the AsyncFunctionResultEditor
	/// module, where the undo buffer lives. At most one is expected; the first available is used.
	class ICaptureStepProvider : public IModularFeature
	{
	public:
		virtual ~ICaptureStepProvider() = default;

		static UE_API FName GetModularFeatureName();

		/// Opens a step that records edits into Buffer. Returns null when the step could not be
		/// opened; the provider logs why. Game thread only.
		virtual TUniquePtr<ICaptureStep> BeginCaptureStep(UTransactor* Buffer) = 0;
	};

	/// Opens a step recording into Buffer through the provider, loading AsyncFunctionResultEditor
	/// on first use. Null when Buffer is null or the provider refuses the step (it logs why).
	/// No provider after loading: checkf in Debug and Development builds, warning otherwise.
	/// Game thread only when Buffer is set.
	UE_API TUniquePtr<ICaptureStep> BeginCaptureStep(UTransactor* Buffer);
}

#undef UE_API
