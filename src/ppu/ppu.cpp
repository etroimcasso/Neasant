#include "neasant/ppu/ppu.h"

namespace neasant {

void ppuWrite(PpuState& state, std::uint8_t, std::uint8_t value, const PpuInputs&) {
  // NESdev wiki, "Open bus behavior", PPU open bus: a write to any port loads the I/O bus.
  state.ioLatch = value;
}

std::uint8_t ppuRead(PpuState& state, std::uint8_t, const PpuInputs&) {
  // NESdev wiki, "Open bus behavior", PPU open bus: a read of any port answers the I/O bus.
  return state.ioLatch;
}

std::uint8_t ppuPeekRegister(const PpuState& state, std::uint8_t) noexcept {
  return state.ioLatch;
}

bool ppuTickDot(PpuState& state) noexcept {
  // NESdev wiki, "PPU frame timing", Even/Odd Frames: 341 dots a scanline, 262 scanlines
  // a frame, the parity toggling every frame.
  state.dot = static_cast<std::uint16_t>(state.dot + 1u);
  if (state.dot < kDotsPerScanline) return false;
  state.dot = 0;
  state.scanline = static_cast<std::uint16_t>(state.scanline + 1u);
  if (state.scanline < kScanlinesPerFrame) return false;
  state.scanline = 0;
  state.odd = !state.odd;
  return true;
}

}  // namespace neasant
