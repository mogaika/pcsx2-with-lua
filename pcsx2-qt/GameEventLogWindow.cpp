// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GameEventLogWindow.h"
#include "MainWindow.h"
#include "QtHost.h"

#include "DebugTools/MemoryTrace.h"
#include "Memory.h"
#include "R5900.h"
#include "x86/iR5900.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtCore/QUtf8StringView>
#include <QtGui/QIcon>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

GameEventLogWindow* g_game_event_log_window = nullptr;
static std::mutex s_game_event_log_mutex;

// Static member definitions for WAD injection
std::map<u32, std::pair<QFile*, QString>> GameEventLogWindow::s_injectedFiles;
QString GameEventLogWindow::s_customWadDirectory;
bool GameEventLogWindow::s_fileReadLogsEnabled = false;

void GameEventLogWindow::setCustomWadDirectory(const QString& dir)
{
	s_customWadDirectory = dir;
}

QString GameEventLogWindow::customWadDirectory()
{
	return s_customWadDirectory;
}

bool GameEventLogWindow::isFileReadLogsEnabled()
{
	return s_fileReadLogsEnabled;
}

void GameEventLogWindow::setFileReadLogsEnabled(bool enabled)
{
	s_fileReadLogsEnabled = enabled;
}

void GameEventLogWindow::logInjectionMessage(const QString& message)
{
	std::unique_lock lock(s_game_event_log_mutex);
	if (!g_game_event_log_window)
		return;

	if (g_emu_thread->isOnUIThread())
	{
		g_game_event_log_window->appendMessage(message);
	}
	else
	{
		QMetaObject::invokeMethod(g_game_event_log_window, "appendMessage", Qt::QueuedConnection,
			Q_ARG(const QString&, message));
	}
}

// Hook callback for GameWadLoader::AddCommand at 0x001bb0f8
static void hookWadEventAdded()
{
	u16 cmdType = cpuRegs.GPR.n.a0.US[0];
	u16 param2 = cpuRegs.GPR.n.a1.US[0];
	u32 param3 = cpuRegs.GPR.n.a2.UL[0];

	const char* name = nullptr;
	if (cmdType == 3 || cmdType == 9)
	{
		u32 namePtr = cpuRegs.GPR.n.a3.UL[0];
		if (namePtr != 0)
			name = (const char*)PSM(namePtr);
	}

	GameEventLogWindow::logCommand(cmdType, param2, param3, name);
}

// Hook ID for memory tracing
static u32 s_clientParmHookId = 0;

// Callback for hook results (called on write detection)
static void onClientParmTraceResult(const HookTrace& trace)
{
	if (trace.firstWritePC != 0)
	{
		GameEventLogWindow::logInjectionMessage(
			QString("[TRACE] Overwritten at PC=0x%1\n").arg(trace.firstWritePC, 8, 16, QChar('0')));
	}
}

// Hook for svrClientParm::IFFProcessClientParm at 0x01789e0
static void hookIFFProcessClientParm()
{
	u32 headerPtr = cpuRegs.GPR.n.a0.UL[0];
	u32 dataPtr = cpuRegs.GPR.n.a1.UL[0];

	if (dataPtr == 0 || s_clientParmHookId == 0)
		return;

	u32 magic = *(u32*)PSM(dataPtr);
	if (magic != 0x00020001)
		return;

	u32 traceStart = dataPtr + 0x00;
	u32 traceSize = 0x5c;

	MemoryTraceManager::Instance().AddTracedRange(s_clientParmHookId, traceStart, traceSize);
}

// Hook at 0x001342f8 to trace v0 register range [v0+0x8, v0+0x44)
static void hookTraceV0()
{
	if (s_clientParmHookId == 0)
		return;

	u32 v0 = cpuRegs.GPR.n.v0.UL[0];
	if (v0 == 0)
		return;

	u32 traceStart = v0 + 0x8;
	u32 traceSize = 0x44 - 0x8; // 0x3c bytes

	MemoryTraceManager::Instance().AddTracedRange(s_clientParmHookId, traceStart, traceSize);
}

// Hook for sysFile::OpenResource at 0x17ad70
static void hookSysFileOpenResource()
{
	u32 thisPtr = cpuRegs.GPR.n.a0.UL[0];
	u32 filenamePtr = cpuRegs.GPR.n.a1.UL[0];
	u32 mode = cpuRegs.GPR.n.a2.UL[0];

	const char* filename = nullptr;
	if (filenamePtr != 0)
		filename = (const char*)PSM(filenamePtr);

	if (filename && !GameEventLogWindow::s_customWadDirectory.isEmpty())
	{
		QString qFilename = QString::fromLatin1(filename);
		QString targetName = qFilename + QStringLiteral(".WAD");

		QDir customDir(GameEventLogWindow::s_customWadDirectory);
		QString foundPath;

		QString exactPath = customDir.filePath(targetName);
		if (QFileInfo::exists(exactPath))
		{
			foundPath = exactPath;
		}
		else
		{
			QStringList entries = customDir.entryList(QDir::Files);
			for (const QString& entry : entries)
			{
				if (entry.compare(targetName, Qt::CaseInsensitive) == 0)
				{
					foundPath = customDir.filePath(entry);
					break;
				}
			}
		}

		if (!foundPath.isEmpty())
		{
			QFile* file = new QFile(foundPath);
			if (file->open(QIODevice::ReadOnly))
			{
				u32* sysFile = (u32*)PSM(thisPtr);
				sysFile[0] = (u32)GameEventLogWindow::INJECTED_HANDLE_MAGIC;
				sysFile[1] = mode | 0x200;
				sysFile[2] = (u32)file->size();
				sysFile[3] = 0;

				GameEventLogWindow::s_injectedFiles[thisPtr] = {file, qFilename};

				GameEventLogWindow::logInjectionMessage(
					QStringLiteral("[WAD_INJECT] Opened: %1 (%2 bytes)\n")
						.arg(foundPath)
						.arg(file->size()));

				cpuRegs.GPR.n.v0.UL[0] = 1;
				cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
				g_executionHookSkipBlock = true;
				return;
			}
			delete file;
		}
	}

	GameEventLogWindow::logFileOpen(thisPtr, filename, mode);
}

// Hook for sysFile::Read at 0x17afe0
static void hookSysFileRead()
{
	u32 thisPtr = cpuRegs.GPR.n.a0.UL[0];
	u32 bufferPtr = cpuRegs.GPR.n.a1.UL[0];
	u32 amount = cpuRegs.GPR.n.a2.UL[0];

	s32 fHandle = *(s32*)PSM(thisPtr + 0x00);
	u32 fPosition = *(u32*)PSM(thisPtr + 0x0c);

	if (fHandle == GameEventLogWindow::INJECTED_HANDLE_MAGIC)
	{
		auto it = GameEventLogWindow::s_injectedFiles.find(thisPtr);
		if (it != GameEventLogWindow::s_injectedFiles.end())
		{
			QFile* file = it->second.first;
			u32* sysFile = (u32*)PSM(thisPtr);

			u32 fLength = sysFile[2];
			u32 fPos = sysFile[3];

			u32 remaining = fLength - fPos;
			if (amount > remaining)
				amount = remaining;

			u32 bytesRead = 0;
			if (amount > 0)
			{
				file->seek(fPos);
				QByteArray data = file->read(amount);
				bytesRead = (u32)data.size();

				u8* eeBuffer = (u8*)PSM(bufferPtr);
				std::memcpy(eeBuffer, data.data(), bytesRead);

				sysFile[3] = fPos + bytesRead;
			}

			cpuRegs.GPR.n.v0.UL[0] = bytesRead;
			cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
			g_executionHookSkipBlock = true;
			return;
		}
	}

	GameEventLogWindow::logFileRead(thisPtr, (u32)fHandle, bufferPtr, amount, fPosition);
}

// Hook for sysFile::Close at 0x17aee8
static void hookSysFileClose()
{
	u32 thisPtr = cpuRegs.GPR.n.a0.UL[0];
	s32 fHandle = *(s32*)PSM(thisPtr + 0x00);

	if (fHandle == GameEventLogWindow::INJECTED_HANDLE_MAGIC)
	{
		auto it = GameEventLogWindow::s_injectedFiles.find(thisPtr);
		if (it != GameEventLogWindow::s_injectedFiles.end())
		{
			QString filename = it->second.second;
			delete it->second.first;
			GameEventLogWindow::s_injectedFiles.erase(it);

			*(s32*)PSM(thisPtr + 0x00) = -1;

			GameEventLogWindow::logInjectionMessage(
				QStringLiteral("[WAD_INJECT] Closed: %1\n").arg(filename));

			cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
			g_executionHookSkipBlock = true;
			return;
		}
	}

	GameEventLogWindow::logFileClose(thisPtr, (u32)fHandle);
}

// Hook for sysFile::IsReadDone at 0x17af78
static void hookSysFileIsReadDone()
{
	u32 thisPtr = cpuRegs.GPR.n.a0.UL[0];
	s32 fHandle = *(s32*)PSM(thisPtr + 0x00);

	if (fHandle == GameEventLogWindow::INJECTED_HANDLE_MAGIC)
	{
		cpuRegs.GPR.n.v0.UL[0] = 1;
		cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
		g_executionHookSkipBlock = true;
		return;
	}
}

// Hook for wadLoader::ProcessWadFile at 0x185f28
static void hookWadLoaderProcessWadFile()
{
	u32 wadNamePtr = cpuRegs.GPR.n.a1.UL[0];

	const char* wadName = nullptr;
	if (wadNamePtr != 0)
		wadName = (const char*)PSM(wadNamePtr);

	GameEventLogWindow::logWadProcess(wadName);
}

GameEventLogWindow::GameEventLogWindow()
	: QMainWindow()
{
	s_customWadDirectory = QString::fromStdString(
		Host::GetBaseStringSettingValue("GameEventLog", "CustomWadDirectory", ""));

	restoreSize();
	createUi();
	initHooks();

	// Start timer for updating trace stats table
	m_updateTimer = new QTimer(this);
	connect(m_updateTimer, &QTimer::timeout, this, &GameEventLogWindow::updateTraceTable);
	m_updateTimer->start(500);  // Update every 500ms

	if (!s_customWadDirectory.isEmpty())
	{
		appendMessage(QStringLiteral("[CONFIG] Custom WAD directory: %1\n").arg(s_customWadDirectory));
	}
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

static QString decodeModeFlags(u32 mode)
{
	QStringList flags;
	if (mode & 0x80000000) flags << "HOST";
	if (mode & 0x10000000) flags << "SYNC";
	if (mode & 0x00000200) flags << "SYNC2";
	if (mode & 0x00000020) flags << "MOVIE";
	if (mode & 0x00000010) flags << "WAD";
	u32 streamType = mode & 0x0f;
	if (streamType == 0) flags << "type:WAD";
	else if (streamType == 1) flags << "type:VAG";
	else flags << QStringLiteral("type:%1").arg(streamType);
	return flags.join("|");
}

void GameEventLogWindow::logFileOpen(u32 sysFilePtr, const char* filename, u32 mode)
{
	std::unique_lock lock(s_game_event_log_mutex);
	if (!g_game_event_log_window)
		return;

	QString qname = filename ? QString::fromUtf8(filename) : QStringLiteral("(null)");
	QString modeStr = decodeModeFlags(mode);
	QString msg = QStringLiteral("[FILE_OPEN] sysFile=0x%1 name=%2 mode=0x%3 (%4)\n")
		.arg(sysFilePtr, 8, 16, QChar('0'))
		.arg(qname)
		.arg(mode, 8, 16, QChar('0'))
		.arg(modeStr);

	if (g_emu_thread->isOnUIThread())
	{
		g_game_event_log_window->appendMessage(msg);
	}
	else
	{
		QMetaObject::invokeMethod(g_game_event_log_window, "appendMessage", Qt::QueuedConnection,
			Q_ARG(const QString&, msg));
	}
}

void GameEventLogWindow::logFileRead(u32 sysFilePtr, u32 handle, u32 buffer, u32 amount, u32 position)
{
	std::unique_lock lock(s_game_event_log_mutex);
	if (!g_game_event_log_window)
		return;

	if (!s_fileReadLogsEnabled)
		return;

	QString msg = QStringLiteral("[FILE_READ] sysFile=0x%1 handle=%2 buffer=0x%3 amount=%4 pos=%5\n")
		.arg(sysFilePtr, 8, 16, QChar('0'))
		.arg(handle)
		.arg(buffer, 8, 16, QChar('0'))
		.arg(amount)
		.arg(position);

	if (g_emu_thread->isOnUIThread())
	{
		g_game_event_log_window->appendMessage(msg);
	}
	else
	{
		QMetaObject::invokeMethod(g_game_event_log_window, "appendMessage", Qt::QueuedConnection,
			Q_ARG(const QString&, msg));
	}
}

void GameEventLogWindow::logFileClose(u32 sysFilePtr, u32 handle)
{
	std::unique_lock lock(s_game_event_log_mutex);
	if (!g_game_event_log_window)
		return;

	if (handle == 0xFFFFFFFF)
		return;

	QString msg = QStringLiteral("[FILE_CLOSE] sysFile=0x%1 handle=%2\n")
		.arg(sysFilePtr, 8, 16, QChar('0'))
		.arg(handle);

	if (g_emu_thread->isOnUIThread())
	{
		g_game_event_log_window->appendMessage(msg);
	}
	else
	{
		QMetaObject::invokeMethod(g_game_event_log_window, "appendMessage", Qt::QueuedConnection,
			Q_ARG(const QString&, msg));
	}
}

void GameEventLogWindow::logWadProcess(const char* wadName)
{
	std::unique_lock lock(s_game_event_log_mutex);
	if (!g_game_event_log_window)
		return;

	QString qname = wadName ? QString::fromUtf8(wadName) : QStringLiteral("(null)");
	QString msg = QStringLiteral("[WAD_PROCESS] name=%1\n").arg(qname);

	if (g_emu_thread->isOnUIThread())
	{
		g_game_event_log_window->appendMessage(msg);
	}
	else
	{
		QMetaObject::invokeMethod(g_game_event_log_window, "appendMessage", Qt::QueuedConnection,
			Q_ARG(const QString&, msg));
	}
}

void GameEventLogWindow::closeEvent(QCloseEvent* event)
{
	saveSize();
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

void GameEventLogWindow::onSetWadDirectoryTriggered()
{
	QString dir = QFileDialog::getExistingDirectory(this,
		tr("Select Custom WAD Directory"),
		s_customWadDirectory);

	if (!dir.isEmpty())
	{
		s_customWadDirectory = dir;
		Host::SetBaseStringSettingValue("GameEventLog", "CustomWadDirectory",
			dir.toStdString().c_str());
		Host::CommitBaseSettingChanges();

		appendMessage(QStringLiteral("[CONFIG] Custom WAD directory: %1\n").arg(dir));
	}
}

void GameEventLogWindow::updateTraceTable()
{
	if (s_clientParmHookId == 0)
		return;

	auto stats = MemoryTraceManager::Instance().GetHookTraceStats(s_clientParmHookId);

	// Sort by count (descending)
	std::vector<std::pair<TraceKey, u32>> sorted(stats.begin(), stats.end());
	std::sort(sorted.begin(), sorted.end(),
		[](const auto& a, const auto& b) { return a.second > b.second; });

	m_traceTable->setRowCount(static_cast<int>(sorted.size()));

	int row = 0;
	for (const auto& [key, count] : sorted)
	{
		// Offset column
		m_traceTable->setItem(row, 0,
			new QTableWidgetItem(QString("0x%1").arg(key.offset, 4, 16, QChar('0'))));

		// Count column
		m_traceTable->setItem(row, 1,
			new QTableWidgetItem(QString::number(count)));

		// Stack columns (4 PCs)
		for (int i = 0; i < 4; i++)
		{
			QString pcStr = key.stack.pcs[i] != 0
				? QString("0x%1").arg(key.stack.pcs[i], 8, 16, QChar('0'))
				: QString();
			m_traceTable->setItem(row, 2 + i, new QTableWidgetItem(pcStr));
		}

		row++;
	}
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

void GameEventLogWindow::appendMessage(const QString& message)
{
	QTextCursor temp_cursor = m_text->textCursor();
	QScrollBar* scrollbar = m_text->verticalScrollBar();
	const bool cursor_at_end = temp_cursor.atEnd();
	const bool scroll_at_end = scrollbar->sliderPosition() == scrollbar->maximum();

	temp_cursor.movePosition(QTextCursor::End);
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

	QMenu* inject_menu = menu->addMenu(tr("&Inject"));
	action = inject_menu->addAction(tr("Set Custom WAD &Directory..."));
	connect(action, &QAction::triggered, this, &GameEventLogWindow::onSetWadDirectoryTriggered);

	QMenu* view_menu = menu->addMenu(tr("&View"));
	m_fileReadLogsAction = view_menu->addAction(tr("&File Read Logs"));
	m_fileReadLogsAction->setCheckable(true);
	m_fileReadLogsAction->setChecked(s_fileReadLogsEnabled);
	connect(m_fileReadLogsAction, &QAction::toggled, this, [](bool checked) {
		s_fileReadLogsEnabled = checked;
	});

	// Create splitter for log and trace table
	QSplitter* splitter = new QSplitter(Qt::Vertical, this);

	// Log text area
	m_text = new QPlainTextEdit(this);
	m_text->setReadOnly(true);
	m_text->setUndoRedoEnabled(false);
	m_text->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
	m_text->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
	m_text->setWordWrapMode(QTextOption::WrapAnywhere);

	// Trace stats table
	m_traceTable = new QTableWidget(this);
	m_traceTable->setColumnCount(6);
	m_traceTable->setHorizontalHeaderLabels({tr("Offset"), tr("Count"), tr("PC[0]"), tr("PC[1]"), tr("PC[2]"), tr("PC[3]")});
	m_traceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_traceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_traceTable->horizontalHeader()->setStretchLastSection(true);
	m_traceTable->verticalHeader()->setVisible(false);
	m_traceTable->setSortingEnabled(false);

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
	m_traceTable->setFont(font);

	splitter->addWidget(m_text);
	splitter->addWidget(m_traceTable);
	splitter->setSizes({300, 200});

	setCentralWidget(splitter);
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
	addExecutionHook(0x1BB0F8, hookWadEventAdded);
	addExecutionHook(0x17AD70, hookSysFileOpenResource);
	addExecutionHook(0x17AF78, hookSysFileIsReadDone);
	addExecutionHook(0x17AFE0, hookSysFileRead);
	addExecutionHook(0x17AEE8, hookSysFileClose);
	addExecutionHook(0x185F28, hookWadLoaderProcessWadFile);

	s_clientParmHookId = MemoryTraceManager::Instance().RegisterHook(
		"goServer_LoadClient",
		onClientParmTraceResult,
		MEMTRACE_TRACK_READS | MEMTRACE_STOP_ON_WRITE
	);

	// addExecutionHook(0x01789e0, hookIFFProcessClientParm);  // Disabled
	addExecutionHook(0x001342f8, hookTraceV0);
}

void GameEventLogWindow::shutdownHooks()
{
	removeExecutionHook(0x1BB0F8);
	removeExecutionHook(0x17AD70);
	removeExecutionHook(0x17AF78);
	removeExecutionHook(0x17AFE0);
	removeExecutionHook(0x17AEE8);
	removeExecutionHook(0x185F28);
	// removeExecutionHook(0x01789e0);  // Disabled
	removeExecutionHook(0x001342f8);

	if (s_clientParmHookId != 0)
	{
		MemoryTraceManager::Instance().UnregisterHook(s_clientParmHookId);
		s_clientParmHookId = 0;
	}

	MemoryTraceManager::Instance().ClearAllTraces();
}
