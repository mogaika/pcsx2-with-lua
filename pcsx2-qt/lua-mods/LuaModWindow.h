// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include "LuaEngine.h"
#include "MemTracker.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSlider>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTextEdit>

class QVBoxLayout;

class LuaModWindow : public QMainWindow
{
	Q_OBJECT

public:
	LuaModWindow(QWidget* parent = nullptr);
	~LuaModWindow();

	static void updateSettings();
	static void destroy();

protected:
	void closeEvent(QCloseEvent* event) override;

private Q_SLOTS:
	void onClearTriggered();
	void onSaveTriggered();
	void appendMessage(const QString& message);
	void appendLuaLog(int level, const QString& system, const QString& message);

	// Log filter
	void onLogFilterChanged();

	// Memory tab slots
	void onMemSnapshotTriggered();
	void onMemFilterChanged();
	void onMemTableSelectionChanged();
	void onMemFlushTracesTriggered();
	void onMemClearTracesTriggered();

	// Lua script slots
	void onLuaConsoleSubmit();

	// Commands tab slots
	void onWidgetsChanged();

	// Settings tab slots
	void onAddDirClicked();
	void onRemoveDirClicked();

private:
	static constexpr int DEFAULT_WIDTH = 800;
	static constexpr int DEFAULT_HEIGHT = 600;
	static constexpr int MAX_LOG_BUFFER = 2000;
	static constexpr int LOG_SYSTEM = 3;

	void createUi();
	void createMemoryTab();
	void createCommandsTab();
	void rebuildCommandsTab();
	void createSettingsTab();
	void refreshDirList();
	void saveSize();
	void restoreSize();
	void refreshMemoryTable();
	void rebuildLogDisplay();
	static QString formatTraceStats(const std::vector<TraceEntry>& stats);

	QPlainTextEdit* m_text = nullptr;
	QTabWidget* m_tabWidget = nullptr;
	bool m_destroying = false;

	// Lua Logs tab filter checkboxes + system filter
	QCheckBox* m_logFilterInfo = nullptr;
	QCheckBox* m_logFilterWarn = nullptr;
	QCheckBox* m_logFilterError = nullptr;
	QLineEdit* m_logSystemFilter = nullptr;

	// Log ring buffer for re-filtering
	struct LogEntry { int level; QString system; QString text; };
	QList<LogEntry> m_logBuffer;

	// Memory tab widgets
	QTableWidget* m_memTable = nullptr;
	QTextEdit* m_memDetail = nullptr;
	QLineEdit* m_memFilterAddr = nullptr;
	QLineEdit* m_memFilterParent = nullptr;
	QLineEdit* m_memFilterTag = nullptr;
	QLineEdit* m_memFilterStack = nullptr;

	// Memory snapshot data
	std::map<u32, TrackedRegion> m_memSnapshot;

	// Lua UI widgets
	QLineEdit* m_luaConsole = nullptr;

	// Widgets tab
	QScrollArea* m_commandsScrollArea = nullptr;
	QWidget* m_commandsContainer = nullptr;
	QVBoxLayout* m_commandsLayout = nullptr;

	// Settings tab widgets
	QListWidget* m_dirList = nullptr;
	QPushButton* m_removeDirBtn = nullptr;
};

extern LuaModWindow* g_lua_mod_window;
