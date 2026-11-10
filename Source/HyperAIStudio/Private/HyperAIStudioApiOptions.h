// Games by Hyper 2026.
#pragma once
#include "CoreMinimal.h"

namespace HyperAIStudio::ApiOptions
{
    /** 0 = no HyperAI cap. Claude uses its model maximum; other providers select their defaults. */
    int32 MaxOutputTokens(const FString& ProjectRoot);
    bool SetMaxOutputTokens(const FString& ProjectRoot, int32 Tokens);
}
