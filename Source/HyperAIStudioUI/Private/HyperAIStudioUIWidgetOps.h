// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioUIToolset.h"

class UClass;
class UWidget;
class UWidgetBlueprint;

/**
 * The widget half of a Widget Blueprint plan: add widgets to the tree, set their properties, styles and slot layout,
 * and give them navigation rules. Every write goes through the property's setter between PreEditChange and
 * PostEditChangeProperty, the Details panel's way, inside the plan's one transaction.
 */
namespace HyperAIStudio::UI::WidgetOps
{
	struct FPlannedWidget
	{
		const UClass* Class = nullptr;
		FName Parent;
	};

	/** The widget tree as a plan leaves it so far: the Blueprint's widgets, then each widget.add before this op. */
	struct FPlannedTree
	{
		TMap<FName, FPlannedWidget> Widgets;
		FName Root;

		explicit FPlannedTree(const UWidgetBlueprint& Blueprint);
		const FPlannedWidget* Find(const FString& Subject) const;
		int32 ChildCount(FName Parent) const;
		FString Names() const;
	};

	/** "name:Title" or "Title" to Title. */
	FString WidgetNameOf(const FString& Subject);

	/** widget.add, widget.set_property, widget.set_style, widget.set_slot or widget.set_navigation. */
	bool IsWidgetOp(const FString& Kind);

	/** Checks one widget op against the tree as earlier ops leave it, and records a widget.add in Tree. */
	bool Validate(const FHyperAIUIPlanOperation& Operation, const UWidgetBlueprint& Blueprint, FPlannedTree& Tree,
		FString& OutNote, FString& OutError);

	/** Applies one validated widget op inside the caller's transaction. */
	bool Apply(UWidgetBlueprint& Blueprint, const FHyperAIUIPlanOperation& Operation, FString& OutError);

	/** After compile: true when the op's effect is in the Widget Blueprint. */
	bool Landed(const UWidgetBlueprint& Blueprint, const FHyperAIUIPlanOperation& Operation, FString& OutWhy);

	/** The float property an animation's float track may key on this widget class, e.g. RenderOpacity or Percent. */
	FProperty* FindAnimatableFloat(const UClass& Class, const FString& Name);

	/** Names of the float properties FindAnimatableFloat accepts on this class, for an error message. */
	FString AnimatableFloatNames(const UClass& Class);

	/** Every widget's name, class, parent, slot class and navigation, for the plan's revision. */
	void AppendTreeRevision(const UWidgetBlueprint& Blueprint, TArray<FString>& OutEntries);
}
