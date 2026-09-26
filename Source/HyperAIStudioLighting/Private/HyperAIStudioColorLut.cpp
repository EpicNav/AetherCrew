// Games by Hyper 2026.

#include "HyperAIStudioColorLut.h"

namespace HyperAIStudio::Lighting::Lut
{
	namespace
	{
		constexpr int32 Nodes = Size * Size * Size;
		/** How many pixels' worth of pull the overall transform has on a node: enough to fill a gap, not to overrule data. */
		constexpr float PriorWeight = 8.f;

		int32 IndexOf(const int32 R, const int32 G, const int32 B)
		{
			return (B * Size + G) * Size + R;
		}

		FVector3f ToUnit(const FColor& Colour)
		{
			return FVector3f(Colour.R, Colour.G, Colour.B) / 255.f;
		}

		FColor ToColor(const FVector3f& Value)
		{
			auto Byte = [](const float Channel) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Channel * 255.f), 0, 255)); };
			return FColor(Byte(Value.X), Byte(Value.Y), Byte(Value.Z), 255);
		}

		FVector3f Saturate(const FVector3f& Value)
		{
			return FVector3f(FMath::Clamp(Value.X, 0.f, 1.f), FMath::Clamp(Value.Y, 0.f, 1.f), FMath::Clamp(Value.Z, 0.f, 1.f));
		}

		float ToLinear(const float Encoded)
		{
			return Encoded <= 0.04045f ? Encoded / 12.92f : FMath::Pow((Encoded + 0.055f) / 1.055f, 2.4f);
		}

		float ToEncoded(const float Linear)
		{
			return Linear <= 0.0031308f ? Linear * 12.92f : 1.055f * FMath::Pow(FMath::Max(Linear, 0.f), 1.f / 2.4f) - 0.055f;
		}

		/** sRGB to CIE Lab under D65, where spreads of colour compare as the eye sees them. */
		FVector3f ToLab(const FVector3f& Srgb)
		{
			const float R = ToLinear(Srgb.X), G = ToLinear(Srgb.Y), B = ToLinear(Srgb.Z);
			const float X = (0.4124f * R + 0.3576f * G + 0.1805f * B) / 0.95047f;
			const float Y = 0.2126f * R + 0.7152f * G + 0.0722f * B;
			const float Z = (0.0193f * R + 0.1192f * G + 0.9505f * B) / 1.08883f;
			auto F = [](const float T) { return T > 0.008856f ? FMath::Pow(T, 1.f / 3.f) : 7.787f * T + 16.f / 116.f; };
			const float Fx = F(X), Fy = F(Y), Fz = F(Z);
			return FVector3f(116.f * Fy - 16.f, 500.f * (Fx - Fy), 200.f * (Fy - Fz));
		}

		FVector3f FromLab(const FVector3f& Lab)
		{
			const float Fy = (Lab.X + 16.f) / 116.f;
			const float Fx = Fy + Lab.Y / 500.f;
			const float Fz = Fy - Lab.Z / 200.f;
			auto Inverse = [](const float T) { return T > 0.206893f ? T * T * T : (T - 16.f / 116.f) / 7.787f; };
			const float X = Inverse(Fx) * 0.95047f, Y = Inverse(Fy), Z = Inverse(Fz) * 1.08883f;
			return Saturate(FVector3f(ToEncoded(3.2406f * X - 1.5372f * Y - 0.4986f * Z), ToEncoded(-0.9689f * X + 1.8758f * Y + 0.0415f * Z),
				ToEncoded(0.0557f * X - 0.2040f * Y + 1.0570f * Z)));
		}

		/**
		 * The affine colour transform, Output = A (Input, 1), closest to the pairs in least squares, pulled a little towards
		 * no change: a blockout's few flat colours pin down too little on their own.
		 */
		void FitAffine(TConstArrayView<FColor> Candidate, TConstArrayView<FColor> Reference, const int32 Count, double OutA[3][4])
		{
			double Normal[4][4] = {};
			double Right[4][3] = {};
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FVector3f In = ToUnit(Candidate[Index]);
				const FVector3f Out = ToUnit(Reference[Index]);
				const double X[4] = {In.X, In.Y, In.Z, 1.0};
				const double Y[3] = {Out.X, Out.Y, Out.Z};
				for (int32 Row = 0; Row < 4; ++Row)
				{
					for (int32 Column = 0; Column < 4; ++Column) Normal[Row][Column] += X[Row] * X[Column];
					for (int32 Channel = 0; Channel < 3; ++Channel) Right[Row][Channel] += X[Row] * Y[Channel];
				}
			}
			const double Pull = 0.01 * FMath::Max(Count, 1);
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				// (Normal + Pull I) a = Right + Pull (the identity's row), solved by elimination with partial pivoting.
				double M[4][5];
				for (int32 Row = 0; Row < 4; ++Row)
				{
					for (int32 Column = 0; Column < 4; ++Column) M[Row][Column] = Normal[Row][Column] + (Row == Column ? Pull : 0.0);
					M[Row][4] = Right[Row][Channel] + (Row == Channel ? Pull : 0.0);
				}
				for (int32 Pivot = 0; Pivot < 4; ++Pivot)
				{
					int32 Best = Pivot;
					for (int32 Row = Pivot + 1; Row < 4; ++Row) Best = FMath::Abs(M[Row][Pivot]) > FMath::Abs(M[Best][Pivot]) ? Row : Best;
					for (int32 Column = 0; Column < 5; ++Column) Swap(M[Pivot][Column], M[Best][Column]);
					const double Divisor = FMath::Abs(M[Pivot][Pivot]) > 1e-12 ? M[Pivot][Pivot] : 1e-12;
					for (int32 Row = 0; Row < 4; ++Row)
					{
						if (Row == Pivot) continue;
						const double Factor = M[Row][Pivot] / Divisor;
						for (int32 Column = Pivot; Column < 5; ++Column) M[Row][Column] -= Factor * M[Pivot][Column];
					}
				}
				for (int32 Row = 0; Row < 4; ++Row)
				{
					OutA[Channel][Row] = M[Row][4] / (FMath::Abs(M[Row][Row]) > 1e-12 ? M[Row][Row] : 1e-12);
				}
			}
		}

		/** A value at Position along a curve through ascending Points, and straight on past its ends with the end's slope, or 1. */
		float AlongCurve(const TArray<FVector2f>& Points, const float Position)
		{
			if (Points.Num() == 1 || Position <= Points[0].X)
			{
				return Points[0].Y + (Position - Points[0].X);
			}
			for (int32 Index = 1; Index < Points.Num(); ++Index)
			{
				if (Position <= Points[Index].X)
				{
					const FVector2f& A = Points[Index - 1];
					const FVector2f& B = Points[Index];
					return FMath::Lerp(A.Y, B.Y, (Position - A.X) / FMath::Max(B.X - A.X, 1e-4f));
				}
			}
			return Points.Last().Y + (Position - Points.Last().X);
		}

		/**
		 * The look of one image graded onto another, as a colourist matches a frame by eye: lightness through its whole
		 * distribution, as a tone curve, and colour as its mean and spread at each level of lightness, so a warm sky over cool
		 * shadows carries over as it is.
		 */
		FTable FitLook(TConstArrayView<FColor> Candidate, TConstArrayView<FColor> Reference)
		{
			constexpr int32 Levels = 64;
			constexpr int32 Bands = 5;
			// The tone curve's steepest and flattest: a gap in the attempt's lightness never becomes a jump.
			constexpr float MinSlope = 0.25f;
			constexpr float MaxSlope = 4.f;
			TArray<FVector3f> From, To;
			for (const FColor& Pixel : Candidate) From.Add(ToLab(ToUnit(Pixel)));
			for (const FColor& Pixel : Reference) To.Add(ToLab(ToUnit(Pixel)));
			From.Sort([](const FVector3f& A, const FVector3f& B) { return A.X < B.X; });
			To.Sort([](const FVector3f& A, const FVector3f& B) { return A.X < B.X; });
			auto At = [](const TArray<FVector3f>& Sorted, const float Fraction) { return Sorted[FMath::Clamp(FMath::RoundToInt(Fraction * (Sorted.Num() - 1)), 0, Sorted.Num() - 1)]; };

			// Lightness: the attempt's quantiles onto the reference's; a run of equal lightness, such as a flat blockout colour, is one point.
			TArray<FVector2f> Curve;
			for (int32 Level = 0; Level <= Levels; ++Level)
			{
				const FVector2f Point(At(From, static_cast<float>(Level) / Levels).X, At(To, static_cast<float>(Level) / Levels).X);
				if (!Curve.IsEmpty() && Point.X - Curve.Last().X < 0.05f)
				{
					Curve.Last().Y = (Curve.Last().Y + Point.Y) * 0.5f;
					continue;
				}
				Curve.Add(Point);
			}
			for (int32 Index = 1; Index < Curve.Num(); ++Index)
			{
				const float Run = Curve[Index].X - Curve[Index - 1].X;
				Curve[Index].Y = Curve[Index - 1].Y + FMath::Clamp(Curve[Index].Y - Curve[Index - 1].Y, MinSlope * Run, MaxSlope * Run);
			}

			// Colour: the mean of each band of lightness, the same share of pixels in each image, and one overall spread.
			TArray<FVector3f> MeanFrom, MeanTo;
			FVector2d SpreadFrom = FVector2d::ZeroVector, SpreadTo = FVector2d::ZeroVector;
			for (int32 Band = 0; Band < Bands; ++Band)
			{
				auto BandMean = [&](const TArray<FVector3f>& Sorted, FVector2d& Spread)
				{
					const int32 First = Band * Sorted.Num() / Bands;
					const int32 Last = FMath::Max(First + 1, (Band + 1) * Sorted.Num() / Bands);
					FVector3d Sum = FVector3d::ZeroVector;
					for (int32 Index = First; Index < Last; ++Index) Sum += FVector3d(Sorted[Index]);
					const FVector3f Mean(Sum / (Last - First));
					for (int32 Index = First; Index < Last; ++Index)
					{
						Spread += FVector2d(FMath::Square(Sorted[Index].Y - Mean.Y), FMath::Square(Sorted[Index].Z - Mean.Z));
					}
					return Mean;
				};
				MeanFrom.Add(BandMean(From, SpreadFrom));
				MeanTo.Add(BandMean(To, SpreadTo));
			}
			auto GainOf = [&](const double To2, const double From2)
			{
				return static_cast<float>(FMath::Clamp(FMath::Sqrt((To2 / To.Num() + 1.0) / (From2 / From.Num() + 1.0)), 0.5, 2.0));
			};
			const FVector2f Gain(GainOf(SpreadTo.X, SpreadFrom.X), GainOf(SpreadTo.Y, SpreadFrom.Y));
			auto BandsAt = [&](const float Lightness, FVector2f& OutFrom, FVector2f& OutTo)
			{
				int32 Upper = 0;
				while (Upper < Bands && MeanFrom[Upper].X < Lightness) ++Upper;
				const int32 Lower = FMath::Max(Upper - 1, 0);
				Upper = FMath::Min(Upper, Bands - 1);
				const float Blend = Upper == Lower ? 0.f : FMath::Clamp((Lightness - MeanFrom[Lower].X) / FMath::Max(MeanFrom[Upper].X - MeanFrom[Lower].X, 1e-4f), 0.f, 1.f);
				OutFrom = FMath::Lerp(FVector2f(MeanFrom[Lower].Y, MeanFrom[Lower].Z), FVector2f(MeanFrom[Upper].Y, MeanFrom[Upper].Z), Blend);
				OutTo = FMath::Lerp(FVector2f(MeanTo[Lower].Y, MeanTo[Lower].Z), FVector2f(MeanTo[Upper].Y, MeanTo[Upper].Z), Blend);
			};

			FTable Table = Identity();
			for (FVector3f& Node : Table)
			{
				const FVector3f Lab = ToLab(Node);
				FVector2f ColourFrom, ColourTo;
				BandsAt(Lab.X, ColourFrom, ColourTo);
				const FVector2f Colour = (FVector2f(Lab.Y, Lab.Z) - ColourFrom) * Gain + ColourTo;
				Node = FromLab(FVector3f(FMath::Clamp(AlongCurve(Curve, Lab.X), 0.f, 100.f), Colour.X, Colour.Y));
			}
			return Table;
		}

		/** Each node with its six neighbours, so a step between nodes never shows as a band across a gradient. */
		FTable Smooth(const FTable& Table)
		{
			FTable Out = Table;
			for (int32 B = 0; B < Size; ++B)
			{
				for (int32 G = 0; G < Size; ++G)
				{
					for (int32 R = 0; R < Size; ++R)
					{
						FVector3f Sum = Table[IndexOf(R, G, B)] * 2.f;
						float Weight = 2.f;
						const FIntVector Steps[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
						for (const FIntVector& Step : Steps)
						{
							const int32 NR = R + Step.X, NG = G + Step.Y, NB = B + Step.Z;
							if (NR < 0 || NG < 0 || NB < 0 || NR >= Size || NG >= Size || NB >= Size) continue;
							Sum += Table[IndexOf(NR, NG, NB)];
							Weight += 1.f;
						}
						Out[IndexOf(R, G, B)] = Sum / Weight;
					}
				}
			}
			return Out;
		}
	}

	FTable Identity()
	{
		FTable Table;
		Table.SetNumUninitialized(Nodes);
		for (int32 B = 0; B < Size; ++B)
		{
			for (int32 G = 0; G < Size; ++G)
			{
				for (int32 R = 0; R < Size; ++R)
				{
					Table[IndexOf(R, G, B)] = FVector3f(R, G, B) / (Size - 1);
				}
			}
		}
		return Table;
	}

	FTable Fit(TConstArrayView<FColor> Candidate, TConstArrayView<FColor> Reference, const bool bAligned)
	{
		const FTable Neutral = Identity();
		const int32 Count = FMath::Min(Candidate.Num(), Reference.Num());
		if (Count == 0)
		{
			return Neutral;
		}
		if (!bAligned)
		{
			return Smooth(FitLook(Candidate, Reference));
		}
		FTable Table;
		Table.SetNumUninitialized(Nodes);
		// The same view: every pixel says what its colour should become. Each lands on the eight nodes around it.
		double A[3][4];
		FitAffine(Candidate, Reference, Count, A);
		TArray<FVector3f> Sum;
		TArray<float> Weight;
		Sum.Init(FVector3f::ZeroVector, Nodes);
		Weight.Init(0.f, Nodes);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVector3f At = ToUnit(Candidate[Index]) * (Size - 1);
			const FVector3f Target = ToUnit(Reference[Index]);
			const int32 R0 = FMath::Clamp(FMath::FloorToInt(At.X), 0, Size - 2);
			const int32 G0 = FMath::Clamp(FMath::FloorToInt(At.Y), 0, Size - 2);
			const int32 B0 = FMath::Clamp(FMath::FloorToInt(At.Z), 0, Size - 2);
			const float FR = At.X - R0, FG = At.Y - G0, FB = At.Z - B0;
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const int32 DR = Corner & 1, DG = (Corner >> 1) & 1, DB = (Corner >> 2) & 1;
				const float W = (DR ? FR : 1.f - FR) * (DG ? FG : 1.f - FG) * (DB ? FB : 1.f - FB);
				const int32 Node = IndexOf(R0 + DR, G0 + DG, B0 + DB);
				Sum[Node] += Target * W;
				Weight[Node] += W;
			}
		}
		for (int32 Node = 0; Node < Nodes; ++Node)
		{
			const FVector3f& In = Neutral[Node];
			const FVector3f Prior(A[0][0] * In.X + A[0][1] * In.Y + A[0][2] * In.Z + A[0][3], A[1][0] * In.X + A[1][1] * In.Y + A[1][2] * In.Z + A[1][3],
				A[2][0] * In.X + A[2][1] * In.Y + A[2][2] * In.Z + A[2][3]);
			Table[Node] = Saturate((Sum[Node] + Prior * PriorWeight) / (Weight[Node] + PriorWeight));
		}
		const FTable Smoothed = Smooth(Table);
		for (int32 Node = 0; Node < Nodes; ++Node)
		{
			Table[Node] = Saturate((Table[Node] + Smoothed[Node]) * 0.5f);
		}
		return Table;
	}

	FVector3f Sample(const FTable& Table, const FVector3f& Colour)
	{
		const FVector3f At = Saturate(Colour) * (Size - 1);
		const int32 R0 = FMath::Min(FMath::FloorToInt(At.X), Size - 2);
		const int32 G0 = FMath::Min(FMath::FloorToInt(At.Y), Size - 2);
		const int32 B0 = FMath::Min(FMath::FloorToInt(At.Z), Size - 2);
		const float FR = At.X - R0, FG = At.Y - G0, FB = At.Z - B0;
		auto Node = [&](const int32 DR, const int32 DG, const int32 DB) { return Table[IndexOf(R0 + DR, G0 + DG, B0 + DB)]; };
		const FVector3f G0B0 = FMath::Lerp(Node(0, 0, 0), Node(1, 0, 0), FR);
		const FVector3f G1B0 = FMath::Lerp(Node(0, 1, 0), Node(1, 1, 0), FR);
		const FVector3f G0B1 = FMath::Lerp(Node(0, 0, 1), Node(1, 0, 1), FR);
		const FVector3f G1B1 = FMath::Lerp(Node(0, 1, 1), Node(1, 1, 1), FR);
		return FMath::Lerp(FMath::Lerp(G0B0, G1B0, FG), FMath::Lerp(G0B1, G1B1, FG), FB);
	}

	FTable Compose(const FTable& Second, const FTable& First)
	{
		FTable Out;
		Out.SetNumUninitialized(Nodes);
		for (int32 Node = 0; Node < Nodes; ++Node)
		{
			Out[Node] = Sample(Second, First[Node]);
		}
		return Out;
	}

	FTable Weaken(const FTable& Table, const float Intensity)
	{
		const FTable Neutral = Identity();
		FTable Out;
		Out.SetNumUninitialized(Nodes);
		for (int32 Node = 0; Node < Nodes; ++Node)
		{
			Out[Node] = FMath::Lerp(Neutral[Node], Table[Node], FMath::Clamp(Intensity, 0.f, 1.f));
		}
		return Out;
	}

	TArray<FColor> Apply(const FTable& Table, TConstArrayView<FColor> Pixels)
	{
		TArray<FColor> Out;
		Out.Reserve(Pixels.Num());
		for (const FColor& Pixel : Pixels)
		{
			Out.Add(ToColor(Sample(Table, ToUnit(Pixel))));
		}
		return Out;
	}

	TArray<FColor> ToStrip(const FTable& Table)
	{
		TArray<FColor> Strip;
		Strip.SetNumUninitialized(StripWidth * StripHeight);
		for (int32 Y = 0; Y < StripHeight; ++Y)
		{
			for (int32 X = 0; X < StripWidth; ++X)
			{
				Strip[Y * StripWidth + X] = ToColor(Table[IndexOf(X % Size, Y, X / Size)]);
			}
		}
		return Strip;
	}

	bool FromStrip(TConstArrayView<FColor> Strip, const int32 Width, const int32 Height, FTable& OutTable)
	{
		if (Width != StripWidth || Height != StripHeight || Strip.Num() != StripWidth * StripHeight)
		{
			return false;
		}
		OutTable.SetNumUninitialized(Nodes);
		for (int32 Y = 0; Y < StripHeight; ++Y)
		{
			for (int32 X = 0; X < StripWidth; ++X)
			{
				OutTable[IndexOf(X % Size, Y, X / Size)] = ToUnit(Strip[Y * StripWidth + X]);
			}
		}
		return true;
	}
}
