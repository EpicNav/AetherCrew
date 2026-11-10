// Games by Hyper 2026.
// UE 5.8: see InputToolset58.h.

#include "InputToolset58.h"

#include "AsyncFunctionResult/AsyncFunctionResultUtils.h"
#include "AsyncFunctionResult/AsyncFunctionResultVoid.h"
#include "Kismet/KismetSystemLibrary.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(InputToolset58)

namespace UE::InputToolset58::Private
{
	// 5.9's registry reports a missing dispatch target in these words.
	const TCHAR* NoTargetError =
		TEXT("Toolset InputToolset uses instance dispatch but its dispatch target could not be resolved. "
			 "The instance does not exist yet, or has been destroyed. Start a PIE or game session with a local player first.");
}

UInputToolset* UInputToolset58::ResolveTarget()
{
	return Cast<UInputToolset>(GetDefault<UInputToolset>()->ResolveToolDispatchTarget());
}

UAsyncFunctionResultVoid* UInputToolset58::ApplyInput(const TArray<FInputToolsetEntry>& Entries, float DurationSeconds)
{
	UInputToolset* Target = ResolveTarget();
	if (Target == nullptr)
	{
		return UE::AsyncFunctionResult::MakeErrorResult<UAsyncFunctionResultVoid>(UE::InputToolset58::Private::NoTargetError);
	}
	return Target->ApplyInput(Entries, DurationSeconds);
}

TArray<bool> UInputToolset58::IsInputApplied(const TArray<FInputToolsetEntry>& Entries)
{
	UInputToolset* Target = ResolveTarget();
	if (Target == nullptr)
	{
		UKismetSystemLibrary::RaiseScriptError(UE::InputToolset58::Private::NoTargetError);
		return {};
	}
	return Target->IsInputApplied(Entries);
}
