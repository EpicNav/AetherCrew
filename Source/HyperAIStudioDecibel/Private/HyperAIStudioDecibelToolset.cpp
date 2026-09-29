// Games by Hyper 2026.

#include "HyperAIStudioDecibelToolset.h"

#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/CoreDelegates.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#if WITH_DECIBEL
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "DecibelAudioEngine.h"
#include "DecibelDeviceCatalog.h"
#include "DecibelSession.h"
#include "DecibelSessionCompiler.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioSourceControl.h"
#include "IAssetTools.h"
#include "Interfaces/IPluginManager.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundSource.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/StringOutputDevice.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDecibel, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioDecibel"

FString FHyperAIStudioDecibelContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioDecibel.HyperAIStudioDecibelToolset");
}

TArray<FString> FHyperAIStudioDecibelContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName, RenderToolName};
}

bool FHyperAIStudioDecibelContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

#if WITH_DECIBEL

namespace HyperAIStudio::Decibel::Private
{
	using C = FHyperAIStudioDecibelContracts;

	FString KindName(const EDecibelDeviceKind Kind)
	{
		switch (Kind)
		{
		case EDecibelDeviceKind::Generator: return TEXT("generator");
		case EDecibelDeviceKind::AudioEffect: return TEXT("audio_effect");
		case EDecibelDeviceKind::MidiModifier: return TEXT("midi_effect");
		default: return TEXT("modulator");
		}
	}

	FString TrackTypeName(const EDecibelTrackType Type)
	{
		switch (Type)
		{
		case EDecibelTrackType::Audio: return TEXT("audio");
		case EDecibelTrackType::Instrument: return TEXT("instrument");
		case EDecibelTrackType::Fx: return TEXT("fx");
		case EDecibelTrackType::Label: return TEXT("label");
		case EDecibelTrackType::Foley: return TEXT("foley");
		default: return TEXT("visual");
		}
	}

	FString ClipKindName(const EDecibelClipKind Kind)
	{
		switch (Kind)
		{
		case EDecibelClipKind::Audio: return TEXT("audio");
		case EDecibelClipKind::Midi: return TEXT("midi");
		case EDecibelClipKind::Trigger: return TEXT("trigger");
		default: return TEXT("other");
		}
	}

	FString PresetFolder()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Decibel"));
		return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/Presets")) : FString();
	}

	FString ObjectPath(const FString& PackageOrObject)
	{
		return PackageOrObject.Contains(TEXT(".")) ? PackageOrObject
			: PackageOrObject + TEXT(".") + FPackageName::GetShortName(PackageOrObject);
	}

	FString PackageOf(const FString& PackageOrObject)
	{
		FString Package = PackageOrObject;
		int32 Dot = INDEX_NONE;
		if (Package.FindChar(TEXT('.'), Dot))
		{
			Package.LeftInline(Dot);
		}
		return Package;
	}

	/** A sound file on disk, such as a sound library's WAV, which a plan imports before it uses it. */
	bool IsSoundFile(const FString& Sound)
	{
		static const TArray<FString> Extensions = {TEXT("wav"), TEXT("flac"), TEXT("ogg"), TEXT("aif"), TEXT("aiff")};
		return !Sound.StartsWith(TEXT("/Game/")) && !FPaths::IsRelative(Sound) && Extensions.Contains(FPaths::GetExtension(Sound).ToLower());
	}

	/** Where a sound file lives once imported: /Game/Audio/Library/<its name>. */
	FString LibraryPackage(const FString& File)
	{
		return TEXT("/Game/Audio/Library/") + ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(File));
	}

	USoundWave* ImportWave(const FString& File, const FString& Package, const bool bLoop, FString& OutError);

	bool IsProjectPackage(const FString& Package, FString& OutError)
	{
		if (!Package.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Package))
		{
			OutError = FString::Printf(TEXT("'%s' is not a package path under /Game, such as /Game/Audio/SFX/DS_RifleShot."), *Package);
			return false;
		}
		return true;
	}

	double SecondsPerBeat(const UDecibelSession& Session)
	{
		return 60.0 / FMath::Max(1, Session.Bpm);
	}

	FString AutoParamName(const EDecibelAutoParam Param)
	{
		switch (Param)
		{
		case EDecibelAutoParam::Volume: return TEXT("volume");
		case EDecibelAutoParam::Pan: return TEXT("pan");
		case EDecibelAutoParam::Pitch: return TEXT("pitch");
		default: return StaticEnum<EDecibelAutoParam>()->GetNameStringByValue(static_cast<int64>(Param)).ToLower();
		}
	}

	/** What a track's automation drives, "volume: 3 points" or "fx.filter.fltcut: 2 points". */
	TArray<FString> DescribeLanes(const FDecibelTrackData& Track)
	{
		TArray<FString> Out;
		for (const FDecibelAutomationLane& Lane : Track.AutomationLanes)
		{
			const FString Target = Lane.TargetKind == EDecibelAutoTarget::TrackParam ? AutoParamName(Lane.Param)
				: Track.Devices.IsValidIndex(Lane.DeviceIndex) ? Track.Devices[Lane.DeviceIndex].DeviceId + TEXT(".") + Lane.DeviceParamId.ToString()
				: Lane.DeviceParamId.ToString();
			Out.Add(FString::Printf(TEXT("%s: %d points"), *Target, Lane.Points.Num()));
		}
		return Out;
	}

	TArray<FHyperAISoundTrackInfo> DescribeTracks(const UDecibelSession& Session)
	{
		TArray<FHyperAISoundTrackInfo> Out;
		for (int32 Index = 0; Index < Session.Tracks.Num(); ++Index)
		{
			const FDecibelTrackData& Track = Session.Tracks[Index];
			FHyperAISoundTrackInfo& Info = Out.AddDefaulted_GetRef();
			Info.Index = Index;
			Info.Name = Track.Name;
			Info.Type = TrackTypeName(Track.TrackType);
			Info.Volume = Track.Volume;
			Info.Pan = Track.Pan;
			for (const FDecibelDevice& Device : Track.Devices)
			{
				Info.Devices.Add(Device.DeviceId);
			}
			for (const FDecibelClipData& Clip : Track.Clips)
			{
				FHyperAISoundClipInfo& ClipInfo = Info.Clips.AddDefaulted_GetRef();
				ClipInfo.Kind = ClipKindName(Clip.Kind);
				ClipInfo.StartBeat = Clip.StartBeat;
				ClipInfo.LengthBeats = Clip.LengthBeats;
				ClipInfo.Notes = Clip.Notes.Num();
				ClipInfo.Sound = Clip.Sound.ToSoftObjectPath().ToString();
			}
			for (const FDecibelSend& Send : Track.Sends)
			{
				const FDecibelTrackData* Target = Session.Tracks.FindByPredicate([&Send](const FDecibelTrackData& Row) { return Row.TrackId == Send.TargetId; });
				Info.Sends.Add(FString::Printf(TEXT("%s x %.2f"), Target ? *Target->Name : TEXT("master"), Send.Amount));
			}
			Info.Lanes = DescribeLanes(Track);
		}
		return Out;
	}

	/** Where the last clip ends, in beats. */
	double LastClipEndBeat(const UDecibelSession& Session)
	{
		double End = 0.0;
		for (const FDecibelTrackData& Track : Session.Tracks)
		{
			for (const FDecibelClipData& Clip : Track.Clips)
			{
				End = FMath::Max(End, Clip.StartBeat + Clip.LengthBeats);
			}
		}
		return End;
	}

	/** Track by index, by name, or master (for device ops). */
	bool FindTrack(const UDecibelSession& Session, const FString& Spec, const bool bAllowMaster, int32& OutIndex, FString& OutError)
	{
		const FString Trimmed = Spec.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutError = TEXT("Name the track: its index from 0, its name, or master.");
			return false;
		}
		if (Trimmed.Equals(TEXT("master"), ESearchCase::IgnoreCase))
		{
			if (!bAllowMaster)
			{
				OutError = TEXT("The master only holds devices: clips and track settings go on a track.");
				return false;
			}
			OutIndex = UDecibelSession::MasterDeviceTrackIndex;
			return true;
		}
		if (Trimmed.IsNumeric())
		{
			OutIndex = FCString::Atoi(*Trimmed);
			if (!Session.Tracks.IsValidIndex(OutIndex))
			{
				OutError = FString::Printf(TEXT("There is no track %d: the session has %d."), OutIndex, Session.Tracks.Num());
				return false;
			}
			return true;
		}
		OutIndex = Session.Tracks.IndexOfByPredicate([&Trimmed](const FDecibelTrackData& Track) { return Track.Name.Equals(Trimmed, ESearchCase::IgnoreCase); });
		if (OutIndex == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("No track is named '%s'."), *Trimmed);
			return false;
		}
		return true;
	}

	FDecibelTrackData& Row(UDecibelSession& Session, const int32 TrackIndex)
	{
		return TrackIndex == UDecibelSession::MasterDeviceTrackIndex ? Session.MasterChainRow() : Session.Tracks[TrackIndex];
	}

	bool FindDevice(const FDecibelTrackData& Track, const FHyperAISoundOp& Op, int32& OutIndex, FString& OutError)
	{
		if (Op.Device >= 0)
		{
			if (!Track.Devices.IsValidIndex(Op.Device))
			{
				OutError = FString::Printf(TEXT("The track has %d device(s): there is no device %d."), Track.Devices.Num(), Op.Device);
				return false;
			}
			OutIndex = Op.Device;
			return true;
		}
		OutIndex = Track.Devices.IndexOfByPredicate([&Op](const FDecibelDevice& Device) { return Device.DeviceId == Op.DeviceId; });
		if (OutIndex == INDEX_NONE)
		{
			OutError = Op.DeviceId.IsEmpty() ? FString(TEXT("Name the device: its DeviceId, or its Device index on the track."))
				: FString::Printf(TEXT("The track has no %s device."), *Op.DeviceId);
			return false;
		}
		return true;
	}

	/** Sets each value, fitted into the parameter's range; OutNotes says which were fitted. */
	bool SetParams(FDecibelDevice& Device, const TArray<FHyperAISoundParamValue>& Values, TArray<FString>& OutNotes, FString& OutError)
	{
		for (const FHyperAISoundParamValue& Value : Values)
		{
			FDecibelDeviceParam* Param = Device.FindParam(FName(*Value.Id));
			if (!Param)
			{
				TArray<FString> Ids;
				for (const FDecibelDeviceParam& Known : Device.Params)
				{
					Ids.Add(Known.Id.ToString());
				}
				OutError = FString::Printf(TEXT("%s has no parameter '%s'. It has: %s."), *Device.DeviceId, *Value.Id, *FString::Join(Ids, TEXT(", ")).Left(1500));
				return false;
			}
			const float Fitted = FMath::Clamp(Value.Value, Param->MinValue, Param->MaxValue);
			if (Fitted != Value.Value)
			{
				OutNotes.Add(FString::Printf(TEXT("%s.%s: %g is outside %g..%g, so %g was used."), *Device.DeviceId, *Value.Id, Value.Value,
					Param->MinValue, Param->MaxValue, Fitted));
			}
			Param->Value = Fitted;
		}
		return true;
	}

	bool LoadPreset(FDecibelDevice& Device, const FString& Preset, FString& OutError)
	{
		const FString File = PresetFolder() / Device.DeviceId / Preset + TEXT(".dbpreset");
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File))
		{
			OutError = FString::Printf(TEXT("%s has no preset '%s': hyper_sound_inspect with DeviceId and bPresets lists them."), *Device.DeviceId, *Preset);
			return false;
		}
		FDecibelDevicePresetFile Parsed;
		FStringOutputDevice Errors;
		FDecibelDevicePresetFile::StaticStruct()->ImportText(*Text.TrimStartAndEnd(), &Parsed, nullptr, PPF_None, &Errors, TEXT("DecibelPreset"));
		if (Parsed.Block.IsEmpty())
		{
			OutError = FString::Printf(TEXT("The preset '%s' could not be read: %s"), *Preset, *FString(Errors).Left(300));
			return false;
		}
		for (const FDecibelDeviceParam& Source : Parsed.Block[0].Params)
		{
			if (FDecibelDeviceParam* Param = Device.FindParam(Source.Id))
			{
				Param->Value = FMath::Clamp(Source.Value, Param->MinValue, Param->MaxValue);
			}
		}
		return true;
	}

	/**
	 * The SoundWave a Sound names: a project asset, or a sound file on disk, which the plan imported before running for real.
	 * A trial imports nothing, so a file never imported before comes back null with bPending set, and no error.
	 */
	USoundWave* ResolveSound(const FString& Sound, const TMap<FString, USoundWave*>* Library, bool& bPending, FString& OutError)
	{
		bPending = false;
		USoundWave* Wave = nullptr;
		if (IsSoundFile(Sound))
		{
			if (!FPaths::FileExists(Sound))
			{
				OutError = FString::Printf(TEXT("There is no file %s."), *Sound);
				return nullptr;
			}
			Wave = Library ? Library->FindRef(Sound) : LoadObject<USoundWave>(nullptr, *ObjectPath(LibraryPackage(Sound)), nullptr, LOAD_NoWarn | LOAD_Quiet);
			bPending = !Wave && !Library;
		}
		else
		{
			Wave = LoadObject<USoundWave>(nullptr, *ObjectPath(Sound));
		}
		if (!Wave && !bPending)
		{
			OutError = FString::Printf(TEXT("'%s' is not a SoundWave, or a sound file on disk."), *Sound);
		}
		return Wave;
	}

	/** device.add, device.set: the sound a sampler plays or a convolution reverb takes as its room, and how a sampler reads it. */
	bool SetDeviceSound(FDecibelDevice& Device, const FHyperAISoundOp& Op, const TMap<FString, USoundWave*>* Library, FString& OutError)
	{
		const bool bSampler = Device.DeviceId == TEXT("gen.sampler");
		if (!Op.Sound.IsEmpty())
		{
			if (!bSampler && Device.DeviceId != TEXT("fx.verb"))
			{
				OutError = FString::Printf(TEXT("%s takes no sound: gen.sampler plays one, fx.verb takes one as its impulse response."), *Device.DeviceId);
				return false;
			}
			bool bPending = false;
			USoundWave* Wave = ResolveSound(Op.Sound, Library, bPending, OutError);
			if (!Wave && !bPending)
			{
				return false;
			}
			Device.Sample = Wave;
		}
		if (!Op.SamplerMode.IsEmpty())
		{
			// EDecibelSampleMode: the ones a plan can use without a pool, zones or slices of their own.
			static const TMap<FString, uint8> Modes = {{TEXT("one_shot"), 0}, {TEXT("loop"), 1}, {TEXT("granular"), 3}};
			const uint8* Mode = bSampler ? Modes.Find(Op.SamplerMode.TrimStartAndEnd().ToLower()) : nullptr;
			if (!Mode)
			{
				OutError = bSampler ? FString::Printf(TEXT("A sampler's mode is one_shot, loop or granular, not '%s'."), *Op.SamplerMode)
					: FString::Printf(TEXT("%s is not a sampler: only gen.sampler has a SamplerMode."), *Device.DeviceId);
				return false;
			}
			Device.SamplerMode = *Mode;
		}
		return true;
	}

	/**
	 * lane.set: a track's (or the master's) automation on one target, replacing what was there. Points arrive in the
	 * target's own units and are stored on Decibel's normalised axis through the same laws its editor uses: the fader's
	 * dB for volume, the pitch lane's +-24 semitones, a device parameter's own RealToNorm (its skew and taper).
	 */
	bool SetLane(FDecibelTrackData& Track, const bool bMaster, const FHyperAISoundOp& Op, FString& OutError)
	{
		static const TMap<FString, EDecibelAutoParam> TrackParams = {
			{TEXT("volume"), EDecibelAutoParam::Volume}, {TEXT("pan"), EDecibelAutoParam::Pan}, {TEXT("pitch"), EDecibelAutoParam::Pitch}};
		const FString Name = Op.Param.TrimStartAndEnd();
		if (Name.IsEmpty() || Op.Points.IsEmpty())
		{
			OutError = TEXT("A lane names its Param (volume, pan, pitch, or a device parameter's id with DeviceId) and gives its Points.");
			return false;
		}
		FDecibelAutomationLane Lane;
		TFunction<float(float)> ToNorm;
		if (const EDecibelAutoParam* Param = TrackParams.Find(Name.ToLower()))
		{
			if (bMaster && *Param == EDecibelAutoParam::Pitch)
			{
				OutError = TEXT("The master has no pitch: its lanes are volume, pan and its devices' parameters.");
				return false;
			}
			Lane.Param = *Param;
			switch (*Param)
			{
			case EDecibelAutoParam::Volume:
				ToNorm = [](const float Db) { return ::Decibel::DecibelGainToFaderPos(::Decibel::DecibelDbToGain(FMath::Clamp(Db, -60.f, 12.f))); };
				break;
			case EDecibelAutoParam::Pan:
				ToNorm = [](const float PanValue) { return (FMath::Clamp(PanValue, -1.f, 1.f) + 1.f) * 0.5f; };
				break;
			default:
				ToNorm = [](const float Semitones) { return static_cast<float>(::Decibel::DecibelPitchAutoSemisToValue(FMath::Clamp(Semitones, -24.f, 24.f))); };
				break;
			}
		}
		else
		{
			int32 DeviceIndex = INDEX_NONE;
			if (!FindDevice(Track, Op, DeviceIndex, OutError))
			{
				return false;
			}
			const FDecibelDevice& Device = Track.Devices[DeviceIndex];
			const FDecibelDeviceParam* DeviceParam = Device.FindParam(FName(*Name));
			if (!DeviceParam)
			{
				OutError = FString::Printf(TEXT("%s has no parameter '%s' to automate: hyper_sound_inspect with its DeviceId lists them."), *Device.DeviceId, *Name);
				return false;
			}
			Lane.TargetKind = EDecibelAutoTarget::DeviceParam;
			Lane.DeviceIndex = DeviceIndex;
			Lane.DeviceParamId = DeviceParam->Id;
			Lane.DeviceParamLabel = DeviceParam->Label;
			const FDecibelDeviceParam Copy = *DeviceParam;
			ToNorm = [Copy](const float Real) { return Copy.RealToNorm(Real); };
		}
		for (const FHyperAISoundAutoPoint& Point : Op.Points)
		{
			FDecibelAutoPoint& Stored = Lane.Points.AddDefaulted_GetRef();
			Stored.Beat = FMath::Max(0.0, Point.Beat);
			Stored.Value = FMath::Clamp(ToNorm(Point.Value), 0.f, 1.f);
			Stored.Curve = FMath::Clamp(Point.Curve, -1.f, 1.f);
		}
		Lane.Points.StableSort([](const FDecibelAutoPoint& A, const FDecibelAutoPoint& B) { return A.Beat < B.Beat; });
		FDecibelAutomationLane* Existing = Track.AutomationLanes.FindByPredicate([&Lane](const FDecibelAutomationLane& Other)
		{
			return Other.TargetKind == Lane.TargetKind && (Lane.TargetKind == EDecibelAutoTarget::TrackParam ? Other.Param == Lane.Param
				: Other.DeviceIndex == Lane.DeviceIndex && Other.DeviceParamId == Lane.DeviceParamId);
		});
		if (Existing)
		{
			Existing->Points = MoveTemp(Lane.Points);
		}
		else
		{
			Track.AutomationLanes.Add(MoveTemp(Lane));
		}
		return true;
	}

	/** Applies one op; false with OutError stops the plan. */
	bool ApplyOp(UDecibelSession& Session, const FHyperAISoundOp& Op, const TMap<FString, USoundWave*>* Library, TArray<FString>& OutNotes, FString& OutError)
	{
		const FString Kind = Op.Kind.TrimStartAndEnd().ToLower();
		if (Kind == TEXT("session.set"))
		{
			if (Op.Bpm > 0) Session.Bpm = FMath::Clamp(Op.Bpm, 20, 400);
			if (Op.TimeSigNumerator > 0) Session.TimeSigNumerator = FMath::Clamp(Op.TimeSigNumerator, 1, 32);
			if (Op.TimeSigDenominator > 0) Session.TimeSigDenominator = FMath::Clamp(Op.TimeSigDenominator, 1, 32);
			if (Op.OneShot >= 0) Session.bOneShot = Op.OneShot != 0;
			if (Op.Loop >= 0) Session.bLoopActive = Op.Loop != 0;
			if (Op.LoopStartBeat >= 0.0) Session.LoopStartBeat = Op.LoopStartBeat;
			if (Op.LoopEndBeat >= 0.0) Session.LoopEndBeat = Op.LoopEndBeat;
			if (Session.bLoopActive && Session.LoopEndBeat <= Session.LoopStartBeat)
			{
				OutError = TEXT("The loop must end after it starts.");
				return false;
			}
			return true;
		}
		if (Kind == TEXT("track.add"))
		{
			const FString Type = Op.Type.TrimStartAndEnd().ToLower();
			const EDecibelTrackType TrackType = Type == TEXT("instrument") ? EDecibelTrackType::Instrument
				: Type == TEXT("fx") ? EDecibelTrackType::Fx : EDecibelTrackType::Audio;
			if (!Type.IsEmpty() && Type != TEXT("audio") && Type != TEXT("instrument") && Type != TEXT("fx"))
			{
				OutError = FString::Printf(TEXT("A track's Type is audio, instrument or fx, not '%s'."), *Op.Type);
				return false;
			}
			const int32 Index = Session.AddTrack(TrackType);
			if (!Session.Tracks.IsValidIndex(Index))
			{
				OutError = TEXT("Decibel did not add the track.");
				return false;
			}
			if (!Op.Name.IsEmpty())
			{
				Session.Tracks[Index].Name = Op.Name;
			}
			return true;
		}
		int32 TrackIndex = INDEX_NONE;
		const bool bDeviceOp = Kind.StartsWith(TEXT("device."));
		const bool bMaster = Op.Track.TrimStartAndEnd().Equals(TEXT("master"), ESearchCase::IgnoreCase);
		if (!FindTrack(Session, Op.Track, bDeviceOp || Kind == TEXT("lane.set"), TrackIndex, OutError))
		{
			return false;
		}
		FDecibelTrackData& Track = Row(Session, TrackIndex);
		if (Kind == TEXT("track.set"))
		{
			if (!Op.Name.IsEmpty()) Track.Name = Op.Name;
			if (Op.Volume >= 0.f) Track.Volume = FMath::Clamp(Op.Volume, 0.f, 4.f);
			if (Op.Pan >= -1.f) Track.Pan = FMath::Clamp(Op.Pan, -1.f, 1.f);
			return true;
		}
		if (Kind == TEXT("track.send"))
		{
			int32 TargetIndex = INDEX_NONE;
			if (!FindTrack(Session, Op.SendTo, /*bAllowMaster=*/false, TargetIndex, OutError))
			{
				return false;
			}
			if (TargetIndex == TrackIndex || Session.Tracks[TargetIndex].TrackType != EDecibelTrackType::Fx)
			{
				OutError = TEXT("A send feeds a return: an fx track other than the one sending.");
				return false;
			}
			static const TMap<FString, EDecibelSendTap> Taps = {
				{TEXT("post"), EDecibelSendTap::PostFader}, {TEXT("pre"), EDecibelSendTap::PreFader}, {TEXT("prefx"), EDecibelSendTap::PreFx}};
			const FString TapName = Op.SendTap.TrimStartAndEnd().ToLower();
			const EDecibelSendTap* Tap = Taps.Find(TapName.IsEmpty() ? FString(TEXT("post")) : TapName);
			if (!Tap)
			{
				OutError = FString::Printf(TEXT("A send taps post, pre or prefx, not '%s'."), *Op.SendTap);
				return false;
			}
			const FGuid TargetId = Session.Tracks[TargetIndex].TrackId;
			FDecibelSend* Send = Track.Sends.FindByPredicate([&TargetId](const FDecibelSend& Existing) { return Existing.TargetId == TargetId; });
			if (!Send)
			{
				Send = &Track.Sends.AddDefaulted_GetRef();
				Send->TargetId = TargetId;
			}
			Send->Amount = FMath::Clamp(Op.SendGain, 0.f, 4.f);
			Send->bEnabled = true;
			DecibelSetSendTap(*Send, *Tap);
			return true;
		}
		if (Kind == TEXT("lane.set"))
		{
			return SetLane(Track, bMaster, Op, OutError);
		}
		if (Kind == TEXT("track.remove"))
		{
			Session.RemoveTrack(TrackIndex);
			return true;
		}
		if (Kind == TEXT("device.add"))
		{
			if (!DecibelDevices::Find(Op.DeviceId))
			{
				OutError = FString::Printf(TEXT("Decibel has no device '%s': hyper_sound_inspect with bCatalog lists them."), *Op.DeviceId);
				return false;
			}
			const int32 Before = Track.Devices.Num();
			Session.AddDevice(TrackIndex, Op.DeviceId);
			FDecibelTrackData& After = Row(Session, TrackIndex);
			if (After.Devices.Num() != Before + 1)
			{
				OutError = FString::Printf(TEXT("Decibel did not add %s to this track: a generator goes on an instrument track, an effect on any."), *Op.DeviceId);
				return false;
			}
			FDecibelDevice* Added = After.Devices.FindByPredicate([&Op](const FDecibelDevice& Device) { return Device.DeviceId == Op.DeviceId; });
			if (Added && !Op.Preset.IsEmpty() && !LoadPreset(*Added, Op.Preset, OutError))
			{
				return false;
			}
			return !Added || (SetParams(*Added, Op.Params, OutNotes, OutError) && SetDeviceSound(*Added, Op, Library, OutError));
		}
		int32 DeviceIndex = INDEX_NONE;
		if (bDeviceOp && !FindDevice(Track, Op, DeviceIndex, OutError))
		{
			return false;
		}
		if (Kind == TEXT("device.set"))
		{
			return SetParams(Track.Devices[DeviceIndex], Op.Params, OutNotes, OutError) && SetDeviceSound(Track.Devices[DeviceIndex], Op, Library, OutError);
		}
		if (Kind == TEXT("device.preset"))
		{
			return LoadPreset(Track.Devices[DeviceIndex], Op.Preset, OutError) && SetParams(Track.Devices[DeviceIndex], Op.Params, OutNotes, OutError);
		}
		if (Kind == TEXT("device.remove"))
		{
			Session.RemoveDeviceWithLanes(TrackIndex, DeviceIndex);
			return true;
		}
		if (Kind == TEXT("clip.remove"))
		{
			if (!Track.Clips.IsValidIndex(Op.Clip))
			{
				OutError = FString::Printf(TEXT("The track has %d clip(s): there is no clip %d."), Track.Clips.Num(), Op.Clip);
				return false;
			}
			Track.Clips.RemoveAt(Op.Clip);
			return true;
		}
		if (Kind != TEXT("clip.midi") && Kind != TEXT("clip.trigger") && Kind != TEXT("clip.audio"))
		{
			OutError = FString::Printf(TEXT("Unknown op '%s': session.set, track.add, track.set, track.send, track.remove, device.add, device.set, ")
				TEXT("device.preset, device.remove, lane.set, clip.midi, clip.trigger, clip.audio or clip.remove."), *Op.Kind);
			return false;
		}
		if (Op.StartBeat < 0.0)
		{
			OutError = TEXT("A clip starts at beat 0 or later.");
			return false;
		}
		FDecibelClipData Clip;
		Clip.StartBeat = Op.StartBeat;
		if (Kind == TEXT("clip.audio"))
		{
			if (Track.TrackType != EDecibelTrackType::Audio)
			{
				OutError = TEXT("An audio clip goes on an audio track.");
				return false;
			}
			bool bPending = false;
			USoundWave* Wave = ResolveSound(Op.Sound, Library, bPending, OutError);
			if (bPending)
			{
				// A trial, before the file is imported: the clip holds its place.
				Clip.Kind = EDecibelClipKind::Audio;
				Clip.Name = FPaths::GetBaseFilename(Op.Sound);
				Clip.LengthBeats = Op.LengthBeats > 0.0 ? Op.LengthBeats : 4.0;
				Track.Clips.Add(MoveTemp(Clip));
				return true;
			}
			if (!Wave)
			{
				return false;
			}
			Clip.Kind = EDecibelClipKind::Audio;
			Clip.Sound = Wave;
			const double Duration = Wave->GetDuration();
			if (Op.SourceStartSeconds >= Duration)
			{
				OutError = FString::Printf(TEXT("%s lasts %.2f s: it has nothing after %.2f s."), *Wave->GetName(), Duration, Op.SourceStartSeconds);
				return false;
			}
			Clip.StartOffsetSec = FMath::Max(0.0, Op.SourceStartSeconds);
			Clip.SrcLenSec = Op.SourceSeconds > 0.0 ? FMath::Min(Op.SourceSeconds, Duration - Clip.StartOffsetSec) : Duration - Clip.StartOffsetSec;
			Clip.Gain = FMath::Clamp(Op.Gain, 0.f, ::Decibel::DecibelDbToGain(::Decibel::kDecibelClipGainMaxDb));
			Clip.bReversed = Op.bReverse;
			if (!Op.StretchMode.IsEmpty())
			{
				static const TMap<FString, EDecibelStretchMode> Modes = {{TEXT("tape"), EDecibelStretchMode::Default},
					{TEXT("default"), EDecibelStretchMode::Default}, {TEXT("texture"), EDecibelStretchMode::Texture}, {TEXT("complex"), EDecibelStretchMode::Complex}};
				const EDecibelStretchMode* Mode = Modes.Find(Op.StretchMode.TrimStartAndEnd().ToLower());
				if (!Mode)
				{
					OutError = FString::Printf(TEXT("A clip's StretchMode is tape, texture or complex, not '%s'."), *Op.StretchMode);
					return false;
				}
				Clip.StretchMode = *Mode;
			}
			Clip.TextureGrainMs = FMath::Clamp(Op.TextureGrainMs, 5.0, 500.0);
			Clip.TextureJitter = FMath::Clamp(Op.TextureJitter, 0.0, 1.0);
			Clip.TextureTranspose = FMath::Clamp(Op.TextureTranspose, -48.0, 48.0);
			Clip.Pitch = FMath::Clamp(Op.Pitch, -48.f, 48.f);
			Clip.FadeInBeats = FMath::Max(0.0, Op.FadeInBeats);
			Clip.FadeOutBeats = FMath::Max(0.0, Op.FadeOutBeats);
			Clip.Name = Wave->GetName();
			Clip.LengthBeats = Op.LengthBeats > 0.0 ? Op.LengthBeats : Clip.SrcLenSec / SecondsPerBeat(Session);
		}
		else
		{
			if (Track.TrackType != EDecibelTrackType::Instrument)
			{
				OutError = TEXT("MIDI and trigger clips go on an instrument track, which plays them through its generator.");
				return false;
			}
			if (!Track.Devices.ContainsByPredicate([](const FDecibelDevice& Device) { return Device.Kind == EDecibelDeviceKind::Generator; }))
			{
				OutNotes.Add(FString::Printf(TEXT("Track '%s' has no generator yet: its clips play nothing until device.add puts one on it."), *Track.Name));
			}
			if (Kind == TEXT("clip.trigger"))
			{
				Clip.Kind = EDecibelClipKind::Trigger;
				Clip.LengthBeats = Op.LengthBeats > 0.0 ? Op.LengthBeats : 1.0;
			}
			else
			{
				if (Op.Notes.IsEmpty() || Op.Notes.Num() > C::MaxNotesPerClip)
				{
					OutError = FString::Printf(TEXT("A MIDI clip needs 1 to %d notes."), C::MaxNotesPerClip);
					return false;
				}
				Clip.Kind = EDecibelClipKind::Midi;
				double LastEnd = 0.0;
				for (const FHyperAISoundNote& In : Op.Notes)
				{
					FDecibelMidiNote& Note = Clip.Notes.AddDefaulted_GetRef();
					Note.Pitch = FMath::Clamp(In.Pitch, 0, 127);
					Note.StartBeat = FMath::Max(0.0, In.StartBeat);
					Note.LengthBeats = FMath::Max(0.03125, In.LengthBeats);
					Note.Velocity = FMath::Clamp(In.Velocity, 0.f, 1.f);
					LastEnd = FMath::Max(LastEnd, Note.StartBeat + Note.LengthBeats);
				}
				Clip.LengthBeats = Op.LengthBeats > 0.0 ? Op.LengthBeats : FMath::CeilToDouble(LastEnd);
			}
		}
		Track.Clips.Add(MoveTemp(Clip));
		return true;
	}

	/** Runs the plan on Session; stops at the first failing op, naming it. Library holds the sound files imported for it. */
	bool ApplyAll(UDecibelSession& Session, const TArray<FHyperAISoundOp>& Ops, const TMap<FString, USoundWave*>* Library, int32& OutApplied,
		TArray<FString>& OutNotes, FString& OutError)
	{
		OutApplied = 0;
		for (int32 Index = 0; Index < Ops.Num(); ++Index)
		{
			FString Error;
			if (!ApplyOp(Session, Ops[Index], Library, OutNotes, Error))
			{
				OutError = FString::Printf(TEXT("Operation %d (%s): %s"), Index, *Ops[Index].Kind, *Error);
				return false;
			}
			++OutApplied;
		}
		return true;
	}

	bool SaveAsset(UObject* Asset, FString& OutError)
	{
		UPackage* Package = Asset->GetOutermost();
		if (!FHyperAIStudioSourceControl::MakeEditable({Package->GetName()}, OutError))
		{
			return false;
		}
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(Package, Asset, *File, Args))
		{
			OutError = FString::Printf(TEXT("%s could not be saved."), *Package->GetName());
			return false;
		}
		return true;
	}

	void RecordOutcome(const TCHAR* Tool, const FString& OperationId, const FString& Target, const bool bOk, const FString& Detail)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = Tool;
		Entry.Target = Target;
		Entry.OperationId = OperationId;
		Entry.StatusCode = bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Detail.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	FString NewOperationId(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s-%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower());
	}

	/** The plan applied for real: one undo step, then saved. */
	FHyperAISoundPlanReport Execute(const FHyperAISoundPlanRequest& Request, const FString& OperationId)
	{
		FHyperAISoundPlanReport Report;
		Report.OperationId = OperationId;
		Report.SessionPath = ObjectPath(Request.SessionPath);
		const FString Package = PackageOf(Request.SessionPath);
		UDecibelSession* Session = nullptr;
		FString Error;
		// Sound files on disk become SoundWaves under /Game/Audio/Library first, outside the undo step, once each.
		TMap<FString, USoundWave*> Library;
		for (const FHyperAISoundOp& Op : Request.Operations)
		{
			if (!IsSoundFile(Op.Sound) || Library.Contains(Op.Sound))
			{
				continue;
			}
			const FString LibraryAsset = LibraryPackage(Op.Sound);
			USoundWave* Wave = LoadObject<USoundWave>(nullptr, *ObjectPath(LibraryAsset), nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Wave && !(Wave = ImportWave(Op.Sound, LibraryAsset, /*bLoop=*/false, Error)))
			{
				Report.Status = TEXT("import_failed");
				Report.Diagnostic = FString::Printf(TEXT("%s could not be imported: %s"), *Op.Sound, *Error);
				return Report;
			}
			Library.Add(Op.Sound, Wave);
		}
		{
			const FScopedTransaction Transaction(LOCTEXT("SoundPlan", "HyperAI: Edit Decibel Session"));
			if (Request.bCreate)
			{
				UPackage* Outer = CreatePackage(*Package);
				Session = NewObject<UDecibelSession>(Outer, *FPackageName::GetShortName(Package), RF_Public | RF_Standalone | RF_Transactional);
				Session->EnsureHostChainDefaults();
				FAssetRegistryModule::AssetCreated(Session);
			}
			else
			{
				Session = LoadObject<UDecibelSession>(nullptr, *Report.SessionPath);
			}
			if (!Session)
			{
				Report.Status = TEXT("session_not_found");
				Report.Diagnostic = FString::Printf(TEXT("%s is not a Decibel session."), *Report.SessionPath);
				return Report;
			}
			Session->Modify();
			if (!ApplyAll(*Session, Request.Operations, &Library, Report.Applied, Report.Notes, Error))
			{
				Report.Status = TEXT("operation_failed");
				Report.Diagnostic = Error + TEXT(" The operations before it stay applied; Edit > Undo takes the whole plan back.");
				Report.Tracks = DescribeTracks(*Session);
				return Report;
			}
			Session->MarkPackageDirty();
		}
		if (!SaveAsset(Session, Error))
		{
			Report.Status = TEXT("save_failed");
			Report.Diagnostic = Error;
			Report.Tracks = DescribeTracks(*Session);
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) applied to %s and saved. Hear it with hyper_sound_render."), Report.Applied, *Report.SessionPath);
		Report.Tracks = DescribeTracks(*Session);
		return Report;
	}

	struct FLevels
	{
		float PeakDb = -144.f;
		float RmsDb = -144.f;
		double LeadingSilenceSeconds = 0.0;
		bool bSilent = true;
		bool bClipped = false;
	};

	float ToDb(const double Linear)
	{
		return Linear > 1e-7 ? static_cast<float>(20.0 * FMath::LogX(10.0, Linear)) : -144.f;
	}

	FLevels Measure(const TArray<float>& Samples, const int32 Channels, const int32 SampleRate)
	{
		FLevels Levels;
		double Peak = 0.0;
		double SumSquares = 0.0;
		int64 FirstLoud = INDEX_NONE;
		constexpr double Threshold = 0.001; // -60 dBFS
		for (int64 Index = 0; Index < Samples.Num(); ++Index)
		{
			const double Value = FMath::Abs(static_cast<double>(Samples[Index]));
			Peak = FMath::Max(Peak, Value);
			SumSquares += Value * Value;
			if (FirstLoud == INDEX_NONE && Value > Threshold)
			{
				FirstLoud = Index;
			}
		}
		Levels.PeakDb = ToDb(Peak);
		Levels.RmsDb = Samples.IsEmpty() ? -144.f : ToDb(FMath::Sqrt(SumSquares / Samples.Num()));
		Levels.bSilent = FirstLoud == INDEX_NONE;
		Levels.bClipped = Peak >= 0.999;
		Levels.LeadingSilenceSeconds = Levels.bSilent ? 0.0 : static_cast<double>(FirstLoud / FMath::Max(1, Channels)) / SampleRate;
		return Levels;
	}

	bool WriteWav(const FString& File, const TArray<float>& Samples, const int32 Channels, const int32 SampleRate)
	{
		TArray<uint8> Bytes;
		const uint32 DataBytes = static_cast<uint32>(Samples.Num()) * 2;
		auto U32 = [&Bytes](const uint32 V) { Bytes.Append(reinterpret_cast<const uint8*>(&V), 4); };
		auto U16 = [&Bytes](const uint16 V) { Bytes.Append(reinterpret_cast<const uint8*>(&V), 2); };
		Bytes.Append(reinterpret_cast<const uint8*>("RIFF"), 4);
		U32(36 + DataBytes);
		Bytes.Append(reinterpret_cast<const uint8*>("WAVEfmt "), 8);
		U32(16);
		U16(1);
		U16(static_cast<uint16>(Channels));
		U32(static_cast<uint32>(SampleRate));
		U32(static_cast<uint32>(SampleRate * Channels * 2));
		U16(static_cast<uint16>(Channels * 2));
		U16(16);
		Bytes.Append(reinterpret_cast<const uint8*>("data"), 4);
		U32(DataBytes);
		Bytes.Reserve(Bytes.Num() + DataBytes);
		for (const float Sample : Samples)
		{
			const int16 Value = static_cast<int16>(FMath::RoundToInt(FMath::Clamp(Sample, -1.f, 1.f) * 32767.f));
			Bytes.Append(reinterpret_cast<const uint8*>(&Value), 2);
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
		return FFileHelper::SaveArrayToFile(Bytes, *File);
	}

	/** Imports a WAV as the SoundWave at Package, replacing one already there, and saves it. */
	USoundWave* ImportWave(const FString& File, const FString& Package, const bool bLoop, FString& OutError)
	{
		if (!FHyperAIStudioSourceControl::MakeEditable({Package}, OutError))
		{
			return nullptr;
		}
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = FPackageName::GetLongPackagePath(Package);
		Task->DestinationName = FPackageName::GetShortName(Package);
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = false;
		FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task});
		USoundWave* Wave = nullptr;
		for (UObject* Object : Task->GetObjects())
		{
			Wave = Wave ? Wave : Cast<USoundWave>(Object);
		}
		if (!Wave)
		{
			OutError = FString::Printf(TEXT("%s was rendered but did not import as a SoundWave."), *File);
			return nullptr;
		}
		Wave->Modify();
		Wave->bLooping = bLoop;
		return SaveAsset(Wave, OutError) ? Wave : nullptr;
	}

	/** A MetaSound Source that plays Wave: its Wave Player starts on play, loops or finishes the source. */
	FString WrapInMetaSound(USoundWave* Wave, const int32 Channels, const bool bLoop, FString& OutError)
	{
		const FString Folder = FPackageName::GetLongPackagePath(Wave->GetOutermost()->GetName());
		FString Name = Wave->GetName();
		Name.RemoveFromStart(TEXT("SW_"));
		Name = TEXT("MS_") + Name;
		// A MetaSound made by an earlier render already plays this SoundWave, which was just replaced in place. Building
		// over it would ask the editor whether to replace it, in a dialog nobody may be there to answer.
		if (UObject* Existing = LoadObject<UObject>(nullptr, *(Folder / Name + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			if (Existing->IsA<UMetaSoundSource>())
			{
				return Existing->GetPathName();
			}
			OutError = FString::Printf(TEXT("%s is taken by a %s."), *Existing->GetPathName(), *Existing->GetClass()->GetName());
			return FString();
		}
		UMetaSoundBuilderSubsystem& Builders = UMetaSoundBuilderSubsystem::GetChecked();
		EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
		FMetaSoundBuilderNodeOutputHandle OnPlay;
		FMetaSoundBuilderNodeInputHandle OnFinished;
		TArray<FMetaSoundBuilderNodeInputHandle> AudioOuts;
		const bool bStereo = Channels > 1;
		UMetaSoundSourceBuilder* Builder = Builders.CreateSourceBuilder(FName(*(Name + TEXT("_Builder"))), OnPlay, OnFinished, AudioOuts, Result,
			bStereo ? EMetaSoundOutputAudioFormat::Stereo : EMetaSoundOutputAudioFormat::Mono, /*bIsOneShot=*/!bLoop);
		if (!Builder || Result != EMetaSoundBuilderResult::Succeeded)
		{
			OutError = TEXT("The MetaSound builder did not start a source.");
			return FString();
		}
		const FMetaSoundNodeHandle Player = Builder->AddNodeByClassName(
			FMetasoundFrontendClassName(TEXT("UE"), TEXT("Wave Player"), bStereo ? TEXT("Stereo") : TEXT("Mono")), Result);
		bool bOk = Result == EMetaSoundBuilderResult::Succeeded;
		auto Input = [&](const TCHAR* InputName) { return Builder->FindNodeInputByName(Player, InputName, Result); };
		auto Output = [&](const TCHAR* OutputName) { return Builder->FindNodeOutputByName(Player, OutputName, Result); };
		if (bOk)
		{
			Builder->SetNodeInputDefault(Input(TEXT("Wave Asset")), Builders.CreateObjectMetaSoundLiteral(Wave), Result);
			bOk &= Result == EMetaSoundBuilderResult::Succeeded;
			FName BoolType;
			Builder->SetNodeInputDefault(Input(TEXT("Loop")), Builders.CreateBoolMetaSoundLiteral(bLoop, BoolType), Result);
			bOk &= Result == EMetaSoundBuilderResult::Succeeded;
			Builder->ConnectNodes(OnPlay, Input(TEXT("Play")), Result);
			bOk &= Result == EMetaSoundBuilderResult::Succeeded;
			const TArray<const TCHAR*> Outs = bStereo ? TArray<const TCHAR*>{TEXT("Out Left"), TEXT("Out Right")} : TArray<const TCHAR*>{TEXT("Out Mono")};
			for (int32 Index = 0; Index < Outs.Num() && AudioOuts.IsValidIndex(Index); ++Index)
			{
				Builder->ConnectNodes(Output(Outs[Index]), AudioOuts[Index], Result);
				bOk &= Result == EMetaSoundBuilderResult::Succeeded;
			}
			if (!bLoop)
			{
				Builder->ConnectNodes(Output(TEXT("On Finished")), OnFinished, Result);
				bOk &= Result == EMetaSoundBuilderResult::Succeeded;
			}
		}
		if (!bOk)
		{
			OutError = TEXT("The MetaSound's Wave Player could not be wired.");
			return FString();
		}
		if (!FHyperAIStudioSourceControl::MakeEditable({Folder / Name}, OutError))
		{
			return FString();
		}
		const TScriptInterface<IMetaSoundDocumentInterface> Built = UMetaSoundEditorSubsystem::GetChecked().BuildToAsset(Builder, TEXT("HyperAI Studio"), Name, Folder, Result);
		UObject* Asset = Built.GetObject();
		if (!Asset || Result != EMetaSoundBuilderResult::Succeeded)
		{
			OutError = TEXT("The MetaSound Source could not be built into an asset.");
			return FString();
		}
		return SaveAsset(Asset, OutError) ? Asset->GetPathName() : FString();
	}

	/** The physically modelled generators, which do not sound yet in an offline render of a session built here. */
	bool IsDesigner(const FString& DeviceId)
	{
		return DeviceId == TEXT("gen.weapon") || DeviceId == TEXT("gen.impact") || DeviceId == TEXT("gen.engine") || DeviceId == TEXT("gen.piano");
	}

	FString RelativeToProject(const FString& File)
	{
		FString Relative = File;
		FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		return Relative;
	}

	struct FRenderPlan
	{
		FString SessionObject;
		FString OutputPackage;
		double StartSeconds = 0.0;
		double DurationSeconds = 0.0;
	};

	bool PlanRender(const FHyperAISoundRenderRequest& Request, FRenderPlan& Out, FString& OutError)
	{
		Out.SessionObject = ObjectPath(Request.SessionPath);
		const UDecibelSession* Session = LoadObject<UDecibelSession>(nullptr, *Out.SessionObject);
		if (!Session)
		{
			OutError = FString::Printf(TEXT("%s is not a Decibel session."), *Out.SessionObject);
			return false;
		}
		FString Name = FPackageName::GetShortName(PackageOf(Request.SessionPath));
		Name.RemoveFromStart(TEXT("DS_"));
		Out.OutputPackage = Request.OutputPath.IsEmpty() ? FPackageName::GetLongPackagePath(PackageOf(Request.SessionPath)) / (TEXT("SW_") + Name)
			: PackageOf(Request.OutputPath);
		if (!IsProjectPackage(Out.OutputPackage, OutError))
		{
			return false;
		}
		if (Request.LoopCrossfadeSeconds > 0.0 && Request.LoopTailSeconds > 0.0)
		{
			OutError = TEXT("A loop crossfades (LoopCrossfadeSeconds, a bed that runs on past the loop point) or wraps its tail (LoopTailSeconds, "
				"music whose notes end there), not both.");
			return false;
		}
		const double Spb = SecondsPerBeat(*Session);
		Out.StartSeconds = FMath::Max(0.0, Request.StartSeconds);
		Out.DurationSeconds = Request.DurationSeconds > 0.0 ? Request.DurationSeconds
			: Session->bLoopActive ? (Session->LoopEndBeat - Session->LoopStartBeat) * Spb
			: LastClipEndBeat(*Session) * Spb + FMath::Clamp(Request.TailSeconds, 0.0, 30.0) - Out.StartSeconds;
		if (Session->bLoopActive && Request.DurationSeconds <= 0.0 && Request.StartSeconds <= 0.0)
		{
			Out.StartSeconds = Session->LoopStartBeat * Spb;
		}
		if (Out.DurationSeconds <= 0.0 || Out.DurationSeconds > C::MaxRenderSeconds)
		{
			OutError = FString::Printf(TEXT("A render lasts more than 0 and at most %.0f s; this one would last %.1f s. Add clips, or set DurationSeconds."),
				C::MaxRenderSeconds, Out.DurationSeconds);
			return false;
		}
		return true;
	}

	/** One offline render of Session into a SoundWave at Package; OutLevels measure it. */
	USoundWave* RenderOne(const UDecibelSession& Session, const FRenderPlan& Plan, const FString& Package, const FHyperAISoundRenderRequest& Request,
		FLevels& OutLevels, FString& OutWav, FString& OutError)
	{
		const int32 SampleRate = FMath::Clamp(Request.SampleRate, 22050, 96000);
		const int32 Channels = FMath::Clamp(Request.Channels, 1, 2);
		// Plug-ins are left out, as Decibel's own offline bounce does: a VST3 is not reproducible off the audio thread.
		const TSharedRef<const ::Decibel::FRenderSession> Snapshot = ::Decibel::CompileSession(Session, static_cast<double>(SampleRate),
			::Decibel::EDecibelCompileMode::Sync, ::Decibel::EDecibelCompileExternals::Strip);
		const bool bCrossfade = Request.LoopCrossfadeSeconds > 0.0;
		const double Wrap = bCrossfade ? FMath::Min(Request.LoopCrossfadeSeconds, Plan.DurationSeconds * 0.5)
			: FMath::Clamp(Request.LoopTailSeconds, 0.0, Plan.DurationSeconds);
		TArray<float> Samples;
		::Decibel::FAudioEngine::RenderWindowOffline(Snapshot, Plan.StartSeconds, Plan.DurationSeconds + Wrap, SampleRate, Channels, Samples);
		if (Samples.IsEmpty())
		{
			OutError = TEXT("Decibel's offline render returned nothing.");
			return nullptr;
		}
		// The loops. The seamless one fades the overrun past the end out over the start while the start fades in (equal
		// power), so the frame after the last is the one that followed it in the render. The tail one adds the overrun onto
		// the start as it is: the start keeps its first beat, and what rings past the loop point rings on into it.
		const int32 Frames = FMath::RoundToInt(Plan.DurationSeconds * SampleRate);
		const int32 Over = FMath::Min(FMath::RoundToInt(Wrap * SampleRate), Samples.Num() / Channels - Frames);
		if (Over > 0)
		{
			for (int32 Frame = 0; Frame < Over; ++Frame)
			{
				const float Angle = HALF_PI * Frame / Over;
				const float In = bCrossfade ? FMath::Sin(Angle) : 1.f;
				const float Out = bCrossfade ? FMath::Cos(Angle) : 1.f;
				for (int32 Channel = 0; Channel < Channels; ++Channel)
				{
					float& Head = Samples[Frame * Channels + Channel];
					Head = Head * In + Samples[(Frames + Frame) * Channels + Channel] * Out;
				}
			}
			Samples.SetNum(Frames * Channels);
		}
		OutLevels = Measure(Samples, Channels, SampleRate);
		// Peak normalising: the whole render scaled so its loudest sample lands on the target. Nothing can clip at the
		// WAV, however fast a transient got past a limiter, and a set of sounds lands at one level.
		if (Request.NormalizePeakDb < 0.f && !OutLevels.bSilent)
		{
			const float Scale = FMath::Pow(10.f, (FMath::Max(Request.NormalizePeakDb, -60.f) - OutLevels.PeakDb) / 20.f);
			for (float& Sample : Samples)
			{
				Sample *= Scale;
			}
			OutLevels = Measure(Samples, Channels, SampleRate);
		}
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Sound") / FPackageName::GetShortName(Package) + TEXT(".wav"));
		if (!WriteWav(File, Samples, Channels, SampleRate))
		{
			OutError = FString::Printf(TEXT("%s could not be written."), *File);
			return nullptr;
		}
		OutWav = RelativeToProject(File);
		return ImportWave(File, Package, Request.bLoop, OutError);
	}

	FHyperAISoundRenderReport ExecuteRender(const FHyperAISoundRenderRequest& Request, const FString& OperationId)
	{
		FHyperAISoundRenderReport Report;
		Report.OperationId = OperationId;
		FRenderPlan Plan;
		FString Error;
		if (!PlanRender(Request, Plan, Error))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Error;
			return Report;
		}
		const UDecibelSession* Session = LoadObject<UDecibelSession>(nullptr, *Plan.SessionObject);
		FLevels Levels;
		USoundWave* Wave = Session ? RenderOne(*Session, Plan, Plan.OutputPackage, Request, Levels, Report.WavFile, Error) : nullptr;
		if (!Wave)
		{
			Report.Status = TEXT("render_failed");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.SoundWave = Wave->GetPathName();
		Report.Seconds = Plan.DurationSeconds;
		Report.PeakDb = Levels.PeakDb;
		Report.RmsDb = Levels.RmsDb;
		Report.LeadingSilenceSeconds = Levels.LeadingSilenceSeconds;
		Report.bSilent = Levels.bSilent;
		Report.bClipped = Levels.bClipped;
		if (Levels.bSilent)
		{
			const bool bDesigner = Session->Tracks.ContainsByPredicate([](const FDecibelTrackData& Track)
				{ return Track.Devices.ContainsByPredicate([](const FDecibelDevice& Device) { return IsDesigner(Device.DeviceId); }); });
			Report.Issues.Add(bDesigner
				? FString(TEXT("silent: Decibel's Weapon, Impact, Engine and Piano Designers do not sound yet in an offline render of a session ")
					TEXT("built outside Decibel's window (Decibel.Engine.BounceHash renders the same silence). Build this sound from gen.synth, ")
					TEXT("gen.sampler, audio clips and effects for now."))
				: FString(TEXT("silent: nothing rose above -60 dBFS. Check each instrument track has a generator and clips, and each audio track a SoundWave.")));
		}
		if (Levels.bClipped)
		{
			Report.Issues.Add(TEXT("clipped: the mix reaches full scale. Lower track volumes or MasterVolume, or put fx.limit on the master."));
		}
		if (!Levels.bSilent && Levels.LeadingSilenceSeconds > 0.25)
		{
			Report.Issues.Add(FString::Printf(TEXT("late start: %.2f s of silence before the first sound; a one-shot should start at once."), Levels.LeadingSilenceSeconds));
		}
		// Stems: each track alone, in a transient copy with the others MUTED, so its sends and the master chain still apply.
		// Muted, not faded down: a volume lane would ride a zeroed fader straight back up, and a pre-fader send ignores it.
		if (Request.bStems && Session)
		{
			for (int32 Index = 0; Index < Session->Tracks.Num(); ++Index)
			{
				const FDecibelTrackData& Track = Session->Tracks[Index];
				if (Track.TrackType != EDecibelTrackType::Audio && Track.TrackType != EDecibelTrackType::Instrument)
				{
					continue;
				}
				UDecibelSession* Solo = DuplicateObject<UDecibelSession>(Session, GetTransientPackage());
				for (int32 Other = 0; Other < Solo->Tracks.Num(); ++Other)
				{
					if (Other != Index && Solo->Tracks[Other].TrackType != EDecibelTrackType::Fx)
					{
						Solo->Tracks[Other].bMuted = true;
					}
				}
				FString TrackName = FPaths::MakeValidFileName(Track.Name.IsEmpty() ? FString::Printf(TEXT("Track%d"), Index) : Track.Name).Replace(TEXT(" "), TEXT(""));
				FLevels StemLevels;
				FString StemWav;
				if (USoundWave* Stem = RenderOne(*Solo, Plan, Plan.OutputPackage + TEXT("_") + TrackName, Request, StemLevels, StemWav, Error))
				{
					Report.Stems.Add(Stem->GetPathName());
				}
				else
				{
					Report.Issues.Add(FString::Printf(TEXT("stem %s: %s"), *TrackName, *Error));
				}
			}
		}
		if (Request.bMetaSound)
		{
			Report.MetaSound = WrapInMetaSound(Wave, FMath::Clamp(Request.Channels, 1, 2), Request.bLoop, Error);
			if (Report.MetaSound.IsEmpty())
			{
				Report.Issues.Add(TEXT("metasound: ") + Error);
			}
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("Rendered %.2f s into %s: peak %.1f dBFS, average %.1f dBFS%s."), Report.Seconds, *Report.SoundWave,
			Report.PeakDb, Report.RmsDb, Report.Issues.IsEmpty() ? TEXT(", no issues") : *FString::Printf(TEXT(", %d issue(s)"), Report.Issues.Num()));
		return Report;
	}

	FString DescribeRenderOutcome(const FHyperAISoundRenderReport& Report)
	{
		FString Text = Report.Diagnostic;
		for (const FString& Issue : Report.Issues)
		{
			Text += TEXT("\n- ") + Issue;
		}
		if (!Report.MetaSound.IsEmpty()) Text += TEXT("\nMetaSound: ") + Report.MetaSound;
		if (!Report.Stems.IsEmpty()) Text += TEXT("\nStems: ") + FString::Join(Report.Stems, TEXT(", "));
		return Text;
	}
}

FHyperAISoundInspectReport FHyperAIStudioDecibelContracts::Inspect(const FHyperAISoundInspectRequest& Request)
{
	using namespace HyperAIStudio::Decibel::Private;
	FHyperAISoundInspectReport Report;
	if (Request.bCatalog)
	{
		for (const FDecibelDeviceDef& Def : DecibelDevices::Catalog())
		{
			FHyperAISoundDeviceInfo& Info = Report.Catalog.AddDefaulted_GetRef();
			Info.Id = Def.Id;
			Info.Name = Def.Name;
			Info.Kind = KindName(Def.Kind);
			Info.Category = Def.Category ? Def.Category : TEXT("");
			Info.Description = Def.Desc ? Def.Desc : TEXT("");
		}
	}
	if (!Request.DeviceId.IsEmpty())
	{
		if (!DecibelDevices::Find(Request.DeviceId))
		{
			Report.Diagnostic = FString::Printf(TEXT("Decibel has no device '%s': bCatalog lists them."), *Request.DeviceId);
			return Report;
		}
		for (const FDecibelDeviceParam& Param : DecibelDevices::MakeDevice(Request.DeviceId).Params)
		{
			FHyperAISoundParamInfo& Info = Report.Params.AddDefaulted_GetRef();
			Info.Id = Param.Id.ToString();
			Info.Label = Param.Label;
			Info.Description = Param.Desc;
			Info.Value = Param.Value;
			Info.Min = Param.MinValue;
			Info.Max = Param.MaxValue;
			Info.Default = Param.DefaultValue;
		}
	}
	if (Request.bPresets)
	{
		TArray<FString> Folders;
		if (Request.DeviceId.IsEmpty())
		{
			IFileManager::Get().FindFiles(Folders, *(PresetFolder() / TEXT("*")), /*Files=*/false, /*Directories=*/true);
		}
		else
		{
			Folders.Add(Request.DeviceId);
		}
		for (const FString& Folder : Folders)
		{
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *(PresetFolder() / Folder / TEXT("*.dbpreset")), true, false);
			Files.Sort();
			for (const FString& File : Files)
			{
				Report.Presets.Add(Folder / FPaths::GetBaseFilename(File));
			}
		}
	}
	if (Request.bSessions)
	{
		TArray<FAssetData> Assets;
		FAssetRegistryModule::GetRegistry().GetAssetsByClass(UDecibelSession::StaticClass()->GetClassPathName(), Assets);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.PackageName.ToString().StartsWith(TEXT("/Game/")))
			{
				Report.Sessions.Add(Asset.GetObjectPathString());
			}
		}
		Report.Sessions.Sort();
	}
	if (!Request.SessionPath.IsEmpty())
	{
		Report.SessionPath = ObjectPath(Request.SessionPath);
		const UDecibelSession* Session = LoadObject<UDecibelSession>(nullptr, *Report.SessionPath);
		if (!Session)
		{
			Report.Diagnostic = FString::Printf(TEXT("%s is not a Decibel session."), *Report.SessionPath);
			return Report;
		}
		Report.Bpm = Session->Bpm;
		Report.TimeSigNumerator = Session->TimeSigNumerator;
		Report.TimeSigDenominator = Session->TimeSigDenominator;
		Report.LengthBeats = LastClipEndBeat(*Session);
		Report.LengthSeconds = Report.LengthBeats * SecondsPerBeat(*Session);
		Report.bOneShot = Session->bOneShot;
		Report.bLoop = Session->bLoopActive;
		Report.LoopStartBeat = Session->LoopStartBeat;
		Report.LoopEndBeat = Session->LoopEndBeat;
		Report.Tracks = DescribeTracks(*Session);
		for (const FDecibelDevice& Device : Session->MasterChainRow().Devices)
		{
			Report.MasterDevices.Add(Device.DeviceId);
		}
	}
	Report.bOk = true;
	return Report;
}

FHyperAISoundPlanReport FHyperAIStudioDecibelContracts::ApplyPlan(const FHyperAISoundPlanRequest& InRequest)
{
	using namespace HyperAIStudio::Decibel::Private;
	FHyperAISoundPlanRequest Request = InRequest;
	FHyperAISoundPlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty() ? NewOperationId(TEXT("sound-plan")) : Request.OperationId;
	Report.SessionPath = ObjectPath(Request.SessionPath);
	FString Error;
	const FString Package = PackageOf(Request.SessionPath);
	if (!IsProjectPackage(Package, Error) || Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error.IsEmpty() ? FString::Printf(TEXT("A plan has 1 to %d operations."), MaxOperations) : Error;
		return Report;
	}
	UDecibelSession* Existing = LoadObject<UDecibelSession>(nullptr, *Report.SessionPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (Request.bCreate == (Existing != nullptr))
	{
		Report.Status = Existing ? TEXT("session_exists") : TEXT("session_not_found");
		Report.Diagnostic = Existing ? FString::Printf(TEXT("%s exists already: edit it with bCreate false."), *Report.SessionPath)
			: FString::Printf(TEXT("%s is not a Decibel session: make it with bCreate true."), *Report.SessionPath);
		return Report;
	}
	// The plan runs on a transient copy first, so a failing op never leaves a half-edited session.
	UDecibelSession* Trial = Existing ? DuplicateObject<UDecibelSession>(Existing, GetTransientPackage())
		: NewObject<UDecibelSession>(GetTransientPackage());
	if (!Existing)
	{
		Trial->EnsureHostChainDefaults();
	}
	if (!ApplyAll(*Trial, Request.Operations, /*Library=*/nullptr, Report.Applied, Report.Notes, Error))
	{
		Report.Status = TEXT("operation_failed");
		Report.Diagnostic = Error + TEXT(" Nothing was changed.");
		return Report;
	}
	Report.Tracks = DescribeTracks(*Trial);
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("All %d operation(s) apply; nothing was changed."), Report.Applied);
		return Report;
	}
	Report.Applied = 0;
	Report.Notes.Reset();
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = PlanToolName;
		Summary.VariantId = TEXT("sound.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Report.SessionPath;
		Summary.Effects.Add(FString::Printf(TEXT("%s the Decibel session %s: %d operation(s), one undo step"),
			Request.bCreate ? TEXT("Make") : TEXT("Edit"), *Report.SessionPath, Request.Operations.Num()));
		for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
		{
			const FHyperAISoundOp& Op = Request.Operations[Index];
			Summary.Effects.Add(FString::Printf(TEXT("| %s %s%s"), *Op.Kind, *Op.Track, Op.DeviceId.IsEmpty() ? TEXT("") : *(TEXT(" ") + Op.DeviceId)));
		}
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				const FHyperAISoundPlanReport Done = Execute(Request, OperationId);
				RecordOutcome(PlanToolName, OperationId, Done.SessionPath, Done.bOk, Done.Diagnostic);
			}, Error))
		{
			Report.Status = TEXT("approval_not_queued");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
		return Report;
	}
	return Execute(Request, Report.OperationId);
}

FHyperAISoundRenderReport FHyperAIStudioDecibelContracts::Render(const FHyperAISoundRenderRequest& Request)
{
	using namespace HyperAIStudio::Decibel::Private;
	FHyperAISoundRenderReport Report;
	Report.OperationId = Request.OperationId.IsEmpty() ? NewOperationId(TEXT("sound-render")) : Request.OperationId;
	FRenderPlan Plan;
	FString Error;
	if (!PlanRender(Request, Plan, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	Report.Seconds = Plan.DurationSeconds;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("Would render %.2f s from %.2f s into %s."), Plan.DurationSeconds, Plan.StartSeconds, *Plan.OutputPackage);
		return Report;
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = RenderToolName;
		Summary.VariantId = TEXT("sound.render.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Plan.OutputPackage;
		Summary.Effects.Add(FString::Printf(TEXT("Render %.1f s of %s into %s%s%s"), Plan.DurationSeconds, *Plan.SessionObject, *Plan.OutputPackage,
			Request.bStems ? TEXT(", with stems") : TEXT(""), Request.bMetaSound ? TEXT(", and a MetaSound Source") : TEXT("")));
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				const FHyperAISoundRenderReport Done = ExecuteRender(Request, OperationId);
				RecordOutcome(RenderToolName, OperationId, Done.SoundWave, Done.bOk, DescribeRenderOutcome(Done));
			}, Error))
		{
			Report.Status = TEXT("approval_not_queued");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
		return Report;
	}
	return ExecuteRender(Request, Report.OperationId);
}

#else

namespace
{
	const TCHAR* const NotInstalled = TEXT("Decibel is not installed in this project, so HyperAI Studio's sound tools are off.");
}

FHyperAISoundInspectReport FHyperAIStudioDecibelContracts::Inspect(const FHyperAISoundInspectRequest&)
{
	FHyperAISoundInspectReport Report;
	Report.Diagnostic = NotInstalled;
	return Report;
}

FHyperAISoundPlanReport FHyperAIStudioDecibelContracts::ApplyPlan(const FHyperAISoundPlanRequest&)
{
	FHyperAISoundPlanReport Report;
	Report.Status = TEXT("decibel_not_installed");
	Report.Diagnostic = NotInstalled;
	return Report;
}

FHyperAISoundRenderReport FHyperAIStudioDecibelContracts::Render(const FHyperAISoundRenderRequest&)
{
	FHyperAISoundRenderReport Report;
	Report.Status = TEXT("decibel_not_installed");
	Report.Diagnostic = NotInstalled;
	return Report;
}

#endif

FHyperAISoundInspectReport UHyperAIStudioDecibelToolset::hyper_sound_inspect(const FHyperAISoundInspectRequest& Request)
{
	return FHyperAIStudioDecibelContracts::Inspect(Request);
}

FHyperAISoundPlanReport UHyperAIStudioDecibelToolset::hyper_sound_apply_plan(const FHyperAISoundPlanRequest& Request)
{
	return FHyperAIStudioDecibelContracts::ApplyPlan(Request);
}

FHyperAISoundRenderReport UHyperAIStudioDecibelToolset::hyper_sound_render(const FHyperAISoundRenderRequest& Request)
{
	return FHyperAIStudioDecibelContracts::Render(Request);
}

void FHyperAIStudioDecibelRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioDecibelRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioDecibelRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioDecibelToolset::StaticClass(),
			FHyperAIStudioDecibelContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioDecibel, Warning, TEXT("Could not unregister the sound toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioDecibelRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioDecibelContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioDecibel, Verbose, TEXT("The sound design source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioDecibelToolset::StaticClass(),
		FHyperAIStudioDecibelContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioDecibel, Error, TEXT("Sound design cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
