// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <QtCore/QFile>
#include <QtCore/QString>

#include <map>
#include <utility>

namespace GowWadInjector
{
	// Magic handle value to identify injected files
	static constexpr s32 INJECTED_HANDLE_MAGIC = -100;

	// Map: sysFile EE pointer -> (host QFile*, filename)
	extern std::map<u32, std::pair<QFile*, QString>> s_injectedFiles;

	// Custom WAD directory for injection
	void setCustomWadDirectory(const QString& dir);
	QString customWadDirectory();

	// Register WAD injection execution hooks
	void initWadInjectorHooks();
	// Remove WAD injection execution hooks
	void shutdownWadInjectorHooks();
} // namespace GowWadInjector
