// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// Originally written by Sven Peter <sven@fail0verflow.com> for anergistic.

#include <algorithm>
#include <atomic>
#include <climits>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <numeric>
#include <fcntl.h>

#ifdef _WIN32
#include <winsock2.h>
// winsock2.h needs to be included first to prevent winsock.h being included by other includes
#include <io.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#define SHUT_RDWR 2
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "common/logging/log.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/core_cpu.h"
#include "core/gdbstub/gdbstub.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/scheduler.h"
#include "core/loader/loader.h"
#include "core/memory.h"

const int GDB_BUFFER_SIZE = 10000;

const char GDB_STUB_START = '$';
const char GDB_STUB_END = '#';
const char GDB_STUB_ACK = '+';
const char GDB_STUB_NACK = '-';

#ifndef SIGTRAP
const u32 SIGTRAP = 5;
#endif

#ifndef SIGTERM
const u32 SIGTERM = 15;
#endif

#ifndef MSG_WAITALL
const u32 MSG_WAITALL = 8;
#endif

const u32 LR_REGISTER = 30;
const u32 SP_REGISTER = 31;
const u32 PC_REGISTER = 32;
const u32 CPSR_REGISTER = 33;
const u32 UC_ARM64_REG_Q0 = 34;
const u32 FPSCR_REGISTER = 66;

// TODO/WiP - Used while working on support for FPU
const u32 TODO_DUMMY_REG_997 = 997;
const u32 TODO_DUMMY_REG_998 = 998;

// For sample XML files see the GDB source /gdb/features
// GDB also wants the l character at the start
// This XML defines what the registers are for this specific ARM device
static const char* target_xml =
    R"(l<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target version="1.0">
  <feature name="org.gnu.gdb.aarch64.core">
    <reg name="x0" bitsize="64"/>
    <reg name="x1" bitsize="64"/>
    <reg name="x2" bitsize="64"/>
    <reg name="x3" bitsize="64"/>
    <reg name="x4" bitsize="64"/>
    <reg name="x5" bitsize="64"/>
    <reg name="x6" bitsize="64"/>
    <reg name="x7" bitsize="64"/>
    <reg name="x8" bitsize="64"/>
    <reg name="x9" bitsize="64"/>
    <reg name="x10" bitsize="64"/>
    <reg name="x11" bitsize="64"/>
    <reg name="x12" bitsize="64"/>
    <reg name="x13" bitsize="64"/>
    <reg name="x14" bitsize="64"/>
    <reg name="x15" bitsize="64"/>
    <reg name="x16" bitsize="64"/>
    <reg name="x17" bitsize="64"/>
    <reg name="x18" bitsize="64"/>
    <reg name="x19" bitsize="64"/>
    <reg name="x20" bitsize="64"/>
    <reg name="x21" bitsize="64"/>
    <reg name="x22" bitsize="64"/>
    <reg name="x23" bitsize="64"/>
    <reg name="x24" bitsize="64"/>
    <reg name="x25" bitsize="64"/>
    <reg name="x26" bitsize="64"/>
    <reg name="x27" bitsize="64"/>
    <reg name="x28" bitsize="64"/>
    <reg name="x29" bitsize="64"/>
    <reg name="x30" bitsize="64"/>
    <reg name="sp" bitsize="64" type="data_ptr"/>

    <reg name="pc" bitsize="64" type="code_ptr"/>

    <flags id="cpsr_flags" size="4">
      <field name="SP" start="0" end="0"/>
      <field name="" start="1" end="1"/>
      <field name="EL" start="2" end="3"/>
      <field name="nRW" start="4" end="4"/>
      <field name="" start="5" end="5"/>
      <field name="F" start="6" end="6"/>
      <field name="I" start="7" end="7"/>
      <field name="A" start="8" end="8"/>
      <field name="D" start="9" end="9"/>

      <field name="IL" start="20" end="20"/>
      <field name="SS" start="21" end="21"/>

      <field name="V" start="28" end="28"/>
      <field name="C" start="29" end="29"/>
      <field name="Z" start="30" end="30"/>
      <field name="N" start="31" end="31"/>
    </flags>
    <reg name="cpsr" bitsize="32" type="cpsr_flags"/>
  </feature>
  <feature name="org.gnu.gdb.aarch64.fpu">
  </feature>
</target>
)";

namespace GDBStub {

static int gdbserver_socket = -1;

static u8 command_buffer[GDB_BUFFER_SIZE];
static u32 command_length;

static u32 latest_signal = 0;
static bool memory_break = false;

static Kernel::Thread* current_thread = nullptr;
static u32 current_core = 0;

// Binding to a port within the reserved ports range (0-1023) requires root permissions,
// so default to a port outside of that range.
static u16 gdbstub_port = 24689;

static bool halt_loop = true;
static bool step_loop = false;
static bool send_trap = false;

// If set to false, the server will never be started and no
// gdbstub-related functions will be executed.
static std::atomic<bool> server_enabled(false);

#ifdef _WIN32
WSADATA InitData;
#endif

struct Breakpoint {
    bool active;
    PAddr addr;
    u64 len;
};

static std::map<u64, Breakpoint> breakpoints_execute;
static std::map<u64, Breakpoint> breakpoints_read;
static std::map<u64, Breakpoint> breakpoints_write;

struct Module {
    std::string name;
    PAddr beg;
    PAddr end;
};

static std::vector<Module> modules;

void RegisterModule(std::string name, PAddr beg, PAddr end, bool add_elf_ext) {
    Module module;
    if (add_elf_ext) {
        Common::SplitPath(name, nullptr, &module.name, nullptr);
        module.name += ".elf";
    } else {
        module.name = std::move(name);
    }
    module.beg = beg;
    module.end = end;
    modules.push_back(std::move(module));
}

static Kernel::Thread* FindThreadById(int id) {
    for (u32 core = 0; core < Core::NUM_CPU_CORES; core++) {
        const auto& threads = Core::System::GetInstance().Scheduler(core)->GetThreadList();
        for (auto& thread : threads) {
            if (thread->GetThreadId() == id) {
                current_core = core;
                return thread.get();
            }
        }
    }
    return nullptr;
}

static u64 RegRead(int id, Kernel::Thread* thread = nullptr) {
    if (!thread) {
        return 0;
    }

    if (id < SP_REGISTER) {
        return thread->context.cpu_registers[id];
    } else if (id == SP_REGISTER) {
        return thread->context.sp;
    } else if (id == PC_REGISTER) {
        return thread->context.pc;
    } else if (id == CPSR_REGISTER) {
        return thread->context.cpsr;
    } else if (id > CPSR_REGISTER && id < FPSCR_REGISTER) {
        return thread->context.fpu_registers[id - UC_ARM64_REG_Q0][0];
    } else {
        return 0;
    }
}

static void RegWrite(int id, u64 val, Kernel::Thread* thread = nullptr) {
    if (!thread) {
        return;
    }

    if (id < SP_REGISTER) {
        thread->context.cpu_registers[id] = val;
    } else if (id == SP_REGISTER) {
        thread->context.sp = val;
    } else if (id == PC_REGISTER) {
        thread->context.pc = val;
    } else if (id == CPSR_REGISTER) {
        thread->context.cpsr = val;
    } else if (id > CPSR_REGISTER && id < FPSCR_REGISTER) {
        thread->context.fpu_registers[id - (CPSR_REGISTER + 1)][0] = val;
    }
}

/**
 * Turns hex string character into the equivalent byte.
 *
 * @param hex Input hex character to be turned into byte.
 */
static u8 HexCharToValue(u8 hex) {
    if (hex >= '0' && hex <= '9') {
        return hex - '0';
    } else if (hex >= 'a' && hex <= 'f') {
        return hex - 'a' + 0xA;
    } else if (hex >= 'A' && hex <= 'F') {
        return hex - 'A' + 0xA;
    }

    LOG_ERROR(Debug_GDBStub, "Invalid nibble: {} ({:02X})", hex, hex);
    return 0;
}

/**
 * Turn nibble of byte into hex string character.
 *
 * @param n Nibble to be turned into hex character.
 */
static u8 NibbleToHex(u8 n) {
    n &= 0xF;
    if (n < 0xA) {
        return '0' + n;
    } else {
        return 'a' + n - 0xA;
    }
}

/**
 * Converts input hex string characters into an array of equivalent of u8 bytes.
 *
 * @param src Pointer to array of output hex string characters.
 * @param len Length of src array.
 */
static u32 HexToInt(const u8* src, size_t len) {
    u32 output = 0;
    while (len-- > 0) {
        output = (output << 4) | HexCharToValue(src[0]);
        src++;
    }
    return output;
}

/**
 * Converts input hex string characters into an array of equivalent of u8 bytes.
 *
 * @param src Pointer to array of output hex string characters.
 * @param len Length of src array.
 */
static u64 HexToLong(const u8* src, size_t len) {
    u64 output = 0;
    while (len-- > 0) {
        output = (output << 4) | HexCharToValue(src[0]);
        src++;
    }
    return output;
}

/**
 * Converts input array of u8 bytes into their equivalent hex string characters.
 *
 * @param dest Pointer to buffer to store output hex string characters.
 * @param src Pointer to array of u8 bytes.
 * @param len Length of src array.
 */
static void MemToGdbHex(u8* dest, const u8* src, size_t len) {
    while (len-- > 0) {
        u8 tmp = *src++;
        *dest++ = NibbleToHex(tmp >> 4);
        *dest++ = NibbleToHex(tmp);
    }
}

/**
 * Converts input gdb-formatted hex string characters into an array of equivalent of u8 bytes.
 *
 * @param dest Pointer to buffer to store u8 bytes.
 * @param src Pointer to array of output hex string characters.
 * @param len Length of src array.
 */
static void GdbHexToMem(u8* dest, const u8* src, size_t len) {
    while (len-- > 0) {
        *dest++ = (HexCharToValue(src[0]) << 4) | HexCharToValue(src[1]);
        src += 2;
    }
}

/**
 * Convert a u32 into a gdb-formatted hex string.
 *
 * @param dest Pointer to buffer to store output hex string characters.
 * @param v    Value to convert.
 */
static void IntToGdbHex(u8* dest, u32 v) {
    for (int i = 0; i < 8; i += 2) {
        dest[i + 1] = NibbleToHex(static_cast<u8>(v >> (4 * i)));
        dest[i] = NibbleToHex(static_cast<u8>(v >> (4 * (i + 1))));
    }
}

/**
 * Convert a u64 into a gdb-formatted hex string.
 *
 * @param dest Pointer to buffer to store output hex string characters.
 * @param v    Value to convert.
 */
static void LongToGdbHex(u8* dest, u64 v) {
    for (int i = 0; i < 16; i += 2) {
        dest[i + 1] = NibbleToHex(static_cast<u8>(v >> (4 * i)));
        dest[i] = NibbleToHex(static_cast<u8>(v >> (4 * (i + 1))));
    }
}

/**
 * Convert a gdb-formatted hex string into a u32.
 *
 * @param src Pointer to hex string.
 */
static u32 GdbHexToInt(const u8* src) {
    u32 output = 0;

    for (int i = 0; i < 8; i += 2) {
        output = (output << 4) | HexCharToValue(src[7 - i - 1]);
        output = (output << 4) | HexCharToValue(src[7 - i]);
    }

    return output;
}

/**
 * Convert a gdb-formatted hex string into a u64.
 *
 * @param src Pointer to hex string.
 */
static u64 GdbHexToLong(const u8* src) {
    u64 output = 0;

    for (int i = 0; i < 16; i += 2) {
        output = (output << 4) | HexCharToValue(src[15 - i - 1]);
        output = (output << 4) | HexCharToValue(src[15 - i]);
    }

    return output;
}

/// Read a byte from the gdb client.
static u8 ReadByte() {
    u8 c;
    size_t received_size = recv(gdbserver_socket, reinterpret_cast<char*>(&c), 1, MSG_WAITALL);
    if (received_size != 1) {
        LOG_ERROR(Debug_GDBStub, "recv failed: {}", received_size);
        Shutdown();
    }

    return c;
}

/// Calculate the checksum of the current command buffer.
static u8 CalculateChecksum(const u8* buffer, size_t length) {
    return static_cast<u8>(std::accumulate(buffer, buffer + length, 0, std::plus<u8>()));
}

/**
 * Get the list of breakpoints for a given breakpoint type.
 *
 * @param type Type of breakpoint list.
 */
static std::map<u64, Breakpoint>& GetBreakpointList(BreakpointType type) {
    switch (type) {
    case BreakpointType::Execute:
        return breakpoints_execute;
    case BreakpointType::Read:
        return breakpoints_read;
    case BreakpointType::Write:
        return breakpoints_write;
    default:
        return breakpoints_read;
    }
}

/**
 * Remove the breakpoint from the given address of the specified type.
 *
 * @param type Type of breakpoint.
 * @param addr Address of breakpoint.
 */
static void RemoveBreakpoint(BreakpointType type, PAddr addr) {
    std::map<u64, Breakpoint>& p = GetBreakpointList(type);

    auto bp = p.find(static_cast<u64>(addr));
    if (bp != p.end()) {
        LOG_DEBUG(Debug_GDBStub, "gdb: removed a breakpoint: {:016X} bytes at {:016X} of type {}",
                  bp->second.len, bp->second.addr, static_cast<int>(type));
        p.erase(static_cast<u64>(addr));
    }
}

BreakpointAddress GetNextBreakpointFromAddress(PAddr addr, BreakpointType type) {
    std::map<u64, Breakpoint>& p = GetBreakpointList(type);
    auto next_breakpoint = p.lower_bound(static_cast<u64>(addr));
    BreakpointAddress breakpoint;

    if (next_breakpoint != p.end()) {
        breakpoint.address = next_breakpoint->first;
        breakpoint.type = type;
    } else {
        breakpoint.address = 0;
        breakpoint.type = BreakpointType::None;
    }

    return breakpoint;
}

bool CheckBreakpoint(PAddr addr, BreakpointType type) {
    if (!IsConnected()) {
        return false;
    }

    std::map<u64, Breakpoint>& p = GetBreakpointList(type);

    auto bp = p.find(static_cast<u64>(addr));
    if (bp != p.end()) {
        u64 len = bp->second.len;

        // IDA Pro defaults to 4-byte breakpoints for all non-hardware breakpoints
        // no matter if it's a 4-byte or 2-byte instruction. When you execute a
        // Thumb instruction with a 4-byte breakpoint set, it will set a breakpoint on
        // two instructions instead of the single instruction you placed the breakpoint
        // on. So, as a way to make sure that execution breakpoints are only breaking
        // on the instruction that was specified, set the length of an execution
        // breakpoint to 1. This should be fine since the CPU should never begin executing
        // an instruction anywhere except the beginning of the instruction.
        if (type == BreakpointType::Execute) {
            len = 1;
        }

        if (bp->second.active && (addr >= bp->second.addr && addr < bp->second.addr + len)) {
            LOG_DEBUG(Debug_GDBStub,
                      "Found breakpoint type {} @ {:016X}, range: {:016X}"
                      " - {:016X} ({:X} bytes)",
                      static_cast<int>(type), addr, bp->second.addr, bp->second.addr + len, len);
            return true;
        }
    }

    return false;
}

/**
 * Send packet to gdb client.
 *
 * @param packet Packet to be sent to client.
 */
static void SendPacket(const char packet) {
    size_t sent_size = send(gdbserver_socket, &packet, 1, 0);
    if (sent_size != 1) {
        LOG_ERROR(Debug_GDBStub, "send failed");
    }
}

/**
 * Send reply to gdb client.
 *
 * @param reply Reply to be sent to client.
 */
static void SendReply(const char* reply) {
    if (!IsConnected()) {
        return;
    }

    LOG_DEBUG(Debug_GDBStub, "Reply: {}", reply);

    memset(command_buffer, 0, sizeof(command_buffer));

    command_length = static_cast<u32>(strlen(reply));
    if (command_length + 4 > sizeof(command_buffer)) {
        LOG_ERROR(Debug_GDBStub, "command_buffer overflow in SendReply");
        return;
    }

    memcpy(command_buffer + 1, reply, command_length);

    u8 checksum = CalculateChecksum(command_buffer, command_length + 1);
    command_buffer[0] = GDB_STUB_START;
    command_buffer[command_length + 1] = GDB_STUB_END;
    command_buffer[command_length + 2] = NibbleToHex(checksum >> 4);
    command_buffer[command_length + 3] = NibbleToHex(checksum);

    u8* ptr = command_buffer;
    u32 left = command_length + 4;
    while (left > 0) {
        int sent_size = send(gdbserver_socket, reinterpret_cast<char*>(ptr), left, 0);
        if (sent_size < 0) {
            LOG_ERROR(Debug_GDBStub, "gdb: send failed");
            return Shutdown();
        }

        left -= sent_size;
        ptr += sent_size;
    }
}

/// Handle query command from gdb client.
static void HandleQuery() {
    LOG_DEBUG(Debug_GDBStub, "gdb: query '{}'", command_buffer + 1);

    const char* query = reinterpret_cast<const char*>(command_buffer + 1);

    if (strcmp(query, "TStatus") == 0) {
        SendReply("T0");
    } else if (strncmp(query, "Supported", strlen("Supported")) == 0) {
        // PacketSize needs to be large enough for target xml
        std::string buffer = "PacketSize=2000;qXfer:features:read+;qXfer:threads:read+";
        if (!modules.empty()) {
            buffer += ";qXfer:libraries:read+";
        }
        SendReply(buffer.c_str());
    } else if (strncmp(query, "Xfer:features:read:target.xml:",
                       strlen("Xfer:features:read:target.xml:")) == 0) {
        SendReply(target_xml);
    } else if (strncmp(query, "Offsets", strlen("Offsets")) == 0) {
        std::string buffer = fmt::format("TextSeg={:0x}", Memory::PROCESS_IMAGE_VADDR);
        SendReply(buffer.c_str());
    } else if (strncmp(query, "fThreadInfo", strlen("fThreadInfo")) == 0) {
        std::string val = "m";
        for (u32 core = 0; core < Core::NUM_CPU_CORES; core++) {
            const auto& threads = Core::System::GetInstance().Scheduler(core)->GetThreadList();
            for (const auto& thread : threads) {
                val += fmt::format("{:x}", thread->GetThreadId());
                val += ",";
            }
        }
        val.pop_back();
        SendReply(val.c_str());
    } else if (strncmp(query, "sThreadInfo", strlen("sThreadInfo")) == 0) {
        SendReply("l");
    } else if (strncmp(query, "Xfer:threads:read", strlen("Xfer:threads:read")) == 0) {
        std::string buffer;
        buffer += "l<?xml version=\"1.0\"?>";
        buffer += "<threads>";
        for (u32 core = 0; core < Core::NUM_CPU_CORES; core++) {
            const auto& threads = Core::System::GetInstance().Scheduler(core)->GetThreadList();
            for (const auto& thread : threads) {
                buffer +=
                    fmt::format(R"*(<thread id="{:x}" core="{:d}" name="Thread {:x}"></thread>)*",
                                thread->GetThreadId(), core, thread->GetThreadId());
            }
        }
        buffer += "</threads>";
        SendReply(buffer.c_str());
    } else if (strncmp(query, "Xfer:libraries:read", strlen("Xfer:libraries:read")) == 0) {
        std::string buffer;
        buffer += "l<?xml version=\"1.0\"?>";
        buffer += "<library-list>";
        for (const auto& module : modules) {
            buffer +=
                fmt::format(R"*("<library name = "{}"><segment address = "0x{:x}"/></library>)*",
                            module.name, module.beg);
        }
        buffer += "</library-list>";
        SendReply(buffer.c_str());
    } else {
        SendReply("");
    }
}

/// Handle set thread command from gdb client.
static void HandleSetThread() {
    int thread_id = -1;
    if (command_buffer[2] != '-') {
        thread_id = static_cast<int>(HexToInt(command_buffer + 2, command_length - 2));
    }
    if (thread_id >= 1) {
        current_thread = FindThreadById(thread_id);
    }
    if (!current_thread) {
        thread_id = 1;
        current_thread = FindThreadById(thread_id);
    }
    if (current_thread) {
        SendReply("OK");
        return;
    }
    SendReply("E01");
}

/// Handle thread alive command from gdb client.
static void HandleThreadAlive() {
    int thread_id = static_cast<int>(HexToInt(command_buffer + 1, command_length - 1));
    if (thread_id == 0) {
        thread_id = 1;
    }
    if (FindThreadById(thread_id)) {
        SendReply("OK");
        return;
    }
    SendReply("E01");
}

/**
 * Send signal packet to client.
 *
 * @param signal Signal to be sent to client.
 */
static void SendSignal(Kernel::Thread* thread, u32 signal, bool full = true) {
    if (gdbserver_socket == -1) {
        return;
    }

    latest_signal = signal;

    if (!thread) {
        full = false;
    }

    std::string buffer;
    if (full) {
        buffer = fmt::format("T{:02x}{:02x}:{:016x};{:02x}:{:016x};{:02x}:{:016x}", latest_signal,
                             PC_REGISTER, Common::swap64(RegRead(PC_REGISTER, thread)), SP_REGISTER,
                             Common::swap64(RegRead(SP_REGISTER, thread)), LR_REGISTER,
                             Common::swap64(RegRead(LR_REGISTER, thread)));
    } else {
        buffer = fmt::format("T{:02x}", latest_signal);
    }

    if (thread) {
        buffer += fmt::format(";thread:{:x};", thread->GetThreadId());
    }

    SendReply(buffer.c_str());
}

/// Read command from gdb client.
static void ReadCommand() {
    command_length = 0;
    memset(command_buffer, 0, sizeof(command_buffer));

    u8 c = ReadByte();
    if (c == '+') {
        // ignore ack
        return;
    } else if (c == 0x03) {
        LOG_INFO(Debug_GDBStub, "gdb: found break command");
        halt_loop = true;
        SendSignal(current_thread, SIGTRAP);
        return;
    } else if (c != GDB_STUB_START) {
        LOG_DEBUG(Debug_GDBStub, "gdb: read invalid byte {:02X}", c);
        return;
    }

    while ((c = ReadByte()) != GDB_STUB_END) {
        if (command_length >= sizeof(command_buffer)) {
            LOG_ERROR(Debug_GDBStub, "gdb: command_buffer overflow");
            SendPacket(GDB_STUB_NACK);
            return;
        }
        command_buffer[command_length++] = c;
    }

    u8 checksum_received = HexCharToValue(ReadByte()) << 4;
    checksum_received |= HexCharToValue(ReadByte());

    u8 checksum_calculated = CalculateChecksum(command_buffer, command_length);

    if (checksum_received != checksum_calculated) {
        LOG_ERROR(Debug_GDBStub,
                  "gdb: invalid checksum: calculated {:02X} and read {:02X} for ${}# (length: {})",
                  checksum_calculated, checksum_received, command_buffer, command_length);

        command_length = 0;

        SendPacket(GDB_STUB_NACK);
        return;
    }

    SendPacket(GDB_STUB_ACK);
}

/// Check if there is data to be read from the gdb client.
static bool IsDataAvailable() {
    if (!IsConnected()) {
        return false;
    }

    fd_set fd_socket;

    FD_ZERO(&fd_socket);
    FD_SET(gdbserver_socket, &fd_socket);

    struct timeval t;
    t.tv_sec = 0;
    t.tv_usec = 0;

    if (select(gdbserver_socket + 1, &fd_socket, nullptr, nullptr, &t) < 0) {
        LOG_ERROR(Debug_GDBStub, "select failed");
        return false;
    }

    return FD_ISSET(gdbserver_socket, &fd_socket) != 0;
}

/// Send requested register to gdb client.
static void ReadRegister() {
    static u8 reply[64];
    memset(reply, 0, sizeof(reply));

    u32 id = HexCharToValue(command_buffer[1]);
    if (command_buffer[2] != '\0') {
        id <<= 4;
        id |= HexCharToValue(command_buffer[2]);
    }

    if (id <= SP_REGISTER) {
        LongToGdbHex(reply, RegRead(id, current_thread));
    } else if (id == PC_REGISTER) {
        LongToGdbHex(reply, RegRead(id, current_thread));
    } else if (id == CPSR_REGISTER) {
        IntToGdbHex(reply, (u32)RegRead(id, current_thread));
    } else if (id >= UC_ARM64_REG_Q0 && id < FPSCR_REGISTER) {
        LongToGdbHex(reply, RegRead(id, current_thread));
    } else if (id == FPSCR_REGISTER) {
        LongToGdbHex(reply, RegRead(TODO_DUMMY_REG_998, current_thread));
    } else {
        LongToGdbHex(reply, RegRead(TODO_DUMMY_REG_997, current_thread));
    }

    SendReply(reinterpret_cast<char*>(reply));
}

/// Send all registers to the gdb client.
static void ReadRegisters() {
    static u8 buffer[GDB_BUFFER_SIZE - 4];
    memset(buffer, 0, sizeof(buffer));

    u8* bufptr = buffer;

    for (int reg = 0; reg <= SP_REGISTER; reg++) {
        LongToGdbHex(bufptr + reg * 16, RegRead(reg, current_thread));
    }

    bufptr += 32 * 16;

    LongToGdbHex(bufptr, RegRead(PC_REGISTER, current_thread));

    bufptr += 16;

    IntToGdbHex(bufptr, (u32)RegRead(CPSR_REGISTER, current_thread));

    bufptr += 8;

    for (int reg = UC_ARM64_REG_Q0; reg <= UC_ARM64_REG_Q0 + 31; reg++) {
        LongToGdbHex(bufptr + reg * 16, RegRead(reg, current_thread));
    }

    bufptr += 32 * 32;

    LongToGdbHex(bufptr, RegRead(TODO_DUMMY_REG_998, current_thread));

    bufptr += 8;

    SendReply(reinterpret_cast<char*>(buffer));
}

/// Modify data of register specified by gdb client.
static void WriteRegister() {
    const u8* buffer_ptr = command_buffer + 3;

    u32 id = HexCharToValue(command_buffer[1]);
    if (command_buffer[2] != '=') {
        ++buffer_ptr;
        id <<= 4;
        id |= HexCharToValue(command_buffer[2]);
    }

    if (id <= SP_REGISTER) {
        RegWrite(id, GdbHexToLong(buffer_ptr), current_thread);
    } else if (id == PC_REGISTER) {
        RegWrite(id, GdbHexToLong(buffer_ptr), current_thread);
    } else if (id == CPSR_REGISTER) {
        RegWrite(id, GdbHexToInt(buffer_ptr), current_thread);
    } else if (id >= UC_ARM64_REG_Q0 && id < FPSCR_REGISTER) {
        RegWrite(id, GdbHexToLong(buffer_ptr), current_thread);
    } else if (id == FPSCR_REGISTER) {
        RegWrite(TODO_DUMMY_REG_998, GdbHexToLong(buffer_ptr), current_thread);
    } else {
        RegWrite(TODO_DUMMY_REG_997, GdbHexToLong(buffer_ptr), current_thread);
    }

    // Update Unicorn context skipping scheduler, no running threads at this point
    Core::System::GetInstance().ArmInterface(current_core).LoadContext(current_thread->context);

    SendReply("OK");
}

/// Modify all registers with data received from the client.
static void WriteRegisters() {
    const u8* buffer_ptr = command_buffer + 1;

    if (command_buffer[0] != 'G')
        return SendReply("E01");

    for (int i = 0, reg = 0; reg <= FPSCR_REGISTER; i++, reg++) {
        if (reg <= SP_REGISTER) {
            RegWrite(reg, GdbHexToLong(buffer_ptr + i * 16), current_thread);
        } else if (reg == PC_REGISTER) {
            RegWrite(PC_REGISTER, GdbHexToLong(buffer_ptr + i * 16), current_thread);
        } else if (reg == CPSR_REGISTER) {
            RegWrite(CPSR_REGISTER, GdbHexToInt(buffer_ptr + i * 16), current_thread);
        } else if (reg >= UC_ARM64_REG_Q0 && reg < FPSCR_REGISTER) {
            RegWrite(reg, GdbHexToLong(buffer_ptr + i * 16), current_thread);
        } else if (reg == FPSCR_REGISTER) {
            RegWrite(TODO_DUMMY_REG_998, GdbHexToLong(buffer_ptr + i * 16), current_thread);
        } else {
            UNIMPLEMENTED();
        }
    }

    // Update Unicorn context skipping scheduler, no running threads at this point
    Core::System::GetInstance().ArmInterface(current_core).LoadContext(current_thread->context);

    SendReply("OK");
}

/// Read location in memory specified by gdb client.
static void ReadMemory() {
    static u8 reply[GDB_BUFFER_SIZE - 4];

    auto start_offset = command_buffer + 1;
    auto addr_pos = std::find(start_offset, command_buffer + command_length, ',');
    VAddr addr = HexToLong(start_offset, static_cast<u64>(addr_pos - start_offset));

    start_offset = addr_pos + 1;
    u64 len =
        HexToLong(start_offset, static_cast<u64>((command_buffer + command_length) - start_offset));

    LOG_DEBUG(Debug_GDBStub, "gdb: addr: {:016X} len: {:016X}", addr, len);

    if (len * 2 > sizeof(reply)) {
        SendReply("E01");
    }

    if (addr < Memory::PROCESS_IMAGE_VADDR || addr >= Memory::MAP_REGION_VADDR_END) {
        return SendReply("E00");
    }

    if (!Memory::IsValidVirtualAddress(addr)) {
        return SendReply("E00");
    }

    std::vector<u8> data(len);
    Memory::ReadBlock(addr, data.data(), len);

    MemToGdbHex(reply, data.data(), len);
    reply[len * 2] = '\0';
    SendReply(reinterpret_cast<char*>(reply));
}

/// Modify location in memory with data received from the gdb client.
static void WriteMemory() {
    auto start_offset = command_buffer + 1;
    auto addr_pos = std::find(start_offset, command_buffer + command_length, ',');
    VAddr addr = HexToLong(start_offset, static_cast<u64>(addr_pos - start_offset));

    start_offset = addr_pos + 1;
    auto len_pos = std::find(start_offset, command_buffer + command_length, ':');
    u64 len = HexToLong(start_offset, static_cast<u64>(len_pos - start_offset));

    if (!Memory::IsValidVirtualAddress(addr)) {
        return SendReply("E00");
    }

    std::vector<u8> data(len);

    GdbHexToMem(data.data(), len_pos + 1, len);
    Memory::WriteBlock(addr, data.data(), len);
    SendReply("OK");
}

void Break(bool is_memory_break) {
    send_trap = true;

    memory_break = is_memory_break;
}

/// Tell the CPU that it should perform a single step.
static void Step() {
    if (command_length > 1) {
        RegWrite(PC_REGISTER, GdbHexToLong(command_buffer + 1), current_thread);
        // Update Unicorn context skipping scheduler, no running threads at this point
        Core::System::GetInstance().ArmInterface(current_core).LoadContext(current_thread->context);
    }
    step_loop = true;
    halt_loop = true;
    send_trap = true;
}

/// Tell the CPU if we hit a memory breakpoint.
bool IsMemoryBreak() {
    if (IsConnected()) {
        return false;
    }

    return memory_break;
}

/// Tell the CPU to continue executing.
static void Continue() {
    memory_break = false;
    step_loop = false;
    halt_loop = false;
}

/**
 * Commit breakpoint to list of breakpoints.
 *
 * @param type Type of breakpoint.
 * @param addr Address of breakpoint.
 * @param len Length of breakpoint.
 */
static bool CommitBreakpoint(BreakpointType type, PAddr addr, u64 len) {
    std::map<u64, Breakpoint>& p = GetBreakpointList(type);

    Breakpoint breakpoint;
    breakpoint.active = true;
    breakpoint.addr = addr;
    breakpoint.len = len;
    p.insert({addr, breakpoint});

    LOG_DEBUG(Debug_GDBStub, "gdb: added {} breakpoint: {:016X} bytes at {:016X}",
              static_cast<int>(type), breakpoint.len, breakpoint.addr);

    return true;
}

/// Handle add breakpoint command from gdb client.
static void AddBreakpoint() {
    BreakpointType type;

    u8 type_id = HexCharToValue(command_buffer[1]);
    switch (type_id) {
    case 0:
    case 1:
        type = BreakpointType::Execute;
        break;
    case 2:
        type = BreakpointType::Write;
        break;
    case 3:
        type = BreakpointType::Read;
        break;
    case 4:
        type = BreakpointType::Access;
        break;
    default:
        return SendReply("E01");
    }

    auto start_offset = command_buffer + 3;
    auto addr_pos = std::find(start_offset, command_buffer + command_length, ',');
    PAddr addr = HexToLong(start_offset, static_cast<u64>(addr_pos - start_offset));

    start_offset = addr_pos + 1;
    u64 len =
        HexToLong(start_offset, static_cast<u64>((command_buffer + command_length) - start_offset));

    if (type == BreakpointType::Access) {
        // Access is made up of Read and Write types, so add both breakpoints
        type = BreakpointType::Read;

        if (!CommitBreakpoint(type, addr, len)) {
            return SendReply("E02");
        }

        type = BreakpointType::Write;
    }

    if (!CommitBreakpoint(type, addr, len)) {
        return SendReply("E02");
    }

    SendReply("OK");
}

/// Handle remove breakpoint command from gdb client.
static void RemoveBreakpoint() {
    BreakpointType type;

    u8 type_id = HexCharToValue(command_buffer[1]);
    switch (type_id) {
    case 0:
    case 1:
        type = BreakpointType::Execute;
        break;
    case 2:
        type = BreakpointType::Write;
        break;
    case 3:
        type = BreakpointType::Read;
        break;
    case 4:
        type = BreakpointType::Access;
        break;
    default:
        return SendReply("E01");
    }

    auto start_offset = command_buffer + 3;
    auto addr_pos = std::find(start_offset, command_buffer + command_length, ',');
    PAddr addr = HexToLong(start_offset, static_cast<u64>(addr_pos - start_offset));

    if (type == BreakpointType::Access) {
        // Access is made up of Read and Write types, so add both breakpoints
        type = BreakpointType::Read;
        RemoveBreakpoint(type, addr);

        type = BreakpointType::Write;
    }

    RemoveBreakpoint(type, addr);
    SendReply("OK");
}

void HandlePacket() {
    if (!IsConnected()) {
        return;
    }

    if (!IsDataAvailable()) {
        return;
    }

    ReadCommand();
    if (command_length == 0) {
        return;
    }

    LOG_DEBUG(Debug_GDBStub, "Packet: {}", command_buffer);

    switch (command_buffer[0]) {
    case 'q':
        HandleQuery();
        break;
    case 'H':
        HandleSetThread();
        break;
    case '?':
        SendSignal(current_thread, latest_signal);
        break;
    case 'k':
        Shutdown();
        LOG_INFO(Debug_GDBStub, "killed by gdb");
        return;
    case 'g':
        ReadRegisters();
        break;
    case 'G':
        WriteRegisters();
        break;
    case 'p':
        ReadRegister();
        break;
    case 'P':
        WriteRegister();
        break;
    case 'm':
        ReadMemory();
        break;
    case 'M':
        WriteMemory();
        break;
    case 's':
        Step();
        return;
    case 'C':
    case 'c':
        Continue();
        return;
    case 'z':
        RemoveBreakpoint();
        break;
    case 'Z':
        AddBreakpoint();
        break;
    case 'T':
        HandleThreadAlive();
        break;
    default:
        SendReply("");
        break;
    }
}

void SetServerPort(u16 port) {
    gdbstub_port = port;
}

void ToggleServer(bool status) {
    if (status) {
        server_enabled = status;

        // Start server
        if (!IsConnected() && Core::System::GetInstance().IsPoweredOn()) {
            Init();
        }
    } else {
        // Stop server
        if (IsConnected()) {
            Shutdown();
        }

        server_enabled = status;
    }
}

static void Init(u16 port) {
    if (!server_enabled) {
        // Set the halt loop to false in case the user enabled the gdbstub mid-execution.
        // This way the CPU can still execute normally.
        halt_loop = false;
        step_loop = false;
        return;
    }

    // Setup initial gdbstub status
    halt_loop = true;
    step_loop = false;

    breakpoints_execute.clear();
    breakpoints_read.clear();
    breakpoints_write.clear();

    modules.clear();

    // Start gdb server
    LOG_INFO(Debug_GDBStub, "Starting GDB server on port {}...", port);

    sockaddr_in saddr_server = {};
    saddr_server.sin_family = AF_INET;
    saddr_server.sin_port = htons(port);
    saddr_server.sin_addr.s_addr = INADDR_ANY;

#ifdef _WIN32
    WSAStartup(MAKEWORD(2, 2), &InitData);
#endif

    int tmpsock = static_cast<int>(socket(PF_INET, SOCK_STREAM, 0));
    if (tmpsock == -1) {
        LOG_ERROR(Debug_GDBStub, "Failed to create gdb socket");
    }

    // Set socket to SO_REUSEADDR so it can always bind on the same port
    int reuse_enabled = 1;
    if (setsockopt(tmpsock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse_enabled,
                   sizeof(reuse_enabled)) < 0) {
        LOG_ERROR(Debug_GDBStub, "Failed to set gdb socket option");
    }

    const sockaddr* server_addr = reinterpret_cast<const sockaddr*>(&saddr_server);
    socklen_t server_addrlen = sizeof(saddr_server);
    if (bind(tmpsock, server_addr, server_addrlen) < 0) {
        LOG_ERROR(Debug_GDBStub, "Failed to bind gdb socket");
    }

    if (listen(tmpsock, 1) < 0) {
        LOG_ERROR(Debug_GDBStub, "Failed to listen to gdb socket");
    }

    // Wait for gdb to connect
    LOG_INFO(Debug_GDBStub, "Waiting for gdb to connect...");
    sockaddr_in saddr_client;
    sockaddr* client_addr = reinterpret_cast<sockaddr*>(&saddr_client);
    socklen_t client_addrlen = sizeof(saddr_client);
    gdbserver_socket = static_cast<int>(accept(tmpsock, client_addr, &client_addrlen));
    if (gdbserver_socket < 0) {
        // In the case that we couldn't start the server for whatever reason, just start CPU
        // execution like normal.
        halt_loop = false;
        step_loop = false;

        LOG_ERROR(Debug_GDBStub, "Failed to accept gdb client");
    } else {
        LOG_INFO(Debug_GDBStub, "Client connected.");
        saddr_client.sin_addr.s_addr = ntohl(saddr_client.sin_addr.s_addr);
    }

    // Clean up temporary socket if it's still alive at this point.
    if (tmpsock != -1) {
        shutdown(tmpsock, SHUT_RDWR);
    }
}

void Init() {
    Init(gdbstub_port);
}

void Shutdown() {
    if (!server_enabled) {
        return;
    }

    LOG_INFO(Debug_GDBStub, "Stopping GDB ...");
    if (gdbserver_socket != -1) {
        shutdown(gdbserver_socket, SHUT_RDWR);
        gdbserver_socket = -1;
    }

#ifdef _WIN32
    WSACleanup();
#endif

    LOG_INFO(Debug_GDBStub, "GDB stopped.");
}

bool IsServerEnabled() {
    return server_enabled;
}

bool IsConnected() {
    return IsServerEnabled() && gdbserver_socket != -1;
}

bool GetCpuHaltFlag() {
    return halt_loop;
}

bool GetCpuStepFlag() {
    return step_loop;
}

void SetCpuStepFlag(bool is_step) {
    step_loop = is_step;
}

void SendTrap(Kernel::Thread* thread, int trap) {
    if (send_trap) {
        if (!halt_loop || current_thread == thread) {
            current_thread = thread;
            SendSignal(thread, trap);
        }
        halt_loop = true;
        send_trap = false;
    }
}
}; // namespace GDBStub
