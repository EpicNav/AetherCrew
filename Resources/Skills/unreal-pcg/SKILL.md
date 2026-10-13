---
name: unreal-pcg
description: Work with Procedural Content Generation - build and edit PCG graphs node by node with Epic's PCG tools, scatter meshes and grow ecosystems over landscapes with HyperAI's PCG scatter, read any graph back as recipes, and generate, clean up and validate a PCG component as a job you can wait for, with the known World Partition and regeneration gotchas. Use for any PCG graph, PCG volume, procedural scatter, generation, or a PCG result that came out empty or doubled.
---

# PCG graphs, scatter and generation

Three sets of tools, through `call_tool`:

| toolset | tools | for |
|---|---|---|
| `HyperAIStudioPCGScatter.HyperAIStudioPCGScatterToolset` | `hyper_pcg_scatter_inspect`, `hyper_pcg_scatter_apply_plan` | scatter and ecosystem graphs in one plan, PCG volumes, generate and clear; reading any graph |
| Epic's `PCGToolset.PCGToolset` and `PCGToolset.PCGSpatialToolset` | `CreateGraph`, `AddNode`, `ConnectNodePins` and the rest below | building any other graph node by node |
| `HyperAIStudioPCG.HyperAIStudioPCGToolset` | `hyper_pcg_inspect`, `hyper_pcg_validate`, `hyper_pcg_apply_plan` | one component: its nodes and state, validation, generate or clean up as a job |

The level-scale workflow (scatter rules, ecosystems with biomes and species, densities, `Cover` and `Partitioned`
volumes, road margins) is in the unreal-level-design skill. Read it before scattering; this skill covers the rest.

## Scatter plans, briefly

`hyper_pcg_scatter_apply_plan` takes `{"request": {"Operations": [...], "bDryRun", "bSave", "OperationId"}}`: up to 32
ops, `graph.create`, `graph.set`, `ecosystem.create`, `ecosystem.set`, `volume.create`, `volume.generate` and
`volume.clear`. A plan generates once, as its last op (`volume.create` or `volume.generate`), and then answers
`generating`: call `hyper_operation_wait` with the `OperationId` for the instance counts per mesh. Limits: `Density` up
to 20 points per square meter; 8 biomes, 12 species per biome and 40 species in all. Graphs are always saved;
`graph.set` and `ecosystem.set` only change graphs this tool built. The level edits are one undo step.

To retune a scatter, change the rule and regenerate in one plan, rather than editing instances:

```json
{"request": {"Operations": [
 {"Kind": "graph.set", "Graph": "/Game/PCG/PCG_ValleyRocks", "Density": 0.08, "SlopeMax": 30, "Seed": 12},
 {"Kind": "volume.generate", "Volume": "ValleyRocks"}],
 "bDryRun": false, "OperationId": "valley-rocks-2"}}
```

Fields left out (or 0, or negative where the field says so) keep their values. Every volume running the graph follows
the new rules when it regenerates.

## Building a graph by hand

For what the scatter ops cannot express (spline-driven placement, attribute logic, subgraphs, a marketplace
generator's method), build with Epic's `PCGToolset.PCGToolset`:

1. `ListNativeNodes` (common nodes; `bCommonOnly` false for all) and `GetNativeNodeSchema` for a node's pins and params.
2. `CreateGraph` (`Name`, `Path`, default `/Game/PCG`), then `AddNode` (`Graph`, `NativeNodeType`, a unique
   `NodeName`, `JsonParams` with only the non-default params, `NodeTitle`, `XPositionIdx`, `YPositionIdx`) or
   `AddSubgraphNode`.
3. `ConnectNodePins` (from node and pin label to node and pin label); `UpdateNode`, `RemoveNode`, `DisconnectNodePins`
   to change it; `SetGraphParams` for user parameters; `GetGraphStructure` to read it back.
4. Run it: `SpawnGraphInstance` places a PCG volume running it, `ExecuteGraphInstance` generates that volume, and
   `SetGraphInstanceParams` overrides its parameters. `PCGSpatialToolset.RunPCGInstantGraph` runs a graph once with
   parameters, without a volume. `GetNodeDataView` shows the data a node produced.

Before copying a generator, read it: `hyper_pcg_scatter_inspect` with `Graph` gives one recipe per mesh spawner (the
nodes its points pass through, settings that differ from defaults); `Detail: "nodes"` or `"full"` adds every node and
edge, and `Filter` narrows by name, title or mesh. With no `Graph` it lists the level's PCG volumes and the graphs
under `/Game`.

## Generating one component as a job

`hyper_pcg_apply_plan` generates or cleans up any PCG component and waits for it to finish. It is two calls:

1. `hyper_pcg_inspect` with `GraphPath` (the saved graph's object path, loaded in the editor; nothing is loaded for
   you) and `ComponentPath` (such as `/Game/Maps/Valley.Valley:PersistentLevel.PathVolume.PCGComponent`). Keep
   `Graph.PersistedRevision` and `Component.PersistedFingerprint`; `bGenerated` and `bGenerating` say its state.
2. A dry run, then the same request with `bDryRun: false`, a new `OperationId` and `ExpectedPlanHash` set to the dry
   run's `PlanHash`:

```json
{"request": {"bDryRun": true,
 "GraphPath": "/Game/PCG/PCG_Path.PCG_Path",
 "ComponentPath": "/Game/Maps/Valley.Valley:PersistentLevel.PathVolume.PCGComponent",
 "ExpectedPersistedRevision": "<Graph.PersistedRevision>",
 "ExpectedComponentFingerprint": "<Component.PersistedFingerprint>",
 "Intent": {"Lifecycle": "generate", "ValidationPolicy": "generation_ready", "ExpectedTerminalState": "generated"},
 "DeadlineMs": 60000}}
```

- `Lifecycle` is `generate` (terminal `generated`), `cleanup` (terminal `clean`) or `generate_validate_cleanup`
  (proves the graph generates and leaves nothing; terminal `clean`). `ValidationPolicy` is always `generation_ready`.
- `DeadlineMs` is 5000 to 120000; past it the job is cancelled. It can also be cancelled from the Activity panel.
- A changed graph or component after inspection is refused: inspect again. With approval on, wait with
  `hyper_operation_wait` and `bWaitThroughApproval: true`.

## Gotchas

- **Nothing grew, and no error.** In a World Partition level PCG sees a landscape kept whole (not split into streaming
  proxies) only once the level is saved. The scatter job says so and names the landscape; generate again after
  `bSave: true`. With Epic's tools or `hyper_pcg_apply_plan`, save the level yourself first.
- **Every instance twice.** If PCG restarts a generation partway (actors it tracks were added the frame it began), the
  cancelled pass can leave its instances beside the new ones. The scatter job detects this and generates once more,
  and says so if it could not. After other generation routes, compare counts with the expected and run `cleanup`, then
  `generate`.
- **Partition seams.** A partitioned volume generates per World Partition cell (256 m by default). Scatter species
  sample up to 128 m past their cell so they keep out of each other across the edge; a hand-built graph needs its own
  overlap.
- "Identical ISM Component spawn" warnings in the log are normal for reused components.
- A volume grows on the ground inside it: make it tall enough, or `Cover` the landscape.

## Checking

- The scatter job's result: instances per mesh, and why when none grew.
- `hyper_pcg_validate` with `GraphPath`, `ComponentPath`, the revision and fingerprint you expect, and `Policy`
  `structural` or `generation_ready`: `bValid`, `ErrorCount`, and `Issues` with codes.
- `hyper_pcg_scatter_inspect` with no `Graph`: each volume's graph, size, instances made and whether it is generating.
- Look at the result from where the player stands; `hyper_scene_cost_report` (unreal-level-design) for what it costs.
