// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class DebugServer
{
public:
#ifdef _WIN32
	using socket_t = uintptr_t; // SOCKET is UINT_PTR on Windows
	static constexpr socket_t INVALID_SOCK = ~static_cast<socket_t>(0);
#else
	using socket_t = int;
	static constexpr socket_t INVALID_SOCK = -1;
#endif

	static DebugServer& instance();

	bool start(u16 port = 0);
	void stop();
	bool isRunning() const { return m_running.load(); }

	// Breakpoint tracking — command handlers in .cpp need these
	struct MemWatchEntry { u32 start; u32 size; };

	void trackBreakpoint(u32 addr);
	void untrackBreakpoint(u32 addr);
	void trackMemWatch(u32 start, u32 size);
	void untrackMemWatch(u32 start, u32 size);
	std::vector<u32> getTrackedBreakpoints();
	std::vector<MemWatchEntry> getTrackedMemWatches();

private:
	static constexpr u16 DEFAULT_PORT = 16767;

	void serverLoop();
	void handleClient(socket_t clientFd);
	std::string dispatchCommand(const std::string& jsonLine);

	std::thread m_thread;
	std::atomic_bool m_running{false};
	socket_t m_serverFd = INVALID_SOCK;
	u16 m_port = 0;

	// Breakpoints installed by this server for cleanup
	std::mutex m_bpMutex;
	std::vector<u32> m_breakpoints;
	std::vector<MemWatchEntry> m_memWatches;
};
