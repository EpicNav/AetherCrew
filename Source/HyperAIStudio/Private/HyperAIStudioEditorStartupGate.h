// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * Says whether this editor can answer requests to its own Unreal MCP server yet, and holds the probes that must wait.
 *
 * Epic's server reads requests in the HTTP server module's core ticker, which first runs inside FEngineLoop::Tick.
 * FCoreDelegates::OnFEngineLoopInitComplete is too early for that in 5.8: it fires inside FEngineLoop::Init, before
 * EditorInit loads the startup map and builds the main frame (which restores the chat tabs, and they probe at once),
 * and the first frame then ran for 16 more seconds in one restart's log. A request sent in that window sits unread in
 * the listen backlog, times out, and says nothing about the server. So the gate opens on the first core tick after the
 * main loop has finished a whole frame, and runs what waited then. Game thread only.
 */
class FHyperAIStudioEditorStartupGate
{
public:
	/** Starts watching for the main loop. Already open in a commandlet, or when the module loads after startup. */
	static void Startup();
	/** Drops the held work without running it. */
	static void Shutdown();

	/** True once this editor answers requests to its own MCP server. */
	static bool IsEditorAnswering();

	/** Runs Work now when the editor answers, or else as soon as it does; true when it ran now. */
	static bool RunWhenEditorAnswers(TFunction<void()> Work);

	/**
	 * True when Endpoint is this editor's own server: loopback on the port HyperAI is set to, or on the port Epic's
	 * server runs on. Another editor's port is not, so probes of it never wait here.
	 */
	static bool IsOwnServerEndpoint(const FString& Endpoint);

	/** Tests only: true holds the gate as if the editor were still starting; false lets go and runs what waited. */
	static void SimulateStartingForTests(bool bStarting);
};
