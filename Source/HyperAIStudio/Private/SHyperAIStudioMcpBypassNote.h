// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SHyperAIStudioMarkdown;
class SVerticalBox;
class SWindow;

/**
 * Activity's note on other MCP servers: while a server or plugin that changes this editor outside HyperAI is found (and
 * not silenced), it says that these approvals do not cover it, with a Details button. Collapsed otherwise.
 */
class SHyperAIStudioMcpBypassNote : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioMcpBypassNote) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHyperAIStudioMcpBypassNote() override;

	/** What it says now; empty while it is hidden. */
	FText GetText() const { return Text; }

private:
	void Refresh();

	FText Text;
	FDelegateHandle ChangedHandle;
};

/**
 * Settings > Agents: every other MCP server the agents have and this editor's plugins and settings that act like one,
 * whether each changes this editor outside HyperAI, where each is configured and how to remove it (as text: HyperAI
 * never edits those files), and "Don't warn again" / "Warn again".
 */
class SHyperAIStudioMcpBypassPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioMcpBypassPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHyperAIStudioMcpBypassPanel() override;

private:
	void Rebuild();

	TSharedPtr<SVerticalBox> List;
	FDelegateHandle ChangedHandle;
};

/** The details window: every finding as a report, with Check again and Close. */
class SHyperAIStudioMcpBypassDetails : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioMcpBypassDetails) {}
		SLATE_ARGUMENT(TSharedPtr<SWindow>, Window)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHyperAIStudioMcpBypassDetails() override;

private:
	void Refresh();

	TWeakPtr<SWindow> Window;
	TSharedPtr<SHyperAIStudioMarkdown> Markdown;
	FDelegateHandle ChangedHandle;
};
