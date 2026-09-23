// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioSkills.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

/** Skills in the workbench: the project's, shared by Claude Code and Codex, and the user's personal ones. */
class SHyperAIStudioSkillsPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioSkillsPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	using FSkillPtr = TSharedPtr<FHyperAIStudioSkill>;

	void Reload();
	void ShowNotes(const FText& Title, const TArray<FString>& Notes, int32 Changed);
	TSharedRef<ITableRow> GenerateRow(FSkillPtr Skill, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<SWidget> BuildExportMenu(FString Folder);
	void ImportFrom(const FString& Source);

	TArray<FSkillPtr> Skills;
	TSharedPtr<SListView<FSkillPtr>> List;
};
