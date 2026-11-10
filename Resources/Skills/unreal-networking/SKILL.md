---
name: unreal-networking
description: Make Unreal multiplayer work - actor replication, replicated and RepNotify variables, server, client and multicast RPCs, relevancy, dormancy and update rates, and the game framework (GameMode, GameState, PlayerController, PlayerState, HUD, GameSession, project and level defaults) - with HyperAI's Networking tools, the Blueprint structure tool, and listen-server play tests with several players. Use for any replication, RPC, net role, game mode setup or multiplayer bug.
---

# Networking and the game framework

HyperAI's Networking tools are in toolset `HyperAIStudioNetworking.HyperAIStudioNetworkingToolset`, called with
`{"request": {...}}`: `hyper_network_inspect`, `hyper_network_apply_plan`, `hyper_network_validate`,
`hyper_game_framework_inspect`, `hyper_game_framework_apply_plan` and `hyper_game_framework_validate`. If they are missing
the networking_game_framework pack is off: say so.

They read loaded objects and Asset Registry metadata without loading anything; a record with `bLoaded` false is metadata
only, so load it (`AssetTools.load_asset`) and inspect again. The apply plans check and stage edits but do not apply
them yet: the change itself is made with the Blueprint structure tool and Epic's tools, below.

## Who runs what

- The server has authority. GameMode and GameSession exist only on the server; GameState and every PlayerState replicate
  to everyone; a PlayerController exists on the server and its owning client only.
- An actor replicates only with `bReplicates`. Its replicated variables and components need that first.
- A server RPC reaches the server only from an actor the calling client owns (its pawn, controller or something they
  own); otherwise it is dropped. Multicast runs on the server and every client that has the actor. Reliable is for
  events that must arrive; never for cosmetic, frequent multicasts.
- RepNotify runs its `OnRep_` function on clients when the value arrives; owner_only sends a value to its owner alone.
- Dormant actors stop replicating until woken: code that changes their state must flush dormancy first.

## Inspect and validate

`hyper_network_inspect`: `targetPaths` (Blueprint asset or generated class paths, or loaded `/Script` classes), `scope`
`all`, `class_defaults`, `blueprint_declarations`, `world_instances`, `net_drivers` or `on_disk_assets`, `projection`
`details` or `summary`; follow `nextCursor`. A `class_default` record gives bReplicates, bAlwaysRelevant,
bOnlyRelevantToOwner, bUseOwnerRelevancy, Dormancy, NetUpdateFrequency, MinNetUpdateFrequency, NetPriority and its
`revision`; `elements` list replicated variables (Condition, RepNotifyFunction), components and RPCs (RpcMode,
bReliable). In play, `world_instances` gives each actor's LocalRole, RemoteRole, bHasAuthority and OwnerPath per world,
and `net_drivers` its NetMode, ClientConnectionCount and bHasServerConnection.

`hyper_game_framework_inspect`: `targetPaths` such as `project:game_maps_settings` (GameDefaultMap, TransitionMap,
global game mode and game instance), a level's world path (its game mode override), or a game mode or session class;
`scope` `all`, `project_settings`, `class_defaults`, `world_settings`, `world_runtime`, `session`, `online` or
`on_disk_assets`. Records name each role's class path and the session's MaxPlayers and MaxSpectators.

The validators take the same `targetPaths` and `scope`. Network issues: `dormancy_without_replication`,
`replicated_component_without_actor_replication`, `missing_rep_notify_function`, `rpc_direction_missing`,
`rpc_validation_direction_invalid`, `invalid_property_replication_condition`, `invalid_update_policy`,
`contradictory_relevancy`, `blueprint_compile_error`. Framework issues: `missing_global_game_mode`,
`game_mode_missing_player_state` (and `_default_pawn`, `_game_state`, `_player_controller`), `missing_game_instance_class`,
`game_default_map_not_configured`, `invalid_game_session_limits`, `project_default_role_mismatch`.

## Plan ops

Both plans: up to 64 `operations`; dry run (`bDryRun: true`) must answer `valid_dry_run`; sending it again with a fresh
`operationId` and `expectedPlanHash` = its `planHash` answers `staged_backend_required` (nothing changed). An op on an
existing asset carries `expectedRevision` from its record; `framework.create_class` carries none. Unused numbers stay -1
and unused strings empty. Plans reference loaded classes only.

| `hyper_network_apply_plan` type | fields |
|---|---|
| `class.set_replication` | `replicates` 0 or 1 |
| `class.set_relevancy` | `alwaysRelevant`, `onlyRelevantToOwner`, `useOwnerRelevancy` (0 or 1; not always and owner-only both) |
| `class.set_update_policy` | `netUpdateFrequency` above 0, `minNetUpdateFrequency` at most that, `netPriority` above 0 |
| `class.set_dormancy` | `dormancy` never, awake, dormant_all, dormant_partial or initial |
| `component.set_replicated` | `memberName`, `replicates` |
| `property.set_replication` | `memberName`, `replicates`, `replicationCondition` (none, initial_only, owner_only, skip_owner, simulated_only, autonomous_only ...), `repNotifyFunction` (an existing function) |
| `rpc.configure_existing` | `memberName`, `rpcMode` server, client or multicast, `reliable`, `withValidation` (server only) |

| `hyper_game_framework_apply_plan` type | fields |
|---|---|
| `framework.create_class` | `targetPath` new, `role` game_mode, game_state, player_controller, player_state, pawn, spectator_pawn, hud, game_session or game_instance, `parentClassPath` |
| `project.set_defaults` | `targetPath` `project:game_maps_settings`, `gameModeClassPath`, `gameInstanceClassPath` |
| `world.set_game_mode_override` | the world path, `gameModeClassPath` |
| `game_mode.set_class_defaults` | `gameStateClassPath`, `playerControllerClassPath`, `playerStateClassPath`, `pawnClassPath`, `hudClassPath`, `gameSessionClassPath`, `spectatorClassPath` |
| `game_session.set_limits` | `maxPlayers`, `maxSpectators`, `maxSplitscreensPerConnection`, `requiresPushToTalk` |

## Making the change

- Variables, RPCs, components: `hyper_blueprint_structure_apply_plan` (toolset
  `HyperAIStudioBlueprintStructure.HyperAIStudioBlueprintStructureToolset`, see the unreal-blueprint skill):
  `variable.set_replication` (`replication` none, replicated or rep_notify, which makes `OnRep_<Name>`; `condition`),
  `event.add` or `event.set_rpc` (`rpc` none, server, client or multicast, `bReliable`), `component.set_replicated`. On
  `awaiting_user_approval`, `hyper_operation_wait` with `bWaitThroughApproval: true`.
- Class defaults: Epic's `ObjectTools.set_properties` on the Blueprint's default object: `bReplicates`,
  `bAlwaysRelevant`, `bOnlyRelevantToOwner`, `bNetUseOwnerRelevancy`, `NetUpdateFrequency`, `MinNetUpdateFrequency`,
  `NetPriority`, `NetDormancy` (`DORM_DormantAll`); a game mode's `GameStateClass`, `PlayerControllerClass`,
  `PlayerStateClass`, `DefaultPawnClass`, `HUDClass`; a level's World Settings `DefaultGameMode`. New classes:
  `BlueprintTools.create`. Compile each with `hyper_blueprint_compile_validate`, which saves it.
- Project Maps and Modes (GlobalDefaultGameMode, GameDefaultMap): the unreal-data skill's config settings.

Replicating a door, then the fallback:

```json
{"request": {"bDryRun": true, "operations": [
  {"type": "class.set_replication", "targetPath": "/Game/Props/BP_Door.BP_Door", "expectedRevision": "sha256:a1...", "replicates": 1},
  {"type": "property.set_replication", "targetPath": "/Game/Props/BP_Door.BP_Door", "expectedRevision": "sha256:a1...",
   "memberName": "bOpen", "replicates": 1, "replicationCondition": "none"}]}}
```

On `staged_backend_required`: `ObjectTools.set_properties` `bReplicates` true on its default object, then one
`hyper_blueprint_structure_apply_plan` with `blueprintPath` `/Game/Props/BP_Door` and `{"kind":
"variable.set_replication", "name": "bOpen", "replication": "rep_notify"}`. Inspect and validate again.

## Testing with several players

`hyper_playtest_run` (unreal-playtest skill) with `mode` `StartInProcess`, `netMode` `ListenServer` (instance 0 hosts) or
`Client` (dedicated server), `numPlayers` 2-4. A step's `pieInstance` picks the player: act on 0, then check on 1 with
`WaitUntil` `AssertProperty` to prove it replicated; `JoinPlayer` adds a late joiner. With `teardown`
`LeaveSessionRunning`, read the live session with `hyper_network_inspect` (`net_drivers`, `world_instances`) and run
`hyper_network_validate` with `bRequirePIEMultiplayerEvidence: true` and `hyper_game_framework_validate` (`scope`
`world_runtime`, `bRequirePIEWorldEvidence: true`); then stop with an `AttachOnly` run of one `StopSession` step.
