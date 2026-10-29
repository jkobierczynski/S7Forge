# Modbus/TCP

`s7forge serve` runs a program in real time and answers Modbus/TCP on the
network, so that a Modbus client, a proxy, a gateway or a scanner can talk
to a PLC whose behaviour is that of the program. It is meant for testing
such tools against a known, repeatable device: the values a client reads
come from the program, and what it writes changes what the program does.

```
s7forge serve tests/data/MOTORPLC.json --map tests/modbus/motor.map.json -v
```

| option | |
|---|---|
| `--listen ADDRESS` | the IPv4 address to listen on; default `127.0.0.1`, this computer only. `0.0.0.0` listens on every interface: any host that reaches it can read and write, Modbus has no authentication |
| `--port N` | default 5020; 502, the standard port, needs administrator rights on most systems; 0 takes a free port |
| `--map MAP.json` | which Modbus addresses reach which operands (below); without it the default map |
| `--read-only` | refuse every write (function codes 5, 6, 15, 16, 23) with exception 01, illegal function |
| `--scan-ms N` | the cycle time (default 10 ms) |
| `--instruction-us N` | the time of a statement (see SCENARIOS.md) |
| `--speed X` | the simulated time runs X times as fast as real time (default 1) |
| `--for MS` | stop after this much simulated time (default: run until stopped, Ctrl+C) |
| `--set OPERAND=VALUE` | a value before the start (`--set StopPB=TRUE`) |
| `--clock DATETIME` | the CPU's clock at the start (`"2026-10-24 08:00:00"`) |
| `-v` | one line per connection and request on standard error |

## How it runs

One thread runs the simulation and the server. The program runs in cycles
as in `run`, paced to real time (times `--speed`); between two cycles the
server reads what has come in and answers it. So a client reads the values
as they are at the end of a cycle, and what it writes is there for the next
cycle, as with a CPU whose program reads its Modbus data at the start of the
cycle. When the CPU goes to STOP (a fault) the server goes on answering
with the values as they were.

Writing an input (%I) sets it as the field would; the program sees it from
the next cycle on, and an input with a hardware interrupt raises it on its
edge. Writing an output (%Q) or a variable the program writes itself is
allowed, but the program overwrites it in its next cycle, as on a CPU.

At most 16 clients at a time; a client without a whole request for 60 s is
dropped, and one that leaves a frame incomplete for 10 s; a frame that is
not Modbus/TCP (protocol identifier not 0, a length below 2 or above 254)
closes the connection. A client that closes while answers are waiting does
not stop the server.

When the program cannot keep up with real time (a short `--scan-ms`, a high
`--speed`), the simulation runs as fast as it can and says so once; the
server still answers within about 20 ms, and the time it cannot catch up is
dropped.

## The default map

| table | Modbus address n | example |
|---|---|---|
| coils (read and write) | bit n of %Q: %Q(n/8).(n%8) | coil 9 is %Q1.1 |
| discrete inputs (read) | bit n of %I | discrete input 0 is %I0.0 |
| holding registers (read and write) | %MW(2n) | register 10 is %MW20 |
| input registers (read) | %IW(2n) | register 1 is %IW2 |

Addresses are as on the wire, from 0. Tools that count from 1 (the "40001"
style, `mbpoll` without `-0`) show them one higher.

## A map file

```json
{
  "unit_id": 1,
  "coils": [{"address": 0, "operand": "%I0.0", "count": 3}],
  "discrete_inputs": [{"address": 0, "operand": "MotorLadOut"}],
  "holding_registers": [
    {"address": 0, "operand": "\"MotorData\".starts"},
    {"address": 1, "operand": "\"MotorLAD_DB\".delay.ET"},
    {"address": 10, "operand": "%MW100", "count": 8},
    {"address": 20, "operand": "\"Data\".arr[0]", "count": 5}
  ],
  "input_registers": [{"address": 0, "operand": "%IW64", "count": 4}]
}
```

Each table is a list of ranges; addresses that no range covers answer with
exception 02, illegal data address. Ranges of a table must not overlap.

- `operand` an address in I, Q or M: a bit (`%Q0.0`) for coils and
  discrete inputs, a word (`%MW20`) or double word (`%MD20`, two registers
  each, `count` even) for registers; `count` addresses from there.
- `operand` a name: a PLC tag, a data block member (`"Motor_DB".speed`),
  as in a scenario. A Bool for a coil or discrete input; for a register a
  16-bit type takes one register, a 32-bit type (DInt, DWord, Real, Time)
  two, a 64-bit type four, the high word first. With `count`, an array
  element and the elements after it.
- `unit_id` (optional): answer only this unit identifier; other requests get
  no answer. Without it every unit identifier is answered.

The operands are checked when the server starts; a map that names
something the program does not have is refused.

## Values

A register holds the bits the PLC keeps: an Int as its two's complement, a
Real as its IEEE 754 bits (16#40490FDB is 3.14159), a Time as its
milliseconds (signed), a Time_Of_Day as milliseconds since midnight, a Date
as days since 1990-01-01, an S5Time as time base and BCD (S5T#5S is
16#0500), a Bool as 0 or 1. A Date_And_Time (8 bytes of BCD) and a String
cannot be mapped. A write of one register of a 32-bit value (function 6)
changes that half and keeps the other; a write of all its registers at once
(function 16) writes the value once.

## Function codes

| code | |
|---|---|
| 1 | read coils (1 to 2000) |
| 2 | read discrete inputs (1 to 2000) |
| 3 | read holding registers (1 to 125) |
| 4 | read input registers (1 to 125) |
| 5 | write single coil (FF00 on, 0000 off; anything else exception 03) |
| 6 | write single register |
| 15 | write multiple coils (1 to 1968) |
| 16 | write multiple registers (1 to 123) |
| 23 | read/write multiple registers (the write first) |

Any other function code answers exception 01, illegal function. A count
out of range is exception 03, illegal data value; an address not mapped is
02. A write of several addresses checks them all first: when one is not
mapped nothing is written. An operand that cannot be read or written is
exception 04, server device failure.

## Testing a client or a proxy

Against the motor of s15_motor, ten times real time:

```
s7forge serve tests/data/MOTORPLC.json --map tests/modbus/motor.map.json --speed 10 -v
mbpoll -m tcp -p 5020 -0 -1 -t 0 -r 1 -c 2 127.0.0.1 1 1   # coils 1, 2 on: stop and guard closed
mbpoll -m tcp -p 5020 -0 -1 -t 0 -r 0 127.0.0.1 1           # coil 0 on: start pressed
mbpoll -m tcp -p 5020 -0 -1 -t 0 -r 0 127.0.0.1 0           # and released
mbpoll -m tcp -p 5020 -0 -1 -t 1 -r 0 -c 2 127.0.0.1        # the motor outputs (on 5 s later)
mbpoll -m tcp -p 5020 -0 -1 -t 4 -r 0 127.0.0.1             # starts counted
```

A read-only proxy in front of it should pass function codes 1 to 4 and
answer the writes itself; with `-v` the server's log shows which requests
reached it. `--read-only` makes the server itself refuse writes, for
comparison.
