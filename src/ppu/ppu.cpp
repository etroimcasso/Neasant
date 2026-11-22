#include "neasant/ppu/ppu.h"

#include <array>
#include <cstddef>

namespace neasant {

namespace {

// The frame's events: the flag is set at dot 1 of scanline 241 and cleared at dot 1
// of scanline 261 (NESdev wiki, "NMI", Operation), and an odd frame with rendering
// enabled leaves scanline 261 from dot 339 (NESdev wiki, "PPU frame timing", Even/Odd
// Frames).
constexpr std::uint16_t kPostRenderScanline = 240;
constexpr std::uint16_t kVblankScanline = 241;
constexpr std::uint16_t kPreRenderScanline = 261;
constexpr std::uint16_t kFlagDot = 1;
constexpr std::uint16_t kSkippedFromDot = 339;

// The dots a write to $2001 takes to reach the frame counter, whose skip, fetches,
// shifts and moves of `v` read it: the third dot after the write's access is the first
// that does. blargg's ppu_vbl_nmi, 10-even_odd_timing, enables and disables the
// background one PPU clock apart around the odd frame's last dots and reports which
// write the skip followed; NESdev wiki, "PPU registers", PPUMASK, Rendering control,
// gives approximately 3-4 dots after the write for rendering as a whole.
constexpr std::uint8_t kMaskLagDots = 3;

// The 14 bits of the address space a $2007 access reaches, and where the palette
// begins in it (NESdev wiki, "PPU memory map"; "PPU scrolling", PPU internal
// registers).
constexpr std::uint16_t kAddressMask = 0x3FFF;
constexpr std::uint16_t kPaletteBase = 0x3F00;

// The picture's dots: position x of line y is resolved on dot x + 4 of scanline y, the
// chip's output running four dots behind its counter, so a line's 256 positions take
// dots 4-259 and lines 0-239 are the picture (NESdev wiki, "PPU rendering", Drawing
// overview, and Cycles 1-256: the first pixel is output during cycle 4; its frame timing
// diagram's notes: actual pixel output has a 4-dot delay).
constexpr std::uint16_t kFirstPictureDot = 4;
constexpr std::uint16_t kLastPictureLine = 239;

// NESdev wiki, "PPU registers", PPUMASK, Rendering control: rendering is enabled while
// either of bits 4 and 3 of $2001 is.
[[nodiscard]] bool renderingEnabled(std::uint8_t mask) noexcept {
  return (mask & 0x18u) != 0;
}

// Rendering as the frame counter reads it: by `maskBefore` while a write to $2001 is
// short of its third dot, by $2001 otherwise.
[[nodiscard]] bool renderingAtTheCounter(const PpuState& state) noexcept {
  return renderingEnabled(state.maskLag != 0 ? state.maskBefore : state.mask);
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

// The byte OAM answers at `address`. A sprite's byte 2 is five bits in the chip, and its
// bits 4-2 read back as 0; every other byte is answered whole (NESdev wiki, "PPU OAM",
// Byte 2 - Attributes).
[[nodiscard]] std::uint8_t oamByte(const PpuState& state, std::uint8_t address) noexcept {
  const std::uint8_t byte = state.oam[address];
  return (address & 0x03u) == 0x02u ? static_cast<std::uint8_t>(byte & 0xE3u) : byte;
}

// The value the chip drives for the picture position the dot resolves. With rendering
// enabled it is the palette entry `pixel` addresses, the background's pixel for the
// position, and a transparent pixel, 0, addresses the backdrop, entry 0; with rendering
// disabled it is the backdrop, or the entry the low 14 bits of `v` name while they are
// in the palette's range; under greyscale, with $2001's emphasis bits above it (NESdev
// wiki, "PPU rendering", Drawing overview and Rendering disabled; "PPU palettes",
// Backdrop override; "PPU registers", PPUMASK, Color control).
[[nodiscard]] std::uint16_t pictureDot(const PpuState& state, std::uint8_t pixel) noexcept {
  std::uint16_t address = static_cast<std::uint16_t>(kPaletteBase | pixel);
  if (!renderingEnabled(state.mask)) {
    const auto pointed = static_cast<std::uint16_t>(state.v & kAddressMask);
    address = pointed >= kPaletteBase ? pointed : kPaletteBase;
  }
  const std::uint8_t entry = paletteRead(state, address);
  return static_cast<std::uint16_t>(((state.mask & 0xE0u) << 1) | entry);
}

// Drives the bits of the I/O bus `mask` names to `value`'s and recharges them: each
// holds for the dots the machine was built with, then decays to zero (NESdev wiki,
// "Open bus behavior", PPU open bus; "PPU registers", MMIO registers).
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

// The accesses the background makes: the four of a tile, and the nametable fetch whose
// byte the chip does not keep.
enum class Fetch : std::uint8_t {
  None,
  Nametable,
  NametableDropped,
  Attribute,
  PatternLow,
  PatternHigh,
};

// The access dot `dot` of a rendered scanline belongs to, for dots 1-340. An access is
// two dots and a line's accesses run in eights from dot 1. Dots 1-256 and 321-336 fetch
// a tile every eight: its nametable byte, its attribute byte, its low pattern byte and
// its high one. Each eight of dots 257-320 opens with two nametable fetches, the first
// kept and the second dropped, and its last four dots make no access; dots 337-340 are
// two nametable fetches, kept and dropped (NESdev wiki, "PPU rendering", Line-by-line
// timing, and its frame timing diagram with its legend: an unused nametable fetch
// updates the nametable byte register and an ignored one does not).
[[nodiscard]] Fetch fetchOn(std::uint16_t dot) noexcept {
  constexpr std::array<Fetch, 4> kTile{Fetch::Nametable, Fetch::Attribute, Fetch::PatternLow,
                                       Fetch::PatternHigh};
  constexpr std::array<Fetch, 4> kPair{Fetch::Nametable, Fetch::NametableDropped, Fetch::None,
                                       Fetch::None};
  if (dot == 0) return Fetch::None;
  const std::size_t slot = static_cast<std::size_t>(((dot - 1u) & 7u) >> 1);
  if (dot <= 256 || (dot >= 321 && dot <= 336)) return kTile[slot];
  return kPair[slot];
}

// The address an access drives. A nametable fetch reads the tile `v` points at and an
// attribute fetch the byte that covers that tile (NESdev wiki, "PPU scrolling", Tile and
// attribute fetching). A pattern fetch reads the row `v`'s fine Y names of the tile the
// nametable latch names, in the half of the pattern table $2000 bit 4 selects, the high
// byte eight above the low (NESdev wiki, "PPU pattern tables", Addressing).
[[nodiscard]] std::uint16_t fetchAddress(const PpuState& state, Fetch fetch) noexcept {
  switch (fetch) {
    case Fetch::Attribute:
      return static_cast<std::uint16_t>(0x23C0u | (state.v & 0x0C00u) | ((state.v >> 4) & 0x38u) |
                                        ((state.v >> 2) & 0x07u));
    case Fetch::PatternLow:
    case Fetch::PatternHigh:
      return static_cast<std::uint16_t>(((state.ctrl & 0x10u) << 8) | (static_cast<unsigned>(state.nametableLatch) << 4) |
                                        (fetch == Fetch::PatternHigh ? 0x08u : 0u) |
                                        ((state.v >> 12) & 0x07u));
    default:
      return static_cast<std::uint16_t>(0x2000u | (state.v & 0x0FFFu));
  }
}

// NESdev wiki, "PPU scrolling", Coarse X increment: coarse X counts 0 to 31 and its
// overflow switches the horizontal nametable.
void stepCoarseX(PpuState& state) noexcept {
  if ((state.v & 0x001Fu) == 31u) {
    state.v = static_cast<std::uint16_t>((state.v & ~0x001Fu) ^ 0x0400u);
  } else {
    state.v = static_cast<std::uint16_t>(state.v + 1u);
  }
}

// NESdev wiki, "PPU scrolling", Y increment: fine Y counts 0 to 7 and overflows into
// coarse Y, which goes from 29 to 0 switching the vertical nametable, and from 31 to 0
// without switching it.
void stepY(PpuState& state) noexcept {
  if ((state.v & 0x7000u) != 0x7000u) {
    state.v = static_cast<std::uint16_t>(state.v + 0x1000u);
    return;
  }
  state.v = static_cast<std::uint16_t>(state.v & ~0x7000u);
  std::uint16_t coarseY = static_cast<std::uint16_t>((state.v & 0x03E0u) >> 5);
  if (coarseY == 29u) {
    coarseY = 0;
    state.v = static_cast<std::uint16_t>(state.v ^ 0x0800u);
  } else if (coarseY == 31u) {
    coarseY = 0;
  } else {
    ++coarseY;
  }
  state.v = static_cast<std::uint16_t>((state.v & ~0x03E0u) | (static_cast<unsigned>(coarseY) << 5));
}

// `v`'s move on dot `dot` of a rendered scanline: coarse X steps on every eighth dot
// from 8 to 256 and on dots 328 and 336; Y steps on dot 256; the horizontal bits, coarse
// X and bit 10, are copied from `t` on dot 257; and on the pre-render line the vertical
// bits, fine Y, coarse Y and bit 11, are copied from `t` on each of dots 280-304 (NESdev
// wiki, "PPU scrolling", At dot 256 of each scanline, At dot 257 of each scanline, During
// dots 280 to 304 of the pre-render scanline, and Between dot 328 of a scanline, and 256
// of the next scanline).
void moveV(PpuState& state, std::uint16_t dot, bool preRender) noexcept {
  if ((dot & 7u) == 0 && dot != 0 && (dot <= 256 || dot == 328 || dot == 336)) {
    stepCoarseX(state);
  }
  if (dot == 256) stepY(state);
  if (dot == 257) {
    state.v = static_cast<std::uint16_t>((state.v & ~0x041Fu) | (state.t & 0x041Fu));
  }
  if (preRender && dot >= 280 && dot <= 304) {
    state.v = static_cast<std::uint16_t>((state.v & ~0x7BE0u) | (state.t & 0x7BE0u));
  }
}

// The first dot of an access: the chip drives all fourteen address lines, the console's
// latch captures the low eight, and the board is presented the address with neither
// strobe low. `low` is the address's low byte as `v` stood when the dot began, and the
// high six lines are the address's as `v` stands after the dot's own move: the first
// nametable fetch after dot 256 carries the low byte of `v` as dot 256 left it under the
// high bits dot 257 reloads (NESdev wiki, "PPU rendering", PPU address bus contents, and
// Cycles 257-320: the first garbage nametable fetch; "PPU pinout", Signal descriptions).
void addressDot(PpuState& state, const PpuInputs& inputs, Fetch fetch, std::uint8_t low) {
  state.addressPins = static_cast<std::uint16_t>((fetchAddress(state, fetch) & 0x3F00u) | low);
  state.addressLatch = low;
  PpuPins pins{.address = state.addressPins, .data = low};
  static_cast<void>(boardPpuAccess(inputs.board, inputs.cartridge, pins, inputs.chrRam));
}

// The second dot of an access: the read, at the high six lines as the chip drives them
// now, after the dot's own move of `v`, over the low eight the latch holds, and the byte
// into its latch. An attribute byte leaves its two bits for the tile, the pair bit 1 of
// `v`'s coarse X and bit 1 of its coarse Y select (NESdev wiki, "PPU rendering", its
// notes: reads come from an address composed of the latched low 8 bits and the current
// high 6 bits; "PPU attribute tables").
void readDot(PpuState& state, const PpuInputs& inputs, Fetch fetch) {
  state.addressPins = static_cast<std::uint16_t>((fetchAddress(state, fetch) & 0x3F00u) |
                                                 (state.addressPins & 0x00FFu));
  const auto address =
      static_cast<std::uint16_t>((state.addressPins & 0x3F00u) | state.addressLatch);
  const std::uint8_t byte = busRead(state, address, inputs);
  switch (fetch) {
    case Fetch::Nametable:
      state.nametableLatch = byte;
      return;
    case Fetch::Attribute: {
      const unsigned shift = ((state.v >> 4) & 4u) | (state.v & 2u);
      state.attributeLatch = static_cast<std::uint8_t>((byte >> shift) & 0x03u);
      return;
    }
    case Fetch::PatternLow:
      state.patternLatch[0] = byte;
      return;
    case Fetch::PatternHigh:
      state.patternLatch[1] = byte;
      return;
    default:
      return;
  }
}

// A pattern byte as a shift register's high eight bits take it: its bit 7, the tile's
// leftmost pixel, at bit 8, the first to reach the low eight (NESdev wiki, "PPU pattern
// tables"; "PPU rendering", Picture region).
[[nodiscard]] std::uint16_t loaded(std::uint8_t byte) noexcept {
  std::uint16_t reversed = 0;
  for (unsigned bit = 0; bit < 8; ++bit) {
    if ((byte >> bit) & 1u) reversed = static_cast<std::uint16_t>(reversed | (0x80u >> bit));
  }
  return static_cast<std::uint16_t>(reversed << 8);
}

// The background's pixel for picture position `position`: the bit the fine X scroll
// picks of each shift register, the pattern's two bits under the palette's two. A
// pattern of 0 is transparent whatever the palette, and so is every pixel while $2001
// bit 3 is clear, and the eight leftmost while bit 1 is (NESdev wiki, "PPU rendering",
// Picture region; "PPU registers", PPUMASK, Rendering control).
[[nodiscard]] std::uint8_t backgroundPixel(const PpuState& state, unsigned position) noexcept {
  if ((state.mask & 0x08u) == 0) return 0;
  if (position < 8 && (state.mask & 0x02u) == 0) return 0;
  const unsigned x = state.x & 7u;
  const unsigned pattern =
      ((state.patternShift[0] >> x) & 1u) | (((state.patternShift[1] >> x) & 1u) << 1);
  if (pattern == 0) return 0;
  const unsigned palette =
      ((state.attributeShift[0] >> x) & 1u) | (((state.attributeShift[1] >> x) & 1u) << 1);
  return static_cast<std::uint8_t>((palette << 2) | pattern);
}

// One shift of the four registers: every bit a place toward bit 0, the low pattern
// register taking a 0 at its top, the high one a 1, and each attribute register its
// 1-bit latch (NESdev wiki, "PPU rendering", Picture region and its notes).
void shiftBackground(PpuState& state) noexcept {
  state.patternShift[0] = static_cast<std::uint16_t>(state.patternShift[0] >> 1);
  state.patternShift[1] = static_cast<std::uint16_t>((state.patternShift[1] >> 1) | 0x8000u);
  for (std::size_t bit = 0; bit < 2; ++bit) {
    state.attributeShift[bit] = static_cast<std::uint8_t>(
        (state.attributeShift[bit] >> 1) | (((state.attributeFeed >> bit) & 1u) << 7));
  }
}

// The reload: the two pattern latches into the high eight bits of their registers and
// the attribute latch into the two 1-bit latches (NESdev wiki, "PPU rendering", Picture
// region).
void reloadBackground(PpuState& state) noexcept {
  for (std::size_t half = 0; half < 2; ++half) {
    state.patternShift[half] = static_cast<std::uint16_t>((state.patternShift[half] & 0x00FFu) |
                                                          loaded(state.patternLatch[half]));
  }
  state.attributeFeed = state.attributeLatch;
}

// The background's work on a dot with rendering enabled, answering the pixel selected
// on it, 0 when none is. On scanlines 0-239 and 261: dots 2-257 and 322-337 shift, the
// picture's scanlines first selecting the pixel of the position two dots behind on dots
// 2-257, and the registers reload after the shift on every eighth dot from 9 to 257 and
// on dots 329 and 337; then `v`'s move for the dot and the dot's half of an access, the
// move falling after the latch has captured an address's low eight and before the high
// six lines and a read are taken. Dot 0 of scanlines 0-240 drives the low pattern byte's
// address and reads nothing. `skipped` is the dot an odd frame reaches from dot 339 of
// scanline 261, which makes the read dot 340 would have made in place of dot 0's address
// (NESdev wiki, "PPU rendering", Pre-render scanline, Cycle 0, Cycles 1-256 and Cycles
// 257-320, and its frame timing diagram).
[[nodiscard]] std::uint8_t backgroundDot(PpuState& state, const PpuInputs& inputs,
                                         bool skipped) {
  const std::uint16_t dot = state.dot;
  const std::uint16_t scanline = state.scanline;
  if (skipped) {
    readDot(state, inputs, Fetch::NametableDropped);
    return 0;
  }
  if (dot == 0) {
    if (scanline <= kPostRenderScanline) {
      addressDot(state, inputs, Fetch::PatternLow,
                 static_cast<std::uint8_t>(fetchAddress(state, Fetch::PatternLow) & 0xFFu));
    }
    return 0;
  }
  const bool preRender = scanline == kPreRenderScanline;
  if (scanline > kLastPictureLine && !preRender) return 0;
  std::uint8_t selected = 0;
  if ((dot >= 2 && dot <= 257) || (dot >= 322 && dot <= 337)) {
    if (!preRender && dot <= 257) selected = backgroundPixel(state, dot - 2u);
    shiftBackground(state);
    if ((dot & 7u) == 1) reloadBackground(state);
  }
  const Fetch fetch = fetchOn(dot);
  const auto low = static_cast<std::uint8_t>(fetchAddress(state, fetch) & 0xFFu);
  moveV(state, dot, preRender);
  if (fetch == Fetch::None) return selected;
  if ((dot & 1u) != 0) {
    addressDot(state, inputs, fetch, low);
  } else {
    readDot(state, inputs, fetch);
  }
  return selected;
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
  state.maskBefore = 0;
  state.maskLag = 0;
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
      // NESdev wiki, "PPU registers", PPUMASK, Rendering control: the frame counter keeps
      // reading the rendering bits it was reading until the write's third dot.
      state.maskBefore = state.maskLag != 0 ? state.maskBefore
                                            : static_cast<std::uint8_t>(state.mask & 0x18u);
      state.maskLag = kMaskLagDots;
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
      // address and does not step it; "PPU OAM", Byte 2 - Attributes: a sprite's byte 2
      // answers five bits; "Open bus behavior", PPU open bus: the byte answered is driven
      // onto the I/O bus whole.
      driveBus(state, oamByte(state, state.oamAddr), 0xFF, inputs);
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
      return oamByte(state, state.oamAddr);
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

PpuDotEvent ppuTickDot(PpuState& state, bool statusRead, const PpuInputs& inputs) {
  // The order of work in a dot: the count of a $2001 write on its way; the counter's
  // step; the flag and the warm-up at dot 1; the background's dot, which selects the
  // pixel of the position two dots behind, shifts, reloads, moves `v` and makes its half
  // of an access; the picture's position, drawn from the pixel selected two dots ago,
  // and that pixel's successors moved a place; the I/O bus's decay.
  //
  // A write to $2001 comes a dot closer to the frame counter; the dot that takes the
  // count to zero reads the write.
  if (state.maskLag != 0) --state.maskLag;
  PpuDotEvent event = PpuDotEvent::None;
  bool skipped = false;
  if (state.scanline == kPreRenderScanline && state.dot == kSkippedFromDot && state.odd &&
      renderingAtTheCounter(state)) {
    // NESdev wiki, "PPU frame timing", Even/Odd Frames: an odd frame with rendering
    // enabled goes from dot 339 of scanline 261 straight to dot 0 of scanline 0.
    state.dot = 0;
    state.scanline = 0;
    state.odd = !state.odd;
    event = PpuDotEvent::FrameEnded;
    skipped = true;
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
        event = PpuDotEvent::FrameEnded;
      }
    }
  }
  if (state.dot == kFlagDot) {
    // NESdev wiki, "NMI", Operation: the flag is set at dot 1 of scanline 241 and
    // cleared at dot 1 of scanline 261. "PPU frame timing", VBL Flag Timing: a read of
    // $2002 that begins on the dot before the set reads the flag clear, and the flag is
    // never set for that frame. "PPU power up state": the signal that clears the flag
    // ends the internal reset.
    if (state.scanline == kVblankScanline) {
      if (!statusRead) state.vblank = true;
      event = PpuDotEvent::VerticalBlank;
    }
    if (state.scanline == kPreRenderScanline) {
      state.vblank = false;
      state.warmingUp = false;
    }
  }
  // NESdev wiki, "PPU registers", PPUMASK, Rendering control: rendering is the chip
  // fetching memory and drawing, and the frame counter runs the background's dot while
  // it reads rendering enabled.
  const std::uint8_t selected =
      renderingAtTheCounter(state) ? backgroundDot(state, inputs, skipped) : std::uint8_t{0};
  // NESdev wiki, "PPU rendering", Drawing overview and Cycles 1-256, and its frame timing
  // diagram's notes: the dot resolves the picture position four dots behind the counter,
  // from the pixel selected two dots behind it, whether or not anything stores it; the
  // store is the host's picture, when one is held.
  if (state.scanline <= kLastPictureLine && state.dot >= kFirstPictureDot &&
      state.dot < kFirstPictureDot + kPictureWidth) {
    const std::size_t index = static_cast<std::size_t>(state.scanline) * kPictureWidth +
                              static_cast<std::size_t>(state.dot - kFirstPictureDot);
    const std::uint16_t value = pictureDot(state, state.pixelPipeline[1]);
    if (index < inputs.picture.size()) inputs.picture[index] = value;
    if (index == 0) event = PpuDotEvent::PictureBegun;
  }
  state.pixelPipeline[1] = state.pixelPipeline[0];
  state.pixelPipeline[0] = selected;
  // NESdev wiki, "PPU registers", MMIO registers: the I/O bus is a dynamic latch, and a
  // bit keeps its charge for a time after it is driven, then decays to zero.
  for (std::size_t bit = 0; bit < 8; ++bit) {
    std::uint32_t& left = state.ioLatchDecay[bit];
    if (left != 0 && --left == 0) {
      state.ioLatch = static_cast<std::uint8_t>(state.ioLatch & ~(1u << bit));
    }
  }
  return event;
}

}  // namespace neasant
