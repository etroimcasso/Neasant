# APU behavior and provenance

A reference for the parts of the 2A03's APU where the published hardware documentation is
incomplete, ambiguous or at odds with itself, and for what settles each case.

[`apu.md`](apu.md) is the usage guide: what the chip's surface is and how to drive it. This
page is the evidence behind it. Every claim below names its source, and where two sources
disagree it names the disagreement and what decided it.

## Contents

- [The evidentiary standard](#the-evidentiary-standard)
- [Sources](#sources)
- [The frame counter](#the-frame-counter)
  - [One sequence under three numberings](#one-sequence-under-three-numberings)
  - [A write to $4017 restarts the sequence two or three cycles on, by the half it lands in](#a-write-to-4017-restarts-the-sequence-two-or-three-cycles-on-by-the-half-it-lands-in)
  - [A write with bit 7 set clocks the units one cycle before the restart](#a-write-with-bit-7-set-clocks-the-units-one-cycle-before-the-restart)
  - [The flag is set on three consecutive cycles, and a read in the second or third does not keep it clear](#the-flag-is-set-on-three-consecutive-cycles-and-a-read-in-the-second-or-third-does-not-keep-it-clear)
  - [The IRQ line follows the flag by one cycle](#the-irq-line-follows-the-flag-by-one-cycle)
  - [Power-on and reset start the sequence as if $4017 were written nine cycles before the first instruction](#power-on-and-reset-start-the-sequence-as-if-4017-were-written-nine-cycles-before-the-first-instruction)
- [The length counters](#the-length-counters)
  - [A halt bit takes effect after the clock of its own cycle](#a-halt-bit-takes-effect-after-the-clock-of-its-own-cycle)
  - [A load written in the cycle of a clock is dropped when the clock counts](#a-load-written-in-the-cycle-of-a-clock-is-dropped-when-the-clock-counts)
  - [The 2A03's counter holds one less than the table's value](#the-2a03s-counter-holds-one-less-than-the-tables-value)
- [The channels](#the-channels)
  - [The pulses, the noise and the DMC step as a put cycle begins](#the-pulses-the-noise-and-the-dmc-step-as-a-put-cycle-begins)
  - [Inside a cycle the frame counter's clocks come before the timers](#inside-a-cycle-the-frame-counters-clocks-come-before-the-timers)
  - [A sweep rewrites the period only while the length counter is above 0](#a-sweep-rewrites-the-period-only-while-the-length-counter-is-above-0)
  - [The noise's shift register holds 1 at power-on](#the-noises-shift-register-holds-1-at-power-on)
  - [The triangle stands at its first step at power-on](#the-triangle-stands-at-its-first-step-at-power-on)
- [The DMC](#the-dmc)
  - [A sample's first fetch halts the CPU on the third or fourth cycle after the write](#a-samples-first-fetch-halts-the-cpu-on-the-third-or-fourth-cycle-after-the-write)
  - [The end of a sample reaches $4015 at the fetch](#the-end-of-a-sample-reaches-4015-at-the-fetch)
  - [A sample stopped just before its reload still halts the CPU for one cycle](#a-sample-stopped-just-before-its-reload-still-halts-the-cpu-for-one-cycle)
  - [A read the fetch lands on is repeated on every halted cycle](#a-read-the-fetch-lands-on-is-repeated-on-every-halted-cycle)
- [The output](#the-output)
  - [The mixer is the page's formula, where its lookup tables leave a tone](#the-mixer-is-the-pages-formula-where-its-lookup-tables-leave-a-tone)
  - [The output stage is the page's two filters at the page's corners](#the-output-stage-is-the-pages-two-filters-at-the-pages-corners)
  - [The "APU Mixer" page listed a third filter at 440 Hz until October 2026](#the-apu-mixer-page-listed-a-third-filter-at-440-hz-until-october-2026)
- [What remains open](#what-remains-open)
- [How a contested claim gets settled here](#how-a-contested-claim-gets-settled-here)

## The evidentiary standard

Four kinds of source appear here, and they do different work:

- **The documents** say what the 2A03 does: the NESdev wiki's pages, blargg's NES APU
  reference and Brad Taylor's 2A03 technical reference. Every rule in the code cites one.
- **Published measurements** say what a console did, cycle by cycle: blargg's readmes for his
  2005 frame-counter programs and for his `apu_reset` programs. A measurement is a document
  for this purpose.
- **The test programs** pass or fail on the machine. A program is evidence that a console
  behaved some way when its author ran it; it does not say why, so a program is never the
  rule itself. Where two documents disagree, the reading under which the programs pass is
  taken, and the pair is recorded here.
- **Visual 2A03**, the transistor-level simulation of the RP2A03 made from photographs of the
  die, is read as an instrument: a trace of named nodes, cycle by cycle, for a question the
  documents leave open.

Three kinds of claim follow from them:

- **Documented and corroborated.** A document states it, and a measurement or a program
  agrees.
- **Contested.** Two sources disagree, or one is silent where another speaks. Each such case
  names both, and what decided it.
- **Open.** No source here settles it. The chip's choice is stated as a choice, with what
  would settle it, under [What remains open](#what-remains-open).

Every claim here rests on a public document, a published measurement, a test program with
its own description, or a reading of the simulation. The chip is built from those and from
no other implementation's source.

## Sources

| Source | Role |
|---|---|
| NESdev wiki, "APU" and its pages "APU Frame Counter", "APU Length Counter", "APU Envelope", "APU Pulse", "APU Triangle", "APU Noise", "APU DMC" and "APU Mixer" | Primary. The registers, each unit's operation, the tables. |
| NESdev wiki, "DMA" | Primary for the DMC's sample fetch: the halt, dummy and alignment cycles, the two bugs, the collisions with OAM DMA. |
| NESdev wiki, "CPU power up state" | The chip at power-on and after reset. Several of its entries carry a question mark. |
| blargg, NES APU Sound Hardware Reference (`apu_ref.txt`, 2004) | Cross-check: the second account of every unit, from tests on a 1988 NTSC NES with the G revision of the 2A03. |
| Brad Taylor, 2A03 technical reference (2004) | Secondary; decisive once, on the sweep. |
| blargg's 2005 frame counter readme and his `apu_reset` readme | Published measurements. The first says it "either extends or contradicts" the reference and the wiki. |
| The test programs | Pass and fail: blargg's `apu_test`, `apu_reset`, 2005 set, `cpu_interrupts_v2`, `dmc_dma_during_read4`, `sprdma_and_dmc_dma` and `apu_mixer`; Fiskbit's `dmc_dma_start_test` in its two versions, whose readme tabulates an RP2A03H's results. |
| Visual 2A03 | Instrument. It settles a half of the APU cycle, an order and a condition, and the programs contradict it once, on the DMC's status. |
| "NES Audio Path", the circuit the "APU Mixer" page links, and Nexperia's 74HCU04 data sheet, Rev. 10 | The output stage's components, worked beside the page's figures; recorded and not built. |

The blargg programs come from the `christopherpow/nes-test-roms` repository at
`https://github.com/christopherpow/nes-test-roms` with their readmes and sources; his
`sprdma_and_dmc_dma` programs are described in his post of 2010-06-08, "DMA operation in
APU", at `https://forums.nesdev.org/viewtopic.php?p=62690`. Fiskbit's programs are cited by
their readme.

---

## The frame counter

### One sequence under three numberings

The three sources count the same steps from three places. The "APU Frame Counter" page
counts APU cycles from the count's return to 0 and names the half; the chip counts CPU cycles
from the sequence's origin, the tick at which its count reads 0; blargg's readme counts CPU
cycles from the `$4017` write, with the origin two cycles on.

| Step | "APU Frame Counter", APU cycles | The chip, cycles from the origin | blargg's readme, cycles from the write |
|---|---|---|---|
| 1 | 3728, PUT | 7457 | 7459 |
| 2 | 7456, PUT | 14 913 | 14 915 |
| 3 | 11185, PUT | 22 371 | 22 373 |
| the flag's three cycles | 14914, GET; 14914, PUT; 0 (14915), GET | 29 828, 29 829, 29 830 | 29 830, 29 831, 29 832 |
| 4, the 4-step sequence's last | 14914, PUT | 29 829 | 29 831 |
| 5, the 5-step sequence's last | 18640, PUT | 37 281 | 37 283 |

An APU cycle n is CPU cycles 2n and 2n + 1 from the origin, the second the page's PUT. The
page labels a step by the half in which its effect is in place, and on this chip that is the
tick ending the cycle before, a get cycle's: Visual 2A03 has the clocked units change in the
first half of a put cycle, and the envelope's decay level loads 15 at the start of the cycle
7460 after a write on a put cycle and 7461 after one on a get, which is the tick of 7459, the
readme's figure, and of 7460. `apu_test`'s `5-len_timing` and `6-irq_flag_timing` and the
2005 set's `05`, `06` and `07` pass on these cycles under both halves.

*Corroborated by* blargg's readme, Mode 0 Timing and Mode 1 Timing, and those five programs.

### A write to $4017 restarts the sequence two or three cycles on, by the half it lands in

Contested; settled by reading the pages together and by a program.

The origin is two cycles after a write that lands on a put cycle and three after one on a
get.

"APU Frame Counter", Control register: "If the write occurs during an APU cycle, the effects
occur 3 CPU cycles after the write cycle, and if the write occurs between APU cycles, the
effects occurs 4 CPU cycles after the write cycle." "APU", Frame Counter: "it happens either
2 or 3 cycles after the write (i.e. on the 2nd or 3rd cycle of the next instruction)".
blargg's 2005 readme, Clock Jitter: "if written on an odd clock, the first step of the mode is
delayed by one clock".

The three are one pair of figures counted from different cycles: the chip's tick two or
three cycles after the write is in place for an access three or four after it. What none of
them fixes is which half is the short one. `cpu_interrupts_v2`'s `4-irq_and_dma` does: it
lands an interrupt around an OAM DMA transfer whose length, 513 or 514 cycles, the "DMA" page
ties to the half its halt falls on, and its table matches its readme's only when the short
restart and the 514-cycle transfer belong to the same half, which makes the short one the
write on a put. Visual 2A03 agrees on both counts: a transfer takes 514 cycles after a `$4014`
write on a put cycle, and the sequence's first step is in place 7460 cycles after a `$4017`
write on a put and 7461 after one on a get. On the chip the delay is `restartDelay`, 3 ticks
from a put and 4 from a get with the write's own counted.

*Settled by* `4-irq_and_dma` and the reading.

### A write with bit 7 set clocks the units one cycle before the restart

Contested; settled by the simulation.

A write to `$4017` with bit 7 set runs the quarter- and half-frame clocks in the tick before
the restart: one cycle after a write on a put and two after one on a get.

The sources give three figures. blargg's 2005 readme, Mode 1 Timing, puts "Clock linear &
length" at 1, the clock after the write; "APU", Frame Counter, has the triggers "2 or 3
cycles after the write"; "APU Frame Counter" has the write's effects 3 or 4 CPU cycles after
the write cycle and adds "both Quarter frame and Half frame clocks are also generated". No
program here tells them apart: the earliest access a program can make after a `$4017` write is
three cycles on. Visual 2A03 has the sweep and the length counter changed as the second cycle
after a write in a put cycle begins, and as the third after one in a get cycle, which is the
"APU" page's figure. A reset and power-on, whose restart is pending with no write, make no
such clock; the same simulation shows none at a reset in the 5-step sequence.

*Settled by* the reading.

### The flag is set on three consecutive cycles, and a read in the second or third does not keep it clear

Documented and corroborated; one rule covers it.

The 4-step sequence sets the frame interrupt flag in each of its last three cycles. A read of
`$4015` precedes the tick of its own cycle, so a read in the first of the three answers 0, a
read in the second or third answers 1 and clears the flag, and that cycle's tick sets it
again.

The "APU Frame Counter" page has three "Set if interrupt inhibit flag (I bit) is clear" rows.
"APU", Status: "If an interrupt flag was set at the same moment of the read, it will read back
as 1 but it will not be cleared." blargg's 2005 readme, Misc: "the frame irq flag is set three
times in a row. Thus when polling it, always read $4015 an extra time after the flag is found
to be set". The chip has no rule for the "same moment": the order of the access and the tick
gives the page's sentence.

*Corroborated by* the 2005 set's `07.irq_flag_timing` and `apu_test`'s `6-irq_flag_timing`.

### The IRQ line follows the flag by one cycle

The documents are silent on the line; programs settle it.

The chip's IRQ output takes a flag one cycle after the tick that set it, and the CPU's level
detector takes the output at the end of that cycle.

"CPU interrupts", Detailed interrupt behavior, has the level detector sample the line during
the second half of a cycle and raise its signal the cycle after; no APU page gives the cycle
the flag reaches the pin. blargg's 2005 readme, Misc: "The IRQ handler is invoked at minimum
29833 clocks after writing $00 to $4017". With the output driven in the flag's own tick the
2005 set's `08.irq_timing` reports its code 2, the handler invoked too soon, and
`cpu_interrupts_v2`'s `3-nmi_and_irq`, `4-irq_and_dma` and `5-branch_delays_irq` fail, each
one cycle early; with the output a cycle behind, all four pass under both halves. On the chip
the output is `irqLine`, sampled at the top of each tick from the flags as the cycle's access
left them.

*Settled by* those four programs.

### Power-on and reset start the sequence as if $4017 were written nine cycles before the first instruction

Documented as a range; the chip takes the usual figure.

At power-on the sequence's origin is the tick of the first machine cycle, where a write of
`$00` to `$4017` nine cycles before the first instruction would put it. A reset does the same
with the mode last written.

blargg's `apu_reset` readme, `4017_timing`: "At power, it is as if $00 were written to $4017,
then a 9-12 clock delay, then execution from address in reset vector. At reset, same as above,
except last value written to $4017 is written again, rather than $00. … Delay after NES being
powered off for a minute is usually 9." His 2005 readme, Misc: "After reset or power-up, APU
acts as if $4017 were written with $00 from 9 to 12 clocks before first instruction begins."
The CPU's reset sequence is the machine's first seven cycles, and a write whose origin is
cycle 0 lands two cycles before it. Every delay from 9 to 12 passes the programs that measure
it (`4017_timing`, which prints the delay it finds, `4017_written` and the 2005 set's
`09.reset_timing`); the chip is built with 9 and `4017_timing` prints 9. The spread is a
console's and is not a config field. The readme's `4017_written` adds that at reset the "IRQ
inhibit flag is sometimes cleared"; the chip keeps it ([open](#what-remains-open)).

---

## The length counters

### A halt bit takes effect after the clock of its own cycle

Measured; the "APU Length Counter" page does not state the delay.

A halt bit written in the cycle of a half-frame clock does not hold that clock, and one
written the cycle before does; clearing it works the same way.

blargg's 2005 readme, Length Halt: "Write to halt flag is delayed by one clock", with its four
timed cases: a halt set at 14914 holds the clock at 14915; set at 14915, the length is
clocked; cleared at 14914, clocked; cleared at 14915, not clocked. On the chip the clock
consults `haltLatched`, the bit as the previous tick left it. The envelope's loop flag and the
linear counter's control flag are the same bit read as written: the measurement is of the
length counter alone.

*Corroborated by* the 2005 set's `10.len_halt_timing`.

### A load written in the cycle of a clock is dropped when the clock counts

Measured.

A write to a fourth register in the cycle of a half-frame clock loads the counter only when
the clock did not count it down: with the counter above 0 and not halted the load is dropped,
and with the counter at 0 it lands and is not clocked.

blargg's 2005 readme, Length Reload: "Length reload is completely ignored if written during
length clocking and length counter is non-zero before clocking", with its three timed cases.
On the chip the load is `reloadPending`, applied by the cycle's tick after its clocks. One
case the readme does not time is the chip's choice: with the counter halted in the clock's
cycle the load lands, the halt having suspended the clocking the rule names
([open](#what-remains-open)).

*Corroborated by* the 2005 set's `11.len_reload_timing`.

### The 2A03's counter holds one less than the table's value

Documented and corroborated; the two models are equal.

`ApuLengthCounter::counter` holds the "APU Length Counter" page's table value and silences
its channel at 0. The 2A03's own counter holds one less and silences the channel when it is
clocked at 0.

"APU Length Counter", Length counter internals: "The values in the tables presented here are
the actual values the length counter gets loaded with plus one, to allow us to use a model
where the channel is silenced when the length counter becomes zero." Visual 2A03 has the
counter's bits read 253 after a load of 254 and wrap from 0 to 255 as the channel goes
silent. Every status bit and every silence falls on the same cycle under either model.

---

## The channels

### The pulses, the noise and the DMC step as a put cycle begins

The documents are silent on the half; the simulation settles it.

On the 2A03 the pulses' timers and sequencers, the noise's shift register and the DMC's
output unit change in the first half of a put cycle. The chip counts those timers in the
tick of the get cycle before, so each change is in place when the put cycle's access is
made.

"APU", Glossary, has the triangle's timer clocked on every CPU cycle and the pulses', the
noise's and the DMC's on every second one; "APU Pulse" says once an APU cycle; neither names
the half. Visual 2A03 has a pulse's timer written 8 in a put cycle read 8 two cycles on, then
7 two cycles after, each change in the first half of a put cycle, with the sequencer stepping
in the same half; the noise's shift register and the DMC's shift register and bit counter
change in that half too; the triangle's timer changes in the second half of every cycle. The
half is audible in what cancels: blargg's `apu_mixer` programs leave different residues under
the two halves the machine can be built in
([apu.md](apu.md#testing-against-the-programs)).

*Settled by* the reading.

### Inside a cycle the frame counter's clocks come before the timers

The documents are silent; the simulation settles it.

In one tick the quarter- and half-frame clocks run first and the timers after them, so a
linear counter a clock loads lets the triangle's sequencer step in the same tick.

The pages order the units inside a clock, as the "APU Triangle" page's two actions of a
quarter-frame clock and the "APU Pulse" page's two steps of a half-frame clock, and none
orders a clock against a timer. Visual 2A03 has the frame counter's clocks land in the first
half of a put cycle and the triangle's timer change in the second half of every cycle.

*Settled by* the reading.

### A sweep rewrites the period only while the length counter is above 0

Contested, two documents against one; the simulation settles it.

A sweep's half-frame clock sets the period to the target only while the channel's length
counter is above 0, as the clock finds it. Its divider counts whatever the counter holds.

2A03 technical reference, Sweep unit: the register "will be updated with the New value, but
only if all 3 of these conditions are met: bit 7 is set (sweeping enabled); the shift value
(which is N in the formula) does not equal to 0; the channel's length counter contains a
non-zero value". "APU Pulse", Sweep unit operation, lists the divider at 0, the sweep enabled,
the shift nonzero and the channel not muted; blargg's NES APU reference, Sweep Unit, lists the
sweep enabled and the shift above 0; neither names the length counter. Visual 2A03, with a
period of `$100`, a shift of 1 and a half-frame clock made by each `$4017` write: the period
goes `$180`, `$240`, `$360`, `$510` with the counter loaded, and stays `$100` with the channel
disabled; with a counter loaded for two clocks it goes `$180` and `$240`, the second in the
clock that empties the counter, and stays `$240` through five more; the divider counts 2, 1,
0, 2 either way. So the half-frame clock runs the sweeps before the length counters.

*Settled by* the reading, for the technical reference.

### The noise's shift register holds 1 at power-on

Contested between two wiki pages; no program can tell.

The register is 1 at power-on. Its first clock, on the first get cycle, leaves `$4000`.

"APU Noise", Operation: "On power-up, the shift register is loaded with the value 1. Reset
does not affect shift register." blargg's NES APU reference says the same of power-up. "CPU
power up state" gives the register `$0000` at power-on, "all 0s, first clock shifts in a 1".
From 1 the feedback is 1 and the register becomes `$4000`; from `$0000` with a 1 shifted in it
is `$4000`. The first clock comes before any instruction, and the channel is disabled until a
program enables it, so the two are one chip to every program. The chip takes the channel's
own page.

### The triangle stands at its first step at power-on

One document marks it unknown; a program's source states it.

At power-on the triangle's sequencer is at its first step and its level is 15, so the
mixer's level is 8074 before a program writes a register.

"CPU power up state" marks the triangle's place in its sequence at power-on with a question
mark and gives 0, "output = 15", after reset. The shell of blargg's `apu_mixer` programs: "at
power, it starts with its DAC at 15", and its readme has the tests run "from a
freshly-powered NES, as this is the only way to ensure that the triangle wave doesn't
interfere". The chip takes the shell's statement as a constant, not a config field: the
page's mark is an unknown, not a spread a console is measured to have.

---

## The DMC

### A sample's first fetch halts the CPU on the third or fourth cycle after the write

Documented and corroborated twice.

The DMA unit attempts the halt for a sample's first byte on the get cycle of the second APU
cycle after the `$4015` write: the third cycle after a write on a put, the fourth after one
on a get. Every later byte's halt is attempted in the cycle after the output unit empties
the buffer, a put.

The "DMA" page, DMC DMA, gives the rule and its examples. Visual 2A03 has RDY fall in the
third cycle after a write in a put cycle, with the read two cycles on, and in the fourth after
one in a get cycle; the reload's RDY falls in the put cycle in which the shift register loads.
Fiskbit's `dmc_dma_start_test`, in both versions, measures the same on an RP2A03H, and each of
its 21 cases gives its readme's figure on the machine, by the half of the write.

*Corroborated by* the reading and those cases.

### The end of a sample reaches $4015 at the fetch

Contested between the simulation and four programs; the programs' figure is taken, and a
console run would confirm it.

The fetch of a sample's last byte counts the bytes remaining to 0 and sets the interrupt
flag in its own cycle: a read of `$4015` in any later cycle answers bit 4 clear and, with
the interrupt enabled, bit 7 set.

No document gives the latency. Visual 2A03 gives three cycles: the bytes remaining, the active
bit and the interrupt flag change in the second put cycle after the fetch's read, and the
core's data bus carries the old bits at a `$4015` read two cycles after the fetch and the new
ones at the next. Four of blargg's programs need the new bits sooner: `apu_test`'s
`7-dmc_basics`, whose last test ("There should be a one-byte buffer that's filled immediately
if empty") reads `$4015` two cycles after a fetch and requires bit 7 set and bit 4 clear; and
`dma_2007_read` and both `sprdma_and_dmc_dma` builds, whose synchronization to the DMC waits on
bit 4 clearing. With the bits changed at the fetch all four pass; with them changed three
cycles later all four fail. The chip takes the programs' figure and records the reading
beside it. `7-dmc_basics` run on a console decides: a pass is this figure, and a failure at
its test 19 is the simulation's.

*Taken from* the four programs; *open* until that run.

### A sample stopped just before its reload still halts the CPU for one cycle

Documented; half of it is built.

A sample stopped in the two cycles before the output unit empties the buffer still asks for
a fetch. The DMA unit halts the CPU, finds the request gone and releases it after that one
cycle; a write in the way of the halt brings no halted cycle at all.

"DMA", Bugs: "When sample playback is stopped during the APU cycle before a reload DMA would
schedule (that is, on the 2nd or 3rd CPU cycle before the halt attempt), the DMA starts, but
is aborted after a single cycle. If the halt is delayed due to a write cycle, the aborted DMA
doesn't occur at all." On the chip this is `stopAge`, the ticks since the sample stopped, and
the request's lapse at the next tick. The page's second bug is not built: "On RP2A03H and late
RP2A03G CPUs, when playback is stopped implicitly on the same APU cycle that a reload DMA
would schedule …, an unexpected reload DMA occurs from the same address." Fiskbit's readme
reports "1 or 4 cycle blips" beside its solid figures, read here as this aborted fetch and
that reload ([open](#what-remains-open)).

### A read the fetch lands on is repeated on every halted cycle

Documented and corroborated, with one part the PPU's.

On the halt, dummy and alignment cycles of a fetch the halted CPU repeats its read, and the
read reaches the bus: a register with a read side effect sees it each time.

The "DMA" page gives the rule under Register conflicts. Each program of `dmc_dma_during_read4`
states its result at the head of its source. `dma_4016_read`, "DMC DMA during $4016 read
causes extra $4016 read", prints `08 08 07 08 08` and passes; `dma_2007_write` and
`read_write_2007` pass; `dma_2007_read`, "DMC DMA during $2007 read causes 2-3 extra $2007
reads before real read", accepts the CRCs `159A7A8F` and `5E3DF9C4` by alignment, and the
machine makes three extra reads under every alignment and both halves and prints `5E3DF9C4`.
The two-read result belongs to what the PPU's port does with reads on adjacent cycles, which
`double_2007_read` measures with no DMA at all and which is the PPU's to build
([open](#what-remains-open)).

---

## The output

### The mixer is the page's formula, where its lookup tables leave a tone

Contested inside one page; programs settle it.

`apuMix` evaluates the "APU Mixer" page's two formulas in whole numbers, each curve scaled by
32 767 and rounded. It carries no lookup table.

"APU Mixer" gives the formula as "the approximate audio output level", then "Faster but less
accurate approximations are also possible: using an efficient lookup table", whose second
table "is approximated (within 4%) by using a base unit close to the DMC's DAC". blargg's
`apu_mixer` programs cancel each channel against the DMC's level and are written to the
formula: their readme has "near silence between the beeps". On the formula the four leave a
tone of 7 to 13 thousandths of a beep's step between their beeps; on the table form
`triangle` leaves 63.

*Settled by* `apu_mixer`.

### The output stage is the page's two filters at the page's corners

Documented; the circuit the page links computes differently, and that is recorded.

The stage is a first-order high-pass at 90 Hz followed by a first-order low-pass at 14 kHz,
run once a CPU cycle. Each filter moves a fixed part of the way to the level it is given,
5300 and 804 639 parts in 16 777 216, which stand for corners of 90.00002 Hz and
13 999.997 Hz.

"APU Mixer": "The NES hardware follows the DACs with a surprisingly involved circuit … that
adds several low-pass and high-pass filters", with three items: "A first-order high-pass
filter at 90 Hz", "A first-order low-pass filter at 14 kHz" and "A second-order low-pass
comfortably above the audible range and thus not needed for emulation".

The circuit the page links ("NES Audio Path", archive.nesdev.org/NESAudio.gif) draws the third
as 39 µH into 0.011 µF, a corner near 243 kHz, behind an amplifier made of one inverter of a
74HCU04, and the chip omits it. Worked from its component values, the drawing gives other
corners than the page: with an ideal amplifier the feedback pair, 47 kΩ and 220 pF, is a
low-pass at 15.4 kHz and the input's 1 µF behind 7.5 kΩ a high-pass at 21 Hz; at the
open-loop gain of 20 the 74HCU04's data sheet gives as typical (Nexperia, Rev. 10,
"Application information") they are 17 kHz and 16 Hz; and the output's 10 µF behind 150 Ω
reaches 90 Hz only into a load of 27 Ω. The chip takes the page's figures.

The page's corners are the ones built; a console's own corners are
[open](#what-remains-open).

### The "APU Mixer" page listed a third filter at 440 Hz until October 2026

A document's revision; the chip has no such filter.

The chip has no high-pass at 440 Hz. A reader matching it against an implementation written
from the page's earlier text meets one there.

The page's revision of 19 January 2026 lists three filters for the NES: "A first-order
high-pass filter at 90 Hz", "Another first-order high-pass filter at 440 Hz" and "A
first-order low-pass filter at 14 kHz". Its revision of 27 October 2026 removes the second
and adds the second-order low-pass line, with the edit summary "the 440hz filter is wrong,
and I don't know why blargg's data showed it. Enough people have implemented it and observed
it sounds wrong that we should stop claiming it" (the "APU Mixer" page's revision history).
The two figures that stand are the page's statement as it reads now.

---

## What remains open

Stated plainly: what the chip does at each point below is fixed and tested, and no source
here says it is what a console does.

- **How long the old sequence runs after a `$4017` write.** blargg's 2005 readme: "Not
  documented here is a delay when changing modes by writing to $4017. … This delay when
  changing modes involves the current mode running a few clocks before switching to the new
  mode". The chip steps the old sequence until the restart, two or three cycles after the
  write, and a step it reaches in those cycles runs. No program measures a write that close
  to a step.
- **The DMC's status at a sample's end.**
  [The case above](#the-end-of-a-sample-reaches-4015-at-the-fetch) takes four programs'
  figure over the simulation's. `7-dmc_basics` run on a console settles it.
- **The RP2A03H's reload, and a stop in the reload's own cycle.** The "DMA" page gives the
  RP2A03H and the late RP2A03G an unexpected reload when a sample ends implicitly in the APU
  cycle its reload would schedule in; the chip has no revision to select and fetches nothing
  there. The page is silent on an explicit stop in that cycle, and the chip fetches nothing
  there either.
- **A `$4011` write that meets the output unit's clock.** The "APU DMC" page has the level
  "occasionally not changed properly", and blargg's reference has a write "while a sample is
  playing sometimes has no effect". The chip loads the level at the write.
- **The DMC's timer at power-on.** On the 2A03 it is a shift register in an arbitrary state,
  and the simulation's first clock comes about a thousand cycles in. The chip's timer starts
  at 0 with the output unit eight clocks from its first byte, so its first clock is on the
  first get cycle.
- **A load written while the counter is halted, in the cycle of a clock.** blargg's rule
  names a load "written during length clocking"; with the halt bit holding the clock off, the
  chip lets the load land. His readme times no such case.
- **The chip across a reset.** "CPU power up state" marks the pulses', the triangle's and the
  noise's registers "unchanged?" after reset, and blargg's `apu_reset` readme has the IRQ
  inhibit flag "sometimes cleared". The chip keeps all of them. Visual 2A03 has a reset clear
  an envelope's decay level and leave the envelope restarting; the chip keeps the level. A
  note restarts its envelope before it sounds, so no output depends on it.
- **The earliest chips.** "APU Noise": "In the earliest revisions of the 2A03 CPU, the
  periodic noise flag (M bit) was nonexistent", and the last period was 2046 cycles. The chip
  is the later one.
- **The mixer's own error.** The page calls its formula approximate and gives no bound. The
  chip is exact to the formula.
- **The output stage's corners.** The page's 90 Hz and 14 kHz are built; the linked circuit,
  worked from its component values, gives a high-pass at 16 to 21 Hz and a low-pass at 15 to
  17 kHz. A console settles it: the level a `$4011` write steps falls to 1 / e of itself in
  1.77 ms through a high-pass at 90 Hz and in about 10 ms through one at 16 Hz. A recording
  that measures it notes what the audio output was plugged into, since the circuit's last
  capacitor works against that load.
- **The stage's third filter.** The page's second-order low-pass, which the circuit draws as
  39 µH into 0.011 µF, a corner near 243 kHz, is omitted, as the page itself sets it aside.
- **The stage's gain, its sense and its charge at power-on.** The circuit's amplifier inverts
  and the drawing does not give the sense of the DACs' pins, so the output keeps the
  formula's sense and rises with the level; no document turns the stage's gain into a
  sample's scale, and the chip applies none. No document gives the charge the stage's
  capacitors hold at power-on; the chip starts the stage settled on the level the DACs start
  at, so its output opens at 0.
- **The Famicom's stage.** "APU Mixer": "The Famicom hardware instead ONLY specifies a
  first-order high-pass filter at 37 Hz, followed by the unknown (and varying) properties of
  the RF modulator and demodulator." The chip's stage is the NES's.
- **The decimator.** The mean over a sample's CPU cycles is a choice of this implementation
  and no part of the console; it lets through, attenuated, what lies above half the rate.
- **The triangle at its two shortest periods.** blargg's reference: "the resulting frequency
  is so high that the DAC effectively outputs a value half way between 7 and 8." The chip
  steps the sequencer at those periods as at any other. No case measures the result.
- **`2-nmi_and_brk` and the alignment.** The readme of `cpu_interrupts_v2` says the program
  "occasionally fails on NES due to PPU-CPU synchronization"; on the machine it passes under
  alignment 0 and fails under 1, 2 and 3. Which alignments fail on a console is not stated.
- **Reads of `$2007` on adjacent cycles.** `double_2007_read` and the two-read result of
  `dma_2007_read` turn on what the PPU's port does there, which the PPU does not build; the
  machine makes each read whole.
- **`volume_tests`.** Damian Yerrick's program carries a recording of an NTSC NES-001 playing
  its twelve tones. Its readme states no level for them, and no case compares against the
  recording.

## How a contested claim gets settled here

In three steps, in this order.

1. **Read every document that speaks to the point, and set down what each says in its own
   words.** Most disagreements end here: two pages counting one cycle from two places, as
   with the `$4017` write, or a page labelling the half in which an effect is in place where
   the chip counts the tick before it.
2. **Run the programs that measure the point under each reading.** A reading under which a
   program fails is set aside, and the pair is recorded. The programs run under both halves
   of the APU cycle, and under each CPU-PPU alignment where a readme says a result turns on
   it.
3. **Where the documents are silent and no program tells two readings apart, read the
   simulation.** A short program runs on Visual 2A03 with the nodes of interest traced cycle
   by cycle, and the reading is cited as a measurement of the die's netlist, with the write's
   cycle, the half and the cycle of each change.

A program is never the rule: the rule is a document's, and the program picks between
documents. Where a reading and programs written against a console disagree, as on the DMC's
status, the programs' figure is taken and both are recorded. What none of the three settles
stays on the open list with what would: for the output stage a recording of a console, for
the DMC's status one program's result on one.
