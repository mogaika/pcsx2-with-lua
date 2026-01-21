// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>
#include <vector>

namespace LuaHooks
{
	void init();
	void shutdown();
	void reloadScripts();

	// Returns the list of Lua script directories: default lua-autoload/ first, then user-configured dirs.
	std::vector<std::string> getScriptDirs();
} // namespace LuaHooks
