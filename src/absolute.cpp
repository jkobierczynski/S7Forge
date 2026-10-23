// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Absolute and indirect operands of STL: I, Q, M, PI, PQ, DBX ... DID with
// an open data block, DB5.DBW2, memory-indirect (MW [MD 10]), register-
// indirect (DBW [AR1,P#2.0]) and area-crossing (W [AR1,P#0.0]) addresses,
// and pointers (P#M10.0). A data block with standard access is laid out in
// bytes as the CPU lays it out, so that DBW 2 reaches the members there.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>

#include "sim.hpp"

namespace s7forge {
namespace {

std::string squash(const std::string& s) {
    // upper case, '%' and blanks next to the punctuation gone
    std::string o;
    for (char c : s) {
        if (c == '%') continue;
        o += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    std::string r;
    for (size_t i = 0; i < o.size(); ++i) {
        if (o[i] == ' ' || o[i] == '\t') {
            if (r.empty() || r.back() == ' ' || r.back() == '[' || r.back() == ',' || r.back() == '.') continue;
            const size_t j = o.find_first_not_of(" \t", i);
            if (j == std::string::npos || o[j] == ']' || o[j] == ',' || o[j] == '.' || o[j] == '[') continue;
            r += ' ';
            continue;
        }
        r += o[i];
    }
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

struct Cursor {
    const std::string& s;
    size_t p = 0;
    bool end() const { return p >= s.size(); }
    char peek() const { return p < s.size() ? s[p] : '\0'; }
    void space() {
        while (p < s.size() && s[p] == ' ') ++p;
    }
    bool take(char c) {
        space();
        if (peek() != c) return false;
        ++p;
        return true;
    }
    bool takeWord(const char* w) {
        space();
        const size_t n = std::char_traits<char>::length(w);
        if (s.compare(p, n, w) != 0) return false;
        p += n;
        return true;
    }
    std::string letters() {
        space();
        std::string o;
        while (p < s.size() && std::isalpha(static_cast<unsigned char>(s[p]))) o += s[p++];
        return o;
    }
    bool number(uint32_t& v) {
        space();
        if (!std::isdigit(static_cast<unsigned char>(peek()))) return false;
        uint64_t n = 0;
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            n = n * 10 + static_cast<uint64_t>(s[p++] - '0');
            if (n > 0xFFFFFFFFULL) return false;
        }
        v = static_cast<uint32_t>(n);
        return true;
    }
};

// an area and size from its letters: MW, DBX, PIB, EB (German), ...
bool areaOf(const std::string& w, char& area, char& size) {
    static const struct {
        const char* w;
        char area, size;
    } t[] = {
        {"I", 'I', 'X'},   {"IB", 'I', 'B'},   {"IW", 'I', 'W'},   {"ID", 'I', 'D'},   {"E", 'I', 'X'},
        {"EB", 'I', 'B'},  {"EW", 'I', 'W'},   {"ED", 'I', 'D'},   {"Q", 'Q', 'X'},    {"QB", 'Q', 'B'},
        {"QW", 'Q', 'W'},  {"QD", 'Q', 'D'},   {"A", 'Q', 'X'},    {"AB", 'Q', 'B'},   {"AW", 'Q', 'W'},
        {"AD", 'Q', 'D'},  {"M", 'M', 'X'},    {"MB", 'M', 'B'},   {"MW", 'M', 'W'},   {"MD", 'M', 'D'},
        {"L", 'L', 'X'},   {"LB", 'L', 'B'},   {"LW", 'L', 'W'},   {"LD", 'L', 'D'},   {"PIB", 'P', 'B'},
        {"PIW", 'P', 'W'}, {"PID", 'P', 'D'},  {"PEB", 'P', 'B'},  {"PEW", 'P', 'W'},  {"PED", 'P', 'D'},
        {"PQB", 'O', 'B'}, {"PQW", 'O', 'W'},  {"PQD", 'O', 'D'},  {"PAB", 'O', 'B'},  {"PAW", 'O', 'W'},
        {"PAD", 'O', 'D'}, {"DBX", 'D', 'X'},  {"DBB", 'D', 'B'},  {"DBW", 'D', 'W'},  {"DBD", 'D', 'D'},
        {"DIX", 'N', 'X'}, {"DIB", 'N', 'B'},  {"DIW", 'N', 'W'},  {"DID", 'N', 'D'},
    };
    for (const auto& x : t)
        if (w == x.w) {
            area = x.area;
            size = x.size;
            return true;
        }
    return false;
}

// the area code of an area-crossing pointer
uint32_t areaCode(char area) {
    switch (area) {
        case 'P': case 'O': return 0x80;
        case 'I': return 0x81;
        case 'Q': return 0x82;
        case 'M': return 0x83;
        case 'D': return 0x84;
        case 'N': return 0x85;
        case 'L': return 0x86;
        default: return 0;
    }
}
char areaOfCode(uint32_t code) {
    switch (code) {
        case 0x80: return 'P';
        case 0x81: return 'I';
        case 0x82: return 'Q';
        case 0x83: return 'M';
        case 0x84: return 'D';
        case 0x85: return 'N';
        case 0x86: return 'L';
        default: return 0;
    }
}

int sizeBits(char size) { return size == 'X' ? 1 : size == 'B' ? 8 : size == 'W' ? 16 : 32; }

}  // namespace

// ---- operands ----

// Reads `text` as an absolute or indirect operand. `isAddress` false: it is
// not one (a name, a constant), and the operand is taken from the code tree.
bool Simulator::stlAddress(Frame& f, const std::string& text, Abs& a, bool& isAddress) {
    isAddress = false;
    const std::string s = squash(text);
    if (s.empty() || s[0] == '"' || s[0] == '#' || s[0] == '\'') return false;
    Cursor c{s};
    auto bad = [&](const std::string& why) {
        isAddress = true;
        warn("block " + f.block->name + ": the operand " + text + " is not read (" + why + ")");
        return false;
    };
    // a pointer offset P#b.x after AR1, or a pointer constant
    auto pointerBits = [&](uint32_t& bits, char& area) -> bool {
        area = 0;
        if (!c.takeWord("P#")) return false;
        const size_t save = c.p;
        std::string w = c.letters();
        if (w == "DB" && std::isdigit(static_cast<unsigned char>(c.peek()))) {
            // P#DB5.DBX0.0: the area-crossing part only
            uint32_t n = 0;
            c.number(n);
            if (!c.take('.')) return false;
            w = c.letters();
        }
        char size;
        if (!w.empty()) {
            if (w == "P" || w == "PE" || w == "PI") area = 'P';
            else if (w == "PA" || w == "PQ") area = 'O';
            else if (areaOf(w, area, size)) {
            } else {
                c.p = save;
                return false;
            }
        }
        uint32_t byte, bit = 0;
        if (!c.number(byte) || !c.take('.') || !c.number(bit) || bit > 7 || byte > 0xFFFF) return false;
        bits = (byte << 3) | bit;
        return true;
    };
    // the operand of an indirect address, read: [MD 10] or [AR1,P#2.0]
    // gives the address bits (and an area for an area-crossing register)
    auto indirect = [&](bool word, uint32_t& bits, char& area, bool& crossing) -> bool {
        area = 0;
        crossing = false;
        if (c.takeWord("AR1") || c.takeWord("AR2")) {
            const bool two = s[c.p - 1] == '2';
            const uint32_t ar = two ? ar2_ : ar1_;
            if (!c.take(',')) return false;
            uint32_t off;
            char none;
            if (!pointerBits(off, none)) return false;
            bits = ((ar & 0x7FFFF) + off) & 0x7FFFF;
            area = areaOfCode(ar >> 24);
            crossing = true;
            return c.take(']');
        }
        // memory-indirect: a word (T, C, DB numbers) or a double word (an
        // address), by address ([MD 10]) or by a name (["Ptr"], [#ptr]: TIA
        // Portal shows the tag of MD 10 there)
        c.space();
        if (c.peek() == '"' || c.peek() == '#') {
            const size_t open = text.find('['), close = text.rfind(']');
            if (open == std::string::npos || close == std::string::npos || close < open) return false;
            std::string name = text.substr(open + 1, close - open - 1);
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            Place pl;
            if (name[0] == '#') {
                Var* v = f.local ? f.local->member(name.substr(1)) : nullptr;
                if (!v && f.instance) v = f.instance->member(name.substr(1));
                if (v) pl.var = v;
            } else {
                std::string error;
                pl = operand(name, error);
            }
            if (!pl.var && !pl.area) return false;
            const Val v = load(pl);
            const uint32_t raw = static_cast<uint32_t>(asInt(v));
            bits = word ? (raw & 0xFFFF) : (raw & 0x7FFFF);
            // past the name
            const size_t end = c.s.find(']', c.p);
            if (end == std::string::npos) return false;
            c.p = end + 1;
            return true;
        }
        const std::string w = c.letters();
        char ia, isz;
        uint32_t byte, bit = 0;
        if (!areaOf(w, ia, isz) || isz == 'X' || !c.number(byte) || byte > 65535 || !c.take(']')) return false;
        if (word ? isz != 'W' : isz != 'D') return false;
        Abs in;
        in.area = ia;
        in.size = isz;
        in.byte = byte;
        in.bit = static_cast<int>(bit);
        bool ok = true;
        const uint32_t v = absRead(f, in, ok);
        if (!ok) return false;
        bits = word ? (v & 0xFFFF) : (v & 0x7FFFF);
        return true;
    };

    // P##temp: an area-crossing pointer to a temporary (a block with standard access)
    if (s.compare(0, 3, "P##") == 0) {
        isAddress = true;
        std::string name = text.substr(text.find("##") + 2);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if ((*f.block->json)["optimized"].type != Json::Bool || (*f.block->json)["optimized"].b) {
            warn("block " + f.block->name + ": P##" + name + " in a block with optimized access: not done");
            return false;
        }
        Var* v = f.local ? f.local->member(name) : nullptr;
        if (!v) {
            warn("block " + f.block->name + ": P##" + name + ": no temporary " + name);
            return false;
        }
        DbInfo& d = localLayout(f);
        auto it = d.starts.find(v);
        if (it == d.starts.end()) {
            warn("block " + f.block->name + ": P##" + name + ": not a temporary");
            return false;
        }
        a.isPointer = true;
        a.pointer = (0x86u << 24) | static_cast<uint32_t>(it->second & 0x7FFFF);
        return true;
    }
    // P#: a pointer as the value
    if (s.compare(0, 2, "P#") == 0) {
        uint32_t bits;
        char area;
        if (!pointerBits(bits, area) || !c.end()) return false;  // P#DB1.DBX0.0 BYTE 10 (an ANY) and the like
        isAddress = true;
        a.isPointer = true;
        a.pointer = (areaCode(area) << 24) | bits;
        return true;
    }
    // T 5, C 5 (Z 5), T [MW 10]; DB 5, DI 5 (OPN)
    {
        const size_t save = c.p;
        const std::string w = c.letters();
        const char kind = w == "T" ? 'T' : (w == "C" || w == "Z") ? 'C' : w == "DB" ? 'B' : w == "DI" ? 'J' : 0;
        c.space();
        if (kind && (std::isdigit(static_cast<unsigned char>(c.peek())) || c.peek() == '[')) {
            uint32_t n = 0;
            if (c.take('[')) {
                uint32_t bits;
                char area;
                bool crossing;
                if (!indirect(true, bits, area, crossing) || crossing) return bad("an indirect number");
                n = bits;
            } else if (!c.number(n) || n > 65535) {
                return bad("a number");
            }
            if (kind == 'B' && c.take('.')) {
                // DB5.DBW2
                c.p = save;
            } else {
                if (!c.end()) return bad("more after the number");
                isAddress = true;
                a.area = kind;
                a.number = static_cast<int>(n);
                return true;
            }
        }
        c.p = save;
    }
    // DB5.DBX0.0
    int db = -1;
    if (c.takeWord("DB") && std::isdigit(static_cast<unsigned char>(c.peek()))) {
        uint32_t n = 0;
        if (!c.number(n) || n > 65535 || !c.take('.')) return false;
        db = static_cast<int>(n);
    } else {
        c.p = 0;
    }
    std::string w = c.letters();
    char area = 0, size = 'X';
    bool crossingForm = false;
    if (w.empty() || w == "X" || w == "B" || w == "W" || w == "D") {
        c.space();
        if (c.peek() != '[') return false;
        crossingForm = true;
        size = w.empty() ? 'X' : w[0];
    } else if (!areaOf(w, area, size)) {
        return false;
    }
    if (db >= 0 && area != 'D') return false;
    a.size = size;
    a.db = db;
    if (c.take('[')) {
        uint32_t bits;
        char crossArea;
        bool crossing;
        if (!indirect(false, bits, crossArea, crossing)) return bad("an indirect address");
        if (crossingForm) {
            if (!crossing || !crossArea) return bad("an area-crossing address without an area in the register");
            area = crossArea;
        }
        a.byte = bits >> 3;
        a.bit = static_cast<int>(bits & 7);
        if (size != 'X' && a.bit != 0) {
            stop("STL: an address of a byte, word or double word with a bit (" + text + ") in block " + f.block->name);
            isAddress = true;
            return false;
        }
    } else {
        uint32_t byte, bit = 0;
        if (!c.number(byte)) return false;
        if (byte > 65535) return bad("an address beyond 65535");
        if (size == 'X') {
            if (!c.take('.') || !c.number(bit) || bit > 7) return false;
        }
        a.byte = byte;
        a.bit = static_cast<int>(bit);
    }
    if (!c.end()) return false;
    a.area = area;
    isAddress = true;
    return true;
}

// ---- data blocks with standard access, as bytes ----

namespace {

int64_t alignTo(int64_t bit, int64_t bytes) { return (bit + bytes * 8 - 1) / (bytes * 8) * (bytes * 8); }

// the size in bits of a leaf; 0 when not known
int64_t leafBits(const Var& v, const Json* type) {
    if (type && (*type)["kind"].str() == "parameter") {
        // the parameter types a temporary can have: an ANY takes 10 bytes, a
        // POINTER 6, a block or timer number a word
        const std::string& n = (*type)["name"].str();
        if (n == "Any" || n == "ANY") return 80;
        if (n == "Pointer" || n == "POINTER") return 48;
        return 16;
    }
    if (v.type) {
        if (v.type->kind == Kind::String) return (static_cast<int64_t>(v.maxLen) + 2) * 8;
        if (v.type->bits) return v.type->bits;
    }
    if (type && (*type)["bits"].type == Json::Number) return (*type)["bits"].asInt();
    return 0;
}

}  // namespace

// Lays out the members of a data block as the CPU does with standard access:
// a Bool takes a bit and Bools in a row share a byte; a Byte, Char, SInt or
// USInt takes the next byte; everything else starts on an even byte; a
// structure starts on an even byte and takes an even number of bytes; an
// array starts on an even byte, its Bools and bytes packed, other elements
// each on an even byte. Where tiaconv gives the offset of a member of the
// data block itself, that one is taken.
void Simulator::layOut(DbInfo& d) {
    d.laidOut = true;
    d.slots.clear();
    d.starts.clear();
    auto dbv = dbs_.find(d.name);
    if (dbv == dbs_.end()) return;
    layOutVars(d, *dbv->second, (*d.json)["members"], nullptr);
}

// The members of `root` (described by `members`) from bit 0 on; with
// `section` only the members of that section (Temp: the local data).
void Simulator::layOutVars(DbInfo& d, Var& root, const Json& members, const char* onlySection) {
    d.laidOut = true;
    std::vector<Slot>& slots = d.slots;
    std::map<const Var*, int64_t>& starts = d.starts;
    // a variable at `pos`; returns the bit after it
    std::function<int64_t(Var&, const Json*, int64_t)> place = [&](Var& v, const Json* m, int64_t pos) -> int64_t {
        const Json* type = m ? &(*m)["type"] : nullptr;
        if (type && (*type)["kind"].str() == "parameter") {
            // ANY, POINTER, a block number: space, no value S7Forge keeps
            pos = alignTo(pos, 2);
            starts[&v] = pos;
            return pos + leafBits(v, type);
        }
        if (v.shape == Var::Leaf) {
            const int64_t bits = leafBits(v, type);
            if (bits == 1) {
                starts[&v] = pos;
                slots.push_back({&v, pos, 1});
                return pos + 1;
            }
            if (bits == 8 && !(v.type && v.type->kind == Kind::String)) pos = alignTo(pos, 1);
            else pos = alignTo(pos, 2);
            starts[&v] = pos;
            if (bits > 0) slots.push_back({&v, pos, bits});
            return pos + std::max<int64_t>(bits, 8);
        }
        pos = alignTo(pos, 2);
        starts[&v] = pos;
        if (v.shape == Var::Struct) {
            const Json* members = m ? &(*m)["members"] : nullptr;
            for (auto& mv : v.members) {
                const Json* mj = nullptr;
                if (members)
                    for (const auto& x : members->items)
                        if (x["name"].str() == mv.first) mj = &x;
                pos = place(*mv.second, mj, pos);
            }
            return alignTo(pos, 2);
        }
        // an array: the elements one after the other
        Json element;
        if (m) {
            element.type = Json::Object;
            element.members.emplace_back("type", (*m)["type"]["element"]);
            element.members.emplace_back("members", (*m)["members"]);
        }
        for (auto& e : v.elems) {
            const bool small = e->shape == Var::Leaf && leafBits(*e, m ? &element["type"] : nullptr) <= 8 &&
                               !(e->type && e->type->kind == Kind::String);
            if (!small) pos = alignTo(pos, 2);
            pos = place(*e, m ? &element : nullptr, pos);
        }
        return alignTo(pos, 2);
    };
    starts[&root] = 0;
    int64_t pos = 0;
    std::string section;
    for (auto& mv : root.members) {
        const Json* mj = nullptr;
        for (const auto& x : members.items)
            if (x["name"].str() == mv.first) mj = &x;
        if (onlySection && (!mj || (*mj)["section"].str() != onlySection)) continue;
        // an instance: each section (Input, Output, InOut, Static) starts on a word
        if (mj && (*mj)["section"].str() != section) {
            if (!section.empty()) pos = alignTo(pos, 2);
            section = (*mj)["section"].str();
        }
        if (mj && (*mj)["offset_bits"].type == Json::Number) pos = (*mj)["offset_bits"].asInt();
        pos = place(*mv.second, mj, pos);
    }
    d.bytes = alignTo(pos, 2) / 8;
    std::sort(slots.begin(), slots.end(), [](const Slot& x, const Slot& y) { return x.bit < y.bit; });
}

namespace {

// a leaf's value as the bytes it takes, `buf` from its first byte
void encode(const Var& v, int64_t bits, std::vector<uint8_t>& buf) {
    buf.assign(static_cast<size_t>((bits + 7) / 8), 0);
    if (!v.type) return;
    if (v.type->kind == Kind::String) {
        buf[0] = static_cast<uint8_t>(std::min(v.maxLen, 254));
        const size_t n = std::min(v.value.s.size(), buf.size() - 2);
        buf[1] = static_cast<uint8_t>(n);
        for (size_t k = 0; k < n; ++k) buf[2 + k] = static_cast<uint8_t>(v.value.s[k]);
        return;
    }
    if (bits == 1) {
        buf[0] = truthy(v.value) ? 1 : 0;
        return;
    }
    Area tmp('x', buf.size());
    Address at;
    at.area = 'M';
    writeArea(tmp, at, *v.type, v.value);
    buf = tmp.bytes;
}

void decode(Var& v, int64_t bits, const std::vector<uint8_t>& buf) {
    if (!v.type) return;
    if (v.type->kind == Kind::String) {
        const size_t n = std::min<size_t>(buf[1], buf.size() - 2);
        v.value = convertTo(stringVal(std::string(buf.begin() + 2, buf.begin() + 2 + static_cast<long>(n))), *v.type, v.maxLen);
        return;
    }
    if (bits == 1) {
        v.value = boolVal(buf[0] & 1);
        return;
    }
    Area tmp('x', buf.size());
    tmp.bytes = buf;
    Address at;
    at.area = 'M';
    v.value = readArea(tmp, at, *v.type);
}

}  // namespace

bool Simulator::dbRead(DbInfo& d, uint32_t byte, int n, uint8_t* out) {
    if (!d.laidOut) layOut(d);
    if (static_cast<int64_t>(byte) + n > d.bytes) return false;
    // what is not a variable (padding, the bits next to a Bool) is kept as written
    if (static_cast<int64_t>(d.image.size()) < d.bytes) d.image.resize(static_cast<size_t>(d.bytes), 0);
    std::copy(d.image.begin() + byte, d.image.begin() + byte + n, out);
    const int64_t from = static_cast<int64_t>(byte) * 8, to = from + n * 8;
    auto it = std::lower_bound(d.slots.begin(), d.slots.end(), from - 8 * 300,
                               [](const Slot& s, int64_t b) { return s.bit < b; });
    std::vector<uint8_t> buf;
    for (; it != d.slots.end() && it->bit < to; ++it) {
        if (it->bit + it->bits <= from) continue;
        encode(*it->var, it->bits, buf);
        if (it->bits == 1) {
            const int64_t b = it->bit / 8 - byte;
            const uint8_t mask = static_cast<uint8_t>(1u << (it->bit % 8));
            if (b >= 0 && b < n) out[b] = static_cast<uint8_t>((buf[0] & 1) ? (out[b] | mask) : (out[b] & ~mask));
            continue;
        }
        const int64_t first = it->bit / 8;
        for (size_t k = 0; k < buf.size(); ++k) {
            const int64_t b = first + static_cast<int64_t>(k) - byte;
            if (b >= 0 && b < n) out[b] = buf[k];
        }
    }
    return true;
}

bool Simulator::dbWrite(DbInfo& d, uint32_t byte, int n, const uint8_t* in) {
    if (!d.laidOut) layOut(d);
    if (static_cast<int64_t>(byte) + n > d.bytes) return false;
    if (static_cast<int64_t>(d.image.size()) < d.bytes) d.image.resize(static_cast<size_t>(d.bytes), 0);
    std::copy(in, in + n, d.image.begin() + byte);
    const int64_t from = static_cast<int64_t>(byte) * 8, to = from + n * 8;
    auto it = std::lower_bound(d.slots.begin(), d.slots.end(), from - 8 * 300,
                               [](const Slot& s, int64_t b) { return s.bit < b; });
    std::vector<uint8_t> buf;
    for (; it != d.slots.end() && it->bit < to; ++it) {
        if (it->bit + it->bits <= from) continue;
        if (it->bits == 1) {
            const int64_t b = it->bit / 8 - byte;
            if (b >= 0 && b < n) it->var->value = boolVal((in[b] >> (it->bit % 8)) & 1);
            continue;
        }
        encode(*it->var, it->bits, buf);
        const int64_t first = it->bit / 8;
        for (size_t k = 0; k < buf.size(); ++k) {
            const int64_t b = first + static_cast<int64_t>(k) - byte;
            if (b >= 0 && b < n) buf[k] = in[b];
        }
        decode(*it->var, it->bits, buf);
    }
    return true;
}

Simulator::DbInfo& Simulator::localLayout(Frame& f) {
    if (!f.lData) {
        f.lData = std::make_unique<DbInfo>();
        f.lData->name = "the local data of " + f.block->name;
        f.lData->optimized = false;
        if (f.local && f.block->interface) layOutVars(*f.lData, *f.local, *f.block->interface, "Temp");
        std::sort(f.lData->slots.begin(), f.lData->slots.end(), [](const Slot& x, const Slot& y) { return x.bit < y.bit; });
    }
    return *f.lData;
}

// The local data of the block running, by address: its temporaries laid
// out from L 0 as a data block is; the bytes after them are local data with
// no name, kept per call. Either `out` (read) or `in` (write).
bool Simulator::localBytes(Frame& f, uint32_t byte, int n, uint8_t* out, const uint8_t* in) {
    if ((*f.block->json)["optimized"].type != Json::Bool || (*f.block->json)["optimized"].b) {
        warn("block " + f.block->name + ": local data by address in a block with optimized access: not done");
        return false;
    }
    if (static_cast<uint64_t>(byte) + static_cast<uint64_t>(n) > 65536) {
        stop("STL: an address beyond the local data (L " + std::to_string(byte) + ") in block " + f.block->name);
        return false;
    }
    DbInfo& d = localLayout(f);
    // the declared part, then the rest
    const int named = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(d.bytes - byte, n)));
    if (named > 0) {
        if (out) dbRead(d, byte, named, out);
        else dbWrite(d, byte, named, in);
    }
    if (named < n) {
        if (f.lRaw.size() < 65536) f.lRaw.assign(65536, 0);
        for (int k = named; k < n; ++k) {
            if (out) out[k] = f.lRaw[byte + static_cast<uint32_t>(k)];
            else f.lRaw[byte + static_cast<uint32_t>(k)] = in[k];
        }
    }
    return true;
}

int64_t Simulator::bitOffset(const std::string& name) {
    std::string err;
    Place p = operand(name, err);
    if (!p.var) return -1;
    int db = 0;
    return bitOfVar(p.var, db);
}

Simulator::DbInfo* Simulator::dbOf(Frame& f, char area, int db) {
    const int n = db >= 0 ? db : area == 'N' ? curDi_ : curDb_;
    if (n <= 0) {
        stop("STL: no data block open for an access to " + std::string(area == 'N' ? "DI" : "DB") + " in block " + f.block->name);
        return nullptr;
    }
    auto it = dbInfo_.find(n);
    if (it == dbInfo_.end()) {
        stop("STL: the data block DB" + std::to_string(n) + " is not in the program (block " + f.block->name + ")");
        return nullptr;
    }
    if (it->second.optimized) {
        warn("block " + f.block->name + ": an absolute access to " + it->second.name +
             ", which has optimized access: not done");
        return nullptr;
    }
    return &it->second;
}

int64_t Simulator::bitOfVar(const Var* v, int& db) {
    for (auto& d : dbInfo_) {
        if (d.second.optimized) continue;
        if (!d.second.laidOut) layOut(d.second);
        auto it = d.second.starts.find(v);
        if (it != d.second.starts.end()) {
            db = d.first;
            return it->second;
        }
    }
    return -1;
}

uint32_t Simulator::absRead(Frame& f, const Abs& a, bool& ok) {
    ok = true;
    if (a.isPointer) return a.pointer;
    const int n = sizeBits(a.size) == 1 ? 1 : sizeBits(a.size) / 8;
    uint8_t b[4] = {0, 0, 0, 0};
    switch (a.area) {
        case 'I': case 'P': case 'Q': case 'O': case 'M': {
            Area& ar = a.area == 'I' || a.area == 'P' ? i_ : a.area == 'M' ? m_ : q_;
            if (static_cast<uint64_t>(a.byte) + static_cast<uint64_t>(n) > ar.bytes.size()) {
                stop("STL: an address beyond the area (" + std::string(1, a.area) + std::to_string(a.byte) + ")");
                ok = false;
                return 0;
            }
            for (int k = 0; k < n; ++k) b[k] = ar.bytes[a.byte + static_cast<uint32_t>(k)];
            break;
        }
        case 'D': case 'N': {
            DbInfo* d = dbOf(f, a.area, a.db);
            if (!d) {
                ok = false;
                return 0;
            }
            if (!dbRead(*d, a.byte, n, b)) {
                stop("STL: an access beyond the end of " + d->name + " (byte " + std::to_string(a.byte) + ", " +
                     std::to_string(d->bytes) + " bytes) in block " + f.block->name);
                ok = false;
                return 0;
            }
            break;
        }
        case 'L':
            if (!localBytes(f, a.byte, n, b, nullptr)) {
                ok = false;
                return 0;
            }
            break;
        default:
            ok = false;
            return 0;
    }
    if (a.size == 'X') return (b[0] >> a.bit) & 1;
    uint32_t v = 0;
    for (int k = 0; k < n; ++k) v = (v << 8) | b[k];
    return v;
}

void Simulator::absWrite(Frame& f, const Abs& a, uint32_t raw) {
    const int n = a.size == 'X' ? 1 : sizeBits(a.size) / 8;
    uint8_t b[4] = {0, 0, 0, 0};
    if (a.size == 'X') {
        Abs whole = a;
        whole.size = 'B';
        bool ok = true;
        const uint32_t old = absRead(f, whole, ok);
        if (!ok) return;
        b[0] = static_cast<uint8_t>((old & ~(1u << a.bit)) | ((raw & 1) << a.bit));
    } else {
        for (int k = n - 1; k >= 0; --k) {
            b[k] = static_cast<uint8_t>(raw & 0xFF);
            raw >>= 8;
        }
    }
    switch (a.area) {
        case 'I': case 'Q': case 'O': case 'P': case 'M': {
            // a write to the periphery (P, by an area-crossing pointer) goes to the outputs
            Area& ar = a.area == 'I' ? i_ : a.area == 'M' ? m_ : q_;
            if (static_cast<uint64_t>(a.byte) + static_cast<uint64_t>(n) > ar.bytes.size()) {
                stop("STL: an address beyond the area (" + std::string(1, a.area) + std::to_string(a.byte) + ")");
                return;
            }
            for (int k = 0; k < n; ++k) ar.bytes[a.byte + static_cast<uint32_t>(k)] = b[k];
            return;
        }
        case 'D': case 'N': {
            DbInfo* d = dbOf(f, a.area, a.db);
            if (!d) return;
            if (!dbWrite(*d, a.byte, n, b))
                stop("STL: an access beyond the end of " + d->name + " (byte " + std::to_string(a.byte) + ") in block " +
                     f.block->name);
            return;
        }
        case 'L':
            localBytes(f, a.byte, n, nullptr, b);
            return;
        default:
            return;
    }
}

}  // namespace s7forge
