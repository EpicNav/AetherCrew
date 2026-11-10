// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameFeaturesToolset59.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFeaturesSubsystem.h"
#include "GameFeaturesSubsystemSettings.h"
#include "GameFeatureTypes.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "PluginDescriptor.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "AsyncFunctionResult/AsyncFunctionResultBool.h"
#include "AsyncFunctionResult/AsyncFunctionResultUtils.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GameFeaturesToolset59)

DEFINE_LOG_CATEGORY(LogGameFeaturesToolset);

namespace UE::GameFeaturesToolset::Private
{

	// Maps the engine's ~34 internal GFP states to
	// the simplified user-facing enum. Transitional and
	// error states all collapse to Unknown.
	static EPluginToolsetGFPState59 ToGFPState(
		EGameFeaturePluginState State)
	{
		switch (State)
		{
		case EGameFeaturePluginState::Uninitialized:
			return EPluginToolsetGFPState59::Uninitialized;
		case EGameFeaturePluginState::Installed:
			return EPluginToolsetGFPState59::Installed;
		case EGameFeaturePluginState::Registered:
			return EPluginToolsetGFPState59::Registered;
		case EGameFeaturePluginState::Loaded:
			return EPluginToolsetGFPState59::Loaded;
		case EGameFeaturePluginState::Active:
			return EPluginToolsetGFPState59::Active;
		default:
			return EPluginToolsetGFPState59::Unknown;
		}
	}

	static void RaiseError(const FString& Message)
	{
		UKismetSystemLibrary::RaiseScriptError(
			FString::Printf(
				TEXT("GameFeaturesToolset: %s"), *Message));
	}

	// Reports why resolution failed instead of raising, so a caller that already has a failure
	// channel of its own does not report the same problem twice.
	static FString ResolvePluginURL(const FString& PluginName,
		UGameFeaturesSubsystem*& OutSubsystem,
		FString& OutError)
	{
		OutSubsystem = GEngine ?
			GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
		if (!OutSubsystem)
		{
			OutError = TEXT("GameFeaturesSubsystem not available. Ensure GameFeatures "
					"plugin is enabled.");
			return FString();
		}

		FString PluginURL;
		if (!OutSubsystem->GetPluginURLByName(PluginName, PluginURL))
		{
			OutError = FString::Printf(TEXT("GFP not found: %s"), *PluginName);
			return FString();
		}

		return PluginURL;
	}

	FString ResolvePluginURLOrRaise(const FString& PluginName,
		UGameFeaturesSubsystem*& OutSubsystem)
	{
		FString Error;
		FString PluginURL = ResolvePluginURL(PluginName, OutSubsystem, Error);
		if (!Error.IsEmpty())
		{
			RaiseError(Error);
		}

		return PluginURL;
	}

	// Completes a pending tool-call result from a plugin state transition. A transition may finish
	// after the tool call has already returned, and a raised script error only reaches the caller
	// while its frame is on the stack, so the pending result is the only channel covering both
	// timings. Verb names the transition for the error text, e.g. "Activate".
	static FGameFeaturePluginChangeStateComplete MakeTransitionCompleteDelegate(
		const FString& Verb,
		const FString& PluginName,
		const TStrongObjectPtr<UAsyncFunctionResultBool>& Result)
	{
		return FGameFeaturePluginChangeStateComplete::CreateLambda(
			[Verb, PluginName, StrongResult = Result](
				const UE::GameFeatures::FResult& TransitionResult)
			{
				if (!TransitionResult.HasError())
				{
					StrongResult->SetValue(true);
					return;
				}

				const FString Message = FString::Printf(TEXT("%s GFP '%s' failed: %s"),
					*Verb, *PluginName, *TransitionResult.GetError());
				UE_LOGF(LogGameFeaturesToolset, Error, "%ls", *Message);
				StrongResult->SetError(Message);
			});
	}

	// Find an IPlugin by name and confirm it is a Game Feature Plugin. Raises on
	// missing plugin or non-GFP and returns nullptr in those cases.
	static TSharedPtr<IPlugin> ResolveGameFeaturePluginOrRaise(const FString& PluginName)
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		if (!Plugin.IsValid())
		{
			RaiseError(FString::Printf(TEXT("Plugin not found: %s"), *PluginName));
			return nullptr;
		}
		if (!UGameFeaturesSubsystem::IsGameFeaturePlugin(Plugin.ToSharedRef()))
		{
			RaiseError(FString::Printf(TEXT("Plugin is not a Game Feature Plugin: %s"), *PluginName));
			return nullptr;
		}
		return Plugin;
	}

	static EPluginToolsetGFPAutoState ToToolsetAutoState(EBuiltInAutoState State)
	{
		switch (State)
		{
		case EBuiltInAutoState::Installed:
			return EPluginToolsetGFPAutoState::Installed;
		case EBuiltInAutoState::Registered:
			return EPluginToolsetGFPAutoState::Registered;
		case EBuiltInAutoState::Loaded:
			return EPluginToolsetGFPAutoState::Loaded;
		case EBuiltInAutoState::Active:
			return EPluginToolsetGFPAutoState::Active;
		case EBuiltInAutoState::Invalid:
			return EPluginToolsetGFPAutoState::Invalid;
		default:
			checkNoEntry();
			return EPluginToolsetGFPAutoState::Invalid;
		}
		static_assert((uint8)EBuiltInAutoState::Count == 5, "Update ToToolsetAutoState to include new EBuiltInAutoState");
	}

	static EBuiltInAutoState ToEngineAutoState(EPluginToolsetGFPAutoState State)
	{
		switch (State)
		{
		case EPluginToolsetGFPAutoState::Installed:
			return EBuiltInAutoState::Installed;
		case EPluginToolsetGFPAutoState::Registered:
			return EBuiltInAutoState::Registered;
		case EPluginToolsetGFPAutoState::Loaded:
			return EBuiltInAutoState::Loaded;
		case EPluginToolsetGFPAutoState::Active:
			return EBuiltInAutoState::Active;
		case EPluginToolsetGFPAutoState::Invalid:
			return EBuiltInAutoState::Invalid;
		default:
			checkNoEntry();
			return EBuiltInAutoState::Invalid;
		}
		static_assert((uint8)EBuiltInAutoState::Count == 5, "Update ToEngineAutoState to include new EBuiltInAutoState");
	}

	// Serialise a single FJsonValue to its JSON literal text form. Wraps in a
	// throwaway object so the standard FJsonSerializer entry point can be used,
	// then strips the wrapper. Returns "null" for a null/invalid value.
	static FString SerializeJsonValueLiteral(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("null");
		}

		TSharedRef<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetField(TEXT("v"), Value);

		// Use the condensed policy so we can mechanically strip the {"v":...} wrapper.
		// The default factory uses TPrettyJsonPrintPolicy which adds whitespace/newlines
		// and would leave the value inside a multi-line block.
		FString Json;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Wrapper, Writer);

		const TCHAR Prefix[] = TEXT("{\"v\":");
		constexpr int32 PrefixLen = UE_ARRAY_COUNT(Prefix) - 1;
		if (Json.StartsWith(Prefix) && Json.EndsWith(TEXT("}")))
		{
			return Json.Mid(PrefixLen, Json.Len() - PrefixLen - 1);
		}
		return Json;
	}

	// Parse a JSON literal into an FJsonValue. Returns nullptr on parse failure.
	static TSharedPtr<FJsonValue> ParseJsonValueLiteral(const FString& Literal)
	{
		const FString Wrapped = FString::Printf(TEXT("{\"v\":%s}"), *Literal);
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Wrapped);
		if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
		{
			return Object->TryGetField(TEXT("v"));
		}
		return nullptr;
	}

	// Build the AI-facing info struct by reading the plugin's in-memory descriptor JSON
	// directly. We bypass UGameFeaturesSubsystem::GetGameFeaturePluginDetails because that
	// path is backed by a filename-keyed cache (CachedPluginDetailsByFilename) with no
	// public flush API, so it would silently return stale data after we rewrite the
	// .uplugin. The plugin manager's descriptor JSON, by contrast, is updated in-place by
	// IPlugin::UpdateDescriptor (see PluginManager.cpp:454-470), so it always reflects the
	// current on-disk state. We only need two top-level fields (BuiltInInitialFeatureState
	// and the configured AdditionalPluginMetadataKeys), which the engine parser also reads
	// from the top level of the descriptor JSON.
	static FGameFeaturePluginDetailsToolsetInfo BuildInfoFromDescriptor(IPlugin& Plugin)
	{
		FGameFeaturePluginDetailsToolsetInfo Info;
		const TSharedPtr<FJsonObject>& Json = Plugin.GetDescriptorJson();
		if (!Json.IsValid())
		{
			return Info;
		}

		// Defer to the engine helper so legacy plugins (which use BuiltInAutoRegister /
		// BuiltInAutoLoad / BuiltInAutoActivate bools instead of BuiltInInitialFeatureState)
		// resolve to a concrete state rather than Invalid. Pass an empty ErrorContext so
		// the helper does not emit per-plugin log spam during reads.
		Info.BuiltInAutoState = ToToolsetAutoState(
			UGameFeaturesSubsystem::DetermineBuiltInInitialFeatureState(Json, FString()));

		const TArray<FString>& Keys =
			GetDefault<UGameFeaturesSubsystemSettings>()->AdditionalPluginMetadataKeys;
		for (const FString& Key : Keys)
		{
			TSharedPtr<FJsonValue> Field = Json->TryGetField(Key);
			if (Field.IsValid())
			{
				Info.AdditionalMetadata.Add(Key, SerializeJsonValueLiteral(Field));
			}
			else
			{
				// Match the engine parser's behaviour for missing keys: empty string.
				Info.AdditionalMetadata.Add(Key, FString());
			}
		}

		return Info;
	}

	// Stage Mutate(Descriptor) onto the resolved plugin, persist via IPlugin::UpdateDescriptor
	// (which rewrites the .uplugin on disk and updates the plugin manager's in-memory
	// descriptor JSON), and return the post-write info. Raises and returns a
	// default-constructed info on any error.
	static FGameFeaturePluginDetailsToolsetInfo CommitDescriptorEdit(
		const FString& PluginName,
		TFunctionRef<void(FPluginDescriptor&)> Mutate)
	{
		TSharedPtr<IPlugin> Plugin = ResolveGameFeaturePluginOrRaise(PluginName);
		if (!Plugin.IsValid())
		{
			return FGameFeaturePluginDetailsToolsetInfo();
		}

		FPluginDescriptor NewDescriptor = Plugin->GetDescriptor();
		Mutate(NewDescriptor);

		FText FailReason;
		if (!Plugin->UpdateDescriptor(NewDescriptor, FailReason))
		{
			RaiseError(FString::Printf(TEXT("Failed to update descriptor for '%s': %s"),
				*PluginName, *FailReason.ToString()));
			return FGameFeaturePluginDetailsToolsetInfo();
		}

		return BuildInfoFromDescriptor(*Plugin);
	}
}

TArray<FString> UGameFeaturesToolset59::ListEnabledGameFeaturePlugins()
{
	using namespace UE::GameFeaturesToolset::Private;
	UGameFeaturesSubsystem* Subsystem = GEngine ?
		GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
	if (!Subsystem)
	{
		RaiseError(TEXT("GameFeaturesSubsystem not available. Ensure GameFeatures plugin is enabled."));
		return TArray<FString>();
	}

	TArray<FString> Names;
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
	{
		// The project policy can disallow an enabled plugin, and the subsystem never registers a
		// plugin it disallows, so none of the other tools could resolve it by name.
		FString UnusedURL;
		if (UGameFeaturesSubsystem::IsGameFeaturePlugin(Plugin)
			&& Subsystem->GetPluginURLByName(Plugin->GetName(), UnusedURL))
		{
			Names.Add(Plugin->GetName());
		}
	}
	Names.Sort();
	return Names;
}

TArray<FString> UGameFeaturesToolset59::ListDiscoveredGameFeaturePlugins()
{
	TArray<FString> Names;
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
	{
		if (UGameFeaturesSubsystem::IsGameFeaturePlugin(Plugin))
		{
			Names.Add(Plugin->GetName());
		}
	}
	Names.Sort();
	return Names;
}

bool UGameFeaturesToolset59::IsGameFeaturePlugin(const FString& PluginName)
{
	using namespace UE::GameFeaturesToolset::Private;
	TSharedPtr<IPlugin> FoundPlugin = IPluginManager::Get().FindPlugin(PluginName);
	if(!FoundPlugin)
	{
		RaiseError(FString::Printf(TEXT("Could not find a plugin with name '%s'"), *PluginName));
		return false;
	}
	return UGameFeaturesSubsystem::IsGameFeaturePlugin(FoundPlugin);
}

bool UGameFeaturesToolset59::IsGameFeatureActive(const FString& PluginName)
{
	using namespace UE::GameFeaturesToolset::Private;

	UGameFeaturesSubsystem* Subsystem = nullptr;
	FString PluginURL = ResolvePluginURLOrRaise(PluginName, Subsystem);
	if (PluginURL.IsEmpty())
	{
		return false;
	}
	check(Subsystem);
	EGameFeaturePluginState State =	Subsystem->GetPluginState(PluginURL);
	return State == EGameFeaturePluginState::Active;
}

EPluginToolsetGFPState59
UGameFeaturesToolset59::GetGameFeatureState(const FString& PluginName)
{
	using namespace UE::GameFeaturesToolset::Private;

	UGameFeaturesSubsystem* Subsystem = nullptr;
	FString PluginURL = ResolvePluginURLOrRaise(PluginName, Subsystem);
	if (PluginURL.IsEmpty())
	{
		return EPluginToolsetGFPState59::Unknown;
	}
	check(Subsystem);

	EGameFeaturePluginState State =	Subsystem->GetPluginState(PluginURL);
	return ToGFPState(State);
}

UAsyncFunctionResultBool* UGameFeaturesToolset59::RequestActivateGameFeature(const FString& PluginName)
{
	using namespace UE::AsyncFunctionResult;
	using namespace UE::GameFeaturesToolset::Private;
	UGameFeaturesSubsystem* Subsystem = nullptr;
	FString ResolveError;
	FString PluginURL = ResolvePluginURL(PluginName, Subsystem, ResolveError);
	if (PluginURL.IsEmpty())
	{
		return MakeErrorResult<UAsyncFunctionResultBool>(ResolveError);
	}
	check(Subsystem);

	TStrongObjectPtr<UAsyncFunctionResultBool> Result(NewObject<UAsyncFunctionResultBool>());
	Subsystem->LoadAndActivateGameFeaturePlugin(
		PluginURL,
		MakeTransitionCompleteDelegate(TEXT("Activate"), PluginName, Result));

	return Result.Get();
}

UAsyncFunctionResultBool* UGameFeaturesToolset59::RequestLoadGameFeature(const FString& PluginName)
{
	using namespace UE::AsyncFunctionResult;
	using namespace UE::GameFeaturesToolset::Private;
	UGameFeaturesSubsystem* Subsystem = nullptr;
	FString ResolveError;
	FString PluginURL = ResolvePluginURL(PluginName, Subsystem, ResolveError);
	if (PluginURL.IsEmpty())
	{
		return MakeErrorResult<UAsyncFunctionResultBool>(ResolveError);
	}
	check(Subsystem);

	TStrongObjectPtr<UAsyncFunctionResultBool> Result(NewObject<UAsyncFunctionResultBool>());
	Subsystem->LoadGameFeaturePlugin(
		PluginURL,
		MakeTransitionCompleteDelegate(TEXT("Load"), PluginName, Result));

	return Result.Get();
}

UAsyncFunctionResultBool* UGameFeaturesToolset59::RequestDeactivateGameFeature(
	const FString& PluginName)
{
	using namespace UE::AsyncFunctionResult;
	using namespace UE::GameFeaturesToolset::Private;
	UGameFeaturesSubsystem* Subsystem = nullptr;
	FString ResolveError;
	FString PluginURL = ResolvePluginURL(PluginName, Subsystem, ResolveError);
	if (PluginURL.IsEmpty())
	{
		return MakeErrorResult<UAsyncFunctionResultBool>(ResolveError);
	}
	check(Subsystem);

	TStrongObjectPtr<UAsyncFunctionResultBool> Result(NewObject<UAsyncFunctionResultBool>());
	Subsystem->DeactivateGameFeaturePlugin(
		PluginURL,
		MakeTransitionCompleteDelegate(TEXT("Deactivate"), PluginName, Result));

	return Result.Get();
}

UAsyncFunctionResultBool* UGameFeaturesToolset59::RequestUnloadGameFeature(
	const FString& PluginName,
	bool bKeepRegistered)
{
	using namespace UE::AsyncFunctionResult;
	using namespace UE::GameFeaturesToolset::Private;
	UGameFeaturesSubsystem* Subsystem = nullptr;
	FString ResolveError;
	FString PluginURL = ResolvePluginURL(PluginName, Subsystem, ResolveError);
	if (PluginURL.IsEmpty())
	{
		return MakeErrorResult<UAsyncFunctionResultBool>(ResolveError);
	}
	check(Subsystem);

	TStrongObjectPtr<UAsyncFunctionResultBool> Result(NewObject<UAsyncFunctionResultBool>());
	Subsystem->UnloadGameFeaturePlugin(
		PluginURL,
		MakeTransitionCompleteDelegate(TEXT("Unload"), PluginName, Result),
		bKeepRegistered);

	return Result.Get();
}

FGameFeaturePluginDetailsToolsetInfo
UGameFeaturesToolset59::GetGameFeaturePluginDetails(const FString& PluginName)
{
	using namespace UE::GameFeaturesToolset::Private;

	TSharedPtr<IPlugin> Plugin = ResolveGameFeaturePluginOrRaise(PluginName);
	if (!Plugin.IsValid())
	{
		return FGameFeaturePluginDetailsToolsetInfo();
	}

	return BuildInfoFromDescriptor(*Plugin);
}

TArray<FString> UGameFeaturesToolset59::GetConfiguredAdditionalMetadataKeys()
{
	TArray<FString> Keys = GetDefault<UGameFeaturesSubsystemSettings>()->AdditionalPluginMetadataKeys;
	Keys.Sort();
	return Keys;
}

FGameFeaturePluginDetailsToolsetInfo
UGameFeaturesToolset59::SetGameFeaturePluginBuiltInAutoState(const FString& PluginName, EPluginToolsetGFPAutoState NewState)
{
	using namespace UE::GameFeaturesToolset::Private;

	if (NewState == EPluginToolsetGFPAutoState::Invalid)
	{
		RaiseError(TEXT("NewState must not be Invalid."));
		return FGameFeaturePluginDetailsToolsetInfo();
	}

	const FString StateStr = LexToString(ToEngineAutoState(NewState));

	return CommitDescriptorEdit(PluginName, [&StateStr](FPluginDescriptor& Descriptor)
	{
		// Mirror FFortGameFeaturePluginMetadataCustomization::CommitEdits: stage the new
		// BuiltInInitialFeatureState into AdditionalFieldsToWrite. The descriptor save
		// path (FPluginDescriptor::UpdateJson) will apply these on top of the existing
		// .uplugin JSON, preserving unknown fields.
		Descriptor.AdditionalFieldsToWrite.FindOrAdd(TEXT("BuiltInInitialFeatureState")) =
			MakeShared<FJsonValueString>(StateStr);
		Descriptor.AdditionalFieldsToRemove.Remove(TEXT("BuiltInInitialFeatureState"));
	});
}

FGameFeaturePluginDetailsToolsetInfo
UGameFeaturesToolset59::SetGameFeaturePluginMetadataValue(const FString& PluginName, const FString& Key, const FString& JsonValueLiteral)
{
	using namespace UE::GameFeaturesToolset::Private;

	const TArray<FString>& ConfiguredKeys =
		GetDefault<UGameFeaturesSubsystemSettings>()->AdditionalPluginMetadataKeys;
	if (!ConfiguredKeys.Contains(Key))
	{
		RaiseError(FString::Printf(TEXT("Metadata key '%s' is not in UGameFeaturesSubsystemSettings::AdditionalPluginMetadataKeys."), *Key));
		return FGameFeaturePluginDetailsToolsetInfo();
	}

	TSharedPtr<FJsonValue> ParsedValue = ParseJsonValueLiteral(JsonValueLiteral);
	if (!ParsedValue.IsValid())
	{
		RaiseError(FString::Printf(TEXT("JsonValueLiteral for key '%s' is not valid JSON: %s"), *Key, *JsonValueLiteral));
		return FGameFeaturePluginDetailsToolsetInfo();
	}

	return CommitDescriptorEdit(PluginName, [&Key, &ParsedValue](FPluginDescriptor& Descriptor)
	{
		Descriptor.AdditionalFieldsToWrite.FindOrAdd(Key) = ParsedValue;
		Descriptor.AdditionalFieldsToRemove.Remove(Key);
	});
}

FGameFeaturePluginDetailsToolsetInfo
UGameFeaturesToolset59::RemoveGameFeaturePluginMetadataValue(const FString& PluginName, const FString& Key)
{
	using namespace UE::GameFeaturesToolset::Private;

	const TArray<FString>& ConfiguredKeys =
		GetDefault<UGameFeaturesSubsystemSettings>()->AdditionalPluginMetadataKeys;
	if (!ConfiguredKeys.Contains(Key))
	{
		RaiseError(FString::Printf(TEXT("Metadata key '%s' is not in UGameFeaturesSubsystemSettings::AdditionalPluginMetadataKeys."), *Key));
		return FGameFeaturePluginDetailsToolsetInfo();
	}

	return CommitDescriptorEdit(PluginName, [&Key](FPluginDescriptor& Descriptor)
	{
		Descriptor.AdditionalFieldsToWrite.Remove(Key);
		Descriptor.AdditionalFieldsToRemove.Add(Key);
	});
}

bool UGameFeaturesToolset59::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}
