// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The interrupt OBs: their events (cyclic, time delay, time of day,
// hardware, time error), the priorities by which an OB interrupts another,
// the instructions that start and stop the events (SRT_DINT, CAN_DINT,
// QRY_DINT, SET_TINTL, ACT_TINT, CAN_TINT, QRY_TINT) and the clock of the
// CPU (RD_SYS_T, RD_LOC_T, WR_SYS_T).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>

#include "sim.hpp"

namespace s7forge {
namespace {

std::string up(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

constexpr int64_t kSecond = 1000000000LL;
constexpr int64_t kDay = 86400LL * kSecond;
// the clock is in ns since 1970 in 64 bits, which ends in April 2262; the
// years the clock and the time-of-day interrupts reach end with 2261
constexpr int64_t kLastYear = 2261;

// The civil calendar (proleptic Gregorian), days since 1970-01-01
int64_t daysFrom(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil(int64_t z, int64_t& y, int64_t& m, int64_t& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
}

bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int64_t monthDays(int64_t y, int64_t m) {
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : d[m - 1];
}

int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// the codes of RET_VAL (an Int)
constexpr int kOk = 0;
constexpr int kWrongOb = 0x8090;     // no such OB, or not of the right kind
constexpr int kWrongValue = 0x8091;  // DTIME out of range, a start time that is not a date
constexpr int kWrongPeriod = 0x8092;
constexpr int kNotStarted = 0x80A0;  // CAN_DINT: not started; CAN_TINT: no start time set
constexpr int kInThePast = 0x80A1;   // a single time-of-day interrupt in the past

const std::set<std::string>& interruptInstructions() {
    static const std::set<std::string> n = {"SRT_DINT", "CAN_DINT", "QRY_DINT", "SET_TINTL", "ACT_TINT", "CAN_TINT",
                                            "QRY_TINT", "RD_SYS_T", "RD_LOC_T", "WR_SYS_T"};
    return n;
}

}  // namespace

Simulator::IntOb* Simulator::intOb(int number) {
    for (auto& o : iobs_)
        if (o.number == number) return &o;
    return nullptr;
}

bool Simulator::raise(const std::string& ob, std::string& error) {
    IntOb* o = nullptr;
    const bool digits = !ob.empty() && std::all_of(ob.begin(), ob.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    for (auto& x : iobs_) {
        if ((digits && x.number == std::atoi(ob.c_str())) || up(x.ob->name) == up(ob) || up("\"" + x.ob->name + "\"") == up(ob))
            o = &x;
        for (const auto& ev : x.events)  // a hardware event by its name: Rising edge0
            if (up(ev) == up(ob) || up("\"" + ev + "\"") == up(ob)) o = &x;
    }
    if (!o) {
        error = "no interrupt OB " + ob;
        return false;
    }
    if (o->queued < o->maxQueued) {
        ++o->queued;
        eventsWaiting(*o, o->queued);
    } else {
        eventsLost(*o, 1);
    }
    return true;
}

void Simulator::setClock(int64_t ns) {
    clockBase_ = ns - now_;
    // the time-of-day interrupts by the new clock (one that was passed by
    // the change is not run)
    for (auto& o : iobs_)
        if (o.kind == 'T' && o.todActive) {
            const int64_t t = nextTod(o, clock());
            o.next = t == kNever ? kNever : t - clockBase_;
        }
}

void Simulator::configureTod(IntOb& o, const Json& tod) {
    // only the executions seen in a project are mapped (DEVELOPMENT.md)
    static const std::map<std::string, uint16_t> periods = {{"never", 0xFFFF}, {"once", 0x0000}, {"every_minute", 0x0201},
                                                            {"hourly", 0x0401}, {"daily", 0x1001}, {"weekly", 0x1201},
                                                            {"monthly", 0x1401}, {"yearly", 0x1801}, {"end_of_month", 0x2001}};
    const std::string ex = tod["execution"].str();
    auto it = periods.find(ex);
    if (it == periods.end()) {
        warn("OB " + o.ob->name + ": a time-of-day execution S7Forge does not know (stored " +
             std::to_string(tod["execution_stored"].asInt()) + "): the OB runs only when the program sets it");
        return;
    }
    if (it->second == 0xFFFF) return;  // never
    // the start: 2026-01-01T08:00:00, system time (there is no time zone)
    const std::string t = tod["start"].str();
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (std::sscanf(t.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 5 || y < 1970 || y > kLastYear ||
        mo < 1 || mo > 12 || d < 1 || d > monthDays(y, mo)) {
        warn("OB " + o.ob->name + ": a time-of-day start S7Forge cannot read (" + t + ")");
        return;
    }
    o.cfgTod = true;
    o.cfgStart = daysFrom(y, mo, d) * kDay + ((h * 60LL + mi) * 60 + sec) * kSecond;
    o.cfgPeriod = it->second;
}

void Simulator::loadHardwareInterrupts() {
    for (const auto& h : prog_["hardware_interrupts"].items) {
        IntOb* o = nullptr;
        for (auto& x : iobs_)
            if (x.ob->name == h["ob"].str()) o = &x;
        if (!o) continue;
        o->events.push_back(h["event"].str());
        const std::string& edge = h["edge"].str();
        const Address at = parseAddress(h["input"].str());
        if (at.area != 'I' || at.size != 'X' || (edge != "rising_edge" && edge != "falling_edge")) {
            warn("OB " + o->ob->name + ": its hardware interrupt " + h["event"].str() +
                 " (an input or edge S7Forge does not know): a scenario can raise it");
            continue;
        }
        HwInput in;
        in.at = at;
        in.rising = edge == "rising_edge";
        in.ob = o->number;
        hwInputs_.push_back(in);
    }
}

// A time error: OB 80 when the program has one; without it the CPU goes on
// (an S7-1500 goes to STOP only at twice the maximum cycle time)
void Simulator::timeError(const std::string& why) {
    for (auto& e : iobs_)
        if (e.kind == 'E') {
            if (e.queued < e.maxQueued) ++e.queued;
            return;
        }
    warn(why + "; without a time error OB (OB 80) the CPU goes on");
}

// Events of an OB waiting to run: with Enable time error, a time error when
// they reach the threshold (once until the OB runs again)
void Simulator::eventsWaiting(IntOb& o, int64_t waiting) {
    if (o.threshold <= 0 || o.timeErrorRaised || waiting < o.threshold) return;
    o.timeErrorRaised = true;
    timeError("OB " + o.ob->name + ": " + std::to_string(waiting) + " events waiting (time error threshold " +
              std::to_string(o.threshold) + ")");
}

// Events of an OB lost because too many were waiting
void Simulator::eventsLost(IntOb& o, int64_t n) {
    o.lost += static_cast<int>(std::min<int64_t>(n, 1000000));
    warn("OB " + o.ob->name + ": events lost (more than " + std::to_string(o.maxQueued) + " waiting)" +
         (o.reportOverflow ? "; reported in the diagnostic buffer" : ""));
}

// A statement takes its time; the maximum cycle time; the interrupts that came
void Simulator::tick() {
    // without an instruction time nothing happens between two statements:
    // the interrupts come before and after the OB
    if (instrNs_ == 0) return;
    now_ += instrNs_;
    if (cycleStart_ >= 0) {
        const int64_t used = now_ - cycleStart_;
        if (overruns_ == 0 && used > maxCycleNs_) {
            overruns_ = 1;
            timeError("the maximum cycle time (" + std::to_string(maxCycleNs_ / 1000000) + " ms) was exceeded");
        } else if (overruns_ == 1 && used > 2 * maxCycleNs_) {
            stop("the maximum cycle time (" + std::to_string(maxCycleNs_ / 1000000) + " ms) was exceeded twice in one cycle");
            return;
        }
    }
    for (const auto& o : iobs_)
        if ((o.queued > 0 || o.next <= now_) && o.priority > curPriority_) {
            service(curPriority_);
            return;
        }
}

// Runs the interrupts that are due and of a higher priority than `above`:
// the highest priority first, of the same priority the earliest event
void Simulator::service(int above) {
    for (int guard = 0; guard < 1000000 && running_; ++guard) {
        IntOb* best = nullptr;
        int64_t bestAt = kNever;
        for (auto& o : iobs_) {
            if (o.priority <= above) continue;
            const int64_t at = o.queued > 0 ? std::min(now_, o.next) : o.next;
            if (at > now_) continue;
            if (!best || o.priority > best->priority || (o.priority == best->priority && at < bestAt)) {
                best = &o;
                bestAt = at;
            }
        }
        if (!best) return;
        runInterrupt(*best);
    }
}

void Simulator::runInterrupt(IntOb& o) {
    // the event is taken: one raised (a hardware interrupt, a time error,
    // an event a scenario raises), else the one of its time
    if (o.queued > 0) {
        --o.queued;
    } else if (o.kind == 'C') {
        o.next += o.cycle;
        // events that came while it waited: up to the events it may keep
        // waiting, the rest lost
        int64_t overdue = 0;
        if (o.next <= now_) overdue = (now_ - o.next) / o.cycle + 1;
        const int64_t keep = std::min<int64_t>(overdue, o.maxQueued - 1);
        // the events still waiting, this one taken (for the threshold)
        if (overdue > 0) eventsWaiting(o, overdue + 1);
        if (overdue > keep) {
            eventsLost(o, overdue - keep);
            o.next += (overdue - keep) * o.cycle;
        }
    } else if (o.kind == 'T') {
        const int64_t following = nextTod(o, clockBase_ + o.next + 1);
        o.next = following == kNever ? kNever : following - clockBase_;
        if (o.next == kNever) o.todActive = false;
    } else {
        o.next = kNever;  // a time delay: once
    }
    // the CPU keeps the registers of the OB it interrupts
    const int savedPriority = curPriority_;
    const uint64_t savedSteps = steps_;
    const int savedDb = curDb_, savedDi = curDi_;
    const uint32_t savedAr1 = ar1_, savedAr2 = ar2_;
    const bool savedEno = lastEno_;
    curPriority_ = o.priority;
    steps_ = 0;
    const IntOb info = o;
    o.lost = 0;
    o.ran = true;
    o.timeErrorRaised = false;
    runOb(*o.ob, !info.ran, &info);
    curPriority_ = savedPriority;
    steps_ = savedSteps;
    curDb_ = savedDb;
    curDi_ = savedDi;
    ar1_ = savedAr1;
    ar2_ = savedAr2;
    lastEno_ = savedEno;
}

// The first start of a time-of-day interrupt at or after the clock time `after`
int64_t Simulator::nextTod(const IntOb& o, int64_t after) const {
    if (!o.todActive) return kNever;
    const int64_t start = o.todStart;
    const int64_t last = daysFrom(kLastYear + 1, 1, 1) * kDay;  // no time from here on
    if (after >= last) return kNever;
    auto every = [&](int64_t d) -> int64_t {
        if (start >= after) return start;
        const int64_t t = start + ((after - start + d - 1) / d) * d;
        return t < last ? t : kNever;
    };
    const int64_t day = floorDiv(start, kDay), timeOfDay = start - day * kDay;
    int64_t y, m, d;
    civil(day, y, m, d);
    switch (o.period) {
        case 0x0000: return start >= after ? start : kNever;
        case 0x0201: return every(60 * kSecond);
        case 0x0401: return every(3600 * kSecond);
        case 0x1001: return every(kDay);
        case 0x1201: return every(7 * kDay);
        case 0x1401:  // monthly, on the day of the start: months without it are left out
        case 0x2001:  // at the end of the month
            for (int64_t k = 0; k < 12 * 400; ++k) {
                const int64_t mm = (m - 1 + k) % 12 + 1, yy = y + (m - 1 + k) / 12;
                if (yy > kLastYear) return kNever;
                int64_t dd = o.period == 0x2001 ? monthDays(yy, mm) : d;
                if (dd > monthDays(yy, mm)) continue;
                const int64_t t = daysFrom(yy, mm, dd) * kDay + timeOfDay;
                if (t >= start && t >= after) return t;
            }
            return kNever;
        case 0x1801:  // yearly: a 29 February only in leap years
            for (int64_t k = 0; k < 400; ++k) {
                const int64_t yy = y + k;
                if (yy > kLastYear) return kNever;
                if (d > monthDays(yy, m)) continue;
                const int64_t t = daysFrom(yy, m, d) * kDay + timeOfDay;
                if (t >= after) return t;
            }
            return kNever;
        default: return kNever;
    }
}

// A DTL: a variable of the type DTL or a DTL# constant, as ns since 1970
bool Simulator::readDtl(Frame& f, const Json& e, int64_t& ns) {
    if (e["op"].str() == "const") {
        const Json& v = e["value"];
        if (v.type == Json::Object && v["kind"].str() == "date_and_time" && v["ns"].type == Json::Number) {
            ns = v["ns"].asInt();
            return true;
        }
        return false;
    }
    Place p = placeOf(f, e);
    if (!p.var || p.var->shape != Var::Struct) return false;
    std::map<std::string, int64_t> x;
    for (auto& m : p.var->members) x[up(m.first)] = asInt(m.second->value);
    const int64_t y = x["YEAR"], mo = x["MONTH"], d = x["DAY"];
    if (y < 1970 || y > kLastYear || mo < 1 || mo > 12 || d < 1 || d > monthDays(y, mo) || x["HOUR"] > 23 || x["MINUTE"] > 59 ||
        x["SECOND"] > 59 || x["NANOSECOND"] > 999999999 || x["HOUR"] < 0 || x["MINUTE"] < 0 || x["SECOND"] < 0 ||
        x["NANOSECOND"] < 0)
        return false;
    ns = daysFrom(y, mo, d) * kDay + ((x["HOUR"] * 60 + x["MINUTE"]) * 60 + x["SECOND"]) * kSecond + x["NANOSECOND"];
    return true;
}

void Simulator::writeDtl(const Place& p, int64_t ns) {
    if (!p.var || p.var->shape != Var::Struct) {
        warn("a DTL output that is not a variable of the type DTL: not written");
        return;
    }
    const int64_t day = floorDiv(ns, kDay), rest = ns - day * kDay;
    int64_t y, m, d;
    civil(day, y, m, d);
    const int64_t weekday = ((day % 7) + 7 + 4) % 7 + 1;  // 1970-01-01 was a Thursday; 1 is Sunday
    const std::map<std::string, int64_t> x = {{"YEAR", y},
                                              {"MONTH", m},
                                              {"DAY", d},
                                              {"WEEKDAY", weekday},
                                              {"HOUR", rest / (3600 * kSecond)},
                                              {"MINUTE", rest / (60 * kSecond) % 60},
                                              {"SECOND", rest / kSecond % 60},
                                              {"NANOSECOND", rest % kSecond}};
    for (auto& mem : p.var->members) {
        auto it = x.find(up(mem.first));
        if (it == x.end()) continue;
        Place q;
        q.var = mem.second.get();
        store(q, intVal(it->second));
    }
}

// The instructions of the interrupts and the clock; false when `name` is not one
bool Simulator::interruptCall(Frame& f, const Json& e, const std::string& name, Val& ret) {
    if (!interruptInstructions().count(name)) return false;
    std::map<std::string, const Json*> in, out;
    for (const auto& a : e["args"].items) {
        if (a["args"].items.empty()) continue;
        (a["op"].str() == "out" ? out : in)[up(a["name"].str())] = &a["args"].items[0];
    }
    auto has = [&](const char* n) { return in.count(n) != 0; };
    auto inVal = [&](const char* n) -> Val { return has(n) ? eval(f, *in[n]) : Val{}; };
    auto put = [&](const char* n, const Val& v) {
        auto it = out.find(n);
        if (it == out.end()) return;
        const Json& x = *it->second;
        if (x["op"].str() == "wire") storeWire(f, x["name"].str(), v);
        else store(placeOf(f, x), v);
    };
    const std::string who = "block " + f.block->name + ": " + name;
    int code = kOk;
    // the OB of OB_NR, of the kind the instruction wants
    auto obOf = [&](char kind) -> IntOb* {
        const int nr = static_cast<int>(asInt(inVal("OB_NR")));
        IntOb* o = intOb(nr);
        if (!o || o->kind != kind) {
            code = kWrongOb;
            return nullptr;
        }
        return o;
    };
    auto activate = [&](IntOb& o) {
        o.todActive = true;
        const int64_t first = nextTod(o, clock());
        if (first == kNever) {
            o.todActive = false;
            code = kInThePast;
            return;
        }
        o.next = first - clockBase_;
    };
    if (name == "SRT_DINT") {
        if (IntOb* o = obOf('D')) {
            const Val t = inVal("DTIME");
            const int64_t ms = t.kind == Kind::Time ? t.i / 1000000 : asInt(t);  // a Time in ns, a number in ms
            if (ms < 1 || ms > 60000) {
                code = kWrongValue;
            } else {
                // a start while it runs starts it anew
                o->next = now_ + ms * 1000000;
                o->sign = static_cast<uint16_t>(asInt(inVal("SIGN")) & 0xFFFF);
            }
        }
    } else if (name == "CAN_DINT") {
        if (IntOb* o = obOf('D')) {
            if (o->next == kNever) code = kNotStarted;
            o->next = kNever;
        }
    } else if (name == "QRY_DINT") {
        uint16_t status = 0;
        if (IntOb* o = obOf('D')) status = static_cast<uint16_t>((o->next != kNever ? 0x0004 : 0) | 0x0010);
        put("STATUS", intVal(status, elemType("Word")));
    } else if (name == "SET_TINTL") {
        if (IntOb* o = obOf('T')) {
            int64_t start = 0;
            const uint16_t period = static_cast<uint16_t>(asInt(inVal("PERIOD")) & 0xFFFF);
            static const std::set<uint16_t> periods = {0x0000, 0x0201, 0x0401, 0x1001, 0x1201, 0x1401, 0x1801, 0x2001};
            if (!has("SDT") || !readDtl(f, *in["SDT"], start)) {
                code = kWrongValue;
            } else if (!periods.count(period)) {
                code = kWrongPeriod;
            } else {
                // LOCAL: the CPU has no time zone here, local time is the system time
                o->todSet = true;
                o->todActive = false;
                o->next = kNever;
                o->todStart = start - start % kSecond;  // to the second
                o->period = period;
                if (truthy(inVal("ACTIVATE"))) activate(*o);
            }
        }
    } else if (name == "ACT_TINT") {
        if (IntOb* o = obOf('T')) {
            if (!o->todSet) code = kNotStarted;
            else activate(*o);
        }
    } else if (name == "CAN_TINT") {
        if (IntOb* o = obOf('T')) {
            if (!o->todSet) code = kNotStarted;
            o->todActive = false;
            o->next = kNever;
        }
    } else if (name == "QRY_TINT") {
        uint16_t status = 0;
        if (IntOb* o = obOf('T')) status = static_cast<uint16_t>((o->todActive ? 0x0004 : 0) | 0x0010);
        put("STATUS", intVal(status, elemType("Word")));
    } else if (name == "RD_SYS_T" || name == "RD_LOC_T") {
        auto it = out.find("OUT");
        if (it != out.end()) writeDtl(placeOf(f, *it->second), clock());
    } else if (name == "WR_SYS_T") {
        int64_t t = 0;
        if (!has("IN") || !readDtl(f, *in["IN"], t)) code = kWrongValue;
        else setClock(t);
    }
    if (code != kOk) warn(who + " gives an error (RET_VAL 16#" + [&] {
                              char b[8];
                              std::snprintf(b, sizeof b, "%04X", code);
                              return std::string(b);
                          }() + ")");
    ret = intVal(static_cast<int16_t>(code), elemType("Int"));
    put("RET_VAL", ret);
    lastEno_ = code == kOk;
    put("ENO", boolVal(lastEno_));
    return true;
}

}  // namespace s7forge
