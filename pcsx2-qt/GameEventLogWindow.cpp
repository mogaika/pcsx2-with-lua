// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GameEventLogWindow.h"
#include "MainWindow.h"
#include "QtHost.h"

#include "Memory.h"
#include "R5900.h"
#include "x86/iR5900.h"

#include <QtCore/QUtf8StringView>
#include <QtGui/QIcon>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollBar>

#include <mutex>

GameEventLogWindow* g_game_event_log_window = nullptr;
static std::mutex s_game_event_log_mutex;

// Hook callback for GameWadLoader::AddCommand at 0x001bb0f8
static void hookWadEventAdded()
{
	u16 cmdType = cpuRegs.GPR.n.a0.US[0];      // param_1: command type (short)
	u16 param2 = cpuRegs.GPR.n.a1.US[0];       // param_2
	u32 param3 = cpuRegs.GPR.n.a2.UL[0];       // param_3

	// Only certain commands have a valid name string in a3
	// Load commands (3, 6, 9, 12) use the name, unload commands don't
	const char* name = nullptr;
	if (cmdType == 3 || cmdType == 9) // load scene wad, load slot wad
	{
		u32 namePtr = cpuRegs.GPR.n.a3.UL[0];
		if (namePtr != 0)
			name = (const char*)PSM(namePtr);
	}

	GameEventLogWindow::logCommand(cmdType, param2, param3, name);
}

GameEventLogWindow::GameEventLogWindow()
	: QMainWindow()
{
	restoreSize();
	createUi();
	initHooks();
}

GameEventLogWindow::~GameEventLogWindow()
{
	shutdownHooks();
}

void GameEventLogWindow::updateSettings()
{
	std::unique_lock lock(s_game_event_log_mutex);

	const bool new_enabled = Host::GetBaseBoolSettingValue("Logging", "EnableGameEventLog", false);
	const bool curr_enabled = (g_game_event_log_window != nullptr);

	if (new_enabled == curr_enabled)
		return;

	if (new_enabled)
	{
		g_game_event_log_window = new GameEventLogWindow();
		g_game_event_log_window->show();
	}
	else if (g_game_event_log_window)
	{
		g_game_event_log_window->m_destroying = true;
		g_game_event_log_window->close();
		g_game_event_log_window->deleteLater();
		g_game_event_log_window = nullptr;
	}
}

void GameEventLogWindow::destroy()
{
	std::unique_lock lock(s_game_event_log_mutex);
	if (!g_game_event_log_window)
		return;

	g_game_event_log_window->m_destroying = true;
	g_game_event_log_window->close();
	g_game_event_log_window->deleteLater();
	g_game_event_log_window = nullptr;
}

void GameEventLogWindow::logCommand(u16 cmdType, u16 param2, u32 param3, const char* name)
{
	std::unique_lock lock(s_game_event_log_mutex);
	if (!g_game_event_log_window)
		return;

	QString qname;
	if (name && name[0])
		qname = QString::fromUtf8(name);
	else
		qname = QStringLiteral("(null)");

	if (g_emu_thread->isOnUIThread())
	{
		g_game_event_log_window->appendCommand(cmdType, param2, param3, qname);
	}
	else
	{
		QMetaObject::invokeMethod(g_game_event_log_window, "appendCommand", Qt::QueuedConnection,
			Q_ARG(quint32, cmdType), Q_ARG(quint32, param2), Q_ARG(quint32, param3),
			Q_ARG(const QString&, qname));
	}
}

void GameEventLogWindow::closeEvent(QCloseEvent* event)
{
	saveSize();

	// Just hide the window instead of destroying it
	event->ignore();
	hide();
}

void GameEventLogWindow::onClearTriggered()
{
	m_text->clear();
}

void GameEventLogWindow::onSaveTriggered()
{
	const QString path = QFileDialog::getSaveFileName(this, tr("Select Log File"), QString(), tr("Log Files (*.txt)"));
	if (path.isEmpty())
		return;

	QFile file(path);
	if (!file.open(QFile::WriteOnly | QFile::Text))
	{
		QMessageBox::critical(this, tr("Error"), tr("Failed to open file for writing."));
		return;
	}

	file.write(m_text->toPlainText().toUtf8());
	file.close();
}

void GameEventLogWindow::appendCommand(quint32 cmdType, quint32 param2, quint32 param3, const QString& name)
{
	QTextCursor temp_cursor = m_text->textCursor();
	QScrollBar* scrollbar = m_text->verticalScrollBar();
	const bool cursor_at_end = temp_cursor.atEnd();
	const bool scroll_at_end = scrollbar->sliderPosition() == scrollbar->maximum();

	temp_cursor.movePosition(QTextCursor::End);

	const char* cmdName = getCommandName(static_cast<u16>(cmdType));
	QString message = QStringLiteral("[%1] cmd=%2 (0x%3) p2=%4 p3=0x%5 name=%6\n")
		.arg(cmdName)
		.arg(cmdType)
		.arg(cmdType, 4, 16, QChar('0'))
		.arg(param2)
		.arg(param3, 8, 16, QChar('0'))
		.arg(name);

	temp_cursor.insertText(message);

	if (cursor_at_end)
	{
		if (scroll_at_end)
		{
			m_text->setTextCursor(temp_cursor);
			scrollbar->setSliderPosition(scrollbar->maximum());
		}
		else
		{
			const int pos = scrollbar->sliderPosition();
			m_text->setTextCursor(temp_cursor);
			scrollbar->setSliderPosition(pos);
		}
	}
}

void GameEventLogWindow::createUi()
{
	setWindowIcon(QtHost::GetAppIcon());
	setWindowTitle(tr("Game Event Log"));

	QAction* action;

	QMenuBar* menu = new QMenuBar(this);
	setMenuBar(menu);

	QMenu* log_menu = menu->addMenu(tr("&Log"));
	action = log_menu->addAction(tr("&Clear"));
	connect(action, &QAction::triggered, this, &GameEventLogWindow::onClearTriggered);
	action = log_menu->addAction(tr("&Save..."));
	connect(action, &QAction::triggered, this, &GameEventLogWindow::onSaveTriggered);

	log_menu->addSeparator();

	action = log_menu->addAction(tr("Cl&ose"));
	connect(action, &QAction::triggered, this, &GameEventLogWindow::close);

	m_text = new QPlainTextEdit(this);
	m_text->setReadOnly(true);
	m_text->setUndoRedoEnabled(false);
	m_text->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
	m_text->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
	m_text->setWordWrapMode(QTextOption::WrapAnywhere);

#if defined(_WIN32)
	QFont font("Consolas");
	font.setPointSize(10);
#elif defined(__APPLE__)
	QFont font("Monaco");
	font.setPointSize(11);
#else
	QFont font("Monospace");
	font.setStyleHint(QFont::TypeWriter);
#endif
	m_text->setFont(font);

	setCentralWidget(m_text);
}

void GameEventLogWindow::saveSize()
{
	const int current_width = Host::GetBaseIntSettingValue("UI", "GameEventLogWindowWidth", DEFAULT_WIDTH);
	const int current_height = Host::GetBaseIntSettingValue("UI", "GameEventLogWindowHeight", DEFAULT_HEIGHT);
	const QSize wsize = size();

	bool changed = false;
	if (current_width != wsize.width())
	{
		Host::SetBaseIntSettingValue("UI", "GameEventLogWindowWidth", wsize.width());
		changed = true;
	}
	if (current_height != wsize.height())
	{
		Host::SetBaseIntSettingValue("UI", "GameEventLogWindowHeight", wsize.height());
		changed = true;
	}

	if (changed)
		Host::CommitBaseSettingChanges();
}

void GameEventLogWindow::restoreSize()
{
	const int width = Host::GetBaseIntSettingValue("UI", "GameEventLogWindowWidth", DEFAULT_WIDTH);
	const int height = Host::GetBaseIntSettingValue("UI", "GameEventLogWindowHeight", DEFAULT_HEIGHT);
	resize(width, height);
}

const char* GameEventLogWindow::getCommandName(u16 cmdType)
{
	switch (cmdType)
	{
		case 1: return "unload scene wad";
		case 3: return "load scene wad";
		case 5: return "unload hero wad";
		case 6: return "load hero wad";
		case 8: return "unload slot wad";
		case 9: return "load slot wad";
		case 0xb: return "unload weapon/skill wad";
		case 0xc: return "load weapon/skill wad";
		default: return "unknown";
	}
}

void GameEventLogWindow::initHooks()
{
	// Register hook for GameWadLoader::AddCommand at 0x001bb0f8
	addExecutionHook(0x1BB0F8, hookWadEventAdded);
}

void GameEventLogWindow::shutdownHooks()
{
	// Remove the hook when window is destroyed
	removeExecutionHook(0x1BB0F8);
}
