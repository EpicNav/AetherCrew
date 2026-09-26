// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "HyperAIStudioSettings.generated.h"

UENUM()
enum class EHyperAIStudioCapabilityPreset : uint8
{
	// Keep these numeric values stable: existing per-project user configs serialize this enum.
	Core = 0 UMETA(DisplayName = "Project Plugins (Recommended)"),
	Domains = 1 UMETA(DisplayName = "Selected Domains"),
	Everything = 2 UMETA(DisplayName = "Everything (Epic All Toolsets)")
};

UENUM()
enum class EHyperAIStudioCapabilityDomain : uint8
{
	Animation UMETA(DisplayName = "Animation"),
	VfxAndSimulation UMETA(DisplayName = "VFX and Simulation"),
	Worldbuilding UMETA(DisplayName = "Worldbuilding"),
	Gameplay UMETA(DisplayName = "Gameplay"),
	UiAndTest UMETA(DisplayName = "UI and Test"),
	ProjectAndPipeline UMETA(DisplayName = "Project and Pipeline")
};

/**
 * One service outside the editor that a pack can call: a model server on this machine, or a hosted API.
 *
 * A key is never stored here. ApiKeyVariable names an environment variable, so a key cannot reach a config
 * file that gets committed; the settings screen and every report say only whether one is set.
 */
USTRUCT(BlueprintType)
struct FHyperAIStudioExternalServiceSettings
{
	GENERATED_BODY()

	/** Identifier a pack asks for, e.g. dramabox. */
	UPROPERTY(Config, EditAnywhere, Category = "External Services")
	FString Id;

	UPROPERTY(Config, EditAnywhere, Category = "External Services")
	FString Label;

	/** Scheme, host and port, with no trailing slash, e.g. http://127.0.0.1:8080. */
	UPROPERTY(Config, EditAnywhere, Category = "External Services")
	FString BaseUrl;

	/** Path the health check asks for; empty means the base url answers it. */
	UPROPERTY(Config, EditAnywhere, Category = "External Services")
	FString HealthPath;

	/** Environment variable holding the key. Empty for a service that needs none. */
	UPROPERTY(Config, EditAnywhere, Category = "External Services")
	FString ApiKeyVariable;

	/** Header the key goes in, and what precedes it, e.g. Authorization and "Bearer ". */
	UPROPERTY(Config, EditAnywhere, Category = "External Services")
	FString ApiKeyHeader = TEXT("Authorization");

	UPROPERTY(Config, EditAnywhere, Category = "External Services")
	FString ApiKeyPrefix = TEXT("Bearer ");

	/** What one call costs, for the approval summary and the session cap. Zero for a service on this machine. */
	UPROPERTY(Config, EditAnywhere, Category = "External Services")
	float CostPerCall = 0.0f;

	/** A call that has not answered by now is abandoned; a queued GPU model needs a generous one. */
	UPROPERTY(Config, EditAnywhere, Category = "External Services")
	int32 TimeoutSeconds = 180;
};

/**
 * One image model, described rather than coded.
 *
 * Image endpoints and model names change faster than anything else this plugin touches, and a name compiled
 * in goes stale where nobody can correct it. So a model is a request template and a path into the reply:
 * adding one is an edit here, not a rebuild. The cookbook has a worked example of each common shape.
 */
USTRUCT(BlueprintType)
struct FHyperAIStudioImageModelSettings
{
	GENERATED_BODY()

	/** Identifier a tool asks for, e.g. gpt_image or sdxl_local. */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	FString Id;

	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	FString Label;

	/** Which entry in External Services carries this model's url, key and cost. */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	FString ServiceId;

	/**
	 * Path under the service's base url, e.g. /v1/images/generations, /sdapi/v1/txt2img or
	 * /v1beta/models/{model}:generateContent, with {model} substituted.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	FString Path;

	/**
	 * The request body. Substituted in:
	 * - {prompt}, {negative}, {width}, {height}, {seed} and {model}.
	 * - {aspect_ratio}: the nearest of 1:1, 2:3, 3:2, 3:4, 4:3, 4:5, 5:4, 9:16, 16:9 and 21:9.
	 * - {image_size}: 1K, 2K or 4K, from the longest side.
	 * Values are JSON-escaped as they go in, so a prompt with quotes cannot break the body. A template without
	 * {negative} still gets the negative prompt, at the end of the prompt as "Avoid: ...".
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation", meta = (MultiLine = "true"))
	FString RequestTemplate;

	/**
	 * The request body when a picture goes in to be changed rather than drawn from nothing, such as a view of the level
	 * to relight. Substituted like Request Template, plus {input_image}, the picture as base64, and {input_mime_type}:
	 * image/png, image/jpeg or image/webp. Empty: this model draws from words only.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation", meta = (MultiLine = "true"))
	FString EditRequestTemplate;

	/** The path for a request with a picture in it, when it is not Path, such as /sdapi/v1/img2img. */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	FString EditPath;

	/**
	 * Dotted path to the base64 image in the reply: data.0.b64_json for an OpenAI-shaped one, images.0 for
	 * an Automatic1111, candidates.0.content.parts.*.inlineData.data for a Gemini. A * takes the first element
	 * that has the rest of the path, for a reply that puts text before the picture.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	FString ResponseImagePath;

	/** Substituted for {model}. */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	FString Model;

	UPROPERTY(Config, EditAnywhere, Category = "Image Generation", meta = (ClampMin = "64", ClampMax = "4096"))
	int32 Width = 1024;

	UPROPERTY(Config, EditAnywhere, Category = "Image Generation", meta = (ClampMin = "64", ClampMax = "4096"))
	int32 Height = 1024;

	/**
	 * What one image costs, for the approval summary and the session cap. There are prices for a longest side up
	 * to 1024 (1K), 2048 (2K) and above (4K). A size left at zero costs what the one below it does; all zero uses
	 * the service's Cost Per Call.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation", meta = (ClampMin = "0.0", DisplayName = "Cost Per Image 1K"))
	float CostPerImage = 0.0f;

	UPROPERTY(Config, EditAnywhere, Category = "Image Generation", meta = (ClampMin = "0.0", DisplayName = "Cost Per Image 2K"))
	float CostPerImage2K = 0.0f;

	UPROPERTY(Config, EditAnywhere, Category = "Image Generation", meta = (ClampMin = "0.0", DisplayName = "Cost Per Image 4K"))
	float CostPerImage4K = 0.0f;

	/**
	 * True when this model runs on this machine. A local model competes with the editor for the same
	 * graphics card, and when they do not both fit the generation crawls; the tools say so before starting.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	bool bRunsOnThisMachine = false;
};

/**
 * A prompt with named slots. Visible and editable on purpose: a hidden prompt that "gets good results" is
 * one nobody can tune, and matching a level to a reference needs the reference to pin the things the
 * lighting compare measures.
 */
USTRUCT(BlueprintType)
struct FHyperAIStudioImagePromptTemplateSettings
{
	GENERATED_BODY()

	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	FString Id;

	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	FString Label;

	/** Slots are written {like_this} and filled from the tool's slot list; an unfilled slot drops out. */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation", meta = (MultiLine = "true"))
	FString Template;
};

/** The Nano Banana model Gemini CLI's nanobanana extension draws with. */
UENUM()
enum class EHyperAIStudioNanoBananaModel : uint8
{
	ExtensionDefault UMETA(DisplayName = "Extension default"),
	NanoBanana UMETA(DisplayName = "Nano Banana (Gemini 2.5 Flash Image)"),
	NanoBanana2 UMETA(DisplayName = "Nano Banana 2 (Gemini 3.1 Flash Image)"),
	NanoBananaPro UMETA(DisplayName = "Nano Banana Pro (Gemini 3 Pro Image)")
};

UENUM()
enum class EHyperAIStudioMCPTransport : uint8
{
	StreamableHttp UMETA(DisplayName = "Streamable HTTP"),
	Command UMETA(DisplayName = "Command")
};

/** Runtime policy for native HyperAI tools. Risky effects remain strict in every mode. */
UENUM()
enum class EHyperAIStudioNativeExecutionMode : uint8
{
	Fast UMETA(DisplayName = "Fast (Recommended)"),
	StrictSafety UMETA(DisplayName = "Strict Safety")
};

/** User-facing Unreal MCP tool set. Internal evidence/admission state remains separate. */
UENUM()
enum class EHyperAIStudioNativeToolChannel : uint8
{
	Preview UMETA(DisplayName = "Unreal MCP + Extended Hyper Tools"),
	StableOnly UMETA(DisplayName = "Unreal MCP Only")
};

/** How a memory an agent saves on its own reaches future chats. Memories the user adds always apply in full. */
UENUM()
enum class EHyperAIStudioAgentMemoryMode : uint8
{
	/** Future chats see its title at once and can recall its text; the text joins every chat's instructions once you confirm it. */
	TitleUntilReviewed UMETA(DisplayName = "Title now, full text after review"),
	/** Straight into every future chat's instructions; review or delete it later. */
	Automatic UMETA(DisplayName = "Fully automatic"),
	/** Invisible to future chats until you confirm it. */
	AfterReview UMETA(DisplayName = "Only after review")
};

USTRUCT()
struct FHyperAIStudioMCPServerEntry
{
	GENERATED_BODY()

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	FString Id = TEXT("custom-mcp");

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	FString DisplayName = TEXT("Custom MCP Server");

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	FString Description;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	EHyperAIStudioMCPTransport Transport = EHyperAIStudioMCPTransport::StreamableHttp;

	UPROPERTY(Config, EditAnywhere, Category = "MCP", meta = (EditCondition = "Transport == EHyperAIStudioMCPTransport::StreamableHttp"))
	FString Url = TEXT("http://127.0.0.1:9000/mcp");

	UPROPERTY(Config, EditAnywhere, Category = "MCP", meta = (EditCondition = "Transport == EHyperAIStudioMCPTransport::Command"))
	FString Command;

	UPROPERTY(Config, EditAnywhere, Category = "MCP", meta = (EditCondition = "Transport == EHyperAIStudioMCPTransport::Command"))
	TArray<FString> Arguments;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	int32 Priority = 100;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	TArray<FString> Domains;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	TArray<FString> TargetClients;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	TArray<FString> AgentInstructions;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	FString SetupNotes;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	bool bEnabled = false;
};

/** How agents may build material graphs. */
UENUM()
enum class EHyperAIStudioMaterialAuthoringMode : uint8
{
	/** Graph nodes only; Custom HLSL nodes are refused. */
	Nodes,
	/** Custom HLSL wherever the agent likes. */
	Hlsl,
	/** Graph nodes, with a Custom HLSL node only where nodes cannot express the math, each with a reason. */
	Hybrid
};

/** Model choices for one HyperAI Chat agent. */
USTRUCT()
struct FHyperAIStudioAgentModelRoute
{
	GENERATED_BODY()

	/** Chat agent this applies to: Codex, Claude Code or Gemini. */
	UPROPERTY(Config, EditAnywhere, Category = "Models")
	FString AgentName;

	/** Launch argument with exactly one {0} for the model id. Codex, Claude Code and Gemini all accept --model {0}. */
	UPROPERTY(Config, EditAnywhere, Category = "Models")
	FString ModelFlagFormat = TEXT("--model {0}");

	/** Model ids offered in the chat Model menu, spelled exactly as the agent CLI accepts them. */
	UPROPERTY(Config, EditAnywhere, Category = "Models")
	TArray<FString> Models;

	/** Empty means the agent's own configured default: no model flag is passed. */
	UPROPERTY(Config, EditAnywhere, Category = "Models")
	FString SelectedModel;
};

/** Which agent a new chat tab for one kind of task starts with. */
USTRUCT()
struct FHyperAIStudioAgentTaskRoute
{
	GENERATED_BODY()

	/** Shown in the new-tab menu and on the tab, e.g. VFX. */
	UPROPERTY(Config, EditAnywhere, Category = "Routing")
	FString Category;

	/** Empty picks the agent with the best record on Packs, or the preferred agent until one has a record. */
	UPROPERTY(Config, EditAnywhere, Category = "Routing")
	FString AgentName;

	/** Empty uses the agent's selected model. Must be in that agent's model list, or smart for Claude Code. */
	UPROPERTY(Config, EditAnywhere, Category = "Routing")
	FString ModelId;

	/** Capability packs this kind of task uses; their operation outcomes decide the automatic pick. */
	UPROPERTY(Config, EditAnywhere, Category = "Routing")
	TArray<FString> Packs;

	/** Claude Code only: the tab starts as this subagent from .hyperai/agents or .claude/agents, e.g. programmer. */
	UPROPERTY(Config, EditAnywhere, Category = "Routing")
	FString Subagent;
};

UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig, meta = (DisplayName = "HyperAIStudio"))
class HYPERAISTUDIO_API UHyperAIStudioSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(Config, EditAnywhere, Category = "First Run")
	bool bShowWelcomeOnStartup = true;

	/**
	 * Language of HyperAI's own windows, whatever language the editor uses. Editor follows the editor's language when
	 * HyperAI has a translation for it; en is English. Applies at once.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Interface", meta = (DisplayName = "HyperAI Language", GetOptions = "GetLanguageOptions"))
	FString UILanguage = TEXT("Editor");

	/** Editor, en, and every language HyperAI has a translation for. */
	UFUNCTION()
	static TArray<FString> GetLanguageOptions();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Internal one-shot marker used to finish a toolset sync after Unreal restarts. */
	UPROPERTY(Config)
	bool bResumeSetupAfterRestart = false;

	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP", meta = (ClampMin = "1", ClampMax = "65535"))
	uint32 UnrealMCPPort = 8000;

	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP")
	FString UnrealMCPUrlPath = TEXT("/mcp");

	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP")
	bool bAutoStartUnrealMCP = true;

	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP")
	bool bEnableToolSearch = true;

	/**
	 * Fast avoids repeated deep trust work for core-owned reads and reversible edits. Destructive
	 * and external-effect routes always retain strict validation, regardless of this setting.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP|Performance")
	EHyperAIStudioNativeExecutionMode NativeExecutionMode = EHyperAIStudioNativeExecutionMode::Fast;

	/** Select Epic's Unreal MCP alone or add HyperAIStudio's extended tools on the same endpoint. */
	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP|Capabilities", meta = (DisplayName = "Unreal MCP Tool Set", ConfigRestartRequired = true))
	EHyperAIStudioNativeToolChannel NativeToolChannel = EHyperAIStudioNativeToolChannel::Preview;

	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP|Capabilities", meta = (
		DisplayName = "Epic MCP Toolsets",
		ToolTip = "Project Plugins automatically enables the matching Epic MCP toolset wrappers for domain plugins already active in this project. It never enables missing domain plugins."))
	EHyperAIStudioCapabilityPreset CapabilityPreset = EHyperAIStudioCapabilityPreset::Core;

	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP|Capabilities", meta = (EditCondition = "CapabilityPreset == EHyperAIStudioCapabilityPreset::Domains"))
	TArray<EHyperAIStudioCapabilityDomain> EnabledCapabilityDomains;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Unreal MCP|Capabilities")
	bool bEnableChaosClothAssetToolset = false;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Unreal MCP|Capabilities")
	bool bEnableLiveCodingToolset = false;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Unreal MCP|Capabilities")
	bool bEnableMetaHumanGenerator = false;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Unreal MCP|Capabilities")
	bool bEnableMVVMToolset = false;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Unreal MCP|Capabilities")
	bool bEnableSequencerAnimMixerToolset = false;

	/** Reserved managed-config migration level. Advance only after ownership-safe reconciliation succeeds completely. */
	UPROPERTY(Config, VisibleAnywhere, Category = "Unreal MCP")
	int32 NativeToolArchitectureMigrationVersion = 0;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Files")
	bool bGenerateCodexConfig = true;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Files")
	bool bGenerateClaudeConfig = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Files")
	bool bGenerateCursorConfig = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Files")
	bool bGenerateVSCodeConfig = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Files")
	bool bGenerateGeminiConfig = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Files")
	bool bGenerateClaudeMarkdown = false;

	UPROPERTY(Config, EditAnywhere, Category = "Community MCP")
	TArray<FHyperAIStudioMCPServerEntry> ExtraServers;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	FString PreferredAgent = TEXT("Codex");

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bEnableCodexAgent = true;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bEnableClaudeAgent = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bEnableGeminiAgent = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bEnableCursorAgent = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bEnableVSCodeAgent = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bAllowClipboardPromptCopy = true;

	/**
	 * Hold every agent asset edit in the chat panel's Activity view until you approve it. Turn this off and
	 * edits run as soon as the agent submits them. Destructive and external-effect tools always need approval,
	 * because their server grant is issued only by that click.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bRequireApprovalForAgentEdits = true;

	/**
	 * Lines that mean an agent is waiting for an answer: a permission prompt, a choice, a confirmation.
	 * Matched case-insensitively against the last rows of that tab's terminal. Empty restores the built-in list.
	 * These exist because agent CLIs change their wording; editing them here beats waiting for a plugin build.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Tab Status")
	TArray<FString> AgentNeedsInputPatterns;

	/** Lines that mean an agent is showing a plan and waiting to be told to go ahead. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Tab Status")
	TArray<FString> AgentPlanReviewPatterns;

	/** Lines that mean an agent cannot proceed: a rate limit, a login, a busy editor lane. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Tab Status")
	TArray<FString> AgentBlockedPatterns;

	/** Models offered per chat agent. Agents without an entry get one the first time HyperAI Chat opens. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Models")
	TArray<FHyperAIStudioAgentModelRoute> AgentModelRoutes;

	/** Model Claude Code plans with when the Model menu is set to Smart. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Models")
	FString SmartPlanModel = TEXT("claude-fable-5-1");

	/** Model Claude Code builds with once a Smart plan is accepted. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Models")
	FString SmartBuildModel = TEXT("claude-opus-5");

	/** In Smart mode, answer Claude Code's plan approval with its auto-mode choice so building starts without you. Unreal edits still pass the approval gate. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Models")
	bool bAutoAcceptPlansInSmartMode = true;

	/**
	 * Claude Code chats with Remote on run as background sessions that the chat tab attaches to, so closing the tab
	 * or the editor leaves them running and reachable from claude.ai/code and the Claude app. Reopen one from Chats.
	 * Off: remote chats run inside the tab and stop with it.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Remote", meta = (DisplayName = "Keep Remote Chats Running After The Editor Closes"))
	bool bKeepRemoteChatsRunning = true;

	/**
	 * While on, everything saved in the editor lands in a UE File Sandbox until you approve or discard all of it in HyperAI's
	 * Activity panel: agent edits, Epic's tools, scripts, and your own saves too. Agents cannot approve or discard.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Sandbox", meta = (DisplayName = "Sandbox Mode"))
	bool bSandboxMode = false;

	/** Chats open in HyperAI's own chat view instead of the terminal. A Claude Code chat with Remote Control stays in the terminal. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Chat", meta = (DisplayName = "Native Chat View"))
	bool bUseNativeChatView = true;

	/** Actor classes a playtest may spawn into the PIE world. Nothing else can be spawned. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Playtests", meta = (AllowAbstract = "false", MetaClass = "/Script/Engine.Actor"))
	TArray<FSoftClassPath> PlaytestSpawnableClasses;

	/**
	 * How a memory an agent saves on its own (hyper_memory_remember) reaches future chats: at once by default. A fact
	 * that reads like instructions to agents, as one planted by text the agent read would, waits for you in
	 * HyperAIStudio > Agents > Project Memory whatever this says. (Not AgentMemoryMode: projects that kept the old
	 * review-first default take this one.)
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Memory", meta = (DisplayName = "Agent-Saved Memories"))
	EHyperAIStudioAgentMemoryMode AgentMemory = EHyperAIStudioAgentMemoryMode::Automatic;

	/** The chat panel's new-tab menu: one entry per kind of task. Empty restores the built-in list. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Routing")
	TArray<FHyperAIStudioAgentTaskRoute> AgentTaskRoutes;

	/**
	 * During a handoff between tabs, send each prompt (plan, implement, review) as soon as the receiving agent is
	 * idle. Off, or while that agent is busy, the prompt is only typed in for you to send.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Routing")
	bool bAutoSendHandoffPrompts = true;

	/**
	 * Services outside the editor that packs may call: a model server on this machine, or a hosted API.
	 * Add one by naming the environment variable its key lives in, never the key itself.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "External Services")
	TArray<FHyperAIStudioExternalServiceSettings> ExternalServices;

	/**
	 * Let an agent apply an edit in one call, without a dry run first. The plan is still computed, sealed,
	 * journaled and undoable, and still waits for approval when approval is on; this only drops the
	 * round trip that asked the agent to repeat the plan's hash back.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Agents")
	bool bAllowOneShotAgentEdits = true;

	/**
	 * Rewrite the HyperAIStudio block in AGENTS.md and CLAUDE.md at startup when it is older than this
	 * build's. Only the managed block changes; text outside it is kept. Without this, guidance added in
	 * an update reaches agents only after someone reruns Setup.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Agents")
	bool bRefreshAgentInstructionsOnStartup = true;

	/**
	 * Adds hyper_script_run: agents run editor Python inside one undo step that rolls back if the script
	 * fails or runs out of time. A script that imports system modules, deletes, renames, loads levels or
	 * writes files waits for your approval instead. Restart the editor after changing this.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Agents", meta = (DisplayName = "Allow Agents To Run Python Scripts"))
	bool bEnablePythonScripting = false;

	/**
	 * Image models available to the generation tools, besides the built-in ones. Those are Google's Nano Banana
	 * models through the Gemini API (nano_banana_2, nano_banana_pro, nano_banana_2_lite, nano_banana) and the
	 * agents that draw on your own plan (codex_image_gen, gemini_image_gen). An entry here with a built-in id
	 * replaces that model, as when Google renames one. The cookbook shows how to add one.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	TArray<FHyperAIStudioImageModelSettings> ImageModels;

	/**
	 * The model gemini_image_gen draws with. It is passed to Gemini CLI's nanobanana extension as
	 * NANOBANANA_MODEL, and GEMINI_API_KEY as its key when NANOBANANA_API_KEY is not set. The Nano Banana
	 * models call the Gemini API directly instead, with the key in GEMINI_API_KEY.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation", meta = (DisplayName = "Gemini CLI Image Model"))
	EHyperAIStudioNanoBananaModel GeminiCliImageModel = EHyperAIStudioNanoBananaModel::ExtensionDefault;

	/** The Gemini model id for Gemini CLI Image Model; empty for the extension's default. */
	FString GetGeminiCliImageModelId() const;

	/** Google's Gemini API, for the Nano Banana models. It is there without settings; an entry with its id replaces it. */
	static FHyperAIStudioExternalServiceSettings GetGeminiService();

	/**
	 * Nano Banana 2, Nano Banana Pro, Nano Banana 2 Lite and the original Nano Banana, through the Gemini API, with
	 * Google's prices per image.
	 */
	static TArray<FHyperAIStudioImageModelSettings> GetNanoBananaModels();

	/** Prompt templates the generation tools fill in. Seeded with one for scene references. */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	TArray<FHyperAIStudioImagePromptTemplateSettings> ImagePromptTemplates;

	/** Where generated reference images land, inside the project. */
	UPROPERTY(Config, EditAnywhere, Category = "Image Generation")
	FString ImageOutputFolder = TEXT("Content/References/Generated");

	/**
	 * Seeds the prompt templates when there are none. Templates carry no model name, so unlike a model
	 * entry they do not go stale.
	 */
	bool EnsureDefaultImagePromptTemplates()
	{
		if (!ImagePromptTemplates.IsEmpty())
		{
			return false;
		}
		FHyperAIStudioImagePromptTemplateSettings& Scene = ImagePromptTemplates.AddDefaulted_GetRef();
		Scene.Id = TEXT("scene_reference");
		Scene.Label = TEXT("Scene reference for lighting");
		// Every slot after the subject names something hyper_lighting_compare measures. A reference that
		// leaves them to the model is one the level can chase without ever reaching.
		Scene.Template = TEXT("{subject}. {time_of_day}, {weather}. Key light {key_light}. ")
			TEXT("{contrast} contrast, {palette} palette, {mood} mood. Shot on {lens}. ")
			TEXT("Photographic, no text, no watermark, no people unless asked.");
		return true;
	}

	/**
	 * What generated content may cost in one editor session, in whatever currency the services bill in.
	 * A call that would pass this is refused rather than quietly spending more. Zero refuses every paid call.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "External Services", meta = (ClampMin = "0.0"))
	float ExternalServiceSessionSpendCap = 5.0f;

	/** Reference images the preview panel has been pointed at, newest first. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Preview")
	TArray<FString> RecentPreviewReferences;

	/** Moves Path to the front of the recent list, keeping it short, and saves. */
	void RememberPreviewReference(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return;
		}
		RecentPreviewReferences.RemoveAll([&Path](const FString& Existing) { return Existing.Equals(Path, ESearchCase::IgnoreCase); });
		RecentPreviewReferences.Insert(Path, 0);
		constexpr int32 MaxRecentPreviewReferences = 8;
		if (RecentPreviewReferences.Num() > MaxRecentPreviewReferences)
		{
			RecentPreviewReferences.SetNum(MaxRecentPreviewReferences);
		}
		SaveConfig();
	}

	/**
	 * How agents build materials. Hybrid: graph nodes, which artists can read and the compiler optimises, plus a
	 * Custom HLSL node only for math nodes cannot express, each with a stated reason. Nodes forbids Custom HLSL;
	 * Hlsl allows it freely.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP|Materials")
	EHyperAIStudioMaterialAuthoringMode MaterialAuthoringMode = EHyperAIStudioMaterialAuthoringMode::Hybrid;

	/** Model menu id for Smart: plan with SmartPlanModel, build with SmartBuildModel. */
	static constexpr const TCHAR* SmartModelId = TEXT("smart");

	/** Smart relies on Claude Code's opusplan alias, so only Claude Code offers it. */
	static bool IsSmartModelAgent(const FString& AgentName)
	{
		return AgentName.Equals(TEXT("Claude Code"), ESearchCase::IgnoreCase);
	}

	/** Shows the fast local title-based comment action in Blueprint/PCG graph context menus. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Blueprint Context Menu", meta = (
		DisplayName = "Show Quick Auto Comment in Blueprint Menu",
		ToolTip = "Adds the local title-based Quick Auto Comment action to Blueprint and PCG graph context menus. Smart Auto Comment remains the normal AI-assisted action."))
	bool bShowQuickAutoCommentInBlueprintMenu = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bAllowTerminalLaunch = true;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bAllowExternalAgentLaunch = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bRequireConfirmationForDestructiveActions = true;

	UPROPERTY(Config, EditAnywhere, Category = "Documentation")
	FString DocumentationUrl = TEXT("https://gamesbyhyper.com/docs/project-foundations/hyper-ai-studio/");

	const FHyperAIStudioAgentModelRoute* FindAgentModelRoute(const FString& AgentName) const
	{
		return AgentModelRoutes.FindByPredicate([&AgentName](const FHyperAIStudioAgentModelRoute& Route)
		{
			return Route.AgentName.Equals(AgentName, ESearchCase::IgnoreCase);
		});
	}

	FHyperAIStudioAgentModelRoute* FindAgentModelRoute(const FString& AgentName)
	{
		return AgentModelRoutes.FindByPredicate([&AgentName](const FHyperAIStudioAgentModelRoute& Route)
		{
			return Route.AgentName.Equals(AgentName, ESearchCase::IgnoreCase);
		});
	}

	/** Adds a route for each bundled chat agent that has none. Returns true if anything was added. */
	bool EnsureDefaultAgentModelRoutes()
	{
		// Claude Code's --help documents these aliases. Codex and Gemini document no model names, so their lists
		// start empty and Default uses whatever model the agent's own config selects.
		struct FSeed
		{
			const TCHAR* AgentName;
			TArray<FString> Models;
		};
		const FSeed Seeds[] = {
			{ TEXT("Codex"), {} },
			{ TEXT("Claude Code"), { TEXT("fable"), TEXT("opus"), TEXT("sonnet") } },
			{ TEXT("Gemini"), {} }
		};

		bool bAdded = false;
		for (const FSeed& Seed : Seeds)
		{
			if (!FindAgentModelRoute(Seed.AgentName))
			{
				FHyperAIStudioAgentModelRoute& Route = AgentModelRoutes.AddDefaulted_GetRef();
				Route.AgentName = Seed.AgentName;
				Route.Models = Seed.Models;
				bAdded = true;
			}
		}
		return bAdded;
	}

	/** Seeds the task routes when the list is empty. Returns true if it did. */
	bool EnsureDefaultTaskRoutes()
	{
		if (!AgentTaskRoutes.IsEmpty())
		{
			return false;
		}
		auto Add = [this](const TCHAR* Category, TArray<FString> Packs, const TCHAR* Agent = TEXT(""), const TCHAR* Model = TEXT(""))
		{
			FHyperAIStudioAgentTaskRoute& Route = AgentTaskRoutes.AddDefaulted_GetRef();
			Route.Category = Category;
			Route.AgentName = Agent;
			Route.ModelId = Model;
			Route.Packs = MoveTemp(Packs);
		};
		Add(TEXT("Plan a feature"), {}, TEXT("Claude Code"), SmartModelId);
		Add(TEXT("Materials"), {TEXT("materials_dynamic_material"), TEXT("texture_graph")});
		Add(TEXT("VFX"), {TEXT("niagara_vfx")});
		Add(TEXT("Lighting"), {TEXT("lighting_lookdev")});
		Add(TEXT("Level Design"), {TEXT("level_design"), TEXT("worldbuilding_navigation"), TEXT("pcg"), TEXT("geometry_interchange"), TEXT("mesh_terrain")});
		Add(TEXT("Animation"), {TEXT("anim_blueprint"), TEXT("uaf_graph"), TEXT("animation_rigging"), TEXT("character")});
		Add(TEXT("Gameplay"), {TEXT("blueprint"), TEXT("gameplay_systems"), TEXT("gas"), TEXT("gameplay_ai"), TEXT("enhanced_input")});
		return true;
	}

	/**
	 * Builds the launch argument for Route's selected model. True with an empty OutArgument means "use the agent's
	 * default". False means the route is unsafe to launch with; OutArgument stays empty and OutError says why.
	 * The argument is spliced into a cmd.exe line, so anything that could chain commands or inject a flag is refused.
	 */
	static bool TryResolveModelArgument(const FHyperAIStudioAgentModelRoute& Route, FString& OutArgument, FString& OutError)
	{
		OutArgument.Reset();
		OutError.Reset();
		const FString& Model = Route.SelectedModel;
		if (Model.IsEmpty())
		{
			return true;
		}

		if (!IsSafeModelId(Model))
		{
			OutError = FString::Printf(TEXT("Model \"%s\" is not a plain model id, so it was not passed to the agent."), *Model);
			return false;
		}
		if (!Route.Models.Contains(Model))
		{
			OutError = FString::Printf(TEXT("Model \"%s\" is not in the %s model list."), *Model, *Route.AgentName);
			return false;
		}

		const FString& Format = Route.ModelFlagFormat;
		const FString ShellCharacters = TEXT("&|<>^\"'%!`");
		const int32 Placeholder = Format.Find(TEXT("{0}"), ESearchCase::CaseSensitive);
		bool bFormatSafe = Format.Len() <= 64
			&& Placeholder != INDEX_NONE
			&& Format.Find(TEXT("{0}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Placeholder + 3) == INDEX_NONE;
		for (const TCHAR Char : Format)
		{
			int32 Unused = INDEX_NONE;
			bFormatSafe &= Char >= 0x20 && Char < 0x7F && !ShellCharacters.FindChar(Char, Unused);
		}
		if (!bFormatSafe)
		{
			OutError = FString::Printf(TEXT("The %s model flag format \"%s\" must contain {0} exactly once and no shell characters."), *Route.AgentName, *Format);
			return false;
		}

		OutArgument = Format.Replace(TEXT("{0}"), *Model, ESearchCase::CaseSensitive);
		return true;
	}

	/** A plain model id. It lands in a cmd.exe line, so no shell characters, and no leading '-' that would read as a flag. */
	static bool IsSafeModelId(const FString& Model)
	{
		auto IsAsciiAlnum = [](TCHAR Char)
		{
			return (Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('A') && Char <= TEXT('Z'));
		};
		bool bSafe = !Model.IsEmpty() && Model.Len() <= 128 && IsAsciiAlnum(Model[0]);
		for (const TCHAR Char : Model)
		{
			bSafe &= IsAsciiAlnum(Char) || Char == TEXT('.') || Char == TEXT('_') || Char == TEXT('-')
				|| Char == TEXT(':') || Char == TEXT('/') || Char == TEXT('@');
		}
		return bSafe;
	}

	/**
	 * Claude Code's Smart launch. The opusplan alias runs whatever `opus` names while in plan mode and whatever
	 * `sonnet` names otherwise; these two variables re-point those names. OutPrefix goes inside the agent's own
	 * launch parentheses so only that process sees them, and the quoted `set "NAME=value"` form keeps cmd from
	 * storing the space before `&&` in the value.
	 */
	static bool TryBuildSmartModelLaunch(const FString& PlanModel, const FString& BuildModel, FString& OutPrefix, FString& OutArgument, FString& OutError)
	{
		OutPrefix.Reset();
		OutArgument.Reset();
		OutError.Reset();
		if (!IsSafeModelId(PlanModel) || !IsSafeModelId(BuildModel))
		{
			OutError = FString::Printf(TEXT("Smart plan model \"%s\" and build model \"%s\" must both be plain model ids."), *PlanModel, *BuildModel);
			return false;
		}
		OutPrefix = FString::Printf(
			TEXT("set \"ANTHROPIC_DEFAULT_OPUS_MODEL=%s\" && set \"ANTHROPIC_DEFAULT_SONNET_MODEL=%s\" && "), *PlanModel, *BuildModel);
		OutArgument = TEXT("--model opusplan");
		return true;
	}

	/** Route's launch prefix and model argument, Smart included. Same contract as TryResolveModelArgument. */
	bool TryResolveModelLaunch(const FHyperAIStudioAgentModelRoute& Route, FString& OutPrefix, FString& OutArgument, FString& OutError) const
	{
		OutPrefix.Reset();
		if (Route.SelectedModel != SmartModelId)
		{
			return TryResolveModelArgument(Route, OutArgument, OutError);
		}
		if (!IsSmartModelAgent(Route.AgentName))
		{
			OutArgument.Reset();
			OutError = FString::Printf(TEXT("Smart is only available for Claude Code, not %s."), *Route.AgentName);
			return false;
		}
		return TryBuildSmartModelLaunch(SmartPlanModel, SmartBuildModel, OutPrefix, OutArgument, OutError);
	}

	bool IsBuiltInAgentEnabled(const FString& AgentName) const
	{
		if (AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase))
		{
			return bEnableCodexAgent;
		}
		if (AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase))
		{
			return bEnableClaudeAgent;
		}
		if (AgentName.Contains(TEXT("Gemini"), ESearchCase::IgnoreCase))
		{
			return bEnableGeminiAgent;
		}
		if (AgentName.Contains(TEXT("Cursor"), ESearchCase::IgnoreCase))
		{
			return bEnableCursorAgent;
		}
		if (AgentName.Contains(TEXT("VS Code"), ESearchCase::IgnoreCase)
			|| AgentName.Contains(TEXT("VSCode"), ESearchCase::IgnoreCase)
			|| AgentName.Contains(TEXT("Copilot"), ESearchCase::IgnoreCase))
		{
			return bEnableVSCodeAgent;
		}
		return true;
	}

	void SetBuiltInAgentEnabled(const FString& AgentName, bool bEnabled)
	{
		if (AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase))
		{
			bEnableCodexAgent = bEnabled;
			if (bEnabled)
			{
				bGenerateCodexConfig = true;
			}
			return;
		}
			if (AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase))
			{
				bEnableClaudeAgent = bEnabled;
				if (bEnabled)
				{
					bGenerateClaudeConfig = true;
					bGenerateClaudeMarkdown = true;
				}
				return;
			}
		if (AgentName.Contains(TEXT("Gemini"), ESearchCase::IgnoreCase))
		{
			bEnableGeminiAgent = bEnabled;
			if (bEnabled)
			{
				bGenerateGeminiConfig = true;
			}
			return;
		}
		if (AgentName.Contains(TEXT("Cursor"), ESearchCase::IgnoreCase))
		{
			bEnableCursorAgent = bEnabled;
			if (bEnabled)
			{
				bGenerateCursorConfig = true;
			}
			return;
		}
		if (AgentName.Contains(TEXT("VS Code"), ESearchCase::IgnoreCase)
			|| AgentName.Contains(TEXT("VSCode"), ESearchCase::IgnoreCase)
			|| AgentName.Contains(TEXT("Copilot"), ESearchCase::IgnoreCase))
		{
			bEnableVSCodeAgent = bEnabled;
			if (bEnabled)
			{
				bGenerateVSCodeConfig = true;
			}
		}
	}

	bool EnsureEnabledBuiltInAgentConfigsSelected()
	{
		bool bChanged = false;
		if (bEnableCodexAgent && !bGenerateCodexConfig)
		{
			bGenerateCodexConfig = true;
			bChanged = true;
		}
			if (bEnableClaudeAgent && !bGenerateClaudeConfig)
			{
				bGenerateClaudeConfig = true;
				bChanged = true;
			}
			if (bEnableClaudeAgent && !bGenerateClaudeMarkdown)
			{
				bGenerateClaudeMarkdown = true;
				bChanged = true;
			}
		if (bEnableGeminiAgent && !bGenerateGeminiConfig)
		{
			bGenerateGeminiConfig = true;
			bChanged = true;
		}
		if (bEnableCursorAgent && !bGenerateCursorConfig)
		{
			bGenerateCursorConfig = true;
			bChanged = true;
		}
		if (bEnableVSCodeAgent && !bGenerateVSCodeConfig)
		{
			bGenerateVSCodeConfig = true;
			bChanged = true;
		}
		return bChanged;
	}
};
