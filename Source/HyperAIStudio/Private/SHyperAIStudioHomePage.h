// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioService.h"
#include "Widgets/SCompoundWidget.h"

class SWrapBox;

/**
 * HyperAI Studio's first page.
 * - Setup: whether it is finished, with the button that finishes it.
 * - State: the MCP server, the agents that can work here, and what waits on the user.
 * - Actions: tiles that start the common tasks.
 */
class SHyperAIStudioHomePage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioHomePage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Reads the status and the agents again, as opening the page does. */
	void Refresh();
	bool IsReady() const { return bReady; }

private:
	TSharedRef<SWidget> BuildStatus();
	TSharedRef<SWidget> BuildAgent(const FString& Id, const FText& Name) const;
	TSharedRef<SWidget> BuildActions();
	TSharedRef<SWidget> Tile(FName Icon, const FText& Title, const FText& Body, TFunction<void()> Run) const;

	TSharedPtr<FHyperAIStudioService> Service;
	FHyperAIStudioStatus Status;
	bool bHasStatus = false;
	bool bReady = false;
	TArray<FHyperAIStudioPrerequisiteStatus> Prerequisites;
	int32 Approvals = 0;
	int32 Running = 0;
};
