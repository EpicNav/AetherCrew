// Copyright Epic Games, Inc. All Rights Reserved.

#include "SlateInspectorToolset59.h"

#include "SlateInspectorToolsetInputScope.h"
#include "SlateInspectorToolsetObserverManager.h"
#include "SlateInspectorToolsetSnapshotRenderer.h"
#include "SlateInspectorToolsetUMGResolver.h"
#include "SlateInspectorToolsetWidgetTree.h"
#include "WHRefCache.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/IConsoleManager.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Layout/Clipping.h"
#include "Layout/WidgetPath.h"
#include "Logging/StructuredLog.h"
#include "Misc/App.h"
#include "Subsystems/SubsystemCollection.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogSlateInspectorToolset, Log, All);

static bool bEnableSlateInspectorToolset = true;

static void OnEnableSlateInspectorToolsetChanged(IConsoleVariable* Variable)
{
	if (GEditor == nullptr)
	{
		return;
	}

	if (USlateInspectorToolset59* Toolset = GEditor->GetEditorSubsystem<USlateInspectorToolset59>())
	{
		Toolset->SetToolsetEnabled(Variable->GetBool());
	}
}

static FAutoConsoleVariableRef CVarEnableSlateInspectorToolset(
	TEXT("SlateInspectorToolset59.Enable"),
	bEnableSlateInspectorToolset,
	TEXT("Enable or disable SlateInspectorToolset registration. When disabled, its tools will not appear in the MCP tool list."),
	FConsoleVariableDelegate::CreateStatic(&OnEnableSlateInspectorToolsetChanged),
	ECVF_Default);

void USlateInspectorToolset59::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UToolsetRegistrySubsystem>();
	SetToolsetEnabled(bEnableSlateInspectorToolset);

	if (FSlateApplication::IsInitialized())
	{
		PostTickDelegateHandle = FSlateApplication::Get().OnPostTick().AddUObject(this, &USlateInspectorToolset59::OnSlatePostTick);
	}

	// Create a shallow default root observer (depth 0: window names only).
	// Use Observe() on specific windows or panels to drill deeper.
	RootObserverIdentifier = FSlateInspectorToolsetObserverManager::Get().AddObserver(nullptr, 0);

	EndPIEDelegateHandle = UE::Editor::PIE::OnEnd.AddUObject(this, &USlateInspectorToolset59::OnEndPIE);
}

void USlateInspectorToolset59::Deinitialize()
{
	// Anything still held would outlive the tools that could release it.
	ReleaseHeldInput();

	UE::Editor::PIE::OnEnd.Remove(EndPIEDelegateHandle);
	EndPIEDelegateHandle.Reset();

	if (!RootObserverIdentifier.IsEmpty())
	{
		FSlateInspectorToolsetObserverManager::Get().RemoveObserver(RootObserverIdentifier);
		RootObserverIdentifier.Empty();
	}

	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnPostTick().Remove(PostTickDelegateHandle);
	}

	SetToolsetEnabled(false);
	Super::Deinitialize();
}

void USlateInspectorToolset59::OnSlatePostTick(float DeltaTime)
{
	FSlateInspectorToolsetObserverManager::Get().Tick(DeltaTime);
}

void USlateInspectorToolset59::OnEndPIE(bool bIsSimulating)
{
	// A button or finger still held on a game widget would otherwise keep the mouse capture the widget's destruction orphans.
	ReleaseHeldInput();

	// PIE teardown destroys game UMG widgets; purge their now-expired refs so a
	// subsequent ResolveRef fails immediately instead of after the next periodic purge.
	UE::WidgetHelper::FWidgetRefCache::Get().PurgeExpired();
}

void USlateInspectorToolset59::SetToolsetEnabled(bool bEnabled)
{
	// RegisterToolsetClass returns void and can bail without registering, so assuming success
	// here would latch the flag on and stop the guard below ever retrying.
	if (bEnabled && !bToolsetRegistered)
	{
		UToolsetRegistry::RegisterToolsetClass(USlateInspectorToolset59::StaticClass());
		bToolsetRegistered = UToolsetRegistry::IsToolsetClassRegistered(
			USlateInspectorToolset59::StaticClass());
	}
	else if (!bEnabled && bToolsetRegistered)
	{
		UToolsetRegistry::UnregisterToolsetClass(USlateInspectorToolset59::StaticClass());
		bToolsetRegistered = UToolsetRegistry::IsToolsetClassRegistered(
			USlateInspectorToolset59::StaticClass());
	}
}

namespace UE::SlateInspectorToolset::Private
{

/** Escape a string for safe embedding in a JSON string value (RFC 8259). */
FString EscapeJsonString(const FString& Input)
{
	FString Result;
	Result.Reserve(Input.Len() + 16);
	for (TCHAR Character : Input)
	{
		switch (Character)
		{
		case TEXT('"'):  Result += TEXT("\\\""); break;
		case TEXT('\\'): Result += TEXT("\\\\"); break;
		case TEXT('\b'): Result += TEXT("\\b");  break;
		case TEXT('\f'): Result += TEXT("\\f");  break;
		case TEXT('\n'): Result += TEXT("\\n");  break;
		case TEXT('\r'): Result += TEXT("\\r");  break;
		case TEXT('\t'): Result += TEXT("\\t");  break;
		default:
			// Escape remaining ASCII control characters (U+0000..U+001F) per RFC 8259.
			if (Character < 0x20)
			{
				Result += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Character));
			}
			else
			{
				Result += Character;
			}
			break;
		}
	}
	return Result;
}

/** Fail a tool call: the reason reaches a script or tool caller as a script error, and the log as a warning. */
void RaiseToolError(const FString& Message)
{
	UE_LOGFMT(LogSlateInspectorToolset, Warning, "SlateInspectorToolset: {Message}", Message);
	UKismetSystemLibrary::RaiseScriptError(Message);
}

/** Short human-readable identification of a widget for log messages. Text block text is truncated. */
FString DescribeWidget(TSharedRef<SWidget> Widget)
{
	static const FName TextBlockType(TEXT("STextBlock"));
	constexpr int32 MaxDescribedTextLength = 64;

	const FName WidgetType = Widget->GetType();
	FString Description = WidgetType.ToString();
	if (WidgetType == TextBlockType)
	{
		FString Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
		if (Text.Len() > MaxDescribedTextLength)
		{
			Text.LeftInline(MaxDescribedTextLength);
			Text += TEXT("...");
		}
		Description += FString::Printf(TEXT(" \"%s\""), *Text);
	}
	return Description;
}

/**
 * Refusal reason for a key press and release nothing handled, naming where they went and what to try instead.
 *
 * @param ModifierKeys  Modifiers the events carried, including those KeyDown holds.
 */
FString DescribeUnhandledKey(const FKey& Key, const FModifierKeysState& ModifierKeys, int32 SlateUserIndex)
{
	const TSharedPtr<SWidget> FocusedWidget = FSlateApplication::Get().GetUserFocusedWidget(SlateUserIndex);
	const FString Destination = FocusedWidget.IsValid()
		? FString::Printf(TEXT("it went to Slate user %d's focused widget %s and its ancestors"),
			SlateUserIndex, *DescribeWidget(FocusedWidget.ToSharedRef()))
		: FString::Printf(TEXT("Slate user %d has no focused widget"), SlateUserIndex);

	// Only BackSpace, Tab, Enter and Escape get a char event, so a focused text box ignores a printable key.
	const uint32* KeyCode = nullptr;
	const uint32* CharCode = nullptr;
	FInputKeyManager::Get().GetCodesFromKey(Key, KeyCode, CharCode);
	const bool bTypesCharacter = CharCode && !ModifierKeys.IsControlDown() && !ModifierKeys.IsAltDown() && !ModifierKeys.IsCommandDown();
	const TCHAR* Advice = bTypesCharacter
		? TEXT("no character is sent for this key, so use Type to enter text")
		: TEXT("Focus or Click the widget that should take the key first");
	return FString::Printf(TEXT("nothing handled the press or release; %s -- %s"), *Destination, Advice);
}

/**
 * True when Widget has a non-zero arranged size, so there is a point inside it to aim at.
 *
 * @param OutReason  Filled in with why the widget cannot be aimed at.
 */
bool HasArrangedSize(TSharedRef<SWidget> Widget, FString& OutReason)
{
	const FVector2f LocalSize = Widget->GetTickSpaceGeometry().GetLocalSize();
	if (LocalSize.X <= 0.f || LocalSize.Y <= 0.f)
	{
		OutReason = FString::Printf(
			TEXT("%s has no arranged size (%gx%g) -- it is collapsed, or its window has not painted, so there is no point to aim at"),
			*DescribeWidget(Widget), LocalSize.X, LocalSize.Y);
		return false;
	}
	return true;
}

/**
 * True when a pointer event at Point would be routed to Widget or its branch. Assumes Widget has
 * an arranged size; see HasArrangedSize.
 *
 * Uses the same hit test as Slate's pointer routing (LocateWindowUnderMouse over the interactive
 * top-level windows). A hit on the widget or any descendant counts. A hit on an ancestor counts
 * only when the widget itself is not hit-test visible, since Slate then routes to the ancestor,
 * and nothing between them clips the point away.
 *
 * @param Windows    Interactive top-level windows, from FSlateApplication::GetInteractiveTopLevelWindows.
 * @param OutReason  When not null, filled in with what is in the way.
 */
bool PointReachesWidget(TSharedRef<SWidget> Widget, const FVector2D& Point, const TArray<TSharedRef<SWindow>>& Windows, FString* OutReason)
{
	FSlateApplication& SlateApp = FSlateApplication::Get();
	const FWidgetPath PathUnderPoint = SlateApp.LocateWindowUnderMouse(
		Point, Windows, /* bIgnoreEnabledStatus */ false, SlateApp.GetUserIndexForMouse());

	if (!PathUnderPoint.IsValid())
	{
		if (OutReason)
		{
			*OutReason = FString::Printf(
				TEXT("nothing is under (%g, %g) -- the widget's window is offscreen, hidden or minimized"),
				Point.X, Point.Y);
		}
		return false;
	}

	// The target is on the path: it either receives the event itself, or a descendant of it does
	// because the point is deeper inside. Both are hits on the target's own branch.
	if (PathUnderPoint.ContainsWidget(&Widget.Get()))
	{
		return true;
	}

	// Slate truncates the hit path at the first disabled widget, so a disabled target or ancestor
	// shows up as a hit on the nearest enabled ancestor. Walk up to that ancestor to tell the cases apart.
	// A target that is not hit-test visible is never on the path, so its own enabled state does not matter.
	const TSharedRef<SWidget> HitWidget = PathUnderPoint.GetLastWidget();
	const bool bTargetIsHitTestVisible = Widget->GetVisibility().IsHitTestVisible();
	TSharedPtr<SWidget> DisabledWidget;
	bool bPointIsOutsideAnIntermediate = false;
	bool bHitIsAncestorOfTarget = false;
	for (TSharedPtr<SWidget> Walk = Widget; Walk.IsValid(); Walk = Walk->GetParentWidget())
	{
		if (Walk == HitWidget)
		{
			bHitIsAncestorOfTarget = true;
			break;
		}
		const bool bCountsForEnabled = Walk != Widget || bTargetIsHitTestVisible;
		if (bCountsForEnabled && !DisabledWidget.IsValid() && !Walk->IsEnabled())
		{
			DisabledWidget = Walk;
		}

		// A widget between the target and the hit that clips, or that would itself have been hit,
		// hides the target wherever the point is outside it (e.g. a scroll panel with the target
		// scrolled out of view).
		const bool bCanHideTarget = Walk->GetVisibility().IsHitTestVisible() || Walk->GetClipping() != EWidgetClipping::Inherit;
		if (Walk != Widget && bCanHideTarget && !Walk->GetTickSpaceGeometry().IsUnderLocation(Point))
		{
			bPointIsOutsideAnIntermediate = true;
		}
	}

	if (bHitIsAncestorOfTarget && DisabledWidget.IsValid())
	{
		if (OutReason)
		{
			*OutReason = DisabledWidget == Widget
				? FString::Printf(
					TEXT("%s at (%g, %g) is disabled, so it cannot receive input"),
					*DescribeWidget(Widget), Point.X, Point.Y)
				: FString::Printf(
					TEXT("(%g, %g) is inside %s, but its ancestor %s in window '%s' is disabled, ")
					TEXT("so nothing inside it can receive input"),
					Point.X, Point.Y, *DescribeWidget(Widget), *DescribeWidget(DisabledWidget.ToSharedRef()),
					*PathUnderPoint.GetDeepestWindow()->GetTitle().ToString());
		}
		return false;
	}

	// An ancestor hit counts only when the target cannot take pointer input itself, so Slate routes
	// the event to the ancestor. Otherwise the target is clipped, culled or scrolled out of view.
	if (bHitIsAncestorOfTarget && !bTargetIsHitTestVisible && !bPointIsOutsideAnIntermediate)
	{
		return true;
	}

	if (OutReason)
	{
		*OutReason = bHitIsAncestorOfTarget
			? FString::Printf(
				TEXT("(%g, %g) is inside %s, but the point resolves to its ancestor %s in window '%s' ")
				TEXT("-- the widget is not under that point, so it is clipped, culled or scrolled out of view"),
				Point.X, Point.Y, *DescribeWidget(Widget), *DescribeWidget(HitWidget),
				*PathUnderPoint.GetDeepestWindow()->GetTitle().ToString())
			: FString::Printf(
				TEXT("(%g, %g) belongs to %s in window '%s', not to %s"),
				Point.X, Point.Y,
				*DescribeWidget(HitWidget),
				*PathUnderPoint.GetDeepestWindow()->GetTitle().ToString(),
				*DescribeWidget(Widget));
	}
	return false;
}

/** Convenience form of PointReachesWidget for a single point: also checks the arranged size. */
bool PointReachesWidget(TSharedRef<SWidget> Widget, const FVector2D& Point, FString& OutReason)
{
	return HasArrangedSize(Widget, OutReason)
		&& PointReachesWidget(Widget, Point, FSlateApplication::Get().GetInteractiveTopLevelWindows(), &OutReason);
}

/**
 * True when both coordinates of Point are in [0, 1].
 *
 * @param Name       Parameter name for the reason, e.g. "Position" or "StartPosition".
 * @param OutReason  Filled in with which value is out of range.
 */
bool IsNormalizedPoint(const FSlateInspectorToolsetNormalizedPoint& Point, const TCHAR* Name, FString& OutReason)
{
	if (Point.X < 0.f || Point.X > 1.f || Point.Y < 0.f || Point.Y > 1.f)
	{
		OutReason = FString::Printf(TEXT("%s (%g, %g) must be in [0, 1]: 0 = left/top edge, 0.5 = center, 1 = right/bottom edge"),
			Name, Point.X, Point.Y);
		return false;
	}
	return true;
}

/**
 * Screen point at a normalized position inside a widget; (0.5, 0.5) is the center.
 *
 * Slate's hit test includes both edges of a widget, so a point exactly on the edge can also belong
 * to the neighboring widget. Points are kept half a pixel inside the edges so 0 and 1 aim at the
 * widget's own outermost pixels.
 */
FVector2D WidgetScreenPointAt(TSharedRef<SWidget> Widget, double NormalizedX, double NormalizedY)
{
	const FGeometry& Geometry = Widget->GetTickSpaceGeometry();
	const FVector2D LocalSize(Geometry.GetLocalSize());

	auto InsetAxis = [](double Normalized, double Size)
	{
		constexpr double EdgeInset = 0.5;
		const double Local = FMath::Clamp(Normalized, 0.0, 1.0) * Size;
		return Size > 2.0 * EdgeInset ? FMath::Clamp(Local, EdgeInset, Size - EdgeInset) : Size * 0.5;
	};

	const FVector2D LocalPoint(InsetAxis(NormalizedX, LocalSize.X), InsetAxis(NormalizedY, LocalSize.Y));
	return FVector2D(Geometry.LocalToAbsolute(LocalPoint));
}

FVector2D WidgetScreenPointAt(TSharedRef<SWidget> Widget, const FSlateInspectorToolsetNormalizedPoint& Point)
{
	return WidgetScreenPointAt(Widget, Point.X, Point.Y);
}

/**
 * Pick the window a screenshot with no ref should capture: the active top-level window if it is
 * visible, otherwise the first visible regular window, otherwise the first visible window.
 */
TSharedPtr<SWindow> ResolveDefaultScreenshotWindow()
{
	FSlateApplication& SlateApp = FSlateApplication::Get();

	TSharedPtr<SWindow> ActiveWindow = SlateApp.GetActiveTopLevelWindow();
	if (ActiveWindow.IsValid() && ActiveWindow->IsVisible() && !ActiveWindow->IsWindowMinimized())
	{
		return ActiveWindow;
	}

	TArray<TSharedRef<SWindow>> VisibleWindows;
	SlateApp.GetAllVisibleWindowsOrdered(VisibleWindows);
	if (VisibleWindows.IsEmpty())
	{
		return nullptr;
	}

	for (const TSharedRef<SWindow>& Window : VisibleWindows)
	{
		if (Window->IsRegularWindow())
		{
			return Window;
		}
	}

	return VisibleWindows[0];
}

/**
 * The most recent refusal recorded by an action tool. Game-thread only. Seq increments on each
 * refusal, so a caller can compare it before and after an action.
 */
struct FRefusalRecord
{
	int32 Seq = 0;
	FString Tool;
	FString Target;
	FString Reason;
};
FRefusalRecord GLastRefusal;

/** Last position sent for each finger the touch tools still hold down, since Slate does not expose whether a finger is down. */
TMap<int32, FVector2D> GActiveTouchPositions;

/** Buttons MouseDown pressed and nothing has released yet; kept apart from Slate's pressed set so a button the user is physically holding is never released. */
TSet<FKey> GHeldMouseButtons;

/** A key KeyDown pressed and nothing has released yet, with the device, Slate user and prefix modifiers its release is stamped with. */
struct FHeldKey
{
	FKey Key;
	FInputDeviceId DeviceId;
	int32 SlateUserIndex = INDEX_NONE;
	FModifierKeysState ModifierKeys;
};

/** Keys KeyDown holds, in press order, at most one per key and Slate user. The modifier keys among them are down for every later key event of that user, as a keyboard reports them. */
TArray<FHeldKey> GHeldKeys;

/** Whether KeyDown holds Key for the given Slate user. */
bool IsKeyHeldByUser(const FKey& Key, int32 SlateUserIndex)
{
	return GHeldKeys.ContainsByPredicate([&Key, SlateUserIndex](const FHeldKey& HeldKey)
	{
		return HeldKey.Key == Key && HeldKey.SlateUserIndex == SlateUserIndex;
	});
}

/** ModifierKeys with the modifier keys KeyDown holds for the given Slate user added. */
FModifierKeysState WithHeldModifierKeys(const FModifierKeysState& ModifierKeys, int32 SlateUserIndex)
{
	return FModifierKeysState(
		ModifierKeys.IsLeftShiftDown() || IsKeyHeldByUser(EKeys::LeftShift, SlateUserIndex),
		ModifierKeys.IsRightShiftDown() || IsKeyHeldByUser(EKeys::RightShift, SlateUserIndex),
		ModifierKeys.IsLeftControlDown() || IsKeyHeldByUser(EKeys::LeftControl, SlateUserIndex),
		ModifierKeys.IsRightControlDown() || IsKeyHeldByUser(EKeys::RightControl, SlateUserIndex),
		ModifierKeys.IsLeftAltDown() || IsKeyHeldByUser(EKeys::LeftAlt, SlateUserIndex),
		ModifierKeys.IsRightAltDown() || IsKeyHeldByUser(EKeys::RightAlt, SlateUserIndex),
		ModifierKeys.IsLeftCommandDown() || IsKeyHeldByUser(EKeys::LeftCommand, SlateUserIndex),
		ModifierKeys.IsRightCommandDown() || IsKeyHeldByUser(EKeys::RightCommand, SlateUserIndex),
		ModifierKeys.AreCapsLocked());
}

/** Forget the hold KeyDown has on Key for the given Slate user, if any. */
void ForgetHeldKey(const FKey& Key, int32 SlateUserIndex)
{
	GHeldKeys.RemoveAll([&Key, SlateUserIndex](const FHeldKey& HeldKey)
	{
		return HeldKey.Key == Key && HeldKey.SlateUserIndex == SlateUserIndex;
	});
}

/** Time of the last tool call that pressed, moved or released a button or finger, or pressed or released a held key, which the hold watchdog measures from. */
double GLastHeldInputTime = 0.0;

FTSTicker::FDelegateHandle GHeldInputWatchdogHandle;

float GHeldInputTimeoutSeconds = 100.0f;

FAutoConsoleVariableRef CVarHeldInputTimeoutSeconds(
	TEXT("SlateInspectorToolset59.HeldInputTimeoutSeconds"),
	GHeldInputTimeoutSeconds,
	TEXT("Seconds a button, finger or key held by MouseDown, TouchStart or KeyDown may go without another pointer, touch, KeyDown or KeyUp call before it is released automatically. 0 disables the release."),
	ECVF_Default);

/** Whether MouseDown, TouchStart or KeyDown holds anything the hold watchdog would release. */
bool IsAnyInputHeld()
{
	return !GHeldMouseButtons.IsEmpty() || !GActiveTouchPositions.IsEmpty() || !GHeldKeys.IsEmpty();
}

} // namespace UE::SlateInspectorToolset::Private

// -- Helpers ----------------------------------------------------------

TSharedPtr<SWidget> USlateInspectorToolset59::ResolveRefOrWarn(const FString& Ref)
{
	TSharedPtr<SWidget> Widget = UE::WidgetHelper::FWidgetRefCache::Get().ResolveRef(FStringView(Ref));
	if (!Widget.IsValid())
	{
		UE_LOGFMT(LogSlateInspectorToolset, Warning, "SlateInspectorToolset: Could not resolve ref '{Ref}'", Ref);
	}
	return Widget;
}

TSharedPtr<SWidget> USlateInspectorToolset59::ResolveRefOrRefuse(const FString& Tool, const FString& Ref)
{
	TSharedPtr<SWidget> Widget = UE::WidgetHelper::FWidgetRefCache::Get().ResolveRef(FStringView(Ref));
	if (!Widget.IsValid())
	{
		RecordRefusal(Tool, Ref, FString::Printf(TEXT("could not resolve ref '%s'"), *Ref));
	}
	return Widget;
}

void USlateInspectorToolset59::RecordRefusal(const FString& Tool, const FString& Target, const FString& Reason)
{
	UE::SlateInspectorToolset::Private::FRefusalRecord& Record = UE::SlateInspectorToolset::Private::GLastRefusal;
	++Record.Seq;
	Record.Tool = Tool;
	Record.Target = Target;
	Record.Reason = Reason;

	UE_LOGFMT(LogSlateInspectorToolset, Warning, "SlateInspectorToolset: {Tool} refused -- {Reason}", Tool, Reason);
}

FString USlateInspectorToolset59::LastRefusal()
{
	const UE::SlateInspectorToolset::Private::FRefusalRecord& Record = UE::SlateInspectorToolset::Private::GLastRefusal;
	return FString::Printf(
		TEXT("{\"seq\":%d,\"tool\":\"%s\",\"target\":\"%s\",\"reason\":\"%s\"}"),
		Record.Seq,
		*UE::SlateInspectorToolset::Private::EscapeJsonString(Record.Tool),
		*UE::SlateInspectorToolset::Private::EscapeJsonString(Record.Target),
		*UE::SlateInspectorToolset::Private::EscapeJsonString(Record.Reason));
}

FKey USlateInspectorToolset59::ParseMouseButtonKey(const FString& Button)
{
	if (Button.Equals(TEXT("left"), ESearchCase::IgnoreCase))
	{
		return EKeys::LeftMouseButton;
	}
	if (Button.Equals(TEXT("right"), ESearchCase::IgnoreCase))
	{
		return EKeys::RightMouseButton;
	}
	if (Button.Equals(TEXT("middle"), ESearchCase::IgnoreCase))
	{
		return EKeys::MiddleMouseButton;
	}

	return EKeys::Invalid;
}

TSharedPtr<SWindow> USlateInspectorToolset59::FindWidgetWindowOrWarn(TSharedRef<SWidget> Widget)
{
	TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(Widget);
	if (!Window.IsValid())
	{
		UE_LOGFMT(LogSlateInspectorToolset, Warning, "SlateInspectorToolset: Could not find window containing widget");
	}
	return Window;
}

bool USlateInspectorToolset59::ResolvePointerTarget(const FString& Ref, double OffsetX, double OffsetY, TSharedPtr<SWidget>& OutWidget, FVector2D& OutScreenPosition)
{
	OutWidget.Reset();

	if (Ref.IsEmpty())
	{
		OutScreenPosition = FVector2D(OffsetX, OffsetY);
		return true;
	}

	OutWidget = ResolveRefOrWarn(Ref);
	if (!OutWidget.IsValid())
	{
		return false;
	}

	OutScreenPosition = UE::SlateInspectorToolset::Private::WidgetScreenPointAt(OutWidget.ToSharedRef(), FSlateInspectorToolsetNormalizedPoint())
		+ FVector2D(OffsetX, OffsetY);
	return true;
}

bool USlateInspectorToolset59::IsMouseButtonFree(FKey MouseButton)
{
	// Hardware cannot press a button that is still down, and Slate would route such a press to the captor, not the widget under the pointer.
	return !FSlateApplication::Get().GetPressedMouseButtons().Contains(MouseButton);
}

void USlateInspectorToolset59::SimulateHeldMouseMove(const FVector2D& ScreenPosition, const FVector2D& LastScreenPosition, const TSet<FKey>& PressedButtons, const FModifierKeysState& ModifierKeys)
{
	// Moving the cursor first lets Slate drop the OS echo as a zero-delta move and keeps GetCursorPos() in step with the event.
	FSlateApplication::Get().SetCursorPos(ScreenPosition);

	FPointerEvent MouseMoveEvent(
		FSlateApplication::Get().GetUserIndexForMouse(),
		FSlateApplication::CursorPointerIndex,
		ScreenPosition,
		LastScreenPosition,
		PressedButtons,
		EKeys::Invalid,
		0.0f,
		ModifierKeys);
	FSlateApplication::Get().ProcessMouseMoveEvent(MouseMoveEvent);

	NoteHeldInputActivity();
}

void USlateInspectorToolset59::SimulateRedirectedMouseMove(TSharedPtr<SWidget> TargetWidget, const FVector2D& ScreenPosition, const FVector2D& LastScreenPosition, const FModifierKeysState& ModifierKeys)
{
	TUniquePtr<FScopedSlateInputRedirect> InputRedirect;
	if (TargetWidget.IsValid())
	{
		InputRedirect = MakeUnique<FScopedSlateInputRedirect>(TargetWidget.ToSharedRef());
	}

	SimulateHeldMouseMove(ScreenPosition, LastScreenPosition, FSlateApplication::Get().GetPressedMouseButtons(), ModifierKeys);
}

void USlateInspectorToolset59::SimulateHeldMouseButtonDown(TSharedRef<SWindow> Window, const FVector2D& ScreenPosition, FKey MouseButton, const FModifierKeysState& ModifierKeys)
{
	// Like the platform path, the event reports the button set as it is after the press.
	TSet<FKey> PressedButtons = FSlateApplication::Get().GetPressedMouseButtons();
	PressedButtons.Add(MouseButton);

	FPointerEvent MouseDownEvent(
		FSlateApplication::Get().GetUserIndexForMouse(),
		FSlateApplication::CursorPointerIndex,
		ScreenPosition,
		ScreenPosition,
		PressedButtons,
		MouseButton,
		0.0f,
		ModifierKeys);
	FSlateApplication::Get().ProcessMouseButtonDownEvent(Window->GetNativeWindow(), MouseDownEvent);

	UE::SlateInspectorToolset::Private::GHeldMouseButtons.Add(MouseButton);
	NoteHeldInputActivity();
}

void USlateInspectorToolset59::SimulateHeldMouseButtonUp(const FVector2D& ScreenPosition, FKey MouseButton, const FModifierKeysState& ModifierKeys)
{
	// Like the platform path, the event reports the button set as it is after the release.
	TSet<FKey> PressedButtons = FSlateApplication::Get().GetPressedMouseButtons();
	PressedButtons.Remove(MouseButton);

	FPointerEvent MouseUpEvent(
		FSlateApplication::Get().GetUserIndexForMouse(),
		FSlateApplication::CursorPointerIndex,
		ScreenPosition,
		ScreenPosition,
		PressedButtons,
		MouseButton,
		0.0f,
		ModifierKeys);
	FSlateApplication::Get().ProcessMouseButtonUpEvent(MouseUpEvent);

	// Only once nothing is pressed: a redirect that ended mid-press parked its captor so this release could hit-test normally.
	if (FSlateApplication::Get().GetPressedMouseButtons().IsEmpty())
	{
		FScopedSlateInputRedirect::RestoreDeferredCapture();
	}

	UE::SlateInspectorToolset::Private::GHeldMouseButtons.Remove(MouseButton);
	NoteHeldInputActivity();
}

bool USlateInspectorToolset59::SimulateClick(const FString& Tool, const FString& Target, TSharedRef<SWidget> Widget, FKey MouseButton, bool bDoubleClick,
	const FModifierKeysState& ModifierKeys, const FSlateInspectorToolsetNormalizedPoint& Position)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(Widget);
	if (!Window.IsValid())
	{
		RecordRefusal(Tool, Target, TEXT("the widget is not in any window, so there is nothing to send the event to"));
		return false;
	}

	if (!IsMouseButtonFree(MouseButton))
	{
		RecordRefusal(Tool, Target, FString::Printf(
			TEXT("the %s button is already held, so the press would route to its captor; call MouseUp first"), *MouseButton.GetDisplayName().ToString()));
		return false;
	}

	// In PIE, release any foreign mouse capture (e.g. the game viewport) so the
	// synthetic events below hit-test to this widget, restoring it afterward.
	// No-op in the editor, where nothing holds capture between tool calls.
	FScopedSlateInputRedirect InputRedirect(Widget);

	const FVector2D ScreenPoint = UE::SlateInspectorToolset::Private::WidgetScreenPointAt(Widget, Position);
	UE::SlateInspectorToolset::Private::MovePlatformCursorForPointerInput(ScreenPoint);

	// Capture is released above before this test, so it reflects where the click will land.
	FString MissReason;
	if (!UE::SlateInspectorToolset::Private::PointReachesWidget(Widget, ScreenPoint, MissReason))
	{
		RecordRefusal(Tool, Target, MissReason);
		return false;
	}

	// Move Slate's pointer without moving the OS cursor, except on Mac.
	FScopedSlateDetachedCursor DetachedCursor;

	DetachedCursor.SetCursorPos(ScreenPoint);

	// Send a mouse move so Slate updates its hover tracking, widget path
	// cache, and window z-order for hit-testing.
	FPointerEvent MoveEvent(
		FSlateApplication::Get().GetUserIndexForMouse(),
		FSlateApplication::CursorPointerIndex,
		ScreenPoint,
		ScreenPoint,
		TSet<FKey>(),
		EKeys::Invalid,
		0.0f,
		ModifierKeys);
	FSlateApplication::Get().ProcessMouseMoveEvent(MoveEvent);

	TSet<FKey> PressedButtons;
	PressedButtons.Add(MouseButton);

	FPointerEvent MouseEvent(
		FSlateApplication::Get().GetUserIndexForMouse(),
		FSlateApplication::CursorPointerIndex,
		ScreenPoint,
		ScreenPoint,
		PressedButtons,
		MouseButton,
		0.0f,
		ModifierKeys);

	if (bDoubleClick)
	{
		FSlateApplication::Get().ProcessMouseButtonDoubleClickEvent(Window->GetNativeWindow(), MouseEvent);
	}
	else
	{
		FSlateApplication::Get().ProcessMouseButtonDownEvent(Window->GetNativeWindow(), MouseEvent);
	}

	// Release for both paths. ProcessMouseButtonDoubleClickEvent implies a
	// mouse-down, so we must release to avoid leaving widgets in pressed state.
	FPointerEvent MouseUpEvent(
		FSlateApplication::Get().GetUserIndexForMouse(),
		FSlateApplication::CursorPointerIndex,
		ScreenPoint,
		ScreenPoint,
		TSet<FKey>(),
		MouseButton,
		0.0f,
		ModifierKeys);
	FSlateApplication::Get().ProcessMouseButtonUpEvent(MouseUpEvent);

	return true;
}

// -- Snapshot tools ---------------------------------------------------

FString USlateInspectorToolset59::Snapshot(const FString& Ref, int32 MaxDepth, bool bIncludeSourceLocations)
{
	MaxDepth = FMath::Max(MaxDepth, 0);

	TSharedPtr<SWidget> Root;
	if (!Ref.IsEmpty())
	{
		Root = ResolveRefOrWarn(Ref);
		if (!Root.IsValid())
		{
			return TEXT("Error: Could not resolve ref");
		}
	}

	// If an observer covers this root, return its cached snapshot.
	if (!bIncludeSourceLocations)
	{
		FString CachedSnapshot = FSlateInspectorToolsetObserverManager::Get().FindMatchingObserverSnapshot(Root, MaxDepth);
		if (!CachedSnapshot.IsEmpty())
		{
			return CachedSnapshot;
		}
	}

	// No observer covers this request. Render fresh without resetting the cache
	// (observers may be maintaining refs for other subtrees).
	return FSlateInspectorToolsetSnapshotRenderer::Render(Root, MaxDepth, bIncludeSourceLocations);
}

FSlateInspectorToolsetUMGInfoResult USlateInspectorToolset59::GetUMGInfo(const FString& Ref)
{
	FSlateInspectorToolsetUMGInfoResult Result;

	TSharedPtr<SWidget> Widget = ResolveRefOrWarn(Ref);
	if (!Widget.IsValid())
	{
		Result.Error = TEXT("Could not resolve ref");
		return Result;
	}

	TOptional<FSlateInspectorToolsetUMGInfo> Info = FSlateInspectorToolsetUMGResolver::Resolve(Widget.ToSharedRef());
	if (!Info.IsSet())
	{
		Result.Error = TEXT("No UMG metadata found (pure Slate widget)");
		return Result;
	}

	// SourceObject is weak: the authoring UWidget can be collected while the SWidget
	// and its metadata outlive it (PIE teardown). Report that as an error so a null
	// Widget handle never reads as success, while still returning the metadata fields
	// the caller can use to identify what the widget was.
	Result.Widget = Info->SourceObject.Get();
	if (Result.Widget == nullptr)
	{
		Result.Error = TEXT("UMG metadata found but the authoring UWidget is no longer live");
	}

	Result.WidgetName = Info->WidgetName;
	Result.WidgetClass = Info->WidgetClassName;
	Result.Asset = Info->AssetPath;
	Result.ResolvedVia = Info->bResolvedOnParent ? TEXT("parent") : TEXT("self");

	for (const TPair<FString, FString>& Outer : Info->OuterUserWidgetChain)
	{
		FSlateInspectorToolsetUMGOuter& Entry = Result.OuterUserWidgets.AddDefaulted_GetRef();
		Entry.Name = Outer.Key;
		Entry.Class = Outer.Value;
	}

	return Result;
}

FString USlateInspectorToolset59::Observe(const FString& Ref, int32 MaxDepth)
{
	MaxDepth = FMath::Max(MaxDepth, 0);

	TSharedPtr<SWidget> RootWidget;
	if (!Ref.IsEmpty())
	{
		RootWidget = ResolveRefOrWarn(Ref);
		if (!RootWidget.IsValid())
		{
			return TEXT("Error: Could not resolve ref");
		}
	}

	return FSlateInspectorToolsetObserverManager::Get().AddObserver(RootWidget, MaxDepth);
}

bool USlateInspectorToolset59::Unobserve(const FString& Identifier)
{
	return FSlateInspectorToolsetObserverManager::Get().RemoveObserver(Identifier);
}

FString USlateInspectorToolset59::ListObservers()
{
	TArray<FSlateInspectorToolsetObserver> Observers = FSlateInspectorToolsetObserverManager::Get().GetObservers();

	FString Result = TEXT("[");
	for (int32 Index = 0; Index < Observers.Num(); ++Index)
	{
		const FSlateInspectorToolsetObserver& Observer = Observers[Index];
		if (Index > 0)
		{
			Result += TEXT(", ");
		}

		FString RootRef;
		if (!Observer.bRoot)
		{
			TSharedPtr<SWidget> RootWidget = Observer.RootWidget.Pin();
			if (RootWidget.IsValid())
			{
				const FName FoundRef = UE::WidgetHelper::FWidgetRefCache::Get().FindRef(RootWidget.ToSharedRef());
				RootRef = FoundRef.IsNone() ? FString() : FoundRef.ToString();
			}
		}

		Result += FString::Printf(
			TEXT("{\"identifier\": \"%s\", \"root\": %s, \"rootRef\": \"%s\", \"maxDepth\": %d, \"cachedSnapshotSize\": %d}"),
			*UE::SlateInspectorToolset::Private::EscapeJsonString(Observer.Identifier),
			Observer.bRoot ? TEXT("true") : TEXT("false"),
			*UE::SlateInspectorToolset::Private::EscapeJsonString(RootRef),
			Observer.MaxDepth,
			Observer.CachedSnapshotText.Len());
	}
	Result += TEXT("]");

	return Result;
}

FToolsetImage USlateInspectorToolset59::Screenshot(const FString& Ref)
{
	FToolsetImage Result;

	if (!FSlateApplication::IsInitialized())
	{
		UE::SlateInspectorToolset::Private::RaiseToolError(TEXT("Screenshot requires Slate; none is running."));
		return Result;
	}

	TSharedPtr<SWidget> Widget;
	if (!Ref.IsEmpty())
	{
		Widget = UE::WidgetHelper::FWidgetRefCache::Get().ResolveRef(FStringView(Ref));
		if (!Widget.IsValid())
		{
			UE::SlateInspectorToolset::Private::RaiseToolError(FString::Printf(
				TEXT("Screenshot could not resolve ref '%s'. Call Snapshot for current refs, or Windows('list') for window refs."), *Ref));
			return Result;
		}
	}
	else
	{
		Widget = UE::SlateInspectorToolset::Private::ResolveDefaultScreenshotWindow();
		if (!Widget.IsValid())
		{
			UE::SlateInspectorToolset::Private::RaiseToolError(TEXT("Screenshot found no visible window to capture. Call Windows('list') and pass a window ref."));
			return Result;
		}
	}

	// TakeScreenshot asserts on a widget that is not reachable from a live window, so check first.
	const FString WidgetDescription = UE::SlateInspectorToolset::Private::DescribeWidget(Widget.ToSharedRef());
	FWidgetPath WidgetPath;
	if (!FSlateApplication::Get().FindPathToWidget(Widget.ToSharedRef(), WidgetPath))
	{
		UE::SlateInspectorToolset::Private::RaiseToolError(FString::Printf(
			TEXT("Screenshot target %s (ref '%s') is not in any visible window."), *WidgetDescription, *Ref));
		return Result;
	}

	// The renderer reads back only the window's own pixels and fails the whole capture for a rect reaching past them,
	// so the capture is cropped to the part of the widget inside its window. TakeScreenshot offsets the area by the
	// widget's unrounded layout position in the window, ignoring any render transform, and truncates; the window-side
	// edges are rounded up so they land on the window's edges after that truncation.
	const TSharedRef<SWindow> Window = WidgetPath.GetWindow();
	const FGeometry Geometry = WidgetPath.FindArrangedWidget(Widget.ToSharedRef()).Get(FArrangedWidget::GetNullWidget()).Geometry;
	const FVector2f WidgetSize = Geometry.GetDrawSize();
	const FVector2f WidgetOffset = FVector2f(Geometry.GetAccumulatedLayoutTransform().GetTranslation()) - FVector2f(Window->GetPositionInScreen());
	const FVector2f WindowSize = FVector2f(Window->GetViewportSize());
	const FIntRect CaptureArea(
		FMath::Max(0, FMath::CeilToInt32(-WidgetOffset.X)),
		FMath::Max(0, FMath::CeilToInt32(-WidgetOffset.Y)),
		FMath::Min(FMath::FloorToInt32(WidgetSize.X), FMath::CeilToInt32(WindowSize.X - WidgetOffset.X)),
		FMath::Min(FMath::FloorToInt32(WidgetSize.Y), FMath::CeilToInt32(WindowSize.Y - WidgetOffset.Y)));
	if (CaptureArea.Width() <= 0 || CaptureArea.Height() <= 0)
	{
		UE::SlateInspectorToolset::Private::RaiseToolError(FString::Printf(
			TEXT("Screenshot target %s (ref '%s') has no visible pixels in window '%s': it is %gx%g at (%g, %g) in a %gx%g window."),
			*WidgetDescription, *Ref, *Window->GetTitle().ToString(),
			WidgetSize.X, WidgetSize.Y, WidgetOffset.X, WidgetOffset.Y, WindowSize.X, WindowSize.Y));
		return Result;
	}

	constexpr bool bForceOpaque = true;
	TArray<FColor> ColorData;
	FIntVector Size = FIntVector::ZeroValue;
	if (!FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), CaptureArea, ColorData, Size))
	{
		UE::SlateInspectorToolset::Private::RaiseToolError(FString::Printf(
			TEXT("Slate declined to capture %s (ref '%s') in window '%s' (%dx%d requested, %d pixels read back)."),
			*WidgetDescription, *Ref, *Window->GetTitle().ToString(), Size.X, Size.Y, ColorData.Num()));
		return Result;
	}

	Result.SetFromImageView(FImageView(ColorData.GetData(), Size.X, Size.Y), bForceOpaque);

	return Result;
}

// -- Action tools -----------------------------------------------------

bool USlateInspectorToolset59::Click(const FString& Ref, const FSlateInspectorToolsetClickParams& Params)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	TSharedPtr<SWidget> Widget = ResolveRefOrRefuse(TEXT("click"), Ref);
	if (!Widget.IsValid())
	{
		return false;
	}

	const FKey MouseButton = ParseMouseButtonKey(Params.Button);
	if (!MouseButton.IsValid())
	{
		RecordRefusal(TEXT("click"), Ref, FString::Printf(TEXT("unknown Button '%s'; expected left, right or middle"), *Params.Button));
		return false;
	}

	FString InvalidReason;
	if (!UE::SlateInspectorToolset::Private::IsNormalizedPoint(Params.Position, TEXT("Position"), InvalidReason))
	{
		RecordRefusal(TEXT("click"), Ref, InvalidReason);
		return false;
	}

	return SimulateClick(TEXT("click"), Ref, Widget.ToSharedRef(), MouseButton, Params.bDoubleClick,
		Params.Modifiers.ToModifierKeysState(), Params.Position);
}

bool USlateInspectorToolset59::Hover(const FString& Ref, const FSlateInspectorToolsetHoverParams& Params)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	TSharedPtr<SWidget> Widget = ResolveRefOrRefuse(TEXT("hover"), Ref);
	if (!Widget.IsValid())
	{
		return false;
	}

	FString InvalidReason;
	if (!UE::SlateInspectorToolset::Private::IsNormalizedPoint(Params.Position, TEXT("Position"), InvalidReason))
	{
		RecordRefusal(TEXT("hover"), Ref, InvalidReason);
		return false;
	}

	// In PIE, release any foreign mouse capture so the hover move event hit-tests to this widget.
	FScopedSlateInputRedirect InputRedirect(Widget.ToSharedRef());

	const FVector2D ScreenPoint = UE::SlateInspectorToolset::Private::WidgetScreenPointAt(Widget.ToSharedRef(), Params.Position);
	UE::SlateInspectorToolset::Private::MovePlatformCursorForPointerInput(ScreenPoint);

	FString MissReason;
	if (!UE::SlateInspectorToolset::Private::PointReachesWidget(Widget.ToSharedRef(), ScreenPoint, MissReason))
	{
		RecordRefusal(TEXT("hover"), Ref, MissReason);
		return false;
	}

	// Unlike the other action tools, Hover moves the OS cursor rather than using
	// FScopedSlateDetachedCursor. Restoring Slate's pointer on scope exit makes the next tick
	// synthesize a move back to the real mouse position, so the widget would get OnMouseLeave
	// before any tooltip or hover-opened submenu appears.
	FSlateApplication::Get().SetCursorPos(ScreenPoint);

	// SetCursorPos alone does not update Slate's hover tracking.
	// Send a mouse move event so the widget receives OnMouseEnter and IsHovered() becomes true.
	FModifierKeysState ModifierKeys;
	FPointerEvent MouseMoveEvent(
		FSlateApplication::Get().GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex, ScreenPoint, ScreenPoint,
		TSet<FKey>(), FKey(), 0.0f, ModifierKeys);
	FSlateApplication::Get().ProcessMouseMoveEvent(MouseMoveEvent);

	return true;
}

FString USlateInspectorToolset59::HitTest(const FString& Ref, const FSlateInspectorToolsetHitTestParams& Params)
{
	auto Report = [](bool bReached, int32 Samples, int32 ReachedSamples, const FVector2D& Point, const FString& Reason)
	{
		return FString::Printf(
			TEXT("{\"reached\":%s,\"samples\":%d,\"reachedSamples\":%d,\"x\":%.1f,\"y\":%.1f,\"reason\":\"%s\"}"),
			bReached ? TEXT("true") : TEXT("false"), Samples, ReachedSamples, Point.X, Point.Y,
			*UE::SlateInspectorToolset::Private::EscapeJsonString(Reason));
	};

	if (!FSlateApplication::IsInitialized())
	{
		return Report(false, 0, 0, FVector2D::ZeroVector, TEXT("Slate is not running"));
	}

	// Resolved directly: HitTest reports failures in its result, so it neither logs nor records a refusal.
	const TSharedPtr<SWidget> Widget = UE::WidgetHelper::FWidgetRefCache::Get().ResolveRef(FStringView(Ref));
	if (!Widget.IsValid())
	{
		return Report(false, 0, 0, FVector2D::ZeroVector,
			FString::Printf(TEXT("could not resolve ref '%s'"), *Ref));
	}

	FString InvalidReason;
	if (!UE::SlateInspectorToolset::Private::IsNormalizedPoint(Params.Position, TEXT("Position"), InvalidReason))
	{
		return Report(false, 0, 0, FVector2D::ZeroVector, InvalidReason);
	}

	constexpr int32 MaxGridSide = 16;
	const int32 Side = FMath::Clamp(Params.Grid, 1, MaxGridSide);
	const int32 Samples = Side * Side;
	const FVector2D DefaultPoint = UE::SlateInspectorToolset::Private::WidgetScreenPointAt(
		Widget.ToSharedRef(), Side == 1 ? Params.Position : FSlateInspectorToolsetNormalizedPoint());

	FString FirstReason;
	if (!UE::SlateInspectorToolset::Private::HasArrangedSize(Widget.ToSharedRef(), FirstReason))
	{
		return Report(false, Samples, 0, DefaultPoint, FirstReason);
	}

	const TArray<TSharedRef<SWindow>> Windows = FSlateApplication::Get().GetInteractiveTopLevelWindows();
	int32 ReachedSamples = 0;
	FVector2D FirstMissPoint = FVector2D::ZeroVector;

	for (int32 Row = 0; Row < Side; ++Row)
	{
		for (int32 Column = 0; Column < Side; ++Column)
		{
			// A single sample uses Position. A lattice samples cell centers across the
			// whole widget, inset half a cell from each edge.
			const double SampleX = Side == 1 ? Params.Position.X : (Column + 0.5) / Side;
			const double SampleY = Side == 1 ? Params.Position.Y : (Row + 0.5) / Side;
			const FVector2D Point = UE::SlateInspectorToolset::Private::WidgetScreenPointAt(Widget.ToSharedRef(), SampleX, SampleY);

			// Only the first miss is reported, so skip building the reason for later ones.
			FString* OutReason = FirstReason.IsEmpty() ? &FirstReason : nullptr;
			if (UE::SlateInspectorToolset::Private::PointReachesWidget(Widget.ToSharedRef(), Point, Windows, OutReason))
			{
				++ReachedSamples;
			}
			else if (OutReason)
			{
				FirstMissPoint = Point;
			}
		}
	}

	const bool bReached = ReachedSamples == Samples;
	return Report(bReached, Samples, ReachedSamples, bReached ? DefaultPoint : FirstMissPoint, FirstReason);
}

bool USlateInspectorToolset59::Scroll(const FString& Ref, const FSlateInspectorToolsetScrollParams& Params)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	TSharedPtr<SWidget> Widget = ResolveRefOrRefuse(TEXT("scroll"), Ref);
	if (!Widget.IsValid())
	{
		return false;
	}

	const FVector2D GestureDelta(Params.GestureX, Params.GestureY);
	if (Params.Ticks == 0 && GestureDelta.IsZero())
	{
		RecordRefusal(TEXT("scroll"), Ref, TEXT("Ticks and the gesture deltas are all 0, so there is nothing to scroll"));
		return false;
	}

	FString InvalidReason;
	if (!UE::SlateInspectorToolset::Private::IsNormalizedPoint(Params.Position, TEXT("Position"), InvalidReason))
	{
		RecordRefusal(TEXT("scroll"), Ref, InvalidReason);
		return false;
	}

	FScopedSlateInputRedirect InputRedirect(Widget.ToSharedRef());

	const FVector2D ScreenPoint = UE::SlateInspectorToolset::Private::WidgetScreenPointAt(Widget.ToSharedRef(), Params.Position);
	UE::SlateInspectorToolset::Private::MovePlatformCursorForPointerInput(ScreenPoint);

	FString MissReason;
	if (!UE::SlateInspectorToolset::Private::PointReachesWidget(Widget.ToSharedRef(), ScreenPoint, MissReason))
	{
		RecordRefusal(TEXT("scroll"), Ref, MissReason);
		return false;
	}

	// Move Slate's pointer without moving the OS cursor, except on Mac.
	FScopedSlateDetachedCursor DetachedCursor;

	const FModifierKeysState ModifierKeys = Params.Modifiers.ToModifierKeysState();
	DetachedCursor.SetCursorPos(ScreenPoint);

	// Send a move first so Slate updates the widget path under the pointer before routing the wheel.
	const FPointerEvent MoveEvent(
		FSlateApplication::Get().GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
		ScreenPoint, ScreenPoint, TSet<FKey>(), EKeys::Invalid, 0.0f, ModifierKeys);
	FSlateApplication::Get().ProcessMouseMoveEvent(MoveEvent);

	// One wheel event per notch, since panels apply one step per event regardless of delta.
	// Clamped so a runaway value cannot stall the game thread.
	constexpr int32 MaxScrollTicks = 100;
	const int32 Notches = FMath::Min(FMath::Abs(Params.Ticks), MaxScrollTicks);
	const float Direction = Params.Ticks < 0 ? -1.f : 1.f;

	for (int32 Notch = 0; Notch < Notches; ++Notch)
	{
		// EKeys::Invalid as the effecting button, matching hardware wheel events from FSlateApplication::OnMouseWheel.
		const FPointerEvent WheelEvent(
			FSlateApplication::Get().GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			ScreenPoint, ScreenPoint, TSet<FKey>(), EKeys::Invalid, Direction, ModifierKeys);
		FSlateApplication::Get().ProcessMouseWheelOrGestureEvent(WheelEvent, /*InGestureEvent*/ nullptr);
	}

	if (!GestureDelta.IsZero())
	{
		// Slate falls back from OnTouchGesture to OnMouseWheel only on a non-zero wheel delta, so a zero-delta carrier avoids scrolling twice.
		const FPointerEvent GestureCarrier(
			FSlateApplication::Get().GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			ScreenPoint, ScreenPoint, TSet<FKey>(), EKeys::Invalid, 0.0f, ModifierKeys);
		FPointerEvent GestureEvent(
			IPlatformInputDeviceMapper::Get().GetDefaultInputDevice(),
			ScreenPoint,
			ScreenPoint,
			TSet<FKey>(),
			ModifierKeys,
			EGestureEvent::Scroll,
			EGesturePhase::Update,
			GestureDelta,
			/*bInIsDirectionInvertedFromDevice*/ false);
		FSlateApplication::Get().ProcessMouseWheelOrGestureEvent(GestureCarrier, &GestureEvent);
	}

	return true;
}

// -- Held pointer and touch tools -------------------------------------

void USlateInspectorToolset59::NoteHeldInputActivity()
{
	using namespace UE::SlateInspectorToolset::Private;

	GLastHeldInputTime = FApp::GetCurrentTime();

	// The ticker unregisters itself once nothing is held, so it only needs starting when a hold begins.
	if (IsAnyInputHeld() && !GHeldInputWatchdogHandle.IsValid())
	{
		GHeldInputWatchdogHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&USlateInspectorToolset59::TickHeldInputWatchdog));
	}
}

bool USlateInspectorToolset59::TickHeldInputWatchdog(float DeltaTime)
{
	using namespace UE::SlateInspectorToolset::Private;

	if (!IsAnyInputHeld())
	{
		GHeldInputWatchdogHandle.Reset();
		return false;
	}

	if (GHeldInputTimeoutSeconds <= 0.0f || FApp::GetCurrentTime() - GLastHeldInputTime < GHeldInputTimeoutSeconds)
	{
		return true;
	}

	UE_LOGFMT(LogSlateInspectorToolset, Warning,
		"SlateInspectorToolset: {ButtonCount} mouse button(s), {FingerCount} finger(s) and {KeyCount} key(s) held for over {Seconds}s without a tool call; releasing them",
		GHeldMouseButtons.Num(), GActiveTouchPositions.Num(), GHeldKeys.Num(), GHeldInputTimeoutSeconds);
	ReleaseHeldInput();
	return false;
}

bool USlateInspectorToolset59::MouseMove(double X, double Y, const FSlateInspectorToolsetModifierKeys59& Modifiers)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	// An absolute move names no widget, so the one under the destination is the PIE redirect target; the lookup needs the cursor moved first.
	const FVector2D ScreenPosition(X, Y);
	FSlateApplication& SlateApplication = FSlateApplication::Get();
	const FVector2D LastScreenPosition = SlateApplication.GetCursorPos();
	SlateApplication.SetCursorPos(ScreenPosition);
	const FWidgetPath PathUnderPoint = SlateApplication.LocateWindowUnderMouse(ScreenPosition, SlateApplication.GetInteractiveTopLevelWindows());
	TSharedPtr<SWidget> WidgetUnderPoint;
	if (PathUnderPoint.IsValid())
	{
		WidgetUnderPoint = PathUnderPoint.GetLastWidget();
	}

	SimulateRedirectedMouseMove(WidgetUnderPoint, ScreenPosition, LastScreenPosition, Modifiers.ToModifierKeysState());
	return true;
}

bool USlateInspectorToolset59::MouseMoveTo(const FString& Ref, double OffsetX, double OffsetY, const FSlateInspectorToolsetModifierKeys59& Modifiers)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	TSharedPtr<SWidget> Widget;
	FVector2D ScreenPosition = FVector2D::ZeroVector;
	if (!ResolvePointerTarget(Ref, OffsetX, OffsetY, Widget, ScreenPosition))
	{
		return false;
	}
	if (!Widget.IsValid())
	{
		UE_LOGFMT(LogSlateInspectorToolset, Warning,
			"SlateInspectorToolset: MouseMoveTo requires a widget ref; use MouseMove for an absolute position");
		return false;
	}

	// Starting from the cursor Slate already reports makes GetCursorDelta() the real travel.
	SimulateRedirectedMouseMove(Widget, ScreenPosition, FSlateApplication::Get().GetCursorPos(), Modifiers.ToModifierKeysState());
	return true;
}

bool USlateInspectorToolset59::MouseDown(const FString& Ref, const FString& Button, const FSlateInspectorToolsetModifierKeys59& Modifiers)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	const FKey MouseButton = ParseMouseButtonKey(Button);
	if (!MouseButton.IsValid())
	{
		UE_LOGFMT(LogSlateInspectorToolset, Warning, "SlateInspectorToolset: Unknown mouse button '{Button}'; expected left, right or middle", Button);
		return false;
	}

	TSharedPtr<SWidget> Widget = ResolveRefOrWarn(Ref);
	if (!Widget.IsValid())
	{
		return false;
	}

	const TSharedPtr<SWindow> Window = FindWidgetWindowOrWarn(Widget.ToSharedRef());
	if (!Window.IsValid())
	{
		return false;
	}

	// A repeat press would drop the captor's capture and un-press an SButton, so it is a no-op for the owning widget and refused for any other.
	if (!IsMouseButtonFree(MouseButton))
	{
		const TSharedPtr<FSlateUser> CursorUser = FSlateApplication::Get().GetCursorUser();
		if (CursorUser.IsValid() && CursorUser->GetCursorCaptor() == Widget)
		{
			UE_LOGFMT(LogSlateInspectorToolset, Verbose,
				"SlateInspectorToolset: MouseDown: the {Button} button is already held by this widget; leaving it held", Button);
			return true;
		}

		UE_LOGFMT(LogSlateInspectorToolset, Warning,
			"SlateInspectorToolset: MouseDown: the {Button} button is already held; call MouseUp first", Button);
		return false;
	}

	// Release foreign PIE capture so the press hit-tests here; it stays released until the release, so the rest of the gesture hit-tests too.
	FScopedSlateInputRedirect InputRedirect(Widget.ToSharedRef());

	const FModifierKeysState ModifierKeys = Modifiers.ToModifierKeysState();
	const FVector2D ScreenCenter =
		UE::SlateInspectorToolset::Private::WidgetScreenPointAt(Widget.ToSharedRef(), FSlateInspectorToolsetNormalizedPoint());

	// The move refreshes hover and the widget path the press is routed along, as Click does.
	SimulateHeldMouseMove(ScreenCenter, FSlateApplication::Get().GetCursorPos(), FSlateApplication::Get().GetPressedMouseButtons(), ModifierKeys);
	SimulateHeldMouseButtonDown(Window.ToSharedRef(), ScreenCenter, MouseButton, ModifierKeys);

	return true;
}

bool USlateInspectorToolset59::MouseUp(const FString& Button, const FSlateInspectorToolsetModifierKeys59& Modifiers)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	const FKey MouseButton = ParseMouseButtonKey(Button);
	if (!MouseButton.IsValid())
	{
		UE_LOGFMT(LogSlateInspectorToolset, Warning, "SlateInspectorToolset: Unknown mouse button '{Button}'; expected left, right or middle", Button);
		return false;
	}

	// Slate routes the release to the captor, or hit-tests under the cursor when nothing holds capture, so no lookup or redirect is needed.
	SimulateHeldMouseButtonUp(FSlateApplication::Get().GetCursorPos(), MouseButton, Modifiers.ToModifierKeysState());
	return true;
}

int32 USlateInspectorToolset59::ReleaseHeldInput()
{
	using namespace UE::SlateInspectorToolset::Private;

	if (!FSlateApplication::IsInitialized())
	{
		return 0;
	}

	int32 ReleasedCount = 0;

	// Buttons before fingers: a touch end also drops the left button from Slate's pressed set, which would hide a held MouseDown from this sweep.
	for (const FKey& MouseButton : TSet<FKey>(GHeldMouseButtons))
	{
		if (!IsMouseButtonFree(MouseButton))
		{
			SimulateHeldMouseButtonUp(FSlateApplication::Get().GetCursorPos(), MouseButton, FModifierKeysState());
			++ReleasedCount;
		}
		else
		{
			// Hardware released it in the meantime, so Slate has already routed the release.
			GHeldMouseButtons.Remove(MouseButton);
		}
	}

	for (const TPair<int32, FVector2D>& Finger : TMap<int32, FVector2D>(GActiveTouchPositions))
	{
		if (TouchEnd(Finger.Value.X, Finger.Value.Y, Finger.Key))
		{
			++ReleasedCount;
		}
	}

	// Latest press first, so each release reports the modifiers pressed before it as still down.
	while (!GHeldKeys.IsEmpty())
	{
		const FHeldKey HeldKey = GHeldKeys.Pop();
		FResolvedKeyInput Input;
		Input.Key = HeldKey.Key;
		Input.DeviceId = HeldKey.DeviceId;
		Input.SlateUserIndex = HeldKey.SlateUserIndex;
		Input.ModifierKeys = HeldKey.ModifierKeys;
		SendKeyEvent(Input, /*bDown*/ false, /*bRepeat*/ false);
		++ReleasedCount;
	}

	// Unconditionally, unlike the release path: a press the hardware ended for us leaves parked capture nothing else would drain.
	FScopedSlateInputRedirect::RestoreDeferredCapture();

	GHeldInputWatchdogHandle.Reset();
	return ReleasedCount;
}

bool USlateInspectorToolset59::ResolveTouchInput(int32 TouchIndex, FResolvedTouchInput& OutInput)
{
	constexpr int32 MaxTouchIndex = ETouchIndex::CursorPointerIndex - 1;
	if (TouchIndex < 0 || TouchIndex > MaxTouchIndex)
	{
		UE_LOGFMT(LogSlateInspectorToolset, Warning,
			"SlateInspectorToolset: TouchIndex {TouchIndex} is out of range; expected 0 to {MaxTouchIndex}", TouchIndex, MaxTouchIndex);
		return false;
	}

	// Unlike a gamepad key, a touch can fall back to the cursor's user, which exists in every session and keeps per-finger pointer state.
	const TOptional<int32> MappedUserIndex = GetDefaultInputDeviceUser(OutInput.DeviceId);
	OutInput.SlateUserIndex = MappedUserIndex.IsSet() ? MappedUserIndex.GetValue() : FSlateApplication::Get().GetUserIndexForMouse();
	return true;
}

void USlateInspectorToolset59::SimulateTouch(ETouchPhase Phase, const FResolvedTouchInput& Input, int32 TouchIndex, const FVector2D& ScreenPosition, const FVector2D& LastScreenPosition, const TSharedPtr<FGenericWindow>& PlatformWindow)
{
	const float Force = Phase == ETouchPhase::End ? 0.0f : 1.0f;
	FPointerEvent TouchEvent(
		Input.DeviceId,
		TouchIndex,
		ScreenPosition,
		LastScreenPosition,
		Force,
		/*bPressLeftMouseButton*/ true,
		/*bInIsForceChanged*/ false,
		/*bInIsFirstMove*/ false,
		FModifierKeysState(),
		TOptional<int32>(Input.SlateUserIndex));

	// Slate finds the window for a touch move or uncaptured release via the window under the OS cursor, so the cursor follows the finger.
	FSlateApplication& SlateApplication = FSlateApplication::Get();
	SlateApplication.SetCursorPos(ScreenPosition);

	// The finger record changes before routing so a handler calling back into these tools sees the state Slate is about to have.
	switch (Phase)
	{
	case ETouchPhase::Start:
		UE::SlateInspectorToolset::Private::GActiveTouchPositions.Add(TouchIndex, ScreenPosition);
		SlateApplication.ProcessTouchStartedEvent(PlatformWindow, TouchEvent);
		break;
	case ETouchPhase::Move:
		UE::SlateInspectorToolset::Private::GActiveTouchPositions.Add(TouchIndex, ScreenPosition);
		SlateApplication.ProcessTouchMovedEvent(TouchEvent);
		break;
	case ETouchPhase::End:
		UE::SlateInspectorToolset::Private::GActiveTouchPositions.Remove(TouchIndex);
		SlateApplication.ProcessTouchEndedEvent(TouchEvent);
		break;
	}

	NoteHeldInputActivity();
}

bool USlateInspectorToolset59::TouchStart(const FString& Ref, double X, double Y, int32 TouchIndex)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	FResolvedTouchInput Input;
	if (!ResolveTouchInput(TouchIndex, Input))
	{
		return false;
	}

	// Slate logs an error when a tracked finger starts again, so a finger these tools still hold down is refused until TouchEnd lifts it.
	if (UE::SlateInspectorToolset::Private::GActiveTouchPositions.Contains(TouchIndex))
	{
		UE_LOGFMT(LogSlateInspectorToolset, Warning,
			"SlateInspectorToolset: Touch {TouchIndex} is already down; call TouchEnd before starting it again", TouchIndex);
		return false;
	}

	FSlateApplication& SlateApplication = FSlateApplication::Get();

	TSharedPtr<SWidget> Widget;
	FVector2D ScreenPosition = FVector2D::ZeroVector;
	if (!ResolvePointerTarget(Ref, X, Y, Widget, ScreenPosition))
	{
		return false;
	}

	// A touch down is hit-tested inside the window that produced it, so an absolute position needs the window under it.
	TSharedPtr<SWindow> Window;
	if (Widget.IsValid())
	{
		Window = FindWidgetWindowOrWarn(Widget.ToSharedRef());
	}
	else
	{
		// The lookup consults the window under the OS cursor first, so the cursor is moved to the point before it runs.
		SlateApplication.SetCursorPos(ScreenPosition);
		const FWidgetPath PathUnderPoint =
			SlateApplication.LocateWindowUnderMouse(ScreenPosition, SlateApplication.GetInteractiveTopLevelWindows());
		if (PathUnderPoint.IsValid())
		{
			Window = PathUnderPoint.GetWindow();
		}
		else
		{
			UE_LOGFMT(LogSlateInspectorToolset, Warning, "SlateInspectorToolset: No window under ({X}, {Y}) for TouchStart", X, Y);
		}
	}
	if (!Window.IsValid())
	{
		return false;
	}

	// Release foreign PIE capture so the touch hit-tests here; the finger captures on its own index, so restoring cursor capture leaves it intact.
	TUniquePtr<FScopedSlateInputRedirect> InputRedirect;
	if (Widget.IsValid())
	{
		InputRedirect = MakeUnique<FScopedSlateInputRedirect>(Widget.ToSharedRef());
	}

	SimulateTouch(ETouchPhase::Start, Input, TouchIndex, ScreenPosition, ScreenPosition, Window->GetNativeWindow());
	return true;
}

bool USlateInspectorToolset59::TouchMove(double X, double Y, int32 TouchIndex)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	FResolvedTouchInput Input;
	if (!ResolveTouchInput(TouchIndex, Input))
	{
		return false;
	}

	// A move is only meaningful for a finger that is down; copied out because SimulateTouch rewrites the record.
	const FVector2D* FoundLastScreenPosition = UE::SlateInspectorToolset::Private::GActiveTouchPositions.Find(TouchIndex);
	if (FoundLastScreenPosition == nullptr)
	{
		UE_LOGFMT(LogSlateInspectorToolset, Warning,
			"SlateInspectorToolset: Touch {TouchIndex} is not down; call TouchStart before TouchMove", TouchIndex);
		return false;
	}

	const FVector2D LastScreenPosition = *FoundLastScreenPosition;
	SimulateTouch(ETouchPhase::Move, Input, TouchIndex, FVector2D(X, Y), LastScreenPosition);
	return true;
}

bool USlateInspectorToolset59::TouchEnd(double X, double Y, int32 TouchIndex)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	FResolvedTouchInput Input;
	if (!ResolveTouchInput(TouchIndex, Input))
	{
		return false;
	}

	// A release is only meaningful for a finger that is down; the touch also carries the left button, so an unpaired end would lift a MouseDown.
	if (!UE::SlateInspectorToolset::Private::GActiveTouchPositions.Contains(TouchIndex))
	{
		UE_LOGFMT(LogSlateInspectorToolset, Warning,
			"SlateInspectorToolset: Touch {TouchIndex} is not down; call TouchStart before TouchEnd", TouchIndex);
		return false;
	}

	const FVector2D ScreenPosition(X, Y);
	SimulateTouch(ETouchPhase::End, Input, TouchIndex, ScreenPosition, ScreenPosition);
	return true;
}

bool USlateInspectorToolset59::Type(const FString& Ref, const FString& Text, bool Submit)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	TSharedPtr<SWidget> Widget = ResolveRefOrRefuse(TEXT("type"), Ref);
	if (!Widget.IsValid())
	{
		return false;
	}

	// Focus the target widget only if neither it nor a child already has focus. HasKeyboardFocus()
	// alone is true only for the exact widget, so it misses a child holding focus (e.g. the inner
	// SEditableText of an SEditableTextBox). Setting focus on the outer widget would take it from
	// the child, ending any edit that commits on focus loss.
	if (!Widget->HasKeyboardFocus() && !Widget->HasFocusedDescendants())
	{
		FSlateApplication::Get().SetKeyboardFocus(Widget, EFocusCause::SetDirectly);

		if (!Widget->HasKeyboardFocus() && !Widget->HasFocusedDescendants())
		{
			RecordRefusal(TEXT("type"), Ref, FString::Printf(
				TEXT("%s did not take keyboard focus, so there is nothing to type into -- it may be disabled or not focusable"),
				*UE::SlateInspectorToolset::Private::DescribeWidget(Widget.ToSharedRef())));
			return false;
		}
	}

	FModifierKeysState ModifierKeys;

	// Send each character as a key char event.
	for (TCHAR Character : Text)
	{
		FCharacterEvent CharEvent(Character, ModifierKeys, 0, false);
		FSlateApplication::Get().ProcessKeyCharEvent(CharEvent);
	}

	if (Submit)
	{
		FKeyEvent KeyDownEvent(EKeys::Enter, ModifierKeys, 0, false, 0, 0);
		FSlateApplication::Get().ProcessKeyDownEvent(KeyDownEvent);
		FKeyEvent KeyUpEvent(EKeys::Enter, ModifierKeys, 0, false, 0, 0);
		FSlateApplication::Get().ProcessKeyUpEvent(KeyUpEvent);
	}

	return true;
}

bool USlateInspectorToolset59::ParseKeyWithModifiers(const FString& KeySpec, FKey& OutKey, FModifierKeysState& OutModifierKeys, FString& OutInvalidReason)
{
	// Parse modifier prefixes: "Ctrl+Shift+Alt+Meta+KeyName"
	bool bShift = false;
	bool bControl = false;
	bool bAlt = false;
	bool bCommand = false;
	FString Remaining = KeySpec;

	while (true)
	{
		if (Remaining.StartsWith(TEXT("Shift+"), ESearchCase::IgnoreCase))
		{
			bShift = true;
			Remaining.RightChopInline(6);
		}
		else if (Remaining.StartsWith(TEXT("Ctrl+"), ESearchCase::IgnoreCase) || Remaining.StartsWith(TEXT("Control+"), ESearchCase::IgnoreCase))
		{
			bControl = true;
			Remaining.RightChopInline(Remaining.StartsWith(TEXT("Ctrl+"), ESearchCase::IgnoreCase) ? 5 : 8);
		}
		else if (Remaining.StartsWith(TEXT("Alt+"), ESearchCase::IgnoreCase))
		{
			bAlt = true;
			Remaining.RightChopInline(4);
		}
		else if (Remaining.StartsWith(TEXT("Meta+"), ESearchCase::IgnoreCase) || Remaining.StartsWith(TEXT("Cmd+"), ESearchCase::IgnoreCase))
		{
			bCommand = true;
			Remaining.RightChopInline(Remaining.StartsWith(TEXT("Meta+"), ESearchCase::IgnoreCase) ? 5 : 4);
		}
		else
		{
			break;
		}
	}

	OutKey = FKey(*Remaining);
	if (!OutKey.IsValid())
	{
		OutInvalidReason = FString::Printf(TEXT("unknown key name '%s'; expected an FKey name such as Enter, Home or Gamepad_DPad_Up"), *Remaining);
		return false;
	}

	OutModifierKeys = FModifierKeysState(bShift, bShift, bControl, bControl, bAlt, bAlt, bCommand, bCommand, false);
	return true;
}

TOptional<int32> USlateInspectorToolset59::GetDefaultInputDeviceUser(FInputDeviceId& OutDeviceId)
{
	OutDeviceId = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
	return FSlateApplication::Get().GetUserIndexForInputDevice(OutDeviceId);
}

bool USlateInspectorToolset59::ResolveInputUser(const FKey& Key, int32 UserIndex, FInputDeviceId& OutDeviceId, int32& OutSlateUserIndex, FString& OutInvalidReason)
{
	FSlateApplication& SlateApplication = FSlateApplication::Get();

	if (!Key.IsGamepadKey())
	{
		// Slate creates a user for any index an event names, so an unknown index is refused rather than leaving a stray user behind.
		if (UserIndex >= 0 && !SlateApplication.GetUser(UserIndex).IsValid())
		{
			OutInvalidReason = FString::Printf(TEXT("no Slate user at index %d; pass -1 for the keyboard's user"), UserIndex);
			return false;
		}

		OutDeviceId = SlateApplication.GetInputDeviceIdForKeyboard();
		OutSlateUserIndex = UserIndex >= 0 ? UserIndex : SlateApplication.GetUserIndexForKeyboard();
		return true;
	}

	// Gamepad keys are routed as controller input so the Slate user index matches the owning player, as CommonUI expects from hardware.
	if (UserIndex < 0)
	{
		const TOptional<int32> MappedUserIndex = GetDefaultInputDeviceUser(OutDeviceId);

		// Mirror FSlateApplication::OnControllerButtonPressed: with no Slate user
		// mapped to the device the event has nowhere to route, so report failure
		// rather than a misleading success.
		if (!MappedUserIndex.IsSet())
		{
			OutInvalidReason = TEXT("no Slate user is mapped to the default input device, so a gamepad key has nowhere to go");
			return false;
		}

		OutSlateUserIndex = MappedUserIndex.GetValue();
		return true;
	}

	const TSharedPtr<const FSlateUser> SlateUser = SlateApplication.GetUser(UserIndex);
	if (!SlateUser.IsValid())
	{
		OutInvalidReason = FString::Printf(TEXT("no Slate user at index %d; pass -1 for the default controller's user"), UserIndex);
		return false;
	}

	// A local player without a controller still has a focus path, so their events carry the default device rather than being dropped.
	OutDeviceId = IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(SlateUser->GetPlatformUserId());
	if (!OutDeviceId.IsValid())
	{
		OutDeviceId = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
	}
	OutSlateUserIndex = UserIndex;
	return true;
}

bool USlateInspectorToolset59::ResolveKeySpec(const FString& KeySpec, int32 UserIndex, FResolvedKeyInput& OutInput, FString& OutInvalidReason)
{
	return ParseKeyWithModifiers(KeySpec, OutInput.Key, OutInput.ModifierKeys, OutInvalidReason)
		&& ResolveInputUser(OutInput.Key, UserIndex, OutInput.DeviceId, OutInput.SlateUserIndex, OutInvalidReason);
}

bool USlateInspectorToolset59::SendKeyEvent(const FResolvedKeyInput& Input, bool bDown, bool bRepeat)
{
	const FModifierKeysState ModifierKeys = UE::SlateInspectorToolset::Private::WithHeldModifierKeys(Input.ModifierKeys, Input.SlateUserIndex);
	FKeyEvent KeyEvent(Input.Key, ModifierKeys, Input.DeviceId, bRepeat, 0, 0, Input.SlateUserIndex);

	if (!bDown)
	{
		return FSlateApplication::Get().ProcessKeyUpEvent(KeyEvent);
	}

	bool bHandled = FSlateApplication::Get().ProcessKeyDownEvent(KeyEvent);

	// Some keys are handled via OnKeyChar rather than OnKeyDown (e.g.
	// BackSpace in SEditableText). Normally the OS translates WM_KEYDOWN
	// into WM_CHAR, but since we bypass the OS message loop we must send
	// the character event ourselves. Only send for unmodified keys (Ctrl/Alt
	// combos are handled purely via OnKeyDown).
	if (!ModifierKeys.IsControlDown() && !ModifierKeys.IsAltDown() && !ModifierKeys.IsCommandDown())
	{
		static const TMap<FKey, TCHAR> KeyToCharacter = {
			{ EKeys::BackSpace, TCHAR('\b') },
			{ EKeys::Tab,       TCHAR('\t') },
			{ EKeys::Enter,     TCHAR('\r') },
			{ EKeys::Escape,    TCHAR('\x1b') },
		};

		if (const TCHAR* CharCode = KeyToCharacter.Find(Input.Key))
		{
			FCharacterEvent CharEvent(*CharCode, ModifierKeys, Input.SlateUserIndex, bRepeat);
			bHandled |= FSlateApplication::Get().ProcessKeyCharEvent(CharEvent);
		}
	}

	return bHandled;
}

bool USlateInspectorToolset59::PressKey(const FString& Key, int32 UserIndex, int32 RepeatCount)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	constexpr int32 MaxRepeatCount = 1000;
	if (RepeatCount < 0 || RepeatCount > MaxRepeatCount)
	{
		RecordRefusal(TEXT("presskey"), Key, FString::Printf(TEXT("RepeatCount %d is invalid; it must be 0 to %d"), RepeatCount, MaxRepeatCount));
		return false;
	}

	FResolvedKeyInput Input;
	FString InvalidReason;
	if (!ResolveKeySpec(Key, UserIndex, Input, InvalidReason))
	{
		RecordRefusal(TEXT("presskey"), Key, InvalidReason);
		return false;
	}

	// The release is sent even when nothing took the press, as with a real key; a handler acting on the release counts too.
	bool bHandled = SendKeyEvent(Input, /*bDown*/ true, /*bRepeat*/ false);
	for (int32 RepeatIndex = 0; RepeatIndex < RepeatCount; ++RepeatIndex)
	{
		bHandled |= SendKeyEvent(Input, /*bDown*/ true, /*bRepeat*/ true);
	}

	// Releasing the key ends any hold KeyDown has on it for this user, as releasing a held key on a keyboard does.
	using namespace UE::SlateInspectorToolset::Private;
	ForgetHeldKey(Input.Key, Input.SlateUserIndex);
	bHandled |= SendKeyEvent(Input, /*bDown*/ false, /*bRepeat*/ false);

	if (!bHandled)
	{
		RecordRefusal(TEXT("presskey"), Key,
			DescribeUnhandledKey(Input.Key, WithHeldModifierKeys(Input.ModifierKeys, Input.SlateUserIndex), Input.SlateUserIndex));
		return false;
	}
	return true;
}

bool USlateInspectorToolset59::KeyDown(const FString& Key, int32 UserIndex)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	FResolvedKeyInput Input;
	FString InvalidReason;
	if (!ResolveKeySpec(Key, UserIndex, Input, InvalidReason))
	{
		RecordRefusal(TEXT("keydown"), Key, InvalidReason);
		return false;
	}

	// Recorded before the press is routed, so a modifier's own press reports it down as a keyboard's does.
	using namespace UE::SlateInspectorToolset::Private;
	if (!IsKeyHeldByUser(Input.Key, Input.SlateUserIndex))
	{
		GHeldKeys.Add(FHeldKey{ Input.Key, Input.DeviceId, Input.SlateUserIndex, Input.ModifierKeys });
	}
	// A held modifier is normally unhandled, so a press nothing handled is not refused.
	SendKeyEvent(Input, /*bDown*/ true, /*bRepeat*/ false);
	NoteHeldInputActivity();
	return true;
}

bool USlateInspectorToolset59::KeyUp(const FString& Key, int32 UserIndex)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	FResolvedKeyInput Input;
	FString InvalidReason;
	if (!ResolveKeySpec(Key, UserIndex, Input, InvalidReason))
	{
		RecordRefusal(TEXT("keyup"), Key, InvalidReason);
		return false;
	}

	// Most widgets act on the press, so a release nothing handled is normal and not refused.
	using namespace UE::SlateInspectorToolset::Private;
	ForgetHeldKey(Input.Key, Input.SlateUserIndex);
	SendKeyEvent(Input, /*bDown*/ false, /*bRepeat*/ false);
	NoteHeldInputActivity();
	return true;
}

bool USlateInspectorToolset59::AnalogInput(const FString& Key, double Value, int32 UserIndex)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	FResolvedKeyInput Input;
	FString InvalidReason;
	if (!ResolveKeySpec(Key, UserIndex, Input, InvalidReason))
	{
		UE_LOGFMT(LogSlateInspectorToolset, Warning, "SlateInspectorToolset: AnalogInput not sent -- {Reason}", InvalidReason);
		return false;
	}

	// A single value only describes a one-dimensional axis; paired keys such as Gamepad_Left2D are driven through their X and Y keys.
	if (!Input.Key.IsAxis1D())
	{
		UE_LOGFMT(LogSlateInspectorToolset, Warning,
			"SlateInspectorToolset: Key '{Key}' is not a 1D axis; AnalogInput needs an axis such as Gamepad_LeftX", Key);
		return false;
	}

	const FModifierKeysState& ModifierKeys = Input.ModifierKeys;
	if (ModifierKeys.IsShiftDown() || ModifierKeys.IsControlDown() || ModifierKeys.IsAltDown() || ModifierKeys.IsCommandDown())
	{
		UE_LOGFMT(LogSlateInspectorToolset, Warning, "SlateInspectorToolset: Modifier prefixes are not supported for AnalogInput ('{Key}')", Key);
		return false;
	}

	// Slate does not clamp analog values, and neither does this; hardware reports sticks in -1..1 and triggers in 0..1.
	FAnalogInputEvent AnalogEvent(
		Input.Key, ModifierKeys, Input.DeviceId, /*bInIsRepeat*/ false, 0, 0, Value, Input.SlateUserIndex);
	FSlateApplication::Get().ProcessAnalogInputEvent(AnalogEvent);
	return true;
}

bool USlateInspectorToolset59::Focus(const FString& Ref, int32 UserIndex)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	if (UserIndex < 0)
	{
		RecordRefusal(TEXT("focus"), Ref, FString::Printf(TEXT("UserIndex %d is invalid; it must be >= 0"), UserIndex));
		return false;
	}

	TSharedPtr<SWidget> Widget = ResolveRefOrRefuse(TEXT("focus"), Ref);
	if (!Widget.IsValid())
	{
		return false;
	}

	if (!FSlateApplication::Get().SetUserFocus(UserIndex, Widget, EFocusCause::SetDirectly))
	{
		RecordRefusal(TEXT("focus"), Ref, FString::Printf(
			TEXT("Slate did not give focus to %s for user %d -- the user may not exist, or the widget may be disabled or not focusable"),
			*UE::SlateInspectorToolset::Private::DescribeWidget(Widget.ToSharedRef()), UserIndex));
		return false;
	}
	return true;
}

bool USlateInspectorToolset59::SelectOption(const FString& Ref, const FString& Value)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	TSharedPtr<SWidget> Widget = ResolveRefOrRefuse(TEXT("select"), Ref);
	if (!Widget.IsValid())
	{
		return false;
	}

	TArray<TSharedRef<SWindow>> WindowsBefore;
	FSlateApplication::Get().GetAllVisibleWindowsOrdered(WindowsBefore);

	if (!SimulateClick(TEXT("select"), Ref, Widget.ToSharedRef(), EKeys::LeftMouseButton, false))
	{
		return false;
	}

	// Poll: the first tick typically creates the popup window and the second materializes its `SListView` items,
	// but large lists can spill item generation across additional ticks.
	static const FName TextBlockType(TEXT("STextBlock"));
	const TSet<TSharedRef<SWindow>> WindowsBeforeSet(WindowsBefore);
	TSharedPtr<SWidget> MatchingTextBlock;

	constexpr int32 MaxPollTicks = 4;
	for (int32 Iteration = 0; Iteration < MaxPollTicks && !MatchingTextBlock.IsValid(); ++Iteration)
	{
		FSlateApplication::Get().Tick();

		TArray<TSharedRef<SWindow>> VisibleWindows;
		FSlateApplication::Get().GetAllVisibleWindowsOrdered(VisibleWindows);

		for (const TSharedRef<SWindow>& Window : VisibleWindows)
		{
			if (WindowsBeforeSet.Contains(Window))
			{
				continue;
			}

			TArray<TSharedRef<SWidget>> Queue;
			FChildren* WindowChildren = UE::SlateInspectorToolset::Private::GetChildrenToWalk(*Window);
			for (int32 ChildIndex = 0; ChildIndex < WindowChildren->Num(); ++ChildIndex)
			{
				Queue.Add(WindowChildren->GetChildAt(ChildIndex));
			}

			int32 QueueIndex = 0;
			while (QueueIndex < Queue.Num())
			{
				TSharedRef<SWidget> Current = Queue[QueueIndex++];
				if (Current->GetType() == TextBlockType
					&& StaticCastSharedRef<STextBlock>(Current)->GetText().ToString() == Value)
				{
					MatchingTextBlock = Current;
					break;
				}
				FChildren* CurrentChildren = UE::SlateInspectorToolset::Private::GetChildrenToWalk(*Current);
				for (int32 ChildIndex = 0; ChildIndex < CurrentChildren->Num(); ++ChildIndex)
				{
					Queue.Add(CurrentChildren->GetChildAt(ChildIndex));
				}
			}

			if (MatchingTextBlock.IsValid())
			{
				break;
			}
		}
	}

	if (!MatchingTextBlock.IsValid())
	{
		RecordRefusal(TEXT("select"), Ref, FString::Printf(
			TEXT("no option with text '%s' appeared in a new popup window after clicking the widget"), *Value));
		return false;
	}

	return SimulateClick(TEXT("select"), Ref, MatchingTextBlock.ToSharedRef(), EKeys::LeftMouseButton, false);
}

bool USlateInspectorToolset59::Drag(const FString& StartRef, const FString& EndRef, const FSlateInspectorToolsetDragParams& Params)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	TSharedPtr<SWidget> StartWidget = ResolveRefOrRefuse(TEXT("drag"), StartRef);
	if (!StartWidget.IsValid())
	{
		return false;
	}

	const FKey MouseButton = ParseMouseButtonKey(Params.Button);
	if (!MouseButton.IsValid())
	{
		RecordRefusal(TEXT("drag"), StartRef, FString::Printf(TEXT("unknown Button '%s'; expected left, right or middle"), *Params.Button));
		return false;
	}

	// An empty EndRef drags StartWidget itself by its waypoints and MoveBy, e.g. a splitter or
	// slider that captures the pointer on press.
	const bool bDragBySelf = EndRef.IsEmpty();
	const FVector2D MoveBy(Params.MoveByX, Params.MoveByY);

	FString InvalidReason;
	if (!UE::SlateInspectorToolset::Private::IsNormalizedPoint(Params.StartPosition, TEXT("StartPosition"), InvalidReason)
		|| (!bDragBySelf && !UE::SlateInspectorToolset::Private::IsNormalizedPoint(Params.EndPosition, TEXT("EndPosition"), InvalidReason)))
	{
		RecordRefusal(TEXT("drag"), StartRef, InvalidReason);
		return false;
	}

	TSharedPtr<SWidget> EndWidget = bDragBySelf ? StartWidget : ResolveRefOrRefuse(TEXT("drag"), EndRef);
	if (!EndWidget.IsValid())
	{
		return false;
	}

	const TSharedPtr<SWindow> StartWindow = FSlateApplication::Get().FindWidgetWindow(StartWidget.ToSharedRef());
	if (!StartWindow.IsValid())
	{
		RecordRefusal(TEXT("drag"), StartRef, TEXT("the drag source is not in any window"));
		return false;
	}

	if (!IsMouseButtonFree(MouseButton))
	{
		RecordRefusal(TEXT("drag"), StartRef, FString::Printf(
			TEXT("the %s button is already held, so the press would route to its captor; call MouseUp first"), *MouseButton.GetDisplayName().ToString()));
		return false;
	}

	// In PIE, release any foreign mouse capture so the drag begins on the intended widget.
	FScopedSlateInputRedirect InputRedirect(StartWidget.ToSharedRef());

	const FVector2D StartPosition = UE::SlateInspectorToolset::Private::WidgetScreenPointAt(StartWidget.ToSharedRef(), Params.StartPosition);
	const FVector2D EndPosition = MoveBy + (bDragBySelf
		? StartPosition
		: UE::SlateInspectorToolset::Private::WidgetScreenPointAt(EndWidget.ToSharedRef(), Params.EndPosition));

	// Waypoints are offsets from the press point, visited in order before the drop point. Without a target
	// widget or a MoveBy the drop point is the press point, so appending it would walk the drag back.
	const bool bDropPointIsPressPoint = bDragBySelf && MoveBy.IsZero() && !Params.Waypoints.Points.IsEmpty();
	TArray<FVector2D> PathPoints;
	PathPoints.Reserve(Params.Waypoints.Points.Num() + 1);
	for (const FSlateInspectorToolsetPoint& Waypoint : Params.Waypoints.Points)
	{
		PathPoints.Add(StartPosition + FVector2D(Waypoint.X, Waypoint.Y));
	}
	if (!bDropPointIsPressPoint)
	{
		PathPoints.Add(EndPosition);
	}

	// A path that never leaves the press point would press and release in place, which is a click, not a drag.
	if (!PathPoints.ContainsByPredicate([&StartPosition](const FVector2D& Point) { return Point != StartPosition; }))
	{
		RecordRefusal(TEXT("drag"), StartRef,
			TEXT("the drag never leaves its start point; pass EndRef, a non-zero MoveBy, or a waypoint away from it"));
		return false;
	}

	UE::SlateInspectorToolset::Private::MovePlatformCursorForPointerInput(StartPosition);

	// Check the source before pressing, so nothing is picked up from the wrong widget.
	FString MissReason;
	if (!UE::SlateInspectorToolset::Private::PointReachesWidget(StartWidget.ToSharedRef(), StartPosition, MissReason))
	{
		RecordRefusal(TEXT("drag"), StartRef, FString::Printf(TEXT("source unreachable: %s"), *MissReason));
		return false;
	}

	// With a target widget, the drop point (including MoveBy) must reach that widget. Without one,
	// the point only needs to be on a window, since a captured drag routes to its captor.
	if (!bDragBySelf)
	{
		if (!UE::SlateInspectorToolset::Private::PointReachesWidget(EndWidget.ToSharedRef(), EndPosition, MissReason))
		{
			RecordRefusal(TEXT("drag"), EndRef, FString::Printf(TEXT("target unreachable: %s"), *MissReason));
			return false;
		}
	}
	else
	{
		const FVector2D Destination = PathPoints.Last();
		FSlateApplication& SlateApp = FSlateApplication::Get();
		const FWidgetPath PathAtDestination = SlateApp.LocateWindowUnderMouse(
			Destination, SlateApp.GetInteractiveTopLevelWindows(), /* bIgnoreEnabledStatus */ false, SlateApp.GetUserIndexForMouse());
		if (!PathAtDestination.IsValid())
		{
			RecordRefusal(TEXT("drag"), StartRef, FString::Printf(
				TEXT("destination (%g, %g) is not on any window; the waypoints or MoveBy took the pointer off-screen"),
				Destination.X, Destination.Y));
			return false;
		}
	}

	// Move Slate's pointer without moving the OS cursor, except on Mac.
	FScopedSlateDetachedCursor DetachedCursor;

	const FModifierKeysState ModifierKeys = Params.Modifiers.ToModifierKeysState();
	TSet<FKey> PressedButtons;
	PressedButtons.Add(MouseButton);

	// Move cursor to start, update hover tracking, then press.
	DetachedCursor.SetCursorPos(StartPosition);
	FPointerEvent StartMoveEvent(
		FSlateApplication::Get().GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex, StartPosition, StartPosition,
		TSet<FKey>(), EKeys::Invalid, 0.0f, ModifierKeys);
	FSlateApplication::Get().ProcessMouseMoveEvent(StartMoveEvent);

	FPointerEvent MouseDownEvent(
		FSlateApplication::Get().GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex, StartPosition, StartPosition,
		PressedButtons, MouseButton, 0.0f, ModifierKeys);
	FSlateApplication::Get().ProcessMouseButtonDownEvent(StartWindow->GetNativeWindow(), MouseDownEvent);

	// Step the pointer along each leg rather than jumping to its end, so Slate detects the drag
	// partway and the target receives OnDragOver before the drop. Delta-driven handlers such as
	// splitters and sliders also see a continuous sequence.
	constexpr double DragStepPixels = 16.0;
	constexpr int32 MinDragSteps = 2;
	constexpr int32 MaxDragSteps = 24;

	FVector2D LegStart = StartPosition;
	FVector2D PreviousPosition = StartPosition;
	for (const FVector2D& LegEnd : PathPoints)
	{
		const double Distance = FVector2D::Distance(LegStart, LegEnd);
		const int32 StepCount = FMath::Clamp(FMath::CeilToInt32(Distance / DragStepPixels), MinDragSteps, MaxDragSteps);

		for (int32 Step = 1; Step <= StepCount; ++Step)
		{
			// The last step lands exactly on LegEnd, so each leg's final OnDragOver sees the requested point.
			const FVector2D StepPosition = (Step == StepCount)
				? LegEnd
				: FMath::Lerp(LegStart, LegEnd, static_cast<double>(Step) / StepCount);

			// Set before the event so handlers reading FSlateApplication::GetCursorPos() see the same point.
			DetachedCursor.SetCursorPos(StepPosition);
			FPointerEvent MouseMoveEvent(
				FSlateApplication::Get().GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex, StepPosition, PreviousPosition,
				PressedButtons, EKeys::Invalid, 0.0f, ModifierKeys);
			FSlateApplication::Get().ProcessMouseMoveEvent(MouseMoveEvent);

			PreviousPosition = StepPosition;
		}

		LegStart = LegEnd;
	}

	// Release where the path ended, which is the last waypoint when there is no drop point.
	const FVector2D ReleasePosition = PathPoints.Last();
	FPointerEvent MouseUpEvent(
		FSlateApplication::Get().GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex, ReleasePosition, ReleasePosition,
		TSet<FKey>(), MouseButton, 0.0f, ModifierKeys);
	FSlateApplication::Get().ProcessMouseButtonUpEvent(MouseUpEvent);

	return true;
}

// -- Window tools -----------------------------------------------------

FString USlateInspectorToolset59::Windows(const FString& Action, int32 Index, int32 Width, int32 Height, const FString& Ref)
{
	if (!FSlateApplication::IsInitialized())
	{
		return TEXT("Error: Slate not initialized");
	}

	TArray<TSharedRef<SWindow>> VisibleWindows;
	FSlateApplication::Get().GetAllVisibleWindowsOrdered(VisibleWindows);

	if (Action.Equals(TEXT("list"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<SWindow> ActiveWindow = FSlateApplication::Get().GetActiveTopLevelWindow();

		FString Result = TEXT("[");
		for (int32 WindowIndex = 0; WindowIndex < VisibleWindows.Num(); ++WindowIndex)
		{
			const TSharedRef<SWindow>& Window = VisibleWindows[WindowIndex];
			if (WindowIndex > 0)
			{
				Result += TEXT(", ");
			}
			FString Title = UE::SlateInspectorToolset::Private::EscapeJsonString(Window->GetTitle().ToString());

			// Same "w" prefix and ref cache as the snapshot renderer, so a window has one ref in both.
			static const FName WindowRefPrefix("w");
			FString WindowRef = UE::WidgetHelper::FWidgetRefCache::Get().GetOrAssignRef(Window, WindowRefPrefix).ToString();

			const FVector2D Position = Window->GetPositionInScreen();
			const FVector2D SizeInScreen = Window->GetSizeInScreen();

			Result += FString::Printf(
				TEXT("{\"index\": %d, \"ref\": \"%s\", \"title\": \"%s\", \"active\": %s, \"regular\": %s, \"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d}"),
				WindowIndex,
				*WindowRef,
				*Title,
				(ActiveWindow == Window) ? TEXT("true") : TEXT("false"),
				Window->IsRegularWindow() ? TEXT("true") : TEXT("false"),
				FMath::RoundToInt32(Position.X),
				FMath::RoundToInt32(Position.Y),
				FMath::RoundToInt32(SizeInScreen.X),
				FMath::RoundToInt32(SizeInScreen.Y));
		}
		Result += TEXT("]");
		return Result;
	}

	// When Ref is given, it selects the window and Index is ignored. "list" never uses it,
	// so it is resolved only for the index-based actions below.
	if (!Ref.IsEmpty())
	{
		const TSharedPtr<SWidget> Widget = UE::WidgetHelper::FWidgetRefCache::Get().ResolveRef(FStringView(Ref));
		if (!Widget.IsValid())
		{
			return FString::Printf(TEXT("Error: Ref '%s' is stale or unknown; call Windows(\"list\") for current refs."), *Ref);
		}

		// Ref may name any widget; it resolves to the window containing it.
		const TSharedPtr<SWindow> Named = FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef());
		if (!Named.IsValid())
		{
			return FString::Printf(TEXT("Error: Ref '%s' is not in any window."), *Ref);
		}

		Index = VisibleWindows.IndexOfByPredicate(
			[&Named](const TSharedRef<SWindow>& Window) { return Window == Named; });
		if (Index == INDEX_NONE)
		{
			return FString::Printf(
				TEXT("Error: the window holding ref '%s' ('%s') is not in the visible window list."),
				*Ref, *Named->GetTitle().ToString());
		}
	}

	if (Action.Equals(TEXT("select"), ESearchCase::IgnoreCase))
	{
		if (Index >= 0 && Index < VisibleWindows.Num())
		{
			VisibleWindows[Index]->BringToFront();
			FSlateApplication::Get().SetAllUserFocus(VisibleWindows[Index], EFocusCause::SetDirectly);
			return TEXT("OK");
		}
		return FString::Printf(TEXT("Error: Window index %d out of range (0..%d)"), Index, VisibleWindows.Num() - 1);
	}

	if (Action.Equals(TEXT("close"), ESearchCase::IgnoreCase))
	{
		if (Index >= 0 && Index < VisibleWindows.Num())
		{
			VisibleWindows[Index]->RequestDestroyWindow();
			return TEXT("OK");
		}
		return FString::Printf(TEXT("Error: Window index %d out of range (0..%d)"), Index, VisibleWindows.Num() - 1);
	}

	const bool bResize = Action.Equals(TEXT("resize"), ESearchCase::IgnoreCase);
	const bool bMaximize = Action.Equals(TEXT("maximize"), ESearchCase::IgnoreCase);
	if (bResize || bMaximize)
	{
		if (Index < 0 || Index >= VisibleWindows.Num())
		{
			return FString::Printf(TEXT("Error: Window index %d out of range (0..%d)"), Index, VisibleWindows.Num() - 1);
		}

		FVector2D NewSize(static_cast<double>(Width), static_cast<double>(Height));
		FVector2D NewPosition = FVector2D::ZeroVector;
		if (bMaximize)
		{
			// Resize to the primary work area rather than calling SWindow::Maximize(), which needs a native
			// window and so does nothing under -RenderOffScreen.
			FDisplayMetrics DisplayMetrics;
			FDisplayMetrics::RebuildDisplayMetrics(DisplayMetrics);
			const FPlatformRect& WorkArea = DisplayMetrics.PrimaryDisplayWorkAreaRect;
			NewSize.X = WorkArea.Right - WorkArea.Left;
			NewSize.Y = WorkArea.Bottom - WorkArea.Top;
			// The work area excludes taskbars, so its origin is not (0, 0) when one is on the top or left.
			NewPosition = FVector2D(WorkArea.Left, WorkArea.Top);
		}

		if (NewSize.X < 1.0 || NewSize.Y < 1.0)
		{
			return TEXT("Error: resize needs a positive Width and Height.");
		}

		// ReshapeWindow rather than Resize, so the window is also moved back on screen.
		const TSharedRef<SWindow>& Window = VisibleWindows[Index];
		Window->ReshapeWindow(bMaximize ? NewPosition : Window->GetPositionInScreen(), NewSize);

		const FVector2D Applied = Window->GetSizeInScreen();
		return FString::Printf(TEXT("{\"width\": %d, \"height\": %d}"),
			FMath::RoundToInt32(Applied.X), FMath::RoundToInt32(Applied.Y));
	}

	return TEXT("Error: Unknown action. Use 'list', 'select', 'close', 'resize' or 'maximize'.");
}

// -- Wait tool --------------------------------------------------------

bool USlateInspectorToolset59::WaitFor(const FString& Text, const FString& TextGone)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	const bool bWaitForAppear = !Text.IsEmpty();
	const bool bWaitForDisappear = !TextGone.IsEmpty();

	if (!bWaitForAppear && !bWaitForDisappear)
	{
		return true;
	}

	// Walk the widget tree looking for text without rendering a full snapshot,
	// which would be unnecessary overhead for a simple text presence check.
	bool bFoundText = false;
	bool bFoundTextGone = false;

	TArray<TSharedRef<SWindow>> Windows;
	FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);

	TFunction<void(TSharedRef<SWidget>)> Walk = [&](TSharedRef<SWidget> Widget)
	{
		// Early out if both conditions already determined.
		if ((!bWaitForAppear || bFoundText) && (!bWaitForDisappear || bFoundTextGone))
		{
			return;
		}

		FString WidgetText;
		static const FName TextBlockType(TEXT("STextBlock"));
		if (Widget->GetType() == TextBlockType)
		{
			WidgetText = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
		}
#if WITH_ACCESSIBILITY
		if (WidgetText.IsEmpty())
		{
			WidgetText = Widget->GetAccessibleText(EAccessibleType::Main).ToString();
		}
#endif

		if (!WidgetText.IsEmpty())
		{
			if (bWaitForAppear && WidgetText.Contains(Text))
			{
				bFoundText = true;
			}
			if (bWaitForDisappear && WidgetText.Contains(TextGone))
			{
				bFoundTextGone = true;
			}
		}

		FChildren* Children = UE::SlateInspectorToolset::Private::GetChildrenToWalk(*Widget);
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			Walk(Children->GetChildAt(Index));
		}
	};

	for (const TSharedRef<SWindow>& Window : Windows)
	{
		Walk(Window);
	}

	bool bAppearConditionMet = !bWaitForAppear || bFoundText;
	bool bDisappearConditionMet = !bWaitForDisappear || !bFoundTextGone;

	return bAppearConditionMet && bDisappearConditionMet;
}

// -- Form tool --------------------------------------------------------

bool USlateInspectorToolset59::FillForm(const TArray<FSlateInspectorToolsetFormField59>& Fields)
{
	bool bAllSuccess = true;

	for (const FSlateInspectorToolsetFormField59& Field : Fields)
	{
		bool bFieldSuccess = false;

		if (Field.FieldType.Equals(TEXT("textbox"), ESearchCase::IgnoreCase))
		{
			bFieldSuccess = Type(Field.Ref, Field.Value, false);
		}
		else if (Field.FieldType.Equals(TEXT("checkbox"), ESearchCase::IgnoreCase))
		{
			// Click the checkbox if the desired state differs from the current state.
			TSharedPtr<SWidget> Widget = ResolveRefOrWarn(Field.Ref);
			static const FName CheckBoxType(TEXT("SCheckBox"));
			if (Widget.IsValid() && Widget->GetType() == CheckBoxType)
			{
				TSharedRef<SCheckBox> CheckBox = StaticCastSharedRef<SCheckBox>(Widget.ToSharedRef());
				bool bDesiredChecked = Field.Value.Equals(TEXT("true"), ESearchCase::IgnoreCase);
				bool bCurrentlyChecked = CheckBox->IsChecked();
				if (bDesiredChecked != bCurrentlyChecked)
				{
					bFieldSuccess = Click(Field.Ref);
				}
				else
				{
					bFieldSuccess = true; // Already in desired state.
				}
			}
			else if (Widget.IsValid())
			{
				UE_LOGFMT(LogSlateInspectorToolset, Warning, "SlateInspectorToolset: Widget '{Ref}' is not an SCheckBox", Field.Ref);
			}
		}
		else if (Field.FieldType.Equals(TEXT("combobox"), ESearchCase::IgnoreCase))
		{
			bFieldSuccess = SelectOption(Field.Ref, Field.Value);
		}
		else
		{
			UE_LOGFMT(LogSlateInspectorToolset, Warning, "SlateInspectorToolset: Unknown field type '{FieldType}'", Field.FieldType);
		}

		if (!bFieldSuccess)
		{
			bAllSuccess = false;
		}
	}

	return bAllSuccess;
}

bool USlateInspectorToolset59::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}
