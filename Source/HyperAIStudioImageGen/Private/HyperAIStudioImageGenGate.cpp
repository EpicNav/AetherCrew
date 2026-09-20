// Games by Hyper 2026.

#include "HyperAIStudioImageGenGate.h"

#include "HAL/FileManager.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioExternalService.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
		};

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
				*FDateTime::UtcNow().ToIso8601());
			const FString SidecarFile = FPaths::ChangeExtension(ImageFile, TEXT("json"));
			FFileHelper::SaveStringToFile(Json, *SidecarFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		void SendNextImage(const TSharedPtr<FBatch>& Batch)
		{
			const int32 Index = Batch->Sent;
			FString Body;
			FString BuildError;
			if (!BuildRequestBody(Batch->Model, Batch->Prompt, Batch->Negative,
				Batch->Width, Batch->Height, SeedFor(*Batch, Index), Body, BuildError))
			{
				Batch->bFailed = true;
				Batch->Error = BuildError;
				return;
			}
			Batch->bInFlight = true;
			const TSharedPtr<FBatch> Kept = Batch;
			FString PostError;
			const bool bSent = FHyperAIStudioExternalServices::PostJson(
				Batch->Model.ServiceId, Batch->Model.Path, Body,
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
				PostError);
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
		return false;
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
			FHyperAIImageModelInfo Info;
			Info.Id = Model.Id;
			Info.Label = Model.Label;
			Info.ServiceId = Model.ServiceId;
			Info.Model = Model.Model;
			Info.Width = Model.Width;
			Info.Height = Model.Height;
			Info.bRunsOnThisMachine = Model.bRunsOnThisMachine;
			FHyperAIStudioExternalServiceSettings Service;
			if (!FHyperAIStudioExternalServices::Find(Model.ServiceId, Service))
			{
				Info.Health = TEXT("unknown");
				Info.Issue = FString::Printf(TEXT("No external service is configured with id %s."), *Model.ServiceId);
			}
			else
			{
				Info.CostPerCall = Service.CostPerCall;
				Info.bHasApiKey = FHyperAIStudioExternalServices::HasApiKey(Service);
				const FHyperAIStudioServiceStatus Status = FHyperAIStudioExternalServices::QueryHealth(Model.ServiceId);
				Info.Health = FHyperAIStudioExternalServices::LexToString(Status.Health);
				if (Status.Health == EHyperAIStudioServiceHealth::Unreachable || !Info.bHasApiKey)
				{
					Info.Issue = Status.Diagnostic;
				}
				else if (Model.RequestTemplate.IsEmpty() || Model.ResponseImagePath.IsEmpty() || Model.Path.IsEmpty())
				{
					Info.Issue = TEXT("This model has no path, request_template or response_image_path; see the cookbook.");
				}
			}
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
			FHyperAIImageRecord Record;
			const FString ImageName = Object->GetStringField(TEXT("image"));
			const FString ImageFile = FPaths::GetPath(Sidecar) / ImageName;
			Record.File = ImageFile;
			FPaths::MakePathRelativeTo(Record.File, *(FPaths::ProjectDir()));
			Record.BatchId = Object->GetStringField(TEXT("batch_id"));
			Record.Prompt = Object->GetStringField(TEXT("prompt"));
			Record.ModelId = Object->GetStringField(TEXT("model_id"));
			Record.GeneratedUtc = Object->GetStringField(TEXT("generated_utc"));
			Object->TryGetNumberField(TEXT("seed"), Record.Seed);
			Object->TryGetNumberField(TEXT("width"), Record.Width);
			Object->TryGetNumberField(TEXT("height"), Record.Height);
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

	bool BuildRequestBody(const FHyperAIStudioImageModelSettings& Model, const FString& Prompt, const FString& Negative,
		const int32 Width, const int32 Height, const int32 Seed, FString& OutBody, FString& OutError)
	{
		OutBody.Reset();
		OutError.Reset();
		if (Model.RequestTemplate.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Model %s has no request_template; see the cookbook for three worked shapes."), *Model.Id);
			return false;
		}
		OutBody = Model.RequestTemplate;
		OutBody = OutBody.Replace(TEXT("{prompt}"), *EscapeForJsonString(Prompt), ESearchCase::IgnoreCase);
		OutBody = OutBody.Replace(TEXT("{negative}"), *EscapeForJsonString(Negative), ESearchCase::IgnoreCase);
		OutBody = OutBody.Replace(TEXT("{model}"), *EscapeForJsonString(Model.Model), ESearchCase::IgnoreCase);
		OutBody = OutBody.Replace(TEXT("{width}"), *FString::FromInt(Width), ESearchCase::IgnoreCase);
		OutBody = OutBody.Replace(TEXT("{height}"), *FString::FromInt(Height), ESearchCase::IgnoreCase);
		OutBody = OutBody.Replace(TEXT("{seed}"), *FString::FromInt(Seed), ESearchCase::IgnoreCase);
		// A template that does not parse would be rejected by the endpoint with a message nobody can act on,
		// so it is caught here where the setting that caused it can be named.
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutBody);
		if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
		{
			OutBody.Reset();
			OutError = FString::Printf(
				TEXT("Model %s produced a request body that is not valid JSON. Check request_template: {prompt} belongs inside a string's quotes, {width} outside them."),
				*Model.Id);
			return false;
		}
		if (OutBody.Len() * 2 > FHyperAIStudioExternalServices::MaxRequestBodyBytes)
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
		TSharedPtr<FJsonValue> Current = Root;
		for (const FString& Step : Steps)
		{
			if (!Current.IsValid())
			{
				break;
			}
			if (Step.IsNumeric())
			{
				const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
				const int32 Index = FCString::Atoi(*Step);
				if (!Current->TryGetArray(Array) || !Array->IsValidIndex(Index))
				{
					Current.Reset();
					break;
				}
				Current = (*Array)[Index];
				continue;
			}
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Current->TryGetObject(Object) || !(*Object)->HasField(Step))
			{
				Current.Reset();
				break;
			}
			Current = (*Object)->TryGetField(Step);
		}
		FString Encoded;
		if (!Current.IsValid() || !Current->TryGetString(Encoded) || Encoded.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("Nothing at response_image_path %s in the reply: %s"), *DottedPath, *Text.Left(200));
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
		const int32 Seed, FString& OutError)
	{
		OutError.Reset();
		if (Batches().Contains(BatchId))
		{
			OutError = FString::Printf(TEXT("A batch named %s is already known this session."), *BatchId);
			return false;
		}
		const TSharedPtr<FBatch> Batch = MakeShared<FBatch>();
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
		FHyperAIStudioExternalServiceSettings Service;
		if (FHyperAIStudioExternalServices::Find(Model.ServiceId, Service))
		{
			Batch->CostPerCall = Service.CostPerCall;
		}
		if (!IFileManager::Get().MakeDirectory(*Batch->FolderOnDisk, /*Tree=*/true))
		{
			OutError = FString::Printf(TEXT("Could not make %s."), *Batch->FolderOnDisk);
			return false;
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
		FString JobId;
		const bool bStarted = FHyperAIStudioAsyncJobHost::Start(Job,
			[Kept](FString& OutProgress, FString& OutDiagnostic)
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
				if (!Kept->bInFlight && Kept->Sent < Kept->Count)
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
