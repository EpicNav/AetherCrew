// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * What phone playtests need from Epic's Pixel Streaming 2 plugin, which HyperAI does not link: the optional
 * HyperAIStudioPixelStreaming module implements it when the project enables the plugin, and registers it here.
 */
class IHyperAIStudioPixelStreamingBackend
{
public:
	virtual ~IHyperAIStudioPixelStreamingBackend() = default;

	/**
	 * Starts Pixel Streaming's own C++ signalling server in this editor, with no download: HttpPort serves the player page
	 * and its viewers, StreamerPort the game's stream. AllowedOrigins limits which pages' websockets it takes.
	 */
	virtual bool StartSignalling(int32 HttpPort, int32 StreamerPort, const FString& AllowedOrigins, FString& OutError) = 0;
	virtual void StopSignalling() = 0;
	virtual bool IsSignallingReady() const = 0;

	/** The folder of the player page the server serves, whether or not it exists. */
	virtual FString PlayerPageFolder() const = 0;

	/**
	 * While on, every Play In Editor session is streamed to the server at StreamerPort, at most MaxViewers watching (more
	 * are turned away), every viewer's input reaching the game when bAllViewersControl, else the first's. Off stops it.
	 */
	virtual void SetPlayInEditorStreaming(bool bOn, int32 StreamerPort, int32 MaxViewers, bool bAllViewersControl) = 0;

	/** The streamer id a PIE session streams as. */
	virtual FString PlayInEditorStreamerId() const = 0;

	/** Viewers watching a PIE session now. */
	virtual int32 NumViewers() const = 0;
};

/** The registered backend, or null when the Pixel Streaming 2 plugin is not enabled. Game thread. */
class HYPERAISTUDIO_API FHyperAIStudioPixelStreamingBackends
{
public:
	static void Register(TSharedPtr<IHyperAIStudioPixelStreamingBackend> Backend);
	static void Unregister();
	static TSharedPtr<IHyperAIStudioPixelStreamingBackend> Get();
};
