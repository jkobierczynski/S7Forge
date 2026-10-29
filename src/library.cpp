// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Instructions of the "Basic instructions" library that work on blocks of
// array elements and on strings: MOVE_BLK, UMOVE_BLK, MOVE_BLK_VARIANT,
// FILL_BLK, UFILL_BLK; FIND, REPLACE, INSERT, DELETE; STRG_VAL, VAL_STRG;
// Chars_TO_Strg, Strg_TO_Chars. (LEN, CONCAT, LEFT, RIGHT and MID are with
// the other functions in sim.cpp.) What each does when a parameter is out of
// range is described in DEVELOPMENT.md.
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>
#include <cstdio>
#include <map>
#include <set>

#include "sim.hpp"

namespace s7forge {
namespace {

Simulator::Place at(Var& v) {
    Simulator::Place p;
    p.var = &v;
    return p;
}

std::string up(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const std::set<std::string>& libraryInstructions() {
    static const std::set<std::string> names = {
        "MOVE_BLK", "UMOVE_BLK", "MOVE_BLK_VARIANT", "FILL_BLK", "UFILL_BLK", "FIND", "REPLACE", "INSERT",
        "DELETE", "STRG_VAL", "VAL_STRG", "CHARS_TO_STRG", "STRG_TO_CHARS"};
    return names;
}

// a value as the characters it stands for: a String, or a Char
std::string chars(const Val& v) {
    if (v.kind == Kind::Char) return std::string(1, static_cast<char>(v.i));
    return v.s;
}

// the elements of an array, in the order the CPU keeps them (row by row)
size_t elementCount(const Var& a) { return a.elems.size(); }

}  // namespace

// The array an operand is an element of, and the element's position in it
// (counted from 0, row by row); a whole array is its first element.
bool Simulator::arraySlot(Frame& f, const Json& e, Var*& array, size_t& pos) {
    array = nullptr;
    pos = 0;
    if (e["op"].str() == "index" && !e["args"].items.empty()) {
        Place a = placeOf(f, e["args"].items[0]);
        if (!a.var || a.var->shape != Var::Array) return false;
        const auto& dims = a.var->dims;
        if (e["args"].items.size() != dims.size() + 1) return false;
        int64_t flat = 0;
        for (size_t k = 0; k < dims.size(); ++k) {
            const int64_t i = asInt(eval(f, e["args"].items[k + 1]));
            if (i < dims[k].first || i > dims[k].second) return false;
            flat = flat * (dims[k].second - dims[k].first + 1) + (i - dims[k].first);
        }
        array = a.var;
        pos = static_cast<size_t>(flat);
        return true;
    }
    Place a = placeOf(f, e);
    if (!a.var || a.var->shape != Var::Array) return false;
    array = a.var;
    return true;
}

bool Simulator::libraryCall(Frame& f, const Json& e, const std::string& name, Val& ret) {
    if (!libraryInstructions().count(name)) return false;
    std::map<std::string, const Json*> params;
    std::map<std::string, std::string> ops;
    for (const auto& a : e["args"].items) {
        if (a["args"].items.empty()) continue;
        params[up(a["name"].str())] = &a["args"].items[0];
        ops[up(a["name"].str())] = a["op"].str();
    }
    auto has = [&](const char* n) { return params.count(n) != 0; };
    auto inVal = [&](const char* n) -> Val { return has(n) ? eval(f, *params[n]) : Val{}; };
    // an output, or an in-out given as an operand
    auto put = [&](const char* n, const Val& v) {
        auto it = params.find(n);
        if (it == params.end() || v.kind == Kind::None) return;
        const Json& x = *it->second;
        if (x["op"].str() == "wire") storeWire(f, x["name"].str(), v);
        else if (x["op"].str() != "const") store(placeOf(f, x), v);
    };
    // the type of the operand an output goes to
    auto outType = [&](const char* n) -> const ElemType* {
        auto it = params.find(n);
        if (it == params.end()) return nullptr;
        Place p = placeOf(f, *it->second);
        if (p.var && p.var->shape == Var::Leaf) return p.var->type;
        return p.type;
    };
    auto outMaxLen = [&](const char* n) -> int {
        auto it = params.find(n);
        if (it == params.end()) return 254;
        Place p = placeOf(f, *it->second);
        return p.var ? p.var->maxLen : 254;
    };
    const std::string who = "block " + f.block->name + ": " + name;
    bool ok = true;
    auto fail = [&](const std::string& why) {
        ok = false;
        warn(who + ": " + why + " (ENO FALSE)");
    };

    if (name == "MOVE_BLK" || name == "UMOVE_BLK" || name == "FILL_BLK" || name == "UFILL_BLK") {
        const bool fill = name == "FILL_BLK" || name == "UFILL_BLK";
        const int64_t count = asInt(inVal("COUNT"));
        Var *src = nullptr, *dst = nullptr;
        size_t from = 0, to = 0;
        if (!has("OUT") || !arraySlot(f, *params["OUT"], dst, to)) {
            fail("OUT is not an element of an array");
        } else if (!fill && (!has("IN") || !arraySlot(f, *params["IN"], src, from))) {
            fail("IN is not an element of an array");
        } else if (count < 0 || static_cast<uint64_t>(count) > elementCount(*dst) - to ||
                   (!fill && static_cast<uint64_t>(count) > elementCount(*src) - from)) {
            // nothing is copied when the block does not fit
            fail("COUNT " + std::to_string(count) + " elements do not fit the array");
        } else if (fill) {
            Var* in = nullptr;
            Val v;
            if (has("IN") && (*params["IN"])["op"].str() != "const") {
                Place p = placeOf(f, *params["IN"]);
                if (p.var && p.var->shape != Var::Leaf) in = p.var;
            }
            if (!in && has("IN")) v = eval(f, *params["IN"]);
            for (int64_t k = 0; k < count; ++k) {
                Var& d = *dst->elems[to + static_cast<size_t>(k)];
                if (in) copyValues(d, *in);
                else if (d.shape == Var::Leaf) store(at(d), v);
            }
        } else {
            // the source first: blocks that overlap are copied as they were
            std::vector<std::unique_ptr<Var>> copy;
            for (int64_t k = 0; k < count; ++k) copy.push_back(src->elems[from + static_cast<size_t>(k)]->clone());
            for (int64_t k = 0; k < count; ++k) {
                Var& d = *dst->elems[to + static_cast<size_t>(k)];
                if (d.shape == Var::Leaf) store(at(d), copy[static_cast<size_t>(k)]->value);
                else copyValues(d, *copy[static_cast<size_t>(k)]);
            }
        }
    } else if (name == "MOVE_BLK_VARIANT") {
        // SRC and DEST whole arrays; SRC_INDEX and DEST_INDEX count from 0
        Var *src = nullptr, *dst = nullptr;
        size_t s0 = 0, d0 = 0;
        int16_t code = 0;
        const int64_t count = asInt(inVal("COUNT")), si = asInt(inVal("SRC_INDEX")), di = asInt(inVal("DEST_INDEX"));
        if (!has("SRC") || !arraySlot(f, *params["SRC"], src, s0) || !has("DEST") ||
            !arraySlot(f, *params["DEST"], dst, d0)) {
            code = static_cast<int16_t>(0x80B4);
        } else if (si < 0 || si + count > static_cast<int64_t>(elementCount(*src)) - static_cast<int64_t>(s0)) {
            code = static_cast<int16_t>(0x80B5);
        } else if (di < 0 || di + count > static_cast<int64_t>(elementCount(*dst)) - static_cast<int64_t>(d0)) {
            code = static_cast<int16_t>(0x80B6);
        } else if (count > 0) {
            std::vector<std::unique_ptr<Var>> copy;
            for (int64_t k = 0; k < count; ++k)
                copy.push_back(src->elems[s0 + static_cast<size_t>(si + k)]->clone());
            for (int64_t k = 0; k < count; ++k) {
                Var& d = *dst->elems[d0 + static_cast<size_t>(di + k)];
                if (d.shape == Var::Leaf) store(at(d), copy[static_cast<size_t>(k)]->value);
                else copyValues(d, *copy[static_cast<size_t>(k)]);
            }
        }
        if (code) fail("RET_VAL " + std::to_string(code));
        ret = intVal(code, elemType("Int"));
        put("RET_VAL", ret);
    } else if (name == "FIND") {
        const std::string in1 = chars(inVal("IN1")), in2 = chars(inVal("IN2"));
        const size_t at = in2.empty() ? std::string::npos : in1.find(in2);
        ret = intVal(at == std::string::npos ? 0 : static_cast<int64_t>(at) + 1, elemType("Int"));
        put("OUT", ret);
    } else if (name == "REPLACE" || name == "INSERT" || name == "DELETE") {
        const std::string in = chars(inVal(name == "DELETE" ? "IN" : "IN1"));
        const std::string in2 = chars(inVal("IN2"));
        const int64_t l = name == "INSERT" ? 0 : asInt(inVal("L"));
        const int64_t p = asInt(inVal("P"));
        std::string out;
        if (p < (name == "INSERT" ? 0 : 1) || l < 0) {
            fail("P " + std::to_string(p) + " or L " + std::to_string(l) + " out of range: the result is ''");
        } else if (static_cast<size_t>(p) > in.size()) {
            // past the end: INSERT and REPLACE append IN2, DELETE leaves IN
            out = name == "DELETE" ? in : in + in2;
        } else if (name == "INSERT") {
            out = in.substr(0, static_cast<size_t>(p)) + in2 + in.substr(static_cast<size_t>(p));
        } else {
            const size_t at = static_cast<size_t>(p - 1);
            const size_t n = std::min(static_cast<size_t>(l), in.size() - at);
            out = in.substr(0, at) + (name == "REPLACE" ? in2 : "") + in.substr(at + n);
        }
        if (static_cast<int>(out.size()) > outMaxLen("OUT")) fail("the result is longer than OUT: cut");
        ret = stringVal(out);
        put("OUT", ret);
    } else if (name == "STRG_VAL") {
        // FORMAT: bit 0 exponential, bit 1 a comma as the decimal sign
        const std::string in = chars(inVal("IN"));
        const uint16_t fmt = static_cast<uint16_t>(asInt(inVal("FORMAT")));
        const char point = (fmt & 2) ? ',' : '.';
        const ElemType* t = outType("OUT");
        int64_t p = has("P") ? asInt(inVal("P")) : 1;
        if (fmt > 3) {
            fail("FORMAT " + std::to_string(fmt) + " not known");
        } else if (!t || (t->kind != Kind::Int && t->kind != Kind::Real) || (t->kind == Kind::Int && t->bitString)) {
            fail("OUT is not a number");
        } else if (p < 1 || static_cast<size_t>(p) > in.size()) {
            fail("P " + std::to_string(p) + " is not in IN");
        } else {
            size_t k = static_cast<size_t>(p - 1);
            while (k < in.size() && in[k] == ' ') ++k;
            std::string num;
            if (k < in.size() && (in[k] == '+' || in[k] == '-')) num += in[k++];
            size_t digits = 0;
            while (k < in.size() && std::isdigit(static_cast<unsigned char>(in[k]))) num += in[k++], ++digits;
            // an integer stops at the decimal sign
            if (t->kind == Kind::Real && k < in.size() && in[k] == point) {
                num += '.';
                ++k;
                while (k < in.size() && std::isdigit(static_cast<unsigned char>(in[k]))) num += in[k++], ++digits;
            }
            if (t->kind == Kind::Real && (fmt & 1) && digits && k < in.size() && (in[k] == 'e' || in[k] == 'E')) {
                size_t j = k + 1;
                std::string ex = "e";
                if (j < in.size() && (in[j] == '+' || in[j] == '-')) ex += in[j++];
                size_t exDigits = 0;
                while (j < in.size() && std::isdigit(static_cast<unsigned char>(in[j]))) ex += in[j++], ++exDigits;
                if (exDigits) {
                    num += ex;
                    k = j;
                }
            }
            if (!digits) {
                fail("no number at P " + std::to_string(p));
            } else if (t->kind == Kind::Real) {
                const double v = std::strtod(num.c_str(), nullptr);
                if (!std::isfinite(v) || (t->bits == 32 && std::fabs(v) > 3.402823466e38)) fail("the number is too big for OUT");
                else {
                    put("OUT", realVal(v, t));
                    put("P", intVal(static_cast<int64_t>(k) + 1));
                }
            } else {
                errno = 0;
                const long long v = std::strtoll(num.c_str(), nullptr, 10);
                const int64_t maxv = t->bits >= 64 ? INT64_MAX
                                     : t->isSigned ? (int64_t(1) << (t->bits - 1)) - 1
                                                   : (int64_t(1) << t->bits) - 1;
                const int64_t minv = t->bits >= 64 ? INT64_MIN : t->isSigned ? -(int64_t(1) << (t->bits - 1)) : 0;
                if (errno == ERANGE || v > maxv || v < minv) fail("the number does not fit OUT");
                else {
                    put("OUT", intVal(v, t));
                    put("P", intVal(static_cast<int64_t>(k) + 1));
                }
            }
        }
    } else if (name == "VAL_STRG") {
        // FORMAT: bit 0 exponential, bit 1 a comma, bit 2 a sign also for
        // a positive number
        const Val in = inVal("IN");
        const uint16_t fmt = static_cast<uint16_t>(asInt(inVal("FORMAT")));
        const int64_t size = asInt(inVal("SIZE")), prec = asInt(inVal("PREC"));
        int64_t p = has("P") ? asInt(inVal("P")) : 1;
        const int maxLen = outMaxLen("OUT");
        std::string cur;
        if (has("OUT")) cur = chars(eval(f, *params["OUT"]));
        if (fmt > 7) {
            fail("FORMAT " + std::to_string(fmt) + " not known");
        } else if (in.kind != Kind::Int && in.kind != Kind::Real) {
            fail("IN is not a number");
        } else if (size < 1 || p < 1 || p - 1 + size > maxLen || prec < 0 || prec > 31) {
            fail("SIZE, P or PREC out of range");
        } else {
            std::string body;
            bool negative = false;
            if (in.kind == Kind::Real) {
                negative = std::signbit(in.r) && in.r != 0;
                char b[512];
                std::snprintf(b, sizeof b, (fmt & 1) ? "%.*E" : "%.*f", static_cast<int>(prec), std::fabs(in.r));
                body = b;
            } else {
                // a whole number: PREC places after the decimal sign
                negative = in.i < 0;
                const uint64_t mag = negative ? 0 - static_cast<uint64_t>(in.i) : static_cast<uint64_t>(in.i);
                body = std::to_string(mag);
                if (prec > 0) {
                    if (static_cast<int64_t>(body.size()) <= prec) body.insert(0, static_cast<size_t>(prec) - body.size() + 1, '0');
                    body.insert(body.size() - static_cast<size_t>(prec), ".");
                }
            }
            if (fmt & 2) std::replace(body.begin(), body.end(), '.', ',');
            if (negative) body.insert(0, "-");
            else if (fmt & 4) body.insert(0, "+");
            std::string field;
            if (static_cast<int64_t>(body.size()) > size) {
                field.assign(static_cast<size_t>(size), '*');
                fail("the number does not fit SIZE " + std::to_string(size));
            } else {
                field = std::string(static_cast<size_t>(size) - body.size(), ' ') + body;
            }
            // written into OUT from P; before P the string is filled with
            // blanks where it is shorter
            if (cur.size() < static_cast<size_t>(p - 1)) cur.resize(static_cast<size_t>(p - 1), ' ');
            cur.replace(static_cast<size_t>(p - 1), std::min(cur.size() - static_cast<size_t>(p - 1), field.size()), field);
            put("OUT", stringVal(cur));
            put("P", intVal(p + size));
        }
    } else if (name == "CHARS_TO_STRG" || name == "STRG_TO_CHARS") {
        // pChars: the position in the array counted from its first element
        Var* arr = nullptr;
        size_t base = 0;
        const int64_t pc = asInt(inVal("PCHARS"));
        if (!has("CHARS") || !arraySlot(f, *params["CHARS"], arr, base) || arr->elems.empty() ||
            arr->elems[0]->shape != Var::Leaf || !arr->elems[0]->type ||
            (arr->elems[0]->type->kind != Kind::Char && !(arr->elems[0]->type->kind == Kind::Int && arr->elems[0]->type->bits == 8))) {
            fail("Chars is not an array of Char or Byte");
        } else if (pc < 0 || static_cast<size_t>(pc) >= arr->elems.size() - base) {
            fail("pChars " + std::to_string(pc) + " is not in the array");
        } else if (name == "CHARS_TO_STRG") {
            const int64_t cnt = asInt(inVal("CNT"));
            const size_t avail = arr->elems.size() - base - static_cast<size_t>(pc);
            std::string s;
            if (cnt < 0 || static_cast<uint64_t>(cnt) > avail) {
                fail("Cnt " + std::to_string(cnt) + " is more than the array holds");
            } else {
                // Cnt 0: all up to the end of the array or the first $00
                const size_t n = cnt == 0 ? avail : static_cast<size_t>(cnt);
                for (size_t k = 0; k < n; ++k) {
                    const int64_t c = arr->elems[base + static_cast<size_t>(pc) + k]->value.i & 0xFF;
                    if (cnt == 0 && c == 0) break;
                    s += static_cast<char>(c);
                }
                if (static_cast<int>(s.size()) > outMaxLen("STRG")) fail("the string is longer than Strg: cut");
                put("STRG", stringVal(s));
            }
        } else {
            const std::string s = chars(inVal("STRG"));
            const size_t avail = arr->elems.size() - base - static_cast<size_t>(pc);
            if (s.size() > avail) {
                fail("the string is longer than the array from pChars");
            } else {
                for (size_t k = 0; k < s.size(); ++k) {
                    Var& d = *arr->elems[base + static_cast<size_t>(pc) + k];
                    d.value = convertTo(intVal(static_cast<unsigned char>(s[k])), *d.type);
                }
                put("CNT", intVal(static_cast<int64_t>(s.size())));
            }
        }
    }
    lastEno_ = ok;
    put("ENO", boolVal(ok));
    return true;
}

}  // namespace s7forge
