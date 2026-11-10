---
name: performance
description: Profile an Unreal play session with HyperAIStudio and explain it - whether it is game-thread, render-thread, RHI or GPU bound or capped by vsync or a frame limit, its hitches and hot functions, and what to try for each bound - then compare before and after a change, and read Nanite streaming, Niagara cost and renderer settings live during play. Use when the user mentions performance, frame rate, fps, frame time, hitches, stutter, lag, slow, profiling, CPU or GPU bound, bottleneck, Unreal Insights, stat unit, Nanite streaming or Niagara cost.
---

# Profile a play session

All three tools are on toolset `HyperAIStudioProfiling.HyperAIStudioProfilingToolset`, through `call_tool`.

## Measure

`hyper_profile_session` measures Play In Editor and answers when it is done (warm-up + duration + about 10-30 s of
analysis). One session runs at a time.

```json
{"request": {"mode": "auto", "durationSeconds": 10, "warmupSeconds": 3, "hitchThresholdMs": 50, "topN": 10,
  "bHotFunctions": true, "bSceneCost": true, "label": "before LOD change", "operationId": "arena-before"}}
```

- **mode:** `auto` (default) attaches when play runs, else starts it; `attach` needs play running; `start` starts
  play and stops it after. Starting play or a bot route is an edit: with approvals on it answers
  `awaiting_user_approval`. Then `hyper_operation_wait` with the operationId and `bWaitThroughApproval: true`, and
  read the profile with `{"request": {"profileId": "<id>"}}`.
- **warmupSeconds:** 3 by default when it starts play, 0 when it attaches. Loading and first-use shader hitches stay
  out of the numbers.
- **durationSeconds:** 1 to 60. 10 is enough for a steady scene. Measure a whole fight or route for hitches.
- **bot:** up to 16 playtest bot steps, played while it measures (`MoveTo`, `Play` with goal `Explore`,
  `Survive`...). The same objects as `hyper_playtest_run`'s `bot` field; see the `unreal-playtest` skill. `Bot`
  reports how the route went.
- **csvPath:** analyse a CSV you or the user captured (`csvprofile start/stop`, `Saved/Profiling/CSV/...`) instead.
- Background throttling is held off while it measures, so a profile taken with the editor behind other windows still
  measures the game. The editor's own cost (Slate, editor ticks) is in every number: for shipping figures, measure a
  packaged or `-game` build too.

## Read the verdict

`verdict.bound`, with `verdict.summary`, `evidence` and `breakdown` (share of frames per bound):

| bound | means | look at |
|---|---|---|
| `game_thread` | The game thread is the busiest on most frames | `topCpuStats` (GameThread), `hotFunctions` |
| `render_thread` | Building draw commands and visibility | `drawCalls`, `topCpuStats` (RenderThread), `sceneCost` |
| `rhi_thread` | Submitting to the GPU driver | `drawCalls`, `primitives` |
| `gpu` | The GPU is the busiest | `topGpuStats` (passes), `sceneCost`, `renderer` |
| `capped` | Nothing is busy for most of the frame: vsync, `t.MaxFPS`, a fixed or smoothed frame rate | `verdict.capSource` |
| `unknown` | GPU time unavailable and no known limit | `gpuNote` |

- A frame is bound by whatever was busiest in it; when nothing is busy for 80% of it, it was waiting on a limit.
- Evidence notes a near tie ("within 10%"): fixing one leaves the frame bound by the other.
- `bGpuTimeAvailable` false (`gpuNote` "GPU time unavailable", e.g. a `-nullrhi` editor): the GPU's share is
  unknown, never zero. Say so; do not call it CPU-bound without the evidence.
- `capped`: the real bound is hidden. Measure again with `t.MaxFPS 0` and `r.VSync 0` (ask the user first: it
  changes their settings) before optimising anything.

## What to try per bound

- **game_thread:** the top GameThread stats and hot functions. Ticking actors (tick intervals, turn off ticks that do
  nothing), animation (update rate optimisation, budgeting), physics (simpler collision, sleeping), Blueprint loops,
  AI and navigation, garbage collection, Niagara game-thread cost (`fxSystems`).
- **render_thread / rhi_thread:** draw calls. Nanite or instancing for repeated meshes, merged actors, cull distances,
  fewer movable shadow-casting lights (each draws the scene again). `sceneCost` names the heaviest meshes.
- **gpu:** the heaviest passes in `topGpuStats`. Lumen (scalability), shadows (fewer movable shadow casters, virtual
  shadow map page pool), translucency overdraw, base pass material cost, post processing. A lower
  `r.ScreenPercentage` that lowers frame time confirms the GPU bound.
- Change one thing at a time and compare.

## Hitches and hot functions

- `hitches`: frames over `hitchThresholdMs`, or over twice the median and 5 ms above it, slowest first. Each has its
  `frame`, `atSeconds`, `frameMs`, `bound`, the `spikes` (stats far over their median that frame) and CSV `events`
  (level streaming, GC). `hitchCount` counts them all.
- `hotFunctions`: the hottest CPU timers (exclusive time, waits left out) on the game, render and RHI threads, from an
  Unreal Insights trace analysed in the editor. `hotFunctionsInWorstFrame`: inside the trace's slowest game frame.
  `hotFunctionsNote` says when there are none: a trace was already recording (Insights, `-trace`), so a second could
  not start.
- `waits`: time threads spend waiting on each other. A large game-thread wait on a GPU-bound frame is expected.

## Compare before and after

Profile with the same mode, duration, level and route before and after the change, then:

```json
{"request": {"baseline": "arena-before", "candidate": "arena-after", "thresholdPercent": 5}}
```

`metrics` (frame time average and p50/p95/p99/max, fps, each thread, GPU, draw calls, hitches per minute, memory) and
`stats` (the CPU and GPU stats that moved most), each `better`, `worse` or `same`, plus `boundBefore`/`boundAfter`.
A change under the threshold is noise. Either side can be a CSV path.

## Live: Nanite, Niagara, renderer

`hyper_profile_live` reads the running game, read-only, while play runs:

```json
{"request": {"sampleSeconds": 3, "topN": 10}}
```

- `sampleSeconds` 0 reads now. 1-10 also measures per-system Niagara cost (game thread, render thread, GPU,
  microseconds per frame), Nanite streaming (root pages, visible streaming data, pages requested) and shadow pages
  over the window. It cannot sample while a profile session runs.
- `nanite`: enabled, streaming pool (MB, pages), `qualityScale` (below 1: the visible set does not fit the pool), Nanite
  and fallback components with why each falls back. Visible clusters and instances are a renderer-private GPU readback
  in 5.8 (`r.Nanite.ShowStats 1` shows them on screen); eviction counts are not exposed.
- `niagara`: active components, emitters, CPU and GPU particles (GPU counts lag a frame), `effectTypes` with their
  caps, `overBudget` (an effect type at its cap culls new spawns), the FX budget.
- `renderer`: GI and reflection method, Lumen hardware ray tracing, virtual shadow maps, MegaLights, screen percentage,
  vsync and `t.MaxFPS`, and the last frame's thread times and draw calls.

## Report

Lead with the bound and its evidence, then a table of frame time (avg, p50, p95, p99, max), threads, draw calls and
hitches, then the top stats or passes and the advice. Quote numbers from the profile; keep advice to what the figures
show.
