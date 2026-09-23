// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** Exact tool-name/class binding used only after generated-catalog admission resolves true. */
struct FHyperAIStudioDiagnosticsToolBinding
{
	FString ToolName;
	FString QualifiedToolset;
	UClass* ToolsetClass = nullptr;
};

/** Owns three independent diagnostics source-candidate registrations and their bounded services. */
class FHyperAIStudioDiagnosticsRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;

	static const TArray<FHyperAIStudioDiagnosticsToolBinding>& GetBindings();
	static bool IsPendingTestRegistrationEnabled();
	static bool IsRegistrationAllowed(const FString& ToolName, bool bAllowPendingForTests);

private:
	void RegisterAfterEngineInit();
	void RefreshMcpToolsIfSafe() const;

	FDelegateHandle PostEngineInitHandle;
	TArray<UClass*> OwnedToolsets;
	bool bStarted = false;
	bool bAuditStoreStarted = false;
	bool bLogBufferStarted = false;
};
