---
name: tripo-model-creator
description: Make game-ready 3D models with Tripo or Hi3D inside Unreal and fix them as part of making them - hyper_model_create picks the provider (Hi3D for the densest, most faithful models from a picture), makes a reference picture first from words, classifies a brief into H3.1 (high detail) or P2 (Smart Poly) or Hi3D's model and resolution, a face budget, quads or triangles and a texture tier, generates, checks the model in the editor (budget, holes, winding, stray bits, size, pivot, UVs, texel density, PBR maps, Nanite, collision, a visual compare) and fixes it, free fixes first and paid Tripo fixes priced and approved; and hyper_model_check_and_fix for any mesh, including models sent from Tripo Studio by Tripo's DCC Bridge. Use when the user wants a generated prop, environment piece, vehicle, weapon or character that must be game-ready, or wants a Tripo model checked or repaired; for rigging and animation follow generated-characters, for props modelled for free follow unreal-modeling.
---

# Tripo model creator: a brief to a checked, fixed, textured model

Two routes, and they use different credits:

- **API route** (`hyper_model_create`): Tripo's API, paid in **API credits** ($0.01 each) with the key from Settings >
  Agents > Keys (or `TRIPO_API_KEY`). Every paid step is priced first and waits for the user's approval (or their
  auto-approval) under the session's spend cap.
- **Tripo Studio route**: the user makes the model on tripo3d.ai with their Studio plan's own credits and presses
  *Send to Unreal* in Tripo's DCC Bridge plugin. HyperAI notices the import and offers *Check and fix* (free fixes) in
  a notification and in Activity. Studio credits cannot be spent through the API, and HyperAI never drives the Studio
  website or asks for a Tripo login. To move credits between the two, the user asks Tripo (support@tripo3d.ai).

## Tripo or Hi3D

`Brief.Provider` is `auto` (the default), `tripo` or `hi3d`. Hi3D (Hitem3D: Sparc3D and Ultra3D) works only from a
picture or up to four views, at 100,000 to 2,000,000 faces (5,000,000 at `2048master`). Auto chooses:

- **Hi3D** for a picture of a hero prop, environment piece or vehicle (Nanite keeps the dense mesh), a portrait or bust
  (portrait models), or a brief asking for photoreal, intricate, max detail or sculpted - and only when Hi3D's key is
  set (otherwise Tripo, saying why).
- **Tripo** for words alone, a face budget under 100,000, quads, parts, low poly, geometry quality, a rig, view edits or
  a multiview task, mobile or VR, characters and creatures.

Hi3D's plan: `Model` `hi3dv3.0` (default), `hitem3dv2.1`, `hitem3dv2.0`, `hitem3dv1.5`, or `scene-portraitv2.1` (busts);
`Resolution` `2048quality` (default) or `2048master` (an extreme or master brief). Under 100,000 faces it is made at
100,000 and reduced in the editor for free (`bLocalReduce`). `provider: hi3d` with words alone makes a reference
picture first (Tripo's text-to-image, or HyperAI's image generation without a Tripo key). Credits at $0.02: v3.0 45 or
145, v2.1 25/45, v2.0 35/45, v1.5 15-40, portraits 25-45; texture only 10 (15 with PBR, inferred).

The key: on Hi3D's developer platform make an API key (AK and SK); store `client_id:client_secret` as one value in
Settings > Agents > Keys under Hi3D (Hitem3D), or `HITEM3D_API_KEY` / `HYPERAI_HI3D_KEY`. HyperAI trades it for an
access token sent to Hi3D only; neither is ever shown or logged. `hyper_tripo_account` shows Hi3D's balance too
(`Hi3DStatus`, `Hi3DBalance`).

## A picture first (words alone)

`ReferenceImage` auto turns it on for words (not background or miniature pieces on H3.1): Tripo's text-to-image draws
the object, then image-to-model makes it from that picture's task, and the picture becomes the visual check's
reference. `ImageModel` (names as people say them work: "gpt-image-2.5-sunburn" is `chat_image_2.5_sunburst`; default
`chat_image_2.5_flare`), `ImageQuality` (high by default), `ImageSize` (1024x1024, 2048x2048 for hero pieces),
`ImageBackground` (transparent), `ImageTemplate` (asset_extraction for props, t_pose for characters).
`ReviewReference` (auto: on for hero props, characters, vehicles, weapons) approves the picture first; the job then
waits in `awaiting_reference_review` with the picture in `PreviewImages`, and `CurrentOperationId` is the 3D
generation's approval. Show the user the picture; turning the generation down ends the job (`fix_declined`).

## The job

1. **Dry-run first** (`bDryRun` true, the default): `hyper_model_create` with `Brief` (`Prompt`, or `ImagePath`, or
   `MultiviewImages` front/left/back/right, and `Use`, `Platform`, `SizeMeters`). Read `Plan`: the family, model id,
   `FaceLimit`, `bQuad`, `TextureQuality`, `bPbr`, `bParts`, `bSmartLowPoly`, `Reasons` (one line a choice),
   `PriceLines`, `Credits` and `EstimatedCost`. Tell the user the plan and the price.
2. Override anything the user asked for (`Model` `H3.1`/`P2`, `FaceLimit`, `Topology` `tris`/`quads`,
   `TextureQuality`, `Pbr`, `Parts`, `SmartLowPoly`, `GeometryQuality`, `Seed`), then send with `bDryRun: false`,
   `FixBudgetUSD` (what paid fixes may spend after the generation) and `MaxRounds`. `bLocalFixesOnly` never offers a
   paid fix. `bRig` with `RetargetTo` the project's Mannequin rigs it with spec `unreal` once checks pass (25 credits,
   its own approval).
3. Follow `OperationId` with `hyper_operation_wait` (`bWaitThroughApproval: true`), then `hyper_model_status`.
   `CurrentOperationId` names whatever waits now: the generation, or a paid fix the job offers (its step shows the fix,
   its credits and the checks it clears). Wait on it the same way. The job ends `completed` (every check passes) or
   `stopped` with `StopReason`: `budget_used`, `rounds_used`, `fix_declined`, `no_paid_fix`, `local_only`.
4. Report from the status: `AssetPath`, `Materials`, `PreviewImages`, `FirstCheck` and `LastCheck`, and `Steps` (each
   with its price and Tripo's `CreditsConsumed`). The journal (`Journal`) keeps every step; `ResumeOperationId`
   continues a job the editor restarted under, asking Tripo after a task it already took instead of paying again.

## H3.1 or P2

| | H3.1 (`v3.1-20260211`) | P2 (`P2-20260801`) |
|---|---|---|
| for | detail: hero props, organic environment pieces, large hero vehicles, anything Nanite renders | clean, light topology: props, kit pieces, characters, weapons, foliage, background |
| triangles | 500 - 2,000,000 (over 1.5M needs `GeometryQuality` detailed) | 500 - 50,000 |
| quads | 500 - 50,000 (FBX) | 500 - 25,000 |
| only here | parts, smart low poly, Ultra geometry | texture alignment and image autofix as on H3.1 |
| price (credits) | words 10/20/30/40, pictures 20/30/40/50 by texture tier (none, standard/fast, detailed, extreme 8K); detailed geometry +20, quads +5, smart low poly +10, parts +20 | 100/110/120/130 |

Face budgets by use (PC; mobile quarters them and caps textures at standard, VR halves them):

| use | family | budget | textures | size |
|---|---|---|---|---|
| hero_prop | H3.1 | 300K tris, Nanite | detailed | 1 m |
| prop | P2 | 20K tris | standard | 0.8 m |
| environment (rock, cliff, trunk) | H3.1 | 200K tris, Nanite | detailed | 3 m |
| kit (walls, floors, pillars) | P2 | 30K tris, complex collision | standard | 4 m |
| background | P2 | 5K tris, no collision | standard | 5 m |
| character / creature | P2 | 20K / 25K **quads** | detailed | 1.8 / 1.5 m |
| vehicle | H3.1 | 150K tris, Nanite | detailed | 4.5 m |
| weapon | P2 | 15K tris | detailed | 1 m |
| foliage | P2 | 8K tris, Nanite | standard | 2 m |
| miniature (coin, gem, potion) | P2 | 2K tris | standard | 0.3 m |

Quads when the model deforms (characters, creatures) or will be subdivided; triangles for everything that only
renders. Words in the brief move the choice: "photoreal", "intricate", "cinematic" go up to H3.1 and a texture tier up;
"low poly" goes to P2 (or smart low poly on H3.1); "separate parts" turns parts on (H3.1, untextured, then a texture task).

## Prompting Tripo

- One object, its shape, material, style and scale in a sentence: "a weathered oak treasure chest with iron bands and
  a rusted lock, stylised, game prop". Not "a box".
- Name materials for good PBR: oak, brushed steel, wet stone, leather.
- For characters: "full-body, A-pose, arms away from the body, facing forward" so the rig works.
- Negative prompt (255 characters): the creator sends one for words by default (floating parts, holes, ground plane,
  baked lighting, text). Add what the reference keeps getting wrong.
- A picture: one object, clean background, no heavy shadows, at least 256 px. Several views (front required) give the
  best shape for anything asymmetric.
- A new seed (`Seed`) changes the shape; the same seed and input reproduce it.

## The fix ladder (cheapest correct fix first)

Free, in the editor, run every round before anything paid: stray bits removed, faces wound outward, small holes capped,
normals recomputed (hard edges kept), zero-area faces removed, UVs unwrapped (only when there is no texture to lose),
scaled to `SizeMeters`, stood upright (characters, creatures, foliage), pivot to the bottom centre, collision (box, or
complex for kit pieces), lightmap UVs, Nanite, and the materials: base colour sRGB, normal maps compressed as normal
maps, roughness/metal/occlusion packed into one linear ORM, a material instance of HyperAI's master
(`/Game/HyperAI/Materials/M_HyperAIGeneratedMaster`: tiling, a detail normal, ORM) on every slot, one slot a part for a
parts model.

Paid, each priced and approved, one a round, within the fix budget:

| fails | fix | credits |
|---|---|---|
| texture or PBR maps missing, weak, malformed | `/models/texture` (a tier up on a second try; `part_names` for one part) | 10 / 20 / 30 |
| overlapping or missing UVs on a textured model (under 80K tris) | `/mesh/uv` Smart UV, re-baked | 20 |
| over the face budget, wrong topology, non-manifold, holes | `/mesh/decimate` v3.0 retopology to the budget, `smart_uv` included | 35 (40 quads) |
| holes or a broken part retopology left | `/mesh/segment` then `/mesh/complete` quick cap, then AI completion | 70, then 90 |
| a skeletal mesh's pivot or scale | `/models/convert` with `pivot_to_center_bottom`, `scale_factor` | 10 |
| the shape is wrong (the visual compare against the reference fails) | a new generation: new seed (from the same reference picture), refined prompt, image autofix | the generation's price |
| Hi3D model: textures missing or weak | Hi3D texture only on the current mesh (v3.0 or v1.5) | 15 Hi3D credits |
| Hi3D model: the shape is wrong | Hi3D again, a resolution up (or from the views) | that resolution's price |
| Hi3D failed a task with 50010001 | the same task again (Hi3D refunded it) | the same price, approved again |

Over the face budget always goes to retopology first: nothing cheaper fixes it (for Hi3D's floor, the free reduction
runs first). A fix Tripo fails is refunded by Tripo, and the ladder climbs past it. A texture retry changes only its
seed first, then goes a tier up. A rig runs Tripo's free rig check first: a biped gets v3.0 with spec unreal, another
body v2.5 as its rig_type, and a model it cannot rig stops there. Rate-limited or busy answers (429, 5xx) are sent
again after a back-off.

## Judging the result

- `LastCheck.bPassed` means nothing failed; warnings are judgement calls (a model under its least faces, several large
  parts, a translucent material on Nanite, no lightmap UVs under Lumen).
- `visual`: the editor's capture against the reference picture (55 or more passes; under 40 means the shape is wrong).
  Against Tripo's own render it only warns: a low score there means the import went wrong (textures, orientation).
- Look at the capture and Tripo's render in `PreviewImages` and `LastCheck.Images` before calling it done.
- `texel_density` under half the use's target means a blurry model up close: a higher texture tier fixes it.

## Models from Tripo Studio

- The user installs Tripo's DCC Bridge for Unreal themselves (Tripo Studio > DCC Bridge > Unreal; copy it into the
  project's `Plugins` folder, matching their engine version, then Window > Tripo Bridge).
- HyperAI watches the import folders in Editor Preferences > Plugins > HyperAIStudio Tripo (`TripoBridgeFolders`; Tripo
  does not document where its bridge imports, so set it once you see where models land) and meshes whose name or source
  file contains `tripo`. Each one is offered once as *Check and fix*.
- `hyper_model_check_and_fix` with `AssetPath` does the same on any static or skeletal mesh: `bCheckOnly` (the default)
  reports and lists `LocalFixes` and `PaidFixes`; `bCheckOnly: false` asks once for the free fixes; `bAllowPaidFixes`
  also offers paid ones (API credits, each approved). Skeletal meshes are reported and get their materials; their
  geometry is not reshaped in the editor.
- `hyper_tripo_account` reads the API balance and recent usage (the Settings page shows it too). It never shows the key,
  and it cannot see Studio credits.
