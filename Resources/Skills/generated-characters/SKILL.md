---
name: generated-characters
description: Turn a prompt or a picture into a game character in Unreal with HyperAI's paid 3D services - a textured model (Tripo, Meshy, Hi3D), rigged as a skeletal mesh with its skeleton and physics asset (Meshy, Tripo), animated from the providers' preset libraries or from words (Meshy text to motion, Viggle), retextured up to 8K, and retargeted onto the project's own skeletons such as the UE5 Mannequin with IK Rigs and a retargeter. Also animates the project's own Mannequin from a sentence. Use when the user asks for a generated, rigged or animated character, text-to-animation, auto-rigging, or new textures on a model; not for props modelled for free (unreal-modeling) or hand-made animation assets (unreal-animation).
---

# Generated characters: prompt to rig to animation to your Mannequin

Every call here is paid and leaves the editor. Each tool dry-runs by default (`bDryRun` true): it reports the provider
tasks, their price and where the assets land, and spends nothing. Sent with `bDryRun: false`, it waits for the user's
approval in Activity under the session's spend cap, then runs as a background job: follow its `OperationId` with
`hyper_operation_wait` (`bWaitThroughApproval: true`, `TimeoutMs` 120000, repeat until terminal) or `hyper_mesh_status`.
A job takes one to ten minutes.

| step | tool | providers |
|---|---|---|
| a textured model from words or a picture | `hyper_mesh_generate` | Tripo, Meshy (words or picture), Hi3D (picture) |
| rig it: skeletal mesh, skeleton, physics asset | `hyper_mesh_rig` | Meshy (adds a walk and a run), Tripo |
| find preset animations | `hyper_mesh_actions` (free, offline) | Meshy's library, Tripo's presets |
| animate: presets or a clip from words | `hyper_mesh_animate` | Meshy, Tripo (presets only), Viggle (words only) |
| a new texture, up to 8K | `hyper_mesh_retexture` | Meshy (words or a picture), Tripo (words) |
| stop a job | `hyper_mesh_cancel` | |
| IK Rigs, retargeter, copies on your skeleton | handed to `hyper_retarget_apply_plan` | animation_rigging pack |

## Prompt to a rigged, animated character on the Mannequin

1. `hyper_mesh_generate` with a prompt that asks for what riggers need: "full-body humanoid character, standing in a
   T-pose or A-pose, arms away from the body, facing forward, game-ready, PBR textures". Keep the operation id.
2. `hyper_mesh_rig` with `SourceOperationId` (the provider works from its own task), `Name` such as `SK_Knight`, and
   `RetargetTo` the project's Mannequin skeletal mesh (for example `/Game/Characters/Mannequins/Meshes/SKM_Manny`).
   Meshy needs a textured humanoid facing +Z under 300,000 faces; Tripo rigs with Mixamo's bone names by default
   (`BoneNaming: mixamo`), which Epic's retargeting recognises. The job imports `SkeletalMesh`, `Skeleton` and
   `PhysicsAsset`, then hands the retarget plan on: `RetargetOperationId` waits for its own approval.
3. `hyper_mesh_actions` with `Search` (`walk`, `attack`, `dance`) and the rig's provider, then `hyper_mesh_animate`
   with `RigOperationId` and `Actions` by id or name (Meshy up to 10, Tripo up to 20), and `RetargetTo` again. The
   clips land as AnimSequences on the rig's skeleton in `Folder/Name`, and the retarget copies them onto the Mannequin
   under `Folder/Name/<Mannequin name>`.
4. `hyper_operation_wait` on each `RetargetOperationId`, then `hyper_retarget_inspect` the IK Rigs and retargeter it
   made (in a `Retarget` folder beside the rig, reused by later jobs). If the rig's chains were not named
   automatically (an unknown skeleton), fix them with `ik_rig.add_chain` / `retargeter.map_chain` as the
   unreal-animation skill shows, then retarget again with `retarget.animations`.

## Animate the project's own Mannequin from words

- **Viggle** (`Provider: viggle`, `TargetMesh` the Mannequin, `Prompt`, `DurationSeconds` 3 to 60): its MetaHuman
  body clip uses the UE5 Mannequin's bone names, so it is imported straight onto the Mannequin's skeleton, with no rig
  and no retarget. A Mixamo-named skeleton gets its Mixamo clip instead. $0.10 a clip.
- **Meshy** (`Provider: meshy`, `TargetMesh`, `Prompt`, `DurationSeconds` 2 to 10 in half seconds, `Mode` prime or
  swift): Meshy puts a clip only on its own rig, so the job exports the Mannequin as a GLB, rigs it, makes the clip, puts
  it on the rig, and retargets it back onto the Mannequin. Given `RigOperationId` of a Meshy rig instead, it skips the
  rigging. About $0.36 with the rig, $0.26 without.
- Tripo makes no animation from words.

## Retexture

`hyper_mesh_retexture` with a source (`SourceOperationId`, `SourceTaskId` or `MeshPath`), `Prompt` (or `ImagePath`
with Meshy) and `Resolution`: Meshy `2k`, `4k` or `8k`; Tripo `standard`, `detailed` or `8k`. `bKeepUVs` keeps a
project mesh's own UVs (Meshy). The model comes back with its material instances and textures in `Folder/Name`; assign
the material instance to your own mesh when you kept its UVs.

## Rules that save money and time

- Dry-run first and read `Tasks` and `EstimatedCost`; tell the user the price before sending for real.
- A provider works from its own model only when it made it (a `hyper_mesh_generate` job, same provider). Anything else
  (a Hi3D model, a project mesh, a model from the other provider) is exported as a GLB and sent: Meshy inline (100 MB),
  Tripo by upload (60 MB). The glTF Exporter plugin writes the GLB.
- `hyper_mesh_actions` knows Tripo's 117 presets and Meshy's first 40 actions offline; any other Meshy name is looked up
  in its library when the job runs (free), and the whole library is kept for the next search.
- Tripo's presets depend on its rig model: `v1.0-20240301` (the default, humanoids, `preset:biped:<name>`) or
  `v2.5-20260210` (other creatures too, 16 presets). Pass `walk` or the full id.
- `hyper_mesh_cancel` stops a job: before approval nothing is spent; after, a task the provider already took runs on
  there and stays booked (neither Meshy nor Tripo stops a running task).
- A failed job says why in the provider's words (`out of credits`, `Pose estimation failed`); the provider refunds a
  failed task. Files and a `job.json` are kept in `Saved/HyperAIStudio/Models/<operation id>`.
- Prices are in Settings, 3D Generation, Mesh Task Prices; a change there ends auto-approval.
