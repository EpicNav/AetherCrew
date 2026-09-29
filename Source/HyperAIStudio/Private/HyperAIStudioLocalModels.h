// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * Models Ollama and LM Studio serve on this computer, found through their local HTTP APIs, for the Model menu of the
 * agents that can run them (UHyperAIStudioSettings::CanRunLocalModels). Game thread only.
 */
class FHyperAIStudioLocalModels
{
public:
	/** ollama/<name> and lmstudio/<id>, as last found. Asks both servers again when that was more than 30 s ago. */
	static TArray<FString> Get();
	/** Asks both servers now. OnDone runs on the game thread once both answered or gave up. */
	static void Refresh(TFunction<void()> OnDone = nullptr);
#if WITH_DEV_AUTOMATION_TESTS
	/** Asks other servers instead, such as a test's stand-ins; empty strings restore Ollama's and LM Studio's ports. */
	static void SetServersForTests(const FString& Ollama, const FString& LMStudio);
#endif
};
