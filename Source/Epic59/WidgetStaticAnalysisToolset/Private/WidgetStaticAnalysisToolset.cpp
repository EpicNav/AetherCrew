// Copyright Epic Games, Inc. All Rights Reserved.

#include "WidgetStaticAnalysisToolset.h"

#include "Blueprint/WidgetTree.h"
#include "Kismet/KismetSystemLibrary.h"
#include "MVVMViewModelBase.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintOperationUtils.h" // UE 5.8: 5.9 moved these helpers to UWidgetBlueprintEditorSubsystem
#include "WidgetComposedColorResolver.h"
#include "WidgetConnectionMapper.h"
#include "WidgetDescriptionRenderer.h"
#include "WidgetPropertyOriginScanner.h"
#include "WidgetPropertyTracer.h"

DEFINE_LOG_CATEGORY_STATIC(LogWidgetStaticAnalysisToolset, Log, All);

FWidgetDescriptionResult UWidgetStaticAnalysisToolset::DescribeWidget(const UWidgetBlueprint* WidgetBlueprint,
	const UWidget* StartWidget, int32 MaxDepth, const FString& PropertyNameFilter, bool bSkipDefaultValues,
	bool bIncludeFullDescription, bool bIncludeComposedColors)
{
	if (!WidgetBlueprint)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("DescribeWidget: WidgetBlueprint is null."));
		return {};
	}

	if (!WidgetBlueprint->WidgetTree)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("DescribeWidget: WidgetBlueprint has no widget tree."));
		return {};
	}

	if (StartWidget && !StartWidget->IsIn(WidgetBlueprint->WidgetTree.Get()))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("DescribeWidget: StartWidget does not belong to the given WidgetBlueprint."));
		return {};
	}

	if (MaxDepth < -1)
	{
		UE_LOGF(LogWidgetStaticAnalysisToolset, Warning, "DescribeWidget: MaxDepth %d clamped to -1 (no limit).", MaxDepth);
		MaxDepth = -1;
	}

	FWidgetDescriptionResult Result = FWidgetDescriptionRenderer::RenderDescription(WidgetBlueprint, StartWidget, MaxDepth,
		PropertyNameFilter, bSkipDefaultValues, bIncludeFullDescription);

	if (bIncludeComposedColors)
	{
		const FWidgetComposedColorResolverContext ColorContext = FWidgetComposedColorResolver::MakeContext(WidgetBlueprint);
		for (FWidgetDescriptionEntry& Entry : Result.Widgets)
		{
			Entry.ComposedColors = FWidgetComposedColorResolver::Resolve(Entry.Widget, ColorContext);
		}
	}

	return Result;
}

int32 UWidgetStaticAnalysisToolset::GetWidgetTreeDepth(const UWidgetBlueprint* WidgetBlueprint, const UWidget* StartWidget)
{
	if (!WidgetBlueprint)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetWidgetTreeDepth: WidgetBlueprint is null."));
		return -1;
	}

	if (!WidgetBlueprint->WidgetTree)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetWidgetTreeDepth: WidgetBlueprint has no widget tree."));
		return -1;
	}

	if (StartWidget && !StartWidget->IsIn(WidgetBlueprint->WidgetTree.Get()))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetWidgetTreeDepth: StartWidget does not belong to the given WidgetBlueprint."));
		return -1;
	}

	// ComputeWidgetTreeDepth returns 0 for an empty tree, which reads the same as a lone root.
	if (!StartWidget && !WidgetBlueprint->WidgetTree->RootWidget)
	{
		return -1;
	}

	return FWidgetBlueprintOperationUtils::ComputeWidgetTreeDepth(WidgetBlueprint, StartWidget); // UE 5.8: 5.8 name of the 5.9 helper
}

namespace
{
	// Classifies a write by how early in the widget lifecycle it's reachable.
	EWidgetPropertyAssignmentSource ClassifyAssignmentSource(const FTracedPropertyWrite& Write)
	{
		if (Write.bPreConstructReachable)
		{
			return EWidgetPropertyAssignmentSource::PreConstructSetter;
		}
		return Write.bLifecycleReachable
			? EWidgetPropertyAssignmentSource::ConstructSetter
			: EWidgetPropertyAssignmentSource::RuntimeHandler;
	}

	// Order is only comparable within one property's group.
	FWidgetPropertyOverrideEntry TranslateWrite(const FTracedPropertyWrite& Write, const FString& PropertyName, int32 Order)
	{
		FWidgetPropertyOverrideEntry Entry;
		Entry.WidgetName = Write.WidgetName;
		Entry.PropertyName = PropertyName;
		Entry.Order = Order;
		Entry.Source = ClassifyAssignmentSource(Write);
		Entry.SourceDetail = Write.FunctionName.IsNone()
			? FString::Printf(TEXT("Widget=%s; VariableSet"), *Write.WidgetName.ToString())
			: FString::Printf(TEXT("Widget=%s; CallFunction:%s"), *Write.WidgetName.ToString(), *Write.FunctionName.ToString());
		Entry.ExportedValue = Write.ExportedValue;
		// Branch-gated writes are unconfirmed.
		Entry.ApplicationStatus = Write.bBranchGated
			? EWidgetPropertyApplicationStatus::DeclaredButUnconfirmed
			: EWidgetPropertyApplicationStatus::Applied;
		return Entry;
	}

	// As TranslateWrite, for a static/declarative origin.
	FWidgetPropertyOverrideEntry TranslateOrigin(const FTracedPropertyOrigin& Origin, const FString& PropertyName, int32 Order)
	{
		static_assert(static_cast<uint8>(EOriginSourceKind::NativeClassDefault) == static_cast<uint8>(EWidgetPropertyAssignmentSource::NativeClassDefault)
			&& static_cast<uint8>(EOriginSourceKind::GeneratedClassDefault) == static_cast<uint8>(EWidgetPropertyAssignmentSource::GeneratedClassDefault)
			&& static_cast<uint8>(EOriginSourceKind::WidgetTreeArchetype) == static_cast<uint8>(EWidgetPropertyAssignmentSource::WidgetTreeArchetype)
			&& static_cast<uint8>(EOriginSourceKind::NamedSlotArchetype) == static_cast<uint8>(EWidgetPropertyAssignmentSource::NamedSlotArchetype)
			&& static_cast<uint8>(EOriginSourceKind::VariantResolver) == static_cast<uint8>(EWidgetPropertyAssignmentSource::VariantResolver)
			&& static_cast<uint8>(EOriginSourceKind::LegacyBinding) == static_cast<uint8>(EWidgetPropertyAssignmentSource::LegacyBinding)
			&& static_cast<uint8>(EOriginSourceKind::MVVMBinding) == static_cast<uint8>(EWidgetPropertyAssignmentSource::MVVMBinding),
			"EOriginSourceKind must mirror EWidgetPropertyAssignmentSource's first seven enumerators 1:1 for this static_cast to stay value-correct.");
		static_assert(static_cast<uint8>(EOriginApplicationStatus::Applied) == static_cast<uint8>(EWidgetPropertyApplicationStatus::Applied)
			&& static_cast<uint8>(EOriginApplicationStatus::DeclaredButUnconfirmed) == static_cast<uint8>(EWidgetPropertyApplicationStatus::DeclaredButUnconfirmed)
			&& static_cast<uint8>(EOriginApplicationStatus::KnownNotApplied) == static_cast<uint8>(EWidgetPropertyApplicationStatus::KnownNotApplied),
			"EOriginApplicationStatus must mirror EWidgetPropertyApplicationStatus 1:1 for this static_cast to stay value-correct.");

		FWidgetPropertyOverrideEntry Entry;
		Entry.WidgetName = Origin.WidgetName;
		Entry.PropertyName = PropertyName;
		Entry.Order = Order;
		Entry.Source = static_cast<EWidgetPropertyAssignmentSource>(Origin.Kind);
		Entry.SourceDetail = Origin.SourceDetail;
		Entry.ExportedValue = Origin.ExportedValue;
		Entry.ApplicationStatus = static_cast<EWidgetPropertyApplicationStatus>(Origin.ApplicationStatus);
		return Entry;
	}

	// Only Blueprint-graph writes get a "remove this write" hint.
	FString BuildRedundantRecommendation(const FWidgetPropertyOverrideEntry& OtherEntry, const FWidgetPropertyOverrideEntry& WinnerEntry)
	{
		if (OtherEntry.Source == EWidgetPropertyAssignmentSource::PreConstructSetter
			|| OtherEntry.Source == EWidgetPropertyAssignmentSource::ConstructSetter
			|| OtherEntry.Source == EWidgetPropertyAssignmentSource::RuntimeHandler)
		{
			return FString::Printf(
				TEXT("Remove this write. It is always overwritten afterward by the unconditionally-applied write at Order %d (%s)."),
				WinnerEntry.Order, *WinnerEntry.SourceDetail);
		}
		return FString::Printf(
			TEXT("No action needed here. This value is always overwritten afterward by the unconditionally-applied write at Order %d (%s), which is expected, not a bug."),
			WinnerEntry.Order, *WinnerEntry.SourceDetail);
	}

	// bCertainWinnerAbove is false when the winner is only an uncertain guess.
	FString BuildConflictRecommendation(const FWidgetPropertyOverrideEntry& ConflictEntry, bool bCertainWinnerAbove)
	{
		if (ConflictEntry.ApplicationStatus == EWidgetPropertyApplicationStatus::KnownNotApplied)
		{
			return TEXT("No action needed. This write is statically known to never apply (e.g. a disabled MVVM binding), so it cannot override the winner above.");
		}

		const TCHAR* WinnerPhrase = bCertainWinnerAbove
			? TEXT("otherwise-certain winner above")
			: TEXT("uncertain best-guess winner above (neither is statically guaranteed to be this property's final value)");

		switch (ConflictEntry.Source)
		{
		case EWidgetPropertyAssignmentSource::VariantResolver:
			return FString::Printf(TEXT("Confirm which class this widget's IUserWidgetVariantResolver actually returns at runtime. If it returns this variant, its own class default silently overrides the %s."), WinnerPhrase);
		case EWidgetPropertyAssignmentSource::MVVMBinding:
			return FString::Printf(TEXT("Confirm whether this MVVM binding's source ViewModel field is actually populated and pushes a value at runtime. If so, it silently overrides the %s."), WinnerPhrase);
		default:
			return FString::Printf(TEXT("Confirm whether this branch-gated write's condition evaluates true at runtime. If so, it silently overrides the %s."), WinnerPhrase);
		}
	}

	// Picks the Winner and builds Redundant/Conflict flags for one property's Order-sorted entries.
	void BuildWinnerAndFlagsForProperty(const FString& PropertyName, const TArray<FWidgetPropertyOverrideEntry>& PropertyEntries,
		TArray<FWidgetPropertyWinner>& OutWinners, TArray<FWidgetPropertyOverrideFlag>& OutFlags)
	{
		if (PropertyEntries.Num() == 0)
		{
			return;
		}

		const FName WidgetName = PropertyEntries[0].WidgetName;

		int32 WinnerIndex = INDEX_NONE;
		for (int32 Index = 0; Index < PropertyEntries.Num(); ++Index)
		{
			if (PropertyEntries[Index].ApplicationStatus == EWidgetPropertyApplicationStatus::Applied)
			{
				WinnerIndex = Index;
			}
		}

		FWidgetPropertyWinner Winner;
		Winner.WidgetName = WidgetName;
		Winner.PropertyName = PropertyName;

		if (WinnerIndex != INDEX_NONE)
		{
			const FWidgetPropertyOverrideEntry& WinnerEntry = PropertyEntries[WinnerIndex];
			Winner.WinningSource = WinnerEntry.Source;
			Winner.WinningSourceDetail = WinnerEntry.SourceDetail;
			Winner.EffectiveValue = WinnerEntry.ExportedValue;
			Winner.bCertain = true;
			Winner.Rationale = TEXT("Highest-order unconditionally-applied write found for this property.");

			for (int32 Index = 0; Index < PropertyEntries.Num(); ++Index)
			{
				if (Index == WinnerIndex)
				{
					continue;
				}

				const FWidgetPropertyOverrideEntry& OtherEntry = PropertyEntries[Index];
				FWidgetPropertyOverrideFlag Flag;
				Flag.WidgetName = WidgetName;
				Flag.PropertyName = PropertyName;
				Flag.Location = OtherEntry.SourceDetail;
				Flag.ValueSet = OtherEntry.ExportedValue;

				if (Index < WinnerIndex)
				{
					Flag.Kind = EWidgetPropertyOverrideFlagKind::Redundant;
					Flag.Recommendation = BuildRedundantRecommendation(OtherEntry, WinnerEntry);
				}
				else
				{
					Flag.Kind = EWidgetPropertyOverrideFlagKind::Conflict;
					Flag.Recommendation = BuildConflictRecommendation(OtherEntry, /*bCertainWinnerAbove=*/true);
				}

				OutFlags.Add(MoveTemp(Flag));
			}
		}
		else
		{
			const int32 BestGuessIndex = PropertyEntries.Num() - 1;
			const FWidgetPropertyOverrideEntry& BestGuess = PropertyEntries[BestGuessIndex];
			Winner.WinningSource = BestGuess.Source;
			Winner.WinningSourceDetail = BestGuess.SourceDetail;
			Winner.EffectiveValue = BestGuess.ExportedValue;
			Winner.bCertain = false;
			Winner.Rationale = TEXT("No unconditionally-applied write found for this property; reporting the last-declared candidate as an uncertain best guess.");

			if (PropertyEntries.Num() > 1)
			{
				// Excluded here since it cannot conflict with itself.
				for (int32 Index = 0; Index < PropertyEntries.Num(); ++Index)
				{
					if (Index == BestGuessIndex)
					{
						continue;
					}

					const FWidgetPropertyOverrideEntry& Entry = PropertyEntries[Index];
					FWidgetPropertyOverrideFlag Flag;
					Flag.Kind = EWidgetPropertyOverrideFlagKind::Conflict;
					Flag.WidgetName = WidgetName;
					Flag.PropertyName = PropertyName;
					Flag.Location = Entry.SourceDetail;
					Flag.ValueSet = Entry.ExportedValue;
					Flag.Recommendation = BuildConflictRecommendation(Entry, /*bCertainWinnerAbove=*/false);
					OutFlags.Add(MoveTemp(Flag));
				}
			}
		}

		OutWinners.Add(MoveTemp(Winner));
	}
}

FWidgetPropertyOverrideResult UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(
	const UWidgetBlueprint* WidgetBlueprint, const FString& PropertyNameFilter, const FString& WidgetNameFilter)
{
	if (!WidgetBlueprint)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("TraceWidgetPropertyOverrides: WidgetBlueprint is null."));
		return {};
	}

	if (!WidgetBlueprint->WidgetTree)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("TraceWidgetPropertyOverrides: WidgetBlueprint has no widget tree."));
		return {};
	}

	const TArray<FTracedPropertyWrite> Writes = FWidgetPropertyTracer::TraceWrites(WidgetBlueprint);

	// (Widget, Property) pairs already covered by a graph write; the origin scanner skips these.
	TSet<TPair<FName, FName>> GraphCoveredWidgetProperties;
	for (const FTracedPropertyWrite& Write : Writes)
	{
		if (!Write.PropertyName.IsNone())
		{
			GraphCoveredWidgetProperties.Add(TPair<FName, FName>(Write.WidgetName, Write.PropertyName));
		}
	}
	const TArray<FTracedPropertyOrigin> Origins = FWidgetPropertyOriginScanner::ScanOrigins(WidgetBlueprint, GraphCoveredWidgetProperties, PropertyNameFilter);

	// Group per (WidgetName, PropertyName); writes with no resolved property are skipped.
	using FWidgetPropertyKey = TPair<FName, FName>;
	TArray<FWidgetPropertyKey> OrderedGroups;
	TMap<FWidgetPropertyKey, TArray<const FTracedPropertyOrigin*>> OriginsByGroup;
	TMap<FWidgetPropertyKey, TArray<const FTracedPropertyWrite*>> WritesByGroup;

	auto RegisterGroup = [&OrderedGroups, &OriginsByGroup, &WritesByGroup](const FWidgetPropertyKey& Key)
	{
		if (!OriginsByGroup.Contains(Key) && !WritesByGroup.Contains(Key))
		{
			OrderedGroups.Add(Key);
		}
	};

	const auto PassesWidgetNameFilter = [&WidgetNameFilter](FName WidgetName)
	{
		return WidgetNameFilter.IsEmpty() || WidgetName.ToString() == WidgetNameFilter;
	};

	for (const FTracedPropertyOrigin& Origin : Origins)
	{
		if (!PassesWidgetNameFilter(Origin.WidgetName))
		{
			continue;
		}

		const FWidgetPropertyKey Key(Origin.WidgetName, Origin.PropertyName);
		RegisterGroup(Key);
		OriginsByGroup.FindOrAdd(Key).Add(&Origin);
	}

	for (const FTracedPropertyWrite& Write : Writes)
	{
		if (Write.PropertyName.IsNone())
		{
			continue;
		}

		const FString PropertyNameStr = Write.PropertyName.ToString();
		if ((!PropertyNameFilter.IsEmpty() && PropertyNameStr != PropertyNameFilter) || !PassesWidgetNameFilter(Write.WidgetName))
		{
			continue;
		}

		const FWidgetPropertyKey Key(Write.WidgetName, Write.PropertyName);
		RegisterGroup(Key);
		WritesByGroup.FindOrAdd(Key).Add(&Write);
	}

	FWidgetPropertyOverrideResult Result;
	Result.AvailablePropertyCount = OrderedGroups.Num();

	for (const FWidgetPropertyKey& Key : OrderedGroups)
	{
		const FString PropertyName = Key.Value.ToString();
		const TArray<const FTracedPropertyOrigin*>* PropertyOrigins = OriginsByGroup.Find(Key);
		const TArray<const FTracedPropertyWrite*>* PropertyWrites = WritesByGroup.Find(Key);

		bool bHasMvvmOrigin = false;
		if (PropertyOrigins)
		{
			for (const FTracedPropertyOrigin* Origin : *PropertyOrigins)
			{
				if (Origin->Kind == EOriginSourceKind::MVVMBinding)
				{
					bHasMvvmOrigin = true;
					break;
				}
			}
		}

		TArray<FWidgetPropertyOverrideEntry> PropertyEntries;
		PropertyEntries.Reserve((PropertyOrigins ? PropertyOrigins->Num() : 0) + (PropertyWrites ? PropertyWrites->Num() : 0));

		if (bHasMvvmOrigin)
		{
			// MVVM bindings apply after Construct: order them between the ConstructSetter and RuntimeHandler writes.
			TArray<const FTracedPropertyWrite*> WritesBeforeVmBindingStage;
			TArray<const FTracedPropertyWrite*> WritesAfterVmBindingStage;
			if (PropertyWrites)
			{
				for (const FTracedPropertyWrite* Write : *PropertyWrites)
				{
					if (ClassifyAssignmentSource(*Write) == EWidgetPropertyAssignmentSource::RuntimeHandler)
					{
						WritesAfterVmBindingStage.Add(Write);
					}
					else
					{
						WritesBeforeVmBindingStage.Add(Write);
					}
				}
			}

			for (const FTracedPropertyOrigin* Origin : *PropertyOrigins)
			{
				if (Origin->Kind != EOriginSourceKind::MVVMBinding)
				{
					PropertyEntries.Add(TranslateOrigin(*Origin, PropertyName, PropertyEntries.Num()));
				}
			}
			for (const FTracedPropertyWrite* Write : WritesBeforeVmBindingStage)
			{
				PropertyEntries.Add(TranslateWrite(*Write, PropertyName, PropertyEntries.Num()));
			}
			for (const FTracedPropertyOrigin* Origin : *PropertyOrigins)
			{
				if (Origin->Kind == EOriginSourceKind::MVVMBinding)
				{
					PropertyEntries.Add(TranslateOrigin(*Origin, PropertyName, PropertyEntries.Num()));
				}
			}
			for (const FTracedPropertyWrite* Write : WritesAfterVmBindingStage)
			{
				PropertyEntries.Add(TranslateWrite(*Write, PropertyName, PropertyEntries.Num()));
			}
		}
		else
		{
			// No MVVM binding: static origins precede the writes.
			if (PropertyOrigins)
			{
				for (const FTracedPropertyOrigin* Origin : *PropertyOrigins)
				{
					PropertyEntries.Add(TranslateOrigin(*Origin, PropertyName, PropertyEntries.Num()));
				}
			}
			if (PropertyWrites)
			{
				for (const FTracedPropertyWrite* Write : *PropertyWrites)
				{
					PropertyEntries.Add(TranslateWrite(*Write, PropertyName, PropertyEntries.Num()));
				}
			}
		}

		Result.Chains.Append(PropertyEntries);

		BuildWinnerAndFlagsForProperty(PropertyName, PropertyEntries, Result.Winners, Result.Flags);
	}

	return Result;
}

namespace
{
	// Translates FMappedWidgetConnection to the UHT-visible entry shape (enums mirror 1:1).
	FWidgetConnectionEntry TranslateMappedConnection(const FMappedWidgetConnection& InConnection)
	{
		static_assert(static_cast<uint8>(EMappedConnectionKind::VMFieldBinding) == static_cast<uint8>(EWidgetConnectionKind::VMFieldBinding)
			&& static_cast<uint8>(EMappedConnectionKind::EventBinding) == static_cast<uint8>(EWidgetConnectionKind::EventBinding)
			&& static_cast<uint8>(EMappedConnectionKind::Delegate) == static_cast<uint8>(EWidgetConnectionKind::Delegate)
			&& static_cast<uint8>(EMappedConnectionKind::LegacyPropertyBinding) == static_cast<uint8>(EWidgetConnectionKind::LegacyPropertyBinding),
			"EMappedConnectionKind must mirror EWidgetConnectionKind 1:1 for this static_cast to stay value-correct.");
		static_assert(static_cast<uint8>(EMappedConnectionStatus::Connected) == static_cast<uint8>(EWidgetConnectionStatus::Connected)
			&& static_cast<uint8>(EMappedConnectionStatus::Unconnected) == static_cast<uint8>(EWidgetConnectionStatus::Unconnected)
			&& static_cast<uint8>(EMappedConnectionStatus::Misconfigured) == static_cast<uint8>(EWidgetConnectionStatus::Misconfigured),
			"EMappedConnectionStatus must mirror EWidgetConnectionStatus 1:1 for this static_cast to stay value-correct.");
		static_assert(static_cast<uint8>(EMappedConnectionFixability::Fixable) == static_cast<uint8>(EWidgetConnectionFixability::Fixable)
			&& static_cast<uint8>(EMappedConnectionFixability::Unbridgeable) == static_cast<uint8>(EWidgetConnectionFixability::Unbridgeable)
			&& static_cast<uint8>(EMappedConnectionFixability::Unknown) == static_cast<uint8>(EWidgetConnectionFixability::Unknown),
			"EMappedConnectionFixability must mirror EWidgetConnectionFixability 1:1 for this static_cast to stay value-correct.");

		FWidgetConnectionEntry OutConnection;
		OutConnection.Kind = static_cast<EWidgetConnectionKind>(InConnection.Kind);
		OutConnection.KindDetail = InConnection.KindDetail;
		OutConnection.ConnectionName = InConnection.ConnectionName;
		OutConnection.ViewModel = InConnection.ViewModel;
		OutConnection.Widget = InConnection.Widget;
		OutConnection.TargetVariable = InConnection.TargetVariable;
		OutConnection.DeclaredIn = InConnection.DeclaredIn;
		OutConnection.Status = static_cast<EWidgetConnectionStatus>(InConnection.Status);
		OutConnection.Fix = InConnection.Fix;
		OutConnection.Fixability = static_cast<EWidgetConnectionFixability>(InConnection.Fixability);
		return OutConnection;
	}

	// Translates, filters, tallies and sorts; returns the matching row count.
	int32 BuildConnectionEntries(const TArray<FMappedWidgetConnection>& InConnections, const FString& ScopeFilter,
		TArray<FWidgetConnectionEntry>& OutEntries, int32& OutConnectedCount, int32& OutUnconnectedCount, int32& OutMisconfiguredCount)
	{
		const UEnum* KindEnum = StaticEnum<EWidgetConnectionKind>();

		OutEntries.Reserve(InConnections.Num());
		for (const FMappedWidgetConnection& InConnection : InConnections)
		{
			FWidgetConnectionEntry Entry = TranslateMappedConnection(InConnection);

			// Case-insensitive substring match on the Kind name; empty matches all.
			if (!ScopeFilter.IsEmpty())
			{
				const FString KindName = KindEnum ? KindEnum->GetNameStringByValue(static_cast<int64>(Entry.Kind)) : FString();
				if (!KindName.Contains(ScopeFilter))
				{
					continue;
				}
			}

			switch (Entry.Status)
			{
			case EWidgetConnectionStatus::Connected:
				++OutConnectedCount;
				break;
			case EWidgetConnectionStatus::Unconnected:
				++OutUnconnectedCount;
				break;
			case EWidgetConnectionStatus::Misconfigured:
				++OutMisconfiguredCount;
				break;
			}

			OutEntries.Add(MoveTemp(Entry));
		}

		// Unconnected/Misconfigured rows first.
		OutEntries.StableSort([](const FWidgetConnectionEntry& A, const FWidgetConnectionEntry& B)
		{
			const bool bABroken = A.Status != EWidgetConnectionStatus::Connected;
			const bool bBBroken = B.Status != EWidgetConnectionStatus::Connected;
			return bABroken && !bBBroken;
		});
		return OutEntries.Num();
	}
}

FWidgetConnectionMapResult UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(const UWidgetBlueprint* WidgetBlueprint, const FString& ScopeFilter)
{
	if (!WidgetBlueprint)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetWidgetConnectionMap: WidgetBlueprint is null."));
		return {};
	}

	const TArray<FMappedWidgetConnection> Mapped = FWidgetConnectionMapper::MapConnections(WidgetBlueprint);

	FWidgetConnectionMapResult Result;
	Result.AvailableConnectionCount = BuildConnectionEntries(Mapped, ScopeFilter, Result.Connections,
		Result.ConnectedCount, Result.UnconnectedCount, Result.MisconfiguredCount);

	return Result;
}

FViewModelConnectionMapResult UWidgetStaticAnalysisToolset::GetViewModelConnectionMap(TSubclassOf<UMVVMViewModelBase> ViewModelClass, const FString& ScopeFilter)
{
	if (!ViewModelClass)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetViewModelConnectionMap: ViewModelClass is null."));
		return {};
	}

	const TArray<FMappedViewModelWidgetConnections> Mapped = FWidgetConnectionMapper::MapViewModelConnections(ViewModelClass);

	FViewModelConnectionMapResult Result;
	Result.AvailableWidgetCount = Mapped.Num();

	for (const FMappedViewModelWidgetConnections& MappedWidget : Mapped)
	{
		FViewModelWidgetConnections WidgetConnections;
		WidgetConnections.WidgetPath = MappedWidget.WidgetPath;

		Result.AvailableConnectionCount += BuildConnectionEntries(MappedWidget.Connections, ScopeFilter, WidgetConnections.Connections,
			WidgetConnections.ConnectedCount, WidgetConnections.UnconnectedCount, WidgetConnections.MisconfiguredCount);

		Result.Widgets.Add(MoveTemp(WidgetConnections));
	}

	return Result;
}
