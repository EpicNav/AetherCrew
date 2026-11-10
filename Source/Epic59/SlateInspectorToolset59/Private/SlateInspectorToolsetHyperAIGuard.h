// Games by Hyper 2026.

// UE 5.8: HyperAI Studio addition, not in Epic's 5.9 source. SlateInspectorToolset drives editor UI with synthetic
// Slate input, so an agent could otherwise press HyperAI Studio's own Approve buttons (issuing a grant the user never
// gave), type into its composer, or close its dialogs. This guard keeps the toolset's input away from HyperAI Studio:
//  - refs that resolve into HyperAI Studio's widgets are refused (ResolveRefOrWarn / ResolveRefOrRefuse);
//  - key, char and analog input is refused while keyboard focus is inside HyperAI Studio;
//  - window actions are refused on HyperAI Studio's windows;
//  - while a tool runs, a Slate input pre-processor drops any pointer, wheel, touch or key event whose target
//    (the widget under the pointer, the pointer captor, or the focused widget) belongs to HyperAI Studio, so a
//    coordinate-based tool (MouseMove, MouseUp, TouchMove, TouchEnd, Drag) cannot reach it either;
//  - snapshots render HyperAI Studio's widgets as a single hidden node, so no refs are minted for them.
// HyperAI Studio's widgets are recognised by where they were created (SWidget::GetCreatedInLocation, a source file of
// the HyperAIStudio* modules, whose files are all named HyperAI* or SHyperAI*) or by their type name; a widget is
// protected when it, an ancestor or its window is. Toast notifications (some of HyperAI's carry Discard and Stop
// buttons) are built by the engine, so every notification window is protected. Screenshots are not redacted.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class SWidget;
class SWindow;

namespace UE::SlateInspectorToolset::HyperAIGuard
{
	/** The refusal reason the tools record for a protected target. */
	FString GetRefusalReason();

	/** True when the widget itself was created by HyperAI Studio's source (no ancestor walk). */
	bool IsHyperAIStudioCreated(const SWidget& Widget);

	/** True for HyperAI Studio's own windows and for notification (toast) windows. */
	bool IsProtectedWindow(const TSharedPtr<SWindow>& Window);

	/** True when Widget, any ancestor, or its window is protected. */
	bool IsProtectedWidget(const TSharedPtr<SWidget>& Widget);

	/** True when the topmost widget path under ScreenPoint holds a protected widget. */
	bool IsProtectedPoint(const FVector2D& ScreenPoint);

	/** True when the Slate user's focused widget (keyboard focus for INDEX_NONE) is protected. */
	bool IsFocusProtected(int32 SlateUserIndex);

	/** While any scope is alive, synthetic input headed to HyperAI Studio is dropped. Scopes nest. */
	class FInjectionScope
	{
	public:
		FInjectionScope();
		~FInjectionScope();
		UE_NONCOPYABLE(FInjectionScope)
	};

	/** Removes the input pre-processor; called from the module's shutdown. */
	void Shutdown();
}
