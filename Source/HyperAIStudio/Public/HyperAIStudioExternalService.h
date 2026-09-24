// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

struct FHyperAIStudioExternalServiceSettings;

/** What the last health check found. */
enum class EHyperAIStudioServiceHealth : uint8
{
	/** Not checked yet, or checking now. */
	Unknown,
	Reachable,
	Unreachable,
	/** Reachable, but the environment variable holding its key is empty. */
	KeyMissing
};

struct FHyperAIStudioServiceStatus
{
	FString Id;
	EHyperAIStudioServiceHealth Health = EHyperAIStudioServiceHealth::Unknown;
	/** Plain enough to act on: what was tried, and what to do about it. */
	FString Diagnostic;
	FDateTime CheckedUtc = FDateTime::MinValue();
	int32 RoundTripMs = 0;
};

/**
 * Model servers and hosted APIs that packs call for work the editor cannot do itself: speech, images.
 *
 * Three things separate these from every other tool in the plugin. They are outside the editor, so they can
 * simply be absent, and a pack has to say so rather than fail mid-plan. They may need a key, which must never
 * reach a config file that gets committed, so keys live in environment variables and this never returns one.
 * And they can cost money, so a call is reserved against a session cap before it is made.
 *
 * Game thread only. Responses arrive on the game thread; nothing here blocks.
 */
class HYPERAISTUDIO_API FHyperAIStudioExternalServices
{
public:
	/** Health older than this is rechecked on the next query. */
	static constexpr int32 HealthTimeToLiveSeconds = 30;
	static constexpr int32 HealthTimeoutSeconds = 5;
	static constexpr int32 MaxRequestBodyBytes = 8 * 1024 * 1024;
	static constexpr int32 MaxResponseBodyBytes = 64 * 1024 * 1024;

	/** The project's services, and Google's Gemini API for the Nano Banana models unless the project names its own. */
	static TArray<FHyperAIStudioExternalServiceSettings> GetServices();
	static bool Find(const FString& Id, FHyperAIStudioExternalServiceSettings& OutService);

	/** True when the service needs no key, or its environment variable holds one. Never returns the key. */
	static bool HasApiKey(const FHyperAIStudioExternalServiceSettings& Service);

	/** The cached status, starting a check when it is missing or stale. Never blocks. */
	static FHyperAIStudioServiceStatus QueryHealth(const FString& Id);
	/** Rechecks now, whatever the cache holds. */
	static void RequestHealthCheck(const FString& Id);
	static FSimpleMulticastDelegate& OnHealthChanged();

	/**
	 * Books Calls calls against this session's cap. Fails, without spending, when they would pass it.
	 * A service that costs nothing always succeeds and books nothing. CostPerCall, when not negative, is what
	 * each call costs instead of the service's own price, for a model priced per image.
	 */
	static bool Reserve(const FString& Id, int32 Calls, double& OutEstimatedCost, FString& OutError, double CostPerCall = -1.0);
	/** Records calls that actually went out, so the cap follows real spend rather than plans. */
	static void RecordSpend(const FString& Id, int32 Calls, double CostPerCall = -1.0);
	static double GetSessionSpend();
	static void ResetSessionSpend();

	/** Body may be binary; Headers are the response's. Runs on the game thread, once. */
	using FOnResponse = TFunction<void(bool bOk, const TArray<uint8>& Body, const TMap<FString, FString>& Headers, const FString& Error)>;

	/**
	 * One bounded POST with the service's key attached, if it has one. False means nothing was sent and
	 * OnResponse will not run. Records the spend when the request goes out, at CostPerCall when it is not negative.
	 */
	static bool PostJson(const FString& Id, const FString& Path, const FString& JsonBody, FOnResponse OnResponse, FString& OutError,
		double CostPerCall = -1.0);

	static const TCHAR* LexToString(EHyperAIStudioServiceHealth Health);
	static void Shutdown();
};
