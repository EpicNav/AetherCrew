// Games by Hyper 2026.

#include "HyperAIStudioMetaSoundRecipes.h"

#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundSource.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Sound/SoundWave.h"

namespace HyperAIStudio::AudioAuthoring::MetaSounds
{
	namespace
	{
		struct FInput
		{
			const TCHAR* Name;
			/** Float or Time. */
			const TCHAR* Type;
			float Default;
			float Min;
			float Max;
			const TCHAR* Description;
		};

		class FGraph;

		/** Whether a recipe plays SoundWaves given to it. */
		enum class EWaves : uint8
		{
			None,
			Optional,
			Required
		};

		struct FRecipe
		{
			const TCHAR* Id;
			const TCHAR* Description;
			bool bOneShot;
			TArray<FInput> Inputs;
			void (*Build)(FGraph&);
			EWaves Waves = EWaves::None;
			/** The Sound Cue version's shape, or null when the recipe has none. */
			const TCHAR* Cue = nullptr;
		};

		/**
		 * A source being built, node by node, with the first failure kept. Node names, pins and variants are the ones UE 5.8
		 * registers (MetasoundStandardNodes, MetasoundEngine); a wrong one fails the build with its name rather than a crash.
		 */
		class FGraph
		{
		public:
			UMetaSoundSourceBuilder* Builder = nullptr;
			FMetaSoundBuilderNodeOutputHandle OnPlay;
			FMetaSoundBuilderNodeInputHandle OnFinished;
			TArray<FMetaSoundBuilderNodeInputHandle> Outs;
			TMap<FName, FMetaSoundBuilderNodeOutputHandle> Inputs;
			FString Error;
			int32 NodeCount = 0;
			/** Wave recipes: the takes, the impact's tails, and whether any is stereo (the players and output follow). */
			TArray<UObject*> Waves;
			TArray<UObject*> Tails;
			bool bStereo = false;

			bool Start(const bool bOneShot, const bool bStereoOutput)
			{
				EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
				const FName Name(*(TEXT("HyperAIAudio_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
				Builder = UMetaSoundBuilderSubsystem::GetChecked().CreateSourceBuilder(Name, OnPlay, OnFinished, Outs, Result,
					bStereoOutput ? EMetaSoundOutputAudioFormat::Stereo : EMetaSoundOutputAudioFormat::Mono, bOneShot);
				if (!Builder || Result != EMetaSoundBuilderResult::Succeeded || Outs.Num() != (bStereoOutput ? 2 : 1))
				{
					Error = TEXT("The MetaSound builder did not start a source.");
					return false;
				}
				return true;
			}

			void Expect(const EMetaSoundBuilderResult Result, const FString& What)
			{
				if (Result != EMetaSoundBuilderResult::Succeeded && Error.IsEmpty())
				{
					Error = What;
				}
			}

			/** A node, placed on a grid of columns (left to right, signal flow) and rows. */
			FMetaSoundNodeHandle Node(const TCHAR* Namespace, const TCHAR* Name, const TCHAR* Variant, const float Column, const float Row)
			{
				EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
				const FMetaSoundNodeHandle Handle = Builder->AddNodeByClassName(FMetasoundFrontendClassName(Namespace, Name, Variant), Result, 1);
				Expect(Result, FString::Printf(TEXT("The MetaSound node %s.%s.%s is not registered in this editor."), Namespace, Name, Variant));
				if (Result == EMetaSoundBuilderResult::Succeeded)
				{
					++NodeCount;
					Builder->SetNodeLocation(Handle, FVector2D(Column * 320.0, Row * 170.0), Result);
				}
				return Handle;
			}

			FMetaSoundBuilderNodeInputHandle In(const FMetaSoundNodeHandle& Node, const TCHAR* Input)
			{
				EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
				const FMetaSoundBuilderNodeInputHandle Handle = Builder->FindNodeInputByName(Node, Input, Result);
				Expect(Result, FString::Printf(TEXT("A MetaSound node has no input %s."), Input));
				return Handle;
			}

			FMetaSoundBuilderNodeOutputHandle Out(const FMetaSoundNodeHandle& Node, const TCHAR* Output)
			{
				EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
				const FMetaSoundBuilderNodeOutputHandle Handle = Builder->FindNodeOutputByName(Node, Output, Result);
				Expect(Result, FString::Printf(TEXT("A MetaSound node has no output %s."), Output));
				return Handle;
			}

			template <typename T>
			void Set(const FMetaSoundNodeHandle& Node, const TCHAR* Input, const T Value)
			{
				if (!Error.IsEmpty()) return;
				FMetasoundFrontendLiteral Literal;
				Literal.Set(Value);
				EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
				Builder->SetNodeInputDefault(In(Node, Input), Literal, Result);
				Expect(Result, FString::Printf(TEXT("The MetaSound input %s did not take its value."), Input));
			}

			void Wire(const FMetaSoundNodeHandle& From, const TCHAR* Output, const FMetaSoundNodeHandle& To, const TCHAR* Input)
			{
				Connect(Out(From, Output), In(To, Input), FString::Printf(TEXT("%s -> %s"), Output, Input));
			}

			void Connect(const FMetaSoundBuilderNodeOutputHandle& From, const FMetaSoundBuilderNodeInputHandle& To, const FString& What)
			{
				if (!Error.IsEmpty()) return;
				EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
				Builder->ConnectNodes(From, To, Result);
				Expect(Result, TEXT("The MetaSound connection ") + What + TEXT(" was refused."));
			}

			void Play(const FMetaSoundNodeHandle& To, const TCHAR* Input) { Connect(OnPlay, In(To, Input), FString(TEXT("On Play -> ")) + Input); }
			void ToOut(const FMetaSoundNodeHandle& From, const TCHAR* Output, const int32 Channel = 0) { Connect(Out(From, Output), Outs[Channel], FString(Output) + TEXT(" -> output")); }
			void Finish(const FMetaSoundNodeHandle& From, const TCHAR* Output) { Connect(Out(From, Output), OnFinished, FString(Output) + TEXT(" -> On Finished")); }

			/** A graph input: what a Blueprint or the Details panel sets, such as RPM. */
			void Input(const FInput& Spec, const float Value)
			{
				if (!Error.IsEmpty()) return;
				FMetasoundFrontendLiteral Literal;
				Literal.Set(Value);
				EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
				const FMetaSoundBuilderNodeOutputHandle Handle = Builder->AddGraphInputNode(Spec.Name, Spec.Type, Literal, Result);
				Expect(Result, FString::Printf(TEXT("The MetaSound input %s could not be added."), Spec.Name));
				Inputs.Add(Spec.Name, Handle);
			}

			void From(const TCHAR* GraphInput, const FMetaSoundNodeHandle& To, const TCHAR* Input)
			{
				const FMetaSoundBuilderNodeOutputHandle* Handle = Inputs.Find(GraphInput);
				if (!Handle)
				{
					Error = Error.IsEmpty() ? FString::Printf(TEXT("The recipe reads an input %s it does not declare."), GraphInput) : Error;
					return;
				}
				Connect(*Handle, In(To, Input), FString(GraphInput) + TEXT(" -> ") + Input);
			}

			// Building blocks.
			FMetaSoundNodeHandle MulAudioByFloat(const float Column, const float Row) { return Node(TEXT("UE"), TEXT("Multiply"), TEXT("Audio by Float"), Column, Row); }
			FMetaSoundNodeHandle MulAudio(const float Column, const float Row) { return Node(TEXT("UE"), TEXT("Multiply"), TEXT("Audio"), Column, Row); }
			FMetaSoundNodeHandle MulFloat(const float Column, const float Row) { return Node(TEXT("UE"), TEXT("Multiply"), TEXT("Float"), Column, Row); }
			FMetaSoundNodeHandle AddFloat(const float Column, const float Row) { return Node(TEXT("UE"), TEXT("Add"), TEXT("Float"), Column, Row); }
			FMetaSoundNodeHandle AddAudio(const float Column, const float Row) { return Node(TEXT("UE"), TEXT("Add"), TEXT("Audio"), Column, Row); }
			FMetaSoundNodeHandle Mixer(const int32 Channels, const float Column, const float Row)
			{
				return Node(TEXT("AudioMixer"), *FString::Printf(TEXT("Audio Mixer (Mono, %d)"), Channels), TEXT(""), Column, Row);
			}
			FMetaSoundNodeHandle Envelope(const bool bAudio, const float Column, const float Row)
			{
				return Node(TEXT("AD Envelope"), TEXT("AD Envelope"), bAudio ? TEXT("Audio") : TEXT("Float"), Column, Row);
			}

			int32 Channels() const { return bStereo ? 2 : 1; }
			const TCHAR* PlayerOut(const int32 Channel) const { return bStereo ? (Channel == 0 ? TEXT("Out Left") : TEXT("Out Right")) : TEXT("Out Mono"); }

			/** A graph input holding the takes, so a designer can add more in the Details panel. */
			FMetaSoundBuilderNodeOutputHandle WaveArray(const TCHAR* Name, const TArray<UObject*>& Objects)
			{
				FMetaSoundBuilderNodeOutputHandle Handle;
				if (!Error.IsEmpty()) return Handle;
				FMetasoundFrontendLiteral Literal;
				Literal.Set(Objects);
				EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
				Handle = Builder->AddGraphInputNode(Name, TEXT("WaveAsset:Array"), Literal, Result);
				Expect(Result, FString::Printf(TEXT("The MetaSound input %s could not be added."), Name));
				return Handle;
			}

			/** A take picked per trigger (never the same twice running when there are two or more) and the Wave Player playing it. */
			FMetaSoundNodeHandle Picker(const FMetaSoundBuilderNodeOutputHandle& Takes, const int32 Count, const FMetaSoundBuilderNodeOutputHandle* Trigger,
				const float Column, const float Row)
			{
				const FMetaSoundNodeHandle Pick = Node(TEXT("Array"), TEXT("Random Get"), TEXT("WaveAsset:Array"), Column, Row);
				Connect(Takes, In(Pick, TEXT("In Array")), TEXT("takes -> In Array"));
				if (Trigger)
				{
					Connect(*Trigger, In(Pick, TEXT("Next")), TEXT("trigger -> Next"));
				}
				else
				{
					Play(Pick, TEXT("Next"));
				}
				Set(Pick, TEXT("No Repeats"), Count > 1 ? 1 : 0);
				const FMetaSoundNodeHandle Player = Node(TEXT("UE"), TEXT("Wave Player"), bStereo ? TEXT("Stereo") : TEXT("Mono"), Column + 1, Row);
				Wire(Pick, TEXT("Value"), Player, TEXT("Wave Asset"));
				Wire(Pick, TEXT("On Next"), Player, TEXT("Play"));
				return Player;
			}

			/** A pitch shift picked per play between -Pitch Jitter and +Pitch Jitter semitones. */
			FMetaSoundBuilderNodeOutputHandle PitchJitter(const float Column, const float Row)
			{
				const FMetaSoundNodeHandle Negative = MulFloat(Column, Row);
				From(TEXT("Pitch Jitter"), Negative, TEXT("PrimaryOperand"));
				Set(Negative, TEXT("AdditionalOperands"), -1.f);
				const FMetaSoundNodeHandle Pick = Node(TEXT("UE"), TEXT("RandomFloat"), TEXT(""), Column + 1, Row);
				Play(Pick, TEXT("Next"));
				Wire(Negative, TEXT("Out"), Pick, TEXT("Min"));
				From(TEXT("Pitch Jitter"), Pick, TEXT("Max"));
				return Out(Pick, TEXT("Value"));
			}

			/** A gain picked per play between 1 - Volume Jitter and 1, times Volume. */
			FMetaSoundBuilderNodeOutputHandle Level(const float Column, const float Row)
			{
				const FMetaSoundNodeHandle Low = Node(TEXT("UE"), TEXT("Subtract"), TEXT("Float"), Column, Row);
				Set(Low, TEXT("PrimaryOperand"), 1.f);
				From(TEXT("Volume Jitter"), Low, TEXT("AdditionalOperands"));
				const FMetaSoundNodeHandle Pick = Node(TEXT("UE"), TEXT("RandomFloat"), TEXT(""), Column + 1, Row);
				Play(Pick, TEXT("Next"));
				Wire(Low, TEXT("Out"), Pick, TEXT("Min"));
				Set(Pick, TEXT("Max"), 1.f);
				const FMetaSoundNodeHandle Gain = MulFloat(Column + 2, Row);
				Wire(Pick, TEXT("Value"), Gain, TEXT("PrimaryOperand"));
				From(TEXT("Volume"), Gain, TEXT("AdditionalOperands"));
				return Out(Gain, TEXT("Out"));
			}

			/** Time input x Factor, as a Time. */
			FMetaSoundBuilderNodeOutputHandle TimeTimes(const TCHAR* GraphInput, const float Factor, const float Column, const float Row)
			{
				const FMetaSoundNodeHandle Scaled = Node(TEXT("UE"), TEXT("Multiply"), TEXT("Time by Float"), Column, Row);
				From(GraphInput, Scaled, TEXT("PrimaryOperand"));
				Set(Scaled, TEXT("AdditionalOperands"), Factor);
				return Out(Scaled, TEXT("Out"));
			}
		};

		// Enum values, as UE 5.8 registers them.
		constexpr int32 PinkNoise = 0;
		constexpr int32 WhiteNoise = 1;
		constexpr int32 LfoSine = 0;

		/** Pink noise through a band pass that a slow LFO sweeps, and a gust swell on the level. */
		void BuildWind(FGraph& G)
		{
			const FMetaSoundNodeHandle Noise = G.Node(TEXT("UE"), TEXT("Noise"), TEXT("Audio"), 0, 0);
			G.Set(Noise, TEXT("Type"), PinkNoise);
			const FMetaSoundNodeHandle Gust = G.Node(TEXT("UE"), TEXT("LFO"), TEXT("Audio"), 0, 1);
			G.From(TEXT("Gust Rate"), Gust, TEXT("Frequency"));
			G.Set(Gust, TEXT("Shape"), LfoSine);
			G.Set(Gust, TEXT("Min Value"), 0.35f);
			G.Set(Gust, TEXT("Max Value"), 1.f);
			const FMetaSoundNodeHandle Cutoff = G.MulFloat(1, 1);
			G.Wire(Gust, TEXT("Out"), Cutoff, TEXT("PrimaryOperand"));
			G.From(TEXT("Brightness"), Cutoff, TEXT("AdditionalOperands"));
			const FMetaSoundNodeHandle Filter = G.Node(TEXT("UE"), TEXT("State Variable Filter"), TEXT("Audio"), 2, 0);
			G.Wire(Noise, TEXT("Audio"), Filter, TEXT("In"));
			G.Wire(Cutoff, TEXT("Out"), Filter, TEXT("Cutoff Frequency"));
			G.Set(Filter, TEXT("Resonance"), 0.4f);
			const FMetaSoundNodeHandle Body = G.Mixer(2, 3, 0);
			G.Wire(Filter, TEXT("Band Pass"), Body, TEXT("In 0"));
			G.Set(Body, TEXT("Gain 0"), 1.f);
			G.Wire(Filter, TEXT("Low Pass Filter"), Body, TEXT("In 1"));
			G.Set(Body, TEXT("Gain 1"), 0.35f);
			// A second, slower-drifting swell, so gusts do not repeat on the beat.
			const FMetaSoundNodeHandle SwellRate = G.MulFloat(0, 2);
			G.From(TEXT("Gust Rate"), SwellRate, TEXT("PrimaryOperand"));
			G.Set(SwellRate, TEXT("AdditionalOperands"), 1.7f);
			const FMetaSoundNodeHandle Swell = G.Node(TEXT("UE"), TEXT("LFO"), TEXT("Audio"), 1, 2);
			G.Wire(SwellRate, TEXT("Out"), Swell, TEXT("Frequency"));
			G.Set(Swell, TEXT("Min Value"), 0.5f);
			G.Set(Swell, TEXT("Max Value"), 1.f);
			const FMetaSoundNodeHandle Level = G.MulFloat(2, 2);
			G.Wire(Swell, TEXT("Out"), Level, TEXT("PrimaryOperand"));
			G.From(TEXT("Intensity"), Level, TEXT("AdditionalOperands"));
			const FMetaSoundNodeHandle Gain = G.MulAudioByFloat(4, 0);
			G.Wire(Body, TEXT("Out"), Gain, TEXT("PrimaryOperand"));
			G.Wire(Level, TEXT("Out"), Gain, TEXT("AdditionalOperands"));
			G.ToOut(Gain, TEXT("Out"));
		}

		/** A sine with a fast attack and a short decay. */
		void BuildBlip(FGraph& G)
		{
			const FMetaSoundNodeHandle Tone = G.Node(TEXT("UE"), TEXT("Sine"), TEXT("Audio"), 0, 0);
			G.From(TEXT("Pitch"), Tone, TEXT("Frequency"));
			const FMetaSoundNodeHandle Envelope = G.Envelope(true, 0, 1);
			G.Play(Envelope, TEXT("Trigger"));
			G.Set(Envelope, TEXT("Attack Time"), 0.002f);
			G.From(TEXT("Length"), Envelope, TEXT("Decay Time"));
			G.Set(Envelope, TEXT("Decay Curve"), 2.f);
			const FMetaSoundNodeHandle Shaped = G.MulAudio(1, 0);
			G.Wire(Tone, TEXT("Audio"), Shaped, TEXT("PrimaryOperand"));
			G.Wire(Envelope, TEXT("Out Envelope"), Shaped, TEXT("AdditionalOperands"));
			const FMetaSoundNodeHandle Gain = G.MulAudioByFloat(2, 0);
			G.Wire(Shaped, TEXT("Out"), Gain, TEXT("PrimaryOperand"));
			G.From(TEXT("Volume"), Gain, TEXT("AdditionalOperands"));
			G.ToOut(Gain, TEXT("Out"));
			G.Finish(Envelope, TEXT("On Done"));
		}

		/** Two rising notes: the second starts as the first ends, an interval above it, and rings a little longer. */
		void BuildConfirm(FGraph& G)
		{
			const FMetaSoundNodeHandle First = G.Node(TEXT("UE"), TEXT("Sine"), TEXT("Audio"), 1, 0);
			G.From(TEXT("Pitch"), First, TEXT("Frequency"));
			const FMetaSoundNodeHandle Upper = G.MulFloat(0, 2);
			G.From(TEXT("Pitch"), Upper, TEXT("PrimaryOperand"));
			G.From(TEXT("Interval"), Upper, TEXT("AdditionalOperands"));
			const FMetaSoundNodeHandle Second = G.Node(TEXT("UE"), TEXT("Sine"), TEXT("Audio"), 1, 2);
			G.Wire(Upper, TEXT("Out"), Second, TEXT("Frequency"));
			const FMetaSoundNodeHandle FirstEnvelope = G.Envelope(true, 1, 1);
			G.Play(FirstEnvelope, TEXT("Trigger"));
			G.Set(FirstEnvelope, TEXT("Attack Time"), 0.003f);
			G.From(TEXT("Note Length"), FirstEnvelope, TEXT("Decay Time"));
			const FMetaSoundNodeHandle Tail = G.Node(TEXT("UE"), TEXT("Multiply"), TEXT("Time by Float"), 0, 3);
			G.From(TEXT("Note Length"), Tail, TEXT("PrimaryOperand"));
			G.Set(Tail, TEXT("AdditionalOperands"), 1.8f);
			const FMetaSoundNodeHandle SecondEnvelope = G.Envelope(true, 1, 3);
			G.Wire(FirstEnvelope, TEXT("On Done"), SecondEnvelope, TEXT("Trigger"));
			G.Set(SecondEnvelope, TEXT("Attack Time"), 0.003f);
			G.Wire(Tail, TEXT("Out"), SecondEnvelope, TEXT("Decay Time"));
			const FMetaSoundNodeHandle FirstNote = G.MulAudio(2, 0);
			G.Wire(First, TEXT("Audio"), FirstNote, TEXT("PrimaryOperand"));
			G.Wire(FirstEnvelope, TEXT("Out Envelope"), FirstNote, TEXT("AdditionalOperands"));
			const FMetaSoundNodeHandle SecondNote = G.MulAudio(2, 2);
			G.Wire(Second, TEXT("Audio"), SecondNote, TEXT("PrimaryOperand"));
			G.Wire(SecondEnvelope, TEXT("Out Envelope"), SecondNote, TEXT("AdditionalOperands"));
			const FMetaSoundNodeHandle Both = G.AddAudio(3, 1);
			G.Wire(FirstNote, TEXT("Out"), Both, TEXT("PrimaryOperand"));
			G.Wire(SecondNote, TEXT("Out"), Both, TEXT("AdditionalOperands"));
			const FMetaSoundNodeHandle Gain = G.MulAudioByFloat(4, 1);
			G.Wire(Both, TEXT("Out"), Gain, TEXT("PrimaryOperand"));
			G.From(TEXT("Volume"), Gain, TEXT("AdditionalOperands"));
			G.ToOut(Gain, TEXT("Out"));
			G.Finish(SecondEnvelope, TEXT("On Done"));
		}

		/**
		 * The firing frequency of a four-stroke engine, RPM x cylinders / 120: a saw at it, a square an octave down and a
		 * little noise, through a ladder filter that load opens.
		 */
		void BuildEngine(FGraph& G)
		{
			const FMetaSoundNodeHandle Product = G.MulFloat(0, 0);
			G.From(TEXT("RPM"), Product, TEXT("PrimaryOperand"));
			G.From(TEXT("Cylinders"), Product, TEXT("AdditionalOperands"));
			const FMetaSoundNodeHandle Firing = G.MulFloat(1, 0);
			G.Wire(Product, TEXT("Out"), Firing, TEXT("PrimaryOperand"));
			G.Set(Firing, TEXT("AdditionalOperands"), 1.f / 120.f);
			const FMetaSoundNodeHandle Saw = G.Node(TEXT("UE"), TEXT("Saw"), TEXT("Audio"), 2, 0);
			G.Wire(Firing, TEXT("Out"), Saw, TEXT("Frequency"));
			const FMetaSoundNodeHandle Half = G.MulFloat(1, 1);
			G.Wire(Firing, TEXT("Out"), Half, TEXT("PrimaryOperand"));
			G.Set(Half, TEXT("AdditionalOperands"), 0.5f);
			const FMetaSoundNodeHandle Sub = G.Node(TEXT("UE"), TEXT("Square"), TEXT("Audio"), 2, 1);
			G.Wire(Half, TEXT("Out"), Sub, TEXT("Frequency"));
			const FMetaSoundNodeHandle Rumble = G.Node(TEXT("UE"), TEXT("Noise"), TEXT("Audio"), 2, 2);
			G.Set(Rumble, TEXT("Type"), PinkNoise);
			const FMetaSoundNodeHandle Mix = G.Mixer(3, 3, 1);
			G.Wire(Saw, TEXT("Audio"), Mix, TEXT("In 0"));
			G.Set(Mix, TEXT("Gain 0"), 0.5f);
			G.Wire(Sub, TEXT("Audio"), Mix, TEXT("In 1"));
			G.Set(Mix, TEXT("Gain 1"), 0.35f);
			G.Wire(Rumble, TEXT("Audio"), Mix, TEXT("In 2"));
			G.Set(Mix, TEXT("Gain 2"), 0.08f);
			const FMetaSoundNodeHandle Opening = G.MulFloat(2, 3);
			G.From(TEXT("Load"), Opening, TEXT("PrimaryOperand"));
			G.Set(Opening, TEXT("AdditionalOperands"), 2600.f);
			const FMetaSoundNodeHandle Cutoff = G.AddFloat(3, 3);
			G.Wire(Opening, TEXT("Out"), Cutoff, TEXT("PrimaryOperand"));
			G.Set(Cutoff, TEXT("AdditionalOperands"), 250.f);
			const FMetaSoundNodeHandle Filter = G.Node(TEXT("UE"), TEXT("Ladder Filter"), TEXT("Audio"), 4, 1);
			G.Wire(Mix, TEXT("Out"), Filter, TEXT("In"));
			G.Wire(Cutoff, TEXT("Out"), Filter, TEXT("Cutoff Frequency"));
			G.Set(Filter, TEXT("Resonance"), 2.f);
			const FMetaSoundNodeHandle Gain = G.MulAudioByFloat(5, 1);
			G.Wire(Filter, TEXT("Out"), Gain, TEXT("PrimaryOperand"));
			G.From(TEXT("Volume"), Gain, TEXT("AdditionalOperands"));
			G.ToOut(Gain, TEXT("Out"));
		}

		/** A filtered white-noise bed, and drops: noise bursts through a band pass whose centre is picked at random per drop. */
		void BuildRain(FGraph& G)
		{
			const FMetaSoundNodeHandle Hiss = G.Node(TEXT("UE"), TEXT("Noise"), TEXT("Audio"), 0, 0);
			G.Set(Hiss, TEXT("Type"), WhiteNoise);
			const FMetaSoundNodeHandle Low = G.Node(TEXT("UE"), TEXT("One-Pole High Pass Filter"), TEXT("Audio"), 1, 0);
			G.Wire(Hiss, TEXT("Audio"), Low, TEXT("In"));
			G.Set(Low, TEXT("Cutoff Frequency"), 300.f);
			const FMetaSoundNodeHandle Bed = G.Node(TEXT("UE"), TEXT("One-Pole Low Pass Filter"), TEXT("Audio"), 2, 0);
			G.Wire(Low, TEXT("Out"), Bed, TEXT("In"));
			G.From(TEXT("Brightness"), Bed, TEXT("Cutoff Frequency"));
			const FMetaSoundNodeHandle Clock = G.Node(TEXT("UE"), TEXT("TriggerRepeat"), TEXT(""), 0, 2);
			G.Play(Clock, TEXT("Start"));
			G.From(TEXT("Drop Interval"), Clock, TEXT("Period"));
			const FMetaSoundNodeHandle Pick = G.Node(TEXT("UE"), TEXT("RandomFloat"), TEXT(""), 1, 2);
			G.Wire(Clock, TEXT("RepeatOut"), Pick, TEXT("Next"));
			G.Set(Pick, TEXT("Min"), 1500.f);
			G.Set(Pick, TEXT("Max"), 7000.f);
			const FMetaSoundNodeHandle Burst = G.Envelope(true, 1, 3);
			G.Wire(Clock, TEXT("RepeatOut"), Burst, TEXT("Trigger"));
			G.Set(Burst, TEXT("Attack Time"), 0.001f);
			G.Set(Burst, TEXT("Decay Time"), 0.025f);
			const FMetaSoundNodeHandle Splash = G.Node(TEXT("UE"), TEXT("Noise"), TEXT("Audio"), 0, 1);
			G.Set(Splash, TEXT("Type"), WhiteNoise);
			G.Set(Splash, TEXT("Seed"), 7);
			const FMetaSoundNodeHandle Tone = G.Node(TEXT("UE"), TEXT("State Variable Filter"), TEXT("Audio"), 2, 1);
			G.Wire(Splash, TEXT("Audio"), Tone, TEXT("In"));
			G.Wire(Pick, TEXT("Value"), Tone, TEXT("Cutoff Frequency"));
			G.Set(Tone, TEXT("Resonance"), 2.f);
			const FMetaSoundNodeHandle Drops = G.MulAudio(3, 2);
			G.Wire(Tone, TEXT("Band Pass"), Drops, TEXT("PrimaryOperand"));
			G.Wire(Burst, TEXT("Out Envelope"), Drops, TEXT("AdditionalOperands"));
			const FMetaSoundNodeHandle Mix = G.Mixer(2, 4, 1);
			G.Wire(Bed, TEXT("Out"), Mix, TEXT("In 0"));
			G.Set(Mix, TEXT("Gain 0"), 0.6f);
			G.Wire(Drops, TEXT("Out"), Mix, TEXT("In 1"));
			G.Set(Mix, TEXT("Gain 1"), 0.9f);
			const FMetaSoundNodeHandle Gain = G.MulAudioByFloat(5, 1);
			G.Wire(Mix, TEXT("Out"), Gain, TEXT("PrimaryOperand"));
			G.From(TEXT("Intensity"), Gain, TEXT("AdditionalOperands"));
			G.ToOut(Gain, TEXT("Out"));
		}

		/** A square and a detuned saw swept from the start pitch down to the end pitch, through a resonant ladder filter. */
		void BuildLaser(FGraph& G)
		{
			const FMetaSoundNodeHandle Sweep = G.Envelope(false, 0, 0);
			G.Play(Sweep, TEXT("Trigger"));
			G.Set(Sweep, TEXT("Attack Time"), 0.001f);
			G.From(TEXT("Length"), Sweep, TEXT("Decay Time"));
			G.Set(Sweep, TEXT("Decay Curve"), 1.5f);
			const FMetaSoundNodeHandle Pitch = G.Node(TEXT("MapRange"), TEXT("MapRange"), TEXT("Float"), 1, 0);
			G.Wire(Sweep, TEXT("Out Envelope"), Pitch, TEXT("In"));
			G.Set(Pitch, TEXT("In Range A"), 0.f);
			G.Set(Pitch, TEXT("In Range B"), 1.f);
			G.From(TEXT("End Pitch"), Pitch, TEXT("Out Range A"));
			G.From(TEXT("Start Pitch"), Pitch, TEXT("Out Range B"));
			const FMetaSoundNodeHandle Square = G.Node(TEXT("UE"), TEXT("Square"), TEXT("Audio"), 2, 0);
			G.Wire(Pitch, TEXT("Out Value"), Square, TEXT("Frequency"));
			const FMetaSoundNodeHandle Detune = G.MulFloat(2, 1);
			G.Wire(Pitch, TEXT("Out Value"), Detune, TEXT("PrimaryOperand"));
			G.Set(Detune, TEXT("AdditionalOperands"), 1.012f);
			const FMetaSoundNodeHandle Saw = G.Node(TEXT("UE"), TEXT("Saw"), TEXT("Audio"), 3, 1);
			G.Wire(Detune, TEXT("Out"), Saw, TEXT("Frequency"));
			const FMetaSoundNodeHandle Mix = G.Mixer(2, 4, 0);
			G.Wire(Square, TEXT("Audio"), Mix, TEXT("In 0"));
			G.Set(Mix, TEXT("Gain 0"), 0.5f);
			G.Wire(Saw, TEXT("Audio"), Mix, TEXT("In 1"));
			G.Set(Mix, TEXT("Gain 1"), 0.5f);
			const FMetaSoundNodeHandle Filter = G.Node(TEXT("UE"), TEXT("Ladder Filter"), TEXT("Audio"), 5, 0);
			G.Wire(Mix, TEXT("Out"), Filter, TEXT("In"));
			G.Set(Filter, TEXT("Cutoff Frequency"), 6000.f);
			G.Set(Filter, TEXT("Resonance"), 3.f);
			const FMetaSoundNodeHandle Amp = G.Envelope(true, 5, 2);
			G.Play(Amp, TEXT("Trigger"));
			G.Set(Amp, TEXT("Attack Time"), 0.002f);
			G.From(TEXT("Length"), Amp, TEXT("Decay Time"));
			const FMetaSoundNodeHandle Shaped = G.MulAudio(6, 0);
			G.Wire(Filter, TEXT("Out"), Shaped, TEXT("PrimaryOperand"));
			G.Wire(Amp, TEXT("Out Envelope"), Shaped, TEXT("AdditionalOperands"));
			const FMetaSoundNodeHandle Gain = G.MulAudioByFloat(7, 0);
			G.Wire(Shaped, TEXT("Out"), Gain, TEXT("PrimaryOperand"));
			G.From(TEXT("Volume"), Gain, TEXT("AdditionalOperands"));
			G.ToOut(Gain, TEXT("Out"));
			G.Finish(Amp, TEXT("On Done"));
		}

		/** A take per step from Sounds, never the same twice running, with pitch and volume jitter. */
		void BuildFootstep(FGraph& G)
		{
			const FMetaSoundBuilderNodeOutputHandle Takes = G.WaveArray(TEXT("Sounds"), G.Waves);
			const FMetaSoundNodeHandle Player = G.Picker(Takes, G.Waves.Num(), nullptr, 1, 0);
			G.Connect(G.PitchJitter(0, 2), G.In(Player, TEXT("Pitch Shift")), TEXT("pitch jitter -> Pitch Shift"));
			const FMetaSoundBuilderNodeOutputHandle Level = G.Level(1, 3);
			for (int32 Channel = 0; Channel < G.Channels(); ++Channel)
			{
				const FMetaSoundNodeHandle Gain = G.MulAudioByFloat(4, Channel);
				G.Wire(Player, G.PlayerOut(Channel), Gain, TEXT("PrimaryOperand"));
				G.Connect(Level, G.In(Gain, TEXT("AdditionalOperands")), TEXT("level -> gain"));
				G.ToOut(Gain, TEXT("Out"), Channel);
			}
			G.Finish(Player, TEXT("On Finished"));
		}

		/** A transient layer and, Tail Delay later, a tail layer, each a take per hit, sharing the pitch and level jitter. */
		void BuildImpact(FGraph& G)
		{
			const FMetaSoundBuilderNodeOutputHandle Transients = G.WaveArray(TEXT("Transients"), G.Waves);
			const FMetaSoundNodeHandle Hit = G.Picker(Transients, G.Waves.Num(), nullptr, 1, 0);
			const FMetaSoundBuilderNodeOutputHandle Pitch = G.PitchJitter(0, 4);
			G.Connect(Pitch, G.In(Hit, TEXT("Pitch Shift")), TEXT("pitch jitter -> transient"));
			const FMetaSoundBuilderNodeOutputHandle Level = G.Level(1, 5);
			const FMetaSoundNodeHandle HitGain = G.MulFloat(4, 5);
			G.Connect(Level, G.In(HitGain, TEXT("PrimaryOperand")), TEXT("level -> transient gain"));
			G.From(TEXT("Transient Gain"), HitGain, TEXT("AdditionalOperands"));
			if (G.Tails.IsEmpty())
			{
				for (int32 Channel = 0; Channel < G.Channels(); ++Channel)
				{
					const FMetaSoundNodeHandle Gain = G.MulAudioByFloat(5, Channel);
					G.Wire(Hit, G.PlayerOut(Channel), Gain, TEXT("PrimaryOperand"));
					G.Wire(HitGain, TEXT("Out"), Gain, TEXT("AdditionalOperands"));
					G.ToOut(Gain, TEXT("Out"), Channel);
				}
				G.Finish(Hit, TEXT("On Finished"));
				return;
			}
			const FMetaSoundNodeHandle Delay = G.Node(TEXT("UE"), TEXT("Trigger Delay"), TEXT(""), 0, 2);
			G.Play(Delay, TEXT("In"));
			G.From(TEXT("Tail Delay"), Delay, TEXT("Delay Time"));
			const FMetaSoundBuilderNodeOutputHandle Delayed = G.Out(Delay, TEXT("Out"));
			const FMetaSoundBuilderNodeOutputHandle TailTakes = G.WaveArray(TEXT("Tails"), G.Tails);
			const FMetaSoundNodeHandle Tail = G.Picker(TailTakes, G.Tails.Num(), &Delayed, 1, 2);
			G.Connect(Pitch, G.In(Tail, TEXT("Pitch Shift")), TEXT("pitch jitter -> tail"));
			const FMetaSoundNodeHandle TailGain = G.MulFloat(4, 6);
			G.Connect(Level, G.In(TailGain, TEXT("PrimaryOperand")), TEXT("level -> tail gain"));
			G.From(TEXT("Tail Gain"), TailGain, TEXT("AdditionalOperands"));
			for (int32 Channel = 0; Channel < G.Channels(); ++Channel)
			{
				const FMetaSoundNodeHandle Mix = G.Mixer(2, 5, Channel);
				G.Wire(Hit, G.PlayerOut(Channel), Mix, TEXT("In 0"));
				G.Wire(HitGain, TEXT("Out"), Mix, TEXT("Gain 0"));
				G.Wire(Tail, G.PlayerOut(Channel), Mix, TEXT("In 1"));
				G.Wire(TailGain, TEXT("Out"), Mix, TEXT("Gain 1"));
				G.ToOut(Mix, TEXT("Out"), Channel);
			}
			// Finished once both layers are, whichever ends last.
			const FMetaSoundNodeHandle Both = G.Node(TEXT("TriggerAccumulator"), TEXT("Trigger Accumulate (2)"), TEXT(""), 5, 3);
			G.Wire(Hit, TEXT("On Finished"), Both, TEXT("In 0"));
			G.Wire(Tail, TEXT("On Finished"), Both, TEXT("In 1"));
			G.Finish(Both, TEXT("Out"));
		}

		/** A band pass swept up from Sweep Low to Sweep High and back over Length: through the takes, or pink noise when there are none. */
		void BuildWhoosh(FGraph& G)
		{
			const FMetaSoundBuilderNodeOutputHandle Rise = G.TimeTimes(TEXT("Length"), 0.55f, 0, 4);
			const FMetaSoundBuilderNodeOutputHandle Fall = G.TimeTimes(TEXT("Length"), 0.45f, 0, 5);
			const FMetaSoundNodeHandle Sweep = G.Envelope(false, 1, 4);
			G.Play(Sweep, TEXT("Trigger"));
			G.Connect(Rise, G.In(Sweep, TEXT("Attack Time")), TEXT("rise -> sweep"));
			G.Connect(Fall, G.In(Sweep, TEXT("Decay Time")), TEXT("fall -> sweep"));
			const FMetaSoundNodeHandle Cutoff = G.Node(TEXT("MapRange"), TEXT("MapRange"), TEXT("Float"), 2, 4);
			G.Wire(Sweep, TEXT("Out Envelope"), Cutoff, TEXT("In"));
			G.Set(Cutoff, TEXT("In Range A"), 0.f);
			G.Set(Cutoff, TEXT("In Range B"), 1.f);
			G.From(TEXT("Sweep Low"), Cutoff, TEXT("Out Range A"));
			G.From(TEXT("Sweep High"), Cutoff, TEXT("Out Range B"));
			const bool bTakes = !G.Waves.IsEmpty();
			FMetaSoundNodeHandle Source;
			FMetaSoundNodeHandle Amp;
			if (bTakes)
			{
				const FMetaSoundBuilderNodeOutputHandle Takes = G.WaveArray(TEXT("Sounds"), G.Waves);
				Source = G.Picker(Takes, G.Waves.Num(), nullptr, 1, 0);
				G.Connect(G.PitchJitter(0, 2), G.In(Source, TEXT("Pitch Shift")), TEXT("pitch jitter -> Pitch Shift"));
			}
			else
			{
				Source = G.Node(TEXT("UE"), TEXT("Noise"), TEXT("Audio"), 2, 0);
				G.Set(Source, TEXT("Type"), PinkNoise);
				Amp = G.Envelope(true, 3, 3);
				G.Play(Amp, TEXT("Trigger"));
				G.Connect(Rise, G.In(Amp, TEXT("Attack Time")), TEXT("rise -> amp"));
				G.Connect(Fall, G.In(Amp, TEXT("Decay Time")), TEXT("fall -> amp"));
				G.Set(Amp, TEXT("Attack Curve"), 2.f);
				G.Set(Amp, TEXT("Decay Curve"), 0.6f);
			}
			for (int32 Channel = 0; Channel < (bTakes ? G.Channels() : 1); ++Channel)
			{
				const FMetaSoundNodeHandle Filter = G.Node(TEXT("UE"), TEXT("State Variable Filter"), TEXT("Audio"), 3, Channel);
				G.Wire(Source, bTakes ? G.PlayerOut(Channel) : TEXT("Audio"), Filter, TEXT("In"));
				G.Wire(Cutoff, TEXT("Out Value"), Filter, TEXT("Cutoff Frequency"));
				G.From(TEXT("Resonance"), Filter, TEXT("Resonance"));
				FMetaSoundNodeHandle Shaped;
				if (bTakes)
				{
					// Mostly the swept band, with some of the take under it so it keeps its body.
					Shaped = G.Mixer(2, 4, Channel);
					G.Wire(Filter, TEXT("Band Pass"), Shaped, TEXT("In 0"));
					G.Set(Shaped, TEXT("Gain 0"), 0.9f);
					G.Wire(Source, G.PlayerOut(Channel), Shaped, TEXT("In 1"));
					G.Set(Shaped, TEXT("Gain 1"), 0.35f);
				}
				else
				{
					Shaped = G.MulAudio(4, Channel);
					G.Wire(Filter, TEXT("Band Pass"), Shaped, TEXT("PrimaryOperand"));
					G.Wire(Amp, TEXT("Out Envelope"), Shaped, TEXT("AdditionalOperands"));
				}
				const FMetaSoundNodeHandle Gain = G.MulAudioByFloat(5, Channel);
				G.Wire(Shaped, TEXT("Out"), Gain, TEXT("PrimaryOperand"));
				G.From(TEXT("Volume"), Gain, TEXT("AdditionalOperands"));
				G.ToOut(Gain, TEXT("Out"), Channel);
			}
			G.Finish(bTakes ? Source : Amp, bTakes ? TEXT("On Finished") : TEXT("On Done"));
		}

		/**
		 * Two looping players, each picking a bed when its envelope is silent and fading it up over Segment and down over the
		 * next: player A every 2 x Segment from the start, player B Segment later, so one is always rising while the other falls.
		 */
		void BuildCrossfadeLoop(FGraph& G)
		{
			const FMetaSoundBuilderNodeOutputHandle Beds = G.WaveArray(TEXT("Beds"), G.Waves);
			const FMetaSoundBuilderNodeOutputHandle Cycle = G.TimeTimes(TEXT("Segment"), 2.f, 0, 3);
			const FMetaSoundNodeHandle ClockA = G.Node(TEXT("UE"), TEXT("TriggerRepeat"), TEXT(""), 1, 0);
			G.Play(ClockA, TEXT("Start"));
			G.Connect(Cycle, G.In(ClockA, TEXT("Period")), TEXT("cycle -> clock A"));
			const FMetaSoundNodeHandle Offset = G.Node(TEXT("UE"), TEXT("Trigger Delay"), TEXT(""), 0, 4);
			G.Play(Offset, TEXT("In"));
			G.From(TEXT("Segment"), Offset, TEXT("Delay Time"));
			const FMetaSoundNodeHandle ClockB = G.Node(TEXT("UE"), TEXT("TriggerRepeat"), TEXT(""), 1, 4);
			G.Wire(Offset, TEXT("Out"), ClockB, TEXT("Start"));
			G.Connect(Cycle, G.In(ClockB, TEXT("Period")), TEXT("cycle -> clock B"));
			FMetaSoundNodeHandle Players[2];
			FMetaSoundNodeHandle Envelopes[2];
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const FMetaSoundBuilderNodeOutputHandle Tick = G.Out(Side == 0 ? ClockA : ClockB, TEXT("RepeatOut"));
				Players[Side] = G.Picker(Beds, G.Waves.Num(), &Tick, 2, Side * 4);
				G.Set(Players[Side], TEXT("Loop"), true);
				Envelopes[Side] = G.Envelope(true, 3, Side * 4 + 2);
				G.Connect(Tick, G.In(Envelopes[Side], TEXT("Trigger")), TEXT("clock -> envelope"));
				G.From(TEXT("Segment"), Envelopes[Side], TEXT("Attack Time"));
				G.From(TEXT("Segment"), Envelopes[Side], TEXT("Decay Time"));
				// Close to equal power: a fast early rise and a late fall keep the sum of two unrelated beds level.
				G.Set(Envelopes[Side], TEXT("Attack Curve"), 0.6f);
				G.Set(Envelopes[Side], TEXT("Decay Curve"), 1.6f);
			}
			for (int32 Channel = 0; Channel < G.Channels(); ++Channel)
			{
				FMetaSoundNodeHandle Faded[2];
				for (int32 Side = 0; Side < 2; ++Side)
				{
					Faded[Side] = G.MulAudio(4, Side * 4 + Channel);
					G.Wire(Players[Side], G.PlayerOut(Channel), Faded[Side], TEXT("PrimaryOperand"));
					G.Wire(Envelopes[Side], TEXT("Out Envelope"), Faded[Side], TEXT("AdditionalOperands"));
				}
				const FMetaSoundNodeHandle Sum = G.AddAudio(5, Channel);
				G.Wire(Faded[0], TEXT("Out"), Sum, TEXT("PrimaryOperand"));
				G.Wire(Faded[1], TEXT("Out"), Sum, TEXT("AdditionalOperands"));
				const FMetaSoundNodeHandle Gain = G.MulAudioByFloat(6, Channel);
				G.Wire(Sum, TEXT("Out"), Gain, TEXT("PrimaryOperand"));
				G.From(TEXT("Volume"), Gain, TEXT("AdditionalOperands"));
				G.ToOut(Gain, TEXT("Out"), Channel);
			}
		}

		const TArray<FRecipe>& AllRecipes()
		{
			static const TArray<FRecipe> Table = {
				{TEXT("wind_loop"), TEXT("Wind: pink noise through a band pass a slow LFO sweeps, with gusts on the level. Loops."), false, {
					{TEXT("Intensity"), TEXT("Float"), 0.6f, 0.f, 1.f, TEXT("How loud the wind is.")},
					{TEXT("Gust Rate"), TEXT("Float"), 0.15f, 0.02f, 2.f, TEXT("Gusts per second.")},
					{TEXT("Brightness"), TEXT("Float"), 900.f, 150.f, 5000.f, TEXT("The band's centre at the top of a gust, in Hz.")}}, &BuildWind},
				{TEXT("ui_blip"), TEXT("A UI blip: a sine with a fast attack and short decay. One-shot."), true, {
					{TEXT("Pitch"), TEXT("Float"), 1200.f, 200.f, 5000.f, TEXT("Hz.")},
					{TEXT("Length"), TEXT("Time"), 0.08f, 0.02f, 0.5f, TEXT("The decay, in seconds.")},
					{TEXT("Volume"), TEXT("Float"), 0.5f, 0.f, 1.f, TEXT("Linear gain.")}}, &BuildBlip},
				{TEXT("ui_confirm"), TEXT("A UI confirm: two rising sine notes, the second an interval above the first. One-shot."), true, {
					{TEXT("Pitch"), TEXT("Float"), 660.f, 200.f, 3000.f, TEXT("The first note, in Hz.")},
					{TEXT("Interval"), TEXT("Float"), 1.5f, 1.f, 3.f, TEXT("The second note's ratio to the first: 1.5 a fifth, 1.335 a fourth, 2 an octave.")},
					{TEXT("Note Length"), TEXT("Time"), 0.09f, 0.03f, 0.4f, TEXT("Each note's decay in seconds; the second rings 1.8 times as long.")},
					{TEXT("Volume"), TEXT("Float"), 0.5f, 0.f, 1.f, TEXT("Linear gain.")}}, &BuildConfirm},
				{TEXT("engine_hum"), TEXT("An engine: saw and sub square at the firing frequency (RPM x cylinders / 120), a little noise, a ladder filter that load opens. Loops; drive RPM and Load from gameplay."), false, {
					{TEXT("RPM"), TEXT("Float"), 1500.f, 600.f, 9000.f, TEXT("Engine speed; set it every frame from the vehicle.")},
					{TEXT("Cylinders"), TEXT("Float"), 4.f, 1.f, 12.f, TEXT("Cylinder count of a four-stroke engine.")},
					{TEXT("Load"), TEXT("Float"), 0.4f, 0.f, 1.f, TEXT("Throttle or load: opens the filter.")},
					{TEXT("Volume"), TEXT("Float"), 0.5f, 0.f, 1.f, TEXT("Linear gain.")}}, &BuildEngine},
				{TEXT("rain_bed"), TEXT("Rain: a filtered white-noise bed and random band-passed drops. Loops."), false, {
					{TEXT("Intensity"), TEXT("Float"), 0.6f, 0.f, 1.f, TEXT("How loud the rain is.")},
					{TEXT("Brightness"), TEXT("Float"), 5000.f, 800.f, 14000.f, TEXT("The bed's low-pass cutoff, in Hz.")},
					{TEXT("Drop Interval"), TEXT("Time"), 0.05f, 0.01f, 0.5f, TEXT("Seconds between drops.")}}, &BuildRain},
				{TEXT("laser_zap"), TEXT("A laser zap: square and detuned saw swept from a high pitch down, through a resonant filter. One-shot."), true, {
					{TEXT("Start Pitch"), TEXT("Float"), 2400.f, 200.f, 8000.f, TEXT("Hz at the start.")},
					{TEXT("End Pitch"), TEXT("Float"), 180.f, 40.f, 2000.f, TEXT("Hz at the end.")},
					{TEXT("Length"), TEXT("Time"), 0.35f, 0.05f, 2.f, TEXT("Seconds.")},
					{TEXT("Volume"), TEXT("Float"), 0.5f, 0.f, 1.f, TEXT("Linear gain.")}}, &BuildLaser},
				{TEXT("footstep"), TEXT("Footsteps from your takes (Waves): one per step, never the same twice running, with pitch and volume jitter. One-shot; play it per step. ")
					TEXT("As a Sound Cue: a modulator over a no-repeat random of the takes."), true, {
					{TEXT("Pitch Jitter"), TEXT("Float"), 1.f, 0.f, 12.f, TEXT("Semitones up or down, picked per step.")},
					{TEXT("Volume Jitter"), TEXT("Float"), 0.15f, 0.f, 0.9f, TEXT("How far under full a step's level may be picked (0.15: 0.85 to 1).")},
					{TEXT("Volume"), TEXT("Float"), 1.f, 0.f, 2.f, TEXT("Linear gain.")}}, &BuildFootstep, EWaves::Required, TEXT("footstep")},
				{TEXT("impact"), TEXT("An impact: a transient take (Waves) and, Tail Delay later, a tail take (Tails: debris, ring, room), each picked per hit with shared pitch and level jitter. ")
					TEXT("One-shot, finished when both layers are. As a Sound Cue: a mixer of the two random layers, the tail behind a delay."), true, {
					{TEXT("Pitch Jitter"), TEXT("Float"), 0.5f, 0.f, 12.f, TEXT("Semitones up or down, picked per hit.")},
					{TEXT("Volume Jitter"), TEXT("Float"), 0.1f, 0.f, 0.9f, TEXT("How far under full a hit's level may be picked.")},
					{TEXT("Volume"), TEXT("Float"), 1.f, 0.f, 2.f, TEXT("Linear gain of the whole hit.")},
					{TEXT("Transient Gain"), TEXT("Float"), 1.f, 0.f, 2.f, TEXT("The transient layer's gain.")},
					{TEXT("Tail Gain"), TEXT("Float"), 0.7f, 0.f, 2.f, TEXT("The tail layer's gain.")},
					{TEXT("Tail Delay"), TEXT("Time"), 0.01f, 0.f, 1.f, TEXT("Seconds from the transient to the tail.")}}, &BuildImpact, EWaves::Required, TEXT("impact")},
				{TEXT("whoosh"), TEXT("A whoosh: a band pass swept from Sweep Low up to Sweep High and back over Length, through your takes (Waves, one per play) or, with none, pink noise. ")
					TEXT("One-shot. As a Sound Cue (takes needed): a modulator over a no-repeat random, without the sweep."), true, {
					{TEXT("Length"), TEXT("Time"), 0.6f, 0.1f, 3.f, TEXT("Seconds of the sweep (and of the noise whoosh).")},
					{TEXT("Sweep Low"), TEXT("Float"), 250.f, 40.f, 4000.f, TEXT("The band's centre at the ends, Hz.")},
					{TEXT("Sweep High"), TEXT("Float"), 3200.f, 200.f, 16000.f, TEXT("The band's centre at the middle, Hz.")},
					{TEXT("Resonance"), TEXT("Float"), 1.5f, 0.f, 10.f, TEXT("How narrow the band is.")},
					{TEXT("Pitch Jitter"), TEXT("Float"), 1.f, 0.f, 12.f, TEXT("Takes: semitones up or down, picked per play.")},
					{TEXT("Volume"), TEXT("Float"), 0.7f, 0.f, 2.f, TEXT("Linear gain.")}}, &BuildWhoosh, EWaves::Optional, TEXT("whoosh")},
				{TEXT("crossfade_loop"), TEXT("An endless ambience bed from your takes (Waves): two looping players crossfading, each picking a new bed (never the same twice running) when it is silent. ")
					TEXT("Loops; it fades in over the first Segment. As a Sound Cue: the beds looped back to back, without crossfades."), false, {
					{TEXT("Segment"), TEXT("Time"), 6.f, 0.5f, 60.f, TEXT("Seconds each bed rises, then as long again falling, while the other moves the other way.")},
					{TEXT("Volume"), TEXT("Float"), 0.8f, 0.f, 2.f, TEXT("Linear gain.")}}, &BuildCrossfadeLoop, EWaves::Required, TEXT("crossfade_loop")},
			};
			return Table;
		}

		const FRecipe* FindRecipe(const FString& Id)
		{
			return AllRecipes().FindByPredicate([&Id](const FRecipe& Recipe) { return Id.Equals(Recipe.Id, ESearchCase::IgnoreCase); });
		}

		/** The source's graph made into an asset at Package and saved. */
		FString Finish(FGraph& G, const FString& Package, FString& OutError)
		{
			if (!G.Error.IsEmpty())
			{
				OutError = G.Error;
				return FString();
			}
			const FString Folder = FPackageName::GetLongPackagePath(Package);
			const FString Name = FPackageName::GetShortName(Package);
			if (!FHyperAIStudioSourceControl::MakeEditable({Package}, OutError))
			{
				return FString();
			}
			EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
			const TScriptInterface<IMetaSoundDocumentInterface> Built = UMetaSoundEditorSubsystem::GetChecked().BuildToAsset(G.Builder, TEXT("HyperAI Studio"), Name, Folder, Result);
			UObject* Asset = Built.GetObject();
			if (!Asset || Result != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = TEXT("The MetaSound Source could not be built into an asset.");
				return FString();
			}
			return FHyperAIStudioSourceControl::SaveAsset(*Asset, OutError) ? Asset->GetPathName() : FString();
		}
	}

	TArray<FHyperAIMetaSoundRecipeInfo> Recipes(const FString& Only)
	{
		TArray<FHyperAIMetaSoundRecipeInfo> Out;
		for (const FRecipe& Recipe : AllRecipes())
		{
			if (!Only.IsEmpty() && !Only.Equals(Recipe.Id, ESearchCase::IgnoreCase))
			{
				continue;
			}
			FHyperAIMetaSoundRecipeInfo& Info = Out.AddDefaulted_GetRef();
			Info.Recipe = Recipe.Id;
			Info.Description = Recipe.Description;
			Info.bOneShot = Recipe.bOneShot;
			Info.bUsesWaves = Recipe.Waves != EWaves::None;
			Info.bSoundCue = Recipe.Cue != nullptr;
			for (const FInput& Input : Recipe.Inputs)
			{
				FHyperAIMetaSoundInputInfo& Field = Info.Inputs.AddDefaulted_GetRef();
				Field.Name = Input.Name;
				Field.Type = Input.Type;
				Field.Default = Input.Default;
				Field.Min = Input.Min;
				Field.Max = Input.Max;
				Field.Description = Input.Description;
			}
		}
		return Out;
	}

	bool CheckInputs(const FString& RecipeId, const TArray<FHyperAIAudioSetting>& Inputs, FString& OutError)
	{
		const FRecipe* Recipe = FindRecipe(RecipeId);
		if (!Recipe)
		{
			OutError = FString::Printf(TEXT("Recipe is one of %s."), *FString::JoinBy(AllRecipes(), TEXT(", "), [](const FRecipe& R) { return FString(R.Id); }));
			return false;
		}
		for (const FHyperAIAudioSetting& Setting : Inputs)
		{
			const FInput* Input = Recipe->Inputs.FindByPredicate([&Setting](const FInput& In) { return Setting.Name.TrimStartAndEnd().Equals(In.Name, ESearchCase::IgnoreCase); });
			if (!Input)
			{
				OutError = FString::Printf(TEXT("%s has no input %s; its inputs are %s."), Recipe->Id, *Setting.Name,
					*FString::JoinBy(Recipe->Inputs, TEXT(", "), [](const FInput& In) { return FString(In.Name); }));
				return false;
			}
			const FString Text = Setting.Value.TrimStartAndEnd();
			const float Value = FCString::Atof(*Text);
			if (Text.IsEmpty() || !Text.IsNumeric() || Value < Input->Min || Value > Input->Max)
			{
				OutError = FString::Printf(TEXT("%s's %s is a number from %g to %g."), Recipe->Id, Input->Name, Input->Min, Input->Max);
				return false;
			}
		}
		return true;
	}

	bool CheckWaves(const FString& RecipeId, const FString& Kind, const TArray<USoundWave*>& Waves, const TArray<USoundWave*>& Tails, FString& OutError)
	{
		const FRecipe* Recipe = FindRecipe(RecipeId);
		if (!Recipe)
		{
			OutError = TEXT("Unknown recipe.");
			return false;
		}
		const bool bCue = Kind.Equals(TEXT("sound_cue"), ESearchCase::IgnoreCase);
		if (!Kind.IsEmpty() && !bCue && !Kind.Equals(TEXT("metasound"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("Kind is metasound (the default) or sound_cue.");
			return false;
		}
		if (bCue && !Recipe->Cue)
		{
			OutError = FString::Printf(TEXT("%s is a MetaSound only; Sound Cue versions exist for footstep, impact, whoosh and crossfade_loop."), Recipe->Id);
			return false;
		}
		if (Recipe->Waves == EWaves::None && (!Waves.IsEmpty() || !Tails.IsEmpty()))
		{
			OutError = FString::Printf(TEXT("%s is procedural and plays no SoundWaves; leave Waves and Tails empty."), Recipe->Id);
			return false;
		}
		if ((Recipe->Waves == EWaves::Required || bCue) && Waves.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s plays your SoundWaves: give their paths in Waves (hyper_sound_asset_inspect with a Folder lists them)."), Recipe->Id);
			return false;
		}
		if (!Tails.IsEmpty() && RecipeId != TEXT("impact"))
		{
			OutError = TEXT("Only impact takes Tails.");
			return false;
		}
		if (Waves.Num() > 32 || Tails.Num() > 32 || Waves.Contains(nullptr) || Tails.Contains(nullptr))
		{
			OutError = TEXT("Waves and Tails hold up to 32 SoundWaves each.");
			return false;
		}
		return true;
	}

	bool CueRecipe(const FString& RecipeId, const TArray<USoundWave*>& Waves, const TArray<USoundWave*>& Tails, const TArray<FHyperAIAudioSetting>& Inputs,
		TArray<FHyperAISoundCueNode>& OutNodes, FString& OutRoot, float& OutVolume, FString& OutError)
	{
		const FRecipe* Recipe = FindRecipe(RecipeId);
		if (!Recipe || !CheckInputs(RecipeId, Inputs, OutError) || !CheckWaves(RecipeId, TEXT("sound_cue"), Waves, Tails, OutError))
		{
			OutError = OutError.IsEmpty() ? TEXT("Unknown recipe.") : OutError;
			return false;
		}
		const auto Value = [&](const TCHAR* Name)
		{
			const FInput* Spec = Recipe->Inputs.FindByPredicate([Name](const FInput& In) { return FCString::Stricmp(In.Name, Name) == 0; });
			const FHyperAIAudioSetting* Given = Inputs.FindByPredicate([Name](const FHyperAIAudioSetting& Setting) { return Setting.Name.TrimStartAndEnd().Equals(Name, ESearchCase::IgnoreCase); });
			return Given ? FCString::Atof(*Given->Value.TrimStartAndEnd()) : Spec ? Spec->Default : 0.f;
		};
		OutNodes.Reset();
		const auto Players = [&OutNodes](const FString& Prefix, const TArray<USoundWave*>& Takes, const bool bLoop)
		{
			TArray<FString> Ids;
			for (int32 Index = 0; Index < Takes.Num(); ++Index)
			{
				FHyperAISoundCueNode& Player = OutNodes.AddDefaulted_GetRef();
				Player.Id = FString::Printf(TEXT("%s_%d"), *Prefix, Index + 1);
				Player.Kind = TEXT("wave_player");
				Player.Wave = Takes[Index]->GetPathName();
				Player.bLooping = bLoop;
				Ids.Add(Player.Id);
			}
			return Ids;
		};
		const auto Random = [&OutNodes](const FString& Id, const TArray<FString>& Children)
		{
			FHyperAISoundCueNode& Node = OutNodes.AddDefaulted_GetRef();
			Node.Id = Id;
			Node.Kind = TEXT("random");
			Node.Children = Children;
			Node.bNoRepeat = Children.Num() > 1;
			return Id;
		};
		const auto Modulator = [&OutNodes, &Value](const FString& Id, const FString& Child, const bool bVolume)
		{
			FHyperAISoundCueNode& Node = OutNodes.AddDefaulted_GetRef();
			Node.Id = Id;
			Node.Kind = TEXT("modulator");
			Node.Children = {Child};
			const float Semitones = Value(TEXT("Pitch Jitter"));
			Node.PitchMin = FMath::Pow(2.f, -Semitones / 12.f);
			Node.PitchMax = FMath::Pow(2.f, Semitones / 12.f);
			Node.VolumeMin = bVolume ? 1.f - Value(TEXT("Volume Jitter")) : 1.f;
			Node.VolumeMax = 1.f;
			return Id;
		};
		OutVolume = Value(TEXT("Volume"));
		if (RecipeId == TEXT("footstep") || RecipeId == TEXT("whoosh"))
		{
			OutRoot = TEXT("variation");
			FHyperAISoundCueNode& Root = OutNodes.AddDefaulted_GetRef();
			Root.Id = OutRoot;
			const int32 RootIndex = OutNodes.Num() - 1;
			const FString Pick = Random(TEXT("pick"), Players(TEXT("take"), Waves, false));
			OutNodes[RootIndex].Kind = TEXT("modulator");
			OutNodes[RootIndex].Children = {Pick};
			const float Semitones = Value(TEXT("Pitch Jitter"));
			OutNodes[RootIndex].PitchMin = FMath::Pow(2.f, -Semitones / 12.f);
			OutNodes[RootIndex].PitchMax = FMath::Pow(2.f, Semitones / 12.f);
			OutNodes[RootIndex].VolumeMin = RecipeId == TEXT("footstep") ? 1.f - Value(TEXT("Volume Jitter")) : 1.f;
			OutNodes[RootIndex].VolumeMax = 1.f;
			return true;
		}
		if (RecipeId == TEXT("impact"))
		{
			OutRoot = Tails.IsEmpty() ? FString(TEXT("hit")) : FString(TEXT("layers"));
			const FString Hit = Modulator(TEXT("hit"), Random(TEXT("hit_pick"), Players(TEXT("hit"), Waves, false)), true);
			if (Tails.IsEmpty())
			{
				OutVolume *= Value(TEXT("Transient Gain"));
				return true;
			}
			const FString TailMod = Modulator(TEXT("tail"), Random(TEXT("tail_pick"), Players(TEXT("tail"), Tails, false)), true);
			FHyperAISoundCueNode& Delay = OutNodes.AddDefaulted_GetRef();
			Delay.Id = TEXT("tail_delay");
			Delay.Kind = TEXT("delay");
			Delay.Children = {TailMod};
			Delay.DelayMin = Delay.DelayMax = Value(TEXT("Tail Delay"));
			FHyperAISoundCueNode& Mixer = OutNodes.AddDefaulted_GetRef();
			Mixer.Id = OutRoot;
			Mixer.Kind = TEXT("mixer");
			Mixer.Children = {Hit, Delay.Id};
			Mixer.Weights = {Value(TEXT("Transient Gain")), Value(TEXT("Tail Gain"))};
			return true;
		}
		// crossfade_loop: the beds back to back, a new one (never the same twice running) after each.
		OutRoot = TEXT("loop");
		FHyperAISoundCueNode& Loop = OutNodes.AddDefaulted_GetRef();
		Loop.Id = OutRoot;
		Loop.Kind = TEXT("looping");
		const int32 LoopIndex = OutNodes.Num() - 1;
		OutNodes[LoopIndex].Children = {Random(TEXT("bed"), Players(TEXT("bed"), Waves, false))};
		return true;
	}

	FString BuildRecipe(const FString& RecipeId, const FString& Package, const TArray<FHyperAIAudioSetting>& Inputs, const TArray<USoundWave*>& Waves,
		const TArray<USoundWave*>& Tails, int32& OutNodeCount, FString& OutError)
	{
		OutNodeCount = 0;
		const FRecipe* Recipe = FindRecipe(RecipeId);
		if (!Recipe || !CheckInputs(RecipeId, Inputs, OutError) || !CheckWaves(RecipeId, TEXT("metasound"), Waves, Tails, OutError))
		{
			OutError = OutError.IsEmpty() ? TEXT("Unknown recipe.") : OutError;
			return FString();
		}
		FGraph G;
		for (USoundWave* Wave : Waves)
		{
			G.Waves.Add(Wave);
			G.bStereo |= Wave->NumChannels >= 2;
		}
		for (USoundWave* Wave : Tails)
		{
			G.Tails.Add(Wave);
			G.bStereo |= Wave->NumChannels >= 2;
		}
		if (!G.Start(Recipe->bOneShot, G.bStereo))
		{
			OutError = G.Error;
			return FString();
		}
		for (const FInput& Input : Recipe->Inputs)
		{
			const FHyperAIAudioSetting* Given = Inputs.FindByPredicate([&Input](const FHyperAIAudioSetting& Setting) { return Setting.Name.TrimStartAndEnd().Equals(Input.Name, ESearchCase::IgnoreCase); });
			G.Input(Input, Given ? FCString::Atof(*Given->Value.TrimStartAndEnd()) : Input.Default);
		}
		Recipe->Build(G);
		OutNodeCount = G.NodeCount;
		return Finish(G, Package, OutError);
	}

	FString BuildPlayer(const FString& Package, const TArray<USoundWave*>& Waves, const FString& Mode, const FVector2D PitchSemitones, const FVector2D Volume,
		FString& OutError)
	{
		if (Waves.IsEmpty() || Waves.Contains(nullptr))
		{
			OutError = TEXT("A MetaSound player needs at least one SoundWave.");
			return FString();
		}
		const bool bRandom = Mode == TEXT("random");
		const bool bLoop = Mode == TEXT("loop");
		if (!bRandom && !bLoop && Mode != TEXT("one_shot"))
		{
			OutError = TEXT("A MetaSound player is one_shot, loop or random.");
			return FString();
		}
		int32 Channels = 1;
		for (const USoundWave* Wave : Waves)
		{
			Channels = FMath::Max(Channels, Wave->NumChannels);
		}
		const bool bStereo = Channels >= 2;
		FGraph G;
		if (!G.Start(!bLoop, bStereo))
		{
			OutError = G.Error;
			return FString();
		}
		const FMetaSoundNodeHandle Player = G.Node(TEXT("UE"), TEXT("Wave Player"), bStereo ? TEXT("Stereo") : TEXT("Mono"), 2, 0);
		if (bRandom)
		{
			// The waves as an input (so a designer can add takes), and Random Get never picking the last one again.
			FMetasoundFrontendLiteral Sounds;
			TArray<UObject*> Objects(Waves);
			Sounds.Set(Objects);
			EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
			const FMetaSoundBuilderNodeOutputHandle SoundsInput = G.Builder->AddGraphInputNode(TEXT("Sounds"), TEXT("WaveAsset:Array"), Sounds, Result);
			G.Expect(Result, TEXT("The MetaSound input Sounds could not be added."));
			const FMetaSoundNodeHandle Pick = G.Node(TEXT("Array"), TEXT("Random Get"), TEXT("WaveAsset:Array"), 1, 0);
			G.Connect(SoundsInput, G.In(Pick, TEXT("In Array")), TEXT("Sounds -> In Array"));
			G.Play(Pick, TEXT("Next"));
			G.Set(Pick, TEXT("No Repeats"), 1);
			G.Wire(Pick, TEXT("Value"), Player, TEXT("Wave Asset"));
			G.Wire(Pick, TEXT("On Next"), Player, TEXT("Play"));
		}
		else
		{
			G.Set(Player, TEXT("Wave Asset"), static_cast<UObject*>(Waves[0]));
			G.Play(Player, TEXT("Play"));
		}
		G.Set(Player, TEXT("Loop"), bLoop);
		if (!FMath::IsNearlyZero(PitchSemitones.X) || !FMath::IsNearlyZero(PitchSemitones.Y))
		{
			const FMetaSoundNodeHandle Pitch = G.Node(TEXT("UE"), TEXT("RandomFloat"), TEXT(""), 1, 1);
			G.Play(Pitch, TEXT("Next"));
			G.Set(Pitch, TEXT("Min"), static_cast<float>(FMath::Min(PitchSemitones.X, PitchSemitones.Y)));
			G.Set(Pitch, TEXT("Max"), static_cast<float>(FMath::Max(PitchSemitones.X, PitchSemitones.Y)));
			G.Wire(Pitch, TEXT("Value"), Player, TEXT("Pitch Shift"));
		}
		const bool bVolume = !FMath::IsNearlyEqual(Volume.X, 1.0) || !FMath::IsNearlyEqual(Volume.Y, 1.0);
		FMetaSoundNodeHandle Level;
		if (bVolume)
		{
			Level = G.Node(TEXT("UE"), TEXT("RandomFloat"), TEXT(""), 1, 2);
			G.Play(Level, TEXT("Next"));
			G.Set(Level, TEXT("Min"), static_cast<float>(FMath::Min(Volume.X, Volume.Y)));
			G.Set(Level, TEXT("Max"), static_cast<float>(FMath::Max(Volume.X, Volume.Y)));
		}
		const TArray<const TCHAR*> Outputs = bStereo ? TArray<const TCHAR*>{TEXT("Out Left"), TEXT("Out Right")} : TArray<const TCHAR*>{TEXT("Out Mono")};
		for (int32 Channel = 0; Channel < Outputs.Num(); ++Channel)
		{
			if (bVolume)
			{
				const FMetaSoundNodeHandle Gain = G.MulAudioByFloat(3, Channel);
				G.Wire(Player, Outputs[Channel], Gain, TEXT("PrimaryOperand"));
				G.Wire(Level, TEXT("Value"), Gain, TEXT("AdditionalOperands"));
				G.ToOut(Gain, TEXT("Out"), Channel);
			}
			else
			{
				G.ToOut(Player, Outputs[Channel], Channel);
			}
		}
		if (!bLoop)
		{
			G.Finish(Player, TEXT("On Finished"));
		}
		return Finish(G, Package, OutError);
	}

	TArray<FString> DescribeInputs(const UObject& MetaSoundSource)
	{
		TArray<FString> Out;
		if (const IMetaSoundDocumentInterface* Document = Cast<IMetaSoundDocumentInterface>(const_cast<UObject*>(&MetaSoundSource)))
		{
			for (const FMetasoundFrontendClassInput& Input : Document->GetConstDocument().RootGraph.GetDefaultInterface().Inputs)
			{
				Out.Add(FString::Printf(TEXT("input %s (%s)"), *Input.Name.ToString(), *Input.TypeName.ToString()));
			}
		}
		return Out;
	}
}
