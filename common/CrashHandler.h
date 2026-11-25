// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <string_view>

#ifndef _WIN32
#include <csignal>
#endif

namespace CrashHandler
{
	bool Install();
	void SetWriteDirectory(std::string_view dump_directory);
	void WriteDumpForCaller();

	/// For a PCSX2 that runs hidden: a crash or a failed assertion ends it without any dialog, the system's included,
	/// its report going to stderr and the crash dump.
	void SetNoDialogs();
	bool ShowsNoDialogs();

#ifndef _WIN32
	// Allow crash handler to be invoked from a signal.
	void CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx);
#endif
} // namespace CrashHandler
