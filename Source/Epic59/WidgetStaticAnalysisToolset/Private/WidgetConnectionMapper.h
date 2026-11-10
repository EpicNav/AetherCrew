// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Templates/SubclassOf.h"
#include "UObject/NameTypes.h"

class UWidgetBlueprint;
class UMVVMViewModelBase;

/** Plain-enum mirror of the toolset's EWidgetConnectionKind (no UHT dependency). */
enum class EMappedConnectionKind : uint8
{
	VMFieldBinding,
	EventBinding,
	Delegate,
	LegacyPropertyBinding
};

/** Mirrors EWidgetConnectionStatus. Misconfigured is only set on VMFieldBinding rows. */
enum class EMappedConnectionStatus : uint8
{
	Connected,
	Unconnected,
	Misconfigured
};

/** Mirrors EWidgetConnectionFixability. Only set when Misconfigured; Unknown means a non-type-mismatch rejection. */
enum class EMappedConnectionFixability : uint8
{
	Fixable,
	Unbridgeable,
	Unknown
};

/** One row of the unified connection map, discriminated by Kind. */
struct FMappedWidgetConnection
{
	EMappedConnectionKind Kind = EMappedConnectionKind::VMFieldBinding;

	/** Only used for EventBinding rows: "mvvm" or "blueprint_bind". */
	FString KindDetail;

	/** The bound field/event/delegate name. For VMFieldBinding rows, the ViewModel source field (last path hop). */
	FString ConnectionName;

	/** Populated for VMFieldBinding rows only. */
	FString ViewModel;

	/** The widget this connection is found on. Empty for a non-widget destination or an unbound field. */
	FString Widget;

	/** Set only when Widget is empty: the SelfContext/ViewModel-rooted handler the binding resolved to. */
	FString TargetVariable;

	/** A graph name, "MVVM View", "Blueprint Bindings", or the scope searched for an undeclared row. */
	FString DeclaredIn;

	EMappedConnectionStatus Status = EMappedConnectionStatus::Connected;

	/** Populated only when Status == Misconfigured. */
	FString Fix;
	EMappedConnectionFixability Fixability = EMappedConnectionFixability::Unknown;
};

/** One Widget Blueprint's connection map from the reverse search. WidgetPath is its soft object path. */
struct FMappedViewModelWidgetConnections
{
	FString WidgetPath;
	TArray<FMappedWidgetConnection> Connections;
};

/** Maps every connection on a WidgetBlueprint into one unified row shape. */
class FWidgetConnectionMapper
{
public:
	/** WidgetBlueprint's connections, unsorted. Empty for a null WidgetBlueprint. */
	[[nodiscard]] static TArray<FMappedWidgetConnection> MapConnections(const UWidgetBlueprint* WidgetBlueprint);

	/** Finds Widget Blueprints referencing ViewModelClass (via the asset registry) with their VMFieldBinding rows
	 *  for it. Native ViewModel classes never surface (no on-disk package). */
	[[nodiscard]] static TArray<FMappedViewModelWidgetConnections> MapViewModelConnections(TSubclassOf<UMVVMViewModelBase> ViewModelClass);
};
