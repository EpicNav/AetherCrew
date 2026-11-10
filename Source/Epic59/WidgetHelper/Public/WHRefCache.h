// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Map.h"
#include "Containers/StringView.h"
#include "CoreTypes.h"
#include "Misc/CoreMiscDefines.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

class SWidget;

namespace UE::WidgetHelper
{
	/**
	 * Process-wide, game-thread-only map between live widgets and short refs, the contract that lets one plugin
	 * locate a widget and another act on it. A ref is FName(Prefix, N), printed "q_12": every ref shares its
	 * prefix's name entry, so minting never grows the name table. Refs are append-only and never reused, so a
	 * stale ref resolves to nothing rather than to another widget. Holds widgets weakly.
	 */
	class FWidgetRefCache
	{
	public:
		static WIDGETHELPER_API FWidgetRefCache& Get();

		/** The widget's existing ref, else the next one for Prefix (eg: "q" mints q_1, q_2...). */
		WIDGETHELPER_API FName GetOrAssignRef(const TSharedRef<SWidget>& Widget, FName Prefix);

		/** The live widget for Ref, or null when the ref is unknown or its widget is gone. */
		WIDGETHELPER_API TSharedPtr<SWidget> ResolveRef(FName Ref) const;

		/** Text overload for refs from outside: never adds Ref to the name table when it is unknown. */
		WIDGETHELPER_API TSharedPtr<SWidget> ResolveRef(FStringView Ref) const;

		/** The widget's ref without minting one; NAME_None when it has none. */
		WIDGETHELPER_API FName FindRef(const TSharedRef<SWidget>& Widget) const;

		/** Drops entries whose widget is gone. Also runs on its own as the cache grows. */
		WIDGETHELPER_API void PurgeExpired();

	private:
		FWidgetRefCache() = default;
		UE_NONCOPYABLE(FWidgetRefCache)

		TMap<TWeakPtr<SWidget>, FName> WidgetToRef;
		TMap<FName, TWeakPtr<SWidget>> RefToWidget;
		TMap<FName, int32> PrefixCounters;

		/** WidgetToRef size at which the next mint purges first, so the maps stay bounded without a ticker. */
		int32 NextPurgeSize = 0;
	};
}
