#!/usr/bin/env python3
# S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
# Copyright (C) 2026 Jurgen Kobierczynski
# SPDX-License-Identifier: GPL-3.0-or-later
#
# stl2json.py SRC OUT: a small STL program in a compact text form -> a
# program file as tiaconv --program-json writes it, for tests.
#
#   tag NAME TYPE %ADDR        a PLC tag (%T5 and %C5 for the types Timer, Counter)
#   db NAME NUMBER [optimized] a data block, standard access unless optimized,
#   field NAME TYPE              and its members: Bool, Byte, Int, Word, DInt,
#                                DWord, Real, Time, S5Time, Char, String[n],
#                                Array[a..b] of an elementary type; temps
#                                also Any, Pointer
#   block NAME FC|FB           the next block (the first one is the OB Main)
#   temp NAME TYPE             a temporary of the block (types as for field)
#   optimized                  the block has optimized access (standard otherwise)
#   ---                        a new network
#   NAME:                      a label
#   INSTR [OPERAND]            OPERAND:
#       a tag name             the tag
#       "DB".member            a member of a data block
#       #name                  a temporary of the block
#       db:NAME                a data block (OPN db:Data)
#       i:VALUE[:TYPE]         an integer constant (Int unless given)
#       r:REAL, t:MS, s5:MS    a Real, a Time, an S5Time
#       k:WORD                 a keyword (BR, OV, >0, ...)
#       @LABEL                 a jump's label
#       call:BLOCK             the block of a CALL
#       (                      the bracket of A(, O(, ...
#       anything else          only as the text, as tiaconv gives an
#                              absolute operand: MW 10, DBX 0.0, T 5,
#                              DBW [AR1,P#2.0], P#M10.0
import json
import re
import sys

src = open(sys.argv[1]).read().splitlines()
tags = []
dbs = []  # [name, number, optimized, [(name, type)]]
blocks = []  # [name, type, networks]
cur = ["Main", "OB", [[]], [], False]  # name, type, networks, temps, optimized
blocks.append(cur)
refs = []


def ref(r):
    r["ref_id"] = 1001 + len(refs)  # the tags are 1, 2, ...
    refs.append(r)
    return r["ref_id"]


def elementary(dt):
    bits = {"Bool": 1, "Byte": 8, "Char": 8, "SInt": 8, "USInt": 8, "Int": 16, "Word": 16, "UInt": 16, "S5Time": 16,
            "Date": 16, "DInt": 32, "DWord": 32, "UDInt": 32, "Real": 32, "Time": 32, "Time_Of_Day": 32,
            "LReal": 64, "LInt": 64}.get(dt)
    return {"kind": "elementary", "name": dt, "bits": bits, "length": None, "dims": [], "element": None}


def typeOf(dt):
    m = re.fullmatch(r"String\[(\d+)\]", dt)
    if m:
        return {"kind": "string", "name": "String", "bits": None, "length": int(m.group(1)), "dims": [], "element": None}
    m = re.fullmatch(r"Array\[(-?\d+)\.\.(-?\d+)\] of (\w+)", dt)
    if m:
        lo, hi = int(m.group(1)), int(m.group(2))
        return {"kind": "array", "name": "Array", "bits": None, "length": None,
                "dims": [{"low": lo, "low_text": str(lo), "high": hi, "high_text": str(hi)}],
                "element": elementary(m.group(3))}
    if dt in ("Any", "Pointer"):
        return {"kind": "parameter", "name": dt, "bits": None, "length": None, "dims": [], "element": None}
    return elementary(dt)


def member(name, dt):
    return {"name": name, "data_type": dt, "type": typeOf(dt), "section": None, "offset": None, "offset_bits": None,
            "start": None, "retain": False, "members": []}


for ln in src:
    ln = ln.split("//")[0].strip()
    if not ln:
        continue
    if ln.startswith("tag "):
        _, n, t, a = ln.split()
        tags.append((n, t, a))
        continue
    if ln.startswith("db "):
        f = ln.split()
        dbs.append([f[1], int(f[2]), len(f) > 3 and f[3] == "optimized", []])
        continue
    if ln.startswith("field "):
        _, n, t = ln.split(None, 2)
        dbs[-1][3].append((n, t))
        continue
    if ln.startswith("block "):
        _, n, t = ln.split()
        cur = [n, t, [[]], [], False]
        blocks.append(cur)
        continue
    if ln.startswith("temp "):
        _, n, t = ln.split(None, 2)
        cur[3].append((n, t))
        continue
    if ln == "optimized":
        cur[4] = True
        continue
    if ln == "---":
        cur[2].append([])
        continue
    if ln.endswith(":") and " " not in ln:
        cur[2][-1].append({"op": "label", "name": ln[:-1], "exprs": []})
        continue
    parts = ln.split(None, 1)
    name = parts[0]
    exprs = []
    target = None
    text = ""
    if len(parts) > 1:
        o = parts[1].strip()
        text = o
        names = [t[0] for t in tags]
        if o.startswith("i:"):
            f = o.split(":")
            text = f[1]
            exprs.append({"op": "const", "name": f[1], "type": f[2] if len(f) > 2 else "Int",
                          "value": {"kind": "int", "int": int(f[1], 0)}, "args": []})
        elif o.startswith("r:"):
            text = o[2:]
            exprs.append({"op": "const", "name": o[2:], "type": "Real", "value": {"kind": "real", "real": float(o[2:])}, "args": []})
        elif o.startswith("t:"):
            text = "T#" + o[2:] + "MS"
            exprs.append({"op": "const", "name": text, "type": "Time",
                          "value": {"kind": "duration", "ns": int(o[2:]) * 1000000}, "args": []})
        elif o.startswith("s5:"):
            text = "S5T#" + o[3:] + "MS"
            exprs.append({"op": "const", "name": text, "type": "S5Time",
                          "value": {"kind": "duration", "ns": int(o[3:]) * 1000000}, "args": []})
        elif o.startswith("k:"):
            text = o[2:]
            exprs.append({"op": "const", "name": o[2:], "args": []})
        elif o == "(":
            exprs.append({"op": "const", "name": "(", "args": []})
        elif o.startswith("@"):
            text = o[1:]
            exprs.append({"op": "ref", "name": o[1:], "args": []})
        elif o.startswith("call:"):
            b = o[5:]
            text = '"' + b + '"'
            exprs.append({"op": "ref", "name": text, "args": []})
            target = {"kind": "block", "name": b, "instance": None}
        elif o.startswith("db:"):
            text = '"' + o[3:] + '"'
            i = ref({"kind": "data block", "text": text, "name": o[3:], "path": [], "modifier": None})
            exprs.append({"op": "ref", "ref_id": i, "name": text, "args": []})
        elif re.fullmatch(r'"\w+"\.\w+', o):
            d, m = o.split(".")
            i = ref({"kind": "data block member", "text": o, "name": None,
                     "path": [{"name": d.strip('"'), "index": []}, {"name": m, "index": []}], "modifier": None})
            exprs.append({"op": "ref", "ref_id": i, "name": o, "args": []})
        elif o.startswith("#"):
            i = ref({"kind": "local", "text": o, "name": None, "path": [{"name": o[1:], "index": []}], "modifier": None})
            exprs.append({"op": "ref", "ref_id": i, "name": o, "args": []})
        elif o in names:
            text = '"' + o + '"'
            exprs.append({"op": "ref", "ref_id": names.index(o) + 1, "name": text, "args": []})
        # else: an absolute operand, as text only
    st = {"op": "stl", "name": name, "exprs": exprs, "text": text}
    if target:
        st["target"] = target
    cur[2][-1].append(st)

# the tags first, so that their ref_ids are 1, 2, ...
tagRefs = [{"ref_id": k + 1, "kind": "tag", "text": '"' + t[0] + '"', "name": None, "path": [], "modifier": None}
           for k, t in enumerate(tags)]
prog = {
    "format": "tiaconv-program", "schema_version": 2, "plc": {"name": "T"},
    "tags": [{"name": n, "data_type": t, "type": elementary(t), "address": a} for n, t, a in tags],
    "constants": [],
    "data_blocks": [{"name": n, "number": num, "kind": "global", "optimized": opt, "members": [member(m, t) for m, t in ms]}
                    for n, num, opt, ms in dbs],
    "organization_blocks": [{"name": "Main", "event": "ProgramCycle", "priority": 1, "cycle_us": None}],
    "runtime": {"startup": [], "program_cycle": ["Main"], "interrupts": []},
    "blocks": [{"type": t, "name": n, "language": "STL", "optimized": opt, "protection": None,
                "interface": [dict(member(m, ty), section="Temp") for m, ty in temps],
                "code": {"networks": [{"number": k + 1, "statements": s} for k, s in enumerate(nets)],
                         "references": tagRefs + refs}}
               for n, t, nets, temps, opt in blocks],
}
json.dump(prog, open(sys.argv[2], "w"), indent=1)
