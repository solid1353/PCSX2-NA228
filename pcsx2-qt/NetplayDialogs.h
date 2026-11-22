// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <functional>
#include <optional>
#include <string>

class QDialog;
class QWidget;

// The windows behind the main window's Netplay menu.
namespace NetplayDialogs
{
	/// Asks for the code of the room to join. Returns nothing when cancelled.
	std::optional<std::string> AskForRoomCode(QWidget* parent);

	/// Opens the Netplay window, which follows the session until the game boots: the host's room code with a button
	/// that copies it, what the session is doing, the ping and input delay once measured, and why it ended if it
	/// does. Rejecting it, with Cancel or its close button, calls `cancel` while the session runs; the boot accepts it.
	QDialog* ShowRoom(QWidget* parent, bool host, std::function<void()> cancel);
} // namespace NetplayDialogs
