---
name: unreal-playtest
description: Play the game in Unreal and check that it works, with HyperAI's playtest bot. It plays like a person through the game's own controls, moving round obstacles, jumping, collecting, aiming the way the game reads aim (its cursor, right stick or view) and attacking, dodging, exploring, and fighting other players in PvP, with up to 8 networked players. It also presses input actions, clicks UI, waits for things to happen, checks actors, gameplay attributes, tags, abilities, effects and widgets, surveys a whole level for what a player can reach, and captures screenshots and video, then reports evidence per step. hyper_verify_run plays many such cases at once in under a minute, and hyper_phone_playtest_start streams play to phones on the local network. Use it to verify gameplay, such as a pickup adding score, a door opening, an enemy dying when shot, or a HUD updating.
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

### Record it as proof

Set `"bRecordVideo": true` (and `videoFps`, 10 unless set, 5 to 30) to record the run as an MP4 a person can watch.
- **What it shows:** the game view with its UI, and each step with its result drawn over it.
- **Where it goes:** `Saved/HyperAIStudio/Playtests/<operationId>/playtest.mp4`. A `timeline.json` beside it gives each step's start and end in milliseconds, with its status and detail.
- **How it reports:** as the run's last evidence item, kind `video`, with the file in `objectPath`.
- **When to use it:** whenever someone will review gameplay you built. Link the video in your reply.

### Multiplayer: host, clients and late joiners

- **Start a networked session.** Set `netMode` to `ListenServer` or `Client`, with `numPlayers` 2 to 8. Every player plays in this editor process, in a window of its own, and the run starts once all of them are in.
  - Players and `JoinPlayer` steps together are 8 at most.
  - Past 4 players, each needs about 0.5 GB of free memory, and 1.5 GB stay free for the editor; a dedicated server counts as one more player. A machine short of it is refused before anything starts, with `not_enough_memory` and how many players it can play. Ask for that many.
  - Each player keeps a bot of its own: steps with that player's `pieInstance` drive it.
  - With `ListenServer`, PIE instance 0 hosts and plays, and 1 and up are its clients.
  - With `Client`, a dedicated server is instance 0, and the players come after it.
- **Pick a player per step.** A step's `pieInstance` (-1 for the run's own world) picks the player it acts as. A check on a client, such as `AssertProperty` or `UiAssert` at `pieInstance: 1`, proves what replicated to it.
- **Late join.** `{"stepId": "join", "kind": "JoinPlayer", "timeoutMs": 20000}` adds a client the way a late joiner arrives. Its evidence names the new instance, which later steps can use.

```json
{"request": {"operationId": "door-replicates-01", "mode": "StartInProcess", "teardown": "StopOwnedSession",
  "netMode": "ListenServer", "numPlayers": 2, "bRecordVideo": true,
  "steps": [
    {"stepId": "open", "kind": "Bot", "timeoutMs": 20000, "bot": {"action": "MoveTo", "target": "BP_Door", "radius": 150, "timeoutSeconds": 15}},
    {"stepId": "client-sees-it", "kind": "Bot", "pieInstance": 1, "timeoutMs": 5000,
     "bot": {"action": "WaitUntil", "until": "AssertProperty", "target": "BP_Door", "property": "bOpen", "compare": "==", "value": "true", "timeoutSeconds": 3}},
    {"stepId": "join", "kind": "JoinPlayer", "timeoutMs": 20000},
    {"stepId": "joiner-sees-it", "kind": "Bot", "pieInstance": 2, "timeoutMs": 5000,
     "bot": {"action": "AssertProperty", "target": "BP_Door", "property": "bOpen", "compare": "==", "value": "true"}}]}}
```

## Check actors, GAS and widgets

These are Assert actions, so `WaitUntil` waits for any of them, and they work in both tools (UiAssert only in `hyper_playtest_run`).

| action | fields | passes when |
|---|---|---|
| AssertCount | target (a class, tag, label or name), compare, value | That many actors match, counting all of them. |
| AssertDistance | target, other or location, compare, value (cm) | The distance compares as asked, such as `<` 300. |
| AssertAttribute | target, property (`Health`, or `HealthSet.Health`; `Health.Base` for the base value), compare, value | The gameplay attribute compares as asked. A miss lists the attributes there are. |
| AssertTag | target, property (the tag), value `present` (unless set) or `absent` | The target's gameplay tags have it, or not. A parent tag matches its children. |
| AssertAbility | target, property (the ability's class, `GA_Dash`), value `granted` (unless set), `active`, `inactive` or `absent` | The ability is in that state. Reports every granted ability with its level. |
| AssertEffect | target, property (the effect's class, `GE_Burning`), compare, value (stacks, `>= 1` unless set) | The active effect's stack count compares as asked. Inhibited effects are counted apart. |
| UiAssert | widget, property, compare, value | See below. |

- **GAS stays optional.** The bot finds the ability system the way the engine does (the AbilitySystemInterface, then a component), on the actor or, for a player, its player state, and reads it by reflection. A game without GAS fails these with `no_ability_system`, not an error.
- **Tags** come from the ability system and from an actor that has gameplay tags of its own.
- **On a client** (`pieInstance` 1 and up), these read what replicated, so they prove replication too.

`UiAssert` finds `widget` by name or by class, after an optional user widget: `WBP_HUD.ScoreText`, `WBP_HUD.ProgressBar`, or `WBP_Pause` alone. `property` says what to check:

| property | passes when |
|---|---|
| Visible (unless set) | The widget, every widget it sits in, and its user widget are visible and on the screen. With value, its text compares too. |
| Text | Its text (a text block, rich text or text box) compares with value: `==`, `contains`... |
| Enabled / Disabled | It is enabled, or disabled. |
| Focused | It or a widget inside it has focus. |
| Hidden | It is not on the screen. |
| Percent | A progress bar's fill, 0 to 1, compares with value. |

```json
{"stepId": "burning", "kind": "Bot", "timeoutMs": 5000, "bot": {"action": "WaitUntil", "until": "AssertEffect", "target": "BP_Enemy", "property": "GE_Burning", "compare": ">=", "value": "2", "timeoutSeconds": 3}},
{"stepId": "mana", "kind": "Bot", "timeoutMs": 5000, "bot": {"action": "AssertAttribute", "target": "Player", "property": "Mana", "compare": "<", "value": "100"}},
{"stepId": "bar", "kind": "Bot", "timeoutMs": 5000, "bot": {"action": "UiAssert", "widget": "WBP_HUD.HealthBar", "property": "Percent", "compare": "<", "value": "1"}}
```

## Survey a level

`SurveyLevel` walks and films the whole level the player is in, in one step. Run it as a `hyper_verify_run` case (`timeoutSeconds` 50) or a `hyper_playtest_run` Bot step (`timeoutMs` 30000).

- **The grid:** `value` cells a side (24 unless set, 4 to 48) over the level, or over `range` cm round the player. Each cell is reachable, unreachable (ground the player cannot get to), or has no ground at all. With a navmesh it asks the navmesh for a path to each cell; without one it follows the ground from cell to cell, over rises a player can climb and round walls.
- **Frames:** up to 16 views at eye height over the reachable ground, then frames of the walks.
- **Walks:** short walks (`seconds` of walking, 12 unless set) from spread-out reachable spots, every other one toward an edge with nothing below. A walk that makes no progress for a second is a stuck spot; a fall with nothing below, past KillZ, or a pawn the game destroyed is a fall out of the world.
- **What it writes,** into a folder named after `text`: `overview.png` (the level from above: reachable green, unreachable red, no ground dark, walked blue, the start white, stuck spots yellow, falls magenta), `coverage.json` (counts, the reachable share, a character map of the grid, stuck spots and falls with locations), the frames, and `survey.mp4` made from them. The step's `file` is the folder.
- **It always passes** when it ran; read the counts, stuck spots and falls, and open the overview. The player is put back where it started.

```json
{"name": "level-survey", "timeoutSeconds": 50, "steps": [
  {"action": "Wait", "seconds": 0.5},
  {"action": "SurveyLevel", "value": "24", "seconds": 12, "timeoutSeconds": 45, "text": "survey"}]}
```

## Playtest on phones

`hyper_phone_playtest_start` streams the game with Epic's Pixel Streaming 2 to phones and browsers on this PC's network, and returns a URL and `qrCodePng`, a QR code to scan. Studio's Playtest page shows both, with Start and Stop.

- **Needs** the Pixel Streaming 2 plugin enabled in the project (then restart the editor) and its player page on disk. HyperAI never downloads it; a refusal says so.
- **It always asks the user** (an external effect): it starts a server in this editor, listening for phones. Run it with `bDryRun: true` first to show the user the URL, ports and plan, then for real, then `hyper_operation_wait` with `bWaitThroughApproval: true`, then `hyper_phone_playtest_status`.
- **Local network only.** The engine's server listens on every network interface, so HyperAI refuses on a PC with a public internet address, and takes websockets only from pages served on its LAN address or this PC. Say so to the user, with `exposure`. A router forwarding the ports would expose it; nothing else does.
- **target `PlayInEditor`** (default) streams every PIE session started while it runs: the user presses Play, or you run `hyper_playtest_run` with the bot and `bRecordVideo`, so a bot or a person plays while it records. Phones see the first player's view (a client's, in a listen-server session).
- **target `Standalone`** launches the saved level as its own game process (`map`, `resX`, `resY`, `bOffscreen`).
- **Viewers:** `maxViewers` 1 to 8 (8 unless set). With `bAllViewersControl` (default) every viewer's touches, keys and mouse reach the game's first player; false gives control to the first viewer only. A browser gamepad becomes a player of its own when the game adds local players.
- **Ports:** `httpPort` 8080 (the page) and `streamerPort` 8888, both 1024 to 65535 and free. Windows may ask to let UnrealEditor accept connections: private networks only.
- `hyper_phone_playtest_stop` ends it, closing a Standalone game.

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
- **Fighting:** it reacts, turns and aims like a person, with an aim that settles, and attacks when the crosshair is on the target. It strafes, hops and dodges meanwhile. In a game that aims by its cursor or its right stick it aims those too, and attacks once the game's own aim is on the target (see Aim the way the game aims).

| goal | fields | passes when |
|---|---|---|
| Reach | target or location, radius (100 cm), timeoutSeconds | The player got within radius. Fails `stuck`, saying where. |
| Collect | target: a tag or class naming all of them, timeoutSeconds | Every one was picked up. Presses the interact action when walking onto one is not enough. Fails `not_collected` with the ones that stayed. |
| Defeat | target, skill, range, healthProperty, inputAction, aim (Auto unless set), timeoutSeconds | Every target's health reached 0 or it is gone. See the failures below. |
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

## Aim the way the game aims

A game reads its player's aim from one of three places. Aim through the one it reads, or attacks go somewhere else.

| the game | aim | what the bot does |
|---|---|---|
| Top-down, isometric, twin-stick on the mouse, click to attack | `Cursor` | Puts the game window's own cursor on the target's screen point, and moves it a few pixels round it each frame with the matching mouse motion, so checks such as "the cursor moved lately" keep passing. The user's real cursor never moves. |
| A gamepad game, or a player on a gamepad | `Stick` | Pushes the right stick toward the target, relative to the camera. |
| First-person, third-person over the shoulder | `Look` | Turns the view (the control rotation) to the target, as LookAt does. |
| Not sure | `Auto`, the default | Picks one from what the game shows, and says which and why in the evidence. |

Two ways to aim:
- **`AimAt`:** aims at `target` or `location` for `seconds` (0.5 unless set), then checks.
- **`aim` on Press, Hold or Interact:** the attack is aimed before it fires and for as long as it is held. Press and Hold aim at `target` or `location`.

```json
{"stepId": "slash", "kind": "Bot", "timeoutMs": 5000, "bot": {"action": "Press", "inputAction": "IA_Attack", "target": "BP_Enemy", "aim": "Auto"}},
{"stepId": "channel", "kind": "Bot", "timeoutMs": 8000, "bot": {"action": "Hold", "inputAction": "IA_Beam", "target": "BP_Enemy", "aim": "Cursor", "seconds": 2, "lead": 0.2}}
```

How it checks:
- **The game's own aim:** when the pawn or one of its components has GetAimWorldLocation, GetAimDirection or GetAimDirectionFlat, the bot reads them after aiming. A direction passes within `tolerance` degrees (10 unless set). A cursor's aim point passes within `radius` cm (100).
- **The evidence:** how it aimed and why, the cursor's point in the window (such as `(812, 455) of 1280x720`), and each reading of the game's aim.
- **A failure:** `aim_off_target` means the game aims somewhere else than the bot aimed. Try another aim. `aim_not_applied` means the aim never took hold, such as a target always behind the camera; the evidence says why. `viewport_unavailable` means a cursor aim ran where there is no window.

More to know:
- **`lead`** aims ahead of a moving target by that many seconds of its travel, 0 to 2.
- **A cursor rests.** It stays where the last aimed step left it until the run ends, as a hand rests on a mouse. A resting cursor stops counting as moving after a moment, as a still mouse does, so aim the attack itself rather than AimAt and then an unaimed Press.
- **The video** draws each aim when `bRecordVideo` is on: a ring on the point, and a line from the player.
- **Where the cursor works:** only in `hyper_playtest_run`, which has a window. Verification copies have none, so there Auto aims by the view and a pinned `Cursor` is refused.
- **Players:** in a networked session each client has its own window and cursor, so each client's steps aim by its own. A second local player shares the first one's window and cursor, so it aims by stick or look.
- **CommonInput:** the bot's stick does not switch CommonInput to Gamepad, because CommonUI then hides and moves the real cursor. A game that picks its aim by CommonInput's device keeps reading the mouse, so aim it by cursor. Auto picks the stick only when CommonInput already reports a gamepad.

## Bot actions

| action | fields | passes when |
|---|---|---|
| Play | goal, and the goal's fields above | The goal was met. Writes a map of the play. |
| AddPlayer | location (optional) | A local player joined with a pawn. |
| Survey | target (optional) | Always passes. Reports the player and the camera, an actor census, the controls with their keys, the player's health, and the other pawns near it. With a target, also its distance and whether the navmesh reaches it. |
| MoveTo | target or location, radius (100 cm), timeoutSeconds (10) | The player is within radius. Follows the navmesh when there is one. Fails `stuck` after 3 s without progress. |
| LookAt | target or location | The view faces it. |
| AimAt | target or location, aim, lead, tolerance, seconds (0.5) | Aimed the way the game reads aim, and the game's own aim points there. Fails `aim_off_target`. |
| Jump | timeoutSeconds | The character left the ground and landed. Characters only. |
| Press | inputAction (`IA_Jump`, or an asset path), value; aim with target or location | One press was injected through Enhanced Input, so the game's own bindings run. With aim, it fires once the aim is on the target. |
| Hold | inputAction, seconds, value; aim with target or location | The action was held for that long. `value: "0,1"` is forward for a 2D move action. With aim, it stays aimed all through. |
| Interact | target, inputAction (optional), aim (optional) | The player faced the target and pressed its interact action. Without one, the action named like Interact or Use. With aim, it aims at the target the game's way instead. |
| Wait | seconds | That much time passed. |
| WaitUntil | until (one of the Assert actions), its fields, timeoutSeconds | The assertion became true. |
| AssertProperty | target, property, compare (`==` `!=` `<` `<=` `>` `>=` `contains`), value | The comparison holds. Numbers compare as numbers. |
| AssertNear | target, other or location, radius | The target is within radius. |
| AssertOnScreen | target | The target is in the camera's view and not hidden. |
| AssertExists / AssertGone | target | The target is there, or destroyed. |
| Sample | target, property (optional), seconds | Always passes. Reports first, last, min and max. Without a property, speed and distance moved. |
| Capture | text | A PNG was written. Without a screen, as in verification copies, it renders the player's camera. Its path is in the evidence: open it and look. |
| RecordStart / RecordStop | text, seconds (interval, 0.5) | A frame was saved every interval into a folder, until RecordStop. |
| UiClick / UiType / UiAssert | widget (`WBP_HUD.ScoreText`), text, property, compare, value | A button clicked through Slate, text typed, or a widget checked as Check actors, GAS and widgets says. |
| Damage | target (geometry collections) or location, radius (cm reached), value (strain; twice the highest damage threshold unless set), seconds (keep it on) | A strain field hit Chaos geometry collections there. |
| Fire | target or location, value (cm/s, 3000), range (drop from this high instead) | A 40 cm, 200 kg test ball was fired from the player's view, or dropped. |
| AssertBroken | target (empty: every geometry collection), compare, value | That many pieces broke off. The detail counts the clusters broken and names what fell how far. |
| AssertCount, AssertDistance, AssertAttribute, AssertTag, AssertAbility, AssertEffect | see Check actors, GAS and widgets | |
| SurveyLevel | value (cells a side), range, seconds, text | Always passes when it ran; see Survey a level. |

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
- **An attack's direction:** Press the attack with `aim` and the enemy as target. The evidence says how it aimed and where the game's own aim pointed. For a top-down or isometric game, also try `aim: Look`: if that one fails `aim_off_target` and Cursor passes, the game aims by its cursor.
- **Combat:** Play Defeat on the enemies' tag or class. Its evidence says whether shots landed and did damage. `no_damage` means hits did nothing, which is a bug in the damage path. Then check what a defeat should do, such as score or loot.
- **Can it be finished:** Play Reach to the goal, or Play Collect on every pickup. A `stuck` result says where the level blocks a player.
- **Level coverage:** SurveyLevel for the whole level's reachable ground, stuck spots and falls, with an overview and a video. Play Explore for 30 to 60 seconds shows how a wandering player moves through it.
- **An ability:** Press its input, then WaitUntil AssertAbility `active`, AssertEffect on the effect it applies, and AssertAttribute on what it costs.
- **Before building anything:** Survey tells you the class names, where the player starts, and whether a target is reachable.
- **Destruction:** Wait 0.5 s, Damage the geometry collection (or Fire at it), WaitUntil AssertBroken with compare `>=`, then Wait and AssertBroken again to read what fell. `hyper_destruction_apply_plan` hands back such a case as `VerifyCase`; the `unreal-destruction` skill has more.

## Rules

- Several checks at once: use `hyper_verify_run`. A single playtest that needs the screen, UI or captures: use `hyper_playtest_run`.
- Only one playtest can play in the editor at a time. A run started while another is playing is refused with `session_already_active`: wait for the first, then run again.
- Look at captures before saying something looks right.
- Report every check with its evidence line. A check you could not run is a failure, with the reason.
