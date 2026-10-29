// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Modbus/TCP: the protocol on bytes, and the map from the Modbus tables
// onto operands of the program. Modbus Application Protocol Specification
// V1.1b3 and Modbus Messaging on TCP/IP Implementation Guide V1.0b.
#include "modbus.hpp"

#include <cstring>
#include <stdexcept>

#include "sim.hpp"
#include "value.hpp"

namespace s7forge {
namespace {

// the exception codes
constexpr uint8_t kIllegalFunction = 0x01;
constexpr uint8_t kIllegalAddress = 0x02;
constexpr uint8_t kIllegalValue = 0x03;
constexpr uint8_t kDeviceFailure = 0x04;

std::string addressText(const Address& a) {
    std::string t = "%";
    t += a.area;
    if (a.size != 'X') t += a.size;
    t += std::to_string(a.byte);
    if (a.size == 'X') t += "." + std::to_string(a.bit);
    return t;
}

// the number of 16-bit registers a value of this type takes, and whether
// its bits are those of a Real; 0 for a type Modbus cannot carry here
// (a String; a Date_And_Time, which the CPU keeps as 8 bytes of BCD)
int wordsOf(const Val& v, bool& real) {
    real = v.kind == Kind::Real;
    if (v.kind == Kind::String || v.kind == Kind::None) return 0;
    if (v.kind == Kind::Dt && v.type && v.type->unitNs != 1) return 0;
    const int bits = v.type ? v.type->bits : (v.kind == Kind::Real ? 32 : 16);
    if (bits <= 16) return 1;
    if (bits <= 32) return 2;
    return 4;
}

// the value as the bits the PLC keeps (a Time in its units, a Real as IEEE)
uint64_t rawOf(const Val& v, int words) {
    switch (v.kind) {
        case Kind::Bool: return v.b ? 1 : 0;
        case Kind::Real: {
            if (words == 2) {
                const float f = static_cast<float>(v.r);
                uint32_t u;
                std::memcpy(&u, &f, 4);
                return u;
            }
            uint64_t u;
            std::memcpy(&u, &v.r, 8);
            return u;
        }
        case Kind::Time:
        case Kind::Tod:
        case Kind::Dt: {
            // an S5TIME as the CPU keeps it: time base and BCD
            if (isS5Time(v.type)) return s5timeBits(v.i);
            const int64_t unit = v.type && v.type->unitNs > 0 ? v.type->unitNs : 1000000;
            return static_cast<uint64_t>(v.i / unit);
        }
        default: return static_cast<uint64_t>(v.i);
    }
}

// a value of the type of `current` from its bits
Val valueOf(uint64_t raw, const Val& current, int words) {
    const int bits = words * 16;
    const uint64_t mask = bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
    raw &= mask;
    switch (current.kind) {
        case Kind::Bool: return boolVal(raw != 0);
        case Kind::Real: {
            if (words == 2) {
                const uint32_t u = static_cast<uint32_t>(raw);
                float f;
                std::memcpy(&f, &u, 4);
                return realVal(f, current.type);
            }
            double d;
            std::memcpy(&d, &raw, 8);
            return realVal(d, current.type);
        }
        case Kind::Time:
        case Kind::Tod:
        case Kind::Dt: {
            Val v = current;
            if (isS5Time(current.type)) {
                v.i = s5timeNs(static_cast<uint16_t>(raw));
                return v;
            }
            const int64_t unit = current.type && current.type->unitNs > 0 ? current.type->unitNs : 1000000;
            int64_t n = static_cast<int64_t>(raw);
            // a Time is signed; a time of day and an LDT are not
            if (current.kind == Kind::Time && bits < 64 && ((raw >> (bits - 1)) & 1))
                n = static_cast<int64_t>(raw) - static_cast<int64_t>(1ULL << bits);
            v.i = n * unit;
            return v;
        }
        default: {
            // signed types are written as their two's complement; store
            // wraps the number to the variable's size
            int64_t n = static_cast<int64_t>(raw);
            if (current.type && current.type->isSigned && bits < 64 && ((raw >> (bits - 1)) & 1))
                n = static_cast<int64_t>(raw) - static_cast<int64_t>(1ULL << bits);
            Val v = intVal(n, current.type);
            v.kind = current.kind == Kind::None ? Kind::Int : current.kind;
            return v;
        }
    }
}

// The next element of an array operand: "D".arr[3] -> "D".arr[4]
bool nextIndex(const std::string& op, std::string& next) {
    if (op.empty() || op.back() != ']') return false;
    const size_t open = op.rfind('[');
    if (open == std::string::npos) return false;
    const std::string inner = op.substr(open + 1, op.size() - open - 2);
    const size_t comma = inner.rfind(',');
    const std::string last = comma == std::string::npos ? inner : inner.substr(comma + 1);
    char* end = nullptr;
    const long n = std::strtol(last.c_str(), &end, 10);
    if (end == last.c_str() || *end) return false;
    next = op.substr(0, open + 1) + (comma == std::string::npos ? std::string() : inner.substr(0, comma + 1)) +
           std::to_string(n + 1) + "]";
    return true;
}

void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}
uint16_t get16(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<uint16_t>((b[at] << 8) | b[at + 1]);
}

std::vector<uint8_t> exceptionPdu(uint8_t fc, uint8_t code) {
    return {static_cast<uint8_t>(fc | 0x80), code};
}

const ModbusRange* rangeOf(const std::vector<ModbusRange>& table, uint32_t address) {
    for (const auto& r : table)
        if (address >= r.start && address < r.start + r.count) return &r;
    return nullptr;
}

}  // namespace

// ---- the map ----

ModbusMap ModbusMap::defaults() {
    ModbusMap m;
    auto range = [](char area, char size, uint32_t count, const char* text) {
        ModbusRange r;
        r.start = 0;
        r.count = count;
        r.area = true;
        r.at.area = area;
        r.at.size = size;
        r.text = text;
        return r;
    };
    m.coils.push_back(range('Q', 'X', 65536, "%Q0.0"));
    m.discreteInputs.push_back(range('I', 'X', 65536, "%I0.0"));
    m.holdingRegisters.push_back(range('M', 'W', 32768, "%MW0"));
    m.inputRegisters.push_back(range('I', 'W', 32768, "%IW0"));
    return m;
}

ModbusMap ModbusMap::fromJson(const Json& j, Simulator& sim) {
    if (j.type != Json::Object) throw std::runtime_error("the Modbus map is not a JSON object");
    ModbusMap m;
    // a whole number where the map has one; a key with anything else is refused
    auto whole = [](const Json& x, const std::string& what) -> int64_t {
        if (x.type != Json::Number || x.number != static_cast<double>(static_cast<int64_t>(x.number)))
            throw std::runtime_error(what + " is a whole number");
        return static_cast<int64_t>(x.number);
    };
    if (j["unit_id"].type != Json::Null) {
        const int64_t u = whole(j["unit_id"], "unit_id");
        if (u < 0 || u > 255) throw std::runtime_error("unit_id is 0 to 255");
        m.unitId = static_cast<int>(u);
    }
    for (const auto& member : j.members) {
        const std::string& key = member.first;
        if (key == "unit_id" || key == "comment") continue;
        const bool bits = key == "coils" || key == "discrete_inputs";
        std::vector<ModbusRange>* table = key == "coils"               ? &m.coils
                                          : key == "discrete_inputs"   ? &m.discreteInputs
                                          : key == "holding_registers" ? &m.holdingRegisters
                                          : key == "input_registers"   ? &m.inputRegisters
                                                                       : nullptr;
        if (!table) throw std::runtime_error("the Modbus map has an unknown key \"" + key + "\"");
        if (member.second.type != Json::Array) throw std::runtime_error(key + " is a list of ranges");
        for (const auto& e : member.second.items) {
            if (e.type != Json::Object) throw std::runtime_error(key + ": each range is an object");
            for (const auto& f : e.members)
                if (f.first != "address" && f.first != "count" && f.first != "operand" && f.first != "comment")
                    throw std::runtime_error(key + ": a range has an unknown key \"" + f.first + "\"");
            const int64_t start = whole(e["address"], key + ": address");
            const int64_t count = e["count"].type == Json::Null ? 1 : whole(e["count"], key + ": count");
            const std::string op = e["operand"].str();
            const std::string where = key + " " + std::to_string(start) + " (" + op + ")";
            if (start < 0 || start > 65535) throw std::runtime_error(key + ": an address 0 to 65535 is needed");
            if (count < 1 || start + count > 65536) throw std::runtime_error(where + ": count 1 to the end of the table");
            if (op.empty()) throw std::runtime_error(where + ": no operand");
            ModbusRange r;
            r.start = static_cast<uint16_t>(start);
            r.text = op;
            const Address at = parseAddress(op);
            if (at.area) {
                // an area range
                r.area = true;
                r.at = at;
                const char sz = at.size;
                if (bits) {
                    if (sz != 'X') throw std::runtime_error(where + ": a coil or discrete input is a bit (%Q0.0)");
                    if ((static_cast<uint64_t>(at.byte) * 8 + at.bit + count) > 65536ULL * 8)
                        throw std::runtime_error(where + ": beyond the end of the area");
                    r.count = static_cast<uint32_t>(count);
                } else {
                    if (sz != 'W' && sz != 'D')
                        throw std::runtime_error(where + ": a register range is words or double words (%MW20, %MD20)");
                    const int64_t bytes = (sz == 'W' ? 2 : 4) * (sz == 'W' ? count : (count + 1) / 2);
                    if (sz == 'D' && count % 2) throw std::runtime_error(where + ": double words take two registers each: an even count");
                    if (at.byte + bytes > 65536) throw std::runtime_error(where + ": beyond the end of the area");
                    r.count = static_cast<uint32_t>(count);
                }
            } else {
                // symbolic operands; with a count, the elements of an array from the one given
                std::string cur = op;
                uint32_t offset = 0;
                for (int64_t k = 0; k < count; ++k) {
                    if (k > 0 && !nextIndex(cur, cur))
                        throw std::runtime_error(where + ": a count needs an array element (\"Data\".arr[0])");
                    Val v;
                    std::string err;
                    if (!sim.read(cur, v, err)) throw std::runtime_error(where + ": " + err);
                    ModbusRange::Item it;
                    it.operand = cur;
                    if (bits) {
                        if (v.kind != Kind::Bool) throw std::runtime_error(where + ": " + cur + " is not a Bool");
                        it.words = 1;
                    } else {
                        it.words = wordsOf(v, it.real);
                        if (!it.words) throw std::runtime_error(where + ": " + cur + " has a type Modbus cannot carry");
                    }
                    r.firstWord.push_back(offset);
                    offset += bits ? 1 : static_cast<uint32_t>(it.words);
                    r.items.push_back(it);
                }
                r.count = offset;
                if (r.start + r.count > 65536) throw std::runtime_error(where + ": beyond the end of the table");
            }
            for (const auto& other : *table)
                if (r.start < other.start + other.count && other.start < r.start + r.count)
                    throw std::runtime_error(where + ": overlaps " + other.text);
            table->push_back(r);
        }
    }
    return m;
}

// ---- the server ----

ModbusServer::ModbusServer(Simulator& sim, ModbusMap map, bool readOnly)
    : sim_(sim), map_(std::move(map)), readOnly_(readOnly) {}

bool ModbusServer::readBit(const std::vector<ModbusRange>& table, uint32_t address, bool& v, int& error) {
    const ModbusRange* r = rangeOf(table, address);
    if (!r) {
        error = kIllegalAddress;
        return false;
    }
    const uint32_t k = address - r->start;
    std::string op;
    if (r->area) {
        const uint64_t bit = static_cast<uint64_t>(r->at.byte) * 8 + r->at.bit + k;
        Address a = r->at;
        a.byte = static_cast<uint32_t>(bit / 8);
        a.bit = static_cast<int>(bit % 8);
        op = addressText(a);
    } else {
        op = r->items[k].operand;
    }
    Val val;
    std::string err;
    if (!sim_.read(op, val, err)) {
        error = kDeviceFailure;
        return false;
    }
    v = truthy(val);
    return true;
}

// the register: the operand, which part of it, and how many parts it has
namespace {
struct WordAt {
    std::string operand;
    int part = 0, words = 1;
};
bool wordAt(const ModbusRange& r, uint32_t k, WordAt& w) {
    if (r.area) {
        Address a = r.at;
        if (a.size == 'W') {
            a.byte = r.at.byte + 2 * k;
            w.words = 1;
            w.part = 0;
        } else {
            a.byte = r.at.byte + 4 * (k / 2);
            w.words = 2;
            w.part = static_cast<int>(k % 2);
        }
        w.operand = addressText(a);
        return true;
    }
    for (size_t i = 0; i < r.items.size(); ++i)
        if (k >= r.firstWord[i] && k < r.firstWord[i] + static_cast<uint32_t>(r.items[i].words)) {
            w.operand = r.items[i].operand;
            w.words = r.items[i].words;
            w.part = static_cast<int>(k - r.firstWord[i]);
            return true;
        }
    return false;
}
}  // namespace

bool ModbusServer::readWord(const std::vector<ModbusRange>& table, uint32_t address, uint16_t& v, int& error) {
    const ModbusRange* r = rangeOf(table, address);
    WordAt w;
    if (!r || !wordAt(*r, address - r->start, w)) {
        error = kIllegalAddress;
        return false;
    }
    Val val;
    std::string err;
    if (!sim_.read(w.operand, val, err)) {
        error = kDeviceFailure;
        return false;
    }
    const uint64_t raw = rawOf(val, w.words);
    // the high word first
    v = static_cast<uint16_t>(raw >> (16 * (w.words - 1 - w.part)));
    return true;
}

bool ModbusServer::writeBit(const std::vector<ModbusRange>& table, uint32_t address, bool v, int& error) {
    const ModbusRange* r = rangeOf(table, address);
    if (!r) {
        error = kIllegalAddress;
        return false;
    }
    const uint32_t k = address - r->start;
    std::string op;
    if (r->area) {
        const uint64_t bit = static_cast<uint64_t>(r->at.byte) * 8 + r->at.bit + k;
        Address a = r->at;
        a.byte = static_cast<uint32_t>(bit / 8);
        a.bit = static_cast<int>(bit % 8);
        op = addressText(a);
    } else {
        op = r->items[k].operand;
    }
    std::string err;
    if (!sim_.write(op, boolVal(v), err)) {
        error = kDeviceFailure;
        return false;
    }
    return true;
}

bool ModbusServer::writeWords(const std::vector<ModbusRange>& table, uint32_t start, const std::vector<uint16_t>& v,
                              int& error) {
    // the values touched, in order, each with its bits as they become
    struct Pending {
        std::string operand;
        Val current;
        int words = 1;
        uint64_t raw = 0;
    };
    std::vector<Pending> pending;
    std::string err;
    for (size_t k = 0; k < v.size(); ++k) {
        const ModbusRange* r = rangeOf(table, start + static_cast<uint32_t>(k));
        WordAt w;
        if (!r || !wordAt(*r, start + static_cast<uint32_t>(k) - r->start, w)) {
            error = kIllegalAddress;
            return false;
        }
        if (pending.empty() || pending.back().operand != w.operand) {
            Pending p;
            p.operand = w.operand;
            p.words = w.words;
            if (!sim_.read(w.operand, p.current, err)) {
                error = kDeviceFailure;
                return false;
            }
            // the other words of a 32- or 64-bit value stay as they are
            p.raw = rawOf(p.current, w.words);
            pending.push_back(p);
        }
        Pending& p = pending.back();
        const int shift = 16 * (w.words - 1 - w.part);
        p.raw = (p.raw & ~(0xFFFFULL << shift)) | (static_cast<uint64_t>(v[k]) << shift);
    }
    for (const Pending& p : pending)
        if (!sim_.write(p.operand, valueOf(p.raw, p.current, p.words), err)) {
            error = kDeviceFailure;
            return false;
        }
    return true;
}

std::vector<uint8_t> ModbusServer::pdu(const std::vector<uint8_t>& q) {
    ++stats_.requests;
    if (q.empty()) {
        ++stats_.exceptions;
        return exceptionPdu(0, kIllegalFunction);
    }
    const uint8_t fc = q[0];
    auto fail = [&](uint8_t code) {
        ++stats_.exceptions;
        last_ += " -> exception " + std::to_string(code);
        return exceptionPdu(fc, code);
    };
    auto need = [&](size_t n) { return q.size() == n; };
    std::vector<uint8_t> a{fc};
    int error = 0;
    const bool write = fc == 5 || fc == 6 || fc == 15 || fc == 16 || fc == 23;
    last_ = "FC" + std::to_string(fc);
    if (write && readOnly_) {
        ++stats_.refusedWrites;
        last_ += " (a write, refused: read-only)";
        return fail(kIllegalFunction);
    }
    switch (fc) {
        case 1:  // read coils
        case 2: {  // read discrete inputs
            if (!need(5)) return fail(kIllegalValue);
            const uint32_t start = get16(q, 1), n = get16(q, 3);
            last_ += std::string(fc == 1 ? " read coils " : " read discrete inputs ") + std::to_string(start) + "+" +
                     std::to_string(n);
            if (n < 1 || n > 2000) return fail(kIllegalValue);
            if (start + n > 65536) return fail(kIllegalAddress);
            std::vector<uint8_t> bytes((n + 7) / 8, 0);
            for (uint32_t k = 0; k < n; ++k) {
                bool v = false;
                if (!readBit(fc == 1 ? map_.coils : map_.discreteInputs, start + k, v, error))
                    return fail(static_cast<uint8_t>(error));
                if (v) bytes[k / 8] |= static_cast<uint8_t>(1u << (k % 8));
            }
            a.push_back(static_cast<uint8_t>(bytes.size()));
            a.insert(a.end(), bytes.begin(), bytes.end());
            return a;
        }
        case 3:  // read holding registers
        case 4: {  // read input registers
            if (!need(5)) return fail(kIllegalValue);
            const uint32_t start = get16(q, 1), n = get16(q, 3);
            last_ += std::string(fc == 3 ? " read holding registers " : " read input registers ") + std::to_string(start) +
                     "+" + std::to_string(n);
            if (n < 1 || n > 125) return fail(kIllegalValue);
            if (start + n > 65536) return fail(kIllegalAddress);
            a.push_back(static_cast<uint8_t>(2 * n));
            for (uint32_t k = 0; k < n; ++k) {
                uint16_t v = 0;
                if (!readWord(fc == 3 ? map_.holdingRegisters : map_.inputRegisters, start + k, v, error))
                    return fail(static_cast<uint8_t>(error));
                put16(a, v);
            }
            return a;
        }
        case 5: {  // write single coil
            if (!need(5)) return fail(kIllegalValue);
            const uint32_t at = get16(q, 1);
            const uint16_t v = get16(q, 3);
            last_ += " write coil " + std::to_string(at);
            if (v != 0xFF00 && v != 0x0000) return fail(kIllegalValue);
            if (!writeBit(map_.coils, at, v == 0xFF00, error)) return fail(static_cast<uint8_t>(error));
            ++stats_.writes;
            return q;
        }
        case 6: {  // write single register
            if (!need(5)) return fail(kIllegalValue);
            const uint32_t at = get16(q, 1);
            last_ += " write register " + std::to_string(at);
            if (!writeWords(map_.holdingRegisters, at, {get16(q, 3)}, error)) return fail(static_cast<uint8_t>(error));
            ++stats_.writes;
            return q;
        }
        case 15: {  // write multiple coils
            if (q.size() < 6) return fail(kIllegalValue);
            const uint32_t start = get16(q, 1), n = get16(q, 3), bytes = q[5];
            last_ += " write coils " + std::to_string(start) + "+" + std::to_string(n);
            if (n < 1 || n > 1968 || bytes != (n + 7) / 8 || q.size() != 6 + bytes) return fail(kIllegalValue);
            if (start + n > 65536) return fail(kIllegalAddress);
            // all addresses mapped before anything is written
            for (uint32_t k = 0; k < n; ++k)
                if (!rangeOf(map_.coils, start + k)) return fail(kIllegalAddress);
            for (uint32_t k = 0; k < n; ++k)
                if (!writeBit(map_.coils, start + k, (q[6 + k / 8] >> (k % 8)) & 1, error))
                    return fail(static_cast<uint8_t>(error));
            ++stats_.writes;
            a.insert(a.end(), q.begin() + 1, q.begin() + 5);
            return a;
        }
        case 16: {  // write multiple registers
            if (q.size() < 6) return fail(kIllegalValue);
            const uint32_t start = get16(q, 1), n = get16(q, 3), bytes = q[5];
            last_ += " write registers " + std::to_string(start) + "+" + std::to_string(n);
            if (n < 1 || n > 123 || bytes != 2 * n || q.size() != 6 + bytes) return fail(kIllegalValue);
            if (start + n > 65536) return fail(kIllegalAddress);
            for (uint32_t k = 0; k < n; ++k)
                if (!rangeOf(map_.holdingRegisters, start + k)) return fail(kIllegalAddress);
            std::vector<uint16_t> words;
            for (uint32_t k = 0; k < n; ++k) words.push_back(get16(q, 6 + 2 * k));
            if (!writeWords(map_.holdingRegisters, start, words, error)) return fail(static_cast<uint8_t>(error));
            ++stats_.writes;
            a.insert(a.end(), q.begin() + 1, q.begin() + 5);
            return a;
        }
        case 23: {  // read/write multiple registers: the write first
            if (q.size() < 10) return fail(kIllegalValue);
            const uint32_t rs = get16(q, 1), rn = get16(q, 3), ws = get16(q, 5), wn = get16(q, 7), bytes = q[9];
            last_ += " read registers " + std::to_string(rs) + "+" + std::to_string(rn) + ", write registers " +
                     std::to_string(ws) + "+" + std::to_string(wn);
            if (rn < 1 || rn > 125 || wn < 1 || wn > 121 || bytes != 2 * wn || q.size() != 10 + bytes)
                return fail(kIllegalValue);
            if (rs + rn > 65536 || ws + wn > 65536) return fail(kIllegalAddress);
            for (uint32_t k = 0; k < wn; ++k)
                if (!rangeOf(map_.holdingRegisters, ws + k)) return fail(kIllegalAddress);
            for (uint32_t k = 0; k < rn; ++k)
                if (!rangeOf(map_.holdingRegisters, rs + k)) return fail(kIllegalAddress);
            std::vector<uint16_t> words;
            for (uint32_t k = 0; k < wn; ++k) words.push_back(get16(q, 10 + 2 * k));
            if (!writeWords(map_.holdingRegisters, ws, words, error)) return fail(static_cast<uint8_t>(error));
            ++stats_.writes;
            a.push_back(static_cast<uint8_t>(2 * rn));
            for (uint32_t k = 0; k < rn; ++k) {
                uint16_t v = 0;
                if (!readWord(map_.holdingRegisters, rs + k, v, error)) return fail(static_cast<uint8_t>(error));
                put16(a, v);
            }
            return a;
        }
        default:
            last_ += " (not supported)";
            return fail(kIllegalFunction);
    }
}

std::vector<uint8_t> ModbusServer::handle(const std::vector<uint8_t>& f, bool& close) {
    close = false;
    // the MBAP header: transaction, protocol 0, length (unit + PDU), unit
    if (f.size() < 8 || get16(f, 2) != 0 || get16(f, 4) != f.size() - 6 || f.size() > 260) {
        close = true;
        return {};
    }
    const uint8_t unit = f[6];
    if (map_.unitId >= 0 && unit != map_.unitId) return {};
    const std::vector<uint8_t> answer = pdu(std::vector<uint8_t>(f.begin() + 7, f.end()));
    std::vector<uint8_t> out{f[0], f[1], 0, 0};
    put16(out, static_cast<uint16_t>(answer.size() + 1));
    out.push_back(unit);
    out.insert(out.end(), answer.begin(), answer.end());
    return out;
}

}  // namespace s7forge
