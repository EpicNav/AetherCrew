// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** The lore command on this machine. */
struct FHyperAIStudioLoreCli
{
	bool bFound = false;
	/** Where it is, as an absolute path. */
	FString Path;
	/** Its release, such as 0.10.1, and the full build it reports. */
	FString Version;
	FString Build;
	/** Why it could not be used, when it was found but did not answer. */
	FString Error;
};

/** Where a Lore repository keeps its history: on this computer only, or on a server it pushes to. */
enum class EHyperAIStudioLoreMode : uint8
{
	None,
	Local,
	Remote
};

/** A Lore workspace: the folder holding .lore, and what its .lore/config.toml says. */
struct FHyperAIStudioLoreWorkspace
{
	/** The workspace's top folder, without a trailing slash; empty when there is none. */
	FString Root;
	/** remote_url: empty for a local repository (lore repository create --offline). */
	FString RemoteUrl;
	/** Who commits are recorded as, when set. */
	FString Identity;
	EHyperAIStudioLoreMode Mode = EHyperAIStudioLoreMode::None;
};

/** What a project's Lore setup is asked to do. */
struct FHyperAIStudioLoreSetupOptions
{
	/** The project folder that becomes the workspace. */
	FString Folder;
	/** The repository's name: letters, digits, '/', '-', '_' and '.'. */
	FString Name;
	/** Empty for a local repository; otherwise the server, such as lore://127.0.0.1:41337. */
	FString ServerUrl;
	/** Recorded on commits; optional. */
	FString Identity;
	/** Add Unreal's build output and caches to .loreignore. */
	bool bWriteIgnore = true;
	/** Stage everything not ignored and commit it. */
	bool bFirstCommit = true;
	FString CommitMessage;
};

enum class EHyperAIStudioLoreStepKind : uint8
{
	CheckServer,
	WriteIgnore,
	Create,
	Stage,
	Commit,
	Push
};

/** One step of a setup, shown to the user before anything runs. */
struct FHyperAIStudioLoreSetupStep
{
	EHyperAIStudioLoreStepKind Kind = EHyperAIStudioLoreStepKind::Create;
	/** What it does, in a line. */
	FString Title;
	/** What it changes, and where. */
	FString Detail;
	/** The command as a user would type it, or the file written. */
	FString Command;
	/** The lore arguments, for a step the lore command runs. */
	TArray<FString> Arguments;
	/** How to take it back. */
	FString Undo;
	/** Whether it writes in the project folder. */
	bool bChangesFolder = false;
};

/**
 * Epic's Lore version control (https://lore.org, https://github.com/EpicGames/lore) as HyperAI Studio sets it up: the lore
 * command, a project's workspace and whether it is local or on a server, and the steps that make a project folder a Lore
 * workspace. Nothing here signs in or reads tokens: `lore login` is the user's to run in a terminal.
 *
 * Lore 0.10 has two ways to keep a project: a local repository (`lore repository create --offline <name>`), whose
 * history stays in the project's .lore folder and which needs no server, and a repository on a Lore server
 * (`lore repository create lore://host:41337/<name>`), which can push, sync and take file locks. The server is set once,
 * when the repository is made.
 */
class HYPERAISTUDIO_API FHyperAIStudioLore
{
public:
	/** Where to install Lore from. */
	static const TCHAR* InstallGuideUrl;
	/** lore.org, Epic's page for Lore. */
	static const TCHAR* HomeUrl;
	/** The port a loreserver answers on, and its HTTP health check's. */
	static constexpr int32 DefaultServerPort = 41337;

	/** The lore command: the Lore Command setting, else PATH and the folders Lore's installers use. Game thread. */
	static bool FindExecutable(FString& OutPath);
	/** FindExecutable, and `lore --version` (a second at most). Game thread. */
	static FHyperAIStudioLoreCli DetectCli();
	/** Reads `lore --version`'s output: "lore 0.10.1+1476-lore_v0.10.1__urc_main" is 0.10.1, built 1476-lore_v0.10.1__urc_main. */
	static bool ParseVersion(const FString& Output, FString& OutVersion, FString& OutBuild);

	/** The workspace Folder is in: the nearest folder at or above it holding .lore. */
	static FHyperAIStudioLoreWorkspace FindWorkspace(const FString& Folder);
	/** remote_url and identity from a .lore/config.toml. */
	static void ReadWorkspaceConfig(const FString& ConfigToml, FString& OutRemoteUrl, FString& OutIdentity);

	/** A repository name Lore accepts, made from a project's name: other characters become '-'. */
	static FString RepositoryName(const FString& ProjectName);
	static bool IsValidRepositoryName(const FString& Name);
	/** A server address the lore command can use: scheme://host[:port], no spaces or quotes. */
	static bool IsValidServerUrl(const FString& Url, FString& OutError);
	/** The repository's address on a server: the server, then /name. */
	static FString RepositoryUrl(const FString& ServerUrl, const FString& Name);
	/** The command a user runs in a terminal to sign in to a server; HyperAI only shows it. */
	static FString LoginCommand(const FString& ServerUrl);

	/** The .loreignore patterns for an Unreal project: build output, caches, IDE files. Gitignore syntax. */
	static FString UnrealIgnoreText();
	/** Existing .loreignore text with the Unreal patterns it lacks appended; nothing it has is removed or reordered. */
	static FString MergeIgnore(const FString& Existing, bool& bOutChanged);

	/** The steps a setup takes, in order, or false with OutError when the folder cannot be set up as asked. */
	static bool PlanSetup(const FHyperAIStudioLoreSetupOptions& Options, TArray<FHyperAIStudioLoreSetupStep>& OutSteps, FString& OutError);
	/** Runs one planned step. Any thread; a step the lore command runs waits for it, up to a long timeout. */
	static bool RunStep(const FString& Executable, const FHyperAIStudioLoreSetupOptions& Options, const FHyperAIStudioLoreSetupStep& Step, FString& OutText);

	/** Whether a server answers, with `lore repository list <server>`. Any thread; up to TimeoutSeconds. */
	static bool CheckServer(const FString& Executable, const FString& ServerUrl, FString& OutText, double TimeoutSeconds = 20.0);
	/**
	 * The sign-ins the lore command keeps, from `lore auth list` (never with tokens): one line each, or empty when there
	 * are none. Any thread.
	 */
	static bool ListSignIns(const FString& Executable, TArray<FString>& OutIdentities, FString& OutText);
	/** The identities in `lore auth list` output, with anything shaped like a token left out. */
	static TArray<FString> ParseSignIns(const FString& Output);

	/**
	 * Runs the lore command with Arguments in WorkingDirectory, never asking anything (--non-interactive, --no-pager are
	 * added), and stops it after TimeoutSeconds. OutText holds what it printed, trimmed. Any thread.
	 */
	static bool Run(const FString& Executable, const FString& WorkingDirectory, const TArray<FString>& Arguments, double TimeoutSeconds,
		FString& OutText, int32* OutCode = nullptr);
	/** Runs Executable with a ready command line, stdout and stderr together, stopped after TimeoutSeconds. Any thread. */
	static bool RunProcess(const FString& Executable, const FString& Params, const FString& WorkingDirectory, double TimeoutSeconds,
		FString& OutText, int32* OutCode = nullptr);
	/** Arguments joined for a command line, each quoted when it needs to be. */
	static FString JoinArguments(const TArray<FString>& Arguments);
};
