// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioNiagaraToolset.h"

class UNiagaraScript;
class UNiagaraSystem;

/**
 * The only door to UE 5.8's UNiagaraExternalEditUtilities, which Epic marks experimental and subject to change.
 * Nothing here exposes an FNiagaraExt_* type, so when that API moves the fix stays inside the gate's .cpp.
 * Every function must run on the game thread and builds its own edit context for the duration of the call.
 */
namespace HyperAIStudio::Niagara::ExternalEditGate
{
	static constexpr int32 MaxTopologyModules = 512;
	static constexpr int32 MaxInputNamesPerModule = 64;
	static constexpr int32 MaxInputNameDepth = 8;
	/** hlsl_expression, data_interface JSON and curve keys. */
	static constexpr int32 MaxLongValueChars = 8192;

	struct FDiagnostics
	{
		bool bCompileStateKnown = false;
		bool bCompiling = false;
		bool bCompileHasErrors = false;
		bool bCompileHasWarnings = false;
		/** Undismissed stack errors only; the edit baseline compares this. */
		int32 StackErrorCount = 0;
		/** Stack issues plus compile events, counted before the Issues list is truncated. */
		int32 ErrorCount = 0;
		int32 WarningCount = 0;
		int32 InfoCount = 0;
		/** Undismissed stack issues, then compile events at warning or error severity. */
		TArray<FHyperAINiagaraIssue> Issues;
		bool bTruncated = false;
	};

	bool ReadTopology(
		UNiagaraSystem& System,
		TArray<FHyperAINiagaraEmitterTopology>& OutEmitters,
		bool& bOutTruncated,
		FString& OutError,
		bool bIncludeValues = false);

	bool ReadDiagnostics(UNiagaraSystem& System, int32 MaxIssues, FDiagnostics& OutDiagnostics, FString& OutError);

	/**
	 * Checks one op's shape. With bResolveAssets it also resolves the asset or class the op names, loading a module
	 * script or emitter template if needed. It never loads or resolves the Niagara System being edited.
	 */
	bool ValidateOp(const FHyperAINiagaraEditOp& Op, bool bResolveAssets, FString& OutStatus, FString& OutDiagnostic);

	/**
	 * Applies ops in order inside one undo transaction. Stops at the first failure; OutAppliedCount is how many ops
	 * landed before it, which decides whether the failure left an effect behind.
	 */
	bool ApplyOps(
		UNiagaraSystem& System,
		const TArray<FHyperAINiagaraEditOp>& Ops,
		int32& OutAppliedCount,
		TArray<FString>& OutPerOpStatus,
		FString& OutError);

	/** False if the experimental API's reflected class is missing from this engine build. */
	bool IsApiAvailable();

	/** The plan makes its target: its first op is create_system. */
	bool IsCreatePlan(const TArray<FHyperAINiagaraEditOp>& Ops);

	/** create_system's value: the emitter names to keep, in order, without blanks or repeats. */
	TArray<FName> ParseEmitterList(const FString& Value);

	/**
	 * Makes the System at TargetPath (which must be absent) with Epic's CreateNiagaraSystem: a copy of the template
	 * System create_system names, or an empty System, then removes the emitters its value does not keep. The asset
	 * is registered and dirty; it is not saved and its creation is not an undo step.
	 */
	UNiagaraSystem* CreateSystem(const FString& TargetPath, const FHyperAINiagaraEditOp& Op, FString& OutError);

	/** A module's or dynamic input's inputs (name, value_type, category, tooltip) and outputs, read from the asset. */
	bool ReadScriptSchema(const UNiagaraScript& Script, TArray<FHyperAINiagaraInputSchema>& OutInputs, TArray<FString>& OutOutputs, FString& OutError);

	/** Library dynamic input scripts whose output is ValueType (float, vector, color...). */
	bool ListDynamicInputs(const FString& ValueType, TArray<FString>& OutPaths, FString& OutError);
}
