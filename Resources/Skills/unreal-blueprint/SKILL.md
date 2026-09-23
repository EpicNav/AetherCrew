---
name: unreal-blueprint
description: Build or change a gameplay Blueprint through the Unreal MCP tools in two calls - class, components, variables, defaults and event logic in one script. Use for any Blueprint work - pickups, doors, spawners, health, projectiles, triggers, timers.
---

# Build a Blueprint in two calls

Work through Epic's `editor_toolset` tools, batched in one `execute_tool_script` call. Every call to the model costs far
more time than the editor spends, so plan the whole Blueprint first and send it in one script.

## Call 1, once per chat

`call_tool` with toolset `editor_toolset.toolsets.programmatic.ProgrammaticToolset`, tool `get_execution_environment`,
arguments `{}`. It is required before `execute_tool_script`. Read its `instructions` field once.

## Call 2: one script that does everything

`call_tool` with toolset `editor_toolset.toolsets.programmatic.ProgrammaticToolset`, tool `execute_tool_script`,
arguments `{"script": "<python>"}`. The script defines `run()` and returns a dict. Copy this shape; it is known to work:

```python
import json
B = 'editor_toolset.toolsets.blueprint.BlueprintTools.'
A = 'editor_toolset.toolsets.actor.ActorTools.'
O = 'editor_toolset.toolsets.object.ObjectTools.'
S = 'editor_toolset.toolsets.asset.AssetTools.'

def t(name, args):
    return execute_tool(name, json.dumps(args))['returnValue']

LOGIC = '''
(event Collision|EventActorBeginOverlap (OtherActor)
  (bind pawn (Utilities|Casting|CastToPawn :Object OtherActor)
    (:then
      (if (Pawn|IsPlayerControlled :self pawn)
        (Development|PrintString (Utilities|String|ToString(Integer) (Variables|Default|GetScoreValue)))
        (Actor|DestroyActor)))
    (:CastFailed)))
'''

def run():
    bp = t(B + 'create', {'folder_path': '/Game/Gameplay', 'asset_name': 'BP_Coin', 'asset_type': '/Script/Engine.Actor'})
    mesh = t(A + 'add_component', {'owner': bp, 'component_type': '/Script/Engine.StaticMeshComponent', 'name': 'Mesh'})
    trigger = t(A + 'add_component', {'owner': bp, 'component_type': '/Script/Engine.SphereComponent', 'name': 'Trigger'})
    spin = t(A + 'add_component', {'owner': bp, 'component_type': '/Script/Engine.RotatingMovementComponent', 'name': 'Spin'})
    t(O + 'set_properties', {'instance': mesh, 'values': json.dumps({'StaticMesh': '/Engine/BasicShapes/Cylinder.Cylinder'})})
    t(O + 'set_properties', {'instance': trigger, 'values': json.dumps({'SphereRadius': 80.0})})
    t(O + 'set_properties', {'instance': spin, 'values': json.dumps({'RotationRate': {'Pitch': 0, 'Yaw': 90, 'Roll': 0}})})
    t(B + 'add_variable', {'blueprint': bp, 'name': 'ScoreValue', 'type_name': 'int'})
    graph = {'refPath': bp['refPath'] + ':EventGraph'}
    t(B + 'write_graph_dsl', {'graph': graph, 'code': LOGIC})          # also compiles
    cdo = t(B + 'get_default_object', {'blueprint': bp})
    t(O + 'set_properties', {'instance': cdo, 'values': json.dumps({'ScoreValue': 10})})
    saved = t(S + 'save_assets', {'asset_paths': ['/Game/Gameplay/BP_Coin']})
    return {'saved': saved, 'graph': t(B + 'read_graph_dsl', {'graph': graph})}
```

The returned `graph` is the Event Graph read back in the same DSL: check it matches what you meant before you say you
are done. A compile error raises inside the script and comes back as the tool's error; fix the DSL and run the script
again (use a new asset name, or reuse the existing Blueprint instead of `create`).

## Facts that save calls

- Objects travel as `{"refPath": "..."}` dicts. Pass what a tool returned straight into the next call. A Blueprint's
  graphs are `<blueprint refPath>:EventGraph`, `:UserConstructionScript`, or `:<FunctionName>`.
- Classes are paths: `/Script/Engine.Actor`, `/Script/Engine.Character`, `/Script/Engine.StaticMeshComponent`,
  `/Script/Engine.SphereComponent`, `/Script/Engine.BoxComponent`, `/Script/Engine.PointLightComponent`,
  `/Script/Engine.RotatingMovementComponent`, `/Script/Engine.ProjectileMovementComponent`, `/Script/Engine.AudioComponent`.
  A Blueprint parent is `/Game/Path/BP_Base.BP_Base_C`.
- `add_variable` types: `bool`, `int`, `float`, `byte`, `string`, `name`, `text`, `Vector`, `Rotator`, `Transform`,
  `Vector2D`, `LinearColor`. Set defaults after the graph compiles, on `get_default_object`.
- A new Actor Blueprint's Event Graph already has `(event EventBeginPlay)`,
  `(event Collision|EventActorBeginOverlap (OtherActor))` and `(event EventTick (DeltaSeconds))`. Writing one of them
  fills that event.
- Parameters with an empty default are still required by Epic's schema: pass `context_pins: []` to `find_node_types`,
  and every argument a tool lists.
- Never guess a node type ID. If the pattern below does not cover a node, look up every node you need in one script:
  `t(B + 'find_node_types', {'graph': graph, 'type_id_filter': 'DestroyActor', 'context_pins': []})` returns IDs such as
  `Actor|DestroyActor`. Put all the lookups in one script with a dict comprehension, then write the Blueprint in the next.
- Grammar reference, only when needed: `get_graph_dsl_docs` on `editor_toolset.toolsets.blueprint.BlueprintTools`.

## Node type IDs confirmed in UE 5.8

| Need | Type ID |
|---|---|
| Print a message | `Development|PrintString` |
| Destroy this actor | `Actor|DestroyActor` |
| Cast to Pawn (continuations `:then`, `:CastFailed`) | `Utilities|Casting|CastToPawn` |
| Is the pawn the player's | `Pawn|IsPlayerControlled` |
| Integer to text | `Utilities|String|ToString(Integer)` |
| Read or write member variable `X` | `Variables|Default|GetX`, `Variables|Default|SetX` |
| Rotate by a delta / set a rotation | `Transformation|AddActorLocalRotation`, `Transformation|SetActorRotation` |
| Build a rotator | `Math|Rotator|MakeRotator` |
| This actor's transform | `Transformation|GetActorTransform` |
| Spawn an actor | `Game|SpawnActorfromClass` |
| Call a function on a timer | `Utilities|Time|SetTimerbyFunctionName` |
| Clamp a float | `Math|Float|Clamp(Float)` |
| Deal damage / the GameMode | `Game|Damage|ApplyDamage`, `Game|GetGameMode` |
| Events named like their "AddEvent" type | `(event Game|Damage|EventAnyDamage ...)` for `AddEvent|Game|Damage|EventAnyDamage` |

There is no Timeline node in the DSL: animate with `EventTick`, or with a looping timer.

## DSL in one screen

```
(event EventName stmt ...)                 ; an event; (event Name (Param ...) ...) names the outputs you use
(fn FunctionName (A B) stmt ... (return x)) ; only inside that function's own graph (see below)
(CallFunction|FunctionName args...)        ; call this Blueprint's own function from another graph
(bind name (Node|Id args...))              ; bind a result once, then reuse the name
(Node|Id positional :PinName value ...)    ; call a node
(if cond stmt ... (else stmt ...))          (for i (range 5) stmt ...)
(+ a b) (== a b) (and a b) (not x) (select cond a b)
self                                        ; this actor
"quoted"                                    ; class paths, enum values and asset paths must be quoted
```

Bind a node's result before using it twice; every inline call makes another node that runs again.

Each `write_graph_dsl` fills one graph. For a function, first
`die = t(B + 'add_function_graph', {'blueprint': bp, 'graph_name': 'Die'})`, then write `(fn Die () ...)` into `die`,
and only then write the Event Graph that calls `(CallFunction|Die)`.

## Patterns

- **Pickup:** the script above. Add score to the player with a variable on the pawn class and
  `Variables|Default|SetScore` on the cast pawn, or through the GameMode.
- **Spawner:** a `Spawn` function graph holding
  `(fn Spawn () (Game|SpawnActorfromClass :Class "/Game/Path/BP_Thing.BP_Thing_C" :SpawnTransform (Transformation|GetActorTransform)))`,
  and `(event EventBeginPlay (Utilities|Time|SetTimerbyFunctionName :Object self :FunctionName "Spawn" :Time 2.0 :bLooping true))`.
- **Health** (built and compiled this way):
  ```
  ; graph Die, from add_function_graph
  (fn Die ()
    (Development|PrintString "dead")
    (Actor|DestroyActor))
  ; Event Graph
  (event Game|Damage|EventAnyDamage (Damage)
    (Variables|Default|SetHealth (Math|Float|Clamp(Float) (- (Variables|Default|GetHealth) Damage) 0.0 100.0))
    (if (<= (Variables|Default|GetHealth) 0.0)
      (CallFunction|Die)))
  ```
  Other actors hurt it with `Game|Damage|ApplyDamage`.
- **Door** (built and compiled this way, with `bool` IsOpening and `float` OpenYaw):
  ```
  (event Collision|EventActorBeginOverlap (OtherActor)
    (Variables|Default|SetIsOpening true))
  (event EventTick (DeltaSeconds)
    (if (and (Variables|Default|GetIsOpening) (< (Variables|Default|GetOpenYaw) 90.0))
      (bind step (* DeltaSeconds 90.0))
      (Transformation|AddActorLocalRotation :DeltaRotation (Math|Rotator|MakeRotator :Yaw step))
      (Variables|Default|SetOpenYaw (+ (Variables|Default|GetOpenYaw) step))
      (else
        (Variables|Default|SetIsOpening false))))
  ```

After the Blueprint exists, place it with `editor_toolset.toolsets.scene.SceneTools` or a HyperAI level plan, and prove
it works with a playtest when the task asks for runtime behaviour.
