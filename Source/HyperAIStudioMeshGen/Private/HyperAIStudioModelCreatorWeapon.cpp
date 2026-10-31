// Games by Hyper 2026.

#include "HyperAIStudioModelCreator.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HyperAIStudioTripoSettings.h"
#include "ImageCore.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"

/**
 * Weapons as hand-made game weapons are: generated weapons come out inflated (a blade nearly square in section, a guard
 * like a brick, a fat grip) and lying any way. A weapon is measured section by section along its length, then turned onto
 * Y with its head toward -Y, its pivot put in the grip, scaled to its real length and thinned to real proportions, the flat
 * of its blade across X. A reference mesh the user names is measured the same way and becomes the project's convention.
 * Ported from the Editor Python prototype proven on Tripo's sword (31 October 2026).
 */
namespace HyperAIStudio::MeshGen::Creator
{
	using namespace UE::Geometry;

	namespace WeaponDetail
	{
		/** The two axes across a length along Axis, in order. */
		void AcrossAxes(const int32 Axis, int32& OutU, int32& OutV)
		{
			OutU = Axis == 0 ? 1 : 0;
			OutV = Axis == 2 ? 1 : 2;
		}

		/**
		 * A weapon cut into sections along its length, the handle's end toward +T (T = Sign x the coordinate on Axis): each
		 * section's extent and centre across (U, V), the guard (the widest, and its neighbours over half its span), and the
		 * sections beyond it either way.
		 */
		struct FCut
		{
			int32 Axis = 1;
			double Sign = 1.0;
			int32 U = 0;
			int32 V = 2;
			double Lo = 0.0;
			double Hi = 0.0;
			double Step = 1.0;
			int32 Count = 0;
			TArray<FVector2d> Min;
			TArray<FVector2d> Max;
			int32 GuardLo = 0;
			int32 GuardHi = 0;
			/** Toward -T from the guard (a blade, or an axe's spike), and toward +T (the handle), non-empty sections only. */
			TArray<int32> Blade;
			TArray<int32> Handle;
			bool bHeaded = false;
			/** Across which of U (0) or V (1) the blade, or the head, is thin. */
			int32 Thin = 1;

			bool Has(const int32 Bin) const { return Max[Bin].X >= Min[Bin].X; }
			FVector2d Extent(const int32 Bin) const { return Has(Bin) ? Max[Bin] - Min[Bin] : FVector2d::Zero(); }
			FVector2d Centre(const int32 Bin) const { return Has(Bin) ? (Max[Bin] + Min[Bin]) * 0.5 : FVector2d::Zero(); }
			double Span(const int32 Bin) const { const FVector2d E = Extent(Bin); return FMath::Max(E.X, E.Y); }
			int32 BinOf(const double T) const { return FMath::Clamp(int32((T - Lo) / Step), 0, Count - 1); }
			double T(const FVector3d& P) const { return Sign * P[Axis]; }
		};

		/** The sections of Points along Axis with the handle toward +T; Bins of them over the length. */
		void Section(const TArray<FVector3d>& Points, FCut& Into, const int32 Bins)
		{
			Into.Lo = TNumericLimits<double>::Max();
			Into.Hi = TNumericLimits<double>::Lowest();
			for (const FVector3d& P : Points)
			{
				Into.Lo = FMath::Min(Into.Lo, Into.T(P));
				Into.Hi = FMath::Max(Into.Hi, Into.T(P));
			}
			Into.Count = FMath::Max(Bins, 1);
			Into.Step = FMath::Max((Into.Hi - Into.Lo) / Into.Count, 1e-9);
			Into.Min.Init(FVector2d(TNumericLimits<double>::Max()), Into.Count);
			Into.Max.Init(FVector2d(TNumericLimits<double>::Lowest()), Into.Count);
			for (const FVector3d& P : Points)
			{
				const int32 Bin = Into.BinOf(Into.T(P));
				const FVector2d Across(P[Into.U], P[Into.V]);
				Into.Min[Bin] = FVector2d::Min(Into.Min[Bin], Across);
				Into.Max[Bin] = FVector2d::Max(Into.Max[Bin], Across);
			}
		}

		/**
		 * Which axis is the length, which end the handle is on, and the sections a unit long. The widest of 64 coarse
		 * sections is the guard, or a head: the handle is the shorter side of a guard (a blade is longer than a grip), but
		 * the longer side of a head at one end (an axe's haft, a spear's shaft), when the shorter side is under 15% long.
		 */
		bool Cut(const TArray<FVector3d>& Points, FCut& Out)
		{
			if (Points.Num() < 8)
			{
				return false;
			}
			FBox3d Box(ForceInit);
			for (const FVector3d& P : Points)
			{
				Box += P;
			}
			const FVector3d Size = Box.GetSize();
			Out.Axis = Size.X >= Size.Y && Size.X >= Size.Z ? 0 : Size.Y >= Size.Z ? 1 : 2;
			AcrossAxes(Out.Axis, Out.U, Out.V);
			Out.Sign = 1.0;
			FCut Coarse = Out;
			Section(Points, Coarse, 64);
			int32 Widest = 0;
			for (int32 Bin = 0; Bin < Coarse.Count; ++Bin)
			{
				Widest = Coarse.Span(Bin) > Coarse.Span(Widest) ? Bin : Widest;
			}
			const int32 Below = Widest;
			const int32 Above = Coarse.Count - 1 - Widest;
			const bool bShortAbove = Above < Below;
			const int32 Shorter = FMath::Min(Below, Above);
			Out.bHeaded = Shorter < Coarse.Count * 0.15;
			// The handle's side, on the coarse cut's own +axis or not.
			const bool bHandleAbove = Out.bHeaded ? !bShortAbove : bShortAbove;
			Out.Sign = bHandleAbove ? 1.0 : -1.0;
			const double Length = Size[Out.Axis];
			Section(Points, Out, FMath::Clamp(int32(Length) + 1, 16, 4096));
			int32 Guard = 0;
			for (int32 Bin = 0; Bin < Out.Count; ++Bin)
			{
				Guard = Out.Span(Bin) > Out.Span(Guard) ? Bin : Guard;
			}
			Out.GuardLo = Guard;
			while (Out.GuardLo > 0 && Out.Span(Out.GuardLo - 1) > 0.5 * Out.Span(Guard))
			{
				--Out.GuardLo;
			}
			Out.GuardHi = Guard;
			while (Out.GuardHi < Out.Count - 1 && Out.Span(Out.GuardHi + 1) > 0.5 * Out.Span(Guard))
			{
				++Out.GuardHi;
			}
			for (int32 Bin = 0; Bin < Out.GuardLo; ++Bin)
			{
				if (Out.Has(Bin) && Out.Span(Bin) > 0.0)
				{
					Out.Blade.Add(Bin);
				}
			}
			for (int32 Bin = Out.GuardHi + 1; Bin < Out.Count; ++Bin)
			{
				if (Out.Has(Bin) && Out.Span(Bin) > 0.0)
				{
					Out.Handle.Add(Bin);
				}
			}
			// The blade's thin side; a headed weapon's head's.
			TArray<int32> Flat = Out.bHeaded ? TArray<int32>() : Out.Blade;
			if (Flat.IsEmpty())
			{
				for (int32 Bin = Out.GuardLo; Bin <= Out.GuardHi; ++Bin)
				{
					Flat.Add(Bin);
				}
			}
			double SumU = 0.0, SumV = 0.0;
			for (const int32 Bin : Flat)
			{
				SumU += Out.Extent(Bin).X;
				SumV += Out.Extent(Bin).Y;
			}
			Out.Thin = SumU < SumV ? 0 : 1;
			return Length > KINDA_SMALL_NUMBER && !Out.Handle.IsEmpty();
		}

		double Median(TArray<double> Values)
		{
			if (Values.IsEmpty())
			{
				return 0.0;
			}
			Values.Sort();
			return Values[Values.Num() / 2];
		}

		/** The grip: the handle's sections nearest the guard (70% of them); the rest is the pommel or a haft's butt. */
		int32 GripCount(const FCut& Cut)
		{
			return FMath::Max(1, int32(Cut.Handle.Num() * 0.7));
		}

		/** A coordinate mapping that turns Axis onto Y with Sign's end toward +Y (a proper rotation). */
		FVector3d OntoY(const FVector3d& P, const int32 Axis, const double Sign)
		{
			if (Axis == 0)
			{
				return Sign > 0.0 ? FVector3d(-P.Y, P.X, P.Z) : FVector3d(P.Y, -P.X, P.Z);
			}
			if (Axis == 2)
			{
				return Sign > 0.0 ? FVector3d(P.X, P.Z, -P.Y) : FVector3d(P.X, -P.Z, P.Y);
			}
			return Sign > 0.0 ? P : FVector3d(-P.X, -P.Y, P.Z);
		}

		TArray<FVector3d> PointsOf(const FDynamicMesh3& Mesh)
		{
			TArray<FVector3d> Points;
			Points.Reserve(Mesh.VertexCount());
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				Points.Add(Mesh.GetVertex(Vid));
			}
			return Points;
		}

		/** Each texture's role by its name and setup: colour, normal, or a packed mask. */
		enum class ETextureKind : uint8 { Color, Normal, Mask };

		ETextureKind KindOf(const UTexture2D& Texture)
		{
			const FString Name = Texture.GetName().ToLower();
			if (Texture.CompressionSettings == TC_Normalmap || Name.Contains(TEXT("normal")) || Name.EndsWith(TEXT("_n")))
			{
				return ETextureKind::Normal;
			}
			return Texture.SRGB && !Name.EndsWith(TEXT("_orm")) && !Name.Contains(TEXT("mask")) ? ETextureKind::Color : ETextureKind::Mask;
		}

		/**
		 * A packed mask's channels read from their values: occlusion is mostly near 1, metal mostly 0 or 1, roughness the
		 * one left. Letters R G B: O occlusion, R roughness, M metal (ORM is glTF's and HyperAI's).
		 */
		FString PackingOf(UTexture2D& Texture)
		{
			FImage Source, Small;
			if (!Texture.Source.IsValid() || !Texture.Source.GetMipImage(Source, 0))
			{
				return FString();
			}
			Source.GammaSpace = EGammaSpace::Linear;
			Source.ResizeTo(Small, 64, 64, ERawImageFormat::BGRA8, EGammaSpace::Linear);
			double Mean[3] = {0.0, 0.0, 0.0};
			double Binary[3] = {0.0, 0.0, 0.0};
			const int32 Pixels = 64 * 64;
			for (int32 Index = 0; Index < Pixels; ++Index)
			{
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					// BGRA bytes: channel 0 (red) is byte 2.
					const double Value = Small.RawData[Index * 4 + 2 - Channel] / 255.0;
					Mean[Channel] += Value / Pixels;
					Binary[Channel] += (Value < 0.1 || Value > 0.9) ? 1.0 / Pixels : 0.0;
				}
			}
			TCHAR Letters[3] = {TEXT('R'), TEXT('R'), TEXT('R')};
			int32 Occlusion = 0;
			for (int32 Channel = 1; Channel < 3; ++Channel)
			{
				Occlusion = Mean[Channel] > Mean[Occlusion] ? Channel : Occlusion;
			}
			int32 Metal = Occlusion == 0 ? 1 : 0;
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				if (Channel != Occlusion && Binary[Channel] > Binary[Metal])
				{
					Metal = Channel;
				}
			}
			Letters[Occlusion] = TEXT('O');
			Letters[Metal] = TEXT('M');
			return FString::Printf(TEXT("%c%c%c"), Letters[0], Letters[1], Letters[2]);
		}

		const TCHAR* AxisName(const int32 Axis)
		{
			return Axis == 0 ? TEXT("X") : Axis == 1 ? TEXT("Y") : TEXT("Z");
		}
	}

	using namespace WeaponDetail;

	FWeaponShape MeasureWeapon(const TArray<FVector3d>& Points)
	{
		FWeaponShape Shape;
		FCut Sections;
		if (!WeaponDetail::Cut(Points, Sections))
		{
			return Shape;
		}
		Shape.bValid = true;
		Shape.Axis = Sections.Axis;
		Shape.bHandlePositive = Sections.Sign > 0.0;
		Shape.bHeaded = Sections.bHeaded;
		Shape.Length = static_cast<float>(Sections.Hi - Sections.Lo);
		int32 U = 0, V = 0;
		AcrossAxes(Sections.Axis, U, V);
		Shape.ThicknessAxis = Sections.Thin == 0 ? U : V;
		const TArray<int32>& Flat = Sections.bHeaded || Sections.Blade.IsEmpty() ? TArray<int32>() : Sections.Blade;
		TArray<double> Widths, Thicknesses, Ratios, Grips;
		auto Measure = [&](const int32 Bin)
		{
			const FVector2d Extent = Sections.Extent(Bin);
			const double Thick = Sections.Thin == 0 ? Extent.X : Extent.Y;
			const double Wide = Sections.Thin == 0 ? Extent.Y : Extent.X;
			Widths.Add(Wide);
			Thicknesses.Add(Thick);
			Ratios.Add(Wide > KINDA_SMALL_NUMBER ? Thick / Wide : 0.0);
		};
		if (Flat.IsEmpty())
		{
			for (int32 Bin = Sections.GuardLo; Bin <= Sections.GuardHi; ++Bin)
			{
				Measure(Bin);
			}
		}
		else
		{
			for (const int32 Bin : Flat)
			{
				Measure(Bin);
			}
		}
		Shape.BladeWidth = static_cast<float>(Median(Widths));
		Shape.BladeThickness = static_cast<float>(Median(Thicknesses));
		Shape.BladeRatio = static_cast<float>(Median(Ratios));
		for (int32 Index = 0; Index < GripCount(Sections) && Index < Sections.Handle.Num(); ++Index)
		{
			Grips.Add(Sections.Span(Sections.Handle[Index]));
		}
		Shape.GripCm = static_cast<float>(Median(Grips));
		for (int32 Bin = Sections.GuardLo; Bin <= Sections.GuardHi; ++Bin)
		{
			const FVector2d Extent = Sections.Extent(Bin);
			Shape.GuardDepthCm = FMath::Max(Shape.GuardDepthCm, static_cast<float>(Sections.Thin == 0 ? Extent.X : Extent.Y));
			Shape.GuardSpanCm = FMath::Max(Shape.GuardSpanCm, static_cast<float>(Sections.Span(Bin)));
		}
		if (Sections.bHeaded)
		{
			// A head is not a guard: its depth is the head's own business.
			Shape.GuardDepthCm = 0.f;
		}
		// The origin along the length, from the handle's end; in the grip when it lies between the guard and the handle's end
		// and inside the grip's cross-section.
		Shape.PivotFraction = static_cast<float>((Sections.Hi - 0.0) / FMath::Max(Sections.Hi - Sections.Lo, 1e-9));
		const double GuardTop = Sections.Lo + (Sections.GuardHi + 1) * Sections.Step;
		const int32 Bin = Sections.BinOf(0.0);
		const FVector2d Centre = Sections.Centre(Bin);
		const double Reach = FMath::Max(Sections.Span(Bin) * 0.5, 0.5);
		Shape.bPivotInGrip = 0.0 >= GuardTop && 0.0 <= Sections.Hi && FMath::Abs(Centre.X) <= Reach && FMath::Abs(Centre.Y) <= Reach;
		return Shape;
	}

	bool ShapeWeapon(FDynamicMesh3& Mesh, const FWeaponFinish& Finish, TArray<FString>& OutNotes)
	{
		FCut Before;
		if (!WeaponDetail::Cut(PointsOf(Mesh), Before))
		{
			OutNotes.Add(TEXT("weapon_finish: it does not read as a weapon (no handle beside its widest section); left as it was."));
			return false;
		}
		// The grip's middle, between the guard's edge and the handle's end, centred across.
		const double GuardTop = Before.Lo + (Before.GuardHi + 1) * Before.Step;
		const double GripT = (GuardTop + Before.Hi) * 0.5;
		const FVector2d GripCentre = Before.Centre(Before.BinOf(GripT));
		FVector3d Origin = FVector3d::Zero();
		Origin[Before.Axis] = GripT * Before.Sign;
		Origin[Before.U] = GripCentre.X;
		Origin[Before.V] = GripCentre.Y;
		const double Length = Before.Hi - Before.Lo;
		const double Scale = Finish.LengthCm > 0.f ? Finish.LengthCm / Length : 1.0;
		for (const int32 Vid : Mesh.VertexIndicesItr())
		{
			Mesh.SetVertex(Vid, OntoY(Mesh.GetVertex(Vid) - Origin, Before.Axis, Before.Sign) * Scale);
		}
		OutNotes.Add(FString::Printf(TEXT("weapon_finish: its length (%s) turned onto Y, the %s toward -Y and the handle toward +Y, the grip's middle at the origin; scaled by %.3f to %.0f cm."),
			AxisName(Before.Axis), Before.bHeaded ? TEXT("head") : TEXT("blade"), Scale, Length * Scale));

		// Thin section by section, a centimetre at a time, each axis measured. Blade: thickness to BladeRatio of its width
		// (0.3-1.2 cm), width kept. Guard: depth to GuardDepthCm, span kept. Grip: GripCm across; the pommel by the square
		// root of the grip's factor, so it stays round. A head (axe, hammer) is left as it is.
		FCut Along;
		Along.Axis = 1;
		Along.Sign = 1.0;
		AcrossAxes(1, Along.U, Along.V);
		const TArray<FVector3d> Points = PointsOf(Mesh);
		WeaponDetail::Cut(Points, Along);
		if (Along.Axis != 1 || Along.Sign < 0.0)
		{
			// Thinned work found it lying another way: measured as it now is, along Y, handle at +Y.
			Along = FCut();
			Along.Axis = 1;
			Along.Sign = 1.0;
			AcrossAxes(1, Along.U, Along.V);
			Section(Points, Along, FMath::Clamp(int32(Length * Scale) + 1, 16, 4096));
		}
		const int32 N = Along.Count;
		TArray<FVector2d> Factor;
		Factor.Init(FVector2d(1.0), N);
		const int32 Thin = Along.Thin;
		const int32 Wide = 1 - Thin;
		double BladeBefore = 0.0, BladeAfter = 0.0;
		if (!Along.bHeaded)
		{
			for (const int32 Bin : Along.Blade)
			{
				const FVector2d Extent = Along.Extent(Bin);
				const double Target = FMath::Clamp(Extent[Wide] * Finish.BladeRatio, 0.3, 1.2);
				BladeBefore = FMath::Max(BladeBefore, Extent[Thin]);
				if (Extent[Thin] > Target)
				{
					Factor[Bin][Thin] = Target / Extent[Thin];
				}
				BladeAfter = FMath::Max(BladeAfter, FMath::Min(Extent[Thin], Target));
			}
			for (int32 Bin = Along.GuardLo; Bin <= Along.GuardHi; ++Bin)
			{
				const double Depth = Along.Extent(Bin)[Thin];
				if (Depth > Finish.GuardDepthCm)
				{
					Factor[Bin][Thin] = Finish.GuardDepthCm / Depth;
				}
			}
		}
		TArray<double> GripSpans;
		const int32 Grip = GripCount(Along);
		for (int32 Index = 0; Index < Grip && Index < Along.Handle.Num(); ++Index)
		{
			GripSpans.Add(Along.Span(Along.Handle[Index]));
		}
		const double GripBefore = Median(GripSpans);
		const double GripFactor = GripBefore > 0.0 ? FMath::Min(1.0, Finish.GripCm / GripBefore) : 1.0;
		for (int32 Index = 0; Index < Along.Handle.Num(); ++Index)
		{
			const double Each = Index < Grip ? GripFactor : FMath::Sqrt(GripFactor);
			Factor[Along.Handle[Index]] = FVector2d(Each, Each);
		}
		// Which part each section is (0 blade or spike, 1 guard or head, 2 handle; -1 empty): smoothing and blending stay
		// within a part, so a guard is held to its depth right to its edges and a grip to its width.
		TArray<int32> Part;
		Part.Init(INDEX_NONE, N);
		for (int32 Bin = 0; Bin < N; ++Bin)
		{
			Part[Bin] = !Along.Has(Bin) ? INDEX_NONE : Bin < Along.GuardLo ? 0 : Bin <= Along.GuardHi ? 1 : 2;
		}
		// A three-section moving average within the part, so its sections change without a step; never above a section's
		// own target, so a blade or guard is never left thicker than it should be.
		TArray<FVector2d> Smooth;
		Smooth.SetNum(N);
		for (int32 Bin = 0; Bin < N; ++Bin)
		{
			FVector2d Sum = FVector2d::Zero();
			int32 Count = 0;
			for (int32 Near = FMath::Max(0, Bin - 1); Near <= FMath::Min(N - 1, Bin + 1); ++Near)
			{
				if (Part[Near] == Part[Bin])
				{
					Sum += Factor[Near];
					++Count;
				}
			}
			Smooth[Bin] = FVector2d::Min(Sum / double(FMath::Max(Count, 1)), Factor[Bin]);
		}
		for (const int32 Vid : Mesh.VertexIndicesItr())
		{
			const FVector3d P = Mesh.GetVertex(Vid);
			const double At = (P.Y - Along.Lo) / Along.Step - 0.5;
			const int32 B0 = FMath::Clamp(FMath::FloorToInt32(At), 0, N - 1);
			const int32 B1 = FMath::Min(N - 1, B0 + 1);
			double Blend = FMath::Clamp(At - B0, 0.0, 1.0);
			if (Part[B0] != Part[B1])
			{
				// Across a part's edge: the vertex takes its own section's factor alone.
				Blend = Along.BinOf(P.Y) == B1 ? 1.0 : 0.0;
			}
			const FVector2d F = Smooth[B0] * (1.0 - Blend) + Smooth[B1] * Blend;
			const FVector2d C = Along.Centre(B0) * (1.0 - Blend) + Along.Centre(B1) * Blend;
			Mesh.SetVertex(Vid, FVector3d(C.X + (P.X - C.X) * F.X, P.Y, C.Y + (P.Z - C.Y) * F.Y));
		}
		if (!Along.bHeaded)
		{
			OutNotes.Add(FString::Printf(TEXT("weapon_finish: blade thinned from %.1f to %.1f cm thick (%.2f of its width at most), guard to %.1f cm deep, grip from %.1f to %.1f cm across, the pommel kept round."),
				BladeBefore, BladeAfter, Finish.BladeRatio, Finish.GuardDepthCm, GripBefore, GripBefore * GripFactor));
		}
		else
		{
			OutNotes.Add(FString::Printf(TEXT("weapon_finish: a headed weapon; its head kept, its haft from %.1f to %.1f cm across."), GripBefore, GripBefore * GripFactor));
		}
		// The flat across X, the thickness along Z: a quarter turn about Y when the thickness lies on X.
		if (Thin == 0)
		{
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				const FVector3d P = Mesh.GetVertex(Vid);
				Mesh.SetVertex(Vid, FVector3d(-P.Z, P.Y, P.X));
			}
			OutNotes.Add(TEXT("weapon_finish: turned about its length, the flat across X and the thickness along Z."));
		}
		if (Finish.PivotFraction > 0.f)
		{
			// The convention's pivot: that far down the length from the handle's end.
			double Top = TNumericLimits<double>::Lowest(), Bottom = TNumericLimits<double>::Max();
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				Top = FMath::Max(Top, Mesh.GetVertex(Vid).Y);
				Bottom = FMath::Min(Bottom, Mesh.GetVertex(Vid).Y);
			}
			const double Shift = Top - Finish.PivotFraction * (Top - Bottom);
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				Mesh.SetVertex(Vid, Mesh.GetVertex(Vid) - FVector3d(0.0, Shift, 0.0));
			}
			OutNotes.Add(FString::Printf(TEXT("weapon_finish: the pivot %.0f%% of the length from the handle's end, as %s."), Finish.PivotFraction * 100.f, *Finish.Source));
		}
		if (Mesh.HasAttributes() && Mesh.Attributes()->PrimaryNormals())
		{
			FMeshNormals::QuickRecomputeOverlayNormals(Mesh);
		}
		return true;
	}

	FWeaponFinish WeaponFinishFor(const float SizeMeters, const int32 Triangles)
	{
		FWeaponFinish Finish;
		const FHyperAIStudioWeaponConvention& Convention = GetDefault<UHyperAIStudioTripoSettings>()->WeaponConvention;
		if (!Convention.Source.IsEmpty())
		{
			Finish.TargetTris = Convention.Triangles > 0 ? Convention.Triangles : Finish.TargetTris;
			Finish.bNanite = Convention.bNanite;
			Finish.PivotFraction = Convention.PivotFraction;
			Finish.BladeRatio = Convention.BladeRatio > 0.f ? Convention.BladeRatio : Finish.BladeRatio;
			Finish.GripCm = Convention.GripCm > 0.f ? Convention.GripCm : Finish.GripCm;
			Finish.GuardDepthCm = Convention.GuardDepthCm > 0.f ? Convention.GuardDepthCm : Finish.GuardDepthCm;
			Finish.ColorSize = Convention.ColorSize > 0 ? Convention.ColorSize : Finish.ColorSize;
			Finish.NormalSize = Convention.NormalSize > 0 ? Convention.NormalSize : Finish.NormalSize;
			Finish.MaskSize = Convention.MaskSize > 0 ? Convention.MaskSize : Finish.MaskSize;
			Finish.Source = FString::Printf(TEXT("the project's weapon convention (from %s)"), *Convention.Source);
		}
		else
		{
			Finish.Source = TEXT("HyperAI's weapon defaults");
		}
		Finish.LengthCm = SizeMeters * 100.f;
		if (Triangles > 0)
		{
			Finish.TargetTris = Triangles;
		}
		return Finish;
	}

	bool MatchReference(const FString& AssetPath, FWeaponFinish& Out, TArray<FString>& OutReasons, FString& OutError, const bool bKeep)
	{
		UStaticMesh* Reference = LoadObject<UStaticMesh>(nullptr, *AssetPath.TrimStartAndEnd(), nullptr, LOAD_NoWarn | LOAD_Quiet);
		const FMeshDescription* Source = Reference && Reference->IsMeshDescriptionValid(0) ? Reference->GetMeshDescription(0) : nullptr;
		if (!Source)
		{
			OutError = TEXT("match_asset is a static mesh in the project with a source mesh, such as /Game/Weapons/SM_Axe.SM_Axe.");
			return false;
		}
		Out = FWeaponFinish();
		Out.Source = FString::Printf(TEXT("the reference %s"), *Reference->GetName());
		FStaticMeshConstAttributes Attributes(*Source);
		const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TArray<FVector3d> Points;
		Points.Reserve(Source->Vertices().Num());
		for (const FVertexID Vertex : Source->Vertices().GetElementIDs())
		{
			Points.Add(FVector3d(Positions[Vertex]));
		}
		const FWeaponShape Shape = MeasureWeapon(Points);
		const int32 Triangles = Source->Triangles().Num();
		const bool bNanite = Reference->GetNaniteSettings().bEnabled;
		const int32 UVs = Attributes.GetVertexInstanceUVs().GetNumChannels();
		const UBodySetup* Body = Reference->GetBodySetup();
		const bool bCollision = Body && (Body->AggGeom.GetElementCount() > 0 || Body->CollisionTraceFlag == CTF_UseComplexAsSimple);
		Out.TargetTris = Triangles;
		Out.bNanite = bNanite;
		OutReasons.Add(FString::Printf(TEXT("match %s: %d triangles (the budget), Nanite %s, %d LOD(s), %d UV channel(s), %d material slot(s), %s"), *Reference->GetName(), Triangles,
			bNanite ? TEXT("on") : TEXT("off"), Reference->GetNumSourceModels(), UVs, Reference->GetStaticMaterials().Num(), bCollision ? TEXT("collision") : TEXT("no collision")));
		if (Shape.bValid)
		{
			// Its length, for a weapon whose size was not given.
			Out.LengthCm = Shape.Length;
			const TCHAR* End = Shape.bHandlePositive ? TEXT("-") : TEXT("+");
			OutReasons.Add(FString::Printf(TEXT("match: its length lies on %s, %.0f cm, the %s toward %s%s%s"), AxisName(Shape.Axis), Shape.Length, Shape.bHeaded ? TEXT("head") : TEXT("blade"), End,
				AxisName(Shape.Axis), Shape.Axis == 1 && Shape.bHandlePositive ? TEXT(", as HyperAI finishes weapons") : TEXT("; HyperAI finishes weapons on Y with the head toward -Y")));
			if (Shape.PivotFraction > 0.f && Shape.PivotFraction < 1.f)
			{
				Out.PivotFraction = Shape.PivotFraction;
				OutReasons.Add(FString::Printf(TEXT("match: the pivot %.0f cm from the handle's end (%.0f%% of its length)%s"), Shape.PivotFraction * Shape.Length, Shape.PivotFraction * 100.f,
					Shape.bPivotInGrip ? TEXT(", in the grip") : TEXT("")));
			}
			OutReasons.Add(FString::Printf(TEXT("match: the %s flat across %s, %.1f cm thick by %.1f cm wide (%.2f)"), Shape.bHeaded ? TEXT("head") : TEXT("blade"),
				AxisName(Shape.ThicknessAxis == 2 ? 0 : 2), Shape.BladeThickness, Shape.BladeWidth, Shape.BladeRatio));
			if (!Shape.bHeaded && Shape.BladeRatio > 0.02f && Shape.BladeRatio < 0.3f)
			{
				Out.BladeRatio = Shape.BladeRatio;
			}
			if (Shape.GripCm >= 2.f && Shape.GripCm <= 6.f)
			{
				Out.GripCm = Shape.GripCm;
				OutReasons.Add(FString::Printf(TEXT("match: the grip %.1f cm across"), Shape.GripCm));
			}
			if (!Shape.bHeaded && Shape.GuardDepthCm >= 1.f && Shape.GuardDepthCm <= 6.f)
			{
				Out.GuardDepthCm = Shape.GuardDepthCm;
				OutReasons.Add(FString::Printf(TEXT("match: the guard %.1f cm deep"), Shape.GuardDepthCm));
			}
		}
		else
		{
			OutReasons.Add(TEXT("match: its shape does not read as a weapon (no handle beside its widest section); only its counts and textures are followed"));
		}
		// Texture sizes and the mask's packing, from what its materials use.
		FString Packing;
		bool bAnyTexture = false;
		for (const FStaticMaterial& Slot : Reference->GetStaticMaterials())
		{
			TArray<UTexture*> Used;
			if (Slot.MaterialInterface)
			{
				Slot.MaterialInterface->GetUsedTextures(Used);
			}
			for (UTexture* Each : Used)
			{
				UTexture2D* Texture = Cast<UTexture2D>(Each);
				if (!Texture || Texture->GetPathName().StartsWith(TEXT("/Engine/")))
				{
					continue;
				}
				bAnyTexture = true;
				int32 Size = FMath::Max(Texture->Source.GetSizeX(), Texture->Source.GetSizeY());
				Size = Texture->MaxTextureSize > 0 ? FMath::Min(Size, Texture->MaxTextureSize) : Size;
				switch (KindOf(*Texture))
				{
				case ETextureKind::Color: Out.ColorSize = Size; break;
				case ETextureKind::Normal: Out.NormalSize = Size; break;
				default: Out.MaskSize = Size; Packing = PackingOf(*Texture); break;
				}
			}
		}
		OutReasons.Add(bAnyTexture ? FString::Printf(TEXT("match: textures colour %d, normal %d, mask %d%s; HyperAI's master reads ORM, so its mask stays ORM"), Out.ColorSize, Out.NormalSize,
				Out.MaskSize, Packing.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (the reference packs %s)"), *Packing))
			: FString::Printf(TEXT("match: no textures to read on its material slots; textures capped at colour %d, normal %d, mask %d, the mask ORM with HyperAI's master"), Out.ColorSize,
				Out.NormalSize, Out.MaskSize));
		if (bKeep)
		{
			UHyperAIStudioTripoSettings* Settings = GetMutableDefault<UHyperAIStudioTripoSettings>();
			FHyperAIStudioWeaponConvention& Convention = Settings->WeaponConvention;
			Convention.Source = Reference->GetPathName();
			Convention.Triangles = Out.TargetTris;
			Convention.bNanite = Out.bNanite;
			Convention.PivotFraction = Out.PivotFraction;
			Convention.BladeRatio = Out.BladeRatio;
			Convention.GripCm = Out.GripCm;
			Convention.GuardDepthCm = Out.GuardDepthCm;
			Convention.ColorSize = Out.ColorSize;
			Convention.NormalSize = Out.NormalSize;
			Convention.MaskSize = Out.MaskSize;
			Convention.MaskPacking = Packing;
			Convention.Summary = FString::Printf(TEXT("weapons follow %s: long axis Y, head toward -Y, pivot %s, Nanite %s, %d triangles, colour/normal %d/%d, mask %d%s"),
				*Reference->GetName(), Out.PivotFraction > 0.f ? *FString::Printf(TEXT("%.0f%% from the handle's end"), Out.PivotFraction * 100.f) : TEXT("in the grip"),
				Out.bNanite ? TEXT("on") : TEXT("off"), Out.TargetTris, Out.ColorSize, Out.NormalSize, Out.MaskSize, Packing.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (packed %s)"), *Packing));
			Settings->SaveConfig();
			OutReasons.Add(TEXT("kept as this project's weapon convention (Editor Preferences > Plugins > HyperAIStudio Tripo): ") + Convention.Summary);
		}
		return true;
	}
}
