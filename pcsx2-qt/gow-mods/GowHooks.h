// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

class QString;

void gowInitHooks();
void gowShutdownHooks();
void gowLoadCustomLevel(const QString& levelName);
