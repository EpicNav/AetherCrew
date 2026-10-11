// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class UObject;

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
 * out through Unreal's source control provider (Perforce, Plastic, Git with LFS locks), or locked with the Lore CLI in a
 * Lore workspace on a server when there is no Unreal provider (UE 5.8 ships none for Lore). A file someone else has
 * checked out or locked, or one behind the latest revision, refuses the edit instead, so an agent never works over a
 * teammate. A local Lore repository (no server) has no locks: its files are marked dirty once saved. Files an agent adds
 * are marked for add once saved. Game thread only.
 */
class HYPERAISTUDIO_API FHyperAIStudioSourceControl
{
public:
	/** Before an edit or a save: true when every package may be changed; OutError says who holds one, or what to do. */
	static bool MakeEditable(const TArray<FString>& PackageNames, FString& OutError);
	/**
	 * Saves the asset's package as the editor's Save does: made editable as above, and loaded in full first, since Epic
	 * refuses a package loaded only in part (one another asset needed only a class from). Epic's save raises a refusal
	 * through a fatal error device unless told otherwise; this returns it in OutError.
	 */
	static bool SaveAsset(UObject& Asset, FString& OutError);
	/** unreal (with the provider's name), lore (with the workspace), or none. */
	static FString DescribeBackend();
	/** For a Lore workspace: local (history on this computer, no server) or remote (the server it pushes to); else empty. */
	static FString DescribeLoreMode();
	/** Files agents made editable or added this session, as absolute paths. */
	static TArray<FString> GetAgentFiles();
	/** What an agent's edit of a file with State needs: nothing, a check-out, or a refusal with OutError. */
	enum class EDecision : uint8 { Edit, CheckOut, Refuse };
	static EDecision Decide(const FHyperAIStudioSourceControlFileState& State, const FString& DisplayName, FString& OutError);

	static void Startup();
	static void Shutdown();
#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Runs Lore as Executable with Prefix before its arguments, for the workspace at Root on the server RemoteUrl (empty: a
	 * local repository); empty strings restore detection. Either way the agents' files start over.
	 */
	static void SetLoreForTests(const FString& Executable, const FString& Prefix, const FString& Root, const FString& RemoteUrl = TEXT("lore://127.0.0.1:41337"));
#endif
};
