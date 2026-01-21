// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "LuaHooks.h"
#include "DebugServer.h"
#include "LuaEngine.h"
#include "LuaOverlay.h"
#include "MemTracker.h"
#include "LuaModWindow.h"

#include "Config.h"
#include "Host.h"

// Forward declaration to avoid pulling in ImGuiOverlays.h (which needs TinyString from PCH)
namespace ImGuiManager
{
	using CustomOverlayRenderFunc = void(*)();
	void SetCustomOverlayRenderFunc(CustomOverlayRenderFunc func);
}

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

std::vector<std::string> LuaHooks::getScriptDirs()
{
	std::vector<std::string> dirs;

	// Default directory is always first
	dirs.push_back(Path::Combine(EmuFolders::DataRoot, "lua-autoload"));

	// User-configured directories
	std::vector<std::string> userDirs = Host::GetBaseStringListSetting("LuaMods", "ScriptDirs");
	for (auto& d : userDirs)
	{
		// Avoid duplicates with the default dir
		if (d != dirs[0])
			dirs.push_back(std::move(d));
	}

	return dirs;
}

static void luaAutoloadScripts()
{
	const auto dirs = LuaHooks::getScriptDirs();
	int totalCount = 0;

	for (const auto& dir : dirs)
	{
		FileSystem::EnsureDirectoryExists(dir.c_str(), false);

		FileSystem::FindResultsArray results;
		FileSystem::FindFiles(dir.c_str(), "*.lua", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_SORT_BY_NAME, &results);

		if (results.empty())
			continue;

		int count = 0;
		for (const auto& entry : results)
		{
			Console.WriteLn("[Lua] Autoloading Lua script: %s", entry.FileName.c_str());
			if (LuaEngine::instance().runFile(entry.FileName))
				count++;
		}

		Console.WriteLn("[Lua] Autoloaded %d Lua script(s) from %s", count, dir.c_str());
		totalCount += count;
	}

	if (totalCount > 0)
		Console.WriteLn("[Lua] Total autoloaded: %d script(s) from %zu director(ies)", totalCount, dirs.size());
}

void LuaHooks::reloadScripts()
{
	LuaEngine::instance().stopScript();
	LuaOverlay::Reset();
	MemTracker::instance().clearAllTracking();
	luaAutoloadScripts();
}

void LuaHooks::init()
{
	// Wire OSD overlay rendering into the ImGui overlay pipeline
	ImGuiManager::SetCustomOverlayRenderFunc(&LuaOverlay::RenderFrame);

	DebugServer::instance().start();
	luaAutoloadScripts();
}

void LuaHooks::shutdown()
{
	LuaEngine::instance().stopScript();
	LuaOverlay::Reset();
	ImGuiManager::SetCustomOverlayRenderFunc(nullptr);
	DebugServer::instance().stop();
	MemTracker::instance().clearAllTracking();
}
