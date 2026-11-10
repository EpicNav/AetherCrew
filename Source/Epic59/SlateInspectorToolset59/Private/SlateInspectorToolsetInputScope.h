// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class SWidget;

/**
 * RAII scope that lets synthetic pointer events hit-test normally during PIE.
 *
 * When the game viewport holds mouse capture (e.g. FInputModeGameOnly), Slate
 * routes pointer events straight to the captor along its stored capture path and
 * skips hit-testing under the cursor, so a synthetic click aimed at a UMG widget
 * would be swallowed by the viewport.  Note the viewport is an ANCESTOR of the
 * UMG content it hosts, yet capture routing still delivers to the viewport, not
 * the descendant under the cursor -- so we must release whenever anything other
 * than the target itself holds capture.  Nothing is released while a mouse button
 * is held, because that captor owns an in-flight press or drag.  While in scope
 * capture is released, then restored on destruction unless the synthetic input
 * handed capture to something new (a held button, an open menu, a started drag),
 * in which case the new owner keeps both capture and cursor state.  A scope that
 * ends while its own press is still down parks the captor instead of restoring
 * it, since handing capture back mid-press would steer the rest of the gesture
 * away from the widget being pressed; RestoreDeferredCapture() completes it once
 * the last button comes up.  If the original captor is gone or no longer reachable
 * in a widget path, capture cannot be restored but the saved cursor visibility
 * still is, since that is global state the game relies on.
 * It only acts during a PIE/SIE session and is otherwise a no-op, so editor
 * interaction is unaffected.
 */
class FScopedSlateInputRedirect
{
public:

	explicit FScopedSlateInputRedirect(TSharedRef<SWidget> TargetWidget);
	~FScopedSlateInputRedirect();

	/** Hand back capture a scope parked because its press was still held; call it once the last button is released. */
	static void RestoreDeferredCapture();

	FScopedSlateInputRedirect(const FScopedSlateInputRedirect&) = delete;
	FScopedSlateInputRedirect& operator=(const FScopedSlateInputRedirect&) = delete;

private:

	/** The captor released on construction; weak so this scope never keeps a destroyed
	 *  widget alive. Restored on destruction only if still present and visible. */
	TWeakPtr<SWidget> SavedCaptor;

	/** True only when this scope released capture and is responsible for restoring it. */
	bool bReleasedCapture = false;

	/** Cursor visibility saved before releasing capture. */
	bool bSavedCursorVisible = true;
};

/**
 * RAII scope that lets synthetic input move Slate's pointer without moving the OS cursor.
 *
 * Swaps the cursor user's ICursor for FFauxSlateCursor via
 * FSlateApplication::UsePlatformCursorForCursorUser, so SetCursorPos updates Slate's pointer
 * state (hover, menu placement, drag-and-drop) while the platform cursor stays put.
 * On exit, Slate's pointer is restored to where it was on entry, and the next tick synthesizes a
 * move there. Hover and tooltip state set inside the scope is therefore dropped on that tick,
 * which is why Hover moves the OS cursor instead of using this scope. User mouse movement during
 * the scope is not tracked; the next real move re-syncs it.
 * If the cursor user is already on a faux cursor on entry, the scope does nothing, so it never
 * reattaches a platform cursor that someone else detached. If synthetic input inside the scope
 * reattaches the platform cursor, SetCursorPos stops writing and the destructor leaves it alone,
 * so the OS cursor still never moves.
 *
 * On Mac the scope never detaches, and SetCursorPos moves the OS cursor and leaves it there; see
 * UE::SlateInspectorToolset::Private::MovePlatformCursorForPointerInput for why.
 */
class FScopedSlateDetachedCursor
{
public:

	FScopedSlateDetachedCursor();
	~FScopedSlateDetachedCursor();

	/** Moves Slate's pointer; does nothing while the platform cursor is attached, except on Mac,
	 *  where it moves the OS cursor. Use this rather than FSlateApplication::SetCursorPos inside
	 *  the scope. */
	void SetCursorPos(const FVector2D& Position) const;

	FScopedSlateDetachedCursor(const FScopedSlateDetachedCursor&) = delete;
	FScopedSlateDetachedCursor& operator=(const FScopedSlateDetachedCursor&) = delete;

private:

	/** Where the pointer really is, so Slate can be re-synced to it on the way out. */
	FVector2D SavedPosition = FVector2D::ZeroVector;

	/** True only when this scope detached the cursor and owns re-attaching it. */
	bool bDetached = false;
};

namespace UE::SlateInspectorToolset::Private
{

/**
 * On Mac, moves the OS cursor to ScreenPosition and leaves it there; elsewhere does nothing.
 * Call before hit-testing a synthetic pointer action.
 *
 * On Mac, FSlateApplication::LocateWindowUnderMouse only searches the window the platform reports
 * under the real cursor, so synthetic input only reaches the window the OS cursor is over. The
 * report is a cache Cocoa refreshes from later mouse events, so this refreshes it before
 * returning, and never moves the cursor back so the cache stays valid for the next action.
 */
void MovePlatformCursorForPointerInput(const FVector2D& ScreenPosition);

} // namespace UE::SlateInspectorToolset::Private
