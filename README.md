# S7Forge

S7Forge simulates the program of a Siemens S7-1200 (or S7-1500) CPU on a PC. It
reads the program as [tiaconv](https://github.com/jkobierczynski/TIAconv)
exports it (`tiaconv --program-json DIR project`), so no TIA Portal and no
PLCSIM are needed. It runs the program the way the CPU does:

- the startup OBs once, then the program cycle OBs cycle after cycle;
- the cyclic interrupt OBs at their cycle and phase;
- LAD, FBD and SCL, from one code tree, and STL (see below), with S5
  timers and counters in all of them;
- FB calls with their instance data blocks and multi-instances, FC calls with
  in-out parameters by reference and a return value;
- the IEC timers and counters (TON, TOF, TP, TONR, CTU, CTD, CTUD), edges
  (P, N, P_TRIG, N_TRIG, R_TRIG, F_TRIG), the conversions (`INT_TO_REAL`, ...)
  and the common functions (ABS, MIN, MAX, LIMIT, SEL, MUX, SHL, SHR, ROL,
  ROR, SWAP, SQRT, ..., NORM_X, SCALE_X, LEN, CONCAT, LEFT, RIGHT, MID);
- EN and ENO of calls;
- the areas I, Q and M with the PLC tags at their addresses, big-endian as in
  the CPU;
- start values and retentive data: a warm restart (STOP to RUN) keeps the
  retentive values, a power-on resets everything;
- a cycle that does not end (an endless loop) and an array index out of
  bounds put the CPU in STOP, as in the CPU.

Time is virtual: every cycle takes the scan time (10 ms unless set), so a
run gives the same result every time, and 10 minutes of the program run in a
fraction of a second.

What it does not do (yet), and says so once in its warnings: blocks that
are know-how protected (their code is not in the export), the instructions
of the Siemens libraries other than the timers and counters (communication,
PID, motion), the hardware (I/O modules, high-speed counters), and interrupts
other than the cyclic ones.

Where it differs from the CPU, as far as known:

- a cyclic interrupt does not interrupt OB 1: it runs after the cycle in
  which it came due;
- an overflow does not set ENO to FALSE; the result wraps as in the CPU;
- the outputs Q and ET of an IEC timer change when it is called, also on an
  S7-1500 (which updates them when they are read too);
- ULInt and LWord are kept in 64 bits as signed numbers: comparisons and
  divisions of values from 2^63 on are wrong;
- LEN and the length of a String count bytes (UTF-8), not characters;
- a function or instruction that is not simulated leaves its outputs as
  they are.

## STL

STL runs as on an S7-1500 (the S7-1200 has no STL): the result of logic
operation with the first check, O without an operand (AND before OR) and
brackets up to 7 deep; the status bits BR, OV, OS, CC0 and CC1; four
32-bit accumulators. It takes the bit logic (A, AN, O, ON, X, XN and their
brackets, =, S, R, SET, CLR, NOT, SAVE, FP, FN), load and transfer (L, T,
TAK, PUSH, POP, ENT, LEAVE), the arithmetic of Int, DInt and Real (+I ... /I,
+D ... MOD, +R ... /R, + constant, INC, DEC, NEGI, NEGD, NEGR, INVI, INVD,
ABS, SQR, SQRT, EXP, LN, SIN ... ATAN), the comparisons (==I ... <=R), the
conversions (ITD, DTR, RND, RND+, RND-, TRUNC, ITB, BTI, DTB, BTD, CAW,
CAD), word logic (AW ... XOD), shifts and rotations (SLW ... RRD), the
jumps (JU, JC ... JUO, JL, LOOP, to a label in any network of the block), BE,
BEU, BEC and CALL. The ENO of an STL block is BR at its end. German
mnemonics (U, SPB, ...) are read too.

S5 timers and counters (2048 of each): SP, SE, SD, SS, SF, FR and R with
a time value from ACCU1 in S5TIME; CU, CD, S (from ACCU1 in BCD), R and FR;
A, L (binary) and LC (BCD, a timer with its time base). A timer runs on the
virtual time and is checked whenever it is used. In German, SI, SV, SE, SS
and SA; a block in German mnemonics is told by the ones only German has (U,
SPB, ...), so that SE is the on delay there.

Absolute and indirect addresses: I, Q, M, PI, PQ (the process image: there
is no I/O; an area-crossing pointer of the periphery writes the outputs); DBX ... DBD and DIX ... DID of the data block opened with OPN DB,
OPN DI (by number, by name, or DB [MW 2]); DB5.DBW2; memory-indirect
(`MW [MD 10]`, `T [MW 4]`); register-indirect (`DBW [AR1,P#2.0]`) and
area-crossing (`W [AR2,P#0.0]`) with LAR1, LAR2, TAR1, TAR2, CAR, +AR1,
+AR2; pointers (`P#M10.0`); L DBNO, DBLG, DINO, DILG; CDB; L and T STW.
A data block with standard access is laid out in bytes as the CPU lays it
out (checked against the offsets TIA Portal stores), so `DBW 2` is the
member at byte 2; a data block with optimized access has no addresses. An
access beyond the end of a data block puts the CPU in STOP. An FB opens its
instance as DI (a multi-instance: AR2 points into it); DB, DI and AR2 are
back after a call.

MCR: MCRA, MCRD, MCR( and )MCR, 8 deep; with a FALSE on the MCR stack, =
and T write 0 and S and R do nothing, in the blocks that have MCRA on. The
stack is passed on to called blocks; each OB starts with an empty one.

Local data by address (`L 0.0`, `LW 2`, `LD 4`, pointers to L) in blocks
with standard access: the temporaries of the block are laid out from L 0
as a data block is, and the bytes after them are local data without a
name, kept for the call.

Not taken: the status word bits as operands other than BR, OV, OS and the
conditions (>0, ...). A called block starts with its accumulators at 0 and
the caller's are kept over the call (on an S7-300/400 they pass through; on
an S7-1500 they are not defined after a call).

## S5 timers and counters in LAD and FBD

The boxes S_PULSE, S_PEXT, S_ODT, S_ODTS, S_OFFDT (in German S_IMPULS,
S_VIMP, S_EVERZ, S_SEVERZ, S_AVERZ), S_CU, S_CD, S_CUD (Z_VORW, Z_RUECK,
ZAEHLER), the coils SP, SE, SD, SS, SF, CU, CD and SC, contacts on a timer
or counter, and the R coil on one, on the same timers and counters as STL.
A time value is an S5TIME constant or a word with its bits, a count C#n or
a word in BCD.

What goes into ACCU1: an 8- or 16-bit operand in the low word with the
high word 0, as `L MW10` does; a constant of a signed type sign-extended;
a Real as its bits; a Time in ms; an S5Time as its BCD with the time base,
as the CPU keeps it in memory too. An LReal or a 64-bit number is loaded as
a Real or its low 32 bits, with a warning.

tiaconv gives each STL statement its operand as the editor shows it
(`text`); S7Forge reads absolute and indirect operands from there.

`tools/stl2json.py` turns a small STL program in a compact text form into a
program file, for tests: see `tests/stl`.

## Building

C++17 and CMake 3.14, nothing else:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build

## Use

    tiaconv --program-json out MyProject      # out/PLC_1.json for each PLC
    s7forge info out/PLC_1.json
    s7forge run out/PLC_1.json --set Start=TRUE --for 6000 --show Motor --show "\"Data\".count"
    s7forge test tests/scenarios/s15_motor.json

`run` powers the CPU on, sets the operands, runs for the time given (in ms,
1000 unless set) and shows the operands asked for. An operand is a PLC tag
(`Start`), a data block member (`"Data".count`, `"Motor_DB".delay.ET`,
`"Data".values[3]`, `"Data".flags.%X2`) or an address (`%I0.0`, `%MW20`). A
value is `TRUE`, `12`, `2.5`, `T#5s`, `16#FF` or `'text'`.

`test` runs scenario files: inputs set step by step, time run, and values
checked after each step. The format is in [docs/SCENARIOS.md](docs/SCENARIOS.md).
The exit status is 0 when every check passed, 1 when one failed, 2 on an
error and 3 (for `run`) when the CPU went to STOP.

## Tests

`tests/data` holds the programs of two of tiaconv's test projects, exported
by tiaconv: `MOTORPLC.json` (s15_motor: a motor start/stop with a 5 s delay,
in LAD and in SCL) and `ZZPLC.json` (s14_code_rewritten: LAD, FBD, SCL, STL
and a know-how protected block). The scenarios in `tests/scenarios` check
them; the values expected in s14 were derived by hand from the code.

## Licence

GPL-3.0-or-later; see [LICENSE](LICENSE).
