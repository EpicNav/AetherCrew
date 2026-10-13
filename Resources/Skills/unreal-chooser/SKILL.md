---
name: unreal-chooser
description: Build, read and test Unreal chooser tables - pick an animation (or any asset or class) from the character's state with filter columns (bool, enum, float ranges, gameplay tags), scoring, random and output columns, nested tables and a fallback, in one undoable plan; check which row wins for given values; call the table from an Animation Blueprint or Blueprint; and how pose_match columns tie in with motion matching.
---

# Chooser tables

| work | tool |
|---|---|
| the column kinds this editor has, what their cells take, the ops, the tables under /Game; or one table's parameters, columns, rows, fallback and nested tables | `hyper_chooser_inspect` |
| make a table, add and change columns and rows, set the fallback, one undo step | `hyper_chooser_apply_plan` |
| run the table with chosen values: the row that won, what it gave, the outputs, and the rows each column turned away | `hyper_chooser_evaluate` |

Toolset `HyperAIStudioChooser.HyperAIStudioChooserToolset`, each tool called with `{"request": {...}}`. The pack needs
the Chooser plugin; pose_match columns also need the PoseSearch plugin.

A chooser table answers "which animation now?" from data instead of graph logic. It reads **parameters** (a class
such as the character or its movement component, or a struct of the anim instance's variables), and each **row**
gives a result: an asset, a class, or another chooser table. **Columns** decide which row wins:

- filter columns turn rows away: a row passes when its cell matches the bound property (speed in 0..300, stance is
  Crouch, has the tag State.Armed);
- scoring columns (`float_distance`, `pose_match`) order the rows that passed, best first;
- a `randomize` column picks among the rows that passed, by weight;
- output columns write a value of the winning row back into a parameter (a play rate, a blend time, a tag).

The first row that passes every filter wins, so put specific rows above general ones. When no row passes, the
**fallback** is used; without one the table gives nothing.

## A plan

Locomotion: a speed range and a stance enum pick idle, walk or run, with a play rate written out.

```json
{"request": {"Asset": "/Game/Animation/CT_Locomotion",
 "Operations": [
  {"Kind": "create", "OutputObjectType": "/Script/Engine.AnimSequence",
   "ContextStructs": ["/Game/Animation/S_LocomotionState"], "ContextDirections": ["both"]},
  {"Kind": "column.add", "ColumnKind": "float_range", "Binding": "Speed"},
  {"Kind": "column.add", "ColumnKind": "enum", "Binding": "Stance"},
  {"Kind": "column.add", "ColumnKind": "output_float", "Binding": "PlayRate"},
  {"Kind": "row.add", "Result": "/Game/Animation/Idle", "Cells": ["..10", "Standing", "1"]},
  {"Kind": "row.add", "Result": "/Game/Animation/Walk", "Cells": ["10..300", "Standing", "1"]},
  {"Kind": "row.add", "Result": "/Game/Animation/Run", "Cells": ["300..", "Standing", "1.1"]},
  {"Kind": "row.add", "Result": "/Game/Animation/CrouchWalk", "Cells": ["", "Crouching", "0.9"]},
  {"Kind": "fallback.set", "Result": "/Game/Animation/Idle", "Cells": ["", "", "1"]}],
 "bDryRun": true}}
```

Run it with `bDryRun: true` first: every op runs on a copy and the reply's `Notes` say what each did (the index each
new column got) and `Warnings` what could go wrong in play. Then run it without `bDryRun`. With approval on it waits
in the Activity panel: call `hyper_operation_wait` with `bWaitThroughApproval: true`. One Edit > Undo takes back the
whole plan; `bSave: true` saves the table.

A mistake is refused before anything changes, naming the op by its number and what it takes: an unknown column kind
lists the kinds, a binding to a property that is not there lists the parameters and the properties that fit, a row
with the wrong number of cells lists the columns in order.

## Ops

- `create` comes first, once, for a table that is not there yet. `ResultType` is `object` (each row gives an asset
  of `OutputObjectType`, the default), `class` (each row gives a subclass of it: a character to spawn, an anim
  instance to link) or `none` (rows only write outputs). `ContextClasses` then `ContextStructs` are the parameters,
  named Context0, Context1... in that order; `ContextDirections` says for each whether it is `input`, `output` or
  `both`. Output columns write to an output or both parameter.
- `column.add`: `ColumnKind`, `Binding` (the property it reads or writes) and optional `Params` settings and
  `Cells` (one per existing row). The table keeps the Chooser editor's order: filter columns first, output columns
  after them, `randomize` last; a filter added later goes before the outputs, so check `Notes` for its index.
- `column.set`: `Column`, then any of `Binding`, `Params` (`Disabled=true` turns a column off), `Cells` (one per row).
- `column.remove`: `Column`; the columns after it move left.
- `row.add`: `Result`, `Cells` (one per column in column order, empty for any), `Row` to insert somewhere other than
  the end, `Disabled`.
- `row.set`: `Row`, then any of `Result`, `Cells`, `Disabled`.
- `row.remove`: `Row`; the rows after it move up.
- `fallback.set`: `Result` (`none` clears it) and `Cells`: the output columns' values when the fallback is used;
  filter columns' cells stay empty.

A `Result` is an asset path, `soft:/Game/...` for a soft reference (loaded on demand), a class path for a class
table, `chooser:/Game/CT_Other` to run another table (sub-tables per weapon or per stance), or `nested:Name` for a
table nested inside this one (nested tables are made in the Chooser editor; inspect lists them).

`Binding` is a property path on the parameters: `Speed` (the first parameter that has it), `Context1.Speed`, or a
struct member `Context0.Velocity.X`. As in the Chooser editor, only Blueprint-visible properties bind, a path goes
through struct members (not object references), and each column takes its own type: bool for `bool`, a float,
double or int for the float columns, an enum (or enum byte) for the enum columns, a gameplay tag container for the
tag columns, an object reference for `object` and `object_class`, a name for `name`.

## Column kinds and their cells

| kind | cells |
|---|---|
| `bool` | true, false, or any (empty) |
| `enum` | a value (Walking), `!value` for any other, or any |
| `multi_enum` | values joined by a vertical bar (below): passes when the input is one of them |
| `float_range` | `min..max` (0..300), `..max`, `min..`, or any; both ends inclusive. With `WrapInput=true` (angles) a backwards range such as `135..-135` runs across the wrap |
| `float_distance` | a number: the rows nearest the input score best (`MaxDistance`, `CostMultiplier`, `FilterOverMaxDistance`) |
| `gameplay_tag` | tags, comma separated; `TagMatchType` (Any, All), `TagMatchDirection`, `MatchExact`, `InvertMatchingLogic` |
| `gameplay_tag_query` | `any:Tags`, `all:Tags` or `none:Tags`; empty matches anything |
| `object` | an object path, `!path`, or any |
| `object_class` | a class (the object is one or a subclass), `=class` (exactly), `!=class`, `!class` (not one), or any |
| `name` | a name, `!name`, or any |
| `randomize` | a weight (1 when empty); bind an optional FChooserRandomizationContext to avoid repeats (`RepeatProbabilityMultipliers`) |
| `output_float`, `output_bool`, `output_enum`, `output_name`, `output_gameplay_tag` | the value written to the bound property |
| `output_object` | an asset path, `soft:path`, `chooser:path` or `nested:Name` |
| `pose_match` | left empty (see motion matching below) |

A `multi_enum` cell is written `Walking|Running`; empty is any.

`hyper_chooser_inspect` with no Asset lists every column type this editor has, what its binding takes, and its
settings with their defaults (the names `Params` takes: `bWrapInput` or `WrapInput`, `MaxDistance`...).

## Checking it

`hyper_chooser_evaluate` runs the table as the game does, through the engine's own evaluation, with values you give:

```json
{"request": {"Asset": "/Game/Animation/CT_Locomotion", "Values": "Speed=450; Stance=Standing"}}
```

It replies with `Row` (the winner, -1 when the fallback or nothing was used), `bFallback`, `Result`, `Outputs`
(each output column's value after the choice), `Passed` (rows that passed every filter), and `Filtered`: for each
filter column on its own, the value it read and the rows it turned away. That is the quickest way to find the
column that blocks a row. Class parameters start from their class defaults and struct parameters from the struct's
defaults; a name without `ContextN.` is set on the first parameter that has it. A `randomize` column makes the pick
random (`bRandom`); give `Seed` to repeat it. Check a value in each row's range and one on each boundary.

## Calling it from a Blueprint or Animation Blueprint

- **Evaluate Chooser** (Blueprint node, also in an Animation Blueprint's thread-safe functions): pick the table and
  it grows a pin per parameter and an output of the result type; outputs written by output columns come back
  through the parameters marked output or both. Use it in `BlueprintThreadSafeUpdateAnimation` to choose a sequence
  into a variable a Sequence Player plays.
- **Chooser Player** (AnimGraph node): plays what the table chooses through a blend stack and re-evaluates on the
  frequency you set (on become relevant, on loop, every update), with blend time and play rate from its settings
  or output columns.
- A table's parameter types must match what the caller passes: the anim instance class (or the character), or a
  struct of locomotion variables. Make a struct parameter `both` when output columns write into it.

## Motion matching

A `pose_match` column (Pose Match, from the PoseSearch plugin) runs a motion matching search over the rows'
animations that passed the filters, scoring each by how well its poses continue the current pose history; its
`Binding` is the pose history (an FPoseHistoryReference). The column keeps its own pose search databases inside the
table, one per row group, so the plan adds the column and binds it but leaves its cells empty: pick each row's
database in the Chooser editor. Put it right of the filters (the plan does), so filters narrow the rows first.
For standalone motion matching data (schemas and databases searched by a Motion Matching node), use
`hyper_pose_search_apply_plan` and `hyper_pose_search_inspect`; a chooser table is often what feeds a Motion
Matching node its database by state.

## Pitfalls

- Row order matters: the first passing row wins (unless a scoring column orders them). General rows go last, or in
  the fallback.
- An unbound filter column lets every row through; inspect shows `Binding` and any `Problem` for each column.
- A float range's ends are inclusive: `0..150` and `150..` both pass 150; use `..149.9` when it matters.
- An enum cell names a value of the bound enum; bind the column before giving cells.
- Output columns write only into output or both parameters at run time; the Warnings say when one points at an input.
- Changing the parameters of an existing table is done in the Chooser editor; the plan's `create` sets them for a
  new table.

## Limits

64 ops per plan, 32 columns, 512 rows, 8 parameters (at most 4 classes). Tables go under `/Game` only. Plans are
refused during Play In Editor. Evaluation makes fresh objects of class parameters, so an abstract class parameter
is evaluated in play instead.
