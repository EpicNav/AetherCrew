// Copyright Epic Games, Inc. All Rights Reserved.

// RewindDebuggerToolset
#include "RewindDebuggerToolset.h"
#include "RewindDebuggerToolsetEventDecoder.h"

// Core
#include "Features/IModularFeatures.h"
#include "HAL/IConsoleManager.h"
#include "Math/NumericLimits.h"

// Editor
#include "Editor.h"
#include "Subsystems/SubsystemCollection.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

// GameplayInsights
#include "IGameplayProvider.h"

// Json
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// RewindDebuggerInterface
#include "IRewindDebugger.h"

// TraceServices
#include "Common/ProviderLock.h"
#include "TraceServices/Containers/Timelines.h"
#include "TraceServices/Model/AnalysisSession.h"

DEFINE_LOG_CATEGORY_STATIC(LogRewindDebuggerToolset, Log, All);

static bool bEnableRewindDebuggerToolset = true;

static void OnEnableRewindDebuggerToolsetChanged(IConsoleVariable* Variable)
{
	if (!GEditor)
	{
		return;
	}

	if (URewindDebuggerToolset* Toolset = GEditor->GetEditorSubsystem<URewindDebuggerToolset>())
	{
		Toolset->SetToolsetEnabled(Variable->GetBool());
	}
}

static FAutoConsoleVariableRef CVarEnableRewindDebuggerToolset(
	TEXT("RewindDebuggerToolset.Enable"),
	bEnableRewindDebuggerToolset,
	TEXT("Enable or disable the RewindDebugger toolset registration. When disabled, RewindDebugger tools will not appear in the MCP tool list."),
	FConsoleVariableDelegate::CreateStatic(&OnEnableRewindDebuggerToolsetChanged),
	ECVF_Default);

void URewindDebuggerToolset::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UToolsetRegistrySubsystem>();
	SetToolsetEnabled(bEnableRewindDebuggerToolset);
}

void URewindDebuggerToolset::Deinitialize()
{
	SetToolsetEnabled(false);
	Super::Deinitialize();
}

void URewindDebuggerToolset::SetToolsetEnabled(bool bEnabled)
{
	// RegisterToolsetClass returns void and can bail without registering, so assuming success
	// here would latch the flag on and stop the guard below ever retrying.
	if (bEnabled && !bToolsetRegistered)
	{
		UToolsetRegistry::RegisterToolsetClass(URewindDebuggerToolset::StaticClass());
		bToolsetRegistered = UToolsetRegistry::IsToolsetClassRegistered(
			URewindDebuggerToolset::StaticClass());
	}
	else if (!bEnabled && bToolsetRegistered)
	{
		UToolsetRegistry::UnregisterToolsetClass(URewindDebuggerToolset::StaticClass());
		bToolsetRegistered = UToolsetRegistry::IsToolsetClassRegistered(
			URewindDebuggerToolset::StaticClass());
	}
}

static FString SerializeJsonObject(const TSharedRef<FJsonObject>& JsonObject)
{
	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(JsonObject, Writer);
	return Output;
}

static FString SerializeJsonArray(const TArray<TSharedPtr<FJsonValue>>& JsonArray)
{
	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(JsonArray, Writer);
	return Output;
}

FString URewindDebuggerToolset::GetSessionInfo()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	IRewindDebugger* Debugger = IRewindDebugger::Instance();
	if (!Debugger)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetSessionInfo: no IRewindDebugger instance");
		Root->SetBoolField(TEXT("IsActive"), false);
		Root->SetNumberField(TEXT("ScrubTime"), 0.0);
		Root->SetNumberField(TEXT("RecordingDuration"), 0.0);
		Root->SetNumberField(TEXT("TraceRangeStart"), 0.0);
		Root->SetNumberField(TEXT("TraceRangeEnd"), 0.0);
		return SerializeJsonObject(Root);
	}

	const TraceServices::IAnalysisSession* Session = Debugger->GetAnalysisSession();
	if (!Session)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetSessionInfo: no active analysis session");
		Root->SetBoolField(TEXT("IsActive"), false);
		Root->SetNumberField(TEXT("ScrubTime"), 0.0);
		Root->SetNumberField(TEXT("RecordingDuration"), 0.0);
		Root->SetNumberField(TEXT("TraceRangeStart"), 0.0);
		Root->SetNumberField(TEXT("TraceRangeEnd"), 0.0);
		return SerializeJsonObject(Root);
	}

	const TRange<double>& TraceRange = Debugger->GetCurrentTraceRange();

	Root->SetBoolField(TEXT("IsActive"), true);
	Root->SetNumberField(TEXT("ScrubTime"), Debugger->GetScrubTime());
	Root->SetNumberField(TEXT("RecordingDuration"), Debugger->GetRecordingDuration());
	Root->SetNumberField(TEXT("TraceRangeStart"), TraceRange.HasLowerBound() ? TraceRange.GetLowerBoundValue() : 0.0);
	Root->SetNumberField(TEXT("TraceRangeEnd"), TraceRange.HasUpperBound() ? TraceRange.GetUpperBoundValue() : 0.0);
	return SerializeJsonObject(Root);
}

FString URewindDebuggerToolset::ListObjects(const FString& ClassName)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ObjectArray;

	IRewindDebugger* Debugger = IRewindDebugger::Instance();
	if (!Debugger)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "ListObjects: no IRewindDebugger instance");
		Root->SetArrayField(TEXT("Objects"), ObjectArray);
		return SerializeJsonObject(Root);
	}

	const TraceServices::IAnalysisSession* Session = Debugger->GetAnalysisSession();
	if (!Session)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "ListObjects: no active analysis session");
		Root->SetArrayField(TEXT("Objects"), ObjectArray);
		return SerializeJsonObject(Root);
	}

	const IGameplayProvider* Provider = Session->ReadProvider<IGameplayProvider>(IGameplayProvider::ProviderName);
	if (!Provider)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "ListObjects: GameplayProvider not available");
		Root->SetArrayField(TEXT("Objects"), ObjectArray);
		return SerializeJsonObject(Root);
	}

	TraceServices::FProviderReadScopeLock ProviderReadScope(*Provider);

	Provider->EnumerateObjects([&](const FObjectInfo& Info)
	{
		const FClassInfo* ClassInfo = Provider->FindClassInfo(Info.ClassId);
		const FString ObjectClassName = ClassInfo ? FString(ClassInfo->Name) : FString();

		if (!ClassName.IsEmpty() && !ObjectClassName.Contains(ClassName, ESearchCase::IgnoreCase))
		{
			return;
		}

		const TRange<double> Lifetime = Provider->GetObjectTraceLifetime(Info.GetId());

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("ObjectId"), static_cast<double>(Info.GetUObjectId()));
		Entry->SetStringField(TEXT("Name"), FString(Info.Name));
		Entry->SetStringField(TEXT("ClassName"), ObjectClassName);
		Entry->SetNumberField(TEXT("LifetimeStart"), Lifetime.HasLowerBound() ? Lifetime.GetLowerBoundValue() : 0.0);
		Entry->SetNumberField(TEXT("LifetimeEnd"), Lifetime.HasUpperBound() ? Lifetime.GetUpperBoundValue() : 0.0);

		ObjectArray.Add(MakeShared<FJsonValueObject>(Entry));
	});

	Root->SetArrayField(TEXT("Objects"), ObjectArray);
	return SerializeJsonObject(Root);
}

FString URewindDebuggerToolset::GetObjectEvents(int64 ObjectId, double StartTime, double EndTime)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> EventArray;

	IRewindDebugger* Debugger = IRewindDebugger::Instance();
	if (!Debugger)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetObjectEvents: no IRewindDebugger instance");
		Root->SetArrayField(TEXT("Events"), EventArray);
		return SerializeJsonObject(Root);
	}

	const TraceServices::IAnalysisSession* Session = Debugger->GetAnalysisSession();
	if (!Session)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetObjectEvents: no active analysis session");
		Root->SetArrayField(TEXT("Events"), EventArray);
		return SerializeJsonObject(Root);
	}

	const IGameplayProvider* Provider = Session->ReadProvider<IGameplayProvider>(IGameplayProvider::ProviderName);
	if (!Provider)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetObjectEvents: GameplayProvider not available");
		Root->SetArrayField(TEXT("Events"), EventArray);
		return SerializeJsonObject(Root);
	}

	TraceServices::FProviderReadScopeLock ProviderReadScope(*Provider);

	const RewindDebugger::FObjectId ObjId(static_cast<uint64>(ObjectId));
	Provider->ReadObjectEventsTimeline(ObjId, [&](const IGameplayProvider::ObjectEventsTimeline& Timeline)
	{
		Timeline.EnumerateEvents(StartTime, EndTime, [&](bool bStart, double Time, const FObjectEventMessage& Message) -> TraceServices::EEventEnumerate
		{
			if (bStart)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("Time"), Time);
				Entry->SetStringField(TEXT("Name"), FString(Message.Name));
				EventArray.Add(MakeShared<FJsonValueObject>(Entry));
			}
			return TraceServices::EEventEnumerate::Continue;
		});
	});

	Root->SetArrayField(TEXT("Events"), EventArray);
	return SerializeJsonObject(Root);
}

FString URewindDebuggerToolset::GetObjectProperties(int64 ObjectId, double Time)
{
	IRewindDebugger* Debugger = IRewindDebugger::Instance();
	if (!Debugger)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetObjectProperties: no IRewindDebugger instance");
		return TEXT("{}");
	}

	const TraceServices::IAnalysisSession* Session = Debugger->GetAnalysisSession();
	if (!Session)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetObjectProperties: no active analysis session");
		return TEXT("{}");
	}

	const IGameplayProvider* Provider = Session->ReadProvider<IGameplayProvider>(IGameplayProvider::ProviderName);
	if (!Provider)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetObjectProperties: GameplayProvider not available");
		return TEXT("{}");
	}

	TraceServices::FProviderReadScopeLock ProviderReadScope(*Provider);

	const uint64 ObjId = static_cast<uint64>(ObjectId);
	FObjectPropertiesMessage ClosestMessage;
	bool bFound = false;

	Provider->ReadObjectPropertiesTimeline(ObjId, [&](const IGameplayProvider::ObjectPropertiesTimeline& Timeline)
	{
		// Lower bound is the lowest representable double so snapshots recorded before recording time 0
		// (possible when the trace's start time differs from the session's time origin) are not missed.
		Timeline.EnumerateEvents(TNumericLimits<double>::Lowest(), Time, [&](bool bStart, double /*EventTime*/, const FObjectPropertiesMessage& Message) -> TraceServices::EEventEnumerate
		{
			if (bStart)
			{
				ClosestMessage = Message;
				bFound = true;
			}
			return TraceServices::EEventEnumerate::Continue;
		});
	});

	if (!bFound)
	{
		return TEXT("{}");
	}

	TArray<TSharedPtr<FJsonValue>> PropertyArray;
	Provider->EnumerateObjectPropertyValues(ObjId, ClosestMessage, [&](const FObjectPropertyValue& PropertyValue)
	{
		const TCHAR* PropertyName = Provider->GetPropertyName(PropertyValue.NameId);

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("Name"), PropertyName ? FString(PropertyName) : FString());
		Entry->SetStringField(TEXT("Value"), PropertyValue.Value ? FString(PropertyValue.Value) : FString());
		Entry->SetNumberField(TEXT("ParentId"), static_cast<double>(PropertyValue.ParentId));
		PropertyArray.Add(MakeShared<FJsonValueObject>(Entry));
	});

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("ProfileTime"), ClosestMessage.ProfileTime);
	Root->SetNumberField(TEXT("ElapsedTime"), ClosestMessage.ElapsedTime);
	Root->SetArrayField(TEXT("Properties"), PropertyArray);
	return SerializeJsonObject(Root);
}

FString URewindDebuggerToolset::GetSelectedObject()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	IRewindDebugger* Debugger = IRewindDebugger::Instance();
	if (!Debugger)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetSelectedObject: no IRewindDebugger instance");
		Root->SetStringField(TEXT("Error"), TEXT("Rewind Debugger is not available."));
		return SerializeJsonObject(Root);
	}

	const TraceServices::IAnalysisSession* Session = Debugger->GetAnalysisSession();
	if (!Session)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetSelectedObject: no active analysis session");
		Root->SetStringField(TEXT("Error"), TEXT("No active analysis session."));
		return SerializeJsonObject(Root);
	}

	const IGameplayProvider* Provider = Session->ReadProvider<IGameplayProvider>(IGameplayProvider::ProviderName);
	if (!Provider)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetSelectedObject: GameplayProvider not available");
		Root->SetStringField(TEXT("Error"), TEXT("GameplayProvider not available."));
		return SerializeJsonObject(Root);
	}

	const TSharedPtr<FDebugObjectInfo> Selected = Debugger->GetSelectedObject();
	if (!Selected.IsValid())
	{
		Root->SetStringField(TEXT("Error"), TEXT("No object is currently selected in the Rewind Debugger."));
		return SerializeJsonObject(Root);
	}

	const uint64 ObjectIdU64 = Selected->GetUObjectId();
	const int64 ObjectId = static_cast<int64>(ObjectIdU64);

	{
		TraceServices::FProviderReadScopeLock ProviderReadScope(*Provider);

		const FObjectInfo* ObjectInfo = Provider->FindObjectInfo(ObjectIdU64);
		if (!ObjectInfo)
		{
			Root->SetStringField(TEXT("Error"), FString::Printf(TEXT("No recorded object with id %lld."), ObjectId));
			return SerializeJsonObject(Root);
		}

		const FClassInfo* ClassInfo = Provider->FindClassInfo(ObjectInfo->ClassId);
		Root->SetNumberField(TEXT("ObjectId"), static_cast<double>(ObjectId));
		Root->SetStringField(TEXT("Name"), FString(ObjectInfo->Name));
		Root->SetStringField(TEXT("ClassName"), ClassInfo ? FString(ClassInfo->Name) : FString());
	}

	return SerializeJsonObject(Root);
}

FString URewindDebuggerToolset::SelectObject(int64 ObjectId)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	IRewindDebugger* Debugger = IRewindDebugger::Instance();
	if (!Debugger)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "SelectObject: no IRewindDebugger instance");
		Root->SetStringField(TEXT("Error"), TEXT("Rewind Debugger is not available."));
		return SerializeJsonObject(Root);
	}

	const TraceServices::IAnalysisSession* Session = Debugger->GetAnalysisSession();
	if (!Session)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "SelectObject: no active analysis session");
		Root->SetStringField(TEXT("Error"), TEXT("No active analysis session."));
		return SerializeJsonObject(Root);
	}

	const IGameplayProvider* Provider = Session->ReadProvider<IGameplayProvider>(IGameplayProvider::ProviderName);
	if (!Provider)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "SelectObject: GameplayProvider not available");
		Root->SetStringField(TEXT("Error"), TEXT("GameplayProvider not available."));
		return SerializeJsonObject(Root);
	}

	const uint64 ObjectIdU64 = static_cast<uint64>(ObjectId);

	{
		TraceServices::FProviderReadScopeLock ProviderReadScope(*Provider);

		const FObjectInfo* ObjectInfo = Provider->FindObjectInfo(ObjectIdU64);
		if (!ObjectInfo)
		{
			Root->SetStringField(TEXT("Error"), FString::Printf(TEXT("No recorded object with id %lld."), ObjectId));
			return SerializeJsonObject(Root);
		}

		const FClassInfo* ClassInfo = Provider->FindClassInfo(ObjectInfo->ClassId);
		Root->SetNumberField(TEXT("ObjectId"), static_cast<double>(ObjectInfo->GetUObjectId()));
		Root->SetStringField(TEXT("Name"), FString(ObjectInfo->Name));
		Root->SetStringField(TEXT("ClassName"), ClassInfo ? FString(ClassInfo->Name) : FString());
	}

	// SelectTrack only walks already-debugged tracks; SetObjectToDebug pins the object first
	// (no-op if it's already in the debug list, otherwise replaces the debug set with just this one).
	Debugger->SetObjectToDebug(RewindDebugger::FObjectId(ObjectIdU64));
	Debugger->SelectTrack(RewindDebugger::FObjectId(ObjectIdU64));
	Debugger->OpenDetailsPanel();

	return SerializeJsonObject(Root);
}

FString URewindDebuggerToolset::SetScrubTime(double Time)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	if (!FMath::IsFinite(Time))
	{
		Root->SetStringField(TEXT("Error"), TEXT("Time must be a finite number (NaN/Inf rejected)."));
		return SerializeJsonObject(Root);
	}

	IRewindDebugger* Debugger = IRewindDebugger::Instance();
	if (!Debugger)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "SetScrubTime: no IRewindDebugger instance");
		Root->SetStringField(TEXT("Error"), TEXT("Rewind Debugger is not available."));
		return SerializeJsonObject(Root);
	}

	if (!Debugger->GetAnalysisSession())
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "SetScrubTime: no active analysis session");
		Root->SetStringField(TEXT("Error"), TEXT("No active analysis session."));
		return SerializeJsonObject(Root);
	}

	Debugger->SetScrubTime(Time);

	Root->SetNumberField(TEXT("ScrubTime"), Debugger->GetScrubTime());
	return SerializeJsonObject(Root);
}

FString URewindDebuggerToolset::CenterView(double Time)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	if (!FMath::IsFinite(Time))
	{
		Root->SetStringField(TEXT("Error"), TEXT("Time must be a finite number (NaN/Inf rejected)."));
		return SerializeJsonObject(Root);
	}

	IRewindDebugger* Debugger = IRewindDebugger::Instance();
	if (!Debugger)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "CenterView: no IRewindDebugger instance");
		Root->SetStringField(TEXT("Error"), TEXT("Rewind Debugger is not available."));
		return SerializeJsonObject(Root);
	}

	if (!Debugger->GetAnalysisSession())
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "CenterView: no active analysis session");
		Root->SetStringField(TEXT("Error"), TEXT("No active analysis session."));
		return SerializeJsonObject(Root);
	}

	Debugger->CenterViewOnTime(Time);

	const TRange<double>& ViewRange = Debugger->GetCurrentViewRange();
	Root->SetNumberField(TEXT("ViewRangeStart"), ViewRange.HasLowerBound() ? ViewRange.GetLowerBoundValue() : 0.0);
	Root->SetNumberField(TEXT("ViewRangeEnd"), ViewRange.HasUpperBound() ? ViewRange.GetUpperBoundValue() : 0.0);
	return SerializeJsonObject(Root);
}

FString URewindDebuggerToolset::ListObjectTraceEvents(
	int64 ObjectId,
	double StartTime,
	double EndTime,
	const FString& SourceFilter,
	const FString& IncludedNameFilter,
	const FString& ExcludedNameFilter,
	int32 Limit,
	int32 Offset)
{
	TArray<TSharedPtr<FJsonValue>> EmptyArray;

	IRewindDebugger* Debugger = IRewindDebugger::Instance();
	if (!Debugger)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "ListObjectTraceEvents: no IRewindDebugger instance");
		return SerializeJsonArray(EmptyArray);
	}

	const TraceServices::IAnalysisSession* Session = Debugger->GetAnalysisSession();
	if (!Session)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "ListObjectTraceEvents: no active analysis session");
		return SerializeJsonArray(EmptyArray);
	}

	const IGameplayProvider* Provider = Session->ReadProvider<IGameplayProvider>(IGameplayProvider::ProviderName);
	if (!Provider)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "ListObjectTraceEvents: GameplayProvider not available");
		return SerializeJsonArray(EmptyArray);
	}

	const uint64 ObjectIdU64 = static_cast<uint64>(ObjectId);
	const FName SourceFilterFName = SourceFilter.IsEmpty() ? FName() : FName(*SourceFilter);

	struct FRow
	{
		FName Source;
		FRewindDebuggerToolsetEventSummary Summary;
	};
	TArray<FRow> Rows;

	// NOTE: Provider read locks are the decoder's responsibility (see IRewindDebuggerToolsetEventDecoder).
	// The toolset holds no lock here because a typical decoder reads both its own typed trace provider
	// and GameplayProvider, and locking GameplayProvider here would force the decoder to skip the second.
	TArray<IRewindDebuggerToolsetEventDecoder*> Decoders = IModularFeatures::Get()
		.GetModularFeatureImplementations<IRewindDebuggerToolsetEventDecoder>(IRewindDebuggerToolsetEventDecoder::GetModularFeatureName());

	for (IRewindDebuggerToolsetEventDecoder* Decoder : Decoders)
	{
		if (!Decoder)
		{
			continue;
		}
		const FName SourceName = Decoder->GetSourceName();
		if (!SourceFilterFName.IsNone() && !SourceName.IsEqual(SourceFilterFName, ENameCase::IgnoreCase))
		{
			continue;
		}

		Decoder->EnumerateObjectEvents(*Session, *Provider, ObjectIdU64, StartTime, EndTime,
			[&](const FRewindDebuggerToolsetEventSummary& Summary)
			{
				if (!IncludedNameFilter.IsEmpty() && !Summary.EventName.Contains(IncludedNameFilter, ESearchCase::IgnoreCase))
				{
					return;
				}
				if (!ExcludedNameFilter.IsEmpty() && Summary.EventName.Contains(ExcludedNameFilter, ESearchCase::IgnoreCase))
				{
					return;
				}
				Rows.Add({ SourceName, Summary });
			});
	}

	Rows.Sort([](const FRow& A, const FRow& B) { return A.Summary.RecordingTime < B.Summary.RecordingTime; });

	const int32 ClampedOffset = FMath::Max(Offset, 0);
	const int32 StartIdx = FMath::Min(ClampedOffset, Rows.Num());
	const int32 EndIdx = (Limit <= 0) ? Rows.Num() : FMath::Min(StartIdx + Limit, Rows.Num());

	TArray<TSharedPtr<FJsonValue>> ResultArray;
	for (int32 Idx = StartIdx; Idx < EndIdx; ++Idx)
	{
		const FRow& Row = Rows[Idx];

		// Use the decoder's AdditionalFields as the base when present, then overlay identity fields.
		// Identity fields take precedence so decoders can't accidentally clobber them.
		TSharedRef<FJsonObject> Obj = Row.Summary.AdditionalFields.IsValid()
			? Row.Summary.AdditionalFields.ToSharedRef()
			: MakeShared<FJsonObject>();

		Obj->SetStringField(TEXT("Source"), Row.Source.ToString());
		Obj->SetStringField(TEXT("EventId"), LexToString(Row.Summary.EventId));
		Obj->SetStringField(TEXT("EventName"), Row.Summary.EventName);
		Obj->SetNumberField(TEXT("RecordingTime"), Row.Summary.RecordingTime);

		ResultArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return SerializeJsonArray(ResultArray);
}

FString URewindDebuggerToolset::GetTraceEventDetails(const FString& Source, const FString& EventIdString)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	if (Source.IsEmpty())
	{
		Root->SetStringField(TEXT("Error"), TEXT("Source must not be empty."));
		return SerializeJsonObject(Root);
	}
	if (EventIdString.IsEmpty())
	{
		Root->SetStringField(TEXT("Error"), TEXT("EventId must not be empty."));
		return SerializeJsonObject(Root);
	}

	// Strict parse: every character must be a decimal digit. FCString::Strtoui64 alone returns 0 on
	// garbage input, which collides with the legitimate EventId 0.
	for (const TCHAR Ch : EventIdString)
	{
		if (Ch < TEXT('0') || Ch > TEXT('9'))
		{
			Root->SetStringField(TEXT("Error"), FString::Printf(TEXT("EventId '%s' is not a non-negative decimal integer."), *EventIdString));
			return SerializeJsonObject(Root);
		}
	}
	const uint64 EventId = FCString::Strtoui64(*EventIdString, nullptr, 10);

	IRewindDebugger* Debugger = IRewindDebugger::Instance();
	if (!Debugger)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetTraceEventDetails: no IRewindDebugger instance");
		Root->SetStringField(TEXT("Error"), TEXT("Rewind Debugger is not available."));
		return SerializeJsonObject(Root);
	}

	const TraceServices::IAnalysisSession* Session = Debugger->GetAnalysisSession();
	if (!Session)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetTraceEventDetails: no active analysis session");
		Root->SetStringField(TEXT("Error"), TEXT("No active analysis session."));
		return SerializeJsonObject(Root);
	}

	const IGameplayProvider* Provider = Session->ReadProvider<IGameplayProvider>(IGameplayProvider::ProviderName);
	if (!Provider)
	{
		UE_LOGF(LogRewindDebuggerToolset, Log, "GetTraceEventDetails: GameplayProvider not available");
		Root->SetStringField(TEXT("Error"), TEXT("GameplayProvider not available."));
		return SerializeJsonObject(Root);
	}

	// NOTE: Provider read locks are the decoder's responsibility (see IRewindDebuggerToolsetEventDecoder).
	const FName SourceFName(*Source);
	TArray<IRewindDebuggerToolsetEventDecoder*> Decoders = IModularFeatures::Get()
		.GetModularFeatureImplementations<IRewindDebuggerToolsetEventDecoder>(IRewindDebuggerToolsetEventDecoder::GetModularFeatureName());

	for (IRewindDebuggerToolsetEventDecoder* Decoder : Decoders)
	{
		if (!Decoder)
		{
			continue;
		}
		if (!Decoder->GetSourceName().IsEqual(SourceFName, ENameCase::IgnoreCase))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Details = Decoder->DecodeEvent(*Session, *Provider, EventId);
		if (Details.IsValid())
		{
			return SerializeJsonObject(Details.ToSharedRef());
		}
	}

	Root->SetStringField(TEXT("Error"), FString::Printf(TEXT("No event with id '%s' found for source '%s'."), *EventIdString, *Source));
	return SerializeJsonObject(Root);
}

bool URewindDebuggerToolset::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}

