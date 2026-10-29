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
- Interrupts: time-delay (SRT_DINT, CAN_DINT, QRY_DINT), time-of-day
  (SET_TINTL, ACT_TINT, CAN_TINT, QRY_TINT, with the periods once to
  yearly and the end of the month), hardware interrupts raised by a
  scenario (`raise`) or `--raise`, the time error OB at the maximum cycle
  time and STOP at twice it; priorities, events that may wait and lost
  events (`Event_Count`), `Sign` of a time delay.
- `s7forge serve`: the program in real time behind a Modbus/TCP server;
  function codes 1–6, 15, 16, 23; a default map onto %Q, %I, %MW, %IW or a
  map file onto tags and data block members; `--read-only`, `--speed`,
  `--listen`; docs/MODBUS.md.
- The interrupts as s17_interrupts stores them: the event names, a time of
  day set in an OB's properties, the hardware interrupt of an input edge
  (writing the input raises it; `raise` also takes the event's name);
  scenario s17_interrupts.
- An instruction time (`instruction_us`, `--instruction-us`): the program
  takes time, and an interrupt of a higher priority runs between two
  statements of the OB it interrupts.
- The CPU's clock (`clock` in a scenario), RD_SYS_T, RD_LOC_T, WR_SYS_T.
- The system constants of the export (OB numbers, hardware identifiers)
  are known to the code that names them.
- The CPU's settings as s18_settings stores them (tiaconv's `cpu_settings`):
  the retentive bytes of M and the retentive S5 timers and counters, the
  maximum cycle time (the time error) and the minimum cycle time (the
  default scan time), the startup after power on. A data block with
  standard access set retentive keeps all of it. Scenario s18_settings.
- `restart: "power"` is now a power cycle (retentive values kept, RUN or
  STOP by the startup setting); `restart: "reset"` is the memory reset that
  `"power"` was. `stop: true` puts the CPU in STOP. `%C5` and `%T5` read S5
  counters and timers.
- "Enable time error" of an interrupt OB: a time error when its waiting
  events reach the threshold; lost events say whether they are reported in
  the diagnostic buffer.
- Block moves and strings: MOVE_BLK, UMOVE_BLK, MOVE_BLK_VARIANT,
  FILL_BLK, UFILL_BLK, FIND, REPLACE, INSERT, DELETE, STRG_VAL, VAL_STRG,
  Chars_TO_Strg, Strg_TO_Chars, with ENO FALSE where they fail. Scenarios
  s19_library and library.
- S5 counters: S clears the edge memories of CU and CD, so a counter set
  while CU is on counts in the next cycle, as Siemens documents it. The S5
  rules of the manuals are each a step of the scenario stl_s5rules.
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
