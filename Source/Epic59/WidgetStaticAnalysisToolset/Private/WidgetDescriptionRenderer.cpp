// Copyright Epic Games, Inc. All Rights Reserved.

#include "WidgetDescriptionRenderer.h"

#include "Blueprint/WidgetTree.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Extensions/UIComponent.h"
#include "Misc/StringBuilder.h"
#include "UIComponentWidgetBlueprintExtension.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"
#include "WidgetBlueprintExtension.h"
#include "WidgetBlueprintEditorSubsystem.h"
#include "WidgetPropertyReflectionUtils.h"

namespace
{
	/**
	 * Returns an FSoftClassPath that round-trips through TSubclassOf. Native classes:
	 * /Script/UMG.TextBlock (works as-is). Blueprint classes: FSoftClassPath(GeneratedClass)
	 * produces a _C subobject path that ToolsetRegistry's FSoftObjectPath::TryLoad can't resolve
	 * -- use the Blueprint asset path instead.
	 */
	FSoftClassPath GetResolvableClassPath(const UClass* Class)
	{
		if (Class && Class->ClassGeneratedBy)
		{
			return FSoftClassPath(Class->ClassGeneratedBy->GetPathName());
		}
		return FSoftClassPath(Class);
	}

	/**
	 * True when Widget's name matches a widget-typed UPROPERTY declared (or inherited) on
	 * WidgetBlueprint's ParentClass -- i.e. this widget's identity was already declared by an
	 * ancestor class (native C++ or a base Widget Blueprint), not created fresh in this tree.
	 * Non-widget properties are ignored, so a widget that happens to be named after one (Padding,
	 * Visibility, ...) is not reported as inherited.
	 */
	bool ComputeIsInherited(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
	{
		if (!WidgetBlueprint || !Widget || Widget->GetFName().IsNone())
		{
			return false;
		}

		UClass* ParentClass = WidgetBlueprint->ParentClass;
		if (!ParentClass)
		{
			return false;
		}

		const FObjectPropertyBase* ObjectProperty = FindFProperty<FObjectPropertyBase>(ParentClass, Widget->GetFName());
		return ObjectProperty && ObjectProperty->PropertyClass && ObjectProperty->PropertyClass->IsChildOf<UWidget>();
	}

	/**
	 * WalkWidgetTree only reports a SlotName for named-slot content it descends into, so a
	 * StartWidget that is itself named-slot content needs its host looked up in the tree.
	 */
	UWidget* FindStartWidgetNamedSlotHost(const UWidgetBlueprint* WidgetBlueprint, const UWidget* StartWidget)
	{
		if (!StartWidget || !WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			return nullptr;
		}

		return FWidgetBlueprintEditorUtils::FindNamedSlotHostWidgetForContent(
			const_cast<UWidget*>(StartWidget), WidgetBlueprint->WidgetTree);
	}

	/** Name of the slot on Host (a widget, or the widget tree for slots inherited from a parent class) that holds Content, or NAME_None. */
	FName FindNamedSlotName(const UObject* Host, const UWidget* Content)
	{
		const INamedSlotInterface* SlotHost = Cast<INamedSlotInterface>(Host);
		if (!SlotHost)
		{
			return NAME_None;
		}

		TArray<FName> SlotNames;
		SlotHost->GetSlotNames(SlotNames);
		for (const FName SlotNameCandidate : SlotNames)
		{
			if (SlotHost->GetContentForSlot(SlotNameCandidate) == Content)
			{
				return SlotNameCandidate;
			}
		}
		return NAME_None;
	}

	/**
	 * UWidget::Slot's own property name -- excluded from default output: the object path it would
	 * export is redundant (parent/slot-type are already recoverable from tree position and the
	 * adjacent slot:(...) entry) and noisy. FWidgetPropertyReflectionUtils is a generic,
	 * per-plugin-consumer-agnostic reflector, so this exclusion belongs here, at DescribeWidget's
	 * own call sites, not inside the shared reflector.
	 */
	const FName SlotPropertyName(TEXT("Slot"));

	/** Read-only export: ExportText_InContainer wants a non-const Parent but never mutates it. */
	FString ExportValue(const FProperty* Property, const UObject* Container)
	{
		return FWidgetPropertyReflectionUtils::ExportPropertyValue(Property, const_cast<UObject*>(Container));
	}

	/** Appends every ShouldEmitProperty-passing property on Container as a {Name, ExportedValue} entry. */
	void PopulateProperties(TArray<FWidgetPropertyEntry>& OutProperties, const UObject* Container,
		const FString& PropertyNameFilter, bool bSkipDefaultValues)
	{
		if (!IsValid(Container))
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Container->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;

			if (Property->GetFName() == SlotPropertyName)
			{
				continue;
			}

			if (!FWidgetPropertyReflectionUtils::ShouldEmitProperty(Property, Container, PropertyNameFilter, bSkipDefaultValues))
			{
				continue;
			}

			const FString ValueStr = ExportValue(Property, Container);

			if (ValueStr.IsEmpty())
			{
				continue;
			}

			FWidgetPropertyEntry PropertyEntry;
			PropertyEntry.PropertyName = Property->GetName();
			PropertyEntry.ExportedValue = ValueStr;
			OutProperties.Add(MoveTemp(PropertyEntry));
		}
	}

	/**
	 * Keeps a multi-line value on its widget's line, so Description line [N] stays Widgets[N].
	 * Backslashes are escaped first so a literal "\n" in a value stays distinguishable from a line break.
	 */
	FString EscapeLineBreaks(const FString& Value)
	{
		return Value.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\n"), TEXT("\\n"));
	}

	/**
	 * Renders one already-populated entry (class, name, properties, slot) into the verbose Description
	 * text, so the text applies the same PropertyNameFilter/bSkipDefaultValues as the structured fields.
	 */
	void RenderWidgetLine(FStringBuilderBase& OutString, const FWidgetDescriptionEntry& Entry, FName SlotName, int32 Index)
	{
		if (!Entry.Widget)
		{
			return;
		}

		OutString.Append(FString::ChrN(Entry.Depth * 2, TEXT(' ')));

		OutString.Appendf(TEXT("[%d] %s %s"),
			Index,
			*Entry.Widget->GetClass()->GetName(),
			*Entry.WidgetName.ToString());

		for (const FWidgetPropertyEntry& Property : Entry.Properties)
		{
			OutString.Appendf(TEXT("  %s:%s"), *Property.PropertyName, *EscapeLineBreaks(Property.ExportedValue));
		}

		if (!Entry.SlotProperties.IsEmpty())
		{
			FString SlotInner;
			for (const FWidgetPropertyEntry& SlotProperty : Entry.SlotProperties)
			{
				if (!SlotInner.IsEmpty())
				{
					SlotInner.Append(TEXT(", "));
				}

				SlotInner.Appendf(TEXT("%s:%s"), *SlotProperty.PropertyName, *EscapeLineBreaks(SlotProperty.ExportedValue));
			}

			OutString.Appendf(TEXT("  slot:(%s)"), *SlotInner);
		}

		if (SlotName != NAME_None)
		{
			OutString.Appendf(TEXT("  # named-slot:%s"), *SlotName.ToString());
		}

		OutString.AppendChar(TEXT('\n'));
	}
}

FWidgetDescriptionResult FWidgetDescriptionRenderer::RenderDescription(const UWidgetBlueprint* WidgetBlueprint,
	const UWidget* StartWidget, int32 MaxDepth, const FString& PropertyNameFilter, bool bSkipDefaultValues,
	bool bIncludeFullDescription)
{
	FWidgetDescriptionResult Result;
	TStringBuilder<4096> Out;

	// AncestorAtDepth[D] is the widget most recently visited at depth D in this depth-first walk.
	// WalkWidgetTree's own callback hands us a widget's SlotName but not its named-slot host
	// object; since the walk visits a widget before any of its descendants, when the visitor sees
	// a widget at depth D with SlotName set, AncestorAtDepth[D - 1] is that widget's host.
	TArray<const UWidget*> AncestorAtDepth;
	UWidget* StartWidgetNamedSlotHost = FindStartWidgetNamedSlotHost(WidgetBlueprint, StartWidget);
	UUIComponentWidgetBlueprintExtension* UIComponentExtension =
		UWidgetBlueprintExtension::GetExtension<UUIComponentWidgetBlueprintExtension>(WidgetBlueprint);

	UWidgetBlueprintEditorSubsystem::WalkWidgetTree(
		WidgetBlueprint, StartWidget, MaxDepth,
		[&Result, &Out, WidgetBlueprint, &PropertyNameFilter, bSkipDefaultValues, bIncludeFullDescription, &AncestorAtDepth,
			StartWidgetNamedSlotHost, UIComponentExtension]
		(const UWidget* Widget, int32 Depth, FName SlotName)
		{
			if (AncestorAtDepth.Num() <= Depth)
			{
				AncestorAtDepth.SetNum(Depth + 1);
			}
			AncestorAtDepth[Depth] = Widget;

			FWidgetDescriptionEntry Entry;
			Entry.Widget = const_cast<UWidget*>(Widget);
			Entry.Parent = Widget->GetParent();
			Entry.WidgetClassPath = GetResolvableClassPath(Widget->GetClass());
			Entry.WidgetName = Widget->GetFName();
			Entry.bIsVariable = Widget->bIsVariable;
			Entry.Depth = Depth;
			if (Depth == 0)
			{
				Entry.NamedSlotHost = StartWidgetNamedSlotHost;

				// Content in a slot inherited from a parent class is hosted by the widget tree itself,
				// which has no UWidget to report as NamedSlotHost; the slot tag still applies.
				const UObject* SlotHost = StartWidgetNamedSlotHost;
				if (!SlotHost)
				{
					SlotHost = WidgetBlueprint->WidgetTree.Get();
				}
				SlotName = FindNamedSlotName(SlotHost, Widget);
			}
			else if (SlotName != NAME_None)
			{
				Entry.NamedSlotHost = const_cast<UWidget*>(AncestorAtDepth[Depth - 1]);
			}
			Entry.bInherited = ComputeIsInherited(WidgetBlueprint, Widget);

			PopulateProperties(Entry.Properties, Widget, PropertyNameFilter, bSkipDefaultValues);

			if (IsValid(Widget->Slot))
			{
				PopulateProperties(Entry.SlotProperties, Widget->Slot, PropertyNameFilter, bSkipDefaultValues);
			}

			if (UIComponentExtension)
			{
				for (UUIComponent* Component : UIComponentExtension->GetComponentsFor(Widget))
				{
					if (Component)
					{
						FWidgetUIComponentInfo ComponentInfo;
						ComponentInfo.Component = Component;
						ComponentInfo.ComponentClassPath = GetResolvableClassPath(Component->GetClass());
						Entry.UIComponents.Add(MoveTemp(ComponentInfo));
					}
				}
			}

			const int32 Index = Result.Widgets.Add(MoveTemp(Entry));

			if (bIncludeFullDescription)
			{
				RenderWidgetLine(Out, Result.Widgets[Index], SlotName, Index);
			}
		});

	if (bIncludeFullDescription)
	{
		Result.Description = FString(Out);
	}

	Result.AvailableWidgetCount = Result.Widgets.Num();

	return Result;
}
