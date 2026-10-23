// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests: the JSON reader, values, the memory areas, literals, and the
// interpreter on small programs written here as tiaconv would export them.
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "json.hpp"
#include "memory.hpp"
#include "scenario.hpp"
#include "sim.hpp"
#include "value.hpp"

using namespace s7forge;

namespace {

int checks = 0, failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++checks;                                                                      \
        if (!(cond)) {                                                                 \
            ++failures;                                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << ": failed: " #cond << "\n"; \
        }                                                                              \
    } while (0)

template <class A, class B>
void checkEq(const A& a, const B& b, const char* what, int line) {
    ++checks;
    if (!(a == b)) {
        ++failures;
        std::cerr << __FILE__ << ":" << line << ": " << what << ": got " << a << ", expected " << b << "\n";
    }
}
#define EQ(a, b) checkEq((a), (b), #a, __LINE__)

bool throws(const std::string& text) {
    try {
        parseJson(text);
    } catch (const JsonError&) {
        return true;
    }
    return false;
}

void testJson() {
    Json j = parseJson(R"({"a": [1, 2.5, -3e2, true, null, "x\u00e9\ud83d\ude00"], "b": {"c": 9007199254740993}})");
    EQ(j["a"].items.size(), 6u);
    EQ(j["a"].items[0].asInt(), 1);
    CHECK(j["a"].items[0].integer);
    CHECK(!j["a"].items[1].integer);
    EQ(j["a"].items[2].number, -300.0);
    CHECK(j["a"].items[3].b);
    CHECK(j["a"].items[4].isNull());
    EQ(j["a"].items[5].s, std::string("x\xC3\xA9\xF0\x9F\x98\x80"));
    EQ(j["b"]["c"].asInt(), 9007199254740993LL);
    CHECK(j["missing"]["deeper"].isNull());
    CHECK(throws("{"));
    CHECK(throws("[1,]"));
    CHECK(throws("{\"a\" 1}"));
    CHECK(throws("\"\\ud800\""));
    CHECK(throws("01"));
    CHECK(throws("[1] x"));
    CHECK(throws("1-2"));
    CHECK(throws("-"));
    CHECK(throws("1."));
    CHECK(throws("\"\\udc00\""));
    CHECK(throws(std::string(600, '[') + std::string(600, ']')));
}

void testValues() {
    const ElemType* i = elemType("int");
    const ElemType* usint = elemType("USInt");
    const ElemType* real = elemType("Real");
    const ElemType* time = elemType("Time");
    const ElemType* word = elemType("Word");
    CHECK(i && usint && real && time && word);
    CHECK(!elemType("TON_TIME"));
    EQ(convertTo(intVal(32768), *i).i, -32768);
    EQ(convertTo(intVal(-1), *usint).i, 255);
    EQ(convertTo(intVal(70000), *word).i, 70000 & 0xFFFF);
    EQ(convertTo(realVal(2.5), *i).i, 2);   // round half to even
    EQ(convertTo(realVal(3.5), *i).i, 4);
    EQ(convertTo(realVal(-2.5), *i).i, -2);
    EQ(convertTo(realVal(0.1), *real).r, static_cast<double>(0.1f));
    EQ(convertTo(timeVal(1500999999), *time).i, 1500000000);  // Time keeps ms
    EQ(text(timeVal(90500000000LL)), std::string("T#1M_30S_500MS"));
    EQ(text(convertTo(intVal(255), *word)), std::string("16#FF"));
    EQ(text(realVal(1)), std::string("1.0"));
    EQ(text(boolVal(true)), std::string("TRUE"));
    CHECK(sameValue(realVal(0.1f), realVal(0.1)));
    CHECK(!sameValue(intVal(1), intVal(2)));
    CHECK(sameValue(boolVal(true), intVal(1)));
    EQ(asInt(realVal(2.9)), 2);
    CHECK(truthy(intVal(3)));
    CHECK(!truthy(boolVal(false)));
}

void testS5Time() {
    EQ(int(s5timeBits(10000000000LL)), 0x1100);   // S5T#10S: 100 ms base, 100
    EQ(int(s5timeBits(990000000LL)), 0x0099);     // 990 ms in 10 ms
    EQ(int(s5timeBits(2500000000LL)), 0x0250);    // 2.5 s in 10 ms
    EQ(s5timeNs(0x3999), 9990000000000LL);
    EQ(s5timeNs(0x2010), 10000000000LL);
    Area m('M');
    writeArea(m, parseAddress("%MW0"), *elemType("S5Time"), timeVal(10000000000LL));
    EQ(readArea(m, parseAddress("%MW0"), *elemType("Word")).i, 0x1100);
    EQ(readArea(m, parseAddress("%MW0"), *elemType("S5Time")).i, 10000000000LL);
    EQ(convertTo(timeVal(12345000000LL), *elemType("S5Time")).i, 12300000000LL);  // 100 ms base
}

void testAreas() {
    Area m('M');
    const Address w = parseAddress("%MW20");
    EQ(w.area, 'M');
    EQ(w.size, 'W');
    EQ(w.byte, 20u);
    writeArea(m, w, *elemType("Int"), intVal(-2));
    EQ(int(m.bytes[20]), 0xFF);
    EQ(int(m.bytes[21]), 0xFE);
    EQ(readArea(m, w, *elemType("Int")).i, -2);
    EQ(readArea(m, w, *elemType("Word")).i, 0xFFFE);
    const Address b = parseAddress("M21.0");
    EQ(b.bit, 0);
    CHECK(!readArea(m, b, *elemType("Bool")).b);
    CHECK(readArea(m, parseAddress("%M21.1"), *elemType("Bool")).b);
    writeArea(m, parseAddress("%MD4"), *elemType("Real"), realVal(1.0));
    EQ(int(m.bytes[4]), 0x3F);
    EQ(int(m.bytes[5]), 0x80);
    EQ(readArea(m, parseAddress("%MD4"), *elemType("Real")).r, 1.0);
    writeArea(m, parseAddress("%MD8"), *elemType("Time"), timeVal(5000000000LL));
    EQ(readArea(m, parseAddress("%MD8"), *elemType("DInt")).i, 5000);
    EQ(readArea(m, parseAddress("%MD8"), *elemType("Time")).i, 5000000000LL);
    CHECK(!parseAddress("%M1").area);      // a bit needs one
    CHECK(!parseAddress("%M1.8").area);
    CHECK(!parseAddress("%DB1.DBX0.0").area);
    CHECK(!parseAddress("%MW70000").area);
}

void testLiterals() {
    Val v;
    CHECK(parseLiteral("T#1m_30s", v) && v.kind == Kind::Time && v.i == 90000000000LL);
    CHECK(parseLiteral("t#250ms", v) && v.i == 250000000);
    CHECK(parseLiteral("T#-2s", v) && v.i == -2000000000LL);
    CHECK(parseLiteral("16#FF", v) && v.i == 255);
    CHECK(parseLiteral("2#1010", v) && v.i == 10);
    CHECK(parseLiteral("-12", v) && v.kind == Kind::Int && v.i == -12);
    CHECK(parseLiteral("2.5", v) && v.kind == Kind::Real && v.r == 2.5);
    CHECK(parseLiteral("true", v) && v.kind == Kind::Bool && v.b);
    CHECK(parseLiteral("'ab'", v) && v.s == "ab");
    CHECK(!parseLiteral("T#5x", v));
    CHECK(!parseLiteral("abc", v));
    CHECK(!parseLiteral("", v));
}

// ---- small programs ----

// The pieces of a program file as tiaconv writes them.
std::string type(const std::string& name) {
    const bool b = name == "Bool";
    return R"({"kind":"elementary","name":")" + name + R"(","bits":)" + (b ? "1" : "16") +
           R"(,"length":null,"dims":[],"element":null})";
}
std::string member(const std::string& name, const std::string& dt, const std::string& section, bool retain = false,
                   const std::string& extra = "") {
    std::string t;
    if (dt == "TON_TIME" || dt == "TOF_TIME" || dt == "TP_TIME" || dt == "TONR_TIME") {
        t = R"({"kind":"system_type","name":")" + dt + R"(","bits":null,"length":null,"dims":[],"element":null})";
        return R"({"name":")" + name + R"(","data_type":")" + dt + R"(","type":)" + t + R"(,"section":")" + section +
               R"(","start":null,"retain":false,"members":[)" + member("PT", "Time", "Input") + "," +
               member("ET", "Time", "Output") + "," + member("IN", "Bool", "Input") + "," + member("Q", "Bool", "Output") +
               "," + member("R", "Bool", "Input") + "]}";
    }
    if (dt == "CTUD_INT") {
        t = R"({"kind":"system_type","name":"CTUD_INT","bits":null,"length":null,"dims":[],"element":null})";
        std::string ms;
        for (const char* b : {"CU", "CD", "R", "LD", "QU", "QD"}) ms += member(b, "Bool", "Input") + ",";
        ms += member("PV", "Int", "Input") + "," + member("CV", "Int", "Output");
        return R"({"name":")" + name + R"(","data_type":"CTUD_INT","type":)" + t + R"(,"section":")" + section +
               R"(","start":null,"retain":false,"members":[)" + ms + "]}";
    }
    t = type(dt);
    if (dt == "Time") t = R"({"kind":"elementary","name":"Time","bits":32,"length":null,"dims":[],"element":null})";
    if (dt == "Real") t = R"({"kind":"elementary","name":"Real","bits":32,"length":null,"dims":[],"element":null})";
    if (dt.rfind("Array", 0) == 0)
        t = R"({"kind":"array","name":"Array","bits":null,"length":null,"dims":[{"low":1,"low_text":"1","high":3,"high_text":"3"}],"element":)" +
            type("Int") + "}";
    return R"({"name":")" + name + R"(","data_type":")" + dt + R"(","type":)" + t + R"(,"section":")" + section +
           R"(","start":null,"retain":)" + (retain ? "true" : "false") + extra + R"(,"members":[]})";
}

// An expression: op, with the fields given
std::string e(const std::string& op, const std::string& fields = "", const std::string& args = "") {
    return R"({"op":")" + op + "\"" + (fields.empty() ? "" : "," + fields) + R"(,"args":[)" + args + "]}";
}
std::string r(int id) { return e("ref", "\"ref_id\":" + std::to_string(id)); }
std::string ci(long long v, const std::string& t = "Int") {
    return e("const", R"("type":")" + t + R"(","value":{"kind":"int","int":)" + std::to_string(v) + "}");
}
std::string ct(long long ms) {
    return e("const", R"("type":"Time","value":{"kind":"duration","ns":)" + std::to_string(ms * 1000000) + "}");
}
std::string op2(const std::string& op, const std::string& a, const std::string& b, const std::string& t = "") {
    return e(op, t.empty() ? "" : R"("type":")" + t + "\"", a + "," + b);
}
std::string s(const std::string& op, const std::string& exprs, const std::string& more = "") {
    return R"({"op":")" + op + R"(","exprs":[)" + exprs + "]" + (more.empty() ? "" : "," + more) + "}";
}
std::string assign(const std::string& to, const std::string& v) { return s("assign", to + "," + v); }
std::string param(const std::string& dir, const std::string& name, const std::string& actual) {
    return e(dir, R"("name":")" + name + "\"", actual);
}
// a reference of the block: a local, a tag or a data block member
std::string local(int id, const std::string& path, const std::string& dt = "Int") {
    std::string p;
    size_t from = 0;
    while (from <= path.size()) {
        size_t dot = path.find('.', from);
        if (dot == std::string::npos) dot = path.size();
        if (!p.empty()) p += ",";
        p += R"({"name":")" + path.substr(from, dot - from) + R"(","index":[]})";
        from = dot + 1;
    }
    return R"({"ref_id":)" + std::to_string(id) + R"(,"kind":"local","text":"#)" + path + R"(","data_type":")" + dt +
           R"(","name":null,"path":[)" + p + R"(],"modifier":null})";
}
std::string tag(int id, const std::string& name) {
    return R"({"ref_id":)" + std::to_string(id) + R"(,"kind":"tag","text":"\")" + name + R"(\"","name":null,"path":[],"modifier":null})";
}
std::string dbm(int id, const std::string& db, const std::string& m, const std::string& index = "") {
    return R"({"ref_id":)" + std::to_string(id) + R"(,"kind":"data block member","text":"x","name":null,"path":[{"name":")" + db +
           R"(","index":[]},{"name":")" + m + R"(","index":[)" + index + R"(]}],"modifier":null})";
}
std::string block(const std::string& name, const std::string& t, const std::string& iface, const std::string& refs,
                  const std::string& stmts) {
    return R"({"type":")" + t + R"(","name":")" + name + R"(","language":"SCL","protection":null,"interface":[)" + iface +
           R"(],"code":{"networks":[{"number":1,"statements":[)" + stmts + R"(]}],"references":[)" + refs + "]}}";
}
std::string program(const std::string& tags, const std::string& dbs, const std::string& blocks,
                    const std::string& extraObs = "", const std::string& interrupts = "") {
    return R"({"format":"tiaconv-program","schema_version":2,"plc":{"name":"T"},"tags":[)" + tags +
           R"(],"data_blocks":[)" + dbs + R"(],"organization_blocks":[{"name":"Main","event":"ProgramCycle","priority":1,"cycle_us":null})" +
           extraObs + R"(],"runtime":{"startup":[],"program_cycle":["Main"],"interrupts":[)" + interrupts +
           R"(]},"blocks":[)" + blocks + "]}";
}
std::string tagDecl(const std::string& name, const std::string& dt, const std::string& at) {
    return R"({"name":")" + name + R"(","data_type":")" + dt + R"(","type":)" + type(dt) + R"(,"address":")" + at + "\"}";
}
std::string db(const std::string& name, const std::string& members) {
    return R"({"name":")" + name + R"(","members":[)" + members + "]}";
}

Val get(Simulator& sim, const std::string& op) {
    Val v;
    std::string err;
    if (!sim.read(op, v, err)) {
        ++failures;
        std::cerr << "cannot read " << op << ": " << err << "\n";
    }
    return v;
}
void set(Simulator& sim, const std::string& op, const Val& v) {
    std::string err;
    if (!sim.write(op, v, err)) {
        ++failures;
        std::cerr << "cannot write " << op << ": " << err << "\n";
    }
}

// A timer in a global DB, driven by %M0.0: IN and the outputs to M.
std::string timerProgram(const std::string& kind) {
    const std::string tags = tagDecl("In", "Bool", "%M0.0") + "," + tagDecl("Q", "Bool", "%M0.1") + "," +
                             tagDecl("Rst", "Bool", "%M0.2");
    const std::string dbs = db("T", member("t", kind + "_TIME", "Static"));
    const std::string call =
        e("call", R"("name":")" + kind + R"(","target":{"kind":"instruction","name":")" + kind +
                      R"(","instance":{"kind":"data_block","name":"T_t","path":[]}})",
          param("in", "IN", r(1)) + "," + param("in", "PT", ct(1000)) + "," +
              (kind == "TONR" ? param("in", "R", r(3)) + "," : std::string()) + param("out", "Q", r(2)));
    const std::string refs = tag(1, "In") + "," + tag(2, "Q") + "," + tag(3, "Rst");
    // the instance as a data block of its own, as an IEC timer DB is
    const std::string inst = db("T_t", std::string(R"({"name":"PT","data_type":"Time","type":)") +
                                           R"({"kind":"elementary","name":"Time","bits":32,"length":null,"dims":[],"element":null})" +
                                           R"(,"section":"Input","start":null,"retain":false,"members":[]},)" +
                                           member("ET", "Time", "Output") + "," + member("IN", "Bool", "Input") + "," +
                                           member("Q", "Bool", "Output") + "," + member("R", "Bool", "Input"));
    return program(tags, dbs + "," + inst, block("Main", "OB", "", refs, s("call", call)));
}

void testTimers() {
    auto ms = [](int64_t n) { return n * 1000000; };
    {
        Simulator sim(parseJson(timerProgram("TON")));
        sim.powerOn();
        sim.scan();
        set(sim, "In", boolVal(true));
        sim.runFor(ms(990));  // 99 cycles; the last one at 980 ms after the start
        CHECK(!get(sim, "Q").b);
        EQ(get(sim, "\"T_t\".ET").i, ms(980));
        sim.scan();  // at 990
        CHECK(!get(sim, "Q").b);
        sim.scan();  // at 1000: on
        CHECK(get(sim, "Q").b);
        EQ(get(sim, "\"T_t\".ET").i, ms(1000));
        set(sim, "In", boolVal(false));
        sim.scan();
        CHECK(!get(sim, "Q").b);
        EQ(get(sim, "\"T_t\".ET").i, 0);
    }
    {
        Simulator sim(parseJson(timerProgram("TOF")));
        sim.powerOn();
        set(sim, "In", boolVal(true));
        sim.scan();
        CHECK(get(sim, "Q").b);
        set(sim, "In", boolVal(false));
        sim.runFor(ms(500));
        CHECK(get(sim, "Q").b);
        sim.runFor(ms(600));
        CHECK(!get(sim, "Q").b);
        EQ(get(sim, "\"T_t\".ET").i, ms(1000));
        set(sim, "In", boolVal(true));
        sim.scan();
        CHECK(get(sim, "Q").b);
        EQ(get(sim, "\"T_t\".ET").i, 0);
    }
    {
        Simulator sim(parseJson(timerProgram("TP")));
        sim.powerOn();
        set(sim, "In", boolVal(true));
        sim.scan();
        set(sim, "In", boolVal(false));
        sim.runFor(ms(500));
        CHECK(get(sim, "Q").b);  // the pulse goes on without IN
        set(sim, "In", boolVal(true));
        sim.runFor(ms(600));  // a new edge during the pulse does not restart it
        CHECK(!get(sim, "Q").b);
        EQ(get(sim, "\"T_t\".ET").i, ms(1000));  // IN still on: ET stays at PT
        set(sim, "In", boolVal(false));
        sim.scan();
        EQ(get(sim, "\"T_t\".ET").i, 0);
    }
    {
        Simulator sim(parseJson(timerProgram("TONR")));
        sim.powerOn();
        set(sim, "In", boolVal(true));
        sim.runFor(ms(600));
        set(sim, "In", boolVal(false));
        sim.runFor(ms(300));
        CHECK(!get(sim, "Q").b);
        EQ(get(sim, "\"T_t\".ET").i, ms(600));  // IN was seen on until 600 ms
        set(sim, "In", boolVal(true));
        sim.runFor(ms(420));
        CHECK(get(sim, "Q").b);
        set(sim, "In", boolVal(false));
        sim.scan();
        CHECK(get(sim, "Q").b);  // stays on until R
        set(sim, "Rst", boolVal(true));
        sim.scan();
        CHECK(!get(sim, "Q").b);
        EQ(get(sim, "\"T_t\".ET").i, 0);
    }
}

void testCounter() {
    const std::string tags = tagDecl("Up", "Bool", "%M0.0") + "," + tagDecl("Down", "Bool", "%M0.1") + "," +
                             tagDecl("Ld", "Bool", "%M0.2") + "," + tagDecl("Cv", "Int", "%MW2");
    const std::string call = e("call", R"("name":"CTUD","target":{"kind":"instruction","name":"CTUD","instance":{"kind":"data_block","name":"C","path":[]}})",
                               param("in", "CU", r(1)) + "," + param("in", "CD", r(2)) + "," + param("in", "LD", r(3)) +
                                   "," + param("in", "PV", ci(3)) + "," + param("out", "CV", r(4)));
    const std::string refs = tag(1, "Up") + "," + tag(2, "Down") + "," + tag(3, "Ld") + "," + tag(4, "Cv");
    std::string ms;
    for (const char* b : {"CU", "CD", "R", "LD", "QU", "QD"}) ms += member(b, "Bool", "Input") + ",";
    ms += member("PV", "Int", "Input") + "," + member("CV", "Int", "Output");
    Simulator sim(parseJson(program(tags, db("C", ms), block("Main", "OB", "", refs, s("call", call)))));
    sim.powerOn();
    for (int k = 0; k < 4; ++k) {
        set(sim, "Up", boolVal(true));
        sim.scan();
        set(sim, "Up", boolVal(false));
        sim.scan();
    }
    EQ(get(sim, "Cv").i, 4);
    CHECK(get(sim, "\"C\".QU").b);
    set(sim, "Up", boolVal(true));
    sim.scan();
    sim.scan();  // held: counts once
    EQ(get(sim, "Cv").i, 5);
    set(sim, "Down", boolVal(true));
    sim.scan();
    EQ(get(sim, "Cv").i, 4);
    set(sim, "Ld", boolVal(true));
    sim.scan();
    EQ(get(sim, "Cv").i, 3);
    CHECK(!get(sim, "\"C\".QD").b);
}

// SCL: an FC with an in-out by reference and a return value; loops; a
// CASE; built-ins; arrays.
void testScl() {
    const std::string tags = tagDecl("A", "Int", "%MW0") + "," + tagDecl("B", "Int", "%MW2") + "," +
                             tagDecl("C", "Int", "%MW4") + "," + tagDecl("F", "Bool", "%M6.0");
    const std::string dbs = db("D", member("arr", "Array[1..3] of Int", "Static") + "," + member("i", "Int", "Static") +
                                        "," + member("x", "Real", "Static") + "," + member("keep", "Int", "Static", true));
    // FC Twice(IN v: Int; IN_OUT io: Int) : Int    io := io + v; Twice := v * 2;
    const std::string fc = block("Twice", "FC",
                                 member("v", "Int", "Input") + "," + member("io", "Int", "InOut") + "," +
                                     member("Ret_Val", "Int", "Return"),
                                 local(1, "v") + "," + local(2, "io") + "," + local(3, "Ret_Val"),
                                 assign(r(2), op2("add", r(2), r(1), "Int")) + "," +
                                     assign(r(3), op2("mul", r(1), ci(2), "Int")));
    const std::string call = e("call", R"("name":"Twice","target":{"kind":"block","name":"Twice","instance":null})",
                               param("in", "v", ci(5)) + "," + param("inout", "io", r(2)));
    // FOR #i := 1 TO 3 DO "D".arr[#i] := #i * 10; IF #i = 2 THEN EXIT; END_IF; END_FOR;
    // (the index as the ref 11: the loop variable is "D".i)
    const std::string forLoop =
        s("for", r(10) + "," + ci(1) + "," + ci(3),
          R"("body":[)" + assign(r(12), op2("mul", r(10), ci(10), "Int")) + "," +
              s("if", "", R"("branches":[{"exprs":[)" + op2("eq", r(10), ci(2)) + R"(],"body":[)" + s("exit", "") +
                              R"(]}],"else":null)") +
              "]");
    // CASE A OF 1..4: F := TRUE; 7, 9: F := FALSE; END_CASE
    const std::string caseStmt =
        s("case", r(1),
          R"("branches":[{"exprs":[)" + e("range", "", ci(1) + "," + ci(4)) + R"(],"body":[)" +
              assign(r(4), e("const", R"("type":"Bool","value":{"kind":"bool","bool":true})")) + R"(]},{"exprs":[)" +
              ci(7) + "," + ci(9) + R"(],"body":[)" +
              assign(r(4), e("const", R"("type":"Bool","value":{"kind":"bool","bool":false})")) + R"(]}],"else":null)");
    // "D".x := INT_TO_REAL(LIMIT(MN := 0, IN := A, MX := 3)) / 2.0
    const std::string lim =
        e("call", R"("name":"LIMIT","type":"Int","target":{"kind":"builtin","name":"LIMIT"})",
          param("in", "MN", ci(0)) + "," + param("in", "IN", r(1)) + "," + param("in", "MX", ci(3)));
    const std::string conv =
        e("call", R"("name":"INT_TO_REAL","type":"Real","target":{"kind":"builtin","name":"INT_TO_REAL"})",
          param("in", "IN", lim));
    const std::string half =
        e("const", R"("type":"Real","value":{"kind":"real","real":2.0})");
    const std::string main = block(
        "Main", "OB", member("t", "Int", "Temp"),
        tag(1, "A") + "," + tag(2, "B") + "," + tag(3, "C") + "," + tag(4, "F") + "," + dbm(10, "D", "i") + "," +
            dbm(12, "D", "arr", "10") + "," + dbm(13, "D", "x"),
        assign(r(3), call) + "," + forLoop + "," + caseStmt + "," + assign(r(13), op2("div", conv, half, "Real")));
    Simulator sim(parseJson(program(tags, dbs, main + "," + fc)));
    sim.powerOn();
    set(sim, "A", intVal(4));
    set(sim, "B", intVal(1));
    set(sim, "\"D\".keep", intVal(9));
    sim.scan();
    CHECK(sim.running());
    EQ(get(sim, "C").i, 10);   // the return value
    EQ(get(sim, "B").i, 6);    // the in-out, changed in place
    EQ(get(sim, "\"D\".arr[1]").i, 10);
    EQ(get(sim, "\"D\".arr[2]").i, 20);
    EQ(get(sim, "\"D\".arr[3]").i, 0);  // EXIT
    EQ(get(sim, "\"D\".i").i, 2);
    CHECK(get(sim, "F").b);
    EQ(get(sim, "\"D\".x").r, 1.5);
    set(sim, "A", intVal(9));
    sim.scan();
    CHECK(!get(sim, "F").b);
    sim.warmRestart();
    EQ(get(sim, "\"D\".keep").i, 9);
    EQ(get(sim, "\"D\".arr[1]").i, 0);
    EQ(get(sim, "B").i, 0);  // M is not retentive here
    sim.powerOn();
    EQ(get(sim, "\"D\".keep").i, 0);
    Val v;
    std::string err;
    CHECK(!sim.read("\"D\".arr[4]", v, err));
    CHECK(!sim.read("\"D\".nothing", v, err));
    CHECK(!sim.read("\"D\".arr", v, err));
    CHECK(!sim.read("Nothing", v, err));
    CHECK(sim.read("%MW2", v, err));
    CHECK(sim.read("\"D\".i.%X0", v, err));
}

void testFaults() {
    const std::string tags = tagDecl("A", "Int", "%MW0");
    // WHILE TRUE DO A := A + 1; END_WHILE
    const std::string loop = s("while", e("const", R"("type":"Bool","value":{"kind":"bool","bool":true})"),
                               R"("body":[)" + assign(r(1), op2("add", r(1), ci(1), "Int")) + "]");
    Simulator sim(parseJson(program(tags, "", block("Main", "OB", "", tag(1, "A"), loop))));
    sim.powerOn();
    sim.scan();
    CHECK(!sim.running());
    CHECK(sim.fault().find("maximum cycle time") != std::string::npos);
    const int64_t t = sim.now();
    sim.runFor(1000000000);
    EQ(sim.now(), t + 1000000000);  // time goes on in STOP; the program does not
    // an index out of bounds stops the CPU
    const std::string dbs = db("D", member("arr", "Array[1..3] of Int", "Static"));
    Simulator s2(parseJson(program(tags, dbs,
                                   block("Main", "OB", "", tag(1, "A") + "," + dbm(2, "D", "arr", "1"),
                                         assign(r(2), ci(1))))));
    s2.powerOn();
    set(s2, "A", intVal(7));
    s2.scan();
    CHECK(!s2.running());
    s2.warmRestart();
    CHECK(s2.running());
    try {
        Simulator bad(parseJson(R"({"format":"x"})"));
        CHECK(false);
    } catch (const std::runtime_error&) {
        CHECK(true);
    }
    try {
        Simulator bad(parseJson(R"({"format":"tiaconv-program","schema_version":1})"));
        CHECK(false);
    } catch (const std::runtime_error&) {
        CHECK(true);
    }
}

// A cyclic interrupt of 100 ms, phase 0, counting in M.
void testCyclic() {
    const std::string tags = tagDecl("N", "Int", "%MW0") + "," + tagDecl("K", "Int", "%MW2");
    const std::string ob = R"(,{"name":"Cyc","event":"CyclicInterrupt","priority":8,"cycle_us":100000,"phase_shift_us":0})";
    const std::string blocks = block("Main", "OB", "", tag(1, "K"), assign(r(1), op2("add", r(1), ci(1), "Int"))) + "," +
                               block("Cyc", "OB", "", tag(1, "N"), assign(r(1), op2("add", r(1), ci(1), "Int")));
    Simulator sim(parseJson(program(tags, "", blocks, ob, "\"Cyc\"")));
    sim.setScanTime(30000000);
    sim.powerOn();
    sim.runFor(1000000000);
    EQ(get(sim, "N").i, 10);
    EQ(get(sim, "K").i, 34);  // 1000 / 30, rounded up
}

// SHL, ROL, SWAP, MUX; R_TRIG; a call with EN FALSE
void testInstructions() {
    const std::string tags = tagDecl("W", "Word", "%MW0") + "," + tagDecl("A", "Int", "%MW2") + "," +
                             tagDecl("B", "Int", "%MW4") + "," + tagDecl("Clk", "Bool", "%M6.0") + "," +
                             tagDecl("Q", "Bool", "%M6.1") + "," + tagDecl("En", "Bool", "%M6.2") + "," +
                             tagDecl("Eno", "Bool", "%M6.3") + "," + tagDecl("S", "Word", "%MW8");
    auto fn = [](const std::string& n, const std::string& t, const std::string& params) {
        return e("call", R"("name":")" + n + R"(","type":")" + t + R"(","target":{"kind":"builtin","name":")" + n + "\"}",
                 params);
    };
    const std::string w16 = e("const", R"("type":"Word","value":{"kind":"int","int":4660})");  // 16#1234
    const std::string trig = e("call", R"("name":"R_TRIG","target":{"kind":"instruction","name":"R_TRIG","instance":{"kind":"data_block","name":"RT","path":[]}})",
                               param("in", "CLK", r(4)) + "," + param("out", "Q", r(5)));
    const std::string guarded = fn("ABS", "Int", param("in", "EN", r(6)) + "," + param("in", "IN", ci(-9)) + "," +
                                                     param("out", "ENO", r(7)));
    const std::string main = block(
        "Main", "OB", "",
        tag(1, "W") + "," + tag(2, "A") + "," + tag(3, "B") + "," + tag(4, "Clk") + "," + tag(5, "Q") + "," +
            tag(6, "En") + "," + tag(7, "Eno") + "," + tag(8, "S"),
        assign(r(1), fn("ROL", "Word", param("in", "IN", w16) + "," + param("in", "N", ci(4)))) + "," +
            assign(r(8), fn("SWAP", "Word", param("in", "IN", w16))) + "," +
            assign(r(2), fn("MUX", "Int", param("in", "K", ci(1)) + "," + param("in", "IN0", ci(10)) + "," +
                                               param("in", "IN1", ci(11)) + "," + param("in", "ELSE", ci(99)))) +
            "," + s("call", trig) + "," + assign(r(3), guarded));
    std::string ms = member("CLK", "Bool", "Input") + "," + member("Q", "Bool", "Output");
    Simulator sim(parseJson(program(tags, db("RT", ms), main)));
    sim.powerOn();
    set(sim, "B", intVal(5));
    sim.scan();
    EQ(get(sim, "W").i, 0x2341);
    EQ(get(sim, "S").i, 0x3412);
    EQ(get(sim, "A").i, 11);
    EQ(get(sim, "B").i, 5);  // EN FALSE: not done
    CHECK(!get(sim, "Eno").b);
    CHECK(!get(sim, "Q").b);
    set(sim, "Clk", boolVal(true));
    set(sim, "En", boolVal(true));
    sim.scan();
    CHECK(get(sim, "Q").b);
    EQ(get(sim, "B").i, 9);
    CHECK(get(sim, "Eno").b);
    sim.scan();
    CHECK(!get(sim, "Q").b);  // one cycle only
}

// ---- STL ----

std::string stl(const std::string& name, const std::string& exprs = "", const std::string& target = "") {
    return R"({"op":"stl","name":")" + name + R"(","exprs":[)" + exprs + "]" +
           (target.empty() ? std::string() : R"(,"target":)" + target) + "}";
}
std::string label(const std::string& name) { return R"({"op":"label","name":")" + name + R"(","exprs":[]})"; }
std::string lab(const std::string& name) { return e("ref", R"("name":")" + name + "\""); }  // a jump's operand
std::string cr(double v) {
    std::ostringstream o;
    o << v;
    return e("const", R"("type":"Real","value":{"kind":"real","real":)" + o.str() + "}");
}
std::string kw(const std::string& k) { return e("const", R"("name":")" + k + "\""); }  // BR, OV, >0
// a block in STL: each network a list of statements
std::string stlBlock(const std::string& name, const std::string& t, const std::string& refs,
                     const std::vector<std::string>& networks) {
    std::string n;
    for (size_t k = 0; k < networks.size(); ++k)
        n += (k ? "," : "") + std::string(R"({"number":)") + std::to_string(k + 1) + R"(,"statements":[)" + networks[k] + "]}";
    return R"({"type":")" + t + R"(","name":")" + name + R"(","language":"STL","protection":null,"interface":[],"code":{"networks":[)" +
           n + R"(],"references":[)" + refs + "]}}";
}

void testStl() {
    struct T {
        const char* name;
        const char* type;
        const char* at;
    };
    const std::vector<T> ts = {
        {"a", "Bool", "%M0.0"},     {"b", "Bool", "%M0.1"},     {"c", "Bool", "%M0.2"},    {"d", "Bool", "%M0.3"},
        {"q1", "Bool", "%M1.0"},    {"q2", "Bool", "%M1.1"},    {"q3", "Bool", "%M1.2"},   {"q4", "Bool", "%M1.3"},
        {"mem", "Bool", "%M1.4"},   {"fp", "Bool", "%M1.5"},    {"gt", "Bool", "%M2.0"},   {"br", "Bool", "%M2.1"},
        {"ov", "Bool", "%M2.2"},
        {"I1", "Int", "%MW10"},     {"I2", "Int", "%MW12"},     {"Sum", "Int", "%MW14"},   {"Quot", "Int", "%MW16"},
        {"D1", "DInt", "%MD20"},    {"Rem", "DInt", "%MD24"},   {"R1", "Real", "%MD28"},   {"R2", "Real", "%MD32"},
        {"Prod", "Real", "%MD36"},  {"Rf", "Real", "%MD40"},    {"Rn", "DInt", "%MD44"},   {"N", "Int", "%MW48"},
        {"Cnt", "Int", "%MW50"},    {"Flag", "Int", "%MW52"},   {"W1", "Word", "%MW54"},   {"W2", "Word", "%MW56"},
        {"Sh", "Int", "%MW58"},     {"Neg", "DInt", "%MD60"},   {"Bcd", "Word", "%MW64"},  {"Z", "Int", "%MW66"},
    };
    std::string tags, refs;
    std::map<std::string, int> id;
    for (size_t k = 0; k < ts.size(); ++k) {
        tags += (k ? "," : "") + tagDecl(ts[k].name, ts[k].type, ts[k].at);
        refs += (k ? "," : "") + tag(static_cast<int>(k + 1), ts[k].name);
        id[ts[k].name] = static_cast<int>(k + 1);
    }
    auto t = [&](const char* n) { return r(id[n]); };
    const std::string logic =
        // (a AND b) OR (c AND d)
        stl("A", t("a")) + "," + stl("A", t("b")) + "," + stl("O") + "," + stl("A", t("c")) + "," + stl("A", t("d")) +
        "," + stl("=", t("q1")) + "," +
        // a OR (b AND c)
        stl("A", t("a")) + "," + stl("O(") + "," + stl("A", t("b")) + "," + stl("A", t("c")) + "," + stl(")") + "," +
        stl("=", t("q2")) + "," +
        // (a OR b) AND NOT c, in German
        stl("U(") + "," + stl("O", t("a")) + "," + stl("O", t("b")) + "," + stl(")") + "," + stl("UN", t("c")) + "," +
        stl("=", t("q3")) + "," +
        // a XOR b
        stl("X", t("a")) + "," + stl("X", t("b")) + "," + stl("=", t("q4")) + "," +
        // a rising edge of d
        stl("A", t("d")) + "," + stl("FP", t("mem")) + "," + stl("=", t("fp"));
    const std::string arith =
        stl("L", t("I1")) + "," + stl("L", t("I2")) + "," + stl("+I") + "," + stl("T", t("Sum")) + "," +
        stl("L", t("I1")) + "," + stl("L", t("I2")) + "," + stl("/I") + "," + stl("T", t("Quot")) + "," +
        stl("L", t("D1")) + "," + stl("L", ci(7, "DInt")) + "," + stl("MOD") + "," + stl("T", t("Rem")) + "," +
        stl("L", t("R1")) + "," + stl("L", t("R2")) + "," + stl("*R") + "," + stl("T", t("Prod")) + "," +
        stl("L", t("I1")) + "," + stl("ITD") + "," + stl("DTR") + "," + stl("T", t("Rf")) + "," +
        stl("L", cr(2.5)) + "," + stl("RND") + "," + stl("T", t("Rn"));
    // N := 0; FOR Cnt := 5 DOWNTO 1 (LOOP): N := N + 3; I1 > I2; IF I1 = 17 THEN Flag := 2 ELSE Flag := 1
    const std::string jumps =
        stl("L", ci(0)) + "," + stl("T", t("N")) + "," + stl("L", ci(5)) + "," + label("NXT") + "," +
        stl("T", t("Cnt")) + "," + stl("L", t("N")) + "," + stl("+", ci(3)) + "," + stl("T", t("N")) + "," +
        stl("L", t("Cnt")) + "," + stl("LOOP", lab("NXT")) + "," +
        stl("L", t("I1")) + "," + stl("L", t("I2")) + "," + stl(">I") + "," + stl("=", t("gt")) + "," +
        stl("L", t("I1")) + "," + stl("L", ci(17)) + "," + stl("==I") + "," + stl("JC", lab("EQ")) + "," +
        stl("L", ci(1)) + "," + stl("T", t("Flag")) + "," + stl("JU", lab("END")) + "," + label("EQ") + "," +
        stl("L", ci(2)) + "," + stl("T", t("Flag")) + "," + label("END") + "," + stl("NOP", ci(0));
    const std::string words =
        stl("L", ci(0xF0F0, "Word")) + "," + stl("AW", ci(0x0FF0, "Word")) + "," + stl("T", t("W1")) + "," +
        stl("L", t("W1")) + "," + stl("SLW", ci(4)) + "," + stl("T", t("W2")) + "," +
        stl("L", ci(-8)) + "," + stl("SSI", ci(1)) + "," + stl("T", t("Sh")) + "," +
        stl("L", t("D1")) + "," + stl("NEGD") + "," + stl("T", t("Neg")) + "," +
        stl("L", ci(123)) + "," + stl("ITB") + "," + stl("T", t("Bcd")) + "," +
        // 1234 is more than ITB takes: OV, ACCU1 unchanged
        stl("L", ci(1234)) + "," + stl("ITB") + "," + stl("A", kw("OV")) + "," + stl("=", t("ov"));
    // an FC in STL: BR := a, and BEC when b
    const std::string fc = stlBlock("Sub", "FC", refs,
                                    {stl("U", t("a")) + "," + stl("SAVE") + "," + stl("SET") + "," + stl("A", t("b")) + "," + stl("BEC") +
                                     "," + stl("L", ci(9)) + "," + stl("T", t("Z"))});
    const std::string callFc = stl("CALL", e("ref", R"("name":"\"Sub\"")"), R"({"kind":"block","name":"Sub","instance":null})") +
                               "," + stl("A", kw("BR")) + "," + stl("=", t("br"));
    const std::string main = stlBlock("Main", "OB", refs, {logic, arith, jumps, words, callFc});
    Simulator sim(parseJson(program(tags, "", main + "," + fc)));
    sim.powerOn();
    set(sim, "a", boolVal(false));
    set(sim, "b", boolVal(true));
    set(sim, "d", boolVal(true));
    set(sim, "I1", intVal(17));
    set(sim, "I2", intVal(5));
    set(sim, "D1", intVal(100));
    set(sim, "R1", realVal(1.5));
    set(sim, "R2", realVal(4.0));
    sim.scan();
    CHECK(sim.running());
    CHECK(sim.warnings().empty());
    for (const auto& w : sim.warnings()) std::cerr << "  warning: " << w << "\n";
    CHECK(!get(sim, "q1").b);  // (0 AND 1) OR (0 AND 1)
    CHECK(!get(sim, "q2").b);  // 0 OR (1 AND 0)
    CHECK(get(sim, "q3").b);   // (0 OR 1) AND NOT 0
    CHECK(get(sim, "q4").b);   // 0 XOR 1
    CHECK(get(sim, "ov").b);
    CHECK(get(sim, "fp").b);
    EQ(get(sim, "Sum").i, 22);
    EQ(get(sim, "Quot").i, 3);
    EQ(get(sim, "Rem").i, 2);
    EQ(get(sim, "Prod").r, 6.0);
    EQ(get(sim, "Rf").r, 17.0);
    EQ(get(sim, "Rn").i, 2);  // halves to even
    EQ(get(sim, "N").i, 15);
    EQ(get(sim, "Cnt").i, 1);
    CHECK(get(sim, "gt").b);
    EQ(get(sim, "Flag").i, 2);
    EQ(get(sim, "W1").i, 0x00F0);
    EQ(get(sim, "W2").i, 0x0F00);
    EQ(get(sim, "Sh").i, -4);
    EQ(get(sim, "Neg").i, -100);
    EQ(get(sim, "Bcd").i, 0x0123);
    CHECK(!get(sim, "br").b);  // BR of Sub: a
    EQ(get(sim, "Z").i, 0);    // BEC: b is on
    // the other values
    set(sim, "a", boolVal(true));
    set(sim, "b", boolVal(false));
    set(sim, "c", boolVal(true));
    set(sim, "I1", intVal(3));
    sim.scan();
    CHECK(get(sim, "q1").b);   // (1 AND 0) OR (1 AND 1)
    CHECK(get(sim, "q2").b);   // 1 OR ...
    CHECK(!get(sim, "q3").b);  // ... AND NOT 1
    CHECK(get(sim, "q4").b);   // 1 XOR 0
    set(sim, "b", boolVal(true));
    sim.scan();
    CHECK(!get(sim, "q4").b);  // 1 XOR 1
    set(sim, "b", boolVal(false));
    sim.scan();
    CHECK(!get(sim, "fp").b);  // d stays on: no edge
    CHECK(!get(sim, "gt").b);
    EQ(get(sim, "Flag").i, 1);
    CHECK(get(sim, "br").b);
    EQ(get(sim, "Z").i, 9);
    // an endless STL loop stops the CPU
    const std::string loop = stlBlock("Main", "OB", "", {label("L1") + "," + stl("JU", lab("L1"))});
    Simulator s2(parseJson(program("", "", loop)));
    s2.powerOn();
    s2.scan();
    CHECK(!s2.running());
}

// The layout of data blocks with standard access against the offsets TIA
// Portal stored (tiaconv's export of s13_hmi and s14_code_rewritten)
void testLayout() {
    for (const char* file : {"data/ZZBRAVO.json", "data/ZZPLC.json"}) {
        const Json p = parseJson(readFile(file));
        Simulator sim(p);
        int n = 0;
        std::function<void(const std::string&, const Json&)> walk = [&](const std::string& path, const Json& members) {
            for (const auto& m : members.items) {
                const std::string op = path + "." + m["name"].str();
                if (m["offset_bits"].type == Json::Number) {
                    ++n;
                    const int64_t got = sim.bitOffset(op);
                    if (got != m["offset_bits"].asInt()) {
                        ++failures;
                        std::cerr << file << ": " << op << " at bit " << got << ", stored " << m["offset_bits"].asInt() << "\n";
                    }
                    ++checks;
                }
                if (m["type"]["kind"].str() != "array") walk(op, m["members"]);
            }
        };
        for (const auto& d : p["data_blocks"].items)
            if (!d["optimized"].b) walk("\"" + d["name"].str() + "\"", d["members"]);
        CHECK(n > 5);
    }
    // arrays: the elements one after the other
    Simulator sim(parseJson(readFile("data/ZZBRAVO.json")));
    EQ(sim.bitOffset("\"DB_Standard\".my_stringof10[1]"), 2192 + 256 * 8);
    EQ(sim.bitOffset("\"DB_Standard\".my_array_of_int[3]"), 24720 + 3 * 16);
    EQ(sim.bitOffset("\"DB_Optimized\".my_int"), -1);
}

void testScenarioFiles() {
    std::ostringstream out;
    const ScenarioResult r = runScenario("scenarios/s15_motor.json", out, false);
    EQ(r.failures, 0);
    CHECK(r.checks > 40);
}

}  // namespace

int main() {
    testJson();
    testValues();
    testAreas();
    testS5Time();
    testLiterals();
    testTimers();
    testCounter();
    testScl();
    testFaults();
    testCyclic();
    testInstructions();
    testStl();
    testLayout();
    testScenarioFiles();
    std::cout << checks << " checks, " << failures << " failed\n";
    return failures ? 1 : 0;
}
