// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioMotionWarpingToolset.h"

class FProperty;
class UAnimNotifyState_MotionWarping;
class UAnimSequenceBase;
class UBlueprint;
class UMotionWarpingComponent;
class USCS_Node;

/**
 * The motion warping plan's checks, the reflection that reads and writes modifier and component settings, and the views
 * inspect and the plan report share. A window is a UAnimNotifyState_MotionWarping on the asset's Notifies, set up the way
 * the notify panel (and hyper_animation_apply_plan) adds a notify state. Game thread only.
 */
namespace HyperAIStudio::MotionWarping::Gate
{
	struct FKindInfo
	{
		const TCHAR* Kind = TEXT("");
		const TCHAR* Summary = TEXT("");
	};

	/** Every op kind, in the order the docs list them. */
	const TArray<FKindInfo>& Kinds();
	bool IsKind(const FString& Kind);
	FString KindList();

	struct FModifierInfo
	{
		/** The word a plan uses: skew_warp, scale, adjustment_blend, precomputed_warp. */
		const TCHAR* Word = TEXT("");
		const TCHAR* Summary = TEXT("");
		UClass* Class = nullptr;
		/** It warps to a named target (a URootMotionModifier_Warp). */
		bool bWarp = false;
		/** Epic marks it experimental (hidden from the notify's modifier list, or Experimental). */
		bool bExperimental = false;
	};

	/** The modifiers a window can take, skew_warp first (the one a new Motion Warping notify starts with). */
	const TArray<FModifierInfo>& Modifiers();
	const FModifierInfo* FindModifier(const FString& Word);
	FString ModifierList();
	/** The plan's word for a modifier class, or its class name when it is another one; none for no modifier. */
	FString ModifierWord(const UClass* Class);

	/** What Params may set on Class: editable, not read-only, not an instanced object, not a delegate. */
	bool IsSettable(const FProperty& Property);
	/** The settable properties of Class; bOwnOnly leaves out what its parents declare (the component's are only its own). */
	TArray<FProperty*> Settable(const UClass& Class, bool bOwnOnly);
	/** A setting by name in any case, or by its display name with the spaces taken out (WarpTranslation for bWarpTranslation). */
	FProperty* FindSetting(const UClass& Class, const FString& Name);
	/** "Name: type, default value" of each settable property, read from the class defaults. */
	TArray<FString> DescribeSettings(const UClass& Class, bool bOwnOnly);
	/** A value as Params takes it back: enums by short name, bools true or false, numbers plain, the rest in Unreal's text form. */
	FString ValueText(const FProperty& Property, const void* Container);
	/** "Name=Value" of each settable property of Object. */
	TArray<FString> SettingsOf(const UObject& Object, bool bOwnOnly);

	/** "Name=Value; Name=Value" split into pairs; a pair without '=' or a name given twice is refused. */
	bool SplitParams(const FString& Text, TArray<TPair<FString, FString>>& Out, FString& OutError);

	/**
	 * Text as Property's text import takes it: enums by short name (Facing), full name or display name; bools true, false,
	 * yes, no, 1, 0, on, off; numbers inside the property's clamp; vectors "x,y,z" and rotators "pitch,yaw,roll"; a list
	 * "a,b" for an array; an asset path that must be there. Checked by importing it into scratch memory.
	 */
	bool CoerceValue(const FProperty& Property, const FString& Text, FString& OutImport, FString& OutError);

	struct FSetting
	{
		FProperty* Property = nullptr;
		FString Import;
	};

	/** Params against Class: unknown names refused listing the valid ones, each value coerced. */
	bool ParseSettings(const UClass& Class, const FString& Params, TArray<FSetting>& Out, FString& OutError);
	/** Writes the settings into Object with the text import; Modify() it first when it is not new. */
	bool WriteSettings(UObject& Object, const TArray<FSetting>& Settings, FString& OutError);

	enum class EAssetKind : uint8
	{
		Montage,
		Sequence,
		Character
	};

	FString AssetKindName(EAssetKind Kind);

	/** A window as the asset holds it. */
	struct FWindowRef
	{
		UAnimNotifyState_MotionWarping* State = nullptr;
		float Start = 0.f;
		float End = 0.f;
		int32 TrackIndex = 0;
	};

	/** The asset's windows, by start time (the Notifies order for equal starts): the order Index counts. */
	TArray<FWindowRef> WindowsOf(const UAnimSequenceBase& Asset);
	int32 FindEvent(const UAnimSequenceBase& Asset, const UAnimNotifyState_MotionWarping* State);
	FHyperAIMotionWarpingWindow DescribeWindow(const UAnimSequenceBase& Asset, const FWindowRef& Window, int32 Index);

	/** What inspect and the plan report show of a montage or sequence. */
	struct FAnimView
	{
		float Length = 0.f;
		bool bRootMotion = false;
		TArray<FHyperAIMotionWarpingWindow> Windows;
		TArray<FString> WarpTargets;
		TArray<FString> Warnings;
	};

	FAnimView ViewAnim(const UAnimSequenceBase& Asset);

	/** A character Blueprint's Motion Warping component, wherever it comes from. */
	struct FComponentFound
	{
		const UMotionWarpingComponent* Template = nullptr;
		/** The Blueprint's own construction script node, when the component is its own. */
		USCS_Node* OwnNode = nullptr;
		FString Name;
		/** "this Blueprint", the parent Blueprint's path, or "native class X". */
		FString Source;
	};

	bool IsCharacterBlueprint(const UBlueprint& Blueprint);
	bool FindComponent(const UBlueprint& Blueprint, FComponentFound& Out);

	/** One checked op. */
	struct FStep
	{
		int32 Index = 0;
		FString Kind;
		FHyperAIMotionWarpingOp Op;
		/** window.set and window.remove: the window, as it was before the plan. */
		UAnimNotifyState_MotionWarping* Window = nullptr;
		/** window.add: its modifier; window.set: a new modifier, null to keep the one it has. */
		UClass* ModifierClass = nullptr;
		TArray<FSetting> Settings;
		/** window.add and window.set: the window's times after the op. */
		float Start = 0.f;
		float End = 0.f;
		bool bTimes = false;
		/** component.add: the variable name asked for. */
		FString ComponentName;

		FString Describe() const;
	};

	struct FPlan
	{
		UObject* Asset = nullptr;
		/** Object path of the asset. */
		FString AssetPath;
		EAssetKind Kind = EAssetKind::Montage;
		TArray<FStep> Steps;
		/** Warp target names the windows used before the plan. */
		TArray<FString> TargetsBefore;
	};

	/** The asset at Path (package or object path) under /Game: a montage, a sequence or a character Blueprint. */
	UObject* LoadAsset(const FString& Path, FString& OutError);

	/** Everything validated, nothing changed: refusals say what is wrong and what is accepted. */
	bool Check(const FHyperAIMotionWarpingPlanRequest& Request, FPlan& OutPlan, FString& OutError);
}
