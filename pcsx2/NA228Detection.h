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

	// The language the NA2.28 app starts the game in, -na228-language: 228.BIN's header word at +0x2C as the mod reads
	// it, the language's folder code in its 4 bytes, "EN\0\0" or "PT\0\0", or zero for none. The mod takes it once
	// Continue's silent load is over, when no save holds a language, so the first-boot language screen never opens.
	void SetLaunchLanguage(uint32_t word);

	// Writes the launch language into 228.BIN's header once a running NA228 build has loaded it, whenever the word
	// differs, as the game may read the whole file again. Nothing without a launch language. Same span rules as Detect.
	void ApplyLaunchLanguage(std::span<uint8_t> ram);
} // namespace NA228Detection
