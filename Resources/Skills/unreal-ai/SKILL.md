---
name: unreal-ai
description: Read, check and plan Unreal AI - Behavior Trees, Blackboards and their keys, AI controllers and their perception senses, State Trees and navmesh coverage - with HyperAI's Gameplay AI tools, then build what they cannot change with Blueprint tools and prove the AI in play. Use for enemy, guard, companion or NPC behaviour, Blackboard keys, Behavior Tree tasks and services, State Tree reviews, AI perception and AI that does not move.
---

# AI: inspect, plan, build the rest, prove it in play

HyperAI's Gameplay AI tools are in toolset `HyperAIStudioGameplayAI.HyperAIStudioGameplayAIToolset`, called with
`{"request": {...}}`: `hyper_gameplay_ai_inspect`, `hyper_gameplay_ai_apply_plan` and `hyper_gameplay_ai_validate`. If
they are missing the gameplay_ai pack is off: say so. They read loaded assets only, so list the assets first with Epic's
`AssetTools.find_assets` and load each with `AssetTools.load_asset`; paths are object paths
(`/Game/AI/BB_Guard.BB_Guard`).

What they cover today:

| Asset | Inspect and validate | Change |
|---|---|---|
| Blackboard | keys (name, key type class, instance synced), parent | plan checks and stages key and parent edits; it does not apply them yet |
| Behavior Tree | every node with its kind, runtime class, error and breakpoint; runtime and editor-root blackboard | plan checks the blackboard link only; no tool edits the tree's nodes |
| AI controller Blueprint | default blackboard, perception senses (max age, starts enabled, dominant), compile status | none: build it with the unreal-blueprint skill |
| State Tree, Smart Objects, Gameplay Behaviors | prerequisite rows only | none; their ops are refused with `variant_adapter_unavailable` |

EQS has its own tools and skill when they are installed; this skill does not cover it.

## Inspect and validate

`hyper_gameplay_ai_inspect`: `assetPaths` (empty lists what is loaded), `variant` (`all`, `behavior_tree`, `blackboard`,
`ai_controller_perception`; `state_tree`, `eqs` and the others report prerequisites only), `bIncludeDetails`,
`bIncludePrerequisites`, `pageSize` up to 128. Follow `nextCursor` until `bTruncated` is false. Each record has a
`revision`: the `expectedRevision` its plan ops need. `variants` says per family what is installed, enabled and
implemented.

`hyper_gameplay_ai_validate`: the same `assetPaths` and `variant`, `bRequirePackagesClean` to fail unsaved assets. It
answers `valid` or `validation_failed` with issues such as `behavior_tree_blackboard_link_mismatch`,
`invalid_blackboard_reference`, `behavior_node_error`, `runtime_node_missing`, `duplicate_or_missing_blackboard_key`,
`blackboard_parent_unavailable_or_cyclic`, `dominant_sense_not_configured`, `perception_max_age_invalid`,
`duplicate_perception_sense` (warning), `ai_controller_compile_error`, `package_dirty` and `object_not_loaded`.

## Plan ops

`hyper_gameplay_ai_apply_plan`: up to 64 `operations`, each `{"type", "targetPath", "expectedRevision", ...}`.

| type | target | fields |
|---|---|---|
| `add_blackboard_key` | Blackboard | `name`, `keyType` bool, int, float, name, string, vector or rotator, `bInstanceSynced` |
| `remove_blackboard_key` | Blackboard | `name` of a local key |
| `rename_blackboard_key` | Blackboard | `name` (old), `newName` |
| `set_blackboard_parent` | Blackboard | `referencePath` a loaded Blackboard, or empty to clear |
| `set_behavior_tree_blackboard` | Behavior Tree | `referencePath` a loaded Blackboard, or empty to clear |

Leave every field an op does not use empty. Object, class and enum keys and inherited keys are not in the plan.

1. **Dry run**: `bDryRun: true`, no `operationId`. It must answer `valid_dry_run`; `effects` counts `keysAdded`,
   `keysRemoved`, `keysRenamed`. A wrong op is refused with its index and reason (`blackboard_key_exists`,
   `blackboard_key_missing`, `blackboard_rename_conflict`, `revision_precondition_failed`, `target_variant_mismatch`).
   Inspect again for a fresh revision after any change. A tree that has other errors fails a link change with
   `postcondition_invalid`: leave it out and report its errors.
2. **Send**: the same plan with `bDryRun: false`, a fresh `operationId` and `expectedPlanHash` = the dry run's
   `planHash`. Today it answers `staged_backend_required`: the plan is checked and staged, and nothing changed.
3. **Make the change**: a Blackboard's keys and parent with Epic's `ObjectTools.set_properties` on its `Keys` array
   (each `KeyType` a class path such as `/Script/AIModule.BlackboardKeyType_Vector`, every existing entry kept) or
   `Parent`. A tree's blackboard is set on its Root node, which no tool edits: give the user the list (tree, Blackboard)
   and wait. Inspect and validate again; save only if asked (`AssetTools.save_assets`).

Keys for a stealth guard, on a Blackboard inspected at revision `sha256:3f...`:

```json
{"request": {"bDryRun": true, "operations": [
  {"type": "add_blackboard_key", "targetPath": "/Game/AI/BB_Guard.BB_Guard", "expectedRevision": "sha256:3f...", "name": "AlertState", "keyType": "name"},
  {"type": "add_blackboard_key", "targetPath": "/Game/AI/BB_Guard.BB_Guard", "expectedRevision": "sha256:3f...", "name": "LastSeenLocation", "keyType": "vector"},
  {"type": "add_blackboard_key", "targetPath": "/Game/AI/BB_Guard.BB_Guard", "expectedRevision": "sha256:3f...", "name": "bAlerted", "keyType": "bool"},
  {"type": "add_blackboard_key", "targetPath": "/Game/AI/BB_Guard.BB_Guard", "expectedRevision": "sha256:3f...", "name": "SearchTime", "keyType": "float"}]}}
```

## Behavior Tree logic

Epic's `BehaviorTreeTools` (`aimodule_toolset.toolsets.behavior_tree.BehaviorTreeTools`) reads a tree: `list_nodes`
with `get_node_depths` (same order) for its outline, `get_root_decorators`, `get_children`, `get_blackboard` and
`get_subtree` for a Run Behavior task. A node's key selector
reads with `ObjectTools.get_properties` (`BlackboardKey`) and a renamed key is repointed with `set_properties`
(`SelectedKeyName`).

New tasks, services and decorators are Blueprints: write them with the unreal-blueprint skill on parents
`/Script/AIModule.BTTask_BlueprintBase` (Receive Execute AI, ending in Finish Execute on every path),
`/Script/AIModule.BTService_BlueprintBase` (Receive Tick AI) or `/Script/AIModule.BTDecorator_BlueprintBase`, with
instance-editable `BlackboardKeySelector` variables for the keys they use. Compile with
`hyper_blueprint_compile_validate`. Placing them in the tree is done in the Behavior Tree editor: say so and list where
each goes. The AI controller starts the tree with Run Behavior Tree on possess; the pawn uses the controller through
`AIControllerClass` and `AutoPossessAI`.

## State Trees

Epic's `StateTreeTools` (`state_tree_toolset.toolsets.state_tree.StateTreeTools`) reads one: `get_root_states`,
`get_children`, `get_tasks`, `get_enter_conditions`, `get_transitions`, `get_evaluators`, `get_global_tasks`,
`get_node_description`. Nothing edits states or transitions.
Write tasks and conditions as Blueprints (parents `/Script/StateTreeModule.StateTreeTaskBlueprintBase` and
`/Script/StateTreeModule.StateTreeConditionBlueprintBase`) and give the user the state layout to assemble in the State
Tree editor. When reviewing, check every leaf has a task and a way out and every transition targets a real state.

## Perception and navigation

Perception is read by inspect (`variant: "ai_controller_perception"`) and built with the unreal-blueprint skill: an
`AIPerception` component on the controller with its sense configs (sight radius, lose-sight radius, peripheral angle,
max age above 0, a dominant sense). Navigation is HyperAI's `hyper_navigation_inspect` and `hyper_navigation_validate`
(toolset `HyperAIStudioWorldbuilding.HyperAIStudioNavigationToolset`), `variants` `navmesh` and `nav_system`, projection
`identity`, `counts` and `bounds`: an AI only moves where a navmesh covers it.

## Proving it

Validate every asset you touched: 0 errors. Then play it (unreal-playtest skill): `hyper_verify_run` cases such as a
patrol (`WaitUntil` `AssertNear` the guard at its second point), a chase (`MoveTo` near the guard, then `WaitUntil`
`AssertNear` with `other` `Player`), and `Sample` to show it moved. For an AI that never moves, check in order:
possessed by the right controller, tree assets valid, a Run Behavior Tree call, navmesh under it, then
`hyper_log_tail` with categories `LogBehaviorTree`, `LogNavigation` and `LogAINavigation`.
