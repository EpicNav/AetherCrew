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
`hyper_image_generate`. Read this before the first generation in a session. It ships no model of its own, so
`hyper_image_generate` does nothing until one is configured - and section 2 may mean you never need to.

## 1. What this pack is for

A reference image is a **look target**: the light, the palette, the mood, the weather. It exists so the
level can be measured against something, not so it can be copied.

It is **not a plan**. A generated image has no scale, no geometry, no metres, and no idea what a
playable space needs. Nothing in it tells you a doorway is 2.2 m. Route layout through
`hyper_level_design_*`, which measures the level itself, and use the image only for the look.

The loop this pack is built for:

1. `hyper_image_generate` writes a reference PNG into the project.
2. Open the HyperAI Preview tab and point it at that PNG, beside a viewport capture.
3. `hyper_lighting_compare` measures the two and reports the gap.
4. Adjust the lighting, capture again, compare again.

Step 3 is why the prompt template pins time of day, key light direction, contrast and colour
temperature. A reference whose light is beautiful but unreachable makes step 4 grind forever.

## 2. First: can you already generate images yourself?

Check before configuring anything here. Some agents that drive this plugin carry their own image
generation - Codex has an `imagegen` skill, for instance - and when yours does, **use it**. It costs
nothing beyond the subscription already running the session, there is no key to set, no endpoint to
configure, and no model name to go stale.

This pack does not care who made an image. `hyper_image_inspect`, the Preview tab and
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

Configure a model below only when you need something your agent cannot do: a specific checkpoint or
style, a fixed seed you can return to, a batch of variations in one go, or a model running offline.

## 3. Configure a model

Run `hyper_image_inspect`. If it reports no models, nothing can generate yet.

A model is two settings entries, both under **Project Settings > Plugins > HyperAI Studio**:

- **External Services** - one entry with the endpoint's base url, the *name of the environment
  variable* holding the API key (never the key itself), the cost per call, and a timeout.
- **Image Models** - one entry naming that service, the path, a request template, and where the image
  sits in the reply.

This plugin deliberately ships neither. Image model names change faster than plugin releases, and a
name compiled in goes stale somewhere nobody can correct it.

### The request template

`request_template` is the JSON body, with these substituted in before sending:

| Slot | Becomes |
|---|---|
| `{prompt}` | the composed prompt |
| `{negative}` | the negative prompt, or empty |
| `{model}` | the entry's `model` |
| `{width}` `{height}` | the requested size |
| `{seed}` | the seed, or `-1` |

Values are JSON-escaped as they are substituted, so a prompt containing quotes, backslashes or
newlines cannot break the body. Put `{prompt}` inside the quotes of a string field, not around them.

### The response path

`response_image_path` is a dotted path to **base64 image data** in the reply. Numbers index arrays:
`data.0.b64_json` means "the `b64_json` field of the first element of `data`".

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

A Gemini-shaped endpoint:

    path:                 /v1beta/models/{model}:generateContent
    request_template:     {"contents": [{"parts": [{"text": "{prompt}"}]}]}
    response_image_path:  candidates.0.content.parts.0.inlineData.data

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

Files rather than assets on purpose: the Preview tab and `hyper_lighting_compare` both take a file
path, so a reference is useful the moment it lands, with no import step and nothing to cook. Import
one as a texture only if it is going into the game.
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
