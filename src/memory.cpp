// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace s7forge {

Var* Var::member(const std::string& name) {
    for (auto& m : members)
        if (m.first == name) return m.second.get();
    return nullptr;
}

const Var* Var::member(const std::string& name) const {
    for (const auto& m : members)
        if (m.first == name) return m.second.get();
    return nullptr;
}

Var* Var::element(const std::vector<int64_t>& index) {
    if (shape != Array || index.size() != dims.size()) return nullptr;
    size_t flat = 0;
    for (size_t k = 0; k < dims.size(); ++k) {
        const int64_t lo = dims[k].first, hi = dims[k].second;
        if (index[k] < lo || index[k] > hi) return nullptr;
        flat = flat * static_cast<size_t>(hi - lo + 1) + static_cast<size_t>(index[k] - lo);
    }
    return flat < elems.size() ? elems[flat].get() : nullptr;
}

void Var::reset(bool retainedToo) {
    if (retain && !retainedToo) return;
    if (shape == Leaf) value = start;
    for (auto& m : members) m.second->reset(retainedToo);
    for (auto& e : elems) e->reset(retainedToo);
}

std::unique_ptr<Var> Var::clone() const {
    auto v = std::make_unique<Var>();
    v->shape = shape;
    v->dataType = dataType;
    v->type = type;
    v->maxLen = maxLen;
    v->value = value;
    v->start = start;
    v->retain = retain;
    v->section = section;
    for (const auto& m : members) v->members.emplace_back(m.first, m.second->clone());
    for (const auto& e : elems) v->elems.push_back(e->clone());
    v->dims = dims;
    return v;
}

void copyValues(Var& to, const Var& from) {
    if (to.shape == Var::Leaf && from.shape == Var::Leaf) {
        to.value = to.type ? convertTo(from.value, *to.type, to.maxLen) : from.value;
        return;
    }
    for (auto& m : to.members)
        if (const Var* f = from.member(m.first)) copyValues(*m.second, *f);
    for (size_t k = 0; k < to.elems.size() && k < from.elems.size(); ++k) copyValues(*to.elems[k], *from.elems[k]);
}

namespace {


std::unique_ptr<Var> leaf(const std::string& dataType, const Json& type) {
    auto v = std::make_unique<Var>();
    v->dataType = dataType;
    v->type = elemType(type["kind"].str() == "string" ? type["name"].str() : type["name"].str());
    if (type["kind"].str() == "string" && type["length"].type == Json::Number)
        v->maxLen = static_cast<int>(type["length"].asInt());
    if (v->type) v->start = v->value = defaultOf(*v->type);
    return v;
}

// A variable of the type `type`; `members` are the members of one element
// (of the structure, or of one element of an array).
std::unique_ptr<Var> build(const std::string& dataType, const Json& type, const Json& members, size_t& budget,
                           int depth) {
    if (depth > 64) throw std::runtime_error("types nested too deeply in " + dataType);
    if (budget == 0) throw std::runtime_error("the program has more variables than S7Forge holds");
    --budget;
    const std::string& kind = type["kind"].str();
    if (kind == "array") {
        auto v = std::make_unique<Var>();
        v->shape = Var::Array;
        v->dataType = dataType;
        size_t count = 1;
        for (const auto& d : type["dims"].items) {
            if (d["low"].type != Json::Number || d["high"].type != Json::Number)
                throw std::runtime_error("an array with bounds not known: " + dataType);
            const int64_t lo = d["low"].asInt(), hi = d["high"].asInt();
            if (hi < lo) throw std::runtime_error("an array with no elements: " + dataType);
            v->dims.emplace_back(lo, hi);
            count *= static_cast<size_t>(hi - lo + 1);
            if (count > budget) throw std::runtime_error("an array larger than S7Forge holds: " + dataType);
        }
        const Json& element = type["element"];
        for (size_t k = 0; k < count; ++k)
            v->elems.push_back(build(element["name"].str(), element, members, budget, depth + 1));
        return v;
    }
    if (kind == "elementary" || kind == "string") return leaf(dataType, type);
    // a structure, PLC data type, system type or multi-instance
    auto v = std::make_unique<Var>();
    v->shape = Var::Struct;
    v->dataType = dataType;
    for (const auto& m : members.items) {
        auto child = buildVar(m, budget);
        v->members.emplace_back(m["name"].str(), std::move(child));
    }
    return v;
}

}  // namespace

std::unique_ptr<Var> buildVar(const Json& member, size_t& budget) {
    auto v = build(member["data_type"].str(), member["type"], member["members"], budget, 0);
    v->section = member["section"].str();
    v->retain = member["retain"].b;
    // the start value, of a leaf; the members of a structure have their own
    if (v->shape == Var::Leaf && v->type && member["start"].type == Json::Object) {
        const Val s = fromExport(member["start"], v->type);
        if (s.kind != Kind::None) v->start = v->value = convertTo(s, *v->type, v->maxLen);
    }
    return v;
}

std::unique_ptr<Var> buildStruct(const Json& members, size_t& budget, const std::vector<std::string>& sections) {
    auto v = std::make_unique<Var>();
    v->shape = Var::Struct;
    for (const auto& m : members.items) {
        if (!sections.empty()) {
            bool keep = false;
            for (const auto& s : sections) keep = keep || m["section"].str() == s;
            if (!keep) continue;
        }
        v->members.emplace_back(m["name"].str(), buildVar(m, budget));
    }
    return v;
}

Address parseAddress(const std::string& text) {
    Address a;
    std::string t = text;
    if (!t.empty() && t[0] == '%') t = t.substr(1);
    if (t.empty()) return a;
    const char area = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    if (area != 'I' && area != 'Q' && area != 'M') return a;
    size_t p = 1;
    char size = 'X';
    if (p < t.size() && std::isalpha(static_cast<unsigned char>(t[p]))) {
        size = static_cast<char>(std::toupper(static_cast<unsigned char>(t[p])));
        ++p;
    }
    if (size != 'X' && size != 'B' && size != 'W' && size != 'D' && size != 'L') return a;
    char* end = nullptr;
    const unsigned long byte = std::strtoul(t.c_str() + p, &end, 10);
    if (end == t.c_str() + p || byte > 65535) return a;
    int bit = 0;
    if (*end == '.') {
        bit = static_cast<int>(std::strtol(end + 1, &end, 10));
        if (bit < 0 || bit > 7) return a;
    } else if (size == 'X') {
        return a;  // a bit needs one
    }
    if (*end) return a;
    a.area = area;
    a.size = size;
    a.byte = static_cast<uint32_t>(byte);
    a.bit = bit;
    return a;
}

namespace {

uint64_t readBytes(const Area& a, uint32_t at, int n) {
    uint64_t v = 0;
    for (int k = 0; k < n; ++k) v = (v << 8) | (at + k < a.bytes.size() ? a.bytes[at + k] : 0);
    return v;
}

void writeBytes(Area& a, uint32_t at, int n, uint64_t v) {
    for (int k = n - 1; k >= 0; --k) {
        if (at + k < a.bytes.size()) a.bytes[at + k] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

}  // namespace

Val readArea(const Area& a, const Address& at, const ElemType& t) {
    if (t.kind == Kind::Bool) {
        const uint8_t byte = at.byte < a.bytes.size() ? a.bytes[at.byte] : 0;
        return boolVal((byte >> at.bit) & 1);
    }
    const int n = t.bits / 8;
    if (n <= 0) return defaultOf(t);
    const uint64_t raw = readBytes(a, at.byte, n);
    if (isS5Time(&t)) return timeVal(s5timeNs(static_cast<uint16_t>(raw)), &t);
    if (t.kind == Kind::Real) {
        if (n == 4) {
            float f;
            const uint32_t r32 = static_cast<uint32_t>(raw);
            std::memcpy(&f, &r32, 4);
            return realVal(f, &t);
        }
        double d;
        std::memcpy(&d, &raw, 8);
        return realVal(d, &t);
    }
    Val v = intVal(static_cast<int64_t>(raw), &t);
    v = convertTo(v, t.kind == Kind::Time || t.kind == Kind::Tod || t.kind == Kind::Dt ? *elemType(n == 8 ? "LInt" : "DInt") : t);
    if (t.kind == Kind::Time || t.kind == Kind::Tod || t.kind == Kind::Dt) {
        v.kind = t.kind;
        v.i *= t.unitNs ? t.unitNs : 1;
    }
    v.kind = t.kind;
    v.type = &t;
    return v;
}

void writeArea(Area& a, const Address& at, const ElemType& t, const Val& v0) {
    const Val v = convertTo(v0, t);
    if (t.kind == Kind::Bool) {
        if (at.byte >= a.bytes.size()) return;
        if (v.b) a.bytes[at.byte] = static_cast<uint8_t>(a.bytes[at.byte] | (1u << at.bit));
        else a.bytes[at.byte] = static_cast<uint8_t>(a.bytes[at.byte] & ~(1u << at.bit));
        return;
    }
    const int n = t.bits / 8;
    if (n <= 0) return;
    uint64_t raw = 0;
    if (t.kind == Kind::Real) {
        if (n == 4) {
            const float f = static_cast<float>(v.r);
            uint32_t r32;
            std::memcpy(&r32, &f, 4);
            raw = r32;
        } else {
            std::memcpy(&raw, &v.r, 8);
        }
    } else if (isS5Time(&t)) {
        raw = s5timeBits(v.i);
    } else if (t.kind == Kind::Time || t.kind == Kind::Tod || t.kind == Kind::Dt) {
        raw = static_cast<uint64_t>(v.i / (t.unitNs ? t.unitNs : 1));
    } else {
        raw = static_cast<uint64_t>(v.i);
    }
    writeBytes(a, at.byte, n, raw);
}

}  // namespace s7forge
