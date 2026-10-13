// Games by Hyper 2026.

#include "HyperAIStudioMeshGenToolset.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Containers/Ticker.h"
#include "Engine/StaticMesh.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioMeshGenJob.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSettings.h"
#include "IAssetTools.h"
#include "Misc/Base64.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMeshGen, Log, All);

namespace HyperAIStudio::MeshGen::Private
{
	const TCHAR* const ProviderOrder[] = {TEXT("tripo"), TEXT("meshy"), TEXT("hi3d")};

	FString DefaultModel(const FString& Provider)
	{
		return Provider == TEXT("tripo") ? FString(TEXT("v3.1-20260211")) : Provider == TEXT("meshy") ? FString(TEXT("latest")) : FString(TEXT("hi3dv3.0"));
	}

	TArray<uint8> ImageBytes(const FJob& Job)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Job.ImageFile);
		return Bytes;
	}

	/** The provider took the task: from here it is paid for. */
	void Accepted(FJob& Job, const FString& TaskId)
	{
		const bool bFirst = Job.Info.TaskId.IsEmpty();
		Job.Info.TaskId = TaskId;
		Job.Info.State = TEXT("generating");
		Job.Phase = EPhase::Generate;
		Job.NextPollAt = FPlatformTime::Seconds() + PollSeconds();
		if (bFirst)
		{
			FHyperAIStudioExternalServices::RecordSpend(Job.Service.Id, 1);
		}
		WriteRecord(Job);
	}

	void Prepare(const TSharedRef<FJob>& Job)
	{
		const FString Base = Job->Service.BaseUrl;
		if (Job->Info.Provider == TEXT("hi3d"))
		{
			// Hi3D's key is client_id:client_secret, traded for an access token.
			const TMap<FString, FString> Headers = {{TEXT("Authorization"), TEXT("Basic ") + FBase64::Encode(Key(Job->Service))},
				{TEXT("Content-Type"), TEXT("application/json")}};
			Send(Job, TEXT("POST"), Base + TEXT("/open-api/v1/auth/token"), Headers, Utf8(TEXT("{}")), [](FJob& Job, const int32 Code, const TArray<uint8>& Body)
			{
				Job.Token = Field(ParseJson(Body), TEXT("data.accessToken"));
				if (Code != 200 || Job.Token.IsEmpty())
				{
					Fail(Job, ProviderError(Job, Code, Body) + TEXT(" HITEM3D_API_KEY holds client_id:client_secret."));
					return;
				}
				Job.Phase = EPhase::Submit;
			});
			return;
		}
		if (Job->Info.Provider == TEXT("tripo") && !Job->ImageFile.IsEmpty())
		{
			FForm Form;
			Form.File(TEXT("file"), Job->ImageFile, ImageBytes(*Job));
			TMap<FString, FString> Headers = KeyHeaders(*Job);
			Headers.Add(TEXT("Content-Type"), Form.ContentType());
			Send(Job, TEXT("POST"), Base + TEXT("/files"), Headers, Form.Finish(), [](FJob& Job, const int32 Code, const TArray<uint8>& Body)
			{
				Job.Token = Field(ParseJson(Body), TEXT("data.file_token"));
				if (Code != 200 || Job.Token.IsEmpty())
				{
					Fail(Job, ProviderError(Job, Code, Body));
					return;
				}
				Job.Phase = EPhase::Submit;
			});
			return;
		}
		Job->Phase = EPhase::Submit;
	}

	void Submit(const TSharedRef<FJob>& Job)
	{
		const FString Base = Job->Service.BaseUrl;
		const FString& Provider = Job->Info.Provider;
		const bool bPicture = !Job->ImageFile.IsEmpty();
		auto OnTask = [](const TCHAR* TaskPath)
		{
			return [TaskPath = FString(TaskPath)](FJob& Job, const int32 Code, const TArray<uint8>& Body)
			{
				const TSharedPtr<FJsonObject> Json = ParseJson(Body);
				const FString TaskId = Field(Json, TaskPath);
				const FString ProviderCode = Field(Json, TEXT("code"));
				// Tripo answers code 0 and Hi3D code 200 inside a 200; Meshy answers 200 or 202.
				const bool bOk = (Code == 200 || Code == 202) && !TaskId.IsEmpty() && (ProviderCode.IsEmpty() || ProviderCode == TEXT("0") || ProviderCode == TEXT("200"));
				bOk ? Accepted(Job, TaskId) : Fail(Job, ProviderError(Job, Code, Body));
			};
		};
		if (Provider == TEXT("tripo"))
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("model"), Job->Info.Model);
			bPicture ? Json->SetStringField(TEXT("input"), Job->Token) : Json->SetStringField(TEXT("prompt"), Job->Info.Prompt);
			Send(Job, TEXT("POST"), Base + (bPicture ? TEXT("/generation/image-to-model") : TEXT("/generation/text-to-model")), JsonHeaders(*Job),
				Utf8(JsonText(Json)), OnTask(TEXT("data.task_id")));
		}
		else if (Provider == TEXT("meshy"))
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("ai_model"), Job->Info.Model);
			if (Job->bRefined)
			{
				// The mesh is made; now its texture, from the same task.
				Json->SetStringField(TEXT("mode"), TEXT("refine"));
				Json->SetStringField(TEXT("preview_task_id"), Job->Info.TaskId);
				Json->SetBoolField(TEXT("enable_pbr"), true);
			}
			else if (bPicture)
			{
				const FString Extension = FPaths::GetExtension(Job->ImageFile).ToLower();
				Json->SetStringField(TEXT("image_url"), FString::Printf(TEXT("data:image/%s;base64,%s"),
					Extension == TEXT("png") ? TEXT("png") : TEXT("jpeg"), *FBase64::Encode(ImageBytes(*Job))));
				Json->SetBoolField(TEXT("should_texture"), true);
				Json->SetBoolField(TEXT("enable_pbr"), true);
			}
			else
			{
				Json->SetStringField(TEXT("mode"), TEXT("preview"));
				Json->SetStringField(TEXT("prompt"), Job->Info.Prompt);
			}
			Send(Job, TEXT("POST"), Base + (bPicture ? TEXT("/openapi/v1/image-to-3d") : TEXT("/openapi/v2/text-to-3d")), JsonHeaders(*Job),
				Utf8(JsonText(Json)), OnTask(TEXT("result")));
		}
		else
		{
			FForm Form;
			Form.Text(TEXT("request_type"), TEXT("3"));
			Form.Text(TEXT("model"), Job->Info.Model);
			Form.Text(TEXT("format"), TEXT("2"));
			Form.File(TEXT("images"), Job->ImageFile, ImageBytes(*Job));
			const TMap<FString, FString> Headers = {{TEXT("Authorization"), TEXT("Bearer ") + Job->Token}, {TEXT("Content-Type"), Form.ContentType()}};
			Send(Job, TEXT("POST"), Base + TEXT("/open-api/v1/submit-task"), Headers, Form.Finish(), OnTask(TEXT("data.task_id")));
		}
	}

	/** Asks how the task is going; a finished one gives the model's address. */
	void Query(const TSharedRef<FJob>& Job)
	{
		const FString Base = Job->Service.BaseUrl;
		const FString& Provider = Job->Info.Provider;
		const bool bPicture = !Job->ImageFile.IsEmpty();
		const FString Url = Provider == TEXT("tripo") ? Base + TEXT("/tasks/") + Job->Info.TaskId
			: Provider == TEXT("meshy") ? Base + (bPicture ? TEXT("/openapi/v1/image-to-3d/") : TEXT("/openapi/v2/text-to-3d/")) + Job->Info.TaskId
			: Base + TEXT("/open-api/v1/query-task?task_id=") + FGenericPlatformHttp::UrlEncode(Job->Info.TaskId);
		const TMap<FString, FString> Headers = Provider == TEXT("hi3d") ? TMap<FString, FString>{{TEXT("Authorization"), TEXT("Bearer ") + Job->Token}} : KeyHeaders(*Job);
		Send(Job, TEXT("GET"), Url, Headers, {}, [](FJob& Job, const int32 Code, const TArray<uint8>& Body)
		{
			const TSharedPtr<FJsonObject> Json = ParseJson(Body);
			if (Code != 200 || !Json.IsValid())
			{
				Fail(Job, ProviderError(Job, Code, Body));
				return;
			}
			const FString& Provider = Job.Info.Provider;
			const FString State = (Provider == TEXT("tripo") ? Field(Json, TEXT("data.status"))
				: Provider == TEXT("meshy") ? Field(Json, TEXT("status")) : Field(Json, TEXT("data.state"))).ToLower();
			const FString Progress = Provider == TEXT("tripo") ? Field(Json, TEXT("data.progress")) : Field(Json, TEXT("progress"));
			Job.Info.Progress = Progress.IsEmpty() ? (State == TEXT("processing") ? 50 : Job.Info.Progress) : FMath::Clamp(FCString::Atoi(*Progress), 0, 100);
			if (State == TEXT("success") || State == TEXT("succeeded"))
			{
				if (Provider == TEXT("meshy") && Job.ImageFile.IsEmpty() && !Job.bRefined)
				{
					Job.bRefined = true;
					Job.Info.Progress = 50;
					Job.Phase = EPhase::Submit;
					return;
				}
				Job.ModelUrl = Provider == TEXT("tripo") ? Field(Json, TEXT("data.output.model_url"))
					: Provider == TEXT("meshy") ? Field(Json, TEXT("model_urls.glb")) : Field(Json, TEXT("data.url"));
				if (Job.ModelUrl.IsEmpty())
				{
					Fail(Job, FString::Printf(TEXT("%s finished the task without a model to download."), *Job.Service.Label));
					return;
				}
				Job.Info.State = TEXT("downloading");
				Job.Info.Progress = 100;
				Job.Phase = EPhase::Download;
				return;
			}
			if (State == TEXT("failed") || State == TEXT("banned") || State == TEXT("expired") || State == TEXT("cancelled") || State == TEXT("canceled"))
			{
				Fail(Job, FString::Printf(TEXT("%s's task %s: %s"), *Job.Service.Label, *State, *ProviderError(Job, Code, Body)));
				return;
			}
			Job.NextPollAt = FPlatformTime::Seconds() + PollSeconds();
		}, 60.f);
	}

	void Download(const TSharedRef<FJob>& Job)
	{
		Send(Job, TEXT("GET"), Job->ModelUrl, {}, {}, [](FJob& Job, const int32 Code, const TArray<uint8>& Body)
		{
			// A GLB starts glTF; the links last minutes, so the file is fetched as soon as the task is done.
			if (Code != 200 || Body.Num() < 12 || FMemory::Memcmp(Body.GetData(), "glTF", 4) != 0 || Body.Num() > MaxModelBytes)
			{
				Fail(Job, FString::Printf(TEXT("The model from %s could not be downloaded as a GLB (HTTP %d, %d bytes)."), *Job.Service.Label, Code, Body.Num()));
				return;
			}
			// Named after the asset: Unreal imports a model into a folder named after its file, taken only now.
			Job.AssetName = FreeName(Job.AssetFolder, Job.AssetName, &Job);
			const FString File = ModelsRoot() / Job.Info.OperationId / Job.AssetName + TEXT(".glb");
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
			if (!FFileHelper::SaveArrayToFile(Body, *File))
			{
				Fail(Job, TEXT("The model could not be written under Saved/HyperAIStudio/Models."));
				return;
			}
			Job.Info.ModelFile = ProjectRelative(File);
			Job.Info.State = TEXT("importing");
			Job.Phase = EPhase::Import;
		}, 300.f);
	}

	/** The import is an editor operation of its own, run on a tick of its own rather than inside the job's quick poll. */
	void Import(const TSharedRef<FJob>& Job)
	{
		Job->bWaiting = true;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Job](float)
		{
			Job->bWaiting = false;
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Job->Info.ModelFile);
			// The model, its materials and its textures land in a folder of its own, named after it.
			Task->DestinationPath = Job->AssetFolder;
			Task->DestinationName = Job->AssetName;
			Task->bAutomated = true;
			Task->bReplaceExisting = false;
			Task->bSave = true;
			AssetTools.ImportAssetTasks({Task});
			Job->Info.ImportedAssets = Task->ImportedObjectPaths;
			for (const FString& Path : Task->ImportedObjectPaths)
			{
				if (Cast<UStaticMesh>(StaticLoadObject(UObject::StaticClass(), nullptr, *Path)))
				{
					Job->Info.AssetPath = Path;
					break;
				}
			}
			if (Job->Info.AssetPath.IsEmpty())
			{
				Fail(*Job, FString::Printf(TEXT("Unreal imported %d assets from the model but no static mesh; the file is %s."),
					Task->ImportedObjectPaths.Num(), *Job->Info.ModelFile));
				return false;
			}
			Job->Phase = EPhase::Done;
			Job->Info.State = TEXT("completed");
			Job->Info.Diagnostic = FString::Printf(TEXT("Imported as %s, with %d assets in all; the model file is %s.%s"), *Job->Info.AssetPath,
				Job->Info.ImportedAssets.Num(), *Job->Info.ModelFile,
				Job->Info.Provider == TEXT("hi3d") ? TEXT(" Hi3D asks for credit where it is shown: Created with Hitem3D.") : TEXT(""));
			WriteRecord(*Job);
			UE_LOG(LogHyperAIStudioMeshGen, Display, TEXT("3D job %s: %s"), *Job->Info.OperationId, *Job->Info.Diagnostic);
			return false;
		}));
	}

	void StepGenerate(const TSharedRef<FJob>& Job)
	{
		switch (Job->Phase)
		{
		case EPhase::Prepare: Prepare(Job); break;
		case EPhase::Submit: Submit(Job); break;
		case EPhase::Generate: Query(Job); break;
		case EPhase::Download: Download(Job); break;
		case EPhase::Import: Import(Job); break;
		default: break;
		}
	}
}

FString FHyperAIStudioMeshGenContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioMeshGen.HyperAIStudioMeshGenToolset");
}

TArray<FString> FHyperAIStudioMeshGenContracts::GetToolNames()
{
	return {GenerateToolName, StatusToolName, RigToolName, AnimateToolName, RetextureToolName, ActionsToolName, CancelToolName};
}

bool FHyperAIStudioMeshGenContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioMeshGenContracts::SetPollSecondsForTests(const double Seconds)
{
	HyperAIStudio::MeshGen::Private::PollSeconds() = Seconds > 0.0 ? Seconds : 3.0;
}
#endif

FHyperAIMeshGenerateReport FHyperAIStudioMeshGenContracts::Generate(const FHyperAIMeshGenerateRequest& Request)
{
	using namespace HyperAIStudio::MeshGen::Private;
	FHyperAIMeshGenerateReport Report;
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
		return Refuse(TEXT("game_thread_required"), TEXT("3D generation runs on the game thread."));
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
	FHyperAIStudioExternalServiceSettings Service;
	if (Report.Provider.IsEmpty())
	{
		return Refuse(TEXT("missing_key"), TEXT("No 3D provider has a key: set TRIPO_API_KEY, MESHY_API_KEY or HITEM3D_API_KEY (client_id:client_secret) for your user, then restart the editor."));
	}
	bool bKnownProvider = false;
	for (const TCHAR* Id : ProviderOrder)
	{
		bKnownProvider |= Report.Provider == Id;
	}
	if (!bKnownProvider || !FHyperAIStudioExternalServices::Find(Report.Provider, Service))
	{
		return Refuse(TEXT("unknown_provider"), TEXT("provider is tripo, meshy or hi3d."));
	}
	if (Key(Service).IsEmpty())
	{
		return Refuse(TEXT("missing_key"), FString::Printf(TEXT("%s's key is not set: set %s for your user%s, then restart the editor."), *Service.Label,
			*Service.ApiKeyVariable, Report.Provider == TEXT("hi3d") ? TEXT(" as client_id:client_secret") : TEXT("")));
	}

	const FString Prompt = Request.Prompt.TrimStartAndEnd();
	FString Image;
	if (!Request.ImagePath.TrimStartAndEnd().IsEmpty())
	{
		const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		Image = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Request.ImagePath) ? ProjectDir / Request.ImagePath : Request.ImagePath);
		const FString Extension = FPaths::GetExtension(Image).ToLower();
		const bool bKnown = Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg") || (Extension == TEXT("webp") && Report.Provider == TEXT("hi3d"));
		const int64 Size = IFileManager::Get().FileSize(*Image);
		if (!FPaths::IsUnderDirectory(Image, ProjectDir) || Size <= 0 || Size > MaxImageBytes || !bKnown)
		{
			return Refuse(TEXT("invalid_image"), TEXT("image_path is a PNG or JPEG inside the project (Hi3D takes WebP too), up to 20 MB."));
		}
	}
	if (Prompt.IsEmpty() && Image.IsEmpty())
	{
		return Refuse(TEXT("invalid_request"), TEXT("Say what to make in prompt, or give a picture in image_path."));
	}
	if (Report.Provider == TEXT("hi3d") && Image.IsEmpty())
	{
		return Refuse(TEXT("needs_picture"), TEXT("Hi3D makes models only from a picture: pass image_path, or use tripo or meshy for words."));
	}
	if (Prompt.Len() > MaxPromptCharacters)
	{
		return Refuse(TEXT("invalid_request"), TEXT("prompt is 800 characters at most."));
	}
	Report.Model = Request.Model.TrimStartAndEnd().IsEmpty() ? DefaultModel(Report.Provider) : Request.Model.TrimStartAndEnd();
	if (Report.Model.Len() > 64 || Report.Model.Contains(TEXT("\"")) || Report.Model.Contains(TEXT("\r")) || Report.Model.Contains(TEXT("\n")))
	{
		return Refuse(TEXT("invalid_request"), TEXT("model is the provider's model id, such as v3.1-20260211, latest or hi3dv3.0."));
	}
	FString Folder = Request.Folder.TrimStartAndEnd().IsEmpty() ? GetDefault<UHyperAIStudioSettings>()->MeshImportFolder : Request.Folder.TrimStartAndEnd();
	Folder.RemoveFromEnd(TEXT("/"));
	FString FolderReason;
	if ((!Folder.StartsWith(TEXT("/Game/")) && Folder != TEXT("/Game")) || !FHyperAIStudioNames::IsFolderPath(Folder, TEXT("folder"), &FolderReason))
	{
		return Refuse(TEXT("invalid_request"), TEXT("folder is a content path under /Game, such as /Game/Generated/Models.")
			+ (FolderReason.IsEmpty() ? FString() : TEXT(" ") + FolderReason));
	}
	const FString Name = FreeName(Folder, AssetNameFor(Request.Name, Prompt));
	// Imported as Folder/Name/Name: the package that makes must be one, past its paid job.
	if (!FHyperAIStudioNames::IsPackagePath(Folder / Name / Name, TEXT("The folder and name together"), &FolderReason))
	{
		return Refuse(TEXT("invalid_request"), FolderReason + TEXT(" Give a shorter folder or name."));
	}
	Report.Destination = Folder / Name;
	Report.Source = Image.IsEmpty() ? TEXT("words") : TEXT("picture");
	Report.EstimatedCost = Service.CostPerCall;
	Report.OperationId = Request.OperationId.IsEmpty() ? TEXT("mesh-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower() : Request.OperationId;
	if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Report.OperationId))
	{
		return Refuse(TEXT("invalid_operation_id"), TEXT("operation_id must be a short journal-safe id, or empty to have one made."));
	}
	if (FindJob(Report.OperationId).IsValid())
	{
		return Refuse(TEXT("operation_id_used"), TEXT("That operation_id already names a 3D job; pass a new one or leave it empty."));
	}
	const FString What = FString::Printf(TEXT("a textured model with %s (%s) from %s"), *Service.Label, *Report.Model,
		Image.IsEmpty() ? *FString::Printf(TEXT("words: \"%s\""), *Prompt.Left(120)) : *FString::Printf(TEXT("the picture %s"), *ProjectRelative(Image)));
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("Would make %s, for at most $%.2f of this session's cap, and import it into %s. Nothing was spent; ")
			TEXT("resubmit with bDryRun false to ask the user."), *What, Report.EstimatedCost, *Report.Destination);
		return Report;
	}
	double Cost = 0.0;
	FString Error;
	if (!FHyperAIStudioExternalServices::Reserve(Service.Id, 1, Cost, Error))
	{
		return Refuse(TEXT("over_spend_cap"), Error);
	}

	const TSharedRef<FJob> Job = MakeShared<FJob>();
	Job->Info.Kind = TEXT("generate");
	Job->Service = Service;
	Job->ImageFile = Image;
	Job->AssetFolder = Folder;
	Job->AssetName = Name;
	Job->Info.OperationId = Report.OperationId;
	Job->Info.Provider = Report.Provider;
	Job->Info.Model = Report.Model;
	Job->Info.State = TEXT("awaiting_user_approval");
	Job->Info.Prompt = Prompt;
	Job->Info.ImagePath = Image.IsEmpty() ? FString() : ProjectRelative(Image);
	Job->Info.Cost = Report.EstimatedCost;
	FHyperAIStudioApprovalSummary Summary;
	Summary.PackId = PackId;
	Summary.ToolName = GenerateToolName;
	Summary.VariantId = TEXT("mesh.generate.v1");
	Summary.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
	Summary.EffectTarget = Report.Destination;
	Summary.Effects = {TEXT("Make ") + What,
		FString::Printf(TEXT("Pay %s at most $%.2f, against this session's spend cap"), *Service.Label, Report.EstimatedCost),
		TEXT("Import it into ") + Report.Destination};
	Summary.Touches = {Report.Destination};
	if (!FHyperAIStudioApprovalGate::RequestAction(Report.OperationId, Summary, [Job]() { Begin(Job); }, Error))
	{
		return Refuse(TEXT("approval_not_queued"), Error);
	}
	Jobs().Insert(Job, 0);
	Report.bOk = true;
	Report.Status = TEXT("awaiting_user_approval");
	Report.Diagnostic = FString::Printf(TEXT("Waiting for the user's approval in HyperAI Chat, Activity; nothing was spent yet. Then it makes %s and imports it into %s, ")
		TEXT("in one to five minutes: call hyper_operation_wait with operation_id and bWaitThroughApproval true, then hyper_mesh_status."), *What, *Report.Destination);
	FHyperAIStudioApprovalGate::DescribeHeld(Report.OperationId, Report.Status, Report.Diagnostic);
	return Report;
}

FHyperAIMeshStatusReport FHyperAIStudioMeshGenContracts::Status(const FString& OperationId)
{
	using namespace HyperAIStudio::MeshGen::Private;
	FHyperAIMeshStatusReport Report;
	for (const TSharedRef<FJob>& Job : Jobs())
	{
		if (OperationId.IsEmpty() || Job->Info.OperationId == OperationId)
		{
			Report.Jobs.Add(Snapshot(*Job));
		}
	}
	Report.bOk = OperationId.IsEmpty() || !Report.Jobs.IsEmpty();
	Report.Diagnostic = Report.bOk ? FString::Printf(TEXT("%d job(s) this session."), Report.Jobs.Num())
		: TEXT("No 3D job has that operation_id in this editor session.");
	return Report;
}

FHyperAIMeshGenerateReport UHyperAIStudioMeshGenToolset::hyper_mesh_generate(const FHyperAIMeshGenerateRequest& Request)
{
	return FHyperAIStudioMeshGenContracts::Generate(Request);
}

FHyperAIMeshStatusReport UHyperAIStudioMeshGenToolset::hyper_mesh_status(const FHyperAIMeshStatusRequest& Request)
{
	return FHyperAIStudioMeshGenContracts::Status(Request.OperationId);
}

FHyperAIMeshTaskReport UHyperAIStudioMeshGenToolset::hyper_mesh_rig(const FHyperAIMeshRigRequest& Request)
{
	return FHyperAIStudioMeshGenContracts::Rig(Request);
}

FHyperAIMeshTaskReport UHyperAIStudioMeshGenToolset::hyper_mesh_animate(const FHyperAIMeshAnimateRequest& Request)
{
	return FHyperAIStudioMeshGenContracts::Animate(Request);
}

FHyperAIMeshTaskReport UHyperAIStudioMeshGenToolset::hyper_mesh_retexture(const FHyperAIMeshRetextureRequest& Request)
{
	return FHyperAIStudioMeshGenContracts::Retexture(Request);
}

FHyperAIMeshActionsReport UHyperAIStudioMeshGenToolset::hyper_mesh_actions(const FHyperAIMeshActionsRequest& Request)
{
	return FHyperAIStudioMeshGenContracts::Actions(Request);
}

FHyperAIMeshCancelReport UHyperAIStudioMeshGenToolset::hyper_mesh_cancel(const FHyperAIMeshCancelRequest& Request)
{
	return FHyperAIStudioMeshGenContracts::Cancel(Request.OperationId);
}

void FHyperAIStudioMeshGenRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioMeshGenRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioMeshGenRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioMeshGenToolset::StaticClass(),
			FHyperAIStudioMeshGenContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioMeshGen, Warning, TEXT("Could not unregister the 3D generation toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioMeshGenRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioMeshGenContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioMeshGen, Verbose, TEXT("3D generation source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioMeshGenToolset::StaticClass(),
		FHyperAIStudioMeshGenContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioMeshGen, Error, TEXT("3D generation cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}
