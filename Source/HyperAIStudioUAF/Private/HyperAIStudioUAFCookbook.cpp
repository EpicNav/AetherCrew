// Games by Hyper 2026.

#include "HyperAIStudioUAFCookbook.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::UAF
{
	namespace
	{
		// Split into literals under the compiler's single-literal size limit.
		const TCHAR* const Sections[] = {
TEXT(R"(# UAF Cookbook

Written by HyperAIStudio for agents building animation with the Unreal Animation Framework (UAF), the
experimental successor to Animation Blueprints in UE 5.8. Use it with `hyper_uaf_inspect`,
`hyper_uaf_apply_plan` and `hyper_uaf_validate`: plan, apply, then validate after every plan.

UAF is experimental. Its assets and API change between engine versions, and a graph you build today may need
rework after an engine update. Animation Blueprints remain supported; see
`.hyperai/docs/HyperAIStudio-AnimationCookbook.md` and the `anim_blueprint` tools for those.

## 1. How UAF differs from an Animation Blueprint

| Animation Blueprint | UAF animation graph |
|---|---|
| AnimGraph of anim nodes | RigVM graph of **trait stacks** |
| One node = one behaviour | One stack = a **base trait** plus **additive traits** layered on it |
| State machine asset-side | a State Tree, a chooser, or a blend stack requester |
| Event Graph updates variables | a **Module** asset runs the per-frame logic |
| Node pins | trait pins, set to a value or fed by a variable getter |

The unit to think in is the stack, not the node. `FSequencePlayerTrait` plays a sequence;
adding `FBlendSmootherTrait` to the same stack makes its transitions blend; adding
`FSynchronizeUsingGroupsTrait` syncs it to other players. You compose behaviour vertically inside one stack
instead of wiring five nodes in a row.

## 2. The rules the tools enforce

- Every stack needs exactly **one base trait**, at the bottom. Additive traits go on top of it.
- A stack's `Result` feeds either another stack's **pose pin** or the graph's **output pose**.
  Exactly one stack reaches the output; `link_output` sets it.
- A pose pin with nothing linked evaluates to the reference pose, which reads as a T-pose. `hyper_uaf_validate`
  warns about it.
- A stack nothing reaches from the output never evaluates. Remove it or link it.

## 3. Building a graph

1. `hyper_uaf_inspect` with `include_trait_catalog` and a `trait_filter` such as `Blend` or `Player` to see the
   traits registered in this project and every pin each one takes. The catalog is the live registry, so it
   already includes traits from UAFPoseSearch, UAFStateTree, UAFMirroring and UAFWarping when those are enabled.
2. Plan with `hyper_uaf_apply_plan`, `bCreate` true, ops in this order:
   `add_variable` -> `add_node` -> `add_trait` -> `set_pin` / `bind_pin` -> `link` -> `link_output`.
3. Dry-run, resubmit with the plan hash, poll `hyper_operation_status`, then `hyper_uaf_validate`.

A minimal blended locomotion graph:

```
add_variable   Name=Speed    Value=float
add_variable   Name=IsMoving Value=bool
add_node       Node=Idle
add_trait      Node=Idle  Trait=FSequencePlayerTrait
set_pin        Node=Idle  Trait=FSequencePlayerTrait Pin=AnimSequence Value=/Game/.../MM_Idle.MM_Idle
add_node       Node=Jog
add_trait      Node=Jog   Trait=FBlendSpacePlayerTrait
set_pin        Node=Jog   Trait=FBlendSpacePlayerTrait Pin=BlendSpace  Value=/Game/.../BS_Jog.BS_Jog
bind_pin       Node=Jog   Trait=FBlendSpacePlayerTrait Pin=XAxisSamplePoint Value=Speed
add_node       Node=Locomotion
add_trait      Node=Locomotion Trait=FBlendByBoolTrait
add_trait      Node=Locomotion Trait=FBlendSmootherTrait
bind_pin       Node=Locomotion Trait=FBlendByBoolTrait Pin=bCondition Value=IsMoving
link           Node=Idle  To=Locomotion Trait=FBlendByBoolTrait Pin=FalseChild
link           Node=Jog   To=Locomotion Trait=FBlendByBoolTrait Pin=TrueChild
link_output    Node=Locomotion
```

`FBlendSmootherTrait` on the blend stack is what stops the switch popping; without it the pose changes in one
frame. Give it a blend time of 0.15-0.25 s for locomotion, 0.1 s for a landing, and inertialization
(`FBlendInertializerTrait`) when the two poses are far apart.

## 4. Variables

`add_variable` takes bool, float, int, name or vector. `bind_pin` adds a getter for one and wires it into a pin,
which is how gameplay reaches the graph. `set_pin` writes a constant instead.

Nothing **sets** those variables yet: that is a Module asset's job, and these tools do not author Modules. Until
you build one by hand in the UAF editor, a graph built here evaluates with each variable at its default. Plan
around that: the graph's shape is authored here, the per-frame values are not.

## 5. What these tools do not build

Author these in the UAF editor, in a workspace alongside the graph:

- **Modules** (`UAnimNextModule`), their event graphs, and binding a graph to a character with
  `UAnimNextComponent`. Without a module, nothing drives the graph's variables at runtime.
- **Workspaces**, which are how the UAF editor groups a graph with its module and data interfaces.
- Pose Search databases and schemas, IK Rigs and retargeters, which the `FMotionMatchingTrait` and the
  retargeting traits reference but do not create.
- Running a UAF graph from inside an existing Animation Blueprint, through the UAF anim node.
)")};
	}

	FString GetCookbook()
	{
		FString Text;
		for (const TCHAR* Section : Sections)
		{
			Text += Section;
		}
		return Text;
	}

	FString GetCookbookPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".hyperai/docs/HyperAIStudio-UAFCookbook.md"));
	}

	bool EnsureCookbook()
	{
		const FString Path = GetCookbookPath();
		const FString Text = GetCookbook();
		FString Existing;
		if (FFileHelper::LoadFileToString(Existing, *Path) && Existing == Text)
		{
			return true;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}
