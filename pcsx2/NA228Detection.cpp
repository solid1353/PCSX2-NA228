// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NA228Detection.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>

namespace NA228Detection
{
	static constexpr uint32_t PHYSICAL_MASK = 0x1fffffff;

	// Slot 2 of the ELF's PRG loader destination table. An NA228 build stores 228.BIN's load address there;
	// retail NA2 has zero.
	static constexpr uint32_t PAYLOAD_ADDRESS_SLOT = 0x006029c8;

	// 228.BIN's header word the mod takes the launch language from.
	static constexpr uint32_t LAUNCH_LANGUAGE_OFFSET = 0x2c;

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
} // namespace NA228Detection
