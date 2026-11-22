// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <string_view>

// Delay-based lockstep netplay between two PCs. Each VSync, both send their own pad input for the frame
// `input_delay` VSyncs ahead, wait for the other's input for the current frame, then run it with the host's
// input in pad 1 and the guest's in pad 2. Both games stay identical because both run every frame with the
// same inputs from the same start.
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

	/// Ends the session, giving the reason on screen. Pads go back to local control.
	void End(std::string_view reason);
} // namespace Netplay
