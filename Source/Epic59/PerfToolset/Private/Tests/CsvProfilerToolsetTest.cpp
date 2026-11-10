// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include <limits>

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "UObject/UnrealType.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#include "CsvProfilerToolset.h"
#include "CsvProfilerToolsetUtils.h"
#include "CSVProfilerUtils.h"

BEGIN_DEFINE_SPEC(FCsvProfilerToolsetSpec,
	"AIAssistant.Toolsets.CsvProfilerToolset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	// Fixtures are written under the project's Saved dir (AutomationTransientDir resolves there), one of
	// the toolset's allowed roots. The tools take the '/Saved/...' virtual form; the writers and the
	// CsvUtils-direct reads use the absolute path underneath.
	FString FixtureAbsDir() const
	{
		return FPaths::AutomationTransientDir();
	}

	FString FixtureVirtualPath(const FString& FileName) const
	{
		return FString(TEXT("/Saved/Automation/Tmp")) / FileName;
	}

	FString ToAbsolute(const FString& VirtualPath) const
	{
		return FixtureAbsDir() / FPaths::GetCleanFilename(VirtualPath);
	}

	// Writes a known text-CSV fixture and returns its virtual path. The header is the first line (no
	// header-at-end metadata), an "events" column carries ';'-separated event names per frame, and the
	// six frames have hand-chosen values the tests assert against.
	//   FrameTime: 10 20 30 40 50 60   (whole run: avg 35, min 10, max 60, total 210)
	//   GpuTime:    5 15 25 35 45 55
	//   events:    frame 1 = "Start", frame 3 = "End"
	FString WriteFixtureCsv()
	{
		const FString FileName = TEXT("CsvProfilerToolsetFixture.csv");

		const FString Contents =
			TEXT("FrameTime,GpuTime,events\n")
			TEXT("10,5,\n")
			TEXT("20,15,Start\n")
			TEXT("30,25,\n")
			TEXT("40,35,End\n")
			TEXT("50,45,\n")
			TEXT("60,55,\n");

		FFileHelper::SaveStringToFile(Contents, *(FixtureAbsDir() / FileName));
		return FixtureVirtualPath(FileName);
	}

	// Writes a large fixture with 1100 frames of a single stat. Generates values inline so no
	// external file is needed. Used to read a span far wider than the small fixtures hold.
	FString WriteFixtureCsvLarge()
	{
		const FString FileName = TEXT("CsvProfilerToolsetFixtureLarge.csv");

		FString Contents = TEXT("FrameTime\n");
		for (int32 i = 0; i < 1100; ++i)
		{
			Contents += FString::Printf(TEXT("%d\n"), (i % 100) + 1);
		}

		FFileHelper::SaveStringToFile(Contents, *(FixtureAbsDir() / FileName));
		return FixtureVirtualPath(FileName);
	}

	// As above but with no "events" column, so the capture has zero events.
	FString WriteFixtureCsvNoEvents()
	{
		const FString FileName = TEXT("CsvProfilerToolsetFixtureNoEvents.csv");

		const FString Contents =
			TEXT("FrameTime,GpuTime\n")
			TEXT("10,5\n")
			TEXT("20,15\n")
			TEXT("30,25\n");

		FFileHelper::SaveStringToFile(Contents, *(FixtureAbsDir() / FileName));
		return FixtureVirtualPath(FileName);
	}

	// As WriteFixtureCsv but with a trailing metadata line ([key],value,... with keys lowercased by
	// the reader). Carries one curated key (platform) and one non-curated key (commandline) so the
	// default-subset vs. full-metadata behaviour of GetCsvInfo can be told apart.
	FString WriteFixtureCsvWithMetadata()
	{
		const FString FileName = TEXT("CsvProfilerToolsetFixtureMetadata.csv");

		const FString Contents =
			TEXT("FrameTime,GpuTime\n")
			TEXT("10,5\n")
			TEXT("20,15\n")
			TEXT("30,25\n")
			TEXT("[platform],Windows,[commandline],-foo -bar baz\n");

		FFileHelper::SaveStringToFile(Contents, *(FixtureAbsDir() / FileName));
		return FixtureVirtualPath(FileName);
	}

	// A fixture whose events column repeats a name several times on one frame, so the same-frame
	// collapse and Count field of ListEvents can be exercised.
	//   frame 1 = "Flush" x3, frame 2 = "Mark" x1
	FString WriteFixtureCsvRepeatedEvents()
	{
		const FString FileName = TEXT("CsvProfilerToolsetFixtureRepeatedEvents.csv");

		const FString Contents =
			TEXT("FrameTime,events\n")
			TEXT("10,\n")
			TEXT("20,Flush;Flush;Flush\n")
			TEXT("30,Mark\n");

		FFileHelper::SaveStringToFile(Contents, *(FixtureAbsDir() / FileName));
		return FixtureVirtualPath(FileName);
	}

	// A fixture whose events column interleaves two names on one frame, so the collapse must key on the
	// name across the whole frame rather than merging only adjacent repeats.
	//   frame 1 = Load, Flush, Load, Flush, Load  ->  Load x3, Flush x2
	FString WriteFixtureCsvInterleavedEvents()
	{
		const FString FileName = TEXT("CsvProfilerToolsetFixtureInterleavedEvents.csv");

		const FString Contents =
			TEXT("FrameTime,events\n")
			TEXT("10,\n")
			TEXT("20,Load;Flush;Load;Flush;Load\n")
			TEXT("30,Mark\n");

		FFileHelper::SaveStringToFile(Contents, *(FixtureAbsDir() / FileName));
		return FixtureVirtualPath(FileName);
	}

	// A fixture whose stat names carry a '/' group prefix, so the group histogram (StatGroupOf) and
	// group-wildcard filtering are exercised; every other fixture has only ungrouped names.
	//   GPU/Basepass, GPU/Shadows -> group "GPU" (2); Game/Tick -> group "Game" (1)
	FString WriteFixtureCsvGrouped()
	{
		const FString FileName = TEXT("CsvProfilerToolsetFixtureGrouped.csv");

		const FString Contents =
			TEXT("GPU/Basepass,GPU/Shadows,Game/Tick\n")
			TEXT("10,5,1\n")
			TEXT("20,15,2\n")
			TEXT("30,25,3\n");

		FFileHelper::SaveStringToFile(Contents, *(FixtureAbsDir() / FileName));
		return FixtureVirtualPath(FileName);
	}

	// A fixture whose stat names nest three levels deep, so the GroupDepth roll-up can be exercised at
	// a depth that splits a shared prefix and at a depth deeper than some names reach.
	//   GPU/Basepass/Opaque, GPU/Basepass/Translucent, GPU/Shadows, Game/Tick
	// depth 1 -> GPU(3), Game(1); depth 2 -> GPU/Basepass(2), GPU(1), Game(1). Depth 3 matches depth 2:
	// no name has a third group level, and the leaf metric is never its own group.
	FString WriteFixtureCsvNested()
	{
		const FString FileName = TEXT("CsvProfilerToolsetFixtureNested.csv");

		const FString Contents =
			TEXT("GPU/Basepass/Opaque,GPU/Basepass/Translucent,GPU/Shadows,Game/Tick\n")
			TEXT("10,5,3,1\n")
			TEXT("20,15,6,2\n")
			TEXT("30,25,9,3\n");

		FFileHelper::SaveStringToFile(Contents, *(FixtureAbsDir() / FileName));
		return FixtureVirtualPath(FileName);
	}

	// Two stats whose ranking order depends on the measure, so the Max axis can be told apart from the
	// Average/Total axes (which coincide, since Total is Average times the shared frame count).
	//   Spike:  0 0 0 0 0 90   (avg 15, max 90, total 90)
	//   Steady: 30 x6          (avg 30, max 30, total 180)
	// Average/Total rank Steady first; Max ranks Spike first.
	FString WriteFixtureCsvRankSkew()
	{
		const FString FileName = TEXT("CsvProfilerToolsetFixtureRankSkew.csv");

		const FString Contents =
			TEXT("Spike,Steady\n")
			TEXT("0,30\n")
			TEXT("0,30\n")
			TEXT("0,30\n")
			TEXT("0,30\n")
			TEXT("0,30\n")
			TEXT("90,30\n");

		FFileHelper::SaveStringToFile(Contents, *(FixtureAbsDir() / FileName));
		return FixtureVirtualPath(FileName);
	}

	void DeleteFixture(const FString& Path)
	{
		IFileManager::Get().Delete(*ToAbsolute(Path), false, true, true);
	}

	// Runs a toolset call and returns the script error it raised (empty if none). A toolset method
	// called directly from C++ has no Blueprints stack frame, so RaiseScriptError is silent unless
	// wrapped; CaptureErrorsIn supplies that frame and records the raised message.
	FString CaptureToolError(TFunction<void()>&& Func)
	{
		UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
		Handler.CaptureErrorsIn(MoveTemp(Func));
		return Handler.GetException();
	}

	// Appends a 7-bit length-prefixed UTF-8 string to Bytes, mirroring C# BinaryWriter.Write(string).
	void WritePrefixedString(TArray<uint8>& Bytes, const char* Str)
	{
		uint32 Len = (uint32)FCStringAnsi::Strlen(Str);

		// Length as a 7-bit variable-length integer: low 7 bits per byte, high bit flags a continuation.
		uint32 Remaining = Len;
		do
		{
			uint8 ByteVal = Remaining & 0x7F;
			Remaining >>= 7;
			if (Remaining > 0)
			{
				ByteVal |= 0x80;
			}
			Bytes.Add(ByteVal);
		} while (Remaining > 0);

		for (uint32 i = 0; i < Len; ++i)
		{
			Bytes.Add((uint8)Str[i]);
		}
	}

	// Writes a little-endian int32 to Bytes.
	void WriteInt32(TArray<uint8>& Bytes, int32 Value)
	{
		Bytes.Add((uint8)(Value & 0xFF));
		Bytes.Add((uint8)((Value >> 8) & 0xFF));
		Bytes.Add((uint8)((Value >> 16) & 0xFF));
		Bytes.Add((uint8)((Value >> 24) & 0xFF));
	}

	// Writes a little-endian uint32 to Bytes.
	void WriteUInt32(TArray<uint8>& Bytes, uint32 Value)
	{
		Bytes.Add((uint8)(Value & 0xFF));
		Bytes.Add((uint8)((Value >> 8) & 0xFF));
		Bytes.Add((uint8)((Value >> 16) & 0xFF));
		Bytes.Add((uint8)((Value >> 24) & 0xFF));
	}

	// Writes a little-endian double (8 bytes) to Bytes.
	void WriteDouble(TArray<uint8>& Bytes, double Value)
	{
		uint64 Bits;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		for (int32 Shift = 0; Shift < 64; Shift += 8)
		{
			Bytes.Add((uint8)((Bits >> Shift) & 0xFF));
		}
	}

	// Builds a minimal CSVBIN blob: zero stats, EventCount events.
	// Version must be 3 (CompressionSupportAndFlags) or 4 (EventTimestamps).
	// For v4, Timestamps must have exactly EventCount entries.
	FString WriteFixtureCsvBin(int32 Version, TConstArrayView<int32> Frames, TConstArrayView<FString> Names, TConstArrayView<double> Timestamps, bool bTruncateLastTimestamp = false)
	{
		const FString FileName = TEXT("CsvProfilerToolsetFixture.csv.bin");

		TArray<uint8> Bytes;

		// Magic: 7-bit prefixed "CSVBIN"
		WritePrefixedString(Bytes, "CSVBIN");

		// Version (int32)
		WriteInt32(Bytes, Version);

		// Flags (uint32): no metadata = 0
		WriteUInt32(Bytes, 0);

		// Compression level byte: 1 = Min (non-None so the reader takes the compressed path)
		Bytes.Add(1);

		// Compression type byte: 0 = MsDeflate
		Bytes.Add(0);

		// Counts: eventCount, valueCount (0), statCount (0)
		WriteInt32(Bytes, Frames.Num());
		WriteInt32(Bytes, 0);
		WriteInt32(Bytes, 0);

		// Per-event: frame (int32), name (prefixed string), timestamp (double, v4 only)
		for (int32 i = 0; i < Frames.Num(); ++i)
		{
			WriteInt32(Bytes, Frames[i]);
			WritePrefixedString(Bytes, TCHAR_TO_UTF8(*Names[i]));

			if (Version >= 4)
			{
				const bool bLastEvent = (i == Frames.Num() - 1);
				if (bTruncateLastTimestamp && bLastEvent)
				{
					// Write only 4 of the 8 bytes so the archive hits EOF mid-timestamp.
					double Ts = Timestamps[i];
					uint64 Bits;
					FMemory::Memcpy(&Bits, &Ts, sizeof(Bits));
					Bytes.Add((uint8)(Bits & 0xFF));
					Bytes.Add((uint8)((Bits >> 8) & 0xFF));
					Bytes.Add((uint8)((Bits >> 16) & 0xFF));
					Bytes.Add((uint8)((Bits >> 24) & 0xFF));
				}
				else
				{
					WriteDouble(Bytes, Timestamps[i]);
				}
			}
		}

		FFileHelper::SaveArrayToFile(Bytes, *(FixtureAbsDir() / FileName));
		return FixtureVirtualPath(FileName);
	}

END_DEFINE_SPEC(FCsvProfilerToolsetSpec)

void FCsvProfilerToolsetSpec::Define()
{
	Describe("GetCsvInfo", [this]()
	{
		It(TEXT("returns count, group histogram, and event names but no stat names by default"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerInfoResult Info = UCsvProfilerToolset::GetCsvInfo(Path);

			TestEqual(TEXT("frame count"), Info.FrameCount, 6);
			TestEqual(TEXT("stat count"), Info.StatCount, 2);

			// Neither fixture stat has a '/', so both bucket under "(ungrouped)".
			const int32* Ungrouped = Info.StatGroups.Find(TEXT("(ungrouped)"));
			TestNotNull(TEXT("ungrouped bucket present"), Ungrouped);
			if (Ungrouped != nullptr)
			{
				TestEqual(TEXT("two ungrouped stats"), *Ungrouped, 2);
			}

			TestEqual(TEXT("no stat names without a filter"), Info.StatNames.Num(), 0);
			TestTrue(TEXT("has Start event"), Info.EventNames.Contains(TEXT("Start")));
			TestTrue(TEXT("has End event"), Info.EventNames.Contains(TEXT("End")));
		});

		It(TEXT("buckets stats by their slash-prefixed group"), [this]()
		{
			const FString Path = WriteFixtureCsvGrouped();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// Depth 1 buckets under the top '/'-segment (GPU, Game).
			const FCsvProfilerInfoResult Info =
				UCsvProfilerToolset::GetCsvInfo(Path, TEXT(""), /*bIncludeAllMetadata=*/false, /*GroupDepth=*/1);

			TestEqual(TEXT("three stats"), Info.StatCount, 3);

			const int32* Gpu = Info.StatGroups.Find(TEXT("GPU"));
			const int32* Game = Info.StatGroups.Find(TEXT("Game"));
			TestNotNull(TEXT("GPU group present"), Gpu);
			TestNotNull(TEXT("Game group present"), Game);
			if (Gpu != nullptr)
			{
				TestEqual(TEXT("two GPU stats"), *Gpu, 2);
			}
			if (Game != nullptr)
			{
				TestEqual(TEXT("one Game stat"), *Game, 1);
			}
		});

		It(TEXT("rolls the group histogram up to the requested depth"), [this]()
		{
			const FString Path = WriteFixtureCsvNested();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerInfoResult Info =
				UCsvProfilerToolset::GetCsvInfo(Path, TEXT(""), /*bIncludeAllMetadata=*/false, /*GroupDepth=*/2);

			// Depth 2 splits the three-segment names into "GPU/Basepass"; the two-segment names have no
			// second group level, so their leaf drops and they roll up to "GPU" and "Game".
			const int32* Basepass = Info.StatGroups.Find(TEXT("GPU/Basepass"));
			const int32* Gpu = Info.StatGroups.Find(TEXT("GPU"));
			const int32* Game = Info.StatGroups.Find(TEXT("Game"));
			TestNotNull(TEXT("GPU/Basepass group present"), Basepass);
			TestNotNull(TEXT("GPU group present"), Gpu);
			TestNotNull(TEXT("Game group present"), Game);
			if (Basepass != nullptr)
			{
				TestEqual(TEXT("two GPU/Basepass stats"), *Basepass, 2);
			}
			if (Gpu != nullptr)
			{
				TestEqual(TEXT("one GPU stat from the Shadows leaf"), *Gpu, 1);
			}
			if (Game != nullptr)
			{
				TestEqual(TEXT("one Game stat from the Tick leaf"), *Game, 1);
			}
			TestFalse(TEXT("leaf GPU/Shadows never keyed as a group"), Info.StatGroups.Contains(TEXT("GPU/Shadows")));
			TestFalse(TEXT("leaf Game/Tick never keyed as a group"), Info.StatGroups.Contains(TEXT("Game/Tick")));
		});

		It(TEXT("never keys a leaf metric as its own group, even past the deepest name"), [this]()
		{
			const FString Path = WriteFixtureCsvNested();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerInfoResult Info =
				UCsvProfilerToolset::GetCsvInfo(Path, TEXT(""), /*bIncludeAllMetadata=*/false, /*GroupDepth=*/3);

			// No name reaches a third group level, so depth 3 matches depth 2: the three-segment leaves
			// cap at their "GPU/Basepass" parent rather than keying whole.
			const int32* Basepass = Info.StatGroups.Find(TEXT("GPU/Basepass"));
			TestNotNull(TEXT("GPU/Basepass group present"), Basepass);
			if (Basepass != nullptr)
			{
				TestEqual(TEXT("two GPU/Basepass stats"), *Basepass, 2);
			}
			TestFalse(TEXT("Opaque leaf never keyed whole"), Info.StatGroups.Contains(TEXT("GPU/Basepass/Opaque")));
			TestFalse(TEXT("Translucent leaf never keyed whole"), Info.StatGroups.Contains(TEXT("GPU/Basepass/Translucent")));
		});

		It(TEXT("treats a depth below 1 as depth 1"), [this]()
		{
			const FString Path = WriteFixtureCsvNested();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerInfoResult Info =
				UCsvProfilerToolset::GetCsvInfo(Path, TEXT(""), /*bIncludeAllMetadata=*/false, /*GroupDepth=*/0);

			const int32* Gpu = Info.StatGroups.Find(TEXT("GPU"));
			TestNotNull(TEXT("GPU top-level group present"), Gpu);
			if (Gpu != nullptr)
			{
				TestEqual(TEXT("three GPU stats at clamped depth 1"), *Gpu, 3);
			}
		});

		It(TEXT("lists only the stat names matching a filter"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerInfoResult Info = UCsvProfilerToolset::GetCsvInfo(Path, TEXT("Frame*"));

			TestEqual(TEXT("one name matches"), Info.StatNames.Num(), 1);
			TestTrue(TEXT("FrameTime matched"), Info.StatNames.Contains(TEXT("FrameTime")));
			TestFalse(TEXT("GpuTime not matched"), Info.StatNames.Contains(TEXT("GpuTime")));
			TestEqual(TEXT("stat count still total"), Info.StatCount, 2);
		});

		It(TEXT("returns the curated metadata subset by default and the full block on request"), [this]()
		{
			const FString Path = WriteFixtureCsvWithMetadata();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerInfoResult Curated = UCsvProfilerToolset::GetCsvInfo(Path);
			TestTrue(TEXT("curated keeps platform"), Curated.Metadata.Contains(TEXT("platform")));
			TestFalse(TEXT("curated drops commandline"), Curated.Metadata.Contains(TEXT("commandline")));

			const FCsvProfilerInfoResult Full = UCsvProfilerToolset::GetCsvInfo(Path, TEXT(""), /*bIncludeAllMetadata=*/true);
			TestTrue(TEXT("full keeps platform"), Full.Metadata.Contains(TEXT("platform")));
			TestTrue(TEXT("full keeps commandline"), Full.Metadata.Contains(TEXT("commandline")));
		});

		It(TEXT("carries no per-frame values field"), [this]()
		{
			// Reflection guard: the no-raw-bytes invariant requires FCsvProfilerInfoResult to expose no
			// per-frame value array. If a TArray<float> field is ever added, this fails.
			const UScriptStruct* Struct = FCsvProfilerInfoResult::StaticStruct();
			TestNotNull(TEXT("FCsvProfilerInfoResult struct is valid"), Struct);

			bool bHasFloatArray = false;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(*It))
				{
					if (ArrayProp->Inner != nullptr && ArrayProp->Inner->IsA<FFloatProperty>())
					{
						bHasFloatArray = true;
					}
				}
			}

			TestFalse(TEXT("FCsvProfilerInfoResult has no TArray<float> field"), bHasFloatArray);
		});

		It(TEXT("raises for a missing file"), [this]()
		{
			const FString Path = FixtureVirtualPath(TEXT("CsvProfilerToolsetDoesNotExist.csv"));

			const FString Error = CaptureToolError([&]() { UCsvProfilerToolset::GetCsvInfo(Path); });

			TestFalse(TEXT("error raised"), Error.IsEmpty());
		});
	});

	Describe("Path gating", [this]()
	{
		It(TEXT("raises for a non-virtual filesystem path"), [this]()
		{
			// An absolute OS path is not a virtual content path, so it is rejected before any read.
			const FString Path = FPaths::ConvertRelativePathToFull(FixtureAbsDir() / TEXT("CsvProfilerToolsetFixture.csv"));

			const FString Error = CaptureToolError([&]() { UCsvProfilerToolset::GetCsvInfo(Path); });

			TestFalse(TEXT("error raised"), Error.IsEmpty());
		});

		It(TEXT("raises for a virtual path outside the allowed roots"), [this]()
		{
			// '/Engine/' resolves through the engine mount table but is not one of the allowed roots
			// (project Content, project Saved, or a plugin's Content), so the gate rejects it.
			const FString Error = CaptureToolError([&]() { UCsvProfilerToolset::GetCsvInfo(TEXT("/Engine/CsvProfilerToolsetOutsideRoot.csv")); });

			TestFalse(TEXT("error raised"), Error.IsEmpty());
		});
	});

	Describe("ReduceStat", [this]()
	{
		It(TEXT("computes avg/min/max/total over the whole run from the values"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerReduceResult Result = UCsvProfilerToolset::ReduceStat(Path, { TEXT("FrameTime") });

			const FCsvProfilerStatReduction* Reduction = Result.Stats.Find(TEXT("FrameTime"));
			TestNotNull(TEXT("FrameTime reduction present"), Reduction);
			if (Reduction != nullptr)
			{
				TestEqual(TEXT("average"), Reduction->Average, 35.0);
				TestEqual(TEXT("min"), Reduction->Min, 10.f);
				TestEqual(TEXT("max"), Reduction->Max, 60.f);
				TestEqual(TEXT("total"), Reduction->Total, 210.0);

				// FrameTime 10 20 30 40 50 60 (N=6): P50 at rank 3, the tail percentiles at rank 6.
				TestEqual(TEXT("p50"), Reduction->P50, 30.f);
				TestEqual(TEXT("p90"), Reduction->P90, 60.f);
				TestEqual(TEXT("p95"), Reduction->P95, 60.f);
				TestEqual(TEXT("p99"), Reduction->P99, 60.f);
			}
		});

		It(TEXT("computes aggregates over a frame sub-range"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// [1, 4) -> FrameTime values 20, 30, 40.
			const FCsvProfilerReduceResult Result = UCsvProfilerToolset::ReduceStat(Path, { TEXT("FrameTime") }, 1, 4);

			const FCsvProfilerStatReduction* Reduction = Result.Stats.Find(TEXT("FrameTime"));
			TestNotNull(TEXT("FrameTime reduction present"), Reduction);
			if (Reduction != nullptr)
			{
				TestEqual(TEXT("average"), Reduction->Average, 30.0);
				TestEqual(TEXT("min"), Reduction->Min, 20.f);
				TestEqual(TEXT("max"), Reduction->Max, 40.f);
				TestEqual(TEXT("total"), Reduction->Total, 90.0);

				// [1, 4) is 20 30 40 (N=3): P50 at rank 2, the tail percentiles at rank 3.
				TestEqual(TEXT("p50"), Reduction->P50, 30.f);
				TestEqual(TEXT("p90"), Reduction->P90, 40.f);
				TestEqual(TEXT("p95"), Reduction->P95, 40.f);
				TestEqual(TEXT("p99"), Reduction->P99, 40.f);
			}
		});

		It(TEXT("treats FrameEnd -1 as running to the end"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// FrameTime is 10 20 30 40 50 60. (3, -1) is the tail 40 50 60.
			const FCsvProfilerReduceResult OpenEnd = UCsvProfilerToolset::ReduceStat(Path, { TEXT("FrameTime") }, 3, -1);

			const FCsvProfilerStatReduction* Tail = OpenEnd.Stats.Find(TEXT("FrameTime"));
			TestNotNull(TEXT("open-end reduction present"), Tail);
			if (Tail != nullptr)
			{
				TestEqual(TEXT("open end totals the tail"), Tail->Total, 150.0);
				TestEqual(TEXT("open end min"), Tail->Min, 40.f);
			}
		});

		It(TEXT("yields different aggregates for different ranges"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerReduceResult A = UCsvProfilerToolset::ReduceStat(Path, { TEXT("FrameTime") }, 0, 3);
			const FCsvProfilerReduceResult B = UCsvProfilerToolset::ReduceStat(Path, { TEXT("FrameTime") }, 3, 6);

			const FCsvProfilerStatReduction* RA = A.Stats.Find(TEXT("FrameTime"));
			const FCsvProfilerStatReduction* RB = B.Stats.Find(TEXT("FrameTime"));
			TestNotNull(TEXT("range A present"), RA);
			TestNotNull(TEXT("range B present"), RB);
			if (RA != nullptr && RB != nullptr)
			{
				TestNotEqual(TEXT("totals differ between ranges"), RA->Total, RB->Total);
			}
		});

		It(TEXT("returns one reduction per requested stat"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerReduceResult Result = UCsvProfilerToolset::ReduceStat(Path, { TEXT("FrameTime"), TEXT("GpuTime") });

			TestEqual(TEXT("two reductions"), Result.Stats.Num(), 2);
			TestTrue(TEXT("FrameTime keyed"), Result.Stats.Contains(TEXT("FrameTime")));
			TestTrue(TEXT("GpuTime keyed"), Result.Stats.Contains(TEXT("GpuTime")));
		});

		It(TEXT("omits an absent stat without failing the call"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerReduceResult Result = UCsvProfilerToolset::ReduceStat(Path, { TEXT("FrameTime"), TEXT("NoSuchStat") });

			TestNotNull(TEXT("present stat still resolves"), Result.Stats.Find(TEXT("FrameTime")));
			TestNull(TEXT("absent stat omitted"), Result.Stats.Find(TEXT("NoSuchStat")));
			TestEqual(TEXT("only the present stat is returned"), Result.Stats.Num(), 1);
		});

		It(TEXT("rejects invalid ranges by raising"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const TArray<FString> Stats = { TEXT("FrameTime") };

			TestFalse(TEXT("negative start (the old -1 open sentinel is now rejected)"), CaptureToolError([&]() { UCsvProfilerToolset::ReduceStat(Path, Stats, -1, 3); }).IsEmpty());
			TestFalse(TEXT("end <= start"), CaptureToolError([&]() { UCsvProfilerToolset::ReduceStat(Path, Stats, 3, 3); }).IsEmpty());
			TestFalse(TEXT("end > count"), CaptureToolError([&]() { UCsvProfilerToolset::ReduceStat(Path, Stats, 0, 7); }).IsEmpty());
			TestFalse(TEXT("start >= count"), CaptureToolError([&]() { UCsvProfilerToolset::ReduceStat(Path, Stats, 6, 6); }).IsEmpty());
		});

	});

	Describe("ReadStatValues", [this]()
	{
		It(TEXT("reads Count frames starting at FrameStart"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// FrameTime is 10 20 30 40 50 60; start 1, count 3 -> frames 1..3 = 20, 30, 40, window [1, 4).
			const FCsvProfilerStatValues Result = UCsvProfilerToolset::ReadStatValues(Path, TEXT("FrameTime"), 1, 3);

			TestEqual(TEXT("reported window start"), Result.FrameStart, 1);
			TestEqual(TEXT("reported window end"), Result.FrameEnd, 4);
			TestEqual(TEXT("window matches value count"), Result.FrameEnd - Result.FrameStart, Result.Values.Num());
			TestEqual(TEXT("three values"), Result.Values.Num(), 3);
			if (Result.Values.Num() == 3)
			{
				TestEqual(TEXT("value 0"), Result.Values[0], 20.f);
				TestEqual(TEXT("value 1"), Result.Values[1], 30.f);
				TestEqual(TEXT("value 2"), Result.Values[2], 40.f);
			}
		});

		It(TEXT("returns different values for different windows"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerStatValues A = UCsvProfilerToolset::ReadStatValues(Path, TEXT("FrameTime"), 0, 2);
			const FCsvProfilerStatValues B = UCsvProfilerToolset::ReadStatValues(Path, TEXT("FrameTime"), 4, 2);

			TestEqual(TEXT("A returned its window"), A.Values.Num(), 2);
			TestEqual(TEXT("B returned its window"), B.Values.Num(), 2);
			if (A.Values.Num() > 0 && B.Values.Num() > 0)
			{
				TestNotEqual(TEXT("first value differs"), A.Values[0], B.Values[0]);
			}
		});

		It(TEXT("reads to the end of the column when Count is -1"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// Count -1 is the explicit whole-column read: the six FrameTime values from frame 0, window [0, 6).
			const FCsvProfilerStatValues Result = UCsvProfilerToolset::ReadStatValues(Path, TEXT("FrameTime"), 0, -1);

			TestEqual(TEXT("reads every frame"), Result.Values.Num(), 6);
			TestEqual(TEXT("window start"), Result.FrameStart, 0);
			TestEqual(TEXT("window end"), Result.FrameEnd, 6);
			if (Result.Values.Num() == 6)
			{
				TestEqual(TEXT("first value"), Result.Values[0], 10.f);
				TestEqual(TEXT("last value"), Result.Values[5], 60.f);
			}
		});

		It(TEXT("defaults to a bounded window and clamps an over-large Count to the column"), [this]()
		{
			// The large fixture has 1100 frames. The default Count returns a bounded head window rather
			// than the whole column; an explicit Count past the column clamps; a tail window clamps to what
			// remains. None of these raise.
			const FString Path = WriteFixtureCsvLarge();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerStatValues Defaulted = UCsvProfilerToolset::ReadStatValues(Path, TEXT("FrameTime"));
			TestEqual(TEXT("default bounds the window to 256 frames"), Defaulted.Values.Num(), 256);
			TestEqual(TEXT("default window starts at 0"), Defaulted.FrameStart, 0);
			TestEqual(TEXT("default window ends at 256"), Defaulted.FrameEnd, 256);

			const FCsvProfilerStatValues Clamped = UCsvProfilerToolset::ReadStatValues(Path, TEXT("FrameTime"), 0, 5000);
			TestEqual(TEXT("over-large Count clamps to the column"), Clamped.Values.Num(), 1100);

			const FCsvProfilerStatValues Tail = UCsvProfilerToolset::ReadStatValues(Path, TEXT("FrameTime"), 1000, 256);
			TestEqual(TEXT("tail window clamps to what remains"), Tail.Values.Num(), 100);
			TestEqual(TEXT("tail window end is the column length"), Tail.FrameEnd, 1100);
		});

		It(TEXT("raises for an absent stat"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FString Error = CaptureToolError([&]() { UCsvProfilerToolset::ReadStatValues(Path, TEXT("NoSuchStat"), 0, 3); });

			TestFalse(TEXT("error raised"), Error.IsEmpty());
		});

		It(TEXT("rejects a malformed request by raising"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// A negative start, a zero Count, or a Count below the -1 whole-run sentinel are all invalid;
			// an over-large positive Count is not (it clamps, covered above).
			TestFalse(TEXT("negative start"), CaptureToolError([&]() { UCsvProfilerToolset::ReadStatValues(Path, TEXT("FrameTime"), -1, 3); }).IsEmpty());
			TestFalse(TEXT("zero count"), CaptureToolError([&]() { UCsvProfilerToolset::ReadStatValues(Path, TEXT("FrameTime"), 0, 0); }).IsEmpty());
			TestFalse(TEXT("count below -1"), CaptureToolError([&]() { UCsvProfilerToolset::ReadStatValues(Path, TEXT("FrameTime"), 0, -2); }).IsEmpty());
		});

		It(TEXT("raises when the start is at or past the stat's column length"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// A start past the column copies no values, so the covered end never advances past the start.
			const FString Error = CaptureToolError([&]() { UCsvProfilerToolset::ReadStatValues(Path, TEXT("FrameTime"), 10, 3); });

			TestFalse(TEXT("error raised"), Error.IsEmpty());
		});
	});

	// ReadStatValues clamps a requested end down to the stat's own column length, guarding a short
	// column. Both readers produce full-length columns today (text pads every column to the row count;
	// binary sizes every sample to one shared value count), so that clamp is unreachable through any
	// fixture. Exercise the SliceStatValues helper directly to pin its invariant: the covered end it
	// returns always matches the values it copies, so a caller's [FrameStart, FrameEnd) never overstates
	// the data.
	Describe("SliceStatValues short-column clamp", [this]()
	{
		It(TEXT("clamps the end to the column length and covers exactly the values returned"), [this]()
		{
			using namespace UE::CsvProfilerToolset;

			const TArray<float> ShortColumn = { 10.f, 20.f, 30.f };
			TArray<float> Out;
			const int32 CoveredEnd = SliceStatValues(ShortColumn, 1, 6, Out);

			TestEqual(TEXT("end clamped to column length"), CoveredEnd, 3);
			TestEqual(TEXT("window matches value count"), CoveredEnd, 1 + Out.Num());
			TestEqual(TEXT("two values covered"), Out.Num(), 2);
			if (Out.Num() == 2)
			{
				TestEqual(TEXT("first covered value"), Out[0], 20.f);
				TestEqual(TEXT("second covered value"), Out[1], 30.f);
			}
		});

		It(TEXT("copies nothing when the start is already at or past the column"), [this]()
		{
			using namespace UE::CsvProfilerToolset;

			const TArray<float> ShortColumn = { 10.f, 20.f };
			TArray<float> Out;
			const int32 CoveredEnd = SliceStatValues(ShortColumn, 2, 5, Out);

			TestTrue(TEXT("covered end does not exceed start"), CoveredEnd <= 2);
			TestEqual(TEXT("no values copied"), Out.Num(), 0);
		});
	});

	// TopStats ranks via RanksAheadDescending rather than a raw '>' so the sort predicate stays a strict
	// weak ordering even if an aggregate is NaN. Pin the ordering directly: finite keys sort highest-first
	// and NaN sinks to the bottom, with equal and NaN-vs-NaN pairs unordered.
	Describe("RanksAheadDescending NaN-safe ranking", [this]()
	{
		It(TEXT("orders finite keys highest-first and sinks NaN to the bottom"), [this]()
		{
			using namespace UE::CsvProfilerToolset;
			const double NaN = std::numeric_limits<double>::quiet_NaN();

			TestTrue(TEXT("higher key ranks ahead"), RanksAheadDescending(5.0, 1.0));
			TestFalse(TEXT("lower key does not rank ahead"), RanksAheadDescending(1.0, 5.0));
			TestFalse(TEXT("equal keys are unordered"), RanksAheadDescending(3.0, 3.0));
			TestTrue(TEXT("finite ranks ahead of NaN"), RanksAheadDescending(0.0, NaN));
			TestFalse(TEXT("NaN does not rank ahead of finite"), RanksAheadDescending(NaN, 0.0));
			TestFalse(TEXT("NaN vs NaN is unordered"), RanksAheadDescending(NaN, NaN));
		});
	});

	// Percentiles are nearest-rank via NearestRankPercentile: the value at 1-based rank ceil(Fraction*N),
	// an actually observed sample. Pin the rank arithmetic, the clamped 0/1 ends, and NaN handling.
	Describe("NearestRankPercentile", [this]()
	{
		It(TEXT("selects the value at the nearest rank, clamping the 0 and 1 ends"), [this]()
		{
			using namespace UE::CsvProfilerToolset;

			const TArray<float> Sorted = { 10.f, 20.f, 30.f, 40.f, 50.f, 60.f, 70.f, 80.f, 90.f, 100.f };

			TestEqual(TEXT("p50"), NearestRankPercentile(Sorted, 0.50), 50.f);
			TestEqual(TEXT("p90"), NearestRankPercentile(Sorted, 0.90), 90.f);
			TestEqual(TEXT("p95"), NearestRankPercentile(Sorted, 0.95), 100.f);
			TestEqual(TEXT("p99"), NearestRankPercentile(Sorted, 0.99), 100.f);
			TestEqual(TEXT("fraction 0 clamps to the minimum"), NearestRankPercentile(Sorted, 0.0), 10.f);
			TestEqual(TEXT("fraction 1 is the maximum"), NearestRankPercentile(Sorted, 1.0), 100.f);
		});

		It(TEXT("sinks NaN so a slice with NaN still ranks the finite values"), [this]()
		{
			using namespace UE::CsvProfilerToolset;
			const float NaN = std::numeric_limits<float>::quiet_NaN();

			TArray<float> Values = { 30.f, NaN, 10.f, 20.f };
			Values.Sort(RanksBeforeAscending);

			// Finite values sort ascending (10 20 30) with NaN sunk last; the median rank lands on 20.
			TestEqual(TEXT("smallest finite value first"), Values[0], 10.f);
			TestEqual(TEXT("p50 over the finite head"), NearestRankPercentile(Values, 0.50), 20.f);
		});
	});

	Describe("ListEvents", [this]()
	{
		It(TEXT("returns each event with its frame index"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerEventList Result = UCsvProfilerToolset::ListEvents(Path);

			TestEqual(TEXT("two events"), Result.Events.Num(), 2);

			bool bStartAt1 = false;
			bool bEndAt3 = false;
			for (const FCsvProfilerEventEntry& Entry : Result.Events)
			{
				if (Entry.Name == TEXT("Start") && Entry.Frame == 1)
				{
					bStartAt1 = true;
				}
				if (Entry.Name == TEXT("End") && Entry.Frame == 3)
				{
					bEndAt3 = true;
				}
			}
			TestTrue(TEXT("Start at frame 1"), bStartAt1);
			TestTrue(TEXT("End at frame 3"), bEndAt3);
		});

		It(TEXT("returns an empty list for a capture with no events"), [this]()
		{
			const FString Path = WriteFixtureCsvNoEvents();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerEventList Result = UCsvProfilerToolset::ListEvents(Path);

			TestEqual(TEXT("no events"), Result.Events.Num(), 0);
		});

		It(TEXT("returns whole-run events for a capture with events but no stat columns"), [this]()
		{
			// A CSVBIN with zero stats but two events: FrameCountOf() is 0, so a whole-run window bounded
			// by the stat frame count would wrongly drop every event. Whole-run must still return them.
			const TArray<int32> Frames = { 0, 1 };
			const TArray<FString> Names = { TEXT("Alpha"), TEXT("Beta") };
			const TArray<double> Timestamps = { 1.5, 2.75 };
			const FString Path = WriteFixtureCsvBin(4, Frames, Names, Timestamps);
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerEventList Result = UCsvProfilerToolset::ListEvents(Path);

			TestEqual(TEXT("both events returned despite no stats"), Result.Events.Num(), 2);
		});

		It(TEXT("raises for a missing file"), [this]()
		{
			const FString Path = FixtureVirtualPath(TEXT("CsvProfilerToolsetListEventsMissing.csv"));

			const FString Error = CaptureToolError([&]() { UCsvProfilerToolset::ListEvents(Path); });

			TestFalse(TEXT("error raised"), Error.IsEmpty());
		});

		It(TEXT("collapses repeats of the same event on the same frame into one entry with a count"), [this]()
		{
			const FString Path = WriteFixtureCsvRepeatedEvents();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerEventList Result = UCsvProfilerToolset::ListEvents(Path);

			TestEqual(TEXT("two collapsed entries"), Result.Events.Num(), 2);

			for (const FCsvProfilerEventEntry& Entry : Result.Events)
			{
				if (Entry.Name == TEXT("Flush"))
				{
					TestEqual(TEXT("Flush on frame 1"), Entry.Frame, 1);
					TestEqual(TEXT("Flush fired three times"), Entry.Count, 3);
				}
				if (Entry.Name == TEXT("Mark"))
				{
					TestEqual(TEXT("Mark fired once"), Entry.Count, 1);
				}
			}
		});

		It(TEXT("collapses interleaved repeats of the same event on one frame by name, not adjacency"), [this]()
		{
			const FString Path = WriteFixtureCsvInterleavedEvents();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerEventList Result = UCsvProfilerToolset::ListEvents(Path);

			// Load and Flush interleave on frame 1; each collapses to one entry, then Mark on frame 2.
			TestEqual(TEXT("three collapsed entries"), Result.Events.Num(), 3);

			for (const FCsvProfilerEventEntry& Entry : Result.Events)
			{
				if (Entry.Name == TEXT("Load"))
				{
					TestEqual(TEXT("Load on frame 1"), Entry.Frame, 1);
					TestEqual(TEXT("Load fired three times"), Entry.Count, 3);
				}
				if (Entry.Name == TEXT("Flush"))
				{
					TestEqual(TEXT("Flush on frame 1"), Entry.Frame, 1);
					TestEqual(TEXT("Flush fired twice"), Entry.Count, 2);
				}
				if (Entry.Name == TEXT("Mark"))
				{
					TestEqual(TEXT("Mark fired once"), Entry.Count, 1);
				}
			}

			// First-seen order within the frame is preserved: Load appears before Flush.
			if (Result.Events.Num() == 3)
			{
				TestEqual(TEXT("Load kept first-seen ahead of Flush"), Result.Events[0].Name, FString(TEXT("Load")));
				TestEqual(TEXT("Flush second"), Result.Events[1].Name, FString(TEXT("Flush")));
			}
		});

		It(TEXT("restricts events to a frame range"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// Whole-run events are Start@1 and End@3; [2, 6) excludes Start and keeps End.
			const FCsvProfilerEventList Result = UCsvProfilerToolset::ListEvents(Path, 256, 2, 6);

			TestEqual(TEXT("one event in range"), Result.Events.Num(), 1);
			if (Result.Events.Num() == 1)
			{
				TestEqual(TEXT("kept End"), Result.Events[0].Name, FString(TEXT("End")));
			}
		});

		It(TEXT("returns at most the requested Count and reports the full considered count"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// The fixture has two events (Start@1, End@3); a Count of 1 returns one, ConsideredCount two.
			const FCsvProfilerEventList Result = UCsvProfilerToolset::ListEvents(Path, 1);

			TestEqual(TEXT("one event returned"), Result.Events.Num(), 1);
			TestEqual(TEXT("both events considered"), Result.ConsideredCount, 2);
		});

		It(TEXT("bounds the result by the Count default but honors a larger explicit Count"), [this]()
		{
			// More distinct events than the Count default (256): each on its own frame with a unique
			// name so none collapse.
			const int32 DefaultCount = 256;
			const int32 EventCount = 300;
			TArray<int32> Frames;
			TArray<FString> Names;
			const TArray<double> NoTimestamps;
			for (int32 Index = 0; Index < EventCount; ++Index)
			{
				Frames.Add(Index);
				Names.Add(FString::Printf(TEXT("Evt%d"), Index));
			}

			const FString Path = WriteFixtureCsvBin(3, Frames, Names, NoTimestamps);
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// Default Count bounds the result; ConsideredCount always reports the full total.
			const FCsvProfilerEventList Defaulted = UCsvProfilerToolset::ListEvents(Path);
			TestEqual(TEXT("default Count bounds the result"), Defaulted.Events.Num(), DefaultCount);
			TestEqual(TEXT("considered count is the full total"), Defaulted.ConsideredCount, EventCount);

			// An explicit larger Count is not silently re-capped: the caller gets what it asked for.
			const FCsvProfilerEventList Explicit = UCsvProfilerToolset::ListEvents(Path, EventCount);
			TestEqual(TEXT("explicit larger Count is honored"), Explicit.Events.Num(), EventCount);
		});

		It(TEXT("treats FrameEnd -1 as running to the end"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// Events are Start@1 and End@3. An open end (3, -1) keeps End and drops the earlier Start.
			const FCsvProfilerEventList OpenEnd = UCsvProfilerToolset::ListEvents(Path, 256, 3, -1);
			TestEqual(TEXT("open end keeps events from frame 3 on"), OpenEnd.Events.Num(), 1);
			if (OpenEnd.Events.Num() == 1)
			{
				TestEqual(TEXT("kept End"), OpenEnd.Events[0].Name, FString(TEXT("End")));
			}
		});

		It(TEXT("accepts an explicit end past the stat frame count, since events can outlive the stat columns"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// The fixture has six stat frames, but events are not bound by the stat frame count. An explicit
			// end well past it is honored, not rejected; the range still selects the events it covers.
			FCsvProfilerEventList Result;
			const FString Error = CaptureToolError([&]() { Result = UCsvProfilerToolset::ListEvents(Path, 256, 0, 99); });

			TestTrue(TEXT("wide explicit end does not raise"), Error.IsEmpty());
			TestEqual(TEXT("both events selected"), Result.Events.Num(), 2);
		});

		It(TEXT("rejects a malformed explicit range by raising"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// A negative start or a non-positive-width range is invalid regardless of the frame count.
			TestFalse(TEXT("negative start"), CaptureToolError([&]() { UCsvProfilerToolset::ListEvents(Path, 256, -2, 3); }).IsEmpty());
			TestFalse(TEXT("end <= start"), CaptureToolError([&]() { UCsvProfilerToolset::ListEvents(Path, 256, 4, 4); }).IsEmpty());
		});
	});

	Describe("TopStats", [this]()
	{
		// Fixture aggregates: FrameTime avg 35 / max 60 / total 210; GpuTime avg 30 / max 55 / total 180.
		// FrameTime ranks above GpuTime on all three measures.
		It(TEXT("ranks stats by average, highest first, and reports the considered count"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerTopStatsResult Result = UCsvProfilerToolset::TopStats(Path, 10);

			TestEqual(TEXT("both stats considered"), Result.ConsideredCount, 2);
			TestEqual(TEXT("both returned"), Result.Stats.Num(), 2);
			if (Result.Stats.Num() == 2)
			{
				TestEqual(TEXT("FrameTime ranks first"), Result.Stats[0].StatName, FString(TEXT("FrameTime")));
				TestEqual(TEXT("GpuTime ranks second"), Result.Stats[1].StatName, FString(TEXT("GpuTime")));
			}
		});

		It(TEXT("ranks by max, putting the spiky stat ahead of the steady one"), [this]()
		{
			const FString Path = WriteFixtureCsvRankSkew();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerTopStatsResult Result = UCsvProfilerToolset::TopStats(Path, 10, ECsvProfilerStatRanking::Max);

			TestEqual(TEXT("both returned"), Result.Stats.Num(), 2);
			if (Result.Stats.Num() == 2)
			{
				TestEqual(TEXT("Spike leads on max"), Result.Stats[0].StatName, FString(TEXT("Spike")));
				TestEqual(TEXT("Spike's max value"), Result.Stats[0].Max, 90.f);
			}
		});

		It(TEXT("ranks by total, which tracks the average order (steady stat first)"), [this]()
		{
			const FString Path = WriteFixtureCsvRankSkew();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerTopStatsResult ByTotal = UCsvProfilerToolset::TopStats(Path, 10, ECsvProfilerStatRanking::Total);
			const FCsvProfilerTopStatsResult ByAverage = UCsvProfilerToolset::TopStats(Path, 10, ECsvProfilerStatRanking::Average);

			TestEqual(TEXT("total returns both"), ByTotal.Stats.Num(), 2);
			if (ByTotal.Stats.Num() == 2)
			{
				TestEqual(TEXT("Steady leads on total"), ByTotal.Stats[0].StatName, FString(TEXT("Steady")));
				TestEqual(TEXT("Steady's total value"), ByTotal.Stats[0].Total, 180.0);
			}
			if (ByTotal.Stats.Num() == 2 && ByAverage.Stats.Num() == 2)
			{
				TestEqual(TEXT("total order matches average order"), ByTotal.Stats[0].StatName, ByAverage.Stats[0].StatName);
			}
		});

		It(TEXT("ranks by a percentile: P99 favours the spiky stat, P50 the steady one"), [this]()
		{
			const FString Path = WriteFixtureCsvRankSkew();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// Spike is 0 0 0 0 0 90, Steady is 30 x6. P99 lands on Spike's 90; the P50 median is Spike 0
			// vs Steady 30, so the two percentiles rank the stats in opposite order.
			const FCsvProfilerTopStatsResult ByP99 = UCsvProfilerToolset::TopStats(Path, 10, ECsvProfilerStatRanking::P99);
			TestEqual(TEXT("P99 returns both"), ByP99.Stats.Num(), 2);
			if (ByP99.Stats.Num() == 2)
			{
				TestEqual(TEXT("Spike leads on P99"), ByP99.Stats[0].StatName, FString(TEXT("Spike")));
				TestEqual(TEXT("Spike's P99 value"), ByP99.Stats[0].P99, 90.f);
			}

			const FCsvProfilerTopStatsResult ByP50 = UCsvProfilerToolset::TopStats(Path, 10, ECsvProfilerStatRanking::P50);
			if (ByP50.Stats.Num() == 2)
			{
				TestEqual(TEXT("Steady leads on P50"), ByP50.Stats[0].StatName, FString(TEXT("Steady")));
				TestEqual(TEXT("Steady's P50 value"), ByP50.Stats[0].P50, 30.f);
			}
		});

		It(TEXT("restricts the ranking to a stat group via a wildcard"), [this]()
		{
			const FString Path = WriteFixtureCsvGrouped();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerTopStatsResult Result = UCsvProfilerToolset::TopStats(Path, 10, ECsvProfilerStatRanking::Average, TEXT("GPU/*"));

			TestEqual(TEXT("only the two GPU-group stats considered"), Result.ConsideredCount, 2);
			for (const FCsvProfilerStatReduction& Stat : Result.Stats)
			{
				TestTrue(TEXT("only GPU-group stats returned"), Stat.StatName.StartsWith(TEXT("GPU/")));
			}
		});

		It(TEXT("returns at most Count stats"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerTopStatsResult Result = UCsvProfilerToolset::TopStats(Path, 1);

			TestEqual(TEXT("one returned"), Result.Stats.Num(), 1);
			TestEqual(TEXT("still considered both"), Result.ConsideredCount, 2);
			if (Result.Stats.Num() == 1)
			{
				TestEqual(TEXT("top is FrameTime"), Result.Stats[0].StatName, FString(TEXT("FrameTime")));
			}
		});

		It(TEXT("restricts the ranking to stats matching a filter"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerTopStatsResult Result = UCsvProfilerToolset::TopStats(Path, 10, ECsvProfilerStatRanking::Average, TEXT("Gpu*"));

			TestEqual(TEXT("only one considered"), Result.ConsideredCount, 1);
			TestEqual(TEXT("one returned"), Result.Stats.Num(), 1);
			if (Result.Stats.Num() == 1)
			{
				TestEqual(TEXT("only GpuTime"), Result.Stats[0].StatName, FString(TEXT("GpuTime")));
			}
		});

		It(TEXT("clamps an over-large Count to the stats available"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FCsvProfilerTopStatsResult Result = UCsvProfilerToolset::TopStats(Path, 100);

			TestEqual(TEXT("clamped to the two stats"), Result.Stats.Num(), 2);
		});

		It(TEXT("ranks over an open-ended range when an end bound is -1"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// (3, -1) ranks over frames 3..end without raising, and matches the equivalent explicit range.
			const FCsvProfilerTopStatsResult OpenEnd =
				UCsvProfilerToolset::TopStats(Path, 10, ECsvProfilerStatRanking::Average, TEXT(""), 3, -1);
			const FCsvProfilerTopStatsResult Explicit =
				UCsvProfilerToolset::TopStats(Path, 10, ECsvProfilerStatRanking::Average, TEXT(""), 3, 6);

			TestEqual(TEXT("open end ranks both stats"), OpenEnd.Stats.Num(), 2);
			if (OpenEnd.Stats.Num() == 2 && Explicit.Stats.Num() == 2)
			{
				TestEqual(TEXT("open end matches explicit end"), OpenEnd.Stats[0].Average, Explicit.Stats[0].Average);
			}
		});

		It(TEXT("fails the call on an invalid range by raising"), [this]()
		{
			const FString Path = WriteFixtureCsv();
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			const FString Error = CaptureToolError([&]() { UCsvProfilerToolset::TopStats(Path, 10, ECsvProfilerStatRanking::Average, TEXT(""), 0, 99); });

			TestFalse(TEXT("error raised"), Error.IsEmpty());
		});
	});

	Describe("EmptyCapture", [this]()
	{
		// A CSVBIN with no stats and no events reads cleanly but has zero frames. The default whole-run
		// request must yield empty results, not a misleading range error.
		It(TEXT("ReduceStat treats whole-run on a zero-frame capture as empty, not an error"), [this]()
		{
			const TArray<int32> NoFrames;
			const TArray<FString> NoNames;
			const TArray<double> NoTimestamps;
			const FString Path = WriteFixtureCsvBin(4, NoFrames, NoNames, NoTimestamps);
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// Call through the exception handler so a raise is observable: a direct call would swallow
			// RaiseScriptError (no Blueprints frame), and the empty result alone cannot tell an intended
			// empty run from the range error this test guards against.
			FCsvProfilerReduceResult Result;
			const FString Error = CaptureToolError([&]() { Result = UCsvProfilerToolset::ReduceStat(Path, { TEXT("FrameTime") }); });

			TestTrue(TEXT("whole-run on an empty capture does not raise"), Error.IsEmpty());
			TestEqual(TEXT("no reductions from an empty capture"), Result.Stats.Num(), 0);
		});

		It(TEXT("TopStats returns an empty ranking for whole-run on a zero-frame capture"), [this]()
		{
			const TArray<int32> NoFrames;
			const TArray<FString> NoNames;
			const TArray<double> NoTimestamps;
			const FString Path = WriteFixtureCsvBin(4, NoFrames, NoNames, NoTimestamps);
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// Route through the handler so the guard fails if whole-run regresses to raising rather than
			// returning an empty ranking; the empty result by itself would pass either way.
			FCsvProfilerTopStatsResult Result;
			const FString Error = CaptureToolError([&]() { Result = UCsvProfilerToolset::TopStats(Path, 10); });

			TestTrue(TEXT("whole-run on an empty capture does not raise"), Error.IsEmpty());
			TestEqual(TEXT("nothing considered"), Result.ConsideredCount, 0);
			TestEqual(TEXT("no stats returned"), Result.Stats.Num(), 0);
		});
	});

	Describe("Registration", [this]()
	{
		It(TEXT("registers UCsvProfilerToolset with the ToolsetRegistry"), [this]()
		{
			if (!UToolsetRegistry::IsAvailable())
			{
				AddWarning(TEXT("ToolsetRegistry unavailable; skipping registration assertion."));
				return;
			}

			TestTrue(TEXT("UCsvProfilerToolset class is registered"),
				UToolsetRegistry::IsToolsetClassRegistered(UCsvProfilerToolset::StaticClass()));
		});
	});

#if 0 // UE 5.8: these test 5.9 CsvUtils' v4/v5 .csv.bin reader (per-event timestamps); 5.8's reader stops at v3 and has no FCsvProfilerEvent::Timestamp
	Describe("ReadFromCsvBin", [this]()
	{
		It(TEXT("reads a v4 capture and populates each event timestamp with the written value"), [this]()
		{
			const TArray<int32> Frames = { 0, 1 };
			const TArray<FString> Names = { TEXT("Alpha"), TEXT("Beta") };
			const TArray<double> Timestamps = { 1.5, 2.75 };
			const FString Path = WriteFixtureCsvBin(4, Frames, Names, Timestamps);
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			CsvUtils::FCsvProfilerCapture Capture;
			TArray<FText> Errors;
			const bool bOk = CsvUtils::ReadFromCsvBin(Capture, *ToAbsolute(Path), &Errors);

			TestTrue(TEXT("parse succeeds"), bOk);
			TestEqual(TEXT("no errors"), Errors.Num(), 0);
			TestEqual(TEXT("two events"), Capture.Events.Num(), 2);
			if (Capture.Events.Num() == 2)
			{
				TestEqual(TEXT("event 0 name"), Capture.Events[0].Name, FString(TEXT("Alpha")));
				TestEqual(TEXT("event 0 frame"), Capture.Events[0].Frame, 0);
				TestTrue(TEXT("event 0 timestamp set"), Capture.Events[0].Timestamp.IsSet());
				if (Capture.Events[0].Timestamp.IsSet())
				{
					TestEqual(TEXT("event 0 timestamp value"), Capture.Events[0].Timestamp.GetValue(), 1.5);
				}

				TestEqual(TEXT("event 1 name"), Capture.Events[1].Name, FString(TEXT("Beta")));
				TestEqual(TEXT("event 1 frame"), Capture.Events[1].Frame, 1);
				TestTrue(TEXT("event 1 timestamp set"), Capture.Events[1].Timestamp.IsSet());
				if (Capture.Events[1].Timestamp.IsSet())
				{
					TestEqual(TEXT("event 1 timestamp value"), Capture.Events[1].Timestamp.GetValue(), 2.75);
				}
			}
		});

		It(TEXT("reads a v3 capture and leaves every event timestamp unset"), [this]()
		{
			const TArray<int32> Frames = { 0, 2 };
			const TArray<FString> Names = { TEXT("Start"), TEXT("End") };
			const TArray<double> NoTimestamps;
			const FString Path = WriteFixtureCsvBin(3, Frames, Names, NoTimestamps);
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			CsvUtils::FCsvProfilerCapture Capture;
			TArray<FText> Errors;
			const bool bOk = CsvUtils::ReadFromCsvBin(Capture, *ToAbsolute(Path), &Errors);

			TestTrue(TEXT("parse succeeds"), bOk);
			TestEqual(TEXT("no errors"), Errors.Num(), 0);
			TestEqual(TEXT("two events"), Capture.Events.Num(), 2);
			if (Capture.Events.Num() == 2)
			{
				TestFalse(TEXT("event 0 timestamp unset"), Capture.Events[0].Timestamp.IsSet());
				TestFalse(TEXT("event 1 timestamp unset"), Capture.Events[1].Timestamp.IsSet());
			}
		});

		It(TEXT("reads a v4 capture with zero events cleanly with an empty event list"), [this]()
		{
			const TArray<int32> NoFrames;
			const TArray<FString> NoNames;
			const TArray<double> NoTimestamps;
			const FString Path = WriteFixtureCsvBin(4, NoFrames, NoNames, NoTimestamps);
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			CsvUtils::FCsvProfilerCapture Capture;
			TArray<FText> Errors;
			const bool bOk = CsvUtils::ReadFromCsvBin(Capture, *ToAbsolute(Path), &Errors);

			TestTrue(TEXT("parse succeeds"), bOk);
			TestEqual(TEXT("no errors"), Errors.Num(), 0);
			TestEqual(TEXT("no events"), Capture.Events.Num(), 0);
		});

		It(TEXT("rejects a version above current without hanging or misreading"), [this]()
		{
			// Version 5 is above CURRENT (4), so the version guard must reject it.
			const TArray<int32> Frames = { 0 };
			const TArray<FString> Names = { TEXT("Event") };
			const TArray<double> Timestamps = { 0.0 };
			const FString Path = WriteFixtureCsvBin(5, Frames, Names, Timestamps);
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			CsvUtils::FCsvProfilerCapture Capture;
			TArray<FText> Errors;
			const bool bOk = CsvUtils::ReadFromCsvBin(Capture, *ToAbsolute(Path), &Errors);

			TestFalse(TEXT("parse is rejected"), bOk);
			TestFalse(TEXT("error reported"), Errors.IsEmpty());
			TestEqual(TEXT("no events"), Capture.Events.Num(), 0);
		});

		It(TEXT("fails cleanly when a v4 event timestamp is truncated mid-read"), [this]()
		{
			// Write a v4 blob with one event but only 4 of the 8 timestamp bytes, so the
			// archive hits EOF partway through the double read.
			const TArray<int32> Frames = { 0 };
			const TArray<FString> Names = { TEXT("Truncated") };
			const TArray<double> Timestamps = { 9.9 };
			const FString Path = WriteFixtureCsvBin(4, Frames, Names, Timestamps, /*bTruncateLastTimestamp=*/true);
			ON_SCOPE_EXIT { DeleteFixture(Path); };

			// The file manager logs an error when a read is attempted past EOF; that is the
			// expected mechanism by which truncation is detected. Suppress it so the framework
			// does not count it as a test failure.
			AddExpectedError(TEXT("Requested read of"), EAutomationExpectedErrorFlags::Contains, 1);

			CsvUtils::FCsvProfilerCapture Capture;
			TArray<FText> Errors;
			const bool bOk = CsvUtils::ReadFromCsvBin(Capture, *ToAbsolute(Path), &Errors);

			TestFalse(TEXT("parse fails on truncation"), bOk);
			TestFalse(TEXT("error reported"), Errors.IsEmpty());
		});

		It(TEXT("parses a v4 blob where a v5 blob is rejected, confirming current version is exactly 4"), [this]()
		{
			// v4 must parse cleanly.
			{
				const TArray<int32> Frames = { 0 };
				const TArray<FString> Names = { TEXT("Evt") };
				const TArray<double> Timestamps = { 1.0 };
				const FString PathV4 = WriteFixtureCsvBin(4, Frames, Names, Timestamps);
				ON_SCOPE_EXIT { DeleteFixture(PathV4); };

				CsvUtils::FCsvProfilerCapture CaptureV4;
				TArray<FText> ErrorsV4;
				TestTrue(TEXT("v4 parses"), CsvUtils::ReadFromCsvBin(CaptureV4, *ToAbsolute(PathV4), &ErrorsV4));
			}

			// v5 must be rejected by the version guard.
			{
				const TArray<int32> Frames = { 0 };
				const TArray<FString> Names = { TEXT("Evt") };
				const TArray<double> Timestamps = { 1.0 };
				const FString PathV5 = WriteFixtureCsvBin(5, Frames, Names, Timestamps);
				ON_SCOPE_EXIT { DeleteFixture(PathV5); };

				CsvUtils::FCsvProfilerCapture CaptureV5;
				TArray<FText> ErrorsV5;
				TestFalse(TEXT("v5 rejected"), CsvUtils::ReadFromCsvBin(CaptureV5, *ToAbsolute(PathV5), &ErrorsV5));
			}
		});
	});
#endif // UE 5.8
}

#endif // WITH_DEV_AUTOMATION_TESTS
