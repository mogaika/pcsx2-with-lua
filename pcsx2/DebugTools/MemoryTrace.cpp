// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "MemoryTrace.h"

#include "common/Console.h"

static constexpr u32 PAGE_SHIFT = 12;

MemoryTraceManager& MemoryTraceManager::Instance()
{
	static MemoryTraceManager instance;
	return instance;
}

u32 MemoryTraceManager::AddTrace(u32 start, u32 size, const std::string& description,
	MemTraceResultCallback callback,
	u32 flags,
	void* userData)
{
	std::unique_lock lock(m_mutex);

	u32 traceId = m_nextTraceId++;

	MemoryTraceRegion region;
	region.id = traceId;
	region.start = start;
	region.end = start + size;
	region.flags = flags;
	region.firstWritePC = 0;
	region.completed = false;
	region.callback = std::move(callback);
	region.userData = userData;
	region.description = description;

	m_traces[traceId] = std::move(region);
	UpdateTracedPages();

	Console.WriteLn("[MemTrace] Added trace %u: 0x%08X-0x%08X (%s) - %zu pages tracked, page=0x%05X",
		traceId, start, start + size, description.c_str(), m_tracedPages.size(), start >> PAGE_SHIFT);

	return traceId;
}

void MemoryTraceManager::RemoveTrace(u32 traceId)
{
	std::unique_lock lock(m_mutex);

	auto it = m_traces.find(traceId);
	if (it != m_traces.end())
	{
		Console.WriteLn("[MemTrace] Removed trace %u: %s", traceId, it->second.description.c_str());
		m_traces.erase(it);
		UpdateTracedPages();
	}
}

void MemoryTraceManager::ClearAllTraces()
{
	std::unique_lock lock(m_mutex);

	m_traces.clear();
	m_tracedPages.clear();
	Console.WriteLn("[MemTrace] Cleared all traces");
}

void MemoryTraceManager::OnMemoryRead(u32 addr, u32 size, u32 pc)
{
	ProcessAccess(addr, size, pc, false);
}

void MemoryTraceManager::OnMemoryWrite(u32 addr, u32 size, u32 pc)
{
	ProcessAccess(addr, size, pc, true);
}

void MemoryTraceManager::FlushTrace(u32 traceId)
{
	std::unique_lock lock(m_mutex);

	auto it = m_traces.find(traceId);
	if (it != m_traces.end())
	{
		CompleteTrace(it->second);
		m_traces.erase(it);
		UpdateTracedPages();
	}
}

void MemoryTraceManager::FlushAllTraces()
{
	std::unique_lock lock(m_mutex);

	for (auto& [id, region] : m_traces)
	{
		if (!region.completed)
		{
			CompleteTrace(region);
		}
	}

	m_traces.clear();
	m_tracedPages.clear();
	Console.WriteLn("[MemTrace] Flushed all traces");
}

bool MemoryTraceManager::HasActiveTraces() const
{
	// Note: Not thread-safe but acceptable for fast-path check
	// The actual operations acquire the mutex
	return !m_tracedPages.empty();
}

size_t MemoryTraceManager::GetActiveTraceCount() const
{
	std::unique_lock lock(m_mutex);
	return m_traces.size();
}

bool MemoryTraceManager::IsPageTraced(u32 addr) const
{
	u32 page = addr >> PAGE_SHIFT;
	// Note: Not thread-safe but acceptable for fast-path check
	return m_tracedPages.count(page) > 0;
}

// Debug function to dump current trace state
void MemoryTraceManager::DumpTraceState() const
{
	std::unique_lock lock(m_mutex);
	Console.WriteLn("[MemTrace] === Trace State Dump ===");
	Console.WriteLn("[MemTrace] Active traces: %zu, Traced pages: %zu", m_traces.size(), m_tracedPages.size());
	for (const auto& [id, region] : m_traces)
	{
		Console.WriteLn("[MemTrace]   Trace %u: 0x%08X-0x%08X '%s' completed=%d reads=%zu",
			id, region.start, region.end, region.description.c_str(), region.completed, region.readInfo.size());
	}
	Console.WriteLn("[MemTrace] Traced pages:");
	for (u32 page : m_tracedPages)
	{
		Console.WriteLn("[MemTrace]   Page 0x%05X (addr range 0x%08X-0x%08X)", page, page << PAGE_SHIFT, ((page + 1) << PAGE_SHIFT) - 1);
	}
	Console.WriteLn("[MemTrace] === End Dump ===");
}

void MemoryTraceManager::UpdateTracedPages()
{
	m_tracedPages.clear();

	for (const auto& [id, region] : m_traces)
	{
		if (region.completed)
			continue;

		u32 startPage = region.start >> PAGE_SHIFT;
		u32 endPage = (region.end - 1) >> PAGE_SHIFT;

		for (u32 page = startPage; page <= endPage; ++page)
		{
			m_tracedPages.insert(page);
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

	// Find which region(s) this access falls into
	std::vector<u32> completedTraces;

	for (auto& [id, region] : m_traces)
	{
		if (region.completed)
			continue;

		// Check if any byte of the access falls within the region
		u32 accessEnd = addr + size;
		if (addr >= region.end || accessEnd <= region.start)
			continue;

		// Access overlaps with this region
		if (isWrite)
		{
			// Handle write access
			if (region.flags & MEMTRACE_STOP_ON_WRITE)
			{
				// Stop and report on first write
				region.firstWritePC = pc;
				region.completed = true;
				completedTraces.push_back(id);

				// Always log when trace is stopped due to write
				Console.WriteLn("[MemTrace] Trace %u '%s' STOPPED: Write at 0x%08X from PC=0x%08X (region 0x%08X-0x%08X, %zu reads tracked)",
					id, region.description.c_str(), addr, pc, region.start, region.end, region.readInfo.size());
			}
			else if (region.flags & MEMTRACE_TRACK_WRITES)
			{
				// Track the write
				for (u32 a = addr; a < accessEnd && a < region.end; ++a)
				{
					if (a >= region.start)
					{
						auto& info = region.writeInfo[a];
						info.count++;
						info.pcCounts[pc]++;
					}
				}

				if (region.flags & MEMTRACE_LOG_EACH_ACCESS)
				{
					Console.WriteLn("[MemTrace] Write to trace %u at 0x%08X (size %u) from PC=0x%08X",
						id, addr, size, pc);
				}
			}
		}
		else
		{
			// Handle read access
			if (region.flags & MEMTRACE_TRACK_READS)
			{
				for (u32 a = addr; a < accessEnd && a < region.end; ++a)
				{
					if (a >= region.start)
					{
						auto& info = region.readInfo[a];
						info.count++;
						info.pcCounts[pc]++;
					}
				}

				if (region.flags & MEMTRACE_LOG_EACH_ACCESS)
				{
					Console.WriteLn("[MemTrace] Read from trace %u at 0x%08X (size %u) from PC=0x%08X",
						id, addr, size, pc);
				}
			}
		}
	}

	// Process completed traces (call callbacks and remove)
	for (u32 traceId : completedTraces)
	{
		auto it = m_traces.find(traceId);
		if (it != m_traces.end())
		{
			CompleteTrace(it->second);
			m_traces.erase(it);
		}
	}

	if (!completedTraces.empty())
	{
		UpdateTracedPages();
	}
}

MemoryTraceRegion* MemoryTraceManager::FindRegionForAddress(u32 addr)
{
	for (auto& [id, region] : m_traces)
	{
		if (!region.completed && addr >= region.start && addr < region.end)
		{
			return &region;
		}
	}
	return nullptr;
}

void MemoryTraceManager::CompleteTrace(MemoryTraceRegion& region)
{
	Console.WriteLn("[MemTrace] Completing trace %u: %s (reads: %zu addresses, writes: %zu addresses)",
		region.id, region.description.c_str(), region.readInfo.size(), region.writeInfo.size());

	if (region.callback)
	{
		region.callback(region.start, region.end, region.readInfo, region.writeInfo, region.firstWritePC, region.userData);
	}

	region.completed = true;
}
