// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugServer.h"
#include "LuaHooks.h"
#include "LuaEngine.h"
#include "MemTracker.h"
#include "DebugTools/Breakpoints.h"
#include "DebugTools/MemoryTrace.h"
#include "Memory.h"
#include "R5900.h"
#include "VMManager.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <WinSock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>
#endif
#include <cerrno>
#include <cstring>
#include <optional>

using namespace rapidjson;

// Cross-platform socket portability
#ifdef _WIN32
#define poll_portable WSAPoll
#define close_socket closesocket
using ssize_portable = int;
static bool InitializeWinsock()
{
	WSADATA wsa{};
	return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
#else
#define poll_portable poll
#define close_socket close
using ssize_portable = ssize_t;
#endif

static std::string SocketErrorString()
{
#ifdef _WIN32
	const int err = WSAGetLastError();
	char buf[256];
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		buf, sizeof(buf), nullptr);
	return buf;
#else
	return strerror(errno);
#endif
}

// Forward declarations for command handlers (defined below)
// These are free functions because rapidjson types can't appear in the header
// (Qt PCH's QJsonPrivate::Value conflicts with the forward declaration)
using CmdHandler = std::string (*)(const Value& params);

static std::string cmdStatus(const Value& params);
static std::string cmdPause(const Value& params);
static std::string cmdResume(const Value& params);
static std::string cmdFrameAdvance(const Value& params);
static std::string cmdReadMemory(const Value& params);
static std::string cmdWriteMemory(const Value& params);
static std::string cmdReadRegisters(const Value& params);
static std::string cmdSetRegister(const Value& params);
static std::string cmdAddBreakpoint(const Value& params);
static std::string cmdRemoveBreakpoint(const Value& params);
static std::string cmdWaitBreakpoint(const Value& params);
static std::string cmdListBreakpoints(const Value& params);
static std::string cmdAddMemWatch(const Value& params);
static std::string cmdRemoveMemWatch(const Value& params);
static std::string cmdAllocations(const Value& params);
static std::string cmdMemTrace(const Value& params);
static std::string cmdTraceResults(const Value& params);
static std::string cmdHexdump(const Value& params);
static std::string cmdExecuteLua(const Value& params);
static std::string cmdLuaScript(const Value& params);
static std::string cmdLuaLogs(const Value& params);
static std::string cmdLuaDocs(const Value& params);
static std::string cmdReset(const Value& params);
static std::string cmdLuaCommands(const Value& params);

static std::string jsonOk(const std::string& resultJson = "{}");
static std::string jsonError(const std::string& msg);
static std::optional<u32> parseHexAddress(const Value& params, const char* key);

// ============================================================
// Singleton
// ============================================================

DebugServer& DebugServer::instance()
{
	static DebugServer s;
	return s;
}

// ============================================================
// Server lifecycle
// ============================================================

bool DebugServer::start(u16 port)
{
	if (m_running.load())
		return true;

	m_port = port ? port : DEFAULT_PORT;

#ifdef _WIN32
	if (!InitializeWinsock())
	{
		Console.Error("[DebugServer] WSAStartup failed");
		return false;
	}
#endif

	m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
	if (m_serverFd == INVALID_SOCK)
	{
		Console.Error("[DebugServer] socket() failed: %s", SocketErrorString().c_str());
		return false;
	}

	int reuse = 1;
	setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(m_port);

	if (bind(m_serverFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
	{
		Console.Error("[DebugServer] bind(127.0.0.1:%u) failed: %s", m_port, SocketErrorString().c_str());
		close_socket(m_serverFd);
		m_serverFd = INVALID_SOCK;
		return false;
	}

	if (listen(m_serverFd, 1) < 0)
	{
		Console.Error("[DebugServer] listen() failed: %s", SocketErrorString().c_str());
		close_socket(m_serverFd);
		m_serverFd = INVALID_SOCK;
		return false;
	}

	m_running.store(true);
	m_thread = std::thread(&DebugServer::serverLoop, this);

	Console.WriteLn("[DebugServer] Listening on 127.0.0.1:%u", m_port);
	return true;
}

void DebugServer::stop()
{
	if (!m_running.load())
		return;

	m_running.store(false);

	// Interrupt accept() by closing server fd
	if (m_serverFd != INVALID_SOCK)
	{
#ifdef _WIN32
		shutdown(m_serverFd, SD_BOTH);
#else
		shutdown(m_serverFd, SHUT_RDWR);
#endif
		close_socket(m_serverFd);
		m_serverFd = INVALID_SOCK;
	}

	if (m_thread.joinable())
		m_thread.join();

	// Cleanup any breakpoints/memwatches we installed
	{
		std::lock_guard lock(m_bpMutex);
		for (u32 addr : m_breakpoints)
			CBreakPoints::RemoveBreakPoint(BREAKPOINT_EE, addr);
		m_breakpoints.clear();
		for (const auto& mw : m_memWatches)
			CBreakPoints::RemoveMemCheck(BREAKPOINT_EE, mw.start, mw.start + mw.size);
		m_memWatches.clear();
	}

#ifdef _WIN32
	WSACleanup();
#endif

	Console.WriteLn("[DebugServer] Stopped.");
}

void DebugServer::trackBreakpoint(u32 addr)
{
	std::lock_guard lock(m_bpMutex);
	m_breakpoints.push_back(addr);
}

void DebugServer::untrackBreakpoint(u32 addr)
{
	std::lock_guard lock(m_bpMutex);
	auto it = std::find(m_breakpoints.begin(), m_breakpoints.end(), addr);
	if (it != m_breakpoints.end())
		m_breakpoints.erase(it);
}

void DebugServer::trackMemWatch(u32 start, u32 size)
{
	std::lock_guard lock(m_bpMutex);
	m_memWatches.push_back({start, size});
}

void DebugServer::untrackMemWatch(u32 start, u32 size)
{
	std::lock_guard lock(m_bpMutex);
	auto it = std::find_if(m_memWatches.begin(), m_memWatches.end(),
		[start, size](const MemWatchEntry& e) { return e.start == start && e.size == size; });
	if (it != m_memWatches.end())
		m_memWatches.erase(it);
}

std::vector<u32> DebugServer::getTrackedBreakpoints()
{
	std::lock_guard lock(m_bpMutex);
	return m_breakpoints;
}

std::vector<DebugServer::MemWatchEntry> DebugServer::getTrackedMemWatches()
{
	std::lock_guard lock(m_bpMutex);
	return m_memWatches;
}

// ============================================================
// Server loop — accept clients one at a time
// ============================================================

void DebugServer::serverLoop()
{
	while (m_running.load())
	{
		// Poll with timeout so we can check m_running
		struct pollfd pfd = {};
		pfd.fd = m_serverFd;
		pfd.events = POLLIN;

		int ret = poll_portable(&pfd, 1, 500); // 500ms timeout
		if (ret <= 0)
			continue;

		socket_t clientFd = accept(m_serverFd, nullptr, nullptr);
		if (clientFd == INVALID_SOCK)
		{
			if (m_running.load())
				Console.Error("[DebugServer] accept() failed: %s", SocketErrorString().c_str());
			continue;
		}

		Console.WriteLn("[DebugServer] Client connected.");
		handleClient(clientFd);
		close_socket(clientFd);
		Console.WriteLn("[DebugServer] Client disconnected.");
	}
}

void DebugServer::handleClient(socket_t clientFd)
{
	// Line-buffered reading
	std::string buffer;
	char chunk[4096];

	while (m_running.load())
	{
		struct pollfd pfd = {};
		pfd.fd = clientFd;
		pfd.events = POLLIN;

		int ret = poll_portable(&pfd, 1, 500);
		if (ret < 0)
			break;
		if (ret == 0)
			continue;

		ssize_portable n = recv(clientFd, chunk, sizeof(chunk), 0);
		if (n <= 0)
			break;

		buffer.append(chunk, n);

		// Process complete lines
		size_t pos;
		while ((pos = buffer.find('\n')) != std::string::npos)
		{
			std::string line = buffer.substr(0, pos);
			buffer.erase(0, pos + 1);

			if (line.empty())
				continue;

			std::string response = dispatchCommand(line);
			response += '\n';

			ssize_portable written = 0;
			ssize_portable total = static_cast<ssize_portable>(response.size());
			while (written < total)
			{
				ssize_portable w = send(clientFd, response.data() + written, total - written, 0);
				if (w <= 0)
					return;
				written += w;
			}
		}
	}
}

// ============================================================
// Command dispatch
// ============================================================

std::string DebugServer::dispatchCommand(const std::string& jsonLine)
{
	Document doc;
	doc.Parse(jsonLine.c_str());

	if (doc.HasParseError())
		return jsonError("Invalid JSON");

	if (!doc.HasMember("cmd") || !doc["cmd"].IsString())
		return jsonError("Missing 'cmd' field");

	const char* cmd = doc["cmd"].GetString();

	// Use empty object if no params
	Value emptyParams(kObjectType);
	const Value& params = doc.HasMember("params") ? doc["params"] : emptyParams;

	// Dispatch table
	if (std::strcmp(cmd, "status") == 0) return cmdStatus(params);
	if (std::strcmp(cmd, "pause") == 0) return cmdPause(params);
	if (std::strcmp(cmd, "resume") == 0) return cmdResume(params);
	if (std::strcmp(cmd, "frame_advance") == 0) return cmdFrameAdvance(params);
	if (std::strcmp(cmd, "read_memory") == 0) return cmdReadMemory(params);
	if (std::strcmp(cmd, "write_memory") == 0) return cmdWriteMemory(params);
	if (std::strcmp(cmd, "read_registers") == 0) return cmdReadRegisters(params);
	if (std::strcmp(cmd, "set_register") == 0) return cmdSetRegister(params);
	if (std::strcmp(cmd, "add_breakpoint") == 0) return cmdAddBreakpoint(params);
	if (std::strcmp(cmd, "remove_breakpoint") == 0) return cmdRemoveBreakpoint(params);
	if (std::strcmp(cmd, "wait_breakpoint") == 0) return cmdWaitBreakpoint(params);
	if (std::strcmp(cmd, "list_breakpoints") == 0) return cmdListBreakpoints(params);
	if (std::strcmp(cmd, "add_memwatch") == 0) return cmdAddMemWatch(params);
	if (std::strcmp(cmd, "remove_memwatch") == 0) return cmdRemoveMemWatch(params);
	if (std::strcmp(cmd, "allocations") == 0) return cmdAllocations(params);
	if (std::strcmp(cmd, "mem_trace") == 0) return cmdMemTrace(params);
	if (std::strcmp(cmd, "mem_trace_results") == 0) return cmdTraceResults(params);
	if (std::strcmp(cmd, "hexdump") == 0) return cmdHexdump(params);
	if (std::strcmp(cmd, "execute_lua") == 0) return cmdExecuteLua(params);
	if (std::strcmp(cmd, "lua_script") == 0) return cmdLuaScript(params);
	if (std::strcmp(cmd, "lua_logs") == 0) return cmdLuaLogs(params);
	if (std::strcmp(cmd, "lua_docs") == 0) return cmdLuaDocs(params);
	if (std::strcmp(cmd, "reset") == 0) return cmdReset(params);
	if (std::strcmp(cmd, "lua_commands") == 0) return cmdLuaCommands(params);

	return jsonError(std::string("Unknown command: ") + cmd);
}

// ============================================================
// JSON helpers
// ============================================================

std::string jsonOk(const std::string& resultJson)
{
	if (resultJson.empty())
		return R"({"ok":true,"result":null})";
	return std::string(R"({"ok":true,"result":)") + resultJson + "}";
}

std::string jsonError(const std::string& msg)
{
	StringBuffer sb;
	Writer<StringBuffer> w(sb);
	w.StartObject();
	w.Key("ok"); w.Bool(false);
	w.Key("error"); w.String(msg.c_str());
	w.EndObject();
	return sb.GetString();
}

static std::optional<u32> parseHexAddress(const Value& params, const char* key)
{
	if (!params.HasMember(key))
		return std::nullopt;
	const Value& v = params[key];
	if (v.IsString())
	{
		const char* s = v.GetString();
		return static_cast<u32>(std::strtoul(s, nullptr, 16));
	}
	if (v.IsUint())
		return v.GetUint();
	return std::nullopt;
}

static bool getBoolOrDefault(const Value& params, const char* key, bool defaultVal)
{
	if (!params.HasMember(key))
		return defaultVal;
	const Value& v = params[key];
	if (v.IsBool())
		return v.GetBool();
	if (v.IsInt())
		return v.GetInt() != 0;
	return defaultVal;
}

// Shared GPR name table
static const char* kGprNames[] = {
	"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
	"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
	"t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

// ============================================================
// Region-to-JSON helper for cmdAllocations
// ============================================================

static void writeRegionJson(Writer<StringBuffer>& w, const TrackedRegion& region)
{
	w.Key("address"); w.String(fmt::format("0x{:08x}", region.address).c_str());
	w.Key("size"); w.Uint(region.size);
	w.Key("parent"); w.String(fmt::format("0x{:08x}", region.parentAddr).c_str());
	w.Key("name"); w.String(region.name.c_str());
	w.Key("tags"); w.StartObject();
	for (const auto& [k, v] : region.tags)
	{
		w.Key(k.c_str());
		w.String(v.c_str());
	}
	w.EndObject();
}

// ============================================================
// Emulator control commands
// ============================================================

static const char* pauseReasonStr(VMPauseReason r)
{
	switch (r)
	{
		case VMPauseReason::None: return "none";
		case VMPauseReason::UserRequest: return "user_request";
		case VMPauseReason::Breakpoint: return "breakpoint";
		case VMPauseReason::MemWatch: return "memwatch";
		case VMPauseReason::TLBMiss: return "tlb_miss";
		case VMPauseReason::BusError: return "bus_error";
		case VMPauseReason::VIFError: return "vif_error";
		case VMPauseReason::DMAError: return "dma_error";
		case VMPauseReason::FrameAdvance: return "frame_advance";
		case VMPauseReason::InputRecording: return "input_recording";
		case VMPauseReason::LuaAssert: return "lua_assert";
	}
	return "unknown";
}

static void writePauseInfo(Writer<StringBuffer>& w)
{
	VMPauseInfo info = VMManager::GetPauseInfo();
	w.Key("pause_reason"); w.String(pauseReasonStr(info.reason));
	if (!info.message.empty())
	{
		w.Key("pause_message"); w.String(info.message.c_str());
	}
	if (info.pc != 0)
	{
		w.Key("pause_pc"); w.String(fmt::format("0x{:08x}", info.pc).c_str());
	}
}

std::string cmdStatus(const Value& /*params*/)
{
	StringBuffer sb;
	Writer<StringBuffer> w(sb);
	w.StartObject();

	VMState state = VMManager::GetState();
	const char* stateStr = "unknown";
	switch (state)
	{
		case VMState::Shutdown: stateStr = "shutdown"; break;
		case VMState::Initializing: stateStr = "initializing"; break;
		case VMState::Running: stateStr = "running"; break;
		case VMState::Paused: stateStr = "paused"; break;
		case VMState::Resetting: stateStr = "resetting"; break;
		case VMState::Stopping: stateStr = "stopping"; break;
	}
	w.Key("state"); w.String(stateStr);
	w.Key("paused"); w.Bool(state == VMState::Paused);

	if (state == VMState::Paused)
		writePauseInfo(w);

	if (VMManager::HasValidVM())
	{
		w.Key("pc"); w.String(fmt::format("0x{:08x}", cpuRegs.pc).c_str());

		std::string serial = VMManager::GetDiscSerial();
		w.Key("disc_serial"); w.String(serial.c_str());
		w.Key("disc_crc"); w.String(fmt::format("0x{:08x}", VMManager::GetDiscCRC()).c_str());
	}

	w.EndObject();
	return jsonOk(sb.GetString());
}

std::string cmdPause(const Value& /*params*/)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");
	VMManager::SetPaused(true);
	return jsonOk(R"({"paused":true})");
}

std::string cmdResume(const Value& /*params*/)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");
	VMManager::SetPaused(false);
	return jsonOk(R"({"paused":false})");
}

std::string cmdFrameAdvance(const Value& params)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");
	u32 frames = 1;
	if (params.HasMember("frames") && params["frames"].IsUint())
		frames = params["frames"].GetUint();
	VMManager::FrameAdvance(frames);
	return jsonOk(fmt::format(R"({{"frames":{}}})", frames));
}


// ============================================================
// Memory access
// ============================================================

std::string cmdReadMemory(const Value& params)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");
	if (VMManager::GetState() != VMState::Paused)
		return jsonError("VM must be paused to read/write CPU state");

	auto addrOpt = parseHexAddress(params, "address");
	if (!addrOpt)
		return jsonError("Missing required 'address'");
	u32 addr = *addrOpt;

	u32 size = 4;
	if (params.HasMember("size") && params["size"].IsUint())
		size = params["size"].GetUint();
	if (size > 4096)
		return jsonError("Size exceeds maximum (4096)");

	// Bounds check (PS2 RAM is 32MB)
	if (addr + size > 0x02000000)
		return jsonError("Address out of PS2 RAM range");

	std::string format = "hex";
	if (params.HasMember("format") && params["format"].IsString())
		format = params["format"].GetString();

	const u8* ptr = reinterpret_cast<const u8*>(PSM(addr));

	StringBuffer sb;
	Writer<StringBuffer> w(sb);
	w.StartObject();

	if (format == "hex")
	{
		std::string hex;
		hex.reserve(size * 2);
		for (u32 i = 0; i < size; i++)
		{
			char buf[3];
			std::snprintf(buf, sizeof(buf), "%02x", ptr[i]);
			hex += buf;
		}
		w.Key("hex"); w.String(hex.c_str());
	}
	else if (format == "u8")
	{
		w.Key("values");
		w.StartArray();
		for (u32 i = 0; i < size; i++)
			w.Uint(ptr[i]);
		w.EndArray();
	}
	else if (format == "u16")
	{
		w.Key("values");
		w.StartArray();
		for (u32 i = 0; i + 1 < size; i += 2)
			w.Uint(*reinterpret_cast<const u16*>(ptr + i));
		w.EndArray();
	}
	else if (format == "u32")
	{
		w.Key("values");
		w.StartArray();
		for (u32 i = 0; i + 3 < size; i += 4)
			w.Uint(*reinterpret_cast<const u32*>(ptr + i));
		w.EndArray();
	}
	else if (format == "float")
	{
		w.Key("values");
		w.StartArray();
		for (u32 i = 0; i + 3 < size; i += 4)
			w.Double(*reinterpret_cast<const float*>(ptr + i));
		w.EndArray();
	}
	else if (format == "string")
	{
		const char* cptr = reinterpret_cast<const char*>(ptr);
		u32 len = 0;
		while (len < size && cptr[len] != '\0')
			len++;
		w.Key("string"); w.String(cptr, len);
	}
	else
	{
		return jsonError("Unknown format: " + format);
	}

	w.EndObject();
	return jsonOk(sb.GetString());
}

std::string cmdWriteMemory(const Value& params)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");

	if (VMManager::GetState() != VMState::Paused)
		return jsonError("VM must be paused to write memory");

	auto addrOpt = parseHexAddress(params, "address");
	if (!addrOpt)
		return jsonError("Missing required 'address'");
	u32 addr = *addrOpt;

	std::string format = "hex";
	if (params.HasMember("format") && params["format"].IsString())
		format = params["format"].GetString();

	if (addr + 1 > 0x02000000)
		return jsonError("Address out of PS2 RAM range");

	u8* ptr = reinterpret_cast<u8*>(PSM(addr));

	if (format == "hex")
	{
		if (!params.HasMember("hex") || !params["hex"].IsString())
			return jsonError("Missing 'hex' parameter");
		const char* hexStr = params["hex"].GetString();
		size_t hexLen = std::strlen(hexStr);
		if (hexLen % 2 != 0)
			return jsonError("Hex string must have even length");
		u32 byteCount = static_cast<u32>(hexLen / 2);
		if (addr + byteCount > 0x02000000)
			return jsonError("Write exceeds PS2 RAM range");
		for (u32 i = 0; i < byteCount; i++)
		{
			char byte[3] = {hexStr[i * 2], hexStr[i * 2 + 1], '\0'};
			ptr[i] = static_cast<u8>(std::strtoul(byte, nullptr, 16));
		}
		return jsonOk(fmt::format(R"({{"bytes_written":{}}})", byteCount));
	}
	else if (format == "u8" || format == "u16" || format == "u32" || format == "float")
	{
		if (!params.HasMember("values") || !params["values"].IsArray())
			return jsonError("Missing 'values' array");
		const auto& values = params["values"].GetArray();
		u32 written = 0;
		for (SizeType i = 0; i < values.Size(); i++)
		{
			if (format == "u8")
			{
				if (addr + written + 1 > 0x02000000) break;
				ptr[written] = static_cast<u8>(values[i].GetUint());
				written += 1;
			}
			else if (format == "u16")
			{
				if (addr + written + 2 > 0x02000000) break;
				*reinterpret_cast<u16*>(ptr + written) = static_cast<u16>(values[i].GetUint());
				written += 2;
			}
			else if (format == "u32")
			{
				if (addr + written + 4 > 0x02000000) break;
				*reinterpret_cast<u32*>(ptr + written) = values[i].GetUint();
				written += 4;
			}
			else if (format == "float")
			{
				if (addr + written + 4 > 0x02000000) break;
				*reinterpret_cast<float*>(ptr + written) = static_cast<float>(values[i].GetDouble());
				written += 4;
			}
		}
		return jsonOk(fmt::format(R"({{"bytes_written":{}}})", written));
	}
	else if (format == "string")
	{
		if (!params.HasMember("string") || !params["string"].IsString())
			return jsonError("Missing 'string' parameter");
		const char* str = params["string"].GetString();
		size_t len = std::strlen(str);
		if (addr + len + 1 > 0x02000000)
			return jsonError("Write exceeds PS2 RAM range");
		std::memcpy(ptr, str, len + 1);
		return jsonOk(fmt::format(R"({{"bytes_written":{}}})", len + 1));
	}

	return jsonError("Unknown format: " + format);
}

// ============================================================
// Registers
// ============================================================

static int gprNameToIndex(const char* name)
{
	for (int i = 0; i < 32; i++)
	{
		if (std::strcmp(name, kGprNames[i]) == 0)
			return i;
	}
	return -1;
}

std::string cmdReadRegisters(const Value& params)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");
	if (VMManager::GetState() != VMState::Paused)
		return jsonError("VM must be paused to read/write CPU state");

	StringBuffer sb;
	Writer<StringBuffer> w(sb);
	w.StartObject();

	if (params.HasMember("names") && params["names"].IsArray())
	{
		const auto& names = params["names"].GetArray();
		for (SizeType i = 0; i < names.Size(); i++)
		{
			if (!names[i].IsString()) continue;
			const char* name = names[i].GetString();

			if (std::strcmp(name, "pc") == 0)
			{
				w.Key("pc"); w.String(fmt::format("0x{:08x}", cpuRegs.pc).c_str());
			}
			else if (std::strcmp(name, "hi") == 0)
			{
				w.Key("hi"); w.String(fmt::format("0x{:08x}", cpuRegs.HI.UL[0]).c_str());
			}
			else if (std::strcmp(name, "lo") == 0)
			{
				w.Key("lo"); w.String(fmt::format("0x{:08x}", cpuRegs.LO.UL[0]).c_str());
			}
			else
			{
				int idx = gprNameToIndex(name);
				if (idx >= 0)
				{
					w.Key(name);
					w.String(fmt::format("0x{:08x}", cpuRegs.GPR.r[idx].UL[0]).c_str());
				}
			}
		}
	}
	else
	{
		// Return all GPRs + pc
		for (int i = 0; i < 32; i++)
		{
			w.Key(kGprNames[i]);
			w.String(fmt::format("0x{:08x}", cpuRegs.GPR.r[i].UL[0]).c_str());
		}
		w.Key("pc"); w.String(fmt::format("0x{:08x}", cpuRegs.pc).c_str());
		w.Key("hi"); w.String(fmt::format("0x{:08x}", cpuRegs.HI.UL[0]).c_str());
		w.Key("lo"); w.String(fmt::format("0x{:08x}", cpuRegs.LO.UL[0]).c_str());
	}

	w.EndObject();
	return jsonOk(sb.GetString());
}

std::string cmdSetRegister(const Value& params)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");
	if (VMManager::GetState() != VMState::Paused)
		return jsonError("VM must be paused to read/write CPU state");

	if (!params.HasMember("sets") || !params["sets"].IsObject())
		return jsonError("Missing 'sets' object");

	u32 count = 0;
	for (auto it = params["sets"].MemberBegin(); it != params["sets"].MemberEnd(); ++it)
	{
		const char* name = it->name.GetString();
		u32 val = 0;
		if (it->value.IsString())
			val = static_cast<u32>(std::strtoul(it->value.GetString(), nullptr, 16));
		else if (it->value.IsUint())
			val = it->value.GetUint();

		if (std::strcmp(name, "pc") == 0)
		{
			cpuRegs.pc = val;
			count++;
		}
		else
		{
			int idx = gprNameToIndex(name);
			if (idx >= 0)
			{
				cpuRegs.GPR.r[idx].UL[0] = val;
				count++;
			}
		}
	}

	return jsonOk(fmt::format(R"({{"registers_set":{}}})", count));
}

// ============================================================
// Breakpoints
// ============================================================

std::string cmdAddBreakpoint(const Value& params)
{
	auto addrOpt = parseHexAddress(params, "address");
	if (!addrOpt)
		return jsonError("Missing required 'address'");
	u32 addr = *addrOpt;
	CBreakPoints::AddBreakPoint(BREAKPOINT_EE, addr);
	DebugServer::instance().trackBreakpoint(addr);
	return jsonOk(fmt::format(R"({{"address":"0x{:08x}"}})", addr));
}

std::string cmdRemoveBreakpoint(const Value& params)
{
	auto addrOpt = parseHexAddress(params, "address");
	if (!addrOpt)
		return jsonError("Missing required 'address'");
	u32 addr = *addrOpt;
	CBreakPoints::RemoveBreakPoint(BREAKPOINT_EE, addr);
	DebugServer::instance().untrackBreakpoint(addr);
	return jsonOk(fmt::format(R"({{"address":"0x{:08x}"}})", addr));
}

std::string cmdWaitBreakpoint(const Value& params)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");

	double timeoutSec = 10.0;
	if (params.HasMember("timeout") && params["timeout"].IsNumber())
		timeoutSec = params["timeout"].GetDouble();

	int timeoutMs = static_cast<int>(timeoutSec * 1000);
	int elapsed = 0;
	const int pollInterval = 10; // 10ms

	// If already paused, return immediately
	if (VMManager::GetState() == VMState::Paused)
	{
		StringBuffer sb;
		Writer<StringBuffer> w(sb);
		w.StartObject();
		w.Key("hit"); w.Bool(true);
		w.Key("pc"); w.String(fmt::format("0x{:08x}", cpuRegs.pc).c_str());
		w.Key("reason"); w.String("already_paused");
		w.EndObject();
		return jsonOk(sb.GetString());
	}

	// Poll for VM pause (breakpoint hit causes pause)
	while (elapsed < timeoutMs && DebugServer::instance().isRunning())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(pollInterval));
		elapsed += pollInterval;

		if (VMManager::GetState() == VMState::Paused)
		{
			StringBuffer sb;
			Writer<StringBuffer> w(sb);
			w.StartObject();
			w.Key("hit"); w.Bool(true);
			w.Key("pc"); w.String(fmt::format("0x{:08x}", cpuRegs.pc).c_str());

			// Include key registers
			w.Key("registers");
			w.StartObject();
			static const char* keyRegs[] = {"a0", "a1", "a2", "a3", "v0", "v1", "ra", "sp", "s0", "s1", "s2", "s3"};
			for (const char* name : keyRegs)
			{
				int idx = gprNameToIndex(name);
				if (idx >= 0)
				{
					w.Key(name);
					w.String(fmt::format("0x{:08x}", cpuRegs.GPR.r[idx].UL[0]).c_str());
				}
			}
			w.EndObject();

			w.EndObject();
			return jsonOk(sb.GetString());
		}
	}

	return jsonOk(R"({"hit":false,"reason":"timeout"})");
}

std::string cmdListBreakpoints(const Value& /*params*/)
{
	auto& srv = DebugServer::instance();
	StringBuffer sb;
	Writer<StringBuffer> w(sb);
	w.StartObject();
	w.Key("breakpoints");
	w.StartArray();

	auto breakpoints = srv.getTrackedBreakpoints();
	for (u32 addr : breakpoints)
	{
		w.StartObject();
		w.Key("address"); w.String(fmt::format("0x{:08x}", addr).c_str());
		w.Key("enabled"); w.Bool(CBreakPoints::IsAddressBreakPoint(BREAKPOINT_EE, addr));
		w.EndObject();
	}
	w.EndArray();

	w.Key("memwatches");
	w.StartArray();
	auto memWatches = srv.getTrackedMemWatches();
	for (const auto& mw : memWatches)
	{
		w.StartObject();
		w.Key("address"); w.String(fmt::format("0x{:08x}", mw.start).c_str());
		w.Key("size"); w.Uint(mw.size);
		w.EndObject();
	}
	w.EndArray();

	w.EndObject();
	return jsonOk(sb.GetString());
}

std::string cmdAddMemWatch(const Value& params)
{
	auto addrOpt = parseHexAddress(params, "address");
	if (!addrOpt)
		return jsonError("Missing required 'address'");
	u32 addr = *addrOpt;
	u32 size = 4;
	if (params.HasMember("size") && params["size"].IsUint())
		size = params["size"].GetUint();

	MemCheckCondition cond = MEMCHECK_READWRITE;
	if (params.HasMember("condition") && params["condition"].IsString())
	{
		const char* c = params["condition"].GetString();
		if (std::strcmp(c, "r") == 0) cond = MEMCHECK_READ;
		else if (std::strcmp(c, "w") == 0) cond = MEMCHECK_WRITE;
	}

	CBreakPoints::AddMemCheck(BREAKPOINT_EE, addr, addr + size, cond, MEMCHECK_BREAK);
	DebugServer::instance().trackMemWatch(addr, size);
	return jsonOk(fmt::format(R"({{"address":"0x{:08x}","size":{}}})", addr, size));
}

std::string cmdRemoveMemWatch(const Value& params)
{
	auto addrOpt = parseHexAddress(params, "address");
	if (!addrOpt)
		return jsonError("Missing required 'address'");
	u32 addr = *addrOpt;
	u32 size = 4;
	if (params.HasMember("size") && params["size"].IsUint())
		size = params["size"].GetUint();

	CBreakPoints::RemoveMemCheck(BREAKPOINT_EE, addr, addr + size);
	DebugServer::instance().untrackMemWatch(addr, size);
	return jsonOk(fmt::format(R"({{"address":"0x{:08x}","size":{}}})", addr, size));
}

std::string cmdAllocations(const Value& params)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");

	std::string action = "query";
	if (params.HasMember("action") && params["action"].IsString())
		action = params["action"].GetString();

	auto& tracker = MemTracker::instance();

	if (action == "count")
	{
		return jsonOk(fmt::format(R"({{"count":{}}})", tracker.regionCount()));
	}
	else if (action == "find")
	{
		auto addrOpt = parseHexAddress(params, "address");
		if (!addrOpt)
			return jsonError("Missing required 'address'");
		const TrackedRegion* region = tracker.findRegion(*addrOpt);
		if (!region)
			return jsonOk(R"({"found":false})");

		StringBuffer sb;
		Writer<StringBuffer> w(sb);
		w.StartObject();
		w.Key("found"); w.Bool(true);
		writeRegionJson(w, *region);
		w.EndObject();
		return jsonOk(sb.GetString());
	}
	else if (action == "find_containing")
	{
		auto addrOpt = parseHexAddress(params, "address");
		if (!addrOpt)
			return jsonError("Missing required 'address'");
		const TrackedRegion* region = tracker.findContaining(*addrOpt);
		if (!region)
			return jsonOk(R"({"found":false})");

		StringBuffer sb;
		Writer<StringBuffer> w(sb);
		w.StartObject();
		w.Key("found"); w.Bool(true);
		writeRegionJson(w, *region);
		w.EndObject();
		return jsonOk(sb.GetString());
	}
	else if (action == "query")
	{
		TrackedRegionQuery query;
		if (params.HasMember("parent") && params["parent"].IsString())
		{
			// Accept hex string like "0x00123456"
			const char* s = params["parent"].GetString();
			query.parentAddr = (u32)std::strtoul(s, nullptr, 16);
		}
		if (params.HasMember("contains"))
		{
			auto containsOpt = parseHexAddress(params, "contains");
			if (containsOpt)
				query.containsAddress = *containsOpt;
		}
		if (params.HasMember("tag") && params["tag"].IsObject())
		{
			for (auto it = params["tag"].MemberBegin(); it != params["tag"].MemberEnd(); ++it)
			{
				if (it->value.IsString())
					query.tags[it->name.GetString()] = it->value.GetString();
			}
		}

		u32 limit = 100;
		if (params.HasMember("limit") && params["limit"].IsUint())
			limit = params["limit"].GetUint();

		auto results = tracker.query(query);

		StringBuffer sb;
		Writer<StringBuffer> w(sb);
		w.StartObject();
		w.Key("count"); w.Uint(static_cast<unsigned>(results.size()));
		w.Key("allocations");
		w.StartArray();
		u32 count = 0;
		for (const auto* region : results)
		{
			if (count >= limit) break;
			w.StartObject();
			writeRegionJson(w, *region);
			w.EndObject();
			count++;
		}
		w.EndArray();
		w.EndObject();
		return jsonOk(sb.GetString());
	}
	else if (action == "tag")
	{
		auto addrOpt = parseHexAddress(params, "address");
		if (!addrOpt)
			return jsonError("Missing required 'address'");
		if (!params.HasMember("key") || !params["key"].IsString())
			return jsonError("Missing 'key'");
		if (!params.HasMember("value") || !params["value"].IsString())
			return jsonError("Missing 'value'");
		tracker.addTag(*addrOpt, params["key"].GetString(), params["value"].GetString());
		return jsonOk(R"({"tagged":true})");
	}
	else if (action == "untag")
	{
		auto addrOpt = parseHexAddress(params, "address");
		if (!addrOpt)
			return jsonError("Missing required 'address'");
		if (!params.HasMember("key") || !params["key"].IsString())
			return jsonError("Missing 'key'");
		tracker.removeTag(*addrOpt, params["key"].GetString());
		return jsonOk(R"({"untagged":true})");
	}

	return jsonError("Unknown action: " + action);
}

std::string cmdMemTrace(const Value& params)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");

	std::string action = "start";
	if (params.HasMember("action") && params["action"].IsString())
		action = params["action"].GetString();

	auto& traceMgr = MemoryTraceManager::Instance();

	if (action == "start")
	{
		auto addrOpt = parseHexAddress(params, "address");
		if (!addrOpt)
			return jsonError("Missing required 'address'");
		u32 addr = *addrOpt;
		const TrackedRegion* region = MemTracker::instance().findRegion(addr);
		if (!region)
			return jsonError(fmt::format("No region at 0x{:08x}", addr));

		std::string name = "mcp_trace";
		if (params.HasMember("name") && params["name"].IsString())
			name = params["name"].GetString();

		u32 flags = 0;
		if (getBoolOrDefault(params, "reads", true))
			flags |= MEMTRACE_TRACK_READS;
		if (getBoolOrDefault(params, "writes", false))
			flags |= MEMTRACE_TRACK_WRITES;
		if (getBoolOrDefault(params, "dma", false))
			flags |= MEMTRACE_TRACK_DMA;

		u32 hookId = traceMgr.RegisterHook(name, nullptr, flags);
		traceMgr.AddTracedRange(hookId, region->address, region->size);

		return jsonOk(fmt::format(R"({{"hook_id":{},"address":"0x{:08x}","size":{}}})",
			hookId, region->address, region->size));
	}
	else if (action == "stop")
	{
		if (!params.HasMember("hook_id") || !params["hook_id"].IsUint())
			return jsonError("Missing 'hook_id'");
		u32 hookId = params["hook_id"].GetUint();
		traceMgr.UnregisterHook(hookId);
		return jsonOk(fmt::format(R"({{"hook_id":{},"stopped":true}})", hookId));
	}
	else if (action == "flush_all")
	{
		MemTracker::instance().flushAllTraces();
		return jsonOk(R"({"flushed":true})");
	}

	return jsonError("Unknown action: " + action);
}

std::string cmdTraceResults(const Value& params)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");

	if (!params.HasMember("hook_id") || !params["hook_id"].IsUint())
		return jsonError("Missing 'hook_id'");

	u32 hookId = params["hook_id"].GetUint();
	auto& traceMgr = MemoryTraceManager::Instance();
	auto stats = traceMgr.GetHookTraceStats(hookId);

	bool flush = true;
	if (params.HasMember("flush") && params["flush"].IsBool())
		flush = params["flush"].GetBool();

	StringBuffer sb;
	Writer<StringBuffer> w(sb);
	w.StartObject();
	w.Key("hook_id"); w.Uint(hookId);
	w.Key("entries"); w.Uint(static_cast<unsigned>(stats.size()));
	w.Key("stats");
	w.StartArray();
	for (const auto& entry : stats)
	{
		w.StartObject();
		w.Key("offset"); w.Uint(entry.key.offset);
		w.Key("count"); w.Uint(entry.count);
		w.Key("is_dma"); w.Bool(entry.isDma);
		w.Key("stack");
		w.StartArray();
		for (u32 pc : entry.key.stack.pcs)
		{
			if (pc == 0)
				break;
			w.String(fmt::format("0x{:08x}", pc).c_str());
		}
		w.EndArray();
		w.EndObject();
	}
	w.EndArray();
	w.EndObject();

	if (flush)
		traceMgr.FlushHook(hookId);

	return jsonOk(sb.GetString());
}

// ============================================================
// Hexdump
// ============================================================

std::string cmdHexdump(const Value& params)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");

	auto addrOpt = parseHexAddress(params, "address");
	if (!addrOpt)
		return jsonError("Missing required 'address'");
	u32 addr = *addrOpt;
	u32 size = 256;
	if (params.HasMember("size") && params["size"].IsUint())
		size = params["size"].GetUint();
	if (size > 4096)
		size = 4096;
	if (addr + size > 0x02000000)
		return jsonError("Address range exceeds PS2 RAM");

	const u8* ptr = reinterpret_cast<const u8*>(PSM(addr));

	std::string dump;
	dump.reserve(size * 5); // rough estimate

	for (u32 offset = 0; offset < size; offset += 16)
	{
		dump += fmt::format("{:08x}  ", addr + offset);

		// Hex bytes
		for (u32 i = 0; i < 16; i++)
		{
			if (offset + i < size)
				dump += fmt::format("{:02x} ", ptr[offset + i]);
			else
				dump += "   ";
			if (i == 7)
				dump += " ";
		}

		dump += " |";
		// ASCII
		for (u32 i = 0; i < 16 && offset + i < size; i++)
		{
			u8 c = ptr[offset + i];
			dump += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.';
		}
		dump += "|\n";
	}

	StringBuffer sb;
	Writer<StringBuffer> w(sb);
	w.StartObject();
	w.Key("address"); w.String(fmt::format("0x{:08x}", addr).c_str());
	w.Key("size"); w.Uint(size);
	w.Key("dump"); w.String(dump.c_str());
	w.EndObject();
	return jsonOk(sb.GetString());
}

// ============================================================
// Lua execution
// ============================================================

static std::string luaLogsToJson(const std::vector<LuaEngine::LogEntry>& logs)
{
	StringBuffer sb;
	Writer<StringBuffer> w(sb);
	w.StartArray();
	for (const auto& entry : logs)
	{
		w.StartObject();
		w.Key("level"); w.Int(entry.level);
		w.Key("system"); w.String(entry.system.c_str());
		w.Key("msg"); w.String(entry.msg.c_str());
		w.EndObject();
	}
	w.EndArray();
	return sb.GetString();
}

std::string cmdExecuteLua(const Value& params)
{
	if (!params.HasMember("code") || !params["code"].IsString())
		return jsonError("Missing 'code' parameter");

	const char* code = params["code"].GetString();
	auto& lua = LuaEngine::instance();

	// Drain stale logs before execution
	lua.drainLogs();

	std::string returnValue;
	bool ok = lua.executeString(code, returnValue);

	// Capture logs generated during execution
	auto logs = lua.drainLogs();

	StringBuffer sb;
	Writer<StringBuffer> w(sb);
	w.StartObject();
	w.Key("ok"); w.Bool(ok);
	if (!returnValue.empty())
	{
		w.Key("return"); w.String(returnValue.c_str());
	}
	std::string logsJson = luaLogsToJson(logs);
	w.Key("logs"); w.RawValue(logsJson.c_str(), logsJson.size(), kArrayType);
	w.EndObject();
	return jsonOk(sb.GetString());
}

std::string cmdLuaScript(const Value& params)
{
	std::string action = "status";
	if (params.HasMember("action") && params["action"].IsString())
		action = params["action"].GetString();

	auto& lua = LuaEngine::instance();

	// Pause VM during Lua lifecycle operations — destroyState calls
	// removeExecutionHook which calls recClear, unsafe while EE is running.
	auto pauseGuard = [&]() -> bool {
		bool wasPaused = !VMManager::HasValidVM() || VMManager::GetState() == VMState::Paused;
		if (!wasPaused)
			VMManager::SetPaused(true);
		return wasPaused;
	};
	auto resumeGuard = [&](bool wasPaused) {
		if (!wasPaused && VMManager::HasValidVM())
			VMManager::SetPaused(false);
	};

	if (action == "stop")
	{
		bool wasPaused = pauseGuard();
		lua.stopScript();
		resumeGuard(wasPaused);
		return jsonOk(R"({"stopped":true})");
	}
	else if (action == "run")
	{
		if (!params.HasMember("path") || !params["path"].IsString())
			return jsonError("Missing 'path' parameter");
		const char* path = params["path"].GetString();

		lua.drainLogs();
		bool wasPaused = pauseGuard();
		bool ok = lua.runFile(path);
		resumeGuard(wasPaused);
		auto logs = lua.drainLogs();

		StringBuffer sb;
		Writer<StringBuffer> w(sb);
		w.StartObject();
		w.Key("ok"); w.Bool(ok);
		w.Key("path"); w.String(path);
		std::string logsJson = luaLogsToJson(logs);
		w.Key("logs"); w.RawValue(logsJson.c_str(), logsJson.size(), kArrayType);
		w.EndObject();
		return jsonOk(sb.GetString());
	}
	else if (action == "status")
	{
		StringBuffer sb;
		Writer<StringBuffer> w(sb);
		w.StartObject();
		w.Key("loaded"); w.Bool(lua.isLoaded());
		std::string scriptPath = lua.scriptPath();
		w.Key("script_path"); w.String(scriptPath.c_str());
		auto runFiles = lua.runFiles();
		w.Key("run_files");
		w.StartArray();
		for (const auto& f : runFiles)
			w.String(f.c_str());
		w.EndArray();
		auto errors = lua.scriptErrors();
		if (!errors.empty())
		{
			w.Key("errors");
			w.StartArray();
			for (const auto& e : errors)
				w.String(e.c_str());
			w.EndArray();
		}
		w.EndObject();
		return jsonOk(sb.GetString());
	}

	return jsonError("Unknown action: " + action);
}

std::string cmdLuaLogs(const Value& /*params*/)
{
	auto logs = LuaEngine::instance().drainLogs();

	StringBuffer sb;
	Writer<StringBuffer> w(sb);
	w.StartObject();
	w.Key("count"); w.Int(static_cast<int>(logs.size()));
	std::string logsJson = luaLogsToJson(logs);
	w.Key("logs"); w.RawValue(logsJson.c_str(), logsJson.size(), kArrayType);
	w.EndObject();
	return jsonOk(sb.GetString());
}

std::string cmdReset(const Value& params)
{
	if (!VMManager::HasValidVM())
		return jsonError("VM not running");

	// Default: resume after reset (most common workflow)
	bool resume = true;
	if (params.HasMember("resume") && params["resume"].IsBool())
		resume = params["resume"].GetBool();

	// Re-read autoload scripts from disk before reset
	// Must pause VM first since stopScript removes execution hooks (calls recClear)
	bool wasPaused = VMManager::GetState() == VMState::Paused;
	if (!wasPaused)
		VMManager::SetPaused(true);
	LuaHooks::reloadScripts();

	VMManager::Reset();

	if (resume)
		VMManager::SetPaused(false);

	return jsonOk(fmt::format(R"({{"reset":true,"resumed":{}}})", resume ? "true" : "false"));
}

static const char* widgetTypeName(LuaEngine::LuaWidget::Type t)
{
	switch (t)
	{
		case LuaEngine::LuaWidget::Type::Text: return "text";
		case LuaEngine::LuaWidget::Type::Checkbox: return "checkbox";
		case LuaEngine::LuaWidget::Type::Slider: return "slider";
		case LuaEngine::LuaWidget::Type::Button: return "button";
	}
	return "unknown";
}

std::string cmdLuaCommands(const Value& params)
{
	std::string action = "list";
	if (params.HasMember("action") && params["action"].IsString())
		action = params["action"].GetString();

	auto& lua = LuaEngine::instance();

	if (action == "list")
	{
		StringBuffer sb;
		Writer<StringBuffer> w(sb);
		w.StartObject();
		w.Key("widgets");
		w.StartArray();
		for (const auto& wgt : lua.snapshotWidgets())
		{
			w.StartObject();
			w.Key("key"); w.String(wgt.key.c_str());
			w.Key("type"); w.String(widgetTypeName(wgt.type));
			w.Key("label"); w.String(wgt.label.c_str());
			w.Key("group"); w.String(wgt.source.c_str());
			switch (wgt.type)
			{
				case LuaEngine::LuaWidget::Type::Text:
					w.Key("value"); w.String(wgt.stringVal.c_str());
					if (!wgt.browse.empty()) { w.Key("browse"); w.String(wgt.browse.c_str()); }
					break;
				case LuaEngine::LuaWidget::Type::Checkbox:
					if (wgt.options.empty())
					{
						w.Key("value"); w.Bool(wgt.boolVal);
					}
					else
					{
						w.Key("value"); w.String(wgt.stringVal.c_str());
						w.Key("options");
						w.StartArray();
						for (const auto& o : wgt.options)
							w.String(o.c_str());
						w.EndArray();
						if (wgt.dropdown) { w.Key("dropdown"); w.Bool(true); }
						if (wgt.list) { w.Key("list"); w.Bool(true); }
					}
					break;
				case LuaEngine::LuaWidget::Type::Slider:
					w.Key("value"); w.Double(wgt.numberVal);
					w.Key("min"); w.Double(wgt.sliderMin);
					w.Key("max"); w.Double(wgt.sliderMax);
					w.Key("step"); w.Double(wgt.sliderStep);
					break;
				case LuaEngine::LuaWidget::Type::Button:
					break;
			}
			w.EndObject();
		}
		w.EndArray();
		w.EndObject();
		return jsonOk(sb.GetString());
	}
	else if (action == "get")
	{
		if (!params.HasMember("key") || !params["key"].IsString())
			return jsonError("Missing 'key' parameter");
		std::string key = params["key"].GetString();

		auto widgets = lua.snapshotWidgets();
		auto it = std::find_if(widgets.begin(), widgets.end(),
			[&key](const LuaEngine::LuaWidget& w) { return w.key == key; });
		if (it == widgets.end())
			return jsonError("Unknown widget: " + key);

		StringBuffer sb;
		Writer<StringBuffer> w(sb);
		w.StartObject();
		w.Key("key"); w.String(it->key.c_str());
		switch (it->type)
		{
			case LuaEngine::LuaWidget::Type::Text:
				w.Key("value"); w.String(it->stringVal.c_str());
				break;
			case LuaEngine::LuaWidget::Type::Checkbox:
				if (it->options.empty())
				{ w.Key("value"); w.Bool(it->boolVal); }
				else
				{ w.Key("value"); w.String(it->stringVal.c_str()); }
				break;
			case LuaEngine::LuaWidget::Type::Slider:
				w.Key("value"); w.Double(it->numberVal);
				break;
			case LuaEngine::LuaWidget::Type::Button:
				break;
		}
		w.EndObject();
		return jsonOk(sb.GetString());
	}
	else if (action == "set")
	{
		if (!params.HasMember("key") || !params["key"].IsString())
			return jsonError("Missing 'key' parameter");
		std::string key = params["key"].GetString();

		if (params.HasMember("value"))
		{
			const auto& val = params["value"];
			if (val.IsString())
				lua.setWidgetStringValue(key, val.GetString());
			else if (val.IsBool())
				lua.setWidgetBoolValue(key, val.GetBool());
			else if (val.IsNumber())
				lua.setWidgetNumberValue(key, val.GetDouble());
			else
				return jsonError("Invalid value type");
		}
		return jsonOk();
	}
	else if (action == "click")
	{
		if (!params.HasMember("key") || !params["key"].IsString())
			return jsonError("Missing 'key' parameter");

		// Drain stale logs
		lua.drainLogs();

		lua.widgetButtonClicked(params["key"].GetString());

		auto logs = lua.drainLogs();
		StringBuffer sb;
		Writer<StringBuffer> w(sb);
		w.StartObject();
		w.Key("ok"); w.Bool(true);
		std::string logsJson = luaLogsToJson(logs);
		w.Key("logs"); w.RawValue(logsJson.c_str(), logsJson.size(), kArrayType);
		w.EndObject();
		return jsonOk(sb.GetString());
	}

	return jsonError("Unknown action: " + action);
}

std::string cmdLuaDocs(const Value& /*params*/)
{
	static const char* docs = R"doc(# PCSX2 Lua Scripting API

## mem — PS2 memory access
- `mem.read8(addr) -> int` — Read unsigned byte
- `mem.read16(addr) -> int` — Read unsigned 16-bit
- `mem.read32(addr) -> int` — Read unsigned 32-bit
- `mem.read64(addr) -> int` — Read unsigned 64-bit
- `mem.write8(addr, val)` — Write byte
- `mem.write16(addr, val)` — Write 16-bit
- `mem.write32(addr, val)` — Write 32-bit
- `mem.write64(addr, val)` — Write 64-bit
- `mem.read_float(addr) -> float` — Read IEEE 754 float
- `mem.write_float(addr, val)` — Write float
- `mem.read_string(addr [, maxLen=256]) -> string` — Read null-terminated string
- `mem.write_string(addr, str)` — Write null-terminated string to PS2 RAM
- `mem.read_bytes(addr, len) -> table` — Read byte array (1-indexed table)
- `mem.write_bytes(addr, data [, offset [, length]])` — Write string/bytes to PS2 RAM
- `mem.breakpoint(start, size [, mode="rw"])` — Add memory watchpoint. mode: "r", "w", or "rw"
- `mem.remove_breakpoint(start, size)` — Remove memory watchpoint
- `mem.trace_start(addr, {name=, reads=true, writes=false, dma=false}) -> hook_id` — Start memory access trace on a tracked zone
- `mem.trace_stop(hook_id)` — Stop and remove trace
- `mem.trace_read(hook_id) -> table` — Read trace stats without clearing
- `mem.trace_flush(hook_id)` — Clear accumulated trace stats for one trace
- `mem.traces_flush()` — Flush all active traces
- `mem.zone_find(addr) -> table|nil` — Find tracked zone at exact address. Returns {address, size, parent, name, tags}.
- `mem.zone_find_containing(addr) -> table|nil` — Find zone whose range contains addr.
- `mem.zone_tag(addr, key, value)` — Add tag to zone
- `mem.zone_untag(addr, key)` — Remove tag from zone
- `mem.zones_count() -> int` — Total tracked zones
- `mem.zones_snapshot() -> table` — Snapshot all tracked zones
- `mem.zones_query({parent=, contains=, tag={k=v}}) -> table` — Query zones by filter
- `mem.zone_track(addr, size [, parent [, name [, stack]]])` — Register a tracked zone
- `mem.zone_free(addr)` — Unregister a tracked zone
- `mem.zones_clear()` — Clear all tracked zones

## reg — CPU registers
- `reg.a0`, `reg.v0`, `reg.sp`, etc. — Read/write GPRs by name (metatable)
- `reg.pc` — Read program counter (read-only via metatable)
- `reg.cycle` — Read cycle counter
- `reg.get(idx) -> int` — Read GPR by index (0-31)
- `reg.set(idx, val)` — Write GPR by index

## cpu — Execution hooks and breakpoints
- `cpu.hook(addr, fn)` — Install execution hook at PS2 address (persists until reset or stop). `fn()` called each time PC hits addr.
- `cpu.hook_once(addr, fn)` — One-shot hook, auto-removed after first hit.
- `cpu.unhook(addr)` — Remove all hooks at address.
- `cpu.breakpoint(addr)` — Add execution breakpoint (pauses emulator)
- `cpu.remove_breakpoint(addr)` — Remove execution breakpoint
- `cpu.capture_stack() -> table{frames, depth}` — Capture current MIPS stack trace. Call inside a hook.

## emu — Emulator control
- `emu.pause([reason])` — Pause emulation
- `emu.resume()` — Resume emulation
- `emu.reset()` — Reset the PS2 VM
- `emu.is_paused() -> bool`
- `emu.is_running() -> bool`
- `emu.game_serial() -> string` — Disc serial (e.g. "SCUS-97399")
- `emu.game_crc() -> string` — Disc CRC as hex (e.g. "0x12345678")
- `emu.set_speed(mode)` — Set speed: "normal", "turbo", "slow", "unlimited"
- `emu.skip_block()` — Skip the currently hooked function (set pc=ra and skip block)
- `emu.assert(msg [, stack])` — Pause VM immediately with message. pause_reason will be "lua_assert".
- `emu.vu1_profile()` — Print VU1 per-PC hit counts (sorted by address) to the log, then reset counters.
- `emu.vu1_trace_start([path], [opts])` — Start recording VU1 execution traces to file. path defaults to EmuFolders::Logs/vu1_trace.txt. opts is a table with optional boolean fields: regs, memory, pc_hits (VIF command log is always included). Each MSCAL records VIF commands, registers, memory, and PC hit counts per the flags.
- `emu.vu1_trace_stop()` — Stop recording VU1 traces and close the output file.

## log — Logging
- `log.info(system, msg)` — Info log (appears in console + Lua window)
- `log.warn(system, msg)` — Warning log
- `log.error(system, msg)` — Error log
- `print(...)` — Alias for log.info with system="Lua", concatenates args with tabs

## ui — Widget registration
- `ui.set_group(name [, description])` — Set the group for subsequently registered widgets. Call with `""` to reset.
- `ui.widget_text(key, {label=, default=, browse=, filter=}) -> handle` — Text input. browse: nil, "dir", "file", "save". filter: file filter string.
- `ui.widget_checkbox(key, {label=, default=, options=, dropdown=, list=}) -> handle` — Checkbox (bool), radio buttons (options), dropdown (options+dropdown=true), or list (options+list=true).
- `ui.widget_slider(key, {label=, default=, min=, max=, step=}) -> handle` — Numeric slider. min/max required. step defaults to 1.
- `ui.widget_button(key, {label=, callback=}) -> handle` — Button. callback required.
- Handle methods: `:get()`, `:set(val)`, `:set_default(val)`, `:set_options(list)`, `:on_change(fn)`, `:on_submit(fn)`
- on_change: Text=keystroke+focus-lost, Checkbox=toggle/selection, Slider=release, List=selection change.
- on_submit: Text=Enter, Slider=release, List=double-click. Not for bool checkbox/radio/dropdown/button.

## config — Persistent key-value storage
- `config.get(key [, default]) -> string|nil` — Read from base settings "LuaMods" section.
- `config.set(key, value)` — Write to base settings. nil value removes key.
- `config.game_get(key [, default]) -> string|nil` — Read from per-game settings.
- `config.game_set(key, value)` — Write to per-game settings.

## File I/O
Standard Lua `io` library is available (`io.open`, `io.read`, `io.write`, etc.) for reading/writing files on the host filesystem.

## Autoload
Scripts placed in `~/.config/PCSX2/lua-autoload/` (or equivalent `EmuFolders::DataRoot + "/lua-autoload/"`) are automatically loaded in alphabetical order when the game boots (including after `emu.reset()`). Use filename prefixes like `01_`, `02_` to control load order. This is the primary way to deploy hooks and scripts — edit files, reset, and they take effect immediately.

## Multi-Script Composition

All scripts (autoload and `run`) share a **single Lua state** — there is no isolation between them. They run sequentially, not in parallel. To share data between scripts, use **global** variables (not `local`). A `local` variable is only visible within the file that defines it; a global (assigned without `local`) is accessible to all scripts loaded afterward.

### Loading modes
- `lua exec` — execute code into the current Lua state. Hooks installed this way are **temporary** — they are destroyed on reset.
- `lua run` — **additive**: executes script file into the existing Lua state. Previously defined globals, hooks, and widgets are preserved within the session. However, run files are **NOT re-executed on reset** — their hooks are lost.
- `emu_control reset` — destroys the **entire** Lua state (all hooks, globals, widgets from exec/run), then re-runs only autoload scripts from scratch. This is the primary way to reload scripts after editing.

### Using autoload scripts as shared libraries

**Global module pattern** — define global tables in autoload scripts:
```lua
-- ~/.config/PCSX2/lua-autoload/01_utils.lua
utils = {}
function utils.read_vec4(addr)
    return { x=mem.read_float(addr), y=mem.read_float(addr+4),
             z=mem.read_float(addr+8), w=mem.read_float(addr+12) }
end
```
Scripts loaded later (via `run` or autoload) can call `utils.read_vec4(addr)` directly.

**require() pattern** — return a module table:
```lua
-- ~/.config/PCSX2/lua-autoload/helpers/matrix.lua
local M = {}
function M.identity() return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1} end
return M
```
```lua
local matrix = require("helpers.matrix")  -- resolves in autoload dir
```

### package.path resolution
`require()` searches these locations in order:
1. The directory of the script being loaded/run (added automatically)
2. The autoload directory (`lua-autoload/`)
3. Default Lua paths

### Recommended workflow (autoload + reset)
1. Write/edit `.lua` scripts in `~/.config/PCSX2/lua-autoload/` with `01_`, `02_` prefixes for load order
2. Reset the game (`emu.reset()` from Lua, or `emu_control reset` via MCP) — autoload scripts re-run automatically on boot
3. Scripts install hooks and define globals during autoload; hooks fire as the game runs
4. Scripts communicate via **global** variables — e.g. one script writes `detected_events = {}`, another reads it
5. Multiple scripts can hook the **same PS2 address** — all callbacks fire in alphabetical filename order
6. Read results via `lua exec` (e.g. `return my_global`) or drain logs via `lua logs`

### Ad-hoc workflow (temporary — lost on reset)
- `lua run` additively loads a script into the existing state (preserves autoloaded scripts)
- `lua status` shows loaded scripts, run files, and any script errors
- `lua stop` cleans up all hooks/traces/breakpoints/widgets

## Notes
- All addresses are PS2 virtual addresses (u32), typically passed as Lua numbers: `0x0029e560`
- Hooks execute on the EE thread at the hooked PC before the instruction runs
- **Delay slot limitation**: Hooks cannot fire on branch delay slot addresses (the instruction immediately after a branch/jump). If you hook a delay slot address, a warning will be logged. To hook around a branch, hook the branch instruction itself (addr - 4) instead.
- Inside hooks, `reg.*` gives the current register state; `mem.*` reads live PS2 RAM
- All Lua API calls are serialized by a recursive mutex — safe from any thread
- `require()` caches modules in `package.loaded` — repeated calls return the same table
- To reload scripts after editing, reset the game (`emu.reset()` or `emu_control reset`) — this re-triggers autoload
)doc";

	StringBuffer sb;
	Writer<StringBuffer> w(sb);
	w.StartObject();
	w.Key("docs"); w.String(docs);
	w.EndObject();
	return jsonOk(sb.GetString());
}
