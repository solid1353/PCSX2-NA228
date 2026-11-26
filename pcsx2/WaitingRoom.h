// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string_view>

// The netplay room a player hosts while playing offline: the NA2.28 app opens the room on the matchmaking server and
// reports through PINE what becomes of it; PCSX2 tells the player and, once the guest joins, closes the game so the app
// can start the match.
namespace WaitingRoom
{
	enum class Event : u8
	{
		// The room opened: the pause menu offers Stop Hosting.
		Opened = 1,
		// The room expired, which the game says on screen.
		Expired = 2,
		// The guest joined: the game says so on screen and closes.
		Joined = 3,
		// The room closed otherwise, as after Stop Hosting.
		Closed = 4,
	};

	/// The app's news of the room, from PINE; `guest` is the guest's name, for Joined. Safe from any thread.
	void Report(Event event, std::string_view guest);

	/// Whether a room is open, so the pause menu offers Stop Hosting. Safe from any thread.
	bool IsOpen();

	/// The player chose Stop Hosting: the item goes, and PINE's status says so until the app reports the room closed.
	void StopHosting();

	/// Whether the player chose Stop Hosting and the app hasn't reported the room closed yet. Safe from any thread.
	bool IsStopHostingChosen();

	/// Whether the game closes for the match the guest joined: it no longer pauses. Safe from any thread.
	bool IsClosing();

	/// Runs on the CPU thread once per VSync: closes the game for the match once the guest's notice has shown and no
	/// save is being written.
	void OnVSync();

	/// Runs on the CPU thread each time the game writes to or erases a memory card.
	void OnMemoryCardWrite();
} // namespace WaitingRoom
