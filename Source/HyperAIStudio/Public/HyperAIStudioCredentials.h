// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"

/**
 * One key or address HyperAI can keep for an agent CLI or a provider: what the Agents settings list, and what an agent
 * the chat starts gets in its environment.
 */
struct FHyperAIStudioKeySlot
{
	/** "agent/claude-code/ANTHROPIC_API_KEY" or "provider/MESHY_API_KEY"; the Credential Manager target is HyperAIStudio/<Id>. */
	FString Id;
	/** The agent as the chat names it ("Claude Code"), or empty for a provider HyperAI's own tools call. */
	FString Agent;
	/** "Anthropic API key". */
	FString Label;
	/** The environment variable the agent (or HyperAI's tool) reads it from. */
	FString Variable;
	/** A key is shown masked; an address (a base URL) is shown as it is. */
	bool bSecret = true;
	/** The free authenticated call Test makes: anthropic, openai, gemini, openrouter, elevenlabs, stability, or empty for none. */
	FString Test;
	/** Where to get one, or how the agent uses it. */
	FString Note;
	/** The agent's own documentation names this variable; false when it could not be checked, which the list says. */
	bool bVerified = true;
	/** Other variables the same value is passed as, such as NANOBANANA_API_KEY beside Gemini's GEMINI_API_KEY. */
	TArray<FString> Aliases;
};

/** A request Test sends: never logged, since its headers hold the key. */
struct FHyperAIStudioKeyTestRequest
{
	FString Url;
	TMap<FString, FString> Headers;
};

/**
 * Keys the user gives HyperAI for their own agents and providers, kept in Windows Credential Manager (generic
 * credentials for this user, on this computer) under targets named HyperAIStudio/<slot>. A key is never written to an
 * ini, the project, a log or Activity: it goes only into the environment of a process HyperAI starts for that agent,
 * or into the request header of a provider call HyperAI's own tools make.
 */
namespace HyperAIStudio::Credentials
{
	/** HyperAIStudio/<SlotId>, or the tests' own prefix while they run. */
	HYPERAISTUDIO_API FString TargetName(const FString& SlotId);

	/** Credential Manager, by target name. Write replaces what is there; Remove is true when nothing is left. */
	HYPERAISTUDIO_API bool Write(const FString& Target, const FString& Secret, FString& OutError);
	HYPERAISTUDIO_API bool Read(const FString& Target, FString& OutSecret);
	HYPERAISTUDIO_API bool Remove(const FString& Target);

	/** What a key shows on screen: its first and last few characters around a fixed run of dots, or only dots when it is short. */
	HYPERAISTUDIO_API FString Mask(const FString& Secret);

	/** Every agent's and provider's slot, the agents' first, in the order the settings list them. */
	HYPERAISTUDIO_API TArray<FHyperAIStudioKeySlot> Slots();
	HYPERAISTUDIO_API bool FindSlot(const FString& SlotId, FHyperAIStudioKeySlot& OutSlot);
	/** The stored value of a slot, or empty. */
	HYPERAISTUDIO_API FString ReadSlot(const FString& SlotId);

	/** What an agent HyperAI starts gets: each of its slots that holds a value, by variable and alias. */
	HYPERAISTUDIO_API TMap<FString, FString> EnvironmentFor(const FString& AgentName);

	/**
	 * A provider's key for HyperAI's own tools: the one stored for Variable in Settings > Agents first, then the
	 * environment variable. Never logged or returned to an agent.
	 */
	HYPERAISTUDIO_API FString ReadKey(const FString& Variable);
	/** Where ReadKey finds it: "stored", "environment" or empty. */
	HYPERAISTUDIO_API FString KeySource(const FString& Variable);

	/**
	 * Starts Executable with Params like FPlatformProcess::CreateProc (hidden, the pipes as its stdout and stdin), with
	 * Extra added to a copy of this editor's environment for that process alone; the editor's own is not changed.
	 */
	HYPERAISTUDIO_API FProcHandle CreateProcWithEnvironment(const FString& Executable, const FString& Params, const FString& WorkingDirectory,
		const TMap<FString, FString>& Extra, void* PipeWriteChild, void* PipeReadChild, uint32* OutProcessId = nullptr);

	/**
	 * A terminal launch with AgentName's stored keys: Command runs from a PowerShell that reads them from Credential
	 * Manager by target name into its own environment, so no key is on a command line or on screen. Command as it is
	 * when the agent has none stored.
	 */
	HYPERAISTUDIO_API FString WrapTerminalCommand(const FString& AgentName, const FString& Command);
	/** AgentName's stored slots as (variable, Credential Manager target) pairs, aliases included. Game thread. */
	HYPERAISTUDIO_API TArray<TPair<FString, FString>> TargetsFor(const FString& AgentName);
	/** WrapTerminalCommand for explicit variables and targets; only builds text, so any thread may call it. */
	HYPERAISTUDIO_API FString WrapCommandWithTargets(const TArray<TPair<FString, FString>>& VariableTargets, const FString& Command);

	/**
	 * The free call that shows whether Key works for Slot: a model list or an account read, never a generation.
	 * BaseUrl, when set, is the agent's OpenAI- or Anthropic-compatible address. False when Slot has no such call.
	 */
	HYPERAISTUDIO_API bool BuildTestRequest(const FHyperAIStudioKeySlot& Slot, const FString& Key, const FString& BaseUrl, FHyperAIStudioKeyTestRequest& OutRequest);
	/** Runs Slot's test with its stored key (or the environment's); OnDone on the game thread with a line to show. */
	HYPERAISTUDIO_API void TestSlot(const FString& SlotId, TFunction<void(bool bOk, const FString& Message)> OnDone);

#if WITH_DEV_AUTOMATION_TESTS
	/** Targets start with Prefix instead of HyperAIStudio/, and Extra slots join the list; empty and none restore it. */
	HYPERAISTUDIO_API void SetTestScope(const FString& Prefix, const TArray<FHyperAIStudioKeySlot>& Extra);
#endif
}
