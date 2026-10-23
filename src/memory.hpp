// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The memory of the simulated CPU: variables (data blocks, instances, the
// temporary data of a call) as trees built from the members tiaconv
// exports, and the areas I, Q and M that the PLC tags are addresses in.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"
#include "value.hpp"

namespace s7forge {

// A variable: a value of an elementary type, a structure, or an array.
struct Var {
    enum Shape { Leaf, Struct, Array } shape = Leaf;
    std::string dataType;               // as declared: Int, "Motor", TON_TIME, Array[0..9] of Real
    const ElemType* type = nullptr;     // a leaf: its elementary type (nullptr: one not simulated)
    int maxLen = 254;                   // a String
    Val value;
    Val start;                          // the start value (the default when none is stored)
    bool retain = false;
    std::string section;                // Input, Output, InOut, Static, Temp, Constant, Return
    std::vector<std::pair<std::string, std::unique_ptr<Var>>> members;  // a structure
    std::vector<std::unique_ptr<Var>> elems;                             // an array, row by row
    std::vector<std::pair<int64_t, int64_t>> dims;                       // its bounds

    Var* member(const std::string& name);
    const Var* member(const std::string& name) const;
    // an element by its indices; nullptr when out of bounds
    Var* element(const std::vector<int64_t>& index);
    // back to the start values; with `retainedToo` false, retained values stay
    void reset(bool retainedToo);
    std::unique_ptr<Var> clone() const;
};

// Copies the values of `from` into `to`, member by member where the names
// match and element by element: assigning a structure or array.
void copyValues(Var& to, const Var& from);

// A variable from a member of tiaconv's export (its "type", "members",
// "start", "retain", "section"). Arrays are expanded into their elements.
std::unique_ptr<Var> buildVar(const Json& member, size_t& budget);
// A structure of the members of a list; `sections`, when not empty, keeps
// only members of those sections.
std::unique_ptr<Var> buildStruct(const Json& members, size_t& budget,
                                 const std::vector<std::string>& sections = {});

// An area of the CPU: I, Q or M, byte addressed.
struct Area {
    char letter;
    std::vector<uint8_t> bytes;
    explicit Area(char l, size_t size = 65536) : letter(l), bytes(size, 0) {}
};

// Where a PLC tag is: %I0.0, %MW20, %QD4, ...
struct Address {
    char area = 0;     // 'I', 'Q', 'M'; 0 when not an address
    char size = 'X';   // X bit, B byte, W word, D double word, L long word
    uint32_t byte = 0;
    int bit = 0;
};
Address parseAddress(const std::string& text);

// A value of type `t` read from or written to an area at an address, as
// the CPU keeps it there (big-endian; Real as IEEE 754; Time in ms).
Val readArea(const Area& a, const Address& at, const ElemType& t);
void writeArea(Area& a, const Address& at, const ElemType& t, const Val& v);

}  // namespace s7forge
