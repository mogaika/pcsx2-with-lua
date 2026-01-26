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
#include <QtCore/QUtf8StringView>
#include <QtGui/QIcon>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollBar>

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

// Hook for svrClientParm::IFFProcessClientParm at 0x01789e0
// Signature: void IFFProcessClientParm(Header* HeaderPtr, char* DataPtr)
// - a0 = HeaderPtr (contains resource name at +8, 24 chars)
// - a1 = DataPtr (this is what we want to trace)
// DataPtr structure: { u32 magic, ..., data at +0x40 with size 0x10 }
// Filter: magic == 0x00020001
static void hookIFFProcessClientParm()
{
	u32 headerPtr = cpuRegs.GPR.n.a0.UL[0];  // Header* HeaderPtr
	u32 dataPtr = cpuRegs.GPR.n.a1.UL[0];    // char* DataPtr

	if (dataPtr == 0)
		return;

	// Read magic from DataPtr (first u32)
	u32 magic = *(u32*)PSM(dataPtr);

	// Filter by magic - only trace resources with magic 0x00020001
	// (Add more magic values here as needed)
	if (magic != 0x00020001)
		return;

	// Get resource name from HeaderPtr + 8 (24 char name)
	QString resourceName;
	if (headerPtr != 0)
	{
		const char* namePtr = (const char*)PSM(headerPtr + 8);
		if (namePtr)
			resourceName = QString::fromLatin1(namePtr, strnlen(namePtr, 24));
	}

	// The data we want to trace is at DataPtr + 0x00, size 0x5c
	u32 traceStart = dataPtr + 0x00;
	u32 traceSize = 0x5c;

	GameEventLogWindow::logInjectionMessage(
		QString("[TRACE_ADD] name='%1' magic=0x%2 dataPtr=0x%3 tracing 0x%4-0x%5\n")
			.arg(resourceName)
			.arg(magic, 8, 16, QChar('0'))
			.arg(dataPtr, 8, 16, QChar('0'))
			.arg(traceStart, 8, 16, QChar('0'))
			.arg(traceStart + traceSize, 8, 16, QChar('0')));

	// Add trace with STOP_ON_WRITE flag - trace stops and reports when memory is reused
	// Store the resource name as userData (allocated, will be freed in callback)
	std::string* descriptionPtr = new std::string(
		QString("ClientParm '%1' magic=0x%2")
			.arg(resourceName)
			.arg(magic, 8, 16, QChar('0')).toStdString());

	MemoryTraceManager::Instance().AddTrace(
		traceStart, traceSize,
		*descriptionPtr,
		[](u32 start, u32 end, const std::map<u32, MemoryAccessInfo>& reads,
		   const std::map<u32, MemoryAccessInfo>& writes, u32 writePC, void* userData) {
			// Get the description from userData
			std::string* desc = static_cast<std::string*>(userData);
			QString description = desc ? QString::fromStdString(*desc) : QString();
			delete desc;  // Clean up

			// Report results when overwritten
			QString msg = QString("[TRACE_RESULT] %1 Region 0x%2-0x%3\n")
				.arg(description)
				.arg(start, 8, 16, QChar('0'))
				.arg(end, 8, 16, QChar('0'));

			if (writePC)
				msg += QString("  Overwritten at PC=0x%1\n").arg(writePC, 8, 16, QChar('0'));

			msg += QString("  Unique addresses read: %1\n").arg(reads.size());

			// Show all reads with their PCs (for small traced regions)
			for (const auto& [addr, info] : reads) {
				msg += QString("  Addr 0x%1: %2 total reads from:\n")
					.arg(addr, 8, 16, QChar('0'))
					.arg(info.count);

				// Sort PCs by read count
				std::vector<std::pair<u32, u32>> pcSorted(info.pcCounts.begin(), info.pcCounts.end());
				std::sort(pcSorted.begin(), pcSorted.end(),
					[](const auto& a, const auto& b) { return a.second > b.second; });

				for (const auto& [pc, count] : pcSorted) {
					msg += QString("      PC=0x%1: %2 times\n")
						.arg(pc, 8, 16, QChar('0'))
						.arg(count);
				}
			}

			GameEventLogWindow::logInjectionMessage(msg);
		},
		MEMTRACE_TRACK_READS | MEMTRACE_STOP_ON_WRITE,  // Stop and report when memory reused
		descriptionPtr  // Pass description as userData
	);
}

// Hook for sysFile::OpenResource at 0x17ad70
// Signature: sysFile::OpenResource(sysFile* this, char* filename, uint mode)
// Returns: uint (1=success, 0=fail) in v0
static void hookSysFileOpenResource()
{
	u32 thisPtr = cpuRegs.GPR.n.a0.UL[0];     // sysFile* this
	u32 filenamePtr = cpuRegs.GPR.n.a1.UL[0]; // char* filename
	u32 mode = cpuRegs.GPR.n.a2.UL[0];        // mode flags

	const char* filename = nullptr;
	if (filenamePtr != 0)
		filename = (const char*)PSM(filenamePtr);

	// Check if we have a custom WAD directory configured
	if (filename && !GameEventLogWindow::s_customWadDirectory.isEmpty())
	{
		QString qFilename = QString::fromLatin1(filename);
		QString targetName = qFilename + QStringLiteral(".WAD");

		// Case-insensitive file search in the custom directory
		QDir customDir(GameEventLogWindow::s_customWadDirectory);
		QString foundPath;

		// First try exact match for performance
		QString exactPath = customDir.filePath(targetName);
		if (QFileInfo::exists(exactPath))
		{
			foundPath = exactPath;
		}
		else
		{
			// Search directory for case-insensitive match
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
				// Get sysFile structure in EE memory
				// struct sysFile { int fHandle; uint fFlags; int fLength; uint fPosition; void* vtbl; }
				u32* sysFile = (u32*)PSM(thisPtr);

				// Set up sysFile structure
				// Force sync mode (0x200) for injected files so IsReadDone always returns true
				// This prevents the game from polling in an infinite loop waiting for async completion
				sysFile[0] = (u32)GameEventLogWindow::INJECTED_HANDLE_MAGIC;  // fHandle = -100
				sysFile[1] = mode | 0x200;                                     // fFlags with sync bit
				sysFile[2] = (u32)file->size();                                // fLength
				sysFile[3] = 0;                                                // fPosition

				// Track this file
				GameEventLogWindow::s_injectedFiles[thisPtr] = {file, qFilename};

				// Log injection with flags (mode | 0x200 for sync)
				GameEventLogWindow::logInjectionMessage(
					QStringLiteral("[WAD_INJECT] Opened custom: %1 (%2 bytes, flags=0x%3)\n")
						.arg(foundPath)
						.arg(file->size())
						.arg(mode | 0x200, 8, 16, QChar('0')));

				// Set return value (1 = success)
				cpuRegs.GPR.n.v0.UL[0] = 1;

				// Skip original function - set PC to return address and signal to skip block
				cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
				g_executionHookSkipBlock = true;
				return;
			}
			else
			{
				// File exists but failed to open
				GameEventLogWindow::logInjectionMessage(
					QStringLiteral("[WAD_INJECT] ERROR: Failed to open file: %1 (error: %2)\n")
						.arg(foundPath)
						.arg(file->errorString()));
				delete file;
			}
		}
		else
		{
			// Log that we checked but file doesn't exist in custom directory
			GameEventLogWindow::logInjectionMessage(
				QStringLiteral("[WAD_INJECT] Not found in custom dir: %1 (looking for %2)\n")
					.arg(GameEventLogWindow::s_customWadDirectory)
					.arg(targetName));
		}
	}

	// No custom file or injection failed - log and let original function run
	GameEventLogWindow::logFileOpen(thisPtr, filename, mode);
}

// Hook for sysFile::Read at 0x17afe0
// Signature: sysFile::Read(sysFile* this, void* buffer, uint amount)
// Returns: uint bytes_read in v0
static void hookSysFileRead()
{
	u32 thisPtr = cpuRegs.GPR.n.a0.UL[0];    // sysFile* this
	u32 bufferPtr = cpuRegs.GPR.n.a1.UL[0];  // void* buffer
	u32 amount = cpuRegs.GPR.n.a2.UL[0];     // uint amount

	// Read sysFile fields: fHandle at +0x00, fPosition at +0x0c
	s32 fHandle = *(s32*)PSM(thisPtr + 0x00);
	u32 fPosition = *(u32*)PSM(thisPtr + 0x0c);

	// Check if this is an injected file
	if (fHandle == GameEventLogWindow::INJECTED_HANDLE_MAGIC)
	{
		auto it = GameEventLogWindow::s_injectedFiles.find(thisPtr);
		if (it != GameEventLogWindow::s_injectedFiles.end())
		{
			QFile* file = it->second.first;
			QString& filename = it->second.second;
			u32* sysFile = (u32*)PSM(thisPtr);

			u32 fLength = sysFile[2];    // +0x08
			u32 fPos = sysFile[3];       // +0x0c

			// Clamp read to remaining bytes
			u32 remaining = fLength - fPos;
			u32 requestedAmount = amount;
			if (amount > remaining)
				amount = remaining;

			u32 bytesRead = 0;
			if (amount > 0)
			{
				// Seek to position
				file->seek(fPos);

				// Read data
				QByteArray data = file->read(amount);
				bytesRead = (u32)data.size();

				// Copy to EE memory
				u8* eeBuffer = (u8*)PSM(bufferPtr);
				std::memcpy(eeBuffer, data.data(), bytesRead);

				// Update position
				sysFile[3] = fPos + bytesRead;
			}

			// Log the read operation (only log significant reads to avoid spam)
			u32 fFlags = sysFile[1];
			if (bytesRead == 0 || fPos == 0 || fPos + bytesRead >= fLength)
			{
				GameEventLogWindow::logInjectionMessage(
					QStringLiteral("[WAD_INJECT] Read %1: pos=%2 req=%3 read=%4 len=%5 flags=0x%6\n")
						.arg(filename)
						.arg(fPos)
						.arg(requestedAmount)
						.arg(bytesRead)
						.arg(fLength)
						.arg(fFlags, 8, 16, QChar('0')));
			}

			// Set return value
			cpuRegs.GPR.n.v0.UL[0] = bytesRead;

			// Skip original function
			cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
			g_executionHookSkipBlock = true;
			return;
		}
		else
		{
			// Handle has magic value but not in our map - this is an error
			GameEventLogWindow::logInjectionMessage(
				QStringLiteral("[WAD_INJECT] ERROR: Magic handle but sysFile=0x%1 not tracked!\n")
					.arg(thisPtr, 8, 16, QChar('0')));
		}
	}

	// Not injected - log and let original run
	GameEventLogWindow::logFileRead(thisPtr, (u32)fHandle, bufferPtr, amount, fPosition);
}

// Hook for sysFile::Close at 0x17aee8
// Returns: void
static void hookSysFileClose()
{
	u32 thisPtr = cpuRegs.GPR.n.a0.UL[0];
	s32 fHandle = *(s32*)PSM(thisPtr + 0x00);

	// Check if this is an injected file
	if (fHandle == GameEventLogWindow::INJECTED_HANDLE_MAGIC)
	{
		auto it = GameEventLogWindow::s_injectedFiles.find(thisPtr);
		if (it != GameEventLogWindow::s_injectedFiles.end())
		{
			QString filename = it->second.second;
			delete it->second.first;  // Close QFile
			GameEventLogWindow::s_injectedFiles.erase(it);

			// Clear sysFile handle
			*(s32*)PSM(thisPtr + 0x00) = -1;

			// Log closure
			GameEventLogWindow::logInjectionMessage(
				QStringLiteral("[WAD_INJECT] Closed: %1\n").arg(filename));

			// Skip original function (returns void)
			cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
			g_executionHookSkipBlock = true;
			return;
		}
	}

	// Not injected - log and let original run
	GameEventLogWindow::logFileClose(thisPtr, (u32)fHandle);
}

// Hook for sysFile::IsReadDone at 0x17af78
// Returns: bool (true if read complete, false if pending)
// This is polled by the game after async reads - we must return true for injected files
static void hookSysFileIsReadDone()
{
	u32 thisPtr = cpuRegs.GPR.n.a0.UL[0];
	s32 fHandle = *(s32*)PSM(thisPtr + 0x00);

	// Check if this is an injected file
	if (fHandle == GameEventLogWindow::INJECTED_HANDLE_MAGIC)
	{
		// Injected files are always "done" - we read synchronously
		cpuRegs.GPR.n.v0.UL[0] = 1;  // return true
		cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
		g_executionHookSkipBlock = true;
		return;
	}
	// Let original run for non-injected files
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
	// Load custom WAD directory setting
	s_customWadDirectory = QString::fromStdString(
		Host::GetBaseStringSettingValue("GameEventLog", "CustomWadDirectory", ""));

	restoreSize();
	createUi();
	initHooks();

	// Log current injection directory if set
	if (!s_customWadDirectory.isEmpty())
	{
		appendMessage(QStringLiteral("[CONFIG] Custom WAD directory loaded: %1\n").arg(s_customWadDirectory));
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

	// Skip if file read logs are disabled
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

	// Skip logging closes on invalid handles (already closed or never opened)
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

	// File I/O hooks for tracing WAD file operations
	addExecutionHook(0x17AD70, hookSysFileOpenResource);  // sysFile::OpenResource
	addExecutionHook(0x17AF78, hookSysFileIsReadDone);    // sysFile::IsReadDone
	addExecutionHook(0x17AFE0, hookSysFileRead);          // sysFile::Read
	addExecutionHook(0x17AEE8, hookSysFileClose);         // sysFile::Close
	addExecutionHook(0x185F28, hookWadLoaderProcessWadFile); // wadLoader::ProcessWadFile

	// Memory trace hook for resource parsing analysis
	addExecutionHook(0x01789e0, hookIFFProcessClientParm);  // IFFProcessClientParm
}

void GameEventLogWindow::shutdownHooks()
{
	// Remove all hooks when window is destroyed
	removeExecutionHook(0x1BB0F8);
	removeExecutionHook(0x17AD70);
	removeExecutionHook(0x17AF78);
	removeExecutionHook(0x17AFE0);
	removeExecutionHook(0x17AEE8);
	removeExecutionHook(0x185F28);
	removeExecutionHook(0x01789e0);

	// Flush any remaining memory traces
	MemoryTraceManager::Instance().FlushAllTraces();
}
