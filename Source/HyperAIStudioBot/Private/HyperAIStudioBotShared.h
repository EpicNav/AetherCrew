// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioBot.h"

class AActor;
class APawn;
class APlayerController;
class UEnhancedInputLocalPlayerSubsystem;
class UInputAction;
class UWorld;
struct FInputActionValue;

/** What the bot's steps and its brain share: finding players and actors, reading them, and the game's input. */
namespace HyperAIStudio::Bot::Private
{
	using FResult = FHyperAIBotResult;

	FString Vec(const FVector& Value);
	FString Describe(const UObject* Object);
	/** A name's words, lower case: IA_ComboAttack is ia, combo and attack. */
	TArray<FString> Words(const FString& Name);

	/** The Index-th player controller, in the order the players joined. */
	APlayerController* PlayerController(UWorld& World, int32 Index);
	APawn* PlayerPawn(UWorld& World, int32 Index);
	/** Player0 to Player7: that player's pawn; OutIndex says which. */
	bool IsPlayerName(const FString& Name, int32& OutIndex);

	/**
	 * Player or Pawn, PlayerController and PlayerState mean the acting player's; Player0 to Player7 a player's pawn;
	 * GameState and GameMode the game's. Otherwise an actor: an exact name wins, then a label, a tag, a class.
	 */
	UObject* FindTarget(UWorld& World, const FString& Name, int32 PlayerIndex);
	/** Every live actor the name matches by name, label, tag or class, up to Max (64 unless set); a player name gives that player's pawn. */
	TArray<AActor*> FindAll(UWorld& World, const FString& Name, int32 PlayerIndex, int32 Max = 64);

	struct FReading
	{
		bool bOk = false;
		bool bNumeric = false;
		double Number = 0.0;
		FString Text;
		FString Error;
	};

	/**
	 * Path is names joined by dots: properties, into objects and structs; a component of an actor; and last, a const
	 * or pure function with no inputs, such as GetHealth. A gameplay attribute reads as its current value.
	 */
	FReading ReadProperty(UObject* Root, const FString& Path);
	/** Where a player would read the actor's health: a number of its own named like Health or HP, one on a component, or a GetHealth. Empty when it has none. */
	FString FindHealth(AActor& Actor);

	/** Reading compared with Expected by Compare (==, !=, <, <=, >, >= or contains): numbers as numbers, other text ignoring case. */
	bool Holds(const FReading& Reading, const FString& Compare, const FString& Expected);

	UEnhancedInputLocalPlayerSubsystem* InputFor(UWorld& World, int32 PlayerIndex, FString& OutCode, FString& OutError);
	/** Applies the mapping contexts the game added now, rather than at Enhanced Input's next tick. */
	void ApplyMappings(UEnhancedInputLocalPlayerSubsystem& Input);
	const UInputAction* FindInputAction(const FString& NameOrPath);
	FInputActionValue InputValue(const UInputAction& Action, const FString& Text);

	/** What an action does for the player, by the names games give their actions. */
	enum class ERole : uint8
	{
		Move,
		Look,
		Jump,
		Attack,
		Aim,
		Reload,
		Crouch,
		Sprint,
		Dodge,
		Interact,
		Heal,
		Count
	};
	const TCHAR* RoleName(ERole Role);

	struct FControls
	{
		const UInputAction* Roles[static_cast<int32>(ERole::Count)] = {};
		/** Every mapped action with its role and keys, as a controls screen lists them. */
		FString Summary;

		const UInputAction* Get(const ERole Role) const { return Roles[static_cast<int32>(Role)]; }
	};

	/** The actions the player's mapping contexts map now. */
	FControls ReadControls(UWorld& World, int32 PlayerIndex);
}
