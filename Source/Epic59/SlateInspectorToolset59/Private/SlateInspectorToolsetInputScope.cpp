// Copyright Epic Games, Inc. All Rights Reserved.

#include "SlateInspectorToolsetInputScope.h"

#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Layout/WidgetPath.h"
#include "Widgets/SWidget.h"

#if PLATFORM_MAC
#include "Mac/CocoaThread.h"
#include "Mac/MacApplication.h"
#endif

namespace UE::SlateInspectorToolset::Private
{

// UE 5.8: 5.9 added FSlateApplication::IsUsingPlatformCursorForCursorUser, which compares the cursor user's cursor
// with the platform cursor. 5.8 has the same test inside UsePlatformCursorForCursorUser, but FSlateUser::GetCursor is
// protected (FSlateApplication is its friend), so the member is reached through the standard explicit-instantiation
// access idiom rather than an engine edit.
struct FSlateUserGetCursorTag
{
	using Type = TSharedPtr<ICursor> (FSlateUser::*)() const;
	friend Type GetMember(FSlateUserGetCursorTag);
};

template <typename Tag, typename Tag::Type Member>
struct TSlateUserMemberAccess
{
	friend typename Tag::Type GetMember(Tag) { return Member; }
};

template struct TSlateUserMemberAccess<FSlateUserGetCursorTag, &FSlateUser::GetCursor>;

static bool IsUsingPlatformCursorForCursorUser()
{
	const TSharedPtr<FSlateUser> CursorUser = FSlateApplication::Get().GetCursorUser();
	const TSharedPtr<GenericApplication> PlatformApplication = FSlateApplication::Get().GetPlatformApplication();
	return CursorUser.IsValid() && PlatformApplication.IsValid()
		&& (CursorUser.Get()->*GetMember(FSlateUserGetCursorTag()))() == PlatformApplication->Cursor;
}

#if PLATFORM_MAC
static void MoveMacCursorAndRefreshWindowUnderCursor(const FVector2D& ScreenPosition)
{
	FSlateApplication::Get().SetCursorPos(ScreenPosition);

	// FMacApplication caches the window under the cursor and only re-reads it when Cocoa delivers
	// the next event, which is after this tool call has hit-tested and routed its input. DeferEvent
	// re-reads it first; a nil object queues an empty event that ProcessEvent ignores.
	if (MacApplication)
	{
		MainThreadCall(^{ MacApplication->DeferEvent(nil); }, /* bWait */ true, NSDefaultRunLoopMode);
	}
}
#endif

/** Capture a scope could not restore because the press it opened was still held, handed to the release path instead. */
static TWeakPtr<SWidget> DeferredCaptor;
static bool bDeferredCursorVisible = true;
static bool bHasDeferredCapture = false;

/** Restore a saved capture and cursor visibility, unless a new owner took capture meanwhile or the saved captor is gone. */
static void RestoreSavedCapture(const TWeakPtr<SWidget>& SavedCaptor, bool bSavedCursorVisible)
{
	// An owner the synthetic input left behind -- an open menu, a live drag and drop -- keeps both capture and cursor state.
	TSharedPtr<FSlateUser> CursorUser = FSlateApplication::Get().GetCursorUser();
	if (!CursorUser.IsValid() || CursorUser->HasCursorCapture())
	{
		return;
	}

	if (TSharedPtr<SWidget> Captor = SavedCaptor.Pin())
	{
		FWidgetPath CaptorPath;
		if (FSlateApplication::Get().FindPathToWidget(Captor.ToSharedRef(), CaptorPath))
		{
			CursorUser->SetCursorCaptor(Captor.ToSharedRef(), CaptorPath);
		}
	}

	CursorUser->SetCursorVisibility(bSavedCursorVisible);
}

} // namespace UE::SlateInspectorToolset::Private

FScopedSlateInputRedirect::FScopedSlateInputRedirect(TSharedRef<SWidget> TargetWidget)
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	// Only intervene during a PIE/SIE session. The persistent mouse capture this
	// scope works around is specific to the game viewport (FInputModeGameOnly);
	// editor widgets only capture transiently during a live user drag, which cannot
	// be in flight between synchronous tool calls. Restricting to PIE guarantees we
	// never disturb editor capture state.
	if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
	{
		return;
	}

	// A held button means its captor owns an in-flight press or drag, and taking that capture away would cancel it mid-gesture.
	if (!FSlateApplication::Get().GetPressedMouseButtons().IsEmpty())
	{
		return;
	}

	TSharedPtr<FSlateUser> CursorUser = FSlateApplication::Get().GetCursorUser();
	if (!CursorUser.IsValid() || !CursorUser->HasCursorCapture())
	{
		return;
	}

	TSharedPtr<SWidget> Captor = CursorUser->GetCursorCaptor();
	if (!Captor.IsValid())
	{
		return;
	}

	// Only the target itself holding capture already routes the click to the right
	// place. For anything else -- including an ancestor such as the PIE game viewport
	// that hosts the target UMG widget -- capture routing delivers to the captor, not
	// the descendant under the cursor, so we must release for the click to land.
	if (Captor == TargetWidget)
	{
		return;
	}

	SavedCaptor = Captor;
	bSavedCursorVisible = CursorUser->IsCursorVisible();

	CursorUser->ReleaseCursorCapture();
	bReleasedCapture = true;
}

FScopedSlateInputRedirect::~FScopedSlateInputRedirect()
{
	if (!bReleasedCapture || !FSlateApplication::IsInitialized())
	{
		return;
	}

	using namespace UE::SlateInspectorToolset::Private;

	// Hardware never hands capture back mid-press, and doing so here would steer the rest of the gesture to the old captor.
	if (!FSlateApplication::Get().GetPressedMouseButtons().IsEmpty())
	{
		DeferredCaptor = SavedCaptor;
		bDeferredCursorVisible = bSavedCursorVisible;
		bHasDeferredCapture = true;
		return;
	}

	// Restore the capture we released so the game keeps receiving pointer input once the synthetic events are done.
	RestoreSavedCapture(SavedCaptor, bSavedCursorVisible);
}

void FScopedSlateInputRedirect::RestoreDeferredCapture()
{
	using namespace UE::SlateInspectorToolset::Private;

	if (!bHasDeferredCapture || !FSlateApplication::IsInitialized())
	{
		return;
	}

	bHasDeferredCapture = false;
	const TWeakPtr<SWidget> Captor = DeferredCaptor;
	DeferredCaptor.Reset();

	RestoreSavedCapture(Captor, bDeferredCursorVisible);
}

FScopedSlateDetachedCursor::FScopedSlateDetachedCursor()
{
	// Mac hit-tests against the window under the OS cursor, so it has to keep moving that cursor;
	// see MovePlatformCursorForPointerInput.
	if (PLATFORM_MAC || !FSlateApplication::IsInitialized())
	{
		return;
	}

	// Leave a cursor user that is already on a faux cursor alone (e.g. CommonInput in PIE), so the
	// destructor does not force the platform cursor back and desync whoever swapped it.
	if (!UE::SlateInspectorToolset::Private::IsUsingPlatformCursorForCursorUser() /* UE 5.8 */)
	{
		return;
	}

	// Save the pointer position before detaching; afterwards synthetic input moves Slate's tracked
	// position and the platform cursor position is no longer readable.
	SavedPosition = FSlateApplication::Get().GetCursorPos();

	FSlateApplication::Get().UsePlatformCursorForCursorUser(false);
	bDetached = true;
}

FScopedSlateDetachedCursor::~FScopedSlateDetachedCursor()
{
	// Synthetic input inside the scope may have reattached the platform cursor already; restoring
	// the position then would move the OS cursor.
	if (!bDetached || !FSlateApplication::IsInitialized() || UE::SlateInspectorToolset::Private::IsUsingPlatformCursorForCursorUser() /* UE 5.8 */)
	{
		return;
	}

	// Restore Slate's pointer position before reattaching the platform cursor, so the write goes
	// to the faux cursor and the OS cursor never moves.
	FSlateApplication::Get().SetCursorPos(SavedPosition);
	FSlateApplication::Get().UsePlatformCursorForCursorUser(true);
}

void FScopedSlateDetachedCursor::SetCursorPos(const FVector2D& Position) const
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

#if PLATFORM_MAC
	UE::SlateInspectorToolset::Private::MoveMacCursorAndRefreshWindowUnderCursor(Position);
#else
	if (!UE::SlateInspectorToolset::Private::IsUsingPlatformCursorForCursorUser() /* UE 5.8 */)
	{
		FSlateApplication::Get().SetCursorPos(Position);
	}
#endif
}

namespace UE::SlateInspectorToolset::Private
{

void MovePlatformCursorForPointerInput(const FVector2D& ScreenPosition)
{
#if PLATFORM_MAC
	if (FSlateApplication::IsInitialized())
	{
		MoveMacCursorAndRefreshWindowUnderCursor(ScreenPosition);
	}
#endif
}

} // namespace UE::SlateInspectorToolset::Private
