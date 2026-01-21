#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["mcp"]
# ///
"""
PCSX2 Lua Debug MCP Server — bridges Claude Code to PCSX2's debug TCP server.

Usage:
    claude mcp add --transport stdio pcsx2-lua -- uv run /path/to/mcp_server.py

Override host/port with environment variables PCSX2_HOST / PCSX2_PORT (default 127.0.0.1:16767).
"""

import json
import os
import socket
from typing import Any

from mcp.server.fastmcp import FastMCP

PCSX2_HOST = os.environ.get("PCSX2_HOST", "127.0.0.1")
PCSX2_PORT = int(os.environ.get("PCSX2_PORT", "16767"))

mcp = FastMCP("pcsx2-lua")


class PCSX2Error(Exception):
    pass


def send_command(cmd: str, params: dict | None = None) -> dict:
    """Send JSON command to PCSX2 debug socket, return result dict. Raises on error."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(120)
        sock.connect((PCSX2_HOST, PCSX2_PORT))

        request = {"cmd": cmd}
        if params:
            request["params"] = params
        sock.sendall((json.dumps(request, separators=(",", ":")) + "\n").encode())

        data = b""
        while b"\n" not in data:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk

        sock.close()
        resp = json.loads(data.split(b"\n", 1)[0])
    except ConnectionRefusedError:
        raise PCSX2Error(f"Connection refused: {PCSX2_HOST}:{PCSX2_PORT}. Is PCSX2 running?")
    except socket.timeout:
        raise PCSX2Error("Timeout (120s)")
    except Exception as e:
        raise PCSX2Error(str(e))

    if not resp.get("ok", False):
        raise PCSX2Error(resp.get("error", "Unknown error"))
    return resp.get("result", {})


def R(result: dict) -> str:
    """Format result dict as compact JSON string."""
    return json.dumps(result, separators=(",", ":"))


@mcp.tool()
def emu_control(action: str, frames: int | None = None,
                resume: bool = True) -> str:
    """Emulator control. Pause before reading registers or writing memory.

Actions:
- status: Returns {state, paused, pc, disc_serial, disc_crc}.
  When paused also includes: pause_reason (user_request|breakpoint|memwatch|tlb_miss|bus_error|vif_error|dma_error|frame_advance|input_recording|lua_assert), pause_message (detail string), pause_pc (hex string).
  state is one of: shutdown, initializing, running, paused, resetting, stopping.
- pause: Pauses VM. Returns {paused:true}.
- resume: Resumes VM. Returns {paused:false}.
- frame_advance: Advance N frames then pause. frames param (default 1). Returns {frames}.
- reset: Cold-reboot the PS2 VM. Reloads Lua autoload scripts from disk.
  resume param (default true) auto-resumes after reset.
  Returns {reset:true, resumed:bool}."""
    if action == "frame_advance":
        return R(send_command("frame_advance", {"frames": frames or 1}))
    elif action == "reset":
        return R(send_command("reset", {"resume": resume}))
    else:
        return R(send_command(action))


@mcp.tool()
def read_memory(address: str, size: int = 4, format: str = "hex") -> str:
    """Read PS2 RAM. Address as hex string (e.g. "0x0029e560"). Max 4096 bytes.

Formats and return values:
- hex (default): Returns {hex: "aabbccdd..."} — raw bytes as hex string.
- u8: Returns {values: [0,255,...]} — array of unsigned bytes.
- u16: Returns {values: [0,65535,...]} — array of unsigned 16-bit ints (little-endian).
- u32: Returns {values: [0,4294967295,...]} — array of unsigned 32-bit ints.
- float: Returns {values: [1.0,...]} — array of IEEE 754 floats.
- string: Returns {string: "..."} — null-terminated string."""
    return R(send_command("read_memory", {"address": address, "size": size, "format": format}))


@mcp.tool()
def write_memory(address: str, format: str = "hex", hex: str | None = None, values: list | None = None, string: str | None = None) -> str:
    """Write PS2 RAM. VM must be paused. Returns {bytes_written}.

Formats:
- hex: Provide hex="aabbccdd" (even-length hex string, no 0x prefix).
- u8/u16/u32/float: Provide values=[...] array of numbers.
- string: Provide string="..." (writes with null terminator)."""
    params: dict[str, Any] = {"address": address, "format": format}
    if hex is not None:
        params["hex"] = hex
    if values is not None:
        params["values"] = values
    if string is not None:
        params["string"] = string
    return R(send_command("write_memory", params))


@mcp.tool()
def registers(action: str = "get", names: list[str] | None = None, sets: dict[str, str] | None = None) -> str:
    """EE (R5900) MIPS registers.

Actions:
- get: Read registers. If names provided (e.g. ["pc","a0","v0"]), returns only those.
  If names omitted, returns all 32 GPRs + pc + hi + lo.
  Valid names: zero,at,v0,v1,a0-a3,t0-t9,s0-s7,k0,k1,gp,sp,fp,ra,pc,hi,lo.
  All values returned as hex strings (e.g. "0x0029e560").
- set: Write registers. VM must be paused. Provide sets={"a0":"0x1234","v0":"0x5678"}.
  Values as hex strings or integers. Returns {registers_set: N}."""
    if action == "get":
        p: dict[str, Any] = {}
        if names:
            p["names"] = names
        return R(send_command("read_registers", p))
    elif action == "set":
        return R(send_command("set_register", {"sets": sets}))
    else:
        raise PCSX2Error(f"Unknown action: {action}")


@mcp.tool()
def breakpoint(action: str, address: str | None = None, timeout: float = 10.0, size: int = 4, condition: str = "rw") -> str:
    """EE breakpoints and memory watchpoints.

Actions:
- add: Add execution breakpoint at address (hex string). Returns {address}.
- remove: Remove breakpoint at address. Returns {address}.
- list: List all tracked breakpoints and memwatches.
  Returns {breakpoints: [{address, enabled}], memwatches: [{address, size}]}.
- wait: Block until VM pauses (breakpoint/memwatch hit) or timeout.
  timeout param in seconds (default 10). Returns {hit:true, pc, registers:{a0,a1,...}}
  or {hit:false, reason:"timeout"}. If already paused: {hit:true, pc, reason:"already_paused"}.
- add_memwatch: Add memory watchpoint. Params: address, size (bytes, default 4),
  condition ("r"=read, "w"=write, "rw"=both, default "rw"). Returns {address, size}.
- remove_memwatch: Remove memory watchpoint. Params: address, size. Returns {address, size}."""
    if action == "add":
        return R(send_command("add_breakpoint", {"address": address}))
    elif action == "remove":
        return R(send_command("remove_breakpoint", {"address": address}))
    elif action == "list":
        return R(send_command("list_breakpoints"))
    elif action == "wait":
        return R(send_command("wait_breakpoint", {"timeout": timeout}))
    elif action == "add_memwatch":
        return R(send_command("add_memwatch", {"address": address, "size": size, "condition": condition}))
    elif action == "remove_memwatch":
        return R(send_command("remove_memwatch", {"address": address, "size": size}))
    else:
        raise PCSX2Error(f"Unknown action: {action}")


@mcp.tool()
def alloc_tracker(action: str = "count", address: str | None = None, parent: str | None = None,
                  tag: dict | None = None, contains: str | None = None,
                  key: str | None = None, value: str | None = None, limit: int = 100) -> str:
    """PS2 memory zone tracker (heap allocations, pools, zones).

Actions:
- count: Returns {count} — total tracked zones.
- find: Find zone at exact address. Returns {found, address, size, parent, name, tags} or {found:false}.
- find_containing: Find zone whose range contains address. Same return as find.
- query: Search zones. Filters: parent (hex parent addr), contains (hex addr inside zone),
  tag ({key:value} dict to match). limit param caps results (default 100).
  Returns {count, allocations: [{address, size, parent, name, tags?}]}.
- tag: Add metadata tag. Params: address, key, value. Returns {tagged:true}.
- untag: Remove tag. Params: address, key. Returns {untagged:true}."""
    params: dict[str, Any] = {"action": action}
    if address is not None:
        params["address"] = address
    if parent is not None:
        params["parent"] = parent
    if tag is not None:
        params["tag"] = tag
    if contains is not None:
        params["contains"] = contains
    if key is not None:
        params["key"] = key
    if value is not None:
        params["value"] = value
    if action == "query":
        params["limit"] = limit
    return R(send_command("allocations", params))


@mcp.tool()
def mem_trace(action: str, address: str | None = None, hook_id: int | None = None,
              name: str | None = None, reads: bool = True, writes: bool = False,
              dma: bool = False, flush: bool = True) -> str:
    """Memory access tracing on PS2 heap allocations. Tracks which code reads/writes an allocation.

Actions:
- start: Begin tracing allocation at address. Params: name (label), reads (default true),
  writes (default false), dma (track DMA, default false).
  Returns {hook_id, address, size}. Save hook_id for stop/results.
- stop: Stop tracing. Param: hook_id. Returns {hook_id, stopped:true}.
- results: Get trace stats. Param: hook_id, flush (clear after read, default true).
  Returns {hook_id, entries, stats: [{offset, count, is_dma, stack: ["0x...", ...]}]}.
  stack is a 1-4 frame call chain: [pc, ra, caller1, caller2]. Use with Ghidra to resolve function names.
- flush_all: Flush all active traces to snapshot storage. Returns {flushed:true}."""
    params: dict[str, Any] = {"action": action}
    if action == "start":
        params.update({"address": address, "reads": reads, "writes": writes, "dma": dma})
        if name:
            params["name"] = name
        return R(send_command("mem_trace", params))
    elif action == "stop":
        params["hook_id"] = hook_id
        return R(send_command("mem_trace", params))
    elif action == "results":
        return R(send_command("mem_trace_results", {"hook_id": hook_id, "flush": flush}))
    elif action == "flush_all":
        return R(send_command("mem_trace", params))
    else:
        raise PCSX2Error(f"Unknown action: {action}")


@mcp.tool()
def hexdump(address: str, size: int = 256) -> str:
    """Hex+ASCII dump of PS2 memory. Max 4096 bytes.
Returns formatted multi-line string with "address  hex bytes  |ascii|" per 16-byte row."""
    return send_command("hexdump", {"address": address, "size": size}).get("dump", "")


@mcp.tool()
def lua(action: str = "exec", code: str | None = None, path: str | None = None,
        key: str | None = None, value: Any = None) -> str:
    """Lua scripting engine. Scripts can hook PS2 functions, read/write memory, register UI widgets.

Actions:
- exec: Execute Lua code string. Param: code. Returns {ok, return? (tostring of result), logs: [{level,msg}]}.
  NOTE: Hooks/widgets installed via exec do NOT survive emu_control reset — use autoload scripts for persistent hooks.
- run: Run script file additively into the existing Lua state (no state reset within this session).
  Param: path. Returns {ok, path, logs}.
  Previously defined globals, hooks, and widgets remain in the shared state.
  NOTE: run files are NOT re-executed on reset — only autoload scripts survive. Use run for ad-hoc/temporary work.
  Script can use globals/functions defined by autoload or previously run scripts.
  require() resolves relative to the script's directory and the autoload directory.
- stop: Stop script, cleanup hooks/traces/breakpoints/widgets. Returns {stopped:true}.
- status: Returns {loaded, script_path, run_files: [paths], errors?: [strings]}.
  errors field present only if autoload or run scripts had load errors.
- logs: Drain log buffer. Returns {count, logs: [{level, system, msg}]}. Levels: 0=info, 1=warn, 2=error.
- docs: Get full Lua API reference (includes multi-script composition guide). Returns plain text documentation string.
- widgets: List registered UI widgets. Returns {widgets: [{key, type, label, group, value, ...}]}.
- widget_get: Get a widget's current value. Param: key. Returns {key, value}.
- widget_set: Set a widget's value. Params: key, value.
- widget_click: Click a button widget. Param: key. Returns {ok, logs}.

Workflow (autoload + reset):
  1. Write/edit .lua scripts in ~/.config/PCSX2/lua-autoload/ (prefix with 01_, 02_ for order).
  2. Reset the game (emu_control reset) — destroys ALL Lua state, then re-runs only autoload scripts.
     Hooks from "exec" and "run" are lost on reset. Only autoload scripts persist across resets.
  3. Scripts share a single Lua state: use globals (not local) to pass data between scripts.
  4. Multiple scripts can hook the same address — all callbacks fire in load order.
  5. Read results via "lua exec" (e.g. `return my_global`) or "lua logs".
  6. Check "lua status" for errors field to see if any autoload scripts failed.
  Use "run" to additively load a script into the existing state without reset.
  require("module") works — searches script's dir, then autoload dir."""
    if action == "exec":
        if not code:
            raise PCSX2Error("Missing 'code' for exec action")
        return R(send_command("execute_lua", {"code": code}))
    elif action == "run":
        if not path:
            raise PCSX2Error("Missing 'path' for run action")
        return R(send_command("lua_script", {"action": "run", "path": path}))
    elif action == "stop":
        return R(send_command("lua_script", {"action": "stop"}))
    elif action == "status":
        return R(send_command("lua_script", {"action": "status"}))
    elif action == "logs":
        return R(send_command("lua_logs"))
    elif action == "docs":
        return send_command("lua_docs").get("docs", "")
    elif action == "widgets":
        return R(send_command("lua_commands", {"action": "list"}))
    elif action == "widget_get":
        if not key:
            raise PCSX2Error("Missing 'key' for widget_get action")
        return R(send_command("lua_commands", {"action": "get", "key": key}))
    elif action == "widget_set":
        if not key:
            raise PCSX2Error("Missing 'key' for widget_set action")
        return R(send_command("lua_commands", {"action": "set", "key": key, "value": value}))
    elif action == "widget_click":
        if not key:
            raise PCSX2Error("Missing 'key' for widget_click action")
        return R(send_command("lua_commands", {"action": "click", "key": key}))
    else:
        raise PCSX2Error(f"Unknown action: {action}")


if __name__ == "__main__":
    mcp.run(transport="stdio")
