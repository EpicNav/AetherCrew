// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * A QR code (ISO/IEC 18004) of a short text such as a URL, so a phone's camera can open it: byte mode, error
 * correction level M, versions 1 to 10 (up to 213 bytes of UTF-8), the mask with the lowest penalty.
 */
struct FHyperAIStudioQrCode
{
	static constexpr int32 MaxVersion = 10;

	int32 Version = 0;
	/** Modules a side: 17 + 4 x Version. */
	int32 Size = 0;
	int32 Mask = -1;
	/** Size x Size, row by row from the top left; true is dark. */
	TArray<bool> Modules;

	bool IsDark(const int32 X, const int32 Y) const
	{
		return X >= 0 && Y >= 0 && X < Size && Y < Size && Modules[Y * Size + X];
	}

	/** Encodes Text as UTF-8 in the smallest version that holds it; false with OutError when it is too long. */
	static bool Encode(const FString& Text, FHyperAIStudioQrCode& Out, FString& OutError);

	/** Writes the code as a PNG, Scale pixels a module with the four-module quiet zone around it. */
	bool SavePng(const FString& Path, int32 Scale, FString& OutError) const;

	// The parts the tests check against the standard's published values.
	static TArray<uint8> ReedSolomonRemainder(const TArray<uint8>& Data, int32 Degree);
	/** The 15 format bits for an error-correction level's 2-bit code (L 1, M 0, Q 3, H 2) and a mask, masked with 0x5412. */
	static uint32 FormatBits(int32 LevelBits, int32 MaskPattern);
	/** The 18 version bits, for versions 7 and up. */
	static uint32 VersionBits(int32 InVersion);
	/** How many data codewords a version holds at level M. */
	static int32 DataCodewords(int32 InVersion);
};
