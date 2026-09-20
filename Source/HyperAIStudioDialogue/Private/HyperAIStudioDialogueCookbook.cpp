// Games by Hyper 2026.

#include "HyperAIStudioDialogueCookbook.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::Dialogue
{
	namespace
	{
		// Split into literals under the compiler's single-literal size limit.
		const TCHAR* const Sections[] = {
TEXT(R"(# Dialogue Cookbook

Written by HyperAIStudio for agents writing and rendering character dialogue. You write the lines; the
`dialogue_speech` tools render them to speech and bring them into the project.

## 1. Writing the lines

**You write them.** There is no dialogue-writing tool and there does not need to be: you are already the
language model. Write the scene, then hand the finished lines to `hyper_dialogue_render`.

Write for the ear. It will be heard, never read.

- Characters want something in every line. If a line does not pursue, deflect, conceal, or wound, cut it.
- Never let a character accurately name their own feeling. Show it in what they do with the subject.
- People talk past each other. At least a third of your lines should not answer what was just said.
- Facts are extracted, not offered. If someone explains something willingly, you have written a lecture.
- Enter the scene after it started. Leave before it finishes. Nobody explains how they got there.
- No line may summarize the scene's meaning. Especially not the last one.
- Specificity over adjectives: the brand of cigarette, the name of the ferry, the exact sum owed.
- Register is character. A dockworker and a coroner do not build sentences the same way.
- Leave at least two things unresolved.
- Make speakers identifiable from their words alone. If two characters could swap lines unnoticed, rewrite both.
- Human speech is never level. Let the performance turn with the situation rather than writing one even read.

## 2. Text and direction

Every line has **text**, which is spoken, and a **direction**, which is not. This is the whole control surface:
the model has no emotion dials, because the direction does that job the way a director talks to an actor.

```
text:      You signed my name.
direction: with barely controlled rage
```

- Put performance in the direction, never in the text. Writing "sigh" or "ahem" in the text makes the model
  **say the word**. `hyper_dialogue_validate` warns when it sees one.
- Write directions in the third person, as pacing or physical cues: `she sighs deeply`, `a long pause`,
  `her voice cracks`, `he gulps nervously`.
- Describe situation-led prosody rather than a fixed pitch. A voice can rise on a challenge, fall on a threat,
  catch on a confession, or lose energy after a long pause.
- Genuine vocalisations are spoken, so they belong in the text: `Hahaha`, `Hmm`, `Ugh`, `Argh`.
- A double quote in the text would close the spoken span early and the rest would be performed as direction.
  It is demoted to a single quote before rendering, and validate warns.

The prompt the service receives is assembled for you:

```
A weary man in his fifties, low gravelly voice, with barely controlled rage, "You signed my name."
```

## 3. Voices

A voice is an asset, not a string. Create a HyperAIStudio Voice with:

- **Reference clip** - a file name inside the Voice Reference Folder in settings. At least ten seconds of
  clean speech from one speaker. It pins the timbre. **Use a clip you have the right to clone.**
- **Speaker description** - a sentence that pins the register: age, build of the voice, habitual delivery.
  It opens every prompt for that character.
- **Seed** - fixed per voice, so the same line renders the same twice. Change it to draw a different reading
  of the whole character.

The clip pins who it sounds like; the description pins how they carry themselves. You need both. A clip alone
gives a voice that performs flat; a description alone gives a performance in nobody's voice.

## 4. The flow

1. `hyper_dialogue_inspect` - is the service up, which voices exist, are their clips present.
2. `hyper_dialogue_validate` - check the lines before spending the render. It returns the prompt the first
   line would send and roughly how long the batch takes.
3. `hyper_dialogue_render` - returns at once with a render_id. Lines render one at a time, because the model
   owns the graphics card. Watch it with `hyper_dialogue_inspect`.
4. `hyper_dialogue_apply_plan` - imports the finished render as sound waves, and Dialogue Waves carrying the
   speaker and spoken text. Dry-run, resubmit with the plan hash, poll.

Each line is its own wav, so every character stays separate and can be mixed later. Roughly a second of
rendering per second of audio: a forty-line scene is minutes, not seconds.

## 5. What these tools do not do

- **They do not write the dialogue.** That is section 1, and it is your job.
- **English only.** The speech model is English-only and refuses anything else.
- **No mixing.** One wav per line, unmixed and unprocessed, is what you get.
- **No Decibel session yet.** Importing a scene as one track per character comes later.
)")};
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
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".hyperai/docs/HyperAIStudio-DialogueCookbook.md"));
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
