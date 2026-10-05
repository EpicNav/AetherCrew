// Games by Hyper 2026.

#include "HyperAIStudioModelingGate.h"

#include "Math/RandomStream.h"

namespace HyperAIStudio::Modeling::Recipes
{
	namespace
	{
		const FInfo Infos[] = {
			{TEXT("rock"), 150.0, TEXT("a boulder: a lumpy, faceted stone with a flat base and moss on top")},
			{TEXT("standing_stone"), 300.0, TEXT("a weathered upright monolith, slightly twisted and leaning, lichen on top")},
			{TEXT("pillar"), 400.0, TEXT("a column on a square plinth under a square capital, with rings at Detail 2+")},
			{TEXT("arch"), 400.0, TEXT("a stone archway Size wide: two piers and a half-round arch with a keystone; write it with Collision=hulls:4 or complex so the opening stays open")},
			{TEXT("ruined_wall"), 500.0, TEXT("a broken wall Size long: chunks bitten out, a slanted broken top, moss, and rubble at its foot")},
			{TEXT("tower"), 1200.0, TEXT("a round watchtower Size tall: battlements, window slits at Detail 2+, a conical roof (Accent)")},
			{TEXT("crystal_cluster"), 120.0, TEXT("glowing crystal shards (material slot 1, Accent) springing from a rock base (slot 0, Tint)")},
			{TEXT("mushroom"), 80.0, TEXT("a toadstool: a flared, leaning stem and a domed cap (Tint) with spots (Accent) at Detail 2+")},
			{TEXT("tree_trunk"), 400.0, TEXT("a tapering, curving trunk with flared roots and branch stubs, bark-bumped")},
			{TEXT("tree"), 600.0, TEXT("a stylised tree: a trunk (Tint, slot 0) under a lumpy canopy (Accent, slot 1)")},
			{TEXT("bush"), 150.0, TEXT("a rounded shrub of fused leafy lumps, flat underneath")},
			{TEXT("grass_tuft"), 40.0, TEXT("a tuft of curved blades, darker at the root (separate blades, so not watertight)")},
		};

		FString N(const double Value)
		{
			const double Rounded = FMath::RoundToDouble(Value * 100.0) / 100.0;
			return FString::Printf(TEXT("%g"), FMath::IsNearlyZero(Rounded) ? 0.0 : Rounded);
		}

		FString V(const double X, const double Y, const double Z)
		{
			return N(X) + TEXT(",") + N(Y) + TEXT(",") + N(Z);
		}

		FString Hex(const FLinearColor& Color)
		{
			auto Byte = [](const float Value) { return FMath::Clamp(FMath::RoundToInt(Value * 255.f), 0, 255); };
			return FString::Printf(TEXT("#%02x%02x%02x"), Byte(Color.R), Byte(Color.G), Byte(Color.B));
		}

		FLinearColor FromHex(const TCHAR* Text)
		{
			FLinearColor Out;
			Gate::ParseColor(Text, Out);
			return Out;
		}

		/** Writes one recipe's ops. Parts are named Mesh__part and dropped before the recipe ends. */
		struct FBuilder
		{
			const FParams& P;
			TArray<FHyperAIModelOp>& Out;
			FRandomStream Random;
			double S;
			int32 D;
			FLinearColor Tint;
			FLinearColor Accent;

			FBuilder(const FParams& InParams, TArray<FHyperAIModelOp>& InOut, const TCHAR* DefaultTint, const TCHAR* DefaultAccent)
				: P(InParams), Out(InOut), Random(InParams.Seed), S(InParams.Size), D(FMath::Clamp(InParams.Detail, 1, 3))
			{
				Tint = P.Tint.A > 0.f ? P.Tint : FromHex(DefaultTint);
				Accent = P.Accent.A > 0.f ? P.Accent : FromHex(DefaultAccent);
			}

			FHyperAIModelOp& Op(const TCHAR* Kind, const FString& Mesh, const FString& Params = FString())
			{
				FHyperAIModelOp& Made = Out.AddDefaulted_GetRef();
				Made.Kind = Kind;
				Made.Mesh = Mesh;
				Made.Params = Params;
				return Made;
			}

			FString Part(const TCHAR* Name) const
			{
				return P.Mesh + TEXT("__") + Name;
			}

			double Range(const double Min, const double Max)
			{
				return Random.FRandRange(Min, Max);
			}

			int32 Seed(const int32 Offset) const
			{
				// In 64 bits and folded back into the +-1e9 a Seed param takes, however large the recipe's own seed.
				return static_cast<int32>((static_cast<int64>(P.Seed) * 7919 + Offset) % 1000000000);
			}

			/** The colour varied a little, so repeated parts do not read as copies. */
			FString Varied(const FLinearColor& Color, const double Amount)
			{
				const float Scale = static_cast<float>(1.0 + Range(-Amount, Amount));
				return Hex(FLinearColor(Color.R * Scale, Color.G * Scale, Color.B * Scale));
			}

			/** UVs, the pivot on the base, and the place the request gave. */
			void Finish(const TCHAR* UVMode, const double UVScale)
			{
				Op(TEXT("uv"), P.Mesh, FString::Printf(TEXT("Mode=%s; Scale=%s"), UVMode, *N(FMath::Max(UVScale, 1.0))));
				Op(TEXT("pivot"), P.Mesh, TEXT("At=base"));
				if (!P.Location.TrimStartAndEnd().IsEmpty() || !P.Rotation.TrimStartAndEnd().IsEmpty() || !P.Scale.TrimStartAndEnd().IsEmpty())
				{
					FHyperAIModelOp& Place = Op(TEXT("transform"), P.Mesh);
					Place.Location = P.Location;
					Place.Rotation = P.Rotation;
					Place.Scale = P.Scale;
				}
			}
		};

		void Rock(FBuilder& B)
		{
			const double Width = B.S * B.Range(0.85, 1.05);
			const double Depth = B.S * 0.8 * B.Range(0.85, 1.1);
			const double Height = B.S * 0.6 * B.Range(0.8, 1.15);
			FHyperAIModelOp& Body = B.Op(TEXT("add.sphere"), B.P.Mesh, FString::Printf(TEXT("Style=box; Steps=%d; Origin=center; Color=%s"), 6 + 4 * B.D, *Hex(B.Tint)));
			Body.Size = V(Width, Depth, Height);
			Body.Location = V(0, 0, Height * 0.5);
			B.Op(TEXT("noise"), B.P.Mesh, FString::Printf(TEXT("Magnitude=%s; Frequency=%s; Seed=%d"), *N(B.S * 0.1), *FString::SanitizeFloat(1.0 / (B.S * 0.55)), B.Seed(1)));
			B.Op(TEXT("noise"), B.P.Mesh, FString::Printf(TEXT("Magnitude=%s; Frequency=%s; Seed=%d"), *N(B.S * 0.035), *FString::SanitizeFloat(1.0 / (B.S * 0.18)), B.Seed(2)));
			B.Op(TEXT("plane_cut"), B.P.Mesh, TEXT("Normal=0,0,-1")).Location = V(0, 0, Height * 0.18);
			B.Op(TEXT("simplify"), B.P.Mesh, FString::Printf(TEXT("Triangles=%d"), 250 * B.D * B.D + 150));
			B.Op(TEXT("normals"), B.P.Mesh, TEXT("Mode=split; Angle=32"));
			B.Op(TEXT("color"), B.P.Mesh, FString::Printf(TEXT("Color=%s"), *Hex(B.Accent))).Select = TEXT("normal:0,0,1,32");
			B.Finish(TEXT("box"), B.S * 0.5);
		}

		void StandingStone(FBuilder& B)
		{
			FHyperAIModelOp& Body = B.Op(TEXT("add.box"), B.P.Mesh, FString::Printf(TEXT("Steps=%d; Color=%s"), 3 + 2 * B.D, *Hex(B.Tint)));
			Body.Size = V(B.S * 0.36, B.S * 0.24, B.S);
			B.Op(TEXT("tessellate"), B.P.Mesh, TEXT("Level=1"));
			B.Op(TEXT("noise"), B.P.Mesh, FString::Printf(TEXT("Magnitude=%s; Frequency=%s; Seed=%d"), *N(B.S * 0.025), *FString::SanitizeFloat(1.0 / (B.S * 0.25)), B.Seed(1)));
			B.Op(TEXT("noise"), B.P.Mesh, FString::Printf(TEXT("Magnitude=%s; Frequency=%s; Seed=%d"), *N(B.S * 0.01), *FString::SanitizeFloat(1.0 / (B.S * 0.08)), B.Seed(2)));
			B.Op(TEXT("twist"), B.P.Mesh, FString::Printf(TEXT("Angle=%s"), *N(B.Range(-12.0, 12.0))));
			B.Op(TEXT("bend"), B.P.Mesh, FString::Printf(TEXT("Angle=%s"), *N(B.Range(-8.0, 8.0))));
			B.Op(TEXT("simplify"), B.P.Mesh, FString::Printf(TEXT("Triangles=%d"), 500 * B.D));
			B.Op(TEXT("normals"), B.P.Mesh, TEXT("Mode=split; Angle=35"));
			B.Op(TEXT("color"), B.P.Mesh, FString::Printf(TEXT("Color=%s"), *Hex(B.Accent))).Select = TEXT("normal:0,0,1,45");
			B.Finish(TEXT("box"), B.S * 0.3);
		}

		void Pillar(FBuilder& B)
		{
			const double Shaft = B.S * 0.2;
			const double Block = Shaft * 1.6;
			FHyperAIModelOp& Plinth = B.Op(TEXT("add.box"), B.P.Mesh, FString::Printf(TEXT("Color=%s"), *Hex(B.Accent)));
			Plinth.Size = V(Block, Block, B.S * 0.07);
			FHyperAIModelOp& Column = B.Op(TEXT("add.cylinder"), B.P.Mesh, FString::Printf(TEXT("Steps=%d; HeightSteps=%d; Color=%s"), 12 + 6 * B.D, 2 * B.D, *Hex(B.Tint)));
			Column.Size = V(Shaft, Shaft, B.S * 0.88);
			Column.Location = V(0, 0, B.S * 0.06);
			FHyperAIModelOp& Capital = B.Op(TEXT("add.box"), B.P.Mesh, FString::Printf(TEXT("Color=%s"), *Hex(B.Accent)));
			Capital.Size = V(Block, Block, B.S * 0.07);
			Capital.Location = V(0, 0, B.S * 0.93);
			if (B.D >= 2)
			{
				for (const double Height : {B.S * 0.07 - Shaft * 0.06, B.S * 0.93 - Shaft * 0.12})
				{
					FHyperAIModelOp& Ring = B.Op(TEXT("add.torus"), B.P.Mesh, FString::Printf(TEXT("Steps=%d; MinorSteps=8; Color=%s"), 16 + 8 * B.D, *Hex(B.Tint)));
					Ring.Size = V(Shaft * 1.3, Shaft * 1.3, Shaft * 0.18);
					Ring.Location = V(0, 0, Height);
				}
			}
			B.Op(TEXT("self_union"), B.P.Mesh);
			B.Op(TEXT("normals"), B.P.Mesh, TEXT("Mode=split; Angle=40"));
			B.Finish(TEXT("box"), 100.0);
		}

		void Arch(FBuilder& B)
		{
			const double Inner = B.S * 0.3;
			const double Wall = B.S * 0.1;
			const double Depth = B.S * 0.15;
			const double Pier = B.S * 0.55;
			for (const double Side : {-1.0, 1.0})
			{
				FHyperAIModelOp& Leg = B.Op(TEXT("add.box"), B.P.Mesh, FString::Printf(TEXT("Steps=%d; Color=%s"), B.D, *Hex(B.Tint)));
				// A little into the ring above, so the two fuse.
				Leg.Size = V(Wall, Depth, Pier + Wall * 0.3);
				Leg.Location = V(Side * (Inner + Wall * 0.5), 0, 0);
			}
			// The half ring turned about Z, then stood up into the XZ plane on top of the piers.
			FHyperAIModelOp& Ring = B.Op(TEXT("add.revolve"), B.P.Mesh, FString::Printf(TEXT("Ring=%s; Degrees=180; Steps=%d; Color=%s"), *N(Inner + Wall * 0.5), 8 + 6 * B.D, *Hex(B.Tint)));
			Ring.Points = FString::Printf(TEXT("%s,%s; %s,%s; %s,%s; %s,%s"), *N(-Wall * 0.5), *N(-Depth * 0.5), *N(Wall * 0.5), *N(-Depth * 0.5), *N(Wall * 0.5), *N(Depth * 0.5),
				*N(-Wall * 0.5), *N(Depth * 0.5));
			// Roll -90 takes the ring's +Y half up to +Z.
			Ring.Rotation = V(0, 0, -90);
			Ring.Location = V(0, 0, Pier);
			if (B.D >= 2)
			{
				FHyperAIModelOp& Key = B.Op(TEXT("add.box"), B.P.Mesh, FString::Printf(TEXT("Color=%s"), *Hex(B.Accent)));
				Key.Size = V(Wall * 0.7, Depth * 1.15, Wall * 1.3);
				Key.Location = V(0, 0, Pier + Inner + Wall * 0.5 - Wall * 0.65);
			}
			B.Op(TEXT("self_union"), B.P.Mesh);
			B.Op(TEXT("normals"), B.P.Mesh, TEXT("Mode=split; Angle=40"));
			B.Finish(TEXT("box"), 100.0);
		}

		void RuinedWall(FBuilder& B)
		{
			const double Height = B.S * 0.55;
			const double Thick = B.S * 0.1;
			FHyperAIModelOp& Wall = B.Op(TEXT("add.box"), B.P.Mesh, FString::Printf(TEXT("Steps=%d; Color=%s"), 4 + 2 * B.D, *Hex(B.Tint)));
			Wall.Size = V(B.S, Thick, Height);
			for (int32 Index = 0; Index < B.D; ++Index)
			{
				const FString Bite = B.Part(*FString::Printf(TEXT("bite%d"), Index));
				const double Radius = B.S * B.Range(0.1, 0.17);
				FHyperAIModelOp& Ball = B.Op(TEXT("add.sphere"), Bite, TEXT("Style=box; Steps=6; Origin=center"));
				Ball.Size = V(Radius * 2.0, Thick * 2.5, Radius * 2.0);
				Ball.Location = V(B.Range(-B.S * 0.4, B.S * 0.4), 0, Height - Radius * B.Range(0.1, 0.4));
				B.Op(TEXT("noise"), Bite, FString::Printf(TEXT("Magnitude=%s; Frequency=%s; Seed=%d"), *N(Radius * 0.15), *FString::SanitizeFloat(1.0 / (Radius * 0.8)), B.Seed(10 + Index)));
				FHyperAIModelOp& Cut = B.Op(TEXT("boolean"), B.P.Mesh, TEXT("Op=subtract"));
				Cut.Source = Bite;
				B.Op(TEXT("drop"), Bite);
			}
			const double Slant = FMath::DegreesToRadians(B.Range(14.0, 26.0)) * (B.Random.FRand() < 0.5 ? -1.0 : 1.0);
			B.Op(TEXT("plane_cut"), B.P.Mesh, FString::Printf(TEXT("Normal=%s"), *V(FMath::Sin(Slant), 0, FMath::Cos(Slant)))).Location = V(0, 0, Height * 0.78);
			B.Op(TEXT("remesh"), B.P.Mesh, FString::Printf(TEXT("Triangles=%d"), 1200 * B.D));
			B.Op(TEXT("noise"), B.P.Mesh, FString::Printf(TEXT("Magnitude=%s; Frequency=%s; Seed=%d"), *N(B.S * 0.006), *FString::SanitizeFloat(1.0 / (B.S * 0.08)), B.Seed(3)));
			const FString Rubble = B.Part(TEXT("rubble"));
			FHyperAIModelOp& Stone = B.Op(TEXT("add.sphere"), Rubble, FString::Printf(TEXT("Style=box; Steps=4; Color=%s"), *Hex(B.Tint)));
			Stone.Size = V(B.S * 0.09, B.S * 0.07, B.S * 0.06);
			B.Op(TEXT("noise"), Rubble, FString::Printf(TEXT("Magnitude=%s; Frequency=%s; Seed=%d"), *N(B.S * 0.01), *FString::SanitizeFloat(1.0 / (B.S * 0.05)), B.Seed(4)));
			for (int32 Index = 0; Index < 2 + B.D; ++Index)
			{
				FHyperAIModelOp& Piece = B.Op(TEXT("append"), B.P.Mesh);
				Piece.Source = Rubble;
				Piece.Location = V(B.Range(-B.S * 0.45, B.S * 0.45), (B.Random.FRand() < 0.5 ? -1.0 : 1.0) * (Thick * 0.5 + B.S * B.Range(0.02, 0.08)), 0);
				Piece.Rotation = V(B.Range(-15.0, 15.0), B.Range(0.0, 360.0), B.Range(-15.0, 15.0));
				Piece.Scale = N(B.Range(0.6, 1.3));
			}
			B.Op(TEXT("drop"), Rubble);
			B.Op(TEXT("normals"), B.P.Mesh, TEXT("Mode=split; Angle=35"));
			B.Op(TEXT("color"), B.P.Mesh, FString::Printf(TEXT("Color=%s"), *Hex(B.Accent))).Select = TEXT("normal:0,0,1,35");
			B.Finish(TEXT("box"), 100.0);
		}

		void Tower(FBuilder& B)
		{
			const double Body = B.S * 0.14;
			const double Rim = Body * 1.12;
			const double BodyHeight = B.S * 0.64;
			const double RimHeight = B.S * 0.08;
			const int32 Around = 16 + 8 * B.D;
			FHyperAIModelOp& Wall = B.Op(TEXT("add.cylinder"), B.P.Mesh, FString::Printf(TEXT("Steps=%d; HeightSteps=%d; Color=%s"), Around, 2 + 2 * B.D, *Hex(B.Tint)));
			Wall.Size = V(Body * 2.0, Body * 2.0, BodyHeight);
			FHyperAIModelOp& Parapet = B.Op(TEXT("add.cylinder"), B.P.Mesh, FString::Printf(TEXT("Steps=%d; Color=%s"), Around, *Hex(B.Tint)));
			Parapet.Size = V(Rim * 2.0, Rim * 2.0, RimHeight);
			Parapet.Location = V(0, 0, BodyHeight - B.S * 0.01);
			const FString Merlon = B.Part(TEXT("merlon"));
			FHyperAIModelOp& Tooth = B.Op(TEXT("add.box"), Merlon, FString::Printf(TEXT("Color=%s"), *Hex(B.Tint)));
			Tooth.Size = V(Rim * 0.22, Rim * 0.35, RimHeight * 0.6);
			FHyperAIModelOp& Ring = B.Op(TEXT("array.radial"), B.P.Mesh, FString::Printf(TEXT("Count=%d; Radius=%s"), 8 + 2 * B.D, *N(Rim * 0.88)));
			Ring.Source = Merlon;
			Ring.Location = V(0, 0, BodyHeight + RimHeight - B.S * 0.022);
			B.Op(TEXT("drop"), Merlon);
			if (B.D >= 2)
			{
				const FString Slit = B.Part(TEXT("slit"));
				const FString Slits = B.Part(TEXT("slits"));
				FHyperAIModelOp& Window = B.Op(TEXT("add.box"), Slit);
				Window.Size = V(Body * 0.5, B.S * 0.025, B.S * 0.07);
				for (int32 Row = 0; Row < B.D - 1; ++Row)
				{
					FHyperAIModelOp& Set = B.Op(TEXT("array.radial"), Slits, FString::Printf(TEXT("Count=3; Radius=%s"), *N(Body)));
					Set.Source = Slit;
					Set.Location = V(0, 0, BodyHeight * (0.62 - 0.3 * Row));
				}
				FHyperAIModelOp& Cut = B.Op(TEXT("boolean"), B.P.Mesh, TEXT("Op=subtract"));
				Cut.Source = Slits;
				B.Op(TEXT("drop"), Slit);
				B.Op(TEXT("drop"), Slits);
			}
			FHyperAIModelOp& Roof = B.Op(TEXT("add.cone"), B.P.Mesh, FString::Printf(TEXT("Steps=%d; Top=0.02; Color=%s"), Around, *Hex(B.Accent)));
			Roof.Size = V(Rim * 2.1, Rim * 2.1, B.S * 0.3);
			Roof.Location = V(0, 0, BodyHeight + RimHeight * 0.6);
			B.Op(TEXT("self_union"), B.P.Mesh);
			B.Op(TEXT("normals"), B.P.Mesh, TEXT("Mode=split; Angle=40"));
			B.Finish(TEXT("cylinder"), 200.0);
		}

		void CrystalCluster(FBuilder& B)
		{
			FHyperAIModelOp& Base = B.Op(TEXT("add.sphere"), B.P.Mesh, FString::Printf(TEXT("Style=box; Steps=%d; Origin=center; Color=%s"), 6 + 2 * B.D, *Hex(B.Tint)));
			Base.Size = V(B.S * 0.9, B.S * 0.75, B.S * 0.35);
			Base.Location = V(0, 0, B.S * 0.08);
			B.Op(TEXT("noise"), B.P.Mesh, FString::Printf(TEXT("Magnitude=%s; Frequency=%s; Seed=%d"), *N(B.S * 0.05), *FString::SanitizeFloat(1.0 / (B.S * 0.35)), B.Seed(1)));
			B.Op(TEXT("plane_cut"), B.P.Mesh, TEXT("Normal=0,0,-1"));
			const FString Shard = B.Part(TEXT("shard"));
			FHyperAIModelOp& Point = B.Op(TEXT("add.cone"), Shard, TEXT("Steps=6; Top=0.05; HeightSteps=1; Material=1"));
			Point.Size = V(B.S * 0.16, B.S * 0.16, B.S * 0.75);
			B.Op(TEXT("color"), Shard, FString::Printf(TEXT("Color=%s; Glow=0.35"), *Hex(B.Accent)));
			FHyperAIModelOp& Heart = B.Op(TEXT("append"), B.P.Mesh);
			Heart.Source = Shard;
			Heart.Location = V(0, 0, B.S * 0.05);
			Heart.Rotation = V(B.Range(-8.0, 8.0), B.Range(0.0, 360.0), B.Range(-8.0, 8.0));
			Heart.Scale = TEXT("1.25");
			FHyperAIModelOp& Spray = B.Op(TEXT("scatter_on"), B.P.Mesh, FString::Printf(TEXT("Count=%d; Spacing=%s; Seed=%d; ScaleMin=0.45; ScaleMax=0.95; Tilt=18; Up=60; Sink=%s"),
				5 + 3 * B.D, *N(B.S * 0.14), B.Seed(2), *N(B.S * 0.04)));
			Spray.Source = Shard;
			B.Op(TEXT("drop"), Shard);
			B.Op(TEXT("self_union"), B.P.Mesh);
			B.Op(TEXT("normals"), B.P.Mesh, TEXT("Mode=hard"));
			B.Finish(TEXT("box"), B.S * 0.5);
		}

		void Mushroom(FBuilder& B)
		{
			const FString Cap = B.Part(TEXT("cap"));
			FHyperAIModelOp& Stem = B.Op(TEXT("add.cylinder"), B.P.Mesh, FString::Printf(TEXT("Steps=%d; HeightSteps=%d; Color=%s"), 10 + 4 * B.D, 4 + 2 * B.D, *Hex(B.Accent)));
			Stem.Size = V(B.S * 0.2, B.S * 0.2, B.S * 0.72);
			B.Op(TEXT("flare"), B.P.Mesh, FString::Printf(TEXT("Percent=45; Extent=%s"), *N(B.S * 0.22))).Location = V(0, 0, 0);
			// The cap's profile from its crown out to the rim and back under to the stem, radius,height.
			const double Radius = B.S * 0.42;
			const double Rise = B.S * 0.38;
			const double Axis = FMath::Max(0.2, B.S * 0.005);
			FHyperAIModelOp& Dome = B.Op(TEXT("add.revolve"), Cap, FString::Printf(TEXT("Steps=%d; Capped=true; Color=%s"), 16 + 8 * B.D, *Hex(B.Tint)));
			Dome.Points = FString::Printf(TEXT("%s,%s; %s,%s; %s,%s; %s,%s; %s,%s; %s,%s; %s,%s; %s,%s"), *N(Axis), *N(Rise), *N(Radius * 0.45), *N(Rise * 0.92), *N(Radius * 0.8),
				*N(Rise * 0.62), *N(Radius), *N(Rise * 0.22), *N(Radius * 0.96), *N(0.0), *N(Radius * 0.6), *N(Rise * 0.06), *N(Radius * 0.2), *N(Rise * 0.1), *N(Axis), *N(Rise * 0.1));
			if (B.D >= 2)
			{
				const FString Spot = B.Part(TEXT("spot"));
				FHyperAIModelOp& Dot = B.Op(TEXT("add.sphere"), Spot, FString::Printf(TEXT("Steps=8; Origin=center; Color=%s"), *Hex(B.Accent)));
				Dot.Size = V(B.S * 0.07, B.S * 0.07, B.S * 0.035);
				FHyperAIModelOp& Spots = B.Op(TEXT("scatter_on"), Cap, FString::Printf(TEXT("Count=%d; Spacing=%s; Seed=%d; ScaleMin=0.6; ScaleMax=1.2; Tilt=0; Up=55; Sink=%s"),
					3 + 3 * B.D, *N(B.S * 0.1), B.Seed(1), *N(B.S * 0.01)));
				Spots.Source = Spot;
				B.Op(TEXT("drop"), Spot);
			}
			FHyperAIModelOp& Join = B.Op(TEXT("append"), B.P.Mesh);
			Join.Source = Cap;
			Join.Location = V(0, 0, B.S * 0.6);
			B.Op(TEXT("drop"), Cap);
			B.Op(TEXT("bend"), B.P.Mesh, FString::Printf(TEXT("Angle=%s"), *N(B.Range(10.0, 22.0) * (B.Random.FRand() < 0.5 ? -1.0 : 1.0))));
			B.Op(TEXT("self_union"), B.P.Mesh);
			B.Op(TEXT("normals"), B.P.Mesh, TEXT("Mode=smooth"));
			B.Finish(TEXT("cylinder"), B.S);
		}

		/** A tube along a curve: Count points from From to To, bowed sideways by Bow, as an add.sweep's Path. */
		FString Curve(const FVector& From, const FVector& To, const FVector& Bow, const int32 Count)
		{
			TArray<FString> Points;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const double T = static_cast<double>(Index) / (Count - 1);
				const FVector Point = FMath::Lerp(From, To, T) + Bow * FMath::Sin(T * UE_DOUBLE_PI);
				Points.Add(V(Point.X, Point.Y, Point.Z));
			}
			return FString::Join(Points, TEXT("; "));
		}

		/** A trunk Height tall from the ground, roots and branch stubs swept with it, all into Mesh. */
		void Trunk(FBuilder& B, const double Height, const double Radius, const int32 Roots, const int32 Branches)
		{
			const int32 Sides = 8 + 4 * B.D;
			const FVector Lean(B.Range(-1.0, 1.0) * Height * 0.05, B.Range(-1.0, 1.0) * Height * 0.05, 0.0);
			FHyperAIModelOp& Body = B.Op(TEXT("add.sweep"), B.P.Mesh, FString::Printf(TEXT("EndScale=0.45; Color=%s"), *Hex(B.Tint)));
			Body.Points = FString::Printf(TEXT("circle:%s,%d"), *N(Radius), Sides);
			Body.Path = Curve(FVector(0, 0, -Radius * 0.3), FVector(Lean.X * 0.5, Lean.Y * 0.5, Height), Lean, 4 + 2 * B.D);
			for (int32 Index = 0; Index < Roots; ++Index)
			{
				const double Angle = 2.0 * UE_DOUBLE_PI * (Index + B.Range(-0.2, 0.2)) / Roots;
				const FVector Out(FMath::Cos(Angle), FMath::Sin(Angle), 0.0);
				FHyperAIModelOp& Root = B.Op(TEXT("add.sweep"), B.P.Mesh, FString::Printf(TEXT("EndScale=0.25; Color=%s"), *Hex(B.Tint)));
				Root.Points = FString::Printf(TEXT("circle:%s,8"), *N(Radius * 0.45));
				Root.Path = Curve(FVector(0, 0, Height * 0.12), Out * Height * B.Range(0.18, 0.26) + FVector(0, 0, -Radius * 0.2), Out * Height * 0.04 + FVector(0, 0, Height * 0.02), 4);
			}
			for (int32 Index = 0; Index < Branches; ++Index)
			{
				const double Angle = B.Range(0.0, 2.0 * UE_DOUBLE_PI);
				const FVector Out(FMath::Cos(Angle), FMath::Sin(Angle), 0.0);
				const double From = Height * B.Range(0.45, 0.8);
				FHyperAIModelOp& Branch = B.Op(TEXT("add.sweep"), B.P.Mesh, FString::Printf(TEXT("EndScale=0.35; Color=%s"), *Hex(B.Tint)));
				Branch.Points = FString::Printf(TEXT("circle:%s,8"), *N(Radius * 0.35));
				Branch.Path = Curve(FVector(Lean.X * 0.4, Lean.Y * 0.4, From), Out * Height * B.Range(0.16, 0.24) + FVector(0, 0, From + Height * 0.14), FVector(0, 0, Height * 0.03), 4);
			}
		}

		void TreeTrunk(FBuilder& B)
		{
			const double Radius = B.S * 0.07;
			Trunk(B, B.S, Radius, 3 + B.D, B.D >= 2 ? B.D : 0);
			B.Op(TEXT("self_union"), B.P.Mesh);
			B.Op(TEXT("noise"), B.P.Mesh, FString::Printf(TEXT("Magnitude=%s; Frequency=%s; Seed=%d"), *N(Radius * 0.08), *FString::SanitizeFloat(1.0 / (Radius * 1.2)), B.Seed(1)));
			B.Op(TEXT("plane_cut"), B.P.Mesh, TEXT("Normal=0,0,-1"));
			B.Op(TEXT("normals"), B.P.Mesh, TEXT("Mode=smooth"));
			B.Finish(TEXT("cylinder"), Radius * 6.0);
		}

		void Tree(FBuilder& B)
		{
			const double Radius = B.S * 0.045;
			Trunk(B, B.S * 0.62, Radius, 2 + B.D, 2);
			const FString Leaves = B.Part(TEXT("leaves"));
			for (int32 Index = 0; Index < 2 + B.D; ++Index)
			{
				const double Across = B.S * B.Range(0.32, 0.42);
				FHyperAIModelOp& Blob = B.Op(TEXT("add.sphere"), Leaves, FString::Printf(TEXT("Style=box; Steps=%d; Origin=center; Material=1; Color=%s"), 5 + 2 * B.D,
					*B.Varied(B.Accent, 0.12)));
				Blob.Size = V(Across, Across * B.Range(0.85, 1.0), B.S * B.Range(0.26, 0.32));
				Blob.Location = V(B.Range(-B.S * 0.12, B.S * 0.12), B.Range(-B.S * 0.12, B.S * 0.12), B.S * B.Range(0.62, 0.78));
			}
			B.Op(TEXT("noise"), Leaves, FString::Printf(TEXT("Magnitude=%s; Frequency=%s; Seed=%d"), *N(B.S * 0.03), *FString::SanitizeFloat(1.0 / (B.S * 0.12)), B.Seed(1)));
			FHyperAIModelOp& Crown = B.Op(TEXT("append"), B.P.Mesh);
			Crown.Source = Leaves;
			B.Op(TEXT("drop"), Leaves);
			B.Op(TEXT("self_union"), B.P.Mesh);
			B.Op(TEXT("plane_cut"), B.P.Mesh, TEXT("Normal=0,0,-1"));
			B.Op(TEXT("normals"), B.P.Mesh, TEXT("Mode=smooth"));
			B.Finish(TEXT("box"), B.S * 0.2);
		}

		void Bush(FBuilder& B)
		{
			for (int32 Index = 0; Index < 3 + B.D; ++Index)
			{
				FHyperAIModelOp& Blob = B.Op(TEXT("add.sphere"), B.P.Mesh, FString::Printf(TEXT("Style=box; Steps=%d; Origin=center; Color=%s"), 5 + 2 * B.D, *B.Varied(B.Tint, 0.12)));
				const double Across = B.S * B.Range(0.45, 0.6);
				Blob.Size = V(Across, Across * B.Range(0.85, 1.0), B.S * B.Range(0.4, 0.5));
				Blob.Location = V(B.Range(-B.S * 0.22, B.S * 0.22), B.Range(-B.S * 0.22, B.S * 0.22), B.S * B.Range(0.2, 0.3));
			}
			B.Op(TEXT("noise"), B.P.Mesh, FString::Printf(TEXT("Magnitude=%s; Frequency=%s; Seed=%d"), *N(B.S * 0.05), *FString::SanitizeFloat(1.0 / (B.S * 0.18)), B.Seed(1)));
			B.Op(TEXT("self_union"), B.P.Mesh);
			B.Op(TEXT("plane_cut"), B.P.Mesh, TEXT("Normal=0,0,-1")).Location = V(0, 0, B.S * 0.03);
			B.Op(TEXT("normals"), B.P.Mesh, TEXT("Mode=smooth"));
			B.Finish(TEXT("box"), B.S * 0.5);
		}

		void GrassTuft(FBuilder& B)
		{
			const FString Blade = B.Part(TEXT("blade"));
			FHyperAIModelOp& Leaf = B.Op(TEXT("add.cone"), Blade, FString::Printf(TEXT("Steps=4; HeightSteps=%d; Top=0.05; Color=%s"), 2 + B.D, *Hex(B.Tint)));
			Leaf.Size = V(B.S * 0.07, B.S * 0.025, B.S);
			B.Op(TEXT("bend"), Blade, TEXT("Angle=28"));
			FHyperAIModelOp& Ring = B.Op(TEXT("array.radial"), B.P.Mesh, FString::Printf(TEXT("Count=%d; Radius=%s; Jitter=0.8; Seed=%d"), 6 + 4 * B.D, *N(B.S * 0.06), B.Seed(1)));
			Ring.Source = Blade;
			if (B.D >= 2)
			{
				FHyperAIModelOp& Inner = B.Op(TEXT("array.radial"), B.P.Mesh, FString::Printf(TEXT("Count=%d; Radius=%s; Jitter=0.9; Seed=%d"), 2 + 2 * B.D, *N(B.S * 0.02), B.Seed(2)));
				Inner.Source = Blade;
			}
			B.Op(TEXT("drop"), Blade);
			B.Op(TEXT("color"), B.P.Mesh, FString::Printf(TEXT("Color=%s"), *Hex(B.Accent))).Select = FString::Printf(TEXT("box:%s,%s,%s,%s,%s,%s"), *N(-B.S), *N(-B.S), *N(-1.0),
				*N(B.S), *N(B.S), *N(B.S * 0.22));
			B.Op(TEXT("normals"), B.P.Mesh, TEXT("Mode=smooth"));
			B.Finish(TEXT("box"), B.S);
		}
	}

	TArrayView<const FInfo> List()
	{
		return MakeArrayView(Infos);
	}

	const FInfo* Find(const FString& Name)
	{
		for (const FInfo& Info : Infos)
		{
			if (Name.Equals(Info.Name, ESearchCase::IgnoreCase)) return &Info;
		}
		return nullptr;
	}

	bool Expand(const FParams& Params, TArray<FHyperAIModelOp>& OutOps, FString& OutError)
	{
		const FString Shape = Params.Shape.ToLower();
		if (!Find(Shape))
		{
			OutError = FString::Printf(TEXT("'%s' is not a recipe."), *Params.Shape);
			return false;
		}
		if (Params.Size <= 0.0)
		{
			OutError = TEXT("Size is above 0 cm.");
			return false;
		}
		struct FRecipe
		{
			const TCHAR* Name;
			void (*Make)(FBuilder&);
			const TCHAR* Tint;
			const TCHAR* Accent;
		};
		static const FRecipe Recipes[] = {
			{TEXT("rock"), &Rock, TEXT("#7d776d"), TEXT("#5e6b3c")},
			{TEXT("standing_stone"), &StandingStone, TEXT("#8a857c"), TEXT("#8d9a5b")},
			{TEXT("pillar"), &Pillar, TEXT("#c9c1b0"), TEXT("#a39b8a")},
			{TEXT("arch"), &Arch, TEXT("#a59d8e"), TEXT("#857d6f")},
			{TEXT("ruined_wall"), &RuinedWall, TEXT("#8f877a"), TEXT("#5e6b3c")},
			{TEXT("tower"), &Tower, TEXT("#9c9488"), TEXT("#6b3a2e")},
			{TEXT("crystal_cluster"), &CrystalCluster, TEXT("#5a5560"), TEXT("#7fd8ff")},
			{TEXT("mushroom"), &Mushroom, TEXT("#b3342b"), TEXT("#efe6cf")},
			{TEXT("tree_trunk"), &TreeTrunk, TEXT("#6b4a2f"), TEXT("#4e7d34")},
			{TEXT("tree"), &Tree, TEXT("#6b4a2f"), TEXT("#4e7d34")},
			{TEXT("bush"), &Bush, TEXT("#4a7a32"), TEXT("#6a9a3c")},
			{TEXT("grass_tuft"), &GrassTuft, TEXT("#6f9a3a"), TEXT("#4c6b2a")},
		};
		for (const FRecipe& Recipe : Recipes)
		{
			if (Shape != Recipe.Name) continue;
			FBuilder Builder(Params, OutOps, Recipe.Tint, Recipe.Accent);
			Recipe.Make(Builder);
			return true;
		}
		OutError = FString::Printf(TEXT("'%s' has no ops."), *Params.Shape);
		return false;
	}
}
