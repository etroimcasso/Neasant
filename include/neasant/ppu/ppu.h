#pragma once

// The 2C02 as a register file over a state value: the eight ports the CPU reaches at
// $2000-$2007, with every latch a write passes through and the value and side effect
// of every read; the I/O bus behind them and its decay; the scroll file the ports fill;
// the port at $2007 over the chip's own 14-bit address space; the frame counter that
// runs 341 dots by 262 scanlines; the vertical-blank flag and the NMI output it drives;
// the warm-up window after power-on and reset; the memories the console and the chip
// hold, nametable RAM, palette RAM and object attribute memory; the background, each
// tile fetched through the board in four accesses of two dots, shifted out a pixel a
// dot and scrolled by `v`; and the picture, 256 dots by 240 lines, each resolved on its
// dot: with rendering enabled the background's pixel, or the backdrop where that is
// transparent, and with rendering disabled the backdrop or the palette entry `v` points
// at, under greyscale and emphasis. No sprite is evaluated or drawn.

#include "neasant/nes/board.h"
#include "neasant/nes/cartridge.h"
#include "neasant/nes/video_frame.h"

#include <array>
#include <cstdint>
#include <span>

namespace neasant {

// A frame is 341 dots by 262 scanlines with rendering disabled (NESdev wiki, "PPU frame
// timing", Even/Odd Frames).
inline constexpr std::uint16_t kDotsPerScanline = 341;
inline constexpr std::uint16_t kScanlinesPerFrame = 262;

// The chip's whole mutable state as a value: snapshot by copy, restore by assignment.
struct PpuState {
  // $2000 as last written: bit 7 enables the NMI output, bit 4 selects the half of the
  // pattern table the background's tiles are read from, $0000 or $1000, bit 2 selects
  // the step the address takes after a $2007 access, 1 or 32, and bits 1-0 are copied
  // into the temporary address as the nametable select (NESdev wiki, "PPU registers",
  // PPUCTRL; "PPU pattern tables", Addressing; "PPU scrolling", $2000 write). Bits 6, 5
  // and 3 are held for the sprites and the EXT pins.
  std::uint8_t ctrl = 0;
  // $2001 as last written: bits 4 and 3 enable rendering, which runs the fetches, the
  // shift registers, `v`'s moves and the odd-frame skip, and decides whether a position
  // of the picture is the background's or `v` names its entry; bit 3 shows the
  // background, and bit 1 shows it in the picture's eight leftmost positions; bit 0 is
  // greyscale, which a palette read and the picture apply; and bits 7-5 are the emphasis
  // bits every dot of the picture carries (NESdev wiki, "PPU registers", PPUMASK). Bit 2
  // is held for the sprites.
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
  // The background's pixels on their way to the picture: [0] the one selected on the
  // last dot and [1] the one selected on the dot before it. A pixel is a palette
  // address: 0 when it is transparent, and otherwise the tile's palette in bits 3-2 over
  // the pattern's two bits. A position's pixel is selected two dots behind the counter
  // and reaches the picture four behind it (NESdev wiki, "PPU rendering", Cycles 1-256:
  // the shifters first shift on cycle 2, where sprite 0 hit takes the image to start,
  // and the first pixel is output during cycle 4).
  std::array<std::uint8_t, 2> pixelPipeline{};
  // The address lines as the fetch circuit last drove them, and the console's address
  // latch. On the first dot of an access the chip drives all fourteen lines and the
  // latch captures the low eight; on the second the low eight lines carry the data, the
  // chip drives the high six, and the cartridge and the nametable RAM are addressed by
  // those six over the latch. `addressPins` keeps its low eight bits as an address last
  // put them (NESdev wiki, "PPU rendering", PPU address bus contents; "PPU pinout",
  // Signal descriptions).
  std::uint16_t addressPins = 0;
  std::uint8_t addressLatch = 0;
  [[nodiscard]] bool operator==(const PpuState&) const noexcept = default;
};

// What the chip reaches through and does not hold: the board with its cartridge, for
// the PPU's address space; the CHR RAM the board writes; the dots a bit of the I/O bus
// holds its charge after an access drives it, from the machine's config, 0 holding it
// for ever; and the picture's storage, kPictureDots entries the chip writes each dot's
// value into as it resolves the dot, or empty, with which the chip resolves every dot,
// stores none, and runs exactly as it does with the storage.
struct PpuInputs {
  Board& board;
  const Cartridge& cartridge;
  std::span<std::uint8_t> chrRam;
  std::uint32_t busDecayDots;
  std::span<std::uint16_t> picture{};
};

// What a dot was, beyond one more of the frame: the dot that resolved the picture's
// first position, dot 4 of scanline 0; the dot the vertical blank begins on, dot 1 of
// scanline 241, where the flag is set; the dot that ended the frame, taking the counter
// to dot 0 of scanline 0; or none of these, as most dots are. No dot is two of them.
enum class PpuDotEvent : std::uint8_t { None, PictureBegun, VerticalBlank, FrameEnded };

// The chip after the reset line: $2000, $2001 with any write of it still short of the
// frame counter, the temporary address, the fine X scroll, the write toggle and the read
// buffer cleared, the counter at dot 0 of scanline 0 of an even frame, and the warm-up
// window open; the vertical-blank flag, the OAM address, the current address, the I/O bus
// with its decay, the fetch latches, the shift registers, the pixels on their way to the
// picture, the address lines and every memory keep what they hold (NESdev wiki, "PPU
// power up state", Initial Register Values, the After Reset column and the note on the
// internal reset signal).
void ppuReset(PpuState& state) noexcept;

// A CPU write to port `port`, 0-7, the address's low three bits. Every write loads the
// I/O bus whole. $2000, $2001, $2005 and $2006 then take the value as PpuState says,
// unless the warm-up window stands; $2003 sets the OAM address; $2004 stores at the OAM
// address and steps it; $2007 writes through the chip's address space at `v` and
// steps `v`; a write to $2002 loads the bus and nothing else.
void ppuWrite(PpuState& state, std::uint8_t port, std::uint8_t value, const PpuInputs& inputs);

// A CPU read of port `port`. $2002 answers the vertical-blank flag in bit 7 over the
// I/O bus's low five bits, then clears the flag and the write toggle; $2004 answers
// the OAM byte at the OAM address, a sprite's byte 2 with bits 4-2 clear; $2007 answers
// the read buffer and refills it from the chip's address space at `v`, or answers a
// palette entry at once under the bus's top two bits, and steps `v`; every other port
// answers the I/O bus as it stands.
[[nodiscard]] std::uint8_t ppuRead(PpuState& state, std::uint8_t port, const PpuInputs& inputs);

// What a read of port `port` would answer, with nothing moved: the flag, the toggle,
// the read buffer, the address and the I/O bus all stay as they are.
[[nodiscard]] std::uint8_t ppuPeekRegister(const PpuState& state, std::uint8_t port) noexcept;

// One dot. Steps the frame counter, going from dot 339 of scanline 261 straight to dot
// 0 of scanline 0 on an odd frame with rendering enabled; sets the vertical-blank flag
// at dot 1 of scanline 241 unless a read of $2002 is under way on the CPU's bus, and
// clears it and closes the warm-up window at dot 1 of scanline 261.
//
// With rendering enabled, on scanlines 0-239 and 261, runs the background's dot. An
// access through the board is two dots, the address and then the read. Dots 1-256 and
// 321-336 fetch a tile every eight: its nametable byte, its attribute byte and its two
// pattern bytes. Each eight dots from 257 to 320 open with two nametable fetches, the
// first kept and the second dropped, as dots 337-340 are; the four dots that follow
// them in each eight make no access. Dot 0 of scanlines 0-240 drives the address of the
// next pattern fetch and reads nothing, and the dot an odd frame reaches by its skip
// makes the read dot 340 would have. The shift registers shift on dots 2-257 and
// 322-337 and reload on every eighth dot from 9 to 257 and on dots 329 and 337. `v`
// steps its coarse X on every eighth dot from 8 to 256 and on dots 328 and 336, steps
// its Y on dot 256, takes `t`'s horizontal bits on dot 257 and, on scanline 261, `t`'s
// vertical bits on each of dots 280-304. A write to $2001 counts for the skip and for
// all of this from the third dot after its access.
//
// Resolves the picture's position for the dot on dots 4-259 of scanlines 0-239, and
// stores it when `inputs` carries the picture: position x of line y on dot x + 4 of
// scanline y. With rendering enabled its value is the background's pixel, selected two
// dots earlier, or the backdrop, palette entry 0, where that pixel is transparent; with
// rendering disabled the backdrop, or the entry the low 14 bits of `v` name while they
// are in $3F00-$3FFF; each under greyscale and the emphasis bits as $2001 stands on that
// dot. Counts each bit of the I/O bus a dot closer to its decay, and answers what the
// dot was (PpuDotEvent).
//
// NESdev wiki, "NMI", Operation; "PPU frame timing", Even/Odd Frames and VBL Flag
// Timing; "PPU power up state"; "PPU registers", PPUMASK; "PPU rendering", Drawing
// overview, Rendering disabled, Line-by-line timing and PPU address bus contents, and its
// frame timing diagram with its notes; "PPU scrolling", Register controls and Wrapping
// around; "PPU palettes", Backdrop override.
PpuDotEvent ppuTickDot(PpuState& state, bool statusRead, const PpuInputs& inputs);

// The chip's NMI output, true while it is pulled low: the vertical-blank flag set and
// $2000 bit 7 set (NESdev wiki, "NMI", Operation).
[[nodiscard]] constexpr bool ppuNmiAsserted(const PpuState& state) noexcept {
  return state.vblank && (state.ctrl & 0x80u) != 0;
}

}  // namespace neasant
