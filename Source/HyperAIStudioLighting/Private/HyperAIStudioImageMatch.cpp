// Games by Hyper 2026.

#include "HyperAIStudioImageMatch.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioLightingGate.h"
#include "ImageCore.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace HyperAIStudio::Lighting::Match::Private
{
	namespace Gate = HyperAIStudio::Lighting::Gate;

	constexpr int32 WorkWidth = 256;
	constexpr int32 DisplayWidth = 480;
	constexpr int32 MaxIterations = 60;
	constexpr float Epsilon = 0.002f;
	/** Luminance floor for stops: low enough that a halved dark sea still reads as one stop darker. */
	constexpr float StopsFloor = 0.0005f;

	struct FLab
	{
		float L = 0.f;
		float A = 0.f;
		float B = 0.f;
	};

	float ToLinear(const uint8 Value)
	{
		static const TArray<float> Table = []
		{
			TArray<float> Values;
			for (int32 Index = 0; Index < 256; ++Index)
			{
				const float C = Index / 255.f;
				Values.Add(C <= 0.04045f ? C / 12.92f : FMath::Pow((C + 0.055f) / 1.055f, 2.4f));
			}
			return Values;
		}();
		return Table[Value];
	}

	/** CIE L*a*b* under D65, and linear luminance for exposure. */
	FLab ToLab(const FColor& Color, float& OutLuma)
	{
		const float R = ToLinear(Color.R);
		const float G = ToLinear(Color.G);
		const float B = ToLinear(Color.B);
		OutLuma = 0.2126f * R + 0.7152f * G + 0.0722f * B;
		const float X = (0.4124f * R + 0.3576f * G + 0.1805f * B) / 0.95047f;
		const float Z = (0.0193f * R + 0.1192f * G + 0.9505f * B) / 1.08883f;
		auto F = [](const float T) { return T > 0.008856f ? FMath::Pow(T, 1.f / 3.f) : 7.787f * T + 16.f / 116.f; };
		const float Fx = F(X);
		const float Fy = F(OutLuma);
		const float Fz = F(Z);
		return {116.f * Fy - 16.f, 500.f * (Fx - Fy), 200.f * (Fy - Fz)};
	}

	/** Crops Source to Aspect (width over height) about its centre, then resamples it to Width x Height. */
	TArray<FColor> Resample(const TArray<FColor>& Source, const int32 SourceWidth, const int32 SourceHeight, const float Aspect,
		const int32 Width, const int32 Height)
	{
		int32 CropWidth = SourceWidth;
		int32 CropHeight = SourceHeight;
		if (static_cast<float>(SourceWidth) / SourceHeight > Aspect)
		{
			CropWidth = FMath::Clamp(FMath::RoundToInt(SourceHeight * Aspect), 1, SourceWidth);
		}
		else
		{
			CropHeight = FMath::Clamp(FMath::RoundToInt(SourceWidth / Aspect), 1, SourceHeight);
		}
		const int32 X0 = (SourceWidth - CropWidth) / 2;
		const int32 Y0 = (SourceHeight - CropHeight) / 2;
		FImage Cropped;
		Cropped.Init(CropWidth, CropHeight, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const TArrayView64<FColor> Rows = Cropped.AsBGRA8();
		for (int32 Y = 0; Y < CropHeight; ++Y)
		{
			FMemory::Memcpy(&Rows[static_cast<int64>(Y) * CropWidth], &Source[(Y0 + Y) * SourceWidth + X0], CropWidth * sizeof(FColor));
		}
		FImage Resized;
		Resized.Init(Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FImageCore::ResizeImage(Cropped, Resized);
		const TArrayView64<const FColor> Out = Resized.AsBGRA8();
		TArray<FColor> Pixels(Out.GetData(), static_cast<int32>(Out.Num()));
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}
		return Pixels;
	}

	/**
	 * One image at the working size: its pixels, Lab, luminance and edge strength, and two channels an overall change
	 * of exposure leaves alone: brightness in stops, and chromaticity (each linear channel's share of their sum).
	 */
	struct FWork
	{
		int32 Width = 0;
		int32 Height = 0;
		TArray<FColor> Pixels;
		TArray<FLab> Lab;
		TArray<float> Luma;
		TArray<float> Edge;
		TArray<float> Stops;
		TArray<FVector3f> Share;
	};

	FWork Prepare(const TArray<FColor>& Source, const int32 SourceWidth, const int32 SourceHeight, const float Aspect, const int32 Width, const int32 Height)
	{
		FWork Work;
		Work.Width = Width;
		Work.Height = Height;
		Work.Pixels = Resample(Source, SourceWidth, SourceHeight, Aspect, Width, Height);
		const int32 Count = Width * Height;
		Work.Lab.SetNumUninitialized(Count);
		Work.Luma.SetNumUninitialized(Count);
		Work.Stops.SetNumUninitialized(Count);
		Work.Share.SetNumUninitialized(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FColor& Pixel = Work.Pixels[Index];
			Work.Lab[Index] = ToLab(Pixel, Work.Luma[Index]);
			Work.Stops[Index] = FMath::Log2(Work.Luma[Index] + StopsFloor);
			const FVector3f Linear(ToLinear(Pixel.R), ToLinear(Pixel.G), ToLinear(Pixel.B));
			const float Sum = Linear.X + Linear.Y + Linear.Z;
			// Black has no hue; it counts as grey, so a dark corner does not read as a colour.
			Work.Share[Index] = Sum > 0.003f ? Linear / Sum : FVector3f(1.f / 3.f);
		}
		Work.Edge.SetNumZeroed(Count);
		for (int32 Y = 1; Y < Height - 1; ++Y)
		{
			for (int32 X = 1; X < Width - 1; ++X)
			{
				// Edges in stops, so detail reads the same at any exposure.
				auto L = [&Work, X, Y, Width](const int32 DX, const int32 DY) { return Work.Stops[(Y + DY) * Width + X + DX]; };
				const float Gx = (L(1, -1) + 2.f * L(1, 0) + L(1, 1)) - (L(-1, -1) + 2.f * L(-1, 0) + L(-1, 1));
				const float Gy = (L(-1, 1) + 2.f * L(0, 1) + L(1, 1)) - (L(-1, -1) + 2.f * L(0, -1) + L(1, -1));
				Work.Edge[Y * Width + X] = FMath::Sqrt(Gx * Gx + Gy * Gy) / 8.f;
			}
		}
		return Work;
	}

	struct FStats
	{
		double L = 0.0;
		double A = 0.0;
		double B = 0.0;
		double Luma = 0.0;
		double Edge = 0.0;
		double Red = 0.0;
		double Green = 0.0;
		double Blue = 0.0;
		double Stops = 0.0;
		FVector3d Share = FVector3d::ZeroVector;
	};

	FStats Mean(const FWork& Work, const int32 X0, const int32 Y0, const int32 X1, const int32 Y1)
	{
		FStats Stats;
		int32 Count = 0;
		for (int32 Y = Y0; Y < Y1; ++Y)
		{
			for (int32 X = X0; X < X1; ++X)
			{
				const int32 Index = Y * Work.Width + X;
				Stats.L += Work.Lab[Index].L;
				Stats.A += Work.Lab[Index].A;
				Stats.B += Work.Lab[Index].B;
				Stats.Luma += Work.Luma[Index];
				Stats.Edge += Work.Edge[Index];
				Stats.Red += Work.Pixels[Index].R;
				Stats.Green += Work.Pixels[Index].G;
				Stats.Blue += Work.Pixels[Index].B;
				Stats.Stops += Work.Stops[Index];
				Stats.Share += FVector3d(Work.Share[Index]);
				++Count;
			}
		}
		if (Count > 0)
		{
			Stats.L /= Count;
			Stats.A /= Count;
			Stats.B /= Count;
			Stats.Luma /= Count;
			Stats.Edge /= Count;
			Stats.Red /= 255.0 * Count;
			Stats.Green /= 255.0 * Count;
			Stats.Blue /= 255.0 * Count;
			Stats.Stops /= Count;
			Stats.Share /= Count;
		}
		return Stats;
	}

	/**
	 * How far a cell sits from its image's whole, compared between the two images, in terms an overall change of
	 * exposure leaves alone: perceptual lightness with the candidate brought to the reference's exposure first, so deep
	 * shadows weigh as little as the eye gives them, and colour share.
	 */
	struct FResidual
	{
		/** Stops to add to the candidate's cell, beyond the overall difference. */
		double BrightenEV = 0.0;
		/** Reference minus candidate lightness (L*, 0-100) at matched exposure, beyond the overall difference. */
		double Lightness = 0.0;
		/** Candidate minus reference share of each channel, beyond the overall difference. */
		FVector3d Share = FVector3d::ZeroVector;

		/**
		 * One number, where 1 is a clear difference: lightness, and colour share weighted by ColourWeight. A cell's
		 * score weighs colour fully; layout barely, since the palette measure already counts colour.
		 */
		double Distance(const double ColourWeight = 12.0) const
		{
			return FMath::Sqrt(FMath::Square(Lightness / 10.0) + FMath::Square(Share.Size() * ColourWeight));
		}
		double Warmer() const { return Share.X - Share.Z; }
		double Greener() const { return Share.Y - 0.5 * (Share.X + Share.Z); }
	};

	/** CIE L* of a linear luminance, where 1 is white. */
	float Lightness(const float Y)
	{
		return Y > 0.008856f ? 116.f * FMath::Pow(Y, 1.f / 3.f) - 16.f : 903.3f * Y;
	}

	double MeanOf(const TArray<float>& Values, const int32 Width, const int32 X0, const int32 Y0, const int32 X1, const int32 Y1)
	{
		double Sum = 0.0;
		int32 Count = 0;
		for (int32 Y = Y0; Y < Y1; ++Y)
		{
			for (int32 X = X0; X < X1; ++X)
			{
				Sum += Values[Y * Width + X];
				++Count;
			}
		}
		return Count > 0 ? Sum / Count : 0.0;
	}

	/** The means a residual needs: the cell's and the whole image's, for both images, lightness at matched exposure. */
	struct FCell
	{
		FStats Ref;
		FStats Cand;
		double RefLight = 0.0;
		double CandLight = 0.0;
	};

	FResidual ResidualOf(const FCell& Cell, const FCell& All)
	{
		FResidual Residual;
		Residual.BrightenEV = (Cell.Ref.Stops - All.Ref.Stops) - (Cell.Cand.Stops - All.Cand.Stops);
		Residual.Lightness = (Cell.RefLight - All.RefLight) - (Cell.CandLight - All.CandLight);
		Residual.Share = (Cell.Cand.Share - All.Cand.Share) - (Cell.Ref.Share - All.Ref.Share);
		return Residual;
	}

	/** Distance of an average colour share from grey: how coloured it is, whatever its brightness. */
	double Colourfulness(const FVector3d& Share)
	{
		return (Share - FVector3d(1.0 / 3.0)).Size();
	}

	FLinearColor ColorOf(const FStats& Stats)
	{
		return FLinearColor(static_cast<float>(Stats.Red), static_cast<float>(Stats.Green), static_cast<float>(Stats.Blue));
	}

	/** Mean structural similarity of lightness over 8-pixel windows, -1 to 1. */
	float Ssim(const FWork& A, const FWork& B)
	{
		constexpr int32 Window = 8;
		constexpr int32 Step = 4;
		constexpr double C1 = 0.0001;
		constexpr double C2 = 0.0009;
		double Sum = 0.0;
		int32 Windows = 0;
		for (int32 Y = 0; Y + Window <= A.Height; Y += Step)
		{
			for (int32 X = 0; X + Window <= A.Width; X += Step)
			{
				double MeanA = 0.0;
				double MeanB = 0.0;
				for (int32 DY = 0; DY < Window; ++DY)
				{
					for (int32 DX = 0; DX < Window; ++DX)
					{
						const int32 Index = (Y + DY) * A.Width + X + DX;
						MeanA += A.Lab[Index].L / 100.0;
						MeanB += B.Lab[Index].L / 100.0;
					}
				}
				MeanA /= Window * Window;
				MeanB /= Window * Window;
				double VarA = 0.0;
				double VarB = 0.0;
				double Covariance = 0.0;
				for (int32 DY = 0; DY < Window; ++DY)
				{
					for (int32 DX = 0; DX < Window; ++DX)
					{
						const int32 Index = (Y + DY) * A.Width + X + DX;
						const double DA = A.Lab[Index].L / 100.0 - MeanA;
						const double DB = B.Lab[Index].L / 100.0 - MeanB;
						VarA += DA * DA;
						VarB += DB * DB;
						Covariance += DA * DB;
					}
				}
				VarA /= Window * Window - 1;
				VarB /= Window * Window - 1;
				Covariance /= Window * Window - 1;
				Sum += ((2.0 * MeanA * MeanB + C1) * (2.0 * Covariance + C2)) / ((MeanA * MeanA + MeanB * MeanB + C1) * (VarA + VarB + C2));
				++Windows;
			}
		}
		return Windows > 0 ? static_cast<float>(Sum / Windows) : 0.f;
	}

	/** Mean distance between two distributions of values, read at 64 matching quantiles. */
	float QuantileDistance(TArray<float> A, TArray<float> B)
	{
		if (A.IsEmpty() || B.IsEmpty())
		{
			return 0.f;
		}
		A.Sort();
		B.Sort();
		constexpr int32 Quantiles = 64;
		double Sum = 0.0;
		for (int32 Quantile = 0; Quantile < Quantiles; ++Quantile)
		{
			const double Fraction = (Quantile + 0.5) / Quantiles;
			Sum += FMath::Abs(A[FMath::FloorToInt(Fraction * (A.Num() - 1))] - B[FMath::FloorToInt(Fraction * (B.Num() - 1))]);
		}
		return static_cast<float>(Sum / Quantiles);
	}

	TArray<float> Channel(const FWork& Work, float FLab::* Member)
	{
		TArray<float> Values;
		Values.Reserve(Work.Lab.Num());
		for (const FLab& Lab : Work.Lab)
		{
			Values.Add(Lab.*Member);
		}
		return Values;
	}

	FString PlaceName(const int32 Row, const int32 Column, const int32 Grid)
	{
		const float V = (Row + 0.5f) / Grid;
		const float H = (Column + 0.5f) / Grid;
		const TCHAR* Vertical = V < 0.34f ? TEXT("top") : V > 0.66f ? TEXT("bottom") : TEXT("middle");
		const TCHAR* Horizontal = H < 0.34f ? TEXT("left") : H > 0.66f ? TEXT("right") : TEXT("center");
		FString Name = FString(Vertical) == TEXT("middle")
			? (FString(Horizontal) == TEXT("center") ? FString(TEXT("center")) : FString(Horizontal))
			: (FString(Horizontal) == TEXT("center") ? FString(Vertical) : FString::Printf(TEXT("%s %s"), Vertical, Horizontal));
		return FString::Printf(TEXT("%s (row %d, column %d)"), *Name, Row + 1, Column + 1);
	}

	/** Heat for a difference of 0 (black) to 1 (yellow), through violet and orange. */
	FColor Heat(float T)
	{
		T = FMath::Clamp(T, 0.f, 1.f);
		const FLinearColor Stops[] = {FLinearColor(0.f, 0.f, 0.f), FLinearColor(0.32f, 0.08f, 0.55f), FLinearColor(0.9f, 0.35f, 0.12f), FLinearColor(1.f, 0.94f, 0.47f)};
		const float Scaled = T * 3.f;
		const int32 Index = FMath::Min(2, FMath::FloorToInt(Scaled));
		const FLinearColor Color = FMath::Lerp(Stops[Index], Stops[Index + 1], Scaled - Index);
		return Color.ToFColor(/*bSRGB=*/false);
	}

	void Blend(FColor& Pixel, const FColor& Over, const float Alpha)
	{
		Pixel.R = static_cast<uint8>(FMath::Lerp(static_cast<float>(Pixel.R), static_cast<float>(Over.R), Alpha));
		Pixel.G = static_cast<uint8>(FMath::Lerp(static_cast<float>(Pixel.G), static_cast<float>(Over.G), Alpha));
		Pixel.B = static_cast<uint8>(FMath::Lerp(static_cast<float>(Pixel.B), static_cast<float>(Over.B), Alpha));
	}

	/** Grid lines over an image, and the given cells outlined. */
	void MarkGrid(TArray<FColor>& Pixels, const int32 Width, const int32 Height, const int32 Grid, const TArray<FIntPoint>& Outlined)
	{
		for (int32 Line = 1; Line < Grid; ++Line)
		{
			const int32 X = Line * Width / Grid;
			const int32 Y = Line * Height / Grid;
			for (int32 Row = 0; Row < Height; ++Row) Blend(Pixels[Row * Width + X], FColor::White, 0.35f);
			for (int32 Column = 0; Column < Width; ++Column) Blend(Pixels[Y * Width + Column], FColor::White, 0.35f);
		}
		const FColor Red(235, 64, 52);
		for (const FIntPoint& Cell : Outlined)
		{
			const int32 X0 = Cell.X * Width / Grid;
			const int32 X1 = (Cell.X + 1) * Width / Grid - 1;
			const int32 Y0 = Cell.Y * Height / Grid;
			const int32 Y1 = (Cell.Y + 1) * Height / Grid - 1;
			for (int32 Thick = 0; Thick < 2; ++Thick)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					Pixels[FMath::Clamp(Y0 + Thick, 0, Height - 1) * Width + X] = Red;
					Pixels[FMath::Clamp(Y1 - Thick, 0, Height - 1) * Width + X] = Red;
				}
				for (int32 Y = Y0; Y <= Y1; ++Y)
				{
					Pixels[Y * Width + FMath::Clamp(X0 + Thick, 0, Width - 1)] = Red;
					Pixels[Y * Width + FMath::Clamp(X1 - Thick, 0, Width - 1)] = Red;
				}
			}
		}
	}

	FString ProjectRelative(const FString& Path)
	{
		FString Relative = FPaths::ConvertRelativePathToFull(Path);
		FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		return Relative;
	}

	FString FullPath(const FString& Path)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::ProjectDir() / Path : Path);
	}

	bool IsSessionId(const FString& Id)
	{
		if (Id.IsEmpty() || Id.Len() > 48)
		{
			return false;
		}
		for (const TCHAR Character : Id)
		{
			if (!(FChar::IsAlnum(Character) && Character < 128) && Character != TEXT('_') && Character != TEXT('-'))
			{
				return false;
			}
		}
		return true;
	}

	TSharedPtr<FJsonObject> LoadJson(const FString& File)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		if (FFileHelper::LoadFileToString(Text, *File))
		{
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
		}
		return Object;
	}

	TArray<FHyperAIImageIteration> ReadIterations(const TSharedPtr<FJsonObject>& Session)
	{
		TArray<FHyperAIImageIteration> History;
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Session.IsValid() || !Session->TryGetArrayField(TEXT("iterations"), Entries))
		{
			return History;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			if (!Entry.IsValid())
			{
				continue;
			}
			FHyperAIImageIteration& Iteration = History.AddDefaulted_GetRef();
			Entry->TryGetNumberField(TEXT("index"), Iteration.Index);
			double Score = 0.0;
			Entry->TryGetNumberField(TEXT("score"), Score);
			Iteration.Score = static_cast<float>(Score);
			Entry->TryGetStringField(TEXT("note"), Iteration.Note);
			Entry->TryGetStringField(TEXT("candidate"), Iteration.CandidateImagePath);
			Entry->TryGetStringField(TEXT("side_by_side"), Iteration.SideBySidePath);
			Entry->TryGetStringField(TEXT("compared_utc"), Iteration.ComparedUtc);
		}
		return History;
	}

	bool WriteSession(const FString& File, const FString& SessionId, const FString& Reference, const FString& Mode, const float Target,
		const TArray<FHyperAIImageIteration>& History)
	{
		const TSharedRef<FJsonObject> Session = MakeShared<FJsonObject>();
		Session->SetStringField(TEXT("session_id"), SessionId);
		Session->SetStringField(TEXT("reference"), Reference);
		Session->SetStringField(TEXT("mode"), Mode);
		Session->SetNumberField(TEXT("target_score"), Target);
		TArray<TSharedPtr<FJsonValue>> Entries;
		for (const FHyperAIImageIteration& Iteration : History)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("index"), Iteration.Index);
			// Full precision: a rounded score read back would compare unevenly with the one just measured.
			Entry->SetNumberField(TEXT("score"), Iteration.Score);
			Entry->SetStringField(TEXT("note"), Iteration.Note);
			Entry->SetStringField(TEXT("candidate"), Iteration.CandidateImagePath);
			Entry->SetStringField(TEXT("side_by_side"), Iteration.SideBySidePath);
			Entry->SetStringField(TEXT("compared_utc"), Iteration.ComparedUtc);
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Session->SetArrayField(TEXT("iterations"), Entries);
		FString Json;
		FJsonSerializer::Serialize(Session, TJsonWriterFactory<>::Create(&Json));
		return FFileHelper::SaveStringToFile(Json, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

namespace HyperAIStudio::Lighting::Match
{
	using namespace Private;

	FString SessionsFolder()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio") / TEXT("Match"));
	}

	FHyperAIImageCompareReport ComparePixels(const TArray<FColor>& Reference, const int32 ReferenceWidth, const int32 ReferenceHeight,
		const TArray<FColor>& Candidate, const int32 CandidateWidth, const int32 CandidateHeight, const FString& Mode, const float TargetScore,
		const int32 Grid)
	{
		FHyperAIImageCompareReport Report;
		Report.Mode = Mode;
		Report.TargetScore = TargetScore;
		if (ReferenceWidth < 8 || ReferenceHeight < 8 || CandidateWidth < 8 || CandidateHeight < 8
			|| Reference.Num() != ReferenceWidth * ReferenceHeight || Candidate.Num() != CandidateWidth * CandidateHeight)
		{
			Report.Status = TEXT("image_too_small");
			Report.Diagnostic = TEXT("Both images need at least 8 x 8 pixels.");
			return Report;
		}
		const float Aspect = static_cast<float>(ReferenceWidth) / ReferenceHeight;
		const int32 Width = WorkWidth;
		const int32 Height = FMath::Clamp(FMath::RoundToInt(Width / Aspect), 16, 512);
		const FWork Ref = Prepare(Reference, ReferenceWidth, ReferenceHeight, Aspect, Width, Height);
		const FWork Cand = Prepare(Candidate, CandidateWidth, CandidateHeight, Aspect, Width, Height);
		const bool bAligned = Mode == TEXT("aligned");

		// Tone and the overall deltas come from the lighting measurements, so both tools agree on what a stop is.
		const FHyperAILightingImageMetrics RefMetrics = Gate::MeasurePixels(Ref.Pixels, Width, Height);
		const FHyperAILightingImageMetrics CandMetrics = Gate::MeasurePixels(Cand.Pixels, Width, Height);
		const float ToneDistance = Gate::HistogramDistance(RefMetrics.Histogram, CandMetrics.Histogram);
		Report.ToneScore = 100.f * FMath::Exp(-ToneDistance / 0.12f);
		Report.ExposureDeltaEV = FMath::Log2((RefMetrics.LuminanceP50 + Epsilon) / (CandMetrics.LuminanceP50 + Epsilon));
		Report.ColorTemperatureDeltaK = RefMetrics.ColorTemperatureK - CandMetrics.ColorTemperatureK;
		Report.SaturationDelta = RefMetrics.MeanSaturation - CandMetrics.MeanSaturation;
		Report.ContrastRatioDelta = RefMetrics.ContrastRatio - CandMetrics.ContrastRatio;

		const float PaletteDistance = QuantileDistance(Channel(Ref, &FLab::A), Channel(Cand, &FLab::A))
			+ QuantileDistance(Channel(Ref, &FLab::B), Channel(Cand, &FLab::B));
		Report.PaletteScore = 100.f * FMath::Exp(-PaletteDistance / 12.f);

		const FStats RefAll = Mean(Ref, 0, 0, Width, Height);
		const FStats CandAll = Mean(Cand, 0, 0, Width, Height);
		Report.ReferenceMeanColor = ColorOf(RefAll);
		Report.CandidateMeanColor = ColorOf(CandAll);

		// Lightness at matched exposure: the candidate's light scaled by the overall difference first.
		const float Scale = FMath::Pow(2.f, FMath::Clamp(Report.ExposureDeltaEV, -8.f, 8.f));
		TArray<float> RefLight;
		TArray<float> CandLight;
		RefLight.SetNumUninitialized(Width * Height);
		CandLight.SetNumUninitialized(Width * Height);
		for (int32 Index = 0; Index < Width * Height; ++Index)
		{
			RefLight[Index] = Lightness(Ref.Luma[Index]);
			CandLight[Index] = Lightness(Cand.Luma[Index] * Scale);
		}
		auto CellOf = [&](const int32 X0, const int32 Y0, const int32 X1, const int32 Y1)
		{
			FCell Cell;
			Cell.Ref = Mean(Ref, X0, Y0, X1, Y1);
			Cell.Cand = Mean(Cand, X0, Y0, X1, Y1);
			Cell.RefLight = MeanOf(RefLight, Width, X0, Y0, X1, Y1);
			Cell.CandLight = MeanOf(CandLight, Width, X0, Y0, X1, Y1);
			return Cell;
		};
		const FCell All = CellOf(0, 0, Width, Height);

		// Layout: where the light, dark and coloured masses sit, measured in stops and colour share with the whole
		// image's taken out, so an overall change of exposure or tint, which tone and palette count, costs nothing here.
		const int32 Columns = 8;
		const int32 Rows = FMath::Clamp(FMath::RoundToInt(8.f * Height / Width), 2, 16);
		double LayoutSum = 0.0;
		for (int32 Row = 0; Row < Rows; ++Row)
		{
			for (int32 Column = 0; Column < Columns; ++Column)
			{
				const int32 X0 = Column * Width / Columns;
				const int32 X1 = (Column + 1) * Width / Columns;
				const int32 Y0 = Row * Height / Rows;
				const int32 Y1 = (Row + 1) * Height / Rows;
				// Measured on live renders: at full colour weight, shadows of another hue swamped a matching composition.
				LayoutSum += ResidualOf(CellOf(X0, Y0, X1, Y1), All).Distance(/*ColourWeight=*/1.5);
			}
		}
		Report.LayoutScore = 100.f * FMath::Exp(-static_cast<float>(LayoutSum / (Rows * Columns)) / 0.8f);

		const double DetailRatio = (CandAll.Edge + 0.02) / (RefAll.Edge + 0.02);
		Report.DetailScore = 100.f * FMath::Exp(-FMath::Abs(static_cast<float>(FMath::Log2(DetailRatio))) / 1.2f);
		Report.StructureScore = 100.f * FMath::Clamp(Ssim(Ref, Cand), 0.f, 1.f);

		Report.Score = bAligned
			? 0.35f * Report.StructureScore + 0.20f * Report.PaletteScore + 0.20f * Report.ToneScore + 0.15f * Report.LayoutScore + 0.10f * Report.DetailScore
			: 0.30f * Report.PaletteScore + 0.30f * Report.ToneScore + 0.25f * Report.LayoutScore + 0.15f * Report.DetailScore;
		Report.bMatched = Report.Score >= TargetScore;

		// Regions: each cell measured beyond the overall differences, so the worst cells are local problems.
		for (int32 Row = 0; Row < Grid; ++Row)
		{
			for (int32 Column = 0; Column < Grid; ++Column)
			{
				const int32 X0 = Column * Width / Grid;
				const int32 X1 = (Column + 1) * Width / Grid;
				const int32 Y0 = Row * Height / Grid;
				const int32 Y1 = (Row + 1) * Height / Grid;
				const FCell Cell = CellOf(X0, Y0, X1, Y1);
				const FStats& R = Cell.Ref;
				const FStats& C = Cell.Cand;
				FHyperAIImageRegion& Region = Report.Regions.AddDefaulted_GetRef();
				Region.Name = PlaceName(Row, Column, Grid);
				Region.Row = Row;
				Region.Column = Column;
				const FResidual Residual = ResidualOf(Cell, All);
				Region.BrightenEV = static_cast<float>(Residual.BrightenEV);
				const double DL = (C.L - CandAll.L) - (R.L - RefAll.L);
				const double DA = (C.A - CandAll.A) - (R.A - RefAll.A);
				const double DB = (C.B - CandAll.B) - (R.B - RefAll.B);
				Region.ColorDeltaE = static_cast<float>(FMath::Sqrt(DL * DL + DA * DA + DB * DB));
				Region.DetailRatio = static_cast<float>((C.Edge + 0.02) / (R.Edge + 0.02));
				Region.ReferenceColor = ColorOf(R);
				Region.CandidateColor = ColorOf(C);
				Region.Score = 100.f * FMath::Exp(-static_cast<float>(Residual.Distance()) / 0.8f) * FMath::Exp(-FMath::Abs(FMath::Log2(Region.DetailRatio)) / 2.f);
				TArray<FString> Words;
				// Only a difference the eye would see: deep shadows can differ by stops and still both read as black.
				if (Region.BrightenEV > 0.3f && Residual.Lightness > 6.0) Words.Add(FString::Printf(TEXT("darker by %.1f EV"), Region.BrightenEV));
				if (Region.BrightenEV < -0.3f && Residual.Lightness < -6.0) Words.Add(FString::Printf(TEXT("brighter by %.1f EV"), -Region.BrightenEV));
				if (Residual.Warmer() > 0.04) Words.Add(TEXT("warmer"));
				if (Residual.Warmer() < -0.04) Words.Add(TEXT("cooler"));
				if (Residual.Greener() > 0.03) Words.Add(TEXT("greener"));
				if (Residual.Greener() < -0.03) Words.Add(TEXT("more magenta"));
				const double Colour = (Colourfulness(C.Share) - Colourfulness(CandAll.Share)) - (Colourfulness(R.Share) - Colourfulness(RefAll.Share));
				if (Colour < -0.04) Words.Add(TEXT("less saturated"));
				if (Colour > 0.04) Words.Add(TEXT("more saturated"));
				if (Region.DetailRatio < 0.7f) Words.Add(TEXT("flatter"));
				if (Region.DetailRatio > 1.4f) Words.Add(TEXT("busier"));
				Region.Difference = Words.IsEmpty() ? FString(TEXT("close")) : FString::Join(Words, TEXT(", "));
			}
		}
		Report.Regions.StableSort([](const FHyperAIImageRegion& A, const FHyperAIImageRegion& B) { return A.Score < B.Score; });

		// What to change, strongest first, in terms every domain has a lever for.
		TArray<TPair<float, FString>> Ranked;
		if (FMath::Abs(Report.ExposureDeltaEV) > 0.25f)
		{
			Ranked.Add({FMath::Abs(Report.ExposureDeltaEV), FString::Printf(
				TEXT("%s by %.2f EV overall: exposure or light intensity in a scene, base colour or emissive in a material, emissive or spawn count in an effect."),
				Report.ExposureDeltaEV > 0.f ? TEXT("Brighten") : TEXT("Darken"), FMath::Abs(Report.ExposureDeltaEV))});
		}
		// Warmth from the red-to-blue balance of the colour share: the kelvin estimate goes wrong for strongly
		// coloured images, whose average sits far from any light's colour.
		const double Warmth = (RefAll.Share.X - RefAll.Share.Z) - (CandAll.Share.X - CandAll.Share.Z);
		if (FMath::Abs(Warmth) > 0.03)
		{
			Ranked.Add({static_cast<float>(FMath::Abs(Warmth) * 10.0), FString::Printf(
				TEXT("%s: red against blue is %+.2f in the reference and %+.2f here (%s)."),
				Warmth > 0.0 ? TEXT("Warm it") : TEXT("Cool it"), RefAll.Share.X - RefAll.Share.Z, CandAll.Share.X - CandAll.Share.Z,
				Warmth > 0.0 ? TEXT("lower the light's temperature, raise post-process white_temp, or add a warmer tint")
					: TEXT("raise the light's temperature, lower post-process white_temp, or add a cooler tint"))});
		}
		if (FMath::Abs(Report.SaturationDelta) > 0.06f)
		{
			Ranked.Add({FMath::Abs(Report.SaturationDelta) * 6.f, FString::Printf(TEXT("%s saturation by about %.2f."),
				Report.SaturationDelta > 0.f ? TEXT("Raise") : TEXT("Lower"), FMath::Abs(Report.SaturationDelta))});
		}
		const float ContrastStops = FMath::Log2(FMath::Max(RefMetrics.ContrastRatio, 1.f) / FMath::Max(CandMetrics.ContrastRatio, 1.f));
		if (FMath::Abs(ContrastStops) > 0.5f)
		{
			Ranked.Add({FMath::Abs(ContrastStops) / 2.f, FString::Printf(TEXT("%s contrast: bright-to-dark ratio %.1f in the reference against %.1f here."),
				ContrastStops > 0.f ? TEXT("More") : TEXT("Less"), RefMetrics.ContrastRatio, CandMetrics.ContrastRatio)});
		}
		if (Report.LayoutScore < 70.f && !Report.Regions.IsEmpty())
		{
			Ranked.Add({(70.f - Report.LayoutScore) / 30.f, FString::Printf(
				TEXT("The light and dark masses sit in different places (layout %.0f): reframe the camera, move the key light, or move what fills %s."),
				Report.LayoutScore, *Report.Regions[0].Name)});
		}
		if (Report.DetailScore < 70.f)
		{
			Ranked.Add({(70.f - Report.DetailScore) / 30.f, FString::Printf(
				TEXT("The candidate is %s than the reference (detail %.1fx): %s."), DetailRatio < 1.0 ? TEXT("flatter") : TEXT("busier"), DetailRatio,
				DetailRatio < 1.0 ? TEXT("add surface detail, geometry or texture resolution") : TEXT("simplify: fewer small shapes, softer normals, or less noise"))});
		}
		if (bAligned && Report.StructureScore < 60.f)
		{
			Ranked.Add({(60.f - Report.StructureScore) / 30.f, FString::Printf(
				TEXT("The shapes do not line up (structure %.0f): check the camera matches the reference's, or compare in look mode."), Report.StructureScore)});
		}
		if (!Report.Regions.IsEmpty() && Report.Regions[0].Score < 85.f)
		{
			const FHyperAIImageRegion& Worst = Report.Regions[0];
			Ranked.Add({(85.f - Worst.Score) / 40.f, FString::Printf(TEXT("Start with the %s: %s."), *Worst.Name, *Worst.Difference)});
		}
		Ranked.StableSort([](const TPair<float, FString>& A, const TPair<float, FString>& B) { return A.Key > B.Key; });
		for (const TPair<float, FString>& Entry : Ranked)
		{
			Report.Suggestions.Add(Entry.Value);
		}
		if (Report.bMatched)
		{
			Report.Suggestions.Insert(FString::Printf(TEXT("Matched: %.0f reaches the target of %.0f. Keep this iteration."), Report.Score, TargetScore), 0);
		}
		else if (Report.Suggestions.IsEmpty())
		{
			Report.Suggestions.Add(TEXT("The measures are close but the score is short of the target: open the side-by-side and look for what the numbers miss."));
		}
		Report.bOk = true;
		Report.Status = Report.bMatched ? TEXT("matched") : TEXT("differs");
		return Report;
	}

	/** The side-by-side with the grid and worst cells, and the heat map of per-pixel colour difference. */
	void WriteImages(const TArray<FColor>& Reference, const int32 ReferenceWidth, const int32 ReferenceHeight,
		const TArray<FColor>& Candidate, const int32 CandidateWidth, const int32 CandidateHeight, const int32 Grid,
		const FString& SideFile, const FString& DiffFile, FHyperAIImageCompareReport& Report)
	{
		const float Aspect = static_cast<float>(ReferenceWidth) / ReferenceHeight;
		const int32 Width = DisplayWidth;
		const int32 Height = FMath::Clamp(FMath::RoundToInt(Width / Aspect), 32, 960);
		TArray<FColor> Left = Resample(Reference, ReferenceWidth, ReferenceHeight, Aspect, Width, Height);
		TArray<FColor> Right = Resample(Candidate, CandidateWidth, CandidateHeight, Aspect, Width, Height);
		TArray<FIntPoint> Outlined;
		for (const FHyperAIImageRegion& Region : Report.Regions)
		{
			if (Outlined.Num() < 3 && Region.Score < 90.f)
			{
				Outlined.Add(FIntPoint(Region.Column, Region.Row));
			}
		}
		MarkGrid(Left, Width, Height, Grid, Outlined);
		MarkGrid(Right, Width, Height, Grid, Outlined);
		constexpr int32 Margin = 8;
		constexpr int32 Gap = 16;
		const int32 CanvasWidth = Margin * 2 + Width * 2 + Gap;
		const int32 CanvasHeight = Margin * 2 + Height;
		TArray<FColor> Canvas;
		Canvas.Init(FColor(24, 24, 24), CanvasWidth * CanvasHeight);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			FMemory::Memcpy(&Canvas[(Margin + Y) * CanvasWidth + Margin], &Left[Y * Width], Width * sizeof(FColor));
			FMemory::Memcpy(&Canvas[(Margin + Y) * CanvasWidth + Margin + Width + Gap], &Right[Y * Width], Width * sizeof(FColor));
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SideFile), /*Tree=*/true);
		if (Gate::WritePng(SideFile, Canvas, CanvasWidth, CanvasHeight))
		{
			Report.SideBySidePath = ProjectRelative(SideFile);
		}

		// Per-pixel difference at the working size, drawn at the display size.
		const int32 WorkHeight = FMath::Clamp(FMath::RoundToInt(WorkWidth / Aspect), 16, 512);
		const FWork Ref = Prepare(Reference, ReferenceWidth, ReferenceHeight, Aspect, WorkWidth, WorkHeight);
		const FWork Cand = Prepare(Candidate, CandidateWidth, CandidateHeight, Aspect, WorkWidth, WorkHeight);
		TArray<FColor> Diff;
		Diff.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const int32 Index = FMath::Min(Y * WorkHeight / Height, WorkHeight - 1) * WorkWidth + FMath::Min(X * WorkWidth / Width, WorkWidth - 1);
				const FLab& A = Ref.Lab[Index];
				const FLab& B = Cand.Lab[Index];
				const float DeltaE = FMath::Sqrt(FMath::Square(A.L - B.L) + FMath::Square(A.A - B.A) + FMath::Square(A.B - B.B));
				Diff[Y * Width + X] = Heat(DeltaE / 30.f);
			}
		}
		MarkGrid(Diff, Width, Height, Grid, Outlined);
		if (Gate::WritePng(DiffFile, Diff, Width, Height))
		{
			Report.DiffPath = ProjectRelative(DiffFile);
		}
	}

	/**
	 * The grade that would close the colour gap, as a colourist would give it: a LUT fitted from the candidate to the reference
	 * and composed after the level's own LUT, so it replaces that one; the candidate's score graded through it; and the graded
	 * candidate beside the reference.
	 */
	void FitGrade(const FHyperAIImageCompareRequest& Request, const TArray<FColor>& Reference, const int32 ReferenceWidth, const int32 ReferenceHeight,
		const TArray<FColor>& Candidate, const int32 CandidateWidth, const int32 CandidateHeight, const FString& Folder, const FString& Stem,
		FHyperAIImageCompareReport& Report)
	{
		// At the working size with both cropped to the reference's shape, so the pixels pair up as the scores pair them.
		const float Aspect = static_cast<float>(ReferenceWidth) / ReferenceHeight;
		const int32 Height = FMath::Clamp(FMath::RoundToInt(WorkWidth / Aspect), 16, 512);
		const TArray<FColor> Ref = Resample(Reference, ReferenceWidth, ReferenceHeight, Aspect, WorkWidth, Height);
		const TArray<FColor> Cand = Resample(Candidate, CandidateWidth, CandidateHeight, Aspect, WorkWidth, Height);
		// Pixel by pixel suits a view that differs from the reference in its grade alone; the look suits one whose content
		// differs too, such as a blockout under finished assets, where pairing pixels would recolour shapes. Both are fitted,
		// and the one that grades the candidate closer is kept.
		Lut::FTable Fitted;
		TArray<FColor> Graded;
		FHyperAIImageCompareReport GradedReport;
		bool bPairs = false;
		for (const bool bByPixel : {true, false})
		{
			Lut::FTable Table = Lut::Fit(Cand, Ref, bByPixel);
			TArray<FColor> Pixels = Lut::Apply(Table, Candidate);
			FHyperAIImageCompareReport Scored = ComparePixels(Reference, ReferenceWidth, ReferenceHeight, Pixels, CandidateWidth, CandidateHeight,
				Request.Mode, Request.TargetScore, Request.Grid);
			if (Fitted.IsEmpty() || Scored.Score > GradedReport.Score)
			{
				Fitted = MoveTemp(Table);
				Graded = MoveTemp(Pixels);
				GradedReport = MoveTemp(Scored);
				bPairs = bByPixel;
			}
		}
		Lut::FTable Current = Lut::Identity();
		FString CurrentPath;
		if (UWorld* World = Gate::GetEditorWorld(); World && Gate::ReadLevelLut(*World, Current, CurrentPath))
		{
			Report.LutComposedWith = CurrentPath;
		}
		const FString Name = Request.SessionId.IsEmpty() ? FString::Printf(TEXT("lut_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")))
			: Request.SessionId + TEXT("_") + Stem;
		const FString LutFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio") / TEXT("LUTs") / (Name + TEXT(".png")));
		if (!Gate::WritePng(LutFile, Lut::ToStrip(Lut::Compose(Fitted, Current)), Lut::StripWidth, Lut::StripHeight))
		{
			Report.Diagnostic += TEXT(" The LUT could not be written.");
			return;
		}
		Report.LutPath = ProjectRelative(LutFile);
		Report.LutScore = GradedReport.Score;
		WriteImages(Reference, ReferenceWidth, ReferenceHeight, Graded, CandidateWidth, CandidateHeight, Request.Grid,
			Folder / (Stem + TEXT("_lut_side.png")), Folder / (Stem + TEXT("_lut_diff.png")), GradedReport);
		Report.LutSideBySidePath = GradedReport.SideBySidePath;
		Report.Diagnostic += FString::Printf(TEXT(" LUT fitted %s: graded, the candidate scores %.0f; it is %s%s."), bPairs ? TEXT("pixel by pixel") : TEXT("to the look"),
			Report.LutScore, *Report.LutPath, CurrentPath.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", and replaces %s"), *CurrentPath));
		if (Report.LutScore > Report.Score + 1.f)
		{
			Report.Suggestions.Insert(FString::Printf(TEXT("Grade it: through a LUT fitted to the reference the candidate scores %.0f, not %.0f. Once the light is close, apply it with ")
				TEXT("set_post_process color_grading_lut %s, and fit again after a big change of light."), Report.LutScore, Report.Score, *Report.LutPath), 0);
		}
	}

	FHyperAIImageCompareReport Compare(const FHyperAIImageCompareRequest& Request)
	{
		FHyperAIImageCompareReport Report;
		Report.Capabilities = FHyperAIStudioLightingContracts::GetCapabilities();
		Report.Mode = Request.Mode;
		Report.TargetScore = Request.TargetScore;
		Report.SessionId = Request.SessionId;
		auto Reject = [&Report](const TCHAR* Status, const FString& Diagnostic)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Diagnostic;
			return Report;
		};
		if (!IsInGameThread())
		{
			return Reject(TEXT("game_thread_required"), TEXT("Image comparison runs on the game thread."));
		}
		if (Request.ReferenceImagePath.IsEmpty() || Request.ReferenceImagePath.Len() > 1024 || Request.CandidateImagePath.Len() > 1024
			|| (Request.Mode != TEXT("look") && Request.Mode != TEXT("aligned"))
			|| !(Request.TargetScore > 0.f && Request.TargetScore <= 100.f) || Request.Grid < 2 || Request.Grid > 8
			|| (!Request.SessionId.IsEmpty() && !IsSessionId(Request.SessionId)) || Request.Note.Len() > 400)
		{
			return Reject(TEXT("invalid_request"), TEXT("Pass reference_image_path; mode look or aligned; target_score above 0 and at most 100; ")
				TEXT("grid 2 to 8; a session_id of letters, digits, - and _ up to 48; and a note up to 400 characters."));
		}
		TArray<FColor> Reference;
		int32 ReferenceWidth = 0;
		int32 ReferenceHeight = 0;
		FString Error;
		if (!Gate::LoadProjectImage(Request.ReferenceImagePath, Reference, ReferenceWidth, ReferenceHeight, Error))
		{
			return Reject(TEXT("reference_unreadable"), Error);
		}
		TArray<FColor> Candidate;
		int32 CandidateWidth = 0;
		int32 CandidateHeight = 0;
		bool bCaptured = false;
		if (!Request.CandidateImagePath.IsEmpty())
		{
			if (!Gate::LoadProjectImage(Request.CandidateImagePath, Candidate, CandidateWidth, CandidateHeight, Error))
			{
				return Reject(TEXT("candidate_unreadable"), Error);
			}
		}
		else
		{
			if (!Gate::GetEditorWorld())
			{
				return Reject(TEXT("no_editor_world"), TEXT("No level is open to capture; pass candidate_image_path."));
			}
			if (!Gate::CaptureActiveViewport(Candidate, CandidateWidth, CandidateHeight, Error))
			{
				return Reject(TEXT("viewport_unavailable"), Error);
			}
			bCaptured = true;
		}

		// The session, read before measuring, so a changed reference is refused rather than mixed into one history.
		const FString Folder = SessionsFolder() / (Request.SessionId.IsEmpty() ? FString(TEXT("_single")) : Request.SessionId);
		const FString SessionFile = Folder / TEXT("session.json");
		const FString ReferenceRelative = ProjectRelative(FullPath(Request.ReferenceImagePath));
		TArray<FHyperAIImageIteration> History;
		if (!Request.SessionId.IsEmpty())
		{
			const TSharedPtr<FJsonObject> Session = LoadJson(SessionFile);
			FString StoredReference;
			if (Session.IsValid() && Session->TryGetStringField(TEXT("reference"), StoredReference) && StoredReference != ReferenceRelative)
			{
				return Reject(TEXT("session_reference_changed"), FString::Printf(
					TEXT("Session %s matches against %s; start a new session_id for a new reference."), *Request.SessionId, *StoredReference));
			}
			History = ReadIterations(Session);
			if (History.Num() >= MaxIterations)
			{
				return Reject(TEXT("session_full"), FString::Printf(TEXT("Session %s has %d iterations, the most one keeps; start a new one."),
					*Request.SessionId, MaxIterations));
			}
		}

		FHyperAIImageCompareReport Measured = ComparePixels(Reference, ReferenceWidth, ReferenceHeight, Candidate, CandidateWidth, CandidateHeight,
			Request.Mode, Request.TargetScore, Request.Grid);
		if (!Measured.bOk)
		{
			return Reject(*Measured.Status, Measured.Diagnostic);
		}
		Measured.Capabilities = Report.Capabilities;
		Measured.SessionId = Request.SessionId;
		Report = MoveTemp(Measured);

		const int32 Index = History.Num();
		const FString Stem = Request.SessionId.IsEmpty() ? FString(TEXT("latest")) : FString::Printf(TEXT("iter_%02d"), Index);
		IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
		FString CandidateKept;
		if (bCaptured)
		{
			const FString File = Folder / (Stem + TEXT("_candidate.png"));
			if (Gate::WritePng(File, Candidate, CandidateWidth, CandidateHeight))
			{
				Report.CandidateWrittenPath = ProjectRelative(File);
				CandidateKept = Report.CandidateWrittenPath;
			}
		}
		else if (!Request.SessionId.IsEmpty())
		{
			const FString Source = FullPath(Request.CandidateImagePath);
			const FString File = Folder / (Stem + TEXT("_candidate.") + FPaths::GetExtension(Source).ToLower());
			CandidateKept = IFileManager::Get().Copy(*File, *Source) == COPY_OK ? ProjectRelative(File) : ProjectRelative(Source);
		}
		else
		{
			CandidateKept = ProjectRelative(FullPath(Request.CandidateImagePath));
		}
		WriteImages(Reference, ReferenceWidth, ReferenceHeight, Candidate, CandidateWidth, CandidateHeight, Request.Grid,
			Folder / (Stem + TEXT("_side.png")), Folder / (Stem + TEXT("_diff.png")), Report);

		const float CandidateAspect = static_cast<float>(CandidateWidth) / CandidateHeight;
		const float ReferenceAspect = static_cast<float>(ReferenceWidth) / ReferenceHeight;
		const FString Cropped = FMath::Abs(CandidateAspect - ReferenceAspect) > 0.02f * ReferenceAspect
			? FString::Printf(TEXT(" The candidate (%dx%d) was cropped to the reference's shape (%dx%d)."), CandidateWidth, CandidateHeight, ReferenceWidth, ReferenceHeight)
			: FString();
		Report.Diagnostic = FString::Printf(TEXT("Score %.0f of 100 (target %.0f, %s mode): palette %.0f, tone %.0f, layout %.0f, detail %.0f%s.%s"),
			Report.Score, Request.TargetScore, *Request.Mode, Report.PaletteScore, Report.ToneScore, Report.LayoutScore, Report.DetailScore,
			Request.Mode == TEXT("aligned") ? *FString::Printf(TEXT(", structure %.0f"), Report.StructureScore) : TEXT(""), *Cropped);
		if (Request.bFitLut)
		{
			FitGrade(Request, Reference, ReferenceWidth, ReferenceHeight, Candidate, CandidateWidth, CandidateHeight, Folder, Stem, Report);
		}

		if (!Request.SessionId.IsEmpty())
		{
			FHyperAIImageIteration& Iteration = History.AddDefaulted_GetRef();
			Iteration.Index = Index;
			Iteration.Score = Report.Score;
			Iteration.Note = Request.Note;
			Iteration.CandidateImagePath = CandidateKept;
			Iteration.SideBySidePath = Report.SideBySidePath;
			Iteration.ComparedUtc = FDateTime::UtcNow().ToIso8601();
			for (const FHyperAIImageIteration& Entry : History)
			{
				if (Report.BestIteration < 0 || Entry.Score > Report.BestScore)
				{
					Report.BestIteration = Entry.Index;
					Report.BestScore = Entry.Score;
				}
			}
			Report.Improvement = History.Num() >= 2 ? Report.Score - History[History.Num() - 2].Score : 0.f;
			if (History.Num() >= 4)
			{
				float BestBefore = 0.f;
				float BestRecent = 0.f;
				for (int32 Entry = 0; Entry < History.Num(); ++Entry)
				{
					float& Slot = Entry < History.Num() - 3 ? BestBefore : BestRecent;
					Slot = FMath::Max(Slot, History[Entry].Score);
				}
				Report.bPlateau = !Report.bMatched && BestRecent - BestBefore < 1.f;
			}
			Report.History = History;
			if (!WriteSession(SessionFile, Request.SessionId, ReferenceRelative, Request.Mode, Request.TargetScore, History))
			{
				Report.Diagnostic += TEXT(" The session file could not be written, so this iteration is not remembered.");
			}
			Report.Diagnostic += FString::Printf(TEXT(" Iteration %d of session %s; best %.0f at iteration %d; %+.1f since the last."),
				Index, *Request.SessionId, Report.BestScore, Report.BestIteration, Report.Improvement);
			if (Report.bPlateau)
			{
				Report.Diagnostic += TEXT(" It has stopped improving: change approach, or stop and keep the best.");
				Report.Suggestions.Insert(TEXT("Three iterations without a point gained: try a different lever, or stop and keep the best iteration."), 0);
			}
		}
		if (Report.bMatched)
		{
			Report.Diagnostic += TEXT(" Matched.");
		}
		return Report;
	}

	FString FindSessionFor(const FString& ReferenceImagePath)
	{
		const FString Wanted = ProjectRelative(FullPath(ReferenceImagePath));
		TArray<FString> Folders;
		IFileManager::Get().FindFiles(Folders, *(SessionsFolder() / TEXT("*")), /*Files=*/false, /*Directories=*/true);
		FString Newest;
		FDateTime NewestStamp = FDateTime::MinValue();
		for (const FString& Folder : Folders)
		{
			const FString File = SessionsFolder() / Folder / TEXT("session.json");
			const TSharedPtr<FJsonObject> Session = Folder.StartsWith(TEXT("_")) ? nullptr : LoadJson(File);
			FString Reference;
			if (Session.IsValid() && Session->TryGetStringField(TEXT("reference"), Reference) && Reference == Wanted)
			{
				const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*File);
				if (Stamp > NewestStamp)
				{
					NewestStamp = Stamp;
					Newest = Folder;
				}
			}
		}
		return Newest;
	}

	bool ReadSession(const FString& SessionId, FString& OutReference, FString& OutMode, TArray<FHyperAIImageIteration>& OutHistory, float& OutTargetScore)
	{
		const TSharedPtr<FJsonObject> Session = IsSessionId(SessionId) ? LoadJson(SessionsFolder() / SessionId / TEXT("session.json")) : nullptr;
		if (!Session.IsValid())
		{
			return false;
		}
		Session->TryGetStringField(TEXT("reference"), OutReference);
		OutMode = TEXT("look");
		Session->TryGetStringField(TEXT("mode"), OutMode);
		double Target = 80.0;
		Session->TryGetNumberField(TEXT("target_score"), Target);
		OutTargetScore = static_cast<float>(Target);
		OutHistory = ReadIterations(Session);
		return true;
	}
}
