// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAudioAuthoringToolset.h"
#include "HyperAIStudioAudioWrappers.h"
#include "HyperAIStudioMetaSoundRecipes.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSoundCueGraph.h"
#include "IAssetTools.h"
#include "MetasoundSource.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeModulator.h"
#include "Sound/SoundNodeRandom.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"

namespace HyperAIStudio::AudioAuthoringTests
{
	using C = FHyperAIStudioAudioAuthoringContracts;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** A tiny mono 16-bit WAV: a sine of Hz for Seconds. Audio runs on a null device here, so only the assets are tested. */
	TArray<uint8> TinyWav(const float Seconds, const float Hz, const int32 SampleRate = 22050)
	{
		const int32 Frames = FMath::Max(1, FMath::RoundToInt(Seconds * SampleRate));
		TArray<uint8> Bytes;
		auto U32 = [&Bytes](const uint32 V) { Bytes.Append(reinterpret_cast<const uint8*>(&V), 4); };
		auto U16 = [&Bytes](const uint16 V) { Bytes.Append(reinterpret_cast<const uint8*>(&V), 2); };
		Bytes.Append(reinterpret_cast<const uint8*>("RIFF"), 4);
		U32(36 + Frames * 2);
		Bytes.Append(reinterpret_cast<const uint8*>("WAVEfmt "), 8);
		U32(16);
		U16(1);
		U16(1);
		U32(SampleRate);
		U32(SampleRate * 2);
		U16(2);
		U16(16);
		Bytes.Append(reinterpret_cast<const uint8*>("data"), 4);
		U32(Frames * 2);
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			const int16 Value = static_cast<int16>(FMath::Sin(2.f * PI * Hz * Frame / SampleRate) * 12000.f);
			Bytes.Append(reinterpret_cast<const uint8*>(&Value), 2);
		}
		return Bytes;
	}

	USoundWave* ImportWave(const FString& Folder, const FString& Name, const float Seconds, const float Hz)
	{
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests") / Name + TEXT(".wav"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
		FFileHelper::SaveArrayToFile(TinyWav(Seconds, Hz), *File);
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = Folder;
		Task->DestinationName = Name;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = false;
		FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task});
		IFileManager::Get().Delete(*File, false, true, true);
		for (UObject* Object : Task->GetObjects())
		{
			if (USoundWave* Wave = Cast<USoundWave>(Object))
			{
				return Wave;
			}
		}
		return nullptr;
	}

	FHyperAISoundCueNode Node(const TCHAR* Id, const TCHAR* Kind, TArray<FString> Children = {})
	{
		FHyperAISoundCueNode Out;
		Out.Id = Id;
		Out.Kind = Kind;
		Out.Children = MoveTemp(Children);
		return Out;
	}

	FHyperAISoundCueNode Wave(const FString& Id, const USoundWave* Asset)
	{
		FHyperAISoundCueNode Out = Node(*Id, TEXT("wave_player"));
		Out.Wave = Asset ? Asset->GetPathName() : FString();
		return Out;
	}

	FHyperAISoundAssetInfo Inspect(const FString& Path, const bool bAll = false)
	{
		FHyperAISoundAssetInspectRequest Request;
		Request.Paths = {Path};
		Request.bAllSettings = bAll;
		const FHyperAISoundAssetInspectReport Report = C::Inspect(Request);
		return Report.Assets.Num() == 1 ? Report.Assets[0] : FHyperAISoundAssetInfo();
	}

	FString Setting(const FHyperAISoundAssetInfo& Info, const TCHAR* Name)
	{
		const FHyperAIAudioSetting* Found = Info.Settings.FindByPredicate([Name](const FHyperAIAudioSetting& Each) { return Each.Name == Name; });
		return Found ? Found->Value : FString();
	}

	float Number(const FHyperAISoundAssetInfo& Info, const TCHAR* Name)
	{
		return FCString::Atof(*Setting(Info, Name));
	}

	bool HasIssue(const TArray<FHyperAISoundIssue>& Issues, const TCHAR* Code)
	{
		return Issues.ContainsByPredicate([Code](const FHyperAISoundIssue& Issue) { return Issue.Code == Code; });
	}

	bool HasErrors(const TArray<FHyperAISoundIssue>& Issues)
	{
		return Issues.ContainsByPredicate([](const FHyperAISoundIssue& Issue) { return Issue.Severity == TEXT("error"); });
	}

	FHyperAIAudioSetting Set(const TCHAR* Name, const TCHAR* Value)
	{
		FHyperAIAudioSetting Out;
		Out.Name = Name;
		Out.Value = Value;
		return Out;
	}

	USoundCue* LoadCue(const FString& Path)
	{
		return Cast<USoundCue>(HyperAIStudio::AudioAuthoring::CueGraph::LoadAsset(Path, *USoundCue::StaticClass()));
	}

	/** A folder of its own per run, and everything in it deleted after, with the approval setting put back. */
	struct FScratch
	{
		FString Folder = TEXT("/Game/__HyperAIStudioTests/Audio_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
		bool bApproval = true;

		FScratch()
		{
			UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
			bApproval = Settings->bRequireApprovalForAgentEdits;
			Settings->bRequireApprovalForAgentEdits = false;
		}

		~FScratch()
		{
			GetMutableDefault<UHyperAIStudioSettings>()->bRequireApprovalForAgentEdits = bApproval;
			TArray<FAssetData> Assets;
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByPath(FName(*Folder), Assets, /*bRecursive=*/true);
			// The cues and players first: a wave a cue's Wave Player still holds is "in use", and its package stays behind.
			TArray<UObject*> Users;
			TArray<UObject*> Waves;
			for (const FAssetData& Asset : Assets)
			{
				if (UObject* Object = Asset.GetAsset())
				{
					(Object->GetClass() == USoundWave::StaticClass() ? Waves : Users).Add(Object);
				}
			}
			for (const TArray<UObject*>* Objects : {&Users, &Waves})
			{
				if (!Objects->IsEmpty())
				{
					ObjectTools::ForceDeleteObjects(*Objects, /*ShowConfirmation=*/false);
				}
			}
		}

		FString Path(const TCHAR* Name) const
		{
			return Folder / Name;
		}
	};

	FHyperAISoundCuePlanReport Cue(const FString& Path, TArray<FHyperAISoundCueNode> Nodes, const bool bDryRun = false, const bool bReplace = false)
	{
		FHyperAISoundCuePlanRequest Request;
		Request.Cue = Path;
		Request.Nodes = MoveTemp(Nodes);
		Request.bDryRun = bDryRun;
		Request.bReplaceGraph = bReplace;
		return C::CuePlan(Request);
	}

	FHyperAISoundMixPlanReport Mix(TArray<FHyperAISoundMixAsset> Assets, const bool bDryRun = false)
	{
		FHyperAISoundMixPlanRequest Request;
		Request.Assets = MoveTemp(Assets);
		Request.bDryRun = bDryRun;
		return C::MixPlan(Request);
	}

	FHyperAISoundMixAsset Asset(const TCHAR* Kind, const FString& Path, TArray<FHyperAIAudioSetting> Settings = {}, const FString& Parent = FString())
	{
		FHyperAISoundMixAsset Out;
		Out.Kind = Kind;
		Out.Path = Path;
		Out.Settings = MoveTemp(Settings);
		Out.Parent = Parent;
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioCueGraphTest, "HyperAIStudio.Audio.Authoring.CueGraph", HyperAIStudio::AudioAuthoringTests::Flags)

bool FHyperAIStudioAudioCueGraphTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioAuthoringTests;
	const FScratch Scratch;
	USoundWave* WaveA = ImportWave(Scratch.Folder, TEXT("SW_StepA"), 0.25f, 220.f);
	USoundWave* WaveB = ImportWave(Scratch.Folder, TEXT("SW_StepB"), 0.4f, 330.f);
	if (!TestTrue(TEXT("Two tiny WAVs import as SoundWaves"), WaveA && WaveB))
	{
		return false;
	}
	USoundAttenuation* Falloff = NewObject<USoundAttenuation>(CreatePackage(*Scratch.Path(TEXT("ATT_Steps"))), TEXT("ATT_Steps"), RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(Falloff);

	// One graph with every kind a plan builds: a tree from the attenuation down to eight wave players.
	TArray<FHyperAISoundCueNode> Nodes;
	FHyperAISoundCueNode Attenuation = Node(TEXT("att"), TEXT("attenuation"), {TEXT("mod")});
	Attenuation.Attenuation = Falloff->GetPathName();
	Nodes.Add(Attenuation);
	FHyperAISoundCueNode Modulator = Node(TEXT("mod"), TEXT("modulator"), {TEXT("env")});
	Modulator.PitchMin = 0.95f;
	Modulator.PitchMax = 1.05f;
	Modulator.VolumeMin = 0.8f;
	Nodes.Add(Modulator);
	FHyperAISoundCueNode Enveloper = Node(TEXT("env"), TEXT("enveloper"), {TEXT("loop")});
	Enveloper.VolumeCurve = {FVector2D(0, 0), FVector2D(0.05, 1), FVector2D(0.4, 0.5)};
	Nodes.Add(Enveloper);
	FHyperAISoundCueNode Looping = Node(TEXT("loop"), TEXT("looping"), {TEXT("delay")});
	Looping.LoopCount = 2;
	Nodes.Add(Looping);
	FHyperAISoundCueNode Delay = Node(TEXT("delay"), TEXT("delay"), {TEXT("cont")});
	Delay.DelayMax = 0.1f;
	Nodes.Add(Delay);
	FHyperAISoundCueNode Continuous = Node(TEXT("cont"), TEXT("continuous_modulator"), {TEXT("mix")});
	Continuous.Parameter = TEXT("Speed");
	Continuous.VolumeMin = 0.2f;
	Nodes.Add(Continuous);
	FHyperAISoundCueNode Mixer = Node(TEXT("mix"), TEXT("mixer"), {TEXT("rand"), TEXT("cat")});
	Mixer.Weights = {1.f, 0.5f};
	Nodes.Add(Mixer);
	FHyperAISoundCueNode Random = Node(TEXT("rand"), TEXT("random"), {TEXT("w1"), TEXT("w2")});
	Random.Weights = {2.f, 1.f};
	Random.bNoRepeat = true;
	Nodes.Add(Random);
	Nodes.Add(Node(TEXT("cat"), TEXT("concatenator"), {TEXT("w3"), TEXT("br")}));
	FHyperAISoundCueNode Branch = Node(TEXT("br"), TEXT("branch"), {TEXT("w4"), TEXT("w5"), TEXT("sw")});
	Branch.Parameter = TEXT("Wet");
	Nodes.Add(Branch);
	FHyperAISoundCueNode Switch = Node(TEXT("sw"), TEXT("switch"), {TEXT("xf"), TEXT("w6")});
	Switch.Parameter = TEXT("Surface");
	Nodes.Add(Switch);
	FHyperAISoundCueNode CrossFade = Node(TEXT("xf"), TEXT("param_crossfade"), {TEXT("wp"), TEXT("w7")});
	CrossFade.Parameter = TEXT("Speed");
	FHyperAISoundCueFade Slow;
	Slow.FadeOutStart = 0.4f;
	Slow.FadeOutEnd = 0.6f;
	FHyperAISoundCueFade Fast;
	Fast.FadeInStart = 0.4f;
	Fast.FadeInEnd = 0.6f;
	CrossFade.Fades = {Slow, Fast};
	Nodes.Add(CrossFade);
	FHyperAISoundCueNode WaveParam = Node(TEXT("wp"), TEXT("wave_param"), {TEXT("w8")});
	WaveParam.Parameter = TEXT("Override");
	Nodes.Add(WaveParam);
	for (int32 Index = 1; Index <= 8; ++Index)
	{
		Nodes.Add(Wave(FString::Printf(TEXT("w%d"), Index), Index % 2 ? WaveA : WaveB));
	}
	const FString CuePath = Scratch.Path(TEXT("SC_Steps"));

	const FHyperAISoundCuePlanReport Checked = Cue(CuePath, Nodes, /*bDryRun=*/true);
	TestTrue(TEXT("A dry run checks and lays out all 21 nodes and makes nothing: ") + Checked.Diagnostic, Checked.bOk && Checked.Status == TEXT("checked")
		&& Checked.bCreates && Checked.Nodes.Num() == 21 && FHyperAIStudioNames::IsFreePackage(CuePath));
	if (Checked.Nodes.Num() == 21)
	{
		TestTrue(TEXT("The root sits one column left of the output, level with it"), Checked.Nodes[0].Id == TEXT("att") && Checked.Nodes[0].X == -320 && Checked.Nodes[0].Y == 0);
		const FHyperAISoundCueNodeInfo* Deepest = Checked.Nodes.FindByPredicate([](const FHyperAISoundCueNodeInfo& Info) { return Info.Id == TEXT("w8"); });
		TestTrue(TEXT("and each level a column further left"), Deepest && Deepest->X == -320 * 13);
		TSet<FIntPoint> Spots;
		for (const FHyperAISoundCueNodeInfo& Info : Checked.Nodes)
		{
			Spots.Add(FIntPoint(Info.X, Info.Y));
		}
		TestEqual(TEXT("and no two nodes share a spot"), Spots.Num(), 21);
	}

	const FHyperAISoundCuePlanReport Built = Cue(CuePath, Nodes);
	TestTrue(TEXT("The plan builds and saves the cue: ") + Built.Diagnostic, Built.bOk && Built.Status == TEXT("completed") && Built.Saved.Num() == 1);
	TestFalse(TEXT("and its graph validates clean"), HasErrors(Built.Issues));
	USoundCue* Steps = LoadCue(CuePath);
	if (!TestNotNull(TEXT("The cue is in the project"), Steps))
	{
		return false;
	}
	TestEqual(TEXT("It holds every node"), Steps->AllNodes.Num(), 21);
	int32 Placed = 0;
	for (const USoundNode* SoundNode : Steps->AllNodes)
	{
		Placed += SoundNode && SoundNode->GetGraphNode() && SoundNode->GetGraphNode()->NodePosX < 0 ? 1 : 0;
	}
	TestEqual(TEXT("and every node has a graph node laid out left of the output"), Placed, 21);

	// Read back as a plan: the same kinds, links and settings, ready to send again.
	const FHyperAISoundAssetInfo Read = Inspect(CuePath);
	TestTrue(TEXT("Inspect reads the cue as 21 plan nodes from its root"), Read.Kind == TEXT("sound_cue") && Read.CueNodes.Num() == 21 && Read.CueRoot == TEXT("n0"));
	TSet<FString> Kinds;
	for (const FHyperAISoundCueNode& Each : Read.CueNodes)
	{
		Kinds.Add(Each.Kind);
	}
	TestEqual(TEXT("with all 14 kinds"), Kinds.Num(), 14);
	const FHyperAISoundCueNode* ReadRandom = Read.CueNodes.FindByPredicate([](const FHyperAISoundCueNode& Each) { return Each.Kind == TEXT("random"); });
	TestTrue(TEXT("The random node keeps its weights and no-repeat"), ReadRandom && ReadRandom->bNoRepeat && ReadRandom->Weights.Num() == 2
		&& FMath::IsNearlyEqual(ReadRandom->Weights[0], 2.f));
	const FHyperAISoundCueNode* ReadSwitch = Read.CueNodes.FindByPredicate([](const FHyperAISoundCueNode& Each) { return Each.Kind == TEXT("switch"); });
	TestTrue(TEXT("the switch its parameter"), ReadSwitch && ReadSwitch->Parameter == TEXT("Surface") && ReadSwitch->Children.Num() == 2);
	const FHyperAISoundCueNode* ReadFade = Read.CueNodes.FindByPredicate([](const FHyperAISoundCueNode& Each) { return Each.Kind == TEXT("param_crossfade"); });
	TestTrue(TEXT("the cross-fade its fades"), ReadFade && ReadFade->Fades.Num() == 2 && FMath::IsNearlyEqual(ReadFade->Fades[1].FadeInEnd, 0.6f));
	const FHyperAISoundCueNode* ReadEnvelope = Read.CueNodes.FindByPredicate([](const FHyperAISoundCueNode& Each) { return Each.Kind == TEXT("enveloper"); });
	TestTrue(TEXT("the enveloper its curve"), ReadEnvelope && ReadEnvelope->VolumeCurve.Num() == 3);
	TestTrue(TEXT("Inspect finds nothing wrong"), !HasErrors(Read.Issues));

	FHyperAISoundCuePlanRequest Again;
	Again.Cue = CuePath;
	Again.Nodes = Read.CueNodes;
	Again.Root = Read.CueRoot;
	Again.bReplaceGraph = true;
	Again.bDryRun = false;
	const FHyperAISoundCuePlanReport Rebuilt = C::CuePlan(Again);
	TestTrue(TEXT("What inspect read builds the same graph again: ") + Rebuilt.Diagnostic, Rebuilt.bOk && LoadCue(CuePath)->AllNodes.Num() == 21);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioCueRefusalsTest, "HyperAIStudio.Audio.Authoring.CueValidationAndRefusals", HyperAIStudio::AudioAuthoringTests::Flags)

bool FHyperAIStudioAudioCueRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioAuthoringTests;
	const FScratch Scratch;
	USoundWave* WaveA = ImportWave(Scratch.Folder, TEXT("SW_HitA"), 0.2f, 440.f);
	if (!TestNotNull(TEXT("A tiny WAV imports"), WaveA))
	{
		return false;
	}
	const FString CuePath = Scratch.Path(TEXT("SC_Hit"));
	auto Refused = [&](const TCHAR* What, TArray<FHyperAISoundCueNode> Nodes, const TCHAR* Expect)
	{
		const FHyperAISoundCuePlanReport Report = Cue(CuePath, MoveTemp(Nodes), /*bDryRun=*/true);
		TestTrue(FString(What) + TEXT(": ") + Report.Diagnostic, !Report.bOk && Report.Status == TEXT("invalid_request") && Report.Diagnostic.Contains(Expect));
	};
	FHyperAISoundCueNode Missing = Wave(TEXT("w"), WaveA);
	Missing.Wave = Scratch.Path(TEXT("SW_NotThere"));
	Refused(TEXT("A wave that is not in the project is refused"), {Missing}, TEXT("not a SoundWave"));
	Refused(TEXT("A node the root does not reach would dangle"), {Wave(TEXT("w1"), WaveA), Wave(TEXT("w2"), WaveA)}, TEXT("dangle"));
	Refused(TEXT("A node fed into two parents is refused"), {Node(TEXT("mix"), TEXT("mixer"), {TEXT("a"), TEXT("b")}), Node(TEXT("a"), TEXT("modulator"), {TEXT("w")}),
		Node(TEXT("b"), TEXT("modulator"), {TEXT("w")}), Wave(TEXT("w"), WaveA)}, TEXT("feeds both"));
	Refused(TEXT("A modulator takes one input"), {Node(TEXT("mod"), TEXT("modulator"), {TEXT("w1"), TEXT("w2")}), Wave(TEXT("w1"), WaveA), Wave(TEXT("w2"), WaveA)},
		TEXT("takes 1 input"));
	Refused(TEXT("A branch takes three"), {Node(TEXT("br"), TEXT("branch"), {TEXT("w1")}), Wave(TEXT("w1"), WaveA)}, TEXT("takes 3"));
	Refused(TEXT("An unknown kind is refused"), {Node(TEXT("x"), TEXT("granular"))}, TEXT("Kind is one of"));
	Refused(TEXT("A loop is refused"), {Node(TEXT("a"), TEXT("modulator"), {TEXT("b")}), Node(TEXT("b"), TEXT("modulator"), {TEXT("a")})}, TEXT("loop"));
	FHyperAISoundCueNode Weighted = Node(TEXT("r"), TEXT("random"), {TEXT("w")});
	Weighted.Weights = {1.f, 1.f};
	Refused(TEXT("Weights must match the inputs"), {Weighted, Wave(TEXT("w"), WaveA)}, TEXT("Weights has 2"));
	FHyperAISoundCueNode NoParameter = Node(TEXT("br"), TEXT("branch"), {TEXT("w1"), TEXT(""), TEXT("w2")});
	Refused(TEXT("A branch needs its parameter"), {NoParameter, Wave(TEXT("w1"), WaveA), Wave(TEXT("w2"), WaveA)}, TEXT("Parameter"));
	FHyperAISoundCueNode Pitch = Node(TEXT("mod"), TEXT("modulator"), {TEXT("w")});
	Pitch.PitchMin = 1.2f;
	Pitch.PitchMax = 0.8f;
	Refused(TEXT("A pitch range upside down is refused"), {Pitch, Wave(TEXT("w"), WaveA)}, TEXT("PitchMin"));

	// A branch may leave an input empty on purpose: it plays nothing for that value, and validation only warns.
	NoParameter.Parameter = TEXT("Indoors");
	const FHyperAISoundCuePlanReport Branchy = Cue(CuePath, {NoParameter, Wave(TEXT("w1"), WaveA), Wave(TEXT("w2"), WaveA)});
	TestTrue(TEXT("A branch with an empty input builds, with a warning: ") + Branchy.Diagnostic, Branchy.bOk && HasIssue(Branchy.Issues, TEXT("empty_input"))
		&& !HasErrors(Branchy.Issues));
	const FHyperAISoundCuePlanReport Exists = Cue(CuePath, {Wave(TEXT("w"), WaveA)}, /*bDryRun=*/true);
	TestTrue(TEXT("An existing cue's graph is replaced only with bReplaceGraph"), !Exists.bOk && Exists.Status == TEXT("graph_exists"));
	TestTrue(TEXT("A new cue needs a graph"), !Cue(Scratch.Path(TEXT("SC_Empty")), {}, /*bDryRun=*/true).bOk);
	FHyperAISoundCuePlanRequest NotACue;
	NotACue.Cue = WaveA->GetPathName();
	NotACue.VolumeMultiplier = 0.5f;
	TestTrue(TEXT("A SoundWave is not a cue"), C::CuePlan(NotACue).Diagnostic.Contains(TEXT("not a Sound Cue")));

	// Validation of a cue broken by hand: a wave player without its wave, a dangling node, a random input left empty.
	USoundCue* Broken = LoadCue(CuePath);
	if (!TestNotNull(TEXT("The branch cue is there"), Broken))
	{
		return false;
	}
	USoundNodeWavePlayer* Orphan = Broken->ConstructSoundNode<USoundNodeWavePlayer>(USoundNodeWavePlayer::StaticClass(), false);
	TestTrue(TEXT("A node the output does not reach is a dangling node"), HasIssue(HyperAIStudio::AudioAuthoring::CueGraph::Validate(*Broken), TEXT("dangling_node")));
	TestTrue(TEXT("and one without its wave is a missing wave, once reached"), Orphan != nullptr);
	USoundNodeRandom* Random = Broken->ConstructSoundNode<USoundNodeRandom>(USoundNodeRandom::StaticClass(), false);
	Random->InsertChildNode(0);
	Random->InsertChildNode(1);
	Random->ChildNodes[0] = Orphan;
	Broken->FirstNode = Random;
	const TArray<FHyperAISoundIssue> Issues = HyperAIStudio::AudioAuthoring::CueGraph::Validate(*Broken);
	TestTrue(TEXT("An empty random input is an error"), HasIssue(Issues, TEXT("empty_input")) && HasErrors(Issues));
	TestTrue(TEXT("A wave player without a wave is missing its wave"), HasIssue(Issues, TEXT("missing_wave")));
	TestTrue(TEXT("The old graph now dangles"), HasIssue(Issues, TEXT("dangling_node")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioCueUndoTest, "HyperAIStudio.Audio.Authoring.CueUndoAndApproval", HyperAIStudio::AudioAuthoringTests::Flags)

bool FHyperAIStudioAudioCueUndoTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioAuthoringTests;
	const FScratch Scratch;
	USoundWave* WaveA = ImportWave(Scratch.Folder, TEXT("SW_DoorA"), 0.2f, 200.f);
	USoundWave* WaveB = ImportWave(Scratch.Folder, TEXT("SW_DoorB"), 0.3f, 250.f);
	if (!TestTrue(TEXT("Two tiny WAVs import"), WaveA && WaveB) || !GEditor || !GEditor->Trans)
	{
		return true;
	}
	const FString CuePath = Scratch.Path(TEXT("SC_Door"));
	TestTrue(TEXT("A one-wave cue is made"), Cue(CuePath, {Wave(TEXT("w"), WaveA)}).bOk);
	USoundCue* Door = LoadCue(CuePath);
	if (!TestNotNull(TEXT("and loads"), Door))
	{
		return false;
	}
	FHyperAISoundCuePlanRequest Rebuild;
	Rebuild.Cue = CuePath;
	Rebuild.Nodes = {Node(TEXT("mod"), TEXT("modulator"), {TEXT("pick")}), Node(TEXT("pick"), TEXT("random"), {TEXT("a"), TEXT("b")}),
		Wave(TEXT("a"), WaveA), Wave(TEXT("b"), WaveB)};
	Rebuild.bReplaceGraph = true;
	Rebuild.VolumeMultiplier = 0.5f;
	Rebuild.bSave = false;
	Rebuild.bDryRun = false;

	// With approval on, the plan waits; turned down, the cue is untouched.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	Settings->bRequireApprovalForAgentEdits = true;
	const FHyperAISoundCuePlanReport Held = C::CuePlan(Rebuild);
	TestTrue(TEXT("With approval on, the plan waits for the user: ") + Held.Diagnostic, Held.bOk && Held.Status == TEXT("awaiting_user_approval")
		&& FHyperAIStudioApprovalGate::HasPending(Held.OperationId));
	FHyperAIStudioApprovalGate::Reject(Held.OperationId, TEXT("Not now."));
	TestTrue(TEXT("Rejected, the cue keeps its one wave"), Door->AllNodes.Num() == 1 && Door->FirstNode && Door->FirstNode->IsA<USoundNodeWavePlayer>());
	Settings->bRequireApprovalForAgentEdits = false;

	const float VolumeBefore = Door->VolumeMultiplier;
	const FHyperAISoundCuePlanReport Done = C::CuePlan(Rebuild);
	TestTrue(TEXT("The rebuild runs: ") + Done.Diagnostic, Done.bOk && Door->AllNodes.Num() == 4 && Door->FirstNode && Door->FirstNode->IsA<USoundNodeModulator>()
		&& FMath::IsNearlyEqual(Door->VolumeMultiplier, 0.5f));
	TestTrue(TEXT("Edit > Undo takes the whole plan back in one step"), GEditor->UndoTransaction());
	TestTrue(TEXT("the old graph and volume are back"), Door->AllNodes.Num() == 1 && Door->FirstNode && Door->FirstNode->IsA<USoundNodeWavePlayer>()
		&& FMath::IsNearlyEqual(Door->VolumeMultiplier, VolumeBefore));
	TestFalse(TEXT("and the restored graph validates clean"), HasErrors(HyperAIStudio::AudioAuthoring::CueGraph::Validate(*Door)));
	TestTrue(TEXT("Redo puts the plan back"), GEditor->RedoTransaction() && Door->AllNodes.Num() == 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioMixTest, "HyperAIStudio.Audio.Authoring.MixRoundTrip", HyperAIStudio::AudioAuthoringTests::Flags)

bool FHyperAIStudioAudioMixTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioAuthoringTests;
	const FScratch Scratch;
	const FString Master = Scratch.Path(TEXT("SCL_Master"));
	const FString Sfx = Scratch.Path(TEXT("SCL_SFX"));
	const FString Duck = Scratch.Path(TEXT("MIX_Duck"));
	const FString Falloff = Scratch.Path(TEXT("ATT_Footsteps"));
	const FString Voices = Scratch.Path(TEXT("CON_Footsteps"));
	const FString MasterSubmix = Scratch.Path(TEXT("SMX_Master"));
	const FString World = Scratch.Path(TEXT("SMX_World"));

	TArray<FHyperAISoundMixAsset> Plan;
	Plan.Add(Asset(TEXT("sound_class"), Master, {Set(TEXT("Volume"), TEXT("0.9"))}));
	Plan.Add(Asset(TEXT("sound_class"), Sfx, {Set(TEXT("Volume"), TEXT("0.8")), Set(TEXT("bIsUISound"), TEXT("true")), Set(TEXT("LowPassFilterFrequency"), TEXT("12000"))}, Master));
	FHyperAISoundMixAsset DuckMix = Asset(TEXT("sound_mix"), Duck, {Set(TEXT("FadeInTime"), TEXT("0.25")), Set(TEXT("Duration"), TEXT("-1")), Set(TEXT("FadeOutTime"), TEXT("0.5"))});
	FHyperAISoundMixAdjuster Adjuster;
	Adjuster.SoundClass = Sfx;
	Adjuster.Volume = 0.5f;
	Adjuster.LowPassFilterFrequency = 8000.f;
	DuckMix.Adjusters = {Adjuster};
	FHyperAISoundEQBand Band;
	Band.Frequency = 500.f;
	Band.Gain = 1.2f;
	DuckMix.EQBands = {Band};
	Plan.Add(DuckMix);
	Plan.Add(Asset(TEXT("attenuation"), Falloff, {Set(TEXT("FalloffDistance"), TEXT("2500")), Set(TEXT("DistanceAlgorithm"), TEXT("NaturalSound")),
		Set(TEXT("bAttenuateWithLPF"), TEXT("true")), Set(TEXT("AttenuationShapeExtents"), TEXT("(X=400,Y=0,Z=0)"))}));
	Plan.Add(Asset(TEXT("concurrency"), Voices, {Set(TEXT("MaxCount"), TEXT("4")), Set(TEXT("ResolutionRule"), TEXT("StopOldest")), Set(TEXT("VolumeScale"), TEXT("0.8"))}));
	Plan.Add(Asset(TEXT("submix"), MasterSubmix));
	FHyperAISoundMixAsset WorldSubmix = Asset(TEXT("submix"), World, {}, MasterSubmix);
	FHyperAISubmixEffect Reverb;
	Reverb.Kind = TEXT("reverb");
	Reverb.Settings = {Set(TEXT("DecayTime"), TEXT("2.5")), Set(TEXT("WetLevel"), TEXT("0.4"))};
	FHyperAISubmixEffect EQ;
	EQ.Kind = TEXT("eq");
	FHyperAISoundEQBand Cut;
	Cut.Frequency = 200.f;
	Cut.Gain = -3.f;
	EQ.Bands = {Cut};
	FHyperAISubmixEffect Compressor;
	Compressor.Kind = TEXT("compressor");
	Compressor.Settings = {Set(TEXT("ThresholdDb"), TEXT("-12")), Set(TEXT("Ratio"), TEXT("4"))};
	WorldSubmix.Effects = {Reverb, EQ, Compressor};
	Plan.Add(WorldSubmix);

	const FHyperAISoundMixPlanReport Checked = Mix(Plan, /*bDryRun=*/true);
	TestTrue(TEXT("A dry run lists every change and makes nothing: ") + Checked.Diagnostic, Checked.bOk && Checked.Status == TEXT("checked")
		&& Checked.Changes.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("FalloffDistance")) && Line.Contains(TEXT("2500")); })
		&& FHyperAIStudioNames::IsFreePackage(Master));
	const FHyperAISoundMixPlanReport Done = Mix(Plan);
	TestTrue(TEXT("The plan makes all seven assets and the three effect presets: ") + Done.Diagnostic, Done.bOk && Done.Created.Num() == 10 && Done.Saved.Num() == 10);

	// Read back: each setting as set.
	const FHyperAISoundAssetInfo SfxInfo = Inspect(Sfx);
	TestTrue(TEXT("The class keeps its volume, UI flag and low pass"), FMath::IsNearlyEqual(Number(SfxInfo, TEXT("Volume")), 0.8f)
		&& Setting(SfxInfo, TEXT("bIsUISound")) == TEXT("True") && FMath::IsNearlyEqual(Number(SfxInfo, TEXT("LowPassFilterFrequency")), 12000.f));
	const FHyperAISoundAssetInfo MasterInfo = Inspect(Master);
	TestTrue(TEXT("and its parent, which lists it as a child"), SfxInfo.Parent.StartsWith(Master) && MasterInfo.Children.Num() == 1 && MasterInfo.Children[0].StartsWith(Sfx));
	const FHyperAISoundAssetInfo DuckInfo = Inspect(Duck);
	TestTrue(TEXT("The mix keeps its fades, its adjuster and its EQ"), FMath::IsNearlyEqual(Number(DuckInfo, TEXT("FadeInTime")), 0.25f)
		&& FMath::IsNearlyEqual(Number(DuckInfo, TEXT("Duration")), -1.f) && Setting(DuckInfo, TEXT("bApplyEQ")) == TEXT("True")
		&& DuckInfo.Details.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("SCL_SFX")) && Line.Contains(TEXT("volume 0.5")); }));
	const FHyperAISoundAssetInfo FalloffInfo = Inspect(Falloff);
	TestTrue(TEXT("The attenuation keeps its falloff, model, low pass and radius"), FMath::IsNearlyEqual(Number(FalloffInfo, TEXT("FalloffDistance")), 2500.f)
		&& Setting(FalloffInfo, TEXT("DistanceAlgorithm")) == TEXT("NaturalSound") && Setting(FalloffInfo, TEXT("bAttenuateWithLPF")) == TEXT("True")
		&& Setting(FalloffInfo, TEXT("AttenuationShapeExtents")).Contains(TEXT("X=400")));
	const FHyperAISoundAssetInfo VoicesInfo = Inspect(Voices);
	TestTrue(TEXT("The concurrency keeps its count, rule and volume scale"), Setting(VoicesInfo, TEXT("MaxCount")) == TEXT("4")
		&& Setting(VoicesInfo, TEXT("ResolutionRule")) == TEXT("StopOldest") && FMath::IsNearlyEqual(Number(VoicesInfo, TEXT("VolumeScale")), 0.8f));
	const FHyperAISoundAssetInfo WorldInfo = Inspect(World);
	TestTrue(TEXT("The submix has its parent and three effects in order"), WorldInfo.Parent.StartsWith(MasterSubmix) && WorldInfo.Details.Num() == 3
		&& WorldInfo.Details[0].Contains(TEXT("reverb")) && WorldInfo.Details[0].Contains(TEXT("DecayTime=2.5"))
		&& WorldInfo.Details[1].Contains(TEXT("band 200 Hz -3 dB")) && WorldInfo.Details[2].Contains(TEXT("ThresholdDb=-12")));
	const FHyperAISoundAssetInfo MasterSubmixInfo = Inspect(MasterSubmix);
	TestTrue(TEXT("and its parent lists it"), MasterSubmixInfo.Children.Num() == 1 && MasterSubmixInfo.Children[0].StartsWith(World));

	// An edit of existing assets is one undo step: the class's volume and parent both come back.
	USoundClass* SfxClass = Cast<USoundClass>(HyperAIStudio::AudioAuthoring::CueGraph::LoadAsset(Sfx, *USoundClass::StaticClass()));
	USoundClass* MasterClass = Cast<USoundClass>(HyperAIStudio::AudioAuthoring::CueGraph::LoadAsset(Master, *USoundClass::StaticClass()));
	FHyperAISoundMixPlanRequest Edit;
	Edit.Assets = {Asset(TEXT("sound_class"), Sfx, {Set(TEXT("Volume"), TEXT("0.6"))}, TEXT("none"))};
	Edit.bDryRun = false;
	Edit.bSave = false;
	const FHyperAISoundMixPlanReport Edited = C::MixPlan(Edit);
	TestTrue(TEXT("An edit detaches the class and turns it down: ") + Edited.Diagnostic, Edited.bOk && SfxClass && MasterClass
		&& !SfxClass->ParentClass && FMath::IsNearlyEqual(SfxClass->Properties.Volume, 0.6f) && !MasterClass->ChildClasses.Contains(SfxClass));
	if (GEditor && GEditor->Trans && SfxClass && MasterClass)
	{
		TestTrue(TEXT("Edit > Undo takes the edit back"), GEditor->UndoTransaction());
		TestTrue(TEXT("volume, parent and the parent's child list together"), SfxClass->ParentClass == MasterClass
			&& FMath::IsNearlyEqual(SfxClass->Properties.Volume, 0.8f) && MasterClass->ChildClasses.Contains(SfxClass));
	}

	// Refused before anything changes.
	auto Refused = [&](const TCHAR* What, TArray<FHyperAISoundMixAsset> Assets, const TCHAR* Expect)
	{
		const FHyperAISoundMixPlanReport Report = Mix(MoveTemp(Assets), /*bDryRun=*/true);
		TestTrue(FString(What) + TEXT(": ") + Report.Diagnostic, !Report.bOk && Report.Status == TEXT("invalid_request") && Report.Diagnostic.Contains(Expect));
	};
	Refused(TEXT("An unknown setting"), {Asset(TEXT("sound_class"), Sfx, {Set(TEXT("Loudness"), TEXT("1"))})}, TEXT("Loudness"));
	Refused(TEXT("A link the plan's own fields set"), {Asset(TEXT("sound_class"), Sfx, {Set(TEXT("ChildClasses"), TEXT("()"))})}, TEXT("ChildClasses"));
	Refused(TEXT("A value the setting does not take"), {Asset(TEXT("sound_class"), Sfx, {Set(TEXT("Volume"), TEXT("loud"))})}, TEXT("Volume"));
	Refused(TEXT("A parent that is the class's own child"), {Asset(TEXT("sound_class"), Master, {}, Sfx), Asset(TEXT("sound_class"), Sfx, {}, Master)}, TEXT("own child"));
	FHyperAISoundMixAsset WrongEffects = Asset(TEXT("sound_class"), Sfx);
	WrongEffects.Effects = {Reverb};
	Refused(TEXT("Effects on a class"), {WrongEffects}, TEXT("submix"));
	Refused(TEXT("An asset of another kind at the path"), {Asset(TEXT("sound_mix"), Sfx)}, TEXT("not a SoundMix"));
	Refused(TEXT("An unknown kind"), {Asset(TEXT("bus"), Scratch.Path(TEXT("BUS_X")))}, TEXT("Kind is"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioRecipesTest, "HyperAIStudio.Audio.Authoring.MetaSoundRecipes", HyperAIStudio::AudioAuthoringTests::Flags)

bool FHyperAIStudioAudioRecipesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioAuthoringTests;
	const FScratch Scratch;
	FHyperAIMetaSoundRecipeRequest List;
	const FHyperAIMetaSoundRecipeReport Listed = C::Recipe(List);
	TestTrue(TEXT("An empty recipe lists the six, labelled experimental"), Listed.bOk && Listed.Recipes.Num() == 6 && Listed.bExperimental);
	for (const FHyperAIMetaSoundRecipeInfo& Info : Listed.Recipes)
	{
		FHyperAIMetaSoundRecipeRequest Request;
		Request.Recipe = Info.Recipe;
		Request.Path = Scratch.Path(*(TEXT("MS_") + Info.Recipe));
		Request.bDryRun = true;
		const FHyperAIMetaSoundRecipeReport Checked = C::Recipe(Request);
		TestTrue(Info.Recipe + TEXT(" dry-runs and makes nothing"), Checked.bOk && Checked.Status == TEXT("checked") && FHyperAIStudioNames::IsFreePackage(Request.Path));
		Request.bDryRun = false;
		if (Info.Recipe == TEXT("engine_hum"))
		{
			Request.Inputs = {Set(TEXT("RPM"), TEXT("2400"))};
		}
		const FHyperAIMetaSoundRecipeReport Built = C::Recipe(Request);
		UObject* Source = HyperAIStudio::AudioAuthoring::CueGraph::LoadAsset(Request.Path, *UMetaSoundSource::StaticClass());
		TestTrue(Info.Recipe + TEXT(" builds a MetaSound Source: ") + Built.Diagnostic, Built.bOk && Built.NodeCount > 3 && Source != nullptr);
		if (!Source)
		{
			continue;
		}
		const TArray<FString> Inputs = HyperAIStudio::AudioAuthoring::MetaSounds::DescribeInputs(*Source);
		for (const FHyperAIMetaSoundInputInfo& Input : Info.Inputs)
		{
			TestTrue(Info.Recipe + TEXT(" exposes its input ") + Input.Name, Inputs.ContainsByPredicate([&Input](const FString& Line) { return Line.Contains(TEXT("input ") + Input.Name + TEXT(" (")); }));
		}
		const FHyperAISoundAssetInfo Read = Inspect(Request.Path);
		TestTrue(Info.Recipe + TEXT(" reads back as a MetaSound Source with its inputs"), Read.Kind == TEXT("metasound_source") && Read.Details.Num() >= Info.Inputs.Num());
	}
	auto Refused = [&](const TCHAR* What, const TCHAR* Recipe, TArray<FHyperAIAudioSetting> Inputs, const FString& Path, const TCHAR* Status)
	{
		FHyperAIMetaSoundRecipeRequest Request;
		Request.Recipe = Recipe;
		Request.Inputs = MoveTemp(Inputs);
		Request.Path = Path;
		const FHyperAIMetaSoundRecipeReport Report = C::Recipe(Request);
		TestTrue(FString(What) + TEXT(": ") + Report.Diagnostic, !Report.bOk && Report.Status == Status);
	};
	Refused(TEXT("An unknown recipe"), TEXT("thunder"), {}, Scratch.Path(TEXT("MS_Thunder")), TEXT("invalid_request"));
	Refused(TEXT("An input out of range"), TEXT("engine_hum"), {Set(TEXT("RPM"), TEXT("90000"))}, Scratch.Path(TEXT("MS_Engine2")), TEXT("invalid_request"));
	Refused(TEXT("An input the recipe lacks"), TEXT("ui_blip"), {Set(TEXT("Color"), TEXT("1"))}, Scratch.Path(TEXT("MS_Blip2")), TEXT("invalid_request"));
	Refused(TEXT("A path already taken"), TEXT("ui_blip"), {}, Scratch.Path(TEXT("MS_wind_loop")), TEXT("asset_exists"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioWrappersTest, "HyperAIStudio.Audio.Authoring.Wrappers", HyperAIStudio::AudioAuthoringTests::Flags)

bool FHyperAIStudioAudioWrappersTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioAuthoringTests;
	const FScratch Scratch;
	USoundWave* WaveA = ImportWave(Scratch.Folder, TEXT("SFX_Zap_01"), 0.2f, 600.f);
	USoundWave* WaveB = ImportWave(Scratch.Folder, TEXT("SFX_Zap_02"), 0.3f, 700.f);
	USoundWave* WaveC = ImportWave(Scratch.Folder, TEXT("SFX_Zap_03"), 0.25f, 800.f);
	if (!TestTrue(TEXT("Three tiny WAVs import"), WaveA && WaveB && WaveC))
	{
		return false;
	}
	const TArray<USoundWave*> Waves = {WaveA, WaveB, WaveC};
	for (const TCHAR* Mode : {TEXT("one_shot"), TEXT("loop"), TEXT("random")})
	{
		FString Error;
		const FString Path = FHyperAIStudioAudioWrappers::MakeMetaSoundPlayer(Scratch.Path(*(FString(TEXT("MS_Zap_")) + Mode)), Waves, Mode,
			FVector2D(-1.0, 1.0), FVector2D(0.9, 1.0), Error);
		TestTrue(FString(Mode) + TEXT(" makes a MetaSound Source: ") + Error, !Path.IsEmpty() && HyperAIStudio::AudioAuthoring::CueGraph::LoadAsset(Path, *UMetaSoundSource::StaticClass()));
		if (FCString::Strcmp(Mode, TEXT("random")) == 0 && !Path.IsEmpty())
		{
			TestTrue(TEXT("The random player takes its takes as an input a designer can add to"),
				Inspect(Path).Details.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("input Sounds")); }));
		}
	}
	FString Error;
	TestTrue(TEXT("A taken path is refused"), FHyperAIStudioAudioWrappers::MakeMetaSoundPlayer(Scratch.Path(TEXT("MS_Zap_random")), Waves, TEXT("random"),
		FVector2D::ZeroVector, FVector2D(1.0, 1.0), Error).IsEmpty() && !Error.IsEmpty());
	Error.Reset();
	const FString CuePath = FHyperAIStudioAudioWrappers::MakeRandomCue(Scratch.Path(TEXT("SC_Zap")), Waves, FVector2D(-2.0, 2.0), FVector2D(0.8, 1.0), /*bLoop=*/false, Error);
	USoundCue* Zap = LoadCue(CuePath);
	TestTrue(TEXT("The random cue is made: ") + Error, Zap != nullptr);
	if (Zap)
	{
		const USoundNodeModulator* Modulator = Cast<USoundNodeModulator>(Zap->FirstNode);
		TestTrue(TEXT("A modulator on top with the pitch range in multipliers"), Modulator && FMath::IsNearlyEqual(Modulator->PitchMin, FMath::Pow(2.f, -2.f / 12.f), 0.001f)
			&& FMath::IsNearlyEqual(Modulator->VolumeMin, 0.8f));
		const USoundNodeRandom* Random = Modulator && Modulator->ChildNodes.Num() == 1 ? Cast<USoundNodeRandom>(Modulator->ChildNodes[0]) : nullptr;
		TestTrue(TEXT("over a no-repeat random node of the three takes"), Random && Random->ChildNodes.Num() == 3 && Random->bRandomizeWithoutReplacement);
		TestFalse(TEXT("and it validates clean"), HasErrors(HyperAIStudio::AudioAuthoring::CueGraph::Validate(*Zap)));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
