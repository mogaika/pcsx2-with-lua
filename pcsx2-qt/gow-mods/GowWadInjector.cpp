// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GowWadInjector.h"
#include "GowModWindow.h"

#include "Memory.h"
#include "R5900.h"
#include "x86/iR5900.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#include <cstring>

namespace GowWadInjector
{
	std::map<u32, std::pair<QFile*, QString>> s_injectedFiles;
	static QString s_customWadDirectory;

	void setCustomWadDirectory(const QString& dir)
	{
		s_customWadDirectory = dir;
	}

	QString customWadDirectory()
	{
		return s_customWadDirectory;
	}
} // namespace GowWadInjector

// Hook for sysFile::OpenResource at 0x17ad70
static void hookSysFileOpenResource()
{
	u32 thisPtr = cpuRegs.GPR.n.a0.UL[0];
	u32 filenamePtr = cpuRegs.GPR.n.a1.UL[0];
	u32 mode = cpuRegs.GPR.n.a2.UL[0];

	const char* filename = nullptr;
	if (filenamePtr != 0)
		filename = (const char*)PSM(filenamePtr);

	if (filename && !GowWadInjector::customWadDirectory().isEmpty())
	{
		QString qFilename = QString::fromLatin1(filename);
		QString targetName = qFilename + QStringLiteral(".WAD");

		QDir customDir(GowWadInjector::customWadDirectory());
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
				sysFile[0] = (u32)GowWadInjector::INJECTED_HANDLE_MAGIC;
				sysFile[1] = mode | 0x200;
				sysFile[2] = (u32)file->size();
				sysFile[3] = 0;

				GowWadInjector::s_injectedFiles[thisPtr] = {file, qFilename};

				GowModWindow::logInjectionMessage(
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

	GowModWindow::logFileOpen(thisPtr, filename, mode);
}

// Hook for sysFile::Read at 0x17afe0
static void hookSysFileRead()
{
	u32 thisPtr = cpuRegs.GPR.n.a0.UL[0];
	u32 bufferPtr = cpuRegs.GPR.n.a1.UL[0];
	u32 amount = cpuRegs.GPR.n.a2.UL[0];

	s32 fHandle = *(s32*)PSM(thisPtr + 0x00);
	u32 fPosition = *(u32*)PSM(thisPtr + 0x0c);

	if (fHandle == GowWadInjector::INJECTED_HANDLE_MAGIC)
	{
		auto it = GowWadInjector::s_injectedFiles.find(thisPtr);
		if (it != GowWadInjector::s_injectedFiles.end())
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

	GowModWindow::logFileRead(thisPtr, (u32)fHandle, bufferPtr, amount, fPosition);
}

// Hook for sysFile::Close at 0x17aee8
static void hookSysFileClose()
{
	u32 thisPtr = cpuRegs.GPR.n.a0.UL[0];
	s32 fHandle = *(s32*)PSM(thisPtr + 0x00);

	if (fHandle == GowWadInjector::INJECTED_HANDLE_MAGIC)
	{
		auto it = GowWadInjector::s_injectedFiles.find(thisPtr);
		if (it != GowWadInjector::s_injectedFiles.end())
		{
			QString filename = it->second.second;
			delete it->second.first;
			GowWadInjector::s_injectedFiles.erase(it);

			*(s32*)PSM(thisPtr + 0x00) = -1;

			GowModWindow::logInjectionMessage(
				QStringLiteral("[WAD_INJECT] Closed: %1\n").arg(filename));

			cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
			g_executionHookSkipBlock = true;
			return;
		}
	}

	GowModWindow::logFileClose(thisPtr, (u32)fHandle);
}

// Hook for sysFile::IsReadDone at 0x17af78
static void hookSysFileIsReadDone()
{
	u32 thisPtr = cpuRegs.GPR.n.a0.UL[0];
	s32 fHandle = *(s32*)PSM(thisPtr + 0x00);

	if (fHandle == GowWadInjector::INJECTED_HANDLE_MAGIC)
	{
		cpuRegs.GPR.n.v0.UL[0] = 1;
		cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
		g_executionHookSkipBlock = true;
		return;
	}
}

void GowWadInjector::initWadInjectorHooks()
{
	addExecutionHook(0x17AD70, hookSysFileOpenResource);
	addExecutionHook(0x17AF78, hookSysFileIsReadDone);
	addExecutionHook(0x17AFE0, hookSysFileRead);
	addExecutionHook(0x17AEE8, hookSysFileClose);
}

void GowWadInjector::shutdownWadInjectorHooks()
{
	removeExecutionHook(0x17AD70);
	removeExecutionHook(0x17AF78);
	removeExecutionHook(0x17AFE0);
	removeExecutionHook(0x17AEE8);
}
