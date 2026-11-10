// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioBot.h"

class APawn;
class UWorld;

/**
 * Aiming the way the game under test aims. A game reads its player's aim from one of three places: where the mouse
 * cursor is in its window (top-down and isometric games deproject it onto the world), where the right stick points
 * (gamepad aim, relative to the camera), or where the view looks (the control rotation, as shooters do). The bot aims
 * through the one the game reads, and checks the game's own aim afterwards.
 */
namespace HyperAIStudio::Bot::Aim
{
	/** What the game shows of how it aims, read from its player. */
	struct FEvidence
	{
		/** The player has a window. Verification copies have none, and so no cursor. */
		bool bViewport = false;
		/** A second local player, sharing the first player's window and its one cursor. */
		bool bSharedWindow = false;
		/** The player controller's ShouldShowMouseCursor. */
		bool bCursorShown = false;
		/** The first of GetAimWorldLocation, GetAimDirection or GetAimDirectionFlat on the pawn or a component, such as AimComponent.GetAimDirection. */
		FString Reader;
		/** Where the camera is: its pitch in degrees, and its distance from the pawn in cm. */
		double CameraPitch = 0.0;
		double CameraDistance = 0.0;
		/** The action the mapping contexts give Mouse XY, and whether it is named like a look. */
		FString MouseAction;
		bool bMouseLooks = false;
		/** The action the right stick drives. */
		FString StickAction;
		/** CommonInput's current input type (MouseAndKeyboard, Gamepad or Touch); empty when the game has no CommonInput. */
		FString InputType;
	};

	struct FChoice
	{
		EHyperAIBotAim Mode = EHyperAIBotAim::Look;
		/** Why, in words: the evidence for and against the cursor. */
		FString Why;
	};

	/**
	 * Auto's choice. Stick when CommonInput says the player is on a gamepad and the right stick drives an action.
	 * Otherwise the cursor and the view are weighed: a shown cursor (2), an aim of the game's own (1) and a camera
	 * looking steeply down from afar (3) count for the cursor; a hidden cursor (1), a camera close behind the player (1)
	 * and Mouse XY turning the view (2) count for the view. Without a window, or for a second player sharing one, there
	 * is no cursor to aim with.
	 */
	HYPERAISTUDIOBOT_API FChoice Choose(const FEvidence& Evidence);
	/** Reads the evidence from the PlayerIndex-th player of World. */
	HYPERAISTUDIOBOT_API FEvidence Gather(UWorld& World, int32 PlayerIndex);

	/** The right stick, X right and Y up, at full tilt toward Direction, for a camera facing CameraYaw: up is where the camera faces. */
	HYPERAISTUDIOBOT_API FVector2D StickToward(double CameraYaw, const FVector& Direction);

	/** How far, in pixels, the cursor moves round its target each frame. */
	constexpr double NudgePixels = 4.0;
	/**
	 * Where the cursor rests round its target on Frame: NudgePixels off it, a quarter turn further each frame, so that
	 * from one frame to the next it moves about 5.7 pixels. A game that counts the cursor as aiming only while it moves
	 * keeps counting it.
	 */
	HYPERAISTUDIOBOT_API FVector2D Nudge(int32 Frame);

	/** One reading of the game's own aim. */
	struct FAimReading
	{
		/** Where it came from, such as AimComponent.GetAimDirectionFlat. */
		FString Source;
		/** A world point, from GetAimWorldLocation; otherwise a direction. */
		bool bPoint = false;
		FVector Value = FVector::ZeroVector;
	};

	/** The game's aim: GetAimWorldLocation, GetAimDirection and GetAimDirectionFlat, found by name on the pawn or any of its components. */
	HYPERAISTUDIOBOT_API TArray<FAimReading> ReadGameAim(APawn& Pawn);

	/**
	 * How far Reading is from Aimed, seen from From: for a direction, the degrees between their bearings, level; for a
	 * point, the level distance in cm. Games that put the aim on the floor under the cursor read level.
	 */
	HYPERAISTUDIOBOT_API double OffBy(const FAimReading& Reading, const FVector& From, const FVector& Aimed);

	/** An aim the bot holds now, to draw over a recording. */
	struct FView
	{
		int32 PlayerIndex = 0;
		EHyperAIBotAim Mode = EHyperAIBotAim::Look;
		/** The window's size, and the aim and the player in its pixels. */
		FIntPoint ViewportSize = FIntPoint::ZeroValue;
		FVector2D Point = FVector2D::ZeroVector;
		FVector2D From = FVector2D::ZeroVector;
		bool bPointOnScreen = false;
		bool bFromOnScreen = false;
	};

	/** The aims the bot holds in World now. */
	HYPERAISTUDIOBOT_API TArray<FView> Active(const UWorld& World);

	HYPERAISTUDIOBOT_API FString ModeName(EHyperAIBotAim Mode);
}
