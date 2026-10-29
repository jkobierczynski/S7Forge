// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sim.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace s7forge {
namespace {

constexpr uint64_t kMaxStepsPerCycle = 2000000;  // beyond this the cycle is taken as endless
constexpr int kMaxCallDepth = 64;
constexpr size_t kMaxVariables = 4000000;

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string unquote(const std::string& s) {
    return s.size() >= 2 && s.front() == '"' && s.back() == '"' ? s.substr(1, s.size() - 2) : s;
}

bool sameName(const std::string& a, const std::string& b) { return upper(a) == upper(b); }

Simulator::Place varPlace(Var* v) {
    Simulator::Place p;
    p.var = v;
    return p;
}

bool isConstantKind(const std::string& k) { return k == "constant" || k == "local constant" || k == "user constant"; }

}  // namespace

// ---- loading ----

Simulator::Simulator(Json program) : prog_(std::move(program)) {
    if (prog_["format"].str() != "tiaconv-program")
        throw std::runtime_error("not a program file of tiaconv (--program-json)");
    if (prog_["schema_version"].asInt() != 2)
        throw std::runtime_error("schema version " + std::to_string(prog_["schema_version"].asInt()) +
                                 " of the program file; S7Forge reads version 2");
    size_t budget = kMaxVariables;
    for (const auto& t : prog_["tags"].items) {
        Tag tag;
        tag.name = t["name"].str();
        tag.type = elemType(t["data_type"].str());
        tag.at = parseAddress(t["address"].str());
        // an S5 timer or counter: %T5, %C5
        const std::string dt = t["data_type"].str(), at = t["address"].str();
        if ((dt == "Timer" || dt == "Counter") && at.size() > 2 && at[0] == '%' && (at[1] == 'T' || at[1] == 'C')) {
            s5Tags_[tag.name] = {at[1], std::atoi(at.c_str() + 2)};
            continue;
        }
        if (!tag.at.area || !tag.type) {
            // without an address in I, Q or M: a variable of its own
            Json m;
            m.type = Json::Object;
            m.members.emplace_back("data_type", t["data_type"]);
            m.members.emplace_back("type", t["type"]);
            tag.var = buildVar(m, budget);
        }
        tags_[tag.name] = std::move(tag);
    }
    // the user constants, and the system constants (OB numbers, hardware
    // identifiers) where the code names them
    for (const auto& c : prog_["constants"].items)
        if (c["parsed"].type == Json::Object)
            constants_[c["name"].str()] = fromExport(c["parsed"], elemType(c["data_type"].str()));
    for (const auto& d : prog_["data_blocks"].items) {
        dbs_[d["name"].str()] = buildStruct(d["members"], budget);
        // a data block with standard access set retentive keeps all of it
        if (d["retain_whole"].type == Json::Bool && d["retain_whole"].b) dbs_[d["name"].str()]->retain = true;
        DbInfo info;
        info.name = d["name"].str();
        info.number = static_cast<int>(d["number"].asInt());
        info.optimized = d["optimized"].type != Json::Bool || d["optimized"].b;
        info.json = &d;
        if (info.number > 0) dbInfo_[info.number] = std::move(info);
    }
    for (const auto& b : prog_["blocks"].items) {
        Block blk;
        blk.json = &b;
        blk.name = b["name"].str();
        blk.type = b["type"].str();
        blk.language = b["language"].str();
        if (b["interface"].type == Json::Array) blk.interface = &b["interface"];
        if (b["code"].type == Json::Object) {
            blk.code = &b["code"];
            for (const auto& r : b["code"]["references"].items) blk.refs[r["ref_id"].asInt()] = &r;
            blk.stl = stlCode(b["code"]["networks"]);
        }
        blocks_[blk.name] = std::move(blk);
    }
    const Json& rt = prog_["runtime"];
    auto ob = [&](const Json& name) -> const Block* {
        auto it = blocks_.find(name.str());
        return it == blocks_.end() ? nullptr : &it->second;
    };
    for (const auto& n : rt["startup"].items)
        if (const Block* b = ob(n)) startup_.push_back(b);
    for (const auto& n : rt["program_cycle"].items)
        if (const Block* b = ob(n)) cycle_.push_back(b);
    for (const auto& n : rt["interrupts"].items) {
        const Block* b = ob(n);
        const Json* info = nullptr;
        for (const auto& o : prog_["organization_blocks"].items)
            if (o["name"].str() == n.str()) info = &o;
        if (!b || !info) {
            warn("OB " + n.str() + ": not in the program, its events do nothing");
            continue;
        }
        IntOb o;
        o.ob = b;
        o.number = static_cast<int>((*info)["number"].type == Json::Number ? (*info)["number"].asInt()
                                                                            : (*b->json)["number"].asInt());
        o.priority = static_cast<int>(std::max<int64_t>(2, std::min<int64_t>(26, (*info)["priority"].asInt())));
        o.maxQueued = static_cast<int>(std::max<int64_t>(1, std::min<int64_t>(12, (*info)["max_buffered_events"].asInt())));
        // the kind of event: by its name, its event class, else its number
        const std::string ev = (*info)["event"].str();
        const int64_t cls = (*info)["event_class"].asInt();
        auto has = [&](const char* w) { return ev.find(w) != std::string::npos; };
        const int nr = o.number;
        if (has("Cyclic") || cls == 30 || (cls == 0 && nr >= 30 && nr <= 38)) o.kind = 'C';
        else if (has("Delay") || cls == 20 || (cls == 0 && nr >= 20 && nr <= 23)) o.kind = 'D';
        else if (has("TimeOfDay") || cls == 10 || (cls == 0 && nr >= 10 && nr <= 17)) o.kind = 'T';
        else if (has("Hardware") || has("HWProcess") || cls == 40 || (cls == 0 && nr >= 40 && nr <= 47)) o.kind = 'H';
        else if (has("TimeError") || nr == 80) o.kind = 'E';
        else o.kind = 'O';
        if ((*info)["time_error_threshold"].type == Json::Number)
            o.threshold = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(12, (*info)["time_error_threshold"].asInt())));
        if ((*info)["report_event_overflow"].type == Json::Bool) o.reportOverflow = (*info)["report_event_overflow"].b;
        if (o.kind == 'C') {
            if ((*info)["cycle_us"].type != Json::Number || (*info)["cycle_us"].asInt() <= 0) {
                warn("OB " + n.str() + ": a cyclic interrupt without a cycle: it does not run");
                continue;
            }
            // at most a day; the CPU allows up to a minute
            o.cycle = std::min<int64_t>((*info)["cycle_us"].asInt(), 86400000000LL) * 1000;
            o.phase = std::min<int64_t>(std::max<int64_t>(0, (*info)["phase_shift_us"].asInt()), 86400000000LL) * 1000;
        } else if (o.kind == 'T') {
            if ((*info)["time_of_day"].type == Json::Object) configureTod(o, (*info)["time_of_day"]);
        } else if (o.kind == 'O') {
            warn("OB " + n.str() + " (" + ev + ") handles an event the simulation does not raise; a scenario can raise it");
        }
        iobs_.push_back(o);
    }
    loadHardwareInterrupts();
    // the CPU's settings; a program file without them keeps TIA Portal's
    // defaults (150 ms, no minimum, mode before power off, nothing retentive)
    const Json& cpu = prog_["cpu_settings"];
    if (cpu.type == Json::Object) {
        if (cpu["max_cycle_ms"].type == Json::Number && cpu["max_cycle_ms"].asInt() > 0)
            maxCycleNs_ = std::min<int64_t>(cpu["max_cycle_ms"].asInt(), 6000) * 1000000;
        if (cpu["min_cycle_ms"].type == Json::Number && cpu["min_cycle_ms"].asInt() > 0)
            scanNs_ = std::min<int64_t>(cpu["min_cycle_ms"].asInt(), 6000) * 1000000;
        if (cpu["startup"].type == Json::String) {
            const std::string s = cpu["startup"].str();
            if (s == "warm_restart_run" || s == "warm_restart_previous_mode" || s == "no_restart") startupMode_ = s;
            else warn("CPU startup " + s + " not known: Warm restart - mode before POWER OFF taken");
        }
        const Json& r = cpu["retentive_memory"];
        if (r.type == Json::Object) {
            retainBytes_ = static_cast<size_t>(std::max<int64_t>(0, r["memory_bytes"].asInt()));
            retainTimers_ = static_cast<size_t>(std::max<int64_t>(0, r["s7_timers"].asInt()));
            retainCounters_ = static_cast<size_t>(std::max<int64_t>(0, r["s7_counters"].asInt()));
        }
    }
    if (cycle_.empty()) warn("no program cycle OB");
}

std::string Simulator::plcName() const { return prog_["plc"]["name"].str(); }

void Simulator::warn(const std::string& w) {
    if (warned_.insert(w).second) warnings_.push_back(w);
}

void Simulator::stop(const std::string& why) {
    if (!running_) return;
    running_ = false;
    fault_ = why;
}

// ---- running ----

void Simulator::resetMemory(bool retainedToo) {
    for (auto& d : dbs_) d.second->reset(retainedToo);
    for (auto& t : tags_)
        if (t.second.var) t.second.var->reset(retainedToo);
    // the outputs; the bit memory, S5 timers and counters but for their
    // retentive ranges (the Retain dialog of the tag table: MB0 on, T0 on,
    // C0 on); the inputs are what the field gives, they stay
    const size_t keepM = retainedToo ? 0 : std::min<size_t>(retainBytes_, m_.bytes.size());
    const size_t keepT = retainedToo ? 0 : std::min<size_t>(retainTimers_, t5_.size());
    const size_t keepC = retainedToo ? 0 : std::min<size_t>(retainCounters_, c5_.size());
    std::fill(q_.bytes.begin(), q_.bytes.end(), 0);
    std::fill(m_.bytes.begin() + static_cast<std::ptrdiff_t>(keepM), m_.bytes.end(), 0);
    timers_.clear();
    std::fill(t5_.begin() + static_cast<std::ptrdiff_t>(keepT), t5_.end(), S5Timer{});
    std::fill(c5_.begin() + static_cast<std::ptrdiff_t>(keepC), c5_.end(), S5Counter{});
    curDb_ = curDi_ = 0;
    ar1_ = ar2_ = 0;
    // the bytes of a data block with standard access (its padding): kept
    // for one retentive as a whole
    for (auto& d : dbInfo_) {
        auto v = dbs_.find(d.second.name);
        if (retainedToo || v == dbs_.end() || !v->second->retain) d.second.image.clear();
    }
    mcrStack_.clear();
}

void Simulator::powerOn() {
    resetMemory(true);
    warmRestart();
}

void Simulator::powerCycle() {
    // what was in RUN before the power went off
    const bool wasRunning = running_;
    resetMemory(false);
    if (startupMode_ == "no_restart" || (startupMode_ == "warm_restart_previous_mode" && !wasRunning)) {
        running_ = false;
        fault_ = startupMode_ == "no_restart" ? "STOP after power on: the startup setting is No restart"
                                          : "STOP after power on: the CPU was in STOP before power off";
        return;
    }
    warmRestart();
}

void Simulator::warmRestart() {
    resetMemory(false);
    running_ = true;
    fault_.clear();
    firstScan_ = true;
    steps_ = 0;
    // the events of the interrupts start anew: time delays and time-of-day
    // interrupts set by the program are gone, the cyclic ones start their cycle
    for (auto& o : iobs_) {
        o.next = o.kind == 'C' ? now_ + o.phase + o.cycle : kNever;
        o.queued = o.lost = 0;
        o.ran = o.timeErrorRaised = false;
        o.todSet = o.todActive = false;
        // a time of day set in the OB's properties is active from the start
        if (o.cfgTod) {
            o.todSet = o.todActive = true;
            o.todStart = o.cfgStart;
            o.period = o.cfgPeriod;
            const int64_t t = nextTod(o, clock());
            o.next = t == kNever ? kNever : t - clockBase_;
            if (o.next == kNever) o.todActive = false;
        }
    }
    cycleStart_ = -1;
    // nothing interrupts the startup OBs: the events wait for the first cycle
    curPriority_ = 100;
    for (const Block* b : startup_) runOb(*b, true);
    curPriority_ = 0;
}

void Simulator::scan() {
    if (!running_) return;
    const int64_t start = now_;
    steps_ = 0;
    cycleStart_ = start;
    overruns_ = 0;
    curPriority_ = 1;
    service(1);  // an event already there comes before the cycle
    for (const Block* b : cycle_) {
        runOb(*b, firstScan_);
        if (!running_) break;
    }
    curPriority_ = 0;
    if (!running_) {
        cycleStart_ = -1;
        return;
    }
    firstScan_ = false;
    // up to the end of the cycle (the scan time, or longer when the program
    // took longer) the interrupts as their events come
    const int64_t until = std::max(start + scanNs_, now_);
    for (int guard = 0; guard < 1000000 && running_; ++guard) {
        int64_t at = kNever;
        for (const auto& o : iobs_) at = std::min(at, o.queued > 0 ? now_ : o.next);
        if (at > until) break;
        now_ = std::max(now_, at);
        // the interrupts after OB 1 count in the cycle time as well: an
        // overload calls the time error OB and at twice the maximum stops the CPU
        service(0);
    }
    cycleStart_ = -1;
    now_ = std::max(now_, until);
}

void Simulator::runFor(int64_t ns) {
    const int64_t end = now_ + ns;
    while (running_ && now_ < end) scan();
    if (!running_ && now_ < end) now_ = end;
}

std::unique_ptr<Var> Simulator::frameLocals(const Block& b, const std::vector<std::string>& sections) {
    size_t budget = kMaxVariables;
    if (!b.interface) {
        auto v = std::make_unique<Var>();
        v->shape = Var::Struct;
        return v;
    }
    return buildStruct(*b.interface, budget, sections);
}

void Simulator::runOb(const Block& ob, bool initialCall, const IntOb* info) {
    // an OB starts with an empty MCR stack; an interrupted one gets its own back
    std::vector<bool> mcr;
    mcr.swap(mcrStack_);
    struct Back {
        std::vector<bool>& stack;
        std::vector<bool>& saved;
        ~Back() { stack.swap(saved); }
    } back{mcrStack_, mcr};
    Frame f;
    f.block = &ob;
    f.local = frameLocals(ob, {"Input", "Temp", "Constant"});
    // the start information the OB has
    if (Var* ic = f.local->member("Initial_Call")) store(varPlace(ic), boolVal(initialCall));
    if (info) {
        if (Var* v = f.local->member("Event_Count")) store(varPlace(v), intVal(info->lost));
        if (Var* v = f.local->member("Sign")) store(varPlace(v), intVal(info->sign));
    }
    runBlock(f);
}

void Simulator::runBlock(Frame& f) {
    const Block& b = *f.block;
    if (!b.code) {
        warn("block " + b.name + " has no code to run (" +
             ((*b.json)["protection"].str().empty() ? std::string("not read") : (*b.json)["protection"].str() + " protected") +
             "): calling it does nothing");
        return;
    }
    const auto& nets = b.code->operator[]("networks").items;
    for (size_t k = 0; k < nets.size();) {
        const Json& n = nets[k];
        if (n["statements"].type != Json::Array) {
            warn("block " + b.name + ", network " + std::to_string(n["number"].asInt()) + " was not read: not run");
            ++k;
            continue;
        }
        if (b.stl && b.stl->first[k] != StlCode::npos()) {
            k = runStl(f, k, b.stl->first[k]);
        } else {
            f.wires.clear();
            exec(f, n["statements"]);
            ++k;
        }
        if (f.returned || !running_) break;
    }
    // an STL block gives BR as its ENO
    if (f.stl && b.language == "STL") f.eno = f.stl->br;
}

// ---- operands ----

const Json* Simulator::ref(const Frame& f, const Json& e) const {
    if (e["ref_id"].type != Json::Number || !f.block) return nullptr;
    auto it = f.block->refs.find(e["ref_id"].asInt());
    return it == f.block->refs.end() ? nullptr : it->second;
}

bool Simulator::constantOf(const Json& r, Val& v) const {
    if (isConstantKind(r["kind"].str())) {
        v = fromExport(r["value"], elemType(r["data_type"].str()));
        return true;
    }
    // a user constant, referred to as a tag
    if (r["kind"].str() == "tag") {
        const std::string name = r["name"].str().empty() ? unquote(r["text"].str()) : r["name"].str();
        if (tags_.count(name)) return false;
        auto c = constants_.find(name);
        if (c != constants_.end()) {
            v = c->second;
            return true;
        }
    }
    return false;
}

Simulator::Place Simulator::area(const Address& at, const ElemType* t) {
    Place p;
    p.area = at.area == 'I' ? &i_ : at.area == 'Q' ? &q_ : &m_;
    p.at = at;
    p.type = t;
    return p;
}

namespace {

// %X3, %B0, x3, b0: a part of an integer
bool parseSlice(const std::string& s, int& bits, int& index) {
    std::string t = upper(s);
    if (!t.empty() && t[0] == '%') t = t.substr(1);
    if (t.size() < 2) return false;
    switch (t[0]) {
        case 'X': bits = 1; break;
        case 'B': bits = 8; break;
        case 'W': bits = 16; break;
        case 'D': bits = 32; break;
        default: return false;
    }
    char* end = nullptr;
    const long n = std::strtol(t.c_str() + 1, &end, 10);
    if (end == t.c_str() + 1 || *end != '\0' || n < 0 || (n + 1) * bits > 64) return false;
    index = static_cast<int>(n);
    return true;
}

}  // namespace

// The steps of a path from `from` on: members, and array indices given as
// ref_ids of the block's references.
Simulator::Place Simulator::follow(Frame& f, Place p, const Json& path, size_t from) {
    for (size_t k = from; k < path.items.size() && p.var; ++k) {
        const Json& step = path.items[k];
        if (k > 0) {  // the first step is the root, already found
            Var* m = p.var->member(step["name"].str());
            if (!m) {
                warn("no member " + step["name"].str() + " in " + p.var->dataType);
                return Place{};
            }
            p.var = m;
        }
        if (!step["index"].items.empty()) {
            std::vector<int64_t> idx;
            for (const auto& i : step["index"].items) {
                auto it = f.block->refs.find(i.asInt());
                if (it == f.block->refs.end()) return Place{};
                const Json& r = *it->second;
                Val v;
                if (!constantOf(r, v)) v = load(resolveRef(f, r));
                idx.push_back(asInt(v));
            }
            Var* e = p.var->element(idx);
            if (!e) {
                stop("array index out of bounds in " + p.var->dataType + " (block " + f.block->name + ")");
                return Place{};
            }
            p.var = e;
        }
    }
    return p;
}

Simulator::Place Simulator::resolveRef(Frame& f, const Json& r) {
    const std::string& kind = r["kind"].str();
    const Json& path = r["path"];
    Place p;
    if (kind == "tag") {
        const std::string name = r["name"].str().empty() ? unquote(r["text"].str()) : r["name"].str();
        auto t = tags_.find(name);
        if (t == tags_.end()) {
            if (constants_.count(name))
                warn("block " + f.block->name + " writes the constant " + r["text"].str() + ": not done");
            else
                warn("tag " + r["text"].str() + " not in the tag tables");
            return p;
        }
        if (t->second.var) p.var = t->second.var.get();
        else p = area(t->second.at, t->second.type);
    } else if (kind == "data block member") {
        if (path.items.empty()) return p;
        auto d = dbs_.find(path.items[0]["name"].str());
        if (d == dbs_.end()) {
            warn("data block " + path.items[0]["name"].str() + " not in the program");
            return p;
        }
        p.var = d->second.get();
        p = follow(f, p, path, 1);
    } else if (kind == "data block" || kind == "instance data block") {
        auto d = dbs_.find(r["name"].str());
        if (d != dbs_.end()) p.var = d->second.get();
    } else if (kind == "local" || kind == "multi-instance") {
        std::string root = path.items.empty() ? r["name"].str() : path.items[0]["name"].str();
        if (root.empty()) root = r["text"].str();
        if (!root.empty() && root[0] == '#') root = root.substr(1);
        root = unquote(root);
        auto io = f.inOut.find(root);
        if (io != f.inOut.end()) {
            p = io->second;
        } else if (Var* v = f.local ? f.local->member(root) : nullptr) {
            p.var = v;
        } else if (Var* w = f.instance ? f.instance->member(root) : nullptr) {
            p.var = w;
        } else if (f.local && sameName(root, f.block->name)) {
            // an FC's own name in SCL: its return value
            for (auto& m : f.local->members)
                if (m.second->section == "Return") p.var = m.second.get();
        }
        if (!p.valid()) {
            warn("block " + f.block->name + ": " + r["text"].str() + " not found in its interface");
            return p;
        }
        // the root's own indices, then the members and theirs
        if (!path.items.empty() && p.var) p = follow(f, p, path, 0);
    } else {
        warn("an operand of the kind '" + kind + "' (" + r["text"].str() + ") is not a place to read or write");
        return p;
    }
    int bits = 0, index = 0;
    if (p.valid() && !r["modifier"].str().empty() && parseSlice(r["modifier"].str(), bits, index)) {
        p.sliceBits = bits;
        p.sliceIndex = index;
    }
    return p;
}

Simulator::Place Simulator::placeOf(Frame& f, const Json& e) {
    const std::string& op = e["op"].str();
    if (op == "ref" || ((op == "member" || op == "index") && ref(f, e))) {
        const Json* r = ref(f, e);
        if (!r) {
            warn("block " + f.block->name + ": an operand without its reference (" + e["name"].str() + ")");
            return Place{};
        }
        return resolveRef(f, *r);
    }
    if (op == "member") {
        Place p = placeOf(f, e["args"].items.at(0));
        if (!p.var) return Place{};
        Var* m = p.var->member(e["name"].str());
        if (!m) return Place{};
        p.var = m;
        return p;
    }
    if (op == "index") {
        Place p = placeOf(f, e["args"].items.at(0));
        if (!p.var) return Place{};
        std::vector<int64_t> idx;
        for (size_t k = 1; k < e["args"].items.size(); ++k) idx.push_back(asInt(eval(f, e["args"].items[k])));
        Var* el = p.var->element(idx);
        if (!el) {
            stop("array index out of bounds in block " + f.block->name);
            return Place{};
        }
        p.var = el;
        return p;
    }
    if (op == "slice") {
        Place p = placeOf(f, e["args"].items.at(0));
        int bits = 0, index = 0;
        if (p.valid() && parseSlice(e["name"].str(), bits, index)) {
            p.sliceBits = bits;
            p.sliceIndex = index;
        }
        return p;
    }
    warn("block " + f.block->name + ": an expression of the kind '" + op + "' written to");
    return Place{};
}

Val Simulator::load(const Place& p) {
    Val v;
    if (p.area) v = readArea(*p.area, p.at, *p.type);
    else if (p.var && p.var->shape == Var::Leaf) v = p.var->value;
    else return v;
    if (p.sliceBits) {
        const uint64_t raw = static_cast<uint64_t>(v.i) >> (p.sliceBits * p.sliceIndex);
        const uint64_t mask = p.sliceBits >= 64 ? ~0ULL : ((1ULL << p.sliceBits) - 1);
        if (p.sliceBits == 1) return boolVal(raw & 1);
        return intVal(static_cast<int64_t>(raw & mask),
                      elemType(p.sliceBits == 8 ? "Byte" : p.sliceBits == 16 ? "Word" : "DWord"));
    }
    return v;
}

void Simulator::store(const Place& p, const Val& v) {
    if (!p.valid() || v.kind == Kind::None) return;  // no value (a function not simulated): the place keeps its own
    if (p.sliceBits) {
        Place whole = p;
        whole.sliceBits = 0;
        Val base = load(whole);
        const int shift = p.sliceBits * p.sliceIndex;
        const uint64_t mask = (p.sliceBits >= 64 ? ~0ULL : ((1ULL << p.sliceBits) - 1)) << shift;
        const uint64_t bits = (static_cast<uint64_t>(p.sliceBits == 1 ? (truthy(v) ? 1 : 0) : asInt(v)) << shift) & mask;
        base.i = static_cast<int64_t>((static_cast<uint64_t>(base.i) & ~mask) | bits);
        store(whole, base);
        return;
    }
    if (p.area) {
        writeArea(*p.area, p.at, *p.type, v);
        return;
    }
    if (p.var->shape != Var::Leaf) {
        warn("a value written to the structure " + p.var->dataType);
        return;
    }
    p.var->value = p.var->type ? convertTo(v, *p.var->type, p.var->maxLen) : v;
}

void Simulator::storeWire(Frame& f, const std::string& name, const Val& v) { f.wires[name] = v; }

void Simulator::assign(Frame& f, const Json& target, const Json& value) {
    if (target["op"].str() == "wire") {
        storeWire(f, target["name"].str(), eval(f, value));
        return;
    }
    if (target["op"].str() == "ref") {
        // a timer or counter: the reset coil (R) writes FALSE to it
        const Json* r = ref(f, target);
        char kind = 0;
        int n = 0;
        if (r && s5Ref(f, *r, kind, n)) {
            if (!truthy(eval(f, value))) s5Reset(f, *r);
            else warn("block " + f.block->name + ": TRUE written to the " + (kind == 'T' ? "timer " : "counter ") +
                      (*r)["text"].str() + ": not done");
            return;
        }
    }
    if (target["op"].str() == "builtin") {  // ENO := ...
        f.eno = truthy(eval(f, value));
        return;
    }
    Place p = placeOf(f, target);
    if (!p.valid()) return;
    if (p.var && p.var->shape != Var::Leaf && !p.sliceBits) {
        // a structure or array: copied from the place the value is in
        const std::string& op = value["op"].str();
        if (op == "ref" || op == "member" || op == "index") {
            Place s = placeOf(f, value);
            if (s.var) copyValues(*p.var, *s.var);
            return;
        }
        warn("block " + f.block->name + ": a structure assigned from an expression");
        return;
    }
    store(p, eval(f, value));
}

// ---- expressions ----

Val Simulator::eval(Frame& f, const Json& e) {
    const std::string& op = e["op"].str();
    const Json& args = e["args"];
    if (op == "ref") {
        const Json* r = ref(f, e);
        if (!r) {
            warn("block " + f.block->name + ": an operand without its reference (" + e["name"].str() + ")");
            return Val{};
        }
        Val c;
        if (constantOf(*r, c)) return c;
        bool status;
        if (s5Status(f, *r, status)) return boolVal(status);  // a contact of a timer or counter
        return load(resolveRef(f, *r));
    }
    if (op == "const") {
        if (e["value"].type == Json::Object) return fromExport(e["value"], elemType(e["type"].str()));
        return Val{};
    }
    if (op == "member" || op == "index" || op == "slice") return load(placeOf(f, e));
    if (op == "not") {
        Val a = eval(f, args.items.at(0));
        if (a.kind == Kind::Bool) return boolVal(!a.b);
        Val r = intVal(~a.i, a.type);
        return a.type ? convertTo(r, *a.type) : r;
    }
    if (op == "neg") {
        Val a = eval(f, args.items.at(0));
        if (a.kind == Kind::Real) return realVal(-a.r, a.type);
        Val r = a;
        r.i = static_cast<int64_t>(0 - static_cast<uint64_t>(a.i));
        return a.type && a.kind == Kind::Int ? convertTo(r, *a.type) : r;
    }
    if (op == "call") return call(f, e);
    if (op == "edge_pos" || op == "edge_neg") {
        const bool sig = truthy(eval(f, args.items.at(0)));
        Place mem = placeOf(f, args.items.at(1));
        const bool old = truthy(load(mem));
        store(mem, boolVal(sig));
        return boolVal(op == "edge_pos" ? (sig && !old) : (!sig && old));
    }
    if (op == "wire") {
        auto it = f.wires.find(e["name"].str());
        return it == f.wires.end() ? boolVal(false) : it->second;
    }
    if (op == "builtin") return boolVal(f.eno);
    if (op == "missing") {
        warn("block " + f.block->name + ": an operand that was not entered");
        return boolVal(false);
    }
    if (args.items.size() == 2) {
        // every operand is evaluated: an edge in one of them updates its memory
        const Val a = eval(f, args.items[0]);
        const Val b = eval(f, args.items[1]);
        return binary(op, a, b, elemType(e["type"].str()));
    }
    warn("block " + f.block->name + ": an expression of the kind '" + op + "' is not simulated");
    return Val{};
}

Val Simulator::binary(const std::string& op, const Val& a, const Val& b, const ElemType* rt) {
    const bool real = a.kind == Kind::Real || b.kind == Kind::Real;
    const bool time = a.kind == Kind::Time || b.kind == Kind::Time;
    if (op == "eq" || op == "ne" || op == "lt" || op == "le" || op == "gt" || op == "ge") {
        int c;
        if (a.kind == Kind::String || b.kind == Kind::String) c = a.s < b.s ? -1 : a.s > b.s ? 1 : 0;
        else if (real) c = asReal(a) < asReal(b) ? -1 : asReal(a) > asReal(b) ? 1 : 0;
        else c = asInt(a) < asInt(b) ? -1 : asInt(a) > asInt(b) ? 1 : 0;
        if (op == "eq") return boolVal(c == 0);
        if (op == "ne") return boolVal(c != 0);
        if (op == "lt") return boolVal(c < 0);
        if (op == "le") return boolVal(c <= 0);
        if (op == "gt") return boolVal(c > 0);
        return boolVal(c >= 0);
    }
    const ElemType* t = rt ? rt : (a.type && a.kind == Kind::Int ? a.type : b.type);
    if (op == "and" || op == "or" || op == "xor") {
        if (a.kind == Kind::Bool && b.kind == Kind::Bool) {
            if (op == "and") return boolVal(a.b && b.b);
            if (op == "or") return boolVal(a.b || b.b);
            return boolVal(a.b != b.b);
        }
        const int64_t x = asInt(a), y = asInt(b);
        Val r = intVal(op == "and" ? (x & y) : op == "or" ? (x | y) : (x ^ y), t);
        return t && t->kind == Kind::Int ? convertTo(r, *t) : r;
    }
    if (op == "pow") return realVal(std::pow(asReal(a), asReal(b)), rt && rt->kind == Kind::Real ? rt : elemType("LReal"));
    if (real) {
        const double x = asReal(a), y = asReal(b);
        double r = 0;
        if (op == "add") r = x + y;
        else if (op == "sub") r = x - y;
        else if (op == "mul") r = x * y;
        else if (op == "div") r = x / y;  // a division by zero gives Inf, as in the CPU
        else if (op == "mod") r = std::fmod(x, y);
        else return Val{};
        const ElemType* rr = rt && rt->kind == Kind::Real ? rt : (a.kind == Kind::Real ? a.type : b.type);
        Val v = realVal(r, rr);
        return rr ? convertTo(v, *rr) : v;
    }
    // in 64 bits, wrapping as the CPU does (the result is then cut to its type)
    auto wrapAdd = [](int64_t p, int64_t q) { return static_cast<int64_t>(static_cast<uint64_t>(p) + static_cast<uint64_t>(q)); };
    auto wrapSub = [](int64_t p, int64_t q) { return static_cast<int64_t>(static_cast<uint64_t>(p) - static_cast<uint64_t>(q)); };
    auto wrapMul = [](int64_t p, int64_t q) { return static_cast<int64_t>(static_cast<uint64_t>(p) * static_cast<uint64_t>(q)); };
    auto divide = [&](int64_t p, int64_t q, bool mod) -> int64_t {
        if (q == 0) {
            warn("a division by zero (the result is 0)");
            return 0;
        }
        if (p == INT64_MIN && q == -1) return mod ? 0 : INT64_MIN;  // overflows, as LInt does
        return mod ? p % q : p / q;
    };
    const int64_t x = asInt(a), y = asInt(b);
    int64_t r = 0;
    if (op == "add") r = wrapAdd(x, y);
    else if (op == "sub") r = wrapSub(x, y);
    else if (op == "mul") r = wrapMul(x, y);
    else if (op == "div" || op == "mod") r = divide(x, y, op == "mod");
    else return Val{};
    if (time) {
        // Time +- Time, Time * n, Time / n stay a Time; Time / Time is a number
        if (op == "div" && a.kind == Kind::Time && b.kind == Kind::Time) return intVal(r, elemType("DInt"));
        const ElemType* tt = a.kind == Kind::Time ? a.type : b.type;
        Val v = timeVal(r, tt);
        return tt ? convertTo(v, *tt) : v;
    }
    Val v = intVal(r, t);
    return t && t->kind == Kind::Int ? convertTo(v, *t) : v;
}

// ---- statements ----

void Simulator::exec(Frame& f, const Json& list) {
    for (const auto& s : list.items) {
        if (!running_ || f.returned || f.loop) return;
        stmt(f, s);
    }
}

void Simulator::stmt(Frame& f, const Json& s) {
    if (++steps_ > kMaxStepsPerCycle) {
        stop("the cycle did not end (an endless loop?): maximum cycle time exceeded");
        return;
    }
    tick();
    if (!running_) return;
    const std::string& op = s["op"].str();
    const auto& x = s["exprs"].items;
    if (op == "assign") {
        if (x.size() == 2) assign(f, x[0], x[1]);
    } else if (op == "call") {
        if (!x.empty()) call(f, x[0]);
    } else if (op == "if") {
        for (const auto& b : s["branches"].items) {
            if (truthy(eval(f, b["exprs"].items.at(0)))) {
                exec(f, b["body"]);
                return;
            }
        }
        if (s["else"].type == Json::Array) exec(f, s["else"]);
    } else if (op == "case") {
        const Val sel = eval(f, x.at(0));
        const int64_t v = asInt(sel);
        for (const auto& b : s["branches"].items) {
            for (const auto& l : b["exprs"].items) {
                bool hit;
                if (l["op"].str() == "range")
                    hit = v >= asInt(eval(f, l["args"].items.at(0))) && v <= asInt(eval(f, l["args"].items.at(1)));
                else
                    hit = v == asInt(eval(f, l));
                if (hit) {
                    exec(f, b["body"]);
                    return;
                }
            }
        }
        if (s["else"].type == Json::Array) exec(f, s["else"]);
    } else if (op == "for") {
        Place var = placeOf(f, x.at(0));
        if (!var.valid()) {
            warn("block " + f.block->name + ": a FOR loop whose variable was not found: not run");
            return;
        }
        const int64_t to = asInt(eval(f, x.at(2)));
        const int64_t by = x.size() > 3 ? asInt(eval(f, x[3])) : 1;
        if (by == 0) {
            stop("a FOR loop with a step of 0 in block " + f.block->name);
            return;
        }
        store(var, eval(f, x.at(1)));
        for (;;) {
            const int64_t i = asInt(load(var));
            if (by > 0 ? i > to : i < to) break;
            exec(f, s["body"]);
            if (f.loop == 1) {
                f.loop = 0;
                break;
            }
            f.loop = 0;
            if (f.returned || !running_) break;
            if (++steps_ > kMaxStepsPerCycle) {
                stop("the cycle did not end (an endless loop?): maximum cycle time exceeded");
                break;
            }
            tick();  // a turn of the loop takes its time, also with an empty body
            if (!running_) break;
            // past the end of the type the loop variable wraps: the loop goes on, as in the CPU
            const int64_t before = asInt(load(var));
            store(var, intVal(static_cast<int64_t>(static_cast<uint64_t>(before) + static_cast<uint64_t>(by))));
        }
    } else if (op == "while") {
        while (running_ && truthy(eval(f, x.at(0)))) {
            exec(f, s["body"]);
            if (f.loop == 1) {
                f.loop = 0;
                break;
            }
            f.loop = 0;
            if (f.returned) break;
            if (++steps_ > kMaxStepsPerCycle) stop("the cycle did not end (an endless loop?): maximum cycle time exceeded");
            else tick();
        }
    } else if (op == "repeat") {
        do {
            exec(f, s["body"]);
            if (f.loop == 1) {
                f.loop = 0;
                break;
            }
            f.loop = 0;
            if (f.returned) break;
            if (++steps_ > kMaxStepsPerCycle) stop("the cycle did not end (an endless loop?): maximum cycle time exceeded");
            else tick();
        } while (running_ && !truthy(eval(f, x.at(0))));
    } else if (op == "return") {
        f.returned = true;
    } else if (op == "exit") {
        f.loop = 1;
    } else if (op == "continue") {
        f.loop = 2;
    } else if (op == "region") {
        exec(f, s["body"]);
    } else if (op == "eval") {
        if (!x.empty()) eval(f, x[0]);
    } else if (op == "empty" || op == "label") {
    } else if (op == "stl") {
        warn("block " + f.block->name + ": an STL statement outside an STL network: not run");
    } else {
        warn("block " + f.block->name + ": a statement of the kind '" + op + "' (" + s["name"].str() + ") is not simulated");
    }
}

// ---- calls ----

Val Simulator::call(Frame& f, const Json& e) {
    const Json& t = e["target"];
    const std::string& kind = t["kind"].str();
    if (!enabled(f, e)) return Val{};
    // the boxes and coils of S5 timers and counters (S_ODT, SD, ...)
    if (kind != "block" && s5Call(f, e, t["name"].str().empty() ? e["name"].str() : t["name"].str())) {
        for (const auto& a : e["args"].items)
            if (sameName(a["name"].str(), "ENO") && a["op"].str() == "out" && !a["args"].items.empty()) {
                const Json& to = a["args"].items[0];
                if (to["op"].str() == "wire") storeWire(f, to["name"].str(), boolVal(true));
                else store(placeOf(f, to), boolVal(true));
            }
        lastEno_ = true;
        return Val{};
    }
    if (kind != "block") {
        Val ret;
        const std::string iname = upper(t["name"].str().empty() ? e["name"].str() : t["name"].str());
        if (interruptCall(f, e, iname, ret)) return ret;
        if (libraryCall(f, e, iname, ret)) return ret;
    }
    if (kind == "block") return callBlock(f, e, t);
    if (kind == "instruction") return callInstruction(f, e, t);
    if (kind == "builtin" || t.type != Json::Object) {
        Val v = builtin(f, e);
        for (const auto& a : e["args"].items)
            if (sameName(a["name"].str(), "ENO") && a["op"].str() == "out" && !a["args"].items.empty()) {
                const Json& to = a["args"].items[0];
                if (to["op"].str() == "wire") storeWire(f, to["name"].str(), boolVal(true));
                else store(placeOf(f, to), boolVal(true));
            }
        return v;
    }
    warn("a call of " + e["name"].str() + ", which is not in the program");
    return Val{};
}

Var* Simulator::instanceOf(Frame& f, const Json& target) {
    const Json& inst = target["instance"];
    if (inst.type != Json::Object) return nullptr;
    const std::string& kind = inst["kind"].str();
    if (kind == "data_block") {
        auto d = dbs_.find(inst["name"].str());
        return d == dbs_.end() ? nullptr : d->second.get();
    }
    if (kind == "multi_instance") {
        Place p;
        const Json& path = inst["path"];
        if (path.items.empty()) return nullptr;
        const std::string& root = path.items[0]["name"].str();
        // an FC's in-out: the instance passed by reference
        auto io = f.inOut.find(root);
        if (io != f.inOut.end()) p = io->second;
        if (!p.var && f.instance) p.var = f.instance->member(root);
        if (!p.var && f.local) p.var = f.local->member(root);
        if (!p.var) return nullptr;
        p = follow(f, p, path, 0);
        return p.var;
    }
    return nullptr;
}

namespace {

struct ParamOut {
    Var* from;
    const Json* to;
    bool inOut;
};

}  // namespace

Val Simulator::callBlock(Frame& f, const Json& e, const Json& target) {
    auto bi = blocks_.find(target["name"].str());
    if (bi == blocks_.end()) {
        warn("a call of " + target["name"].str() + ", which is not in the program");
        return Val{};
    }
    const Block& b = bi->second;
    if (f.depth + 1 > kMaxCallDepth) {
        stop("calls nested deeper than " + std::to_string(kMaxCallDepth) + " (a recursion?)");
        return Val{};
    }
    Frame g;
    g.block = &b;
    g.depth = f.depth + 1;
    const bool fb = b.type == "FB";
    if (fb) {
        g.instance = instanceOf(f, target);
        if (!g.instance) {
            warn("a call of " + b.name + " whose instance was not found");
            return Val{};
        }
        g.local = frameLocals(b, {"Temp", "Constant"});
    } else {
        g.local = frameLocals(b, {"Input", "Output", "InOut", "Temp", "Constant", "Return"});
    }
    Var* params = fb ? g.instance : g.local.get();
    std::vector<ParamOut> outs;
    const Json* eno = nullptr;
    for (const auto& a : e["args"].items) {
        const std::string& op = a["op"].str();
        const std::string& name = a["name"].str();
        if (a["args"].items.empty()) continue;
        const Json& actual = a["args"].items[0];
        if (sameName(name, "ENO") && op == "out") {
            eno = &actual;
            continue;
        }
        if (sameName(name, "EN")) continue;  // see enabled()
        Var* m = params->member(name);
        if (!m)
            for (auto& x : params->members)
                if (sameName(x.first, name)) m = x.second.get();
        if (!m) {
            warn("a call of " + b.name + " with the parameter " + name + ", which it does not have");
            continue;
        }
        if ((op == "inout" || op == "out") && actual["op"].str() == "const") {
            warn("a call of " + b.name + " with the pointer " + actual["value"]["text"].str() + " for " + name +
                 ": pointers are not simulated");
            continue;
        }
        if (op == "inout" && !fb) {
            // an FC works on the actual parameter itself
            g.inOut[name] = placeOf(f, actual);
            continue;
        }
        if (op == "in" || op == "inout") {
            if (m->shape != Var::Leaf) {
                Place s = placeOf(f, actual);
                if (s.var) copyValues(*m, *s.var);
            } else {
                store(varPlace(m), eval(f, actual));
            }
        }
        if (op == "out" || op == "inout") outs.push_back({m, &actual, op == "inout"});
    }
    // the registers: an FB opens its instance as DI (a multi-instance: AR2
    // points into it); DB, DI and AR2 come back after the call
    const int saveDb = curDb_, saveDi = curDi_;
    const uint32_t saveAr2 = ar2_;
    if (fb) {
        int db = 0;
        const int64_t bit = bitOfVar(g.instance, db);
        if (bit >= 0) {
            curDi_ = db;
            ar2_ = (0x84u << 24) | static_cast<uint32_t>(bit & 0x7FFFF);
        } else if (target["instance"]["kind"].str() == "data_block") {
            for (const auto& d : dbInfo_)
                if (d.second.name == target["instance"]["name"].str()) curDi_ = d.first;
            ar2_ = 0x84u << 24;
        }
    }
    runBlock(g);
    curDb_ = saveDb;
    curDi_ = saveDi;
    ar2_ = saveAr2;
    for (const auto& o : outs) {
        if ((*o.to)["op"].str() == "wire") {
            storeWire(f, (*o.to)["name"].str(), o.from->value);
            continue;
        }
        Place p = placeOf(f, *o.to);
        if (!p.valid()) continue;
        if (p.var && p.var->shape != Var::Leaf) copyValues(*p.var, *o.from);
        else store(p, o.from->value);
    }
    if (eno) {
        if ((*eno)["op"].str() == "wire") storeWire(f, (*eno)["name"].str(), boolVal(g.eno));
        else store(placeOf(f, *eno), boolVal(g.eno));
    }
    lastEno_ = g.eno;
    if (!fb)
        for (auto& m : g.local->members)
            if (m.second->section == "Return") return m.second->value;
    return Val{};
}

// EN of a call: when it is FALSE the call is not made and ENO is FALSE
bool Simulator::enabled(Frame& f, const Json& e) {
    const Json* en = nullptr;
    const Json* eno = nullptr;
    for (const auto& a : e["args"].items) {
        if (a["args"].items.empty()) continue;
        if (sameName(a["name"].str(), "EN") && a["op"].str() == "in") en = &a["args"].items[0];
        if (sameName(a["name"].str(), "ENO") && a["op"].str() == "out") eno = &a["args"].items[0];
    }
    if (!en || truthy(eval(f, *en))) return true;
    lastEno_ = false;
    if (eno) {
        if ((*eno)["op"].str() == "wire") storeWire(f, (*eno)["name"].str(), boolVal(false));
        else store(placeOf(f, *eno), boolVal(false));
    }
    return false;
}

Val Simulator::callInstruction(Frame& f, const Json& e, const Json& target) {
    const std::string name = upper(target["name"].str());
    const bool timer = name == "TON" || name == "TOF" || name == "TP" || name == "TONR";
    const bool counter = name == "CTU" || name == "CTD" || name == "CTUD";
    const bool trig = name == "R_TRIG" || name == "F_TRIG";
    if (!timer && !counter && !trig) {
        warn("the instruction " + target["name"].str() + " is not simulated: its outputs keep their values");
        return Val{};
    }
    Var* inst = instanceOf(f, target);
    if (!inst) {
        warn("a call of " + target["name"].str() + " whose instance was not found");
        return Val{};
    }
    auto memberOf = [&](const std::string& param) -> Var* {
        // CTU and CTD keep their Q as QU and QD
        std::string m = upper(param);
        if (m == "Q" && name == "CTU") m = "QU";
        if (m == "Q" && name == "CTD") m = "QD";
        for (auto& x : inst->members)
            if (upper(x.first) == m) return x.second.get();
        return nullptr;
    };
    Var* cu = memberOf("CU");
    Var* cd = memberOf("CD");
    const bool oldCu = cu && truthy(cu->value), oldCd = cd && truthy(cd->value);
    std::vector<std::pair<Var*, const Json*>> outs;
    const Json* eno = nullptr;
    for (const auto& a : e["args"].items) {
        if (a["args"].items.empty()) continue;
        const std::string& pname = a["name"].str();
        const Json& actual = a["args"].items[0];
        if (sameName(pname, "ENO")) {
            eno = &actual;
            continue;
        }
        if (sameName(pname, "EN")) continue;  // see enabled()
        Var* m = memberOf(pname);
        if (!m) {
            warn("the instruction " + target["name"].str() + " has no parameter " + pname);
            continue;
        }
        if (a["op"].str() == "out") outs.emplace_back(m, &actual);
        else store(varPlace(m), eval(f, actual));
    }
    if (timer) {
        iecTimer(name, *inst);
    } else if (counter) {
        iecCounter(name, *inst, oldCu, oldCd);
    } else {
        // R_TRIG, F_TRIG: Q for one call on an edge of CLK
        Var* clk = memberOf("CLK");
        Var* q = memberOf("Q");
        if (clk && q) {
            TimerState& st = timers_[inst];
            const bool now = truthy(clk->value);
            store(varPlace(q), boolVal(name == "R_TRIG" ? now && !st.lastIn : !now && st.lastIn));
            st.lastIn = now;
        } else {
            warn("the instance of " + name + " has not the members CLK, Q");
        }
    }
    for (const auto& o : outs) {
        if ((*o.second)["op"].str() == "wire") storeWire(f, (*o.second)["name"].str(), o.first->value);
        else store(placeOf(f, *o.second), o.first->value);
    }
    if (eno) {
        if ((*eno)["op"].str() == "wire") storeWire(f, (*eno)["name"].str(), boolVal(true));
        else store(placeOf(f, *eno), boolVal(true));
    }
    lastEno_ = true;
    return Val{};
}

void Simulator::iecTimer(const std::string& name, Var& inst) {
    auto m = [&](const char* n) -> Var* { return inst.member(n); };
    Var *in = m("IN"), *pt = m("PT"), *q = m("Q"), *et = m("ET"), *r = m("R");
    if (!in || !pt || !q || !et) {
        warn("the instance of " + name + " has not the members IN, PT, Q, ET");
        return;
    }
    const bool fresh = !timers_.count(&inst);
    TimerState& st = timers_[&inst];
    const bool input = truthy(in->value);
    const int64_t preset = std::max<int64_t>(0, pt->value.i);
    auto set = [&](bool qv, int64_t etv) {
        store(varPlace(q), boolVal(qv));
        store(varPlace(et), timeVal(etv));
    };
    if (name == "TON") {
        if (input && !st.lastIn) {
            st.start = now_;
            st.running = true;
        }
        if (input) {
            const int64_t el = std::min(now_ - st.start, preset);
            set(el >= preset, el);
        } else {
            st.running = false;
            set(false, 0);
        }
    } else if (name == "TOF") {
        if (input) {
            st.running = false;
            set(true, 0);
        } else {
            if (st.lastIn) {
                st.start = now_;
                st.running = true;
            }
            if (st.running) {
                const int64_t el = std::min(now_ - st.start, preset);
                if (el >= preset) st.running = false;
                set(el < preset, el);
            } else {
                store(varPlace(q), boolVal(false));
            }
        }
    } else if (name == "TP") {
        if (input && !st.lastIn && !st.running) {
            st.start = now_;
            st.running = true;
        }
        if (st.running) {
            const int64_t el = now_ - st.start;
            if (el >= preset) {
                st.running = false;
                set(false, input ? preset : 0);
            } else {
                set(true, el);
            }
        } else {
            set(false, input && et->value.i >= preset ? preset : 0);
        }
    } else {  // TONR: the time adds up over the periods IN is on, until R
        if (fresh) st.elapsed = std::min(std::max<int64_t>(0, et->value.i), preset);  // retained
        if (r && truthy(r->value)) {
            // IN is taken as off: with IN on, the time starts when R goes off
            st = TimerState{};
            set(false, 0);
            return;
        } else {
            if (input && !st.lastIn) st.start = now_;
            if (!input && st.lastIn) st.elapsed += now_ - st.start;
            const int64_t total = std::min(st.elapsed + (input ? now_ - st.start : 0), preset);
            if (total >= preset) st.elapsed = preset;
            set(total >= preset, total);
        }
    }
    st.lastIn = input;
}

void Simulator::iecCounter(const std::string& name, Var& inst, bool oldCu, bool oldCd) {
    auto m = [&](const char* n) -> Var* { return inst.member(n); };
    Var *cu = m("CU"), *cd = m("CD"), *r = m("R"), *ld = m("LD"), *pv = m("PV"), *cv = m("CV"), *qu = m("QU"),
        *qd = m("QD");
    if (!cv || !pv) {
        warn("the instance of " + name + " has not the members PV, CV");
        return;
    }
    int64_t v = cv->value.i;
    const ElemType* t = cv->type;
    const int64_t maxv = t && t->bits < 64 ? (t->isSigned ? (int64_t(1) << (t->bits - 1)) - 1 : (int64_t(1) << t->bits) - 1)
                                         : INT64_MAX;
    const int64_t minv = t && t->isSigned && t->bits < 64 ? -(int64_t(1) << (t->bits - 1)) : (t && !t->isSigned ? 0 : INT64_MIN);
    const bool up = name != "CTD", down = name != "CTU";
    if (up && r && truthy(r->value)) {
        v = 0;
    } else if (down && ld && truthy(ld->value)) {
        v = pv->value.i;
    } else {
        if (up && cu && truthy(cu->value) && !oldCu && v < maxv) ++v;
        if (down && cd && truthy(cd->value) && !oldCd && v > minv) --v;
    }
    store(varPlace(cv), intVal(v));
    if (qu) store(varPlace(qu), boolVal(v >= pv->value.i));
    if (qd) store(varPlace(qd), boolVal(v <= 0));
}

Val Simulator::builtin(Frame& f, const Json& e) {
    const std::string name = upper(e["name"].str());
    const ElemType* rt = elemType(e["type"].str());
    std::vector<Val> args;
    std::vector<std::string> names;
    for (const auto& a : e["args"].items) {
        if (a["op"].str() != "in" || a["args"].items.empty() || sameName(a["name"].str(), "EN")) continue;
        args.push_back(eval(f, a["args"].items[0]));
        names.push_back(upper(a["name"].str()));
    }
    auto arg = [&](const char* n, size_t pos) -> Val {
        for (size_t k = 0; k < names.size(); ++k)
            if (names[k] == n) return args[k];
        return pos < args.size() ? args[pos] : Val{};
    };
    auto result = [&](Val v) { return rt ? convertTo(v, *rt) : v; };
    // the conversions: INT_TO_WORD, REAL_TO_INT, ...
    const size_t to = name.find("_TO_");
    if (to != std::string::npos) {
        const ElemType* dst = elemType(name.substr(to + 4));
        if (!dst) dst = rt;
        if (!dst || args.empty()) {
            warn("the conversion " + name + " is not simulated");
            return Val{};
        }
        return convertTo(args[0], *dst);
    }
    if (args.empty() && name != "RUNTIME") {
        warn("the function " + name + " without parameters is not simulated");
        return Val{};
    }
    const Val x = args.empty() ? Val{} : args[0];
    if (name == "ABS") return result(x.kind == Kind::Real ? realVal(std::fabs(x.r), x.type) : intVal(x.i < 0 ? static_cast<int64_t>(0 - static_cast<uint64_t>(x.i)) : x.i, x.type));
    if (name == "MIN" || name == "MAX") {
        Val best = args[0];
        for (const Val& v : args) {
            const bool less = (v.kind == Kind::Real || best.kind == Kind::Real) ? asReal(v) < asReal(best) : asInt(v) < asInt(best);
            if (name == "MIN" ? less : (!less && !sameValue(v, best))) best = v;
        }
        return result(best);
    }
    if (name == "LIMIT") {
        const Val mn = arg("MN", 0), in = arg("IN", 1), mx = arg("MX", 2);
        const bool real = in.kind == Kind::Real || mn.kind == Kind::Real || mx.kind == Kind::Real;
        if (real) return result(realVal(std::min(std::max(asReal(in), asReal(mn)), asReal(mx)), in.type));
        return result(intVal(std::min(std::max(asInt(in), asInt(mn)), asInt(mx)), in.type));
    }
    if (name == "SEL") return result(truthy(arg("G", 0)) ? arg("IN1", 2) : arg("IN0", 1));
    if (name == "MOVE") return result(x);
    if (name == "SHL" || name == "SHR" || name == "ROL" || name == "ROR" || name == "SWAP") {
        const Val in = arg("IN", 0);
        const ElemType* t = in.type && in.type->kind == Kind::Int ? in.type : (rt && rt->kind == Kind::Int ? rt : nullptr);
        const int bits = t ? t->bits : 32;
        const uint64_t mask = bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
        const uint64_t u = static_cast<uint64_t>(asInt(in)) & mask;
        uint64_t o = 0;
        if (name == "SWAP") {
            for (int k = 0; k < bits / 8; ++k) o = (o << 8) | ((u >> (8 * k)) & 0xFF);
        } else {
            const int64_t n = std::max<int64_t>(0, asInt(arg("N", 1)));
            if (name == "SHL") o = n >= bits ? 0 : (u << n) & mask;
            else if (name == "SHR") {
                // a signed number keeps its sign
                if (t && t->isSigned && (u >> (bits - 1)) & 1) o = n >= bits ? mask : ((u >> n) | (mask & ~(mask >> n))) & mask;
                else o = n >= bits ? 0 : u >> n;
            } else {
                const int k = static_cast<int>(n % bits);
                o = k == 0 ? u : (name == "ROL" ? ((u << k) | (u >> (bits - k))) : ((u >> k) | (u << (bits - k)))) & mask;
            }
        }
        Val v = intVal(static_cast<int64_t>(o), t);
        return t ? (rt ? convertTo(convertTo(v, *t), *rt) : convertTo(v, *t)) : v;
    }
    if (name == "MUX") {
        const int64_t k = asInt(arg("K", 0));
        for (size_t j = 0; j < names.size(); ++j)
            if (names[j] == "IN" + std::to_string(k)) return result(args[j]);
        for (size_t j = 0; j < names.size(); ++j)
            if (names[j] == "ELSE") return result(args[j]);
        return Val{};
    }
    const double d = asReal(x);
    if (name == "SQR") return result(realVal(d * d, x.type));
    if (name == "SQRT") return result(realVal(std::sqrt(d), x.type));
    if (name == "EXP") return result(realVal(std::exp(d), x.type));
    if (name == "LN") return result(realVal(std::log(d), x.type));
    if (name == "SIN") return result(realVal(std::sin(d), x.type));
    if (name == "COS") return result(realVal(std::cos(d), x.type));
    if (name == "TAN") return result(realVal(std::tan(d), x.type));
    if (name == "ASIN") return result(realVal(std::asin(d), x.type));
    if (name == "ACOS") return result(realVal(std::acos(d), x.type));
    if (name == "ATAN") return result(realVal(std::atan(d), x.type));
    if (name == "FRAC") return result(realVal(d - std::trunc(d), x.type));
    if (name == "TRUNC") return result(intVal(clampToInt64(std::trunc(d)), elemType("DInt")));
    if (name == "ROUND") return result(intVal(clampToInt64(std::nearbyint(d)), elemType("DInt")));
    if (name == "CEIL") return result(intVal(clampToInt64(std::ceil(d)), elemType("DInt")));
    if (name == "FLOOR") return result(intVal(clampToInt64(std::floor(d)), elemType("DInt")));
    if (name == "NORM_X") {
        const double mn = asReal(arg("MIN", 0)), v = asReal(arg("VALUE", 1)), mx = asReal(arg("MAX", 2));
        return result(realVal(mx == mn ? 0 : (v - mn) / (mx - mn), elemType("Real")));
    }
    if (name == "SCALE_X") {
        const double mn = asReal(arg("MIN", 0)), v = asReal(arg("VALUE", 1)), mx = asReal(arg("MAX", 2));
        return result(realVal(v * (mx - mn) + mn, elemType("Real")));
    }
    if (name == "LEN") return result(intVal(static_cast<int64_t>(x.s.size()), elemType("Int")));
    if (name == "CONCAT") return result(stringVal(arg("IN1", 0).s + arg("IN2", 1).s));
    if (name == "LEFT" || name == "RIGHT") {
        const std::string s = arg("IN", 0).s;
        const size_t l = static_cast<size_t>(std::max<int64_t>(0, asInt(arg("L", 1))));
        if (l >= s.size()) return result(stringVal(s));
        return result(stringVal(name == "LEFT" ? s.substr(0, l) : s.substr(s.size() - l)));
    }
    if (name == "MID") {
        const std::string s = arg("IN", 0).s;
        const int64_t l = asInt(arg("L", 1)), p = asInt(arg("P", 2));
        if (p < 1 || l < 0 || static_cast<size_t>(p) > s.size()) return result(stringVal(""));
        return result(stringVal(s.substr(static_cast<size_t>(p - 1), static_cast<size_t>(l))));
    }
    warn("the function " + e["name"].str() + " is not simulated (the result is 0)");
    return Val{};
}

// ---- operands by name ----

Simulator::Place Simulator::operand(const std::string& text, std::string& error) {
    size_t p = 0;
    auto skip = [&] {
        while (p < text.size() && text[p] == ' ') ++p;
    };
    auto name = [&]() -> std::string {
        skip();
        std::string n;
        if (p < text.size() && text[p] == '"') {
            const size_t q = text.find('"', p + 1);
            if (q == std::string::npos) return std::string();
            n = text.substr(p + 1, q - p - 1);
            p = q + 1;
            return n;
        }
        while (p < text.size() && text[p] != '.' && text[p] != '[' && text[p] != ' ') n += text[p++];
        return n;
    };
    skip();
    if (p < text.size() && text[p] == '%') {
        const Address at = parseAddress(text.substr(p));
        if (!at.area) {
            error = "not an address: " + text;
            return Place{};
        }
        const char* t = at.size == 'X' ? "Bool" : at.size == 'B' ? "Byte" : at.size == 'W' ? "Word" : at.size == 'D' ? "DWord" : "LWord";
        return area(at, elemType(t));
    }
    const std::string first = name();
    Place pl;
    skip();
    auto tag = tags_.find(first);
    if (tag != tags_.end() && p >= text.size()) {
        if (tag->second.var) pl.var = tag->second.var.get();
        else pl = area(tag->second.at, tag->second.type);
        return pl;
    }
    auto db = dbs_.find(first);
    if (db == dbs_.end()) {
        error = "no tag or data block " + first;
        return Place{};
    }
    pl.var = db->second.get();
    while (p < text.size()) {
        skip();
        if (p >= text.size()) break;
        if (text[p] == '.') {
            ++p;
            const std::string m = name();
            if (m.size() >= 2 && m[0] == '%') {
                int bits = 0, index = 0;
                if (!parseSlice(m, bits, index)) {
                    error = "not a part of a variable: " + m;
                    return Place{};
                }
                pl.sliceBits = bits;
                pl.sliceIndex = index;
                continue;
            }
            Var* v = pl.var ? pl.var->member(m) : nullptr;
            if (!v) {
                error = "no member " + m + " in " + text;
                return Place{};
            }
            pl.var = v;
        } else if (text[p] == '[') {
            const size_t q = text.find(']', p);
            if (q == std::string::npos) {
                error = "no ] in " + text;
                return Place{};
            }
            std::vector<int64_t> idx;
            std::string inner = text.substr(p + 1, q - p - 1);
            size_t from = 0;
            while (from <= inner.size()) {
                size_t comma = inner.find(',', from);
                if (comma == std::string::npos) comma = inner.size();
                std::string one = inner.substr(from, comma - from);
                while (!one.empty() && one.front() == ' ') one.erase(0, 1);
                while (!one.empty() && one.back() == ' ') one.pop_back();
                char* end = nullptr;
                errno = 0;
                const long long n = std::strtoll(one.c_str(), &end, 10);
                if (one.empty() || *end || errno == ERANGE) {
                    error = "not an index: [" + inner + "] in " + text;
                    return Place{};
                }
                idx.push_back(n);
                from = comma + 1;
            }
            Var* el = pl.var ? pl.var->element(idx) : nullptr;
            if (!el) {
                error = "no element " + text.substr(p, q - p + 1) + " in " + text;
                return Place{};
            }
            pl.var = el;
            p = q + 1;
        } else {
            error = "cannot read " + text;
            return Place{};
        }
    }
    return pl;
}

// %C5: an S5 counter's value; %T5: an S5 timer's status (read only)
static bool s5Address(const std::string& name, char& kind, int& n) {
    if (name.size() < 3 || name[0] != '%' || (name[1] != 'C' && name[1] != 'T' && name[1] != 'Z')) return false;
    for (size_t k = 2; k < name.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(name[k]))) return false;
    if (name.size() > 6) return false;
    kind = name[1] == 'T' ? 'T' : 'C';
    n = std::atoi(name.c_str() + 2);
    return true;
}

bool Simulator::read(const std::string& name, Val& out, std::string& error) {
    char kind = 0;
    int n = 0;
    if (s5Address(name, kind, n)) {
        if (n >= 2048) {
            error = name + " does not exist (0 to 2047)";
            return false;
        }
        if (kind == 'C') {
            out = intVal(c5_[static_cast<size_t>(n)].value);
        } else {
            S5Timer& t = t5_[static_cast<size_t>(n)];
            s5Update(t);
            out = boolVal(t.q);
        }
        return true;
    }
    Place p = operand(name, error);
    if (!p.valid()) return false;
    if (p.var && p.var->shape != Var::Leaf && !p.sliceBits) {
        error = name + " is a structure or array";
        return false;
    }
    out = load(p);
    return true;
}

bool Simulator::write(const std::string& name, const Val& value, std::string& error) {
    char kind = 0;
    int n = 0;
    if (s5Address(name, kind, n)) {
        const Val v = convertTo(value, *elemType("Int"));
        if (kind == 'T' || n >= 2048 || v.i < 0 || v.i > 999) {
            error = kind == 'T' ? name + ": an S5 timer cannot be written" : name + ": a counter 0 to 2047, a value 0 to 999";
            return false;
        }
        c5_[static_cast<size_t>(n)].value = static_cast<int>(v.i);
        return true;
    }
    Place p = operand(name, error);
    if (!p.valid()) return false;
    if (p.var && p.var->shape != Var::Leaf && !p.sliceBits) {
        error = name + " is a structure or array";
        return false;
    }
    // an input with a hardware interrupt: its edge raises the interrupt
    std::vector<bool> before;
    for (const auto& h : hwInputs_) before.push_back(truthy(readArea(i_, h.at, *elemType("Bool"))));
    store(p, value);
    for (size_t k = 0; k < hwInputs_.size(); ++k) {
        const bool now = truthy(readArea(i_, hwInputs_[k].at, *elemType("Bool")));
        if (now != before[k] && now == hwInputs_[k].rising) {
            std::string err;
            raise(std::to_string(hwInputs_[k].ob), err);
        }
    }
    return true;
}

}  // namespace s7forge
