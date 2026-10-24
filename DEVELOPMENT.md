# Development

What S7Forge is built on, the decisions taken in implementing it, and what
is still open. The user-facing description is in [README.md](README.md),
the scenario format in [docs/SCENARIOS.md](docs/SCENARIOS.md), the input
format in tiaconv's `docs/PROGRAM-JSON.md`, the releases in
[CHANGELOG.md](CHANGELOG.md).

State at the time of writing: version 0.1.0, not released.

## Ground rules

- **The input is tiaconv's program export, nothing else.** S7Forge reads the
  file `tiaconv --program-json` writes (schema version 2) and never a TIA
  Portal project. Everything about the project (types, start values, code,
  calls, the OBs and their events) comes structured from tiaconv, so S7Forge
  parses no TIA Portal notation, except where the export has no structure:
  the operand text of STL statements and S7 literals in scenarios.
- **Protected blocks stay protected.** A know-how protected block has no code
  and no interface in the export; calling it does nothing and S7Forge says
  so. It never tries to get at the code another way.
- **What is not simulated is said, once.** Every instruction, operand or
  construct S7Forge does not run produces one warning (`warnings()`, printed
  by `run` and `test -v`), and the simulation goes on. A fault a real CPU
  would go to STOP for puts the simulated CPU in STOP with the reason.
- **The same run gives the same result.** Time is virtual; nothing depends
  on the wall clock, on threads or on the order of a hash map.
- **Test data is made by the author.** `tests/data` holds exports of
  tiaconv's own test projects (s13_hmi, s14_code_rewritten, s15_motor);
  the other test programs are written here. Projects of others are not
  added to the repository.
- **Licence:** GPL-3.0-or-later, as tiaconv.

## Repository

| path | what |
|---|---|
| `src/json.*` | a small, strict JSON reader |
| `src/value.*` | the elementary types of S7 and values: conversions, wrapping, S5TIME |
| `src/memory.*` | variables as trees, the areas I, Q, M as bytes |
| `src/sim.*` | the CPU: loading, OB scheduling, the code tree (expressions, statements, calls), IEC timers and counters, built-in functions, operands by name |
| `src/stl.cpp` | the STL machine and MCR |
| `src/s5.cpp` | S5 timers and counters, for STL and for the boxes and coils of LAD/FBD |
| `src/absolute.cpp` | absolute and indirect operands, data blocks and local data as bytes |
| `src/scenario.*` | scenario files and S7 literals |
| `src/main.cpp` | the command line: `run`, `test`, `info` |
| `tests/tests.cpp` | unit tests on small programs written in the test |
| `tests/scenarios/` | scenarios run by ctest; `tests/data`, `tests/stl`, `tests/review` hold their programs |
| `tests/failing/` | scenarios that must fail (ctest `WILL_FAIL`) |
| `tools/stl2json.py` | a small STL program in a compact text form → a program file |

## Building and checking

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build

C++17 and CMake 3.14, no other dependency. CI builds and tests on Linux,
macOS and Windows (`-DS7FORGE_STATIC=ON`) and attaches the binaries to a
release for a `v*` tag.

Before a release, besides the tests: the suite under AddressSanitizer and
UBSan (gcc), a clang build with `-Wall -Wextra -Wshadow -Wconversion`, every
program tiaconv exports from its fixtures and the local samples run for a
few seconds under the sanitizers (exit 0 or 3 only), and a review of the
semantics by someone (or an agent) who has not written the code, with what
it finds kept as regression scenarios (`tests/review`, `tests/scenarios/stl_*`).

## The model of the CPU

### Loading

- The program file must have `format` `tiaconv-program` and
  `schema_version` 2; anything else is refused with a message.
- Each data block becomes a tree of variables (`Var`: a leaf, a structure or
  an array, arrays expanded into their elements) from its `members` with
  their `type` descriptors and `start` values. A program is limited to a few
  million variables (`kMaxVariables`) and 64 levels of nesting, so that a
  damaged file cannot exhaust memory.
- A PLC tag with an address in I, Q or M lives in that area; a tag without
  one (or of a type that is not elementary) gets a variable of its own. A tag
  of the type Timer or Counter (`%T5`, `%C5`) names an S5 timer or counter.
- The user constants of the tag tables are values; a reference of the kind
  `tag` whose name is a user constant reads the constant (tiaconv exports
  some uses of user constants that way).
- The OBs are taken from `runtime`: `startup`, `program_cycle`, and of the
  `interrupts` the cyclic ones (`cycle_us` set). An OB of another event is
  never run and named in a warning.

### Time and the cycle

- Time is virtual, in nanoseconds. A cycle (`scan()`) runs the program
  cycle OBs from the time `t`; then, up to the end of the cycle, the
  interrupt OBs as their events come; then time is the end of the cycle:
  `t + scan time`, or later when the OBs took longer. The scan time is a
  minimum cycle time; its default is 10 ms.
- By default a statement takes no time (`instruction_us` 0). Then an
  interrupt cannot come within OB 1: one due at the start of the cycle runs
  before OB 1, the others after it, at their time. This is the old behaviour
  and keeps the scenarios written for it valid.
- With an instruction time every statement (SCL, LAD/FBD, one STL line,
  each turn of a loop) advances the time by it, so the program takes time
  as on the CPU. Between two statements the interrupts due of a higher
  priority than the OB running run first: a cyclic interrupt interrupts OB
  1, a higher priority interrupts a lower one, one of the same or a lower
  priority waits. The registers DB, DI, AR1, AR2, the MCR stack and the
  status of the last call are kept for the interrupted OB, as the CPU keeps
  them. The instruction time is the same for all statements: a mean, not
  the execution times of the CPU's instructions.
- The maximum cycle time is 150 ms (TIA Portal's default; the export does
  not have the setting). With an instruction time, a cycle beyond it calls
  the time error OB (OB 80) when there is one, and goes on without one (the
  S7-1500 ignores the error without OB 80); beyond twice the maximum the CPU
  goes to STOP. The interrupts that run after OB 1 in the same cycle count
  in its time as well, so an interrupt OB that takes longer than its own
  cycle overloads the CPU as on the real one. Without an instruction time it is a count of statements per
  OB run (2 000 000): a cycle that does not end puts the CPU in STOP. FOR,
  WHILE, REPEAT and STL jumps all count.
- In STOP, time still passes (`runFor`), the program does not run.
  `warmRestart()` (STOP to RUN) resets the non-retentive data and runs the
  startup OBs; `powerOn()` resets everything first. Inputs (%I) keep their
  values over a restart: they are what the field gives. Outputs, bit memory,
  S5 timers and counters are reset: no retentive range of M, T or C is known
  from the export. No interrupt runs during the startup OBs; its events wait
  for the first cycle.
- `Initial_Call` of an OB is TRUE in the first cycle after a restart, and
  for an interrupt OB at its first run after a restart.

### Interrupts

`interrupts.cpp`. Every OB that is not a startup or program cycle OB is an
interrupt OB with the priority of the export (2 to 26) and the number of
its events that may wait (`max_buffered_events`, 1 to 12). Its kind comes
from the event's name (`CyclicInterrupt`, `TimeDelayInterrupt`,
`TimeOfDay`, `HardwareInterrupt`, `TimeError`), else the event class (30,
20, 10, 40), else the OB number (30–38, 20–23, 10–17, 40–47, 80). The
names and classes of the time delay, time of day, hardware and time error
OBs are not seen in a test project yet (s17 will show them).

- Of the OBs due, the highest priority runs first; of the same priority
  the event that came first.
- Cyclic: every `cycle_us`, shifted by `phase_shift_us`, from a restart.
  Events that come while it waits are kept up to the number that may wait;
  the rest are lost, with a warning, and `Event_Count` of the OB's start
  information says how many were lost.
- Time delay (OB 20–23): `SRT_DINT(OB_NR, DTIME, SIGN)` starts it, the OB
  runs once `DTIME` after the call (1 to 60000 ms), with `SIGN` in its
  start information `Sign`. A start while it runs starts it anew from the
  new call (not documented for the S7-1500; S7Forge's choice). `CAN_DINT`
  cancels it (16#80A0 when it was not started), `QRY_DINT` gives STATUS
  with bit 2 started and bit 4 the OB there. The start happens when the
  instruction runs, i.e. with EN TRUE; a page of Siemens China describes a
  start on the falling edge of EN, which is not modelled (to check on a
  CPU).
- Time of day (OB 10–17): the clock of the CPU runs with the virtual time,
  from 1970-01-01 00:00 or the scenario's `clock`, in ns in 64 bits: up to
  the end of 2261 (a DTL goes to 2262-04-11). `SET_TINTL(OB_NR, SDT,
  LOCAL, PERIOD, ACTIVATE)` sets the start (to the second) and the period
  (16#0000 once, 16#0201 every minute, 16#0401 hourly, 16#1001 daily,
  16#1201 weekly, 16#1401 monthly, 16#1801 yearly, 16#2001 at the end of
  the month; another is 16#8092); `ACT_TINT` activates, `CAN_TINT`
  cancels, `QRY_TINT` gives STATUS (bit 2 active, bit 4 the OB there).
  Activated with a start in the past, a periodic one runs at its next time
  from now, a single one is refused (16#80A1). Monthly on a day a month does
  not have (the 31st) leaves that month out; yearly on 29 February only in
  leap years. A change of the clock (`WR_SYS_T`, the scenario) moves the
  next time; one passed by the change is not run (the CPU would run it once,
  `CaughtUp`). There is no time zone: LOCAL and RD_LOC_T are the system
  time. The time-of-day interrupts configured in the OB's properties (not
  by SET_TINTL) are not in the export yet: s17 will show where they are.
- Hardware (OB 40–47): the program file does not say which input edge
  raises it (the hardware configuration is not exported yet), so a scenario
  raises it (`raise`), or the command line (`--raise`). It runs as soon as
  the priorities let it: before the next statement, or before OB 1.
- Time error (OB 80): at the maximum cycle time, above. Lost events of other
  OBs do not call it yet.
- Not done: the start information other than `Initial_Call`, `Event_Count`
  and `Sign` (LADDR, IChannel, EventType of a hardware interrupt, CaughtUp);
  DIS_IRT, EN_IRT, DIS_AIRT, EN_AIRT; ATTACH and DETACH; diagnostic and
  error OBs (OB 82, 83, 86, 121, 122); the S7-1500's other reactions to
  overload.
- The return codes are those of the S7 instructions (16#8090 a wrong OB
  number or an OB of another kind, 16#8091 a wrong value, 16#8092 a wrong
  period, 16#80A0 not started or no start time, 16#80A1 in the past); with
  one, RET_VAL is negative as an Int, ENO is FALSE and a warning says so.

### Memory

- **Values** (`Val`) carry a kind and, where known, their elementary type.
  Times, dates and times of day are in nanoseconds (a Date in days since
  1990-01-01), a Char as its code.
- **Storing a value** converts it to the type of the place: whole numbers
  wrap to their size and sign, Real is rounded to single precision, a Real
  to an integer rounds half to even, Time keeps whole milliseconds (LTime
  nanoseconds), a String is cut to its length, S5Time to its time base. A
  Time stored into or converted to an integer gives its units (ms for Time
  and TOD, ns for LTime). A value that is not there (a function not
  simulated) is not stored: the place keeps its value.
- **The areas I, Q, M** are 64 KiB each, big-endian as in the CPU, Real as
  IEEE 754, Time as a DInt of ms, S5Time as BCD with its time base.
- **ULInt and LWord** are kept in 64 signed bits: values from 2^63 on are
  read correctly but compare and divide wrong. Accepted for now.

### The code tree

The code of LAD, FBD and SCL comes as one tree from tiaconv (statements and
expressions with `ref_id`s into the block's references); S7Forge interprets
it directly, without compiling it first. That keeps the interpreter close to
the format; it costs speed (about 15 µs a cycle for the s15_motor program),
which a later compile step can win back.

- **Operands** are resolved per use from their reference: a tag, a data
  block member (its `path` from the DB down, array indices as `ref_id`s), a
  local (the frame's temporaries and parameters, an FB's instance, an FC's
  in-outs by reference), a constant. A bit, byte or word of a variable
  (`%X3`, `.%B1`) is a slice of it.
- **Both operands of AND and OR are always evaluated** (no short circuit):
  an edge instruction in the second one must update its memory.
- **Wires** (the connections inside a LAD/FBD network) live per network.
- **Integer arithmetic** is done in 64 bits with wrapping and cut to the
  result type; a division by zero gives 0 with a warning (the CPU sets ENO
  FALSE, which is not modelled); `LInt` MIN / -1 does not trap. Real
  arithmetic is in double, rounded to the result type; a Real division by
  zero gives Inf as in the CPU. An overflow never sets ENO FALSE.
- **Comparisons** of mixed types compare as Real when one side is Real,
  as strings when one is a String.
- **FOR** evaluates its end and step once; a step of 0 stops the CPU (it
  would never end); the loop variable wraps past the end of its type as in
  the CPU. EXIT and CONTINUE end or skip the innermost loop.
- **An array index out of bounds** puts the CPU in STOP.

### Calls

- **FB:** the inputs and in-outs are copied into the instance, the block
  runs, the outputs and in-outs are copied back to the actual parameters. A
  parameter not given keeps its value in the instance. The instance is a data
  block or a multi-instance (a member of the caller's instance, or of an
  FC's in-out: an instance passed to an FC as in-out is used by reference).
- **FC:** a frame with its parameters and temporaries; inputs are copied in,
  in-outs are the actual parameters themselves (by reference), outputs are
  copied out after; the return value is the value of the call. Temporaries
  and outputs start at their default values (the CPU leaves them
  undefined).
- **EN/ENO:** a call with EN FALSE is not made and its ENO is FALSE. ENO of
  a block is what the block leaves in its ENO (SCL `ENO := ...`, BR of an
  STL block), TRUE otherwise; of an instruction or a built-in function TRUE.
- Calls nest at most 64 deep; deeper is taken as a recursion and stops the
  CPU.
- A block that is protected or not read: the call does nothing, with a
  warning. A pointer (`P#DB2.DBX0.0 WORD 100`) as a parameter is not
  simulated.

### Instructions

- **IEC timers** (TON, TOF, TP, TONR) keep their state outside the instance
  (keyed by the instance), measured on the virtual time from the cycle in
  which IN changed; Q and ET change when the timer is called (on an
  S7-1500 they also change when read; not modelled). TONR keeps its
  accumulated time in ET over a restart when the instance is retentive, and
  starts from 0 when R goes off with IN on.
- **IEC counters** (CTU, CTD, CTUD) take the edge of CU and CD from the
  instance's member, R before LD; CTU's Q is QU, CTD's QD.
- **R_TRIG, F_TRIG**: Q for one call on an edge of CLK.
- **Built-in functions:** the conversions `X_TO_Y` (to the type named after
  `_TO_`), ABS, MIN, MAX, LIMIT, SEL, MUX, MOVE, SHL, SHR (a signed number
  keeps its sign), ROL, ROR, SWAP, the Real functions, TRUNC, ROUND (half to
  even), CEIL, FLOOR, NORM_X, SCALE_X, and the string functions (counting
  bytes, not characters). Anything else gives no value, with a warning, so
  its target keeps its value.
- The instructions of the Siemens libraries other than these
  (communication, PID, motion) are not simulated: their outputs keep their
  values.

### Operands by name (scenarios, the command line)

`"DB".member.sub[3]`, a tag, `%MW20`, `"DB".word.%X3`. Indices must be
whole numbers; a bit or part beyond the width of the variable is refused.

## STL

STL runs as on an S7-1500 (the S7-1200 has no STL). The STL statements of a
block (all its networks) are one list, so that a jump can go to a label in
another network; a network that is not STL runs as a tree. An STL statement
outside an STL network is not run.

### The status word and the accumulators

- The RLO is kept as the current AND term and an OR bit (`O` without an
  operand: AND before OR). The first check (/FC) starts a logic string.
  `=`, S, R, SET, CLR, the conditional jumps, CALL, the timer, counter and
  MCR instructions end it (/FC 0, OR 0). Brackets (A( ... )) nest 7 deep;
  an eighth stops the CPU.
- BR, OV, OS, CC0, CC1 as in the manual; STA is not modelled. `L STW` gives
  /FC, RLO, OR, OS, OV, CC0, CC1, BR; `T STW` writes all but /FC, STA, OR.
  Status bits as operands: BR, OV, OS, UO and the conditions ==0, <>0, >0,
  <0, >=0, <=0.
- Four 32-bit accumulators; an operation of ACCU1 and ACCU2 moves ACCU3 and
  ACCU4 up (+I, +D, +R, ...); word logic does not.
- A called block starts with its accumulators at 0 and the caller's are kept
  over the call. On an S7-300/400 they pass through a call, on an S7-1500
  their contents after a call are not defined; the simpler model was taken.
- `L` puts an 8- or 16-bit variable in the low word with the high word 0
  (as `L MW10`), a constant of a signed type sign-extended (so that both +I
  and +D work on it), a Real as its bits, a Time in ms, an S5Time as BCD
  with its time base. An LReal or a 64-bit number is loaded as a Real or its
  low 32 bits, with a warning. `T` converts back by the type of the place.
- Arithmetic sets CC by the sign of the result; after an overflow of *I and
  *D by the sign of the true result, of +I, +D by the wrapped one. Real
  results: NaN sets CC 11 and OV, a denormal counts as 0 with OV. An
  unordered comparison sets OS too. RND, TRUNC, ITB, DTB clear OV when they
  succeed; ITB and DTB out of range set OV and leave ACCU1.
- BTI, BTD of a number that is not BCD stop the CPU.

### Jumps and blocks

- JL picks the n-th JU of the list after it (ACCU1-LL), past the end the
  label of the list. LOOP counts ACCU1-L down.
- BE, BEU end the block; BEC when the RLO is TRUE. The ENO of an STL block
  is BR at its end.
- CALL is a call of the code tree (its parameters are the operands after the
  block and its instance); BR afterwards is the callee's ENO.

### German mnemonics

U, UN, SPB, ... are read as A, AN, JC, .... The S5 timers clash (German SE
is the English SD): a block is taken as German when it uses a mnemonic only
German has (U, SPB, BEA, SI, SV, SA, ZV, ZR, ...) or an operand of a German
area (E, A, PE, PA, Z), per block.

### Operands from the text

The code tree has no node for absolute and indirect operands, S5 timers and
counters or pointers, so tiaconv gives each STL statement its operand as the
editor shows it (`text`). S7Forge reads every operand from the text first;
when the text is not an address (a name in quotes, `#local`, a constant) the
operand comes from the tree. Programs without `text` (older exports) still
run; the operand text is then the names of the expressions.

- I, Q, M, PI, PQ (the process image: there is no I/O; a write through an
  area-crossing pointer of the periphery goes to the outputs), DBX ... DBD
  and DIX ... DID of the open DB and DI, DB5.DBW2, L (local data, see
  below); T 5, C 5 (Z 5); in German E, A, PE, PA.
- Memory-indirect: `MW [MD 10]` (a double word with the address as
  byte·8+bit), `T [MW 4]`, `OPN DB [MW 2]` (a word with the number). The
  editor shows the tag of the address when there is one (`DBW ["Tag_7" ]`
  for `DBW [MD 44]`, s16_stl), so the operand in brackets may also be a tag
  or a `#local`.
- Register-indirect: `DBW [AR1,P#2.0]` (the address in AR1 plus the offset,
  within the area named), and area-crossing `W [AR2,P#0.0]` (the area from
  the register's byte 0: 16#80 P, 16#81 I, 16#82 Q, 16#83 M, 16#84 DB,
  16#85 DI, 16#86 L).
- Pointers: `P#M10.0` (area-crossing), `P#10.0`, `P##temp`.
- An operand that is only in the text is a constant (`C#5`, `S5T#2S`) or a
  status bit (`A OV`, `A >0`): tiaconv puts into `exprs` only what is a
  reference. A count `C#5` is BCD in the accumulator, whatever its parsed
  value.
- tiaconv also gives an access by address as a reference of the kind
  `address` with its area, bit offset and size; S7Forge reads the text for
  STL and does not use it yet.
- Addresses beyond 65535, timer and counter numbers beyond 2047, and
  addresses beyond an area put the CPU in STOP or are refused with a
  warning; nothing indexes outside the memory.

### Data blocks as bytes

A data block with standard access is laid out in bytes as the CPU lays it
out, so that `DBW 2` reaches the member at byte 2: a Bool takes a bit and
Bools in a row share a byte; Byte, Char, SInt, USInt take the next byte;
everything else starts on an even byte; a structure starts on an even byte
and takes an even number of bytes; an array starts on an even byte, its
Bools and bytes packed, other elements each on an even byte; a String takes
its length + 2 bytes; an ANY 10, a POINTER 6, a block number a word (S7Forge
keeps no value in them, only their bytes); each section of an instance
starts on a word. Where
tiaconv gives the offset of a member of the DB itself (`offset_bits`), that
one is taken. The layout was checked against the offsets TIA Portal stores
for every member of DB_Standard (s13_hmi, with a PLC data type and arrays of
strings) and ZZSTD_DB (s14_code_rewritten). What is not a variable (the
padding, the bits of a byte next to a Bool, the bytes of an ANY) is kept as
last written, so that it reads back as on the CPU; it is 0 after a restart. An access beyond the end of a DB stops the
CPU; a DB with optimized access has no addresses (a warning, nothing done).

### Local data by address

`L 0.0`, `LB 1`, `LW 2`, `LD 4` and pointers to L (16#86) reach the local
data of the block running. Its Temp section is laid out from L 0 with the
rules of a data block (tiaconv gives no offsets for the interface; the
layout is computed). The bytes after the temporaries are local data without
a name: a 64 KiB buffer per call, 0 at the start of the call (the CPU leaves
them undefined). Beyond 64 KiB the CPU goes to STOP. `P##name` is an
area-crossing pointer to a temporary (16#86 with its bit address), as
`LAR1 P##tmp` uses it. A block with optimized
access has no local data by address: a warning, nothing done. The local
data of an OB are its Temp section too; S7-300 OBs declare their 20 bytes
of start information there, which then lie at L 0 as on that CPU, but their
values are not filled in.

### Registers over calls

The open DB and DI and AR1, AR2 are CPU registers. An FB opens its instance
as DI and, for a multi-instance, AR2 points into it (16#84 with the bit
offset of the multi-instance); DB, DI and AR2 are back after the call. AR1
is not restored.

### S5 timers and counters

- One set of operations (`s5.cpp`) serves STL and LAD/FBD, so a timer
  started by a box and reset by STL behaves the same, with one edge memory
  per timer and per counter input.
- 2048 of each. A timer keeps its start time and preset and is checked
  against the virtual time whenever it is used (`s5Update`), so it does not
  need a tick.
- SP: Q while running and the start is on; SE: Q while running, a new edge
  restarts; SD: Q when the time is up and the start still on, off with the
  start; SS: Q when the time is up until R, a new edge restarts; SF: Q while
  the start is on and until the time is up after it goes off. A timer starts
  on a rising edge of the start (one edge memory per timer); FR on a rising
  edge clears that memory, so the next start instruction with its start on
  starts again. R resets Q, the time value and the time base.
- The preset comes from ACCU1-L in S5TIME; one that is not BCD stops the
  CPU. `L T` gives the units left of the time base (rounded up), `LC T` the
  same in BCD with the time base. S5T# constants take the finest time base
  that holds them, as TIA Portal encodes them (S5T#10S is 16#1100).
- Counters 0 to 999: CU and CD on rising edges, S loads ACCU1-L in BCD
  (one that is not BCD stops the CPU), R, FR clears the edge memories. A C
  is TRUE when the count is not 0. S also clears the edge memories of CU
  and CD: "if the counter is set and the RLO at CU is 1, the counter counts
  in the next scan cycle, even without a change of edge" (the LAD manual,
  S_CUD).
- Checked against Siemens' documentation for the S7-300/400 (the STL and
  LAD reference manuals, and the SCE training appendix on the programming
  instructions): the start on an edge and the restart of SE and SS on a new
  edge while running, SS running on without its start, SF stopped by a new
  start, SP and SD keeping their time value when stopped, FR, R of a counter
  working statically, the four time bases with a time truncated to its base
  (S5T#2S15MS is 16#0201) and the largest time S5T#2H46M30S (16#3999),
  counting stopping at 999 and 0. Each rule is a step of the scenario
  `stl_s5rules`. Not documented there, so taken from the model: the time
  value while running is the units left rounded up (the CPU counts it down
  every time base on its own clock, so a reading between two ticks may be
  one unit less).

### S5 timers and counters in LAD and FBD

tiaconv writes a box as a call of its name with its pins; a special coil
(SD, CU, ...) as a call of the coil's name with `in` (the power flow),
`operand` and `value`. The project s16_stl shows the names stored: the
boxes `Sp`, `Sd`, `Cud` (S_PULSE, S_ODT, S_CUD; so `Se`, `Ss`, `Sf`, `Cu`,
`Cd` for the others), the coils `SdCoil`, `CuCoil`, `ScCoil`, the pins
`timer`, `s`, `tv`, `r`, `q`, `rt` (BI), `rtbcd` (BCD) and `counter`, `cu`,
`cd`, `s`, `pv`, `r`, `cv`, `q`. S7Forge takes these, and also the names the
editor shows in English and German (S_ODT, S_EVERZ, ...) and other pin
names (T_NO, TNR; TW; BI, DUAL; BCD, DEZ; ...), ignoring case and `_`.

- A box runs as its STL: the start with the time value, then R, then BI,
  BCD and Q (a counter: CU, CD, S with PV, R, then CV, CV_BCD, Q). An input
  that is not connected does nothing, as the CPU generates no instruction
  for it: its edge memory stays, so an SC coil on the same counter sees no
  new edge. A start without a time value starts with 0, with a warning.
- A block of the program (kind `block`) is always the block, even when its
  name is one of the S5 boxes (an FC called `Zaehler`).
- A coil is the instruction of the same name on the power flow; SC is S C
  with the count.
- A time value that is a Time (an S5TIME constant) is encoded with the
  finest time base; a number is taken as the bits of an S5TIME; an S5TIME
  variable in I, Q or M gives its own bits, with its time base. An S5TIME
  in a data block is kept as a time and loses the time base it was written
  with (the finest one is taken again); accepted for now.
- A count C#n beyond 999 is limited to 999, with a warning (TIA Portal
  refuses it). A count
  written C#n is decimal; a number is taken as BCD, as the CPU takes it.
- A contact on a timer is its Q, on a counter "the count is not 0"; the R
  coil (`IF in THEN T := FALSE` in the tree) resets the timer or counter.
- A timer or counter is named by a tag of the type Timer or Counter, or by
  its address (%T5, %C5).

### MCR

The S7-1500 refuses MCR ("The instruction is not supported by the current
CPU", s16_stl); the S7-300/400 has it, and STL migrated from them may use
it, so S7Forge runs it. MCRA and MCRD switch the MCR on and off in the block; MCR( pushes the RLO,
)MCR pops it, 8 deep. With MCRA on and a FALSE anywhere on the stack, `=`
and T write 0, S and R (also on timers and counters) do nothing; CU and CD
are not MCR-dependent. The stack is passed on to called blocks (with MCRA
off in them until they switch it on); each OB starts with an empty one.

## Scenarios

- The CPU is powered on before the first step. A step sets, restarts, runs
  cycles (`scans`) or time (`run_ms`), then checks; at most 100 million
  cycles a step.
- A Real is equal within a relative 1e-6; a Bool equals a number when both
  are 0 or both are not.
- An operand that is not found, or a value that cannot be read, is an error
  of the scenario (exit 2), not a failed check.

## Open

- A compile step from the tree to a compact form, for speed.
- ENO FALSE on an overflow; IEC timers that update when read (S7-1500).
- ULInt/LWord beyond 2^63.
- The instructions of the libraries (communication, PID, motion), the
  hardware (high-speed counters, analog I/O).
- The S7-1500 compiler wants a data block opened in the network that uses
  DBX ... DBD, also after a fully qualified access; S7Forge does not check
  that and uses whatever the DB register holds.
- The project does not store the mnemonic setting: the code is stored
  without mnemonics, and German is only seen in the text of an older
  export. Tag addresses keep what was typed (`%A1.0`); tiaconv writes them
  with the international mnemonics.
