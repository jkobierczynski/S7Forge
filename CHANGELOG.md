# Changelog

## 0.1.0 (unreleased)

The first version: the core of the simulator, and STL.

- STL as an S7-1500 runs it: bit logic with the first check, AND before OR
  and brackets; four accumulators; Int, DInt and Real arithmetic,
  comparisons, conversions, word logic, shifts; the jumps, LOOP, BE, BEC,
  CALL; BR as the ENO of an STL block; German mnemonics.
- STL: S5 timers and counters; absolute and indirect addresses (OPN, DBX,
  DB5.DBW2, memory- and register-indirect, area-crossing, pointers, the
  address registers); data blocks with standard access laid out in bytes;
  MCR. Needs the `text` of STL statements from tiaconv.
- STL: local data by address (L, LB, LW, LD, pointers to L).
- STL: memory-indirect addresses by a tag (`DBW ["Tag_7" ]`), operands only
  in the text (status bits, `C#5` as BCD), as the s16_stl project gives them.
- LAD/FBD: the S5 box names the project stores (`Sp`, `Sd`, `Cud`, ...)
  and the pins `rt`, `rtbcd`.
- Scenario s16_stl on the program of tiaconv's s16_stl fixture.
- LAD/FBD: the S5 timer and counter boxes (S_PULSE ... S_OFFDT, S_CU,
  S_CD, S_CUD, also their German names), the coils SP ... SF, CU, CD, SC,
  contacts and the R coil on timers and counters.
- DEVELOPMENT.md: the decisions taken in implementing S7Forge.
- Loads a program file of tiaconv `--program-json` (schema version 2).
- Runs the startup OBs, the program cycle OBs and the cyclic interrupt OBs in
  virtual time; a warm restart keeps the retentive data, a power-on does not.
- LAD, FBD and SCL from the code tree: expressions, IF, CASE, FOR, WHILE,
  REPEAT, EXIT, CONTINUE, RETURN, edges, ENO.
- FB calls with instance data blocks and multi-instances; FC calls with
  in-outs by reference and the return value.
- IEC timers TON, TOF, TP, TONR (retentive), counters CTU, CTD, CTUD, R_TRIG
  and F_TRIG; the conversions and the common functions of the language,
  shifts and rotations; EN and ENO.
- The areas I, Q and M, the PLC tags at their addresses, and the user
  constants of the tag tables.
- A cycle that does not end and an array index out of bounds put the CPU in
  STOP.
- `s7forge run`, `s7forge test` (scenario files) and `s7forge info`.
- Tests: unit tests, scenarios on the programs of tiaconv's test projects
  s15_motor and s14_code_rewritten, and small programs from a review of the
  semantics (overflows, TONR, Time conversions, paths into arrays of
  structures, a timer passed to an FC as in-out).
