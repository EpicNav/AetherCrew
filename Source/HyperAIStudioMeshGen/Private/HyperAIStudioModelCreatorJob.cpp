// Games by Hyper 2026.

#include "HyperAIStudioModelCreator.h"

#include "Containers/Ticker.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioMeshGenJob.h"
#include "HyperAIStudioMeshGenToolset.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSourceControl.h"
#include "HyperAIStudioTripoSettings.h"
#include "JsonObjectConverter.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioModelCreator, Log, All);

/**
 * Creator and check-and-fix jobs. A job is a small state machine journaled to Saved/HyperAIStudio/Models/<id>/creator.json
 * after every step: generate (one approval, the price shown), import, check, free fixes, check again, then while checks
 * fail and the budget and rounds last, the cheapest paid Tripo fix that would clear one (each its own approval, its price
 * shown), and finally the materials and, when asked, a rig. Provider work runs as the 3D jobs every other mesh tool runs
 * (hyper_operation_wait follows them); the rest runs here, on the game thread, a step a tick.
 */
namespace HyperAIStudio::MeshGen::Creator
{
	using namespace HyperAIStudio::MeshGen::Private;

	namespace JobDetail
	{
		constexpr const TCHAR* CreateToolName = TEXT("hyper_model_create");
		constexpr const TCHAR* CheckFixToolName = TEXT("hyper_model_check_and_fix");

		struct FCreator
		{
			FHyperAIModelJob Info;
			FHyperAIModelBrief Brief;
			/** Tripo, whose tasks every Tripo stage runs (a Tripo plan's generation, the fixes, the picture, the rig). */
			FHyperAIStudioExternalServiceSettings Service;
			/** Hi3D, for a Hi3D plan's generation and its Hi3D fixes; and whether Tripo's key is set, for Tripo's fixes on it. */
			FHyperAIStudioExternalServiceSettings Hi3D;
			bool bTripoKey = true;
			/** The reference picture's Tripo task (image-to-model's input and texture_prompt.image), or the picture's file token. */
			FString ReferenceTaskId;
			FString PictureToken;
			/** The picture was approved on its own; the generation now waits for its own approval (the review). */
			bool bReviewing = false;
			/** A Hi3D regeneration's resolution, the plan's once it is done. */
			FString PendingResolution;
			FCheckTargets Targets;
			/** Where the generation imports: Folder/Name, as hyper_mesh_generate's models land. */
			FString Folder;
			FString Name;
			FString ReferenceImage;
			bool bLocalOnly = false;
			bool bAllowPaid = true;
			bool bRig = false;
			FString RetargetTo;
			/** Tripo's task the asset is while Tripo can take it as an input: its generation, not reshaped by a paid fix since. */
			FString GenerationTaskId;
			bool bInputIsGeneration = false;
			TMap<FString, int32> Tried;
			float FixSpentUSD = 0.f;
			/** The provider work under way, what it is (generate, paid_fix, regenerate, rig), and its step. */
			TSharedPtr<FJob> Stage;
			FString StageKind;
			int32 StageStep = INDEX_NONE;
			FString StageFixId;
			/** A check-and-fix job waits for its own approval before it changes anything. */
			bool bLocalApproved = true;
			int32 LocalRound = -1;
			bool bFinished = false;
			/** Started from the Tripo Studio watch's offer: whatever it asks for later waits for a click, never auto-approval. */
			bool bClickOnly = false;
			/**
			 * The static mesh as it was when the paid fix under way was asked for (geometry and slots), and the check it had
			 * then: a fix that leaves more failing, or looks worse, is undone from this. Kept in memory for this session; the
			 * round's folder keeps a GLB of it as well.
			 */
			TOptional<FMeshDescription> Before;
			TArray<FStaticMaterial> BeforeMaterials;
			FString BeforeAssetPath;
			FHyperAIModelCheckReport BeforeCheck;
			bool bCompareAfterFix = false;
		};

		FString PackageOf(const FString& ObjectPath)
		{
			FString Package = ObjectPath;
			int32 Dot = INDEX_NONE;
			if (Package.FindChar(TEXT('.'), Dot))
			{
				Package.LeftInline(Dot);
			}
			return Package;
		}

		TArray<TSharedRef<FCreator>>& Creators()
		{
			static TArray<TSharedRef<FCreator>> Items;
			return Items;
		}

		TSharedPtr<FCreator> FindCreator(const FString& OperationId)
		{
			const TSharedRef<FCreator>* Found = Creators().FindByPredicate([&](const TSharedRef<FCreator>& C) { return C->Info.OperationId == OperationId; });
			return Found ? TSharedPtr<FCreator>(*Found) : nullptr;
		}

		FString JournalFile(const FString& OperationId)
		{
			return ModelsRoot() / OperationId / TEXT("creator.json");
		}

		void Journal(FCreator& C)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject> Info = FJsonObjectConverter::UStructToJsonObject(C.Info);
			const TSharedPtr<FJsonObject> Brief = FJsonObjectConverter::UStructToJsonObject(C.Brief);
			Json->SetNumberField(TEXT("journalVersion"), 1);
			Json->SetObjectField(TEXT("job"), Info);
			Json->SetObjectField(TEXT("brief"), Brief);
			Json->SetStringField(TEXT("folder"), C.Folder);
			Json->SetStringField(TEXT("name"), C.Name);
			Json->SetStringField(TEXT("referenceImage"), C.ReferenceImage);
			Json->SetBoolField(TEXT("localOnly"), C.bLocalOnly);
			Json->SetBoolField(TEXT("allowPaid"), C.bAllowPaid);
			Json->SetBoolField(TEXT("rig"), C.bRig);
			Json->SetStringField(TEXT("retargetTo"), C.RetargetTo);
			Json->SetStringField(TEXT("generationTaskId"), C.GenerationTaskId);
			Json->SetBoolField(TEXT("inputIsGeneration"), C.bInputIsGeneration);
			Json->SetNumberField(TEXT("fixSpentUSD"), C.FixSpentUSD);
			Json->SetStringField(TEXT("referenceTaskId"), C.ReferenceTaskId);
			Json->SetStringField(TEXT("pictureToken"), C.PictureToken);
			const TSharedRef<FJsonObject> Tried = MakeShared<FJsonObject>();
			for (const TPair<FString, int32>& Pair : C.Tried)
			{
				Tried->SetNumberField(Pair.Key, Pair.Value);
			}
			Json->SetObjectField(TEXT("tried"), Tried);
			const FString File = JournalFile(C.Info.OperationId);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
			FFileHelper::SaveStringToFile(JsonText(Json), *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			C.Info.Journal = ProjectRelative(File);
		}

		bool ReadJournal(const FString& OperationId, FCreator& C, FString& OutError)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *JournalFile(OperationId)))
			{
				OutError = TEXT("No journal for that operation_id under Saved/HyperAIStudio/Models.");
				return false;
			}
			const TSharedPtr<FJsonObject> Json = ParseJson(Utf8(Text));
			const TSharedPtr<FJsonObject>* Info = nullptr;
			const TSharedPtr<FJsonObject>* Brief = nullptr;
			if (!Json.IsValid() || !Json->TryGetObjectField(TEXT("job"), Info) || !FJsonObjectConverter::JsonObjectToUStruct(Info->ToSharedRef(), &C.Info))
			{
				OutError = TEXT("The journal could not be read.");
				return false;
			}
			if (Json->TryGetObjectField(TEXT("brief"), Brief))
			{
				FJsonObjectConverter::JsonObjectToUStruct(Brief->ToSharedRef(), &C.Brief);
			}
			C.Folder = Json->GetStringField(TEXT("folder"));
			C.Name = Json->GetStringField(TEXT("name"));
			C.ReferenceImage = Json->GetStringField(TEXT("referenceImage"));
			C.bLocalOnly = Json->GetBoolField(TEXT("localOnly"));
			C.bAllowPaid = Json->GetBoolField(TEXT("allowPaid"));
			C.bRig = Json->GetBoolField(TEXT("rig"));
			C.RetargetTo = Json->GetStringField(TEXT("retargetTo"));
			C.GenerationTaskId = Json->GetStringField(TEXT("generationTaskId"));
			C.bInputIsGeneration = Json->GetBoolField(TEXT("inputIsGeneration"));
			C.FixSpentUSD = static_cast<float>(Json->GetNumberField(TEXT("fixSpentUSD")));
			Json->TryGetStringField(TEXT("referenceTaskId"), C.ReferenceTaskId);
			Json->TryGetStringField(TEXT("pictureToken"), C.PictureToken);
			const TSharedPtr<FJsonObject>* Tried = nullptr;
			if (Json->TryGetObjectField(TEXT("tried"), Tried))
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Tried)->Values)
				{
					C.Tried.Add(Pair.Key, static_cast<int32>(Pair.Value->AsNumber()));
				}
			}
			return true;
		}

		int32 AddStep(FCreator& C, const TCHAR* Kind, const FString& What, const TCHAR* Status, const float PriceUSD = 0.f, const FString& Detail = FString())
		{
			FHyperAIModelStep& Step = C.Info.Steps.AddDefaulted_GetRef();
			Step.Round = C.Info.Round;
			Step.Kind = Kind;
			Step.What = What;
			Step.Status = Status;
			Step.PriceUSD = PriceUSD;
			Step.Credits = Credits(PriceUSD);
			Step.Detail = Detail.Left(2000);
			return C.Info.Steps.Num() - 1;
		}

		void Record(const FCreator& C, const EHyperAIStudioActivityKind Kind, const FString& Detail)
		{
			FHyperAIStudioActivityEntry Entry;
			Entry.Kind = Kind;
			Entry.PackId = FHyperAIStudioMeshGenContracts::PackId;
			Entry.ToolName = C.Info.Kind == TEXT("create") ? CreateToolName : CheckFixToolName;
			Entry.Target = C.Info.AssetPath;
			Entry.OperationId = C.Info.OperationId;
			Entry.StatusCode = C.Info.State;
			Entry.Detail = Detail.Left(1000);
			FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
		}

		void Finish(FCreator& C, const TCHAR* Reason)
		{
			C.bFinished = true;
			C.Info.StopReason = Reason;
			C.Info.State = FCString::Strcmp(Reason, TEXT("checks_passed")) == 0 ? TEXT("completed") : FCString::Strcmp(Reason, TEXT("failed")) == 0 ? TEXT("failed")
				: FCString::Strcmp(Reason, TEXT("cancelled")) == 0 ? TEXT("cancelled") : TEXT("stopped");
			C.Info.CurrentOperationId.Reset();
			const FString Why = FCString::Strcmp(Reason, TEXT("checks_passed")) == 0 ? FString(TEXT("Every check passes."))
				: FCString::Strcmp(Reason, TEXT("budget_used")) == 0 ? FString(TEXT("Checks still fail and the next paid fix costs more than the fix budget has left."))
				: FCString::Strcmp(Reason, TEXT("rounds_used")) == 0 ? FString(TEXT("Checks still fail after the most rounds allowed."))
				: FCString::Strcmp(Reason, TEXT("fix_declined")) == 0 ? FString(TEXT("Checks still fail; the user turned the paid fix down."))
				: FCString::Strcmp(Reason, TEXT("no_paid_fix")) == 0 ? FString(TEXT("Checks still fail and no paid fix addresses what is left."))
				: FCString::Strcmp(Reason, TEXT("local_only")) == 0 ? FString(TEXT("Checks still fail; only free fixes were allowed (or the provider's key is not set)."))
				: FCString::Strcmp(Reason, TEXT("fix_made_it_worse")) == 0 ? FString(TEXT("The last paid fix made the model worse, so it was undone and no more paid fixes are offered."))
				: FString(Reason);
			AddStep(C, TEXT("stop"), Why, TEXT("done"));
			C.Info.Diagnostic = FString::Printf(TEXT("%s %s Spent $%.2f (%.0f credits by Tripo's count) over %d round(s). %s"), *C.Info.State, *Why, C.Info.SpentUSD,
				FMath::Max(C.Info.CreditsConsumed, 0.f), C.Info.Round, *C.Info.LastCheck.Summary);
			Journal(C);
			Record(C, C.Info.State == TEXT("failed") ? EHyperAIStudioActivityKind::Failed : EHyperAIStudioActivityKind::Completed, C.Info.Diagnostic);
			UE_LOG(LogHyperAIStudioModelCreator, Display, TEXT("Model job %s: %s"), *C.Info.OperationId, *C.Info.Diagnostic);
		}

		// ------------------------------------------------------------ provider stages

		/** A Tripo v3 task: POSTed to Endpoint with Body, where {input} becomes the uploaded file token and {segment_task} the task before. */
		FTask Task(const FCreator& C, const FString& Label, const FString& Endpoint, const TSharedPtr<FJsonObject>& Body, const double Cost, const bool bLast)
		{
			FTask Out;
			Out.Label = Label;
			Out.CreateUrl = C.Service.BaseUrl + Endpoint;
			Out.QueryUrl = C.Service.BaseUrl + TEXT("/tasks/{id}");
			Out.TaskIdPath = TEXT("data.task_id");
			Out.StatePath = TEXT("data.status");
			Out.ProgressPath = TEXT("data.progress");
			Out.Cost = Cost;
			const FString Text = Body.IsValid() ? JsonText(Body.ToSharedRef()) : FString(TEXT("{}"));
			Out.Body = [Text](const FJob& Job)
			{
				FString Filled = Text.Replace(TEXT("{input}"), *Job.Token);
				if (Job.TaskIndex > 0)
				{
					Filled = Filled.Replace(TEXT("{segment_task}"), *Job.Tasks[Job.TaskIndex - 1].TaskId);
				}
				const TSharedPtr<FJsonObject> Json = ParseJson(Utf8(Filled));
				return Json.IsValid() ? Json.ToSharedRef() : MakeShared<FJsonObject>();
			};
			Out.OnDone = [bLast](FJob& Job, const TSharedPtr<FJsonObject>& Json, FString& OutError)
			{
				const FString Consumed = Field(Json, TEXT("data.credits_consumed"));
				if (!Consumed.IsEmpty())
				{
					Job.CreditsConsumed = FMath::Max(Job.CreditsConsumed, 0.f) + FCString::Atof(*Consumed);
				}
				if (!bLast)
				{
					return true;
				}
				const FString Model = Field(Json, TEXT("data.output.model_url"));
				if (!Model.StartsWith(TEXT("http://")) && !Model.StartsWith(TEXT("https://")))
				{
					OutError = TEXT("Tripo finished the task without a model to download.");
					return false;
				}
				Job.Downloads.Add({Model, TEXT("model"), FString()});
				const FString Render = Field(Json, TEXT("data.output.rendered_image_url"));
				if (Render.StartsWith(TEXT("http://")) || Render.StartsWith(TEXT("https://")))
				{
					Job.Downloads.Add({Render, TEXT("preview"), FString()});
				}
				return true;
			};
			return Out;
		}

		TSharedRef<FJob> NewStage(const FCreator& C, const FString& OperationId, const FString& Folder, const FString& Name, const bool bHi3D = false)
		{
			const TSharedRef<FJob> Job = MakeShared<FJob>();
			Job->Kind = EKind::Model;
			Job->ToolName = C.Info.Kind == TEXT("create") ? CreateToolName : CheckFixToolName;
			Job->Service = bHi3D ? C.Hi3D : C.Service;
			Job->AssetFolder = Folder;
			Job->AssetName = Name;
			Job->Info.Kind = TEXT("model");
			Job->Info.OperationId = OperationId;
			Job->Info.Provider = bHi3D ? TEXT("hi3d") : TEXT("tripo");
			Job->Info.Model = C.Info.Plan.Model;
			Job->Info.Prompt = C.Brief.Prompt;
			return Job;
		}

		/** A Hi3D task: its multipart form built when it is sent, the model and its cover taken from the finished task. */
		FTask Hi3DTask(const FCreator& C, const FString& Label, const FHi3DRequest& Request, const double Cost)
		{
			FTask Out;
			Out.Label = Label;
			Out.CreateUrl = C.Hi3D.BaseUrl + Hi3DSubmitPath;
			Out.QueryUrl = C.Hi3D.BaseUrl + TEXT("/open-api/v1/query-task?task_id={id}");
			Out.TaskIdPath = TEXT("data.task_id");
			Out.StatePath = TEXT("data.state");
			Out.Cost = Cost;
			Out.Form = [Request](const FJob& Job, FForm& Form, FString& OutError)
			{
				FHi3DRequest Now = Request;
				if (Now.Images.IsEmpty() && !Job.ImageFile.IsEmpty())
				{
					Now.Images = {Job.ImageFile};
				}
				if (Now.RequestType == 2 && Now.Mesh.IsEmpty())
				{
					Now.Mesh = Job.ExportFile;
				}
				return Hi3DForm(Now, Form, OutError);
			};
			Out.OnDone = [](FJob& Job, const TSharedPtr<FJsonObject>& Json, FString& OutError)
			{
				// Both links last an hour: they are fetched as soon as the task is done.
				const FString Model = Field(Json, TEXT("data.url"));
				if (!Model.StartsWith(TEXT("http://")) && !Model.StartsWith(TEXT("https://")))
				{
					OutError = TEXT("Hi3D finished the task without a model to download.");
					return false;
				}
				Job.Downloads.Add({Model, TEXT("model"), FString()});
				const FString Cover = Field(Json, TEXT("data.cover_url"));
				if (Cover.StartsWith(TEXT("http://")) || Cover.StartsWith(TEXT("https://")))
				{
					Job.Downloads.Add({Cover, TEXT("preview"), FString()});
				}
				return true;
			};
			return Out;
		}

		/** The pictures a Hi3D task is made from, absolute: the views in Hi3D's order, the brief's picture, or the reference made first. */
		TArray<FString> Hi3DImagesOf(const FCreator& C, bool& bOutMultiview)
		{
			const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			auto Absolute = [&ProjectDir](const FString& Path) { return Path.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? ProjectDir / Path : Path); };
			bOutMultiview = C.Brief.MultiviewImages.Num() > 0;
			if (bOutMultiview)
			{
				TArray<FString> Views;
				for (const FString& View : Hi3DViewOrder(C.Brief.MultiviewImages))
				{
					Views.Add(Absolute(View));
				}
				return Views;
			}
			const FString Picture = !C.Brief.ImagePath.TrimStartAndEnd().IsEmpty() ? Absolute(C.Brief.ImagePath.TrimStartAndEnd()) : C.ReferenceImage;
			return Picture.IsEmpty() ? TArray<FString>() : TArray<FString>{Picture};
		}

		/** Tripo's text-to-image: the reference picture, downloaded at once (its link lasts minutes) as the visual check's reference. */
		FTask ReferenceTask(const FCreator& C)
		{
			const FHyperAIModelPlan& Plan = C.Info.Plan;
			FTask Out;
			Out.Label = TEXT("reference");
			Out.CreateUrl = C.Service.BaseUrl + TEXT("/generation/text-to-image");
			Out.QueryUrl = C.Service.BaseUrl + TEXT("/tasks/{id}");
			Out.TaskIdPath = TEXT("data.task_id");
			Out.StatePath = TEXT("data.status");
			Out.ProgressPath = TEXT("data.progress");
			Out.Cost = ImagePrice(Plan.ImageModel, Plan.ImageQuality, Plan.ImageSize);
			const TSharedRef<FJsonObject> Body = TextToImageBody(Plan);
			Out.Body = [Body](const FJob&) { return Body; };
			Out.OnDone = [](FJob& Job, const TSharedPtr<FJsonObject>& Json, FString& OutError)
			{
				const FString Consumed = Field(Json, TEXT("data.credits_consumed"));
				if (!Consumed.IsEmpty())
				{
					Job.CreditsConsumed = FMath::Max(Job.CreditsConsumed, 0.f) + FCString::Atof(*Consumed);
				}
				FString Url = Field(Json, TEXT("data.output.generated_image_url"));
				Url = Url.IsEmpty() ? Field(Json, TEXT("data.output.image_url")) : Url;
				if (!Url.StartsWith(TEXT("http://")) && !Url.StartsWith(TEXT("https://")))
				{
					OutError = TEXT("Tripo finished the picture without one to download.");
					return false;
				}
				Job.Downloads.Add({Url, TEXT("reference"), FString()});
				return true;
			};
			return Out;
		}

		/** The model as Tripo's input: its generation task while that is still what the asset is, else the asset exported and uploaded. */
		FString InputFor(FCreator& C, FJob& Job)
		{
			if (C.bInputIsGeneration && !C.GenerationTaskId.IsEmpty())
			{
				return C.GenerationTaskId;
			}
			Job.ExportMesh = C.Info.AssetPath;
			Job.bUpload = true;
			return TEXT("{input}");
		}

		/**
		 * Asks the user for a stage and, approved, runs it; the stage's job carries the price, booked against the session's
		 * cap. bApproved: the user approved it already (in the picture's approval, which priced both): it runs at once.
		 */
		bool AskStage(FCreator& C, const TSharedRef<FJob>& Job, const FString& What, const float PriceUSD, const TCHAR* VariantId, const TArray<FString>& Lines, FString& OutError,
			const bool bApproved = false)
		{
			Job->Price = PriceUSD;
			Job->Info.Cost = PriceUSD;
			Job->Info.State = TEXT("awaiting_user_approval");
			double Cost = 0.0;
			if (!FHyperAIStudioExternalServices::Reserve(Job->Service.Id, 1, Cost, OutError, PriceUSD))
			{
				return false;
			}
			if (bApproved)
			{
				Jobs().Insert(Job, 0);
				C.Stage = Job;
				C.Info.CurrentOperationId = Job->Info.OperationId;
				Begin(Job);
				return true;
			}
			FHyperAIStudioApprovalSummary Summary;
			Summary.PackId = FHyperAIStudioMeshGenContracts::PackId;
			Summary.ToolName = Job->ToolName;
			Summary.VariantId = VariantId;
			Summary.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
			Summary.bClickOnly = C.bClickOnly;
			Summary.EffectTarget = C.Info.AssetPath.IsEmpty() ? C.Folder / C.Name : C.Info.AssetPath;
			const bool bHi3D = Job->Service.Id == TEXT("hi3d");
			Summary.Effects = {What, FString::Printf(TEXT("Pay %s %d %s ($%.2f), against this session's spend cap"), bHi3D ? TEXT("Hi3D") : TEXT("Tripo"),
				ProviderCredits(Job->Service.Id, PriceUSD), bHi3D ? TEXT("credits") : TEXT("API credits"), PriceUSD)};
			for (const FString& Line : Lines)
			{
				Summary.Effects.Add(TEXT("| ") + Line);
			}
			Summary.Touches = {Summary.EffectTarget};
			if (!FHyperAIStudioApprovalGate::RequestAction(Job->Info.OperationId, Summary, [Job]() { Begin(Job); }, OutError))
			{
				return false;
			}
			Jobs().Insert(Job, 0);
			C.Stage = Job;
			C.Info.CurrentOperationId = Job->Info.OperationId;
			return true;
		}

		/** The generation (or a new one): its upload, its task, and a parts model's texture task after it. */
		TSharedRef<FJob> GenerationStage(FCreator& C, const FString& OperationId, const FHyperAIModelBrief& Brief, const bool bAutofix)
		{
			const TSharedRef<FJob> Job = NewStage(C, OperationId, C.Folder, C.Name);
			const FHyperAIModelPlan& Plan = C.Info.Plan;
			const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			auto Absolute = [&ProjectDir](const FString& Path) { return Path.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? ProjectDir / Path : Path); };
			if ((Plan.Input == TEXT("image") || Plan.bMakeViews) && !Plan.bReferenceFirst)
			{
				Job->UploadFiles = {Absolute(Brief.ImagePath)};
			}
			else if (Plan.Input == TEXT("multiview") && Brief.MultiviewTaskId.IsEmpty())
			{
				for (const FString& View : Brief.MultiviewImages)
				{
					Job->UploadFiles.Add(Absolute(View.TrimStartAndEnd()));
				}
			}
			Job->bUpload = !Job->UploadFiles.IsEmpty();
			// Edited views: the picture made into four views (or the given multiview task), the views edited, then the model.
			if (Plan.bMakeViews)
			{
				FTask Views = Task(C, TEXT("views"), TEXT("/generation/image-to-multiview"), nullptr, Price(TEXT("image_to_multiview")), /*bLast=*/false);
				Views.Body = [](const FJob& Job) { return ImageToMultiviewBody(Job.UploadTokens.IsEmpty() ? FString() : Job.UploadTokens[0]); };
				Job->Tasks.Add(MoveTemp(Views));
			}
			if (!Plan.ViewEdits.IsEmpty())
			{
				const TArray<FString> EditLines = Plan.ViewEdits;
				const FString Given = Brief.MultiviewTaskId.TrimStartAndEnd();
				FTask Edit = Task(C, TEXT("view edits"), TEXT("/generation/edit-multiview"), nullptr, Price(TEXT("edit_multiview")) * Plan.ViewEdits.Num(), /*bLast=*/false);
				Edit.Body = [EditLines, Given](const FJob& Job)
				{
					TArray<TPair<FString, FString>> Edits;
					for (const FString& Line : EditLines)
					{
						FString View, What;
						Line.Split(TEXT(": "), &View, &What);
						Edits.Add({View, What});
					}
					return EditMultiviewBody(Job.TaskIndex > 0 ? Job.Tasks[Job.TaskIndex - 1].TaskId : Given, Edits);
				};
				Job->Tasks.Add(MoveTemp(Edit));
			}
			// The picture made first is its own stage, priced on its own: what the generation costs is the 3D alone.
			FHyperAIModelPlan Priced = Plan;
			Priced.bReferenceFirst = false;
			FTask Generate = Task(C, TEXT("generate"), Plan.Endpoint, nullptr, GenerationPrice(Priced), !(Plan.bParts && Plan.TextureQuality != TEXT("none")));
			const FHyperAIModelPlan PlanCopy = Plan;
			const FHyperAIModelBrief BriefCopy = Brief;
			const FString ReferenceTaskId = C.ReferenceTaskId;
			Generate.Body = [PlanCopy, BriefCopy, bAutofix, ReferenceTaskId](const FJob& Job)
			{
				FHyperAIModelBrief Now = BriefCopy;
				if (Job.TaskIndex > 0)
				{
					// The model is made from the views the task before made or edited.
					Now.MultiviewTaskId = Job.Tasks[Job.TaskIndex - 1].TaskId;
					Now.MultiviewImages.Reset();
				}
				// Reference first: image-to-model takes the picture's text-to-image task as its input, nothing downloaded or uploaded.
				const TArray<FString> Inputs = PlanCopy.bReferenceFirst ? TArray<FString>{ReferenceTaskId} : Job.TaskIndex > 0 ? TArray<FString>() : Job.UploadTokens;
				const TSharedRef<FJsonObject> Json = GenerationBody(PlanCopy, Now, Inputs);
				if (bAutofix && PlanCopy.Input == TEXT("image"))
				{
					Json->SetBoolField(TEXT("enable_image_autofix"), true);
				}
				return Json;
			};
			Job->Tasks.Add(MoveTemp(Generate));
			if (Plan.bParts && Plan.TextureQuality != TEXT("none"))
			{
				// Parts come untextured; this textures every part, from the generation's own task.
				FTask Texture = Task(C, TEXT("texture"), TEXT("/models/texture"), nullptr, 0.0, /*bLast=*/true);
				Texture.Body = [PlanCopy, BriefCopy](const FJob& Job)
				{
					return TextureBody(Job.Tasks[Job.TaskIndex - 1].TaskId, PlanCopy.TextureQuality, PlanCopy.bPbr, {}, BriefCopy.Prompt);
				};
				Job->Tasks.Add(MoveTemp(Texture));
			}
			return Job;
		}

		/** A Hi3D plan's generation: one submit-task from the picture, the views, or the reference made first (by HyperAI's image generation without Tripo). */
		TSharedRef<FJob> Hi3DGenerationStage(FCreator& C, const FString& OperationId, const FHyperAIModelPlan& Plan)
		{
			const TSharedRef<FJob> Job = NewStage(C, OperationId, C.Folder, C.Name, /*bHi3D=*/true);
			bool bMultiview = false;
			const TArray<FString> Images = Hi3DImagesOf(C, bMultiview);
			FHyperAIModelPlan Priced = Plan;
			Priced.bReferenceFirst = false;
			const float Hi3DUSD = GenerationPrice(Priced);
			Job->Tasks.Add(Hi3DTask(C, TEXT("generate"), Hi3DRequestFromPlan(Plan, Images, bMultiview), Hi3DUSD));
			if (Plan.bReferenceFirst && Plan.ReferenceSource == TEXT("imagegen") && Images.IsEmpty())
			{
				Job->ReferencePrompt = Plan.ImagePrompt.IsEmpty() ? C.Brief.Prompt : Plan.ImagePrompt;
				Job->ReferenceModel = Plan.ImageModel;
				Job->ReferenceCost = ReferenceImagePrice(Plan.ImageModel);
			}
			return Job;
		}

		/** The picture alone, as a stage: approved and paid on its own when it is reviewed, or the first of two the user approved together. */
		TSharedRef<FJob> ReferenceStage(FCreator& C, const FString& OperationId)
		{
			const TSharedRef<FJob> Job = NewStage(C, OperationId, C.Folder, C.Name);
			Job->Tasks.Add(ReferenceTask(C));
			return Job;
		}

		/** The model's generation for the plan's provider; a reference-first plan's picture is a stage before it. */
		TSharedRef<FJob> MakeGeneration(FCreator& C, const FString& OperationId, const FHyperAIModelBrief& Brief, const bool bAutofix)
		{
			return C.Info.Plan.Provider == TEXT("hi3d") ? Hi3DGenerationStage(C, OperationId, C.Info.Plan) : GenerationStage(C, OperationId, Brief, bAutofix);
		}

		/** What the generation costs without the picture (paid in a stage of its own when the plan makes one with Tripo). */
		float ModelPrice(const FHyperAIModelPlan& Plan)
		{
			if (Plan.bReferenceFirst && Plan.ReferenceSource == TEXT("tripo"))
			{
				FHyperAIModelPlan Priced = Plan;
				Priced.bReferenceFirst = false;
				return GenerationPrice(Priced);
			}
			return Plan.EstimatedCost;
		}

		/** A copy of a fix's or a new generation's mesh onto the asset the user already has, so its path and references stay. */
		bool CopyOnto(const FString& TargetPath, const FString& SourcePath, FString& OutNote)
		{
			UStaticMesh* Target = LoadObject<UStaticMesh>(nullptr, *TargetPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			UStaticMesh* Source = LoadObject<UStaticMesh>(nullptr, *SourcePath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			const FMeshDescription* From = Source && Source->IsMeshDescriptionValid(0) ? Source->GetMeshDescription(0) : nullptr;
			if (!Target || !From)
			{
				OutNote = TEXT("not both static meshes");
				return false;
			}
			Target->Modify();
			FMeshDescription* To = Target->IsMeshDescriptionValid(0) ? Target->GetMeshDescription(0) : Target->CreateMeshDescription(0);
			Target->ModifyMeshDescription(0);
			*To = *From;
			Target->CommitMeshDescription(0);
			Target->GetStaticMaterials() = Source->GetStaticMaterials();
			Target->PostEditChange();
			Target->MarkPackageDirty();
			return FHyperAIStudioSourceControl::SaveAsset(*Target, OutNote);
		}

		/** The task that made the model (after any views and edits), which Tripo's later operations take as input. */
		FString GenerationTaskOf(const FJob& Job)
		{
			const FTask* Generate = Job.Tasks.FindByPredicate([](const FTask& Task) { return Task.Label == TEXT("generate"); });
			return Generate ? Generate->TaskId : FString();
		}

		UObject* LoadMesh(const FString& Path)
		{
			UObject* Object = LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
			return Cast<UStaticMesh>(Object) || Cast<USkeletalMesh>(Object) ? Object : nullptr;
		}

		// ------------------------------------------------------------ before and after a paid fix

		/** What the guard compares: triangles, extents sorted small to large, mapped UV area, and slots with a material. */
		struct FMeshFacts
		{
			int32 Triangles = 0;
			FVector3d Extents = FVector3d::ZeroVector;
			double UVArea = 0.0;
			int32 Materials = 0;
		};

		bool FactsOf(const FString& Path, FMeshFacts& Out)
		{
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
			const FMeshDescription* Source = Mesh && Mesh->IsMeshDescriptionValid(0) ? Mesh->GetMeshDescription(0) : nullptr;
			if (!Source)
			{
				return false;
			}
			FStaticMeshConstAttributes Attributes(*Source);
			const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
			const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
			FBox3d Box(ForceInit);
			for (const FVertexID Vertex : Source->Vertices().GetElementIDs())
			{
				Box += FVector3d(Positions[Vertex]);
			}
			Out.Triangles = Source->Triangles().Num();
			if (Box.IsValid)
			{
				TArray<double> Sizes = {Box.GetSize().X, Box.GetSize().Y, Box.GetSize().Z};
				Sizes.Sort();
				Out.Extents = FVector3d(Sizes[0], Sizes[1], Sizes[2]);
			}
			if (UVs.IsValid() && UVs.GetNumChannels() > 0)
			{
				for (const FTriangleID Triangle : Source->Triangles().GetElementIDs())
				{
					const TArrayView<const FVertexInstanceID> Corners = Source->GetTriangleVertexInstances(Triangle);
					const FVector2f A = UVs.Get(Corners[0], 0), B = UVs.Get(Corners[1], 0), Cc = UVs.Get(Corners[2], 0);
					Out.UVArea += 0.5 * FMath::Abs(double((B.X - A.X) * (Cc.Y - A.Y) - (Cc.X - A.X) * (B.Y - A.Y)));
				}
			}
			for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
			{
				Out.Materials += Slot.MaterialInterface != nullptr;
			}
			return true;
		}

		/**
		 * Why a paid fix's result must not replace the asset, or empty when it may: a fraction of its triangles (unless the
		 * fix was a reduction asked for, and even then not far under the budget), its shape collapsed (Tripo hands results
		 * back at its own scale, so proportions are compared, not sizes), or its UVs or materials gone when the asset had them.
		 */
		FString BrokenResult(const FCreator& C, const FString& ResultPath, const FString& FixId)
		{
			FMeshFacts Now, Result;
			if (!FactsOf(C.Info.AssetPath, Now) || !FactsOf(ResultPath, Result))
			{
				// A skeletal result, or nothing to compare with: the checks after it decide.
				return FString();
			}
			const int32 Budget = C.Info.Plan.FaceLimit > 0 ? C.Info.Plan.FaceLimit : Now.Triangles;
			const bool bReduction = FixId == TEXT("retopo") && Now.Triangles > Budget;
			const int32 Floor = bReduction ? FMath::Min(Budget, Now.Triangles) : Now.Triangles;
			if (Result.Triangles * 4 < Floor)
			{
				return FString::Printf(TEXT("it came back with %d triangles, under a quarter of the %d %s"), Result.Triangles, Floor,
					bReduction ? TEXT("the budget asked for") : TEXT("the model has"));
			}
			if (Now.Extents.Z > 0.0 && Result.Extents.Z > 0.0)
			{
				for (int32 Axis = 0; Axis < 2; ++Axis)
				{
					const double Was = Now.Extents[Axis] / Now.Extents.Z;
					const double Is = Result.Extents[Axis] / Result.Extents.Z;
					if (Was > 0.02 && Is < Was * 0.5)
					{
						return FString::Printf(TEXT("its shape collapsed: %.2f x %.2f of its longest side, against %.2f x %.2f before"), Result.Extents.X / Result.Extents.Z,
							Result.Extents.Y / Result.Extents.Z, Now.Extents.X / Now.Extents.Z, Now.Extents.Y / Now.Extents.Z);
					}
				}
			}
			if (Now.UVArea > 0.0 && Result.UVArea <= 0.0)
			{
				return TEXT("it came back with no UVs, and the model had them");
			}
			if (Now.Materials > 0 && Result.Materials == 0)
			{
				return TEXT("it came back with no materials, and the model had them");
			}
			return FString();
		}

		/** The asset as it is now, to undo a paid fix with; and a GLB of it in the round's folder, for the record. */
		void KeepBefore(FCreator& C)
		{
			C.Before.Reset();
			C.BeforeMaterials.Reset();
			C.bCompareAfterFix = false;
			C.BeforeCheck = C.Info.LastCheck;
			C.BeforeAssetPath = C.Info.AssetPath;
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *C.Info.AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Mesh || !Mesh->IsMeshDescriptionValid(0))
			{
				return;
			}
			C.Before = *Mesh->GetMeshDescription(0);
			C.BeforeMaterials = Mesh->GetStaticMaterials();
			FString Error;
			const FString File = ModelsRoot() / C.Info.OperationId / FString::Printf(TEXT("round%d"), C.Info.Round) / TEXT("before_fix.glb");
			if (!HyperAIStudio::MeshGen::Characters::ExportGlb(C.Info.AssetPath, File, Error))
			{
				UE_LOG(LogHyperAIStudioModelCreator, Warning, TEXT("Model job %s: the round's snapshot was not written: %s"), *C.Info.OperationId, *Error);
			}
		}

		/** Puts back what KeepBefore kept: the geometry and slots on the same asset, or the asset itself when a fix replaced it. */
		bool RestoreBefore(FCreator& C, FString& OutNote)
		{
			if (!C.BeforeAssetPath.IsEmpty() && C.BeforeAssetPath != C.Info.AssetPath)
			{
				// A skeletal result became the asset checked; the one before it was never changed.
				C.Info.AssetPath = C.BeforeAssetPath;
				return true;
			}
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *C.Info.AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Mesh || !C.Before.IsSet())
			{
				OutNote = TEXT("nothing was kept to undo it with");
				return false;
			}
			Mesh->Modify();
			FMeshDescription* To = Mesh->IsMeshDescriptionValid(0) ? Mesh->GetMeshDescription(0) : Mesh->CreateMeshDescription(0);
			Mesh->ModifyMeshDescription(0);
			*To = C.Before.GetValue();
			Mesh->CommitMeshDescription(0);
			Mesh->GetStaticMaterials() = C.BeforeMaterials;
			Mesh->PostEditChange();
			Mesh->MarkPackageDirty();
			return FHyperAIStudioSourceControl::SaveAsset(*Mesh, OutNote);
		}

		// ------------------------------------------------------------ the generation, a retry

		/**
		 * The 3D generation, asked for (the review: after the user saw the picture, or with no picture made first) or run at
		 * once when the picture's approval already covered it.
		 */
		void AskGeneration(FCreator& C, const bool bApproved)
		{
			const FHyperAIModelPlan& Plan = C.Info.Plan;
			const TSharedRef<FJob> Job = MakeGeneration(C, C.Info.OperationId + TEXT("-model"), C.Brief, false);
			const float USD = ModelPrice(Plan);
			const FString What = FString::Printf(TEXT("Make the %s with %s %s from the reference picture %s; check and fix it in the editor; import it into %s/%s"), *Plan.Use,
				Plan.Provider == TEXT("hi3d") ? TEXT("Hi3D") : TEXT("Tripo"), *Plan.Model, C.ReferenceImage.IsEmpty() ? TEXT("(made first)") : *ProjectRelative(C.ReferenceImage), *C.Folder, *C.Name);
			TArray<FString> Lines;
			for (const FString& Line : Plan.PriceLines)
			{
				if (!Line.StartsWith(TEXT("reference picture")))
				{
					Lines.Add(Line);
				}
			}
			FString Error;
			C.bReviewing = !bApproved;
			if (!AskStage(C, Job, What, USD, TEXT("model.generate.v1"), Lines, Error, bApproved))
			{
				AddStep(C, TEXT("generate"), What, TEXT("failed"), USD, Error);
				Finish(C, Error.Contains(TEXT("cap")) ? TEXT("budget_used") : TEXT("failed"));
				return;
			}
			C.StageKind = TEXT("generate");
			C.StageStep = AddStep(C, TEXT("generate"), What, bApproved ? TEXT("running") : TEXT("awaiting_user_approval"), USD, Plan.RequestJson);
			C.Info.Steps[C.StageStep].Credits = ProviderCredits(Plan.Provider, USD);
			C.Info.Steps[C.StageStep].Endpoint = Plan.Endpoint;
			C.Info.Steps[C.StageStep].OperationId = Job->Info.OperationId;
			C.Info.State = bApproved ? TEXT("generating") : TEXT("awaiting_reference_review");
			if (!bApproved)
			{
				C.Info.Diagnostic = FString::Printf(TEXT("The reference picture is ready (%s): look at it, then approve the 3D generation (%s) in Activity, or turn it down."),
					*FString::Join(C.Info.PreviewImages, TEXT(", ")), *Job->Info.OperationId);
			}
			Journal(C);
		}

		/** Hi3D failed the task with 50010001 and refunded it: offered once or twice more, priced and approved, within the budget. */
		bool AskRetry(FCreator& C, const TSharedRef<FJob>& Failed, const FString& Kind)
		{
			const int32 Tries = C.Tried.FindRef(TEXT("retry"));
			if (!Failed->bRefunded || Tries >= 2)
			{
				return false;
			}
			const bool bPictureMade = !Failed->ImageFile.IsEmpty() && IFileManager::Get().FileSize(*Failed->ImageFile) > 0;
			const float USD = static_cast<float>(Failed->Price - (bPictureMade ? Failed->ReferenceCost : 0.0));
			if (Kind != TEXT("generate") && USD > FMath::Max(C.Info.FixBudgetUSD - C.FixSpentUSD, 0.f) + KINDA_SMALL_NUMBER)
			{
				return false;
			}
			const TSharedRef<FJob> Job = NewStage(C, FString::Printf(TEXT("%s-retry%d"), *Failed->Info.OperationId, Tries + 1), Failed->AssetFolder, Failed->AssetName, Failed->Service.Id == TEXT("hi3d"));
			Job->Tasks = Failed->Tasks;
			for (FTask& Each : Job->Tasks)
			{
				Each.TaskId.Reset();
			}
			Job->ExportMesh = Failed->ExportMesh;
			Job->ImageFile = bPictureMade ? Failed->ImageFile : FString();
			Job->ReferenceFile = Failed->ReferenceFile;
			Job->ReferencePrompt = bPictureMade ? FString() : Failed->ReferencePrompt;
			Job->ReferenceModel = Failed->ReferenceModel;
			Job->ReferenceCost = bPictureMade ? 0.0 : Failed->ReferenceCost;
			const FString What = FString::Printf(TEXT("Try again: Hi3D could not generate it (50010001) and refunded its credits"));
			FString Error;
			if (!AskStage(C, Job, What, USD, TEXT("model.retry.v1"), {FString::Printf(TEXT("retry %d of 2: %d Hi3D credits ($%.2f)"), Tries + 1, Hi3DCredits(USD), USD)}, Error))
			{
				return false;
			}
			C.Tried.Add(TEXT("retry"), Tries + 1);
			C.StageKind = Kind;
			C.StageStep = AddStep(C, Kind == TEXT("generate") ? TEXT("generate") : TEXT("paid_fix"), What, TEXT("awaiting_user_approval"), USD, Failed->Info.Diagnostic);
			C.Info.Steps[C.StageStep].Credits = Hi3DCredits(USD);
			C.Info.Steps[C.StageStep].OperationId = Job->Info.OperationId;
			C.Info.State = Kind == TEXT("generate") ? TEXT("awaiting_user_approval") : TEXT("awaiting_fix_approval");
			Journal(C);
			return true;
		}

		// ------------------------------------------------------------ rounds

		void AskRig(FCreator& C);

		void AfterChecks(FCreator& C)
		{
			if (C.Info.LastCheck.bPassed)
			{
				if (C.bRig && C.Info.SkeletalMesh.IsEmpty() && !C.Tried.Contains(TEXT("rig")))
				{
					AskRig(C);
					return;
				}
				Finish(C, TEXT("checks_passed"));
				return;
			}
			if (C.bLocalOnly || !C.bAllowPaid)
			{
				Finish(C, TEXT("local_only"));
				return;
			}
			if (C.Info.Round >= C.Info.MaxRounds)
			{
				Finish(C, TEXT("rounds_used"));
				return;
			}
			FFixContext Context;
			Context.InputTaskId = C.bInputIsGeneration ? C.GenerationTaskId : FString();
			Context.InputToken = TEXT("{input}");
			Context.Provider = C.Info.Plan.Provider.IsEmpty() ? FString(TEXT("tripo")) : C.Info.Plan.Provider;
			Context.bTripo = C.bTripoKey;
			Context.Hi3DImages = Hi3DImagesOf(C, Context.bHi3DMultiview);
			Context.ReferenceInput = !C.ReferenceTaskId.IsEmpty() ? C.ReferenceTaskId : C.PictureToken;
			Context.bHasTextures = C.Info.LastCheck.Checks.ContainsByPredicate([](const FHyperAIModelCheck& Check) { return Check.Id == TEXT("textures") && Check.Result != TEXT("fail") && Check.Result != TEXT("skip"); });
			Context.bSkeletal = Cast<USkeletalMesh>(LoadMesh(C.Info.AssetPath)) != nullptr;
			Context.Tried = C.Tried;
			Context.RemainingUSD = FMath::Max(C.Info.FixBudgetUSD - C.FixSpentUSD, 0.f);
			Context.Plan = C.Info.Plan;
			Context.bCanRegenerate = C.Info.Kind == TEXT("create");
			TArray<FString> OverBudget;
			const TArray<FPaidFix> Ladder = PaidLadder(C.Info.LastCheck, Context, &OverBudget);
			if (Ladder.IsEmpty())
			{
				if (!OverBudget.IsEmpty())
				{
					AddStep(C, TEXT("paid_fix"), OverBudget[0], TEXT("skipped"), 0.f, TEXT("Over the job's fix budget."));
				}
				Finish(C, OverBudget.IsEmpty() ? TEXT("no_paid_fix") : TEXT("budget_used"));
				return;
			}
			const FPaidFix& Fix = Ladder[0];
			const FString OperationId = FString::Printf(TEXT("%s-fix%d"), *C.Info.OperationId, C.Info.Round + 1);
			FString Error;
			TSharedPtr<FJob> Job;
			TArray<FString> Lines;
			for (const FPaidFix& Other : Ladder)
			{
				Lines.Add(FString::Printf(TEXT("%s%s: %d credits ($%.2f)%s"), &Other == &Fix ? TEXT("this: ") : TEXT("next if needed: "), *Other.Id, Credits(Other.PriceUSD),
					Other.PriceUSD, Other.Clears.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", for %s"), *FString::Join(Other.Clears, TEXT(", ")))));
			}
			Lines.Add(FString::Printf(TEXT("fix budget: $%.2f of $%.2f left; round %d of %d"), Context.RemainingUSD, C.Info.FixBudgetUSD, C.Info.Round + 1, C.Info.MaxRounds));
			const FString AssetName = FPackageName::GetShortName(PackageOf(C.Info.AssetPath));
			const FString FixFolder = FPackageName::GetLongPackagePath(PackageOf(C.Info.AssetPath)) / TEXT("Fixes");
			if (Fix.Id == TEXT("regenerate"))
			{
				// A new model_seed; a reference-first model is made again from the same picture's task (the picture is re-made only when asked).
				FHyperAIModelBrief Again = C.Brief;
				Again.Seed = 1 + (FMath::Rand() % 100000);
				if (C.Info.Plan.Input == TEXT("text"))
				{
					Again.Prompt = (Again.Prompt + TEXT(", one complete object, true to the description, clean silhouette")).Left(MaxPromptCharacters);
				}
				Job = GenerationStage(C, OperationId, Again, /*bAutofix=*/!C.Info.Plan.bReferenceFirst);
				Job->AssetFolder = FixFolder;
				Job->AssetName = FString::Printf(TEXT("%s_R%d"), *AssetName, C.Info.Round + 1);
			}
			else if (Fix.Provider == TEXT("hi3d"))
			{
				Job = NewStage(C, OperationId, FixFolder, FString::Printf(TEXT("%s_R%d"), *AssetName, C.Info.Round + 1), /*bHi3D=*/true);
				if (Fix.Hi3D.RequestType == 2)
				{
					// Texture only: the asset as it is now, written as a GLB and sent as the mesh.
					Job->ExportMesh = C.Info.AssetPath;
				}
				Job->Tasks.Add(Hi3DTask(C, Fix.Id, Fix.Hi3D, Fix.PriceUSD));
				C.PendingResolution = Fix.Id == TEXT("hi3d_regenerate") ? Fix.Hi3D.Resolution : FString();
			}
			else
			{
				Job = NewStage(C, OperationId, FixFolder, FString::Printf(TEXT("%s_R%d"), *AssetName, C.Info.Round + 1));
				// The ladder was given the generation's task or {input}, the uploaded file's token: InputFor sets the upload up for the latter.
				InputFor(C, *Job);
				for (int32 Index = 0; Index < Fix.Endpoints.Num(); ++Index)
				{
					Job->Tasks.Add(Task(C, Fix.Id, Fix.Endpoints[Index], Fix.Bodies[Index], Index == 0 ? Fix.PriceUSD : 0.0, Index == Fix.Endpoints.Num() - 1));
				}
			}
			if (!AskStage(C, Job.ToSharedRef(), Fix.What, Fix.PriceUSD, TEXT("model.fix.v1"), Lines, Error))
			{
				AddStep(C, TEXT("paid_fix"), Fix.What, TEXT("failed"), Fix.PriceUSD, Error);
				Finish(C, Error.Contains(TEXT("cap")) ? TEXT("budget_used") : TEXT("no_paid_fix"));
				return;
			}
			C.StageKind = Fix.Id == TEXT("regenerate") || Fix.Id == TEXT("hi3d_regenerate") ? TEXT("regenerate") : TEXT("paid_fix");
			C.StageFixId = Fix.Id;
			// What it is now, to compare the fix with and undo it from if it makes things worse.
			KeepBefore(C);
			C.StageStep = AddStep(C, TEXT("paid_fix"), Fix.What, TEXT("awaiting_user_approval"), Fix.PriceUSD, FString::Join(Fix.Clears, TEXT(", ")));
			C.Info.Steps[C.StageStep].Credits = ProviderCredits(Fix.Provider, Fix.PriceUSD);
			C.Info.Steps[C.StageStep].Endpoint = FString::Join(Fix.Endpoints.IsEmpty() ? TArray<FString>{C.Info.Plan.Endpoint} : Fix.Endpoints, TEXT(" + "));
			C.Info.Steps[C.StageStep].OperationId = OperationId;
			C.Info.State = TEXT("awaiting_fix_approval");
			FHyperAIStudioApprovalGate::DescribeHeld(OperationId, C.Info.State, C.Info.Diagnostic);
			if (C.Info.State == TEXT("queued"))
			{
				C.Info.State = TEXT("awaiting_fix_approval");
			}
			Journal(C);
		}

		void AskRig(FCreator& C)
		{
			C.Tried.Add(TEXT("rig"), 1);
			const FString OperationId = C.Info.OperationId + TEXT("-rig");
			const FString Stem = FPackageName::GetShortName(PackageOf(C.Info.AssetPath)).Replace(TEXT("SM_"), TEXT(""));
			const TSharedRef<FJob> Job = NewStage(C, OperationId, FPackageName::GetLongPackagePath(PackageOf(C.Info.AssetPath)), TEXT("SK_") + Stem);
			const FString Input = InputFor(C, *Job);
			const float Price_ = Price(TEXT("rig"));
			// The free rig check first: a biped is rigged with v3.0 and Unreal's bone names, any other body with v2.5 as its
			// rig_type; a model Tripo cannot rig stops here, nothing paid.
			FTask Check = Task(C, TEXT("rig_check"), TEXT("/animations/rig-check"), RigCheckBody(Input), 0.0, /*bLast=*/false);
			Check.OnDone = [](FJob& Job, const TSharedPtr<FJsonObject>& Json, FString& OutError)
			{
				const TSharedPtr<FJsonObject>* Data = nullptr;
				const TSharedPtr<FJsonObject>* Output = nullptr;
				bool bRiggable = true;
				if (Json.IsValid() && Json->TryGetObjectField(TEXT("data"), Data) && (*Data)->TryGetObjectField(TEXT("output"), Output))
				{
					(*Output)->TryGetBoolField(TEXT("riggable"), bRiggable);
				}
				const FString Type = Field(Json, TEXT("data.output.rig_type"));
				if (!bRiggable)
				{
					OutError = FString::Printf(TEXT("Tripo's rig check says the model cannot be rigged%s; nothing was paid."), Type.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Type));
					return false;
				}
				Job.RigModel = Type.IsEmpty() ? FString(TEXT("biped")) : Type;
				return true;
			};
			Job->Tasks.Add(MoveTemp(Check));
			FTask Rig = Task(C, TEXT("rig"), TEXT("/animations/rig"), nullptr, Price_, /*bLast=*/true);
			Rig.Body = [Input](const FJob& Job)
			{
				FString Model, Spec;
				RigChoice(Job.RigModel, Model, Spec);
				return RigBody(Input == TEXT("{input}") ? Job.Token : Input, Model, Job.RigModel, Spec);
			};
			Job->Tasks.Add(MoveTemp(Rig));
			Job->RetargetTo = C.RetargetTo;
			FString Error;
			const FString What = FString::Printf(TEXT("Rig %s with Tripo (a free rig check first: a biped with v3.0 and the Unreal Mannequin's bone names, another body with v2.5) and import it as a skeletal mesh%s"),
				*C.Info.AssetPath, C.RetargetTo.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", then set up retargeting onto %s"), *C.RetargetTo));
			if (!AskStage(C, Job, What, Price_, TEXT("model.rig.v1"), {}, Error))
			{
				AddStep(C, TEXT("rig"), What, TEXT("failed"), Price_, Error);
				Finish(C, TEXT("checks_passed"));
				return;
			}
			C.StageKind = TEXT("rig");
			C.StageStep = AddStep(C, TEXT("rig"), What, TEXT("awaiting_user_approval"), Price_);
			C.Info.Steps[C.StageStep].Endpoint = TEXT("/animations/rig");
			C.Info.Steps[C.StageStep].OperationId = OperationId;
			C.Info.State = TEXT("rigging");
			Journal(C);
		}

		/** One round in the editor: check, free fixes, check again; then on to the paid ladder or the end. */
		void CheckRound(FCreator& C)
		{
			UObject* Mesh = LoadMesh(C.Info.AssetPath);
			if (!Mesh)
			{
				AddStep(C, TEXT("check"), C.Info.AssetPath, TEXT("failed"), 0.f, TEXT("The asset is no longer a static or skeletal mesh."));
				Finish(C, TEXT("failed"));
				return;
			}
			FCheckTargets Targets = C.Targets;
			Targets.CaptureFolder = ModelsRoot() / C.Info.OperationId / FString::Printf(TEXT("round%d"), C.Info.Round);
			Targets.ReferenceImage = C.ReferenceImage;
			Targets.ProviderRender = C.Info.PreviewImages.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / C.Info.PreviewImages.Last());
			FHyperAIModelCheckReport Report;
			FString Error;
			if (!Inspect(*Mesh, Targets, Report, Error))
			{
				AddStep(C, TEXT("check"), C.Info.AssetPath, TEXT("failed"), 0.f, Error);
				Finish(C, TEXT("failed"));
				return;
			}
			if (C.Info.FirstCheck.AssetPath.IsEmpty())
			{
				C.Info.FirstCheck = Report;
			}
			AddStep(C, TEXT("check"), Report.Summary, Report.bPassed ? TEXT("done") : TEXT("failed"));
			TArray<FString> Fixes = LocalFixesFor(Report);
			if (!Fixes.IsEmpty() && C.LocalRound != C.Info.Round)
			{
				C.LocalRound = C.Info.Round;
				C.Info.State = TEXT("fixing");
				const bool bMaterials = Fixes.Remove(TEXT("materials")) > 0;
				TArray<FString> Notes;
				bool bOk = Fixes.IsEmpty() || ApplyLocalFixes(*Mesh, Targets, Fixes, Notes, Error);
				if (bOk && bMaterials)
				{
					FMaterialResult Materials;
					bOk = BuildMaterials(*Mesh, false, Materials, Error);
					if (bOk && Targets.bWeapon)
					{
						// The material build may have packed a new ORM: capped as the weapon's other textures are.
						CapWeaponTextureSizes(*Mesh, Targets.Weapon, Materials.Notes);
					}
					Notes.Append(Materials.Notes);
					C.Info.Materials = Materials.Materials;
					C.Info.Textures = Materials.Textures;
					if (bMaterials)
					{
						Fixes.Add(TEXT("materials"));
					}
				}
				AddStep(C, TEXT("local_fix"), FString::Join(Fixes, TEXT(", ")), bOk ? TEXT("done") : TEXT("failed"), 0.f, FString::Join(Notes, TEXT(" ")) + (bOk ? FString() : TEXT(" ") + Error));
				Mesh = LoadMesh(C.Info.AssetPath);
				if (!Mesh || !Inspect(*Mesh, Targets, Report, Error))
				{
					Finish(C, TEXT("failed"));
					return;
				}
				AddStep(C, TEXT("check"), Report.Summary, Report.bPassed ? TEXT("done") : TEXT("failed"));
			}
			C.Info.LastCheck = Report;
			C.Info.State = TEXT("checking");
			if (C.bCompareAfterFix)
			{
				C.bCompareAfterFix = false;
				// A paid fix must leave fewer checks failing, and must not look worse against the reference; otherwise it is undone.
				const bool bNoBetter = Report.Failed >= C.BeforeCheck.Failed;
				const bool bLooksWorse = Report.VisualScore >= 0.f && C.BeforeCheck.VisualScore >= 0.f && Report.VisualScore < C.BeforeCheck.VisualScore - 5.f;
				if (bNoBetter || bLooksWorse)
				{
					const FString Why = bNoBetter ? FString::Printf(TEXT("%d check(s) failed after it, %d before"), Report.Failed, C.BeforeCheck.Failed)
						: FString::Printf(TEXT("its capture scored %.0f against the reference, %.0f before"), Report.VisualScore, C.BeforeCheck.VisualScore);
					FString Note;
					const bool bUndone = RestoreBefore(C, Note);
					UObject* Restored = bUndone ? LoadMesh(C.Info.AssetPath) : nullptr;
					FHyperAIModelCheckReport Again;
					if (Restored && Inspect(*Restored, Targets, Again, Error))
					{
						C.Info.LastCheck = Again;
					}
					AddStep(C, TEXT("paid_fix"), FString::Printf(TEXT("Undo the %s fix"), *C.StageFixId), bUndone ? TEXT("done") : TEXT("failed"), 0.f,
						FString::Printf(TEXT("It made the model worse: %s. %s"), *Why, bUndone ? TEXT("The asset is back as it was before the fix.") : *(TEXT("It could not be undone: ") + Note)));
					C.Info.Diagnostic = FString::Printf(TEXT("The %s fix made the model worse (%s)%s."), *C.StageFixId, *Why, bUndone ? TEXT(" and was undone") : TEXT(""));
					Finish(C, TEXT("fix_made_it_worse"));
					return;
				}
			}
			Journal(C);
			AfterChecks(C);
		}

		/** What a finished stage leaves: the asset (made, or copied onto the old one), its render, what it consumed. */
		void StageDone(FCreator& C)
		{
			const TSharedRef<FJob> Job = C.Stage.ToSharedRef();
			FHyperAIModelStep* Step = C.Info.Steps.IsValidIndex(C.StageStep) ? &C.Info.Steps[C.StageStep] : nullptr;
			if (Step)
			{
				Step->Status = TEXT("done");
				Step->TaskId = FString::JoinBy(Job->Tasks, TEXT(", "), [](const FTask& Task) { return Task.TaskId; });
				Step->CreditsConsumed = Job->CreditsConsumed;
				Step->Detail = Job->Info.Diagnostic.Left(1000);
			}
			C.Info.SpentUSD += Job->Price;
			C.Info.CreditsConsumed += FMath::Max(Job->CreditsConsumed, 0.f);
			if (!Job->PreviewFile.IsEmpty())
			{
				C.Info.PreviewImages.AddUnique(Job->PreviewFile);
			}
			const FString Kind = C.StageKind;
			C.Stage.Reset();
			C.StageKind.Reset();
			if (!Job->Normalization.IsEmpty() && Step)
			{
				Step->Detail += TEXT(" Tripo normalised the result: ") + Job->Normalization + TEXT(".");
			}
			if (Kind == TEXT("reference"))
			{
				// The picture is in: the reference for the visual check, and the generation's input.
				C.ReferenceTaskId = Job->Tasks.IsEmpty() ? FString() : Job->Tasks[0].TaskId;
				if (!Job->ReferenceFile.IsEmpty())
				{
					C.ReferenceImage = Job->ReferenceFile;
					C.Info.PreviewImages.AddUnique(ProjectRelative(Job->ReferenceFile));
				}
				AskGeneration(C, /*bApproved=*/!C.Info.Plan.bReviewReference);
				return;
			}
			if (Kind == TEXT("rig"))
			{
				C.Info.SkeletalMesh = Job->Info.SkeletalMesh;
				Finish(C, TEXT("checks_passed"));
				return;
			}
			if (Kind == TEXT("generate"))
			{
				if (C.bReviewing)
				{
					// The review is over: the picture was approved into a model.
					C.bReviewing = false;
					C.Info.Diagnostic.Reset();
				}
				C.Info.AssetPath = Job->Info.AssetPath;
				C.GenerationTaskId = Job->Service.Id == TEXT("hi3d") ? FString() : GenerationTaskOf(*Job);
				if (C.PictureToken.IsEmpty() && !Job->UploadTokens.IsEmpty() && C.Info.Plan.Input == TEXT("image"))
				{
					C.PictureToken = Job->UploadTokens[0];
				}
				if (!Job->ReferenceFile.IsEmpty() && C.ReferenceImage.IsEmpty())
				{
					// HyperAI's image generation made the reference inside the Hi3D stage.
					C.ReferenceImage = Job->ReferenceFile;
					C.Brief.ImagePath = ProjectRelative(Job->ReferenceFile);
					C.Info.PreviewImages.AddUnique(ProjectRelative(Job->ReferenceFile));
				}
				// A parts model's texture task follows its generation; Tripo's operations then take the textured result as a file. Hi3D's
				// task is no input Tripo takes: Tripo's fixes go as an uploaded file.
				C.bInputIsGeneration = Job->Service.Id != TEXT("hi3d") && Job->Tasks.Last().Label == TEXT("generate");
				if (C.Info.Plan.bParts)
				{
					// A slot a part, before the first check, so the checks see the materials the model will keep.
					if (UObject* Mesh = LoadMesh(C.Info.AssetPath))
					{
						FMaterialResult Materials;
						FString Error;
						const bool bOk = BuildMaterials(*Mesh, /*bPerPartSlots=*/true, Materials, Error);
						AddStep(C, TEXT("materials"), TEXT("A material slot a part"), bOk ? TEXT("done") : TEXT("failed"), 0.f, FString::Join(Materials.Notes, TEXT(" ")) + Error);
						C.Info.Materials = Materials.Materials;
						C.Info.Textures = Materials.Textures;
					}
				}
			}
			else
			{
				C.FixSpentUSD += Job->Price;
				const int32 Tries = C.Tried.FindRef(C.StageFixId);
				C.Tried.Add(C.StageFixId, Tries + 1);
				const FString Broken = BrokenResult(C, Job->Info.AssetPath, C.StageFixId);
				if (!Broken.IsEmpty())
				{
					// Clearly broken: the asset keeps what it had, and no more is spent on it.
					if (Step)
					{
						Step->Status = TEXT("failed");
						Step->Detail += FString::Printf(TEXT(" Not copied onto %s: %s (the result is %s)."), *C.Info.AssetPath, *Broken, *Job->Info.AssetPath);
					}
					++C.Info.Round;
					C.Info.Diagnostic = FString::Printf(TEXT("The %s fix's result was not used: %s."), *C.StageFixId, *Broken);
					Finish(C, TEXT("fix_made_it_worse"));
					return;
				}
				FString Note;
				if (CopyOnto(C.Info.AssetPath, Job->Info.AssetPath, Note))
				{
					if (Step)
					{
						Step->Detail += FString::Printf(TEXT(" Copied onto %s from %s."), *C.Info.AssetPath, *Job->Info.AssetPath);
					}
				}
				else
				{
					// A skeletal result cannot be copied onto the old mesh: the new asset becomes the one checked.
					if (Step)
					{
						Step->Detail += FString::Printf(TEXT(" Kept as %s (%s)."), *Job->Info.AssetPath, *Note);
					}
					C.Info.AssetPath = Job->Info.AssetPath;
				}
				C.bInputIsGeneration = Kind == TEXT("regenerate") && Job->Service.Id != TEXT("hi3d");
				if (C.bInputIsGeneration)
				{
					C.GenerationTaskId = GenerationTaskOf(*Job);
				}
				if (C.StageFixId == TEXT("hi3d_regenerate") && !C.PendingResolution.IsEmpty())
				{
					C.Info.Plan.Resolution = C.PendingResolution;
					C.PendingResolution.Reset();
				}
				// The next round's check says whether it helped.
				C.bCompareAfterFix = true;
			}
			// A round is a paid fix: the generation's own check is round 0.
			if (Kind != TEXT("generate"))
			{
				++C.Info.Round;
			}
			C.Info.State = TEXT("checking");
			C.Info.CurrentOperationId.Reset();
			Journal(C);
		}

		void Advance(FCreator& C)
		{
			if (C.bFinished)
			{
				return;
			}
			if (!C.bLocalApproved)
			{
				// Turned down (or dismissed from the notification): nothing was changed.
				if (!FHyperAIStudioApprovalGate::HasPending(C.Info.OperationId))
				{
					Finish(C, TEXT("fix_declined"));
				}
				return;
			}
			if (C.Stage.IsValid())
			{
				const FHyperAIMeshJob Snap = Snapshot(*C.Stage);
				FHyperAIModelStep* Step = C.Info.Steps.IsValidIndex(C.StageStep) ? &C.Info.Steps[C.StageStep] : nullptr;
				if (Snap.State == TEXT("rejected"))
				{
					if (Step)
					{
						Step->Status = TEXT("declined");
					}
					const FString Kind = C.StageKind;
					C.Stage.Reset();
					if (Kind == TEXT("generate") && C.bReviewing)
					{
						// The picture was seen and the generation turned down.
						Finish(C, TEXT("fix_declined"));
					}
					else if (Kind == TEXT("generate") || Kind == TEXT("reference"))
					{
						Finish(C, TEXT("cancelled"));
					}
					else if (Kind == TEXT("rig"))
					{
						Finish(C, TEXT("checks_passed"));
					}
					else
					{
						Finish(C, TEXT("fix_declined"));
					}
					return;
				}
				if (Snap.State == TEXT("completed"))
				{
					StageDone(C);
					return;
				}
				if (Snap.State == TEXT("failed") || Snap.State == TEXT("cancelled"))
				{
					if (Step)
					{
						Step->Status = TEXT("failed");
						Step->Detail = Snap.Diagnostic.Left(1000);
					}
					const FString Kind = C.StageKind;
					const TSharedRef<FJob> Failed = C.Stage.ToSharedRef();
					C.Stage.Reset();
					if (Snap.State == TEXT("failed") && AskRetry(C, Failed, Kind))
					{
						return;
					}
					if (Kind == TEXT("generate") || Kind == TEXT("reference"))
					{
						C.Info.Diagnostic = Snap.Diagnostic;
						Finish(C, Snap.State == TEXT("cancelled") ? TEXT("cancelled") : TEXT("failed"));
					}
					else if (Kind == TEXT("rig"))
					{
						Finish(C, TEXT("checks_passed"));
					}
					else
					{
						// Tripo refunds a failed task; the ladder climbs past it next round.
						const int32 Tries = C.Tried.FindRef(C.StageFixId);
						C.Tried.Add(C.StageFixId, Tries + 1);
						++C.Info.Round;
						C.Info.State = TEXT("checking");
						Journal(C);
					}
					return;
				}
				if (Snap.State != TEXT("awaiting_user_approval"))
				{
					if (Step && (Step->Status == TEXT("awaiting_user_approval") || Step->TaskId != C.Stage->Info.TaskId))
					{
						// Journaled as soon as Tripo takes a task, so a restart asks after it rather than paying again.
						Step->Status = TEXT("running");
						Step->TaskId = C.Stage->Info.TaskId;
						Journal(C);
					}
					C.Info.State = C.StageKind == TEXT("generate") ? TEXT("generating") : C.StageKind == TEXT("reference") ? TEXT("making_reference")
						: C.StageKind == TEXT("rig") ? TEXT("rigging") : TEXT("fixing");
				}
				return;
			}
			if (C.Info.State == TEXT("checking"))
			{
				CheckRound(C);
			}
		}

		bool Tick(float)
		{
			TickJobs();
			return true;
		}

		void EnsureTicker()
		{
			static FTSTicker::FDelegateHandle Handle;
			if (!Handle.IsValid())
			{
				Handle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.25f);
			}
		}

		FString NewOperationId(const FString& Given, const TCHAR* Prefix)
		{
			return Given.IsEmpty() ? Prefix + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower() : Given;
		}

		bool TripoService(FHyperAIStudioExternalServiceSettings& Out)
		{
			return FHyperAIStudioExternalServices::Find(TEXT("tripo"), Out);
		}
	}

	using namespace JobDetail;

	bool IsAssetBusy(const FString& AssetPath)
	{
		const FString Package = PackageOf(AssetPath.TrimStartAndEnd());
		return !Package.IsEmpty() && Creators().ContainsByPredicate([&Package](const TSharedRef<FCreator>& C)
		{
			return !C->bFinished && !C->Info.AssetPath.IsEmpty() && PackageOf(C->Info.AssetPath) == Package;
		});
	}

	void TickJobs()
	{
		// A copy: a step may add a job.
		const TArray<TSharedRef<FCreator>> All = Creators();
		for (const TSharedRef<FCreator>& C : All)
		{
			Advance(*C);
		}
	}

	FHyperAIModelCreateReport Create(const FHyperAIModelCreateRequest& Request)
	{
		FHyperAIModelCreateReport Report;
		auto Refuse = [&Report](const TCHAR* Status, const FString& Diagnostic)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Diagnostic;
			return Report;
		};
		if (!IsInGameThread())
		{
			return Refuse(TEXT("game_thread_required"), TEXT("The model creator runs on the game thread."));
		}
		const UHyperAIStudioTripoSettings* Settings = GetDefault<UHyperAIStudioTripoSettings>();
		FHyperAIStudioExternalServiceSettings Service;
		if (!TripoService(Service))
		{
			return Refuse(TEXT("unknown_provider"), TEXT("No Tripo service is configured."));
		}
		FHyperAIStudioExternalServiceSettings Hi3D;
		FHyperAIStudioExternalServices::Find(TEXT("hi3d"), Hi3D);
		const bool bTripoKey = !Key(Service).IsEmpty();
		const bool bHi3DKey = !Hi3D.Id.IsEmpty() && !Key(Hi3D).IsEmpty();

		// Resume: pick a journaled job up where it stopped.
		const FString Resume = Request.ResumeOperationId.TrimStartAndEnd();
		if (!Resume.IsEmpty())
		{
			if (const TSharedPtr<FCreator> Running = FindCreator(Resume))
			{
				Report.bOk = true;
				Report.Status = Running->Info.State;
				Report.OperationId = Resume;
				Report.Plan = Running->Info.Plan;
				Report.Diagnostic = TEXT("That job is in this session already: follow it with hyper_model_status.");
				return Report;
			}
			const TSharedRef<FCreator> C = MakeShared<FCreator>();
			FString Error;
			if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Resume) || !ReadJournal(Resume, *C, Error))
			{
				return Refuse(TEXT("not_found"), Error.IsEmpty() ? FString(TEXT("resume_operation_id is not a journal-safe id.")) : Error);
			}
			if (C->Info.State == TEXT("completed") || C->Info.State == TEXT("stopped") || C->Info.State == TEXT("failed") || C->Info.State == TEXT("cancelled"))
			{
				Report.bOk = true;
				Report.Status = C->Info.State;
				Report.OperationId = Resume;
				Report.Plan = C->Info.Plan;
				Report.Diagnostic = FString::Printf(TEXT("That job finished (%s): %s"), *C->Info.State, *C->Info.Diagnostic);
				return Report;
			}
			C->Service = Service;
			C->Hi3D = Hi3D;
			C->bTripoKey = bTripoKey;
			const bool bHi3DPlan = C->Info.Plan.Provider == TEXT("hi3d");
			C->Targets = C->Info.Plan.Family.IsEmpty() ? TargetsForUse(FString(), FString(), 0.f, 0, FString()) : TargetsFromPlan(C->Info.Plan);
			if (bHi3DPlan ? !bHi3DKey : !bTripoKey)
			{
				C->bAllowPaid = false;
			}
			const int32 RunningIndex = C->Info.Steps.FindLastByPredicate([](const FHyperAIModelStep& Step)
			{
				return Step.Status == TEXT("running") || Step.Status == TEXT("awaiting_user_approval");
			});
			const FHyperAIModelStep* Running = RunningIndex == INDEX_NONE ? nullptr : &C->Info.Steps[RunningIndex];
			FString Note;
			if (C->Info.AssetPath.IsEmpty())
			{
				// The generation never finished here. Its task, when Tripo had taken it, is asked after again: nothing is paid twice.
				const FString TaskId = Running && Running->Kind == TEXT("generate") ? Running->TaskId : FString();
				const bool bPictureNeeded = C->Info.Plan.bReferenceFirst && C->Info.Plan.ReferenceSource == TEXT("tripo") && C->ReferenceTaskId.IsEmpty();
				if (TaskId.IsEmpty() && bPictureNeeded)
				{
					// The picture never finished here: asked for again (a picture Tripo had made is paid again; its task is not journaled until it is done).
					const TSharedRef<FJob> Job = ReferenceStage(*C, Resume + TEXT("-reference"));
					const float PictureUSD = ImagePrice(C->Info.Plan.ImageModel, C->Info.Plan.ImageQuality, C->Info.Plan.ImageSize);
					if (!AskStage(*C, Job, TEXT("Make the reference picture with Tripo (resumed)"), PictureUSD, TEXT("model.reference.v1"), C->Info.Plan.PriceLines, Error))
					{
						return Refuse(TEXT("approval_not_queued"), Error);
					}
					C->StageKind = TEXT("reference");
					C->StageStep = INDEX_NONE;
					C->Info.State = TEXT("awaiting_user_approval");
					Note = TEXT("Asked for the reference picture's approval again.");
				}
				else if (TaskId.IsEmpty())
				{
					const TSharedRef<FJob> Job = MakeGeneration(*C, Resume, C->Brief, false);
					if (!AskStage(*C, Job, FString::Printf(TEXT("Make the model with %s (resumed: the earlier generation was never approved)"), bHi3DPlan ? TEXT("Hi3D") : TEXT("Tripo")),
						ModelPrice(C->Info.Plan), TEXT("model.generate.v1"), C->Info.Plan.PriceLines, Error))
					{
						return Refuse(TEXT("approval_not_queued"), Error);
					}
					C->StageKind = TEXT("generate");
					C->StageStep = INDEX_NONE;
					C->Info.State = TEXT("awaiting_user_approval");
					Note = TEXT("Asked for the generation's approval again.");
				}
				else
				{
					const TSharedRef<FJob> Job = NewStage(*C, Resume + TEXT("-resume"), C->Folder, C->Name, bHi3DPlan);
					if (bHi3DPlan)
					{
						// Hi3D: a token first, then straight to asking after the task.
						bool bMultiview = false;
						FTask Poll = Hi3DTask(*C, TEXT("generate"), Hi3DRequestFromPlan(C->Info.Plan, Hi3DImagesOf(*C, bMultiview), bMultiview), 0.0);
						Poll.TaskId = TaskId;
						Job->Tasks.Add(MoveTemp(Poll));
						Job->Phase = EPhase::Prepare;
						Job->AuthReturn = EPhase::Generate;
					}
					else
					{
						FTask Poll = Task(*C, TEXT("generate"), C->Info.Plan.Endpoint, nullptr, 0.0, /*bLast=*/true);
						Poll.TaskId = TaskId;
						Job->Tasks.Add(MoveTemp(Poll));
						Job->Phase = EPhase::Generate;
					}
					Job->Price = 0.0;
					Jobs().Insert(Job, 0);
					Begin(Job);
					C->Stage = Job;
					C->StageKind = TEXT("generate");
					C->StageStep = C->Info.Steps.IndexOfByPredicate([&TaskId](const FHyperAIModelStep& Step) { return Step.TaskId == TaskId; });
					C->Info.State = TEXT("generating");
					C->Info.CurrentOperationId = Job->Info.OperationId;
					Note = FString::Printf(TEXT("Asking %s after its task %s again; nothing new is paid."), bHi3DPlan ? TEXT("Hi3D") : TEXT("Tripo"), *TaskId);
				}
			}
			else
			{
				// The asset is there: a paid fix waiting for approval is offered again from a fresh check, else the round runs again.
				for (FHyperAIModelStep& Step : C->Info.Steps)
				{
					if (Step.Status == TEXT("awaiting_user_approval") || Step.Status == TEXT("running"))
					{
						Step.Status = TEXT("skipped");
						Step.Detail += TEXT(" (interrupted; the job resumed from a new check)");
					}
				}
				C->Info.State = TEXT("checking");
				C->LocalRound = C->Info.Round;
				Note = TEXT("Resumed with a new check of the asset.");
			}
			Creators().Add(C);
			EnsureTicker();
			Journal(*C);
			Report.bOk = true;
			Report.Status = TEXT("resumed");
			Report.OperationId = Resume;
			Report.Plan = C->Info.Plan;
			Report.FixBudgetUSD = C->Info.FixBudgetUSD;
			Report.MaxRounds = C->Info.MaxRounds;
			Report.Diagnostic = Note + TEXT(" Follow it with hyper_model_status.");
			return Report;
		}

		FHyperAIModelPlan Plan;
		FString Error;
		if (!Classify(Request.Brief, Plan, Error, Request.bRig))
		{
			return Refuse(TEXT("invalid_request"), Error);
		}
		// Auto chose Hi3D, but its key is not set: Tripo instead, with the reason.
		const bool bAutoProvider = (Request.Brief.Provider.TrimStartAndEnd().IsEmpty() || Request.Brief.Provider.TrimStartAndEnd().Equals(TEXT("auto"), ESearchCase::IgnoreCase))
			&& !Hi3DIsModel(Request.Brief.Model) && Request.Brief.Resolution.TrimStartAndEnd().IsEmpty();
		if (Plan.Provider == TEXT("hi3d") && bAutoProvider && !bHi3DKey)
		{
			FHyperAIModelBrief Tripo = Request.Brief;
			Tripo.Provider = TEXT("tripo");
			FHyperAIModelPlan Instead;
			if (Classify(Tripo, Instead, Error, Request.bRig))
			{
				Instead.Reasons.Insert(FString::Printf(TEXT("provider: tripo after all; Hi3D would suit this brief (%s), but its key (%s) is not set"),
					*Plan.Reasons[0].Replace(TEXT("provider: hi3d, "), TEXT("")), Hi3D.ApiKeyVariable.IsEmpty() ? TEXT("HITEM3D_API_KEY") : *Hi3D.ApiKeyVariable), 0);
				Instead.Reasons.RemoveAt(1);
				Plan = Instead;
			}
		}
		// Hi3D from words without Tripo's key: HyperAI's own image generation draws the picture.
		if (Plan.Provider == TEXT("hi3d") && Plan.bReferenceFirst && !bTripoKey)
		{
			UseImageGenReference(Plan);
		}
		Report.Plan = Plan;
		const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		auto CheckPicture = [&ProjectDir, &Plan](const FString& Path, FString& OutAbsolute)
		{
			OutAbsolute = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? ProjectDir / Path : Path);
			const FString Extension = FPaths::GetExtension(OutAbsolute).ToLower();
			const int64 Size = IFileManager::Get().FileSize(*OutAbsolute);
			return FPaths::IsUnderDirectory(OutAbsolute, ProjectDir) && Size > 0 && Size <= FHyperAIStudioMeshGenContracts::MaxImageBytes
				&& (Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg") || (Extension == TEXT("webp") && Plan.Provider == TEXT("hi3d")));
		};
		FString Reference;
		if ((Plan.Input == TEXT("image") || Plan.bMakeViews) && !Plan.bReferenceFirst && !CheckPicture(Request.Brief.ImagePath.TrimStartAndEnd(), Reference))
		{
			return Refuse(TEXT("invalid_image"), Plan.Provider == TEXT("hi3d") ? TEXT("image_path is a PNG, JPEG or WebP inside the project, up to 20 MB.")
				: TEXT("image_path is a PNG or JPEG inside the project, up to 20 MB."));
		}
		for (const FString& View : Request.Brief.MultiviewImages)
		{
			FString Absolute;
			if (!View.TrimStartAndEnd().IsEmpty() && !CheckPicture(View.TrimStartAndEnd(), Absolute))
			{
				return Refuse(TEXT("invalid_image"), TEXT("Each view is a PNG or JPEG inside the project, up to 20 MB."));
			}
			Reference = Reference.IsEmpty() ? Absolute : Reference;
		}
		FString Folder = Request.Folder.TrimStartAndEnd().IsEmpty() ? GetDefault<UHyperAIStudioSettings>()->MeshImportFolder : Request.Folder.TrimStartAndEnd();
		Folder.RemoveFromEnd(TEXT("/"));
		FString Reason;
		if ((!Folder.StartsWith(TEXT("/Game/")) && Folder != TEXT("/Game")) || !FHyperAIStudioNames::IsFolderPath(Folder, TEXT("folder"), &Reason))
		{
			return Refuse(TEXT("invalid_request"), TEXT("folder is a content path under /Game, such as /Game/Generated/Models.") + (Reason.IsEmpty() ? FString() : TEXT(" ") + Reason));
		}
		const FString Name = FreeName(Folder, AssetNameFor(Request.Name, Request.Brief.Prompt.IsEmpty() ? Plan.Use : Request.Brief.Prompt));
		if (!FHyperAIStudioNames::IsPackagePath(Folder / Name / Name, TEXT("The folder and name together"), &Reason))
		{
			return Refuse(TEXT("invalid_request"), Reason + TEXT(" Give a shorter folder or name."));
		}
		if (!Request.RetargetTo.TrimStartAndEnd().IsEmpty() && !LoadObject<USkeletalMesh>(nullptr, *Request.RetargetTo.TrimStartAndEnd(), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Refuse(TEXT("invalid_request"), TEXT("retarget_to is a skeletal mesh in the project, such as the UE5 Mannequin."));
		}
		Report.Destination = Folder / Name;
		Report.FixBudgetUSD = Request.bLocalFixesOnly ? 0.f : Request.FixBudgetUSD > 0.f ? Request.FixBudgetUSD : Settings->FixBudgetUSD;
		Report.MaxRounds = Request.MaxRounds > 0 ? FMath::Min(Request.MaxRounds, 10) : Settings->MaxFixRounds;
		Report.OperationId = NewOperationId(Request.OperationId, TEXT("model-"));
		if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Report.OperationId))
		{
			return Refuse(TEXT("invalid_operation_id"), TEXT("operation_id must be a short journal-safe id, or empty to have one made."));
		}
		if (FindCreator(Report.OperationId).IsValid() || FindJob(Report.OperationId).IsValid())
		{
			return Refuse(TEXT("operation_id_used"), TEXT("That operation_id already names a job; pass a new one or leave it empty."));
		}
		const bool bHi3DPlan = Plan.Provider == TEXT("hi3d");
		const FString Price_ = FString::Printf(TEXT("%d %s ($%.2f in all%s) for the generation, then paid fixes only as needed, each approved, up to $%.2f in %d round(s)"),
			Plan.Credits, bHi3DPlan ? TEXT("Hi3D credits") : TEXT("Tripo API credits"), Plan.EstimatedCost, Plan.bReferenceFirst ? TEXT(", the reference picture included") : TEXT(""),
			Report.FixBudgetUSD, Report.MaxRounds);
		const FString Shape = bHi3DPlan ? FString::Printf(TEXT("Hi3D (%s at %s, %d faces asked%s, %s%s)"), *Plan.Model, *Plan.Resolution, Plan.ProviderFaces,
				Plan.bLocalReduce ? *FString::Printf(TEXT(", reduced in the editor to %d"), Plan.FaceLimit) : TEXT(""), Plan.TextureQuality == TEXT("none") ? TEXT("geometry only") : TEXT("textured"),
				Plan.bPbr ? TEXT(", PBR") : TEXT(""))
			: FString::Printf(TEXT("%s (%s, %d %s, %s textures%s%s%s)"), *Plan.Family, *Plan.Model, Plan.FaceLimit, Plan.bQuad ? TEXT("quads") : TEXT("triangles"), *Plan.TextureQuality,
				Plan.bPbr ? TEXT(", PBR") : TEXT(""), Plan.bParts ? TEXT(", parts") : TEXT(""), Plan.bSmartLowPoly ? TEXT(", smart low poly") : TEXT(""));
		const FString Picture = Plan.bReferenceFirst ? FString::Printf(TEXT(" from a reference picture made first (%s%s)"), *Plan.ImageModel,
			Plan.bReviewReference ? TEXT(", which you approve and see before the 3D is asked for") : TEXT("")) : FString();
		if (Request.bDryRun)
		{
			Report.bOk = true;
			Report.Status = TEXT("checked");
			Report.Diagnostic = FString::Printf(TEXT("Would make it with %s%s: %s. Imported into %s. Nothing was spent; resubmit with bDryRun false to ask the user."),
				*Shape, *Picture, *Price_, *Report.Destination);
			return Report;
		}
		if (!bHi3DPlan && !bTripoKey)
		{
			return Refuse(TEXT("missing_key"), FString::Printf(TEXT("Tripo's API key is not set: add it in Settings > Agents > Keys, or set %s. Tripo Studio's own credits cannot be used through the API; ")
				TEXT("a model made in Tripo Studio can be sent with Tripo's DCC Bridge and checked with hyper_model_check_and_fix."), *Service.ApiKeyVariable));
		}
		if (bHi3DPlan && !bHi3DKey)
		{
			return Refuse(TEXT("missing_key"), FString::Printf(TEXT("Hi3D's key is not set: from Hi3D's developer platform (API keys), add client_id:client_secret in Settings > Agents > Keys ")
				TEXT("as Hi3D (Hitem3D), or set %s (or HYPERAI_HI3D_KEY). Or choose provider tripo."), Hi3D.ApiKeyVariable.IsEmpty() ? TEXT("HITEM3D_API_KEY") : *Hi3D.ApiKeyVariable));
		}
		if (bHi3DPlan && Request.bRig && !bTripoKey)
		{
			return Refuse(TEXT("missing_key"), TEXT("Rigging is Tripo's: set Tripo's API key, or leave bRig off."));
		}
		if (Plan.ReferenceSource == TEXT("imagegen"))
		{
			FHyperAIStudioImageModelSettings Image;
			FHyperAIStudioExternalServiceSettings ImageService;
			if (!ReferenceImageModel(Image) || !FHyperAIStudioExternalServices::Find(Image.ServiceId, ImageService) || !FHyperAIStudioExternalServices::HasApiKey(ImageService))
			{
				return Refuse(TEXT("missing_key"), TEXT("Hi3D works from a picture, and none can be made: set Tripo's API key (its text-to-image) or an image service's key, or give image_path."));
			}
		}

		const TSharedRef<FCreator> C = MakeShared<FCreator>();
		C->Info.OperationId = Report.OperationId;
		C->Info.Kind = TEXT("create");
		C->Info.Provider = Plan.Provider;
		C->Info.Plan = Plan;
		C->Info.FixBudgetUSD = Report.FixBudgetUSD;
		C->Info.MaxRounds = Report.MaxRounds;
		C->Brief = Request.Brief;
		C->Service = Service;
		C->Hi3D = Hi3D;
		C->bTripoKey = bTripoKey;
		if (!Plan.MatchAsset.IsEmpty())
		{
			// The reference the brief matched becomes the project's weapon convention: later weapons follow it untold.
			FWeaponFinish Matched;
			TArray<FString> Kept;
			if (MatchReference(Plan.MatchAsset, Matched, Kept, Error, /*bKeep=*/true) && !Kept.IsEmpty())
			{
				C->Info.Plan.Reasons.Add(Kept.Last());
			}
		}
		C->Targets = TargetsFromPlan(Plan);
		C->Folder = Folder;
		C->Name = Name;
		C->ReferenceImage = Reference;
		C->bLocalOnly = Request.bLocalFixesOnly;
		C->bRig = Request.bRig;
		C->RetargetTo = Request.RetargetTo.TrimStartAndEnd();
		AddStep(*C, TEXT("classify"), bHi3DPlan ? Shape : FString::Printf(TEXT("%s, %d %s, %s textures"), *Plan.Family, Plan.FaceLimit, Plan.bQuad ? TEXT("quads") : TEXT("triangles"), *Plan.TextureQuality),
			TEXT("done"), 0.f, FString::Join(Plan.Reasons, TEXT("; ")));
		const FString Source = Plan.bReferenceFirst ? FString::Printf(TEXT("a reference picture made from the words \"%s\""), *Request.Brief.Prompt.Left(100))
			: Plan.Input == TEXT("text") ? FString::Printf(TEXT("words: \"%s\""), *Request.Brief.Prompt.Left(100))
			: FString::Printf(TEXT("%s picture(s)"), Plan.Input == TEXT("image") ? TEXT("one") : TEXT("multiview"));
		const FString What = FString::Printf(TEXT("Make a %s with %s from %s; check and fix it in the editor; import it into %s"), *Plan.Use, *Shape, *Source, *Report.Destination);
		TArray<FString> Lines = Plan.PriceLines;
		Lines.Add(FString::Printf(TEXT("later paid fixes: each asked for on its own, up to $%.2f in %d round(s)"), Report.FixBudgetUSD, Report.MaxRounds));
		const bool bPictureStage = Plan.bReferenceFirst && Plan.ReferenceSource == TEXT("tripo");
		const float PictureUSD = bPictureStage ? ImagePrice(Plan.ImageModel, Plan.ImageQuality, Plan.ImageSize) : 0.f;
		if (bPictureStage)
		{
			// The picture first: on its own (reviewed: the 3D is asked for once it is seen), or both in this one approval.
			const TSharedRef<FJob> Job = ReferenceStage(*C, Report.OperationId);
			const FString PictureWhat = Plan.bReviewReference ? FString::Printf(TEXT("Make the reference picture for a %s with Tripo (%s); you see it before the 3D is asked for"), *Plan.Use, *Plan.ImageModel)
				: What;
			if (Plan.bReviewReference)
			{
				Lines.Insert(TEXT("now: the reference picture only; the 3D generation is asked for after you see it"), 0);
			}
			if (!AskStage(*C, Job, PictureWhat, Plan.bReviewReference ? PictureUSD : Plan.EstimatedCost, TEXT("model.reference.v1"), Lines, Error))
			{
				return Refuse(Error.Contains(TEXT("cap")) ? TEXT("over_spend_cap") : TEXT("approval_not_queued"), Error);
			}
			// The stage's job books what it runs: the picture; the 3D is booked by its own stage.
			Job->Price = PictureUSD;
			C->StageKind = TEXT("reference");
			C->StageStep = AddStep(*C, TEXT("reference"), PictureWhat, TEXT("awaiting_user_approval"), PictureUSD, JsonText(TextToImageBody(Plan)));
			C->Info.Steps[C->StageStep].Endpoint = TEXT("/generation/text-to-image");
		}
		else
		{
			const TSharedRef<FJob> Job = MakeGeneration(*C, Report.OperationId, Request.Brief, false);
			if (!AskStage(*C, Job, What, Plan.EstimatedCost, TEXT("model.generate.v1"), Lines, Error))
			{
				return Refuse(Error.Contains(TEXT("cap")) ? TEXT("over_spend_cap") : TEXT("approval_not_queued"), Error);
			}
			C->StageKind = TEXT("generate");
			C->StageStep = AddStep(*C, TEXT("generate"), What, TEXT("awaiting_user_approval"), Plan.EstimatedCost, Plan.RequestJson);
			C->Info.Steps[C->StageStep].Credits = ProviderCredits(Plan.Provider, ModelPrice(Plan));
			C->Info.Steps[C->StageStep].Endpoint = Plan.Endpoint;
		}
		C->Info.Steps[C->StageStep].OperationId = Report.OperationId;
		C->Info.State = TEXT("awaiting_user_approval");
		Creators().Insert(C, 0);
		EnsureTicker();
		Journal(*C);
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = FString::Printf(TEXT("Waiting for the user's approval in HyperAI Chat, Activity: %s%s. Then it generates, imports, checks and fixes in a few minutes: ")
			TEXT("follow operation_id with hyper_operation_wait (bWaitThroughApproval), then hyper_model_status, whose current_operation_id names whatever waits for approval next ")
			TEXT("(the 3D generation after a reviewed picture, or a paid fix)."), bPictureStage && Plan.bReviewReference ? TEXT("the reference picture first; ") : TEXT(""), *Price_);
		FHyperAIStudioApprovalGate::DescribeHeld(Report.OperationId, Report.Status, Report.Diagnostic);
		return Report;
	}

	FHyperAIModelStatusReport Status(const FString& OperationId)
	{
		FHyperAIModelStatusReport Report;
		for (const TSharedRef<FCreator>& C : Creators())
		{
			if (OperationId.IsEmpty() || C->Info.OperationId == OperationId)
			{
				Report.Jobs.Add(C->Info);
			}
		}
		if (Report.Jobs.IsEmpty() && !OperationId.IsEmpty())
		{
			// A job from before the editor restarted: its journal.
			FCreator C;
			FString Error;
			if (FHyperAIStudioExtensionRuntime::IsValidOperationId(OperationId) && ReadJournal(OperationId, C, Error))
			{
				C.Info.Diagnostic = TEXT("From its journal (not running in this session; hyper_model_create with resume_operation_id picks it up). ") + C.Info.Diagnostic;
				Report.Jobs.Add(C.Info);
			}
		}
		Report.bOk = OperationId.IsEmpty() || !Report.Jobs.IsEmpty();
		Report.Diagnostic = Report.bOk ? FString::Printf(TEXT("%d model job(s)."), Report.Jobs.Num()) : TEXT("No model job has that operation_id.");
		return Report;
	}

	static FHyperAIModelCheckFixReport CheckAndFixImpl(const FHyperAIModelCheckFixRequest& Request, const bool bPreApproved)
	{
		FHyperAIModelCheckFixReport Report;
		auto Refuse = [&Report](const TCHAR* Status, const FString& Diagnostic)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Diagnostic;
			return Report;
		};
		if (!IsInGameThread())
		{
			return Refuse(TEXT("game_thread_required"), TEXT("Checks run on the game thread."));
		}
		UObject* Mesh = LoadMesh(Request.AssetPath.TrimStartAndEnd());
		if (!Mesh)
		{
			return Refuse(TEXT("invalid_request"), TEXT("asset_path is a static or skeletal mesh in the project, such as /Game/Tripo/SM_Chair.SM_Chair."));
		}
		FString Use = Request.Use.TrimStartAndEnd().ToLower();
		if (Use.IsEmpty())
		{
			Use = UseFromWords(Mesh->GetName().Replace(TEXT("_"), TEXT(" ")));
		}
		// A reference to match: measured, applied, and (when this run changes anything) kept as the project's convention.
		FWeaponFinish Matched;
		TArray<FString> MatchLines;
		const FString Match = Request.MatchAsset.TrimStartAndEnd();
		if (!Match.IsEmpty())
		{
			FString MatchError;
			if (!MatchReference(Match, Matched, MatchLines, MatchError, /*bKeep=*/!Request.bCheckOnly || bPreApproved))
			{
				return Refuse(TEXT("invalid_request"), MatchError);
			}
			Use = Use.IsEmpty() || Use == TEXT("prop") ? FString(TEXT("weapon")) : Use;
		}
		FCheckTargets Targets = TargetsForUse(Use, Request.Platform, Request.SizeMeters, Request.FaceLimit > 0 ? Request.FaceLimit : Match.IsEmpty() ? 0 : Matched.TargetTris, Request.Topology);
		if (!Match.IsEmpty() && Targets.bWeapon)
		{
			// Its size as asked, else the reference's length.
			const float Length = Request.SizeMeters > 0.f || Matched.LengthCm <= 0.f ? Targets.Weapon.LengthCm : Matched.LengthCm;
			Targets.Weapon = Matched;
			Targets.Weapon.LengthCm = Length;
			Targets.SizeMeters = Length / 100.f;
			Targets.Weapon.TargetTris = Targets.FaceLimit > 0 ? Targets.FaceLimit : Matched.TargetTris;
			Targets.bNanite = Matched.bNanite;
		}
		const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		if (!Request.ReferenceImagePath.TrimStartAndEnd().IsEmpty())
		{
			const FString Path = Request.ReferenceImagePath.TrimStartAndEnd();
			Targets.ReferenceImage = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? ProjectDir / Path : Path);
			if (!FPaths::IsUnderDirectory(Targets.ReferenceImage, ProjectDir) || IFileManager::Get().FileSize(*Targets.ReferenceImage) <= 0)
			{
				return Refuse(TEXT("invalid_image"), TEXT("reference_image_path is a picture inside the project."));
			}
		}
		Report.OperationId = NewOperationId(Request.OperationId, TEXT("check-"));
		if ((!Request.bCheckOnly || bPreApproved) && IsAssetBusy(Mesh->GetPathName()))
		{
			// One job changes an asset at a time: a second would race the first's fixes and copies.
			const TSharedRef<FCreator>* Holder = Creators().FindByPredicate([Package = Mesh->GetOutermost()->GetName()](const TSharedRef<FCreator>& C)
			{
				return !C->bFinished && PackageOf(C->Info.AssetPath) == Package;
			});
			return Refuse(TEXT("asset_busy"), FString::Printf(TEXT("%s is already being checked or fixed by %s (%s): follow it with hyper_model_status, or stop it with hyper_mesh_cancel, then ask again."),
				*Mesh->GetPathName(), Holder ? *(*Holder)->Info.OperationId : TEXT("another job"), Holder ? *(*Holder)->Info.State : TEXT("running")));
		}
		if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Report.OperationId) || FindCreator(Report.OperationId).IsValid())
		{
			return Refuse(TEXT("invalid_operation_id"), TEXT("operation_id must be a new, short journal-safe id, or empty to have one made."));
		}
		FCheckTargets Measure = Targets;
		Measure.CaptureFolder = ModelsRoot() / Report.OperationId / TEXT("check");
		FString Error;
		if (!Inspect(*Mesh, Measure, Report.Check, Error))
		{
			return Refuse(TEXT("invalid_request"), Error);
		}
		Report.LocalFixes = LocalFixesFor(Report.Check);
		FHyperAIStudioExternalServiceSettings Service;
		const bool bKey = TripoService(Service) && !Key(Service).IsEmpty();
		if (bKey)
		{
			FFixContext Context;
			Context.InputToken = TEXT("{input}");
			Context.bHasTextures = !Report.Check.Checks.ContainsByPredicate([](const FHyperAIModelCheck& Check) { return Check.Id == TEXT("textures") && Check.Result == TEXT("fail"); });
			Context.bSkeletal = Cast<USkeletalMesh>(Mesh) != nullptr;
			Context.RemainingUSD = Request.FixBudgetUSD > 0.f ? Request.FixBudgetUSD : GetDefault<UHyperAIStudioTripoSettings>()->FixBudgetUSD;
			for (const FPaidFix& Fix : PaidLadder(Report.Check, Context))
			{
				Report.PaidFixes.Add(FString::Printf(TEXT("%s: %s, %d credits ($%.2f)"), *Fix.Id, *Fix.What, Credits(Fix.PriceUSD), Fix.PriceUSD));
			}
		}
		if (Request.bCheckOnly && !bPreApproved)
		{
			Report.bOk = true;
			Report.Status = TEXT("checked");
			Report.Diagnostic = Report.Check.Summary + (Report.LocalFixes.IsEmpty() ? FString(TEXT(" No free fix applies.")) : FString::Printf(TEXT(" Free fixes it would make: %s; resubmit with bCheckOnly false to ask the user."), *FString::Join(Report.LocalFixes, TEXT(", "))))
				+ (bKey ? FString() : FString(TEXT(" Paid Tripo fixes need the Tripo API key.")));
			return Report;
		}
		const TSharedRef<FCreator> C = MakeShared<FCreator>();
		C->Info.OperationId = Report.OperationId;
		C->Info.Kind = TEXT("check_and_fix");
		C->Info.Provider = TEXT("tripo");
		C->Info.AssetPath = Mesh->GetPathName();
		C->Info.FixBudgetUSD = Request.bAllowPaidFixes ? (Request.FixBudgetUSD > 0.f ? Request.FixBudgetUSD : GetDefault<UHyperAIStudioTripoSettings>()->FixBudgetUSD) : 0.f;
		C->Info.MaxRounds = Request.MaxRounds > 0 ? FMath::Min(Request.MaxRounds, 10) : GetDefault<UHyperAIStudioTripoSettings>()->MaxFixRounds;
		C->Info.FirstCheck = Report.Check;
		C->Info.Plan.Use = Use;
		// The use's budget, so a paid retopology aims at it rather than at Tripo's own ceiling.
		C->Info.Plan.FaceLimit = Targets.FaceLimit;
		C->Info.Plan.MinFaces = Targets.MinFaces;
		C->Service = Service;
		C->Targets = Targets;
		C->ReferenceImage = Targets.ReferenceImage;
		C->bAllowPaid = Request.bAllowPaidFixes && bKey;
		C->bLocalOnly = !C->bAllowPaid;
		C->bLocalApproved = false;
		C->Info.State = TEXT("awaiting_user_approval");
		C->Info.CurrentOperationId = Report.OperationId;
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = FHyperAIStudioMeshGenContracts::PackId;
		Summary.ToolName = CheckFixToolName;
		Summary.VariantId = TEXT("model.check_and_fix.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = C->Info.AssetPath;
		Summary.Effects = {FString::Printf(TEXT("Check %s and make the free fixes in the editor: %s"), *C->Info.AssetPath,
			Report.LocalFixes.IsEmpty() ? TEXT("none needed now") : *FString::Join(Report.LocalFixes, TEXT(", "))), Report.Check.Summary};
		if (C->bAllowPaid)
		{
			Summary.Effects.Add(FString::Printf(TEXT("Paid Tripo fixes, if checks still fail, are each priced and asked for on their own (up to $%.2f)"), C->Info.FixBudgetUSD));
		}
		Summary.Touches = {C->Info.AssetPath};
		if (bPreApproved)
		{
			// The user approved this already (the Tripo Studio offer): it starts at once.
			C->bLocalApproved = true;
			// Started from the watch's offer, which only a click approves: anything it asks for later waits for a click as well.
			C->bClickOnly = true;
			C->Info.State = TEXT("checking");
			C->Info.CurrentOperationId.Reset();
			AddStep(*C, TEXT("check"), Report.Check.Summary, Report.Check.bPassed ? TEXT("done") : TEXT("failed"));
			Creators().Insert(C, 0);
			EnsureTicker();
			Journal(*C);
			Report.bOk = true;
			Report.Status = TEXT("queued");
			Report.Diagnostic = Report.Check.Summary + TEXT(" Approved; the free fixes run now. Follow it with hyper_model_status.");
			return Report;
		}
		const TWeakPtr<FCreator> Weak = C;
		if (!FHyperAIStudioApprovalGate::RequestAction(Report.OperationId, Summary, [Weak]()
		{
			if (const TSharedPtr<FCreator> Pinned = Weak.Pin())
			{
				Pinned->bLocalApproved = true;
				Pinned->Info.State = TEXT("checking");
				Pinned->Info.CurrentOperationId.Reset();
				Journal(*Pinned);
			}
		}, Error))
		{
			return Refuse(TEXT("approval_not_queued"), Error);
		}
		AddStep(*C, TEXT("check"), Report.Check.Summary, Report.Check.bPassed ? TEXT("done") : TEXT("failed"));
		Creators().Insert(C, 0);
		EnsureTicker();
		Journal(*C);
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = FString::Printf(TEXT("%s Waiting for the user's approval in HyperAI Chat, Activity to make the free fixes; then follow it with hyper_model_status."), *Report.Check.Summary);
		FHyperAIStudioApprovalGate::DescribeHeld(Report.OperationId, Report.Status, Report.Diagnostic);
		return Report;
	}

	FHyperAIModelCheckFixReport CheckAndFix(const FHyperAIModelCheckFixRequest& Request)
	{
		return CheckAndFixImpl(Request, false);
	}

	bool StartApprovedCheckAndFix(const FHyperAIModelCheckFixRequest& Request, FString& OutError)
	{
		const FHyperAIModelCheckFixReport Report = CheckAndFixImpl(Request, true);
		OutError = Report.bOk ? FString() : Report.Diagnostic;
		return Report.bOk;
	}

#if WITH_DEV_AUTOMATION_TESTS
	void ForgetForTests(const FString& OperationId)
	{
		const TSharedPtr<FCreator> C = FindCreator(OperationId);
		if (!C.IsValid())
		{
			return;
		}
		if (C->Stage.IsValid() && !C->Stage->JobId.IsEmpty())
		{
			FString Error;
			FHyperAIStudioAsyncJobHost::Cancel(C->Stage->JobId, TEXT("The editor closed (test)."), Error);
		}
		Creators().RemoveAll([&OperationId](const TSharedRef<FCreator>& Item) { return Item->Info.OperationId == OperationId; });
	}
#endif

	bool Cancel(const FString& OperationId, FHyperAIMeshCancelReport& OutReport)
	{
		const TSharedPtr<FCreator> C = FindCreator(OperationId.TrimStartAndEnd());
		if (!C.IsValid())
		{
			return false;
		}
		if (C->bFinished)
		{
			OutReport.Status = TEXT("already_finished");
			OutReport.Diagnostic = FString::Printf(TEXT("The job is %s; there is nothing to stop."), *C->Info.State);
			return true;
		}
		if (FHyperAIStudioApprovalGate::HasPending(C->Info.OperationId))
		{
			FHyperAIStudioApprovalGate::Reject(C->Info.OperationId, TEXT("Cancelled by the agent before it was approved."));
		}
		FString Note = TEXT("Nothing was waiting at Tripo.");
		if (C->Stage.IsValid())
		{
			if (FHyperAIStudioApprovalGate::HasPending(C->Stage->Info.OperationId))
			{
				FHyperAIStudioApprovalGate::Reject(C->Stage->Info.OperationId, TEXT("Cancelled by the agent before it was approved; nothing was spent."));
			}
			else if (!C->Stage->JobId.IsEmpty() && C->Stage->Phase != EPhase::Done && C->Stage->Phase != EPhase::Failed)
			{
				C->Stage->bCancelRequested = true;
				FString Error;
				FHyperAIStudioAsyncJobHost::Cancel(C->Stage->JobId, TEXT("Cancelled by the agent."), Error);
				Note = TEXT("A task Tripo had already taken runs on there; its credits are Tripo's to settle (a cancelled task is refunded, a finished one is not).");
			}
			C->Stage.Reset();
		}
		for (FHyperAIModelStep& Step : C->Info.Steps)
		{
			if (Step.Status == TEXT("awaiting_user_approval") || Step.Status == TEXT("running"))
			{
				Step.Status = TEXT("cancelled");
			}
		}
		Finish(*C, TEXT("cancelled"));
		OutReport.bOk = true;
		OutReport.Status = TEXT("cancelled");
		OutReport.Diagnostic = TEXT("Stopped. ") + Note;
		return true;
	}
}
