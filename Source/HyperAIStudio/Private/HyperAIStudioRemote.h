// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * The remote page: the chats as they run, the questions they ask and the edits waiting for approval, in a browser on
 * this computer or, when allowed, on another device on the same network, such as a phone. A browser pairs with a code
 * the editor shows and gets a key that lives only as long as this editor, so nothing on disk lets an agent pair itself
 * and approve its own edits. Game thread only.
 */
class FHyperAIStudioRemote
{
public:
	/** Module lifetime: serves the page from the start when Remote Page is on. */
	static void Startup();
	static void Shutdown();

	/** Serves the page on Remote Page Port; false, with OutError, when the port cannot be had. */
	static bool Start(FString& OutError);
	/** The page stops answering and paired browsers are forgotten. Unreal's server keeps the port until the editor closes. */
	static void Stop();
	static bool IsRunning();
	/** A code for the next browser to pair with: good for ten minutes and one browser, and void after five wrong tries. */
	static FString NewCode();
	/** The code on offer; empty when there is none. */
	static FString GetCode();
	/** Every paired browser must pair again. */
	static void ForgetBrowsers();
	/** Where the page opens: this computer, and this network's addresses when other devices may open it. */
	static TArray<FString> GetAddresses();
	/** The editor's window for it: on or off, the addresses and the pairing code. */
	static void OpenDialog();
};
