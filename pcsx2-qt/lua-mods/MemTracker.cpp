// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "MemTracker.h"

#include "common/Console.h"
#include "DebugTools/MemoryTrace.h"
#include "Memory.h"
#include "R5900.h"
#include "VMManager.h"
#include "x86/iR5900.h"

#include <cstring>
#include <sstream>

// --- LuaStackTrace ---

bool LuaStackTrace::containsAddress(u32 addr) const
{
	for (int i = 0; i < depth; i++)
	{
		if (frames[i] == addr)
			return true;
	}
	return false;
}

std::string LuaStackTrace::toString() const
{
	std::ostringstream ss;
	for (int i = 0; i < depth; i++)
	{
		if (i > 0)
			ss << " <- ";
		char buf[16];
		std::snprintf(buf, sizeof(buf), "0x%08x", frames[i]);
		ss << buf;
	}
	return ss.str();
}

std::string LuaStackTrace::toMultilineString() const
{
	std::ostringstream ss;
	for (int i = 0; i < depth; i++)
	{
		char buf[16];
		std::snprintf(buf, sizeof(buf), "0x%08x", frames[i]);
		if (i == 0)
			ss << "    " << buf << "\n";
		else
			ss << "    <- " << buf << "\n";
	}
	return ss.str();
}

// --- MemTracker ---

MemTracker& MemTracker::instance()
{
	static MemTracker s_instance;
	return s_instance;
}

// Fast inline stack capture that scans raw PS2 RAM without going through the
// debug interface virtual dispatch and vtlb safe-read machinery.
// Returns the host pointer for a PS2 address if it maps to plain RAM, else nullptr.
static const u32* ps2RamPtr(u32 ps2addr)
{
	// Strip KSEG mirrors: 0x00xxxxxx, 0x20xxxxxx, 0x80xxxxxx, 0xA0xxxxxx all map to RAM
	u32 phys = ps2addr & 0x1FFFFFFFu;
	if (phys >= Ps2MemSize::ExposedRam)
		return nullptr;
	void* p = PSM(phys);
	return static_cast<const u32*>(p);
}

// Scan backwards from `pc` looking for the function entry (addiu sp,sp,-N prologue).
// Returns the stack frame size and ra-save offset, or false if not found within limit.
// Scan backwards from pc looking for the function prologue.
// Finds frame size (ADDIU/DADDI/DADDIU $sp,$sp,-N) and ra save offset (SW/SD/SQ $ra,imm($sp)).
// Returns true if frame allocation was found.
// If ra was saved on stack: outRaOffset >= 0, read saved ra from sp+outRaOffset.
// If ra was NOT saved (leaf): outRaOffset = -1, caller's ra is in the register.
static bool fastScanPrologue(u32 pc, u32 maxBytes, int& outFrameSize, int& outRaOffset)
{
	const u32 scanWords = maxBytes / 4;
	outFrameSize = 0;
	outRaOffset  = -1;

	for (u32 i = 0; i < scanWords; i++)
	{
		u32 addr = pc - i * 4;
		const u32* p = ps2RamPtr(addr);
		if (!p)
			break;
		u32 instr = *p;

		u32 op = instr >> 26;
		u32 rs  = (instr >> 21) & 0x1F;
		u32 rt  = (instr >> 16) & 0x1F;
		s16 imm = static_cast<s16>(instr & 0xFFFF);

		// JR $ra — previous function boundary, stop scanning
		if (op == 0 && (instr & 0x3F) == 8 && rs == 31)
			break;

		// SW $ra, imm($sp) (op=43) or SD $ra, imm($sp) (op=63) or SQ $ra, imm($sp) (op=31)
		if ((op == 43 || op == 63 || op == 31) && rs == 29 && rt == 31)
			outRaOffset = imm;

		// ADDIU $sp, $sp, -N (op=9) or DADDI/DADDIU $sp, $sp, -N (op=24/25) — frame allocation
		if ((op == 9 || op == 24 || op == 25) && rs == 29 && rt == 29 && imm < 0)
		{
			outFrameSize = -imm;
			return true;
		}
	}
	return false;
}

// Must be called from EE thread only (reads cpuRegs directly).
LuaStackTrace MemTracker::captureCurrentStackTrace() const
{
	LuaStackTrace trace;

	if (!VMManager::HasValidVM())
		return trace;

	u32 pc = cpuRegs.pc;
	u32 ra = cpuRegs.GPR.n.ra.UL[0];
	u32 sp = cpuRegs.GPR.n.sp.UL[0];

	// Frame 0: record hooked pc, advance using $ra register (prologue may not have executed yet).
	// Frame 1+: pc is a return address inside a function whose prologue HAS run,
	//           so we can read saved $ra from the stack.
	for (int i = 0; i < LuaStackTrace::MAX_FRAMES; i++)
	{
		if (pc == 0 || !ps2RamPtr(pc))
			break;

		trace.frames[trace.depth++] = pc;

		int frameSize = 0, raOffset = -1;
		if (!fastScanPrologue(pc, 64 * 1024, frameSize, raOffset))
		{
			// No prologue found. If ra looks valid, record one more frame.
			if (ra != 0 && ra != pc)
			{
				if (trace.depth < LuaStackTrace::MAX_FRAMES)
					trace.frames[trace.depth++] = ra;
			}
			break;
		}

		if (i == 0)
		{
			// First frame: hooked at or near function entry.
			// Prologue may not have executed, so don't read ra from stack.
			// $ra register is the correct return address.
			// Don't advance sp either — it's still the caller's sp.
			pc = ra;
			ra = 0;
		}
		else
		{
			// Subsequent frames: prologue has executed, stack is set up.
			// Read saved ra from stack if this function saved it.
			if (raOffset >= 0)
			{
				const u32* rp = ps2RamPtr(sp + static_cast<u32>(raOffset));
				if (rp)
					ra = *rp;
			}
			// Advance sp past this frame
			pc = ra;
			sp += static_cast<u32>(frameSize);
			ra = 0;
		}
	}

	return trace;
}

void MemTracker::applyAutoTags(TrackedRegion& region)
{
	for (const auto& rule : m_autoTagRules)
	{
		if (region.stack.containsAddress(rule.stackFunction))
		{
			region.tags[rule.key] = rule.value;
		}
	}
}

// --- Region lifecycle ---

void MemTracker::recordAlloc(u32 address, u32 size, u32 parentAddr,
                                const std::string& name, const LuaStackTrace& stack)
{
	std::lock_guard lock(m_mutex);

	auto existing = m_regions.find(address);
	if (existing != m_regions.end())
	{
		flushTrace(existing->second.address);
		Console.Warning("[MemTracker] Overwriting existing allocation at 0x%08x (old size: %u, new size: %u)",
			address, existing->second.size, size);
	}

	TrackedRegion r;
	r.address = address;
	r.size = size;
	r.parentAddr = parentAddr;
	r.name = name;
	r.stack = stack;
	applyAutoTags(r);
	m_regions[address] = std::move(r);
}

void MemTracker::recordFree(u32 address)
{
	std::lock_guard lock(m_mutex);
	if (address == 0)
		return;
	flushTrace(address);
	m_regions.erase(address);
}

void MemTracker::clearAllTracking()
{
	std::lock_guard lock(m_mutex);
	flushAllTracesUnlocked();
	m_regions.clear();
}

// --- Query ---

std::vector<const TrackedRegion*> MemTracker::query(const TrackedRegionQuery& q) const
{
	std::lock_guard lock(m_mutex);
	std::vector<const TrackedRegion*> results;

	for (const auto& [addr, region] : m_regions)
	{
		if (q.exactAddress && region.address != *q.exactAddress)
			continue;

		if (q.containsAddress)
		{
			u32 target = *q.containsAddress;
			if (target < region.address || target >= region.address + region.size)
				continue;
		}

		if (q.stackContainsFunc && !region.stack.containsAddress(*q.stackContainsFunc))
			continue;

		if (q.parentAddr && region.parentAddr != *q.parentAddr)
			continue;

		if (q.minSize && region.size < *q.minSize)
			continue;
		if (q.maxSize && region.size > *q.maxSize)
			continue;

		bool tagsMatch = true;
		for (const auto& [key, value] : q.tags)
		{
			auto it = region.tags.find(key);
			if (it == region.tags.end() || it->second != value)
			{
				tagsMatch = false;
				break;
			}
		}
		if (!tagsMatch)
			continue;

		results.push_back(&region);
	}

	return results;
}

const TrackedRegion* MemTracker::findRegion(u32 address) const
{
	std::lock_guard lock(m_mutex);
	auto it = m_regions.find(address);
	return it != m_regions.end() ? &it->second : nullptr;
}

const TrackedRegion* MemTracker::findContaining(u32 address) const
{
	std::lock_guard lock(m_mutex);
	auto it = m_regions.upper_bound(address);
	if (it != m_regions.begin())
	{
		--it;
		if (address >= it->second.address &&
			address < it->second.address + it->second.size)
		{
			return &it->second;
		}
	}
	return nullptr;
}

// --- Tagging ---

void MemTracker::addTag(u32 address, const std::string& key, const std::string& value)
{
	std::lock_guard lock(m_mutex);
	auto it = m_regions.find(address);
	if (it != m_regions.end())
		it->second.tags[key] = value;
}

void MemTracker::removeTag(u32 address, const std::string& key)
{
	std::lock_guard lock(m_mutex);
	auto it = m_regions.find(address);
	if (it != m_regions.end())
		it->second.tags.erase(key);
}

void MemTracker::addAutoTagRule(const AutoTagRule& rule)
{
	std::lock_guard lock(m_mutex);
	m_autoTagRules.push_back(rule);

	// Apply retroactively to existing regions
	for (auto& [addr, region] : m_regions)
	{
		if (region.stack.containsAddress(rule.stackFunction))
			region.tags[rule.key] = rule.value;
	}
}

// --- Snapshots ---

std::map<u32, TrackedRegion> MemTracker::snapshotRegions() const
{
	std::lock_guard lock(m_mutex);
	return m_regions;
}

// --- Memory Region Tracing ---

void MemTracker::flushTrace(u32 allocAddr)
{
	auto traceIt = m_activeTraces.find(allocAddr);
	if (traceIt == m_activeTraces.end())
		return;

	u32 hookId = traceIt->second;

	auto stats = MemoryTraceManager::Instance().GetHookTraceStats(hookId);

	auto regionIt = m_regions.find(allocAddr);
	std::string resourceName;
	u32 allocSize = 0;
	if (regionIt != m_regions.end())
	{
		auto tagIt = regionIt->second.tags.find("traced");
		if (tagIt != regionIt->second.tags.end())
			resourceName = tagIt->second;
		allocSize = regionIt->second.size;
	}

	if (!stats.empty())
	{
		TraceResult result;
		result.resourceName = resourceName;
		result.allocAddress = allocAddr;
		result.allocSize = allocSize;
		result.stats = std::move(stats);
		m_traceResults.push_back(std::move(result));
		Console.WriteLn("[MemTracker] Flushed trace for '%s' at 0x%08x: %zu unique access patterns",
			resourceName.c_str(), allocAddr, m_traceResults.back().stats.size());
	}

	MemoryTraceManager::Instance().UnregisterHook(hookId);
	m_activeTraces.erase(traceIt);
}

void MemTracker::flushAllTracesUnlocked()
{
	std::vector<u32> addrs;
	addrs.reserve(m_activeTraces.size());
	for (const auto& [addr, _] : m_activeTraces)
		addrs.push_back(addr);

	for (u32 addr : addrs)
		flushTrace(addr);
}

void MemTracker::flushAllTraces()
{
	std::lock_guard lock(m_mutex);
	flushAllTracesUnlocked();
}

std::vector<TraceResult> MemTracker::snapshotTraceResults() const
{
	std::lock_guard lock(m_mutex);
	return m_traceResults;
}

void MemTracker::clearTraceResults()
{
	std::lock_guard lock(m_mutex);
	m_traceResults.clear();
}

// --- Formatting ---

static std::string jsonEscape(const std::string& s)
{
	std::string result;
	result.reserve(s.size());
	for (char c : s)
	{
		switch (c)
		{
			case '"': result += "\\\""; break;
			case '\\': result += "\\\\"; break;
			case '\n': result += "\\n"; break;
			case '\r': result += "\\r"; break;
			case '\t': result += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20)
				{
					char buf[8];
					snprintf(buf, sizeof(buf), "\\u%04x", c);
					result += buf;
				}
				else
					result += c;
				break;
		}
	}
	return result;
}

std::string MemTracker::formatRegionText(const TrackedRegion& r)
{
	std::ostringstream ss;
	char buf[64];
	std::snprintf(buf, sizeof(buf), "0x%08x (size=0x%x", r.address, r.size);
	ss << buf;

	if (r.parentAddr != 0)
	{
		std::snprintf(buf, sizeof(buf), ", parent=0x%08x", r.parentAddr);
		ss << buf;
	}

	if (!r.name.empty())
		ss << ", name=\"" << r.name << "\"";

	for (const auto& [key, value] : r.tags)
		ss << ", " << key << "=" << value;

	ss << ")\n  Stack:\n" << r.stack.toMultilineString();
	return ss.str();
}

std::string MemTracker::formatRegionJson(const TrackedRegion& r)
{
	std::ostringstream ss;
	char buf[64];

	std::snprintf(buf, sizeof(buf), "\"0x%08x\"", r.address);
	ss << "{\"address\":" << buf;
	ss << ",\"size\":" << r.size;

	if (r.parentAddr != 0)
	{
		std::snprintf(buf, sizeof(buf), "\"0x%08x\"", r.parentAddr);
		ss << ",\"parent\":" << buf;
	}

	if (!r.name.empty())
		ss << ",\"name\":\"" << jsonEscape(r.name) << "\"";

	if (!r.tags.empty())
	{
		ss << ",\"tags\":{";
		bool first = true;
		for (const auto& [key, value] : r.tags)
		{
			if (!first)
				ss << ",";
			ss << "\"" << jsonEscape(key) << "\":\"" << jsonEscape(value) << "\"";
			first = false;
		}
		ss << "}";
	}

	ss << ",\"stack\":[";
	for (int i = 0; i < r.stack.depth; i++)
	{
		if (i > 0)
			ss << ",";
		std::snprintf(buf, sizeof(buf), "\"0x%08x\"", r.stack.frames[i]);
		ss << buf;
	}
	ss << "]}";

	return ss.str();
}
