---
name: unreal-input
description: Set up and change Enhanced Input - Input Actions, Input Mapping Contexts, key and gamepad mappings, triggers (hold, tap, double tap, pulse, charge) and modifiers (dead zones, sensitivity, inversion, swizzle, smoothing) - with HyperAI's Enhanced Input tools, bind them in Blueprint or C++, and prove the controls in play. Use for controls, keybinds, gamepad support, aim and look tuning, hold-to-interact, auto fire, and input that does nothing.
---

# Enhanced Input: inspect, plan, bind, press it in play

HyperAI's Enhanced Input tools are in toolset `HyperAIStudioEnhancedInput.HyperAIStudioEnhancedInputToolset`, called
with `{"request": {...}}`: `hyper_input_inspect`, `hyper_input_apply_plan` and `hyper_input_validate`. If they are
missing the enhanced_input pack is off: say so. They read loaded assets only: list with Epic's `AssetTools.find_assets`,
load with `AssetTools.load_asset`, and use object paths (`/Game/Input/IA_Jump.IA_Jump`). An unloaded path answers
`asset_not_loaded`.

## Inspect and validate

`hyper_input_inspect`: `assetPaths` (empty lists what is loaded), `bIncludeMappings`, `bIncludeProfiles`,
`bIncludeComponents`, `bIncludeReferenceEvidence`, `bIncludeRuntimeBindings`, `pageSize` up to 128; follow `nextCursor`.
Records are `action` (ValueType, bTriggerWhenPaused, bConsumeInput, AccumulationBehavior, triggers, modifiers,
ReferenceCount = mappings that name it), `mapping_context`, `mapping` (ActionPath, Key, MappingIndex,
MappingFingerprint, triggers, modifiers), `profile_mapping` and, during play, `runtime_binding` (the contexts registered
on a player and their Priority).

**Revisions.** A plan op's `expectedRevision` is the `revision` from inspecting that one asset alone with
`bIncludeRuntimeBindings: false`. Inspect again after every change.

`hyper_input_validate` (`assetPaths`) reports `conflicting_key`, `duplicate_mapping`, `orphan_loaded_action`,
`empty_mapping_context`, `value_type_dimension_loss` (a 2D stick on a boolean action), `invalid_key`, `missing_action`,
`modifier_value_type_incompatible` and `unsupported_action_trigger`, with `errorCount`.

## Plan ops

`hyper_input_apply_plan` takes up to 128 `operations`, each `{"type", "targetPath", "expectedRevision", ...}`.

| type | fields |
|---|---|
| `create_action` | `valueType` boolean, axis1d, axis2d or axis3d; no `expectedRevision` |
| `create_context` | no `expectedRevision` |
| `set_action_config` | any of `valueType`; `bSetTriggerWhenPaused` + `bTriggerWhenPaused`; `bSetConsumeInput` + `bConsumeInput`; `bSetReserveAllMappings` + `bReserveAllMappings`; `bSetAccumulationBehavior` + `accumulationBehavior` highest_absolute or cumulative |
| `add_action_trigger`, `add_action_modifier` | one entry in `triggers` or `modifiers` |
| `add_mapping` | `actionPath`, `key` (`SpaceBar`, `W`, `Gamepad_LeftX`, `Gamepad_Left2D`, `Mouse2D`, `Gamepad_FaceButton_Bottom`), optional `triggers` and `modifiers` |
| `append_mapping_trigger`, `append_mapping_modifier` | `mappingIndex`, `expectedMappingFingerprint`, one entry |
| `replace_mapping_key` (destructive) | `mappingIndex`, `expectedMappingFingerprint`, `key` |
| `remove_mapping`, `delete_asset` (destructive) | the mapping's selector, or just the asset |
| `set_runtime_priority` (runtime) | `runtimeOwnerPath` a loaded Enhanced Player Input, `priority` |

Triggers and modifiers are `{"kind", ...}` with every unused number at -1 and every unused flag false. Up to 16 each.

| kind | fields |
|---|---|
| `trigger.down`, `trigger.pressed`, `trigger.released` | `actuationThreshold` 0-1 (every trigger needs it) |
| `trigger.hold` | `durationSeconds` 0-120, `bHasOption: true` + `bOption` (one shot), `mode` real_time or dilated_time |
| `trigger.hold_and_release` | `durationSeconds` 0-120, `mode` |
| `trigger.tap` | `durationSeconds` 0-10, `mode` |
| `trigger.repeated_tap` | `durationSeconds` 0-10, `secondarySeconds` 0-10 (the window), `count` 2-16, `mode` |
| `trigger.pulse` | `durationSeconds` 0.001-60 (interval), `count` 0-10000 (0: no limit), `bHasOption` + `bOption` (fire on press), `mode` |
| `modifier.dead_zone` | `lowerThreshold` < `upperThreshold` (0-1), `mode` axial, radial or unscaled_radial |
| `modifier.scalar`, `modifier.response_exponential` | `bHasVector: true`, `vector` `{"X", "Y", "Z"}` |
| `modifier.negate` | `bHasAxes: true`, `bX`, `bY`, `bZ` |
| `modifier.swizzle` | `mode` yxz, zyx, xzy, yzx or zxy |
| `modifier.smooth_delta` | `mode` (lerp, interp_to, ease_in_out ...), `speed` 0-1000, `exponent` 0-100 |
| `modifier.fov_scaling` | `mode` standard or ue4_backcompat, `scale` |
| `modifier.smooth`, `modifier.scale_by_delta_time`, `modifier.to_world_space` | none |

Rules: edit, destructive and runtime ops go in separate plans (`mixed_safety_cohort`). Only `add_mapping` may target a
context created in the same plan. A plan changes each mapping once; a second change needs a new inspect.
`profileId` stays empty: profile overrides are read-only in UE 5.8.

## The loop

1. Dry run (`bDryRun: true`): it must answer `valid_dry_run`; `effects` counts what would change.
2. Send it with `bDryRun: false`, a fresh `operationId` and `expectedPlanHash` = the dry run's `planHash`.
   - A plan of only `set_action_config` and `add_mapping` without triggers or modifiers answers
     `applied_fast_transaction`: one undo step, assets left unsaved. Save with `AssetTools.save_assets` when asked.
   - Anything else answers `staged_backend_required`: checked, nothing changed. Make exactly that plan with one
     `hyper_script_run` (editor Python, toolset `HyperAIStudioScripting.HyperAIStudioScriptingToolset`), or report the
     checked plan if the Scripting pack is off. Strict Safety mode stages every plan the same way.
   - Destructive and runtime plans dry-run to `valid_requires_trusted_authorization`. Never send them: list what they
     would do and let the user approve it.
3. Inspect the assets again and validate: `errorCount` 0.

Gamepad bindings beside the keyboard ones, on a context and action inspected alone:

```json
{"request": {"bDryRun": true, "operations": [
  {"type": "add_mapping", "targetPath": "/Game/Input/IMC_Default.IMC_Default", "expectedRevision": "sha256:9c...",
   "actionPath": "/Game/Input/IA_Jump.IA_Jump", "key": "Gamepad_FaceButton_Bottom"},
  {"type": "add_mapping", "targetPath": "/Game/Input/IMC_Default.IMC_Default", "expectedRevision": "sha256:9c...",
   "actionPath": "/Game/Input/IA_Move.IA_Move", "key": "Gamepad_Left2D"},
  {"type": "set_action_config", "targetPath": "/Game/Input/IA_Pause.IA_Pause", "expectedRevision": "sha256:41...",
   "bSetTriggerWhenPaused": true, "bTriggerWhenPaused": true}]}}
```

A hold-to-interact trigger, for an `add_action_trigger` (staged, then built with a script):
`{"kind": "trigger.hold", "actuationThreshold": 0.5, "durationSeconds": 0.8, "bHasOption": true, "bOption": true,
"mode": "dilated_time"}`. WASD on a 2D move action (modifiers run in order): W swizzle `yxz`; S swizzle `yxz` then
negate `bY`; A negate `bX`; D nothing.

## Binding it

- **Blueprint** (unreal-blueprint skill): on BeginPlay get the Enhanced Input Local Player Subsystem from the player
  controller and Add Mapping Context with the context and a priority; then one Enhanced Input Action event per action.
  Find the exact nodes with `find_node_types`. Logic for hold, pulse and charge triggers listens on Triggered, not
  Started. After a `valueType` change, recompile every Blueprint that binds the action (`AssetTools.get_referencers`,
  then `hyper_blueprint_compile_validate`).
- **C++**: in `SetupPlayerInputComponent`, cast to `UEnhancedInputComponent` and
  `BindAction(MoveAction, ETriggerEvent::Triggered, this, &AMyCharacter::Move)`; add the context through
  `UEnhancedInputLocalPlayerSubsystem::AddMappingContext(Context, Priority)`. A higher priority context whose action has
  bConsumeInput true hides the same key from lower ones.

## Proving it

Play it with the unreal-playtest skill: `hyper_playtest_run` (mode `StartInProcess`) Bot steps `Survey` (the controls
the player has), `Press` or `Hold` with `inputAction` (`IA_Jump` or a path) and `value` (`1`, or `0,1` for a 2D axis),
then `Sample` or `AssertProperty` the result; `hyper_operation_wait`, then `hyper_playtest_status`. For dead input, keep
the session running (`teardown: "LeaveSessionRunning"`), inspect with `bIncludeRuntimeBindings: true` for the
registered contexts and priorities, and stop it with an `AttachOnly` run of one `StopSession` step.
