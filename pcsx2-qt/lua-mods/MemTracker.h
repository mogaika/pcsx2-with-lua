// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "DebugTools/MemoryTrace.h"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct LuaStackTrace
{
	static constexpr int MAX_FRAMES = 8;
	u32 frames[MAX_FRAMES] = {};
	int depth = 0;

	bool containsAddress(u32 addr) const;
	std::string toString() const;
	std::string toMultilineString() const;
};

struct TrackedRegion
{
	u32 address = 0;
	u32 size = 0;
	u32 parentAddr = 0;          // optional parent region address (0 = none)
	std::string name;            // optional human-readable label
	LuaStackTrace stack;         // optional stack trace at creation time
	std::map<std::string, std::string> tags;
};

struct TrackedRegionQuery
{
	std::optional<u32> containsAddress;
	std::optional<u32> exactAddress;
	std::optional<u32> stackContainsFunc;
	std::optional<u32> parentAddr;
	std::map<std::string, std::string> tags;
	std::optional<u32> minSize;
	std::optional<u32> maxSize;
};

struct AutoTagRule
{
	u32 stackFunction = 0;
	std::string key;
	std::string value;
};

// Result of a flushed memory trace
struct TraceResult
{
	std::string resourceName;
	u32 allocAddress = 0;
	u32 allocSize = 0;
	std::vector<TraceEntry> stats; // in first-seen order
};


class MemTracker
{
public:
	static MemTracker& instance();

	// Stack capture
	LuaStackTrace captureCurrentStackTrace() const;

	// Region lifecycle — called from Lua
	void recordAlloc(u32 address, u32 size, u32 parentAddr,
	                 const std::string& name, const LuaStackTrace& stack);
	void recordFree(u32 address);

	// Clear all tracking state
	void clearAllTracking();

	// Query / find
	std::vector<const TrackedRegion*> query(const TrackedRegionQuery& q) const;
	const TrackedRegion* findRegion(u32 address) const;
	const TrackedRegion* findContaining(u32 address) const;

	// Tagging
	void addTag(u32 address, const std::string& key, const std::string& value);
	void removeTag(u32 address, const std::string& key);
	void addAutoTagRule(const AutoTagRule& rule);

	// Snapshots for UI
	std::map<u32, TrackedRegion> snapshotRegions() const;

	// Memory region tracing (unchanged)
	void flushAllTraces();
	std::vector<TraceResult> snapshotTraceResults() const;
	void clearTraceResults();

	// Stats
	size_t regionCount() const { std::lock_guard lock(m_mutex); return m_regions.size(); }

	// Formatting
	static std::string formatRegionText(const TrackedRegion& r);
	static std::string formatRegionJson(const TrackedRegion& r);

private:
	MemTracker() = default;

	// Apply auto-tag rules to a region
	void applyAutoTags(TrackedRegion& region);

	// Flush a single active trace and store result (caller must hold m_mutex)
	void flushTrace(u32 allocAddr);

	// Flush all traces without locking (caller must hold m_mutex)
	void flushAllTracesUnlocked();

	// Tracking data
	std::map<u32, TrackedRegion> m_regions;     // address -> region

	// Thread safety
	mutable std::mutex m_mutex;

	// Auto-tag rules
	std::vector<AutoTagRule> m_autoTagRules;

	// Memory region tracing
	std::map<u32, u32> m_activeTraces;               // alloc addr -> MemoryTraceManager hookId
	std::vector<TraceResult> m_traceResults;      // flushed trace results
};
