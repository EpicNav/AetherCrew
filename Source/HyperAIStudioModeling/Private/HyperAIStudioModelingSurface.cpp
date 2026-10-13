// Games by Hyper 2026.

// UV checks, AutoUV and texture bakes for any static mesh in the project: hyper_model_uv_check, hyper_model_uv_apply,
// hyper_model_bake and hyper_model_prepare. Edits follow the model plan's path: checked, built in memory (the dry
// run's answer), approved, written in one undo step, read back, saved, drawn, and logged in Activity.

#include "HyperAIStudioModelingToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetUtils/CreateStaticMeshUtil.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioModelingBake.h"
#include "HyperAIStudioModelingGate.h"
#include "HyperAIStudioModelingUV.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "JsonObjectConverter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "ScopedTransaction.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioModeling"

namespace HyperAIStudio::ModelingSurface::Private
{
	using C = FHyperAIStudioModelingContracts;
	namespace UV = HyperAIStudio::Modeling::UV;
	namespace Bake = HyperAIStudio::Modeling::Bake;
	namespace Gate = HyperAIStudio::Modeling::Gate;
	using UE::Geometry::FDynamicMesh3;

	bool IsPlaying()
	{
		return GEditor && GEditor->PlayWorld != nullptr;
	}

	FString NewOperationId(const TCHAR* Prefix, const FString& Given)
	{
		return Given.IsEmpty() ? FString::Printf(TEXT("%s-%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Given;
	}

	/** The request as JSON, hashed: what the approval card shows, so the plan approved is the one that runs. */
	template <typename RequestType>
	FString HashOf(const RequestType& Request)
	{
		RequestType Copy = Request;
		Copy.OperationId.Reset();
		Copy.bDryRun = false;
		FString Json;
		FJsonObjectConverter::UStructToJsonObjectString(Copy, Json);
		return TEXT("surface-") + FMD5::HashAnsiString(*Json).Left(16);
	}

	/** The static mesh at Reference, or null with the reason. Nothing on disk is never loaded: a failed load leaves an empty package behind. */
	UStaticMesh* LoadMesh(const FString& Reference, FString& OutReason)
	{
		const FString Trimmed = Reference.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutReason = TEXT("Give the static mesh's path, like /Game/Models/SM_Rock.");
			return nullptr;
		}
		if (!Trimmed.StartsWith(TEXT("/")) || !FHyperAIStudioNames::IsObjectPath(Trimmed, TEXT("Asset"), &OutReason))
		{
			if (OutReason.IsEmpty()) OutReason = FString::Printf(TEXT("'%s' is not a content path, like /Game/Models/SM_Rock."), *Trimmed);
			return nullptr;
		}
		const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
		UStaticMesh* Mesh = FindObject<UStaticMesh>(nullptr, *ObjectPath);
		if (!Mesh && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ObjectPath)))
		{
			Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (!Mesh) OutReason = FString::Printf(TEXT("%s is not a static mesh in the project (hyper_model_inspect lists the meshes in a folder)."), *Trimmed);
		return Mesh;
	}

	FString FolderOf(const UObject& Asset)
	{
		return FPackageName::GetLongPackagePath(Asset.GetPackage()->GetName());
	}

	FString CleanFolder(const FString& Requested, const UObject& Beside)
	{
		FString Folder = Requested.TrimStartAndEnd();
		while (Folder.Len() > 1 && Folder.EndsWith(TEXT("/"))) Folder.LeftChopInline(1);
		return Folder.IsEmpty() ? FolderOf(Beside) : Folder;
	}

	void Record(const FString& OperationId, const TCHAR* ToolName, const bool bOk, const FString& Target, const FString& Diagnostic)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = ToolName;
		Entry.Target = Target.Left(200);
		Entry.OperationId = OperationId;
		Entry.StatusCode = bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	/** Holds Run for the person's approval; fills Status and Diagnostic for the reply. */
	bool Hold(const FString& OperationId, const TCHAR* ToolName, const TCHAR* Variant, const FString& PlanHash, const FString& Target, const TArray<FString>& Effects,
		const TArray<FString>& Touches, TFunction<void()> Run, FString& OutStatus, FString& OutDiagnostic)
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = C::PackId;
		Summary.ToolName = ToolName;
		Summary.VariantId = Variant;
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.PlanHash = PlanHash;
		Summary.EffectTarget = Target;
		Summary.Effects = Effects;
		Summary.Touches = Touches;
		FString Error;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, MoveTemp(Run), Error))
		{
			OutStatus = TEXT("approval_not_queued");
			OutDiagnostic = Error;
			return false;
		}
		OutStatus = TEXT("awaiting_user_approval");
		OutDiagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true; ")
			TEXT("its diagnostic gives the result, and the PNG paths named in this reply hold the pictures once it ran.");
		FHyperAIStudioApprovalGate::DescribeHeld(OperationId, OutStatus, OutDiagnostic);
		return true;
	}

	/** New assets a failed edit made, deleted again: an edit never leaves one half-written. */
	void Discard(TArray<UObject*>& Created)
	{
		Created.RemoveAll([](const UObject* Object) { return Object == nullptr; });
		if (!Created.IsEmpty()) ObjectTools::ForceDeleteObjects(Created, /*ShowConfirmation=*/false);
		Created.Reset();
	}

	bool SaveAll(const TArray<UObject*>& Assets, TArray<FString>& OutSaved, FString& OutError)
	{
		for (UObject* Asset : Assets)
		{
			if (!Asset || OutSaved.Contains(Asset->GetPathName())) continue;
			if (!FHyperAIStudioSourceControl::SaveAsset(*Asset, OutError))
			{
				OutError = FString::Printf(TEXT("%s was written but not saved: %s"), *Asset->GetPathName(), *OutError);
				return false;
			}
			OutSaved.Add(Asset->GetPathName());
		}
		return true;
	}

	void FinishCompiling(UStaticMesh& Mesh)
	{
		TArray<UStaticMesh*> Compile = {&Mesh};
		FStaticMeshCompilingManager::Get().FinishCompilation(Compile);
	}

	const FMeshBuildSettings* BuildSettingsOf(UStaticMesh& Mesh, const int32 Lod)
	{
		if (Lod < 0) return &Mesh.GetHiResSourceModel().BuildSettings;
		return Lod < Mesh.GetNumSourceModels() ? &Mesh.GetSourceModel(Lod).BuildSettings : nullptr;
	}

	/** Tangents follow channel 0: when the build does not make them again, the edited mesh's are made here. */
	void TangentsForChannelZero(UStaticMesh& Asset, const int32 Lod, UDynamicMesh& Mesh)
	{
		const FMeshBuildSettings* Settings = BuildSettingsOf(Asset, Lod);
		if (Settings && Settings->bRecomputeTangents) return;
		FGeometryScriptTangentsOptions Options;
		Options.UVLayer = 0;
		UGeometryScriptLibrary_MeshNormalsFunctions::ComputeTangents(&Mesh, Options, nullptr);
	}

	TStrongObjectPtr<UDynamicMesh> NewDynamic()
	{
		return TStrongObjectPtr<UDynamicMesh>(NewObject<UDynamicMesh>(GetTransientPackage()));
	}

	FString LodName(const int32 Lod)
	{
		return Lod < 0 ? FString(TEXT("hi-res source")) : FString::Printf(TEXT("LOD %d"), Lod);
	}

	/** One channel read and measured. */
	struct FMeasured
	{
		bool bPresent = false;
		UV::FSoup Soup;
		UV::FMetrics Metrics;
		FString Verdict = TEXT("missing");
		TArray<FString> Problems;
	};

	FMeasured Measure(const UV::FSoup* Soup, const int32 TextureSize)
	{
		FMeasured Out;
		if (!Soup) return Out;
		Out.bPresent = true;
		Out.Soup = *Soup;
		UV::FOptions Options;
		Options.TextureSize = TextureSize;
		Out.Metrics = UV::Analyze(Out.Soup, Options);
		UV::Judge(Out.Metrics, Out.Verdict, Out.Problems);
		return Out;
	}

	FMeasured MeasureDynamic(const FDynamicMesh3& Mesh, const int32 Channel, const int32 TextureSize)
	{
		UV::FSoup Soup;
		return UV::SoupFromDynamic(Mesh, Channel, Soup) ? Measure(&Soup, TextureSize) : FMeasured();
	}

	/** True when After is no worse than Before on any count the check makes. */
	bool NoWorse(const FMeasured& Before, const FMeasured& After)
	{
		if (!After.bPresent) return false;
		if (!Before.bPresent) return true;
		const UV::FMetrics& B = Before.Metrics;
		const UV::FMetrics& A = After.Metrics;
		return UV::VerdictRank(After.Verdict) <= UV::VerdictRank(Before.Verdict) && A.OverlapPercent <= B.OverlapPercent + 0.01 && A.Flipped <= B.Flipped
			&& A.Degenerate <= B.Degenerate && A.OutsidePercent <= B.OutsidePercent + 0.01;
	}

	/** The lightmap channel as the built mesh has it, or as the source does when the build does not make it. */
	void CheckLightmap(UStaticMesh& Mesh, const int32 Lod, const int32 Resolution, const int32 Padding, FHyperAIModelLightmapCheck& Out)
	{
		const int32 Channel = Mesh.GetLightMapCoordinateIndex();
		const int32 RenderLod = FMath::Max(0, Lod);
		const FMeshBuildSettings* Settings = BuildSettingsOf(Mesh, RenderLod);
		const bool bGenerated = Settings && Settings->bGenerateLightmapUVs;
		const int32 Texels = Resolution > 0 ? Resolution : FMath::Max(4, Mesh.GetLightMapResolution());
		UV::FSoup Soup;
		bool bPresent = false;
		FString Ignored;
		const FMeshDescription* Source = UV::SourceOf(Mesh, RenderLod);
		if (!bGenerated && Source) bPresent = UV::SoupFromDescription(*Source, Channel, UV::BuildScaleOf(Mesh, RenderLod), Soup);
		if (!bPresent) bPresent = UV::SoupFromRenderData(Mesh, RenderLod, Channel, Soup, Ignored);
		UV::FLightmapMetrics Metrics;
		if (bPresent)
		{
			const UV::FMetrics Layout = UV::Analyze(Soup);
			Metrics = UV::AnalyzeLightmap(Soup, Layout, Texels, Padding);
		}
		else
		{
			Metrics.Resolution = Texels;
		}
		UV::ToStruct(Metrics, Channel, bPresent, bGenerated, Out);
	}

	FString PreviewOf(UStaticMesh& Mesh, TArray<FString>& OutNotes)
	{
		FString Path;
		FString Reason;
		if (!Gate::RenderPreview(Mesh, Path, Reason))
		{
			OutNotes.Add(FString::Printf(TEXT("%s: no preview, %s."), *Mesh.GetPathName(), *Reason));
			return FString();
		}
		return Path;
	}

	FHyperAIModelBakedTexture Describe(const Bake::FImage& Image, const FString& Path, const bool bReplaced)
	{
		FHyperAIModelBakedTexture Out;
		const Bake::FTextureKind Kind = Bake::KindOf(Image.Map);
		Out.Map = Bake::MapName(Image.Map);
		Out.Path = Path;
		Out.Size = Image.Size;
		Out.Format = TEXT("BGRA8");
		Out.Compression = Kind.CompressionName;
		Out.bSRGB = Kind.bSRGB;
		Out.bReplaced = bReplaced;
		Bake::Stats(Image, Out.Min, Out.Mean, Out.Max);
		return Out;
	}

	/** A mesh's name without its SM_ prefix, for the names of what is made for it: SM_Rock gives T_Rock_N and MI_Rock_Baked. */
	FString Bare(const FString& Owner)
	{
		FString Name = Owner;
		Name.RemoveFromStart(TEXT("SM_"), ESearchCase::CaseSensitive);
		return Name;
	}

	FString TexturePathOf(const FString& Folder, const FString& Owner, const Bake::EMap Map)
	{
		return Folder / FString::Printf(TEXT("T_%s_%s"), *Bare(Owner), Bake::MapSuffix(Map));
	}

	// ----------------------------------------------------------------------------------------------- hyper_model_uv_apply

	struct FUVPlan
	{
		FString Asset;
		UV::EMethod Method = UV::EMethod::XAtlas;
		int32 Channel = 0;
		int32 SourceChannel = 0;
		int32 Resolution = 1024;
		int32 Padding = 4;
		bool bRepack = true;
		double TileSize = 100.0;
		TArray<int32> Lods;
		FString Hash;
	};

	struct FUVLodBuilt
	{
		int32 Lod = 0;
		int32 Triangles = 0;
		TStrongObjectPtr<UDynamicMesh> Mesh;
		FMeasured Before;
		FMeasured After;
	};

	struct FUVBuilt
	{
		TArray<FUVLodBuilt> Lods;
		UV::FLightmapMetrics Lightmap;
	};

	/** Most triangles one LOD may have for each method: XAtlas and PatchBuilder hold the editor longest. */
	int32 MaxTrianglesFor(const UV::EMethod Method)
	{
		return Method == UV::EMethod::XAtlas || Method == UV::EMethod::Patch ? C::MaxUnwrapTriangles : C::MaxLayoutTriangles;
	}

	bool CheckUVPlan(const FHyperAIModelUVApplyRequest& Request, FUVPlan& Plan, UStaticMesh*& OutMesh, FString& OutError)
	{
		OutMesh = LoadMesh(Request.Asset, OutError);
		if (!OutMesh) return false;
		const FString Refusal = UV::WhyNotWritable(*OutMesh);
		if (!Refusal.IsEmpty())
		{
			OutError = Refusal;
			return false;
		}
		Plan.Asset = OutMesh->GetPathName();
		if (!UV::ParseMethod(Request.Method, Plan.Method))
		{
			OutError = FString::Printf(TEXT("Method '%s' is not one of xatlas, patch, box, planar, cylinder, repack, lightmap."), *Request.Method);
			return false;
		}
		const bool bLightmap = Plan.Method == UV::EMethod::Lightmap;
		Plan.Channel = Request.Channel >= 0 ? Request.Channel : bLightmap ? 1 : 0;
		Plan.SourceChannel = Request.SourceChannel;
		Plan.Resolution = Request.Resolution > 0 ? Request.Resolution : bLightmap ? 64 : 1024;
		Plan.Padding = bLightmap ? 1 : Request.Padding >= 0 ? Request.Padding : 4;
		Plan.bRepack = Request.bRepack;
		Plan.TileSize = Request.TileSize;
		if (Plan.Channel > 7 || Plan.SourceChannel < 0 || Plan.SourceChannel > 7)
		{
			OutError = TEXT("UV channels are 0 to 7.");
			return false;
		}
		if (bLightmap ? (Plan.Resolution < 4 || Plan.Resolution > 4096) : (Plan.Resolution < 16 || Plan.Resolution > 8192))
		{
			OutError = bLightmap ? TEXT("Resolution is the lightmap's, 4 to 4096 texels.") : TEXT("Resolution is a texture size from 16 to 8192.");
			return false;
		}
		if (!bLightmap && (Plan.Padding < 1 || Plan.Padding > 64 || Plan.Padding * 16 > Plan.Resolution))
		{
			OutError = FString::Printf(TEXT("Padding is 1 to 64 texels, and at most a sixteenth of Resolution (%d)."), Plan.Resolution);
			return false;
		}
		if (!Plan.bRepack && !(Plan.TileSize > 0.0 && Plan.TileSize <= 100000.0))
		{
			OutError = TEXT("TileSize is cm per texture tile, above 0.");
			return false;
		}
		Plan.Lods = Request.LODs.IsEmpty() ? TArray<int32>({0}) : Request.LODs;
		TSet<int32> Seen;
		for (const int32 Lod : Plan.Lods)
		{
			if (Seen.Contains(Lod))
			{
				OutError = FString::Printf(TEXT("LODs names %d twice."), Lod);
				return false;
			}
			Seen.Add(Lod);
			if (bLightmap && Lod < 0)
			{
				OutError = TEXT("lightmap UVs are built for source LODs; the hi-res source has none.");
				return false;
			}
			if (!UV::SourceOf(*OutMesh, Lod))
			{
				OutError = Lod < 0 ? FString::Printf(TEXT("%s has no hi-res source."), *Plan.Asset)
					: Lod < OutMesh->GetNumSourceModels() ? FString::Printf(TEXT("LOD %d of %s is reduced from another LOD when it builds, so it follows that LOD's UVs; leave it out."), Lod, *Plan.Asset)
					: FString::Printf(TEXT("%s has %d source LOD(s); there is no LOD %d."), *Plan.Asset, OutMesh->GetNumSourceModels(), Lod);
				return false;
			}
			const int32 Channels = UV::ChannelsOf(*OutMesh, Lod);
			if (Plan.Channel > Channels || (Plan.Method == UV::EMethod::Repack && Plan.Channel >= Channels))
			{
				OutError = FString::Printf(TEXT("%s has %d UV channel(s): Channel %d %s."), *LodName(Lod), Channels, Plan.Channel,
					Plan.Method == UV::EMethod::Repack ? TEXT("is not there to repack") : *FString::Printf(TEXT("is too far; name one of them, or %d to add one"), Channels));
				return false;
			}
			if (bLightmap && Plan.SourceChannel >= Channels)
			{
				OutError = FString::Printf(TEXT("%s has no UV channel %d for the lightmap's charts."), *LodName(Lod), Plan.SourceChannel);
				return false;
			}
			if (bLightmap && Plan.Channel == Plan.SourceChannel)
			{
				OutError = TEXT("The lightmap channel must differ from SourceChannel, whose layout textures use.");
				return false;
			}
			const int32 Triangles = UV::SourceOf(*OutMesh, Lod)->Triangles().Num();
			if (Triangles > MaxTrianglesFor(Plan.Method))
			{
				OutError = FString::Printf(TEXT("%s has %d triangles, over the %d %s lays out here; simplify it first (hyper_model_prepare with LowPoly)."), *LodName(Lod), Triangles,
					MaxTrianglesFor(Plan.Method), UV::MethodName(Plan.Method));
				return false;
			}
		}
		if (bLightmap && !Plan.Lods.Contains(0))
		{
			OutError = TEXT("The lightmap channel is the asset's, and LOD 0 sets it; include LOD 0.");
			return false;
		}
		Plan.Hash = HashOf(Request);
		return true;
	}

	bool BuildUVPlan(const FUVPlan& Plan, UStaticMesh& Mesh, FUVBuilt& Built, FString& OutError)
	{
		const bool bLightmap = Plan.Method == UV::EMethod::Lightmap;
		for (const int32 Lod : Plan.Lods)
		{
			FUVLodBuilt& Out = Built.Lods.AddDefaulted_GetRef();
			Out.Lod = Lod;
			FDynamicMesh3 Read;
			if (!UV::Read(Mesh, Lod, Read, OutError)) return false;
			Out.Triangles = Read.TriangleCount();
			Out.Before = MeasureDynamic(Read, Plan.Channel, Plan.Resolution);
			if (bLightmap)
			{
				// What the build will make: Unreal's own packer, run on a copy now.
				UV::FSoup Soup;
				if (!UV::PreviewLightmap(Mesh, *UV::SourceOf(Mesh, Lod), Plan.SourceChannel, Plan.Channel, Plan.Resolution, Soup, OutError)) return false;
				Out.After = Measure(&Soup, Plan.Resolution);
				if (Lod == Plan.Lods[0]) Built.Lightmap = UV::AnalyzeLightmap(Out.After.Soup, Out.After.Metrics, Plan.Resolution, 0);
				continue;
			}
			Out.Mesh = NewDynamic();
			Out.Mesh->SetMesh(MoveTemp(Read));
			UV::FUnwrap Settings;
			Settings.Method = Plan.Method;
			Settings.Channel = Plan.Channel;
			Settings.Resolution = Plan.Resolution;
			Settings.Padding = Plan.Padding;
			Settings.bRepack = Plan.bRepack;
			Settings.TileSize = Plan.TileSize;
			if (!UV::Unwrap(*Out.Mesh, Settings, OutError))
			{
				OutError = FString::Printf(TEXT("%s: %s"), *LodName(Lod), *OutError);
				return false;
			}
			Out.Mesh->ProcessMesh([&Out, &Plan](const FDynamicMesh3& Edited) { Out.After = MeasureDynamic(Edited, Plan.Channel, Plan.Resolution); });
		}
		return true;
	}

	void DescribeLods(const FUVBuilt& Built, const int32 Channel, FHyperAIModelUVApplyReport& Report)
	{
		Report.LODs.Reset();
		Report.bImproved = !Built.Lods.IsEmpty();
		for (const FUVLodBuilt& Lod : Built.Lods)
		{
			FHyperAIModelUVLodResult& Out = Report.LODs.AddDefaulted_GetRef();
			Out.LOD = Lod.Lod;
			Out.Triangles = Lod.Triangles;
			Out.VerdictBefore = Lod.Before.Verdict;
			Out.VerdictAfter = Lod.After.Verdict;
			UV::ToStruct(Lod.Before.Metrics, Channel, Out.Before);
			UV::ToStruct(Lod.After.Metrics, Channel, Out.After);
			Report.bImproved &= NoWorse(Lod.Before, Lod.After);
		}
	}

	FHyperAIModelUVApplyReport ExecuteUV(const FHyperAIModelUVApplyRequest& Request, const FString& OperationId, const FUVPlan* Checked, FUVBuilt* Prebuilt)
	{
		FHyperAIModelUVApplyReport Report;
		Report.OperationId = OperationId;
		Report.Method = UV::MethodName(Checked ? Checked->Method : UV::EMethod::XAtlas);
		FString Error;
		if (IsPlaying())
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = TEXT("Play in Editor is running; stop it, then send the edit again.");
			return Report;
		}
		// After an approval the project may have moved on: checked and built again against it.
		FUVPlan Fresh;
		UStaticMesh* Mesh = nullptr;
		if (!CheckUVPlan(Request, Fresh, Mesh, Error))
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Error;
			return Report;
		}
		const FUVPlan& Plan = Checked ? *Checked : Fresh;
		Report.Asset = Plan.Asset;
		Report.Method = UV::MethodName(Plan.Method);
		Report.Channel = Plan.Channel;
		TUniquePtr<FUVBuilt> Own;
		FUVBuilt* Built = Prebuilt;
		if (!Built)
		{
			Own = MakeUnique<FUVBuilt>();
			Built = Own.Get();
			if (!BuildUVPlan(Plan, *Mesh, *Built, Error))
			{
				Report.Status = TEXT("operation_failed");
				Report.Diagnostic = Error;
				return Report;
			}
		}
		DescribeLods(*Built, Plan.Channel, Report);
		if (!FHyperAIStudioSourceControl::MakeEditable({Mesh->GetPackage()->GetName()}, Error))
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Error;
			return Report;
		}
		const bool bLightmap = Plan.Method == UV::EMethod::Lightmap;
		bool bWritten = true;
		{
			const FScopedTransaction Transaction(LOCTEXT("UVLayout", "HyperAI: UV Layout"));
			Mesh->Modify();
			for (FUVLodBuilt& Lod : Built->Lods)
			{
				if (bLightmap)
				{
					FMeshBuildSettings& Settings = Mesh->GetSourceModel(Lod.Lod).BuildSettings;
					Settings.bGenerateLightmapUVs = true;
					Settings.SrcLightmapIndex = Plan.SourceChannel;
					Settings.DstLightmapIndex = Plan.Channel;
					Settings.MinLightmapResolution = Plan.Resolution;
					continue;
				}
				if (Plan.Channel == 0) TangentsForChannelZero(*Mesh, Lod.Lod, *Lod.Mesh);
				Lod.Mesh->ProcessMesh([&](const FDynamicMesh3& Edited) { bWritten = bWritten && UV::Write(*Mesh, Lod.Lod, Edited, Error); });
				if (!bWritten) break;
			}
			if (bWritten && bLightmap)
			{
				Mesh->SetLightMapCoordinateIndex(Plan.Channel);
				Mesh->SetLightMapResolution(Plan.Resolution);
			}
			Mesh->PostEditChange();
			Mesh->MarkPackageDirty();
		}
		if (!bWritten)
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Error + TEXT(" Edit > Undo takes back what was written.");
			return Report;
		}
		// Read back: the asset now holds the layout built in memory.
		FinishCompiling(*Mesh);
		TArray<FString> Mismatch;
		UV::FSoup FirstSoup;
		UV::FMetrics FirstMetrics;
		for (int32 Index = 0; Index < Built->Lods.Num(); ++Index)
		{
			const FUVLodBuilt& Lod = Built->Lods[Index];
			UV::FSoup Soup;
			const bool bRead = bLightmap ? UV::SoupFromRenderData(*Mesh, Lod.Lod, Plan.Channel, Soup, Error)
				: UV::SoupFromDescription(*UV::SourceOf(*Mesh, Lod.Lod), Plan.Channel, UV::BuildScaleOf(*Mesh, Lod.Lod), Soup);
			const FMeasured Back = bRead ? Measure(&Soup, Plan.Resolution) : FMeasured();
			const UV::FMetrics& Want = Lod.After.Metrics;
			const bool bSame = Back.bPresent && FMath::Abs(Back.Metrics.Islands - Want.Islands) <= FMath::Max(1, Want.Islands / 50)
				&& FMath::Abs(Back.Metrics.OverlapPercent - Want.OverlapPercent) <= 0.5 && Back.Metrics.Flipped <= Want.Flipped + FMath::Max(1, Want.Triangles / 1000);
			Report.LODs[Index].bVerified = bSame;
			if (!bSame)
			{
				Mismatch.Add(FString::Printf(TEXT("%s reads back as %s, not %s"), *LodName(Lod.Lod), Back.bPresent ? *UV::Describe(Back.Verdict, Back.Metrics) : TEXT("missing"),
					*UV::Describe(Lod.After.Verdict, Want)));
			}
			if (Index == 0)
			{
				FirstSoup = Back.Soup;
				FirstMetrics = Back.Metrics;
			}
		}
		if (!Mismatch.IsEmpty())
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = FString::Join(Mismatch, TEXT("; ")) + TEXT(". Nothing was saved; Edit > Undo takes the change back.");
			return Report;
		}
		if (bLightmap) CheckLightmap(*Mesh, Plan.Lods[0], Plan.Resolution, 0, Report.Lightmap);
		if (!SaveAll({Mesh}, Report.Saved, Error))
		{
			Report.Status = TEXT("save_failed");
			Report.Diagnostic = Error;
			return Report;
		}
		// Drawn last: a saved write stands whether or not it can be drawn.
		Report.LayoutPath = UV::LayoutPathOf(Plan.Asset, Plan.Lods[0], Plan.Channel, false);
		FString DrawError;
		if (!UV::DrawLayout(FirstSoup, FirstMetrics, Report.LayoutPath, DrawError))
		{
			Report.Notes.Add(DrawError);
			Report.LayoutPath.Reset();
		}
		Report.PreviewPath = PreviewOf(*Mesh, Report.Notes);
		Report.bOk = true;
		Report.Status = TEXT("completed");
		const FHyperAIModelUVLodResult& First = Report.LODs[0];
		Report.Diagnostic = FString::Printf(TEXT("%s channel %d laid out with %s on %d LOD(s) and saved: %s -> %s (%d islands, %.1f%% overlap)%s. Layout PNG: %s. Undo takes it back."),
			*Plan.Asset, Plan.Channel, *Report.Method, Report.LODs.Num(), *First.VerdictBefore, *First.VerdictAfter, First.After.Islands, First.After.OverlapPercent,
			bLightmap ? *FString::Printf(TEXT("; %s"), *Report.Lightmap.Summary) : TEXT(""), Report.LayoutPath.IsEmpty() ? TEXT("none") : *Report.LayoutPath);
		return Report;
	}

	// --------------------------------------------------------------------------------------------------- hyper_model_bake

	/** The textures the maps write, in order: material writes base colour and normal; the ORM pack comes last. */
	TArray<Bake::EMap> OutputsOf(const TArray<Bake::EMap>& Maps, const bool bORM)
	{
		TArray<Bake::EMap> Out;
		for (const Bake::EMap Map : Maps)
		{
			if (Map == Bake::EMap::Material)
			{
				Out.Add(Bake::EMap::Texture);
				Out.Add(Bake::EMap::Normal);
			}
			else
			{
				Out.Add(Map);
			}
		}
		if (bORM) Out.Add(Bake::EMap::ORM);
		return Out;
	}

	struct FBakePlan
	{
		FString Target;
		FString Source;
		int32 SourceLod = 0;
		TArray<Bake::EMap> Maps;
		FString Folder;
		FString Owner;
		TMap<Bake::EMap, FString> Paths;
		TMap<Bake::EMap, bool> Exists;
		bool bORM = false;
		FString Material;
		bool bMaterialExists = false;
		TArray<int32> Slots;
		float ProjectionDistance = 3.f;
		TWeakObjectPtr<UTexture2D> SourceTexture;
		TArray<TWeakObjectPtr<UTexture2D>> SlotTextures;
		TWeakObjectPtr<UTexture2D> SourceNormalMap;
		FString TargetUVs;
		TArray<FString> Notes;
		FString Hash;
	};

	bool ParseSlots(const FString& Text, const int32 SlotCount, TArray<int32>& Out, FString& OutError)
	{
		Out.Reset();
		const FString Trimmed = Text.TrimStartAndEnd().ToLower();
		if (Trimmed.IsEmpty()) return true;
		if (Trimmed == TEXT("all"))
		{
			for (int32 Slot = 0; Slot < SlotCount; ++Slot) Out.Add(Slot);
			return true;
		}
		TArray<FString> Parts;
		Trimmed.ParseIntoArray(Parts, TEXT(","));
		for (const FString& Part : Parts)
		{
			const FString Number = Part.TrimStartAndEnd();
			if (Number.IsEmpty() || !Number.IsNumeric() || FCString::Atoi(*Number) < 0 || FCString::Atoi(*Number) >= SlotCount)
			{
				OutError = FString::Printf(TEXT("AssignSlots is all or slot numbers 0 to %d, like \"0,2\"; '%s' is not one."), SlotCount - 1, *Number);
				return false;
			}
			Out.AddUnique(FCString::Atoi(*Number));
		}
		return true;
	}

	bool CheckBakePlan(const FHyperAIModelBakeRequest& Request, FBakePlan& Plan, UStaticMesh*& OutTarget, UStaticMesh*& OutSource, FString& OutError)
	{
		OutTarget = LoadMesh(Request.Target, OutError);
		if (!OutTarget) return false;
		Plan.Target = OutTarget->GetPathName();
		Plan.Owner = OutTarget->GetName();
		OutSource = OutTarget;
		if (!Request.Source.TrimStartAndEnd().IsEmpty())
		{
			OutSource = LoadMesh(Request.Source, OutError);
			if (!OutSource) return false;
		}
		Plan.Source = OutSource->GetPathName();
		Plan.SourceLod = OutSource != OutTarget && OutSource->IsHiResMeshDescriptionValid() ? -1 : 0;
		if (Request.Maps.IsEmpty() || Request.Maps.Num() > 10)
		{
			OutError = FString::Printf(TEXT("Give 1 to 10 Maps: %s."), *Bake::MapNames());
			return false;
		}
		for (const FString& Name : Request.Maps)
		{
			Bake::EMap Map;
			if (!Bake::ParseMap(Name, Map))
			{
				OutError = FString::Printf(TEXT("Map '%s' is not one of %s."), *Name, *Bake::MapNames());
				return false;
			}
			if (Plan.Maps.Contains(Map))
			{
				OutError = FString::Printf(TEXT("Maps names %s twice."), *Name);
				return false;
			}
			Plan.Maps.Add(Map);
		}
		if (Plan.Maps.Contains(Bake::EMap::Texture) && Plan.Maps.Contains(Bake::EMap::MultiTexture))
		{
			OutError = TEXT("texture and multi_texture both make the base colour (BC); ask for one.");
			return false;
		}
		if (Plan.Maps.Contains(Bake::EMap::Material))
		{
			if (Plan.Maps.Contains(Bake::EMap::Texture) || Plan.Maps.Contains(Bake::EMap::MultiTexture) || Plan.Maps.Contains(Bake::EMap::Normal))
			{
				OutError = TEXT("material makes the base colour and the normal map itself; leave texture, multi_texture and normal out.");
				return false;
			}
			if (!Bake::CanCapture())
			{
				OutError = TEXT("material draws the source with render captures, and this editor runs without rendering (null RHI); bake texture (its base colour) instead.");
				return false;
			}
		}
		if (!Bake::IsResolution(Request.Resolution) || Request.Resolution > C::MaxBakeResolution)
		{
			OutError = FString::Printf(TEXT("Resolution is a power of two from 16 to %d."), C::MaxBakeResolution);
			return false;
		}
		if (!Bake::IsSampleCount(Request.Samples) || Request.Samples > 64)
		{
			OutError = TEXT("Samples is 1, 4, 16 or 64.");
			return false;
		}
		if (Request.Padding < 0 || Request.Padding > 64 || Request.OcclusionRays < 1 || Request.OcclusionRays > 256 || Request.OcclusionDistance < 0.f || Request.ProjectionDistance < 0.f)
		{
			OutError = TEXT("Padding is 0 to 64 pixels, OcclusionRays 1 to 256, OcclusionDistance and ProjectionDistance 0 or more cm.");
			return false;
		}
		if (!(Request.Roughness >= 0.f && Request.Roughness <= 1.f && Request.Metallic >= 0.f && Request.Metallic <= 1.f))
		{
			OutError = TEXT("Roughness and Metallic are 0 to 1.");
			return false;
		}
		if (Request.TargetLOD < 0 || Request.TargetLOD >= FMath::Max(OutTarget->GetNumSourceModels(), OutTarget->GetNumLODs()))
		{
			OutError = FString::Printf(TEXT("%s has no LOD %d."), *Plan.Target, Request.TargetLOD);
			return false;
		}
		const int32 TargetTriangles = OutTarget->GetNumTriangles(Request.TargetLOD);
		const FMeshDescription* TargetSource = UV::SourceOf(*OutTarget, Request.TargetLOD);
		if ((TargetSource ? TargetSource->Triangles().Num() : TargetTriangles) > C::MaxBakeTargetTriangles)
		{
			OutError = FString::Printf(TEXT("%s has over %d triangles to bake onto; bake onto a lighter copy (hyper_model_prepare with LowPoly)."), *Plan.Target, C::MaxBakeTargetTriangles);
			return false;
		}
		// The target's channel, measured: a bake wants islands that do not overlap.
		UV::FSoup Soup;
		FString Ignored;
		const bool bPresent = TargetSource ? UV::SoupFromDescription(*TargetSource, Request.TargetChannel, UV::BuildScaleOf(*OutTarget, Request.TargetLOD), Soup)
			: UV::SoupFromRenderData(*OutTarget, Request.TargetLOD, Request.TargetChannel, Soup, Ignored);
		if (!bPresent || Request.TargetChannel < 0)
		{
			OutError = FString::Printf(TEXT("%s has no UV channel %d to lay the maps out in; unwrap one first (hyper_model_uv_apply)."), *Plan.Target, Request.TargetChannel);
			return false;
		}
		const FMeasured Target = Measure(&Soup, Request.Resolution);
		Plan.TargetUVs = UV::Describe(Target.Verdict, Target.Metrics);
		if (Target.Metrics.OverlapPercent > 0.5)
		{
			Plan.Notes.Add(FString::Printf(TEXT("The target's channel %d overlaps (%.1f%%): overlapping islands get one bake between them; unwrap it first (hyper_model_uv_apply Method=xatlas) for a clean bake."),
				Request.TargetChannel, Target.Metrics.OverlapPercent));
		}
		if (Target.Metrics.OutsidePercent > 0.0)
		{
			Plan.Notes.Add(TEXT("The target's UVs reach outside 0-1: those parts wrap onto the same texels."));
		}
		const FBox Bounds = OutTarget->GetBoundingBox();
		Plan.ProjectionDistance = Request.ProjectionDistance > 0.f ? Request.ProjectionDistance : float(FMath::Max(0.5, Bounds.GetSize().GetMax() * 0.03));
		// Textures carried across: named, or found on the source's materials.
		const TArray<FStaticMaterial>& SourceSlots = OutSource->GetStaticMaterials();
		if (Plan.Maps.Contains(Bake::EMap::Texture))
		{
			UTexture2D* Texture = nullptr;
			if (!Request.SourceTexture.TrimStartAndEnd().IsEmpty())
			{
				const FString Path = Request.SourceTexture.TrimStartAndEnd();
				if (FHyperAIStudioNames::IsObjectPath(Path, TEXT("SourceTexture"), &OutError))
				{
					const FString ObjectPath = Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
					if (FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ObjectPath)) || FindObject<UTexture2D>(nullptr, *ObjectPath))
					{
						Texture = LoadObject<UTexture2D>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
					}
				}
				if (!Texture)
				{
					OutError = FString::Printf(TEXT("SourceTexture %s is not a 2D texture."), *Path);
					return false;
				}
			}
			else
			{
				Texture = SourceSlots.IsEmpty() ? nullptr : Bake::BaseColorTexture(SourceSlots[0].MaterialInterface);
				if (!Texture)
				{
					OutError = FString::Printf(TEXT("%s's slot 0 material samples no base colour texture to carry across; give SourceTexture."), *Plan.Source);
					return false;
				}
				Plan.Notes.Add(FString::Printf(TEXT("texture carries %s, the base colour of %s's slot 0 material."), *Texture->GetPathName(), *Plan.Source));
			}
			Plan.SourceTexture = Texture;
		}
		if (Plan.Maps.Contains(Bake::EMap::MultiTexture))
		{
			int32 Found = 0;
			for (const FStaticMaterial& Slot : SourceSlots)
			{
				UTexture2D* Texture = Bake::BaseColorTexture(Slot.MaterialInterface);
				Plan.SlotTextures.Add(Texture);
				Found += Texture ? 1 : 0;
			}
			if (Found == 0)
			{
				OutError = FString::Printf(TEXT("None of %s's material slots samples a base colour texture for multi_texture."), *Plan.Source);
				return false;
			}
		}
		if (OutSource != OutTarget && (Plan.Maps.Contains(Bake::EMap::Normal) || Plan.Maps.Contains(Bake::EMap::ObjectNormal)) && !SourceSlots.IsEmpty())
		{
			if (UTexture2D* Normal = Bake::NormalTexture(SourceSlots[0].MaterialInterface))
			{
				Plan.SourceNormalMap = Normal;
				Plan.Notes.Add(FString::Printf(TEXT("normal carries the source's own normal map %s, so its detail stays."), *Normal->GetPathName()));
			}
		}
		// Where the maps go.
		Plan.Folder = CleanFolder(Request.Folder, *OutTarget);
		FString Refusal = UV::WhyNotWritableFolder(Plan.Folder);
		if (!Refusal.IsEmpty())
		{
			OutError = Refusal;
			return false;
		}
		// A material capture's roughness and metal always go into the ORM pack: that is where they are kept.
		Plan.bORM = (Request.bPackORM && Plan.Maps.Contains(Bake::EMap::AO)) || Plan.Maps.Contains(Bake::EMap::Material);
		for (const Bake::EMap Map : OutputsOf(Plan.Maps, Plan.bORM))
		{
			const FString Path = TexturePathOf(Plan.Folder, Plan.Owner, Map);
			bool bExists = false;
			Refusal = Bake::WhyNotTexturePath(Path, Request.bReplace, bExists);
			if (!Refusal.IsEmpty())
			{
				OutError = Refusal;
				return false;
			}
			Plan.Paths.Add(Map, Path);
			Plan.Exists.Add(Map, bExists);
		}
		const FString Material = Request.Material.TrimStartAndEnd();
		if (!Material.IsEmpty())
		{
			Plan.Material = Material.ToLower() == TEXT("new") ? Plan.Folder / FString::Printf(TEXT("MI_%s_Baked"), *Bare(Plan.Owner))
				: Material;
			Refusal = Bake::WhyNotMaterialPath(Plan.Material, Plan.bMaterialExists);
			if (!Refusal.IsEmpty())
			{
				OutError = Refusal;
				return false;
			}
		}
		if (!ParseSlots(Request.AssignSlots, OutTarget->GetStaticMaterials().Num(), Plan.Slots, OutError)) return false;
		if (!Plan.Slots.IsEmpty())
		{
			if (Plan.Material.IsEmpty())
			{
				OutError = TEXT("AssignSlots puts the bake's material instance on the target: give Material too (new, or a material instance's path).");
				return false;
			}
			Refusal = UV::WhyNotWritable(*OutTarget);
			if (!Refusal.IsEmpty())
			{
				OutError = Refusal;
				return false;
			}
		}
		Plan.Hash = HashOf(Request);
		return true;
	}

	/** Reads both meshes and bakes. Resolution and Rays override the request's for a dry run's preview. */
	bool RunBake(const FHyperAIModelBakeRequest& Request, const FBakePlan& Plan, UStaticMesh& Target, UStaticMesh& Source, const int32 Resolution, const int32 Rays,
		TArray<Bake::FImage>& OutImages, int32& OutMs, FString& OutError)
	{
		const double Start = FPlatformTime::Seconds();
		const TStrongObjectPtr<UDynamicMesh> TargetMesh = NewDynamic();
		if (!Bake::ReadForBake(Target, Request.TargetLOD, *TargetMesh, OutError)) return false;
		TStrongObjectPtr<UDynamicMesh> SourceMesh;
		if (&Source != &Target)
		{
			SourceMesh = NewDynamic();
			if (!Bake::ReadForBake(Source, Plan.SourceLod, *SourceMesh, OutError)) return false;
		}
		Bake::FJob Job;
		Job.Target = TargetMesh.Get();
		Job.TargetChannel = Request.TargetChannel;
		Job.Source = SourceMesh ? SourceMesh.Get() : TargetMesh.Get();
		Job.Maps = Plan.Maps;
		Job.Resolution = Resolution;
		Job.Samples = Request.Samples;
		Job.Padding = Request.Padding;
		Job.Rays = Rays;
		Job.OcclusionDistance = Request.OcclusionDistance;
		Job.ProjectionDistance = Plan.ProjectionDistance;
		Job.SourceTexture = Plan.SourceTexture.Get();
		for (const TWeakObjectPtr<UTexture2D>& Texture : Plan.SlotTextures) Job.SlotTextures.Add(Texture.Get());
		Job.SourceChannel = Request.SourceChannel;
		Job.SourceNormalMap = Plan.SourceNormalMap.Get();
		Job.SourceNormalChannel = 0;
		Job.SourceAsset = &Source;
		if (!Bake::Run(Job, OutImages, OutError)) return false;
		if (Plan.bORM)
		{
			const Bake::FImage* Occlusion = OutImages.FindByPredicate([](const Bake::FImage& Image) { return Image.Map == Bake::EMap::AO; });
			const Bake::FImage* Captured = OutImages.FindByPredicate([](const Bake::FImage& Image) { return Image.Map == Bake::EMap::MaterialMRS; });
			Bake::FImage Packed = Bake::PackORM(Occlusion, Captured, Resolution, Request.Roughness, Request.Metallic);
			OutImages.Add(MoveTemp(Packed));
		}
		OutMs = int32((FPlatformTime::Seconds() - Start) * 1000.0);
		return true;
	}

	/** Writes the images, the material instance and the slots; inside the caller's transaction. */
	bool WriteBake(const FBakePlan& Plan, UStaticMesh& Target, const TArray<Bake::FImage>& Images, const bool bReplace, FHyperAIModelBakeReport& Report, TArray<UObject*>& OutTouched,
		TArray<UObject*>& OutCreated, FString& OutError)
	{
		TMap<Bake::EMap, UTexture2D*> Written;
		for (const Bake::FImage& Image : Images)
		{
			const FString* Path = Plan.Paths.Find(Image.Map);
			if (!Path) continue;
			bool bCreated = false;
			UTexture2D* Texture = Bake::WriteTexture(*Path, Image, bReplace, bCreated, OutError);
			if (!Texture) return false;
			(bCreated ? OutCreated : OutTouched).Add(Texture);
			Written.Add(Image.Map, Texture);
			Report.Textures.Add(Describe(Image, Texture->GetPathName(), !bCreated));
		}
		if (Plan.Material.IsEmpty()) return true;
		UTexture2D* const* Color = Written.Find(Bake::EMap::Texture);
		if (!Color) Color = Written.Find(Bake::EMap::MultiTexture);
		if (!Color) Color = Written.Find(Bake::EMap::VertexColor);
		UTexture2D* const* Normal = Written.Find(Bake::EMap::Normal);
		UTexture2D* const* Packed = Written.Find(Bake::EMap::ORM);
		bool bCreated = false;
		UMaterialInstanceConstant* Instance = Bake::WriteMaterial(Plan.Material, Color ? *Color : nullptr, Normal ? *Normal : nullptr, Packed ? *Packed : nullptr, bCreated, Report.Notes,
			OutError);
		if (!Instance) return false;
		(bCreated ? OutCreated : OutTouched).Add(Instance);
		Report.MaterialInstance = Instance->GetPathName();
		if (!Plan.Slots.IsEmpty())
		{
			if (!FHyperAIStudioSourceControl::MakeEditable({Target.GetPackage()->GetName()}, OutError)) return false;
			Target.Modify();
			TArray<FStaticMaterial> Slots = Target.GetStaticMaterials();
			for (const int32 Slot : Plan.Slots)
			{
				if (Slots.IsValidIndex(Slot)) Slots[Slot].MaterialInterface = Instance;
			}
			Target.SetStaticMaterials(Slots);
			Target.PostEditChange();
			Target.MarkPackageDirty();
			Report.AssignedSlots = Plan.Slots;
			OutTouched.Add(&Target);
		}
		return true;
	}

	FHyperAIModelBakeReport ExecuteBake(const FHyperAIModelBakeRequest& Request, const FString& OperationId)
	{
		FHyperAIModelBakeReport Report;
		Report.OperationId = OperationId;
		FString Error;
		if (IsPlaying())
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = TEXT("Play in Editor is running; stop it, then send the bake again.");
			return Report;
		}
		FBakePlan Plan;
		UStaticMesh* Target = nullptr;
		UStaticMesh* Source = nullptr;
		if (!CheckBakePlan(Request, Plan, Target, Source, Error))
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.Target = Plan.Target;
		Report.Source = FString::Printf(TEXT("%s (%s)"), *Plan.Source, *LodName(Plan.SourceLod));
		Report.TargetUVs = Plan.TargetUVs;
		Report.Notes = Plan.Notes;
		TArray<Bake::FImage> Images;
		if (!RunBake(Request, Plan, *Target, *Source, Request.Resolution, Request.OcclusionRays, Images, Report.BakeMs, Error))
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Error + TEXT(" Nothing was written.");
			return Report;
		}
		TArray<UObject*> Touched;
		TArray<UObject*> Created;
		bool bWritten = false;
		{
			const FScopedTransaction Transaction(LOCTEXT("BakeMaps", "HyperAI: Bake Maps"));
			bWritten = WriteBake(Plan, *Target, Images, Request.bReplace, Report, Touched, Created, Error);
		}
		if (!bWritten)
		{
			Discard(Created);
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Error + TEXT(" New textures were deleted; Edit > Undo takes back any overwritten.");
			Report.Textures.Reset();
			return Report;
		}
		TArray<UObject*> All = Created;
		All.Append(Touched);
		if (!SaveAll(All, Report.Saved, Error))
		{
			Report.Status = TEXT("save_failed");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.PreviewPath = Bake::SheetPathOf(Plan.Target, false);
		FString DrawError;
		if (!Bake::DrawSheet(Images, Report.PreviewPath, DrawError))
		{
			Report.Notes.Add(DrawError);
			Report.PreviewPath.Reset();
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		TArray<FString> Lines;
		for (const FHyperAIModelBakedTexture& Texture : Report.Textures) Lines.Add(FString::Printf(TEXT("%s %s (%.2f-%.2f)"), *Texture.Map, *Texture.Path, Texture.Min, Texture.Max));
		Report.Diagnostic = FString::Printf(TEXT("%d map(s) baked at %d px in %d ms and saved. Maps PNG: %s. %s%s. Undo takes back slots and overwritten textures; new ones stay until deleted."),
			Images.Num(), Request.Resolution, Report.BakeMs, Report.PreviewPath.IsEmpty() ? TEXT("none") : *Report.PreviewPath, *FString::Join(Lines, TEXT("; ")),
			Report.MaterialInstance.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; material %s%s"), *Report.MaterialInstance,
				Report.AssignedSlots.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" on %d slot(s)"), Report.AssignedSlots.Num())));
		return Report;
	}

	// ------------------------------------------------------------------------------------------------ hyper_model_prepare

	struct FPreparePlan
	{
		FString Asset;
		FString Owner;
		UV::EMethod Method = UV::EMethod::XAtlas;
		FString Unwrap = TEXT("auto");
		bool bUnwrap = false;
		FString LowPoly;
		int32 TargetTriangles = 0;
		TArray<Bake::EMap> Maps;
		FString Folder;
		FString Hash;
		/** What the original's materials sample: carried across an unwrap, and the bake's source textures. */
		TArray<TWeakObjectPtr<UTexture2D>> SlotColors;
		TWeakObjectPtr<UTexture2D> Normal;
		int32 Triangles = 0;
		FMeasured Before;
		TArray<FString> Notes;
	};

	FString PrepareTexturePath(const FString& Folder, const FString& Owner, const Bake::EMap Map)
	{
		return TexturePathOf(Folder, Owner, Map);
	}

	FString PrepareMaterialPath(const FString& Folder, const FString& Owner)
	{
		return Folder / FString::Printf(TEXT("MI_%s_Baked"), *Bare(Owner));
	}

	bool CheckPreparePlan(const FHyperAIModelPrepareRequest& Request, FPreparePlan& Plan, UStaticMesh*& OutMesh, FString& OutError)
	{
		OutMesh = LoadMesh(Request.Asset, OutError);
		if (!OutMesh) return false;
		FString Refusal = UV::WhyNotWritable(*OutMesh);
		if (!Refusal.IsEmpty())
		{
			OutError = Refusal;
			return false;
		}
		Plan.Asset = OutMesh->GetPathName();
		Plan.Owner = OutMesh->GetName();
		Plan.Unwrap = Request.UnwrapUVs.TrimStartAndEnd().ToLower();
		if (Plan.Unwrap.IsEmpty()) Plan.Unwrap = TEXT("auto");
		if (Plan.Unwrap != TEXT("auto") && Plan.Unwrap != TEXT("always") && Plan.Unwrap != TEXT("never"))
		{
			OutError = TEXT("UnwrapUVs is auto, always or never.");
			return false;
		}
		if (!UV::ParseMethod(Request.UVMethod, Plan.Method) || (Plan.Method != UV::EMethod::XAtlas && Plan.Method != UV::EMethod::Patch))
		{
			OutError = TEXT("UVMethod is xatlas or patch.");
			return false;
		}
		if (!Bake::IsResolution(Request.Resolution) || Request.Resolution > C::MaxBakeResolution || !Bake::IsSampleCount(Request.Samples) || Request.Samples > 64
			|| Request.Padding < 1 || Request.Padding > 64 || Request.Padding * 16 > Request.Resolution)
		{
			OutError = FString::Printf(TEXT("Resolution is a power of two from 16 to %d, Samples 1, 4, 16 or 64, Padding 1 to 64 and at most a sixteenth of Resolution."),
				C::MaxBakeResolution);
			return false;
		}
		const FMeshDescription* Source = UV::SourceOf(*OutMesh, 0);
		Plan.Triangles = Source ? Source->Triangles().Num() : 0;
		if (Plan.Triangles > C::MaxUnwrapTriangles)
		{
			OutError = FString::Printf(TEXT("%s has %d triangles, over the %d prepare unwraps and decimates here; reduce it in its import settings first."), *Plan.Asset, Plan.Triangles,
				C::MaxUnwrapTriangles);
			return false;
		}
		Plan.Folder = CleanFolder(Request.Folder, *OutMesh);
		Refusal = UV::WhyNotWritableFolder(Plan.Folder);
		if (!Refusal.IsEmpty())
		{
			OutError = Refusal;
			return false;
		}
		// The textures its materials sample, by slot.
		for (const FStaticMaterial& Slot : OutMesh->GetStaticMaterials()) Plan.SlotColors.Add(Bake::BaseColorTexture(Slot.MaterialInterface));
		if (!OutMesh->GetStaticMaterials().IsEmpty()) Plan.Normal = Bake::NormalTexture(OutMesh->GetStaticMaterials()[0].MaterialInterface);
		const bool bHasColor = Plan.SlotColors.ContainsByPredicate([](const TWeakObjectPtr<UTexture2D>& Texture) { return Texture.IsValid(); });
		// The UV check decides an automatic unwrap.
		UV::FSoup Soup;
		Plan.Before = Source && UV::SoupFromDescription(*Source, 0, UV::BuildScaleOf(*OutMesh, 0), Soup) ? Measure(&Soup, Request.Resolution) : FMeasured();
		Plan.bUnwrap = Plan.Unwrap == TEXT("always") || (Plan.Unwrap == TEXT("auto") && UV::VerdictRank(Plan.Before.Verdict) >= 2);
		if (Plan.bUnwrap && OutMesh->GetNumSourceModels() > 1)
		{
			Plan.Notes.Add(FString::Printf(TEXT("Only LOD 0 is unwrapped; %s's other %d LOD(s) keep their UVs, so the carried textures fit LOD 0 (reduce the others from it to match)."),
				*Plan.Asset, OutMesh->GetNumSourceModels() - 1));
		}
		Plan.LowPoly = Request.LowPoly.TrimStartAndEnd();
		if (!Plan.LowPoly.IsEmpty())
		{
			FString Reason;
			if (!Plan.LowPoly.StartsWith(TEXT("/")) || !FHyperAIStudioNames::IsObjectPath(Plan.LowPoly, TEXT("LowPoly"), &Reason))
			{
				OutError = Reason.IsEmpty() ? FString::Printf(TEXT("LowPoly '%s' is not a content path, like /Game/Models/SM_Rock_Low."), *Plan.LowPoly) : Reason;
				return false;
			}
			if (Plan.LowPoly.Contains(TEXT("."))) Plan.LowPoly.LeftInline(Plan.LowPoly.Find(TEXT(".")));
			Refusal = UV::WhyNotWritableFolder(FPackageName::GetLongPackagePath(Plan.LowPoly));
			if (!Refusal.IsEmpty())
			{
				OutError = Refusal;
				return false;
			}
			if (FPackageName::DoesPackageExist(Plan.LowPoly) || FindObject<UObject>(nullptr, *(Plan.LowPoly + TEXT(".") + FPackageName::GetShortName(Plan.LowPoly))))
			{
				OutError = FString::Printf(TEXT("%s already exists; pick a new LowPoly path."), *Plan.LowPoly);
				return false;
			}
			Plan.TargetTriangles = Request.TargetTriangles > 0 ? Request.TargetTriangles : FMath::Max(500, Plan.Triangles / 10);
			if (Plan.TargetTriangles < 50 || Plan.TargetTriangles >= Plan.Triangles)
			{
				OutError = FString::Printf(TEXT("TargetTriangles is 50 or more and under the original's %d."), Plan.Triangles);
				return false;
			}
			if (Request.Maps.IsEmpty())
			{
				Plan.Maps = {Bake::EMap::Normal, Bake::EMap::AO};
				if (bHasColor) Plan.Maps.Add(Bake::EMap::Texture);
			}
			for (const FString& Name : Request.Maps)
			{
				Bake::EMap Map;
				if (!Bake::ParseMap(Name, Map) || Map == Bake::EMap::MultiTexture || Map == Bake::EMap::Material)
				{
					OutError = FString::Printf(TEXT("Map '%s' is not one of normal, object_normal, bent_normal, ao, curvature, position, height, vertex_color, texture (bake material with hyper_model_bake)."), *Name);
					return false;
				}
				if (Map == Bake::EMap::Texture && !bHasColor)
				{
					OutError = FString::Printf(TEXT("texture carries the original's base colour, and %s's materials sample none."), *Plan.Asset);
					return false;
				}
				Plan.Maps.AddUnique(Map);
			}
		}
		else if (!Request.Maps.IsEmpty())
		{
			OutError = TEXT("Maps are baked onto LowPoly; give LowPoly too, or bake with hyper_model_bake.");
			return false;
		}
		// Every texture it would write: none may be there already.
		TArray<TPair<FString, Bake::EMap>> Textures;
		if (Plan.bUnwrap && bHasColor) Textures.Add({Plan.Owner, Bake::EMap::Texture});
		if (Plan.bUnwrap && Plan.Normal.IsValid()) Textures.Add({Plan.Owner, Bake::EMap::Normal});
		const FString LowName = Plan.LowPoly.IsEmpty() ? FString() : FPackageName::GetShortName(Plan.LowPoly);
		for (const Bake::EMap Map : Plan.Maps) Textures.Add({LowName, Map});
		if (Plan.Maps.Contains(Bake::EMap::AO)) Textures.Add({LowName, Bake::EMap::ORM});
		for (const TPair<FString, Bake::EMap>& Texture : Textures)
		{
			bool bExists = false;
			Refusal = Bake::WhyNotTexturePath(PrepareTexturePath(Plan.Folder, Texture.Key, Texture.Value), /*bReplace=*/false, bExists);
			if (!Refusal.IsEmpty())
			{
				OutError = Refusal;
				return false;
			}
		}
		for (const FString& Owner : {Plan.bUnwrap && (bHasColor || Plan.Normal.IsValid()) ? Plan.Owner : FString(), LowName})
		{
			if (Owner.IsEmpty()) continue;
			bool bExists = false;
			Refusal = Bake::WhyNotMaterialPath(PrepareMaterialPath(Plan.Folder, Owner), bExists);
			if (Refusal.IsEmpty() && bExists) Refusal = FString::Printf(TEXT("%s already exists; pick another Folder."), *PrepareMaterialPath(Plan.Folder, Owner));
			if (!Refusal.IsEmpty())
			{
				OutError = Refusal;
				return false;
			}
		}
		Plan.Hash = HashOf(Request);
		return true;
	}

	/** LOD 0 unwrapped, and the decimated copy unwrapped, in memory. */
	struct FPrepareBuilt
	{
		TStrongObjectPtr<UDynamicMesh> Unwrapped;
		FMeasured After;
		TStrongObjectPtr<UDynamicMesh> Low;
		FMeasured LowUVs;
	};

	bool Decimate(UDynamicMesh& Mesh, const int32 Triangles, const FHyperAIModelPrepareRequest& Request, FMeasured& OutUVs, FString& OutError)
	{
		FGeometryScriptSimplifyMeshOptions Options;
		UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(&Mesh, Triangles, Options, nullptr);
		// A decimated mesh's old seams no longer fit it: unwrapped again for its own bake.
		UV::FUnwrap Settings;
		Settings.Method = UV::EMethod::XAtlas;
		Settings.Channel = 0;
		Settings.Resolution = Request.Resolution;
		Settings.Padding = Request.Padding;
		if (!UV::Unwrap(Mesh, Settings, OutError)) return false;
		Mesh.ProcessMesh([&OutUVs, &Request](const FDynamicMesh3& Read) { OutUVs = MeasureDynamic(Read, 0, Request.Resolution); });
		FGeometryScriptTangentsOptions Tangents;
		Tangents.UVLayer = 0;
		UGeometryScriptLibrary_MeshNormalsFunctions::ComputeTangents(&Mesh, Tangents, nullptr);
		return true;
	}

	bool BuildPrepare(const FPreparePlan& Plan, const FHyperAIModelPrepareRequest& Request, UStaticMesh& Mesh, FPrepareBuilt& Built, FString& OutError)
	{
		if (Plan.bUnwrap)
		{
			FDynamicMesh3 Read;
			if (!UV::Read(Mesh, 0, Read, OutError)) return false;
			Built.Unwrapped = NewDynamic();
			Built.Unwrapped->SetMesh(MoveTemp(Read));
			UV::FUnwrap Settings;
			Settings.Method = Plan.Method;
			Settings.Channel = 0;
			Settings.Resolution = Request.Resolution;
			Settings.Padding = Request.Padding;
			if (!UV::Unwrap(*Built.Unwrapped, Settings, OutError)) return false;
			Built.Unwrapped->ProcessMesh([&Built, &Request](const FDynamicMesh3& Edited) { Built.After = MeasureDynamic(Edited, 0, Request.Resolution); });
		}
		if (!Plan.LowPoly.IsEmpty())
		{
			// In memory the copy starts from the unwrapped LOD 0 (a dry run) or the asset as read for a bake.
			Built.Low = NewDynamic();
			if (!Bake::ReadForBake(Mesh, 0, *Built.Low, OutError)) return false;
			if (!Decimate(*Built.Low, Plan.TargetTriangles, Request, Built.LowUVs, OutError)) return false;
		}
		return true;
	}

	/** The decimated copy as a new static mesh, its slots all the bake's material instance, the original's simple collision. */
	UStaticMesh* WriteLow(const FPreparePlan& Plan, UStaticMesh& Original, UDynamicMesh& Low, UMaterialInterface* Material, const bool bLightmap, FString& OutError)
	{
		FDynamicMesh3 Copy;
		Low.ProcessMesh([&Copy](const FDynamicMesh3& Read) { Copy = Read; });
		const int32 Slots = FMath::Max(1, Original.GetStaticMaterials().Num());
		TArray<UMaterialInterface*> Materials;
		for (int32 Slot = 0; Slot < Slots; ++Slot) Materials.Add(Material ? Material : Original.GetStaticMaterials().IsValidIndex(Slot) ? Original.GetStaticMaterials()[Slot].MaterialInterface.Get() : nullptr);
		UE::AssetUtils::FStaticMeshAssetOptions Options;
		Options.NewAssetPath = Plan.LowPoly;
		Options.NumSourceModels = 1;
		Options.NumMaterialSlots = Slots;
		Options.AssetMaterials = Materials;
		Options.bEnableRecomputeNormals = false;
		Options.bEnableRecomputeTangents = true;
		Options.bGenerateNaniteEnabledMesh = false;
		Options.bGenerateLightmapUVs = bLightmap;
		Options.bCreatePhysicsBody = true;
		const UBodySetup* Body = Original.GetBodySetup();
		Options.CollisionType = Body ? Body->CollisionTraceFlag.GetValue() : ECollisionTraceFlag::CTF_UseDefault;
		Options.SourceMeshes.DynamicMeshes.Add(&Copy);
		Options.bDeferPostEditChange = true;
		UE::AssetUtils::FStaticMeshResults Results;
		if (UE::AssetUtils::CreateStaticMeshAsset(Options, Results) != UE::AssetUtils::ECreateStaticMeshResult::Ok || !Results.StaticMesh)
		{
			OutError = FString::Printf(TEXT("%s could not be made."), *Plan.LowPoly);
			return nullptr;
		}
		UStaticMesh* Asset = Results.StaticMesh;
		{
			// A new asset takes its settings outside the undo step, as Epic's asset creation does.
			TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
			if (Body && Asset->GetBodySetup())
			{
				Asset->GetBodySetup()->AggGeom = Body->AggGeom;
				Asset->GetBodySetup()->InvalidatePhysicsData();
				Asset->GetBodySetup()->CreatePhysicsMeshes();
			}
			if (bLightmap)
			{
				Asset->SetLightMapCoordinateIndex(1);
				Asset->GetSourceModel(0).BuildSettings.DstLightmapIndex = 1;
			}
			Asset->PostEditChange();
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
		Asset->SetFlags(RF_Transactional);
		return Asset;
	}

	FHyperAIModelPrepareReport ExecutePrepare(const FHyperAIModelPrepareRequest& Request, const FString& OperationId)
	{
		FHyperAIModelPrepareReport Report;
		Report.OperationId = OperationId;
		FString Error;
		auto Fail = [&Report](const FString& Status, const FString& Why)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Why;
			return Report;
		};
		if (IsPlaying()) return Fail(TEXT("operation_failed"), TEXT("Play in Editor is running; stop it, then send it again."));
		FPreparePlan Plan;
		UStaticMesh* Mesh = nullptr;
		if (!CheckPreparePlan(Request, Plan, Mesh, Error)) return Fail(TEXT("operation_failed"), Error);
		Report.Asset = Plan.Asset;
		Report.Notes = Plan.Notes;
		Report.VerdictBefore = Plan.Before.Verdict;
		Report.VerdictAfter = Plan.Before.Verdict;
		Report.Steps.Add(FString::Printf(TEXT("uv_check: channel 0 is %s"), *UV::Describe(Plan.Before.Verdict, Plan.Before.Metrics)));
		const bool bHasColor = Plan.SlotColors.ContainsByPredicate([](const TWeakObjectPtr<UTexture2D>& Texture) { return Texture.IsValid(); });
		if (!Plan.bUnwrap && Plan.LowPoly.IsEmpty() && !Request.bLightmapUVs)
		{
			Report.bOk = true;
			Report.Status = TEXT("completed");
			Report.Steps.Add(TEXT("nothing to do: the UVs are not bad, and no LowPoly or lightmap UVs were asked for"));
			Report.Diagnostic = FString::Printf(TEXT("%s needs nothing: channel 0 is %s. Nothing was changed."), *Plan.Asset, *Plan.Before.Verdict);
			return Report;
		}
		if (!FHyperAIStudioSourceControl::MakeEditable({Mesh->GetPackage()->GetName()}, Error)) return Fail(TEXT("operation_failed"), Error);
		// The original as the renderer sees it, read before anything changes: the source of every carry and bake.
		const TStrongObjectPtr<UDynamicMesh> Original = NewDynamic();
		if (!Bake::ReadForBake(*Mesh, 0, *Original, Error)) return Fail(TEXT("operation_failed"), Error);
		FPrepareBuilt Built;
		Built.Unwrapped = NewDynamic();
		TArray<UObject*> Created;
		TArray<UObject*> Touched;
		TArray<Bake::FImage> Sheet;
		bool bOk = true;
		{
			const FScopedTransaction Transaction(LOCTEXT("PrepareModel", "HyperAI: Prepare Model"));
			// Each step leaves its reason in Error when it fails; the first failure stops the rest.
			auto Step = [&bOk](const bool bDone) { bOk = bOk && bDone; return bOk; };
			if (Plan.bUnwrap)
			{
				FDynamicMesh3 Read;
				Step(UV::Read(*Mesh, 0, Read, Error));
				if (bOk)
				{
					Built.Unwrapped->SetMesh(MoveTemp(Read));
					UV::FUnwrap Settings;
					Settings.Method = Plan.Method;
					Settings.Resolution = Request.Resolution;
					Settings.Padding = Request.Padding;
					Step(UV::Unwrap(*Built.Unwrapped, Settings, Error));
				}
				if (bOk)
				{
					Built.Unwrapped->ProcessMesh([&Built, &Request](const FDynamicMesh3& Edited) { Built.After = MeasureDynamic(Edited, 0, Request.Resolution); });
					TangentsForChannelZero(*Mesh, 0, *Built.Unwrapped);
					Mesh->Modify();
					Built.Unwrapped->ProcessMesh([&](const FDynamicMesh3& Edited) { Step(UV::Write(*Mesh, 0, Edited, Error)); });
					if (bOk)
					{
						Mesh->PostEditChange();
						Mesh->MarkPackageDirty();
						FinishCompiling(*Mesh);
						Touched.Add(Mesh);
						Report.bUnwrapped = true;
						Report.VerdictAfter = Built.After.Verdict;
						Report.Steps.Add(FString::Printf(TEXT("unwrap: %s into channel 0, now %s"), UV::MethodName(Plan.Method), *UV::Describe(Built.After.Verdict, Built.After.Metrics)));
					}
				}
				// The old layout's textures, carried onto the new one: the same surface, so its colours and normal detail stay.
				if (bOk && (bHasColor || Plan.Normal.IsValid()))
				{
					const TStrongObjectPtr<UDynamicMesh> Target = NewDynamic();
					Step(Bake::ReadForBake(*Mesh, 0, *Target, Error));
					Bake::FJob Job;
					Job.Target = Target.Get();
					Job.Source = Original.Get();
					Job.Resolution = Request.Resolution;
					Job.Samples = Request.Samples;
					Job.Padding = Request.Padding;
					Job.ProjectionDistance = float(FMath::Max(0.1, Mesh->GetBoundingBox().GetSize().GetMax() * 0.005));
					if (Plan.SlotColors.Num() > 1)
					{
						Job.Maps.Add(Bake::EMap::MultiTexture);
						for (const TWeakObjectPtr<UTexture2D>& Texture : Plan.SlotColors) Job.SlotTextures.Add(Texture.Get());
					}
					else if (bHasColor)
					{
						Job.Maps.Add(Bake::EMap::Texture);
						Job.SourceTexture = Plan.SlotColors[0].Get();
					}
					if (Plan.Normal.IsValid())
					{
						Job.Maps.Add(Bake::EMap::Normal);
						Job.SourceNormalMap = Plan.Normal.Get();
					}
					TArray<Bake::FImage> Images;
					if (bOk) Step(Bake::Run(Job, Images, Error));
					UTexture2D* Color = nullptr;
					UTexture2D* Normal = nullptr;
					for (const Bake::FImage& Image : Images)
					{
						if (!bOk) break;
						bool bCreated = false;
						const Bake::EMap Name = Image.Map == Bake::EMap::MultiTexture ? Bake::EMap::Texture : Image.Map;
						UTexture2D* Texture = Bake::WriteTexture(PrepareTexturePath(Plan.Folder, Plan.Owner, Name), Image, false, bCreated, Error);
						if (!Step(Texture != nullptr)) break;
						Created.Add(Texture);
						(Image.Map == Bake::EMap::Normal ? Normal : Color) = Texture;
						Report.Textures.Add(Describe(Image, Texture->GetPathName(), false));
						Sheet.Add(Image);
					}
					if (bOk)
					{
						bool bCreated = false;
						UMaterialInstanceConstant* Instance = Bake::WriteMaterial(PrepareMaterialPath(Plan.Folder, Plan.Owner), Color, Normal, nullptr, bCreated, Report.Notes, Error);
						if (Step(Instance != nullptr))
						{
							Created.Add(Instance);
							Report.MaterialInstance = Instance->GetPathName();
							TArray<FStaticMaterial> Slots = Mesh->GetStaticMaterials();
							for (FStaticMaterial& Slot : Slots) Slot.MaterialInterface = Instance;
							Mesh->SetStaticMaterials(Slots);
							Mesh->PostEditChange();
							Report.Steps.Add(FString::Printf(TEXT("carry: the old layout's %s baked onto the new one, in %s on every slot"),
								Normal && Color ? TEXT("base colour and normal map") : Normal ? TEXT("normal map") : TEXT("base colour"), *Instance->GetPathName()));
						}
					}
				}
			}
			if (bOk && !Plan.LowPoly.IsEmpty())
			{
				// From the asset as it now is: its new layout and carried textures, or the original's.
				const TStrongObjectPtr<UDynamicMesh> High = NewDynamic();
				Step(Bake::ReadForBake(*Mesh, 0, *High, Error));
				Built.Low = NewDynamic();
				if (bOk) Step(Bake::ReadForBake(*Mesh, 0, *Built.Low, Error));
				if (bOk) Step(Decimate(*Built.Low, Plan.TargetTriangles, Request, Built.LowUVs, Error));
				const FString LowName = FPackageName::GetShortName(Plan.LowPoly);
				TArray<Bake::FImage> Images;
				if (bOk)
				{
					Bake::FJob Job;
					Job.Target = Built.Low.Get();
					Job.Source = High.Get();
					Job.Maps = Plan.Maps;
					Job.Resolution = Request.Resolution;
					Job.Samples = Request.Samples;
					Job.Padding = Request.Padding;
					Job.ProjectionDistance = float(FMath::Max(0.5, Mesh->GetBoundingBox().GetSize().GetMax() * 0.03));
					const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
					Job.SourceTexture = Slots.IsEmpty() ? nullptr : Bake::BaseColorTexture(Slots[0].MaterialInterface);
					Job.SourceNormalMap = Slots.IsEmpty() ? nullptr : Bake::NormalTexture(Slots[0].MaterialInterface);
					Step(Bake::Run(Job, Images, Error));
					const Bake::FImage* Occlusion = Images.FindByPredicate([](const Bake::FImage& Image) { return Image.Map == Bake::EMap::AO; });
					if (Occlusion)
					{
						Bake::FImage Packed = Bake::PackORM(Occlusion, nullptr, Occlusion->Size, 0.8f, 0.f);
						Images.Add(MoveTemp(Packed));
					}
				}
				TMap<Bake::EMap, UTexture2D*> Written;
				for (const Bake::FImage& Image : Images)
				{
					if (!bOk) break;
					bool bCreated = false;
					UTexture2D* Texture = Bake::WriteTexture(PrepareTexturePath(Plan.Folder, LowName, Image.Map), Image, false, bCreated, Error);
					if (!Step(Texture != nullptr)) break;
					Created.Add(Texture);
					Written.Add(Image.Map, Texture);
					Report.Textures.Add(Describe(Image, Texture->GetPathName(), false));
					Sheet.Add(Image);
				}
				UMaterialInstanceConstant* Instance = nullptr;
				if (bOk)
				{
					bool bCreated = false;
					UTexture2D* const* Color = Written.Find(Bake::EMap::Texture);
					UTexture2D* const* Normal = Written.Find(Bake::EMap::Normal);
					UTexture2D* const* Packed = Written.Find(Bake::EMap::ORM);
					Instance = Bake::WriteMaterial(PrepareMaterialPath(Plan.Folder, LowName), Color ? *Color : nullptr, Normal ? *Normal : nullptr, Packed ? *Packed : nullptr, bCreated,
						Report.Notes, Error);
					if (Step(Instance != nullptr)) Created.Add(Instance);
				}
				if (bOk)
				{
					UStaticMesh* LowAsset = WriteLow(Plan, *Mesh, *Built.Low, Instance, Request.bLightmapUVs, Error);
					if (Step(LowAsset != nullptr))
					{
						Created.Add(LowAsset);
						Report.LowPoly = LowAsset->GetPathName();
						Report.LowPolyTriangles = Built.Low->GetTriangleCount();
						TArray<FString> Names;
						for (const Bake::EMap Map : Plan.Maps) Names.Add(Bake::MapName(Map));
						Report.Steps.Add(FString::Printf(TEXT("low_poly: %s, %d triangles (from %d), unwrapped (%s), baked %s from the original, in %s"), *Report.LowPoly, Report.LowPolyTriangles,
							Plan.Triangles, *Built.LowUVs.Verdict, *FString::Join(Names, TEXT(", ")), *Instance->GetPathName()));
					}
				}
			}
			if (bOk && Request.bLightmapUVs)
			{
				Mesh->Modify();
				FMeshBuildSettings& Settings = Mesh->GetSourceModel(0).BuildSettings;
				Settings.bGenerateLightmapUVs = true;
				Settings.SrcLightmapIndex = 0;
				Settings.DstLightmapIndex = 1;
				Mesh->SetLightMapCoordinateIndex(1);
				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();
				Touched.AddUnique(Mesh);
				Report.Steps.Add(TEXT("lightmap: channel 1 is made from channel 0 when the mesh builds"));
			}
		}
		if (!bOk)
		{
			Discard(Created);
			return Fail(TEXT("operation_failed"), Error + TEXT(" New assets were deleted; Edit > Undo takes back the change to the asset."));
		}
		TArray<UObject*> All = Created;
		All.Append(Touched);
		if (!SaveAll(All, Report.Saved, Error)) return Fail(TEXT("save_failed"), Error);
		// Pictures last: what was saved stands whether or not it can be drawn.
		FinishCompiling(*Mesh);
		if (const FMeshDescription* Source = UV::SourceOf(*Mesh, 0))
		{
			UV::FSoup Soup;
			if (UV::SoupFromDescription(*Source, 0, UV::BuildScaleOf(*Mesh, 0), Soup))
			{
				const FMeasured Now = Measure(&Soup, Request.Resolution);
				Report.LayoutPath = UV::LayoutPathOf(Plan.Asset, 0, 0, false);
				if (!UV::DrawLayout(Now.Soup, Now.Metrics, Report.LayoutPath, Error)) Report.LayoutPath.Reset();
			}
		}
		if (!Sheet.IsEmpty())
		{
			Report.MapsPreviewPath = Bake::SheetPathOf(Report.LowPoly.IsEmpty() ? Plan.Asset : Report.LowPoly, false);
			if (!Bake::DrawSheet(Sheet, Report.MapsPreviewPath, Error)) Report.MapsPreviewPath.Reset();
		}
		UStaticMesh* Shown = Report.LowPoly.IsEmpty() ? Mesh : FindObject<UStaticMesh>(nullptr, *Report.LowPoly);
		if (Shown) Report.PreviewPath = PreviewOf(*Shown, Report.Notes);
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%s prepared and saved: UVs %s -> %s%s%s. Steps: %s. Undo takes back the asset's change; new assets stay until deleted."), *Plan.Asset,
			*Report.VerdictBefore, *Report.VerdictAfter, Report.LowPoly.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; LowPoly %s (%d triangles)"), *Report.LowPoly, Report.LowPolyTriangles),
			Report.MaterialInstance.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; material %s"), *Report.MaterialInstance), *FString::Join(Report.Steps, TEXT("; ")));
		return Report;
	}
}

FHyperAIModelUVCheckReport FHyperAIStudioModelingContracts::CheckUVs(const FHyperAIModelUVCheckRequest& Request)
{
	using namespace HyperAIStudio::ModelingSurface::Private;
	FHyperAIModelUVCheckReport Report;
	Report.LOD = Request.LOD;
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	FString Error;
	UStaticMesh* Mesh = LoadMesh(Request.Asset, Error);
	if (!Mesh)
	{
		Report.Diagnostic = Error;
		return Report;
	}
	Report.Asset = Mesh->GetPathName();
	if (Request.TextureSize < 16 || Request.TextureSize > 8192 || Request.Channel < 0 || Request.Channel > 7 || Request.LOD < -1 || Request.LightmapPadding < 0
		|| Request.LightmapPadding > 8 || (Request.LightmapResolution != 0 && (Request.LightmapResolution < 4 || Request.LightmapResolution > 4096)))
	{
		Report.Diagnostic = TEXT("TextureSize is 16 to 8192, Channel 0 to 7, LOD -1 (hi-res) or more, LightmapResolution 0 (the asset's) or 4 to 4096, LightmapPadding 0 to 8.");
		return Report;
	}
	const FMeshDescription* Source = UV::SourceOf(*Mesh, Request.LOD);
	const bool bBuilt = !Source && Request.LOD >= 0 && Request.LOD < Mesh->GetNumLODs();
	if (!Source && !bBuilt)
	{
		Report.Diagnostic = Request.LOD < 0 ? FString::Printf(TEXT("%s has no hi-res source."), *Report.Asset)
			: FString::Printf(TEXT("%s has %d LOD(s); there is no LOD %d."), *Report.Asset, Mesh->GetNumLODs(), Request.LOD);
		return Report;
	}
	const int32 Triangles = Source ? Source->Triangles().Num() : Mesh->GetNumTriangles(Request.LOD);
	if (Triangles > C::MaxCheckTriangles)
	{
		Report.Diagnostic = FString::Printf(TEXT("%s has %d triangles in %s, over the %d the check reads at once."), *Report.Asset, Triangles, *LodName(Request.LOD), C::MaxCheckTriangles);
		return Report;
	}
	UV::FSoup Soup;
	bool bPresent = false;
	if (Source)
	{
		FStaticMeshConstAttributes Attributes(*Source);
		Report.Channels = Attributes.GetVertexInstanceUVs().IsValid() ? Attributes.GetVertexInstanceUVs().GetNumChannels() : 0;
		bPresent = UV::SoupFromDescription(*Source, Request.Channel, UV::BuildScaleOf(*Mesh, Request.LOD), Soup);
	}
	else
	{
		const FStaticMeshRenderData* Render = Mesh->GetRenderData();
		Report.Channels = Render && Render->LODResources.IsValidIndex(Request.LOD) ? int32(Render->LODResources[Request.LOD].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()) : 0;
		bPresent = UV::SoupFromRenderData(*Mesh, Request.LOD, Request.Channel, Soup, Error);
		if (!bPresent && !Error.IsEmpty())
		{
			Report.Diagnostic = FString::Printf(TEXT("%s: %s."), *Report.Asset, *Error);
			return Report;
		}
	}
	CheckLightmap(*Mesh, Request.LOD, Request.LightmapResolution, Request.LightmapPadding, Report.Lightmap);
	Report.bOk = true;
	if (!bPresent)
	{
		Report.Verdict = TEXT("missing");
		Report.Metrics.Channel = Request.Channel;
		Report.Problems.Add(FString::Printf(TEXT("error missing_channel: %s has %d UV channel(s), so there is no channel %d (hyper_model_uv_apply adds one)."), *LodName(Request.LOD),
			Report.Channels, Request.Channel));
		Report.Diagnostic = FString::Printf(TEXT("%s %s: missing, it has %d UV channel(s). %s"), *Report.Asset, *LodName(Request.LOD), Report.Channels, *Report.Lightmap.Summary);
		return Report;
	}
	FMeasured Measured = Measure(&Soup, Request.TextureSize);
	Report.Verdict = Measured.Verdict;
	Report.Problems = Measured.Problems;
	UV::ToStruct(Measured.Metrics, Request.Channel, Report.Metrics);
	if (Request.bLayoutImage)
	{
		Report.LayoutPath = UV::LayoutPathOf(Report.Asset, Request.LOD, Request.Channel, false);
		FString DrawError;
		if (!UV::DrawLayout(Measured.Soup, Measured.Metrics, Report.LayoutPath, DrawError)) Report.LayoutPath.Reset();
	}
	Report.Diagnostic = FString::Printf(TEXT("%s %s channel %d is %s (%.0f texels/m median at %d px%s). %s Layout PNG: %s."), *Report.Asset, *LodName(Request.LOD), Request.Channel,
		*UV::Describe(Measured.Verdict, Measured.Metrics), Measured.Metrics.DensityMedian, Request.TextureSize, bBuilt ? TEXT(", read from the built mesh") : TEXT(""),
		*Report.Lightmap.Summary, Report.LayoutPath.IsEmpty() ? TEXT("none") : *Report.LayoutPath);
	return Report;
}

FHyperAIModelUVApplyReport FHyperAIStudioModelingContracts::ApplyUVs(const FHyperAIModelUVApplyRequest& Request)
{
	using namespace HyperAIStudio::ModelingSurface::Private;
	FHyperAIModelUVApplyReport Report;
	Report.OperationId = NewOperationId(TEXT("uv"), Request.OperationId);
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (IsPlaying())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Play in Editor is running; stop it, then send the edit again.");
		return Report;
	}
	FString Error;
	FUVPlan Plan;
	UStaticMesh* Mesh = nullptr;
	if (!CheckUVPlan(Request, Plan, Mesh, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	Report.Asset = Plan.Asset;
	Report.Method = UV::MethodName(Plan.Method);
	Report.Channel = Plan.Channel;
	TUniquePtr<FUVBuilt> Built = MakeUnique<FUVBuilt>();
	if (!BuildUVPlan(Plan, *Mesh, *Built, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error + TEXT(" Nothing was changed.");
		return Report;
	}
	DescribeLods(*Built, Plan.Channel, Report);
	const FUVLodBuilt& First = Built->Lods[0];
	if (Plan.Method == UV::EMethod::Lightmap) UV::ToStruct(Built->Lightmap, Plan.Channel, true, true, Report.Lightmap);
	// A layout that comes out worse than it went in is never written.
	for (const FUVLodBuilt& Lod : Built->Lods)
	{
		if (Lod.Before.bPresent && UV::VerdictRank(Lod.After.Verdict) > UV::VerdictRank(Lod.Before.Verdict))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = FString::Printf(TEXT("%s with %s would make %s's channel %d worse (%s, was %s); nothing was changed. Try xatlas, or a dry run of another method."), *Plan.Asset,
				UV::MethodName(Plan.Method), *LodName(Lod.Lod), Plan.Channel, *UV::Describe(Lod.After.Verdict, Lod.After.Metrics), *Lod.Before.Verdict);
			return Report;
		}
	}
	if (Request.bDryRun)
	{
		Report.LayoutPath = UV::LayoutPathOf(Plan.Asset, First.Lod, Plan.Channel, true);
		if (!UV::DrawLayout(First.After.Soup, First.After.Metrics, Report.LayoutPath, Error))
		{
			Report.Notes.Add(Error);
			Report.LayoutPath.Reset();
		}
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%s channel %d with %s: %s -> %s on %s%s. Layout PNG: %s. Nothing was changed."), *Plan.Asset, Plan.Channel, UV::MethodName(Plan.Method),
			*UV::Describe(First.Before.Verdict, First.Before.Metrics), *UV::Describe(First.After.Verdict, First.After.Metrics), *LodName(First.Lod),
			Built->Lods.Num() > 1 ? *FString::Printf(TEXT(" and %d more LOD(s)"), Built->Lods.Num() - 1) : TEXT(""), Report.LayoutPath.IsEmpty() ? TEXT("none") : *Report.LayoutPath);
		return Report;
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		TArray<FString> Effects;
		Effects.Add(FString::Printf(TEXT("UV layout: %s into channel %d of %s, saved"), UV::MethodName(Plan.Method), Plan.Channel, *Plan.Asset));
		for (const FUVLodBuilt& Lod : Built->Lods)
		{
			Effects.Add(FString::Printf(TEXT("| %s: %s -> %s"), *LodName(Lod.Lod), *Lod.Before.Verdict, *UV::Describe(Lod.After.Verdict, Lod.After.Metrics)));
		}
		Report.LayoutPath = UV::LayoutPathOf(Plan.Asset, First.Lod, Plan.Channel, false);
		const FString OperationId = Report.OperationId;
		if (Hold(OperationId, UVApplyToolName, TEXT("model.uv.v1"), Plan.Hash, Plan.Asset, Effects, {Plan.Asset}, [Request, OperationId]()
			{
				const FHyperAIModelUVApplyReport Done = ExecuteUV(Request, OperationId, nullptr, nullptr);
				Record(OperationId, UVApplyToolName, Done.bOk, Done.Asset, Done.Diagnostic);
			}, Report.Status, Report.Diagnostic))
		{
			Report.bOk = true;
		}
		return Report;
	}
	FHyperAIModelUVApplyReport Done = ExecuteUV(Request, Report.OperationId, &Plan, Built.Get());
	Record(Report.OperationId, UVApplyToolName, Done.bOk, Done.Asset, Done.Diagnostic);
	return Done;
}

FHyperAIModelBakeReport FHyperAIStudioModelingContracts::BakeMaps(const FHyperAIModelBakeRequest& Request)
{
	using namespace HyperAIStudio::ModelingSurface::Private;
	FHyperAIModelBakeReport Report;
	Report.OperationId = NewOperationId(TEXT("bake"), Request.OperationId);
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (IsPlaying())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Play in Editor is running; stop it, then send the bake again.");
		return Report;
	}
	FString Error;
	FBakePlan Plan;
	UStaticMesh* Target = nullptr;
	UStaticMesh* Source = nullptr;
	if (!CheckBakePlan(Request, Plan, Target, Source, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	Report.Target = Plan.Target;
	Report.Source = FString::Printf(TEXT("%s (%s)"), *Plan.Source, *LodName(Plan.SourceLod));
	Report.TargetUVs = Plan.TargetUVs;
	Report.Notes = Plan.Notes;
	// What would be written, named before anything runs.
	for (const Bake::EMap Map : OutputsOf(Plan.Maps, Plan.bORM))
	{
		FHyperAIModelBakedTexture& Texture = Report.Textures.AddDefaulted_GetRef();
		const Bake::FTextureKind Kind = Bake::KindOf(Map);
		Texture.Map = Bake::MapName(Map);
		Texture.Path = Plan.Paths[Map];
		Texture.Size = Request.Resolution;
		Texture.Format = TEXT("BGRA8");
		Texture.Compression = Kind.CompressionName;
		Texture.bSRGB = Kind.bSRGB;
		Texture.bReplaced = Plan.Exists[Map];
	}
	Report.MaterialInstance = Plan.Material;
	Report.AssignedSlots = Plan.Slots;
	if (Request.bDryRun)
	{
		if (Request.bPreviewDryRun)
		{
			TArray<Bake::FImage> Images;
			int32 Ms = 0;
			if (!RunBake(Request, Plan, *Target, *Source, FMath::Min(128, Request.Resolution), FMath::Min(8, Request.OcclusionRays), Images, Ms, Error))
			{
				Report.Status = TEXT("invalid_request");
				Report.Diagnostic = Error + TEXT(" Nothing was changed.");
				return Report;
			}
			for (const Bake::FImage& Image : Images)
			{
				FHyperAIModelBakedTexture* Texture = Report.Textures.FindByPredicate([&Image](const FHyperAIModelBakedTexture& Each) { return Each.Map == Bake::MapName(Image.Map); });
				if (Texture) Bake::Stats(Image, Texture->Min, Texture->Mean, Texture->Max);
			}
			Report.BakeMs = Ms;
			Report.PreviewPath = Bake::SheetPathOf(Plan.Target, true);
			if (!Bake::DrawSheet(Images, Report.PreviewPath, Error)) Report.PreviewPath.Reset();
		}
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%d texture(s) would be written to %s from %s; target UVs %s.%s Nothing was changed."), Report.Textures.Num(), *Plan.Folder, *Report.Source,
			*Plan.TargetUVs, Report.PreviewPath.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" 128 px preview: %s."), *Report.PreviewPath));
		return Report;
	}
	TArray<FString> Touches;
	for (const FHyperAIModelBakedTexture& Texture : Report.Textures) Touches.Add(Texture.Path);
	if (!Plan.Material.IsEmpty()) Touches.Add(Plan.Material);
	if (!Plan.Slots.IsEmpty()) Touches.Add(Plan.Target);
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		TArray<FString> Effects;
		TArray<FString> Names;
		for (const Bake::EMap Map : Plan.Maps) Names.Add(Bake::MapName(Map));
		Effects.Add(FString::Printf(TEXT("Bake %s at %d px from %s onto %s, saved"), *FString::Join(Names, TEXT(", ")), Request.Resolution, *Plan.Source, *Plan.Target));
		for (const FHyperAIModelBakedTexture& Texture : Report.Textures)
		{
			Effects.Add(FString::Printf(TEXT("| %s %s (%s%s)"), Texture.bReplaced ? TEXT("overwrite") : TEXT("write"), *Texture.Path, *Texture.Compression, Texture.bSRGB ? TEXT(", sRGB") : TEXT("")));
		}
		if (!Plan.Material.IsEmpty()) Effects.Add(FString::Printf(TEXT("| material %s%s"), *Plan.Material, Plan.Slots.IsEmpty() ? TEXT("") : TEXT(", put on the target's slots")));
		Report.PreviewPath = Bake::SheetPathOf(Plan.Target, false);
		const FString OperationId = Report.OperationId;
		if (Hold(OperationId, BakeToolName, TEXT("model.bake.v1"), Plan.Hash, Plan.Target, Effects, Touches, [Request, OperationId]()
			{
				const FHyperAIModelBakeReport Done = ExecuteBake(Request, OperationId);
				Record(OperationId, BakeToolName, Done.bOk, Done.Target, Done.Diagnostic);
			}, Report.Status, Report.Diagnostic))
		{
			Report.bOk = true;
		}
		return Report;
	}
	FHyperAIModelBakeReport Done = ExecuteBake(Request, Report.OperationId);
	Record(Report.OperationId, BakeToolName, Done.bOk, Done.Target, Done.Diagnostic);
	return Done;
}

FHyperAIModelPrepareReport FHyperAIStudioModelingContracts::PrepareModel(const FHyperAIModelPrepareRequest& Request)
{
	using namespace HyperAIStudio::ModelingSurface::Private;
	FHyperAIModelPrepareReport Report;
	Report.OperationId = NewOperationId(TEXT("prepare"), Request.OperationId);
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (IsPlaying())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Play in Editor is running; stop it, then send it again.");
		return Report;
	}
	FString Error;
	FPreparePlan Plan;
	UStaticMesh* Mesh = nullptr;
	if (!CheckPreparePlan(Request, Plan, Mesh, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	Report.Asset = Plan.Asset;
	Report.Notes = Plan.Notes;
	Report.VerdictBefore = Plan.Before.Verdict;
	Report.VerdictAfter = Plan.Before.Verdict;
	Report.LowPoly = Plan.LowPoly;
	if (Request.bDryRun)
	{
		// Built in memory: the dry run's answer, and the check that the unwrap and the decimation run.
		FPrepareBuilt Built;
		if (!BuildPrepare(Plan, Request, *Mesh, Built, Error))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Error + TEXT(" Nothing was changed.");
			return Report;
		}
		{
			Report.Steps.Add(FString::Printf(TEXT("uv_check: channel 0 is %s"), *UV::Describe(Plan.Before.Verdict, Plan.Before.Metrics)));
			const bool bHasColor = Plan.SlotColors.ContainsByPredicate([](const TWeakObjectPtr<UTexture2D>& Texture) { return Texture.IsValid(); });
			if (Plan.bUnwrap)
			{
				Report.bUnwrapped = true;
				Report.VerdictAfter = Built.After.Verdict;
				Report.Steps.Add(FString::Printf(TEXT("unwrap: %s into channel 0, would be %s"), UV::MethodName(Plan.Method), *UV::Describe(Built.After.Verdict, Built.After.Metrics)));
				if (bHasColor || Plan.Normal.IsValid())
				{
					Report.Steps.Add(FString::Printf(TEXT("carry: the old layout's textures baked onto the new one, in %s on every slot"), *PrepareMaterialPath(Plan.Folder, Plan.Owner)));
				}
				Report.LayoutPath = UV::LayoutPathOf(Plan.Asset, 0, 0, true);
				if (!UV::DrawLayout(Built.After.Soup, Built.After.Metrics, Report.LayoutPath, Error)) Report.LayoutPath.Reset();
			}
			else
			{
				Report.Steps.Add(FString::Printf(TEXT("unwrap: skipped (UnwrapUVs %s, channel 0 is %s)"), *Plan.Unwrap, *Plan.Before.Verdict));
			}
			if (Built.Low)
			{
				Report.LowPolyTriangles = Built.Low->GetTriangleCount();
				TArray<FString> Names;
				for (const Bake::EMap Map : Plan.Maps) Names.Add(Bake::MapName(Map));
				Report.Steps.Add(FString::Printf(TEXT("low_poly: %s, %d triangles (from %d), unwrapped (%s); bake %s from the original"), *Plan.LowPoly, Report.LowPolyTriangles,
					Plan.Triangles, *UV::Describe(Built.LowUVs.Verdict, Built.LowUVs.Metrics), *FString::Join(Names, TEXT(", "))));
			}
			if (Request.bLightmapUVs) Report.Steps.Add(TEXT("lightmap: channel 1 made from channel 0 when the mesh builds"));
			Report.bOk = true;
			Report.Status = TEXT("checked");
			Report.Diagnostic = FString::Printf(TEXT("%s: %s. Nothing was changed."), *Plan.Asset, *FString::Join(Report.Steps, TEXT("; ")));
			return Report;
		}
	}
	if (!Plan.bUnwrap && Plan.LowPoly.IsEmpty() && !Request.bLightmapUVs)
	{
		FHyperAIModelPrepareReport Done = ExecutePrepare(Request, Report.OperationId);
		return Done;
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		TArray<FString> Effects;
		TArray<FString> Touches = {Plan.Asset};
		Effects.Add(FString::Printf(TEXT("Prepare %s (channel 0 is %s), saved"), *Plan.Asset, *Plan.Before.Verdict));
		if (Plan.bUnwrap) Effects.Add(FString::Printf(TEXT("| unwrap channel 0 with %s, carrying its textures to the new layout"), UV::MethodName(Plan.Method)));
		if (!Plan.LowPoly.IsEmpty())
		{
			Effects.Add(FString::Printf(TEXT("| new %s, about %d triangles, with baked maps"), *Plan.LowPoly, Plan.TargetTriangles));
			Touches.Add(Plan.LowPoly);
		}
		if (Request.bLightmapUVs) Effects.Add(TEXT("| lightmap UVs into channel 1 at build"));
		Report.LayoutPath = UV::LayoutPathOf(Plan.Asset, 0, 0, false);
		const FString OperationId = Report.OperationId;
		if (Hold(OperationId, PrepareToolName, TEXT("model.prepare.v1"), Plan.Hash, Plan.Asset, Effects, Touches, [Request, OperationId]()
			{
				const FHyperAIModelPrepareReport Done = ExecutePrepare(Request, OperationId);
				Record(OperationId, PrepareToolName, Done.bOk, Done.Asset, Done.Diagnostic);
			}, Report.Status, Report.Diagnostic))
		{
			Report.bOk = true;
		}
		return Report;
	}
	FHyperAIModelPrepareReport Done = ExecutePrepare(Request, Report.OperationId);
	Record(Report.OperationId, PrepareToolName, Done.bOk, Done.Asset, Done.Diagnostic);
	return Done;
}

FHyperAIModelUVCheckReport UHyperAIStudioModelingToolset::hyper_model_uv_check(const FHyperAIModelUVCheckRequest& Request)
{
	return FHyperAIStudioModelingContracts::CheckUVs(Request);
}

FHyperAIModelUVApplyReport UHyperAIStudioModelingToolset::hyper_model_uv_apply(const FHyperAIModelUVApplyRequest& Request)
{
	return FHyperAIStudioModelingContracts::ApplyUVs(Request);
}

FHyperAIModelBakeReport UHyperAIStudioModelingToolset::hyper_model_bake(const FHyperAIModelBakeRequest& Request)
{
	return FHyperAIStudioModelingContracts::BakeMaps(Request);
}

FHyperAIModelPrepareReport UHyperAIStudioModelingToolset::hyper_model_prepare(const FHyperAIModelPrepareRequest& Request)
{
	return FHyperAIStudioModelingContracts::PrepareModel(Request);
}

#undef LOCTEXT_NAMESPACE
