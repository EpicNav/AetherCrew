---
name: project-search
description: Find assets and code in an Unreal project through HyperAIStudio's project search, by name or by meaning ("the stone wall material with moss"), and read the exact references a user's @-mentions send. Use when asked to find, locate or list assets, materials, meshes, textures, Blueprints or C++ symbols, when a description rather than a name is given, or when a message ends with "Referenced with @ (exact references)".
---

# Project search

`hyper_project_search` (toolset `HyperAIStudio.HyperAIStudioProjectSearchToolset`) searches the project's on-disk
assets and C++ symbols. `hyper_project_index_status` (`HyperAIStudio.HyperAIStudioProjectIndexStatusToolset`) says how
much is indexed. Both read only, and run on this computer: nothing is sent anywhere.

## Pick the mode

- A name, or part of one (`SM_Rock`, `BP_Door`): leave `Mode` empty or use `literal`. Every word must be in the
  asset's name, class or folder.
- A description (`the stone wall material with moss`, `mossy rock`, `rusty metal crate`): `semantic`. Assets come best
  first, ranked by the words in their names, tags, classes and folders, by a built-in vocabulary of what words mean
  (stone and cobble, moss and lichen and overgrown, `MI_` for a material instance), and by spelling, so `barel` finds
  `SM_Barrel_Wood`.
- Both: `hybrid` lists literal matches first, then the ranked ones. Empty (`auto`) ranks a query of three or more
  words, or one nothing is named after.

Use `"kinds": ["asset"]` for assets alone. Ranked records carry `match` and `score`:

- `name`: every word is in the name, class or folder; trust it.
- `meaning`: found through the vocabulary or stems; check it is what was asked before using it.
- `spelling`: a near spelling only.
- `words`: Search By Meaning is off, so only the words counted.

## Check the index first when results look thin

`hyper_project_index_status`: `AssetIndexStatus` (`ready`, `indexing`, `partial`), `AssetRecordCount`,
`bPausedForPlayInEditor` (new assets wait until Play In Editor stops), `IndexBytes`, `Storage` (what it holds: editor
memory only) and `EpicSemanticSearch`. Epic's SemanticSearch plugin, when enabled, embeds through a web API; HyperAI's
search never calls it.

## @-mentions in the user's message

A message from HyperAI's chat may end with:

```
Referenced with @ (exact references):
- @SM_Rock: asset /Game/Env/SM_Rock.SM_Rock (StaticMesh)
- @Torch_01: level actor "Torch 01" /Game/Maps/Keep.Keep:PersistentLevel.Torch_01 (PointLight)
- @README.md: file E:/Project/README.md
- @image-match: skill E:/Project/.agents/skills/image-match/SKILL.md
- @hyper_scene_apply_plan op-1234: HyperAI operation op-1234abcd (Completed)
```

Each `@Name` in the text means that line's reference, exactly: use the object path, actor path or file path as given,
and do not search for it again. A skill reference is a SKILL.md to read and follow; an operation id goes to
`hyper_operation_status`.
