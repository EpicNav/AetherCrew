// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class SDockTab;
class SWidget;

/** One page of HyperAI Studio: a place in the navigation and a tab in the window's own dock area. */
struct FHyperAIStudioPage
{
	FName Id;
	FText Label;
	/** One line on what the page is for, shown on hover and in search. */
	FText Tooltip;
	/** Start, Create, Test, Project or System: the navigation's groups, in that order. */
	FName Group;
	/** Its place within the group. */
	int32 Order = 0;
	/** A brush name in the plugin style set, which falls back to the editor's. */
	FName Icon;
	/** Builds the page's content inside its tab; the tab is given so the page can react to it closing. */
	TFunction<TSharedRef<SWidget>(const TSharedRef<SDockTab>& Tab)> Create;
	/** A number beside the page, such as approvals waiting; 0 shows nothing. */
	TFunction<int32()> Badge;
	/** A page that only opens from somewhere else, such as a second chat, is left out of the navigation. */
	bool bHidden = false;
};

/** Something the search box can do, such as starting a new chat. */
struct FHyperAIStudioAction
{
	FName Id;
	FText Label;
	FText Description;
	FName Icon;
	TFunction<void()> Run;
};

/**
 * HyperAI Studio: the one window every HyperAI feature lives in.
 *
 * It has a header with search and status, a navigation rail that folds to icons in a narrow window, and pages as tabs
 * that can be split, docked side by side or floated, as in any Unreal asset editor. There is also a status bar.
 * Features register pages and actions; packs register theirs when they load. Game thread only.
 */
class HYPERAISTUDIO_API FHyperAIStudioShell
{
public:
	/** The major tab the window lives in. */
	static FName GetTabName();

	static void RegisterPage(const FHyperAIStudioPage& Page);
	static void UnregisterPage(FName Id);
	/** Every page, in navigation order: by group, then order. */
	static TArray<FHyperAIStudioPage> GetPages();
	static const FHyperAIStudioPage* FindPage(FName Id);

	static void RegisterAction(const FHyperAIStudioAction& Action);
	static void UnregisterAction(FName Id);
	static TArray<FHyperAIStudioAction> GetActions();

	/** Opens the window, on PageId when one is named. Returns the page's tab once it is open. */
	static TSharedPtr<SDockTab> Open(FName PageId = NAME_None);
	/** The page's tab when it is open in the window. */
	static TSharedPtr<SDockTab> FindPageTab(FName PageId);
	/** Whether the window is open. */
	static bool IsOpen();

	/** Fires when pages or actions are added or removed. */
	static FSimpleMulticastDelegate& OnChanged();

	/** The module registers the window's tab spawner; nothing else needs to. */
	static void RegisterTabSpawner();
	static void UnregisterTabSpawner();
};
