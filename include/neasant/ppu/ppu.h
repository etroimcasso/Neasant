#pragma once

// The 2C02 as a register file over a state value: the eight ports the CPU reaches at
// $2000-$2007, the one I/O latch behind them, the frame counter that runs the
// picture's 341 dots by 262 scanlines, and the console's nametable RAM. Every port
// write loads the latch and every port read answers it; the counter counts dots and
// marks the frame's end. The picture, the memories behind $2007, the vertical-blank
// flag and the NMI output are absent.

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
  // The PPU's I/O bus: the byte every port write loads and every port read answers
  // (NESdev wiki, "Open bus behavior", PPU open bus).
  std::uint8_t ioLatch = 0;
  // Where the frame counter is: the dot within the scanline, 0-340, the scanline within
  // the frame, 0-261, and the parity, which toggles as each frame ends (NESdev wiki,
  // "PPU frame timing", Even/Odd Frames). At power-on the counter is at dot 0 of
  // scanline 0 of an even frame.
  std::uint16_t dot = 0;
  std::uint16_t scanline = 0;
  bool odd = false;
  // The console's 2 KB of nametable RAM, reached through the board's CIRAM lines (NESdev
  // wiki, "Cartridge connector", Signal descriptions: CIRAM /CE and CIRAM A10).
  std::array<std::uint8_t, 2048> ciram{};
  [[nodiscard]] bool operator==(const PpuState&) const noexcept = default;
};

// What the chip reaches through and does not hold: the board with its cartridge, for
// the PPU's address space, and the CHR RAM the board writes.
struct PpuInputs {
  Board& board;
  const Cartridge& cartridge;
  std::span<std::uint8_t> chrRam;
};

// A CPU write to port `port`, 0-7, the address's low three bits: loads the I/O latch.
void ppuWrite(PpuState& state, std::uint8_t port, std::uint8_t value, const PpuInputs& inputs);

// A CPU read of port `port`: answers the I/O latch.
[[nodiscard]] std::uint8_t ppuRead(PpuState& state, std::uint8_t port, const PpuInputs& inputs);

// What a read of port `port` would answer, with nothing moved.
[[nodiscard]] std::uint8_t ppuPeekRegister(const PpuState& state, std::uint8_t port) noexcept;

// One dot: steps the frame counter and answers whether this dot ended a frame, the
// counter wrapping from dot 340 of scanline 261 to dot 0 of scanline 0 and the parity
// toggling (NESdev wiki, "PPU frame timing", Even/Odd Frames).
bool ppuTickDot(PpuState& state) noexcept;

}  // namespace neasant
