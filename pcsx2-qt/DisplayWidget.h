// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "common/WindowInfo.h"
#include <QtCore/QTimer>
#include <QtGui/QDragMoveEvent>
#include <QtGui/QWindow>
#include <optional>
#include <vector>

class QCloseEvent;

class DisplaySurface final : public QWindow
{
	Q_OBJECT

public:
	explicit DisplaySurface();
	~DisplaySurface();

	// while QWindow can be used directly as a window, Popups requre a QWidget parent.
	// Additionally, we use saveGeometry/restoreGeometry for render to seperate window mode
	// but those functions only exist on QWidget.
	// Thus, we always need a container widget.
	QWidget* createWindowContainer(QWidget* parent = nullptr);

	std::optional<WindowInfo> getWindowInfo();

	void updateRelativeMode(bool enabled);
	// hidden: hide the cursor over the window once the mouse rests, and show it again while it moves.
	void updateCursor(bool hidden);

	bool isFullScreen() const;
	void setFocus();

	QByteArray saveGeometry() const;
	void restoreGeometry(const QByteArray& geometry);

Q_SIGNALS:
	void windowResizedEvent(u32 width, u32 height, float scale);
	void windowRestoredEvent();

	void dragEnterEvent(QDragEnterEvent* event);
	void dropEvent(QDropEvent* event);

protected:
	void handleCloseEvent(QCloseEvent* event);
	void handleKeyInputEvent(QEvent* event);
	bool event(QEvent* event) override;
	bool eventFilter(QObject* object, QEvent* event) override;

private Q_SLOTS:
	void onResizeDebounceTimer();

private:
	void updateCenterPos();
	void setCursorHidden(bool hidden);

	// How long the mouse rests before the cursor hides.
	static constexpr int CURSOR_IDLE_MS = 2000;

	QPoint m_relative_mouse_start_pos{};
	QPoint m_relative_mouse_center_pos{};
	bool m_relative_mouse_enabled = false;
#ifdef _WIN32
	bool m_clip_mouse_enabled = false;
#endif
	bool m_cursor_hidden = false;
	bool m_cursor_hide_when_idle = false;
	QPoint m_cursor_last_pos{};
	QTimer* m_cursor_idle_timer = nullptr;

	std::vector<int> m_keys_pressed_with_modifiers;

	u32 m_last_window_width = 0;
	u32 m_last_window_height = 0;
	float m_last_window_scale = 1.0f;

	QTimer* m_resize_debounce_timer = nullptr;
	QWidget* m_container = nullptr;
};
