// Games by Hyper 2026.

#include "HyperAIStudioModelCreator.h"

#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioMeshGenJob.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioHi3D, Log, All);

/**
 * Hi3D (Hitem3D, by Math Magic) in the model creator, as docs.hi3d.ai documents its open API (read 31 October 2026): the
 * key is client_id:client_secret, traded at /open-api/v1/auth/token (Basic) for an access token every later call sends
 * as a Bearer; a model is one multipart submit-task, followed with query-task until it is a success, whose model and
 * cover links last an hour. The token's lifetime is not documented: it is kept for the session and asked for again when
 * Hi3D refuses it. The key, the token and the Authorization header are never logged, journaled or returned.
 */
namespace HyperAIStudio::MeshGen::Creator
{
	using namespace HyperAIStudio::MeshGen::Private;

	namespace Hi3DDetail
	{
		struct FHi3DModelRow
		{
			const TCHAR* Model;
			TArray<const TCHAR*> Resolutions;
		};

		const TArray<FHi3DModelRow>& Hi3DModelTable()
		{
			// The docs print portrait v2.1's pair once as "1536profast": read as 1536fast and 1536pro, as v2.1 has.
			static const TArray<FHi3DModelRow> Rows = {
				{TEXT("hi3dv3.0"), {TEXT("2048quality"), TEXT("2048master")}},
				{TEXT("hitem3dv2.1"), {TEXT("1536fast"), TEXT("1536pro")}},
				{TEXT("hitem3dv2.0"), {TEXT("1536"), TEXT("1536pro")}},
				{TEXT("hitem3dv1.5"), {TEXT("512"), TEXT("1024"), TEXT("1536"), TEXT("1536pro")}},
				{TEXT("scene-portraitv2.1"), {TEXT("1536fast"), TEXT("1536pro")}},
				{TEXT("scene-portraitv2.0"), {TEXT("1536pro")}},
				{TEXT("scene-portraitv1.5"), {TEXT("1536")}},
			};
			return Rows;
		}

		/** The session's access token, and a hash of the key it was given for (a new key needs a new token). */
		struct FHi3DTokenCache
		{
			FString Token;
			FString KeyHash;
		};

		FHi3DTokenCache& Hi3DTokenCache()
		{
			static FHi3DTokenCache Value;
			return Value;
		}

		FString Hi3DKeyHash(const FString& Key)
		{
			return FMD5::HashAnsiString(*Key);
		}

		FString Hi3DExtension(const FString& Path)
		{
			return FPaths::GetExtension(Path).ToLower();
		}

		/** What Hi3D said went wrong, its code put into words, in a line that never carries the key or the token. */
		FString Hi3DError(const FJob& Job, const int32 Code, const TArray<uint8>& Body)
		{
			const TSharedPtr<FJsonObject> Json = ParseJson(Body);
			const FString ProviderCode = Field(Json, TEXT("code"));
			const FString Meaning = Hi3DCodeMessage(ProviderCode);
			if (Meaning.IsEmpty())
			{
				return ProviderError(Job, Code, Body);
			}
			return FString::Printf(TEXT("%s answered %d (code %s): %s"), *Job.Service.Label, Code, *ProviderCode, *Meaning);
		}

		void Hi3DAfterToken(FJob& Job)
		{
			const EPhase Next = Job.AuthReturn != EPhase::Prepare ? Job.AuthReturn
				: !Job.ExportMesh.IsEmpty() && Job.ExportFile.IsEmpty() ? EPhase::Export : EPhase::Submit;
			Job.AuthReturn = EPhase::Prepare;
			Job.Phase = Next;
			Job.Info.State = Next == EPhase::Export ? TEXT("exporting") : Next == EPhase::Generate ? TEXT("generating") : TEXT("submitting");
		}

		/** A refused token: forget it, get a new one, and send again from Return; twice in a row is the key's fault. */
		bool Hi3DReauth(FJob& Job, const int32 Code, const TArray<uint8>& Body, const EPhase Return)
		{
			if (!Hi3DIsAuthError(Code, Field(ParseJson(Body), TEXT("code"))))
			{
				Job.AuthTries = 0;
				return false;
			}
			Hi3DTokenCache().Token.Reset();
			Job.Token.Reset();
			if (++Job.AuthTries > 2)
			{
				Fail(Job, Hi3DError(Job, Code, Body) + TEXT(" Hi3D refused a new access token too: check the key (client_id:client_secret) in Settings > Agents > Keys."));
				return true;
			}
			Job.AuthReturn = Return;
			Job.Phase = EPhase::Prepare;
			Job.Info.State = TEXT("authorising");
			return true;
		}

		// ------------------------------------------------------------ the reference picture without Tripo

		/** The base64 picture at a dotted path such as candidates.0.content.parts.*.inlineData.data; * takes the first that has the rest. */
		FString Hi3DImageAt(const TSharedPtr<FJsonValue>& Value, const TArray<FString>& Parts, const int32 Index)
		{
			if (!Value.IsValid())
			{
				return FString();
			}
			if (Index >= Parts.Num())
			{
				FString Text;
				return Value->TryGetString(Text) ? Text : FString();
			}
			const FString& Part = Parts[Index];
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (Value->TryGetArray(Array))
			{
				if (Part == TEXT("*"))
				{
					for (const TSharedPtr<FJsonValue>& Item : *Array)
					{
						const FString Found = Hi3DImageAt(Item, Parts, Index + 1);
						if (!Found.IsEmpty())
						{
							return Found;
						}
					}
					return FString();
				}
				const int32 At = FCString::Atoi(*Part);
				return Array->IsValidIndex(At) ? Hi3DImageAt((*Array)[At], Parts, Index + 1) : FString();
			}
			const TSharedPtr<FJsonObject>* Object = nullptr;
			return Value->TryGetObject(Object) && (*Object)->HasField(Part) ? Hi3DImageAt((*Object)->TryGetField(Part), Parts, Index + 1) : FString();
		}

		FString Hi3DJsonEscaped(const FString& Text)
		{
			// As JSON wants it inside a string: the template's quotes stay its own.
			return Text.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\"")).Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\t"), TEXT("\\t"));
		}

		bool FindImageModel(const FString& Id, FHyperAIStudioImageModelSettings& Out)
		{
			const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
			TArray<FHyperAIStudioImageModelSettings> All = Settings ? Settings->ImageModels : TArray<FHyperAIStudioImageModelSettings>();
			All.Append(UHyperAIStudioSettings::GetNanoBananaModels());
			for (const FHyperAIStudioImageModelSettings& Model : All)
			{
				if (Id.IsEmpty() || Model.Id == Id)
				{
					Out = Model;
					return true;
				}
			}
			return false;
		}

		/** The picture made with HyperAI's image generation (no Tripo key), into the job's folder; then the token. */
		void MakeImageGenReference(const TSharedRef<FJob>& Job)
		{
			FHyperAIStudioImageModelSettings Model;
			if (!FindImageModel(Job->ReferenceModel, Model))
			{
				Fail(*Job, FString::Printf(TEXT("The image model %s for the reference picture is not configured. Nothing was spent."), *Job->ReferenceModel));
				return;
			}
			FString Body = Model.RequestTemplate;
			Body = Body.Replace(TEXT("{prompt}"), *Hi3DJsonEscaped(Job->ReferencePrompt)).Replace(TEXT("{negative}"), TEXT("text, watermark, cropped, several objects"))
				.Replace(TEXT("{width}"), *FString::FromInt(Model.Width)).Replace(TEXT("{height}"), *FString::FromInt(Model.Height))
				.Replace(TEXT("{seed}"), *FString::FromInt(FMath::Rand() % 100000)).Replace(TEXT("{model}"), *Model.Model)
				.Replace(TEXT("{aspect_ratio}"), TEXT("1:1")).Replace(TEXT("{image_size}"), TEXT("1K"));
			Job->bWaiting = true;
			Job->Info.State = TEXT("making_reference");
			FString Error;
			const FString Path = Model.Path.Replace(TEXT("{model}"), *Model.Model);
			const FString ResponsePath = Model.ResponseImagePath;
			const bool bSent = FHyperAIStudioExternalServices::PostJson(Model.ServiceId, Path, Body,
				[Job, ResponsePath](const bool bOk, const TArray<uint8>& Reply, const TMap<FString, FString>&, const FString& Why)
				{
					Job->bWaiting = false;
					if (Job->Phase == EPhase::Failed || Job->Phase == EPhase::Done)
					{
						return;
					}
					const TSharedPtr<FJsonObject> Json = ParseJson(Reply);
					TArray<FString> Parts;
					ResponsePath.ParseIntoArray(Parts, TEXT("."));
					const FString Encoded = bOk && Json.IsValid() ? Hi3DImageAt(MakeShared<FJsonValueObject>(Json), Parts, 0) : FString();
					TArray<uint8> Bytes;
					if (Encoded.IsEmpty() || !FBase64::Decode(Encoded, Bytes) || Bytes.Num() < 16)
					{
						Fail(*Job, FString::Printf(TEXT("The reference picture could not be made%s%s. Hi3D was not asked; nothing more was spent."), Why.IsEmpty() ? TEXT("") : TEXT(": "), *Why.Left(300)));
						return;
					}
					const bool bPng = Bytes[0] == 0x89 && Bytes[1] == 'P';
					const FString File = ModelsRoot() / Job->Info.OperationId / (bPng ? TEXT("reference.png") : TEXT("reference.jpg"));
					IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
					if (!FFileHelper::SaveArrayToFile(Bytes, *File))
					{
						Fail(*Job, TEXT("The reference picture could not be written under Saved/HyperAIStudio/Models."));
						return;
					}
					Job->ImageFile = File;
					Job->ReferenceFile = File;
				}, Error, Job->ReferenceCost);
			if (!bSent)
			{
				Job->bWaiting = false;
				Fail(*Job, TEXT("The reference picture could not be asked for: ") + Error);
			}
		}

		// ------------------------------------------------------------ the steps

		void Hi3DPrepare(const TSharedRef<FJob>& Job)
		{
			if (!Job->ReferencePrompt.IsEmpty() && (Job->ImageFile.IsEmpty() || IFileManager::Get().FileSize(*Job->ImageFile) <= 0))
			{
				MakeImageGenReference(Job);
				return;
			}
			const FString Secret = Key(Job->Service);
			if (Secret.IsEmpty())
			{
				Fail(*Job, FString::Printf(TEXT("Hi3D's key is not set: add it in Settings > Agents > Keys (%s) as client_id:client_secret. Nothing was spent."), *Job->Service.ApiKeyVariable));
				return;
			}
			const FString Hash = Hi3DKeyHash(Secret);
			FHi3DTokenCache& Cache = Hi3DTokenCache();
			if (!Cache.Token.IsEmpty() && Cache.KeyHash == Hash)
			{
				Job->Token = Cache.Token;
				Hi3DAfterToken(*Job);
				return;
			}
			Job->Info.State = TEXT("authorising");
			const TMap<FString, FString> Headers = {{TEXT("Authorization"), Hi3DBasicAuth(Secret)}, {TEXT("Content-Type"), TEXT("application/json")}};
			Send(Job, TEXT("POST"), Job->Service.BaseUrl + TEXT("/open-api/v1/auth/token"), Headers, {}, [Hash](FJob& Job, const int32 Code, const TArray<uint8>& Body)
			{
				const TSharedPtr<FJsonObject> Json = ParseJson(Body);
				const FString Token = Field(Json, TEXT("data.accessToken"));
				const FString ProviderCode = Field(Json, TEXT("code"));
				if (Code != 200 || Token.IsEmpty() || (!ProviderCode.IsEmpty() && ProviderCode != TEXT("200")))
				{
					Fail(Job, Hi3DError(Job, Code, Body) + TEXT(" Hi3D's key is client_id:client_secret from its developer platform. Nothing was spent."));
					return;
				}
				Hi3DTokenCache().Token = Token;
				Hi3DTokenCache().KeyHash = Hash;
				Job.Token = Token;
				Hi3DAfterToken(Job);
			}, 60.f);
		}

		void Hi3DSubmit(const TSharedRef<FJob>& Job)
		{
			FTask& Task = Job->Tasks[Job->TaskIndex];
			FForm Form;
			FString Error;
			if (!Task.Form || !Task.Form(*Job, Form, Error))
			{
				Fail(*Job, (Error.IsEmpty() ? FString(TEXT("The Hi3D task has no form to send.")) : Error) + TEXT(" Nothing was spent."));
				return;
			}
			const TMap<FString, FString> Headers = {{TEXT("Authorization"), TEXT("Bearer ") + Job->Token}, {TEXT("Content-Type"), Form.ContentType()}};
			Job->Info.State = TEXT("submitting");
			Send(Job, TEXT("POST"), Task.CreateUrl, Headers, Form.Finish(), [](FJob& Job, const int32 Code, const TArray<uint8>& Body)
			{
				if (Hi3DReauth(Job, Code, Body, EPhase::Submit))
				{
					return;
				}
				FTask& Task = Job.Tasks[Job.TaskIndex];
				const TSharedPtr<FJsonObject> Json = ParseJson(Body);
				const FString TaskId = Field(Json, Task.TaskIdPath);
				if (Code != 200 || TaskId.IsEmpty() || Field(Json, TEXT("code")) != TEXT("200"))
				{
					Fail(Job, Hi3DError(Job, Code, Body));
					return;
				}
				// Hi3D took the task: from here it is paid for.
				Task.TaskId = TaskId;
				Job.Info.TaskId = TaskId;
				Job.Info.Tasks.Add(Task.Label + TEXT(": ") + TaskId);
				if (Task.Cost > 0.0)
				{
					FHyperAIStudioExternalServices::RecordSpend(Job.Service.Id, 1, Task.Cost);
				}
				Job.Phase = EPhase::Generate;
				Job.Info.State = TEXT("generating");
				Job.NextPollAt = FPlatformTime::Seconds() + PollSeconds();
				WriteRecord(Job);
			}, 300.f);
		}

		void Hi3DQuery(const TSharedRef<FJob>& Job)
		{
			const FTask& Task = Job->Tasks[Job->TaskIndex];
			const TMap<FString, FString> Headers = {{TEXT("Authorization"), TEXT("Bearer ") + Job->Token}};
			Send(Job, TEXT("GET"), Task.QueryUrl.Replace(TEXT("{id}"), *FGenericPlatformHttp::UrlEncode(Task.TaskId)), Headers, {}, [](FJob& Job, const int32 Code, const TArray<uint8>& Body)
			{
				if (Hi3DReauth(Job, Code, Body, EPhase::Generate))
				{
					return;
				}
				FTask& Task = Job.Tasks[Job.TaskIndex];
				const TSharedPtr<FJsonObject> Json = ParseJson(Body);
				const FString ProviderCode = Field(Json, TEXT("code"));
				if (ProviderCode == TEXT("50010001"))
				{
					// Generation failed at Hi3D, which gave the credits back: the creator offers it again.
					Job.bRefunded = true;
					Fail(Job, Hi3DError(Job, Code, Body));
					return;
				}
				if (Code != 200 || !Json.IsValid() || ProviderCode != TEXT("200"))
				{
					Fail(Job, Hi3DError(Job, Code, Body));
					return;
				}
				const FString State = Field(Json, Task.StatePath).ToLower();
				const int32 Own = State == TEXT("processing") ? 50 : State == TEXT("queueing") ? 10 : State == TEXT("success") ? 100 : 0;
				Job.Info.Progress = (Job.TaskIndex * 100 + Own) / FMath::Max(1, Job.Tasks.Num());
				if (State == TEXT("success"))
				{
					FString Error;
					if (Task.OnDone && !Task.OnDone(Job, Json, Error))
					{
						Fail(Job, Error);
						return;
					}
					++Job.TaskIndex;
					Job.Phase = Job.TaskIndex < Job.Tasks.Num() ? EPhase::Submit : Job.Downloads.IsEmpty() ? EPhase::Import : EPhase::Download;
					Job.Info.State = Job.Phase == EPhase::Submit ? TEXT("submitting") : Job.Phase == EPhase::Download ? TEXT("downloading") : TEXT("importing");
					WriteRecord(Job);
					return;
				}
				if (State == TEXT("failed"))
				{
					Fail(Job, FString::Printf(TEXT("%s's %s task failed: %s"), *Job.Service.Label, *Task.Label, *Hi3DError(Job, Code, Body)));
					return;
				}
				// created, queueing, processing: asked again shortly.
				Job.NextPollAt = FPlatformTime::Seconds() + PollSeconds();
			}, 60.f);
		}

		// ------------------------------------------------------------ the balance

		struct FHi3DBalanceRead
		{
			FHyperAIStudioExternalServiceSettings Service;
			FString Secret;
			int32 Tries = 0;
			TFunction<void(const FString&, float, const FString&)> OnDone;
		};

		void Hi3DHttp(const FString& Verb, const FString& Url, const TMap<FString, FString>& Headers, TFunction<void(int32, const TArray<uint8>&)> OnReply)
		{
			const TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
			Request->SetURL(Url);
			Request->SetVerb(Verb);
			Request->SetTimeout(30.f);
			for (const TPair<FString, FString>& Header : Headers)
			{
				Request->SetHeader(Header.Key, Header.Value);
			}
			Request->OnProcessRequestComplete().BindLambda([OnReply = MoveTemp(OnReply)](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				OnReply(bConnected && Response.IsValid() ? Response->GetResponseCode() : 0, bConnected && Response.IsValid() ? Response->GetContent() : TArray<uint8>());
			});
			Request->ProcessRequest();
		}

		void Hi3DBalanceStep(const TSharedRef<FHi3DBalanceRead>& Read)
		{
			FHi3DTokenCache& Cache = Hi3DTokenCache();
			const FString Hash = Hi3DKeyHash(Read->Secret);
			if (Cache.Token.IsEmpty() || Cache.KeyHash != Hash)
			{
				Hi3DHttp(TEXT("POST"), Read->Service.BaseUrl + TEXT("/open-api/v1/auth/token"),
					{{TEXT("Authorization"), Hi3DBasicAuth(Read->Secret)}, {TEXT("Content-Type"), TEXT("application/json")}}, [Read, Hash](const int32 Code, const TArray<uint8>& Body)
					{
						const TSharedPtr<FJsonObject> Json = ParseJson(Body);
						const FString Token = Field(Json, TEXT("data.accessToken"));
						if (Code != 200 || Token.IsEmpty())
						{
							const FString ProviderCode = Field(Json, TEXT("code"));
							const FString Meaning = Hi3DCodeMessage(ProviderCode);
							Read->OnDone(ProviderCode == TEXT("40010000") || Code == 401 ? TEXT("bad_key") : TEXT("error"), 0.f,
								FString::Printf(TEXT("Hi3D answered %d%s%s"), Code, Meaning.IsEmpty() ? TEXT(".") : TEXT(": "), *Meaning));
							return;
						}
						Hi3DTokenCache().Token = Token;
						Hi3DTokenCache().KeyHash = Hash;
						Hi3DBalanceStep(Read);
					});
				return;
			}
			Hi3DHttp(TEXT("GET"), Read->Service.BaseUrl + TEXT("/open-api/v1/balance"), {{TEXT("Authorization"), TEXT("Bearer ") + Cache.Token}}, [Read](const int32 Code, const TArray<uint8>& Body)
			{
				const TSharedPtr<FJsonObject> Json = ParseJson(Body);
				const FString ProviderCode = Field(Json, TEXT("code"));
				if (Hi3DIsAuthError(Code, ProviderCode) && ++Read->Tries < 2)
				{
					Hi3DTokenCache().Token.Reset();
					Hi3DBalanceStep(Read);
					return;
				}
				const FString Balance = Field(Json, TEXT("data.totalBalance"));
				if (Code != 200 || Balance.IsEmpty())
				{
					Read->OnDone(TEXT("error"), 0.f, FString::Printf(TEXT("Hi3D answered %d reading the balance."), Code));
					return;
				}
				const float Credits = FCString::Atof(*Balance);
				Read->OnDone(TEXT("ok"), Credits, FString::Printf(TEXT("%.0f Hi3D credits available (about $%.2f)."), Credits, Credits * 0.02f));
			});
		}
	}

	using namespace Hi3DDetail;

	// ---------------------------------------------------------------- the model table

	TArray<FString> Hi3DModels()
	{
		TArray<FString> Out;
		for (const FHi3DModelRow& Row : Hi3DModelTable())
		{
			Out.Add(Row.Model);
		}
		return Out;
	}

	TArray<FString> Hi3DResolutions(const FString& Model)
	{
		TArray<FString> Out;
		for (const FHi3DModelRow& Row : Hi3DModelTable())
		{
			if (Model.Equals(Row.Model, ESearchCase::IgnoreCase))
			{
				for (const TCHAR* Resolution : Row.Resolutions)
				{
					Out.Add(Resolution);
				}
			}
		}
		return Out;
	}

	bool Hi3DIsModel(const FString& Model)
	{
		return !Hi3DResolutions(Model.TrimStartAndEnd()).IsEmpty();
	}

	bool Hi3DIsPortrait(const FString& Model)
	{
		return Model.StartsWith(TEXT("scene-portrait"), ESearchCase::IgnoreCase);
	}

	bool Hi3DSupportsPbr(const FString& Model)
	{
		return Model.EndsWith(TEXT("v2.0"), ESearchCase::IgnoreCase) || Model.EndsWith(TEXT("v2.1"), ESearchCase::IgnoreCase)
			|| Model.EndsWith(TEXT("v3.0"), ESearchCase::IgnoreCase);
	}

	bool Hi3DCanTextureOnly(const FString& Model)
	{
		return Model.Equals(TEXT("hi3dv3.0"), ESearchCase::IgnoreCase) || Model.Equals(TEXT("hitem3dv1.5"), ESearchCase::IgnoreCase);
	}

	int32 Hi3DRecommendedFaces(const FString& Resolution)
	{
		return Resolution == TEXT("512") ? 500000 : Resolution == TEXT("1024") ? 1000000 : Resolution == TEXT("2048master") ? 5000000 : 2000000;
	}

	int32 Hi3DMaxFacesFor(const FString& Resolution)
	{
		return Resolution == TEXT("2048master") ? Hi3DMasterMaxFaces : Hi3DMaxFaces;
	}

	FString Hi3DResolutionUp(const FString& Model, const FString& Resolution)
	{
		const TArray<FString> All = Hi3DResolutions(Model);
		const int32 Index = All.IndexOfByKey(Resolution);
		return Index != INDEX_NONE && Index + 1 < All.Num() ? All[Index + 1] : FString();
	}

	bool Hi3DValidate(const FHi3DRequest& Request, FString& OutError)
	{
		const TArray<FString> Resolutions = Hi3DResolutions(Request.Model);
		if (Resolutions.IsEmpty())
		{
			OutError = FString::Printf(TEXT("model %s is not Hi3D's: it takes %s."), *Request.Model, *FString::Join(Hi3DModels(), TEXT(", ")));
			return false;
		}
		if (Request.RequestType < 1 || Request.RequestType > 3)
		{
			OutError = TEXT("request_type is 1 (geometry), 2 (texture on a given mesh) or 3 (both).");
			return false;
		}
		if (Request.RequestType != 2 && !Resolutions.Contains(Request.Resolution))
		{
			OutError = FString::Printf(TEXT("%s takes resolution %s, not %s."), *Request.Model, *FString::Join(Resolutions, TEXT(" or ")), *Request.Resolution);
			return false;
		}
		const int32 Most = Hi3DMaxFacesFor(Request.Resolution);
		if (Request.RequestType != 2 && (Request.Faces < Hi3DMinFaces || Request.Faces > Most))
		{
			OutError = FString::Printf(TEXT("Hi3D makes %d to %d faces at %s, not %d."), Hi3DMinFaces, Most, *Request.Resolution, Request.Faces);
			return false;
		}
		if (Request.bPbr && Request.RequestType != 1 && !Hi3DSupportsPbr(Request.Model))
		{
			OutError = FString::Printf(TEXT("%s has no PBR maps; v2.0, v2.1 and v3.0 do."), *Request.Model);
			return false;
		}
		if (Request.RequestType == 2 && !Hi3DCanTextureOnly(Request.Model))
		{
			OutError = FString::Printf(TEXT("Texturing a given mesh is offered on hi3dv3.0 and hitem3dv1.5 only, not %s."), *Request.Model);
			return false;
		}
		if (Request.RequestType == 2 && Hi3DExtension(Request.Mesh) != TEXT("glb"))
		{
			OutError = TEXT("Hi3D textures a GLB: the mesh to texture must be a .glb.");
			return false;
		}
		int32 Given = 0;
		for (const FString& Image : Request.Images)
		{
			Given += !Image.IsEmpty();
		}
		if (Given == 0)
		{
			OutError = TEXT("Hi3D works from a picture: give one, or views.");
			return false;
		}
		if (Request.Images.Num() > 4 || (!Request.bMultiview && Request.Images.Num() != 1))
		{
			OutError = TEXT("One picture, or up to four views (front, back, left, right).");
			return false;
		}
		if (Request.bMultiview && Request.Images[0].IsEmpty())
		{
			OutError = TEXT("Views need the front view.");
			return false;
		}
		return true;
	}

	void Hi3DFields(const FHi3DRequest& Request, TArray<TPair<FString, FString>>& OutText, TArray<TPair<FString, FString>>& OutFiles)
	{
		OutText.Add({TEXT("request_type"), FString::FromInt(Request.RequestType)});
		OutText.Add({TEXT("model"), Request.Model});
		if (Request.RequestType != 2)
		{
			OutText.Add({TEXT("resolution"), Request.Resolution});
			OutText.Add({TEXT("face"), FString::FromInt(Request.Faces)});
		}
		if (Request.RequestType != 1 && Hi3DSupportsPbr(Request.Model))
		{
			OutText.Add({TEXT("pbr"), Request.bPbr ? TEXT("1") : TEXT("0")});
			OutText.Add({TEXT("shading"), FString::Printf(TEXT("%.1f"), FMath::RoundToFloat(FMath::Clamp(Request.Shading, 0.f, 1.f) * 10.f) / 10.f)});
		}
		OutText.Add({TEXT("rmbg"), Request.bRemoveBackground ? TEXT("1") : TEXT("0")});
		// GLB: what Unreal imports with its materials and textures.
		OutText.Add({TEXT("format"), TEXT("2")});
		if (Request.bMultiview)
		{
			FString Bits;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				const bool bGiven = Request.Images.IsValidIndex(Index) && !Request.Images[Index].IsEmpty();
				Bits.AppendChar(bGiven ? TEXT('1') : TEXT('0'));
				if (bGiven)
				{
					OutFiles.Add({TEXT("multi_images"), Request.Images[Index]});
				}
			}
			OutText.Add({TEXT("multi_images_bit"), Bits});
		}
		else if (!Request.Images.IsEmpty())
		{
			OutFiles.Add({TEXT("images"), Request.Images[0]});
		}
		if (Request.RequestType == 2)
		{
			OutFiles.Add({TEXT("mesh"), Request.Mesh});
		}
	}

	bool Hi3DForm(const FHi3DRequest& Request, FForm& Out, FString& OutError)
	{
		if (!Hi3DValidate(Request, OutError))
		{
			return false;
		}
		TArray<TPair<FString, FString>> Text, Files;
		Hi3DFields(Request, Text, Files);
		for (const TPair<FString, FString>& Pair : Text)
		{
			Out.Text(Pair.Key, Pair.Value);
		}
		for (const TPair<FString, FString>& Pair : Files)
		{
			const FString Extension = Hi3DExtension(Pair.Value);
			if (Pair.Key != TEXT("mesh") && Extension != TEXT("png") && Extension != TEXT("jpg") && Extension != TEXT("jpeg") && Extension != TEXT("webp"))
			{
				OutError = FString::Printf(TEXT("%s is not a PNG, JPEG or WebP, which Hi3D takes."), *FPaths::GetCleanFilename(Pair.Value));
				return false;
			}
			const int64 Size = IFileManager::Get().FileSize(*Pair.Value);
			if (Size <= 0 || Size > Hi3DMaxFileBytes)
			{
				OutError = FString::Printf(TEXT("%s is %s; Hi3D takes files up to 20 MB."), *FPaths::GetCleanFilename(Pair.Value), Size <= 0 ? TEXT("missing") : TEXT("over 20 MB"));
				return false;
			}
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Pair.Value))
			{
				OutError = FString::Printf(TEXT("%s could not be read."), *FPaths::GetCleanFilename(Pair.Value));
				return false;
			}
			Out.File(Pair.Key, Pair.Value, Bytes);
		}
		return true;
	}

	FString Hi3DCodeMessage(const FString& Code)
	{
		static const TMap<FString, FString> Messages = {
			{TEXT("30010000"), TEXT("the Hi3D balance is too low for this task; top up on Hi3D's developer platform.")},
			{TEXT("10031001"), TEXT("a file is over 20 MB, Hi3D's limit.")},
			{TEXT("10031002"), TEXT("the face count is not one Hi3D takes (100,000 to 2,000,000; 5,000,000 at 2048master).")},
			{TEXT("10031003"), TEXT("the resolution is not one this model takes.")},
			{TEXT("10031005"), TEXT("the picture is not a format Hi3D takes (PNG, JPEG or WebP).")},
			{TEXT("10031006"), TEXT("the model is not one of Hi3D's.")},
			{TEXT("10031007"), TEXT("one picture and views were both sent; send one or the other.")},
			{TEXT("10031008"), TEXT("no picture was sent.")},
			{TEXT("10031009"), TEXT("too many pictures: four views at most.")},
			{TEXT("10031011"), TEXT("texturing needs the mesh to texture, and none was sent.")},
			{TEXT("10031016"), TEXT("the mesh to texture is not a GLB.")},
			{TEXT("10031017"), TEXT("this model cannot texture a given mesh; texture with hi3dv3.0, or make the model again.")},
			{TEXT("10000000"), TEXT("Hi3D had a system error; try again later.")},
			{TEXT("50010001"), TEXT("Hi3D could not generate it (generate failed); its credits were refunded, and it can be tried again.")},
			{TEXT("40010000"), TEXT("Hi3D refused the key: it is client_id:client_secret from Hi3D's developer platform.")},
		};
		const FString* Found = Messages.Find(Code.TrimStartAndEnd());
		return Found ? *Found : FString();
	}

	bool Hi3DIsAuthError(const int32 HttpCode, const FString& Code)
	{
		// The token's lifetime is not documented: a 401, or one of Hi3D's 4001xxxx codes, means ask for a new one.
		return HttpCode == 401 || Code == TEXT("401") || Code.StartsWith(TEXT("4001"));
	}

	FString Hi3DBasicAuth(const FString& Key)
	{
		return TEXT("Basic ") + FBase64::Encode(Key.TrimStartAndEnd());
	}

	bool ReferenceImageModel(FHyperAIStudioImageModelSettings& Out)
	{
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		TArray<FHyperAIStudioImageModelSettings> All = Settings ? Settings->ImageModels : TArray<FHyperAIStudioImageModelSettings>();
		All.Append(UHyperAIStudioSettings::GetNanoBananaModels());
		for (const FHyperAIStudioImageModelSettings& Model : All)
		{
			FHyperAIStudioExternalServiceSettings Service;
			if (!Model.bRunsOnThisMachine && FHyperAIStudioExternalServices::Find(Model.ServiceId, Service) && FHyperAIStudioExternalServices::HasApiKey(Service))
			{
				Out = Model;
				return true;
			}
		}
		if (All.IsEmpty())
		{
			return false;
		}
		Out = All[0];
		return true;
	}

	float ReferenceImagePrice(const FString& ModelId)
	{
		FHyperAIStudioImageModelSettings Model;
		if (!FindImageModel(ModelId, Model))
		{
			return 0.f;
		}
		FHyperAIStudioExternalServiceSettings Service;
		return Model.CostPerImage > 0.f ? Model.CostPerImage : FHyperAIStudioExternalServices::Find(Model.ServiceId, Service) ? Service.CostPerCall : 0.f;
	}

	// ---------------------------------------------------------------- prices

	float Hi3DPrice(const FString& Model, const FString& Resolution, const int32 RequestType, const bool bPbr)
	{
		if (RequestType == 2)
		{
			return Hi3DTexturePrice(bPbr);
		}
		const float Total = FMath::Max(UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("hi3d"), Model.ToLower() + TEXT("_") + Resolution.ToLower()), 0.f);
		const float Texture = FMath::Max(UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("hi3d"), TEXT("texture")), 0.f);
		const float Pbr = Hi3DSupportsPbr(Model) ? FMath::Max(UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("hi3d"), TEXT("pbr")), 0.f) : 0.f;
		// The list price is a whole textured model, with PBR where the model has it.
		return FMath::Max(RequestType == 1 ? Total - Texture - Pbr : bPbr ? Total : Total - Pbr, 0.f);
	}

	float Hi3DTexturePrice(const bool bPbr)
	{
		return FMath::Max(UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("hi3d"), bPbr ? TEXT("texture_only_pbr") : TEXT("texture_only")), 0.f);
	}

	int32 Hi3DCredits(const float USD)
	{
		return FMath::RoundToInt(USD * 50.f);
	}

	int32 ProviderCredits(const FString& Provider, const float USD)
	{
		return Provider == TEXT("hi3d") ? Hi3DCredits(USD) : Credits(USD);
	}

	TArray<FString> Hi3DViewOrder(const TArray<FString>& FrontLeftBackRight)
	{
		auto At = [&FrontLeftBackRight](const int32 Index) { return FrontLeftBackRight.IsValidIndex(Index) ? FrontLeftBackRight[Index].TrimStartAndEnd() : FString(); };
		// The creator's views are front, left, back, right (Tripo's order); Hi3D's are front, back, left, right.
		return {At(0), At(2), At(1), At(3)};
	}

	FHi3DRequest Hi3DRequestFromPlan(const FHyperAIModelPlan& Plan, const TArray<FString>& Images, const bool bMultiview)
	{
		FHi3DRequest Request;
		Request.RequestType = Plan.TextureQuality == TEXT("none") ? 1 : 3;
		Request.Model = Plan.Model;
		Request.Resolution = Plan.Resolution;
		Request.Faces = Plan.ProviderFaces;
		Request.bPbr = Plan.bPbr && Hi3DSupportsPbr(Plan.Model);
		Request.Images = Images;
		Request.bMultiview = bMultiview;
		return Request;
	}

	void Hi3DReadBalance(TFunction<void(const FString& Status, float Balance, const FString& Diagnostic)> OnDone)
	{
		FHyperAIStudioExternalServiceSettings Service;
		const FString Secret = FHyperAIStudioExternalServices::Find(TEXT("hi3d"), Service) ? Key(Service) : FString();
		if (Secret.IsEmpty())
		{
			OnDone(TEXT("missing_key"), 0.f, FString::Printf(TEXT("No Hi3D key: add client_id:client_secret in Settings > Agents > Keys (%s), or set HYPERAI_HI3D_KEY."),
				Service.ApiKeyVariable.IsEmpty() ? TEXT("HITEM3D_API_KEY") : *Service.ApiKeyVariable));
			return;
		}
		const TSharedRef<FHi3DBalanceRead> Read = MakeShared<FHi3DBalanceRead>();
		Read->Service = Service;
		Read->Secret = Secret;
		Read->OnDone = MoveTemp(OnDone);
		Hi3DBalanceStep(Read);
	}

#if WITH_DEV_AUTOMATION_TESTS
	void ForgetHi3DTokenForTests()
	{
		Hi3DTokenCache() = FHi3DTokenCache();
	}

	void SetHi3DTokenForTests(const FString& Token)
	{
		FHyperAIStudioExternalServiceSettings Service;
		Hi3DTokenCache().Token = Token;
		Hi3DTokenCache().KeyHash = FHyperAIStudioExternalServices::Find(TEXT("hi3d"), Service) ? Hi3DKeyHash(Key(Service)) : FString();
	}
#endif
}

namespace HyperAIStudio::MeshGen::Private
{
	bool StepHi3D(const TSharedRef<FJob>& Job)
	{
		using namespace HyperAIStudio::MeshGen::Creator::Hi3DDetail;
		switch (Job->Phase)
		{
		case EPhase::Prepare: Hi3DPrepare(Job); return true;
		case EPhase::Submit: Hi3DSubmit(Job); return true;
		case EPhase::Generate: Hi3DQuery(Job); return true;
		default: return false;
		}
	}
}
