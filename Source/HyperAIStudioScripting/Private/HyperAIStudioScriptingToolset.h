// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioScriptingToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIScriptRunRequest
{
	GENERATED_BODY()

	/** Python that defines run() and returns a JSON-serializable dict; import unreal for the editor API. print() is captured. */
	UPROPERTY() FString Code;
	/** 1-60 s. Checked between Python lines, so one long engine call runs to its end. */
	UPROPERTY() int32 TimeoutSeconds = 10;
	/** Names the run; generated when empty. A script that needs approval is followed with hyper_operation_wait. */
	UPROPERTY() FString OperationId;
	/** Only check the script: whether it would run at once or wait for approval, and why. */
	UPROPERTY() bool bDryRun = false;
};

USTRUCT(BlueprintType)
struct FHyperAIScriptRunReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, failed, timed_out, checked, awaiting_user_approval, invalid_script, or unavailable. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	/** undoable (runs at once inside one undo step) or needs_approval. */
	UPROPERTY() FString Safety;
	/** Why the script waits for approval: imports, calls or file writes an undo cannot take back. */
	UPROPERTY() TArray<FString> EscalationReasons;
	/** run()'s return value as JSON. */
	UPROPERTY() FString ResultJson;
	/** print() output, last 20 KB. */
	UPROPERTY() FString Output;
	UPROPERTY() FString Traceback;
	/** A failed or timed-out undoable run's edits were undone. */
	UPROPERTY() bool bRolledBack = false;
	UPROPERTY() bool bTimedOut = false;
	/** Packages the run changed and left unsaved. */
	UPROPERTY() TArray<FString> DirtyPackages;
	UPROPERTY() float Seconds = 0.f;
	/** Holds script.py and result.json, relative to the project. */
	UPROPERTY() FString ScriptFolder;
};

/** Editor Python for the long tail no typed tool covers, guarded and undoable by default. */
UCLASS()
class HYPERAISTUDIOSCRIPTING_API UHyperAIStudioScriptingToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Runs editor Python (import unreal) for work no other tool covers: define run() returning a dict. It runs at
	 * once in one undo step, undone if it raises or passes timeout_seconds. Scripts that import system modules,
	 * delete, rename, import files, load levels or write files wait for the user's approval (then
	 * hyper_operation_wait). A best-effort guard, not a sandbox.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Scripting")
	static FHyperAIScriptRunReport hyper_script_run(const FHyperAIScriptRunRequest& Request);
};

class FHyperAIStudioScriptingContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("scripting");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioscriptingtoolset.v1");
	static constexpr const TCHAR* ToolName = TEXT("hyper_script_run");
	static constexpr int32 MaxCodeCharacters = 64 * 1024;
	static constexpr int32 MaxTimeoutSeconds = 60;
	static constexpr int32 MaxResultCharacters = 64 * 1024;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	/** The static check: whether the script defines run() and why it would need approval. */
	static bool Analyze(const FString& Code, bool& bOutHasRun, TArray<FString>& OutReasons, FString& OutError);
	static FHyperAIScriptRunReport Run(const FHyperAIScriptRunRequest& Request);
};

class FHyperAIStudioScriptingRegistration final
{
public:
	void Startup();
	void Shutdown();

private:
	void RegisterAfterEngineInit();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
