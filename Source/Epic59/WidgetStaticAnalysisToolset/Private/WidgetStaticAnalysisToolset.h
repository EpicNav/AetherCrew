// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Extensions/UIComponent.h"
#include "Templates/SubclassOf.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions
#include "UObject/NameTypes.h"
#include "UObject/ObjectPtr.h"
#include "UObject/SoftObjectPath.h"
#include "WidgetDescriptionTypes.h"
#include "WidgetStaticAnalysisToolset.generated.h"

class UWidgetBlueprint;
class UMVVMViewModelBase;

// TraceWidgetPropertyOverrides types

/** Where a traced property write came from: static origins (first seven) or the Blueprint-graph walk (last three). */
UENUM(BlueprintType)
enum class EWidgetPropertyAssignmentSource : uint8
{
	NativeClassDefault,
	GeneratedClassDefault,
	WidgetTreeArchetype,
	NamedSlotArchetype,
	VariantResolver,
	LegacyBinding,
	MVVMBinding,
	PreConstructSetter,
	ConstructSetter,
	RuntimeHandler,
};

/** Applied: guaranteed to run. DeclaredButUnconfirmed: might run (branch-gated write, enabled MVVM binding,
 *  variant candidate). KnownNotApplied: never runs (disabled MVVM binding). */
UENUM(BlueprintType)
enum class EWidgetPropertyApplicationStatus : uint8
{
	Applied,
	DeclaredButUnconfirmed,
	KnownNotApplied,
};

/** Discriminates the one Flags array's two possible row meanings. */
UENUM(BlueprintType)
enum class EWidgetPropertyOverrideFlagKind : uint8
{
	/** Guaranteed overwritten by a later, unconditional write to the same property. */
	Redundant,
	/** Whether this write or another wins cannot be statically determined. */
	Conflict,
};

/** One write location for one traced property; one Chains row. */
USTRUCT(BlueprintType)
struct FWidgetPropertyOverrideEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FName WidgetName;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString PropertyName;

	/** Position within this entry's own (WidgetName, PropertyName) chain. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 Order = 0;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	EWidgetPropertyAssignmentSource Source = EWidgetPropertyAssignmentSource::NativeClassDefault;

	/** Which widget variable this write targets, and whether it's a VariableSet or a setter call. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString SourceDetail;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString ExportedValue;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	EWidgetPropertyApplicationStatus ApplicationStatus = EWidgetPropertyApplicationStatus::Applied;
};

/** The write TraceWidgetPropertyOverrides believes wins, one row per traced (widget, property) pair. */
USTRUCT(BlueprintType)
struct FWidgetPropertyWinner
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FName WidgetName;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString PropertyName;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	EWidgetPropertyAssignmentSource WinningSource = EWidgetPropertyAssignmentSource::NativeClassDefault;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString WinningSourceDetail;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString EffectiveValue;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString Rationale;

	/** True only when this winner's own ApplicationStatus is Applied. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	bool bCertain = false;
};

/** One Redundant or Conflict observation about a traced property's write chain. */
USTRUCT(BlueprintType)
struct FWidgetPropertyOverrideFlag
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	EWidgetPropertyOverrideFlagKind Kind = EWidgetPropertyOverrideFlagKind::Redundant;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FName WidgetName;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString PropertyName;

	/** The write location this flag is about, matching the entry's own SourceDetail. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString Location;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString ValueSet;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString Recommendation;
};

/** Return value of TraceWidgetPropertyOverrides. */
USTRUCT(BlueprintType)
struct FWidgetPropertyOverrideResult
{
	GENERATED_BODY()

	/** Keep alphabetically first: output keys are sorted, and this is the canary if the arrays get cut. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 AvailablePropertyCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FWidgetPropertyOverrideEntry> Chains;

	/** One row per distinct traced (widget, property) pair. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FWidgetPropertyWinner> Winners;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FWidgetPropertyOverrideFlag> Flags;
};

// GetWidgetConnectionMap / GetViewModelConnectionMap types

/** Which of the four connection mechanisms a row describes. */
UENUM(BlueprintType)
enum class EWidgetConnectionKind : uint8
{
	VMFieldBinding,
	EventBinding,
	Delegate,
	LegacyPropertyBinding,
};

/** Misconfigured only applies to VMFieldBinding rows, the only kind with a checkable type. Every
 *  other kind stays binary: Connected or Unconnected. */
UENUM(BlueprintType)
enum class EWidgetConnectionStatus : uint8
{
	Connected,
	Unconnected,
	Misconfigured,
};

/** Populated only when Status == Misconfigured. */
UENUM(BlueprintType)
enum class EWidgetConnectionFixability : uint8
{
	Fixable,
	Unbridgeable,
	Unknown,
};

/** One row of a widget's or ViewModel's connection map: a binding, event, or delegate. */
USTRUCT(BlueprintType)
struct FWidgetConnectionEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	EWidgetConnectionKind Kind = EWidgetConnectionKind::VMFieldBinding;

	/** Only meaningful for EventBinding rows ("mvvm" vs "blueprint_bind"). Empty otherwise. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString KindDetail;

	/** The bound field/event/delegate's own name. For VMFieldBinding rows, the ViewModel's source field. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString ConnectionName;

	/** Populated for VMFieldBinding rows only. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString ViewModel;

	/** The widget this connection is declared or found on. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString Widget;

	/** Set instead of Widget when a VMFieldBinding targets a class member variable rather than a
	 *  widget-tree property. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString TargetVariable;

	/** A graph name, "MVVM View", "Blueprint Bindings", or the scope searched if nothing was found. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString DeclaredIn;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	EWidgetConnectionStatus Status = EWidgetConnectionStatus::Connected;

	/** Populated only when Status == Misconfigured. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString Fix;

	/** Populated only when Status == Misconfigured. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	EWidgetConnectionFixability Fixability = EWidgetConnectionFixability::Unknown;
};

/** Return value of GetWidgetConnectionMap. */
USTRUCT(BlueprintType)
struct FWidgetConnectionMapResult
{
	GENERATED_BODY()

	/** Total rows matching ScopeFilter. Keep alphabetically first: output keys are sorted, and this is
	 *  the canary if Connections gets cut. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 AvailableConnectionCount = 0;

	/** Unconnected/Misconfigured rows first. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FWidgetConnectionEntry> Connections;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 ConnectedCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 UnconnectedCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 MisconfiguredCount = 0;
};

/** One Widget Blueprint's own connections to the queried ViewModel, narrowed to just its
 *  VMFieldBinding rows for that ViewModel. */
USTRUCT(BlueprintType)
struct FViewModelWidgetConnections
{
	GENERATED_BODY()

	/** The widget's own soft object path, not a display name. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString WidgetPath;

	/** Unconnected/Misconfigured rows first. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FWidgetConnectionEntry> Connections;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 ConnectedCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 UnconnectedCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 MisconfiguredCount = 0;
};

/** Return value of GetViewModelConnectionMap. */
USTRUCT(BlueprintType)
struct FViewModelConnectionMapResult
{
	GENERATED_BODY()

	/** Sum of every referencing widget's own Connections.Num(). Keep alphabetically first: output keys
	 *  are sorted, and this is the canary if Widgets gets cut. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 AvailableConnectionCount = 0;

	/** Count of referencing Widget Blueprints found. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 AvailableWidgetCount = 0;

	/** Grouped per widget, not one flat list. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FViewModelWidgetConnections> Widgets;
};

// Toolset

/**
 * AI assistant static-analysis tools over UMG widgets: widget tree descriptions, property override
 * tracing, and binding/event/delegate connection maps.
 * Load this when analyzing existing UMG / Widget Blueprint (UserWidget-based) UI assets.
 */
UCLASS(BlueprintType)
class UWidgetStaticAnalysisToolset : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	/**
	 * Full structured description of every widget in the tree (or the subtree rooted at StartWidget).
	 *
	 * Values are the designer-time state stored in the asset. Anything set at runtime (for example a
	 * Blueprint graph changing a slot through SlotAsCanvasSlot, or style state that is not a
	 * UPROPERTY) does not appear; use TraceWidgetPropertyOverrides for graph writes.
	 *
	 * ComposedColors reports the composed pre-display **linear** tint only -- never a display-space
	 * value, and never for URichTextBlock/CommonRichTextBlock (a fundamentally different
	 * per-inline-tag-run model, excluded from v1 coverage). It does not model the contrast setting,
	 * DisplayGamma, retainer boxes, Button's DisabledEffect, or widget animations.
	 *
	 * @param WidgetBlueprint          The WBP asset.
	 * @param StartWidget              Root of the description; nullptr describes the whole tree.
	 * @param MaxDepth                 -1 = no limit; 0 = StartWidget only; N = N levels.
	 * @param PropertyNameFilter       Substring match against property name; TEXT("") matches every property.
	 * @param bSkipDefaultValues       When true (default), a property whose value matches the class default is excluded.
	 * @param bIncludeFullDescription  When true, populates Description (verbose per-property text); defaults to false.
	 * @param bIncludeComposedColors   When true, populates each entry's ComposedColors; defaults to false.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read", PythonCallable), Category = "WidgetStaticAnalysis|Description")
	static FWidgetDescriptionResult DescribeWidget(
		const UWidgetBlueprint* WidgetBlueprint,
		const UWidget* StartWidget = nullptr,
		int32 MaxDepth = -1,
		const FString& PropertyNameFilter = TEXT(""),
		bool bSkipDefaultValues = true,
		bool bIncludeFullDescription = false,
		bool bIncludeComposedColors = false);

	/**
	 * Returns the maximum depth of the widget tree. Depth: root with no children = 0; root + children = 1; etc.
	 *
	 * @return Depth as int32; -1 on error or when the tree has no root widget.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read", PythonCallable), Category = "WidgetStaticAnalysis|Description")
	static int32 GetWidgetTreeDepth(
		const UWidgetBlueprint* WidgetBlueprint,
		const UWidget* StartWidget = nullptr);

	/**
	  * Traces every statically-discoverable write to a widget property across the whole tree, merging
	  * Blueprint-graph writes with static origins (class defaults, archetype overrides, bindings).
	  * Results are grouped per (widget, property) pair.
	  *
	  * The highest-Order Applied entry is the Winner; earlier entries are Redundant, later ones Conflict.
	  * With no Applied entry, the highest-Order entry is an uncertain best guess and all are Conflict.
	 *
	 * @param WidgetBlueprint     The WBP asset.
	 * @param PropertyNameFilter  Exact match against a property name. Empty traces every property
	 *                            with at least one write or archetype override.
	 * @param WidgetNameFilter    Exact match against a widget name. Empty traces every widget.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read", PythonCallable), Category = "WidgetStaticAnalysis|PropertyTracer")
	static FWidgetPropertyOverrideResult TraceWidgetPropertyOverrides(
		const UWidgetBlueprint* WidgetBlueprint,
		const FString& PropertyNameFilter = TEXT(""),
		const FString& WidgetNameFilter = TEXT(""));

	/**
	 * Maps every connection on WidgetBlueprint (MVVM field bindings, event bindings, delegates, and
	 * legacy property bindings) into one pre-sorted checklist, Unconnected/Misconfigured rows first.
	 * Misconfigured only applies to VMFieldBinding rows: Fixable if a conversion function can bridge
	 * the types, Unbridgeable if none is allowlisted.
	 *
	 * @param WidgetBlueprint  The WBP asset.
	 * @param ScopeFilter      Case-insensitive substring match against the row's Kind name (e.g.
	 *                         "VM", "Event", "Delegate", "Legacy"). Empty matches every kind.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read", PythonCallable), Category = "WidgetStaticAnalysis|ConnectionMap")
	static FWidgetConnectionMapResult GetWidgetConnectionMap(
		const UWidgetBlueprint* WidgetBlueprint,
		const FString& ScopeFilter = TEXT(""));

	/**
	 * Reverse-searches every Widget Blueprint whose MVVM View references ViewModelClass, and returns
	 * each one's own connection map grouped per widget. Each widget's rows are narrowed to just the
	 * VMFieldBinding bindings to that exact ViewModelClass; call GetWidgetConnectionMap directly for
	 * a widget's whole map. Native (non-Blueprint) ViewModel classes never surface here, since they
	 * have no on-disk package identity for the reverse search to find.
	 *
	 * @param ViewModelClass  The ViewModel class to search for.
	 * @param ScopeFilter     Same substring-match semantics as GetWidgetConnectionMap's ScopeFilter.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read", PythonCallable), Category = "WidgetStaticAnalysis|ConnectionMap")
	static FViewModelConnectionMapResult GetViewModelConnectionMap(
		TSubclassOf<UMVVMViewModelBase> ViewModelClass,
		const FString& ScopeFilter = TEXT(""));
};
