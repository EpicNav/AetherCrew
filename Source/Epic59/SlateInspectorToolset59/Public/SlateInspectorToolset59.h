// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "EditorSubsystem.h"
#include "Input/Events.h"
#include "SlateInspectorToolsetTypes.h"
#include "ToolsetCore/ToolsetProvider.h"
#include "ToolsetRegistry/ToolsetImage.h"
#include "ToolsetRegistry/ToolsetPermissionProvider.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: the toolset is a UToolsetDefinition here (see below)
#include "Widgets/SWidget.h"

#include "SlateInspectorToolset59.generated.h"

class FGenericWindow;
class FSubsystemCollectionBase;
class SWindow;

/** One enclosing UUserWidget in a resolved widget's outer chain. */
USTRUCT(BlueprintType)
struct FSlateInspectorToolsetUMGOuter
{
	GENERATED_BODY()

	/** UUserWidget object name. */
	UPROPERTY(BlueprintReadOnly, Category = "SlateInspectorToolset")
	FString Name;

	/** UUserWidget class name. */
	UPROPERTY(BlueprintReadOnly, Category = "SlateInspectorToolset")
	FString Class;
};

/**
 * Result of GetUMGInfo: the UMG UWidget behind a Slate ref, plus a live handle.
 *
 * The toolset registry serializes Widget as a {"refPath": "..."} handle, which
 * the agent can pass to ObjectTools.list_properties / get_properties /
 * set_properties to read or write the widget's reflected UPROPERTYs.
 */
USTRUCT(BlueprintType)
struct FSlateInspectorToolsetUMGInfoResult
{
	GENERATED_BODY()

	/** Error message when resolution failed; empty on success. */
	UPROPERTY(BlueprintReadOnly, Category = "SlateInspectorToolset")
	FString Error;

	/** The live UI-tech object wrapping the Slate widget (a UMG UWidget in practice).
	 *  Non-null exactly when Error is empty. Serialized as a {refPath} handle for use
	 *  with ObjectTools property accessors, which reflect over the instance's own class. */
	UPROPERTY(BlueprintReadOnly, Category = "SlateInspectorToolset")
	TObjectPtr<UObject> Widget = nullptr;

	/** UWidget name in its hierarchy, e.g. "SaveButton". */
	UPROPERTY(BlueprintReadOnly, Category = "SlateInspectorToolset")
	FString WidgetName;

	/** UWidget class name, e.g. "Button". */
	UPROPERTY(BlueprintReadOnly, Category = "SlateInspectorToolset")
	FString WidgetClass;

	/** Owning WidgetBlueprint asset path; empty when unknown. */
	UPROPERTY(BlueprintReadOnly, Category = "SlateInspectorToolset")
	FString Asset;

	/** "self" when the widget itself carried UMG metadata, "parent" when an ancestor did. */
	UPROPERTY(BlueprintReadOnly, Category = "SlateInspectorToolset")
	FString ResolvedVia;

	/** Enclosing UUserWidget chain, innermost first. */
	UPROPERTY(BlueprintReadOnly, Category = "SlateInspectorToolset")
	TArray<FSlateInspectorToolsetUMGOuter> OuterUserWidgets;
};

/** Describes a single form field for the FillForm tool. */
USTRUCT(BlueprintType)
struct FSlateInspectorToolsetFormField59
{
	GENERATED_BODY()

	/** The ref identifier of the form field widget. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FString Ref;

	/** The value to set. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FString Value;

	/** The type of field: "textbox", "checkbox", or "combobox". */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FString FieldType;
};

/**
 * Slate UI automation toolset.
 *
 * Exposes snapshot, screenshot, and interaction tools for driving the
 * Unreal Editor UI programmatically.  Registered via UToolsetRegistry
 * so the ModelContextProtocol plugin picks them up automatically.
 *
 * A shallow root observer (depth 0) continuously tracks top-level
 * windows.  Before working with a specific window or panel, call
 * Observe() on it to get deep widget coverage, then Unobserve() when
 * done.  Observers walk their subtree every ~100ms, assigning refs to
 * newly appeared widgets and keeping the ref cache current.
 *
 * Input simulation uses direct Slate event APIs (ProcessKeyCharEvent,
 * ProcessMouseButtonDownEvent, etc.).
 *
 * The one-shot action tools (Click, Hover, Scroll, Drag) leave no input held.
 * MouseDown, TouchStart and KeyDown do, so each needs its matching release;
 * ReleaseHeldInput drains whatever is left.
 */
// UE 5.8: 5.9's registry takes any IToolsetProvider, so 5.9 makes this class the editor subsystem too. 5.8's
// UToolsetRegistry::RegisterToolsetClass takes UToolsetDefinition classes only, so the tools stay here and the
// subsystem half (registration, observer ticking, PIE clean-up) moves to USlateInspectorToolset59Subsystem below.
UCLASS(BlueprintType, MinimalAPI)
class USlateInspectorToolset59 : public UToolsetDefinition, public IToolsetProvider // UE 5.8: was UEditorSubsystem
{
	GENERATED_BODY()

public:

	/** Capture a Slate UI accessibility snapshot.  Use this to read the current
	 * widget tree and discover refs for action tools (Click, Type, Hover, etc.).
	 * A shallow root observer (depth 0) covers top-level windows automatically.
	 * Before interacting with a specific window or panel, call Observe() on it
	 * to get deep coverage, then Snapshot that subtree to see its contents.
	 * Refs discovered by a previous Snapshot remain usable. You do NOT need to
	 * call Snapshot again before every action.
	 * Each [pos=X,Y size=W,H] is the widget's on-screen bounds in desktop pixels, whatever the DPI scale or rotation,
	 * the same space as the pointer and touch tools' screen positions and offsets.
	 * @param Ref                      Subtree root ref. Empty = all windows.
	 * @param MaxDepth                 Maximum depth (default 30); negative values count as 0.
	 * @param bIncludeSourceLocations  Include [src=File:Line] widget creation tags (default false); true bypasses snapshot caches. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API FString Snapshot(const FString& Ref, int32 MaxDepth = 30, bool bIncludeSourceLocations = false);

	/** Resolve a Slate widget ref back to the UMG UWidget that created it (for UMG
	 * and PIE UIs). Returns the UWidget name, its UClass, the owning WidgetBlueprint
	 * asset path, the enclosing UUserWidget chain, and a live Widget handle.
	 * "resolvedVia" is "self" when the widget itself carries UMG metadata, or
	 * "parent" when an ancestor did.
	 * On failure Error is set and Widget is null: either the widget is pure Slate
	 * with no UMG owner, or its authoring UWidget has since been destroyed. The
	 * remaining metadata fields are still filled in for the latter case.
	 * @param Ref  Slate widget ref from Snapshot. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API FSlateInspectorToolsetUMGInfoResult GetUMGInfo(const FString& Ref);

	/** Register an observer on a widget subtree so its refs are continuously
	 * kept up to date (~100ms tick).  Call this on the window or panel you are
	 * about to work with. It ensures new widgets appearing in that subtree
	 * are assigned refs automatically.  Unobserve when you are done.
	 * A shallow root observer (depth 0) already covers top-level windows.
	 * @param Ref       Root widget ref to observe. Empty = all visible windows.
	 * @param MaxDepth  Maximum depth to walk from the root (default 30); negative values count as 0. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API FString Observe(const FString& Ref, int32 MaxDepth = 30);

	/** Remove an observer by its identifier.
	 * @param Identifier  The identifier returned by Observe(). */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool Unobserve(const FString& Identifier);

	/** List all active observers as a JSON array for debugging.
	 * Each entry includes the observer identifier, whether it is the root observer,
	 * the root widget ref (if any), max depth, and cached snapshot size. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API FString ListObservers();

	/** Screenshot a Slate widget or an editor window. Prefer this over
	 * SceneTools.take_screenshot for Editor UI; use SceneTools only for 3D viewport.
	 * Pass a window ref from Windows("list") to capture a specific window; with no ref
	 * it captures the active window, falling back to the main editor window when nothing
	 * is active. A widget partly outside its window is cropped to the part inside it. Fails with an error naming the
	 * reason when there is nothing to capture: an unknown ref, a widget in no visible window, or one entirely outside it.
	 * @param Ref  Slate widget or window ref. Empty = active window, else main window. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API FToolsetImage Screenshot(const FString& Ref);

	// -- Action tools --

	/** Click a Slate widget identified by its ref.
	 * Returns false without clicking when the target point would not receive the event --
	 * something is on top of it, the widget has no arranged size, or MouseDown still holds the
	 * button. The log and LastRefusal() name the reason. True means the event was delivered to
	 * the widget's branch, not that the widget handled it.
	 * @param Ref     Slate widget ref.
	 * @param Params  Button, double-click, modifiers and position; omitted fields keep their defaults. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool Click(const FString& Ref, const FSlateInspectorToolsetClickParams& Params = FSlateInspectorToolsetClickParams());

	/** Hover over a Slate widget, triggering any hover state or tooltip.
	 * Returns false without hovering when the target point is covered or the widget is unarranged.
	 * Unlike the other action tools, Hover moves the OS cursor, since hover state only lasts while
	 * the pointer stays over the widget. On Mac, Click, Drag and Scroll move it too and leave it
	 * on the target, because Mac only routes pointer input to the window under the real cursor.
	 * A button held by MouseDown stays held.
	 * @param Ref     Slate widget ref.
	 * @param Params  Position to hover; omitted fields keep their defaults. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool Hover(const FString& Ref, const FSlateInspectorToolsetHoverParams& Params = FSlateInspectorToolsetHoverParams());

	/** Report whether a widget is reachable by pointer input, without changing anything.
	 * Returns JSON {reached, samples, reachedSamples, x, y, reason}, using the same hit test Click,
	 * Hover, Drag and Scroll refuse on; `reason` names the obstruction.
	 * Grid > 1 samples a Grid x Grid lattice inset inside the widget instead of a single point.
	 * `reachedSamples` counts the samples that hit, and x/y locate the first sample that missed.
	 * @param Ref     Slate widget ref.
	 * @param Params  Position and grid size; omitted fields keep their defaults. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API FString HitTest(const FString& Ref, const FSlateInspectorToolsetHitTestParams& Params = FSlateInspectorToolsetHitTestParams());

	/** Scroll the mouse wheel over a Slate widget.
	 * Hit-tested like Click; returns false without scrolling if the point is covered or unarranged.
	 * @param Ref     Slate widget ref to scroll over. The event routes to whatever is under that
	 *                point handles scrolling, which is usually an ancestor of the ref.
	 * @param Params  Ticks, gesture deltas, modifiers and position; omitted fields keep their defaults. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool Scroll(const FString& Ref, const FSlateInspectorToolsetScrollParams& Params = FSlateInspectorToolsetScrollParams());

	/** Why the last action tool refused, as JSON {seq, tool, target, reason}; seq is 0 when nothing
	 * has refused this session. seq increments on each refusal, so sample it before an action and
	 * compare afterwards to tell a new refusal from one already read.
	 * Click, Hover, Scroll, Drag, Type, Focus, SelectOption, PressKey, KeyDown and KeyUp record
	 * refusals, and they record one every time they return false unless Slate is not running. For
	 * PressKey that includes a key that was sent but that nothing handled. Read-only tools such as
	 * Snapshot and HitTest never change it. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API FString LastRefusal();

	/** Move the pointer to an absolute screen position; a button held by MouseDown stays held, so MouseDown/MouseMove/MouseUp drags.
	 * @param X          Horizontal screen position in pixels; positive is right.
	 * @param Y          Vertical screen position in pixels; positive is down.
	 * @param Modifiers  Modifier keys held during the move; none by default. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool MouseMove(double X, double Y, const FSlateInspectorToolsetModifierKeys59& Modifiers = FSlateInspectorToolsetModifierKeys59());

	/** Move the pointer to a widget's center plus an offset; a held button stays held. Use MouseMove for an absolute position.
	 * @param Ref        Slate widget ref.
	 * @param OffsetX    Horizontal offset from the widget center in screen pixels; positive moves right (default 0).
	 * @param OffsetY    Vertical offset from the widget center in screen pixels; positive moves down (default 0).
	 * @param Modifiers  Modifier keys held during the move; none by default. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool MouseMoveTo(const FString& Ref, double OffsetX = 0.0, double OffsetY = 0.0, const FSlateInspectorToolsetModifierKeys59& Modifiers = FSlateInspectorToolsetModifierKeys59());

	/** Press a mouse button at a widget's center and leave it held; idempotent for a button this widget already holds.
	 * MouseUp or ReleaseHeldInput releases it, as does the hold watchdog after SlateInspectorToolset.HeldInputTimeoutSeconds without a pointer, touch, KeyDown or KeyUp call.
	 * @param Ref        Slate widget ref.
	 * @param Button     "left", "right", or "middle" (default "left").
	 * @param Modifiers  Modifier keys held during the press; none by default. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool MouseDown(const FString& Ref, const FString& Button = TEXT("left"), const FSlateInspectorToolsetModifierKeys59& Modifiers = FSlateInspectorToolsetModifierKeys59());

	/** Release a mouse button at the pointer, to its captor or whatever lies under it; safe without a prior MouseDown.
	 * @param Button     "left", "right", or "middle" (default "left").
	 * @param Modifiers  Modifier keys held during the release; none by default. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool MouseUp(const FString& Button = TEXT("left"), const FSlateInspectorToolsetModifierKeys59& Modifiers = FSlateInspectorToolsetModifierKeys59());

	/** Release every button MouseDown still holds, lift every finger TouchStart still holds and release every key KeyDown still holds, returning how many; buttons the user holds are left alone.
	 * Also run by the hold watchdog after SlateInspectorToolset.HeldInputTimeoutSeconds (default 100) without a pointer, touch, KeyDown or KeyUp call, at PIE end and at shutdown. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API int32 ReleaseHeldInput();

	/** Put a finger down and leave it there; it shares the left mouse button with the cursor, so do not mix it with a held MouseDown.
	 * TouchEnd or ReleaseHeldInput lifts it, as does the hold watchdog after SlateInspectorToolset.HeldInputTimeoutSeconds without a pointer, touch, KeyDown or KeyUp call.
	 * @param Ref         Slate widget ref; empty to touch an absolute screen position.
	 * @param X           Offset in screen pixels from the widget center, or the absolute screen X when Ref is empty (default 0).
	 * @param Y           Offset in screen pixels from the widget center, or the absolute screen Y when Ref is empty (default 0).
	 * @param TouchIndex  Finger index 0..9 (default 0); several fingers can be down at once on different indices. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool TouchStart(const FString& Ref, double X = 0.0, double Y = 0.0, int32 TouchIndex = 0);

	/** Move a finger held by an earlier TouchStart to an absolute screen position.
	 * @param X           Horizontal screen position in pixels; positive is right.
	 * @param Y           Vertical screen position in pixels; positive is down.
	 * @param TouchIndex  Finger index 0..9 matching the TouchStart call (default 0). */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool TouchMove(double X, double Y, int32 TouchIndex = 0);

	/** Lift a finger held by an earlier TouchStart at an absolute screen position.
	 * @param X           Horizontal screen position in pixels; positive is right.
	 * @param Y           Vertical screen position in pixels; positive is down.
	 * @param TouchIndex  Finger index 0..9 matching the TouchStart call (default 0). */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool TouchEnd(double X, double Y, int32 TouchIndex = 0);

	/** Type text into a Slate text input widget, focusing it first; use PressKey for keys that are not characters.
	 * @param Ref    Slate textbox ref.
	 * @param Text   Text to type.
	 * @param Submit Press Enter after typing (default false). */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool Type(const FString& Ref, const FString& Text, bool Submit = false);

	/** Press and release a key on the focused Slate widget; gamepad keys route as controller input for the target user.
	 * Modifier keys held by KeyDown are down for the press, as with "Ctrl+" prefixes.
	 * True means some handler consumed the press, an auto-repeat, its char event or the release, not that the UI
	 * changed as intended. Any focusable widget handles Tab, arrow and D-pad keys as a navigation request, so true for
	 * them does not mean focus moved; compare Snapshot or HitTest before and after. False means nothing handled the key,
	 * or that nothing was sent because the key, user or RepeatCount is invalid; LastRefusal() names the reason, including
	 * which widget had focus. In PIE, false can also mean the game viewport passed the key to the game and the game did
	 * not consume it. Only BackSpace, Tab, Enter and Escape send a character; use Type to enter text.
	 * @param Key          FKey name with optional "Shift+", "Ctrl+", "Alt+", "Cmd+" prefixes, e.g. "Enter", "Ctrl+A", "Gamepad_DPad_Up".
	 * @param UserIndex    Slate user to send to (default -1 = default controller or keyboard user); an explicit index must exist.
	 * @param RepeatCount  Extra auto-repeat presses between the press and the release, 0..1000 (default 0). */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool PressKey(const FString& Key, int32 UserIndex = -1, int32 RepeatCount = 0);

	/** Press a key and leave it held; pair with KeyUp. Same key specs and gamepad routing as PressKey.
	 * A held modifier key (LeftShift, RightControl, LeftAlt, LeftCommand, ...) is down for every later key event of the same
	 * Slate user until released, by KeyUp or by a PressKey of the same key. It does not apply to pointer tools, which take
	 * their own Modifiers, nor to the platform modifier state FSlateApplication::GetModifierKeys reports.
	 * KeyUp or ReleaseHeldInput releases it, as does the hold watchdog after SlateInspectorToolset.HeldInputTimeoutSeconds without a pointer, touch, KeyDown or KeyUp call.
	 * Returns true once the press is sent, whether or not a widget handled it, as a held modifier is normally unhandled.
	 * False means nothing was sent because the key or user is invalid; LastRefusal() names the reason.
	 * @param Key        Key spec with optional modifier prefixes, e.g. "LeftShift", "Ctrl+A", "Gamepad_LeftShoulder".
	 * @param UserIndex  Slate user to send to (default -1 = default controller or keyboard user); an explicit index must exist. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool KeyDown(const FString& Key, int32 UserIndex = -1);

	/** Release a key held by an earlier KeyDown for the same Slate user. Same key specs and gamepad routing as PressKey.
	 * Returns true once the release is sent, whether or not a widget handled it, as most widgets act on the press.
	 * False means nothing was sent because the key or user is invalid; LastRefusal() names the reason.
	 * @param Key        Key spec with optional modifier prefixes, matching the KeyDown call.
	 * @param UserIndex  Slate user to send to (default -1 = default controller or keyboard user); an explicit index must exist. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool KeyUp(const FString& Key, int32 UserIndex = -1);

	/** Send an analog axis value along the target user's focus path; 1D axes only, send 0 afterwards to release.
	 * @param Key        FKey name of a 1D axis, e.g. "Gamepad_LeftX", "Gamepad_RightY", "Gamepad_LeftTriggerAxis".
	 * @param Value      Axis value, delivered unclamped; sticks -1..1, triggers 0..1 on hardware.
	 * @param UserIndex  Slate user to send to (default -1 = user mapped to the default controller); an explicit index must exist. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool AnalogInput(const FString& Key, double Value, int32 UserIndex = -1);

	/** Set Slate keyboard/navigation focus to a widget, e.g. before gamepad navigation in PIE.
	 * @param Ref        Slate widget ref.
	 * @param UserIndex  Slate user index (default 0, the primary player); must be 0 or greater. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool Focus(const FString& Ref, int32 UserIndex = 0);

	/** Select an option in a Slate combobox by its text label.
	 * Opens the dropdown, finds the matching text, and clicks it.
	 * @param Ref    Slate combobox ref.
	 * @param Value  Exact option text to select. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool SelectOption(const FString& Ref, const FString& Value);

	/** Drag from one Slate widget to another (mouse down, stepped move, release).
	 * Both ends are hit-tested first; returns false without dragging if either is covered
	 * or unarranged.
	 * The pointer is stepped along the path, so drag detection fires partway and the target
	 * receives OnDragOver, including once at the drop point. Waypoints bend that path, for
	 * gestures a straight line would not produce.
	 * Start and end positions are normalized within each widget and default to the center. Use them to
	 * aim at edge-band drop zones, e.g. EndPosition.Y = 0.1 to drop against the target's top edge.
	 * Leave EndRef empty and pass MoveBy or Waypoints to drag StartRef by a distance instead of onto
	 * another widget, e.g. splitters, sliders, scrollbars and divider grips. EndPosition is then
	 * ignored, and a path that never leaves the press point is refused since the drag would not move.
	 * @param StartRef  Slate widget ref for the drag source.
	 * @param EndRef    Slate widget ref for the drop target; empty means "drag StartRef itself".
	 * @param Params    Modifiers, button, waypoints, start/end positions and MoveBy; omitted fields keep their defaults. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool Drag(const FString& StartRef, const FString& EndRef,
		const FSlateInspectorToolsetDragParams& Params = FSlateInspectorToolsetDragParams());

	/** List, select, close or resize top-level Slate editor windows.
	 * "list" returns a JSON array of {index, ref, title, active, regular, x, y, width, height}.
	 * Pass the ref to Screenshot or Snapshot to target that window; the index is positional
	 * and changes as windows open and close, the ref does not.
	 * "resize" and "maximize" also work under -RenderOffScreen.
	 * @param Action  "list" returns JSON array, "select" brings to front, "close" destroys,
	 *                "resize" applies Width/Height, "maximize" fills the primary work area.
	 * @param Index   Window index for select/close/resize/maximize. Ignored when Ref is given.
	 * @param Width   New width in screen pixels, for "resize".
	 * @param Height  New height in screen pixels, for "resize".
	 * @param Ref     Window ref from "list"; takes precedence over Index. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API FString Windows(const FString& Action = TEXT("list"), int32 Index = -1,
		int32 Width = 0, int32 Height = 0, const FString& Ref = TEXT(""));

	/** Check once whether text is present or absent in the Slate widget tree (substring match); poll to wait.
	 * @param Text       Text that must be present (default empty = skip).
	 * @param TextGone   Text that must be absent (default empty = skip). */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool WaitFor(const FString& Text = TEXT(""), const FString& TextGone = TEXT(""));

	/** Fill multiple Slate form fields at once.
	 * @param Fields  Array of {Ref, Value, FieldType} where FieldType is
	 *                "textbox", "checkbox", or "combobox". */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "SlateInspectorToolset")
	static SLATEINSPECTORTOOLSET59_API bool FillForm(const TArray<FSlateInspectorToolsetFormField59>& Fields);

	//~ Begin IToolsetProvider Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End IToolsetProvider Interface

private:

	/** Resolve a ref to a widget, logging a warning if it fails. */
	static TSharedPtr<SWidget> ResolveRefOrWarn(const FString& Ref);

	/** Resolve a ref for an action tool, recording a refusal for LastRefusal() if it fails. */
	static TSharedPtr<SWidget> ResolveRefOrRefuse(const FString& Tool, const FString& Ref);

	/** Log a refusal and store it for LastRefusal(). */
	static void RecordRefusal(const FString& Tool, const FString& Target, const FString& Reason);

	/** Find the window containing a widget, logging a warning if there is none. */
	static TSharedPtr<SWindow> FindWidgetWindowOrWarn(TSharedRef<SWidget> Widget);

	/** Resolve a pointer tool's target: widget center plus offset for a non-empty Ref, else the offset as an absolute position. */
	static bool ResolvePointerTarget(const FString& Ref, double OffsetX, double OffsetY, TSharedPtr<SWidget>& OutWidget, FVector2D& OutScreenPosition);

	/** Map a button name ("left", "right", "middle") to an FKey; EKeys::Invalid, after a warning, for anything else. */
	static FKey ParseMouseButtonKey(const FString& Button);

	/** False when Slate reports MouseButton as already held, since a one-shot press would then route to the captor. */
	static bool IsMouseButtonFree(FKey MouseButton);

	/**
	 * Simulate a mouse click at a normalized point inside a widget; (0.5, 0.5) is its center.
	 * @param Tool    Tool name recorded if the click is refused, e.g. "click" or "select".
	 * @param Target  Ref recorded as the target if the click is refused.
	 */
	static bool SimulateClick(const FString& Tool, const FString& Target, TSharedRef<SWidget> Widget, FKey MouseButton, bool bDoubleClick,
		const FModifierKeysState& ModifierKeys = FModifierKeysState(),
		const FSlateInspectorToolsetNormalizedPoint& Position = FSlateInspectorToolsetNormalizedPoint());

	/** Move Slate's pointer and the OS cursor together, routing the matching move event; PressedButtons is the set the event reports as held. */
	static void SimulateHeldMouseMove(const FVector2D& ScreenPosition, const FVector2D& LastScreenPosition, const TSet<FKey>& PressedButtons, const FModifierKeysState& ModifierKeys);

	/** SimulateHeldMouseMove carrying Slate's held buttons; redirects PIE capture to TargetWidget only while no button is held. */
	static void SimulateRedirectedMouseMove(TSharedPtr<SWidget> TargetWidget, const FVector2D& ScreenPosition, const FVector2D& LastScreenPosition, const FModifierKeysState& ModifierKeys);

	/** Route a mouse button press at a screen-space position into Window and record the button as held. */
	static void SimulateHeldMouseButtonDown(TSharedRef<SWindow> Window, const FVector2D& ScreenPosition, FKey MouseButton, const FModifierKeysState& ModifierKeys);

	/** Route a mouse button release at a screen-space position and drop the button from the held set. */
	static void SimulateHeldMouseButtonUp(const FVector2D& ScreenPosition, FKey MouseButton, const FModifierKeysState& ModifierKeys);

	/** A key spec ("Ctrl+A", "Gamepad_LeftX") parsed and resolved to the device and Slate user its events are stamped with. */
	struct FResolvedKeyInput
	{
		FKey Key;
		FModifierKeysState ModifierKeys;
		FInputDeviceId DeviceId;
		int32 SlateUserIndex = INDEX_NONE;
	};

	/** Split "Ctrl+Shift+KeyName" into FKey and modifier state; false, with OutInvalidReason set, when the key name is unknown. */
	static bool ParseKeyWithModifiers(const FString& KeySpec, FKey& OutKey, FModifierKeysState& OutModifierKeys, FString& OutInvalidReason);

	/** Resolve the device and Slate user for Key; a negative UserIndex means the default controller's or keyboard's user. False, with OutInvalidReason set, when there is no such user. */
	static bool ResolveInputUser(const FKey& Key, int32 UserIndex, FInputDeviceId& OutDeviceId, int32& OutSlateUserIndex, FString& OutInvalidReason);

	/** The default input device, written to OutDeviceId, and the Slate user mapped to it; unset when no user is mapped. */
	static TOptional<int32> GetDefaultInputDeviceUser(FInputDeviceId& OutDeviceId);

	/** ParseKeyWithModifiers followed by ResolveInputUser; false, with OutInvalidReason set, when either fails. */
	static bool ResolveKeySpec(const FString& KeySpec, int32 UserIndex, FResolvedKeyInput& OutInput, FString& OutInvalidReason);

	/** The device and Slate user every event of one touch is stamped with. */
	struct FResolvedTouchInput
	{
		FInputDeviceId DeviceId;
		int32 SlateUserIndex = INDEX_NONE;
	};

	/** The three phases a finger goes through, each routed through its own FSlateApplication::ProcessTouch*Event. */
	enum class ETouchPhase : uint8
	{
		Start,
		Move,
		End,
	};

	/** Range-check TouchIndex (below the cursor's pointer index) and resolve the device and Slate user its events carry. */
	static bool ResolveTouchInput(int32 TouchIndex, FResolvedTouchInput& OutInput);

	/** Route one touch event as the platform touch path does, moving the cursor first; PlatformWindow is used by Start only. */
	static void SimulateTouch(ETouchPhase Phase, const FResolvedTouchInput& Input, int32 TouchIndex, const FVector2D& ScreenPosition, const FVector2D& LastScreenPosition, const TSharedPtr<FGenericWindow>& PlatformWindow = nullptr);

	/** Restart the hold watchdog's inactivity clock, starting the watchdog when a button, finger or key is held and it is not yet running. */
	static void NoteHeldInputActivity();

	/** Core ticker callback: releases held input once the inactivity timeout passes; false stops the ticker once nothing is held. */
	static bool TickHeldInputWatchdog(float DeltaTime);

	/**
	 * Route one key press or release, with the modifier keys KeyDown holds down, returning whether any handler consumed it;
	 * BackSpace, Tab, Enter and Escape presses also send the char event the OS would, and a handled char counts.
	 */
	static bool SendKeyEvent(const FResolvedKeyInput& Input, bool bDown, bool bRepeat);
};

/** UE 5.8: the UEditorSubsystem half of 5.9's USlateInspectorToolset (see above); its code is 5.9's, unchanged. */
UCLASS(MinimalAPI)
class USlateInspectorToolset59Subsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:

	SLATEINSPECTORTOOLSET59_API virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	SLATEINSPECTORTOOLSET59_API virtual void Deinitialize() override;

	/** Register or unregister the toolset at runtime. */
	void SetToolsetEnabled(bool bEnabled);

private:

	/** Forwarded from FSlateApplication::OnPostTick to drive observer updates. */
	void OnSlatePostTick(float DeltaTime);

	/** Purges refs to widgets destroyed when a PIE session ends, so stale refs fail fast. */
	void OnEndPIE(bool bIsSimulating);

	bool bToolsetRegistered = false;

	FDelegateHandle PostTickDelegateHandle;

	FDelegateHandle EndPIEDelegateHandle;

	FString RootObserverIdentifier;
};
