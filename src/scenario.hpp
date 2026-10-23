// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scenarios: a program, inputs set step by step, time run, and the values
// expected after each step. The format is in docs/SCENARIOS.md.
#pragma once

#include <ostream>
#include <string>

#include "json.hpp"
#include "value.hpp"

namespace s7forge {

// A value as written in a scenario or on the command line: true, false, a
// number (with a point: a Real), or a string with an S7 literal: TRUE,
// T#5s, T#1m_30s, 16#FF, 'text', 2.5. False when `text` is none of them.
bool parseLiteral(const std::string& text, Val& out);
bool valueOf(const Json& j, Val& out);

struct ScenarioResult {
    int steps = 0;
    int checks = 0;
    int failures = 0;
};

// Runs the scenario in `file`; reports each failed check (and with
// `verbose` each step) to `out`. Throws std::runtime_error when the file or
// its program cannot be read.
ScenarioResult runScenario(const std::string& file, std::ostream& out, bool verbose);

std::string readFile(const std::string& path);

}  // namespace s7forge
