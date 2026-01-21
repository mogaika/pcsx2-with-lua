// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "LuaModWindow.h"
#include "LuaHooks.h"
#include "MemTracker.h"
#include "MainWindow.h"
#include "QtHost.h"
#include "VMManager.h"

#include "Config.h"
#include "Host.h"
#include "common/Path.h"

#include <QtGui/QIcon>
#include <QtCore/QDir>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>

#include <mutex>

// Returns true if str matches pattern with * wildcards at start/end.
// "*" matches anything, "pre*" prefix, "*suf" suffix, "*mid*" contains, else exact.
static bool matchesPattern(const std::string& str, const std::string& pattern)
{
	if (pattern == "*")
		return true;
	bool startsWild = !pattern.empty() && pattern.front() == '*';
	bool endsWild = pattern.size() > 1 && pattern.back() == '*';
	std::string core = pattern;
	if (startsWild)
		core = core.substr(1);
	if (endsWild && !core.empty())
		core.pop_back();
	if (core.empty())
		return true; // was just "*" or "**"
	if (startsWild && endsWild)
		return str.find(core) != std::string::npos;
	if (startsWild)
		return str.size() >= core.size() && str.compare(str.size() - core.size(), core.size(), core) == 0;
	if (endsWild)
		return str.compare(0, core.size(), core) == 0;
	return str == core;
}

LuaModWindow* g_lua_mod_window = nullptr;
static std::mutex s_lua_mod_mutex;

static QString formatLogPrefix(int level)
{
	switch (level)
	{
		case LuaEngine::LOG_INFO:  return QStringLiteral("[INFO] ");
		case LuaEngine::LOG_WARN:  return QStringLiteral("[WARN] ");
		case LuaEngine::LOG_ERROR: return QStringLiteral("[ERROR] ");
		default: return {};
	}
}

LuaModWindow::LuaModWindow(QWidget* parent)
	: QMainWindow(parent)
{
	// Route Lua log messages to the Lua Logs tab
	LuaEngine::instance().setLogCallback([this](int level, const std::string& system, const std::string& msg) {
		QString qsys = QString::fromStdString(system);
		QString qmsg = QString::fromStdString(msg);
		QMetaObject::invokeMethod(this, "appendLuaLog", Qt::QueuedConnection,
			Q_ARG(int, level),
			Q_ARG(const QString&, qsys),
			Q_ARG(const QString&, qmsg));
	});

	// Notify UI when Lua widgets change (register/cleanup)
	LuaEngine::instance().setWidgetsChangedCallback([this]() {
		QMetaObject::invokeMethod(this, "onWidgetsChanged", Qt::QueuedConnection);
	});

	restoreSize();
	createUi();
	LuaHooks::init();
}

LuaModWindow::~LuaModWindow()
{
	LuaEngine::instance().setWidgetsChangedCallback(nullptr);
	LuaEngine::instance().setLogCallback(nullptr);
	LuaHooks::shutdown();
}

void LuaModWindow::updateSettings()
{
	std::unique_lock lock(s_lua_mod_mutex);

	const bool new_enabled = Host::GetBaseBoolSettingValue("Logging", "EnableLuaMods", false);
	const bool curr_enabled = (g_lua_mod_window != nullptr);

	if (new_enabled == curr_enabled)
		return;

	if (new_enabled)
	{
		g_lua_mod_window = new LuaModWindow();
		g_lua_mod_window->show();
	}
	else if (g_lua_mod_window)
	{
		g_lua_mod_window->m_destroying = true;
		g_lua_mod_window->close();
		g_lua_mod_window->deleteLater();
		g_lua_mod_window = nullptr;
	}
}

void LuaModWindow::destroy()
{
	std::unique_lock lock(s_lua_mod_mutex);
	if (!g_lua_mod_window)
		return;

	g_lua_mod_window->m_destroying = true;
	g_lua_mod_window->close();
	g_lua_mod_window->deleteLater();
	g_lua_mod_window = nullptr;
}

void LuaModWindow::closeEvent(QCloseEvent* event)
{
	saveSize();
	if (m_destroying)
	{
		event->accept();
		return;
	}
	event->ignore();
	hide();
}

void LuaModWindow::onClearTriggered()
{
	m_logBuffer.clear();
	m_text->clear();
}

void LuaModWindow::onSaveTriggered()
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

void LuaModWindow::appendMessage(const QString& message)
{
	// Store in ring buffer for filter rebuild
	if (m_logBuffer.size() >= MAX_LOG_BUFFER)
		m_logBuffer.removeFirst();
	m_logBuffer.append({LOG_SYSTEM, QString(), message.chopped(message.endsWith('\n') ? 1 : 0)});

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

void LuaModWindow::appendLuaLog(int level, const QString& system, const QString& message)
{
	// Add to ring buffer (bounded)
	if (m_logBuffer.size() >= MAX_LOG_BUFFER)
		m_logBuffer.removeFirst();
	m_logBuffer.append({level, system, message});

	// Check if this level is currently visible
	bool visible = false;
	switch (level)
	{
		case LuaEngine::LOG_INFO:  visible = m_logFilterInfo  && m_logFilterInfo->isChecked();  break;
		case LuaEngine::LOG_WARN:  visible = m_logFilterWarn  && m_logFilterWarn->isChecked();  break;
		case LuaEngine::LOG_ERROR: visible = m_logFilterError && m_logFilterError->isChecked(); break;
		default: visible = true; break;
	}

	if (!visible)
		return;

	// Apply system filter (substring match)
	if (m_logSystemFilter && !m_logSystemFilter->text().isEmpty())
	{
		if (!system.contains(m_logSystemFilter->text(), Qt::CaseInsensitive))
			return;
	}

	// Append directly to display — don't call appendMessage() which would double-add to buffer
	QString text = formatLogPrefix(level) + QStringLiteral("[") + system + QStringLiteral("] ") + message + QStringLiteral("\n");

	QTextCursor temp_cursor = m_text->textCursor();
	QScrollBar* scrollbar = m_text->verticalScrollBar();
	const bool cursor_at_end = temp_cursor.atEnd();
	const bool scroll_at_end = scrollbar->sliderPosition() == scrollbar->maximum();

	temp_cursor.movePosition(QTextCursor::End);
	temp_cursor.insertText(text);

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

void LuaModWindow::onLogFilterChanged()
{
	rebuildLogDisplay();
}

void LuaModWindow::rebuildLogDisplay()
{
	m_text->clear();

	bool showInfo  = m_logFilterInfo  && m_logFilterInfo->isChecked();
	bool showWarn  = m_logFilterWarn  && m_logFilterWarn->isChecked();
	bool showError = m_logFilterError && m_logFilterError->isChecked();
	QString sysFilter = m_logSystemFilter ? m_logSystemFilter->text() : QString();

	for (const auto& entry : m_logBuffer)
	{
		bool show = false;
		switch (entry.level)
		{
			case LuaEngine::LOG_INFO:  show = showInfo;  break;
			case LuaEngine::LOG_WARN:  show = showWarn;  break;
			case LuaEngine::LOG_ERROR: show = showError; break;
			default: show = true; break;
		}
		if (!show)
			continue;

		if (!sysFilter.isEmpty() && entry.level != LOG_SYSTEM && !entry.system.contains(sysFilter, Qt::CaseInsensitive))
			continue;

		QTextCursor c = m_text->textCursor();
		c.movePosition(QTextCursor::End);

		if (entry.level == LOG_SYSTEM)
		{
			c.insertText(entry.text + QStringLiteral("\n"));
		}
		else
		{
			c.insertText(formatLogPrefix(entry.level) + QStringLiteral("[") + entry.system + QStringLiteral("] ") + entry.text + QStringLiteral("\n"));
		}
	}

	// Scroll to end
	QScrollBar* scrollbar = m_text->verticalScrollBar();
	scrollbar->setSliderPosition(scrollbar->maximum());
}

void LuaModWindow::createUi()
{
	setWindowIcon(QtHost::GetAppIcon());
	setWindowTitle(tr("Lua Mods"));

	QAction* action;

	QMenuBar* menu = new QMenuBar(this);
	setMenuBar(menu);

	QMenu* log_menu = menu->addMenu(tr("&Log"));
	action = log_menu->addAction(tr("&Clear"));
	connect(action, &QAction::triggered, this, &LuaModWindow::onClearTriggered);
	action = log_menu->addAction(tr("&Save..."));
	connect(action, &QAction::triggered, this, &LuaModWindow::onSaveTriggered);

	log_menu->addSeparator();

	action = log_menu->addAction(tr("Cl&ose"));
	connect(action, &QAction::triggered, this, &LuaModWindow::close);

	// Central container with vertical layout
	QWidget* container = new QWidget(this);
	QVBoxLayout* mainLayout = new QVBoxLayout(container);
	mainLayout->setContentsMargins(0, 0, 0, 0);

	// Tab widget
	m_tabWidget = new QTabWidget(this);

	// Lua Logs tab
	{
		QWidget* logWidget = new QWidget(this);
		QVBoxLayout* logLayout = new QVBoxLayout(logWidget);
		logLayout->setContentsMargins(0, 0, 0, 0);

		// Filter bar
		QHBoxLayout* filterBar = new QHBoxLayout();
		filterBar->addWidget(new QLabel(tr("Filter:"), this));

		m_logFilterInfo = new QCheckBox(tr("INFO"), this);
		m_logFilterInfo->setChecked(true);
		connect(m_logFilterInfo, &QCheckBox::toggled, this, &LuaModWindow::onLogFilterChanged);
		filterBar->addWidget(m_logFilterInfo);

		m_logFilterWarn = new QCheckBox(tr("WARN"), this);
		m_logFilterWarn->setChecked(true);
		connect(m_logFilterWarn, &QCheckBox::toggled, this, &LuaModWindow::onLogFilterChanged);
		filterBar->addWidget(m_logFilterWarn);

		m_logFilterError = new QCheckBox(tr("ERROR"), this);
		m_logFilterError->setChecked(true);
		connect(m_logFilterError, &QCheckBox::toggled, this, &LuaModWindow::onLogFilterChanged);
		filterBar->addWidget(m_logFilterError);

		filterBar->addWidget(new QLabel(tr("System:"), this));
		m_logSystemFilter = new QLineEdit(this);
		m_logSystemFilter->setPlaceholderText(tr("filter by system..."));
		m_logSystemFilter->setMaximumWidth(150);
		connect(m_logSystemFilter, &QLineEdit::textChanged, this, &LuaModWindow::onLogFilterChanged);
		filterBar->addWidget(m_logSystemFilter);

		filterBar->addStretch();
		logLayout->addLayout(filterBar);

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
		logLayout->addWidget(m_text);

		m_tabWidget->addTab(logWidget, tr("Lua Logs"));
	}

	createMemoryTab();
	createCommandsTab();
	createSettingsTab();

	mainLayout->addWidget(m_tabWidget);

	m_luaConsole = new QLineEdit(this);
	m_luaConsole->setPlaceholderText(tr("Lua console (press Enter to execute)"));
	connect(m_luaConsole, &QLineEdit::returnPressed, this, &LuaModWindow::onLuaConsoleSubmit);
	mainLayout->addWidget(m_luaConsole);

	setCentralWidget(container);
}

void LuaModWindow::createMemoryTab()
{
	QWidget* memWidget = new QWidget(this);
	QVBoxLayout* memLayout = new QVBoxLayout(memWidget);

	// Top bar: Snapshot + trace controls
	QHBoxLayout* topBar = new QHBoxLayout();
	QPushButton* snapshotBtn = new QPushButton(tr("Snapshot"), this);
	connect(snapshotBtn, &QPushButton::clicked, this, &LuaModWindow::onMemSnapshotTriggered);
	topBar->addWidget(snapshotBtn);

	QPushButton* flushTracesBtn = new QPushButton(tr("Flush Traces"), this);
	connect(flushTracesBtn, &QPushButton::clicked, this, &LuaModWindow::onMemFlushTracesTriggered);
	topBar->addWidget(flushTracesBtn);

	QPushButton* clearTracesBtn = new QPushButton(tr("Clear Traces"), this);
	connect(clearTracesBtn, &QPushButton::clicked, this, &LuaModWindow::onMemClearTracesTriggered);
	topBar->addWidget(clearTracesBtn);

	topBar->addStretch();
	memLayout->addLayout(topBar);

	// Filter bar
	QHBoxLayout* filterBar = new QHBoxLayout();
	filterBar->addWidget(new QLabel(tr("Addr:"), this));
	m_memFilterAddr = new QLineEdit(this);
	m_memFilterAddr->setPlaceholderText(tr("0x..."));
	m_memFilterAddr->setMaximumWidth(120);
	connect(m_memFilterAddr, &QLineEdit::returnPressed, this, &LuaModWindow::onMemFilterChanged);
	filterBar->addWidget(m_memFilterAddr);

	filterBar->addWidget(new QLabel(tr("Parent:"), this));
	m_memFilterParent = new QLineEdit(this);
	m_memFilterParent->setPlaceholderText(tr("0x... parent addr"));
	m_memFilterParent->setMaximumWidth(120);
	connect(m_memFilterParent, &QLineEdit::returnPressed, this, &LuaModWindow::onMemFilterChanged);
	filterBar->addWidget(m_memFilterParent);

	filterBar->addWidget(new QLabel(tr("Tag:"), this));
	m_memFilterTag = new QLineEdit(this);
	m_memFilterTag->setPlaceholderText(tr("key=val  *=*sub*  !key"));
	m_memFilterTag->setMaximumWidth(140);
	connect(m_memFilterTag, &QLineEdit::returnPressed, this, &LuaModWindow::onMemFilterChanged);
	filterBar->addWidget(m_memFilterTag);

	filterBar->addWidget(new QLabel(tr("Stack:"), this));
	m_memFilterStack = new QLineEdit(this);
	m_memFilterStack->setPlaceholderText(tr("0x... func addr"));
	m_memFilterStack->setMaximumWidth(120);
	connect(m_memFilterStack, &QLineEdit::returnPressed, this, &LuaModWindow::onMemFilterChanged);
	filterBar->addWidget(m_memFilterStack);

	QPushButton* filterBtn = new QPushButton(tr("Filter"), this);
	connect(filterBtn, &QPushButton::clicked, this, &LuaModWindow::onMemFilterChanged);
	filterBar->addWidget(filterBtn);

	filterBar->addStretch();
	memLayout->addLayout(filterBar);

	// Splitter: table on top, detail below
	QSplitter* splitter = new QSplitter(Qt::Vertical, this);

	// Results table
	m_memTable = new QTableWidget(this);
	m_memTable->setColumnCount(5);
	m_memTable->setHorizontalHeaderLabels({tr("Address"), tr("Size"), tr("Parent"), tr("Name/Tags"), tr("Caller")});
	m_memTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_memTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_memTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_memTable->horizontalHeader()->setStretchLastSection(true);
	m_memTable->verticalHeader()->setDefaultSectionSize(20);
	m_memTable->setAlternatingRowColors(true);
	connect(m_memTable, &QTableWidget::itemSelectionChanged, this, &LuaModWindow::onMemTableSelectionChanged);
	splitter->addWidget(m_memTable);

	// Detail panel
	QWidget* detailWidget = new QWidget(this);
	QVBoxLayout* detailLayout = new QVBoxLayout(detailWidget);
	detailLayout->setContentsMargins(0, 0, 0, 0);

	m_memDetail = new QTextEdit(this);
	m_memDetail->setReadOnly(true);
	m_memDetail->setFont(m_text->font());
	detailLayout->addWidget(m_memDetail);

	splitter->addWidget(detailWidget);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 1);

	memLayout->addWidget(splitter);

	m_tabWidget->addTab(memWidget, tr("Memory"));
}

void LuaModWindow::onMemSnapshotTriggered()
{
	m_memSnapshot = MemTracker::instance().snapshotRegions();
	refreshMemoryTable();
	appendMessage(QStringLiteral("[MEM] Snapshot taken: %1 regions\n").arg(m_memSnapshot.size()));
}

void LuaModWindow::onMemFilterChanged()
{
	refreshMemoryTable();
}

void LuaModWindow::refreshMemoryTable()
{
	m_memTable->setRowCount(0);

	// Build query from filter fields
	TrackedRegionQuery query;

	QString addrText = m_memFilterAddr->text().trimmed();
	if (!addrText.isEmpty())
	{
		bool ok = false;
		u32 addr = addrText.toUInt(&ok, 16);
		if (ok)
			query.containsAddress = addr;
	}

	QString parentText = m_memFilterParent->text().trimmed();
	if (!parentText.isEmpty())
	{
		bool ok = false;
		u32 parent = parentText.toUInt(&ok, 16);
		if (ok)
			query.parentAddr = parent;
	}

	// Tag filter modes
	enum class TagFilterMode { None, NoTags, MissingKey, KeyValue, Substring, KeyExists };
	TagFilterMode tagMode = TagFilterMode::None;
	std::string tagKeyPattern, tagValuePattern;

	QString tagText = m_memFilterTag->text().trimmed();
	if (!tagText.isEmpty())
	{
		std::string tagStr = tagText.toStdString();
		if (tagStr == "!")
		{
			tagMode = TagFilterMode::NoTags;
		}
		else if (tagStr.size() > 1 && tagStr[0] == '!')
		{
			tagMode = TagFilterMode::MissingKey;
			tagKeyPattern = tagStr.substr(1);
		}
		else
		{
			int eqPos = tagText.indexOf('=');
			if (eqPos >= 0)
			{
				tagMode = TagFilterMode::KeyValue;
				tagKeyPattern = tagText.left(eqPos).toStdString();
				tagValuePattern = tagText.mid(eqPos + 1).toStdString();
				if (tagKeyPattern.empty())
					tagKeyPattern = "*";
			}
			else if (!tagStr.empty() && tagStr[0] == '*')
			{
				tagMode = TagFilterMode::Substring;
				tagKeyPattern = tagStr.substr(1); // text to search for
			}
			else
			{
				tagMode = TagFilterMode::KeyExists;
				tagKeyPattern = tagStr;
			}
		}
	}

	QString stackText = m_memFilterStack->text().trimmed();
	if (!stackText.isEmpty())
	{
		bool ok = false;
		u32 func = stackText.toUInt(&ok, 16);
		if (ok)
			query.stackContainsFunc = func;
	}

	// Filter snapshot
	std::vector<const TrackedRegion*> results;
	for (const auto& [addr, region] : m_memSnapshot)
	{
		bool match = true;

		if (query.containsAddress)
		{
			u32 target = *query.containsAddress;
			if (target < region.address || target >= region.address + region.size)
				match = false;
		}

		if (query.parentAddr && region.parentAddr != *query.parentAddr)
			match = false;

		if (query.stackContainsFunc && !region.stack.containsAddress(*query.stackContainsFunc))
			match = false;

		switch (tagMode)
		{
			case TagFilterMode::None:
				break;
			case TagFilterMode::NoTags:
				if (!region.tags.empty())
					match = false;
				break;
			case TagFilterMode::MissingKey:
				if (region.tags.find(tagKeyPattern) != region.tags.end())
					match = false;
				break;
			case TagFilterMode::KeyValue:
			{
				bool found = false;
				for (const auto& [k, v] : region.tags)
				{
					if (matchesPattern(k, tagKeyPattern) && matchesPattern(v, tagValuePattern))
					{
						found = true;
						break;
					}
				}
				if (!found)
					match = false;
				break;
			}
			case TagFilterMode::Substring:
			{
				bool found = false;
				if (region.name.find(tagKeyPattern) != std::string::npos)
					found = true;
				if (!found)
				{
					for (const auto& [k, v] : region.tags)
					{
						if (k.find(tagKeyPattern) != std::string::npos || v.find(tagKeyPattern) != std::string::npos)
						{
							found = true;
							break;
						}
					}
				}
				if (!found)
					match = false;
				break;
			}
			case TagFilterMode::KeyExists:
				if (region.tags.find(tagKeyPattern) == region.tags.end())
					match = false;
				break;
		}

		if (match)
			results.push_back(&region);
	}

	// Populate table
	m_memTable->setRowCount(static_cast<int>(results.size()));
	for (int i = 0; i < static_cast<int>(results.size()); i++)
	{
		const TrackedRegion* region = results[i];

		m_memTable->setItem(i, 0, new QTableWidgetItem(QStringLiteral("0x%1").arg(region->address, 8, 16, QChar('0'))));
		m_memTable->setItem(i, 1, new QTableWidgetItem(QStringLiteral("0x%1").arg(region->size, 0, 16)));
		if (region->parentAddr != 0)
			m_memTable->setItem(i, 2, new QTableWidgetItem(QStringLiteral("0x%1").arg(region->parentAddr, 8, 16, QChar('0'))));
		else
			m_memTable->setItem(i, 2, new QTableWidgetItem(QString()));

		// Name + tags combined
		QString nameTagStr = QString::fromStdString(region->name);
		for (const auto& [key, value] : region->tags)
		{
			if (!nameTagStr.isEmpty())
				nameTagStr += QStringLiteral(", ");
			nameTagStr += QString::fromStdString(key) + QStringLiteral("=") + QString::fromStdString(value);
		}
		m_memTable->setItem(i, 3, new QTableWidgetItem(nameTagStr));

		// Caller (first stack frame)
		QString caller;
		if (region->stack.depth > 0)
			caller = QStringLiteral("0x%1").arg(region->stack.frames[0], 8, 16, QChar('0'));
		m_memTable->setItem(i, 4, new QTableWidgetItem(caller));

		// Store address in first column's data for later lookup
		m_memTable->item(i, 0)->setData(Qt::UserRole, region->address);
	}

	m_memTable->resizeColumnsToContents();
}

void LuaModWindow::onMemTableSelectionChanged()
{
	QList<QTableWidgetItem*> selected = m_memTable->selectedItems();
	if (selected.isEmpty())
	{
		m_memDetail->clear();
		return;
	}

	int row = selected.first()->row();
	u32 addr = m_memTable->item(row, 0)->data(Qt::UserRole).toUInt();

	auto it = m_memSnapshot.find(addr);
	if (it == m_memSnapshot.end())
	{
		m_memDetail->clear();
		return;
	}

	const TrackedRegion& region = it->second;
	QString detail = QString::fromStdString(MemTracker::formatRegionText(region));

	// Show live trace stats if this region is being traced
	auto tracedTag = region.tags.find("traced");
	if (tracedTag != region.tags.end())
	{
		auto& tracker = MemTracker::instance();
		detail += QStringLiteral("\n\n=== Active Memory Trace: %1 ===\n")
			.arg(QString::fromStdString(tracedTag->second));

		auto results = tracker.snapshotTraceResults();
		for (const auto& result : results)
		{
			if (result.allocAddress == addr)
			{
				detail += QStringLiteral("  [Flushed] %1 unique access patterns\n")
					.arg(result.stats.size());
				detail += formatTraceStats(result.stats);
			}
		}
	}

	m_memDetail->setPlainText(detail);
}

void LuaModWindow::onMemFlushTracesTriggered()
{
	MemTracker::instance().flushAllTraces();
	auto results = MemTracker::instance().snapshotTraceResults();
	appendMessage(QStringLiteral("[MEM] Flushed all active traces. %1 total results.\n").arg(results.size()));

	if (!results.empty())
	{
		QString detail;
		for (const auto& result : results)
		{
			detail += QStringLiteral("=== Trace: %1 (0x%2, size=0x%3) — %4 access patterns ===\n")
				.arg(QString::fromStdString(result.resourceName))
				.arg(result.allocAddress, 8, 16, QChar('0'))
				.arg(result.allocSize, 0, 16)
				.arg(result.stats.size());
			detail += formatTraceStats(result.stats);
			detail += QStringLiteral("\n");
		}
		m_memDetail->setPlainText(detail);
	}
}

void LuaModWindow::onMemClearTracesTriggered()
{
	MemTracker::instance().clearTraceResults();
	appendMessage(QStringLiteral("[MEM] Cleared trace results.\n"));
}

QString LuaModWindow::formatTraceStats(const std::vector<TraceEntry>& stats)
{
	QString result;
	result += QStringLiteral("  Offset     Count    Stack\n");

	// Already in first-seen order
	for (const auto& entry : stats)
	{
		QString stackStr;
		for (int i = 0; i < 4; i++)
		{
			if (entry.key.stack.pcs[i] == 0)
				break;
			if (!stackStr.isEmpty())
				stackStr += QStringLiteral(" ");
			stackStr += QStringLiteral("0x%1").arg(entry.key.stack.pcs[i], 8, 16, QChar('0'));
		}
		QString prefix = entry.isDma ? QStringLiteral("[DMA] ") : QString();
		result += QStringLiteral("  %1+0x%2  %3  %4\n")
			.arg(prefix)
			.arg(entry.key.offset, 4, 16, QChar('0'))
			.arg(entry.count, -8)
			.arg(stackStr);
	}

	return result;
}

void LuaModWindow::saveSize()
{
	const int current_width = Host::GetBaseIntSettingValue("UI", "LuaModWindowWidth", DEFAULT_WIDTH);
	const int current_height = Host::GetBaseIntSettingValue("UI", "LuaModWindowHeight", DEFAULT_HEIGHT);
	const QSize wsize = size();

	bool changed = false;
	if (current_width != wsize.width())
	{
		Host::SetBaseIntSettingValue("UI", "LuaModWindowWidth", wsize.width());
		changed = true;
	}
	if (current_height != wsize.height())
	{
		Host::SetBaseIntSettingValue("UI", "LuaModWindowHeight", wsize.height());
		changed = true;
	}

	if (changed)
		Host::CommitBaseSettingChanges();
}

void LuaModWindow::restoreSize()
{
	const int width = Host::GetBaseIntSettingValue("UI", "LuaModWindowWidth", DEFAULT_WIDTH);
	const int height = Host::GetBaseIntSettingValue("UI", "LuaModWindowHeight", DEFAULT_HEIGHT);
	resize(width, height);
}

// ============================================================
// Lua script slots
// ============================================================

void LuaModWindow::onLuaConsoleSubmit()
{
	QString code = m_luaConsole->text().trimmed();
	if (code.isEmpty())
		return;

	appendMessage(QStringLiteral("[Lua>] %1\n").arg(code));
	LuaEngine::instance().executeString(code.toStdString());
	m_luaConsole->clear();
}

// ============================================================
// Commands tab
// ============================================================

void LuaModWindow::createCommandsTab()
{
	m_commandsScrollArea = new QScrollArea(this);
	m_commandsScrollArea->setWidgetResizable(true);

	m_commandsContainer = new QWidget(this);
	m_commandsLayout = new QVBoxLayout(m_commandsContainer);
	m_commandsLayout->setAlignment(Qt::AlignTop);

	QLabel* placeholder = new QLabel(tr("No widgets registered. Use ui.widget_*() in a Lua script."), m_commandsContainer);
	placeholder->setAlignment(Qt::AlignCenter);
	placeholder->setStyleSheet(QStringLiteral("color: gray; padding: 20px;"));
	m_commandsLayout->addWidget(placeholder);

	m_commandsScrollArea->setWidget(m_commandsContainer);
	m_tabWidget->addTab(m_commandsScrollArea, tr("Commands"));
}

void LuaModWindow::rebuildCommandsTab()
{
	// Clear existing widgets
	QLayoutItem* item;
	while ((item = m_commandsLayout->takeAt(0)) != nullptr)
	{
		delete item->widget();
		delete item;
	}

	auto widgets = LuaEngine::instance().snapshotWidgets();
	auto groupDescs = LuaEngine::instance().snapshotGroupDescriptions();

	if (widgets.empty())
	{
		QLabel* placeholder = new QLabel(tr("No widgets registered. Use ui.widget_*() in a Lua script."), m_commandsContainer);
		placeholder->setAlignment(Qt::AlignCenter);
		placeholder->setStyleSheet(QStringLiteral("color: gray; padding: 20px;"));
		m_commandsLayout->addWidget(placeholder);
		return;
	}

	// Determine group insertion order (first-seen) and bucket widgets by group
	std::vector<std::string> groupOrder;
	std::map<std::string, std::vector<const LuaEngine::LuaWidget*>> widgetsByGroup;

	for (const auto& w : widgets)
	{
		if (std::find(groupOrder.begin(), groupOrder.end(), w.source) == groupOrder.end())
			groupOrder.push_back(w.source);
		widgetsByGroup[w.source].push_back(&w);
	}

	// Render one QGroupBox per group
	for (const auto& groupName : groupOrder)
	{
		QString title = groupName.empty() ? tr("General") : QString::fromStdString(groupName);
		QGroupBox* box = new QGroupBox(title, m_commandsContainer);
		QVBoxLayout* boxLayout = new QVBoxLayout(box);

		// Group description
		auto descIt = groupDescs.find(groupName);
		if (descIt != groupDescs.end() && !descIt->second.empty())
		{
			QLabel* descLabel = new QLabel(QString::fromStdString(descIt->second), box);
			descLabel->setStyleSheet(QStringLiteral("color: gray; padding-left: 4px; padding-bottom: 4px;"));
			QFont smallFont = descLabel->font();
			smallFont.setPointSizeF(smallFont.pointSizeF() * 0.85);
			descLabel->setFont(smallFont);
			descLabel->setWordWrap(true);
			boxLayout->addWidget(descLabel);
		}

		auto it = widgetsByGroup.find(groupName);
		if (it == widgetsByGroup.end())
			continue;

		for (const auto* wp : it->second)
		{
			QWidget* row = new QWidget(box);
			QHBoxLayout* rowLayout = new QHBoxLayout(row);
			rowLayout->setContentsMargins(4, 2, 4, 2);

			const std::string widgetKey = wp->key;

			switch (wp->type)
			{
				case LuaEngine::LuaWidget::Type::Text:
				{
					QLabel* lbl = new QLabel(QString::fromStdString(wp->label) + QStringLiteral(":"), row);
					lbl->setMinimumWidth(120);
					rowLayout->addWidget(lbl);

					QLineEdit* edit = new QLineEdit(row);
					edit->setText(QString::fromStdString(wp->stringVal));
					rowLayout->addWidget(edit);

					// on_change: fires on every keystroke and focus-lost
					connect(edit, &QLineEdit::textChanged, this, [widgetKey](const QString& text) {
						LuaEngine::instance().setWidgetStringValue(widgetKey, text.toStdString());
					});
					// on_submit: fires on Enter only
					connect(edit, &QLineEdit::returnPressed, this, [widgetKey]() {
						LuaEngine::instance().widgetSubmit(widgetKey);
					});

					if (wp->browse == "dir")
					{
						QPushButton* browseBtn = new QPushButton(tr("Browse..."), row);
						connect(browseBtn, &QPushButton::clicked, this, [this, edit]() {
							QString dir = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(this, tr("Select Directory"), edit->text()));
							if (!dir.isEmpty())
								edit->setText(dir);
						});
						rowLayout->addWidget(browseBtn);
					}
					else if (wp->browse == "file")
					{
						QPushButton* browseBtn = new QPushButton(tr("Browse..."), row);
						QString filter = QString::fromStdString(wp->filter);
						connect(browseBtn, &QPushButton::clicked, this, [this, edit, filter]() {
							QString file = QDir::toNativeSeparators(QFileDialog::getOpenFileName(this, tr("Select File"), edit->text(), filter));
							if (!file.isEmpty())
								edit->setText(file);
						});
						rowLayout->addWidget(browseBtn);
					}
					else if (wp->browse == "save")
					{
						QPushButton* browseBtn = new QPushButton(tr("Browse..."), row);
						QString filter = QString::fromStdString(wp->filter);
						connect(browseBtn, &QPushButton::clicked, this, [this, edit, filter]() {
							QString file = QDir::toNativeSeparators(QFileDialog::getSaveFileName(this, tr("Save File"), edit->text(), filter));
							if (!file.isEmpty())
								edit->setText(file);
						});
						rowLayout->addWidget(browseBtn);
					}
					break;
				}

				case LuaEngine::LuaWidget::Type::Checkbox:
				{
					if (wp->options.empty())
					{
						// Bool checkbox
						QCheckBox* cb = new QCheckBox(QString::fromStdString(wp->label), row);
						cb->setChecked(wp->boolVal);
						connect(cb, &QCheckBox::toggled, this, [widgetKey](bool checked) {
							LuaEngine::instance().setWidgetBoolValue(widgetKey, checked);
						});
						rowLayout->addWidget(cb);
					}
					else if (wp->dropdown)
					{
						// Dropdown (combobox)
						QLabel* lbl = new QLabel(QString::fromStdString(wp->label) + QStringLiteral(":"), row);
						lbl->setMinimumWidth(120);
						rowLayout->addWidget(lbl);

						QComboBox* combo = new QComboBox(row);
						int selectedIdx = 0;
						for (size_t i = 0; i < wp->options.size(); i++)
						{
							combo->addItem(QString::fromStdString(wp->options[i]));
							if (wp->options[i] == wp->stringVal)
								selectedIdx = static_cast<int>(i);
						}
						combo->setCurrentIndex(selectedIdx);
						connect(combo, &QComboBox::currentTextChanged, this, [widgetKey](const QString& text) {
							LuaEngine::instance().setWidgetStringValue(widgetKey, text.toStdString());
						});
						rowLayout->addWidget(combo);
					}
					else if (wp->list)
					{
						// List selection
						QLabel* lbl = new QLabel(QString::fromStdString(wp->label) + QStringLiteral(":"), row);
						lbl->setMinimumWidth(120);
						rowLayout->addWidget(lbl);

						QListWidget* listWidget = new QListWidget(row);
						listWidget->setSelectionMode(QAbstractItemView::SingleSelection);
						for (const auto& opt : wp->options)
						{
							QListWidgetItem* item = new QListWidgetItem(QString::fromStdString(opt), listWidget);
							if (opt == wp->stringVal)
								item->setSelected(true);
						}
						// Max 5 visible items before scrollbar
						int itemHeight = listWidget->sizeHintForRow(0);
						if (itemHeight <= 0)
							itemHeight = 20;
						int visibleItems = std::min(static_cast<int>(wp->options.size()), 5);
						listWidget->setMaximumHeight(itemHeight * visibleItems + 2 * listWidget->frameWidth());
						connect(listWidget, &QListWidget::currentTextChanged, this, [widgetKey](const QString& text) {
							if (!text.isEmpty())
								LuaEngine::instance().setWidgetStringValue(widgetKey, text.toStdString());
						});
						// on_submit: fires on double-click
						connect(listWidget, &QListWidget::itemDoubleClicked, this, [widgetKey]() {
							LuaEngine::instance().widgetSubmit(widgetKey);
						});
						rowLayout->addWidget(listWidget);
					}
					else
					{
						// Radio buttons
						QLabel* lbl = new QLabel(QString::fromStdString(wp->label) + QStringLiteral(":"), row);
						lbl->setMinimumWidth(120);
						rowLayout->addWidget(lbl);

						for (const auto& opt : wp->options)
						{
							QRadioButton* radio = new QRadioButton(QString::fromStdString(opt), row);
							if (opt == wp->stringVal)
								radio->setChecked(true);
							connect(radio, &QRadioButton::toggled, this, [widgetKey, opt](bool checked) {
								if (checked)
									LuaEngine::instance().setWidgetStringValue(widgetKey, opt);
							});
							rowLayout->addWidget(radio);
						}
					}
					break;
				}

				case LuaEngine::LuaWidget::Type::Slider:
				{
					QLabel* lbl = new QLabel(QString::fromStdString(wp->label) + QStringLiteral(":"), row);
					lbl->setMinimumWidth(120);
					rowLayout->addWidget(lbl);

					int sliderSteps = static_cast<int>((wp->sliderMax - wp->sliderMin) / wp->sliderStep);
					int sliderPos = static_cast<int>((wp->numberVal - wp->sliderMin) / wp->sliderStep);

					QSlider* slider = new QSlider(Qt::Horizontal, row);
					slider->setRange(0, sliderSteps);
					slider->setValue(sliderPos);
					rowLayout->addWidget(slider);

					QDoubleSpinBox* spinBox = new QDoubleSpinBox(row);
					spinBox->setRange(wp->sliderMin, wp->sliderMax);
					spinBox->setSingleStep(wp->sliderStep);
					spinBox->setValue(wp->numberVal);
					// Determine decimal places from step
					int decimals = 0;
					double frac = wp->sliderStep - static_cast<int>(wp->sliderStep);
					if (frac > 0)
					{
						double s = wp->sliderStep;
						while (s < 1.0 && decimals < 6)
						{
							s *= 10.0;
							decimals++;
						}
					}
					spinBox->setDecimals(decimals);
					spinBox->setMaximumWidth(100);
					rowLayout->addWidget(spinBox);

					double sMin = wp->sliderMin;
					double sStep = wp->sliderStep;

					// Sync slider → spinbox (visual only during drag)
					connect(slider, &QSlider::valueChanged, this, [spinBox, sMin, sStep](int pos) {
						double val = sMin + pos * sStep;
						spinBox->blockSignals(true);
						spinBox->setValue(val);
						spinBox->blockSignals(false);
					});

					// Slider released → fire on_change + on_submit
					connect(slider, &QSlider::sliderReleased, this, [slider, widgetKey, sMin, sStep]() {
						double val = sMin + slider->value() * sStep;
						LuaEngine::instance().setWidgetNumberValue(widgetKey, val);
						LuaEngine::instance().widgetSubmit(widgetKey);
					});

					// Sync spinbox → slider + fire on_change + on_submit
					connect(spinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
						[slider, widgetKey, sMin, sStep](double val) {
							int pos = static_cast<int>((val - sMin) / sStep);
							slider->blockSignals(true);
							slider->setValue(pos);
							slider->blockSignals(false);
							LuaEngine::instance().setWidgetNumberValue(widgetKey, val);
							LuaEngine::instance().widgetSubmit(widgetKey);
						});
					break;
				}

				case LuaEngine::LuaWidget::Type::Button:
				{
					QPushButton* btn = new QPushButton(QString::fromStdString(wp->label), row);
					connect(btn, &QPushButton::clicked, this, [widgetKey]() {
						LuaEngine::instance().widgetButtonClicked(widgetKey);
					});
					rowLayout->addWidget(btn);
					break;
				}
			}

			boxLayout->addWidget(row);
		}

		m_commandsLayout->addWidget(box);
	}

	m_commandsLayout->addStretch();
}

void LuaModWindow::onWidgetsChanged()
{
	rebuildCommandsTab();
}

// ============================================================
// Settings tab
// ============================================================

void LuaModWindow::createSettingsTab()
{
	QWidget* settingsWidget = new QWidget(this);
	QVBoxLayout* layout = new QVBoxLayout(settingsWidget);

	QLabel* header = new QLabel(tr("Lua Script Directories"), settingsWidget);
	QFont boldFont = header->font();
	boldFont.setBold(true);
	header->setFont(boldFont);
	layout->addWidget(header);

	QLabel* desc = new QLabel(tr("Scripts are loaded from the default lua-autoload/ directory first, then from each directory below in order. Within each directory, scripts load alphabetically."), settingsWidget);
	desc->setWordWrap(true);
	desc->setStyleSheet(QStringLiteral("color: gray;"));
	layout->addWidget(desc);

	m_dirList = new QListWidget(settingsWidget);
	m_dirList->setSelectionMode(QAbstractItemView::SingleSelection);
	connect(m_dirList, &QListWidget::itemSelectionChanged, this, [this]() {
		// Don't allow removing the default directory (first item)
		int row = m_dirList->currentRow();
		m_removeDirBtn->setEnabled(row > 0);
	});
	layout->addWidget(m_dirList);

	QHBoxLayout* btnLayout = new QHBoxLayout();
	QPushButton* addBtn = new QPushButton(tr("Add Directory..."), settingsWidget);
	connect(addBtn, &QPushButton::clicked, this, &LuaModWindow::onAddDirClicked);
	btnLayout->addWidget(addBtn);

	m_removeDirBtn = new QPushButton(tr("Remove"), settingsWidget);
	m_removeDirBtn->setEnabled(false);
	connect(m_removeDirBtn, &QPushButton::clicked, this, &LuaModWindow::onRemoveDirClicked);
	btnLayout->addWidget(m_removeDirBtn);

	btnLayout->addStretch();
	layout->addLayout(btnLayout);

	layout->addStretch();

	m_tabWidget->addTab(settingsWidget, tr("Settings"));

	refreshDirList();
}

void LuaModWindow::refreshDirList()
{
	m_dirList->clear();

	// Default directory (always first, not removable)
	std::string defaultDir = Path::Combine(EmuFolders::DataRoot, "lua-autoload");
	QListWidgetItem* defaultItem = new QListWidgetItem(QString::fromStdString(defaultDir) + tr(" (default)"));
	defaultItem->setFlags(defaultItem->flags() & ~Qt::ItemIsSelectable);
	m_dirList->addItem(defaultItem);

	// User-configured directories
	std::vector<std::string> userDirs = Host::GetBaseStringListSetting("LuaMods", "ScriptDirs");
	for (const auto& dir : userDirs)
		m_dirList->addItem(QString::fromStdString(dir));

	m_removeDirBtn->setEnabled(false);
}

void LuaModWindow::onAddDirClicked()
{
	QString dir = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(this, tr("Select Lua Script Directory")));
	if (dir.isEmpty())
		return;

	std::string spath = dir.toStdString();
	Host::AddBaseValueToStringList("LuaMods", "ScriptDirs", spath.c_str());
	Host::CommitBaseSettingChanges();
	refreshDirList();
	appendMessage(QStringLiteral("[Settings] Added script directory: %1\n").arg(dir));
}

void LuaModWindow::onRemoveDirClicked()
{
	int row = m_dirList->currentRow();
	if (row <= 0) // 0 is the default dir
		return;

	QListWidgetItem* item = m_dirList->item(row);
	if (!item)
		return;

	QString itemText = item->text();
	std::string spath = itemText.toStdString();
	Host::RemoveBaseValueFromStringList("LuaMods", "ScriptDirs", spath.c_str());
	Host::CommitBaseSettingChanges();
	refreshDirList();
	appendMessage(QStringLiteral("[Settings] Removed script directory: %1\n").arg(itemText));
}

