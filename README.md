# PCSX2 with Lua

Lua scripting engine and MCP debug server integrated into PCSX2.

Scripts directory controlled from `Settings` tab of new window started with emulator. Scripts are reloaded every time game is restarted.

Examples can be found in [god of war editor repository](https://github.com/mogaika/god_of_war_editor/tree/main/pcsx2-lua-scripts)

## Lua API

### cpu — Execution hooks and breakpoints

```lua
cpu.hook(addr, fn)          -- Install execution hook at PS2 address
cpu.hook_once(addr, fn)     -- One-shot hook, auto-removed after first hit
cpu.unhook(addr)            -- Remove all hooks at address
cpu.breakpoint(addr)        -- Add execution breakpoint (pauses emulator)
cpu.remove_breakpoint(addr) -- Remove breakpoint
cpu.capture_stack()         -- Capture current MIPS stack trace (call inside a hook)
                            -- Returns {frames={pc1,...}, depth=N}
```

### mem — PS2 memory access and heap tracking

```lua
-- Basic memory I/O
mem.read8/16/32/64(addr) -> int
mem.write8/16/32/64(addr, val)
mem.read_float(addr) -> float
mem.write_float(addr, val)
mem.read_string(addr [, maxLen=256]) -> string
mem.write_string(addr, str)
mem.read_bytes(addr, len) -> table
mem.write_bytes(addr, data [, offset [, length]])

-- Memory watchpoints
mem.breakpoint(start, size [, mode="rw"])
mem.remove_breakpoint(start, size)

-- Memory access tracing
mem.trace_start(addr, {name=, reads=true, writes=false, dma=false}) -> hook_id
mem.trace_stop(hook_id)
mem.trace_read(hook_id) -> table      -- read without clearing
mem.trace_flush(hook_id)              -- clear accumulated trace stats
mem.traces_flush()                    -- flush all active traces

-- Heap zone tracker
mem.zone_track(addr, size [, parent_addr [, name [, stack]]])
mem.zone_free(addr)
mem.zones_clear()
mem.zone_find(addr) -> {address, size, parent, name, tags} | nil
mem.zone_find_containing(addr) -> {address, size, parent, name, tags} | nil
mem.zone_tag(addr, key, value)
mem.zone_untag(addr, key)
mem.zones_count() -> int
mem.zones_snapshot() -> table
mem.zones_query({parent=, contains=, tag={k=v}}) -> table
```

### reg — CPU registers

```lua
reg.get(idx) -> u32                -- Get GPR by index (0-31)
reg.set(idx, val)                  -- Set GPR by index
reg.pc -> u32                      -- Program counter (read-only)
reg.cycle -> u32                   -- CPU cycle count (read-only)
reg.zero, reg.at, reg.v0, reg.v1  -- Named GPR access (all 32 MIPS registers)
reg.a0..a3, reg.t0..t9, reg.s0..s7, reg.gp, reg.sp, reg.fp, reg.ra
```

### emu — Emulator control

```lua
emu.pause([reason])                -- Pause emulator
emu.resume()                       -- Resume emulator
emu.is_paused() -> bool
emu.is_running() -> bool
emu.reset()                        -- Reset emulator (triggers script reload)
emu.game_serial() -> string
emu.game_crc() -> string           -- Formatted as "0x%08x"
emu.skip_block()                   -- Skip hooked function (set pc=ra)
emu.assert(msg [, stack])          -- Pause VM with message and optional stack
emu.set_speed(mode)                -- "normal", "turbo", "slow", "unlimited"

-- VU1 debugging
emu.vu1_profile()                  -- Print per-PC hit counts sorted by address, then reset
emu.vu1_trace_start([path], [opts])-- Start recording VU1 execution traces to file
                                   -- path: output file (default EmuFolders::Logs/vu1_trace.txt)
                                   -- opts: {regs=bool, memory=bool, pc_hits=bool}
                                   -- VIF command log is always included
emu.vu1_trace_stop()               -- Stop recording and close trace file
```

### ui — Widget registration

```lua
ui.set_group(name [, description])     -- Set group for subsequent widgets; description shown as muted text

-- All widget_* functions return a handle with methods:
--   :get()           -- returns current value (string/bool/number depending on type)
--   :set(val)        -- set current value
--   :set_default(val)-- change default value
--   :set_options(list) -- update options (checkbox with options only)
--   :on_change(fn)   -- register change callback (replaces previous)
--   :on_submit(fn)   -- register submit callback (replaces previous)
--                     -- Text: fires on Enter; Slider: fires on release; List: fires on double-click
--                     -- Not applicable to bool checkbox, radio, dropdown, or button

-- Text input (optional browse button for dir/file/save)
ui.widget_text(key, {label=, default=, browse=, filter=})
  -- browse: nil (plain text), "dir", "file", "save"
  -- filter: file filter string for browse="file"/"save" (e.g. "*.lua")
  -- on_change: fires on every keystroke and focus-lost
  -- on_submit: fires on Enter key only

-- Checkbox (bool toggle, radio buttons, dropdown, or list)
ui.widget_checkbox(key, {label=, default=, options=, dropdown=, list=})
  -- no options: bool toggle (QCheckBox)
  -- options={"A","B"}: radio buttons
  -- options + dropdown=true: combobox/dropdown
  -- options + list=true: list widget (max 5 visible, scrollbar after)
  -- on_change: fires on toggle/selection change
  -- on_submit: list only — fires on double-click

-- Numeric slider (always double precision)
ui.widget_slider(key, {label=, default=, min=, max=, step=})
  -- min/max required, step defaults to 1
  -- on_change: fires on slider release (not during drag)
  -- on_submit: fires on slider release (same timing as on_change)

-- Button
ui.widget_button(key, {label=, callback=})
  -- callback required (no on_change/on_submit)
```

**Event summary**:

| Widget | `on_change` | `on_submit` |
|--------|-------------|-------------|
| Text | keystroke + focus-lost | Enter |
| Checkbox (bool) | toggle | — |
| Checkbox (radio/dropdown) | selection | — |
| Checkbox (list) | selection | double-click |
| Slider | release | release |
| Button | — | — (use `callback`) |

### config — Persistent key-value storage

```lua
config.get(key [, default]) -> string|nil  -- Read from base settings "LuaMods" section
config.set(key, value)                     -- Write to base settings; nil removes key
config.game_get(key [, default]) -> string|nil  -- Read from per-game settings
config.game_set(key, value)                     -- Write to per-game settings
```

### osd — On-screen overlay

Blocks are ImGui windows anchored to screen corners. Multiple blocks on the same corner stack vertically as collapsible headers. Text-only output — use `ui.*` widgets for interactive controls.

```lua
-- Create blocks (once, at script load)
local blk = osd.create_block(name, align_h, align_v)
  -- align_h: "left" | "right"
  -- align_v: "top" | "bottom"
  -- Returns block handle (integer)
osd.destroy_block(blk)
osd.set_block_width(blk, 250)  -- fixed minimum width in pixels (0 = auto)

-- Global visibility
osd.show()
osd.hide()
osd.toggle()

-- Per-frame content (call from cpu.hook each frame)
osd.begin(blk)                                    -- start filling a block
osd.text("Hello")                                  -- plain white text
osd.text("Warning!", 0xFFFF0000)                   -- colored text (0xAARRGGBB integer)
-- Color format: 0xAARRGGBB where AA=alpha, RR=red, GG=green, BB=blue
-- Examples: 0xFFFF0000 = red, 0xFF00FF00 = green, 0xFF888888 = gray
--           0xFFFFFF00 = yellow, 0xFF00FFFF = cyan, 0x80FFFFFF = semi-transparent white
osd.separator()                                    -- horizontal line
osd.finish()                                       -- end block
```

Block content is cleared each frame — Lua must re-submit from `cpu.hook` callbacks. Block definitions (created at init) persist until script stop/reload.

**Example**:
```lua
local mem_block = osd.create_block("Memory", "left", "top")

cpu.hook(MAIN_LOOP_ADDR, function()
    osd.begin(mem_block)
    osd.text(string.format("Heap: %dK / %dK", used, total))
    osd.text(string.format("Objects: %d", count))
    osd.finish()
end)
```

### log — Logging

```lua
log.info(system, msg)   -- 0=info
log.warn(system, msg)   -- 1=warn
log.error(system, msg)  -- 2=error
print(...)              -- routes to log.info("Lua", ...)
```

## MCP Server

`mcp_server.py` bridges Claude Code to the PCSX2 debug socket.

Install:
```
claude mcp add --transport stdio pcsx2-lua -- uv run /path/to/mcp_server.py
```

### MCP Tools

| Tool | Description |
|------|-------------|
| `emu_control` | Pause/resume/reset/frame-advance the PS2 VM |
| `read_memory` / `write_memory` | Read/write PS2 RAM |
| `registers` | Read/write EE (R5900) registers |
| `breakpoint` | Execution breakpoints and memory watchpoints |
| `alloc_tracker` | Query the heap allocation tracker |
| `mem_trace` | Start/stop/read memory access traces |
| `hexdump` | Hex+ASCII dump of PS2 memory |
| `lua` | Execute Lua code, run scripts, drain logs, get docs, manage widgets |
