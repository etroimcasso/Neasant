#pragma once

// The 2C02 as a register file over a state value: the eight ports the CPU reaches at
// $2000-$2007, with every latch a write passes through and the value and side effect
// of every read; the I/O bus behind them and its decay; the scroll file the ports fill;
// the port at $2007 over the chip's own 14-bit address space; the frame counter that
// runs 341 dots by 262 scanlines; the vertical-blank flag and the NMI output it drives;
// the warm-up window after power-on and reset; and the memories the console and the
// chip hold, nametable RAM, palette RAM and object attribute memory. The picture is
// absent: no tile is fetched and no sprite is evaluated, and a program that enables
// rendering gets the frame counter's odd-frame skip and nothing else.

#include "neasant/nes/board.h"
#include "neasant/nes/cartridge.h"

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

// What the chip reaches through and does not hold: the board with its cartridge, for
// the PPU's address space; the CHR RAM the board writes; and the dots a bit of the I/O
// bus holds its charge after an access drives it, from the machine's config, 0 holding
// it for ever.
struct PpuInputs {
  Board& board;
  const Cartridge& cartridge;
  std::span<std::uint8_t> chrRam;
  std::uint32_t busDecayDots;
};

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

}  // namespace neasant
