# The PPU

The 2C02 is the NES's picture processor, the chip a program reaches at `$2000`–`$2007`. This is
its register file over a state value: the eight ports with every latch a write passes through and
the value and side effect of every read, the I/O bus behind them and its decay, the scroll file
the ports fill, the port at `$2007` over the chip's own 14-bit address space, the frame counter
that runs 341 dots by 262 scanlines, the vertical-blank flag and the NMI output it drives, the
warm-up window after power-on and reset, and the memories the console and the chip hold:
nametable RAM, palette RAM and object attribute memory.

The register file is what a program reads and writes, and the frame counter is what its timing
stands on. The picture is absent: no tile is fetched and no sprite is evaluated, and a program
that enables rendering gets the frame counter's odd-frame skip and nothing else. The chip's state
is one value inside [the machine's](nes-machine.md), `state().ppu`, and the machine runs its dots.

The status table in the [README](../README.md) is the component's status row.

## Contents

- [The surface](#the-surface)
- [The state value](#the-state-value)
- [The ports](#the-ports)
  - [A bare chip](#a-bare-chip)
  - [$2000 and $2001](#2000-and-2001)
  - [$2002](#2002)
  - [$2003 and $2004](#2003-and-2004)
  - [$2005 and $2006](#2005-and-2006)
  - [$2007](#2007)
- [The I/O bus](#the-io-bus)
  - [Its decay](#its-decay)
- [The address space](#the-address-space)
  - [The palette](#the-palette)
- [The frame](#the-frame)
  - [The odd-frame skip](#the-odd-frame-skip)
  - [The machine's frame](#the-machines-frame)
- [The NMI output](#the-nmi-output)
  - [The interrupt](#the-interrupt)
  - [The $2002 race](#the-2002-race)
- [Power-on, reset and the warm-up](#power-on-reset-and-the-warm-up)
  - [The reset line](#the-reset-line)
- [Reading a register without its effect](#reading-a-register-without-its-effect)
- [Gotchas](#gotchas)
- [What remains open](#what-remains-open)
- [See also](#see-also)
- [Where to look](#where-to-look)
- [Sources](#sources)

## The surface

The chip's state is one value, `PpuState`, held inside the machine's state as `NesState::ppu`; a
snapshot of the machine carries it whole and a restore puts it back. The chip's behavior is six
functions over that state: `ppuWrite` and `ppuRead` for a CPU access to a port,
`ppuPeekRegister` for a read's value without its effect, `ppuTickDot` for one dot, `ppuReset` for
the reset line, and `ppuNmiAsserted` for the NMI output. The machine calls them as its cycles run,
and a host that runs the machine never needs to.

```cpp
// The chip after the reset line: $2000, $2001, the temporary address, the fine X
// scroll, the write toggle and the read buffer cleared, the counter at dot 0 of
// scanline 0 of an even frame, and the warm-up window open; the vertical-blank flag,
// the OAM address, the current address, the I/O bus with its decay and every memory
// keep what they hold (NESdev wiki, "PPU power up state", Initial Register Values, the
// After Reset column and the note on the internal reset signal).
void ppuReset(PpuState& state) noexcept;

// A CPU write to port `port`, 0-7, the address's low three bits. Every write loads the
// I/O bus whole. $2000, $2001, $2005 and $2006 then take the value as PpuState says,
// unless the warm-up window stands; $2003 sets the OAM address; $2004 stores at the OAM
// address and steps it; $2007 writes through the chip's address space at `v` and
// steps `v`; a write to $2002 loads the bus and nothing else.
void ppuWrite(PpuState& state, std::uint8_t port, std::uint8_t value, const PpuInputs& inputs);

// A CPU read of port `port`. $2002 answers the vertical-blank flag in bit 7 over the
// I/O bus's low five bits, then clears the flag and the write toggle; $2004 answers
// the OAM byte at the OAM address; $2007 answers the read buffer and refills it from
// the chip's address space at `v`, or answers a palette entry at once under the bus's
// top two bits, and steps `v`; every other port answers the I/O bus as it stands.
[[nodiscard]] std::uint8_t ppuRead(PpuState& state, std::uint8_t port, const PpuInputs& inputs);

// What a read of port `port` would answer, with nothing moved: the flag, the toggle,
// the read buffer, the address and the I/O bus all stay as they are.
[[nodiscard]] std::uint8_t ppuPeekRegister(const PpuState& state, std::uint8_t port) noexcept;

// One dot: steps the frame counter, sets the vertical-blank flag at dot 1 of scanline
// 241 unless a read of $2002 is under way on the CPU's bus, clears the flag and closes
// the warm-up window at dot 1 of scanline 261, goes from dot 339 of scanline 261
// straight to dot 0 of scanline 0 on an odd frame with rendering enabled, counts each
// bit of the I/O bus a dot closer to its decay, and answers whether this dot ended a
// frame (NESdev wiki, "NMI", Operation; "PPU frame timing", Even/Odd Frames and VBL
// Flag Timing; "PPU power up state").
bool ppuTickDot(PpuState& state, bool statusRead) noexcept;

// The chip's NMI output, true while it is pulled low: the vertical-blank flag set and
// $2000 bit 7 set (NESdev wiki, "NMI", Operation).
[[nodiscard]] constexpr bool ppuNmiAsserted(const PpuState& state) noexcept {
  return state.vblank && (state.ctrl & 0x80u) != 0;
}
```

What the chip reaches through and does not hold is handed in as `PpuInputs`: the board with its
cartridge, for the chip's address space; the CHR RAM the board writes; and the dots a bit of the
I/O bus holds its charge after an access drives it, from the machine's config, 0 holding it for
ever.

```cpp
struct PpuInputs {
  Board& board;
  const Cartridge& cartridge;
  std::span<std::uint8_t> chrRam;
  std::uint32_t busDecayDots;
};
```

The examples on this page build their images with one helper, whose NMI vector points at an
`RTI`:

```cpp
#include "neasant/nes/nes.h"
#include "neasant/ppu/ppu.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

// A 16 KB NROM image with 8 KB of CHR ROM and horizontal mirroring: `program` at $8000, the
// reset and IRQ vectors pointing there, the NMI vector at $9000 and an RTI there.
std::vector<std::uint8_t> programImage(std::initializer_list<std::uint8_t> program) {
  std::vector<std::uint8_t> image{0x4E, 0x45, 0x53, 0x1A, 1, 1};
  image.resize(16 + 16384 + 8192);
  std::size_t at = 16;
  for (const std::uint8_t byte : program) image[at++] = byte;
  image[16 + 0x1000] = 0x40;     // RTI at $9000
  image[16 + 16384 - 6] = 0x00;  // the NMI vector: $9000
  image[16 + 16384 - 5] = 0x90;
  image[16 + 16384 - 4] = 0x00;  // the reset vector: $8000
  image[16 + 16384 - 3] = 0x80;
  image[16 + 16384 - 2] = 0x00;  // the IRQ vector: $8000
  image[16 + 16384 - 1] = 0x80;
  return image;
}
```

Every later example on this page uses this `programImage` and needs no further `#include`.

## The state value

`PpuState` is the chip's whole mutable state as a value: snapshot by copy, restore by assignment.

```cpp
struct PpuState {
  // $2000 as last written: bit 7 enables the NMI output, bit 2 selects the step the
  // address takes after a $2007 access, 1 or 32, and bits 1-0 are copied into the
  // temporary address as the nametable select (NESdev wiki, "PPU registers", PPUCTRL;
  // "PPU scrolling", $2000 write). Bits 6-3 are held for the picture.
  std::uint8_t ctrl = 0;
  // $2001 as last written: bits 4 and 3 enable rendering, which here decides the
  // odd-frame skip alone, and bit 0 is greyscale, which a palette read applies (NESdev
  // wiki, "PPU registers", PPUMASK). Bits 7-5 and 2-1 are held for the picture.
  std::uint8_t mask = 0;
  // The vertical-blank flag, bit 7 of $2002: set at dot 1 of scanline 241, cleared at
  // dot 1 of scanline 261 and by a read of $2002 (NESdev wiki, "NMI", Operation). At
  // power-on it is what the config says.
  bool vblank = false;
  // $2003 as last written, stepped by every write to $2004 (NESdev wiki, "PPU
  // registers", OAMADDR and OAMDATA).
  std::uint8_t oamAddr = 0;
  // The scroll file (NESdev wiki, "PPU scrolling", PPU internal registers): the current
  // address `v` and the temporary address `t`, 15 bits each, the fine X scroll `x`,
  // 3 bits, and the write toggle `w`, which says whether the next write to $2005 or
  // $2006 is the first or the second of its pair. A $2007 access reaches the low 14
  // bits of `v`.
  std::uint16_t v = 0;
  std::uint16_t t = 0;
  std::uint8_t x = 0;
  bool w = false;
  // The byte the next read of $2007 answers, loaded by the read before it (NESdev
  // wiki, "PPU registers", The PPUDATA read buffer).
  std::uint8_t readBuffer = 0;
  // The PPU's I/O bus: the byte every port write loads and every read of a write-only
  // port answers; a read of $2002 loads its bits 7-5, a read of $2004 or of $2007
  // through the read buffer loads it whole, and a palette read through $2007 loads its
  // bits 5-0 (NESdev wiki, "Open bus behavior", PPU open bus; "PPU registers", Reading
  // palette RAM).
  std::uint8_t ioLatch = 0;
  // For each bit of the I/O bus, bit 0 first, the dots left before it decays to zero:
  // reloaded by every access that drives the bit, from the count the machine was built
  // with, and 0 once the bit has decayed or when it has never been driven. A read of a
  // write-only port drives nothing and reloads nothing. A set bit decays to zero
  // (NESdev forum, "Tests for ACE speedruns that rely on PPU's internal open bus": the
  // thread's ppudecay test ROM leaves a byte on the bus and counts until it changes;
  // a single set bit holds longest and a byte of set bits changes first).
  std::array<std::uint32_t, 8> ioLatchDecay{};
  // The warm-up window: while it stands, writes to $2000, $2001, $2005 and $2006 have
  // no effect and the read buffer is held at zero. It opens at power-on and at reset
  // and closes at dot 1 of scanline 261 (NESdev wiki, "PPU power up state"; "PPU
  // registers", the opening paragraph).
  bool warmingUp = true;
  // Where the frame counter is: the dot within the scanline, 0-340, the scanline within
  // the frame, 0-261, and the parity, which toggles as each frame ends (NESdev wiki,
  // "PPU frame timing", Even/Odd Frames). At power-on and after reset the counter is
  // at dot 0 of scanline 0 of an even frame (NESdev wiki, "PPU power up state").
  std::uint16_t dot = 0;
  std::uint16_t scanline = 0;
  bool odd = false;
  // The console's 2 KB of nametable RAM, reached through the board's CIRAM lines (NESdev
  // wiki, "Cartridge connector", Signal descriptions: CIRAM /CE and CIRAM A10).
  std::array<std::uint8_t, 2048> ciram{};
  // The 32 entries of palette RAM at $3F00-$3F1F, repeated through $3FFF, six bits
  // each; entry 0 of each sprite palette is the storage of entry 0 of the background
  // palette beside it, so the array holds $3F10, $3F14, $3F18 and $3F1C at $3F00,
  // $3F04, $3F08 and $3F0C (NESdev wiki, "PPU palettes", Palette RAM; "PPU memory
  // map").
  std::array<std::uint8_t, 32> palette{};
  // The 256 bytes of object attribute memory, reached through $2003 and $2004 (NESdev
  // wiki, "PPU memory map", OAM).
  std::array<std::uint8_t, 256> oam{};
  [[nodiscard]] bool operator==(const PpuState&) const noexcept = default;
};
```

A frame is 341 dots by 262 scanlines with rendering disabled (NESdev wiki, "PPU frame timing",
Even/Odd Frames):

```cpp
inline constexpr std::uint16_t kDotsPerScanline = 341;
inline constexpr std::uint16_t kScanlinesPerFrame = 262;
```

`ctrl` and `mask` are `$2000` and `$2001` as last written; `v`, `t`, `x` and `w` are the scroll
file the "PPU scrolling" page names; the I/O bus and its eight decay counts, the warm-up window,
the counter's dot, scanline and parity, and the three memories are the rest. Every field is
plain, so a snapshot taken between two ports' accesses restores to the same place.

## The ports

The eight ports repeat every eight bytes through `$3FFF`, and `port` is the address's low three
bits. Every write loads the I/O bus whole, and every read of a write-only port answers it:

| Port | A write | A read answers | The read's side effects |
|---|---|---|---|
| `$2000` | `ctrl`, and bits 1–0 into bits 11–10 of `t`; ignored in the warm-up | the bus | none |
| `$2001` | `mask`; ignored in the warm-up | the bus | none |
| `$2002` | the bus and nothing more | the vertical-blank flag in bit 7 over the bus's low five bits | the flag and the toggle cleared; bits 7–5 of the bus driven |
| `$2003` | the OAM address | the bus | none |
| `$2004` | the OAM byte at the address, which then steps | the OAM byte at the address | the bus driven whole; the address unmoved |
| `$2005` | the first write coarse X into `t` and fine X into `x`, the second fine Y and coarse Y into `t`; the toggle flips; ignored in the warm-up | the bus | none |
| `$2006` | the first write bits 13–8 of `t` with bit 14 cleared, the second its low byte and then the whole of `t` into `v`; the toggle flips; ignored in the warm-up | the bus | none |
| `$2007` | the byte through the chip's address space at `v`, then `v` steps by 1 or 32 | the read buffer, or a palette entry at once under the bus's top two bits | the buffer refilled from `v`; the bus driven; `v` steps |

(NESdev wiki, "PPU registers", MMIO registers, PPUCTRL, PPUMASK, PPUSTATUS, OAMADDR, OAMDATA,
PPUSCROLL, PPUADDR, PPUDATA; "PPU scrolling", Register controls; "Open bus behavior", PPU open
bus.)

### A bare chip

The functions take the state, the port, the value and the inputs. The example builds the inputs
over an NROM board from the page's helper and clears the warm-up window, which holds off four
ports' writes after power-on ([Power-on, reset and the warm-up](#power-on-reset-and-the-warm-up)):

```cpp
std::vector<std::uint8_t> image = programImage({});
neasant::Cartridge cartridge = neasant::makeCartridge(image);
neasant::Board board = neasant::boardFor(cartridge);
std::vector<std::uint8_t> chrRam(cartridge.chrRamBytes);  // empty: the image carries CHR ROM
neasant::PpuState chip;
chip.warmingUp = false;  // the window after power-on, in which four ports ignore writes
const neasant::PpuInputs inputs{board, cartridge, chrRam, 115200u};

neasant::ppuWrite(chip, 6, 0x21, inputs);  // $2006: the address's high byte
neasant::ppuWrite(chip, 6, 0x08, inputs);  // its low byte: v is now $2108
neasant::ppuWrite(chip, 7, 0x5A, inputs);  // $2007: the byte lands at $2108 and v steps
// chip.v == 0x2109
// chip.ciram[0x108] == 0x5A: $2108 is byte $108 of the first kilobyte under horizontal
//                            mirroring
// chip.ioLatch == 0x5A: every port write loads the I/O bus
// neasant::ppuRead(chip, 0, inputs) == 0x5A: a write-only port answers the bus
```

The examples in the next five sections continue on the same `chip`, in order, and so do those
under [The I/O bus](#the-io-bus), [Its decay](#its-decay) and [The palette](#the-palette).

### $2000 and $2001

Bit 7 of `$2000` enables the NMI output, bit 2 selects the step `v` takes after a `$2007` access,
1 or 32, and bits 1–0 are copied into the temporary address as the nametable select; bits 6–3 are
held for the picture. `$2001` bits 4 and 3 enable rendering, which here decides the odd-frame skip
alone, and bit 0 is greyscale, which a palette read applies; bits 7–5 and 2–1 are held for the
picture (NESdev wiki, "PPU registers", PPUCTRL and PPUMASK; "PPU scrolling", $2000 write).

```cpp
neasant::ppuWrite(chip, 0, 0x83, inputs);
// chip.ctrl == 0x83 and chip.t == 0x2D08: bits 1-0 of the value went to bits 11-10 of the
//                                         temporary address, which held $2108 from the writes
// neasant::ppuNmiAsserted(chip) == false: bit 7 is set but the vertical-blank flag is clear
neasant::ppuWrite(chip, 1, 0x1E, inputs);
// chip.mask == 0x1E: rendering is enabled, which here decides the odd-frame skip alone
```

### $2002

A read answers the vertical-blank flag in bit 7 over the I/O bus's low five bits, then clears the
flag and the write toggle, and drives bits 7–5 of the bus with what it answered. A write to
`$2002` loads the bus and nothing else (NESdev wiki, "NMI", Operation; "PPU scrolling", $2002
read; "Open bus behavior", PPU open bus).

```cpp
chip.vblank = true;
chip.w = true;
neasant::ppuWrite(chip, 3, 0x5F, inputs);  // $2003: the bus holds $5F
const std::uint8_t status = neasant::ppuRead(chip, 2, inputs);
// status == 0x9F: the flag in bit 7 over the bus's low five bits
// chip.vblank == false and chip.w == false: the read cleared both
// chip.ioLatch == 0x9F: the read drives bits 7-5 of the bus
// neasant::ppuRead(chip, 2, inputs) == 0x1F
```

### $2003 and $2004

`$2003` sets the OAM address. A write to `$2004` lands at it and steps it; a read answers the byte
there and does not step it (NESdev wiki, "PPU registers", OAMADDR and OAMDATA).

```cpp
neasant::ppuWrite(chip, 3, 0x10, inputs);
neasant::ppuWrite(chip, 4, 0xAA, inputs);
neasant::ppuWrite(chip, 4, 0xBB, inputs);
// chip.oam[0x10] == 0xAA, chip.oam[0x11] == 0xBB and chip.oamAddr == 0x12: a write steps it
neasant::ppuWrite(chip, 3, 0x10, inputs);
const std::uint8_t first = neasant::ppuRead(chip, 4, inputs);
// first == 0xAA, and chip.oamAddr is still 0x10: a read does not
```

### $2005 and $2006

The two ports share the write toggle `w`, which a `$2002` read clears. The first `$2005` write
puts the value's high five bits into bits 4–0 of `t`, the coarse X scroll, and its low three bits
into `x`, the fine X scroll; the second puts the low three bits into bits 14–12 of `t`, the fine Y
scroll, and the high five into bits 9–5, the coarse Y scroll. The first `$2006` write puts the
value's low six bits into bits 13–8 of `t` and clears bit 14; the second puts the value into `t`'s
low byte and then copies the whole of `t` into `v`. Each write keeps the bits of `t` it does not
name (NESdev wiki, "PPU scrolling", Register controls and Summary). The page's worked sequence:

```cpp
neasant::ppuWrite(chip, 0, 0x00, inputs);
const std::uint8_t cleared = neasant::ppuRead(chip, 2, inputs);  // $2002: the toggle clears
neasant::ppuWrite(chip, 5, 0x7D, inputs);  // $2005 first: coarse X 15, fine X 5
// chip.t == 0x210F: coarse X in bits 4-0; chip.x == 5 and chip.w == true
neasant::ppuWrite(chip, 5, 0x5E, inputs);  // second: coarse Y 11, fine Y 6
// chip.t == 0x616F, chip.w == false and chip.v == 0x2109: nothing reaches v yet
neasant::ppuWrite(chip, 6, 0x3D, inputs);  // $2006 first: bits 13-8, with bit 14 cleared
// chip.t == 0x3D6F and chip.w == true
neasant::ppuWrite(chip, 6, 0xF0, inputs);  // second: the low byte, then the whole of t into v
// chip.t == 0x3DF0, chip.v == 0x3DF0, chip.w == false and chip.x == 5
```

### $2007

A write lands through the chip's address space at the low 14 bits of `v`: a palette entry when
`v` is `$3F00` or above, otherwise the board or the console's nametable RAM
([The address space](#the-address-space)). `v` then steps by 1 or 32, as `$2000` bit 2 says,
wrapping inside its 15 bits. A read answers the read buffer and refills it from `v`, so a read
lags one access behind the address. A palette address is the exception, answered at once under
the bus's top two bits while the read underneath fills the buffer (NESdev wiki, "PPU registers",
PPUDATA, The PPUDATA read buffer and Reading palette RAM).

```cpp
cartridge.chrRom[0x10] = 0xA1;  // three CHR ROM bytes to read
cartridge.chrRom[0x11] = 0xB2;
cartridge.chrRom[0x12] = 0xC3;
neasant::ppuWrite(chip, 6, 0x00, inputs);
neasant::ppuWrite(chip, 6, 0x10, inputs);  // v is $0010
const std::uint8_t stale = neasant::ppuRead(chip, 7, inputs);
// stale == 0x00: the buffer as it stood; the read loads $A1 into it and steps v to $0011
const std::uint8_t fresh = neasant::ppuRead(chip, 7, inputs);
// fresh == 0xA1, and the buffer now holds $B2
// chip.readBuffer == 0xB2 and chip.v == 0x0012
```

## The I/O bus

The chip's data lines to the CPU are one byte the chip remembers, `ioLatch`. Every port write
loads it whole, a read of a write-only port answers it, a `$2002` read loads its bits 7–5, a
`$2004` read or a buffered `$2007` read loads it whole, and a palette read loads its bits 5–0
(NESdev wiki, "Open bus behavior", PPU open bus; "PPU registers", MMIO registers and Reading
palette RAM).

```cpp
neasant::ppuWrite(chip, 2, 0xAA, inputs);  // a write to $2002 loads the bus and nothing else
// chip.ioLatch == 0xAA
// neasant::ppuRead(chip, 0, inputs) == 0xAA and neasant::ppuRead(chip, 6, inputs) == 0xAA
```

### Its decay

Each bit of the bus is a dynamic latch: an access that drives it recharges it for the count the
machine was built with, in dots, and it decays to zero when the count runs out. A read of a
write-only port drives nothing and recharges nothing. The count is `NesConfig::ppuOpenBusDecay`
in master cycles, four to a dot, 0 holding the bus for ever. The default, 460 800 master cycles,
is one reading of an NTSC NES at power-on: a test ROM writes a value to `$2002` and polls `$2000`
every fifteen CPU cycles until the byte changes, counting `$A00` polls, 38 400 CPU cycles, about
21 ms. The readings run from 4 to 24 ms across consoles and shorten as a console warms. A byte of
set bits changes first and a single set bit holds longest, which is why the chip ages each bit on
its own and clears set bits to zero (NESdev forum, "Tests for ACE speedruns that rely on PPU's
internal open bus"; NESdev wiki, "PPU registers", MMIO registers).

```cpp
const neasant::PpuInputs quick{board, cartridge, chrRam, 10u};  // ten dots
neasant::ppuWrite(chip, 2, 0xFF, quick);
// chip.ioLatchDecay[0] == 10, and so on up to chip.ioLatchDecay[7]
for (int dot = 0; dot < 9; ++dot) neasant::ppuTickDot(chip, false);
// neasant::ppuRead(chip, 0, quick) == 0xFF: nine dots on, every bit holds
neasant::ppuTickDot(chip, false);
// neasant::ppuRead(chip, 0, quick) == 0x00: on the tenth every bit fell
```

## The address space

A `$2007` access reaches the chip's 14-bit space:

| Address | What answers |
|---|---|
| `$0000-$1FFF` | the pattern tables: the board's CHR ROM or CHR RAM |
| `$2000-$2FFF` | the four nametables: the console's 2 KB of nametable RAM through the board's `CIRAM A10` and `CIRAM /CE`, or a board's own RAM |
| `$3000-$3EFF` | `$2000-$2EFF` again |
| `$3F00-$3F1F` | the 32 entries of palette RAM, inside the chip |
| `$3F20-$3FFF` | the palette again, seven times |

(NESdev wiki, "PPU memory map"; "Cartridge connector", Signal descriptions.) The board sees every
access below the palette on the PPU pins, drives the two CIRAM lines, and answers the access
itself when it decodes the address; the console's RAM answers when the board enables it; an
address neither answers reads as its own low byte (NESdev wiki, "Open bus behavior", PPU open
bus). Under NROM the nametables fall as
[the cartridge page's mirroring table](nes-cartridge.md#mirroring) says. `state().ciram` is the
console's 2 KB, and `state().oam` the 256 bytes of object attribute memory, reached through
`$2003` and `$2004`.

### The palette

An entry is six bits, and a write keeps `value & $3F`. Entry 0 of each sprite palette is the
storage of entry 0 of the background palette beside it, so `$3F10`, `$3F14`, `$3F18` and `$3F1C`
reach `$3F00`, `$3F04`, `$3F08` and `$3F0C`, and the 32 entries repeat through `$3FFF`. A read
answers the entry at once under the bus's top two bits, with greyscale applied as `entry & $30`
when `$2001` bit 0 is set; the entry itself keeps its bits (NESdev wiki, "PPU palettes", Palette
RAM; "PPU registers", Reading palette RAM and Color control; "PPU memory map"). A palette access
is not presented to the board.

```cpp
neasant::ppuWrite(chip, 6, 0x3F, inputs);
neasant::ppuWrite(chip, 6, 0x10, inputs);  // v is $3F10
neasant::ppuWrite(chip, 7, 0x21, inputs);  // lands at entry 0: $3F10 is the storage of $3F00
// chip.palette[0x00] == 0x21 and chip.palette[0x10] == 0x00
chip.palette[0x05] = 0x2C;
chip.ciram[0x705] = 0x77;  // the nametable byte underneath $3F05 under horizontal mirroring
neasant::ppuWrite(chip, 6, 0x3F, inputs);
neasant::ppuWrite(chip, 6, 0x05, inputs);
neasant::ppuWrite(chip, 2, 0xC3, inputs);  // the bus's top two bits set
const std::uint8_t entry = neasant::ppuRead(chip, 7, inputs);
// entry == 0xEC: the entry at once, under the bus's top two bits
// chip.readBuffer == 0x77: the read underneath fills the buffer
```

## The frame

The counter runs 341 dots a scanline and 262 scanlines a frame, with a parity that toggles as
each frame ends, and `ppuTickDot` steps it one dot and answers whether the dot ended a frame. The
vertical-blank flag is set at dot 1 of scanline 241 and cleared at dot 1 of scanline 261, 6820
dots later, where the warm-up window closes too (NESdev wiki, "NMI", Operation; "PPU frame
timing", Even/Odd Frames and VBL Flag Timing; "PPU power up state").

```cpp
neasant::PpuState counter;
int ticks = 0;
while (!(counter.scanline == 241 && counter.dot == 0)) {
  neasant::ppuTickDot(counter, false);
  ++ticks;
}
// ticks == 82181: scanline 241, dot 0
neasant::ppuTickDot(counter, false);
// counter.vblank == true: set at dot 1 of scanline 241
for (int dot = 0; dot < 6820; ++dot) neasant::ppuTickDot(counter, false);
// counter.scanline == 261, counter.dot == 1 and counter.vblank == false: cleared 6820 dots on
// counter.warmingUp == false: the same dot closes the warm-up window
bool ended = false;
for (int dot = 0; dot < 340; ++dot) ended = neasant::ppuTickDot(counter, false);
// ended == true: the 89342nd tick wraps the counter to scanline 0, dot 0, on an odd frame
// counter.odd == true
```

### The odd-frame skip

With rendering enabled, either of `$2001` bits 4 and 3, an odd frame goes from dot 339 of scanline
261 straight to dot 0 of scanline 0, one dot short. With rendering disabled every frame is 89 342
dots and no dot is skipped (NESdev wiki, "PPU frame timing", Even/Odd Frames; "PPU registers",
PPUMASK, Rendering control).

```cpp
neasant::PpuState rendering;
rendering.warmingUp = false;
rendering.mask = 0x08;  // the background enabled: rendering is on
int even = 0;
while (!neasant::ppuTickDot(rendering, false)) ++even;
// even + 1 == 89342: an even frame is whole
int odd = 0;
while (!neasant::ppuTickDot(rendering, false)) ++odd;
// odd + 1 == 89341: the odd frame leaves scanline 261 from dot 339
```

### The machine's frame

The machine runs the dots from a dot clock the alignment seeds, four master cycles a dot. Counting
the first cycle after power-on as cycle 0, the flag's first set falls in machine cycle 27393, its
first clear in cycle 29667 and the first frame's end in cycle 29780, under every alignment
([the machine page](nes-machine.md#a-machine-cycle)). With rendering off a frame is 357 368 master
cycles, 29 780 and two thirds machine cycles.

```cpp
std::vector<std::uint8_t> image = programImage({0x4C, 0x00, 0x80});  // JMP $8000
neasant::Nes machine(neasant::NesConfig{.rom = image, .vblankAtPowerOn = false});
machine.run(27393u * 12u);
// machine.state().ppu.vblank == false
machine.step();
// machine.state().ppu.vblank == true: the flag's first set falls in cycle 27393
machine.run((29667u - 27394u) * 12u);
// machine.state().ppu.warmingUp == true
machine.step();
// machine.state().ppu.vblank == false and machine.state().ppu.warmingUp == false: cycle 29667
machine.run((29780u - 29668u) * 12u);
machine.step();
// machine.state().ppu.scanline == 0, machine.state().ppu.dot == 1 and machine.state().ppu.odd
// == true: the frame ended inside cycle 29780
```

## The NMI output

The chip pulls its NMI line low while the vertical-blank flag and `$2000` bit 7 are both set, and
`ppuNmiAsserted` answers that. A program that toggles bit 7 inside vertical blank without reading
`$2002` pulls the line again each time (NESdev wiki, "NMI", Operation; "PPU registers", Vblank
NMI).

```cpp
neasant::PpuState output;
// neasant::ppuNmiAsserted(output) == false
output.vblank = true;
// neasant::ppuNmiAsserted(output) == false still: $2000 bit 7 is clear
output.ctrl = 0x80;
// neasant::ppuNmiAsserted(output) == true: the line is pulled low
```

### The interrupt

The machine samples the output as each cycle ends and the CPU latches the edge; the request is
taken at the boundary after the instruction whose poll before its last cycle found it (NESdev
wiki, "CPU interrupts", Detailed interrupt behavior;
[the CPU page](6502-cpu.md#when-a-request-is-taken)).

```cpp
std::vector<std::uint8_t> image = programImage({0x4C, 0x00, 0x80});  // JMP $8000; RTI at $9000
neasant::Nes machine(neasant::NesConfig{.rom = image, .vblankAtPowerOn = false});
for (int cycle = 0; cycle < 7; ++cycle) machine.step();
neasant::NesState enabled = machine.state();
enabled.ppu.ctrl = 0x80;  // past the warm-up a program writes $2000; here the state is written
machine.restore(enabled);
machine.run((27393u - 7u) * 12u);
machine.step();  // the flag sets in a late dot of the jump's last cycle
// machine.state().ppu.vblank == true and machine.state().cpu.nmiPending == true
for (int cycle = 0; cycle < 3; ++cycle) machine.step();  // the next jump runs whole
machine.step();  // the sequence begins
// machine.state().cpu.servicing == neasant::CpuSequence::Nmi
for (int cycle = 0; cycle < 6; ++cycle) machine.step();
// machine.state().cpu.pc == 0x9000: the handler's RTI
// machine.state().cpu.s == 0xFA
```

### The $2002 race

A read of `$2002` that overlaps the flag's set loses it. The read answers the flag as the dots
before the access left it, a set that lands inside the access window is lost for that frame, and
the CPU samples the NMI line as the cycle ends, after the window, so a line the read pulled back up
is not seen (NESdev wiki, "PPU frame timing", VBL Flag Timing; "NMI", Race condition; "PPU
registers", Vblank flag; "CPU pinout", M2). With `a` the alignment and `o` the set's master offset
from the read cycle's start:

| `o` | Where the set ticks | The read answers | The flag after | The NMI |
|---|---|---|---|---|
| `a − 4` | the previous cycle's last dot, after its access | 1 | cleared | taken: the previous cycle's sample saw the line low |
| `a` | this cycle, before the access | 1 | cleared | suppressed: no sample saw the line low |
| `a + 4` | before the access when `a` is 0; inside the window otherwise | 1, or 0 | cleared, or never set | suppressed, or none |
| `a + 8` | inside the window | 0 | never set this frame | none |
| `a + 12` | the next cycle's first dot | 0 | set | taken at the next cycle's sample |

(NESdev wiki, "PPU frame timing", VBL Flag Timing and CPU-PPU Clock Alignment.) Under alignment 0
the rows are the "PPU frame timing" page's five outcomes in order; under the other alignments the
`a + 4` row joins the `a + 8` row, the one-dot shift the page says other alignments show.

## Power-on, reset and the warm-up

At power-on `$2000`, `$2001`, the OAM address, the scroll file, the read buffer and the I/O bus
are 0, and the counter is at dot 0 of scanline 0 of an even frame. The vertical-blank flag is what
`NesConfig::vblankAtPowerOn` says, usually set, and the three memories hold the seed, the
palette's entries kept to six bits. A warm-up window opens at power-on and at reset and closes at
dot 1 of scanline 261, the chip's first pre-render line. While it stands, a write to `$2000`,
`$2001`, `$2005` or `$2006` loads the I/O bus and does nothing else, the toggle does not move, and
the read buffer is held at zero; the other ports work. From power-on the window closes in machine
cycle 29667, 29 660 cycles after the reset sequence ends; the pages give around 29658 CPU cycles
after reset (NESdev wiki, "PPU power up state", Initial Register Values and the notes; "PPU
registers", the opening paragraph).

| Field | At power-on | After the reset line |
|---|---|---|
| `ctrl`, `mask` | 0 | 0 |
| `vblank` | `vblankAtPowerOn` | kept |
| `oamAddr` | 0 | kept |
| `v` | 0 | kept |
| `t`, `x`, `w` | 0, 0, clear | 0, 0, clear |
| `readBuffer` | 0 | 0 |
| `ioLatch` and its decay counts | 0, nothing driven | kept |
| `warmingUp` | open | open |
| `dot`, `scanline`, `odd` | 0, 0, even | 0, 0, even |
| `ciram`, `oam` | the seed | kept |
| `palette` | the seed, each entry `& $3F` | kept |

(NESdev wiki, "PPU power up state", Initial Register Values, the After Reset column and the note
on the internal reset signal.) A program that writes `$2000` and `$2001` in a loop from power-on:

```cpp
// LDA #$80; STA $2000; LDA #$1E; STA $2001; JMP $8000: fifteen cycles a loop
std::vector<std::uint8_t> image = programImage({0xA9, 0x80, 0x8D, 0x00, 0x20, 0xA9, 0x1E, 0x8D,
                                                0x01, 0x20, 0x4C, 0x00, 0x80});
neasant::Nes machine(neasant::NesConfig{.rom = image});
// machine.state().ppu.vblank == true and machine.state().ppu.warmingUp == true at power-on
machine.run(29667u * 12u);
// machine.state().ppu.ctrl == 0 and machine.state().ppu.mask == 0: every write so far ignored
// machine.state().ppu.warmingUp == true
machine.run(15u * 12u);
// machine.state().ppu.ctrl == 0x80 and machine.state().ppu.mask == 0x1E: the window closed in
// cycle 29667 and the loop's next writes landed
// machine.state().ppu.warmingUp == false
```

### The reset line

The machine's reset line resets the chip with the CPU, as a front-loading console's does, and
`ppuReset` is the chip's half (NESdev wiki, "PPU power up state").

```cpp
neasant::PpuState running;
running.ctrl = 0x83;
running.mask = 0x1E;
running.vblank = true;
running.oamAddr = 0x12;
running.v = 0x2108;
running.t = 0x3DF0;
running.x = 5;
running.w = true;
running.readBuffer = 0x44;
running.ioLatch = 0x55;
running.warmingUp = false;
running.scanline = 50;
running.dot = 100;
running.odd = true;
neasant::ppuReset(running);
// running.ctrl == 0, running.mask == 0, running.t == 0, running.x == 0, running.w == false and
// running.readBuffer == 0: cleared
// running.scanline == 0, running.dot == 0, running.odd == false and running.warmingUp == true
// running.vblank == true, running.oamAddr == 0x12, running.v == 0x2108 and
// running.ioLatch == 0x55: kept
```

## Reading a register without its effect

`ppuPeekRegister` answers what a read of a port would answer and moves nothing: the flag, the
toggle, the read buffer, the address and the I/O bus all stay as they are.

```cpp
neasant::PpuState peeked;
peeked.vblank = true;
peeked.w = true;
peeked.ioLatch = 0x1A;
// neasant::ppuPeekRegister(peeked, 2) == 0x9A, with peeked.vblank and peeked.w left set
peeked.oamAddr = 3;
peeked.oam[3] = 0x77;
// neasant::ppuPeekRegister(peeked, 4) == 0x77
peeked.v = 0x0010;
peeked.readBuffer = 0x33;
// neasant::ppuPeekRegister(peeked, 7) == 0x33: the buffer, with v left at $0010
// neasant::ppuPeekRegister(peeked, 0) == 0x1A: a write-only port answers the bus
```

## Gotchas

- **Four ports ignore writes until the first pre-render line.** `$2000`, `$2001`, `$2005` and
  `$2006` take nothing in the warm-up window, the toggle does not move, and the bus takes the byte
  all the same.
- **A `$2002` read clears two things.** The flag and the write toggle; its low five bits are the
  bus's.
- **`$2005` and `$2006` share the toggle.** A `$2002` read resets it; a program writing both reads
  `$2002` first.
- **A `$2007` read lags one access, except the palette.** The first read after setting the
  address answers the buffer as it stood.
- **The I/O bus decays.** A value left on it is gone after the configured count; a read of a
  write-only port recharges nothing.
- **`$2004` writes step the address and reads do not.**
- **The vertical-blank flag is set at power-on by default.** `vblankAtPowerOn` clears it; a
  program that waits for the flag to clear first is waiting on this field.
- **A read of `$2002` on the dot before the set reads 0 and loses the frame's flag.** The NMI is
  lost with it.
- **`$3F10` is `$3F00`.** The four sprite palettes' entry 0 are the background palettes'.
- **Greyscale changes what a palette read answers, not what it holds.**
- **Rendering enabled changes one thing here.** The odd frame's skipped dot; no tile is fetched.
- **`v` steps linearly through `$2007`.** By 1 or 32, wrapping inside 15 bits; its top bit
  reaches no address.
- **The reset line keeps the flag, the OAM address, `v`, the bus and the memories.**

## What remains open

What the code does at each point below is fixed and tested; what the hardware does is not settled
by the documents the chip is built from.

1. **The flag against a read under alignments 1 to 3.** The race's outcomes under alignment 0 are
   the "PPU frame timing" page's. The other alignments' columns follow from the page's statement
   that a CPU tick begins 0 to 3 master cycles before the nearest PPU tick and from the "CPU
   pinout" page's M2 duty cycle, and are derived, not measured.
2. **How wide the NMI suppression is.** The "PPU registers" page has the NMI suppressed by reads
   landing on the following dot or two; the "PPU frame timing" page has one dot; the chip follows
   the frame-timing page.
3. **The chip while rendering.** The "PPU registers" page states behaviors the register file does
   not carry: a rendering toggle reaching the picture three or four dots after the write, the
   second `$2006` write reaching `v` one to one and a half dots on (loaded at the write here), the
   OAM address forced to 0 during sprite evaluation and its glitchy increment, the evaluation
   exposed through `$2004`, the `$2000` bit 0 race at dot 257, the `$2006` bus conflict, and the
   palette corruption; the "DMA" page adds the DPCM read conflict with `$2007`. Each is stated by
   its page and belongs to the picture or the DMC.
4. **The 2C02G's OAM corruption on `$2003` writes, and OAM byte 2's unimplemented bits.** The
   "PPU registers" page's OAMADDR note; the chip holds every byte written.
5. **Whether a palette write drives the PPU bus.** No document the chip is built from states that
   the chip drives `/WR` for a write to `$3F00-$3FFF`; the chip presents a palette access to no
   board.
6. **The top-loader's reset.** The "PPU power up state" page says the Famicom and NES-101 reset
   the CPU alone; the machine's reset line resets both chips, the front-loader's.
7. **The warm power-on.** The page's values for a console off under twenty seconds — PPUSTATUS
   `$80`, OAMADDR `$2F` or `$01`, PPUADDR `$0001` — are not modelled; a host that wants them
   writes the state.
8. **The I/O bus's decay.** The thread's readings run from 4 to 24 ms across consoles and bits and
   shorten as a console warms; the config holds one count for every bit, the thread author's NTSC
   NES at power-on.
9. **The palette read's revision.** The "PPU registers" page's immediate palette read is the
   2C02G's and later; earlier revisions answered the read buffer. The chip is the 2C02G's and the
   earlier answer is not selectable.
10. **OAM's reads.** The CPU's `cpu_dummy_writes_oam` program passes on this chip's OAM, which
    reads back every byte; its readme says the console's does not.

## See also

- [The NES machine](nes-machine.md) — the cycle the dots run in, the alignment, the reset line,
  the observers.
- [The cartridge](nes-cartridge.md) — the PPU pins, CHR ROM and RAM, the CIRAM lines.
- [The 6502 core](6502-cpu.md) — when the NMI is taken.

## Where to look

- `include/neasant/ppu/ppu.h` — the state value, the inputs, the six functions.
- `src/ppu/ppu.cpp` — every port's write and read, the palette and CIRAM, the dot counter with its
  flag, warm-up and skip, and the bus's decay.
- `tests/ppu/registers_test.cpp` — the suite.

## Sources

- **NESdev wiki** — the pages "PPU registers", "PPU scrolling", "PPU memory map", "PPU palettes",
  "PPU frame timing", "PPU power up state", "NMI", "Open bus behavior", "Cartridge connector",
  "CPU pinout", "CPU interrupts" and "DMA", at `https://www.nesdev.org/wiki/`.
- **NESdev forum** — the thread "Tests for ACE speedruns that rely on PPU's internal open bus" at
  `https://forums.nesdev.org/viewtopic.php?t=24639`, with its test ROM and source, read as the
  measurement of the I/O bus's decay.
