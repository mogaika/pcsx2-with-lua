// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPlainTextEdit>

class GameEventLogWindow : public QMainWindow
{
	Q_OBJECT

public:
	GameEventLogWindow();
	~GameEventLogWindow();

	static void updateSettings();
	static void destroy();

	// Thread-safe logging function called from emulation thread
	static void logCommand(u16 cmdType, u16 param2, u32 param3, const char* name);

protected:
	void closeEvent(QCloseEvent* event) override;

private Q_SLOTS:
	void onClearTriggered();
	void onSaveTriggered();
	void appendCommand(quint32 cmdType, quint32 param2, quint32 param3, const QString& name);

private:
	static constexpr int DEFAULT_WIDTH = 600;
	static constexpr int DEFAULT_HEIGHT = 400;

	void createUi();
	void saveSize();
	void restoreSize();

	static const char* getCommandName(u16 cmdType);
	static void initHooks();
	static void shutdownHooks();

	QPlainTextEdit* m_text;
	bool m_destroying = false;
};

extern GameEventLogWindow* g_game_event_log_window;
