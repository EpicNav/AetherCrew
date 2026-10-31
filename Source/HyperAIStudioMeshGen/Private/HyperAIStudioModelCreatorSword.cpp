// Games by Hyper 2026.

#include "HyperAIStudioModelCreator.h"

#include "Algo/BinarySearch.h"

/**
 * AAA swords: what a generator cannot make well, made exactly. The generated hilt (guard, grip, pommel) is kept, slimmed;
 * the generated blade, lumpy and textured with baked light, is replaced by an exact one (distal taper, a fuller, an ogival
 * point, sharp edges, clean UVs); the hilt's colour atlas is made physically based by material; and the sword is balanced
 * like a real one by the pommel. The pure parts live here (geometry, balance, the atlas); the asset work is in
 * HyperAIStudioModelInspect.cpp's weapon finishing. A C++ port of the numpy and Editor Python prototypes proven on the
 * user's longsword (31 October 2026), so it needs no Python packages the editor does not ship and runs in its tests.
 */
namespace HyperAIStudio::MeshGen::Creator
{
	namespace SwordDetail
	{
		/** Hue in degrees, saturation and value of an sRGB colour, 0-1 each channel. */
		void Hsv(const float R, const float G, const float B, float& OutHue, float& OutSat, float& OutValue)
		{
			const float Max = FMath::Max3(R, G, B);
			const float Min = FMath::Min3(R, G, B);
			const float Delta = Max - Min + 1e-8f;
			float Hue = 0.f;
			if (Max == R)
			{
				Hue = FMath::Fmod((G - B) / Delta, 6.f);
				Hue = Hue < 0.f ? Hue + 6.f : Hue;
			}
			else if (Max == G)
			{
				Hue = (B - R) / Delta + 2.f;
			}
			else
			{
				Hue = (R - G) / Delta + 4.f;
			}
			OutHue = Hue * 60.f;
			OutSat = Max > 0.f ? (Max - Min) / (Max + 1e-8f) : 0.f;
			OutValue = Max;
		}

		bool InHue(const float Hue, const float Min, const float Max)
		{
			return Min <= Max ? Hue >= Min && Hue <= Max : Hue >= Min || Hue <= Max;
		}

		bool Matches(const FMaterialClass& Class, const float Hue, const float Sat, const float Value)
		{
			return InHue(Hue, Class.HueMin, Class.HueMax) && Sat > Class.SatMin && Value >= Class.ValueMin && Value <= Class.ValueMax;
		}

		/** A separable box mean of radius Radius, edges clamped, by running sums. */
		TArray<float> BoxMean(const TArray<float>& In, const int32 Width, const int32 Height, const int32 Radius)
		{
			TArray<float> Rows;
			Rows.SetNumUninitialized(In.Num());
			const float Span = float(2 * Radius + 1);
			for (int32 Y = 0; Y < Height; ++Y)
			{
				const float* Row = In.GetData() + int64(Y) * Width;
				double Sum = 0.0;
				for (int32 I = -Radius; I <= Radius; ++I)
				{
					Sum += Row[FMath::Clamp(I, 0, Width - 1)];
				}
				for (int32 X = 0; X < Width; ++X)
				{
					Rows[int64(Y) * Width + X] = float(Sum / Span);
					Sum += Row[FMath::Clamp(X + Radius + 1, 0, Width - 1)] - Row[FMath::Clamp(X - Radius, 0, Width - 1)];
				}
			}
			TArray<float> Out;
			Out.SetNumUninitialized(In.Num());
			for (int32 X = 0; X < Width; ++X)
			{
				double Sum = 0.0;
				for (int32 I = -Radius; I <= Radius; ++I)
				{
					Sum += Rows[int64(FMath::Clamp(I, 0, Height - 1)) * Width + X];
				}
				for (int32 Y = 0; Y < Height; ++Y)
				{
					Out[int64(Y) * Width + X] = float(Sum / Span);
					Sum += Rows[int64(FMath::Clamp(Y + Radius + 1, 0, Height - 1)) * Width + X] - Rows[int64(FMath::Clamp(Y - Radius, 0, Height - 1)) * Width + X];
				}
			}
			return Out;
		}

		float ToLinear(const float C)
		{
			return C <= 0.04045f ? C / 12.92f : FMath::Pow((C + 0.055f) / 1.055f, 2.4f);
		}

		float ToSrgb(const float L)
		{
			return L <= 0.0031308f ? L * 12.92f : 1.055f * FMath::Pow(FMath::Clamp(L, 0.f, 1.f), 1.f / 2.4f) - 0.055f;
		}

		uint8 Byte(const float Value)
		{
			return uint8(FMath::Clamp(Value, 0.f, 1.f) * 255.f + 0.5f);
		}

		// ------------------------------------------------------------ the palette's classes

		FMaterialClass Metal(const TCHAR* Name, const FVector3f& Albedo, const float Pull, const float HueMin, const float HueMax, const float SatMin, const float ValueMin,
			const float RoughBase, const float RoughSpan)
		{
			FMaterialClass Class;
			Class.Name = Name;
			Class.bMetal = true;
			Class.Albedo = Albedo;
			Class.Pull = Pull;
			Class.HueMin = HueMin;
			Class.HueMax = HueMax;
			Class.SatMin = SatMin;
			Class.ValueMin = ValueMin;
			Class.RoughBase = RoughBase;
			Class.RoughSpan = RoughSpan;
			return Class;
		}

		FMaterialClass Gold() { return Metal(TEXT("gold"), FVector3f(1.00f, 0.71f, 0.29f), 0.65f, 25.f, 65.f, 0.28f, 0.30f, 0.26f, 0.18f); }
		FMaterialClass Brass() { return Metal(TEXT("brass"), FVector3f(0.91f, 0.78f, 0.42f), 0.65f, 25.f, 65.f, 0.28f, 0.30f, 0.28f, 0.18f); }

		FMaterialClass WhiteMetal(const TCHAR* Name, const FVector3f& Albedo)
		{
			FMaterialClass Class = Metal(Name, Albedo, 0.70f, 0.f, 360.f, -1.f, 0.f, 0.20f, 0.15f);
			Class.bFallback = true;
			Class.bBladeSteel = true;
			return Class;
		}

		FMaterialClass Leather()
		{
			FMaterialClass Class;
			Class.Name = TEXT("leather");
			Class.HueMin = 340.f;
			Class.HueMax = 30.f;
			Class.SatMin = 0.25f;
			Class.ValueMax = 0.55f;
			Class.bUseTone = true;
			Class.LinearMin = 0.008f;
			Class.LinearMax = 0.10f;
			Class.RoughBase = 0.58f;
			Class.RoughSpan = 0.14f;
			return Class;
		}

		FMaterialClass Wood()
		{
			FMaterialClass Class = Leather();
			Class.Name = TEXT("wood");
			Class.HueMin = 15.f;
			Class.HueMax = 50.f;
			Class.SatMin = 0.20f;
			Class.ValueMax = 0.70f;
			Class.LinearMin = 0.01f;
			Class.LinearMax = 0.30f;
			Class.RoughBase = 0.62f;
			Class.RoughSpan = 0.12f;
			return Class;
		}

		FMaterialClass Gem(const TCHAR* Name, const float HueMin, const float HueMax)
		{
			FMaterialClass Class;
			Class.Name = Name;
			Class.HueMin = HueMin;
			Class.HueMax = HueMax;
			Class.SatMin = 0.35f;
			Class.Darken = 0.8f;
			Class.RoughBase = 0.04f;
			Class.VoteWeight = 1.2f;
			return Class;
		}

		bool Has(const TArray<FMaterialClass>& Palette, const TCHAR* Name)
		{
			return Palette.ContainsByPredicate([Name](const FMaterialClass& Class) { return Class.Name == Name; });
		}

		bool AnyOf(const FString& Text, std::initializer_list<const TCHAR*> Words)
		{
			for (const TCHAR* Word : Words)
			{
				if (Text.Contains(Word))
				{
					return true;
				}
			}
			return false;
		}
	}

	using namespace SwordDetail;

	bool IsSwordWords(const FString& Words)
	{
		const FString Text = Words.ToLower();
		const bool bSword = AnyOf(Text, {TEXT("sword"), TEXT("claymore"), TEXT("katana"), TEXT("sabre"), TEXT("saber"), TEXT("rapier"), TEXT("scimitar"), TEXT("falchion"),
			TEXT("gladius"), TEXT("cutlass"), TEXT("zweihander"), TEXT("blade")});
		const bool bOther = AnyOf(Text, {TEXT("axe"), TEXT("spear"), TEXT("hammer"), TEXT("mace"), TEXT("dagger"), TEXT("knife"), TEXT("halberd"), TEXT("staff")});
		return bSword && !bOther;
	}

	TArray<FString> PaletteWords(const FString& Text)
	{
		static const TCHAR* const Known[] = {TEXT("gold"), TEXT("gilded"), TEXT("gilt"), TEXT("brass"), TEXT("bronze"), TEXT("silver"), TEXT("steel"), TEXT("iron"),
			TEXT("leather"), TEXT("wrap"), TEXT("hide"), TEXT("wood"), TEXT("oak"), TEXT("ebony"), TEXT("sapphire"), TEXT("ruby"), TEXT("garnet"), TEXT("emerald"), TEXT("jade"),
			TEXT("amethyst"), TEXT("gem"), TEXT("jewel"), TEXT("crystal")};
		const FString Lower = Text.ToLower();
		TArray<FString> Out;
		for (const TCHAR* Word : Known)
		{
			if (Lower.Contains(Word))
			{
				Out.Add(Word);
			}
		}
		return Out;
	}

	void MakeSwordFinish(FWeaponFinish& Finish, const bool bKeepBudget)
	{
		const FSwordSpec Spec = SwordSpecFor(Finish.LengthCm);
		Finish.bSword = true;
		Finish.GuardSpanCm = static_cast<float>(Spec.GuardSpan);
		Finish.GuardDepthCm = static_cast<float>(Spec.GuardDepth);
		Finish.GripCm = static_cast<float>(Spec.GripDiameter);
		Finish.PommelCm = static_cast<float>(Spec.PommelDiameter);
		Finish.ColorSize = 4096;
		Finish.NormalSize = 4096;
		Finish.MaskSize = 2048;
		if (!bKeepBudget)
		{
			Finish.TargetTris = 36000;
		}
	}

	FSwordSpec SwordSpecFor(const double LengthCm)
	{
		FSwordSpec Spec;
		if (LengthCm <= 0.0)
		{
			return Spec;
		}
		// A longsword's blade is 92 of its 110 cm; a shorter or longer sword keeps those proportions, widths following.
		const double Scale = FMath::Clamp(LengthCm * (92.0 / 110.0) / Spec.BladeLength, 0.6, 1.3);
		Spec.BladeLength = LengthCm * (92.0 / 110.0);
		Spec.BaseWidth *= Scale;
		Spec.PointWidth *= Scale;
		Spec.FullerWidth *= Scale;
		Spec.GuardSpan *= Scale;
		return Spec;
	}

	void BladeProfile(const FSwordSpec& Spec, const double S, double& OutWidth, double& OutThickness)
	{
		if (S <= Spec.PointStart)
		{
			const double F = S / Spec.PointStart;
			OutWidth = Spec.BaseWidth + (Spec.PointWidth - Spec.BaseWidth) * F;
			OutThickness = Spec.BaseThickness + (Spec.MidThickness - Spec.BaseThickness) * F;
		}
		else
		{
			// An ogival point.
			const double Q = (S - Spec.PointStart) / (1.0 - Spec.PointStart);
			OutWidth = Spec.PointWidth * FMath::Pow(FMath::Max(0.0, 1.0 - Q), 0.75);
			OutThickness = Spec.MidThickness + (Spec.TipThickness - Spec.MidThickness) * Q;
		}
	}

	FBladeMesh BuildBlade(const FSwordSpec& Spec, const double GuardBottom, const FVector2d& Centre)
	{
		FBladeMesh Out;
		const int32 Rings = int32(Spec.BladeLength / Spec.RingCm) + 1;
		const int32 Columns = 2 * Spec.Columns + 1;
		for (const int32 Side : {1, -1})
		{
			const int32 Base = Out.Positions.Num();
			for (int32 Ring = 0; Ring <= Rings; ++Ring)
			{
				const double S = double(Ring) / Rings;
				double Width = 0.0, Thick = 0.0;
				BladeProfile(Spec, S, Width, Thick);
				const double Y = GuardBottom + Spec.Ricasso - S * (Spec.BladeLength + Spec.Ricasso);
				// A little thicker where it seats (the 2 cm below the guard and the ricasso inside it), never wider: a blade
				// wider than the guard's throat shows its top as a shelf.
				const double Flare = FMath::Square(FMath::Clamp((Y - (GuardBottom - 2.0)) / (Spec.Ricasso + 2.0), 0.0, 1.0));
				Thick *= 1.0 + Spec.RicassoThicken * Flare;
				for (int32 Column = 0; Column < Columns; ++Column)
				{
					const double U = -1.0 + 2.0 * Column / (Columns - 1);
					const double X = U * Width * 0.5;
					// A convex flat falling to a sharp edge, the fuller's cos^2 groove down its middle.
					double Z = Thick * 0.5 * (1.0 - FMath::Pow(FMath::Abs(U), 1.9));
					if (S >= Spec.FullerStart && S <= Spec.FullerEnd && FMath::Abs(X) < Spec.FullerWidth * 0.5)
					{
						const double Ramp = FMath::Min3(1.0, (S - Spec.FullerStart) / 0.03, (Spec.FullerEnd - S) / 0.05);
						Z -= FMath::Min(Spec.FullerDepth, Thick * 0.3) * Ramp * FMath::Square(FMath::Cos(UE_DOUBLE_PI * X / Spec.FullerWidth));
					}
					Out.Positions.Add(FVector3d(Centre.X + X, Y, Centre.Y + Side * Z));
					Out.UVs.Add(FVector2f(float(Side > 0 ? 0.25 + 0.24 * U : 0.75 - 0.24 * U), float(S)));
				}
			}
			for (int32 Ring = 0; Ring < Rings; ++Ring)
			{
				for (int32 Column = 0; Column < Columns - 1; ++Column)
				{
					const int32 A = Base + Ring * Columns + Column, B = A + 1, C = A + Columns, D = C + 1;
					for (FIntVector Tri : {FIntVector(A, B, C), FIntVector(B, D, C)})
					{
						const FVector3d P0 = Out.Positions[Tri.X], P1 = Out.Positions[Tri.Y], P2 = Out.Positions[Tri.Z];
						// Unreal's normal is (C - A) x (B - A); the front faces +Z, the back -Z.
						const FVector3d Normal = (P2 - P0).Cross(P1 - P0);
						// Slivers (where the point's width runs out, two corners nearly one) go.
						const double Longest = FMath::Max3((P1 - P0).SquaredLength(), (P2 - P1).SquaredLength(), (P0 - P2).SquaredLength());
						if (Normal.Length() <= 1e-3 * Longest)
						{
							continue;
						}
						// A triangle across a face points along Z; one standing side-on (at the point, where the width runs out)
						// points away from the blade's middle.
						const FVector3d Middle(Centre.X, GuardBottom - Spec.BladeLength * 0.5, Centre.Y);
						const double Facing = FMath::Abs(Normal.Z) > 1e-6 * Normal.Length() ? Normal.Z * Side : Normal.Dot((P0 + P1 + P2) / 3.0 - Middle);
						if (Facing < 0.0)
						{
							Swap(Tri.Y, Tri.Z);
						}
						Out.Triangles.Add(Tri);
					}
				}
			}
			if (Side > 0)
			{
				Out.FrontVertices = Out.Positions.Num();
			}
		}
		// Per-vertex normals, each face its own: smooth across a face, hard at the edges where the faces meet.
		TArray<FVector3d> Sum;
		Sum.Init(FVector3d::Zero(), Out.Positions.Num());
		for (const FIntVector& Tri : Out.Triangles)
		{
			const FVector3d Normal = (Out.Positions[Tri.Z] - Out.Positions[Tri.X]).Cross(Out.Positions[Tri.Y] - Out.Positions[Tri.X]);
			Sum[Tri.X] += Normal;
			Sum[Tri.Y] += Normal;
			Sum[Tri.Z] += Normal;
		}
		for (int32 Index = 0; Index < Sum.Num(); ++Index)
		{
			const FVector3d Fallback(0.0, 0.0, Index < Out.FrontVertices ? 1.0 : -1.0);
			Out.Normals.Add(FVector3f(Sum[Index].GetSafeNormal(1e-12, Fallback)));
		}
		return Out;
	}

	TArray<double> SliceAreas(const TArray<FVector3d>& Positions, const TArray<FIntVector>& Triangles, const TArray<double>& Ys)
	{
		TArray<double> Areas;
		Areas.Init(0.0, Ys.Num());
		for (const FIntVector& Tri : Triangles)
		{
			const FVector3d V[3] = {Positions[Tri.X], Positions[Tri.Y], Positions[Tri.Z]};
			const double Low = FMath::Min3(V[0].Y, V[1].Y, V[2].Y);
			const double High = FMath::Max3(V[0].Y, V[1].Y, V[2].Y);
			const FVector3d Normal = (V[2] - V[0]).Cross(V[1] - V[0]);
			// The slices this triangle crosses: Ys rise, so a binary search finds the first.
			int32 First = Algo::LowerBound(Ys, Low);
			for (int32 K = First; K < Ys.Num() && Ys[K] < High; ++K)
			{
				const double Y = Ys[K];
				if (Ys[K] <= Low)
				{
					continue;
				}
				FVector3d Cut[2];
				int32 Found = 0;
				for (int32 Edge = 0; Edge < 3 && Found < 2; ++Edge)
				{
					const FVector3d& A = V[Edge];
					const FVector3d& B = V[(Edge + 1) % 3];
					const double Da = A.Y - Y, Db = B.Y - Y;
					if (Da * Db < 0.0)
					{
						Cut[Found++] = A + (B - A) * (Da / (Da - Db));
					}
				}
				if (Found != 2)
				{
					continue;
				}
				// The segment oriented so the solid lies to its left seen from +Y, by the face's outward normal; then the
				// shoelace term of Green's theorem over the cross-section.
				const FVector3d Segment = Cut[1] - Cut[0];
				const double Sign = FMath::Sign(Segment.Cross(FVector3d(0.0, 1.0, 0.0)).Dot(Normal));
				Areas[K] += 0.5 * Sign * (Cut[0].X * Cut[1].Z - Cut[1].X * Cut[0].Z);
			}
		}
		for (double& Area : Areas)
		{
			Area = FMath::Abs(Area);
		}
		return Areas;
	}

	FSwordBalance SwordBalance(const FSwordSpec& Spec, const TArray<FVector3d>& Hilt, const TArray<FIntVector>& Triangles, const FHiltFrame& Frame)
	{
		FSwordBalance Out;
		double Moment = 0.0;
		// The blade from its spec: the lenticular section's area is w t (1 - 1/2.9), the fuller taking some 3% more.
		constexpr int32 Steps = 400;
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			const double S = (Step + 0.5) / Steps;
			double Width = 0.0, Thick = 0.0;
			BladeProfile(Spec, S, Width, Thick);
			const double Mass = Width * Thick * 0.655 * (Spec.BladeLength / Steps) * Spec.SteelDensity;
			Out.BladeG += Mass;
			Moment += Mass * (Frame.GuardBottom - S * Spec.BladeLength);
		}
		// The hilt sliced every 2.5 mm, each slice's exact area, with the density of the part it is in.
		constexpr double Slice = 0.25;
		double Top = Frame.HandleTop;
		for (const FVector3d& P : Hilt)
		{
			Top = FMath::Max(Top, P.Y);
		}
		TArray<double> Ys;
		for (double Y = Frame.GuardBottom + 0.5 + Slice * 0.5; Y < Top; Y += Slice)
		{
			Ys.Add(Y);
		}
		const TArray<double> Areas = SliceAreas(Hilt, Triangles, Ys);
		for (int32 K = 0; K < Ys.Num(); ++K)
		{
			const bool bBrass = Ys[K] <= Frame.GuardTop || Ys[K] >= Frame.GripEnd;
			const double Mass = Areas[K] * Slice * (bBrass ? Spec.BrassDensity : Spec.GripDensity);
			Out.HiltG += Mass;
			Moment += Mass * Ys[K];
		}
		Out.MassG = Out.BladeG + Out.HiltG;
		Out.BalanceCm = Out.MassG > 0.0 ? Frame.GuardBottom - Moment / Out.MassG : 0.0;
		Out.bInWindow = Out.BalanceCm >= Spec.BalanceMin && Out.BalanceCm <= Spec.BalanceMax;
		return Out;
	}

	void ScalePommel(TArray<FVector3d>& Hilt, const FHiltFrame& Frame, const double K)
	{
		FVector2d Centre = FVector2d::Zero();
		int32 Count = 0;
		for (const FVector3d& P : Hilt)
		{
			if (P.Y >= Frame.GripEnd)
			{
				Centre += FVector2d(P.X, P.Z);
				++Count;
			}
		}
		if (Count == 0)
		{
			return;
		}
		Centre /= double(Count);
		for (FVector3d& P : Hilt)
		{
			if (P.Y >= Frame.GripEnd)
			{
				P = FVector3d(Centre.X + (P.X - Centre.X) * K, Frame.GripEnd + (P.Y - Frame.GripEnd) * K, Centre.Y + (P.Z - Centre.Y) * K);
			}
		}
	}

	FSwordBalance BalanceSword(const FSwordSpec& Spec, const TArray<FVector3d>& Hilt, const TArray<FIntVector>& Triangles, const FHiltFrame& Frame)
	{
		const double Middle = (Spec.BalanceMin + Spec.BalanceMax) * 0.5;
		FSwordBalance Best;
		double BestMiss = TNumericLimits<double>::Max();
		constexpr int32 Tries = 23;
		for (int32 Try = 0; Try < Tries; ++Try)
		{
			const double K = Spec.PommelScaleMin + (Spec.PommelScaleMax - Spec.PommelScaleMin) * Try / (Tries - 1);
			TArray<FVector3d> Scaled = Hilt;
			ScalePommel(Scaled, Frame, K);
			FSwordBalance Each = SwordBalance(Spec, Scaled, Triangles, Frame);
			Each.PommelScale = K;
			const double Miss = FMath::Abs(Each.BalanceCm - Middle);
			if (Miss < BestMiss)
			{
				BestMiss = Miss;
				Best = Each;
			}
		}
		return Best;
	}

	bool SolidMetalTexel(const FAtlasResult& Atlas, const TArray<FMaterialClass>& Palette, const int32 Window, FVector2f& OutUV)
	{
		if (Atlas.Width < Window || Atlas.Height < Window || Palette.IsEmpty())
		{
			return false;
		}
		// Gold or brass first; the white metal when the hilt has no coloured one.
		TArray<int32> Wanted;
		for (int32 Class = 0; Class < Palette.Num(); ++Class)
		{
			if (Palette[Class].bMetal && !Palette[Class].bFallback)
			{
				Wanted.Add(Class);
			}
		}
		for (int32 Class = 0; Class < Palette.Num(); ++Class)
		{
			if (Palette[Class].bMetal && Palette[Class].bFallback)
			{
				Wanted.Add(Class);
			}
		}
		const int32 Width = Atlas.Width, Height = Atlas.Height;
		for (const int32 Class : Wanted)
		{
			// A summed-area table of the texels that are this metal, fully metal and unoccluded.
			TArray<int32> Sum;
			Sum.Init(0, (Width + 1) * (Height + 1));
			for (int32 Y = 0; Y < Height; ++Y)
			{
				int32 Row = 0;
				for (int32 X = 0; X < Width; ++X)
				{
					const int64 Index = int64(Y) * Width + X;
					const FColor& ORM = Atlas.ORM[Index];
					Row += Atlas.Classes[Index] == Class && ORM.B >= 240 && ORM.R >= 230;
					Sum[(Y + 1) * (Width + 1) + X + 1] = Sum[Y * (Width + 1) + X + 1] + Row;
				}
			}
			const int32 Full = Window * Window;
			for (int32 Y = 0; Y + Window <= Height; Y += 2)
			{
				for (int32 X = 0; X + Window <= Width; X += 2)
				{
					const int32 Count = Sum[(Y + Window) * (Width + 1) + X + Window] - Sum[Y * (Width + 1) + X + Window] - Sum[(Y + Window) * (Width + 1) + X] + Sum[Y * (Width + 1) + X];
					if (Count == Full)
					{
						OutUV = FVector2f((X + Window * 0.5f) / Width, (Y + Window * 0.5f) / Height);
						return true;
					}
				}
			}
		}
		return false;
	}

	TArray<FMaterialClass> MaterialPalette(const TArray<FString>& Words, const TArray<FColor>& Picture, TArray<FString>& OutReasons)
	{
		FString Text = FString::Join(Words, TEXT(" ")).ToLower();
		TArray<FMaterialClass> Gems, Metals, Others;
		auto Named = [&OutReasons](const FMaterialClass& Class, TArray<FMaterialClass>& Into, const TCHAR* Why)
		{
			if (!Into.ContainsByPredicate([&Class](const FMaterialClass& Each) { return Each.Name == Class.Name; }))
			{
				Into.Add(Class);
				OutReasons.Add(FString::Printf(TEXT("palette: %s, %s"), *Class.Name, Why));
			}
		};
		// The brief's words.
		if (AnyOf(Text, {TEXT("sapphire")})) Named(Gem(TEXT("sapphire"), 190.f, 265.f), Gems, TEXT("from the brief"));
		if (AnyOf(Text, {TEXT("ruby"), TEXT("garnet")})) Named(Gem(TEXT("ruby"), 340.f, 15.f), Gems, TEXT("from the brief"));
		if (AnyOf(Text, {TEXT("emerald"), TEXT("jade")})) Named(Gem(TEXT("emerald"), 90.f, 170.f), Gems, TEXT("from the brief"));
		if (AnyOf(Text, {TEXT("amethyst")})) Named(Gem(TEXT("amethyst"), 265.f, 320.f), Gems, TEXT("from the brief"));
		if (Gems.IsEmpty() && AnyOf(Text, {TEXT("gem"), TEXT("jewel"), TEXT("crystal")})) Named(Gem(TEXT("sapphire"), 190.f, 265.f), Gems, TEXT("a gem the brief names, blue unless the picture says otherwise"));
		if (AnyOf(Text, {TEXT("gold"), TEXT("gilded"), TEXT("gilt")})) Named(Gold(), Metals, TEXT("from the brief"));
		else if (AnyOf(Text, {TEXT("brass"), TEXT("bronze")})) Named(Brass(), Metals, TEXT("from the brief"));
		if (AnyOf(Text, {TEXT("leather"), TEXT("wrap"), TEXT("hide")})) Named(Leather(), Others, TEXT("from the brief"));
		if (AnyOf(Text, {TEXT("wood"), TEXT("oak"), TEXT("ebony")})) Named(Wood(), Others, TEXT("from the brief"));
		FMaterialClass Fallback = AnyOf(Text, {TEXT("steel"), TEXT("iron")}) ? WhiteMetal(TEXT("steel"), FVector3f(0.56f, 0.57f, 0.58f))
			: WhiteMetal(TEXT("silver"), FVector3f(0.91f, 0.90f, 0.87f));
		const bool bNamedAny = !Gems.IsEmpty() || !Metals.IsEmpty() || !Others.IsEmpty();

		// The reference picture's colours: a material it plainly shows that the words did not name.
		if (!Picture.IsEmpty())
		{
			int64 Counted = 0, Golden = 0, Blue = 0, Red = 0, Green = 0, Brown = 0;
			for (const FColor& Pixel : Picture)
			{
				if (Pixel.A < 128)
				{
					continue;
				}
				++Counted;
				float Hue, Sat, Value;
				Hsv(Pixel.R / 255.f, Pixel.G / 255.f, Pixel.B / 255.f, Hue, Sat, Value);
				Golden += InHue(Hue, 25.f, 65.f) && Sat > 0.28f && Value > 0.30f;
				Blue += InHue(Hue, 190.f, 265.f) && Sat > 0.35f;
				Red += InHue(Hue, 340.f, 15.f) && Sat > 0.5f && Value > 0.4f;
				Green += InHue(Hue, 90.f, 170.f) && Sat > 0.35f;
				Brown += InHue(Hue, 340.f, 30.f) && Sat > 0.25f && Value <= 0.55f;
			}
			auto Share = [Counted](const int64 Count) { return Counted > 0 ? double(Count) / Counted : 0.0; };
			if (Share(Golden) >= 0.02 && !Has(Metals, TEXT("gold")) && !Has(Metals, TEXT("brass"))) Named(Gold(), Metals, TEXT("the reference picture shows it"));
			if (Share(Blue) >= 0.003 && Gems.IsEmpty()) Named(Gem(TEXT("sapphire"), 190.f, 265.f), Gems, TEXT("the reference picture shows a blue stone"));
			if (Share(Red) >= 0.003 && !Has(Gems, TEXT("ruby"))) Named(Gem(TEXT("ruby"), 340.f, 15.f), Gems, TEXT("the reference picture shows a red stone"));
			if (Share(Green) >= 0.003 && !Has(Gems, TEXT("emerald"))) Named(Gem(TEXT("emerald"), 90.f, 170.f), Gems, TEXT("the reference picture shows a green stone"));
			if (Share(Brown) >= 0.02 && !Has(Others, TEXT("leather")) && !Has(Others, TEXT("wood"))) Named(Leather(), Others, TEXT("the reference picture shows a dark brown wrap"));
		}
		if (!bNamedAny && Picture.IsEmpty())
		{
			// Nothing to read: an ornate sword's usual materials.
			Named(Gem(TEXT("sapphire"), 190.f, 265.f), Gems, TEXT("HyperAI's default for an ornate hilt"));
			Named(Gold(), Metals, TEXT("HyperAI's default for an ornate hilt"));
			Named(Leather(), Others, TEXT("HyperAI's default for an ornate hilt"));
		}
		// Told apart in this order: stones, coloured metals, leather and wood, and the white metal catching the rest.
		TArray<FMaterialClass> Palette = Gems;
		Palette.Append(Metals);
		Palette.Append(Others);
		Palette.Add(Fallback);
		OutReasons.Add(FString::Printf(TEXT("palette: %s, every texel the others do not claim"), *Fallback.Name));
		return Palette;
	}

	FAtlasResult CorrectAtlas(const TArray<FColor>& Pixels, const int32 Width, const int32 Height, const TArray<FMaterialClass>& Palette)
	{
		FAtlasResult Out;
		const int64 Count = int64(Width) * Height;
		if (Width <= 0 || Height <= 0 || Pixels.Num() < Count || Palette.IsEmpty())
		{
			return Out;
		}
		Out.Width = Width;
		Out.Height = Height;
		const int32 Classes = Palette.Num();
		int32 Fallback = Palette.IndexOfByPredicate([](const FMaterialClass& Class) { return Class.bFallback; });
		Fallback = Fallback == INDEX_NONE ? Classes - 1 : Fallback;
		// Each texel's own class, the first that claims it.
		TArray<TArray<float>> Masks;
		Masks.SetNum(Classes);
		for (TArray<float>& Mask : Masks)
		{
			Mask.Init(0.f, Count);
		}
		TArray<float> Lum;
		Lum.SetNumUninitialized(Count);
		for (int64 Index = 0; Index < Count; ++Index)
		{
			const FColor& Pixel = Pixels[Index];
			const float R = Pixel.R / 255.f, G = Pixel.G / 255.f, B = Pixel.B / 255.f;
			Lum[Index] = 0.2126f * R + 0.7152f * G + 0.0722f * B;
			float Hue, Sat, Value;
			Hsv(R, G, B, Hue, Sat, Value);
			int32 Claim = Fallback;
			for (int32 Class = 0; Class < Classes; ++Class)
			{
				if (!Palette[Class].bFallback && Matches(Palette[Class], Hue, Sat, Value))
				{
					Claim = Class;
					break;
				}
			}
			Masks[Claim][Index] = 1.f;
		}
		// Only a one-texel majority clean-up: thin features (a wire over leather) stay what they are.
		TArray<TArray<float>> Votes;
		Votes.SetNum(Classes);
		for (int32 Class = 0; Class < Classes; ++Class)
		{
			Votes[Class] = BoxMean(Masks[Class], Width, Height, 1);
		}
		Out.Classes.SetNumUninitialized(Count);
		Out.Shares.Init(0.f, Classes);
		for (int64 Index = 0; Index < Count; ++Index)
		{
			int32 Pick = 0;
			float Most = -1.f;
			for (int32 Class = 0; Class < Classes; ++Class)
			{
				const float Vote = Votes[Class][Index] * Palette[Class].VoteWeight;
				if (Vote > Most)
				{
					Most = Vote;
					Pick = Class;
				}
			}
			Out.Classes[Index] = uint8(Pick);
			Out.Shares[Pick] += 1.f / float(Count);
		}
		// The cavity: how much darker a texel is than its 12-texel neighbourhood; occlusion from it.
		const TArray<float> Local = BoxMean(Lum, Width, Height, 12);
		TArray<float> Metal, Rough;
		Metal.SetNumUninitialized(Count);
		Rough.SetNumUninitialized(Count);
		Out.BaseColor.SetNumUninitialized(Count);
		Out.ORM.SetNumUninitialized(Count);
		TArray<float> Occlusion;
		Occlusion.SetNumUninitialized(Count);
		for (int64 Index = 0; Index < Count; ++Index)
		{
			const FMaterialClass& Class = Palette[Out.Classes[Index]];
			const float Near = Local[Index] + 1e-4f;
			const float Cavity = FMath::Clamp(Lum[Index] / Near, 0.f, 1.2f);
			Occlusion[Index] = FMath::Pow(FMath::Clamp(Cavity, 0.35f, 1.f), 0.8f);
			// Baked highlights pressed down to 1.05 x the neighbourhood: the lighting taken out, the engraving kept.
			const float Tone = FMath::Min(Lum[Index], Near * 1.05f) / FMath::Max(Near, 1e-4f);
			const FColor& Pixel = Pixels[Index];
			const FVector3f Source(ToLinear(Pixel.R / 255.f), ToLinear(Pixel.G / 255.f), ToLinear(Pixel.B / 255.f));
			FVector3f Linear = Source;
			if (Class.bMetal)
			{
				Linear = Class.Albedo * (Class.Pull * FMath::Clamp(Tone, 0.3f, 1.05f)) + Source * (1.f - Class.Pull);
				Rough[Index] = Class.RoughBase + Class.RoughSpan * (1.f - FMath::Clamp(Cavity, 0.f, 1.f));
			}
			else
			{
				Linear = Source * (Class.bUseTone ? FMath::Clamp(Tone, 0.4f, 1.f) : Class.Darken);
				Linear = FVector3f(FMath::Clamp(Linear.X, Class.LinearMin, Class.LinearMax), FMath::Clamp(Linear.Y, Class.LinearMin, Class.LinearMax),
					FMath::Clamp(Linear.Z, Class.LinearMin, Class.LinearMax));
				Rough[Index] = Class.RoughBase + Class.RoughSpan * FMath::Clamp(1.f - Lum[Index] / Near, -0.5f, 0.5f);
			}
			Metal[Index] = Class.bMetal ? 1.f : 0.f;
			Out.BaseColor[Index] = FColor(Byte(ToSrgb(Linear.X)), Byte(ToSrgb(Linear.Y)), Byte(ToSrgb(Linear.Z)), 255);
		}
		// Material borders softened by a texel, so mip-mapping does not alias them.
		const TArray<float> SoftMetal = BoxMean(Metal, Width, Height, 1);
		const TArray<float> SoftRough = BoxMean(Rough, Width, Height, 1);
		for (int64 Index = 0; Index < Count; ++Index)
		{
			Out.ORM[Index] = FColor(Byte(Occlusion[Index]), Byte(FMath::Clamp(SoftRough[Index], 0.03f, 1.f)), Byte(SoftMetal[Index]), 255);
		}
		return Out;
	}
}
