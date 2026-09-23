---
name: unreal-playtest
description: Play the game in Unreal and check that it works, with HyperAI's playtest bot. It starts Play In Editor, walks the player to things, jumps, presses the game's own input actions, clicks UI, waits for things to happen, and captures screenshots, then reports evidence per step. Use it to verify gameplay, such as a pickup adding score, a door opening, or a HUD updating.
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

## Bot actions

| action | fields | passes when |
|---|---|---|
| Survey | target (optional) | Always passes. Reports the player, the camera and an actor census. With a target, also its distance and whether the navmesh reaches it. |
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
| Capture | text | A PNG was written. Its path is in the evidence: open it and look. |
| RecordStart / RecordStop | text, seconds (interval, 0.5) | A frame was saved every interval into a folder, until RecordStop. |
| UiClick / UiType / UiAssert | widget (`WBP_HUD.ScoreText`), text, compare, value | A button clicked through Slate, text typed, or a widget visible with matching text. |

**Targets** are an actor's label, name, tag or class name (`BP_Coin`), or one of Player, PlayerController, PlayerState, GameState or GameMode. An empty target means Player.

**Properties** are variable names, dotted through components (`HealthComponent.Health`). Any actor also has Location, Rotation, Velocity and Speed.

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
- **Before building anything:** Survey tells you the class names, where the player starts, and whether a target is reachable.

## Rules

- Only one playtest can play in the editor at a time. A run started while another is playing is refused with `session_already_active`: wait for the first, then run again.
- Look at captures before saying something looks right.
- Report every check with its evidence line. A check you could not run is a failure, with the reason.
