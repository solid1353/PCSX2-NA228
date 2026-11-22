// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <string_view>

// Delay-based lockstep netplay between two PCs. The host plays on its own until a guest joins; then it sends its
// savestate and memory card, which the guest loads, playing on a copy of the card. From the next VSync, both send
// their own pad input for the frame `input_delay` VSyncs ahead, wait for the other's input for the current frame,
// then run it with the host's input in pad 1 and the guest's in pad 2. Both games stay identical because both run
// every frame with the same inputs from the same state; every 2 seconds both compare a checksum of EE and IOP RAM,
// and the first difference ends the match.
namespace Netplay
{
	inline constexpr u32 MAX_INPUT_DELAY = 30;

	struct SessionConfig
	{
		bool host = false;
		// The host's address, for the guest.
		std::string peer_host;
		// The host listens on this UDP port; the guest sends to it.
		u16 port = 0;
		u32 input_delay = 2;
	};

	/// Sets up a session for the next boot. The first VSync after boot opens the connection.
	void Configure(SessionConfig config);

	/// Whether a configured session has not ended. The PS2 clock is fixed while one is.
	bool IsActive();

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
