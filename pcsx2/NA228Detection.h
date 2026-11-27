// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

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

	// The two players' names as the game draws them in place of the HUD's character names: 228.BIN's header from
	// +0x30, player 1's then player 2's, each up to 16 bytes and a NUL, zero-padded, then two zero bytes, so the field
	// is whole words. An empty name shows the character's.
	inline constexpr size_t PLAYER_NAME_SIZE = 17;
	inline constexpr size_t PLAYER_NAMES_SIZE = 36;
	using PlayerNames = std::array<uint8_t, PLAYER_NAMES_SIZE>;

	// Both names, given in UTF-8, as the game's bytes: Latin-1, keeping only what the game's font draws, A to Z, a to z,
	// 0 to 9, space, the 32 ASCII punctuation marks and ÀÁÂÃÇÉÊÍÓÔÕÚÜ àáâãçéêíóôõúü, each cut to 16 bytes.
	PlayerNames MakePlayerNames(std::string_view player1, std::string_view player2);

	// Writes the names into 228.BIN's header once a running NA228 build has loaded it, whenever they differ. Same span
	// rules as Detect.
	void WritePlayerNames(std::span<uint8_t> ram, const PlayerNames& names);
} // namespace NA228Detection
