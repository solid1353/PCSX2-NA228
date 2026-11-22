// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <span>

namespace NA228Detection
{
	inline constexpr uint32_t EE_RAM_SIZE = 32 * 1024 * 1024;

	// Whether a running NA228 build has loaded its 228.BIN payload.
	// The span must refer directly to EE RAM and remain valid throughout the call.
	// Callers must check VM lifetime before supplying live memory. No guest accessors are used.
	bool Detect(std::span<const uint8_t> ram);
} // namespace NA228Detection
