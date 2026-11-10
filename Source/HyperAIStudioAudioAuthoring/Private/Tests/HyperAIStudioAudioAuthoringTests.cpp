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
#include "HyperAIStudioMetaSoundGraph.h"
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
	TestTrue(TEXT("An empty recipe lists the ten, labelled experimental"), Listed.bOk && Listed.Recipes.Num() == 10 && Listed.bExperimental);
	for (const FHyperAIMetaSoundRecipeInfo& Info : Listed.Recipes)
	{
		// The recipes that need SoundWaves are tested with takes in WaveRecipes; whoosh also builds from noise alone.
		if (Info.bUsesWaves && Info.Recipe != TEXT("whoosh"))
		{
			continue;
		}
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

namespace HyperAIStudio::AudioAuthoringTests
{
	/** A mono 16-bit WAV of the samples, imported as a SoundWave: footstep-like thumps, noise bursts, sines. */
	USoundWave* ImportSamples(const FString& Folder, const FString& Name, const TArray<float>& Samples, const int32 SampleRate = 48000)
	{
		TArray<uint8> Bytes;
		auto U32 = [&Bytes](const uint32 V) { Bytes.Append(reinterpret_cast<const uint8*>(&V), 4); };
		auto U16 = [&Bytes](const uint16 V) { Bytes.Append(reinterpret_cast<const uint8*>(&V), 2); };
		Bytes.Append(reinterpret_cast<const uint8*>("RIFF"), 4);
		U32(36 + Samples.Num() * 2);
		Bytes.Append(reinterpret_cast<const uint8*>("WAVEfmt "), 8);
		U32(16);
		U16(1);
		U16(1);
		U32(SampleRate);
		U32(SampleRate * 2);
		U16(2);
		U16(16);
		Bytes.Append(reinterpret_cast<const uint8*>("data"), 4);
		U32(Samples.Num() * 2);
		for (const float Sample : Samples)
		{
			const int16 Value = static_cast<int16>(FMath::Clamp(Sample, -1.f, 1.f) * 32767.f);
			Bytes.Append(reinterpret_cast<const uint8*>(&Value), 2);
		}
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests") / Name + TEXT(".wav"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
		FFileHelper::SaveArrayToFile(Bytes, *File);
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
				// Inline: tests run with no audio device, whose streaming manager is a dummy that never loads a streamed chunk.
				Wave->LoadingBehavior = ESoundWaveLoadingBehavior::ForceInline;
				Wave->PostEditChange();
				return Wave;
			}
		}
		return nullptr;
	}

	/** A step: a low thump with a short decay and a burst of filtered noise for the scuff. */
	TArray<float> Step(const int32 Seed, const float Seconds = 0.25f, const int32 Rate = 48000)
	{
		FRandomStream Random(Seed);
		TArray<float> Out;
		Out.SetNumZeroed(FMath::RoundToInt(Seconds * Rate));
		float Low = 0.f;
		const float Hz = 70.f + 15.f * Seed;
		for (int32 Index = 0; Index < Out.Num(); ++Index)
		{
			const float T = Index / static_cast<float>(Rate);
			Low += 0.25f * (Random.FRandRange(-1.f, 1.f) - Low);
			Out[Index] = 0.5f * FMath::Sin(2.f * PI * Hz * T) * FMath::Exp(-T * 30.f) + 0.35f * Low * FMath::Exp(-T * 18.f);
		}
		return Out;
	}

	/** A hit's transient: a click of white noise; or its tail: a long, darker ring. */
	TArray<float> Burst(const int32 Seed, const float Seconds, const float Decay, const float Smooth, const int32 Rate = 48000)
	{
		FRandomStream Random(Seed);
		TArray<float> Out;
		Out.SetNumZeroed(FMath::RoundToInt(Seconds * Rate));
		float State = 0.f;
		for (int32 Index = 0; Index < Out.Num(); ++Index)
		{
			const float T = Index / static_cast<float>(Rate);
			State += Smooth * (Random.FRandRange(-1.f, 1.f) - State);
			Out[Index] = 0.7f * State * FMath::Exp(-T * Decay);
		}
		return Out;
	}

	TArray<float> Sine(const float Hz, const float Seconds, const float Amplitude, const int32 Rate = 48000)
	{
		TArray<float> Out;
		Out.SetNumZeroed(FMath::RoundToInt(Seconds * Rate));
		for (int32 Index = 0; Index < Out.Num(); ++Index)
		{
			Out[Index] = Amplitude * FMath::Sin(2.f * PI * Hz * Index / Rate);
		}
		return Out;
	}

	FHyperAIAudioAnalyzeReport Analyze(const FString& Sound, const int32 Plays = 1, const float MaxSeconds = 3.f, TArray<FHyperAIAudioSetting> Parameters = {})
	{
		FHyperAIAudioAnalyzeRequest Request;
		Request.Sound = Sound;
		Request.Plays = Plays;
		Request.MaxSeconds = MaxSeconds;
		Request.Parameters = MoveTemp(Parameters);
		return C::Analyze(Request);
	}

	FHyperAIMetaSoundGraphNode GraphNode(const TCHAR* Id, const TCHAR* Class, TArray<FHyperAIAudioSetting> Defaults = {})
	{
		FHyperAIMetaSoundGraphNode Out;
		Out.Id = Id;
		Out.Class = Class;
		Out.Defaults = MoveTemp(Defaults);
		return Out;
	}

	FHyperAIMetaSoundConnection Wire(const TCHAR* FromNode, const TCHAR* FromPin, const TCHAR* ToNode, const TCHAR* ToPin)
	{
		FHyperAIMetaSoundConnection Out;
		Out.FromNode = FromNode;
		Out.FromPin = FromPin;
		Out.ToNode = ToNode;
		Out.ToPin = ToPin;
		return Out;
	}

	/** A blip: a sine through an AD envelope, finished when the envelope is, its pitch a graph input. */
	FHyperAIMetaSoundGraphPlanRequest BlipPlan(const FString& Path)
	{
		FHyperAIMetaSoundGraphPlanRequest Request;
		Request.Path = Path;
		FHyperAIMetaSoundGraphMember& Pitch = Request.Inputs.AddDefaulted_GetRef();
		Pitch.Name = TEXT("Pitch");
		Pitch.Type = TEXT("Float");
		Pitch.Default = TEXT("880");
		Request.Nodes = {
			GraphNode(TEXT("tone"), TEXT("UE.Sine.Audio")),
			GraphNode(TEXT("env"), TEXT("AD Envelope.AD Envelope.Audio"), {Set(TEXT("Attack Time"), TEXT("0.005")), Set(TEXT("Decay Time"), TEXT("0.3"))}),
			GraphNode(TEXT("shape"), TEXT("UE.Multiply.Audio")),
			GraphNode(TEXT("gain"), TEXT("UE.Multiply.Audio by Float"), {Set(TEXT("AdditionalOperands"), TEXT("0.5"))})};
		Request.Connections = {
			Wire(TEXT("graph"), TEXT("Pitch"), TEXT("tone"), TEXT("Frequency")),
			Wire(TEXT("graph"), TEXT("On Play"), TEXT("env"), TEXT("Trigger")),
			Wire(TEXT("tone"), TEXT("Audio"), TEXT("shape"), TEXT("PrimaryOperand")),
			Wire(TEXT("env"), TEXT("Out Envelope"), TEXT("shape"), TEXT("AdditionalOperands")),
			Wire(TEXT("shape"), TEXT("Out"), TEXT("gain"), TEXT("PrimaryOperand")),
			Wire(TEXT("gain"), TEXT("Out"), TEXT("graph"), TEXT("Out Mono")),
			Wire(TEXT("env"), TEXT("On Done"), TEXT("graph"), TEXT("On Finished"))};
		Request.bDryRun = false;
		return Request;
	}

	FHyperAIMetaSoundRecipeReport WaveRecipe(const TCHAR* Recipe, const FString& Path, const TArray<USoundWave*>& Waves, const TArray<USoundWave*>& Tails = {},
		const TCHAR* Kind = TEXT(""), const bool bDryRun = false)
	{
		FHyperAIMetaSoundRecipeRequest Request;
		Request.Recipe = Recipe;
		Request.Path = Path;
		Request.Kind = Kind;
		for (const USoundWave* Wave : Waves) Request.Waves.Add(Wave->GetPathName());
		for (const USoundWave* Wave : Tails) Request.Tails.Add(Wave->GetPathName());
		Request.bDryRun = bDryRun;
		return C::Recipe(Request);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioNodeRegistryTest, "HyperAIStudio.Audio.Authoring.NodeRegistry", HyperAIStudio::AudioAuthoringTests::Flags)

bool FHyperAIStudioAudioNodeRegistryTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioAuthoringTests;
	FHyperAIMetaSoundNodeSearchRequest Search;
	Search.Query = TEXT("wave player");
	const FHyperAIMetaSoundNodeSearchReport Players = C::NodeSearch(Search);
	TestTrue(TEXT("Searching finds the Wave Player: ") + Players.Diagnostic, Players.bOk && Players.Nodes.ContainsByPredicate(
		[](const FHyperAIMetaSoundNodeClass& Node) { return Node.Class == TEXT("UE.Wave Player.Mono"); }));
	TestTrue(TEXT("with its pins"), Players.Nodes.Num() > 0 && Players.Nodes[0].Inputs.Num() > 2 && Players.Nodes[0].Outputs.Num() > 1);
	Search.Query = TEXT("");
	Search.InputType = TEXT("WaveAsset:Array");
	const FHyperAIMetaSoundNodeSearchReport Arrays = C::NodeSearch(Search);
	TestTrue(TEXT("An input type filter keeps nodes with such a pin, Random Get among them"), Arrays.bOk && Arrays.Nodes.Num() > 0
		&& Arrays.Nodes.ContainsByPredicate([](const FHyperAIMetaSoundNodeClass& Node) { return Node.Class == TEXT("Array.Random Get.WaveAsset:Array"); })
		&& !Arrays.Nodes.ContainsByPredicate([](const FHyperAIMetaSoundNodeClass& Node) { return Node.Class == TEXT("UE.Sine.Audio"); }));
	Search.InputType.Reset();
	Search.MaxResults = 5;
	const FHyperAIMetaSoundNodeSearchReport Page = C::NodeSearch(Search);
	TestTrue(TEXT("Every node, a page at a time"), Page.bOk && Page.Nodes.Num() == 5 && Page.bTruncated && Page.TotalMatches > 100);
	Search.Query = TEXT("zzqx no such node");
	TestTrue(TEXT("Nothing matches nonsense"), C::NodeSearch(Search).TotalMatches == 0);

	FHyperAIMetaSoundNodeDescribeRequest Describe;
	Describe.Classes = {TEXT("UE.Wave Player.Mono"), TEXT("UE.Noise.Audio"), TEXT("UE.TriggerRepeat")};
	Describe.bListDataTypes = true;
	const FHyperAIMetaSoundNodeDescribeReport Described = C::NodeDescribe(Describe);
	TestTrue(TEXT("Three classes described: ") + Described.Diagnostic, Described.bOk && Described.Nodes.Num() == 3);
	if (Described.Nodes.Num() == 3)
	{
		const FHyperAIMetaSoundNodeClass& Player = Described.Nodes[0];
		const FHyperAIMetaSoundPin* Pitch = Player.Inputs.FindByPredicate([](const FHyperAIMetaSoundPin& Pin) { return Pin.Name == TEXT("Pitch Shift"); });
		TestTrue(TEXT("The Wave Player's inputs have types and defaults"), Pitch && Pitch->Type == TEXT("Float") && !Pitch->Default.IsEmpty()
			&& Player.Inputs.ContainsByPredicate([](const FHyperAIMetaSoundPin& Pin) { return Pin.Name == TEXT("Wave Asset") && Pin.Type == TEXT("WaveAsset"); }));
		const FHyperAIMetaSoundPin* Type = Described.Nodes[1].Inputs.FindByPredicate([](const FHyperAIMetaSoundPin& Pin) { return Pin.Name == TEXT("Type"); });
		TestTrue(TEXT("An enum pin lists its values"), Type && Type->EnumValues.Num() >= 2);
		TestTrue(TEXT("A class with no variant is found by Namespace.Name"), Described.Nodes[2].Class == TEXT("UE.TriggerRepeat"));
	}
	TestTrue(TEXT("The data types are listed"), Described.DataTypes.Contains(TEXT("Float")) && Described.DataTypes.Contains(TEXT("WaveAsset:Array")));
	Describe.Classes = {TEXT("UE.No Such Node.Audio")};
	Describe.bListDataTypes = false;
	const FHyperAIMetaSoundNodeDescribeReport Missing = C::NodeDescribe(Describe);
	TestTrue(TEXT("An unknown class is named as missing"), !Missing.bOk && Missing.Status == TEXT("not_found") && Missing.Diagnostic.Contains(TEXT("No Such Node")));

	FMetasoundFrontendLiteral Literal;
	FString Error;
	TestTrue(TEXT("An enum takes its name"), HyperAIStudio::AudioAuthoring::Graph::MakeLiteral(TEXT("Enum:NoiseType"), TEXT("Pink"), Literal, Error)
		|| HyperAIStudio::AudioAuthoring::Graph::MakeLiteral(TEXT("Enum:NoiseGenerator"), TEXT("0"), Literal, Error));
	TestTrue(TEXT("A float array takes a comma list"), HyperAIStudio::AudioAuthoring::Graph::MakeLiteral(TEXT("Float:Array"), TEXT("[1, 2.5, 3]"), Literal, Error)
		&& Literal.GetType() == EMetasoundFrontendLiteralType::FloatArray);
	TestFalse(TEXT("A float refuses words"), HyperAIStudio::AudioAuthoring::Graph::MakeLiteral(TEXT("Float"), TEXT("loud"), Literal, Error));
	TestFalse(TEXT("An audio pin takes no value"), HyperAIStudio::AudioAuthoring::Graph::MakeLiteral(TEXT("Audio"), TEXT("1"), Literal, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioGraphPlanTest, "HyperAIStudio.Audio.Authoring.GraphPlan", HyperAIStudio::AudioAuthoringTests::Flags)

bool FHyperAIStudioAudioGraphPlanTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioAuthoringTests;
	const FScratch Scratch;
	const FString Path = Scratch.Path(TEXT("MS_GraphBlip"));
	FHyperAIMetaSoundGraphPlanRequest Request = BlipPlan(Path);
	Request.bDryRun = true;
	const FHyperAIMetaSoundGraphPlanReport Checked = C::GraphPlan(Request);
	TestTrue(TEXT("The plan dry-runs, laid out, and makes nothing: ") + Checked.Diagnostic, Checked.bOk && Checked.Status == TEXT("checked") && Checked.bCreates
		&& Checked.Nodes.Num() == 4 && Checked.Warnings.IsEmpty() && FHyperAIStudioNames::IsFreePackage(Path));
	TestTrue(TEXT("The layout follows the signal: the gain right of the tone"), Checked.Nodes.Num() == 4 && Checked.Nodes[3].Column > Checked.Nodes[0].Column);

	auto Refused = [&](const TCHAR* What, TFunctionRef<void(FHyperAIMetaSoundGraphPlanRequest&)> Break, const TCHAR* Expect)
	{
		FHyperAIMetaSoundGraphPlanRequest Broken = BlipPlan(Scratch.Path(TEXT("MS_Broken")));
		Broken.bDryRun = true;
		Break(Broken);
		const FHyperAIMetaSoundGraphPlanReport Report = C::GraphPlan(Broken);
		TestTrue(FString(What) + TEXT(": ") + Report.Diagnostic, !Report.bOk && Report.Status == TEXT("invalid_request") && Report.Diagnostic.Contains(Expect));
	};
	Refused(TEXT("An unknown class"), [](FHyperAIMetaSoundGraphPlanRequest& R) { R.Nodes[0].Class = TEXT("UE.Sinus.Audio"); }, TEXT("not a registered"));
	Refused(TEXT("An unknown pin"), [](FHyperAIMetaSoundGraphPlanRequest& R) { R.Connections[2].FromPin = TEXT("Wave"); }, TEXT("no output Wave"));
	Refused(TEXT("Two types joined"), [](FHyperAIMetaSoundGraphPlanRequest& R) { R.Connections[2].ToNode = TEXT("gain"); R.Connections[2].ToPin = TEXT("AdditionalOperands"); }, TEXT("joins a Audio to a Float"));
	Refused(TEXT("An input fed twice"), [](FHyperAIMetaSoundGraphPlanRequest& R) { R.Connections.Add(Wire(TEXT("tone"), TEXT("Audio"), TEXT("shape"), TEXT("PrimaryOperand"))); }, TEXT("already fed"));
	Refused(TEXT("A value its pin cannot take"), [](FHyperAIMetaSoundGraphPlanRequest& R) { R.Nodes[1].Defaults[0].Value = TEXT("soon"); }, TEXT("takes a number"));
	Refused(TEXT("A default on a pin the node lacks"), [](FHyperAIMetaSoundGraphPlanRequest& R) { R.Nodes[1].Defaults.Add(Set(TEXT("Sustain"), TEXT("1"))); }, TEXT("no input Sustain"));
	Refused(TEXT("A graph member the source lacks"), [](FHyperAIMetaSoundGraphPlanRequest& R) { R.Connections[5].ToPin = TEXT("Out Left"); }, TEXT("no output"));
	Refused(TEXT("A node id twice"), [](FHyperAIMetaSoundGraphPlanRequest& R) { R.Nodes[1].Id = TEXT("tone"); }, TEXT("Id of its own"));
	{
		FHyperAIMetaSoundGraphPlanRequest Loose = BlipPlan(Scratch.Path(TEXT("MS_Loose")));
		Loose.bDryRun = true;
		Loose.Connections.RemoveAt(6);
		const FHyperAIMetaSoundGraphPlanReport Report = C::GraphPlan(Loose);
		TestTrue(TEXT("A one-shot whose On Finished is not wired is warned of"), Report.bOk && Report.Warnings.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("never finishes")); }));
	}

	// With approval on, the plan waits and makes nothing until it is approved.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	Settings->bRequireApprovalForAgentEdits = true;
	Request.bDryRun = false;
	const FHyperAIMetaSoundGraphPlanReport Held = C::GraphPlan(Request);
	TestTrue(TEXT("With approval on, the plan waits: ") + Held.Diagnostic, Held.bOk && Held.Status == TEXT("awaiting_user_approval") && FHyperAIStudioApprovalGate::HasPending(Held.OperationId));
	FHyperAIStudioApprovalGate::Reject(Held.OperationId, TEXT("Not now."));
	TestTrue(TEXT("Rejected, nothing is made"), FHyperAIStudioNames::IsFreePackage(Path));
	Settings->bRequireApprovalForAgentEdits = false;

	const FHyperAIMetaSoundGraphPlanReport Built = C::GraphPlan(Request);
	UMetaSoundSource* Source = Cast<UMetaSoundSource>(HyperAIStudio::AudioAuthoring::CueGraph::LoadAsset(Path, *UMetaSoundSource::StaticClass()));
	TestTrue(TEXT("The plan builds a MetaSound Source: ") + Built.Diagnostic, Built.bOk && Built.Status == TEXT("completed") && Source != nullptr);
	if (!Source)
	{
		return false;
	}
	TestTrue(TEXT("Each node reports its GUID"), Built.Nodes.Num() == 4 && Built.Nodes.FindByPredicate([](const FHyperAIMetaSoundBuiltNode& Node) { return Node.NodeId.IsEmpty(); }) == nullptr);
	const FHyperAISoundAssetInfo Read = Inspect(Path);
	const int32 NodeLines = Read.Details.FilterByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("node ")); }).Num();
	TestTrue(TEXT("Inspect shows its input, nodes and wires: ") + FString::Join(Read.Details, TEXT(" | ")), Read.Details.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("input Pitch (Float)")); })
		&& NodeLines == 4 && Read.Details.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("wire ")) && Line.Contains(TEXT("Frequency")); }));

	// It sounds: an 880 Hz tone a little under -6 dBFS, finished in about 0.3 s.
	const FHyperAIAudioAnalyzeReport Heard = Analyze(Path, 1, 2.f);
	TestTrue(TEXT("Rendered offline it is heard: ") + Heard.Diagnostic, Heard.bOk && Heard.Plays.Num() == 1 && !Heard.Plays[0].bSilent && Heard.Method == TEXT("metasound_offline"));
	if (Heard.Plays.Num() == 1)
	{
		const FHyperAIAudioMeasure& M = Heard.Plays[0];
		TestTrue(FString::Printf(TEXT("its centroid is near 880 Hz (%.0f)"), M.SpectralCentroidHz), FMath::Abs(M.SpectralCentroidHz - 880.f) < 120.f);
		TestTrue(FString::Printf(TEXT("it finishes about 0.3 s in (%.2f s)"), M.DurationSeconds), M.bFinished && M.DurationSeconds > 0.2f && M.DurationSeconds < 0.6f);
		TestTrue(FString::Printf(TEXT("its peak is about -6 dBFS (%.1f)"), M.PeakDb), M.PeakDb < -4.f && M.PeakDb > -9.f && M.ClippedSamples == 0);
	}
	const FHyperAIAudioAnalyzeReport Lower = Analyze(Path, 1, 2.f, {Set(TEXT("Pitch"), TEXT("220"))});
	TestTrue(TEXT("A graph input set for the render moves the centroid down"), Lower.bOk && Lower.Plays.Num() == 1 && Heard.Plays.Num() == 1
		&& Lower.Plays[0].SpectralCentroidHz < Heard.Plays[0].SpectralCentroidHz * 0.5f);

	// Adding to the source: one more node on an existing one, in one undo step.
	if (GEditor && GEditor->Trans)
	{
		FHyperAIMetaSoundGraphPlanRequest Add;
		Add.Path = Path;
		Add.bDryRun = false;
		Add.bSave = false;
		const FString* ToneLine = Read.Details.FindByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("node ")) && Line.EndsWith(TEXT("UE.Sine.Audio")); });
		const FString ToneId = ToneLine ? ToneLine->Mid(5, 36) : FString();
		Add.Nodes = {GraphNode(TEXT("lfo"), TEXT("UE.LFO.Audio"), {Set(TEXT("Frequency"), TEXT("3"))}), GraphNode(*ToneId, TEXT(""))};
		const FHyperAIMetaSoundGraphPlanReport Added = C::GraphPlan(Add);
		const auto Count = [&Path]() { return Inspect(Path).Details.FilterByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("node ")); }).Num(); };
		TestTrue(TEXT("A node is added to the existing source: ") + Added.Diagnostic + TEXT(" ") + FString::Join(Inspect(Path).Details, TEXT(" | ")), Added.bOk && !Added.bCreates && Count() == 5);
		TestTrue(TEXT("Edit > Undo takes it back"), GEditor->UndoTransaction() && Count() == 4);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioAnalyzeTest, "HyperAIStudio.Audio.Authoring.Analyze", HyperAIStudio::AudioAuthoringTests::Flags)

bool FHyperAIStudioAudioAnalyzeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioAuthoringTests;
	const FScratch Scratch;
	// A 1 kHz sine at 0.5 for 2 s: -6.02 dBFS peak, and by BS.1770 -6.02 - 3.01 = -9.03 LUFS (K-weighting is flat at 1 kHz but for +0.69 dB, which its -0.691 offset cancels).
	USoundWave* Tone = ImportSamples(Scratch.Folder, TEXT("SW_Tone1k"), Sine(1000.f, 2.f, 0.5f));
	USoundWave* Thud = ImportSamples(Scratch.Folder, TEXT("SW_Thud"), Step(3, 0.6f));
	if (!TestTrue(TEXT("Waves import"), Tone && Thud))
	{
		return false;
	}
	const FHyperAIAudioAnalyzeReport Sine1k = Analyze(Tone->GetPathName());
	TestTrue(TEXT("A SoundWave is read from its samples: ") + Sine1k.Diagnostic, Sine1k.bOk && Sine1k.Method == TEXT("sound_wave_pcm") && Sine1k.Plays.Num() == 1);
	if (Sine1k.Plays.Num() == 1)
	{
		const FHyperAIAudioMeasure& M = Sine1k.Plays[0];
		TestTrue(FString::Printf(TEXT("peak -6.0 dBFS (%.2f)"), M.PeakDb), FMath::Abs(M.PeakDb + 6.02f) < 0.2f);
		TestTrue(FString::Printf(TEXT("integrated loudness -9.0 LUFS (%.2f)"), M.IntegratedLufs), FMath::Abs(M.IntegratedLufs + 9.03f) < 0.4f);
		TestTrue(FString::Printf(TEXT("centroid 1 kHz (%.0f)"), M.SpectralCentroidHz), FMath::Abs(M.SpectralCentroidHz - 1000.f) < 60.f);
		TestTrue(FString::Printf(TEXT("onset at once (%.1f ms), lasting 2 s (%.2f)"), M.OnsetMs, M.DurationSeconds), M.OnsetMs < 2.f && FMath::Abs(M.DurationSeconds - 2.f) < 0.01f);
		TestTrue(TEXT("no clipping"), M.ClippedSamples == 0 && !M.bSilent);
	}
	const FHyperAIAudioAnalyzeReport Thump = Analyze(Thud->GetPathName());
	TestTrue(TEXT("A thud is dark and decays: ") + Thump.Diagnostic, Thump.bOk && Thump.Plays.Num() == 1 && Thump.Plays[0].SpectralCentroidHz < 1500.f
		&& Thump.Plays[0].TailSeconds > 0.1f && Thump.Plays[0].TailSeconds < 0.6f);
	const FHyperAIAudioAnalyzeReport Loud = Analyze(ImportSamples(Scratch.Folder, TEXT("SW_Hot"), Sine(200.f, 0.5f, 1.2f))->GetPathName());
	TestTrue(TEXT("A clipped wave is reported as clipping"), Loud.bOk && Loud.Issues.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("clipping")); }));
	TestTrue(TEXT("A sound that is not there is not found"), Analyze(Scratch.Path(TEXT("SC_None"))).Status == TEXT("not_found"));
	FHyperAIAudioAnalyzeRequest Empty;
	TestTrue(TEXT("A request without a sound is refused"), !C::Analyze(Empty).bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioWaveRecipesTest, "HyperAIStudio.Audio.Authoring.WaveRecipes", HyperAIStudio::AudioAuthoringTests::Flags)

bool FHyperAIStudioAudioWaveRecipesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioAuthoringTests;
	const FScratch Scratch;
	TArray<USoundWave*> Steps;
	for (int32 Seed = 1; Seed <= 4; ++Seed)
	{
		Steps.Add(ImportSamples(Scratch.Folder, FString::Printf(TEXT("SW_Step_%02d"), Seed), Step(Seed)));
	}
	USoundWave* HitA = ImportSamples(Scratch.Folder, TEXT("SW_HitA"), Burst(11, 0.12f, 40.f, 0.9f));
	USoundWave* HitB = ImportSamples(Scratch.Folder, TEXT("SW_HitB"), Burst(12, 0.12f, 45.f, 0.8f));
	USoundWave* Tail = ImportSamples(Scratch.Folder, TEXT("SW_Tail"), Burst(13, 1.2f, 4.f, 0.05f));
	USoundWave* BedA = ImportSamples(Scratch.Folder, TEXT("SW_BedA"), Burst(21, 3.f, 0.f, 0.1f));
	USoundWave* BedB = ImportSamples(Scratch.Folder, TEXT("SW_BedB"), Burst(22, 3.f, 0.f, 0.3f));
	if (!TestTrue(TEXT("The takes import"), !Steps.Contains(nullptr) && HitA && HitB && Tail && BedA && BedB))
	{
		return false;
	}

	FHyperAIMetaSoundRecipeRequest List;
	const FHyperAIMetaSoundRecipeReport Listed = C::Recipe(List);
	TestTrue(TEXT("The four wave recipes are listed, with a Sound Cue version each"), Listed.Recipes.FilterByPredicate([](const FHyperAIMetaSoundRecipeInfo& Info) { return Info.bUsesWaves && Info.bSoundCue; }).Num() == 4);

	// Footsteps: a different take most steps, a pitch and level that move, and each step finishes.
	const FString Footsteps = Scratch.Path(TEXT("MS_Footsteps"));
	TestTrue(TEXT("footstep dry-runs"), WaveRecipe(TEXT("footstep"), Footsteps, Steps, {}, TEXT(""), true).Status == TEXT("checked"));
	const FHyperAIMetaSoundRecipeReport Made = WaveRecipe(TEXT("footstep"), Footsteps, Steps);
	TestTrue(TEXT("footstep builds: ") + Made.Diagnostic, Made.bOk && Made.NodeCount >= 8);
	const FHyperAIAudioAnalyzeReport Walk = Analyze(Footsteps, 6, 2.f);
	TestTrue(TEXT("Six steps render, none silent, each finished: ") + Walk.Diagnostic, Walk.bOk && Walk.Plays.Num() == 6
		&& !Walk.Plays.ContainsByPredicate([](const FHyperAIAudioMeasure& M) { return M.bSilent || !M.bFinished; }));
	TestTrue(FString::Printf(TEXT("and vary (%.2f LU, %.0f Hz)"), Walk.LoudnessSpreadLu, Walk.CentroidSpreadHz), Walk.LoudnessSpreadLu > 0.2f || Walk.CentroidSpreadHz > 5.f);
	const FHyperAIAudioAnalyzeReport Still = Analyze(Footsteps, 3, 2.f, {Set(TEXT("Pitch Jitter"), TEXT("0")), Set(TEXT("Volume Jitter"), TEXT("0"))});
	TestTrue(TEXT("Rendered with its inputs set, it still plays"), Still.bOk && Still.Plays.Num() == 3 && !Still.Plays[0].bSilent);

	// Impact: the tail keeps it ringing well past the transient.
	const FString Impact = Scratch.Path(TEXT("MS_Impact"));
	const FHyperAIMetaSoundRecipeReport Hit = WaveRecipe(TEXT("impact"), Impact, {HitA, HitB}, {Tail});
	TestTrue(TEXT("impact builds: ") + Hit.Diagnostic, Hit.bOk);
	const FHyperAIAudioAnalyzeReport Crash = Analyze(Impact, 3, 3.f);
	TestTrue(TEXT("impact renders and finishes: ") + Crash.Diagnostic, Crash.bOk && Crash.Plays.Num() == 3 && !Crash.Plays[0].bSilent && Crash.Plays[0].bFinished);
	TestTrue(FString::Printf(TEXT("with a tail of more than half a second (%.2f s)"), Crash.Plays.Num() ? Crash.Plays[0].TailSeconds : 0.f), Crash.Plays.Num() && Crash.Plays[0].TailSeconds > 0.5f);
	TestTrue(TEXT("A transient-only impact builds too"), WaveRecipe(TEXT("impact"), Scratch.Path(TEXT("MS_Tap")), {HitA}).bOk);

	// Whoosh: from takes, and from noise alone.
	TestTrue(TEXT("whoosh builds from takes"), WaveRecipe(TEXT("whoosh"), Scratch.Path(TEXT("MS_WhooshTakes")), {BedA}).bOk);
	const FString Whoosh = Scratch.Path(TEXT("MS_Whoosh"));
	TestTrue(TEXT("whoosh builds from noise"), WaveRecipe(TEXT("whoosh"), Whoosh, {}).bOk);
	const FHyperAIAudioAnalyzeReport Swish = Analyze(Whoosh, 1, 3.f);
	TestTrue(TEXT("and sounds, finishing about 0.6 s in: ") + Swish.Diagnostic, Swish.bOk && Swish.Plays.Num() == 1 && !Swish.Plays[0].bSilent && Swish.Plays[0].bFinished
		&& Swish.Plays[0].DurationSeconds < 1.2f);

	// Crossfade loop: never finishes, never silent once it has faded in.
	const FString Bed = Scratch.Path(TEXT("MS_Bed"));
	TestTrue(TEXT("crossfade_loop builds"), WaveRecipe(TEXT("crossfade_loop"), Bed, {BedA, BedB}).bOk);
	const FHyperAIAudioAnalyzeReport Ambience = Analyze(Bed, 1, 8.f, {Set(TEXT("Segment"), TEXT("1"))});
	TestTrue(TEXT("crossfade_loop plays on past its beds without finishing: ") + Ambience.Diagnostic, Ambience.bOk && Ambience.Plays.Num() == 1 && !Ambience.Plays[0].bSilent
		&& !Ambience.Plays[0].bFinished && Ambience.Plays[0].DurationSeconds > 7.9f);

	// The Sound Cue versions.
	const FString StepCue = Scratch.Path(TEXT("SC_Footsteps"));
	const FHyperAIMetaSoundRecipeReport Cue = WaveRecipe(TEXT("footstep"), StepCue, Steps, {}, TEXT("sound_cue"));
	USoundCue* Steps1 = LoadCue(StepCue);
	TestTrue(TEXT("footstep as a Sound Cue: ") + Cue.Diagnostic, Cue.bOk && Steps1 && Steps1->FirstNode && Steps1->FirstNode->IsA<USoundNodeModulator>() && Steps1->AllNodes.Num() == 6);
	const FString HitCue = Scratch.Path(TEXT("SC_Impact"));
	TestTrue(TEXT("impact as a Sound Cue"), WaveRecipe(TEXT("impact"), HitCue, {HitA, HitB}, {Tail}, TEXT("sound_cue")).bOk && LoadCue(HitCue) && LoadCue(HitCue)->AllNodes.Num() == 9);
	TestTrue(TEXT("crossfade_loop as a Sound Cue"), WaveRecipe(TEXT("crossfade_loop"), Scratch.Path(TEXT("SC_Bed")), {BedA, BedB}, {}, TEXT("sound_cue")).bOk);

	// Refusals.
	TestTrue(TEXT("A wave recipe without waves is refused"), WaveRecipe(TEXT("footstep"), Scratch.Path(TEXT("MS_NoSteps")), {}).Status == TEXT("invalid_request"));
	TestTrue(TEXT("A procedural recipe refuses waves"), WaveRecipe(TEXT("wind_loop"), Scratch.Path(TEXT("MS_WindWaves")), Steps).Status == TEXT("invalid_request"));
	TestTrue(TEXT("Only impact takes tails"), WaveRecipe(TEXT("footstep"), Scratch.Path(TEXT("MS_StepTails")), Steps, {Tail}).Status == TEXT("invalid_request"));
	TestTrue(TEXT("A procedural recipe has no Sound Cue version"), WaveRecipe(TEXT("ui_blip"), Scratch.Path(TEXT("SC_Blip")), {}, {}, TEXT("sound_cue")).Status == TEXT("invalid_request"));
	FHyperAIMetaSoundRecipeRequest Bad;
	Bad.Recipe = TEXT("footstep");
	Bad.Path = Scratch.Path(TEXT("MS_BadWave"));
	Bad.Waves = {Scratch.Path(TEXT("SW_Missing"))};
	TestTrue(TEXT("A missing wave is named"), C::Recipe(Bad).Diagnostic.Contains(TEXT("SW_Missing")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioPreviewTest, "HyperAIStudio.Audio.Authoring.Preview", HyperAIStudio::AudioAuthoringTests::Flags)

bool FHyperAIStudioAudioPreviewTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioAuthoringTests;
	const FScratch Scratch;
	USoundWave* Tone = ImportSamples(Scratch.Folder, TEXT("SW_PreviewTone"), Sine(440.f, 0.5f, 0.3f));
	if (!TestNotNull(TEXT("A wave imports"), Tone))
	{
		return false;
	}
	FHyperAIAudioPreviewRequest Request;
	Request.Action = TEXT("status");
	const FHyperAIAudioPreviewReport Status = C::Preview(Request);
	TestTrue(TEXT("Status answers: ") + Status.Diagnostic, Status.bOk && Status.Status == TEXT("ok"));
	Request.Action = TEXT("dance");
	TestTrue(TEXT("An unknown action is refused"), C::Preview(Request).Status == TEXT("invalid_request"));
	Request.Action = TEXT("play");
	Request.Sound = Tone->GetPathName();
	Request.Parameters = {Set(TEXT("Pitch"), TEXT("loud"))};
	const FHyperAIAudioPreviewReport Played = C::Preview(Request);
	if (Status.bAudioAvailable)
	{
		TestTrue(TEXT("A bad parameter is refused"), Played.Status == TEXT("invalid_request"));
		Request.Parameters = {Set(TEXT("Gain"), TEXT("0.5"))};
		const FHyperAIAudioPreviewReport Playing = C::Preview(Request);
		TestTrue(TEXT("With a device it plays: ") + Playing.Diagnostic, Playing.bOk && Playing.bPreviewing);
		Request.Action = TEXT("mute");
		Request.Target = Tone->GetPathName();
		TestTrue(TEXT("and mutes a wave"), C::Preview(Request).Muted.Contains(Tone->GetPathName()));
		Request.Action = TEXT("clear");
		TestTrue(TEXT("and clears it"), C::Preview(Request).Muted.IsEmpty());
	}
	else
	{
		TestTrue(TEXT("Without a device play says so: ") + Played.Diagnostic, !Played.bOk && Played.Status == TEXT("no_audio_device") && !Played.bAudioAvailable);
		Request.Action = TEXT("solo");
		Request.Target = Tone->GetPathName();
		TestTrue(TEXT("and so does solo"), C::Preview(Request).Status == TEXT("no_audio_device"));
	}
	Request.Action = TEXT("stop");
	const FHyperAIAudioPreviewReport Stopped = C::Preview(Request);
	TestTrue(TEXT("Stop always answers, leaving nothing playing"), Stopped.bOk && !Stopped.bPreviewing);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
