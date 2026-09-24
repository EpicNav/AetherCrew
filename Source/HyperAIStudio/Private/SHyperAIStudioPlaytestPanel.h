// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioVerify.h"
#include "Widgets/SCompoundWidget.h"

class FHyperAIStudioPreviewImage;
class SMultiLineEditableTextBox;
class SVerticalBox;

/**
 * The Playtest tab: cases of bot steps, written by hand or added from templates of what a player does, played all at
 * once in copies of the open level, as hyper_verify_run plays them. Each case's result shows with its steps' evidence
 * and the maps and pictures they made; a picture opens in the Preview tab.
 */
class SHyperAIStudioPlaytestPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioPlaytestPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHyperAIStudioPlaytestPanel() override;

	static FName GetTabName();

	/** The request the panel holds, as hyper_verify_run takes it: a deadline and cases. */
	FString GetCases() const;
	void SetCases(const FString& Json);
	/** Adds a case of one kind: Survey, Reach, Collect, Defeat, Survive, Explore or Duel. */
	void AddTemplate(const FString& Kind);
	/** Plays the cases; false with the reason when they cannot start. */
	bool Run(FString& OutError);
	bool IsRunning() const { return bRunning; }
	const TOptional<HyperAIStudio::Verify::FBatchOutcome>& GetOutcome() const { return Outcome; }

private:
	void Rebuild();

	TSharedPtr<SMultiLineEditableTextBox> CasesBox;
	TSharedPtr<SVerticalBox> Results;
	FText Status;
	bool bRunning = false;
	TOptional<HyperAIStudio::Verify::FBatchOutcome> Outcome;
	TArray<TSharedPtr<FHyperAIStudioPreviewImage>> Thumbnails;
	/** The batch answers on a later frame, and the tab may have closed by then. */
	TSharedRef<bool> bAlive = MakeShared<bool>(true);
};
