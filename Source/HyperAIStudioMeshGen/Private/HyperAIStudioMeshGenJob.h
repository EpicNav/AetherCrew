// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioMeshGenToolset.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IHttpRequest.h"
#include "Misc/Guid.h"

/**
 * What every 3D job shares, whatever it makes: its record, its requests to the provider, its spend against the session's
 * cap, and its place in the Activity panel. A model from words or a picture runs its own steps
 * (HyperAIStudioMeshGenToolset.cpp); rigging, animating and texturing run a list of provider tasks
 * (HyperAIStudioMeshGenCharacters.cpp).
 */
namespace HyperAIStudio::MeshGen::Private
{
	using C = FHyperAIStudioMeshGenContracts;

	/** A provider task takes one to five minutes; the job host bounds every job at ten. */
	constexpr int32 JobDeadlineMs = 10 * 60 * 1000;
	constexpr int32 MaxModelBytes = 200 * 1024 * 1024;

	/** How often a running job asks its provider; tests shorten it. */
	double& PollSeconds();
	/** Read at the moment of the call, never kept in a setting, and never reported. */
	FString Key(const FHyperAIStudioExternalServiceSettings& Service);
	FString ProjectRelative(const FString& Path);
	FString ModelsRoot();

	enum class EPhase : uint8
	{
		Prepare,
		/** A mesh from the project, written as a GLB to send. */
		Export,
		/** Tripo: that file uploaded, for a file token. */
		Upload,
		/** Meshy: action names found in its library. */
		Lookup,
		Submit,
		Generate,
		Download,
		Import,
		/** The retarget plan handed to hyper_retarget_apply_plan. */
		Handoff,
		Done,
		Failed
	};

	enum class EKind : uint8
	{
		Generate,
		Rig,
		Animate,
		Retexture,
		/** A model the creator makes or fixes: provider tasks whose last result is a static (or rigged) mesh to import. */
		Model
	};

	struct FJob;

	/** One task a provider runs for a job: how to start it, how to ask after it, and what to take from it once it is done. */
	struct FTask
	{
		/** What it is, in the job's progress and its list of tasks: rig, animation, text to motion... */
		FString Label;
		/** POSTed with Body. Empty for a task that only reads what the one before it started, as Viggle's export does. */
		FString CreateUrl;
		TFunction<TSharedRef<FJsonObject>(const FJob&)> Body;
		/** Where the reply to the POST names the task: result, data.task_id or id. */
		FString TaskIdPath;
		/** Asked with GET; {id} is the task. */
		FString QueryUrl;
		FString StatePath;
		FString ProgressPath;
		/** Booked against the session's cap when the provider takes the task. */
		double Cost = 0.0;
		/** Reads the finished task: the files to download, the ids later tasks need. False, with a reason, fails the job. */
		TFunction<bool(FJob&, const TSharedPtr<FJsonObject>&, FString&)> OnDone;
		FString TaskId;
	};

	/** A file the job downloads, only from an address the provider's result gave. */
	struct FDownload
	{
		FString Url;
		/** model (imported as it is) or animation (imported onto the rig's skeleton). */
		FString Role;
		/** Absolute, once downloaded. */
		FString File;
	};

	struct FJob
	{
		/** What hyper_mesh_status reports. */
		FHyperAIMeshJob Info;
		FHyperAIStudioExternalServiceSettings Service;
		EKind Kind = EKind::Generate;
		FString ToolName = C::GenerateToolName;
		/** Absolute, when the model is made from a picture, or a picture gives the texture its look. */
		FString ImageFile;
		FString AssetFolder;
		FString AssetName;
		EPhase Phase = EPhase::Prepare;
		bool bWaiting = false;
		double NextPollAt = 0.0;
		/** Hi3D's access token, or the file token Tripo gave an uploaded file. */
		FString Token;
		/** Meshy makes a textured model from words in two tasks: the mesh, then its texture. */
		bool bRefined = false;
		FString ModelUrl;
		TSharedPtr<IHttpRequest> Request;
		/** What the whole job may cost, checked against the cap when approved; negative for the service's own price. */
		double Price = -1.0;
		/** The job host's id, to cancel it, and whether hyper_mesh_cancel asked to. */
		FString JobId;
		bool bCancelRequested = false;

		// Rigging, animating and texturing.
		TArray<FTask> Tasks;
		int32 TaskIndex = 0;
		TArray<FDownload> Downloads;
		int32 DownloadIndex = 0;
		/** The provider's own task the job works from: a model it made, to rig or texture. */
		FString SourceTaskId;
		/** A project mesh sent to the provider: written to ExportFile first, then sent inline (Meshy) or uploaded (Tripo). */
		FString ExportMesh;
		FString ExportFile;
		bool bUpload = false;
		/** Meshy: action names to find in its library first, and the action ids the job sends. */
		TArray<FString> Lookups;
		TArray<int32> ActionIds;
		/** The rig's task, which animation tasks name, and the skeletal mesh whose skeleton animations go onto. */
		FString RigTaskId;
		FString RigMesh;
		/** The provider's rig model, which decides Tripo's presets. */
		FString RigModel;
		/** A skeletal mesh to retarget onto, through hyper_retarget_apply_plan, once everything is imported. */
		FString RetargetTo;

		// The model creator.
		/** Files to upload in turn for file tokens (a picture, or views where "" skips one), and the tokens they gave. */
		TArray<FString> UploadFiles;
		TArray<FString> UploadTokens;
		/** The provider's render of the result, downloaded beside the model; relative to the project. */
		FString PreviewFile;
		/** Credits the provider reported its finished tasks consumed (Tripo's credits_consumed); negative while none said. */
		float CreditsConsumed = -1.f;
	};

	TArray<TSharedRef<FJob>>& Jobs();
	TSharedPtr<FJob> FindJob(const FString& OperationId);

	/**
	 * Name, or Name_2 and on: a model gets a folder of its own under Folder, so a name is free when no asset is in that
	 * folder and no other job waiting to import has it.
	 */
	FString FreeName(const FString& Folder, const FString& Name, const FJob* Self = nullptr);

	TSharedPtr<FJsonObject> ParseJson(const TArray<uint8>& Body);
	FString JsonText(const TSharedRef<FJsonObject>& Json);
	TArray<uint8> Utf8(const FString& Text);
	/** A dotted path into a reply, such as data.output.model_url; a number comes back as text. */
	FString Field(const TSharedPtr<FJsonObject>& Json, const FString& Path);

	/** A multipart form, for the providers that take a file. */
	struct FForm
	{
		FString Boundary = TEXT("----HyperAIStudio") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		TArray<uint8> Body;

		void Text(const FString& Name, const FString& Value);
		void File(const FString& Name, const FString& Path, const TArray<uint8>& Bytes);
		TArray<uint8> Finish();
		FString ContentType() const;
	};

	/** The job's record beside its files, which also keeps the provider's credit line for Hi3D. */
	void WriteRecord(const FJob& Job);
	void Fail(FJob& Job, const FString& Why);

	using FOnReply = TFunction<void(FJob& Job, int32 Code, const TArray<uint8>& Body)>;

	/** One request for the job; OnReply runs on the game thread, and not at all once the job has failed or stopped. */
	void Send(const TSharedRef<FJob>& Job, const TCHAR* Verb, const FString& Url, const TMap<FString, FString>& Headers,
		TArray<uint8> Body, FOnReply OnReply, float TimeoutSeconds = 120.f);

	/** The key in the service's own header (Authorization: Bearer for Tripo, Meshy and Viggle). */
	TMap<FString, FString> KeyHeaders(const FJob& Job);
	TMap<FString, FString> JsonHeaders(const FJob& Job);

	/** What a provider said went wrong, in its own words where it gave any. */
	FString ProviderError(const FJob& Job, int32 Code, const TArray<uint8>& Body);

	/** Prefix and the prompt's first words, or the name given, as an asset name: letters, digits and underscores. */
	FString AssetNameFor(const FString& Name, const FString& Prompt, const TCHAR* Prefix = TEXT("SM_"));

	/** The job as hyper_mesh_status reports it. */
	FHyperAIMeshJob Snapshot(const FJob& Job);
	void RecordFailure(const FJob& Job);

	/** Approved: book the spend, then run the job in the background, where the Activity panel shows it. */
	void Begin(const TSharedRef<FJob>& Job);

	/** The steps of a model made from words or a picture. */
	void StepGenerate(const TSharedRef<FJob>& Job);
	/** The steps of rigging, animating and texturing. */
	void StepCharacter(const TSharedRef<FJob>& Job);
}
