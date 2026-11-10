---
name: unreal-gas
description: Build Gameplay Ability System content - Gameplay Effects (damage, costs, cooldowns, buffs, staggers) and Gameplay Abilities (attacks, dodges, heals) with their tags, costs and policies - in one undoable plan with HyperAI's GAS tools, then the Blueprint logic that grants and triggers them. Use for any GAS work in a project that has the GameplayAbilities plugin.
---

# Effects and abilities in one plan

HyperAI's GAS tools are in toolset `HyperAIStudioGAS.HyperAIStudioGASToolset`, called with `{"request": {...}}`.
`hyper_gas_apply_plan` makes and edits Gameplay Effect and Gameplay Ability Blueprints in one call: up to 64 ops, each
checked before anything changes, applied as one undo step, compiled and saved. `Made` reads every asset back.

## Before the plan

- **Tags must exist.** Add them with Epic's Gameplay Tags tool: `AddTag` on `GameplayTagsToolset` with `tagName`,
  `comment` and an empty `tagSource`, once per tag (`Ability.Attack.Heavy`, `State.Staggered`, `Data.Damage`,
  `GameplayCue.Hit.Heavy`). A plan naming an unknown tag is refused with its name. Cue tags start with `GameplayCue.`.
- **Attributes live in C++.** An Attribute Set is a native class with `FGameplayAttributeData` properties; a plan names
  one as `attributeOwnerClassPath` (`/Script/MyGame.MyAttributeSet`) and `attributeName` (`Stamina`). Epic's
  `AttributeSetToolset.FindAttributeSetClasses` and `ListAttributes` show what the project has. A project without one
  needs a programmer to add it first.

## Ops

Each op is `{"type", "targetPath", ...}`; `targetPath` is the asset (`/Game/GAS/GE_HeavyHit.GE_HeavyHit`).

| type | fields |
|---|---|
| `create_gameplay_effect_blueprint`, `create_gameplay_ability_blueprint` | `parentClassPath` optional (another effect or ability, or a native subclass) |
| `set_effect_duration` | `durationPolicy` instant, infinite or duration; `durationSeconds`; `periodSeconds` (0 for none); `bSetExecutePeriodicEffectOnApplication` with `bExecutePeriodicEffectOnApplication` |
| `replace_effect_modifiers` | `modifiers`: `{attributeOwnerClassPath, attributeName, operation (additive, multiplicative, division, override), magnitude}`, or `setByCallerTag` instead of `magnitude` |
| `replace_effect_granted_tags` | `tags` the target has while the effect lasts (`State.Staggered`) |
| `replace_effect_asset_tags`, `replace_effect_blocked_ability_tags` | `tags` the effect carries; abilities with these tags can't start while it lasts |
| `replace_effect_cues` | `cues`: `{cueTags, minLevel, maxLevel}` |
| `set_effect_cue_flags` | `bSetRequireModifierSuccessToTriggerCues`/`bRequireModifierSuccessToTriggerCues`, `bSetSuppressStackingCues`/`bSuppressStackingCues` |
| `set_ability_asset_tags` | `tags` naming the ability (`Ability.Attack.Heavy`) |
| `set_ability_tags` | `property`: activation_owned, activation_required, activation_blocked, cancel_abilities_with, block_abilities_with, source_required, source_blocked, target_required, target_blocked; `tags` |
| `set_ability_property` | `property` instancing_policy (`per_actor`, `per_execution`), net_execution_policy (`local_predicted`, `local_only`, `server_initiated`, `server_only`), cost_effect or cooldown_effect (`value`: the effect's path, made earlier in the plan or not) |

`bDryRun: true` checks a plan without changing anything. `expectedRevision` from `hyper_gas_inspect` is optional: with
it, the plan is refused if the asset changed since.

## A heavy attack

A stamina cost, a hit that staggers for 0.6 s with damage the caller sets, and an attack that pays the cost and can't
start while staggered or already attacking:

```json
[{"type": "create_gameplay_effect_blueprint", "targetPath": "/Game/GAS/GE_StaminaHeavy.GE_StaminaHeavy"},
 {"type": "set_effect_duration", "targetPath": "/Game/GAS/GE_StaminaHeavy.GE_StaminaHeavy", "durationPolicy": "instant"},
 {"type": "replace_effect_modifiers", "targetPath": "/Game/GAS/GE_StaminaHeavy.GE_StaminaHeavy", "modifiers": [
   {"attributeOwnerClassPath": "/Script/Aetherfall.AFAttributeSet", "attributeName": "Stamina", "operation": "additive", "magnitude": -25}]},
 {"type": "create_gameplay_effect_blueprint", "targetPath": "/Game/GAS/GE_HeavyHit.GE_HeavyHit"},
 {"type": "set_effect_duration", "targetPath": "/Game/GAS/GE_HeavyHit.GE_HeavyHit", "durationPolicy": "duration", "durationSeconds": 0.6},
 {"type": "replace_effect_modifiers", "targetPath": "/Game/GAS/GE_HeavyHit.GE_HeavyHit", "modifiers": [
   {"attributeOwnerClassPath": "/Script/Aetherfall.AFAttributeSet", "attributeName": "IncomingDamage", "operation": "additive", "setByCallerTag": "Data.Damage"}]},
 {"type": "replace_effect_granted_tags", "targetPath": "/Game/GAS/GE_HeavyHit.GE_HeavyHit", "tags": ["State.Staggered"]},
 {"type": "replace_effect_cues", "targetPath": "/Game/GAS/GE_HeavyHit.GE_HeavyHit", "cues": [{"cueTags": ["GameplayCue.Hit.Heavy"], "maxLevel": 1}]},
 {"type": "create_gameplay_ability_blueprint", "targetPath": "/Game/GAS/GA_HeavyAttack.GA_HeavyAttack"},
 {"type": "set_ability_asset_tags", "targetPath": "/Game/GAS/GA_HeavyAttack.GA_HeavyAttack", "tags": ["Ability.Attack.Heavy"]},
 {"type": "set_ability_tags", "targetPath": "/Game/GAS/GA_HeavyAttack.GA_HeavyAttack", "property": "activation_owned", "tags": ["State.Attacking"]},
 {"type": "set_ability_tags", "targetPath": "/Game/GAS/GA_HeavyAttack.GA_HeavyAttack", "property": "activation_blocked", "tags": ["State.Staggered", "State.Attacking"]},
 {"type": "set_ability_property", "targetPath": "/Game/GAS/GA_HeavyAttack.GA_HeavyAttack", "property": "cost_effect", "value": "/Game/GAS/GE_StaminaHeavy.GE_StaminaHeavy"},
 {"type": "set_ability_property", "targetPath": "/Game/GAS/GA_HeavyAttack.GA_HeavyAttack", "property": "net_execution_policy", "value": "local_predicted"}]
```

## Abilities through Blueprint

HyperAI's plan makes the ability's class and its defaults; what it does when it runs is Blueprint logic in the
ability's own `EventGraph`, written with the unreal-blueprint skill (`find_node_types` lists every node named here).

- **The ability's graph.** `Event ActivateAbility` (or `Event ActivateAbilityFromEvent` when a gameplay event starts
  it, with the event's `EventData`) runs when it starts. Call `CommitAbility` first: it pays the cost effect and starts
  the cooldown, and returns false when it cannot (then `EndAbility` and stop). Do the work with ability tasks, latent
  nodes that end with the ability: `PlayMontageAndWait` (its On Completed, On Blend Out, On Interrupted and On
  Cancelled pins), `WaitGameplayEvent` (an anim notify's event tag, such as the hit frame), `WaitDelay`,
  `WaitInputRelease` (charge attacks), `WaitTargetData`. Apply what it does with `MakeOutgoingGameplayEffectSpec`,
  `AssignTagSetByCallerMagnitude` and `ApplyGameplayEffectSpecToTarget` (or `ApplyGameplayEffectToOwner` for buffs);
  `GetAvatarActorFromActorInfo` is the character, `GetAbilitySystemComponentFromActorInfo` its component. Every path
  ends in `EndAbility`, or the ability stays active and blocks the next one. `Event OnEndAbility` cleans up.
- **Granting.** On the character (or its Player State, wherever the Ability System Component lives), call `Give
  Ability` with the ability class, on the server (`BlueprintAuthorityOnly`), usually from `BeginPlay` or when the
  ability is unlocked; `Give Ability And Activate Once` for one-shot abilities.
- **Triggering from Blueprint.** From input (an Enhanced Input action event), call `TryActivateAbilitiesByTag` with
  the ability's asset tag (`Ability.Attack.Heavy`) or `TryActivateAbilityByClass`; both return false when tags,
  cost or cooldown refuse it. To start an ability from an event instead (a hit reaction, a parry window), give the
  ability an Ability Trigger (in its class defaults: trigger tag `Event.Hit`, source Gameplay Event) and send the event
  with `SendGameplayEventToActor` (Ability System Blueprint Library) and a payload; the ability runs `ActivateAbilityFromEvent`.
- **Cancelling.** Abilities whose `cancel_abilities_with` tags match are cancelled when this one starts; from
  Blueprint, `CancelAbilities` on the component with tags, or `CancelAbility` inside the ability.

Compile the ability and the character after the graph changes; Epic's `AbilitySystemInspectorToolset` then shows the
ability granted and active in Play In Editor.

## Checking

`hyper_gas_inspect` with `assetPaths` reads each asset back: duration, modifiers and their magnitude kind, tags, cues,
policies, cost and cooldown. Epic's `AbilitySystemInspectorToolset` reads a running actor's attributes, active effects,
granted abilities and tags in Play In Editor: the proof that the attack costs stamina and the hit staggers.
