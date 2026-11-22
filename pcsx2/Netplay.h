// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <string_view>

struct Pcsx2Config;

// Delay-based lockstep netplay between two PCs. The host opens a room on the matchmaking server and plays on its own
// until a guest joins with the room's code; the server passes each PC's connection details to the other, and the two
// connect through their routers with ICE (libjuice, with Cloudflare's STUN, and Cloudflare's TURN relay when a
// direct path fails). Both first check that they play the same emulator build, game, BIOS and input delay. The
// host then sends its savestate and memory card, which the guest loads, playing on a copy of the card. From the next
// VSync, both send their own pad input for the frame `input_delay` VSyncs ahead, wait for the other's input for the
// current frame, then run it with the host's input in pad 1 and the guest's in pad 2. Both games stay identical
// because both run every frame with the same inputs from the same state; every 2 seconds both compare a checksum of
// EE and IOP RAM, and the first difference ends the match.
namespace Netplay
{
	inline constexpr u32 MAX_INPUT_DELAY = 30;

	// The matchmaking server, matchmaking.js in NA228's scripts/online, deployed by `na online deploy`.
	inline constexpr const char* MATCHMAKING_URL = "https://matchmaking.na228.workers.dev";

	/// Whether this is a room code the matchmaking server can give: six of its letters and digits, in capitals.
	bool IsRoomCode(std::string_view code);

	struct SessionConfig
	{
		bool host = false;
		// The room the guest joins; the host gets a new one from the server.
		std::string room_code;
		u32 input_delay = 2;
	};

	/// Sets up a session for the next boot. The first VSync after boot opens the connection.
	void Configure(SessionConfig config);

	struct RoomState
	{
		// The host's room code, once the server has given one.
		std::string code;
		// Why the session ended, once it has.
		std::string end_reason;
	};

	/// The session's room and whether it ended, for the UI; safe from any thread.
	RoomState GetRoomState();

	/// Whether a configured session has not ended. The PS2 clock is fixed while one is.
	bool IsActive();

	/// While a session is set up, puts every setting that changes how the game runs back to PCSX2's defaults, so
	/// two PCs with different settings run the same game: CPU, speed hacks and game fixes (the game database's own
	/// still apply), cheats and patches, GS readbacks, the network adapter and USB devices, and both pads connected
	/// without multitaps. Runs each time the core settings load.
	void LockSettings(Pcsx2Config& config);

	/// Exchanges this VSync's input with the other PC and writes both players' inputs into pads 1 and 2.
	/// Runs on the CPU thread once per VSync, after local input has been read.
	void OnVSync();

	/// Runs on the CPU thread each time emulation stops running, outside any VSync handler: where the host saves
	/// the start and the guest loads it.
	void OnExecutionExit();

	/// Ends the session, giving the reason on screen. Pads go back to local control, and a guest's slot 1 to its
	/// own memory card.
	void End(std::string_view reason);

	/// The memory card file a memory card slot uses during the session in place of its configured one, or empty.
	std::string_view SessionMemoryCard(u32 slot);
} // namespace Netplay
