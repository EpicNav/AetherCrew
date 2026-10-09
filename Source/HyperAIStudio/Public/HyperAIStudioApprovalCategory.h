// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * What an approval request is, for auto-approval: the user chooses which of these run without a click.
 * Plain C++ on purpose: nothing here is reflected, so no script, tool or config can name it.
 */
enum class EHyperAIStudioApprovalCategory : uint8
{
	/** Undoable asset and level changes, playtests, and scripts the runner proves undoable. */
	Edits,
	/** hyper_script_run scripts the runner could not prove undoable; they run without the undo step or the guard. */
	Scripts,
	/** Plans whose sealed variant deletes or replaces; each needs the editor's one-time grant. */
	Destructive,
	/** Paid services, the network and source-control submits; each needs the editor's one-time grant. */
	ExternalEffects
};

/** edits, scripts, destructive, external_effects: for logs and the status agents read. */
HYPERAISTUDIO_API const TCHAR* LexToString(EHyperAIStudioApprovalCategory Category);
