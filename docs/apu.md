# The APU

The 2A03's audio processing unit is the chip a program reaches at `$4000`–`$4017`: two pulse
channels, a triangle, a noise channel and a delta modulation channel, a frame counter that
clocks their envelopes, sweeps and counters and raises an interrupt, and a status register
over them all. This is that chip over a state value, a CPU cycle at a time: every register's
write, the status read, the frame counter's two sequences, each channel down to the level its
DAC takes, the DMC's sample fetch and its interrupt, the mixer that makes one level of the
five, the two filters the console follows its DACs with, and the decimator that averages
their output down to the samples a host drains.

The chip's state is one value inside [the machine's](nes-machine.md), `state().apu`; the
machine ticks it once a CPU cycle and queues its samples as frames. The machine is the NTSC
NES: the noise's and the DMC's tables are the NTSC columns, the output stage is the NES's,
and the level a cartridge's sound hardware adds is 0 on every board built. Where the
published documentation is silent, or two sources disagree,
[APU behavior and provenance](apu-behavior.md) records what the chip does and what settles
each case.

The status table in the [README](../README.md) is the component's status row.

## Contents

- [The surface](#the-surface)
- [The state value](#the-state-value)
- [A cycle](#a-cycle)
  - [A bare chip](#a-bare-chip)
- [The registers](#the-registers)
- [The frame counter](#the-frame-counter)
  - [A write to $4017](#a-write-to-4017)
  - [The frame interrupt](#the-frame-interrupt)
  - [The IRQ line](#the-irq-line)
- [The status register](#the-status-register)
- [The length counters](#the-length-counters)
- [The envelopes](#the-envelopes)
- [The pulse channels](#the-pulse-channels)
  - [The sweeps](#the-sweeps)
- [The triangle](#the-triangle)
- [The noise](#the-noise)
- [The DMC](#the-dmc)
  - [The sample fetch](#the-sample-fetch)
- [The output](#the-output)
  - [The mixer](#the-mixer)
  - [The output stage](#the-output-stage)
  - [The decimator](#the-decimator)
- [Power-on and reset](#power-on-and-reset)
- [Testing against the programs](#testing-against-the-programs)
- [Gotchas](#gotchas)
- [What remains open](#what-remains-open)
- [See also](#see-also)
- [Where to look](#where-to-look)
- [Sources](#sources)

## The surface

The chip's state is one value, `ApuState`, held inside the machine's state as `NesState::apu`;
a snapshot of the machine carries it whole and a restore puts it back. The chip's behavior is
eleven functions over that state:

| Function | What it is |
|---|---|
| `apuWrite` | a CPU write to a register ([The registers](#the-registers)) |
| `apuReadStatus`, `apuPeekStatus` | a CPU read of `$4015`, and its answer with nothing cleared ([The status register](#the-status-register)) |
| `apuTickCycle` | one CPU cycle of the chip ([A cycle](#a-cycle)) |
| `apuIrqAsserted` | the chip's IRQ output ([The IRQ line](#the-irq-line)) |
| `apuDmcWantsFetch`, `apuDmcDeliver` | the DMC's request for a sample byte, and the byte's delivery ([The sample fetch](#the-sample-fetch)) |
| `apuMix` | the level the five DACs make together ([The mixer](#the-mixer)) |
| `apuFilter` | one CPU cycle of the console's output stage ([The output stage](#the-output-stage)) |
| `apuDecimate` | one CPU cycle of the output, answering a sample when one completes ([The decimator](#the-decimator)) |
| `apuReset` | the reset line ([Power-on and reset](#power-on-and-reset)) |

The machine calls them as its cycles run, and a host that runs the machine reads the state and
drains the frames ([the machine page](nes-machine.md#the-apu)). What the chip's output reaches
and does not hold is handed in as `ApuInputs`: the rate from the machine's config, and the
level the cartridge adds, 0 on every board built.

```cpp
// What the chip's output reaches and does not hold: the rate its level is decimated to,
// in samples a second, from the machine's config, 0 making no samples; and the level the
// cartridge's sound hardware adds to the chip's, in a sample's units, 0 for a cartridge
// with none.
struct ApuInputs {
  std::uint32_t audioRate = 0;
  std::int16_t expansion = 0;
};
```

The examples on this page build their images with one helper, whose IRQ vector points at a
handler of its own:

```cpp
#include "neasant/apu/apu.h"
#include "neasant/nes/nes.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

// A 16 KB NROM image with 8 KB of CHR ROM: `program` at $8000 with the reset and NMI vectors
// pointing there, and `handler` at $9000 with the IRQ vector pointing there, an RTI unless
// one is given.
std::vector<std::uint8_t> apuImage(std::initializer_list<std::uint8_t> program,
                                   std::initializer_list<std::uint8_t> handler = {0x40}) {
  std::vector<std::uint8_t> image{0x4E, 0x45, 0x53, 0x1A, 1, 1};
  image.resize(16 + 16384 + 8192);
  std::size_t at = 16;
  for (const std::uint8_t byte : program) image[at++] = byte;
  at = 16 + 0x1000;
  for (const std::uint8_t byte : handler) image[at++] = byte;
  image[16 + 16384 - 6] = 0x00;  // the NMI vector: $8000
  image[16 + 16384 - 5] = 0x80;
  image[16 + 16384 - 4] = 0x00;  // the reset vector: $8000
  image[16 + 16384 - 3] = 0x80;
  image[16 + 16384 - 2] = 0x00;  // the IRQ vector: $9000
  image[16 + 16384 - 1] = 0x90;
  return image;
}
```

Every later example on this page uses this `apuImage` and needs no further `#include`.

## The state value

`ApuState` is the chip's whole mutable state as a value: snapshot by copy, restore by
assignment.

```cpp
// The chip's whole mutable state as a value: snapshot by copy, restore by assignment. The
// defaults are the power-on state (NESdev wiki, "CPU power up state", APU): every
// register 0 and every channel disabled with its counter at 0, the triangle at its first
// step, the noise's shift register 1, the DMC silent with its level 0, mode 0 with the
// interrupt enabled, the flag clear, and the sequence restarting at the first machine
// cycle's tick, where a write of $00 to $4017 nine cycles before the first instruction
// would put its origin, the figure blargg's apu_reset programs measure as usual; the
// output stage settled on the level the DACs start at, and the output's first sample not
// yet begun.
struct ApuState {
  // Pulse 1, pulse 2, the triangle and the noise, in $4015's bit order.
  std::array<ApuLengthCounter, 4> length{};
  // Pulse 1 and pulse 2.
  std::array<ApuPulse, 2> pulse{};
  ApuTriangle triangle{};
  ApuNoise noise{};
  ApuDmc dmc{};
  // The mode the sequencer runs in: false the 4-step sequence, true the 5-step (NESdev
  // wiki, "APU Frame Counter").
  bool fiveStep = false;
  // $4017 bit 7 as last written, which the next restart makes the mode.
  bool pendingFiveStep = false;
  // $4017 bit 6 as last written: set, the frame interrupt flag is cleared and never set.
  bool inhibit = false;
  // The frame interrupt flag, bit 6 of $4015: set by the three consecutive ticks that end
  // the 4-step sequence, cleared by a read of $4015 and by a write to $4017 with bit 6 set
  // (NESdev wiki, "APU Frame Counter"; "APU", Status).
  bool frameInterrupt = false;
  // The IRQ line as the CPU samples it: the frame interrupt flag with the inhibit clear,
  // or the DMC's interrupt flag, as they stood when the current tick began. A set reaches
  // the line one cycle after a read of $4015 sees it (NESdev wiki, "CPU interrupts",
  // Detailed interrupt behavior: the level detector samples the line during the second
  // half of a cycle and raises its signal the cycle after).
  bool irqLine = false;
  // CPU cycles since the sequence's origin, as the last tick left it: the steps fall at
  // 7457, 14913, 22371 and 29829 or 37281, and the count returns to 0 at 29830 (4-step)
  // or 37282 (5-step), the APU-cycle counts of the frame counter's table doubled, the
  // quarter- and half-frame steps one cycle on (NESdev wiki, "APU Frame Counter").
  std::uint16_t frameCycle = 0;
  // Ticks until the sequence restarts, 0 when none is due: a write to $4017 sets 3 on a
  // put cycle and 4 on a get, so the origin is two or three cycles after the write, and
  // the restart takes the pending mode. A write with bit 7 set clocks the quarter- and
  // half-frame units on the tick before the restart (NESdev wiki, "APU", Frame Counter;
  // "APU Frame Counter", Control register; Visual 2A03). Until the restart the counter
  // keeps stepping in its old mode.
  std::uint8_t restartDelay = 1;
  // The console's output stage. Reset leaves it as it is.
  ApuFilter filter{};
  // The output's place in its current sample. Reset leaves it as it is.
  ApuDecimator decimator{};
  [[nodiscard]] bool operator==(const ApuState&) const noexcept = default;
};
```

`length` is covered under [The length counters](#the-length-counters); `pulse`, `triangle`,
`noise` and `dmc` under their channels; the six frame-counter fields, `fiveStep`,
`pendingFiveStep`, `inhibit`, `frameInterrupt`, `frameCycle` and `restartDelay`, with
`irqLine`, under [The frame counter](#the-frame-counter); and `filter` and `decimator` under
[The output](#the-output). Every field is plain and every struct compares with `==`, so a
snapshot taken between two cycles restores to the same place.

## A cycle

An APU cycle is two CPU cycles, a get half and a put half, alternating (NESdev wiki, "DMA",
Cadence). The chip is ticked once a CPU cycle and told which half the cycle is: the machine
holds the half as `NesState::apuPhase` and seeds it from the config's `apuPhase`, 0 a get. A
CPU write lands in the cycle's bus access and the cycle's tick follows it, so a write is seen
by its own cycle's tick, and a read of `$4015` answers what the previous cycle's tick left.

```cpp
// One CPU cycle, in the half of the APU cycle `apuPhase` names, 0 a get and 1 a put:
// samples the IRQ line from the two interrupt flags, lets a DMC fetch request lapse once
// its bytes remaining are 0, counts the restart delay down and restarts the sequence
// when it reaches 0, otherwise steps the counter and runs the step that falls on it, the
// quarter-frame clock on the envelopes and the linear counter and the half-frame clock
// on the sweeps and the length counters; applies each channel's pending load and latches
// its halt bit; counts the triangle's timer, and on a get cycle the pulses', the noise's
// and the DMC's, so that their sequencer and shift register change as a put cycle
// begins (Visual 2A03); and leaves each channel's output as the tick's end finds it.
void apuTickCycle(ApuState& state, std::uint8_t apuPhase) noexcept;
```

Inside one tick the order is fixed:

1. The IRQ line takes the two interrupt flags as the cycle's access left them.
2. A DMC fetch request whose bytes remaining are 0 lapses.
3. A pending restart counts down. One cycle before a restart that `$4017` bit 7 asked for,
   the quarter- and half-frame clocks run; at the restart the count returns to 0 in the mode
   last written. With no restart in the tick the count steps.
4. The step the count stands on runs: the quarter-frame clock on the envelopes and the
   linear counter, then the half-frame clock on the sweeps and after them the length
   counters; and the last three cycles of the 4-step sequence set the frame interrupt flag.
5. Each length counter takes a load written this cycle, unless the tick's half-frame clock
   counted it down, and latches its halt bit for the next tick's clock.
6. The triangle's timer counts.
7. On a get cycle the two pulses' timers, the noise's and the DMC's count, so their
   sequencers, shift register and output unit change as a put cycle begins.
8. Each channel's `output` is left as the tick's end finds it.

(NESdev wiki, "APU", Glossary; "APU Frame Counter"; blargg's 2005 frame counter readme,
Length Halt and Length Reload; Visual 2A03.) A counter that a clock loads or empties gates
the same tick's timer, and the state a host reads between two cycles is coherent: every
level in it is the one the mixer is given.

### A bare chip

The functions take the state and the half, and the examples drive a chip as the machine
does, through one struct:

```cpp
// A chip driven as the machine drives it: a write lands in the cycle under way, the cycle's
// tick follows it, and the half of the APU cycle then alternates, the first cycle a get.
struct Chip {
  neasant::ApuState state;
  std::uint8_t half = 0;  // 0 a get, 1 a put
  int cycle = 0;          // the cycle under way, counted from the chip's first

  void write(std::uint16_t address, std::uint8_t value) {
    neasant::apuWrite(state, address, value, half);
  }
  void tick(int count = 1) {
    for (int n = 0; n < count; ++n) {
      neasant::apuTickCycle(state, half);
      half = half == 0 ? 1 : 0;
      ++cycle;
    }
  }
  // Ticks until `target` is the cycle under way.
  void runTo(int target) { tick(target - cycle); }
};
```

Each example below builds a fresh `Chip`. A cycle is counted from the chip's first, a get, so
a cycle number here is the count `frameCycle` reads after that cycle's tick, while no write
has restarted the sequence.

```cpp
Chip chip;
chip.write(0x4015, 0x01);  // cycle 0: enable pulse 1
chip.tick();
chip.write(0x4003, 0x18);  // cycle 1: load its length counter from entry 3 of the table
// chip.state.length[0].counter == 0: the load waits for the cycle's tick
chip.tick();
// chip.state.length[0].counter == 2
// neasant::apuPeekStatus(chip.state, 0x00) == 0x01: pulse 1's counter is above 0
```

## The registers

```cpp
// A CPU write to `address`, one of $4000-$4013, $4015 or $4017, in a cycle whose APU half
// is `apuPhase`, 0 a get and 1 a put. $4000, $4004 and $400C take their channel's halt
// bit, constant-volume bit and volume, and $4000 and $4004 their pulse's duty; $4001 and
// $4005 the sweep's fields, setting its reload flag; $4002, $4006 and $400A the low eight
// bits of a timer's period; $4003, $4007 and $400B the high three, and $400E the noise's
// mode and period index. $4010 takes the DMC's interrupt enable, loop flag and rate,
// clearing the interrupt flag when the enable is written clear; $4011 its level; $4012
// and $4013 its sample address and length. $4008 takes the triangle's halt bit and the
// linear counter's reload value. A write to a fourth register, $4003, $4007, $400B or
// $400F, also loads the channel's length counter from the table at the cycle's tick while
// the channel is enabled, and whether or not it is enabled restarts a pulse's sequencer
// and its envelope, sets the triangle's linear reload flag, or restarts the noise's
// envelope. $4015 sets the four enables, clearing the counter of each channel it
// disables; its bit 4 clear stops the DMC's sample and set restarts one whose bytes
// remaining are 0, asking for the first byte at once when the buffer is empty, and the
// write clears the DMC's interrupt flag; $4017 sets the inhibit bit, clearing the flag
// when it is set, and schedules the sequence's restart two cycles on from a put cycle and
// three from a get. Every other address is dropped.
void apuWrite(ApuState& state, std::uint16_t address, std::uint8_t value,
              std::uint8_t apuPhase) noexcept;
```

| Address | A write |
|---|---|
| `$4000`, `$4004` | pulse 1's and pulse 2's: bits 7–6 the duty, bit 5 the length counter's halt bit and the envelope's loop flag, bit 4 constant volume, bits 3–0 the volume and the envelope's divider period |
| `$4001`, `$4005` | the sweep: bit 7 enabled, bits 6–4 the divider period, bit 3 negate, bits 2–0 the shift; the write sets the sweep's reload flag |
| `$4002`, `$4006` | the low eight bits of the timer's period |
| `$4003`, `$4007` | bits 2–0 the period's high three bits, bits 7–3 the length counter's load; the write returns the sequencer to its first step and sets the envelope's start flag |
| `$4008` | the triangle: bit 7 the length counter's halt bit and the linear counter's control flag, bits 6–0 the linear counter's reload value |
| `$400A` | the low eight bits of the triangle's period |
| `$400B` | bits 2–0 the period's high three bits, bits 7–3 the length counter's load; the write sets the linear counter's reload flag |
| `$400C` | the noise: bit 5 the halt bit and the loop flag, bit 4 constant volume, bits 3–0 the volume and the divider period |
| `$400E` | bit 7 the mode, bits 3–0 the entry of the period table |
| `$400F` | bits 7–3 the length counter's load; the write sets the envelope's start flag |
| `$4010` | the DMC: bit 7 the interrupt enable, bit 6 the loop flag, bits 3–0 the entry of the rate table; a write with bit 7 clear clears the interrupt flag |
| `$4011` | bits 6–0 the DMC's level |
| `$4012` | the sample's address, `$C000` plus 64 times the byte |
| `$4013` | the sample's length, 16 times the byte plus 1 |
| `$4015` | bits 3–0 the enables of the noise, the triangle, pulse 2 and pulse 1, bit 4 the DMC's sample ([The status register](#the-status-register)) |
| `$4017` | bit 7 the sequence, bit 6 the interrupt inhibit; the write restarts the sequence ([A write to $4017](#a-write-to-4017)) |
| `$4009`, `$400D` | dropped |

(NESdev wiki, "APU", Registers.) A length counter's load waits on its channel's enable bit
and on the cycle's tick; every other effect in the table is immediate and waits on no enable
bit. `$4015` is the one register a program reads; on the machine a read of any other answers
the open bus ([the machine page](nes-machine.md#open-bus)).

## The frame counter

The frame counter runs the 4-step sequence or the 5-step one by `$4017` bit 7, counting CPU
cycles from its origin.

| Step | 4-step sequence, at cycle | 5-step sequence, at cycle | Clocks |
|---|---|---|---|
| 1 | 7457 | 7457 | the quarter frame's |
| 2 | 14 913 | 14 913 | the quarter frame's and the half frame's |
| 3 | 22 371 | 22 371 | the quarter frame's |
| 4 | 29 829 | none | the quarter frame's and the half frame's |
| 5 | none | 37 281 | the quarter frame's and the half frame's |
| the count returns to 0 | 29 830 | 37 282 | |

A cycle in the table is counted from the sequence's origin, the tick at which `frameCycle`
reads 0.

The quarter-frame clock falls at cycles 7457, 14 913, 22 371 and 29 829 of the 4-step sequence
and at 7457, 14 913, 22 371 and 37 281 of the 5-step one, the half-frame clock at the second
and the last of each four, and the count returns to 0 at 29 830 or 37 282: the page's
APU-cycle counts doubled, the quarter- and half-frame steps one CPU cycle on (NESdev wiki, "APU
Frame Counter"). The quarter-frame clock runs the envelopes and the triangle's linear counter,
the half-frame clock the length counters and the sweeps. A write to `$4017` restarts the
sequence two cycles after a write on a put cycle and three after one on a get, and a write with
bit 7 set makes both clocks one cycle before the restart, two or three cycles after the write;
until the restart the old sequence keeps stepping (NESdev wiki, "APU Frame Counter", Control
register; "APU", Frame Counter; blargg's 2005 frame counter readme; Visual 2A03).

```cpp
Chip chip;
chip.write(0x4015, 0x01);
chip.tick();               // cycle 0: the power-on restart, the sequence's origin
chip.write(0x4003, 0x18);  // cycle 1: pulse 1's length counter, 2
chip.runTo(14913);
// chip.state.length[0].counter == 2: the step at cycle 7457 was a quarter frame's alone
chip.tick();               // cycle 14913: the second step, a half frame's too
// chip.state.length[0].counter == 1
chip.runTo(29828);
// chip.state.frameInterrupt == false
chip.tick();               // cycle 29828: the first of the flag's three cycles
// chip.state.frameInterrupt == true and neasant::apuIrqAsserted(chip.state) == false
chip.tick();               // cycle 29829: the last step, and the line follows the flag
// chip.state.length[0].counter == 0 and neasant::apuIrqAsserted(chip.state) == true
chip.tick();               // cycle 29830: the count returns to 0
// chip.state.frameCycle == 0
```

### A write to $4017

A write to `$4017` sets the inhibit bit from bit 6, clearing the frame interrupt flag when it
is set, and holds bit 7 as the mode the next restart takes, `pendingFiveStep`. The restart
comes two cycles after a write on a put cycle and three after one on a get: `restartDelay`
takes 3 or 4 ticks, the write's own counted. With bit 7 set the tick before the restart runs
both clocks, so an access two or three cycles after the write is the first to see them
(NESdev wiki, "APU", Frame Counter: "2 or 3 cycles after the write"; "APU Frame Counter",
Control register; Visual 2A03). Until the restart the count keeps stepping in the old mode,
and a step it reaches there runs (blargg's 2005 frame counter readme).

```cpp
Chip chip;
chip.write(0x4015, 0x01);
chip.tick();
chip.write(0x4003, 0x18);  // cycle 1: pulse 1's length counter, 2
chip.tick();
chip.write(0x4017, 0x80);  // cycle 2, a get: the 5-step sequence
chip.tick(2);              // cycles 2 and 3
// chip.state.length[0].counter == 2 and chip.state.fiveStep == false
chip.tick();               // cycle 4: both clocks
// chip.state.length[0].counter == 1
chip.tick();               // cycle 5: the restart, three cycles after the write
// chip.state.fiveStep == true and chip.state.frameCycle == 0
chip.runTo(5 + 37283);     // through the 5-step sequence's 37 282 cycles
// chip.state.frameCycle == 0 and chip.state.frameInterrupt == false
```

The behavior page gives the evidence for
[the restart's cycle](apu-behavior.md#a-write-to-4017-restarts-the-sequence-two-or-three-cycles-on-by-the-half-it-lands-in),
for [the clocks' cycle](apu-behavior.md#a-write-with-bit-7-set-clocks-the-units-one-cycle-before-the-restart)
and for [how long the old sequence runs](apu-behavior.md#what-remains-open).

### The frame interrupt

The frame interrupt flag, `$4015` bit 6, is set on the last three cycles of the 4-step
sequence, 29 828 to 29 830, unless `$4017` bit 6 inhibits it; the 5-step sequence never sets
it. A read of `$4015` clears it, and so does a write to `$4017` with bit 6 set. A read in the
second or third of those cycles answers the flag set and that cycle's tick sets it again
(NESdev wiki, "APU", Status; "APU Frame Counter"). A program polling the flag reads `$4015`
once more after it finds it set, as blargg's readme advises, since the next cycle's tick may
set it again.

On the machine the flag reaches the CPU. A program that clears I and a handler that reads
`$4015`:

```cpp
// CLI; JMP $8001, and the handler LDA $4015; STA $10; RTI
std::vector<std::uint8_t> image =
    apuImage({0x58, 0x4C, 0x01, 0x80}, {0xAD, 0x15, 0x40, 0x85, 0x10, 0x40});
neasant::Nes machine(neasant::NesConfig{.rom = image});
for (int cycle = 0; cycle < 29830; ++cycle) machine.step();
// machine.state().apu.frameInterrupt == true: set by the tick of cycle 29828
// neasant::apuIrqAsserted(machine.state().apu) == true: the line, a cycle behind it
for (int cycle = 29830; cycle < 29852; ++cycle) machine.step();
// machine.state().ram[0x10] == 0x40: the handler's read of $4015 answered the flag
// machine.state().apu.frameInterrupt == false: and cleared it
// neasant::apuIrqAsserted(machine.state().apu) == false
```

The handler runs once; the 4-step sequence sets the flag again 29 830 cycles on.

### The IRQ line

```cpp
// The chip's IRQ output, true while it is pulled low: the line as the last tick sampled
// it.
[[nodiscard]] constexpr bool apuIrqAsserted(const ApuState& state) noexcept {
  return state.irqLine;
}
```

The IRQ line is the frame interrupt flag with the inhibit clear, or the DMC's interrupt flag,
sampled at the start of each tick, so it follows a flag a cycle later, and the CPU's level
detector takes it at the end of that cycle (NESdev wiki, "CPU interrupts", Detailed interrupt
behavior). With I clear, the CPU takes the interrupt at its next poll. The machine drives the
CPU's IRQ line from it at the end of every cycle
([a machine cycle](nes-machine.md#a-machine-cycle)), and
[the 6502 core's page](6502-cpu.md#when-a-request-is-taken) says when the CPU takes it.

## The status register

```cpp
// A CPU read of $4015: bit 7 the DMC's interrupt flag, bit 6 the frame interrupt flag,
// bit 4 whether the DMC's bytes remaining are above 0, bits 3-0 whether each length
// counter is above 0, bit 5 the open bus's, and the frame interrupt flag is then cleared
// (NESdev wiki, "APU", Status). A set the same cycle's tick makes is not cleared by it,
// since the tick runs after the access; the DMC's flag is not cleared by a read.
[[nodiscard]] std::uint8_t apuReadStatus(ApuState& state, std::uint8_t openBus) noexcept;

// What a read of $4015 would answer, with nothing cleared.
[[nodiscard]] std::uint8_t apuPeekStatus(const ApuState& state, std::uint8_t openBus) noexcept;
```

| Bit | A read answers | A write |
|---|---|---|
| 7 | the DMC's interrupt flag | nothing |
| 6 | the frame interrupt flag, which the read then clears | nothing |
| 5 | the open bus's bit | nothing |
| 4 | whether the DMC's bytes remaining are above 0 | clear, the sample stops: the bytes remaining go to 0 and the buffered byte plays out; set, a sample whose bytes remaining are 0 starts |
| 3 | whether the noise's length counter is above 0 | the noise's enable |
| 2 | whether the triangle's is | the triangle's enable |
| 1 | whether pulse 2's is | pulse 2's enable |
| 0 | whether pulse 1's is | pulse 1's enable |

(NESdev wiki, "APU", Status.) Every write clears the DMC's interrupt flag, and a read clears
the frame interrupt flag alone. A cleared enable bit empties its channel's length counter at
once and holds it at 0; a set one loads nothing. The triangle's bit is its length counter's:
the linear counter does not show in it.

On the machine the read leaves the open bus as it was, the register being internal to the CPU
([the machine page](nes-machine.md#open-bus)). `apuPeekStatus` is the answer with nothing
cleared, for a host looking at a chip it must not disturb.

```cpp
Chip chip;
chip.write(0x4015, 0x0C);  // cycle 0: enable the triangle and the noise
chip.tick();
chip.write(0x400B, 0x08);  // cycle 1: the triangle's length counter, 254
chip.tick();
chip.write(0x400F, 0x08);  // cycle 2: the noise's
chip.tick();
// neasant::apuPeekStatus(chip.state, 0x20) == 0x2C: bits 3 and 2 over the bus's bit 5
chip.runTo(29829);
// neasant::apuPeekStatus(chip.state, 0x20) == 0x6C: the frame interrupt flag beside them
const std::uint8_t read = neasant::apuReadStatus(chip.state, 0x00);
// read == 0x4C: the read answers the flag
// neasant::apuPeekStatus(chip.state, 0x00) == 0x0C: and has cleared it
```

## The length counters

```cpp
// One channel's length counter (NESdev wiki, "APU Length Counter"). The counter is
// loaded from the table by a write to the channel's fourth register while the channel is
// enabled, counts down on each half-frame clock while it is above 0 and not halted, and
// silences the channel at 0.
struct ApuLengthCounter {
  // The channel's enable bit as last written to $4015: cleared, the counter is held at 0
  // and a load is ignored (NESdev wiki, "APU Length Counter", Clocking).
  bool enabled = false;
  // The halt bit as last written: bit 5 of $4000, $4004 or $400C, bit 7 of $4008. The
  // same bit is the envelope's loop flag on a pulse and the noise, and the linear
  // counter's control flag on the triangle.
  bool halt = false;
  // The halt bit the half-frame clock consults: `halt` as it stood at the end of the
  // previous tick, so a halt written in the clock's own cycle does not hold it and one
  // written the cycle before does (blargg's 2005 frame counter readme, Length Halt).
  bool haltLatched = false;
  std::uint8_t counter = 0;
  // A load written this cycle, applied by the cycle's tick after its clocks: dropped if
  // that tick's half-frame clock counted the counter down, loaded otherwise (blargg's
  // 2005 frame counter readme, Length Reload).
  bool reloadPending = false;
  std::uint8_t reloadValue = 0;
  [[nodiscard]] bool operator==(const ApuLengthCounter&) const noexcept = default;
};
```

There are four, one each for pulse 1, pulse 2, the triangle and the noise. A write to a
channel's fourth register, `$4003`, `$4007`, `$400B` or `$400F`, loads its counter from the
32-entry table by bits 7–3 while the channel is enabled; the half-frame clock counts each
counter down while it is above 0 and its halt bit is clear, bit 5 of `$4000`, `$4004` and
`$400C`, bit 7 of `$4008`. A write to `$4015` with a channel's enable bit clear zeroes its
counter and holds it there (NESdev wiki, "APU Length Counter"). A halt bit takes effect a cycle
late, and a load written in the cycle of a half-frame clock that counts the counter down is
dropped (blargg's 2005 frame counter readme, Length Halt and Length Reload).

The table, by bits 7–3 of the fourth register:

| Entry | +0 | +1 | +2 | +3 | +4 | +5 | +6 | +7 |
|---|---|---|---|---|---|---|---|---|
| 0 | 10 | 254 | 20 | 2 | 40 | 4 | 80 | 6 |
| 8 | 160 | 8 | 60 | 10 | 14 | 12 | 26 | 14 |
| 16 | 12 | 16 | 24 | 18 | 48 | 20 | 96 | 22 |
| 24 | 192 | 24 | 72 | 26 | 16 | 28 | 32 | 30 |

(NESdev wiki, "APU Length Counter", Length table.)

```cpp
Chip chip;
chip.write(0x4015, 0x01);
chip.tick();
chip.write(0x4003, 0x08);  // cycle 1: entry 1 of the table, 254
chip.runTo(14914);
// chip.state.length[0].counter == 253: the half-frame clock of cycle 14913
chip.write(0x4000, 0x20);  // the halt bit
chip.runTo(29830);
// chip.state.length[0].counter == 253: the clock of cycle 29829 found the counter halted
chip.write(0x4015, 0x00);
// chip.state.length[0].counter == 0: a cleared enable bit empties the counter at once
chip.write(0x4003, 0x08);
chip.tick();
// chip.state.length[0].counter == 0: a load is ignored while the channel is disabled
```

## The envelopes

```cpp
// A volume envelope, one each for pulse 1, pulse 2 and the noise (NESdev wiki, "APU
// Envelope"). The quarter-frame clock runs it: with the start flag set it loads the
// decay level with 15 and the divider with the volume parameter; otherwise it counts the
// divider down, and a divider clocked at 0 reloads and counts the decay level down, or
// returns it from 0 to 15 while the channel's halt bit, the envelope's loop flag, is set.
struct ApuEnvelope {
  // Bit 4 of $4000, $4004 or $400C: set, the channel's volume is `volume`; clear, it is
  // the decay level.
  bool constant = false;
  // Bits 3-0 of the same register: the constant volume, and the divider's reload value,
  // so the decay level steps every `volume` + 1 quarter frames.
  std::uint8_t volume = 0;
  // Set by a write to the channel's fourth register; the next quarter-frame clock
  // restarts the envelope and clears it.
  bool start = false;
  std::uint8_t divider = 0;
  // The decay level, 15 down to 0. It runs whichever volume is selected.
  std::uint8_t decay = 0;
  [[nodiscard]] bool operator==(const ApuEnvelope&) const noexcept = default;
};
```

There are three, one each for pulse 1, pulse 2 and the noise. A write to the channel's fourth
register sets the envelope's start flag; the next quarter-frame clock loads the decay level with
15 and the divider with bits 3–0 of `$4000`, `$4004` or `$400C`. Each later clock counts the
divider down, and a divider clocked at 0 reloads and counts the decay level down, or returns it
from 0 to 15 while bit 5, the loop flag and the length counter's halt bit, is set. The channel's
volume is the decay level, or bits 3–0 while bit 4 is set (NESdev wiki, "APU Envelope").

```cpp
Chip chip;
chip.write(0x4015, 0x01);
chip.tick();
chip.write(0x4000, 0x02);  // cycle 1: the decay level as the volume, a divider period of 2
chip.tick();
chip.write(0x4003, 0x08);  // cycle 2: the start flag
chip.tick();
// chip.state.pulse[0].envelope.start == true
chip.runTo(7458);          // through the quarter-frame clock of cycle 7457
// chip.state.pulse[0].envelope.decay == 15 and chip.state.pulse[0].envelope.divider == 2
chip.runTo(22372);         // two more clocks: the divider counts 1, 0
// chip.state.pulse[0].envelope.decay == 15 and chip.state.pulse[0].envelope.divider == 0
chip.runTo(29830);         // the fourth: the divider reloads and the level steps
// chip.state.pulse[0].envelope.decay == 14 and chip.state.pulse[0].envelope.divider == 2
```

The level steps every `volume` + 1 quarter-frame clocks, three here.

## The pulse channels

```cpp
// A pulse channel (NESdev wiki, "APU Pulse"): a timer that counts once an APU cycle and
// steps an 8-step duty sequencer every `period` + 1 counts, an envelope, a sweep, and
// the length counter ApuState holds for it.
struct ApuPulse {
  // Bits 7-6 of $4000 or $4004: the waveform the sequencer reads, high for one step of
  // eight, two, four, or all but two.
  std::uint8_t duty = 0;
  ApuEnvelope envelope{};
  ApuSweep sweep{};
  // The timer's 11-bit period: $4002 or $4006 its low eight bits, bits 2-0 of $4003 or
  // $4007 its high three. The sweep rewrites it. A period below 8 silences the channel.
  std::uint16_t period = 0;
  // The timer's count, down to 0 and back to `period`.
  std::uint16_t timer = 0;
  // The sequencer's position in the waveform, 0 to 7. A write to $4003 or $4007 returns
  // it to 0; the timer's count is left as it is.
  std::uint8_t step = 0;
  // The level the channel's DAC takes, 0 to 15: the envelope's volume while the
  // waveform is high, the length counter is above 0 and neither the period nor the
  // sweep's target silences the channel, and 0 otherwise.
  std::uint8_t output = 0;
  [[nodiscard]] bool operator==(const ApuPulse&) const noexcept = default;
};
```

Each pulse's timer counts once an APU cycle, on get cycles, so that the sequencer moves as a
put cycle begins, down from its 11-bit period, `$4002` and bits 2–0 of `$4003` for pulse 1 and
`$4006` and `$4007` for pulse 2, and each count through 0 moves the sequencer one step along
the waveform bits 7–6 of `$4000` or `$4004` choose: high for one step of eight, two, four, or
all but two. A write to the fourth register returns the sequencer to its first step and leaves
the timer's count. `pulse[n].output` is the envelope's volume while the waveform is high and
the length counter is above 0, and 0 otherwise; a period below 8 silences the channel (NESdev
wiki, "APU Pulse"; Visual 2A03).

| Bits 7–6 | The waveform, step 0 first | High for |
|---|---|---|
| 0 | `0 1 0 0 0 0 0 0` | one step of eight |
| 1 | `0 1 1 0 0 0 0 0` | two |
| 2 | `0 1 1 1 1 0 0 0` | four |
| 3 | `1 0 0 1 1 1 1 1` | all but two |

(NESdev wiki, "APU Pulse", Duty Cycle Sequences.) A wave is eight steps of 2 × (period + 1)
CPU cycles each.

A program that sounds pulse 1 at a period of 8, a 50% duty and constant volume 15:

```cpp
// LDA #$01; STA $4015; LDA #$BF; STA $4000; LDA #$08; STA $4001; STA $4002; STA $4003;
// JMP $8015
std::vector<std::uint8_t> image = apuImage({0xA9, 0x01, 0x8D, 0x15, 0x40, 0xA9, 0xBF, 0x8D,
                                            0x00, 0x40, 0xA9, 0x08, 0x8D, 0x01, 0x40, 0x8D,
                                            0x02, 0x40, 0x8D, 0x03, 0x40, 0x4C, 0x15, 0x80});
neasant::Nes machine(neasant::NesConfig{.rom = image});
for (int cycle = 0; cycle < 47; ++cycle) machine.step();
// machine.state().apu.pulse[0].output == 15: the waveform's second step, its first high one
for (int cycle = 47; cycle < 119; ++cycle) machine.step();
// machine.state().apu.pulse[0].output == 0: four steps of 18 cycles on, the waveform is low
```

The write to `$4003` in cycle 32 returns the sequencer to its first step. The timer, reloaded
with 8 in the cycle of the write to `$4002`, a get cycle, counts through 0 at cycle 46 and every
18 cycles after, each time moving the sequencer a step.

### The sweeps

```cpp
// A pulse channel's sweep unit (NESdev wiki, "APU Pulse", Sweep unit operation). Its
// target is the channel's period plus the period shifted right by `shift`, or minus it
// when `negate` is set; pulse 1 then subtracts one more, adding the ones' complement
// where pulse 2 adds the two's. A target above $7FF silences the channel whether or not
// the sweep is enabled. The half-frame clock sets the period to the target when the
// divider is at 0, the sweep is enabled, the shift is above 0, the channel's length
// counter is above 0 and neither the period nor the target silences the channel; it then
// reloads the divider when it is at 0 or `reload` is set, and counts it down otherwise,
// whatever the length counter holds (2A03 technical reference, Sweep unit; Visual 2A03).
struct ApuSweep {
  // Bit 7 of $4001 or $4005.
  bool enabled = false;
  // Bits 6-4: the divider's reload value, so the period is rewritten every `period` + 1
  // half frames.
  std::uint8_t period = 0;
  // Bit 3: set, the target is below the period and the pitch rises.
  bool negate = false;
  // Bits 2-0.
  std::uint8_t shift = 0;
  // Set by a write to $4001 or $4005; the next half-frame clock reloads the divider and
  // clears it.
  bool reload = false;
  std::uint8_t divider = 0;
  [[nodiscard]] bool operator==(const ApuSweep&) const noexcept = default;
};
```

A pulse's sweep targets its period plus the period shifted right by bits 2–0 of `$4001` or
`$4005`, or minus it with bit 3 set, where pulse 1 subtracts one more than pulse 2. A target
above `$7FF` silences the channel whether or not the sweep is enabled. On a half-frame clock
the period becomes the target when the divider is at 0, bit 7 enables the sweep, the shift is
above 0, the length counter is above 0 and the channel is not silenced; the divider then
reloads with bits 6–4 when it is at 0 or the register was written since the last clock, and
counts down otherwise (NESdev wiki, "APU Pulse", Sweep unit operation; blargg's NES APU
reference, Sweep Unit; 2A03 technical reference, Sweep unit; Visual 2A03).

```cpp
Chip chip;
chip.write(0x4015, 0x01);
chip.tick();
chip.write(0x4000, 0xBF);  // cycle 1: a 50% duty, the halt bit, constant volume 15
chip.tick();
chip.write(0x4001, 0x81);  // cycle 2: the sweep enabled, a divider period of 0, a shift of 1
chip.tick();
chip.write(0x4002, 0x00);  // cycle 3
chip.tick();
chip.write(0x4003, 0x09);  // cycle 4: a period of $100, and the length counter
chip.runTo(14914);
// chip.state.pulse[0].period == 0x180: the half-frame clock added the period shifted once
chip.runTo(74574);
// chip.state.pulse[0].period == 0x798: four more clocks, through $240, $360 and $510
chip.runTo(89490);
// chip.state.pulse[0].period == 0x798: the next target, $B64, is above $7FF, so the
//                                      channel is silent and keeps its period
```

A divider period of 0 gives a rewrite at every half-frame clock, and the period climbs by half
of itself until the next target would pass `$7FF`.

## The triangle

```cpp
// The triangle channel (NESdev wiki, "APU Triangle"): a timer that counts every CPU
// cycle and steps a 32-step sequencer every `period` + 1 counts while the linear counter
// and the length counter are both above 0. A counter at 0 stops the sequencer where it
// stands, and the channel keeps answering that step's level.
struct ApuTriangle {
  // Bits 6-0 of $4008: the value the linear counter reloads with.
  std::uint8_t linearLoad = 0;
  // Set by a write to $400B. While it is set the quarter-frame clock reloads the linear
  // counter; the clock then clears it unless the channel's halt bit, the linear
  // counter's control flag, is set.
  bool linearReload = false;
  // The linear counter: the quarter-frame clock counts it down while it is above 0 and
  // `linearReload` is clear.
  std::uint8_t linear = 0;
  // The timer's 11-bit period: $400A its low eight bits, bits 2-0 of $400B its high
  // three.
  std::uint16_t period = 0;
  std::uint16_t timer = 0;
  // The sequencer's position, 0 to 31: the levels 15 down to 0, then 0 up to 15. Reset
  // returns it to 0.
  std::uint8_t step = 0;
  // The level the channel's DAC takes, 0 to 15: the sequence's value at `step`, 15 at
  // power-on.
  std::uint8_t output = 15;
  [[nodiscard]] bool operator==(const ApuTriangle&) const noexcept = default;
};
```

Its timer counts every CPU cycle down from its period, `$400A` and bits 2–0 of `$400B`, and
each count through 0 moves the 32-step sequencer, 15 down to 0 and back up, while the linear
counter and the length counter are both above 0; with either at 0 the sequencer stands and
`triangle.output` keeps its level. A write to `$400B` sets the linear counter's reload flag. A
quarter-frame clock reloads the counter from bits 6–0 of `$4008` while the flag is set and
counts it down otherwise, and then clears the flag unless bit 7 of `$4008` is set (NESdev wiki,
"APU Triangle"). A wave is 32 steps of period + 1 CPU cycles each.

```cpp
Chip chip;
chip.write(0x4015, 0x04);  // cycle 0: enable the triangle
chip.tick();
chip.write(0x4008, 0x7F);  // cycle 1: the linear counter's reload value, 127; bit 7 clear
chip.tick();
chip.write(0x400A, 0x03);  // cycle 2: a period of 3, a step every four CPU cycles
chip.tick();
chip.write(0x400B, 0x08);  // cycle 3: the length counter, 254, and the reload flag
chip.tick();
// chip.state.triangle.output == 15 and chip.state.triangle.linear == 0: the sequencer stands
chip.runTo(7458);          // through the quarter-frame clock of cycle 7457
// chip.state.triangle.linear == 127 and chip.state.triangle.linearReload == false
chip.tick();               // cycle 7458: the timer counts through 0
// chip.state.triangle.output == 14
chip.tick(4);
// chip.state.triangle.output == 13
chip.runTo(14914);         // through the next quarter-frame clock
// chip.state.triangle.linear == 126
```

## The noise

```cpp
// The noise channel (NESdev wiki, "APU Noise"): a timer that clocks a 15-bit shift
// register every so many CPU cycles as the period table gives, and an envelope. Each
// clock shifts the register right and puts the exclusive OR of bit 0 and bit 1, or of
// bit 0 and bit 6 in the periodic mode, in bit 14.
struct ApuNoise {
  ApuEnvelope envelope{};
  // Bit 7 of $400E: set, the feedback takes bit 6 and the sequence is 93 or 31 steps
  // long; clear, it takes bit 1 and the sequence is 32 767 steps long.
  bool mode = false;
  // Bits 3-0 of $400E: the entry of the period table the timer reloads with.
  std::uint8_t periodIndex = 0;
  // The timer's count, in APU cycles.
  std::uint16_t timer = 0;
  // The shift register, 1 at power-on. Reset leaves it as it is.
  std::uint16_t shift = 1;
  // The level the channel's DAC takes, 0 to 15: the envelope's volume while bit 0 of the
  // shift register is clear and the length counter is above 0, and 0 otherwise.
  std::uint8_t output = 0;
  [[nodiscard]] bool operator==(const ApuNoise&) const noexcept = default;
};
```

Its timer clocks a 15-bit shift register every 4 to 4068 CPU cycles by bits 3–0 of `$400E`,
counted on get cycles. A clock shifts the register right and puts in bit 14 the exclusive OR of
bits 0 and 1, or of bits 0 and 6 with bit 7 of `$400E` set, which shortens the sequence from
32 767 steps to 93 or 31. `noise.output` is the envelope's volume while bit 0 of the register is
clear and the length counter is above 0 (NESdev wiki, "APU Noise").

| Entry | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| CPU cycles a clock | 4 | 8 | 16 | 32 | 64 | 96 | 128 | 160 | 202 | 254 | 380 | 508 | 762 | 1016 | 2034 | 4068 |

(NESdev wiki, "APU Noise", the NTSC column.)

```cpp
Chip chip;
chip.write(0x4015, 0x08);  // cycle 0: enable the noise
chip.tick();
// chip.state.noise.shift == 0x4000: the register's first clock, on the first get cycle
chip.write(0x400C, 0x3F);  // cycle 1: the halt bit, constant volume 15
chip.tick();
chip.write(0x400E, 0x00);  // cycle 2: the shortest period, four CPU cycles
chip.tick();
chip.write(0x400F, 0x08);  // cycle 3: the length counter
chip.tick();
// chip.state.noise.output == 15: bit 0 of the register is clear
chip.runTo(57);            // thirteen more clocks bring the 1 down to bit 1, and the next
// chip.state.noise.shift == 0x4001 and chip.state.noise.output == 0: bit 0 is set
```

The register holds 1 at power-on and takes its first clock on the first get cycle, before any
instruction runs.

## The DMC

```cpp
// What the DMC's memory reader is waiting to fetch: nothing, the first byte of a sample
// after a $4015 write started it with the buffer empty, or the next byte after the output
// unit emptied the buffer (NESdev wiki, "DMA", DMC DMA).
enum class DmcFetch : std::uint8_t { None, Load, Reload };

// The delta modulation channel (NESdev wiki, "APU DMC"): a memory reader that fetches a
// sample's bytes one at a time through the machine's DMA unit, a one-byte sample buffer,
// an output unit that shifts the buffered byte out a bit a clock and steps a 7-bit
// counter by 2 up for a 1 and down for a 0, a timer clocking it at one of sixteen rates,
// and the interrupt flag the fetch that ends a sample sets.
struct ApuDmc {
  // Bit 7 of $4010: set, the fetch that ends a sample sets the interrupt flag; a write
  // clearing it clears the flag.
  bool irqEnable = false;
  // Bit 6 of $4010: set, a sample that ends restarts from $4012 and $4013 and sets no
  // flag.
  bool loop = false;
  // Bits 3-0 of $4010: the entry of the rate table, the CPU cycles between two clocks of
  // the output unit.
  std::uint8_t rateIndex = 0;
  // $4012 as written: a sample starts at $C000 plus 64 times it.
  std::uint8_t sampleAddress = 0;
  // $4013 as written: a sample is 16 times it plus 1 bytes long.
  std::uint8_t sampleLength = 0;
  // The reader's address, the next byte to fetch; it steps from $FFFF to $8000.
  std::uint16_t address = 0;
  // The reader's bytes remaining, bit 4 of a $4015 read while above 0. A $4015 write
  // with bit 4 set restarts the sample when it is 0 and a write with bit 4 clear sets it
  // to 0, the buffered byte still playing out.
  std::uint16_t bytesRemaining = 0;
  // The sample buffer: the byte the DMA unit delivered last, and whether the output unit
  // has yet to take it.
  std::uint8_t buffer = 0;
  bool bufferFull = false;
  // The fetch the reader is waiting for, which the machine's DMA unit carries out.
  DmcFetch fetch = DmcFetch::None;
  // Ticks since the sample stopped, by a $4015 write or by its last fetch, held at 3. A
  // stop one or two ticks before the output unit empties the buffer still asks for a
  // fetch, which the DMA unit abandons after its halt cycle (NESdev wiki, "DMA", Bugs).
  std::uint8_t stopAge = 3;
  // The output unit's shift register, the bits left in it, 8 down to 1, and the silence
  // flag, set when a byte's cycle began with the buffer empty.
  std::uint8_t shift = 0;
  std::uint8_t bitsRemaining = 8;
  bool silence = true;
  // The timer's count, in APU cycles.
  std::uint16_t timer = 0;
  // The 7-bit counter, the level the channel's DAC takes: bits 6-0 of $4011 load it, and
  // each clock with the silence flag clear steps it by 2, up for a 1 bit while it is
  // below 126 and down for a 0 while it is above 1.
  std::uint8_t output = 0;
  // The interrupt flag, bit 7 of $4015: set by the fetch that ends a sample while bit 7
  // of $4010 is set, cleared by any write to $4015 and by a $4010 write with bit 7
  // clear.
  bool interrupt = false;
  [[nodiscard]] bool operator==(const ApuDmc&) const noexcept = default;
};
```

A write to `$4015` with bit 4 set starts a sample whose bytes remaining are 0: the reader takes
its address, `$C000` plus 64 times `$4012`, and its length, 16 times `$4013` plus 1, and asks
the DMA unit for the first byte at once when the sample buffer is empty
([The sample fetch](#the-sample-fetch)). Each fetched byte waits in the buffer until the output
unit, which runs at the rate bits 3–0 of `$4010` name, 428 down to 54 CPU cycles a bit, takes it
after the eighth bit of the last; the reader then asks for the next while bytes remain, the
address stepping from `$FFFF` to `$8000`. Each bit steps the 7-bit level, `dmc.output`, by 2,
up for a 1 while it is below 126 and down for a 0 while it is above 1; a write to `$4011` loads
the level directly. The fetch that ends a sample restarts it while bit 6 of `$4010` is set and
otherwise sets the interrupt flag, bit 7 of `$4015`, while bit 7 of `$4010` is set; any write to
`$4015` and a `$4010` write with bit 7 clear clear the flag. A write to `$4015` with bit 4 clear
sets the bytes remaining to 0 and the buffered byte plays out (NESdev wiki, "APU DMC"; "APU",
Status).

| Entry | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| CPU cycles a bit | 428 | 380 | 340 | 320 | 286 | 254 | 226 | 214 | 190 | 160 | 142 | 128 | 106 | 84 | 72 | 54 |

(NESdev wiki, "APU DMC", Pitch table, the NTSC column.)

A chip driven by hand stands in for the DMA unit: it asks the reader what it wants and hands it
the byte.

```cpp
Chip chip;
chip.write(0x4011, 0x40);
// chip.state.dmc.output == 64: the level is loaded at the write
chip.tick();
chip.write(0x4012, 0xFC);  // cycle 1: samples start at $C000 + 64 * $FC, $FF00
chip.tick();
chip.write(0x4013, 0x00);  // cycle 2: and are 16 * 0 + 1 bytes long
chip.tick();
chip.write(0x4015, 0x10);  // cycle 3: start the sample
// neasant::apuDmcWantsFetch(chip.state) == neasant::DmcFetch::Load
// chip.state.dmc.address == 0xFF00 and chip.state.dmc.bytesRemaining == 1
neasant::apuDmcDeliver(chip.state, 0xFF);  // the byte a DMA unit read at that address
// chip.state.dmc.bufferFull == true and chip.state.dmc.bytesRemaining == 0
// neasant::apuDmcWantsFetch(chip.state) == neasant::DmcFetch::None
chip.runTo(2997);  // through cycle 2996, where the output unit's eighth clock takes the byte
// chip.state.dmc.bufferFull == false and chip.state.dmc.silence == false
chip.runTo(6421);  // eight clocks of 428 cycles, each a 1 bit
// chip.state.dmc.output == 80: eight steps of 2 up
```

At power-on the output unit has eight clocks to go at the rate of entry 0, so the first byte is
taken at cycle 2996.

### The sample fetch

```cpp
// The fetch the DMC's reader is waiting to make, which the machine's DMA unit carries
// out on the CPU's RDY line from the reader's address: a load from the cycle after the
// $4015 write that started the sample, a reload from the cycle after the tick in which
// the output unit emptied the buffer. The request stands until the byte is delivered,
// and lapses at the next tick once the bytes remaining are 0 (NESdev wiki, "DMA", DMC
// DMA and Bugs).
[[nodiscard]] constexpr DmcFetch apuDmcWantsFetch(const ApuState& state) noexcept {
  return state.dmc.fetch;
}

// Puts the byte the DMA unit fetched into the sample buffer, steps the reader's address,
// wrapping from $FFFF to $8000, and counts its bytes remaining down; at 0 the sample
// restarts from $4012 and $4013 while $4010 bit 6 is set, and otherwise ends, setting the
// interrupt flag while $4010 bit 7 is set (NESdev wiki, "APU DMC", Memory reader).
void apuDmcDeliver(ApuState& state, std::uint8_t byte) noexcept;
```

The reader asks, and the machine's DMA unit carries the fetch out on the CPU's RDY line. A
sample's first byte halts the CPU on the third cycle after a `$4015` write on a put cycle and
the fourth after one on a get; every later byte halts it in the cycle after the tick in which
the output unit emptied the buffer. A byte takes three or four cycles, the halted CPU repeating
its read on each but the get. A request lapses once the bytes remaining are 0, which is how a
sample stopped just before its reload costs one halted cycle and no byte (NESdev wiki, "DMA",
DMC DMA and Bugs). The DMA unit's own stages are on
[the machine page](nes-machine.md#oam-dma-and-the-dmcs-fetch).

A program that starts a one-byte sample at the fastest rate and reads the status back:

```cpp
// LDA #$8F; STA $4010; LDA #$FC; STA $4012; LDA #$00; STA $4013; LDA #$10; STA $4015;
// LDA $4015; STA $10; JMP $8019
std::vector<std::uint8_t> image = apuImage({0xA9, 0x8F, 0x8D, 0x10, 0x40, 0xA9, 0xFC, 0x8D,
                                            0x12, 0x40, 0xA9, 0x00, 0x8D, 0x13, 0x40, 0xA9,
                                            0x10, 0x8D, 0x15, 0x40, 0xAD, 0x15, 0x40, 0x85,
                                            0x10, 0x4C, 0x19, 0x80});
image[16 + 0x3F00] = 0x5A;  // the sample's one byte, at $FF00 through the mirror
neasant::Nes machine(neasant::NesConfig{.rom = image});
for (int cycle = 0; cycle < 34; ++cycle) machine.step();
// machine.state().cpu.rdyLine == true: the write landed in cycle 30 and the CPU runs on
machine.step();  // cycle 34: the halt cycle, the read of $4015 repeated while the CPU is held
// machine.state().cpu.rdyHeld == true and machine.state().dma.dmcStage == neasant::DmcStage::Dummy
machine.step();  // cycle 35: the dummy cycle
machine.step();  // cycle 36: the DMA unit reads $FF00
// machine.state().apu.dmc.buffer == 0x5A and machine.state().apu.dmc.bytesRemaining == 0
// machine.state().apu.dmc.interrupt == true: the one-byte sample ended with bit 7 of $4010 set
for (int cycle = 37; cycle < 41; ++cycle) machine.step();
// machine.state().ram[0x10] == 0x80: the read of $4015 made at cycle 37 sees the flag and no bytes
```

The write to `$4015` lands in cycle 30, a get cycle; the DMA unit halts the CPU on the fourth
cycle after it, spends a dummy cycle and reads the byte on the sixth, and the CPU resumes on the
seventh. The read of `$4015` is the instruction the CPU was halted on, made for real after the
fetch.

## The output

The five levels become the console's sound in three steps, each a function the machine calls
once a CPU cycle after the tick: `apuMix` makes one level of the five, `apuFilter` passes it
through the two filters the NES follows its DACs with, and `apuDecimate` averages the
filters' output down to samples at the rate the machine was built with. The machine queues
each sample as a frame, and a host drains them with `takeFrames`
([the machine page](nes-machine.md#the-apu)).

### The mixer

```cpp
// The level the chip's output takes, 0 to 32 767, from the level each channel's DAC
// takes. The two pulses go through one curve, 95.88 / (8128 / (pulse 1 + pulse 2) + 100),
// and the triangle, the noise and the DMC through another, 159.79 / (1 / (triangle /
// 8227 + noise / 12241 + dmc / 22638) + 100), a group whose channels all stand at 0
// adding 0. The two curves together run from 0.0 to 1.0; each one's value is scaled by
// 32 767 and rounded to the nearest whole number, a half upward, and the two are added
// (NESdev wiki, "APU Mixer"). Each curve flattens as its group's levels rise, so the
// DMC's level sets how far a step of the triangle or the noise moves the output.
// `expansion` is added to the sum, which is held at 32 767, the most a sample holds. The
// level is the DACs' own, 0 with every channel at 0; the console's output stage takes it
// from here (apuFilter).
[[nodiscard]] std::int16_t apuMix(const ApuState& state, std::int16_t expansion) noexcept;
```

`apuMix` answers the level the chip's output takes, 0 to 32 767, from the five channels'
levels: the two pulses through one curve, `95.88 / (8128 / (pulse1 + pulse2) + 100)`, and the
triangle, the noise and the DMC through another,
`159.79 / (1 / (triangle / 8227 + noise / 12241 + dmc / 22638) + 100)`, a group at 0 adding 0;
each curve's value is scaled by 32 767 and rounded to the nearest whole number, and the two are
added (NESdev wiki, "APU Mixer"). Each curve flattens as it rises, so the DMC's level sets how
far the triangle and the noise move the output: the triangle's 15 adds 8074 over a DMC at 0 and
3508 over one at 127. The level is the DACs' own, 0 with every channel at 0.

| The channels' levels | `apuMix` |
|---|---|
| every channel at 0 | 0 |
| the triangle at 15, as at power-on | 8074 |
| pulse 1 at 15 | 4895 |
| both pulses at 15 | 8470 |
| the noise at 15 | 5716 |
| the DMC at 127 | 18 817 |
| the triangle at 15 and the DMC at 127 | 22 325 |
| the triangle and the noise at 15 and the DMC at 127 | 24 297 |
| every channel at its top | 32 767 |

```cpp
neasant::ApuState state;  // power-on: the triangle at 15, every other channel at 0
// neasant::apuMix(state, 0) == 8074
state.pulse[0].output = 15;
// neasant::apuMix(state, 0) == 12969: pulse 1 at 15 adds 4895, whatever the other group holds
state.dmc.output = 127;
// neasant::apuMix(state, 0) == 27220: the DMC's 127 adds 14 251 beside the triangle's 15
// neasant::apuMix(state, 6000) == 32767: a cartridge's level is added and the sum held
```

### The output stage

```cpp
// The console's output stage, the two filters the NES follows its DACs with (apuFilter):
// the level each holds, in units of 1 / 16 777 216 of the mixer's level.
struct ApuFilter {
  // The level the high-pass has settled on, which it takes away from the mixer's. At
  // power-on it is the level the DACs start at, 8074 with the triangle at its first step,
  // so the output opens at 0.
  std::int64_t highPass = std::int64_t{8074} * 16777216;
  // The level the low-pass has reached, the stage's output.
  std::int64_t lowPass = 0;
  [[nodiscard]] bool operator==(const ApuFilter&) const noexcept = default;
};

// One CPU cycle of the console's output stage, given the level the mixer answers for the
// cycle: the two filters the NES follows its DACs with, a first-order high-pass at 90 Hz
// and a first-order low-pass at 14 kHz (NESdev wiki, "APU Mixer"). Each moves a fixed
// part of the way from the level it holds to the level it is given, the part a filter of
// its corner moves toward a level held for one CPU cycle: 5300 / 16 777 216 for the
// high-pass and 804 639 / 16 777 216 for the low-pass. The high-pass answers the mixer's
// level less the level it holds; the low-pass is given that, and the level it holds,
// rounded to the nearest whole number and held within -32 768 and 32 767, is the stage's
// answer. A level that stands is taken away: a step of the level passes, falls to 1 / e
// of itself every 3165 cycles, 1.77 ms, and reaches 0, so the output rests at 0 whatever
// the DACs hold. The low-pass follows a step with a time constant of 21 cycles. At
// power-on the high-pass holds the level the DACs start at, so the output opens at 0.
[[nodiscard]] std::int16_t apuFilter(ApuState& state, std::int16_t level) noexcept;
```

The NES follows its DACs with a first-order high-pass filter at 90 Hz and a first-order
low-pass at 14 kHz (NESdev wiki, "APU Mixer"), and `apuFilter` is the two, run once a CPU cycle
on the mixer's level. Each filter moves a fixed part of the way from the level it holds to the
level it is given, the part a filter of its corner moves toward a level held for one CPU cycle:
5300 / 16 777 216 for the high-pass, a corner of 90.00002 Hz, and 804 639 / 16 777 216 for the
low-pass, 13 999.997 Hz. The high-pass answers the mixer's level less the level it holds; the
low-pass is given that, and the level it holds, rounded and held within −32 768 and 32 767, is
the stage's output. A level that stands is taken away, so the output rests at 0 whatever the
DACs hold: a step of the level passes, falls to 1 / e of itself every 3165 cycles, 1.77 ms, and
reaches 0. The low-pass follows a step with a time constant of 21 cycles, which rounds an edge
over a sample or two at 48 000 Hz. The two levels are state, `apu.filter`. At power-on the
high-pass holds the level the DACs start at, 8074, so the output opens at 0; a reset keeps
both.

```cpp
neasant::ApuState state;
std::int16_t out = neasant::apuFilter(state, 8074);
// out == 0: at power-on the stage is settled on the level the DACs start at
out = neasant::apuFilter(state, 22325);
// out == 683: a step of 14 251, the low-pass one cycle into its rise
for (int cycle = 1; cycle < 102; ++cycle) out = neasant::apuFilter(state, 22325);
// out == 13791: the step's peak, 102 cycles in
for (int cycle = 102; cycle < 3165; ++cycle) out = neasant::apuFilter(state, 22325);
// out == 5276: the high-pass's time constant on, 1.77 ms
for (int cycle = 3165; cycle < 32481; ++cycle) out = neasant::apuFilter(state, 22325);
// out == 0: the standing level is taken away, 18 ms after the step
```

### The decimator

```cpp
// Where the console's output stands in the sample under way (apuDecimate). Time is
// counted in units of 1 / (236 250 000 times the output rate) of a second: a CPU cycle is
// 132 times the rate of them and a sample is 236 250 000 (NESdev wiki, "CPU",
// Frequencies: the CPU clock is 236 250 000 / 132 Hz).
struct ApuDecimator {
  // The time run since the last sample was made, in those units; below 236 250 000
  // between calls.
  std::uint32_t elapsed = 0;
  // The output level summed over the CPU cycles since the last sample, and the count of
  // those cycles.
  std::int64_t sum = 0;
  std::uint32_t count = 0;
  [[nodiscard]] bool operator==(const ApuDecimator&) const noexcept = default;
};

// One CPU cycle of the console's output, called after the cycle's tick: passes the level
// apuMix answers through the output stage (apuFilter), adds the stage's answer to the
// sample under way and moves the decimator on by the cycle, 132 times the rate; when that
// completes a sample's 236 250 000 it answers the sample, the sum divided by the count of
// cycles it spans with the fraction dropped, and begins the next with the time left
// over. A sample is thus the mean of the console's output over the cycles it spans, 37 or
// 38 of them at 48 000 Hz, and the first N cycles from power-on make the whole part of N
// times the rate times 132 / 236 250 000 samples. A rate of 0 answers nothing and leaves
// the state as it is, the output stage included; the rate is at most the CPU clock's
// 1 789 772, a sample a cycle.
[[nodiscard]] std::optional<std::int16_t> apuDecimate(ApuState& state,
                                                      const ApuInputs& inputs) noexcept;
```

After each tick the mixer's level goes through the output stage and the stage's answer is
added to the sample under way. Time is counted in units of 1 / (236 250 000 × rate) of a
second, in which a CPU cycle is 132 × rate and a sample 236 250 000 (NESdev wiki, "CPU",
Frequencies), so the samples come at exactly the rate against the master clock: the first N
cycles from power-on make the whole part of N × rate × 132 / 236 250 000 of them. A sample is
the mean of the output over the CPU cycles it spans, 37 or 38 at 48 000 Hz, with the fraction
dropped. A rate of 0 answers nothing and leaves the state as it is, the output stage
included; the rate is at most the CPU clock's 1 789 772, a sample a cycle. The decimator's
place in its sample is state, `decimator`, so a snapshot resumes inside a sample and two
chips run alike make equal samples.

```cpp
neasant::ApuState state;
const neasant::ApuInputs inputs{.audioRate = 48000, .expansion = 0};
int samples = 0;
for (int cycle = 0; cycle < 39375; ++cycle) {
  if (neasant::apuDecimate(state, inputs)) ++samples;
}
// samples == 1056 and state.decimator.elapsed == 0: 39 375 CPU cycles are 1056 samples
```

On the machine a picture's 357 368 master cycles make 798 or 799 frames at 48 000 Hz, and
[the machine page](nes-machine.md#the-apu) has the two drains.

## Power-on and reset

```cpp
// The chip after the reset line: every channel disabled with its counter at 0, the DMC's
// sample stopped with its interrupt flag clear and its level keeping its low bit, the
// frame interrupt flag clear, the sequence restarting at the next tick in the mode last
// written to $4017, the triangle's sequencer at its first step, and the pulses and the
// noise silent. The inhibit bit, every halt bit, every channel register, every timer,
// envelope and sweep, the noise's shift register, the DMC's buffer and output unit are
// kept, and so are the output stage and the decimator, which stand outside the chip
// (NESdev wiki, "CPU power up state", APU, the After Reset column; "APU", Status; "APU
// Noise", Operation).
void apuReset(ApuState& state) noexcept;
```

`ApuState`'s defaults are the chip at power-on, and `apuReset` is the reset line. At power-on
the sequence restarts at the first cycle's tick, which is where a write of `$00` to `$4017`
nine cycles before the first instruction would put its origin: the usual figure of the 9 to
12 that blargg's `apu_reset` readme measures. A reset restarts it the same way, in the mode
last written.

| | At power-on | After `apuReset` |
|---|---|---|
| the frame counter | the 4-step sequence with the interrupt enabled, restarting at the first tick | restarting at the next tick in the mode last written to `$4017`; the inhibit bit kept |
| the frame interrupt flag | clear | clear |
| the four enables and length counters | disabled, at 0 | disabled, at 0; the halt bits kept |
| the pulses and the noise | every register 0, the noise's shift register 1 | their registers, timers, envelopes, sweeps and sequencer places and the shift register kept; silent, their counters being at 0 |
| the triangle | at its first step, level 15 | returned to its first step, level 15; its registers, its timer and its linear counter kept |
| the DMC | silent, its level 0, its output unit eight clocks from taking a byte | its sample stopped and a pending fetch dropped, its interrupt flag clear, its level keeping its low bit; `$4010`, `$4012`, `$4013`, the buffer and the output unit kept |
| the output stage and the decimator | the stage settled on the mixer's level, 8074, with no sample begun | kept, standing outside the chip |

(NESdev wiki, "CPU power up state", APU; "APU", Status; "APU Noise", Operation; blargg's
`apu_reset` readme.) The mixer's level at power-on is 8074, the triangle's 15 alone, and the
output, whose stage starts settled on that level, opens at 0.

```cpp
Chip chip;
chip.write(0x4015, 0x01);
chip.tick();
chip.write(0x4017, 0xC0);  // cycle 1: the 5-step sequence, the interrupt inhibited
chip.tick();
chip.write(0x4002, 0xFD);  // cycle 2: pulse 1's period
chip.tick();
chip.write(0x4003, 0x08);  // cycle 3: its length counter, 254
chip.tick();
chip.write(0x4011, 0x7F);  // cycle 4: the DMC's level
chip.tick();
neasant::apuReset(chip.state);
// chip.state.length[0].enabled == false and chip.state.length[0].counter == 0
// chip.state.inhibit == true and chip.state.pendingFiveStep == true: $4017 as last written
// chip.state.pulse[0].period == 0x0FD: the channels' registers are kept
// chip.state.dmc.output == 1: the DMC's level keeps its low bit
chip.tick();
// chip.state.fiveStep == true and chip.state.frameCycle == 0: the sequence, restarted
```

The machine's [reset line](nes-machine.md#the-reset-line) resets the chip this way, and also
ends a fetch under way and discards the frames not yet drained.

## Testing against the programs

The APU's self-checking test programs run on [the machine](nes-machine.md), each booted with
the config's defaults and run a frame at a time until it reports, as
[the machine page](nes-machine.md#testing-against-the-programs) describes: `apu_test`,
`apu_reset`, `blargg_apu_2005.07.30`, `cpu_interrupts_v2`, `dmc_dma_during_read4`,
`sprdma_and_dmc_dma`, `dmc_tests` and `apu_mixer` as the `christopherpow/nes-test-roms`
repository collects them, 47 ROM files, and Fiskbit's two `dmc_dma_start_test` programs and
Damian Yerrick's `dpcmletterbox` from their own archives.

Of the APU's 47, 40 pass; `apu_test.nes` and `cpu_interrupts.nes`, the multi-test builds on mapper
1, skip naming the mapper; `double_2007_read`, which reads `$2007` on two adjacent cycles with no
DMA, skips naming the PPU's port; `dmc_tests`'s four, which carry no readme or source, skip naming
the description they lack. `dpcmletterbox`, whose NMI handler waits for the sprite 0 hit at the top
of the picture and then times the frame by the DMC's interrupt, takes twenty interrupt sequences a
frame, the measured playback's and nineteen of its chain, counted through the bus observer from its
fourth frame under either half. Fiskbit's 21 cases are measured through the bus observer as the
cycles between the two `$2001` writes that bound each greyscale region and the deleted-bit counters
in RAM, and every one gives the figure his readme measures on an RP2A03H. `apu_mixer`'s four report
by sound: each plays a beep, a test tone its DMC level cancels, and a beep, and its case drains the
frames a picture at a time and pins where the two beeps fall and, between them, the level and the
tone in thousandths of the beeps' own: 22 and 10 for `square`, 15 and 7 for `triangle`, 18 and 8 for
`dmc`, and 130 and 13 for `noise`, whose readme has its noise fade in and out with no tone.
`2-nmi_and_brk`, whose readme says it "occasionally fails on NES due to PPU-CPU synchronization",
passes under alignment 0, the default, and fails under 1, 2 and 3. Every APU program passes under
either `apuPhase`; the mixer programs' figures are the default's, and under the other half `square`
gives 14 and 7 and `dmc` 12 and 6.

`NEASANT_DMC_PROGRAMS_DIR` points at a directory holding `dmc_dma_start_test/`,
`dmc_dma_start_test_v2/` and `dpcmletterbox/`, each with its ROM, and
`NEASANT_REQUIRE_DMC_PROGRAMS=1` turns their skips into failures as
[the other variable](nes-machine.md#testing-against-the-programs) does.

The hand-derived suites, by executable:

- `apu_frame_test` — the frame counter's two sequences from a write on either APU half, the
  flag's three cycles and the status read against them, the IRQ line a cycle behind the flag,
  the restart with its delay, the length table, the halt bit a cycle late, the load against a
  clock, the enable bits, power-on and reset, driven through `apu.h` and on the machine; and the
  APU's programs above.
- `apu_channels_test` — the pulses' timers, duty waveforms and period floor, the envelopes, the
  sweeps with their two negates and the target that silences, the triangle's sequence, its linear
  counter and the counters that stop it, the noise's shift register in both modes and its period
  table, the quarter- and half-frame clocks on those units, reset and the enables, driven through
  `apu.h` and on the machine.
- `apu_dmc_test` — the DMC's registers, reader, output unit, timer, interrupt and reset driven
  through `apu.h`; the load's third or fourth cycle, the reload's four, a write in the halt's way,
  the fetch inside and at the end of an OAM transfer and the one abandoned after its halt cycle,
  counted through the bus observer on the machine; and the APU's DMC programs above.
- `apu_mixer_test` — the mixer's level for each channel alone and together against the "APU
  Mixer" page's formulas, the cartridge's level added and held at the top, the decimator's 37 and
  38 cycles a sample, its mean, its highest rate and a rate of 0, driven through `apu.h`; on the
  machine the level a write leaves, the frames of a picture, the two drains, the frames a restore
  and a reset discard and two machines' equal frames; and the mixer programs above.
- `apu_filter_test` — the output stage's two filters a cycle at a time: the level the
  stage holds at power-on, one cycle's move of each filter, a step's rise and decay, a
  standing level settling to 0, a fall, a square wave's sag and the answer held within a
  sample, driven through `apu.h`; on the machine an idle program's silence, the stage in
  a snapshot, across a reset and at a rate of 0.

Every value they assert is read from a document named under [Sources](#sources).

## Gotchas

- **The frame interrupt is on from power-on.** The 4-step sequence sets the flag 29 828 cycles
  after its origin and every 29 830 after that, and the flag holds the IRQ line low until
  `$4015` is read or `$4017` is written with bit 6 set; a program that clears I without doing
  either is interrupted.
- **The flag is set three cycles running.** A read of `$4015` in the second or third answers
  it set, and that cycle's tick sets it again; a program polling the flag reads once more
  after it finds it.
- **A write reaches its own cycle's tick; a `$4015` read sees the previous cycle's.** The tick
  follows the cycle's access.
- **The half a write lands in moves what follows.** A `$4017` write restarts the sequence two
  cycles on from a put and three from a get, and the pulses, the noise and the DMC step on get
  cycles alone; the config's `apuPhase` seeds the half.
- **A length counter's load needs its channel's enable bit; a fourth register's other effects
  do not.** The sequencer's restart, the envelope's start flag and the linear counter's reload
  flag land on a disabled channel.
- **Enabling a channel loads nothing.** A `$4015` write with a bit set leaves that counter as
  it is; a note starts at the write to the fourth register.
- **A stopped triangle keeps its level.** With either of its counters at 0 the sequencer
  stands and `triangle.output` stays, so the mixer's level does not return to 0 when a note
  ends.
- **The DMC's level moves the triangle and the noise.** The three share one curve: the
  triangle's 15 adds 8074 over a DMC at 0 and 3508 over one at 127.
- **The output rests at 0.** The output stage takes a standing level away: a chip opens at 0
  with the mixer's level at 8074, the triangle at its first step, and a change of the DMC's
  level passes as a step that fades to 0 within 20 ms. A sample is the output's distance from
  that rest, never the DACs' level itself.
- **`apuMix` answers the DACs' level, 0 to 32 767; a sample is the output stage's, −32 768 to
  32 767.**
- **A reset silences the channels and keeps their registers.** It empties the length counters
  and stops the DMC's sample; the periods, envelopes and sweeps, the noise's shift register,
  the output stage and the decimator stay.
- **The DMC's fetch halts the CPU.** Three or four cycles a byte, on a read the CPU then
  repeats ([the machine page](nes-machine.md#oam-dma-and-the-dmcs-fetch)).

## What remains open

What the code does at each point below is fixed and tested; what the hardware does is not
settled by the documents the chip is built from.
[APU behavior and provenance](apu-behavior.md#what-remains-open) carries the documents, the
figures and what would settle each.

1. **The chip across a reset.** The "CPU power up state" page's after-reset column marks the
   channel registers "unchanged?", and blargg's `apu_reset` readme says the IRQ inhibit flag
   "is sometimes cleared"; the chip keeps the channels' registers, timers, envelopes and
   sweeps, the halt bits, the mode and the inhibit. Visual 2A03 has a reset clear an
   envelope's decay level, where the chip keeps it; a note restarts its envelope before it
   sounds, so no level depends on it.
2. **The delay at power-on and reset.** The readmes measure the sequence starting as if
   `$4017` were written 9 to 12 cycles before the first instruction; the chip takes 9, the
   figure they call usual.
3. **How long the old sequence runs after a `$4017` write.** blargg's 2005 readme leaves the
   delay undocumented; the chip steps the old sequence until the restart, two or three cycles
   after the write.
4. **The DMC's status at a sample's end.** Visual 2A03 changes the bytes remaining, the
   active bit and the interrupt flag three cycles after the fetch; blargg's programs,
   measured on a console, need them changed two cycles after it; the chip changes them at the
   fetch.
5. **The DMC where the documents are silent.** The "DMA" page's unexpected reload on the
   RP2A03H and late RP2A03G, a sample ending implicitly in the APU cycle its reload would
   schedule in, is not modelled, and the page is silent on an explicit stop there: the chip
   asks for nothing in either case. The "APU DMC" page's `$4011` write that lands with the
   output unit's clock and is "occasionally not changed properly" is a direct load here. At
   power-on the DMC's timer, an LFSR in an arbitrary state on the 2A03, reloads at the first
   get cycle.
6. **The channels at power-on, and the order inside a tick.** The "CPU power up state" page
   marks the triangle's place in its sequence at power-on unknown; the chip starts it at its
   first step, level 15, where the source of blargg's `apu_mixer` programs has it start. The
   same page has the noise's shift register `$0000` at power-on with the first clock shifting
   in a 1, and the "APU Noise" page and blargg's reference have it 1; either way the register
   holds `$4000` after its first clock, before the first instruction. Within a tick the frame
   counter's clocks run before the timers, as Visual 2A03 has them; the documents order the
   units inside a clock and leave a clock against a timer unstated.
7. **A sweep's wait on its length counter.** The "APU Pulse" page and blargg's reference list
   a sweep's conditions without the length counter and the 2A03 technical reference includes
   it; the chip follows the reference, which Visual 2A03 bears out.
8. **The earliest chips.** The "APU Noise" page gives the earliest 2A03 revisions no periodic
   mode and a last period of 2046 cycles; the chip is the later one, with both.
9. **The mixer.** The "APU Mixer" page calls its formula approximate and gives a lookup-table
   form within 4% of it; the chip evaluates the formula itself.
10. **The output stage.** The page gives the NES a third filter, a second-order low-pass above
    the audible range, which the chip omits; the circuit the page links computes to other
    corners than the page's 90 Hz and 14 kHz, which the chip takes; no document gives the
    stage's gain against a sample's scale, the sense of the DACs' pins or the charge its
    capacitors hold at power-on. The Famicom's stage, a single high-pass at 37 Hz, is not
    built.
11. **The decimator.** A sample is a mean over its CPU cycles, which lets through, attenuated,
    what lies above half the rate.
12. **The triangle at its two shortest periods.** blargg's reference has the DAC there
    "effectively" at "a value half way between 7 and 8"; the chip steps the sequencer every
    cycle or every second one, and a sample's mean approaches that value. No case measures
    it.
13. **A cartridge's sound and the other regions.** The level a cartridge adds is 0 on every
    board built, and the noise's and the DMC's tables are the NTSC columns.
14. **`volume_tests`.** Damian Yerrick's program carries a recording of an NTSC NES-001
    playing its twelve tones, and its readme states no level for them; no case compares
    against the recording.

## See also

- [APU behavior and provenance](apu-behavior.md) — where the documents are silent or disagree,
  and what settles each case.
- [The NES machine](nes-machine.md) — the cycle the chip is ticked in, the DMA unit that
  carries the DMC's fetch, and the frames a host drains.
- [The 6502 core](6502-cpu.md) — when an IRQ is taken, and the RDY line a fetch halts the CPU
  on.
- [The render tool](../tools/render/README.md) — a cartridge's sound as a WAV file.

## Where to look

- `include/neasant/apu/apu.h` — the state value with its channels' structs, the inputs and
  the eleven functions.
- `src/apu/apu.cpp` — the length, duty, triangle, noise period and DMC rate tables, the frame
  counter's steps and its restart, the quarter- and half-frame clocks, the timers and each
  channel's level, the DMC's reader and its output unit, the mixer's two curves, the output
  stage's two filters and the decimator.
- `src/nes/nes.cpp` — where the machine calls them: the write and the status read in the
  decode, the DMC's fetch in the DMA unit, the tick and the output's cycle in the machine
  cycle.
- `tests/apu/` — the five suites.

## Sources

- **NESdev wiki** — the pages "APU", "APU Frame Counter", "APU Length Counter", "APU
  Envelope", "APU Pulse", "APU Triangle", "APU Noise", "APU DMC", "APU Mixer", "DMA", "CPU",
  "CPU power up state" and "CPU interrupts", at `https://www.nesdev.org/wiki/`.
- **2A03 technical reference** — Brad Taylor, *2A03 technical reference* (2004).
- **NES APU Sound Hardware Reference** — blargg, `apu_ref.txt` (2004), "blargg's NES APU
  reference" above.
- **Visual 2A03** — the transistor-level simulation of the RP2A03 at
  `http://www.qmtpro.com/~nes/chipimages/visual2a03/`, read as a measurement of the chip: the
  half of the APU cycle the pulses' and the noise's units change in, a sweep's wait on its
  length counter, the cycle of the clocks a `$4017` write makes, and the DMC's timer, output
  unit and fetch.
- **The test programs** — blargg's APU, reset, frame-counter and interrupt programs and his
  `dmc_dma_during_read4`, `sprdma_and_dmc_dma` and `apu_mixer` programs, as the
  `christopherpow/nes-test-roms` repository at `https://github.com/christopherpow/nes-test-roms`
  collects them, with their readmes and sources, the second described in his post of
  2010-06-08, "DMA operation in APU", at `https://forums.nesdev.org/viewtopic.php?p=62690`;
  Fiskbit's `dmc_dma_start_test` and `dmc_dma_start_test_v2` with their readmes and sources,
  and Damian Yerrick's `dpcmletterbox` with its source and `volume_tests` with its readme. The
  2005 set's readme, "blargg's 2005 frame counter readme" above, and `apu_reset`'s readme are
  published measurements of the console. The shell of his `apu_mixer` programs states the
  triangle's level at power-on.
