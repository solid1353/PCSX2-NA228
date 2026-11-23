// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

struct Pcsx2Config;

// Delay-based lockstep netplay between two PCs. The host opens a room on the matchmaking server and waits, without
// running the game, until a guest joins with the room's code; the server passes each PC's connection details to the
// other, and the two connect through their routers with ICE (libjuice, with Cloudflare's STUN, and Cloudflare's TURN
// relay when a direct path fails). Each checks that the other has the same emulator build, game and BIOS, the host
// picks the input delay from the ping it measures, and both boot the game from power-on. From the first VSync, both
// send their own pad input for the frame `input_delay` VSyncs ahead, wait for the other's input for the current frame,
// then run it with the host's input in pad 1 and the guest's in pad 2. During the match the host raises the delay when
// frames keep waiting and lowers it when the ping allows, and the guest follows; the delay only decides how early each
// input is sent, never what a frame gets. Both games stay identical because both run
// every frame with the same inputs from the same power-on, with no memory cards and no savestates; every 2 seconds
// both compare a checksum of EE and IOP RAM, and the first difference ends the match.
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
		// The room: the one the guest joins, or the one the host takes when the NA2.28 app opened it before
		// starting PCSX2. A host without one opens a room on the server.
		std::string room_code;
		// The disc image this PC boots for the match.
		std::string game_path;
		// For tests: every packet from the other PC is held this long, plus up to `jitter_ms` more at random, as a
		// slower or less steady connection would deliver it.
		double lag_ms = 0.0;
		double jitter_ms = 0.0;
	};

	/// Opens or joins a room and connects to the other PC, before anything runs. Once both PCs are ready, the room
	/// state says so, and the caller boots `game_path` with the session in place.
	void Configure(SessionConfig config);

	/// Where a session stands, which PINE reports to the NA2.28 app.
	enum class Stage : u8
	{
		// The two PCs pass each other their connection details through the room.
		Matching,
		Connecting,
		// The host times round trips and sets the delay from them.
		Measuring,
		// Both PCs are ready, and the game boots.
		Starting,
		// The match runs, from its first VSync.
		Playing,
		Ended,
	};

	struct RoomState
	{
		// The host's room code, once the server has given one.
		std::string code;
		// What the session is doing, for the room window.
		std::string status;
		Stage stage = Stage::Matching;
		// The round trip the host measured and the input delay both PCs use, once known.
		u32 ping_ms = 0;
		u32 input_delay = 0;
		// Once the two PCs reach each other: whether they talk through the relay rather than directly.
		bool connected = false;
		bool relayed = false;
		// Both PCs are ready: the game boots now.
		bool ready = false;
		// Why the session ended, once it has: a word for the NA2.28 app, such as "different_bios", which it words
		// in its own languages, and what PCSX2 shows and logs.
		std::string end_code;
		std::string end_reason;
	};

	/// The session's room, for the UI; safe from any thread.
	RoomState GetRoomState();

	/// The session's status, for PINE: key=value lines giving the stage, the ping, the input delay, the route, once
	/// the session has ended why, and during a match the frame it plays, the last second's speed, the time it waited
	/// for the other PC, how often frames waited noticeably or ran late on this one, and the longest of each. A status
	/// that says why counts as reporting it. Safe from any thread.
	std::string ReportStatus();

	/// An agent's input, through PINE: from game frame `frame` on, counted from power-on as the match counts them, this
	/// PC's player plays `state`, in the input recording's 18-byte DualShock 2 layout, instead of its pad, until a later
	/// entry; an entry without a state hands the player back to the pad. Because the match takes each frame's input at
	/// that frame, whatever the input delay, a script lands on the same frames every run. False without a session.
	/// Safe from any thread.
	bool ScheduleAgentInput(u32 frame, const std::optional<std::array<u8, 18>>& state);

	/// Whether PINE has reported why the session ended; a hidden PCSX2 waits for it before quitting.
	bool IsEndReported();

	struct ConnectionStats
	{
		// The latest round trip to the other PC.
		float ping_ms = 0.0f;
		u32 input_delay = 0;
		// Whether the two PCs talk through Cloudflare's relay rather than directly.
		bool relayed = false;
		// How long a VSync waits for the other player's input, averaged over about a second.
		float wait_ms = 0.0f;
		// Over the whole match: the VSyncs that waited noticeably for the other player's input, the longest wait and
		// all the waiting, and the VSyncs this PC itself ran late and the latest one.
		u32 waits = 0;
		float longest_wait_ms = 0.0f;
		float total_wait_ms = 0.0f;
		u32 late_frames = 0;
		float longest_late_ms = 0.0f;
		// The last full second's speed, as a share of full speed: both PCs run at the slower one's.
		float speed_percent = 100.0f;
	};

	/// The connection while a match plays, for the connection overlay; nothing otherwise. Safe from any thread.
	std::optional<ConnectionStats> GetConnectionStats();

	/// Whether the connection overlay shows during matches; safe from any thread.
	bool IsOverlayShown();
	void SetOverlayShown(bool shown);

	/// Whether a configured session has not ended. The PS2 clock is fixed while one is, and savestates are refused.
	bool IsActive();

	/// While a session is set up, puts every setting that changes how the game runs back to PCSX2's defaults, so
	/// two PCs with different settings run the same game: CPU, speed hacks and game fixes (the game database's own
	/// still apply), cheats and patches, GS readbacks, the network adapter and USB devices, fast boot, both pads
	/// connected without multitaps, and no memory cards. Runs each time the core settings load.
	void LockSettings(Pcsx2Config& config);

	/// Puts this PC's own player's input back into pad 1, which holds a player's input from the last VSync, so this
	/// VSync's input changes apply to what the player holds. Runs on the CPU thread once per VSync, before input is read.
	void BeforeInputPoll();

	/// Exchanges this VSync's input with the other PC and writes both players' inputs into pads 1 and 2.
	/// Runs on the CPU thread once per VSync, after local input has been read.
	void OnVSync();

	/// Ends the session, giving the reason in the log and on screen and its code to PINE. Pads go back to local
	/// control.
	void End(std::string_view code, std::string_view reason);
} // namespace Netplay
