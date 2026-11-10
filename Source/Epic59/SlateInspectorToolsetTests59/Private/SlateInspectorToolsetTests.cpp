// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"
#include "Components/CQTestSlateComponent.h"
#include "SlateInspectorToolsetTestPanel.h"

#include "Components/Button.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "InputCoreTypes.h"
#include "Misc/ScopeExit.h"
#include "SSlateInspectorToolsetInputRecorder.h"
#include "SlateInspectorToolsetObserverManager.h"
#include "SlateInspectorToolset59.h"
#include "SlateInspectorToolsetUMGResolver.h"
#include "UObject/StrongObjectPtr.h"
#include "WHRefCache.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/SCanvas.h"
#include "Widgets/SInvalidationPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

TEST_CLASS(SlateInspectorToolsetTest, "AIAssistant.Toolsets.SlateInspectorToolset")
{
	TUniquePtr<FCQTestSlateComponent> SlateComponent;
	TSharedPtr<SWindow> TestWindow;
	TSharedPtr<SSlateInspectorToolsetTestPanel> TestPanel;

	BEFORE_EACH()
	{
		SlateComponent = MakeUnique<FCQTestSlateComponent>();

		TestPanel = SNew(SSlateInspectorToolsetTestPanel);

		TestWindow = SNew(SWindow)
			.Title(FText::FromString(TEXT("SlateInspectorToolset Test Window")))
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

	// -- Snapshot tests ----------------------------------------------

	TEST_METHOD(Snapshot_ReturnsWidgetTree)
	{
		const FString Snapshot = SnapshotTestWindow();
		ASSERT_THAT(IsTrue(!Snapshot.IsEmpty()));
		ASSERT_THAT(IsTrue(Snapshot.Contains(TEXT("button"))));
		ASSERT_THAT(IsTrue(Snapshot.Contains(TEXT("textbox"))));
		ASSERT_THAT(IsTrue(Snapshot.Contains(TEXT("checkbox"))));
		ASSERT_THAT(IsTrue(Snapshot.Contains(TEXT("combobox"))));
		ASSERT_THAT(IsTrue(Snapshot.Contains(TEXT("slider"))));
	}

	TEST_METHOD(Snapshot_SubtreeByRef)
	{
		const FString FullSnapshot = USlateInspectorToolset59::Snapshot(TEXT(""), 30);
		ASSERT_THAT(IsTrue(FullSnapshot.Contains(TEXT("SlateInspectorToolset Test Window"))));

		const FString WindowRef = FindRefByRole(FullSnapshot, TEXT("window"), TEXT("SlateInspectorToolset Test Window"));
		ASSERT_THAT(IsTrue(!WindowRef.IsEmpty()));

		const FString SubtreeSnapshot = USlateInspectorToolset59::Snapshot(WindowRef, 30);
		ASSERT_THAT(IsTrue(SubtreeSnapshot.Contains(TEXT("button"))));
	}

	TEST_METHOD(Snapshot_ContainsStaticLabel)
	{
		const FString Snapshot = SnapshotTestWindow();
		ASSERT_THAT(IsTrue(Snapshot.Contains(TEXT("Static Label"))));
	}

	TEST_METHOD(Snapshot_ContainsSpinBox)
	{
		const FString Snapshot = SnapshotTestWindow();
		// SSpinBox<> maps to "slider" role via template prefix matching.
		ASSERT_THAT(IsTrue(Snapshot.Contains(TEXT("slider"))));
	}

	TEST_METHOD(Snapshot_ListsChildrenOfCachingInvalidationPanel)
	{
		TSharedPtr<SInvalidationPanel> InvalidationPanel;
		const TSharedRef<SWindow> Window = AddContentWindow(
			SAssignNew(InvalidationPanel, SInvalidationPanel)
			[
				SNew(SButton).Text(FText::FromString(TEXT("Cached Button")))
			]);
		ON_SCOPE_EXIT { Window->RequestDestroyWindow(); };

		// Graph editors wrap their nodes like this. A caching panel reports no children through GetChildren.
		InvalidationPanel->SetCanCache(true);
		if (!InvalidationPanel->GetCanCache())
		{
			TestRunner->AddWarning(TEXT("Skipped: invalidation panels cannot cache (Slate.EnableInvalidationPanels or global invalidation)."));
			return;
		}
		ASSERT_THAT(IsTrue(InvalidationPanel->GetChildren()->Num() == 0));

		const FString WindowRef = UE::WidgetHelper::FWidgetRefCache::Get().GetOrAssignRef(Window, TEXT("w")).ToString();
		ASSERT_THAT(IsTrue(USlateInspectorToolset59::Snapshot(WindowRef, 30).Contains(TEXT("Cached Button"))));
	}

	TEST_METHOD(Snapshot_SkipsListRowsWaitingForRefresh)
	{
		TArray<TSharedPtr<FString>> Items = { MakeShared<FString>(TEXT("Stale Row")) };
		TSharedPtr<SListView<TSharedPtr<FString>>> ListView;
		const TSharedRef<SWindow> Window = AddContentWindow(
			SAssignNew(ListView, SListView<TSharedPtr<FString>>)
			.ListItemsSource(&Items)
			.OnGenerateRow_Lambda([](TSharedPtr<FString> Item, const TSharedRef<STableViewBase>& OwnerTable)
			{
				return SNew(STableRow<TSharedPtr<FString>>, OwnerTable)
				[
					SNew(STextBlock).Text(FText::FromString(*Item))
				];
			}));
		ON_SCOPE_EXIT { Window->RequestDestroyWindow(); };

		const FString WindowRef = UE::WidgetHelper::FWidgetRefCache::Get().GetOrAssignRef(Window, TEXT("w")).ToString();
		ASSERT_THAT(IsTrue(USlateInspectorToolset59::Snapshot(WindowRef, 30).Contains(TEXT("Stale Row"))));

		// Until the list's next tick, its rows may point at data the source no longer holds, which is unsafe to read.
		Items.Reset();
		ListView->RequestListRefresh();
		ASSERT_THAT(IsTrue(!USlateInspectorToolset59::Snapshot(WindowRef, 30).Contains(TEXT("Stale Row"))));
	}

	TEST_METHOD(Snapshot_SizeIsInDesktopPixelsUnderDPIScale)
	{
		TSharedPtr<SButton> ScaledButton;
		const TSharedRef<SWindow> Window = AddContentWindow(
			SNew(SDPIScaler)
			.DPIScale(2.0f)
			[
				SNew(SCanvas)
				+ SCanvas::Slot()
				.Size(FVector2D(50.0, 20.0))
				[
					SAssignNew(ScaledButton, SButton).Text(FText::FromString(TEXT("Scaled Button")))
				]
			]);
		ON_SCOPE_EXIT { Window->RequestDestroyWindow(); };

		// The scaler doubles the 50x20 local size on screen, on top of the monitor's own DPI scale.
		const FVector2D PixelSize = ScaledButton->GetCachedGeometry().GetAbsoluteSize();
		ASSERT_THAT(IsTrue(PixelSize.X >= 99.0));

		const FString WindowRef = UE::WidgetHelper::FWidgetRefCache::Get().GetOrAssignRef(Window, TEXT("w")).ToString();
		const FString Snapshot = USlateInspectorToolset59::Snapshot(WindowRef, 30);
		ASSERT_THAT(IsTrue(Snapshot.Contains(FString::Printf(TEXT("size=%.0f,%.0f]"), PixelSize.X, PixelSize.Y))));
		ASSERT_THAT(IsTrue(!Snapshot.Contains(TEXT("size=50,20]"))));
	}

	TEST_METHOD(Snapshot_RectIsBoundsOfRotatedWidget)
	{
		TSharedPtr<SButton> RotatedButton;
		const TSharedRef<SWindow> Window = AddContentWindow(
			SNew(SCanvas)
			+ SCanvas::Slot()
			.Position(FVector2D(60.0, 60.0))
			.Size(FVector2D(50.0, 20.0))
			[
				SAssignNew(RotatedButton, SButton)
				.Text(FText::FromString(TEXT("Rotated Button")))
				.RenderTransform(FSlateRenderTransform(FQuat2D(UE_HALF_PI)))
				.RenderTransformPivot(FVector2D(0.5, 0.5))
			]);
		ON_SCOPE_EXIT { Window->RequestDestroyWindow(); };

		// A quarter turn stands the 50x20 button 20 wide and 50 tall. Its transformed size would be negative on X.
		const FGeometry& Geometry = RotatedButton->GetCachedGeometry();
		ASSERT_THAT(IsTrue(Geometry.GetAbsoluteSize().X < 0.0));
		const FSlateRect Bounds = Geometry.GetRenderBoundingRect();
		ASSERT_THAT(IsTrue(Bounds.GetSize().Y > Bounds.GetSize().X));

		const FString WindowRef = UE::WidgetHelper::FWidgetRefCache::Get().GetOrAssignRef(Window, TEXT("w")).ToString();
		const FString Expected = FString::Printf(TEXT("[pos=%.0f,%.0f size=%.0f,%.0f]"), Bounds.Left, Bounds.Top, Bounds.GetSize().X, Bounds.GetSize().Y);
		ASSERT_THAT(IsTrue(USlateInspectorToolset59::Snapshot(WindowRef, 30).Contains(Expected)));
	}

	// -- Observer tests ----------------------------------------------

	TEST_METHOD(Observe_CreatesObserver)
	{
		const FString Snapshot = SnapshotTestWindow();
		const FString WindowRef = FindRefByRole(Snapshot, TEXT("window"), TEXT("SlateInspectorToolset Test Window"));
		ASSERT_THAT(IsTrue(!WindowRef.IsEmpty()));

		const FString Identifier = USlateInspectorToolset59::Observe(WindowRef, 30);
		ASSERT_THAT(IsTrue(!Identifier.IsEmpty()));
		ASSERT_THAT(IsTrue(!Identifier.StartsWith(TEXT("Error"))));

		const FString ObserverList = USlateInspectorToolset59::ListObservers();
		ASSERT_THAT(IsTrue(ObserverList.Contains(Identifier)));

		USlateInspectorToolset59::Unobserve(Identifier);
	}

	TEST_METHOD(Observe_Deduplicates)
	{
		const FString Snapshot = SnapshotTestWindow();
		const FString WindowRef = FindRefByRole(Snapshot, TEXT("window"), TEXT("SlateInspectorToolset Test Window"));

		const FString Identifier1 = USlateInspectorToolset59::Observe(WindowRef, 30);
		const FString Identifier2 = USlateInspectorToolset59::Observe(WindowRef, 30);
		ASSERT_THAT(IsTrue(Identifier1 == Identifier2));

		USlateInspectorToolset59::Unobserve(Identifier1);
	}

	TEST_METHOD(Unobserve_RemovesObserver)
	{
		const FString Snapshot = SnapshotTestWindow();
		const FString WindowRef = FindRefByRole(Snapshot, TEXT("window"), TEXT("SlateInspectorToolset Test Window"));
		const FString Identifier = USlateInspectorToolset59::Observe(WindowRef, 30);

		const bool bRemoved = USlateInspectorToolset59::Unobserve(Identifier);
		ASSERT_THAT(IsTrue(bRemoved));

		const FString ObserverList = USlateInspectorToolset59::ListObservers();
		ASSERT_THAT(IsTrue(!ObserverList.Contains(Identifier)));
	}

	TEST_METHOD(Unobserve_ReturnsFalseForUnknown)
	{
		const bool bRemoved = USlateInspectorToolset59::Unobserve(TEXT("nonexistent_observer"));
		ASSERT_THAT(IsTrue(!bRemoved));
	}

	TEST_METHOD(Observer_KeepsRefsAlive)
	{
		const FString Snapshot = SnapshotTestWindow();
		const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
		ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

		const FString WindowRef = FindRefByRole(Snapshot, TEXT("window"), TEXT("SlateInspectorToolset Test Window"));
		const FString ObserverId = USlateInspectorToolset59::Observe(WindowRef, 30);

		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(15); })
			.Then([this, ButtonRef, ObserverId]()
			{
				const bool bClicked = USlateInspectorToolset59::Click(ButtonRef);
				ASSERT_THAT(IsTrue(bClicked));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount > 0));
				USlateInspectorToolset59::Unobserve(ObserverId);
			});
	}

	// -- Click tests -------------------------------------------------

	TEST_METHOD(Click_PressesButton)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 0));
				const bool bClicked = USlateInspectorToolset59::Click(ButtonRef);
				ASSERT_THAT(IsTrue(bClicked));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 1));
			});
	}

	TEST_METHOD(Click_MultipleButtons)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString FirstRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				const FString SecondRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Second Button"));
				ASSERT_THAT(IsTrue(!FirstRef.IsEmpty()));
				ASSERT_THAT(IsTrue(!SecondRef.IsEmpty()));
				ASSERT_THAT(IsTrue(FirstRef != SecondRef));

				USlateInspectorToolset59::Click(FirstRef);
				USlateInspectorToolset59::Click(SecondRef);
				USlateInspectorToolset59::Click(FirstRef);

				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 2));
				ASSERT_THAT(IsTrue(TestPanel->SecondButtonClickCount == 1));
			});
	}

	TEST_METHOD(Click_DoubleClick)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				FSlateInspectorToolsetClickParams Params;
				Params.bDoubleClick = true;
				const bool bClicked = USlateInspectorToolset59::Click(ButtonRef, Params);
				ASSERT_THAT(IsTrue(bClicked));
			});
	}

	TEST_METHOD(Click_RightClick)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				FSlateInspectorToolsetClickParams Params;
				Params.Button = TEXT("right");
				const bool bClicked = USlateInspectorToolset59::Click(ButtonRef, Params);
				ASSERT_THAT(IsTrue(bClicked));
			});
	}

	TEST_METHOD(Click_InvalidRef)
	{
		const bool bClicked = USlateInspectorToolset59::Click(TEXT("nonexistent_ref"));
		ASSERT_THAT(IsTrue(!bClicked));
	}

	TEST_METHOD(Click_OutOfRangePositionRefused)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				FSlateInspectorToolsetClickParams Params;
				Params.Position.X = 1.5f;
				const bool bClicked = USlateInspectorToolset59::Click(ButtonRef, Params);
				ASSERT_THAT(IsTrue(!bClicked));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 0));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::LastRefusal().Contains(TEXT("Position"))));
			});
	}

	TEST_METHOD(LastRefusal_UnchangedByReadOnlyTools)
	{
		const FString Before = USlateInspectorToolset59::LastRefusal();
		USlateInspectorToolset59::Snapshot(TEXT("nonexistent_ref"));
		USlateInspectorToolset59::GetUMGInfo(TEXT("nonexistent_ref"));
		USlateInspectorToolset59::HitTest(TEXT("nonexistent_ref"));
		ASSERT_THAT(AreEqual(Before, USlateInspectorToolset59::LastRefusal()));

		USlateInspectorToolset59::Click(TEXT("nonexistent_ref"));
		ASSERT_THAT(IsTrue(Before != USlateInspectorToolset59::LastRefusal()));
	}

	TEST_METHOD(Scroll_ZeroTicksRefused)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				FSlateInspectorToolsetScrollParams Params;
				Params.Ticks = 0;
				ASSERT_THAT(IsTrue(!USlateInspectorToolset59::Scroll(ButtonRef, Params)));
			});
	}

	TEST_METHOD(Drag_BySelfWithoutMoveRefused)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				const bool bDragged = USlateInspectorToolset59::Drag(ButtonRef, FString());
				ASSERT_THAT(IsTrue(!bDragged));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 0));
			});
	}

	TEST_METHOD(Click_WithShiftModifier)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				FSlateInspectorToolsetClickParams Params;
				Params.Modifiers.bShift = true;
				const bool bClicked = USlateInspectorToolset59::Click(ButtonRef, Params);
				ASSERT_THAT(IsTrue(bClicked));
				ASSERT_THAT(IsTrue(TestPanel->ButtonClickCount == 1));
			});
	}

	TEST_METHOD(Click_WithCtrlModifier)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				FSlateInspectorToolsetClickParams Params;
				Params.Modifiers.bCtrl = true;
				const bool bClicked = USlateInspectorToolset59::Click(ButtonRef, Params);
				ASSERT_THAT(IsTrue(bClicked));
			});
	}

	TEST_METHOD(Click_WithMultipleModifiers)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				FSlateInspectorToolsetClickParams Params;
				Params.Modifiers.bShift = true;
				Params.Modifiers.bCtrl = true;
				Params.Modifiers.bAlt = true;
				const bool bClicked = USlateInspectorToolset59::Click(ButtonRef, Params);
				ASSERT_THAT(IsTrue(bClicked));
			});
	}

	// -- Drag tests -------------------------------------------------

	TEST_METHOD(Drag_BetweenButtons)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString FirstRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				const FString SecondRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Second Button"));
				ASSERT_THAT(IsTrue(!FirstRef.IsEmpty()));
				ASSERT_THAT(IsTrue(!SecondRef.IsEmpty()));

				const bool bDragged = USlateInspectorToolset59::Drag(FirstRef, SecondRef);
				ASSERT_THAT(IsTrue(bDragged));
			});
	}

	TEST_METHOD(Drag_WithModifiers)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString FirstRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				const FString SecondRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Second Button"));

				FSlateInspectorToolsetDragParams Params;
				Params.Modifiers.bShift = true;
				const bool bDragged = USlateInspectorToolset59::Drag(FirstRef, SecondRef, Params);
				ASSERT_THAT(IsTrue(bDragged));
			});
	}

	TEST_METHOD(Drag_InvalidStartRef)
	{
		const bool bDragged = USlateInspectorToolset59::Drag(TEXT("nonexistent"), TEXT("nonexistent2"));
		ASSERT_THAT(IsTrue(!bDragged));
	}

	TEST_METHOD(Drag_SliderChangesValue)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				ASSERT_THAT(IsTrue(TestPanel->SliderValue == 0.0f));

				const FString Snapshot = SnapshotTestWindow();
				const TArray<FString> SliderRefs = FindAllRefsByRole(Snapshot, TEXT("slider"));
				// First slider ref is the SSlider, second is the SSpinBox.
				ASSERT_THAT(IsTrue(SliderRefs.Num() >= 1));
				const FString SliderRef = SliderRefs[0];

				// Drag from the slider to one of the buttons (to the right and up).
				// This should move the slider thumb and change the value.
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				const bool bDragged = USlateInspectorToolset59::Drag(SliderRef, ButtonRef);
				ASSERT_THAT(IsTrue(bDragged));

				// The slider value should have changed from its initial 0.0.
				// We can't predict the exact value since it depends on geometry,
				// but it should no longer be zero.
				ASSERT_THAT(IsTrue(TestPanel->SliderValue != 0.0f));
			});
	}

	// -- Checkbox tests ----------------------------------------------

	TEST_METHOD(Click_TogglesCheckbox)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString CheckBoxRef = FindRefByRole(Snapshot, TEXT("checkbox"), TEXT(""));
				ASSERT_THAT(IsTrue(!CheckBoxRef.IsEmpty()));

				ASSERT_THAT(IsTrue(!TestPanel->CheckBox->IsChecked()));

				USlateInspectorToolset59::Click(CheckBoxRef);
				ASSERT_THAT(IsTrue(TestPanel->CheckBox->IsChecked()));

				USlateInspectorToolset59::Click(CheckBoxRef);
				ASSERT_THAT(IsTrue(!TestPanel->CheckBox->IsChecked()));
			});
	}

	// -- Type tests --------------------------------------------------

	TEST_METHOD(Type_EntersText)
	{
		const FString Snapshot = SnapshotTestWindow();
		const FString TextBoxRef = FindRefByRole(Snapshot, TEXT("textbox"), TEXT(""));
		ASSERT_THAT(IsTrue(!TextBoxRef.IsEmpty()));

		const bool bTyped = USlateInspectorToolset59::Type(TextBoxRef, TEXT("hello"));
		ASSERT_THAT(IsTrue(bTyped));
		ASSERT_THAT(IsTrue(TestPanel->TextBox->GetText().ToString() == TEXT("hello")));
	}

	TEST_METHOD(Type_AppendsToExistingText)
	{
		const FString Snapshot = SnapshotTestWindow();
		const FString TextBoxRef = FindRefByRole(Snapshot, TEXT("textbox"), TEXT(""));

		USlateInspectorToolset59::Type(TextBoxRef, TEXT("foo"));
		USlateInspectorToolset59::Type(TextBoxRef, TEXT("bar"));
		ASSERT_THAT(IsTrue(TestPanel->TextBox->GetText().ToString() == TEXT("foobar")));
	}

	TEST_METHOD(Type_SpecialCharacters)
	{
		const FString Snapshot = SnapshotTestWindow();
		const FString TextBoxRef = FindRefByRole(Snapshot, TEXT("textbox"), TEXT(""));

		USlateInspectorToolset59::Type(TextBoxRef, TEXT("a@b#c$d"));
		ASSERT_THAT(IsTrue(TestPanel->TextBox->GetText().ToString() == TEXT("a@b#c$d")));
	}

	TEST_METHOD(Type_WithSubmit)
	{
		const FString Snapshot = SnapshotTestWindow();
		const FString TextBoxRef = FindRefByRole(Snapshot, TEXT("textbox"), TEXT(""));
		ASSERT_THAT(IsTrue(!TextBoxRef.IsEmpty()));

		const bool bTyped = USlateInspectorToolset59::Type(TextBoxRef, TEXT("submitted"), true);
		ASSERT_THAT(IsTrue(bTyped));
	}

	TEST_METHOD(Type_IntoSearchBox)
	{
		const FString Snapshot = SnapshotTestWindow();
		// SSearchBox contains an SEditableTextBox internally, so look for a textbox.
		// The search box is the third textbox in the panel (after TextBox and MultiLineTextBox).
		const TArray<FString> TextBoxRefs = FindAllRefsByRole(Snapshot, TEXT("textbox"));
		// We need at least 2 textbox refs (single-line and search box; multi-line may be a different role).
		ASSERT_THAT(IsTrue(TextBoxRefs.Num() >= 2));

		// Type into the last textbox (search box).
		const FString SearchRef = TextBoxRefs.Last();
		const bool bTyped = USlateInspectorToolset59::Type(SearchRef, TEXT("search query"));
		ASSERT_THAT(IsTrue(bTyped));
	}

	TEST_METHOD(Type_InvalidRef)
	{
		const bool bTyped = USlateInspectorToolset59::Type(TEXT("nonexistent_ref"), TEXT("hello"));
		ASSERT_THAT(IsTrue(!bTyped));
	}

	// -- PressKey in text context ------------------------------------

	TEST_METHOD(PressKey_BackspaceDeletesCharacter)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString TextBoxRef = FindRefByRole(Snapshot, TEXT("textbox"), TEXT(""));

				USlateInspectorToolset59::Type(TextBoxRef, TEXT("abc"));
				ASSERT_THAT(IsTrue(TestPanel->TextBox->GetText().ToString() == TEXT("abc")));

				// BackSpace should delete the last character.
				// Re-focus via Type to ensure the editable text has focus.
				USlateInspectorToolset59::Type(TextBoxRef, TEXT(""));
				// SEditableText deletes on the char event, not the key down, so that alone must count as handled.
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::PressKey(TEXT("BackSpace"))));
				ASSERT_THAT(IsTrue(TestPanel->TextBox->GetText().ToString() == TEXT("ab")));
			});
	}

	TEST_METHOD(PressKey_SelectAllAndDelete)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString TextBoxRef = FindRefByRole(Snapshot, TEXT("textbox"), TEXT(""));

				USlateInspectorToolset59::Type(TextBoxRef, TEXT("hello world"));
				ASSERT_THAT(IsTrue(TestPanel->TextBox->GetText().ToString() == TEXT("hello world")));

				// Ctrl+A to select all, then Delete to clear.
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::PressKey(TEXT("Ctrl+A"))));
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::PressKey(TEXT("Delete"))));
				ASSERT_THAT(IsTrue(TestPanel->TextBox->GetText().ToString().IsEmpty()));
			});
	}

	// -- Screenshot tests --------------------------------------------

	TEST_METHOD(Screenshot_CapturesWidget)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString ButtonRef = FindRefByRole(SnapshotTestWindow(), TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				const FToolsetImage Image = USlateInspectorToolset59::Screenshot(ButtonRef);
				ASSERT_THAT(IsTrue(!Image.MimeType.IsEmpty() && !Image.Data.IsEmpty()));
			});
	}

	TEST_METHOD(Screenshot_CropsWidgetReachingPastItsWindow)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				// Starts above and left of the window and ends past its right and bottom edges.
				TSharedPtr<SButton> OverflowButton;
				const TSharedRef<SWindow> SmallWindow = AddCanvasWindow(FVector2D(-40.0, -40.0), FVector2D(300.0, 300.0), OverflowButton);
				ON_SCOPE_EXIT { SmallWindow->RequestDestroyWindow(); };

				const FString ButtonRef = UE::WidgetHelper::FWidgetRefCache::Get().GetOrAssignRef(OverflowButton.ToSharedRef(), FName("b")).ToString();
				const FToolsetImage Image = USlateInspectorToolset59::Screenshot(ButtonRef);
				ASSERT_THAT(IsTrue(!Image.MimeType.IsEmpty() && !Image.Data.IsEmpty()));
			});
	}

	TEST_METHOD(Screenshot_CropsByLayoutPositionUnderRenderTransform)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				// Laid out above and left of the window, but translated into it when rendered: the crop must follow the
				// layout position TakeScreenshot offsets by, not where the button is drawn.
				TSharedPtr<SButton> TransformedButton;
				const TSharedRef<SWindow> SmallWindow = AddCanvasWindow(FVector2D(-40.0, -40.0), FVector2D(60.0, 60.0), TransformedButton);
				ON_SCOPE_EXIT { SmallWindow->RequestDestroyWindow(); };
				TransformedButton->SetRenderTransform(FSlateRenderTransform(FVector2f(50.0f, 50.0f)));
				FSlateApplication::Get().Tick();

				const FString ButtonRef = UE::WidgetHelper::FWidgetRefCache::Get().GetOrAssignRef(TransformedButton.ToSharedRef(), FName("b")).ToString();
				const FToolsetImage Image = USlateInspectorToolset59::Screenshot(ButtonRef);
				ASSERT_THAT(IsTrue(!Image.MimeType.IsEmpty() && !Image.Data.IsEmpty()));
			});
	}

	TEST_METHOD(Screenshot_WidgetOutsideItsWindowReturnsNoImage)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				TSharedPtr<SButton> OutsideButton;
				const TSharedRef<SWindow> SmallWindow = AddCanvasWindow(FVector2D(500.0, 500.0), FVector2D(50.0, 50.0), OutsideButton);
				ON_SCOPE_EXIT { SmallWindow->RequestDestroyWindow(); };

				const FString ButtonRef = UE::WidgetHelper::FWidgetRefCache::Get().GetOrAssignRef(OutsideButton.ToSharedRef(), FName("b")).ToString();
				ASSERT_THAT(IsTrue(USlateInspectorToolset59::Screenshot(ButtonRef).Data.IsEmpty()));
			});
	}

	TEST_METHOD(Screenshot_UnknownRefReturnsNoImage)
	{
		ASSERT_THAT(IsTrue(USlateInspectorToolset59::Screenshot(TEXT("b999999")).Data.IsEmpty()));
	}

	// -- Hover tests -------------------------------------------------

	TEST_METHOD(Hover_ReturnsTrue)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
				ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

				const bool bHovered = USlateInspectorToolset59::Hover(ButtonRef);
				ASSERT_THAT(IsTrue(bHovered));
			});
	}

	TEST_METHOD(Hover_InvalidRef)
	{
		const bool bHovered = USlateInspectorToolset59::Hover(TEXT("nonexistent_ref"));
		ASSERT_THAT(IsTrue(!bHovered));
	}

	// -- PressKey tests ----------------------------------------------

	TEST_METHOD(PressKey_Escape)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				ASSERT_THAT(IsTrue(FocusInputRecorder()));

				const bool bPressed = USlateInspectorToolset59::PressKey(TEXT("Escape"));
				ASSERT_THAT(IsTrue(bPressed));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastKeyDown == EKeys::Escape));
				ASSERT_THAT(IsTrue(Recorded.LastKeyChar == TCHAR('\x1b')));
			});
	}

	TEST_METHOD(PressKey_Tab)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				ASSERT_THAT(IsTrue(FocusInputRecorder()));

				const bool bPressed = USlateInspectorToolset59::PressKey(TEXT("Tab"));
				ASSERT_THAT(IsTrue(bPressed));

				const FSlateInspectorToolsetRecordedInput& Recorded = TestPanel->GetInputRecorder()->GetRecorded();
				ASSERT_THAT(IsTrue(Recorded.LastKeyDown == EKeys::Tab));
				ASSERT_THAT(IsTrue(Recorded.LastKeyChar == TCHAR('\t')));
			});
	}

	// -- SelectOption tests ------------------------------------------

	TEST_METHOD(SelectOption_ChangesCombobox)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				ASSERT_THAT(IsTrue(*TestPanel->SelectedComboOption == TEXT("Alpha")));

				const FString Snapshot = SnapshotTestWindow();
				const FString ComboRef = FindRefByRole(Snapshot, TEXT("combobox"), TEXT(""));
				ASSERT_THAT(IsTrue(!ComboRef.IsEmpty()));

				const bool bSelected = USlateInspectorToolset59::SelectOption(ComboRef, TEXT("Beta"));
				ASSERT_THAT(IsTrue(bSelected));
				ASSERT_THAT(IsTrue(*TestPanel->SelectedComboOption == TEXT("Beta")));
			});
	}

	TEST_METHOD(SelectOption_InvalidOption)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString ComboRef = FindRefByRole(Snapshot, TEXT("combobox"), TEXT(""));
				ASSERT_THAT(IsTrue(!ComboRef.IsEmpty()));

				const bool bSelected = USlateInspectorToolset59::SelectOption(ComboRef, TEXT("Nonexistent"));
				ASSERT_THAT(IsTrue(!bSelected));
				// Original selection unchanged.
				ASSERT_THAT(IsTrue(*TestPanel->SelectedComboOption == TEXT("Alpha")));
			});
	}

	// -- FillForm tests ----------------------------------------------

	TEST_METHOD(FillForm_TextBox)
	{
		const FString Snapshot = SnapshotTestWindow();
		const FString TextBoxRef = FindRefByRole(Snapshot, TEXT("textbox"), TEXT(""));
		ASSERT_THAT(IsTrue(!TextBoxRef.IsEmpty()));

		TArray<FSlateInspectorToolsetFormField59> Fields;
		FSlateInspectorToolsetFormField59 Field;
		Field.Ref = TextBoxRef;
		Field.Value = TEXT("form value");
		Field.FieldType = TEXT("textbox");
		Fields.Add(Field);

		const bool bFilled = USlateInspectorToolset59::FillForm(Fields);
		ASSERT_THAT(IsTrue(bFilled));
		ASSERT_THAT(IsTrue(TestPanel->TextBox->GetText().ToString() == TEXT("form value")));
	}

	TEST_METHOD(FillForm_Checkbox)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString CheckBoxRef = FindRefByRole(Snapshot, TEXT("checkbox"), TEXT(""));
				ASSERT_THAT(IsTrue(!CheckBoxRef.IsEmpty()));
				ASSERT_THAT(IsTrue(!TestPanel->CheckBox->IsChecked()));

				TArray<FSlateInspectorToolsetFormField59> Fields;
				FSlateInspectorToolsetFormField59 Field;
				Field.Ref = CheckBoxRef;
				Field.Value = TEXT("true");
				Field.FieldType = TEXT("checkbox");
				Fields.Add(Field);

				const bool bFilled = USlateInspectorToolset59::FillForm(Fields);
				ASSERT_THAT(IsTrue(bFilled));
				ASSERT_THAT(IsTrue(TestPanel->CheckBox->IsChecked()));
			});
	}

	TEST_METHOD(FillForm_CheckboxAlreadyInDesiredState)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString CheckBoxRef = FindRefByRole(Snapshot, TEXT("checkbox"), TEXT(""));
				ASSERT_THAT(IsTrue(!TestPanel->CheckBox->IsChecked()));

				TArray<FSlateInspectorToolsetFormField59> Fields;
				FSlateInspectorToolsetFormField59 Field;
				Field.Ref = CheckBoxRef;
				Field.Value = TEXT("false");
				Field.FieldType = TEXT("checkbox");
				Fields.Add(Field);

				// Should succeed without clicking (already unchecked).
				const bool bFilled = USlateInspectorToolset59::FillForm(Fields);
				ASSERT_THAT(IsTrue(bFilled));
				ASSERT_THAT(IsTrue(!TestPanel->CheckBox->IsChecked()));
			});
	}

	// -- WaitFor tests -----------------------------------------------

	TEST_METHOD(WaitFor_FindsExistingText)
	{
		const bool bFound = USlateInspectorToolset59::WaitFor(TEXT("Static Label"));
		ASSERT_THAT(IsTrue(bFound));
	}

	TEST_METHOD(WaitFor_DoesNotFindMissingText)
	{
		const bool bFound = USlateInspectorToolset59::WaitFor(TEXT("This Text Does Not Exist"));
		ASSERT_THAT(IsTrue(!bFound));
	}

	TEST_METHOD(WaitFor_TextGone)
	{
		const bool bGone = USlateInspectorToolset59::WaitFor(TEXT(""), TEXT("This Text Does Not Exist"));
		ASSERT_THAT(IsTrue(bGone));
	}

	TEST_METHOD(WaitFor_TextGoneFails)
	{
		const bool bGone = USlateInspectorToolset59::WaitFor(TEXT(""), TEXT("Static Label"));
		ASSERT_THAT(IsTrue(!bGone));
	}

	// -- Windows tests -----------------------------------------------

	TEST_METHOD(Windows_ListContainsTestWindow)
	{
		const FString WindowList = USlateInspectorToolset59::Windows(TEXT("list"));
		ASSERT_THAT(IsTrue(WindowList.Contains(TEXT("SlateInspectorToolset Test Window"))));
	}

	// -- Ref cache tests ---------------------------------------------

	TEST_METHOD(PurgeExpired_CleansDeadRefs)
	{
		TSharedRef<STextBlock> EphemeralWidget = SNew(STextBlock).Text(FText::FromString(TEXT("Ephemeral")));
		const FName Ref = UE::WidgetHelper::FWidgetRefCache::Get().GetOrAssignRef(EphemeralWidget, FName("x"));
		ASSERT_THAT(IsTrue(!Ref.IsNone()));

		TSharedPtr<SWidget> Resolved = UE::WidgetHelper::FWidgetRefCache::Get().ResolveRef(Ref);
		ASSERT_THAT(IsTrue(Resolved.IsValid()));

		// Drop all references so the widget is destroyed.
		Resolved.Reset();
		EphemeralWidget = SNew(STextBlock);

		UE::WidgetHelper::FWidgetRefCache::Get().PurgeExpired();
		TSharedPtr<SWidget> ResolvedAfterPurge = UE::WidgetHelper::FWidgetRefCache::Get().ResolveRef(Ref);
		ASSERT_THAT(IsTrue(!ResolvedAfterPurge.IsValid()));
	}

	TEST_METHOD(FindRef_ReturnsEmptyForUnknownWidget)
	{
		TSharedRef<STextBlock> Widget = SNew(STextBlock);
		const FName Ref = UE::WidgetHelper::FWidgetRefCache::Get().FindRef(Widget);
		ASSERT_THAT(IsTrue(Ref.IsNone()));
	}

	TEST_METHOD(ResolveRef_ReturnsNullForUnknownRef)
	{
		TSharedPtr<SWidget> Resolved = UE::WidgetHelper::FWidgetRefCache::Get().ResolveRef(FStringView(TEXT("zzz999")));
		ASSERT_THAT(IsTrue(!Resolved.IsValid()));
	}

	// -- UMG back-mapping tests --------------------------------------

	TEST_METHOD(GetUMGInfo_InvalidRef)
	{
		const FSlateInspectorToolsetUMGInfoResult Result = USlateInspectorToolset59::GetUMGInfo(TEXT("nonexistent_ref"));
		ASSERT_THAT(IsTrue(Result.Error.Contains(TEXT("Could not resolve ref"))));
		ASSERT_THAT(IsTrue(Result.Widget == nullptr));
	}

	TEST_METHOD(GetUMGInfo_ErrorsOnPureSlateWidget)
	{
		const FString Snapshot = SnapshotTestWindow();
		const FString ButtonRef = FindRefByRole(Snapshot, TEXT("button"), TEXT("Test Button"));
		ASSERT_THAT(IsTrue(!ButtonRef.IsEmpty()));

		// The test panel is pure Slate, so there is no UMG metadata to resolve.
		const FSlateInspectorToolsetUMGInfoResult Result = USlateInspectorToolset59::GetUMGInfo(ButtonRef);
		ASSERT_THAT(IsTrue(!Result.Error.IsEmpty()));
		ASSERT_THAT(IsTrue(Result.Widget == nullptr));
	}

	TEST_METHOD(GetUMGInfo_ResolvesUMGButton)
	{
		// A UMG UButton attaches FReflectionMetaData to its SButton on TakeWidget.
		TStrongObjectPtr<UButton> Button(NewObject<UButton>());
		TSharedRef<SWidget> SlateButton = Button->TakeWidget();

		const FName Ref = UE::WidgetHelper::FWidgetRefCache::Get().GetOrAssignRef(SlateButton, FName("b"));
		const FSlateInspectorToolsetUMGInfoResult Result = USlateInspectorToolset59::GetUMGInfo(Ref.ToString());

		ASSERT_THAT(IsTrue(Result.Error.IsEmpty()));
		ASSERT_THAT(IsTrue(Result.WidgetClass == TEXT("Button")));
		ASSERT_THAT(IsTrue(Result.ResolvedVia == TEXT("self")));
		ASSERT_THAT(IsTrue(Result.Widget == Button.Get()));
	}

	TEST_METHOD(UMGResolver_ResolvesSelf)
	{
		TStrongObjectPtr<UButton> Button(NewObject<UButton>());
		TSharedRef<SWidget> SlateButton = Button->TakeWidget();

		const TOptional<FSlateInspectorToolsetUMGInfo> Info = FSlateInspectorToolsetUMGResolver::Resolve(SlateButton, /* bAllowParentFallback */ false);
		ASSERT_THAT(IsTrue(Info.IsSet()));
		ASSERT_THAT(IsTrue(!Info->bResolvedOnParent));
		ASSERT_THAT(IsTrue(Info->WidgetClassName == TEXT("Button")));
	}

	TEST_METHOD(UMGResolver_UnsetForPureSlate)
	{
		TSharedRef<SWidget> PureSlate = SNew(STextBlock).Text(FText::FromString(TEXT("Pure")));
		const TOptional<FSlateInspectorToolsetUMGInfo> Info = FSlateInspectorToolsetUMGResolver::Resolve(PureSlate);
		ASSERT_THAT(IsTrue(!Info.IsSet()));
	}

	TEST_METHOD(Snapshot_ContainsUMGAnnotation)
	{
		TStrongObjectPtr<UButton> Button(NewObject<UButton>());
		TSharedRef<SWidget> SlateButton = Button->TakeWidget();

		TSharedRef<SWindow> UMGWindow = SNew(SWindow)
			.Title(FText::FromString(TEXT("SlateInspectorToolset UMG Window")))
			.ScreenPosition(FVector2D(600, 100))
			.ClientSize(FVector2D(200, 100))
			[
				SlateButton
			];
		FSlateApplication::Get().AddWindow(UMGWindow);
		FSlateApplication::Get().Tick();

		// Destroy the window however this test exits: an ASSERT_THAT failure below
		// returns early, and a window left registered with FSlateApplication would
		// show up in later tests' all-window snapshots.
		ON_SCOPE_EXIT
		{
			UMGWindow->RequestDestroyWindow();
		};

		// An empty ref would make Snapshot cover every window, letting unrelated
		// editor UMG satisfy the assertion below.
		const FString WindowRef = FindRefByRole(USlateInspectorToolset59::Snapshot(TEXT(""), 30), TEXT("window"), TEXT("SlateInspectorToolset UMG Window"));
		ASSERT_THAT(IsTrue(!WindowRef.IsEmpty()));

		const FString Snapshot = USlateInspectorToolset59::Snapshot(WindowRef, 30);
		ASSERT_THAT(IsTrue(Snapshot.Contains(TEXT("[umg="))));
	}

	// -- Focus tool --------------------------------------------------

	TEST_METHOD(Focus_SetsKeyboardFocus)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const FString Snapshot = SnapshotTestWindow();
				const FString TextBoxRef = FindRefByRole(Snapshot, TEXT("textbox"), TEXT(""));
				ASSERT_THAT(IsTrue(!TextBoxRef.IsEmpty()));

				const bool bFocused = USlateInspectorToolset59::Focus(TextBoxRef);
				ASSERT_THAT(IsTrue(bFocused));
				ASSERT_THAT(IsTrue(TestPanel->TextBox->HasKeyboardFocus()));
			});
	}

	// -- Gamepad key simulation --------------------------------------

	TEST_METHOD(PressKey_GamepadKeyReturnsTrue)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const TOptional<int32> GamepadUserIndex = GetDefaultInputDeviceUserIndex();
				ASSERT_THAT(IsTrue(GamepadUserIndex.IsSet()));
				ASSERT_THAT(IsTrue(FocusInputRecorder(GamepadUserIndex.GetValue())));

				const bool bPressed = USlateInspectorToolset59::PressKey(TEXT("Gamepad_FaceButton_Bottom"));
				ASSERT_THAT(IsTrue(bPressed));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().LastKeyDown == EKeys::Gamepad_FaceButton_Bottom));
			});
	}

	TEST_METHOD(PressKey_GamepadDPadReturnsTrue)
	{
		TestCommandBuilder
			.StartWhen([this]() { return SlateComponent->HaveTicksElapsed(3); })
			.Then([this]()
			{
				const TOptional<int32> GamepadUserIndex = GetDefaultInputDeviceUserIndex();
				ASSERT_THAT(IsTrue(GamepadUserIndex.IsSet()));
				ASSERT_THAT(IsTrue(FocusInputRecorder(GamepadUserIndex.GetValue())));

				const bool bPressed = USlateInspectorToolset59::PressKey(TEXT("Gamepad_DPad_Down"));
				ASSERT_THAT(IsTrue(bPressed));
				ASSERT_THAT(IsTrue(TestPanel->GetInputRecorder()->GetRecorded().LastKeyDown == EKeys::Gamepad_DPad_Down));
			});
	}

	// -- Helpers -----------------------------------------------------

	/** Focus the panel's input recorder, which handles every key, for UserIndex; false when it could not be found or focused. */
	bool FocusInputRecorder(int32 UserIndex = 0)
	{
		const FString RecorderRef = FindRefByRole(SnapshotTestWindow(), TEXT("generic"), SSlateInspectorToolsetInputRecorder::GetLabel());
		return !RecorderRef.IsEmpty() && USlateInspectorToolset59::Focus(RecorderRef, UserIndex);
	}

	/** The Slate user that gamepad keys sent with the default UserIndex reach; unset when no user is mapped to the default device. */
	static TOptional<int32> GetDefaultInputDeviceUserIndex()
	{
		return FSlateApplication::Get().GetUserIndexForInputDevice(IPlatformInputDeviceMapper::Get().GetDefaultInputDevice());
	}

	/** Snapshot only the test window's subtree so we don't accidentally
	 *  match editor widgets (e.g. editor checkboxes vs. our test checkbox). */
	FString SnapshotTestWindow()
	{
		// First get the window ref from a full snapshot.
		const FString FullSnapshot = USlateInspectorToolset59::Snapshot(TEXT(""), 30);
		const FString WindowRef = FindRefByRole(FullSnapshot, TEXT("window"), TEXT("SlateInspectorToolset Test Window"));
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

	TArray<FString> FindAllRefsByRole(const FString& Snapshot, const FString& Role)
	{
		TArray<FString> Refs;
		TArray<FString> Lines;
		Snapshot.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			FString Trimmed = Line.TrimStartAndEnd();
			if (!Trimmed.StartsWith(Role))
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
					Refs.Add(Trimmed.Mid(RefStart, RefEnd - RefStart));
				}
			}
		}
		return Refs;
	}

	/** Show a 200x200 window holding Content, laid out and painted once. */
	TSharedRef<SWindow> AddContentWindow(const TSharedRef<SWidget>& Content)
	{
		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(FText::FromString(TEXT("SlateInspectorToolset Snapshot Window")))
			.ClientSize(FVector2D(200.0, 200.0))
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			[
				Content
			];
		FSlateApplication::Get().AddWindow(Window);
		Window->MoveWindowTo(FVector2D(150.0, 150.0));
		FSlateApplication::Get().Tick();
		return Window;
	}

	/** Show a 100x100 window holding one button placed on a canvas at Position with Size, laid out and painted once. */
	TSharedRef<SWindow> AddCanvasWindow(const FVector2D& Position, const FVector2D& Size, TSharedPtr<SButton>& OutButton)
	{
		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(FText::FromString(TEXT("SlateInspectorToolset Screenshot Window")))
			.ClientSize(FVector2D(100.0, 100.0))
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			[
				SNew(SCanvas)
				+ SCanvas::Slot()
				.Position(Position)
				.Size(Size)
				[
					SAssignNew(OutButton, SButton)
					.Text(FText::FromString(TEXT("Canvas Button")))
				]
			];
		FSlateApplication::Get().AddWindow(Window);
		Window->MoveWindowTo(FVector2D(150.0, 150.0));
		FSlateApplication::Get().Tick();
		return Window;
	}
};
