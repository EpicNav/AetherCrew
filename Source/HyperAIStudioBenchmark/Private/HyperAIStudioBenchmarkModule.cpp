// Games by Hyper 2026.

#include "Modules/ModuleManager.h"

// ponytail: no module state. The verification tests register themselves when this DLL loads, which the
// harness triggers with "Module Load HyperAIStudioBenchmark" in a fresh editor process.
IMPLEMENT_MODULE(FDefaultModuleImpl, HyperAIStudioBenchmark)
