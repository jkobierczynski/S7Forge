// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// S5 timers and counters: what STL (SP, SE, SD, SS, SF, FR, R; CU, CD, S,
// R, FR) and LAD/FBD (the boxes S_PULSE ... S_OFFDT, S_CU, S_CD, S_CUD and
// the coils SP ... SF, CU, CD, SC) do with them. A timer runs on the virtual
// time and is brought up to date whenever it is used.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "sim.hpp"

namespace s7forge {
namespace {

std::string key(const std::string& s) {
    // upper case, without '_' and blanks: S_ODT, s_odt, SdCoil -> SODT, SDCOIL
    std::string o;
    for (char c : s)
        if (c != '_' && c != ' ') o += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

uint32_t bcd3(int64_t n) {
    uint32_t v = 0;
    for (int k = 0; k < 3; ++k) {
        v |= static_cast<uint32_t>(n % 10) << (4 * k);
        n /= 10;
    }
    return v;
}

bool isBcd(uint32_t v) { return (v & 0xF) <= 9 && ((v >> 4) & 0xF) <= 9 && ((v >> 8) & 0xF) <= 9; }

// what a box or coil is: a timer of a mode (P E D S F), a counter, a coil
struct S5Kind {
    char what = 0;  // T: timer, C: counter
    char mode = 0;  // a timer: P E D S F; a counter: U (S_CU), D (S_CD), B (S_CUD), u (CU coil), d (CD coil), s (SC coil)
    bool coil = false;
};

bool s5Kind(const std::string& name, S5Kind& k) {
    static const std::map<std::string, S5Kind> m = {
        {"SPULSE", {'T', 'P', false}},  {"SPEXT", {'T', 'E', false}},  {"SODT", {'T', 'D', false}},
        {"SODTS", {'T', 'S', false}},   {"SOFFDT", {'T', 'F', false}}, {"SCU", {'C', 'U', false}},
        {"SCD", {'C', 'D', false}},     {"SCUD", {'C', 'B', false}},
        // the names the project stores for the boxes (S_ODT is Sd, S_CUD Cud)
        {"SP", {'T', 'P', false}},      {"SE", {'T', 'E', false}},     {"SD", {'T', 'D', false}},
        {"SS", {'T', 'S', false}},      {"SF", {'T', 'F', false}},     {"CU", {'C', 'U', false}},
        {"CD", {'C', 'D', false}},      {"CUD", {'C', 'B', false}},
        // German
        {"SIMPULS", {'T', 'P', false}}, {"SVIMP", {'T', 'E', false}},  {"SEVERZ", {'T', 'D', false}},
        {"SSEVERZ", {'T', 'S', false}}, {"SAVERZ", {'T', 'F', false}}, {"ZVORW", {'C', 'U', false}},
        {"ZRUECK", {'C', 'D', false}},  {"ZAEHLER", {'C', 'B', false}},
        // the coils
        {"SPCOIL", {'T', 'P', true}},   {"SECOIL", {'T', 'E', true}},  {"SDCOIL", {'T', 'D', true}},
        {"SSCOIL", {'T', 'S', true}},   {"SFCOIL", {'T', 'F', true}},  {"CUCOIL", {'C', 'u', true}},
        {"CDCOIL", {'C', 'd', true}},   {"SCCOIL", {'C', 's', true}},
    };
    auto it = m.find(key(name));
    if (it == m.end()) return false;
    k = it->second;
    return true;
}

}  // namespace

// ---- timers ----

void Simulator::s5Update(S5Timer& t) {
    if (!t.running || now_ - t.start < t.presetNs) return;
    t.running = false;
    t.frozenUnits = 0;
    switch (t.mode) {
        case 'D': t.q = t.startRlo; break;  // on while the start is on
        case 'S': t.q = true; break;        // on until reset
        default: t.q = false; break;        // a pulse, an off delay: over
    }
}

// the time value: the units of the time base left
int64_t Simulator::s5Units(S5Timer& t) {
    s5Update(t);
    if (!t.running) return t.frozenUnits;
    static const int64_t base[4] = {10000000, 100000000, 1000000000, 10000000000LL};
    const int64_t b = base[t.base & 3];
    const int64_t left = (t.presetNs - (now_ - t.start) + b - 1) / b;
    return std::max<int64_t>(0, std::min<int64_t>(999, left));
}

bool Simulator::s5TimerStart(int n, char mode, bool rlo, uint16_t tv, const std::string& who) {
    S5Timer& t = t5_[static_cast<size_t>(n)];
    s5Update(t);
    const bool rising = rlo && !t.lastStart, falling = !rlo && t.lastStart;
    t.lastStart = rlo;
    t.startRlo = rlo;
    bool ok = true;
    auto start = [&] {
        if (!isBcd(tv)) {
            stop(who + ": not an S5 time (16#" + [&] {
                char b[8];
                std::snprintf(b, sizeof b, "%04X", tv);
                return std::string(b);
            }() + ")");
            ok = false;
            return;
        }
        t.running = true;
        t.mode = mode;
        t.start = now_;
        t.presetNs = s5timeNs(tv);
        t.base = (tv >> 12) & 3;
    };
    if (mode == 'F') {
        if (rlo) {
            t.running = false;
            t.q = true;
            t.mode = 'F';
        } else if (falling) {
            start();
            t.q = true;
        }
    } else {
        if (rising) start();
        if (mode == 'P' || mode == 'D') {
            if (!rlo && t.running) {
                t.frozenUnits = s5Units(t);
                t.running = false;
            }
            if (!rlo) t.q = false;
        }
        if (mode == 'P') t.q = t.running && rlo;
        if (mode == 'E') t.q = t.running;
    }
    s5Update(t);
    return ok;
}

void Simulator::s5TimerFr(int n, bool rlo) {
    // a rising edge of FR lets the next start instruction start again
    S5Timer& t = t5_[static_cast<size_t>(n)];
    if (rlo && !t.lastFr) t.lastStart = false;
    t.lastFr = rlo;
}

void Simulator::s5TimerReset(int n) {
    S5Timer& t = t5_[static_cast<size_t>(n)];
    t.running = false;
    t.q = false;
    t.frozenUnits = 0;
    t.base = 0;
}

// ---- counters ----

void Simulator::s5CounterCount(int n, bool up, bool rlo) {
    S5Counter& k = c5_[static_cast<size_t>(n)];
    bool& last = up ? k.lastCu : k.lastCd;
    if (rlo && !last) k.value = up ? std::min(999, k.value + 1) : std::max(0, k.value - 1);
    last = rlo;
}

bool Simulator::s5CounterSet(int n, bool rlo, uint16_t bcd, const std::string& who) {
    S5Counter& k = c5_[static_cast<size_t>(n)];
    const bool rising = rlo && !k.lastS;
    k.lastS = rlo;
    if (!rising) return true;
    const uint32_t v = bcd & 0xFFF;
    if (!isBcd(v)) {
        stop(who + ": not a BCD count");
        return false;
    }
    k.value = static_cast<int>(((v >> 8) & 0xF) * 100 + ((v >> 4) & 0xF) * 10 + (v & 0xF));
    return true;
}

void Simulator::s5CounterReset(int n) { c5_[static_cast<size_t>(n)].value = 0; }

void Simulator::s5CounterFr(int n, bool rlo) {
    S5Counter& k = c5_[static_cast<size_t>(n)];
    if (rlo && !k.lastFr) k.lastCu = k.lastCd = k.lastS = false;
    k.lastFr = rlo;
}

// An S5TIME in I, Q or M: the bits there, with their own time base (a
// variable of a data block keeps its time, in the finest time base)
bool Simulator::s5Bits(Frame& f, const Json& e, uint16_t& bits) {
    const std::string& op = e["op"].str();
    if (op != "ref" && op != "member" && op != "index") return false;
    const Json* r = ref(f, e);
    if (r) {
        Val c;
        if (constantOf(*r, c)) return false;
    }
    Place p = placeOf(f, e);
    if (!p.area || !isS5Time(p.type) || p.sliceBits) return false;
    Address at = p.at;
    at.size = 'W';
    bits = static_cast<uint16_t>(readArea(*p.area, at, *elemType("Word")).i & 0xFFFF);
    return true;
}

// ---- LAD and FBD ----

// A reference that names a timer or counter: a tag of the type Timer or
// Counter, or an address (%T5, T 5)
bool Simulator::s5Ref(Frame& f, const Json& r, char& kind, int& n) {
    const std::string& k = r["kind"].str();
    std::string name = r["name"].str().empty() ? r["text"].str() : r["name"].str();
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
    if (k == "tag") {
        auto t = s5Tags_.find(name);
        if (t != s5Tags_.end()) {
            kind = t->second.first;
            n = t->second.second;
            return true;
        }
    }
    // an address: %T5, T 5, %C5, C 5, Z 5
    const std::string& text = r["text"].str();
    size_t p = text.empty() ? 0 : (text[0] == '%' ? 1 : 0);
    if (p >= text.size()) return false;
    const char c0 = static_cast<char>(std::toupper(static_cast<unsigned char>(text[p])));
    if (c0 != 'T' && c0 != 'C' && c0 != 'Z') return false;
    ++p;
    while (p < text.size() && text[p] == ' ') ++p;
    if (p >= text.size() || !std::isdigit(static_cast<unsigned char>(text[p]))) return false;
    Abs a;
    bool isAddress = false;
    if (!stlAddress(f, text, a, isAddress) || (a.area != 'T' && a.area != 'C')) return false;
    kind = a.area;
    n = a.number;
    return true;
}

bool Simulator::s5Operand(Frame& f, const Json& e, char want, int& n) {
    char kind = 0;
    const Json* r = ref(f, e);
    bool found = false;
    if (r) found = s5Ref(f, *r, kind, n);
    if (!found && e["op"].str() == "const") {
        Abs a;
        bool isAddress = false;
        const std::string text = e["value"].type == Json::Object ? e["value"]["text"].str() : e["name"].str();
        if (stlAddress(f, text, a, isAddress) && (a.area == 'T' || a.area == 'C')) {
            kind = a.area;
            n = a.number;
            found = true;
        }
    }
    if (!found || kind != want) {
        warn("block " + f.block->name + ": " + e["name"].str() + " is not a" + (want == 'T' ? " timer" : " counter"));
        return false;
    }
    if (n < 0 || n >= 2048) {
        stop(std::string(1, want) + " " + std::to_string(n) + " does not exist (block " + f.block->name + ")");
        return false;
    }
    return true;
}

bool Simulator::s5Status(Frame& f, const Json& r, bool& value) {
    char kind = 0;
    int n = 0;
    if (!s5Ref(f, r, kind, n)) return false;
    if (n < 0 || n >= 2048) {
        stop(std::string(1, kind) + " " + std::to_string(n) + " does not exist (block " + f.block->name + ")");
        value = false;
        return true;
    }
    if (kind == 'C') {
        value = c5_[static_cast<size_t>(n)].value != 0;
    } else {
        S5Timer& t = t5_[static_cast<size_t>(n)];
        s5Update(t);
        value = t.q;
    }
    return true;
}

bool Simulator::s5Reset(Frame& f, const Json& r) {
    char kind = 0;
    int n = 0;
    if (!s5Ref(f, r, kind, n)) return false;
    if (n < 0 || n >= 2048) {
        stop(std::string(1, kind) + " " + std::to_string(n) + " does not exist (block " + f.block->name + ")");
        return true;
    }
    if (kind == 'T') s5TimerReset(n);
    else s5CounterReset(n);
    return true;
}

// A box or coil of an S5 timer or counter; false when `e` is not one.
bool Simulator::s5Call(Frame& f, const Json& e, const std::string& name) {
    S5Kind k;
    if (!s5Kind(name, k)) return false;
    const std::string who = (k.coil ? "the coil " : "the box ") + name + " in block " + f.block->name;
    // the parameters by their names, as given
    std::map<std::string, const Json*> in, out;
    for (const auto& a : e["args"].items) {
        if (a["args"].items.empty()) continue;
        (a["op"].str() == "out" ? out : in)[key(a["name"].str())] = &a["args"].items[0];
    }
    auto find = [&](std::map<std::string, const Json*>& m, std::initializer_list<const char*> names) -> const Json* {
        for (const char* n : names) {
            auto it = m.find(n);
            if (it != m.end()) return it->second;
        }
        return nullptr;
    };
    auto boolOf = [&](std::initializer_list<const char*> names) -> bool {
        const Json* x = find(in, names);
        return x && truthy(eval(f, *x));
    };
    // a time value: an S5TIME (a constant, a Time) or a word with its bits
    auto timeValue = [&](std::initializer_list<const char*> names) -> uint16_t {
        const Json* x = find(in, names);
        if (!x) return 0;
        uint16_t raw;
        if (s5Bits(f, *x, raw)) return raw;
        const Val v = eval(f, *x);
        if (v.kind == Kind::Time) return s5timeBits(v.i);
        return static_cast<uint16_t>(asInt(v) & 0xFFFF);
    };
    // a count: C#5, or a word in BCD
    auto countValue = [&](std::initializer_list<const char*> names) -> uint16_t {
        const Json* x = find(in, names);
        if (!x) return 0;
        std::string text = (*x)["value"].type == Json::Object ? (*x)["value"]["text"].str() : std::string();
        if (text.empty() && (*x)["op"].str() == "const") text = (*x)["name"].str();
        if (text.size() > 2 && (text[0] == 'C' || text[0] == 'c') && text[1] == '#') {
            const long c = std::strtol(text.c_str() + 2, nullptr, 10);
            if (c < 0 || c > 999) warn(who + ": the count " + text + " is not 0 to 999");
            return static_cast<uint16_t>(bcd3(std::max(0L, std::min(999L, c))));
        }
        return static_cast<uint16_t>(asInt(eval(f, *x)) & 0xFFFF);
    };
    auto put = [&](std::initializer_list<const char*> names, const Val& v) {
        const Json* x = find(out, names);
        if (!x) return;
        if ((*x)["op"].str() == "wire") storeWire(f, (*x)["name"].str(), v);
        else store(placeOf(f, *x), v);
    };
    const Json* opd = find(in, {"TNO", "TNR", "T", "TIMER", "CNO", "ZNR", "C", "Z", "COUNTER", "OPERAND"});
    if (!opd) {
        warn(who + ": without its timer or counter");
        return true;
    }
    int n = 0;
    if (!s5Operand(f, *opd, k.what, n)) return true;
    // an input that is not connected does nothing (its edge memory stays)
    auto has = [&](std::initializer_list<const char*> names) { return find(in, names) != nullptr; };
    if (k.what == 'T') {
        if (has({"S", "IN"})) {
            if (!has({"TV", "TW", "VALUE"})) warn(who + ": without a time value: 0");
            if (!s5TimerStart(n, k.mode, boolOf({"S", "IN"}), timeValue({"TV", "TW", "VALUE"}), who)) return true;
        }
        if (!k.coil && boolOf({"R"})) s5TimerReset(n);
        if (!k.coil) {
            S5Timer& t = t5_[static_cast<size_t>(n)];
            const int64_t units = s5Units(t);
            put({"BI", "RT", "DUAL"}, intVal(units, elemType("Word")));
            put({"BCD", "RTBCD", "DEZ"}, intVal((static_cast<int64_t>(t.base) << 12) | bcd3(units), elemType("Word")));
            put({"Q"}, boolVal(t.q));
        }
        return true;
    }
    // counters
    switch (k.mode) {
        case 'u': s5CounterCount(n, true, boolOf({"IN"})); return true;
        case 'd': s5CounterCount(n, false, boolOf({"IN"})); return true;
        case 's': s5CounterSet(n, boolOf({"IN"}), countValue({"VALUE", "PV", "ZW"}), who); return true;
        default: break;
    }
    if ((k.mode == 'U' || k.mode == 'B') && has({"CU", "ZV"})) s5CounterCount(n, true, boolOf({"CU", "ZV"}));
    if ((k.mode == 'D' || k.mode == 'B') && has({"CD", "ZR"})) s5CounterCount(n, false, boolOf({"CD", "ZR"}));
    if (has({"S"}) && !s5CounterSet(n, boolOf({"S"}), countValue({"PV", "ZW"}), who)) return true;
    if (boolOf({"R"})) s5CounterReset(n);
    const int v = c5_[static_cast<size_t>(n)].value;
    put({"CV", "DUAL"}, intVal(v, elemType("Word")));
    put({"CVBCD", "DEZ"}, intVal(bcd3(v), elemType("Word")));
    put({"Q"}, boolVal(v != 0));
    return true;
}

}  // namespace s7forge
