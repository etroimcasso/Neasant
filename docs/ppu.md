# The PPU

The 2C02 is the NES's picture processor, the chip a program reaches at `$2000`–`$2007`. This is its
register file over a state value: the eight ports with every latch a write passes through and the
value and side effect of every read, the I/O bus behind them and its decay, the scroll file the
ports fill, the port at `$2007` over the chip's own 14-bit address space, the frame counter that
runs 341 dots by 262 scanlines, the vertical-blank flag and the NMI output it drives, the warm-up
window after power-on and reset, the memories the console and the chip hold, nametable RAM, palette
RAM and object attribute memory, the background, fetched through the board and scrolled by `v`, the
sprites, found in OAM a line ahead and fetched through the board, and the picture, 256 dots by 240
lines, each resolved on its dot.

The register file is what a program reads and writes, and the frame counter is what its timing
stands on. With rendering enabled the chip fetches the background a tile every eight dots, finds the
next line's sprites in OAM as it goes, and draws both over the backdrop through the priority
multiplexer, setting sprite 0 hit and the overflow flag beside; with rendering disabled every
position is the backdrop, or the palette entry `v` points at; either way under greyscale and
emphasis. The chip's state is one value inside [the machine's](nes-machine.md), `state().ppu`, and
the machine runs its dots and hands the picture to [its frame
observer](nes-machine.md#the-frame-observer).

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
- [The pins](#the-pins)
- [The frame](#the-frame)
  - [The odd-frame skip](#the-odd-frame-skip)
  - [The machine's frame](#the-machines-frame)
- [The background](#the-background)
  - [A tile's four accesses](#a-tiles-four-accesses)
  - [A line's dots](#a-lines-dots)
  - [The scroll while rendering](#the-scroll-while-rendering)
  - [The shift registers and the pixel](#the-shift-registers-and-the-pixel)
- [The sprites](#the-sprites)
  - [A sprite's four bytes](#a-sprites-four-bytes)
  - [The evaluation](#the-evaluation)
  - [The output units](#the-output-units)
  - [The sprites' pixel and the multiplexer](#the-sprites-pixel-and-the-multiplexer)
- [The picture](#the-picture)
  - [What a dot holds](#what-a-dot-holds)
- [The NMI output](#the-nmi-output)
  - [The interrupt](#the-interrupt)
  - [The $2002 race](#the-2002-race)
- [Power-on, reset and the warm-up](#power-on-reset-and-the-warm-up)
  - [The reset line](#the-reset-line)
- [Reading a register without its effect](#reading-a-register-without-its-effect)
- [Testing against the programs](#testing-against-the-programs)
- [Gotchas](#gotchas)
- [What remains open](#what-remains-open)
- [See also](#see-also)
- [Where to look](#where-to-look)
- [Sources](#sources)

## The surface

The chip's state is one value, `PpuState`, held inside the machine's state as `NesState::ppu`; a
snapshot of the machine carries it whole and a restore puts it back. The chip's behavior is six
functions over that state: `ppuWrite` and `ppuRead` for a CPU access to a port, `ppuPeekRegister`
for a read's value without its effect, `ppuTickDot` for one dot, the background's and the sprites'
work and the picture's position with it, `ppuReset` for the reset line, and `ppuNmiAsserted` for the
NMI output. The machine calls them as its cycles run, and a host that runs the machine never needs
to.

```cpp
// The chip after the reset line: $2000, $2001 with any write of it still short of the
// frame counter, the temporary address, the fine X scroll, the write toggle and the read
// buffer cleared, any $2007 access on its way dropped, the counter at dot 0 of scanline 0
// of an even frame, and the warm-up window open; the vertical-blank, sprite 0 hit and
// sprite overflow flags, the OAM address, the current address, the I/O bus with its
// decay, the fetch latches, the shift registers, the pixels on their way to the picture,
// the address lines, secondary OAM with the evaluation's place, the OAM bus, the sprite 0
// flags, the output units and every memory keep what they hold (NESdev wiki, "PPU power
// up state", Initial Register Values, the After Reset column and the note on the internal
// reset signal).
void ppuReset(PpuState& state) noexcept;

// A CPU write to port `port`, 0-7, the address's low three bits. Every write loads the
// I/O bus whole. $2000, $2001, $2005 and $2006 then take the value as PpuState says,
// unless the warm-up window stands; $2003 sets the OAM address; $2004 stores at the OAM
// address and steps it; $2007 takes a write through the chip's address space at `v`,
// which the chip makes on its own dots and steps `v` with (PpuDataAccess); a write to
// $2002 loads the bus and nothing else.
void ppuWrite(PpuState& state, std::uint8_t port, std::uint8_t value, const PpuInputs& inputs);

// A CPU read of port `port`. $2002 answers the vertical-blank flag in bit 7, the sprite 0
// hit flag in bit 6 and the sprite overflow flag in bit 5 over the I/O bus's low five
// bits, then clears the vertical-blank flag and the write toggle; $2004 answers the OAM
// byte at the OAM address, a sprite's byte 2 with bits 4-2 clear, or the byte on the
// chip's OAM bus while the chip renders; $2007 answers the read buffer, or a palette
// entry at once under the bus's top two bits, and takes a read through the chip's
// address space at `v`, which the chip makes on its own dots, refilling the buffer and
// stepping `v` (PpuDataAccess); every other port answers the I/O bus as it stands.
[[nodiscard]] std::uint8_t ppuRead(PpuState& state, std::uint8_t port, const PpuInputs& inputs);

// What a read of port `port` would answer, with nothing moved: the flag, the toggle,
// the read buffer, the address and the I/O bus all stay as they are.
[[nodiscard]] std::uint8_t ppuPeekRegister(const PpuState& state, std::uint8_t port) noexcept;

// One dot. Steps the frame counter, going from dot 339 of scanline 261 straight to dot
// 0 of scanline 0 on an odd frame with rendering enabled; sets the vertical-blank flag
// at dot 1 of scanline 241 unless a read of $2002 is under way on the CPU's bus, and
// clears it, the sprite 0 hit flag and the sprite overflow flag and closes the warm-up
// window at dot 1 of scanline 261.
//
// With rendering enabled, on scanlines 0-239 and 261, runs the background's dot. An
// access through the board is two dots, the address and then the read. Dots 1-256 and
// 321-336 fetch a tile every eight: its nametable byte, its attribute byte and its two
// pattern bytes. Each eight dots from 257 to 320 open with two nametable fetches, the
// first kept and the second dropped, as dots 337-340 are; the four dots that follow
// them in each eight fetch a sprite's two pattern bytes. Dot 0 of scanlines 0-240
// drives the address of the next pattern fetch and reads nothing, and the dot an odd
// frame reaches by its skip makes the read dot 340 would have. The shift registers
// shift on dots 2-257 and 322-337 and reload on every eighth dot from 9 to 257 and on
// dots 329 and 337. `v` steps its coarse X on every eighth dot from 8 to 256 and on
// dots 328 and 336, steps its Y on dot 256, takes `t`'s horizontal bits on dot 257 and,
// on scanline 261, `t`'s vertical bits on each of dots 280-304. A write to $2001 counts
// for the skip and for all of this from the third dot after its access.
//
// With rendering enabled, on scanlines 0-239, runs the sprites' evaluation beside the
// background: dots 1-64 clear secondary OAM to $FF, and from dot 65 each odd dot reads
// OAM at the OAM address and each even dot copies a sprite found in range for the next
// line into secondary OAM, eight at most, a ninth found setting the overflow flag by
// the search's own fault. On dots 257-320 of scanlines 0-239 and 261 each eight reads a
// slot of secondary OAM into an output unit, its attribute on the third dot, its X on
// the fourth and its two pattern bytes by the fetches, 0 for a slot whose sprite has no
// row on the line, and holds the OAM address at 0. On each of dots 2-257 of scanlines
// 0-239 the units count down and shift, the frontmost opaque sprite pixel meets the
// background's in the priority multiplexer, and sprite 0 hit is set where both are
// opaque with sprite 0 in the first unit, at every position but 255.
//
// Makes the dots of the $2007 accesses the chip holds: on an access's address dot
// drives the lines and the latch from `v` and presents the board the address alone; on
// its strobe dot reads the latched address into the read buffer, held at zero through
// the warm-up, or writes the byte there, a palette entry taking it with no strobe to
// the board, and steps `v`. On a dot that makes no access, the lines take `v` as it
// stands and the board is presented the new address when they change; the accesses are
// a fetch's two dots, dot 0 of scanlines 0-240 and a $2007 access's two.
//
// Resolves the picture's position for the dot on dots 4-259 of scanlines 0-239, and
// stores it when `inputs` carries the picture: position x of line y on dot x + 4 of
// scanline y. With rendering enabled its value is the multiplexer's pixel, selected two
// dots earlier, or the backdrop, palette entry 0, where that pixel is transparent; with
// rendering disabled the backdrop, or the entry the low 14 bits of `v` name while they
// are in $3F00-$3FFF; each under greyscale and the emphasis bits as $2001 stands on that
// dot. Counts each bit of the I/O bus a dot closer to its decay, and answers what the
// dot was (PpuDotEvent).
//
// NESdev wiki, "NMI", Operation; "PPU frame timing", Even/Odd Frames and VBL Flag
// Timing; "PPU power up state"; "PPU registers", PPUMASK, PPUSTATUS and OAMADDR; "PPU
// rendering", Drawing overview, Rendering disabled, Line-by-line timing and PPU address
// bus contents, and its frame timing diagram with its notes; "PPU scrolling", Register
// controls and Wrapping around; "PPU palettes", Backdrop override; "PPU OAM"; "PPU
// sprite evaluation"; "PPU sprite priority"; "PPU pinout", Signal descriptions; Visual
// 2C02.
PpuDotEvent ppuTickDot(PpuState& state, bool statusRead, const PpuInputs& inputs);

// The chip's NMI output, true while it is pulled low: the vertical-blank flag set and
// $2000 bit 7 set (NESdev wiki, "NMI", Operation).
[[nodiscard]] constexpr bool ppuNmiAsserted(const PpuState& state) noexcept {
  return state.vblank && (state.ctrl & 0x80u) != 0;
}
```

What the chip reaches through and does not hold is handed in as `PpuInputs`: the board with its
cartridge, for the chip's address space; the CHR RAM the board writes; the dots a bit of the I/O
bus holds its charge after an access drives it, from the machine's config, 0 holding it for
ever; the picture's storage, `kPictureDots` entries the chip writes each dot's value into as it
resolves the dot, or empty, with which the chip resolves every dot, stores none, and runs exactly
as it does with the storage ([The picture](#the-picture)); and the master cycle of the dot under
way that a CPU access to a port falls on, 0 to 3, which the machine takes from its alignment and
a bare chip's caller leaves at 1 ([The pins](#the-pins)).

```cpp
struct PpuInputs {
  Board& board;
  const Cartridge& cartridge;
  std::span<std::uint8_t> chrRam;
  std::uint32_t busDecayDots;
  std::span<std::uint16_t> picture{};
  std::uint8_t accessCycle = 1;
};
```

`ppuTickDot` answers what the dot was, beyond one more of the frame:

```cpp
// What a dot was, beyond one more of the frame: the dot that resolved the picture's
// first position, dot 4 of scanline 0; the dot the vertical blank begins on, dot 1 of
// scanline 241, where the flag is set; the dot that ended the frame, taking the counter
// to dot 0 of scanline 0; or none of these, as most dots are. No dot is two of them.
enum class PpuDotEvent : std::uint8_t { None, PictureBegun, VerticalBlank, FrameEnded };
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

`PpuState` is the chip's whole mutable state as a value: snapshot by copy, restore by assignment;
`SpriteUnit` is one of the eight output units it holds and `PpuDataAccess` a `$2007` access it has
taken and not yet made ([The pins](#the-pins)).

```cpp
// One sprite output unit: the two 8-bit shift registers holding its row, [0] the pixels'
// low bits and [1] their high bits, with the leftmost pixel at bit 0 and each shift
// moving every bit a place toward bit 0 with a 0 in at bit 7; its attribute byte, bits
// 1-0 the palette and bit 5 the priority, behind the background when set; and its X
// counter, which counts down a picture position at a time and lets the registers shift
// once it is 0. A unit loaded from a slot that holds no sprite for the line has its
// registers 0 (NESdev wiki, "PPU rendering", Cycles 257-320; "PPU OAM", Byte 2 -
// Attributes; Brad Taylor's NTSC 2C02 technical reference, Details of object pattern
// fetch & render).
struct SpriteUnit {
  std::array<std::uint8_t, 2> pattern{};
  std::uint8_t attribute = 0;
  std::uint8_t x = 0;
  [[nodiscard]] bool operator==(const SpriteUnit&) const noexcept = default;
};

// What a CPU access through $2007 is on its way to the chip's address space: a read or a
// write, or no access held.
enum class PpuDataAccessKind : std::uint8_t { None, Read, Write };

// A CPU access through $2007 that the chip has taken and not yet made. The chip takes the
// access in the dot under way and makes it on its own dots: the address dot, three dots
// on, where the address lines take `v`, the console's latch captures its low byte and
// the board is presented the address alone; and the strobe dot, two dots after that,
// where a read loads the read buffer from the latched address or a write lands there,
// and `v` steps. `data` is the byte a write carries; `dotsToAddress` and `dotsToStrobe`
// are the ticks still to go to each dot. A second access of the same kind taken on the
// first's address dot brings the first's strobe a dot nearer, or is dropped when it
// falls in that dot's last master cycle; an access of the other kind leaves the first as
// it is (Visual 2C02: ALE three dots after the port select falls and /RD or /WR two dots
// after that; NESdev wiki, "PPU registers", The PPUDATA read buffer: the chip's read is
// too slow to serve the CPU's).
struct PpuDataAccess {
  PpuDataAccessKind kind = PpuDataAccessKind::None;
  std::uint8_t data = 0;
  std::uint8_t dotsToAddress = 0;
  std::uint8_t dotsToStrobe = 0;
  [[nodiscard]] bool operator==(const PpuDataAccess&) const noexcept = default;
};

// The chip's whole mutable state as a value: snapshot by copy, restore by assignment.
struct PpuState {
  // $2000 as last written: bit 7 enables the NMI output, bit 5 makes every sprite 8x16,
  // bit 4 selects the half of the pattern table the background's tiles are read from,
  // $0000 or $1000, bit 3 the half 8x8 sprites are read from, bit 2 selects the step
  // the address takes after a $2007 access, 1 or 32, and bits 1-0 are copied into the
  // temporary address as the nametable select (NESdev wiki, "PPU registers", PPUCTRL;
  // "PPU pattern tables", Addressing; "PPU OAM", Byte 1 - Tile/index; "PPU scrolling",
  // $2000 write). Bit 6 is held for the EXT pins and drives nothing.
  std::uint8_t ctrl = 0;
  // $2001 as last written: bits 4 and 3 enable rendering, which runs the fetches, the
  // shift registers, `v`'s moves, the sprites' evaluation and units and the odd-frame
  // skip, and decides whether a position of the picture is the multiplexer's or `v`
  // names its entry; bit 4 shows the sprites and bit 3 the background, and bits 2 and 1
  // show each in the picture's eight leftmost positions; bit 0 is greyscale, which a
  // palette read and the picture apply; and bits 7-5 are the emphasis bits every dot of
  // the picture carries (NESdev wiki, "PPU registers", PPUMASK).
  std::uint8_t mask = 0;
  // A write to $2001 reaches the frame counter on the third dot after its access.
  // `maskLag` counts the dots still to pass, and while it is not zero the odd-frame
  // skip, the fetches, the shift registers and `v`'s moves read `maskBefore`, the two
  // rendering bits as they stood before the write (NESdev wiki, "PPU registers",
  // PPUMASK, Rendering control: toggling rendering takes effect approximately 3-4 dots
  // after the write; blargg's ppu_vbl_nmi, 10-even_odd_timing, whose four results place
  // the dot).
  std::uint8_t maskBefore = 0;
  std::uint8_t maskLag = 0;
  // The vertical-blank flag, bit 7 of $2002: set at dot 1 of scanline 241, cleared at
  // dot 1 of scanline 261 and by a read of $2002 (NESdev wiki, "NMI", Operation). At
  // power-on it is what the config says.
  bool vblank = false;
  // The sprite 0 hit flag, bit 6 of $2002: set on the dot the chip selects a picture
  // position where sprite 0's pixel and the background's are both opaque. The sprite
  // overflow flag, bit 5: set when a line's evaluation finds a ninth sprite in range, by
  // its own faulty search. Both clear at dot 1 of scanline 261 and at no other time; a
  // read of $2002 answers them and leaves them (NESdev wiki, "PPU registers",
  // PPUSTATUS; "PPU OAM", Sprite 0 hits; "PPU sprite evaluation", Sprite overflow bug).
  bool sprite0Hit = false;
  bool spriteOverflow = false;
  // $2003 as last written, stepped by every write to $2004. While the chip renders it is
  // the evaluation's pointer into OAM, stepped as the evaluation reads, and it is held
  // at 0 on each of dots 257-320 of scanlines 0-239 and 261 (NESdev wiki, "PPU
  // registers", OAMADDR with its Values during rendering, and OAMDATA).
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
  // The byte the next read of $2007 answers, loaded on the strobe dot of the read before
  // it (NESdev wiki, "PPU registers", The PPUDATA read buffer).
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
  // write-only port drives nothing and reloads nothing. A set bit decays to zero (NESdev
  // wiki, "PPU registers", MMIO registers: the bus is a dynamic latch, and at least one
  // bit of it decays after 3 to 30 ms; the ppudecay test program writes a byte to $2002
  // and counts until a read of $2000 answers another).
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
  // The 256 bytes of object attribute memory, four to a sprite, reached through $2003
  // and $2004 (NESdev wiki, "PPU memory map", OAM). A sprite's byte 2 is five bits in
  // the chip, 7-5 and 1-0: the array keeps the byte as written, and a read of $2004
  // answers those five with bits 4-2 clear (NESdev wiki, "PPU OAM", Byte 2 -
  // Attributes).
  std::array<std::uint8_t, 256> oam{};
  // What the chip keeps of the tile it is fetching, each from its read until the next
  // read of its kind: the nametable byte, which names the tile; the two bits of the
  // attribute byte that are the tile's palette, selected as the byte is read by bit 1
  // of `v`'s coarse X and bit 1 of its coarse Y; and the tile's row of pixels as its two
  // pattern bytes, [0] the byte of the pixels' low bits and [1] the byte of their high
  // bits (NESdev wiki, "PPU rendering", Cycles 1-256; "PPU attribute tables"; "PPU
  // pattern tables"; Brad Taylor's NTSC 2C02 technical reference, Playfield render
  // pipeline details: the palette select data is selected from the fetched byte as it
  // is latched).
  std::uint8_t nametableLatch = 0;
  std::uint8_t attributeLatch = 0;
  std::array<std::uint8_t, 2> patternLatch{};
  // The background's shift registers. `patternShift` is the two 16-bit registers, [0]
  // for the pixels' low bits and [1] for their high bits: a reload puts a pattern byte
  // in the high eight bits with the tile's leftmost pixel at bit 8, a shift moves every
  // bit one place toward bit 0, and bit 15 then takes a 0 in [0] and a 1 in [1].
  // `attributeShift` is the two 8-bit registers that carry a pixel's palette beside it,
  // each taking at bit 7 its bit of `attributeFeed`, the two 1-bit latches a reload
  // fills from `attributeLatch`. The fine X scroll picks the pixel: bit `x` of each
  // register (NESdev wiki, "PPU rendering", Picture region and its notes).
  std::array<std::uint16_t, 2> patternShift{};
  std::array<std::uint8_t, 2> attributeShift{};
  std::uint8_t attributeFeed = 0;
  // The pixels on their way to the picture: [0] the one selected on the last dot and [1]
  // the one selected on the dot before it. A pixel is a palette address: 0 when it is
  // transparent, and otherwise the pattern's two bits under the palette's two, with bit 4
  // set for a sprite's. A position's pixel is selected two dots behind the counter and
  // reaches the picture four behind it (NESdev wiki, "PPU rendering", Cycles 1-256: the
  // shifters first shift on cycle 2, where sprite 0 hit takes the image to start, and
  // the first pixel is output during cycle 4).
  std::array<std::uint8_t, 2> pixelPipeline{};
  // The address lines as the chip last drove them, and the console's address latch. On
  // the first dot of an access the chip drives all fourteen lines and the latch captures
  // the low eight; on the second the low eight lines carry the data, the chip drives the
  // high six, and the cartridge and the nametable RAM are addressed by those six over the
  // latch. On a dot the chip makes no access on, the lines carry the low fourteen bits
  // of `v` and the latch keeps what it captured. `addressPins` keeps its low eight bits
  // as an address last put them (NESdev wiki, "PPU rendering", PPU address bus contents;
  // "PPU pinout", Signal descriptions).
  std::uint16_t addressPins = 0;
  std::uint8_t addressLatch = 0;
  // The $2007 accesses the chip has taken and not yet made, two at most, each in the
  // first entry free when it was taken; an entry whose kind is None holds nothing.
  std::array<PpuDataAccess, 2> dataAccess{};
  // Secondary OAM, the 32 bytes a line's evaluation fills with the next line's sprites,
  // four bytes each in OAM's order, cleared to $FF on dots 1-64 and read into the output
  // units on dots 257-320. `secondaryAddress` is the byte the next copied byte goes to,
  // 32 once eight sprites are found and the writes stop (NESdev wiki, "PPU OAM",
  // Internal operation; "PPU sprite evaluation", Details).
  std::array<std::uint8_t, 32> secondaryOam{};
  std::uint8_t secondaryAddress = 0;
  // The byte on the chip's OAM bus: what its last access to OAM or secondary OAM read,
  // and what a read of $2004 answers while the chip renders (NESdev wiki, "PPU
  // registers", OAMDATA; "PPU sprite evaluation", Details).
  std::uint8_t oamBus = 0;
  // Where a line's evaluation stands: the bytes of a sprite still to be read after its Y
  // was found in range, 3 to 0, and whether every sprite has been looked at, after which
  // the evaluation reads a Y every other dot and copies nothing (NESdev wiki, "PPU
  // sprite evaluation", Details).
  std::uint8_t copyLeft = 0;
  bool evaluationDone = false;
  // The chip's two sprite 0 flags: whether the first sprite a line's evaluation looked
  // at, sprite 0 while the OAM address is 0, was found in range for the next line, and
  // whether it was for the line being drawn, which dot 257 copies from the first and
  // the hit consults (NESdev wiki, "PPU OAM", Internal operation; "PPU registers",
  // OAMADDR, Values during rendering).
  bool sprite0Next = false;
  bool sprite0Current = false;
  // The eight sprite output units, unit 0 the frontmost, each loaded on dots 257-320
  // from its slot of secondary OAM for the next line (NESdev wiki, "PPU OAM", Internal
  // operation; "PPU sprite priority", Detailed internals of the sprite priority quirk).
  std::array<SpriteUnit, 8> spriteUnits{};
  [[nodiscard]] bool operator==(const PpuState&) const noexcept = default;
};
```

A frame is 341 dots by 262 scanlines with rendering disabled (NESdev wiki, "PPU frame timing",
Even/Odd Frames):

```cpp
inline constexpr std::uint16_t kDotsPerScanline = 341;
inline constexpr std::uint16_t kScanlinesPerFrame = 262;
```

`ctrl` and `mask` are `$2000` and `$2001` as last written, and `maskBefore` and `maskLag` carry a
`$2001` write to the frame counter; `v`, `t`, `x` and `w` are the scroll file the "PPU scrolling"
page names; the I/O bus and its eight decay counts, the warm-up window, the counter's dot, scanline
and parity, and the three memories follow; the fetch latches, the shift registers and the two pixels
on their way to the picture are [the background's](#the-background); the address lines with the
console's latch and the `$2007` accesses on their way are [the pins'](#the-pins); and secondary OAM
with the evaluation's place, the OAM bus, the two sprite 0 marks and the output units are [the
sprites'](#the-sprites). Every field is plain, so a snapshot taken on any dot of any line restores
to the same place.

## The ports

The eight ports repeat every eight bytes through `$3FFF`, and `port` is the address's low three
bits. Every write loads the I/O bus whole, and every read of a write-only port answers it:

| Port | A write | A read answers | The read's side effects |
|---|---|---|---|
| `$2000` | `ctrl`, and bits 1–0 into bits 11–10 of `t`; ignored in the warm-up | the bus | none |
| `$2001` | `mask`, which the frame counter reads from the write's third dot; ignored in the warm-up | the bus | none |
| `$2002` | the bus and nothing more | the vertical-blank flag in bit 7, the sprite 0 hit flag in bit 6 and the sprite overflow flag in bit 5 over the bus's low five bits | the vertical-blank flag and the toggle cleared; bits 7–5 of the bus driven |
| `$2003` | the OAM address | the bus | none |
| `$2004` | the OAM byte at the address, which then steps | the OAM byte at the address, a sprite's byte 2 with bits 4–2 clear, or the byte on the chip's OAM bus while it renders | the bus driven whole; the address unmoved |
| `$2005` | the first write coarse X into `t` and fine X into `x`, the second fine Y and coarse Y into `t`; the toggle flips; ignored in the warm-up | the bus | none |
| `$2006` | the first write bits 13–8 of `t` with bit 14 cleared, the second its low byte and then the whole of `t` into `v`; the toggle flips; ignored in the warm-up | the bus | none |
| `$2007` | the byte through the chip's address space at `v`, then `v` steps by 1 or 32, on the fifth dot after the access | the read buffer, or a palette entry at once under the bus's top two bits | the bus driven; the buffer refilled from `v` and `v` stepped on the fifth dot after the access |

(NESdev wiki, "PPU registers", MMIO registers, PPUCTRL, PPUMASK, PPUSTATUS, OAMADDR, OAMDATA,
PPUSCROLL, PPUADDR, PPUDATA; "PPU scrolling", Register controls; "PPU OAM", Byte 2 - Attributes;
"Open bus behavior", PPU open bus.)

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
neasant::ppuWrite(chip, 7, 0x5A, inputs);  // $2007: the chip takes a write to $2108
for (int dot = 0; dot < 5; ++dot) neasant::ppuTickDot(chip, false, inputs);  // its five dots
// chip.v == 0x2109: stepped on the fifth dot, where the byte landed
// chip.ciram[0x108] == 0x5A: $2108 is byte $108 of the first kilobyte under horizontal
//                            mirroring
// chip.ioLatch == 0x5A: every port write loads the I/O bus
// neasant::ppuRead(chip, 0, inputs) == 0x5A: a write-only port answers the bus
```

The examples in the next five sections continue on the same `chip`, in order, and so do those
under [The I/O bus](#the-io-bus), [Its decay](#its-decay) and [The palette](#the-palette).

### $2000 and $2001

Bit 7 of `$2000` enables the NMI output, bit 5 makes every sprite 8x16, bit 4 selects the half of
the pattern table the background's tiles are read from, `$0000` or `$1000`, bit 3 the half 8x8
sprites are read from, bit 2 selects the step `v` takes after a `$2007` access, 1 or 32, and bits
1–0 are copied into the temporary address as the nametable select; bit 6 is held for the EXT pins
and drives nothing. `$2001` bits 4 and 3 enable rendering, which runs the fetches, the shift
registers, `v`'s moves, the sprites' evaluation and units and the odd-frame skip, and decides
whether a position of the picture is the multiplexer's or `v` names its entry; bit 4 shows the
sprites and bit 3 the background, and bits 2 and 1 show each in the picture's eight leftmost
positions; bit 0 is greyscale, which a palette read and the picture apply; and bits 7–5 are the
emphasis bits every dot of the picture carries (NESdev wiki, "PPU registers", PPUCTRL and PPUMASK;
"PPU pattern tables", Addressing; "PPU OAM", Byte 1 - Tile/index; "PPU scrolling", $2000 write).

```cpp
neasant::ppuWrite(chip, 0, 0x83, inputs);
// chip.ctrl == 0x83 and chip.t == 0x2D08: bits 1-0 of the value went to bits 11-10 of the
//                                         temporary address, which held $2108 from the writes
// neasant::ppuNmiAsserted(chip) == false: bit 7 is set but the vertical-blank flag is clear
neasant::ppuWrite(chip, 1, 0x1E, inputs);
// chip.mask == 0x1E: rendering is enabled, so the chip fetches and draws the background
//                    and the sprites, and an odd frame skips its last dot
```

### $2002

A read answers the vertical-blank flag in bit 7, the sprite 0 hit flag in bit 6 and the sprite
overflow flag in bit 5 over the I/O bus's low five bits, then clears the vertical-blank flag and the
write toggle, leaving the two sprite flags ([The sprites](#the-sprites)), and drives bits 7–5 of the
bus with what it answered. A write to `$2002` loads the bus and nothing else (NESdev wiki, "NMI",
Operation; "PPU registers", PPUSTATUS; "PPU scrolling", $2002 read; "Open bus behavior", PPU open
bus).

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
there and does not step it. While the chip renders, on scanlines 0–239 and 261 with rendering
enabled, a read answers the byte on the chip's OAM bus instead, what its own access to OAM or
secondary OAM last read, and the OAM address is the evaluation's pointer, held at 0 on dots 257–320,
as [the evaluation](#the-evaluation) describes (NESdev wiki, "PPU registers", OAMADDR and OAMDATA).

```cpp
neasant::ppuWrite(chip, 3, 0x10, inputs);
neasant::ppuWrite(chip, 4, 0xAA, inputs);
neasant::ppuWrite(chip, 4, 0xBB, inputs);
// chip.oam[0x10] == 0xAA, chip.oam[0x11] == 0xBB and chip.oamAddr == 0x12: a write steps it
neasant::ppuWrite(chip, 3, 0x10, inputs);
const std::uint8_t first = neasant::ppuRead(chip, 4, inputs);
// first == 0xAA, and chip.oamAddr is still 0x10: a read does not
```

A sprite's byte 2, its attributes, is five bits in the chip: bits 7–5 and 1–0. `oam` keeps the
byte as written, and a read of `$2004` answers the five with bits 4–2 clear and drives the I/O bus
with what it answered (NESdev wiki, "PPU OAM", Byte 2 - Attributes).

```cpp
neasant::ppuWrite(chip, 3, 0x12, inputs);  // sprite 4's byte 2
neasant::ppuWrite(chip, 4, 0xFF, inputs);
neasant::ppuWrite(chip, 3, 0x12, inputs);
const std::uint8_t attributes = neasant::ppuRead(chip, 4, inputs);
// attributes == 0xE3: bits 4-2 read back as 0
// chip.oam[0x12] == 0xFF: the array keeps the byte as written
// chip.ioLatch == 0xE3: the read drives the bus with what it answered
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

A `$2007` access is taken in the dot under way and made on the chip's own dots: on the third
dot after the access the chip drives its address lines and the console's latch from the low 14
bits of `v` and presents the board the address alone, and on the fifth a read loads the read
buffer from that address or a write lands there, and `v` steps by 1 or 32, as `$2000` bit 2 says,
wrapping inside its 15 bits ([The pins](#the-pins)). A write lands through the chip's
address space: a palette entry when `v` is `$3F00` or above, otherwise the board or the console's
nametable RAM ([The address space](#the-address-space)). A read answers the read buffer as it
stands at the access, so a read lags one access behind the address. A palette address is the
exception, answered at once under the bus's top two bits while the read underneath fills the
buffer on the fifth dot (NESdev wiki, "PPU registers", PPUDATA, The PPUDATA read buffer and
Reading palette RAM; Visual 2C02).

```cpp
neasant::ppuWrite(chip, 1, 0x00, inputs);  // rendering off: the dots below make no fetch
cartridge.chrRom[0x10] = 0xA1;  // three CHR ROM bytes to read
cartridge.chrRom[0x11] = 0xB2;
cartridge.chrRom[0x12] = 0xC3;
neasant::ppuWrite(chip, 6, 0x00, inputs);
neasant::ppuWrite(chip, 6, 0x10, inputs);  // v is $0010
const std::uint8_t stale = neasant::ppuRead(chip, 7, inputs);
// stale == 0x00: the buffer as it stood; the chip holds the read
// chip.dataAccess[0].kind == neasant::PpuDataAccessKind::Read and chip.readBuffer == 0x00
for (int dot = 0; dot < 5; ++dot) neasant::ppuTickDot(chip, false, inputs);
// chip.readBuffer == 0xA1 and chip.v == 0x0011: the read made on its fifth dot
const std::uint8_t fresh = neasant::ppuRead(chip, 7, inputs);
for (int dot = 0; dot < 5; ++dot) neasant::ppuTickDot(chip, false, inputs);
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
is the result lidnariq's `ppudecay` test program reports on its author's NTSC NES at power-on:
the program writes a value to `$2002` and polls `$2000` every fifteen CPU cycles until the byte
changes, counting `$A00` polls, 38 400 CPU cycles, about 21 ms (its source and its author's
description). The bus is a dynamic latch, and at least one bit of it decays after 3 to 30 ms,
sooner as the PPU warms (NESdev wiki, "PPU registers", MMIO registers), so the chip ages each bit
on its own; a decayed bit reads 0.

```cpp
const neasant::PpuInputs quick{board, cartridge, chrRam, 10u};  // ten dots
neasant::ppuWrite(chip, 2, 0xFF, quick);
// chip.ioLatchDecay[0] == 10, and so on up to chip.ioLatchDecay[7]
for (int dot = 0; dot < 9; ++dot) neasant::ppuTickDot(chip, false, quick);
// neasant::ppuRead(chip, 0, quick) == 0xFF: nine dots on, every bit holds
neasant::ppuTickDot(chip, false, quick);
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
RAM; "PPU registers", Reading palette RAM and Color control; "PPU memory map"). A palette
access's address is presented to the board on its address dot; a write's strobe dot presents
nothing, `/WR` staying high, and a read's presents its read, which the console's RAM answers
under NROM ([The pins](#the-pins)).

```cpp
neasant::ppuWrite(chip, 6, 0x3F, inputs);
neasant::ppuWrite(chip, 6, 0x10, inputs);  // v is $3F10
neasant::ppuWrite(chip, 7, 0x21, inputs);  // for entry 0: $3F10 is the storage of $3F00
for (int dot = 0; dot < 5; ++dot) neasant::ppuTickDot(chip, false, inputs);
// chip.palette[0x00] == 0x21 and chip.palette[0x10] == 0x00
chip.palette[0x05] = 0x2C;
chip.ciram[0x705] = 0x77;  // the nametable byte underneath $3F05 under horizontal mirroring
neasant::ppuWrite(chip, 6, 0x3F, inputs);
neasant::ppuWrite(chip, 6, 0x05, inputs);
neasant::ppuWrite(chip, 2, 0xC3, inputs);  // the bus's top two bits set
const std::uint8_t entry = neasant::ppuRead(chip, 7, inputs);
// entry == 0xEC: the entry at once, under the bus's top two bits
for (int dot = 0; dot < 5; ++dot) neasant::ppuTickDot(chip, false, inputs);
// chip.readBuffer == 0x77: the read underneath fills the buffer on the fifth dot
```

## The pins

The board sees the chip's fourteen address lines, its two strobes and its data lines once for
each dot the lines take a new value or a strobe is low ([the cartridge
page](nes-cartridge.md#the-pins)). `addressPins` holds the lines as the chip last drove them and
`addressLatch` the console's latch, the low eight bits an access's first dot put there: only an
access's first dot, ALE, captures it. On a dot the chip makes no access on — rendering disabled,
scanlines 240 to 260 but for dot 0 of 240, and dot 0 of the pre-render line — the lines carry
the low 14 bits of `v`, taking a new value on the dot after `v` changes, and the latch keeps its
byte (NESdev wiki, "PPU rendering", PPU address bus contents; "PPU pinout", Signal descriptions;
Visual 2C02).

A `$2007` access is one of the chip's accesses. The CPU's access falls on one of the four master
cycles of the dot under way, `PpuInputs::accessCycle`, which [the
machine](nes-machine.md#a-machine-cycle) sets from its alignment, 1, 0, 3 and 2 under alignments 0
to 3, and a bare chip's caller leaves at 1. The chip takes the access and makes it three and five
ticks on, one more each when the cycle is 0, the dot having yet to tick: on the address dot the
lines and the latch take `v` and the board is presented the address alone; on the strobe dot the
chip presents the read or the write, a palette write's excepted, and the buffer, the memory and `v`
move. `dataAccess` holds the accesses taken and not yet made, two at most. Two accesses of a kind on
adjacent CPU cycles meet: the second is taken in the first's address dot, and the first's strobe
comes on the very next dot, unless the access falls on the dot's last master cycle, alignment 2,
where the second is dropped; a read and a write on adjacent cycles each keep their dots. The
console's own measure of this is blargg's `double_2007_read` and `dma_2007_read`, whose outputs the
machine prints by alignment ([the APU page](apu.md#testing-against-the-programs)) (NESdev wiki, "PPU
registers", The PPUDATA read buffer and Read conflict with DPCM samples; "DMA", Register conflicts;
Visual 2C02).

Across a picture line the chip drives 171 addresses, dot 0's among them, and address line 13
rises 43 times; the pre-render line makes no access on dot 0 ([A line's dots](#a-lines-dots)).

```cpp
neasant::PpuState lines;  // on the same `inputs`
lines.warmingUp = false;
lines.addressLatch = 0xEE;
neasant::ppuWrite(lines, 6, 0x0A, inputs);
neasant::ppuWrite(lines, 6, 0x5C, inputs);  // v is $0A5C
neasant::ppuTickDot(lines, false, inputs);
// lines.addressPins == 0x0A5C and lines.addressLatch == 0xEE: the lines follow v; no ALE
cartridge.chrRom[0x0A5C] = 0x99;
const std::uint8_t held = neasant::ppuRead(lines, 7, inputs);  // on master cycle 1 of the dot
// held == 0x00: the buffer as it stood
// lines.dataAccess[0].dotsToAddress == 3 and lines.dataAccess[0].dotsToStrobe == 5
for (int dot = 0; dot < 3; ++dot) neasant::ppuTickDot(lines, false, inputs);
// lines.addressLatch == 0x5C: the address dot, ALE
for (int dot = 0; dot < 2; ++dot) neasant::ppuTickDot(lines, false, inputs);
// lines.readBuffer == 0x99 and lines.v == 0x0A5D: the strobe dot
neasant::ppuTickDot(lines, false, inputs);
// lines.addressPins == 0x0A5D: the lines follow the stepped v
```

## The frame

The counter runs 341 dots a scanline and 262 scanlines a frame, with a parity that toggles as
each frame ends, and `ppuTickDot` steps it one dot and answers what the dot was. The
vertical-blank flag is set at dot 1 of scanline 241, the dot answered as `VerticalBlank`, and
cleared at dot 1 of scanline 261, 6820 dots later, where the warm-up window closes too; the dot
that wraps the counter is answered as `FrameEnded` (NESdev wiki, "NMI", Operation; "PPU frame
timing", Even/Odd Frames and VBL Flag Timing; "PPU power up state"). The examples in this section
and the next two hold no picture:

```cpp
std::vector<std::uint8_t> image = programImage({});
neasant::Cartridge cartridge = neasant::makeCartridge(image);
neasant::Board board = neasant::boardFor(cartridge);
std::vector<std::uint8_t> chrRam(cartridge.chrRamBytes);
const neasant::PpuInputs inputs{board, cartridge, chrRam, 115200u};  // no picture held

neasant::PpuState counter;
int ticks = 0;
while (!(counter.scanline == 241 && counter.dot == 0)) {
  neasant::ppuTickDot(counter, false, inputs);
  ++ticks;
}
// ticks == 82181: scanline 241, dot 0
const neasant::PpuDotEvent set = neasant::ppuTickDot(counter, false, inputs);
// set == neasant::PpuDotEvent::VerticalBlank and counter.vblank == true: dot 1 of scanline 241
for (int dot = 0; dot < 6820; ++dot) neasant::ppuTickDot(counter, false, inputs);
// counter.scanline == 261, counter.dot == 1 and counter.vblank == false: cleared 6820 dots on
// counter.warmingUp == false: the same dot closes the warm-up window
neasant::PpuDotEvent last = neasant::PpuDotEvent::None;
for (int dot = 0; dot < 340; ++dot) last = neasant::ppuTickDot(counter, false, inputs);
// last == neasant::PpuDotEvent::FrameEnded: the 89342nd tick wraps the counter to scanline 0,
//                                           dot 0, on an odd frame
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
while (neasant::ppuTickDot(rendering, false, inputs) != neasant::PpuDotEvent::FrameEnded) ++even;
// even + 1 == 89342: an even frame is whole
int odd = 0;
while (neasant::ppuTickDot(rendering, false, inputs) != neasant::PpuDotEvent::FrameEnded) ++odd;
// odd + 1 == 89341: the odd frame leaves scanline 261 from dot 339
```

A write to `$2001` reaches the skip on the third dot after its access. Until then the counter
reads the rendering bits as they stood before the write, which `maskBefore` holds while `maskLag`
counts the dots. A write that enables the background while the counter stands at dot 337 of an
odd frame's scanline 261 is read at dot 339 and the dot is skipped; the same write one dot later
leaves the frame whole (NESdev wiki, "PPU registers", PPUMASK, Rendering control; blargg's
`ppu_vbl_nmi`, `10-even_odd_timing`).

```cpp
neasant::PpuState late;
late.warmingUp = false;
late.odd = true;
late.scanline = 261;
late.dot = 337;
neasant::ppuWrite(late, 1, 0x08, inputs);  // the background enabled at dot 337
// late.maskLag == 3 and late.maskBefore == 0x00: the counter reads the bits before the write
neasant::ppuTickDot(late, false, inputs);  // dot 338
neasant::ppuTickDot(late, false, inputs);  // dot 339
const neasant::PpuDotEvent third = neasant::ppuTickDot(late, false, inputs);  // reads the write
// third == neasant::PpuDotEvent::FrameEnded, late.scanline == 0 and late.dot == 0: the frame
//                                                                        ended a dot short
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

## The background

With rendering enabled, either of `$2001` bits 4 and 3, the chip draws the background the
nametables lay out: 30 rows of 32 tiles, a tile eight dots by eight lines, its shape two bytes a
row in a pattern table and its palette two bits of an attribute byte. The chip fetches a tile
every eight dots through the board, keeps the rows of the next two tiles in its shift registers,
and takes one pixel a dot from them. The background runs on scanlines 0–239 and on scanline 261,
the pre-render line, which fetches as a line of the picture does and draws nothing (NESdev wiki,
"PPU rendering", Drawing overview and Line-by-line timing; "PPU nametables").

### A tile's four accesses

An access to the chip's address space takes two dots. On the first the chip drives all fourteen
address lines and the console's latch captures the low eight. On the second the low eight lines
carry the data, the chip drives the high six, and the read is made at those six over the latch.
`addressPins` and `addressLatch` are the lines and the latch as the chip last drove them ([The
pins](#the-pins)).
The board is presented on both dots: the address alone, with neither strobe low, and then the
read ([the cartridge page](nes-cartridge.md#the-pins)). A tile is four accesses, each byte kept
in its latch until the next of its kind:

| Dots of the eight | The byte | Its address | Kept as |
|---|---|---|---|
| 1 and 2 | the nametable byte, which names the tile | `$2000 \| (v & $0FFF)` | `nametableLatch` |
| 3 and 4 | the attribute byte | `$23C0 \| (v & $0C00) \| ((v >> 4) & $38) \| ((v >> 2) & $07)` | `attributeLatch`, the two bits of the tile's quarter |
| 5 and 6 | the low byte of the tile's row | `$2000` bit 4 in bit 12, the nametable byte in bits 11–4, `v`'s fine Y in bits 2–0 | `patternLatch[0]` |
| 7 and 8 | the high byte of the row | the same address with bit 3 set | `patternLatch[1]` |

An attribute byte covers four tiles by four in quarters of two by two: bits 1–0 the top left,
3–2 the top right, 5–4 the bottom left and 7–6 the bottom right. Bit 1 of `v`'s coarse X and bit
1 of its coarse Y name the tile's quarter, and the chip keeps that pair as the byte is read
(NESdev wiki, "PPU rendering", Cycles 1-256 and PPU address bus contents; "PPU scrolling", Tile
and attribute fetching; "PPU pattern tables", Addressing; "PPU attribute tables"; "PPU pinout",
Signal descriptions).

```cpp
std::vector<std::uint8_t> image = programImage({});
image[16 + 16384 + 0x04E5] = 0x41; // CHR ROM: row 5 of tile $4E, its low byte
image[16 + 16384 + 0x04ED] = 0x16; // and its high byte
neasant::Cartridge cartridge = neasant::makeCartridge(image);
neasant::Board board = neasant::boardFor(cartridge);
std::vector<std::uint8_t> chrRam(cartridge.chrRamBytes);
const neasant::PpuInputs inputs{board, cartridge, chrRam, 115200u}; // no picture held

neasant::PpuState chip;
chip.warmingUp = false;
chip.mask = 0x0A; // the background shown, its left column too
chip.scanline = 10; // a line of the picture, the counter at its dot 0
chip.v = 0x54A7; // fine Y 5, nametable 1, coarse Y 5, coarse X 7
chip.ciram[0x0A7] = 0x4E; // $24A7: the tile
chip.ciram[0x3C9] = 0xB4; // $27C9: its attribute byte, the top right quarter %01
neasant::ppuTickDot(chip, false, inputs); // dot 1
// chip.addressPins == 0x24A7 and chip.addressLatch == 0xA7: the address, and no read yet
neasant::ppuTickDot(chip, false, inputs); // dot 2
// chip.nametableLatch == 0x4E
for (int dot = 3; dot <= 8; ++dot) neasant::ppuTickDot(chip, false, inputs);
// chip.attributeLatch == 1: bits 3-2 of $B4, coarse X 7 and coarse Y 5 being the top right
// chip.patternLatch[0] == 0x41 and chip.patternLatch[1] == 0x16, read at $04E5 and $04ED
// chip.addressPins == 0x04ED and chip.addressLatch == 0xED
// chip.v == 0x54A8: dot 8 stepped coarse X
```

### A line's dots

A line's accesses run in eights from dot 1:

| Dots | Accesses | `v` | The shift registers |
|---|---|---|---|
| 0 | the address of the next low pattern byte, and no read | | |
| 1–256 | 32 tiles | coarse X steps on every eighth dot, 8 to 256; Y steps on dot 256 | shift on dots 2–257; reload on dot 9 and every eighth dot to 257 |
| 257–320 | in each eight, two nametable fetches, the first kept and the second dropped, then a sprite's two pattern fetches | `t`'s horizontal bits on dot 257; on scanline 261, `t`'s vertical bits on each of dots 280–304 | |
| 321–336 | the next line's first two tiles | coarse X steps on dots 328 and 336 | shift on dots 322–337; reload on dots 329 and 337 |
| 337–340 | two nametable fetches, the first kept and the second dropped | | |

The 32 tiles of dots 1–256 are the line's third to its thirty-fourth: the first two were fetched on
dots 321–336 of the line before, and the thirty-fourth is fetched and never drawn. The nametable
fetches of dots 257–320 and 337–340 read the tile `v` points at; the first one after dot 256 drives
its low eight lines as `v` stood when dot 257 began and its high six as that dot's copy from `t`
leaves them. The move `v` makes on a dot falls after the latch has captured an address's low eight
and before the high six lines and a read are taken. The four dots that follow each pair on dots
257–320 fetch the two pattern bytes of the sprite in the eight's slot of secondary OAM ([The output
units](#the-output-units)).

Dot 0 drives an address on scanlines 0–240: the low pattern byte's, for the tile the last kept
nametable byte names. On an odd frame the counter goes from dot 339 of scanline 261 to dot 0 of
scanline 0, and that dot makes the read dot 340 would have made in place of dot 0's address
(NESdev wiki, "PPU rendering", Pre-render scanline, Cycle 0, Cycles 1-256, Cycles 257-320,
Cycles 321-336 and Cycles 337-340, and its frame timing diagram with its legend; "PPU
nametables", Background evaluation).

### The scroll while rendering

While the chip renders, `v` is the address of the tile being fetched: coarse X in bits 4–0,
coarse Y in bits 9–5, the nametable in bits 11–10 and fine Y, the row within the tile, in bits
14–12. A step of coarse X goes from 31 to 0 and switches the horizontal nametable, bit 10. A step
of Y steps fine Y, which goes from 7 to 0 and steps coarse Y; coarse Y goes from 29 to 0 and
switches the vertical nametable, bit 11, and from 31 to 0 without switching it, so a coarse Y of
30 or 31 reads the attribute table's bytes as tiles. Dot 257 copies coarse X and bit 10 from
`t`, and dots 280–304 of the pre-render line copy fine Y, coarse Y and bit 11, which is how the
scroll a program writes through `$2000` and `$2005` reaches the picture: `t` holds where the
picture starts, the fine X scroll `x` picks the pixel within the tile, and `v` walks from there
(NESdev wiki, "PPU scrolling", Register controls and Wrapping around).

```cpp
neasant::PpuState line; // on the same `inputs`
line.warmingUp = false;
line.mask = 0x0A;
line.scanline = 10;
line.dot = 247;
line.v = 0x73BF; // fine Y 7, coarse Y 29, coarse X 31
line.t = 0x0005; // coarse X 5 in the first nametable
neasant::ppuTickDot(line, false, inputs); // dot 248
// line.v == 0x77A0: coarse X went from 31 to 0 and bit 10 switched
for (int dot = 249; dot <= 256; ++dot) neasant::ppuTickDot(line, false, inputs);
// line.v == 0x0C01: dot 256 stepped coarse X to 1 and Y, fine Y 7 to 0 and coarse Y 29 to 0
// with bit 11 switched
neasant::ppuTickDot(line, false, inputs); // dot 257
// line.v == 0x0805: coarse X and bit 10 are t's
```

### The shift registers and the pixel

`patternShift` is two 16-bit registers, one for the low bit of each pixel and one for the high
bit. A reload puts the two pattern latches in their high eight bits, the tile's leftmost pixel
at bit 8, and a shift moves every bit one place toward bit 0, the low register taking a 0 at bit
15 and the high one a 1. `attributeShift` is two 8-bit registers that carry each pixel's palette
beside it; each takes at bit 7 its bit of `attributeFeed`, the two 1-bit latches a reload fills
from `attributeLatch`. So the low eight bits hold the tile being drawn and the high eight the
tile after it.

On each of dots 2–257 of a line of the picture the chip selects the background's pixel and then
shifts: the fine X scroll picks bit `x` of each register, the pattern's two bits under the palette's
two. The four bits are an address into palette RAM, and a pattern of 0 is transparent whatever its
palette. Every pixel is transparent while `$2001` bit 3 is clear, and the eight leftmost while bit 1
is. The pixel selected on dot `x + 2`, the background's or a sprite's through [the
multiplexer](#the-sprites-pixel-and-the-multiplexer), is position `x`'s; it waits in `pixelPipeline`
and reaches [the picture](#the-picture) on dot `x + 4` (NESdev wiki, "PPU rendering", Picture region
and its notes, and Cycles 1-256; "PPU registers", PPUMASK, Rendering control).

```cpp
std::vector<std::uint16_t> picture(neasant::kPictureDots, 0xFFFF); // a value no dot takes
const neasant::PpuInputs held{board, cartridge, chrRam, 115200u, picture};

neasant::PpuState drawn;
drawn.warmingUp = false;
drawn.mask = 0x0A;
drawn.palette[0x00] = 0x0F; // the backdrop
drawn.palette[0x01] = 0x21; // the first palette's first colour
drawn.scanline = 10;
drawn.dot = 1;
drawn.patternShift[0] = 0x0001; // a pixel of 1 at bit 0, where a fine X of 0 picks
neasant::ppuTickDot(drawn, false, held); // dot 2
// drawn.pixelPipeline[0] == 1: position 0's pixel, selected before the dot's shift
neasant::ppuTickDot(drawn, false, held); // dot 3
neasant::ppuTickDot(drawn, false, held); // dot 4
// picture[10 * 256] == 0x21: the pixel reaches the picture two dots after its selection
neasant::ppuTickDot(drawn, false, held); // dot 5
// picture[10 * 256 + 1] == 0x0F: a transparent pixel shows the backdrop
```

## The sprites

With rendering enabled the chip draws up to 64 sprites from object attribute memory, each a tile
eight dots wide and eight or sixteen lines tall, over or under the background. A line shows at
most eight of them: while the chip draws a line it reads through OAM for the sprites that have a
row on the next line, copies the first eight it finds into secondary OAM, fetches their rows
through the board after the line's last tile, and loads eight output units that shift the rows
out as the next line is drawn. The evaluation runs on scanlines 0–239, the loading on scanlines
0–239 and 261, and every part of it only while rendering is enabled (NESdev wiki, "PPU OAM",
Internal operation; "PPU sprite evaluation"; "PPU rendering", Line-by-line timing).

### A sprite's four bytes

| Byte | Holds |
|---|---|
| 0 | Y: the line above the sprite's first row, so a sprite at Y shows from line Y + 1; $EF–$FF keep it off the picture |
| 1 | the tile: for an 8x8 sprite its number in the half of the pattern table `$2000` bit 3 selects; for an 8x16 sprite bit 0 selects the half, `$0000` or `$1000`, and the even number above it names the top tile, the one after it the bottom |
| 2 | the attributes: bits 1–0 the palette, 4 to 7; bit 5 the priority, 0 in front of the background and 1 behind it; bit 6 a horizontal flip; bit 7 a vertical flip. Bits 4–2 are not in the chip and read as 0 |
| 3 | X: the position of the sprite's left column |

(NESdev wiki, "PPU OAM", OAM (Sprite) Data; "PPU registers", PPUCTRL.) A vertical flip of an
8x16 sprite flips each tile and swaps the two. Nothing of a sprite at X = $F9–$FF past position
255 shows, and a sprite cannot be partly off the left edge: `$2001` bit 2 hides the eight
leftmost positions instead.

### The evaluation

On scanlines 0–239 the chip evaluates OAM for the next line while it draws this one, reading
OAM on the odd dots of 65–256 and acting on the even dots. Its pointer is the OAM address,
`oamAddr`, which the dots 257–320 before it have held at 0 unless the program has written
`$2003` since:

| Dots | What the chip does |
|---|---|
| 1–64 | clears secondary OAM: each odd dot reads $FF onto the OAM bus and each even dot writes it to the next byte; dot 1 clears the mark that says sprite 0 is on the next line |
| 65 | begins the evaluation at the first free byte of secondary OAM; the chip being a 2C02G, if the OAM address is 8 or more the eight-byte row of OAM it is in is copied over OAM's first eight bytes |
| 65, 67, …, 255 | read the byte at the OAM address onto the OAM bus, a byte 2 as its five bits |
| 66, 68, …, 256 | act on the byte read: a Y goes to the free byte of secondary OAM, and in range, the line less Y a row of the sprite, its sprite's three other bytes follow it on the next three pairs of dots and the free byte moves on; out of range, the pointer moves to the next sprite. The first sprite looked at, in range, marks sprite 0 for the next line |
| after the eighth sprite found | the writes stop: a Y in range sets the overflow flag and three more bytes are read; a Y out of range moves the pointer to the next sprite and to the next byte at once, so the search reads the sprites' bytes diagonally, byte 1 of the next, byte 2 of the one after, round the four |
| after sprite 63, or after the ninth sprite's three bytes | the evaluation is done: each remaining pair reads a Y and moves the pointer a sprite, copying nothing |

The free byte after the last sprite copied holds the last Y the search read, sprite 63's, and
the bytes after it the clear's $FF. The comparison is against the line's number, so a sprite
below the line is never in range, nor one at Y = $FF, and a sprite's height is 8 or 16 rows as
`$2000` bit 5 stands on the dot of the comparison. The flag the ninth sprite sets, bit 5 of
`$2002`, stands until dot 1 of scanline 261 (NESdev wiki, "PPU sprite evaluation", Details, its
numbered steps, Sprite overflow bug and Notes; "PPU OAM", Byte 0 - Y position and Internal
operation; "PPU registers", OAMADDR, Values during rendering and OAMADDR precautions, and
PPUSTATUS, Sprite overflow flag; Brad Taylor's NTSC 2C02 technical reference, In-range object
evaluation).

```cpp
std::vector<std::uint8_t> image = programImage({});
neasant::Cartridge cartridge = neasant::makeCartridge(image);
neasant::Board board = neasant::boardFor(cartridge);
std::vector<std::uint8_t> chrRam(cartridge.chrRamBytes);
const neasant::PpuInputs inputs{board, cartridge, chrRam, 115200u}; // no picture held

neasant::PpuState chip;
chip.warmingUp = false;
chip.mask = 0x1E; // the background and the sprites shown, their left columns too
chip.oam.fill(0xF8); // every sprite below the picture
chip.oam[12] = 10; // sprite 3: Y 10, in range on scanline 10
chip.oam[13] = 0x4E; // its tile
chip.oam[14] = 0xFF; // its attributes, five bits in the chip
chip.oam[15] = 0x42; // its X
chip.oam[20] = 5; // sprite 5: Y 5, its other bytes $F8
chip.scanline = 10; // the counter at dot 0 of a line of the picture
for (int dot = 1; dot <= 64; ++dot) neasant::ppuTickDot(chip, false, inputs);
// chip.secondaryOam[0] == 0xFF and chip.oamBus == 0xFF: the clear
for (int dot = 65; dot <= 256; ++dot) neasant::ppuTickDot(chip, false, inputs);
// chip.secondaryOam[0] == 10, [1] == 0x4E, [2] == 0xE3 and [3] == 0x42: sprite 3, its byte 2 as
//                                                                      five bits
// chip.secondaryOam[4] == 5, [5] == 0xF8, [6] == 0xE0 and [7] == 0xF8: sprite 5
// chip.secondaryOam[8] == 0xF8 and chip.secondaryOam[9] == 0xFF: the last Y looked at, sprite
//                                                                63's, then the clear's byte
// chip.secondaryAddress == 8 and chip.evaluationDone == true
// chip.sprite0Next == false: sprite 0 is not on the next line
// chip.oamAddr == 104: the search passed sprite 63 on dot 204 and moved on a sprite every
//                      pair of dots after, 26 of them
for (int dot = 257; dot <= 340; ++dot) neasant::ppuTickDot(chip, false, inputs);
// chip.oamAddr == 0: dots 257-320 held it there
for (std::size_t n = 0; n < 9; ++n) chip.oam[4 * n] = 20; // nine sprites at Y 20
chip.scanline = 20;
chip.dot = 0;
for (int dot = 1; dot <= 129; ++dot) neasant::ppuTickDot(chip, false, inputs);
// chip.spriteOverflow == false: eight found by dot 128, the ninth's Y read on dot 129
neasant::ppuTickDot(chip, false, inputs); // dot 130
// chip.spriteOverflow == true
```

### The output units

On dots 257–320 of scanlines 0–239 and 261 the chip loads the eight output units, one each
eight dots, from the slots of secondary OAM in order, and holds the OAM address at 0. In each
eight the first four dots read the slot's Y, tile, attributes and X onto the OAM bus and the
last four read its X again; the attributes enter the unit on the third dot and the X its counter
on the fourth; the fifth to eighth dots are the two pattern fetches, the low byte of the
sprite's row and then the high, through the board like a background tile's, where the pair of
nametable fetches that opens the eight stands in for the tile's nametable and attribute fetches
([A line's dots](#a-lines-dots)).

The row is the line less the sprite's Y: an 8x8 sprite's row of its tile in the half `$2000`
bit 3 selects, `$1000` with the bit set; an 8x16 sprite's row of its top or bottom tile in the
half bit 0 of its tile byte selects; from the bottom when the sprite is flipped vertically. A
byte enters the unit's register with the leftmost pixel at bit 0, which reverses it unless the
sprite is flipped horizontally. A slot whose Y has no row on the line, the $FF of a slot no
sprite filled or a sprite found for a line this is not, still has its two addresses driven,
tile $FF's for the empty slot, and its unit's registers are loaded with 0. On scanline 261 the
slots are scanline 239's; the addresses driven take 261 for the line, so a $FF slot's row is 6,
flipped by its $FF attribute to 1, while the units are loaded with 0 as for a line of −1:
nothing shows on the first line of the picture (NESdev wiki, "PPU rendering", Cycles 257-320
and Pre-render scanline; "PPU sprite evaluation", Details, Cycles 257-320 and Notes; "PPU
pattern tables", Addressing; "PPU OAM", Byte 1 - Tile/index and Byte 2 - Attributes; "PPU
registers", OAMADDR, Values during rendering; Brad Taylor's NTSC 2C02 technical reference,
Details of object pattern fetch & render; Visual 2C02).

```cpp
cartridge.chrRom[0x04E3] = 0x41; // row 3 of tile $4E, its low byte
cartridge.chrRom[0x04EB] = 0x16; // and its high byte

neasant::PpuState loader; // on the same `inputs`
loader.warmingUp = false;
loader.mask = 0x1E;
loader.secondaryOam.fill(0xFF); // as the clear leaves the slots
loader.secondaryOam[0] = 7; // slot 0: Y 7, so its row on scanline 10 is 3
loader.secondaryOam[1] = 0x4E; // tile $4E
loader.secondaryOam[2] = 0x03; // palette 3, in front, not flipped
loader.secondaryOam[3] = 0x42; // X $42
loader.scanline = 10;
loader.dot = 256;
for (int dot = 257; dot <= 260; ++dot) neasant::ppuTickDot(loader, false, inputs);
// loader.oamAddr == 0: held there through dot 320
// loader.spriteUnits[0].attribute == 0x03 and loader.spriteUnits[0].x == 0x42
neasant::ppuTickDot(loader, false, inputs); // dot 261
// loader.addressPins == 0x04E3: the low byte's address, on all fourteen lines
for (int dot = 262; dot <= 264; ++dot) neasant::ppuTickDot(loader, false, inputs);
// loader.spriteUnits[0].pattern[0] == 0x82 and loader.spriteUnits[0].pattern[1] == 0x68: the
//                                            two bytes reversed, the leftmost pixel at bit 0
for (int dot = 265; dot <= 272; ++dot) neasant::ppuTickDot(loader, false, inputs);
// loader.spriteUnits[1].pattern[0] == 0 and loader.spriteUnits[1].x == 0xFF: slot 1 holds no
//                                                                            sprite
```

### The sprites' pixel and the multiplexer

On each of dots 2–257 of a line of the picture, as the chip selects the background's pixel for
the position two dots behind, every unit counts: a unit whose X counter is not 0 counts it down,
and one whose counter is 0 shifts a pixel out of its registers, the pattern's two bits, eight of
them and then nothing. The lowest-numbered unit with an opaque pixel is the sprites' pixel for
the position, whatever its priority bit: a sprite behind the background hides the sprites behind
it even where the background covers it. The pixel is its palette's two bits over the pattern's
two, with bit 4 set, an address into the sprite palettes; it is transparent while `$2001` bit 4
is clear, and in the eight leftmost positions while bit 2 is.

The priority multiplexer then takes one of the two pixels for the position: both transparent
gives the backdrop, one opaque gives that one, and both opaque gives the sprite's unless its
priority bit puts it behind, when the background's shows. Where the background's pixel and the
first unit's are both opaque, with sprite 0 marked as the unit's sprite for the line, the chip
sets the sprite 0 hit flag, bit 6 of `$2002`, on that dot, two dots before the position reaches
the picture; never at position 255, and so never in the eight leftmost positions while either
layer is hidden there, nor while either layer is off, since the hidden layer's pixel is
transparent. The flag stands until dot 1 of scanline 261, so a frame reports its first hit
alone (NESdev wiki, "PPU rendering", Picture region and its priority multiplexer decision
table, and Cycles 1-256, with the frame timing diagram's notes; "PPU sprite priority"; "PPU
OAM", Sprite 0 hits; "PPU registers", PPUMASK, and PPUSTATUS, Sprite 0 hit flag; Brad Taylor's
NTSC 2C02 technical reference, Details of object pattern fetch & render and Multiplexer
operation).

```cpp
std::vector<std::uint16_t> picture(neasant::kPictureDots, 0xFFFF); // a value no dot takes
const neasant::PpuInputs held{board, cartridge, chrRam, 115200u, picture};

neasant::PpuState drawn;
drawn.warmingUp = false;
drawn.mask = 0x1E;
drawn.palette[0x00] = 0x0F; // the backdrop
drawn.palette[0x01] = 0x21; // the first background palette's first colour
drawn.palette[0x15] = 0x27; // the second sprite palette's first colour
drawn.patternShift[0] = 0x00FF; // background pixels of 1 at positions 0-7
drawn.spriteUnits[0] = neasant::SpriteUnit{.pattern = {0x0F, 0x00}, .attribute = 0x20, .x = 2};
//                     four pixels of 1 at positions 2-5, palette 0, behind the background
drawn.spriteUnits[1] = neasant::SpriteUnit{.pattern = {0xFF, 0x00}, .attribute = 0x01, .x = 4};
//                     eight pixels of 1 at positions 4-11, palette 1, in front
drawn.sprite0Current = true; // sprite 0 is in the first unit
drawn.scanline = 10;
drawn.dot = 1;
neasant::ppuTickDot(drawn, false, held); // dot 2: position 0 selected, the background's
neasant::ppuTickDot(drawn, false, held); // dot 3: position 1
// drawn.sprite0Hit == false
neasant::ppuTickDot(drawn, false, held); // dot 4: position 2, both opaque
// drawn.sprite0Hit == true: set on the selection's dot
for (int dot = 5; dot <= 16; ++dot) neasant::ppuTickDot(drawn, false, held);
// picture[10 * 256 + 0] == 0x21: the background alone
// picture[10 * 256 + 2] == 0x21: the first unit is behind the background, which shows
// picture[10 * 256 + 4] == 0x21: the first unit wins among the sprites and hides the second,
//                                and the background shows over it
// picture[10 * 256 + 6] == 0x27: the second unit, in front of the background
// picture[10 * 256 + 8] == 0x27: the second unit over the backdrop
// picture[10 * 256 + 12] == 0x0F: the backdrop
```

## The picture

The chip resolves one position of the picture a dot: position `x` of line `y` on dot `x + 4` of
scanline `y`, its output running four dots behind its counter, so a line's 256 positions take dots
4–259 and lines 0–239 are the picture. The border the chip drives around it is not kept. With
rendering enabled a position is the palette entry the multiplexer's pixel addresses, the
background's or a sprite's, selected two dots earlier, and a transparent pixel addresses the
backdrop, palette entry 0. With rendering disabled a position is the backdrop, or, while the low 14
bits of `v` are in `$3F00`–`$3FFF`, the entry they name through the palette's aliases. The entry is
taken under greyscale, and `$2001`'s emphasis bits ride above it. The chip reads `v`, the palette
and `$2001` on the dot it resolves a position, so a write that lands between two dots shows from the
next position (NESdev wiki, "PPU rendering", Drawing overview, Rendering disabled and Cycles 1-256,
and its frame timing diagram's notes; "PPU palettes", Backdrop override; "PPU registers", PPUMASK).

The picture's storage is the caller's, `PpuInputs::picture`: `kPictureDots` entries, 256 × 240,
row-major from the top line. The chip writes entry `y * 256 + x` when the span holds it and
resolves the position whether or not it does, so a chip holding no picture evolves exactly as
one that does. The dot that resolves position 0 of line 0 is answered as `PictureBegun`. On the
machine the storage is the machine's own, written while a frame observer is set, and each
picture is handed over at the vertical blank
([the machine page](nes-machine.md#the-frame-observer)).

```cpp
std::vector<std::uint8_t> image = programImage({});
neasant::Cartridge cartridge = neasant::makeCartridge(image);
neasant::Board board = neasant::boardFor(cartridge);
std::vector<std::uint8_t> chrRam(cartridge.chrRamBytes);
std::vector<std::uint16_t> picture(neasant::kPictureDots, 0xFFFF);  // a value no dot takes
const neasant::PpuInputs held{board, cartridge, chrRam, 115200u, picture};

neasant::PpuState chip;
chip.warmingUp = false;
chip.palette[0x00] = 0x21;
chip.palette[0x05] = 0x16;
for (int dot = 0; dot < 3; ++dot) neasant::ppuTickDot(chip, false, held);
// picture[0] == 0xFFFF: dots 1-3 of scanline 0 resolve no position
const neasant::PpuDotEvent begun = neasant::ppuTickDot(chip, false, held);
// begun == neasant::PpuDotEvent::PictureBegun and picture[0] == 0x21: position 0 of line 0,
//                                                                     the backdrop, on dot 4
chip.v = 0x3F05;  // the low 14 bits in the palette's range: the entry they name is drawn
neasant::ppuTickDot(chip, false, held);
// picture[1] == 0x16
chip.mask = 0x41;  // greyscale, and the green emphasis bit
neasant::ppuTickDot(chip, false, held);
// picture[2] == 0x90: the entry's grey column, $10, under bit 7
// neasant::dotEntry(0x90) == 0x10 and neasant::dotEmphasis(0x90) == 2
```

### What a dot holds

A dot's value is what the chip drives its video output with for that position: bits 5–0 the
palette entry, with greyscale applied, and bits 8–6 `$2001`'s emphasis bits, bit 6 red, bit 7
green and bit 8 blue; bits 15–9 are 0. `dotEntry` and `dotEmphasis` take the two apart, the
emphasis as bit 0 red, bit 1 green, bit 2 blue. The value is a palette entry and a tint, not a
colour: a host decodes the pair with a palette table of its own (NESdev wiki, "PPU registers",
PPUMASK, Color control; "PPU palettes").

```cpp
inline constexpr unsigned kPictureWidth = 256;
inline constexpr unsigned kPictureHeight = 240;
inline constexpr std::size_t kPictureDots = 61440;

[[nodiscard]] constexpr std::uint8_t dotEntry(std::uint16_t dot) noexcept;
[[nodiscard]] constexpr std::uint8_t dotEmphasis(std::uint16_t dot) noexcept;
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
| `maskBefore`, `maskLag` | 0, 0 | 0, 0 |
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

## Testing against the programs

The register file's self-checking test programs run on [the machine](nes-machine.md), each
booted on a machine built from a config value and run a frame at a time until it reports, as
[the machine page](nes-machine.md#testing-against-the-programs) describes: blargg's
`ppu_vbl_nmi`, `vbl_nmi_timing`, `blargg_ppu_tests_2005.09.15b`, `ppu_open_bus`, `oam_read` and
`oam_stress`, and Bisqwit's and blargg's `ppu_read_buffer`, as the
`christopherpow/nes-test-roms` repository collects them, 27 ROM files.

Of the 27, 25 pass. `ppu_vbl_nmi.nes`, the multi-test build on mapper 1, skips naming the
mapper, and `test_ppu_read_buffer.nes`, on mapper 3 with four banks of CHR, skips naming its
mapper. `ppu_vbl_nmi` and `vbl_nmi_timing` time the flag, the NMI and the odd frame's skipped
dot to one PPU clock, and their readmes say the console fails some of them after some resets;
they pass under alignment 0, the config's default. `02-vbl_set_time` is also run under
alignments 1 to 3, where it prints the row its readme gives for those resets.
`power_up_palette` compares the palette at power-on with its author's console's, so its
machine is built with that table as `NesConfig::ramSeed`. `vbl_nmi_timing` prints `PASSED` or
`FAILED` on the screen and the 2005 tests a result code, `$01` a pass; the others report at
`$6000`.

blargg's `sprite_hit_tests_2005.10.05`, eleven ROM files, and `sprite_overflow_tests`, five, measure
sprite 0 hit and the overflow flag on the console: the hit behind the background, with either layer
off, at each edge and corner of a tile, under each flip, in the hidden left column, at position 255
and at the bottom of the picture, with 8x16 sprites, and its time to the pixel; the flag's set and
clear, its indifference to X and to rendering disabled, Y = 239, 240 and 255, 8x16 sprites, the
diagonal search, its time to a CPU clock or two, and what a chip that predicts it gets wrong. All
sixteen pass, each printing a result code. Damian Yerrick's `dpcmletterbox`, whose NMI handler waits
for the hit at the top of each frame, runs with [the APU's
programs](apu.md#testing-against-the-programs).

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
- **A `$2007` access is made five dots after it is taken.** The buffer, the memory and `v` move
  on the strobe dot; a case on a bare chip ticks five dots to see them. Two reads or two writes
  on adjacent CPU cycles meet, and under alignment 2 the second is dropped.
- **The I/O bus decays.** A value left on it is gone after the configured count; a read of a
  write-only port recharges nothing.
- **`$2004` writes step the address and reads do not.**
- **A sprite's byte 2 reads back five bits.** `$2004` answers bits 4–2 clear; `oam` keeps the
  byte as written.
- **The vertical-blank flag is set at power-on by default.** `vblankAtPowerOn` clears it; a
  program that waits for the flag to clear first is waiting on this field.
- **A read of `$2002` on the dot before the set reads 0 and loses the frame's flag.** The NMI is
  lost with it.
- **`$3F10` is `$3F00`.** The four sprite palettes' entry 0 are the background palettes'.
- **Greyscale changes what a palette read answers, not what it holds.**
- **Rendering moves `v`.** While rendering is enabled the chip steps `v` through every line and
  reloads it from `t`; a `$2007` access inside the picture lands where the fetches have taken the
  address.
- **The picture's scroll is `t`'s.** `v` takes its horizontal bits on dot 257 of every line and its
  vertical bits on dots 280–304 of the pre-render line; a vertical scroll written after that copy
  shows in the next frame.
- **A pattern of 0 is transparent whatever its palette.** It shows the backdrop, entry 0, and
  entries `$3F04`, `$3F08` and `$3F0C` are never drawn by the background.
- **The background hidden still fetches.** With the sprites enabled alone the chip makes every
  access and moves `v`, and every background pixel is transparent.
- **A sprite shows a line below its Y.** Y is the line above its first row; Y = $EF–$FF keeps it off
  the picture, and no sprite shows on line 0.
- **Eight sprites a line.** The ninth and later in OAM's order are dropped from the line; the ninth
  sets the overflow flag, and the search then reads the bytes after it diagonally, so the flag is
  both missed and raised falsely.
- **A sprite behind the background hides the sprites behind it.** The lowest unit with an opaque
  pixel wins before the priority bit is consulted.
- **Sprite 0 hit is the first unit's pixel against the background's.** Both opaque by their pattern
  bits, both layers shown, not at position 255 and not in the hidden left column; set on the
  selection's dot, two before the position's; cleared with the overflow flag at dot 1 of scanline
  261 and by nothing else.
- **A `$2002` read leaves the two sprite flags.** It clears the vertical-blank flag alone.
- **`$2004` reads the OAM bus while the chip renders.** The OAM address is the evaluation's pointer
  then, and dots 257–320 hold it at 0.
- **An OAM address of 8 or more at a line's dot 65 copies a row of OAM.** The eight bytes at
  `oamAddr & $F8` replace OAM's first eight as the line's evaluation begins.
- **The picture runs four dots behind the counter.** Position `x` is resolved on dot `x + 4`; a
  write that lands between two dots shows from the next position.
- **The picture reads `$2001` live.** Greyscale, emphasis, the two bits that show the background and
  the rendering bits' choice between the background and `v`'s entry reach the picture on the first
  dot after the write; the three-dot lag is the frame counter's.
- **A `$2001` write reaches the frame counter on its third dot.** The fetches, the shift registers,
  `v`'s moves and the odd-frame skip read it from there: a write that enables the background within
  two dots of an odd frame's dot 339 leaves that frame whole.
- **`v` steps linearly through `$2007`.** By 1 or 32, wrapping inside 15 bits; its top bit
  reaches no address.
- **The reset line keeps the flag, the OAM address, `v`, the bus and the memories.**

## What remains open

What the code does at each point below is fixed and tested; what the hardware does is not settled
by the documents the chip is built from.

1. **The flag against a read under alignments 1 to 3.** The race's outcomes under alignment 0 are
   the "PPU frame timing" page's, and blargg's `ppu_vbl_nmi` and `vbl_nmi_timing`, which time the
   flag to one PPU clock, pass under it. Under the other alignments a read loses the flag on two
   dots where alignment 0 loses it on one, the outcome `02-vbl_set_time`'s readme reports on the
   console "after some resets". The other alignments' remaining columns follow from the page's
   statement that a CPU tick begins 0 to 3 master cycles before the nearest PPU tick and from the
   "CPU pinout" page's M2 duty cycle, and are derived, not measured.
2. **How wide the NMI suppression is.** The "PPU registers" page has the NMI suppressed by reads
   landing on the following dot or two; the "PPU frame timing" page has the read on the set's dot
   or one later. `06-suppression` and `5.nmi_suppression` measure the frame-timing page's two dots
   on the console, and the chip gives them under alignment 0. Under alignments 1 to 3 the chip
   suppresses on one dot, derived as item 1's columns are.
3. **The chip while rendering.** The "PPU registers" page states behaviors the chip does not carry:
   a rendering toggle reaching the picture three or four dots after the write, where the picture
   here reads `$2001` on the first dot after it, the second `$2006` write reaching `v` one to one
   and a half dots on (loaded at the write here), the OAM address's glitchy increment on a `$2004`
   write while rendering, the `$2000` bit 0 race at dot 257, the `$2006` bus conflict, and the
   palette corruption; the "PPU scrolling" page adds a `$2007` access while rendering stepping
   coarse X and Y together, where `v` steps by 1 or 32 here; the "PPU rendering" page adds the shift
   registers shifting past their loads when rendering is toggled inside a line, where they neither
   shift nor load here while rendering is disabled; and the "DMA" page adds the DPCM read conflict
   with `$2007`. Each is stated by its page and belongs to the ports while the chip renders or the
   DMC.
4. **The 2C02G's OAM corruption on `$2003` writes.** The "PPU registers" page's OAMADDR note and
   the "Errata" page's sprite entries have a write to `$2003` copy one eight-byte row of OAM over
   another on some CPU-PPU alignments, without stating which bytes on which alignment;
   `oam_stress`'s readme has the console passing "only for one of the four random PPU-CPU
   synchronizations", and `oam_read`'s shows four patterns. A `$2003` write on this chip sets the
   address and leaves OAM as it is, and both programs pass.
5. **The bytes the console answers on the second of two adjacent `$2007` reads.**
   `double_2007_read` has four outputs for its two-cycle read, one per alignment on the console:
   `22` twice, the buffer as it stood, `02`, the address's low byte on the lines, and `32`, `$33`
   with bit 0 still `$22`'s. The chip answers the buffer as it stood under every alignment, so it
   prints the first two outputs' CRCs and never the other two; which alignment prints which on a
   console is not known.
6. **The top-loader's reset.** The "PPU power up state" page says the Famicom and NES-101 reset
   the CPU alone; the machine's reset line resets both chips, the front-loader's.
7. **The warm power-on.** The page's values for a console off under twenty seconds — PPUSTATUS
   `$80`, OAMADDR `$2F` or `$01`, PPUADDR `$0001` — are not modelled; a host that wants them
   writes the state.
8. **The I/O bus's decay.** The "PPU registers" page gives 3 to 30 ms, sooner as the PPU warms,
   and the `ppudecay` program's author reports his NTSC NES from about 21 ms at power-on to 19 ms
   warm; the config holds one count for every bit, his power-on figure. No document the chip is
   built from says which value a decayed bit takes or how the time differs from bit to bit; the
   chip clears a decayed bit to 0 and ages every bit by the same count.
9. **The palette read's revision.** The "PPU registers" page's immediate palette read is the
   2C02G's and later; earlier revisions answered the read buffer. The chip is the 2C02G's and the
   earlier answer is not selectable.
10. **OAM's reads.** The `cpu_dummy_writes_oam` program needs reliable OAM reads, which its text
    says is "often the case on emulators, but NOT on the real NES"; the "PPU registers" page puts
    the console's unreliable reads down to the corruption by `$2003` writes, item 4's. The chip's
    `$2004` answers every byte it holds, a sprite's byte 2 as five bits.
11. **The dot a `$2001` write reaches the frame counter on.** `10-even_odd_timing` places it to one
    PPU clock and passes on this chip under alignment 0 with the third dot after the access; the
    "PPU registers" page gives approximately 3-4 dots after the write. The count is the same under
    the other alignments, which the program is not run under. The background reads the register as
    the skip does, the page's sentence being about rendering as a whole, and the picture reads it on
    the first dot after the write, item 3's.
12. **The dot the output appears on.** The "PPU rendering" page has the first pixel output during
    cycle 4 and its frame timing diagram a 4-dot delay from the dot the chip decides a pixel on,
    which the chip follows: position `x` on dot `x + 4`. The "NTSC video" page's scanline table
    has the active picture from position 1 of 341, one pixel clock behind the counter. A reading
    of the chip's video output against its counter on Visual 2C02 would settle the two against
    each other.
13. **`v`'s move against an access on the same dot.** The "PPU rendering" page states one case: the
    first nametable fetch after dot 256 has its low eight address bits from before dot 257's copy
    and its high six from after it, and a read is made at the current high six bits. The chip
    applies that order to every dot: the latch captures before `v` moves, and the high six lines and
    a read are taken after. Only a write that changes `t` or `v` between the two dots of an access
    reaches the difference; a reading of the address lines on Visual 2C02 would settle it.
14. **The evaluation past sprite 63.** The "PPU sprite evaluation" page has the copy after the
    wrap to sprite 0 "attempt (and fail)"; Visual 2C02 copies sprite 0 again when it is in range,
    the next slot taking the last Y looked at. The chip copies nothing after the wrap.
15. **Where the attribute byte's two bits are selected.** The "PPU rendering" page draws the
    selection at the reload, and Brad Taylor's reference has the two bits selected as the byte is
    latched, which the chip follows. The two differ only if bit 1 of `v`'s coarse X or of its coarse
    Y changes between the read and the reload other than by the chip's own step.
16. **The registers' decay.** The "PPU rendering" page has the fetch latches, the shift registers
    and the 1-bit latches decay to 0 "on a time scale of probably around 2 frames"; here they hold
    what they have for ever.
17. **The pre-render line's units.** The address lines take 261 for the line there (Visual
    2C02), so a $FF slot's addresses carry row 6 flipped to 1 and a sprite scanline 239 found row
    261 less its Y, while the units load 0 as for a line of −1, which keeps the clear's slots and
    Y = 239 off line 0 as the pages and `07.screen_bottom` have it. Whether the chip's units load
    by that comparison or by another means the pins do not show.
18. **The dot of the 2C02G's OAM copy.** The "PPU sprite evaluation" page's notes and the "PPU
    registers" page's OAMADDR precautions state the copy's bytes, the eight at `oamAddr & $F8`, its
    destination, OAM's first eight, and its condition, an address of 8 or more as evaluation starts,
    and the OAMADDR section puts the start at tick 65. The chip makes the copy on dot 65 of each
    picture line whose evaluation begins with the address at 8 or more, before that dot's read.
    Which dot of the start it falls on, and whether a line after the first copies again, are not
    stated. The corruption a `$2003` write makes is item 4's.
19. **The search after the ninth sprite.** The "PPU sprite evaluation" page's step 3a reads the
    three bytes after the Y that sets the overflow flag and says nothing of what follows. The chip
    ends the evaluation there, as it does when the pointer passes sprite 63: each remaining even dot
    moves the pointer a sprite and copies nothing. Only the bytes `$2004` answers on those dots
    differ from a search that went on.
20. **Position 255.** The "PPU OAM" page has sprite 0 hit never set at position 255 for "an obscure
    reason related to the pixel pipeline", and says no more; the chip skips the position by rule.
    `06.right_edge` and `11.edge_timing` measure the edge on the console and pass.
21. **The units on the dots no pixel is selected on.** Brad Taylor's reference has a unit's counter
    decremented every time the PPU renders a pixel, the first 256 clocks of a scanline. The chip
    steps the units on dots 2–257 of the picture's lines, as each position is selected, and not on
    dots 322–337 or on the pre-render line, where the background's registers shift and no pixel is
    selected. Dots 257–320 reload every unit before anything it held could show, so the difference
    reaches no picture.

## See also

- [The NES machine](nes-machine.md) — the cycle the dots run in, the alignment, the reset line,
  the observers.
- [The cartridge](nes-cartridge.md) — the PPU pins, CHR ROM and RAM, the CIRAM lines.
- [The 6502 core](6502-cpu.md) — when the NMI is taken.

## Where to look

- `include/neasant/ppu/ppu.h` — the state value, the inputs, the six functions.
- `src/ppu/ppu.cpp` — every port's write and read, the palette and CIRAM, the dot counter with its
  flag, warm-up and skip, the background's accesses, shift registers and scroll, the sprites'
  evaluation, output units and multiplexer, the picture's positions, and the bus's decay.
- `tests/ppu/registers_test.cpp` — the hand-derived suite.
- `tests/ppu/programs_test.cpp` — the register file's test programs, a sprite's byte 2 and the
  dot a `$2001` write reaches the skip on.
- `include/neasant/nes/video_frame.h` — the picture's size, a dot's layout, `VideoFrame` and
  `FrameObserver`.
- `tests/ppu/picture_test.cpp` — the picture on the bare chip and through the machine's frame
  observer.
- `tests/ppu/background_test.cpp` — the background dot by dot on the bare chip, and drawn in the
  machine's picture.
- `tests/ppu/sprites_test.cpp` — the sprites' evaluation, output units and multiplexer dot by dot
  on the bare chip, drawn in the machine's picture, and blargg's sprite hit and sprite overflow
  programs.
- `tests/ppu/bus_test.cpp` — the pins: the lines following `v`, a `$2007` access's dots, two
  accesses on adjacent cycles, the dot's master cycle by the alignment, dot 0 and the pre-render
  line's sprite fetch rows.

## Sources

- **NESdev wiki** — the pages "PPU registers", "PPU scrolling", "PPU memory map", "PPU palettes",
  "PPU OAM", "PPU sprite evaluation", "PPU sprite priority", "PPU rendering" with its frame timing
  diagram, "PPU nametables", "PPU attribute tables", "PPU pattern tables", "PPU pinout", "NTSC
  video", "PPU frame timing", "PPU power up state", "NMI", "Open bus behavior", "Cartridge
  connector", "CPU pinout", "CPU interrupts", "DMA" and "Errata", at `https://www.nesdev.org/wiki/`.
- **Brad Taylor's NTSC 2C02 technical reference** — the fifth release, 2004, which the "PPU
  rendering" page lists as its reference, at `http://nesdev.org/2C02%20technical%20reference.TXT`.
- **The test programs** — blargg's `ppu_vbl_nmi`, `vbl_nmi_timing`, `blargg_ppu_tests_2005.09.15b`,
  `ppu_open_bus`, `oam_read`, `oam_stress`, `sprite_hit_tests_2005.10.05` and
  `sprite_overflow_tests`, with their readmes and sources, as the `christopherpow/nes-test-roms`
  repository at `https://github.com/christopherpow/nes-test-roms` collects them. The tables
  `ppu_vbl_nmi`'s readme prints for each program are published measurements of the console.
- **Visual 2C02** — Quietust's transistor-level simulation of the 2C02, at
  `https://www.qmtpro.com/~nes/chipimages/visual2c02/`, run for the `$2007` access's dots, the
  lines while the chip makes no access, dot 0's ALE and the pre-render line's sprite fetch rows.
- **The `ppudecay` test program** — lidnariq's, public domain (2023), with its source and its
  author's description of what it measures and what it reports on his console, published at
  `https://forums.nesdev.org/viewtopic.php?t=24639`.
