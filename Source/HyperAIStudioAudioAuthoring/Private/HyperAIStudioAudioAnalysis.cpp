// Games by Hyper 2026.

#include "HyperAIStudioAudioAnalysis.h"

#include "AudioParameter.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioSoundCueGraph.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundGenerator.h"
#include "MetasoundSource.h"
#include "Modules/ModuleManager.h"
#include "Misc/App.h"
#include "Misc/DefaultValueHelper.h"
#include "Sound/SoundGenerator.h"
#include "Sound/SoundWave.h"

namespace HyperAIStudio::AudioAuthoring::Analysis
{
	namespace
	{
		constexpr float Floor = -144.f;

		float ToDb(const double Linear)
		{
			return Linear > 1e-8 ? static_cast<float>(20.0 * FMath::LogX(10.0, Linear)) : Floor;
		}

		/** A biquad, direct form I, one per channel. */
		struct FBiquad
		{
			double B0 = 1, B1 = 0, B2 = 0, A1 = 0, A2 = 0;
			double X1 = 0, X2 = 0, Y1 = 0, Y2 = 0;

			double Process(const double X)
			{
				const double Y = B0 * X + B1 * X1 + B2 * X2 - A1 * Y1 - A2 * Y2;
				X2 = X1; X1 = X; Y2 = Y1; Y1 = Y;
				return Y;
			}
		};

		/** BS.1770's K-weighting at any rate: a high shelf (+4 dB above about 1.5 kHz) then a high pass at about 38 Hz. */
		void MakeKWeighting(const double Rate, FBiquad& Shelf, FBiquad& HighPass)
		{
			{
				const double F0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
				const double K = FMath::Tan(PI * F0 / Rate);
				const double Vh = FMath::Pow(10.0, G / 20.0);
				const double Vb = FMath::Pow(Vh, 0.4996667741545416);
				const double A0 = 1.0 + K / Q + K * K;
				Shelf.B0 = (Vh + Vb * K / Q + K * K) / A0;
				Shelf.B1 = 2.0 * (K * K - Vh) / A0;
				Shelf.B2 = (Vh - Vb * K / Q + K * K) / A0;
				Shelf.A1 = 2.0 * (K * K - 1.0) / A0;
				Shelf.A2 = (1.0 - K / Q + K * K) / A0;
			}
			{
				const double F0 = 38.13547087602444, Q = 0.5003270373238773;
				const double K = FMath::Tan(PI * F0 / Rate);
				const double A0 = 1.0 + K / Q + K * K;
				HighPass.B0 = 1.0;
				HighPass.B1 = -2.0;
				HighPass.B2 = 1.0;
				HighPass.A1 = 2.0 * (K * K - 1.0) / A0;
				HighPass.A2 = (1.0 - K / Q + K * K) / A0;
			}
		}

		/** Integrated (gated) and the loudest momentary loudness, LUFS. */
		void Loudness(const FBuffer& Buffer, float& OutIntegrated, float& OutMaxMomentary)
		{
			const int32 Frames = Buffer.Samples.Num() / FMath::Max(1, Buffer.Channels);
			OutIntegrated = OutMaxMomentary = Floor;
			if (Frames <= 0) return;
			// Per-frame K-weighted power summed over channels (each weighted 1: mono, left and right).
			TArray<double> Power;
			Power.SetNumZeroed(Frames);
			for (int32 Channel = 0; Channel < Buffer.Channels; ++Channel)
			{
				FBiquad Shelf, HighPass;
				MakeKWeighting(Buffer.SampleRate, Shelf, HighPass);
				for (int32 Frame = 0; Frame < Frames; ++Frame)
				{
					const double Y = HighPass.Process(Shelf.Process(Buffer.Samples[Frame * Buffer.Channels + Channel]));
					Power[Frame] += Y * Y;
				}
			}
			const auto Lufs = [](const double MeanSquare) { return MeanSquare > 1e-15 ? static_cast<float>(-0.691 + 10.0 * FMath::LogX(10.0, MeanSquare)) : Floor; };
			const int32 Block = FMath::RoundToInt(0.4 * Buffer.SampleRate);
			const int32 Step = FMath::Max(1, Block / 4);
			TArray<double> Blocks;
			if (Frames < Block)
			{
				double Sum = 0;
				for (const double P : Power) Sum += P;
				Blocks.Add(Sum / Frames);
			}
			else
			{
				TArray<double> Prefix;
				Prefix.SetNumZeroed(Frames + 1);
				for (int32 Frame = 0; Frame < Frames; ++Frame) Prefix[Frame + 1] = Prefix[Frame] + Power[Frame];
				for (int32 Start = 0; Start + Block <= Frames; Start += Step)
				{
					Blocks.Add((Prefix[Start + Block] - Prefix[Start]) / Block);
				}
			}
			double Max = 0;
			for (const double B : Blocks) Max = FMath::Max(Max, B);
			OutMaxMomentary = Lufs(Max);
			// The absolute gate at -70 LUFS, then the relative gate 10 LU under the absolute-gated mean.
			double Sum = 0;
			int32 Count = 0;
			for (const double B : Blocks) if (Lufs(B) > -70.f) { Sum += B; ++Count; }
			if (Count == 0) return;
			const float Relative = Lufs(Sum / Count) - 10.f;
			Sum = 0;
			Count = 0;
			for (const double B : Blocks) if (Lufs(B) > -70.f && Lufs(B) > Relative) { Sum += B; ++Count; }
			OutIntegrated = Count > 0 ? Lufs(Sum / Count) : Floor;
		}

		/** An in-place radix-2 FFT of N (a power of two) complex values. */
		void Fft(TArray<double>& Re, TArray<double>& Im)
		{
			const int32 N = Re.Num();
			for (int32 I = 1, J = 0; I < N; ++I)
			{
				int32 Bit = N >> 1;
				for (; J & Bit; Bit >>= 1) J ^= Bit;
				J ^= Bit;
				if (I < J) { Swap(Re[I], Re[J]); Swap(Im[I], Im[J]); }
			}
			for (int32 Length = 2; Length <= N; Length <<= 1)
			{
				const double Angle = -2.0 * PI / Length;
				const double WRe = FMath::Cos(Angle), WIm = FMath::Sin(Angle);
				for (int32 I = 0; I < N; I += Length)
				{
					double CRe = 1, CIm = 0;
					for (int32 K = 0; K < Length / 2; ++K)
					{
						const int32 A = I + K, B = I + K + Length / 2;
						const double TRe = Re[B] * CRe - Im[B] * CIm, TIm = Re[B] * CIm + Im[B] * CRe;
						Re[B] = Re[A] - TRe; Im[B] = Im[A] - TIm;
						Re[A] += TRe; Im[A] += TIm;
						const double Next = CRe * WRe - CIm * WIm;
						CIm = CRe * WIm + CIm * WRe;
						CRe = Next;
					}
				}
			}
		}

		/** The centroid of the power spectrum summed over every 2048-sample frame within 50 dB of the loudest. */
		float Centroid(const TArray<float>& Mono, const int32 Rate)
		{
			constexpr int32 N = 2048;
			if (Mono.Num() < 64) return 0.f;
			TArray<double> Window;
			Window.SetNum(N);
			for (int32 I = 0; I < N; ++I) Window[I] = 0.5 - 0.5 * FMath::Cos(2.0 * PI * I / (N - 1));
			TArray<double> Spectrum;
			Spectrum.SetNumZeroed(N / 2);
			TArray<double> Energies;
			TArray<TArray<double>> Frames;
			for (int32 Start = 0; Start < Mono.Num(); Start += N / 2)
			{
				TArray<double> Re, Im;
				Re.SetNumZeroed(N);
				Im.SetNumZeroed(N);
				double Energy = 0;
				for (int32 I = 0; I < N && Start + I < Mono.Num(); ++I)
				{
					Re[I] = Mono[Start + I] * Window[I];
					Energy += Re[I] * Re[I];
				}
				Fft(Re, Im);
				TArray<double>& Power = Frames.AddDefaulted_GetRef();
				Power.SetNum(N / 2);
				for (int32 Bin = 0; Bin < N / 2; ++Bin) Power[Bin] = Re[Bin] * Re[Bin] + Im[Bin] * Im[Bin];
				Energies.Add(Energy);
			}
			double Loudest = 0;
			for (const double E : Energies) Loudest = FMath::Max(Loudest, E);
			const double Gate = Loudest * 1e-5;
			for (int32 Frame = 0; Frame < Frames.Num(); ++Frame)
			{
				if (Energies[Frame] < Gate) continue;
				for (int32 Bin = 0; Bin < N / 2; ++Bin) Spectrum[Bin] += Frames[Frame][Bin];
			}
			double Weighted = 0, Total = 0;
			for (int32 Bin = 1; Bin < N / 2; ++Bin)
			{
				Weighted += Spectrum[Bin] * Bin * Rate / static_cast<double>(N);
				Total += Spectrum[Bin];
			}
			return Total > 0 ? static_cast<float>(Weighted / Total) : 0.f;
		}

		/** A graph input's value for a render, typed by the input it sets. */
		bool MakeParameter(const UMetaSoundSource& Source, const FHyperAIAudioSetting& Setting, FAudioParameter& Out, FString& OutError)
		{
			const FName Name(*Setting.Name.TrimStartAndEnd());
			const IMetaSoundDocumentInterface* Document = Cast<IMetaSoundDocumentInterface>(const_cast<UMetaSoundSource*>(&Source));
			const FMetasoundFrontendClassInput* Input = Document ? Document->GetConstDocument().RootGraph.GetDefaultInterface().Inputs.FindByPredicate(
				[&Name](const FMetasoundFrontendClassInput& Each) { return Each.Name == Name; }) : nullptr;
			if (!Input)
			{
				OutError = FString::Printf(TEXT("%s has no input %s."), *Source.GetName(), *Setting.Name);
				return false;
			}
			const FString Text = Setting.Value.TrimStartAndEnd();
			const FString Type = Input->TypeName.ToString();
			if (Type == TEXT("Bool"))
			{
				Out = FAudioParameter(Name, Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text == TEXT("1"));
				return true;
			}
			if (Type == TEXT("Int32"))
			{
				int32 Value = 0;
				if (!FDefaultValueHelper::ParseInt(Text, Value)) { OutError = FString::Printf(TEXT("%s takes a whole number."), *Setting.Name); return false; }
				Out = FAudioParameter(Name, Value);
				return true;
			}
			if (Type == TEXT("Float") || Type == TEXT("Time"))
			{
				float Value = 0.f;
				if (!FDefaultValueHelper::ParseFloat(Text, Value)) { OutError = FString::Printf(TEXT("%s takes a number."), *Setting.Name); return false; }
				Out = FAudioParameter(Name, Value);
				return true;
			}
			OutError = FString::Printf(TEXT("%s is a %s input; a render sets Bool, Int32, Float and Time inputs."), *Setting.Name, *Type);
			return false;
		}
	}

	namespace
	{
		/** Every SoundWave a MetaSound's graph inputs and node values name. */
		void CollectWaves(const UMetaSoundSource& Source, TSet<USoundWave*>& OutWaves)
		{
			const IMetaSoundDocumentInterface* Document = Cast<IMetaSoundDocumentInterface>(const_cast<UMetaSoundSource*>(&Source));
			if (!Document)
			{
				return;
			}
			const auto Add = [&OutWaves](const FMetasoundFrontendLiteral& Literal)
			{
				UObject* One = nullptr;
				TArray<UObject*> Many;
				if (Literal.TryGet(One))
				{
					Many.Add(One);
				}
				else
				{
					Literal.TryGet(Many);
				}
				for (UObject* Object : Many)
				{
					if (USoundWave* Wave = Cast<USoundWave>(Object))
					{
						OutWaves.Add(Wave);
					}
				}
			};
			const FMetasoundFrontendDocument& Doc = Document->GetConstDocument();
			for (const FMetasoundFrontendClassInput& Input : Doc.RootGraph.GetDefaultInterface().Inputs)
			{
				if (const FMetasoundFrontendLiteral* Default = Input.FindConstDefault(Metasound::Frontend::DefaultPageID))
				{
					Add(*Default);
				}
			}
			for (const FMetasoundFrontendNode& Node : Doc.RootGraph.GetConstDefaultGraph().Nodes)
			{
				for (const FMetasoundFrontendVertexLiteral& Literal : Node.InputLiterals)
				{
					Add(Literal.Value);
				}
			}
		}
	}

	bool RenderMetaSound(UMetaSoundSource& Source, const int32 SampleRate, const float MaxSeconds, const TArray<FHyperAIAudioSetting>& Parameters, FBuffer& Out, FString& OutError)
	{
		TArray<FAudioParameter> Defaults;
		for (const FHyperAIAudioSetting& Setting : Parameters)
		{
			FAudioParameter& Parameter = Defaults.AddDefaulted_GetRef();
			if (!MakeParameter(Source, Setting, Parameter, OutError))
			{
				return false;
			}
		}
		// The Wave Player decodes through the platform's codecs, whose factories an audio device normally loads: without
		// one (tests, -nosound) they are loaded here, or every wave plays silence.
		for (const TCHAR* Codec : {TEXT("BinkAudioDecoder"), TEXT("RadAudioDecoder"), TEXT("VorbisAudioDecoder"), TEXT("AdpcmAudioDecoder"), TEXT("OpusAudioDecoder")})
		{
			FModuleManager::Get().LoadModule(Codec);
		}
		// An audio device loads a wave's compressed data when it starts playing it; with none, the waves are readied here.
		TSet<USoundWave*> Waves;
		CollectWaves(Source, Waves);
		for (USoundWave* Wave : Waves)
		{
			Wave->InitAudioResource(Wave->GetRuntimeFormat());
		}
		Source.InitResources();
		static uint64 NextInstance = 0x48414900000000ull;
		FSoundGeneratorInitParams Params;
		Params.AudioDeviceID = 0;
		Params.SampleRate = static_cast<float>(SampleRate);
		Params.NumChannels = FMath::Max(1, Source.NumChannels);
		Params.NumFramesPerCallback = 256;
		Params.AudioMixerNumOutputFrames = 256;
		Params.InstanceID = ++NextInstance;
		Params.bIsPreviewSound = false;
		Params.GraphName = Source.GetName();
		ISoundGeneratorPtr Generator = Source.CreateSoundGenerator(Params, MoveTemp(Defaults));
		if (!Generator.IsValid())
		{
			OutError = FString::Printf(TEXT("%s made no generator: its graph does not build (open it in the MetaSound editor for the error)."), *Source.GetName());
			return false;
		}
		Metasound::FMetasoundGenerator& MetaSound = static_cast<Metasound::FMetasoundGenerator&>(*Generator);
		const TSharedRef<bool> bGraphSet = MakeShared<bool>(false);
		const FDelegateHandle Handle = MetaSound.AddGraphSetCallback(Metasound::FOnSetGraph::FDelegate::CreateLambda([bGraphSet]() { *bGraphSet = true; }));
		Out = FBuffer();
		Out.SampleRate = SampleRate;
		Out.Channels = FMath::Max(1, Source.NumChannels);
		const int32 Frames = 256;
		TArray<float> Block;
		Block.SetNumZeroed(Frames * Out.Channels);
		const int64 Wanted = FMath::Max<int64>(1, FMath::RoundToInt64(MaxSeconds * SampleRate)) * Out.Channels;
		Out.Samples.Reserve(Wanted);
		// The generator builds its graph on a background task and plays silence until it is set: the render starts there.
		const double GiveUp = FPlatformTime::Seconds() + 10.0;
		bool bTimedOut = false;
		while (Out.Samples.Num() < Wanted)
		{
			FMemory::Memzero(Block.GetData(), Block.Num() * sizeof(float));
			Generator->OnGenerateAudio(Block.GetData(), Block.Num());
			if (!*bGraphSet)
			{
				if (FPlatformTime::Seconds() > GiveUp)
				{
					bTimedOut = true;
					break;
				}
				FPlatformProcess::Sleep(0.001f);
				continue;
			}
			Out.Samples.Append(Block);
			if (Generator->IsFinished())
			{
				Out.bFinished = true;
				break;
			}
		}
		MetaSound.RemoveGraphSetCallback(Handle);
		Source.OnEndGenerate(Generator);
		Generator.Reset();
		if (bTimedOut)
		{
			OutError = FString::Printf(TEXT("%s's graph did not build within 10 s."), *Source.GetName());
			return false;
		}
		if (Out.Samples.Num() > Wanted)
		{
			Out.Samples.SetNum(Wanted);
		}
		return true;
	}

	bool ReadWave(const USoundWave& Wave, FBuffer& Out, FString& OutError)
	{
		TArray<uint8> Pcm;
		uint32 Rate = 0;
		uint16 Channels = 0;
		if (!Wave.GetImportedSoundWaveData(Pcm, Rate, Channels) || Rate == 0 || Channels == 0)
		{
			OutError = FString::Printf(TEXT("%s has no imported samples to read (a procedural or cooked wave)."), *Wave.GetName());
			return false;
		}
		Out = FBuffer();
		Out.SampleRate = static_cast<int32>(Rate);
		Out.Channels = Channels;
		const int32 Count = Pcm.Num() / 2;
		Out.Samples.SetNumUninitialized(Count);
		const int16* Data = reinterpret_cast<const int16*>(Pcm.GetData());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Out.Samples[Index] = Data[Index] / 32768.f;
		}
		return true;
	}

	FHyperAIAudioMeasure Measure(const FBuffer& Buffer, const bool bEnvelope)
	{
		FHyperAIAudioMeasure M;
		const int32 Channels = FMath::Max(1, Buffer.Channels);
		const int32 Frames = Buffer.Samples.Num() / Channels;
		M.DurationSeconds = Frames / static_cast<float>(Buffer.SampleRate);
		M.bFinished = Buffer.bFinished;
		if (Frames == 0)
		{
			M.bSilent = true;
			return M;
		}
		TArray<float> Mono;
		Mono.SetNumZeroed(Frames);
		double Peak = 0, Sum = 0;
		int32 PeakFrame = 0;
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			float Loudest = 0.f;
			for (int32 Channel = 0; Channel < Channels; ++Channel)
			{
				const float Value = Buffer.Samples[Frame * Channels + Channel];
				Mono[Frame] += Value / Channels;
				Sum += Value * Value;
				Loudest = FMath::Max(Loudest, FMath::Abs(Value));
				if (FMath::Abs(Value) >= 0.999f) ++M.ClippedSamples;
			}
			if (Loudest > Peak)
			{
				Peak = Loudest;
				PeakFrame = Frame;
			}
		}
		M.PeakDb = ToDb(Peak);
		M.RmsDb = ToDb(FMath::Sqrt(Sum / Buffer.Samples.Num()));
		M.PeakMs = 1000.f * PeakFrame / Buffer.SampleRate;
		M.bSilent = M.PeakDb < -80.f;
		Loudness(Buffer, M.IntegratedLufs, M.MaxMomentaryLufs);
		if (M.bSilent)
		{
			return M;
		}
		// Onset: the first millisecond whose loudest sample is within 30 dB of the peak.
		const double OnsetLevel = Peak * FMath::Pow(10.0, -30.0 / 20.0);
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			bool bLoud = false;
			for (int32 Channel = 0; Channel < Channels && !bLoud; ++Channel) bLoud = FMath::Abs(Buffer.Samples[Frame * Channels + Channel]) >= OnsetLevel;
			if (bLoud)
			{
				M.OnsetMs = 1000.f * Frame / Buffer.SampleRate;
				break;
			}
		}
		// Tail: from the peak to the last 10 ms window whose RMS is within 60 dB of the peak.
		const int32 Window = FMath::Max(1, Buffer.SampleRate / 100);
		const double TailLevel = Peak * FMath::Pow(10.0, -60.0 / 20.0);
		int32 LastLoud = PeakFrame;
		for (int32 Start = 0; Start < Frames; Start += Window)
		{
			double Energy = 0;
			const int32 End = FMath::Min(Frames, Start + Window);
			for (int32 Frame = Start; Frame < End; ++Frame) Energy += Mono[Frame] * Mono[Frame];
			if (FMath::Sqrt(Energy / (End - Start)) >= TailLevel) LastLoud = End;
		}
		M.TailSeconds = FMath::Max(0, LastLoud - PeakFrame) / static_cast<float>(Buffer.SampleRate);
		M.SpectralCentroidHz = Centroid(Mono, Buffer.SampleRate);
		double Seam = 0;
		for (int32 Channel = 0; Channel < Channels; ++Channel)
		{
			Seam = FMath::Max(Seam, static_cast<double>(FMath::Abs(Buffer.Samples[(Frames - 1) * Channels + Channel] - Buffer.Samples[Channel])));
		}
		M.LoopSeamDb = ToDb(Seam);
		if (bEnvelope)
		{
			const int32 Step = FMath::Max(1, Buffer.SampleRate / 50);
			for (int32 Start = 0; Start < Frames && M.EnvelopeDb.Num() < 1500; Start += Step)
			{
				double Energy = 0;
				const int32 End = FMath::Min(Frames, Start + Step);
				for (int32 Frame = Start; Frame < End; ++Frame) Energy += Mono[Frame] * Mono[Frame];
				M.EnvelopeDb.Add(FMath::RoundToFloat(ToDb(FMath::Sqrt(Energy / (End - Start))) * 10.f) / 10.f);
			}
		}
		return M;
	}

	FHyperAIAudioAnalyzeReport Analyze(const FHyperAIAudioAnalyzeRequest& Request)
	{
		FHyperAIAudioAnalyzeReport Report;
		auto Refuse = [&Report](const TCHAR* Status, const FString& Why)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Why;
			return Report;
		};
		if (Request.Sound.TrimStartAndEnd().IsEmpty())
		{
			return Refuse(TEXT("invalid_request"), TEXT("Sound is a SoundWave or MetaSound Source, such as /Game/Audio/MS_Footsteps."));
		}
		FString Reason;
		UObject* Asset = CueGraph::LoadAsset(Request.Sound.TrimStartAndEnd(), *UObject::StaticClass(), &Reason);
		if (!Asset)
		{
			return Refuse(TEXT("not_found"), Reason.IsEmpty() ? FString::Printf(TEXT("%s is not in the project."), *Request.Sound) : Reason);
		}
		Report.Sound = Asset->GetPathName();
		const int32 Plays = FMath::Clamp(Request.Plays, 1, 16);
		const float MaxSeconds = FMath::Clamp(Request.MaxSeconds <= 0.f ? 5.f : Request.MaxSeconds, 0.1f, 30.f);
		const int32 Rate = FMath::Clamp(Request.SampleRate <= 0 ? 48000 : Request.SampleRate, 8000, 96000);
		bool bOneShot = true;
		bool bLoopingWave = false;
		if (UMetaSoundSource* Source = Cast<UMetaSoundSource>(Asset))
		{
			Report.Kind = TEXT("metasound_source");
			Report.Method = TEXT("metasound_offline");
			bOneShot = Source->IsOneShot();
			for (int32 Play = 0; Play < Plays; ++Play)
			{
				FBuffer Buffer;
				FString Error;
				if (!RenderMetaSound(*Source, Rate, MaxSeconds, Request.Parameters, Buffer, Error))
				{
					return Refuse(TEXT("render_failed"), Error);
				}
				Report.SampleRate = Buffer.SampleRate;
				Report.NumChannels = Buffer.Channels;
				Report.Plays.Add(Measure(Buffer, Request.bEnvelope));
			}
		}
		else if (const USoundWave* Wave = Cast<USoundWave>(Asset))
		{
			Report.Kind = TEXT("sound_wave");
			Report.Method = TEXT("sound_wave_pcm");
			bLoopingWave = Wave->bLooping;
			FBuffer Buffer;
			FString Error;
			if (!ReadWave(*Wave, Buffer, Error))
			{
				return Refuse(TEXT("render_failed"), Error);
			}
			Buffer.bFinished = true;
			Report.SampleRate = Buffer.SampleRate;
			Report.NumChannels = Buffer.Channels;
			Report.Plays.Add(Measure(Buffer, Request.bEnvelope));
		}
		else
		{
			return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("%s is a %s; analyze reads SoundWaves and MetaSound Sources (for a Sound Cue, analyze its waves)."),
				*Asset->GetName(), *Asset->GetClass()->GetName()));
		}

		float MinLufs = TNumericLimits<float>::Max(), MaxLufs = TNumericLimits<float>::Lowest();
		float MinCentroid = TNumericLimits<float>::Max(), MaxCentroid = TNumericLimits<float>::Lowest();
		int32 Silent = 0, Clipped = 0, Unfinished = 0;
		for (const FHyperAIAudioMeasure& M : Report.Plays)
		{
			if (M.bSilent) { ++Silent; continue; }
			MinLufs = FMath::Min(MinLufs, M.IntegratedLufs);
			MaxLufs = FMath::Max(MaxLufs, M.IntegratedLufs);
			MinCentroid = FMath::Min(MinCentroid, M.SpectralCentroidHz);
			MaxCentroid = FMath::Max(MaxCentroid, M.SpectralCentroidHz);
			Clipped += M.ClippedSamples > 0 ? 1 : 0;
			Unfinished += M.bFinished ? 0 : 1;
		}
		if (Silent < Report.Plays.Num())
		{
			Report.LoudnessSpreadLu = MaxLufs - MinLufs;
			Report.CentroidSpreadHz = MaxCentroid - MinCentroid;
		}
		const FHyperAIAudioMeasure& First = Report.Plays[0];
		if (Silent > 0)
		{
			Report.Issues.Add(FString::Printf(TEXT("silent: %d of %d play(s) rendered silence. Check a wave is set, On Play reaches the player and the player reaches the output.%s"), Silent, Report.Plays.Num(),
				Report.Kind == TEXT("metasound_source") && !FApp::CanEverRenderAudio()
					? TEXT(" This editor has no audio device (-nosound), so streamed SoundWaves load no audio past their header: only waves set to Force Inline are heard here.") : TEXT("")));
		}
		if (Clipped > 0)
		{
			Report.Issues.Add(FString::Printf(TEXT("clipping: %d play(s) reach full scale. Lower the gain (aim for peaks under -1 dBFS)."), Clipped));
		}
		if (Silent < Report.Plays.Num() && MaxLufs < -40.f)
		{
			Report.Issues.Add(TEXT("too_quiet: under -40 LUFS. Raise the gain; game effects usually sit around -24 to -12 LUFS before the mix."));
		}
		if (Silent < Report.Plays.Num() && MaxLufs > -6.f)
		{
			Report.Issues.Add(TEXT("too_loud: over -6 LUFS. Leave headroom for the mix."));
		}
		if (!First.bSilent && bOneShot && First.OnsetMs > 50.f)
		{
			Report.Issues.Add(FString::Printf(TEXT("late_onset: the sound starts %.0f ms in. Trim the leading silence so it lands on its cue."), First.OnsetMs));
		}
		if (Report.Kind == TEXT("metasound_source") && bOneShot && Unfinished > 0)
		{
			Report.Issues.Add(FString::Printf(TEXT("never_finishes: %d one-shot play(s) ran %.1f s without On Finished. Wire the last player's On Finished to the graph's On Finished."), Unfinished, MaxSeconds));
		}
		if (bLoopingWave && !First.bSilent && First.LoopSeamDb > -30.f)
		{
			Report.Issues.Add(FString::Printf(TEXT("clicks_at_seam: the loop jumps %.1f dB at its seam. Crossfade the ends (crossfade_loop recipe) or cut at zero crossings."), First.LoopSeamDb));
		}
		if (Report.Plays.Num() > 1 && Silent == 0 && Report.LoudnessSpreadLu < 0.1f && Report.CentroidSpreadHz < 1.f)
		{
			Report.Issues.Add(TEXT("no_variation: every play measured the same. Add takes, or pitch and volume jitter."));
		}
		Report.bOk = true;
		Report.Status = TEXT("ok");
		Report.Diagnostic = FString::Printf(TEXT("%s, %d play(s) at %d Hz, %d channel(s): %.1f LUFS, peak %.1f dBFS, onset %.0f ms, centroid %.0f Hz, tail %.2f s%s."),
			*Report.Sound, Report.Plays.Num(), Report.SampleRate, Report.NumChannels, First.IntegratedLufs, First.PeakDb, First.OnsetMs, First.SpectralCentroidHz,
			First.TailSeconds, Report.Issues.IsEmpty() ? TEXT(", no issues") : *FString::Printf(TEXT(", %d issue(s)"), Report.Issues.Num()));
		return Report;
	}
}
