// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioPIEPlaytestToolset.h"

/**
 * Phone playtests: the game streamed by Pixel Streaming 2 to phones on this PC's local network. The server is Pixel
 * Streaming's own C++ signalling server inside this editor; nothing is downloaded and nothing is reached on the internet.
 * It listens on all of this PC's network interfaces (the engine's server has no bind address), so HyperAI refuses on a PC
 * with a public internet address and lets websockets in only from pages served on its LAN address or this PC.
 */
namespace HyperAIStudio::PhonePlaytest
{
	constexpr int32 MaxViewers = 8;
	constexpr int32 MinPort = 1024;
	constexpr int32 MaxPort = 65535;
	/** The id a Standalone game streams as. */
	constexpr const TCHAR* StandaloneStreamerId = TEXT("HyperAIPlaytest");

	enum class EAddressKind : uint8
	{
		Loopback,
		/** 10/8, 172.16/12 and 192.168/16: a home or office network behind a router. */
		Private,
		/** 169.254/16, which a phone cannot route to. */
		LinkLocal,
		/** 100.64/10, a carrier's network: not this LAN. */
		CarrierNat,
		/** Anything else, reachable from the internet. */
		Public,
		Invalid
	};

	EAddressKind ClassifyIpv4(const FString& Address);

	/** The address phones should use: Requested when it is private and this PC's, else the first private one (192.168 first). */
	FString PickLanAddress(const TArray<FString>& Addresses, const FString& Requested, FString& OutError);

	/** The websocket origins the server takes: pages from the LAN address or this PC on HttpPort, and the game's own stream. */
	FString AllowedOrigins(const FString& LanAddress, int32 HttpPort);

	/** What a phone opens: the player page, connecting at once, muted, with touch input. */
	FString PlayerUrl(const FString& LanAddress, int32 HttpPort, const FString& StreamerId);

	/** A Standalone game's command line after the project path. */
	FString GameArguments(const FHyperAIPhonePlaytestRequest& Request, const FString& Map, const FString& GameLogPath);

	/** The request's shape: ports, viewers, size, target fields. False with OutError. */
	bool Validate(const FHyperAIPhonePlaytestRequest& Request, FString& OutError);

	/** This PC's IPv4 addresses, from its network adapters. */
	TArray<FString> LocalAddresses();

	/** Whether a TCP port on this PC is free to listen on. */
	bool IsPortFree(int32 Port);

	FHyperAIPhonePlaytestReport Start(const FHyperAIPhonePlaytestRequest& Request);
	FHyperAIPhonePlaytestReport Status();
	FHyperAIPhonePlaytestReport Stop();

	/** Tests only: replaces the adapter addresses LocalAddresses reports; empty restores the real ones. */
	void SetAddressesForTests(const TArray<FString>& Addresses);
}
