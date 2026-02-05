// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GowHooks.h"
#include "GowModWindow.h"
#include "GowWadInjector.h"

#include "DebugTools/MemoryTrace.h"
#include "Memory.h"
#include "R5900.h"
#include "x86/iR5900.h"

#include <QtCore/QString>
#include <cstring>

u32 g_gowTraceHookId = 0;

// Hook callback for GameWadLoader::AddCommand at 0x001bb0f8
static void hookWadEventAdded()
{
	u16 cmdType = cpuRegs.GPR.n.a0.US[0];
	u16 param2 = cpuRegs.GPR.n.a1.US[0];
	u32 param3 = cpuRegs.GPR.n.a2.UL[0];

	const char* name = nullptr;
	if (cmdType == 3 || cmdType == 9)
	{
		u32 namePtr = cpuRegs.GPR.n.a3.UL[0];
		if (namePtr != 0)
			name = (const char*)PSM(namePtr);
	}

	GowModWindow::logCommand(cmdType, param2, param3, name);
}

// Callback for hook results (called on write detection)
static void onClientParmTraceResult(const HookTrace& trace)
{
	if (trace.firstWritePC != 0)
	{
		GowModWindow::logInjectionMessage(
			QString("[TRACE] Overwritten at PC=0x%1\n").arg(trace.firstWritePC, 8, 16, QChar('0')));
	}
}

// Hook for svrClientParm::IFFProcessClientParm at 0x01789e0
static void hookIFFProcessClientParm()
{
	u32 headerPtr = cpuRegs.GPR.n.a0.UL[0];
	u32 dataPtr = cpuRegs.GPR.n.a1.UL[0];

	if (dataPtr == 0 || g_gowTraceHookId == 0)
		return;

	u32 magic = *(u32*)PSM(dataPtr);
	if (magic != 0x00020001)
		return;

	u32 traceStart = dataPtr + 0x00;
	u32 traceSize = 0x5c;

	MemoryTraceManager::Instance().AddTracedRange(g_gowTraceHookId, traceStart, traceSize);
}

// Hook at 0x001342f8 to trace v0 register range [v0+0x8, v0+0x44)
static void hookTraceV0()
{
	if (g_gowTraceHookId == 0)
		return;

	u32 v0 = cpuRegs.GPR.n.v0.UL[0];
	if (v0 == 0)
		return;

	u32 traceStart = v0 + 0x8;
	u32 traceSize = 0x44 - 0x8; // 0x3c bytes

	MemoryTraceManager::Instance().AddTracedRange(g_gowTraceHookId, traceStart, traceSize);
}

// Hook for wadLoader::ProcessWadFile at 0x185f28
static void hookWadLoaderProcessWadFile()
{
	u32 wadNamePtr = cpuRegs.GPR.n.a1.UL[0];

	const char* wadName = nullptr;
	if (wadNamePtr != 0)
		wadName = (const char*)PSM(wadNamePtr);

	GowModWindow::logWadProcess(wadName);
}

void gowInitHooks()
{
	addExecutionHook(0x1BB0F8, hookWadEventAdded);
	addExecutionHook(0x185F28, hookWadLoaderProcessWadFile);

	GowWadInjector::initWadInjectorHooks();

	g_gowTraceHookId = MemoryTraceManager::Instance().RegisterHook(
		"goServer_LoadClient",
		onClientParmTraceResult,
		MEMTRACE_TRACK_READS | MEMTRACE_STOP_ON_WRITE
	);

	// addExecutionHook(0x01789e0, hookIFFProcessClientParm);  // Disabled
	addExecutionHook(0x001342f8, hookTraceV0);
}

void gowShutdownHooks()
{
	removeExecutionHook(0x1BB0F8);
	removeExecutionHook(0x185F28);
	// removeExecutionHook(0x01789e0);  // Disabled
	removeExecutionHook(0x001342f8);

	GowWadInjector::shutdownWadInjectorHooks();

	if (g_gowTraceHookId != 0)
	{
		MemoryTraceManager::Instance().UnregisterHook(g_gowTraceHookId);
		g_gowTraceHookId = 0;
	}

	MemoryTraceManager::Instance().ClearAllTraces();
}

void gowLoadCustomLevel(const QString& levelName)
{
	// Check if game is in Shell (main menu) state
	u32 curState = *(u32*)PSM(0x0029e560);
	if (curState != 3) {
		GowModWindow::logInjectionMessage(
			QString("[LEVEL] Cannot load level — not in main menu (curState=%1). Go to main menu first.\n")
				.arg(curState));
		return;
	}

	QByteArray nameBytes = levelName.toLatin1();

	// Write level name to gFirstLevel in PS2 RAM
	char* dst = (char*)PSM(0x0029e550);  // gFirstLevel
	std::memset(dst, 0, 8);
	std::strncpy(dst, nameBytes.constData(), 7);

	// Set bonusLevel pointer to gFirstLevel — Shell will pick it up next frame
	// Shell's code: if (bonusLevel != NULL) { strcpy(gFirstLevel, bonusLevel); GoToShellLoadDone(); }
	*(u32*)PSM(0x0029e5e8) = 0x0029e550;  // bonusLevel = &gFirstLevel
}
