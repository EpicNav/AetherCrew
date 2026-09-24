// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioVerify.h"
#include "Widgets/SCompoundWidget.h"

class FStructOnScope;
class IStructureDetailsView;
class SMultiLineEditableTextBox;
class SVerticalBox;

/**
 * The Playtest page: cases of bot steps, written by hand or added from templates of what a player does, played all at
 * once in copies of the open level, as hyper_verify_run plays them. Each case's result shows with its steps' evidence
 * and the maps and pictures they made; a picture opens in the Match page.
 */
class SHyperAIStudioPlaytestPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioPlaytestPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHyperAIStudioPlaytestPanel() override;

	/** The request the panel holds, as hyper_verify_run takes it: a deadline and cases. */
	FString GetCases() const;
	void SetCases(const FString& Json);
	/** Adds a case of one kind: Survey, Reach, Collect, Defeat, Survive, Explore or Duel. */
	void AddTemplate(const FString& Kind);
	/** The cases as a form, or as the JSON hyper_verify_run takes; false, saying why, when the JSON is not a request. */
	bool ShowJson(bool bInJson);
	bool IsJsonShown() const { return bJson; }
	/** Plays the cases; false with the reason when they cannot start. */
	bool Run(FString& OutError);
	bool IsRunning() const { return bRunning; }
	const TOptional<HyperAIStudio::Verify::FBatchOutcome>& GetOutcome() const { return Outcome; }
	/** Shows a batch's results, as the end of a run does. */
	void ShowOutcome(const HyperAIStudio::Verify::FBatchOutcome& InOutcome);

	/**
	 * A batch as a report: how many cases passed and how long it took, a table of the cases, then each case's steps
	 * and the maps and pictures they made.
	 */
	static FString BuildReport(const HyperAIStudio::Verify::FBatchOutcome& Batch);

	/** A request as the JSON view shows it: fields left at their defaults are left out, so a case reads as written. */
	static FString RequestToJson(const struct FHyperAIVerifyRequest& Request);

private:
	void Rebuild();
	/** Reads the JSON into the form; false, with why, when it is not a request. */
	bool JsonToForm(FText& OutWhy);
	/** Writes the form back as JSON, leaving out what is at its default. */
	void FormToJson();
	void FormToJsonOnChange(const struct FPropertyChangedEvent& Event);

	TSharedPtr<SMultiLineEditableTextBox> CasesBox;
	/** The request the form edits; the JSON in CasesBox follows it. */
	TSharedPtr<FStructOnScope> FormRequest;
	TSharedPtr<IStructureDetailsView> Form;
	bool bJson = false;
	TSharedPtr<SVerticalBox> Results;
	FText Status;
	bool bRunning = false;
	TOptional<HyperAIStudio::Verify::FBatchOutcome> Outcome;
	FString Report;
	/** The batch answers on a later frame, and the tab may have closed by then. */
	TSharedRef<bool> bAlive = MakeShared<bool>(true);
};
