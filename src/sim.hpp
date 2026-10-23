// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The simulated CPU: it loads the program of one PLC as tiaconv writes it
// (--program-json, schema version 2) and runs it the way the CPU does: the
// startup OBs once, then the program cycle OBs again and again, the cyclic
// interrupt OBs when their time comes. Time is virtual: it advances by the
// scan time with every cycle, so a run is the same every time.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"
#include "memory.hpp"
#include "value.hpp"

namespace s7forge {

class Simulator {
public:
    // `program`: the parsed file. Throws std::runtime_error when it is not a
    // program tiaconv wrote or cannot be laid out.
    explicit Simulator(Json program);

    // Power on: everything to its start values, retained data too, then
    // the startup OBs. warmRestart: STOP to RUN, retained data kept.
    void powerOn();
    void warmRestart();
    // One cycle: the program cycle OBs, then virtual time advances by the
    // scan time and the cyclic interrupts that came due run.
    void scan();
    // Cycles until `ns` of virtual time have passed.
    void runFor(int64_t ns);

    int64_t now() const { return now_; }
    int64_t scanTime() const { return scanNs_; }
    void setScanTime(int64_t ns) { scanNs_ = ns > 0 ? ns : 1000000; }
    // false after a fault that stops a real CPU (a loop beyond the maximum
    // cycle time); `fault()` says what
    bool running() const { return running_; }
    const std::string& fault() const { return fault_; }

    // An operand by name: a PLC tag (StartPB, "StartPB"), a data block
    // member ("MotorData".starts, "Motor_DB".delay.ET, "DB".arr[3]) or an
    // address (%I0.0, %MW20). False with `error` set when it is not found.
    bool read(const std::string& operand, Val& out, std::string& error);
    bool write(const std::string& operand, const Val& value, std::string& error);

    // What the program needs that S7Forge does not do, each said once:
    // blocks without code, STL, instructions not simulated.
    const std::vector<std::string>& warnings() const { return warnings_; }
    // where a variable of a data block with standard access is: its first
    // bit from the start of the DB, as STL addresses it (DBX, DBW); -1 when
    // the DB has optimized access or the operand is not found
    int64_t bitOffset(const std::string& operand);
    std::string plcName() const;

    // Places where values live; public for the operand parser in tests.
    struct Place {
        Var* var = nullptr;
        Area* area = nullptr;
        Address at;
        const ElemType* type = nullptr;  // area: the type of the value there
        int sliceBits = 0;               // a part of an integer: 1, 8, 16, 32
        int sliceIndex = 0;
        bool valid() const { return var || area; }
    };

private:
    // The STL statements of a block, all networks in one list: a jump may go
    // to a label in another network of the block.
    struct StlCode {
        std::vector<const Json*> code;
        std::vector<size_t> net;               // the network of each statement
        std::vector<size_t> first;             // of each network: its first statement, or npos
        std::map<std::string, size_t> labels;  // a label: the statement after it
        bool german = false;                   // in German mnemonics (U, SPB, SE for SD)
        static constexpr size_t npos() { return static_cast<size_t>(-1); }
    };
    struct Block {
        const Json* json = nullptr;
        std::string name, type, language;
        const Json* interface = nullptr;  // null when not read (protected)
        const Json* code = nullptr;       // null when not read
        std::map<int64_t, const Json*> refs;
        std::shared_ptr<StlCode> stl;     // null when it has no STL network
    };
    // The registers of STL in a call: the status word (in the parts STL
    // uses) and the four accumulators of an S7-1500 CPU.
    struct StlState {
        bool term = false;   // the result of the current AND term
        bool orBit = false;  // an earlier AND term was TRUE (O without an operand)
        bool fc = false;     // /FC: FALSE at the start of a logic string
        bool br = true;      // the binary result; ENO of an STL block at its end
        bool ov = false, os = false, cc0 = false, cc1 = false;
        bool mcrActive = false;  // MCRA .. MCRD
        uint32_t accu[4] = {0, 0, 0, 0};
        struct Nest {
            std::string op;
            bool term, orBit, fc;
        };
        std::vector<Nest> nest;  // A( O( X( ..., up to 7 deep
        bool rlo() const { return orBit || term; }
    };
    struct Tag {
        std::string name;
        const ElemType* type = nullptr;
        Address at;
        std::unique_ptr<Var> var;  // a tag without an address
    };
    struct DbInfo;
    struct Frame {
        const Block* block = nullptr;
        Var* instance = nullptr;            // FB: its instance
        std::unique_ptr<Var> local;         // Temp, FC parameters, Return, Constant
        std::map<std::string, Place> inOut; // FC: in-out parameters, by reference
        std::map<std::string, Val> wires;   // LAD, FBD: the wires of the network
        bool eno = true;
        bool returned = false;
        int loop = 0;                       // 1 EXIT, 2 CONTINUE
        int depth = 0;
        std::unique_ptr<StlState> stl;      // STL: its registers, made on its first statement
        std::unique_ptr<DbInfo> lData;  // the local data by address, laid out on first use
        std::vector<uint8_t> lRaw;          // the local data after the temporaries
    };
    struct TimerState {
        bool running = false;
        bool lastIn = false;
        int64_t start = 0;
        int64_t elapsed = 0;  // TONR: what was accumulated before
    };
    // An absolute or indirect operand of STL, once its address is known.
    struct Abs {
        char area = 0;     // I Q M L; D (DB), N (DI); P (PI), O (PQ); T, C; B (OPN DB), J (OPN DI)
        char size = 'X';   // X B W D
        uint32_t byte = 0;
        int bit = 0;
        int db = -1;       // a DB given with the operand (DB5.DBW2); -1: the open one
        int number = 0;    // T, C, B, J: the timer, counter, DB
        bool isPointer = false;  // P#M10.0: the pointer itself is the value
        uint32_t pointer = 0;
    };
    // A data block as bytes: its leaves at their offsets (standard access)
    struct Slot {
        Var* var;
        int64_t bit;   // the first bit, from the start of the DB
        int64_t bits;  // the size
    };
    struct DbInfo {
        std::string name;
        int number = 0;
        bool optimized = true;
        const Json* json = nullptr;
        bool laidOut = false;
        std::vector<Slot> slots;
        std::map<const Var*, int64_t> starts;  // every variable: its first bit
        int64_t bytes = 0;
        std::vector<uint8_t> image;            // the bytes as last written: padding kept
    };
    struct S5Timer {
        bool running = false, q = false, lastStart = false, lastFr = false, startRlo = false;
        char mode = 0;  // P E D S F: SP SE SD SS SF
        int64_t start = 0, presetNs = 0;
        int base = 0;              // 0..3: 10 ms, 100 ms, 1 s, 10 s
        int64_t frozenUnits = 0;   // the time value when stopped
    };
    struct S5Counter {
        int value = 0;
        bool lastCu = false, lastCd = false, lastS = false, lastFr = false;
    };
    struct Cyclic {
        const Block* ob = nullptr;
        int64_t cycle = 0, phase = 0, priority = 0, next = 0;
    };

    Json prog_;
    std::map<std::string, Block> blocks_;
    std::map<std::string, std::unique_ptr<Var>> dbs_;
    std::map<std::string, Tag> tags_;
    std::map<std::string, Val> constants_;  // the user constants of the tag tables
    std::map<int, DbInfo> dbInfo_;          // the data blocks by number
    std::map<std::string, std::pair<char, int>> s5Tags_;  // tags of the type Timer, Counter
    std::vector<S5Timer> t5_ = std::vector<S5Timer>(2048);
    std::vector<S5Counter> c5_ = std::vector<S5Counter>(2048);
    // the registers of the CPU that STL addresses with: the open DB and
    // instance DB, AR1, AR2 (DB, DI and AR2 are kept over a call), the MCR stack
    int curDb_ = 0, curDi_ = 0;
    uint32_t ar1_ = 0, ar2_ = 0;
    std::vector<bool> mcrStack_;
    Area i_{'I'}, q_{'Q'}, m_{'M'};
    std::map<const Var*, TimerState> timers_;
    std::vector<const Block*> startup_, cycle_;
    std::vector<Cyclic> cyclics_;
    std::vector<std::string> warnings_;
    std::set<std::string> warned_;
    int64_t now_ = 0, scanNs_ = 10000000;
    bool running_ = true, firstScan_ = true;
    bool lastEno_ = true;  // ENO of the last call: BR after a CALL in STL
    std::string fault_;
    uint64_t steps_ = 0;  // statements in this cycle, against an endless loop

    void warn(const std::string& w);
    void stop(const std::string& why);
    void resetMemory(bool retainedToo);
    void runOb(const Block& ob);
    void runBlock(Frame& f);

    std::unique_ptr<Var> frameLocals(const Block& b, const std::vector<std::string>& sections);

    const Json* ref(const Frame& f, const Json& e) const;
    // a reference that is a constant: its value
    bool constantOf(const Json& r, Val& v) const;
    Place resolveRef(Frame& f, const Json& r);
    Place follow(Frame& f, Place p, const Json& path, size_t from);
    Place placeOf(Frame& f, const Json& e);
    Place area(const Address& at, const ElemType* t);
    Val load(const Place& p);
    void store(const Place& p, const Val& v);
    void storeWire(Frame& f, const std::string& name, const Val& v);
    // a value into a place: a structure or array copied member by member
    void assign(Frame& f, const Json& target, const Json& value);

    Val eval(Frame& f, const Json& e);
    Val binary(const std::string& op, const Val& a, const Val& b, const ElemType* result);
    void exec(Frame& f, const Json& statements);
    void stmt(Frame& f, const Json& s);

    Val call(Frame& f, const Json& e);
    Val callBlock(Frame& f, const Json& e, const Json& target);
    Val callInstruction(Frame& f, const Json& e, const Json& target);
    bool enabled(Frame& f, const Json& e);
    Val builtin(Frame& f, const Json& e);
    Var* instanceOf(Frame& f, const Json& target);
    void iecTimer(const std::string& name, Var& inst);
    void iecCounter(const std::string& name, Var& inst, bool oldCu, bool oldCd);

    Place operand(const std::string& name, std::string& error);

    // STL (stl.cpp)
    static std::shared_ptr<StlCode> stlCode(const Json& networks);
    // runs the STL of the block from statement `pc` on, until it leaves the
    // network `net`; returns the network to go on with (a jump may change it)
    size_t runStl(Frame& f, size_t net, size_t pc);
    // one statement; returns the next statement
    size_t stlStep(Frame& f, StlState& st, size_t pc);
    uint32_t stlLoad(Frame& f, const Json& e, bool& ok);
    void stlTransfer(Frame& f, const Json& e, uint32_t raw);
    bool stlBit(Frame& f, StlState& st, const Json& e);
    // absolute.cpp: absolute and indirect operands, data blocks as bytes
    bool stlAddress(Frame& f, const std::string& text, Abs& a, bool& isAddress);
    uint32_t absRead(Frame& f, const Abs& a, bool& ok);
    void absWrite(Frame& f, const Abs& a, uint32_t raw);
    DbInfo* dbOf(Frame& f, char area, int db);
    void layOut(DbInfo& d);
    void layOutVars(DbInfo& d, Var& root, const Json& members, const char* onlySection);
    bool localBytes(Frame& f, uint32_t byte, int n, uint8_t* out, const uint8_t* in);
    bool dbRead(DbInfo& d, uint32_t byte, int n, uint8_t* out);
    bool dbWrite(DbInfo& d, uint32_t byte, int n, const uint8_t* in);
    int64_t bitOfVar(const Var* v, int& db);  // a variable of a standard DB: its first bit; -1 if none
    // S5 timers and counters (s5.cpp)
    void s5Update(S5Timer& t);
    int64_t s5Units(S5Timer& t);
    bool s5TimerStart(int n, char mode, bool rlo, uint16_t tv, const std::string& who);
    void s5TimerFr(int n, bool rlo);
    void s5TimerReset(int n);
    void s5CounterCount(int n, bool up, bool rlo);
    bool s5CounterSet(int n, bool rlo, uint16_t bcd, const std::string& who);
    void s5CounterReset(int n);
    void s5CounterFr(int n, bool rlo);
    bool s5Ref(Frame& f, const Json& r, char& kind, int& n);
    bool s5Operand(Frame& f, const Json& e, char want, int& n);
    bool s5Status(Frame& f, const Json& r, bool& value);
    bool s5Reset(Frame& f, const Json& r);
    bool s5Call(Frame& f, const Json& e, const std::string& name);
    bool s5Bits(Frame& f, const Json& e, uint16_t& bits);
    DbInfo& localLayout(Frame& f);
    void stlLogic(StlState& st, const std::string& op, bool v);
};

}  // namespace s7forge
