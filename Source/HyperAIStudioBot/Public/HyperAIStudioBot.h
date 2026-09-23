// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/WeakObjectPtr.h"

#include "HyperAIStudioBot.generated.h"

class UInputAction;
class UWorld;

/** What one bot step does. */
UENUM(BlueprintType)
enum class EHyperAIBotAction : uint8
{
	/** Let the game run for Seconds. */
	Wait,
	/** Check the Until assertion every frame until it passes; fails with the last reading after TimeoutSeconds. */
	WaitUntil,
	/** Walk the player to Target or Location: along the navmesh when there is a path, otherwise straight. */
	MoveTo,
	/** Turn the player's view to Target or Location. */
	LookAt,
	/** Jump and land. Characters only. */
	Jump,
	/** One press of InputAction through Enhanced Input, so the game's own bindings run. */
	Press,
	/** Hold InputAction for Seconds. */
	Hold,
	/** Face Target, then press InputAction, or the game's action named like Interact or Use. */
	Interact,
	/** Save the game view as a PNG named after Text. */
	Capture,
	/** The player, the camera and a census of the world; with Target, whether the player can reach it. */
	Survey,
	/** Sample Property on Target, or its location and speed, every frame for Seconds. */
	Sample,
	/** Property on Target compared with Value. */
	AssertProperty,
	/** Target within Radius of Other, or of Location. */
	AssertNear,
	/** Target in the camera's view and not hidden behind something. */
	AssertOnScreen,
	/** Target exists. */
	AssertExists,
	/** Target is gone, such as a picked-up coin. */
	AssertGone,
	/** Save the game view every Seconds (0.5 unless set) into a folder named after Text, until RecordStop. */
	RecordStart,
	RecordStop,
	/** Click the UI button named Widget, through Slate. */
	UiClick,
	/** Type Text into the UI text box named Widget. */
	UiType,
	/** The UI widget named Widget is visible; with Value, its text compared with Value. */
	UiAssert
};

/** One step of the playtest bot. Fields a step does not use stay empty. */
USTRUCT(BlueprintType)
struct HYPERAISTUDIOBOT_API FHyperAIBotStep
{
	GENERATED_BODY()

	UPROPERTY()
	EHyperAIBotAction Action = EHyperAIBotAction::Wait;

	/** WaitUntil only: the assertion to wait for (AssertProperty, AssertNear, AssertOnScreen, AssertExists or AssertGone). */
	UPROPERTY()
	EHyperAIBotAction Until = EHyperAIBotAction::AssertExists;

	/** An actor by label, name, tag or class name, or Player, PlayerController, PlayerState, GameState or GameMode. Empty means Player. */
	UPROPERTY()
	FString Target;

	/** AssertNear only: the actor Target must be near. */
	UPROPERTY()
	FString Other;

	/** A world location, for MoveTo and LookAt without a Target and AssertNear without Other. */
	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	/** A property path on Target, such as Score or HealthComponent.Health. Location, Rotation, Velocity and Speed work on any actor. */
	UPROPERTY()
	FString Property;

	/** ==, !=, <, <=, >, >= or contains. */
	UPROPERTY()
	FString Compare = TEXT("==");

	/** Expected value; for Press and Hold, the input value, such as 1 or 0,1 for a 2D axis. */
	UPROPERTY()
	FString Value;

	/** An InputAction's asset path, or a loaded action's name such as IA_Jump. */
	UPROPERTY()
	FString InputAction;

	/** A UI widget's name, optionally after its user widget: WBP_HUD.ScoreText. */
	UPROPERTY()
	FString Widget;

	/** Capture and recording names; the text UiType types. */
	UPROPERTY()
	FString Text;

	/** Wait, Hold and Sample length; RecordStart's interval. */
	UPROPERTY()
	float Seconds = 0.f;

	/** MoveTo's arrival distance and AssertNear's distance, in cm. */
	UPROPERTY()
	float Radius = 100.f;

	/** MoveTo, Jump and WaitUntil give up after this many seconds. */
	UPROPERTY()
	float TimeoutSeconds = 10.f;
};

/** How a step went, in words an agent or a person can act on. */
struct HYPERAISTUDIOBOT_API FHyperAIBotResult
{
	enum class EState : uint8
	{
		Running,
		Passed,
		Failed
	};

	EState State = EState::Running;
	/** passed, or why not: assertion_failed, not_found, stuck, timed_out, no_player, viewport_unavailable... */
	FString Code;
	/** What the bot saw: positions, values, counts. */
	FString Detail;
	/** A file the step wrote: a capture's PNG, or a recording's folder. */
	FString File;

	bool IsDone() const { return State != EState::Running; }
	static FHyperAIBotResult Passed(const FString& Detail, const FString& File = FString());
	static FHyperAIBotResult Failed(const FString& Code, const FString& Detail);
};

/**
 * Plays one game world, a step at a time: Begin a step, then call Tick once a frame until the result is done.
 * Input goes through Enhanced Input and movement through the pawn's own movement component, so the game's logic
 * runs as it would for a player. Game thread only. Anything a step leaves running, such as held input or a
 * recording, stops when the executor does.
 */
class HYPERAISTUDIOBOT_API FHyperAIBotExecutor
{
public:
	/** Files the steps write go under OutputDirectory. */
	FHyperAIBotExecutor(UWorld& InWorld, const FString& InOutputDirectory);
	~FHyperAIBotExecutor();

	FHyperAIBotExecutor(const FHyperAIBotExecutor&) = delete;
	FHyperAIBotExecutor& operator=(const FHyperAIBotExecutor&) = delete;

	FHyperAIBotResult Begin(const FHyperAIBotStep& Step);
	FHyperAIBotResult Tick();
	/** Ends the running step and anything left running. */
	void Stop();

	/** Checks a step from outside before it runs; OutError says what is wrong. */
	static bool Validate(const FHyperAIBotStep& Step, FString& OutError);
	static FString ActionName(EHyperAIBotAction Action);

private:
	FHyperAIBotResult Evaluate(EHyperAIBotAction Assertion);
	FHyperAIBotResult TickMove();
	FHyperAIBotResult TickSample();
	FHyperAIBotResult CaptureTo(const FString& Path);
	void StopHold();
	void StopRecording();

	TWeakObjectPtr<UWorld> World;
	FString OutputDirectory;
	FHyperAIBotStep Step;
	bool bRunning = false;
	double StartedAt = 0.0;

	TArray<FVector> Path;
	int32 NextPoint = 0;
	double BestDistance = 0.0;
	double LastProgressAt = 0.0;

	bool bLeftGround = false;
	double StartZ = 0.0;
	double ApexZ = 0.0;

	int32 FramesLeft = 0;
	FString Pressed;
	TWeakObjectPtr<const UInputAction> Held;

	int32 Samples = 0;
	double Minimum = 0.0;
	double Maximum = 0.0;
	double FirstValue = 0.0;
	double LastValue = 0.0;
	FString LastText;
	FVector SampleStart = FVector::ZeroVector;

	FHyperAIBotResult LastCheck;
	int32 Captures = 0;

	FTSTicker::FDelegateHandle Recorder;
	FString RecordFolder;
	int32 RecordFrames = 0;
};
