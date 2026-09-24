// Games by Hyper 2026.

#include "HyperAIStudioExternalService.h"

#include "HttpModule.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/App.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioServices, Log, All);

namespace HyperAIStudio::Services::Private
{
	struct FState
	{
		TMap<FString, FHyperAIStudioServiceStatus> Health;
		TSet<FString> InFlightChecks;
		/** Spend booked this session, per service, in whatever currency it bills. */
		TMap<FString, double> Spend;
	};

	FState& State()
	{
		static FState Value;
		return Value;
	}

	double TotalSpend()
	{
		double Total = 0.0;
		for (const TPair<FString, double>& Entry : State().Spend)
		{
			Total += Entry.Value;
		}
		return Total;
	}

	FString JoinUrl(const FString& BaseUrl, const FString& Path)
	{
		FString Url = BaseUrl;
		while (Url.EndsWith(TEXT("/")))
		{
			Url.LeftChopInline(1);
		}
		if (Path.IsEmpty())
		{
			return Url;
		}
		return Path.StartsWith(TEXT("/")) ? Url + Path : Url + TEXT("/") + Path;
	}

	/** Reads the key without ever returning it to a caller or putting it in a log line. */
	FString ReadKey(const FHyperAIStudioExternalServiceSettings& Service)
	{
		return Service.ApiKeyVariable.IsEmpty()
			? FString()
			: FPlatformMisc::GetEnvironmentVariable(*Service.ApiKeyVariable);
	}

	void SetHealth(const FString& Id, const EHyperAIStudioServiceHealth Health, const FString& Diagnostic, const int32 RoundTripMs)
	{
		FHyperAIStudioServiceStatus& Status = State().Health.FindOrAdd(Id);
		Status.Id = Id;
		Status.Health = Health;
		Status.Diagnostic = Diagnostic;
		Status.CheckedUtc = FDateTime::UtcNow();
		Status.RoundTripMs = RoundTripMs;
		State().InFlightChecks.Remove(Id);
		FHyperAIStudioExternalServices::OnHealthChanged().Broadcast();
	}

	void StartHealthCheck(const FHyperAIStudioExternalServiceSettings& Service)
	{
		if (State().InFlightChecks.Contains(Service.Id))
		{
			return;
		}
		if (Service.BaseUrl.IsEmpty())
		{
			SetHealth(Service.Id, EHyperAIStudioServiceHealth::Unreachable,
				TEXT("No base url is configured for this service."), 0);
			return;
		}
		if (!FHyperAIStudioExternalServices::HasApiKey(Service))
		{
			SetHealth(Service.Id, EHyperAIStudioServiceHealth::KeyMissing,
				FString::Printf(TEXT("Set %s in the environment; keys are never kept in project config."), *Service.ApiKeyVariable), 0);
			return;
		}
		State().InFlightChecks.Add(Service.Id);
		const FString Id = Service.Id;
		const FString Url = JoinUrl(Service.BaseUrl, Service.HealthPath);
		const double Started = FPlatformTime::Seconds();
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(FHyperAIStudioExternalServices::HealthTimeoutSeconds);
		Request->OnProcessRequestComplete().BindLambda(
			[Id, Url, Started](FHttpRequestPtr, const FHttpResponsePtr Response, const bool bConnected)
			{
				const int32 RoundTripMs = static_cast<int32>((FPlatformTime::Seconds() - Started) * 1000.0);
				if (!bConnected || !Response.IsValid())
				{
					SetHealth(Id, EHyperAIStudioServiceHealth::Unreachable,
						FString::Printf(TEXT("Nothing answered at %s; start the service, or correct its base url."), *Url), RoundTripMs);
					return;
				}
				const int32 Code = Response->GetResponseCode();
				// Anything that answers is running. A 404 only means this service has no health path.
				const bool bReachable = Code > 0 && Code < 500;
				SetHealth(Id, bReachable ? EHyperAIStudioServiceHealth::Reachable : EHyperAIStudioServiceHealth::Unreachable,
					bReachable
						? FString::Printf(TEXT("Answered %d in %d ms."), Code, RoundTripMs)
						: FString::Printf(TEXT("%s answered %d; the service is up but not ready."), *Url, Code),
					RoundTripMs);
			});
		if (!Request->ProcessRequest())
		{
			SetHealth(Id, EHyperAIStudioServiceHealth::Unreachable, TEXT("The request could not be sent."), 0);
		}
	}
}

TArray<FHyperAIStudioExternalServiceSettings> FHyperAIStudioExternalServices::GetServices()
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	TArray<FHyperAIStudioExternalServiceSettings> Services = Settings ? Settings->ExternalServices : TArray<FHyperAIStudioExternalServiceSettings>();
	// Built in for the Nano Banana models; a project's own entry with the same id wins, as when it goes through a proxy.
	const FHyperAIStudioExternalServiceSettings Gemini = UHyperAIStudioSettings::GetGeminiService();
	if (!Services.ContainsByPredicate([&Gemini](const FHyperAIStudioExternalServiceSettings& Service) { return Service.Id.Equals(Gemini.Id, ESearchCase::IgnoreCase); }))
	{
		Services.Add(Gemini);
	}
	return Services;
}

bool FHyperAIStudioExternalServices::Find(const FString& Id, FHyperAIStudioExternalServiceSettings& OutService)
{
	for (const FHyperAIStudioExternalServiceSettings& Service : GetServices())
	{
		if (Service.Id.Equals(Id, ESearchCase::IgnoreCase))
		{
			OutService = Service;
			return true;
		}
	}
	return false;
}

bool FHyperAIStudioExternalServices::HasApiKey(const FHyperAIStudioExternalServiceSettings& Service)
{
	return Service.ApiKeyVariable.IsEmpty() || !HyperAIStudio::Services::Private::ReadKey(Service).IsEmpty();
}

FHyperAIStudioServiceStatus FHyperAIStudioExternalServices::QueryHealth(const FString& Id)
{
	using namespace HyperAIStudio::Services::Private;
	FHyperAIStudioExternalServiceSettings Service;
	if (!Find(Id, Service))
	{
		FHyperAIStudioServiceStatus Missing;
		Missing.Id = Id;
		Missing.Health = EHyperAIStudioServiceHealth::Unreachable;
		Missing.Diagnostic = FString::Printf(TEXT("No service is configured as %s."), *Id);
		return Missing;
	}
	const FHyperAIStudioServiceStatus* Cached = State().Health.Find(Id);
	const bool bStale = !Cached
		|| (FDateTime::UtcNow() - Cached->CheckedUtc).GetTotalSeconds() > HealthTimeToLiveSeconds;
	if (bStale && IsInGameThread())
	{
		StartHealthCheck(Service);
	}
	if (const FHyperAIStudioServiceStatus* Now = State().Health.Find(Id))
	{
		return *Now;
	}
	FHyperAIStudioServiceStatus Pending;
	Pending.Id = Id;
	Pending.Diagnostic = TEXT("Checking.");
	return Pending;
}

void FHyperAIStudioExternalServices::RequestHealthCheck(const FString& Id)
{
	using namespace HyperAIStudio::Services::Private;
	FHyperAIStudioExternalServiceSettings Service;
	if (IsInGameThread() && Find(Id, Service))
	{
		State().Health.Remove(Id);
		StartHealthCheck(Service);
	}
}

FSimpleMulticastDelegate& FHyperAIStudioExternalServices::OnHealthChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

bool FHyperAIStudioExternalServices::Reserve(const FString& Id, const int32 Calls, double& OutEstimatedCost, FString& OutError,
	const double CostPerCall)
{
	using namespace HyperAIStudio::Services::Private;
	OutEstimatedCost = 0.0;
	OutError.Reset();
	FHyperAIStudioExternalServiceSettings Service;
	if (!Find(Id, Service))
	{
		OutError = FString::Printf(TEXT("No service is configured as %s."), *Id);
		return false;
	}
	if (Calls <= 0)
	{
		OutError = TEXT("A reservation needs at least one call.");
		return false;
	}
	OutEstimatedCost = (CostPerCall >= 0.0 ? CostPerCall : static_cast<double>(Service.CostPerCall)) * Calls;
	if (OutEstimatedCost <= 0.0)
	{
		return true;
	}
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const double Cap = Settings ? static_cast<double>(Settings->ExternalServiceSessionSpendCap) : 0.0;
	const double Spent = TotalSpend();
	if (Spent + OutEstimatedCost > Cap)
	{
		OutError = FString::Printf(
			TEXT("%d calls to %s would cost about %.2f, and %.2f of this session's %.2f cap is already spent. ")
			TEXT("Raise External Service Session Spend Cap, or ask for fewer."),
			Calls, *Service.Id, OutEstimatedCost, Spent, Cap);
		return false;
	}
	return true;
}

void FHyperAIStudioExternalServices::RecordSpend(const FString& Id, const int32 Calls, const double CostPerCall)
{
	using namespace HyperAIStudio::Services::Private;
	FHyperAIStudioExternalServiceSettings Service;
	if (Calls <= 0 || !Find(Id, Service))
	{
		return;
	}
	const double Each = CostPerCall >= 0.0 ? CostPerCall : static_cast<double>(Service.CostPerCall);
	if (Each > 0.0)
	{
		State().Spend.FindOrAdd(Service.Id) += Each * Calls;
	}
}

double FHyperAIStudioExternalServices::GetSessionSpend()
{
	return HyperAIStudio::Services::Private::TotalSpend();
}

void FHyperAIStudioExternalServices::ResetSessionSpend()
{
	HyperAIStudio::Services::Private::State().Spend.Reset();
}

bool FHyperAIStudioExternalServices::PostJson(
	const FString& Id, const FString& Path, const FString& JsonBody, FOnResponse OnResponse, FString& OutError, const double CostPerCall)
{
	using namespace HyperAIStudio::Services::Private;
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("External service calls start on the game thread.");
		return false;
	}
	FHyperAIStudioExternalServiceSettings Service;
	if (!Find(Id, Service))
	{
		OutError = FString::Printf(TEXT("No service is configured as %s."), *Id);
		return false;
	}
	if (Service.BaseUrl.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s has no base url."), *Service.Id);
		return false;
	}
	if (!HasApiKey(Service))
	{
		OutError = FString::Printf(TEXT("%s needs a key in the environment variable %s."), *Service.Id, *Service.ApiKeyVariable);
		return false;
	}
	if (JsonBody.Len() <= 0 || JsonBody.Len() > MaxRequestBodyBytes)
	{
		OutError = TEXT("The request body is empty or past the bounded size.");
		return false;
	}

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(JoinUrl(Service.BaseUrl, Path));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	const float Timeout = static_cast<float>(FMath::Clamp(Service.TimeoutSeconds, 1, 15 * 60));
	Request->SetTimeout(Timeout);
	// A model that renders for a minute before answering sends nothing in the meantime, and the default
	// activity timeout would abandon the request at thirty seconds with the work already under way.
	Request->SetActivityTimeout(Timeout);
	if (!Service.ApiKeyVariable.IsEmpty() && !Service.ApiKeyHeader.IsEmpty())
	{
		Request->SetHeader(Service.ApiKeyHeader, Service.ApiKeyPrefix + ReadKey(Service));
	}
	Request->SetContentAsString(JsonBody);

	const FString Url = Request->GetURL();
	Request->OnProcessRequestComplete().BindLambda(
		[OnResponse = MoveTemp(OnResponse), Url](FHttpRequestPtr, const FHttpResponsePtr Response, const bool bConnected)
		{
			TArray<uint8> Body;
			TMap<FString, FString> Headers;
			if (!bConnected || !Response.IsValid())
			{
				OnResponse(false, Body, Headers, FString::Printf(TEXT("No answer from %s."), *Url));
				return;
			}
			for (const FString& Header : Response->GetAllHeaders())
			{
				FString Key;
				FString Value;
				if (Header.Split(TEXT(": "), &Key, &Value))
				{
					Headers.Add(Key, Value);
				}
			}
			const int32 Code = Response->GetResponseCode();
			Body = Response->GetContent();
			if (Body.Num() > MaxResponseBodyBytes)
			{
				OnResponse(false, TArray<uint8>(), Headers,
					FString::Printf(TEXT("%s answered with %d bytes, past the bounded size."), *Url, Body.Num()));
				return;
			}
			if (Code < 200 || Code >= 300)
			{
				// The body of an error is the useful part: these services explain themselves in it.
				OnResponse(false, Body, Headers,
					FString::Printf(TEXT("%s answered %d: %s"), *Url, Code, *Response->GetContentAsString().Left(512)));
				return;
			}
			OnResponse(true, Body, Headers, FString());
		});

	if (!Request->ProcessRequest())
	{
		OutError = FString::Printf(TEXT("The request to %s could not be sent."), *Url);
		return false;
	}
	RecordSpend(Service.Id, 1, CostPerCall);
	return true;
}

const TCHAR* FHyperAIStudioExternalServices::LexToString(const EHyperAIStudioServiceHealth Health)
{
	switch (Health)
	{
	case EHyperAIStudioServiceHealth::Reachable: return TEXT("reachable");
	case EHyperAIStudioServiceHealth::Unreachable: return TEXT("unreachable");
	case EHyperAIStudioServiceHealth::KeyMissing: return TEXT("key_missing");
	default: return TEXT("unknown");
	}
}

void FHyperAIStudioExternalServices::Shutdown()
{
	HyperAIStudio::Services::Private::State() = HyperAIStudio::Services::Private::FState();
}
