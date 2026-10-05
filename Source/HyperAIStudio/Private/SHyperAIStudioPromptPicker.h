// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioPromptPresets.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class ITableRow;
class SSearchBox;
class STableViewBase;
class SVerticalBox;
class SWrapBox;

/** A preset was picked: its prompt as filled in, and the blanks it still has, each written "[Label]" in the text. */
DECLARE_DELEGATE_ThreeParams(FOnHyperAIStudioPresetChosen, const FHyperAIStudioPromptPreset& /*Preset*/, const FString& /*FilledText*/,
	const TArray<FString>& /*Blanks*/);

/**
 * The prompt library to start a message from. Left: search, then what fits the selection, what was used last, and the
 * categories with their counts. Middle: the presets, greyed when a pack they need is off. Right: the one chosen, with
 * its agent, skills and tools, a field per blank, and the prompt as it will read. Picking only puts the prompt in the
 * composer; nothing is sent. Arrows move through the list, Enter picks, Esc closes.
 */
class SHyperAIStudioPromptPicker : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioPromptPicker) {}
		/** The project whose own presets join the plugin's; empty for the open project. */
		SLATE_ARGUMENT(FString, ProjectRoot)
		/** What the presets fill from; unset reads the editor's selection as the picker opens. */
		SLATE_ARGUMENT(TOptional<HyperAIStudio::PromptPresets::FPromptContext>, Context)
		SLATE_EVENT(FOnHyperAIStudioPresetChosen, OnPresetChosen)
		/** Esc: the picker's owner closes it. */
		SLATE_EVENT(FSimpleDelegate, OnClose)
	SLATE_END_ARGS()

	/** The category chips that are not categories. */
	static const TCHAR* const ForSelection;
	static const TCHAR* const RecentCategory;

	void Construct(const FArguments& InArgs);
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	/** Where typing goes as the picker opens. */
	TSharedPtr<SWidget> GetSearchBox() const;
	/** Searches as typing Query would. */
	void SetSearchText(const FString& Query);
	/** Empty for every preset, ForSelection, RecentCategory, or a category id. */
	void SetCategory(const FString& CategoryId);
	int32 NumShown() const { return Shown.Num(); }
	const FHyperAIStudioPromptPreset* GetShown(int32 Index) const;
	const FHyperAIStudioPromptPreset* GetSelected() const;
	/** The chosen preset's prompt as it would go in now, its fields and selection filled in; without defaults, untyped fields stay blanks. */
	FString GetFilledText(TArray<FString>* OutBlanks = nullptr, bool bUseDefaults = true) const;
	/** Picks the chosen preset, as Enter does; false when none is chosen. */
	bool Choose(bool bUseDefaults = true);

private:
	using FPresetPtr = TSharedPtr<FHyperAIStudioPromptPreset>;

	void Refresh();
	void Select(const FPresetPtr& Preset);
	void Move(int32 Step);
	void RebuildDetails();
	TSharedRef<SWidget> BuildCategories();
	TSharedRef<SWidget> CategoryChip(const FString& Id, const FText& Label, int32 Count);
	TSharedRef<ITableRow> MakeRow(FPresetPtr Preset, const TSharedRef<STableViewBase>& Owner);
	FReply OnSearchKey(const FGeometry& Geometry, const FKeyEvent& KeyEvent);
	int32 CountIn(const FString& CategoryId) const;
	bool Fits(const FHyperAIStudioPromptPreset& Preset) const;

	HyperAIStudio::PromptPresets::FLibrary Library;
	HyperAIStudio::PromptPresets::FPromptContext Context;
	/** Packs with a tool enabled as the picker opened. */
	TSet<FString> EnabledPacks;
	TArray<FPresetPtr> All;
	TArray<FPresetPtr> Shown;
	FString Query;
	FString CategoryId;
	FPresetPtr Selected;
	/** What each field holds, and the fields the user typed in. */
	TMap<FString, FString> Values;
	TSet<FString> Edited;

	FOnHyperAIStudioPresetChosen OnPresetChosen;
	FSimpleDelegate OnClose;
	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SListView<FPresetPtr>> List;
	TSharedPtr<SVerticalBox> Fields;
	TSharedPtr<SWrapBox> Tags;
};
