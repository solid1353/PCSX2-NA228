// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// How tall Recenter makes the game's window, its frame and title bar included, which each fork sets for itself:
// a share of the free height of the window's screen, or else a fixed height in device-independent pixels. The window
// is as wide as the picture's shape needs, and centered on that screen.
namespace RecenterSize
{
	// The share of the screen's free height, above 0 and at most 1; 0 uses FIXED_HEIGHT instead.
	inline constexpr float SCREEN_HEIGHT_SHARE = 0.75f;

	// The window's height in device-independent pixels, when SCREEN_HEIGHT_SHARE is 0.
	inline constexpr int FIXED_HEIGHT = 0;
} // namespace RecenterSize
