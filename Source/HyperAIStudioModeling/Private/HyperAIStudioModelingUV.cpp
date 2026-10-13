// Games by Hyper 2026.

#include "HyperAIStudioModelingUV.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMeshToMeshDescription.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioResultPreview.h"
#include "HyperAIStudioSourceControl.h"
#include "ImageUtils.h"
#include "MeshConversionOptions.h"
#include "MeshDescription.h"
#include "MeshDescriptionBuilder.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "MeshUtilitiesCommon.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "OverlappingCorners.h"
#include "Parameterization/MeshUVPacking.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace HyperAIStudio::Modeling::UV
{
	using namespace UE::Geometry;

	namespace
	{
		/** UV coordinates closer than this are the same UV: a split edge keeps both sides' values exactly, half-precision render data included. */
		constexpr double SameUV = 1e-5;
		/** A triangle with less UV area than this has none (a thousandth of a texel at 1024 on a side). */
		constexpr double NoUVArea = 1e-12;
		/** Triangles with less 3D area than this (cm squared) are left out: nothing renders them. */
		constexpr double No3DArea = 1e-8;
		/** UVs this far past 0 or 1 are outside. */
		constexpr double OutsideSlack = 1e-4;

		/** Twice the signed area of A, B, C. */
		double Cross(const FVector2d& A, const FVector2d& B, const FVector2d& C)
		{
			return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
		}

		bool Less(const FVector2d& A, const FVector2d& B)
		{
			return A.X < B.X || (A.X == B.X && A.Y < B.Y);
		}

		/** The edge function of A->B at P, worked out the same way whichever way the edge is walked: two triangles sharing it see exactly opposite values. */
		double Edge(const FVector2d& A, const FVector2d& B, const FVector2d& P)
		{
			return Less(A, B) ? Cross(A, B, P) : -Cross(B, A, P);
		}

		/** Whether a point exactly on edge A->B of a counter-clockwise triangle is inside it: the rule a tiny nudge along +x (then +y) decides, so shared edges and corners have one owner. */
		bool OwnsEdge(const FVector2d& A, const FVector2d& B)
		{
			const FVector2d D = B - A;
			return D.Y < 0.0 || (D.Y == 0.0 && D.X > 0.0);
		}

		/** Calls Visit(X, Y) for each pixel whose center lies in the triangle (points in pixel units). */
		template <typename FunctionType>
		void Rasterize(FVector2d A, FVector2d B, FVector2d C, const int32 Width, const int32 Height, FunctionType&& Visit)
		{
			const double Area = Cross(A, B, C);
			if (Area == 0.0 || !FMath::IsFinite(Area)) return;
			if (Area < 0.0) Swap(B, C);
			const int32 X0 = FMath::Max(0, FMath::FloorToInt32(FMath::Min3(A.X, B.X, C.X) - 0.5));
			const int32 X1 = FMath::Min(Width - 1, FMath::CeilToInt32(FMath::Max3(A.X, B.X, C.X) - 0.5));
			const int32 Y0 = FMath::Max(0, FMath::FloorToInt32(FMath::Min3(A.Y, B.Y, C.Y) - 0.5));
			const int32 Y1 = FMath::Min(Height - 1, FMath::CeilToInt32(FMath::Max3(A.Y, B.Y, C.Y) - 0.5));
			const bool bAB = OwnsEdge(A, B);
			const bool bBC = OwnsEdge(B, C);
			const bool bCA = OwnsEdge(C, A);
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					const FVector2d P(X + 0.5, Y + 0.5);
					const double E0 = Edge(A, B, P);
					if (E0 < 0.0 || (E0 == 0.0 && !bAB)) continue;
					const double E1 = Edge(B, C, P);
					if (E1 < 0.0 || (E1 == 0.0 && !bBC)) continue;
					const double E2 = Edge(C, A, P);
					if (E2 < 0.0 || (E2 == 0.0 && !bCA)) continue;
					Visit(X, Y);
				}
			}
		}

		struct FUnionFind
		{
			TArray<int32> Parent;
			explicit FUnionFind(const int32 Num)
			{
				Parent.SetNumUninitialized(Num);
				for (int32 Index = 0; Index < Num; ++Index) Parent[Index] = Index;
			}
			int32 Find(int32 Index)
			{
				while (Parent[Index] != Index)
				{
					Parent[Index] = Parent[Parent[Index]];
					Index = Parent[Index];
				}
				return Index;
			}
			void Union(const int32 A, const int32 B)
			{
				const int32 RootA = Find(A);
				const int32 RootB = Find(B);
				if (RootA != RootB) Parent[FMath::Max(RootA, RootB)] = FMath::Min(RootA, RootB);
			}
		};

		uint64 PairKey(const int32 A, const int32 B)
		{
			return (uint64(uint32(FMath::Min(A, B))) << 32) | uint64(uint32(FMath::Max(A, B)));
		}

		TArray<FIslandPair> SortedPairs(const TMap<uint64, int64>& Counts)
		{
			TArray<FIslandPair> Out;
			for (const TPair<uint64, int64>& Pair : Counts) Out.Add({int32(Pair.Key >> 32), int32(Pair.Key & 0xffffffffull), Pair.Value});
			Out.Sort([](const FIslandPair& L, const FIslandPair& R) { return L.Pixels != R.Pixels ? L.Pixels > R.Pixels : PairKey(L.A, L.B) < PairKey(R.A, R.B); });
			return Out;
		}

		FString PairText(const FIslandPair& Pair)
		{
			return Pair.A == Pair.B ? FString::Printf(TEXT("%d itself"), Pair.A) : FString::Printf(TEXT("%d+%d"), Pair.A, Pair.B);
		}

		FString Pairs(const TArray<FIslandPair>& All, const int32 Max)
		{
			TArray<FString> Lines;
			for (int32 Index = 0; Index < All.Num() && Index < Max; ++Index) Lines.Add(PairText(All[Index]));
			if (All.Num() > Max) Lines.Add(FString::Printf(TEXT("%d more"), All.Num() - Max));
			return FString::Join(Lines, TEXT(", "));
		}

		FString UVText(const FVector2d& Value)
		{
			return FString::Printf(TEXT("%.3f,%.3f"), Value.X, Value.Y);
		}

		/** Area-weighted percentile of sorted values. */
		double Percentile(const TArray<TPair<double, double>>& Sorted, const double Total, const double Share)
		{
			if (Sorted.IsEmpty() || Total <= 0.0) return 0.0;
			const double Wanted = Total * Share;
			double Sum = 0.0;
			for (const TPair<double, double>& Entry : Sorted)
			{
				Sum += Entry.Value;
				if (Sum >= Wanted) return Entry.Key;
			}
			return Sorted.Last().Key;
		}

		FString ObjectPathOf(const FString& Reference)
		{
			const FString Trimmed = Reference.TrimStartAndEnd();
			return Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
		}

		bool IsUnderProject(const FString& PackageName, FString& OutFile)
		{
			FString Filename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, FPackageName::GetAssetPackageExtension())) return false;
			OutFile = FPaths::ConvertRelativePathToFull(Filename);
			return FPaths::IsUnderDirectory(OutFile, FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		}

		bool IsEnginePath(const FString& PackageName)
		{
			return PackageName.StartsWith(TEXT("/Engine/")) || PackageName.StartsWith(TEXT("/Script/")) || PackageName.StartsWith(TEXT("/Temp/"))
				|| PackageName.StartsWith(TEXT("/Memory/"));
		}
	}

	void FSoup::Add(const FVector3d& A, const FVector3d& B, const FVector3d& C, const FVector2d& UA, const FVector2d& UB, const FVector2d& UC, const bool bHasUV)
	{
		Positions.Add(A);
		Positions.Add(B);
		Positions.Add(C);
		UVs.Add(UA);
		UVs.Add(UB);
		UVs.Add(UC);
		HasUV.Add(bHasUV);
	}

	FMetrics Analyze(const FSoup& Soup, const FOptions& Options)
	{
		FMetrics Out;
		const int32 Num = Soup.Num();
		Out.IslandOf.Init(0, Num);
		Out.Flags.Init(FMetrics::FlagNone, Num);
		Out.Border.Init(0, Num);
		TArray<double> AreaUV;
		TArray<double> Area3D;
		AreaUV.SetNumZeroed(Num);
		Area3D.SetNumZeroed(Num);
		// Triangles measured: 3D area, and UVs to read. Ones without UVs count as degenerate.
		TArray<bool> Measured;
		Measured.Init(false, Num);
		for (int32 Tri = 0; Tri < Num; ++Tri)
		{
			const FVector3d& P0 = Soup.Positions[Tri * 3];
			Area3D[Tri] = 0.5 * FVector3d::CrossProduct(Soup.Positions[Tri * 3 + 1] - P0, Soup.Positions[Tri * 3 + 2] - P0).Length();
			if (Area3D[Tri] <= No3DArea)
			{
				Out.Flags[Tri] |= FMetrics::FlagSkipped;
				continue;
			}
			++Out.Triangles;
			if (!Soup.HasUV[Tri])
			{
				Out.Flags[Tri] |= FMetrics::FlagDegenerate;
				++Out.Degenerate;
				continue;
			}
			Measured[Tri] = true;
			AreaUV[Tri] = 0.5 * Cross(Soup.UVs[Tri * 3], Soup.UVs[Tri * 3 + 1], Soup.UVs[Tri * 3 + 2]);
			Out.UVArea += FMath::Abs(AreaUV[Tri]);
			if (FMath::Abs(AreaUV[Tri]) <= NoUVArea)
			{
				Out.Flags[Tri] |= FMetrics::FlagDegenerate;
				++Out.Degenerate;
			}
		}

		// Corners welded by position: split vertices (seams, hard edges, the built mesh's copies) meet again.
		TArray<int32> Vertex;
		Vertex.SetNumUninitialized(Num * 3);
		{
			TMap<FVector3d, int32> Welded;
			Welded.Reserve(Num);
			for (int32 Corner = 0; Corner < Num * 3; ++Corner)
			{
				Vertex[Corner] = Welded.FindOrAdd(Soup.Positions[Corner], Welded.Num());
			}
		}
		TMap<uint64, TArray<int32, TInlineAllocator<2>>> Edges;
		Edges.Reserve(Num * 2);
		for (int32 Tri = 0; Tri < Num; ++Tri)
		{
			if (!Measured[Tri]) continue;
			for (int32 Side = 0; Side < 3; ++Side)
			{
				const int32 A = Vertex[Tri * 3 + Side];
				const int32 B = Vertex[Tri * 3 + (Side + 1) % 3];
				if (A != B) Edges.FindOrAdd(PairKey(A, B)).Add(Tri * 3 + Side);
			}
		}
		// Islands: triangles joined across edges whose UVs agree at both ends. Seams: edges where they do not.
		FUnionFind Islands(Num);
		auto SameAt = [&Soup, &Vertex](const int32 EdgeA, const int32 EdgeB)
		{
			const int32 TriA = EdgeA / 3, SideA = EdgeA % 3, TriB = EdgeB / 3, SideB = EdgeB % 3;
			const int32 A0 = TriA * 3 + SideA, A1 = TriA * 3 + (SideA + 1) % 3;
			const int32 B0 = TriB * 3 + SideB, B1 = TriB * 3 + (SideB + 1) % 3;
			const bool bSameWay = Vertex[A0] == Vertex[B0];
			const FVector2d& UA0 = Soup.UVs[A0];
			const FVector2d& UA1 = Soup.UVs[A1];
			const FVector2d& UB0 = Soup.UVs[bSameWay ? B0 : B1];
			const FVector2d& UB1 = Soup.UVs[bSameWay ? B1 : B0];
			return FVector2d::DistSquared(UA0, UB0) <= SameUV * SameUV && FVector2d::DistSquared(UA1, UB1) <= SameUV * SameUV;
		};
		TArray<bool> Joined;
		Joined.Init(false, Num * 3);
		for (const TPair<uint64, TArray<int32, TInlineAllocator<2>>>& Entry : Edges)
		{
			const TArray<int32, TInlineAllocator<2>>& Sides = Entry.Value;
			bool bSeam = false;
			for (int32 I = 0; I < Sides.Num(); ++I)
			{
				for (int32 J = I + 1; J < Sides.Num(); ++J)
				{
					if (SameAt(Sides[I], Sides[J]))
					{
						Islands.Union(Sides[I] / 3, Sides[J] / 3);
						Joined[Sides[I]] = true;
						Joined[Sides[J]] = true;
					}
					else
					{
						bSeam = true;
					}
				}
			}
			if (bSeam)
			{
				const int32 Side = Sides[0];
				Out.SeamLength += FVector3d::Distance(Soup.Positions[Side], Soup.Positions[(Side / 3) * 3 + (Side % 3 + 1) % 3]);
			}
		}
		TMap<int32, int32> Numbered;
		for (int32 Tri = 0; Tri < Num; ++Tri)
		{
			if (!Measured[Tri]) continue;
			Out.IslandOf[Tri] = Numbered.FindOrAdd(Islands.Find(Tri), Numbered.Num() + 1);
			for (int32 Side = 0; Side < 3; ++Side)
			{
				if (!Joined[Tri * 3 + Side]) Out.Border[Tri] |= uint8(1 << Side);
			}
		}
		Out.Islands = Numbered.Num();

		// Flipped: wound against the rest of the island. A whole island mirrored is fine.
		TArray<double> IslandArea;
		IslandArea.SetNumZeroed(Out.Islands + 1);
		for (int32 Tri = 0; Tri < Num; ++Tri) IslandArea[Out.IslandOf[Tri]] += Measured[Tri] ? AreaUV[Tri] : 0.0;
		int32 Outside = 0;
		int32 WithUVs = 0;
		TArray<TPair<double, double>> Density;
		double DensityWeight = 0.0;
		for (int32 Tri = 0; Tri < Num; ++Tri)
		{
			if (!Measured[Tri]) continue;
			++WithUVs;
			const double Sign = IslandArea[Out.IslandOf[Tri]] >= 0.0 ? 1.0 : -1.0;
			if (AreaUV[Tri] * Sign < -NoUVArea)
			{
				Out.Flags[Tri] |= FMetrics::FlagFlipped;
				++Out.Flipped;
			}
			bool bOutside = false;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector2d& Point = Soup.UVs[Tri * 3 + Corner];
				Out.Bounds += Point;
				bOutside |= Point.X < -OutsideSlack || Point.Y < -OutsideSlack || Point.X > 1.0 + OutsideSlack || Point.Y > 1.0 + OutsideSlack;
			}
			if (bOutside)
			{
				Out.Flags[Tri] |= FMetrics::FlagOutside;
				++Outside;
			}
			if (FMath::Abs(AreaUV[Tri]) > NoUVArea)
			{
				// Texels per meter: the UV side the triangle gets, in texels, over its 3D side in meters.
				const double Texels = Options.TextureSize * FMath::Sqrt(FMath::Abs(AreaUV[Tri]) / Area3D[Tri]) * 100.0;
				Density.Add({Texels, Area3D[Tri]});
				DensityWeight += Area3D[Tri];
			}
		}
		Out.OutsidePercent = WithUVs > 0 ? 100.0 * Outside / WithUVs : 0.0;
		Density.Sort([](const TPair<double, double>& L, const TPair<double, double>& R) { return L.Key < R.Key; });
		Out.DensityMin = Percentile(Density, DensityWeight, 0.01);
		Out.DensityMedian = Percentile(Density, DensityWeight, 0.5);
		Out.DensityMax = Percentile(Density, DensityWeight, 0.99);
		Out.DensitySpread = Out.DensityMin > 0.0 ? Out.DensityMax / Out.DensityMin : 0.0;

		// Overlaps, counted in pixels over the bounds of the layout and the 0-1 square.
		if (WithUVs > 0)
		{
			FBox2d Domain = Out.Bounds;
			Domain += FVector2d(0.0, 0.0);
			Domain += FVector2d(1.0, 1.0);
			const FVector2d Extent = Domain.GetSize();
			const double Scale = FMath::Max(1, Options.RasterSize) / FMath::Max(Extent.X, Extent.Y);
			const int32 Width = FMath::Clamp(FMath::CeilToInt32(Extent.X * Scale), 1, Options.RasterSize);
			const int32 Height = FMath::Clamp(FMath::CeilToInt32(Extent.Y * Scale), 1, Options.RasterSize);
			TArray<int32> Owner;
			TArray<int32> OwnerTri;
			TArray<uint8> Count;
			Owner.SetNumZeroed(Width * Height);
			OwnerTri.SetNumZeroed(Width * Height);
			Count.SetNumZeroed(Width * Height);
			TMap<uint64, int64> Overlaps;
			int64 Overlapped = 0;
			for (int32 Tri = 0; Tri < Num; ++Tri)
			{
				if (!Measured[Tri] || (Out.Flags[Tri] & FMetrics::FlagDegenerate)) continue;
				const int32 Island = Out.IslandOf[Tri];
				auto Pixel = [&Domain, Scale](const FVector2d& Point) { return (Point - Domain.Min) * Scale; };
				Rasterize(Pixel(Soup.UVs[Tri * 3]), Pixel(Soup.UVs[Tri * 3 + 1]), Pixel(Soup.UVs[Tri * 3 + 2]), Width, Height, [&](const int32 X, const int32 Y)
				{
					const int32 Index = Y * Width + X;
					if (Owner[Index] == 0)
					{
						Owner[Index] = Island;
						OwnerTri[Index] = Tri;
						Count[Index] = 1;
						return;
					}
					if (Count[Index] == 1)
					{
						++Overlapped;
						Out.Flags[OwnerTri[Index]] |= FMetrics::FlagOverlapping;
					}
					Count[Index] = uint8(FMath::Min(255, Count[Index] + 1));
					Out.Flags[Tri] |= FMetrics::FlagOverlapping;
					Overlaps.FindOrAdd(PairKey(Owner[Index], Island)) += 1;
				});
			}
			int64 Covered = 0;
			int64 InUnit = 0;
			int64 UnitPixels = 0;
			for (int32 Y = 0; Y < Height; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					const FVector2d Center = Domain.Min + FVector2d(X + 0.5, Y + 0.5) / Scale;
					const bool bUnit = Center.X >= 0.0 && Center.X <= 1.0 && Center.Y >= 0.0 && Center.Y <= 1.0;
					UnitPixels += bUnit ? 1 : 0;
					if (Owner[Y * Width + X] == 0) continue;
					++Covered;
					InUnit += bUnit ? 1 : 0;
				}
			}
			Out.OverlapPercent = Covered > 0 ? 100.0 * Overlapped / Covered : 0.0;
			Out.CoveragePercent = UnitPixels > 0 ? 100.0 * InUnit / UnitPixels : 0.0;
			Out.Overlaps = SortedPairs(Overlaps);
		}
		return Out;
	}

	FLightmapMetrics AnalyzeLightmap(const FSoup& Soup, const FMetrics& Metrics, const int32 Resolution, const int32 Padding)
	{
		FLightmapMetrics Out;
		Out.Resolution = FMath::Clamp(Resolution, 4, 4096);
		Out.OutsidePercent = Metrics.OutsidePercent;
		const int32 Texels = Out.Resolution;
		// Four samples a side per texel, and every corner's texel, so a sliver still claims the texels it touches.
		constexpr int32 Sub = 4;
		const int32 Size = Texels * Sub;
		TArray<int32> Owner;
		Owner.SetNumZeroed(Texels * Texels);
		TMap<uint64, int64> Near;
		int64 Shared = 0;
		auto Claim = [&Owner, &Near, &Shared, Texels](const int32 X, const int32 Y, const int32 Island)
		{
			int32& Current = Owner[Y * Texels + X];
			if (Current == 0)
			{
				Current = Island;
			}
			else if (Current != Island)
			{
				if (Current > 0) ++Shared;
				Near.FindOrAdd(PairKey(FMath::Abs(Current), Island)) += 1;
				Current = -FMath::Abs(Current);
			}
		};
		for (int32 Tri = 0; Tri < Soup.Num(); ++Tri)
		{
			const int32 Island = Metrics.IslandOf.IsValidIndex(Tri) ? Metrics.IslandOf[Tri] : 0;
			if (Island == 0 || (Metrics.Flags[Tri] & FMetrics::FlagDegenerate)) continue;
			auto Pixel = [Size](const FVector2d& Point) { return Point * double(Size); };
			Rasterize(Pixel(Soup.UVs[Tri * 3]), Pixel(Soup.UVs[Tri * 3 + 1]), Pixel(Soup.UVs[Tri * 3 + 2]), Size, Size, [&Claim, Island](const int32 X, const int32 Y)
			{
				Claim(X / Sub, Y / Sub, Island);
			});
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector2d& Point = Soup.UVs[Tri * 3 + Corner];
				if (Point.X < 0.0 || Point.Y < 0.0 || Point.X > 1.0 || Point.Y > 1.0) continue;
				Claim(FMath::Min(Texels - 1, int32(Point.X * Texels)), FMath::Min(Texels - 1, int32(Point.Y * Texels)), Island);
			}
		}
		int64 Covered = 0;
		for (const int32 Value : Owner) Covered += Value != 0 ? 1 : 0;
		Out.SharedPercent = Covered > 0 ? 100.0 * Shared / Covered : 0.0;
		// Islands closer than Padding empty texels: a texel of one has a texel of another within Padding.
		if (Padding > 0)
		{
			for (int32 Y = 0; Y < Texels; ++Y)
			{
				for (int32 X = 0; X < Texels; ++X)
				{
					const int32 Here = FMath::Abs(Owner[Y * Texels + X]);
					if (Here == 0) continue;
					for (int32 DY = -Padding; DY <= Padding; ++DY)
					{
						for (int32 DX = -Padding; DX <= Padding; ++DX)
						{
							const int32 NX = X + DX, NY = Y + DY;
							if ((DX == 0 && DY == 0) || NX < 0 || NY < 0 || NX >= Texels || NY >= Texels) continue;
							const int32 There = FMath::Abs(Owner[NY * Texels + NX]);
							if (There != 0 && There != Here && Here < There) Near.FindOrAdd(PairKey(Here, There)) += 1;
						}
					}
				}
			}
		}
		Out.TooClose = SortedPairs(Near);
		const bool bFolded = Metrics.Overlaps.ContainsByPredicate([](const FIslandPair& Pair) { return Pair.A == Pair.B; });
		Out.bSuitable = Metrics.Triangles > 0 && Shared == 0 && Out.TooClose.IsEmpty() && Out.OutsidePercent <= 0.0 && !bFolded && Metrics.Degenerate < Metrics.Triangles;
		return Out;
	}

	void Judge(const FMetrics& Metrics, FString& OutVerdict, TArray<FString>& OutProblems)
	{
		TArray<FString> Errors;
		TArray<FString> Warnings;
		const int32 Measured = FMath::Max(1, Metrics.Triangles);
		if (Metrics.Triangles == 0)
		{
			Errors.Add(TEXT("error no_triangles: the LOD has no triangles with area to lay out."));
		}
		else if (Metrics.UVArea <= NoUVArea * Measured)
		{
			Errors.Add(TEXT("error no_uv_area: every triangle sits on one point or line in UV space; the channel was never unwrapped (hyper_model_uv_apply Method=xatlas)."));
		}
		if (Metrics.OverlapPercent > 0.5)
		{
			Errors.Add(FString::Printf(TEXT("error overlap: %.1f%% of the UV area is covered more than once (islands %s): bakes and lightmaps come out wrong there; fine only for mirrored or tiling textures."),
				Metrics.OverlapPercent, *Pairs(Metrics.Overlaps, 6)));
		}
		else if (Metrics.OverlapPercent > 0.01)
		{
			Warnings.Add(FString::Printf(TEXT("warning overlap: %.2f%% of the UV area is covered more than once (islands %s)."), Metrics.OverlapPercent, *Pairs(Metrics.Overlaps, 6)));
		}
		if (Metrics.Flipped > FMath::Max(1, Measured / 200))
		{
			Errors.Add(FString::Printf(TEXT("error flipped: %d triangles are folded over in UV space (wound against their island); textures smear and normal maps light them backwards."), Metrics.Flipped));
		}
		else if (Metrics.Flipped > 0)
		{
			Warnings.Add(FString::Printf(TEXT("warning flipped: %d triangle(s) folded over in UV space."), Metrics.Flipped));
		}
		if (Metrics.Degenerate * 20 > Measured)
		{
			Errors.Add(FString::Printf(TEXT("error degenerate: %d of %d triangles have no UV area; they show one texel stretched."), Metrics.Degenerate, Metrics.Triangles));
		}
		else if (Metrics.Degenerate > 0)
		{
			Warnings.Add(FString::Printf(TEXT("warning degenerate: %d triangle(s) have no UV area."), Metrics.Degenerate));
		}
		if (Metrics.OutsidePercent > 0.0)
		{
			Warnings.Add(FString::Printf(TEXT("warning outside_unit: %.1f%% of the triangles reach outside 0-1 (UVs %s to %s): fine for tiling textures, not for bakes or lightmaps."),
				Metrics.OutsidePercent, *UVText(Metrics.Bounds.Min), *UVText(Metrics.Bounds.Max)));
		}
		if (Metrics.DensitySpread > 4.0)
		{
			Warnings.Add(FString::Printf(TEXT("warning texel_density: %.0f to %.0f texels per meter (x%.1f): parts look blurry beside others."), Metrics.DensityMin, Metrics.DensityMax,
				Metrics.DensitySpread));
		}
		if (Metrics.Triangles >= 30 && Metrics.Islands * 3 > Metrics.Triangles)
		{
			Warnings.Add(FString::Printf(TEXT("warning fragmented: %d islands for %d triangles; seams show and mips bleed (xatlas or patch makes fewer)."), Metrics.Islands, Metrics.Triangles));
		}
		if (Metrics.OutsidePercent <= 0.0 && Metrics.CoveragePercent > 0.0 && Metrics.CoveragePercent < 30.0)
		{
			Warnings.Add(FString::Printf(TEXT("warning low_coverage: the layout covers %.0f%% of the 0-1 square; repack it for sharper textures."), Metrics.CoveragePercent));
		}
		OutVerdict = !Errors.IsEmpty() ? TEXT("bad") : !Warnings.IsEmpty() ? TEXT("usable") : TEXT("good");
		OutProblems = MoveTemp(Errors);
		OutProblems.Append(Warnings);
	}

	int32 VerdictRank(const FString& Verdict)
	{
		return Verdict == TEXT("missing") ? 3 : Verdict == TEXT("bad") ? 2 : Verdict == TEXT("usable") ? 1 : 0;
	}

	void ToStruct(const FMetrics& Metrics, const int32 Channel, FHyperAIModelUVMetrics& Out)
	{
		Out.Channel = Channel;
		Out.Triangles = Metrics.Triangles;
		Out.Islands = Metrics.Islands;
		Out.SeamLengthCm = float(Metrics.SeamLength);
		Out.OverlapPercent = float(Metrics.OverlapPercent);
		Out.OverlappingIslands.Reset();
		for (int32 Index = 0; Index < Metrics.Overlaps.Num() && Index < 12; ++Index) Out.OverlappingIslands.Add(PairText(Metrics.Overlaps[Index]));
		Out.FlippedTriangles = Metrics.Flipped;
		Out.DegenerateTriangles = Metrics.Degenerate;
		Out.OutsideUnitPercent = float(Metrics.OutsidePercent);
		Out.UVMin = Metrics.Bounds.bIsValid ? UVText(Metrics.Bounds.Min) : FString();
		Out.UVMax = Metrics.Bounds.bIsValid ? UVText(Metrics.Bounds.Max) : FString();
		Out.CoveragePercent = float(Metrics.CoveragePercent);
		Out.TexelDensityMin = float(Metrics.DensityMin);
		Out.TexelDensityMedian = float(Metrics.DensityMedian);
		Out.TexelDensityMax = float(Metrics.DensityMax);
		Out.TexelDensitySpread = float(Metrics.DensitySpread);
	}

	void ToStruct(const FLightmapMetrics& Metrics, const int32 Channel, const bool bPresent, const bool bGenerated, FHyperAIModelLightmapCheck& Out)
	{
		Out.Channel = Channel;
		Out.Resolution = Metrics.Resolution;
		Out.bGeneratedAtBuild = bGenerated;
		Out.bPresent = bPresent;
		Out.SharedTexelsPercent = float(Metrics.SharedPercent);
		Out.TooClosePairs = Metrics.TooClose.Num();
		Out.TooClose.Reset();
		for (int32 Index = 0; Index < Metrics.TooClose.Num() && Index < 12; ++Index) Out.TooClose.Add(PairText(Metrics.TooClose[Index]));
		Out.OutsideUnitPercent = float(Metrics.OutsidePercent);
		Out.bSuitable = bPresent && Metrics.bSuitable;
		if (!bPresent)
		{
			Out.Summary = FString::Printf(TEXT("Lightmap channel %d is missing: the mesh cannot be lit by baked lighting (hyper_model_uv_apply Method=lightmap)."), Channel);
		}
		else if (Out.bSuitable)
		{
			Out.Summary = FString::Printf(TEXT("Lightmap channel %d suits a %d px lightmap: no shared texels, nothing outside 0-1%s."), Channel, Metrics.Resolution,
				bGenerated ? TEXT(" (made when the mesh builds)") : TEXT(""));
		}
		else
		{
			Out.Summary = FString::Printf(TEXT("Lightmap channel %d does not suit a %d px lightmap: %.1f%% of its texels are shared, %d island pair(s) too close%s, %.1f%% of triangles outside 0-1."),
				Channel, Metrics.Resolution, Metrics.SharedPercent, Metrics.TooClose.Num(), Metrics.TooClose.IsEmpty() ? TEXT("") : *(TEXT(" (") + Pairs(Metrics.TooClose, 4) + TEXT(")")),
				Metrics.OutsidePercent);
		}
	}

	FString Describe(const FString& Verdict, const FMetrics& Metrics)
	{
		return FString::Printf(TEXT("%s: %d islands, %.1f%% overlap, %d flipped, %d degenerate, %.1f%% outside 0-1"), *Verdict, Metrics.Islands, Metrics.OverlapPercent, Metrics.Flipped,
			Metrics.Degenerate, Metrics.OutsidePercent);
	}

	bool DrawLayout(const FSoup& Soup, const FMetrics& Metrics, const FString& Path, FString& OutError, const int32 Size)
	{
		// The 0-1 square, and as much of the layout around it as fits from -1 to 2.
		FBox2d Domain(FVector2d(0.0, 0.0), FVector2d(1.0, 1.0));
		if (Metrics.Bounds.bIsValid)
		{
			Domain += FVector2d(FMath::Clamp(Metrics.Bounds.Min.X, -1.0, 2.0), FMath::Clamp(Metrics.Bounds.Min.Y, -1.0, 2.0));
			Domain += FVector2d(FMath::Clamp(Metrics.Bounds.Max.X, -1.0, 2.0), FMath::Clamp(Metrics.Bounds.Max.Y, -1.0, 2.0));
		}
		const double Side = FMath::Max(Domain.GetSize().X, Domain.GetSize().Y) * 1.04;
		const FVector2d Origin = Domain.GetCenter() - FVector2d(Side * 0.5);
		const double Scale = Size / Side;
		auto Pixel = [&Origin, Scale](const FVector2d& Point) { return (Point - Origin) * Scale; };
		TArray<FColor> Pixels;
		Pixels.Init(FColor(22, 22, 24), Size * Size);
		// V runs up the picture: row 0 of the image is the top.
		auto At = [&Pixels, Size](const int32 X, const int32 Y) -> FColor& { return Pixels[(Size - 1 - Y) * Size + X]; };
		const FVector2d UnitMin = Pixel(FVector2d(0.0, 0.0));
		const FVector2d UnitMax = Pixel(FVector2d(1.0, 1.0));
		for (int32 Y = FMath::Max(0, int32(UnitMin.Y)); Y < FMath::Min(Size, int32(UnitMax.Y)); ++Y)
		{
			for (int32 X = FMath::Max(0, int32(UnitMin.X)); X < FMath::Min(Size, int32(UnitMax.X)); ++X) At(X, Y) = FColor(40, 40, 44);
		}
		TArray<uint8> Count;
		Count.SetNumZeroed(Size * Size);
		for (int32 Tri = 0; Tri < Soup.Num(); ++Tri)
		{
			const int32 Island = Metrics.IslandOf.IsValidIndex(Tri) ? Metrics.IslandOf[Tri] : 0;
			if (Island == 0 || (Metrics.Flags[Tri] & FMetrics::FlagDegenerate)) continue;
			const bool bFlipped = (Metrics.Flags[Tri] & FMetrics::FlagFlipped) != 0;
			const FColor Fill = bFlipped ? FColor(230, 60, 230) : FLinearColor::MakeFromHSV8(uint8((Island * 97) % 256), 110, 150).ToFColor(true);
			Rasterize(Pixel(Soup.UVs[Tri * 3]), Pixel(Soup.UVs[Tri * 3 + 1]), Pixel(Soup.UVs[Tri * 3 + 2]), Size, Size, [&](const int32 X, const int32 Y)
			{
				uint8& Times = Count[Y * Size + X];
				Times = uint8(FMath::Min(255, Times + 1));
				At(X, Y) = Times > 1 ? FColor(235, 40, 40) : Fill;
			});
		}
		auto Line = [&At, Size](FVector2d A, FVector2d B, const FColor Colour)
		{
			const int32 Steps = FMath::Clamp(FMath::CeilToInt32(FVector2d::Distance(A, B)), 1, Size * 4);
			for (int32 Step = 0; Step <= Steps; ++Step)
			{
				const FVector2d P = FMath::Lerp(A, B, double(Step) / Steps);
				const int32 X = FMath::FloorToInt32(P.X), Y = FMath::FloorToInt32(P.Y);
				if (X >= 0 && Y >= 0 && X < Size && Y < Size) At(X, Y) = Colour;
			}
		};
		// Each island's outline, so islands that touch still read apart.
		for (int32 Tri = 0; Tri < Soup.Num(); ++Tri)
		{
			if (!Metrics.Border.IsValidIndex(Tri) || Metrics.Border[Tri] == 0 || Metrics.IslandOf[Tri] == 0) continue;
			for (int32 From = 0; From < 3; ++From)
			{
				if (Metrics.Border[Tri] & (1 << From)) Line(Pixel(Soup.UVs[Tri * 3 + From]), Pixel(Soup.UVs[Tri * 3 + (From + 1) % 3]), FColor(215, 215, 215));
			}
		}
		for (int32 Tri = 0; Tri < Soup.Num(); ++Tri)
		{
			if (!(Metrics.Flags[Tri] & FMetrics::FlagDegenerate) || !Soup.HasUV[Tri]) continue;
			const FVector2d Center = Pixel((Soup.UVs[Tri * 3] + Soup.UVs[Tri * 3 + 1] + Soup.UVs[Tri * 3 + 2]) / 3.0);
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					const int32 X = int32(Center.X) + DX, Y = int32(Center.Y) + DY;
					if (X >= 0 && Y >= 0 && X < Size && Y < Size) At(X, Y) = FColor(250, 220, 40);
				}
			}
		}
		const FColor Frame(150, 150, 160);
		Line(UnitMin, FVector2d(UnitMax.X, UnitMin.Y), Frame);
		Line(FVector2d(UnitMax.X, UnitMin.Y), UnitMax, Frame);
		Line(UnitMax, FVector2d(UnitMin.X, UnitMax.Y), Frame);
		Line(FVector2d(UnitMin.X, UnitMax.Y), UnitMin, Frame);
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size, Size, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		if (Png.IsEmpty() || !FFileHelper::SaveArrayToFile(Png, *Path))
		{
			OutError = TEXT("the layout PNG could not be written");
			return false;
		}
		return true;
	}

	FString LayoutPathOf(const FString& AssetPath, const int32 Lod, const int32 Channel, const bool bDryRun)
	{
		return FHyperAIStudioResultPreview::GetAssetPreviewPath(ObjectPathOf(AssetPath) + FString::Printf(TEXT(" uv%d %s%s"), Channel,
			Lod < 0 ? TEXT("hires") : *FString::Printf(TEXT("lod%d"), Lod), bDryRun ? TEXT(" dry run") : TEXT("")));
	}

	bool SoupFromDescription(const FMeshDescription& Mesh, const int32 Channel, const FVector& Scale, FSoup& Out)
	{
		Out = FSoup();
		FStaticMeshConstAttributes Attributes(Mesh);
		const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
		const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		if (!UVs.IsValid() || Channel < 0 || Channel >= UVs.GetNumChannels()) return false;
		Out.Positions.Reserve(Mesh.Triangles().Num() * 3);
		Out.UVs.Reserve(Mesh.Triangles().Num() * 3);
		for (const FTriangleID Triangle : Mesh.Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexInstanceID> Corners = Mesh.GetTriangleVertexInstances(Triangle);
			FVector3d P[3];
			FVector2d U[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				P[Corner] = FVector3d(Positions[Mesh.GetVertexInstanceVertex(Corners[Corner])]) * Scale;
				U[Corner] = FVector2d(UVs.Get(Corners[Corner], Channel));
			}
			Out.Add(P[0], P[1], P[2], U[0], U[1], U[2]);
		}
		return true;
	}

	bool SoupFromDynamic(const FDynamicMesh3& Mesh, const int32 Layer, FSoup& Out)
	{
		Out = FSoup();
		const FDynamicMeshUVOverlay* Overlay = Mesh.HasAttributes() && Layer >= 0 && Layer < Mesh.Attributes()->NumUVLayers() ? Mesh.Attributes()->GetUVLayer(Layer) : nullptr;
		if (!Overlay) return false;
		for (const int32 Tri : Mesh.TriangleIndicesItr())
		{
			const FIndex3i Vertices = Mesh.GetTriangle(Tri);
			FVector2f A = FVector2f::ZeroVector, B = FVector2f::ZeroVector, C = FVector2f::ZeroVector;
			const bool bSet = Overlay->IsSetTriangle(Tri);
			if (bSet) Overlay->GetTriElements(Tri, A, B, C);
			Out.Add(Mesh.GetVertex(Vertices.A), Mesh.GetVertex(Vertices.B), Mesh.GetVertex(Vertices.C), FVector2d(A), FVector2d(B), FVector2d(C), bSet);
		}
		return true;
	}

	bool SoupFromRenderData(UStaticMesh& Mesh, const int32 Lod, const int32 Channel, FSoup& Out, FString& OutError)
	{
		Out = FSoup();
		TArray<UStaticMesh*> Compile = {&Mesh};
		FStaticMeshCompilingManager::Get().FinishCompilation(Compile);
		const FStaticMeshRenderData* Render = Mesh.GetRenderData();
		if (!Render || !Render->LODResources.IsValidIndex(Lod))
		{
			OutError = FString::Printf(TEXT("the built mesh has no LOD %d"), Lod);
			return false;
		}
		const FStaticMeshLODResources& Resources = Render->LODResources[Lod];
		const FPositionVertexBuffer& Positions = Resources.VertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& Vertices = Resources.VertexBuffers.StaticMeshVertexBuffer;
		if (!Positions.GetVertexData() || !Vertices.GetTexCoordData() || Positions.GetNumVertices() == 0)
		{
			OutError = TEXT("the built mesh keeps no vertex data in memory to read");
			return false;
		}
		if (Channel < 0 || Channel >= int32(Vertices.GetNumTexCoords())) return false;
		TArray<uint32> Indices;
		Resources.IndexBuffer.GetCopy(Indices);
		for (int32 Index = 0; Index + 2 < Indices.Num(); Index += 3)
		{
			FVector3d P[3];
			FVector2d U[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 Vertex = Indices[Index + Corner];
				if (Vertex >= Positions.GetNumVertices())
				{
					OutError = TEXT("the built mesh's indices run past its vertices");
					return false;
				}
				P[Corner] = FVector3d(Positions.VertexPosition(Vertex));
				U[Corner] = FVector2d(Vertices.GetVertexUV(Vertex, Channel));
			}
			Out.Add(P[0], P[1], P[2], U[0], U[1], U[2]);
		}
		return true;
	}

	const FMeshDescription* SourceOf(const UStaticMesh& Mesh, const int32 Lod)
	{
		if (Lod < 0) return Mesh.IsHiResMeshDescriptionValid() ? Mesh.GetHiResMeshDescription() : nullptr;
		if (Lod >= Mesh.GetNumSourceModels() || !Mesh.IsMeshDescriptionValid(Lod) || (Lod > 0 && Mesh.IsReductionActive(Lod))) return nullptr;
		return Mesh.GetMeshDescription(Lod);
	}

	int32 ChannelsOf(const UStaticMesh& Mesh, const int32 Lod)
	{
		const FMeshDescription* Source = SourceOf(Mesh, Lod);
		if (!Source) return 0;
		FStaticMeshConstAttributes Attributes(*Source);
		return Attributes.GetVertexInstanceUVs().IsValid() ? Attributes.GetVertexInstanceUVs().GetNumChannels() : 0;
	}

	FVector BuildScaleOf(const UStaticMesh& Mesh, const int32 Lod)
	{
		if (Lod < 0) return Mesh.GetHiResSourceModel().BuildSettings.BuildScale3D;
		return Lod < Mesh.GetNumSourceModels() ? Mesh.GetSourceModel(Lod).BuildSettings.BuildScale3D : FVector::OneVector;
	}

	bool Read(const UStaticMesh& Mesh, const int32 Lod, FDynamicMesh3& Out, FString& OutError)
	{
		const FMeshDescription* Source = SourceOf(Mesh, Lod);
		if (!Source)
		{
			OutError = Lod < 0 ? FString(TEXT("it has no hi-res source")) : FString::Printf(TEXT("LOD %d has no source mesh of its own (it is missing, or reduced from another LOD)"), Lod);
			return false;
		}
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(Source, Out, /*bCopyTangents=*/true);
		if (Out.TriangleCount() == 0)
		{
			OutError = FString::Printf(TEXT("LOD %d has no triangles"), Lod);
			return false;
		}
		return true;
	}

	bool Write(UStaticMesh& Mesh, const int32 Lod, const FDynamicMesh3& Edited, FString& OutError)
	{
		FMeshDescription* Target = Lod < 0 ? (Mesh.IsHiResMeshDescriptionValid() ? Mesh.GetHiResMeshDescription() : nullptr) : Mesh.GetMeshDescription(Lod);
		if (!Target)
		{
			OutError = FString::Printf(TEXT("LOD %d has no source mesh to write"), Lod);
			return false;
		}
		// The conversion makes the polygon groups again from the material ids, which Read set to the groups' ids: their
		// slot names come back here, so each section finds its material as before.
		TMap<int32, FName> SlotNames;
		{
			FStaticMeshConstAttributes Attributes(*Target);
			const TPolygonGroupAttributesConstRef<FName> Names = Attributes.GetPolygonGroupMaterialSlotNames();
			for (const FPolygonGroupID Group : Target->PolygonGroups().GetElementIDs()) SlotNames.Add(Group.GetValue(), Names.IsValid() ? Names[Group] : NAME_None);
		}
		FMeshDescription Out = *Target;
		FConversionToMeshDescriptionOptions Options;
		Options.bSetPolyGroups = Target->PolygonAttributes().HasAttribute(ExtendedMeshAttribute::PolyTriGroups);
		FDynamicMeshToMeshDescription Converter(Options);
		Converter.Convert(&Edited, Out, /*bCopyTangents=*/true);
		{
			FStaticMeshAttributes Attributes(Out);
			TPolygonGroupAttributesRef<FName> Names = Attributes.GetPolygonGroupMaterialSlotNames();
			if (Names.IsValid())
			{
				for (const FPolygonGroupID Group : Out.PolygonGroups().GetElementIDs())
				{
					if (const FName* Name = SlotNames.Find(Group.GetValue())) Names[Group] = *Name;
				}
			}
		}
		if (Out.Triangles().Num() != Edited.TriangleCount())
		{
			OutError = FString::Printf(TEXT("LOD %d came out with %d triangles, not %d"), Lod, Out.Triangles().Num(), Edited.TriangleCount());
			return false;
		}
		if (Lod < 0)
		{
			Mesh.ModifyHiResMeshDescription();
			*Target = MoveTemp(Out);
			Mesh.CommitHiResMeshDescription();
		}
		else
		{
			Mesh.ModifyMeshDescription(Lod);
			*Target = MoveTemp(Out);
			Mesh.CommitMeshDescription(Lod);
		}
		return true;
	}

	bool ParseMethod(const FString& Text, EMethod& Out)
	{
		const FString Name = Text.TrimStartAndEnd().ToLower();
		if (Name.IsEmpty() || Name == TEXT("xatlas")) Out = EMethod::XAtlas;
		else if (Name == TEXT("patch") || Name == TEXT("patchbuilder")) Out = EMethod::Patch;
		else if (Name == TEXT("box")) Out = EMethod::Box;
		else if (Name == TEXT("planar")) Out = EMethod::Planar;
		else if (Name == TEXT("cylinder")) Out = EMethod::Cylinder;
		else if (Name == TEXT("repack")) Out = EMethod::Repack;
		else if (Name == TEXT("lightmap")) Out = EMethod::Lightmap;
		else return false;
		return true;
	}

	const TCHAR* MethodName(const EMethod Method)
	{
		switch (Method)
		{
		case EMethod::Patch: return TEXT("patch");
		case EMethod::Box: return TEXT("box");
		case EMethod::Planar: return TEXT("planar");
		case EMethod::Cylinder: return TEXT("cylinder");
		case EMethod::Repack: return TEXT("repack");
		case EMethod::Lightmap: return TEXT("lightmap");
		default: return TEXT("xatlas");
		}
	}

	bool Pack(FDynamicMesh3& Mesh, const int32 Channel, const int32 Resolution, const int32 Padding, FString& OutError)
	{
		FDynamicMeshUVOverlay* Overlay = Mesh.HasAttributes() && Channel < Mesh.Attributes()->NumUVLayers() ? Mesh.Attributes()->GetUVLayer(Channel) : nullptr;
		if (!Overlay)
		{
			OutError = FString::Printf(TEXT("there is no UV channel %d to pack"), Channel);
			return false;
		}
		// Islands joined only at a corner are packed apart, as Epic's layout tool does.
		Overlay->SplitBowties();
		// Unreal's packer leaves one texel between islands at the resolution it packs for: packing for Resolution / Padding
		// leaves Padding texels at Resolution.
		FDynamicMeshUVPacker Packer(Overlay);
		Packer.TextureResolution = FMath::Max(16, Resolution / FMath::Max(1, Padding));
		Packer.GutterSize = 1.0f;
		Packer.bAllowFlips = false;
		if (!Packer.StandardPack())
		{
			OutError = FString::Printf(TEXT("Unreal's packer could not fit channel %d's islands into 0-1"), Channel);
			return false;
		}
		return true;
	}

	bool Unwrap(UDynamicMesh& Mesh, const FUnwrap& Settings, FString& OutError)
	{
		using UUV = UGeometryScriptLibrary_MeshUVFunctions;
		const TStrongObjectPtr<UGeometryScriptDebug> Debug(NewObject<UGeometryScriptDebug>(GetTransientPackage()));
		int32 Channels = 0;
		FBox Bounds(ForceInit);
		Mesh.ProcessMesh([&Channels, &Bounds](const FDynamicMesh3& Read)
		{
			Channels = Read.HasAttributes() ? Read.Attributes()->NumUVLayers() : 0;
			Bounds = FBox(Read.GetBounds(true));
		});
		if (Settings.Channel < 0 || Settings.Channel > Channels || Settings.Channel >= 8)
		{
			OutError = FString::Printf(TEXT("UV channel %d: the mesh has %d; name one of them, or %d to add one (at most 8)"), Settings.Channel, Channels, Channels);
			return false;
		}
		if (Settings.Method == EMethod::Repack && Settings.Channel == Channels)
		{
			OutError = FString::Printf(TEXT("repack packs channel %d's own islands, and the mesh has no such channel"), Settings.Channel);
			return false;
		}
		if (Settings.Channel == Channels) UUV::SetNumUVSets(&Mesh, Channels + 1, Debug.Get());
		// XAtlas works on a compact mesh; the triangles are written back whole, so their ids may move.
		Mesh.EditMesh([](FDynamicMesh3& Edit) { if (!Edit.IsCompact()) Edit.CompactInPlace(); }, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, true);
		const FVector Center = Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector;
		const double Across = Bounds.IsValid ? FMath::Max(1.0, Bounds.GetSize().GetMax()) : 100.0;
		// Packed layouts project at the mesh's size, then fit 0-1; tiling ones keep TileSize cm per tile.
		const double Tile = Settings.bRepack ? Across : FMath::Max(0.01, Settings.TileSize);
		Debug->Messages.Reset();
		bool bPack = true;
		switch (Settings.Method)
		{
		case EMethod::XAtlas:
			UUV::AutoGenerateXAtlasMeshUVs(&Mesh, Settings.Channel, FGeometryScriptXAtlasOptions(), Debug.Get());
			break;
		case EMethod::Patch:
		{
			FGeometryScriptPatchBuilderOptions Options;
			Options.bAutoPack = false;
			UUV::AutoGeneratePatchBuilderMeshUVs(&Mesh, Settings.Channel, Options, Debug.Get());
			break;
		}
		case EMethod::Box:
			UUV::SetMeshUVsFromBoxProjection(&Mesh, Settings.Channel, FTransform(FRotator::ZeroRotator, Center, FVector(Tile)), FGeometryScriptMeshSelection(), 2, Debug.Get());
			bPack = Settings.bRepack;
			break;
		case EMethod::Planar:
			UUV::SetMeshUVsFromPlanarProjection(&Mesh, Settings.Channel, FTransform(FRotator::ZeroRotator, Center, FVector(Tile)), FGeometryScriptMeshSelection(), Debug.Get());
			bPack = Settings.bRepack;
			break;
		case EMethod::Cylinder:
			UUV::SetMeshUVsFromCylinderProjection(&Mesh, Settings.Channel, FTransform(FRotator::ZeroRotator, Center, FVector(Tile)), FGeometryScriptMeshSelection(), 45.f, Debug.Get());
			bPack = Settings.bRepack;
			break;
		case EMethod::Repack:
			break;
		default:
			OutError = TEXT("lightmap UVs are made when the mesh builds, not unwrapped here");
			return false;
		}
		for (const FGeometryScriptDebugMessage& Message : Debug->Messages)
		{
			if (Message.MessageType == EGeometryScriptDebugMessageType::ErrorMessage)
			{
				OutError = FString::Printf(TEXT("%s failed: %s"), MethodName(Settings.Method), *Message.Message.ToString());
				return false;
			}
		}
		if (!bPack) return true;
		bool bPacked = true;
		Mesh.EditMesh([&](FDynamicMesh3& Edit) { bPacked = Pack(Edit, Settings.Channel, Settings.Resolution, Settings.Padding, OutError); }, EDynamicMeshChangeType::AttributeEdit,
			EDynamicMeshAttributeChangeFlags::UVs, true);
		return bPacked;
	}

	bool PreviewLightmap(const UStaticMesh& Asset, const FMeshDescription& Mesh, int32 SourceChannel, int32 Channel, const int32 Resolution, FSoup& Out, FString& OutError)
	{
		FMeshDescription Copy = Mesh;
		FStaticMeshAttributes Attributes(Copy);
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		const int32 Channels = UVs.GetNumChannels();
		// As the build does (FMeshDescriptionHelper::SetupRenderMeshDescription): a missing source is channel 0, a channel past the last is the next one.
		if (SourceChannel >= Channels) SourceChannel = 0;
		if (Channel >= Channels)
		{
			UVs.SetNumChannels(FMath::Min(Channel, MAX_MESH_TEXTURE_COORDS_MD - 1) + 1);
			Channel = Channels;
		}
		const bool bRemoveDegenerates = Asset.GetNumSourceModels() > 0 && Asset.GetSourceModel(0).BuildSettings.bRemoveDegenerates;
		FOverlappingCorners Corners;
		FStaticMeshOperations::FindOverlappingCorners(Corners, Copy, bRemoveDegenerates ? UE_THRESH_POINTS_ARE_SAME : 0.f);
		if (!FStaticMeshOperations::CreateLightMapUVLayout(Copy, SourceChannel, Channel, Resolution, ELightmapUVVersion(Asset.GetLightmapUVVersion()), Corners))
		{
			OutError = FString::Printf(TEXT("Unreal's lightmap packer could not lay out channel %d from channel %d at %d px"), Channel, SourceChannel, Resolution);
			return false;
		}
		return SoupFromDescription(Copy, Channel, FVector::OneVector, Out);
	}

	FString WhyNotWritable(const UStaticMesh& Mesh)
	{
		const UPackage* Package = Mesh.GetPackage();
		const FString PackageName = Package ? Package->GetName() : FString();
		if (IsEnginePath(PackageName))
		{
			return FString::Printf(TEXT("%s is Engine content, which agents do not change; duplicate it into /Game and change the copy."), *Mesh.GetPathName());
		}
		if (Package && (Package->HasAnyPackageFlags(PKG_Cooked) || Package->bIsCookedForEditor))
		{
			return FString::Printf(TEXT("%s is cooked: it has no source mesh to change."), *Mesh.GetPathName());
		}
		FString File;
		if (!IsUnderProject(PackageName, File))
		{
			return FString::Printf(TEXT("%s is not in the project (it lives in %s); duplicate it into /Game and change the copy."), *Mesh.GetPathName(),
				File.IsEmpty() ? TEXT("no folder on disk") : *FPaths::GetPath(File));
		}
		if (Mesh.GetNumSourceModels() == 0 || !Mesh.IsMeshDescriptionValid(0))
		{
			return FString::Printf(TEXT("%s has no source mesh (only built render data), so its UVs cannot be changed."), *Mesh.GetPathName());
		}
		if (IFileManager::Get().FileExists(*File) && IFileManager::Get().IsReadOnly(*File) && FHyperAIStudioSourceControl::DescribeBackend() == TEXT("none"))
		{
			return FString::Printf(TEXT("%s is read-only on disk and the project has no source control to check it out; clear the read-only flag first."), *File);
		}
		return FString();
	}

	FString WhyNotWritableFolder(const FString& Folder)
	{
		FText Reason;
		const FString Probe = Folder / TEXT("HyperAIProbe");
		if (!Folder.StartsWith(TEXT("/")) || !FPackageName::IsValidLongPackageName(Probe, /*bIncludeReadOnlyRoots=*/false, &Reason) || IsEnginePath(Folder + TEXT("/")))
		{
			return FString::Printf(TEXT("Folder '%s' is not a content folder in the project, like /Game/Models/Textures. %s"), *Folder, *Reason.ToString());
		}
		FString File;
		if (!IsUnderProject(Probe, File))
		{
			return FString::Printf(TEXT("Folder '%s' is not in the project."), *Folder);
		}
		return FString();
	}
}
