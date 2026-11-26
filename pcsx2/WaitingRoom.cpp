// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "WaitingRoom.h"

#include "Host.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Timer.h"

#include "IconsFontAwesome.h"
#include "fmt/format.h"

#include <atomic>
#include <string>

namespace WaitingRoom
{
	// How long the guest's notice shows at least before the game closes; it stays until the game closes.
	static constexpr double NOTICE_SECONDS = 3.0;
	// How long the game must run without writing to a memory card before it closes: a save writes its files one after
	// another, each in a few frames, so this long without a write means no save is under way, and closing can't leave
	// one half written.
	static constexpr u64 SAVE_QUIET_VSYNCS = 120;
	static constexpr const char* OSD_KEY = "WaitingRoom";

	static std::atomic<bool> s_open{false};
	static std::atomic<bool> s_stop_chosen{false};
	static std::atomic<bool> s_closing{false};
	// When the guest joined, and the notice saying so; set before s_closing.
	static Common::Timer::Value s_joined_at = 0;
	static std::string s_joined_notice;

	// The CPU thread's own: the VSyncs it has run, the latest one that wrote to a memory card, and whether it asked
	// for the game to close.
	static u64 s_vsyncs = 0;
	static u64 s_last_write_vsync = 0;
	static bool s_wrote = false;
	static bool s_shutdown_requested = false;

	void Report(Event event, std::string_view guest)
	{
		switch (event)
		{
			case Event::Opened:
				s_stop_chosen.store(false, std::memory_order_release);
				s_open.store(true, std::memory_order_release);
				Console.WriteLn("Waiting room: the room is open.");
				break;

			case Event::Expired:
				s_open.store(false, std::memory_order_release);
				s_stop_chosen.store(false, std::memory_order_release);
				Console.WriteLn("Waiting room: the room expired.");
				Host::AddIconOSDMessage(OSD_KEY, ICON_FA_TOWER_BROADCAST, TRANSLATE_STR("Netplay", "Your room expired."),
					Host::OSD_INFO_DURATION);
				break;

			case Event::Joined:
			{
				s_open.store(false, std::memory_order_release);
				s_stop_chosen.store(false, std::memory_order_release);
				const std::string name(guest);
				Console.WriteLnFmt("Waiting room: {} joined.", name);
				Host::RemoveKeyedOSDMessage(OSD_KEY);
				s_joined_notice = fmt::format(TRANSLATE_FS("Netplay", "{} joined. Starting the match…"), name);
				s_joined_at = Common::Timer::GetCurrentValue();
				s_closing.store(true, std::memory_order_release);
				// A paused game resumes, closing the pause menu, so the player sees the notice and a save the game
				// was writing can finish.
				Host::RunOnCPUThread([]() {
					if (VMManager::GetState() == VMState::Paused)
						VMManager::SetPaused(false);
				});
				break;
			}

			case Event::Closed:
				s_open.store(false, std::memory_order_release);
				s_stop_chosen.store(false, std::memory_order_release);
				Console.WriteLn("Waiting room: the room closed.");
				break;
		}
	}

	bool IsOpen()
	{
		return s_open.load(std::memory_order_acquire);
	}

	void StopHosting()
	{
		if (!s_open.exchange(false, std::memory_order_acq_rel))
			return;
		s_stop_chosen.store(true, std::memory_order_release);
		Console.WriteLn("Waiting room: the player stops hosting.");
	}

	bool IsStopHostingChosen()
	{
		return s_stop_chosen.load(std::memory_order_acquire);
	}

	bool IsClosing()
	{
		return s_closing.load(std::memory_order_acquire);
	}

	std::optional<std::string> GetJoinedNotice()
	{
		if (!s_closing.load(std::memory_order_acquire))
			return std::nullopt;
		return s_joined_notice;
	}

	void OnVSync()
	{
		s_vsyncs++;
		if (s_shutdown_requested || !s_closing.load(std::memory_order_acquire))
			return;
		if (Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_joined_at) < NOTICE_SECONDS)
			return;
		if (s_wrote && s_vsyncs - s_last_write_vsync < SAVE_QUIET_VSYNCS)
			return;
		s_shutdown_requested = true;
		Console.WriteLnFmt("Waiting room: closing the game for the match, {} VSyncs after its last memory card write.",
			s_wrote ? s_vsyncs - s_last_write_vsync : s_vsyncs);
		Host::RequestVMShutdown(false, false, false);
	}

	void OnMemoryCardWrite()
	{
		s_last_write_vsync = s_vsyncs;
		s_wrote = true;
	}
} // namespace WaitingRoom
