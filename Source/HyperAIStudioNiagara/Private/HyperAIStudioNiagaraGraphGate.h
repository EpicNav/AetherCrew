// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioNiagaraToolset.h"

class UNiagaraSystem;

/**
 * Niagara edits the experimental external-edit API cannot make: simulation stages and their module stacks, scratch
 * pad modules built around a Custom HLSL node, and emitter and renderer properties. Each is built from exported
 * engine classes the way the Niagara editor builds it, with Modify and PostEditChange on every object it touches, so
 * the caller's one transaction undoes and redoes all of it. Nothing here uses an FNiagaraExt_* type.
 *
 * Epic's own JSON setters (SetEmitterData, SetRendererData) write properties without Modify or PostEditChange, so
 * their edits skip undo and leave renderers and compiled scripts stale; these ops exist so agents need not use them.
 */
namespace HyperAIStudio::Niagara::GraphGate
{
	static constexpr int32 MaxHlslChars = 16384;
	static constexpr int32 MaxStageIterations = 64;

	/**
	 * add_simulation_stage, add_hlsl_module, set_emitter_property, set_renderer_property, and add_module or
	 * set_module_enabled addressed to a simulation stage (stage_name set).
	 */
	bool IsGraphOp(const FHyperAINiagaraEditOp& Op);

	/** Checks a graph op's shape. With bResolveAssets it also loads a module script an add_module names. */
	bool ValidateGraphOp(const FHyperAINiagaraEditOp& Op, bool bResolveAssets, FString& OutStatus, FString& OutDiagnostic);

	/**
	 * Checks every op's emitter, stage, renderer and property against the loaded System, walking the plan in order so
	 * an op may address what an earlier op adds (an emitter, renderer, stage or stage module). Renderer property
	 * values are parsed against the renderer class here, so a typo fails the dry run instead of the edit.
	 */
	bool ValidatePlanAgainstSystem(UNiagaraSystem& System, const TArray<FHyperAINiagaraEditOp>& Ops, FString& OutStatus, FString& OutDiagnostic);

	/** Applies one graph op inside the caller's transaction; OutStatus names what it made. */
	bool ApplyGraphOp(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op, FString& OutStatus, FString& OutError);

	/** One emitter's simulation stages and their modules, as inspect reports them. */
	void ReadSimulationStages(UNiagaraSystem& System, const FString& EmitterName, TArray<FHyperAINiagaraSimStageTopology>& OutStages,
		int32& InOutModuleBudget, bool& bOutTruncated);

	/** Object names of the scratch pad module scripts the System owns. */
	TArray<FString> ReadScratchPadModules(const UNiagaraSystem& System);
}
