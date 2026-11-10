// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAudioAuthoringToolset.h"

/** The mix's assets as plans: Sound Classes, Sound Mixes, attenuation, concurrency and submixes, and their settings by name. */
namespace HyperAIStudio::AudioAuthoring::Mix
{
	/** One effect of a submix plan, checked. */
	struct FCheckedEffect
	{
		FHyperAISubmixEffect Spec;
		FString Package;
		UClass* Class = nullptr;
		TWeakObjectPtr<UObject> Existing;
	};

	/** One asset of a plan, checked: what it is, whether it is made, and the lines the report and approval show. */
	struct FCheckedAsset
	{
		FHyperAISoundMixAsset Spec;
		FString Package;
		UClass* Class = nullptr;
		TWeakObjectPtr<UObject> Existing;
		/** The parent's package, made in this plan or already there; empty leaves it, none detaches. */
		FString ParentPackage;
		TArray<FString> AdjusterPackages;
		TArray<FCheckedEffect> Effects;
	};

	/** sound_class and the rest, for an asset's class; empty for one plans do not change. */
	FString KindOf(const UObject& Asset);
	UClass* ClassOfKind(const FString& Kind);

	bool Check(const FHyperAISoundMixPlanRequest& Request, TArray<FCheckedAsset>& Out, FHyperAISoundMixPlanReport& Report);

	/** Makes and changes the assets, inside the caller's transaction. Touched holds every asset to save, in order. */
	bool Apply(const TArray<FCheckedAsset>& Checked, TArray<UObject*>& OutTouched, FHyperAISoundMixPlanReport& Report);

	/** An asset's settings by property name: the common ones, or every editable one. */
	TArray<FHyperAIAudioSetting> ReadSettings(UObject& Asset, bool bAll);

	/** Fills Info for a Sound Class, Sound Mix, attenuation, concurrency or submix. */
	void Describe(UObject& Asset, bool bAllSettings, FHyperAISoundAssetInfo& Info);

	/** Sets one setting by name on any asset whose settings these name (used by the generated cues for their class). */
	bool ApplySetting(UObject& Asset, const FHyperAIAudioSetting& Setting, FString& OutError);
}
