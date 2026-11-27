// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NA228Detection.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>
#include <string_view>

namespace NA228Detection
{
	static constexpr uint32_t PHYSICAL_MASK = 0x1fffffff;

	// Slot 2 of the ELF's PRG loader destination table. An NA228 build stores 228.BIN's load address there;
	// retail NA2 has zero.
	static constexpr uint32_t PAYLOAD_ADDRESS_SLOT = 0x006029c8;

	// 228.BIN's header word the mod takes the launch language from, and the players' names after it.
	static constexpr uint32_t LAUNCH_LANGUAGE_OFFSET = 0x2c;
	static constexpr uint32_t PLAYER_NAMES_OFFSET = 0x30;

	static std::atomic<uint32_t> s_launch_language{0};

	static bool Read32(std::span<const uint8_t> ram, uint32_t address, uint32_t& value)
	{
		const uint32_t physical = address & PHYSICAL_MASK;
		if (physical > ram.size() || ram.size() - physical < 4)
			return false;

		// Live EE memory can change between reads. Volatile keeps each load explicit;
		// it does not turn the reads into a synchronized guest snapshot.
		const volatile uint8_t* bytes = ram.data() + physical;
		value = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
		return true;
	}

	// 228.BIN's header, as a physical address, once a running NA228 build has loaded it.
	static std::optional<uint32_t> HeaderBase(std::span<const uint8_t> ram)
	{
		ram = ram.first(std::min<size_t>(ram.size(), EE_RAM_SIZE));

		uint32_t base, magic, load_address, name_start, name_end;
		if (!Read32(ram, PAYLOAD_ADDRESS_SLOT, base) || base == 0)
			return std::nullopt;

		// 228.BIN's header: magic, its own load address at +8, and "228.bin" at +0x20.
		base &= PHYSICAL_MASK;
		if (Read32(ram, base, magic) && magic == 0x336f574d &&
			Read32(ram, base + 0x08, load_address) && (load_address & PHYSICAL_MASK) == base &&
			Read32(ram, base + 0x20, name_start) && name_start == 0x2e383232 &&
			Read32(ram, base + 0x24, name_end) && name_end == 0x006e6962)
		{
			return base;
		}
		return std::nullopt;
	}

	bool Detect(std::span<const uint8_t> ram)
	{
		return HeaderBase(ram).has_value();
	}

	void SetLaunchLanguage(uint32_t word)
	{
		s_launch_language.store(word, std::memory_order_relaxed);
	}

	void ApplyLaunchLanguage(std::span<uint8_t> ram)
	{
		const uint32_t word = s_launch_language.load(std::memory_order_relaxed);
		if (word == 0)
			return;
		const std::optional<uint32_t> base = HeaderBase(ram);
		uint32_t current;
		if (!base.has_value() || !Read32(ram, *base + LAUNCH_LANGUAGE_OFFSET, current) || current == word)
			return;
		std::memcpy(ram.data() + *base + LAUNCH_LANGUAGE_OFFSET, &word, sizeof(word));
	}

	// Whether the game's font draws this character, which Latin-1 holds as the same number.
	static bool DrawsNameCharacter(char32_t c)
	{
		static constexpr std::u32string_view LATIN = U"ÀÁÂÃÇÉÊÍÓÔÕÚÜàáâãçéêíóôõúü";
		return (c >= 0x20 && c <= 0x7e) || LATIN.find(c) != std::u32string_view::npos;
	}

	static void MakePlayerName(std::string_view utf8, uint8_t* out)
	{
		size_t length = 0;
		for (size_t i = 0; i < utf8.size() && length < PLAYER_NAME_SIZE - 1;)
		{
			const uint8_t lead = static_cast<uint8_t>(utf8[i]);
			const size_t extra = lead < 0x80 ? 0 : (lead & 0xe0) == 0xc0 ? 1 : (lead & 0xf0) == 0xe0 ? 2 : (lead & 0xf8) == 0xf0 ? 3 : 0;
			char32_t c = extra == 0 ? lead : (lead & (0x3f >> extra));
			bool valid = lead < 0x80 || extra != 0;
			for (size_t k = 1; k <= extra; k++)
			{
				if (i + k >= utf8.size() || (static_cast<uint8_t>(utf8[i + k]) & 0xc0) != 0x80)
				{
					valid = false;
					break;
				}
				c = (c << 6) | (static_cast<uint8_t>(utf8[i + k]) & 0x3f);
			}
			i += valid ? extra + 1 : 1;
			if (valid && DrawsNameCharacter(c))
				out[length++] = static_cast<uint8_t>(c);
		}
	}

	PlayerNames MakePlayerNames(std::string_view player1, std::string_view player2)
	{
		PlayerNames names{};
		MakePlayerName(player1, names.data());
		MakePlayerName(player2, names.data() + PLAYER_NAME_SIZE);
		return names;
	}

	void WritePlayerNames(std::span<uint8_t> ram, const PlayerNames& names)
	{
		const std::optional<uint32_t> base = HeaderBase(ram);
		if (!base.has_value() || ram.size() - (*base + PLAYER_NAMES_OFFSET) < names.size())
			return;
		uint8_t* const field = ram.data() + *base + PLAYER_NAMES_OFFSET;
		if (std::memcmp(field, names.data(), names.size()) != 0)
			std::memcpy(field, names.data(), names.size());
	}
} // namespace NA228Detection
