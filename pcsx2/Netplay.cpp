// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Netplay.h"
#include "Config.h"
#include "Host.h"
#include "MemoryTypes.h"
#include "R5900.h"
#include "VMManager.h"
#include "Recording/PadData.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Pad/PadTypes.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/HTTPDownloader.h"
#include "common/Path.h"
#include "common/Timer.h"

#include "fmt/format.h"

// PCSX2 builds xxHash into each user, as GS/GSXXH.h does.
#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include <xxhash.h>

#define JUICE_STATIC
#include <juice/juice.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
// libjuice draws its random numbers from Windows' cryptography API.
#pragma comment(lib, "bcrypt.lib")
#endif

namespace Netplay
{

	// One pad's state, in the 18-byte layout input recordings use (InputRecordingFile::writePadData).
	using Input = std::array<u8, 18>;

	// Every packet starts with magic, version, input delay, an input count and its type.
	static constexpr u32 MAGIC = 0x4e32414e; // "NA2N"
	static constexpr u8 VERSION = 3;
	static constexpr size_t COMMON_SIZE = 8;

	enum class PacketType : u8
	{
		// The first local frame the sender lacks from us (its acknowledgement), the first frame of the inputs that
		// follow, the sender's newest memory checksum (its frame plus one, 0 for none, then the checksum), then
		// `count` inputs.
		Input = 0,
		// The guest asks to join; nothing follows.
		Hello = 1,
		// A piece of the host's start: the start's total size, this piece's offset, then its bytes.
		Start = 2,
		// The guest has every byte of the start before this offset.
		StartAck = 3,
	};

	static constexpr size_t INPUT_HEADER_SIZE = 28;
	static constexpr u32 MAX_INPUTS_PER_PACKET = 64;
	static constexpr size_t START_HEADER_SIZE = 16;
	static constexpr u32 START_PIECE_SIZE = 1152;
	static constexpr size_t MAX_PACKET_SIZE = std::max(INPUT_HEADER_SIZE + MAX_INPUTS_PER_PACKET * sizeof(Input),
		START_HEADER_SIZE + START_PIECE_SIZE);

	// Inputs in flight never span more than twice the delay, well inside the ring.
	static constexpr u32 RING_SIZE = 256;
	static constexpr double RESEND_MS = 5.0;
	static constexpr double HELLO_MS = 100.0;
	static constexpr double FIRST_CONTACT_TIMEOUT_MS = 60000.0;
	static constexpr double PEER_TIMEOUT_MS = 10000.0;

	// The matchmaking server forgets a room ten minutes after it opens; the host stops waiting a little before.
	static constexpr double ROOM_WAIT_MS = 9.5 * 60.0 * 1000.0;
	static const std::string EMPTY_POST;
	// The server stores a description once and answers 204, No Content.
	static constexpr s32 STORED = 204;
	static constexpr float MATCHMAKING_TIMEOUT_S = 10.0f;
	static constexpr u32 MATCHMAKING_ATTEMPTS = 3;
	static constexpr const char* STUN_HOST = "stun.cloudflare.com";
	static constexpr u16 STUN_PORT = 3478;

	// The start goes out in a window of pieces past the guest's acknowledgement, from the acknowledgement again
	// when it stops moving.
	static constexpr u32 START_WINDOW_PIECES = 256;
	static constexpr double START_STALL_MS = 100.0;

	// Every 2 seconds at 60 VSyncs a second, both PCs checksum EE and IOP RAM at the start of the same VSync.
	// Identical games give identical checksums; the first difference ends the match.
	static constexpr u32 CHECK_INTERVAL = 120;
	static constexpr u32 CHECK_RING_SIZE = 16;
	// How many matching checks between log lines, about a minute's worth.
	static constexpr u32 CHECKS_PER_LOG = 30;

	// Where the start goes on disk: the host saves its state there, the guest loads it from there and plays on a
	// copy of the host's memory card, leaving its own card untouched.
	static constexpr const char* START_STATE_FILE = "netplay start.p2s";
	static constexpr const char* SESSION_CARD_FILE = "netplay session.ps2";

	// A state saved or loaded inside a VSync handler would differ between the PCs: the rest of the handler runs on
	// values from each PC's own timeline. So the start waits for emulation to stop at the end of that VSync.
	enum class Phase
	{
		// The host plays on its own until a guest says hello.
		WaitingForGuest,
		// The host saves and sends the start once emulation stops.
		Starting,
		// The guest waits for the host's start.
		Joining,
		// The guest loads the start once emulation stops.
		Loading,
		// Both run every VSync on both players' inputs.
		Playing,
	};

	struct Slot
	{
		// The slot's frame plus one; 0 while empty.
		u32 tag = 0;
		Input input{};
	};

	struct Check
	{
		// The checked frame plus one; 0 while empty.
		u32 tag = 0;
		u64 checksum = 0;
	};

	static SessionConfig s_config;
	static bool s_configured = false;
	static bool s_ended = false;
	static bool s_opened = false;
	static Phase s_phase = Phase::WaitingForGuest;
	// The connection, libjuice's agent. Its callbacks run on libjuice's thread and hand packets over through the inbox.
	static juice_agent_t* s_agent = nullptr;
	static std::atomic<bool> s_connected{false};
	static std::atomic<bool> s_connection_failed{false};
	static std::mutex s_inbox_mutex;
	static std::condition_variable s_inbox_changed;
	static std::deque<std::vector<u8>> s_inbox;
	static bool s_gathered = false;
	// Matchmaking runs on its own thread, so the host plays on meanwhile; the CPU thread reads what it found.
	static std::thread s_matchmaking;
	static std::atomic<bool> s_stop_matchmaking{false};
	static std::mutex s_matchmaking_mutex;
	static std::string s_room_code;
	static std::string s_matchmaking_error;
	static bool s_room_code_shown = false;
	// The host has had a guest's hello.
	static bool s_hello = false;
	static bool s_heard_from_peer = false;
	// The host's state and memory card: state size, card size, state, card.
	static std::vector<u8> s_start;
	// The guest has every byte of the start before this offset.
	static u32 s_start_until = 0;
	static u32 s_start_size = 0;
	// The guest's copy of the host's memory card, which slot 1 uses during the session.
	static std::string s_session_card;
	// The VSync about to run, counted from the start.
	static u32 s_frame = 0;
	// Every remote input before this frame has arrived.
	static u32 s_remote_until = 0;
	// The other PC has every local input before this frame.
	static u32 s_peer_until = 0;
	static std::array<Slot, RING_SIZE> s_local;
	static std::array<Slot, RING_SIZE> s_remote;
	static std::array<Check, CHECK_RING_SIZE> s_local_checks;
	static std::array<Check, CHECK_RING_SIZE> s_remote_checks;
	// The newest local check, sent with every input packet; its tag is 0 before the first.
	static Check s_newest_check;
	// Every check before this frame has been compared.
	static u32 s_compared_until = 0;
	static u32 s_matching_checks = 0;

	// Every button released (a set flag bit means released), both sticks centred, no pressure.
	static constexpr Input NEUTRAL_INPUT = {0xff, 0xff, PadData::ANALOG_VECTOR_NEUTRAL, PadData::ANALOG_VECTOR_NEUTRAL,
		PadData::ANALOG_VECTOR_NEUTRAL, PadData::ANALOG_VECTOR_NEUTRAL};

	static Input ToInput(const PadData& pad)
	{
		return {pad.m_compactPressFlagsGroupOne, pad.m_compactPressFlagsGroupTwo,
			std::get<0>(pad.m_rightAnalog), std::get<1>(pad.m_rightAnalog),
			std::get<0>(pad.m_leftAnalog), std::get<1>(pad.m_leftAnalog),
			std::get<1>(pad.m_right), std::get<1>(pad.m_left), std::get<1>(pad.m_up), std::get<1>(pad.m_down),
			std::get<1>(pad.m_triangle), std::get<1>(pad.m_circle), std::get<1>(pad.m_cross), std::get<1>(pad.m_square),
			std::get<1>(pad.m_l1), std::get<1>(pad.m_r1), std::get<1>(pad.m_l2), std::get<1>(pad.m_r2)};
	}

	static void Write32(u8* out, u32 value)
	{
		out[0] = static_cast<u8>(value);
		out[1] = static_cast<u8>(value >> 8);
		out[2] = static_cast<u8>(value >> 16);
		out[3] = static_cast<u8>(value >> 24);
	}

	static u32 Read32(const u8* in)
	{
		return u32(in[0]) | (u32(in[1]) << 8) | (u32(in[2]) << 16) | (u32(in[3]) << 24);
	}

	static void Write64(u8* out, u64 value)
	{
		Write32(out, static_cast<u32>(value));
		Write32(out + 4, static_cast<u32>(value >> 32));
	}

	static u64 Read64(const u8* in)
	{
		return u64(Read32(in)) | (u64(Read32(in + 4)) << 32);
	}

	static u64 MemoryChecksum()
	{
		const u64 ee = XXH3_64bits(eeMem->Main, Ps2MemSize::MainRam);
		return XXH3_64bits_withSeed(iopMem->Main, Ps2MemSize::IopRam, ee);
	}

	static void SetBothPads(const Input& pad1, const Input& pad2)
	{
		PadData(0, 0, pad1).OverrideActualController();
		PadData(1, 0, pad2).OverrideActualController();
	}

	// Reads a file PCSX2 itself keeps open for writing, as it does the memory cards.
	static std::optional<std::vector<u8>> ReadOpenFile(const std::string& path)
	{
		std::FILE* file = FileSystem::OpenSharedCFile(path.c_str(), "rb", FileSystem::FileShareMode::DenyNone);
		if (!file)
			return std::nullopt;

		std::optional<std::vector<u8>> data;
		const s64 size = FileSystem::FSize64(file);
		if (size >= 0)
		{
			data.emplace(static_cast<size_t>(size));
			if (size > 0 && std::fread(data->data(), static_cast<size_t>(size), 1, file) != 1)
				data.reset();
		}
		std::fclose(file);
		return data;
	}

	// Closes and reopens the memory cards, which writes everything to disk and picks up a changed slot 1 card.
	static void ReopenMemoryCards()
	{
		FileMcd_EmuClose();
		FileMcd_SetType();
		FileMcd_EmuOpen();
	}

	// Compares the two PCs' checksums of one frame once both have arrived.
	static void Compare(u32 frame)
	{
		const Check& local = s_local_checks[(frame / CHECK_INTERVAL) % CHECK_RING_SIZE];
		const Check& remote = s_remote_checks[(frame / CHECK_INTERVAL) % CHECK_RING_SIZE];
		if (frame < s_compared_until || local.tag != frame + 1 || remote.tag != frame + 1)
			return;

		s_compared_until = frame + 1;
		if (local.checksum != remote.checksum)
		{
			End(fmt::format("the two games went out of sync by VSync {}", frame));
			return;
		}

		s_matching_checks++;
		if (s_matching_checks == 1 || s_matching_checks % CHECKS_PER_LOG == 0)
			Console.WriteLn(fmt::format("Netplay: both games identical at VSync {} ({} checks so far).", frame, s_matching_checks));
	}

	// libjuice's callbacks run on its own thread; they record the connection's state and hand packets over.
	static void OnJuiceStateChanged(juice_agent_t*, juice_state_t state, void*)
	{
		s_connected.store(state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED, std::memory_order_release);
		if (state == JUICE_STATE_FAILED)
			s_connection_failed.store(true, std::memory_order_release);
		s_inbox_changed.notify_all();
	}

	static void OnJuiceLog(juice_log_level_t level, const char* message)
	{
		if (level >= JUICE_LOG_LEVEL_WARN)
			Console.WarningFmt("libjuice: {}", message);
		else
			Console.WriteLnFmt("libjuice: {}", message);
	}

	static void OnJuiceGatheringDone(juice_agent_t*, void*)
	{
		{
			std::lock_guard lock(s_inbox_mutex);
			s_gathered = true;
		}
		s_inbox_changed.notify_all();
	}

	static void OnJuiceReceived(juice_agent_t*, const char* data, size_t size, void*)
	{
		{
			std::lock_guard lock(s_inbox_mutex);
			s_inbox.emplace_back(reinterpret_cast<const u8*>(data), reinterpret_cast<const u8*>(data) + size);
		}
		s_inbox_changed.notify_all();
	}

	static void SetMatchmakingError(std::string error)
	{
		std::lock_guard lock(s_matchmaking_mutex);
		if (s_matchmaking_error.empty())
			s_matchmaking_error = std::move(error);
	}

	// One request to the matchmaking server: a GET, or a POST of `post`. Returns the HTTP status, or a negative
	// HTTPDownloader error, and fills `body`. Each request opens its own connection: a connection kept open across
	// requests can stall for good on some networks, which then cuts it after half a minute.
	static s32 AskMatchmaking(const std::string& path, const std::string* post, std::string* body)
	{
		const std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create();
		if (!http)
			return HTTPDownloader::HTTP_STATUS_ERROR;
		http->SetTimeout(MATCHMAKING_TIMEOUT_S);
		s32 status = HTTPDownloader::HTTP_STATUS_ERROR;
		const auto done = [&status, body](s32 code, const std::string&, HTTPDownloader::Request::Data data) {
			status = code;
			if (body)
				body->assign(data.begin(), data.end());
		};
		const std::string url = std::string(MATCHMAKING_URL) + path;
		if (post)
			http->CreatePostRequest(url, *post, done);
		else
			http->CreateRequest(url, done);
		http->WaitForAllRequests();
		return status;
	}

	// Posts to the matchmaking server, trying again while the request fails on the way. A description is stored
	// once, so a refusal after a failed try means that try got through.
	static s32 PostToMatchmaking(const std::string& path, const std::string& post, std::string* body)
	{
		s32 status = HTTPDownloader::HTTP_STATUS_ERROR;
		for (u32 attempt = 0; attempt < MATCHMAKING_ATTEMPTS && !s_stop_matchmaking.load(std::memory_order_acquire); attempt++)
		{
			status = AskMatchmaking(path, &post, body);
			if (status >= 0)
				return (status == 409 && attempt > 0) ? STORED : status;
		}
		return status;
	}

	// Asks for the other side's description every half second until it arrives. Returns false once matchmaking
	// stops, the room is gone, or `timeout_ms` passes.
	static bool AwaitDescription(const std::string& path, double timeout_ms, std::string* description)
	{
		const Common::Timer::Value start = Common::Timer::GetCurrentValue();
		while (!s_stop_matchmaking.load(std::memory_order_acquire))
		{
			const s32 status = AskMatchmaking(path, nullptr, description);
			if (status == HTTPDownloader::HTTP_STATUS_OK && !description->empty())
				return true;
			if (status == 404)
			{
				SetMatchmakingError("there is no room with that code");
				return false;
			}
			if (Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - start) >= timeout_ms)
			{
				SetMatchmakingError("the other player never connected");
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		return false;
	}

	static bool UseRemoteDescription(const std::string& description)
	{
		if (juice_set_remote_description(s_agent, description.c_str()) != JUICE_ERR_SUCCESS)
		{
			SetMatchmakingError("the other player's connection details are damaged");
			return false;
		}
		juice_set_remote_gathering_done(s_agent);
		return true;
	}

	// Creates the agent and gathers this PC's candidates, from its own addresses and from Cloudflare's STUN, then
	// returns the description that carries them. The guest gives the host's description first: an agent that
	// gathers before it has the other's takes the controlling role, and one of the two must be controlled.
	static bool GatherDescription(const std::string* remote, std::string* description)
	{
		juice_set_log_handler(OnJuiceLog);
		juice_set_log_level(JUICE_LOG_LEVEL_INFO);
		juice_config_t config = {};
		config.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
		config.stun_server_host = STUN_HOST;
		config.stun_server_port = STUN_PORT;
		config.cb_state_changed = OnJuiceStateChanged;
		config.cb_gathering_done = OnJuiceGatheringDone;
		config.cb_recv = OnJuiceReceived;
		s_agent = juice_create(&config);
		if (!s_agent)
		{
			SetMatchmakingError("the connection could not be set up");
			return false;
		}
		if (remote && !UseRemoteDescription(*remote))
			return false;
		if (juice_gather_candidates(s_agent) != JUICE_ERR_SUCCESS)
		{
			SetMatchmakingError("the connection could not be set up");
			return false;
		}

		std::unique_lock lock(s_inbox_mutex);
		if (!s_inbox_changed.wait_for(lock, std::chrono::seconds(10), [] { return s_gathered || s_stop_matchmaking.load(); }) ||
			!s_gathered)
		{
			if (!s_stop_matchmaking.load())
				SetMatchmakingError("this PC's network addresses could not be found");
			return false;
		}
		lock.unlock();

		std::array<char, JUICE_MAX_SDP_STRING_LEN> buffer = {};
		if (juice_get_local_description(s_agent, buffer.data(), buffer.size()) != JUICE_ERR_SUCCESS)
		{
			SetMatchmakingError("this PC's connection details could not be read");
			return false;
		}
		*description = buffer.data();
		return true;
	}

	// The host: opens a room, posts its description, and waits for a guest's. The CPU thread shows the room's code.
	static void MatchAsHost()
	{
		std::string reply;
		if (PostToMatchmaking("/rooms", EMPTY_POST, &reply) != HTTPDownloader::HTTP_STATUS_OK)
		{
			SetMatchmakingError("the matchmaking server could not be reached");
			return;
		}
		const size_t start = reply.find("\"code\":\"");
		const size_t end = start == std::string::npos ? std::string::npos : reply.find('"', start + 8);
		if (end == std::string::npos)
		{
			SetMatchmakingError("the matchmaking server gave no room code");
			return;
		}
		const std::string code = reply.substr(start + 8, end - start - 8);
		{
			std::lock_guard lock(s_matchmaking_mutex);
			s_room_code = code;
		}

		std::string description, guest;
		if (!GatherDescription(nullptr, &description))
			return;
		if (PostToMatchmaking(fmt::format("/rooms/{}/host", code), description, nullptr) != STORED)
		{
			SetMatchmakingError("the matchmaking server refused this game's connection details");
			return;
		}
		// The room lives ten minutes; a guest has until shortly before then to join.
		if (AwaitDescription(fmt::format("/rooms/{}/guest", code), ROOM_WAIT_MS, &guest))
			UseRemoteDescription(guest);
	}

	// The guest: waits for the host's description in the room, starts connecting with it, and posts its own.
	static void MatchAsGuest(std::string code)
	{
		std::string host, description;
		if (!AwaitDescription(fmt::format("/rooms/{}/host", code), FIRST_CONTACT_TIMEOUT_MS, &host) ||
			!GatherDescription(&host, &description))
		{
			return;
		}
		const s32 status = PostToMatchmaking(fmt::format("/rooms/{}/guest", code), description, nullptr);
		if (status != STORED)
		{
			SetMatchmakingError(status == 409 ? "someone else already joined that room" :
												"the matchmaking server refused this game's connection details");
		}
	}

	static void CloseConnection()
	{
		s_stop_matchmaking.store(true, std::memory_order_release);
		s_inbox_changed.notify_all();
		if (s_matchmaking.joinable())
			s_matchmaking.join();
		if (s_agent)
		{
			juice_destroy(s_agent);
			s_agent = nullptr;
		}
		s_connected.store(false, std::memory_order_release);
		std::lock_guard lock(s_inbox_mutex);
		s_inbox.clear();
	}

	static bool Open(std::string* error)
	{
		const std::string code = s_config.room_code;
		s_stop_matchmaking.store(false, std::memory_order_release);
		s_matchmaking = s_config.host ? std::thread(MatchAsHost) : std::thread(MatchAsGuest, code);
		if (!s_matchmaking.joinable())
		{
			*error = "matchmaking could not start";
			return false;
		}

		// Frames before the delay have no input from either player: both run them with released pads.
		for (u32 frame = 0; frame < s_config.input_delay; frame++)
		{
			s_local[frame] = {frame + 1, NEUTRAL_INPUT};
			s_remote[frame] = {frame + 1, NEUTRAL_INPUT};
		}
		s_remote_until = s_config.input_delay;
		s_peer_until = s_config.input_delay;
		s_phase = s_config.host ? Phase::WaitingForGuest : Phase::Joining;
		return true;
	}

	// Ends the session when matchmaking or the connection failed, and shows the host's room code once it exists.
	static void CheckMatchmaking()
	{
		std::string error, code;
		{
			std::lock_guard lock(s_matchmaking_mutex);
			error = s_matchmaking_error;
			code = s_room_code;
		}
		if (!error.empty())
		{
			End(error);
			return;
		}
		if (s_connection_failed.load(std::memory_order_acquire))
		{
			End("the two PCs could not connect through their routers");
			return;
		}
		if (!code.empty() && !s_room_code_shown)
		{
			s_room_code_shown = true;
			const std::string message = fmt::format("Netplay: room code {}. Waiting for the other player.", code);
			Console.WriteLn(message);
			Host::AddKeyedOSDMessage("Netplay", message, Host::OSD_ERROR_DURATION);
		}
	}

	static void SendPacket(const u8* packet, size_t size)
	{
		if (s_connected.load(std::memory_order_acquire))
			juice_send(s_agent, reinterpret_cast<const char*>(packet), size);
	}

	static void WriteCommon(u8* packet, PacketType type, u32 count = 0)
	{
		Write32(&packet[0], MAGIC);
		packet[4] = VERSION;
		packet[5] = static_cast<u8>(s_config.input_delay);
		packet[6] = static_cast<u8>(count);
		packet[7] = static_cast<u8>(type);
	}

	static void SendInputs()
	{
		const u32 last = s_frame + s_config.input_delay;
		const u32 first = std::max(s_peer_until, last + 1 > MAX_INPUTS_PER_PACKET ? last + 1 - MAX_INPUTS_PER_PACKET : 0);
		const u32 count = first <= last ? last - first + 1 : 0;

		std::array<u8, MAX_PACKET_SIZE> packet = {};
		WriteCommon(packet.data(), PacketType::Input, count);
		Write32(&packet[8], s_remote_until);
		Write32(&packet[12], first);
		Write32(&packet[16], s_newest_check.tag);
		Write64(&packet[20], s_newest_check.checksum);
		for (u32 i = 0; i < count; i++)
			std::memcpy(&packet[INPUT_HEADER_SIZE + i * sizeof(Input)], s_local[(first + i) % RING_SIZE].input.data(), sizeof(Input));
		SendPacket(packet.data(), INPUT_HEADER_SIZE + count * sizeof(Input));
	}

	static void SendHello()
	{
		std::array<u8, COMMON_SIZE> packet = {};
		WriteCommon(packet.data(), PacketType::Hello);
		SendPacket(packet.data(), packet.size());
	}

	static void SendStartPiece(u32 offset)
	{
		const u32 size = std::min<u32>(START_PIECE_SIZE, static_cast<u32>(s_start.size()) - offset);
		std::array<u8, MAX_PACKET_SIZE> packet = {};
		WriteCommon(packet.data(), PacketType::Start);
		Write32(&packet[8], static_cast<u32>(s_start.size()));
		Write32(&packet[12], offset);
		std::memcpy(&packet[START_HEADER_SIZE], s_start.data() + offset, size);
		SendPacket(packet.data(), START_HEADER_SIZE + size);
	}

	static void SendStartAck()
	{
		std::array<u8, 12> packet = {};
		WriteCommon(packet.data(), PacketType::StartAck);
		Write32(&packet[8], s_start_until);
		SendPacket(packet.data(), packet.size());
	}

	static void HandleInputs(const u8* packet, size_t size)
	{
		const u32 count = packet[6];
		if (size < INPUT_HEADER_SIZE || count > MAX_INPUTS_PER_PACKET || size < INPUT_HEADER_SIZE + count * sizeof(Input))
			return;

		const u32 ack = Read32(&packet[8]);
		if (ack > s_peer_until && ack <= s_frame + s_config.input_delay + 1)
			s_peer_until = ack;

		const u32 first = Read32(&packet[12]);
		for (u32 i = 0; i < count; i++)
		{
			const u32 frame = first + i;
			if (frame < s_remote_until)
				continue;
			if (frame >= s_frame + RING_SIZE)
				break;
			Slot& slot = s_remote[frame % RING_SIZE];
			slot.tag = frame + 1;
			std::memcpy(slot.input.data(), &packet[INPUT_HEADER_SIZE + i * sizeof(Input)], sizeof(Input));
		}
		while (s_remote[s_remote_until % RING_SIZE].tag == s_remote_until + 1)
			s_remote_until++;

		const u32 check_tag = Read32(&packet[16]);
		if (check_tag != 0 && check_tag - 1 >= s_compared_until)
		{
			const u32 frame = check_tag - 1;
			s_remote_checks[(frame / CHECK_INTERVAL) % CHECK_RING_SIZE] = {check_tag, Read64(&packet[20])};
			Compare(frame);
		}
	}

	static void HandleStartPiece(const u8* packet, size_t size)
	{
		if (size < START_HEADER_SIZE)
			return;

		const u32 total = Read32(&packet[8]);
		const u32 offset = Read32(&packet[12]);
		if (s_phase == Phase::Joining && s_start_size == 0)
		{
			s_start_size = total;
			s_start.assign(total, 0);
		}

		// Pieces only count in order; a gap makes the host send again from the acknowledgement.
		if (s_phase == Phase::Joining && total == s_start_size && offset == s_start_until && offset < total)
		{
			const u32 piece = std::min<u32>(static_cast<u32>(size - START_HEADER_SIZE), total - offset);
			std::memcpy(s_start.data() + offset, &packet[START_HEADER_SIZE], piece);
			s_start_until += piece;
		}
		SendStartAck();
	}

	static void HandlePacket(const u8* packet, size_t size)
	{
		if (size < COMMON_SIZE || Read32(packet) != MAGIC || packet[4] != VERSION)
			return;

		const PacketType type = static_cast<PacketType>(packet[7]);
		if (packet[5] != s_config.input_delay)
		{
			End(fmt::format("the other player's input delay is {} and ours is {}", packet[5], s_config.input_delay));
			return;
		}

		if (!s_heard_from_peer)
		{
			s_heard_from_peer = true;
			Console.WriteLn("Netplay: connected.");
			Host::AddKeyedOSDMessage("Netplay", "Netplay: connected.", Host::OSD_INFO_DURATION);
		}

		switch (type)
		{
			case PacketType::Input:
				if (s_phase == Phase::Playing)
					HandleInputs(packet, size);
				break;
			case PacketType::Start:
				if (!s_config.host)
					HandleStartPiece(packet, size);
				break;
			case PacketType::StartAck:
				if (s_config.host && size >= 12)
					s_start_until = std::max(s_start_until, Read32(&packet[8]));
				break;
			case PacketType::Hello:
				s_hello = s_config.host;
				break;
		}
	}

	// Handles every packet that has arrived, waiting up to `timeout_ms` for the first one.
	static void Receive(double timeout_ms)
	{
		std::deque<std::vector<u8>> packets;
		{
			std::unique_lock lock(s_inbox_mutex);
			if (s_inbox.empty() && timeout_ms > 0.0)
				s_inbox_changed.wait_for(lock, std::chrono::duration<double, std::milli>(timeout_ms));
			packets.swap(s_inbox);
		}
		for (const std::vector<u8>& packet : packets)
		{
			if (s_ended)
				return;
			HandlePacket(packet.data(), packet.size());
		}
	}

	// While waiting on the other PC: keeps the window responsive, holds the wait while paused, and ends the session
	// when emulation stops or resets or the other PC goes quiet for too long. Returns false once the session ended.
	static bool KeepWaiting(Common::Timer::Value now, Common::Timer::Value& waiting_since, double timeout_ms)
	{
		Host::PumpMessagesOnCPUThread();
		if (!IsActive())
			return false;

		const VMState state = VMManager::GetState();
		if (state == VMState::Paused)
		{
			waiting_since = now;
			return true;
		}
		if (state != VMState::Running)
		{
			End(state == VMState::Resetting ? "the console was reset" : "emulation stopped");
			return false;
		}
		if (Common::Timer::ConvertValueToMilliseconds(now - waiting_since) >= timeout_ms)
		{
			End(s_heard_from_peer ? "the other player stopped responding" : "the other player never connected");
			return false;
		}
		return true;
	}

	// The host, after a guest's hello released both pads and stopped emulation: saves its state and memory card,
	// sends them, and starts the match from the next VSync.
	static void StartAsHost()
	{
		if (EmuConfig.Mcd[0].Type != MemoryCardType::File || !EmuConfig.Mcd[0].Enabled)
		{
			End("netplay needs a memory card file in slot 1");
			return;
		}
		ReopenMemoryCards();

		const std::string state_path = Path::Combine(EmuFolders::Savestates, START_STATE_FILE);
		std::string save_error;
		VMManager::SaveState(state_path.c_str(), false, false, [&save_error](const std::string& error) { save_error = error; });
		const std::optional<std::vector<u8>> state = FileSystem::ReadBinaryFile(state_path.c_str());
		const std::optional<std::vector<u8>> card = ReadOpenFile(EmuConfig.FullpathToMcd(0));
		if (!save_error.empty() || !state.has_value())
		{
			End(save_error.empty() ? "this game's state could not be read" : save_error);
			return;
		}
		if (!card.has_value())
		{
			End("this game's memory card could not be read");
			return;
		}

		s_start.resize(8 + state->size() + card->size());
		Write32(&s_start[0], static_cast<u32>(state->size()));
		Write32(&s_start[4], static_cast<u32>(card->size()));
		std::memcpy(&s_start[8], state->data(), state->size());
		std::memcpy(&s_start[8 + state->size()], card->data(), card->size());
		Console.WriteLn(fmt::format("Netplay: sending this game to the other player ({} KB).", s_start.size() / 1024));

		u32 next = 0;
		u32 last_until = 0;
		Common::Timer::Value progress_at = Common::Timer::GetCurrentValue();
		Common::Timer::Value waiting_since = progress_at;
		while (s_start_until < s_start.size())
		{
			const u32 window_end = static_cast<u32>(std::min<size_t>(s_start.size(), s_start_until + START_WINDOW_PIECES * START_PIECE_SIZE));
			next = std::max(next, s_start_until);
			for (; next < window_end; next += START_PIECE_SIZE)
				SendStartPiece(next);

			Receive(1.0);
			if (!IsActive())
				return;

			const Common::Timer::Value now = Common::Timer::GetCurrentValue();
			if (s_start_until != last_until)
			{
				last_until = s_start_until;
				progress_at = now;
				waiting_since = now;
			}
			else if (Common::Timer::ConvertValueToMilliseconds(now - progress_at) >= START_STALL_MS)
			{
				next = s_start_until;
				progress_at = now;
			}
			if (!KeepWaiting(now, waiting_since, PEER_TIMEOUT_MS))
				return;
		}

		s_start.clear();
		s_start.shrink_to_fit();

		// A game loaded from a state runs slightly differently from one that ran on, since loading drops recompiled
		// code and some caches. So the host loads the state it sent, as the guest does.
		Error error;
		if (!VMManager::LoadState(state_path.c_str(), &error))
		{
			End(fmt::format("this game's state could not be loaded again: {}", error.GetDescription()));
			return;
		}

		s_phase = Phase::Playing;
		Console.WriteLn("Netplay: the other player joined; the match starts now.");
		Host::AddKeyedOSDMessage("Netplay", "Netplay: the other player joined.", Host::OSD_INFO_DURATION);
	}

	// The guest: says hello until the host's start has arrived, then stops emulation to load it.
	static void JoinAsGuest()
	{
		Common::Timer::Value last_hello = 0;
		Common::Timer::Value last_ack = 0;
		Common::Timer::Value waiting_since = Common::Timer::GetCurrentValue();
		u32 last_until = 0;
		while (s_start_size == 0 || s_start_until < s_start_size)
		{
			const Common::Timer::Value now = Common::Timer::GetCurrentValue();
			if (s_start_size == 0 && Common::Timer::ConvertValueToMilliseconds(now - last_hello) >= HELLO_MS)
			{
				SendHello();
				last_hello = now;
			}
			else if (s_start_size != 0 && Common::Timer::ConvertValueToMilliseconds(now - last_ack) >= RESEND_MS)
			{
				SendStartAck();
				last_ack = now;
			}

			Receive(1.0);
			CheckMatchmaking();
			if (!IsActive())
				return;
			if (s_start_until != last_until)
			{
				last_until = s_start_until;
				waiting_since = now;
			}
			if (!KeepWaiting(now, waiting_since, s_heard_from_peer ? PEER_TIMEOUT_MS : FIRST_CONTACT_TIMEOUT_MS))
				return;
		}
		// Let the host see the whole start has arrived, even if this acknowledgement is lost: later pieces get one too.
		SendStartAck();
		s_phase = Phase::Loading;
		Cpu->ExitExecution();
	}

	// The guest, once emulation stopped: plays on a copy of the host's memory card, loads the host's state, and
	// starts the match from the next VSync, the same one as the host.
	static void LoadAsGuest()
	{
		const u32 state_size = s_start_size >= 8 ? Read32(&s_start[0]) : 0;
		const u32 card_size = s_start_size >= 8 ? Read32(&s_start[4]) : 0;
		if (s_start_size < 8 || 8ull + state_size + card_size != s_start_size)
		{
			End("the host's game arrived damaged");
			return;
		}

		const std::string card_path = Path::Combine(EmuFolders::MemoryCards, SESSION_CARD_FILE);
		const std::string state_path = Path::Combine(EmuFolders::Savestates, START_STATE_FILE);
		if (!FileSystem::WriteBinaryFile(card_path.c_str(), &s_start[8 + state_size], card_size) ||
			!FileSystem::WriteBinaryFile(state_path.c_str(), &s_start[8], state_size))
		{
			End("the host's game could not be written to disk");
			return;
		}
		s_start.clear();
		s_start.shrink_to_fit();

		s_session_card = card_path;
		ReopenMemoryCards();

		Error error;
		if (!VMManager::LoadState(state_path.c_str(), &error))
		{
			End(fmt::format("the host's game could not be loaded: {}", error.GetDescription()));
			return;
		}

		s_phase = Phase::Playing;
		Console.WriteLn("Netplay: joined; the match starts from the host's game.");
		Host::AddKeyedOSDMessage("Netplay", "Netplay: joined the other player's game.", Host::OSD_INFO_DURATION);
	}

	// Both PCs, once playing: checks the games every CHECK_INTERVAL VSyncs, sends this VSync's local input for
	// the frame the delay ahead, waits for the other player's input for this frame, and writes both into the pads.
	static void PlayFrame()
	{
		if (s_frame % CHECK_INTERVAL == 0)
		{
			s_newest_check = {s_frame + 1, MemoryChecksum()};
			s_local_checks[(s_frame / CHECK_INTERVAL) % CHECK_RING_SIZE] = s_newest_check;
			Compare(s_frame);
			if (!IsActive())
				return;
		}

		const u32 target = s_frame + s_config.input_delay;
		s_local[target % RING_SIZE] = {target + 1, ToInput(PadData(0, 0))};
		SendInputs();

		Common::Timer::Value last_send = Common::Timer::GetCurrentValue();
		Common::Timer::Value waiting_since = last_send;
		while (s_remote_until <= s_frame)
		{
			Receive(1.0);
			if (!IsActive())
				return;

			const Common::Timer::Value now = Common::Timer::GetCurrentValue();
			if (Common::Timer::ConvertValueToMilliseconds(now - last_send) >= RESEND_MS)
			{
				SendInputs();
				last_send = now;
			}
			if (!KeepWaiting(now, waiting_since, PEER_TIMEOUT_MS))
				return;
		}

		const Input& local = s_local[s_frame % RING_SIZE].input;
		const Input& remote = s_remote[s_frame % RING_SIZE].input;
		if (s_config.host)
			SetBothPads(local, remote);
		else
			SetBothPads(remote, local);
		s_frame++;
	}

	bool IsRoomCode(std::string_view code)
	{
		// The server's codes leave out letters and digits that look alike: 0/O, 1/I.
		static constexpr std::string_view ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
		return code.size() == 6 && std::all_of(code.begin(), code.end(), [](char letter) {
			return ALPHABET.find(letter) != std::string_view::npos;
		});
	}

	void Configure(SessionConfig config)
	{
		CloseConnection();
		s_config = std::move(config);
		s_config.input_delay = std::clamp<u32>(s_config.input_delay, 1, MAX_INPUT_DELAY);
		s_configured = true;
		s_ended = false;
		s_opened = false;
		s_phase = s_config.host ? Phase::WaitingForGuest : Phase::Joining;
		s_connection_failed.store(false);
		s_gathered = false;
		s_room_code = {};
		s_matchmaking_error = {};
		s_room_code_shown = false;
		s_hello = false;
		s_heard_from_peer = false;
		s_start = {};
		s_start_until = 0;
		s_start_size = 0;
		s_session_card = {};
		s_frame = 0;
		s_remote_until = 0;
		s_peer_until = 0;
		s_local = {};
		s_remote = {};
		s_local_checks = {};
		s_remote_checks = {};
		s_newest_check = {};
		s_compared_until = 0;
		s_matching_checks = 0;
	}

	bool IsActive()
	{
		return s_configured && !s_ended;
	}

	void LockSettings(Pcsx2Config& config)
	{
		if (!IsActive())
			return;

		const Pcsx2Config defaults;
		config.Cpu = defaults.Cpu;
		config.Speedhacks = defaults.Speedhacks;
		config.Gamefixes = defaults.Gamefixes;
		config.EnableGameFixes = defaults.EnableGameFixes;
		config.EnablePatches = defaults.EnablePatches;
		config.EnableCheats = false;
		config.EnableWideScreenPatches = false;
		config.EnableNoInterlacingPatches = false;
		config.HostFs = false;
		config.GS.HWDownloadMode = defaults.GS.HWDownloadMode;
		config.DEV9 = defaults.DEV9;
		config.USB = defaults.USB;
		config.Pad.MultitapPort0_Enabled = false;
		config.Pad.MultitapPort1_Enabled = false;
		for (u32 port = 0; port < config.Pad.Ports.size(); port++)
			config.Pad.Ports[port].Type = port < 2 ? Pad::ControllerType::DualShock2 : Pad::ControllerType::NotConnected;
	}

	std::string_view SessionMemoryCard(u32 slot)
	{
		return slot == 0 ? std::string_view(s_session_card) : std::string_view();
	}

	void End(std::string_view reason)
	{
		if (!IsActive())
			return;

		s_ended = true;
		CloseConnection();
		s_start = {};

		// Give slot 1 back the guest's own card, unless emulation is stopping and closes the cards itself.
		if (!s_session_card.empty())
		{
			s_session_card.clear();
			const VMState state = VMManager::GetState();
			if (state == VMState::Running || state == VMState::Paused)
				ReopenMemoryCards();
		}

		const std::string message = fmt::format("Netplay ended: {}.", reason);
		Console.Error(message);
		Host::AddKeyedOSDMessage("Netplay", message, Host::OSD_ERROR_DURATION);
	}

	void OnVSync()
	{
		if (!IsActive())
			return;

		if (!s_opened)
		{
			std::string error;
			if (!Open(&error))
			{
				End(error);
				return;
			}
			s_opened = true;
			const std::string message = s_config.host ?
				fmt::format("Netplay: opening a room, input delay {}.", s_config.input_delay) :
				fmt::format("Netplay: joining room {}, input delay {}.", s_config.room_code, s_config.input_delay);
			Console.WriteLn(message);
			Host::AddKeyedOSDMessage("Netplay", message, Host::OSD_INFO_DURATION);
		}

		if (s_phase == Phase::WaitingForGuest || s_phase == Phase::Joining)
		{
			CheckMatchmaking();
			if (!IsActive())
				return;
		}

		switch (s_phase)
		{
			case Phase::WaitingForGuest:
				// The host keeps playing on its own; a guest's hello starts the match, from released pads.
				Receive(0.0);
				if (IsActive() && s_hello)
				{
					SetBothPads(NEUTRAL_INPUT, NEUTRAL_INPUT);
					s_phase = Phase::Starting;
					Cpu->ExitExecution();
				}
				break;
			case Phase::Joining:
				JoinAsGuest();
				break;
			case Phase::Playing:
				PlayFrame();
				break;
			case Phase::Starting:
			case Phase::Loading:
				break;
		}
	}

	void OnExecutionExit()
	{
		if (!IsActive())
			return;

		if (s_phase == Phase::Starting)
			StartAsHost();
		else if (s_phase == Phase::Loading)
			LoadAsGuest();
	}
} // namespace Netplay
