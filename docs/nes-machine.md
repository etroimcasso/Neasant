# The NES machine

`Nes` is the console around the 6502 core: the master clock and its budget, the bus as the
console decodes it, the 2 KB of work RAM and the open bus, the PPU's register file and frame
([its own page](ppu.md)), the APU's registers, the two controller ports, the OAM DMA unit on the
CPU's RDY line, and the cartridge on its board behind the pins ([its own page](nes-cartridge.md)).
A host builds a machine from one config value, steps it a machine cycle or a master-cycle budget
at a time, holds the whole mutable machine as one state value, and is told about the bus and the
save through two observers it sets.

The machine runs a program's code against the console's bus, its timing and its interrupts, and
the CPU's self-checking test programs pass on it
([Testing against the programs](#testing-against-the-programs)). The PPU is its register file,
its memories, its frame counter and its NMI output, with no tile fetched. The APU's registers hold
the bytes written to them with nothing behind them, `$4015` answers its status bits as 0, and the
DMC does not fetch. PAL and Dendy are named by `Region` and refused at construction, so every
machine is NTSC.

The status table in the [README](../README.md) is the component's status row. The CPU core the
machine steps is on [the 6502 core's page](6502-cpu.md).

## Contents

- [Building a machine](#building-a-machine)
  - [The config value](#the-config-value)
  - [What construction refuses](#what-construction-refuses)
- [The memory map](#the-memory-map)
  - [Open bus](#open-bus)
  - [The APU's registers](#the-apus-registers)
- [Stepping and running](#stepping-and-running)
  - [A machine cycle](#a-machine-cycle)
  - [The full surface](#the-full-surface)
- [The reset line](#the-reset-line)
- [The clock](#the-clock)
- [The controller ports](#the-controller-ports)
  - [Reading a pad](#reading-a-pad)
- [OAM DMA](#oam-dma)
- [The bus observer](#the-bus-observer)
- [The save observer](#the-save-observer)
- [Snapshot and restore](#snapshot-and-restore)
  - [Restore refuses](#restore-refuses)
  - [Determinism](#determinism)
- [Testing against the programs](#testing-against-the-programs)
- [Gotchas](#gotchas)
- [What remains open](#what-remains-open)
- [See also](#see-also)
- [Where to look](#where-to-look)
- [Sources](#sources)

## Building a machine

A machine is built from one value, `NesConfig`, and is at power-on from then: the CPU's reset
sequence is pending, every RAM holds the seed, and the first seven cycles run the sequence to the
reset vector. The image is copied in, so the span need not outlive the call.

The examples on this page build their images with one helper:

```cpp
#include "neasant/nes/nes.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <vector>

// A 16 KB NROM image with 8 KB of CHR ROM: an iNES header naming mapper 0 and horizontal
// mirroring, `program` at $8000, and the NMI, reset and IRQ vectors pointing there.
std::vector<std::uint8_t> nromImage(std::initializer_list<std::uint8_t> program,
                                    bool battery = false) {
  std::vector<std::uint8_t> image{0x4E, 0x45, 0x53, 0x1A, 1, 1};
  image.resize(16 + 16384 + 8192);
  if (battery) image[6] = 0x02;
  std::size_t at = 16;
  for (const std::uint8_t byte : program) image[at++] = byte;
  for (std::size_t vector = 16 + 16384 - 6; vector < 16 + 16384; vector += 2) {
    image[vector] = 0x00;
    image[vector + 1] = 0x80;
  }
  return image;
}
```

Every later example on this page uses this `nromImage` and needs no further `#include`. A machine
over a program that jumps to itself:

```cpp
std::vector<std::uint8_t> image = nromImage({0x4C, 0x00, 0x80});  // JMP $8000
neasant::Nes machine(neasant::NesConfig{.rom = image});
// machine.state().cpu == neasant::powerOn(): the reset sequence is pending
// machine.state().master == 0
// machine.region() == neasant::Region::Ntsc
```

The region, the alignment, the I/O bus decay and the constant are the machine's fixed identity,
read back by `region()`, `alignment()`, `ppuOpenBusDecay()` and `unstableConstant()`; everything
else is state.

### The config value

Every field has a default, so `NesConfig{.rom = image}` is a whole config. Construction throws
`std::invalid_argument`, naming what is supported instead, for a region other than NTSC, an
alignment above 3, an `apuPhase` above 1, and for everything the cartridge and its board refuse.

```cpp
struct NesConfig {
  std::span<const std::uint8_t> rom{};  // an iNES or NES 2.0 image, header included
  Region region = Region::Ntsc;         // NTSC; PAL and Dendy are refused
  // The master cycles from the start of the first CPU cycle to the PPU's first dot, 0
  // to 3, which the hardware settles by chance at power-on (NESdev wiki, "PPU frame
  // timing", CPU-PPU Clock Alignment); every later dot is four master cycles on.
  std::uint8_t alignment = 0;
  // Which half of the APU cycle the first CPU cycle falls in, 0 a get and 1 a put,
  // which the hardware settles by chance at power-on (NESdev wiki, "DMA", Summary and
  // Cadence).
  std::uint8_t apuPhase = 0;
  // Whether the PPU's vertical-blank flag is set at power-on, which the hardware
  // settles by chance; the flag is usually found set (NESdev wiki, "PPU power up
  // state", Initial Register Values and the note on the VBL flag).
  bool vblankAtPowerOn = true;
  // Master cycles a bit of the PPU's I/O bus keeps the value an access drove onto it
  // before it decays to zero; 0 keeps it for ever. The hardware's time varies with the
  // chip and its warmth (NESdev forum, "Tests for ACE speedruns that rely on PPU's
  // internal open bus"); the default is the thread's reading for an NTSC NES at
  // power-on, $A00 counts of 15 CPU cycles, 38 400 CPU cycles.
  std::uint32_t ppuOpenBusDecay = 460800;
  // The constant the CPU's ANE and LXA OR into the accumulator, which the hardware leaves
  // to the chip and its conditions (64doc, "6510/8502 Undocumented Commands"; No More
  // Secrets, "'Magic Constant' group"). The default is the 2A03's measured value: blargg's
  // instr_test-v5 expects LXA #imm to leave its operand in A and X, which only $FF gives
  // (its 03-immediate source: the checksum for ATX #n is TAX's).
  std::uint8_t unstableConstant = 0xFF;
  // The pattern every RAM is filled with at power-on, repeated from its start; empty
  // fills with $00. The hardware leaves RAM unreliable at power-on (NESdev wiki, "CPU
  // power up state", RAM contents) and the PPU's memories unspecified (NESdev wiki,
  // "PPU power up state"). A palette entry keeps six bits of its byte.
  std::span<const std::uint8_t> ramSeed{};
  // Overrides on the header, each taken when set (CartridgeOverrides in cartridge.h).
  std::optional<std::uint16_t> mapper{};
  std::optional<Mirroring> mirroring{};
  std::optional<std::size_t> prgRamBytes{};  // 0 for none
  std::optional<std::size_t> chrRamBytes{};  // 0 for none
  std::optional<bool> battery{};
};
```

- `region` is NTSC, the one region built.
- `alignment` is the master cycles from the start of the first CPU cycle to the PPU's first dot,
  0 to 3, which the hardware settles by chance at power-on (NESdev wiki, "PPU frame timing",
  CPU-PPU Clock Alignment).
- `apuPhase` is which half of the APU cycle the first CPU cycle falls in, 0 a get and 1 a put,
  random at power-on on the hardware (NESdev wiki, "DMA", Summary and Cadence). It decides
  whether an OAM DMA transfer takes 513 or 514 cycles ([OAM DMA](#oam-dma)).
- `vblankAtPowerOn` is the vertical-blank flag's value at power-on, random on the hardware and
  usually found set (NESdev wiki, "PPU power up state", the note on the VBL flag).
- `ppuOpenBusDecay` is how long a bit of the PPU's I/O bus keeps a value, in master cycles, 0
  for ever. The default is a reading of an NTSC NES at power-on, which
  [the PPU page's I/O bus section](ppu.md#the-io-bus) gives with its measurement.
- `unstableConstant` is the constant `ANE` and `LXA` OR into the accumulator: `$FF` by default,
  the 2A03's measured value, where a core built on its own holds the documents' `$EE`
  ([the CPU page](6502-cpu.md#the-unofficial-opcodes)).
- `ramSeed` is the pattern every RAM is filled with at power-on, repeated from its start, empty
  for all `$00`; the hardware leaves RAM unreliable (NESdev wiki, "CPU power up state", RAM
  contents; "PPU power up state").
- The five overrides, `mapper` to `battery`, stand over the header's values, each taken when set
  ([the cartridge page's overrides](nes-cartridge.md#the-overrides)).

Two machines built from equal configs are equal values before their first cycle and after any
number of equal budgets ([Determinism](#determinism)).

### What construction refuses

Construction throws `std::invalid_argument` naming what the core does instead. The config's own
fields are checked first, then the image, then the board:

| Config | Message |
|---|---|
| `region` other than `Ntsc` | `PAL and Dendy are not built; NTSC is` |
| `alignment` above 3 | `an alignment of 4 is not one of 0 to 3` |
| `apuPhase` above 1 | `an apuPhase of 2 is not 0 (a get) or 1 (a put)` |
| an image shorter than a header | `an image of 15 bytes holds no 16-byte header` |
| an image without the identifier | `the image carries no NES header` |
| a header with a trainer | `a trainer is not supported` |
| a Vs. System, PlayChoice-10 or extended-console image | `a Vs. System image is not supported; NES and Famicom images are`, and likewise for the other two |
| an image shorter than its header's sizes | `the image is 16384 bytes short of the header's sizes` |
| a mapper the registry does not hold | `mapper 4 is not built; NROM (0) is` |
| a cartridge NROM cannot carry | the board's refusal, naming the figure ([the cartridge page](nes-cartridge.md#what-is-refused)) |

A message carries the figure the config or the image holds; the figures above are one case each.
Two of the refusals:

```cpp
std::vector<std::uint8_t> image = nromImage({0x4C, 0x00, 0x80});
try {
  neasant::Nes pal(neasant::NesConfig{.rom = image, .region = neasant::Region::Pal});
} catch (const std::invalid_argument& refusal) {
  // refusal.what() is "PAL and Dendy are not built; NTSC is"
}
try {
  neasant::Nes mmc3(neasant::NesConfig{.rom = image, .mapper = 4});
} catch (const std::invalid_argument& refusal) {
  // refusal.what() is "mapper 4 is not built; NROM (0) is"
}
```

## The memory map

The bus maps a 16-bit address the way the console does, and every read and every write resolves
through one decode:

| Address | Content |
|---|---|
| `$0000-$07FF` | the 2 KB of work RAM, `state().ram` |
| `$0800-$1FFF` | work RAM again, three times |
| `$2000-$2007` | the PPU's eight ports ([the PPU page](ppu.md#the-ports)) |
| `$2008-$3FFF` | the ports again, every eight bytes |
| `$4000-$4013` | the APU's registers: a write is held, a read answers the open bus |
| `$4014` | OAM DMA: a write starts a transfer; a read answers the open bus |
| `$4015` | the APU's status: a read answers bit 5 from the open bus and 0 elsewhere; a write is held |
| `$4016` | a write is the controllers' strobe; a read is port one |
| `$4017` | a write is held as the APU's frame counter; a read is port two |
| `$4018-$401F` | nothing on a retail console: a read answers the open bus, a write lands nowhere |
| `$4020-$FFFF` | the cartridge's board: on NROM, PRG RAM at `$6000-$7FFF` when the cartridge has any, PRG ROM at `$8000-$FFFF`, and the open bus elsewhere ([the cartridge page](nes-cartridge.md#where-an-address-lands)) |

(NESdev wiki, "CPU memory map"; "2A03", 2A03 register map.) A store through one copy of work RAM
reads back through another:

```cpp
// LDA #$5A; STA $0801; LDA $0001
std::vector<std::uint8_t> image = nromImage({0xA9, 0x5A, 0x8D, 0x01, 0x08, 0xAD, 0x01, 0x00});
neasant::Nes machine(neasant::NesConfig{.rom = image});
for (int cycle = 0; cycle < 7 + 2 + 4 + 4; ++cycle) machine.step();
// machine.state().ram[1] == 0x5A: $0801 reaches the byte $0001 reaches
// machine.state().cpu.a == 0x5A
```

### Open bus

`state().mdr` is the last byte the data bus carried, and a read nothing drives answers it. Every
read but `$4015`'s leaves its byte there, and so does every write, so an absolute read of an
undriven address answers the high byte of its own operand, the last byte read before it (NESdev
wiki, "Open bus behavior", CPU open bus and Electrical basis). `$4015` is internal to the CPU and
its read leaves the bus as it was; its bit 5 is the bus's (NESdev wiki, "APU", Status). The
controller ports drive bits 4–0 and leave bits 7–5 to the bus
([The controller ports](#the-controller-ports)).

```cpp
// LDA $4018; STA $10; LDA $5FFF; STA $11
std::vector<std::uint8_t> image =
    nromImage({0xAD, 0x18, 0x40, 0x85, 0x10, 0xAD, 0xFF, 0x5F, 0x85, 0x11});
neasant::Nes machine(neasant::NesConfig{.rom = image});
for (int cycle = 0; cycle < 7 + 4 + 3 + 4 + 3; ++cycle) machine.step();
// machine.state().ram[0x10] == 0x40: nothing is at $4018, so the read answers the last byte
//                                    read, the operand's high byte
// machine.state().ram[0x11] == 0x5F: nothing is at $5FFF on this board either
// machine.state().mdr == 0x5F: the last store left its byte on the bus
```

### The APU's registers

`state().apuRegs` holds `$4000-$4017` as last written, for the registers the APU holds:
`$4000-$4013`, `$4015` and `$4017`. The bytes at `$4014` and `$4016` are never written there,
since those addresses are the OAM DMA page and the controllers' strobe (NESdev wiki, "2A03", 2A03
register map; "APU", Registers). The bytes drive nothing: a read of `$4015` answers bit 5 from
the open bus and 0 in every other bit, where the chip reports its length counters, the DMC and
its two interrupt flags (NESdev wiki, "APU", Status). The reset line clears `$4015` and all but
the low bit of `$4011`, and keeps the rest ([The reset line](#the-reset-line)).

```cpp
// LDA #$C0; STA $4017; LDA $4015; STA $10
std::vector<std::uint8_t> image =
    nromImage({0xA9, 0xC0, 0x8D, 0x17, 0x40, 0xAD, 0x15, 0x40, 0x85, 0x10});
neasant::Nes machine(neasant::NesConfig{.rom = image});
for (int cycle = 0; cycle < 7 + 2 + 4 + 4 + 3; ++cycle) machine.step();
// machine.state().apuRegs[0x17] == 0xC0: the byte written, held
// machine.state().ram[0x10] == 0x00: the status read answers bit 5 of the open bus, $40, and 0
//                                    in every other bit
```

## Stepping and running

`step()` runs one machine cycle, which is one CPU cycle and twelve master cycles, and returns the
twelve. `run(budget)` runs whole machine cycles until the machine stands at least `budget` master
cycles past the point the previous call reported, and returns the master cycles it advanced. A
budget that ends inside a cycle finishes the cycle, and the overshoot is credited to the next
call, so `run(a)` then `run(b)` lands where `run(a + b)` does, and `run(0)` runs nothing.
`state().master` is the free-running master-cycle counter and `state().consumed` the point the
calls so far have accounted for; `master - consumed` is the overshoot the next `run()` is
credited, and `step()` leaves it as it stands.

```cpp
std::vector<std::uint8_t> image = nromImage({0x4C, 0x00, 0x80});  // JMP $8000
neasant::Nes machine(neasant::NesConfig{.rom = image});
for (int cycle = 0; cycle < 7; ++cycle) machine.step();  // the reset sequence, twelve each
// machine.state().cpu.pc == 0x8000
// machine.state().master == 84
// machine.state().ppu.dot == 21: three dots a cycle under alignment 0
const std::uint64_t spent = machine.run(100);
// spent == 108: nine whole cycles, the budget ending inside the ninth
// machine.state().master == 192 and machine.state().consumed == 184
const std::uint64_t more = machine.run(100);
// more == 96: eight cycles, the overshoot of eight credited
// machine.state().master == 288 and machine.state().consumed == 284
```

Time is the host's. A host pacing a run to the console's rate runs a frame's master cycles and
waits until the frame is owed ([The clock](#the-clock)); a host with a clock of its own spends
budgets.

### A machine cycle

Inside one cycle the order is fixed: the PPU runs the dots whose master cycle falls before M2
rises, the cycle's one bus access lands, the PPU runs the dots that fall after it, the CPU samples
the PPU's NMI output, and the counters move. M2 is low for the first 9/24 of the cycle and high
for the rest, four and a half master cycles in (NESdev wiki, "CPU pinout", M2), and the console
decodes the PPU's ports while M2 is high (NESdev wiki, "Open bus behavior", Electrical basis), so
a dot at master offset 0–4 of the cycle ticks before the access and one at 5–11 after it. The
dots fall every four master cycles from the alignment (NESdev wiki, "PPU frame timing", CPU-PPU
Clock Alignment), and the edge detector samples the NMI line in the second half of the cycle
(NESdev wiki, "CPU interrupts", Detailed interrupt behavior):

| `alignment` | Dots before the access, at master offsets | Dots after, at master offsets |
|---|---|---|
| 0 | two: 0 and 4 | one: 8 |
| 1 | one: 1 | two: 5 and 9 |
| 2 | one: 2 | two: 6 and 10 |
| 3 | one: 3 | two: 7 and 11 |

(NESdev wiki, "PPU frame timing", CPU-PPU Clock Alignment; "CPU pinout", M2.) The pattern repeats
every cycle, twelve being a multiple of four. The access is the CPU's; while the OAM DMA unit
holds the bus it is the unit's get or put, and on the unit's halt and alignment cycles the halted
CPU repeats its read ([OAM DMA](#oam-dma)).

```cpp
std::vector<std::uint8_t> image = nromImage({0x4C, 0x00, 0x80});
neasant::Nes aligned(neasant::NesConfig{.rom = image, .alignment = 3});
for (int cycle = 0; cycle < 7; ++cycle) aligned.step();
// aligned.state().ppu.dot == 21: the dots at master 3, 7, ... 83 fell inside the seven cycles
// aligned.state().dotClock == 87: the master cycle the next dot falls on
```

### The full surface

The machine's verbs, beside the config and state values:

```cpp
class Nes {
 public:
  explicit Nes(NesConfig config);                 // power-on; throws std::invalid_argument

  const NesState& state() const noexcept;         // the whole machine, by reference
  void restore(const NesState& state);            // replace it; throws for another cartridge's

  void reset();                                   // the console's reset line

  Region region() const noexcept;                 // fixed for the machine's life, like the image
  std::uint8_t alignment() const noexcept;
  std::uint32_t ppuOpenBusDecay() const noexcept;
  std::uint8_t unstableConstant() const noexcept;

  std::uint64_t step();                           // one machine cycle; returns 12
  std::uint64_t run(std::uint64_t budget);        // whole cycles to the budget; the advance

  void setJoypad(JoypadPort port, std::optional<Joypad> pad) noexcept;
  const std::optional<Joypad>& joypad(JoypadPort port) const noexcept;

  void setObserver(BusObserver* observer) noexcept;        // told every access, or none
  BusObserver* observer() const noexcept;
  void setSaveObserver(SaveObserver* observer) noexcept;   // told the save's changes, or none
  SaveObserver* saveObserver() const noexcept;
  std::size_t saveDataSize() const noexcept;               // the save's size; 0 without a battery
};
```

A machine is copied and moved whole: the state, the image, the board and the observer pointers go
with it. After construction the machine allocates nothing; a copy of its state allocates that
copy's own PRG RAM and CHR RAM.

## The reset line

`reset()` is the console's reset line pulled and let go, which resets the CPU and the PPU
together as a front-loading console's does (NESdev wiki, "PPU power up state"). The CPU's reset
sequence is pending again and runs on the next seven cycles. What a reset initializes takes its
value, and everything else keeps what it held:

| | After `reset()` |
|---|---|
| the CPU | the sequence pending: on the next seven cycles the program counter goes to the vector, S ends three lower and I is set; A, X, Y and the other flags keep their values; a jammed core is released; an instruction in progress is abandoned |
| `$4015` | 0 |
| `$4011` | its low bit kept, the rest cleared |
| every other APU register | what it held |
| the OAM DMA unit | a transfer under way ends and the CPU is released; the page latch keeps its byte |
| the PPU | `$2000`, `$2001`, the temporary address, the fine X scroll, the write toggle and the read buffer cleared; the counter at dot 0 of scanline 0 of an even frame; the warm-up window open; the vertical-blank flag, the OAM address, the current address, the I/O bus and the memories kept ([the PPU page](ppu.md#the-reset-line)) |
| work RAM, PRG RAM, CHR RAM, the open bus, the strobe, the shift registers, the pads, the master counters, the observers | what they held |

(NESdev wiki, "CPU power up state", CPU and APU; "APU", Status; "CPU pinout", /RST; "PPU power up
state".) The master counters run on, and a budget `run()` was owed is kept. Call `reset()` between
`step()` and `run()` calls, never from inside an observer's call.

```cpp
// LDA #$55; STA $4015; LDX #$12
std::vector<std::uint8_t> image = nromImage({0xA9, 0x55, 0x8D, 0x15, 0x40, 0xA2, 0x12});
neasant::Nes machine(neasant::NesConfig{.rom = image});
for (int cycle = 0; cycle < 7 + 2 + 4 + 2; ++cycle) machine.step();
// machine.state().apuRegs[0x15] == 0x55
machine.reset();
for (int cycle = 0; cycle < 7; ++cycle) machine.step();  // the sequence again
// machine.state().cpu.pc == 0x8000
// machine.state().cpu.a == 0x55 and .x == 0x12: kept
// machine.state().cpu.s == 0xFA: three lower again
// machine.state().apuRegs[0x15] == 0x00
// machine.state().master == 264: the counter runs on
```

## The clock

`ConsoleClock` carries a region's master clock as an exact ratio and the master cycles one frame
takes, and `consoleClock(Region)` answers it. `FrameDeadline` says when each frame of a run is
owed, in nanoseconds, for a host holding a run to the console's rate.

A region sets the master clock and the dividers the CPU and the PPU run at, and a machine keeps
its region for its life. NTSC is built; `Pal` and `Dendy` are named so a host can state them, and
construction refuses them. The clock is `hertzNumerator / hertzDenominator`; PAL's and Dendy's
frames are not stated and read as 0.

```cpp
enum class Region : std::uint8_t { Ntsc, Pal, Dendy };

struct ConsoleClock {
  std::uint64_t masterCyclesPerFrame = 0;
  std::uint64_t hertzNumerator = 0;
  std::uint64_t hertzDenominator = 1;
};

[[nodiscard]] constexpr ConsoleClock consoleClock(Region region) noexcept {
  switch (region) {
    case Region::Pal:
    case Region::Dendy:
      return ConsoleClock{
          .masterCyclesPerFrame = 0u, .hertzNumerator = 53203425u, .hertzDenominator = 2u};
    case Region::Ntsc:
      break;
  }
  return ConsoleClock{
      .masterCyclesPerFrame = 357368u, .hertzNumerator = 236250000u, .hertzDenominator = 11u};
}
```

`FrameDeadline` answers the nanosecond the Nth frame of a run is owed at, counted from the run's
start; the host asks when the next frame is due and waits however it waits. The frame count is
multiplied by the whole nanoseconds of a frame and by the fraction left over separately, so no
product wraps 64 bits inside a hundred billion frames, sixty years at the console's rate.

```cpp
class FrameDeadline {
 public:
  explicit constexpr FrameDeadline(Region region) noexcept
      : divisor_(consoleClock(region).hertzNumerator),
        whole_(nanosNumerator(region) / consoleClock(region).hertzNumerator),
        remainder_(nanosNumerator(region) % consoleClock(region).hertzNumerator) {}

  // The nanosecond the Nth frame of the run is owed at.
  [[nodiscard]] constexpr std::uint64_t owedAt(std::uint64_t frames) const noexcept {
    return frames * whole_ + frames * remainder_ / divisor_;
  }

  // One frame's interval in whole nanoseconds, the fraction left over dropped.
  [[nodiscard]] constexpr std::uint64_t wholeNanos() const noexcept { return whole_; }

 private:
  static constexpr std::uint64_t nanosNumerator(Region region) noexcept {
    const ConsoleClock clock = consoleClock(region);
    return 1000000000ull * clock.masterCyclesPerFrame * clock.hertzDenominator;
  }

  std::uint64_t divisor_;
  std::uint64_t whole_;
  std::uint64_t remainder_;
};
```

| Region | Master clock | Master cycles a frame |
|---|---|---|
| NTSC | 236 250 000 / 11 Hz, six times the colour subcarrier's 315 / 88 MHz | 357 368: 341 dots by 262 scanlines at four master cycles a dot, the frame with rendering disabled |
| PAL, Dendy | 53 203 425 / 2 Hz, six times 4 433 618.75 Hz | 0: not stated; the regions are refused at construction |

(NESdev wiki, "CPU", Frequencies; 2A03 technical reference, CLK; NESdev wiki, "PPU frame
timing", Even/Odd Frames and CPU-PPU Clock Alignment.)

```cpp
const neasant::ConsoleClock ntsc = neasant::consoleClock(neasant::Region::Ntsc);
// ntsc.masterCyclesPerFrame == 357368: 341 dots by 262 scanlines, four master cycles each
// ntsc.hertzNumerator == 236250000 and ntsc.hertzDenominator == 11
const neasant::FrameDeadline deadline(neasant::Region::Ntsc);
// deadline.wholeNanos() == 16639356
// deadline.owedAt(1) == 16639356 and deadline.owedAt(60) == 998361396
const neasant::ConsoleClock pal = neasant::consoleClock(neasant::Region::Pal);
// pal.hertzNumerator == 53203425 and pal.hertzDenominator == 2
// pal.masterCyclesPerFrame == 0: the frame is not stated
```

A host pacing a run calls `run(ntsc.masterCyclesPerFrame)` once a frame and waits on its own
clock until `deadline.owedAt(frames)` from the run's start; the core reads no clock and never
waits. A frame with rendering enabled is four master cycles shorter every other frame
([the PPU page](ppu.md#the-odd-frame-skip)). The frame figure here is the budget a host spends,
and the save observer's report follows the PPU's own frame, whichever length it had.

## The controller ports

A controller is a value, `Joypad`: the eight buttons of a standard pad, each a `bool`. A port
holds one or nothing. The machine starts with both ports empty; `setJoypad` plugs a pad in or
pulls it out, and `joypad` reads back what a port holds. The pad is part of the state.

`Button` names the eight in the order the pad shifts its bits out (NESdev wiki, "Standard
controller", Report). The set is the controller's, so it is fixed: a controller with other buttons
is a different controller. `buttons()` walks the eight in that order, and `buttonName` and
`buttonFromName` are the one place they are spelled, a name read in any case, so a tool that
reads a button out of a file and a host that prints one cannot disagree. `buttonName` answers an
empty view for a value past the last button, and `buttonFromName` nothing for a word that names
none.

```cpp
enum class Button : std::uint8_t { A, B, Select, Start, Up, Down, Left, Right };
inline constexpr std::size_t kButtonCount = 8;

[[nodiscard]] std::span<const Button> buttons() noexcept;

[[nodiscard]] std::string_view buttonName(Button button) noexcept;
[[nodiscard]] std::optional<Button> buttonFromName(std::string_view name) noexcept;
```

`Joypad` reads and writes one button by its `Button` through `holds` and `hold`, and `bits()`
answers the eight bits the pad shifts out, bit 0 A up to bit 7 Right, a set bit a pressed button
(NESdev wiki, "Controller reading", `$4016` / `$4017` Read). A pad with nothing pressed is a
controller in the socket; an empty optional is a socket with nothing in it, and a program can
tell the two apart ([Reading a pad](#reading-a-pad)). `JoypadPort` names the two ports on the
console's front.

```cpp
struct Joypad {
  bool a = false;
  bool b = false;
  bool select = false;
  bool start = false;
  bool up = false;
  bool down = false;
  bool left = false;
  bool right = false;

  // One button by name, read and written, so a caller working from the Button set
  // does not repeat the eight fields to reach them.
  [[nodiscard]] bool holds(Button button) const noexcept;
  void hold(Button button, bool pressed) noexcept;

  // The eight bits the pad shifts out: bit 0 is A, the first bit on the line, up to
  // bit 7 for Right. A set bit is a pressed button, which is how the port reads it
  // (NESdev wiki, "Controller reading", $4016 / $4017 Read).
  [[nodiscard]] std::uint8_t bits() const noexcept;

  [[nodiscard]] bool operator==(const Joypad&) const noexcept = default;
};

enum class JoypadPort : std::uint8_t { One, Two };
```

A pad as a value:

```cpp
neasant::Joypad pad{.a = true, .right = true};
// pad.bits() == 0x81: bit 0 is A and bit 7 Right, the order the pad shifts them out
// pad.holds(neasant::Button::A) == true
pad.hold(neasant::Button::Right, false);
// pad == neasant::Joypad{.a = true}
// neasant::buttonName(neasant::Button::Select) == "select"
// neasant::buttonFromName("Start") == neasant::Button::Start
// neasant::buttons().size() == 8, in the same order
```

### Reading a pad

A program reads a pad through its port's register, one button a read:

- A write to `$4016` latches its bit 0 as the strobe. While the strobe is high each pad's shift
  register fills from the buttons held, and the write that drops it leaves the buttons held at
  that moment (NESdev wiki, "Controller reading", `$4016` Write; "Standard controller", Input
  ($4016 write)).
- A read of `$4016` answers port one's register and a read of `$4017` port two's, bit 0 first: A,
  then B, Select, Start, Up, Down, Left, Right, a 1 for a pressed button (NESdev wiki, "Standard
  controller", Report and Output ($4016/$4017 read); "Controller reading", `$4016` / `$4017`
  Read).
- The port's clock line is low through a read of its register and stays low across adjacent
  cycles that read it again, so one contiguous run of reads is one clock; it rises on the first
  access that is not such a read, and the pad then shifts, a 1 coming in at the top. From the
  ninth read on, the pad answers 1 (NESdev wiki, "Controller reading", Clock timing; "DMA",
  Register conflicts; "Standard controller", Hardware).
- An empty socket answers 0 on every read; a pad with nothing pressed answers 0 eight times and 1
  from then on, which is how a program tells the two apart (NESdev wiki, "Standard controller",
  Output ($4016/$4017 read); "Controller reading", Unconnected data lines and open bus).
- Bits 4–1 of a read are lines nothing drives on a front-loading console's ports, and read 0;
  bits 7–5 are the open bus, so a read usually answers `$40` or `$41` (NESdev wiki, "Controller
  reading", Unconnected data lines and open bus; "Open bus behavior", CPU open bus).
- The program sees a pad the host set when the port's register next fills: at a write to `$4016`
  that raises the strobe, keeps it high or drops it, and at every read and every clock of the
  port while the strobe is high. Set the pad before the frame that reads it.

The registers are in the state. `state().strobe` is `$4016` bit 0 as last written. `state().shift`
holds each port's shift register: bit 0 is the bit a read answers, each clock moves the next bit
down and a 1 in at the top, and an empty socket's register holds 0. `state().clockLow` says, per
port, whether its clock line is low, which it is when a read of the port's register was the
previous cycle's access.

A program that raises and drops the strobe, then reads port one eight times:

```cpp
// LDA #$01; STA $4016; LDA #$00; STA $4016: the strobe raised and dropped; then eight
// times LDA $4016; STA $10 up to STA $17
std::vector<std::uint8_t> image = nromImage({
    0xA9, 0x01, 0x8D, 0x16, 0x40, 0xA9, 0x00, 0x8D, 0x16, 0x40,
    0xAD, 0x16, 0x40, 0x85, 0x10, 0xAD, 0x16, 0x40, 0x85, 0x11,
    0xAD, 0x16, 0x40, 0x85, 0x12, 0xAD, 0x16, 0x40, 0x85, 0x13,
    0xAD, 0x16, 0x40, 0x85, 0x14, 0xAD, 0x16, 0x40, 0x85, 0x15,
    0xAD, 0x16, 0x40, 0x85, 0x16, 0xAD, 0x16, 0x40, 0x85, 0x17,
});
neasant::Nes machine(neasant::NesConfig{.rom = image});
machine.setJoypad(neasant::JoypadPort::One,
                  neasant::Joypad{.a = true, .select = true, .up = true, .left = true});
for (int cycle = 0; cycle < 7 + 12 + 8 * 7; ++cycle) machine.step();
// machine.state().ram[0x10] == 0x41: A pressed, over the operand's high byte $40
// machine.state().ram[0x11] == 0x40: B
// machine.state().ram[0x12] == 0x41: Select; then Start, Up, Down, Left and Right at $13-$17
// machine.state().ram[0x17] == 0x40: Right
// machine.state().shift[0] == 0xFF: eight clocks have shifted eight ones in
// machine.joypad(neasant::JoypadPort::Two).has_value() == false: an empty socket
```

## OAM DMA

A write to `$4014` names a page of the CPU's address space, and the OAM DMA unit copies its 256
bytes to the PPU's object attribute memory through `$2004`. The unit halts the CPU through its RDY
line on the first cycle after the write, which the CPU allows on a read cycle only: a write
proceeds and the unit tries again on the next cycle. Then, on each get cycle, the unit reads a
byte of the page, and on the put cycle after it writes the byte to `$2004`; a put cycle before
the first read is an alignment cycle. 256 pairs take 512 cycles, the halt one more, and the
alignment cycle one more when the halt lands on a get cycle: 513 or 514 cycles in all, by the
half of the APU cycle the halt falls on, which `apuPhase` seeds. On the halt and alignment cycles
the halted CPU repeats the read it was halted on, and the read reaches the bus; when the transfer
ends the CPU makes that read for real (NESdev wiki, "DMA", Summary, Cadence, Behavior, OAM DMA
and Register conflicts). A read-modify-write of `$4014` writes twice before the halt lands, and
the copy is from the second page written (NESdev wiki, "DMA", OAM DMA).

`DmaStage` is where the unit is in a transfer, and `Dma` is the unit's state:

```cpp
enum class DmaStage : std::uint8_t {
  Idle,  // no transfer
  Halt,  // a write to $4014 landed: RDY is held low until a read cycle halts the CPU
  Get,   // the next get cycle reads a byte of the page; a put cycle before it is an alignment cycle
  Put,   // this cycle writes the byte read to $2004
};

struct Dma {
  // The page a transfer copies from: the byte most recently written to $4014 (NESdev
  // wiki, "DMA", OAM DMA).
  std::uint8_t page = 0;
  DmaStage stage = DmaStage::Idle;
  std::uint8_t index = 0;  // the byte of the page the next get reads
  std::uint8_t byte = 0;   // the byte the last get read, carried to its put
  // Whether the DMC has a sample byte to fetch, whose get takes precedence over the OAM
  // unit's and pauses it (NESdev wiki, "DMA", DMC DMA during OAM DMA). The DMC is not
  // built, so nothing sets it.
  bool dmcRequested = false;
  [[nodiscard]] bool operator==(const Dma&) const noexcept = default;
};
```

| Stage on entry to a cycle | The cycle | Stage after |
|---|---|---|
| `Idle` | the CPU's | `Idle` |
| `Halt`, set by the write to `$4014` with the page | RDY low, then the CPU's cycle: a write proceeds; a read is the halt cycle, repeated | `Get` once a read was held, else `Halt` |
| `Get` on a put cycle | an alignment cycle: the halted read, repeated | `Get` |
| `Get` on a get cycle | the unit reads the page's next byte | `Put` |
| `Put` | the unit writes the byte to `$2004` | `Get`, or `Idle` after the 256th with RDY raised |

(NESdev wiki, "DMA", Behavior, Cadence and OAM DMA.) The unit's accesses go through the bus as the
CPU's would, in the unit's name, so a bus observer is told them with `AccessSource::OamDma`
([The bus observer](#the-bus-observer)). The NMI line is sampled at the end of every cycle, so an
edge during a transfer is taken after the halted instruction, and `reset()` ends a transfer under
way. `Dma::dmcRequested` and `AccessSource::DmcDma` are declared and nothing drives them: the
DMC's fetch and its precedence over this unit (NESdev wiki, "DMA", DMC DMA during OAM DMA) are
the APU's, which is not built.

```cpp
// LDA #$02; STA $4014; NOP
std::vector<std::uint8_t> image = nromImage({0xA9, 0x02, 0x8D, 0x14, 0x40, 0xEA});
neasant::Nes machine(neasant::NesConfig{.rom = image});
neasant::NesState filled = machine.state();
for (std::size_t i = 0; i < 256; ++i) filled.ram[0x0200 + i] = static_cast<std::uint8_t>(i);
machine.restore(filled);  // page 2 of work RAM holds 0, 1, 2 ... 255
for (int cycle = 0; cycle < 7 + 2 + 4; ++cycle) machine.step();  // through the write to $4014
// machine.state().dma.page == 0x02 and machine.state().dma.stage == neasant::DmaStage::Halt
machine.step();  // the halt cycle: the NOP's opcode fetch, repeated while the CPU is held
// machine.state().cpu.rdyHeld == true and machine.state().dma.stage == neasant::DmaStage::Get
for (int cycle = 0; cycle < 512; ++cycle) machine.step();  // 256 get/put pairs
// machine.state().dma.stage == neasant::DmaStage::Idle
// machine.state().ppu.oam[0] == 0x00 and machine.state().ppu.oam[255] == 0xFF
// machine.state().master == 6312: 526 cycles, 513 of them with the CPU held
machine.step();  // the opcode fetch, made for real
// machine.state().cpu.pc == 0x8006
```

## The bus observer

`state()` says what the machine holds after a step; an observer says what the machine did to get
there. It is told every access that crossed the bus, in order, one a machine cycle, with its
address, the byte that crossed (what the bus answered for a read, what the source drove for a
write), its direction, the cycle kind the CPU core drove it with
([the CPU page](6502-cpu.md#the-bus)) and who made it. A host that needs none of that never pays
for it: with no observer set an access costs one test, and the machine runs exactly as it does
with one.

`AccessSource` names who made an access, `BusAccess` is one access as the machine made it, and
`BusObserver` is the class a host derives from to be told them:

```cpp
enum class AccessSource : std::uint8_t { Cpu, OamDma, DmcDma };

struct BusAccess {
  std::uint16_t address = 0;
  std::uint8_t value = 0;
  bool write = false;
  CycleKind kind = CycleKind::DataRead;
  AccessSource source = AccessSource::Cpu;
  [[nodiscard]] bool operator==(const BusAccess&) const noexcept = default;
};

class BusObserver {
 public:
  virtual ~BusObserver() = default;

  // An access that crossed the bus, after its value is settled: a register's read side
  // effect has happened when the read is told. Inside the report state() holds the
  // memories and the registers as the access left them and the CPU as the cycle began.
  // The report is made inside the machine cycle, between the access and the dots that
  // follow it, so it does not throw: a throw would leave the cycle half run.
  virtual void access(const BusAccess& access) = 0;
};
```

| `AccessSource` | Who |
|---|---|
| `Cpu` | the core, every cycle of its own, the cycles it is halted on included: a read it repeats carries `CycleKind::Halted` |
| `OamDma` | the OAM DMA unit's gets and puts, plain data reads and writes |
| `DmcDma` | the DMC's sample fetch; the DMC is not built, so no access carries it |

(NESdev wiki, "DMA", Behavior.) The report is made inside the cycle, after the access settles and
before the dots that follow it, so a register's read side effect has happened when the read is
told, and `state()` inside the report holds the memories and the registers as the access left
them and the CPU as the cycle began. The report does not throw. The observer is the host's
object, not part of the state: a snapshot does not carry it, `restore()` leaves it in place, a
copied machine points at the same one, and it outlives every step it is set for. `observer()`
reads back what is set, and the machine starts with none.

```cpp
struct Log final : neasant::BusObserver {
  std::vector<neasant::BusAccess> accesses;
  void access(const neasant::BusAccess& access) override { accesses.push_back(access); }
};

// LDA #$5A; STA $0200
std::vector<std::uint8_t> image = nromImage({0xA9, 0x5A, 0x8D, 0x00, 0x02});
neasant::Nes machine(neasant::NesConfig{.rom = image});
Log log;
machine.setObserver(&log);
for (int cycle = 0; cycle < 7 + 2 + 4; ++cycle) machine.step();
// log.accesses.size() == 13: one access a cycle
// log.accesses[0]  is {$0000, $00, a read, DummyRead, Cpu}: the reset sequence's first cycle
// log.accesses[7]  is {$8000, $A9, a read, OpcodeFetch, Cpu}
// log.accesses[12] is {$0200, $5A, a write, DataWrite, Cpu}
machine.setObserver(nullptr);
```

## The save observer

A cartridge with a battery keeps what it writes in its PRG RAM, which the machine holds as
`state().prgRam`; `saveDataSize()` is the PRG RAM's size on a cartridge with a battery and 0 on
one without. The save observer says when the save changed, so a host keeping it does not compare
the window against a copy of its own. It is told at the end of the machine cycle in which a frame
ended, when the save differs from the bytes the machine last handed a save observer, or from the
save as the machine was built when none has been handed yet, however many stores the frame made;
it is not told while the bytes are as last handed. A `restore()` that leaves the save other than
as last handed tells it once. The span is the whole save, the machine's own storage, valid for
the call.

```cpp
class SaveObserver {
 public:
  virtual ~SaveObserver() = default;

  // The save differs from the bytes the machine last handed a save observer, or from the
  // save as the machine was built when none has been handed yet, and `save` is the whole
  // of it as it now stands, the size the cartridge declares. Told at the end of the
  // machine cycle in which a frame ended, however many stores the frame made, and once
  // by a restore() that leaves the save other than as last handed; not at all while the
  // bytes are as last handed. The span is the machine's own storage and is valid for
  // the call.
  //
  // A report arrives from inside step() or run(), after the cycle has closed and with
  // state() coherent at its end, or from inside restore(), with the state in place. It
  // may throw: the call returns through the throw with the machine so, and the report
  // is not made again. A run() interrupted this way has accounted for its whole
  // budget, so the next run() spends the remainder before its own and run(a) then
  // run(b) still lands where run(a + b) does.
  virtual void changed(std::span<const std::uint8_t> save) = 0;
};
```

The report may throw. `step()` or `run()` then returns through the throw with the machine at the
cycle's end and `state()` coherent there, and the report is not made again. A `run()` interrupted
this way has accounted for its whole budget, so the next `run()` spends the remainder first, and
`run(a)` then `run(b)` still lands where `run(a + b)` does. Otherwise the save observer is on the
bus observer's terms: the host's object, outside the state, read back by `saveObserver()`, none
at the start. An observer set after the program has run is told, at the next frame's end, of
everything stored since the last report, or since power-on when none has been made. Where a save
is kept is the host's.

```cpp
struct Keeper final : neasant::SaveObserver {
  std::vector<std::uint8_t> save;
  int reports = 0;
  void changed(std::span<const std::uint8_t> bytes) override {
    save.assign(bytes.begin(), bytes.end());
    ++reports;
  }
};

// LDA #$33; STA $6000; JMP $8005
std::vector<std::uint8_t> image =
    nromImage({0xA9, 0x33, 0x8D, 0x00, 0x60, 0x4C, 0x05, 0x80}, true);
neasant::Nes machine(neasant::NesConfig{.rom = image});
// machine.saveDataSize() == 8192: the cartridge has a battery
Keeper keeper;
machine.setSaveObserver(&keeper);
const std::uint64_t frame = neasant::consoleClock(neasant::Region::Ntsc).masterCyclesPerFrame;
const std::uint64_t first = machine.run(frame);
// first == 357372: the frame's end falls inside the cycle that closes there
// keeper.reports == 1 and keeper.save[0] == 0x33
const std::uint64_t second = machine.run(frame);
// second == 357372 and keeper.reports == 1: nothing changed in the second frame
```

## Snapshot and restore

The whole mutable machine is one value, `NesState`. `state()` answers it by reference, coherent
between calls at any cycle, mid-instruction included; `restore()` replaces it and resumes exactly
there. The image, the region, the alignment, the decay and the constant are the machine's fixed
identity and are not in the state, and the observers are the host's and are not in it either.

```cpp
struct NesState {
  Cpu6502State cpu{};
  // The 2 KB of work RAM at $0000-$07FF, repeated through $1FFF.
  std::array<std::uint8_t, 2048> ram{};
  // The last byte the CPU's data bus carried, which a read nothing drives answers
  // (NESdev wiki, "Open bus behavior", CPU open bus).
  std::uint8_t mdr = 0;
  std::uint64_t master = 0;  // the free-running master-cycle counter
  // The master-cycle point the calls so far have accounted for: run() moves it on by
  // its budget as the call begins and step() by the twelve master cycles it ran. master
  // - consumed is the overshoot the next run() is credited; inside a run(), and after
  // one a save report threw out of, consumed stands ahead of master by what the budget
  // still owes.
  std::uint64_t consumed = 0;
  // The master cycle the PPU's next dot falls on: the alignment at power-on, then four
  // more for every dot (NESdev wiki, "PPU frame timing", CPU-PPU Clock Alignment: the
  // PPU divides the master clock by four, and a CPU cycle begins 0 to 3 master cycles
  // before its nearest following dot).
  std::uint64_t dotClock = 0;
  // Which half of the APU cycle the next machine cycle is, 0 a get and 1 a put; it
  // toggles every machine cycle (NESdev wiki, "DMA", Cadence).
  std::uint8_t apuPhase = 0;
  // $4000-$4017 as last written, for the registers the APU holds: $4000-$4013, $4015
  // and $4017. The bytes at $4014 and $4016 are never written: those addresses are the
  // OAM DMA page and the controller strobe, held below.
  std::array<std::uint8_t, 24> apuRegs{};
  PpuState ppu{};
  // The cartridge's PRG RAM and CHR RAM, each sized at construction from the header or
  // the config and never resized; the PRG RAM is the save when the cartridge has a
  // battery.
  std::vector<std::uint8_t> prgRam{};
  std::vector<std::uint8_t> chrRam{};
  Board board{};  // the board's own state
  std::array<std::optional<Joypad>, 2> pads{};  // what is plugged into each port
  // $4016 bit 0 as last written: the controllers' strobe line (NESdev wiki, "Controller
  // reading", $4016 Write). While it is high each pad's shift register fills from the
  // buttons held, and the write that drops it leaves the buttons held at that moment.
  bool strobe = false;
  // Per port, the pad's shift register: bit 0 is the bit a read of the port's register
  // answers, and each clock of the port moves the next bit down and a 1 in at the top,
  // so from the ninth read on the pad answers 1 (NESdev wiki, "Standard controller",
  // Report and Hardware; "Controller reading", Clock timing). An empty socket's register
  // holds 0.
  std::array<std::uint8_t, 2> shift{};
  // Per port, whether its clock line is low: a read of the port's register was the
  // previous cycle's access. The line is low through a read of the register and stays
  // low across adjacent cycles that read it again, so one contiguous run of reads is
  // one clock; it rises, and the pad shifts, on the first access that is not such a
  // read (NESdev wiki, "Controller reading", Clock timing; "DMA", Register conflicts).
  std::array<bool, 2> clockLow{};
  Dma dma{};
  [[nodiscard]] bool operator==(const NesState&) const noexcept = default;
};
```

A snapshot taken inside an instruction, restored into the same machine and into another:

```cpp
std::vector<std::uint8_t> image = nromImage({0x4C, 0x00, 0x80});  // JMP $8000
neasant::Nes machine(neasant::NesConfig{.rom = image});
machine.run(90);  // the reset sequence and the jump's first cycle
const neasant::NesState saved = machine.state();
// saved.cpu.tcu == 1: inside an instruction
machine.run(5000);
machine.restore(saved);
// machine.state() == saved
neasant::Nes other(neasant::NesConfig{.rom = image});
other.restore(saved);
machine.run(1000);
other.run(1000);
// machine.state() == other.state()
```

A snapshot carries the PRG RAM and CHR RAM as vectors, so copying a state allocates; the machine
itself allocates nothing after construction. `operator==` on `NesState` is defaulted and compares
the whole machine.

### Restore refuses

The PRG RAM's size, the CHR RAM's size and the board's kind are the cartridge's, fixed for the
machine's life, so a state that differs in one is a snapshot of another cartridge. `restore()`
throws `std::invalid_argument` for it and leaves the machine as it was. The three messages are
`a snapshot of another cartridge: its PRG RAM is 0 bytes and this machine's is 8192`,
`a snapshot of another cartridge: its CHR RAM is 16 bytes and this machine's is 0` and
`a snapshot of another cartridge: its board is not this machine's`, each carrying the sizes it
compared.

```cpp
std::vector<std::uint8_t> image = nromImage({0xEA});
neasant::Nes machine(neasant::NesConfig{.rom = image});
neasant::NesState other = machine.state();
other.prgRam.clear();
try {
  machine.restore(other);
} catch (const std::invalid_argument& refusal) {
  // refusal.what() is "a snapshot of another cartridge: its PRG RAM is 0 bytes and this
  // machine's is 8192", and the machine is as it was
}
```

### Determinism

The machine reads no clock and starts no thread. Two machines built from equal configs and fed
equal traces produce equal states at every budget boundary, equal bus streams access by access,
and equal save reports. The trace is everything that reaches the machine from outside: the pads
set before each run, the states restored. One machine run a frame at a time and one stepped and
then run, each handed the same pads:

```cpp
// LDA #$01; STA $4016; LDA #$00; STA $4016; LDA $4016; STA $10; JMP $8000
std::vector<std::uint8_t> image = nromImage({0xA9, 0x01, 0x8D, 0x16, 0x40, 0xA9, 0x00, 0x8D,
                                             0x16, 0x40, 0xAD, 0x16, 0x40, 0x85, 0x10, 0x4C,
                                             0x00, 0x80});
neasant::Nes a(neasant::NesConfig{.rom = image});
neasant::Nes b(neasant::NesConfig{.rom = image});
const std::uint64_t frame = neasant::consoleClock(neasant::Region::Ntsc).masterCyclesPerFrame;
for (int n = 0; n < 3; ++n) {
  const neasant::Joypad pad{.a = (n % 2) == 0, .start = true};
  a.setJoypad(neasant::JoypadPort::One, pad);
  b.setJoypad(neasant::JoypadPort::One, pad);
  a.run(frame);
  for (int cycle = 0; cycle < 1000; ++cycle) b.step();
  b.run(frame - 12000);
  // a.state() == b.state() after every frame
}
```

## Testing against the programs

The machine runs the CPU's self-checking test programs, blargg's eight suites as the
`christopherpow/nes-test-roms` repository collects them: `instr_test-v5`, `instr_timing`,
`instr_misc`, `cpu_dummy_reads`, `cpu_dummy_writes`, `cpu_exec_space`, `cpu_reset` and
`branch_timing_tests`, 36 ROM files. Each is booted on a machine built with the config's defaults
and run one frame at a time until it reports. The newer programs report a status byte at `$6000`
with a signature at `$6001-$6003` and their text from `$6004`, read out of `state().prgRam`; the
two older suites, `cpu_dummy_reads` and `branch_timing_tests`, print their result on the screen,
read out of `state().ppu.ciram`. A program that asks for the reset button gets the reset line
seven frames later.

29 of the 36 pass. The other seven register and skip, naming what they need: the APU's length
counter (`1-instr_timing`, `2-branch_timing`), its frame interrupt flag (`04-dummy_reads_apu`),
and mapper 1 (the four multi-test builds, whose sub-tests run from the singles).
`cpu_dummy_reads` names mapper 3 with one bank and runs as NROM under the config's mapper
override. `test_cpu_exec_space_apu` passes with the cartridge range from `$4020` floating as NROM
leaves it.

The repository is reference data and is not vendored. Point the build at a local clone:

```
cmake -B build -DNEASANT_NES_TEST_ROMS=/path/to/nes-test-roms
cmake --build build
ctest --test-dir build
```

Without that path the 36 program cases register and skip, naming the option, and the case on the
constant runs regardless; with the path and a file absent, the case skips naming the file.
`NEASANT_REQUIRE_NES_TEST_ROMS=1` turns either skip into a failure, so an environment that means
to run the programs cannot report green without them.

The hand-derived suites, by executable:

- `nes_cartridge_test` — both header formats byte by byte, the cartridge value and its
  overrides, every refusal by its message, and NROM on the pins.
- `nes_machine_test` — construction and its refusals, the reset sequence, the budget and its
  overshoot, the bus decode with open bus, the reset line, snapshot and restore, and the clock
  values.
- `ppu_registers_test` — the ports, the scroll file, `$2007`, the palette, OAM, the warm-up, the
  frame, the NMI, the `$2002` race by alignment, and the bus decay.
- `nes_input_dma_test` — the buttons, the strobe, the pads read bit by bit, the clock line, open
  bus, and OAM DMA at 513 and 514 cycles.
- `nes_observers_test` — every access told with its kind, byte and source, the save report per
  frame and per restore, and two machines equal every frame and access by access.
- `nes_blargg_cpu_test` — the programs above, and the constant the machine builds its CPU with.

Every value they assert is read from a document named under [Sources](#sources).

## Gotchas

- **`run()`'s budget is in master cycles.** A CPU cycle is twelve, a PPU dot four, a frame
  357 368; `step()` is one CPU cycle, not an instruction.
- **The first seven cycles are the reset sequence.** A fresh machine's program counter reads
  `$0000` until they run; the vectors are read on the sixth and seventh.
- **A snapshot inside an instruction is whole.** `state().cpu.tcu` says how far in; a restore
  resumes on that cycle.
- **An undriven read answers the last byte on the bus.** `$4000-$4014`, `$4018-$401F` and an
  address the board leaves alone answer the operand's high byte on an absolute read.
- **The APU is its registers.** `$4000-$4013`, `$4015` and `$4017` hold their bytes; a read of
  `$4015` answers 0 but for bit 5; nothing sounds and no APU interrupt fires.
- **Set the pad before the frame that reads it.** A pad changed after the strobe's fall is seen
  when the register next fills.
- **An empty port and a pad with nothing pressed differ.** 0 on every read, against 0 eight times
  and 1 from the ninth.
- **A write to `$4014` holds the CPU for 513 or 514 cycles.** `apuPhase` decides which; a
  read-modify-write of `$4014` copies the second page it writes.
- **The observers are not state.** A restore leaves them; a copied machine shares them; a
  snapshot restored into another machine runs with that machine's.
- **A save report may throw, and the budget is owed.** The next `run()` spends what the throw
  interrupted before its own.
- **`restore()` throws for another cartridge's snapshot.** A host restoring bytes from elsewhere
  catches it; the machine is unchanged.
- **`reset()` between calls.** Never from inside an observer's call, which arrives part-way
  through a cycle.

## What remains open

What the code does at each point below is fixed and tested; what the hardware does is not settled
by the documents the machine is built from.

1. **The APU's registers across a reset.** The "CPU power up state" page's after-reset column
   marks the pulse, triangle and noise registers "unchanged?"; the machine keeps every byte but
   `$4015`'s and the high bits of `$4011`, as the column's other cells say.
2. **PAL and Dendy.** The "CPU" page gives their master clock; no page the machine is built from
   gives their frame, so `consoleClock` answers a frame of 0 for both and `FrameDeadline` owes
   every frame at 0. Construction refuses both regions.
3. **A write's byte on the open bus.** The "Open bus behavior" page names the last byte read as
   what an undriven read answers, and its electrical account has any byte placed on the bus float
   there; the machine keeps a write's byte too. From the CPU's side the two cannot be told apart,
   since a read always follows a write before an undriven read.
4. **The 2A03's register decode while the CPU is halted.** The "DMA" page has the 2A03 decode bits
   4–0 from its own address bus and bits 15–5 from the core's; the machine decodes the OAM DMA
   unit's address whole. The case arises only for a program executing from `$4000-$401F`, since
   OAM DMA's halt lands on an opcode fetch; the DMC, which halts anywhere, is not built.
5. **`$4018-$401F`.** The "2A03" page names the range's test mode and its unfinished timer; the
   machine answers the open bus to a read and lands no write there.
6. **The console's clock line for the pads.** The "DMA" page has the RF Famicom clock a pad on
   every read cycle, and the NES-001 and AV Famicom once per contiguous run of reads; the machine
   is the NES-001's. The 2A07's fix of the DPCM double clock, on the "Controller reading" page, is
   the PAL CPU's.
7. **The strobe's other bits.** `$4016` bits 2–1 drive the expansion port ("Controller reading",
   `$4016` Write; "CPU pinout", OUT0–OUT2); the machine keeps bit 0 alone.
8. **The constant `ANE` and `LXA` OR into A on the 2A03.** The one measurement is a checksum in
   blargg's instruction test, which fixes `$FF` for `LXA` on the unit it was generated on; the
   suite does not test `ANE`, and no document the machine is built from states the 2A03's value
   for either. The config states `$FF` for both.
9. **The DMC.** `AccessSource::DmcDma` and `Dma::dmcRequested` are declared and nothing drives
   them: the DMC's fetch, its precedence over OAM DMA ("DMA", DMC DMA during OAM DMA) and the
   double clock it gives a pad ("Controller reading", DPCM conflict) are the APU's, which is not
   built.

## See also

- [The 6502 core](6502-cpu.md) — the instruction set the machine steps, and the cycle kinds every
  access carries.
- [The cartridge](nes-cartridge.md) — the header, the pins and the board the bus reaches at
  `$4020-$FFFF`.
- [The PPU](ppu.md) — the ports at `$2000-$2007`, the frame and the NMI.

## Where to look

- `include/neasant/nes/nes.h` — the config value, the state value, the pads, the observers and
  `Nes`.
- `include/neasant/nes/clock.h` — `Region`, `ConsoleClock` and `FrameDeadline`.
- `src/nes/nes.cpp` — the button names, construction, the decode, the bus with open bus, the
  controller ports' clock lines, the OAM DMA unit, the machine cycle, `step` and `run`, `reset`
  and `restore`.
- `tests/nes/` — the five hand-derived suites, the authored-image helper, and the test-ROM
  harness with its suite.

## Sources

- **NESdev wiki** — the pages "CPU memory map", "2A03", "Open bus behavior", "CPU", "CPU power up
  state", "CPU pinout", "CPU interrupts", "DMA", "APU", "Controller reading", "Standard
  controller", "PPU frame timing" and "PPU power up state", at `https://www.nesdev.org/wiki/`.
- **2A03 technical reference** — Brad Taylor, *2A03 technical reference* (2004).
- **64doc** and **No More Secrets** — as the [CPU page](6502-cpu.md#sources) names them, for the
  constant `ANE` and `LXA` OR into A.
- **The test programs** — blargg's CPU suites as the `christopherpow/nes-test-roms` repository at
  `https://github.com/christopherpow/nes-test-roms` collects them, with their readmes and sources;
  the reference data the program suite runs.
