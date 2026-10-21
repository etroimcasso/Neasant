#include "neasant/ppu/ppu.h"

#include <cstddef>

namespace neasant {

namespace {

// The frame's events: the flag is set at dot 1 of scanline 241 and cleared at dot 1
// of scanline 261 (NESdev wiki, "NMI", Operation), and an odd frame with rendering
// enabled leaves scanline 261 from dot 339 (NESdev wiki, "PPU frame timing", Even/Odd
// Frames).
constexpr std::uint16_t kVblankScanline = 241;
constexpr std::uint16_t kPreRenderScanline = 261;
constexpr std::uint16_t kFlagDot = 1;
constexpr std::uint16_t kSkippedFromDot = 339;

// The 14 bits of the address space a $2007 access reaches, and where the palette
// begins in it (NESdev wiki, "PPU memory map"; "PPU scrolling", PPU internal
// registers).
constexpr std::uint16_t kAddressMask = 0x3FFF;
constexpr std::uint16_t kPaletteBase = 0x3F00;

// NESdev wiki, "PPU registers", PPUMASK, Rendering control: rendering is enabled while
// either of bits 4 and 3 is.
[[nodiscard]] bool renderingEnabled(const PpuState& state) noexcept {
  return (state.mask & 0x18u) != 0;
}

// The palette entry an address in $3F00-$3FFF reaches: the 32 entries repeat through
// the range, and entry 0 of each sprite palette is the storage of entry 0 of the
// background palette beside it, so $3F10, $3F14, $3F18 and $3F1C reach $3F00, $3F04,
// $3F08 and $3F0C (NESdev wiki, "PPU palettes", Palette RAM; "PPU memory map").
[[nodiscard]] std::size_t paletteIndex(std::uint16_t address) noexcept {
  const std::size_t index = address & 0x1Fu;
  return (index & 0x13u) == 0x10u ? index & 0x0Fu : index;
}

// The entry a palette read answers: its six bits, and with greyscale set only the two
// that select the grey column (NESdev wiki, "PPU registers", Reading palette RAM and
// Color control).
[[nodiscard]] std::uint8_t paletteRead(const PpuState& state, std::uint16_t address) noexcept {
  const std::uint8_t entry = state.palette[paletteIndex(address)];
  return (state.mask & 0x01u) != 0 ? static_cast<std::uint8_t>(entry & 0x30u) : entry;
}

// Drives the bits of the I/O bus `mask` names to `value`'s and recharges them: each
// holds for the dots the machine was built with, then decays to zero (NESdev wiki,
// "Open bus behavior", PPU open bus; NESdev forum, "Tests for ACE speedruns that rely on
// PPU's internal open bus").
void driveBus(PpuState& state, std::uint8_t value, std::uint8_t mask,
              const PpuInputs& inputs) noexcept {
  state.ioLatch = static_cast<std::uint8_t>((state.ioLatch & ~mask) | (value & mask));
  for (std::size_t bit = 0; bit < 8; ++bit) {
    if ((mask >> bit) & 1u) state.ioLatchDecay[bit] = inputs.busDecayDots;
  }
}

// The byte of the console's nametable RAM the board's two lines select: the kilobyte
// from CIRAM A10 and the offset from the low ten address bits (NESdev wiki, "Cartridge
// connector", Signal descriptions: CIRAM A10).
[[nodiscard]] std::size_t ciramIndex(const PpuPins& pins) noexcept {
  return (pins.ciramA10 ? 0x400u : 0u) | (pins.address & 0x3FFu);
}

// One access on the chip's address space below the palette. The board sees the
// address and drives the CIRAM lines; it answers the access itself or the console's
// nametable RAM does when the board enables it; with neither, the data lines float at
// the address's low byte (NESdev wiki, "PPU memory map", Hardware mapping; "Cartridge
// connector", Signal descriptions: CIRAM /CE; "Open bus behavior", PPU open bus).
[[nodiscard]] std::uint8_t busRead(PpuState& state, std::uint16_t address,
                                   const PpuInputs& inputs) {
  PpuPins pins{
      .address = address, .data = static_cast<std::uint8_t>(address & 0xFFu), .read = true};
  if (boardPpuAccess(inputs.board, inputs.cartridge, pins, inputs.chrRam)) return pins.data;
  if (pins.ciramEnable) return state.ciram[ciramIndex(pins)];
  return pins.data;
}

void busWrite(PpuState& state, std::uint16_t address, std::uint8_t value,
              const PpuInputs& inputs) {
  PpuPins pins{.address = address, .data = value, .write = true};
  if (boardPpuAccess(inputs.board, inputs.cartridge, pins, inputs.chrRam)) return;
  if (pins.ciramEnable) state.ciram[ciramIndex(pins)] = value;
}

// NESdev wiki, "PPU registers", PPUDATA: after an access the address steps by 1 or by
// 32 as $2000 bit 2 says; "PPU scrolling", PPU internal registers: `v` is 15 bits.
void stepAddress(PpuState& state) noexcept {
  const std::uint16_t step = (state.ctrl & 0x04u) != 0 ? 32u : 1u;
  state.v = static_cast<std::uint16_t>((state.v + step) & 0x7FFFu);
}

}  // namespace

void ppuReset(PpuState& state) noexcept {
  // NESdev wiki, "PPU power up state", Initial Register Values, the After Reset column
  // and the note on the internal reset signal: the signal clears $2000, $2001, the
  // temporary address, the fine X scroll, the toggle and the read buffer, and stands
  // until the end of the first vertical blank; the chip comes out of reset at the top
  // of the picture on an even frame.
  state.ctrl = 0;
  state.mask = 0;
  state.t = 0;
  state.x = 0;
  state.w = false;
  state.readBuffer = 0;
  state.warmingUp = true;
  state.dot = 0;
  state.scanline = 0;
  state.odd = false;
}

void ppuWrite(PpuState& state, std::uint8_t port, std::uint8_t value, const PpuInputs& inputs) {
  // NESdev wiki, "Open bus behavior", PPU open bus: a write to any port loads the I/O bus.
  driveBus(state, value, 0xFF, inputs);
  // NESdev wiki, "PPU power up state": while the internal reset signal stands, a write
  // to $2000, $2001, $2005 or $2006 has no effect and the toggle does not move.
  const bool held = state.warmingUp;
  switch (port) {
    case 0:
      if (held) return;
      // NESdev wiki, "PPU scrolling", $2000 write: the low two bits of the value become
      // bits 11-10 of the temporary address.
      state.ctrl = value;
      state.t = static_cast<std::uint16_t>((state.t & 0x73FFu) | ((value & 0x03u) << 10));
      return;
    case 1:
      if (held) return;
      state.mask = value;
      return;
    case 2:
      return;
    case 3:
      // NESdev wiki, "PPU registers", OAMADDR.
      state.oamAddr = value;
      return;
    case 4:
      // NESdev wiki, "PPU registers", OAMDATA: a write lands at the OAM address and
      // steps it.
      state.oam[state.oamAddr] = value;
      state.oamAddr = static_cast<std::uint8_t>(state.oamAddr + 1u);
      return;
    case 5:
      if (held) return;
      // NESdev wiki, "PPU scrolling", $2005 first write: the high five bits of the value
      // become the coarse X scroll, bits 4-0 of the temporary address, and the low three
      // the fine X scroll; second write: the low three bits become the fine Y scroll,
      // bits 14-12, and the high five the coarse Y scroll, bits 9-5.
      if (!state.w) {
        state.t = static_cast<std::uint16_t>((state.t & 0x7FE0u) | (value >> 3));
        state.x = static_cast<std::uint8_t>(value & 0x07u);
        state.w = true;
      } else {
        state.t = static_cast<std::uint16_t>((state.t & 0x0C1Fu) | ((value & 0x07u) << 12) |
                                             ((value & 0xF8u) << 2));
        state.w = false;
      }
      return;
    case 6:
      if (held) return;
      // NESdev wiki, "PPU scrolling", $2006 first write: the low six bits of the value
      // become bits 13-8 of the temporary address and bit 14 is cleared; second write:
      // the value becomes its low byte and the whole of it is copied into the current
      // address.
      if (!state.w) {
        state.t = static_cast<std::uint16_t>((state.t & 0x00FFu) | ((value & 0x3Fu) << 8));
        state.w = true;
      } else {
        state.t = static_cast<std::uint16_t>((state.t & 0x7F00u) | value);
        state.v = state.t;
        state.w = false;
      }
      return;
    default: {
      // NESdev wiki, "PPU registers", PPUDATA: the write lands at the current address
      // and the address steps. A palette entry keeps six bits (Reading palette RAM).
      const auto address = static_cast<std::uint16_t>(state.v & kAddressMask);
      if (address >= kPaletteBase) {
        state.palette[paletteIndex(address)] = static_cast<std::uint8_t>(value & 0x3Fu);
      } else {
        busWrite(state, address, value, inputs);
      }
      stepAddress(state);
      return;
    }
  }
}

std::uint8_t ppuRead(PpuState& state, std::uint8_t port, const PpuInputs& inputs) {
  switch (port) {
    case 2: {
      // NESdev wiki, "NMI", Operation: the flag is answered and then cleared; "PPU
      // scrolling", $2002 read: the toggle is cleared; "Open bus behavior", PPU open
      // bus: bits 7-5 of the I/O bus are driven and its low five bits are answered as
      // they stand.
      const auto value =
          static_cast<std::uint8_t>((state.vblank ? 0x80u : 0u) | (state.ioLatch & 0x1Fu));
      state.vblank = false;
      state.w = false;
      driveBus(state, value, 0xE0, inputs);
      return value;
    }
    case 4:
      // NESdev wiki, "PPU registers", OAMDATA: a read answers the byte at the OAM
      // address and does not step it; "Open bus behavior", PPU open bus: the byte is
      // driven onto the I/O bus.
      driveBus(state, state.oam[state.oamAddr], 0xFF, inputs);
      return state.ioLatch;
    case 7: {
      const auto address = static_cast<std::uint16_t>(state.v & kAddressMask);
      std::uint8_t value;
      if (address >= kPaletteBase) {
        // NESdev wiki, "PPU registers", Reading palette RAM: the entry is answered at
        // once under the I/O bus's top two bits, which stay as they are, and the read
        // the chip makes underneath at the same address fills the read buffer.
        value = static_cast<std::uint8_t>((state.ioLatch & 0xC0u) | paletteRead(state, address));
        driveBus(state, value, 0x3F, inputs);
      } else {
        // NESdev wiki, "PPU registers", The PPUDATA read buffer: the buffer is answered
        // and refilled from the current address.
        value = state.readBuffer;
        driveBus(state, value, 0xFF, inputs);
      }
      state.readBuffer = busRead(state, address, inputs);
      // NESdev wiki, "PPU power up state": the internal reset signal holds the read
      // buffer cleared while it stands.
      if (state.warmingUp) state.readBuffer = 0;
      stepAddress(state);
      return value;
    }
    default:
      // NESdev wiki, "Open bus behavior", PPU open bus: a write-only port answers the
      // I/O bus, driving nothing.
      return state.ioLatch;
  }
}

std::uint8_t ppuPeekRegister(const PpuState& state, std::uint8_t port) noexcept {
  switch (port) {
    case 2:
      return static_cast<std::uint8_t>((state.vblank ? 0x80u : 0u) | (state.ioLatch & 0x1Fu));
    case 4:
      return state.oam[state.oamAddr];
    case 7: {
      const auto address = static_cast<std::uint16_t>(state.v & kAddressMask);
      if (address >= kPaletteBase) {
        return static_cast<std::uint8_t>((state.ioLatch & 0xC0u) | paletteRead(state, address));
      }
      return state.readBuffer;
    }
    default:
      return state.ioLatch;
  }
}

bool ppuTickDot(PpuState& state, bool statusRead) noexcept {
  bool ended = false;
  if (state.scanline == kPreRenderScanline && state.dot == kSkippedFromDot && state.odd &&
      renderingEnabled(state)) {
    // NESdev wiki, "PPU frame timing", Even/Odd Frames: an odd frame with rendering
    // enabled goes from dot 339 of scanline 261 straight to dot 0 of scanline 0.
    state.dot = 0;
    state.scanline = 0;
    state.odd = !state.odd;
    ended = true;
  } else {
    // NESdev wiki, "PPU frame timing", Even/Odd Frames: 341 dots a scanline, 262
    // scanlines a frame, the parity toggling every frame.
    state.dot = static_cast<std::uint16_t>(state.dot + 1u);
    if (state.dot == kDotsPerScanline) {
      state.dot = 0;
      state.scanline = static_cast<std::uint16_t>(state.scanline + 1u);
      if (state.scanline == kScanlinesPerFrame) {
        state.scanline = 0;
        state.odd = !state.odd;
        ended = true;
      }
    }
  }
  if (state.dot == kFlagDot) {
    // NESdev wiki, "NMI", Operation: the flag is set at dot 1 of scanline 241 and
    // cleared at dot 1 of scanline 261. "PPU frame timing", VBL Flag Timing: a read of
    // $2002 that begins on the dot before the set reads the flag clear, and the flag is
    // never set for that frame. "PPU power up state": the signal that clears the flag
    // ends the internal reset.
    if (state.scanline == kVblankScanline && !statusRead) state.vblank = true;
    if (state.scanline == kPreRenderScanline) {
      state.vblank = false;
      state.warmingUp = false;
    }
  }
  // NESdev forum, "Tests for ACE speedruns that rely on PPU's internal open bus": a bit
  // of the I/O bus keeps its charge for a time after it is driven, then decays to zero.
  for (std::size_t bit = 0; bit < 8; ++bit) {
    std::uint32_t& left = state.ioLatchDecay[bit];
    if (left != 0 && --left == 0) {
      state.ioLatch = static_cast<std::uint8_t>(state.ioLatch & ~(1u << bit));
    }
  }
  return ended;
}

}  // namespace neasant
