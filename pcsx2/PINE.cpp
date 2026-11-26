// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "BuildVersion.h"
#include "Common.h"
#include "Host.h"
#include "Elfheader.h"
#include "GS.h"
#include "ImGui/FullscreenUI.h"
#include "MTGS.h"
#include "Netplay.h"
#include "WaitingRoom.h"
#include "SaveState.h"
#include "PINE.h"
#include "VMManager.h"
#include "vtlb.h"
#include "common/Error.h"
#include "common/Threading.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include <sys/types.h>
#include <thread>

#include "fmt/format.h"

#if defined(_WIN32)
#define read_portable(a, b, c) (recv(a, (char*)b, c, 0))
#define write_portable(a, b, c) (send(a, (const char*)b, c, 0))
#define safe_close_portable(a) \
	do \
	{ \
		if ((a) >= 0) \
		{ \
			closesocket((a)); \
			(a) = INVALID_SOCKET; \
		} \
	} while (0)
#include "common/RedtapeWindows.h"
#include <WinSock2.h>
#elif defined(__linux__) || defined(__FreeBSD__)
#define read_portable(a, b, c) (read(a, b, c))
#define write_portable(a, b, c) (send(a, b, c, MSG_NOSIGNAL))
#define safe_close_portable(a) \
	do \
	{ \
		if ((a) >= 0) \
		{ \
			close((a)); \
			(a) = -1; \
		} \
	} while (0)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#else
#define read_portable(a, b, c) (read(a, b, c))
#define write_portable(a, b, c) (write(a, b, c))
#define safe_close_portable(a) \
	do \
	{ \
		if ((a) >= 0) \
		{ \
			close((a)); \
			(a) = -1; \
		} \
	} while (0)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define PINE_EMULATOR_NAME "pcsx2"

#ifdef _WIN32

static bool InitializeWinsock()
{
	static bool initialized = false;
	if (initialized)
		return true;

	WSADATA wsa = {};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;

	initialized = true;
	std::atexit([]() { WSACleanup(); });
	return true;
}

#endif

namespace PINEServer
{
	static std::thread s_thread;
	static int s_slot;

#ifdef _WIN32
	// windows claim to have support for AF_UNIX sockets but that is a blatant lie,
	// their SDK won't even run their own examples, so we go on TCP sockets.
	using Socket = SOCKET;
	static constexpr Socket NO_SOCKET = INVALID_SOCKET;
#else
	// absolute path of the socket. Stored in XDG_RUNTIME_DIR, if unset /tmp
	static std::string s_socket_name;
	using Socket = int;
	static constexpr Socket NO_SOCKET = -1;
#endif
	static Socket s_sock = NO_SOCKET;

	// PCSX2-NA228 serves several clients at once, each on its own thread, so the NA2.28 app can follow a match while a
	// test script plays it; the commands themselves run one at a time.
	struct Client
	{
		Socket socket = NO_SOCKET;
		std::thread thread;
		std::atomic<bool> done{false};
	};
	static std::mutex s_clients_mutex;
	static std::vector<std::unique_ptr<Client>> s_clients;
	static std::mutex s_command_mutex;

	// The longest path MsgScreenshotToFile takes, the size of each entry MsgNetplayInput takes, and the longest guest
	// name MsgWaitingRoom takes.
	static constexpr u32 MAX_SCREENSHOT_PATH = 32768;
	static constexpr u32 NETPLAY_INPUT_ENTRY_SIZE = 4 + 1 + 18;
	static constexpr u32 MAX_GUEST_NAME = 256;

	// Whether the socket processing thread should stop executing/is stopped.
	static std::atomic_bool s_end{true};

	/**
	 * Maximum memory used by an IPC message request.
	 * Equivalent to 50,000 Write64 requests.
	 */
#define MAX_IPC_SIZE 650000

	/**
	 * Maximum memory used by an IPC message reply.
	 * Equivalent to 50,000 Read64 replies.
	 */
#define MAX_IPC_RETURN_SIZE 450000


	/**
	 * IPC Command messages opcodes.
	 * A list of possible operations possible by the IPC.
	 * Each one of them is what we call an "opcode" and is the first
	 * byte sent by the IPC to differentiate between commands.
	 */
	enum IPCCommand : unsigned char
	{
		MsgRead8 = 0, /**< Read 8 bit value to memory. */
		MsgRead16 = 1, /**< Read 16 bit value to memory. */
		MsgRead32 = 2, /**< Read 32 bit value to memory. */
		MsgRead64 = 3, /**< Read 64 bit value to memory. */
		MsgWrite8 = 4, /**< Write 8 bit value to memory. */
		MsgWrite16 = 5, /**< Write 16 bit value to memory. */
		MsgWrite32 = 6, /**< Write 32 bit value to memory. */
		MsgWrite64 = 7, /**< Write 64 bit value to memory. */
		MsgVersion = 8, /**< Returns PCSX2 version. */
		MsgSaveState = 9, /**< Saves a savestate. */
		MsgLoadState = 0xA, /**< Loads a savestate. */
		MsgTitle = 0xB, /**< Returns the game title. */
		MsgID = 0xC, /**< Returns the game ID. */
		MsgUUID = 0xD, /**< Returns the game UUID. */
		MsgGameVersion = 0xE, /**< Returns the game verion. */
		MsgStatus = 0xF, /**< Returns the emulator status. */
		MsgScreenshotToFile = 0x1F, /**< As the dev fork's: writes the next frame as PNG to an exact path. */
		MsgNetplayStatus = 0x80, /**< PCSX2-NA228's own: returns the netplay session's status, for the NA2.28 app. */
		MsgNetplayInput = 0x81, /**< PCSX2-NA228's own: schedules an agent's input for this PC's netplay player. */
		MsgWaitingRoom = 0x82, /**< PCSX2-NA228's own: the NA2.28 app's news of the room an offline player hosts. */
		MsgOSDScreenshotToFile = 0x83, /**< PCSX2-NA228's own: as MsgScreenshotToFile, on-screen display included. */
		MsgClose = 0x84, /**< PCSX2-NA228's own: closes PCSX2, as its Close does, for an agent done with it. */
		MsgUnimplemented = 0xFF /**< Unimplemented IPC message. */
	};

	/**
	 * Emulator status enum.
	 * A list of possible emulator statuses.
	 */
	enum EmuStatus : uint32_t
	{
		Running = 0, /**< Game is running */
		Paused = 1, /**< Game is paused */
		Shutdown = 2 /**< Game is shutdown */
	};

	/**
	 * IPC message buffer.
	 * A list of all needed fields to store an IPC message.
	 */
	struct IPCBuffer
	{
		int size; /**< Size of the buffer. */
		std::vector<u8> buffer; /**< Buffer. */
	};

	/**
	 * IPC result codes.
	 * A list of possible result codes the IPC can send back.
	 * Each one of them is what we call an "opcode" or "tag" and is the
	 * first byte sent by the IPC to differentiate between results.
	 */
	enum IPCResult : unsigned char
	{
		IPC_OK = 0, /**< IPC command successfully completed. */
		IPC_FAIL = 0xFF /**< IPC command failed to complete. */
	};

	// Thread used to accept clients, and each client's thread.
	void MainLoop();
	void ClientLoop(Client* client);

	/**
	 * Internal function, Parses an IPC command.
	 * buf: buffer containing the IPC command.
	 * buf_size: size of the buffer announced.
	 * ret_buffer: buffer that will be used to send the reply.
	 * return value: IPCBuffer containing a buffer with the result
	 *               of the command and its size.
	 */
	static IPCBuffer ParseCommand(std::span<u8> buf, std::vector<u8>& ret_buffer, u32 buf_size);

	/**
	 * Formats an IPC buffer
	 * ret_buffer: return buffer to use.
	 * size: size of the IPC buffer.
	 * return value: buffer containing the status code allocated of size
	 */
	static std::vector<u8>& MakeOkIPC(std::vector<u8>& ret_buffer, uint32_t size);
	static std::vector<u8>& MakeFailIPC(std::vector<u8>& ret_buffer, uint32_t size);

	/**
	 * Waits for the next client and returns its socket, or NO_SOCKET.
	 */
	Socket AcceptClient();

	/**
	 * Converts a primitive value to bytes in little endian
	 * res_vector: the vector to modify
	 * res: the value to convert
	 * i: where to insert it into the vector
	 * NB: implicitely inlined
	 */
	template <typename T>
	static void ToResultVector(std::vector<u8>& res_vector, T res, int i)
	{
		memcpy(&res_vector[i], (char*)&res, sizeof(T));
	}

	/**
	 * Converts bytes in little endian to a primitive value
	 * span: the span to convert
	 * i: where to load it from the span
	 * return value: the converted value
	 * NB: implicitely inlined
	 */
	template <typename T>
	static T FromSpan(std::span<u8> span, int i)
	{
		return *(T*)(&span[i]);
	}

	/**
	 * Ensures an IPC message isn't too big.
	 * return value: false if checks failed, true otherwise.
	 */
	static inline bool SafetyChecks(u32 command_len, int command_size, u32 reply_len, int reply_size = 0, u32 buf_size = MAX_IPC_SIZE - 1)
	{
		return !((command_len + command_size) > buf_size ||
				 (reply_len + reply_size) >= MAX_IPC_RETURN_SIZE);
	}
} // namespace PINEServer

bool PINEServer::Initialize(int slot)
{
	s_end.store(false, std::memory_order_release);
	s_slot = slot;

#ifdef _WIN32
	if (!InitializeWinsock())
	{
		Console.WriteLn(Color_Red, "PINE: Cannot initialize winsock! Shutting down...");
		Deinitialize();
		return false;
	}

	s_sock = socket(AF_INET, SOCK_STREAM, 0);
	if ((s_sock == INVALID_SOCKET) || slot > 65536)
	{
		Console.WriteLn(Color_Red, "PINE: Cannot open socket! Shutting down...");
		Deinitialize();
		return false;
	}

	sockaddr_in server = {};
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
	server.sin_port = htons(slot);

	if (bind(s_sock, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR)
	{
		Console.WriteLn(Color_Red, "PINE: Error while binding to socket! Shutting down...");
		Deinitialize();
		return false;
	}

#else
	char* runtime_dir = nullptr;
#ifdef __APPLE__
	runtime_dir = std::getenv("TMPDIR");
#else
	runtime_dir = std::getenv("XDG_RUNTIME_DIR");
#endif
	// fallback in case macOS or other OSes don't implement the XDG base
	// spec
	if (runtime_dir == nullptr)
		s_socket_name = "/tmp/" PINE_EMULATOR_NAME ".sock";
	else
	{
		s_socket_name = runtime_dir;
		s_socket_name += "/" PINE_EMULATOR_NAME ".sock";
	}

	if (slot != PINE_DEFAULT_SLOT)
		s_socket_name += "." + std::to_string(slot);

	struct sockaddr_un server;

	s_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s_sock < 0)
	{
		Console.WriteLn(Color_Red, "PINE: Cannot open socket! Shutting down...");
		Deinitialize();
		return false;
	}
	server.sun_family = AF_UNIX;
	StringUtil::Strlcpy(server.sun_path, s_socket_name, sizeof(server.sun_path));

	// we unlink the socket so that when releasing this thread the socket gets
	// freed even if we didn't close correctly the loop
	unlink(s_socket_name.c_str());
	if (bind(s_sock, (struct sockaddr*)&server, sizeof(struct sockaddr_un)))
	{
		Console.WriteLn(Color_Red, "PINE: Error while binding to socket! Shutting down...");
		Deinitialize();
		return false;
	}
#endif

	// maximum queue of 4096 commands before refusing, approximated to the
	// nearest legal value. We do not use SOMAXCONN as windows have this idea
	// that a "reasonable" value is 5, which is not.
	if (listen(s_sock, 4096))
	{
		Console.WriteLn(Color_Red, "PINE: Cannot listen for connections! Shutting down...");
		Deinitialize();
		return false;
	}

	// we start the thread
	s_thread = std::thread(&PINEServer::MainLoop);

	return true;
}

bool PINEServer::IsInitialized()
{
	return !s_end.load(std::memory_order_acquire);
}

int PINEServer::GetSlot()
{
	return s_slot;
}

std::vector<u8>& PINEServer::MakeOkIPC(std::vector<u8>& ret_buffer, uint32_t size = 5)
{
	ToResultVector<uint32_t>(ret_buffer, size, 0);
	ret_buffer[4] = IPC_OK;
	return ret_buffer;
}

std::vector<u8>& PINEServer::MakeFailIPC(std::vector<u8>& ret_buffer, uint32_t size = 5)
{
	ToResultVector<uint32_t>(ret_buffer, size, 0);
	ret_buffer[4] = IPC_FAIL;
	return ret_buffer;
}

PINEServer::Socket PINEServer::AcceptClient()
{
	Socket socket = accept(s_sock, 0, 0);
#ifdef _WIN32
	if (socket == INVALID_SOCKET)
#else
	if (socket < 0)
#endif
	{
		// everything else is non recoverable in our scope
		// we also mark as recoverable socket errors where it would block a
		// non blocking socket, even though our socket is blocking, in case
		// we ever have to implement a non blocking socket.
#ifdef _WIN32
		const int errno_w = WSAGetLastError();
		if (!(errno_w == WSAECONNRESET || errno_w == WSAEINTR || errno_w == WSAEINPROGRESS || errno_w == WSAEMFILE || errno_w == WSAEWOULDBLOCK) && s_sock != INVALID_SOCKET)
			Console.Error("PINE: accept() returned error %d", errno_w);
#else
		if (!(errno == ECONNABORTED || errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) && s_sock >= 0)
			Console.Error("PINE: accept() returned error %d", errno);
#endif

		return NO_SOCKET;
	}

#ifdef __APPLE__
	int nosigpipe = 1;
	setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

	// Gross C-style cast, but SOCKET is a handle on Windows.
	Console.WriteLn("PINE: New client with FD %d connected.", (int)socket);
	return socket;
}

void PINEServer::MainLoop()
{
	Threading::SetNameOfCurrentThread("PINE Server");

	while (!s_end.load(std::memory_order_acquire))
	{
		const Socket socket = AcceptClient();
		if (socket == NO_SOCKET)
			continue;

		std::lock_guard lock(s_clients_mutex);
		// Clients that disconnected leave.
		for (auto it = s_clients.begin(); it != s_clients.end();)
		{
			if ((*it)->done.load(std::memory_order_acquire))
			{
				(*it)->thread.join();
				safe_close_portable((*it)->socket);
				it = s_clients.erase(it);
			}
			else
			{
				++it;
			}
		}
		Client* client = s_clients.emplace_back(std::make_unique<Client>()).get();
		client->socket = socket;
		client->thread = std::thread(&PINEServer::ClientLoop, client);
	}
}

void PINEServer::ClientLoop(Client* client)
{
	Threading::SetNameOfCurrentThread("PINE Client");
	// Each client's own buffers, allocated once, as malloc is expensive when we optimize for µs.
	std::vector<u8> ret_buffer(MAX_IPC_RETURN_SIZE);
	std::vector<u8> ipc_buffer(MAX_IPC_SIZE);
	const Socket socket = client->socket;
	const auto disconnect = [client]() {
		Console.WriteLn("PINE: Client disconnected.");
		client->done.store(true, std::memory_order_release);
	};

	while (!s_end.load(std::memory_order_acquire))
	{
		// either int or ssize_t depending on the platform, so we have to
		// use a bunch of auto
		auto receive_length = 0;
		auto end_length = 4;
		const std::span<u8> ipc_buffer_span(ipc_buffer);

		// while we haven't received the entire packet, maybe due to
		// socket datagram splittage, we continue to read
		while (receive_length < end_length)
		{
			const auto tmp_length = read_portable(socket, &ipc_buffer_span[receive_length], MAX_IPC_SIZE - receive_length);

			// the client is gone when an error happens
			if (tmp_length <= 0)
				return disconnect();

			receive_length += tmp_length;

			// if we got at least the final size then update
			if (end_length == 4 && receive_length >= 4)
			{
				end_length = FromSpan<u32>(ipc_buffer_span, 0);
				// we'd like to avoid a client trying to do OOB
				if (end_length > MAX_IPC_SIZE || end_length < 4)
				{
					receive_length = 0;
					break;
				}
			}
		}
		PINEServer::IPCBuffer res;

		// we remove 4 bytes to get the message size out of the IPC command
		// size in ParseCommand.
		// also, if we got a failed command, let's reset the state so we don't
		// end up deadlocking by getting out of sync, eg when a client
		// disconnects
		if (receive_length != 0)
		{
			{
				std::lock_guard lock(s_command_mutex);
				res = ParseCommand(ipc_buffer_span.subspan(4), ret_buffer, (u32)end_length - 4);
			}

			// the client is gone if we cannot send back our answer
			if (write_portable(socket, res.buffer.data(), res.size) < 0)
				return disconnect();
		}
	}
	disconnect();
}

// Queues a screenshot of the next frame the GS presents, as PNG at `path`, without waiting for the CPU thread, which may
// be shutting PINE down and waiting for this client.
// Hidden, only on Direct3D 11: a window this size's view, on-screen display included, a few frames from now.
static bool QueueOSDScreenshot(std::string path, u32 width, u32 height)
{
	if (!VMManager::HasValidVM() || !Host::InSurfacelessMode() || width == 0 || height == 0 || width > 7680 || height > 4320)
		return false;
	Host::RunOnCPUThread([path = std::move(path), width, height]() mutable {
		if (VMManager::HasValidVM())
			MTGS::RunOnGSThread([path = std::move(path), width, height]() {
				if (!GSQueueOSDSnapshot(path, width, height))
					Console.ErrorFmt("PINE: no screenshot with the on-screen display to {}.", path);
			});
	});
	return true;
}

static bool QueueScreenshot(std::string path)
{
	if (!VMManager::HasValidVM())
		return false;
	Host::RunOnCPUThread([path = std::move(path)]() mutable {
		if (VMManager::HasValidVM())
			MTGS::RunOnGSThread([path = std::move(path)]() { GSQueueSnapshot(path); });
	});
	return true;
}

void PINEServer::Deinitialize()
{
	s_end.store(true, std::memory_order_release);

#ifndef _WIN32
	if (!s_socket_name.empty())
	{
		unlink(s_socket_name.c_str());
		s_socket_name = {};
	}
#endif

	// shutdown() is needed, otherwise accept() will still block.
#ifdef _WIN32
	if (s_sock != INVALID_SOCKET)
		shutdown(s_sock, SD_BOTH);
#else
	if (s_sock >= 0)
		shutdown(s_sock, SHUT_RDWR);
#endif

	safe_close_portable(s_sock);

	if (s_thread.joinable())
		s_thread.join();

	// Each client's thread ends once its socket is shut down and closed. On Windows, shutting the socket down doesn't
	// wake a recv() the thread waits in, as for a client that stays connected without asking anything, as the NA2.28
	// app does while its game closes: only closing it does, so it is closed before the wait, or PCSX2 would quit only
	// once that client went.
	std::lock_guard lock(s_clients_mutex);
	for (const std::unique_ptr<Client>& client : s_clients)
	{
#ifdef _WIN32
		shutdown(client->socket, SD_BOTH);
		safe_close_portable(client->socket);
#else
		shutdown(client->socket, SHUT_RDWR);
#endif
		if (client->thread.joinable())
			client->thread.join();
#ifndef _WIN32
		safe_close_portable(client->socket);
#endif
	}
	s_clients.clear();
}

PINEServer::IPCBuffer PINEServer::ParseCommand(std::span<u8> buf, std::vector<u8>& ret_buffer, u32 buf_size)
{
	u32 ret_cnt = 5;
	u32 buf_cnt = 0;

	while (buf_cnt < buf_size)
	{
		if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
			return IPCBuffer{5, MakeFailIPC(ret_buffer)};
		buf_cnt++;
		// example IPC messages: MsgRead/Write
		// refer to the client doc for more info on the format
		//         IPC Message event (1 byte)
		//         |  Memory address (4 byte)
		//         |  |           argument (VLE)
		//         |  |           |
		// format: XX YY YY YY YY ZZ ZZ ZZ ZZ
		//        reply code: 00 = OK, FF = NOT OK
		//        |  return value (VLE)
		//        |  |
		// reply: XX ZZ ZZ ZZ ZZ
		switch ((IPCCommand)buf[buf_cnt - 1])
		{
			case MsgRead8:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 1, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u8 res = vtlb_ramRead<mem8_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 1;
				buf_cnt += 4;
				break;
			}
			case MsgRead16:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 2, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u16 res = vtlb_ramRead<mem16_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 2;
				buf_cnt += 4;
				break;
			}
			case MsgRead32:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 4, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u32 res = vtlb_ramRead<mem32_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 4;
				buf_cnt += 4;
				break;
			}
			case MsgRead64:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 8, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u64 res = vtlb_ramRead<mem64_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 8;
				buf_cnt += 4;
				break;
			}
			case MsgWrite8:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 1 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem8_t>(a, FromSpan<u8>(buf, buf_cnt + 4));
				buf_cnt += 5;
				break;
			}
			case MsgWrite16:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 2 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem16_t>(a, FromSpan<u16>(buf, buf_cnt + 4));
				buf_cnt += 6;
				break;
			}
			case MsgWrite32:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem32_t>(a, FromSpan<u32>(buf, buf_cnt + 4));
				buf_cnt += 8;
				break;
			}
			case MsgWrite64:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 8 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem64_t>(a, FromSpan<u64>(buf, buf_cnt + 4));
				buf_cnt += 12;
				break;
			}
			case MsgVersion:
			{
				u32 size = strlen(BuildVersion::GitRev) + 7;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				snprintf(reinterpret_cast<char*>(&ret_buffer[ret_cnt]), size, "PCSX2 %s", BuildVersion::GitRev);
				ret_cnt += size;
				break;
			}
			case MsgSaveState:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				Host::RunOnCPUThread([slot = FromSpan<u8>(buf, buf_cnt)] {
					VMManager::SaveStateToSlot(slot, true, [slot](const std::string& error) {
						SaveState_ReportSaveErrorOSD(error, slot);
					});
				});
				buf_cnt += 1;
				break;
			}
			case MsgLoadState:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				Host::RunOnCPUThread([slot = FromSpan<u8>(buf, buf_cnt)] {
					Error state_error;
					if (!VMManager::LoadStateFromSlot(slot, false, &state_error))
						SaveState_ReportLoadErrorOSD(state_error.GetDescription(), slot, false);
				});
				buf_cnt += 1;
				break;
			}
			case MsgTitle:
			{
				if (!VMManager::HasValidVM())
					goto error;
				const std::string gameName = VMManager::GetTitle(false);
				const u32 size = gameName.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], gameName.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgID:
			{
				if (!VMManager::HasValidVM())
					goto error;
				const std::string gameSerial = VMManager::GetDiscSerial();
				const u32 size = gameSerial.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], gameSerial.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgUUID:
			{
				if (!VMManager::HasValidVM())
					goto error;
				const std::string crc = fmt::format("{:08x}", VMManager::GetDiscCRC());
				const u32 size = crc.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], crc.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgGameVersion:
			{
				if (!VMManager::HasValidVM())
					goto error;

				const std::string ElfVersion = VMManager::GetDiscVersion();
				const u32 size = ElfVersion.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], ElfVersion.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgStatus:
			{
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, 4, buf_size)) [[unlikely]]
					goto error;
				EmuStatus status;

				switch (VMManager::GetState())
				{
					case VMState::Running:
						status = EmuStatus::Running;
						break;
					case VMState::Paused:
						status = EmuStatus::Paused;
						break;
					default:
						status = EmuStatus::Shutdown;
						break;
				}

				ToResultVector(ret_buffer, status, ret_cnt);
				ret_cnt += 4;
				break;
			}
			case MsgScreenshotToFile:
			{
				// u8 version 1, u32 length, then the path's bytes; replies u8 1 once queued. The next frame the GS
				// presents is written there as PNG, so the caller waits for the file.
				if (!SafetyChecks(buf_cnt, 5, ret_cnt, 1, buf_size) || buf[buf_cnt] != 1) [[unlikely]]
					goto error;
				const u32 length = FromSpan<u32>(buf, buf_cnt + 1);
				if (length == 0 || length > MAX_SCREENSHOT_PATH || !SafetyChecks(buf_cnt + 5, static_cast<int>(length), ret_cnt, 1, buf_size))
					goto error;
				std::string path(reinterpret_cast<const char*>(&buf[buf_cnt + 5]), length);
				if (path.find('\0') != std::string::npos || !QueueScreenshot(std::move(path)))
					goto error;
				buf_cnt += 5 + length;
				ret_buffer[ret_cnt] = 1;
				ret_cnt += 1;
				break;
			}
			case MsgOSDScreenshotToFile:
			{
				// u8 version 1, u32 width, u32 height, u32 length, then the path's bytes; replies u8 1 once queued. A
				// few frames on, what a window that size would show, on-screen display included, is written there as
				// PNG, so the caller waits for the file. Only a hidden PCSX2 on Direct3D 11 takes it.
				if (!SafetyChecks(buf_cnt, 13, ret_cnt, 1, buf_size) || buf[buf_cnt] != 1) [[unlikely]]
					goto error;
				const u32 width = FromSpan<u32>(buf, buf_cnt + 1);
				const u32 height = FromSpan<u32>(buf, buf_cnt + 5);
				const u32 length = FromSpan<u32>(buf, buf_cnt + 9);
				if (length == 0 || length > MAX_SCREENSHOT_PATH || !SafetyChecks(buf_cnt + 13, static_cast<int>(length), ret_cnt, 1, buf_size))
					goto error;
				std::string path(reinterpret_cast<const char*>(&buf[buf_cnt + 13]), length);
				if (path.find('\0') != std::string::npos || !QueueOSDScreenshot(std::move(path), width, height))
					goto error;
				buf_cnt += 13 + length;
				ret_buffer[ret_cnt] = 1;
				ret_cnt += 1;
				break;
			}
			case MsgClose:
			{
				// u8 version 1; replies u8 1, and PCSX2 closes as its Close closes it, without asking, so its PINE
				// connection ends with it.
				if (!SafetyChecks(buf_cnt, 1, ret_cnt, 1, buf_size) || buf[buf_cnt] != 1) [[unlikely]]
					goto error;
				Console.WriteLn("PINE: closing PCSX2 for its client.");
				Host::RequestExitApplication(false);
				buf_cnt += 1;
				ret_buffer[ret_cnt] = 1;
				ret_cnt += 1;
				break;
			}
			case MsgNetplayInput:
			{
				// u8 version 1, u8 count, then `count` entries of {u32 frame, u8 controlled, u8 state[18]}, which
				// Netplay::ScheduleAgentInput takes; replies u8 1. An entry with `controlled` 0 hands the player back.
				if (!SafetyChecks(buf_cnt, 2, ret_cnt, 1, buf_size) || buf[buf_cnt] != 1) [[unlikely]]
					goto error;
				const u32 count = buf[buf_cnt + 1];
				if (count == 0 || !SafetyChecks(buf_cnt + 2, static_cast<int>(count * NETPLAY_INPUT_ENTRY_SIZE), ret_cnt, 1, buf_size))
					goto error;
				for (u32 i = 0; i < count; i++)
				{
					const u32 at = buf_cnt + 2 + i * NETPLAY_INPUT_ENTRY_SIZE;
					std::optional<std::array<u8, 18>> state;
					if (buf[at + 4] != 0)
					{
						state.emplace();
						memcpy(state->data(), &buf[at + 5], state->size());
					}
					if (!Netplay::ScheduleAgentInput(FromSpan<u32>(buf, at), state))
						goto error;
				}
				buf_cnt += 2 + count * NETPLAY_INPUT_ENTRY_SIZE;
				ret_buffer[ret_cnt] = 1;
				ret_cnt += 1;
				break;
			}
			case MsgWaitingRoom:
			{
				// u8 version 1, u8 event as WaitingRoom::Event numbers it, and for Joined, u32 length and the guest's
				// name in UTF-8; replies u8 1.
				if (!SafetyChecks(buf_cnt, 2, ret_cnt, 1, buf_size) || buf[buf_cnt] != 1) [[unlikely]]
					goto error;
				const u8 event = buf[buf_cnt + 1];
				if (event < static_cast<u8>(WaitingRoom::Event::Opened) || event > static_cast<u8>(WaitingRoom::Event::Closed))
					goto error;
				std::string guest;
				u32 size = 2;
				if (event == static_cast<u8>(WaitingRoom::Event::Joined))
				{
					if (!SafetyChecks(buf_cnt + 2, 4, ret_cnt, 1, buf_size))
						goto error;
					const u32 length = FromSpan<u32>(buf, buf_cnt + 2);
					if (length > MAX_GUEST_NAME || !SafetyChecks(buf_cnt + 6, static_cast<int>(length), ret_cnt, 1, buf_size))
						goto error;
					guest.assign(reinterpret_cast<const char*>(&buf[buf_cnt + 6]), length);
					if (guest.find('\0') != std::string::npos)
						goto error;
					size += 4 + length;
				}
				WaitingRoom::Report(static_cast<WaitingRoom::Event>(event), guest);
				buf_cnt += size;
				ret_buffer[ret_cnt] = 1;
				ret_cnt += 1;
				break;
			}
			case MsgNetplayStatus:
			{
				// Key=value lines, as Netplay::ReportStatus gives them, sized and terminated like MsgTitle's title, and
				// once the player chose Stop Hosting in a waiting room, stop_hosting=1, until the app reports the room
				// closed.
				std::string netplay = Netplay::ReportStatus();
				if (WaitingRoom::IsStopHostingChosen())
					netplay += "stop_hosting=1\n";
				const u32 size = netplay.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], netplay.c_str(), size);
				ret_cnt += size;
				break;
			}
			default:
			{
			error:
				return IPCBuffer{5, MakeFailIPC(ret_buffer)};
			}
		}
	}
	return IPCBuffer{(int)ret_cnt, MakeOkIPC(ret_buffer, ret_cnt)};
}
