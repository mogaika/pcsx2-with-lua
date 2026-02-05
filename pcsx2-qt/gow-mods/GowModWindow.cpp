// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GowModWindow.h"
#include "GowHooks.h"
#include "GowWadInjector.h"
#include "MainWindow.h"
#include "QtHost.h"

#include "DebugTools/MemoryTrace.h"

#include <QtCore/QTimer>
#include <QtGui/QIcon>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <mutex>
#include <vector>

GowModWindow* g_gow_mod_window = nullptr;
static std::mutex s_gow_mod_mutex;

bool GowModWindow::s_fileReadLogsEnabled = false;

bool GowModWindow::isFileReadLogsEnabled()
{
	return s_fileReadLogsEnabled;
}

void GowModWindow::setFileReadLogsEnabled(bool enabled)
{
	s_fileReadLogsEnabled = enabled;
}

void GowModWindow::logInjectionMessage(const QString& message)
{
	std::unique_lock lock(s_gow_mod_mutex);
	if (!g_gow_mod_window)
		return;

	if (g_emu_thread->isOnUIThread())
	{
		g_gow_mod_window->appendMessage(message);
	}
	else
	{
		QMetaObject::invokeMethod(g_gow_mod_window, "appendMessage", Qt::QueuedConnection,
			Q_ARG(const QString&, message));
	}
}

GowModWindow::GowModWindow()
	: QMainWindow()
{
	GowWadInjector::setCustomWadDirectory(QString::fromStdString(
		Host::GetBaseStringSettingValue("GameEventLog", "CustomWadDirectory", "")));

	restoreSize();
	createUi();
	gowInitHooks();

	// Start timer for updating trace stats table
	m_updateTimer = new QTimer(this);
	connect(m_updateTimer, &QTimer::timeout, this, &GowModWindow::updateTraceTable);
	m_updateTimer->start(500); // Update every 500ms

	QString wadDir = GowWadInjector::customWadDirectory();
	if (!wadDir.isEmpty())
	{
		appendMessage(QStringLiteral("[CONFIG] Custom WAD directory: %1\n").arg(wadDir));
	}
}

GowModWindow::~GowModWindow()
{
	gowShutdownHooks();
}

void GowModWindow::updateSettings()
{
	std::unique_lock lock(s_gow_mod_mutex);

	const bool new_enabled = Host::GetBaseBoolSettingValue("Logging", "EnableGameEventLog", false);
	const bool curr_enabled = (g_gow_mod_window != nullptr);

	if (new_enabled == curr_enabled)
		return;

	if (new_enabled)
	{
		g_gow_mod_window = new GowModWindow();
		g_gow_mod_window->show();
	}
	else if (g_gow_mod_window)
	{
		g_gow_mod_window->m_destroying = true;
		g_gow_mod_window->close();
		g_gow_mod_window->deleteLater();
		g_gow_mod_window = nullptr;
	}
}

void GowModWindow::destroy()
{
	std::unique_lock lock(s_gow_mod_mutex);
	if (!g_gow_mod_window)
		return;

	g_gow_mod_window->m_destroying = true;
	g_gow_mod_window->close();
	g_gow_mod_window->deleteLater();
	g_gow_mod_window = nullptr;
}

void GowModWindow::logCommand(u16 cmdType, u16 param2, u32 param3, const char* name)
{
	std::unique_lock lock(s_gow_mod_mutex);
	if (!g_gow_mod_window)
		return;

	QString qname;
	if (name && name[0])
		qname = QString::fromUtf8(name);
	else
		qname = QStringLiteral("(null)");

	if (g_emu_thread->isOnUIThread())
	{
		g_gow_mod_window->appendCommand(cmdType, param2, param3, qname);
	}
	else
	{
		QMetaObject::invokeMethod(g_gow_mod_window, "appendCommand", Qt::QueuedConnection,
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

void GowModWindow::logFileOpen(u32 sysFilePtr, const char* filename, u32 mode)
{
	std::unique_lock lock(s_gow_mod_mutex);
	if (!g_gow_mod_window)
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
		g_gow_mod_window->appendMessage(msg);
	}
	else
	{
		QMetaObject::invokeMethod(g_gow_mod_window, "appendMessage", Qt::QueuedConnection,
			Q_ARG(const QString&, msg));
	}
}

void GowModWindow::logFileRead(u32 sysFilePtr, u32 handle, u32 buffer, u32 amount, u32 position)
{
	std::unique_lock lock(s_gow_mod_mutex);
	if (!g_gow_mod_window)
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
		g_gow_mod_window->appendMessage(msg);
	}
	else
	{
		QMetaObject::invokeMethod(g_gow_mod_window, "appendMessage", Qt::QueuedConnection,
			Q_ARG(const QString&, msg));
	}
}

void GowModWindow::logFileClose(u32 sysFilePtr, u32 handle)
{
	std::unique_lock lock(s_gow_mod_mutex);
	if (!g_gow_mod_window)
		return;

	if (handle == 0xFFFFFFFF)
		return;

	QString msg = QStringLiteral("[FILE_CLOSE] sysFile=0x%1 handle=%2\n")
		.arg(sysFilePtr, 8, 16, QChar('0'))
		.arg(handle);

	if (g_emu_thread->isOnUIThread())
	{
		g_gow_mod_window->appendMessage(msg);
	}
	else
	{
		QMetaObject::invokeMethod(g_gow_mod_window, "appendMessage", Qt::QueuedConnection,
			Q_ARG(const QString&, msg));
	}
}

void GowModWindow::logWadProcess(const char* wadName)
{
	std::unique_lock lock(s_gow_mod_mutex);
	if (!g_gow_mod_window)
		return;

	QString qname = wadName ? QString::fromUtf8(wadName) : QStringLiteral("(null)");
	QString msg = QStringLiteral("[WAD_PROCESS] name=%1\n").arg(qname);

	if (g_emu_thread->isOnUIThread())
	{
		g_gow_mod_window->appendMessage(msg);
	}
	else
	{
		QMetaObject::invokeMethod(g_gow_mod_window, "appendMessage", Qt::QueuedConnection,
			Q_ARG(const QString&, msg));
	}
}

void GowModWindow::closeEvent(QCloseEvent* event)
{
	saveSize();
	event->ignore();
	hide();
}

void GowModWindow::onClearTriggered()
{
	m_text->clear();
}

void GowModWindow::onSaveTriggered()
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

void GowModWindow::onSetWadDirectoryTriggered()
{
	QString dir = QFileDialog::getExistingDirectory(this,
		tr("Select Custom WAD Directory"),
		GowWadInjector::customWadDirectory());

	if (!dir.isEmpty())
	{
		GowWadInjector::setCustomWadDirectory(dir);
		Host::SetBaseStringSettingValue("GameEventLog", "CustomWadDirectory",
			dir.toStdString().c_str());
		Host::CommitBaseSettingChanges();

		appendMessage(QStringLiteral("[CONFIG] Custom WAD directory: %1\n").arg(dir));
	}
}

void GowModWindow::onLoadLevelTriggered()
{
	QString levelName = m_levelNameEdit->text().trimmed();

	// Strip .WAD suffix if present (case-insensitive)
	if (levelName.endsWith(QStringLiteral(".WAD"), Qt::CaseInsensitive))
		levelName.chop(4);

	// Truncate to 7 chars
	if (levelName.length() > 7)
		levelName.truncate(7);

	if (levelName.isEmpty())
		return;

	gowLoadCustomLevel(levelName);
	appendMessage(QStringLiteral("[LEVEL_LOAD] Loading level: %1\n").arg(levelName));
}

void GowModWindow::updateTraceTable()
{
	if (g_gowTraceHookId == 0)
		return;

	auto stats = MemoryTraceManager::Instance().GetHookTraceStats(g_gowTraceHookId);

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

void GowModWindow::appendCommand(quint32 cmdType, quint32 param2, quint32 param3, const QString& name)
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

void GowModWindow::appendMessage(const QString& message)
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

void GowModWindow::createUi()
{
	setWindowIcon(QtHost::GetAppIcon());
	setWindowTitle(tr("Game Event Log"));

	QAction* action;

	QMenuBar* menu = new QMenuBar(this);
	setMenuBar(menu);

	QMenu* log_menu = menu->addMenu(tr("&Log"));
	action = log_menu->addAction(tr("&Clear"));
	connect(action, &QAction::triggered, this, &GowModWindow::onClearTriggered);
	action = log_menu->addAction(tr("&Save..."));
	connect(action, &QAction::triggered, this, &GowModWindow::onSaveTriggered);

	log_menu->addSeparator();

	action = log_menu->addAction(tr("Cl&ose"));
	connect(action, &QAction::triggered, this, &GowModWindow::close);

	QMenu* inject_menu = menu->addMenu(tr("&Inject"));
	action = inject_menu->addAction(tr("Set Custom WAD &Directory..."));
	connect(action, &QAction::triggered, this, &GowModWindow::onSetWadDirectoryTriggered);

	QMenu* view_menu = menu->addMenu(tr("&View"));
	m_fileReadLogsAction = view_menu->addAction(tr("&File Read Logs"));
	m_fileReadLogsAction->setCheckable(true);
	m_fileReadLogsAction->setChecked(s_fileReadLogsEnabled);
	connect(m_fileReadLogsAction, &QAction::toggled, this, [](bool checked) {
		s_fileReadLogsEnabled = checked;
	});

	// Central container with vertical layout
	QWidget* container = new QWidget(this);
	QVBoxLayout* mainLayout = new QVBoxLayout(container);
	mainLayout->setContentsMargins(0, 0, 0, 0);

	// Level loading bar
	QHBoxLayout* levelBar = new QHBoxLayout();
	m_levelNameEdit = new QLineEdit(this);
	m_levelNameEdit->setText(QStringLiteral("Athn01A"));
	m_levelNameEdit->setPlaceholderText(tr("Level name (e.g. Athn01A)"));
	m_levelNameEdit->setMaxLength(7 + 4); // allow typing ".WAD" which gets stripped
	QPushButton* loadBtn = new QPushButton(tr("Load Level"), this);
	levelBar->addWidget(m_levelNameEdit);
	levelBar->addWidget(loadBtn);
	connect(loadBtn, &QPushButton::clicked, this, &GowModWindow::onLoadLevelTriggered);
	connect(m_levelNameEdit, &QLineEdit::returnPressed, this, &GowModWindow::onLoadLevelTriggered);
	mainLayout->addLayout(levelBar);

	// Tab widget
	m_tabWidget = new QTabWidget(this);

	// Game Events tab
	m_text = new QPlainTextEdit(this);
	m_text->setReadOnly(true);
	m_text->setUndoRedoEnabled(false);
	m_text->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
	m_text->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
	m_text->setWordWrapMode(QTextOption::WrapAnywhere);

	// Memory Tracing tab
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

	m_tabWidget->addTab(m_text, tr("Game Events"));
	m_tabWidget->addTab(m_traceTable, tr("Memory Tracing"));

	mainLayout->addWidget(m_tabWidget);
	setCentralWidget(container);
}

void GowModWindow::saveSize()
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

void GowModWindow::restoreSize()
{
	const int width = Host::GetBaseIntSettingValue("UI", "GameEventLogWindowWidth", DEFAULT_WIDTH);
	const int height = Host::GetBaseIntSettingValue("UI", "GameEventLogWindowHeight", DEFAULT_HEIGHT);
	resize(width, height);
}

const char* GowModWindow::getCommandName(u16 cmdType)
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
