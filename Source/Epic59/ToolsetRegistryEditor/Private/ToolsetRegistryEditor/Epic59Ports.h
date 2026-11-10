// Games by Hyper 2026.

// UE 5.8: HyperAI Studio addition; see Epic59Ports.cpp.

#pragma once

namespace UE::ToolsetRegistry::Epic59
{
	/** Loads the ports that need optional plugins and hides the 5.8 toolsets they replace. */
	void StartupPorts();
	void ShutdownPorts();
}
