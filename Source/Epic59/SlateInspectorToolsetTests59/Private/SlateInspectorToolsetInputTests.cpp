// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"
#include "Components/CQTestSlateComponent.h"
#include "SSlateInspectorToolsetInputRecorder.h"
#include "SlateInspectorToolsetTestPanel.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/IConsoleManager.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"
#include "Layout/WidgetPath.h"
#include "SlateInspectorToolset59.h"
#include "SlateInspectorToolsetObserverManager.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SWindow.h"

namespace SlateInspectorToolsetInputTestPrivate
{

/**
 * Ensures a Slate user exists at UserIndex for the scope; a pre-existing user is left alone, one created here is unregistered.
 * Slate only creates users while handling input, so an unhandled reply is processed on the user's behalf to register it.
 */
struct FScopedSlateUser
{
	explicit FScopedSlateUser(int32 InUserIndex)
		: UserIndex(InUserIndex)
		, bCreated(!FSlateApplication::Get().GetUser(InUserIndex).IsValid())
	{
		// A non-cursor pointer index keeps the reply off the branch that would synthesize a mouse event.
		constexpr int32 NonCursorPointerIndex = 0;
		FSlateApplication::Get().ProcessExternalReply(FWidgetPath(), FReply::Unhandled(), UserIndex, NonCursorPointerIndex);
	}

	~FScopedSlateUser()
	{
		if (bCreated && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterUser(UserIndex);
		}
	}

	FScopedSlateUser(const FScopedSlateUser&) = delete;
	FScopedSlateUser& operator=(const FScopedSlateUser&) = delete;

private:

	int32 UserIndex;
	bool bCreated;
};

} // namespace SlateInspectorToolsetInputTestPrivate

using SlateInspectorToolsetInputTestPrivate::FScopedSlateUser;

/**
 * Input simulation tools exercised against the test panel's input recorder and scroll box.
 * Lives in its own window so it can coexist with the general toolset tests in one session.
 */
TEST_CLASS(SlateInspectorToolsetInputTest, "AIAssistant.Toolsets.SlateInspectorToolset.Input")
{
	// -- Fixture -----------------------------------------------------

	static constexpr const TCHAR* WindowTitle = TEXT("SlateInspectorToolset Input Test Window");

	/** Slate user the multi-user gamepad tests target, standing in for a second local player. */
	static constexpr int32 SecondaryUserIndex = 1;

	TUniquePtr<FCQTestSlateComponent> SlateComponent;
	TSharedPtr<SWindow> TestWindow;
	TSharedPtr<SSlateInspectorToolsetTestPanel> TestPanel;

	/** Fingers StartTouch put down that EndTouch has not lifted; AFTER_EACH lifts whatever a failed assertion left behind. */
	TSet<int32> HeldTouchIndices;

	/** The hold watchdog timeout in force before the test, restored by AFTER_EACH after a test shortens it. */
	float SavedHeldInputTimeoutSeconds = 0.0f;

	BEFORE_EACH()
	{
		SlateComponent = MakeUnique<FCQTestSlateComponent>();
		SavedHeldInputTimeoutSeconds = GetHeldInputTimeoutVariable()->GetFloat();

		TestPanel = SNew(SSlateInspectorToolsetTestPanel);

		TestWindow = SNew(SWindow)
			.Title(FText::FromString(WindowTitle))
			.ScreenPosition(FVector2D(100, 100))
			.ClientSize(FVector2D(400, 600))
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.FocusWhenFirstShown(true)
			[
				TestPanel.ToSharedRef()
			];

		FSlateApplication::Get().AddWindow(TestWindow.ToSharedRef());
		TestWindow->MoveWindowTo(FVector2D(100, 100));
		TestWindow->BringToFront();

		// Force layout so widgets have valid geometry before any interaction.
		FSlateApplication::Get().Tick();
	}

	AFTER_EACH()
	{
		// A test that failed between MouseDown and MouseUp would otherwise hand the held button to the next test.
		for (const FKey& PressedButton : TSet<FKey>(FSlateApplication::Get().GetPressedMouseButtons()))
		{
			USlateInspectorToolset59::MouseUp(GetButtonName(PressedButton));
		}

		// After the button sweep, since a touch end also drops the left button from Slate's pressed set.
		if (TestPanel.IsValid() && TestPanel->GetInputRecorder().IsValid())
		{
			const FVector2D RecorderCenter = GetInputRecorderCenter();
			for (int32 TouchIndex : HeldTouchIndices)
			{
				USlateInspectorToolset59::TouchEnd(RecorderCenter.X, RecorderCenter.Y, TouchIndex);
			}
		}
		HeldTouchIndices.Reset();

		// Keys a failed test left held by KeyDown would otherwise be down for the next test's key events.
		USlateInspectorToolset59::ReleaseHeldInput();

		GetHeldInputTimeoutVariable()->Set(SavedHeldInputTimeoutSeconds, ECVF_SetByCode);

		// Clean up any observers created during the test (except the root observer).
		for (const FSlateInspectorToolsetObserver& Observer : FSlateInspectorToolsetObserverManager::Get().GetObservers())
		{
			if (!Observer.bRoot)
			{
				FSlateInspectorToolsetObserverManager::Get().RemoveObserver(Observer.Identifier);
			}
		}

		if (TestWindow.IsValid())
		{
			TestWindow->RequestDestroyWindow();
			TestWindow.Reset();
		}
		TestPanel.Reset();
		SlateComponent.Reset();
	}

	/** Snapshot only the test window's subtree so editor widgets never match a lookup. */
	FString SnapshotTestWindow()
	{
		const FString FullSnapshot = USlateInspectorToolset59::Snapshot(TEXT(""), 30);
		const FString WindowRef = FindRefByRole(FullSnapshot, TEXT("window"), WindowTitle);
		if (WindowRef.IsEmpty())
		{
			return FString();
		}
		return USlateInspectorToolset59::Snapshot(WindowRef, 30);
	}

	FString FindRefByRole(const FString& Snapshot, const FString& Role, const FString& Label)
	{
		TArray<FString> Lines;
		Snapshot.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			FString Trimmed = Line.TrimStartAndEnd();
			if (!Trimmed.StartsWith(Role))
			{
				continue;
			}
			if (!Label.IsEmpty() && !Trimmed.Contains(Label))
			{
				continue;
			}
			int32 RefStart = Trimmed.Find(TEXT("[ref="));
			if (RefStart != INDEX_NONE)
			{
				RefStart += 5;
				int32 RefEnd = Trimmed.Find(TEXT("]"), ESearchCase::IgnoreCase, ESearchDir::FromStart, RefStart);
				if (RefEnd != INDEX_NONE)
				{
					return Trimmed.Mid(RefStart, RefEnd - RefStart);
				}
			}
		}
		return FString();
	}

	/** Ref of the input recorder widget in the test window. */
	FString FindInputRecorderRef(const FString& Snapshot)
	{
		return FindRefByRole(Snapshot, TEXT("generic"), SSlateInspectorToolsetInputRecorder::GetLabel());
	}

	/** Focus the recorder for UserIndex; returns its ref, or empty when it could not be found or focused. */
	FString FocusInputRecorder(int32 UserIndex = 0)
	{
		const FString RecorderRef = FindInputRecorderRef(SnapshotTestWindow());
		if (RecorderRef.IsEmpty() || !USlateInspectorToolset59::Focus(RecorderRef, UserIndex))
		{
			return FString();
		}
		return RecorderRef;
	}

	/** The seq field of LastRefusal(), which counts refusals. */
	static int32 GetRefusalSeq()
	{
		FString AfterSeq;
		USlateInspectorToolset59::LastRefusal().Split(TEXT("\"seq\":"), nullptr, &AfterSeq);
		return FCString::Atoi(*AfterSeq);
	}

	static bool IsLastRefusalFrom(const TCHAR* Tool, const FString& Target)
	{
		const FString Refusal = USlateInspectorToolset59::LastRefusal();
		return Refusal.Contains(FString::Printf(TEXT("\"tool\":\"%s\""), Tool), ESearchCase::CaseSensitive)
			&& Refusal.Contains(FString::Printf(TEXT("\"target\":\"%s\""), *Target), ESearchCase::CaseSensitive);
	}

	/** Lowest Slate user index at or above SecondaryUserIndex + 1 with no registered user; INDEX_NONE when every probed slot is taken. */
	static int32 FindUnregisteredUserIndex()
	{
		constexpr int32 FirstCandidate = SecondaryUserIndex + 1;
		constexpr int32 LastCandidate = 31;
		for (int32 Candidate = FirstCandidate; Candidate <= LastCandidate; ++Candidate)
		{
			if (!FSlateApplication::Get().GetUser(Candidate).IsValid())
			{
				return Candidate;
			}
		}
		return INDEX_NONE;
	}

	/** Screen-space center of a widget, the point ref-targeted tools act at. */
	static FVector2D GetScreenCenter(TSharedRef<SWidget> Widget)
	{
		const FGeometry& Geometry = Widget->GetTickSpaceGeometry();
		return FVector2D(Geometry.GetAbsolutePositionAtCoordinates(FVector2D(0.5, 0.5)));
	}

	/** Screen-space center of the recorder, the point every recorder-targeted tool starts from. */
	FVector2D GetInputRecorderCenter() const
	{
		return GetScreenCenter(TestPanel->GetInputRecorder().ToSharedRef());
	}

	/** TouchStart that records the finger it put down so AFTER_EACH can lift it if the test stops early. */
	bool StartTouch(const FString& Ref, double X = 0.0, double Y = 0.0, int32 TouchIndex = 0)
	{
		const bool bStarted = USlateInspectorToolset59::TouchStart(Ref, X, Y, TouchIndex);
		if (bStarted)
		{
			HeldTouchIndices.Add(TouchIndex);
		}
		return bStarted;
	}

	/** TouchEnd for a finger StartTouch put down. */
	bool EndTouch(double X, double Y, int32 TouchIndex = 0)
	{
		HeldTouchIndices.Remove(TouchIndex);
		return USlateInspectorToolset59::TouchEnd(X, Y, TouchIndex);
	}

	/** The console variable holding the hold watchdog timeout, which a test shortens to make the watchdog fire within a few frames. */
	static IConsoleVariable* GetHeldInputTimeoutVariable()
	{
		return IConsoleManager::Get().FindConsoleVariable(TEXT("SlateInspectorToolset59.HeldInputTimeoutSeconds"));
	}

	/** The tool-facing name of a mouse button key, for handing a Slate-reported button back to MouseUp. */
	static FString GetButtonName(const FKey& MouseButton)
	{
		if (MouseButton == EKeys::RightMouseButton)
		{
			return TEXT("right");
		}
		if (MouseButton == EKeys::MiddleMouseButton)
		{
			return TEXT("middle");
		}
		return TEXT("left");
	}

	static bool IsNearlyAt(const FVector2D& Actual, const FVector2D& Expected)
	{
		// Pointer events carry float positions, so allow for the round trip from double.
		return Actual.Equals(Expected, 1.0);
	}

	static bool IsScrollOffsetAtStart(float ScrollOffset)
	{
		// SScrollBox clamps its offset round trip only on the negative side, so a sub-pixel residual can remain.
		return FMath::IsNearlyZero(ScrollOffset, 0.5f);
	}

	static FSlateInspectorToolsetPoint MakePoint(const FVector2D& Position)
	{
		FSlateInspectorToolsetPoint Point;
		Point.X = Position.X;
		Point.Y = Position.Y;
		return Point;
	}

	// -- MouseMove / MouseMoveTo tests -------------------------------

	TEST_METHOD(MouseMove_AbsoluteUpdatesHover)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const TSharedPtr<SSlateInspectorToolsetInputRecorder> Recorder = TestPanel->GetInputRecorder();
				ASSERT_THAT(IsTrue(Recorder.IsValid()));

				const FVector2D RecorderCenter = GetInputRecorderCenter();
				const bool bMovedIn = USlateInspectorToolset59::MouseMove(RecorderCenter.X, RecorderCenter.Y);
				ASSERT_THAT(IsTrue(bMovedIn));
				ASSERT_THAT(IsTrue(Recorder->GetRecorded().bMouseInside));
				ASSERT_THAT(IsTrue(Recorder->GetRecorded().MouseEnterCount == 1));
				ASSERT_THAT(IsTrue(Recorder->IsHovered()));
				ASSERT_THAT(IsTrue(IsNearlyAt(Recorder->GetRecorded().LastMouseMovePosition, RecorderCenter)));

				// Moving onto another widget in the same window must take hover away again.
				const FVector2D ButtonCenter = GetScreenCenter(TestPanel->GetTestButton().ToSharedRef());
				const bool bMovedOut = USlateInspectorToolset59::MouseMove(ButtonCenter.X, ButtonCenter.Y);
				ASSERT_THAT(IsTrue(bMovedOut));
				ASSERT_THAT(IsTrue(!Recorder->GetRecorded().bMouseInside));
				ASSERT_THAT(IsTrue(Recorder->GetRecorded().MouseLeaveCount == 1));
				ASSERT_THAT(IsTrue(!Recorder->IsHovered()));
				ASSERT_THAT(IsTrue(TestPanel->GetTestButton()->IsHovered()));
			});
	}

	TEST_METHOD(MouseMoveTo_OffsetFromCenter)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const FVector2D Offset(10.0, 5.0);
				const bool bMoved = USlateInspectorToolset59::MouseMoveTo(RecorderRef, Offset.X, Offset.Y);
				ASSERT_THAT(IsTrue(bMoved));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.MouseMoveCount == 1));
				ASSERT_THAT(IsTrue(IsNearlyAt(Recorded.LastMouseMovePosition, GetInputRecorderCenter() + Offset)));
				ASSERT_THAT(IsTrue(Recorded.bMouseInside));
			});
	}

	TEST_METHOD(MouseMoveTo_InvalidRef)
	{
		const bool bMoved = USlateInspectorToolset59::MouseMoveTo(TEXT("nonexistent_ref"));
		ASSERT_THAT(IsTrue(!bMoved));
	}

	// -- MouseDown / MouseUp tests -----------------------------------

	TEST_METHOD(MouseDown_ButtonReportsPressed)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				const TSharedPtr<SButton> TestButton = TestPanel->GetTestButton();
				ASSERT_THAT(IsTrue(TestButton.IsValid()));
				ASSERT_THAT(IsTrue(!TestButton->IsPressed()));

				const bool bPressed = USlateInspectorToolset59::MouseDown(ButtonRef);
				ASSERT_THAT(IsTrue(bPressed));
				// Held, not clicked: pressed state shown, button reported down, no click yet.
				ASSERT_THAT(IsTrue(TestButton->IsPressed()));
				ASSERT_THAT(IsTrue(FSlateApplication::Get().GetPressedMouseButtons().Contains(EKeys::LeftMouseButton)));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 0));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseUp()));
				ASSERT_THAT(IsTrue(!FSlateApplication::Get().GetPressedMouseButtons().Contains(EKeys::LeftMouseButton)));
			});
	}

	TEST_METHOD(MouseUp_AfterDownFiresClickOnce)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseDown(ButtonRef)));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 0));

				const bool bReleased = USlateInspectorToolset59::MouseUp();
				ASSERT_THAT(IsTrue(bReleased));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 1));
				ASSERT_THAT(IsTrue(!TestPanel->GetTestButton()->IsPressed()));
			});
	}

	TEST_METHOD(MouseDown_PreciseClickButton_UpInPlaceClicks)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Precise Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseDown(ButtonRef)));
				ASSERT_THAT(IsTrue(TestPanel->GetPreciseButton()->IsPressed()));
				ASSERT_THAT(IsTrue(TestPanel->PreciseButtonClickCount == 0));

				// PreciseClick never captures, so the release only reaches the button if nothing took capture away from under the cursor.
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseUp()));
				ASSERT_THAT(IsTrue(TestPanel->PreciseButtonClickCount == 1));
				ASSERT_THAT(IsTrue(!TestPanel->GetPreciseButton()->IsPressed()));
			});
	}

	TEST_METHOD(MouseDown_PreciseClickButton_MoveAwayCancelsClick)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Precise Button"));
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseDown(ButtonRef)));
				ASSERT_THAT(IsTrue(TestPanel->GetPreciseButton()->IsPressed()));

				// A precise press cancels through OnMouseLeave, which only fires while the move still hit-tests instead of going to a captor.
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseMoveTo(RecorderRef)));
				ASSERT_THAT(IsTrue(!TestPanel->GetPreciseButton()->IsPressed()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseUp()));
				ASSERT_THAT(IsTrue(TestPanel->PreciseButtonClickCount == 0));
			});
	}

	TEST_METHOD(MouseDown_RightButtonRecorded)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const bool bPressed = USlateInspectorToolset59::MouseDown(RecorderRef, TEXT("right"));
				ASSERT_THAT(IsTrue(bPressed));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastMouseButtonDown == EKeys::RightMouseButton));
				ASSERT_THAT(IsTrue(Recorded.MouseButtonDownCount == 1));
				ASSERT_THAT(IsTrue(Recorded.MouseButtonUpCount == 0));
				ASSERT_THAT(IsTrue(FSlateApplication::Get().GetPressedMouseButtons().Contains(EKeys::RightMouseButton)));

				const bool bReleased = USlateInspectorToolset59::MouseUp(TEXT("right"));
				ASSERT_THAT(IsTrue(bReleased));
				ASSERT_THAT(IsTrue(Recorded.LastMouseButtonUp == EKeys::RightMouseButton));
				ASSERT_THAT(IsTrue(Recorded.MouseButtonUpCount == 1));
				ASSERT_THAT(IsTrue(!FSlateApplication::Get().GetPressedMouseButtons().Contains(EKeys::RightMouseButton)));
			});
	}

	TEST_METHOD(MouseUp_WithoutDownDoesNotClick)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseMoveTo(ButtonRef)));
				ASSERT_THAT(IsTrue(TestPanel->GetTestButton()->IsHovered()));

				// A release with no press behind it is a recovery path: it succeeds without clicking.
				const bool bReleased = USlateInspectorToolset59::MouseUp();
				ASSERT_THAT(IsTrue(bReleased));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 0));
			});
	}

	TEST_METHOD(MouseDown_TwiceThenUp_ClicksOnce)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseDown(ButtonRef)));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseDown(ButtonRef)));
				ASSERT_THAT(IsTrue(TestPanel->GetTestButton()->IsPressed()));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 0));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseUp()));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 1));
				ASSERT_THAT(IsTrue(!TestPanel->GetTestButton()->IsPressed()));
			});
	}

	TEST_METHOD(MouseUp_UnknownButtonReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseMoveTo(RecorderRef)));

				const bool bReleased = USlateInspectorToolset59::MouseUp(TEXT("thumb"));
				ASSERT_THAT(IsTrue(!bReleased));
				// Nothing was sent for the unknown name.
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().MouseButtonUpCount == 0));
			});
	}

	TEST_METHOD(ReleaseHeldInput_ReleasesHeldButtonAndFinger)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FindInputRecorderRef(SnapshotTestWindow());
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				// The right button keeps the held mouse button apart from the left button a touch shares with the cursor.
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseDown(RecorderRef, TEXT("right"))));
				ASSERT_THAT(IsTrue(StartTouch(RecorderRef)));

				const int32 ReleasedCount = USlateInspectorToolset59::ReleaseHeldInput();
				HeldTouchIndices.Reset();
				ASSERT_THAT(IsTrue(ReleasedCount == 2));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastMouseButtonUp == EKeys::RightMouseButton));
				ASSERT_THAT(IsTrue(Recorded.MouseButtonUpCount == 1));
				ASSERT_THAT(IsTrue(Recorded.TouchEndCount == 1));
				ASSERT_THAT(IsTrue(IsNearlyAt(Recorded.LastTouchPosition, GetInputRecorderCenter())));
				ASSERT_THAT(IsTrue(FSlateApplication::Get().GetPressedMouseButtons().IsEmpty()));

				// Nothing left to release, so a second sweep is a no-op rather than a spurious release.
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::ReleaseHeldInput() == 0));
				ASSERT_THAT(IsTrue(Recorded.MouseButtonUpCount == 1));
			});
	}

	TEST_METHOD(HeldInputWatchdog_ReleasesButtonAfterTimeout)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString ButtonRef = FindRefByRole(SnapshotTestWindow(), TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				GetHeldInputTimeoutVariable()->Set(0.01f, ECVF_SetByCode);
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::MouseDown(ButtonRef)));
				ASSERT_THAT(IsTrue(TestPanel->GetTestButton()->IsPressed()));
			})
			// The watchdog polls once per frame, so the release lands a frame or two after the shortened timeout elapses.
			.Until([this]() { return !FSlateApplication::Get().GetPressedMouseButtons().Contains(EKeys::LeftMouseButton); })
			.Then([this]()
			{
				// The release lands on the button the press captured, completing a click.
				ASSERT_THAT(IsTrue(!TestPanel->GetTestButton()->IsPressed()));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 1));
			});
	}
	// -- Drag tests --------------------------------------------------

	TEST_METHOD(Drag_RightButtonRecorded)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				FSlateInspectorToolsetDragParams Params;
				Params.Button = TEXT("right");
				const bool bDragged = USlateInspectorToolset59::Drag(RecorderRef, ButtonRef, Params);
				ASSERT_THAT(IsTrue(bDragged));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastMouseButtonDown == EKeys::RightMouseButton));
				ASSERT_THAT(IsTrue(Recorded.LastMouseButtonUp == EKeys::RightMouseButton));
				ASSERT_THAT(IsTrue(Recorded.MouseButtonDownCount == 1));
				ASSERT_THAT(IsTrue(Recorded.MouseButtonUpCount == 1));
				// The recorder requests drag detection on press, so one held move past the threshold means one OnDragDetected.
				ASSERT_THAT(IsTrue(Recorded.DragDetectedCount == 1));
			});
	}

	TEST_METHOD(Drag_UnknownButton_ReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				FSlateInspectorToolsetDragParams Params;
				Params.Button = TEXT("thumb");
				const bool bDragged = USlateInspectorToolset59::Drag(RecorderRef, RecorderRef, Params);
				ASSERT_THAT(IsTrue(!bDragged));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().MouseButtonDownCount == 0));
			});
	}

	TEST_METHOD(Drag_EmptyEndRef_EndsAtLastWaypoint)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const FVector2D Center = GetInputRecorderCenter();
				const FVector2D Offset(30.0, -20.0);
				FSlateInspectorToolsetDragParams Params;
				Params.Button = TEXT("middle");
				Params.Waypoints.Points.Add(MakePoint(Offset));

				const bool bDragged = USlateInspectorToolset59::Drag(RecorderRef, FString(), Params);
				ASSERT_THAT(IsTrue(bDragged));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastMouseButtonDown == EKeys::MiddleMouseButton));
				ASSERT_THAT(IsTrue(Recorded.LastMouseButtonUp == EKeys::MiddleMouseButton));
				ASSERT_THAT(IsTrue(Recorded.MouseButtonDownCount == 1));
				ASSERT_THAT(IsTrue(Recorded.MouseButtonUpCount == 1));
				// With no EndRef the single waypoint is the whole path, so the release lands exactly Offset from the start.
				ASSERT_THAT(IsTrue(Recorded.PressedMovePositions.Num() >= 1));
				ASSERT_THAT(IsTrue(IsNearlyAt(Recorded.PressedMovePositions.Last(), Center + Offset)));
			});
	}

	TEST_METHOD(Drag_RightButtonOffsetScrollsScrollBox)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RowRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Row 3"));
				ASSERT_THAT(IsTrue(!RowRef.IsEmpty()));

				const TSharedPtr<SScrollBox> ScrollBox = TestPanel->GetScrollBox();
				ASSERT_THAT(IsTrue(ScrollBox.IsValid()));
				ASSERT_THAT(IsTrue(IsScrollOffsetAtStart(ScrollBox->GetScrollOffset())));

				// Right-drag scrolling is opt-in on SScrollBox; dragging content upward scrolls further into the list.
				ScrollBox->SetScrollBarRightClickDragAllowed(true);
				// The scroll box only captures the pointer once a right-drag move reaches it, so the single move must end inside the box.
				FSlateInspectorToolsetDragParams Params;
				Params.Button = TEXT("right");
				Params.Waypoints.Points.Add(MakePoint(FVector2D(0.0, -40.0)));
				const bool bDragged = USlateInspectorToolset59::Drag(RowRef, FString(), Params);
				ASSERT_THAT(IsTrue(bDragged));
				ASSERT_THAT(IsTrue(ScrollBox->GetScrollOffset() > 0.0f));
			});
	}

	TEST_METHOD(Drag_InvalidStartRef)
	{
		const bool bDragged = USlateInspectorToolset59::Drag(TEXT("nonexistent_ref"), FString());
		ASSERT_THAT(IsTrue(!bDragged));
	}

	TEST_METHOD(Drag_NoMovement_ReturnsFalseWithoutClick)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				// A drag that goes nowhere must not degrade into a press/release on the widget, with or without a zero waypoint.
				ASSERT_THAT(IsTrue(!USlateInspectorToolset59::Drag(RecorderRef, FString())));
				FSlateInspectorToolsetDragParams Params;
				Params.Waypoints.Points.Add(MakePoint(FVector2D::ZeroVector));
				ASSERT_THAT(IsTrue(!USlateInspectorToolset59::Drag(RecorderRef, FString(), Params)));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.MouseButtonDownCount == 0));
				ASSERT_THAT(IsTrue(Recorded.MouseButtonUpCount == 0));
			});
	}

	// -- Scroll tests ------------------------------------------------

	TEST_METHOD(Scroll_ChangesScrollBoxOffset)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				// The first row lies fully inside the viewport, so the wheel lands within the scroll box's clip.
				const FString RowRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Row 1"));
				ASSERT_THAT(IsTrue(!RowRef.IsEmpty()));

				const TSharedPtr<SScrollBox> ScrollBox = TestPanel->GetScrollBox();
				ASSERT_THAT(IsTrue(ScrollBox.IsValid()));
				ASSERT_THAT(IsTrue(IsScrollOffsetAtStart(ScrollBox->GetScrollOffset())));

				// The row ignores the wheel, so it bubbles to the scroll box, which scrolls against the sign.
				FSlateInspectorToolsetScrollParams Params;
				Params.Ticks = -3;
				const bool bScrolled = USlateInspectorToolset59::Scroll(RowRef, Params);
				ASSERT_THAT(IsTrue(bScrolled));
				ASSERT_THAT(IsTrue(ScrollBox->GetScrollOffset() > 0.0f));
			});
	}

	TEST_METHOD(Scroll_HorizontalDeltaSendsGesture)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const double DeltaX = 12.0;
				FSlateInspectorToolsetScrollParams Params;
				Params.Ticks = 0;
				Params.GestureX = static_cast<float>(DeltaX);
				const bool bScrolled = USlateInspectorToolset59::Scroll(RecorderRef, Params);
				ASSERT_THAT(IsTrue(bScrolled));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.TouchGestureCount >= 1));
				ASSERT_THAT(IsTrue(Recorded.LastGestureType == EGestureEvent::Scroll));
				ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(Recorded.LastGestureDelta.X, DeltaX)));
				// Without a vertical delta the gesture rides a zero-delta wheel event, which is never a notch.
				ASSERT_THAT(IsTrue(Recorded.MouseWheelCount == 0));
			});
	}

	TEST_METHOD(Scroll_InvalidRef)
	{
		FSlateInspectorToolsetScrollParams Params;
		Params.Ticks = -1;
		const bool bScrolled = USlateInspectorToolset59::Scroll(TEXT("nonexistent_ref"), Params);
		ASSERT_THAT(IsTrue(!bScrolled));
	}

	TEST_METHOD(Scroll_WithModifiersRecorded)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				FSlateInspectorToolsetScrollParams Params;
				Params.Ticks = -1;
				Params.Modifiers.bShift = true;
				const bool bScrolled = USlateInspectorToolset59::Scroll(RecorderRef, Params);
				ASSERT_THAT(IsTrue(bScrolled));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.MouseWheelCount == 1));
				ASSERT_THAT(IsTrue(Recorded.LastWheelDelta == -1.0f));
				ASSERT_THAT(IsTrue(Recorded.LastWheelModifiers.IsShiftDown()));
				ASSERT_THAT(IsTrue(!Recorded.LastWheelModifiers.IsControlDown()));
			});
	}

	// -- KeyDown / KeyUp tests ---------------------------------------

	TEST_METHOD(KeyDown_RecordedWithoutKeyUp)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const bool bPressed = USlateInspectorToolset59::KeyDown(TEXT("A"));
				ASSERT_THAT(IsTrue(bPressed));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastKeyDown == EKeys::A));
				ASSERT_THAT(IsTrue(Recorded.KeyDownCount == 1));
				// The key stays held: no release has been sent yet.
				ASSERT_THAT(IsTrue(Recorded.KeyUpCount == 0));
				ASSERT_THAT(IsTrue(!Recorded.bLastKeyDownWasRepeat));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyUp(TEXT("A"))));
			});
	}

	TEST_METHOD(KeyUp_CompletesPress)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyDown(TEXT("A"))));
				const bool bReleased = USlateInspectorToolset59::KeyUp(TEXT("A"));
				ASSERT_THAT(IsTrue(bReleased));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastKeyUp == EKeys::A));
				ASSERT_THAT(IsTrue(Recorded.KeyDownCount == 1));
				ASSERT_THAT(IsTrue(Recorded.KeyUpCount == 1));
			});
	}

	TEST_METHOD(KeyDown_ModifierPrefixApplied)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const bool bPressed = USlateInspectorToolset59::KeyDown(TEXT("Ctrl+Shift+A"));
				ASSERT_THAT(IsTrue(bPressed));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastKeyDown == EKeys::A));
				ASSERT_THAT(IsTrue(Recorded.LastKeyDownModifiers.IsControlDown()));
				ASSERT_THAT(IsTrue(Recorded.LastKeyDownModifiers.IsShiftDown()));
				ASSERT_THAT(IsTrue(!Recorded.LastKeyDownModifiers.IsAltDown()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyUp(TEXT("Ctrl+Shift+A"))));
			});
	}

	TEST_METHOD(KeyDown_UnknownKeyReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const bool bPressed = USlateInspectorToolset59::KeyDown(TEXT("NotAKeyName"));
				ASSERT_THAT(IsTrue(!bPressed));
				// Nothing was sent for the unknown name.
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().KeyDownCount == 0));
				ASSERT_THAT(IsTrue(IsLastRefusalFrom(TEXT("keydown"), TEXT("NotAKeyName"))));
			});
	}

	TEST_METHOD(KeyDown_UnknownUserIndexReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const int32 UnmappedUserIndex = FindUnregisteredUserIndex();
				ASSERT_THAT(IsTrue(UnmappedUserIndex != INDEX_NONE));

				// Slate would register a user for any index a key event names, so the tool refuses instead.
				const bool bPressed = USlateInspectorToolset59::KeyDown(TEXT("A"), UnmappedUserIndex);
				ASSERT_THAT(IsTrue(!bPressed));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().KeyDownCount == 0));
				ASSERT_THAT(IsTrue(!FSlateApplication::Get().GetUser(UnmappedUserIndex).IsValid()));
				ASSERT_THAT(IsTrue(IsLastRefusalFrom(TEXT("keydown"), TEXT("A"))));
			});
	}

	TEST_METHOD(KeyDown_GamepadUserIndex1Routed)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				FScopedSlateUser SecondUser(SecondaryUserIndex);
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::Focus(RecorderRef, SecondaryUserIndex)));

				const bool bPressed = USlateInspectorToolset59::KeyDown(TEXT("Gamepad_FaceButton_Bottom"), SecondaryUserIndex);
				ASSERT_THAT(IsTrue(bPressed));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastKeyDown == EKeys::Gamepad_FaceButton_Bottom));
				ASSERT_THAT(IsTrue(Recorded.LastKeyUserIndex == SecondaryUserIndex));
				ASSERT_THAT(IsTrue(Recorded.KeyDownCount == 1));
				ASSERT_THAT(IsTrue(Recorded.KeyUpCount == 0));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyUp(TEXT("Gamepad_FaceButton_Bottom"), SecondaryUserIndex)));
				ASSERT_THAT(IsTrue(Recorded.LastKeyUp == EKeys::Gamepad_FaceButton_Bottom));
				ASSERT_THAT(IsTrue(Recorded.KeyUpCount == 1));
			});
	}

	TEST_METHOD(KeyDown_UnhandledReturnsTrue)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));
				TestPanel->GetInputRecorder()->SetHandledKeyEvents(ESlateInspectorToolsetKeyEvents::None);

				// A held key is often unhandled, as a modifier is, so a declined press is not a refusal.
				const int32 RefusalSeqBefore = GetRefusalSeq();
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyDown(TEXT("A"))));
				ASSERT_THAT(IsTrue(GetRefusalSeq() == RefusalSeqBefore));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.KeyDownCount == 1));
				ASSERT_THAT(IsTrue(Recorded.LastKeyDown == EKeys::A));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyUp(TEXT("A"))));
			});
	}

	TEST_METHOD(KeyUp_UnhandledReturnsTrue)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));
				TestPanel->GetInputRecorder()->SetHandledKeyEvents(ESlateInspectorToolsetKeyEvents::KeyDown);

				// Most widgets act on the press, so a declined release is not a refusal.
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyDown(TEXT("A"))));
				const int32 RefusalSeqBefore = GetRefusalSeq();
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyUp(TEXT("A"))));
				ASSERT_THAT(IsTrue(GetRefusalSeq() == RefusalSeqBefore));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.KeyUpCount == 1));
				ASSERT_THAT(IsTrue(Recorded.LastKeyUp == EKeys::A));
			});
	}

	// -- PressKey tests ----------------------------------------------

	TEST_METHOD(PressKey_RepeatCountSendsRepeats)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const bool bPressed = USlateInspectorToolset59::PressKey(TEXT("A"), -1, 2);
				ASSERT_THAT(IsTrue(bPressed));

				// One initial press, two auto-repeats, one release.
				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.KeyDownCount == 3));
				ASSERT_THAT(IsTrue(Recorded.bLastKeyDownWasRepeat));
				ASSERT_THAT(IsTrue(Recorded.KeyUpCount == 1));
				ASSERT_THAT(IsTrue(Recorded.LastKeyUp == EKeys::A));
			});
	}

	TEST_METHOD(PressKey_NegativeRepeatReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const bool bPressed = USlateInspectorToolset59::PressKey(TEXT("A"), -1, -1);
				ASSERT_THAT(IsTrue(!bPressed));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().KeyDownCount == 0));
				ASSERT_THAT(IsTrue(IsLastRefusalFrom(TEXT("presskey"), TEXT("A"))));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::LastRefusal().Contains(TEXT("RepeatCount"))));
			});
	}

	TEST_METHOD(PressKey_RepeatCountAboveLimitReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const bool bPressed = USlateInspectorToolset59::PressKey(TEXT("A"), -1, 1001);
				ASSERT_THAT(IsTrue(!bPressed));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().KeyDownCount == 0));
				ASSERT_THAT(IsTrue(IsLastRefusalFrom(TEXT("presskey"), TEXT("A"))));
			});
	}

	TEST_METHOD(PressKey_UnknownKeyRecordsRefusal)
	{
		const int32 RefusalSeqBefore = GetRefusalSeq();
		const bool bPressed = USlateInspectorToolset59::PressKey(TEXT("Ctrl+NotAKeyName"));
		ASSERT_THAT(IsTrue(!bPressed));
		ASSERT_THAT(IsTrue(GetRefusalSeq() == RefusalSeqBefore + 1));
		ASSERT_THAT(IsTrue(IsLastRefusalFrom(TEXT("presskey"), TEXT("Ctrl+NotAKeyName"))));
	}

	TEST_METHOD(PressKey_UnhandledKeyReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));
				TestPanel->GetInputRecorder()->SetHandledKeyEvents(ESlateInspectorToolsetKeyEvents::None);

				const int32 RefusalSeqBefore = GetRefusalSeq();
				const bool bPressed = USlateInspectorToolset59::PressKey(TEXT("A"));
				ASSERT_THAT(IsTrue(!bPressed));
				ASSERT_THAT(IsTrue(GetRefusalSeq() == RefusalSeqBefore + 1));
				ASSERT_THAT(IsTrue(IsLastRefusalFrom(TEXT("presskey"), TEXT("A"))));
				// The reason names the focused widget, and points at Type since PressKey sends no character for A.
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::LastRefusal().Contains(TEXT("SSlateInspectorToolsetInputRecorder"))));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::LastRefusal().Contains(TEXT("use Type"))));

				// Both halves were delivered and declined, not withheld.
				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.KeyDownCount == 1));
				ASSERT_THAT(IsTrue(Recorded.KeyUpCount == 1));
			});
	}

	TEST_METHOD(PressKey_UnhandledSpaceBarSuggestsType)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));
				TestPanel->GetInputRecorder()->SetHandledKeyEvents(ESlateInspectorToolsetKeyEvents::None);

				// The platform character map has SpaceBar as ' ', so a space gets the same Type advice as a letter.
				ASSERT_THAT(IsTrue(!USlateInspectorToolset59::PressKey(TEXT("SpaceBar"))));
				ASSERT_THAT(IsTrue(IsLastRefusalFrom(TEXT("presskey"), TEXT("SpaceBar"))));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::LastRefusal().Contains(TEXT("use Type"))));
			});
	}

	TEST_METHOD(PressKey_UnhandledGamepadKeyReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				// UserIndex -1 sends gamepad keys to the Slate user mapped to the default input device.
				const TOptional<int32> GamepadUserIndex =
					FSlateApplication::Get().GetUserIndexForInputDevice(IPlatformInputDeviceMapper::Get().GetDefaultInputDevice());
				ASSERT_THAT(IsTrue(GamepadUserIndex.IsSet()));
				const FString RecorderRef = FocusInputRecorder(GamepadUserIndex.GetValue());
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));
				TestPanel->GetInputRecorder()->SetHandledKeyEvents(ESlateInspectorToolsetKeyEvents::None);

				// Not a navigation key, so no focusable ancestor turns the declined press into a navigation request.
				const bool bPressed = USlateInspectorToolset59::PressKey(TEXT("Gamepad_FaceButton_Bottom"));
				ASSERT_THAT(IsTrue(!bPressed));
				ASSERT_THAT(IsTrue(IsLastRefusalFrom(TEXT("presskey"), TEXT("Gamepad_FaceButton_Bottom"))));
				// A gamepad key types nothing, so the reason points at focus instead of Type.
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::LastRefusal().Contains(TEXT("Focus or Click"))));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.KeyDownCount == 1));
				ASSERT_THAT(IsTrue(Recorded.LastKeyUserIndex == GamepadUserIndex.GetValue()));
			});
	}

	TEST_METHOD(PressKey_HandledCharEventReturnsTrue)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));
				// Like SEditableText, which deletes on BackSpace's char event and ignores its key down.
				TestPanel->GetInputRecorder()->SetHandledKeyEvents(ESlateInspectorToolsetKeyEvents::KeyChar);

				const bool bPressed = USlateInspectorToolset59::PressKey(TEXT("BackSpace"));
				ASSERT_THAT(IsTrue(bPressed));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.KeyCharCount == 1));
				ASSERT_THAT(IsTrue(Recorded.LastKeyChar == TCHAR('\b')));
			});
	}

	TEST_METHOD(PressKey_HandledReleaseReturnsTrue)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));
				TestPanel->GetInputRecorder()->SetHandledKeyEvents(ESlateInspectorToolsetKeyEvents::KeyUp);

				const bool bPressed = USlateInspectorToolset59::PressKey(TEXT("A"));
				ASSERT_THAT(IsTrue(bPressed));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().KeyUpCount == 1));
			});
	}

	TEST_METHOD(PressKey_HeldModifierApplied)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyDown(TEXT("LeftControl"))));
				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastKeyDownModifiers.IsLeftControlDown()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::PressKey(TEXT("C"))));
				ASSERT_THAT(IsTrue(Recorded.LastKeyDown == EKeys::C));
				ASSERT_THAT(IsTrue(Recorded.LastKeyDownModifiers.IsLeftControlDown()));
				ASSERT_THAT(IsTrue(!Recorded.LastKeyDownModifiers.IsShiftDown()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyUp(TEXT("LeftControl"))));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::PressKey(TEXT("C"))));
				ASSERT_THAT(IsTrue(!Recorded.LastKeyDownModifiers.IsControlDown()));
			});
	}

	TEST_METHOD(ReleaseHeldInput_ReleasesHeldKeys)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyDown(TEXT("LeftShift"))));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyDown(TEXT("A"))));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::ReleaseHeldInput() == 2));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.KeyUpCount == 2));
				ASSERT_THAT(IsTrue(Recorded.LastKeyUp == EKeys::LeftShift));

				// Released, so a later press reports no Shift.
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::PressKey(TEXT("B"))));
				ASSERT_THAT(IsTrue(!Recorded.LastKeyDownModifiers.IsShiftDown()));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::ReleaseHeldInput() == 0));
			});
	}

	TEST_METHOD(KeyUp_ReleasesOnlyThatUsersHold)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				FScopedSlateUser SecondUser(SecondaryUserIndex);
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::Focus(RecorderRef, SecondaryUserIndex)));

				constexpr int32 PrimaryUserIndex = 0;
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyDown(TEXT("Gamepad_FaceButton_Bottom"), PrimaryUserIndex)));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyDown(TEXT("Gamepad_FaceButton_Bottom"), SecondaryUserIndex)));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyUp(TEXT("Gamepad_FaceButton_Bottom"), PrimaryUserIndex)));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.KeyUpCount == 1));
				ASSERT_THAT(IsTrue(Recorded.LastKeyUserIndex == PrimaryUserIndex));

				// The second user's hold outlives the first user's release, so the sweep still releases it, for that user.
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::ReleaseHeldInput() == 1));
				ASSERT_THAT(IsTrue(Recorded.KeyUpCount == 2));
				ASSERT_THAT(IsTrue(Recorded.LastKeyUp == EKeys::Gamepad_FaceButton_Bottom));
				ASSERT_THAT(IsTrue(Recorded.LastKeyUserIndex == SecondaryUserIndex));
			});
	}

	TEST_METHOD(HeldInputWatchdog_ReleasesHeldKeyAfterTimeout)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				ASSERT_THAT(IsTrue(!FocusInputRecorder().IsEmpty()));

				GetHeldInputTimeoutVariable()->Set(0.01f, ECVF_SetByCode);
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyDown(TEXT("LeftControl"))));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().KeyUpCount == 0));
			})
			// The watchdog polls once per frame, so the release lands a frame or two after the shortened timeout elapses.
			.Until([this]() { return TestPanel->GetInputRecorder()->GetRecorded().KeyUpCount == 1; })
			.Then([this]()
			{
				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastKeyUp == EKeys::LeftControl));

				// Released, so a later press reports no Control.
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::PressKey(TEXT("C"))));
				ASSERT_THAT(IsTrue(!Recorded.LastKeyDownModifiers.IsControlDown()));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::ReleaseHeldInput() == 0));
			});
	}

	TEST_METHOD(PressKey_ReleasesKeyHeldByKeyDown)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				ASSERT_THAT(IsTrue(!FocusInputRecorder().IsEmpty()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyDown(TEXT("LeftControl"))));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::PressKey(TEXT("LeftControl"))));

				// The press's release ended the hold, so a later press reports no Control and nothing is left to release.
				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::PressKey(TEXT("C"))));
				ASSERT_THAT(IsTrue(Recorded.LastKeyDown == EKeys::C));
				ASSERT_THAT(IsTrue(!Recorded.LastKeyDownModifiers.IsControlDown()));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::ReleaseHeldInput() == 0));
			});
	}

	TEST_METHOD(KeyDown_HeldModifierAppliesOnlyToThatUser)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				FScopedSlateUser SecondUser(SecondaryUserIndex);
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::Focus(RecorderRef, SecondaryUserIndex)));

				constexpr int32 PrimaryUserIndex = 0;
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyDown(TEXT("LeftControl"), PrimaryUserIndex)));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::PressKey(TEXT("C"), SecondaryUserIndex)));
				ASSERT_THAT(IsTrue(Recorded.LastKeyUserIndex == SecondaryUserIndex));
				ASSERT_THAT(IsTrue(!Recorded.LastKeyDownModifiers.IsControlDown()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::PressKey(TEXT("C"), PrimaryUserIndex)));
				ASSERT_THAT(IsTrue(Recorded.LastKeyUserIndex == PrimaryUserIndex));
				ASSERT_THAT(IsTrue(Recorded.LastKeyDownModifiers.IsControlDown()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyUp(TEXT("LeftControl"), PrimaryUserIndex)));
			});
	}

	TEST_METHOD(ReleaseHeldInput_SendsHeldKeysPrefixModifiers)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				ASSERT_THAT(IsTrue(!FocusInputRecorder().IsEmpty()));

				ASSERT_THAT(IsTrue(USlateInspectorToolset59::KeyDown(TEXT("Ctrl+A"))));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::ReleaseHeldInput() == 1));

				// Released as KeyUp("Ctrl+A") would release it.
				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastKeyUp == EKeys::A));
				ASSERT_THAT(IsTrue(Recorded.LastKeyUpModifiers.IsControlDown()));
			});
	}

	// -- AnalogInput tests -------------------------------------------

	TEST_METHOD(AnalogInput_RecordsValue)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				// Gamepad events route to the default device's Slate user, whose focus path must lead to the recorder.
				const FInputDeviceId DefaultDevice = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
				const TOptional<int32> DefaultUserIndex = FSlateApplication::Get().GetUserIndexForInputDevice(DefaultDevice);
				ASSERT_THAT(IsTrue(DefaultUserIndex.IsSet()));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::Focus(RecorderRef, DefaultUserIndex.GetValue())));

				const double Value = 0.75;
				const bool bSent = USlateInspectorToolset59::AnalogInput(TEXT("Gamepad_LeftX"), Value, DefaultUserIndex.GetValue());
				ASSERT_THAT(IsTrue(bSent));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.AnalogEventCount == 1));
				ASSERT_THAT(IsTrue(Recorded.LastAnalogKey == EKeys::Gamepad_LeftX));
				ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(Recorded.LastAnalogValue, static_cast<float>(Value))));
				ASSERT_THAT(IsTrue(Recorded.LastAnalogUserIndex == DefaultUserIndex.GetValue()));
			});
	}

	TEST_METHOD(AnalogInput_NonAxisKeyReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				ASSERT_THAT(IsTrue(!USlateInspectorToolset59::AnalogInput(TEXT("Gamepad_FaceButton_Bottom"), 1.0)));
				ASSERT_THAT(IsTrue(!USlateInspectorToolset59::AnalogInput(TEXT("A"), 1.0)));
				// A 2D pair has no single value; its component axes are the accepted keys.
				ASSERT_THAT(IsTrue(!USlateInspectorToolset59::AnalogInput(TEXT("Gamepad_Left2D"), 1.0)));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().AnalogEventCount == 0));
			});
	}

	TEST_METHOD(AnalogInput_UnmappedUserReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString RecorderRef = FocusInputRecorder();
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const int32 UnmappedUserIndex = FindUnregisteredUserIndex();
				ASSERT_THAT(IsTrue(UnmappedUserIndex != INDEX_NONE));

				const bool bSent = USlateInspectorToolset59::AnalogInput(TEXT("Gamepad_LeftX"), 0.5, UnmappedUserIndex);
				ASSERT_THAT(IsTrue(!bSent));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().AnalogEventCount == 0));
				// Refusing the request must not have registered the user as a side effect.
				ASSERT_THAT(IsTrue(!FSlateApplication::Get().GetUser(UnmappedUserIndex).IsValid()));
			});
	}

	TEST_METHOD(AnalogInput_UserIndex1Routed)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				FScopedSlateUser SecondUser(SecondaryUserIndex);
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::Focus(RecorderRef, SecondaryUserIndex)));

				const double Value = -0.5;
				const bool bSent = USlateInspectorToolset59::AnalogInput(TEXT("Gamepad_LeftTriggerAxis"), Value, SecondaryUserIndex);
				ASSERT_THAT(IsTrue(bSent));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.AnalogEventCount == 1));
				ASSERT_THAT(IsTrue(Recorded.LastAnalogKey == EKeys::Gamepad_LeftTriggerAxis));
				ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(Recorded.LastAnalogValue, static_cast<float>(Value))));
				ASSERT_THAT(IsTrue(Recorded.LastAnalogUserIndex == SecondaryUserIndex));
			});
	}

	// -- Touch tests -------------------------------------------------

	TEST_METHOD(TouchStart_RecordsOnTouchStarted)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const FVector2D Center = GetInputRecorderCenter();
				const bool bStarted = StartTouch(RecorderRef);
				// The finger is lifted before asserting so a failure cannot leave it down for later tests.
				const bool bEnded = EndTouch(Center.X, Center.Y);
				ASSERT_THAT(IsTrue(bStarted));
				ASSERT_THAT(IsTrue(bEnded));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.TouchStartCount == 1));
				ASSERT_THAT(IsTrue(Recorded.LastTouchPointerIndex == 0));
				ASSERT_THAT(IsTrue(IsNearlyAt(Recorded.LastTouchPosition, Center)));
				// The recorder handled the touch as a touch, so nothing fell through to its mouse handlers.
				ASSERT_THAT(IsTrue(Recorded.MouseButtonDownCount == 0));
			});
	}

	TEST_METHOD(TouchStart_AbsolutePositionHitsWindowUnderPoint)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FVector2D Center = GetInputRecorderCenter();
				const bool bStarted = StartTouch(TEXT(""), Center.X, Center.Y);
				const bool bEnded = EndTouch(Center.X, Center.Y);
				ASSERT_THAT(IsTrue(bStarted));
				ASSERT_THAT(IsTrue(bEnded));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.TouchStartCount == 1));
				ASSERT_THAT(IsTrue(IsNearlyAt(Recorded.LastTouchPosition, Center)));
			});
	}

	TEST_METHOD(TouchStartEnd_ClicksButton)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				const TSharedPtr<SButton> TestButton = TestPanel->GetTestButton();
				ASSERT_THAT(IsTrue(TestButton.IsValid()));
				const FVector2D ButtonCenter = GetScreenCenter(TestButton.ToSharedRef());

				// SButton has no touch handlers, so the touch reaches its mouse press and is held until the finger lifts.
				const bool bStarted = StartTouch(ButtonRef);
				const bool bPressedWhileDown = TestButton->IsPressed();
				const int32 ClicksWhileDown = TestPanel->ButtonClickCount;
				const bool bEnded = EndTouch(ButtonCenter.X, ButtonCenter.Y);

				ASSERT_THAT(IsTrue(bStarted));
				ASSERT_THAT(IsTrue(bPressedWhileDown));
				ASSERT_THAT(IsTrue(ClicksWhileDown == 0));
				ASSERT_THAT(IsTrue(bEnded));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 1));
				ASSERT_THAT(IsTrue(!TestButton->IsPressed()));
			});
	}

	TEST_METHOD(TouchMove_RecordsMove)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				// The recorder does not capture the touch, so the move stays inside it to reach it by hit-test.
				const FVector2D MovedTo = GetInputRecorderCenter() + FVector2D(5.0, 5.0);
				const bool bStarted = StartTouch(RecorderRef);
				const bool bMoved = USlateInspectorToolset59::TouchMove(MovedTo.X, MovedTo.Y);
				const bool bEnded = EndTouch(MovedTo.X, MovedTo.Y);
				ASSERT_THAT(IsTrue(bStarted));
				ASSERT_THAT(IsTrue(bMoved));
				ASSERT_THAT(IsTrue(bEnded));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.TouchStartCount == 1));
				ASSERT_THAT(IsTrue(Recorded.TouchMoveCount == 1));
				ASSERT_THAT(IsTrue(Recorded.TouchEndCount == 1));
				ASSERT_THAT(IsTrue(IsNearlyAt(Recorded.LastTouchPosition, MovedTo)));
			});
	}

	TEST_METHOD(TouchEnd_RecordsEnd)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const FVector2D Center = GetInputRecorderCenter();
				ASSERT_THAT(IsTrue(StartTouch(RecorderRef)));
				const bool bEnded = EndTouch(Center.X, Center.Y);
				ASSERT_THAT(IsTrue(bEnded));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.TouchEndCount == 1));
				ASSERT_THAT(IsTrue(Recorded.LastTouchPointerIndex == 0));

				// The end released the finger in Slate's eyes: the same index can start again.
				const bool bRestarted = StartTouch(RecorderRef);
				EndTouch(Center.X, Center.Y);
				ASSERT_THAT(IsTrue(bRestarted));
				ASSERT_THAT(IsTrue(Recorded.TouchStartCount == 2));
			});
	}

	TEST_METHOD(TouchStart_WhileDownReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				const FVector2D Center = GetInputRecorderCenter();
				const bool bStarted = StartTouch(RecorderRef);
				// Slate reports an error for a finger that starts twice without lifting, so the tool refuses instead.
				const bool bStartedAgain = StartTouch(RecorderRef);
				EndTouch(Center.X, Center.Y);
				ASSERT_THAT(IsTrue(bStarted));
				ASSERT_THAT(IsTrue(!bStartedAgain));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().TouchStartCount == 1));
			});
	}

	TEST_METHOD(TouchStart_IndexOutOfRangeReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString RecorderRef = FindInputRecorderRef(Snapshot);
				ASSERT_THAT(IsTrue(!RecorderRef.IsEmpty()));

				// The cursor occupies CursorPointerIndex, so fingers stop just below it; negative indices name nothing.
				ASSERT_THAT(IsTrue(!StartTouch(RecorderRef, 0.0, 0.0, static_cast<int32>(ETouchIndex::CursorPointerIndex))));
				ASSERT_THAT(IsTrue(!StartTouch(RecorderRef, 0.0, 0.0, -1)));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().TouchStartCount == 0));
			});
	}

	TEST_METHOD(TouchStart_InvalidRefReturnsFalse)
	{
		const bool bStarted = StartTouch(TEXT("nonexistent_ref"));
		ASSERT_THAT(IsTrue(!bStarted));
	}

	TEST_METHOD(TouchMove_WithoutStartReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FVector2D Center = GetInputRecorderCenter();
				const bool bMoved = USlateInspectorToolset59::TouchMove(Center.X, Center.Y);
				ASSERT_THAT(IsTrue(!bMoved));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().TouchMoveCount == 0));
			});
	}

	TEST_METHOD(TouchEnd_WithoutStartReturnsFalse)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FVector2D Center = GetInputRecorderCenter();
				const bool bEnded = USlateInspectorToolset59::TouchEnd(Center.X, Center.Y);
				ASSERT_THAT(IsTrue(!bEnded));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().TouchEndCount == 0));
			});
	}
};
