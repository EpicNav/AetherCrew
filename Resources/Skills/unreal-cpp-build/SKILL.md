---
name: unreal-cpp-build
description: Change a project's C++ and get it compiled into the running editor - find the code, edit modules, Build.cs dependencies, UCLASS, UPROPERTY and UFUNCTION declarations, choose Live Coding for function bodies or a full rebuild through HyperAI's editor restart for anything else, read compiler, linker and UnrealHeaderTool errors, check the log and the build configuration, and prove the change with tests. Use for any C++ edit, compile or build failure, Live Coding or hot reload question, new C++ class, or module dependency.
---

# Changing C++ and compiling it

| step | tool | toolset |
|---|---|---|
| find the code | `hyper_project_search` with `Kinds: ["cpp_symbol"]` | `HyperAIStudio.HyperAIStudioProjectSearchToolset` |
| compile function bodies into the running editor | `CompileLiveCoding` | Epic's `LiveCodingToolset.LiveCodingToolset` |
| full build: save, close, build, reopen | `hyper_editor_restart` | `HyperAIStudio.HyperAIStudioEditorRestartToolset` |
| this session's log | `hyper_log_tail`; older lines with Epic's `GetLogEntries` | `HyperAIStudio.HyperAIStudioLogTailToolset`; `EditorToolset.LogsToolset` |
| engine version, platform, configuration | `hyper_build_diagnose` | `HyperAIStudioAutomation.HyperAIStudioAutomationToolset` |
| Blueprints and packages after a build | `hyper_diagnose` | `HyperAIStudio.HyperAIStudioDiagnoseToolset` |
| automation tests | `DiscoverTests`, `RunTests`, `RunTestsByFilter`, `GetTestStatus`, `GetTestResults` | Epic's `AutomationTestToolset.AutomationTestToolset` |

HyperAI's tools take `{"request": {...}}` through `call_tool`. Edit source files with your own file tools; never touch
`Intermediate`, `Binaries`, generated `.generated.h` files or other plugins.

## Live Coding or a full rebuild

Live Coding patches the running editor's code. It is safe for **function bodies in .cpp files** only. Anything that
changes what UnrealHeaderTool or the class layout sees needs a full rebuild with the editor closed:

- a header edit that adds or removes members, or changes a signature or base class;
- a new or changed `UCLASS`, `USTRUCT`, `UENUM`, `UPROPERTY` or `UFUNCTION`, or a constructor default;
- a new source file, class or module, or a `Build.cs`, `.uproject` or `.uplugin` change.

`CompileLiveCoding` takes no arguments and returns the compile status with the LogLiveCoding output and compiler
errors. It needs Live Coding on in Editor Preferences, and the toolset enabled in HyperAI's settings (Epic MCP
Toolsets, Enable Live Coding Toolset); if either is off, say so and rebuild instead. Live Coding patches last only
for this session: the next full build compiles them in properly. Old-style Hot Reload is not used; never build from
an IDE or the command line while the editor is open, since the build fights the running editor for its modules.

## Full rebuild: hyper_editor_restart

Fields: `bBuild` (true: build the project's C++ and plugins while the editor is closed), `bSaveDirty` (true: save
every changed level and asset first), `Note` (what you were doing and what comes next; it comes back to you), and
`bDryRun`. Always dry-run first:

```json
{"request": {"bDryRun": true, "bBuild": true, "bSaveDirty": true}}
```

The dry run (`Status` `checked`) lists `DirtyPackages` it would save, the `Chats` that reopen, `BuildCommand` (the
running editor's own target, platform and configuration through `Build.bat`, with `-WaitMutex`), `BuildLog`
(`Saved/HyperAIStudio/Restart/build.log`) and `RelaunchCommand`. If it would save something unexpected, ask first.
Then call it for real with the remaining steps in the note:

```json
{"request": {"bBuild": true, "bSaveDirty": true,
 "Note": "Added UPROPERTY FireDelay to ASentryTurret. Next: hyper_log_tail maxVerbosity error, then rerun the turret test."}}
```

`Status` `restarting`: the editor closes about two seconds later and your turn ends, so reply in one line that the
restart is underway. Play In Editor stops first. When the editor is back, your chat resumes with the build result
(exit code, seconds, up to 30 error lines) and your note. A failed build reopens the editor on the old binaries:
fix only the lines named and restart again, at most twice, then report what is left. `refused` explains why: a restart
already underway, automation tests running, a lighting build running, or a never-saved level with `bSaveDirty` on
(save it, or turn `bSaveDirty` off so the user is asked). From outside the editor, wait with
`.hyperai/scripts/Wait-HyperAIStudioMCP.ps1 -TimeoutSeconds 1800`.

## C++ basics that cause most failures

- **Modules.** Each module has `<Module>.Build.cs`. Using another module's types needs it in
  `PublicDependencyModuleNames` (used from public headers) or `PrivateDependencyModuleNames` (.cpp only). A module from
  a plugin also needs the plugin enabled in the `.uproject` (Epic's `PluginToolset` `IsEnabled`, `SetPluginEnabled`).
- **Exports.** A class used from another module carries the API macro, `class MYGAME_API AMyActor`.
- **Reflection.** `#include "MyActor.generated.h"` is the header's last include; `GENERATED_BODY()` opens the class.
  `UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Turret")` for designer-tuned values (Category is required
  on Blueprint-exposed members); `UFUNCTION(BlueprintCallable, Category = "Turret")`, or `BlueprintPure` for queries.
  A raw `UObject*` member without `UPROPERTY` is not seen by garbage collection; use `TObjectPtr<>` with `UPROPERTY()`.
- **Names.** Prefix `A` for actors, `U` for other UObjects, `F` for structs, `E` for enums. Script paths drop it:
  `/Script/MyGame.SentryTurret`. A renamed class keeps content working through a `[CoreRedirects]`
  `+ClassRedirects=(OldName=...,NewName=...)` line in `Config/DefaultEngine.ini`.
- Turn `PrimaryActorTick.bCanEverTick` off unless the actor needs Tick.

## Reading build errors

Start at the first error; later ones often follow from it.

| error | usually |
|---|---|
| `error C2065`, `C3861`, `C2039` (undeclared, not a member) | a missing `#include`, or a typo |
| `error C2027`, `C2079` (undefined type) | only forward-declared: include the header in the .cpp |
| `error LNK2019`, `LNK2001` (unresolved external) | the module is missing from `Build.cs`, the class lacks its API macro, or a declared function has no body |
| UnrealHeaderTool `Error:` lines (in the header) | a malformed `UPROPERTY` or `UFUNCTION`: an unsupported type, a missing Category, `.generated.h` not last |
| `Unable to build while Live Coding is active` | a build started with the editor open: rebuild through `hyper_editor_restart` |
| a module that "could not be loaded" at startup | it failed to build, or a dependency did: read the build log |

## Checking

1. `hyper_log_tail` with `MaxVerbosity` `error` (or `warning`), `Categories` such as `LogLiveCoding`, and `Contains`
   to narrow; follow `NextCursor` for newer lines. It holds this editor process's log only (the newest 2048 entries),
   so read a restart's build output from its resume message or `BuildLog`.
2. `hyper_build_diagnose`: `EngineVersion`, `Platform`, `BuildConfiguration`, `ProjectName`, and whether the
   automation controller and TraceLog are loaded. It never compiles or reads build logs. A Debug build's timings
   mislead.
3. `hyper_diagnose` with `Checks` `loaded_blueprint_health` (Blueprints built on the changed classes still compile) and
   `dirty_loaded_packages`.
4. Prove the behaviour: an automation test through Epic's `AutomationTestToolset` (`DiscoverTests` with
   `bForceRediscover` true after a build, `RunTests`, `GetTestResults`), or a play check with the unreal-playtest
   skill. `hyper_test_inspect` confirms a test is present and runnable; `hyper_test_run` only seals a run, it starts
   no test.

Report each error you fixed with its file and line, the build result, and the proof.
