---
name: unreal-eqs
description: Build Environment Queries (EQS) that let AI pick places and targets - cover spots, flanking points, the nearest enemy, a spot in sight - with generators, contexts and scored or filtering tests in one undoable plan; wire them into Behavior Trees as a Run EQS Query task or a Run EQS service writing to a Blackboard key; then run the query from a querier in the level or in Play In Editor to see the winning items and their scores. Also how State Trees run a query.
---

# Environment Queries (EQS)

| work | tool |
|---|---|
| the generator, test and context kinds with their settings; a query's options and tests; a tree's composites, keys and EQS nodes | `hyper_eqs_inspect` |
| make or change a query, and wire it into a Behavior Tree, one undo step | `hyper_eqs_apply_plan` |
| run it now from a querier and see the best items and scores | `hyper_eqs_run` |

Toolset `HyperAIStudioEQS.HyperAIStudioEQSToolset`, called with `{"request": {...}}`. EQS is part of the engine's AI
module, so the tools are there in every project. Behavior Trees and Blackboards themselves (keys, the tree's
Blackboard) are `hyper_gameplay_ai_*`'s and Epic's `AIModuleToolset`'s; this pack adds the queries and the EQS nodes.

## How a query works

A query has **options**, tried in order. Each option has one **generator** that makes items (points or actors) around
a **context**, and **tests** that run over the items in order. A test's `TestPurpose` is `Filter` (drop items outside
`FilterType`'s bounds), `Score` (add a weighted, normalised score) or `FilterAndScore`. The first option whose items
survive its filters wins; within it the best score wins. An option with no tests always wins when it makes items, so
put fallbacks last.

Contexts are `querier` (the actor that asks: the AI's pawn), `item` (the item under test, for tests that compare two
things) or a Blueprint context class (`/Game/AI/EQC_Player.EQC_Player_C`, a subclass of `EnvQueryContext_BlueprintBase`
that returns the player, the target...).

## A plan

```json
{"Asset": "/Game/AI/EQS_FindCover",
 "Operations": [
  {"Kind": "create"},
  {"Kind": "option.add", "Type": "donut", "Params": "InnerRadius=300; OuterRadius=1200; NumberOfRings=4; Center=querier"},
  {"Kind": "test.add", "Type": "trace", "Params": "Purpose=Filter; Bool=true; Context=/Game/AI/EQC_Player.EQC_Player_C"},
  {"Kind": "test.add", "Type": "distance", "Params": "Purpose=Score; Scoring=InverseLinear; Context=querier"},
  {"Kind": "test.add", "Type": "distance", "Params": "Purpose=FilterAndScore; Filter=Minimum; Min=param:MinRange; Context=/Game/AI/EQC_Player.EQC_Player_C"},
  {"Kind": "bt.add_composite", "BehaviorTree": "/Game/AI/BT_Soldier", "Parent": "root", "Type": "sequence", "Params": "NodeName=Take Cover"},
  {"Kind": "bt.run_eqs_task", "BehaviorTree": "/Game/AI/BT_Soldier", "Parent": "Take Cover", "BlackboardKey": "CoverLocation", "Params": "RunMode=SingleResult"}]}
```

That finds points 3 to 12 m away, hidden from the player, nearest first, at least `MinRange` from the player, and the
Soldier tree's new Take Cover sequence writes the best one to `CoverLocation`; a Move To that key after it moves there.

`bDryRun: true` applies the plan to a copy and replies with the query it would leave (`Options` with each test's
purpose, description and changed settings, `QueryParams`, `Warnings`); nothing changes. `bDryRun: false` makes it,
unsaved unless `bSave: true`, in one undo step that also takes back the tree nodes. With approval on it waits in the
Activity panel: call `hyper_operation_wait` with `bWaitThroughApproval: true`.

## Op kinds

- `create` comes first, once, and makes the query asset. Without it the plan changes an existing query.
- `option.add` with `Type` the generator: `grid` (square grid: `GridSize` half-width, `SpaceBetween`), `circle`
  (`CircleRadius`, `SpaceBetween` or `NumberOfPoints`, an arc), `donut` (rings from `InnerRadius` to `OuterRadius`),
  `cone` (`ConeDegrees`, `Range`, `AngleStep`), `pathing_grid` (grid points the querier can walk to), `actors_of_class`
  (`SearchedActorClass`, optionally `GenerateOnlyActorsInRadius` and `SearchRadius`), `current_location` (the
  context's own spot), `composite` (several generators' items together: `Children`, each `"kind"` or
  `"kind: Name=Value"`), `perceived_actors` (what the querier's AI Perception sensed), or a Blueprint generator class.
- `option.set` changes a generator's settings; `option.remove` removes the option. `Option` picks it, from 0; -1 (the
  default) is the last, such as the one just added.
- `test.add` with `Type` the test: `distance`, `dot` (two directions, `LineA`/`LineB`: is it in front, behind),
  `trace` (can the context see it), `pathfinding` and `pathfinding_batch` (is there a path, how long), `overlap`,
  `gameplay_tags` (`TagQueryToMatch=any:State.Hidden`), `project` (onto the navmesh), `random` (breaks ties) or
  `volume`. It goes on option `Option`.
- `test.set` and `test.remove` pick the test with `Option` and `Test` (from 0, -1 the last).
- `bt.add_composite` adds a `selector` or `sequence` under `Parent` in `BehaviorTree` (`Params` `NodeName=...` names
  it, so later ops can say `Parent` by that name).
- `bt.run_eqs_task` adds a Run EQS Query task running this query under the composite `Parent`, writing the winner to
  `BlackboardKey` (a Vector key for points, an Actor object key for actors). `Params` set the task: `RunMode`
  (`SingleResult`, `RandomBest5Pct`, `RandomBest25Pct`, `AllMatching`), `bUpdateBBOnFail`, `NodeName`.
- `bt.run_eqs_service` puts a Run EQS service on the composite `Parent` instead: the query reruns every `Interval`
  seconds while that branch is active (`Params` `Interval=0.5; RandomDeviation=0.1`), for targets that move.

`Parent` is `root` (the tree's first composite, or the root itself when the tree is empty: only `bt.add_composite` may
go there), a composite's node name, or `#index` from `hyper_eqs_inspect` with `BehaviorTree`. A tree with no graph yet
(made in code, never opened) gets one, as the Behavior Tree editor would make it. The task and service collect the
query's named params into their Query Config, so the tree can feed them from Blackboard keys.

## Settings

`Params` is `"Name=Value; Name=Value"`: the generator's or test's own properties (`hyper_eqs_inspect` with no Asset
lists every kind's with defaults), case-insensitive, nested ones through a dot (`ProjectionData.TraceMode=None`,
`LineA.LineFrom=querier`). Values: numbers; `true`/`false`; enum short names (`TestPurpose=FilterAndScore`,
`FilterType=Range`, `ScoringEquation=InverseLinear`, `Constant`, `Square`, `SquareRoot`, `Linear`); contexts; class
paths; tag queries `any:A.B,C.D`, `all:...`, `none:...` (the tags must exist). Short names: `Purpose`, `Filter`,
`Min`, `Max`, `Scoring`, `Factor` (the score weight; negative prefers low values), `Clamp` (both clamp types),
`Context` (the kind's main context), `Name` (the option's name).

Numbers that EQS can bind take `param:Name` (`Max=param:Reach`): a named query param that a Behavior Tree node's Query
Config, a Blueprint `Run EQS Query` call or `hyper_eqs_run`'s `Params` sets. `QueryParams` lists them.

Points from `grid`, `circle`, `donut` and `cone` are projected onto the navmesh by default
(`ProjectionData.TraceMode=Navigation`), so a level without a built navmesh (a Nav Mesh Bounds Volume) drops them all;
`Warnings` say so. Set `ProjectionData.TraceMode=None` while blocking out, or `GeometryByChannel` to drop points onto
the floor.

## Running it

`hyper_eqs_run` with `Asset`, `Querier` (an actor's label; empty uses the selected actor) and `Params` for every named
param runs the query now: in the open level, or in Play In Editor while it runs (the querier is then the playing
actor with that label). The reply gives `Option` (which option won), `Passed` (items that passed every filter),
`Items` best first with `Location`, `Actor` and `Score`, and how long it took. `RunMode` `random_best_5` or
`random_best_25` also says how many items the random pick chooses among. `failed` means every item was filtered out:
read `Warnings`, then loosen a filter or check the navmesh. Nothing changes.

Prove a query this way before wiring it: place a target point where the AI stands and run from it, then compare the
best item with where you expect cover to be. Unreal's own EQS Testing Pawn (place one, set its Query Template) draws
the scores in the viewport for a person to look at.

## State Trees

State Trees run queries with the Run Env Query task (`FStateTreeRunEnvQueryTask`, GameplayStateTree plugin): its
`QueryTemplate` is the query, `RunMode` as above, `QueryConfig` its params, and its `Result` is bound to a Vector or
Actor parameter of a parent state, which the sibling state that uses it reads. HyperAI does not edit State Trees yet:
add that task in the State Tree editor (Add Task > Run Env Query) and point it at the query this plan made. Epic's
`StateTreeToolset` reads a tree's states and tasks back.

## Pitfalls

- Score tests need something to compare: `distance` to the querier with `Scoring=InverseLinear` prefers near items;
  `Factor=-1` flips any score.
- Filter tests run before score tests are worth anything: put cheap filters (distance, dot) before traces and
  pathfinding, which cost the most.
- A `trace` test keeps the items whose trace result equals `Bool`: `Bool=true` keeps spots where something blocks
  the line to the context (hidden from it), `Bool=false` spots in its sight.
- A Blackboard key written by Run EQS must match the items: Vector for points, an Actor key for actors.
- Change a query with the EQS editor closed: a plan closes its editor tab, and the editor rebuilds the graph from the
  query when you open it again.
