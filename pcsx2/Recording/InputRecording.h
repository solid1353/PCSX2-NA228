// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <functional>
#include <queue>

#include "Recording/InputRecordingFile.h"
#include "Recording/InputRecordingControls.h"

class InputRecording
{
public:
	enum class Type
	{
		POWER_ON,
		FROM_SAVESTATE
	};

	bool create(const std::string& filename, const bool fromSaveState, const std::string& authorName);
	/// With `view_only`, the recording is only played: its file opens read-only, and once the playback has paused on its
	/// last frame, resuming ends it and the game plays on, where PCSX2 otherwise records on into the file.
	bool play(const std::string& path, bool view_only = false);
	void stop();

	static void InformGSThread();
	void handleControllerDataUpdate();
	void saveControllerData(const PadData& data, const int port, const int slot);
	std::optional<PadData> updateControllerData(const int port, const int slot);
	void incFrameCounter();
	u32 getFrameCounter() const;
	u32 getFrameCounterStateless() const;
	bool isActive() const;
	void processRecordQueue();

	void setStartingFrame(u32 startingFrame);
	u32 getStartingFrame();

	void handleExceededFrameCounter();
	void handleReset();
	void handleLoadingSavestate();
	bool isTypeSavestate() const;
	void adjustFrameCounterOnReRecord(u32 newFrameCounter);

	InputRecordingControls& getControls();
	const InputRecordingFile& getData() const;

private:
	InputRecordingControls m_controls;
	InputRecordingFile m_file;

	Type m_type;

	bool m_initial_load_complete = false;
	bool m_is_active = false;
	bool m_watching_for_rerecords = false;
	bool m_view_only = false;

	// A consistent way to run actions at the end of the each frame (ie. stop the recording)
	std::queue<std::function<void()>> m_recordingQueue;

	u32 m_frame_counter = 0;
	u32 m_frame_counter_stateless = 0;
	// Either 0 for a power-on movie, or the g_FrameCount that is stored on the starting frame
	u32 m_starting_frame = 0;

	void initializeState();
	void closeActiveFile();
};

extern InputRecording g_InputRecording;
