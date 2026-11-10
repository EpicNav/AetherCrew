// Games by Hyper 2026.

#include "HyperAIStudioAudioAuthoringToolset.h"

#include "ActiveSound.h"
#include "Audio/AudioDebug.h"
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSoundCueGraph.h"
#include "Misc/DefaultValueHelper.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundWave.h"

namespace HyperAIStudio::AudioAuthoring::Preview
{
	namespace
	{
		/** What this tool soloed and muted, so status can name them and clear can undo them. */
		TSet<FString>& Soloed()
		{
			static TSet<FString> Names;
			return Names;
		}

		TSet<FString>& Muted()
		{
			static TSet<FString> Names;
			return Names;
		}

		bool HasAudio()
		{
			return GEditor && GEditor->UseSound() && GEngine->GetAudioDeviceManager() != nullptr;
		}

		/** Sets each parameter on the component: true/false a bool, trigger a trigger, a whole number an int, otherwise a float. */
		bool SetParameters(UAudioComponent& Component, const TArray<FHyperAIAudioSetting>& Parameters, TArray<FName>& OutTriggers, FString& OutError)
		{
			for (const FHyperAIAudioSetting& Setting : Parameters)
			{
				const FName Name(*Setting.Name.TrimStartAndEnd());
				const FString Text = Setting.Value.TrimStartAndEnd();
				int32 Int = 0;
				float Float = 0.f;
				if (Name.IsNone())
				{
					OutError = TEXT("Every parameter needs a Name.");
					return false;
				}
				if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("false"), ESearchCase::IgnoreCase))
				{
					Component.SetBoolParameter(Name, Text.Equals(TEXT("true"), ESearchCase::IgnoreCase));
				}
				else if (Text.IsEmpty() || Text.Equals(TEXT("trigger"), ESearchCase::IgnoreCase))
				{
					OutTriggers.Add(Name);
				}
				else if (!Text.Contains(TEXT(".")) && FDefaultValueHelper::ParseInt(Text, Int))
				{
					// MetaSound float inputs take an int parameter too; set both so either kind of input reads it.
					Component.SetIntParameter(Name, Int);
					Component.SetFloatParameter(Name, static_cast<float>(Int));
				}
				else if (FDefaultValueHelper::ParseFloat(Text, Float))
				{
					Component.SetFloatParameter(Name, Float);
				}
				else
				{
					OutError = FString::Printf(TEXT("Parameter %s is a number, true, false or trigger, not %s."), *Setting.Name, *Text);
					return false;
				}
			}
			return true;
		}

		void Fill(FHyperAIAudioPreviewReport& Report)
		{
			Report.bAudioAvailable = HasAudio();
			UAudioComponent* Component = GEditor ? GEditor->GetPreviewAudioComponent() : nullptr;
			Report.bPreviewing = Component && Component->IsPlaying();
			Report.PreviewSound = Report.bPreviewing && Component->Sound ? Component->Sound->GetPathName() : FString();
			Report.Soloed = Soloed().Array();
			Report.Muted = Muted().Array();
			FAudioDevice* Device = GEngine ? GEngine->GetMainAudioDeviceRaw() : nullptr;
			// The device's lists belong to the audio thread; in the editor that is the game thread unless audio runs threaded.
			if (!Device || !IsInAudioThread())
			{
				return;
			}
			for (const FActiveSound* Sound : Device->GetActiveSounds())
			{
				if (Report.ActiveSounds.Num() >= 64) break;
				if (Sound && Sound->GetSound())
				{
					Report.ActiveSounds.Add(FString::Printf(TEXT("%s, %.2f s, volume %.2f, %s"), *Sound->GetSound()->GetPathName(), Sound->PlaybackTime, Sound->GetVolume(),
						*Sound->GetOwnerName()));
				}
			}
			for (const TPair<USoundMix*, FSoundMixState>& Mix : Device->GetSoundMixModifiers())
			{
				if (Mix.Key)
				{
					Report.ActiveMixes.Add(FString::Printf(TEXT("%s, pushed %d, fade %.2f%s"), *Mix.Key->GetPathName(), static_cast<int32>(Mix.Value.ActiveRefCount),
						Mix.Value.InterpValue, Mix.Value.IsBaseSoundMix ? TEXT(", base mix") : TEXT("")));
				}
			}
		}

		bool SoloOrMute(const bool bSolo, const FString& Path, const bool bEnable, FString& OutError)
		{
#if ENABLE_AUDIO_DEBUG
			FString Reason;
			UObject* Target = CueGraph::LoadAsset(Path, *UObject::StaticClass(), &Reason);
			if (!Target)
			{
				OutError = Reason.IsEmpty() ? FString::Printf(TEXT("%s is not in the project."), *Path) : Reason;
				return false;
			}
			::Audio::FAudioDebugger& Debugger = GEngine->GetAudioDeviceManager()->GetDebugger();
			const FName Name = Target->GetFName();
			if (Target->IsA<USoundClass>())
			{
				const bool bNow = bSolo ? Debugger.IsSoloSoundClass(Name) : Debugger.IsMuteSoundClass(Name);
				if (bNow != bEnable)
				{
					bSolo ? Debugger.ToggleSoloSoundClass(Name) : Debugger.ToggleMuteSoundClass(Name);
				}
			}
			else if (Target->IsA<USoundSubmixBase>())
			{
				if (bSolo && !Target->IsA<USoundSubmixWithParentBase>())
				{
					OutError = FString::Printf(TEXT("%s is an endpoint submix, which cannot be soloed."), *Path);
					return false;
				}
				bSolo ? Debugger.SetSoloSubmixChain(Name, bEnable) : Debugger.SetMuteSubmix(Name, bEnable);
			}
			else if (Target->IsA<USoundCue>())
			{
				bSolo ? Debugger.SetSoloSoundCue(Name, bEnable) : Debugger.SetMuteSoundCue(Name, bEnable);
			}
			else if (Target->IsA<USoundWave>())
			{
				bSolo ? Debugger.SetSoloSoundWave(Name, bEnable) : Debugger.SetMuteSoundWave(Name, bEnable);
			}
			else
			{
				OutError = FString::Printf(TEXT("%s is a %s; solo and mute take a Sound Class, Submix, SoundWave or Sound Cue."), *Path, *Target->GetClass()->GetName());
				return false;
			}
			TSet<FString>& Set = bSolo ? Soloed() : Muted();
			bEnable ? (void)Set.Add(Target->GetPathName()) : (void)Set.Remove(Target->GetPathName());
			return true;
#else
			OutError = TEXT("Audio debugging (solo and mute) is compiled out of this build.");
			return false;
#endif
		}
	}

	FHyperAIAudioPreviewReport Run(const FHyperAIAudioPreviewRequest& Request)
	{
		FHyperAIAudioPreviewReport Report;
		const FString Action = Request.Action.TrimStartAndEnd().ToLower();
		auto Done = [&Report](const bool bOk, const TCHAR* Status, const FString& Why)
		{
			Fill(Report);
			Report.bOk = bOk;
			Report.Status = Status;
			Report.Diagnostic = Why;
			return Report;
		};
		if (!GEditor)
		{
			return Done(false, TEXT("no_editor"), TEXT("The preview needs the editor."));
		}
		if (Action.IsEmpty() || Action == TEXT("status"))
		{
			return Done(true, TEXT("ok"), HasAudio() ? TEXT("The preview and the audio device as they are now.") : TEXT("No audio device: nothing can be heard here (tests, -nosound). hyper_audio_analyze still measures sounds."));
		}
		if (Action != TEXT("play") && Action != TEXT("stop") && Action != TEXT("set") && Action != TEXT("solo") && Action != TEXT("mute") && Action != TEXT("clear"))
		{
			return Done(false, TEXT("invalid_request"), TEXT("Action is play, stop, set, status, solo, mute or clear."));
		}
		if (Request.VolumeMultiplier < 0.f || Request.VolumeMultiplier > 4.f || Request.PitchMultiplier < 0.f || Request.PitchMultiplier > 4.f || Request.StartTime < 0.f)
		{
			return Done(false, TEXT("invalid_request"), TEXT("VolumeMultiplier and PitchMultiplier are 0 (leave) to 4, StartTime 0 or more."));
		}
		if (Action == TEXT("stop"))
		{
			GEditor->ResetPreviewAudioComponent();
			return Done(true, TEXT("stopped"), TEXT("The preview stopped."));
		}
		if (!HasAudio())
		{
			return Done(false, TEXT("no_audio_device"), TEXT("No audio device: nothing can be heard here (tests, -nosound). Measure the sound with hyper_audio_analyze instead."));
		}
		FString Error;
		if (Action == TEXT("solo") || Action == TEXT("mute"))
		{
			if (!SoloOrMute(Action == TEXT("solo"), Request.Target.TrimStartAndEnd(), Request.bEnable, Error))
			{
				return Done(false, TEXT("invalid_request"), Error);
			}
			return Done(true, TEXT("completed"), FString::Printf(TEXT("%s %s %s; clear undoes every solo and mute."), *Request.Target, Request.bEnable ? TEXT("now") : TEXT("no longer"),
				Action == TEXT("solo") ? TEXT("soloed") : TEXT("muted")));
		}
		if (Action == TEXT("clear"))
		{
#if ENABLE_AUDIO_DEBUG
			GEngine->GetAudioDeviceManager()->GetDebugger().ClearMutesAndSolos();
			GEngine->GetAudioDeviceManager()->GetDebugger().ClearSubmixMuteSoloState();
#endif
			Soloed().Reset();
			Muted().Reset();
			return Done(true, TEXT("completed"), TEXT("Every solo and mute cleared."));
		}
		UAudioComponent* Component = nullptr;
		if (Action == TEXT("play"))
		{
			FString Reason;
			USoundBase* Sound = Cast<USoundBase>(CueGraph::LoadAsset(Request.Sound.TrimStartAndEnd(), *USoundBase::StaticClass(), &Reason));
			if (!Sound)
			{
				return Done(false, TEXT("not_found"), Reason.IsEmpty() ? FString::Printf(TEXT("%s is not a sound in the project."), *Request.Sound) : Reason);
			}
			Component = GEditor->ResetPreviewAudioComponent(Sound);
			if (!Component)
			{
				return Done(false, TEXT("operation_failed"), TEXT("The editor has no preview audio component."));
			}
		}
		else
		{
			Component = GEditor->GetPreviewAudioComponent();
			if (!Component || !Component->IsPlaying())
			{
				return Done(false, TEXT("not_previewing"), TEXT("Nothing is previewing: play a sound first."));
			}
		}
		TArray<FName> Triggers;
		if (!SetParameters(*Component, Request.Parameters, Triggers, Error))
		{
			return Done(false, TEXT("invalid_request"), Error);
		}
		if (Request.VolumeMultiplier > 0.f) Component->SetVolumeMultiplier(Request.VolumeMultiplier);
		if (Request.PitchMultiplier > 0.f) Component->SetPitchMultiplier(Request.PitchMultiplier);
		if (Action == TEXT("play"))
		{
			Component->bAutoDestroy = false;
			Component->bIsUISound = true;
			Component->bAllowSpatialization = false;
			Component->bReverb = false;
			Component->bCenterChannelOnly = false;
			Component->bIsPreviewSound = true;
			Component->Play(Request.StartTime);
		}
		for (const FName& Trigger : Triggers)
		{
			Component->SetTriggerParameter(Trigger);
		}
		return Done(true, Action == TEXT("play") ? TEXT("playing") : TEXT("completed"), Action == TEXT("play")
			? FString::Printf(TEXT("Playing %s in the editor's preview for the user to hear; stop ends it."), *Component->Sound->GetPathName())
			: TEXT("Set on the playing preview."));
	}
}

FHyperAIAudioPreviewReport FHyperAIStudioAudioAuthoringContracts::Preview(const FHyperAIAudioPreviewRequest& Request)
{
	FHyperAIAudioPreviewReport Report;
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("hyper_audio_preview runs on the game thread.");
		return Report;
	}
	return HyperAIStudio::AudioAuthoring::Preview::Run(Request);
}
