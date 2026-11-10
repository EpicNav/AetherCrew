// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Containers/Array.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Templates/Tuple.h"
#include "UObject/NameTypes.h"

class UWidgetBlueprint;

/** Origin bucket; order matches EWidgetPropertyAssignmentSource's first seven values. */
enum class EOriginSourceKind : uint8
{
	NativeClassDefault,
	GeneratedClassDefault,
	WidgetTreeArchetype,
	NamedSlotArchetype,
	VariantResolver,
	LegacyBinding,
	MVVMBinding,
};

/** Mirrors EWidgetPropertyApplicationStatus 1:1. */
enum class EOriginApplicationStatus : uint8
{
	Applied,
	DeclaredButUnconfirmed,
	KnownNotApplied,
};

/** One static-or-declarative write location found by FWidgetPropertyOriginScanner. */
struct FTracedPropertyOrigin
{
	FName WidgetName;
	FName PropertyName;
	EOriginSourceKind Kind = EOriginSourceKind::NativeClassDefault;
	EOriginApplicationStatus ApplicationStatus = EOriginApplicationStatus::Applied;

	/** Free-text disambiguation shown to the caller. */
	FString SourceDetail;

	/** Empty for LegacyBinding/MVVMBinding, whose value is computed at runtime rather than statically known. */
	FString ExportedValue;
};

/**
 * Finds static/declarative property origins: class defaults, archetype overrides, variant resolver
 * defaults, legacy bindings and MVVM bindings. Skips pairs already covered by a graph write
 * (except MVVMBinding). With no name filter, only archetype values differing from the class default
 * are reported.
 */
class FWidgetPropertyOriginScanner
{
public:
	static TArray<FTracedPropertyOrigin> ScanOrigins(const UWidgetBlueprint* WidgetBlueprint,
		const TSet<TPair<FName, FName>>& GraphCoveredWidgetProperties, const FString& PropertyNameFilter);
};
