// Games by Hyper 2026.

#include "HyperAIStudioLocalModels.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/** How long a look stands before the menu asks again. */
	constexpr double LookSeconds = 30.0;
	/** A server that is up answers at once; one that is not refuses at once. */
	constexpr float AskSeconds = 2.0f;

	struct FState
	{
		TArray<FString> Ollama;
		TArray<FString> LMStudio;
		double LookedAt = -1.0;
		int32 Asking = 0;
		TArray<TFunction<void()>> Waiting;
		FString OllamaUrl;
		FString LMStudioUrl;
	};

	FState& State()
	{
		static FState Instance;
		return Instance;
	}

	/** GETs Url and hands over its JSON, or null when the server is not there. */
	void Ask(const FString& Url, TFunction<void(const TSharedPtr<FJsonObject>&)> OnJson)
	{
		const TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(AskSeconds);
		Request->OnProcessRequestComplete().BindLambda([OnJson = MoveTemp(OnJson)](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			TSharedPtr<FJsonObject> Json;
			if (bConnected && Response.IsValid() && EHttpResponseCodes::IsOk(Response->GetResponseCode()))
			{
				FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Json);
			}
			OnJson(Json);
		});
		Request->ProcessRequest();
	}

	/**
	 * Models that can drive an agent: ones that call tools, where the server says (Ollama lists capabilities), and never
	 * an embedding model. ponytail: embedding models are told apart by name, which both servers' lists allow.
	 */
	TArray<FString> Names(const TSharedPtr<FJsonObject>& Json, const TCHAR* ListField, const TCHAR* NameField)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (Json.IsValid() && Json->TryGetArrayField(ListField, List) && List)
		{
			for (const TSharedPtr<FJsonValue>& Entry : *List)
			{
				const TSharedPtr<FJsonObject> Model = Entry.IsValid() ? Entry->AsObject() : nullptr;
				FString Name;
				TArray<FString> Capabilities;
				if (!Model.IsValid() || !Model->TryGetStringField(NameField, Name) || Name.Contains(TEXT("embed"), ESearchCase::IgnoreCase)
					|| !UHyperAIStudioSettings::IsSafeModelId(Name)
					|| (Model->TryGetStringArrayField(TEXT("capabilities"), Capabilities) && !Capabilities.Contains(TEXT("tools"))))
				{
					continue;
				}
				Out.Add(Name);
			}
		}
		return Out;
	}

	void Answered()
	{
		if (--State().Asking > 0)
		{
			return;
		}
		TArray<TFunction<void()>> Callbacks = MoveTemp(State().Waiting);
		State().Waiting.Reset();
		for (TFunction<void()>& Callback : Callbacks)
		{
			Callback();
		}
	}
}

TArray<FString> FHyperAIStudioLocalModels::Get()
{
	FState& Current = State();
	if (Current.LookedAt < 0.0 || FPlatformTime::Seconds() - Current.LookedAt > LookSeconds)
	{
		Refresh();
	}
	TArray<FString> Ids;
	for (const FString& Name : Current.Ollama)
	{
		Ids.Add(TEXT("ollama/") + Name);
	}
	for (const FString& Name : Current.LMStudio)
	{
		Ids.Add(TEXT("lmstudio/") + Name);
	}
	return Ids;
}

void FHyperAIStudioLocalModels::Refresh(TFunction<void()> OnDone)
{
	FState& Current = State();
	if (OnDone)
	{
		Current.Waiting.Add(MoveTemp(OnDone));
	}
	if (Current.Asking > 0)
	{
		return;
	}
	Current.LookedAt = FPlatformTime::Seconds();
	Current.Asking = 2;
	const FString Ollama = Current.OllamaUrl.IsEmpty() ? UHyperAIStudioSettings::GetLocalModelServerUrl(TEXT("ollama")) : Current.OllamaUrl;
	const FString LMStudio = Current.LMStudioUrl.IsEmpty() ? UHyperAIStudioSettings::GetLocalModelServerUrl(TEXT("lmstudio")) : Current.LMStudioUrl;
	Ask(Ollama + TEXT("/api/tags"), [](const TSharedPtr<FJsonObject>& Json)
	{
		State().Ollama = Names(Json, TEXT("models"), TEXT("name"));
		Answered();
	});
	Ask(LMStudio + TEXT("/v1/models"), [](const TSharedPtr<FJsonObject>& Json)
	{
		State().LMStudio = Names(Json, TEXT("data"), TEXT("id"));
		Answered();
	});
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioLocalModels::SetServersForTests(const FString& Ollama, const FString& LMStudio)
{
	State().OllamaUrl = Ollama;
	State().LMStudioUrl = LMStudio;
	State().LookedAt = -1.0;
}
#endif
