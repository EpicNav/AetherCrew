// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioCredentials.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;

/**
 * Settings > Agents: the user's own keys and addresses for each agent and provider, kept in Windows Credential
 * Manager. Values are shown masked, can be tested with a free authenticated call, and removed.
 */
class SHyperAIStudioKeysPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioKeysPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	struct FRow
	{
		FHyperAIStudioKeySlot Slot;
		/** What the list shows: the masked key, the address, or empty when nothing is stored. */
		FString Shown;
		bool bStored = false;
		/** Nothing stored, but the environment variable is set. */
		bool bFromEnvironment = false;
	};

	void Rebuild();
	TSharedRef<SWidget> BuildRow(const FRow& Row);
	void EditSlot(const FHyperAIStudioKeySlot& Slot);

	TSharedPtr<SVerticalBox> Rows;
	/** The last Test's answer per slot. */
	TMap<FString, TPair<bool, FString>> Results;
};
