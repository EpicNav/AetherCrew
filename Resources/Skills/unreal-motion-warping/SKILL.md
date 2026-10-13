---
name: unreal-motion-warping
description: Make montages bend their root motion to a target the game picks at run time with Unreal's Motion Warping - add, retime and tune warp windows (skew warp, scale) on montages and sequences, put the Motion Warping component on character Blueprints, and set the warp targets from Blueprint or C++ for vaults, mantles, attack lunges and finishers.
---

# Motion Warping

| work | tool |
|---|---|
| the op kinds, the window modifiers with their settings and defaults, the montages with warp windows; a montage's or sequence's windows; a character Blueprint's component | `hyper_motion_warping_inspect` |
| add, change and remove warp windows, or add and set the character's component, one undo step | `hyper_motion_warping_apply_plan` |
| make the montage itself, its segments, sections and other notifies; turn root motion on in a sequence | `hyper_animation_apply_plan` (read with `hyper_animation_inspect`) |

Toolset `HyperAIStudioMotionWarping.HyperAIStudioMotionWarpingToolset`, called with `{"request": {...}}`. The pack
needs Epic's Motion Warping plugin (Animation category); without it its tools are not there.

How it works: a montage holds **warp windows**, Motion Warping notify states from a start to an end time. Each has a
**root motion modifier** and, for the warping ones, a **warp target name**. The character has a **Motion Warping
component**. While the montage plays through a window, the component bends the animation's root motion so the
character arrives at the target with that name when the window ends. Game code gives the target its place just before
the montage plays. Three things must line up: the window's `WarpTargetName`, the name the game code sets, and root
motion on in the montage's sequences.

## A plan

```json
{"request": {"Asset": "/Game/Characters/Hero/AM_Vault",
 "Operations": [
  {"Kind": "window.add", "WarpTargetName": "VaultStart", "StartSeconds": 0.0, "EndSeconds": 0.35,
   "Params": "bIgnoreZAxis=false"},
  {"Kind": "window.add", "WarpTargetName": "VaultLand", "StartSeconds": 0.6, "EndSeconds": 0.9,
   "Params": "bIgnoreZAxis=false; bWarpRotation=false", "Track": "Warp"}]}}
```

and the character:

```json
{"request": {"Asset": "/Game/Characters/Hero/BP_Hero",
 "Operations": [{"Kind": "component.add", "Params": "bSearchForWindowsInAnimsWithinMontages=false"}]}}
```

One plan changes one asset: a montage or sequence takes window ops, a character Blueprint (one whose parent is
Character or a child of it) takes component ops. `Params` are `"Name=Value; Name=Value"`: enums by short name
(`RotationType=Facing`), bools `true`/`false`, vectors `"x,y,z"`, rotators `"pitch,yaw,roll"`, assets by path.
`hyper_motion_warping_inspect` with no Asset lists every setting with its type and default.

1. `bDryRun: true` checks the plan and replies with `Notes` (what each op would do) and the asset as it is now.
2. The same plan with `bDryRun: false` changes it, unsaved unless `bSave: true`. With approval on it waits in the
   Activity panel: call `hyper_operation_wait` with `bWaitThroughApproval: true`.
3. Check the reply: `Windows` (index, track, start and end seconds, warp target, modifier, its settings), `WarpTargets`
   (the names the game must set), `Warnings`. Or read it again with `hyper_motion_warping_inspect`.

One Edit > Undo takes back the whole plan. A component plan compiles the Blueprint after its undo step, and undo or
redo compiles it again.

## Op kinds

- `window.add`: a window from `StartSeconds` to `EndSeconds` (inside the asset's length) warping to `WarpTargetName`,
  with `Modifier` and its `Params`, on `Track` (made when missing; the first track when empty). The modifiers:
  - `skew_warp`, the default: bends translation (skewed toward the target) and rotation. Right for nearly everything.
    Settings worth knowing: `bWarpTranslation`, `bIgnoreZAxis` (true keeps the animation's own height: set false for
    vaults and mantles that must reach a ledge's height), `bWarpRotation`, `RotationType` (`Default` matches the
    target's rotation, `Facing` turns to face the target's location), `RotationMethod` (`Slerp`,
    `SlerpWithClampedRate`, `ConstantRate`, `Scale`), `WarpRotationTimeMultiplier` (0.5 reaches the rotation in half
    the window), `WarpMaxRotationRate` (degrees per second for the clamped and constant methods),
    `AdditionalRotationOffset`, `bSubtractRemainingRootMotion`, `WarpPointAnimProvider` with `WarpPointAnimBoneName`
    or `WarpPointAnimTransform`, `MaxSpeedClampRatio`.
  - `scale`: multiplies the translation by `Scale` (`"1,1,0"` drops vertical motion); it takes no warp target.
  - `adjustment_blend`: experimental (Epic hides it from the notify's list); can move IK bones too.
  - `precomputed_warp`: experimental; plans the whole path on the window's first frame, so only for targets that do
    not move.
- `window.set`: picks a window by `Index` (as inspect lists them, by start time, before the plan) or by
  `WarpTargetName`; changes `StartSeconds` and/or `EndSeconds` (-1 keeps), `Modifier` (a new one; the warp target and
  the settings both share carry over) and `Params`. `Params "WarpTargetName=NewName"` renames its target.
- `window.remove`: picks a window the same way and removes it.
- `component.add`: puts a Motion Warping component (`ComponentName`, `MotionWarping` when empty) in the character
  Blueprint's construction script with its `Params`, and compiles. Refused when the character already has one, its
  own or inherited.
- `component.set`: changes the Blueprint's own component's settings, chiefly `bSearchForWindowsInAnimsWithinMontages`
  (also look for windows on the sequences inside a montage, not just on the montage). An inherited component is
  changed in the parent that adds it.

## Setting warp targets at run time

The windows only say when to warp and toward which name; the game says where. On the character's
`UMotionWarpingComponent`, from Blueprint (the component's Motion Warping functions) or C++:

- `AddOrUpdateWarpTargetFromLocation(WarpTargetName, TargetLocation)`: a place; rotation zero.
- `AddOrUpdateWarpTargetFromLocationAndRotation(WarpTargetName, TargetLocation, TargetRotation)`: a place and a facing.
- `AddOrUpdateWarpTargetFromTransform(WarpTargetName, TargetTransform)`.
- `AddOrUpdateWarpTargetFromComponent(WarpTargetName, Component, BoneName, bFollowComponent, LocationOffsetDirection,
  LocationOffset, RotationOffset)`: a component (and optionally a bone or socket on it); `bFollowComponent` true
  keeps tracking it while the window plays, for a target that moves.
- `AddOrUpdateWarpTarget(FMotionWarpingTarget)` with a target struct made yourself.
- `RemoveWarpTarget(WarpTargetName)`, `RemoveWarpTargets(Names)` and `RemoveAllWarpTargets()` clear them; each
  returns how many it removed.

The name must match the window's `WarpTargetName` exactly; a window whose target was never set does not warp. Set the
target before the montage reaches the window, usually on the line right before Play Montage (Play Anim Montage on a
Character, or `UAnimInstance::Montage_Play`). A target stays until it is replaced or removed, so a stale one from the
last vault is used if the new one is not set: update it every time.

**Vault or mantle.** Trace forward and down to find the obstacle's near edge and its top. Then
`AddOrUpdateWarpTargetFromLocationAndRotation("VaultStart", EdgeTop, WallFacing)` and
`AddOrUpdateWarpTargetFromLocation("VaultLand", LandingPoint)`, and play `AM_Vault`, whose windows warp to `VaultStart`
(hands to the edge, `bIgnoreZAxis=false` so the height is reached) and `VaultLand` (feet to the floor beyond). The
character movement must allow it: Flying or no collision with the wall while the montage plays, as the template's
traversal does.

**Attack lunge.** Before playing the attack, pick the enemy in reach and call
`AddOrUpdateWarpTargetFromComponent("Lunge", Enemy->GetRootComponent(), NAME_None, true, ...)` with a
`LocationOffset` of the attack's reach so the character stops in front of the enemy rather than inside it; the
attack's window uses `skew_warp` with `RotationType=Facing`, so the swing turns toward the target. No enemy in reach:
`RemoveWarpTarget("Lunge")` and the attack plays as animated.

## Checking the result

- `Warnings` name what would stop it working: a montage whose sequences have root motion off (warping bends root
  motion, so nothing moves: turn it on with `hyper_animation_apply_plan`'s `sequence.set_property` `root_motion`), a
  window outside the asset's length, a warp window with no target name, a target name no window uses any more after
  a plan (code that sets it warps nothing), a character with no component, an experimental modifier.
- Windows on a sequence only count when it plays inside a montage and the component's
  `bSearchForWindowsInAnimsWithinMontages` is on; putting them on the montage is simpler.
- `hyper_animation_inspect` shows the same windows among the montage's other notifies, and `hyper_animation_apply_plan`
  makes the montage first when there is none (`montage.create` from the sequence).
- Prove it in play with the playtest bot: play the montage with the target set and check where the character ends.

## Limits

32 ops per plan, one asset per plan, `/Game` content only, refused during Play In Editor. Inspect lists at most 200
montages with warp windows (found from the asset registry, so only saved or known ones).
