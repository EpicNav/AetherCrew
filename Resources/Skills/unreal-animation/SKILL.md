---
name: unreal-animation
description: Animate characters with HyperAI's animation tools - montages with sections, slots, notifies and hit windows; sequences with curves, sync markers and root motion; composites, blend spaces and skeleton slots; IK Rigs and IK Retargeters that copy animation from one character to another; and Pose Search schemas and databases for motion matching. Use for animation assets, retargeting and motion matching data in an Unreal project.
---

# Animation, retargeting and motion matching

Four tools, each for its own assets:

| assets | read | edit |
|---|---|---|
| montages, sequences, composites, blend spaces, skeleton slots | `hyper_animation_inspect` | `hyper_animation_apply_plan`: one asset per plan |
| Animation Blueprints: state machines, montage slots, upper-body layers, inertialization | `hyper_anim_blueprint_inspect` | `hyper_anim_blueprint_apply_plan` |
| IK Rigs, IK Retargeters, retargeted animations | `hyper_retarget_inspect` | `hyper_retarget_apply_plan` (IK Rig plugin, on by default) |
| Pose Search schemas and databases | `hyper_pose_search_inspect` | `hyper_pose_search_apply_plan` (Pose Search plugin) |

Control Rig hierarchies and graphs are Epic's `AnimationAssistantToolset`. Every plan is checked before anything
changes, applied as one undo step and saved; `bDryRun: true` only checks it. With approval on, a plan waits in the
Activity panel: call `hyper_operation_wait` with its `OperationId` and `bWaitThroughApproval: true`.

## Montages and sequences

`hyper_animation_apply_plan` takes `Operations`, each `{Type, TargetPath, ...}` on one asset, with `bDryRun: false` and
an `OperationId`, then `hyper_operation_wait`. A combo attack from two clips, with a hit window, a swing sound and a
branching point where the next attack may start:

```json
[{"Type": "montage.create", "TargetPath": "/Game/Combat/AM_Combo.AM_Combo", "ReferencePath": "/Game/Anims/A_Slash1.A_Slash1", "Name": "UpperBody"},
 {"Type": "montage.set_segments", "TargetPath": "/Game/Combat/AM_Combo.AM_Combo", "Segments": [
   {"SequencePath": "/Game/Anims/A_Slash1.A_Slash1"}, {"SequencePath": "/Game/Anims/A_Slash2.A_Slash2", "PlayRate": 1.1}]},
 {"Type": "montage.add_section", "TargetPath": "/Game/Combat/AM_Combo.AM_Combo", "Name": "Slash2", "Minimum": 0.9},
 {"Type": "montage.set_property", "TargetPath": "/Game/Combat/AM_Combo.AM_Combo", "Name": "blend_in", "Value": "0.1"},
 {"Type": "montage.add_notify", "TargetPath": "/Game/Combat/AM_Combo.AM_Combo", "Notify": {"Kind": "notify_state",
   "NotifyClass": "/Game/Combat/ANS_HitWindow.ANS_HitWindow_C", "TimeSeconds": 0.35, "DurationSeconds": 0.2, "Track": "Combat"}},
 {"Type": "montage.add_notify", "TargetPath": "/Game/Combat/AM_Combo.AM_Combo", "Notify": {"Kind": "notify",
   "NotifyClass": "/Script/Engine.AnimNotify_PlaySound", "TimeSeconds": 0.3, "Track": "Audio",
   "Properties": [{"Source": "Sound", "Target": "/Game/SFX/S_Swing.S_Swing"}]}},
 {"Type": "montage.add_notify", "TargetPath": "/Game/Combat/AM_Combo.AM_Combo", "Notify": {"Kind": "named_event",
   "Name": "ComboWindow", "TimeSeconds": 0.7, "bBranchingPoint": true}}]
```

Sequences take notifies, `sequence.add_curve` with `Name` and `Keys` (`[{TimeSeconds, Value}]`), sync markers (`Name`,
`Minimum` as the time) and `sequence.set_property` (`root_motion`, `root_lock`, `loop`, `rate_scale`). A montage's slot
must be on its skeleton: `skeleton.add_slot` adds one. `hyper_animation_validate` checks segments, sections and notifies.

A montage only plays where its slot is in the character's Animation Blueprint. After the locomotion state machine,
one `hyper_anim_blueprint_apply_plan` adds `add_layered_slot` (`Name` UpperBody, `Value` spine_01: attacks on the upper
body while the legs keep running), `add_slot` (`Name` DefaultSlot: full-body dodges and finishers), then
`add_inertialization`, and `set_transition_blend` for transitions that should blend by inertialization.
`.hyperai/docs/HyperAIStudio-AnimationCookbook.md` has the whole recipe.

## Retargeting a character

`hyper_retarget_apply_plan` takes `Operations`, each `{Kind, Asset, ...}`. The usual plan makes an IK Rig for each
character, a retargeter between them, and copies the animations across:

```json
[{"Kind": "ik_rig.create", "Asset": "/Game/Retarget/IK_Manny", "Mesh": "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"},
 {"Kind": "ik_rig.create", "Asset": "/Game/Retarget/IK_Wraith", "Mesh": "/Game/Characters/Wraith/SK_Wraith"},
 {"Kind": "retargeter.create", "Asset": "/Game/Retarget/RTG_Manny_To_Wraith", "SourceRig": "/Game/Retarget/IK_Manny", "TargetRig": "/Game/Retarget/IK_Wraith"},
 {"Kind": "retarget.animations", "Asset": "/Game/Retarget/RTG_Manny_To_Wraith", "OutputFolder": "/Game/Characters/Wraith/Anims",
  "Prefix": "Wraith_", "Animations": ["/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle", "/Game/Characters/Mannequins/Anims/Unarmed/BS_Idle_Walk_Run"]}]
```

- `ik_rig.create` names the chains and root from Epic's templates (UE5 and UE4 mannequins, MetaHumans, Mixamo and
  more); the report's `Notes` say which template matched. For a skeleton no template knows, the chains are guessed from
  bone names: read them with `hyper_retarget_inspect` and fix them with `ik_rig.add_chain`, `ik_rig.set_chain` (`Chain`,
  `StartBone`, `EndBone`, `Goal`), `ik_rig.remove_chain` and `ik_rig.set_root` (`Bone`, the pelvis).
- `retargeter.create` sets the retargeter up as Epic's auto-retarget does: default ops, chains mapped by exact name,
  the target's retarget pose aligned with the feet kept flat, root motion and pinned IK bones configured, and the IK pass
  off. Target chains without a same-named source chain are listed in `Notes`: map them with `retargeter.map_chain`
  (`TargetChain`, `SourceChain`) or `retargeter.auto_map` with `Mode: "fuzzy"`.
- `retarget.animations` copies sequences, blend spaces and montages (and the clips a montage or blend space uses); the
  new assets are in `Retargeted`. `bOverwrite: true` replaces assets that already have the new names.
- Check the result: `hyper_retarget_inspect` on the retargeter lists `ChainMap` (every chain should read `Target <-
  Source`) and its ops; play a retargeted clip on the target mesh. Feet that slide or hands that miss their targets want
  the IK pass: `ik_rig.auto_full_body_ik` on the target rig, then `retargeter.enable_op` with `OpName:
  "FIKRetargetRunIKRigOp"`.

## Motion matching data

`hyper_pose_search_apply_plan` takes `Operations`, each `{Kind, Asset, ...}`; `Settings` are `[{Name, Value}]` pairs with
the engine's property names in Unreal's text form, as `hyper_pose_search_inspect` lists them. A locomotion database:

```json
[{"Kind": "schema.create", "Asset": "/Game/MotionMatching/PSS_Locomotion", "Skeleton": "/Game/Characters/Mannequins/Meshes/SK_Mannequin",
  "Settings": [{"Name": "SampleRate", "Value": "30"}]},
 {"Kind": "database.create", "Asset": "/Game/MotionMatching/PSD_Locomotion", "Schema": "/Game/MotionMatching/PSS_Locomotion",
  "Settings": [{"Name": "Tags", "Value": "(\"Locomotion\")"}]},
 {"Kind": "database.add_animations", "Asset": "/Game/MotionMatching/PSD_Locomotion",
  "Animations": ["/Game/Anims/MM_Idle", "/Game/Anims/MM_Walk_Fwd", "/Game/Anims/MM_Run_Fwd"],
  "Settings": [{"Name": "MirrorOption", "Value": "UnmirroredOnly"}]},
 {"Kind": "database.build", "Asset": "/Game/MotionMatching/PSD_Locomotion"}]
```

- `schema.create` adds Epic's default channels unless `bDefaultChannels` is false: the trajectory, and both feet's
  positions and velocities (`foot_l`, `foot_r`). A skeleton with other foot names shows a note: change the Pose
  channel's `SampledBones` with `schema.set_channel` (`ChannelIndex` as inspect lists them).
- `schema.add_channel` with `Channel` (Position, Velocity, Heading, Phase, Curve, Trajectory...) and its `Settings`
  (`Bone` as `(BoneName="hand_r")`, `Weight`); `schema.remove_channel` by index.
- `database.set_animations` changes the listed animations' entries (`SamplingRange` as `(Min=0,Max=1.5)`, `bEnabled`,
  `bDisableReselection`, `MirrorOption` UnmirroredOnly, MirroredOnly or UnmirroredAndMirrored, which needs a
  `MirrorTable` on the schema); `database.remove_animations` drops them. `database.set` changes the database's own
  settings: `PoseSearchMode` (BruteForce or PCAKDTree), `ContinuingPoseCostBias`, `LoopingCostBias`, `Tags`.
- `database.build` indexes the database after the edits are saved; `Built` reports its pose count. A build that fails
  names no single cause: inspect the schema's `Notes` (missing bones) and the database's (animations of another skeleton).

The database is searched by a Motion Matching node in an Animation Blueprint, fed by a Pose History node; build that
graph with `hyper_anim_blueprint_apply_plan`.
