// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Values and the elementary data types of S7: what a variable holds, and
// how a value becomes one of a given type when it is stored.
#pragma once

#include <cstdint>
#include <string>

#include "json.hpp"

namespace s7forge {

enum class Kind { None, Bool, Int, Real, Time, Date, Tod, Dt, String, Char };

// An elementary data type: Int, Word, Real, Time, String, ...
struct ElemType {
    const char* name;
    Kind kind;
    int bits;           // the size; 0 for String
    bool isSigned;      // Int: SInt, Int, DInt, LInt
    bool bitString;     // Byte, Word, DWord, LWord (and the hardware numbers)
    int64_t unitNs;     // Time: what one unit of the stored number is (ms for Time, ns for LTime)
};

// The type by its name as TIA Portal writes it, ignoring case; nullptr for
// one that is not elementary.
const ElemType* elemType(const std::string& name);

// A value while the program runs. Times, dates and times of day are kept in
// nanoseconds (a Date in days since 1990-01-01); a Char as its code.
struct Val {
    Kind kind = Kind::None;
    bool b = false;
    int64_t i = 0;
    double r = 0;
    std::string s;
    const ElemType* type = nullptr;  // where known: the type the value is of
};

Val boolVal(bool b);
Val intVal(int64_t i, const ElemType* type = nullptr);
Val realVal(double r, const ElemType* type = nullptr);
Val timeVal(int64_t ns, const ElemType* type = nullptr);
Val stringVal(const std::string& s);

// The value as a condition: a Bool, or a number other than 0.
bool truthy(const Val& v);
// The value as a whole number (a Real truncated, a Bool 0 or 1).
int64_t asInt(const Val& v);
double asReal(const Val& v);
// a double as a whole number, truncated and limited to the range of int64
// (NaN gives 0)
int64_t clampToInt64(double r);

// The value as it is when stored in a variable of type `t`: a whole
// number wrapped to its size, a Real rounded to single precision, a Time
// to milliseconds, a String cut to `maxLen`.
Val convertTo(const Val& v, const ElemType& t, int maxLen = 254);

// S5Time as the CPU keeps it in 16 bits: a time base (10 ms, 100 ms, 1 s,
// 10 s) in bits 12-13 and three BCD digits; the finest base that holds the
// time, at most 9990 s
uint16_t s5timeBits(int64_t ns);
int64_t s5timeNs(uint16_t bits);
bool isS5Time(const ElemType* t);

// The default value of a type (0, FALSE, '', T#0s; DT#1990-01-01).
Val defaultOf(const ElemType& t);

// A value from a parsed value of tiaconv's export ({"kind", "int", "real",
// "ns", "days", "string", "bool"}), for a variable of type `t`.
Val fromExport(const Json& value, const ElemType* t);

// For messages and traces: TRUE, 42, 1.5, T#5S, 'abc'.
std::string text(const Val& v);

// Whether two values are equal for a comparison in a scenario.
bool sameValue(const Val& a, const Val& b);

}  // namespace s7forge
