// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "GenericPlatform/GenericApplicationMessageHandler.h"
#include "Input/Events.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"
#include "Math/Vector2D.h"
#include "Misc/EnumClassFlags.h"
#include "Widgets/SCompoundWidget.h"

/** Key events an SSlateInspectorToolsetInputRecorder handles; it records the ones it declines as well. */
enum class ESlateInspectorToolsetKeyEvents : uint8
{
	None = 0,
	KeyDown = 1 << 0,
	KeyChar = 1 << 1,
	KeyUp = 1 << 2,
	All = KeyDown | KeyChar | KeyUp,
};
ENUM_CLASS_FLAGS(ESlateInspectorToolsetKeyEvents);

/** Everything the recorder has observed since construction or the last Reset(). */
struct FSlateInspectorToolsetRecordedInput
{
	// Pointer
	FKey LastMouseButtonDown;
	FKey LastMouseButtonUp;
	int32 MouseButtonDownCount = 0;
	int32 MouseButtonUpCount = 0;
	int32 MouseMoveCount = 0;
	FVector2D LastMouseMovePosition = FVector2D::ZeroVector;
	/** Tracks OnMouseEnter/OnMouseLeave independently of SWidget's own hover state. */
	bool bMouseInside = false;
	int32 MouseEnterCount = 0;
	int32 MouseLeaveCount = 0;
	int32 MouseWheelCount = 0;
	float LastWheelDelta = 0.0f;
	FModifierKeysState LastWheelModifiers;
	int32 TouchGestureCount = 0;
	EGestureEvent LastGestureType = EGestureEvent::None;
	FVector2D LastGestureDelta = FVector2D::ZeroVector;

	// Drag
	int32 DragDetectedCount = 0;
	FKey LastDragButton;
	/** Screen-space position of every OnMouseMove that arrived with at least one button held. */
	TArray<FVector2D> PressedMovePositions;

	// Keys
	FKey LastKeyDown;
	FKey LastKeyUp;
	int32 KeyDownCount = 0;
	int32 KeyUpCount = 0;
	bool bLastKeyDownWasRepeat = false;
	FModifierKeysState LastKeyDownModifiers;
	FModifierKeysState LastKeyUpModifiers;
	int32 LastKeyUserIndex = INDEX_NONE;
	TCHAR LastKeyChar = 0;
	int32 KeyCharCount = 0;

	// Analog
	FKey LastAnalogKey;
	float LastAnalogValue = 0.0f;
	int32 AnalogEventCount = 0;
	int32 LastAnalogUserIndex = INDEX_NONE;

	// Touch
	int32 TouchStartCount = 0;
	int32 TouchMoveCount = 0;
	int32 TouchEndCount = 0;
	int32 LastTouchPointerIndex = INDEX_NONE;
	FVector2D LastTouchPosition = FVector2D::ZeroVector;
};

/**
 * Focusable widget that handles every pointer, key, analog and touch event and records the last of each kind for tests.
 * SetHandledKeyEvents makes it decline key events, which it still records, so they bubble on up its focus path.
 */
class SSlateInspectorToolsetInputRecorder : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SSlateInspectorToolsetInputRecorder) {}
	SLATE_END_ARGS()

	/** Label shown in the widget and reported to snapshots. */
	static const TCHAR* GetLabel();

	void Construct(const FArguments& InArgs);

	const FSlateInspectorToolsetRecordedInput& GetRecorded() const;

	/** Forgets everything recorded so far. */
	void Reset();

	/** Key events to handle from now on; All by default. */
	void SetHandledKeyEvents(ESlateInspectorToolsetKeyEvents InHandledKeyEvents);

	//~ Begin SWidget Interface
	virtual bool SupportsKeyboardFocus() const override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnDragDetected(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent) override;
	virtual FReply OnAnalogValueChanged(const FGeometry& MyGeometry, const FAnalogInputEvent& InAnalogInputEvent) override;
	virtual FReply OnTouchGesture(const FGeometry& MyGeometry, const FPointerEvent& GestureEvent) override;
	virtual FReply OnTouchStarted(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent) override;
	virtual FReply OnTouchMoved(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent) override;
	virtual FReply OnTouchEnded(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent) override;
	//~ End SWidget Interface

private:

	FReply GetKeyReply(ESlateInspectorToolsetKeyEvents KeyEvent) const;

	FSlateInspectorToolsetRecordedInput Recorded;

	ESlateInspectorToolsetKeyEvents HandledKeyEvents = ESlateInspectorToolsetKeyEvents::All;
};
