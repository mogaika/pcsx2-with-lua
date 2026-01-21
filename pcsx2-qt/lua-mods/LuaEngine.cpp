// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "LuaEngine.h"
#include "LuaHooks.h"
#include "LuaOverlay.h"
#include "MemTracker.h"
#include "Config.h"

#include "DebugTools/Breakpoints.h"
#include "DebugTools/MemoryTrace.h"
#include "Memory.h"
#include "R5900.h"
#include "VMManager.h"
#include "INISettingsInterface.h"
#include "VUops.h"
#include "x86/iR5900.h"

#include "Host.h"
#include "common/Console.h"
#include "common/SettingsInterface.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#define SOL_ALL_SAFETIES_ON 1
#define SOL_USING_CXX_LUA 0
#define SOL_EXCEPTIONS_ALWAYS_UNSAFE 1
#define SOL_PRINT_ERRORS 0
#include <sol/sol.hpp>

#include <cstring>
#include <fstream>

static void addDirToPackagePath(lua_State* L, std::string_view dirView)
{
	if (dirView.empty())
		return;
	std::string dir(dirView);
	sol::state_view lua(L);
	std::string curPath = lua["package"]["path"].get_or<std::string>("");
	std::string prefix = dir + "/?.lua;" + dir + "/?/init.lua;";
	if (curPath.find(prefix) == std::string::npos)
		lua["package"]["path"] = prefix + curPath;
}

LuaEngine* LuaEngine::s_instance = nullptr;

LuaEngine& LuaEngine::instance()
{
	static LuaEngine s;
	return s;
}

LuaEngine::LuaEngine() = default;

LuaEngine::~LuaEngine()
{
	stopScript();
}

static int luaPanicHandler(lua_State* L)
{
	const char* msg = lua_tostring(L, -1);
	Console.Error("[Lua] PANIC: %s", msg ? msg : "(unknown error)");

	// Dump Lua call stack for debugging
	luaL_traceback(L, L, NULL, 0);
	const char* tb = lua_tostring(L, -1);
	if (tb && tb[0])
		Console.Error("[Lua] Traceback: %s", tb);

	// Dump full Lua stack contents for debugging
	int top = lua_gettop(L);
	Console.Error("[Lua] Stack has %d items:", top);
	for (int i = 1; i <= top; i++)
	{
		int t = lua_type(L, i);
		switch (t)
		{
			case LUA_TSTRING:
				Console.Error("[Lua]   [%d] string: '%s'", i, lua_tostring(L, i));
				break;
			case LUA_TNUMBER:
				Console.Error("[Lua]   [%d] number: %g", i, lua_tonumber(L, i));
				break;
			case LUA_TFUNCTION:
				Console.Error("[Lua]   [%d] function: %p", i, lua_topointer(L, i));
				break;
			case LUA_TTABLE:
				Console.Error("[Lua]   [%d] table: %p", i, lua_topointer(L, i));
				break;
			default:
				Console.Error("[Lua]   [%d] %s", i, lua_typename(L, t));
				break;
		}
	}

	return 0;
}

void LuaEngine::createState()
{
	m_luaState = luaL_newstate();
	lua_atpanic(m_luaState, luaPanicHandler);
	luaL_openlibs(m_luaState);
	s_instance = this;

	// Configure package.path to include all script directories for require()
	for (const auto& dir : LuaHooks::getScriptDirs())
		addDirToPackagePath(m_luaState, dir);

	registerBindings();
}

void LuaEngine::destroyState()
{
	if (m_luaState)
	{
		cleanupHooks();
		cleanupTraces();
		cleanupBreakpoints();
		cleanupWidgets();
		cleanupOsdState();
		lua_close(m_luaState);
		m_luaState = nullptr;
	}
	if (s_instance == this)
		s_instance = nullptr;
	m_runFiles.clear();
	m_currentGroup.clear();
	m_groupDescriptions.clear();
}

bool LuaEngine::loadScript(const std::string& path)
{
	std::lock_guard lock(m_luaMutex);

	m_scriptErrors.clear();
	destroyState();
	createState();
	m_scriptPath = path;

	sol::state_view lua(m_luaState);

	// Add script's directory to package.path so require() resolves relative to it
	addDirToPackagePath(m_luaState, Path::GetDirectory(path));

	sol::protected_function_result result = lua.safe_script_file(path, sol::script_pass_on_error);
	if (!result.valid())
	{
		sol::error err = result;
		std::string errMsg = std::string("Script load error (") + path + "): " + err.what();
		logMessage(LOG_ERROR, "Lua", errMsg);
		m_scriptErrors.push_back(std::move(errMsg));
		destroyState();
		m_scriptPath.clear();
		return false;
	}

	logMessage(LOG_INFO, "Lua", "Script loaded: " + path);
	return true;
}

bool LuaEngine::reloadScript()
{
	std::lock_guard lock(m_luaMutex);

	if (m_scriptPath.empty())
		return false;

	std::string path = m_scriptPath;
	m_scriptErrors.clear();
	destroyState();
	createState();

	// Add script's directory to package.path
	addDirToPackagePath(m_luaState, Path::GetDirectory(path));

	sol::state_view lua(m_luaState);
	sol::protected_function_result result = lua.safe_script_file(path, sol::script_pass_on_error);
	if (!result.valid())
	{
		sol::error err = result;
		std::string errMsg = std::string("Script reload error (") + path + "): " + err.what();
		logMessage(LOG_ERROR, "Lua", errMsg);
		m_scriptErrors.push_back(std::move(errMsg));
		destroyState();
		m_scriptPath.clear();
		return false;
	}

	m_scriptPath = path;
	logMessage(LOG_INFO, "Lua", "Script reloaded: " + path);
	return true;
}

void LuaEngine::stopScript()
{
	std::lock_guard lock(m_luaMutex);
	m_scriptErrors.clear();
	destroyState();
	m_scriptPath.clear();
}

bool LuaEngine::runFile(const std::string& path)
{
	std::lock_guard lock(m_luaMutex);

	if (!m_luaState)
		createState();

	sol::state_view lua(m_luaState);

	// Add script's directory to package.path so require() resolves relative to it
	addDirToPackagePath(m_luaState, Path::GetDirectory(path));

	sol::protected_function_result result = lua.safe_script_file(path, sol::script_pass_on_error);
	if (!result.valid())
	{
		sol::error err = result;
		std::string errMsg = std::string("runFile error (") + path + "): " + err.what();
		logMessage(LOG_ERROR, "Lua", errMsg);
		m_scriptErrors.push_back(std::move(errMsg));
		return false;
	}

	m_runFiles.push_back(path);
	logMessage(LOG_INFO, "Lua", "runFile: " + path);
	return true;
}

bool LuaEngine::executeString(const std::string& code)
{
	std::lock_guard lock(m_luaMutex);

	if (!m_luaState)
	{
		// Create a bare state for console-only usage
		createState();
	}

	sol::state_view lua(m_luaState);
	sol::protected_function_result result = lua.safe_script(code, sol::script_pass_on_error);
	if (!result.valid())
	{
		sol::error err = result;
		logMessage(LOG_ERROR, "Lua", std::string("Lua error: ") + err.what());
		return false;
	}

	return true;
}

bool LuaEngine::executeString(const std::string& code, std::string& returnValue)
{
	std::lock_guard lock(m_luaMutex);

	if (!m_luaState)
		createState();

	sol::state_view lua(m_luaState);
	sol::protected_function_result result = lua.safe_script(code, sol::script_pass_on_error);
	if (!result.valid())
	{
		sol::error err = result;
		logMessage(LOG_ERROR, "Lua", std::string("Lua error: ") + err.what());
		return false;
	}

	// Capture return value if any
	if (result.return_count() > 0)
	{
		sol::object obj = result.get<sol::object>(0);
		if (obj.valid() && obj.get_type() != sol::type::none && obj.get_type() != sol::type::lua_nil)
		{
			// Use lua tostring for a universal representation
			lua_getglobal(m_luaState, "tostring");
			obj.push();
			if (lua_pcall(m_luaState, 1, 1, 0) == 0)
			{
				const char* s = lua_tostring(m_luaState, -1);
				if (s)
					returnValue = s;
				lua_pop(m_luaState, 1);
			}
			else
			{
				lua_pop(m_luaState, 1);
			}
		}
	}

	return true;
}

void LuaEngine::dispatchHook(u32 addr)
{
	std::lock_guard lock(m_luaMutex);

	if (!m_luaState)
		return;

	auto it = m_luaHooks.find(addr);
	if (it == m_luaHooks.end())
		return;

	// Copy hooks so callbacks can safely modify m_luaHooks without invalidation
	std::vector<LuaHookEntry> hooksCopy = it->second;

	for (size_t i = 0; i < hooksCopy.size(); i++)
	{
		int stackBase = lua_gettop(m_luaState);
		lua_rawgeti(m_luaState, LUA_REGISTRYINDEX, hooksCopy[i].luaRef);
		sol::protected_function fn(m_luaState, -1);
		sol::protected_function_result res = fn();

		if (!res.valid())
		{
			// Extract error message before cleaning up the stack
			const char* errStr = lua_tostring(m_luaState, -1);
			std::string errMsg = errStr ? errStr : "(unknown error)";
			logMessage(LOG_ERROR, "Lua", fmt::format("Hook error at 0x{:08x}: {}", addr, errMsg));
		}

		// Restore stack to pre-call state (pops function, results, and error)
		lua_settop(m_luaState, stackBase);

		// Lua state may have been destroyed by the callback (e.g. stopScript)
		if (!m_luaState)
			return;

		if (hooksCopy[i].oneShot)
		{
			// Remove this one-shot from the live map if it still exists
			auto liveIt = m_luaHooks.find(addr);
			if (liveIt != m_luaHooks.end())
			{
				auto& liveHooks = liveIt->second;
				for (auto jt = liveHooks.begin(); jt != liveHooks.end(); ++jt)
				{
					if (jt->luaRef == hooksCopy[i].luaRef)
					{
						luaL_unref(m_luaState, LUA_REGISTRYINDEX, jt->luaRef);
						liveHooks.erase(jt);
						break;
					}
				}
			}
		}
	}

	// If no hooks remain at this address, remove the execution hook
	auto liveIt = m_luaHooks.find(addr);
	if (liveIt != m_luaHooks.end() && liveIt->second.empty())
	{
		m_luaHooks.erase(liveIt);
		removeExecutionHook(addr);
		m_installedHookAddresses.erase(addr);
	}
}

void LuaEngine::hookTrampoline()
{
	if (s_instance)
		s_instance->dispatchHook(cpuRegs.pc);
}

void LuaEngine::cleanupHooks()
{
	for (u32 addr : m_installedHookAddresses)
		removeExecutionHook(addr);
	m_installedHookAddresses.clear();

	if (m_luaState)
	{
		for (auto& [addr, hooks] : m_luaHooks)
		{
			for (auto& h : hooks)
				luaL_unref(m_luaState, LUA_REGISTRYINDEX, h.luaRef);
		}
	}
	m_luaHooks.clear();
}

void LuaEngine::cleanupTraces()
{
	auto& mgr = MemoryTraceManager::Instance();
	for (u32 hookId : m_activeTraceHookIds)
		mgr.UnregisterHook(hookId);
	m_activeTraceHookIds.clear();
}

void LuaEngine::cleanupBreakpoints()
{
	for (u32 addr : m_installedBreakpoints)
		CBreakPoints::RemoveBreakPoint(BREAKPOINT_EE, addr);
	m_installedBreakpoints.clear();

	for (const auto& mc : m_installedMemChecks)
		CBreakPoints::RemoveMemCheck(BREAKPOINT_EE, mc.start, mc.start + mc.size);
	m_installedMemChecks.clear();
}

void LuaEngine::cleanupWidgets()
{
	if (m_luaState)
	{
		for (auto& w : m_registeredWidgets)
		{
			if (w.callbackRef != -1)
				luaL_unref(m_luaState, LUA_REGISTRYINDEX, w.callbackRef);
			if (w.onChangeRef != -1)
				luaL_unref(m_luaState, LUA_REGISTRYINDEX, w.onChangeRef);
			if (w.onSubmitRef != -1)
				luaL_unref(m_luaState, LUA_REGISTRYINDEX, w.onSubmitRef);
		}
	}
	m_registeredWidgets.clear();

	if (m_widgetsChangedCallback)
		m_widgetsChangedCallback();
}

std::vector<LuaEngine::LuaWidget> LuaEngine::snapshotWidgets() const
{
	std::lock_guard lock(m_luaMutex);
	return m_registeredWidgets;
}

std::map<std::string, std::string> LuaEngine::snapshotGroupDescriptions() const
{
	std::lock_guard lock(m_luaMutex);
	return m_groupDescriptions;
}

LuaEngine::LuaWidget* LuaEngine::findWidget(const std::string& key)
{
	auto it = std::find_if(m_registeredWidgets.begin(), m_registeredWidgets.end(),
		[&key](const LuaWidget& w) { return w.key == key; });
	return it != m_registeredWidgets.end() ? &(*it) : nullptr;
}

void LuaEngine::fireOnChange(LuaWidget& w)
{
	if (w.onChangeRef == -1 || !m_luaState)
		return;

	lua_rawgeti(m_luaState, LUA_REGISTRYINDEX, w.onChangeRef);
	sol::protected_function fn(m_luaState, -1);

	sol::protected_function_result res;
	switch (w.type)
	{
		case LuaWidget::Type::Text:
			res = fn(w.stringVal);
			break;
		case LuaWidget::Type::Checkbox:
			if (w.options.empty())
				res = fn(w.boolVal);
			else
				res = fn(w.stringVal);
			break;
		case LuaWidget::Type::Slider:
			res = fn(w.numberVal);
			break;
		default:
			lua_pop(m_luaState, 1);
			return;
	}
	lua_pop(m_luaState, 1);

	if (!res.valid())
	{
		sol::error err = res;
		logMessage(LOG_ERROR, "Lua", fmt::format("Widget '{}' on_change error: {}", w.key, err.what()));
	}
}

void LuaEngine::fireOnSubmit(LuaWidget& w)
{
	if (w.onSubmitRef == -1 || !m_luaState)
		return;

	lua_rawgeti(m_luaState, LUA_REGISTRYINDEX, w.onSubmitRef);
	sol::protected_function fn(m_luaState, -1);

	sol::protected_function_result res;
	switch (w.type)
	{
		case LuaWidget::Type::Text:
			res = fn(w.stringVal);
			break;
		case LuaWidget::Type::Checkbox:
			if (w.options.empty())
				res = fn(w.boolVal);
			else
				res = fn(w.stringVal);
			break;
		case LuaWidget::Type::Slider:
			res = fn(w.numberVal);
			break;
		default:
			lua_pop(m_luaState, 1);
			return;
	}
	lua_pop(m_luaState, 1);

	if (!res.valid())
	{
		sol::error err = res;
		logMessage(LOG_ERROR, "Lua", fmt::format("Widget '{}' on_submit error: {}", w.key, err.what()));
	}
}

void LuaEngine::setWidgetStringValue(const std::string& key, const std::string& value)
{
	std::lock_guard lock(m_luaMutex);
	LuaWidget* w = findWidget(key);
	if (!w)
		return;
	w->stringVal = value;
	fireOnChange(*w);
}

void LuaEngine::setWidgetBoolValue(const std::string& key, bool value)
{
	std::lock_guard lock(m_luaMutex);
	LuaWidget* w = findWidget(key);
	if (!w)
		return;
	w->boolVal = value;
	fireOnChange(*w);
}

void LuaEngine::setWidgetNumberValue(const std::string& key, double value)
{
	std::lock_guard lock(m_luaMutex);
	LuaWidget* w = findWidget(key);
	if (!w)
		return;
	// Clamp to range
	if (value < w->sliderMin)
		value = w->sliderMin;
	if (value > w->sliderMax)
		value = w->sliderMax;
	w->numberVal = value;
	fireOnChange(*w);
}

void LuaEngine::widgetButtonClicked(const std::string& key)
{
	std::lock_guard lock(m_luaMutex);
	if (!m_luaState)
		return;

	LuaWidget* w = findWidget(key);
	if (!w || w->type != LuaWidget::Type::Button || w->callbackRef == -1)
		return;

	lua_rawgeti(m_luaState, LUA_REGISTRYINDEX, w->callbackRef);
	sol::protected_function fn(m_luaState, -1);
	sol::protected_function_result res = fn();
	lua_pop(m_luaState, 1);

	if (!res.valid())
	{
		sol::error err = res;
		logMessage(LOG_ERROR, "Lua", fmt::format("Widget button '{}' error: {}", w->key, err.what()));
	}
}

void LuaEngine::widgetSubmit(const std::string& key)
{
	std::lock_guard lock(m_luaMutex);
	LuaWidget* w = findWidget(key);
	if (!w)
		return;
	fireOnSubmit(*w);
}

void LuaEngine::logMessage(int level, const std::string& system, const std::string& msg)
{
	switch (level)
	{
		case LOG_INFO:
			Console.WriteLn("[Lua/%s] %s", system.c_str(), msg.c_str());
			break;
		case LOG_WARN:
			Console.Warning("[Lua/%s] %s", system.c_str(), msg.c_str());
			break;
		case LOG_ERROR:
			Console.Error("[Lua/%s] %s", system.c_str(), msg.c_str());
			break;
	}

	{
		std::lock_guard lock(m_logBufMutex);
		if (m_logBuffer.size() < MAX_LOG_BUFFER)
			m_logBuffer.push_back({level, system, msg});
	}

	LogCallback cb;
	{
		std::lock_guard lock(m_luaMutex);
		cb = m_logCallback;
	}
	if (cb)
		cb(level, system, msg);
}

std::vector<LuaEngine::LogEntry> LuaEngine::drainLogs()
{
	std::lock_guard lock(m_logBufMutex);
	std::vector<LogEntry> result;
	result.swap(m_logBuffer);
	return result;
}

// ============================================================
// Binding registration
// ============================================================

void LuaEngine::registerBindings()
{
	registerMemBindings();
	registerRegBindings();
	registerEmuBindings();
	registerLogBindings();
	registerCpuBindings();
	registerUiBindings();
	registerConfigBindings();
	registerOsdBindings();

	// Override print() to route through log.info
	sol::state_view lua(m_luaState);
	lua["print"] = [this](sol::variadic_args va) {
		lua_State* L = va.lua_state();
		std::string msg;
		for (auto v : va)
		{
			if (!msg.empty())
				msg += "\t";
			// Use Lua's tostring() for safe conversion of any type
			lua_getglobal(L, "tostring");
			v.push();
			if (lua_pcall(L, 1, 1, 0) == 0)
			{
				const char* s = lua_tostring(L, -1);
				msg += s ? s : "";
				lua_pop(L, 1);
			}
			else
			{
				msg += "<tostring error>";
				lua_pop(L, 1);
			}
		}
		logMessage(LOG_INFO, "Lua", msg);
	};
}

// ============================================================
// mem — PS2 memory access
// ============================================================

static void* checkedPSM(u32 addr, sol::this_state ts)
{
	void* ptr = PSM(addr);
	if (!ptr)
		luaL_error(ts.lua_state(), "invalid PS2 address: 0x%08x", addr);
	return ptr;
}

static LuaStackTrace parseLuaStackTrace(sol::optional<sol::table>& stackTable)
{
	LuaStackTrace trace;
	if (stackTable)
	{
		sol::optional<sol::table> framesOpt = (*stackTable)["frames"];
		sol::optional<int> depthOpt = (*stackTable)["depth"];
		if (framesOpt && depthOpt)
		{
			trace.depth = std::min(*depthOpt, LuaStackTrace::MAX_FRAMES);
			for (int i = 0; i < trace.depth; i++)
			{
				sol::optional<u32> f = (*framesOpt)[i + 1];
				trace.frames[i] = f.value_or(0);
			}
		}
	}
	return trace;
}

void LuaEngine::registerMemBindings()
{
	sol::state_view lua(m_luaState);
	sol::table mem = lua.create_named_table("mem");

	mem["read8"] = [](u32 addr, sol::this_state ts) -> u32 {
		return *reinterpret_cast<u8*>(checkedPSM(addr, ts));
	};
	mem["read16"] = [](u32 addr, sol::this_state ts) -> u32 {
		return *reinterpret_cast<u16*>(checkedPSM(addr, ts));
	};
	mem["read32"] = [](u32 addr, sol::this_state ts) -> u32 {
		return *reinterpret_cast<u32*>(checkedPSM(addr, ts));
	};
	mem["read64"] = [](u32 addr, sol::this_state ts) -> u64 {
		return *reinterpret_cast<u64*>(checkedPSM(addr, ts));
	};
	mem["write8"] = [](u32 addr, u32 val, sol::this_state ts) {
		*reinterpret_cast<u8*>(checkedPSM(addr, ts)) = static_cast<u8>(val);
	};
	mem["write16"] = [](u32 addr, u32 val, sol::this_state ts) {
		*reinterpret_cast<u16*>(checkedPSM(addr, ts)) = static_cast<u16>(val);
	};
	mem["write32"] = [](u32 addr, u32 val, sol::this_state ts) {
		*reinterpret_cast<u32*>(checkedPSM(addr, ts)) = val;
	};
	mem["read_float"] = [](u32 addr, sol::this_state ts) -> float {
		return *reinterpret_cast<float*>(checkedPSM(addr, ts));
	};
	mem["write_float"] = [](u32 addr, float val, sol::this_state ts) {
		*reinterpret_cast<float*>(checkedPSM(addr, ts)) = val;
	};
	mem["read_string"] = [](u32 addr, sol::optional<int> maxLen, sol::this_state ts) -> std::string {
		int max = maxLen.value_or(256);
		const char* ptr = reinterpret_cast<const char*>(checkedPSM(addr, ts));
		int len = 0;
		while (len < max && ptr[len] != '\0')
			len++;
		return std::string(ptr, len);
	};
	mem["read_bytes"] = [](u32 addr, int len, sol::this_state ts) -> sol::table {
		sol::state_view lua(ts);
		sol::table t = lua.create_table(len, 0);
		const u8* ptr = reinterpret_cast<const u8*>(checkedPSM(addr, ts));
		for (int i = 0; i < len; i++)
			t[i + 1] = ptr[i];
		return t;
	};

	// mem.write_bytes(addr, data [, offset [, length]])
	mem["write_bytes"] = [](u32 addr, const std::string& data,
	                        sol::optional<int> offsetOpt,
	                        sol::optional<int> lenOpt,
	                        sol::this_state ts) {
		int off = offsetOpt.value_or(0);
		int len = lenOpt.value_or((int)data.size() - off);
		if (off < 0 || len <= 0 || off >= (int)data.size())
			return;
		len = std::min(len, (int)data.size() - off);
		u8* dst = reinterpret_cast<u8*>(checkedPSM(addr, ts));
		std::memcpy(dst, data.data() + off, len);
	};

	mem["write64"] = [](u32 addr, u64 val, sol::this_state ts) {
		*reinterpret_cast<u64*>(checkedPSM(addr, ts)) = val;
	};

	mem["write_string"] = [](u32 addr, const std::string& str, sol::this_state ts) {
		u8* dst = reinterpret_cast<u8*>(checkedPSM(addr, ts));
		std::memcpy(dst, str.c_str(), str.size());
		dst[str.size()] = 0;
	};

	// mem.breakpoint(start, size, mode)
	mem["breakpoint"] = [this](u32 start, u32 size, sol::optional<std::string> mode) {
		std::string m = mode.value_or("rw");
		MemCheckCondition cond = MEMCHECK_READWRITE;
		if (m == "r")
			cond = MEMCHECK_READ;
		else if (m == "w")
			cond = MEMCHECK_WRITE;
		CBreakPoints::AddMemCheck(BREAKPOINT_EE, start, start + size, cond, MEMCHECK_BREAK);
		m_installedMemChecks.push_back({start, size});
	};

	// mem.remove_breakpoint(start, size)
	mem["remove_breakpoint"] = [this](u32 start, u32 size) {
		CBreakPoints::RemoveMemCheck(BREAKPOINT_EE, start, start + size);
		auto it = std::find_if(m_installedMemChecks.begin(), m_installedMemChecks.end(),
			[start, size](const MemCheckEntry& e) { return e.start == start && e.size == size; });
		if (it != m_installedMemChecks.end())
			m_installedMemChecks.erase(it);
	};

	// Helper: convert trace stats to Lua table
	auto traceStatsToTable = [](const std::vector<TraceEntry>& stats, sol::this_state ts) -> sol::table {
		sol::state_view lua(ts);
		sol::table t = lua.create_table();
		int idx = 1;
		for (const auto& entry : stats)
		{
			sol::table e = lua.create_table();
			e["offset"] = entry.key.offset;
			e["count"] = entry.count;
			e["is_dma"] = entry.isDma;
			sol::table stack = lua.create_table();
			int si = 1;
			for (u32 pc : entry.key.stack.pcs)
			{
				if (pc == 0)
					break;
				stack[si++] = pc;
			}
			e["stack"] = stack;
			t[idx++] = e;
		}
		return t;
	};

	// mem.trace_start(addr, {name=, reads=true, writes=false, dma=false}) -> hookId
	mem["trace_start"] = [this](u32 addr, sol::table opts) -> u32 {
		std::string name = opts.get_or<std::string>("name", "lua_trace");
		bool reads = opts.get_or("reads", true);
		bool writes = opts.get_or("writes", false);
		bool dma = opts.get_or("dma", false);

		u32 flags = 0;
		if (reads) flags |= MEMTRACE_TRACK_READS;
		if (writes) flags |= MEMTRACE_TRACK_WRITES;
		if (dma) flags |= MEMTRACE_TRACK_DMA;

		const TrackedRegion* region = MemTracker::instance().findRegion(addr);
		if (!region)
		{
			logMessage(LOG_WARN, "Lua", fmt::format("mem.trace_start: no zone at 0x{:08x}", addr));
			return 0;
		}

		auto& mgr = MemoryTraceManager::Instance();
		u32 hookId = mgr.RegisterHook(name, nullptr, flags);
		mgr.AddTracedRange(hookId, region->address, region->size);
		m_activeTraceHookIds.push_back(hookId);
		return hookId;
	};

	// mem.trace_stop(hookId)
	mem["trace_stop"] = [this](u32 hookId) {
		auto& mgr = MemoryTraceManager::Instance();
		mgr.UnregisterHook(hookId);
		auto it = std::find(m_activeTraceHookIds.begin(), m_activeTraceHookIds.end(), hookId);
		if (it != m_activeTraceHookIds.end())
			m_activeTraceHookIds.erase(it);
	};

	// mem.trace_read(hookId) — read trace stats without clearing
	mem["trace_read"] = [traceStatsToTable](u32 hookId, sol::this_state ts) -> sol::table {
		auto& mgr = MemoryTraceManager::Instance();
		return traceStatsToTable(mgr.GetHookTraceStats(hookId), ts);
	};

	// mem.trace_flush(hookId) — clear accumulated trace stats
	mem["trace_flush"] = [](u32 hookId) {
		MemoryTraceManager::Instance().FlushHook(hookId);
	};

	// mem.traces_flush() — flush all active traces
	mem["traces_flush"] = []() {
		MemTracker::instance().flushAllTraces();
	};

	// Helper: build a region table from a TrackedRegion pointer
	auto regionToTable = [](const TrackedRegion* region, sol::state_view lua) -> sol::table {
		sol::table t = lua.create_table();
		t["address"] = region->address;
		t["size"] = region->size;
		t["parent"] = region->parentAddr;
		t["name"] = region->name;
		sol::table tags = lua.create_table();
		for (const auto& [k, v] : region->tags)
			tags[k] = v;
		t["tags"] = tags;
		return t;
	};

	// mem.zone_find(addr) — find zone at exact address
	mem["zone_find"] = [regionToTable](u32 addr, sol::this_state ts) -> sol::object {
		sol::state_view lua(ts);
		const TrackedRegion* region = MemTracker::instance().findRegion(addr);
		if (!region)
			return sol::nil;
		return regionToTable(region, lua);
	};

	// mem.zone_find_containing(addr) — find zone whose range contains addr
	mem["zone_find_containing"] = [regionToTable](u32 addr, sol::this_state ts) -> sol::object {
		sol::state_view lua(ts);
		const TrackedRegion* region = MemTracker::instance().findContaining(addr);
		if (!region)
			return sol::nil;
		return regionToTable(region, lua);
	};

	// mem.zone_tag(addr, key, value)
	mem["zone_tag"] = [](sol::this_state ts, u32 addr, sol::optional<std::string> key, sol::optional<std::string> value) {
		if (!key || !value)
		{
			luaL_error(ts.lua_state(), "mem.zone_tag: expected (addr, key, value)");
			return;
		}
		MemTracker::instance().addTag(addr, *key, *value);
	};

	// mem.zone_untag(addr, key)
	mem["zone_untag"] = [](sol::this_state ts, u32 addr, sol::optional<std::string> key) {
		if (!key)
		{
			luaL_error(ts.lua_state(), "mem.zone_untag: expected (addr, key)");
			return;
		}
		MemTracker::instance().removeTag(addr, *key);
	};

	// mem.zones_count()
	mem["zones_count"] = []() -> size_t {
		return MemTracker::instance().regionCount();
	};

	// mem.zones_snapshot()
	mem["zones_snapshot"] = [regionToTable](sol::this_state ts) -> sol::table {
		sol::state_view lua(ts);
		auto regions = MemTracker::instance().snapshotRegions();
		sol::table t = lua.create_table();
		int idx = 1;
		for (const auto& [addr, region] : regions)
		{
			t[idx++] = regionToTable(&region, lua);
		}
		return t;
	};

	// mem.zones_query({parent=, contains=, tag={k=v}})
	mem["zones_query"] = [regionToTable](sol::table opts, sol::this_state ts) -> sol::table {
		sol::state_view lua(ts);
		TrackedRegionQuery query;
		if (auto parent = opts.get<sol::optional<u32>>("parent"))
			query.parentAddr = *parent;
		if (auto contains = opts.get<sol::optional<u32>>("contains"))
			query.containsAddress = *contains;
		if (auto tagTbl = opts.get<sol::optional<sol::table>>("tag"))
		{
			for (auto& [k, v] : *tagTbl)
			{
				if (k.is<std::string>() && v.is<std::string>())
					query.tags[k.as<std::string>()] = v.as<std::string>();
			}
		}
		auto results = MemTracker::instance().query(query);
		sol::table t = lua.create_table();
		int idx = 1;
		for (const auto* region : results)
		{
			t[idx++] = regionToTable(region, lua);
		}
		return t;
	};

	// mem.zone_track(address, size [, parent_addr [, name [, stack_table]]])
	mem["zone_track"] = [](u32 addr, u32 size,
	                       sol::optional<u32> parentAddr,
	                       sol::optional<std::string> name,
	                       sol::optional<sol::table> stackTable) {
		LuaStackTrace trace = parseLuaStackTrace(stackTable);
		MemTracker::instance().recordAlloc(
			addr, size, parentAddr.value_or(0), name.value_or(""), trace);
	};

	// mem.zone_free(addr)
	mem["zone_free"] = [](u32 addr) {
		MemTracker::instance().recordFree(addr);
	};

	// mem.zones_clear()
	mem["zones_clear"] = []() {
		MemTracker::instance().clearAllTracking();
	};
}

// ============================================================
// reg — CPU registers
// ============================================================

static int gprNameToIndex(const std::string& name)
{
	// Standard MIPS GPR names
	static const char* names[] = {
		"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
		"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
		"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
		"t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
	};
	for (int i = 0; i < 32; i++)
	{
		if (name == names[i])
			return i;
	}
	return -1;
}

void LuaEngine::registerRegBindings()
{
	sol::state_view lua(m_luaState);
	sol::table reg = lua.create_named_table("reg");

	// Set get/set BEFORE installing metatable, otherwise __newindex
	// intercepts function assignments and rejects them as non-numeric
	reg["get"] = [](int idx) -> u32 {
		if (idx >= 0 && idx < 32)
			return cpuRegs.GPR.r[idx].UL[0];
		return 0;
	};
	reg["set"] = [](int idx, u32 val) {
		if (idx >= 0 && idx < 32)
			cpuRegs.GPR.r[idx].UL[0] = val;
	};

	// Use metatable for named register access (reg.a0, reg.v0, etc.)
	sol::table regMeta = lua.create_table();
	regMeta[sol::meta_function::index] = [](sol::this_state ts, sol::table /*self*/, sol::object keyObj) -> sol::object {
		sol::state_view lua(ts);
		if (!keyObj.is<std::string>())
			return sol::nil;
		std::string key = keyObj.as<std::string>();
		if (key == "pc")
			return sol::make_object(lua, cpuRegs.pc);
		if (key == "cycle")
			return sol::make_object(lua, cpuRegs.cycle);
		int idx = gprNameToIndex(key);
		if (idx >= 0)
			return sol::make_object(lua, cpuRegs.GPR.r[idx].UL[0]);
		return sol::nil;
	};
	regMeta[sol::meta_function::new_index] = [](sol::this_state ts, sol::table /*self*/, sol::object keyObj, u32 val) {
		if (!keyObj.is<std::string>())
		{
			luaL_error(ts.lua_state(), "reg: expected string key");
			return;
		}
		std::string key = keyObj.as<std::string>();
		int idx = gprNameToIndex(key);
		if (idx >= 0)
			cpuRegs.GPR.r[idx].UL[0] = val;
	};
	reg[sol::metatable_key] = regMeta;
}

// ============================================================
// Check if a PS2 MIPS address is a branch delay slot (instruction after a branch/jump).
// Hooks on delay slots are silently skipped by the recompiler.
static bool isMipsDelaySlot(u32 addr)
{
	if (addr < 4 || addr >= 0x02000000)
		return false;

	u32 prevInstr = *reinterpret_cast<const u32*>(PSM(addr - 4));
	u32 op = prevInstr >> 26;

	switch (op)
	{
		case 0x00: // SPECIAL
		{
			u32 func = prevInstr & 0x3f;
			// JR (0x08), JALR (0x09)
			return func == 0x08 || func == 0x09;
		}
		case 0x01: // REGIMM — BLTZ, BGEZ, BLTZL, BGEZL, BLTZAL, BGEZAL, BLTZALL, BGEZALL
			return true;
		case 0x02: // J
		case 0x03: // JAL
		case 0x04: // BEQ
		case 0x05: // BNE
		case 0x06: // BLEZ
		case 0x07: // BGTZ
			return true;
		case 0x14: // BEQL
		case 0x15: // BNEL
		case 0x16: // BLEZL
		case 0x17: // BGTZL
			return true;
		default:
			return false;
	}
}

// ============================================================
// emu — Emulator control
// ============================================================

void LuaEngine::registerEmuBindings()
{
	sol::state_view lua(m_luaState);
	sol::table emu = lua.create_named_table("emu");

	emu["pause"] = [](sol::optional<std::string> reason) {
		if (reason.has_value())
			VMManager::SetPauseReason(VMPauseReason::LuaAssert, *reason, cpuRegs.pc);
		VMManager::SetPaused(true);
	};

	emu["resume"] = []() {
		VMManager::SetPaused(false);
	};

	emu["is_paused"] = []() -> bool {
		return VMManager::GetState() == VMState::Paused;
	};

	emu["is_running"] = []() -> bool {
		return VMManager::GetState() == VMState::Running;
	};

	emu["reset"] = []() {
		bool wasPaused = VMManager::GetState() == VMState::Paused;
		if (!wasPaused)
			VMManager::SetPaused(true);
		LuaHooks::reloadScripts();
		VMManager::Reset();
		if (!wasPaused)
			VMManager::SetPaused(false);
	};

	emu["game_serial"] = []() -> std::string {
		return VMManager::GetDiscSerial();
	};

	emu["game_crc"] = []() -> std::string {
		return fmt::format("0x{:08x}", VMManager::GetDiscCRC());
	};

	// emu.skip_block() — skip the hooked function (set pc=ra)
	emu["skip_block"] = []() {
		cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
		g_executionHookSkipBlock = true;
	};

	// emu.assert(msg [, stack_table]) — pause VM immediately with a message
	emu["assert"] = [](std::string message, sol::optional<sol::table> stackTable) {
		LuaStackTrace trace = parseLuaStackTrace(stackTable);
		std::string fullMsg = message;
		if (trace.depth > 0)
			fullMsg += " stack=[" + trace.toString() + "]";
		Console.Error("[Lua] assert: %s", fullMsg.c_str());
		VMManager::SetPauseReason(VMPauseReason::LuaAssert, fullMsg, cpuRegs.pc);
		VMManager::SetPaused(true);
		Cpu->ExitExecution();
	};

	emu["set_speed"] = [](sol::this_state ts, sol::optional<std::string> modeOpt) {
		if (!modeOpt)
		{
			luaL_error(ts.lua_state(), "emu.set_speed: expected (mode)");
			return;
		}
		const std::string& mode = *modeOpt;
		if (mode == "normal")
			VMManager::SetLimiterMode(LimiterModeType::Nominal);
		else if (mode == "turbo")
			VMManager::SetLimiterMode(LimiterModeType::Turbo);
		else if (mode == "slow")
			VMManager::SetLimiterMode(LimiterModeType::Slomo);
		else if (mode == "unlimited")
			VMManager::SetLimiterMode(LimiterModeType::Unlimited);
	};

	// emu.vu1_profile() — print VU1 per-PC hit counts sorted by address, then reset
	emu["vu1_profile"] = [this]() {
		u32* hits = getVU1PcHitCounts();
		if (!hits)
		{
			logMessage(LOG_WARN, "VU1", "VU1 profiler not available");
			return;
		}

		u64 total = 0;
		for (u32 i = 0; i < 2048; i++)
			total += hits[i];

		logMessage(LOG_INFO, "VU1", fmt::format("=== VU1 PC Profile (total hits: {}) ===", total));

		// Print sorted by PC, 16 entries per line
		for (u32 row = 0; row < 2048; row += 16)
		{
			bool allZero = true;
			for (u32 j = 0; j < 16 && (row + j) < 2048; j++)
			{
				if (hits[row + j] > 0)
				{
					allZero = false;
					break;
				}
			}
			if (allZero)
				continue;

			std::string line = fmt::format("0x{:04x}:", row * 8);
			for (u32 j = 0; j < 16 && (row + j) < 2048; j++)
				line += fmt::format(" {:>8}", hits[row + j]);
			logMessage(LOG_INFO, "VU1", line);
		}

		resetVU1PcHitCounts();
		logMessage(LOG_INFO, "VU1", "Counters reset.");
	};

	// emu.vu1_trace_start([filename], [opts]) — start recording VU1 execution traces to file
	// opts is an optional table with boolean fields: vif, regs, memory, pc_hits (all default true)
	emu["vu1_trace_start"] = [this](sol::optional<std::string> path, sol::optional<sol::table> opts) {
		std::string p = path.value_or(Path::Combine(EmuFolders::Logs, "vu1_trace.txt"));
		u32 flags = VU1_TRACE_VIF; // VIF is always included
		if (opts.has_value())
		{
			sol::table t = opts.value();
			if (t.get_or("regs", false))
				flags |= VU1_TRACE_REGS;
			if (t.get_or("memory", false))
				flags |= VU1_TRACE_MEMORY;
			if (t.get_or("pc_hits", false))
				flags |= VU1_TRACE_PC_HITS;
		}
		{
			std::lock_guard<std::mutex> lock(g_vu1Trace.mutex);
			if (g_vu1Trace.file)
			{
				std::fclose(g_vu1Trace.file);
				g_vu1Trace.file = nullptr;
			}
			g_vu1Trace.file = std::fopen(p.c_str(), "w");
			if (!g_vu1Trace.file)
			{
				logMessage(LOG_ERROR, "VU1", fmt::format("Failed to open trace file: {}", p));
				return;
			}
			g_vu1Trace.execCount = 0;
			g_vu1Trace.active = false;
			g_vu1Trace.flags = flags;
			g_vu1Trace.vifLog.clear();
			g_vu1Trace.pendingVifLogs.clear();
			g_vu1Trace.zoneLookup = [](u32 eeAddr) -> std::string {
				const TrackedRegion* r = MemTracker::instance().findContaining(eeAddr);
				if (!r)
					return {};
				std::string result = r->name;
				for (const auto& [k, v] : r->tags)
				{
					if (!result.empty())
						result += ", ";
					result += k + "=" + v;
				}
				return result;
			};
			g_vu1Trace.armed = true;
		}
		logMessage(LOG_INFO, "VU1", fmt::format("VU1 trace started: {}", p));
	};

	// emu.vu1_trace_stop() — stop recording VU1 execution traces
	emu["vu1_trace_stop"] = [this]() {
		int count;
		{
			std::lock_guard<std::mutex> lock(g_vu1Trace.mutex);
			g_vu1Trace.armed = false;
			g_vu1Trace.active = false;
			g_vu1Trace.zoneLookup = nullptr;
			count = g_vu1Trace.execCount;
			if (g_vu1Trace.file)
			{
				std::fclose(g_vu1Trace.file);
				g_vu1Trace.file = nullptr;
			}
		}
		logMessage(LOG_INFO, "VU1", fmt::format("VU1 trace stopped ({} executions)", count));
	};
}

// ============================================================
// log — Logging
// ============================================================

void LuaEngine::registerLogBindings()
{
	sol::state_view lua(m_luaState);
	sol::table log = lua.create_named_table("log");

	auto safeToString = [](sol::object obj) -> std::string {
		if (!obj.valid() || obj.get_type() == sol::type::none || obj.get_type() == sol::type::lua_nil)
			return "nil";
		if (obj.is<std::string>())
			return obj.as<std::string>();
		lua_State* L = obj.lua_state();
		lua_getglobal(L, "tostring");
		obj.push();
		if (lua_pcall(L, 1, 1, 0) == 0)
		{
			const char* s = lua_tostring(L, -1);
			std::string result = s ? s : "nil";
			lua_pop(L, 1);
			return result;
		}
		lua_pop(L, 1);
		return "<tostring error>";
	};

	// log.info(system, msg), log.warn(system, msg), log.error(system, msg)
	log["info"] = [this, safeToString](sol::object system, sol::object msg) {
		logMessage(LOG_INFO, safeToString(system), safeToString(msg));
	};
	log["warn"] = [this, safeToString](sol::object system, sol::object msg) {
		logMessage(LOG_WARN, safeToString(system), safeToString(msg));
	};
	log["error"] = [this, safeToString](sol::object system, sol::object msg) {
		logMessage(LOG_ERROR, safeToString(system), safeToString(msg));
	};
}

// ============================================================
// cpu — Execution hooks and breakpoints
// ============================================================

void LuaEngine::registerCpuBindings()
{
	sol::state_view lua(m_luaState);
	sol::table cpu = lua.create_named_table("cpu");

	// cpu.hook(addr, fn)
	cpu["hook"] = [this](u32 addr, sol::protected_function fn) {
		std::lock_guard lock(m_luaMutex);

		if (VMManager::HasValidVM() && isMipsDelaySlot(addr))
			logMessage(LOG_WARN, "Lua", fmt::format("Hook at 0x{:08x} is a branch delay slot — it will never fire", addr));

		fn.push();
		int ref = luaL_ref(m_luaState, LUA_REGISTRYINDEX);

		m_luaHooks[addr].push_back({ref, false});

		if (m_installedHookAddresses.count(addr) == 0)
		{
			addExecutionHook(addr, hookTrampoline);
			m_installedHookAddresses.insert(addr);
		}
	};

	// cpu.hook_once(addr, fn)
	cpu["hook_once"] = [this](u32 addr, sol::protected_function fn) {
		std::lock_guard lock(m_luaMutex);

		if (VMManager::HasValidVM() && isMipsDelaySlot(addr))
			logMessage(LOG_WARN, "Lua", fmt::format("Hook at 0x{:08x} is a branch delay slot — it will never fire", addr));

		fn.push();
		int ref = luaL_ref(m_luaState, LUA_REGISTRYINDEX);

		m_luaHooks[addr].push_back({ref, true});

		if (m_installedHookAddresses.count(addr) == 0)
		{
			addExecutionHook(addr, hookTrampoline);
			m_installedHookAddresses.insert(addr);
		}
	};

	// cpu.unhook(addr)
	cpu["unhook"] = [this](u32 addr) {
		std::lock_guard lock(m_luaMutex);
		auto it = m_luaHooks.find(addr);
		if (it != m_luaHooks.end())
		{
			for (auto& h : it->second)
				luaL_unref(m_luaState, LUA_REGISTRYINDEX, h.luaRef);
			m_luaHooks.erase(it);
		}
		removeExecutionHook(addr);
		m_installedHookAddresses.erase(addr);
	};

	// cpu.breakpoint(addr)
	cpu["breakpoint"] = [this](u32 addr) {
		CBreakPoints::AddBreakPoint(BREAKPOINT_EE, addr);
		m_installedBreakpoints.insert(addr);
	};

	// cpu.remove_breakpoint(addr)
	cpu["remove_breakpoint"] = [this](u32 addr) {
		CBreakPoints::RemoveBreakPoint(BREAKPOINT_EE, addr);
		m_installedBreakpoints.erase(addr);
	};

	// cpu.capture_stack() — call inside a hook to get the current stack trace
	cpu["capture_stack"] = [](sol::this_state ts) -> sol::table {
		sol::state_view lua(ts);
		LuaStackTrace trace = MemTracker::instance().captureCurrentStackTrace();
		sol::table t = lua.create_table();
		sol::table frames = lua.create_table();
		for (int i = 0; i < trace.depth; i++)
			frames[i + 1] = trace.frames[i];
		t["frames"] = frames;
		t["depth"] = trace.depth;
		return t;
	};
}

// ============================================================
// ui — Widget registration
// ============================================================

void LuaEngine::registerUiBindings()
{
	sol::state_view lua(m_luaState);
	sol::table ui = lua.create_named_table("ui");

	// ui.set_group(name [, description])
	ui["set_group"] = [this](std::string name, sol::optional<std::string> descOpt) {
		m_currentGroup = name;
		if (descOpt && !descOpt->empty())
			m_groupDescriptions[name] = *descOpt;
	};

	// WidgetHandle usertype — returned by widget_* functions
	sol::usertype<LuaWidget> widgetType = lua.new_usertype<LuaWidget>("WidgetHandle", sol::no_constructor);

	widgetType["get"] = [this](sol::this_state ts, LuaWidget& handle) -> sol::object {
		std::lock_guard lock(m_luaMutex);
		LuaWidget* w = findWidget(handle.key);
		if (!w)
			return sol::nil;
		switch (w->type)
		{
			case LuaWidget::Type::Text:
				return sol::make_object(ts.lua_state(), w->stringVal);
			case LuaWidget::Type::Checkbox:
				if (w->options.empty())
					return sol::make_object(ts.lua_state(), w->boolVal);
				return sol::make_object(ts.lua_state(), w->stringVal);
			case LuaWidget::Type::Slider:
				return sol::make_object(ts.lua_state(), w->numberVal);
			case LuaWidget::Type::Button:
				luaL_error(ts.lua_state(), "Cannot get() on a button widget");
				return sol::nil;
		}
		return sol::nil;
	};

	widgetType["set"] = [this](sol::this_state ts, LuaWidget& handle, sol::object val) {
		std::lock_guard lock(m_luaMutex);
		LuaWidget* w = findWidget(handle.key);
		if (!w)
			return;
		switch (w->type)
		{
			case LuaWidget::Type::Text:
				w->stringVal = val.as<std::string>();
				break;
			case LuaWidget::Type::Checkbox:
				if (w->options.empty())
					w->boolVal = val.as<bool>();
				else
				{
					std::string s = val.as<std::string>();
					if (std::find(w->options.begin(), w->options.end(), s) == w->options.end())
					{
						luaL_error(ts.lua_state(), "Value '%s' not in options for widget '%s'", s.c_str(), w->key.c_str());
						return;
					}
					w->stringVal = s;
				}
				break;
			case LuaWidget::Type::Slider:
			{
				double n = val.as<double>();
				if (n < w->sliderMin) n = w->sliderMin;
				if (n > w->sliderMax) n = w->sliderMax;
				w->numberVal = n;
				break;
			}
			case LuaWidget::Type::Button:
				luaL_error(ts.lua_state(), "Cannot set() on a button widget");
				return;
		}
		fireOnChange(*w);
		if (m_widgetsChangedCallback)
			m_widgetsChangedCallback();
	};

	widgetType["set_default"] = [this](sol::this_state ts, LuaWidget& handle, sol::object val) {
		std::lock_guard lock(m_luaMutex);
		LuaWidget* w = findWidget(handle.key);
		if (!w)
			return;
		switch (w->type)
		{
			case LuaWidget::Type::Text:
				w->defaultString = val.as<std::string>();
				break;
			case LuaWidget::Type::Checkbox:
				if (w->options.empty())
					w->defaultBool = val.as<bool>();
				else
					w->defaultString = val.as<std::string>();
				break;
			case LuaWidget::Type::Slider:
				w->defaultNumber = val.as<double>();
				break;
			default:
				break;
		}
	};

	widgetType["set_options"] = [this](sol::this_state ts, LuaWidget& handle, sol::table optTable) {
		std::lock_guard lock(m_luaMutex);
		LuaWidget* w = findWidget(handle.key);
		if (!w)
			return;
		if (w->type != LuaWidget::Type::Checkbox || w->options.empty())
		{
			luaL_error(ts.lua_state(), "set_options() only works on checkbox widgets with options");
			return;
		}
		std::vector<std::string> newOpts;
		for (auto& [k, v] : optTable)
		{
			if (v.is<std::string>())
				newOpts.push_back(v.as<std::string>());
		}
		if (newOpts.empty())
		{
			luaL_error(ts.lua_state(), "set_options() requires at least one option");
			return;
		}
		w->options = std::move(newOpts);
		// If current value is not in new options, reset to first/default
		if (std::find(w->options.begin(), w->options.end(), w->stringVal) == w->options.end())
		{
			if (!w->defaultString.empty() && std::find(w->options.begin(), w->options.end(), w->defaultString) != w->options.end())
				w->stringVal = w->defaultString;
			else
				w->stringVal = w->options[0];
		}
		if (m_widgetsChangedCallback)
			m_widgetsChangedCallback();
	};

	widgetType["on_change"] = [this](LuaWidget& handle, sol::protected_function fn) {
		std::lock_guard lock(m_luaMutex);
		LuaWidget* w = findWidget(handle.key);
		if (!w || w->type == LuaWidget::Type::Button)
			return;
		if (w->onChangeRef != -1)
			luaL_unref(m_luaState, LUA_REGISTRYINDEX, w->onChangeRef);
		fn.push();
		w->onChangeRef = luaL_ref(m_luaState, LUA_REGISTRYINDEX);
	};

	widgetType["on_submit"] = [this](LuaWidget& handle, sol::protected_function fn) {
		std::lock_guard lock(m_luaMutex);
		LuaWidget* w = findWidget(handle.key);
		if (!w || w->type == LuaWidget::Type::Button)
			return;
		// on_submit only meaningful for Text, Slider, and Checkbox with list
		if (w->type == LuaWidget::Type::Checkbox && !w->list)
			return;
		if (w->onSubmitRef != -1)
			luaL_unref(m_luaState, LUA_REGISTRYINDEX, w->onSubmitRef);
		fn.push();
		w->onSubmitRef = luaL_ref(m_luaState, LUA_REGISTRYINDEX);
	};

	// Helper: remove existing widget with same key, unref callbacks
	auto removeExisting = [this](const std::string& key) {
		auto it = std::find_if(m_registeredWidgets.begin(), m_registeredWidgets.end(),
			[&key](const LuaWidget& w) { return w.key == key; });
		if (it != m_registeredWidgets.end())
		{
			if (it->callbackRef != -1)
				luaL_unref(m_luaState, LUA_REGISTRYINDEX, it->callbackRef);
			if (it->onChangeRef != -1)
				luaL_unref(m_luaState, LUA_REGISTRYINDEX, it->onChangeRef);
			if (it->onSubmitRef != -1)
				luaL_unref(m_luaState, LUA_REGISTRYINDEX, it->onSubmitRef);
			m_registeredWidgets.erase(it);
		}
	};

	// ui.widget_text(key, opts) → handle
	ui["widget_text"] = [this, removeExisting](sol::this_state ts, std::string key, sol::table opts) -> LuaWidget {
		std::lock_guard lock(m_luaMutex);
		removeExisting(key);

		LuaWidget w;
		w.key = key;
		w.type = LuaWidget::Type::Text;
		w.label = opts.get_or<std::string>("label", key);
		w.browse = opts.get_or<std::string>("browse", "");
		w.filter = opts.get_or<std::string>("filter", "");
		w.defaultString = opts.get_or<std::string>("default", "");
		w.stringVal = w.defaultString;
		w.source = m_currentGroup;
		m_registeredWidgets.push_back(w);

		if (m_widgetsChangedCallback)
			m_widgetsChangedCallback();
		return w;
	};

	// ui.widget_checkbox(key, opts) → handle
	ui["widget_checkbox"] = [this, removeExisting](sol::this_state ts, std::string key, sol::table opts) -> LuaWidget {
		std::lock_guard lock(m_luaMutex);
		removeExisting(key);

		LuaWidget w;
		w.key = key;
		w.type = LuaWidget::Type::Checkbox;
		w.label = opts.get_or<std::string>("label", key);
		w.dropdown = opts.get_or("dropdown", false);
		w.list = opts.get_or("list", false);
		w.source = m_currentGroup;

		// Check for options
		sol::optional<sol::table> optionsOpt = opts["options"];
		if (optionsOpt)
		{
			for (auto& [k, v] : *optionsOpt)
			{
				if (v.is<std::string>())
					w.options.push_back(v.as<std::string>());
			}
			if (w.options.empty())
			{
				luaL_error(ts.lua_state(), "widget_checkbox: options list must not be empty");
				return w;
			}
			w.defaultString = opts.get_or<std::string>("default", w.options[0]);
			if (std::find(w.options.begin(), w.options.end(), w.defaultString) == w.options.end())
				w.defaultString = w.options[0];
			w.stringVal = w.defaultString;
		}
		else
		{
			w.defaultBool = opts.get_or("default", false);
			w.boolVal = w.defaultBool;
		}

		m_registeredWidgets.push_back(w);
		if (m_widgetsChangedCallback)
			m_widgetsChangedCallback();
		return w;
	};

	// ui.widget_slider(key, opts) → handle
	ui["widget_slider"] = [this, removeExisting](sol::this_state ts, std::string key, sol::table opts) -> LuaWidget {
		std::lock_guard lock(m_luaMutex);
		removeExisting(key);

		sol::optional<double> minOpt = opts["min"];
		sol::optional<double> maxOpt = opts["max"];
		if (!minOpt || !maxOpt)
		{
			luaL_error(ts.lua_state(), "widget_slider: 'min' and 'max' are required");
			return {};
		}

		LuaWidget w;
		w.key = key;
		w.type = LuaWidget::Type::Slider;
		w.label = opts.get_or<std::string>("label", key);
		w.sliderMin = *minOpt;
		w.sliderMax = *maxOpt;
		w.sliderStep = opts.get_or("step", 1.0);
		w.defaultNumber = opts.get_or<double>("default", w.sliderMin);
		// Clamp default
		if (w.defaultNumber < w.sliderMin) w.defaultNumber = w.sliderMin;
		if (w.defaultNumber > w.sliderMax) w.defaultNumber = w.sliderMax;
		w.numberVal = w.defaultNumber;
		w.source = m_currentGroup;
		m_registeredWidgets.push_back(w);

		if (m_widgetsChangedCallback)
			m_widgetsChangedCallback();
		return w;
	};

	// ui.widget_button(key, opts) → handle
	ui["widget_button"] = [this, removeExisting](sol::this_state ts, std::string key, sol::table opts) -> LuaWidget {
		std::lock_guard lock(m_luaMutex);
		removeExisting(key);

		sol::optional<sol::protected_function> cbOpt = opts["callback"];
		if (!cbOpt)
		{
			luaL_error(ts.lua_state(), "widget_button: 'callback' is required");
			return {};
		}

		LuaWidget w;
		w.key = key;
		w.type = LuaWidget::Type::Button;
		w.label = opts.get_or<std::string>("label", key);
		w.source = m_currentGroup;

		cbOpt->push();
		w.callbackRef = luaL_ref(m_luaState, LUA_REGISTRYINDEX);

		m_registeredWidgets.push_back(w);
		if (m_widgetsChangedCallback)
			m_widgetsChangedCallback();
		return w;
	};
}

// ============================================================
// config — Persistent key-value storage
// ============================================================

void LuaEngine::registerConfigBindings()
{
	sol::state_view lua(m_luaState);
	sol::table config = lua.create_named_table("config");

	// config.get(key [, default]) → string or nil
	config["get"] = [](sol::this_state ts, std::string key, sol::optional<std::string> defaultVal) -> sol::object {
		if (Host::ContainsBaseSettingValue("LuaMods", key.c_str()))
			return sol::make_object(ts.lua_state(), Host::GetBaseStringSettingValue("LuaMods", key.c_str()));
		if (defaultVal)
			return sol::make_object(ts.lua_state(), *defaultVal);
		return sol::nil;
	};

	// config.set(key, value) — nil value removes key
	config["set"] = [](sol::this_state /*ts*/, std::string key, sol::optional<std::string> value) {
		if (value)
			Host::SetBaseStringSettingValue("LuaMods", key.c_str(), value->c_str());
		else
			Host::RemoveBaseSettingValue("LuaMods", key.c_str());
		Host::CommitBaseSettingChanges();
	};

	// config.game_get(key [, default]) → string or nil
	config["game_get"] = [this](sol::this_state ts, std::string key, sol::optional<std::string> defaultVal) -> sol::object {
		const u32 crc = VMManager::GetDiscCRC();
		if (crc == 0)
		{
			logMessage(LOG_WARN, "config", "game_get: no game running");
			if (defaultVal)
				return sol::make_object(ts.lua_state(), *defaultVal);
			return sol::nil;
		}
		const std::string serial = VMManager::GetSerialForGameSettings();
		const std::string path = VMManager::GetGameSettingsPath(serial, crc);
		INISettingsInterface si(std::move(path));
		si.Load();
		std::string val;
		if (si.GetStringValue("LuaMods", key.c_str(), &val))
			return sol::make_object(ts.lua_state(), val);
		if (defaultVal)
			return sol::make_object(ts.lua_state(), *defaultVal);
		return sol::nil;
	};

	// config.game_set(key, value)
	config["game_set"] = [this](sol::this_state /*ts*/, std::string key, sol::optional<std::string> value) {
		const u32 crc = VMManager::GetDiscCRC();
		if (crc == 0)
		{
			logMessage(LOG_WARN, "config", "game_set: no game running");
			return;
		}
		const std::string serial = VMManager::GetSerialForGameSettings();
		const std::string path = VMManager::GetGameSettingsPath(serial, crc);
		// Ensure the game settings directory exists before saving
		const std::string dir(Path::GetDirectory(path));
		if (!dir.empty())
			FileSystem::EnsureDirectoryExists(dir.c_str(), false);
		INISettingsInterface si(std::move(path));
		si.Load();
		if (value)
			si.SetStringValue("LuaMods", key.c_str(), value->c_str());
		else
			si.DeleteValue("LuaMods", key.c_str());
		si.Save();
	};
}

// ============================================================
// OSD overlay bindings
// ============================================================

void LuaEngine::cleanupOsdState()
{
	m_osdBlockCmds.clear();
	m_osdCurrentBlockId = -1;
	m_osdFrameCmds.clear();
	m_osdFrameBlockIds.clear();
}

void LuaEngine::registerOsdBindings()
{
	sol::state_view lua(m_luaState);
	sol::table osd = lua.create_named_table("osd");

	// osd.create_block(name, align_h, align_v) -> block_id
	osd["create_block"] = [](sol::this_state ts, std::string name, std::string align_h, std::string align_v) -> int {
		LuaOverlay::Corner corner = LuaOverlay::ParseCorner(align_h, align_v);
		return LuaOverlay::CreateBlock(name, corner);
	};

	// osd.destroy_block(id)
	osd["destroy_block"] = [](int id) {
		LuaOverlay::DestroyBlock(id);
	};

	// osd.set_block_width(id, width) — set fixed width in pixels (0 = auto)
	osd["set_block_width"] = [](int id, float width) {
		LuaOverlay::SetBlockWidth(id, width);
	};

	// osd.begin(block_id) — start buffering commands for a block locally
	osd["begin"] = [this](int blockId) {
		// Detect new frame: if this block ID was already seen, a new frame
		// has started. Clear the frame buffer.
		bool alreadySeen = std::find(m_osdFrameBlockIds.begin(),
			m_osdFrameBlockIds.end(), blockId) != m_osdFrameBlockIds.end();
		if (alreadySeen)
		{
			m_osdFrameCmds.clear();
			m_osdFrameBlockIds.clear();
		}
		m_osdFrameBlockIds.push_back(blockId);

		m_osdBlockCmds.clear();
		m_osdCurrentBlockId = blockId;

		LuaOverlay::DrawCommand cmd;
		cmd.type = LuaOverlay::CmdType::BlockBegin;
		cmd.blockId = blockId;
		m_osdBlockCmds.push_back(std::move(cmd));
	};

	// osd.finish() — append block to frame buffer, then commit atomically
	osd["finish"] = [this]() {
		LuaOverlay::DrawCommand cmd;
		cmd.type = LuaOverlay::CmdType::BlockEnd;
		m_osdBlockCmds.push_back(std::move(cmd));

		// Move block commands into the frame-level buffer
		m_osdFrameCmds.insert(m_osdFrameCmds.end(),
			std::make_move_iterator(m_osdBlockCmds.begin()),
			std::make_move_iterator(m_osdBlockCmds.end()));
		m_osdBlockCmds.clear();

		// Commit the entire frame so far — CommitFrame replaces (not appends),
		// so GS always sees the latest complete set of blocks.
		LuaOverlay::CommitFrame(m_osdFrameCmds);
	};

	// osd.text(str [, color])
	osd["text"] = [this](std::string text, sol::optional<u32> colorOpt) {
		LuaOverlay::DrawCommand cmd;
		cmd.type = LuaOverlay::CmdType::Text;
		cmd.text = std::move(text);
		cmd.color = colorOpt.value_or(0xFFFFFFFF);
		m_osdBlockCmds.push_back(std::move(cmd));
	};

	// osd.separator()
	osd["separator"] = [this]() {
		LuaOverlay::DrawCommand cmd;
		cmd.type = LuaOverlay::CmdType::Separator;
		m_osdBlockCmds.push_back(std::move(cmd));
	};

	// osd.show() / osd.hide() / osd.toggle()
	osd["show"] = []() { LuaOverlay::SetVisible(true); };
	osd["hide"] = []() { LuaOverlay::SetVisible(false); };
	osd["toggle"] = []() { LuaOverlay::ToggleVisible(); };
}
