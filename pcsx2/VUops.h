// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "VU.h"
#include "VUflags.h"

#include <vector>
#include <mutex>
#include <functional>
#include <cstdio>

struct VU1TraceEntry {
	u32 vifCmd;         // raw VIFcode word
	u32 vifAddr;        // PS2 address of this VIFcode word in EE memory
	u32 dmaAddr;        // vif1ch.madr at time of this command
};

// Callback to resolve EE address to a human-readable zone description (name + tags).
// Set by pcsx2-qt MemTracker integration. Returns empty string if no zone found.
using VU1TraceZoneLookupFn = std::function<std::string(u32 eeAddr)>;

enum VU1TraceFlags : u32
{
	VU1_TRACE_VIF       = 1 << 0, // VIF command log
	VU1_TRACE_REGS      = 1 << 1, // pre/post execution registers
	VU1_TRACE_MEMORY    = 1 << 2, // VU1 data memory dump
	VU1_TRACE_PC_HITS   = 1 << 3, // per-PC hit counts
	VU1_TRACE_ALL       = VU1_TRACE_VIF | VU1_TRACE_REGS | VU1_TRACE_MEMORY | VU1_TRACE_PC_HITS,
};

struct VU1TraceState {
	std::mutex mutex;            // protects file access from MTVU thread
	bool armed = false;          // recording enabled
	bool active = false;         // currently inside a trace frame
	FILE* file = nullptr;        // output file
	int execCount = 0;           // number of MSCAL executions recorded
	u32 flags = VU1_TRACE_ALL;   // which sections to include
	std::vector<VU1TraceEntry> vifLog;          // accumulated on EE thread between MSCALs
	std::vector<std::vector<VU1TraceEntry>> pendingVifLogs; // snapshots queued for MTVU thread
	VU1TraceZoneLookupFn zoneLookup; // optional: resolve EE addr to zone info string
};

extern VU1TraceState g_vu1Trace;
extern void vu1TraceSnapshotVifLog();  // call on EE thread before queuing MSCAL
extern void vu1TraceOnMscal(u32 addr, u32 itop, u32 top);
extern void vu1TraceOnFinish(u32 cyclesConsumed);

struct _VURegsNum {
	u8 pipe; // if 0xff, COP2
	u8 VFwrite;
	u8 VFwxyzw;
	u8 VFr0xyzw;
	u8 VFr1xyzw;
	u8 VFread0;
	u8 VFread1;
	u32 VIwrite;
	u32 VIread;
	int cycles;
};

using FnPtr_VuVoid = void (*)();
using FnPtr_VuRegsN = void(*)(_VURegsNum *VUregsn);

alignas(16) extern const FnPtr_VuVoid VU0_LOWER_OPCODE[128];
alignas(16) extern const FnPtr_VuVoid VU0_UPPER_OPCODE[64];
alignas(16) extern const FnPtr_VuRegsN VU0regs_LOWER_OPCODE[128];
alignas(16) extern const FnPtr_VuRegsN VU0regs_UPPER_OPCODE[64];

alignas(16) extern const FnPtr_VuVoid VU1_LOWER_OPCODE[128];
alignas(16) extern const FnPtr_VuVoid VU1_UPPER_OPCODE[64];
alignas(16) extern const FnPtr_VuRegsN VU1regs_LOWER_OPCODE[128];
alignas(16) extern const FnPtr_VuRegsN VU1regs_UPPER_OPCODE[64];
extern void _vuClearFMAC(VURegs * VU);
extern void _vuTestPipes(VURegs * VU);
extern void _vuTestUpperStalls(VURegs * VU, _VURegsNum *VUregsn);
extern void _vuTestLowerStalls(VURegs * VU, _VURegsNum *VUregsn);
extern void _vuAddUpperStalls(VURegs * VU, _VURegsNum *VUregsn);
extern void _vuAddLowerStalls(VURegs * VU, _VURegsNum *VUregsn);
extern void _vuXGKICKTransfer(s32 cycles, bool flush);
extern void validateXGKickPacketSize(u32 addr);
extern void vu1InfiniteLoopDetected();

// VU1 per-PC profiler
extern u32* getVU1PcHitCounts();
extern void resetVU1PcHitCounts();
