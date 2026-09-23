// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAgents.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

/** Subagents in the workbench: one definition each in .hyperai/agents, written out for Claude Code and Codex. */
class SHyperAIStudioAgentsPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioAgentsPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** New subagent when Existing is null. */
	static void OpenAgentDialog(const FHyperAIStudioAgentDefinition* Existing = nullptr);

private:
	using FAgentPtr = TSharedPtr<FHyperAIStudioAgentDefinition>;

	void Reload();
	void ShowNotes(const FText& Title, const TArray<FString>& Notes, int32 Changed);
	TSharedRef<ITableRow> GenerateRow(FAgentPtr Agent, const TSharedRef<STableViewBase>& OwnerTable);

	TArray<FAgentPtr> Agents;
	TSharedPtr<SListView<FAgentPtr>> List;
};
