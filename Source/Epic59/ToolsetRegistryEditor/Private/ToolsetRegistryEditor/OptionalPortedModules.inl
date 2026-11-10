// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: ported toolset modules that depend on a plugin HyperAIStudio does not force on. Each is listed in
// HyperAIStudio.uplugin with "LoadingPhase": "None" (a module whose plugin dependency is off cannot load),
// and the compat ToolsetRegistryEditor module loads it after engine init when every plugin named here is
// enabled. One row per module: EPIC59_OPTIONAL_MODULE(Module, TEXT("Plugin"), ...).
// A port that supersedes a UE 5.8 toolset of the same purpose adds EPIC59_SUPERSEDES(Module, TEXT("/Script/<5.8 module>.<class>"))
// once it passes its tests: when Module is loaded, that 5.8 toolset class is unregistered. See Source/Epic59/PORTING.md.
