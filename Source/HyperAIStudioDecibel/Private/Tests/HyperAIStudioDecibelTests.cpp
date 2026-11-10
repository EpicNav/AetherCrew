// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS && WITH_DECIBEL

#include "HAL/FileManager.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioDecibelToolset.h"
#include "HyperAIStudioSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Sound/SoundWave.h"

namespace HyperAIStudio::DecibelTests
{
	FString NewSessionPath(const TCHAR* Name)
	{
		return FString::Printf(TEXT("/Game/HyperAITests/Sound/DS_%s_%s"), Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	FHyperAISoundOp Op(const TCHAR* Kind, const TCHAR* Track = TEXT(""), const TCHAR* DeviceId = TEXT(""))
	{
		FHyperAISoundOp Out;
		Out.Kind = Kind;
		Out.Track = Track;
		Out.DeviceId = DeviceId;
		return Out;
	}

	/** Deletes what a test made under /Game/HyperAITests/Sound and the WAVs it rendered. */
	void Clean(const TArray<FString>& ObjectPaths)
	{
		TArray<UObject*> Objects;
		for (const FString& Path : ObjectPaths)
		{
			if (UObject* Object = Path.IsEmpty() ? nullptr : LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				Objects.Add(Object);
			}
		}
		if (!Objects.IsEmpty())
		{
			ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
		}
	}

	/** A sound file on disk as a sound library holds one: a second of 16-bit mono at 48 kHz, a tone gliding From to To Hz. */
	FString WriteSweep(const FString& Name, const double From, const double To)
	{
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / Name + TEXT(".wav"));
		constexpr int32 Rate = 48000;
		TArray<uint8> Bytes;
		auto U32 = [&Bytes](const uint32 V) { Bytes.Append(reinterpret_cast<const uint8*>(&V), 4); };
		auto U16 = [&Bytes](const uint16 V) { Bytes.Append(reinterpret_cast<const uint8*>(&V), 2); };
		Bytes.Append(reinterpret_cast<const uint8*>("RIFF"), 4); U32(36 + Rate * 2);
		Bytes.Append(reinterpret_cast<const uint8*>("WAVEfmt "), 8); U32(16); U16(1); U16(1); U32(Rate); U32(Rate * 2); U16(2); U16(16);
		Bytes.Append(reinterpret_cast<const uint8*>("data"), 4); U32(Rate * 2);
		double Phase = 0.0;
		for (int32 Index = 0; Index < Rate; ++Index)
		{
			U16(static_cast<uint16>(static_cast<int16>(12000.0 * FMath::Sin(Phase))));
			Phase += 2.0 * PI * (From + (To - From) * Index / Rate) / Rate;
		}
		FFileHelper::SaveArrayToFile(Bytes, *File);
		return File;
	}

	/** Rising zero crossings per second between two times: a plain tone's frequency. */
	double Hz(const TArray<int16>& Pcm, const double From, const double To)
	{
		const int32 A = FMath::Clamp(FMath::RoundToInt(From * 48000), 1, Pcm.Num());
		const int32 B = FMath::Clamp(FMath::RoundToInt(To * 48000), A, Pcm.Num());
		int32 Rises = 0;
		for (int32 Index = A; Index < B; ++Index)
		{
			Rises += (Pcm[Index - 1] < 0 && Pcm[Index] >= 0) ? 1 : 0;
		}
		return B > A ? Rises / ((B - A) / 48000.0) : 0.0;
	}

	/** The level between two times, in dBFS. */
	double RmsDb(const TArray<int16>& Pcm, const double From, const double To)
	{
		const int32 A = FMath::Clamp(FMath::RoundToInt(From * 48000), 0, Pcm.Num());
		const int32 B = FMath::Clamp(FMath::RoundToInt(To * 48000), A, Pcm.Num());
		double Sum = 0.0;
		for (int32 Index = A; Index < B; ++Index)
		{
			Sum += FMath::Square(Pcm[Index] / 32768.0);
		}
		return B > A ? 10.0 * FMath::LogX(10.0, Sum / (B - A) + 1e-12) : -120.0;
	}

	FHyperAISoundAutoPoint AutoPoint(const double Beat, const float Value)
	{
		FHyperAISoundAutoPoint Out;
		Out.Beat = Beat;
		Out.Value = Value;
		return Out;
	}

	FHyperAISoundParamValue ParamValue(const TCHAR* Id, const float Value)
	{
		FHyperAISoundParamValue Out;
		Out.Id = Id;
		Out.Value = Value;
		return Out;
	}

	/** The 16-bit samples of a render's WAV (WriteWav's 44-byte header, then PCM), deleting the file. */
	TArray<int16> TakeWav(const FString& WavFile)
	{
		const FString File = FPaths::ProjectDir() / WavFile;
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *File);
		IFileManager::Get().Delete(*File);
		TArray<int16> Samples;
		if (Bytes.Num() > 44)
		{
			Samples.Append(reinterpret_cast<const int16*>(Bytes.GetData() + 44), (Bytes.Num() - 44) / 2);
		}
		return Samples;
	}

	struct FNoApproval
	{
		bool bWas = GetDefault<UHyperAIStudioSettings>()->bRequireApprovalForAgentEdits;
		FNoApproval(const bool bRequire = false) { GetMutableDefault<UHyperAIStudioSettings>()->bRequireApprovalForAgentEdits = bRequire; }
		~FNoApproval() { GetMutableDefault<UHyperAIStudioSettings>()->bRequireApprovalForAgentEdits = bWas; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSoundWeaponShotTest,
	"HyperAIStudio.Sound.WeaponSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSoundWeaponShotTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DecibelTests;
	const FNoApproval NoApproval;
	using C = FHyperAIStudioDecibelContracts;

	FHyperAISoundInspectRequest Look;
	Look.bCatalog = true;
	Look.DeviceId = TEXT("gen.weapon");
	Look.bPresets = true;
	const FHyperAISoundInspectReport Catalog = C::Inspect(Look);
	TestTrue(TEXT("The catalog lists the Weapon Designer as a generator"), Catalog.Catalog.ContainsByPredicate([](const FHyperAISoundDeviceInfo& D)
		{ return D.Id == TEXT("gen.weapon") && D.Kind == TEXT("generator"); }));
	TestTrue(TEXT("with its parameters and ranges"), Catalog.Params.ContainsByPredicate([](const FHyperAISoundParamInfo& P) { return P.Id == TEXT("wprpm") && P.Max > P.Min; }));
	if (!TestTrue(TEXT("and its presets"), !Catalog.Presets.IsEmpty() && Catalog.Presets[0].StartsWith(TEXT("gen.weapon/"))))
	{
		return false;
	}
	const FString Preset = Catalog.Presets[0].RightChop(FString(TEXT("gen.weapon/")).Len());
	const FHyperAISoundDeviceInfo* Limiter = Catalog.Catalog.FindByPredicate([](const FHyperAISoundDeviceInfo& D)
		{ return D.Kind == TEXT("audio_effect") && D.Id.Contains(TEXT("limit")); });

	FHyperAISoundPlanRequest Plan;
	Plan.SessionPath = NewSessionPath(TEXT("Shot"));
	Plan.bCreate = true;
	FHyperAISoundOp Session = Op(TEXT("session.set"));
	Session.OneShot = 1;
	FHyperAISoundOp Track = Op(TEXT("track.add"));
	Track.Name = TEXT("Rifle");
	Track.Type = TEXT("instrument");
	FHyperAISoundOp Gun = Op(TEXT("device.add"), TEXT("Rifle"), TEXT("gen.weapon"));
	Gun.Preset = Preset;
	FHyperAISoundOp Fire = Op(TEXT("clip.trigger"), TEXT("Rifle"));
	Plan.Operations = {Session, Track, Gun, Fire};
	if (Limiter)
	{
		Plan.Operations.Add(Op(TEXT("device.add"), TEXT("master"), *Limiter->Id));
	}
	const FHyperAISoundPlanReport Built = C::ApplyPlan(Plan);
	TArray<FString> Made = {Built.SessionPath};
	ON_SCOPE_EXIT { Clean(Made); };
	if (!TestTrue(TEXT("A one-shot rifle session is built and saved: ") + Built.Diagnostic, Built.bOk && Built.Status == TEXT("completed")))
	{
		return false;
	}
	TestTrue(TEXT("with the Weapon Designer on its track and a trigger clip"), Built.Tracks.Num() == 1 && Built.Tracks[0].Devices.Contains(TEXT("gen.weapon"))
		&& Built.Tracks[0].Clips.Num() == 1 && Built.Tracks[0].Clips[0].Kind == TEXT("trigger"));

	FHyperAISoundInspectRequest Back;
	Back.SessionPath = Plan.SessionPath;
	TestTrue(TEXT("and reads back as a one-shot"), FHyperAIStudioDecibelContracts::Inspect(Back).bOneShot);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSoundMusicTest,
	"HyperAIStudio.Sound.MusicAndChecks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSoundMusicTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DecibelTests;
	const FNoApproval NoApproval;
	using C = FHyperAIStudioDecibelContracts;

	// A failing plan changes nothing: it runs on a copy first.
	FHyperAISoundPlanRequest Bad;
	Bad.SessionPath = NewSessionPath(TEXT("Bad"));
	Bad.bCreate = true;
	FHyperAISoundOp AudioTrack = Op(TEXT("track.add"));
	AudioTrack.Type = TEXT("audio");
	FHyperAISoundOp Notes = Op(TEXT("clip.midi"), TEXT("0"));
	Notes.Notes.AddDefaulted();
	Bad.Operations = {AudioTrack, Notes};
	const FHyperAISoundPlanReport Refused = C::ApplyPlan(Bad);
	TestTrue(TEXT("MIDI on an audio track is refused, naming the op"), !Refused.bOk && Refused.Status == TEXT("operation_failed")
		&& Refused.Diagnostic.Contains(TEXT("Operation 1 (clip.midi)")) && Refused.Diagnostic.Contains(TEXT("Nothing was changed")));
	TestTrue(TEXT("and the session was never made"), LoadObject<UObject>(nullptr, *(Bad.SessionPath + TEXT(".") + FPaths::GetBaseFilename(Bad.SessionPath)), nullptr, LOAD_NoWarn | LOAD_Quiet) == nullptr);

	// Four bars of a C major arpeggio on the synth, with a parameter out of range.
	FHyperAISoundInspectRequest Look;
	Look.DeviceId = TEXT("gen.synth");
	const FHyperAISoundInspectReport Synth = C::Inspect(Look);
	if (!TestTrue(TEXT("The synth has parameters"), Synth.bOk && !Synth.Params.IsEmpty()))
	{
		return false;
	}
	FHyperAISoundPlanRequest Plan;
	Plan.SessionPath = NewSessionPath(TEXT("Arp"));
	Plan.bCreate = true;
	FHyperAISoundOp Tempo = Op(TEXT("session.set"));
	Tempo.Bpm = 96;
	FHyperAISoundOp Lead = Op(TEXT("track.add"));
	Lead.Name = TEXT("Lead");
	Lead.Type = TEXT("instrument");
	FHyperAISoundOp Device = Op(TEXT("device.add"), TEXT("Lead"), TEXT("gen.synth"));
	FHyperAISoundParamValue Wild;
	Wild.Id = Synth.Params[0].Id;
	Wild.Value = Synth.Params[0].Max + 1000.f;
	Device.Params = {Wild};
	FHyperAISoundOp Arp = Op(TEXT("clip.midi"), TEXT("Lead"));
	const int32 Pitches[] = {60, 64, 67, 72};
	for (int32 Bar = 0; Bar < 4; ++Bar)
	{
		for (int32 Step = 0; Step < 4; ++Step)
		{
			FHyperAISoundNote& Note = Arp.Notes.AddDefaulted_GetRef();
			Note.Pitch = Pitches[Step];
			Note.StartBeat = Bar * 4 + Step;
			Note.LengthBeats = 0.9;
		}
	}
	Plan.Operations = {Tempo, Lead, Device, Arp};
	FHyperAISoundPlanRequest DryRun = Plan;
	DryRun.bDryRun = true;
	const FHyperAISoundPlanReport Checked = C::ApplyPlan(DryRun);
	TestTrue(TEXT("A dry run checks every op and changes nothing"), Checked.bOk && Checked.Status == TEXT("checked") && Checked.Applied == 4);
	const FHyperAISoundPlanReport Built = C::ApplyPlan(Plan);
	TArray<FString> Made = {Built.SessionPath};
	ON_SCOPE_EXIT { Clean(Made); };
	if (!TestTrue(TEXT("The arpeggio session is built: ") + Built.Diagnostic, Built.bOk))
	{
		return false;
	}
	TestTrue(TEXT("and the value outside its range was fitted, and said so"), Built.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("outside")); }));
	FHyperAISoundInspectRequest Session;
	Session.SessionPath = Plan.SessionPath;
	const FHyperAISoundInspectReport Summary = C::Inspect(Session);
	TestTrue(TEXT("Inspect reads it back: tempo, one track of 16 notes over 16 beats"), Summary.Bpm == 96 && Summary.Tracks.Num() == 1
		&& Summary.Tracks[0].Clips.Num() == 1 && Summary.Tracks[0].Clips[0].Notes == 16 && FMath::IsNearlyEqual(Summary.LengthBeats, 16.0, 0.01));

	FHyperAISoundRenderRequest Render;
	Render.SessionPath = Plan.SessionPath;
	Render.bStems = true;
	Render.bLoop = true;
	const FHyperAISoundRenderReport Heard = C::Render(Render);
	Made.Add(Heard.SoundWave);
	Made.Append(Heard.Stems);
	AddInfo(FString::Printf(TEXT("Arpeggio: %.2f s, peak %.1f dBFS, average %.1f dBFS; %s"), Heard.Seconds, Heard.PeakDb, Heard.RmsDb, *FString::Join(Heard.Issues, TEXT(" | "))));
	TestTrue(TEXT("It renders its 10 s plus the tail, and sounds: ") + Heard.Diagnostic, Heard.bOk && !Heard.bSilent
		&& FMath::IsNearlyEqual(Heard.Seconds, 16.0 * 60.0 / 96.0 + 1.0, 0.01));
	TestTrue(TEXT("with a stem for its one track, and a looping SoundWave"), Heard.Stems.Num() == 1
		&& LoadObject<USoundWave>(nullptr, *Heard.SoundWave) && LoadObject<USoundWave>(nullptr, *Heard.SoundWave)->bLooping);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSoundApprovalTest,
	"HyperAIStudio.Sound.WaitsForApproval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSoundApprovalTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DecibelTests;
	const FNoApproval Required(/*bRequire=*/true);
	FHyperAISoundPlanRequest Plan;
	Plan.SessionPath = NewSessionPath(TEXT("Approve"));
	Plan.bCreate = true;
	FHyperAISoundOp Track = Op(TEXT("track.add"));
	Track.Type = TEXT("instrument");
	Plan.Operations = {Track};
	const FHyperAISoundPlanReport Asked = FHyperAIStudioDecibelContracts::ApplyPlan(Plan);
	const FString Object = Plan.SessionPath + TEXT(".") + FPaths::GetBaseFilename(Plan.SessionPath);
	TArray<FString> Made = {Object};
	ON_SCOPE_EXIT { Clean(Made); };
	TestEqual(TEXT("With approval required, the edit waits"), Asked.Status, FString(TEXT("awaiting_user_approval")));
	TestNull(TEXT("and nothing is made before it"), LoadObject<UObject>(nullptr, *Object, nullptr, LOAD_NoWarn | LOAD_Quiet));
	FString Error;
	TestTrue(TEXT("Approved: ") + Error, FHyperAIStudioApprovalGate::Approve(Asked.OperationId, Error));
	TestNotNull(TEXT("it is made"), LoadObject<UObject>(nullptr, *Object, nullptr, LOAD_NoWarn | LOAD_Quiet));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSoundSynthEffectTest,
	"HyperAIStudio.Sound.SynthEffect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSoundSynthEffectTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DecibelTests;
	const FNoApproval NoApproval;
	using C = FHyperAIStudioDecibelContracts;
	// A one-shot zap: the synth fired once, through a delay and a reverb, with a limiter on the master.
	FHyperAISoundPlanRequest Plan;
	Plan.SessionPath = NewSessionPath(TEXT("Zap"));
	Plan.bCreate = true;
	FHyperAISoundOp Session = Op(TEXT("session.set"));
	Session.OneShot = 1;
	FHyperAISoundOp Track = Op(TEXT("track.add"));
	Track.Name = TEXT("Zap");
	Track.Type = TEXT("instrument");
	FHyperAISoundOp Fire = Op(TEXT("clip.trigger"), TEXT("Zap"));
	Fire.LengthBeats = 0.5;
	Plan.Operations = {Session, Track, Op(TEXT("device.add"), TEXT("Zap"), TEXT("gen.synth")), Op(TEXT("device.add"), TEXT("Zap"), TEXT("fx.delay")),
		Op(TEXT("device.add"), TEXT("Zap"), TEXT("fx.verb")), Op(TEXT("device.add"), TEXT("master"), TEXT("fx.limit")), Fire};
	const FHyperAISoundPlanReport Built = C::ApplyPlan(Plan);
	TArray<FString> Made = {Built.SessionPath};
	ON_SCOPE_EXIT { Clean(Made); };
	if (!TestTrue(TEXT("The zap session is built: ") + Built.Diagnostic, Built.bOk && Built.Tracks.Num() == 1 && Built.Tracks[0].Devices.Num() == 3))
	{
		return false;
	}
	FHyperAISoundRenderRequest Render;
	Render.SessionPath = Plan.SessionPath;
	Render.bMetaSound = true;
	const FHyperAISoundRenderReport Heard = C::Render(Render);
	Made.Append({Heard.SoundWave, Heard.MetaSound});
	AddInfo(FString::Printf(TEXT("Zap: %.2f s, peak %.1f dBFS, average %.1f dBFS, lead %.3f s; %s"), Heard.Seconds, Heard.PeakDb, Heard.RmsDb,
		Heard.LeadingSilenceSeconds, *FString::Join(Heard.Issues, TEXT(" | "))));
	if (!TestTrue(TEXT("It renders: ") + Heard.Diagnostic, Heard.bOk && Heard.Status == TEXT("completed")))
	{
		return false;
	}
	TestTrue(TEXT("into a SoundWave that sounds, below full scale"), LoadObject<USoundWave>(nullptr, *Heard.SoundWave) != nullptr && !Heard.bSilent && !Heard.bClipped);
	TestTrue(TEXT("and starts at once"), Heard.LeadingSilenceSeconds < 0.25);
	TestTrue(TEXT("wrapped in a MetaSound Source"), !Heard.MetaSound.IsEmpty() && LoadObject<UObject>(nullptr, *Heard.MetaSound) != nullptr);
	TestTrue(TEXT("with the WAV it came from kept under Saved"), FPaths::FileExists(FPaths::ProjectDir() / Heard.WavFile));
	IFileManager::Get().Delete(*(FPaths::ProjectDir() / Heard.WavFile));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSoundLibraryFileTest,
	"HyperAIStudio.Sound.LibraryFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSoundLibraryFileTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DecibelTests;
	const FNoApproval NoApproval;
	using C = FHyperAIStudioDecibelContracts;
	// A sound library's WAV on disk: one second of a 220 Hz tone, 48 kHz mono.
	const FString Name = TEXT("HyperAI_Tone_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString File = WriteSweep(Name, 220.0, 220.0);
	const FString LibraryObject = TEXT("/Game/Audio/Library/") + Name + TEXT(".") + Name;

	FHyperAISoundPlanRequest Plan;
	Plan.SessionPath = NewSessionPath(TEXT("Library"));
	Plan.bCreate = true;
	FHyperAISoundOp Track = Op(TEXT("track.add"));
	Track.Name = TEXT("Bed");
	Track.Type = TEXT("audio");
	FHyperAISoundOp Clip = Op(TEXT("clip.audio"), TEXT("Bed"));
	Clip.Sound = File;
	Plan.Operations = {Track, Clip};
	TArray<FString> Made;
	ON_SCOPE_EXIT
	{
		Clean(Made);
		IFileManager::Get().Delete(*File);
	};

	FHyperAISoundPlanRequest DryRun = Plan;
	DryRun.bDryRun = true;
	TestTrue(TEXT("A dry run takes a sound file on disk"), C::ApplyPlan(DryRun).Status == TEXT("checked"));
	TestNull(TEXT("and imports nothing"), LoadObject<USoundWave>(nullptr, *LibraryObject, nullptr, LOAD_NoWarn | LOAD_Quiet));

	const FHyperAISoundPlanReport Built = C::ApplyPlan(Plan);
	Made.Append({Built.SessionPath, LibraryObject});
	if (!TestTrue(TEXT("For real, the file is imported and played: ") + Built.Diagnostic, Built.bOk))
	{
		return false;
	}
	TestNotNull(TEXT("as a SoundWave under /Game/Audio/Library"), LoadObject<USoundWave>(nullptr, *LibraryObject, nullptr, LOAD_NoWarn | LOAD_Quiet));
	TestTrue(TEXT("which the clip plays, a second long"), Built.Tracks.Num() == 1 && Built.Tracks[0].Clips.Num() == 1
		&& Built.Tracks[0].Clips[0].Sound == LibraryObject && FMath::IsNearlyEqual(Built.Tracks[0].Clips[0].LengthBeats, 2.0, 0.05));

	FHyperAISoundRenderRequest Render;
	Render.SessionPath = Plan.SessionPath;
	const FHyperAISoundRenderReport Heard = C::Render(Render);
	Made.Add(Heard.SoundWave);
	TestTrue(TEXT("and the render hears it: ") + Heard.Diagnostic, Heard.bOk && !Heard.bSilent);

	// A seamless loop. Cut after 110.24 cycles of the tone, a plain render jumps at the join; the crossfaded one does not.
	FHyperAISoundRenderRequest Loop;
	Loop.SessionPath = Plan.SessionPath;
	Loop.OutputPath = Plan.SessionPath + TEXT("_Loop");
	Loop.DurationSeconds = 0.5011;
	Loop.LoopCrossfadeSeconds = 0.25;
	Loop.bLoop = true;
	Loop.Channels = 1;
	const FHyperAISoundRenderReport Looped = C::Render(Loop);
	Made.Add(Looped.SoundWave);
	const TArray<int16> Pcm = TakeWav(Looped.WavFile);
	int32 Step = 0;
	for (int32 Index = 1; Index < Pcm.Num(); ++Index)
	{
		Step = FMath::Max(Step, FMath::Abs(Pcm[Index] - Pcm[Index - 1]));
	}
	const int32 Join = Pcm.IsEmpty() ? -1 : FMath::Abs(Pcm[0] - Pcm.Last());
	TestTrue(TEXT("A seamless loop lasts the loop: ") + Looped.Diagnostic, Looped.bOk && Pcm.Num() == FMath::RoundToInt(0.5011 * 48000));
	TestTrue(FString::Printf(TEXT("and its last sample leads into its first (join %d, largest step %d)"), Join, Step), !Pcm.IsEmpty() && Join <= Step * 3 / 2);

	// A tail loop, for music: the 0.1 s past the loop point lands on its start, which keeps its full level.
	FHyperAISoundRenderRequest Plain;
	Plain.SessionPath = Plan.SessionPath;
	Plain.OutputPath = Plan.SessionPath + TEXT("_Plain");
	Plain.DurationSeconds = 0.5;
	Plain.Channels = 1;
	const FHyperAISoundRenderReport PlainHeard = C::Render(Plain);
	Made.Add(PlainHeard.SoundWave);
	const TArray<int16> Whole = TakeWav(PlainHeard.WavFile);
	FHyperAISoundRenderRequest Tail = Plain;
	Tail.OutputPath = Plan.SessionPath + TEXT("_Tail");
	Tail.DurationSeconds = 0.4;
	Tail.LoopTailSeconds = 0.1;
	const FHyperAISoundRenderReport TailHeard = C::Render(Tail);
	Made.Add(TailHeard.SoundWave);
	const TArray<int16> Wrapped = TakeWav(TailHeard.WavFile);
	int32 Worst = Wrapped.Num() == 19200 && Whole.Num() == 24000 ? 0 : MAX_int32;
	for (int32 Index = 0; Worst != MAX_int32 && Index < Wrapped.Num(); ++Index)
	{
		Worst = FMath::Max(Worst, FMath::Abs(Wrapped[Index] - (Whole[Index] + (Index < 4800 ? Whole[19200 + Index] : 0))));
	}
	TestTrue(FString::Printf(TEXT("A tail loop adds what rings past its end onto its start (largest difference %d)"), Worst), Worst <= 2);
	FHyperAISoundRenderRequest Both = Tail;
	Both.LoopCrossfadeSeconds = 0.1;
	TestFalse(TEXT("and a loop asking for both is refused"), C::Render(Both).bOk);

	// The same file as an instrument: a looping sampler rooted at A3, where the tone was recorded, holding an A4 for 2 s.
	FHyperAISoundPlanRequest Keys;
	Keys.SessionPath = NewSessionPath(TEXT("Sampler"));
	Keys.bCreate = true;
	FHyperAISoundOp Instrument = Op(TEXT("track.add"));
	Instrument.Name = TEXT("Tone");
	Instrument.Type = TEXT("instrument");
	FHyperAISoundOp Sampler = Op(TEXT("device.add"), TEXT("Tone"), TEXT("gen.sampler"));
	Sampler.Sound = File;
	Sampler.SamplerMode = TEXT("loop");
	FHyperAISoundParamValue Root;
	Root.Id = TEXT("root");
	Root.Value = 57.f;
	Sampler.Params = {Root};
	FHyperAISoundOp Held = Op(TEXT("clip.midi"), TEXT("Tone"));
	FHyperAISoundNote& A4 = Held.Notes.AddDefaulted_GetRef();
	A4.Pitch = 69;
	A4.StartBeat = 0.0;
	A4.LengthBeats = 4.0; // 2 s at 120 BPM; an octave up, the 1 s sample lasts 0.5 s, so the rest is the loop
	Keys.Operations = {Instrument, Sampler, Held};
	const FHyperAISoundPlanReport Played = C::ApplyPlan(Keys);
	Made.Add(Played.SessionPath);
	if (!TestTrue(TEXT("A sampler takes the file as its sample: ") + Played.Diagnostic, Played.bOk))
	{
		return false;
	}
	FHyperAISoundRenderRequest Hear;
	Hear.SessionPath = Keys.SessionPath;
	Hear.Channels = 1;
	const FHyperAISoundRenderReport Heard2 = C::Render(Hear);
	Made.Add(Heard2.SoundWave);
	const double HeldHz = Hz(TakeWav(Heard2.WavFile), 0.2, 1.8);
	TestTrue(FString::Printf(TEXT("and plays it an octave up, held past the sample's end by the loop (%.0f Hz from 0.2 to 1.8 s)"), HeldHz),
		Heard2.bOk && FMath::IsNearlyEqual(HeldHz, 440.0, 15.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSoundRoutingTest,
	"HyperAIStudio.Sound.RoutingAndAutomation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSoundRoutingTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DecibelTests;
	const FNoApproval NoApproval;
	using C = FHyperAIStudioDecibelContracts;
	const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString Tone = WriteSweep(TEXT("HyperAI_Tone_") + Id, 220.0, 220.0);
	const FString Chirp = WriteSweep(TEXT("HyperAI_Chirp_") + Id, 110.0, 880.0);
	TArray<FString> Made = {TEXT("/Game/Audio/Library/HyperAI_Tone_") + Id + TEXT(".HyperAI_Tone_") + Id,
		TEXT("/Game/Audio/Library/HyperAI_Chirp_") + Id + TEXT(".HyperAI_Chirp_") + Id};
	ON_SCOPE_EXIT
	{
		Clean(Made);
		IFileManager::Get().Delete(*Tone);
		IFileManager::Get().Delete(*Chirp);
	};

	// At 120 BPM a beat is half a second. The tone for a second, faded to silence over it by a volume lane; the chirp,
	// rising, played backwards from 1.5 s; the tone again from 3 s as a grain cloud walking it at half speed.
	FHyperAISoundPlanRequest Plan;
	Plan.SessionPath = NewSessionPath(TEXT("Routing"));
	Plan.bCreate = true;
	FHyperAISoundOp Dry = Op(TEXT("track.add"));
	Dry.Name = TEXT("Dry");
	Dry.Type = TEXT("audio");
	FHyperAISoundOp Played = Op(TEXT("clip.audio"), TEXT("Dry"));
	Played.Sound = Tone;
	FHyperAISoundOp Fade = Op(TEXT("lane.set"), TEXT("Dry"));
	Fade.Param = TEXT("volume");
	Fade.Points = {AutoPoint(0.0, 0.f), AutoPoint(2.0, -60.f)};
	FHyperAISoundOp Backwards = Op(TEXT("track.add"));
	Backwards.Name = TEXT("Backwards");
	Backwards.Type = TEXT("audio");
	FHyperAISoundOp Reversed = Op(TEXT("clip.audio"), TEXT("Backwards"));
	Reversed.Sound = Chirp;
	Reversed.StartBeat = 3.0;
	Reversed.bReverse = true;
	FHyperAISoundOp Cloud = Op(TEXT("track.add"));
	Cloud.Name = TEXT("Cloud");
	Cloud.Type = TEXT("audio");
	FHyperAISoundOp Grains = Op(TEXT("clip.audio"), TEXT("Cloud"));
	Grains.Sound = Tone;
	Grains.StartBeat = 6.0;
	Grains.LengthBeats = 4.0;
	Grains.Pitch = -12.f;
	Grains.StretchMode = TEXT("texture");
	Plan.Operations = {Dry, Played, Fade, Backwards, Reversed, Cloud, Grains};
	const FHyperAISoundPlanReport Built = C::ApplyPlan(Plan);
	Made.Add(Built.SessionPath);
	if (!TestTrue(TEXT("The session is built: ") + Built.Diagnostic, Built.bOk && Built.Tracks.Num() == 3))
	{
		return false;
	}
	TestEqual(TEXT("with the fade on the dry track's volume"), Built.Tracks[0].Lanes, TArray<FString>{TEXT("volume: 2 points")});

	FHyperAISoundRenderRequest Render;
	Render.SessionPath = Plan.SessionPath;
	Render.Channels = 1;
	Render.DurationSeconds = 5.5;
	const FHyperAISoundRenderReport Heard = C::Render(Render);
	Made.Add(Heard.SoundWave);
	const TArray<int16> Pcm = TakeWav(Heard.WavFile);
	TestTrue(FString::Printf(TEXT("The volume lane fades the tone (%.0f dB, then %.0f dB)"), RmsDb(Pcm, 0.0, 0.1), RmsDb(Pcm, 0.85, 0.95)),
		RmsDb(Pcm, 0.85, 0.95) < RmsDb(Pcm, 0.0, 0.1) - 30.0);
	TestTrue(FString::Printf(TEXT("the chirp played backwards falls (%.0f Hz, then %.0f Hz)"), Hz(Pcm, 1.55, 1.75), Hz(Pcm, 2.25, 2.45)),
		Hz(Pcm, 1.55, 1.75) > 2.0 * Hz(Pcm, 2.25, 2.45));
	TestTrue(FString::Printf(TEXT("and the grain cloud lasts twice the tone at the tone's pitch (%.0f Hz near its end)"), Hz(Pcm, 4.5, 4.9)),
		FMath::IsNearlyEqual(Hz(Pcm, 4.5, 4.9), 220.0, 20.0));
	const double Before = RmsDb(Pcm, 1.05, 1.45);

	// Stems solo each track: the backwards chirp's stem must not carry the dry tone, whose volume lane would ride a merely
	// faded-down fader straight back up.
	FHyperAISoundRenderRequest Split = Render;
	Split.OutputPath = Plan.SessionPath + TEXT("_Split");
	Split.bStems = true;
	const FHyperAISoundRenderReport Stems = C::Render(Split);
	Made.Add(Stems.SoundWave);
	Made.Append(Stems.Stems);
	const bool bBackwardsStem = Stems.Stems.ContainsByPredicate([](const FString& Stem) { return Stem.Contains(TEXT("_Backwards")); });
	const TArray<int16> Alone = bBackwardsStem ? TakeWav(Stems.WavFile.Replace(TEXT(".wav"), TEXT("_Backwards.wav"))) : TArray<int16>();
	IFileManager::Get().Delete(*(FPaths::ProjectDir() / Stems.WavFile));
	TestTrue(FString::Printf(TEXT("A stem holds its own track alone, even beside a lane (%.0f dB of the tone in the backwards stem)"),
		RmsDb(Alone, 0.0, 0.9)), Stems.bOk && !Alone.IsEmpty() && RmsDb(Alone, 0.0, 0.9) < -80.0 && RmsDb(Alone, 1.55, 2.45) > -40.0);

	// A return with a reverb, fed from before the dry track's fader: it rings on after the tone has faded and ended.
	FHyperAISoundPlanRequest Wet;
	Wet.SessionPath = Plan.SessionPath;
	FHyperAISoundOp Room = Op(TEXT("track.add"));
	Room.Name = TEXT("Room");
	Room.Type = TEXT("fx");
	FHyperAISoundOp Verb = Op(TEXT("device.add"), TEXT("Room"), TEXT("fx.reverb"));
	Verb.Params = {ParamValue(TEXT("rvmix"), 1.f), ParamValue(TEXT("rvdecay"), 4.f)};
	FHyperAISoundOp Send = Op(TEXT("track.send"), TEXT("Dry"));
	Send.SendTo = TEXT("Room");
	Send.SendTap = TEXT("pre");
	FHyperAISoundOp Opening = Op(TEXT("lane.set"), TEXT("Room"), TEXT("fx.reverb"));
	Opening.Param = TEXT("rvmix");
	Opening.Points = {AutoPoint(0.0, 1.f), AutoPoint(2.0, 0.5f)};
	Wet.Operations = {Room, Verb, Send, Opening};
	const FHyperAISoundPlanReport Routed = C::ApplyPlan(Wet);
	TestTrue(TEXT("A return, the dry track's send to it and a lane on its reverb are added: ") + Routed.Diagnostic, Routed.bOk && Routed.Tracks.Num() == 4
		&& Routed.Tracks[0].Sends == TArray<FString>{TEXT("Room x 1.00")} && Routed.Tracks[3].Lanes == TArray<FString>{TEXT("fx.reverb.rvmix: 2 points")});
	const FHyperAISoundRenderReport Rang = C::Render(Render);
	const TArray<int16> WetPcm = TakeWav(Rang.WavFile);
	TestTrue(FString::Printf(TEXT("and the room rings on after the tone (%.0f dB; %.0f dB without it)"), RmsDb(WetPcm, 1.05, 1.45), Before),
		RmsDb(WetPcm, 1.05, 1.45) > -50.0 && Before < -80.0);

	// Refused: a send to a track that is not a return, a lane on a parameter the device lacks, a pitch lane on the master.
	FHyperAISoundPlanRequest Refuse;
	Refuse.SessionPath = Plan.SessionPath;
	FHyperAISoundOp ToAudio = Op(TEXT("track.send"), TEXT("Dry"));
	ToAudio.SendTo = TEXT("Cloud");
	Refuse.Operations = {ToAudio};
	TestFalse(TEXT("A send to an audio track is refused"), C::ApplyPlan(Refuse).bOk);
	FHyperAISoundOp Missing = Op(TEXT("lane.set"), TEXT("Room"), TEXT("fx.reverb"));
	Missing.Param = TEXT("nosuch");
	Missing.Points = {AutoPoint(0.0, 0.f)};
	Refuse.Operations = {Missing};
	TestFalse(TEXT("so is a lane on a parameter the device lacks"), C::ApplyPlan(Refuse).bOk);
	FHyperAISoundOp MasterPitch = Op(TEXT("lane.set"), TEXT("master"));
	MasterPitch.Param = TEXT("pitch");
	MasterPitch.Points = {AutoPoint(0.0, 0.f)};
	Refuse.Operations = {MasterPitch};
	TestFalse(TEXT("and a pitch lane on the master"), C::ApplyPlan(Refuse).bOk);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_DECIBEL
