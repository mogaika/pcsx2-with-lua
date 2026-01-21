// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "Gif_Unit.h"
#include "Vif_Dma.h"
#include "MTVU.h"

#include "common/Path.h"

#include <atomic>
#include <cstdio>
#include <cstring>

Gif_Unit gifUnit;

static std::atomic<bool> s_vu1DumpDone{false};

void resetGifDumpFlags()
{
	s_vu1DumpDone.store(false);
}

void DumpVU1StateOnGifError(const char* reason, u32 kickAddr)
{
	// One-shot: only first error triggers a dump
	bool expected = false;
	if (!s_vu1DumpDone.compare_exchange_strong(expected, true))
		return;

	std::string dumpPath = Path::Combine(EmuFolders::Logs, "vu1_gif_error_dump.txt");
	FILE* f = std::fopen(dumpPath.c_str(), "w");
	if (!f)
	{
		Console.Error("DumpVU1StateOnGifError: failed to open %s", dumpPath.c_str());
		return;
	}

	VURegs& vu1 = vuRegs[1];
	u8* pMem = vu1.Mem;

	// Use provided kick address, fall back to VU1.xgkickaddr
	u32 actualKickAddr = (kickAddr != ~0u) ? kickAddr : vu1.xgkickaddr;

	std::fprintf(f, "=== VU1 GIF Error Dump ===\n");
	std::fprintf(f, "Reason: %s\n", reason);
	std::fprintf(f, "EE PC: 0x%08x\n", cpuRegs.pc);
	std::fprintf(f, "xgkickaddr (VU1 reg): 0x%04x  actual kick addr: 0x%04x  xgkickcycle: %u  VU1.cycle: %u\n",
		vu1.xgkickaddr, actualKickAddr, vu1.xgkickcyclecount, vu1.cycle);
	std::fprintf(f, "start_pc: 0x%04x  branch: %u  branchpc: 0x%04x\n\n",
		vu1.start_pc, vu1.branch, vu1.branchpc);

	// GIF tag chain from actual kick address
	std::fprintf(f, "=== GIF Tag Chain from kick addr 0x%04x ===\n", actualKickAddr);
	const char* flgNames[] = {"PACKED", "REGLIST", "IMAGE", "IMAGE2"};
	u32 dumpOff = actualKickAddr;
	for (u32 i = 0; i < 16; i++) // up to 16 tags to avoid infinite loop
	{
		u32 da = dumpOff & 0x3FFF;
		u32* raw = (u32*)&pMem[da];
		Gif_Tag dt(&pMem[da]);
		std::fprintf(f, "  tag[%u] @0x%04x: %08x %08x %08x %08x  FLG=%s NLOOP=%u NREG=%u EOP=%u len=0x%x\n",
			i, da, raw[0], raw[1], raw[2], raw[3],
			flgNames[dt.tag.FLG & 3], dt.tag.NLOOP, dt.tag.NREG, dt.tag.EOP, dt.len);
		dumpOff += 16 + dt.len;
		if (dt.tag.EOP)
			break;
		if ((dumpOff - actualKickAddr) > 0x4000)
		{
			std::fprintf(f, "  ... chain exceeds VU1 memory, stopping\n");
			break;
		}
	}

	// VU1 registers
	std::fprintf(f, "\n=== VU1 Registers ===\n");
	for (int i = 0; i < 32; i++)
		std::fprintf(f, "  VF%02d: %12.6f %12.6f %12.6f %12.6f  (0x%08x %08x %08x %08x)\n",
			i, vu1.VF[i].f.x, vu1.VF[i].f.y, vu1.VF[i].f.z, vu1.VF[i].f.w,
			vu1.VF[i].i.x, vu1.VF[i].i.y, vu1.VF[i].i.z, vu1.VF[i].i.w);
	for (int i = 0; i < 16; i++)
		std::fprintf(f, "  VI%02d: 0x%04x (%d)\n", i, vu1.VI[i].US[0], vu1.VI[i].SS[0]);
	std::fprintf(f, "  ACC:  %12.6f %12.6f %12.6f %12.6f\n",
		vu1.ACC.f.x, vu1.ACC.f.y, vu1.ACC.f.z, vu1.ACC.f.w);
	std::fprintf(f, "  Q: %f  P: %f\n", vu1.q.F, vu1.p.F);
	std::fprintf(f, "  Status: 0x%x  MAC: 0x%x  Clip: 0x%x\n",
		vu1.statusflag, vu1.macflag, vu1.clipflag);

	// VU1 data memory
	std::fprintf(f, "\n=== VU1 Data Memory (0x4000 bytes) ===\n");
	for (u32 a = 0; a < 0x4000; a += 16)
	{
		u8* p = &pMem[a];
		float floats[4];
		u32 u32s[4];
		std::memcpy(floats, p, 16);
		std::memcpy(u32s, p, 16);
		std::fprintf(f, "  %04x:", a);
		for (int j = 0; j < 16; j++)
			std::fprintf(f, " %02x", p[j]);
		std::fprintf(f, "  [%12g %12g %12g %12g]  [%08x %08x %08x %08x]",
			floats[0], floats[1], floats[2], floats[3],
			u32s[0], u32s[1], u32s[2], u32s[3]);
		if (a == (actualKickAddr & 0x3FF0))
			std::fprintf(f, "  <-- XGKICK");
		std::fprintf(f, "\n");
	}

	// VU1 micro memory with disassembly
	std::fprintf(f, "\n=== VU1 Micro Memory (0x4000 bytes) with disassembly ===\n");
	for (u32 a = 0; a < 0x4000; a += 8)
	{
		u32* inst = (u32*)&vu1.Micro[a];
		u32 upper = inst[1];
		u32 lower = inst[0];
		const char* upperStr = disVU1MicroUF(upper, a);
		const char* lowerStr = disVU1MicroLF(lower, a);
		bool isXgkick = ((lower & 0x3f) == 0x3c) &&
			(((lower >> 6) & 0x1f) == 0x1b) &&
			(((lower >> 21) & 0x3) == 0x0);
		std::fprintf(f, "  %04x: %08x %08x  %s | %s%s\n",
			a, upper, lower, upperStr, lowerStr,
			isXgkick ? "  <--- XGKICK" : "");
	}

	// Binary dumps
	std::fprintf(f, "\n=== Binary files written ===\n");
	FILE* fb;
	std::string dataPath = Path::Combine(EmuFolders::Logs, "vu1_data.bin");
	fb = std::fopen(dataPath.c_str(), "wb");
	if (fb) { std::fwrite(pMem, 1, 0x4000, fb); std::fclose(fb); std::fprintf(f, "  %s (16KB data mem)\n", dataPath.c_str()); }
	std::string microPath = Path::Combine(EmuFolders::Logs, "vu1_micro.bin");
	fb = std::fopen(microPath.c_str(), "wb");
	if (fb) { std::fwrite(vu1.Micro, 1, 0x4000, fb); std::fclose(fb); std::fprintf(f, "  %s (16KB micro mem)\n", microPath.c_str()); }

	std::fclose(f);
	Console.Error("VU1 GIF error dump: %s -- start_pc=0x%04x kick_addr=0x%04x -- %s + %s + %s",
		reason, vu1.start_pc, actualKickAddr, dumpPath.c_str(), dataPath.c_str(), microPath.c_str());
}

// Returns true on stalling SIGNAL
bool Gif_HandlerAD(u8* pMem)
{
	u32 reg = pMem[8];
	u32* data = (u32*)pMem;
	if (reg >= GIF_A_D_REG_BITBLTBUF && reg <= GIF_A_D_REG_TRXREG)
	{
		vif1.transfer_registers[reg - GIF_A_D_REG_BITBLTBUF] = *(u64*)pMem;
	}
	else if (reg == GIF_A_D_REG_TRXDIR)
	{ // TRXDIR
		if ((pMem[0] & 3) == 1)
		{                // local -> host
			u8 bpp = 32; // Onimusha does TRXDIR without BLTDIVIDE first, assume 32bit
			switch (vif1.BITBLTBUF.SPSM & 7)
			{
				case 0:
					bpp = 32;
					break;
				case 1:
					bpp = 24;
					break;
				case 2:
					bpp = 16;
					break;
				case 3:
					bpp = 8;
					break;
				default: // 4 is 4 bit but this is forbidden
					Console.Error("Illegal format for GS upload: SPSM=0%02o", vif1.BITBLTBUF.SPSM);
					break;
			}
			// qwords, rounded down; any extra bits are lost
			// games must take care to ensure transfer rectangles are exact multiples of a qword
			vif1.GSLastDownloadSize = vif1.TRXREG.RRW * vif1.TRXREG.RRH * bpp >> 7;
		}
	}
	else if (reg == GIF_A_D_REG_SIGNAL)
	{ // SIGNAL
		if (CSRreg.SIGNAL)
		{ // Time to ignore all subsequent drawing operations.
			GUNIT_WARN(Color_Orange, "GIF Handler - Stalling SIGNAL");
			if (!gifUnit.gsSIGNAL.queued)
			{
				gifUnit.gsSIGNAL.queued = true;
				gifUnit.gsSIGNAL.data[0] = data[0];
				gifUnit.gsSIGNAL.data[1] = data[1];
				return true; // Stalling SIGNAL
			}
		}
		else
		{
			GUNIT_WARN("GIF Handler - SIGNAL");
			GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~data[1]) | (data[0] & data[1]);
			if (!GSIMR.SIGMSK)
				gsIrq();
			CSRreg.SIGNAL = true;
		}
	}
	else if (reg == GIF_A_D_REG_FINISH)
	{ // FINISH
		GUNIT_WARN("GIF Handler - FINISH");
		gifUnit.gsFINISH.gsFINISHFired = false;
		gifUnit.gsFINISH.gsFINISHPending = true;
	}
	else if (reg == GIF_A_D_REG_LABEL)
	{ // LABEL
		GUNIT_WARN("GIF Handler - LABEL");
		GSSIGLBLID.LBLID = (GSSIGLBLID.LBLID & ~data[1]) | (data[0] & data[1]);
	}
	else if (reg >= 0x63 && reg != 0x7f)
	{
		//DevCon.Warning("GIF Handler - Write to unknown register! [reg=%x]", reg);
	}
	return false;
}

void Gif_HandlerAD_MTVU(u8* pMem)
{
	// Note: Atomic communication is with MTVU.cpp Get_GSChanges
	const u8 reg = pMem[8] & 0x7f;
	const u32* data = (u32*)pMem;

	if (reg == GIF_A_D_REG_SIGNAL)
	{ // SIGNAL
		GUNIT_WARN("GIF Handler - SIGNAL");
		if (vu1Thread.mtvuInterrupts.load(std::memory_order_acquire) & VU_Thread::InterruptFlagSignal)
			Console.Error("GIF Handler MTVU - Double SIGNAL Not Handled");
		vu1Thread.gsSignal.store(((u64)data[1] << 32) | data[0], std::memory_order_relaxed);
		vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagSignal, std::memory_order_release);
	}
	else if (reg == GIF_A_D_REG_FINISH)
	{ // FINISH
		GUNIT_WARN("GIF Handler - FINISH");
		u32 old = vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagFinish, std::memory_order_relaxed);
		if (old & VU_Thread::InterruptFlagFinish)
			Console.Error("GIF Handler MTVU - Double FINISH Not Handled");
	}
	else if (reg == GIF_A_D_REG_LABEL)
	{ // LABEL
		GUNIT_WARN("GIF Handler - LABEL");
		// It's okay to coalesce label updates
		u32 labelData = data[0];
		u32 labelMsk = data[1];
		u64 existing = 0;
		u64 wanted = ((u64)labelMsk << 32) | labelData;
		while (!vu1Thread.gsLabel.compare_exchange_weak(existing, wanted, std::memory_order_relaxed))
		{
			u32 existingData = (u32)existing;
			u32 existingMsk = (u32)(existing >> 32);
			u32 wantedData = (existingData & ~labelMsk) | (labelData & labelMsk);
			u32 wantedMsk = existingMsk | labelMsk;
			wanted = ((u64)wantedMsk << 32) | wantedData;
		}
		vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagLabel, std::memory_order_release);
	}
	else if (reg >= 0x63 && reg != 0x7f)
	{
		DevCon.Warning("GIF Handler Debug - Write to unknown register! [reg=%x]", reg);
	}
}

// Returns true if pcsx2 needed to process the packet...
bool Gif_HandlerAD_Debug(u8* pMem)
{
	const u8 reg = pMem[8] & 0x7f;
	if (reg == 0x50)
	{
		Console.Error("GIF Handler Debug - BITBLTBUF");
		return 1;
	}
	else if (reg == 0x52)
	{
		Console.Error("GIF Handler Debug - TRXREG");
		return 1;
	}
	else if (reg == 0x53)
	{
		Console.Error("GIF Handler Debug - TRXDIR");
		return 1;
	}
	else if (reg == 0x60)
	{
		Console.Error("GIF Handler Debug - SIGNAL");
		return 1;
	}
	else if (reg == 0x61)
	{
		Console.Error("GIF Handler Debug - FINISH");
		return 1;
	}
	else if (reg == 0x62)
	{
		Console.Error("GIF Handler Debug - LABEL");
		return 1;
	}
	else if (reg >= 0x63 && reg != 0x7f)
	{
		DevCon.Warning("GIF Handler Debug - Write to unknown register! [reg=%x]", reg);
	}
	return 0;
}

void Gif_FinishIRQ()
{
	if (gifUnit.gsFINISH.gsFINISHPending)
	{
		CSRreg.FINISH = true;
		gifUnit.gsFINISH.gsFINISHPending = false;
	}
	if (CSRreg.FINISH && !GSIMR.FINISHMSK && !gifUnit.gsFINISH.gsFINISHFired)
	{
		gsIrq();
		gifUnit.gsFINISH.gsFINISHFired = true;
	}
}

bool SaveStateBase::gifPathFreeze(u32 path)
{

	Gif_Path& gifPath = gifUnit.gifPath[path];
	pxAssertMsg(!gifPath.readAmount, "Gif Path readAmount should be 0!");
	pxAssertMsg(!gifPath.gsPack.readAmount, "GS Pack readAmount should be 0!");
	pxAssertMsg(!gifPath.GetPendingGSPackets(), "MTVU GS Pack Queue should be 0!");

	if (!gifPath.isMTVU())
	{ // FixMe: savestate freeze bug (Gust games) with MTVU enabled
		if (IsSaving())
		{                            // Move all the buffered data to the start of buffer
			gifPath.RealignPacket(); // May add readAmount which we need to clear on load
		}
	}
	u8* bufferPtr = gifPath.buffer; // Backup current buffer ptr
	Freeze(gifPath.mtvu.fakePackets);
	FreezeMem(&gifPath, sizeof(gifPath) - sizeof(gifPath.mtvu));
	FreezeMem(bufferPtr, gifPath.curSize);
	gifPath.buffer = bufferPtr;
	if (!IsSaving())
	{
		gifPath.readAmount = 0;
		gifPath.gsPack.readAmount = 0;
	}

	return IsOkay();
}

bool SaveStateBase::gifFreeze()
{
	bool mtvuMode = THREAD_VU1;
	pxAssert(vu1Thread.IsDone());
	MTGS::WaitGS();
	if (!FreezeTag("Gif Unit"))
		return false;

	Freeze(mtvuMode);
	Freeze(gifUnit.stat);
	Freeze(gifUnit.gsSIGNAL);
	Freeze(gifUnit.gsFINISH);
	Freeze(gifUnit.lastTranType);
	gifPathFreeze(GIF_PATH_1);
	gifPathFreeze(GIF_PATH_2);
	gifPathFreeze(GIF_PATH_3);
	if (!IsSaving())
	{
		if (mtvuMode != THREAD_VU1)
		{
			DevCon.Warning("gifUnit: MTVU Mode has switched between save/load state");
			// ToDo: gifUnit.SwitchMTVU(mtvuMode);
		}
	}

	return true;
}
