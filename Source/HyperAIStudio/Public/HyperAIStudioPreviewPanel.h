// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** How a measurement reads at a glance: plain, close enough, or worth acting on. */
enum class EHyperAIStudioPreviewTone : uint8
{
	Neutral,
	Good,
	Warning
};

struct FHyperAIStudioPreviewStat
{
	FText Label;
	FText Value;
	EHyperAIStudioPreviewTone Tone = EHyperAIStudioPreviewTone::Neutral;
};

/** One earlier attempt, shown in the strip under the comparison. */
struct FHyperAIStudioPreviewFrame
{
	/** Full path to a readable image. */
	FString ImagePath;
	FText Label;
	FText Tooltip;
	EHyperAIStudioPreviewTone Tone = EHyperAIStudioPreviewTone::Neutral;
};

/** Two images to show together, and whatever the source that produced them measured about the pair. */
struct FHyperAIStudioPreviewComparison
{
	/** Full paths to readable images; B may be empty when a source has only one. */
	FString ImagePathA;
	FString ImagePathB;
	FText LabelA;
	FText LabelB;
	TArray<FHyperAIStudioPreviewStat> Stats;
	/** Equal-length brightness histograms, drawn over each other; leave empty to hide the plot. */
	TArray<float> HistogramA;
	TArray<float> HistogramB;
	/** Earlier attempts, oldest first, as a strip under the comparison; clicking one shows it as B. */
	TArray<FHyperAIStudioPreviewFrame> Frames;
	/** Shown across the top while the comparison cannot be trusted, with one button that fixes it. */
	FText Warning;
	FText WarningActionLabel;
	FSimpleDelegate WarningAction;
};

/** Fills OutComparison for the chosen reference image, or explains why it could not. */
DECLARE_DELEGATE_RetVal_ThreeParams(bool, FHyperAIStudioPreviewRefresh,
	const FString& /*ReferenceImagePath*/, FHyperAIStudioPreviewComparison& /*OutComparison*/, FString& /*OutError*/);

/**
 * The preview tab shows two images together with the numbers behind them. It knows nothing about lighting,
 * materials or levels: optional packs register a source that fills a comparison when asked.
 */
class HYPERAISTUDIO_API FHyperAIStudioPreviewPanel
{
public:
	/** Work an agent can do toward the chosen reference, offered on the Match page under Match it. */
	struct FAgentAction
	{
		FName Id;
		FText Label;
		FText Tooltip;
		/** Starts the work for this reference: true once it is under way, or false with OutError set. */
		TFunction<bool(const FString& /*ReferenceImagePath*/, FString& /*OutError*/)> Start;
	};

	struct FSource
	{
		FName Id;
		FText Label;
		FText Tooltip;
		/** True when the source measures a reference image the user picks, rather than producing both itself. */
		bool bNeedsReference = false;
		FHyperAIStudioPreviewRefresh Refresh;
		/** Agent work this source offers; none hides Match it. */
		TArray<FAgentAction> AgentActions;
		/** Cheap enough to refresh every few seconds while an agent works on the reference, so its attempts appear as they land. */
		bool bRefreshWhileAgentWorks = false;
	};

	/** Replaces any source already registered under this id. */
	static void RegisterSource(FSource Source);
	static void UnregisterSource(FName Id);
	static TArray<FSource> GetSources();
	static const FSource* FindSource(FName Id);
	/** Broadcast when a pack registers or withdraws a source, so a live panel can rebuild its list. */
	static FSimpleMulticastDelegate& OnSourcesChanged();

	/** Opens or focuses the Match page of HyperAI Studio; a source id and reference select what it shows. */
	static void Open(FName SourceId = NAME_None, const FString& ReferenceImagePath = FString());

	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnOpenRequested, FName /*SourceId*/, const FString& /*ReferenceImagePath*/);
	static FOnOpenRequested& OnOpenRequested();

	/** PNG, JPG, BMP or TGA inside the project folder, which is all the image tools accept. */
	static bool IsSupportedProjectImage(const FString& Path, FString& OutError);

	/** Starts one of a source's agent actions, as Match it does. The action decides what to show while it works. */
	static bool StartAgentAction(FName SourceId, FName ActionId, const FString& ReferenceImagePath, FString& OutError);
};
