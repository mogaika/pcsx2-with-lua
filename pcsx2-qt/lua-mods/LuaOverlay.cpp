// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "LuaOverlay.h"

#include "common/Console.h"
#include "ImGui/ImGuiManager.h"

#include "imgui.h"

#include "fmt/format.h"

#include <algorithm>
#include <map>

namespace LuaOverlay
{
	static std::mutex s_mutex;
	static std::vector<BlockDef> s_blocks;
	static int s_nextBlockId = 1;
	static bool s_visible = true;

	// Double-buffered command queues: Lua writes to pending, GS reads from render
	static std::vector<DrawCommand> s_pendingCmds;
	static std::vector<DrawCommand> s_renderCmds;
	static u64 s_frameCounter = 0;

	Corner ParseCorner(const std::string& align_h, const std::string& align_v)
	{
		bool right = (align_h == "right");
		bool bottom = (align_v == "bottom");
		if (!right && !bottom)
			return Corner::TopLeft;
		if (right && !bottom)
			return Corner::TopRight;
		if (!right && bottom)
			return Corner::BottomLeft;
		return Corner::BottomRight;
	}

	int CreateBlock(const std::string& name, Corner corner)
	{
		std::lock_guard lock(s_mutex);
		int id = s_nextBlockId++;
		s_blocks.push_back({id, name, corner, 0, true});
		return id;
	}

	void SetBlockWidth(int id, float width)
	{
		std::lock_guard lock(s_mutex);
		for (auto& b : s_blocks)
		{
			if (b.id == id)
			{
				b.width = width;
				break;
			}
		}
	}

	void DestroyBlock(int id)
	{
		std::lock_guard lock(s_mutex);
		for (auto& b : s_blocks)
		{
			if (b.id == id)
			{
				b.alive = false;
				break;
			}
		}
	}

	void CommitFrame(std::vector<DrawCommand> cmds)
	{
		std::lock_guard lock(s_mutex);
		s_pendingCmds = std::move(cmds);
	}

	void SetVisible(bool v)
	{
		std::lock_guard lock(s_mutex);
		s_visible = v;
	}

	bool IsVisible()
	{
		std::lock_guard lock(s_mutex);
		return s_visible;
	}

	void ToggleVisible()
	{
		std::lock_guard lock(s_mutex);
		s_visible = !s_visible;
	}

	void Reset()
	{
		std::lock_guard lock(s_mutex);
		s_blocks.clear();
		s_pendingCmds.clear();
		s_renderCmds.clear();
		s_nextBlockId = 1;
		s_frameCounter = 0;
	}

	u64 GetFrameCounter()
	{
		std::lock_guard lock(s_mutex);
		return s_frameCounter;
	}

	static ImVec2 GetCornerPos(Corner corner, float margin)
	{
		const float w = ImGuiManager::GetWindowWidth();
		const float h = ImGuiManager::GetWindowHeight();

		switch (corner)
		{
			case Corner::TopLeft:
				return ImVec2(margin, margin);
			case Corner::TopRight:
				return ImVec2(w - margin, margin);
			case Corner::BottomLeft:
				return ImVec2(margin, h - margin);
			case Corner::BottomRight:
				return ImVec2(w - margin, h - margin);
			default:
				return ImVec2(margin, margin);
		}
	}

	static ImVec2 GetCornerPivot(Corner corner)
	{
		switch (corner)
		{
			case Corner::TopLeft:
				return ImVec2(0.0f, 0.0f);
			case Corner::TopRight:
				return ImVec2(1.0f, 0.0f);
			case Corner::BottomLeft:
				return ImVec2(0.0f, 1.0f);
			case Corner::BottomRight:
				return ImVec2(1.0f, 1.0f);
			default:
				return ImVec2(0.0f, 0.0f);
		}
	}

	static ImVec4 ColorToImVec4(u32 argb)
	{
		float a = ((argb >> 24) & 0xFF) / 255.0f;
		float r = ((argb >> 16) & 0xFF) / 255.0f;
		float g = ((argb >> 8) & 0xFF) / 255.0f;
		float b = ((argb) & 0xFF) / 255.0f;
		return ImVec4(r, g, b, a);
	}

	void RenderFrame()
	{
		// Pick up new frame if available, otherwise keep rendering the last one
		std::vector<BlockDef> blocks;
		std::vector<DrawCommand> localCmds;
		bool visible;
		{
			std::lock_guard lock(s_mutex);
			visible = s_visible;
			if (!s_pendingCmds.empty())
			{
				s_renderCmds.swap(s_pendingCmds);
				s_pendingCmds.clear();
				s_frameCounter++;
			}

			// Copy render commands to local to avoid data race with Reset()
			localCmds = s_renderCmds;

			// Purge dead blocks
			std::erase_if(s_blocks, [](const BlockDef& b) { return !b.alive; });

			// Snapshot live blocks
			blocks.reserve(s_blocks.size());
			for (const auto& b : s_blocks)
			{
				if (b.alive)
					blocks.push_back(b);
			}
		}

		if (!visible || blocks.empty())
			return;

		const float scale = ImGuiManager::GetGlobalScale();
		const float margin = std::ceil(10.0f * scale);

		// Build block id → corner + width lookup
		struct BlockInfo
		{
			Corner corner;
			float width;
		};
		std::map<int, BlockInfo> blockInfo;
		for (const auto& b : blocks)
			blockInfo[b.id] = {b.corner, b.width};

		// Group commands by corner
		struct BlockCommands
		{
			int blockId;
			std::string name;
			float width;
			std::vector<const DrawCommand*> cmds;
		};

		// For each corner, collect blocks in order
		std::map<Corner, std::vector<BlockCommands>> cornerBlocks;

		// Parse commands: split by BlockBegin/BlockEnd into per-block groups
		int currentBlockId = -1;
		BlockCommands* currentBlock = nullptr;

		for (const auto& cmd : localCmds)
		{
			if (cmd.type == CmdType::BlockBegin)
			{
				currentBlockId = cmd.blockId;
				auto infoIt = blockInfo.find(currentBlockId);
				if (infoIt == blockInfo.end())
					continue;

				Corner c = infoIt->second.corner;
				float w = infoIt->second.width;
				auto& bvec = cornerBlocks[c];

				// Find or create entry for this block
				currentBlock = nullptr;
				for (auto& bc : bvec)
				{
					if (bc.blockId == currentBlockId)
					{
						currentBlock = &bc;
						break;
					}
				}
				if (!currentBlock)
				{
					// Find block name
					std::string bname;
					for (const auto& b : blocks)
					{
						if (b.id == currentBlockId)
						{
							bname = b.name;
							break;
						}
					}
					bvec.push_back({currentBlockId, bname, w, {}});
					currentBlock = &bvec.back();
				}
			}
			else if (cmd.type == CmdType::BlockEnd)
			{
				currentBlockId = -1;
				currentBlock = nullptr;
			}
			else if (currentBlock)
			{
				currentBlock->cmds.push_back(&cmd);
			}
		}

		// Render each corner as a single ImGui window
		static const char* cornerNames[] = {"##osd_TL", "##osd_TR", "##osd_BL", "##osd_BR"};

		for (auto& [corner, bvec] : cornerBlocks)
		{
			if (bvec.empty())
				continue;

			ImVec2 pos = GetCornerPos(corner, margin);
			ImVec2 pivot = GetCornerPivot(corner);

			// Compute the max fixed width across all blocks in this corner
			float maxBlockWidth = 0;
			for (const auto& bc : bvec)
			{
				if (bc.width > 0)
					maxBlockWidth = std::max(maxBlockWidth, bc.width * scale);
			}

			ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
			ImGui::SetNextWindowBgAlpha(0.65f);

			ImGuiWindowFlags flags =
				ImGuiWindowFlags_NoTitleBar |
				ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoFocusOnAppearing |
				ImGuiWindowFlags_NoNav |
				ImGuiWindowFlags_NoSavedSettings;

			if (maxBlockWidth > 0)
			{
				// Fixed-width mode: set size constraints so width is resizable
				// with the Lua-set width as minimum, height auto-sizes
				ImGui::SetNextWindowSizeConstraints(
					ImVec2(maxBlockWidth, 0),
					ImVec2(ImGuiManager::GetWindowWidth() * 0.5f, ImGuiManager::GetWindowHeight()));
				flags |= ImGuiWindowFlags_AlwaysAutoResize;
			}
			else
			{
				// Auto-size mode
				flags |= ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize;
			}

			int cornerIdx = static_cast<int>(corner);
			if (!ImGui::Begin(cornerNames[cornerIdx], nullptr, flags))
			{
				ImGui::End();
				continue;
			}

			for (auto& bc : bvec)
			{
				// Use block name as collapsing header with unique ID
				std::string headerLabel = fmt::format("{}###osd_block_{}", bc.name, bc.blockId);

				if (ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
				{
					for (const auto* cmd : bc.cmds)
					{
						switch (cmd->type)
						{
							case CmdType::Text:
							{
								ImVec4 col = ColorToImVec4(cmd->color);
								ImGui::TextColored(col, "%s", cmd->text.c_str());
								break;
							}
							case CmdType::Separator:
							{
								ImGui::Separator();
								break;
							}
							default:
								break;
						}
					}
				}
			}

			ImGui::End();
		}
	}

} // namespace LuaOverlay
