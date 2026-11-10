// Games by Hyper 2026.

// UE 5.8: HyperAI Studio addition; see SlateInspectorToolsetHyperAIGuard.h.

#include "SlateInspectorToolsetHyperAIGuard.h"

#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Input/Events.h"
#include "Layout/WidgetPath.h"
#include "Logging/StructuredLog.h"
#include "Misc/Paths.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

DEFINE_LOG_CATEGORY_STATIC(LogSlateInspectorHyperAIGuard, Log, All);

namespace UE::SlateInspectorToolset::HyperAIGuard
{
namespace Private
{
	int32 GScopeDepth = 0;

	bool IsHyperAIStudioSourceFile(const FName Location)
	{
		if (Location.IsNone())
		{
			return false;
		}
		FString Path = Location.GetPlainNameString();
		Path.ReplaceCharInline(TEXT('\\'), TEXT('/'));
		if (Path.Contains(TEXT("/Source/HyperAIStudio"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		// Every source file of the HyperAIStudio* modules is named HyperAI* or SHyperAI*, so this also holds when
		// the compiler trims __FILE__ to a relative path.
		const FString FileName = FPaths::GetCleanFilename(Path);
		return FileName.StartsWith(TEXT("HyperAI"), ESearchCase::IgnoreCase)
			|| FileName.StartsWith(TEXT("SHyperAI"), ESearchCase::IgnoreCase);
	}

	bool IsPathProtected(const FWidgetPath& Path)
	{
		if (!Path.IsValid())
		{
			return false;
		}
		if (IsProtectedWindow(Path.GetWindow()))
		{
			return true;
		}
		for (int32 Index = 0; Index < Path.Widgets.Num(); ++Index)
		{
			if (IsHyperAIStudioCreated(Path.Widgets[Index].Widget.Get()))
			{
				return true;
			}
		}
		return false;
	}

	// Slate routes a pointer event to the pointer's captor when there is one, else along the path under the pointer.
	bool IsPointerEventProtected(const FPointerEvent& Event)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}
		if (const TSharedPtr<FSlateUser> User = FSlateApplication::Get().GetUser(Event.GetUserIndex()))
		{
			if (const TSharedPtr<SWidget> Captor = User->GetPointerCaptor(Event.GetPointerIndex()))
			{
				return IsProtectedWidget(Captor);
			}
		}
		return IsProtectedPoint(Event.GetScreenSpacePosition());
	}

	void NoteDropped(const TCHAR* EventName)
	{
		UE_LOGFMT(LogSlateInspectorHyperAIGuard, Warning,
			"SlateInspectorToolset: dropped a synthetic {Event} event headed to HyperAI Studio's own UI", EventName);
	}

	/** Drops synthetic input headed to HyperAI Studio while a SlateInspectorToolset tool is injecting it. */
	class FInputProcessor final : public IInputProcessor
	{
	public:
		virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

		virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
		{
			return Drop(TEXT("key down"), IsFocusProtected(InKeyEvent.GetUserIndex()));
		}

		virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
		{
			return Drop(TEXT("key up"), IsFocusProtected(InKeyEvent.GetUserIndex()));
		}

		virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& InAnalogInputEvent) override
		{
			return Drop(TEXT("analog"), IsFocusProtected(InAnalogInputEvent.GetUserIndex()));
		}

		virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			return Drop(TEXT("pointer move"), IsPointerEventProtected(MouseEvent));
		}

		virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			return Drop(TEXT("pointer down"), IsPointerEventProtected(MouseEvent));
		}

		virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			return Drop(TEXT("pointer up"), IsPointerEventProtected(MouseEvent));
		}

		virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			return Drop(TEXT("double click"), IsPointerEventProtected(MouseEvent));
		}

		virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override
		{
			return Drop(TEXT("wheel"), IsPointerEventProtected(InWheelEvent));
		}

		virtual const TCHAR* GetDebugName() const override { return TEXT("SlateInspectorToolsetHyperAIGuard"); }

	private:
		static bool Drop(const TCHAR* EventName, const bool bProtected)
		{
			// Only synthetic input from a running tool is filtered; the user's own input is never touched.
			if (GScopeDepth <= 0 || !bProtected)
			{
				return false;
			}
			NoteDropped(EventName);
			return true;
		}
	};

	TSharedPtr<FInputProcessor> GInputProcessor;

	void EnsureInputProcessor()
	{
		if (!GInputProcessor.IsValid() && FSlateApplication::IsInitialized())
		{
			GInputProcessor = MakeShared<FInputProcessor>();
			// First in line, so no other pre-processor sees (or acts on) input this guard drops.
			FSlateApplication::Get().RegisterInputPreProcessor(GInputProcessor, 0);
		}
	}
}

FString GetRefusalReason()
{
	return TEXT("the target is part of HyperAI Studio's own UI, which SlateInspectorToolset may not drive");
}

bool IsHyperAIStudioCreated(const SWidget& Widget)
{
	return Private::IsHyperAIStudioSourceFile(Widget.GetCreatedInLocation())
		|| Widget.GetTypeAsString().Contains(TEXT("HyperAIStudio"), ESearchCase::IgnoreCase);
}

bool IsProtectedWindow(const TSharedPtr<SWindow>& Window)
{
	if (!Window.IsValid())
	{
		return false;
	}
	if (Window->GetType() == EWindowType::Notification)
	{
		return true;
	}
	return IsHyperAIStudioCreated(*Window);
}

bool IsProtectedWidget(const TSharedPtr<SWidget>& Widget)
{
	if (!Widget.IsValid())
	{
		return false;
	}
	for (TSharedPtr<SWidget> Current = Widget; Current.IsValid(); Current = Current->GetParentWidget())
	{
		if (IsHyperAIStudioCreated(*Current))
		{
			return true;
		}
		if (Current->Advanced_IsWindow() && IsProtectedWindow(StaticCastSharedPtr<SWindow>(Current)))
		{
			return true;
		}
	}
	if (FSlateApplication::IsInitialized())
	{
		// The parent chain can stop short of the window (a menu or tooltip host), so check the window too.
		if (IsProtectedWindow(FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef())))
		{
			return true;
		}
	}
	return false;
}

bool IsProtectedPoint(const FVector2D& ScreenPoint)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	FSlateApplication& SlateApplication = FSlateApplication::Get();
	const FWidgetPath Path = SlateApplication.LocateWindowUnderMouse(
		ScreenPoint, SlateApplication.GetInteractiveTopLevelWindows(), /*bIgnoreEnabledStatus=*/true);
	return Private::IsPathProtected(Path);
}

bool IsFocusProtected(const int32 SlateUserIndex)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	const TSharedPtr<SWidget> Focused = SlateUserIndex == INDEX_NONE
		? FSlateApplication::Get().GetKeyboardFocusedWidget()
		: FSlateApplication::Get().GetUserFocusedWidget(SlateUserIndex);
	if (IsProtectedWidget(Focused))
	{
		return true;
	}
	// A key with no focused widget goes to the active window (its commands and menus).
	return !Focused.IsValid() && IsProtectedWindow(FSlateApplication::Get().GetActiveTopLevelWindow());
}

FInjectionScope::FInjectionScope()
{
	Private::EnsureInputProcessor();
	++Private::GScopeDepth;
}

FInjectionScope::~FInjectionScope()
{
	--Private::GScopeDepth;
}

void Shutdown()
{
	if (Private::GInputProcessor.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(Private::GInputProcessor);
		}
		Private::GInputProcessor.Reset();
	}
}
}
