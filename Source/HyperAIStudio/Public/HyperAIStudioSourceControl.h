// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** What source control says about one file, as far as an agent's edit is concerned. */
struct FHyperAIStudioSourceControlFileState
{
	bool bSourceControlled = false;
	bool bCheckedOutOther = false;
	/** Who holds it, when someone else does. */
	FString Who;
	bool bCurrent = true;
	bool bDeleted = false;
	bool bCanCheckOut = false;
};

/**
 * Agent edits under source control. Before an agent changes an asset, its file is made editable the team's way: checked
 * out through Unreal's source control provider (Perforce, Plastic, Git with LFS locks, or Lore through its Unreal
 * plugin), or locked with the Lore CLI in a Lore workspace that has no Unreal provider. A file someone else has checked
 * out or locked, or one behind the latest revision, refuses the edit instead, so an agent never works over a teammate.
 * Files an agent adds are marked for add once saved. Game thread only.
 */
class HYPERAISTUDIO_API FHyperAIStudioSourceControl
{
public:
	/** Before an edit or a save: true when every package may be changed; OutError says who holds one, or what to do. */
	static bool MakeEditable(const TArray<FString>& PackageNames, FString& OutError);
	/** unreal (with the provider's name), lore (with the workspace), or none. */
	static FString DescribeBackend();
	/** Files agents made editable or added this session, as absolute paths. */
	static TArray<FString> GetAgentFiles();
	/** What an agent's edit of a file with State needs: nothing, a check-out, or a refusal with OutError. */
	enum class EDecision : uint8 { Edit, CheckOut, Refuse };
	static EDecision Decide(const FHyperAIStudioSourceControlFileState& State, const FString& DisplayName, FString& OutError);

	static void Startup();
	static void Shutdown();
#if WITH_DEV_AUTOMATION_TESTS
	/** Runs Lore as Executable with Prefix before its arguments, for the workspace at Root; empty strings restore detection. Either way the agents' files start over. */
	static void SetLoreForTests(const FString& Executable, const FString& Prefix, const FString& Root);
#endif
};
