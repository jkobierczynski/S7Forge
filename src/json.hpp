// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small JSON reader: enough for the program files tiaconv writes and the
// scenario files of S7Forge. Objects keep their keys in the order written.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace s7forge {

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double number = 0;
    bool integer = false;  // written without a point or exponent
    int64_t i = 0;         // the value when `integer`
    std::string s;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    bool isNull() const { return type == Null; }
    const Json* get(const char* k) const {
        for (const auto& m : members)
            if (m.first == k) return &m.second;
        return nullptr;
    }
    const Json* get(const std::string& k) const { return get(k.c_str()); }
    // the member, or a null value when there is none
    const Json& operator[](const char* k) const {
        static const Json none;
        const Json* v = get(k);
        return v ? *v : none;
    }
    const Json& operator[](const std::string& k) const { return (*this)[k.c_str()]; }
    int64_t asInt(int64_t fallback = 0) const {
        if (type != Number) return fallback;
        if (integer) return i;
        if (!(number == number)) return fallback;  // NaN
        if (number >= 9223372036854775807.0) return INT64_MAX;
        if (number <= -9223372036854775808.0) return INT64_MIN;
        return static_cast<int64_t>(number);
    }
    const std::string& str() const { return s; }
};

class JsonError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Json parseJson(const std::string& text);

}  // namespace s7forge
