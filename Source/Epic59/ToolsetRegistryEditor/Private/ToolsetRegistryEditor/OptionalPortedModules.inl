// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: ported toolset modules that depend on a plugin HyperAIStudio does not force on. Each is listed in
// HyperAIStudio.uplugin with "LoadingPhase": "None" (a module whose plugin dependency is off cannot load),
// and the compat ToolsetRegistryEditor module loads it after engine init when every plugin named here is
// enabled. One row per module: EPIC59_OPTIONAL_MODULE(Module, TEXT("Plugin"), ...).
// A port that supersedes a UE 5.8 toolset of the same purpose adds EPIC59_SUPERSEDES(Module, TEXT("/Script/<5.8 module>.<class>"))
// once it passes its tests: when Module is loaded, that 5.8 toolset class is unregistered. See Source/Epic59/PORTING.md.

// Wave 2a: each port loads only where HyperAI's presets (or the user) enabled the UE 5.8 toolset plugin it replaces,
// so a preset or opt-in (LiveCodingToolset is opt-in) keeps deciding what agents get.
EPIC59_OPTIONAL_MODULE(ConfigSettingsToolset59, TEXT("ConfigSettingsToolset"))
EPIC59_OPTIONAL_MODULE(GameFeaturesToolset59, TEXT("GameFeatures"), TEXT("GameFeaturesToolset"))
EPIC59_OPTIONAL_MODULE(GameplayTagsToolset59, TEXT("GameplayTagsToolset"))
EPIC59_OPTIONAL_MODULE(PluginToolset59, TEXT("PluginToolset"))
EPIC59_OPTIONAL_MODULE(LiveCodingToolset59, TEXT("LiveCodingToolset"))
EPIC59_OPTIONAL_MODULE(LiveCodingToolsetTests59, TEXT("LiveCodingToolset"))
EPIC59_OPTIONAL_MODULE(GASToolsets59, TEXT("GameplayAbilities"), TEXT("GASToolsets"))
EPIC59_OPTIONAL_MODULE(AIModuleToolset59, TEXT("AIModuleToolset"))

// Each port's tools are a superset of the 5.8 toolset it supersedes (5.8's GameplayCueToolset keeps its
// FindCueNotifyAssets, which 5.9 dropped, so it stays). The 5.8 toolset is unregistered while the port is loaded.
EPIC59_SUPERSEDES(ConfigSettingsToolset59, TEXT("/Script/ConfigSettingsToolset.ConfigSettingsToolset"))
EPIC59_SUPERSEDES(GameFeaturesToolset59, TEXT("/Script/GameFeaturesToolset.GameFeaturesToolset"))
EPIC59_SUPERSEDES(GameplayTagsToolset59, TEXT("/Script/GameplayTagsToolset.GameplayTagsToolset"))
EPIC59_SUPERSEDES(PluginToolset59, TEXT("/Script/PluginToolset.PluginToolset"))
EPIC59_SUPERSEDES(LiveCodingToolset59, TEXT("/Script/LiveCodingToolset.LiveCodingToolset"))
EPIC59_SUPERSEDES(GASToolsets59, TEXT("/Script/GASToolsets.AbilitySystemInspectorToolset"))
EPIC59_SUPERSEDES(GASToolsets59, TEXT("/Script/GASToolsets.AttributeSetToolset"))

// Wave 2c: the UI and texture ports.
EPIC59_OPTIONAL_MODULE(TextureGraphToolset, TEXT("TextureGraph"))
EPIC59_OPTIONAL_MODULE(MVVMToolset59, TEXT("ModelViewViewModel"))
EPIC59_OPTIONAL_MODULE(WidgetStaticAnalysisToolset, TEXT("ModelViewViewModel"))

// UE 5.9 ports that replace a UE 5.8 toolset (their Epic tests pass on 5.8). The 5.8 SlateInspectorToolset has no guard for
// HyperAI Studio's own UI, so superseding it also removes that approval bypass while the port is loaded.
EPIC59_SUPERSEDES(MVVMToolset59, TEXT("/Script/MVVMToolset.MVVMToolset"))
EPIC59_SUPERSEDES(SlateInspectorToolset59, TEXT("/Script/SlateInspectorToolset.SlateInspectorToolset"))
