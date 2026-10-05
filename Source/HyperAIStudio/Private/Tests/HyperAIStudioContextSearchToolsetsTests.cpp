// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioContextSearchToolsets.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioProjectAssetIndex.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace HyperAIStudio::ContextSearch::Tests
{
	FHyperAIContextSearchValueRecord MakeRecord(
		const TCHAR* Kind,
		const TCHAR* Id,
		const TCHAR* Name,
		const TCHAR* ExtraField = TEXT("path"),
		const TCHAR* ExtraValue = TEXT("/Game/Test"))
	{
		FHyperAIContextSearchValueRecord Record;
		Record.Kind = Kind;
		Record.RecordId = Id;
		Record.SearchTextLower = FString(Name).ToLower() + TEXT(" ") + FString(ExtraValue).ToLower();
		Record.Fields.Add({ TEXT("name"), TEXT("name"), Name });
		Record.Fields.Add({ ExtraField, TEXT("string"), ExtraValue });
		return Record;
	}

	FHyperAIProjectIndexValueSnapshot MakeReadyIndex()
	{
		FHyperAIProjectIndexValueSnapshot Snapshot;
		Snapshot.Status = TEXT("ready");
		Snapshot.StartedUtc = TEXT("2026-08-14T10:00:00.000Z");
		Snapshot.CompletedUtc = TEXT("2026-08-14T10:00:01.000Z");
		Snapshot.Value.SnapshotUtc = Snapshot.CompletedUtc;
		Snapshot.Value.ObservationScope = TEXT("project_on_disk_index");
		Snapshot.Value.bOnDiskOnly = true;
		Snapshot.Value.Generation = 7;
		Snapshot.Value.Records.Add(MakeRecord(
			TEXT("asset"),
			TEXT("/Game/Characters/Hero.Hero"),
			TEXT("Hero"),
			TEXT("package_name"),
			TEXT("/Game/Characters/Hero")));
		FHyperAIContextSearchValueRecord Symbol = MakeRecord(
			TEXT("cpp_symbol"),
			TEXT("symbol-1"),
			TEXT("BuildHero"),
			TEXT("relative_path"),
			TEXT("Source/Game/Hero.cpp"));
		Symbol.Fields.Add({ TEXT("symbol_kind"), TEXT("string"), TEXT("function_candidate") });
		Symbol.Fields.Add({ TEXT("line"), TEXT("integer"), TEXT("42") });
		Snapshot.Value.Records.Add(MoveTemp(Symbol));
		Snapshot.AssetRecordCount = 1;
		Snapshot.CppFileCount = 1;
		Snapshot.CppSymbolCount = 1;
		Snapshot.SnapshotFingerprint =
			FHyperAIStudioContextSearchContracts::ComputeSnapshotFingerprint(Snapshot.Value);
		return Snapshot;
	}

	bool HasDiagnostic(const FHyperAIReadReport& Report, const TCHAR* Code)
	{
		return Report.Diagnostics.ContainsByPredicate([Code](const FHyperAIReadDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
	}

	FString MakeFixtureRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("HyperAIStudioContextSearchTests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	/** One asset row as the asset index builds it: search text from name, class, package and parent class. */
	FHyperAIProjectAssetRow MakeAssetRow(
		const FString& ObjectPath,
		const TCHAR* ClassPath,
		const TCHAR* ParentClassPath = nullptr)
	{
		FHyperAIProjectAssetRow Row;
		Row.ObjectPath = FSoftObjectPath(ObjectPath);
		Row.ClassPath = FTopLevelAssetPath(ClassPath);
		Row.SearchTextLower = Row.ObjectPath.GetAssetName() + TEXT(" ") + ClassPath + TEXT(" ")
			+ Row.ObjectPath.GetLongPackageName();
		if (ParentClassPath)
		{
			Row.ParentClassPath = FName(ParentClassPath);
			Row.SearchTextLower += FString(TEXT(" ")) + ParentClassPath;
		}
		Row.SearchTextLower.ToLowerInline();
		return Row;
	}

	/** A published asset snapshot holding the rows, grouped into folders by package path. */
	FHyperAIProjectAssetIndexSnapshot MakeAssetSnapshot(
		const TArray<FHyperAIProjectAssetRow>& Rows,
		const TCHAR* Status = TEXT("ready"),
		const int64 Generation = 1)
	{
		TMap<FString, TSharedRef<FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>> ByFolder;
		TArray<FString> Tokens = { TEXT("fixture") };
		for (const FHyperAIProjectAssetRow& Row : Rows)
		{
			const FString Folder = FPackageName::GetLongPackagePath(Row.ObjectPath.GetLongPackageName());
			if (!ByFolder.Contains(Folder))
			{
				TSharedRef<FHyperAIProjectAssetFolder, ESPMode::ThreadSafe> Chunk =
					MakeShared<FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>();
				Chunk->Path = FName(*Folder);
				ByFolder.Add(Folder, Chunk);
			}
			ByFolder[Folder]->Rows.Add(Row);
			Tokens.Add(Row.ObjectPath.ToString());
			Tokens.Add(Row.ClassPath.ToString());
		}
		ByFolder.KeySort(TLess<FString>());
		FHyperAIProjectAssetIndexSnapshot Snapshot;
		Snapshot.Status = Status;
		Snapshot.Generation = Generation;
		Snapshot.bHasCompletedWalk = FString(Status) == TEXT("ready") || FString(Status) == TEXT("partial");
		Snapshot.AssetCount = Rows.Num();
		for (TPair<FString, TSharedRef<FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>>& Pair : ByFolder)
		{
			Pair.Value->Fingerprint = FHyperAIStudioContextSearchContracts::HashTokens({ Pair.Key });
			Snapshot.Folders.Add(Pair.Value);
		}
		Snapshot.FolderCount = Snapshot.Folders.Num();
		Snapshot.Fingerprint = FHyperAIStudioContextSearchContracts::HashTokens(Tokens);
		return Snapshot;
	}

	TArray<FHyperAIProjectAssetRow> MakeRockAndUnarmedRows()
	{
		return {
			MakeAssetRow(TEXT("/Game/Env/SM_Rock.SM_Rock"), TEXT("/Script/Engine.StaticMesh")),
			MakeAssetRow(TEXT("/Game/Anim/Unarmed/AS_Unarmed_Idle.AS_Unarmed_Idle"), TEXT("/Script/Engine.AnimSequence")),
			MakeAssetRow(TEXT("/Game/Blueprints/BP_Hero.BP_Hero"), TEXT("/Script/Engine.Blueprint"), TEXT("/Script/Engine.Character"))
		};
	}

	FString FindField(const FHyperAIReadRecord& Record, const TCHAR* Name)
	{
		const FHyperAIReadField* Field = Record.Fields.FindByPredicate([Name](const FHyperAIReadField& Candidate)
		{
			return Candidate.Name == Name;
		});
		return Field ? Field->Value : FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchManifestTest,
	"HyperAIStudio.NativeTools.ContextSearch.ManifestAndCohorts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchManifestTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	const TArray<FHyperAIContextSearchManifestEntry>& Manifest =
		FHyperAIStudioContextSearchContracts::GetManifest();
	const FHyperAIStudioCapabilityCatalog& Catalog =
		FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	TestEqual(TEXT("Exactly seven source candidates exist"), Manifest.Num(), 7);
	TSet<FString> Names;
	TSet<FString> Toolsets;
	TSet<FString> AtomicCohorts;
	for (const FHyperAIContextSearchManifestEntry& Entry : Manifest)
	{
		Names.Add(Entry.Name);
		Toolsets.Add(Entry.Toolset);
		TestEqual(
			TEXT("Every entry remains SourceCandidate"),
			Entry.AdmissionState,
			FHyperAIContextSearchManifestEntry::EAdmissionState::SourceCandidate);
		TestFalse(
			TEXT("Source candidates are not registered by production policy"),
			FHyperAIStudioContextSearchContracts::IsRegistrationAllowed(Entry.Toolset, false));
		TestTrue(
			TEXT("Dev-only pending switch may expose each member of the generated cohort"),
			FHyperAIStudioContextSearchContracts::IsRegistrationAllowed(Entry.Toolset, true));
		const FHyperAIStudioCapabilityToolDefinition* CatalogTool =
			Catalog.Tools.FindByPredicate([&Entry](const FHyperAIStudioCapabilityToolDefinition& Tool)
			{
				return Tool.Name == Entry.Name;
			});
		TestNotNull(TEXT("Every manifest entry has one generated-catalog contract"), CatalogTool);
		if (CatalogTool)
		{
			AtomicCohorts.Add(CatalogTool->AtomicCohortId);
			TestEqual(
				TEXT("Manifest and generated catalog agree on source-candidate admission"),
				CatalogTool->AdmissionState,
				EHyperAIStudioCapabilityAdmissionState::SourceCandidate);
		}
	}
	TestEqual(TEXT("No names are duplicated"), Names.Num(), 7);
	TestEqual(TEXT("Seven contracts are exposed by five owned toolset classes"), Toolsets.Num(), 5);
	TestEqual(TEXT("All seven contracts share one generated atomic admission cohort"), AtomicCohorts.Num(), 1);
	TestTrue(
		TEXT("Generated context/search cohort identity is stable"),
		AtomicCohorts.Contains(TEXT("cohort.source.hyperaistudiocontextsearchtoolsets.v1")));
	TestTrue(TEXT("Batch name present"), Names.Contains(TEXT("hyper_batch_query")));
	TestTrue(TEXT("Context name present"), Names.Contains(TEXT("hyper_context_snapshot")));
	TestTrue(TEXT("Scene name present"), Names.Contains(TEXT("hyper_scene_inspect")));
	TestTrue(TEXT("Scene apply name present"), Names.Contains(TEXT("hyper_scene_apply_plan")));
	TestTrue(TEXT("Scene validate name present"), Names.Contains(TEXT("hyper_scene_validate")));
	TestTrue(TEXT("Project search name present"), Names.Contains(TEXT("hyper_project_search")));
	TestTrue(TEXT("Index status name present"), Names.Contains(TEXT("hyper_project_index_status")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchProjectionCursorTest,
	"HyperAIStudio.NativeTools.ContextSearch.ProjectionAndCursor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchProjectionCursorTest::RunTest(const FString& Parameters)
{
	TSet<FString> Allowed = { TEXT("name"), TEXT("path") };
	TArray<FString> Projection;
	FString Code;
	FString Error;
	TestTrue(
		TEXT("Projection normalizes case, order, and duplicates"),
		FHyperAIStudioContextSearchContracts::NormalizeProjection(
			{ TEXT("Path"), TEXT("name"), TEXT("path") },
			Allowed,
			{ TEXT("name") },
			Projection,
			Code,
			Error));
	TestEqual(TEXT("Projection deduplicated"), Projection.Num(), 2);
	TestEqual(TEXT("Projection stable sort first"), Projection[0], FString(TEXT("name")));
	TestEqual(TEXT("Projection stable sort second"), Projection[1], FString(TEXT("path")));
	TestFalse(
		TEXT("Unknown projection fails closed"),
		FHyperAIStudioContextSearchContracts::NormalizeProjection(
			{ TEXT("raw_source") }, Allowed, { TEXT("name") }, Projection, Code, Error));
	TestEqual(TEXT("Unknown field has stable code"), Code, FString(TEXT("value_not_allowlisted")));

	const FString RequestA = FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("request-a") });
	const FString RequestB = FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("request-b") });
	const FString SnapshotA = FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("snapshot-a") });
	const FString SnapshotB = FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("snapshot-b") });
	TestTrue(TEXT("Request fingerprint is SHA-256"), RequestA.StartsWith(TEXT("sha256:")) && RequestA.Len() == 71);
	TestEqual(
		TEXT("Local bounded SHA-256 matches the canonical UTF-8 vector"),
		FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("abc") }),
		FString(TEXT("sha256:aab5f9ae99b2e38fb462025c8f72f570c9c811705d2a4277dc855d7fa293fe97")));
	FHyperAIContextSearchValueSnapshot FingerprintFixture;
	FingerprintFixture.ObservationScope = TEXT("scope");
	FingerprintFixture.Generation = 9;
	FingerprintFixture.DirtyPackages = { TEXT("/Game/Z"), TEXT("/Game/A") };
	FHyperAIContextSearchValueRecord FingerprintRecord;
	FingerprintRecord.QueryId = TEXT("query");
	FingerprintRecord.Kind = TEXT("kind");
	FingerprintRecord.RecordId = TEXT("record");
	FingerprintRecord.Fields = {
		{ TEXT("z"), TEXT("string"), TEXT("last") },
		{ TEXT("a"), TEXT("string"), TEXT("first") }
	};
	FingerprintFixture.Records.Add(MoveTemp(FingerprintRecord));
	const FString MaterializedReference = FHyperAIStudioContextSearchContracts::HashTokens({
		TEXT("scope"), TEXT("0"), TEXT("0"), TEXT("0"), TEXT("0"), TEXT("0"), TEXT("9"),
		TEXT("/Game/A"), TEXT("/Game/Z"), TEXT("query"), TEXT("kind"), TEXT("record"),
		TEXT("a"), TEXT("string"), TEXT("first"), TEXT("z"), TEXT("string"), TEXT("last")
	});
	TestEqual(
		TEXT("Streaming snapshot hash stays byte-identical to the materialized canonical token route"),
		FHyperAIStudioContextSearchContracts::ComputeSnapshotFingerprint(FingerprintFixture),
		MaterializedReference);
	FString MalformedUtf16;
	MalformedUtf16.AppendChar(static_cast<TCHAR>(0xd800));
	TestTrue(
		TEXT("Malformed UTF-16 is rejected before canonical hashing"),
		FHyperAIStudioContextSearchContracts::HashTokens({ MalformedUtf16 }).IsEmpty());
	const FString Cursor = FHyperAIStudioContextSearchContracts::MakeCursor(RequestA, SnapshotA, 3);
	int32 Offset = INDEX_NONE;
	TestTrue(
		TEXT("Bound cursor parses"),
		FHyperAIStudioContextSearchContracts::ParseCursor(Cursor, RequestA, SnapshotA, 10, Offset, Code, Error));
	TestEqual(TEXT("Offset round trips"), Offset, 3);
	TestFalse(
		TEXT("Changed request rejects cursor"),
		FHyperAIStudioContextSearchContracts::ParseCursor(Cursor, RequestB, SnapshotA, 10, Offset, Code, Error));
	TestEqual(TEXT("Request mismatch code"), Code, FString(TEXT("cursor_request_mismatch")));
	TestFalse(
		TEXT("Changed snapshot rejects cursor"),
		FHyperAIStudioContextSearchContracts::ParseCursor(Cursor, RequestA, SnapshotB, 10, Offset, Code, Error));
	TestEqual(TEXT("Snapshot mismatch code"), Code, FString(TEXT("cursor_snapshot_changed")));
	FString Tampered = Cursor;
	Tampered[15] = Tampered[15] == TEXT('a') ? TEXT('b') : TEXT('a');
	TestFalse(
		TEXT("Tampered cursor rejects"),
		FHyperAIStudioContextSearchContracts::ParseCursor(Tampered, RequestA, SnapshotA, 10, Offset, Code, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSnapshotAssetBoundTest,
	"HyperAIStudio.NativeTools.ContextSearch.ContextSnapshotAssetBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSnapshotAssetBoundTest::RunTest(const FString& Parameters)
{
	FHyperAIContextSnapshotRequest InvalidRequest;
	InvalidRequest.MaxSelectedAssets = 1;
	const FHyperAIReadReport InvalidReport =
		UHyperAIStudioContextSnapshotToolset::hyper_context_snapshot(InvalidRequest);
	TestFalse(TEXT("Positive asset selection bound fails closed"), InvalidReport.bOk);
	TestEqual(TEXT("Positive asset selection bound is invalid"), InvalidReport.Status, FString(TEXT("invalid_request")));
	TestTrue(TEXT("Positive asset selection bound has stable diagnostic"),
		HyperAIStudio::ContextSearch::Tests::HasDiagnostic(InvalidReport, TEXT("selected_assets_not_supported")));

	FHyperAIContextSnapshotRequest Request;
	Request.MaxSelectedActors = 0;
	Request.MaxSelectedAssets = 0;
	Request.MaxBlueprintNodes = 0;
	Request.bIncludeBlueprintPins = false;
	const FHyperAIReadReport Report = UHyperAIStudioContextSnapshotToolset::hyper_context_snapshot(Request);
	TestNotEqual(TEXT("Zero asset bound is accepted"), Report.Status, FString(TEXT("invalid_request")));
	TestFalse(TEXT("Zero asset bound emits no selected-asset incomplete diagnostic"),
		Report.Diagnostics.ContainsByPredicate([](const FHyperAIReadDiagnostic& Diagnostic)
		{
			return Diagnostic.Code.Contains(TEXT("asset"), ESearchCase::IgnoreCase)
				|| Diagnostic.Field.Contains(TEXT("asset"), ESearchCase::IgnoreCase);
		}));
	TestFalse(TEXT("Context snapshot never returns selected-asset records"),
		Report.Records.ContainsByPredicate([](const FHyperAIReadRecord& Record)
		{
			return Record.Kind == TEXT("selected_asset");
		}));

	FHyperAIContextSnapshotRequest FocusedRequest;
	FocusedRequest.Fields = {
		TEXT("path"), TEXT("location"), TEXT("rotation"), TEXT("scale")
	};
	FocusedRequest.MaxSelectedActors = 8;
	FocusedRequest.MaxSelectedAssets = 0;
	FocusedRequest.MaxBlueprintNodes = 0;
	FocusedRequest.bIncludeBlueprintPins = false;
	const FHyperAIReadReport FocusedReport =
		UHyperAIStudioContextSnapshotToolset::hyper_context_snapshot(FocusedRequest);
	TestTrue(TEXT("Actor plus viewport fast path succeeds"), FocusedReport.bOk);
	TestTrue(TEXT("Actor plus viewport fast path records the Unreal frame"), FocusedReport.SnapshotGeneration > 0);
	TestFalse(TEXT("Actor plus viewport fast path is complete"), FocusedReport.bIncomplete);
	TestTrue(TEXT("Actor plus viewport fast path captures a viewport"),
		FocusedReport.Records.ContainsByPredicate([](const FHyperAIReadRecord& Record)
		{
			return Record.Kind == TEXT("viewport");
		}));
	TestFalse(TEXT("Actor plus viewport fast path materializes no other categories"),
		FocusedReport.Records.ContainsByPredicate([](const FHyperAIReadRecord& Record)
		{
			return Record.Kind != TEXT("viewport") && Record.Kind != TEXT("selected_actor");
		}));
	TestEqual(TEXT("Actor plus viewport fast path captures no dirty-package inventory"),
		FocusedReport.DirtyPackageCount, 0);

	FHyperAIContextSnapshotRequest AssetFieldRequest;
	AssetFieldRequest.Fields = { TEXT("asset_name") };
	const FHyperAIReadReport AssetFieldReport =
		UHyperAIStudioContextSnapshotToolset::hyper_context_snapshot(AssetFieldRequest);
	TestFalse(TEXT("Removed asset projection fails closed"), AssetFieldReport.bOk);
	TestTrue(TEXT("Removed asset projection is not allowlisted"),
		HyperAIStudio::ContextSearch::Tests::HasDiagnostic(AssetFieldReport, TEXT("value_not_allowlisted")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchPageTest,
	"HyperAIStudio.NativeTools.ContextSearch.ImmutablePagingBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchPageTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	FHyperAIContextSearchValueSnapshot Snapshot;
	Snapshot.SnapshotUtc = TEXT("2026-08-14T10:00:00.000Z");
	Snapshot.ObservationScope = TEXT("test_value_snapshot");
	Snapshot.Generation = 3;
	Snapshot.bDirty = true;
	Snapshot.DirtyPackages.Add(TEXT("/Game/Dirty"));
	Snapshot.Records.Add(MakeRecord(TEXT("actor"), TEXT("z"), TEXT("Zulu"), TEXT("path"), TEXT("/Game/Z")));
	Snapshot.Records.Add(MakeRecord(TEXT("actor"), TEXT("a"), TEXT("Alpha"), TEXT("path"), TEXT("/Game/A")));
	Snapshot.Records.Add(MakeRecord(TEXT("actor"), TEXT("m"), TEXT("Mike"), TEXT("path"), TEXT("/Game/M")));
	const FString RequestFingerprint = FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("page-test") });
	FHyperAIReadReport First;
	TestTrue(
		TEXT("First page succeeds"),
		FHyperAIStudioContextSearchContracts::ProjectPage(
			Snapshot, { TEXT("name") }, RequestFingerprint, 2, FString(), 4096, First));
	TestEqual(TEXT("Stable record sort first"), First.Records[0].RecordId, FString(TEXT("a")));
	TestEqual(TEXT("Stable record sort second"), First.Records[1].RecordId, FString(TEXT("m")));
	TestEqual(TEXT("Projection strips unrequested path"), First.Records[0].Fields.Num(), 1);
	TestTrue(TEXT("Dirty evidence preserved"), First.bDirty);
	TestEqual(TEXT("Dirty package preserved"), First.DirtyPackages[0], FString(TEXT("/Game/Dirty")));
	TestFalse(TEXT("Continuation cursor emitted"), First.NextCursor.IsEmpty());

	FHyperAIReadReport Second;
	TestTrue(
		TEXT("Second page succeeds"),
		FHyperAIStudioContextSearchContracts::ProjectPage(
			Snapshot, { TEXT("name") }, RequestFingerprint, 2, First.NextCursor, 4096, Second));
	TestEqual(TEXT("Second page contains final record"), Second.Records[0].RecordId, FString(TEXT("z")));
	Snapshot.Records[0].Fields[0].Value = TEXT("Changed");
	FHyperAIReadReport Stale;
	TestFalse(
		TEXT("Snapshot mutation invalidates cursor"),
		FHyperAIStudioContextSearchContracts::ProjectPage(
			Snapshot, { TEXT("name") }, RequestFingerprint, 2, First.NextCursor, 4096, Stale));
	TestEqual(TEXT("Stale cursor is invalid_request"), Stale.Status, FString(TEXT("invalid_request")));
	TestTrue(TEXT("Stale cursor diagnostic present"), HasDiagnostic(Stale, TEXT("cursor_snapshot_changed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchSourcePrivacyTest,
	"HyperAIStudio.NativeTools.ContextSearch.CppIndexPrivacyAndContainment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchSourcePrivacyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	const FString FixtureBase = MakeFixtureRoot();
	const FString ProjectRoot = FPaths::Combine(FixtureBase, TEXT("Project"));
	const FString SourceDirectory = FPaths::Combine(ProjectRoot, TEXT("Source"), TEXT("Game"));
	const FString SourcePath = FPaths::Combine(SourceDirectory, TEXT("Hero.cpp"));
	const FString OutsideRoot = FPaths::Combine(FixtureBase, TEXT("Outside"));
	const FString OutsidePath = FPaths::Combine(OutsideRoot, TEXT("Secret.cpp"));
	IFileManager& FileManager = IFileManager::Get();
	TestTrue(TEXT("Fixture project source directory created"), FileManager.MakeDirectory(*SourceDirectory, true));
	TestTrue(TEXT("Fixture outside directory created"), FileManager.MakeDirectory(*OutsideRoot, true));
	const FString Secret = TEXT("DO_NOT_RETURN_SECRET_VALUE_987");
	const FString Source = FString::Printf(
		TEXT("class FHero final {};\nstruct FState {};\nvoid BuildHero(int32 Count) { const TCHAR* Token = TEXT(\"%s\"); }\n"),
		*Secret);
	TestTrue(
		TEXT("Fixture source saved"),
		FFileHelper::SaveStringToFile(Source, *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	TestTrue(
		TEXT("Fixture outside source saved"),
		FFileHelper::SaveStringToFile(TEXT("class FSecret {};"), *OutsidePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

#if !PLATFORM_WINDOWS
	FString UnsupportedRelativePath;
	TestFalse(
		TEXT("Unsupported platforms fail closed instead of using lexical containment"),
		FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
			ProjectRoot, SourcePath, UnsupportedRelativePath));
	TestTrue(TEXT("Unsupported-platform fixture tree removed"), FileManager.DeleteDirectory(*FixtureBase, false, true));
	return true;
#endif

	FString RelativePath;
	TestTrue(
		TEXT("Project C++ source is contained"),
		FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
			ProjectRoot, SourcePath, RelativePath));
	TestEqual(TEXT("Only project-relative path remains"), RelativePath, FString(TEXT("Source/Game/Hero.cpp")));
	TestFalse(
		TEXT("External source is rejected"),
		FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
			ProjectRoot, OutsidePath, RelativePath));
	TestFalse(
		TEXT("Non-C++ file is rejected"),
		FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
			ProjectRoot, FPaths::Combine(SourceDirectory, TEXT("Secrets.env")), RelativePath));

	FHyperAIProjectSourceReadResult Read;
	FString ReadCode;
	TestTrue(
		TEXT("Same-handle contained source read succeeds"),
		FHyperAIStudioContextSearchContracts::ReadProjectContainedCppSource(
			ProjectRoot, SourcePath, Read, ReadCode, 4096));
	TestEqual(TEXT("Same-handle read returns only relative identity"), Read.RelativePath, FString(TEXT("Source/Game/Hero.cpp")));
	TestEqual(TEXT("Same-handle read reports exact bytes"), Read.BytesRead, static_cast<int64>(FTCHARToUTF8(*Source).Length()));
	TestFalse(
		TEXT("Same-handle read rejects before allocating beyond requested byte cap"),
		FHyperAIStudioContextSearchContracts::ReadProjectContainedCppSource(
			ProjectRoot, SourcePath, Read, ReadCode, 4));
	TestEqual(TEXT("Oversize source has stable rejection"), ReadCode, FString(TEXT("source_file_size_bound_rejected")));

	for (int32 Index = 0; Index < 16; ++Index)
	{
		const FString FanoutPath = FPaths::Combine(
			ProjectRoot,
			TEXT("Source"),
			FString::Printf(TEXT("Fanout%02d.cpp"), Index));
		TestTrue(
			TEXT("High-fanout fixture saved"),
			FFileHelper::SaveStringToFile(TEXT("class FBounded {};"), *FanoutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}
	TArray<FString> Enumerated;
	bool bEnumerationTruncated = false;
	FString EnumerationCode;
	TestTrue(
		TEXT("Bounded directory traversal returns a partial observation"),
		FHyperAIStudioContextSearchContracts::EnumerateProjectCppSourceFiles(
			ProjectRoot, 32, 4, Enumerated, bEnumerationTruncated, EnumerationCode));
	TestTrue(TEXT("High fanout is explicitly truncated"), bEnumerationTruncated);
	TestTrue(TEXT("High fanout never exceeds entry work bound"), Enumerated.Num() <= 4);
	for (int32 Index = 1; Index < Enumerated.Num(); ++Index)
	{
		TestTrue(TEXT("Observed candidates are deterministically sorted"), Enumerated[Index - 1] < Enumerated[Index]);
	}

#if PLATFORM_WINDOWS
	const FString LinkPath = FPaths::Combine(ProjectRoot, TEXT("Source"), TEXT("ExternalLink"));
	const DWORD LinkFlags = SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2u;
	if (::CreateSymbolicLinkW(*LinkPath, *OutsideRoot, LinkFlags))
	{
		TestFalse(
			TEXT("Final-handle containment rejects an external directory symlink"),
			FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
				ProjectRoot, FPaths::Combine(LinkPath, TEXT("Secret.cpp")), RelativePath));
		Enumerated.Reset();
		bEnumerationTruncated = false;
		EnumerationCode.Reset();
		TestTrue(
			TEXT("Traversal remains available after rejecting a reparse entry"),
			FHyperAIStudioContextSearchContracts::EnumerateProjectCppSourceFiles(
				ProjectRoot, 64, 128, Enumerated, bEnumerationTruncated, EnumerationCode));
		TestTrue(TEXT("Rejected reparse traversal is explicitly partial"), bEnumerationTruncated);
		TestFalse(TEXT("External symlink target is never enumerated"), Enumerated.ContainsByPredicate([&LinkPath](const FString& Path)
		{
			return Path.StartsWith(LinkPath, ESearchCase::IgnoreCase);
		}));
		TestTrue(TEXT("Directory symlink removed without following it"), ::RemoveDirectoryW(*LinkPath) != 0);
	}
	else
	{
		AddInfo(TEXT("Directory symlink creation was unavailable; Win64 final-handle containment test was skipped best-effort."));
	}

	const FString HardlinkPath = FPaths::Combine(ProjectRoot, TEXT("Source"), TEXT("ExternalHardlink.cpp"));
	if (::CreateHardLinkW(*HardlinkPath, *OutsidePath, nullptr))
	{
		TestFalse(
			TEXT("Project-local hardlink identity is rejected by containment policy"),
			FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
				ProjectRoot, HardlinkPath, RelativePath));
		TestFalse(
			TEXT("Same-handle source read rejects multiply-linked source identity"),
			FHyperAIStudioContextSearchContracts::ReadProjectContainedCppSource(
				ProjectRoot, HardlinkPath, Read, ReadCode, 4096));
		TestEqual(TEXT("Hardlink rejection has a stable code"), ReadCode, FString(TEXT("source_hardlink_rejected")));
		TestTrue(TEXT("Hardlink fixture removed without touching target"), ::DeleteFileW(*HardlinkPath) != 0);
	}
	else
	{
		AddInfo(TEXT("Hardlink creation was unavailable; Win64 multiply-linked source identity test was skipped best-effort."));
	}
#endif

	const FString InvalidUtf8Path = FPaths::Combine(SourceDirectory, TEXT("InvalidUtf8.cpp"));
	TArray<uint8> InvalidUtf8 = { 0xc0u, 0xafu };
	TestTrue(TEXT("Invalid UTF-8 fixture saved"), FFileHelper::SaveArrayToFile(InvalidUtf8, *InvalidUtf8Path));
	TestFalse(
		TEXT("Malformed UTF-8 source fails closed"),
		FHyperAIStudioContextSearchContracts::ReadProjectContainedCppSource(
			ProjectRoot, InvalidUtf8Path, Read, ReadCode, 4096));
	TestEqual(TEXT("Malformed UTF-8 has stable rejection"), ReadCode, FString(TEXT("invalid_utf8_source")));

	bool bTruncated = false;
	const TArray<FHyperAIContextSearchValueRecord> Symbols =
		FHyperAIStudioContextSearchContracts::ExtractCppSymbols(
			TEXT("Source/Game/Hero.cpp"), Source, 16, bTruncated);
	TestFalse(TEXT("Small source is not truncated"), bTruncated);
	TestTrue(TEXT("Class symbol found"), Symbols.ContainsByPredicate([](const FHyperAIContextSearchValueRecord& Record)
	{
		return Record.Fields.ContainsByPredicate([](const FHyperAIReadField& Field)
		{
			return Field.Name == TEXT("name") && Field.Value == TEXT("FHero");
		});
	}));
	for (const FHyperAIContextSearchValueRecord& Record : Symbols)
	{
		for (const FHyperAIReadField& Field : Record.Fields)
		{
			TestFalse(TEXT("No raw source or secret value is exported"), Field.Value.Contains(Secret));
			TestFalse(TEXT("No source body is exported"), Field.Name == TEXT("source") || Field.Name == TEXT("excerpt") || Field.Name == TEXT("signature"));
			if (Field.Name == TEXT("relative_path"))
			{
				TestFalse(TEXT("Indexed source path is never absolute"), FPaths::IsRelative(Field.Value) == false);
			}
		}
	}
	bool bBounded = false;
	const TArray<FHyperAIContextSearchValueRecord> OneSymbol =
		FHyperAIStudioContextSearchContracts::ExtractCppSymbols(
			TEXT("Source/Game/Hero.cpp"), Source, 1, bBounded);
	TestEqual(TEXT("Symbol output hard bounded"), OneSymbol.Num(), 1);
	TestTrue(TEXT("Bound truncation explicit"), bBounded);

	const FString DenseNewlines = FString::ChrN(100000, TEXT('\n')) + TEXT("class FAfterDenseNewlines {};");
	bool bDenseTruncated = false;
	const TArray<FHyperAIContextSearchValueRecord> DenseSymbols =
		FHyperAIStudioContextSearchContracts::ExtractCppSymbols(
			TEXT("Source/Game/Dense.cpp"), DenseNewlines, 16, bDenseTruncated);
	TestTrue(TEXT("Newline-dense source reaches the fixed streaming line bound"), bDenseTruncated);
	TestTrue(TEXT("Newline-dense source performs no unbounded line materialization"), DenseSymbols.IsEmpty());

	TestTrue(TEXT("Fixture tree removed"), FileManager.DeleteDirectory(*FixtureBase, false, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchScalarPreviewBoundsTest,
	"HyperAIStudio.NativeTools.ContextSearch.ScalarPreviewBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchScalarPreviewBoundsTest::RunTest(const FString& Parameters)
{
	const FString Exact = FString::ChrN(
		FHyperAIStudioContextSearchContracts::MaxFieldValueCharacters,
		TEXT('x'));
	FString Preview;
	TestTrue(
		TEXT("Exact-bound FString preview is copied"),
		FHyperAIStudioContextSearchContracts::TryCopyBoundedPreview(Exact, Preview));
	TestEqual(TEXT("Exact-bound preview remains exact"), Preview.Len(), Exact.Len());

	const FString Oversized = Exact + TEXT("x");
	Preview = TEXT("must-clear");
	TestFalse(
		TEXT("Oversized FString is rejected before copy"),
		FHyperAIStudioContextSearchContracts::TryCopyBoundedPreview(Oversized, Preview));
	TestTrue(TEXT("Rejected FString produces no preview"), Preview.IsEmpty());

	const FText OversizedText = FText::FromString(Oversized);
	Preview = TEXT("must-clear");
	TestFalse(
		TEXT("Oversized FText display string is inspected by reference before copy"),
		FHyperAIStudioContextSearchContracts::TryCopyBoundedTextPreview(OversizedText, Preview));
	TestTrue(TEXT("Rejected FText produces no preview"), Preview.IsEmpty());

	static const TCHAR EmbeddedNullData[] = TEXT("before\0after");
	const FString EmbeddedNull = FString::ConstructFromPtrSize(
		EmbeddedNullData,
		UE_ARRAY_COUNT(EmbeddedNullData) - 1);
	TestEqual(TEXT("Embedded-null fixture retains the full logical range"), EmbeddedNull.Len(), 12);
	Preview = TEXT("must-clear");
	TestFalse(
		TEXT("Embedded-null scalar preview fails closed"),
		FHyperAIStudioContextSearchContracts::TryCopyBoundedPreview(EmbeddedNull, Preview));
	TestTrue(TEXT("Rejected embedded-null preview is cleared"), Preview.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchProjectLifecycleTest,
	"HyperAIStudio.NativeTools.ContextSearch.ProjectSearchLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchProjectLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	FHyperAIProjectIndexValueSnapshot Uninitialized;
	Uninitialized.Status = TEXT("uninitialized");
	FHyperAIProjectSearchRequest Request;
	Request.Query = TEXT("hero");
	Request.Fields = { TEXT("name"), TEXT("relative_path"), TEXT("package_name") };
	Request.PageSize = 1;
	Request.bStartIndexIfUnavailable = false;
	const FHyperAIReadReport Missing =
		FHyperAIStudioContextSearchContracts::SearchProjectIndex(Uninitialized, Request);
	TestFalse(TEXT("Uninitialized index is not a false success"), Missing.bOk);
	TestEqual(TEXT("Uninitialized lifecycle returned"), Missing.Status, FString(TEXT("uninitialized")));
	TestTrue(TEXT("Uninitialized is incomplete"), Missing.bIncomplete);

	FHyperAIProjectIndexValueSnapshot Ready = MakeReadyIndex();
	const FHyperAIReadReport First =
		FHyperAIStudioContextSearchContracts::SearchProjectIndex(Ready, Request);
	TestTrue(TEXT("Ready immutable index searches"), First.bOk);
	TestEqual(TEXT("Both asset and C++ symbol match hero"), First.TotalRecords, 2);
	TestEqual(TEXT("Page bound applied"), First.ReturnedRecords, 1);
	TestFalse(TEXT("Search cursor emitted"), First.NextCursor.IsEmpty());
	Request.Cursor = First.NextCursor;
	const FHyperAIReadReport Second =
		FHyperAIStudioContextSearchContracts::SearchProjectIndex(Ready, Request);
	TestTrue(TEXT("Search continuation succeeds"), Second.bOk);
	TestEqual(TEXT("Continuation returns one"), Second.ReturnedRecords, 1);
	for (const FHyperAIReadReport* Report : { &Missing, &First, &Second })
	{
		TestFalse(TEXT("No search refuses assets for want of a backend any more"),
			HasDiagnostic(*Report, TEXT("async_asset_index_backend_required")));
	}
	TestEqual(TEXT("Without an asset index the search adds no diagnostics of its own"), First.Diagnostics.Num(), 0);
	TestFalse(TEXT("Without an asset index a ready snapshot is complete"), First.bIncomplete);

	Request.Kinds = { TEXT("raw_file") };
	Request.Cursor.Reset();
	const FHyperAIReadReport UnsafeKind =
		FHyperAIStudioContextSearchContracts::SearchProjectIndex(Ready, Request);
	TestEqual(TEXT("Raw file kind fails closed"), UnsafeKind.Status, FString(TEXT("invalid_request")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchFirstSearchTest,
	"HyperAIStudio.NativeTools.ContextSearch.FirstSearchAnswers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchFirstSearchTest::RunTest(const FString& Parameters)
{
	// From no index, as in a fresh editor: the search builds it and answers, rather than saying it is being built.
	FHyperAIStudioProjectIndexStore& Store = FHyperAIStudioProjectIndexStore::Get();
	TestTrue(TEXT("no other refresh is left running"), Store.WaitForRefresh(10.0));
	Store.ResetAssetIndexForTests();
	Store.SetSnapshotForTests(FHyperAIProjectIndexValueSnapshot());
	FHyperAIProjectSearchRequest Request;
	Request.Query = TEXT("HyperAIStudio");
	Request.Kinds = { TEXT("cpp_symbol") };
	const FHyperAIReadReport First = UHyperAIStudioProjectSearchToolset::hyper_project_search(Request);
	TestNotEqual(TEXT("the first search does not answer that the index is being built"), First.Status, FString(TEXT("indexing")));
	TestTrue(FString::Printf(TEXT("it finds the project's C++ (%s, %d record(s))"), *First.Status, First.TotalRecords), First.bOk && First.TotalRecords > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchProjectIndexSharedViewTest,
	"HyperAIStudio.NativeTools.ContextSearch.ProjectIndexSharedView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchProjectIndexSharedViewTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	FHyperAIProjectIndexValueSnapshot Ready = MakeReadyIndex();
	Ready.Value.Records.Reserve(2048);
	for (int32 Index = Ready.Value.Records.Num(); Index < 2048; ++Index)
	{
		Ready.Value.Records.Add(MakeRecord(
			TEXT("cpp_symbol"),
			*FString::Printf(TEXT("symbol-%04d"), Index),
			*FString::Printf(TEXT("Symbol%04d"), Index),
			TEXT("relative_path"),
			TEXT("Source/Game/Generated.cpp")));
	}
	Ready.SnapshotFingerprint =
		FHyperAIStudioContextSearchContracts::ComputeSnapshotFingerprint(Ready.Value);

	FHyperAIStudioProjectIndexStore& Store = FHyperAIStudioProjectIndexStore::Get();
	Store.SetSnapshotForTests(Ready);
	const auto First = Store.GetSnapshotShared();
	const auto Second = Store.GetSnapshotShared();
	TestTrue(TEXT("Unchanged index generation reuses one immutable snapshot"), &First.Get() == &Second.Get());
	TestEqual(TEXT("Shared snapshot retains the complete index"), First->Value.Records.Num(), 2048);

	const double SharedStart = FPlatformTime::Seconds();
	for (int32 Iteration = 0; Iteration < 1000; ++Iteration)
	{
		const auto View = Store.GetSnapshotShared();
		if (View->Value.Records.Num() != 2048)
		{
			AddError(TEXT("Shared index view changed during the read-only micro benchmark."));
			break;
		}
	}
	const double SharedMicroseconds = (FPlatformTime::Seconds() - SharedStart) * 1000000.0 / 1000.0;
	const double CopyStart = FPlatformTime::Seconds();
	for (int32 Iteration = 0; Iteration < 20; ++Iteration)
	{
		const FHyperAIProjectIndexValueSnapshot Copy = Store.GetSnapshot();
		if (Copy.Value.Records.Num() != 2048)
		{
			AddError(TEXT("Copied index changed during the read-only micro benchmark."));
			break;
		}
	}
	const double CopyMicroseconds = (FPlatformTime::Seconds() - CopyStart) * 1000000.0 / 20.0;
	AddInfo(FString::Printf(
		TEXT("Project-index access micro benchmark: shared %.3f us/call; deep-copy %.3f us/call."),
		SharedMicroseconds,
		CopyMicroseconds));

	FHyperAIProjectIndexValueSnapshot Cleared;
	Store.SetSnapshotForTests(Cleared);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchBatchTest,
	"HyperAIStudio.NativeTools.ContextSearch.BatchAllowlistAndSnapshotBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchBatchTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	FHyperAIContextSearchValueSnapshot Editor;
	Editor.SnapshotUtc = TEXT("2026-08-14T10:00:00.000Z");
	Editor.ObservationScope = TEXT("loaded_editor_context");
	Editor.Records.Add(MakeRecord(TEXT("project"), TEXT("project"), TEXT("Game"), TEXT("project_name"), TEXT("Game")));
	FHyperAIBatchQueryRequest Unsafe;
	FHyperAIBatchReadOperation UnsafeRead;
	UnsafeRead.QueryId = TEXT("unsafe");
	UnsafeRead.Primitive = TEXT("execute_tool");
	Unsafe.Reads.Add(UnsafeRead);
	const FHyperAIReadReport Rejected =
		FHyperAIStudioContextSearchContracts::AnalyzeBatch(Editor, MakeReadyIndex(), Unsafe);
	TestEqual(TEXT("Raw dispatch primitive rejected"), Rejected.Status, FString(TEXT("invalid_request")));
	TestTrue(TEXT("Allowlist diagnostic returned"), HasDiagnostic(Rejected, TEXT("batch_primitive_not_allowlisted")));

	FHyperAIBatchQueryRequest Batch;
	Batch.PageSize = 2;
	FHyperAIBatchReadOperation Context;
	Context.QueryId = TEXT("context-now");
	Context.Primitive = TEXT("context");
	Context.Fields = { TEXT("project_name") };
	Context.MaxItems = 2;
	Batch.Reads.Add(Context);
	FHyperAIBatchReadOperation Search;
	Search.QueryId = TEXT("find-hero");
	Search.Primitive = TEXT("project_search");
	Search.Query = TEXT("hero");
	Search.Fields = { TEXT("name"), TEXT("package_name"), TEXT("relative_path") };
	Search.MaxItems = 2;
	Batch.Reads.Add(Search);
	const FHyperAIReadReport First =
		FHyperAIStudioContextSearchContracts::AnalyzeBatch(Editor, MakeReadyIndex(), Batch);
	TestTrue(TEXT("Allowlisted compound read succeeds"), First.bOk);
	TestFalse(TEXT("Batch request fingerprint bound"), First.RequestFingerprint.IsEmpty());
	TestFalse(TEXT("Batch snapshot fingerprint bound"), First.SnapshotFingerprint.IsEmpty());
	for (const FHyperAIReadRecord& Record : First.Records)
	{
		TestTrue(TEXT("Every batch record names its query"), Record.QueryId == TEXT("context-now") || Record.QueryId == TEXT("find-hero"));
	}
	Batch.Reads[0].Fields = { TEXT("project_root") };
	Batch.Cursor = First.NextCursor;
	const FHyperAIReadReport ChangedRequest =
		FHyperAIStudioContextSearchContracts::AnalyzeBatch(Editor, MakeReadyIndex(), Batch);
	TestEqual(TEXT("Changed batch projection invalidates cursor"), ChangedRequest.Status, FString(TEXT("invalid_request")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchSceneValueValidationTest,
	"HyperAIStudio.NativeTools.ContextSearch.SceneValueValidationAndZeroEffectPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchSceneValueValidationTest::RunTest(const FString& Parameters)
{
	FHyperAIContextSearchValueSnapshot Snapshot;
	Snapshot.ObservationScope = TEXT("loaded_editor_scene");
	FHyperAIContextSearchValueRecord Actor;
	Actor.Kind = TEXT("actor");
	Actor.RecordId = TEXT("/Game/Maps/Test.Test:PersistentLevel.ActorA");
	Actor.Fields = {
		{TEXT("path"), TEXT("object_path"), Actor.RecordId},
		{TEXT("class_path"), TEXT("class_path"), TEXT("/Script/Engine.Actor")},
		{TEXT("location"), TEXT("vector3"), TEXT("0,0,0")},
		{TEXT("rotation"), TEXT("rotator"), TEXT("0,0,0")},
		{TEXT("scale"), TEXT("vector3"), TEXT("1,1,1")}};
	Snapshot.Records.Add(Actor);
	FHyperAIContextSearchValueRecord Component;
	Component.Kind = TEXT("component");
	Component.RecordId = Actor.RecordId + TEXT(".Root");
	Component.Fields = {
		{TEXT("path"), TEXT("object_path"), Component.RecordId},
		{TEXT("owner_path"), TEXT("object_path"), Actor.RecordId},
		{TEXT("class_path"), TEXT("class_path"), TEXT("/Script/Engine.SceneComponent")}};
	Snapshot.Records.Add(Component);
	bool bTruncated = false;
	const TArray<FHyperAIReadDiagnostic> ValidIssues =
		FHyperAIStudioContextSearchContracts::ValidateSceneSnapshot(Snapshot, 16, bTruncated);
	TestFalse(TEXT("closed value snapshot has no validation error"),
		ValidIssues.ContainsByPredicate([](const FHyperAIReadDiagnostic& Issue)
		{
			return Issue.Severity == TEXT("error");
		}));

	Snapshot.Records[1].Fields[1].Value = TEXT("/Game/Maps/Test.Test:PersistentLevel.Missing");
	const TArray<FHyperAIReadDiagnostic> CorruptIssues =
		FHyperAIStudioContextSearchContracts::ValidateSceneSnapshot(Snapshot, 16, bTruncated);
	TestTrue(TEXT("orphan component is independently rejected"),
		CorruptIssues.ContainsByPredicate([](const FHyperAIReadDiagnostic& Issue)
		{
			return Issue.Code == TEXT("component_owner_invalid")
				&& Issue.Severity == TEXT("error");
		}));

	FHyperAISceneApplyPlanRequest InvalidPlan;
	FHyperAISceneLayoutOperation Layout;
	Layout.ActorPaths = {Actor.RecordId, TEXT("/Game/Maps/Test.Test:PersistentLevel.ActorB")};
	Layout.ExpectedActorRevisions = {TEXT("sha256:") + FString::ChrN(64, TEXT('a'))};
	InvalidPlan.Operations.Add(Layout);
	const FHyperAISceneApplyPlanReport InvalidReport =
		UHyperAIStudioSceneInspectToolset::hyper_scene_apply_plan(InvalidPlan);
	TestFalse(TEXT("invalid layout never reports success"), InvalidReport.bOk);
	TestFalse(TEXT("invalid layout never stages"), InvalidReport.bStaged);
	TestFalse(TEXT("invalid layout never submits"), InvalidReport.bExecutionSubmitted);
	TestEqual(TEXT("parallel CAS cardinality is enforced"), InvalidReport.Status,
		FString(TEXT("invalid_operation")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchToolsetSchemaTest,
	"HyperAIStudio.NativeTools.ContextSearch.ToolsetSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchToolsetSchemaTest::RunTest(const FString& Parameters)
{
	const struct { UClass* Class; const TCHAR* Function; } Cases[] = {
		{ UHyperAIStudioBatchQueryToolset::StaticClass(), TEXT("hyper_batch_query") },
		{ UHyperAIStudioContextSnapshotToolset::StaticClass(), TEXT("hyper_context_snapshot") },
		{ UHyperAIStudioSceneInspectToolset::StaticClass(), TEXT("hyper_scene_inspect") },
		{ UHyperAIStudioSceneInspectToolset::StaticClass(), TEXT("hyper_scene_apply_plan") },
		{ UHyperAIStudioSceneInspectToolset::StaticClass(), TEXT("hyper_scene_validate") },
		{ UHyperAIStudioProjectSearchToolset::StaticClass(), TEXT("hyper_project_search") },
		{ UHyperAIStudioProjectIndexStatusToolset::StaticClass(), TEXT("hyper_project_index_status") }
	};
	for (const auto& Case : Cases)
	{
		UFunction* Function = Case.Class->FindFunctionByName(Case.Function);
		TestNotNull(TEXT("Tool function exists"), Function);
		if (Function)
		{
			const TValueOrError<bool, FString> Callability = UToolsetDefinition::IsFunctionAICallable(Function);
			TestTrue(TEXT("Tool schema validation has a value"), Callability.HasValue());
			if (Callability.HasValue())
			{
				TestTrue(TEXT("Tool function is AICallable"), Callability.GetValue());
			}
		}
	}
	TestTrue(TEXT("Batch records expose QueryId"), FHyperAIReadRecord::StaticStruct()->FindPropertyByName(TEXT("QueryId")) != nullptr);
	TestTrue(TEXT("Report exposes request fingerprint"), FHyperAIReadReport::StaticStruct()->FindPropertyByName(TEXT("RequestFingerprint")) != nullptr);
	TestTrue(TEXT("Report exposes snapshot fingerprint"), FHyperAIReadReport::StaticStruct()->FindPropertyByName(TEXT("SnapshotFingerprint")) != nullptr);
	TestTrue(TEXT("Report exposes incomplete semantics"), FHyperAIReadReport::StaticStruct()->FindPropertyByName(TEXT("bIncomplete")) != nullptr);
	TestTrue(TEXT("Report exposes on-disk semantics"), FHyperAIReadReport::StaticStruct()->FindPropertyByName(TEXT("bOnDiskOnly")) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchFindsAssetsTest,
	"HyperAIStudio.NativeTools.ContextSearch.ProjectSearchFindsAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchFindsAssetsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	const FHyperAIProjectIndexValueSnapshot Cpp = MakeReadyIndex();
	const FHyperAIProjectAssetIndexSnapshot Assets = MakeAssetSnapshot(MakeRockAndUnarmedRows());
	FHyperAIProjectSearchRequest Request;
	Request.bStartIndexIfUnavailable = false;
	Request.Query = TEXT("rock staticmesh");
	const FHyperAIReadReport Rock = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, Assets);
	TestTrue(TEXT("the search succeeds"), Rock.bOk);
	TestEqual(TEXT("name and class terms find exactly the rock"), Rock.TotalRecords, 1);
	if (Rock.Records.Num() == 1)
	{
		const FHyperAIReadRecord& Record = Rock.Records[0];
		TestEqual(TEXT("it is an asset record"), Record.Kind, FString(TEXT("asset")));
		TestEqual(TEXT("its id is the object path"), Record.RecordId, FString(TEXT("/Game/Env/SM_Rock.SM_Rock")));
		TestEqual(TEXT("name"), FindField(Record, TEXT("name")), FString(TEXT("SM_Rock")));
		TestEqual(TEXT("asset_name is kept for older callers"), FindField(Record, TEXT("asset_name")), FString(TEXT("SM_Rock")));
		TestEqual(TEXT("asset_class_path"), FindField(Record, TEXT("asset_class_path")), FString(TEXT("/Script/Engine.StaticMesh")));
		TestEqual(TEXT("package_name"), FindField(Record, TEXT("package_name")), FString(TEXT("/Game/Env/SM_Rock")));
		TestEqual(TEXT("object_path"), FindField(Record, TEXT("object_path")), FString(TEXT("/Game/Env/SM_Rock.SM_Rock")));
	}
	TestFalse(TEXT("with both parts ready the answer is complete"), Rock.bIncomplete);
	TestEqual(TEXT("and says so"), Rock.Status, FString(TEXT("complete")));

	Request.Query = TEXT("unarmed");
	const FHyperAIReadReport Unarmed = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, Assets);
	TestEqual(TEXT("a folder word finds the asset inside it"), Unarmed.TotalRecords, 1);
	TestTrue(TEXT("it is the idle animation"), Unarmed.Records.Num() == 1
		&& Unarmed.Records[0].RecordId == TEXT("/Game/Anim/Unarmed/AS_Unarmed_Idle.AS_Unarmed_Idle"));

	Request.Query = TEXT("engine.character");
	const FHyperAIReadReport Derived = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, Assets);
	TestEqual(TEXT("a Blueprint is found by its parent class"), Derived.TotalRecords, 1);
	TestTrue(TEXT("and reports that parent by default"), Derived.Records.Num() == 1
		&& FindField(Derived.Records[0], TEXT("parent_class_path")) == TEXT("/Script/Engine.Character"));

	Request.Query = TEXT("rock");
	Request.Kinds = { TEXT("cpp_symbol") };
	const FHyperAIReadReport CppOnly = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, Assets);
	TestTrue(TEXT("a C++-only search succeeds"), CppOnly.bOk);
	TestEqual(TEXT("a C++-only search returns no assets"), CppOnly.TotalRecords, 0);

	Request.Kinds = { TEXT("asset") };
	Request.Fields = { TEXT("asset_class_path") };
	const FHyperAIReadReport Projected = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, Assets);
	TestEqual(TEXT("the projection keeps one record"), Projected.ReturnedRecords, 1);
	TestTrue(TEXT("only the requested field is returned"), Projected.Records.Num() == 1
		&& Projected.Records[0].Fields.Num() == 1
		&& Projected.Records[0].Fields[0].Name == TEXT("asset_class_path"));

	// An asset-only search does not inherit the C++ pass's bounds.
	FHyperAIProjectIndexValueSnapshot PartialCpp = MakeReadyIndex();
	PartialCpp.Status = TEXT("partial");
	PartialCpp.Value.bIncomplete = true;
	Request.Fields.Reset();
	const FHyperAIReadReport AssetOnly = FHyperAIStudioContextSearchContracts::SearchProjectIndex(PartialCpp, Request, Assets);
	TestFalse(TEXT("an asset-only search over a ready asset index is complete"), AssetOnly.bIncomplete);
	Request.Kinds.Reset();
	const FHyperAIReadReport Both = FHyperAIStudioContextSearchContracts::SearchProjectIndex(PartialCpp, Request, Assets);
	TestTrue(TEXT("a search that includes C++ still reports its partial pass"), Both.bIncomplete);

	FHyperAIProjectAssetIndexSnapshot Indexing = MakeAssetSnapshot({}, TEXT("indexing"));
	const FHyperAIProjectIndexValueSnapshot NoCpp;
	Request.Kinds = { TEXT("asset") };
	const FHyperAIReadReport Building = FHyperAIStudioContextSearchContracts::SearchProjectIndex(NoCpp, Request, Indexing);
	TestEqual(TEXT("an empty asset index mid-walk answers indexing"), Building.Status, FString(TEXT("indexing")));
	TestTrue(TEXT("and incomplete"), Building.bIncomplete);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchAssetMatchBoundTest,
	"HyperAIStudio.NativeTools.ContextSearch.ProjectSearchAssetMatchBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchAssetMatchBoundTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	TArray<FHyperAIProjectAssetRow> Rows;
	Rows.Reserve(5000);
	for (int32 Index = 0; Index < 5000; ++Index)
	{
		Rows.Add(MakeAssetRow(
			FString::Printf(TEXT("/Game/Bulk/F%02d/SM_%04d.SM_%04d"), Index / 100, Index, Index),
			TEXT("/Script/Engine.StaticMesh")));
	}
	const FHyperAIProjectAssetIndexSnapshot Assets = MakeAssetSnapshot(Rows);
	FHyperAIProjectSearchRequest Request;
	Request.bStartIndexIfUnavailable = false;
	Request.Query = TEXT("sm");
	Request.Kinds = { TEXT("asset") };
	const FHyperAIReadReport Report =
		FHyperAIStudioContextSearchContracts::SearchProjectIndex(MakeReadyIndex(), Request, Assets);
	TestNotEqual(TEXT("the record bound of a page is never hit"), Report.Status, FString(TEXT("invalid_request")));
	TestTrue(TEXT("the bounded search succeeds"), Report.bOk);
	TestEqual(TEXT("matches stop at the fixed bound"), Report.TotalRecords,
		FHyperAIStudioContextSearchContracts::MaxProjectSearchMatches);
	TestTrue(TEXT("the source is reported truncated"), Report.bSourceTruncated);
	TestTrue(TEXT("the bound has a stable diagnostic"), HasDiagnostic(Report, TEXT("project_search_match_bound_reached")));
	TestFalse(TEXT("a continuation cursor is offered"), Report.NextCursor.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchAssetCursorTest,
	"HyperAIStudio.NativeTools.ContextSearch.ProjectSearchCursorBindsAssetGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchAssetCursorTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	TArray<FHyperAIProjectAssetRow> Rows = {
		MakeAssetRow(TEXT("/Game/Props/SM_Crate.SM_Crate"), TEXT("/Script/Engine.StaticMesh")),
		MakeAssetRow(TEXT("/Game/Props/SM_Barrel.SM_Barrel"), TEXT("/Script/Engine.StaticMesh")),
		MakeAssetRow(TEXT("/Game/Props/SM_Lamp.SM_Lamp"), TEXT("/Script/Engine.StaticMesh"))
	};
	const FHyperAIProjectAssetIndexSnapshot G1 = MakeAssetSnapshot(Rows, TEXT("ready"), 1);
	Rows.Add(MakeAssetRow(TEXT("/Game/Props/SM_Table.SM_Table"), TEXT("/Script/Engine.StaticMesh")));
	const FHyperAIProjectAssetIndexSnapshot G2 = MakeAssetSnapshot(Rows, TEXT("ready"), 2);
	const FHyperAIProjectIndexValueSnapshot Cpp = MakeReadyIndex();

	FHyperAIProjectSearchRequest Request;
	Request.bStartIndexIfUnavailable = false;
	Request.Query = TEXT("props");
	Request.Kinds = { TEXT("asset") };
	Request.PageSize = 1;
	const FHyperAIReadReport First = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, G1);
	TestEqual(TEXT("three props match"), First.TotalRecords, 3);
	TestFalse(TEXT("a cursor is issued"), First.NextCursor.IsEmpty());

	Request.Cursor = First.NextCursor;
	const FHyperAIReadReport Same = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, G1);
	TestTrue(TEXT("the cursor continues on the same asset generation"), Same.bOk);
	TestEqual(TEXT("and returns the next record"), Same.ReturnedRecords, 1);

	const FHyperAIReadReport Moved = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, G2);
	TestEqual(TEXT("a new matching asset invalidates the cursor"), Moved.Status, FString(TEXT("invalid_request")));
	TestTrue(TEXT("through the snapshot fingerprint"), HasDiagnostic(Moved, TEXT("cursor_snapshot_changed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchStatusCompositionTest,
	"HyperAIStudio.NativeTools.ContextSearch.ProjectIndexStatusComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchStatusCompositionTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	FHyperAIStudioProjectIndexStore& Store = FHyperAIStudioProjectIndexStore::Get();
	TestTrue(TEXT("no other refresh is left running"), Store.WaitForRefresh(10.0));
	const FHyperAIProjectIndexValueSnapshot Cpp = MakeReadyIndex();
	Store.SetSnapshotForTests(Cpp);
	const FHyperAIProjectAssetIndexSnapshot ReadyAssets = MakeAssetSnapshot(MakeRockAndUnarmedRows());
	Store.SetAssetSnapshotForTests(ReadyAssets);

	const FHyperAIProjectIndexStatus Ready = Store.GetStatus();
	TestEqual(TEXT("C++ ready and assets ready make ready"), Ready.Status, FString(TEXT("ready")));
	TestFalse(TEXT("ready is complete"), Ready.bIncomplete);
	TestTrue(TEXT("ready is ok"), Ready.bOk);
	TestEqual(TEXT("asset count comes from the asset index"), Ready.AssetRecordCount, 3);
	TestEqual(TEXT("total counts both parts"), Ready.TotalRecordCount, Cpp.Value.Records.Num() + 3);
	TestEqual(TEXT("the asset walk's own status is reported"), Ready.AssetIndexStatus, FString(TEXT("ready")));
	TestEqual(TEXT("asset folders are reported"), Ready.AssetFolderCount, ReadyAssets.FolderCount);
	TestTrue(TEXT("the composite fingerprint is canonical"),
		Ready.SnapshotFingerprint.StartsWith(TEXT("sha256:")) && Ready.SnapshotFingerprint.Len() == 71);

	FHyperAIProjectAssetIndexSnapshot ChangedAssets = ReadyAssets;
	ChangedAssets.Fingerprint = FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("changed") });
	Store.SetAssetSnapshotForTests(ChangedAssets);
	TestNotEqual(TEXT("an asset change moves the composite fingerprint"),
		Store.GetStatus().SnapshotFingerprint, Ready.SnapshotFingerprint);

	FHyperAIProjectAssetIndexSnapshot PartialAssets = ReadyAssets;
	PartialAssets.Status = TEXT("partial");
	Store.SetAssetSnapshotForTests(PartialAssets);
	TestEqual(TEXT("partial assets make partial"), Store.GetStatus().Status, FString(TEXT("partial")));

	FHyperAIProjectAssetIndexSnapshot WaitingAssets = MakeAssetSnapshot({}, TEXT("waiting_for_registry"));
	Store.SetAssetSnapshotForTests(WaitingAssets);
	const FHyperAIProjectIndexStatus Waiting = Store.GetStatus();
	TestEqual(TEXT("a registry still scanning keeps the index indexing"), Waiting.Status, FString(TEXT("indexing")));
	TestTrue(TEXT("but the C++ records are usable"), Waiting.bOk);
	TestTrue(TEXT("and it is incomplete"), Waiting.bIncomplete);

	Store.ResetAssetIndexForTests();
	Store.SetSnapshotForTests(FHyperAIProjectIndexValueSnapshot());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchAssetBatchTest,
	"HyperAIStudio.NativeTools.ContextSearch.BatchWithAssetIndex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchAssetBatchTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	FHyperAIContextSearchValueSnapshot Editor;
	Editor.SnapshotUtc = TEXT("2026-08-14T10:00:00.000Z");
	Editor.ObservationScope = TEXT("loaded_editor_context");
	const FHyperAIProjectIndexValueSnapshot Cpp = MakeReadyIndex();
	const FHyperAIProjectAssetIndexSnapshot Assets = MakeAssetSnapshot(MakeRockAndUnarmedRows());

	FHyperAIBatchQueryRequest Batch;
	FHyperAIBatchReadOperation Status;
	Status.QueryId = TEXT("status");
	Status.Primitive = TEXT("project_index_status");
	Batch.Reads.Add(Status);
	FHyperAIBatchReadOperation Search;
	Search.QueryId = TEXT("find-rock");
	Search.Primitive = TEXT("project_search");
	Search.Query = TEXT("rock");
	Search.Kinds = { TEXT("asset") };
	Batch.Reads.Add(Search);

	const FHyperAIReadReport WithAssets = FHyperAIStudioContextSearchContracts::AnalyzeBatch(Editor, Cpp, Assets, Batch);
	TestTrue(TEXT("the batch succeeds"), WithAssets.bOk);
	const FHyperAIReadRecord* StatusRecord = WithAssets.Records.FindByPredicate([](const FHyperAIReadRecord& Record)
	{
		return Record.QueryId == TEXT("status");
	});
	TestNotNull(TEXT("the status read answers"), StatusRecord);
	if (StatusRecord)
	{
		TestEqual(TEXT("asset_count counts the asset rows"), FindField(*StatusRecord, TEXT("asset_count")), FString(TEXT("3")));
		TestEqual(TEXT("total_record_count includes them"), FindField(*StatusRecord, TEXT("total_record_count")),
			LexToString(Cpp.Value.Records.Num() + 3));
		TestEqual(TEXT("the composite status is ready"), FindField(*StatusRecord, TEXT("status")), FString(TEXT("ready")));
	}
	TestTrue(TEXT("the search read finds the rock, tagged with its query"),
		WithAssets.Records.ContainsByPredicate([](const FHyperAIReadRecord& Record)
		{
			return Record.QueryId == TEXT("find-rock") && Record.Kind == TEXT("asset")
				&& Record.RecordId == TEXT("/Game/Env/SM_Rock.SM_Rock");
		}));

	const FHyperAIReadReport WithoutAssets = FHyperAIStudioContextSearchContracts::AnalyzeBatch(Editor, Cpp, Batch);
	const FHyperAIReadRecord* LegacyStatus = WithoutAssets.Records.FindByPredicate([](const FHyperAIReadRecord& Record)
	{
		return Record.QueryId == TEXT("status");
	});
	TestTrue(TEXT("without an asset index asset_count falls back to the value snapshot"),
		LegacyStatus && FindField(*LegacyStatus, TEXT("asset_count")) == TEXT("1"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchFirstSearchFindsAssetsTest,
	"HyperAIStudio.NativeTools.ContextSearch.FirstSearchFindsAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchFirstSearchFindsAssetsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	// From no index at all: the first search walks the project's assets inside its wait and finds a real one.
	FHyperAIStudioProjectIndexStore& Store = FHyperAIStudioProjectIndexStore::Get();
	TestTrue(TEXT("no other refresh is left running"), Store.WaitForRefresh(10.0));
	Store.ResetAssetIndexForTests();
	Store.SetSnapshotForTests(FHyperAIProjectIndexValueSnapshot());

	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry || Registry->IsLoadingAssets())
	{
		AddInfo(TEXT("The Asset Registry is not ready in this run; the live asset search check was skipped."));
		return true;
	}
	FAssetData Target;
	TArray<FName> Queue = { FName(TEXT("/Game")) };
	for (int32 Head = 0; Head < Queue.Num() && Head < 256 && !Target.IsValid(); ++Head)
	{
		const FName Folder = Queue[Head];
		if (FHyperAIStudioProjectAssetIndex::IsExcludedFolder(Folder.ToString()))
		{
			continue;
		}
		TArray<FAssetData> Assets;
		Registry->GetAssetsByPath(Folder, Assets, false, true);
		for (const FAssetData& Asset : Assets)
		{
			if (!Asset.IsRedirector() && Asset.PackageName.ToString().Len() <= 128)
			{
				Target = Asset;
				break;
			}
		}
		TArray<FName> Children;
		Registry->GetSubPaths(Folder, Children, false);
		Children.Sort(FNameLexicalLess());
		Queue.Append(Children);
	}
	if (!Target.IsValid())
	{
		AddInfo(TEXT("The project has no on-disk asset under /Game; the live asset search check was skipped."));
		return true;
	}

	FHyperAIProjectSearchRequest Request;
	Request.Query = Target.PackageName.ToString();
	Request.Kinds = { TEXT("asset") };
	Request.PageSize = 128;
	const double StartSeconds = FPlatformTime::Seconds();
	const FHyperAIReadReport Report = UHyperAIStudioProjectSearchToolset::hyper_project_search(Request);
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	TestTrue(FString::Printf(TEXT("the first asset search succeeds (%s)"), *Report.Status), Report.bOk);
	TestNotEqual(TEXT("it does not answer that the index is being built"), Report.Status, FString(TEXT("indexing")));
	const FString Expected = Target.GetObjectPathString();
	TestTrue(FString::Printf(TEXT("it finds %s among %d record(s)"), *Expected, Report.TotalRecords),
		Report.Records.ContainsByPredicate([&Expected](const FHyperAIReadRecord& Record)
		{
			return Record.RecordId == Expected;
		}));
	const FHyperAIProjectIndexStatus Status = Store.GetStatus();
	AddInfo(FString::Printf(TEXT("First asset search took %.1f ms; the asset index holds %d assets in %d folders (%s)."),
		ElapsedMs, Status.AssetRecordCount, Status.AssetFolderCount, *Status.AssetIndexStatus));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchAssetTimingTest,
	"HyperAIStudio.NativeTools.ContextSearch.ProjectSearchAssetTiming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchAssetTimingTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	// Information only: how long a selective search takes at a large project's scale, against the 10 ms bound.
	TArray<FHyperAIProjectAssetRow> Rows;
	Rows.Reserve(20000);
	static const TCHAR* const Kinds[] = { TEXT("Rock"), TEXT("Tree"), TEXT("Crate"), TEXT("Lamp"), TEXT("Wall") };
	for (int32 Index = 0; Index < 20000; ++Index)
	{
		Rows.Add(MakeAssetRow(
			FString::Printf(TEXT("/Game/Content%03d/SM_%s_%05d.SM_%s_%05d"),
				Index / 64, Kinds[Index % 5], Index, Kinds[Index % 5], Index),
			Index % 7 == 0 ? TEXT("/Script/Engine.SkeletalMesh") : TEXT("/Script/Engine.StaticMesh")));
	}
	const FHyperAIProjectAssetIndexSnapshot Assets = MakeAssetSnapshot(Rows);
	FHyperAIProjectIndexValueSnapshot Cpp = MakeReadyIndex();
	for (int32 Index = Cpp.Value.Records.Num(); Index < FHyperAIStudioContextSearchContracts::MaxCppSymbols; ++Index)
	{
		Cpp.Value.Records.Add(MakeRecord(
			TEXT("cpp_symbol"),
			*FString::Printf(TEXT("symbol-%05d"), Index),
			*FString::Printf(TEXT("Symbol%05d"), Index),
			TEXT("relative_path"),
			TEXT("Source/Game/Generated.cpp")));
	}
	FHyperAIProjectSearchRequest Request;
	Request.bStartIndexIfUnavailable = false;
	Request.Query = TEXT("rock skeletalmesh");
	const double StartSeconds = FPlatformTime::Seconds();
	const FHyperAIReadReport Report = FHyperAIStudioContextSearchContracts::SearchProjectIndex(Cpp, Request, Assets);
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("Search over %d asset rows and %d C++ records took %.2f ms, %d match(es); work bound %s."),
		Rows.Num(), Cpp.Value.Records.Num(), ElapsedMs, Report.TotalRecords,
		HasDiagnostic(Report, TEXT("project_search_work_bound_reached")) ? TEXT("reached") : TEXT("not reached")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
