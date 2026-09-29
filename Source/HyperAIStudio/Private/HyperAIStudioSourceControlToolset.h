// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioSourceControlToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAISourceControlFile
{
	GENERATED_BODY()

	/** Relative to the project. */
	UPROPERTY() FString Path;
	/** checked_out, added, locked, modified, current, checked_out_by_other, not_current, or unknown. */
	UPROPERTY() FString State;
	/** Who else holds it, when someone does. */
	UPROPERTY() FString Who;
};

USTRUCT(BlueprintType)
struct FHyperAISourceControlStatusRequest
{
	GENERATED_BODY()

	/** Ask the server again rather than read what the editor last heard. */
	UPROPERTY() bool bRefresh = true;
};

USTRUCT(BlueprintType)
struct FHyperAISourceControlStatusReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** unreal (with the provider), lore (with the workspace), or none. */
	UPROPERTY() FString Backend;
	UPROPERTY() FString Diagnostic;
	/** The files agents made editable or added in this editor session. */
	UPROPERTY() TArray<FHyperAISourceControlFile> Files;
	/** For Lore: what lore status says, trimmed. */
	UPROPERTY() FString LoreStatus;
};

USTRUCT(BlueprintType)
struct FHyperAISourceControlSubmitRequest
{
	GENERATED_BODY()

	/** What the change does, for the team's history. */
	UPROPERTY() FString Description;
	/** Files relative to the project; empty submits every file agents changed this session. */
	UPROPERTY() TArray<FString> Files;
	/** Names the submit; made when empty. Follow it with hyper_operation_wait (bWaitThroughApproval). */
	UPROPERTY() FString OperationId;
	/** Only list what would be submitted. */
	UPROPERTY() bool bDryRun = true;
};

USTRUCT(BlueprintType)
struct FHyperAISourceControlSubmitReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked (a dry run), awaiting_user_approval, or why it was refused. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Backend;
	UPROPERTY() TArray<FString> Files;
};

/** Agent edits under the project's source control: what they changed, and submitting it once the user approves. */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioSourceControlToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * The project's source control (Unreal's provider, or the Lore CLI in a Lore workspace) and the files agents checked
	 * out, locked or added this session, each with its state. Agent edits check assets out on their own; an asset a
	 * teammate holds refuses the edit, naming them.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|SourceControl")
	static FHyperAISourceControlStatusReport hyper_source_control_status(const FHyperAISourceControlStatusRequest& Request);

	/**
	 * Submits (checks in, or with Lore stages, commits and pushes) the files agents changed, with a description. It
	 * reaches the team, so it always waits for the user's approval; dry-run first (the default) to list the files.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|SourceControl")
	static FHyperAISourceControlSubmitReport hyper_source_control_submit(const FHyperAISourceControlSubmitRequest& Request);
};
