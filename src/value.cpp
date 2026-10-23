// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "value.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace s7forge {
namespace {

const ElemType kTypes[] = {
    {"Bool", Kind::Bool, 1, false, false, 0},
    {"Byte", Kind::Int, 8, false, true, 0},
    {"Word", Kind::Int, 16, false, true, 0},
    {"DWord", Kind::Int, 32, false, true, 0},
    {"LWord", Kind::Int, 64, false, true, 0},
    {"SInt", Kind::Int, 8, true, false, 0},
    {"USInt", Kind::Int, 8, false, false, 0},
    {"Int", Kind::Int, 16, true, false, 0},
    {"UInt", Kind::Int, 16, false, false, 0},
    {"DInt", Kind::Int, 32, true, false, 0},
    {"UDInt", Kind::Int, 32, false, false, 0},
    {"LInt", Kind::Int, 64, true, false, 0},
    {"ULInt", Kind::Int, 64, false, false, 0},
    {"Real", Kind::Real, 32, true, false, 0},
    {"LReal", Kind::Real, 64, true, false, 0},
    {"Time", Kind::Time, 32, true, false, 1000000},
    {"LTime", Kind::Time, 64, true, false, 1},
    {"S5Time", Kind::Time, 16, false, false, 1000000},
    {"Date", Kind::Date, 16, false, false, 0},
    {"Time_Of_Day", Kind::Tod, 32, false, false, 1000000},
    {"TOD", Kind::Tod, 32, false, false, 1000000},
    {"LTime_Of_Day", Kind::Tod, 64, false, false, 1},
    {"LTOD", Kind::Tod, 64, false, false, 1},
    {"Date_And_Time", Kind::Dt, 64, false, false, 1000000},
    {"DT", Kind::Dt, 64, false, false, 1000000},
    {"LDT", Kind::Dt, 64, false, false, 1},
    {"Char", Kind::Char, 8, false, false, 0},
    {"WChar", Kind::Char, 16, false, false, 0},
    {"String", Kind::String, 0, false, false, 0},
    {"WString", Kind::String, 0, false, false, 0},
    // numbers the system gives meaning to: hardware identifiers, OB numbers
    {"Hw_Any", Kind::Int, 16, false, true, 0},
    {"Hw_Device", Kind::Int, 16, false, true, 0},
    {"Hw_DpMaster", Kind::Int, 16, false, true, 0},
    {"Hw_DpSlave", Kind::Int, 16, false, true, 0},
    {"Hw_Io", Kind::Int, 16, false, true, 0},
    {"Hw_IoSystem", Kind::Int, 16, false, true, 0},
    {"Hw_SubModule", Kind::Int, 16, false, true, 0},
    {"Hw_Module", Kind::Int, 16, false, true, 0},
    {"Hw_Interface", Kind::Int, 16, false, true, 0},
    {"Hw_Hsc", Kind::Int, 16, false, true, 0},
    {"Hw_Pwm", Kind::Int, 16, false, true, 0},
    {"Hw_Pto", Kind::Int, 16, false, true, 0},
    {"OB_Any", Kind::Int, 16, true, false, 0},
    {"OB_Delay", Kind::Int, 16, true, false, 0},
    {"OB_Tod", Kind::Int, 16, true, false, 0},
    {"OB_Cyclic", Kind::Int, 16, true, false, 0},
    {"OB_Att", Kind::Int, 16, true, false, 0},
    {"OB_PCycle", Kind::Int, 16, true, false, 0},
    {"OB_HwInt", Kind::Int, 16, true, false, 0},
    {"OB_Diag", Kind::Int, 16, true, false, 0},
    {"OB_TimeError", Kind::Int, 16, true, false, 0},
    {"OB_Startup", Kind::Int, 16, true, false, 0},
    {"Pip", Kind::Int, 16, false, false, 0},
    {"DB_Any", Kind::Int, 16, false, false, 0},
    {"DB_WWW", Kind::Int, 16, false, false, 0},
    {"DB_Dyn", Kind::Int, 16, false, false, 0},
    {"Conn_Any", Kind::Int, 16, false, true, 0},
    {"Conn_Prg", Kind::Int, 16, false, true, 0},
    {"Conn_Ouc", Kind::Int, 16, false, true, 0},
    {"Port", Kind::Int, 16, false, false, 0},
};

std::string lower(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

// a whole number wrapped to the size and sign of `t`
int64_t wrap(int64_t v, const ElemType& t) {
    if (t.bits >= 64) return v;
    const uint64_t mask = (uint64_t(1) << t.bits) - 1;
    uint64_t u = static_cast<uint64_t>(v) & mask;
    if (t.isSigned && (u >> (t.bits - 1)) & 1) return static_cast<int64_t>(u | ~mask);
    return static_cast<int64_t>(u);
}

// round to the nearest, halves to even, as the S7 conversions do
int64_t roundEven(double r) { return clampToInt64(std::nearbyint(r)); }

}  // namespace

int64_t clampToInt64(double r) {
    if (!std::isfinite(r)) return 0;
    if (r >= 9223372036854775807.0) return INT64_MAX;
    if (r <= -9223372036854775808.0) return INT64_MIN;
    return static_cast<int64_t>(r);
}

const ElemType* elemType(const std::string& name) {
    if (name.empty()) return nullptr;
    static const std::unordered_map<std::string, const ElemType*> byName = [] {
        std::unordered_map<std::string, const ElemType*> m;
        for (const ElemType& t : kTypes) m.emplace(lower(t.name), &t);
        return m;
    }();
    std::string n = lower(name);
    // String[20] is a String
    const size_t br = n.find('[');
    if (br != std::string::npos) n = n.substr(0, br);
    while (!n.empty() && n.back() == ' ') n.pop_back();
    const auto it = byName.find(n);
    return it == byName.end() ? nullptr : it->second;
}

Val boolVal(bool b) {
    Val v;
    v.kind = Kind::Bool;
    v.b = b;
    v.type = elemType("Bool");
    return v;
}
Val intVal(int64_t i, const ElemType* type) {
    Val v;
    v.kind = Kind::Int;
    v.i = i;
    v.type = type;
    return v;
}
Val realVal(double r, const ElemType* type) {
    Val v;
    v.kind = Kind::Real;
    v.r = r;
    v.type = type;
    return v;
}
Val timeVal(int64_t ns, const ElemType* type) {
    Val v;
    v.kind = Kind::Time;
    v.i = ns;
    v.type = type ? type : elemType("Time");
    return v;
}
Val stringVal(const std::string& s) {
    Val v;
    v.kind = Kind::String;
    v.s = s;
    v.type = elemType("String");
    return v;
}

bool truthy(const Val& v) {
    switch (v.kind) {
        case Kind::Bool: return v.b;
        case Kind::Real: return v.r != 0;
        case Kind::String: return !v.s.empty();
        case Kind::None: return false;
        default: return v.i != 0;
    }
}

int64_t asInt(const Val& v) {
    switch (v.kind) {
        case Kind::Bool: return v.b ? 1 : 0;
        case Kind::Real: return clampToInt64(std::trunc(v.r));
        case Kind::String: return 0;
        default: return v.i;
    }
}

double asReal(const Val& v) {
    switch (v.kind) {
        case Kind::Bool: return v.b ? 1 : 0;
        case Kind::Real: return v.r;
        case Kind::String: return 0;
        default: return static_cast<double>(v.i);
    }
}

Val convertTo(const Val& v, const ElemType& t, int maxLen) {
    Val o;
    o.kind = t.kind;
    o.type = &t;
    switch (t.kind) {
        case Kind::Bool:
            o.b = truthy(v);
            break;
        case Kind::Int:
            if (v.kind == Kind::Real) {
                o.i = wrap(roundEven(v.r), t);
            } else if (v.kind == Kind::Time || v.kind == Kind::Tod) {
                // a duration as a number counts its units: ms for Time and TOD, ns for LTime
                const int64_t unit = v.type && v.type->unitNs ? v.type->unitNs : 1000000;
                o.i = wrap(v.i / unit, t);
            } else {
                o.i = wrap(asInt(v), t);
            }
            break;
        case Kind::Real:
            o.r = asReal(v);
            if (t.bits == 32) o.r = static_cast<double>(static_cast<float>(o.r));
            break;
        case Kind::Time:
        case Kind::Tod:
        case Kind::Dt:
            // a duration keeps its nanoseconds, cut to what the type holds;
            // a plain number counts the type's units (ms for Time)
            if (v.kind == Kind::Time || v.kind == Kind::Tod || v.kind == Kind::Dt) o.i = v.i;
            else {
                const int64_t unit = t.unitNs ? t.unitNs : 1;
                const int64_t n = asInt(v);
                o.i = (n > INT64_MAX / unit || n < INT64_MIN / unit) ? (n < 0 ? INT64_MIN : INT64_MAX) / unit * unit
                                                                     : n * unit;
            }
            if (t.unitNs > 1) o.i = (o.i / t.unitNs) * t.unitNs;
            if (t.kind == Kind::Time && t.bits == 32) {
                // Time is a DInt of milliseconds
                const int64_t ms = wrap(o.i / 1000000, *elemType("DInt"));
                o.i = ms * 1000000;
            }
            if (t.kind == Kind::Tod && t.unitNs == 1000000) o.i %= 86400000000000LL;
            if (isS5Time(&t)) o.i = s5timeNs(s5timeBits(o.i));  // its time base: 10 ms to 10 s
            break;
        case Kind::Date:
            o.i = wrap(asInt(v), t);
            break;
        case Kind::Char:
            if (v.kind == Kind::String) o.i = v.s.empty() ? 0 : static_cast<unsigned char>(v.s[0]);
            else o.i = wrap(asInt(v), t);
            break;
        case Kind::String:
            if (v.kind == Kind::String) o.s = v.s;
            else if (v.kind == Kind::Char) o.s = std::string(1, static_cast<char>(v.i));
            else o.s = text(v);
            if (static_cast<int>(o.s.size()) > maxLen) o.s.resize(static_cast<size_t>(maxLen));
            break;
        case Kind::None:
            return v;
    }
    return o;
}

uint16_t s5timeBits(int64_t ns) {
    int64_t ms = ns / 1000000;
    if (ms < 0) ms = 0;
    if (ms > 9990000) ms = 9990000;
    static const int64_t base[4] = {10, 100, 1000, 10000};
    int b = 0;
    while (b < 3 && ms / base[b] > 999) ++b;
    const int64_t n = ms / base[b];
    return static_cast<uint16_t>((b << 12) | ((n / 100) << 8) | ((n / 10 % 10) << 4) | (n % 10));
}

int64_t s5timeNs(uint16_t bits) {
    static const int64_t base[4] = {10, 100, 1000, 10000};
    const int64_t n = ((bits >> 8) & 0xF) * 100 + ((bits >> 4) & 0xF) * 10 + (bits & 0xF);
    return n * base[(bits >> 12) & 3] * 1000000;
}

bool isS5Time(const ElemType* t) { return t && std::strcmp(t->name, "S5Time") == 0; }

Val defaultOf(const ElemType& t) {
    Val v;
    v.kind = t.kind;
    v.type = &t;
    // DT and Date_And_Time start at 1990-01-01, LDT at 1970-01-01
    if (t.kind == Kind::Dt && t.unitNs == 1000000) v.i = 7305LL * 86400000000000LL;
    return v;
}

Val fromExport(const Json& value, const ElemType* t) {
    Val v;
    const std::string& k = value["kind"].str();
    if (k == "bool") v = boolVal(value["bool"].b);
    else if (k == "int" || k == "char") v = intVal(value["int"].asInt());
    else if (k == "real") v = realVal(value["real"].number);
    else if (k == "duration") v = timeVal(value["ns"].asInt());
    else if (k == "date") {
        v.kind = Kind::Date;
        v.i = value["days"].asInt();
    } else if (k == "time_of_day") {
        v.kind = Kind::Tod;
        v.i = value["ns"].asInt();
    } else if (k == "date_and_time") {
        v.kind = Kind::Dt;
        v.i = value["ns"].asInt();
    } else if (k == "string") {
        v = stringVal(value["string"].str());
    }
    if (k == "char" && t && t->kind == Kind::String) v = stringVal(value["string"].str());
    if (t && v.kind != Kind::None) return convertTo(v, *t);
    return v;
}

std::string text(const Val& v) {
    char buf[64];
    switch (v.kind) {
        case Kind::None: return "?";
        case Kind::Bool: return v.b ? "TRUE" : "FALSE";
        case Kind::Int:
            if (v.type && v.type->bitString && v.type->bits >= 8) {
                std::snprintf(buf, sizeof buf, "16#%llX", static_cast<unsigned long long>(v.i) &
                                                               (v.type->bits >= 64 ? ~0ULL : ((1ULL << v.type->bits) - 1)));
                return buf;
            }
            return std::to_string(v.i);
        case Kind::Real: {
            for (int p = 6; p <= 17; ++p) {
                std::snprintf(buf, sizeof buf, "%.*g", p, v.r);
                if (std::strtod(buf, nullptr) == v.r) break;
            }
            std::string s = buf;
            if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
            return s;
        }
        case Kind::Time: {
            const bool neg = v.i < 0;
            int64_t ns = neg ? -v.i : v.i;
            std::string s = neg ? "T#-" : "T#";
            const int64_t ms = ns / 1000000;
            ns %= 1000000;
            const int64_t d = ms / 86400000, h = ms / 3600000 % 24, m = ms / 60000 % 60, sec = ms / 1000 % 60,
                          msr = ms % 1000;
            bool any = false;
            auto part = [&](int64_t n, const char* unit) {
                if (!n) return;
                s += (any ? "_" : "") + std::to_string(n) + unit;
                any = true;
            };
            part(d, "D");
            part(h, "H");
            part(m, "M");
            part(sec, "S");
            part(msr, "MS");
            part(ns, "NS");
            if (!any) s += "0MS";
            return s;
        }
        case Kind::Date: return "D#+" + std::to_string(v.i) + "d";
        case Kind::Tod: return "TOD#" + std::to_string(v.i / 1000000) + "ms";
        case Kind::Dt: return "DT#" + std::to_string(v.i / 1000000) + "ms";
        case Kind::String: return "'" + v.s + "'";
        case Kind::Char: return "'" + std::string(1, static_cast<char>(v.i)) + "'";
    }
    return "?";
}

bool sameValue(const Val& a, const Val& b) {
    if (a.kind == Kind::Real || b.kind == Kind::Real) {
        const double x = asReal(a), y = asReal(b);
        return std::fabs(x - y) <= 1e-6 * std::max(1.0, std::max(std::fabs(x), std::fabs(y)));
    }
    if (a.kind == Kind::String || b.kind == Kind::String) return a.s == b.s;
    if (a.kind == Kind::Bool || b.kind == Kind::Bool) return truthy(a) == truthy(b);
    return a.i == b.i;
}

}  // namespace s7forge
