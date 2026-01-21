// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <mutex>
#include <string>
#include <vector>

namespace LuaOverlay
{
	enum class Corner : u8
	{
		TopLeft,
		TopRight,
		BottomLeft,
		BottomRight,
		Count
	};

	Corner ParseCorner(const std::string& align_h, const std::string& align_v);

	enum class CmdType : u8
	{
		BlockBegin,
		BlockEnd,
		Text,
		Separator,
	};

	struct DrawCommand
	{
		CmdType type;
		int blockId = -1;
		u32 color = 0xFFFFFFFF; // 0xAARRGGBB
		std::string text;
	};

	struct BlockDef
	{
		int id;
		std::string name;
		Corner corner;
		float width = 0; // 0 = auto-size, >0 = fixed width in scaled pixels
		bool alive = true;
	};

	int CreateBlock(const std::string& name, Corner corner);
	void DestroyBlock(int id);
	void SetBlockWidth(int id, float width);

	// Commit a complete frame of commands atomically (replaces pending buffer).
	// Called from EE thread once all blocks have been filled for the frame.
	void CommitFrame(std::vector<DrawCommand> cmds);

	// Called on GS thread from RenderOverlays
	void RenderFrame();

	void SetVisible(bool v);
	bool IsVisible();
	void ToggleVisible();

	// Clear all blocks and commands (on script stop/reload)
	void Reset();

	// Frame counter — incremented each time GS thread swaps command buffers
	u64 GetFrameCounter();

} // namespace LuaOverlay
