// Games by Hyper 2026.

#include "HyperAIStudioQrCode.h"

#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::QrCode::Private
{
	// Level M, by version 1 to 10.
	constexpr int32 EccPerBlock[] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
	constexpr int32 Blocks[] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};

	int32 RawDataModules(const int32 Version)
	{
		int32 Result = (16 * Version + 128) * Version + 64;
		if (Version >= 2)
		{
			const int32 Align = Version / 7 + 2;
			Result -= (25 * Align - 10) * Align - 55;
			if (Version >= 7)
			{
				Result -= 36;
			}
		}
		return Result;
	}

	uint8 Multiply(const uint8 X, const uint8 Y)
	{
		int32 Z = 0;
		for (int32 Bit = 7; Bit >= 0; --Bit)
		{
			Z = (Z << 1) ^ ((Z >> 7) * 0x11D);
			Z ^= ((Y >> Bit) & 1) * X;
		}
		return static_cast<uint8>(Z & 0xFF);
	}

	TArray<uint8> Divisor(const int32 Degree)
	{
		TArray<uint8> Result;
		Result.Init(0, Degree);
		Result[Degree - 1] = 1;
		uint8 Root = 1;
		for (int32 Index = 0; Index < Degree; ++Index)
		{
			for (int32 J = 0; J < Degree; ++J)
			{
				Result[J] = Multiply(Result[J], Root);
				if (J + 1 < Degree)
				{
					Result[J] ^= Result[J + 1];
				}
			}
			Root = Multiply(Root, 0x02);
		}
		return Result;
	}

	TArray<int32> AlignmentPositions(const int32 Version, const int32 Size)
	{
		TArray<int32> Result;
		if (Version == 1)
		{
			return Result;
		}
		const int32 Count = Version / 7 + 2;
		const int32 Step = (Version * 4 + Count * 2 + 1) / (Count * 2 - 2) * 2;
		Result.Add(6);
		TArray<int32> Rest;
		for (int32 Position = Size - 7; Rest.Num() < Count - 1; Position -= Step)
		{
			Rest.Insert(Position, 0);
		}
		Result.Append(Rest);
		return Result;
	}

	struct FBuilder
	{
		int32 Version = 1;
		int32 Size = 21;
		TArray<bool> Dark;
		TArray<bool> Function;

		void Set(const int32 X, const int32 Y, const bool bDark)
		{
			Dark[Y * Size + X] = bDark;
			Function[Y * Size + X] = true;
		}

		void Finder(const int32 CX, const int32 CY)
		{
			for (int32 DY = -4; DY <= 4; ++DY)
			{
				for (int32 DX = -4; DX <= 4; ++DX)
				{
					const int32 Distance = FMath::Max(FMath::Abs(DX), FMath::Abs(DY));
					const int32 X = CX + DX;
					const int32 Y = CY + DY;
					if (X >= 0 && X < Size && Y >= 0 && Y < Size)
					{
						Set(X, Y, Distance != 2 && Distance != 4);
					}
				}
			}
		}

		void Alignment(const int32 CX, const int32 CY)
		{
			for (int32 DY = -2; DY <= 2; ++DY)
			{
				for (int32 DX = -2; DX <= 2; ++DX)
				{
					Set(CX + DX, CY + DY, FMath::Max(FMath::Abs(DX), FMath::Abs(DY)) != 1);
				}
			}
		}

		void FormatInfo(const int32 Mask)
		{
			const uint32 Bits = FHyperAIStudioQrCode::FormatBits(0, Mask);
			auto Bit = [Bits](const int32 Index) { return ((Bits >> Index) & 1) != 0; };
			for (int32 Index = 0; Index <= 5; ++Index)
			{
				Set(8, Index, Bit(Index));
			}
			Set(8, 7, Bit(6));
			Set(8, 8, Bit(7));
			Set(7, 8, Bit(8));
			for (int32 Index = 9; Index < 15; ++Index)
			{
				Set(14 - Index, 8, Bit(Index));
			}
			for (int32 Index = 0; Index < 8; ++Index)
			{
				Set(Size - 1 - Index, 8, Bit(Index));
			}
			for (int32 Index = 8; Index < 15; ++Index)
			{
				Set(8, Size - 15 + Index, Bit(Index));
			}
			Set(8, Size - 8, true);
		}

		void FunctionPatterns()
		{
			for (int32 Index = 0; Index < Size; ++Index)
			{
				Set(6, Index, Index % 2 == 0);
				Set(Index, 6, Index % 2 == 0);
			}
			Finder(3, 3);
			Finder(Size - 4, 3);
			Finder(3, Size - 4);
			const TArray<int32> Positions = AlignmentPositions(Version, Size);
			const int32 Count = Positions.Num();
			for (int32 I = 0; I < Count; ++I)
			{
				for (int32 J = 0; J < Count; ++J)
				{
					if (!((I == 0 && J == 0) || (I == 0 && J == Count - 1) || (I == Count - 1 && J == 0)))
					{
						Alignment(Positions[I], Positions[J]);
					}
				}
			}
			FormatInfo(0);
			if (Version >= 7)
			{
				const uint32 Bits = FHyperAIStudioQrCode::VersionBits(Version);
				for (int32 Index = 0; Index < 18; ++Index)
				{
					const bool bBit = ((Bits >> Index) & 1) != 0;
					const int32 A = Size - 11 + Index % 3;
					const int32 B = Index / 3;
					Set(A, B, bBit);
					Set(B, A, bBit);
				}
			}
		}

		void Codewords(const TArray<uint8>& Data)
		{
			int32 Bit = 0;
			for (int32 Right = Size - 1; Right >= 1; Right -= 2)
			{
				if (Right == 6)
				{
					Right = 5;
				}
				for (int32 Vertical = 0; Vertical < Size; ++Vertical)
				{
					for (int32 J = 0; J < 2; ++J)
					{
						const int32 X = Right - J;
						const bool bUpward = ((Right + 1) & 2) == 0;
						const int32 Y = bUpward ? Size - 1 - Vertical : Vertical;
						if (!Function[Y * Size + X] && Bit < Data.Num() * 8)
						{
							Dark[Y * Size + X] = ((Data[Bit >> 3] >> (7 - (Bit & 7))) & 1) != 0;
							++Bit;
						}
					}
				}
			}
		}

		static bool Masked(const int32 Mask, const int32 X, const int32 Y)
		{
			switch (Mask)
			{
			case 0: return (X + Y) % 2 == 0;
			case 1: return Y % 2 == 0;
			case 2: return X % 3 == 0;
			case 3: return (X + Y) % 3 == 0;
			case 4: return (X / 3 + Y / 2) % 2 == 0;
			case 5: return X * Y % 2 + X * Y % 3 == 0;
			case 6: return (X * Y % 2 + X * Y % 3) % 2 == 0;
			default: return ((X + Y) % 2 + X * Y % 3) % 2 == 0;
			}
		}

		void ApplyMask(const int32 Mask)
		{
			for (int32 Y = 0; Y < Size; ++Y)
			{
				for (int32 X = 0; X < Size; ++X)
				{
					if (!Function[Y * Size + X] && Masked(Mask, X, Y))
					{
						Dark[Y * Size + X] = !Dark[Y * Size + X];
					}
				}
			}
		}

		bool At(const int32 X, const int32 Y) const { return Dark[Y * Size + X]; }

		int32 Penalty() const
		{
			int32 Score = 0;
			// Runs of five or more of one colour, across and down.
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				for (int32 A = 0; A < Size; ++A)
				{
					int32 Run = 1;
					for (int32 B = 1; B <= Size; ++B)
					{
						const bool bSame = B < Size && (Pass == 0 ? At(B, A) == At(B - 1, A) : At(A, B) == At(A, B - 1));
						if (bSame)
						{
							++Run;
							continue;
						}
						if (Run >= 5)
						{
							Score += 3 + (Run - 5);
						}
						Run = 1;
					}
				}
			}
			// Two-by-two blocks of one colour.
			for (int32 Y = 0; Y + 1 < Size; ++Y)
			{
				for (int32 X = 0; X + 1 < Size; ++X)
				{
					const bool C = At(X, Y);
					if (C == At(X + 1, Y) && C == At(X, Y + 1) && C == At(X + 1, Y + 1))
					{
						Score += 3;
					}
				}
			}
			// Finder-like patterns, dark-light-dark-dark-dark-light-dark with four light on one side.
			static const bool Pattern[2][11] = {
				{true, false, true, true, true, false, true, false, false, false, false},
				{false, false, false, false, true, false, true, true, true, false, true}};
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				for (int32 A = 0; A < Size; ++A)
				{
					for (int32 B = 0; B + 11 <= Size; ++B)
					{
						for (const auto& Shape : Pattern)
						{
							bool bMatch = true;
							for (int32 K = 0; K < 11 && bMatch; ++K)
							{
								bMatch = (Pass == 0 ? At(B + K, A) : At(A, B + K)) == Shape[K];
							}
							Score += bMatch ? 40 : 0;
						}
					}
				}
			}
			int32 DarkCount = 0;
			for (const bool bDark : Dark)
			{
				DarkCount += bDark;
			}
			const int32 Total = Size * Size;
			const int32 K = (FMath::Abs(DarkCount * 20 - Total * 10) + Total - 1) / Total - 1;
			Score += FMath::Max(K, 0) * 10;
			return Score;
		}
	};
}

TArray<uint8> FHyperAIStudioQrCode::ReedSolomonRemainder(const TArray<uint8>& Data, const int32 Degree)
{
	using namespace HyperAIStudio::QrCode::Private;
	const TArray<uint8> Generator = Divisor(Degree);
	TArray<uint8> Result;
	Result.Init(0, Degree);
	for (const uint8 Byte : Data)
	{
		const uint8 Factor = Byte ^ Result[0];
		Result.RemoveAt(0);
		Result.Add(0);
		for (int32 Index = 0; Index < Degree; ++Index)
		{
			Result[Index] ^= Multiply(Generator[Index], Factor);
		}
	}
	return Result;
}

uint32 FHyperAIStudioQrCode::FormatBits(const int32 LevelBits, const int32 MaskPattern)
{
	const uint32 Data = (static_cast<uint32>(LevelBits) << 3) | static_cast<uint32>(MaskPattern);
	uint32 Remainder = Data;
	for (int32 Index = 0; Index < 10; ++Index)
	{
		Remainder = (Remainder << 1) ^ ((Remainder >> 9) * 0x537);
	}
	return ((Data << 10) | Remainder) ^ 0x5412;
}

uint32 FHyperAIStudioQrCode::VersionBits(const int32 InVersion)
{
	uint32 Remainder = static_cast<uint32>(InVersion);
	for (int32 Index = 0; Index < 12; ++Index)
	{
		Remainder = (Remainder << 1) ^ ((Remainder >> 11) * 0x1F25);
	}
	return (static_cast<uint32>(InVersion) << 12) | Remainder;
}

int32 FHyperAIStudioQrCode::DataCodewords(const int32 InVersion)
{
	using namespace HyperAIStudio::QrCode::Private;
	return InVersion < 1 || InVersion > MaxVersion ? 0 : RawDataModules(InVersion) / 8 - EccPerBlock[InVersion] * Blocks[InVersion];
}

bool FHyperAIStudioQrCode::Encode(const FString& Text, FHyperAIStudioQrCode& Out, FString& OutError)
{
	using namespace HyperAIStudio::QrCode::Private;
	const FTCHARToUTF8 Utf8(*Text);
	const int32 Length = Utf8.Length();
	int32 Version = 1;
	for (; Version <= MaxVersion; ++Version)
	{
		const int32 CountBits = Version <= 9 ? 8 : 16;
		if (4 + CountBits + Length * 8 <= DataCodewords(Version) * 8)
		{
			break;
		}
	}
	if (Version > MaxVersion)
	{
		OutError = FString::Printf(TEXT("%d bytes is more than a version %d QR code holds (%d)."), Length, MaxVersion, DataCodewords(MaxVersion) - 3);
		return false;
	}
	// The bit stream: byte mode, the count, the bytes, a terminator, then padding.
	TArray<bool> Bits;
	auto Append = [&Bits](const uint32 Value, const int32 Count)
	{
		for (int32 Index = Count - 1; Index >= 0; --Index)
		{
			Bits.Add(((Value >> Index) & 1) != 0);
		}
	};
	Append(0x4, 4);
	Append(static_cast<uint32>(Length), Version <= 9 ? 8 : 16);
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Append(static_cast<uint8>(Utf8.Get()[Index]), 8);
	}
	const int32 Capacity = DataCodewords(Version) * 8;
	Append(0, FMath::Min(4, Capacity - Bits.Num()));
	Append(0, (8 - Bits.Num() % 8) % 8);
	for (uint8 Pad = 0xEC; Bits.Num() < Capacity; Pad ^= 0xEC ^ 0x11)
	{
		Append(Pad, 8);
	}
	TArray<uint8> Data;
	Data.Init(0, Bits.Num() / 8);
	for (int32 Index = 0; Index < Bits.Num(); ++Index)
	{
		Data[Index >> 3] |= (Bits[Index] ? 1 : 0) << (7 - (Index & 7));
	}
	// Split into blocks, add each block's error correction, and interleave.
	const int32 BlockCount = Blocks[Version];
	const int32 EccLength = EccPerBlock[Version];
	const int32 Raw = RawDataModules(Version) / 8;
	const int32 ShortBlocks = BlockCount - Raw % BlockCount;
	const int32 ShortLength = Raw / BlockCount;
	TArray<TArray<uint8>> BlockData;
	for (int32 Block = 0, At = 0; Block < BlockCount; ++Block)
	{
		const int32 DataLength = ShortLength - EccLength + (Block < ShortBlocks ? 0 : 1);
		TArray<uint8> Dat(Data.GetData() + At, DataLength);
		At += DataLength;
		const TArray<uint8> Ecc = ReedSolomonRemainder(Dat, EccLength);
		if (Block < ShortBlocks)
		{
			Dat.Add(0);
		}
		Dat.Append(Ecc);
		BlockData.Add(MoveTemp(Dat));
	}
	TArray<uint8> Interleaved;
	for (int32 Index = 0; Index < BlockData[0].Num(); ++Index)
	{
		for (int32 Block = 0; Block < BlockCount; ++Block)
		{
			if (Index != ShortLength - EccLength || Block >= ShortBlocks)
			{
				Interleaved.Add(BlockData[Block][Index]);
			}
		}
	}
	FBuilder Builder;
	Builder.Version = Version;
	Builder.Size = Version * 4 + 17;
	Builder.Dark.Init(false, Builder.Size * Builder.Size);
	Builder.Function.Init(false, Builder.Size * Builder.Size);
	Builder.FunctionPatterns();
	Builder.Codewords(Interleaved);
	int32 Best = 0;
	int32 BestPenalty = MAX_int32;
	for (int32 Mask = 0; Mask < 8; ++Mask)
	{
		Builder.ApplyMask(Mask);
		Builder.FormatInfo(Mask);
		const int32 Penalty = Builder.Penalty();
		if (Penalty < BestPenalty)
		{
			Best = Mask;
			BestPenalty = Penalty;
		}
		Builder.ApplyMask(Mask);
	}
	Builder.ApplyMask(Best);
	Builder.FormatInfo(Best);
	Out.Version = Version;
	Out.Size = Builder.Size;
	Out.Mask = Best;
	Out.Modules = MoveTemp(Builder.Dark);
	return true;
}

bool FHyperAIStudioQrCode::SavePng(const FString& Path, const int32 Scale, FString& OutError) const
{
	const int32 Border = 4;
	const int32 Side = (Size + Border * 2) * Scale;
	if (Size <= 0 || Scale <= 0)
	{
		OutError = TEXT("There is no code to draw.");
		return false;
	}
	TArray<FColor> Pixels;
	Pixels.Init(FColor::White, Side * Side);
	for (int32 Y = 0; Y < Side; ++Y)
	{
		for (int32 X = 0; X < Side; ++X)
		{
			if (IsDark(X / Scale - Border, Y / Scale - Border))
			{
				Pixels[Y * Side + X] = FColor::Black;
			}
		}
	}
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Side, Side, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	if (Png.IsEmpty() || !FFileHelper::SaveArrayToFile(Png, *Path))
	{
		OutError = FString::Printf(TEXT("Could not write %s."), *Path);
		return false;
	}
	return true;
}
