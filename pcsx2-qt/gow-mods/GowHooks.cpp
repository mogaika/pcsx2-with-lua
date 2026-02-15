// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GowHooks.h"
#include "GowModWindow.h"
#include "GowWadInjector.h"

#include "Memory.h"
#include "R5900.h"
#include "x86/iR5900.h"

#include <QtCore/QString>
#include <cstring>

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
}

void gowShutdownHooks()
{
	removeExecutionHook(0x1BB0F8);
	removeExecutionHook(0x185F28);

	GowWadInjector::shutdownWadInjectorHooks();
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
