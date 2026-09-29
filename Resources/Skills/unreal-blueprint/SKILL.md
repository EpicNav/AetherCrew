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

HyperAI Studio lays out the nodes you add when the Blueprint compiles (`write_graph_dsl` compiles): flow left to right, branches fanned out, nothing on anything, and the nodes already there left where they are. You do not place nodes or call `arrange_nodes` after a write. To tidy a graph that was already a mess, pass all its nodes to `BlueprintTools.arrange_nodes` once.

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
- Name bools without a `b`: a variable `bOpen` gets the node IDs `Variables|Default|GetOpen` and `SetOpen`.
- A component is a variable: `(Variables|Default|GetDoor)` is the component named `Door`.
- Another Blueprint's variable `X` is `Class|<its name without underscores>|GetX` and `SetX`, with the object as
  `:self` and the value named after the variable: `(Class|BPBenchGameMode|SetScore :self mode :Score 5)`. A positional
  argument after a named one is refused.
- To cast to a Blueprint you just made, `compile_blueprint` it first; then `Utilities|Casting|CastTo<BP_Name>` exists.
- `create` fails when the asset exists. A script you may run again starts with
  `if t(S + 'exists', {'path': p}): t(S + 'delete', {'path': p})`.

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
| Overlap ends | `(event Collision|EventActorEndOverlap (OtherActor) ...)` |
| Hide it / turn its collision off | `Rendering|SetActorHiddenInGame :bNewHidden`, `Collision|SetActorEnableCollision :bNewActorEnableCollision` |
| Move a value toward a target | `Math|Interpolation|FInterptoConstant` (Current Target DeltaTime Speed), `Math|Interpolation|RInterpTo` |
| Turn a component | `Transformation|SetRelativeRotation :self component :NewRotation rotator` |

There is no Timeline node in the DSL: add timelines with HyperAI's structure tool below.

## Timelines, interfaces, RPCs, replication and collapsing: HyperAI's structure tool

`hyper_blueprint_structure_apply_plan` (toolset `HyperAIStudioBlueprintStructure.HyperAIStudioBlueprintStructureToolset`) does
what the DSL and Epic's tools leave out. A plan is one undo step; it is checked before anything changes, compiled after, and
saved (`bSave` false leaves it unsaved). The report's `Made` names every node it placed (name, title, pins) and
`CompileMessages` gives the compiler's errors and warnings with the node each is about. Ops:

- **Timelines** (Actor Blueprints, event graph): `timeline.add` with `Name`, `Length` in seconds (0 ends at the last key),
  `bAutoPlay`, `bLoop`, `bReplicated`, and `Tracks`, each `{Name, Type: float | vector | color | event, Keys: [{Time, Value}],
  Interp: auto | linear | constant}` (one value for float, three for vector, four for color, none for event). `Drive` names a
  function of the Blueprint that the timeline's Update runs, each track feeding its input of the same name: write
  `(fn ApplyOpen (Alpha) ...)` with the DSL first (into an `add_function_graph` graph), then add the timeline with
  `Drive: "ApplyOpen"`. Start it with `pins.connect` (below) to its `Play`, `PlayFromStart`, `Reverse` or `Stop`.
- **Interfaces**: `interface.create` (`Interface`: the new asset's path), `interface.add_function` (`Interface`, `Name`,
  `Inputs`, `Outputs`: a function without outputs is an event), then `interface.implement` (or `interface.remove`) on the
  Blueprint. The report lists its events and functions: place an event with `BlueprintTools.add_event(bp, 'Interact')` or an
  `(event Interact ...)` in the DSL, fill each function's graph, and call it on other actors with the interface message node
  `find_node_types` lists once the interface exists.
- **Networking**: `event.add` makes a custom event with `Inputs` and `Rpc` none | server | client | multicast and
  `bReliable`; `event.set_rpc` changes one. `variable.set_replication`: `Replication` none | replicated | rep_notify (makes
  `OnRep_<Name>`) and `Condition` owner_only | skip_owner | initial_only | simulated_only | autonomous_only |
  simulated_or_physics | initial_or_owner | custom | replay_or_owner | replay_only | skip_replay | none.
  `component.set_replicated` with `bReplicated`. An actor's replicated members only matter if the actor replicates: set
  `bReplicates` true on its default object with `ObjectTools.set_properties`.
- **Structure**: `graph.collapse` folds `Nodes` into a `collapsed` graph, a `function` or a `macro` (`Into`) named `Name`;
  `graph.expand` opens one collapsed graph, macro instance or function call back out, links kept.
- **Wiring**: `pins.connect` with `From` and `To` as `Node.Pin`: a node by its name (`K2Node_Timeline_0`) or its title when
  only one node has it: a timeline's or custom event's title is its name, and a new Actor Blueprint's events are
  `Event BeginPlay`, `Event ActorBeginOverlap` and `Event Tick` (so `"Event ActorBeginOverlap.then"` to `"Open.Play"`).
  The K2 schema checks every link.

A door that swings on a timeline: `add_function_graph` `ApplyOpen`, `add_function_param` `Alpha` float, write
`(fn ApplyOpen (Alpha) (Transformation|SetRelativeRotation :self (Variables|Default|GetDoor) :NewRotation (Math|Rotator|MakeRotator :Yaw (* Alpha 90.0))))`,
then one structure plan: `timeline.add` `Open` (Length 1, a float track `Alpha` keyed 0 at 0 and 1 at 1, `Drive` ApplyOpen)
and `pins.connect` `Event ActorBeginOverlap.then` to `Open.Play`.

`read_graph_dsl` shows a timeline as a call it is not (a timeline named Open reads back as `(MenuAnchor|Open 0.0)`), so never
write an event that holds a timeline back through `write_graph_dsl`: it would put that call in the timeline's place. Writing
other events into the same graph is safe and leaves the timeline and its links alone.

Types for parameters: `bool`, `int`, `int64`, `float`, `byte`, `name`, `string`, `text`, `vector`, `rotator`, `transform`,
`vector2d`, `linearcolor`, or `object:`, `class:`, `struct:`, `enum:` with a path (`object:/Script/Engine.Actor`,
`object:/Game/BP_Door.BP_Door_C`); `[]` after one makes an array.

`hyper_blueprint_structure_inspect` reads a Blueprint's graphs, interfaces (an interface asset's functions, or the ones it
implements), timelines and tracks, custom events with their replication, replicated variables and components; with `bNodes`
it lists every node with its name, title, graph and links, paged by `Offset` and `Limit`, which is how to read a large graph
and pick the nodes to collapse. A node name is unique only in its own graph: when a plan's node name is found in two graphs it
is refused, so give `Graph` then.

Event dispatchers are in Epic's tools: `BlueprintTools.add_event_dispatcher`, its parameters with `add_function_param` on
the dispatcher's graph, then the Call and Bind nodes `find_node_types` lists.

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

The pickup, door and health patterns below pass the benchmark's runtime checks in a game world. Each is one
`write_graph_dsl` after the components and variables exist.

- **Pickup that scores** (Game Mode `BP_BenchGameMode` with `int` Score, compiled first; pickup with `int` Points):
  ```
  (event Collision|EventActorBeginOverlap (OtherActor)
    (bind pawn (Utilities|Casting|CastToPawn :Object OtherActor)
      (:then
        (bind mode (Utilities|Casting|CastToBP_BenchGameMode :Object (Game|GetGameMode))
          (:then
            (Class|BPBenchGameMode|SetScore :self mode :Score (+ (Class|BPBenchGameMode|GetScore :self mode) (Variables|Default|GetPoints)))
            (Actor|DestroyActor))
          (:CastFailed)))
      (:CastFailed)))
  ```
  Spin it with a `RotatingMovementComponent` whose `RotationRate` is `{"Pitch": 0, "Yaw": 90, "Roll": 0}`.
- **Spawner:** a `Spawn` function graph holding
  `(fn Spawn () (Game|SpawnActorfromClass :Class "/Game/Path/BP_Thing.BP_Thing_C" :SpawnTransform (Transformation|GetActorTransform)))`,
  and `(event EventBeginPlay (Utilities|Time|SetTimerbyFunctionName :Object self :FunctionName "Spawn" :Time 2.0 :bLooping true))`.
- **Health that dies and respawns** (`float` Health, default 100, set on the default object):
  ```
  ; graph Respawn, from add_function_graph
  (fn Respawn ()
    (Variables|Default|SetHealth 100.0)
    (Rendering|SetActorHiddenInGame :bNewHidden false)
    (Collision|SetActorEnableCollision :bNewActorEnableCollision true))
  ; Event Graph
  (event Game|Damage|EventAnyDamage (Damage)
    (Variables|Default|SetHealth (Math|Float|Clamp(Float) (- (Variables|Default|GetHealth) Damage) 0.0 100.0))
    (if (<= (Variables|Default|GetHealth) 0.0)
      (Rendering|SetActorHiddenInGame :bNewHidden true)
      (Collision|SetActorEnableCollision :bNewActorEnableCollision false)
      (Utilities|Time|SetTimerbyFunctionName :Object self :FunctionName "Respawn" :Time 2.0 :bLooping false)))
  ```
  Other actors hurt it with `Game|Damage|ApplyDamage`.
- **Door that opens and closes** (a Static Mesh component `Door`, a Box component around it, `bool` Open,
  `float` Yaw):
  ```
  (event Collision|EventActorBeginOverlap (OtherActor)
    (Variables|Default|SetOpen true))
  (event Collision|EventActorEndOverlap (OtherActor)
    (Variables|Default|SetOpen false))
  (event EventTick (DeltaSeconds)
    (Variables|Default|SetYaw (Math|Interpolation|FInterptoConstant (Variables|Default|GetYaw) (select (Variables|Default|GetOpen) 90.0 0.0) DeltaSeconds 90.0))
    (Transformation|SetRelativeRotation :self (Variables|Default|GetDoor) :NewRotation (Math|Rotator|MakeRotator :Yaw (Variables|Default|GetYaw))))
  ```

After the Blueprint exists, place it with `editor_toolset.toolsets.scene.SceneTools` or a HyperAI level plan, and prove
it works with a playtest when the task asks for runtime behaviour.
