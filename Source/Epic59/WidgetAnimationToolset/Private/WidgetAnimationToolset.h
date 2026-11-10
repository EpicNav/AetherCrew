// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions
#include "MovieSceneBindingProxy.h"
#include "WidgetAnimationToolset.generated.h"

class UWidgetBlueprint;
class UWidgetAnimation;

/**
 * Widget animation summary. Returned by ListWidgetAnimations / FindWidgetAnimation /
 * CreateWidgetAnimation.
 */
USTRUCT(BlueprintType)
struct FWidgetAnimationInfo
{
	GENERATED_BODY()

	/** The animation asset. Pass to AddWidgetToAnimation and every SequencerScripting tool that accepts a UMovieSceneSequence. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetAnimationToolset")
	TObjectPtr<UWidgetAnimation> Animation = nullptr;

	/** Internal asset name (also the Blueprint variable name for runtime access). */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetAnimationToolset")
	FName AnimationName;

	/** User-facing display label shown in the Animations panel. Defaults to AnimationName
	  * after CreateWidgetAnimation; SetWidgetAnimationDisplayLabel overrides it. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetAnimationToolset")
	FString DisplayLabel;

	/** Playback length in seconds. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetAnimationToolset")
	float LengthSeconds = 0.0f;

	/** Number of bindings (possessables) registered on this animation. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetAnimationToolset")
	int32 BindingCount = 0;
};

/**
 * UMG Widget Animation toolset for AI-driven authoring of UWidgetAnimation assets.
 *
 * UWidgetAnimation extends UMovieSceneSequence, so every UMovieSceneSequenceExtensions,
 * UMovieSceneTrackExtensions, UMovieSceneSectionExtensions, and UMovieSceneConditionExtensions
 * method that accepts a UMovieSceneSequence, UMovieSceneTrack, UMovieSceneSection, or
 * channel works on widget animations unmodified. Use this toolset for the UMG-specific
 * lifecycle and binding work; use those SequencerScripting providers for tracks,
 * sections, keyframes, and conditions.
 */
UCLASS(BlueprintType)
class UWidgetAnimationToolset : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	// ---- Lifecycle ----

	/**
	 * Create a new UWidgetAnimation on a Widget Blueprint and return its summary.
	 * The empty FWidgetAnimationInfo return signals failure (Animation == nullptr).
	 * @param AnimationName Internal asset name. Must not collide with an existing animation.
	 * @param LengthSeconds Initial playback-range length in seconds. Defaults to 1.0s.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable, AIAccessMode = "Write"), Category = "WidgetAnimation|Lifecycle")
	static FWidgetAnimationInfo CreateWidgetAnimation(
		UWidgetBlueprint* WidgetBlueprint,
		FName AnimationName,
		float LengthSeconds = 1.0f);

	/** List all animations on a Widget Blueprint. */
	UFUNCTION(BlueprintCallable, meta = (AICallable, AIAccessMode = "Read"), Category = "WidgetAnimation|Lifecycle")
	static TArray<FWidgetAnimationInfo> ListWidgetAnimations(UWidgetBlueprint* WidgetBlueprint);

	/** Find an animation by internal name. Returns empty info (Animation == nullptr) when not found. */
	UFUNCTION(BlueprintCallable, meta = (AICallable, AIAccessMode = "Read"), Category = "WidgetAnimation|Lifecycle")
	static FWidgetAnimationInfo FindWidgetAnimation(
		UWidgetBlueprint* WidgetBlueprint,
		FName AnimationName);

	/** Rename an animation. NewName must not collide with an existing animation on the blueprint. */
	UFUNCTION(BlueprintCallable, meta = (AICallable, AIAccessMode = "Write"), Category = "WidgetAnimation|Lifecycle")
	static bool RenameWidgetAnimation(
		UWidgetBlueprint* WidgetBlueprint,
		UWidgetAnimation* Animation,
		FName NewName);

	/** Set the user-facing display label shown in the Animations panel. */
	UFUNCTION(BlueprintCallable, meta = (AICallable, AIAccessMode = "Write"), Category = "WidgetAnimation|Lifecycle")
	static bool SetWidgetAnimationDisplayLabel(
		UWidgetAnimation* Animation,
		const FString& DisplayLabel);

	/**
	 * Set the animation playback length in seconds. Updates the MovieScene playback
	 * range to [0, LengthSeconds] in tick resolution. LengthSeconds must be > 0.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable, AIAccessMode = "Write"), Category = "WidgetAnimation|Lifecycle")
	static bool SetWidgetAnimationLength(
		UWidgetAnimation* Animation,
		float LengthSeconds);

	/** Remove an animation from a Widget Blueprint. */
	UFUNCTION(BlueprintCallable, meta = (AICallable, AIAccessMode = "Write"), Category = "WidgetAnimation|Lifecycle")
	static bool RemoveWidgetAnimation(
		UWidgetBlueprint* WidgetBlueprint,
		UWidgetAnimation* Animation);

	// ---- Bindings ----

	/**
	 * Add a UWidget (or a UPanelSlot with non-null content) as a possessable binding on a
	 * widget animation. Returns a FMovieSceneBindingProxy usable with every SequencerScripting
	 * method that takes a binding (MovieSceneBindingExtensions.AddTrack, etc.).
	 *
	 * ObjectToBind must be either:
	 *   - A UWidget inside the Widget Blueprint's tree (typical case: animate a child widget's properties).
	 *   - A UPanelSlot whose Content is non-null (animate slot properties like padding/anchors).
	 *
	 * The Widget Blueprint must have a compiled GeneratedClass. The implementation compiles the
	 * blueprint if needed before binding.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable, AIAccessMode = "Write"), Category = "WidgetAnimation|Bindings")
	static FMovieSceneBindingProxy AddWidgetToAnimation(
		UWidgetBlueprint* WidgetBlueprint,
		UWidgetAnimation* Animation,
		UObject* ObjectToBind);

	/** Get all bindings on a widget animation as FMovieSceneBindingProxy entries. */
	UFUNCTION(BlueprintCallable, meta = (AICallable, AIAccessMode = "Read"), Category = "WidgetAnimation|Bindings")
	static TArray<FMovieSceneBindingProxy> GetWidgetAnimationBindings(UWidgetAnimation* Animation);

	/** Remove a binding (by GUID) from a widget animation. Removes both the MovieScene possessable and the UMG binding entry. */
	UFUNCTION(BlueprintCallable, meta = (AICallable, AIAccessMode = "Write"), Category = "WidgetAnimation|Bindings")
	static bool RemoveWidgetBinding(UWidgetAnimation* Animation, FGuid BindingGuid);
};
