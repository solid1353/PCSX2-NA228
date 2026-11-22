// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Netplay.h"
#include "BuildVersion.h"
#include "CDVD/CDVD.h"
#include "CDVD/CDVDcommon.h"
#include "CDVD/IsoReader.h"
#include "Config.h"
#include "Host.h"
#include "MemoryTypes.h"
#include "R3000A.h"
#include "R5900.h"
#include "VMManager.h"
#include "Recording/PadData.h"
#include "SIO/Pad/PadTypes.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/HTTPDownloader.h"
#include "common/ScopedGuard.h"
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
#include <charconv>
#include <chrono>
#include <cmath>
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
	static constexpr u8 VERSION = 4;
	static constexpr size_t COMMON_SIZE = 8;

	enum class PacketType : u8
	{
		// The first local frame the sender lacks from us (its acknowledgement), the first frame of the inputs that
		// follow, the sender's newest memory checksum (its frame plus one, 0 for none, then the checksum), then
		// `count` inputs.
		Input = 0,
		// Times the round trip, before booting and once a second during a match: a Ping carries its sender's timer
		// value, which the Pong echoes with how many microseconds the Ping waited before being answered.
		Ping = 1,
		Pong = 2,
		// The host's input delay for the match; the guest acknowledges it with StartAck.
		Start = 3,
		StartAck = 4,
	};

	static constexpr size_t INPUT_HEADER_SIZE = 28;
	static constexpr u32 MAX_INPUTS_PER_PACKET = 64;
	static constexpr size_t MAX_PACKET_SIZE = INPUT_HEADER_SIZE + MAX_INPUTS_PER_PACKET * sizeof(Input);

	// Inputs in flight never span more than twice the delay, well inside the ring.
	static constexpr u32 RING_SIZE = 256;
	static constexpr double RESEND_MS = 5.0;
	static constexpr double FIRST_CONTACT_TIMEOUT_MS = 60000.0;
	static constexpr double PEER_TIMEOUT_MS = 10000.0;
	// The other PC may take a while longer to boot to its first VSync.
	static constexpr double BOOT_TIMEOUT_MS = 60000.0;

	// The matchmaking server forgets a room ten minutes after it opens; the host stops waiting a little before.
	static constexpr double ROOM_WAIT_MS = 9.5 * 60.0 * 1000.0;
	static const std::string EMPTY_POST;
	// The server stores a description once and answers 204, No Content.
	static constexpr s32 STORED = 204;
	static constexpr float MATCHMAKING_TIMEOUT_S = 10.0f;
	static constexpr u32 MATCHMAKING_ATTEMPTS = 3;
	static constexpr const char* STUN_HOST = "stun.cloudflare.com";
	static constexpr u16 STUN_PORT = 3478;

	// Each PC's description starts with a line saying what it plays with, since two PCs stay in sync only with the
	// same emulator build, game and BIOS. Each compares the other's before connecting.
	static constexpr const char* IDENTITY_TAG = "NA228N2";
	// The files of an NA228 ISO that tell its build apart: the manifest of every file the build changed, and the
	// payload.
	static constexpr std::array<const char*, 2> BUILD_FILES = {"228/MANIFEST.TSV", "228/228.BIN"};
	// The game list may hold the disc reader while it scans.
	static constexpr double DISC_LOCK_WAIT_MS = 60000.0;

	// Before booting, the host times this many round trips, one every PING_MS, and sets the delay from the slowest
	// of the fastest nine tenths: half the round trip, in VSyncs, and one more for slack.
	static constexpr u32 PING_SAMPLES = 20;
	static constexpr double PING_MS = 50.0;
	static constexpr double PING_TIMEOUT_MS = 10000.0;
	static constexpr double START_RESEND_MS = 100.0;
	static constexpr double LIVE_PING_MS = 1000.0;
	// How much each VSync's wait for the other player moves the overlay's average, about a second's worth.
	static constexpr float WAIT_AVERAGE_WEIGHT = 1.0f / 60.0f;
	static constexpr double VSYNC_MS = 1000.0 / 59.94;
	static constexpr double CONNECT_TIMEOUT_MS = 30000.0;
	// How long this PC's routes, its own addresses and those the STUN and relay servers give, have to answer.
	static constexpr double GATHER_WAIT_MS = 5000.0;

	// Every 2 seconds at 60 VSyncs a second, both PCs checksum EE and IOP RAM at the start of the same VSync: a hash
	// of each 64 KB block, then a hash of those. Identical games give identical checksums; the first difference ends
	// the match, and each PC logs its block hashes for that check, so the two logs show which blocks differed.
	static constexpr u32 CHECK_INTERVAL = 120;
	static constexpr u32 CHECK_RING_SIZE = 16;
	static constexpr u32 CHECK_BLOCK_SIZE = 0x10000;
	static constexpr u32 EE_BLOCKS = Ps2MemSize::MainRam / CHECK_BLOCK_SIZE;
	static constexpr u32 IOP_BLOCKS = Ps2MemSize::IopRam / CHECK_BLOCK_SIZE;
	static constexpr u32 BLOCKS_PER_LOG_LINE = 32;
	// How many matching checks between log lines, about a minute's worth.
	static constexpr u32 CHECKS_PER_LOG = 30;

	enum class Phase
	{
		// The session thread opens or joins the room, connects and agrees the delay; nothing runs yet.
		Lobby,
		// The game boots and both run every VSync on both players' inputs.
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

	// EE RAM's blocks, then IOP RAM's.
	using BlockHashes = std::array<u64, EE_BLOCKS + IOP_BLOCKS>;

	static SessionConfig s_config;
	static std::atomic<bool> s_configured{false};
	static std::atomic<bool> s_ended{false};
	static Phase s_phase = Phase::Lobby;
	// The input delay both PCs agreed; 0 until then.
	static u32 s_input_delay = 0;
	// The BIOS this PC boots, for the identity.
	static std::string s_bios_path;
	// The connection, libjuice's agent. Its callbacks run on libjuice's thread and hand packets over through the inbox.
	static juice_agent_t* s_agent = nullptr;
	static std::atomic<bool> s_connected{false};
	static std::atomic<bool> s_connection_failed{false};
	static std::mutex s_inbox_mutex;
	static std::condition_variable s_inbox_changed;
	// A packet, and when it arrived, so a Pong can say how long its Ping waited.
	struct Received
	{
		Common::Timer::Value arrived;
		std::vector<u8> data;
	};
	static std::deque<Received> s_inbox;
	static bool s_gathered = false;
	// The session thread runs everything before the boot; once the room is ready it ends, and the CPU thread plays.
	static std::thread s_session;
	static std::atomic<bool> s_stop_session{false};
	// What the room window shows, written by whichever thread runs the session.
	static std::mutex s_room_mutex;
	static RoomState s_room;
	static std::string s_session_error;
	// This PC's identity line, set before matchmaking starts.
	static std::string s_identity;
	// Before booting: the host's round trips, and whether each side has the other's word on the delay.
	static std::vector<double> s_round_trips;
	static bool s_start_received = false;
	static bool s_start_acknowledged = false;
	static bool s_heard_from_peer = false;
	// The VSync about to run, counted from power-on.
	static u32 s_frame = 0;
	// Every remote input before this frame has arrived.
	static u32 s_remote_until = 0;
	// The other PC has every local input before this frame.
	static u32 s_peer_until = 0;
	static std::array<Slot, RING_SIZE> s_local;
	static std::array<Slot, RING_SIZE> s_remote;
	static std::array<Check, CHECK_RING_SIZE> s_local_checks;
	static std::array<Check, CHECK_RING_SIZE> s_remote_checks;
	// The block hashes behind each local check, and the EE and IOP cycles it was taken at.
	static std::array<BlockHashes, CHECK_RING_SIZE> s_local_blocks;
	static std::array<std::array<u64, 2>, CHECK_RING_SIZE> s_check_cycles;
	// What this PC's own player holds, as pad 1 had it before netplay wrote either player's input into the pads.
	// PCSX2's input sources only report changes, so each VSync starts pad 1 from this before they apply.
	static Input s_own_input = {};
	// The newest local check, sent with every input packet; its tag is 0 before the first.
	static Check s_newest_check;
	// Every check before this frame has been compared.
	static u32 s_compared_until = 0;
	static u32 s_matching_checks = 0;
	// The connection overlay's figures, written by the CPU thread and drawn by the GS thread.
	static std::mutex s_stats_mutex;
	static ConnectionStats s_stats;
	static bool s_stats_live = false;
	static std::atomic<bool> s_overlay_shown{true};
	static Common::Timer::Value s_last_live_ping = 0;

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

	static u64 MemoryChecksum(BlockHashes& blocks)
	{
		for (u32 block = 0; block < EE_BLOCKS; block++)
			blocks[block] = XXH3_64bits(eeMem->Main + block * CHECK_BLOCK_SIZE, CHECK_BLOCK_SIZE);
		for (u32 block = 0; block < IOP_BLOCKS; block++)
			blocks[EE_BLOCKS + block] = XXH3_64bits(iopMem->Main + block * CHECK_BLOCK_SIZE, CHECK_BLOCK_SIZE);
		return XXH3_64bits(blocks.data(), sizeof(blocks));
	}

	// Logs this PC's block hashes from one moment, a line per 2 MB, each line headed by the RAM and address it starts at.
	static void LogBlocks(std::string_view moment, const BlockHashes& blocks)
	{
		Console.WriteLnFmt("Netplay: this PC's hash of each 64 KB block of RAM {}:", moment);
		for (u32 first = 0; first < blocks.size(); first += BLOCKS_PER_LOG_LINE)
		{
			const bool ee = first < EE_BLOCKS;
			std::string line = fmt::format("Netplay:   {} {:08x}:", ee ? "EE " : "IOP", (ee ? first : first - EE_BLOCKS) * CHECK_BLOCK_SIZE);
			for (u32 block = first; block < first + BLOCKS_PER_LOG_LINE; block++)
				line += fmt::format(" {:016x}", blocks[block]);
			Console.WriteLn(line);
		}
	}

	static void SetBothPads(const Input& pad1, const Input& pad2)
	{
		PadData(0, 0, pad1).OverrideActualController();
		PadData(1, 0, pad2).OverrideActualController();
	}

	static void SetStatus(std::string status)
	{
		std::lock_guard lock(s_room_mutex);
		s_room.status = std::move(status);
	}

	// Ends the session from the session thread, which can't end it itself: the reason goes to the room, and whoever
	// watches the room ends the session with it.
	static void SetSessionError(std::string error)
	{
		std::lock_guard lock(s_room_mutex);
		if (s_session_error.empty())
			s_session_error = std::move(error);
	}

	static bool Stopping()
	{
		return s_stop_session.load(std::memory_order_acquire);
	}

	static double MillisecondsSince(Common::Timer::Value start)
	{
		return Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - start);
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
			// Where this PC stood at the check, which the two PCs' logs compare.
			const std::array<u64, 2>& cycles = s_check_cycles[(frame / CHECK_INTERVAL) % CHECK_RING_SIZE];
			Console.WriteLnFmt("Netplay: VSync {} began at EE cycle {}, IOP cycle {}.", frame, cycles[0], cycles[1]);
			LogBlocks(fmt::format("at VSync {}", frame), s_local_blocks[(frame / CHECK_INTERVAL) % CHECK_RING_SIZE]);
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
			s_inbox.push_back({Common::Timer::GetCurrentValue(),
				std::vector<u8>(reinterpret_cast<const u8*>(data), reinterpret_cast<const u8*>(data) + size)});
		}
		s_inbox_changed.notify_all();
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
		for (u32 attempt = 0; attempt < MATCHMAKING_ATTEMPTS && !Stopping(); attempt++)
		{
			status = AskMatchmaking(path, &post, body);
			if (status >= 0)
				return (status == 409 && attempt > 0) ? STORED : status;
		}
		return status;
	}

	// Asks for the other side's description every half second until it arrives. Returns false once the session
	// stops, the room is gone, or `timeout_ms` passes.
	static bool AwaitDescription(const std::string& path, double timeout_ms, std::string* description)
	{
		const Common::Timer::Value start = Common::Timer::GetCurrentValue();
		while (!Stopping())
		{
			const s32 status = AskMatchmaking(path, nullptr, description);
			if (status == HTTPDownloader::HTTP_STATUS_OK && !description->empty())
				return true;
			if (status == 404)
			{
				SetSessionError("there is no room with that code");
				return false;
			}
			if (MillisecondsSince(start) >= timeout_ms)
			{
				SetSessionError("the other player never connected");
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
			SetSessionError("the other player's connection details are damaged");
			return false;
		}
		juice_set_remote_gathering_done(s_agent);
		return true;
	}

	struct Relay
	{
		std::string host;
		u16 port = 0;
		std::string username;
		std::string password;
	};

	// Logins for Cloudflare's TURN relays, which the room gives its host and its guest once each, a line each: host,
	// port, username, password. Without any, a match can only connect directly.
	static std::vector<Relay> FetchRelays(const std::string& code)
	{
		std::vector<Relay> relays;
		std::string list;
		const s32 status = PostToMatchmaking(fmt::format("/rooms/{}/relay/{}", code, s_config.host ? "host" : "guest"),
			EMPTY_POST, &list);
		for (size_t start = 0; status == HTTPDownloader::HTTP_STATUS_OK && start < list.size();)
		{
			const size_t end = std::min(list.find('\n', start), list.size());
			std::array<std::string_view, 4> fields;
			std::string_view line = std::string_view(list).substr(start, end - start);
			size_t count = 0;
			for (; count < fields.size() && !line.empty(); count++)
			{
				const size_t space = line.find(' ');
				fields[count] = line.substr(0, space);
				line = space == std::string_view::npos ? std::string_view() : line.substr(space + 1);
			}
			u16 port = 0;
			if (count == fields.size() && line.empty() &&
				std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), port).ec == std::errc())
			{
				relays.push_back({std::string(fields[0]), port, std::string(fields[2]), std::string(fields[3])});
			}
			start = end + 1;
		}
		if (relays.empty())
			Console.Warning("Netplay: no relay is available, so the two PCs can only connect directly.");
		else
			Console.WriteLnFmt("Netplay: {} relays are available if the two PCs can't connect directly.", relays.size());
		return relays;
	}

	// The game a disc image holds: its serial and boot ELF's CRC and, on an NA228 ISO, its build files. Read the way
	// the game list scans images, since nothing runs yet. Returns false when the image can't be read.
	static bool GameIdentity(const std::string& path, u64* identity)
	{
		Error error;
		const Common::Timer::Value start = Common::Timer::GetCurrentValue();
		while (!cdvdLock(&error))
		{
			if (Stopping() || MillisecondsSince(start) >= DISC_LOCK_WAIT_MS)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		const ScopedGuard unlock(&cdvdUnlock);

		CDVD = &CDVDapi_Iso;
		if (!CDVD->open(path, &error))
		{
			Console.ErrorFmt("Netplay: {} could not be read: {}", path, error.GetDescription());
			return false;
		}
		DoCDVDdetectDiskType();
		std::string serial;
		u32 crc = 0;
		cdvdGetDiscInfo(&serial, nullptr, nullptr, &crc, nullptr);
		*identity = XXH3_64bits_withSeed(serial.data(), serial.size(), crc);

		IsoReader iso;
		std::vector<u8> data;
		if (iso.Open())
		{
			for (const char* file : BUILD_FILES)
			{
				if (iso.ReadFile(file, &data))
					*identity = XXH3_64bits_withSeed(data.data(), data.size(), *identity);
			}
		}
		DoCDVDclose();
		return true;
	}

	// What this PC plays with: the emulator build, the game, and the BIOS file. Empty when the game or BIOS can't
	// be read.
	static std::string Identity()
	{
		u64 game = 0;
		if (!GameIdentity(s_config.game_path, &game))
		{
			SetSessionError("this PC's game could not be read");
			return {};
		}
		const std::optional<std::vector<u8>> bios = FileSystem::ReadBinaryFile(s_bios_path.c_str());
		if (!bios.has_value())
		{
			SetSessionError("this PC's BIOS could not be read");
			return {};
		}
		const std::string_view build = *BuildVersion::GitHash ? BuildVersion::GitHash : "-";
		return fmt::format("{} {} {:016x} {:016x}", IDENTITY_TAG, build, game, XXH3_64bits(bios->data(), bios->size()));
	}

	static std::vector<std::string_view> SplitWords(std::string_view text)
	{
		std::vector<std::string_view> words;
		while (!text.empty())
		{
			const size_t space = text.find(' ');
			words.push_back(text.substr(0, space));
			text = space == std::string_view::npos ? std::string_view() : text.substr(space + 1);
		}
		return words;
	}

	// Why the other PC can't play with this one, or empty when it can.
	static std::string IdentityDifference(std::string_view theirs)
	{
		const std::vector<std::string_view> ours = SplitWords(s_identity), other = SplitWords(theirs);
		if (other.size() != ours.size() || other[0] != ours[0] || other[1] != ours[1])
			return "the other player has a different PCSX2-NA228 build";
		if (other[2] != ours[2])
			return "the other player has a different game or NA228 ISO";
		if (other[3] != ours[3])
			return "the other player has a different BIOS";
		return {};
	}

	// A description posted to the server is the identity line, then libjuice's description.
	static std::string_view IdentityLine(const std::string& posted)
	{
		return std::string_view(posted).substr(0, posted.find('\n'));
	}

	static std::string ConnectionDetails(const std::string& posted)
	{
		const size_t newline = posted.find('\n');
		return newline == std::string::npos ? std::string() : posted.substr(newline + 1);
	}

	// Creates the agent and gathers this PC's candidates, from its own addresses, Cloudflare's STUN and its relays,
	// then returns the description that carries them. The guest gives the host's description first: an agent that
	// gathers before it has the other's takes the controlling role, and one of the two must be controlled.
	static bool GatherDescription(const std::string& code, const std::string* remote, std::string* description)
	{
		juice_set_log_handler(OnJuiceLog);
		juice_set_log_level(JUICE_LOG_LEVEL_INFO);
		const std::vector<Relay> relays = FetchRelays(code);
		std::vector<juice_turn_server_t> servers;
		for (const Relay& relay : relays)
			servers.push_back({relay.host.c_str(), relay.username.c_str(), relay.password.c_str(), relay.port});
		juice_config_t config = {};
		config.turn_servers = servers.data();
		config.turn_servers_count = static_cast<int>(servers.size());
		config.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
		config.stun_server_host = STUN_HOST;
		config.stun_server_port = STUN_PORT;
		config.cb_state_changed = OnJuiceStateChanged;
		config.cb_gathering_done = OnJuiceGatheringDone;
		config.cb_recv = OnJuiceReceived;
		s_agent = juice_create(&config);
		if (!s_agent)
		{
			SetSessionError("the connection could not be set up");
			return false;
		}
		if (remote && !UseRemoteDescription(*remote))
			return false;
		if (juice_gather_candidates(s_agent) != JUICE_ERR_SUCCESS)
		{
			SetSessionError("the connection could not be set up");
			return false;
		}

		// A relay or STUN server some networks block never answers, so the description goes out with the routes
		// that answered in time.
		std::unique_lock lock(s_inbox_mutex);
		s_inbox_changed.wait_for(lock, std::chrono::duration<double, std::milli>(GATHER_WAIT_MS),
			[] { return s_gathered || Stopping(); });
		const bool gathered = s_gathered;
		lock.unlock();
		if (Stopping())
			return false;
		if (!gathered)
			Console.Warning("Netplay: some of this PC's routes didn't answer in time; going ahead with the others.");

		std::array<char, JUICE_MAX_SDP_STRING_LEN> buffer = {};
		if (juice_get_local_description(s_agent, buffer.data(), buffer.size()) != JUICE_ERR_SUCCESS)
		{
			SetSessionError("this PC's connection details could not be read");
			return false;
		}
		*description = buffer.data();
		return true;
	}

	// The host: opens a room, posts its description, and waits for a guest's.
	static bool MatchAsHost()
	{
		std::string reply;
		if (PostToMatchmaking("/rooms", EMPTY_POST, &reply) != HTTPDownloader::HTTP_STATUS_OK)
		{
			SetSessionError("the matchmaking server could not be reached");
			return false;
		}
		const size_t start = reply.find("\"code\":\"");
		const size_t end = start == std::string::npos ? std::string::npos : reply.find('"', start + 8);
		if (end == std::string::npos)
		{
			SetSessionError("the matchmaking server gave no room code");
			return false;
		}
		const std::string code = reply.substr(start + 8, end - start - 8);
		{
			std::lock_guard lock(s_room_mutex);
			s_room.code = code;
			s_room.status = "Waiting for the other player...";
		}
		Console.WriteLnFmt("Netplay: room code {}. Waiting for the other player.", code);

		std::string description, guest;
		if (!GatherDescription(code, nullptr, &description))
			return false;
		if (PostToMatchmaking(fmt::format("/rooms/{}/host", code), s_identity + "\n" + description, nullptr) != STORED)
		{
			SetSessionError("the matchmaking server refused this game's connection details");
			return false;
		}
		// The room lives ten minutes; a guest has until shortly before then to join.
		if (!AwaitDescription(fmt::format("/rooms/{}/guest", code), ROOM_WAIT_MS, &guest))
			return false;
		if (const std::string difference = IdentityDifference(IdentityLine(guest)); !difference.empty())
		{
			SetSessionError(difference);
			return false;
		}
		return UseRemoteDescription(ConnectionDetails(guest));
	}

	// The guest: waits for the host's description in the room, starts connecting with it, and posts its own. A
	// guest that can't play with the host posts only its identity, so the host can tell why too.
	static bool MatchAsGuest()
	{
		std::string host, description;
		if (!AwaitDescription(fmt::format("/rooms/{}/host", s_config.room_code), FIRST_CONTACT_TIMEOUT_MS, &host))
			return false;
		const std::string difference = IdentityDifference(IdentityLine(host));
		const std::string connection = ConnectionDetails(host);
		if (difference.empty() && !GatherDescription(s_config.room_code, &connection, &description))
			return false;
		const s32 status = PostToMatchmaking(fmt::format("/rooms/{}/guest", s_config.room_code), s_identity + "\n" + description, nullptr);
		if (!difference.empty())
		{
			SetSessionError(difference);
			return false;
		}
		if (status != STORED)
		{
			SetSessionError(status == 409 ? "someone else already joined that room" :
											"the matchmaking server refused this game's connection details");
			return false;
		}
		return true;
	}

	static void CloseConnection()
	{
		s_stop_session.store(true, std::memory_order_release);
		s_inbox_changed.notify_all();
		if (s_session.joinable())
			s_session.join();
		if (s_agent)
		{
			juice_destroy(s_agent);
			s_agent = nullptr;
		}
		s_connected.store(false, std::memory_order_release);
		std::lock_guard lock(s_inbox_mutex);
		s_inbox.clear();
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
		packet[5] = static_cast<u8>(s_input_delay);
		packet[6] = static_cast<u8>(count);
		packet[7] = static_cast<u8>(type);
	}

	static void SendInputs()
	{
		const u32 last = s_frame + s_input_delay;
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

	// A packet of the lobby: its type and one 64-bit value, a timer value or the input delay.
	static void SendLobbyPacket(PacketType type, u64 value)
	{
		std::array<u8, COMMON_SIZE + 8> packet = {};
		WriteCommon(packet.data(), type);
		Write64(&packet[COMMON_SIZE], value);
		SendPacket(packet.data(), packet.size());
	}

	// Answers a Ping with its timer value and how long it waited here, which the round trip leaves out.
	static void SendPong(u64 sent, Common::Timer::Value arrived)
	{
		std::array<u8, COMMON_SIZE + 12> packet = {};
		WriteCommon(packet.data(), PacketType::Pong);
		Write64(&packet[COMMON_SIZE], sent);
		const double waited_ms = Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - arrived);
		Write32(&packet[COMMON_SIZE + 8], static_cast<u32>(std::max(0.0, waited_ms) * 1000.0));
		SendPacket(packet.data(), packet.size());
	}

	// Whether the connection the two PCs use goes through a relay.
	static bool UsesRelay()
	{
		std::array<char, JUICE_MAX_CANDIDATE_SDP_STRING_LEN> local = {}, remote = {};
		if (!s_agent || juice_get_selected_candidates(s_agent, local.data(), local.size(), remote.data(), remote.size()) != JUICE_ERR_SUCCESS)
			return false;
		return std::strstr(local.data(), "typ relay") || std::strstr(remote.data(), "typ relay");
	}

	static void HandleInputs(const u8* packet, size_t size)
	{
		const u32 count = packet[6];
		if (size < INPUT_HEADER_SIZE || count > MAX_INPUTS_PER_PACKET || size < INPUT_HEADER_SIZE + count * sizeof(Input))
			return;

		const u32 ack = Read32(&packet[8]);
		if (ack > s_peer_until && ack <= s_frame + s_input_delay + 1)
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

	static void HandlePacket(const u8* packet, size_t size, Common::Timer::Value arrived)
	{
		if (size < COMMON_SIZE || Read32(packet) != MAGIC || packet[4] != VERSION)
			return;

		if (!s_heard_from_peer)
		{
			s_heard_from_peer = true;
			Console.WriteLn("Netplay: connected.");
		}

		const PacketType type = static_cast<PacketType>(packet[7]);
		const u64 value = size >= COMMON_SIZE + 8 ? Read64(&packet[COMMON_SIZE]) : 0;
		switch (type)
		{
			case PacketType::Input:
				if (s_phase != Phase::Playing)
					break;
				if (packet[5] != s_input_delay)
				{
					End(fmt::format("the other player's input delay is {} and ours is {}", packet[5], s_input_delay));
					return;
				}
				HandleInputs(packet, size);
				break;
			case PacketType::Ping:
				SendPong(value, arrived);
				break;
			case PacketType::Pong:
				if (value != 0 && size >= COMMON_SIZE + 12)
				{
					const double round_trip = std::max(0.0,
						Common::Timer::ConvertValueToMilliseconds(arrived - value) - Read32(&packet[COMMON_SIZE + 8]) / 1000.0);
					if (s_phase == Phase::Lobby)
					{
						if (s_config.host)
							s_round_trips.push_back(round_trip);
					}
					else
					{
						const bool relayed = UsesRelay();
						std::lock_guard lock(s_stats_mutex);
						s_stats.ping_ms = static_cast<float>(round_trip);
						s_stats.relayed = relayed;
					}
				}
				break;
			case PacketType::Start:
				if (!s_config.host && value >= 1 && value <= MAX_INPUT_DELAY)
				{
					if (s_phase == Phase::Lobby)
					{
						s_input_delay = static_cast<u32>(value);
						s_start_received = true;
					}
					SendLobbyPacket(PacketType::StartAck, value);
				}
				break;
			case PacketType::StartAck:
				if (s_config.host && value == s_input_delay)
					s_start_acknowledged = true;
				break;
		}
	}

	// Handles every packet that has arrived, waiting up to `timeout_ms` for the first one.
	static void Receive(double timeout_ms)
	{
		std::deque<Received> packets;
		{
			std::unique_lock lock(s_inbox_mutex);
			if (s_inbox.empty() && timeout_ms > 0.0)
				s_inbox_changed.wait_for(lock, std::chrono::duration<double, std::milli>(timeout_ms));
			packets.swap(s_inbox);
		}
		for (const Received& packet : packets)
		{
			if (s_ended.load(std::memory_order_acquire))
				return;
			HandlePacket(packet.data.data(), packet.data.size(), packet.arrived);
		}
	}

	// The session thread, once matchmaking passed both descriptions: waits for the two PCs to reach each other.
	static bool AwaitConnection()
	{
		SetStatus("Connecting to the other player...");
		const Common::Timer::Value start = Common::Timer::GetCurrentValue();
		std::unique_lock lock(s_inbox_mutex);
		while (!s_connected.load(std::memory_order_acquire))
		{
			if (Stopping())
				return false;
			if (s_connection_failed.load(std::memory_order_acquire) || MillisecondsSince(start) >= CONNECT_TIMEOUT_MS)
			{
				lock.unlock();
				SetSessionError("the two PCs could not connect through their routers");
				return false;
			}
			s_inbox_changed.wait_for(lock, std::chrono::milliseconds(50));
		}
		return true;
	}

	// The session thread, host: times round trips to the guest and sets the delay from them, then tells the guest
	// until it acknowledges.
	static bool AgreeAsHost()
	{
		SetStatus("Measuring the ping...");
		Common::Timer::Value start = Common::Timer::GetCurrentValue();
		Common::Timer::Value last_ping = 0;
		while (s_round_trips.size() < PING_SAMPLES)
		{
			if (Stopping())
				return false;
			if (MillisecondsSince(start) >= PING_TIMEOUT_MS)
			{
				SetSessionError("the other player stopped responding");
				return false;
			}
			if (last_ping == 0 || MillisecondsSince(last_ping) >= PING_MS)
			{
				last_ping = Common::Timer::GetCurrentValue();
				SendLobbyPacket(PacketType::Ping, last_ping);
			}
			Receive(5.0);
		}

		std::vector<double> round_trips = s_round_trips;
		std::sort(round_trips.begin(), round_trips.end());
		const double round_trip = round_trips[round_trips.size() * 9 / 10 - 1];
		s_input_delay = std::clamp<u32>(static_cast<u32>(std::ceil(round_trip / 2.0 / VSYNC_MS)) + 1, 1, MAX_INPUT_DELAY);
		{
			std::lock_guard lock(s_room_mutex);
			s_room.ping_ms = static_cast<u32>(std::lround(round_trip));
			s_room.input_delay = s_input_delay;
			s_room.status = "Starting the game...";
		}

		start = Common::Timer::GetCurrentValue();
		Common::Timer::Value last_start = 0;
		while (!s_start_acknowledged)
		{
			if (Stopping())
				return false;
			if (MillisecondsSince(start) >= PEER_TIMEOUT_MS)
			{
				SetSessionError("the other player stopped responding");
				return false;
			}
			if (last_start == 0 || MillisecondsSince(last_start) >= START_RESEND_MS)
			{
				last_start = Common::Timer::GetCurrentValue();
				SendLobbyPacket(PacketType::Start, s_input_delay);
			}
			Receive(5.0);
		}
		Console.WriteLnFmt("Netplay: ping {} ms, input delay {}; starting the game.", std::lround(round_trip), s_input_delay);
		return true;
	}

	// The session thread, guest: answers the host's pings until it sends the delay.
	static bool AgreeAsGuest()
	{
		SetStatus("Measuring the ping...");
		const Common::Timer::Value start = Common::Timer::GetCurrentValue();
		while (!s_start_received)
		{
			if (Stopping())
				return false;
			if (MillisecondsSince(start) >= PING_TIMEOUT_MS + PEER_TIMEOUT_MS)
			{
				SetSessionError("the other player stopped responding");
				return false;
			}
			Receive(5.0);
		}
		{
			std::lock_guard lock(s_room_mutex);
			s_room.input_delay = s_input_delay;
			s_room.status = "Starting the game...";
		}
		Console.WriteLnFmt("Netplay: input delay {}; starting the game.", s_input_delay);
		return true;
	}

	// Everything before the boot, on its own thread: the identity, matchmaking, the connection and the delay. Once
	// both PCs agree, the room says the game can boot, and the CPU thread takes the session over from its first VSync.
	static void RunSession()
	{
		s_identity = Identity();
		if (s_identity.empty())
			return;
		Console.WriteLnFmt("Netplay: this PC plays with {}.", s_identity);

		if (!(s_config.host ? MatchAsHost() : MatchAsGuest()) || !AwaitConnection() ||
			!(s_config.host ? AgreeAsHost() : AgreeAsGuest()))
		{
			return;
		}

		// Frames before the delay have no input from either player: both run them with released pads.
		for (u32 frame = 0; frame < s_input_delay; frame++)
		{
			s_local[frame] = {frame + 1, NEUTRAL_INPUT};
			s_remote[frame] = {frame + 1, NEUTRAL_INPUT};
		}
		s_remote_until = s_input_delay;
		s_peer_until = s_input_delay;
		s_own_input = NEUTRAL_INPUT;
		s_phase = Phase::Playing;

		std::lock_guard lock(s_room_mutex);
		s_room.ready = true;
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
		if (s_connection_failed.load(std::memory_order_acquire))
		{
			End("the connection to the other player broke");
			return false;
		}
		if (Common::Timer::ConvertValueToMilliseconds(now - waiting_since) >= timeout_ms)
		{
			End("the other player stopped responding");
			return false;
		}
		return true;
	}

	// Both PCs, from the first VSync after power-on: checks the games every CHECK_INTERVAL VSyncs, sends this VSync's
	// local input for the frame the delay ahead, waits for the other player's input for this frame, and writes both
	// into the pads.
	static void PlayFrame()
	{
		if (s_frame == 0)
		{
			Console.WriteLn("Netplay: the match starts.");
			Host::AddKeyedOSDMessage("Netplay", "Netplay: the match starts.", Host::OSD_INFO_DURATION);
			const RoomState room = GetRoomState();
			std::lock_guard lock(s_stats_mutex);
			s_stats = {static_cast<float>(room.ping_ms), s_input_delay, UsesRelay(), 0.0f};
			s_stats_live = true;
		}
		if (s_last_live_ping == 0 || Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - s_last_live_ping) >= LIVE_PING_MS)
		{
			s_last_live_ping = Common::Timer::GetCurrentValue();
			SendLobbyPacket(PacketType::Ping, s_last_live_ping);
		}

		if (s_frame % CHECK_INTERVAL == 0)
		{
			s_check_cycles[(s_frame / CHECK_INTERVAL) % CHECK_RING_SIZE] = {cpuRegs.cycle, psxRegs.cycle};
			s_newest_check = {s_frame + 1, MemoryChecksum(s_local_blocks[(s_frame / CHECK_INTERVAL) % CHECK_RING_SIZE])};
			s_local_checks[(s_frame / CHECK_INTERVAL) % CHECK_RING_SIZE] = s_newest_check;
			Compare(s_frame);
			if (!IsActive())
				return;
		}

		const u32 target = s_frame + s_input_delay;
		s_own_input = ToInput(PadData(0, 0));
		s_local[target % RING_SIZE] = {target + 1, s_own_input};
		SendInputs();

		// Until the other PC's first input arrives, it may still be booting.
		const double timeout_ms = s_remote_until > s_input_delay ? PEER_TIMEOUT_MS : BOOT_TIMEOUT_MS;
		Common::Timer::Value last_send = Common::Timer::GetCurrentValue();
		Common::Timer::Value waiting_since = last_send;
		const Common::Timer::Value wait_start = last_send;
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
			if (!KeepWaiting(now, waiting_since, timeout_ms))
				return;
			// Waiting handles the window's input too, which lands in pad 1 before the players' inputs go in, so the
			// next VSync starts from it.
			s_own_input = ToInput(PadData(0, 0));
		}

		{
			const float waited_ms = static_cast<float>(
				Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - wait_start));
			std::lock_guard lock(s_stats_mutex);
			s_stats.wait_ms += (waited_ms - s_stats.wait_ms) * WAIT_AVERAGE_WEIGHT;
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
		s_bios_path = EmuConfig.FullpathToBios();
		s_phase = Phase::Lobby;
		s_input_delay = 0;
		s_connection_failed.store(false);
		s_gathered = false;
		{
			std::lock_guard lock(s_room_mutex);
			s_room = {};
			s_room.status = s_config.host ? "Opening a room..." : fmt::format("Joining room {}...", s_config.room_code);
			s_session_error = {};
		}
		s_identity = {};
		s_round_trips = {};
		s_start_received = false;
		s_start_acknowledged = false;
		s_heard_from_peer = false;
		s_frame = 0;
		s_remote_until = 0;
		s_peer_until = 0;
		s_local = {};
		s_remote = {};
		s_local_checks = {};
		s_remote_checks = {};
		s_local_blocks = {};
		s_check_cycles = {};
		s_newest_check = {};
		s_own_input = NEUTRAL_INPUT;
		s_compared_until = 0;
		s_matching_checks = 0;
		s_last_live_ping = 0;
		{
			std::lock_guard lock(s_stats_mutex);
			s_stats = {};
			s_stats_live = false;
		}
		s_ended.store(false, std::memory_order_release);
		s_configured.store(true, std::memory_order_release);

		Console.WriteLn(s_config.host ? std::string("Netplay: opening a room.") :
										fmt::format("Netplay: joining room {}.", s_config.room_code));
		s_stop_session.store(false, std::memory_order_release);
		s_session = std::thread(RunSession);
	}

	RoomState GetRoomState()
	{
		std::lock_guard lock(s_room_mutex);
		RoomState room = s_room;
		if (room.end_reason.empty())
			room.end_reason = s_session_error;
		return room;
	}

	std::optional<ConnectionStats> GetConnectionStats()
	{
		std::lock_guard lock(s_stats_mutex);
		return s_stats_live ? std::optional<ConnectionStats>(s_stats) : std::nullopt;
	}

	bool IsOverlayShown()
	{
		return s_overlay_shown.load(std::memory_order_acquire);
	}

	void SetOverlayShown(bool shown)
	{
		s_overlay_shown.store(shown, std::memory_order_release);
	}

	bool IsActive()
	{
		return s_configured.load(std::memory_order_acquire) && !s_ended.load(std::memory_order_acquire);
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
		config.EnableFastBoot = true;
		config.HostFs = false;
		config.GS.HWDownloadMode = defaults.GS.HWDownloadMode;
		config.DEV9 = defaults.DEV9;
		config.USB = defaults.USB;
		config.Pad.MultitapPort0_Enabled = false;
		config.Pad.MultitapPort1_Enabled = false;
		for (u32 port = 0; port < config.Pad.Ports.size(); port++)
			config.Pad.Ports[port].Type = port < 2 ? Pad::ControllerType::DualShock2 : Pad::ControllerType::NotConnected;
		for (Pcsx2Config::McdOptions& card : config.Mcd)
			card.Enabled = false;
	}

	void End(std::string_view reason)
	{
		if (!IsActive())
			return;

		s_ended.store(true, std::memory_order_release);
		{
			std::lock_guard lock(s_room_mutex);
			s_room.end_reason = reason;
		}
		{
			std::lock_guard lock(s_stats_mutex);
			s_stats_live = false;
		}
		CloseConnection();

		const std::string message = fmt::format("Netplay ended: {}.", reason);
		Console.Error(message);
		Host::AddKeyedOSDMessage("Netplay", message, Host::OSD_ERROR_DURATION);
	}

	void BeforeInputPoll()
	{
		if (IsActive() && s_phase == Phase::Playing)
			PadData(0, 0, s_own_input).OverrideActualController();
	}

	void OnVSync()
	{
		if (IsActive() && s_phase == Phase::Playing)
			PlayFrame();
	}
} // namespace Netplay
