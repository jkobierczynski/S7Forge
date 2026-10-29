// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A Modbus/TCP server in front of the simulated CPU: the four Modbus tables
// (coils, discrete inputs, holding registers, input registers) mapped onto
// operands of the program (%Q, %I, %MW, %IW, tags, data block members), so
// that a Modbus client, a proxy or a scanner talks to a PLC whose behaviour
// is the program's. The protocol part (`handle`) works on bytes and has no
// sockets; the network part is in modbus_net.cpp.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"
#include "memory.hpp"

namespace s7forge {

class Simulator;

// How the Modbus addresses of one table reach the program.
struct ModbusRange {
    uint16_t start = 0;  // the first Modbus address (0-based, as on the wire)
    uint32_t count = 0;  // how many addresses
    // an area range: %Q0.0 (bits), %MW20, %IW0, %MD4 (two registers each)
    bool area = false;
    Address at;
    // otherwise symbolic operands, one after the other: each takes one
    // coil, or one register (16 bits) or two (32 bits, high word first) or
    // four (64 bits)
    struct Item {
        std::string operand;
        int words = 1;
        bool real = false;  // Real / LReal: IEEE 754 bits
    };
    std::vector<Item> items;
    std::vector<uint32_t> firstWord;  // of each item: its offset from `start`
    std::string text;                 // as written in the map, for messages
};

struct ModbusMap {
    std::vector<ModbusRange> coils, discreteInputs, holdingRegisters, inputRegisters;
    int unitId = -1;  // -1: answer any unit identifier
    // The default map: coils %Q, discrete inputs %I (Modbus address n is
    // bit n of the area, from %Q0.0 / %I0.0), holding registers %MW
    // (address n is %MW(2n)), input registers %IW (address n is %IW(2n)).
    static ModbusMap defaults();
    // A map file (docs/MODBUS.md); the operands are checked against `sim`.
    // Throws std::runtime_error with what is wrong.
    static ModbusMap fromJson(const Json& j, Simulator& sim);
};

struct ModbusStats {
    uint64_t requests = 0, exceptions = 0, writes = 0, refusedWrites = 0;
};

class ModbusServer {
public:
    ModbusServer(Simulator& sim, ModbusMap map, bool readOnly = false);

    // One Modbus/TCP frame (the MBAP header and the PDU) in, the answer out;
    // empty when there is none (a unit identifier that is not ours). `close`
    // is set when the frame is not Modbus/TCP: the connection is to be closed.
    std::vector<uint8_t> handle(const std::vector<uint8_t>& frame, bool& close);
    // The PDU alone: function code and data in, the answer PDU out.
    std::vector<uint8_t> pdu(const std::vector<uint8_t>& request);

    const ModbusStats& stats() const { return stats_; }
    // A line per request, for --verbose: "FC3 read holding registers 0+10".
    std::string lastRequest() const { return last_; }

private:
    Simulator& sim_;
    ModbusMap map_;
    bool readOnly_;
    ModbusStats stats_;
    std::string last_;

    // the value of one coil or register; false: the address is not mapped
    bool readBit(const std::vector<ModbusRange>& table, uint32_t address, bool& v, int& error);
    bool readWord(const std::vector<ModbusRange>& table, uint32_t address, uint16_t& v, int& error);
    bool writeBit(const std::vector<ModbusRange>& table, uint32_t address, bool v, int& error);
    // registers from `start` on: the words of one value are put together
    // first and the value written once
    bool writeWords(const std::vector<ModbusRange>& table, uint32_t start, const std::vector<uint16_t>& v, int& error);
};

// The network part: a TCP server on `listen`:`port` that answers Modbus
// requests between the cycles of the simulation, which runs in real time
// (times `speed`). Returns when `forNs` of simulated time have passed (0:
// never) or `stop` becomes true. `port` 0 takes a free port; `boundPort`
// says which (for tests).
struct ServeOptions {
    std::string listen = "127.0.0.1";
    int port = 5020;
    double speed = 1.0;
    int64_t forNs = 0;
    int maxClients = 16;
    bool verbose = false;
};
int serveModbus(Simulator& sim, ModbusServer& server, const ServeOptions& opt, const std::atomic<bool>* stop = nullptr,
                std::atomic<int>* boundPort = nullptr, std::string* error = nullptr);

}  // namespace s7forge
