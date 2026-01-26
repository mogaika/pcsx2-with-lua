// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Trace options flags
enum MemTraceFlags : u32
{
	MEMTRACE_TRACK_READS = 0x01,     // Track read accesses (default)
	MEMTRACE_TRACK_WRITES = 0x02,    // Track write accesses
	MEMTRACE_STOP_ON_WRITE = 0x04,   // Stop trace and report when ANY write occurs to region
	MEMTRACE_LOG_EACH_ACCESS = 0x08, // Log each access as it happens (verbose)

	// Common combinations
	MEMTRACE_DEFAULT = MEMTRACE_TRACK_READS,
	MEMTRACE_TRACK_UNTIL_REUSED = MEMTRACE_TRACK_READS | MEMTRACE_STOP_ON_WRITE,
};

struct MemoryAccessInfo
{
	u32 count;                  // Total access count
	std::map<u32, u32> pcCounts; // PC -> count (which code locations accessed this address)
};

// Forward declaration for callback type
struct MemoryTraceRegion;

// Callback type for trace results
// Parameters: start, end, readInfo, writeInfo, firstWritePC, userData
using MemTraceResultCallback = std::function<void(
	u32 start,
	u32 end,
	const std::map<u32, MemoryAccessInfo>& readInfo,
	const std::map<u32, MemoryAccessInfo>& writeInfo,
	u32 firstWritePC,
	void* userData)>;

struct MemoryTraceRegion
{
	u32 id;
	u32 start;
	u32 end;
	u32 flags;
	std::map<u32, MemoryAccessInfo> readInfo;  // address -> read info with PC tracking
	std::map<u32, MemoryAccessInfo> writeInfo; // address -> write info (if MEMTRACE_TRACK_WRITES)
	u32 firstWritePC;                          // PC when first write detected
	bool completed;                            // Trace is done (write detected with STOP_ON_WRITE)
	MemTraceResultCallback callback;
	void* userData;
	std::string description;
};

class MemoryTraceManager
{
public:
	static MemoryTraceManager& Instance();

	// Add a trace region (returns trace ID)
	// Can be called from any execution hook to add traces dynamically
	u32 AddTrace(u32 start, u32 size, const std::string& description,
		MemTraceResultCallback callback,
		u32 flags = MEMTRACE_DEFAULT,
		void* userData = nullptr);

	// Remove trace by ID (stops tracing, does NOT trigger callback)
	void RemoveTrace(u32 traceId);

	// Remove all traces
	void ClearAllTraces();

	// Called from memory access points - tracks both address and reading PC
	void OnMemoryRead(u32 addr, u32 size, u32 pc);
	void OnMemoryWrite(u32 addr, u32 size, u32 pc);

	// Manual flush - triggers callback with current results, then removes trace
	void FlushTrace(u32 traceId);
	void FlushAllTraces();

	// Query
	bool HasActiveTraces() const;
	size_t GetActiveTraceCount() const;

	// Check if an address is in any traced page (fast path)
	bool IsPageTraced(u32 addr) const;

	// Debug: dump current trace state to console
	void DumpTraceState() const;

private:
	MemoryTraceManager() = default;
	~MemoryTraceManager() = default;
	MemoryTraceManager(const MemoryTraceManager&) = delete;
	MemoryTraceManager& operator=(const MemoryTraceManager&) = delete;

	void UpdateTracedPages();
	void ProcessAccess(u32 addr, u32 size, u32 pc, bool isWrite);
	MemoryTraceRegion* FindRegionForAddress(u32 addr);
	void CompleteTrace(MemoryTraceRegion& region);

	mutable std::mutex m_mutex;
	std::map<u32, MemoryTraceRegion> m_traces; // traceId -> region
	std::unordered_set<u32> m_tracedPages;     // page = addr >> 12
	u32 m_nextTraceId = 1;
};
