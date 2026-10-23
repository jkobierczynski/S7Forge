// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "scenario.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "sim.hpp"

namespace s7forge {
namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// T#1d_2h_3m_4s_5ms (the part after T#), in ns; false when not one
bool parseDuration(const std::string& t0, int64_t& ns) {
    std::string t = upper(t0);
    bool neg = false;
    size_t p = 0;
    if (p < t.size() && (t[p] == '-' || t[p] == '+')) neg = t[p++] == '-';
    if (p >= t.size()) return false;
    long double total = 0;
    while (p < t.size()) {
        if (t[p] == '_') {
            ++p;
            continue;
        }
        const size_t start = p;
        while (p < t.size() && (std::isdigit(static_cast<unsigned char>(t[p])) || t[p] == '.')) ++p;
        if (p == start) return false;
        const long double n = std::strtold(t.substr(start, p - start).c_str(), nullptr);
        std::string unit;
        while (p < t.size() && std::isalpha(static_cast<unsigned char>(t[p]))) unit += t[p++];
        long double f;
        if (unit == "D") f = 86400e9L;
        else if (unit == "H") f = 3600e9L;
        else if (unit == "M") f = 60e9L;
        else if (unit == "S") f = 1e9L;
        else if (unit == "MS") f = 1e6L;
        else if (unit == "US") f = 1e3L;
        else if (unit == "NS") f = 1;
        else return false;
        total += n * f;
    }
    if (total > 9.2e18L) return false;
    ns = static_cast<int64_t>(total + 0.5L);
    if (neg) ns = -ns;
    return true;
}

}  // namespace

bool parseLiteral(const std::string& text0, Val& out) {
    std::string text = text0;
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
    if (text.empty()) return false;
    const std::string u = upper(text);
    if (u == "TRUE" || u == "FALSE") {
        out = boolVal(u == "TRUE");
        return true;
    }
    if (text.size() >= 2 && text.front() == '\'' && text.back() == '\'') {
        out = stringVal(text.substr(1, text.size() - 2));
        return true;
    }
    for (const char* prefix : {"LT#", "T#", "TIME#", "LTIME#"}) {
        const std::string pr = prefix;
        if (u.compare(0, pr.size(), pr) == 0) {
            int64_t ns;
            if (!parseDuration(u.substr(pr.size()), ns)) return false;
            out = timeVal(ns);
            return true;
        }
    }
    const size_t hash = u.find('#');
    if (hash != std::string::npos) {
        const std::string base = u.substr(0, hash);
        const int radix = base == "16" ? 16 : base == "8" ? 8 : base == "2" ? 2 : 0;
        if (!radix) return false;
        std::string digits;
        for (char c : u.substr(hash + 1))
            if (c != '_') digits += c;
        char* end = nullptr;
        errno = 0;
        const unsigned long long v = std::strtoull(digits.c_str(), &end, radix);
        if (digits.empty() || *end || !std::isxdigit(static_cast<unsigned char>(digits[0])) || errno == ERANGE) return false;
        out = intVal(static_cast<int64_t>(v));
        return true;
    }
    char* end = nullptr;
    if (u.find_first_of(".E") != std::string::npos) {
        const double r = std::strtod(text.c_str(), &end);
        if (*end || !std::isfinite(r)) return false;
        out = realVal(r);
        return true;
    }
    errno = 0;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    if (*end || errno == ERANGE) return false;
    out = intVal(v);
    return true;
}

bool valueOf(const Json& j, Val& out) {
    switch (j.type) {
        case Json::Bool: out = boolVal(j.b); return true;
        case Json::Number: out = j.integer ? intVal(j.i) : realVal(j.number); return true;
        case Json::String: return parseLiteral(j.s, out);
        default: return false;
    }
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

namespace {

std::string dirOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

constexpr int64_t kMaxCycles = 100000000;  // in one step

int64_t msToNs(const Json& j, const std::string& what) {
    if (j.type != Json::Number || !(j.number >= 0) || j.number > 1e10)
        throw std::runtime_error(what + " must be a number of milliseconds");
    return static_cast<int64_t>(j.number * 1e6 + 0.5);
}

}  // namespace

ScenarioResult runScenario(const std::string& file, std::ostream& out, bool verbose) {
    Json sc;
    try {
        sc = parseJson(readFile(file));
    } catch (const JsonError& e) {
        throw std::runtime_error(file + ": " + e.what());
    }
    if (sc["program"].type != Json::String) throw std::runtime_error(file + ": no \"program\"");
    std::string prog = sc["program"].str();
    if (prog.empty() || (prog[0] != '/' && prog.find(':') == std::string::npos)) prog = dirOf(file) + prog;
    Json program;
    try {
        program = parseJson(readFile(prog));
    } catch (const JsonError& e) {
        throw std::runtime_error(prog + ": " + e.what());
    }
    Simulator sim(std::move(program));
    if (sc["scan_ms"].type != Json::Null) {
        const int64_t ns = msToNs(sc["scan_ms"], "scan_ms");
        if (ns < 1000 || ns > 60000000000LL) throw std::runtime_error(file + ": scan_ms is from 0.001 to 60000");
        sim.setScanTime(ns);
    }
    sim.powerOn();
    ScenarioResult res;
    int n = 0;
    for (const auto& step : sc["steps"].items) {
        ++n;
        ++res.steps;
        const std::string label = "step " + std::to_string(n) +
                                  (step["comment"].str().empty() ? std::string() : " (" + step["comment"].str() + ")");
        if (verbose) out << label << "\n";
        for (const auto& s : step["set"].members) {
            Val v;
            std::string err;
            if (!valueOf(s.second, v)) throw std::runtime_error(file + ", " + label + ": not a value for " + s.first);
            if (!sim.write(s.first, v, err)) throw std::runtime_error(file + ", " + label + ": " + err);
        }
        const std::string& restart = step["restart"].str();
        if (restart == "warm") sim.warmRestart();
        else if (restart == "power") sim.powerOn();
        else if (!restart.empty()) throw std::runtime_error(file + ", " + label + ": restart is \"warm\" or \"power\"");
        if (step["scans"].type != Json::Null) {
            const int64_t cycles = step["scans"].asInt(-1);
            if (step["scans"].type != Json::Number || cycles < 0 || cycles > kMaxCycles)
                throw std::runtime_error(file + ", " + label + ": scans is a number up to " + std::to_string(kMaxCycles));
            for (int64_t k = 0; k < cycles; ++k) sim.scan();
        }
        if (step["run_ms"].type != Json::Null) {
            const int64_t ns = msToNs(step["run_ms"], "run_ms");
            if (ns / sim.scanTime() > kMaxCycles)
                throw std::runtime_error(file + ", " + label + ": run_ms is more than " + std::to_string(kMaxCycles) +
                                         " cycles");
            sim.runFor(ns);
        }
        const bool faultExpected = step["expect_fault"].b;
        if (faultExpected) {
            ++res.checks;
            if (sim.running()) {
                ++res.failures;
                out << file << ", " << label << ": the CPU is in RUN, expected STOP\n";
            }
        } else if (!sim.running()) {
            out << file << ", " << label << ": the CPU went to STOP: " << sim.fault() << "\n";
            ++res.failures;
            break;
        }
        for (const auto& e : step["expect"].members) {
            ++res.checks;
            Val want, got;
            std::string err;
            if (!valueOf(e.second, want)) throw std::runtime_error(file + ", " + label + ": not a value for " + e.first);
            if (!sim.read(e.first, got, err)) throw std::runtime_error(file + ", " + label + ": " + err);
            if (!sameValue(got, want)) {
                ++res.failures;
                out << file << ", " << label << ": " << e.first << " is " << text(got) << ", expected " << text(want)
                    << "\n";
            } else if (verbose) {
                out << "  " << e.first << " = " << text(got) << "\n";
            }
        }
    }
    if (verbose)
        for (const auto& w : sim.warnings()) out << "warning: " << w << "\n";
    return res;
}

}  // namespace s7forge
