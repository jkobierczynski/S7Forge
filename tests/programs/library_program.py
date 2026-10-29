# Generates tests/programs/library.program.json (python3 tests/programs/library_program.py tests/programs/library.program.json)
import json, sys
def el(name, bits=None, length=None):
    return {"kind": "string" if name == "String" else "elementary", "name": name, "bits": bits, "length": length, "dims": [], "element": None}
T = {"Int": el("Int", 16), "DInt": el("DInt", 32), "UInt": el("UInt", 16), "Real": el("Real", 32), "Bool": el("Bool", 1), "Char": el("Char", 8)}
def arr(lo, hi, e):
    return {"kind": "array", "name": "Array", "bits": None, "length": None,
            "dims": [{"low": lo, "low_text": str(lo), "high": hi, "high_text": str(hi)}], "element": T[e]}
def mem(name, dt, t, start=None):
    return {"name": name, "data_type": dt, "type": t, "section": None, "start": start, "retain": False, "members": []}
def sv(s): return {"kind": "string", "text": "'%s'" % s, "string": s}
def iv(i): return {"kind": "int", "text": str(i), "int": i}
members = [
    mem("a", "Array[0..7] of Int", arr(0, 7, "Int")),
    mem("b", "Array[0..7] of Int", arr(0, 7, "Int")),
    mem("c", "Array[1..3] of Int", arr(1, 3, "Int")),
    mem("r", "Real", T["Real"]), mem("p", "UInt", T["UInt"], iv(1)), mem("q", "UInt", T["UInt"], iv(1)),
    mem("ret", "Int", T["Int"]), mem("cnt", "Int", T["Int"]), mem("pos", "Int", T["Int"], iv(-1)),
    mem("ch", "Array[0..5] of Char", arr(0, 5, "Char")),
] + [mem(n, "String[20]", el("String", None, 20)) for n in ("s", "t", "u", "v", "w", "x")] \
  + [mem(n, "Bool", T["Bool"]) for n in ("e1", "e2", "e3", "e4")]
refs, ids = [], {}
def ref(m):
    if m not in ids:
        ids[m] = len(ids) + 1
        refs.append({"ref_id": ids[m], "kind": "data block member", "text": '"L".' + m, "name": None,
                     "path": [{"name": "L", "index": []}, {"name": m, "index": []}], "modifier": None})
    return {"op": "ref", "ref_id": ids[m], "args": []}
def idx(m, i): return {"op": "index", "args": [ref(m), c(i, "DInt")]}
def c(v, t="Int"):
    if t == "String": val = sv(v)
    elif t == "Real": val = {"kind": "real", "text": str(v), "real": v}
    else: val = iv(v)
    return {"op": "const", "type": t, "value": val, "args": []}
def p(d, n, a): return {"op": d, "name": n, "args": [a]}
def call(name, *params, t=None):
    return {"op": "call", "name": name, "type": t, "args": list(params),
            "target": {"kind": "instruction", "name": name, "version": None, "instance": None, "parameters": []}}
def st_call(x): return {"op": "call", "exprs": [x]}
def st_assign(m, x): return {"op": "assign", "exprs": [ref(m), x]}
code = [
    st_call(call("MOVE_BLK", p("in", "IN", idx("a", 0)), p("in", "COUNT", c(4, "UInt")), p("out", "OUT", idx("a", 2)))),
    st_call(call("MOVE_BLK", p("in", "IN", idx("a", 0)), p("in", "COUNT", c(9, "UInt")), p("out", "OUT", idx("b", 0)), p("out", "ENO", ref("e1")))),
    st_call(call("FILL_BLK", p("in", "IN", c(5)), p("in", "COUNT", c(3, "UInt")), p("out", "OUT", idx("b", 5)))),
    st_call(call("FILL_BLK", p("in", "IN", c(1)), p("in", "COUNT", c(4, "UInt")), p("out", "OUT", idx("b", 5)), p("out", "ENO", ref("e2")))),
    st_assign("ret", call("MOVE_BLK_VARIANT", p("in", "SRC", ref("a")), p("in", "COUNT", c(2, "UDInt")), p("in", "SRC_INDEX", c(6, "DInt")),
                          p("in", "DEST_INDEX", c(0, "DInt")), p("out", "DEST", ref("c")), t="Int")),
    st_assign("s", call("REPLACE", p("in", "IN1", c("abc", "String")), p("in", "IN2", c("Z", "String")), p("in", "L", c(5)), p("in", "P", c(4)), t="String")),
    st_assign("t", call("DELETE", p("in", "IN", c("abcdef", "String")), p("in", "L", c(10)), p("in", "P", c(3)), t="String")),
    st_call(call("STRG_VAL", p("in", "IN", c(" 1,5E2x", "String")), p("in", "FORMAT", c(3, "Word")), p("in", "P", ref("p")), p("out", "OUT", ref("r")))),
    st_call(call("VAL_STRG", p("in", "IN", c(-3.14159, "Real")), p("in", "SIZE", c(8, "USInt")), p("in", "PREC", c(2, "USInt")),
                 p("in", "FORMAT", c(6, "Word")), p("in", "P", ref("q")), p("out", "OUT", ref("u")))),
    st_call(call("VAL_STRG", p("in", "IN", c(123456, "DInt")), p("in", "SIZE", c(4, "USInt")), p("in", "PREC", c(0, "USInt")),
                 p("in", "FORMAT", c(0, "Word")), p("in", "P", c(1, "UInt")), p("out", "OUT", ref("v")), p("out", "ENO", ref("e3")))),
    st_call(call("VAL_STRG", p("in", "IN", c(5)), p("in", "SIZE", c(6, "USInt")), p("in", "PREC", c(2, "USInt")),
                 p("in", "FORMAT", c(4, "Word")), p("in", "P", c(1, "UInt")), p("out", "OUT", ref("w")))),
    st_call(call("Strg_TO_Chars", p("in", "Strg", c("hi!", "String")), p("in", "pChars", c(2, "DInt")), p("out", "Cnt", ref("cnt")),
                 p("inout", "Chars", ref("ch")))),
    st_call(call("Chars_TO_Strg", p("in", "Chars", ref("ch")), p("in", "pChars", c(2, "DInt")), p("in", "Cnt", c(0, "UInt")), p("out", "Strg", ref("x")))),
    st_assign("pos", call("FIND", p("in", "IN1", c("abc", "String")), p("in", "IN2", c("", "String")), t="Int")),
]
prog = {
    "format": "tiaconv-program", "schema_version": 2, "plc": {"name": "LIB"}, "tags": [],
    "data_blocks": [{"name": "L", "number": 1, "kind": "global", "members": members}],
    "constants": [],
    "organization_blocks": [{"name": "Main", "number": 1, "event": "ProgramCycle", "priority": 1, "cycle_us": None}],
    "runtime": {"startup": [], "program_cycle": ["Main"], "interrupts": []},
    "blocks": [{"type": "OB", "name": "Main", "number": 1, "language": "SCL", "protection": None, "interface": [],
                "code": {"networks": [{"number": 1, "statements": code}], "references": refs}}],
}
json.dump(prog, open(sys.argv[1], "w"), indent=1)
