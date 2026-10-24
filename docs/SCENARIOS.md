# Scenario files

A scenario is a JSON file: a program, and steps that set operands, let time
pass and check values. `s7forge test FILE...` runs them.

```json
{
  "comment": "what the scenario is about",
  "program": "../data/MOTORPLC.json",
  "scan_ms": 10,
  "steps": [
    {"comment": "start pressed", "set": {"StartPB": true}, "run_ms": 50},
    {"set": {"StartPB": false}, "run_ms": 5000,
     "expect": {"MotorLadOut": true, "\"MotorData\".starts": 2, "\"MotorLAD_DB\".delay.ET": "T#5s"}},
    {"restart": "warm", "expect": {"\"MotorData\".starts": 0}}
  ]
}
```

| key | |
|---|---|
| `program` | the program file of tiaconv `--program-json`; a relative path is from the scenario file |
| `scan_ms` | the time one cycle takes, in ms (default 10); may have a fraction. A minimum: a cycle whose OBs take longer is longer |
| `instruction_us` | the time every statement takes, in µs (default 0: the program takes no time). With it an interrupt of a higher priority runs between two statements of the OB it interrupts, and the maximum cycle time (150 ms) counts |
| `clock` | the date and time of the CPU's clock at the start: `"2026-10-24 08:00:00"` (or `DTL#2026-10-24-08:00:00`), 1970 to 2261; default 1970-01-01 00:00 |
| `steps` | the steps, in order |

The CPU is powered on before the first step. Each step does, in this order:

| key | |
|---|---|
| `comment` | shown in a message about the step |
| `set` | operands and their values, written before the step runs |
| `clock` | the clock set to this date and time |
| `raise` | the event of an interrupt OB, by the OB's name or number or the event's name (`Rising edge0`), or a list of them: a hardware interrupt. It runs as soon as its priority lets it. Setting an input that has a hardware interrupt raises it on its edge as well |
| `restart` | `"warm"`: STOP to RUN, retentive values kept; `"power"`: power off and on, everything to its start value. The startup OBs run. |
| `scans` | a number of cycles to run |
| `run_ms` | the time to run, in ms: cycles until it has passed |
| `expect` | operands and the values they must have after the step |
| `expect_fault` | `true` when the CPU is expected to go to STOP in this step; without it a STOP fails the scenario |

Operands are written as in TIA Portal: a PLC tag (`StartPB` or `"StartPB"`), a
data block member (`"MotorData".starts`, `"MotorLAD_DB".delay.Q`,
`"Data".arr[2]`, `"Data".word.%X3`) or an address (`%I0.0`, `%QW2`, `%MD20`).
Values are JSON `true`, `false` or numbers (with a point: a Real), or a string
with an S7 literal: `"T#1m_30s"`, `"16#FF"`, `"'text'"`, `"TRUE"`.

A Real is equal to the expected value within a relative 1e-6; a Bool is equal
to a number when both are 0 or both are not.

Values set on inputs (`%I`) stay until set again, also over a restart: they are
what the field gives. Outputs and bit memory are cleared on a restart.

How time passes: a cycle runs the program cycle OBs from the time `t`, then
the interrupt OBs due up to the end of the cycle, `t + scan_ms` (or later when
the OBs took longer), then time is the end of the cycle. Without
`instruction_us` the OBs take no time, and an interrupt comes before or after
OB 1, never within it. An IEC timer measures from the cycle in which its
input changed. DEVELOPMENT.md ("Interrupts") has the details of the
interrupt OBs and the instructions SRT_DINT, SET_TINTL and the others.
