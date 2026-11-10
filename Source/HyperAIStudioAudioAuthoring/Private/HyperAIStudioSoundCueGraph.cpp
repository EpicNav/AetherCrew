// Games by Hyper 2026.

#include "HyperAIStudioSoundCueGraph.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "HyperAIStudioNames.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeAttenuation.h"
#include "Sound/SoundNodeBranch.h"
#include "Sound/SoundNodeConcatenator.h"
#include "Sound/SoundNodeDelay.h"
#include "Sound/SoundNodeEnveloper.h"
#include "Sound/SoundNodeLooping.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeModulator.h"
#include "Sound/SoundNodeModulatorContinuous.h"
#include "Sound/SoundNodeParamCrossFade.h"
#include "Sound/SoundNodeRandom.h"
#include "Sound/SoundNodeSwitch.h"
#include "Sound/SoundNodeWaveParam.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::AudioAuthoring::CueGraph
{
	namespace
	{
		using C = FHyperAIStudioAudioAuthoringContracts;

		/** Columns and rows of the layout, in graph units: wide enough for a node's title, tall enough for a wave's. */
		constexpr int32 ColumnWidth = 320;
		constexpr int32 RowHeight = 110;
		constexpr int32 MaxCurvePoints = 64;

		struct FKind
		{
			const TCHAR* Name;
			UClass* (*Class)();
		};

		template <typename T>
		UClass* ClassOf() { return T::StaticClass(); }

		const TArray<FKind>& KindTable()
		{
			static const TArray<FKind> Table = {
				{TEXT("wave_player"), &ClassOf<USoundNodeWavePlayer>},
				{TEXT("random"), &ClassOf<USoundNodeRandom>},
				{TEXT("modulator"), &ClassOf<USoundNodeModulator>},
				{TEXT("mixer"), &ClassOf<USoundNodeMixer>},
				{TEXT("concatenator"), &ClassOf<USoundNodeConcatenator>},
				{TEXT("looping"), &ClassOf<USoundNodeLooping>},
				{TEXT("delay"), &ClassOf<USoundNodeDelay>},
				{TEXT("attenuation"), &ClassOf<USoundNodeAttenuation>},
				{TEXT("enveloper"), &ClassOf<USoundNodeEnveloper>},
				{TEXT("branch"), &ClassOf<USoundNodeBranch>},
				{TEXT("switch"), &ClassOf<USoundNodeSwitch>},
				// Before its parent class's kind would be: a parameter cross-fade is a distance cross-fade underneath.
				{TEXT("param_crossfade"), &ClassOf<USoundNodeParamCrossFade>},
				{TEXT("continuous_modulator"), &ClassOf<USoundNodeModulatorContinuous>},
				{TEXT("wave_param"), &ClassOf<USoundNodeWaveParam>},
			};
			return Table;
		}

		UClass* ClassForKind(const FString& Kind)
		{
			for (const FKind& Entry : KindTable())
			{
				if (Kind == Entry.Name) return Entry.Class();
			}
			return nullptr;
		}

		/** Inputs the kind takes: its class's own limits, and at least one for every kind that has any. */
		void InputLimits(const FString& Kind, int32& OutMin, int32& OutMax)
		{
			const UClass* Class = ClassForKind(Kind);
			const USoundNode* Default = Class ? Class->GetDefaultObject<USoundNode>() : nullptr;
			OutMax = Default ? Default->GetMaxChildNodes() : 0;
			OutMin = OutMax > 0 ? FMath::Max(1, Default->GetMinChildNodes()) : 0;
		}

		bool AllowsEmptyInput(const FString& Kind)
		{
			// A branch or switch input left empty plays nothing for that value, which is a choice; anywhere else it is a hole.
			return Kind == TEXT("branch") || Kind == TEXT("switch");
		}

		FString Num(const float Value)
		{
			FString Text = FString::SanitizeFloat(Value);
			Text.RemoveFromEnd(TEXT(".0"));
			return Text;
		}

		FString Range(const float Min, const float Max)
		{
			return FMath::IsNearlyEqual(Min, Max) ? Num(Min) : Num(Min) + TEXT("-") + Num(Max);
		}

		FString WavePathOf(const USoundNodeWavePlayer& Player)
		{
			if (const USoundWave* Wave = Player.GetSoundWave())
			{
				return Wave->GetPathName();
			}
			// The soft reference is private; it is read the way the Details panel reads it.
			if (const FSoftObjectProperty* Property = FindFProperty<FSoftObjectProperty>(USoundNodeWavePlayer::StaticClass(), TEXT("SoundWaveAssetPtr")))
			{
				return Property->GetPropertyValue_InContainer(&Player).ToSoftObjectPath().ToString();
			}
			return FString();
		}

		FString DetailOf(const FHyperAISoundCueNode& Node)
		{
			const FString& Kind = Node.Kind;
			if (Kind == TEXT("wave_player")) return Node.Wave + (Node.bLooping ? TEXT(", looping") : TEXT(""));
			if (Kind == TEXT("random"))
			{
				FString Text = Node.Weights.IsEmpty() ? FString(TEXT("even weights")) : TEXT("weights ") + FString::JoinBy(Node.Weights, TEXT("/"), [](const float W) { return Num(W); });
				return Text + (Node.bNoRepeat ? TEXT(", no repeats") : TEXT(""));
			}
			if (Kind == TEXT("modulator") || Kind == TEXT("enveloper"))
			{
				return FString::Printf(TEXT("pitch %s, volume %s"), *Range(Node.PitchMin, Node.PitchMax), *Range(Node.VolumeMin, Node.VolumeMax));
			}
			if (Kind == TEXT("mixer") || Kind == TEXT("concatenator"))
			{
				return Node.Weights.IsEmpty() ? FString(TEXT("inputs at 1")) : TEXT("input volumes ") + FString::JoinBy(Node.Weights, TEXT("/"), [](const float W) { return Num(W); });
			}
			if (Kind == TEXT("looping")) return Node.LoopCount <= 0 ? FString(TEXT("forever")) : FString::Printf(TEXT("%d times"), Node.LoopCount);
			if (Kind == TEXT("delay")) return Range(Node.DelayMin, Node.DelayMax) + TEXT(" s");
			if (Kind == TEXT("attenuation")) return Node.Attenuation;
			if (Kind == TEXT("continuous_modulator"))
			{
				return FString::Printf(TEXT("volume by %s, pitch by %s"), Node.Parameter.IsEmpty() ? TEXT("-") : *Node.Parameter,
					Node.PitchParameter.IsEmpty() ? TEXT("-") : *Node.PitchParameter);
			}
			return Node.Parameter.IsEmpty() ? FString() : TEXT("parameter ") + Node.Parameter;
		}

		void Fail(FString& OutError, const FString& Why)
		{
			OutError = Why;
		}

		void SetCurve(FRuntimeFloatCurve& Curve, const TArray<FVector2D>& Points)
		{
			FRichCurve* Rich = Curve.GetRichCurve();
			Rich->Reset();
			for (const FVector2D& Point : Points)
			{
				Rich->AddKey(static_cast<float>(Point.X), static_cast<float>(Point.Y));
			}
		}

		TArray<FVector2D> CurvePoints(const FRuntimeFloatCurve& Curve)
		{
			TArray<FVector2D> Points;
			if (const FRichCurve* Rich = Curve.GetRichCurveConst())
			{
				for (const FRichCurveKey& Key : Rich->GetConstRefOfKeys())
				{
					Points.Add(FVector2D(Key.Time, Key.Value));
				}
			}
			return Points;
		}

		/** Gives a new node of its kind the plan's settings. Its inputs exist already, so per-input arrays are sized. */
		void Configure(USoundNode& Node, const FHyperAISoundCueNode& Spec, USoundWave* Wave, USoundAttenuation* Attenuation)
		{
			const int32 Inputs = Node.ChildNodes.Num();
			if (USoundNodeWavePlayer* Player = Cast<USoundNodeWavePlayer>(&Node))
			{
				Player->SetSoundWave(Wave);
				Player->bLooping = Spec.bLooping;
			}
			else if (USoundNodeRandom* Random = Cast<USoundNodeRandom>(&Node))
			{
				for (int32 Index = 0; Index < Inputs && Index < Random->Weights.Num(); ++Index)
				{
					Random->Weights[Index] = Spec.Weights.IsValidIndex(Index) ? Spec.Weights[Index] : 1.f;
				}
				Random->bRandomizeWithoutReplacement = Spec.bNoRepeat;
				Random->PreselectAtLevelLoad = Spec.PreselectAtLevelLoad;
			}
			else if (USoundNodeModulator* Modulator = Cast<USoundNodeModulator>(&Node))
			{
				Modulator->PitchMin = Spec.PitchMin;
				Modulator->PitchMax = Spec.PitchMax;
				Modulator->VolumeMin = Spec.VolumeMin;
				Modulator->VolumeMax = Spec.VolumeMax;
			}
			else if (USoundNodeMixer* Mixer = Cast<USoundNodeMixer>(&Node))
			{
				for (int32 Index = 0; Index < Mixer->InputVolume.Num(); ++Index)
				{
					Mixer->InputVolume[Index] = Spec.Weights.IsValidIndex(Index) ? Spec.Weights[Index] : 1.f;
				}
			}
			else if (USoundNodeConcatenator* Concatenator = Cast<USoundNodeConcatenator>(&Node))
			{
				for (int32 Index = 0; Index < Concatenator->InputVolume.Num(); ++Index)
				{
					Concatenator->InputVolume[Index] = Spec.Weights.IsValidIndex(Index) ? Spec.Weights[Index] : 1.f;
				}
			}
			else if (USoundNodeLooping* Looping = Cast<USoundNodeLooping>(&Node))
			{
				Looping->bLoopIndefinitely = Spec.LoopCount <= 0;
				Looping->LoopCount = FMath::Max(1, Spec.LoopCount);
			}
			else if (USoundNodeDelay* Delay = Cast<USoundNodeDelay>(&Node))
			{
				Delay->DelayMin = Spec.DelayMin;
				Delay->DelayMax = Spec.DelayMax;
			}
			else if (USoundNodeAttenuation* AttenuationNode = Cast<USoundNodeAttenuation>(&Node))
			{
				AttenuationNode->AttenuationSettings = Attenuation;
				AttenuationNode->bOverrideAttenuation = false;
			}
			else if (USoundNodeEnveloper* Enveloper = Cast<USoundNodeEnveloper>(&Node))
			{
				SetCurve(Enveloper->VolumeCurve, Spec.VolumeCurve);
				SetCurve(Enveloper->PitchCurve, Spec.PitchCurve);
				Enveloper->LoopStart = Spec.LoopStart;
				Enveloper->LoopEnd = Spec.LoopEnd;
				Enveloper->DurationAfterLoop = Spec.DurationAfterLoop;
				Enveloper->bLoop = Spec.LoopEnd > Spec.LoopStart;
				Enveloper->bLoopIndefinitely = Enveloper->bLoop && Spec.LoopCount <= 0;
				Enveloper->LoopCount = FMath::Max(0, Spec.LoopCount);
				Enveloper->PitchMin = Spec.PitchMin;
				Enveloper->PitchMax = Spec.PitchMax;
				Enveloper->VolumeMin = Spec.VolumeMin;
				Enveloper->VolumeMax = Spec.VolumeMax;
			}
			else if (USoundNodeBranch* Branch = Cast<USoundNodeBranch>(&Node))
			{
				Branch->BoolParameterName = FName(*Spec.Parameter);
			}
			else if (USoundNodeSwitch* Switch = Cast<USoundNodeSwitch>(&Node))
			{
				Switch->IntParameterName = FName(*Spec.Parameter);
			}
			else if (USoundNodeParamCrossFade* CrossFade = Cast<USoundNodeParamCrossFade>(&Node))
			{
				CrossFade->ParamName = FName(*Spec.Parameter);
				for (int32 Index = 0; Index < CrossFade->CrossFadeInput.Num(); ++Index)
				{
					const FHyperAISoundCueFade Fade = Spec.Fades.IsValidIndex(Index) ? Spec.Fades[Index] : FHyperAISoundCueFade();
					FDistanceDatum& Datum = CrossFade->CrossFadeInput[Index];
					Datum.FadeInDistanceStart = Fade.FadeInStart;
					Datum.FadeInDistanceEnd = Fade.FadeInEnd;
					Datum.FadeOutDistanceStart = Fade.FadeOutStart;
					Datum.FadeOutDistanceEnd = Fade.FadeOutEnd;
					Datum.Volume = Fade.Volume;
				}
			}
			else if (USoundNodeModulatorContinuous* Continuous = Cast<USoundNodeModulatorContinuous>(&Node))
			{
				// Each parameter runs 0 to 1 onto its output range; unset, it sits at 1, the top of the range.
				if (!Spec.Parameter.IsEmpty())
				{
					Continuous->VolumeModulationParams.ParameterName = FName(*Spec.Parameter);
					Continuous->VolumeModulationParams.MinOutput = Spec.VolumeMin;
					Continuous->VolumeModulationParams.MaxOutput = Spec.VolumeMax;
				}
				if (!Spec.PitchParameter.IsEmpty())
				{
					Continuous->PitchModulationParams.ParameterName = FName(*Spec.PitchParameter);
					Continuous->PitchModulationParams.MinOutput = Spec.PitchMin;
					Continuous->PitchModulationParams.MaxOutput = Spec.PitchMax;
				}
			}
			else if (USoundNodeWaveParam* WaveParam = Cast<USoundNodeWaveParam>(&Node))
			{
				WaveParam->WaveParameterName = FName(*Spec.Parameter);
			}
		}

		/** A built node back into a plan node: what a plan would have to say to build it again. */
		FHyperAISoundCueNode Unconfigure(const USoundNode& Node, const FString& Kind)
		{
			FHyperAISoundCueNode Spec;
			Spec.Kind = Kind;
			if (const USoundNodeWavePlayer* Player = Cast<USoundNodeWavePlayer>(&Node))
			{
				Spec.Wave = WavePathOf(*Player);
				Spec.bLooping = Player->bLooping != 0;
			}
			else if (const USoundNodeRandom* Random = Cast<USoundNodeRandom>(&Node))
			{
				Spec.Weights = Random->Weights;
				Spec.bNoRepeat = Random->bRandomizeWithoutReplacement != 0;
				Spec.PreselectAtLevelLoad = Random->PreselectAtLevelLoad;
			}
			else if (const USoundNodeModulator* Modulator = Cast<USoundNodeModulator>(&Node))
			{
				Spec.PitchMin = Modulator->PitchMin;
				Spec.PitchMax = Modulator->PitchMax;
				Spec.VolumeMin = Modulator->VolumeMin;
				Spec.VolumeMax = Modulator->VolumeMax;
			}
			else if (const USoundNodeMixer* Mixer = Cast<USoundNodeMixer>(&Node))
			{
				Spec.Weights = Mixer->InputVolume;
			}
			else if (const USoundNodeConcatenator* Concatenator = Cast<USoundNodeConcatenator>(&Node))
			{
				Spec.Weights = Concatenator->InputVolume;
			}
			else if (const USoundNodeLooping* Looping = Cast<USoundNodeLooping>(&Node))
			{
				Spec.LoopCount = Looping->bLoopIndefinitely ? 0 : Looping->LoopCount;
			}
			else if (const USoundNodeDelay* Delay = Cast<USoundNodeDelay>(&Node))
			{
				Spec.DelayMin = Delay->DelayMin;
				Spec.DelayMax = Delay->DelayMax;
			}
			else if (const USoundNodeAttenuation* AttenuationNode = Cast<USoundNodeAttenuation>(&Node))
			{
				Spec.Attenuation = AttenuationNode->AttenuationSettings ? AttenuationNode->AttenuationSettings->GetPathName() : FString();
			}
			else if (const USoundNodeEnveloper* Enveloper = Cast<USoundNodeEnveloper>(&Node))
			{
				Spec.VolumeCurve = CurvePoints(Enveloper->VolumeCurve);
				Spec.PitchCurve = CurvePoints(Enveloper->PitchCurve);
				Spec.LoopStart = Enveloper->LoopStart;
				Spec.LoopEnd = Enveloper->bLoop ? Enveloper->LoopEnd : Enveloper->LoopStart;
				Spec.DurationAfterLoop = Enveloper->DurationAfterLoop;
				Spec.LoopCount = Enveloper->bLoopIndefinitely ? 0 : Enveloper->LoopCount;
				Spec.PitchMin = Enveloper->PitchMin;
				Spec.PitchMax = Enveloper->PitchMax;
				Spec.VolumeMin = Enveloper->VolumeMin;
				Spec.VolumeMax = Enveloper->VolumeMax;
			}
			else if (const USoundNodeBranch* Branch = Cast<USoundNodeBranch>(&Node))
			{
				Spec.Parameter = Branch->BoolParameterName.IsNone() ? FString() : Branch->BoolParameterName.ToString();
			}
			else if (const USoundNodeSwitch* Switch = Cast<USoundNodeSwitch>(&Node))
			{
				Spec.Parameter = Switch->IntParameterName.IsNone() ? FString() : Switch->IntParameterName.ToString();
			}
			else if (const USoundNodeParamCrossFade* CrossFade = Cast<USoundNodeParamCrossFade>(&Node))
			{
				Spec.Parameter = CrossFade->ParamName.IsNone() ? FString() : CrossFade->ParamName.ToString();
				for (const FDistanceDatum& Datum : CrossFade->CrossFadeInput)
				{
					FHyperAISoundCueFade& Fade = Spec.Fades.AddDefaulted_GetRef();
					Fade.FadeInStart = Datum.FadeInDistanceStart;
					Fade.FadeInEnd = Datum.FadeInDistanceEnd;
					Fade.FadeOutStart = Datum.FadeOutDistanceStart;
					Fade.FadeOutEnd = Datum.FadeOutDistanceEnd;
					Fade.Volume = Datum.Volume;
				}
			}
			else if (const USoundNodeModulatorContinuous* Continuous = Cast<USoundNodeModulatorContinuous>(&Node))
			{
				const FModulatorContinuousParams& VolumeParams = Continuous->VolumeModulationParams;
				const FModulatorContinuousParams& PitchParams = Continuous->PitchModulationParams;
				Spec.Parameter = VolumeParams.ParameterName.IsNone() ? FString() : VolumeParams.ParameterName.ToString();
				Spec.PitchParameter = PitchParams.ParameterName.IsNone() ? FString() : PitchParams.ParameterName.ToString();
				// A range no parameter drives does nothing; it reads back as the plan's unchanged 1.
				Spec.VolumeMin = Spec.Parameter.IsEmpty() ? 1.f : VolumeParams.MinOutput;
				Spec.VolumeMax = Spec.Parameter.IsEmpty() ? 1.f : VolumeParams.MaxOutput;
				Spec.PitchMin = Spec.PitchParameter.IsEmpty() ? 1.f : PitchParams.MinOutput;
				Spec.PitchMax = Spec.PitchParameter.IsEmpty() ? 1.f : PitchParams.MaxOutput;
			}
			else if (const USoundNodeWaveParam* WaveParam = Cast<USoundNodeWaveParam>(&Node))
			{
				Spec.Parameter = WaveParam->WaveParameterName.IsNone() ? FString() : WaveParam->WaveParameterName.ToString();
			}
			return Spec;
		}

		/** Every node the root reaches, each once; a node fed into two parents is reached once. */
		void Reach(const USoundNode* Node, TArray<const USoundNode*>& OutReached)
		{
			if (!Node || OutReached.Contains(Node) || OutReached.Num() > 4096)
			{
				return;
			}
			OutReached.Add(Node);
			for (const USoundNode* Child : Node->ChildNodes)
			{
				Reach(Child, OutReached);
			}
		}

		void AddIssue(TArray<FHyperAISoundIssue>& Issues, const TCHAR* Severity, const TCHAR* Code, const FString& Message)
		{
			FHyperAISoundIssue& Issue = Issues.AddDefaulted_GetRef();
			Issue.Severity = Severity;
			Issue.Code = Code;
			Issue.Message = Message;
		}

		/** The editor half of a Sound Cue (its graph) exists only with the Audio Editor module loaded, which sets it up. */
		bool EnsureCueEditor(FString& OutError)
		{
			if (!USoundCue::GetSoundCueAudioEditor().IsValid())
			{
				FModuleManager::Get().LoadModule(TEXT("AudioEditor"));
			}
			if (!USoundCue::GetSoundCueAudioEditor().IsValid())
			{
				OutError = TEXT("The Audio Editor module is not loaded, so Sound Cue graphs cannot be built in this editor.");
				return false;
			}
			return true;
		}
	}

	const TArray<FString>& Kinds()
	{
		static const TArray<FString> Names = []
		{
			TArray<FString> Out;
			for (const FKind& Entry : KindTable())
			{
				Out.Add(Entry.Name);
			}
			return Out;
		}();
		return Names;
	}

	FString KindOf(const USoundNode& Node)
	{
		for (const FKind& Entry : KindTable())
		{
			if (Node.GetClass() == Entry.Class())
			{
				return Entry.Name;
			}
		}
		return FString();
	}

	FString PackageOf(const FString& Path)
	{
		FString Package = Path.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (Package.FindChar(TEXT('.'), Dot))
		{
			Package.LeftInline(Dot);
		}
		return Package;
	}

	UObject* LoadAsset(const FString& Path, const UClass& Class, FString* OutReason)
	{
		const FString Trimmed = Path.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !Trimmed.StartsWith(TEXT("/")) || !FHyperAIStudioNames::IsObjectPath(Trimmed, TEXT("Path"), OutReason))
		{
			return nullptr;
		}
		const FString Package = PackageOf(Trimmed);
		// A lookup of nothing makes the package it looked for, so a path with no asset behind it is not loaded at all.
		if (FHyperAIStudioNames::IsFreePackage(Package))
		{
			return nullptr;
		}
		const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Package + TEXT(".") + FPackageName::GetShortName(Package);
		UObject* Object = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		return Object && Object->IsA(&Class) ? Object : nullptr;
	}

	bool Check(const TArray<FHyperAISoundCueNode>& Nodes, const FString& Root, FCheckedGraph& Out, FString& OutError)
	{
		Out = FCheckedGraph();
		if (Nodes.IsEmpty() || Nodes.Num() > C::MaxCueNodes)
		{
			Fail(OutError, FString::Printf(TEXT("A graph has 1 to %d nodes."), C::MaxCueNodes));
			return false;
		}
		Out.Nodes = Nodes;
		TMap<FString, int32> IndexOf;
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			FHyperAISoundCueNode& Node = Out.Nodes[Index];
			Node.Id = Node.Id.TrimStartAndEnd();
			Node.Kind = Node.Kind.TrimStartAndEnd().ToLower();
			if (Node.Id.IsEmpty() || Node.Id.Len() > 64)
			{
				Fail(OutError, FString::Printf(TEXT("Nodes[%d] needs an Id of 1 to 64 characters."), Index));
				return false;
			}
			if (IndexOf.Contains(Node.Id.ToLower()))
			{
				Fail(OutError, FString::Printf(TEXT("Two nodes have the Id %s."), *Node.Id));
				return false;
			}
			IndexOf.Add(Node.Id.ToLower(), Index);
			if (!ClassForKind(Node.Kind))
			{
				Fail(OutError, FString::Printf(TEXT("Node %s: Kind is one of %s."), *Node.Id, *FString::Join(Kinds(), TEXT(", "))));
				return false;
			}
		}
		const FString RootId = Root.TrimStartAndEnd().IsEmpty() ? Out.Nodes[0].Id : Root.TrimStartAndEnd();
		const int32* RootIndex = IndexOf.Find(RootId.ToLower());
		if (!RootIndex)
		{
			Fail(OutError, FString::Printf(TEXT("Root %s names no node."), *RootId));
			return false;
		}
		Out.Root = *RootIndex;

		// Each node feeds one input: the plan is a tree, read from the root.
		TArray<int32> ParentOf;
		ParentOf.Init(INDEX_NONE, Nodes.Num());
		for (int32 Index = 0; Index < Out.Nodes.Num(); ++Index)
		{
			const FHyperAISoundCueNode& Node = Out.Nodes[Index];
			int32 Min = 0;
			int32 Max = 0;
			InputLimits(Node.Kind, Min, Max);
			if (Node.Children.Num() < Min || Node.Children.Num() > Max)
			{
				Fail(OutError, Max == 0 ? FString::Printf(TEXT("Node %s: a %s takes no Children."), *Node.Id, *Node.Kind)
					: Min == Max ? FString::Printf(TEXT("Node %s: a %s takes %d %s in Children; it has %d."), *Node.Id, *Node.Kind, Min, Min == 1 ? TEXT("input") : TEXT("inputs"), Node.Children.Num())
					: FString::Printf(TEXT("Node %s: a %s takes %d to %d inputs in Children; it has %d."), *Node.Id, *Node.Kind, Min, Max, Node.Children.Num()));
				return false;
			}
			for (const FString& ChildIdRaw : Node.Children)
			{
				const FString ChildId = ChildIdRaw.TrimStartAndEnd();
				if (ChildId.IsEmpty())
				{
					if (!AllowsEmptyInput(Node.Kind))
					{
						Fail(OutError, FString::Printf(TEXT("Node %s has an empty child; only a branch or switch may leave an input empty."), *Node.Id));
						return false;
					}
					continue;
				}
				const int32* Child = IndexOf.Find(ChildId.ToLower());
				if (!Child)
				{
					Fail(OutError, FString::Printf(TEXT("Node %s: child %s names no node."), *Node.Id, *ChildId));
					return false;
				}
				if (*Child == Out.Root || *Child == Index)
				{
					Fail(OutError, FString::Printf(TEXT("Node %s: %s cannot feed it; the graph would loop."), *Node.Id, *ChildId));
					return false;
				}
				if (ParentOf[*Child] != INDEX_NONE)
				{
					Fail(OutError, FString::Printf(TEXT("Node %s feeds both %s and %s; each node feeds one input, so give the second its own node."),
						*ChildId, *Out.Nodes[ParentOf[*Child]].Id, *Node.Id));
					return false;
				}
				ParentOf[*Child] = Index;
			}
		}

		// Depth first from the root; whatever it does not reach would dangle.
		Out.Positions.SetNum(Nodes.Num());
		TArray<int32> Depth;
		Depth.Init(INDEX_NONE, Nodes.Num());
		int32 NextRow = 0;
		TFunction<int32(int32, int32)> Place = [&](const int32 Index, const int32 Level) -> int32
		{
			Depth[Index] = Level;
			Out.Order.Add(Index);
			TArray<int32> Rows;
			for (const FString& ChildId : Out.Nodes[Index].Children)
			{
				const int32* Child = ChildId.TrimStartAndEnd().IsEmpty() ? nullptr : IndexOf.Find(ChildId.TrimStartAndEnd().ToLower());
				if (Child && Depth[*Child] == INDEX_NONE && Level < 64)
				{
					Rows.Add(Place(*Child, Level + 1));
				}
			}
			const int32 Y = Rows.IsEmpty() ? (NextRow++) * RowHeight : (Rows[0] + Rows.Last()) / 2;
			Out.Positions[Index] = FIntPoint(-ColumnWidth * (Level + 1), Y);
			return Y;
		};
		Place(Out.Root, 0);
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Depth[Index] == INDEX_NONE)
			{
				Fail(OutError, FString::Printf(TEXT("Node %s is not under the root %s, so it would dangle; feed it into a node or leave it out."),
					*Out.Nodes[Index].Id, *Out.Nodes[Out.Root].Id));
				return false;
			}
		}
		// The rows run down from the middle, so the root sits level with the graph's output.
		const int32 Middle = Out.Positions[Out.Root].Y;
		for (FIntPoint& Position : Out.Positions)
		{
			Position.Y -= Middle;
		}

		// Each kind's own fields, and the assets they name.
		Out.Waves.SetNum(Nodes.Num());
		Out.Attenuations.SetNum(Nodes.Num());
		for (int32 Index = 0; Index < Out.Nodes.Num(); ++Index)
		{
			const FHyperAISoundCueNode& Node = Out.Nodes[Index];
			const FString& Kind = Node.Kind;
			const FString Where = FString::Printf(TEXT("Node %s (%s): "), *Node.Id, *Kind);
			const int32 Inputs = Node.Children.Num();
			FString Reason;
			if (Kind == TEXT("wave_player"))
			{
				USoundWave* Wave = Cast<USoundWave>(LoadAsset(Node.Wave, *USoundWave::StaticClass(), &Reason));
				if (!Wave)
				{
					Fail(OutError, Where + (Reason.IsEmpty() ? FString::Printf(TEXT("%s is not a SoundWave in the project."), *Node.Wave) : Reason));
					return false;
				}
				Out.Waves[Index] = Wave;
			}
			else if (Kind == TEXT("attenuation"))
			{
				USoundAttenuation* Attenuation = Cast<USoundAttenuation>(LoadAsset(Node.Attenuation, *USoundAttenuation::StaticClass(), &Reason));
				if (!Attenuation)
				{
					Fail(OutError, Where + (Reason.IsEmpty() ? FString::Printf(TEXT("Attenuation %s is not a Sound Attenuation asset."), *Node.Attenuation) : Reason));
					return false;
				}
				Out.Attenuations[Index] = Attenuation;
			}
			if ((Kind == TEXT("random") || Kind == TEXT("mixer") || Kind == TEXT("concatenator")) && !Node.Weights.IsEmpty())
			{
				if (Node.Weights.Num() != Inputs)
				{
					Fail(OutError, Where + FString::Printf(TEXT("Weights has %d values for %d inputs; give one per child, or none for all 1."), Node.Weights.Num(), Inputs));
					return false;
				}
				for (const float Weight : Node.Weights)
				{
					if (!FMath::IsFinite(Weight) || Weight < 0.f || Weight > 100.f)
					{
						Fail(OutError, Where + TEXT("each weight or input volume is 0 to 100."));
						return false;
					}
				}
				if (Kind == TEXT("random") && !Node.Weights.ContainsByPredicate([](const float Weight) { return Weight > 0.f; }))
				{
					Fail(OutError, Where + TEXT("at least one weight must be above 0, or nothing would ever play."));
					return false;
				}
			}
			if (Kind == TEXT("random") && (Node.PreselectAtLevelLoad < 0 || Node.PreselectAtLevelLoad > Inputs))
			{
				Fail(OutError, Where + FString::Printf(TEXT("PreselectAtLevelLoad is 0 (all) to %d."), Inputs));
				return false;
			}
			if (Kind == TEXT("modulator") || Kind == TEXT("enveloper") || Kind == TEXT("continuous_modulator"))
			{
				// A continuous modulator's range matters only for the parameter that drives it.
				const bool bContinuous = Kind == TEXT("continuous_modulator");
				const bool bPitch = !bContinuous || !Node.PitchParameter.TrimStartAndEnd().IsEmpty();
				const bool bVolume = !bContinuous || !Node.Parameter.TrimStartAndEnd().IsEmpty();
				if (bPitch && !(Node.PitchMin > 0.f && Node.PitchMin <= Node.PitchMax && Node.PitchMax <= 8.f))
				{
					Fail(OutError, Where + TEXT("PitchMin and PitchMax are multipliers above 0 and up to 8, with PitchMin no higher than PitchMax (1 is unchanged)."));
					return false;
				}
				if (bVolume && !(Node.VolumeMin >= 0.f && Node.VolumeMin <= Node.VolumeMax && Node.VolumeMax <= 10.f))
				{
					Fail(OutError, Where + TEXT("VolumeMin and VolumeMax are 0 to 10, with VolumeMin no higher than VolumeMax (1 is unchanged)."));
					return false;
				}
			}
			if ((Kind == TEXT("looping") || Kind == TEXT("enveloper")) && Node.LoopCount < 0)
			{
				Fail(OutError, Where + TEXT("LoopCount is 0 (forever) or more."));
				return false;
			}
			if (Kind == TEXT("delay") && !(Node.DelayMin >= 0.f && Node.DelayMin <= Node.DelayMax && Node.DelayMax <= 60.f))
			{
				Fail(OutError, Where + TEXT("DelayMin and DelayMax are 0 to 60 seconds, DelayMin no higher than DelayMax."));
				return false;
			}
			const bool bNeedsParameter = Kind == TEXT("branch") || Kind == TEXT("switch") || Kind == TEXT("param_crossfade") || Kind == TEXT("wave_param");
			if (bNeedsParameter && (Node.Parameter.TrimStartAndEnd().IsEmpty() || !FHyperAIStudioNames::IsName(Node.Parameter, TEXT("Parameter"), &Reason)))
			{
				Fail(OutError, Where + (Reason.IsEmpty() ? FString(TEXT("Parameter names the sound parameter it reads.")) : Reason));
				return false;
			}
			if (Kind == TEXT("continuous_modulator") && Node.Parameter.TrimStartAndEnd().IsEmpty() && Node.PitchParameter.TrimStartAndEnd().IsEmpty())
			{
				Fail(OutError, Where + TEXT("give Parameter (drives volume), PitchParameter (drives pitch), or both."));
				return false;
			}
			if (Kind == TEXT("param_crossfade") && !Node.Fades.IsEmpty() && Node.Fades.Num() != Inputs)
			{
				Fail(OutError, Where + FString::Printf(TEXT("Fades has %d entries for %d inputs; give one per child."), Node.Fades.Num(), Inputs));
				return false;
			}
			if (Kind == TEXT("enveloper"))
			{
				if (Node.VolumeCurve.Num() > MaxCurvePoints || Node.PitchCurve.Num() > MaxCurvePoints)
				{
					Fail(OutError, Where + FString::Printf(TEXT("a curve has at most %d points."), MaxCurvePoints));
					return false;
				}
				if (Node.LoopStart < 0.f || Node.LoopEnd < 0.f || Node.DurationAfterLoop < 0.f || (Node.LoopEnd > 0.f && Node.LoopEnd < Node.LoopStart))
				{
					Fail(OutError, Where + TEXT("LoopStart, LoopEnd and DurationAfterLoop are seconds from 0, LoopEnd after LoopStart (equal for no loop)."));
					return false;
				}
			}
		}
		return true;
	}

	TArray<FHyperAISoundCueNodeInfo> Describe(const FCheckedGraph& Graph)
	{
		TArray<FHyperAISoundCueNodeInfo> Out;
		for (const int32 Index : Graph.Order)
		{
			const FHyperAISoundCueNode& Node = Graph.Nodes[Index];
			FHyperAISoundCueNodeInfo& Info = Out.AddDefaulted_GetRef();
			Info.Id = Node.Id;
			Info.Kind = Node.Kind;
			Info.Children = Node.Children;
			Info.Detail = DetailOf(Node);
			Info.X = Graph.Positions[Index].X;
			Info.Y = Graph.Positions[Index].Y;
		}
		return Out;
	}

	bool Build(USoundCue& Cue, const FCheckedGraph& Graph, FString& OutError)
	{
		if (!EnsureCueEditor(OutError))
		{
			return false;
		}
		if (!Cue.GetGraph())
		{
			Cue.CreateGraph();
		}
		UEdGraph* EdGraph = Cue.GetGraph();
		if (!EdGraph)
		{
			OutError = TEXT("The Sound Cue has no graph to build into.");
			return false;
		}
		// The graph's own AddNode records nothing for undo, so everything it holds is recorded first.
		EdGraph->Modify();
		for (UEdGraphNode* GraphNode : EdGraph->Nodes)
		{
			if (GraphNode)
			{
				GraphNode->Modify();
			}
		}
		// The old graph goes the way the Sound Cue editor deletes nodes: graph nodes removed, then the cue's list.
		const TArray<TObjectPtr<USoundNode>> Old = Cue.AllNodes;
		for (USoundNode* Node : Old)
		{
			if (!Node)
			{
				continue;
			}
			Node->Modify();
			if (UEdGraphNode* GraphNode = Node->GetGraphNode())
			{
				FBlueprintEditorUtils::RemoveNode(nullptr, GraphNode, /*bDontRecompile=*/true);
			}
		}
		Cue.AllNodes.Reset();
		Cue.FirstNode = nullptr;

		TArray<USoundNode*> Made;
		Made.Init(nullptr, Graph.Nodes.Num());
		for (const int32 Index : Graph.Order)
		{
			const FHyperAISoundCueNode& Spec = Graph.Nodes[Index];
			USoundNode* Node = Cue.ConstructSoundNode<USoundNode>(ClassForKind(Spec.Kind), /*bSelectNewNode=*/false);
			if (!Node || !Node->GetGraphNode())
			{
				OutError = FString::Printf(TEXT("Node %s (%s) could not be made."), *Spec.Id, *Spec.Kind);
				return false;
			}
			// One input per child; each kind's own per-input arrays (weights, volumes, fades) grow with them.
			for (int32 Input = 0; Input < Spec.Children.Num(); ++Input)
			{
				Node->InsertChildNode(Node->ChildNodes.Num());
			}
			if (Node->ChildNodes.Num() != Spec.Children.Num())
			{
				OutError = FString::Printf(TEXT("Node %s (%s) took %d inputs rather than %d."), *Spec.Id, *Spec.Kind, Node->ChildNodes.Num(), Spec.Children.Num());
				return false;
			}
			Configure(*Node, Spec, Graph.Waves[Index].Get(), Graph.Attenuations[Index].Get());
			Node->GetGraphNode()->NodePosX = Graph.Positions[Index].X;
			Node->GetGraphNode()->NodePosY = Graph.Positions[Index].Y;
			Made[Index] = Node;
		}
		TMap<FString, int32> IndexOf;
		for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index)
		{
			IndexOf.Add(Graph.Nodes[Index].Id.ToLower(), Index);
		}
		for (const int32 Index : Graph.Order)
		{
			const FHyperAISoundCueNode& Spec = Graph.Nodes[Index];
			for (int32 Input = 0; Input < Spec.Children.Num(); ++Input)
			{
				const int32* Child = IndexOf.Find(Spec.Children[Input].TrimStartAndEnd().ToLower());
				Made[Index]->ChildNodes[Input] = Child ? Made[*Child] : nullptr;
			}
		}
		Cue.FirstNode = Made[Graph.Root];
		// The graph's pins follow the nodes' children, and the cue's duration and flags are cached again.
		Cue.LinkGraphNodesFromSoundNodes();
		for (USoundNode* Node : Made)
		{
			Node->PostEditChange();
		}
		return true;
	}

	void Read(const USoundCue& Cue, TArray<FHyperAISoundCueNode>& OutNodes, FString& OutRoot)
	{
		OutNodes.Reset();
		OutRoot.Reset();
		TArray<const USoundNode*> Reached;
		Reach(Cue.FirstNode, Reached);
		for (const USoundNode* Node : Cue.AllNodes)
		{
			if (Node && !Reached.Contains(Node))
			{
				Reached.Add(Node);
			}
		}
		TMap<const USoundNode*, FString> IdOf;
		for (int32 Index = 0; Index < Reached.Num(); ++Index)
		{
			IdOf.Add(Reached[Index], FString::Printf(TEXT("n%d"), Index));
		}
		for (const USoundNode* Node : Reached)
		{
			const FString Kind = KindOf(*Node);
			FHyperAISoundCueNode Spec = Unconfigure(*Node, Kind.IsEmpty() ? Node->GetClass()->GetName() : Kind);
			Spec.Id = IdOf[Node];
			for (const USoundNode* Child : Node->ChildNodes)
			{
				Spec.Children.Add(Child && IdOf.Contains(Child) ? IdOf[Child] : FString());
			}
			OutNodes.Add(MoveTemp(Spec));
		}
		const USoundNode* Root = Cue.FirstNode.Get();
		const FString* RootId = Root ? IdOf.Find(Root) : nullptr;
		OutRoot = RootId ? *RootId : FString();
	}

	TArray<FHyperAISoundIssue> Validate(const USoundCue& Cue)
	{
		TArray<FHyperAISoundIssue> Issues;
		if (!Cue.FirstNode)
		{
			AddIssue(Issues, TEXT("error"), TEXT("no_root"), TEXT("Nothing is connected to the cue's output, so it plays nothing."));
		}
		TArray<const USoundNode*> Reached;
		Reach(Cue.FirstNode, Reached);
		int32 Dangling = 0;
		for (const USoundNode* Node : Cue.AllNodes)
		{
			if (Node && !Reached.Contains(Node))
			{
				++Dangling;
			}
		}
		if (Dangling > 0)
		{
			AddIssue(Issues, TEXT("error"), TEXT("dangling_node"), FString::Printf(TEXT("%d node(s) are in the graph but not connected to the output."), Dangling));
		}
		const UEdGraph* EdGraph = const_cast<USoundCue&>(Cue).GetGraph();
		for (const USoundNode* Node : Reached)
		{
			const FString Kind = KindOf(*Node);
			const FString Name = FString::Printf(TEXT("%s (%s)"), *Node->GetName(), Kind.IsEmpty() ? *Node->GetClass()->GetName() : *Kind);
			if (Node->ChildNodes.Num() < Node->GetMinChildNodes())
			{
				AddIssue(Issues, TEXT("error"), TEXT("too_few_inputs"), FString::Printf(TEXT("%s has %d inputs and needs %d."), *Name, Node->ChildNodes.Num(), Node->GetMinChildNodes()));
			}
			int32 Empty = 0;
			for (const USoundNode* Child : Node->ChildNodes)
			{
				Empty += Child ? 0 : 1;
			}
			if (Empty > 0)
			{
				AddIssue(Issues, AllowsEmptyInput(Kind) ? TEXT("warning") : TEXT("error"), TEXT("empty_input"),
					FString::Printf(TEXT("%s has %d input(s) with nothing connected."), *Name, Empty));
			}
			if (const USoundNodeWavePlayer* Player = Cast<USoundNodeWavePlayer>(Node))
			{
				const FString Path = WavePathOf(*Player);
				if (Path.IsEmpty())
				{
					AddIssue(Issues, TEXT("error"), TEXT("missing_wave"), FString::Printf(TEXT("%s has no SoundWave."), *Name));
				}
				else if (!Player->GetSoundWave() && !LoadAsset(Path, *USoundWave::StaticClass()))
				{
					AddIssue(Issues, TEXT("error"), TEXT("missing_wave"), FString::Printf(TEXT("%s plays %s, which is not in the project."), *Name, *Path));
				}
			}
			if (const USoundNodeAttenuation* AttenuationNode = Cast<USoundNodeAttenuation>(Node))
			{
				if (!AttenuationNode->bOverrideAttenuation && !AttenuationNode->AttenuationSettings)
				{
					AddIssue(Issues, TEXT("warning"), TEXT("missing_asset"), FString::Printf(TEXT("%s has no attenuation asset and no override, so it does nothing."), *Name));
				}
			}
			// The editor's graph is what a person sees; it must say what the nodes do.
			const UEdGraphNode* GraphNode = Node->GetGraphNode();
			if (EdGraph && (!GraphNode || !EdGraph->Nodes.Contains(GraphNode)))
			{
				AddIssue(Issues, TEXT("warning"), TEXT("graph_out_of_sync"), FString::Printf(TEXT("%s has no node in the cue's graph."), *Name));
				continue;
			}
			if (GraphNode)
			{
				int32 InputPins = 0;
				int32 Linked = 0;
				for (const UEdGraphPin* Pin : GraphNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input)
					{
						++InputPins;
						Linked += Pin->LinkedTo.Num() > 0 ? 1 : 0;
					}
				}
				if (InputPins != Node->ChildNodes.Num() || Linked != Node->ChildNodes.Num() - Empty)
				{
					AddIssue(Issues, TEXT("warning"), TEXT("graph_out_of_sync"), FString::Printf(TEXT("%s's graph pins do not match its inputs; open and save the cue to rebuild them."), *Name));
				}
			}
		}
		return Issues;
	}

	float DurationOf(USoundCue& Cue)
	{
		Cue.CacheAggregateValues();
		const float Duration = Cue.GetDuration();
		return Duration >= INDEFINITELY_LOOPING_DURATION ? -1.f : Duration;
	}
}
