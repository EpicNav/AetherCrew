// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS && WITH_DECIBEL

#include "HAL/FileManager.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioDecibelToolset.h"
#include "HyperAIStudioSettings.h"
#include "Misc/AutomationTest.h"
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


#endif // WITH_DEV_AUTOMATION_TESTS && WITH_DECIBEL
