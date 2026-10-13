// Games by Hyper 2026.

#include "HyperAIStudioMeshGenJob.h"

#include "Algo/AllOf.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetExportTask.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Containers/Ticker.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Exporters/Exporter.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "IAssetTools.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericAssetsPipelineSharedSettings.h"
#include "InterchangeGenericMaterialPipeline.h"
#include "InterchangeGenericTexturePipeline.h"
#include "InterchangeManager.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PhysicsAssetUtils.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ReferenceSkeleton.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMeshGenCharacters, Log, All);

/**
 * Rigging, animating and texturing models with Meshy, Tripo and Viggle, each call as the provider documents it (October
 * 2026): Meshy's Rigging, Animation, Text to Motion and Retexture APIs (docs.meshy.ai/api); Tripo's v3 rig, retarget and
 * texture endpoints (developers.tripo3d.ai/en/docs); Viggle's motions from text (docs.viggle.ai/v1).
 */
namespace HyperAIStudio::MeshGen::Characters
{
	using namespace HyperAIStudio::MeshGen::Private;

	const TCHAR* const RigProviders[] = {TEXT("meshy"), TEXT("tripo")};
	constexpr const TCHAR* TripoRigV1 = TEXT("v1.0-20240301");
	constexpr const TCHAR* TripoRigV25 = TEXT("v2.5-20260210");
	constexpr const TCHAR* TripoTextureModel = TEXT("v3.0-20250812");
	/** Tripo uploads one file up to 60 MB; larger ones need its presigned upload, which this does not use. */
	constexpr int64 MaxTripoUploadBytes = 60 * 1024 * 1024;

	struct FLibraryAction
	{
		const TCHAR* Id;
		const TCHAR* Name;
		const TCHAR* Category;
	};

	/**
	 * The first forty actions of Meshy's library, as its docs list them (docs.meshy.ai/en/api/animation-library). The rest
	 * of the library, some 680 actions, is read by the first Meshy animation job and kept on this computer.
	 */
	const FLibraryAction MeshyBuiltIn[] = {
		{TEXT("0"), TEXT("Idle"), TEXT("DailyActions")}, {TEXT("1"), TEXT("Walking_Woman"), TEXT("WalkAndRun")},
		{TEXT("2"), TEXT("Alert"), TEXT("DailyActions")}, {TEXT("3"), TEXT("Arise"), TEXT("DailyActions")},
		{TEXT("4"), TEXT("Attack"), TEXT("Fighting")}, {TEXT("5"), TEXT("BackLeft_run"), TEXT("WalkAndRun")},
		{TEXT("6"), TEXT("BackRight_Run"), TEXT("WalkAndRun")}, {TEXT("7"), TEXT("BeHit_FlyUp"), TEXT("Fighting")},
		{TEXT("8"), TEXT("Dead"), TEXT("Fighting")}, {TEXT("9"), TEXT("ForwardLeft_Run_Fight"), TEXT("Fighting")},
		{TEXT("10"), TEXT("ForwardRight_Run_Fight"), TEXT("Fighting")}, {TEXT("11"), TEXT("Idle_02"), TEXT("DailyActions")},
		{TEXT("12"), TEXT("Idle_03"), TEXT("DailyActions")}, {TEXT("13"), TEXT("Jump_Run"), TEXT("WalkAndRun")},
		{TEXT("14"), TEXT("Run_02"), TEXT("WalkAndRun")}, {TEXT("15"), TEXT("Run_03"), TEXT("WalkAndRun")},
		{TEXT("16"), TEXT("RunFast"), TEXT("WalkAndRun")}, {TEXT("17"), TEXT("Skill_01"), TEXT("BodyMovements")},
		{TEXT("18"), TEXT("Skill_02"), TEXT("BodyMovements")}, {TEXT("19"), TEXT("Skill_03"), TEXT("BodyMovements")},
		{TEXT("20"), TEXT("Walk_Fight_Back"), TEXT("WalkAndRun")}, {TEXT("21"), TEXT("Walk_Fight_Forward"), TEXT("WalkAndRun")},
		{TEXT("22"), TEXT("FunnyDancing_01"), TEXT("Dancing")}, {TEXT("23"), TEXT("FunnyDancing_02"), TEXT("Dancing")},
		{TEXT("24"), TEXT("FunnyDancing_03"), TEXT("Dancing")}, {TEXT("25"), TEXT("Agree_Gesture"), TEXT("DailyActions")},
		{TEXT("26"), TEXT("Angry_Stomp"), TEXT("DailyActions")}, {TEXT("27"), TEXT("Big_Heart_Gesture"), TEXT("BodyMovements")},
		{TEXT("28"), TEXT("Big_Wave_Hello"), TEXT("DailyActions")}, {TEXT("29"), TEXT("Call_Gesture"), TEXT("BodyMovements")},
		{TEXT("30"), TEXT("Casual_Walk"), TEXT("WalkAndRun")}, {TEXT("31"), TEXT("Catching_Breath"), TEXT("BodyMovements")},
		{TEXT("32"), TEXT("Chair_Sit_Idle_F"), TEXT("DailyActions")}, {TEXT("33"), TEXT("Chair_Sit_Idle_M"), TEXT("DailyActions")},
		{TEXT("34"), TEXT("Checkout_Gesture"), TEXT("DailyActions")}, {TEXT("35"), TEXT("Clapping_Run"), TEXT("BodyMovements")},
		{TEXT("36"), TEXT("Confused_Scratch"), TEXT("DailyActions")}, {TEXT("37"), TEXT("Discuss_While_Moving"), TEXT("DailyActions")},
		{TEXT("38"), TEXT("Dozing_Elderly"), TEXT("DailyActions")}, {TEXT("39"), TEXT("Excited_Walk_F"), TEXT("BodyMovements")}};

	/** Tripo's presets for its v2.5 rig, every kind of creature (developers.tripo3d.ai/en/docs/animations-retarget). */
	const TCHAR* const TripoV25[] = {TEXT("idle"), TEXT("walk"), TEXT("run"), TEXT("dive"), TEXT("climb"), TEXT("jump"), TEXT("slash"),
		TEXT("shoot"), TEXT("hurt"), TEXT("fall"), TEXT("turn"), TEXT("quadruped:walk"), TEXT("hexapod:walk"), TEXT("octopod:walk"),
		TEXT("serpentine:march"), TEXT("aquatic:march")};

	/** Tripo's presets for its v1.0 rig, humanoids only, each preset:biped:<name>. */
	const TCHAR* const TripoBiped[] = {
		TEXT("afraid"), TEXT("agree"), TEXT("angry_01"), TEXT("angry_02"), TEXT("angry_03"), TEXT("basketball_shot"), TEXT("bow"),
		TEXT("box_01"), TEXT("box_02"), TEXT("box_03"), TEXT("cast_a_spell"), TEXT("cheer"), TEXT("chop"), TEXT("clap"), TEXT("climb"),
		TEXT("complain_01"), TEXT("complain_02"), TEXT("cross_body_crunch"), TEXT("crossover_dribble"), TEXT("cry"), TEXT("dance_01"),
		TEXT("dance_02"), TEXT("dance_03"), TEXT("dance_04"), TEXT("dance_05"), TEXT("dance_06"), TEXT("defeat_02"), TEXT("defeat_03"),
		TEXT("depressed"), TEXT("dig"), TEXT("dive"), TEXT("dribble"), TEXT("fall"), TEXT("fire"), TEXT("flee_01"), TEXT("flee_02"),
		TEXT("flip"), TEXT("fold_arms"), TEXT("football_catch"), TEXT("football_save"), TEXT("football_pass"), TEXT("freaky"),
		TEXT("frightened"), TEXT("front_kick_01"), TEXT("front_kick_02"), TEXT("frustrated_01"), TEXT("frustrated_02"), TEXT("golf"),
		TEXT("greet_01"), TEXT("greet_02"), TEXT("greet_03"), TEXT("greet_04"), TEXT("heart_pose"), TEXT("hit_to_body_01"),
		TEXT("hit_to_body_02"), TEXT("hit_to_head"), TEXT("hit_to_side"), TEXT("hit_to_stomach"), TEXT("hug"), TEXT("hurt"),
		TEXT("idle"), TEXT("jump_down"), TEXT("jump"), TEXT("jump_rope_01"), TEXT("jump_rope_02"), TEXT("laugh_01"), TEXT("laugh_02"),
		TEXT("lift_heavy"), TEXT("look_around"), TEXT("make_a_call_01"), TEXT("make_a_call_02"), TEXT("pitch_baseball"),
		TEXT("play_mobile_game"), TEXT("play_video_game"), TEXT("press-up"), TEXT("run_upstairs"), TEXT("run"), TEXT("scared_01"),
		TEXT("scared_02"), TEXT("scratch"), TEXT("shoot"), TEXT("shovel"), TEXT("sing_01"), TEXT("sing_02"), TEXT("sing_03"),
		TEXT("sing_04"), TEXT("sit"), TEXT("slash"), TEXT("sob"), TEXT("standing_relax"), TEXT("surf"), TEXT("swagger"), TEXT("swim"),
		TEXT("turn"), TEXT("victory_celebration"), TEXT("volleyball"), TEXT("wait"), TEXT("walk"), TEXT("warm_up"),
		TEXT("wave_goodbye_01"), TEXT("wave_goodbye_02")};

	/** Lower case, with spaces, underscores and hyphens dropped: Meshy's names have spaces where its keys have underscores. */
	FString Normal(const FString& Name)
	{
		FString Out;
		for (const TCHAR Char : Name.TrimStartAndEnd())
		{
			if (Char != TEXT(' ') && Char != TEXT('_') && Char != TEXT('-'))
			{
				Out.AppendChar(FChar::ToLower(Char));
			}
		}
		return Out;
	}

	FHyperAIMeshAction MakeAction(const FString& Provider, const FString& Id, const FString& Name, const FString& Category)
	{
		FHyperAIMeshAction Action;
		Action.Provider = Provider;
		Action.Id = Id;
		Action.Name = Name;
		Action.Category = Category;
		return Action;
	}

	FString LibraryFile()
	{
		return ModelsRoot() / TEXT("meshy_animation_library.json");
	}

	/** Meshy's library as GET /openapi/v1/animations/library answers it: an array of {action_id, name, key, category}. */
	bool ParseLibrary(const FString& Text, TArray<FHyperAIMeshAction>& Out)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Items))
		{
			TSharedPtr<FJsonObject> Wrapped;
			const TArray<TSharedPtr<FJsonValue>>* Inner = nullptr;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Wrapped) || !Wrapped.IsValid())
			{
				return false;
			}
			for (const TCHAR* Name : {TEXT("result"), TEXT("data"), TEXT("items"), TEXT("animations")})
			{
				if (!Inner && Wrapped->TryGetArrayField(Name, Inner))
				{
					break;
				}
			}
			if (!Inner)
			{
				return false;
			}
			Items = *Inner;
		}
		for (const TSharedPtr<FJsonValue>& Item : Items)
		{
			const TSharedPtr<FJsonObject> Object = Item.IsValid() ? Item->AsObject() : nullptr;
			double Id = -1.0;
			if (!Object.IsValid() || !Object->TryGetNumberField(TEXT("action_id"), Id) || Id < 0.0)
			{
				continue;
			}
			FHyperAIMeshAction& Action = Out.AddDefaulted_GetRef();
			Action.Provider = TEXT("meshy");
			Action.Id = FString::FromInt(static_cast<int32>(Id));
			Action.Name = Object->GetStringField(TEXT("name"));
			const FString Key = Object->GetStringField(TEXT("key"));
			Action.Name = Action.Name.IsEmpty() ? Key : Key.IsEmpty() || Normal(Key) == Normal(Action.Name) ? Action.Name
				: FString::Printf(TEXT("%s (%s)"), *Action.Name, *Key);
			Action.Category = Object->GetStringField(TEXT("category"));
		}
		return true;
	}

	/** Meshy's actions this editor knows: the built-in ones, and the whole library once a job has read it. */
	TArray<FHyperAIMeshAction> MeshyLibrary(bool* bOutCached = nullptr)
	{
		TArray<FHyperAIMeshAction> Actions;
		FString Text;
		const bool bCached = FFileHelper::LoadFileToString(Text, *LibraryFile()) && ParseLibrary(Text, Actions) && !Actions.IsEmpty();
		if (bOutCached)
		{
			*bOutCached = bCached;
		}
		if (!bCached)
		{
			Actions.Reset();
			for (const FLibraryAction& Row : MeshyBuiltIn)
			{
				FHyperAIMeshAction& Action = Actions.AddDefaulted_GetRef();
				Action.Provider = TEXT("meshy");
				Action.Id = Row.Id;
				Action.Name = Row.Name;
				Action.Category = Row.Category;
			}
		}
		return Actions;
	}

	/** A Meshy action by id, or by name or key; INDEX_NONE when this editor does not know the name yet. */
	int32 MeshyActionId(const FString& Action, const TArray<FHyperAIMeshAction>& Library)
	{
		const FString Trimmed = Action.TrimStartAndEnd();
		if (Trimmed.IsNumeric() && !Trimmed.Contains(TEXT(".")) && !Trimmed.StartsWith(TEXT("-")))
		{
			return FCString::Atoi(*Trimmed);
		}
		const FString Wanted = Normal(Trimmed);
		for (const FHyperAIMeshAction& Entry : Library)
		{
			// A cached entry reads "Name (Key)" when Meshy's name and key differ.
			FString Name = Entry.Name;
			FString Key;
			if (Entry.Name.Split(TEXT(" ("), &Name, &Key))
			{
				Key.RemoveFromEnd(TEXT(")"));
			}
			if (Normal(Name) == Wanted || (!Key.IsEmpty() && Normal(Key) == Wanted))
			{
				return FCString::Atoi(*Entry.Id);
			}
		}
		return INDEX_NONE;
	}

	/** A content path, /Game/A/B or /Game/A/B.B, as the object it names; null when it is not a T. */
	template <typename T>
	T* LoadAsset(const FString& Path)
	{
		FString Object = Path.TrimStartAndEnd();
		if (Object.IsEmpty() || !FHyperAIStudioNames::IsObjectPath(Object, TEXT("path")))
		{
			return nullptr;
		}
		if (!Object.Contains(TEXT(".")))
		{
			Object += TEXT(".") + FPackageName::GetShortName(Object);
		}
		return Cast<T>(StaticLoadObject(T::StaticClass(), nullptr, *Object, nullptr, LOAD_NoWarn | LOAD_Quiet));
	}

	FString PackageOf(const FString& ObjectPath)
	{
		return FPackageName::ObjectPathToPackageName(ObjectPath);
	}

	TArray<FString> BoneNames(const USkeletalMesh& Mesh)
	{
		TArray<FString> Names;
		const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
		for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
		{
			Names.Add(Skeleton.GetBoneName(Index).ToString());
		}
		return Names;
	}

	/** Each provider for these tasks, and whether its key's variable is set. */
	TArray<FString> DescribeProviders(const TArray<FString>& Ids)
	{
		TArray<FString> Lines;
		for (const FString& Id : Ids)
		{
			FHyperAIStudioExternalServiceSettings Service;
			const bool bFound = FHyperAIStudioExternalServices::Find(Id, Service);
			Lines.Add(FString::Printf(TEXT("%s: %s %s"), *Id, *Service.ApiKeyVariable, bFound && !Key(Service).IsEmpty() ? TEXT("is set") : TEXT("is not set")));
		}
		return Lines;
	}

	bool HasKey(const FString& Id)
	{
		FHyperAIStudioExternalServiceSettings Service;
		return FHyperAIStudioExternalServices::Find(Id, Service) && !Key(Service).IsEmpty();
	}

	/** A data URI the provider reads the file from, as Meshy takes a model or a picture inline. */
	FString DataUri(const FString& File, const FString& Mime)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *File);
		return FString::Printf(TEXT("data:%s;base64,%s"), *Mime, *FBase64::Encode(Bytes));
	}

	FString ImageMime(const FString& File)
	{
		const FString Extension = FPaths::GetExtension(File).ToLower();
		return Extension == TEXT("png") ? TEXT("image/png") : Extension == TEXT("webp") ? TEXT("image/webp") : TEXT("image/jpeg");
	}

	// ---------------------------------------------------------------- the provider's tasks

	FTask MeshyTask(const FJob& Job, const TCHAR* Label, const TCHAR* Family, const double Cost)
	{
		FTask Task;
		Task.Label = Label;
		Task.CreateUrl = Job.Service.BaseUrl + TEXT("/openapi/v1/") + Family;
		Task.QueryUrl = Task.CreateUrl + TEXT("/{id}");
		Task.TaskIdPath = TEXT("result");
		Task.StatePath = TEXT("status");
		Task.ProgressPath = TEXT("progress");
		Task.Cost = Cost;
		return Task;
	}

	FTask TripoTask(const FJob& Job, const TCHAR* Label, const TCHAR* Path, const double Cost)
	{
		FTask Task;
		Task.Label = Label;
		Task.CreateUrl = Job.Service.BaseUrl + Path;
		Task.QueryUrl = Job.Service.BaseUrl + TEXT("/tasks/{id}");
		Task.TaskIdPath = TEXT("data.task_id");
		Task.StatePath = TEXT("data.status");
		Task.ProgressPath = TEXT("data.progress");
		Task.Cost = Cost;
		return Task;
	}

	/** Adds a download for the first of Paths the result has; false when it has none. */
	bool AddDownload(FJob& Job, const TSharedPtr<FJsonObject>& Json, std::initializer_list<const TCHAR*> Paths, const TCHAR* Role)
	{
		for (const TCHAR* Path : Paths)
		{
			const FString Url = Field(Json, Path);
			if (Url.StartsWith(TEXT("http://")) || Url.StartsWith(TEXT("https://")))
			{
				FDownload& Download = Job.Downloads.AddDefaulted_GetRef();
				Download.Url = Url;
				Download.Role = Role;
				return true;
			}
		}
		return false;
	}

	/** Meshy's rig: the rigged character, and the walk and run it makes beside it. */
	FTask MeshyRig(const FJob& Job, const float HeightMeters, const bool bWithClips)
	{
		FTask Task = MeshyTask(Job, TEXT("rig"), TEXT("rigging"), UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("meshy"), TEXT("rig")));
		Task.Body = [HeightMeters](const FJob& Job)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			// A model Meshy made goes by its task; one from the project inline, as a GLB (the only kind it rigs).
			Job.ExportFile.IsEmpty() ? Json->SetStringField(TEXT("input_task_id"), Job.SourceTaskId)
				: Json->SetStringField(TEXT("model_url"), DataUri(Job.ExportFile, TEXT("application/octet-stream")));
			Json->SetNumberField(TEXT("height_meters"), FMath::RoundToDouble(HeightMeters * 100.0) / 100.0);
			return Json;
		};
		Task.OnDone = [bWithClips](FJob& Job, const TSharedPtr<FJsonObject>& Json, FString& OutError)
		{
			Job.RigTaskId = Job.Tasks[Job.TaskIndex].TaskId;
			if (!AddDownload(Job, Json, {TEXT("result.rigged_character_glb_url"), TEXT("result.rigged_character_fbx_url")}, TEXT("model")))
			{
				OutError = TEXT("Meshy finished the rig without a rigged character to download.");
				return false;
			}
			if (bWithClips)
			{
				AddDownload(Job, Json, {TEXT("result.basic_animations.walking_glb_url"), TEXT("result.basic_animations.walking_fbx_url")}, TEXT("animation"));
				AddDownload(Job, Json, {TEXT("result.basic_animations.running_glb_url"), TEXT("result.basic_animations.running_fbx_url")}, TEXT("animation"));
			}
			return true;
		};
		return Task;
	}

	/** Meshy's animation: library actions, or a clip from Text to Motion, put on its rig. */
	FTask MeshyAnimation(const FJob& Job, const double Cost, const int32 MotionTask)
	{
		FTask Task = MeshyTask(Job, TEXT("animation"), TEXT("animations"), Cost);
		Task.Body = [MotionTask](const FJob& Job)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("rig_task_id"), Job.RigTaskId);
			if (MotionTask != INDEX_NONE)
			{
				Json->SetStringField(TEXT("motion_task_id"), Job.Tasks[MotionTask].TaskId);
			}
			else if (Job.ActionIds.Num() == 1)
			{
				Json->SetNumberField(TEXT("action_id"), Job.ActionIds[0]);
			}
			else
			{
				TArray<TSharedPtr<FJsonValue>> Ids;
				for (const int32 Id : Job.ActionIds)
				{
					Ids.Add(MakeShared<FJsonValueNumber>(Id));
				}
				Json->SetArrayField(TEXT("action_ids"), Ids);
			}
			return Json;
		};
		Task.OnDone = [](FJob& Job, const TSharedPtr<FJsonObject>& Json, FString& OutError)
		{
			if (!AddDownload(Job, Json, {TEXT("result.animation_glb_url"), TEXT("result.animation_fbx_url")}, TEXT("animation")))
			{
				OutError = TEXT("Meshy finished the animation without a file to download.");
				return false;
			}
			return true;
		};
		return Task;
	}

	FTask MeshyMotion(const FJob& Job, const FString& Prompt, const float Seconds, const FString& Mode)
	{
		FTask Task = MeshyTask(Job, TEXT("text to motion"), TEXT("text-to-motion"),
			UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("meshy"), Mode == TEXT("swift") ? TEXT("motion_swift") : TEXT("motion")));
		Task.Body = [Prompt, Seconds, Mode](const FJob&)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("prompt"), Prompt);
			Json->SetNumberField(TEXT("duration"), Seconds);
			Json->SetStringField(TEXT("mode"), Mode);
			return Json;
		};
		return Task;
	}

	FTask MeshyRetexture(const FJob& Job, const FString& Prompt, const FString& Level, const bool bPBR, const bool bKeepUVs)
	{
		FTask Task = MeshyTask(Job, TEXT("texture"), TEXT("retexture"),
			UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("meshy"), Level == TEXT("8k") ? TEXT("texture_8k") : TEXT("texture")));
		Task.Body = [Prompt, Level, bPBR, bKeepUVs](const FJob& Job)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Job.ExportFile.IsEmpty() ? Json->SetStringField(TEXT("input_task_id"), Job.SourceTaskId)
				: Json->SetStringField(TEXT("model_url"), DataUri(Job.ExportFile, TEXT("application/octet-stream")));
			Job.ImageFile.IsEmpty() ? Json->SetStringField(TEXT("text_style_prompt"), Prompt)
				: Json->SetStringField(TEXT("image_style_url"), DataUri(Job.ImageFile, ImageMime(Job.ImageFile)));
			Json->SetStringField(TEXT("ai_model"), TEXT("latest"));
			Json->SetBoolField(TEXT("enable_original_uv"), bKeepUVs);
			Json->SetBoolField(TEXT("enable_pbr"), bPBR);
			Json->SetStringField(TEXT("texture_resolution"), Level);
			Json->SetArrayField(TEXT("target_formats"), {MakeShared<FJsonValueString>(TEXT("glb"))});
			return Json;
		};
		Task.OnDone = [](FJob& Job, const TSharedPtr<FJsonObject>& Json, FString& OutError)
		{
			if (!AddDownload(Job, Json, {TEXT("model_urls.glb"), TEXT("model_urls.fbx")}, TEXT("model")))
			{
				OutError = TEXT("Meshy finished the texture without a model to download.");
				return false;
			}
			return true;
		};
		return Task;
	}

	/** Tripo's model input: its own task, or the file token an upload gave. */
	FString TripoInput(const FJob& Job)
	{
		return Job.Token.IsEmpty() ? Job.SourceTaskId : Job.Token;
	}

	FTask TripoRig(const FJob& Job, const FString& Spec)
	{
		FTask Task = TripoTask(Job, TEXT("rig"), TEXT("/animations/rig"), UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("tripo"), TEXT("rig")));
		Task.Body = [Spec](const FJob& Job)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("input"), TripoInput(Job));
			Json->SetStringField(TEXT("model"), Job.RigModel);
			Json->SetStringField(TEXT("rig_type"), TEXT("biped"));
			Json->SetStringField(TEXT("spec"), Spec);
			Json->SetStringField(TEXT("out_format"), TEXT("glb"));
			return Json;
		};
		Task.OnDone = [](FJob& Job, const TSharedPtr<FJsonObject>& Json, FString& OutError)
		{
			Job.RigTaskId = Job.Tasks[Job.TaskIndex].TaskId;
			if (!AddDownload(Job, Json, {TEXT("data.output.model_url")}, TEXT("model")))
			{
				OutError = TEXT("Tripo finished the rig without a model to download.");
				return false;
			}
			return true;
		};
		return Task;
	}

	/** Tripo puts up to five presets on its rig in one task; the result is one file, or one a preset. */
	FTask TripoRetarget(const FJob& Job, const TArray<FString>& Presets)
	{
		FTask Task = TripoTask(Job, TEXT("animation"), TEXT("/animations/retarget"),
			UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("tripo"), TEXT("animate")) * Presets.Num());
		Task.Body = [Presets](const FJob& Job)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("input"), Job.RigTaskId);
			if (Presets.Num() == 1)
			{
				Json->SetStringField(TEXT("animation"), Presets[0]);
			}
			else
			{
				TArray<TSharedPtr<FJsonValue>> List;
				for (const FString& Preset : Presets)
				{
					List.Add(MakeShared<FJsonValueString>(Preset));
				}
				Json->SetArrayField(TEXT("animations"), List);
			}
			Json->SetStringField(TEXT("out_format"), TEXT("glb"));
			Json->SetBoolField(TEXT("bake_animation"), true);
			Json->SetBoolField(TEXT("export_with_geometry"), true);
			return Json;
		};
		Task.OnDone = [](FJob& Job, const TSharedPtr<FJsonObject>& Json, FString& OutError)
		{
			// The reference shows output.model_url; Tripo's own batch example reads output.model_urls.
			if (AddDownload(Job, Json, {TEXT("data.output.model_url")}, TEXT("animation")))
			{
				return true;
			}
			const TSharedPtr<FJsonObject>* Data = nullptr;
			const TSharedPtr<FJsonObject>* Output = nullptr;
			if (Json.IsValid() && Json->TryGetObjectField(TEXT("data"), Data) && (*Data)->TryGetObjectField(TEXT("output"), Output))
			{
				const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
				const TSharedPtr<FJsonObject>* Map = nullptr;
				TArray<FString> Urls;
				if ((*Output)->TryGetArrayField(TEXT("model_urls"), List))
				{
					for (const TSharedPtr<FJsonValue>& Value : *List)
					{
						Urls.Add(Value.IsValid() ? Value->AsString() : FString());
					}
				}
				else if ((*Output)->TryGetObjectField(TEXT("model_urls"), Map))
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Map)->Values)
					{
						Urls.Add(Pair.Value.IsValid() ? Pair.Value->AsString() : FString());
					}
				}
				for (const FString& Url : Urls)
				{
					if (Url.StartsWith(TEXT("http://")) || Url.StartsWith(TEXT("https://")))
					{
						Job.Downloads.Add({Url, TEXT("animation"), FString()});
					}
				}
				if (!Urls.IsEmpty())
				{
					return true;
				}
			}
			OutError = TEXT("Tripo finished the animation without a file to download.");
			return false;
		};
		return Task;
	}

	FTask TripoTexture(const FJob& Job, const FString& Prompt, const FString& Quality, const bool bPBR)
	{
		const TCHAR* Price = Quality == TEXT("extreme") ? TEXT("texture_8k") : Quality == TEXT("detailed") ? TEXT("texture_hd") : TEXT("texture");
		FTask Task = TripoTask(Job, TEXT("texture"), TEXT("/models/texture"), UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("tripo"), Price));
		Task.Body = [Prompt, Quality, bPBR](const FJob& Job)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("input"), TripoInput(Job));
			Json->SetStringField(TEXT("model"), TripoTextureModel);
			const TSharedRef<FJsonObject> Guide = MakeShared<FJsonObject>();
			Guide->SetStringField(TEXT("text"), Prompt);
			Json->SetObjectField(TEXT("texture_prompt"), Guide);
			Json->SetBoolField(TEXT("pbr"), bPBR);
			Json->SetStringField(TEXT("texture_quality"), Quality);
			return Json;
		};
		Task.OnDone = [](FJob& Job, const TSharedPtr<FJsonObject>& Json, FString& OutError)
		{
			if (!AddDownload(Job, Json, {TEXT("data.output.model_url")}, TEXT("model")))
			{
				OutError = TEXT("Tripo finished the texture without a model to download.");
				return false;
			}
			return true;
		};
		return Task;
	}

	/** Viggle's motion from words, then its export, which names a file a skeleton. */
	TArray<FTask> ViggleMotion(const FJob& Job, const FString& Prompt, const float Seconds, const FString& Skeleton)
	{
		FTask Motion;
		Motion.Label = TEXT("text to motion");
		Motion.CreateUrl = Job.Service.BaseUrl + TEXT("/v1/motions");
		Motion.QueryUrl = Motion.CreateUrl + TEXT("/{id}");
		Motion.TaskIdPath = TEXT("id");
		Motion.StatePath = TEXT("status");
		Motion.ProgressPath = TEXT("progress");
		Motion.Cost = UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("viggle"), TEXT("motion"));
		Motion.Body = [Prompt, Seconds](const FJob&)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("text"), Prompt);
			Json->SetNumberField(TEXT("duration_seconds"), Seconds);
			return Json;
		};
		FTask Export;
		Export.Label = TEXT("export");
		Export.QueryUrl = Job.Service.BaseUrl + TEXT("/v1/motions/{id}/export?download_type=glb");
		Export.StatePath = TEXT("status");
		Export.OnDone = [Skeleton](FJob& Job, const TSharedPtr<FJsonObject>& Json, FString& OutError)
		{
			if (!AddDownload(Job, Json, {Skeleton == TEXT("mixamo") ? TEXT("mixamo_url") : TEXT("metahuman_body_url")}, TEXT("animation")))
			{
				OutError = FString::Printf(TEXT("Viggle's export has no %s file."), *Skeleton);
				return false;
			}
			return true;
		};
		return {Motion, Export};
	}

	// ---------------------------------------------------------------- the job's steps

	EPhase After(const FJob& Job, const EPhase Finished)
	{
		if (Finished < EPhase::Export && !Job.ExportMesh.IsEmpty())
		{
			return EPhase::Export;
		}
		if (Finished < EPhase::Upload && Job.bUpload)
		{
			return EPhase::Upload;
		}
		if (Finished < EPhase::Lookup && !Job.Lookups.IsEmpty())
		{
			return EPhase::Lookup;
		}
		return EPhase::Submit;
	}

	void Enter(FJob& Job, const EPhase Phase)
	{
		Job.Phase = Phase;
		const TCHAR* State = Phase == EPhase::Export ? TEXT("exporting") : Phase == EPhase::Upload ? TEXT("uploading")
			: Phase == EPhase::Lookup ? TEXT("looking_up") : Phase == EPhase::Submit ? TEXT("submitting")
			: Phase == EPhase::Generate ? TEXT("generating") : Phase == EPhase::Download ? TEXT("downloading")
			: Phase == EPhase::Import ? TEXT("importing") : Phase == EPhase::Handoff ? TEXT("retargeting") : nullptr;
		if (State)
		{
			Job.Info.State = State;
		}
	}

	/** Writes a project mesh as a GLB through the glTF Exporter plugin, on a tick of its own: exporting is an editor operation. */
	bool ExportGlb(const FString& MeshPath, const FString& File, FString& OutError)
	{
		UObject* Mesh = LoadAsset<UStaticMesh>(MeshPath);
		Mesh = Mesh ? Mesh : LoadAsset<USkeletalMesh>(MeshPath);
		if (!Mesh)
		{
			OutError = FString::Printf(TEXT("%s is no longer a static or skeletal mesh."), *MeshPath);
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
		UAssetExportTask* Task = NewObject<UAssetExportTask>();
		const TStrongObjectPtr<UAssetExportTask> Keep(Task);
		Task->Object = Mesh;
		Task->Filename = File;
		Task->bAutomated = true;
		Task->bPrompt = false;
		Task->bReplaceIdentical = true;
		const bool bWritten = UExporter::RunAssetExportTask(Task) && IFileManager::Get().FileSize(*File) > 0;
		if (!bWritten)
		{
			OutError = FString::Printf(TEXT("%s could not be written as a GLB%s%s. The glTF Exporter plugin writes it; is it enabled?"), *MeshPath,
				Task->Errors.IsEmpty() ? TEXT("") : TEXT(": "), Task->Errors.IsEmpty() ? TEXT("") : *Task->Errors[0]);
			return false;
		}
		return true;
	}

	void Export(const TSharedRef<FJob>& Job)
	{
		Job->bWaiting = true;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Job](float)
		{
			Job->bWaiting = false;
			if (Job->Phase != EPhase::Export)
			{
				return false;
			}
			FString Error;
			Job->ExportFile = ModelsRoot() / Job->Info.OperationId / TEXT("source") / FPackageName::GetShortName(PackageOf(Job->ExportMesh)) + TEXT(".glb");
			if (!ExportGlb(Job->ExportMesh, Job->ExportFile, Error))
			{
				Fail(*Job, Error);
				return false;
			}
			const int64 Size = IFileManager::Get().FileSize(*Job->ExportFile);
			const int64 Limit = Job->bUpload ? MaxTripoUploadBytes : C::MaxUploadBytes;
			if (Size > Limit)
			{
				Fail(*Job, FString::Printf(TEXT("%s is %lld MB as a GLB; %s takes up to %lld MB. Nothing was spent."), *Job->ExportMesh,
					Size / (1024 * 1024), *Job->Service.Label, Limit / (1024 * 1024)));
				return false;
			}
			Enter(*Job, After(*Job, EPhase::Export));
			return false;
		}));
	}

	void Upload(const TSharedRef<FJob>& Job)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Job->ExportFile);
		FForm Form;
		Form.File(TEXT("file"), Job->ExportFile, Bytes);
		TMap<FString, FString> Headers = KeyHeaders(*Job);
		Headers.Add(TEXT("Content-Type"), Form.ContentType());
		Send(Job, TEXT("POST"), Job->Service.BaseUrl + TEXT("/files"), Headers, Form.Finish(), [](FJob& Job, const int32 Code, const TArray<uint8>& Body)
		{
			Job.Token = Field(ParseJson(Body), TEXT("data.file_token"));
			if (Code != 200 || Job.Token.IsEmpty())
			{
				Fail(Job, ProviderError(Job, Code, Body));
				return;
			}
			Enter(Job, After(Job, EPhase::Upload));
		}, 300.f);
	}

	/** Meshy's library, free to read: action names become ids, and the whole library is kept for hyper_mesh_actions. */
	void Lookup(const TSharedRef<FJob>& Job)
	{
		Send(Job, TEXT("GET"), Job->Service.BaseUrl + TEXT("/openapi/v1/animations/library"), KeyHeaders(*Job), {},
			[](FJob& Job, const int32 Code, const TArray<uint8>& Body)
			{
				const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
				const FString Text(Converted.Length(), Converted.Get());
				TArray<FHyperAIMeshAction> Library;
				if (Code != 200 || !ParseLibrary(Text, Library) || Library.IsEmpty())
				{
					Fail(Job, ProviderError(Job, Code, Body) + TEXT(" Meshy's animation library could not be read; nothing was spent."));
					return;
				}
				IFileManager::Get().MakeDirectory(*ModelsRoot(), /*Tree=*/true);
				FFileHelper::SaveStringToFile(Text, *LibraryFile(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
				TArray<FString> Missing;
				for (const FString& Name : Job.Lookups)
				{
					const int32 Id = MeshyActionId(Name, Library);
					Id == INDEX_NONE ? (void)Missing.Add(Name) : (void)Job.ActionIds.AddUnique(Id);
				}
				if (!Missing.IsEmpty())
				{
					Fail(Job, FString::Printf(TEXT("Not in Meshy's animation library: %s. hyper_mesh_actions lists its actions; nothing was spent."),
						*FString::Join(Missing, TEXT(", "))));
					return;
				}
				Job.Lookups.Reset();
				Enter(Job, EPhase::Submit);
			});
	}

	void SubmitTask(const TSharedRef<FJob>& Job)
	{
		FTask& Task = Job->Tasks[Job->TaskIndex];
		if (Task.CreateUrl.IsEmpty())
		{
			// Reads what the task before it made.
			Task.TaskId = Job->TaskIndex > 0 ? Job->Tasks[Job->TaskIndex - 1].TaskId : FString();
			Enter(*Job, EPhase::Generate);
			return;
		}
		Send(Job, TEXT("POST"), Task.CreateUrl, JsonHeaders(*Job), Utf8(JsonText(Task.Body(*Job))), [](FJob& Job, const int32 Code, const TArray<uint8>& Body)
		{
			FTask& Task = Job.Tasks[Job.TaskIndex];
			const TSharedPtr<FJsonObject> Json = ParseJson(Body);
			const FString TaskId = Field(Json, Task.TaskIdPath);
			const FString ProviderCode = Field(Json, TEXT("code"));
			// Meshy answers 202 {result}, Tripo 200 {code: 0, data: {task_id}}, Viggle 200 with the motion.
			if ((Code != 200 && Code != 201 && Code != 202) || TaskId.IsEmpty() || (!ProviderCode.IsEmpty() && ProviderCode != TEXT("0")))
			{
				Fail(Job, ProviderError(Job, Code, Body));
				return;
			}
			// The provider took the task: from here it is paid for.
			Task.TaskId = TaskId;
			Job.Info.TaskId = TaskId;
			Job.Info.Tasks.Add(Task.Label + TEXT(": ") + TaskId);
			if (Task.Cost > 0.0)
			{
				FHyperAIStudioExternalServices::RecordSpend(Job.Service.Id, 1, Task.Cost);
			}
			Enter(Job, EPhase::Generate);
			Job.NextPollAt = FPlatformTime::Seconds() + PollSeconds();
			WriteRecord(Job);
		});
	}

	void QueryTask(const TSharedRef<FJob>& Job)
	{
		const FTask& Task = Job->Tasks[Job->TaskIndex];
		Send(Job, TEXT("GET"), Task.QueryUrl.Replace(TEXT("{id}"), *FGenericPlatformHttp::UrlEncode(Task.TaskId)), KeyHeaders(*Job), {},
			[](FJob& Job, const int32 Code, const TArray<uint8>& Body)
			{
				FTask& Task = Job.Tasks[Job.TaskIndex];
				const TSharedPtr<FJsonObject> Json = ParseJson(Body);
				if (Code != 200 || !Json.IsValid())
				{
					Fail(Job, ProviderError(Job, Code, Body));
					return;
				}
				const FString State = Field(Json, Task.StatePath).ToLower();
				const FString Progress = Task.ProgressPath.IsEmpty() ? FString() : Field(Json, Task.ProgressPath);
				const int32 Own = Progress.IsEmpty() ? 50 : FMath::Clamp(FCString::Atoi(*Progress), 0, 100);
				Job.Info.Progress = (Job.TaskIndex * 100 + Own) / FMath::Max(1, Job.Tasks.Num());
				if (State == TEXT("succeeded") || State == TEXT("success") || State == TEXT("ready"))
				{
					FString Error;
					if (Task.OnDone && !Task.OnDone(Job, Json, Error))
					{
						Fail(Job, Error);
						return;
					}
					++Job.TaskIndex;
					Enter(Job, Job.TaskIndex < Job.Tasks.Num() ? EPhase::Submit : Job.Downloads.IsEmpty() ? EPhase::Import : EPhase::Download);
					WriteRecord(Job);
					return;
				}
				if (State == TEXT("failed") || State == TEXT("banned") || State == TEXT("expired") || State == TEXT("cancelled")
					|| State == TEXT("canceled") || State == TEXT("unknown"))
				{
					Fail(Job, FString::Printf(TEXT("%s's %s task %s: %s"), *Job.Service.Label, *Task.Label, *State, *ProviderError(Job, Code, Body)));
					return;
				}
				Job.NextPollAt = FPlatformTime::Seconds() + PollSeconds();
			}, 60.f);
	}

	/** GLB or FBX, by what the file starts with; empty for anything else. */
	FString ModelExtension(const TArray<uint8>& Body)
	{
		if (Body.Num() >= 12 && FMemory::Memcmp(Body.GetData(), "glTF", 4) == 0)
		{
			return TEXT("glb");
		}
		const FString Head = FString(FMath::Min(Body.Num(), 64), reinterpret_cast<const ANSICHAR*>(Body.GetData()));
		return Head.StartsWith(TEXT("Kaydara FBX Binary")) || Head.Contains(TEXT("FBXHeaderExtension")) || Head.StartsWith(TEXT("; FBX"))
			? FString(TEXT("fbx")) : FString();
	}

	void DownloadNext(const TSharedRef<FJob>& Job)
	{
		// Only addresses the provider's result gave; the links last minutes, so each is fetched as soon as the tasks are done.
		Send(Job, TEXT("GET"), Job->Downloads[Job->DownloadIndex].Url, {}, {}, [](FJob& Job, const int32 Code, const TArray<uint8>& Body)
		{
			FDownload& Download = Job.Downloads[Job.DownloadIndex];
			const FString Extension = ModelExtension(Body);
			if (Code != 200 || Extension.IsEmpty() || Body.Num() > MaxModelBytes)
			{
				Fail(Job, FString::Printf(TEXT("A file from %s could not be downloaded as a GLB or FBX (HTTP %d, %d bytes)."), *Job.Service.Label, Code, Body.Num()));
				return;
			}
			if (Job.DownloadIndex == 0)
			{
				// Named after the asset: Unreal imports a model into a folder named after its file, taken only now.
				Job.AssetName = FreeName(Job.AssetFolder, Job.AssetName, &Job);
			}
			Download.File = ModelsRoot() / Job.Info.OperationId / (Job.DownloadIndex == 0 ? Job.AssetName
				: FString::Printf(TEXT("%s_%s%d"), *Job.AssetName, *Download.Role, Job.DownloadIndex)) + TEXT(".") + Extension;
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Download.File), /*Tree=*/true);
			if (!FFileHelper::SaveArrayToFile(Body, *Download.File))
			{
				Fail(Job, TEXT("A downloaded file could not be written under Saved/HyperAIStudio/Models."));
				return;
			}
			Job.Info.ModelFile = Job.Info.ModelFile.IsEmpty() ? ProjectRelative(Download.File) : Job.Info.ModelFile;
			++Job.DownloadIndex;
			if (Job.DownloadIndex >= Job.Downloads.Num())
			{
				Enter(Job, EPhase::Import);
			}
		}, 300.f);
	}

	// ---------------------------------------------------------------- imports

	TArray<FString> ImportFile(const FString& File, const FString& Folder, const FString& Name, UObject* Options)
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		const TStrongObjectPtr<UAssetImportTask> Keep(Task);
		Task->Filename = File;
		Task->DestinationPath = Folder;
		Task->DestinationName = Name;
		Task->bAutomated = true;
		Task->bReplaceExisting = false;
		Task->bSave = true;
		Task->Options = Options;
		AssetTools.ImportAssetTasks({Task});
		return Task->ImportedObjectPaths;
	}

	/**
	 * Animations only, onto Skeleton, through Interchange's generic pipeline in place of the project's: the clips are
	 * imported, the mesh, materials and textures in the file are not.
	 */
	TArray<FString> ImportAnimations(const FString& File, USkeleton& Skeleton, const FString& Folder, TArray<FString>& OutImported)
	{
		UInterchangeGenericAssetsPipeline* Pipeline = NewObject<UInterchangeGenericAssetsPipeline>(GetTransientPackage());
		const TStrongObjectPtr<UInterchangeGenericAssetsPipeline> KeepPipeline(Pipeline);
		Pipeline->CommonSkeletalMeshesAndAnimationsProperties->Skeleton = &Skeleton;
		Pipeline->CommonSkeletalMeshesAndAnimationsProperties->bImportOnlyAnimations = true;
		if (Pipeline->MaterialPipeline)
		{
			Pipeline->MaterialPipeline->bImportMaterials = false;
			if (Pipeline->MaterialPipeline->TexturePipeline)
			{
				Pipeline->MaterialPipeline->TexturePipeline->bImportTextures = false;
			}
		}
		UInterchangePipelineStackOverride* Override = NewObject<UInterchangePipelineStackOverride>(GetTransientPackage());
		const TStrongObjectPtr<UInterchangePipelineStackOverride> KeepOverride(Override);
		Override->AddPipeline(Pipeline);
		TArray<FString> Animations;
		for (const FString& Path : ImportFile(File, Folder, FString(), Override))
		{
			OutImported.AddUnique(Path);
			const UAnimSequence* Sequence = Cast<UAnimSequence>(StaticLoadObject(UObject::StaticClass(), nullptr, *Path));
			if (Sequence && Sequence->GetSkeleton() == &Skeleton)
			{
				Animations.Add(Path);
			}
		}
		return Animations;
	}

	/** Interchange makes a physics asset with the skeletal mesh; when it did not, one is made the way the editor's Create does. */
	UPhysicsAsset* EnsurePhysicsAsset(USkeletalMesh& Mesh, FString& OutNote)
	{
		if (UPhysicsAsset* Existing = Mesh.GetPhysicsAsset())
		{
			return Existing;
		}
		const FString Package = FPackageName::GetLongPackagePath(Mesh.GetOutermost()->GetName()) / (Mesh.GetName() + TEXT("_PhysicsAsset"));
		if (!FHyperAIStudioNames::IsPackagePath(Package, TEXT("physics asset")) || !FHyperAIStudioNames::IsFreePackage(Package))
		{
			OutNote = TEXT(" No physics asset: its path is taken.");
			return nullptr;
		}
		UPhysicsAsset* Asset = NewObject<UPhysicsAsset>(CreatePackage(*Package), FName(*FPackageName::GetShortName(Package)), RF_Public | RF_Standalone);
		FPhysAssetCreateParams Params;
		FText Error;
		if (!FPhysicsAssetUtils::CreateFromSkeletalMesh(Asset, &Mesh, Params, Error, /*bSetToMesh=*/true, /*bShowProgress=*/false))
		{
			OutNote = TEXT(" No physics asset: ") + Error.ToString();
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		FString SaveError;
		FHyperAIStudioSourceControl::SaveAsset(*Asset, SaveError);
		FHyperAIStudioSourceControl::SaveAsset(Mesh, SaveError);
		return Asset;
	}

	/** Everything the job downloaded, imported: the model first, then the clips onto its skeleton (or the rig's). */
	bool ImportFiles(FJob& Job, FString& OutError)
	{
		FString Note;
		USkeleton* Skeleton = nullptr;
		if (USkeletalMesh* Rig = LoadAsset<USkeletalMesh>(Job.RigMesh))
		{
			Skeleton = Rig->GetSkeleton();
		}
		for (const FDownload& Download : Job.Downloads)
		{
			if (Download.Role == TEXT("model"))
			{
				const TArray<FString> Paths = ImportFile(Download.File, Job.AssetFolder, Job.AssetName, nullptr);
				Job.Info.ImportedAssets.Append(Paths);
				for (const FString& Path : Paths)
				{
					UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
					if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Object); Mesh && Job.Info.SkeletalMesh.IsEmpty())
					{
						Job.Info.SkeletalMesh = Path;
						Job.Info.AssetPath = Job.Info.AssetPath.IsEmpty() ? Path : Job.Info.AssetPath;
						Skeleton = Mesh->GetSkeleton();
						Job.Info.Skeleton = Skeleton ? Skeleton->GetPathName() : FString();
						const UPhysicsAsset* Physics = EnsurePhysicsAsset(*Mesh, Note);
						Job.Info.PhysicsAsset = Physics ? Physics->GetPathName() : FString();
						if (Job.Kind != EKind::Retexture)
						{
							Job.RigMesh = Path;
						}
					}
					else if (Cast<UStaticMesh>(Object))
					{
						Job.Info.AssetPath = Job.Info.AssetPath.IsEmpty() ? Path : Job.Info.AssetPath;
					}
					else if (Cast<UMaterialInstance>(Object))
					{
						Job.Info.Materials.Add(Path);
					}
					else if (Cast<UTexture>(Object))
					{
						Job.Info.Textures.Add(Path);
					}
				}
				if (Job.Kind != EKind::Retexture && Job.Info.SkeletalMesh.IsEmpty())
				{
					OutError = FString::Printf(TEXT("Unreal imported %d assets from the rigged model but no skeletal mesh; the file is %s."),
						Paths.Num(), *ProjectRelative(Download.File));
					return false;
				}
				if (Job.Kind == EKind::Retexture && Job.Info.AssetPath.IsEmpty())
				{
					OutError = FString::Printf(TEXT("Unreal imported %d assets from the textured model but no mesh; the file is %s."),
						Paths.Num(), *ProjectRelative(Download.File));
					return false;
				}
				continue;
			}
			if (!Skeleton)
			{
				OutError = TEXT("The animations have no skeleton to go onto: the rig's skeletal mesh is gone.");
				return false;
			}
			const TArray<FString> Clips = ImportAnimations(Download.File, *Skeleton, Job.AssetFolder / Job.AssetName, Job.Info.ImportedAssets);
			Job.Info.Animations.Append(Clips);
			for (const FString& Path : Clips)
			{
				const UAnimSequence* Sequence = Cast<UAnimSequence>(StaticLoadObject(UObject::StaticClass(), nullptr, *Path));
				const IAnimationDataModel* Model = Sequence ? Sequence->GetDataModel() : nullptr;
				if (Model && Model->GetNumBoneTracks() == 0)
				{
					Note += FString::Printf(TEXT(" %s moves no bone of %s: the clip's bone names are not the skeleton's."), *Path, *Skeleton->GetName());
				}
			}
		}
		if (Job.Kind == EKind::Animate && Job.Info.Animations.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Unreal imported no animation onto %s from the provider's files (%s)."), Skeleton ? *Skeleton->GetName() : TEXT("the skeleton"),
				*ProjectRelative(Job.Downloads.Last().File));
			return false;
		}
		Job.Info.Diagnostic = Note;
		return true;
	}

	// ---------------------------------------------------------------- retargeting through the animation_rigging pack

	struct FRetargetOp
	{
		FString Kind;
		FString Asset;
		FString Mesh;
		FString SourceRig;
		FString TargetRig;
		FString OutputFolder;
		TArray<FString> Animations;
	};

	/**
	 * IK Rigs for the rig and the target, and a retargeter between them, beside the rig in a Retarget folder (kept and
	 * reused by later jobs), then the clips copied onto the target. What is already there is used, not made again.
	 */
	TArray<FRetargetOp> RetargetPlan(const FString& RigMesh, const FString& Target, const TArray<FString>& Animations, const FString& OutputFolder)
	{
		const FString RigPackage = PackageOf(RigMesh);
		const FString Folder = FPackageName::GetLongPackagePath(RigPackage) / TEXT("Retarget");
		const FString RigName = FPackageName::GetShortName(RigPackage);
		const FString TargetName = FPackageName::GetShortName(PackageOf(Target));
		const FString SourceRig = Folder / (TEXT("IK_") + RigName);
		const FString TargetRig = Folder / (TEXT("IK_") + TargetName);
		const FString Retargeter = Folder / FString::Printf(TEXT("RTG_%s_To_%s"), *RigName, *TargetName);
		TArray<FRetargetOp> Ops;
		if (FHyperAIStudioNames::IsFreePackage(SourceRig))
		{
			Ops.Add({TEXT("ik_rig.create"), SourceRig, RigMesh});
		}
		if (FHyperAIStudioNames::IsFreePackage(TargetRig))
		{
			Ops.Add({TEXT("ik_rig.create"), TargetRig, Target});
		}
		if (FHyperAIStudioNames::IsFreePackage(Retargeter))
		{
			Ops.Add({TEXT("retargeter.create"), Retargeter, FString(), SourceRig, TargetRig});
		}
		if (!Animations.IsEmpty())
		{
			FRetargetOp Copy;
			Copy.Kind = TEXT("retarget.animations");
			Copy.Asset = Retargeter;
			Copy.Animations = Animations;
			Copy.OutputFolder = OutputFolder;
			Ops.Add(MoveTemp(Copy));
		}
		return Ops;
	}

	FString PlanJson(const TArray<FRetargetOp>& Ops)
	{
		TArray<TSharedPtr<FJsonValue>> List;
		for (const FRetargetOp& Op : Ops)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("Kind"), Op.Kind);
			Json->SetStringField(TEXT("Asset"), Op.Asset);
			auto Optional = [&Json](const TCHAR* Name, const FString& Value) { if (!Value.IsEmpty()) Json->SetStringField(Name, Value); };
			Optional(TEXT("Mesh"), Op.Mesh);
			Optional(TEXT("SourceRig"), Op.SourceRig);
			Optional(TEXT("TargetRig"), Op.TargetRig);
			Optional(TEXT("OutputFolder"), Op.OutputFolder);
			if (!Op.Animations.IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> Animations;
				for (const FString& Path : Op.Animations)
				{
					Animations.Add(MakeShared<FJsonValueString>(Path));
				}
				Json->SetArrayField(TEXT("Animations"), Animations);
			}
			List.Add(MakeShared<FJsonValueObject>(Json));
		}
		const TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetArrayField(TEXT("Operations"), List);
		return JsonText(Plan);
	}

	void SetString(const UStruct& Struct, void* Data, const TCHAR* Name, const FString& Value)
	{
		if (const FStrProperty* Property = CastField<FStrProperty>(Struct.FindPropertyByName(Name)))
		{
			Property->SetPropertyValue_InContainer(Data, Value);
		}
	}

	FString GetString(const UStruct& Struct, const void* Data, const TCHAR* Name)
	{
		const FStrProperty* Property = CastField<FStrProperty>(Struct.FindPropertyByName(Name));
		return Property ? Property->GetPropertyValue_InContainer(Data) : FString();
	}

	/**
	 * hyper_retarget_apply_plan, called the way an agent calls it: the animation_rigging pack is optional, so its module
	 * is reached through reflection, and its plan is an edit of its own that waits for the user's approval like any other.
	 */
	bool HandOffRetarget(const TArray<FRetargetOp>& Ops, const FString& OperationId, FString& OutStatus, FString& OutDiagnostic)
	{
		static const TCHAR* const Toolset = TEXT("HyperAIStudioRetarget.HyperAIStudioRetargetToolset");
		UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/HyperAIStudioRetarget.HyperAIStudioRetargetToolset"));
		UFunction* Function = Class ? Class->FindFunctionByName(TEXT("hyper_retarget_apply_plan")) : nullptr;
		const FStructProperty* RequestProperty = Function ? CastField<FStructProperty>(Function->FindPropertyByName(TEXT("Request"))) : nullptr;
		const FStructProperty* ReturnProperty = Function ? CastField<FStructProperty>(Function->GetReturnProperty()) : nullptr;
		const FArrayProperty* OpsProperty = RequestProperty ? CastField<FArrayProperty>(RequestProperty->Struct->FindPropertyByName(TEXT("Operations"))) : nullptr;
		const FStructProperty* OpProperty = OpsProperty ? CastField<FStructProperty>(OpsProperty->Inner) : nullptr;
		if (!OpProperty || !ReturnProperty || !FHyperAIStudioCapabilityRuntimeIndex::IsOwned(Class, Toolset))
		{
			OutStatus = TEXT("unavailable");
			OutDiagnostic = TEXT("The animation_rigging pack (the IK Rig plugin) is not loaded: send RetargetPlan to hyper_retarget_apply_plan once it is.");
			return false;
		}
		TArray<uint8> Parameters;
		Parameters.SetNumZeroed(Function->ParmsSize);
		Function->InitializeStruct(Parameters.GetData());
		ON_SCOPE_EXIT { Function->DestroyStruct(Parameters.GetData()); };
		void* Request = RequestProperty->ContainerPtrToValuePtr<void>(Parameters.GetData());
		SetString(*RequestProperty->Struct, Request, TEXT("OperationId"), OperationId);
		FScriptArrayHelper Array(OpsProperty, OpsProperty->ContainerPtrToValuePtr<void>(Request));
		for (const FRetargetOp& Op : Ops)
		{
			void* Item = Array.GetRawPtr(Array.AddValue());
			const UScriptStruct& Struct = *OpProperty->Struct;
			SetString(Struct, Item, TEXT("Kind"), Op.Kind);
			SetString(Struct, Item, TEXT("Asset"), Op.Asset);
			SetString(Struct, Item, TEXT("Mesh"), Op.Mesh);
			SetString(Struct, Item, TEXT("SourceRig"), Op.SourceRig);
			SetString(Struct, Item, TEXT("TargetRig"), Op.TargetRig);
			SetString(Struct, Item, TEXT("OutputFolder"), Op.OutputFolder);
			const FArrayProperty* Animations = CastField<FArrayProperty>(Struct.FindPropertyByName(TEXT("Animations")));
			const FStrProperty* Inner = Animations ? CastField<FStrProperty>(Animations->Inner) : nullptr;
			if (Inner)
			{
				FScriptArrayHelper List(Animations, Animations->ContainerPtrToValuePtr<void>(Item));
				for (const FString& Path : Op.Animations)
				{
					Inner->SetPropertyValue(List.GetRawPtr(List.AddValue()), Path);
				}
			}
		}
		Class->GetDefaultObject()->ProcessEvent(Function, Parameters.GetData());
		const void* Result = ReturnProperty->ContainerPtrToValuePtr<void>(Parameters.GetData());
		OutStatus = GetString(*ReturnProperty->Struct, Result, TEXT("Status"));
		OutDiagnostic = GetString(*ReturnProperty->Struct, Result, TEXT("Diagnostic"));
		return true;
	}

	void Complete(FJob& Job)
	{
		Job.Phase = EPhase::Done;
		Job.Info.State = TEXT("completed");
		Job.Info.Progress = 100;
		FString What = Job.Kind == EKind::Retexture ? FString::Printf(TEXT("Imported %s with %d material(s) and %d texture(s)"), *Job.Info.AssetPath,
				Job.Info.Materials.Num(), Job.Info.Textures.Num())
			: Job.Kind == EKind::Rig ? FString::Printf(TEXT("Imported %s, its skeleton %s and physics asset %s"), *Job.Info.SkeletalMesh, *Job.Info.Skeleton,
				Job.Info.PhysicsAsset.IsEmpty() ? TEXT("(none)") : *Job.Info.PhysicsAsset)
			: FString::Printf(TEXT("Imported %d animation(s) onto %s"), Job.Info.Animations.Num(), *Job.RigMesh);
		What += FString::Printf(TEXT("; %d assets in all, from %s."), Job.Info.ImportedAssets.Num(), *Job.Info.ModelFile);
		if (Job.Kind == EKind::Rig && !Job.Info.Animations.IsEmpty())
		{
			What += FString::Printf(TEXT(" With %d clip(s) Meshy made with the rig."), Job.Info.Animations.Num());
		}
		if (!Job.Info.RetargetStatus.IsEmpty())
		{
			What += FString::Printf(TEXT(" Retargeting onto %s went to hyper_retarget_apply_plan as %s: %s."), *Job.RetargetTo,
				*Job.Info.RetargetOperationId, *Job.Info.RetargetStatus);
		}
		Job.Info.Diagnostic = What + Job.Info.Diagnostic;
		WriteRecord(Job);
		UE_LOG(LogHyperAIStudioMeshGenCharacters, Display, TEXT("3D job %s: %s"), *Job.Info.OperationId, *Job.Info.Diagnostic);
	}

	void ImportAll(const TSharedRef<FJob>& Job)
	{
		// The import is an editor operation of its own, run on a tick of its own rather than inside the job's quick poll.
		Job->bWaiting = true;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Job](float)
		{
			Job->bWaiting = false;
			if (Job->Phase != EPhase::Import)
			{
				return false;
			}
			FString Error;
			if (!ImportFiles(*Job, Error))
			{
				Fail(*Job, Error);
				return false;
			}
			if (Job->RetargetTo.IsEmpty())
			{
				Complete(*Job);
				return false;
			}
			Enter(*Job, EPhase::Handoff);
			return false;
		}));
	}

	void Handoff(const TSharedRef<FJob>& Job)
	{
		Job->bWaiting = true;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Job](float)
		{
			Job->bWaiting = false;
			if (Job->Phase != EPhase::Handoff)
			{
				return false;
			}
			const TArray<FRetargetOp> Ops = RetargetPlan(Job->RigMesh, Job->RetargetTo, Job->Info.Animations,
				Job->AssetFolder / Job->AssetName / FPackageName::GetShortName(PackageOf(Job->RetargetTo)));
			Job->Info.RetargetPlan = PlanJson(Ops);
			Job->Info.RetargetOperationId = Job->Info.OperationId + TEXT("-retarget");
			FString Diagnostic;
			if (Ops.IsEmpty())
			{
				Job->Info.RetargetStatus = TEXT("nothing_to_do");
			}
			else if (!HandOffRetarget(Ops, Job->Info.RetargetOperationId, Job->Info.RetargetStatus, Diagnostic))
			{
				Job->Info.Diagnostic += TEXT(" ") + Diagnostic;
			}
			else if (Job->Info.RetargetStatus != TEXT("completed") && Job->Info.RetargetStatus != TEXT("awaiting_user_approval")
				&& Job->Info.RetargetStatus != TEXT("queued"))
			{
				Job->Info.Diagnostic += TEXT(" The retarget plan was refused: ") + Diagnostic;
			}
			Complete(*Job);
			return false;
		}));
	}
}

namespace HyperAIStudio::MeshGen::Private
{
	void StepCharacter(const TSharedRef<FJob>& Job)
	{
		using namespace HyperAIStudio::MeshGen::Characters;
		switch (Job->Phase)
		{
		case EPhase::Prepare: Enter(*Job, After(*Job, EPhase::Prepare)); break;
		case EPhase::Export: Export(Job); break;
		case EPhase::Upload: Upload(Job); break;
		case EPhase::Lookup: Lookup(Job); break;
		case EPhase::Submit: SubmitTask(Job); break;
		case EPhase::Generate: QueryTask(Job); break;
		case EPhase::Download: DownloadNext(Job); break;
		case EPhase::Import: ImportAll(Job); break;
		case EPhase::Handoff: Handoff(Job); break;
		default: break;
		}
	}
}

// ---------------------------------------------------------------- the tools

namespace HyperAIStudio::MeshGen::Characters
{
	/** What a request shares before its own checks: the thread, the provider and its key, the folder and the operation id. */
	struct FCommon
	{
		FString Provider;
		FHyperAIStudioExternalServiceSettings Service;
		FString Folder;
		FString OperationId;
	};

	bool Refuse(FHyperAIMeshTaskReport& Report, const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return false;
	}

	bool CheckProvider(const FString& Provider, const TArray<FString>& Allowed, FHyperAIMeshTaskReport& Report, FCommon& Out)
	{
		if (!Allowed.Contains(Provider) || !FHyperAIStudioExternalServices::Find(Provider, Out.Service))
		{
			return Refuse(Report, TEXT("unknown_provider"), TEXT("provider is ") + FString::Join(Allowed, TEXT(" or ")) + TEXT(" here."));
		}
		if (Key(Out.Service).IsEmpty())
		{
			return Refuse(Report, TEXT("missing_key"), FString::Printf(TEXT("%s's key is not set: set %s for your user, then restart the editor."),
				*Out.Service.Label, *Out.Service.ApiKeyVariable));
		}
		Out.Provider = Provider;
		Report.Provider = Provider;
		return true;
	}

	bool CheckFolderAndId(const FString& RequestFolder, const FString& RequestId, const TCHAR* Prefix, FHyperAIMeshTaskReport& Report, FCommon& Out)
	{
		FString Folder = RequestFolder.TrimStartAndEnd().IsEmpty() ? GetDefault<UHyperAIStudioSettings>()->MeshImportFolder : RequestFolder.TrimStartAndEnd();
		Folder.RemoveFromEnd(TEXT("/"));
		FString Reason;
		if ((!Folder.StartsWith(TEXT("/Game/")) && Folder != TEXT("/Game")) || !FHyperAIStudioNames::IsFolderPath(Folder, TEXT("folder"), &Reason))
		{
			return Refuse(Report, TEXT("invalid_request"), TEXT("folder is a content path under /Game, such as /Game/Generated/Characters.")
				+ (Reason.IsEmpty() ? FString() : TEXT(" ") + Reason));
		}
		Out.Folder = Folder;
		Out.OperationId = RequestId.IsEmpty() ? Prefix + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower() : RequestId;
		Report.OperationId = Out.OperationId;
		if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Out.OperationId))
		{
			return Refuse(Report, TEXT("invalid_operation_id"), TEXT("operation_id must be a short journal-safe id, or empty to have one made."));
		}
		if (FindJob(Out.OperationId).IsValid())
		{
			return Refuse(Report, TEXT("operation_id_used"), TEXT("That operation_id already names a 3D job; pass a new one or leave it empty."));
		}
		return true;
	}

	/** Folder/Name/Name, the package a model's import makes, must be one, past its paid job. */
	bool CheckDestination(const FString& Folder, const FString& Name, FHyperAIMeshTaskReport& Report)
	{
		FString Reason;
		if (!FHyperAIStudioNames::IsPackagePath(Folder / Name / Name, TEXT("The folder and name together"), &Reason))
		{
			return Refuse(Report, TEXT("invalid_request"), Reason + TEXT(" Give a shorter folder or name."));
		}
		Report.Destination = Folder / Name;
		return true;
	}

	/** A skeletal mesh named by a request field, as its object path; refused when it is not one. */
	bool CheckSkeletalMesh(const FString& Path, const TCHAR* Field, FHyperAIMeshTaskReport& Report, FString& OutPath)
	{
		const USkeletalMesh* Mesh = LoadAsset<USkeletalMesh>(Path);
		if (!Mesh || !Mesh->GetSkeleton())
		{
			return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%s, %s, is not a skeletal mesh with a skeleton in the project."), Field, *Path));
		}
		OutPath = Mesh->GetPathName();
		return true;
	}

	/** Where a model to rig or texture comes from: a job of this session, the provider's task, or a mesh sent as a file. */
	struct FSource
	{
		FString TaskId;
		FString MeshPath;
		FString BaseName;
	};

	bool CheckSource(const FString& OperationId, const FString& TaskId, const FString& MeshPath, FString& InOutProvider, FHyperAIMeshTaskReport& Report, FSource& Out)
	{
		const int32 Given = !OperationId.TrimStartAndEnd().IsEmpty() + !TaskId.TrimStartAndEnd().IsEmpty() + !MeshPath.TrimStartAndEnd().IsEmpty();
		if (Given != 1)
		{
			return Refuse(Report, TEXT("invalid_request"), TEXT("Give one of SourceOperationId (a 3D job of this session), SourceTaskId (the provider's task) or MeshPath (a mesh in the project)."));
		}
		if (!OperationId.TrimStartAndEnd().IsEmpty())
		{
			const TSharedPtr<FJob> Job = FindJob(OperationId.TrimStartAndEnd());
			if (!Job.IsValid() || Job->Info.State != TEXT("completed"))
			{
				return Refuse(Report, TEXT("invalid_request"), TEXT("SourceOperationId names no finished 3D job of this editor session; hyper_mesh_status lists them."));
			}
			const bool bRigProvider = Job->Info.Provider == TEXT("meshy") || Job->Info.Provider == TEXT("tripo");
			InOutProvider = InOutProvider.IsEmpty() && bRigProvider ? Job->Info.Provider : InOutProvider;
			Out.BaseName = Job->AssetName;
			// The provider works from its own model only when it made it from words or a picture; anything else is sent as a file.
			if (Job->Kind == EKind::Generate && InOutProvider == Job->Info.Provider && !Job->Info.TaskId.IsEmpty())
			{
				Out.TaskId = Job->Info.TaskId;
				return true;
			}
			Out.MeshPath = !Job->Info.SkeletalMesh.IsEmpty() ? Job->Info.SkeletalMesh : Job->Info.AssetPath;
			if (Out.MeshPath.IsEmpty())
			{
				return Refuse(Report, TEXT("invalid_request"), TEXT("That job imported no mesh to send."));
			}
			return true;
		}
		if (!TaskId.TrimStartAndEnd().IsEmpty())
		{
			Out.TaskId = TaskId.TrimStartAndEnd();
			Out.BaseName = TEXT("Model");
			return true;
		}
		const UObject* Mesh = LoadAsset<UStaticMesh>(MeshPath);
		Mesh = Mesh ? Mesh : LoadAsset<USkeletalMesh>(MeshPath);
		if (!Mesh)
		{
			return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("MeshPath, %s, is not a static or skeletal mesh in the project."), *MeshPath));
		}
		Out.MeshPath = Mesh->GetPathName();
		Out.BaseName = Mesh->GetName();
		return true;
	}

	/** The base name with its asset prefix dropped, so SM_Knight becomes SK_Knight and not SK_SM_Knight. */
	FString Stem(const FString& Name)
	{
		for (const TCHAR* Prefix : {TEXT("SM_"), TEXT("SK_"), TEXT("SKM_"), TEXT("A_")})
		{
			if (Name.StartsWith(Prefix))
			{
				return Name.Mid(FCString::Strlen(Prefix));
			}
		}
		return Name;
	}

	/** Prices the job, then dry-runs it, or checks the cap and asks the user. */
	FHyperAIMeshTaskReport Ask(const TSharedRef<FJob>& Job, const FString& What, const bool bDryRun, const TCHAR* VariantId, FHyperAIMeshTaskReport& Report)
	{
		double Total = 0.0;
		for (const FTask& Task : Job->Tasks)
		{
			Total += Task.Cost;
			if (!Task.CreateUrl.IsEmpty())
			{
				Report.Tasks.Add(FString::Printf(TEXT("%s with %s, $%.2f"), *Task.Label, *Job->Service.Label, Task.Cost));
			}
		}
		Job->Price = Total;
		Job->Info.Cost = static_cast<float>(Total);
		Report.EstimatedCost = static_cast<float>(Total);
		if (!Job->RetargetTo.IsEmpty())
		{
			Report.Tasks.Add(FString::Printf(TEXT("then retargeting onto %s through hyper_retarget_apply_plan, an edit approved on its own"), *Job->RetargetTo));
		}
		const FString Price = FString::Printf(TEXT("for at most $%.2f of this session's cap"), Total);
		if (bDryRun)
		{
			Report.bOk = true;
			Report.Status = TEXT("checked");
			Report.Diagnostic = FString::Printf(TEXT("Would %s, %s, and import it into %s. Nothing was spent; resubmit with bDryRun false to ask the user."),
				*What, *Price, *Report.Destination);
			return Report;
		}
		double Cost = 0.0;
		FString Error;
		if (!FHyperAIStudioExternalServices::Reserve(Job->Service.Id, 1, Cost, Error, Total))
		{
			Refuse(Report, TEXT("over_spend_cap"), Error);
			return Report;
		}
		Job->Info.State = TEXT("awaiting_user_approval");
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = C::PackId;
		Summary.ToolName = Job->ToolName;
		Summary.VariantId = VariantId;
		Summary.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
		Summary.EffectTarget = Report.Destination;
		Summary.Effects = {What.Left(1).ToUpper() + What.Mid(1), FString::Printf(TEXT("Pay %s at most $%.2f, against this session's spend cap"), *Job->Service.Label, Total)};
		for (const FString& Line : Report.Tasks)
		{
			Summary.Effects.Add(TEXT("| ") + Line);
		}
		Summary.Effects.Add(TEXT("Import it into ") + Report.Destination);
		Summary.Touches = {Report.Destination};
		if (!FHyperAIStudioApprovalGate::RequestAction(Report.OperationId, Summary, [Job]() { Begin(Job); }, Error))
		{
			Refuse(Report, TEXT("approval_not_queued"), Error);
			return Report;
		}
		Jobs().Insert(Job, 0);
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = FString::Printf(TEXT("Waiting for the user's approval in HyperAI Chat, Activity; nothing was spent yet. Then it will %s and import it into %s, ")
			TEXT("in one to ten minutes: call hyper_operation_wait with operation_id and bWaitThroughApproval true, then hyper_mesh_status."), *What, *Report.Destination);
		FHyperAIStudioApprovalGate::DescribeHeld(Report.OperationId, Report.Status, Report.Diagnostic);
		return Report;
	}

	TSharedRef<FJob> NewJob(const FCommon& Common, const EKind Kind, const TCHAR* KindName, const TCHAR* ToolName, const FString& Name)
	{
		const TSharedRef<FJob> Job = MakeShared<FJob>();
		Job->Kind = Kind;
		Job->ToolName = ToolName;
		Job->Service = Common.Service;
		Job->AssetFolder = Common.Folder;
		Job->AssetName = Name;
		Job->Info.Kind = KindName;
		Job->Info.OperationId = Common.OperationId;
		Job->Info.Provider = Common.Provider;
		return Job;
	}

	/** A source mesh is sent as a GLB: inline to Meshy, uploaded to Tripo for a file token. */
	void SendSource(FJob& Job, const FSource& Source)
	{
		Job.SourceTaskId = Source.TaskId;
		Job.ExportMesh = Source.MeshPath;
		Job.bUpload = !Source.MeshPath.IsEmpty() && Job.Info.Provider == TEXT("tripo");
	}

	FString DescribeSource(const FSource& Source, const FCommon& Common)
	{
		return Source.MeshPath.IsEmpty() ? FString::Printf(TEXT("%s's task %s"), *Common.Service.Label, *Source.TaskId)
			: FString::Printf(TEXT("%s (sent as a GLB)"), *Source.MeshPath);
	}
}

FString FHyperAIStudioMeshGenContracts::TextureLevel(const FString& Provider, const FString& Resolution)
{
	const FString Level = Resolution.TrimStartAndEnd().ToLower();
	if (Provider == TEXT("meshy"))
	{
		return Level.IsEmpty() ? FString(TEXT("2k")) : Level == TEXT("2k") || Level == TEXT("4k") || Level == TEXT("8k") ? Level : FString();
	}
	if (Provider == TEXT("tripo"))
	{
		return Level.IsEmpty() || Level == TEXT("standard") ? FString(TEXT("standard")) : Level == TEXT("detailed") || Level == TEXT("hd") ? FString(TEXT("detailed"))
			: Level == TEXT("8k") || Level == TEXT("extreme") ? FString(TEXT("extreme")) : FString();
	}
	return FString();
}

FString FHyperAIStudioMeshGenContracts::ViggleSkeletonFor(const TArray<FString>& BoneNames)
{
	bool bPelvis = false;
	bool bSpine = false;
	bool bHips = false;
	for (const FString& Bone : BoneNames)
	{
		const FString Name = Bone.ToLower();
		bPelvis |= Name == TEXT("pelvis");
		bSpine |= Name == TEXT("spine_01");
		// Mixamo's own names carry a mixamorig: prefix, which some importers write mixamorig_.
		bHips |= Name == TEXT("hips") || Name.EndsWith(TEXT(":hips")) || Name == TEXT("mixamorig_hips");
	}
	return bPelvis && bSpine ? FString(TEXT("metahuman_body")) : bHips ? FString(TEXT("mixamo")) : FString();
}

FString FHyperAIStudioMeshGenContracts::TripoPreset(const FString& RigModel, const FString& Action)
{
	using namespace HyperAIStudio::MeshGen::Characters;
	FString Name = Action.TrimStartAndEnd().ToLower();
	Name.RemoveFromStart(TEXT("preset:"));
	if (RigModel.StartsWith(TEXT("v2.5")))
	{
		for (const TCHAR* Preset : TripoV25)
		{
			if (Name == Preset)
			{
				return TEXT("preset:") + Name;
			}
		}
		return FString();
	}
	Name.RemoveFromStart(TEXT("biped:"));
	for (const TCHAR* Preset : TripoBiped)
	{
		if (Name == Preset)
		{
			return TEXT("preset:biped:") + Name;
		}
	}
	return FString();
}

FHyperAIMeshTaskReport FHyperAIStudioMeshGenContracts::Rig(const FHyperAIMeshRigRequest& Request)
{
	using namespace HyperAIStudio::MeshGen::Characters;
	FHyperAIMeshTaskReport Report;
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (!IsInGameThread())
	{
		Refuse(Report, TEXT("game_thread_required"), TEXT("Rigging runs on the game thread."));
		return Report;
	}
	Report.Providers = DescribeProviders({TEXT("meshy"), TEXT("tripo")});
	FString Provider = Request.Provider.TrimStartAndEnd().ToLower();
	FSource Source;
	if (!CheckSource(Request.SourceOperationId, Request.SourceTaskId, Request.MeshPath, Provider, Report, Source))
	{
		return Report;
	}
	for (const TCHAR* Id : RigProviders)
	{
		Provider = Provider.IsEmpty() && HasKey(Id) ? FString(Id) : Provider;
	}
	if (Provider.IsEmpty())
	{
		Refuse(Report, TEXT("missing_key"), TEXT("No provider that rigs has a key: set MESHY_API_KEY or TRIPO_API_KEY for your user, then restart the editor."));
		return Report;
	}
	FCommon Common;
	if (!CheckProvider(Provider, {TEXT("meshy"), TEXT("tripo")}, Report, Common) || !CheckFolderAndId(Request.Folder, Request.OperationId, TEXT("rig-"), Report, Common))
	{
		return Report;
	}
	const float Height = Request.HeightMeters <= 0.f ? 1.7f : Request.HeightMeters;
	if (Height < 0.1f || Height > 100.f)
	{
		Refuse(Report, TEXT("invalid_request"), TEXT("HeightMeters is the character's height in metres, such as 1.8."));
		return Report;
	}
	const FString Spec = Request.BoneNaming.TrimStartAndEnd().IsEmpty() ? FString(TEXT("mixamo")) : Request.BoneNaming.TrimStartAndEnd().ToLower();
	const FString RigModel = Request.Model.TrimStartAndEnd().IsEmpty() ? FString(TripoRigV1) : Request.Model.TrimStartAndEnd();
	if (Provider == TEXT("tripo") && ((Spec != TEXT("mixamo") && Spec != TEXT("tripo")) || (RigModel != TripoRigV1 && RigModel != TripoRigV25)))
	{
		Refuse(Report, TEXT("invalid_request"), TEXT("Tripo: BoneNaming is mixamo or tripo, and Model is v1.0-20240301 or v2.5-20260210."));
		return Report;
	}
	FString RetargetTo;
	if (!Request.RetargetTo.TrimStartAndEnd().IsEmpty() && !CheckSkeletalMesh(Request.RetargetTo, TEXT("RetargetTo"), Report, RetargetTo))
	{
		return Report;
	}
	const FString Name = FreeName(Common.Folder, AssetNameFor(Request.Name.TrimStartAndEnd().IsEmpty() ? TEXT("SK_") + Stem(Source.BaseName) : Request.Name, FString(), TEXT("SK_")));
	if (!CheckDestination(Common.Folder, Name, Report))
	{
		return Report;
	}
	const TSharedRef<FJob> Job = NewJob(Common, EKind::Rig, TEXT("rig"), C::RigToolName, Name);
	SendSource(*Job, Source);
	Job->RigModel = Provider == TEXT("tripo") ? RigModel : FString();
	Job->Info.Model = Job->RigModel;
	Job->RetargetTo = RetargetTo;
	Job->Tasks.Add(Provider == TEXT("meshy") ? MeshyRig(*Job, Height, /*bWithClips=*/true) : TripoRig(*Job, Spec));
	if (!RetargetTo.IsEmpty())
	{
		// Planned now for the report; made again once imported, from what is there then.
		Report.RetargetPlan = PlanJson(RetargetPlan(Common.Folder / Name / Name + TEXT(".") + Name, RetargetTo, {}, FString()));
	}
	const FString What = FString::Printf(TEXT("rig %s with %s as a humanoid, as a skeletal mesh with its skeleton and physics asset%s"),
		*DescribeSource(Source, Common), *Common.Service.Label, Provider == TEXT("meshy") ? TEXT(", with a walk and a run") : TEXT(""));
	return Ask(Job, What, Request.bDryRun, TEXT("mesh.rig.v1"), Report);
}

FHyperAIMeshTaskReport FHyperAIStudioMeshGenContracts::Animate(const FHyperAIMeshAnimateRequest& Request)
{
	using namespace HyperAIStudio::MeshGen::Characters;
	FHyperAIMeshTaskReport Report;
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (!IsInGameThread())
	{
		Refuse(Report, TEXT("game_thread_required"), TEXT("Animating runs on the game thread."));
		return Report;
	}
	Report.Providers = DescribeProviders({TEXT("meshy"), TEXT("tripo"), TEXT("viggle")});
	const FString Prompt = Request.Prompt.TrimStartAndEnd();
	TArray<FString> Actions;
	for (const FString& Action : Request.Actions)
	{
		if (!Action.TrimStartAndEnd().IsEmpty())
		{
			Actions.AddUnique(Action.TrimStartAndEnd());
		}
	}
	if (Prompt.IsEmpty() == Actions.IsEmpty())
	{
		Refuse(Report, TEXT("invalid_request"), TEXT("Give either Actions (presets from the provider's library) or Prompt (a clip made from words), not both."));
		return Report;
	}

	// The rig: a job of this session, or the provider's rig task with the skeletal mesh it became.
	FString RigProvider;
	FString RigTaskId;
	FString RigMesh;
	FString RigModel;
	if (!Request.RigOperationId.TrimStartAndEnd().IsEmpty())
	{
		const TSharedPtr<FJob> Rig = FindJob(Request.RigOperationId.TrimStartAndEnd());
		if (!Rig.IsValid() || Rig->Info.State != TEXT("completed") || Rig->RigTaskId.IsEmpty() || Rig->RigMesh.IsEmpty())
		{
			Refuse(Report, TEXT("invalid_request"), TEXT("RigOperationId names no finished rig of this editor session; hyper_mesh_status lists the jobs."));
			return Report;
		}
		RigProvider = Rig->Info.Provider;
		RigTaskId = Rig->RigTaskId;
		RigMesh = Rig->RigMesh;
		RigModel = Rig->RigModel;
	}
	else if (!Request.RigTaskId.TrimStartAndEnd().IsEmpty())
	{
		RigProvider = Request.Provider.TrimStartAndEnd().ToLower();
		RigTaskId = Request.RigTaskId.TrimStartAndEnd();
		if ((RigProvider != TEXT("meshy") && RigProvider != TEXT("tripo")) || !CheckSkeletalMesh(Request.RigMesh, TEXT("RigMesh"), Report, RigMesh))
		{
			if (Report.Status.IsEmpty())
			{
				Refuse(Report, TEXT("invalid_request"), TEXT("With RigTaskId, name the provider that rigged it (meshy or tripo) and RigMesh, the skeletal mesh it was imported as."));
			}
			return Report;
		}
		RigModel = RigProvider == TEXT("tripo") ? FString(TripoRigV1) : FString();
	}
	FString TargetMesh;
	if (!Request.TargetMesh.TrimStartAndEnd().IsEmpty() && !CheckSkeletalMesh(Request.TargetMesh, TEXT("TargetMesh"), Report, TargetMesh))
	{
		return Report;
	}

	// The provider: the rig's for presets; for words, Meshy on a Meshy rig, else Viggle when the target's skeleton is one of its own.
	FString Provider = Request.Provider.TrimStartAndEnd().ToLower();
	const USkeletalMesh* ClipTarget = LoadAsset<USkeletalMesh>(!TargetMesh.IsEmpty() ? TargetMesh : RigMesh);
	const FString ViggleSkeleton = ClipTarget ? ViggleSkeletonFor(BoneNames(*ClipTarget)) : FString();
	if (Provider.IsEmpty())
	{
		Provider = !Actions.IsEmpty() || RigProvider == TEXT("meshy") ? RigProvider
			: !ViggleSkeleton.IsEmpty() && HasKey(TEXT("viggle")) ? FString(TEXT("viggle")) : FString(TEXT("meshy"));
	}
	FCommon Common;
	if (Provider.IsEmpty())
	{
		Refuse(Report, TEXT("invalid_request"), TEXT("Preset actions go on a rig: give RigOperationId, or RigTaskId with provider and RigMesh."));
		return Report;
	}
	if (!CheckProvider(Provider, {TEXT("meshy"), TEXT("tripo"), TEXT("viggle")}, Report, Common) || !CheckFolderAndId(Request.Folder, Request.OperationId, TEXT("anim-"), Report, Common))
	{
		return Report;
	}
	if (!RigProvider.IsEmpty() && Provider != RigProvider && Provider != TEXT("viggle"))
	{
		Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("A %s rig is animated by %s; a provider animates only its own rigs. For a clip from words on a Mixamo-named rig, use viggle."),
			*RigProvider, *RigProvider));
		return Report;
	}
	FString RetargetTo;
	if (!Request.RetargetTo.TrimStartAndEnd().IsEmpty() && !CheckSkeletalMesh(Request.RetargetTo, TEXT("RetargetTo"), Report, RetargetTo))
	{
		return Report;
	}
	const float Seconds = Request.DurationSeconds <= 0.f ? 4.f : Request.DurationSeconds;
	const FString Mode = Request.Mode.TrimStartAndEnd().IsEmpty() ? FString(TEXT("prime")) : Request.Mode.TrimStartAndEnd().ToLower();
	const FString Base = !RigMesh.IsEmpty() ? FPackageName::GetShortName(PackageOf(RigMesh)) : FPackageName::GetShortName(PackageOf(TargetMesh));
	const FString Name = FreeName(Common.Folder, AssetNameFor(Request.Name.TrimStartAndEnd().IsEmpty()
		? TEXT("A_") + Stem(Base) + TEXT("_") + (Prompt.IsEmpty() ? Actions[0].Replace(TEXT("preset:"), TEXT("")).Replace(TEXT(":"), TEXT("_")) : TEXT("Motion"))
		: Request.Name, FString(), TEXT("A_")));
	if (!CheckDestination(Common.Folder, Name, Report))
	{
		return Report;
	}
	const TSharedRef<FJob> Job = NewJob(Common, EKind::Animate, TEXT("animate"), C::AnimateToolName, Name);
	Job->RigTaskId = RigTaskId;
	Job->RigMesh = RigMesh;
	Job->RigModel = RigModel;
	Job->Info.Model = RigModel;
	Job->Info.Prompt = Prompt;
	FString What;
	if (!Prompt.IsEmpty())
	{
		if (Prompt.Len() > MaxMotionPromptCharacters)
		{
			Refuse(Report, TEXT("invalid_request"), TEXT("Prompt is 400 characters at most."));
			return Report;
		}
		if (Provider == TEXT("tripo"))
		{
			Refuse(Report, TEXT("invalid_request"), TEXT("Tripo makes no animation from words: use meshy or viggle, or Tripo's presets in Actions."));
			return Report;
		}
		if (Provider == TEXT("viggle"))
		{
			const FString Target = !TargetMesh.IsEmpty() ? TargetMesh : RigMesh;
			if (Target.IsEmpty() || ViggleSkeleton.IsEmpty())
			{
				Refuse(Report, TEXT("invalid_request"), TEXT("Viggle's clips come on a Mixamo or a UE5 Mannequin / MetaHuman body skeleton: give TargetMesh, a skeletal mesh on one of those, such as the project's Mannequin. Otherwise use meshy with a rig."));
				return Report;
			}
			if (Seconds < 3.f || Seconds > 60.f)
			{
				Refuse(Report, TEXT("invalid_request"), TEXT("Viggle makes clips of 3 to 60 seconds."));
				return Report;
			}
			Job->RigMesh = Target;
			Job->Tasks = ViggleMotion(*Job, Prompt, Seconds, ViggleSkeleton);
			What = FString::Printf(TEXT("make a %.1f s clip with Viggle from words: \"%s\", on its %s skeleton, imported onto %s's skeleton"),
				Seconds, *Prompt.Left(120), ViggleSkeleton == TEXT("mixamo") ? TEXT("Mixamo") : TEXT("MetaHuman body"), *Target);
		}
		else
		{
			if (Seconds < 2.f || Seconds > 10.f || !FMath::IsNearlyEqual(FMath::Fmod(Seconds * 2.f, 1.f), 0.f) || (Mode != TEXT("prime") && Mode != TEXT("swift")))
			{
				Refuse(Report, TEXT("invalid_request"), TEXT("Meshy makes clips of 2 to 10 seconds in half seconds; Mode is prime or swift."));
				return Report;
			}
			int32 MotionTask = 0;
			if (RigTaskId.IsEmpty())
			{
				// No rig: the target is sent as a GLB and rigged by Meshy first, and the clip comes back onto it by retargeting.
				if (TargetMesh.IsEmpty())
				{
					Refuse(Report, TEXT("invalid_request"), TEXT("Meshy puts a clip on its own rig: give RigOperationId, RigTaskId with RigMesh, or TargetMesh to rig first."));
					return Report;
				}
				Job->ExportMesh = TargetMesh;
				Job->Tasks.Add(MeshyRig(*Job, 1.7f, /*bWithClips=*/false));
				RetargetTo = RetargetTo.IsEmpty() ? TargetMesh : RetargetTo;
				MotionTask = 1;
			}
			Job->Tasks.Add(MeshyMotion(*Job, Prompt, Seconds, Mode));
			Job->Tasks.Add(MeshyAnimation(*Job, UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("meshy"), TEXT("motion_apply")), MotionTask));
			What = FString::Printf(TEXT("make a %.1f s clip with Meshy (%s) from words: \"%s\"%s, put on the rig and imported onto its skeleton"), Seconds, *Mode,
				*Prompt.Left(120), RigTaskId.IsEmpty() ? *FString::Printf(TEXT(", rigging %s first"), *TargetMesh) : TEXT(""));
		}
	}
	else if (Provider == TEXT("viggle"))
	{
		Refuse(Report, TEXT("invalid_request"), TEXT("Viggle makes clips from words only: give Prompt."));
		return Report;
	}
	else if (RigTaskId.IsEmpty())
	{
		Refuse(Report, TEXT("invalid_request"), TEXT("Preset actions go on a rig: give RigOperationId, or RigTaskId with provider and RigMesh."));
		return Report;
	}
	else if (Provider == TEXT("meshy"))
	{
		if (Actions.Num() > MaxMeshyActions)
		{
			Refuse(Report, TEXT("invalid_request"), TEXT("Meshy takes up to 10 actions a job."));
			return Report;
		}
		const TArray<FHyperAIMeshAction> Library = MeshyLibrary();
		for (const FString& Action : Actions)
		{
			const int32 Id = MeshyActionId(Action, Library);
			Id == INDEX_NONE ? (void)Job->Lookups.Add(Action) : (void)Job->ActionIds.AddUnique(Id);
		}
		Job->Tasks.Add(MeshyAnimation(*Job, UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("meshy"), TEXT("animate")) * Actions.Num(), INDEX_NONE));
		What = FString::Printf(TEXT("put %d action(s) from Meshy's library on the rig (%s)%s, imported onto its skeleton"), Actions.Num(), *FString::Join(Actions, TEXT(", ")),
			Job->Lookups.IsEmpty() ? TEXT("") : TEXT(", names looked up in the library first"));
	}
	else
	{
		if (Actions.Num() > MaxTripoActions)
		{
			Refuse(Report, TEXT("invalid_request"), TEXT("Tripo takes up to 20 presets a job (5 a task)."));
			return Report;
		}
		TArray<FString> Presets;
		TArray<FString> Unknown;
		for (const FString& Action : Actions)
		{
			const FString Preset = TripoPreset(RigModel, Action);
			Preset.IsEmpty() ? (void)Unknown.Add(Action) : (void)Presets.AddUnique(Preset);
		}
		if (!Unknown.IsEmpty())
		{
			Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("Not among Tripo's presets for its %s rig: %s. hyper_mesh_actions lists them."),
				*RigModel, *FString::Join(Unknown, TEXT(", "))));
			return Report;
		}
		for (int32 Start = 0; Start < Presets.Num(); Start += 5)
		{
			Job->Tasks.Add(TripoRetarget(*Job, TArray<FString>(Presets.GetData() + Start, FMath::Min(5, Presets.Num() - Start))));
		}
		What = FString::Printf(TEXT("put %d of Tripo's presets on the rig (%s), imported onto its skeleton"), Presets.Num(), *FString::Join(Presets, TEXT(", ")));
	}
	const FString SkeletonOf = !Job->RigMesh.IsEmpty() ? Job->RigMesh : Common.Folder / Name / Name + TEXT(".") + Name;
	if (!RetargetTo.IsEmpty() && RetargetTo == SkeletonOf)
	{
		RetargetTo.Reset();
	}
	Job->RetargetTo = RetargetTo;
	if (!RetargetTo.IsEmpty())
	{
		Report.RetargetPlan = PlanJson(RetargetPlan(SkeletonOf, RetargetTo, {TEXT("(the new animations)")}, Common.Folder / Name / FPackageName::GetShortName(PackageOf(RetargetTo))));
	}
	return Ask(Job, What, Request.bDryRun, TEXT("mesh.animate.v1"), Report);
}

FHyperAIMeshTaskReport FHyperAIStudioMeshGenContracts::Retexture(const FHyperAIMeshRetextureRequest& Request)
{
	using namespace HyperAIStudio::MeshGen::Characters;
	FHyperAIMeshTaskReport Report;
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (!IsInGameThread())
	{
		Refuse(Report, TEXT("game_thread_required"), TEXT("Texturing runs on the game thread."));
		return Report;
	}
	Report.Providers = DescribeProviders({TEXT("meshy"), TEXT("tripo")});
	FString Provider = Request.Provider.TrimStartAndEnd().ToLower();
	FSource Source;
	if (!CheckSource(Request.SourceOperationId, Request.SourceTaskId, Request.MeshPath, Provider, Report, Source))
	{
		return Report;
	}
	for (const TCHAR* Id : RigProviders)
	{
		Provider = Provider.IsEmpty() && HasKey(Id) ? FString(Id) : Provider;
	}
	if (Provider.IsEmpty())
	{
		Refuse(Report, TEXT("missing_key"), TEXT("No provider that textures has a key: set MESHY_API_KEY or TRIPO_API_KEY for your user, then restart the editor."));
		return Report;
	}
	FCommon Common;
	if (!CheckProvider(Provider, {TEXT("meshy"), TEXT("tripo")}, Report, Common) || !CheckFolderAndId(Request.Folder, Request.OperationId, TEXT("tex-"), Report, Common))
	{
		return Report;
	}
	const FString Prompt = Request.Prompt.TrimStartAndEnd();
	FString Image;
	if (!Request.ImagePath.TrimStartAndEnd().IsEmpty())
	{
		if (Provider == TEXT("tripo"))
		{
			Refuse(Report, TEXT("invalid_request"), TEXT("Tripo textures from words here; a picture's look goes to meshy."));
			return Report;
		}
		const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		Image = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Request.ImagePath) ? ProjectDir / Request.ImagePath : Request.ImagePath);
		const FString Extension = FPaths::GetExtension(Image).ToLower();
		const int64 Size = IFileManager::Get().FileSize(*Image);
		if (!FPaths::IsUnderDirectory(Image, ProjectDir) || Size <= 0 || Size > MaxImageBytes
			|| (Extension != TEXT("png") && Extension != TEXT("jpg") && Extension != TEXT("jpeg") && Extension != TEXT("webp")))
		{
			Refuse(Report, TEXT("invalid_image"), TEXT("ImagePath is a PNG, JPEG or WebP inside the project, up to 20 MB."));
			return Report;
		}
	}
	if (Prompt.IsEmpty() && Image.IsEmpty())
	{
		Refuse(Report, TEXT("invalid_request"), TEXT("Say what the surface should look like in Prompt, or give a picture in ImagePath (meshy)."));
		return Report;
	}
	if (Prompt.Len() > MaxPromptCharacters)
	{
		Refuse(Report, TEXT("invalid_request"), TEXT("Prompt is 800 characters at most."));
		return Report;
	}
	const FString Level = TextureLevel(Provider, Request.Resolution);
	if (Level.IsEmpty())
	{
		Refuse(Report, TEXT("invalid_request"), Provider == TEXT("meshy") ? TEXT("Meshy's Resolution is 2k, 4k or 8k.") : TEXT("Tripo's Resolution is standard, detailed or 8k."));
		return Report;
	}
	const FString Name = FreeName(Common.Folder, AssetNameFor(Request.Name.TrimStartAndEnd().IsEmpty() ? TEXT("SM_") + Stem(Source.BaseName) + TEXT("_Retextured") : Request.Name,
		FString(), TEXT("SM_")));
	if (!CheckDestination(Common.Folder, Name, Report))
	{
		return Report;
	}
	const TSharedRef<FJob> Job = NewJob(Common, EKind::Retexture, TEXT("retexture"), C::RetextureToolName, Name);
	SendSource(*Job, Source);
	Job->ImageFile = Image;
	Job->Info.Prompt = Prompt;
	Job->Info.ImagePath = Image.IsEmpty() ? FString() : ProjectRelative(Image);
	Job->Info.Model = Level;
	Job->Tasks.Add(Provider == TEXT("meshy") ? MeshyRetexture(*Job, Prompt, Level, Request.bPBR, Request.bKeepUVs) : TripoTexture(*Job, Prompt, Level, Request.bPBR));
	const FString What = FString::Printf(TEXT("texture %s again with %s at %s%s, from %s"), *DescribeSource(Source, Common), *Common.Service.Label,
		*Level, Request.bPBR ? TEXT(" with PBR maps") : TEXT(""),
		Image.IsEmpty() ? *FString::Printf(TEXT("words: \"%s\""), *Prompt.Left(120)) : *FString::Printf(TEXT("the picture %s"), *ProjectRelative(Image)));
	return Ask(Job, What, Request.bDryRun, TEXT("mesh.retexture.v1"), Report);
}

FHyperAIMeshActionsReport FHyperAIStudioMeshGenContracts::Actions(const FHyperAIMeshActionsRequest& Request)
{
	using namespace HyperAIStudio::MeshGen::Characters;
	FHyperAIMeshActionsReport Report;
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	const FString Provider = Request.Provider.TrimStartAndEnd().ToLower();
	if (!Provider.IsEmpty() && Provider != TEXT("meshy") && Provider != TEXT("tripo"))
	{
		Report.Diagnostic = TEXT("provider is meshy or tripo, or empty for both. Viggle makes clips from words only.");
		return Report;
	}
	TArray<FHyperAIMeshAction> All;
	if (Provider.IsEmpty() || Provider == TEXT("meshy"))
	{
		All = MeshyLibrary(&Report.bMeshyLibraryCached);
	}
	if (Provider.IsEmpty() || Provider == TEXT("tripo"))
	{
		for (const TCHAR* Preset : TripoBiped)
		{
			All.Add(MakeAction(TEXT("tripo"), FString(TEXT("preset:biped:")) + Preset, FString(Preset).Replace(TEXT("_"), TEXT(" ")), TEXT("v1.0")));
		}
		for (const TCHAR* Preset : TripoV25)
		{
			All.Add(MakeAction(TEXT("tripo"), FString(TEXT("preset:")) + Preset, FString(Preset).Replace(TEXT(":"), TEXT(" ")), TEXT("v2.5")));
		}
	}
	TArray<FString> Words;
	Request.Search.ParseIntoArrayWS(Words);
	const FString Category = Request.Category.TrimStartAndEnd();
	const int32 Limit = Request.Limit <= 0 ? 50 : FMath::Min(Request.Limit, 1000);
	for (const FHyperAIMeshAction& Action : All)
	{
		if (!Category.IsEmpty() && !Action.Category.Equals(Category, ESearchCase::IgnoreCase))
		{
			continue;
		}
		const FString Haystack = Normal(Action.Id + Action.Name + Action.Category);
		const bool bMatch = Algo::AllOf(Words, [&Haystack](const FString& Word) { return Haystack.Contains(Normal(Word)); });
		if (!bMatch)
		{
			continue;
		}
		++Report.Matched;
		if (Report.Actions.Num() < Limit)
		{
			Report.Actions.Add(Action);
		}
	}
	Report.bOk = true;
	Report.Diagnostic = FString::Printf(TEXT("%d action(s) match; %d listed. Tripo's v1.0 presets go on its default rig, its v2.5 ones on a v2.5 rig. %s"),
		Report.Matched, Report.Actions.Num(), Report.bMeshyLibraryCached ? TEXT("Meshy's whole library is here.")
			: TEXT("Meshy's library here is its first 40 actions; any other name is looked up when an animation job runs, and the whole library is kept then."));
	return Report;
}

FHyperAIMeshCancelReport FHyperAIStudioMeshGenContracts::Cancel(const FString& OperationId)
{
	using namespace HyperAIStudio::MeshGen::Private;
	FHyperAIMeshCancelReport Report;
	const TSharedPtr<FJob> Job = FindJob(OperationId.TrimStartAndEnd());
	if (!Job.IsValid())
	{
		Report.Status = TEXT("not_found");
		Report.Diagnostic = TEXT("No 3D job has that operation_id in this editor session.");
		return Report;
	}
	if (Job->Info.State == TEXT("awaiting_user_approval") && FHyperAIStudioApprovalGate::HasPending(Job->Info.OperationId))
	{
		FHyperAIStudioApprovalGate::Reject(Job->Info.OperationId, TEXT("Cancelled by the agent before it was approved; nothing was spent."));
		Job->Phase = EPhase::Failed;
		Job->Info.State = TEXT("cancelled");
		Job->Info.Diagnostic = TEXT("Cancelled before it was approved; nothing was spent.");
		Report.bOk = true;
		Report.Status = TEXT("cancelled");
		Report.Diagnostic = Job->Info.Diagnostic;
		Report.Job = Snapshot(*Job);
		return Report;
	}
	FString Error;
	if (Job->Phase == EPhase::Done || Job->Phase == EPhase::Failed || Job->JobId.IsEmpty())
	{
		Report.Status = TEXT("already_finished");
		Report.Diagnostic = FString::Printf(TEXT("The job is %s; there is nothing to stop."), *Snapshot(*Job).State);
		Report.Job = Snapshot(*Job);
		return Report;
	}
	Job->bCancelRequested = true;
	if (!FHyperAIStudioAsyncJobHost::Cancel(Job->JobId, TEXT("Cancelled by the agent."), Error))
	{
		Job->bCancelRequested = false;
		Report.Status = TEXT("already_finished");
		Report.Diagnostic = Error;
		Report.Job = Snapshot(*Job);
		return Report;
	}
	Report.bOk = true;
	Report.Status = TEXT("cancelled");
	Report.Diagnostic = FString::Printf(TEXT("Stopped. %s"), Job->Info.Tasks.IsEmpty() ? TEXT("No task had reached the provider; nothing was spent.")
		: TEXT("The provider runs a task it has taken to its end (neither Meshy nor Tripo stops a running one), so what it took stays booked."));
	Report.Job = Snapshot(*Job);
	return Report;
}
