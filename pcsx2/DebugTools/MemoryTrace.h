// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Trace options flags
enum MemTraceFlags : u32
{
	MEMTRACE_TRACK_READS = 0x01,
	MEMTRACE_TRACK_WRITES = 0x02,
	MEMTRACE_STOP_ON_WRITE = 0x04,

	MEMTRACE_DEFAULT = MEMTRACE_TRACK_READS,
};

// Stack trace structure - captures exactly 4 PCs in call chain
struct StackTrace
{
	std::array<u32, 4> pcs = {0, 0, 0, 0};

	bool operator==(const StackTrace& other) const { return pcs == other.pcs; }
	bool operator<(const StackTrace& other) const { return pcs < other.pcs; }
};

// Key for tracking unique (offset, stack) pairs
struct TraceKey
{
	u32 offset = 0;
	StackTrace stack;

	bool operator<(const TraceKey& other) const
	{
		if (offset != other.offset)
			return offset < other.offset;
		return stack < other.stack;
	}
};

// A single address range within a hook's trace
struct TracedRange
{
	u32 start = 0;
	u32 end = 0;
};

// Forward declaration for callback type
struct HookTrace;

// Callback type for hook trace results
using MemTraceHookCallback = std::function<void(const HookTrace& trace)>;

// Per-hook trace data
struct HookTrace
{
	u32 hookId = 0;
	std::string hookName;
	u32 flags = 0;
	std::vector<TracedRange> ranges;
	std::map<TraceKey, u32> stats;  // (offset, stack) -> count
	u32 firstWritePC = 0;
	bool completed = false;
	MemTraceHookCallback callback;
	void* userData = nullptr;
};

// Fast-path region array for JIT - each byte represents 64KB
// Non-zero = region has active traces
// Index: addr >> 16 (covers full 32-bit address space, 64KB array)
extern u8 g_memTraceRegions[65536];

class MemoryTraceManager
{
public:
	static MemoryTraceManager& Instance();

	// Register a hook and get hookId
	u32 RegisterHook(const std::string& hookName, MemTraceHookCallback callback,
		u32 flags = MEMTRACE_DEFAULT, void* userData = nullptr);

	// Add a traced range to an existing hook
	void AddTracedRange(u32 hookId, u32 start, u32 size);

	// Remove a specific range from hook
	void RemoveTracedRange(u32 hookId, u32 start);

	// Flush hook - triggers callback, clears stats
	void FlushHook(u32 hookId);

	// Unregister hook entirely
	void UnregisterHook(u32 hookId);

	// Clear all hooks
	void ClearAllTraces();

	// Called from memory access points
	void OnMemoryRead(u32 addr, u32 size, u32 pc);
	void OnMemoryWrite(u32 addr, u32 size, u32 pc);

	// Query
	bool HasActiveTraces() const;

	// Get trace stats for UI display (returns copy for thread safety)
	std::map<TraceKey, u32> GetHookTraceStats(u32 hookId) const;

	// Capture current 4-frame stack trace
	static StackTrace CaptureStackTrace(u32 pc = 0);

private:
	MemoryTraceManager() = default;
	~MemoryTraceManager() = default;
	MemoryTraceManager(const MemoryTraceManager&) = delete;
	MemoryTraceManager& operator=(const MemoryTraceManager&) = delete;

	void UpdateTracedPages();
	void ProcessAccess(u32 addr, u32 size, u32 pc, bool isWrite);

	mutable std::mutex m_mutex;
	std::map<u32, HookTrace> m_hooks;
	std::unordered_set<u32> m_tracedPages;
	u32 m_nextHookId = 1;
};
