// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// STL (statement list), as an S7-1500 CPU runs it: the result of logic
// operation with the first check and the nesting stack, the status bits
// BR, OV, OS, CC0 and CC1, and four 32-bit accumulators. tiaconv exports
// each STL statement as {"op": "stl", "name": the instruction, "exprs": its
// operands}, a label as {"op": "label"}. Mnemonics in German (U, SPB, ...)
// are read as their English ones.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>

#include "scenario.hpp"
#include "sim.hpp"

namespace s7forge {
namespace {

constexpr uint64_t kMaxSteps = 2000000;  // statements in one cycle; as in sim.cpp

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// German mnemonics: those that differ from the English ones
const std::map<std::string, std::string>& german() {
    static const std::map<std::string, std::string> m = {
        {"U", "A"},       {"UN", "AN"},     {"U(", "A("},     {"UN(", "AN("},  {"SPA", "JU"},    {"SPB", "JC"},
        {"SPBN", "JCN"},  {"SPBB", "JCB"},  {"SPBNB", "JNB"}, {"SPBI", "JBI"}, {"SPBIN", "JNBI"}, {"SPO", "JO"},
        {"SPS", "JOS"},   {"SPZ", "JZ"},    {"SPN", "JN"},    {"SPP", "JP"},   {"SPM", "JM"},    {"SPPZ", "JPZ"},
        {"SPMZ", "JMZ"},  {"SPU", "JUO"},   {"SPL", "JL"},    {"BEA", "BEU"},  {"BEB", "BEC"},   {"UW", "AW"},
        {"UD", "AD"},     {"AUF", "OPN"},   {"ZV", "CU"},     {"ZR", "CD"},    {"LAR1", "LAR1"},
    };
    return m;
}

// the S5 timers: the German names, of which SE means SD in English
const std::map<std::string, std::string>& germanTimers() {
    static const std::map<std::string, std::string> m = {
        {"SI", "SP"}, {"SV", "SE"}, {"SE", "SD"}, {"SA", "SF"},
    };
    return m;
}

// German only: a block that uses one of these is in German
bool germanOnly(const std::string& n) {
    static const char* const only[] = {"U",   "UN",   "U(",    "UN(",  "SPA",  "SPB", "SPBN", "SPBB", "SPBNB",
                                       "SPBI", "SPBIN", "SPO", "SPS",  "SPZ",  "SPN", "SPP",  "SPM",  "SPPZ",
                                       "SPMZ", "SPU",  "SPL",   "BEA",  "BEB",  "UW",  "UD",   "AUF",  "ZV",
                                       "ZR",   "SI",   "SV",    "SA"};
    for (const char* o : only)
        if (n == o) return true;
    return false;
}

// an operand of a German area: E 1.0, AW 4, PEW 256, Z 3
bool germanOperand(const std::string& text) {
    std::string t = upper(text);
    if (!t.empty() && t[0] == '%') t = t.substr(1);
    size_t n = 0;
    while (n < t.size() && std::isalpha(static_cast<unsigned char>(t[n]))) ++n;
    const std::string w = t.substr(0, n);
    static const char* const areas[] = {"E", "EB", "EW", "ED", "A", "AB", "AW", "AD", "PEB", "PEW", "PED", "PAB", "PAW", "PAD", "Z"};
    bool area = false;
    for (const char* x : areas) area = area || w == x;
    if (!area) return false;
    const size_t k = t.find_first_not_of(' ', n);
    return k != std::string::npos && (std::isdigit(static_cast<unsigned char>(t[k])) || t[k] == '[');
}

std::string mnemonic(const std::string& name, bool inGerman) {
    std::string n = upper(name);
    while (!n.empty() && n.back() == ' ') n.pop_back();
    if (inGerman) {
        auto t = germanTimers().find(n);
        if (t != germanTimers().end()) return t->second;
    }
    auto it = german().find(n);
    return it == german().end() ? n : it->second;
}

uint32_t bcdOf(int64_t n) {
    uint32_t v = 0;
    for (int k = 0; k < 3; ++k) {
        v |= static_cast<uint32_t>(n % 10) << (4 * k);
        n /= 10;
    }
    return v;
}

int16_t low16(uint32_t a) { return static_cast<int16_t>(static_cast<uint16_t>(a & 0xFFFF)); }
int32_t s32(uint32_t a) { return static_cast<int32_t>(a); }
float f32(uint32_t a) {
    float f;
    std::memcpy(&f, &a, 4);
    return f;
}
uint32_t bitsOf(float f) {
    uint32_t a;
    std::memcpy(&a, &f, 4);
    return a;
}

// BCD of the low 12 bits plus sign (ITB, BTI) and of 28 bits (DTB, BTD)
bool fromBcd(uint32_t v, int digits, int64_t& out) {
    int64_t n = 0;
    for (int k = digits - 1; k >= 0; --k) {
        const uint32_t d = (v >> (4 * k)) & 0xF;
        if (d > 9) return false;
        n = n * 10 + d;
    }
    const uint32_t sign = (v >> (4 * digits)) & 0xF;
    out = sign ? -n : n;
    return true;
}
uint32_t toBcd(int64_t n, int digits, bool& ov) {
    const bool neg = n < 0;
    uint64_t m = static_cast<uint64_t>(neg ? -n : n);
    uint32_t v = 0;
    for (int k = 0; k < digits; ++k) {
        v |= static_cast<uint32_t>(m % 10) << (4 * k);
        m /= 10;
    }
    ov = m != 0;
    if (neg) v |= 0xFu << (4 * digits);  // the sign: 1111
    return v;
}

}  // namespace

// ---- the code of a block ----

std::shared_ptr<Simulator::StlCode> Simulator::stlCode(const Json& networks) {
    auto c = std::make_shared<StlCode>();
    bool any = false;
    for (size_t k = 0; k < networks.items.size(); ++k) {
        const Json& st = networks.items[k]["statements"];
        bool stl = false;
        for (const auto& s : st.items)
            if (s["op"].str() == "stl" || s["op"].str() == "label") stl = true;
        if (!stl) {
            c->first.push_back(StlCode::npos());
            continue;
        }
        any = true;
        c->first.push_back(c->code.size());
        for (const auto& s : st.items) {
            if (s["op"].str() == "stl" && (germanOnly(upper(s["name"].str())) || germanOperand(s["text"].str())))
                c->german = true;
            if (s["op"].str() == "label") c->labels.emplace(upper(s["name"].str()), c->code.size());
            c->code.push_back(&s);
            c->net.push_back(k);
        }
    }
    return any ? c : nullptr;
}

size_t Simulator::runStl(Frame& f, size_t net, size_t pc) {
    if (!f.stl) f.stl = std::make_unique<StlState>();
    const StlCode& c = *f.block->stl;
    size_t cur = net;
    while (pc < c.code.size() && running_ && !f.returned) {
        if (c.net[pc] != cur) return cur + 1;  // the end of the network
        const size_t next = stlStep(f, *f.stl, pc);
        if (next != pc + 1 && next < c.code.size()) cur = c.net[next];  // a jump
        pc = next;
    }
    return cur + 1;
}

// ---- operands ----

// The value of an operand as the bits it has in an accumulator: a number of
// 16 bits in the low word (a constant of a signed type sign-extended), a Real
// as its IEEE bits, a Time in ms.
uint32_t Simulator::stlLoad(Frame& f, const Json& e, bool& ok) {
    ok = true;
    Val v;
    const ElemType* t = nullptr;
    bool constant = false;
    if (e["op"].str() == "const") {
        constant = true;
        t = elemType(e["type"].str());
        if (e["value"].type == Json::Object) v = fromExport(e["value"], t);
        if (v.kind == Kind::None) {
            const std::string text = e["value"].type == Json::Object ? e["value"]["text"].str() : e["name"].str();
            const std::string u = upper(text);
            if (u.compare(0, 4, "S5T#") == 0 && parseLiteral("T#" + text.substr(4), v)) return s5timeBits(v.i);
            if (u.compare(0, 2, "C#") == 0) {
                const long n = std::strtol(text.c_str() + 2, nullptr, 10);
                if (n < 0 || n > 999) {
                    warn("block " + f.block->name + ": the count " + text + " is not 0 to 999");
                    ok = false;
                    return 0;
                }
                return bcdOf(n);
            }
            if (!parseLiteral(text, v)) {
                warn("block " + f.block->name + ": STL loads " + text + ", which S7Forge does not read");
                ok = false;
                return 0;
            }
        }
        if (!t) t = v.type;
    } else {
        const Json* r = ref(f, e);
        Val c;
        if (r && constantOf(*r, c)) {
            constant = true;
            v = c;
            t = c.type;
        } else {
            Place p = placeOf(f, e);
            if (!p.valid()) {
                ok = false;
                return 0;
            }
            if (p.var && p.var->shape != Var::Leaf && !p.sliceBits) {
                warn("block " + f.block->name + ": STL loads the structure " + e["name"].str() + ": not done");
                ok = false;
                return 0;
            }
            v = load(p);
            t = p.sliceBits ? v.type : p.var ? p.var->type : p.type;
        }
    }
    if (!t) t = v.type;
    switch (v.kind) {
        case Kind::Bool: return v.b ? 1 : 0;
        case Kind::Real:
            if (t && t->bits == 64) warn("block " + f.block->name + ": STL loads an LReal: as a Real");
            return bitsOf(static_cast<float>(v.r));
        case Kind::Time:
        case Kind::Tod: {
            if (isS5Time(t) || isS5Time(v.type)) {
                uint16_t raw;
                if (!constant && s5Bits(f, e, raw)) return raw;
                return s5timeBits(v.i);
            }
            const int64_t unit = v.type && v.type->unitNs > 1 ? v.type->unitNs : 1000000;
            return static_cast<uint32_t>(v.i / (unit == 1 ? 1000000 : unit));
        }
        case Kind::Int:
        case Kind::Date:
        case Kind::Char: {
            const int bits = t && t->bits ? t->bits : 32;
            if (bits > 32) warn("block " + f.block->name + ": STL loads a 64-bit number: its low 32 bits");
            if (bits >= 32) return static_cast<uint32_t>(v.i);
            if (constant && t && t->isSigned) return static_cast<uint32_t>(static_cast<int32_t>(v.i));
            return static_cast<uint32_t>(v.i) & ((1u << bits) - 1);
        }
        default:
            warn("block " + f.block->name + ": STL loads " + e["name"].str() + ", which is not a number");
            ok = false;
            return 0;
    }
}

void Simulator::stlTransfer(Frame& f, const Json& e, uint32_t raw) {
    Place p = placeOf(f, e);
    if (!p.valid()) return;
    if (p.var && p.var->shape != Var::Leaf && !p.sliceBits) {
        warn("block " + f.block->name + ": STL transfers to the structure " + e["name"].str() + ": not done");
        return;
    }
    const ElemType* t = p.sliceBits ? elemType(p.sliceBits == 1 ? "Bool" : p.sliceBits == 8 ? "Byte" : p.sliceBits == 16 ? "Word" : "DWord")
                                    : p.var ? p.var->type : p.type;
    if (!t) return;
    Val v;
    switch (t->kind) {
        case Kind::Bool: v = boolVal(raw & 1); break;
        case Kind::Real: v = realVal(f32(raw)); break;
        case Kind::Time:
        case Kind::Tod:
            v = isS5Time(t) ? timeVal(s5timeNs(static_cast<uint16_t>(raw))) : timeVal(static_cast<int64_t>(s32(raw)) * 1000000);
            break;
        case Kind::Int:
        case Kind::Date:
        case Kind::Char: {
            const int bits = t->bits ? t->bits : 32;
            int64_t n = raw;
            if (bits < 32) n = raw & ((1u << bits) - 1);
            if (t->isSigned && bits <= 32) {
                const int64_t sign = int64_t(1) << (bits - 1);
                if (n & sign) n -= sign * 2;
            } else if (t->isSigned) {
                n = s32(raw);  // a 64-bit number from 32 bits: sign-extended
            }
            v = intVal(n);
            if (t->kind == Kind::Date) v.kind = Kind::Date;
            if (t->kind == Kind::Char) v.kind = Kind::Char;
            break;
        }
        default:
            warn("block " + f.block->name + ": STL transfers to " + e["name"].str() + ", which is not a number");
            return;
    }
    store(p, v);
}

// A bit operand: a variable, or a status bit (A OV, A BR, A >0, ...)
bool Simulator::stlBit(Frame& f, StlState& st, const Json& e) {
    if (e["op"].str() == "const") {
        std::string k = upper(e["value"].type == Json::Object ? e["value"]["text"].str() : e["name"].str());
        if (k.empty()) k = upper(e["name"].str());
        if (k == "OV") return st.ov;
        if (k == "OS") return st.os;
        if (k == "BR" || k == "BIE") return st.br;
        if (k == "UO") return st.cc0 && st.cc1;
        if (k == "==0") return !st.cc0 && !st.cc1;
        if (k == "<>0") return st.cc0 != st.cc1;
        if (k == ">0") return st.cc1 && !st.cc0;
        if (k == "<0") return st.cc0 && !st.cc1;
        if (k == ">=0") return !st.cc0;
        if (k == "<=0") return !st.cc1;
        if (e["value"].type == Json::Object && e["value"]["kind"].str() == "bool") return e["value"]["bool"].b;
        warn("block " + f.block->name + ": the STL bit operand " + k + " is not simulated");
        return false;
    }
    return truthy(eval(f, e));
}

// A, AN, O, ON, X, XN with the value of their operand
void Simulator::stlLogic(StlState& st, const std::string& op, bool v) {
    const bool neg = op.size() >= 2 && op[op.size() - 1] == 'N';
    if (neg) v = !v;
    const char kind = op[0];
    if (!st.fc) {
        st.term = v;  // the first check: the operand starts the string (or a term after O)
    } else if (kind == 'A') {
        st.term = st.term && v;
    } else if (kind == 'O') {
        st.term = st.rlo() || v;
        st.orBit = false;
    } else {  // X
        st.term = st.rlo() != v;
        st.orBit = false;
    }
    st.fc = true;
}

// ---- statements ----

size_t Simulator::stlStep(Frame& f, StlState& st, size_t pc) {
    const StlCode& c = *f.block->stl;
    const Json& s = *c.code[pc];
    if (++steps_ > kMaxSteps) {
        stop("the cycle did not end (an endless loop?): maximum cycle time exceeded");
        return c.code.size();
    }
    const size_t next = pc + 1;
    if (s["op"].str() == "label") return next;
    if (s["op"].str() != "stl") {
        // a statement of another kind in an STL network: run as such
        stmt(f, s);
        return next;
    }
    std::string op = mnemonic(s["name"].str(), c.german);
    const auto& x = s["exprs"].items;
    const Json* opd = x.empty() ? nullptr : &x[0];
    // the operand as shown: absolute and indirect operands are read from it
    std::string text;
    if (s["text"].type == Json::String) {
        text = s["text"].str();
    } else {
        for (const auto& e : x)
            if (e["op"].str() == "ref" || e["op"].str() == "const") text += (text.empty() ? "" : " ") + e["name"].str();
    }
    // "A (" as two pieces
    if ((op == "A" || op == "AN" || op == "O" || op == "ON" || op == "X" || op == "XN") &&
        ((opd && (*opd)["op"].str() == "const" && (*opd)["name"].str() == "(") || text == "(")) {
        op += "(";
        opd = nullptr;
        text.clear();
    }
    static const char* const needs[] = {"=",  "S",   "R",   "FP",  "FN",  "L",   "T",   "CALL", "JU",  "JC",  "JCN",
                                        "JCB", "JNB", "JBI", "JNBI", "JO", "JOS", "JZ",  "JN",   "JP",  "JM",  "JPZ",
                                        "JMZ", "JUO", "JL",  "LOOP", "LC", "SP",  "SE",  "SD",   "SS",  "SF",  "FR",
                                        "CU",  "CD",  "OPN"};
    if (!opd && text.empty())
        for (const char* n : needs)
            if (op == n) {
                warn("block " + f.block->name + ": STL " + op + " without an operand: not done");
                return next;
            }
    // the operand as an address: absolute, indirect, a timer or counter, a
    // pointer (`isAbs`); a tag of the type Timer or Counter; a data block by name
    Abs abs;
    bool isAbs = false;
    auto address = [&]() -> bool {
        if (isAbs) return true;
        bool isAddress = false;
        if (!text.empty() && stlAddress(f, text, abs, isAddress)) return isAbs = true;
        if (isAddress) return false;  // an address, but a bad one: said
        if (const Json* r = opd ? ref(f, *opd) : nullptr) {
            const std::string& kind = (*r)["kind"].str();
            const std::string name = (*r)["name"].str().empty() ? (*r)["text"].str() : (*r)["name"].str();
            std::string bare = name;
            if (bare.size() >= 2 && bare.front() == '"' && bare.back() == '"') bare = bare.substr(1, bare.size() - 2);
            if (kind == "tag") {
                auto t = s5Tags_.find(bare);
                if (t != s5Tags_.end()) {
                    abs.area = t->second.first;
                    abs.number = t->second.second;
                    return isAbs = true;
                }
            }
            if (kind == "data block" || kind == "instance data block") {
                for (const auto& d : dbInfo_)
                    if (d.second.name == bare) {
                        abs.area = 'B';
                        abs.number = d.first;
                        return isAbs = true;
                    }
            }
        }
        return false;
    };
    // MCR: with MCRA on and a FALSE on the MCR stack, = and T write 0, S and R do nothing
    const bool masked = st.mcrActive && std::find(mcrStack_.begin(), mcrStack_.end(), false) != mcrStack_.end();
    auto& a = st.accu;
    auto pop2 = [&] {  // after an operation of ACCU1 and ACCU2: ACCU3 and 4 move up
        a[1] = a[2];
        a[2] = a[3];
    };
    auto result = [&](int64_t r) {  // CC1, CC0 by the sign of a result
        st.cc1 = r > 0;
        st.cc0 = r < 0;
    };
    auto overflow = [&](bool ov) {
        st.ov = ov;
        if (ov) st.os = true;
    };
    auto setRlo = [&](bool v) {
        st.term = v;
        st.orBit = false;
    };
    auto jumpTo = [&]() -> size_t {
        const std::string label = opd ? (*opd)["name"].str() : text;
        auto it = c.labels.find(upper(label));
        if (it == c.labels.end()) {
            warn("block " + f.block->name + ": STL jumps to the label " + label + ", which is not in the block");
            return next;
        }
        return it->second;
    };
    auto conditional = [&](bool jump) -> size_t {  // after JC, JCN, ...: a new logic string
        st.fc = false;
        st.term = true;
        st.orBit = false;
        return jump ? jumpTo() : next;
    };
    auto timerOrCounter = [&](char kind) -> bool {
        if (!address() || abs.area != kind) return false;
        if (abs.number < 0 || abs.number >= 2048) {
            stop("STL: " + std::string(1, kind) + " " + std::to_string(abs.number) + " does not exist (block " + f.block->name + ")");
            return false;
        }
        return true;
    };
    // the value of the operand, as in ACCU1
    auto operand = [&](bool& ok) -> uint32_t {
        ok = true;
        if (address()) {
            if (abs.area == 'T' || abs.area == 'C') {
                if (!timerOrCounter(abs.area)) {
                    ok = false;
                    return 0;
                }
                if (abs.area == 'T') return static_cast<uint32_t>(s5Units(t5_[static_cast<size_t>(abs.number)]));
                return static_cast<uint32_t>(c5_[static_cast<size_t>(abs.number)].value);
            }
            if (abs.area == 'B' || abs.area == 'J') return static_cast<uint32_t>(abs.number);
            return absRead(f, abs, ok);
        }
        if (!opd) {
            if (text.empty()) {
                ok = false;
                return 0;
            }
            // a constant only as text (C#5, S5T#2S)
            Json e;
            e.type = Json::Object;
            Json op1, name;
            op1.type = name.type = Json::String;
            op1.s = "const";
            name.s = text;
            e.members.emplace_back("op", op1);
            e.members.emplace_back("name", name);
            return stlLoad(f, e, ok);
        }
        return stlLoad(f, *opd, ok);
    };
    auto count = [&]() -> uint32_t {  // of a shift: the operand, or the low byte of ACCU2
        bool ok = true;
        return opd || !text.empty() ? operand(ok) & 0xFF : a[1] & 0xFF;
    };
    // a bit operand
    auto bit = [&]() -> bool {
        if (address()) {
            if (abs.area == 'T' || abs.area == 'C') {
                if (!timerOrCounter(abs.area)) return false;
                if (abs.area == 'C') return c5_[static_cast<size_t>(abs.number)].value != 0;
                S5Timer& t = t5_[static_cast<size_t>(abs.number)];
                s5Update(t);
                return t.q;
            }
            if (abs.isPointer || abs.size != 'X') {
                warn("block " + f.block->name + ": STL " + op + " with the operand " + text + ", which is not a bit");
                return false;
            }
            bool ok = true;
            return absRead(f, abs, ok) != 0;
        }
        return opd ? stlBit(f, st, *opd) : false;
    };
    auto writeBit = [&](bool v) {
        if (address()) {
            if (abs.isPointer || abs.size != 'X' || abs.area == 'T' || abs.area == 'C') {
                warn("block " + f.block->name + ": STL " + op + " with the operand " + text + ", which is not a bit");
                return;
            }
            absWrite(f, abs, v ? 1 : 0);
            return;
        }
        if (opd) store(placeOf(f, *opd), boolVal(v));
    };

    // ---- bit logic ----
    if (op == "A" || op == "AN" || op == "O" || op == "ON" || op == "X" || op == "XN") {
        if (!opd && text.empty()) {
            if (op == "O") {  // AND before OR: the term so far is kept, a new one starts
                st.orBit = st.rlo();
                st.fc = false;
                return next;
            }
            warn("block " + f.block->name + ": STL " + op + " without an operand");
            return next;
        }
        stlLogic(st, op, bit());
        return next;
    }
    if (op == "A(" || op == "AN(" || op == "O(" || op == "ON(" || op == "X(" || op == "XN(") {
        if (st.nest.size() >= 7) {
            stop("STL: more than 7 levels of brackets in block " + f.block->name);
            return c.code.size();
        }
        st.nest.push_back({op.substr(0, op.size() - 1), st.term, st.orBit, st.fc});
        st.fc = false;
        st.orBit = false;
        st.term = false;
        return next;
    }
    if (op == ")") {
        if (st.nest.empty()) {
            warn("block " + f.block->name + ": STL ) without (");
            return next;
        }
        const bool v = st.rlo();
        const StlState::Nest n = st.nest.back();
        st.nest.pop_back();
        st.term = n.term;
        st.orBit = n.orBit;
        st.fc = n.fc;
        stlLogic(st, n.op, v);
        return next;
    }
    if (op == "=" || op == "S" || op == "R") {
        const bool rlo = st.rlo();
        if ((op == "R" || op == "S") && (timerOrCounter('T') || timerOrCounter('C'))) {
            if (abs.area == 'T') {
                if (op == "S") warn("block " + f.block->name + ": STL S with a timer: not an instruction");
                else if (rlo && !masked) s5TimerReset(abs.number);
            } else if (op == "R") {
                if (rlo && !masked) s5CounterReset(abs.number);
            } else if (!s5CounterSet(abs.number, rlo && !masked, static_cast<uint16_t>(a[0] & 0xFFFF),
                                     "STL S C in block " + f.block->name)) {
                return c.code.size();
            }
        } else if (op == "=") {
            writeBit(masked ? false : rlo);
        } else if (rlo && !masked) {
            writeBit(op == "S");
        }
        st.term = rlo;
        st.orBit = false;
        st.fc = false;
        return next;
    }
    if (op == "NOT") {
        setRlo(!st.rlo());
        return next;
    }
    if (op == "SET" || op == "CLR") {
        setRlo(op == "SET");
        st.fc = false;
        return next;
    }
    if (op == "SAVE") {
        st.br = st.rlo();
        return next;
    }
    if (op == "FP" || op == "FN") {
        const bool now = st.rlo();
        bool old;
        if (address()) {
            if (abs.isPointer || abs.size != 'X' || abs.area == 'T' || abs.area == 'C') {
                warn("block " + f.block->name + ": STL " + op + " with the operand " + text + ", which is not a bit");
                return next;
            }
            bool ok = true;
            old = absRead(f, abs, ok) != 0;
            absWrite(f, abs, now ? 1 : 0);
        } else {
            if (!opd) return next;
            Place mem = placeOf(f, *opd);
            old = truthy(load(mem));
            store(mem, boolVal(now));
        }
        setRlo(op == "FP" ? now && !old : !now && old);
        st.fc = true;
        return next;
    }
    if (op == "NOP" || op == "BLD") return next;

    // ---- load, transfer, accumulators ----
    if (op == "L" || op == "LC") {
        uint32_t v = 0;
        const std::string k = upper(text);
        if (op == "L" && (k == "DBNO" || k == "DINO")) {
            v = static_cast<uint32_t>(k == "DBNO" ? curDb_ : curDi_);
        } else if (op == "L" && (k == "DBLG" || k == "DILG")) {
            auto d = dbInfo_.find(k == "DBLG" ? curDb_ : curDi_);
            if (d != dbInfo_.end() && !d->second.optimized) {
                if (!d->second.laidOut) layOut(d->second);
                v = static_cast<uint32_t>(d->second.bytes);
            }
        } else if (op == "L" && k == "STW") {
            v = (st.fc ? 1u : 0) | (st.rlo() ? 2u : 0) | (st.orBit ? 8u : 0) | (st.os ? 16u : 0) | (st.ov ? 32u : 0) |
                (st.cc0 ? 64u : 0) | (st.cc1 ? 128u : 0) | (st.br ? 256u : 0);
        } else if (op == "LC") {
            // a timer or counter in BCD; a timer with its time base
            if (timerOrCounter('T')) {
                S5Timer& t = t5_[static_cast<size_t>(abs.number)];
                v = (static_cast<uint32_t>(t.base) << 12) | bcdOf(s5Units(t));
            } else if (timerOrCounter('C')) {
                v = bcdOf(c5_[static_cast<size_t>(abs.number)].value);
            } else {
                warn("block " + f.block->name + ": STL LC " + text + ": not a timer or counter");
                return next;
            }
        } else {
            bool ok = true;
            v = operand(ok);
        }
        a[1] = a[0];
        a[0] = v;
        return next;
    }
    if (op == "T") {
        const std::string k = upper(text);
        if (k == "STW") {
            // /FC, STA and OR are not written
            const uint32_t v = a[0];
            st.term = (v & 2) != 0;
            st.os = (v & 16) != 0;
            st.ov = (v & 32) != 0;
            st.cc0 = (v & 64) != 0;
            st.cc1 = (v & 128) != 0;
            st.br = (v & 256) != 0;
            return next;
        }
        const uint32_t v = masked ? 0 : a[0];
        if (address()) {
            if (abs.isPointer || abs.area == 'T' || abs.area == 'C' || abs.area == 'B' || abs.area == 'J') {
                warn("block " + f.block->name + ": STL T to " + text + ": not a place to write");
                return next;
            }
            absWrite(f, abs, abs.size == 'X' ? (v & 1) : v);
        } else if (opd) {
            stlTransfer(f, *opd, v);
        }
        return next;
    }
    if (op == "TAK") {
        std::swap(a[0], a[1]);
        return next;
    }
    if (op == "PUSH") {
        a[3] = a[2];
        a[2] = a[1];
        a[1] = a[0];
        return next;
    }
    if (op == "POP") {
        a[0] = a[1];
        a[1] = a[2];
        a[2] = a[3];
        return next;
    }
    if (op == "ENT") {
        a[3] = a[2];
        a[2] = a[1];
        return next;
    }
    if (op == "LEAVE") {
        a[1] = a[2];
        a[2] = a[3];
        return next;
    }

    // ---- integer arithmetic ----
    if (op == "+I" || op == "-I" || op == "*I" || op == "/I") {
        const int32_t p = low16(a[1]), q = low16(a[0]);
        if (op == "/I") {
            if (q == 0) {
                overflow(true);
                st.cc0 = st.cc1 = true;
                pop2();
                return next;
            }
            const int32_t quo = p / q, rem = p % q;
            overflow(quo > 32767);
            a[0] = (static_cast<uint32_t>(static_cast<uint16_t>(rem)) << 16) | static_cast<uint16_t>(quo);
            result(static_cast<int16_t>(quo));
        } else if (op == "*I") {
            const int32_t r = p * q;  // 32 bits in ACCU1
            overflow(r > 32767 || r < -32768);
            a[0] = static_cast<uint32_t>(r);
            result(r);  // the sign of the true product
        } else {
            const int32_t r = op == "+I" ? p + q : p - q;
            overflow(r > 32767 || r < -32768);
            a[0] = (a[0] & 0xFFFF0000u) | static_cast<uint16_t>(r);
            result(static_cast<int16_t>(r));
        }
        pop2();
        return next;
    }
    if (op == "+D" || op == "-D" || op == "*D" || op == "/D" || op == "MOD") {
        const int64_t p = s32(a[1]), q = s32(a[0]);
        int64_t r;
        if (op == "/D" || op == "MOD") {
            if (q == 0) {
                overflow(true);
                st.cc0 = st.cc1 = true;
                pop2();
                return next;
            }
            r = op == "/D" ? p / q : p % q;
        } else {
            r = op == "+D" ? p + q : op == "-D" ? p - q : p * q;
        }
        overflow(r > INT32_MAX || r < INT32_MIN);
        a[0] = static_cast<uint32_t>(static_cast<uint64_t>(r));
        // on an overflow of *D the true sign; of +D, -D the wrapped one
        result(op == "*D" ? r : s32(a[0]));
        pop2();
        return next;
    }
    if (op == "+" && opd) {  // + constant: to ACCU1, no status
        bool ok = true;
        const uint32_t k = operand(ok);
        const ElemType* t = elemType((*opd)["type"].str());
        if (t && t->bits == 16) a[0] = (a[0] & 0xFFFF0000u) | static_cast<uint16_t>(a[0] + k);
        else a[0] += k;
        return next;
    }
    if (op == "INC" || op == "DEC") {
        bool ok = true;
        const uint32_t n = opd ? stlLoad(f, *opd, ok) & 0xFF : 1;
        const uint32_t ll = (a[0] + (op == "INC" ? n : 0x100 - n)) & 0xFF;
        a[0] = (a[0] & 0xFFFFFF00u) | ll;
        return next;
    }
    if (op == "NEGI") {
        const int32_t r = -static_cast<int32_t>(low16(a[0]));
        overflow(r > 32767);
        a[0] = (a[0] & 0xFFFF0000u) | static_cast<uint16_t>(r);
        result(static_cast<int16_t>(r));
        return next;
    }
    if (op == "NEGD") {
        const int64_t r = -static_cast<int64_t>(s32(a[0]));
        overflow(r > INT32_MAX);
        a[0] = static_cast<uint32_t>(static_cast<uint64_t>(r));
        result(s32(a[0]));
        return next;
    }
    if (op == "INVI") {
        a[0] ^= 0xFFFFu;
        return next;
    }
    if (op == "INVD") {
        a[0] = ~a[0];
        return next;
    }

    // ---- real arithmetic ----
    auto realResult = [&](double r) {
        const float v = static_cast<float>(r);
        a[0] = bitsOf(v);
        if (std::isnan(v)) {
            st.cc0 = st.cc1 = true;
            overflow(true);
        } else if (std::fpclassify(v) == FP_SUBNORMAL) {
            overflow(true);  // an underflow: the result counts as 0
            st.cc0 = st.cc1 = false;
        } else {
            overflow(std::isinf(v));
            st.cc1 = v > 0;
            st.cc0 = v < 0;
        }
    };
    if (op == "+R" || op == "-R" || op == "*R" || op == "/R") {
        const double p = f32(a[1]), q = f32(a[0]);
        realResult(op == "+R" ? p + q : op == "-R" ? p - q : op == "*R" ? p * q : p / q);
        pop2();
        return next;
    }
    if (op == "NEGR") {
        a[0] ^= 0x80000000u;
        return next;
    }
    if (op == "ABS") {
        a[0] &= 0x7FFFFFFFu;
        return next;
    }
    if (op == "SQR" || op == "SQRT" || op == "EXP" || op == "LN" || op == "SIN" || op == "COS" || op == "TAN" ||
        op == "ASIN" || op == "ACOS" || op == "ATAN") {
        const double v = f32(a[0]);
        double r;
        if (op == "SQR") r = v * v;
        else if (op == "SQRT") r = std::sqrt(v);
        else if (op == "EXP") r = std::exp(v);
        else if (op == "LN") r = std::log(v);
        else if (op == "SIN") r = std::sin(v);
        else if (op == "COS") r = std::cos(v);
        else if (op == "TAN") r = std::tan(v);
        else if (op == "ASIN") r = std::asin(v);
        else if (op == "ACOS") r = std::acos(v);
        else r = std::atan(v);
        realResult(r);
        return next;
    }

    // ---- comparisons: ACCU2 against ACCU1 ----
    if (op.size() >= 2 && (op[0] == '=' || op[0] == '<' || op[0] == '>') &&
        (op.back() == 'I' || op.back() == 'D' || op.back() == 'R')) {
        const std::string rel = op.substr(0, op.size() - 1);
        int cmp = 0;
        bool unordered = false;
        if (op.back() == 'I') cmp = low16(a[1]) < low16(a[0]) ? -1 : low16(a[1]) > low16(a[0]) ? 1 : 0;
        else if (op.back() == 'D') cmp = s32(a[1]) < s32(a[0]) ? -1 : s32(a[1]) > s32(a[0]) ? 1 : 0;
        else {
            const float p = f32(a[1]), q = f32(a[0]);
            unordered = std::isnan(p) || std::isnan(q);
            cmp = p < q ? -1 : p > q ? 1 : 0;
        }
        bool r;
        if (unordered) r = rel == "<>";
        else if (rel == "==") r = cmp == 0;
        else if (rel == "<>") r = cmp != 0;
        else if (rel == ">") r = cmp > 0;
        else if (rel == "<") r = cmp < 0;
        else if (rel == ">=") r = cmp >= 0;
        else if (rel == "<=") r = cmp <= 0;
        else {
            warn("block " + f.block->name + ": the STL comparison " + op + " is not simulated");
            return next;
        }
        st.cc1 = unordered || cmp > 0;
        st.cc0 = unordered || cmp < 0;
        st.ov = unordered;
        if (unordered) st.os = true;
        setRlo(r);
        st.fc = true;
        return next;
    }

    // ---- conversions ----
    if (op == "ITD") {
        a[0] = static_cast<uint32_t>(static_cast<int32_t>(low16(a[0])));
        return next;
    }
    if (op == "DTR") {
        a[0] = bitsOf(static_cast<float>(s32(a[0])));
        return next;
    }
    if (op == "RND" || op == "RND+" || op == "RND-" || op == "TRUNC") {
        const double v = f32(a[0]);
        const double r = op == "RND" ? std::nearbyint(v) : op == "RND+" ? std::ceil(v) : op == "RND-" ? std::floor(v) : std::trunc(v);
        if (!std::isfinite(r) || r > INT32_MAX || r < INT32_MIN) {
            overflow(true);  // ACCU1 keeps the Real
            return next;
        }
        a[0] = static_cast<uint32_t>(static_cast<int32_t>(r));
        st.ov = false;
        return next;
    }
    if (op == "BTI" || op == "BTD") {
        int64_t n;
        const bool word = op == "BTI";
        if (!fromBcd(word ? a[0] & 0xFFFF : a[0], word ? 3 : 7, n)) {
            stop("STL " + op + ": not a BCD number in block " + f.block->name);
            return c.code.size();
        }
        a[0] = word ? (a[0] & 0xFFFF0000u) | static_cast<uint16_t>(n) : static_cast<uint32_t>(n);
        return next;
    }
    if (op == "ITB" || op == "DTB") {
        bool ov = false;
        const bool word = op == "ITB";
        const uint32_t v = toBcd(word ? low16(a[0]) : s32(a[0]), word ? 3 : 7, ov);
        overflow(ov);
        if (!ov) a[0] = word ? (a[0] & 0xFFFF0000u) | (v & 0xFFFF) : v;
        return next;
    }
    if (op == "CAW") {
        a[0] = (a[0] & 0xFFFF0000u) | ((a[0] & 0xFF) << 8) | ((a[0] >> 8) & 0xFF);
        return next;
    }
    if (op == "CAD") {
        a[0] = ((a[0] & 0xFF) << 24) | ((a[0] & 0xFF00) << 8) | ((a[0] >> 8) & 0xFF00) | (a[0] >> 24);
        return next;
    }

    // ---- word logic ----
    if (op == "AW" || op == "OW" || op == "XOW" || op == "AD" || op == "OD" || op == "XOD") {
        bool ok = true;
        const bool word = op.back() == 'W';
        const uint32_t k = opd || !text.empty() ? operand(ok) : a[1];
        const uint32_t mask = word ? 0xFFFFu : 0xFFFFFFFFu;
        uint32_t v = a[0] & mask;
        const char kind = op[0] == 'X' ? 'X' : op[0];
        v = kind == 'A' ? (v & k) : kind == 'O' ? (v | k) : (v ^ k);
        a[0] = (a[0] & ~mask) | (v & mask);
        st.cc1 = (v & mask) != 0;
        st.cc0 = false;
        st.ov = false;
        return next;
    }

    // ---- shifts and rotations ----
    if (op == "SLW" || op == "SRW" || op == "SSI" || op == "SLD" || op == "SRD" || op == "SSD" || op == "RLD" ||
        op == "RRD") {
        const uint32_t n = count();
        if (n == 0) return next;
        const bool word = op.back() == 'W' || op == "SSI";
        const int bits = word ? 16 : 32;
        const uint32_t mask = word ? 0xFFFFu : 0xFFFFFFFFu;
        const uint64_t v = a[0] & mask;
        uint64_t r = 0;
        bool last = false;
        if (op == "SLW" || op == "SLD") {
            last = n <= static_cast<uint32_t>(bits) && ((v >> (bits - n)) & 1);
            r = n >= static_cast<uint32_t>(bits) ? 0 : (v << n) & mask;
        } else if (op == "SRW" || op == "SRD") {
            last = n <= static_cast<uint32_t>(bits) && ((v >> (n - 1)) & 1);
            r = n >= static_cast<uint32_t>(bits) ? 0 : v >> n;
        } else if (op == "SSI" || op == "SSD") {
            const int64_t sv = word ? static_cast<int64_t>(low16(a[0])) : static_cast<int64_t>(s32(a[0]));
            const uint32_t m = std::min<uint32_t>(n, static_cast<uint32_t>(bits));
            last = ((sv >> (m - 1)) & 1) != 0;
            r = static_cast<uint64_t>(sv >> std::min<uint32_t>(m, 63)) & mask;
        } else {
            const uint32_t k = n % 32;
            r = k == 0 ? v : (op == "RLD" ? ((v << k) | (v >> (32 - k))) : ((v >> k) | (v << (32 - k)))) & mask;
            last = op == "RLD" ? (r & 1) : ((r >> 31) & 1);
        }
        a[0] = (a[0] & ~mask) | static_cast<uint32_t>(r);
        st.cc1 = last;
        st.cc0 = false;
        st.ov = false;
        return next;
    }

    // ---- jumps ----
    if (op == "JU") return jumpTo();
    if (op == "JC") return conditional(st.rlo());
    if (op == "JCN") return conditional(!st.rlo());
    if (op == "JCB" || op == "JNB") {
        const bool rlo = st.rlo();
        st.br = rlo;
        return conditional(op == "JCB" ? rlo : !rlo);
    }
    if (op == "JBI" || op == "JNBI") {
        st.fc = false;
        st.orBit = false;
        return (op == "JBI") == st.br ? jumpTo() : next;
    }
    if (op == "JO") return st.ov ? jumpTo() : next;
    if (op == "JOS") {
        const bool os = st.os;
        st.os = false;
        return os ? jumpTo() : next;
    }
    if (op == "JZ") return !st.cc1 && !st.cc0 ? jumpTo() : next;
    if (op == "JN") return st.cc1 != st.cc0 ? jumpTo() : next;
    if (op == "JP") return st.cc1 && !st.cc0 ? jumpTo() : next;
    if (op == "JM") return !st.cc1 && st.cc0 ? jumpTo() : next;
    if (op == "JPZ") return !st.cc0 ? jumpTo() : next;
    if (op == "JMZ") return !st.cc1 ? jumpTo() : next;
    if (op == "JUO") return st.cc1 && st.cc0 ? jumpTo() : next;
    if (op == "JL") {
        // a list of JU follows, up to the label: ACCU1-LL picks one, past
        // the end the label
        const size_t end = jumpTo();
        if (end == next || end <= pc) return next;
        const size_t n = a[0] & 0xFF;
        size_t k = 0;
        for (size_t i = next; i < end; ++i) {
            if (c.code[i]->operator[]("op").str() == "label") continue;
            if (k++ == n) return i;
        }
        return end;
    }
    if (op == "LOOP") {
        const uint16_t n = static_cast<uint16_t>((a[0] & 0xFFFF) - 1);
        a[0] = (a[0] & 0xFFFF0000u) | n;
        return n != 0 ? jumpTo() : next;
    }

    // ---- S5 timers and counters ----
    if (op == "FR" && address() && abs.area == 'C') {
        const bool rlo = st.rlo();
        if (timerOrCounter('C')) s5CounterFr(abs.number, rlo);
        st.fc = false;
        st.orBit = false;
        st.term = rlo;
        return next;
    }
    if (op == "SP" || op == "SE" || op == "SD" || op == "SS" || op == "SF" || op == "FR") {
        const bool rlo = st.rlo();
        if (timerOrCounter('T')) {
            if (op == "FR") s5TimerFr(abs.number, rlo);
            else if (!s5TimerStart(abs.number, op[1], rlo, static_cast<uint16_t>(a[0] & 0xFFFF),
                                   "STL " + op + " in block " + f.block->name))
                return c.code.size();
        } else {
            warn("block " + f.block->name + ": STL " + op + " " + text + ": not a timer");
        }
        st.fc = false;
        st.orBit = false;
        st.term = rlo;
        return next;
    }
    if (op == "CU" || op == "CD") {
        const bool rlo = st.rlo();
        if (timerOrCounter('C')) s5CounterCount(abs.number, op == "CU", rlo);
        else warn("block " + f.block->name + ": STL " + op + " " + text + ": not a counter");
        st.fc = false;
        st.orBit = false;
        st.term = rlo;
        return next;
    }

    // ---- data blocks and address registers ----
    if (op == "OPN") {
        if (address() && (abs.area == 'B' || abs.area == 'J')) {
            if (!dbInfo_.count(abs.number)) {
                stop("STL OPN: DB" + std::to_string(abs.number) + " is not in the program (block " + f.block->name + ")");
                return c.code.size();
            }
            (abs.area == 'B' ? curDb_ : curDi_) = abs.number;
        } else {
            warn("block " + f.block->name + ": STL OPN " + text + ": not a data block");
        }
        return next;
    }
    if (op == "CDB") {
        std::swap(curDb_, curDi_);
        return next;
    }
    if (op == "LAR1" || op == "LAR2") {
        uint32_t& ar = op == "LAR1" ? ar1_ : ar2_;
        const std::string k = upper(text);
        if (k.empty()) ar = a[0];
        else if (k == "AR2") ar = ar2_;
        else if (k == "AR1") ar = ar1_;
        else {
            bool ok = true;
            const uint32_t v = operand(ok);
            if (ok) ar = v;
        }
        return next;
    }
    if (op == "TAR1" || op == "TAR2") {
        const uint32_t v = op == "TAR1" ? ar1_ : ar2_;
        const std::string k = upper(text);
        if (k.empty()) {
            a[1] = a[0];
            a[0] = v;
        } else if (k == "AR2") {
            ar2_ = v;
        } else if (address()) {
            absWrite(f, abs, v);
        }
        return next;
    }
    if (op == "CAR") {
        std::swap(ar1_, ar2_);
        return next;
    }
    if (op == "+AR1" || op == "+AR2") {
        uint32_t& ar = op == "+AR1" ? ar1_ : ar2_;
        uint32_t off;
        if (text.empty()) {
            off = static_cast<uint32_t>(static_cast<int32_t>(low16(a[0])));  // ACCU1-L, a signed number of bits
        } else {
            bool ok = true;
            off = operand(ok) & 0xFFFFFF;
        }
        ar = (ar & 0xFF000000u) | ((ar + off) & 0x00FFFFFFu);
        return next;
    }

    // ---- master control relay ----
    if (op == "MCRA" || op == "MCRD") {
        st.mcrActive = op == "MCRA";
        return next;
    }
    if (op == "MCR(") {
        if (mcrStack_.size() >= 8) {
            stop("STL: the MCR stack overflows (more than 8 MCR( in block " + f.block->name + ")");
            return c.code.size();
        }
        mcrStack_.push_back(st.rlo());
        st.fc = false;
        st.orBit = false;
        return next;
    }
    if (op == ")MCR") {
        if (mcrStack_.empty()) {
            stop("STL: )MCR without MCR( in block " + f.block->name);
            return c.code.size();
        }
        mcrStack_.pop_back();
        st.fc = false;
        st.orBit = false;
        return next;
    }

    // ---- blocks ----
    if (op == "BE" || op == "BEU") {
        f.returned = true;
        return c.code.size();
    }
    if (op == "BEC") {
        if (st.rlo()) {
            f.returned = true;
            return c.code.size();
        }
        st.fc = false;
        setRlo(true);
        return next;
    }
    if (op == "CALL") {
        // as a call of the code tree: the parameters are the operands after
        // the block (and its instance data block)
        Json e;
        e.type = Json::Object;
        Json args;
        args.type = Json::Array;
        for (const auto& p : x) {
            const std::string& k = p["op"].str();
            if (k == "in" || k == "out" || k == "inout") args.items.push_back(p);
        }
        Json name;
        name.type = Json::String;
        name.s = opd ? (*opd)["name"].str() : std::string();
        e.members.emplace_back("op", Json());
        e.members.back().second.type = Json::String;
        e.members.back().second.s = "call";
        e.members.emplace_back("name", name);
        e.members.emplace_back("args", std::move(args));
        e.members.emplace_back("target", s["target"]);
        lastEno_ = true;
        call(f, e);
        st.br = lastEno_;
        st.fc = false;
        st.orBit = false;
        return next;
    }

    warn("block " + f.block->name + ": the STL instruction " + s["name"].str() + " is not simulated");
    return next;
}

}  // namespace s7forge
