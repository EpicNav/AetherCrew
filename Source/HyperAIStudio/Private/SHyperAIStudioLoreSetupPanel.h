// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioLore.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;

/**
 * Settings > Version control: Epic's Lore for this project. It finds the lore command (and says where to install it), shows
 * whether the project is a Lore workspace and whether its repository is local or on a server, and sets one up: every step
 * is listed with its command and how to undo it, and nothing runs until the user confirms. A server and sign-in are for
 * later (online): the Lore Server setting, a check that the server answers, and the lore login command for the user to
 * run in a terminal. HyperAI never signs in and never reads tokens.
 */
class SHyperAIStudioLoreSetupPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioLoreSetupPanel) {}
		/** The folder to set up; the project's by default. */
		SLATE_ARGUMENT(FString, Folder)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Reads the lore command, the workspace and the sign-ins again. */
	void Refresh();
	/** The setup's options as the page holds them. */
	FHyperAIStudioLoreSetupOptions GetOptions() const;
	/** The steps the setup would take now, or none with the reason in GetPlanError. */
	const TArray<FHyperAIStudioLoreSetupStep>& GetPlan() const { return Plan; }
	const FString& GetPlanError() const { return PlanError; }
	bool IsRunning() const { return bRunning; }
	/** What the last setup said when it finished. */
	const FText& GetResult() const { return Result; }
	/** How many of the plan's steps finished, and how many failed. */
	void CountSteps(int32& OutDone, int32& OutFailed) const;
#if WITH_DEV_AUTOMATION_TESTS
	/** Runs the plan as the confirmed Set up does, without the question: tests answer it. */
	void RunPlanForTests() { RunPlan(); }
#endif

private:
	enum class EStepState : uint8 { Waiting, Running, Done, Failed, Skipped };
	enum class ECheck : uint8 { Unknown, Checking, Answered, Failed };

	TSharedRef<SWidget> BuildCliCard();
	TSharedRef<SWidget> BuildProjectCard();
	TSharedRef<SWidget> BuildSetupCard();
	TSharedRef<SWidget> BuildOnlineCard();
	void Replan();
	void RebuildSteps();
	FReply OnSetUp();
	/** Runs the plan's steps one by one on a worker thread, stopping at the first that fails. */
	void RunPlan();
	FReply OnCheckServer();
	void OnStepFinished(int32 Index, bool bOk, const FString& Text);
	void OnSetupFinished(bool bOk);

	FString Folder;
	FHyperAIStudioLoreCli Cli;
	FHyperAIStudioLoreWorkspace Workspace;
	TArray<FString> SignIns;

	FString Name;
	bool bUseServer = false;
	FString ServerUrl;
	FString Identity;
	bool bWriteIgnore = true;
	bool bFirstCommit = true;
	FString CommitMessage;

	TArray<FHyperAIStudioLoreSetupStep> Plan;
	FString PlanError;
	TArray<EStepState> StepStates;
	TArray<FString> StepOutputs;
	bool bRunning = false;
	FText Result;

	ECheck ServerCheck = ECheck::Unknown;
	FString ServerCheckText;

	TSharedPtr<SVerticalBox> StepList;
};
