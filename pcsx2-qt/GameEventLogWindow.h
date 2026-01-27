// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <QtCore/QFile>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTableWidget>

#include <map>
#include <utility>

class QTimer;

class GameEventLogWindow : public QMainWindow
{
	Q_OBJECT

public:
	GameEventLogWindow();
	~GameEventLogWindow();

	static void updateSettings();
	static void destroy();

	// Thread-safe logging functions called from emulation thread
	static void logCommand(u16 cmdType, u16 param2, u32 param3, const char* name);
	static void logFileOpen(u32 sysFilePtr, const char* filename, u32 mode);
	static void logFileRead(u32 sysFilePtr, u32 handle, u32 buffer, u32 amount, u32 position);
	static void logFileClose(u32 sysFilePtr, u32 handle);
	static void logWadProcess(const char* wadName);

	// Log filtering
	static bool isFileReadLogsEnabled();
	static void setFileReadLogsEnabled(bool enabled);

	// WAD injection
	static void setCustomWadDirectory(const QString& dir);
	static QString customWadDirectory();
	static void logInjectionMessage(const QString& message);

	// Magic handle value to identify injected files
	static constexpr s32 INJECTED_HANDLE_MAGIC = -100;

	// Map: sysFile EE pointer -> (host QFile*, filename)
	static std::map<u32, std::pair<QFile*, QString>> s_injectedFiles;
	static QString s_customWadDirectory;

protected:
	void closeEvent(QCloseEvent* event) override;

private Q_SLOTS:
	void onClearTriggered();
	void onSaveTriggered();
	void onSetWadDirectoryTriggered();
	void updateTraceTable();
	void appendCommand(quint32 cmdType, quint32 param2, quint32 param3, const QString& name);
	void appendMessage(const QString& message);

private:
	static constexpr int DEFAULT_WIDTH = 800;
	static constexpr int DEFAULT_HEIGHT = 600;

	void createUi();
	void saveSize();
	void restoreSize();

	static const char* getCommandName(u16 cmdType);
	static void initHooks();
	static void shutdownHooks();

	QPlainTextEdit* m_text = nullptr;
	QTableWidget* m_traceTable = nullptr;
	QTimer* m_updateTimer = nullptr;
	QAction* m_fileReadLogsAction = nullptr;
	bool m_destroying = false;

	static bool s_fileReadLogsEnabled;
};

extern GameEventLogWindow* g_game_event_log_window;
