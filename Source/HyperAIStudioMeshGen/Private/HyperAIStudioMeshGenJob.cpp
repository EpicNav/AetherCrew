// Games by Hyper 2026.

#include "HyperAIStudioMeshGenJob.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioExternalService.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMeshGenJob, Log, All);

namespace HyperAIStudio::MeshGen::Private
{
	double& PollSeconds()
	{
		static double Seconds = 3.0;
		return Seconds;
	}

	FString Key(const FHyperAIStudioExternalServiceSettings& Service)
	{
		return Service.ApiKeyVariable.IsEmpty() ? FString() : FPlatformMisc::GetEnvironmentVariable(*Service.ApiKeyVariable).TrimStartAndEnd();
	}

	FString ProjectRelative(const FString& Path)
	{
		FString Relative = Path;
		FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		return Relative;
	}

	FString ModelsRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Models"));
	}

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

	FString FreeName(const FString& Folder, const FString& Name, const FJob* Self)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		auto Taken = [&](const FString& Candidate)
		{
			return Registry.HasAssets(FName(*(Folder / Candidate)), /*bRecursive=*/true)
				|| Jobs().ContainsByPredicate([&](const TSharedRef<FJob>& Job)
				{
					return &Job.Get() != Self && Job->Phase != EPhase::Failed && Job->AssetFolder == Folder && Job->AssetName == Candidate;
				});
		};
		FString Candidate = Name;
		for (int32 Suffix = 2; Taken(Candidate) && Suffix < 1000; ++Suffix)
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *Name, Suffix);
		}
		return Candidate;
	}

	TSharedPtr<FJsonObject> ParseJson(const TArray<uint8>& Body)
	{
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
		TSharedPtr<FJsonObject> Json;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get())), Json);
		return Json;
	}

	FString JsonText(const TSharedRef<FJsonObject>& Json)
	{
		FString Text;
		FJsonSerializer::Serialize(Json, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}

	TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	FString Field(const TSharedPtr<FJsonObject>& Json, const FString& Path)
	{
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."));
		TSharedPtr<FJsonObject> Object = Json;
		for (int32 Index = 0; Object.IsValid() && Index < Parts.Num(); ++Index)
		{
			if (Index == Parts.Num() - 1)
			{
				FString Value;
				double Number = 0.0;
				if (Object->TryGetStringField(Parts[Index], Value))
				{
					return Value;
				}
				return Object->TryGetNumberField(Parts[Index], Number) ? FString::Printf(TEXT("%g"), Number) : FString();
			}
			const TSharedPtr<FJsonObject>* Next = nullptr;
			Object = Object->TryGetObjectField(Parts[Index], Next) && Next ? *Next : nullptr;
		}
		return FString();
	}

	void FForm::Text(const FString& Name, const FString& Value)
	{
		Body.Append(Utf8(FString::Printf(TEXT("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n"), *Boundary, *Name, *Value)));
	}

	void FForm::File(const FString& Name, const FString& Path, const TArray<uint8>& Bytes)
	{
		const FString Extension = FPaths::GetExtension(Path).ToLower();
		const FString Mime = Extension == TEXT("png") ? TEXT("image/png") : Extension == TEXT("webp") ? TEXT("image/webp")
			: Extension == TEXT("glb") ? TEXT("model/gltf-binary") : Extension == TEXT("fbx") ? TEXT("application/octet-stream") : TEXT("image/jpeg");
		Body.Append(Utf8(FString::Printf(TEXT("--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: %s\r\n\r\n"),
			*Boundary, *Name, *FPaths::GetCleanFilename(Path), *Mime)));
		Body.Append(Bytes);
		Body.Append(Utf8(TEXT("\r\n")));
	}

	TArray<uint8> FForm::Finish()
	{
		Body.Append(Utf8(FString::Printf(TEXT("--%s--\r\n"), *Boundary)));
		return MoveTemp(Body);
	}

	FString FForm::ContentType() const
	{
		return TEXT("multipart/form-data; boundary=") + Boundary;
	}

	void WriteRecord(const FJob& Job)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("operationId"), Job.Info.OperationId);
		Json->SetStringField(TEXT("kind"), Job.Info.Kind);
		Json->SetStringField(TEXT("provider"), Job.Info.Provider);
		Json->SetStringField(TEXT("model"), Job.Info.Model);
		Json->SetStringField(TEXT("state"), Job.Info.State);
		Json->SetStringField(TEXT("prompt"), Job.Info.Prompt);
		Json->SetStringField(TEXT("imagePath"), Job.Info.ImagePath);
		Json->SetStringField(TEXT("taskId"), Job.Info.TaskId);
		TArray<TSharedPtr<FJsonValue>> Tasks;
		for (const FString& Task : Job.Info.Tasks)
		{
			Tasks.Add(MakeShared<FJsonValueString>(Task));
		}
		Json->SetArrayField(TEXT("tasks"), Tasks);
		Json->SetStringField(TEXT("assetPath"), Job.Info.AssetPath);
		Json->SetStringField(TEXT("diagnostic"), Job.Info.Diagnostic);
		if (Job.Info.Provider == TEXT("hi3d"))
		{
			Json->SetStringField(TEXT("attribution"), TEXT("Created with Hitem3D"));
		}
		const FString Folder = ModelsRoot() / Job.Info.OperationId;
		IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
		FFileHelper::SaveStringToFile(JsonText(Json), *(Folder / TEXT("job.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	void Fail(FJob& Job, const FString& Why)
	{
		Job.Phase = EPhase::Failed;
		Job.Info.State = TEXT("failed");
		Job.Info.Diagnostic = Why.Left(1024);
		WriteRecord(Job);
		UE_LOG(LogHyperAIStudioMeshGenJob, Warning, TEXT("3D job %s (%s) failed: %s"), *Job.Info.OperationId, *Job.Info.Provider, *Job.Info.Diagnostic);
	}

	void Send(const TSharedRef<FJob>& Job, const TCHAR* Verb, const FString& Url, const TMap<FString, FString>& Headers,
		TArray<uint8> Body, FOnReply OnReply, const float TimeoutSeconds)
	{
		const TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(Verb);
		Request->SetTimeout(TimeoutSeconds);
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

	TMap<FString, FString> KeyHeaders(const FJob& Job)
	{
		return {{Job.Service.ApiKeyHeader.IsEmpty() ? FString(TEXT("Authorization")) : Job.Service.ApiKeyHeader, Job.Service.ApiKeyPrefix + Key(Job.Service)}};
	}

	TMap<FString, FString> JsonHeaders(const FJob& Job)
	{
		TMap<FString, FString> Headers = KeyHeaders(Job);
		Headers.Add(TEXT("Content-Type"), TEXT("application/json"));
		return Headers;
	}

	FString ProviderError(const FJob& Job, const int32 Code, const TArray<uint8>& Body)
	{
		const TSharedPtr<FJsonObject> Json = ParseJson(Body);
		// Meshy and Tripo answer {message}, Hi3D {msg}; a failed task carries Meshy's task_error, Tripo's data.error_message
		// or Viggle's error.
		FString Message;
		for (const TCHAR* Path : {TEXT("message"), TEXT("msg"), TEXT("task_error.message"), TEXT("data.error_message"), TEXT("error.message")})
		{
			Message = Message.IsEmpty() ? Field(Json, Path) : Message;
		}
		const FString ProviderCode = Field(Json, TEXT("code"));
		const FString Credits = Code == 402 || ProviderCode == TEXT("30010000") || ProviderCode == TEXT("2010")
			? FString(TEXT(" The account is out of credits.")) : FString();
		return FString::Printf(TEXT("%s answered %d%s%s%s"), *Job.Service.Label, Code, Message.IsEmpty() ? TEXT("") : TEXT(": "), *Message.Left(300), *Credits);
	}

	FString AssetNameFor(const FString& Name, const FString& Prompt, const TCHAR* Prefix)
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
			Out = Out == Prefix ? FString(Prefix) + TEXT("Generated") : Out;
		}
		if (Out.IsEmpty() || !FChar::IsAlpha(Out[0]))
		{
			Out = Prefix + Out;
		}
		return Out.Left(64);
	}

	FHyperAIMeshJob Snapshot(const FJob& Job)
	{
		FHyperAIMeshJob Info = Job.Info;
		// A plan the user turned down never runs; the gate forgets it, so this is where that shows.
		if (Info.State == TEXT("awaiting_user_approval") && !FHyperAIStudioApprovalGate::HasPending(Info.OperationId))
		{
			Info.State = TEXT("rejected");
			Info.Diagnostic = TEXT("The user rejected it; nothing was spent.");
		}
		return Info;
	}

	void RecordFailure(const FJob& Job)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = Job.ToolName;
		Entry.Target = Job.AssetFolder / Job.AssetName;
		Entry.OperationId = Job.Info.OperationId;
		Entry.StatusCode = TEXT("failed");
		Entry.Detail = Job.Info.Diagnostic;
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	EHyperAIStudioAsyncJobPoll Poll(const TSharedRef<FJob>& Job, FString& OutProgress, FString& OutDiagnostic)
	{
		OutProgress = FString::Printf(TEXT("%s %d%%"), *Job->Info.State, Job->Info.Progress);
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
		Job->Kind == EKind::Generate ? StepGenerate(Job) : StepCharacter(Job);
		return EHyperAIStudioAsyncJobPoll::Running;
	}

	void Begin(const TSharedRef<FJob>& Job)
	{
		double Cost = 0.0;
		FString Error;
		if (!FHyperAIStudioExternalServices::Reserve(Job->Service.Id, 1, Cost, Error, Job->Price))
		{
			Fail(*Job, Error);
			RecordFailure(*Job);
			return;
		}
		Job->Info.State = TEXT("submitting");
		FHyperAIStudioAsyncJobRequest Request;
		Request.PackId = C::PackId;
		Request.ToolName = Job->ToolName;
		Request.Target = Job->AssetFolder / Job->AssetName;
		Request.OperationId = Job->Info.OperationId;
		Request.DeadlineMs = JobDeadlineMs;
		Request.PollIntervalMs = 200;
		Request.OnCancel = [Job]()
		{
			if (Job->Request.IsValid())
			{
				Job->Request->CancelRequest();
			}
			if (Job->Phase != EPhase::Done && Job->Phase != EPhase::Failed)
			{
				// What the provider already took keeps running there and stays booked: neither provider stops a running task.
				Fail(*Job, Job->bCancelRequested ? TEXT("Cancelled. A task the provider had already taken runs on there and stays booked.")
					: TEXT("Stopped before it finished."));
				Job->Info.State = Job->bCancelRequested ? TEXT("cancelled") : Job->Info.State;
				WriteRecord(*Job);
			}
		};
		if (!FHyperAIStudioAsyncJobHost::Start(Request, [Job](FString& OutProgress, FString& OutDiagnostic) { return Poll(Job, OutProgress, OutDiagnostic); }, Job->JobId, Error))
		{
			Fail(*Job, Error);
			RecordFailure(*Job);
		}
	}
}
