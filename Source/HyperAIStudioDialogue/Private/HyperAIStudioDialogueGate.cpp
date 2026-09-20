// Games by Hyper 2026.

#include "HyperAIStudioDialogueGate.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "AssetImportTask.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioVoice.h"
#include "IAssetTools.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/DialogueVoice.h"
#include "Sound/DialogueWave.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDialogue, Log, All);

namespace HyperAIStudio::Dialogue::Gate
{
	namespace
	{
		using Contracts = FHyperAIStudioDialogueContracts;

		/** One render in flight or finished, kept for the session so an import can be checked against it. */
		struct FRender
		{
			FString RenderId;
			FString Folder;
			TArray<FHyperAIDialogueLine> Lines;
			TArray<FString> Files;
			int32 Sent = 0;
			int32 Done = 0;
			bool bInFlight = false;
			bool bFailed = false;
			FString Error;
			float SecondsOfAudio = 0.0f;
		};

		TMap<FString, TSharedPtr<FRender>>& Renders()
		{
			static TMap<FString, TSharedPtr<FRender>> Storage;
			return Storage;
		}

		FString RenderRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Dialogue"));
		}

		bool IsIdentifier(const FString& Name, const int32 MaxLength = 64)
		{
			if (Name.IsEmpty() || Name.Len() > MaxLength) return false;
			for (const TCHAR Character : Name)
			{
				const bool bAllowed = (Character >= TEXT('a') && Character <= TEXT('z'))
					|| (Character >= TEXT('A') && Character <= TEXT('Z'))
					|| (Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('_');
				if (!bAllowed) return false;
			}
			return !(Name[0] >= TEXT('0') && Name[0] <= TEXT('9'));
		}

		FString LineName(const TArray<FHyperAIDialogueLine>& Lines, const int32 Index)
		{
			const FHyperAIDialogueLine& Line = Lines[Index];
			if (!Line.Name.IsEmpty())
			{
				return Line.Name;
			}
			const FString Voice = FPackageName::ObjectPathToObjectName(Line.Voice);
			return FString::Printf(TEXT("%s_%02d"), Voice.IsEmpty() ? TEXT("Line") : *Voice, Index + 1);
		}

		const UHyperAIStudioSettings& Settings()
		{
			return *GetDefault<UHyperAIStudioSettings>();
		}

		FString HostVoiceFolder()
		{
			const FString Configured = Settings().VoiceReferenceFolder;
			return Configured.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Configured);
		}
	}

	FString BuildPrompt(const FString& SpeakerDescription, const TArray<FHyperAIDialogueLine>& Segments)
	{
		FString Speaker = SpeakerDescription.TrimStartAndEnd();
		while (Speaker.EndsWith(TEXT(",")))
		{
			Speaker.LeftChopInline(1);
			Speaker.TrimEndInline();
		}
		TArray<FString> Parts;
		for (const FHyperAIDialogueLine& Segment : Segments)
		{
			// A double quote inside the dialogue would close the span, and the rest of the line would be
			// performed as stage direction instead of spoken. Demote it, exactly as the service does.
			FString Text = Segment.Text.TrimStartAndEnd().Replace(TEXT("\""), TEXT("'"));
			if (Text.IsEmpty())
			{
				continue;
			}
			FString Direction = Segment.Direction.TrimStartAndEnd();
			while (Direction.EndsWith(TEXT(",")) || Direction.EndsWith(TEXT(".")))
			{
				Direction.LeftChopInline(1);
				Direction.TrimEndInline();
			}
			// The first direction reads as part of the description sentence; later ones are their own.
			if (!Direction.IsEmpty() && Parts.Num() > 0)
			{
				Direction = Direction.Left(1).ToUpper() + Direction.RightChop(1);
			}
			Parts.Add(Direction.IsEmpty()
				? FString::Printf(TEXT("\"%s\""), *Text)
				: FString::Printf(TEXT("%s, \"%s\""), *Direction, *Text));
		}
		if (Parts.IsEmpty())
		{
			return FString();
		}
		const FString Body = FString::Join(Parts, TEXT(" "));
		return Speaker.IsEmpty() ? Body : Speaker + TEXT(", ") + Body;
	}

	UHyperAIStudioVoice* ResolveVoice(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		if (UHyperAIStudioVoice* Loaded = FindObject<UHyperAIStudioVoice>(nullptr, *Path))
		{
			return Loaded;
		}
		return LoadObject<UHyperAIStudioVoice>(nullptr, *Path);
	}

	TArray<FString> ReadAvailableClips()
	{
		TArray<FString> Clips;
		const FString Folder = HostVoiceFolder();
		if (Folder.IsEmpty())
		{
			return Clips;
		}
		IFileManager::Get().FindFiles(Clips, *(Folder / TEXT("*.wav")), /*Files=*/true, /*Directories=*/false);
		Clips.Sort();
		return Clips;
	}

	bool ResolveReferenceClip(const UHyperAIStudioVoice& Voice, FString& OutServicePath, FString& OutError)
	{
		OutServicePath.Reset();
		OutError.Reset();
		if (Voice.ReferenceClip.IsEmpty())
		{
			OutError = TEXT("This voice names no reference clip.");
			return false;
		}
		if (Voice.ReferenceClip.Contains(TEXT("/")) || Voice.ReferenceClip.Contains(TEXT("\\")))
		{
			OutError = TEXT("A reference clip is a file name inside the voice reference folder, not a path.");
			return false;
		}
		const FString Folder = HostVoiceFolder();
		if (Folder.IsEmpty())
		{
			OutError = TEXT("Set Voice Reference Folder in HyperAIStudio settings to the folder holding the clips.");
			return false;
		}
		if (!IFileManager::Get().FileExists(*(Folder / Voice.ReferenceClip)))
		{
			OutError = FString::Printf(TEXT("%s is not in %s."), *Voice.ReferenceClip, *Folder);
			return false;
		}
		// The service resolves this path itself, and a container sees the folder at its own mount point.
		const FString ServiceFolder = Settings().VoiceReferenceServiceFolder;
		OutServicePath = ServiceFolder.IsEmpty()
			? Folder / Voice.ReferenceClip
			: ServiceFolder / Voice.ReferenceClip;
		return true;
	}

	TArray<FHyperAIDialogueVoiceRecord> ReadVoices()
	{
		TArray<FHyperAIDialogueVoiceRecord> Records;
		const FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> Assets;
		Registry.Get().GetAssetsByClass(UHyperAIStudioVoice::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
		const FString Folder = HostVoiceFolder();
		for (const FAssetData& Asset : Assets)
		{
			const UHyperAIStudioVoice* Voice = Cast<UHyperAIStudioVoice>(Asset.GetAsset());
			if (!Voice)
			{
				continue;
			}
			FHyperAIDialogueVoiceRecord& Record = Records.AddDefaulted_GetRef();
			Record.Path = Asset.GetSoftObjectPath().ToString();
			Record.ReferenceClip = Voice->ReferenceClip;
			Record.SpeakerDescription = Voice->SpeakerDescription;
			Record.bClipPresent = !Folder.IsEmpty() && !Voice->ReferenceClip.IsEmpty()
				&& IFileManager::Get().FileExists(*(Folder / Voice->ReferenceClip));
		}
		Records.Sort([](const FHyperAIDialogueVoiceRecord& Left, const FHyperAIDialogueVoiceRecord& Right)
		{
			return Left.Path < Right.Path;
		});
		return Records;
	}

	TArray<FHyperAIDialogueIssue> CheckLines(const TArray<FHyperAIDialogueLine>& Lines)
	{
		TArray<FHyperAIDialogueIssue> Issues;
		auto Add = [&Issues](const TCHAR* Code, const bool bError, const FString& Subject, const FString& Message)
		{
			Issues.Add({Code, bError ? TEXT("error") : TEXT("warning"), Subject, Message});
		};
		// Writing an action inside the quotes makes the model say the word instead of performing it.
		static const TCHAR* const SpokenActions[] = {
			TEXT("sigh"), TEXT("gasp"), TEXT("cough"), TEXT("ahem"), TEXT("pfft"), TEXT("chuckle"),
			TEXT("laughs"), TEXT("sighs"), TEXT("pause"), TEXT("beat"), TEXT("clears throat")};

		TMap<FString, int32> UsedNames;
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			const FHyperAIDialogueLine& Line = Lines[Index];
			const FString Subject = FString::Printf(TEXT("line %d"), Index + 1);
			if (Line.Text.TrimStartAndEnd().IsEmpty())
			{
				Add(TEXT("empty_line"), true, Subject, TEXT("A line with no text renders nothing."));
				continue;
			}
			if (Line.Text.Len() > Contracts::MaxTextCharacters)
			{
				Add(TEXT("line_too_long"), true, Subject,
					FString::Printf(TEXT("%d characters is past the %d a single render takes; split it."),
						Line.Text.Len(), Contracts::MaxTextCharacters));
			}
			if (!Line.Name.IsEmpty() && !IsIdentifier(Line.Name))
			{
				Add(TEXT("invalid_name"), true, Subject, TEXT("A line name is letters, digits and underscores."));
			}
			const FString Name = LineName(Lines, Index);
			if (const int32* First = UsedNames.Find(Name))
			{
				Add(TEXT("duplicate_name"), true, Subject,
					FString::Printf(TEXT("%s is already line %d; one would overwrite the other."), *Name, *First + 1));
			}
			UsedNames.Add(Name, Index);

			const UHyperAIStudioVoice* Voice = ResolveVoice(Line.Voice);
			if (!Voice)
			{
				Add(TEXT("voice_not_found"), true, Subject,
					FString::Printf(TEXT("No voice asset at %s."), *Line.Voice));
			}
			else
			{
				FString Reason;
				if (!Voice->IsRenderable(Reason))
				{
					Add(TEXT("voice_incomplete"), true, Line.Voice, Reason);
				}
				FString ServicePath;
				FString ClipError;
				if (!ResolveReferenceClip(*Voice, ServicePath, ClipError))
				{
					Add(TEXT("clip_missing"), true, Line.Voice, ClipError);
				}
			}

			if (Line.Text.Contains(TEXT("\"")))
			{
				Add(TEXT("quote_in_text"), false, Subject,
					TEXT("A double quote would close the spoken span; it is demoted to a single quote before rendering."));
			}
			const FString Lower = Line.Text.ToLower();
			for (const TCHAR* Action : SpokenActions)
			{
				if (Lower.Contains(Action))
				{
					Add(TEXT("action_in_text"), false, Subject,
						FString::Printf(TEXT("\"%s\" appears in the spoken text, so it will be said aloud. Put it in the direction instead."), Action));
					break;
				}
			}
			if (Line.Direction.Contains(TEXT("\"")))
			{
				Add(TEXT("quote_in_direction"), false, Subject,
					TEXT("A direction is never spoken, so quotes in it do nothing."));
			}
		}
		if (Lines.IsEmpty())
		{
			Add(TEXT("no_lines"), true, TEXT("scene"), TEXT("There are no lines to render."));
		}
		else if (Lines.Num() > Contracts::MaxLinesPerRender)
		{
			Add(TEXT("too_many_lines"), true, TEXT("scene"),
				FString::Printf(TEXT("%d lines is past the %d one render takes."), Lines.Num(), Contracts::MaxLinesPerRender));
		}
		return Issues;
	}

	float EstimateRenderSeconds(const TArray<FHyperAIDialogueLine>& Lines)
	{
		int32 Characters = 0;
		for (const FHyperAIDialogueLine& Line : Lines)
		{
			Characters += Line.Text.Len();
		}
		return Lines.Num() * Contracts::EstimatedFixedSecondsPerLine + Characters * Contracts::EstimatedSecondsPerCharacter;
	}

	bool WriteWav(const FString& Path, const TConstArrayView<uint8> Pcm, const int32 SampleRate, FString& OutError)
	{
		OutError.Reset();
		if (Pcm.IsEmpty() || SampleRate <= 0)
		{
			OutError = TEXT("The service returned no audio.");
			return false;
		}
		const int32 DataBytes = Pcm.Num();
		TArray<uint8> File;
		File.Reserve(44 + DataBytes);
		auto Put32 = [&File](const uint32 Value)
		{
			File.Add(Value & 0xFF);
			File.Add((Value >> 8) & 0xFF);
			File.Add((Value >> 16) & 0xFF);
			File.Add((Value >> 24) & 0xFF);
		};
		auto Put16 = [&File](const uint16 Value)
		{
			File.Add(Value & 0xFF);
			File.Add((Value >> 8) & 0xFF);
		};
		auto PutTag = [&File](const char* Tag)
		{
			File.Append(reinterpret_cast<const uint8*>(Tag), 4);
		};
		PutTag("RIFF");
		Put32(36 + DataBytes);
		PutTag("WAVE");
		PutTag("fmt ");
		Put32(16);
		Put16(1);                                  // PCM
		Put16(1);                                  // mono, as the service returns
		Put32(SampleRate);
		Put32(SampleRate * 2);                     // byte rate at 16 bits mono
		Put16(2);                                  // block align
		Put16(16);                                 // bits per sample
		PutTag("data");
		Put32(DataBytes);
		File.Append(Pcm.GetData(), DataBytes);
		if (!FFileHelper::SaveArrayToFile(File, *Path))
		{
			OutError = FString::Printf(TEXT("Could not write %s."), *Path);
			return false;
		}
		return true;
	}

	namespace
	{
		FString BuildRequestJson(const UHyperAIStudioVoice& Voice, const FHyperAIDialogueLine& Line, const FString& ServiceClipPath)
		{
			FString Json;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("speaker_description"), Voice.SpeakerDescription);
			Writer->WriteValue(TEXT("language"), TEXT("en"));
			Writer->WriteValue(TEXT("reference_clip"), ServiceClipPath);
			Writer->WriteValue(TEXT("seed"), Voice.Seed);
			Writer->WriteValue(TEXT("cfg_scale"), Voice.CfgScale);
			Writer->WriteValue(TEXT("stg_scale"), Voice.StgScale);
			Writer->WriteValue(TEXT("duration_multiplier"), Voice.DurationMultiplier);
			Writer->WriteValue(TEXT("breathing_factor"), Voice.BreathingFactor);
			Writer->WriteValue(TEXT("reference_window"), Voice.ReferenceWindow);
			Writer->WriteArrayStart(TEXT("segments"));
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("text"), Line.Text.TrimStartAndEnd());
			Writer->WriteValue(TEXT("direction"), Line.Direction.TrimStartAndEnd());
			Writer->WriteObjectEnd();
			Writer->WriteArrayEnd();
			Writer->WriteObjectEnd();
			Writer->Close();
			return Json;
		}

		/** Sends one line; the poll that called this returns immediately and watches bInFlight. */
		void SendNextLine(const TSharedPtr<FRender>& Render)
		{
			const int32 Index = Render->Sent;
			const FHyperAIDialogueLine& Line = Render->Lines[Index];
			const UHyperAIStudioVoice* Voice = ResolveVoice(Line.Voice);
			FString ServicePath;
			FString Error;
			if (!Voice || !ResolveReferenceClip(*Voice, ServicePath, Error))
			{
				Render->bFailed = true;
				Render->Error = Error.IsEmpty() ? FString::Printf(TEXT("No voice at %s."), *Line.Voice) : Error;
				return;
			}
			const FString Path = Render->Folder / (LineName(Render->Lines, Index) + TEXT(".wav"));
			Render->bInFlight = true;
			const TSharedPtr<FRender> Kept = Render;
			FString PostError;
			const bool bSent = FHyperAIStudioExternalServices::PostJson(
				Contracts::ServiceId, TEXT("/synthesize"), BuildRequestJson(*Voice, Line, ServicePath),
				[Kept, Path](const bool bOk, const TArray<uint8>& Body, const TMap<FString, FString>& Headers, const FString& ResponseError)
				{
					Kept->bInFlight = false;
					if (!bOk)
					{
						Kept->bFailed = true;
						Kept->Error = ResponseError;
						return;
					}
					int32 SampleRate = 48000;
					if (const FString* Rate = Headers.Find(TEXT("X-Sample-Rate")))
					{
						SampleRate = FCString::Atoi(**Rate);
					}
					FString WriteError;
					if (!WriteWav(Path, Body, SampleRate, WriteError))
					{
						Kept->bFailed = true;
						Kept->Error = WriteError;
						return;
					}
					// Two bytes a sample, one channel.
					Kept->SecondsOfAudio += SampleRate > 0 ? Body.Num() / 2.0f / SampleRate : 0.0f;
					Kept->Files.Add(Path);
					++Kept->Done;
				},
				PostError);
			if (!bSent)
			{
				Render->bInFlight = false;
				Render->bFailed = true;
				Render->Error = PostError;
				return;
			}
			++Render->Sent;
		}
	}

	bool StartRender(const FString& SceneName, const TArray<FHyperAIDialogueLine>& Lines, FString& OutRenderId, FString& OutFolder, FString& OutError)
	{
		OutError.Reset();
		const TSharedPtr<FRender> Render = MakeShared<FRender>();
		Render->RenderId = SceneName + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
		Render->Folder = RenderRoot() / Render->RenderId;
		Render->Lines = Lines;
		if (!IFileManager::Get().MakeDirectory(*Render->Folder, /*Tree=*/true))
		{
			OutError = FString::Printf(TEXT("Could not make %s."), *Render->Folder);
			return false;
		}

		FHyperAIStudioAsyncJobRequest Job;
		Job.PackId = Contracts::PackId;
		Job.ToolName = Contracts::RenderToolName;
		Job.Target = Render->Folder;
		// The whole budget: a render sharing the card with the editor can take many times its estimate, and
		// Render refuses batches that would not fit here anyway.
		Job.DeadlineMs = FHyperAIStudioAsyncJobHost::MaxDeadlineMs;
		Job.PollIntervalMs = 250;

		const TSharedPtr<FRender> Kept = Render;
		FString JobId;
		const bool bStarted = FHyperAIStudioAsyncJobHost::Start(Job,
			[Kept](FString& OutProgress, FString& OutDiagnostic)
			{
				OutProgress = FString::Printf(TEXT("%d of %d lines"), Kept->Done, Kept->Lines.Num());
				if (Kept->bFailed)
				{
					OutDiagnostic = Kept->Error;
					return EHyperAIStudioAsyncJobPoll::Failed;
				}
				if (Kept->Done >= Kept->Lines.Num())
				{
					OutDiagnostic = FString::Printf(TEXT("%d lines, %.1f seconds of audio, in %s"),
						Kept->Done, Kept->SecondsOfAudio, *Kept->Folder);
					return EHyperAIStudioAsyncJobPoll::Completed;
				}
				// One at a time: the model owns the card, so a second request would only queue behind this.
				if (!Kept->bInFlight && Kept->Sent < Kept->Lines.Num())
				{
					SendNextLine(Kept);
				}
				return EHyperAIStudioAsyncJobPoll::Running;
			},
			JobId, OutError);
		if (!bStarted)
		{
			return false;
		}
		Renders().Add(Render->RenderId, Render);
		OutRenderId = Render->RenderId;
		OutFolder = Render->Folder;
		return true;
	}

	bool FindRender(const FString& RenderId, FHyperAIDialogueRenderRecord& OutRecord)
	{
		const TSharedPtr<FRender>* Found = Renders().Find(RenderId);
		if (!Found || !Found->IsValid())
		{
			return false;
		}
		const FRender& Render = **Found;
		OutRecord.RenderId = Render.RenderId;
		OutRecord.State = Render.bFailed ? TEXT("failed") : Render.Done >= Render.Lines.Num() ? TEXT("complete") : TEXT("running");
		OutRecord.Progress = Render.bFailed ? Render.Error : FString::Printf(TEXT("%d of %d lines"), Render.Done, Render.Lines.Num());
		OutRecord.LinesDone = Render.Done;
		OutRecord.LinesTotal = Render.Lines.Num();
		OutRecord.Folder = Render.Folder;
		OutRecord.SecondsOfAudio = Render.SecondsOfAudio;
		OutRecord.RenderKey = OutRecord.State == TEXT("complete") ? ComputeRenderKey(RenderId) : FString();
		return true;
	}

	TArray<FHyperAIDialogueRenderRecord> ReadRenders()
	{
		TArray<FHyperAIDialogueRenderRecord> Records;
		for (const TPair<FString, TSharedPtr<FRender>>& Entry : Renders())
		{
			FHyperAIDialogueRenderRecord Record;
			if (FindRender(Entry.Key, Record))
			{
				Records.Add(MoveTemp(Record));
			}
		}
		Records.Sort([](const FHyperAIDialogueRenderRecord& Left, const FHyperAIDialogueRenderRecord& Right)
		{
			return Left.RenderId < Right.RenderId;
		});
		return Records;
	}

	FString ComputeRenderKey(const FString& RenderId)
	{
		const TSharedPtr<FRender>* Found = Renders().Find(RenderId);
		if (!Found || !Found->IsValid())
		{
			return FString();
		}
		// Name, size and time rather than the bytes: a batch is tens of megabytes and this runs on the game thread.
		TArray<FString> Rows;
		for (const FString& File : (*Found)->Files)
		{
			Rows.Add(FString::Printf(TEXT("%s|%lld|%lld"), *FPaths::GetCleanFilename(File),
				IFileManager::Get().FileSize(*File), IFileManager::Get().GetTimeStamp(*File).GetTicks()));
		}
		Rows.Sort();
		FString Canonical = RenderId + TEXT("\n") + FString::Join(Rows, TEXT("\n"));
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	TArray<FString> PlanImportPaths(const FString& RenderId, const FString& TargetFolder, const bool bCreateDialogueWaves)
	{
		TArray<FString> Paths;
		const TSharedPtr<FRender>* Found = Renders().Find(RenderId);
		if (!Found || !Found->IsValid())
		{
			return Paths;
		}
		for (const FString& File : (*Found)->Files)
		{
			const FString Name = FPaths::GetBaseFilename(File);
			Paths.Add(FString::Printf(TEXT("%s/%s.%s"), *TargetFolder, *Name, *Name));
			if (bCreateDialogueWaves)
			{
				Paths.Add(FString::Printf(TEXT("%s/DW_%s.DW_%s"), *TargetFolder, *Name, *Name));
			}
		}
		return Paths;
	}

	namespace
	{
		UDialogueVoice* EnsureDialogueVoice(const FString& TargetFolder, const FString& VoiceAssetPath, FString& OutError)
		{
			const FString VoiceName = FPackageName::ObjectPathToObjectName(VoiceAssetPath);
			const FString Name = TEXT("DV_") + (VoiceName.IsEmpty() ? FString(TEXT("Speaker")) : VoiceName);
			const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *TargetFolder, *Name, *Name);
			if (UDialogueVoice* Existing = LoadObject<UDialogueVoice>(nullptr, *ObjectPath))
			{
				return Existing;
			}
			UPackage* Package = CreatePackage(*FPackageName::ObjectPathToPackageName(ObjectPath));
			if (!Package)
			{
				OutError = FString::Printf(TEXT("Could not make a package for %s."), *ObjectPath);
				return nullptr;
			}
			UDialogueVoice* Created = NewObject<UDialogueVoice>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
			if (!Created)
			{
				OutError = FString::Printf(TEXT("Could not create %s."), *ObjectPath);
				return nullptr;
			}
			FAssetRegistryModule::AssetCreated(Created);
			Package->MarkPackageDirty();
			return Created;
		}
	}

	bool ImportRender(const FString& RenderId, const FString& TargetFolder, const bool bCreateDialogueWaves,
		TArray<UObject*>& OutCreated, FString& OutError)
	{
		OutCreated.Reset();
		OutError.Reset();
		const TSharedPtr<FRender>* Found = Renders().Find(RenderId);
		if (!Found || !Found->IsValid())
		{
			OutError = FString::Printf(TEXT("No render called %s."), *RenderId);
			return false;
		}
		const FRender& Render = **Found;
		if (Render.Files.IsEmpty())
		{
			OutError = TEXT("That render produced no audio.");
			return false;
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		for (int32 Index = 0; Index < Render.Files.Num(); ++Index)
		{
			const FString& File = Render.Files[Index];
			const FString Name = FPaths::GetBaseFilename(File);
			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = File;
			Task->DestinationPath = TargetFolder;
			Task->DestinationName = Name;
			Task->bAutomated = true;
			Task->bReplaceExisting = true;
			Task->bSave = false;
			AssetTools.ImportAssetTasks({Task});
			USoundWave* Wave = Task->GetObjects().IsEmpty() ? nullptr : Cast<USoundWave>(Task->GetObjects()[0]);
			if (!Wave)
			{
				OutError = FString::Printf(TEXT("Could not import %s."), *File);
				return false;
			}
			OutCreated.Add(Wave);

			if (!bCreateDialogueWaves)
			{
				continue;
			}
			const FHyperAIDialogueLine* Line = nullptr;
			for (int32 Candidate = 0; Candidate < Render.Lines.Num(); ++Candidate)
			{
				if (LineName(Render.Lines, Candidate) == Name)
				{
					Line = &Render.Lines[Candidate];
					break;
				}
			}
			UDialogueVoice* Speaker = Line ? EnsureDialogueVoice(TargetFolder, Line->Voice, OutError) : nullptr;
			if (!Speaker)
			{
				OutError = OutError.IsEmpty() ? FString::Printf(TEXT("No speaker for %s."), *Name) : OutError;
				return false;
			}
			OutCreated.Add(Speaker);

			const FString DialogueName = TEXT("DW_") + Name;
			const FString DialoguePath = FString::Printf(TEXT("%s/%s.%s"), *TargetFolder, *DialogueName, *DialogueName);
			UPackage* Package = CreatePackage(*FPackageName::ObjectPathToPackageName(DialoguePath));
			UDialogueWave* Dialogue = Package
				? NewObject<UDialogueWave>(Package, FName(*DialogueName), RF_Public | RF_Standalone | RF_Transactional)
				: nullptr;
			if (!Dialogue)
			{
				OutError = FString::Printf(TEXT("Could not create %s."), *DialoguePath);
				return false;
			}
			Dialogue->SpokenText = Line->Text;
			FDialogueContextMapping Mapping;
			Mapping.Context.Speaker = Speaker;
			Mapping.SoundWave = Wave;
			Dialogue->ContextMappings = {Mapping};
			FAssetRegistryModule::AssetCreated(Dialogue);
			Package->MarkPackageDirty();
			OutCreated.Add(Dialogue);
		}
		return true;
	}

	TArray<UPackage*> PackagesOf(const TArray<UObject*>& Objects)
	{
		TArray<UPackage*> Packages;
		for (UObject* Object : Objects)
		{
			if (Object && Object->GetOutermost())
			{
				Packages.AddUnique(Object->GetOutermost());
			}
		}
		return Packages;
	}

	void Shutdown()
	{
		Renders().Reset();
	}
}
