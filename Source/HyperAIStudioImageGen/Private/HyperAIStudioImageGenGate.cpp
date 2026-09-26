// Games by Hyper 2026.

#include "HyperAIStudioImageGenGate.h"

#include "HAL/FileManager.h"
#include "HyperAIStudioAgentImage.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioExternalService.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace HyperAIStudio::ImageGen::Gate
{
	using Contracts = FHyperAIStudioImageGenContracts;

	namespace
	{
		/** One image is at most this after base64 decoding; past it the reply is not a reference image. */
		constexpr int32 MaxImageBytes = 32 * 1024 * 1024;

		FString EscapeForJsonString(const FString& Value)
		{
			FString Escaped;
			Escaped.Reserve(Value.Len() + 16);
			for (const TCHAR Character : Value)
			{
				switch (Character)
				{
				case TEXT('"'): Escaped += TEXT("\\\""); break;
				case TEXT('\\'): Escaped += TEXT("\\\\"); break;
				case TEXT('\n'): Escaped += TEXT("\\n"); break;
				case TEXT('\r'): Escaped += TEXT("\\r"); break;
				case TEXT('\t'): Escaped += TEXT("\\t"); break;
				default:
					if (Character < 0x20)
					{
						Escaped += FString::Printf(TEXT("\\u%04x"), static_cast<int32>(Character));
					}
					else
					{
						Escaped.AppendChar(Character);
					}
				}
			}
			return Escaped;
		}

		/** Splits on a separator, keeping empty pieces, because a dropped clause must leave its comma behind. */
		TArray<FString> SplitKeepingEmpty(const FString& Text, const TCHAR* Separator)
		{
			TArray<FString> Parts;
			Text.ParseIntoArray(Parts, Separator, /*InCullEmpty=*/false);
			return Parts;
		}

		struct FBatch
		{
			FString BatchId;
			FString FolderOnDisk;
			FString ProjectFolder;
			FHyperAIStudioImageModelSettings Model;
			FString TemplateId;
			FString Prompt;
			FString Negative;
			int32 Count = 1;
			int32 Width = 0;
			int32 Height = 0;
			int32 Seed = -1;
			float CostPerCall = 0.0f;
			int32 Sent = 0;
			int32 Done = 0;
			bool bInFlight = false;
			bool bFailed = false;
			FString Error;
			TArray<FString> Files;
			/** The picture an agent is drawing now, for an agent model. */
			TSharedPtr<FHyperAIStudioAgentImage> Drawing;
			/** The picture each image changes, read once when the batch starts; empty bytes draw from words. */
			FInputImage Input;
			/** Project-relative, for the sidecars. */
			FString InputProjectPath;
		};

		/**
		 * Models that are there without settings: Google's Nano Banana models through the Gemini API, and pictures an
		 * agent CLI draws on the user's own plan. They are data like any other model, so an entry in Image Models with
		 * the same id replaces one.
		 */
		TArray<FHyperAIStudioImageModelSettings> BuiltInModels()
		{
			TArray<FHyperAIStudioImageModelSettings> Models = UHyperAIStudioSettings::GetNanoBananaModels();
			FHyperAIStudioImageModelSettings Codex;
			Codex.Id = TEXT("codex_image_gen");
			Codex.Label = TEXT("Codex image generation (uses your Codex plan)");
			Codex.ServiceId = TEXT("agent:codex");
			Codex.Width = 1024;
			Codex.Height = 1024;
			FHyperAIStudioImageModelSettings Gemini = Codex;
			Gemini.Id = TEXT("gemini_image_gen");
			Gemini.Label = TEXT("Gemini CLI, with its nanobanana extension");
			Gemini.ServiceId = TEXT("agent:gemini");
			Models.Add(Codex);
			Models.Add(Gemini);
			return Models;
		}

		/** A model sent over HTTP, as the Images page and hyper_image_inspect show it: what it costs, and what stops it. */
		FHyperAIImageModelInfo DescribeServiceModel(const FHyperAIStudioImageModelSettings& Model)
		{
			FHyperAIImageModelInfo Info;
			Info.Id = Model.Id;
			Info.Label = Model.Label;
			Info.ServiceId = Model.ServiceId;
			Info.Model = Model.Model;
			Info.Width = Model.Width;
			Info.Height = Model.Height;
			Info.bRunsOnThisMachine = Model.bRunsOnThisMachine;
			Info.bTakesInputImage = TakesInputImage(Model);
			FHyperAIStudioExternalServiceSettings Service;
			if (!FHyperAIStudioExternalServices::Find(Model.ServiceId, Service))
			{
				Info.Health = TEXT("unknown");
				Info.Issue = FString::Printf(TEXT("No external service is configured with id %s."), *Model.ServiceId);
				return Info;
			}
			Info.CostPerCall = static_cast<float>(CostPerImage(Model, Model.Width, Model.Height));
			Info.bHasApiKey = FHyperAIStudioExternalServices::HasApiKey(Service);
			if (!Info.bHasApiKey)
			{
				Info.Health = FHyperAIStudioExternalServices::LexToString(EHyperAIStudioServiceHealth::KeyMissing);
				Info.Issue = FString::Printf(TEXT("Needs a key in the %s environment variable%s. Set it, then restart the editor; keys are never kept in project config."),
					*Service.ApiKeyVariable, Service.Id.Equals(TEXT("google_gemini"), ESearchCase::IgnoreCase) ? TEXT(" (get one at aistudio.google.com/apikey)") : TEXT(""));
				return Info;
			}
			const FHyperAIStudioServiceStatus Status = FHyperAIStudioExternalServices::QueryHealth(Model.ServiceId);
			Info.Health = FHyperAIStudioExternalServices::LexToString(Status.Health);
			if (Status.Health == EHyperAIStudioServiceHealth::Unreachable)
			{
				Info.Issue = Status.Diagnostic;
			}
			else if (Model.RequestTemplate.IsEmpty() || Model.ResponseImagePath.IsEmpty() || Model.Path.IsEmpty())
			{
				Info.Issue = TEXT("This model has no path, request_template or response_image_path; see the cookbook.");
			}
			return Info;
		}

		/** Why a Gemini reply holds no picture, in its own words; empty when it does not say. */
		FString GeminiRefusal(const TSharedPtr<FJsonValue>& Root)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Root.IsValid() || !Root->TryGetObject(Object))
			{
				return FString();
			}
			const TSharedPtr<FJsonObject>* Feedback = nullptr;
			FString Reason;
			if ((*Object)->TryGetObjectField(TEXT("promptFeedback"), Feedback) && (*Feedback)->TryGetStringField(TEXT("blockReason"), Reason))
			{
				return FString::Printf(TEXT("Gemini blocked the prompt (%s). Reword it and try again."), *Reason);
			}
			const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
			const TSharedPtr<FJsonObject>* First = nullptr;
			if (!(*Object)->TryGetArrayField(TEXT("candidates"), Candidates) || Candidates->IsEmpty() || !(*Candidates)[0]->TryGetObject(First))
			{
				return FString();
			}
			FString Said;
			const TSharedPtr<FJsonObject>* Content = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
			if ((*First)->TryGetObjectField(TEXT("content"), Content) && (*Content)->TryGetArrayField(TEXT("parts"), Parts))
			{
				for (const TSharedPtr<FJsonValue>& Part : *Parts)
				{
					const TSharedPtr<FJsonObject>* PartObject = nullptr;
					FString Text;
					if (Part->TryGetObject(PartObject) && (*PartObject)->TryGetStringField(TEXT("text"), Text) && !Text.IsEmpty())
					{
						Said = Text.Left(300);
						break;
					}
				}
			}
			(*First)->TryGetStringField(TEXT("finishReason"), Reason);
			if (!Reason.IsEmpty() && Reason != TEXT("STOP"))
			{
				return FString::Printf(TEXT("Gemini stopped without a picture (%s).%s%s"), *Reason, Said.IsEmpty() ? TEXT("") : TEXT(" It said: "), *Said);
			}
			return Said.IsEmpty() ? FString() : TEXT("Gemini answered with text instead of a picture: ") + Said;
		}

		TMap<FString, TSharedPtr<FBatch>>& Batches()
		{
			static TMap<FString, TSharedPtr<FBatch>> Map;
			return Map;
		}

		int32 SeedFor(const FBatch& Batch, const int32 Index)
		{
			// A fixed seed re-renders the same image, so a batch of one prompt would be one image repeated;
			// stepping from it keeps the batch varied while staying reproducible.
			return Batch.Seed < 0 ? -1 : Batch.Seed + Index;
		}

		FString ImageStem(const FBatch& Batch, const int32 Index)
		{
			return FString::Printf(TEXT("%s_%02d"), *Batch.BatchId, Index + 1);
		}

		void WriteSidecar(const FBatch& Batch, const int32 Index, const FString& ImageFile)
		{
			const FString Json = FString::Printf(
				TEXT("{\n")
				TEXT("\t\"image\": \"%s\",\n")
				TEXT("\t\"batch_id\": \"%s\",\n")
				TEXT("\t\"prompt\": \"%s\",\n")
				TEXT("\t\"negative\": \"%s\",\n")
				TEXT("\t\"template_id\": \"%s\",\n")
				TEXT("\t\"model_id\": \"%s\",\n")
				TEXT("\t\"model\": \"%s\",\n")
				TEXT("\t\"service_id\": \"%s\",\n")
				TEXT("\t\"width\": %d,\n")
				TEXT("\t\"height\": %d,\n")
				TEXT("\t\"seed\": %d,\n")
				TEXT("\t\"cost\": %.4f,\n")
				TEXT("%s")
				TEXT("\t\"generated_utc\": \"%s\"\n")
				TEXT("}\n"),
				*EscapeForJsonString(FPaths::GetCleanFilename(ImageFile)),
				*EscapeForJsonString(Batch.BatchId),
				*EscapeForJsonString(Batch.Prompt),
				*EscapeForJsonString(Batch.Negative),
				*EscapeForJsonString(Batch.TemplateId),
				*EscapeForJsonString(Batch.Model.Id),
				*EscapeForJsonString(Batch.Model.Model),
				*EscapeForJsonString(Batch.Model.ServiceId),
				Batch.Width, Batch.Height, SeedFor(Batch, Index),
				Batch.CostPerCall,
				// The picture it changed, so whoever matches to it can find the view it came from.
				Batch.InputProjectPath.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("\t\"input_image\": \"%s\",\n"), *EscapeForJsonString(Batch.InputProjectPath)),
				*FDateTime::UtcNow().ToIso8601());
			const FString SidecarFile = FPaths::ChangeExtension(ImageFile, TEXT("json"));
			FFileHelper::SaveStringToFile(Json, *SidecarFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		/** For an agent model: start the next picture, or take the one being drawn once it is done. */
		void DrawNextImage(const TSharedPtr<FBatch>& Batch)
		{
			if (Batch->Drawing.IsValid())
			{
				if (!Batch->Drawing->IsDone())
				{
					return;
				}
				const TSharedPtr<FHyperAIStudioAgentImage> Drawn = MoveTemp(Batch->Drawing);
				Batch->Drawing.Reset();
				Batch->bInFlight = false;
				if (Drawn->GetImagePath().IsEmpty())
				{
					Batch->bFailed = true;
					Batch->Error = Drawn->GetError();
					return;
				}
				const int32 Index = Batch->Done;
				const FString Extension = FPaths::GetExtension(Drawn->GetImagePath()).ToLower();
				const FString File = Batch->FolderOnDisk / (ImageStem(*Batch, Index) + TEXT(".") + (Extension.IsEmpty() ? FString(TEXT("png")) : Extension));
				if (IFileManager::Get().Copy(*File, *Drawn->GetImagePath()) != COPY_OK)
				{
					Batch->bFailed = true;
					Batch->Error = FString::Printf(TEXT("Could not copy the picture to %s."), *File);
					return;
				}
				WriteSidecar(*Batch, Index, File);
				Batch->Files.Add(File);
				++Batch->Done;
				return;
			}
			if (Batch->Sent >= Batch->Count)
			{
				return;
			}
			FString Agent;
			IsAgentModel(Batch->Model, Agent);
			FString StartError;
			// These generators take no negative prompt, so it goes into the description.
			const FString Prompt = Batch->Negative.IsEmpty() ? Batch->Prompt : Batch->Prompt + TEXT(" Avoid: ") + Batch->Negative;
			Batch->Drawing = FHyperAIStudioAgentImage::Start(Agent, Prompt, FIntPoint(Batch->Width, Batch->Height), StartError, Batch->Input.Path);
			if (!Batch->Drawing.IsValid())
			{
				Batch->bFailed = true;
				Batch->Error = StartError;
				return;
			}
			Batch->bInFlight = true;
			++Batch->Sent;
		}

		void SendNextImage(const TSharedPtr<FBatch>& Batch)
		{
			const int32 Index = Batch->Sent;
			FString Body;
			FString BuildError;
			const bool bChanges = !Batch->Input.Bytes.IsEmpty();
			if (!BuildRequestBody(Batch->Model, Batch->Prompt, Batch->Negative,
				Batch->Width, Batch->Height, SeedFor(*Batch, Index), Body, BuildError, bChanges ? &Batch->Input : nullptr))
			{
				Batch->bFailed = true;
				Batch->Error = BuildError;
				return;
			}
			Batch->bInFlight = true;
			const TSharedPtr<FBatch> Kept = Batch;
			FString PostError;
			// A Gemini model names itself in the path: /v1beta/models/{model}:generateContent.
			const FString& PathTemplate = bChanges && !Batch->Model.EditPath.IsEmpty() ? Batch->Model.EditPath : Batch->Model.Path;
			const FString Path = PathTemplate.Replace(TEXT("{model}"), *Batch->Model.Model, ESearchCase::IgnoreCase);
			const bool bSent = FHyperAIStudioExternalServices::PostJson(
				Batch->Model.ServiceId, Path, Body,
				[Kept, Index](const bool bOk, const TArray<uint8>& Response, const TMap<FString, FString>&, const FString& ResponseError)
				{
					Kept->bInFlight = false;
					if (!bOk)
					{
						Kept->bFailed = true;
						Kept->Error = ResponseError;
						return;
					}
					TArray<uint8> Bytes;
					FString ExtractError;
					if (!ExtractImageBytes(Response, Kept->Model.ResponseImagePath, Bytes, ExtractError))
					{
						Kept->bFailed = true;
						Kept->Error = ExtractError;
						return;
					}
					const FString Extension = SniffImageExtension(Bytes);
					if (Extension.IsEmpty())
					{
						Kept->bFailed = true;
						Kept->Error = TEXT("The decoded reply is not a PNG, JPEG or WebP image. Check response_image_path points at the image data.");
						return;
					}
					const FString File = Kept->FolderOnDisk / (ImageStem(*Kept, Index) + TEXT(".") + Extension);
					if (!FFileHelper::SaveArrayToFile(Bytes, *File))
					{
						Kept->bFailed = true;
						Kept->Error = FString::Printf(TEXT("Could not write %s."), *File);
						return;
					}
					WriteSidecar(*Kept, Index, File);
					Kept->Files.Add(File);
					++Kept->Done;
				},
				PostError, Batch->CostPerCall);
			if (!bSent)
			{
				Batch->bInFlight = false;
				Batch->bFailed = true;
				Batch->Error = PostError;
				return;
			}
			++Batch->Sent;
		}
	}

	bool FindModel(const FString& Id, FHyperAIStudioImageModelSettings& OutModel)
	{
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		if (!Settings || Id.IsEmpty())
		{
			return false;
		}
		for (const FHyperAIStudioImageModelSettings& Model : Settings->ImageModels)
		{
			if (Model.Id.Equals(Id, ESearchCase::IgnoreCase))
			{
				OutModel = Model;
				return true;
			}
		}
		for (const FHyperAIStudioImageModelSettings& Model : BuiltInModels())
		{
			if (Model.Id.Equals(Id, ESearchCase::IgnoreCase))
			{
				OutModel = Model;
				return true;
			}
		}
		return false;
	}

	bool IsAgentModel(const FHyperAIStudioImageModelSettings& Model, FString& OutAgent)
	{
		if (!Model.ServiceId.StartsWith(TEXT("agent:")))
		{
			return false;
		}
		OutAgent = Model.ServiceId.RightChop(6);
		return true;
	}

	bool TakesInputImage(const FHyperAIStudioImageModelSettings& Model)
	{
		FString Agent;
		return IsAgentModel(Model, Agent) || !Model.EditRequestTemplate.IsEmpty();
	}

	TArray<FHyperAIImageModelInfo> ReadModels()
	{
		TArray<FHyperAIImageModelInfo> Infos;
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		if (!Settings)
		{
			return Infos;
		}
		for (const FHyperAIStudioImageModelSettings& Model : Settings->ImageModels)
		{
			Infos.Add(DescribeServiceModel(Model));
		}
		for (const FHyperAIStudioImageModelSettings& Model : BuiltInModels())
		{
			if (Infos.ContainsByPredicate([&Model](const FHyperAIImageModelInfo& Info) { return Info.Id.Equals(Model.Id, ESearchCase::IgnoreCase); }))
			{
				continue;
			}
			FString Agent;
			if (!IsAgentModel(Model, Agent))
			{
				Infos.Add(DescribeServiceModel(Model));
				continue;
			}
			FHyperAIImageModelInfo Info;
			Info.Id = Model.Id;
			Info.Label = Model.Label;
			Info.ServiceId = Model.ServiceId;
			Info.Width = Model.Width;
			Info.Height = Model.Height;
			Info.bTakesInputImage = true;
			// No key: the agent is signed in already, and it spends the user's own plan rather than money here.
			Info.bHasApiKey = true;
			Info.Health = FHyperAIStudioAgentImage::CanDraw(Agent, Info.Issue) ? TEXT("ready") : TEXT("unavailable");
			Infos.Add(MoveTemp(Info));
		}
		return Infos;
	}

	TArray<FString> ReadTemplateSlots(const FString& Template)
	{
		TArray<FString> Slots;
		int32 Cursor = 0;
		while (Cursor < Template.Len())
		{
			const int32 Open = Template.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
			if (Open == INDEX_NONE)
			{
				break;
			}
			const int32 Close = Template.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open);
			if (Close == INDEX_NONE)
			{
				break;
			}
			const FString Name = Template.Mid(Open + 1, Close - Open - 1);
			if (!Name.IsEmpty() && Contracts::IsCanonicalName(Name, 32))
			{
				Slots.AddUnique(Name);
			}
			Cursor = Close + 1;
		}
		return Slots;
	}

	TArray<FHyperAIImageTemplateInfo> ReadTemplates()
	{
		TArray<FHyperAIImageTemplateInfo> Infos;
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		if (!Settings)
		{
			return Infos;
		}
		// Seeded in memory and not saved: this is a read, and a read that writes the user's config would
		// persist whatever else is on the settings object at the time.
		Settings->EnsureDefaultImagePromptTemplates();
		for (const FHyperAIStudioImagePromptTemplateSettings& Template : Settings->ImagePromptTemplates)
		{
			FHyperAIImageTemplateInfo Info;
			Info.Id = Template.Id;
			Info.Label = Template.Label;
			Info.Template = Template.Template;
			Info.Slots = ReadTemplateSlots(Template.Template);
			Infos.Add(MoveTemp(Info));
		}
		return Infos;
	}

	FString OutputFolder()
	{
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		FString Folder = Settings ? Settings->ImageOutputFolder : FString();
		if (Folder.IsEmpty())
		{
			Folder = TEXT("Content/References/Generated");
		}
		FPaths::NormalizeDirectoryName(Folder);
		return Folder;
	}

	FString OutputRootOnDisk()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / OutputFolder());
	}

	TArray<FHyperAIImageRecord> ReadGenerated(const FString& BatchId, const int32 Max)
	{
		TArray<FHyperAIImageRecord> Records;
		const FString Root = BatchId.IsEmpty() ? OutputRootOnDisk() : OutputRootOnDisk() / BatchId;
		TArray<FString> Sidecars;
		IFileManager::Get().FindFilesRecursive(Sidecars, *Root, TEXT("*.json"), /*Files=*/true, /*Directories=*/false);
		for (const FString& Sidecar : Sidecars)
		{
			FString Text;
			TSharedPtr<FJsonObject> Object;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(
				FFileHelper::LoadFileToString(Text, *Sidecar) ? Text : FString());
			if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
			{
				continue;
			}
			FString ImageName;
			if (!Object->TryGetStringField(TEXT("image"), ImageName) || ImageName.IsEmpty())
			{
				continue;
			}
			const FString ImageFile = FPaths::GetPath(Sidecar) / FPaths::GetCleanFilename(ImageName);
			if (!IFileManager::Get().FileExists(*ImageFile))
			{
				continue;
			}
			FHyperAIImageRecord Record;
			Record.File = ImageFile;
			FPaths::MakePathRelativeTo(Record.File, *(FPaths::ProjectDir()));
			Object->TryGetStringField(TEXT("batch_id"), Record.BatchId);
			Object->TryGetStringField(TEXT("prompt"), Record.Prompt);
			Object->TryGetStringField(TEXT("model_id"), Record.ModelId);
			Object->TryGetStringField(TEXT("generated_utc"), Record.GeneratedUtc);
			Object->TryGetNumberField(TEXT("seed"), Record.Seed);
			Object->TryGetNumberField(TEXT("width"), Record.Width);
			Object->TryGetNumberField(TEXT("height"), Record.Height);
			Object->TryGetStringField(TEXT("input_image"), Record.InputImage);
			Record.SizeBytes = IFileManager::Get().FileSize(*ImageFile);
			Records.Add(MoveTemp(Record));
		}
		Records.Sort([](const FHyperAIImageRecord& Left, const FHyperAIImageRecord& Right)
		{
			return Left.GeneratedUtc > Right.GeneratedUtc;
		});
		if (Max > 0 && Records.Num() > Max)
		{
			Records.SetNum(Max);
		}
		return Records;
	}

	bool ComposePrompt(const FString& TemplateId, const TArray<FHyperAIImageSlot>& Slots, const FString& Extra,
		FString& OutPrompt, TArray<FString>& OutUnfilled, FString& OutError)
	{
		OutPrompt.Reset();
		OutUnfilled.Reset();
		OutError.Reset();
		if (Slots.Num() > Contracts::MaxSlots)
		{
			OutError = FString::Printf(TEXT("A prompt takes up to %d slots."), Contracts::MaxSlots);
			return false;
		}
		TMap<FString, FString> Filled;
		for (const FHyperAIImageSlot& Slot : Slots)
		{
			if (!Contracts::IsCanonicalName(Slot.Name, 32) || Slot.Value.Len() > Contracts::MaxSlotValueCharacters)
			{
				OutError = FString::Printf(
					TEXT("Slot %s: a name is letters, digits and underscore up to 32 characters, and a value is up to %d."),
					*Slot.Name.Left(48), Contracts::MaxSlotValueCharacters);
				return false;
			}
			if (!Slot.Value.TrimStartAndEnd().IsEmpty())
			{
				Filled.Add(Slot.Name.ToLower(), Slot.Value.TrimStartAndEnd());
			}
		}

		FString Body;
		if (!TemplateId.IsEmpty())
		{
			const TArray<FHyperAIImageTemplateInfo> Templates = ReadTemplates();
			const FHyperAIImageTemplateInfo* Template = Templates.FindByPredicate(
				[&TemplateId](const FHyperAIImageTemplateInfo& Candidate) { return Candidate.Id.Equals(TemplateId, ESearchCase::IgnoreCase); });
			if (!Template)
			{
				OutError = FString::Printf(TEXT("No prompt template with id %s; hyper_image_inspect lists them."), *TemplateId.Left(48));
				return false;
			}
			for (const FString& Name : Template->Slots)
			{
				if (!Filled.Contains(Name.ToLower()))
				{
					OutUnfilled.Add(Name);
				}
			}
			Body = Template->Template;
			for (const TPair<FString, FString>& Entry : Filled)
			{
				Body = Body.Replace(*FString::Printf(TEXT("{%s}"), *Entry.Key), *Entry.Value, ESearchCase::IgnoreCase);
			}
			// Drop what stayed unfilled: the clause first, then a sentence left with nothing in it. A template
			// is meant to survive being half-answered, and a prompt with a literal {mood} in it reads as noise
			// to the model rather than as a gap.
			TArray<FString> Kept;
			for (const FString& Sentence : SplitKeepingEmpty(Body, TEXT(". ")))
			{
				TArray<FString> Clauses;
				for (const FString& Clause : SplitKeepingEmpty(Sentence, TEXT(", ")))
				{
					if (!Clause.Contains(TEXT("{")))
					{
						Clauses.Add(Clause);
					}
				}
				FString Rebuilt = FString::Join(Clauses, TEXT(", ")).TrimStartAndEnd();
				while (Rebuilt.StartsWith(TEXT(".")) || Rebuilt.StartsWith(TEXT(",")))
				{
					Rebuilt = Rebuilt.RightChop(1).TrimStart();
				}
				if (!Rebuilt.IsEmpty() && Rebuilt != TEXT("."))
				{
					Kept.Add(Rebuilt);
				}
			}
			Body = FString::Join(Kept, TEXT(". "));
		}

		const FString Tail = Extra.TrimStartAndEnd();
		if (!Tail.IsEmpty())
		{
			if (!Body.IsEmpty() && !Body.EndsWith(TEXT(".")))
			{
				Body += TEXT(".");
			}
			Body = Body.IsEmpty() ? Tail : Body + TEXT(" ") + Tail;
		}
		OutPrompt = Body.TrimStartAndEnd();
		if (OutPrompt.IsEmpty())
		{
			OutError = TEXT("The composed prompt is empty: fill at least one slot, or pass prompt text.");
			return false;
		}
		if (OutPrompt.Len() > Contracts::MaxPromptCharacters)
		{
			OutError = FString::Printf(TEXT("The composed prompt is %d characters, past the %d limit."),
				OutPrompt.Len(), Contracts::MaxPromptCharacters);
			return false;
		}
		return true;
	}

	double CostPerImage(const FHyperAIStudioImageModelSettings& Model, const int32 Width, const int32 Height)
	{
		FString Agent;
		if (IsAgentModel(Model, Agent))
		{
			return 0.0;
		}
		const int32 Longest = FMath::Max(Width, Height);
		double Cost = Model.CostPerImage;
		if (Longest > 1024 && Model.CostPerImage2K > 0.0f)
		{
			Cost = Model.CostPerImage2K;
		}
		if (Longest > 2048 && Model.CostPerImage4K > 0.0f)
		{
			Cost = Model.CostPerImage4K;
		}
		FHyperAIStudioExternalServiceSettings Service;
		return Cost > 0.0 ? Cost : FHyperAIStudioExternalServices::Find(Model.ServiceId, Service) ? static_cast<double>(Service.CostPerCall) : 0.0;
	}

	FString AspectRatio(const int32 Width, const int32 Height)
	{
		static const TPair<int32, int32> Ratios[] = {{1, 1}, {2, 3}, {3, 2}, {3, 4}, {4, 3}, {4, 5}, {5, 4}, {9, 16}, {16, 9}, {21, 9}};
		const double Wanted = FMath::Loge(static_cast<double>(FMath::Max(1, Width)) / FMath::Max(1, Height));
		TPair<int32, int32> Best = Ratios[0];
		for (const TPair<int32, int32>& Ratio : Ratios)
		{
			if (FMath::Abs(FMath::Loge(static_cast<double>(Ratio.Key) / Ratio.Value) - Wanted)
				< FMath::Abs(FMath::Loge(static_cast<double>(Best.Key) / Best.Value) - Wanted))
			{
				Best = Ratio;
			}
		}
		return FString::Printf(TEXT("%d:%d"), Best.Key, Best.Value);
	}

	FString ImageSize(const int32 Width, const int32 Height)
	{
		const int32 Longest = FMath::Max(Width, Height);
		return Longest <= 1024 ? TEXT("1K") : Longest <= 2048 ? TEXT("2K") : TEXT("4K");
	}

	bool ReadInputImage(const FString& Path, FInputImage& Out, FString& OutError)
	{
		Out = FInputImage();
		OutError.Reset();
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		Out.Path = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::ProjectDir() / Path : Path);
		if (!FPaths::IsUnderDirectory(Out.Path, Root) || !FFileHelper::LoadFileToArray(Out.Bytes, *Out.Path))
		{
			OutError = FString::Printf(TEXT("input_image_path %s is not a file inside the project; pass a picture such as a hyper_capture_view frame."), *Path.Left(200));
			return false;
		}
		const FString Extension = SniffImageExtension(Out.Bytes);
		if (Extension.IsEmpty())
		{
			OutError = FString::Printf(TEXT("input_image_path %s is not a PNG, JPEG or WebP picture."), *Path.Left(200));
			return false;
		}
		if (Out.Bytes.Num() > MaxInputImageBytes)
		{
			OutError = FString::Printf(TEXT("input_image_path %s is %.1f MB; a picture to change is at most %d MB, which a 1920 x 1080 capture is well within."),
				*Path.Left(200), Out.Bytes.Num() / (1024.0 * 1024.0), MaxInputImageBytes / (1024 * 1024));
			return false;
		}
		Out.MimeType = Extension == TEXT("jpg") ? FString(TEXT("image/jpeg")) : TEXT("image/") + Extension;
		FImage Decoded;
		if (FImageUtils::DecompressImage(Out.Bytes.GetData(), Out.Bytes.Num(), Decoded))
		{
			Out.Size = FIntPoint(Decoded.SizeX, Decoded.SizeY);
		}
		Out.Hash = FMD5::HashBytes(Out.Bytes.GetData(), Out.Bytes.Num());
		return true;
	}

	bool BuildRequestBody(const FHyperAIStudioImageModelSettings& Model, const FString& Prompt, const FString& Negative,
		const int32 Width, const int32 Height, const int32 Seed, FString& OutBody, FString& OutError, const FInputImage* Input)
	{
		OutBody.Reset();
		OutError.Reset();
		const FString& Template = Input ? Model.EditRequestTemplate : Model.RequestTemplate;
		if (Input && Template.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Model %s draws from words only: it has no edit_request_template, so it cannot change a picture. ")
				TEXT("The Nano Banana models, codex_image_gen and gemini_image_gen can; the cookbook shows the shape."), *Model.Id);
			return false;
		}
		if (Input && !Template.Contains(TEXT("{input_image}"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("Model %s's edit_request_template has no {input_image}, so the picture would never reach it; see the cookbook."), *Model.Id);
			return false;
		}
		if (Template.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Model %s has no request_template; see the cookbook for three worked shapes."), *Model.Id);
			return false;
		}
		OutBody = Template;
		// The picture first, so no text substituted after it can hold a placeholder that would take it; base64 has no braces.
		if (Input)
		{
			OutBody = OutBody.Replace(TEXT("{input_mime_type}"), *Input->MimeType, ESearchCase::IgnoreCase);
			OutBody = OutBody.Replace(TEXT("{input_image}"), *FBase64::Encode(Input->Bytes), ESearchCase::IgnoreCase);
		}
		// A model with no field for a negative prompt still hears it, at the end of the description.
		const FString Avoid = Negative.TrimStartAndEnd();
		const bool bTakesNegative = Template.Contains(TEXT("{negative}"), ESearchCase::IgnoreCase);
		const FString Described = bTakesNegative || Avoid.IsEmpty() ? Prompt : Prompt + TEXT(" Avoid: ") + Avoid;
		OutBody = OutBody.Replace(TEXT("{prompt}"), *EscapeForJsonString(Described), ESearchCase::IgnoreCase);
		OutBody = OutBody.Replace(TEXT("{negative}"), *EscapeForJsonString(Negative), ESearchCase::IgnoreCase);
		OutBody = OutBody.Replace(TEXT("{model}"), *EscapeForJsonString(Model.Model), ESearchCase::IgnoreCase);
		OutBody = OutBody.Replace(TEXT("{width}"), *FString::FromInt(Width), ESearchCase::IgnoreCase);
		OutBody = OutBody.Replace(TEXT("{height}"), *FString::FromInt(Height), ESearchCase::IgnoreCase);
		OutBody = OutBody.Replace(TEXT("{seed}"), *FString::FromInt(Seed), ESearchCase::IgnoreCase);
		OutBody = OutBody.Replace(TEXT("{aspect_ratio}"), *AspectRatio(Width, Height), ESearchCase::IgnoreCase);
		OutBody = OutBody.Replace(TEXT("{image_size}"), *ImageSize(Width, Height), ESearchCase::IgnoreCase);
		// A template that does not parse would be rejected by the endpoint with a message nobody can act on,
		// so it is caught here where the setting that caused it can be named.
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutBody);
		if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
		{
			OutBody.Reset();
			OutError = FString::Printf(
				TEXT("Model %s produced a request body that is not valid JSON. Check %s: {prompt} belongs inside a string's quotes, {width} outside them."),
				*Model.Id, Input ? TEXT("edit_request_template") : TEXT("request_template"));
			return false;
		}
		// Counted as sent: UTF-8, where a picture's base64 is a byte a character.
		if (FTCHARToUTF8(*OutBody).Length() > FHyperAIStudioExternalServices::MaxRequestBodyBytes)
		{
			OutBody.Reset();
			OutError = TEXT("The request body exceeds the bounded request size.");
			return false;
		}
		return true;
	}

	bool ExtractImageBytes(const TArray<uint8>& Response, const FString& DottedPath, TArray<uint8>& OutBytes, FString& OutError)
	{
		OutBytes.Reset();
		OutError.Reset();
		if (DottedPath.IsEmpty())
		{
			OutError = TEXT("This model has no response_image_path; see the cookbook.");
			return false;
		}
		FString Text;
		FFileHelper::BufferToString(Text, Response.GetData(), Response.Num());
		TSharedPtr<FJsonValue> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = TEXT("The service replied with something that is not JSON: ") + Text.Left(200);
			return false;
		}
		TArray<FString> Steps;
		DottedPath.ParseIntoArray(Steps, TEXT("."), /*InCullEmpty=*/true);
		// A * is any element of an array: the first one that has the rest of the path, as a picture that follows text.
		TFunction<TSharedPtr<FJsonValue>(const TSharedPtr<FJsonValue>&, int32)> Walk =
			[&Steps, &Walk](const TSharedPtr<FJsonValue>& At, const int32 StepIndex) -> TSharedPtr<FJsonValue>
		{
			if (!At.IsValid() || StepIndex == Steps.Num())
			{
				return At;
			}
			const FString& Step = Steps[StepIndex];
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (Step == TEXT("*"))
			{
				if (At->TryGetArray(Array))
				{
					for (const TSharedPtr<FJsonValue>& Element : *Array)
					{
						const TSharedPtr<FJsonValue> Found = Walk(Element, StepIndex + 1);
						FString Value;
						if (Found.IsValid() && Found->TryGetString(Value) && !Value.IsEmpty())
						{
							return Found;
						}
					}
				}
				return nullptr;
			}
			if (Step.IsNumeric())
			{
				const int32 Index = FCString::Atoi(*Step);
				return At->TryGetArray(Array) && Array->IsValidIndex(Index) ? Walk((*Array)[Index], StepIndex + 1) : nullptr;
			}
			const TSharedPtr<FJsonObject>* Object = nullptr;
			return At->TryGetObject(Object) && (*Object)->HasField(Step) ? Walk((*Object)->TryGetField(Step), StepIndex + 1) : nullptr;
		};
		const TSharedPtr<FJsonValue> Current = Walk(Root, 0);
		FString Encoded;
		if (!Current.IsValid() || !Current->TryGetString(Encoded) || Encoded.IsEmpty())
		{
			const FString Refusal = GeminiRefusal(Root);
			OutError = Refusal.IsEmpty()
				? FString::Printf(TEXT("Nothing at response_image_path %s in the reply: %s"), *DottedPath, *Text.Left(200))
				: Refusal;
			return false;
		}
		if (Encoded.StartsWith(TEXT("http://")) || Encoded.StartsWith(TEXT("https://")))
		{
			OutError = TEXT("The service returned a link rather than image data. Ask it for base64 in the request template ")
				TEXT("(an OpenAI-shaped endpoint takes \"response_format\": \"b64_json\"); this pack does not fetch links.");
			return false;
		}
		// A data URI carries its own prefix, and some services send one where plain base64 is documented.
		int32 Comma = INDEX_NONE;
		if (Encoded.StartsWith(TEXT("data:")) && Encoded.FindChar(TEXT(','), Comma))
		{
			Encoded = Encoded.RightChop(Comma + 1);
		}
		if (Encoded.Len() > MaxImageBytes / 3 * 4)
		{
			OutError = TEXT("The returned image is larger than this pack accepts for a reference.");
			return false;
		}
		if (!FBase64::Decode(Encoded, OutBytes) || OutBytes.IsEmpty())
		{
			OutBytes.Reset();
			OutError = FString::Printf(TEXT("The value at %s is not base64 image data."), *DottedPath);
			return false;
		}
		return true;
	}

	FString SniffImageExtension(const TArray<uint8>& Bytes)
	{
		if (Bytes.Num() >= 8 && Bytes[0] == 0x89 && Bytes[1] == 0x50 && Bytes[2] == 0x4E && Bytes[3] == 0x47)
		{
			return TEXT("png");
		}
		if (Bytes.Num() >= 3 && Bytes[0] == 0xFF && Bytes[1] == 0xD8 && Bytes[2] == 0xFF)
		{
			return TEXT("jpg");
		}
		if (Bytes.Num() >= 12 && Bytes[0] == 'R' && Bytes[1] == 'I' && Bytes[2] == 'F' && Bytes[3] == 'F'
			&& Bytes[8] == 'W' && Bytes[9] == 'E' && Bytes[10] == 'B' && Bytes[11] == 'P')
		{
			return TEXT("webp");
		}
		return FString();
	}

	bool StartBatch(const FString& BatchId, const FHyperAIStudioImageModelSettings& Model, const FString& TemplateId,
		const FString& Prompt, const FString& Negative, const int32 Count, const int32 Width, const int32 Height,
		const int32 Seed, FString& OutError, const FString& InputImagePath, const FString& InputImageHash)
	{
		OutError.Reset();
		if (Batches().Contains(BatchId))
		{
			OutError = FString::Printf(TEXT("A batch named %s is already known this session."), *BatchId);
			return false;
		}
		const TSharedPtr<FBatch> Batch = MakeShared<FBatch>();
		if (!InputImagePath.IsEmpty())
		{
			if (!ReadInputImage(InputImagePath, Batch->Input, OutError))
			{
				return false;
			}
			if (!InputImageHash.IsEmpty() && Batch->Input.Hash != InputImageHash)
			{
				OutError = FString::Printf(TEXT("%s changed after the plan was made, so it is not the picture that was approved; nothing was sent."), *InputImagePath);
				return false;
			}
			Batch->InputProjectPath = Batch->Input.Path;
			FPaths::MakePathRelativeTo(Batch->InputProjectPath, *(FPaths::ProjectDir()));
		}
		Batch->BatchId = BatchId;
		Batch->ProjectFolder = OutputFolder() / BatchId;
		Batch->FolderOnDisk = OutputRootOnDisk() / BatchId;
		Batch->Model = Model;
		Batch->TemplateId = TemplateId;
		Batch->Prompt = Prompt;
		Batch->Negative = Negative;
		Batch->Count = Count;
		Batch->Width = Width;
		Batch->Height = Height;
		Batch->Seed = Seed;
		Batch->CostPerCall = static_cast<float>(CostPerImage(Model, Width, Height));
		if (!IFileManager::Get().MakeDirectory(*Batch->FolderOnDisk, /*Tree=*/true))
		{
			OutError = FString::Printf(TEXT("Could not make %s."), *Batch->FolderOnDisk);
			return false;
		}
		// The picture it changes stays beside the results, with the capture.json naming its view when it is a capture of the
		// level: the reference keeps where it came from after the captures folder is pruned.
		if (!Batch->Input.Bytes.IsEmpty())
		{
			const FString Kept = Batch->FolderOnDisk / (BatchId + TEXT("_input.") + SniffImageExtension(Batch->Input.Bytes));
			if (FFileHelper::SaveArrayToFile(Batch->Input.Bytes, *Kept))
			{
				Batch->InputProjectPath = Kept;
				FPaths::MakePathRelativeTo(Batch->InputProjectPath, *(FPaths::ProjectDir()));
				const FString View = FPaths::GetPath(Batch->Input.Path) / TEXT("capture.json");
				if (IFileManager::Get().FileExists(*View))
				{
					IFileManager::Get().Copy(*(Batch->FolderOnDisk / TEXT("capture.json")), *View);
				}
			}
		}

		FHyperAIStudioAsyncJobRequest Job;
		Job.PackId = Contracts::PackId;
		Job.ToolName = Contracts::GenerateToolName;
		Job.Target = Batch->ProjectFolder;
		// The whole budget: a model on this machine sharing the card with the editor takes many times what a
		// hosted one does, and the tool warns about that rather than cutting the job short.
		Job.DeadlineMs = FHyperAIStudioAsyncJobHost::MaxDeadlineMs;
		Job.PollIntervalMs = 250;

		const TSharedPtr<FBatch> Kept = Batch;
		FString Agent;
		const bool bAgent = IsAgentModel(Model, Agent);
		if (bAgent)
		{
			// Dropping the drawing stops the agent's CLI.
			Job.OnCancel = [Kept]()
			{
				Kept->Drawing.Reset();
				Kept->bFailed = true;
				Kept->Error = TEXT("Cancelled.");
			};
		}
		FString JobId;
		const bool bStarted = FHyperAIStudioAsyncJobHost::Start(Job,
			[Kept, bAgent](FString& OutProgress, FString& OutDiagnostic)
			{
				OutProgress = FString::Printf(TEXT("%d of %d images"), Kept->Done, Kept->Count);
				if (Kept->bFailed)
				{
					OutDiagnostic = Kept->Error;
					return EHyperAIStudioAsyncJobPoll::Failed;
				}
				if (Kept->Done >= Kept->Count)
				{
					OutDiagnostic = FString::Printf(TEXT("%d images in %s"), Kept->Done, *Kept->ProjectFolder);
					return EHyperAIStudioAsyncJobPoll::Completed;
				}
				// ponytail: one request at a time. A local model owns the card, and a hosted one rate-limits;
				// parallel requests would help neither, and this keeps a failure attributable to one image.
				if (bAgent)
				{
					DrawNextImage(Kept);
				}
				else if (!Kept->bInFlight && Kept->Sent < Kept->Count)
				{
					SendNextImage(Kept);
				}
				return EHyperAIStudioAsyncJobPoll::Running;
			},
			JobId, OutError);
		if (!bStarted)
		{
			return false;
		}
		Batches().Add(BatchId, Batch);
		return true;
	}

	bool FindBatch(const FString& BatchId, FHyperAIImageBatchInfo& OutInfo)
	{
		const TSharedPtr<FBatch>* Found = Batches().Find(BatchId);
		if (!Found || !Found->IsValid())
		{
			return false;
		}
		const FBatch& Batch = **Found;
		OutInfo.BatchId = Batch.BatchId;
		OutInfo.State = Batch.bFailed ? TEXT("failed") : Batch.Done >= Batch.Count ? TEXT("complete") : TEXT("running");
		OutInfo.Progress = FString::Printf(TEXT("%d of %d images"), Batch.Done, Batch.Count);
		OutInfo.Folder = Batch.ProjectFolder;
		OutInfo.Diagnostic = Batch.bFailed ? Batch.Error : FString();
		return true;
	}

	TArray<FHyperAIImageBatchInfo> ReadBatches(const FString& BatchId)
	{
		TArray<FHyperAIImageBatchInfo> Infos;
		for (const TPair<FString, TSharedPtr<FBatch>>& Entry : Batches())
		{
			FHyperAIImageBatchInfo Info;
			if ((BatchId.IsEmpty() || BatchId == Entry.Key) && FindBatch(Entry.Key, Info))
			{
				Infos.Add(MoveTemp(Info));
			}
		}
		Infos.Sort([](const FHyperAIImageBatchInfo& Left, const FHyperAIImageBatchInfo& Right)
		{
			return Left.BatchId < Right.BatchId;
		});
		return Infos;
	}
}
