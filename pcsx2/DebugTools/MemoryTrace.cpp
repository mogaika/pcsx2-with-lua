// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "MemoryTrace.h"

#include "common/Console.h"
#include "Memory.h"
#include "R5900.h"

#include <cstring>

static constexpr u32 PAGE_SHIFT = 12;

// Fast-path region array for JIT - each byte represents 64KB
u8 g_memTraceRegions[65536] = {0};

MemoryTraceManager& MemoryTraceManager::Instance()
{
	static MemoryTraceManager instance;
	return instance;
}

StackTrace MemoryTraceManager::CaptureStackTrace(u32 pc)
{
	StackTrace trace = {};
	trace.pcs[0] = (pc != 0) ? pc : cpuRegs.pc;

	// Get RA (return address)
	u32 ra = cpuRegs.GPR.n.ra.UL[0];
	if (ra != 0 && ra >= 0x00100000 && ra < 0x02000000)
		trace.pcs[1] = ra;

	// Try to walk back further using SP
	u32 sp = cpuRegs.GPR.n.sp.UL[0];
	if (sp >= 0x00100000 && sp < 0x02000000)
	{
		static const u32 offsets[] = {0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c};

		for (int i = 2; i < 4; i++)
		{
			bool found = false;
			for (u32 offset : offsets)
			{
				u32 stackAddr = sp + offset + (i - 2) * 0x30;
				if (stackAddr >= 0x02000000)
					break;

				u32 possibleRA = memRead32(stackAddr);
				if (possibleRA >= 0x00100000 && possibleRA < 0x02000000 && (possibleRA & 3) == 0)
				{
					trace.pcs[i] = possibleRA;
					found = true;
					break;
				}
			}
			if (!found)
				break;
		}
	}

	return trace;
}

u32 MemoryTraceManager::RegisterHook(const std::string& hookName, MemTraceHookCallback callback,
	u32 flags, void* userData)
{
	std::unique_lock lock(m_mutex);

	u32 hookId = m_nextHookId++;

	HookTrace hook;
	hook.hookId = hookId;
	hook.hookName = hookName;
	hook.flags = flags;
	hook.callback = std::move(callback);
	hook.userData = userData;

	m_hooks[hookId] = std::move(hook);

	Console.WriteLn("[MemTrace] Registered hook %u: '%s'", hookId, hookName.c_str());
	return hookId;
}

void MemoryTraceManager::AddTracedRange(u32 hookId, u32 start, u32 size)
{
	std::unique_lock lock(m_mutex);

	auto it = m_hooks.find(hookId);
	if (it == m_hooks.end())
		return;

	TracedRange range;
	range.start = start;
	range.end = start + size;
	it->second.ranges.push_back(range);

	UpdateTracedPages();

	Console.WriteLn("[MemTrace] Hook %u: added range 0x%08X-0x%08X (%zu ranges)",
		hookId, start, start + size, it->second.ranges.size());
}

void MemoryTraceManager::RemoveTracedRange(u32 hookId, u32 start)
{
	std::unique_lock lock(m_mutex);

	auto it = m_hooks.find(hookId);
	if (it == m_hooks.end())
		return;

	auto& ranges = it->second.ranges;
	for (auto rangeIt = ranges.begin(); rangeIt != ranges.end(); ++rangeIt)
	{
		if (rangeIt->start == start)
		{
			ranges.erase(rangeIt);
			break;
		}
	}

	UpdateTracedPages();
}

void MemoryTraceManager::FlushHook(u32 hookId)
{
	std::unique_lock lock(m_mutex);

	auto it = m_hooks.find(hookId);
	if (it == m_hooks.end())
		return;

	HookTrace& hook = it->second;

	if (hook.callback && !hook.stats.empty())
		hook.callback(hook);

	hook.ranges.clear();
	hook.stats.clear();
	hook.firstWritePC = 0;
	hook.completed = false;

	UpdateTracedPages();
}

void MemoryTraceManager::UnregisterHook(u32 hookId)
{
	std::unique_lock lock(m_mutex);

	auto it = m_hooks.find(hookId);
	if (it == m_hooks.end())
		return;

	Console.WriteLn("[MemTrace] Unregistered hook %u: '%s'", hookId, it->second.hookName.c_str());
	m_hooks.erase(it);

	UpdateTracedPages();
}

void MemoryTraceManager::ClearAllTraces()
{
	std::unique_lock lock(m_mutex);
	m_hooks.clear();
	m_tracedPages.clear();
	std::memset(g_memTraceRegions, 0, sizeof(g_memTraceRegions));
}

void MemoryTraceManager::OnMemoryRead(u32 addr, u32 size, u32 pc)
{
	ProcessAccess(addr, size, pc, false);
}

void MemoryTraceManager::OnMemoryWrite(u32 addr, u32 size, u32 pc)
{
	ProcessAccess(addr, size, pc, true);
}

bool MemoryTraceManager::HasActiveTraces() const
{
	return !m_tracedPages.empty();
}

std::map<TraceKey, u32> MemoryTraceManager::GetHookTraceStats(u32 hookId) const
{
	std::unique_lock lock(m_mutex);

	auto it = m_hooks.find(hookId);
	if (it == m_hooks.end())
		return {};

	return it->second.stats;
}

void MemoryTraceManager::UpdateTracedPages()
{
	m_tracedPages.clear();
	std::memset(g_memTraceRegions, 0, sizeof(g_memTraceRegions));

	for (const auto& [id, hook] : m_hooks)
	{
		if (hook.completed)
			continue;

		for (const auto& range : hook.ranges)
		{
			// Update page set
			u32 startPage = range.start >> PAGE_SHIFT;
			u32 endPage = (range.end - 1) >> PAGE_SHIFT;
			for (u32 page = startPage; page <= endPage; ++page)
				m_tracedPages.insert(page);

			// Update region array
			for (u32 r = range.start >> 16; r <= ((range.end - 1) >> 16); ++r)
				g_memTraceRegions[r] = 1;
		}
	}
}

void MemoryTraceManager::ProcessAccess(u32 addr, u32 size, u32 pc, bool isWrite)
{
	std::unique_lock lock(m_mutex);

	// Fast page check
	u32 page = addr >> PAGE_SHIFT;
	if (m_tracedPages.count(page) == 0)
		return;

	u32 accessEnd = addr + size;

	for (auto& [id, hook] : m_hooks)
	{
		if (hook.completed)
			continue;

		// Find which range this access falls into
		for (const auto& range : hook.ranges)
		{
			if (addr >= range.end || accessEnd <= range.start)
				continue;

			// Access overlaps this range
			if (isWrite)
			{
				if (hook.flags & MEMTRACE_STOP_ON_WRITE)
				{
					hook.firstWritePC = pc;
					hook.completed = true;

					if (hook.callback)
						hook.callback(hook);

					hook.ranges.clear();
					hook.stats.clear();
					hook.firstWritePC = 0;
					hook.completed = false;
					UpdateTracedPages();
					return;
				}
				else if (hook.flags & MEMTRACE_TRACK_WRITES)
				{
					TraceKey key;
					key.offset = addr - range.start;
					key.stack = CaptureStackTrace(pc);
					hook.stats[key]++;
				}
			}
			else if (hook.flags & MEMTRACE_TRACK_READS)
			{
				TraceKey key;
				key.offset = addr - range.start;
				key.stack = CaptureStackTrace(pc);
				hook.stats[key]++;
			}
			break;  // Only count once per hook
		}
	}
}
