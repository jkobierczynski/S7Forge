// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "json.hpp"
#include "scenario.hpp"
#include "sim.hpp"

#ifndef S7FORGE_VERSION
#define S7FORGE_VERSION "0.1.0"
#endif

using namespace s7forge;

namespace {

void usage(std::ostream& o) {
    o << "usage: s7forge run PROGRAM.json [--scan-ms N] [--instruction-us N] [--for MS] [--set OPERAND=VALUE]...\n"
         "                   [--raise OB]... [--show OPERAND]...\n"
         "       s7forge test [-v] SCENARIO.json...\n"
         "       s7forge info PROGRAM.json\n"
         "       s7forge --version | --help\n"
         "\n"
         "PROGRAM.json is a program file of tiaconv --program-json.\n"
         "An OPERAND is a PLC tag (StartPB), a data block member (\"MotorData\".starts)\n"
         "or an address (%I0.0, %MW20); a VALUE TRUE, 12, 2.5, T#5s, 16#FF or 'text'.\n";
}

Json loadProgram(const std::string& path) {
    try {
        return parseJson(readFile(path));
    } catch (const JsonError& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

int cmdRun(const std::vector<std::string>& a) {
    std::string file;
    double scanMs = -1, forMs = 1000, instrUs = 0;
    std::vector<std::string> raises;
    std::vector<std::pair<std::string, std::string>> sets;
    std::vector<std::string> shows;
    for (size_t k = 0; k < a.size(); ++k) {
        auto next = [&]() -> std::string {
            if (k + 1 >= a.size()) throw std::runtime_error(a[k] + " needs a value");
            return a[++k];
        };
        if (a[k] == "--scan-ms") scanMs = std::atof(next().c_str());
        else if (a[k] == "--for") forMs = std::atof(next().c_str());
        else if (a[k] == "--show") shows.push_back(next());
        else if (a[k] == "--instruction-us") instrUs = std::atof(next().c_str());
        else if (a[k] == "--raise") raises.push_back(next());
        else if (a[k] == "--set") {
            const std::string s = next();
            const size_t eq = s.rfind('=');
            if (eq == std::string::npos) throw std::runtime_error("--set OPERAND=VALUE");
            sets.emplace_back(s.substr(0, eq), s.substr(eq + 1));
        } else if (!a[k].empty() && a[k][0] == '-') throw std::runtime_error("unknown option " + a[k]);
        else if (file.empty()) file = a[k];
        else throw std::runtime_error("more than one program given");
    }
    if (file.empty()) throw std::runtime_error("no program given");
    if (!(forMs >= 0) || forMs > 1e10) throw std::runtime_error("--for: milliseconds, up to 1e10");
    if (scanMs != -1 && !(scanMs >= 0.001 && scanMs <= 60000)) throw std::runtime_error("--scan-ms: from 0.001 to 60000");
    if (!(instrUs >= 0) || instrUs > 1e6) throw std::runtime_error("--instruction-us: from 0 to 1000000");
    Simulator sim(loadProgram(file));
    sim.setInstructionTime(static_cast<int64_t>(instrUs * 1e3 + 0.5));
    if (scanMs > 0) sim.setScanTime(static_cast<int64_t>(scanMs * 1e6));
    if (forMs * 1e6 / static_cast<double>(sim.scanTime()) > 1e8) throw std::runtime_error("--for: more than 1e8 cycles");
    sim.powerOn();
    for (const auto& s : sets) {
        Val v;
        std::string err;
        if (!parseLiteral(s.second, v)) throw std::runtime_error("not a value: " + s.second);
        if (!sim.write(s.first, v, err)) throw std::runtime_error(err);
    }
    for (const auto& r : raises) {
        std::string err;
        if (!sim.raise(r, err)) throw std::runtime_error(err);
    }
    sim.runFor(static_cast<int64_t>(forMs * 1e6));
    std::cout << "t = " << sim.now() / 1000000 << " ms" << (sim.running() ? "" : ", STOP: " + sim.fault()) << "\n";
    for (const auto& s : shows) {
        Val v;
        std::string err;
        if (sim.read(s, v, err)) std::cout << s << " = " << text(v) << "\n";
        else std::cout << s << ": " << err << "\n";
    }
    for (const auto& w : sim.warnings()) std::cerr << "s7forge: warning: " << w << "\n";
    return sim.running() ? 0 : 3;
}

int cmdTest(const std::vector<std::string>& a) {
    bool verbose = false;
    std::vector<std::string> files;
    for (const auto& x : a) {
        if (x == "-v" || x == "--verbose") verbose = true;
        else files.push_back(x);
    }
    if (files.empty()) throw std::runtime_error("no scenario given");
    int failed = 0, errors = 0;
    for (const auto& f : files) {
        try {
            const ScenarioResult r = runScenario(f, std::cout, verbose);
            std::cout << f << ": " << r.steps << " steps, " << r.checks << " checks, " << r.failures << " failed\n";
            if (r.failures) ++failed;
        } catch (const std::exception& e) {
            std::cerr << "s7forge: " << e.what() << "\n";
            ++errors;
        }
    }
    return errors ? 2 : failed ? 1 : 0;
}

int cmdInfo(const std::vector<std::string>& a) {
    if (a.size() != 1) throw std::runtime_error("info PROGRAM.json");
    const Json p = loadProgram(a[0]);
    Simulator sim(p);
    std::cout << "PLC " << sim.plcName() << "\n";
    const Json& rt = p["runtime"];
    auto list = [&](const char* title, const Json& names) {
        std::cout << title << ":";
        for (const auto& n : names.items) std::cout << " " << n.str();
        std::cout << "\n";
    };
    list("startup", rt["startup"]);
    list("program cycle", rt["program_cycle"]);
    list("interrupts", rt["interrupts"]);
    std::cout << p["blocks"].items.size() << " blocks, " << p["data_blocks"].items.size() << " data blocks, "
              << p["tags"].items.size() << " tags\n";
    for (const auto& n : p["not_simulatable"].items) {
        std::cout << "not simulatable: " << n["type"].str() << " " << n["block"].str();
        if (!n["reason"].str().empty()) std::cout << " (" << n["reason"].str() << ")";
        std::cout << "\n";
    }
    for (const auto& w : sim.warnings()) std::cout << "warning: " << w << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage(std::cerr);
        return 2;
    }
    const std::string cmd = args[0];
    args.erase(args.begin());
    try {
        if (cmd == "--help" || cmd == "-h") {
            usage(std::cout);
            return 0;
        }
        if (cmd == "--version") {
            std::cout << "s7forge " << S7FORGE_VERSION << "\n";
            return 0;
        }
        if (cmd == "run") return cmdRun(args);
        if (cmd == "test") return cmdTest(args);
        if (cmd == "info") return cmdInfo(args);
        std::cerr << "s7forge: unknown command " << cmd << "\n";
        usage(std::cerr);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "s7forge: " << e.what() << "\n";
        return 2;
    }
}
