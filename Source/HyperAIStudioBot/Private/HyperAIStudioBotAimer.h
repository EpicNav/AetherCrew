// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "HyperAIStudioBot.h"
#include "HyperAIStudioBotAim.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class APlayerController;
class FSceneViewport;
class UGameViewportClient;
class UWorld;

/**
 * Holds one player's aim on an actor or a point, for as long as it lives, through what the game reads: its window's
 * cursor, its right stick or its view. It acts at the start of every frame of its world, before anything in the game
 * ticks, so the game's aim code reads this frame's aim. The newest aimer of a player is the one that acts. Letting go
 * puts back what it changed.
 *
 * The cursor never moves the real one. FSceneViewport keeps the cursor position a game reads (GetMousePosition,
 * DeprojectMousePositionToWorld, GetHitResultUnderCursor all read FSceneViewport::CachedCursorPos), and it updates it
 * from the mouse events Slate delivers to it. The aimer delivers one itself: a mouse move at the target's screen point,
 * straight to FSceneViewport::OnMouseMove, the ISlateViewport handler Slate calls. That bypasses FSlateApplication, so
 * no Slate widget, hover state or platform cursor sees it. The one line in that handler that touches the real cursor
 * (FSlateApplication::SetCursorPos, when the window captured the mouse and hid its cursor) is ruled out first: see
 * CursorMoveIsSafe.
 */
class FHyperAIBotAimer
{
public:
	FHyperAIBotAimer(UWorld& InWorld, int32 InPlayerIndex);
	~FHyperAIBotAimer();

	FHyperAIBotAimer(const FHyperAIBotAimer&) = delete;
	FHyperAIBotAimer& operator=(const FHyperAIBotAimer&) = delete;

	/** Takes the mode, choosing one when Requested is Off or Auto. False, with a code and why, when it cannot aim that way here. */
	bool Start(EHyperAIBotAim Requested, FString& OutCode, FString& OutWhy);
	/** Aims at Actor, ahead of it by Lead seconds of its travel; without an actor, at Point. */
	void SetTarget(const AActor* Actor, const FVector& Point, float Lead);
	/** Aims this far off the target, as a person's aim is off a little before it settles. Checks still measure from the target. */
	void SetOffset(const FVector& InOffset) { Offset = InOffset; }
	/** Keeps the cursor still where it is, as a hand resting on the mouse; other modes let go. */
	void Rest();
	/** Lets go and puts back what it changed. */
	void Stop();

	EHyperAIBotAim GetMode() const { return Mode; }
	/** What the mode was chosen from, for a report. */
	const FString& GetWhy() const { return Why; }
	/** The target's point for this mode, ahead by Lead: what the game's aim is checked against. */
	FVector TargetPoint() const;
	/** Frames it has aimed. */
	int32 Frames() const { return Applied; }
	/** The game's own aim is on the target; without anything to read, the aim has been in place for two frames. */
	bool OnTarget(double ToleranceDegrees, double ToleranceCm) const;
	/** Checks the game's own aim against the target. False with why when it points elsewhere; bOutRead says whether there was anything to read. */
	bool Check(double ToleranceDegrees, double ToleranceCm, FString& OutDetail, bool& bOutRead) const;
	/** What it did: the mode and why, the cursor's point or the stick, and anything that got in the way. */
	FString Report() const;
	/** The aim as drawn over a recording. */
	HyperAIStudio::Bot::Aim::FView GetView() const { return LastView; }
	UWorld* GetWorld() const { return World.Get(); }
	int32 GetPlayerIndex() const { return PlayerIndex; }

private:
	void Apply(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds);
	bool ApplyCursor(APlayerController& Controller, const FVector& Point, float DeltaSeconds);
	void ApplyStick(APlayerController& Controller, const FVector& Point, float DeltaSeconds);
	void ApplyLook(APlayerController& Controller, const FVector& Point);
	/** Puts the window's cursor on Pixel through the viewport's own mouse-move handler. False when the window holds the real mouse. */
	bool PlaceCursor(FSceneViewport& Viewport, UGameViewportClient& Client, const FIntPoint& Pixel);
	void Restore();
	bool IsCurrent() const;

	TWeakObjectPtr<UWorld> World;
	int32 PlayerIndex = 0;
	EHyperAIBotAim Mode = EHyperAIBotAim::Look;
	FString Why;
	bool bStarted = false;
	bool bResting = false;
	FDelegateHandle TickHandle;

	TWeakObjectPtr<const AActor> Target;
	bool bHasTarget = false;
	bool bHasActor = false;
	/** The target's last point: a gone actor is aimed at where it was. */
	FVector Point = FVector::ZeroVector;
	FVector Offset = FVector::ZeroVector;
	float Lead = 0.f;

	int32 Applied = 0;
	/** The cursor: where it put it, where it was before it first did, and what got in the way. */
	FIntPoint CursorPixel = FIntPoint(-1, -1);
	FIntPoint CursorBefore = FIntPoint(-1, -1);
	bool bTookCursor = false;
	FIntPoint WindowSize = FIntPoint::ZeroValue;
	int32 HeldFrames = 0;
	int32 BehindFrames = 0;
	int32 EdgeFrames = 0;
	int32 Readback = 0;
	FString ReadbackNote;
	/** The stick: its last tilt, and the camera yaw it was tilted against. */
	FVector2D Stick = FVector2D::ZeroVector;
	double StickYaw = 0.0;
	bool bStickPushed = false;
	/** CommonInput's current input type when the aim started, which the stick does not change. */
	FString InputType;

	HyperAIStudio::Bot::Aim::FView LastView;
};
