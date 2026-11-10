// Games by Hyper 2026.

// UE 5.8: HyperAI Studio addition, not in Epic's 5.9 source. Checks that SlateInspectorToolset59's input cannot reach
// HyperAI Studio's own UI (SlateInspectorToolsetHyperAIGuard). HyperAI Studio's widgets are recognised by their
// source file or, as here, by a type name containing "HyperAIStudio", so SHyperAIStudioGuardTestHost stands in for
// one of its panels: a button inside it must not be clickable, focusable for typing, or reachable by a raw touch,
// while the same button outside it still works.

#include "CQTest.h"
#include "Components/CQTestSlateComponent.h"

#include "Framework/Application/SlateApplication.h"
#include "SlateInspectorToolset59.h"
#include "WHRefCache.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

/** Stands in for a HyperAI Studio panel: its type name contains "HyperAIStudio". */
class SHyperAIStudioGuardTestHost : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioGuardTestHost) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		ChildSlot[InArgs._Content.Widget];
	}
};

TEST_CLASS(SlateInspectorToolsetHyperAIGuardTest, "AIAssistant.Toolsets.SlateInspectorToolset.HyperAIGuard")
{
	TUniquePtr<FCQTestSlateComponent> SlateComponent;
	TSharedPtr<SWindow> TestWindow;
	TSharedPtr<SButton> GuardedButton;
	TSharedPtr<SButton> OpenButton;
	int32 GuardedClicks = 0;
	int32 OpenClicks = 0;

	BEFORE_EACH()
	{
		SlateComponent = MakeUnique<FCQTestSlateComponent>();
		GuardedClicks = 0;
		OpenClicks = 0;

		TestWindow = SNew(SWindow)
			.Title(FText::FromString(TEXT("HyperAI guard test window")))
			.ScreenPosition(FVector2D(100, 100))
			.ClientSize(FVector2D(300, 200))
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.FocusWhenFirstShown(true)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHyperAIStudioGuardTestHost)
					[
						SAssignNew(GuardedButton, SButton)
						.OnClicked_Lambda([this]() { ++GuardedClicks; return FReply::Handled(); })
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Approve (guarded)")))
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 40, 0, 0)
				[
					SAssignNew(OpenButton, SButton)
					.OnClicked_Lambda([this]() { ++OpenClicks; return FReply::Handled(); })
					[
						SNew(STextBlock).Text(FText::FromString(TEXT("Open button")))
					]
				]
			];

		FSlateApplication::Get().AddWindow(TestWindow.ToSharedRef());
		TestWindow->MoveWindowTo(FVector2D(100, 100));
		TestWindow->BringToFront();
		FSlateApplication::Get().Tick();
	}

	AFTER_EACH()
	{
		USlateInspectorToolset59::ReleaseHeldInput();
		if (TestWindow.IsValid())
		{
			TestWindow->RequestDestroyWindow();
			TestWindow.Reset();
		}
		GuardedButton.Reset();
		OpenButton.Reset();
		SlateComponent.Reset();
	}

	static FString RefOf(const TSharedPtr<SButton>& Button)
	{
		return UE::WidgetHelper::FWidgetRefCache::Get().GetOrAssignRef(Button.ToSharedRef(), FName(TEXT("b"))).ToString();
	}

	static FVector2D CenterOf(const TSharedPtr<SButton>& Button)
	{
		const FGeometry& Geometry = Button->GetTickSpaceGeometry();
		return Geometry.LocalToAbsolute(Geometry.GetLocalSize() * 0.5);
	}

	TEST_METHOD(Click_RefusesHyperAIStudioWidgets_AndStillClicksOthers)
	{
		ASSERT_THAT(IsFalse(USlateInspectorToolset59::Click(RefOf(GuardedButton))));
		ASSERT_THAT(AreEqual(0, GuardedClicks));
		ASSERT_THAT(IsTrue(USlateInspectorToolset59::LastRefusal().Contains(TEXT("HyperAI Studio"))));

		ASSERT_THAT(IsTrue(USlateInspectorToolset59::Click(RefOf(OpenButton))));
		ASSERT_THAT(AreEqual(1, OpenClicks));
	}

	TEST_METHOD(PressKey_RefusedWhileFocusIsInHyperAIStudio)
	{
		FSlateApplication::Get().SetAllUserFocus(GuardedButton, EFocusCause::SetDirectly);
		ASSERT_THAT(IsFalse(USlateInspectorToolset59::PressKey(TEXT("Enter"))));
		ASSERT_THAT(AreEqual(0, GuardedClicks));
		ASSERT_THAT(IsTrue(USlateInspectorToolset59::LastRefusal().Contains(TEXT("HyperAI Studio"))));
	}

	TEST_METHOD(RawTouch_AtAHyperAIStudioWidget_IsDropped)
	{
		const FVector2D Guarded = CenterOf(GuardedButton);
		USlateInspectorToolset59::TouchStart(FString(), Guarded.X, Guarded.Y, 0);
		USlateInspectorToolset59::TouchEnd(Guarded.X, Guarded.Y, 0);
		ASSERT_THAT(AreEqual(0, GuardedClicks));

		const FVector2D Open = CenterOf(OpenButton);
		ASSERT_THAT(IsTrue(USlateInspectorToolset59::TouchStart(FString(), Open.X, Open.Y, 0)));
		ASSERT_THAT(IsTrue(USlateInspectorToolset59::TouchEnd(Open.X, Open.Y, 0)));
		ASSERT_THAT(AreEqual(1, OpenClicks));
	}

	TEST_METHOD(Snapshot_HidesHyperAIStudioWidgets)
	{
		const FString WindowRef = UE::WidgetHelper::FWidgetRefCache::Get().GetOrAssignRef(TestWindow.ToSharedRef(), FName(TEXT("w"))).ToString();
		const FString Snapshot = USlateInspectorToolset59::Snapshot(WindowRef, 30);
		ASSERT_THAT(IsFalse(Snapshot.Contains(TEXT("Approve (guarded)"))));
		ASSERT_THAT(IsTrue(Snapshot.Contains(TEXT("HyperAI Studio UI"))));
		ASSERT_THAT(IsTrue(Snapshot.Contains(TEXT("Open button"))));
	}
};
