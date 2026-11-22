// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "pcsx2/Netplay.h"

#include <optional>

class QWidget;

// The windows behind the main window's Netplay menu.
namespace NetplayDialogs
{
	/// Asks for a match's settings: the input delay, and the room code when joining. Returns nothing when cancelled.
	std::optional<Netplay::SessionConfig> AskForMatch(QWidget* parent, bool host);

	/// Shows the host's room code once the matchmaking server gives it, with a button that copies it, and why the
	/// session ended if it does, until the window is closed.
	void ShowRoom(QWidget* parent);
} // namespace NetplayDialogs
