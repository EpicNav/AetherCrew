// Games by Hyper 2026.

#include "HyperAIStudioAudioGenToolset.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioAudioWrappers.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSourceControl.h"
#include "IAssetTools.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundWave.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAudioGen, Log, All);

namespace HyperAIStudio::AudioGen::Private
{
	using C = FHyperAIStudioAudioGenContracts;

	/** A take takes seconds to a minute or two; the job host bounds every job at ten minutes. */
	constexpr int32 JobDeadlineMs = 10 * 60 * 1000;
	constexpr int32 MaxAudioBytes = 100 * 1024 * 1024;
	const TCHAR* const ProviderOrder[] = {TEXT("elevenlabs"), TEXT("elevenlabs_music"), TEXT("stability")};

	double& PollSeconds()
	{
		static double Seconds = 2.0;
		return Seconds;
	}

	/** Read at the moment of the call, never kept in a setting, and never reported. */
	FString Key(const FHyperAIStudioExternalServiceSettings& Service)
	{
		return Service.ApiKeyVariable.IsEmpty() ? FString() : FPlatformMisc::GetEnvironmentVariable(*Service.ApiKeyVariable).TrimStartAndEnd();
	}

	FString DefaultModel(const FString& Provider)
	{
		return Provider == TEXT("elevenlabs") ? FString(TEXT("eleven_text_to_sound_v2"))
			: Provider == TEXT("elevenlabs_music") ? FString(TEXT("music_v1")) : FString(TEXT("stable-audio-2.5"));
	}

	bool IsKnownModel(const FString& Provider, const FString& Model)
	{
		if (Provider == TEXT("elevenlabs")) return Model == TEXT("eleven_text_to_sound_v2");
		if (Provider == TEXT("elevenlabs_music")) return Model == TEXT("music_v1") || Model == TEXT("music_v2") || Model == TEXT("music_v2_5");
		return Model == TEXT("stable-audio-2.5") || Model == TEXT("stable-audio-2") || Model == TEXT("stable-audio-3");
	}

	/** The lengths the provider documents for one clip; music is capped at five minutes so a job ends inside its deadline. */
	void LengthLimits(const FString& Provider, const FString& Model, float& OutMin, float& OutMax, bool& bOutAuto)
	{
		bOutAuto = Provider == TEXT("elevenlabs");
		if (Provider == TEXT("elevenlabs")) { OutMin = 0.5f; OutMax = 30.f; }
		else if (Provider == TEXT("elevenlabs_music")) { OutMin = 3.f; OutMax = 300.f; }
		else { OutMin = 1.f; OutMax = Model == TEXT("stable-audio-3") ? 380.f : 190.f; }
	}

	FString DocsUrl(const FString& Provider, const FString& Model)
	{
		if (Provider == TEXT("elevenlabs")) return TEXT("https://elevenlabs.io/docs/api-reference/text-to-sound-effects/convert");
		if (Provider == TEXT("elevenlabs_music")) return TEXT("https://elevenlabs.io/docs/api-reference/music/compose");
		return Model == TEXT("stable-audio-3")
			? TEXT("https://platform.stability.ai/docs/api-reference#tag/Stable-Audio/paths/~1v2beta~1audio~1stable-audio~1text-to-audio/post")
			: TEXT("https://platform.stability.ai/docs/api-reference#tag/Stable-Audio-2/paths/~1v2beta~1audio~1stable-audio-2~1text-to-audio/post");
	}

	TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	FString JsonText(const TSharedRef<FJsonObject>& Json)
	{
		FString Text;
		FJsonSerializer::Serialize(Json, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}

	/** A number as the request should read, 0.6 rather than the 0.59999999999999998 the JSON writer makes of a double. */
	void SetNumber(const TSharedRef<FJsonObject>& Json, const TCHAR* Name, const double Value)
	{
		Json->SetField(Name, MakeShared<FJsonValueNumberString>(FString::SanitizeFloat(FMath::RoundToDouble(Value * 1000.0) / 1000.0)));
	}

	TSharedPtr<FJsonObject> ParseJson(const TArray<uint8>& Body)
	{
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
		TSharedPtr<FJsonObject> Json;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get())), Json);
		return Json;
	}

	/** A multipart form of text fields, as Stability takes its parameters. */
	struct FForm
	{
		FString Boundary = TEXT("----HyperAIStudio") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		TArray<uint8> Body;

		void Text(const FString& Name, const FString& Value)
		{
			Body.Append(Utf8(FString::Printf(TEXT("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n"), *Boundary, *Name, *Value)));
		}
		TArray<uint8> Finish()
		{
			Body.Append(Utf8(FString::Printf(TEXT("--%s--\r\n"), *Boundary)));
			return MoveTemp(Body);
		}
	};

	FString AudioRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Audio"));
	}

	FString ProjectRelative(const FString& Path)
	{
		FString Relative = Path;
		FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		return Relative;
	}

	enum class EPhase : uint8
	{
		Submit,
		Poll,
		Import,
		Done,
		Failed
	};

	struct FJob
	{
		/** What hyper_audio_generate_status reports. */
		FHyperAIAudioJob Info;
		FHyperAIStudioExternalServiceSettings Service;
		FString Folder;
		FString Base;
		FString WrapperName;
		float Seconds = 0.f;
		float PromptInfluence = 0.3f;
		bool bLoop = false;
		bool bInstrumental = true;
		int32 Seed = 0;
		FVector2D Pitch = FVector2D::ZeroVector;
		FVector2D Volume = FVector2D(1.0, 1.0);
		double CostEach = 0.0;
		/** The take being made. */
		int32 Current = 0;
		/** Stability's id for an asynchronous generation. */
		FString PollId;
		EPhase Phase = EPhase::Submit;
		bool bWaiting = false;
		double NextPollAt = 0.0;
		TSharedPtr<IHttpRequest> Request;
	};

	TArray<TSharedRef<FJob>>& Jobs()
	{
		static TArray<TSharedRef<FJob>> Items;
		return Items;
	}

	TSharedPtr<FJob> FindJob(const FString& OperationId)
	{
		const TSharedRef<FJob>* Found = Jobs().FindByPredicate([&OperationId](const TSharedRef<FJob>& Job) { return Job->Info.OperationId == OperationId; });
		return Found ? TSharedPtr<FJob>(*Found) : nullptr;
	}

	FString TakeName(const FString& Base, const int32 Index)
	{
		return FString::Printf(TEXT("%s_%02d"), *Base, Index + 1);
	}

	/** MS_ or SC_ and the base without its own prefix: SFX_DoorSlam is played by MS_DoorSlam. */
	FString WrapperNameFor(const FString& Base, const FString& Wrap)
	{
		if (Wrap == TEXT("none"))
		{
			return FString();
		}
		FString Rest = Base;
		for (const TCHAR* Prefix : {TEXT("SFX_"), TEXT("MUS_"), TEXT("SW_"), TEXT("A_")})
		{
			if (Rest.StartsWith(Prefix, ESearchCase::IgnoreCase) && Rest.Len() > FCString::Strlen(Prefix))
			{
				Rest.RightChopInline(FCString::Strlen(Prefix));
				break;
			}
		}
		return (Wrap == TEXT("sound_cue_random") ? TEXT("SC_") : TEXT("MS_")) + Rest;
	}

	/** Base, or Base_2 and on: every take and the wrapper need packages of their own, also against jobs not imported yet. */
	FString FreeBase(const FString& Folder, const FString& Base, const int32 Variations, const FString& Wrap)
	{
		auto Taken = [&](const FString& Candidate)
		{
			TArray<FString> Names;
			for (int32 Index = 0; Index < Variations; ++Index)
			{
				Names.Add(TakeName(Candidate, Index));
			}
			const FString Wrapper = WrapperNameFor(Candidate, Wrap);
			if (!Wrapper.IsEmpty())
			{
				Names.Add(Wrapper);
			}
			for (const FString& Name : Names)
			{
				if (!FHyperAIStudioNames::IsFreePackage(Folder / Name))
				{
					return true;
				}
			}
			// A job still waiting to import holds its names; one the user turned down never will.
			return Jobs().ContainsByPredicate([&](const TSharedRef<FJob>& Job)
			{
				const bool bRejected = Job->Info.State == TEXT("awaiting_user_approval") && !FHyperAIStudioApprovalGate::HasPending(Job->Info.OperationId);
				return Job->Phase != EPhase::Failed && Job->Phase != EPhase::Done && !bRejected && Job->Folder == Folder && Job->Base == Candidate;
			});
		};
		FString Candidate = Base;
		for (int32 Suffix = 2; Taken(Candidate) && Suffix < 1000; ++Suffix)
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
		}
		return Candidate;
	}

	/** SFX_ (MUS_ for music) and the prompt's first words, or the name given, as an asset name. */
	FString BaseNameFor(const FString& Name, const FString& Prompt, const FString& Provider)
	{
		FString Out;
		if (!Name.TrimStartAndEnd().IsEmpty())
		{
			for (const TCHAR Char : Name.TrimStartAndEnd())
			{
				Out.AppendChar(FChar::IsAlnum(Char) || Char == TEXT('_') ? Char : TEXT('_'));
			}
		}
		else
		{
			const FString Prefix = Provider == TEXT("elevenlabs_music") ? TEXT("MUS_") : TEXT("SFX_");
			TArray<FString> Words;
			Prompt.ParseIntoArrayWS(Words);
			Out = Prefix;
			for (int32 Index = 0, Kept = 0; Index < Words.Num() && Kept < 4; ++Index)
			{
				FString Word;
				for (const TCHAR Char : Words[Index])
				{
					if (FChar::IsAlnum(Char))
					{
						Word.AppendChar(Char);
					}
				}
				if (Word.Len() > 2 || (Kept > 0 && !Word.IsEmpty()))
				{
					Out += Word.Left(1).ToUpper() + Word.Mid(1).ToLower();
					++Kept;
				}
			}
			Out = Out == Prefix ? Prefix + TEXT("Generated") : Out;
		}
		if (Out.IsEmpty() || !FChar::IsAlpha(Out[0]))
		{
			Out = TEXT("SFX_") + Out;
		}
		return Out.Left(56);
	}

	/** The job's record beside its takes. */
	void WriteRecord(const FJob& Job)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("operationId"), Job.Info.OperationId);
		Json->SetStringField(TEXT("provider"), Job.Info.Provider);
		Json->SetStringField(TEXT("model"), Job.Info.Model);
		Json->SetStringField(TEXT("state"), Job.Info.State);
		Json->SetStringField(TEXT("prompt"), Job.Info.Prompt);
		Json->SetStringField(TEXT("wrapper"), Job.Info.WrapperPath);
		Json->SetNumberField(TEXT("cost"), Job.Info.Cost);
		Json->SetStringField(TEXT("diagnostic"), Job.Info.Diagnostic);
		TArray<TSharedPtr<FJsonValue>> Takes;
		for (const FHyperAIAudioVariation& Take : Job.Info.Variations)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Take.Name);
			Entry->SetNumberField(TEXT("seconds"), Take.Seconds);
			Entry->SetNumberField(TEXT("cost"), Take.Cost);
			Entry->SetStringField(TEXT("file"), Take.File);
			Entry->SetStringField(TEXT("asset"), Take.AssetPath);
			Takes.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Json->SetArrayField(TEXT("takes"), Takes);
		const FString Folder = AudioRoot() / Job.Info.OperationId;
		IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
		FFileHelper::SaveStringToFile(JsonText(Json), *(Folder / TEXT("job.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	void Fail(FJob& Job, const FString& Why)
	{
		Job.Phase = EPhase::Failed;
		Job.Info.State = TEXT("failed");
		Job.Info.Diagnostic = Why.Left(1024);
		WriteRecord(Job);
		UE_LOG(LogHyperAIStudioAudioGen, Warning, TEXT("Audio job %s (%s) failed: %s"), *Job.Info.OperationId, *Job.Info.Provider, *Job.Info.Diagnostic);
	}

	/** What a provider said went wrong, in its own words where it gave any. */
	FString ProviderError(const FJob& Job, const int32 Code, const TArray<uint8>& Body)
	{
		const TSharedPtr<FJsonObject> Json = ParseJson(Body);
		FString Message;
		if (Json.IsValid())
		{
			// ElevenLabs: {"detail": {"message"}} or a validation list; Stability: {"errors": [...]} or {"message"}.
			const TSharedPtr<FJsonObject>* Detail = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
			if (Json->TryGetObjectField(TEXT("detail"), Detail) && Detail)
			{
				(*Detail)->TryGetStringField(TEXT("message"), Message);
			}
			else if (Json->TryGetArrayField(TEXT("detail"), List) && List && !List->IsEmpty() && (*List)[0]->AsObject().IsValid())
			{
				(*List)[0]->AsObject()->TryGetStringField(TEXT("msg"), Message);
			}
			else if (Json->TryGetArrayField(TEXT("errors"), List) && List && !List->IsEmpty())
			{
				Message = (*List)[0]->AsString();
			}
			if (Message.IsEmpty())
			{
				Json->TryGetStringField(TEXT("message"), Message);
			}
		}
		const FString Credits = Code == 402 ? FString(TEXT(" The account is out of credits.")) : Code == 401 ? FString(TEXT(" The key was refused.")) : FString();
		return FString::Printf(TEXT("%s answered %d%s%s%s"), *Job.Service.Label, Code, Message.IsEmpty() ? TEXT("") : TEXT(": "), *Message.Left(300), *Credits);
	}

	using FOnReply = TFunction<void(FJob& Job, int32 Code, const TArray<uint8>& Body)>;

	/** One request for the job; OnReply runs on the game thread, and not at all once the job has failed or stopped. */
	void Send(const TSharedRef<FJob>& Job, const FString& Verb, const FString& Url, const TMap<FString, FString>& Headers, TArray<uint8> Body, FOnReply OnReply)
	{
		const TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(Verb);
		// Music of minutes takes minutes to compose.
		Request->SetTimeout(Job->Info.Provider == TEXT("elevenlabs_music") ? 360.f : 180.f);
		for (const TPair<FString, FString>& Header : Headers)
		{
			Request->SetHeader(Header.Key, Header.Value);
		}
		if (!Body.IsEmpty())
		{
			Request->SetContent(MoveTemp(Body));
		}
		Job->bWaiting = true;
		Job->Request = Request;
		Request->OnProcessRequestComplete().BindLambda([Job, OnReply = MoveTemp(OnReply)](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			Job->bWaiting = false;
			Job->Request.Reset();
			if (Job->Phase == EPhase::Failed || Job->Phase == EPhase::Done)
			{
				return;
			}
			if (!bConnected || !Response.IsValid())
			{
				Fail(*Job, FString::Printf(TEXT("%s did not answer."), *Job->Service.Label));
				return;
			}
			OnReply(*Job, Response->GetResponseCode(), Response->GetContent());
		});
		Request->ProcessRequest();
	}

	TMap<FString, FString> KeyHeaders(const FJob& Job, const TMap<FString, FString>& Headers)
	{
		TMap<FString, FString> Out = Headers;
		Out.Add(Job.Service.ApiKeyHeader.IsEmpty() ? FString(TEXT("Authorization")) : Job.Service.ApiKeyHeader, Job.Service.ApiKeyPrefix + Key(Job.Service));
		return Out;
	}

	/** The take arrived: kept under Saved/HyperAIStudio/Audio, booked, and the next one asked for. */
	void Keep(FJob& Job, const TArray<uint8>& Body)
	{
		const FString Extension = C::SniffFormat(Body);
		FHyperAIAudioVariation& Take = Job.Info.Variations[Job.Current];
		if (Extension.IsEmpty() || Body.Num() > MaxAudioBytes)
		{
			Fail(Job, FString::Printf(TEXT("%s sent %d bytes that are not WAV, MP3, OGG or FLAC audio."), *Job.Service.Label, Body.Num()));
			return;
		}
		const FString File = AudioRoot() / Job.Info.OperationId / Take.Name + TEXT(".") + Extension;
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
		if (!FFileHelper::SaveArrayToFile(Body, *File))
		{
			Fail(Job, TEXT("The audio could not be written under Saved/HyperAIStudio/Audio."));
			return;
		}
		Take.File = ProjectRelative(File);
		++Job.Current;
		++Job.Info.Done;
		Job.PollId.Reset();
		Job.Phase = Job.Current < Job.Info.Variations.Num() ? EPhase::Submit : EPhase::Import;
		Job.Info.State = Job.Phase == EPhase::Import ? TEXT("importing") : TEXT("generating");
		WriteRecord(Job);
	}

	void Book(FJob& Job)
	{
		FHyperAIStudioExternalServices::RecordSpend(Job.Service.Id, 1, Job.CostEach);
		Job.Info.Cost += static_cast<float>(Job.CostEach);
	}

	void Submit(const TSharedRef<FJob>& Job)
	{
		const int32 Seed = Job->Seed > 0 ? Job->Seed + Job->Current : 0;
		const FHyperAIAudioGenCall Call = C::BuildCall(Job->Info.Provider, Job->Info.Model, Job->Info.Prompt, Job->Seconds, Job->PromptInfluence,
			Job->bLoop, Job->bInstrumental, Seed);
		TMap<FString, FString> Headers = Call.Headers;
		Headers.Add(TEXT("Content-Type"), Call.ContentType);
		const bool bAsync = Call.bAsync;
		Send(Job, Call.Verb, Job->Service.BaseUrl + Call.Path, KeyHeaders(*Job, Headers), Call.Body, [bAsync](FJob& Job, const int32 Code, const TArray<uint8>& Body)
		{
			if (bAsync)
			{
				const TSharedPtr<FJsonObject> Json = ParseJson(Body);
				FString Id;
				if ((Code == 200 || Code == 202) && Json.IsValid() && Json->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
				{
					// Accepted: from here it is paid for, should it finish.
					Book(Job);
					Job.PollId = Id;
					Job.Phase = EPhase::Poll;
					Job.NextPollAt = FPlatformTime::Seconds() + PollSeconds();
					return;
				}
				Fail(Job, ProviderError(Job, Code, Body));
				return;
			}
			if (Code == 200 && !C::SniffFormat(Body).IsEmpty())
			{
				Book(Job);
				Keep(Job, Body);
				return;
			}
			Fail(Job, ProviderError(Job, Code, Body));
		});
	}

	/** Stability's asynchronous generation: 202 while it runs, 200 with the audio when done. */
	void Query(const TSharedRef<FJob>& Job)
	{
		const FString Url = Job->Service.BaseUrl + TEXT("/v2beta/audio/results/") + Job->PollId;
		Send(Job, TEXT("GET"), Url, KeyHeaders(*Job, {{TEXT("Accept"), TEXT("audio/*")}}), {}, [](FJob& Job, const int32 Code, const TArray<uint8>& Body)
		{
			if (Code == 202)
			{
				Job.NextPollAt = FPlatformTime::Seconds() + PollSeconds();
				return;
			}
			if (Code == 200 && !C::SniffFormat(Body).IsEmpty())
			{
				Keep(Job, Body);
				return;
			}
			Fail(Job, ProviderError(Job, Code, Body));
		});
	}

	/** The import and the wrapper are editor operations of their own, run on a tick of their own rather than inside the job's quick poll. */
	void Import(const TSharedRef<FJob>& Job)
	{
		Job->bWaiting = true;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Job](float)
		{
			Job->bWaiting = false;
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
			TArray<USoundWave*> Waves;
			for (FHyperAIAudioVariation& Take : Job->Info.Variations)
			{
				UAssetImportTask* Task = NewObject<UAssetImportTask>();
				Task->Filename = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Take.File);
				Task->DestinationPath = Job->Folder;
				Task->DestinationName = Take.Name;
				Task->bAutomated = true;
				Task->bReplaceExisting = false;
				Task->bSave = false;
				AssetTools.ImportAssetTasks({Task});
				USoundWave* Wave = nullptr;
				for (UObject* Object : Task->GetObjects())
				{
					Wave = Wave ? Wave : Cast<USoundWave>(Object);
				}
				if (!Wave)
				{
					Fail(*Job, FString::Printf(TEXT("%s did not import as a SoundWave."), *Take.File));
					return false;
				}
				// The real length, which an automatic one only now tells; read before looping, which reports a looping wave as endless.
				Take.Seconds = Wave->GetDuration();
				Wave->bLooping = Job->bLoop || Job->Info.Wrap == TEXT("metasound_loop");
				FString Error;
				if (!FHyperAIStudioSourceControl::SaveAsset(*Wave, Error))
				{
					Fail(*Job, Error);
					return false;
				}
				Take.AssetPath = Wave->GetPathName();
				Waves.Add(Wave);
			}
			const FString& Wrap = Job->Info.Wrap;
			if (Wrap != TEXT("none"))
			{
				Job->Info.State = TEXT("wrapping");
				FString Error;
				const FString Package = Job->Folder / Job->WrapperName;
				Job->Info.WrapperPath = Wrap == TEXT("sound_cue_random")
					? FHyperAIStudioAudioWrappers::MakeRandomCue(Package, Waves, Job->Pitch, Job->Volume, Job->bLoop, Error)
					: FHyperAIStudioAudioWrappers::MakeMetaSoundPlayer(Package, Waves,
						Wrap == TEXT("metasound_random") ? TEXT("random") : Wrap == TEXT("metasound_loop") ? TEXT("loop") : TEXT("one_shot"), Job->Pitch, Job->Volume, Error);
				if (Job->Info.WrapperPath.IsEmpty())
				{
					Fail(*Job, FString::Printf(TEXT("The takes were imported, but the %s wrapper failed: %s"), *Wrap, *Error));
					return false;
				}
			}
			Job->Phase = EPhase::Done;
			Job->Info.State = TEXT("completed");
			Job->Info.Diagnostic = FString::Printf(TEXT("%d take(s) imported into %s%s, for $%.3f."), Waves.Num(), *Job->Folder,
				Job->Info.WrapperPath.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" and played by %s"), *Job->Info.WrapperPath), Job->Info.Cost);
			WriteRecord(*Job);
			UE_LOG(LogHyperAIStudioAudioGen, Display, TEXT("Audio job %s: %s"), *Job->Info.OperationId, *Job->Info.Diagnostic);
			return false;
		}));
	}

	EHyperAIStudioAsyncJobPoll Poll(const TSharedRef<FJob>& Job, FString& OutProgress, FString& OutDiagnostic)
	{
		OutProgress = FString::Printf(TEXT("%s %d/%d"), *Job->Info.State, Job->Info.Done, Job->Info.Variations.Num());
		OutDiagnostic = Job->Info.Diagnostic;
		if (Job->Phase == EPhase::Failed)
		{
			return EHyperAIStudioAsyncJobPoll::Failed;
		}
		if (Job->Phase == EPhase::Done)
		{
			return EHyperAIStudioAsyncJobPoll::Completed;
		}
		if (Job->bWaiting || FPlatformTime::Seconds() < Job->NextPollAt)
		{
			return EHyperAIStudioAsyncJobPoll::Running;
		}
		switch (Job->Phase)
		{
		case EPhase::Submit: Submit(Job); break;
		case EPhase::Poll: Query(Job); break;
		case EPhase::Import: Import(Job); break;
		default: break;
		}
		return EHyperAIStudioAsyncJobPoll::Running;
	}

	void RecordFailure(const FJob& Job)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::GenerateToolName;
		Entry.Target = Job.Folder / Job.Base;
		Entry.OperationId = Job.Info.OperationId;
		Entry.StatusCode = TEXT("failed");
		Entry.Detail = Job.Info.Diagnostic;
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	/** Approved: the spend is checked against the cap again, then the job runs in the background, where Activity shows it. */
	void Begin(const TSharedRef<FJob>& Job)
	{
		double Cost = 0.0;
		FString Error;
		if (!FHyperAIStudioExternalServices::Reserve(Job->Service.Id, Job->Info.Variations.Num(), Cost, Error, Job->CostEach))
		{
			Fail(*Job, Error);
			RecordFailure(*Job);
			return;
		}
		Job->Info.State = TEXT("generating");
		FHyperAIStudioAsyncJobRequest Request;
		Request.PackId = C::PackId;
		Request.ToolName = C::GenerateToolName;
		Request.Target = Job->Folder / Job->Base;
		Request.OperationId = Job->Info.OperationId;
		Request.DeadlineMs = JobDeadlineMs;
		Request.PollIntervalMs = 200;
		Request.OnCancel = [Job]()
		{
			if (Job->Request.IsValid())
			{
				Job->Request->CancelRequest();
			}
			if (Job->Phase != EPhase::Done)
			{
				Fail(*Job, TEXT("Stopped before it finished."));
			}
		};
		FString JobId;
		if (!FHyperAIStudioAsyncJobHost::Start(Request, [Job](FString& OutProgress, FString& OutDiagnostic) { return Poll(Job, OutProgress, OutDiagnostic); }, JobId, Error))
		{
			Fail(*Job, Error);
			RecordFailure(*Job);
		}
	}

	FHyperAIAudioJob Snapshot(const FJob& Job)
	{
		FHyperAIAudioJob Info = Job.Info;
		// A job the user turned down never runs; the gate forgets it, so this is where that shows.
		if (Info.State == TEXT("awaiting_user_approval") && !FHyperAIStudioApprovalGate::HasPending(Info.OperationId))
		{
			Info.State = TEXT("rejected");
			Info.Diagnostic = TEXT("The user rejected it; nothing was spent.");
		}
		return Info;
	}
}

FString FHyperAIStudioAudioGenContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioAudioGen.HyperAIStudioAudioGenToolset");
}

TArray<FString> FHyperAIStudioAudioGenContracts::GetToolNames()
{
	return {GenerateToolName, StatusToolName};
}

bool FHyperAIStudioAudioGenContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioAudioGenContracts::SetPollSecondsForTests(const double Seconds)
{
	HyperAIStudio::AudioGen::Private::PollSeconds() = Seconds > 0.0 ? Seconds : 2.0;
}
#endif

FHyperAIAudioGenCall FHyperAIStudioAudioGenContracts::BuildCall(const FString& Provider, const FString& Model, const FString& Prompt, const float Seconds,
	const float PromptInfluence, const bool bLoop, const bool bInstrumental, const int32 Seed)
{
	using namespace HyperAIStudio::AudioGen::Private;
	FHyperAIAudioGenCall Call;
	Call.Verb = TEXT("POST");
	if (Provider == TEXT("elevenlabs") || Provider == TEXT("elevenlabs_music"))
	{
		// MP3 at 44.1 kHz is open to every ElevenLabs plan (PCM at 44.1 kHz needs Pro), and Unreal imports it.
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		if (Provider == TEXT("elevenlabs"))
		{
			Call.Path = TEXT("/v1/sound-generation?output_format=mp3_44100_128");
			Json->SetStringField(TEXT("text"), Prompt);
			Json->SetStringField(TEXT("model_id"), Model);
			SetNumber(Json, TEXT("prompt_influence"), PromptInfluence);
			Json->SetBoolField(TEXT("loop"), bLoop);
			if (Seconds > 0.f)
			{
				SetNumber(Json, TEXT("duration_seconds"), Seconds);
			}
		}
		else
		{
			Call.Path = TEXT("/v1/music?output_format=mp3_44100_128");
			Json->SetStringField(TEXT("prompt"), Prompt);
			Json->SetStringField(TEXT("model_id"), Model);
			Json->SetField(TEXT("music_length_ms"), MakeShared<FJsonValueNumberString>(FString::FromInt(FMath::RoundToInt(Seconds * 1000.f))));
			Json->SetBoolField(TEXT("force_instrumental"), bInstrumental);
		}
		Call.ContentType = TEXT("application/json");
		Call.Headers.Add(TEXT("Accept"), TEXT("audio/mpeg"));
		Call.Body = Utf8(JsonText(Json));
		return Call;
	}
	// Stability: a multipart form, the audio back as bytes; WAV, which loops cleanly where MP3 pads its ends.
	FForm Form;
	Form.Text(TEXT("prompt"), Prompt);
	Form.Text(TEXT("duration"), FString::SanitizeFloat(Seconds));
	Form.Text(TEXT("model"), Model);
	Form.Text(TEXT("output_format"), TEXT("wav"));
	if (Seed > 0)
	{
		Form.Text(TEXT("seed"), FString::FromInt(Seed));
	}
	Call.bAsync = Model == TEXT("stable-audio-3");
	Call.Path = Call.bAsync ? TEXT("/v2beta/audio/stable-audio/text-to-audio") : TEXT("/v2beta/audio/stable-audio-2/text-to-audio");
	Call.ContentType = TEXT("multipart/form-data; boundary=") + Form.Boundary;
	Call.Headers.Add(TEXT("Accept"), TEXT("audio/*"));
	Call.Body = Form.Finish();
	return Call;
}

double FHyperAIStudioAudioGenContracts::PriceOf(const FString& Provider, const FString& Model, const float Seconds, const double ServicePrice)
{
	// ElevenLabs bills a minute of audio; a sound effect whose length it picks is priced at the longest it makes.
	if (Provider == TEXT("elevenlabs"))
	{
		return ServicePrice * (Seconds > 0.f ? Seconds : 30.f) / 60.0;
	}
	if (Provider == TEXT("elevenlabs_music"))
	{
		return ServicePrice * Seconds / 60.0;
	}
	// Stability bills credits per generation: 8 for Stable Audio 2.5 and 3, 20 for Stable Audio 2 at its default steps.
	return Model == TEXT("stable-audio-2") ? ServicePrice * 20.0 / 8.0 : ServicePrice;
}

FString FHyperAIStudioAudioGenContracts::SniffFormat(const TArray<uint8>& Bytes)
{
	if (Bytes.Num() < 12)
	{
		return FString();
	}
	const uint8* Data = Bytes.GetData();
	if (FMemory::Memcmp(Data, "RIFF", 4) == 0 && FMemory::Memcmp(Data + 8, "WAVE", 4) == 0) return TEXT("wav");
	if (FMemory::Memcmp(Data, "ID3", 3) == 0 || (Data[0] == 0xFF && (Data[1] & 0xE0) == 0xE0)) return TEXT("mp3");
	if (FMemory::Memcmp(Data, "OggS", 4) == 0) return TEXT("ogg");
	if (FMemory::Memcmp(Data, "fLaC", 4) == 0) return TEXT("flac");
	return FString();
}

FHyperAIAudioGenerateReport FHyperAIStudioAudioGenContracts::Generate(const FHyperAIAudioGenerateRequest& Request)
{
	using namespace HyperAIStudio::AudioGen::Private;
	FHyperAIAudioGenerateReport Report;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	auto Refuse = [&Report](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Refuse(TEXT("game_thread_required"), TEXT("Audio generation runs on the game thread."));
	}
	FString FirstWithKey;
	for (const TCHAR* Id : ProviderOrder)
	{
		FHyperAIStudioExternalServiceSettings Service;
		const bool bKey = FHyperAIStudioExternalServices::Find(Id, Service) && !Key(Service).IsEmpty();
		Report.Providers.Add(FString::Printf(TEXT("%s: %s %s"), Id, *Service.ApiKeyVariable, bKey ? TEXT("is set") : TEXT("is not set")));
		FirstWithKey = FirstWithKey.IsEmpty() && bKey ? FString(Id) : FirstWithKey;
	}
	Report.Provider = Request.Provider.TrimStartAndEnd().ToLower();
	Report.Provider = Report.Provider.IsEmpty() ? FirstWithKey : Report.Provider;
	if (Report.Provider.IsEmpty())
	{
		return Refuse(TEXT("missing_key"), TEXT("No audio provider has a key: set ELEVENLABS_API_KEY or STABILITY_API_KEY for your user, then restart the editor."));
	}
	bool bKnownProvider = false;
	for (const TCHAR* Id : ProviderOrder)
	{
		bKnownProvider |= Report.Provider == Id;
	}
	FHyperAIStudioExternalServiceSettings Service;
	if (!bKnownProvider || !FHyperAIStudioExternalServices::Find(Report.Provider, Service))
	{
		return Refuse(TEXT("unknown_provider"), TEXT("Provider is elevenlabs (sound effects), elevenlabs_music or stability (Stable Audio)."));
	}
	if (Key(Service).IsEmpty())
	{
		return Refuse(TEXT("missing_key"), FString::Printf(TEXT("%s's key is not set: set %s for your user, then restart the editor."), *Service.Label, *Service.ApiKeyVariable));
	}
	Report.Model = Request.Model.TrimStartAndEnd().IsEmpty() ? DefaultModel(Report.Provider) : Request.Model.TrimStartAndEnd();
	if (!IsKnownModel(Report.Provider, Report.Model))
	{
		return Refuse(TEXT("invalid_request"), Report.Provider == TEXT("stability") ? TEXT("Model is stable-audio-2.5, stable-audio-2 or stable-audio-3.")
			: Report.Provider == TEXT("elevenlabs_music") ? TEXT("Model is music_v1, music_v2 or music_v2_5.") : TEXT("Model is eleven_text_to_sound_v2."));
	}
	Report.DocsUrl = DocsUrl(Report.Provider, Report.Model);
	const FString Prompt = Request.Prompt.TrimStartAndEnd();
	if (Prompt.IsEmpty() || Prompt.Len() > MaxPromptCharacters)
	{
		return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("Say what to make in Prompt, in up to %d characters."), MaxPromptCharacters));
	}
	if (Request.Variations < 1 || Request.Variations > MaxVariations)
	{
		return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("Variations is 1 to %d."), MaxVariations));
	}
	float MinSeconds = 0.f;
	float MaxSeconds = 0.f;
	bool bAuto = false;
	LengthLimits(Report.Provider, Report.Model, MinSeconds, MaxSeconds, bAuto);
	const float Seconds = Request.DurationSeconds;
	if ((Seconds == 0.f && !bAuto) || (Seconds != 0.f && (Seconds < MinSeconds || Seconds > MaxSeconds)))
	{
		return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("DurationSeconds is %g to %g for %s%s."), MinSeconds, MaxSeconds, *Service.Label,
			bAuto ? TEXT(", or 0 to let it pick (priced as 30 s)") : TEXT("")));
	}
	if (Request.PromptInfluence > 1.f)
	{
		return Refuse(TEXT("invalid_request"), TEXT("PromptInfluence is 0 to 1."));
	}
	if (Request.bLoop && Report.Provider != TEXT("elevenlabs"))
	{
		return Refuse(TEXT("invalid_request"), TEXT("bLoop asks ElevenLabs sound effects for a seamless loop; for music or Stable Audio, wrap the take with metasound_loop instead."));
	}
	Report.Wrap = Request.Wrap.TrimStartAndEnd().ToLower();
	if (Report.Wrap.IsEmpty())
	{
		Report.Wrap = Request.Variations > 1 ? TEXT("metasound_random") : Request.bLoop ? TEXT("metasound_loop") : TEXT("metasound_one_shot");
	}
	if (Report.Wrap != TEXT("none") && Report.Wrap != TEXT("metasound_one_shot") && Report.Wrap != TEXT("metasound_loop")
		&& Report.Wrap != TEXT("metasound_random") && Report.Wrap != TEXT("sound_cue_random"))
	{
		return Refuse(TEXT("invalid_request"), TEXT("Wrap is metasound_one_shot, metasound_loop, metasound_random, sound_cue_random or none."));
	}
	if (!(Request.PitchSemitonesMin >= -12.f && Request.PitchSemitonesMin <= Request.PitchSemitonesMax && Request.PitchSemitonesMax <= 12.f)
		|| !(Request.VolumeMin >= 0.f && Request.VolumeMin <= Request.VolumeMax && Request.VolumeMax <= 4.f))
	{
		return Refuse(TEXT("invalid_request"), TEXT("PitchSemitonesMin and Max are -12 to 12, VolumeMin and Max 0 to 4, each min no higher than its max."));
	}
	FString Folder = Request.Folder.TrimStartAndEnd().IsEmpty() ? GetDefault<UHyperAIStudioSettings>()->AudioImportFolder : Request.Folder.TrimStartAndEnd();
	Folder.RemoveFromEnd(TEXT("/"));
	FString Reason;
	if ((!Folder.StartsWith(TEXT("/Game/")) && Folder != TEXT("/Game")) || !FHyperAIStudioNames::IsFolderPath(Folder, TEXT("folder"), &Reason))
	{
		return Refuse(TEXT("invalid_request"), TEXT("Folder is a content path under /Game, such as /Game/Generated/Audio.") + (Reason.IsEmpty() ? FString() : TEXT(" ") + Reason));
	}
	const FString Base = FreeBase(Folder, BaseNameFor(Request.Name, Prompt, Report.Provider), Request.Variations, Report.Wrap);
	const FString WrapperName = WrapperNameFor(Base, Report.Wrap);
	if (!FHyperAIStudioNames::IsPackagePath(Folder / TakeName(Base, Request.Variations - 1), TEXT("The folder and name together"), &Reason))
	{
		return Refuse(TEXT("invalid_request"), Reason + TEXT(" Give a shorter folder or name."));
	}
	Report.Destination = Folder;
	Report.WrapperPath = WrapperName.IsEmpty() ? FString() : Folder / WrapperName;
	const double CostEach = PriceOf(Report.Provider, Report.Model, Seconds, Service.CostPerCall);
	for (int32 Index = 0; Index < Request.Variations; ++Index)
	{
		FHyperAIAudioVariation& Take = Report.Variations.AddDefaulted_GetRef();
		Take.Name = TakeName(Base, Index);
		Take.Seconds = Seconds > 0.f ? Seconds : 30.f;
		Take.Cost = static_cast<float>(CostEach);
	}
	Report.EstimatedCost = static_cast<float>(CostEach * Request.Variations);
	Report.OperationId = Request.OperationId.IsEmpty() ? TEXT("audio-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower() : Request.OperationId;
	if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Report.OperationId))
	{
		return Refuse(TEXT("invalid_operation_id"), TEXT("operation_id must be a short journal-safe id, or empty to have one made."));
	}
	if (FindJob(Report.OperationId).IsValid())
	{
		return Refuse(TEXT("operation_id_used"), TEXT("That operation_id already names an audio job; pass a new one or leave it empty."));
	}
	const FString Length = Seconds > 0.f ? FString::Printf(TEXT("%g s"), Seconds) : FString(TEXT("up to 30 s (ElevenLabs picks)"));
	const FString What = FString::Printf(TEXT("%d take(s) of %s from %s (%s): \"%s\""), Request.Variations, *Length, *Service.Label, *Report.Model, *Prompt.Left(120));
	const FString Wrapped = Report.Wrap == TEXT("none") ? FString(TEXT("no wrapper")) : FString::Printf(TEXT("%s as %s"), *Report.WrapperPath, *Report.Wrap);
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("Would make %s, at $%.3f a take and $%.3f in all of this session's cap, import them into %s with %s. Nothing was spent; ")
			TEXT("resubmit with bDryRun false to ask the user."), *What, CostEach, Report.EstimatedCost, *Folder, *Wrapped);
		return Report;
	}
	double Cost = 0.0;
	FString Error;
	if (!FHyperAIStudioExternalServices::Reserve(Service.Id, Request.Variations, Cost, Error, CostEach))
	{
		return Refuse(TEXT("over_spend_cap"), Error);
	}

	const TSharedRef<FJob> Job = MakeShared<FJob>();
	Job->Service = Service;
	Job->Folder = Folder;
	Job->Base = Base;
	Job->WrapperName = WrapperName;
	Job->Seconds = Seconds;
	Job->PromptInfluence = Request.PromptInfluence < 0.f ? 0.3f : Request.PromptInfluence;
	Job->bLoop = Request.bLoop;
	Job->bInstrumental = Request.bInstrumental;
	Job->Seed = Request.Seed;
	Job->Pitch = FVector2D(Request.PitchSemitonesMin, Request.PitchSemitonesMax);
	Job->Volume = FVector2D(Request.VolumeMin, Request.VolumeMax);
	Job->CostEach = CostEach;
	Job->Info.OperationId = Report.OperationId;
	Job->Info.Provider = Report.Provider;
	Job->Info.Model = Report.Model;
	Job->Info.State = TEXT("awaiting_user_approval");
	Job->Info.Prompt = Prompt;
	Job->Info.Variations = Report.Variations;
	Job->Info.Wrap = Report.Wrap;
	FHyperAIStudioApprovalSummary Summary;
	Summary.PackId = PackId;
	Summary.ToolName = GenerateToolName;
	Summary.VariantId = TEXT("audio.generate.v1");
	Summary.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
	Summary.EffectTarget = Folder / Base;
	Summary.Effects = {TEXT("Make ") + What,
		FString::Printf(TEXT("Pay %s at most $%.3f a take, $%.3f in all, against this session's spend cap"), *Service.Label, CostEach, Report.EstimatedCost),
		FString::Printf(TEXT("Import them into %s as %s_01 and on, with %s"), *Folder, *Base, *Wrapped)};
	for (const FHyperAIAudioVariation& Take : Report.Variations)
	{
		Summary.Touches.Add(Folder / Take.Name);
	}
	if (!Report.WrapperPath.IsEmpty())
	{
		Summary.Touches.Add(Report.WrapperPath);
	}
	if (!FHyperAIStudioApprovalGate::RequestAction(Report.OperationId, Summary, [Job]() { Begin(Job); }, Error))
	{
		return Refuse(TEXT("approval_not_queued"), Error);
	}
	Jobs().Insert(Job, 0);
	Report.bOk = true;
	Report.Status = TEXT("awaiting_user_approval");
	Report.Diagnostic = FString::Printf(TEXT("Waiting for the user's approval in HyperAI Chat, Activity; nothing was spent yet. Then it makes %s and imports them into %s ")
		TEXT("with %s: call hyper_operation_wait with operation_id and bWaitThroughApproval true, then hyper_audio_generate_status."), *What, *Folder, *Wrapped);
	FHyperAIStudioApprovalGate::DescribeHeld(Report.OperationId, Report.Status, Report.Diagnostic);
	return Report;
}

FHyperAIAudioStatusReport FHyperAIStudioAudioGenContracts::Status(const FString& OperationId)
{
	using namespace HyperAIStudio::AudioGen::Private;
	FHyperAIAudioStatusReport Report;
	for (const TSharedRef<FJob>& Job : Jobs())
	{
		if (OperationId.IsEmpty() || Job->Info.OperationId == OperationId)
		{
			Report.Jobs.Add(Snapshot(*Job));
		}
	}
	Report.bOk = OperationId.IsEmpty() || !Report.Jobs.IsEmpty();
	Report.Diagnostic = Report.bOk ? FString::Printf(TEXT("%d job(s) this session."), Report.Jobs.Num())
		: TEXT("No audio job has that operation_id in this editor session.");
	return Report;
}

FHyperAIAudioGenerateReport UHyperAIStudioAudioGenToolset::hyper_audio_generate(const FHyperAIAudioGenerateRequest& Request)
{
	return FHyperAIStudioAudioGenContracts::Generate(Request);
}

FHyperAIAudioStatusReport UHyperAIStudioAudioGenToolset::hyper_audio_generate_status(const FHyperAIAudioStatusRequest& Request)
{
	return FHyperAIStudioAudioGenContracts::Status(Request.OperationId);
}

void FHyperAIStudioAudioGenRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioAudioGenRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioAudioGenRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioAudioGenToolset::StaticClass(),
			FHyperAIStudioAudioGenContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioAudioGen, Warning, TEXT("Could not unregister the audio generation toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioAudioGenRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioAudioGenContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioAudioGen, Verbose, TEXT("Audio generation source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioAudioGenToolset::StaticClass(),
		FHyperAIStudioAudioGenContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioAudioGen, Error, TEXT("Audio generation cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}
