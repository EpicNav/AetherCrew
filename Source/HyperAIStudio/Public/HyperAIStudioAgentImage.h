// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class FHyperAIStudioNativeChat;

/**
 * One picture drawn by an agent CLI that has an image generator, run headless for the agents and tools that have
 * none, such as Claude Code. Codex draws with its built-in image tool on the user's own plan. Gemini CLI has no
 * generator of its own and draws only with its image extension installed. Game thread only; the CLI runs while the
 * editor ticks.
 */
class HYPERAISTUDIO_API FHyperAIStudioAgentImage
{
public:
	/** Longest one picture may take before it is given up on. */
	static constexpr double TimeoutSeconds = 300.0;

	/** codex or gemini: true when this machine can draw with it; otherwise OutIssue says why. */
	static bool CanDraw(const FString& Agent, FString& OutIssue);

#if WITH_DEV_AUTOMATION_TESTS
	/** Runs Command instead of the agent's CLI, so tests can use a scripted stand-in. Empty restores it. */
	static void SetCommandForTests(const FString& Command);
#endif

	/** Starts drawing Prompt; poll IsDone. The size is a hint in the prompt, since these generators pick from a few sizes. */
	static TSharedPtr<FHyperAIStudioAgentImage> Start(const FString& Agent, const FString& Prompt, const FIntPoint& SizeHint, FString& OutError);

	~FHyperAIStudioAgentImage();

	/** Polls the agent; true once it has drawn the picture or given up. */
	bool IsDone();
	/** The picture the agent saved, once done; empty when it failed. */
	const FString& GetImagePath() const { return ImagePath; }
	/** Why it failed, in the agent's own words where it gave any. */
	const FString& GetError() const { return Error; }
	/** The prompt as the generator rewrote it, when it says. */
	const FString& GetRevisedPrompt() const { return RevisedPrompt; }

private:
	FHyperAIStudioAgentImage() = default;
	void Finish(const FString& InError);

	TSharedPtr<FHyperAIStudioNativeChat> Chat;
	FString Agent;
	FString WorkDirectory;
	FString ImagePath;
	FString Error;
	FString RevisedPrompt;
	double StartedAt = 0.0;
	bool bDone = false;
};
