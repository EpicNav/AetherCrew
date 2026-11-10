// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SWidgetSwitcher;

/**
 * HyperAI Studio's settings, as one page with a list of sections, like the editor's Project Settings:
 * - Setup: readiness, and the button that finishes it.
 * - Agents: the agent CLIs and their files.
 * - MCP servers: the Unreal MCP server and extra servers.
 * - Image services: image models and the keys they need.
 * - Version control: Epic's Lore, set up for the project (local now, a server later).
 * - Preferences: every setting, in a details panel.
 * - Documentation.
 */
class SHyperAIStudioSettingsPage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioSettingsPage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Setup, Agents, Servers, Images, VersionControl, Preferences or Docs. */
	void ShowSection(FName Section);
	FName GetSection() const { return Section; }

private:
	TSharedRef<SWidget> BuildSectionButton(FName Id, const FText& Label, const FText& Description);
	TSharedRef<SWidget> BuildSection(FName Id);
	TSharedRef<SWidget> BuildDetails(TFunction<bool(const FString& Category)> Shows) const;
	/** Google's Nano Banana models: whether the key is set, where to get one, and what each model costs. */
	TSharedRef<SWidget> BuildNanoBanana() const;

	FName Section;
	TArray<FName> Sections;
	TSharedPtr<SWidgetSwitcher> Switcher;
	TMap<FName, int32> Built;
};
