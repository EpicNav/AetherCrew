---
name: unreal-playtest
description: Play the game in Unreal and check that it works, with HyperAI's playtest bot. It plays like a person through the game's own controls, moving round obstacles, jumping, collecting, aiming and attacking, dodging, exploring, and fighting other players in PvP. It also presses input actions, clicks UI, waits for things to happen, and captures screenshots, then reports evidence per step with a map of each play. hyper_verify_run plays many such cases at once in under a minute. Use it to verify gameplay, such as a pickup adding score, a door opening, an enemy dying when shot, or a HUD updating.
---
# Playtest with the HyperAI bot

## Run a playtest

A playtest is one call to `hyper_playtest_run` (toolset `HyperAIStudio.HyperAIStudioPIEPlaytestToolset`). It starts Play In Editor, runs your steps in order, stops play, and returns at once.

1. Call `hyper_operation_wait` with the operationId and `timeoutMs` up to 120000. It returns when the run ends.
   - If the user approves edits, the run first shows `awaiting_user_approval`. Pass `bWaitThroughApproval: true` to wait through it.
2. Call `hyper_playtest_status` with the operationId and `pageSize` 64 to get each step's evidence line.
   - Evidence names the action, what the bot saw, and any file it wrote.

The first failing step ends the run. Its status and diagnostic say why, such as `assertion_failed`, `stuck`, `timed_out` or `not_found`, with positions and values.

```json
{"request": {"operationId": "coin-check-01", "mode": "StartInProcess", "teardown": "StopOwnedSession", "pieInstance": -1,
  "wholeTimeoutMs": 120000, "maxEvidenceItems": 64, "maxOutputBytes": 262144,
  "steps": [
    {"stepId": "look", "kind": "Bot", "timeoutMs": 5000, "bot": {"action": "Survey", "target": "BP_Coin"}},
    {"stepId": "walk", "kind": "Bot", "timeoutMs": 20000, "bot": {"action": "MoveTo", "target": "BP_Coin", "radius": 50, "timeoutSeconds": 15}},
    {"stepId": "gone", "kind": "Bot", "timeoutMs": 5000, "bot": {"action": "WaitUntil", "until": "AssertGone", "target": "BP_Coin", "timeoutSeconds": 3}},
    {"stepId": "score", "kind": "Bot", "timeoutMs": 5000, "bot": {"action": "AssertProperty", "target": "PlayerState", "property": "Score", "compare": ">=", "value": "1"}},
    {"stepId": "shot", "kind": "Bot", "timeoutMs": 5000, "bot": {"action": "Capture", "text": "after-pickup"}}]}}
```

### Fields

- **Every step:** a unique `stepId`, `kind: "Bot"`, a `bot` object, and `timeoutMs`, the hard cap for that step (at most 30000).
- **Leave out** any `bot` field the action does not use.
- **Changes stay in the PIE world.** Nothing in the level being edited changes.

## Verify many things at once

`hyper_verify_run` (same toolset) plays up to `limit` cases at the same time, each in its own copy of the open level. One call answers with every case's result within `deadlineSeconds`, 60 at most.
- `hyper_verify_calibrate` with `{"bMeasure": false}` reports `limit` for this machine and level.
- Unsaved edits are included, and nothing in the level changes.
- A case starts where the game starts the player. It runs its steps in order, and the first failing step ends it.

```json
{"request": {"deadlineSeconds": 60, "cases": [
  {"name": "coin-scores", "timeoutSeconds": 20, "steps": [
    {"action": "MoveTo", "target": "BP_Coin", "radius": 50, "timeoutSeconds": 15},
    {"action": "WaitUntil", "until": "AssertGone", "target": "BP_Coin", "timeoutSeconds": 3},
    {"action": "AssertProperty", "target": "GameMode", "property": "Score", "compare": "==", "value": "10"}]},
  {"name": "door-opens", "timeoutSeconds": 20, "steps": [
    {"action": "MoveTo", "target": "BP_Door", "radius": 150, "timeoutSeconds": 15},
    {"action": "WaitUntil", "until": "AssertProperty", "target": "BP_Door", "property": "Open", "compare": "==", "value": "true", "timeoutSeconds": 3}]}]}}
```

- **Result:** every case reports `passed`, `failed`, `timed_out`, `stopped` or `not_started`, with a diagnostic and each step's evidence line. A step that wrote a picture names it in `file`: a Capture, or a Play step's map.
- **timeoutSeconds** is simulated game time for the case, 1 to the deadline less 5. Simulation usually runs faster than real time.
- **Steps are the bot actions below**, except UiClick, UiType, UiAssert and AssertOnScreen. The copies have no screen, so check those with `hyper_playtest_run`. Capture and recordings render the player's camera instead.
- **A batch over `limit` is refused.** Run several batches one after another.
- **When one case per check helps:** make each case prove one thing, so a failure names the broken requirement.

## Play like a player

`Play` plays toward a goal the way a person would:
- **Controls:** it reads the game's controls and moves with the game's move input, relative to the camera, looking where it goes.
- **Moving:** it steers round what blocks the way, jumps what it can, and backs off to try another way when stuck.
- **Fighting:** it reacts, turns and aims like a person, with an aim that settles, and attacks when the crosshair is on the target. It strafes, hops and dodges meanwhile.

| goal | fields | passes when |
|---|---|---|
| Reach | target or location, radius (100 cm), timeoutSeconds | The player got within radius. Fails `stuck`, saying where. |
| Collect | target: a tag or class naming all of them, timeoutSeconds | Every one was picked up. Presses the interact action when walking onto one is not enough. Fails `not_collected` with the ones that stayed. |
| Defeat | target, skill, range, healthProperty, inputAction, timeoutSeconds | Every target's health reached 0 or it is gone. See the failures below. |
| Survive | target (the threats), seconds, range (2500 cm) | The player is still alive after seconds. It keeps its distance from threats and weaves. Fails `died`. |
| Explore | seconds | The player did not die or fall out of the world. Reports the squares it visited, where it got stuck and where it fell. |
| Follow | target, radius (set it: 300 to 500 cm), seconds | The player kept within radius of the target 80% of the time after the first 2 s. It sprints to catch up. Fails `fell_behind`. |
| Flee | target (the threats), range (1500 cm), seconds | The player is at least range from the nearest threat at the end. It runs, sprinting, round what is in the way. Fails `caught`. |

A Defeat fails with one of these codes:
- `defeated`: the player died first.
- `no_damage`: hits did nothing, even from close range.
- `no_attack_action`: the controls have no attack.

The fields:
- **skill** is 0 to 1, 0.5 unless set. 0 plays like a new player, slow to react with loose aim; 1 plays like a skilled one.
- **range** is Defeat's attack distance, 1500 cm unless set. When hits from there do nothing, it moves closer, as for a melee weapon. A player hurt below a third of its health backs off while it fights.
- **seed** makes the same case play the same way.

What a Play step reports and reads:
- **The result:** what happened. It gives shots and how many had the crosshair on target, damage dealt and taken, and the health left. It also counts jumps, dodges and obstacles gone round, lists stuck spots, and names the inputs it used.
- **The map:** every Play step writes one, a picture of the level from above. It shows where the player went, the targets, where it shot from, and where it got stuck. The map is the step's file: open it and look.
- **Health:** it reads health as a player reads a health bar. That is a number named like Health or HP, on the actor or one of its components, a gameplay attribute, or a GetHealth function. When the game names it differently, pass it in healthProperty, such as `HealthComponent.CurrentHP`.
- **Controls:** these are the actions the game's mapping contexts map, known by the names games give them, such as Move, Jump, Fire or Attack, Dodge and Interact. Survey lists them with their keys. Pass inputAction to attack with a different action.

### More players, and PvP

- `AddPlayer` adds a local player, with the controller and pawn the game mode gives it. With location set, the player stands there. In Play In Editor it gets its own part of the screen.
- `playerIndex` is the player a step acts as; 0 is the first. The targets `Player0` and `Player1` name each player's pawn.
- `bBackground: true` keeps a Play, MoveTo or Hold step running while the next steps run. Each later result ends with what it did meanwhile.

A duel, with the second player 15 m ahead of the start:

```json
{"name": "duel", "timeoutSeconds": 50, "steps": [
  {"action": "AddPlayer", "location": {"x": 1500, "y": 0, "z": 100}},
  {"action": "Wait", "seconds": 0.5},
  {"action": "Play", "goal": "Defeat", "target": "Player0", "playerIndex": 1, "skill": 0.4, "bBackground": true, "timeoutSeconds": 40},
  {"action": "Play", "goal": "Defeat", "target": "Player1", "skill": 0.7, "timeoutSeconds": 40},
  {"action": "Capture", "text": "duel-over"}]}
```

The duel passes when Player0 wins. It fails `defeated` when Player1 wins, and the report tells both sides.

## Bot actions

| action | fields | passes when |
|---|---|---|
| Play | goal, and the goal's fields above | The goal was met. Writes a map of the play. |
| AddPlayer | location (optional) | A local player joined with a pawn. |
| Survey | target (optional) | Always passes. Reports the player and the camera, an actor census, the controls with their keys, the player's health, and the other pawns near it. With a target, also its distance and whether the navmesh reaches it. |
| MoveTo | target or location, radius (100 cm), timeoutSeconds (10) | The player is within radius. Follows the navmesh when there is one. Fails `stuck` after 3 s without progress. |
| LookAt | target or location | The view faces it. |
| Jump | timeoutSeconds | The character left the ground and landed. Characters only. |
| Press | inputAction (`IA_Jump`, or an asset path), value | One press was injected through Enhanced Input, so the game's own bindings run. |
| Hold | inputAction, seconds, value | The action was held for that long. `value: "0,1"` is forward for a 2D move action. |
| Interact | target, inputAction (optional) | The player faced the target and pressed its interact action. Without one, the action named like Interact or Use. |
| Wait | seconds | That much time passed. |
| WaitUntil | until (one of the Assert actions), its fields, timeoutSeconds | The assertion became true. |
| AssertProperty | target, property, compare (`==` `!=` `<` `<=` `>` `>=` `contains`), value | The comparison holds. Numbers compare as numbers. |
| AssertNear | target, other or location, radius | The target is within radius. |
| AssertOnScreen | target | The target is in the camera's view and not hidden. |
| AssertExists / AssertGone | target | The target is there, or destroyed. |
| Sample | target, property (optional), seconds | Always passes. Reports first, last, min and max. Without a property, speed and distance moved. |
| Capture | text | A PNG was written. Without a screen, as in verification copies, it renders the player's camera. Its path is in the evidence: open it and look. |
| RecordStart / RecordStop | text, seconds (interval, 0.5) | A frame was saved every interval into a folder, until RecordStop. |
| UiClick / UiType / UiAssert | widget (`WBP_HUD.ScoreText`), text, compare, value | A button clicked through Slate, text typed, or a widget visible with matching text. |

**Targets** are an actor's label, name, tag or class name (`BP_Coin`), or one of Player, PlayerController, PlayerState, GameState or GameMode. An empty target means Player. For a step with playerIndex, those names mean that player's. `Player0` to `Player7` name each player's pawn.

**Properties** are variable names, dotted through components and structs (`HealthComponent.Health`, `Stats.Health`). A path can end in a const function that only reads, such as `HealthComponent.GetHealth`. A gameplay attribute reads as its current value. Any actor also has Location, Rotation, Velocity and Speed.

## Patterns

- **Pickup:** MoveTo the pickup, then WaitUntil AssertGone, then AssertProperty on the score. Add UiAssert when a HUD shows it.
- **Door:**
  1. MoveTo within 150 cm.
  2. Interact with the door.
  3. WaitUntil AssertProperty for its open flag, or WaitUntil AssertNear on its moving part.
  4. Capture.
- **Damage and death:**
  1. MoveTo the hazard.
  2. WaitUntil AssertProperty on `HealthComponent.Health`, with compare `<` and the value.
  3. Then check what death should do.
- **Movement feel:** Hold `IA_Move` with value `0,1` for 2 seconds, then Sample for 1 second, and read the speeds.
- **Combat:** Play Defeat on the enemies' tag or class. Its evidence says whether shots landed and did damage. `no_damage` means hits did nothing, which is a bug in the damage path. Then check what a defeat should do, such as score or loot.
- **Can it be finished:** Play Reach to the goal, or Play Collect on every pickup. A `stuck` result says where the level blocks a player.
- **Level coverage:** Play Explore for 30 to 60 seconds, then look at the map. Its stuck spots and falls are where players get trapped or fall out.
- **Before building anything:** Survey tells you the class names, where the player starts, and whether a target is reachable.

## Rules

- Several checks at once: use `hyper_verify_run`. A single playtest that needs the screen, UI or captures: use `hyper_playtest_run`.
- Only one playtest can play in the editor at a time. A run started while another is playing is refused with `session_already_active`: wait for the first, then run again.
- Look at captures before saying something looks right.
- Report every check with its evidence line. A check you could not run is a failure, with the reason.
