// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "LuaOverlay.h"

#include "common/Pcsx2Defs.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Forward declare lua_State to avoid including sol2 in the header
struct lua_State;

class LuaEngine
{
public:
	static LuaEngine& instance();

	LuaEngine();
	~LuaEngine();

	// Script lifecycle
	bool loadScript(const std::string& path);
	bool reloadScript();
	void stopScript();
	bool executeString(const std::string& code);
	bool executeString(const std::string& code, std::string& returnValue);
	bool runFile(const std::string& path);

	bool isLoaded() const { std::lock_guard lock(m_luaMutex); return m_luaState != nullptr; }
	std::string scriptPath() const { std::lock_guard lock(m_luaMutex); return m_scriptPath; }
	std::vector<std::string> runFiles() const { std::lock_guard lock(m_luaMutex); return m_runFiles; }

	// Persistent error list (survives log drains, cleared on load/stop/reset)
	std::vector<std::string> scriptErrors() const { std::lock_guard lock(m_luaMutex); return m_scriptErrors; }

	// Called from EE thread when a hooked address is hit
	void dispatchHook(u32 addr);

	// Logging callback — set by LuaModWindow to route output to the UI
	using LogCallback = std::function<void(int level, const std::string& system, const std::string& msg)>;
	void setLogCallback(LogCallback cb) { std::lock_guard lock(m_luaMutex); m_logCallback = std::move(cb); }

	// Log levels
	static constexpr int LOG_INFO = 0;
	static constexpr int LOG_WARN = 1;
	static constexpr int LOG_ERROR = 2;

	// Log ring buffer for MCP consumption
	struct LogEntry
	{
		int level;
		std::string system;
		std::string msg;
	};
	std::vector<LogEntry> drainLogs();

	// Registered widgets (Lua scripts declare UI widgets)
	struct LuaWidget
	{
		std::string key;
		std::string label;
		std::string source;      // group name set by ui.set_group()
		enum class Type { Text, Checkbox, Slider, Button } type = Type::Text;
		// Text options
		std::string browse;      // "", "dir", "file", "save"
		std::string filter;      // file filter for browse="file"/"save"
		// Checkbox options
		std::vector<std::string> options;  // empty = bool toggle, non-empty = radio/dropdown/list
		bool dropdown = false;
		bool list = false;
		// Slider options
		double sliderMin = 0;
		double sliderMax = 100;
		double sliderStep = 1;
		// Current value (depends on type)
		std::string stringVal;   // text, checkbox-with-options
		bool boolVal = false;    // checkbox bool mode
		double numberVal = 0;    // slider
		// Defaults
		std::string defaultString;
		bool defaultBool = false;
		double defaultNumber = 0;
		// Callbacks (Lua registry refs)
		int callbackRef = -1;    // button callback
		int onChangeRef = -1;    // on_change callback (any widget)
		int onSubmitRef = -1;    // on_submit callback (text: Enter, list: double-click, slider: release)
	};
	std::vector<LuaWidget> snapshotWidgets() const;

	// Called from Qt side when user changes a widget value
	void setWidgetStringValue(const std::string& key, const std::string& value);
	void setWidgetBoolValue(const std::string& key, bool value);
	void setWidgetNumberValue(const std::string& key, double value);
	void widgetButtonClicked(const std::string& key);
	// Submit events (Enter for text, double-click for list, release for slider)
	void widgetSubmit(const std::string& key);

	// Notification when widget list changes
	using WidgetsChangedCallback = std::function<void()>;
	void setWidgetsChangedCallback(WidgetsChangedCallback cb) { std::lock_guard lock(m_luaMutex); m_widgetsChangedCallback = std::move(cb); }

	// Group descriptions
	std::map<std::string, std::string> snapshotGroupDescriptions() const;

private:
	// Lua state setup
	void createState();
	void destroyState();
	void registerBindings();

	// Binding registration helpers (implemented in .cpp)
	void registerMemBindings();
	void registerRegBindings();
	void registerEmuBindings();
	void registerLogBindings();
	void registerCpuBindings();
	void registerUiBindings();
	void registerConfigBindings();
	void registerOsdBindings();

	// Cleanup all resources created by the script
	void cleanupHooks();
	void cleanupTraces();
	void cleanupBreakpoints();
	void cleanupWidgets();
	void cleanupOsdState();

	// Static hook dispatch trampoline
	static void hookTrampoline();

	// Widget helpers
	LuaWidget* findWidget(const std::string& key);
	void fireOnChange(LuaWidget& w);
	void fireOnSubmit(LuaWidget& w);

	// Internal logging
	void logMessage(int level, const std::string& system, const std::string& msg);

	lua_State* m_luaState = nullptr;
	std::string m_scriptPath;
	std::vector<std::string> m_runFiles;
	std::vector<std::string> m_scriptErrors;

	// Per-hook Lua callback refs
	struct LuaHookEntry
	{
		int luaRef = -1;  // Lua registry ref to the callback function
		bool oneShot = false;
	};
	std::map<u32, std::vector<LuaHookEntry>> m_luaHooks;
	std::unordered_set<u32> m_installedHookAddresses;

	// Active trace hookIds for cleanup
	std::vector<u32> m_activeTraceHookIds;

	// Installed breakpoints/memchecks for cleanup
	std::unordered_set<u32> m_installedBreakpoints;
	struct MemCheckEntry { u32 start; u32 size; };
	std::vector<MemCheckEntry> m_installedMemChecks;

	// Registered widgets
	std::vector<LuaWidget> m_registeredWidgets;
	WidgetsChangedCallback m_widgetsChangedCallback;

	// Current group name for ui.set_group()
	std::string m_currentGroup;
	std::map<std::string, std::string> m_groupDescriptions;

	// OSD per-block command staging buffer (flushed to frame buffer on osd.finish)
	std::vector<LuaOverlay::DrawCommand> m_osdBlockCmds;
	int m_osdCurrentBlockId = -1;      // block ID set by osd.begin()
	// OSD frame command buffer — all blocks for the current frame, committed atomically
	std::vector<LuaOverlay::DrawCommand> m_osdFrameCmds;
	std::vector<int> m_osdFrameBlockIds; // block IDs seen this frame, for new-frame detection

	mutable std::recursive_mutex m_luaMutex;
	LogCallback m_logCallback;

	// Log ring buffer
	std::mutex m_logBufMutex;
	std::vector<LogEntry> m_logBuffer;
	static constexpr size_t MAX_LOG_BUFFER = 1000;

	// Singleton pointer for static trampoline
	static LuaEngine* s_instance;
};
