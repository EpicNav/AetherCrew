# Writing prompt presets

The chat's **Prompts** picker (and Ctrl+Space in the composer, and Ctrl+K search) offers the presets in this folder. A
preset is a tested request for one kind of work: the prompt the user starts from, the packs, skills and project agent it
goes with, and the blanks the user fills. Picking one only puts the filled prompt in the composer; nothing is sent until
the user sends it. The message then carries the preset's packs, skills and agent, so HyperAI names the right tools and
skills for it instead of guessing from the words.

## Files

- `categories.json`: the categories, `{"schema": "hyperai.prompt-categories.v1", "categories": [...]}`. Each has an `id`
  (lower-case words joined by hyphens), a `title` (60 characters at most), an `order`, an `icon` (a
  `HyperAIStudio.Icon.*` brush), a `min` (how many presets the shipped library must hold for it) and an optional
  `route`, one of the default task routes (`Plan a feature`, `Materials`, `VFX`, `Lighting`, `Level Design`,
  `Animation`, `Gameplay`). A new chat's first message from a preset takes its category's route, or, when the user
  renamed or removed that route, the route its words fit.
- `<category>.json`: one file per category, `{"schema": "hyperai.prompt-presets.v1", "category": "<id>", "presets": [...]}`.
  The file's name is its category's id.
- `Tools/Prompts/zh-Hans/<category>.json`: the Simplified Chinese for each preset, a flat object keyed
  `"<id>.title"`, `"<id>.summary"`, `"<id>.ph.<name>.label"` and `"<id>.ph.<name>.hint"`; category titles are in
  `Tools/Prompts/zh-Hans/categories.json` as `"category.<id>"`. `Tools/Localization/gather.py` copies them into
  `Resources/Localization/zh-Hans.json`. Prompt bodies stay English, like skills and agent files: they are written for the
  agent, and agents answer in the user's language anyway.

## A preset's fields

| Field | What it holds |
| --- | --- |
| `id` | `<category>.<lower-case-words>`, unique in the library. |
| `title` | What the picker lists, 60 characters at most. |
| `summary` | One line, 160 characters at most: what the user gets. |
| `packs` | 1 to 3 catalog pack ids (`Tools/Catalog/catalog_source.json`), the main one first. |
| `skills` | At most 2 skill names from `Resources/Skills` or `Resources/AgentTeam/skills`. |
| `agent` | Optional: a team agent (`Resources/AgentTeam/agents`) or `team`. A `team` preset switches the Team toggle on. |
| `tools` | The `hyper_*` tools the prompt names. Each belongs to one of the preset's packs or to a foundation pack (`shared_foundation`, `context_scene_project_search`, `diagnostics`, `pie_runtime`), and the prompt names each one. |
| `ops` | Optional plan op kinds the prompt asks for; each must appear as `TEXT("<kind>")` in the toolsets' source. |
| `difficulty` | `starter`, `standard` or `advanced`. |
| `tags` | Free words that also feed search. |
| `context` | Optional `{"selection": "actors"\|"assets"\|"none", "classes": ["Landscape"], "min": 1}`: what the preset works on. A preset that fits what is selected is listed under *For your selection* and ranks first among equals. |
| `placeholders` | `[{"name": "biome", "label": "Biome", "default": "temperate forest", "hint": "..."}]`: the blanks, each used in the prompt as `{{biome}}`. |
| `prompt` | The English request, 80 to 4000 characters. |

## Blanks and the selection

`{{name}}` is a blank: a lower-case name in double braces. Anything else in braces, such as a JSON plan
`{"Kind": "landscape.create"}`, stays as written, so prompts may show plans. A blank fills from what the user typed in
the picker, else from the editor for these context tokens, else from the placeholder's default; a blank with nothing
becomes `[Label]` in the composer, and the first one is selected for the user to type over.

| Token | Fills with |
| --- | --- |
| `{{selected_actor}}`, `{{selected_actors}}` | The first selected actor, or all of them one per line, as `Label (path)`. |
| `{{selected_asset}}`, `{{selected_assets}}` | The first selected Content Browser asset's object path, or all of them. |
| `{{level}}` | The current map's package, such as `/Game/Maps/Arena`. |
| `{{content_folder}}` | The Content Browser's current folder, else the first selected asset's, else `/Game/HyperAI`. |
| `{{project}}` | The project's name. |

Use a token rather than "the selected thing", and a placeholder with a sensible default for every part that varies.

## What makes a preset give the right result

1. **States the outcome as checkable facts**: exact asset paths under `{{content_folder}}`, exact names and labels,
   counts, and sizes in the units each tool documents: cm for locations and for the blockout, modelling and spatial
   tools (`Size`, `Extent`, `Clearance`), m for the landscape, foliage, PCG and World Partition sizes the level-design
   skill names (`Patch`, `Spacing`, `RoadMargin`, a loaded region's `Size`).
2. **Inspects before it edits**, naming the tool (`hyper_context_snapshot`, `hyper_level_design_inspect` for player
   metrics, `hyper_pcg_scatter_inspect`, ...) and recalling project memory.
3. **Names the plan tool and its op kinds**, one plan per step with a fresh OperationId, then `hyper_operation_wait`;
   `bDryRun: true` first only when the plan deletes or replaces things or is large.
4. **Ends with proof**: the pack's `*_validate` tool, plus runtime or visual evidence (`hyper_capture_view`,
   `hyper_image_compare`, `hyper_blueprint_compile_validate`, `hyper_verify_run` or `hyper_playtest_run`).
5. **Asks for a PASS/FAIL table** and pictures on their own line, as the Replies section of AGENTS.md asks.
6. **Scopes its edits**: only these assets, leave the rest alone, save with `bSave` only when asked, and honour Sandbox
   Mode and approvals.
7. **Puts every variable part in a placeholder** with a sensible default, and uses the selection tokens.
8. **Runs 80 to 250 words as numbered steps**, never says "make it look good" without a measurable bar, and for an
   optional pack says "if the X pack is off, say so and stop".
9. **Reads like the benchmark tasks** (`Tools/Benchmark/tasks/01-niagara.md`), which scored 10 of 10.

An example, abbreviated:

```json
{"id": "pcg-foliage.forest-ecosystem", "title": "Grow a layered pine forest on a paint layer",
 "summary": "Build a PCG ecosystem of pines, bush clumps, rocks and ferns on one landscape layer, generate it, and prove density, stands and culling.",
 "packs": ["pcg", "worldbuilding_navigation", "lighting_lookdev"], "skills": ["unreal-level-design"], "agent": "level-designer",
 "tools": ["hyper_landscape_inspect", "hyper_pcg_scatter_inspect", "hyper_pcg_scatter_apply_plan", "hyper_operation_wait", "hyper_capture_view"],
 "ops": ["ecosystem.create", "volume.create"],
 "difficulty": "standard", "context": {"selection": "actors", "classes": ["Landscape"], "min": 1},
 "placeholders": [{"name": "layer", "label": "Paint layer", "default": "Grass"}, {"name": "tree_density", "label": "Pine density (per m2)", "default": "0.02"}],
 "prompt": "On {{selected_actor}} in {{level}}, grow a pine forest on the `{{layer}}` paint layer with PCG. If the PCG pack is off, say so and stop.\n1. hyper_landscape_inspect: confirm `{{layer}}` exists, report its share, and note road layers. hyper_pcg_scatter_inspect with no Graph for existing volumes.\n2. One hyper_pcg_scatter_apply_plan (fresh OperationId, bDryRun false): `ecosystem.create` Graph {{content_folder}}/PCG_PineForest, AvoidLayers the road layers, one biome `Forest` on `{{layer}}`, species big to small: Pines (Density {{tree_density}}, Footprint 4, ...), ...; then `volume.create` Volume `PineForest`, Cover the landscape.\n3. hyper_operation_wait on the OperationId for per-mesh counts.\n4. hyper_capture_view one aerial oblique and one shot from 1.7 m up inside the forest. Report a PASS/FAIL table: all species have instances, each has a cull distance, and the pictures show stands and clearings. Show each picture on its own line."}
```

## A project's own presets

A project keeps its own presets in `.hyperai/prompts/*.json`, in the same format, where the team can share them through
source control. A preset with a built-in's `id` replaces it in place; `{"id": "<id>", "hidden": true}` takes a built-in
out of the picker; a file whose `category` is new adds that category, titled by the file's optional `title`. Project
presets show as written, untranslated. A broken project file is reported and skipped; the built-in presets always load.

## Checking

Run these after any change; each exits 1 on a problem:

```
python Tools/Prompts/check_prompts.py check
python Tools/Localization/gather.py check
python Tools/Catalog/build_catalog.py check
```

`check_prompts.py stats` lists the presets per category and difficulty, and the catalog tools no preset uses yet. After
adding or changing presets, run `python Tools/Localization/gather.py` once to write their strings and translations into
`Resources/Localization/zh-Hans.json`. In the editor, the `HyperAIStudio.PromptPresets` automation tests check the same
rules against the catalog compiled into the plugin, and that each preset fills, searches and translates.
