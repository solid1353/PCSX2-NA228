// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Netplay.h"
#include "Host.h"
#include "MemoryTypes.h"
#include "VMManager.h"
#include "Recording/PadData.h"

#include "common/Console.h"
#include "common/Timer.h"

#include "fmt/format.h"

// PCSX2 builds xxHash into each user, as GS/GSXXH.h does.
#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Netplay
{
#ifdef _WIN32
	using Socket = SOCKET;
	static constexpr Socket NO_SOCKET = INVALID_SOCKET;
#else
	using Socket = int;
	static constexpr Socket NO_SOCKET = -1;
#endif

	// One pad's state, in the 18-byte layout input recordings use (InputRecordingFile::writePadData).
	using Input = std::array<u8, 18>;

	// Packet: magic, version, input delay, input count, zero, the first local frame the sender lacks from us
	// (its acknowledgement), the first frame of the inputs that follow, the sender's newest memory checksum
	// (its frame plus one, 0 for none, then the checksum), then `count` inputs.
	static constexpr u32 MAGIC = 0x4e32414e; // "NA2N"
	static constexpr u8 VERSION = 2;
	static constexpr size_t HEADER_SIZE = 28;
	static constexpr u32 MAX_INPUTS_PER_PACKET = 64;
	static constexpr size_t MAX_PACKET_SIZE = HEADER_SIZE + MAX_INPUTS_PER_PACKET * sizeof(Input);

	// Inputs in flight never span more than twice the delay, well inside the ring.
	static constexpr u32 RING_SIZE = 256;
	static constexpr double RESEND_MS = 5.0;
	static constexpr double FIRST_CONTACT_TIMEOUT_MS = 60000.0;
	static constexpr double PEER_TIMEOUT_MS = 10000.0;

	// Every 2 seconds at 60 VSyncs a second, both PCs checksum EE and IOP RAM at the start of the same VSync.
	// Identical games give identical checksums; the first difference ends the match.
	static constexpr u32 CHECK_INTERVAL = 120;
	static constexpr u32 CHECK_RING_SIZE = 16;
	// How many matching checks between log lines, about a minute's worth.
	static constexpr u32 CHECKS_PER_LOG = 30;

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
	static Socket s_socket = NO_SOCKET;
	static sockaddr_in s_peer = {};
	static bool s_peer_known = false;
	static bool s_heard_from_peer = false;
	// The VSync about to run.
	static u32 s_frame = 0;
	// Every remote input before this frame has arrived.
	static u32 s_remote_until = 0;
	// The other PC has every local input before this frame.
	static u32 s_peer_until = 0;
	static std::array<Slot, RING_SIZE> s_local;
	static std::array<Slot, RING_SIZE> s_remote;
	static std::array<Check, CHECK_RING_SIZE> s_local_checks;
	static std::array<Check, CHECK_RING_SIZE> s_remote_checks;
	// The newest local check, sent with every packet; its tag is 0 before the first.
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

	static void CloseSocket()
	{
		if (s_socket == NO_SOCKET)
			return;
#ifdef _WIN32
		closesocket(s_socket);
#else
		close(s_socket);
#endif
		s_socket = NO_SOCKET;
	}

	static bool InitializeSockets()
	{
#ifdef _WIN32
		static bool initialized = false;
		if (!initialized)
		{
			WSADATA wsa = {};
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
				return false;
			initialized = true;
			std::atexit([]() { WSACleanup(); });
		}
#endif
		return true;
	}

	static bool Open(std::string* error)
	{
		if (!InitializeSockets())
		{
			*error = "the network could not be initialized";
			return false;
		}

		s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s_socket == NO_SOCKET)
		{
			*error = "no UDP socket could be opened";
			return false;
		}

#ifdef _WIN32
		u_long non_blocking = 1;
		ioctlsocket(s_socket, FIONBIO, &non_blocking);
		// Otherwise an ICMP "port unreachable", from sending before the host listens, fails the next receive.
		BOOL report_reset = FALSE;
		DWORD returned = 0;
		WSAIoctl(s_socket, SIO_UDP_CONNRESET, &report_reset, sizeof(report_reset), nullptr, 0, &returned, nullptr, nullptr);
#else
		fcntl(s_socket, F_SETFL, fcntl(s_socket, F_GETFL, 0) | O_NONBLOCK);
#endif

		if (s_config.host)
		{
			sockaddr_in address = {};
			address.sin_family = AF_INET;
			address.sin_port = htons(s_config.port);
			address.sin_addr.s_addr = htonl(INADDR_ANY);
			if (bind(s_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
			{
				*error = fmt::format("UDP port {} could not be opened", s_config.port);
				CloseSocket();
				return false;
			}
		}
		else
		{
			addrinfo hints = {};
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_DGRAM;
			addrinfo* result = nullptr;
			const std::string port = std::to_string(s_config.port);
			if (getaddrinfo(s_config.peer_host.c_str(), port.c_str(), &hints, &result) != 0 || !result)
			{
				*error = fmt::format("the host address {} could not be found", s_config.peer_host);
				CloseSocket();
				return false;
			}
			std::memcpy(&s_peer, result->ai_addr, sizeof(s_peer));
			freeaddrinfo(result);
			s_peer_known = true;
		}

		// Frames before the delay have no input from either player: both run them with released pads.
		for (u32 frame = 0; frame < s_config.input_delay; frame++)
		{
			s_local[frame] = {frame + 1, NEUTRAL_INPUT};
			s_remote[frame] = {frame + 1, NEUTRAL_INPUT};
		}
		s_remote_until = s_config.input_delay;
		s_peer_until = s_config.input_delay;
		return true;
	}

	static void Send()
	{
		if (!s_peer_known)
			return;

		const u32 last = s_frame + s_config.input_delay;
		const u32 first = std::max(s_peer_until, last + 1 > MAX_INPUTS_PER_PACKET ? last + 1 - MAX_INPUTS_PER_PACKET : 0);
		const u32 count = first <= last ? last - first + 1 : 0;

		std::array<u8, MAX_PACKET_SIZE> packet = {};
		Write32(&packet[0], MAGIC);
		packet[4] = VERSION;
		packet[5] = static_cast<u8>(s_config.input_delay);
		packet[6] = static_cast<u8>(count);
		Write32(&packet[8], s_remote_until);
		Write32(&packet[12], first);
		Write32(&packet[16], s_newest_check.tag);
		Write64(&packet[20], s_newest_check.checksum);
		for (u32 i = 0; i < count; i++)
			std::memcpy(&packet[HEADER_SIZE + i * sizeof(Input)], s_local[(first + i) % RING_SIZE].input.data(), sizeof(Input));

		sendto(s_socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(HEADER_SIZE + count * sizeof(Input)), 0,
			reinterpret_cast<const sockaddr*>(&s_peer), sizeof(s_peer));
	}

	static void HandlePacket(const u8* packet, size_t size, const sockaddr_in& from)
	{
		if (size < HEADER_SIZE || Read32(packet) != MAGIC || packet[4] != VERSION)
			return;

		if (s_peer_known)
		{
			if (from.sin_addr.s_addr != s_peer.sin_addr.s_addr || from.sin_port != s_peer.sin_port)
				return;
		}
		else
		{
			// The host learns the guest's address from its first packet.
			s_peer = from;
			s_peer_known = true;
		}

		if (packet[5] != s_config.input_delay)
		{
			End(fmt::format("the other player's input delay is {} and ours is {}", packet[5], s_config.input_delay));
			return;
		}

		const u32 count = packet[6];
		if (count > MAX_INPUTS_PER_PACKET || size < HEADER_SIZE + count * sizeof(Input))
			return;

		if (!s_heard_from_peer)
		{
			s_heard_from_peer = true;
			Console.WriteLn("Netplay: connected.");
			Host::AddKeyedOSDMessage("Netplay", "Netplay: connected.", Host::OSD_INFO_DURATION);
		}

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
			std::memcpy(slot.input.data(), &packet[HEADER_SIZE + i * sizeof(Input)], sizeof(Input));
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

	static void Receive(double timeout_ms)
	{
		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(s_socket, &readable);
		timeval timeout = {};
		timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(timeout_ms * 1000.0);
		select(static_cast<int>(s_socket) + 1, &readable, nullptr, nullptr, &timeout);

		for (int i = 0; i < 256 && !s_ended; i++)
		{
			std::array<u8, 2048> packet;
			sockaddr_in from = {};
#ifdef _WIN32
			int from_size = sizeof(from);
#else
			socklen_t from_size = sizeof(from);
#endif
			const auto size = recvfrom(s_socket, reinterpret_cast<char*>(packet.data()), static_cast<int>(packet.size()), 0,
				reinterpret_cast<sockaddr*>(&from), &from_size);
			if (size < 0)
				break;
			HandlePacket(packet.data(), static_cast<size_t>(size), from);
		}
	}

	void Configure(SessionConfig config)
	{
		CloseSocket();
		s_config = std::move(config);
		s_config.input_delay = std::clamp<u32>(s_config.input_delay, 1, MAX_INPUT_DELAY);
		s_configured = true;
		s_ended = false;
		s_opened = false;
		s_peer = {};
		s_peer_known = false;
		s_heard_from_peer = false;
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

	void End(std::string_view reason)
	{
		if (!IsActive())
			return;

		s_ended = true;
		CloseSocket();
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
				fmt::format("Netplay: hosting on UDP port {}, input delay {}. Waiting for the other player.", s_config.port, s_config.input_delay) :
				fmt::format("Netplay: joining {}:{}, input delay {}.", s_config.peer_host, s_config.port, s_config.input_delay);
			Console.WriteLn(message);
			Host::AddKeyedOSDMessage("Netplay", message, Host::OSD_INFO_DURATION);
		}

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
		Send();

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
				Send();
				last_send = now;
			}

			// Keep the window responsive while waiting: pausing holds the wait, stopping or resetting ends the session.
			Host::PumpMessagesOnCPUThread();
			if (!IsActive())
				return;
			const VMState state = VMManager::GetState();
			if (state == VMState::Paused)
			{
				waiting_since = now;
				continue;
			}
			if (state != VMState::Running)
			{
				End(state == VMState::Resetting ? "the console was reset" : "emulation stopped");
				return;
			}

			const double timeout = s_heard_from_peer ? PEER_TIMEOUT_MS : FIRST_CONTACT_TIMEOUT_MS;
			if (Common::Timer::ConvertValueToMilliseconds(now - waiting_since) >= timeout)
			{
				End(s_heard_from_peer ? "the other player stopped responding" : "the other player never connected");
				return;
			}
		}

		const Input& local = s_local[s_frame % RING_SIZE].input;
		const Input& remote = s_remote[s_frame % RING_SIZE].input;
		PadData(0, 0, s_config.host ? local : remote).OverrideActualController();
		PadData(1, 0, s_config.host ? remote : local).OverrideActualController();
		s_frame++;
	}
} // namespace Netplay
