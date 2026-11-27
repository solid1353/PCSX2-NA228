// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NA228Detection.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace
{
	constexpr uint32_t SLOT = 0x006029c8;
	constexpr uint32_t BASE = 0x008dd080;
	constexpr uint32_t EN = 'E' | ('N' << 8);
	constexpr uint32_t PT = 'P' | ('T' << 8);

	void Put32(std::vector<uint8_t>& ram, uint32_t address, uint32_t value) { std::memcpy(ram.data() + address, &value, 4); }
	uint32_t Get32(const std::vector<uint8_t>& ram, uint32_t address)
	{
		uint32_t value;
		std::memcpy(&value, ram.data() + address, 4);
		return value;
	}

	// EE RAM as a running NA228 build has it once 228.BIN is loaded.
	std::vector<uint8_t> LoadedRam()
	{
		std::vector<uint8_t> ram(NA228Detection::EE_RAM_SIZE);
		Put32(ram, SLOT, BASE);
		Put32(ram, BASE, 0x336f574d);
		Put32(ram, BASE + 0x08, BASE);
		Put32(ram, BASE + 0x20, 0x2e383232);
		Put32(ram, BASE + 0x24, 0x006e6962);
		return ram;
	}

	class LaunchLanguageTest : public testing::Test
	{
	protected:
		void TearDown() override { NA228Detection::SetLaunchLanguage(0); }
	};
} // namespace

TEST_F(LaunchLanguageTest, WritesTheLanguageIntoTheLoadedHeaderAndAgainOnceItIsCleared)
{
	std::vector<uint8_t> ram = LoadedRam();
	NA228Detection::SetLaunchLanguage(PT);
	NA228Detection::ApplyLaunchLanguage(ram);
	EXPECT_EQ(Get32(ram, BASE + 0x2c), PT);

	// The game reading 228.BIN again clears the word.
	Put32(ram, BASE + 0x2c, 0);
	NA228Detection::ApplyLaunchLanguage(ram);
	EXPECT_EQ(Get32(ram, BASE + 0x2c), PT);
	EXPECT_EQ(Get32(ram, BASE + 0x28), 0u);
	EXPECT_EQ(Get32(ram, BASE + 0x30), 0u);
}

TEST_F(LaunchLanguageTest, WritesNothingBefore228BinLoadsOrWithoutALanguage)
{
	std::vector<uint8_t> ram = LoadedRam();
	Put32(ram, BASE + 0x20, 0);
	NA228Detection::SetLaunchLanguage(EN);
	NA228Detection::ApplyLaunchLanguage(ram);
	EXPECT_EQ(Get32(ram, BASE + 0x2c), 0u);

	ram = LoadedRam();
	NA228Detection::SetLaunchLanguage(0);
	NA228Detection::ApplyLaunchLanguage(ram);
	EXPECT_EQ(Get32(ram, BASE + 0x2c), 0u);
}

TEST(PlayerNamesTest, KeepsWhatTheGameDrawsAsLatin1AndCutsEachTo16Bytes)
{
	// "João" keeps its ã, "Zoë" loses the ë the game can't draw, and a long name stops at 16 bytes.
	const NA228Detection::PlayerNames names = NA228Detection::MakePlayerNames("Jo\xC3\xA3o-1!", "Zo\xC3\xAB" "abcdefghijklmnopq");
	const uint8_t player1[] = {'J', 'o', 0xe3, 'o', '-', '1', '!', 0};
	EXPECT_EQ(std::memcmp(names.data(), player1, sizeof(player1)), 0);
	const uint8_t player2[] = {'Z', 'o', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 0};
	EXPECT_EQ(std::memcmp(names.data() + NA228Detection::PLAYER_NAME_SIZE, player2, sizeof(player2)), 0);
	EXPECT_EQ(names[34], 0);
	EXPECT_EQ(names[35], 0);
}

TEST(PlayerNamesTest, WritesBothFieldsOnceTheHeaderLoadsAndLeavesTheWordsBeforeThem)
{
	std::vector<uint8_t> ram = LoadedRam();
	Put32(ram, BASE + 0x2c, PT);
	const NA228Detection::PlayerNames names = NA228Detection::MakePlayerNames("Ana", "Bo");
	NA228Detection::WritePlayerNames(ram, names);
	EXPECT_EQ(std::memcmp(ram.data() + BASE + 0x30, names.data(), names.size()), 0);
	EXPECT_EQ(ram[BASE + 0x41], 'B');
	EXPECT_EQ(Get32(ram, BASE + 0x2c), PT);
	EXPECT_EQ(Get32(ram, BASE + 0x54), 0u);

	std::vector<uint8_t> unloaded = LoadedRam();
	Put32(unloaded, SLOT, 0);
	NA228Detection::WritePlayerNames(unloaded, names);
	EXPECT_EQ(unloaded[BASE + 0x30], 0);
}
