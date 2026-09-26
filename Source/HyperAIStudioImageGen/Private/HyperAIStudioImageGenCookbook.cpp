// Games by Hyper 2026.

#include "HyperAIStudioImageGenCookbook.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::ImageGen
{
	namespace
	{
		// Split into literals under the compiler's single-literal size limit.
		const TCHAR* const Sections[] = {
TEXT(R"CB(# Image Generation Cookbook

Written by HyperAIStudio for agents generating reference images with `hyper_image_inspect` and
`hyper_image_generate`. Read this before the first generation in a session. Built in are Google's Nano Banana
models, which need a Gemini API key in `GEMINI_API_KEY`, and Codex drawing on the user's own plan. Section 2
says which to use, and section 3 how to add any other model.

## 1. What this pack is for

A reference image is a **look target**: the light, the palette, the mood, the weather. It exists so the
level can be measured against something, not so it can be copied.

It is **not a plan**. A generated image has no scale, no geometry, no metres, and no idea what a
playable space needs. Nothing in it tells you a doorway is 2.2 m. Route layout through
`hyper_level_design_*`, which measures the level itself, and use the image only for the look.

The loop this pack is built for:

1. `hyper_image_generate` writes a reference PNG into the project.
2. Open the Match page of HyperAI Studio and point it at that PNG, beside a viewport capture.
3. `hyper_image_compare` scores the two, names where they differ, and keeps each iteration in a session.
   `hyper_lighting_compare` gives the lighting-only numbers.
4. Adjust, capture again, compare again, until the score reaches the target. The image-match skill
   (`/hyperai-match` in Claude Code) runs this loop for scenes, materials, effects and UI.

Step 3 is why the prompt template pins time of day, key light direction, contrast and colour
temperature. A reference whose light is beautiful but unreachable makes step 4 grind forever.

No reference at all? Section 8 makes one from the level's own view, relit as the user asks.

## 2. First: can you already generate images yourself?

Check before configuring anything here. Some agents that drive this plugin carry their own image
generation - Codex has an `imagegen` skill, for instance - and when yours does, **use it**. It costs
nothing beyond the subscription already running the session, there is no key to set, no endpoint to
configure, and no model name to go stale.

This pack does not care who made an image. `hyper_image_inspect`, the Match page and
`hyper_lighting_compare` all take a path. So generate the image your own way and drop it in:

    Content/References/Generated/<batch>/<batch>_01.png

Write a sidecar beside it, same name with a `.json` extension, and `hyper_image_inspect` will list it
alongside anything this pack generated. Only `image` is required; the rest is what makes the reference
re-generatable in six months, which a chat-produced image otherwise is not:

    {
      "image": "<batch>_01.png",
      "batch_id": "<batch>",
      "prompt": "the full prompt you used",
      "model_id": "codex_imagegen",
      "width": 1024,
      "height": 1024,
      "seed": -1,
      "generated_utc": "2026-09-20T23:40:00Z"
    }

Section 5 still applies: the prompt is the part that decides whether the level can ever match the
reference, so compose it from the template even when you are generating the image yourself.

**No generator of your own?** Claude Code has none, for one. Then ask for `model_id: codex_image_gen`:
HyperAI has Codex draw the picture, headless, on the user's own Codex plan, and puts the PNG and its
sidecar in the project like any batch. Nothing to configure; `hyper_image_inspect` shows whether this
machine can (Codex CLI installed). It still waits for the user's approval, because it spends their plan.
Then open the PNG it names with your own image reader and look at it. `gemini_image_gen` works the same
way, but only with Gemini's nanobanana extension installed, since Gemini CLI has no generator of its own.

**Nano Banana, through Google's API.** When `GEMINI_API_KEY` holds a Gemini API key, these draw directly,
with no agent in between. Each costs money and waits for the user's approval; `hyper_image_inspect` shows each
one's price and whether the key is set.

| model_id | Model | Use it for | Per image |
|---|---|---|---|
| `nano_banana_2` | Gemini 3.1 Flash Image | most references: fast, good, any size to 4K | $0.067 (1K) to $0.151 (4K) |
| `nano_banana_pro` | Gemini 3 Pro Image | the most detailed and accurate, and legible text | $0.134 (1K, 2K), $0.24 (4K) |
| `nano_banana_2_lite` | Gemini 3.1 Flash Lite Image | quick, cheap drafts at 1K | $0.0336 |
| `nano_banana` | Gemini 2.5 Flash Image | the original, older model | $0.039 |

Width and height become the nearest aspect ratio these take (1:1, 2:3, 3:2, 3:4, 4:3, 4:5, 5:4, 9:16, 16:9 or
21:9) and a size of 1K, 2K or 4K by the longer side. A negative prompt is added to the prompt as "Avoid: ...".

Configure a model below only when you need something none of these do: a specific checkpoint or
style, a fixed seed you can return to, or a model running offline.

## 3. Configure a model

Run `hyper_image_inspect`. If it reports no models, nothing can generate yet.

A model is two settings entries, both under **Project Settings > Plugins > HyperAI Studio**:

- **External Services** - one entry with the endpoint's base url, the *name of the environment
  variable* holding the API key (never the key itself), the cost per call, and a timeout.
- **Image Models** - one entry naming that service, the path, a request template, and where the image
  sits in the reply.

Besides the built-in ones, this plugin ships neither. Image model names change faster than plugin releases, so
even the built-in models are plain data: an Image Models entry with a built-in id, such as `nano_banana_pro`,
replaces it. That is how to follow Google renaming a model, or to send it through a proxy by adding a
`google_gemini` service of your own.

### The request template

`request_template` is the JSON body, with these substituted in before sending:

| Slot | Becomes |
|---|---|
| `{prompt}` | the composed prompt |
| `{negative}` | the negative prompt, or empty |
| `{model}` | the entry's `model` |
| `{width}` `{height}` | the requested size |
| `{seed}` | the seed, or `-1` |
| `{aspect_ratio}` | the nearest of 1:1, 2:3, 3:2, 3:4, 4:3, 4:5, 5:4, 9:16, 16:9 and 21:9 |
| `{image_size}` | `1K`, `2K` or `4K`, from the longer side |

Values are JSON-escaped as they are substituted, so a prompt containing quotes, backslashes or
newlines cannot break the body. Put `{prompt}` inside the quotes of a string field, not around them.
A template without `{negative}` still gets the negative prompt, at the end of the prompt as "Avoid: ...".
`path` takes `{model}` too.

### The response path

`response_image_path` is a dotted path to **base64 image data** in the reply. Numbers index arrays:
`data.0.b64_json` means "the `b64_json` field of the first element of `data`". A `*` takes the first element
that has the rest of the path, for a reply that puts text before the picture, as Gemini's may.

**The reply must carry the image as base64, not a link.** Most endpoints return a URL unless asked
otherwise, and a URL is refused here rather than fetched: downloading it would mean a second request
to a host the plugin has no key, cost or timeout for. Ask for base64 in the request template.
)CB"),
TEXT(R"CB(
### Three shapes that cover most endpoints

An OpenAI-shaped endpoint - note `response_format`, without which the reply is a link:

    path:                 /v1/images/generations
    request_template:     {"model": "{model}", "prompt": "{prompt}", "size": "{width}x{height}",
                           "n": 1, "response_format": "b64_json"}
    response_image_path:  data.0.b64_json

A Gemini-shaped endpoint, as the built-in Nano Banana models are:

    path:                 /v1beta/models/{model}:generateContent
    request_template:     {"contents": [{"parts": [{"text": "{prompt}"}]}],
                           "generationConfig": {"responseModalities": ["IMAGE"],
                             "imageConfig": {"aspectRatio": "{aspect_ratio}", "imageSize": "{image_size}"}}}
    response_image_path:  candidates.0.content.parts.*.inlineData.data

An Automatic1111 or a compatible local server:

    path:                 /sdapi/v1/txt2img
    request_template:     {"prompt": "{prompt}", "negative_prompt": "{negative}",
                           "width": {width}, "height": {height}, "seed": {seed}, "steps": 30}
    response_image_path:  images.0

If a model does not fit any of these, it fits this pack as long as it takes one JSON POST and returns
base64. An endpoint that queues work and needs polling - a ComfyUI graph, for instance - does not, and
is not supported here.

## 4. A local model shares your graphics card

Tick `runs_on_this_machine` for anything on localhost, and `hyper_image_generate` will say so before
starting.

This matters. A model large enough to hold a card, running beside a drawing editor, does not simply
run slower: the two evict each other's memory, and a job that takes seconds alone can take minutes. It
was measured on this project, with a speech model, at over two hundred times slower. A hosted endpoint
has none of this problem - it is a network call and your card is idle.

If a local generation crawls, that is the cause. Close the editor, or move the model to another
machine, or use a hosted one.

## 5. Prompts

`hyper_image_generate` composes the prompt in this order:

1. The template named by `template_id`, with each `{slot}` replaced by the matching entry in `slots`.
2. Any `prompt` text, appended after it.

Unfilled slots drop out cleanly along with the sentence fragment around them, so a template is usable
even when you only know half of it. With no `template_id`, `prompt` is sent exactly as written.

The seeded `scene_reference` template takes these slots:

| Slot | What to write | Example |
|---|---|---|
| `subject` | the place, concretely | `a flooded marble subway platform` |
| `time_of_day` | | `an hour before sunrise` |
| `weather` | | `still air, thin mist` |
| `key_light` | **direction and source** | `low from the left, from a single sodium lamp` |
| `contrast` | | `high` |
| `palette` | | `amber and deep teal` |
| `mood` | | `abandoned, patient` |
| `lens` | | `35mm, shallow depth of field` |

`key_light`, `contrast` and `palette` are the ones `hyper_lighting_compare` measures. Vague answers
there produce a reference the level can never match, and no amount of adjusting will close the gap.

Templates are editable in settings. Change the seeded one rather than working around it.

## 6. Cost, approval and the spend cap

Generating spends real money, so it is an external effect: **every generation waits for you to click
Approve** in HyperAI Chat, Activity panel. The approval shows the exact prompt, the model, the image
count and the estimated cost.

Two more bounds: a per-session spend cap in settings, refused before anything is sent; and a dry run
that costs nothing. Dry-run first - the report carries the fully composed prompt, so you can read and
fix it for free.

## 7. Output

Each image lands in the project as a **file, not an asset**:

    Content/References/Generated/<batch>/<batch>_01.png
    Content/References/Generated/<batch>/<batch>_01.json

The `.json` sidecar records the prompt, model, seed, size, time and cost. It is what makes a reference
re-generatable six months later, and what `hyper_image_inspect` reads back.

Files rather than assets on purpose: the Match page and `hyper_lighting_compare` both take a file
path, so a reference is useful the moment it lands, with no import step and nothing to cook. Import
one as a texture only if it is going into the game.
)CB"),
TEXT(R"CB(
## 8. Change a picture: a reference from the level itself

With no reference, make one from the level's own view. Pass `input_image_path`, a picture inside the project
for the model to change rather than draw from nothing, and say in the prompt what to change and what to keep.

1. Capture the view with `hyper_capture_view`: source `camera`, `camera_actor_label` `HyperAI_MatchCamera`
   (place one where the view should be if the level has none), at the shape you want, such as 1024 x 576.
2. `hyper_image_generate` with `input_image_path` set to the frame it wrote, and a prompt such as: "Relight
   this picture of a game level: late dusk, warm lamps glowing inside, cool blue moonlight through the
   windows. Keep the camera, the framing and every object exactly as they are, with the same shapes,
   positions and materials; change only the light, the shadows, the sky, the fog and the colour grade."
3. Match the level to what comes back, capturing the same camera for every attempt: it frames the same view.

With no width or height, the result keeps the picture's shape at the model's own size, so a 1280 x 720
capture comes back 1024 x 576, at the 1K price. The sidecar records the picture it changed as `input_image`,
and a capture folder records its view in `capture.json`, so a Match job knows the reference is the level's
own view and captures that same view.

The Nano Banana models, `codex_image_gen` and `gemini_image_gen` take a picture; `hyper_image_inspect` marks
each one that does with `bTakesInputImage`. For a model you configure, add `edit_request_template`: the
request body with the picture in it, where `{input_image}` becomes the picture as base64 and
`{input_mime_type}` its type. Add `edit_path` when the endpoint is another, as Automatic1111's is:

    edit_path:              /sdapi/v1/img2img
    edit_request_template:  {"init_images": ["{input_image}"], "prompt": "{prompt}",
                             "negative_prompt": "{negative}", "denoising_strength": 0.45,
                             "width": {width}, "height": {height}, "seed": {seed}}

The Nano Banana models send it as a part beside the words:

    edit_request_template:  {"contents": [{"parts": [{"text": "{prompt}"},
                             {"inlineData": {"mimeType": "{input_mime_type}", "data": "{input_image}"}}]}],
                             "generationConfig": {...}}

The picture is a PNG, JPEG or WebP of at most 5 MB, sealed into the plan by a hash of its bytes: if it
changes between the dry run and the send, nothing is sent.

In HyperAI Studio, the Images page's **Relight this view** preset does all three steps, and the Match page's
**Relight this view...** opens it.
)CB")};
	}

	FString GetCookbook()
	{
		FString Text;
		for (const TCHAR* Section : Sections)
		{
			Text += Section;
		}
		return Text;
	}

	FString GetCookbookPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".hyperai/docs/HyperAIStudio-ImageGenCookbook.md"));
	}

	bool EnsureCookbook()
	{
		const FString Path = GetCookbookPath();
		const FString Text = GetCookbook();
		FString Existing;
		if (FFileHelper::LoadFileToString(Existing, *Path) && Existing == Text)
		{
			return true;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}
