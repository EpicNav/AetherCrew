// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolCallAsyncResultString.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class IStructureDetailsView;
class FStructOnScope;
class SMultiLineEditableTextBox;
class SWrapBox;
class FHyperAIStudioPreviewImage;

/**
 * Any tool, run by hand: its request as a form, Run through Epic's toolset registry (the same call MCP makes), and the
 * answer as readable JSON with every picture it names. A tool without reflected parameters gets a JSON box instead.
 * An edit that waits for approval shows Approve, since the person who pressed Run is the one approving.
 */
class SHyperAIStudioToolForm : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioToolForm) {}
		SLATE_ARGUMENT(FString, Toolset)
		SLATE_ARGUMENT(FString, Tool)
		SLATE_ARGUMENT(FString, Description)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	/** Opens the form for one tool in a window of its own. */
	static void OpenWindow(const FString& Toolset, const FString& Tool, const FString& Description);

	/** The tool's arguments as the form holds them, the JSON Run sends. */
	FString BuildArguments() const;
	/** Sets the arguments from JSON, as a pasted call or a test would. */
	bool SetArguments(const FString& Json, FString& OutError);
	void Run();
	bool IsRunning() const { return Pending.IsValid(); }
	const FString& GetAnswer() const { return Answer; }
	/** The answer as a report: its summary, its fields as tables, and its pictures. Empty when it is not JSON. */
	const FString& GetReport() const { return Report; }
	/** The answer as a report, or as the JSON it is. */
	void ShowJson(bool bJson) { bShowJson = bJson; }
	bool HasReflectedForm() const { return Function != nullptr; }

private:
	/** The JSON shows when asked for, and whenever the answer has no report (before a run, or plain text). */
	bool IsJsonShown() const { return bShowJson || Report.IsEmpty(); }
	/** Picture files named anywhere in the answer, so they can be shown. */
	void FindImages(const TSharedPtr<class FJsonValue>& Value);
	void RebuildImages();
	FString OperationIdOf() const;

	FString Toolset;
	FString Tool;
	UFunction* Function = nullptr;
	/** With one struct parameter, the form shows that struct's fields at the top rather than one closed row. */
	FStructProperty* Single = nullptr;
	TSharedPtr<FStructOnScope> Parameters;
	TSharedPtr<IStructureDetailsView> Details;
	TSharedPtr<SMultiLineEditableTextBox> JsonBox;
	TSharedPtr<SWrapBox> ImageBox;
	TStrongObjectPtr<UToolCallAsyncResultString> Pending;
	double StartedAt = 0.0;
	FString Answer;
	FString Report;
	bool bShowJson = false;
	FString Status;
	bool bStatusIsError = false;
	bool bAwaitingApproval = false;
	TArray<FString> Images;
	TArray<TSharedPtr<FHyperAIStudioPreviewImage>> Thumbnails;
};
