// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

#include "HyperAIStudioBot.generated.h"

class AActor;
class FHyperAIBotAimer;
class FHyperAIBotBrain;
class UGeometryCollectionComponent;
class UInputAction;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
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
	/** One press of InputAction through Enhanced Input, so the game's own bindings run. With Aim, aimed at Target or Location first. */
	Press,
	/** Hold InputAction for Seconds. With Aim, aimed at Target or Location for all of it. */
	Hold,
	/** Face Target, then press InputAction, or the game's action named like Interact or Use. With Aim, aims at it the game's way instead. */
	Interact,
	/** Save the game view as a PNG named after Text. Without a screen, as in verification copies, it renders the player's camera. */
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
	UiAssert,
	/**
	 * Play for Goal the way a person plays: read the game's controls, move with its move input relative to the camera,
	 * look where it goes, steer round what blocks the way and jump what it can, and aim with a person's reaction time,
	 * turn speed and settling aim before pressing the game's attack. Reports what happened and writes a map of it.
	 */
	Play,
	/** Another local player joins, with the controller and pawn the game mode gives it; standing at Location when set. Later steps act as it with PlayerIndex. */
	AddPlayer,
	/**
	 * Damage Chaos geometry collections: a strain field at Location (Target's middle when Location is 0,0,0) reaching
	 * Radius cm, of Value strain (twice the highest damage threshold of what it reaches, unless set), applied again every
	 * frame for Seconds when set. Target names the collections; empty takes every one within Radius.
	 */
	Damage,
	/**
	 * Fire a 40 cm, 200 kg test ball at Target or Location: from the player's view at Value cm/s (3000 unless set), or,
	 * with Range set, dropped from Range cm above it. What it hits takes collision damage.
	 */
	Fire,
	/**
	 * The pieces broken off Target's geometry collections (every one in the level when empty), compared with Value; the
	 * detail counts the clusters broken and says which pieces fell how far.
	 */
	AssertBroken,
	/**
	 * Aim at Target or Location the way the game reads aim (Aim picks how, Auto unless set) for Seconds, 0.5 unless
	 * set, then check the game's own aim points there.
	 */
	AimAt
};

/** How the bot aims: the way the game under test reads its player's aim. */
UENUM(BlueprintType)
enum class EHyperAIBotAim : uint8
{
	/** Press and Hold do not aim, and Interact turns the view to its target. AimAt and Play take it as Auto. */
	Off,
	/** Cursor, Stick or Look, from what the game shows of itself; the result says which, and why. */
	Auto,
	/**
	 * For games that aim where the mouse cursor is: the game window's own cursor position is put on the target's
	 * screen point, with the mouse movement a hand would make, while the real cursor stays where the user has it.
	 */
	Cursor,
	/** For gamepad aim: the right stick pushed toward the target, relative to the camera. */
	Stick,
	/** For games that aim where the view looks: the control rotation turned to the target, as LookAt does. */
	Look
};

/** What a Play step plays for. */
UENUM(BlueprintType)
enum class EHyperAIBotGoal : uint8
{
	/** Get to Target or Location, round obstacles and over low ones. Passes within Radius. */
	Reach,
	/** Pick up every actor matching Target, nearest first; presses the interact action when walking onto one is not enough. */
	Collect,
	/** Defeat every actor matching Target: get within Range, keep moving, aim and attack until its health reaches 0 or it is gone. Target Player1 plays PvP. */
	Defeat,
	/** Stay alive for Seconds, keeping away from and dodging whatever matches Target. */
	Survive,
	/** Wander for Seconds toward places not yet visited; reports coverage, stuck spots and falls. */
	Explore,
	/** Keep within Radius of Target for Seconds, as a teammate or an escort would; sprints to catch up. */
	Follow,
	/** Run from whatever matches Target, sprinting, until at least Range away (1500 cm unless set), and stay away for Seconds. */
	Flee
};

/** One step of the playtest bot. Fields a step does not use stay empty. */
USTRUCT(BlueprintType)
struct HYPERAISTUDIOBOT_API FHyperAIBotStep
{
	GENERATED_BODY()

	UPROPERTY()
	EHyperAIBotAction Action = EHyperAIBotAction::Wait;

	/** WaitUntil only: the assertion to wait for (AssertProperty, AssertNear, AssertOnScreen, AssertExists, AssertGone or AssertBroken). */
	UPROPERTY()
	EHyperAIBotAction Until = EHyperAIBotAction::AssertExists;

	/** An actor by label, name, tag or class name, or Player, PlayerController, PlayerState, GameState or GameMode. Empty means Player. */
	UPROPERTY()
	FString Target;

	/** AssertNear only: the actor Target must be near. */
	UPROPERTY()
	FString Other;

	/** A world location, for MoveTo, LookAt and Play Reach without a Target, AssertNear without Other, where AddPlayer's player stands, and Damage's and Fire's point. */
	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	/** A property path on Target, such as Score or HealthComponent.Health. Location, Rotation, Velocity and Speed work on any actor. */
	UPROPERTY()
	FString Property;

	/** ==, !=, <, <=, >, >= or contains. */
	UPROPERTY()
	FString Compare = TEXT("==");

	/** Expected value; for Press and Hold, the input value, such as 1 or 0,1 for a 2D axis; Damage's strain; Fire's speed in cm/s. */
	UPROPERTY()
	FString Value;

	/** An InputAction's asset path, or a loaded action's name such as IA_Jump. For Play, the attack to use instead of the one it finds. */
	UPROPERTY()
	FString InputAction;

	/** A UI widget's name, optionally after its user widget: WBP_HUD.ScoreText. */
	UPROPERTY()
	FString Widget;

	/** Capture and recording names; the text UiType types. */
	UPROPERTY()
	FString Text;

	/** Wait, Hold and Sample length; RecordStart's interval; how long Play Survive and Explore play. */
	UPROPERTY()
	float Seconds = 0.f;

	/** MoveTo's arrival distance and AssertNear's distance, in cm. */
	UPROPERTY()
	float Radius = 100.f;

	/** MoveTo, Jump, WaitUntil and Play Reach, Collect and Defeat give up after this many seconds. */
	UPROPERTY()
	float TimeoutSeconds = 10.f;

	/** Play only: what to play for. */
	UPROPERTY()
	EHyperAIBotGoal Goal = EHyperAIBotGoal::Reach;

	/** The local player who acts: 0 is the first, and AddPlayer adds more. Targets Player0, Player1... name each player's pawn. */
	UPROPERTY()
	int32 PlayerIndex = 0;

	/** Play only: 0 plays like a new player, slow to react with loose aim; 1 like a skilled one. */
	UPROPERTY()
	float Skill = 0.5f;

	/** In cm. Play: Defeat's attack distance, 1500 unless set, closing in when hits from there do nothing; Survive's distance to keep, 2500 unless set; Flee's distance to get away, 1500 unless set. Fire: drop the ball from this high above the point instead. */
	UPROPERTY()
	float Range = 0.f;

	/** Play only: the property path to read health from, on the player and its targets. Empty finds one named like Health or HP. */
	UPROPERTY()
	FString HealthProperty;

	/** Play only: the same seed plays the same way. */
	UPROPERTY()
	int32 Seed = 0;

	/** Play, MoveTo and Hold: keep running while the next steps run, such as the other player of a duel. Later results say how it went. */
	UPROPERTY()
	bool bBackground = false;

	/**
	 * AimAt, Press, Hold, Interact and Play Defeat: how to aim. Press and Hold aim at Target or Location while they
	 * fire and for as long as they hold; Off leaves them unaimed.
	 */
	UPROPERTY()
	EHyperAIBotAim Aim = EHyperAIBotAim::Off;

	/** Aimed steps: seconds of a moving target's travel to aim ahead of it, 0 to 2. */
	UPROPERTY()
	float Lead = 0.f;

	/** Aimed steps: how far off, in degrees, the game's aim direction may point and still pass; 10 unless set. A world aim point must be within Radius. */
	UPROPERTY()
	float Tolerance = 0.f;
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

	/** How the running step and any background steps are going, for another step's report; empty when nothing runs. */
	FString Progress() const;

	/** Checks a step from outside before it runs; OutError says what is wrong. */
	static bool Validate(const FHyperAIBotStep& Step, FString& OutError);
	static FString ActionName(EHyperAIBotAction Action);

private:
	FHyperAIBotResult Evaluate(EHyperAIBotAction Assertion);
	FHyperAIBotResult TickMove();
	FHyperAIBotResult TickSample();
	FHyperAIBotResult CaptureTo(const FString& Path);
	bool RenderPlayerView(TArray<FColor>& OutPixels, FIntPoint& OutSize);
	FHyperAIBotResult AddPlayer();
	FHyperAIBotResult StartBackground(const FHyperAIBotStep& InStep);
	void TickBackground();
	/** A finished result, with what the background steps did meanwhile. */
	FHyperAIBotResult WithBackground(FHyperAIBotResult Result);
	FString Label() const;
	void StopHold();
	void StopRecording();
	/** An aimed step: starts its aim at Target, or at Point without one. */
	FHyperAIBotResult StartAim(const AActor* Target, const FVector& Point);
	/** An aimed step, a frame at a time: aim, press once the game's aim is on the target, hold, then check. */
	FHyperAIBotResult TickAimed();
	/** Ends an aimed step with the game's aim checked; a cursor then rests where it is until the bot stops. */
	FHyperAIBotResult FinishAimed(const FString& What);
	double AimToleranceDegrees() const;

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

	/** Renders the player's camera when the game has no screen. */
	TStrongObjectPtr<USceneCaptureComponent2D> SceneCapture;
	TStrongObjectPtr<UTextureRenderTarget2D> SceneTarget;

	/** A Play step's player. */
	TUniquePtr<FHyperAIBotBrain> Brain;

	/** A Damage step's strain field, applied again each frame for its Seconds. */
	TArray<TWeakObjectPtr<UGeometryCollectionComponent>> Damaged;
	FVector DamageAt = FVector::ZeroVector;
	float DamageStrain = 0.f;

	/** The running aimed step's aim. */
	TUniquePtr<FHyperAIBotAimer> Aimer;
	/** The cursor where the last aimed step left it, held still until the bot stops, as a hand resting on the mouse. */
	TUniquePtr<FHyperAIBotAimer> RestingCursor;
	/** An aimed step's input action, what it aims at, and when it pressed. */
	TWeakObjectPtr<const UInputAction> AimedAction;
	FString AimedName;
	double PressedAt = 0.0;
	int32 FramesSincePress = 0;
	/** Steps running alongside, each with its own executor. */
	TArray<TUniquePtr<FHyperAIBotExecutor>> Background;
	/** What background steps did since a result last reported it. */
	TArray<FString> BackgroundNotes;
};
