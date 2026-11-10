# Porting Epic's UE 5.9 toolsets to UE 5.8

HyperAIStudio carries Epic's UE 5.9 MCP toolsets so they run on UE 5.8 without engine edits. Everything lives in
this folder: a small compatibility layer that supplies what 5.8 lacks under Epic's own module and header names, and
the ported toolset modules next to it. Ported code compiles nearly unchanged; every hand edit is marked
`// UE 5.8:`.

The user is the sole licensee and uses the plugin privately; copying Epic's 5.9 source here is approved. Keep Epic's
copyright headers.

## Layout

```
Source/Epic59/
  AsyncFunctionResult/      compat: UE 5.9 engine module of the same name
  ToolsetCore/              compat: UE 5.9 engine module of the same name
  ToolsetRegistryEditor/    compat: UE 5.9 ToolsetRegistry plugin module of the same name
  CheatsToolset/            ported, Epic's module name
  ConsoleToolset/           ported
  NavMeshToolset/           ported
  NavMeshToolsetTests/      ported (Epic's test module)
  <Name>59/                 ported module whose name UE 5.8 already uses (see "Name collisions")
Tools/Epic59/port_toolset.py
```

Every module is listed in `HyperAIStudio.uplugin` as `Type: Editor`, Win64, with Epic's loading phase (the
compat modules use `Default`). HyperAIStudio is an editor plugin, so 5.9's `Runtime` toolset modules are added as
`Editor` modules here and their cooked-runtime opt-in is dropped.

## The compat layer

UE 5.8's `ToolsetRegistry` plugin already has the registry, `UToolsetDefinition`, `UToolsetRegistry`,
`UToolsetLibrary`, `UToolsetRegistrySubsystem`, `FToolCallExceptionHandler` and `UToolCallAsyncResult*`. 5.9 moved
or renamed parts of that and added more; the three compat modules fill the gap. None of their module names exist in
5.8, and none of their reflected type names collide with 5.8 (the port script's index checks this).

| Module | Provides | 5.8 behaviour to know |
|---|---|---|
| `AsyncFunctionResult` | `UAsyncFunctionResult` and Bool, Float, Int, String, StringArray, Object, Void, Json results; `Utils` (MakeErrorResult, RunOnNextTick, MakeFailureError), `FutureHandler`, `Context`, `CaptureStep`, `JsonConverter` | `UAsyncFunctionResult` derives from 5.8's `UToolCallAsyncResult`, so 5.8's registry treats every 5.9 result as asynchronous and takes its schema from the `Value` property. `OnCompleted` is 5.8's delegate (its parameter is `UToolCallAsyncResult*`). No context is ever installed, so `IsCanceled()` is false, `GetToolCallId()` is empty and `BeginCaptureStep()` returns false (work runs uncaptured). `UAsyncFunctionResultJson`'s custom schema is not seen by 5.8's schema builder. |
| `ToolsetCore` | `IToolsetProvider`, `IToolsetPermissionProvider`, `FToolsetRegistration`, `IToolsetRegistrar`, `WorldContextResolver` | `IToolsetProvider` has no `GetToolsetVersion()` (5.8's `UToolsetDefinition` keeps it, to avoid ambiguity). 5.8's registry never calls `IsToolsetHidden`/`GetToolsToHide`, and dispatches static tools only, so `UsesInstanceDispatch`, `UsesInstanceArgument` and `ResolveToolDispatchTarget` do nothing. |
| `ToolsetRegistryEditor` | 5.9-only `ToolsetRegistry/*.h`: `ToolsetDefinition59.h` (the 5.9 additions to `ToolsetDefinition.h`: `UE::ToolsetRegistry::IsFunctionAIRuntimeCallable`, `GetFunctionAccessMode`, `IsFunctionMarkedAsTool`, `TakesInstanceArgument`), `ToolAccessMode.h`, `RaiseScriptError.h`, `ToolCallContext.h`, `ToolsetPermissionProvider.h`, `ToolCallAsyncResultImageArray.h`, `ToolCallAsyncResultImages.h`, and 5.9 UnrealEd's `FTestAssetSandbox`
(`Tests/TestAssetSandbox.h`, plus the `ToolsetRegistry/Tests/TestAssetSandbox.h` forwarder). Registers an `IToolsetRegistrar` that forwards `FToolsetRegistration` to 5.8's `UToolsetRegistry`. Publicly re-exports `ToolsetRegistry`, `AsyncFunctionResult` and `ToolsetCore`. | 5.9's `ToolsetLibrary.h` and `ToolsetRegistrySubsystem.h` sit at the same include paths in 5.8's `ToolsetRegistry`, so includes resolve to the 5.8 versions. The registrar accepts `UToolsetDefinition` classes only. |

Also in `ToolsetRegistryEditor` (added for the wave-2 ports):

| Piece | What it is | 5.8 behaviour to know |
|---|---|---|
| `ToolsetRegistry/AssetHelpers.h` | 5.9's ToolsetRegistryEditor `AssetHelpers.h/.cpp` copied (`CreateAsset`, `ImportAsset` + `UAsyncImportResult`, `CheckImportDestination`, `AssetExists`, `IsPathVisible`, `LoadAssetIfVisible`, `AssetIsDirty`, the diff helpers) | Its `UToolsetLibrary` calls go to `FToolsetLibrary59`. `CanonicalProperties` reads through 5.8's `ListStructProperties`/`GetObjectProperties`. |
| `ToolsetRegistry/ToolsetLibrary59.h` | `FToolsetLibrary59`: the 11 functions 5.9 added to `UToolsetLibrary`, and 5.9's 4-argument `ListStructProperties(Struct, bUserVisible, RootPath, MaxDepth)`. The port script rewrites `UToolsetLibrary::<added function>` and 3- or 4-argument `ListStructProperties` calls to it (marked `// UE 5.8:`) and adds the include. | Plain C++ statics, not visible to Blueprint or Python. `ListObjectPropertyNames` uses a port of 5.9's property listing (`ToolsetPropertyListing59.cpp`): readability is 5.8's `PropertyAccessUtil::CanGetPropertyValue` (no 5.9 property-editor read gate), a struct with a custom JSON converter (FTransform...) expands into its fields, and an unknown RootPath segment lists the names there rather than 5.9's closest matches. `StructToJson` goes through 5.8's ToolsetJson pipeline. The 4-argument `ListStructProperties` narrows and prunes 5.8's schema the way 5.9 does. |
| `AssetScriptingUtils.h` | 5.9 AssetTools' path functions: `TryConvertToInternalPath`, `ConvertToVirtualPath`, `ConvertToVirtualPathsInText`, `ConvertToInternalPathsInText` | 5.8 has no such header. 5.9's access checks (`GetAssetAccess`, `ResolveNewAssetPath`, `LoadAccessibleObject`, `PassesAssetReferenceFilter`...) are not ported; the script flags them. |
| `PropertyAccessCheck.h` | `UE::PropertyEditor::IsClassAllowed` (5.9 PropertyEditor) | Only that function; the script flags 5.9's property gates (`CheckCanSetProperty`, `CheckCanGetProperty`, `CanReferenceAsset`, `FPropertyAccessResult`). |
| `ToolsetRegistry/ToolsetJsonSchema.h` | 5.9's `FToolsetJsonSchema` value type and `ToJsonArrayString` | Exported from `ToolsetRegistryEditor`, not `ToolsetRegistry`; 5.8's registry never produces one. |
| `ToolsetRegistryEditor/OptionalPortedModules.inl` | Ported modules whose plugin dependency HyperAI does not force on (e.g. GameFeatures, GameplayAbilities): list the module in the `.uplugin` with `"LoadingPhase": "None"` and add a row `EPIC59_OPTIONAL_MODULE(Module, TEXT("Plugin"), ...)`; the compat module loads it at `OnPostEngineInit` when every named plugin is enabled. `EPIC59_SUPERSEDES(Module, TEXT("/Script/<5.8 module>.<class>"))` rows unregister the 5.8 toolset class a loaded port replaces (see "Name collisions"). | A module whose plugin is off is simply not loaded (Verbose log line). |

The compat module now also depends on AssetTools, BlueprintEditorLibrary, ClassViewer, ContentBrowserData,
EditorScriptingUtilities (listed in the `.uplugin`'s `Plugins`; 5.8's ToolsetRegistry needs it anyway),
EditorSubsystem, InterchangeCore/InterchangeEngine, JsonUtilities, Projects and SubobjectDataInterface, and publicly on Json.

Not in the compat layer yet (the port script flags each use): `UndoCaptureParticipant.h`, `MarkdownAgentSkills.h`,
`CodeBlock.h`, `ToolsetManifestSeed.h`, `ToolsetRegistrySettings.h`, `FToolsetRegistry::Get()/Create()`, the
`FScopedScriptExceptionCapture`-based `FToolCallExceptionHandler::HandleScriptMessage`, 5.9's `AssetScriptingUtils`
access checks and `PropertyAccessCheck` gates, and the registry's private test fixtures.

Behaviour of 5.8's registry that agents and tests notice: every tool parameter is required (5.9 honours C++
defaults, 5.8 rejects a call that omits one), arguments are camelCase (`nameFilter`, `logFilePath`), and an
`AIAccessMode` is not reported in the schema.

## The port script

```
set EPIC59_SRC=<UE 5.9 source root>
python Tools/Epic59/port_toolset.py --list                    # every 5.9 toolset and whether it is ported
python Tools/Epic59/port_toolset.py --check GASToolsets        # report only
python Tools/Epic59/port_toolset.py GASToolsets                # copy, fix, add to the .uplugin, report
python Tools/Epic59/port_toolset.py --force GASToolsets        # re-copy (hand edits are lost)
```

The first run indexes UE 5.8's reflected types, module names and headers (about four minutes) into
`Intermediate/Epic59/ue58_index.json`; later runs take seconds. Files keep their line endings.

### Fix rules (applied by the script)

1. Copy `Source/<Module>/` byte for byte to `Source/Epic59/<Module>/`.
2. Module names that UE 5.8 or HyperAI already uses get `59` appended: folder, `<Module>59.Build.cs` and its class,
   `<MODULE>59_API`, `IMPLEMENT_MODULE`, dependencies between the toolset's own modules, `TEXT("<Module>.`" and
   `/Script/<Module>.` strings, and the `.uplugin` entry.
3. Reflected types (UCLASS, USTRUCT, UENUM, UINTERFACE) whose names UE 5.8 or HyperAI already uses get `59` appended,
   everywhere but `#include` lines (an interface's `I` class follows its `U` class).
   Reflected headers whose *file name* UE 5.8 or HyperAI already uses are renamed the same way (UHT fails with
   "Two headers with the same name is not allowed" otherwise), with their `#include`s, `.generated.h` and
   `UE_INLINE_GENERATED_CPP_BY_NAME`: ConsoleToolset's `LogsToolset.h` is `LogsToolset59.h`.
4. `#include "ToolsetRegistry/ToolsetDefinition.h"` becomes `ToolsetRegistry/ToolsetDefinition59.h`.
5. `UToolsetDefinition::IsFunctionAIRuntimeCallable|GetFunctionAccessMode|IsFunctionMarkedAsTool` become
   `UE::ToolsetRegistry::...` (5.8's class cannot gain statics).
5a. `UToolsetLibrary::<function 5.9 added>` and `UToolsetLibrary::ListStructProperties` called with 5.9's RootPath or
   MaxDepth become `FToolsetLibrary59::...` (with the `ToolsetRegistry/ToolsetLibrary59.h` include), each line marked.
6. A class declared `: public UToolsetDefinition` also lists `public IToolsetProvider`, so its 5.9 hook overrides
   compile.
7. In toolset classes, `AIRuntimeCallable` gains `AICallable` (5.8 recognises only the latter; the 5.9 metadata
   stays for tests that read it), and a UFUNCTION with neither gains `AIIgnore` (5.9 skips unmarked functions
   silently; 5.8 logs an error for each).
8. `UToolsetRegistry::EnableCookedRuntimeSupport(...)` is commented out (5.8 has no cooked-runtime toolsets).
9. Build.cs: a module depending on `ToolsetRegistry` also depends on `ToolsetRegistryEditor`, `ToolsetCore` and
   `AsyncFunctionResult` (all three: UBT passes a public dependency's include paths on but not its import library,
   so a class implementing `IToolsetProvider` must link ToolsetCore itself); `ToolsetRegistryEngine` maps to `ToolsetRegistryEditor`; `AsyncFunctionResultEditor` is dropped.
10. `HyperAIStudio.uplugin` gains the modules (Editor, Epic's loading phase, Win64).

### Optional plugin dependencies

A port that links against a plugin HyperAI lists as optional (`Enabled: false` in `HyperAIStudio.uplugin`'s `Plugins`) has
`LoadingPhase: None`: loading it while that plugin is disabled would fail the whole plugin. `HyperAIStudio::Epic59Modules`
(`Source/HyperAIStudio/Private/HyperAIStudioEpic59Modules.cpp`) loads each such module after engine init when all its
plugins are enabled, and logs the ones it leaves unloaded. Add new ones to its table (tests modules too) and re-set
`LoadingPhase` after a `--force` re-port, which writes Epic's phase back. Bench projects enable the plugin through
`REQUIRED_PLUGINS` in `Tools/Benchmark/bench.py`.

### What the script does not catch

- `RegisterToolsetClass` of a class that is not a `UToolsetDefinition` (5.9 accepts any `IToolsetProvider`): 5.8's
  `TSubclassOf<UToolsetDefinition>` turns it into null and logs `Cannot register null toolset class`. Register a
  `UToolsetDefinition` front whose statics forward (RewindDebuggerToolset58, InputToolset58).
- Converters (`FToolsetJsonConverter`): 5.9's `JsonDataToProperty` returns `FToolsetJsonImportResult` and takes an
  `FToolsetJsonImportContext`; 5.8's returns `bool` and takes `UObject* Outer`. A port whose converter has the name of a
  5.8 converter must rename it.
- Engine APIs newer than 5.8 outside the registry (CsvUtils, RHI, GameplayInsights): the build finds them.

### What it reports for hand edits

5.9-only APIs (above), non-static tools, `UToolsetRegistry::ExecuteTool` results assigned to 5.9 types (5.8 returns
`UToolCallAsyncResultString*`), `OnCompleted.AddDynamic` handlers, hook overrides 5.8 never calls, strings that name a
renamed module or a renamed toolset class, includes that neither UE 5.8 nor the ports provide, and Epic's own `UE59`
adaptation notes (each marks a spot where 5.9 already differs from UE6, so check it against 5.8 too). It also lists
plugin dependencies from the toolset's `.uplugin` that HyperAIStudio.uplugin lacks: add those to its `Plugins`
(disabled, plus a load guard, if the dependency is optional for HyperAI users).

### Hand edit rules

- Smallest change that compiles and keeps the tool's behaviour; prefer `#if 0 // UE 5.8: <why>` with a 5.8 fallback
  over deleting Epic's code.
- End the changed line with, or put above it, a short `// UE 5.8: <reason>` comment. `git grep "UE 5.8:"` lists
  every adaptation.
- Never edit the compat layer for one toolset's convenience unless the 5.9 API really is general; then add it under
  its 5.9 header name.
- LOCTEXT strings are collected by `Tools/Localization/gather.py` like HyperAI's own: run it, fill
  `Resources/Localization/zh-Hans.json`, and `gather.py check` must pass.

## Name collisions with toolsets that also ship in 5.8

These 5.9 toolsets exist in UE 5.8 too: EditorToolset, MVVMToolset, SlateInspectorToolset, ConfigSettingsToolset,
GameFeaturesToolset, GameplayTagsToolset, PluginToolset, LiveCodingToolset, PCGToolset, NiagaraToolsets,
DataflowAgent, ChaosClothAssetToolset and AIModuleToolset (and also AnimationAssistantToolset, AutomationTestToolset,
ConversationToolset, DataRegistryToolset, GASToolsets, MCPClientToolset, PhysicsToolsets, StateTreeToolset,
WorldConditionsToolset). Their module names and many class names collide with 5.8's engine plugins, which HyperAI's
presets enable. Two modules or classes of one name cannot coexist (UBT module names are global; duplicate short
class names make `FindFirstObject` and Python's `unreal.<Class>` ambiguous), so:

- **Rename.** The port script appends `59` to the colliding module and to each colliding type, e.g. module
  `EditorToolset` becomes `EditorToolset59` and `UEditorAppToolset` becomes `UEditorAppToolset59`. 5.8's registry
  names a toolset `<Module>.<Class>`, so agents see `EditorToolset59.EditorAppToolset59` next to 5.8's
  `EditorToolset.EditorAppToolset`. Non-colliding classes keep their names (`ConsoleToolset.ConsoleToolset`).
  ConsoleToolset's `ULogsToolset` collides with 5.8 EditorToolset's and is ported as `ConsoleToolset.LogsToolset59`.
- **Load the port where the 5.8 toolset is enabled, and hide the 5.8 version** once the port passes its tests.
  Both go in `ToolsetRegistryEditor/Private/ToolsetRegistryEditor/OptionalPortedModules.inl`:
  - `EPIC59_OPTIONAL_MODULE(GameplayTagsToolset59, TEXT("GameplayTagsToolset"))` with the module's `.uplugin` entry at
    `"LoadingPhase": "None"`: the port loads (at `OnPostEngineInit`) only where HyperAI's capability presets, or the
    user, enabled the 5.8 toolset plugin it replaces, so presets and opt-ins (LiveCodingToolset is opt-in) keep
    deciding what agents get. List the port's own plugin dependencies there too.
  - `EPIC59_SUPERSEDES(GameplayTagsToolset59, TEXT("/Script/GameplayTagsToolset.GameplayTagsToolset"))`: while the port
    is loaded, the compat module unregisters that 5.8 toolset class (at engine init and whenever a module loads,
    since some 5.8 toolset modules load later). Only for a 5.8 class whose every tool the port has: 5.8's
    `GASToolsets.GameplayCueToolset` keeps `FindCueNotifyAssets`, which 5.9 dropped, so it stays visible.
  - Point HyperAI's references to the 5.8 names at the port (`GetEpicDelegates` lists, the Data delegation matrix,
    `Resources/Skills`): `git grep "<Module>\.<Class>"`.
  - Why not the two documented alternatives: a registry block list (`BlockedNames`, or
    `FToolsetRegistry::AddBlockedName`) makes HyperAI refuse its own runtime toolset registrations
    (`owned_registration_requires_unfiltered_registry` in HyperAIStudioCapabilityRuntimeIndex.cpp), and removing the
    5.8 plugin from the capability presets hides nothing in projects that already enabled it, nor under
    "Everything", whose `AllToolsets` aggregator enables it again. EditorToolset and MCPClientToolset are foundation
    plugins; supersede their overlapping classes the same way rather than disabling them.
- **Test names.** 5.8's toolset tests register automation tests under the same C++ class names as 5.9's (the spec or
  `TEST_CLASS` name is the registration key, whatever its path), and whichever loads second is dropped with "Failed to
  register test with the name ...". Rename the port's spec classes `...59` (and their `friend` declarations).

## HyperAI's tool catalog

`Tools/Catalog/catalog_source.json` and `HyperAIStudioCapabilityPackCatalog.generated.inl` cover HyperAI's own
`hyper_*` tools only: `build_catalog.py check` matches the `static ... hyper_*(` UFUNCTIONs it finds under
`Source/`. Epic's toolsets declare none, so ports need no catalog entries and `build_catalog.py check` stays green.
The capability preset tests count Epic *plugins*; the ports live inside HyperAIStudio, so they change no count.

## Per-toolset checklist

1. `python Tools/Epic59/port_toolset.py --check <Toolset>`: read the notes and the hand-edit list.
2. If it needs a compat piece listed as missing above, add it to the compat layer first (in its own commit).
3. `python Tools/Epic59/port_toolset.py <Toolset>`.
4. Add missing plugin dependencies to `HyperAIStudio.uplugin` (`Plugins`); a plugin that is not enabled by default
   gets `"Enabled": false`, the module `"LoadingPhase": "None"` and a row in `OptionalPortedModules.inl`. Any module dependency that is
   optional for HyperAI users behind the same guard HyperAI's own optional modules use.
5. Make the hand edits, each marked `// UE 5.8:`.
6. Build on a bench project: `HYPERAI_BENCH_PROJECT=E:/HyperAIWork/Bench59 HYPERAI_BENCH_PORT=8031 python
   Tools/Benchmark/bench.py setup --source <worktree>` (wait while `E:/HyperAIWork/ubt-hold.txt` exists).
7. Run Epic's tests: `python E:/HyperAIWork/tools/run_tests.py <bench> <label> AIAssistant.Toolsets.<Toolset>`.
   A test that needs a missing 5.9 engine feature is skipped with `// UE 5.8:` and a reason, not deleted.
8. Start the bench editor (`bench.py editor start`), check the toolset in `list_toolsets` on the bench's MCP port,
   call one read tool, then `bench.py editor stop`.
9. `python Tools/Localization/gather.py check` and `python Tools/Catalog/build_catalog.py check`.
10. Commit the compat change, the port and the hand edits as separate commits.

## What each ported toolset needed

| Toolset | Mechanical | Hand edits |
|---|---|---|
| CheatsToolset | include, IToolsetProvider base | none |
| ConsoleToolset | include, IToolsetProvider base, AICallable on 9 AIRuntimeCallable tools, cooked opt-in dropped, `ULogsToolset` and `LogsToolset.h` renamed `...59` | `SetVerbosity`: 5.8 has no `FLogSuppressionInterface::FindCategory`, so it checks the category through `GetVerbosity` and cannot check the compile-time maximum |
| NavMeshToolset (+ Tests) | include, IToolsetProvider base, AICallable on 21 tools, cooked opt-in dropped, `IsFunctionAIRuntimeCallable` moved | 5.8 lacks the adaptive navmesh build API: adaptive stats report `bAvailable=false`, tiles report build state `Unknown` and take their bounds from the first layer, no tile state grid or updated-tile count; the dirty-area controller is read through a member pointer (protected in 5.8); invokers are listed from 5.8's raw invoker records (no actor or name); `EWorldType`, `ENetMode`, `ENavigationBuildLock`, `ENavigationShapeType`, `ENavigationAreaMode` properties are `uint8` (not UENUMs in 5.8); the test's `ExecuteTool` result type |
| ConfigSettingsToolset59 | module and types renamed | `SetObjectProperties` has no PropertyListHint argument, and 5.8's records no undo transaction, so the call is wrapped in an `FScopedTransaction` for the rollback; the unparseable-value test is skipped (5.8's `SetObjectProperties` accepts a JSON object for a bool, UE-402853) |
| GameFeaturesToolset59 | module and types renamed | `EBuiltInAutoState` has no `Count`; name lookups use the UTF-8 `GetPluginURLByName`; 5.9's `GameFeaturePluginTestHelper` copied into the test folder; the two synthetic-plugin transition tests are skipped (the by-name transitions never complete on 5.8, so they time out) |
| GameplayTagsToolset59 | module and types renamed | 5.8's `IGameplayTagsEditorModule` INI functions have no `OutErrors`: AddTag refuses a source without a tag ini itself (5.8 would write the default config), MoveTags checks the target source and each tag first and reports them as 5.9 does |
| PluginToolset59 | module and types renamed | `OnPluginCreated` has no error out-parameter; the test template folder ships with the port (`PluginToolset59/TestPluginTemplate`) |
| LiveCodingToolset59 (+ Tests) | modules and types renamed, CVar `LiveCodingToolset59.Enable` | 5.8's registry takes `UToolsetDefinition` classes only, so `ULiveCodingToolsetTools59` carries the three tools and the subsystem registers it (`LiveCodingToolset59.LiveCodingToolsetTools59`); no `IsReadyForReload` (ready = started, enabled, not compiling) and no compile-finished delegate (CompileLiveCoding polls `IsCompiling`; a patch applied reads as Success, otherwise "Failure or NoChanges") |
| GASToolsets59 | module and types renamed, 2 `ValidateAssetPath` calls to `FToolsetLibrary59` | specs use BeforeEach/AfterEach for 5.9's BeforeAll/AfterAll; the inspector test expects the renamed attribute set |
| AIModuleToolset59 | module renamed (no type collides), 3 `ValidateAssetPath` calls to `FToolsetLibrary59` | specs use BeforeEach/AfterEach for BeforeAll/AfterAll |
| WidgetAnimationToolset | include, IToolsetProvider base | `Kismet2/Kismet2NameValidators.h` (5.9 renamed it); `IsClassAllowed` via `FToolsetLibrary59`; the 5.9 `AnimationAssistantToolset` plugin dependency is dropped (no module uses it); Epic ships no tests |
| TextureGraphToolset | include, IToolsetProvider base | `ValidateAssetPath` via `FToolsetLibrary59`; optional on `TextureGraph` (`OptionalPortedModules.inl`); `IsToolsetHidden` is never called by 5.8 |
| MVVMToolset59 (31 tools; 5.8 has 9) | renamed `...59`, test fixtures renamed | 5.9's `UMVVMEditorSubsystem::ResolvePropertyPath`, `TryAutoConvertBinding`, `SetConversionFunctionArgumentLiteral`, `SetEventArgumentLiteral` (with its private `DoAction`) and `UMVVMBlueprintView::RecreateGeneratedGraphs` are copied as free functions (`MVVMEditorSubsystem59.h/.cpp`); the 3 design-time preview tools refuse (5.9 designer feature); test `BeforeAll/AfterAll` become `BeforeEach/AfterEach`; optional on `ModelViewViewModel`; supersedes 5.8's `MVVMToolset.MVVMToolset` |
| SlateInspectorToolset59 (+ Tests; 30 tools, 5.8 has 14) | renamed `...59`; needs the 5.9 `WidgetHelper` module, copied unchanged to `Source/Epic59/WidgetHelper` | the class is a `UToolsetDefinition` (5.8's registry takes no other) and its subsystem half is `USlateInspectorToolset59Subsystem`; `FEditorDelegates::EndPIE`; `FToolsetImage::SetFromBitmap`; `IsUsingPlatformCursorForCursorUser` rebuilt (reads the protected `FSlateUser::GetCursor` via explicit-instantiation access); **HyperAI guard** (`SlateInspectorToolsetHyperAIGuard.h/.cpp`): refs, focus-bound keys/chars, window actions and pointer/touch/wheel events headed to HyperAI Studio's widgets (created by `HyperAIStudio*` sources or typed `*HyperAIStudio*`) or to toast windows are refused or dropped, and snapshots hide them; guard tests `...SlateInspectorToolset.HyperAIGuard`; supersedes 5.8's `SlateInspectorToolset.SlateInspectorToolset` (which had no guard) |
| WidgetStaticAnalysisToolset | include, IToolsetProvider base | 5.9's `UWidgetBlueprintEditorSubsystem` helpers are 5.8's `FWidgetBlueprintOperationUtils` (same signatures); test `BeforeAll/AfterAll`; optional on `ModelViewViewModel` |
| PerfToolset | include, IToolsetProvider base, cooked opt-in dropped | 5.8's CsvUtils has no `DetectCsvFormat`, so a copy of 5.9's lives in `CsvProfilerToolset.cpp`; 5.8's RHI has no `GPUProfiler::SetProfileCaptureCallback`/`ProfileNextFrame`, so `GpuProfilerToolset.TriggerGPUProfile` returns an error naming the `ProfileGPU` console command; the `ReadFromCsvBin` tests (5.9's v4/v5 `.csv.bin` reader with event timestamps) are compiled out |
| RewindDebuggerToolset (+ Tests) | include, IToolsetProvider base; needs GameplayInsights (optional, guarded) | the toolset is an editor subsystem, which 5.8's `RegisterToolsetClass` turns into a null class, so `URewindDebuggerToolset58` (a `UToolsetDefinition` whose statics forward) is registered instead: agents see `RewindDebuggerToolset.RewindDebuggerToolset58`; the gameplay provider is read by its 5.8 name `GameplayProvider` (5.8's `IGameplayProvider` has no `ProviderName`) |
| InputToolset | include, cooked opt-in dropped | 5.9 dispatches to the local player's `UInputToolset` subsystem (instance dispatch); `UInputToolset58` is registered instead, its static `ApplyInput`/`IsInputApplied` resolve the same subsystem and forward, and report 5.9's dispatch-target error when no PIE or game session has a local player: agents see `InputToolset.InputToolset58` |
| WorldConditionsToolset59 | module, `UWorldConditionTools` and its header renamed `...59`; optional WorldConditions (guarded) | 5.8 converters return `bool`, not `FToolsetJsonImportResult`; no `FToolsetRegistry::Get()` (the editor subsystem's registry is used); the converter is named `WorldConditionQueryConverter59`, because 5.8 WorldConditionsToolset registers `WorldConditionQueryConverter` and 5.8's registry `verify()`s unique names (a duplicate would crash) |
| DataRegistryToolset59 | module, `UDataRegistryTools`, `FDataRegistryInfo`, `FDataRegistrySourceSummary` and the header renamed `...59`; optional DataRegistry (guarded) | none |

Pilot results on a UE 5.8 bench (Bench59): Epic's own tests `AIAssistant.Toolsets.CheatsToolset` (17),
`ConsoleToolset` (28) and `NavMeshToolset` (23) pass, 68 of 68, with no registry errors in the log; HyperAI's
`HyperAIStudio.NativeTools.Capability*` tests pass (5 of 5). Over MCP, `list_toolsets` shows
`CheatsToolset.CheatsToolset`, `ConsoleToolset.ConsoleToolset`, `ConsoleToolset.LogsToolset59` and
`NavMeshToolset.NavMeshToolset`, and `GetCVarValue`, `GetLogCategories` and `ListWorlds` answer.

Wave 2a results on a UE 5.8 bench (BenchPortA): Epic's tests `AIAssistant.Toolsets.ConfigSettingsToolset` (37),
`GameFeatureToolset` (31), `GameplayTagsToolset` (38), `PluginToolset` (54), `GASToolsets` (57), `AIModuleToolset`
(26) and four of LiveCodingToolset's five (the fifth, `CompileLiveCoding_EventuallyResolvesWithResult`, starts a real
Live Coding build, which would take the UBT mutex the user's editor and other benches share, so it was not run), with
the pilot suites and HyperAI's `HyperAIStudio.NativeTools.Capability*`, `GAS`, `Data` and `GameplaySystems`: 350 of
350. Over MCP, `list_toolsets` shows the seven ports and no longer the superseded 5.8 toolsets (only 5.8's
`GASToolsets.GameplayCueToolset` stays, next to `GASToolsets59.GameplayCueToolset59`), and `ListContainers`,
`ListTagSources`, `GetAdditionalPluginDirectories`, `GetConfiguredAdditionalMetadataKeys`, `IsLiveCodingReady` and
`FindAttributeSetClasses` answer.

Wave 2c on a UE 5.8 bench (BenchPortC): `AIAssistant.Toolsets.TextureGraphToolset` (61), `WidgetStaticAnalysisToolset` (101),
`MVVM` (91) and `SlateInspectorToolset` (61 Epic + 4 HyperAI guard) pass, 318 of 318. CommonUIToolset was tried and dropped: its
5.9 CommonUIDebugging module reads `UCommonUIActionRouterBase` debug internals (`GetActivatableTreeDebugInfo`, `FDebug*Info`,
`ECommonUIFocusRedirectReason`, `UCommonActivatableWidget::OnAnyActivationChanged`, `UCommonInputSubsystem::GetCurrentInputLocks`)
that 5.8's CommonUI does not have, so it cannot be ported without engine edits.

## Remaining toolsets

Ported since the pilot (wave 2a): ConfigSettingsToolset, GameFeaturesToolset, GameplayTagsToolset, PluginToolset,
LiveCodingToolset, GASToolsets and AIModuleToolset (see the table above).

`port_toolset.py --check` over every 5.9 toolset gives the following starting points (counts are flagged lines, many
are informational). Toolsets in Epic's `UE59-DISABLED.md` (CommonUIToolset, CaptureManagerToolset and the MetaHuman
toolsets) need newer plugin APIs than even 5.9 has and are not worth porting to 5.8.

| Toolset | Size (lines) | Plugin deps beyond the registry | Main blockers from `--check` | Estimate |
|---|---|---|---|---|
| ConversationToolset, StateTreeToolset, GeometryToolset, MetaHumanGenerator | < 100 | CommonConversation, StateTree, GeometryScripting, MetaHuman | module rename only (5.8 has the first two) | < 1 h each |
| PerfToolset, RewindDebuggerToolset, WorldConditionsToolset, DataRegistryToolset, InputToolset | 260-4100 | GameplayInsights, WorldConditions, DataRegistry, EnhancedInput | few flags; InputToolset has 3 non-static tools | 1-3 h each |
| PhysicsToolsets, ChaosClothAssetToolset, DataflowAgent, AnimationAssistantToolset, TextureGraphToolset, WidgetAnimationToolset | 500-2900 | as their .uplugin | module and type renames, a few 5.9 UToolsetLibrary calls | 2-4 h each |
| NiagaraToolsets, PCGToolset, MetaSoundToolset, AutomationTestToolset | 1400-5900 | Niagara, PCG, Metasound | renames; AssetHelpers / added UToolsetLibrary functions (now in the compat layer); PCG uses the 5.9 exception-capture API (25 sites); AutomationTest has OnCompleted handlers and non-static tools | 0.5-1 day each |
| FileSystemToolset, MediaToolset, SceneCaptureToolset, AgentPluginToolset, KnowledgeBaseToolset, MCPClientToolset, MVVMToolset, WidgetStaticAnalysisToolset, SlateInspectorToolset | 670-14500 | FileSystemToolset, MediaToolset, AgentCapability, KnowledgeBase, ModelViewViewModel, WidgetHelper | `FToolsetRegistry::Get`, async results through `ExecuteTool`, sibling 5.9 plugins (AgentCapability, KnowledgeBase, WidgetHelper) to port first | 1-2 days each |
| EditorToolset, BlueprintToolsets | 35000, 48700 | DataValidation, Interchange, GraphDsl | ~100 missing includes each (GraphDsl plugin, newer editor APIs), AssetHelpers | 3-5 days each |
| CommonUIToolset, CaptureManagerToolset, MetaHuman*Toolset | | newer plugin APIs than 5.9 has | not worth porting | - |

Ports that collide with a 5.8 plugin HyperAI enables (EditorToolset, MCPClientToolset, the AllToolsets set) also need the
5.8 toolset hidden once the port passes, as described above.
