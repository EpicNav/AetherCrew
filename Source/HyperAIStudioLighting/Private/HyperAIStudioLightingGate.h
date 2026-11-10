// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioColorLut.h"
#include "HyperAIStudioLightingToolset.h"

class AActor;
class UWorld;

/** The only code that reads pixels or touches lighting actors; the toolset handles contracts and execution. */
namespace HyperAIStudio::Lighting::Gate
{
	/** Pixels are display-referred sRGB (a viewport capture or an 8-bit image). */
	FHyperAILightingImageMetrics MeasurePixels(TConstArrayView<FColor> Pixels, int32 Width, int32 Height);
	/** Earth mover's distance between two histograms of equal length, 0 (same) to 1. */
	float HistogramDistance(const TArray<float>& A, const TArray<float>& B);
	/** Fills the deltas, bMatched and ranked suggestions from two measurements. */
	void CompareMetrics(const FHyperAILightingImageMetrics& Reference, const FHyperAILightingImageMetrics& Current,
		bool bExposureLocked, float MatchThreshold, FHyperAILightingCompareReport& Report);

	/** PNG, JPG, BMP or TGA inside the project folder. */
	bool LoadProjectImage(const FString& Path, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError);
	/** Redraws the active level viewport and reads it back. */
	bool CaptureActiveViewport(TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError);
	bool WritePng(const FString& Path, TConstArrayView<FColor> Pixels, int32 Width, int32 Height);

	UWorld* GetEditorWorld();
	/**
	 * Directional lights, sky lights, height fogs, sky atmospheres and unbound post-process volumes, then Blueprint skies
	 * (kind sky_system: Ultra Dynamic Sky and Weather, Sun Sky, Sky Sphere) with the variables that move their look, then
	 * reflection captures.
	 */
	TArray<FHyperAILightingActorRecord> ReadActors(UWorld& World);
	/** How the project renders light, as the level's post process leaves it, and what that means for a match. */
	TArray<FHyperAILightingProperty> ReadRendering(UWorld& World);
	/**
	 * Materials on what the view of CameraLabel's camera (else HyperAI_MatchCamera, else the level viewport) shows, most of
	 * the frame first, up to Limit; OutTotal counts every one there. None without a view.
	 */
	TArray<FHyperAILightingMaterialRecord> ReadMaterials(UWorld& World, const FString& CameraLabel, int32 Limit, int32& OutTotal);
	/**
	 * Point, spot and rect lights: light actors, and lights held inside other actors such as a torch Blueprint. With a view
	 * (CameraLabel's camera, else HyperAI_MatchCamera, else the level viewport) those whose light reaches into it come
	 * first, nearest first, each with in_view and distance; without one, in path order. LabelFilter keeps actors whose
	 * label contains it. OutTotal counts every local light in the level; OutView names the view, empty for none.
	 */
	TArray<FHyperAILightingActorRecord> ReadLocalLights(UWorld& World, const FString& CameraLabel, const FString& LabelFilter, int32 Limit,
		int32& OutTotal, FString& OutView, bool bUseView = true);
	/** Level path plus every value ReadActors reports; any lighting change moves it. */
	FString ComputeRevision(UWorld& World);
	/** True when the level viewport uses a fixed exposure or an unbound post-process volume locks it, and no sky applies its own. */
	bool IsExposureLocked(UWorld& World);
	/**
	 * A Blueprint sky that owns its sun, sky light and fog, such as Ultra Dynamic Sky, as "<system> ('<label>')": the level
	 * is lit through its variables, so stock-light edits are refused. Empty when the level has none.
	 */
	FString LightOwningSky(UWorld& World);

	/**
	 * Checks every op against the level as it would be at that point in the plan; changes nothing. OutNotes gets what a
	 * valid plan sets that is rarely meant, op by op: a sky atmosphere scattering scale far past the engine's default.
	 */
	bool ValidateOps(UWorld& World, const TArray<FHyperAILightingOp>& Ops, FString& OutError, TArray<FString>* OutNotes = nullptr);
	/** Applies ops in order as one undo step, adding a missing actor where an op needs one. */
	bool ApplyOps(UWorld& World, const TArray<FHyperAILightingOp>& Ops, TArray<AActor*>& OutTouched, FString& OutError);
	/**
	 * Every set_actor_material op's slot holds its material, and every material parameter op's instance holds its value;
	 * the Validate phase's postcondition.
	 */
	bool CheckMaterialOps(UWorld& World, const TArray<FHyperAILightingOp>& Ops, FString& OutError);
	/** The actors ops resolve to now, without adding any; for a level-wide material change, those it left unsaved too. */
	TArray<AActor*> ResolveTargets(UWorld& World, const TArray<FHyperAILightingOp>& Ops);
	/**
	 * Packages holding these actors (their own under One File Per Actor, otherwise the level's), and the unsaved assets a
	 * plan made for them: a volume's colour-grading LUT, the clouds' material instance, the level's look-dev instances.
	 */
	TArray<UPackage*> PackagesOf(const TArray<AActor*>& Actors);

	/**
	 * The colour-grading LUT the level's grade applies now, weakened by its intensity: that of the first enabled, unbound
	 * post-process volume that sets one. False, with the identity, when there is none. OutPath names its texture.
	 */
	bool ReadLevelLut(UWorld& World, Lut::FTable& OutTable, FString& OutPath);
	/**
	 * Where made assets go: the LUTs set_post_process color_grading_lut imports, the clouds' material instances, and the
	 * look-dev instances set_material_parameter makes, under LookDev/<level>.
	 */
	inline const TCHAR* const MadeAssetsRoot = TEXT("/Game/HyperAI/");
}
