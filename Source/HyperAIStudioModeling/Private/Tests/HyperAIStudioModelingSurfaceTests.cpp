// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioModelingBake.h"
#include "HyperAIStudioModelingToolset.h"
#include "HyperAIStudioModelingUV.h"
#include "HyperAIStudioOperationWait.h"
#include "HyperAIStudioResultPreview.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSourceControl.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"

namespace HyperAIStudio::ModelingSurfaceTests
{
	using C = FHyperAIStudioModelingContracts;
	namespace UV = HyperAIStudio::Modeling::UV;
	namespace Bake = HyperAIStudio::Modeling::Bake;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** Runs Body with approval off, as an agent with one-shot edits allowed would. */
	template <typename FunctionType>
	auto WithoutApproval(FunctionType&& Body)
	{
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bApproval = Settings->bRequireApprovalForAgentEdits;
		Settings->bRequireApprovalForAgentEdits = false;
		auto Result = Body();
		Settings->bRequireApprovalForAgentEdits = bApproval;
		return Result;
	}

	FString TestFolder()
	{
		return TEXT("/Game/__HyperAIStudioTests/Surface_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	}

	void DeleteFolder(const FString& Folder)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Folder), Assets, /*bRecursive=*/true);
		TArray<UObject*> Objects;
		for (const FAssetData& Asset : Assets)
		{
			if (UObject* Object = Asset.GetAsset()) Objects.Add(Object);
			IFileManager::Get().Delete(*FHyperAIStudioResultPreview::GetAssetPreviewPath(Asset.GetObjectPathString()), false, false, true);
		}
		if (!Objects.IsEmpty()) ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
	}

	FHyperAIModelOp Op(const TCHAR* Kind, const TCHAR* Mesh, const TCHAR* Params = TEXT(""), const TCHAR* Size = TEXT(""), const TCHAR* Location = TEXT(""))
	{
		FHyperAIModelOp Out;
		Out.Kind = Kind;
		Out.Mesh = Mesh;
		Out.Params = Params;
		Out.Size = Size;
		Out.Location = Location;
		return Out;
	}

	/** Writes a static mesh with a model plan; null when the plan failed. */
	UStaticMesh* Model(const FString& Path, TArray<FHyperAIModelOp> Ops, FString& OutError)
	{
		FHyperAIModelOp Write = Op(TEXT("write_asset"), *Ops.Last().Mesh, TEXT("Collision=box"));
		Write.Asset = Path;
		Ops.Add(Write);
		FHyperAIModelPlanRequest Request;
		Request.Operations = MoveTemp(Ops);
		const FHyperAIModelPlanReport Report = WithoutApproval([&Request] { return C::ApplyPlan(Request); });
		OutError = Report.Diagnostic;
		if (!Report.bOk) return nullptr;
		return LoadObject<UStaticMesh>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** A 100 cm box whose six faces are box-projected onto the same tile: every face overlaps every other. */
	UStaticMesh* OverlappingBox(const FString& Path, FString& OutError)
	{
		return Model(Path, {Op(TEXT("add.box"), TEXT("Crate"), TEXT(""), TEXT("100")), Op(TEXT("uv"), TEXT("Crate"), TEXT("Mode=box; Scale=100"))}, OutError);
	}

	FHyperAIModelUVCheckReport Check(const FString& Asset, const int32 Channel = 0)
	{
		FHyperAIModelUVCheckRequest Request;
		Request.Asset = Asset;
		Request.Channel = Channel;
		return C::CheckUVs(Request);
	}

	/** A square of Size cm at Offset, split on its diagonal, laid out in the UV rectangle Min..Max. */
	void Quad(UV::FSoup& Soup, const FVector3d& Offset, const double Size, const FVector2d& Min, const FVector2d& Max)
	{
		const FVector3d P00 = Offset, P10 = Offset + FVector3d(Size, 0, 0), P11 = Offset + FVector3d(Size, Size, 0), P01 = Offset + FVector3d(0, Size, 0);
		const FVector2d U00 = Min, U10(Max.X, Min.Y), U11 = Max, U01(Min.X, Max.Y);
		Soup.Add(P00, P10, P11, U00, U10, U11);
		Soup.Add(P00, P11, P01, U00, U11, U01);
	}

	float Brightness(const FColor& Pixel)
	{
		return (0.299f * Pixel.R + 0.587f * Pixel.G + 0.114f * Pixel.B) / 255.f;
	}

	/** A texture's source pixels as brightness, lowest and highest. */
	bool Range(UTexture2D& Texture, float& OutMin, float& OutMax)
	{
		TArray64<uint8> Bytes;
		if (!Texture.Source.GetMipData(Bytes, 0) || Bytes.Num() < 4) return false;
		OutMin = 1.f;
		OutMax = 0.f;
		for (int64 Index = 0; Index + 3 < Bytes.Num(); Index += 4)
		{
			const float Value = Brightness(FColor(Bytes[Index + 2], Bytes[Index + 1], Bytes[Index], 255));
			OutMin = FMath::Min(OutMin, Value);
			OutMax = FMath::Max(OutMax, Value);
		}
		return true;
	}

	UTexture2D* LoadTexture(const FString& Path)
	{
		return LoadObject<UTexture2D>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingSurfaceContractsTest, "HyperAIStudio.NativeTools.Modeling.Surface.Contracts", HyperAIStudio::ModelingSurfaceTests::Flags)

bool FHyperAIStudioModelingSurfaceContractsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingSurfaceTests;
	for (const TCHAR* Name : {C::UVCheckToolName, C::UVApplyToolName, C::BakeToolName, C::PrepareToolName})
	{
		TestTrue(*FString::Printf(TEXT("%s is one of the pack's tools"), Name), C::GetToolNames().Contains(Name));
		const UFunction* Function = UHyperAIStudioModelingToolset::StaticClass()->FindFunctionByName(FName(Name));
		if (!TestNotNull(*FString::Printf(TEXT("%s is a function of the toolset"), Name), Function)) continue;
		const FString Description = Function->GetMetaData(TEXT("ToolTip"));
		TestTrue(*FString::Printf(TEXT("%s has a description under 400 characters (%d)"), Name, Description.Len()), !Description.IsEmpty() && Description.Len() < 400);
	}
	FHyperAIStudioExtensionCohortAdmission Admission;
	TestTrue(TEXT("the mesh_modeling cohort, with the UV and bake tools, is in the generated catalog exactly: ")
		+ FHyperAIStudioExtensionRuntime::DescribeExactGeneratedCohortMismatch(C::PackId, C::AtomicCohortId, C::GetToolNames()),
		FHyperAIStudioExtensionRuntime::QueryExactGeneratedCohort(C::PackId, C::AtomicCohortId, C::GetToolNames(), Admission) && Admission.bExactCohortMatch);
	// Map names and texture set-ups.
	Bake::EMap Map;
	TestTrue(TEXT("ao is a map"), Bake::ParseMap(TEXT("AO"), Map) && Map == Bake::EMap::AO);
	TestFalse(TEXT("orm is packed, not asked for"), Bake::ParseMap(TEXT("orm"), Map));
	TestTrue(TEXT("normal maps get normal-map compression"), Bake::KindOf(Bake::EMap::Normal).Compression == TC_Normalmap);
	TestFalse(TEXT("and are linear"), Bake::KindOf(Bake::EMap::Normal).bSRGB);
	TestFalse(TEXT("occlusion is linear data"), Bake::KindOf(Bake::EMap::AO).bSRGB);
	TestFalse(TEXT("so are positions"), Bake::KindOf(Bake::EMap::Position).bSRGB);
	TestTrue(TEXT("a carried base colour is sRGB"), Bake::KindOf(Bake::EMap::Texture).bSRGB);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingSurfaceMetricsTest, "HyperAIStudio.NativeTools.Modeling.Surface.UVMetrics", HyperAIStudio::ModelingSurfaceTests::Flags)

bool FHyperAIStudioModelingSurfaceMetricsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingSurfaceTests;
	FString Verdict;
	TArray<FString> Problems;
	{
		// One square on its diagonal: the two triangles share every pixel center on that edge, and neither counts it twice.
		UV::FSoup Soup;
		Quad(Soup, FVector3d::ZeroVector, 100.0, FVector2d(0, 0), FVector2d(1, 1));
		const UV::FMetrics Metrics = UV::Analyze(Soup);
		TestEqual(TEXT("a square laid out whole is one island"), Metrics.Islands, 1);
		TestEqual(TEXT("with no overlap along its diagonal"), Metrics.OverlapPercent, 0.0);
		TestTrue(TEXT("covering the 0-1 square"), Metrics.CoveragePercent > 99.0);
		TestTrue(FString::Printf(TEXT("1 m mapped to 1024 px is 1024 texels per meter (%.1f)"), Metrics.DensityMedian), FMath::IsNearlyEqual(Metrics.DensityMedian, 1024.0, 1.0));
		UV::Judge(Metrics, Verdict, Problems);
		TestEqual(TEXT("and is good: ") + FString::Join(Problems, TEXT(" | ")), Verdict, FString(TEXT("good")));
	}
	{
		// Two squares apart in 3D, one on the other's UVs.
		UV::FSoup Soup;
		Quad(Soup, FVector3d::ZeroVector, 100.0, FVector2d(0, 0), FVector2d(1, 1));
		Quad(Soup, FVector3d(500, 0, 0), 100.0, FVector2d(0, 0), FVector2d(1, 1));
		const UV::FMetrics Metrics = UV::Analyze(Soup);
		TestEqual(TEXT("two separate squares are two islands"), Metrics.Islands, 2);
		TestTrue(FString::Printf(TEXT("stacked, all their area overlaps (%.1f%%)"), Metrics.OverlapPercent), Metrics.OverlapPercent > 99.0);
		TestTrue(TEXT("and the pair is named"), Metrics.Overlaps.Num() == 1 && Metrics.Overlaps[0].A == 1 && Metrics.Overlaps[0].B == 2);
		UV::Judge(Metrics, Verdict, Problems);
		TestEqual(TEXT("which is bad"), Verdict, FString(TEXT("bad")));
		TestTrue(TEXT("naming the overlap and its islands"), Problems.Num() > 0 && Problems[0].StartsWith(TEXT("error overlap")) && Problems[0].Contains(TEXT("1+2")));
	}
	{
		// Overlapping by a fifth of the square: 0.2 of the 1.0 they cover between them.
		UV::FSoup Soup;
		Quad(Soup, FVector3d::ZeroVector, 100.0, FVector2d(0, 0), FVector2d(0.6, 1));
		Quad(Soup, FVector3d(500, 0, 0), 100.0, FVector2d(0.4, 0), FVector2d(1, 1));
		const UV::FMetrics Metrics = UV::Analyze(Soup);
		TestTrue(FString::Printf(TEXT("a fifth overlaps (%.2f%%)"), Metrics.OverlapPercent), FMath::IsNearlyEqual(Metrics.OverlapPercent, 20.0, 0.5));
		// Their texel density differs too: 0.6 and 0.6 of a tile across 1 m.
		TestTrue(TEXT("spread stays near 1 for equal densities"), Metrics.DensitySpread < 1.1);
	}
	{
		// A triangle folded back over its neighbour across their shared edge.
		UV::FSoup Soup;
		Soup.Add(FVector3d(0, 0, 0), FVector3d(100, 0, 0), FVector3d(0, 100, 0), FVector2d(0, 0), FVector2d(1, 0), FVector2d(0, 1));
		Soup.Add(FVector3d(100, 0, 0), FVector3d(100, 100, 0), FVector3d(0, 100, 0), FVector2d(1, 0), FVector2d(0.2, 0.2), FVector2d(0, 1));
		const UV::FMetrics Metrics = UV::Analyze(Soup);
		TestEqual(TEXT("joined by their edge's UVs, one island"), Metrics.Islands, 1);
		TestEqual(TEXT("the folded triangle is flipped"), Metrics.Flipped, 1);
		TestTrue(TEXT("and the island overlaps itself"), Metrics.Overlaps.Num() == 1 && Metrics.Overlaps[0].A == Metrics.Overlaps[0].B && Metrics.OverlapPercent > 1.0);
	}
	{
		// No UV area, and UVs past 1.
		UV::FSoup Soup;
		Soup.Add(FVector3d(0, 0, 0), FVector3d(100, 0, 0), FVector3d(0, 100, 0), FVector2d(0.5, 0.5), FVector2d(0.5, 0.5), FVector2d(0.5, 0.5));
		Quad(Soup, FVector3d(500, 0, 0), 100.0, FVector2d(1.5, 1.5), FVector2d(2.5, 2.5));
		const UV::FMetrics Metrics = UV::Analyze(Soup);
		TestEqual(TEXT("a triangle on one UV point is degenerate"), Metrics.Degenerate, 1);
		TestTrue(FString::Printf(TEXT("the square past 1 is outside (%.0f%% of the triangles with UVs)"), Metrics.OutsidePercent), FMath::IsNearlyEqual(Metrics.OutsidePercent, 100.0 * 2 / 3, 0.1));
		UV::Judge(Metrics, Verdict, Problems);
		TestTrue(TEXT("outside 0-1 is named"), Problems.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("warning outside_unit")); }));
	}
	{
		// Two squares sharing a 100 cm edge in 3D, laid out apart: one seam of 100 cm, two islands.
		UV::FSoup Soup;
		Quad(Soup, FVector3d::ZeroVector, 100.0, FVector2d(0.05, 0.05), FVector2d(0.45, 0.95));
		Quad(Soup, FVector3d(100, 0, 0), 100.0, FVector2d(0.55, 0.05), FVector2d(0.95, 0.95));
		const UV::FMetrics Metrics = UV::Analyze(Soup);
		TestEqual(TEXT("split along their shared edge, two islands"), Metrics.Islands, 2);
		TestTrue(FString::Printf(TEXT("and 100 cm of seam (%.1f)"), Metrics.SeamLength), FMath::IsNearlyEqual(Metrics.SeamLength, 100.0, 0.01));
		const UV::FLightmapMetrics Lightmap = UV::AnalyzeLightmap(Soup, Metrics, 64, 1);
		TestTrue(TEXT("six texels apart at 64 px, they suit a lightmap"), Lightmap.bSuitable && Lightmap.SharedPercent == 0.0 && Lightmap.TooClose.IsEmpty());
		// Two squares apart in 3D whose layouts touch at u = 0.5: two islands with no texel between them.
		UV::FSoup Touching;
		Quad(Touching, FVector3d::ZeroVector, 100.0, FVector2d(0.05, 0.05), FVector2d(0.5, 0.95));
		Quad(Touching, FVector3d(500, 0, 0), 100.0, FVector2d(0.5, 0.05), FVector2d(0.95, 0.95));
		const UV::FMetrics TouchingMetrics = UV::Analyze(Touching);
		const UV::FLightmapMetrics Close = UV::AnalyzeLightmap(Touching, TouchingMetrics, 64, 1);
		TestTrue(TEXT("touching islands do not suit a lightmap"), !Close.bSuitable && !Close.TooClose.IsEmpty());
	}
	{
		// Different texel densities: one square at a whole tile, one at half of it.
		UV::FSoup Soup;
		Quad(Soup, FVector3d::ZeroVector, 100.0, FVector2d(0, 0), FVector2d(0.5, 1));
		Quad(Soup, FVector3d(500, 0, 0), 100.0, FVector2d(0.5, 0), FVector2d(0.75, 0.5));
		UV::FOptions Options;
		Options.TextureSize = 1024;
		const UV::FMetrics Metrics = UV::Analyze(Soup, Options);
		TestTrue(FString::Printf(TEXT("densities spread by 2 (%.2f: %.0f to %.0f)"), Metrics.DensitySpread, Metrics.DensityMin, Metrics.DensityMax),
			FMath::IsNearlyEqual(Metrics.DensitySpread, 2.0, 0.05));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingSurfaceCheckTest, "HyperAIStudio.NativeTools.Modeling.Surface.CheckAsset", HyperAIStudio::ModelingSurfaceTests::Flags)

bool FHyperAIStudioModelingSurfaceCheckTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingSurfaceTests;
	const FString Folder = TestFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };
	FString Error;
	UStaticMesh* Mesh = OverlappingBox(Folder / TEXT("SM_Crate"), Error);
	if (!TestNotNull(TEXT("a box with box-projected UVs is written: ") + Error, Mesh)) return false;
	const FHyperAIModelUVCheckReport Report = Check(Mesh->GetPathName());
	TestTrue(TEXT("the check answers: ") + Report.Diagnostic, Report.bOk);
	TestEqual(TEXT("its faces share the tile, so it is bad"), Report.Verdict, FString(TEXT("bad")));
	TestTrue(FString::Printf(TEXT("most of its UV area overlaps (%.1f%%)"), Report.Metrics.OverlapPercent), Report.Metrics.OverlapPercent > 50.f);
	TestTrue(TEXT("naming the islands that overlap"), !Report.Metrics.OverlappingIslands.IsEmpty());
	TestEqual(TEXT("a box's six faces are six islands"), Report.Metrics.Islands, 6);
	TestTrue(TEXT("its seams run along all twelve edges"), FMath::IsNearlyEqual(Report.Metrics.SeamLengthCm, 1200.f, 1.f));
	TestTrue(TEXT("and the layout PNG is drawn"), !Report.LayoutPath.IsEmpty() && IFileManager::Get().FileExists(*Report.LayoutPath));
	TestTrue(TEXT("the lightmap is checked too: ") + Report.Lightmap.Summary, !Report.Lightmap.Summary.IsEmpty());
	IFileManager::Get().Delete(*Report.LayoutPath, false, false, true);
	const FHyperAIModelUVCheckReport Missing = Check(Mesh->GetPathName(), 5);
	TestTrue(TEXT("a channel it does not have is missing: ") + Missing.Diagnostic, Missing.bOk && Missing.Verdict == TEXT("missing")
		&& Missing.Problems.Num() == 1 && Missing.Problems[0].StartsWith(TEXT("error missing_channel")));
	const FHyperAIModelUVCheckReport Engine = Check(TEXT("/Engine/BasicShapes/Cube"));
	TestTrue(TEXT("Engine content can be read: ") + Engine.Diagnostic, Engine.bOk && !Engine.Verdict.IsEmpty());
	IFileManager::Get().Delete(*Engine.LayoutPath, false, false, true);
	const FHyperAIModelUVCheckReport Nothing = Check(Folder / TEXT("SM_Nothing"));
	TestTrue(TEXT("a path with nothing there is refused: ") + Nothing.Diagnostic, !Nothing.bOk && Nothing.Diagnostic.Contains(TEXT("is not a static mesh")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingSurfaceAutoUVTest, "HyperAIStudio.NativeTools.Modeling.Surface.AutoUVFixesOverlapAndUndoes", HyperAIStudio::ModelingSurfaceTests::Flags)

bool FHyperAIStudioModelingSurfaceAutoUVTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingSurfaceTests;
	if (!GEditor || !GEditor->Trans)
	{
		AddInfo(TEXT("Skipped: no editor transaction buffer."));
		return true;
	}
	const FString Folder = TestFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };
	FString Error;
	UStaticMesh* Mesh = OverlappingBox(Folder / TEXT("SM_Crate"), Error);
	if (!TestNotNull(TEXT("a box with overlapping UVs is written: ") + Error, Mesh)) return false;
	const FString Path = Mesh->GetPathName();
	UMaterialInterface* const Material = Mesh->GetStaticMaterials()[0].MaterialInterface;
	const int32 Slots = Mesh->GetStaticMaterials().Num();
	const float OverlapBefore = Check(Path).Metrics.OverlapPercent;
	// A dry run builds the layout in memory and changes nothing.
	FHyperAIModelUVApplyRequest Request;
	Request.Asset = Path;
	Request.Method = TEXT("xatlas");
	Request.Resolution = 512;
	Request.bDryRun = true;
	const int32 UndoBefore = GEditor->Trans->GetQueueLength();
	const FHyperAIModelUVApplyReport Dry = WithoutApproval([&Request] { return C::ApplyUVs(Request); });
	TestTrue(TEXT("the dry run checks: ") + Dry.Diagnostic, Dry.bOk && Dry.Status == TEXT("checked") && Dry.LODs.Num() == 1);
	TestTrue(TEXT("and shows the overlap gone"), Dry.LODs.Num() == 1 && Dry.LODs[0].After.OverlapPercent < 0.5f && Dry.bImproved);
	TestEqual(TEXT("leaving the asset as it was"), Check(Path).Metrics.OverlapPercent, OverlapBefore);
	TestEqual(TEXT("and nothing in the undo history"), GEditor->Trans->GetQueueLength(), UndoBefore);
	IFileManager::Get().Delete(*Dry.LayoutPath, false, false, true);
	// Written.
	Request.bDryRun = false;
	const FHyperAIModelUVApplyReport Done = WithoutApproval([&Request] { return C::ApplyUVs(Request); });
	if (!TestTrue(TEXT("xatlas lays it out again: ") + Done.Diagnostic, Done.bOk && Done.Status == TEXT("completed") && Done.LODs.Num() == 1)) return false;
	TestTrue(FString::Printf(TEXT("the overlap count drops (%.1f%% -> %.2f%%)"), Done.LODs[0].Before.OverlapPercent, Done.LODs[0].After.OverlapPercent),
		Done.LODs[0].Before.OverlapPercent > 50.f && Done.LODs[0].After.OverlapPercent < 0.5f && Done.LODs[0].After.OverlappingIslands.IsEmpty());
	TestTrue(TEXT("the result says it improved, and read it back"), Done.bImproved && Done.LODs[0].bVerified);
	TestTrue(TEXT("and saved it"), Done.Saved.Contains(Path));
	const FHyperAIModelUVCheckReport After = Check(Path);
	TestTrue(FString::Printf(TEXT("the asset's own check agrees (%s, %.2f%% overlap)"), *After.Verdict, After.Metrics.OverlapPercent), After.Metrics.OverlapPercent < 0.5f && After.Verdict != TEXT("bad"));
	TestEqual(TEXT("its slots stay"), Mesh->GetStaticMaterials().Num(), Slots);
	TestTrue(TEXT("with their material"), Mesh->GetStaticMaterials()[0].MaterialInterface == Material);
	TestTrue(TEXT("the undo step is named"), GEditor->Trans->GetUndoContext().Title.ToString().Contains(TEXT("UV Layout")));
	// Repacked with padding: islands four texels apart at 512 are at least one apart at 128.
	{
		const FMeshDescription* Source = UV::SourceOf(*Mesh, 0);
		UV::FSoup Soup;
		if (TestTrue(TEXT("the source has channel 0"), Source && UV::SoupFromDescription(*Source, 0, FVector::OneVector, Soup)))
		{
			const UV::FMetrics Metrics = UV::Analyze(Soup);
			const UV::FLightmapMetrics Spaced = UV::AnalyzeLightmap(Soup, Metrics, 512 / 4, 0);
			TestEqual(FString::Printf(TEXT("Padding=4 at 512 leaves no texel shared at 128 (%.2f%%)"), Spaced.SharedPercent), Spaced.SharedPercent, 0.0);
		}
	}
	IFileManager::Get().Delete(*Done.LayoutPath, false, false, true);
	GEditor->UndoTransaction();
	TArray<UStaticMesh*> Compile = {Mesh};
	FStaticMeshCompilingManager::Get().FinishCompilation(Compile);
	TestTrue(FString::Printf(TEXT("undo brings back the original layout (%.1f%% overlap)"), Check(Path).Metrics.OverlapPercent),
		FMath::IsNearlyEqual(Check(Path).Metrics.OverlapPercent, OverlapBefore, 0.5f));
	// Lightmap UVs into channel 1, made when the mesh builds.
	FHyperAIModelUVApplyRequest Lightmap;
	Lightmap.Asset = Path;
	Lightmap.Method = TEXT("lightmap");
	const FHyperAIModelUVApplyReport Lit = WithoutApproval([&Lightmap] { return C::ApplyUVs(Lightmap); });
	TestTrue(TEXT("lightmap UVs are set up: ") + Lit.Diagnostic, Lit.bOk && Lit.Channel == 1);
	TestTrue(TEXT("the asset's lightmap channel is 1, built from channel 0"), Mesh->GetLightMapCoordinateIndex() == 1 && Mesh->GetSourceModel(0).BuildSettings.bGenerateLightmapUVs);
	TestTrue(TEXT("Unreal's packer gives a lightmap without shared texels: ") + Lit.Lightmap.Summary, Lit.Lightmap.bPresent && Lit.Lightmap.SharedTexelsPercent == 0.f);
	IFileManager::Get().Delete(*Lit.LayoutPath, false, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingSurfaceApprovalTest, "HyperAIStudio.NativeTools.Modeling.Surface.WaitsForApproval", HyperAIStudio::ModelingSurfaceTests::Flags)

bool FHyperAIStudioModelingSurfaceApprovalTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingSurfaceTests;
	const FString Folder = TestFolder();
	const FString OperationId = TEXT("uv-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower();
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bWasRequired = Settings->bRequireApprovalForAgentEdits;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bWasRequired;
		if (FHyperAIStudioApprovalGate::HasPending(OperationId)) FHyperAIStudioApprovalGate::Reject(OperationId, TEXT("Test finished."));
		DeleteFolder(Folder);
	};
	FString Error;
	UStaticMesh* Mesh = OverlappingBox(Folder / TEXT("SM_Crate"), Error);
	if (!TestNotNull(TEXT("a box with overlapping UVs is written: ") + Error, Mesh)) return false;
	Settings->bRequireApprovalForAgentEdits = true;
	FHyperAIModelUVApplyRequest Request;
	Request.Asset = Mesh->GetPathName();
	Request.OperationId = OperationId;
	const FHyperAIModelUVApplyReport Waiting = C::ApplyUVs(Request);
	if (!TestEqual(TEXT("with approval on, the layout waits: ") + Waiting.Diagnostic, Waiting.Status, FString(TEXT("awaiting_user_approval")))) return false;
	TestTrue(TEXT("naming the layout PNG it will draw"), Waiting.LayoutPath == UV::LayoutPathOf(Mesh->GetPathName(), 0, 0, false));
	TestTrue(TEXT("and changing nothing yet"), Check(Mesh->GetPathName()).Metrics.OverlapPercent > 50.f);
	if (!TestTrue(TEXT("approved, it runs: ") + Error, FHyperAIStudioApprovalGate::Approve(OperationId, Error))) return false;
	const TSharedRef<TOptional<FHyperAIStudioEditOutcome>> Outcome = MakeShared<TOptional<FHyperAIStudioEditOutcome>>();
	HyperAIStudio::OperationWait::WhenDone(OperationId, 1000, [Outcome](const FHyperAIStudioEditOutcome& Done) { *Outcome = Done; });
	if (!TestTrue(TEXT("the wait answers at once"), Outcome->IsSet())) return false;
	TestTrue(TEXT("as completed: ") + Outcome->GetValue().Diagnostic, Outcome->GetValue().bOk && Outcome->GetValue().Status == TEXT("completed"));
	TestTrue(TEXT("and the overlap is gone"), Check(Mesh->GetPathName()).Metrics.OverlapPercent < 0.5f);
	IFileManager::Get().Delete(*Waiting.LayoutPath, false, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingSurfaceRefusalsTest, "HyperAIStudio.NativeTools.Modeling.Surface.Refusals", HyperAIStudio::ModelingSurfaceTests::Flags)

bool FHyperAIStudioModelingSurfaceRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingSurfaceTests;
	const FString Folder = TestFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };
	auto Apply = [](const FString& Asset, const TCHAR* Method = TEXT("xatlas"))
	{
		FHyperAIModelUVApplyRequest Request;
		Request.Asset = Asset;
		Request.Method = Method;
		Request.bDryRun = true;
		return WithoutApproval([&Request] { return C::ApplyUVs(Request); });
	};
	auto Refused = [](const FHyperAIModelUVApplyReport& Report, const TCHAR* Words)
	{
		return !Report.bOk && Report.Status == TEXT("invalid_request") && Report.Diagnostic.Contains(Words);
	};
	TestTrue(TEXT("a mesh that is not in the project is refused"), Refused(Apply(Folder / TEXT("SM_Missing")), TEXT("is not a static mesh")));
	TestTrue(TEXT("Engine content is refused"), Refused(Apply(TEXT("/Engine/BasicShapes/Cube")), TEXT("is Engine content")));
	FString Error;
	UStaticMesh* Mesh = OverlappingBox(Folder / TEXT("SM_Crate"), Error);
	if (!TestNotNull(TEXT("a box is written: ") + Error, Mesh)) return false;
	TestTrue(TEXT("a method it does not know is refused"), Refused(Apply(Mesh->GetPathName(), TEXT("magic")), TEXT("is not one of xatlas")));
	{
		FHyperAIModelUVApplyRequest Far;
		Far.Asset = Mesh->GetPathName();
		Far.Channel = 4;
		Far.bDryRun = true;
		TestTrue(TEXT("a channel past the next one is refused"), Refused(WithoutApproval([&Far] { return C::ApplyUVs(Far); }), TEXT("is too far")));
		Far.Channel = 0;
		Far.LODs = {3};
		TestTrue(TEXT("a LOD it does not have is refused"), Refused(WithoutApproval([&Far] { return C::ApplyUVs(Far); }), TEXT("there is no LOD 3")));
	}
	// Cooked: no source to change.
	UPackage* Package = Mesh->GetPackage();
	Package->SetPackageFlags(PKG_Cooked);
	const FHyperAIModelUVApplyReport Cooked = Apply(Mesh->GetPathName());
	Package->ClearPackageFlags(PKG_Cooked);
	TestTrue(TEXT("a cooked asset is refused: ") + Cooked.Diagnostic, Refused(Cooked, TEXT("is cooked")));
	// Read-only on disk, with no source control to check it out.
	if (FHyperAIStudioSourceControl::DescribeBackend() == TEXT("none"))
	{
		FString File;
		FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), File, FPackageName::GetAssetPackageExtension());
		IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformFile();
		if (TestTrue(TEXT("the box was saved"), Disk.FileExists(*File)))
		{
			Disk.SetReadOnly(*File, true);
			const FHyperAIModelUVApplyReport ReadOnly = Apply(Mesh->GetPathName());
			Disk.SetReadOnly(*File, false);
			TestTrue(TEXT("a read-only asset is refused: ") + ReadOnly.Diagnostic, Refused(ReadOnly, TEXT("read-only on disk")));
		}
	}
	else
	{
		AddInfo(TEXT("Read-only refusal skipped: source control would check the file out."));
	}
	// Bakes refuse the same way, and refuse what they cannot make.
	auto BakeWith = [&Mesh, &Folder](TFunctionRef<void(FHyperAIModelBakeRequest&)> Change)
	{
		FHyperAIModelBakeRequest Request;
		Request.Target = Mesh->GetPathName();
		Request.Maps = {TEXT("ao")};
		Request.Resolution = 64;
		Request.Folder = Folder;
		Request.bDryRun = true;
		Change(Request);
		return WithoutApproval([&Request] { return C::BakeMaps(Request); });
	};
	auto BakeRefused = [](const FHyperAIModelBakeReport& Report, const TCHAR* Words) { return !Report.bOk && Report.Status == TEXT("invalid_request") && Report.Diagnostic.Contains(Words); };
	TestTrue(TEXT("a bake's dry run passes"), BakeWith([](FHyperAIModelBakeRequest&) {}).bOk);
	TestTrue(TEXT("an unknown map is refused"), BakeRefused(BakeWith([](FHyperAIModelBakeRequest& R) { R.Maps = {TEXT("glow")}; }), TEXT("is not one of normal")));
	TestTrue(TEXT("a resolution that is not a power of two is refused"), BakeRefused(BakeWith([](FHyperAIModelBakeRequest& R) { R.Resolution = 1000; }), TEXT("power of two")));
	TestTrue(TEXT("a channel the target lacks is refused"), BakeRefused(BakeWith([](FHyperAIModelBakeRequest& R) { R.TargetChannel = 6; }), TEXT("has no UV channel 6")));
	TestTrue(TEXT("textures into Engine content are refused"), BakeRefused(BakeWith([](FHyperAIModelBakeRequest& R) { R.Folder = TEXT("/Engine/Textures"); }), TEXT("not a content folder")));
	TestTrue(TEXT("putting a material on an Engine mesh is refused"), BakeRefused(BakeWith([](FHyperAIModelBakeRequest& R)
		{
			R.Target = TEXT("/Engine/BasicShapes/Cube");
			R.Material = TEXT("new");
			R.AssignSlots = TEXT("all");
		}), TEXT("is Engine content")));
	TestTrue(TEXT("carrying a texture the source has none of is refused"), BakeRefused(BakeWith([](FHyperAIModelBakeRequest& R) { R.Maps = {TEXT("texture")}; }), TEXT("no base colour texture")));
	FHyperAIModelPrepareRequest Prepare;
	Prepare.Asset = TEXT("/Engine/BasicShapes/Cube");
	Prepare.bDryRun = true;
	const FHyperAIModelPrepareReport Prepared = WithoutApproval([&Prepare] { return C::PrepareModel(Prepare); });
	TestTrue(TEXT("preparing Engine content is refused"), !Prepared.bOk && Prepared.Diagnostic.Contains(TEXT("is Engine content")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingSurfaceBakeTest, "HyperAIStudio.NativeTools.Modeling.Surface.BakeWritesTextures", HyperAIStudio::ModelingSurfaceTests::Flags)

bool FHyperAIStudioModelingSurfaceBakeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingSurfaceTests;
	if (!GEditor || !GEditor->Trans)
	{
		AddInfo(TEXT("Skipped: no editor transaction buffer."));
		return true;
	}
	const FString Folder = TestFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };
	// A cube in the corner of two walls on a floor, unwrapped: the corner and the cube's foot are occluded.
	FString Error;
	UStaticMesh* Mesh = Model(Folder / TEXT("SM_Corner"), {
		Op(TEXT("add.box"), TEXT("Corner"), TEXT(""), TEXT("300,300,10")),
		Op(TEXT("add.box"), TEXT("Corner"), TEXT(""), TEXT("10,300,200"), TEXT("-145,0,10")),
		Op(TEXT("add.box"), TEXT("Corner"), TEXT(""), TEXT("300,10,200"), TEXT("0,-145,10")),
		Op(TEXT("add.box"), TEXT("Corner"), TEXT(""), TEXT("60"), TEXT("-110,-110,10")),
		Op(TEXT("uv"), TEXT("Corner"), TEXT("Mode=auto"))}, Error);
	if (!TestNotNull(TEXT("the corner is written: ") + Error, Mesh)) return false;
	UMaterialInterface* const Original = Mesh->GetStaticMaterials()[0].MaterialInterface;
	FHyperAIModelBakeRequest Request;
	Request.Target = Mesh->GetPathName();
	Request.Maps = {TEXT("ao"), TEXT("normal"), TEXT("curvature")};
	Request.Resolution = 128;
	Request.OcclusionRays = 32;
	Request.Folder = Folder;
	Request.Material = TEXT("new");
	Request.AssignSlots = TEXT("all");
	Request.bDryRun = true;
	Request.bPreviewDryRun = true;
	const FHyperAIModelBakeReport Dry = WithoutApproval([&Request] { return C::BakeMaps(Request); });
	TestTrue(TEXT("a dry run names four textures (with the ORM pack): ") + Dry.Diagnostic, Dry.bOk && Dry.Status == TEXT("checked") && Dry.Textures.Num() == 4);
	TestTrue(TEXT("and previews them"), !Dry.PreviewPath.IsEmpty() && IFileManager::Get().FileExists(*Dry.PreviewPath));
	TestNull(TEXT("writing nothing"), LoadTexture(Folder / TEXT("T_Corner_AO")));
	IFileManager::Get().Delete(*Dry.PreviewPath, false, false, true);
	Request.bDryRun = false;
	Request.bPreviewDryRun = false;
	const FHyperAIModelBakeReport Done = WithoutApproval([&Request] { return C::BakeMaps(Request); });
	if (!TestTrue(TEXT("the bake runs: ") + Done.Diagnostic, Done.bOk && Done.Status == TEXT("completed"))) return false;
	TestTrue(TEXT("the maps PNG is drawn"), !Done.PreviewPath.IsEmpty() && IFileManager::Get().FileExists(*Done.PreviewPath));
	IFileManager::Get().Delete(*Done.PreviewPath, false, false, true);
	UTexture2D* Occlusion = LoadTexture(Folder / TEXT("T_Corner_AO"));
	UTexture2D* Normal = LoadTexture(Folder / TEXT("T_Corner_N"));
	UTexture2D* Curvature = LoadTexture(Folder / TEXT("T_Corner_CV"));
	UTexture2D* Packed = LoadTexture(Folder / TEXT("T_Corner_ORM"));
	if (!TestTrue(TEXT("every map is a texture asset"), Occlusion && Normal && Curvature && Packed)) return false;
	for (UTexture2D* Texture : {Occlusion, Normal, Curvature, Packed})
	{
		TestTrue(*FString::Printf(TEXT("%s is 128 px of BGRA8"), *Texture->GetName()), Texture->Source.GetSizeX() == 128 && Texture->Source.GetSizeY() == 128
			&& Texture->Source.GetFormat() == TSF_BGRA8);
		TestFalse(*FString::Printf(TEXT("%s is linear data, not sRGB"), *Texture->GetName()), Texture->SRGB);
	}
	TestTrue(TEXT("the normal map has normal-map compression"), Normal->CompressionSettings == TC_Normalmap && Normal->LODGroup == TEXTUREGROUP_WorldNormalMap);
	TestTrue(TEXT("occlusion and the ORM pack are masks"), Occlusion->CompressionSettings == TC_Masks && Packed->CompressionSettings == TC_Masks);
	float Min = 0.f, Max = 0.f;
	if (TestTrue(TEXT("the occlusion's pixels can be read"), Range(*Occlusion, Min, Max)))
	{
		TestTrue(FString::Printf(TEXT("the corner is dark and the open floor bright (%.2f to %.2f)"), Min, Max), Min < 0.7f && Max > 0.9f);
	}
	const FHyperAIModelBakedTexture* Reported = Done.Textures.FindByPredicate([](const FHyperAIModelBakedTexture& Texture) { return Texture.Map == TEXT("ao"); });
	TestTrue(TEXT("and the report says so"), Reported && Reported->Min < 0.7f && Reported->Compression == TEXT("TC_Masks") && !Reported->bSRGB);
	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Mesh->GetStaticMaterials()[0].MaterialInterface);
	if (TestNotNull(TEXT("the material instance is on the slot"), Instance))
	{
		UTexture* Value = nullptr;
		TestTrue(TEXT("using the normal map"), Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(TEXT("Normal"))), Value) && Value == Normal);
		TestTrue(TEXT("and the ORM pack"), Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(TEXT("ORM"))), Value) && Value == Packed);
		TestEqual(TEXT("named after the target"), Instance->GetName(), FString(TEXT("MI_Corner_Baked")));
	}
	// Undo puts the original material back on the slot; the new textures stay until deleted.
	GEditor->UndoTransaction();
	TestTrue(TEXT("undo restores the slot's material"), Mesh->GetStaticMaterials()[0].MaterialInterface == Original);
	// Baking again onto textures already there is refused unless bReplace.
	Request.Material.Reset();
	Request.AssignSlots.Reset();
	const FHyperAIModelBakeReport Again = WithoutApproval([&Request] { return C::BakeMaps(Request); });
	TestTrue(TEXT("baking onto existing textures is refused: ") + Again.Diagnostic, !Again.bOk && Again.Diagnostic.Contains(TEXT("bReplace")));
	Request.bReplace = true;
	Request.Maps = {TEXT("ao")};
	Request.OcclusionRays = 1;
	const FHyperAIModelBakeReport Replaced = WithoutApproval([&Request] { return C::BakeMaps(Request); });
	TestTrue(TEXT("and overwrites them with bReplace: ") + Replaced.Diagnostic, Replaced.bOk && Replaced.Textures.Num() == 2 && Replaced.Textures[0].bReplaced);
	IFileManager::Get().Delete(*Replaced.PreviewPath, false, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingSurfaceMaterialTest, "HyperAIStudio.NativeTools.Modeling.Surface.MaterialCapture", HyperAIStudio::ModelingSurfaceTests::Flags)

bool FHyperAIStudioModelingSurfaceMaterialTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingSurfaceTests;
	const FString Folder = TestFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };
	FString Error;
	UStaticMesh* Mesh = Model(Folder / TEXT("SM_Painted"), {
		Op(TEXT("add.box"), TEXT("Painted"), TEXT("Color=#d04020"), TEXT("100")),
		Op(TEXT("uv"), TEXT("Painted"), TEXT("Mode=auto"))}, Error);
	if (!TestNotNull(TEXT("a painted box is written: ") + Error, Mesh)) return false;
	FHyperAIModelBakeRequest Request;
	Request.Target = Mesh->GetPathName();
	Request.Maps = {TEXT("material")};
	Request.Resolution = 128;
	Request.Folder = Folder;
	TestTrue(TEXT("material with normal is refused"), [&Request]
	{
		FHyperAIModelBakeRequest Both = Request;
		Both.Maps.Add(TEXT("normal"));
		Both.bDryRun = true;
		const FHyperAIModelBakeReport Report = WithoutApproval([&Both] { return C::BakeMaps(Both); });
		return !Report.bOk && Report.Diagnostic.Contains(TEXT("leave texture, multi_texture and normal out"));
	}());
	const FHyperAIModelBakeReport Done = WithoutApproval([&Request] { return C::BakeMaps(Request); });
	if (!Bake::CanCapture())
	{
		TestTrue(TEXT("without rendering, material is refused, saying why: ") + Done.Diagnostic, !Done.bOk && Done.Diagnostic.Contains(TEXT("null RHI")));
		return true;
	}
	if (!TestTrue(TEXT("the material is drawn and baked: ") + Done.Diagnostic, Done.bOk)) return false;
	UTexture2D* Color = LoadTexture(Folder / TEXT("T_Painted_BC"));
	UTexture2D* Normal = LoadTexture(Folder / TEXT("T_Painted_N"));
	UTexture2D* Packed = LoadTexture(Folder / TEXT("T_Painted_ORM"));
	TestTrue(TEXT("into base colour, normal and ORM textures"), Color && Normal && Packed);
	float Min = 0.f, Max = 0.f;
	const bool bRead = Color && Range(*Color, Min, Max);
	TestTrue(FString::Printf(TEXT("the base colour holds the paint (%.2f to %.2f)"), Min, Max), bRead && Max > 0.1f && Color->SRGB);
	IFileManager::Get().Delete(*Done.PreviewPath, false, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingSurfacePrepareTest, "HyperAIStudio.NativeTools.Modeling.Surface.PrepareGeneratedModel", HyperAIStudio::ModelingSurfaceTests::Flags)

bool FHyperAIStudioModelingSurfacePrepareTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingSurfaceTests;
	const FString Folder = TestFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };
	// A "generated" model: a rounded rock with overlapping UVs and a base colour texture through a material instance.
	FString Error;
	UStaticMesh* Mesh = Model(Folder / TEXT("SM_Generated"), {
		Op(TEXT("add.sphere"), TEXT("Rock"), TEXT("Steps=24"), TEXT("120")),
		Op(TEXT("uv"), TEXT("Rock"), TEXT("Mode=planar; Scale=120"))}, Error);
	if (!TestNotNull(TEXT("the model is written: ") + Error, Mesh)) return false;
	Bake::FImage Checker;
	Checker.Map = Bake::EMap::Texture;
	Checker.Size = 64;
	for (int32 Y = 0; Y < 64; ++Y)
	{
		for (int32 X = 0; X < 64; ++X) Checker.Pixels.Add(((X / 8) + (Y / 8)) % 2 ? FColor(230, 60, 40) : FColor(40, 80, 220));
	}
	bool bCreated = false;
	UTexture2D* Color = Bake::WriteTexture(Folder / TEXT("T_Generated_Color"), Checker, false, bCreated, Error);
	TArray<FString> Notes;
	UMaterialInstanceConstant* Material = Color ? Bake::WriteMaterial(Folder / TEXT("MI_Generated"), Color, nullptr, nullptr, bCreated, Notes, Error) : nullptr;
	if (!TestNotNull(TEXT("its textured material is made: ") + Error, Material)) return false;
	{
		TArray<FStaticMaterial> Slots = Mesh->GetStaticMaterials();
		Slots[0].MaterialInterface = Material;
		Mesh->SetStaticMaterials(Slots);
		Mesh->PostEditChange();
	}
	TestTrue(TEXT("its base colour texture is found on the material"), Bake::BaseColorTexture(Material) == Color);
	FHyperAIModelPrepareRequest Request;
	Request.Asset = Mesh->GetPathName();
	Request.LowPoly = Folder / TEXT("SM_Generated_Low");
	Request.TargetTriangles = 200;
	Request.Resolution = 128;
	Request.Padding = 2;
	Request.bDryRun = true;
	const FHyperAIModelPrepareReport Dry = WithoutApproval([&Request] { return C::PrepareModel(Request); });
	TestTrue(TEXT("a dry run plans an unwrap and a low-poly bake: ") + Dry.Diagnostic, Dry.bOk && Dry.Status == TEXT("checked") && Dry.bUnwrapped && Dry.VerdictBefore == TEXT("bad")
		&& Dry.LowPolyTriangles > 0);
	TestNull(TEXT("and writes no low-poly"), LoadObject<UStaticMesh>(nullptr, *(Request.LowPoly + TEXT(".SM_Generated_Low")), nullptr, LOAD_NoWarn | LOAD_Quiet));
	IFileManager::Get().Delete(*Dry.LayoutPath, false, false, true);
	Request.bDryRun = false;
	const FHyperAIModelPrepareReport Done = WithoutApproval([&Request] { return C::PrepareModel(Request); });
	if (!TestTrue(TEXT("prepare runs: ") + Done.Diagnostic, Done.bOk && Done.Status == TEXT("completed"))) return false;
	TestTrue(FString::Printf(TEXT("its UVs go from bad to better (%s -> %s)"), *Done.VerdictBefore, *Done.VerdictAfter), Done.bUnwrapped && UV::VerdictRank(Done.VerdictAfter) < 2);
	TestTrue(TEXT("the asset's own check agrees"), Check(Mesh->GetPathName()).Metrics.OverlapPercent < 0.5f);
	UTexture2D* Carried = LoadTexture(Folder / TEXT("T_Generated_BC"));
	float Min = 0.f, Max = 0.f;
	TestTrue(TEXT("its base colour is carried to the new layout, checker and all"), Carried && Range(*Carried, Min, Max) && Max - Min > 0.2f && Carried->SRGB);
	TestTrue(TEXT("in a material instance on its slot"), Mesh->GetStaticMaterials()[0].MaterialInterface && Mesh->GetStaticMaterials()[0].MaterialInterface->GetName() == TEXT("MI_Generated_Baked"));
	UStaticMesh* Low = LoadObject<UStaticMesh>(nullptr, *(Request.LowPoly + TEXT(".SM_Generated_Low")), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (TestNotNull(TEXT("the low-poly is written"), Low))
	{
		const FMeshDescription* Source = Low->GetMeshDescription(0);
		TestTrue(FString::Printf(TEXT("with about the triangles asked for (%d)"), Source ? Source->Triangles().Num() : -1), Source && Source->Triangles().Num() <= 240);
		TestTrue(TEXT("its own UVs good enough to bake into"), Check(Low->GetPathName()).Metrics.OverlapPercent < 0.5f);
	}
	for (const TCHAR* Suffix : {TEXT("N"), TEXT("AO"), TEXT("BC"), TEXT("ORM")})
	{
		TestNotNull(*FString::Printf(TEXT("the low-poly's %s map is written"), Suffix), LoadTexture(Folder / FString::Printf(TEXT("T_Generated_Low_%s"), Suffix)));
	}
	for (const FString& Path : {Done.LayoutPath, Done.MapsPreviewPath, Done.PreviewPath}) IFileManager::Get().Delete(*Path, false, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
