// Copyright Epic Games, Inc. All Rights Reserved.

#include "SSlateInspectorToolsetInputRecorder.h"

#include "Internationalization/Text.h"
#include "Widgets/Text/STextBlock.h"

const TCHAR* SSlateInspectorToolsetInputRecorder::GetLabel()
{
	return TEXT("Input Recorder");
}

void SSlateInspectorToolsetInputRecorder::Construct(const FArguments& InArgs)
{
	const FText Label = FText::FromString(GetLabel());

	ChildSlot
	.HAlign(HAlign_Center)
	.VAlign(VAlign_Center)
	[
		SNew(STextBlock)
		.Text(Label)
	];

#if WITH_ACCESSIBILITY
	SetAccessibleBehavior(EAccessibleBehavior::Custom, Label);
#endif
}

const FSlateInspectorToolsetRecordedInput& SSlateInspectorToolsetInputRecorder::GetRecorded() const
{
	return Recorded;
}

void SSlateInspectorToolsetInputRecorder::Reset()
{
	Recorded = FSlateInspectorToolsetRecordedInput();
}

void SSlateInspectorToolsetInputRecorder::SetHandledKeyEvents(ESlateInspectorToolsetKeyEvents InHandledKeyEvents)
{
	HandledKeyEvents = InHandledKeyEvents;
}

FReply SSlateInspectorToolsetInputRecorder::GetKeyReply(ESlateInspectorToolsetKeyEvents KeyEvent) const
{
	return EnumHasAnyFlags(HandledKeyEvents, KeyEvent) ? FReply::Handled() : FReply::Unhandled();
}

bool SSlateInspectorToolsetInputRecorder::SupportsKeyboardFocus() const
{
	return true;
}

FReply SSlateInspectorToolsetInputRecorder::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	Recorded.LastMouseButtonDown = MouseEvent.GetEffectingButton();
	++Recorded.MouseButtonDownCount;
	return FReply::Handled()
		.DetectDrag(SharedThis(this), MouseEvent.GetEffectingButton())
		.CaptureMouse(SharedThis(this));
}

FReply SSlateInspectorToolsetInputRecorder::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	Recorded.LastMouseButtonUp = MouseEvent.GetEffectingButton();
	++Recorded.MouseButtonUpCount;
	return FReply::Handled().ReleaseMouseCapture();
}

FReply SSlateInspectorToolsetInputRecorder::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const FVector2D ScreenPosition = MouseEvent.GetScreenSpacePosition();
	Recorded.LastMouseMovePosition = ScreenPosition;
	++Recorded.MouseMoveCount;
	if (!MouseEvent.GetPressedButtons().IsEmpty())
	{
		Recorded.PressedMovePositions.Add(ScreenPosition);
	}
	return FReply::Handled();
}

void SSlateInspectorToolsetInputRecorder::OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	SCompoundWidget::OnMouseEnter(MyGeometry, MouseEvent);
	Recorded.bMouseInside = true;
	++Recorded.MouseEnterCount;
}

void SSlateInspectorToolsetInputRecorder::OnMouseLeave(const FPointerEvent& MouseEvent)
{
	SCompoundWidget::OnMouseLeave(MouseEvent);
	Recorded.bMouseInside = false;
	++Recorded.MouseLeaveCount;
}

FReply SSlateInspectorToolsetInputRecorder::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	Recorded.LastWheelDelta = MouseEvent.GetWheelDelta();
	Recorded.LastWheelModifiers = MouseEvent.GetModifierKeys();
	++Recorded.MouseWheelCount;
	return FReply::Handled();
}

FReply SSlateInspectorToolsetInputRecorder::OnDragDetected(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	Recorded.LastDragButton = MouseEvent.GetEffectingButton();
	++Recorded.DragDetectedCount;
	return FReply::Handled();
}

FReply SSlateInspectorToolsetInputRecorder::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	Recorded.LastKeyDown = InKeyEvent.GetKey();
	Recorded.bLastKeyDownWasRepeat = InKeyEvent.IsRepeat();
	Recorded.LastKeyDownModifiers = InKeyEvent.GetModifierKeys();
	Recorded.LastKeyUserIndex = InKeyEvent.GetUserIndex();
	++Recorded.KeyDownCount;
	return GetKeyReply(ESlateInspectorToolsetKeyEvents::KeyDown);
}

FReply SSlateInspectorToolsetInputRecorder::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	Recorded.LastKeyUp = InKeyEvent.GetKey();
	Recorded.LastKeyUpModifiers = InKeyEvent.GetModifierKeys();
	Recorded.LastKeyUserIndex = InKeyEvent.GetUserIndex();
	++Recorded.KeyUpCount;
	return GetKeyReply(ESlateInspectorToolsetKeyEvents::KeyUp);
}

FReply SSlateInspectorToolsetInputRecorder::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent)
{
	Recorded.LastKeyChar = InCharacterEvent.GetCharacter();
	++Recorded.KeyCharCount;
	return GetKeyReply(ESlateInspectorToolsetKeyEvents::KeyChar);
}

FReply SSlateInspectorToolsetInputRecorder::OnAnalogValueChanged(const FGeometry& MyGeometry, const FAnalogInputEvent& InAnalogInputEvent)
{
	Recorded.LastAnalogKey = InAnalogInputEvent.GetKey();
	Recorded.LastAnalogValue = InAnalogInputEvent.GetAnalogValue();
	Recorded.LastAnalogUserIndex = InAnalogInputEvent.GetUserIndex();
	++Recorded.AnalogEventCount;
	return FReply::Handled();
}

FReply SSlateInspectorToolsetInputRecorder::OnTouchGesture(const FGeometry& MyGeometry, const FPointerEvent& GestureEvent)
{
	Recorded.LastGestureType = GestureEvent.GetGestureType();
	Recorded.LastGestureDelta = GestureEvent.GetGestureDelta();
	++Recorded.TouchGestureCount;
	return FReply::Handled();
}

FReply SSlateInspectorToolsetInputRecorder::OnTouchStarted(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent)
{
	Recorded.LastTouchPointerIndex = InTouchEvent.GetPointerIndex();
	Recorded.LastTouchPosition = InTouchEvent.GetScreenSpacePosition();
	++Recorded.TouchStartCount;
	return FReply::Handled();
}

FReply SSlateInspectorToolsetInputRecorder::OnTouchMoved(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent)
{
	Recorded.LastTouchPointerIndex = InTouchEvent.GetPointerIndex();
	Recorded.LastTouchPosition = InTouchEvent.GetScreenSpacePosition();
	++Recorded.TouchMoveCount;
	return FReply::Handled();
}

FReply SSlateInspectorToolsetInputRecorder::OnTouchEnded(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent)
{
	Recorded.LastTouchPointerIndex = InTouchEvent.GetPointerIndex();
	Recorded.LastTouchPosition = InTouchEvent.GetScreenSpacePosition();
	++Recorded.TouchEndCount;
	return FReply::Handled();
}
