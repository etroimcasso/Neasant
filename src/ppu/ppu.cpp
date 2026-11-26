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

// What a read of $2004 answers: the byte on the chip's OAM bus while the chip renders,
// on scanlines 0-239 and 261 with rendering enabled as the frame counter reads it, and
// otherwise the OAM byte at the OAM address (NESdev wiki, "PPU registers", OAMDATA:
// reads during rendering expose the internal accesses of sprite evaluation and loading,
// and reads during vertical or forced blanking return the byte at the address).
[[nodiscard]] std::uint8_t oamAnswer(const PpuState& state) noexcept {
  const bool renderedLine =
      state.scanline <= kLastPictureLine || state.scanline == kPreRenderScanline;
  if (renderedLine && renderingAtTheCounter(state)) return state.oamBus;
  return oamByte(state, state.oamAddr);
}

// The value the chip drives for the picture position the dot resolves. With rendering
// enabled it is the palette entry `pixel` addresses, the multiplexer's pixel for the
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

// The chip drives all fourteen address lines with `address`, the console's latch
// captures the low eight, and the board is presented the address with neither strobe
// low, the low eight lines carrying the address (NESdev wiki, "PPU rendering", PPU
// address bus contents; "PPU pinout", Signal descriptions: ALE).
void driveAddress(PpuState& state, const PpuInputs& inputs, std::uint16_t address) {
  state.addressPins = address;
  state.addressLatch = static_cast<std::uint8_t>(address & 0xFFu);
  PpuPins pins{.address = address, .data = state.addressLatch};
  static_cast<void>(boardPpuAccess(inputs.board, inputs.cartridge, pins, inputs.chrRam));
}

// The ticks from a CPU access through $2007 to the dot the chip drives its address and
// to the dot of its strobe: three and five from the dot under way, one more each when
// that dot has not ticked yet (Visual 2C02: ALE three dots after the port select falls
// and /RD or /WR two dots after that, both a dot later when the select falls in a dot's
// last master cycle).
constexpr std::uint8_t kDataAccessAddressDots = 3;
constexpr std::uint8_t kDataAccessStrobeDots = 5;

// Takes a CPU access through $2007 for the chip to make on its own dots. `cycle` is the
// master cycle of the dot under way the access falls on. An access of the same kind as one
// standing at its address dot is dropped when the access falls on the dot's last master
// cycle and otherwise brings that access's strobe a dot nearer; an access of the other kind
// leaves it alone. With two accesses held a third is dropped, which no sequence on the
// CPU's bus reaches (Visual 2C02: two reads or two writes on adjacent CPU cycles, the
// second's ALE three dots after its own select and the first's /RD or /WR on the dot after
// its ALE; a read and a write on adjacent cycles each on their own dots; the second of two
// of a kind dropped when its select falls in the third master cycle of the first's ALE
// dot).
void takeDataAccess(PpuState& state, PpuDataAccessKind kind, std::uint8_t data,
                    std::uint8_t cycle) noexcept {
  const auto extra = static_cast<std::uint8_t>(cycle == 0 ? 1u : 0u);
  for (PpuDataAccess& held : state.dataAccess) {
    const auto atItsAddressDot =
        static_cast<std::uint8_t>(kDataAccessStrobeDots - kDataAccessAddressDots + extra);
    if (held.kind == kind && held.dotsToStrobe == atItsAddressDot) {
      if (cycle == 3) return;
      --held.dotsToStrobe;
      break;
    }
  }
  for (PpuDataAccess& slot : state.dataAccess) {
    if (slot.kind == PpuDataAccessKind::None) {
      slot = PpuDataAccess{
          .kind = kind,
          .data = data,
          .dotsToAddress = static_cast<std::uint8_t>(kDataAccessAddressDots + extra),
          .dotsToStrobe = static_cast<std::uint8_t>(kDataAccessStrobeDots + extra)};
      return;
    }
  }
}

// One dot of the $2007 accesses the chip holds: each a tick nearer; on its address dot
// the lines and the latch take the low fourteen bits of `v` and the board is presented
// the address alone; on its strobe dot the chip drives the high six lines from `v`
// again and reads the latched address into the read buffer, held at zero through the
// warm-up, or writes the byte there, a palette entry taking it with no strobe to the
// board, and `v` steps. Answers whether a strobe was made, the lines then carrying the
// data (NESdev wiki, "PPU registers", PPUDATA, The PPUDATA read buffer and Reading
// palette RAM; "PPU pinout", Signal descriptions: a write to the palette range does not
// assert /WR; "PPU power up state"; Visual 2C02).
bool dataAccessDot(PpuState& state, const PpuInputs& inputs) {
  bool strobed = false;
  for (PpuDataAccess& held : state.dataAccess) {
    if (held.kind == PpuDataAccessKind::None) continue;
    if (held.dotsToAddress != 0 && --held.dotsToAddress == 0) {
      driveAddress(state, inputs, static_cast<std::uint16_t>(state.v & kAddressMask));
    }
    if (--held.dotsToStrobe != 0) continue;
    state.addressPins =
        static_cast<std::uint16_t>((state.v & 0x3F00u) | (state.addressPins & 0x00FFu));
    const auto address = static_cast<std::uint16_t>((state.v & 0x3F00u) | state.addressLatch);
    if (held.kind == PpuDataAccessKind::Read) {
      const std::uint8_t byte = busRead(state, address, inputs);
      state.readBuffer = state.warmingUp ? std::uint8_t{0} : byte;
    } else if (address >= kPaletteBase) {
      state.palette[paletteIndex(address)] = static_cast<std::uint8_t>(held.data & 0x3Fu);
    } else {
      busWrite(state, address, held.data, inputs);
    }
    stepAddress(state);
    held = PpuDataAccess{};
    strobed = true;
  }
  return strobed;
}

// On a dot the chip makes no access on, the address lines carry the low fourteen bits
// of `v`, the board presented the new address when they change; the latch keeps what it
// captured (NESdev wiki, "PPU rendering", PPU address bus contents; Visual 2C02: the
// lines take a written `v` with no ALE).
void followAddress(PpuState& state, const PpuInputs& inputs) {
  const auto address = static_cast<std::uint16_t>(state.v & kAddressMask);
  if (state.addressPins == address) return;
  state.addressPins = address;
  PpuPins pins{.address = address, .data = static_cast<std::uint8_t>(address & 0xFFu)};
  static_cast<void>(boardPpuAccess(inputs.board, inputs.cartridge, pins, inputs.chrRam));
}

// The accesses the chip makes while it renders: the four of a background tile, the
// nametable fetch whose byte the chip does not keep, and the two pattern bytes of a
// sprite.
enum class Fetch : std::uint8_t {
  None,
  Nametable,
  NametableDropped,
  Attribute,
  PatternLow,
  PatternHigh,
  SpritePatternLow,
  SpritePatternHigh,
};

// The access dot `dot` of a rendered scanline belongs to, for dots 1-340. An access is
// two dots and a line's accesses run in eights from dot 1. Dots 1-256 and 321-336 fetch
// a tile every eight: its nametable byte, its attribute byte, its low pattern byte and
// its high one. Each eight of dots 257-320 opens with two nametable fetches, the first
// kept and the second dropped, and its last four dots fetch the two pattern bytes of the
// sprite in the eight's slot of secondary OAM; dots 337-340 are two nametable fetches,
// kept and dropped (NESdev wiki, "PPU rendering", Line-by-line timing, and its frame
// timing diagram with its legend: an unused nametable fetch updates the nametable byte
// register and an ignored one does not).
[[nodiscard]] Fetch fetchOn(std::uint16_t dot) noexcept {
  constexpr std::array<Fetch, 4> kTile{Fetch::Nametable, Fetch::Attribute, Fetch::PatternLow,
                                       Fetch::PatternHigh};
  constexpr std::array<Fetch, 4> kPair{Fetch::Nametable, Fetch::NametableDropped,
                                       Fetch::SpritePatternLow, Fetch::SpritePatternHigh};
  if (dot == 0) return Fetch::None;
  const std::size_t slot = static_cast<std::size_t>(((dot - 1u) & 7u) >> 1);
  if (dot <= 256 || (dot >= 321 && dot <= 336)) return kTile[slot];
  return kPair[slot];
}

// The sprites' height in rows, 8 or 16 as $2000 bit 5 says (NESdev wiki, "PPU
// registers", PPUCTRL).
[[nodiscard]] int spriteHeight(const PpuState& state) noexcept {
  return (state.ctrl & 0x20u) != 0 ? 16 : 8;
}

// The line the sprites' Y bytes are compared against: the scanline's number on the
// picture's lines, and -1 on the pre-render line, the number the page gives it (NESdev
// wiki, "PPU rendering", Pre-render scanline (-1 or 261)).
[[nodiscard]] int spriteLine(const PpuState& state) noexcept {
  return state.scanline == kPreRenderScanline ? -1 : static_cast<int>(state.scanline);
}

// Whether a sprite whose byte 0 is `y` has a row on the line after `line`: the line less
// `y` is a row of the sprite, 0 to its height less one. The difference is taken whole,
// so a sprite below the line, or one whose Y is $FF, is never in range (NESdev wiki,
// "PPU sprite evaluation", Details; "PPU OAM", Byte 0 - Y position; Brad Taylor's NTSC
// 2C02 technical reference, In-range object evaluation: a 9-bit difference).
[[nodiscard]] bool spriteInRange(const PpuState& state, int line, std::uint8_t y) noexcept {
  const int row = line - static_cast<int>(y);
  return row >= 0 && row < spriteHeight(state);
}

// The slot of secondary OAM dots 257-320 are reading, 0-7, eight dots each.
[[nodiscard]] std::size_t spriteSlot(std::uint16_t dot) noexcept {
  return static_cast<std::size_t>((dot - 257u) >> 3);
}

// The address of a sprite's pattern byte for the line, from its slot's Y and tile bytes
// and its unit's attribute byte. An 8x8 sprite's row is the scanline's number less its
// Y, in the half of the pattern table $2000 bit 3 selects; an 8x16 sprite's sixteen rows
// are two tiles, the even one and the one after it, in the half bit 0 of its tile byte
// selects; a vertical flip takes the rows from the bottom; the high byte is eight above
// the low. The row is the difference's low bits whatever the difference, so a slot
// holding no sprite for the line, tile $FF, drives an address all the same, and the
// pre-render line's fetches take 261 for the scanline (NESdev wiki, "PPU pattern tables",
// Addressing; "PPU OAM", Byte 1 - Tile/index and Byte 2 - Attributes; "PPU rendering",
// Cycles 257-320; Visual 2C02: the pre-render line's dummy fetches drive the rows 261 less
// $FF gives, flipped).
[[nodiscard]] std::uint16_t spriteAddress(const PpuState& state, std::size_t slot,
                                          bool high) noexcept {
  const std::uint8_t y = state.secondaryOam[slot * 4];
  const std::uint8_t tile = state.secondaryOam[slot * 4 + 1];
  const std::uint8_t attribute = state.spriteUnits[slot].attribute;
  const auto last = static_cast<unsigned>(spriteHeight(state) - 1);
  unsigned row =
      static_cast<unsigned>(static_cast<int>(state.scanline) - static_cast<int>(y)) & last;
  if ((attribute & 0x80u) != 0) row ^= last;
  const unsigned byte = high ? 0x08u : 0u;
  if (last == 15u) {
    return static_cast<std::uint16_t>(((tile & 0x01u) << 12) |
                                      (((tile & 0xFEu) + (row >> 3)) << 4) | (row & 0x07u) |
                                      byte);
  }
  return static_cast<std::uint16_t>(((state.ctrl & 0x08u) << 9) |
                                    (static_cast<unsigned>(tile) << 4) | row | byte);
}

// The address an access drives. A nametable fetch reads the tile `v` points at and an
// attribute fetch the byte that covers that tile (NESdev wiki, "PPU scrolling", Tile and
// attribute fetching). A pattern fetch reads the row `v`'s fine Y names of the tile the
// nametable latch names, in the half of the pattern table $2000 bit 4 selects, the high
// byte eight above the low (NESdev wiki, "PPU pattern tables", Addressing). A sprite's
// pattern fetch reads the row spriteAddress gives.
[[nodiscard]] std::uint16_t fetchAddress(const PpuState& state, Fetch fetch) noexcept {
  switch (fetch) {
    case Fetch::SpritePatternLow:
    case Fetch::SpritePatternHigh:
      return spriteAddress(state, spriteSlot(state.dot), fetch == Fetch::SpritePatternHigh);
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
  driveAddress(state, inputs,
               static_cast<std::uint16_t>((fetchAddress(state, fetch) & 0x3F00u) | low));
}

// A byte with its bits reversed. A pattern byte's bit 7 is the tile's leftmost pixel,
// and the shift registers put the leftmost pixel where it leaves first (NESdev wiki,
// "PPU pattern tables"; "PPU rendering", Picture region).
[[nodiscard]] std::uint8_t reversed(std::uint8_t byte) noexcept {
  unsigned out = 0;
  for (unsigned bit = 0; bit < 8; ++bit) {
    if ((byte >> bit) & 1u) out |= 0x80u >> bit;
  }
  return static_cast<std::uint8_t>(out);
}

// The second dot of an access: the read, at the high six lines as the chip drives them
// now, after the dot's own move of `v`, over the low eight the latch holds, and the byte
// into its latch. An attribute byte leaves its two bits for the tile, the pair bit 1 of
// `v`'s coarse X and bit 1 of its coarse Y select (NESdev wiki, "PPU rendering", its
// notes: reads come from an address composed of the latched low 8 bits and the current
// high 6 bits; "PPU attribute tables"). A sprite's pattern byte enters its unit's
// register with the leftmost pixel at bit 0, reversed unless the sprite is flipped
// horizontally, and as 0 when the slot's Y has no row on the line (NESdev wiki, "PPU
// rendering", Cycles 257-320; "PPU OAM", Byte 2 - Attributes).
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
    case Fetch::SpritePatternLow:
    case Fetch::SpritePatternHigh: {
      const std::size_t slot = spriteSlot(state.dot);
      SpriteUnit& unit = state.spriteUnits[slot];
      const bool inRange = spriteInRange(state, spriteLine(state), state.secondaryOam[slot * 4]);
      const std::uint8_t row = (unit.attribute & 0x40u) != 0 ? byte : reversed(byte);
      unit.pattern[fetch == Fetch::SpritePatternHigh ? 1 : 0] = inRange ? row : std::uint8_t{0};
      return;
    }
    default:
      return;
  }
}

// A pattern byte as a background shift register's high eight bits take it: its bit 7,
// the tile's leftmost pixel, at bit 8, the first to reach the low eight (NESdev wiki,
// "PPU pattern tables"; "PPU rendering", Picture region).
[[nodiscard]] std::uint16_t loaded(std::uint8_t byte) noexcept {
  return static_cast<std::uint16_t>(static_cast<unsigned>(reversed(byte)) << 8);
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

// What the sprites contribute to picture position `position`, stepping the eight units:
// a unit whose X counter is not 0 counts it down; one whose counter is 0 shifts its
// registers out a pixel, its pattern's two bits; and the frontmost unit with an opaque
// pixel wins, whatever its priority. The pixel is the winner's palette over its pattern,
// and 0 when no unit's is opaque, while $2001 bit 4 is clear, and in the eight leftmost
// positions while bit 2 is; `behind` is the winner's priority bit and `unit0` whether the
// winner is the first unit (NESdev wiki, "PPU sprite priority", Detailed internals of
// the sprite priority quirk; "PPU registers", PPUMASK, Rendering control; Brad Taylor's
// NTSC 2C02 technical reference, Details of object pattern fetch & render: the counter
// is decremented every pixel and the registers serialize once it is 0).
struct SpritePixel {
  std::uint8_t pixel = 0;
  bool behind = false;
  bool unit0 = false;
};

[[nodiscard]] SpritePixel spritePixel(PpuState& state, unsigned position) noexcept {
  SpritePixel out;
  for (std::size_t index = 0; index < state.spriteUnits.size(); ++index) {
    SpriteUnit& unit = state.spriteUnits[index];
    if (unit.x != 0) {
      --unit.x;
      continue;
    }
    const unsigned pattern = (unit.pattern[0] & 1u) | ((unit.pattern[1] & 1u) << 1);
    unit.pattern[0] = static_cast<std::uint8_t>(unit.pattern[0] >> 1);
    unit.pattern[1] = static_cast<std::uint8_t>(unit.pattern[1] >> 1);
    if (pattern != 0 && out.pixel == 0) {
      out.pixel = static_cast<std::uint8_t>(((unit.attribute & 0x03u) << 2) | pattern);
      out.behind = (unit.attribute & 0x20u) != 0;
      out.unit0 = index == 0;
    }
  }
  if ((state.mask & 0x10u) == 0 || (position < 8 && (state.mask & 0x04u) == 0)) return {};
  return out;
}

// The pixel selected for picture position `position`: the background's and the sprites'
// through the priority multiplexer, a sprite's with bit 4 set. Both transparent gives 0,
// the backdrop; one opaque gives that one; both opaque gives the sprite's unless its
// priority bit puts it behind. Sprite 0 hit is set where both are opaque and the
// sprite's is the first unit's with sprite 0 in it, at every position but 255 (NESdev
// wiki, "PPU rendering", Picture region and its priority multiplexer decision table;
// "PPU OAM", Sprite 0 hits; "PPU registers", PPUSTATUS, Sprite 0 hit flag).
[[nodiscard]] std::uint8_t selectPixel(PpuState& state, unsigned position) noexcept {
  const std::uint8_t background = backgroundPixel(state, position);
  const SpritePixel sprite = spritePixel(state, position);
  if (background != 0 && sprite.pixel != 0 && sprite.unit0 && state.sprite0Current &&
      position != 255) {
    state.sprite0Hit = true;
  }
  if (sprite.pixel == 0) return background;
  if (background == 0 || !sprite.behind) return static_cast<std::uint8_t>(0x10u | sprite.pixel);
  return background;
}

// The evaluation's step on an even dot of 66-256, on the byte the odd dot before it read
// onto the OAM bus (NESdev wiki, "PPU sprite evaluation", Details, its numbered steps). A
// sprite being copied gets its next byte into secondary OAM, or has it read past once
// eight are found; otherwise the byte is a Y, written to the next free byte while one is
// free: in range, its sprite's three other bytes follow it, and the first sprite the
// line looked at marks sprite 0 for the next line; out of range, the pointer moves to
// the next sprite. Once eight are found the writes stop, a Y in range sets the overflow
// flag and three more bytes are read, and a Y out of range moves the pointer to the
// next sprite and the next byte at once, the fault that reads the bytes diagonally. When
// the pointer passes sprite 63 the evaluation is done, and so it is after the ninth
// sprite's three bytes: each even dot then moves the pointer a sprite and copies nothing.
void evaluateEven(PpuState& state) noexcept {
  const bool full = state.secondaryAddress >= state.secondaryOam.size();
  if (state.evaluationDone) {
    state.oamAddr = static_cast<std::uint8_t>(state.oamAddr + 4u);
    return;
  }
  if (state.copyLeft != 0) {
    if (!full) state.secondaryOam[state.secondaryAddress++] = state.oamBus;
    state.oamAddr = static_cast<std::uint8_t>(state.oamAddr + 1u);
    --state.copyLeft;
    if (state.copyLeft == 0 && (full || state.oamAddr < 4u)) state.evaluationDone = true;
    return;
  }
  const bool inRange = spriteInRange(state, spriteLine(state), state.oamBus);
  if (!full) {
    state.secondaryOam[state.secondaryAddress] = state.oamBus;
    if (inRange) {
      if (state.dot == 66) state.sprite0Next = true;
      ++state.secondaryAddress;
      state.copyLeft = 3;
      state.oamAddr = static_cast<std::uint8_t>(state.oamAddr + 1u);
    } else {
      state.oamAddr = static_cast<std::uint8_t>(state.oamAddr + 4u);
      if (state.oamAddr < 4u) state.evaluationDone = true;
    }
    return;
  }
  if (inRange) {
    state.spriteOverflow = true;
    state.copyLeft = 3;
    state.oamAddr = static_cast<std::uint8_t>(state.oamAddr + 1u);
    return;
  }
  state.oamAddr = static_cast<std::uint8_t>(((state.oamAddr + 4u) & 0xFCu) |
                                            ((state.oamAddr + 1u) & 0x03u));
  if (state.oamAddr < 4u) state.evaluationDone = true;
}

// The sprites' work on a dot of a rendered scanline, after the background's, as the chip
// accesses OAM and secondary OAM. On scanlines 0-239: dots 1-64 clear secondary OAM, each
// odd dot putting $FF on the OAM bus and each even dot writing it to the next byte, and
// dot 1 clears sprite 0's flag for the next line; dot 65 begins the evaluation at the
// first free byte and, the chip being a 2C02G, copies the eight-byte row of OAM the OAM
// address is in over OAM's first eight bytes when the address is 8 or more; each odd dot
// of 65-255 reads OAM at the OAM address onto the bus, and each even dot of 66-256 takes
// the evaluation's step. On scanlines 0-239 and 261: dots 257-320 hold the OAM address
// at 0 and read each slot of secondary OAM onto the bus, its four bytes on the first four
// dots of its eight and its X on the other four, the attribute into the slot's unit on
// the third dot and the X into its counter on the fourth, and dot 257 takes sprite 0's
// flag for the line from the one the evaluation set; dots 321-340 read the first byte of
// secondary OAM, as dot 0 of scanlines 0-240 does (NESdev wiki, "PPU sprite evaluation",
// Details and Notes; "PPU rendering", Cycles 257-320; "PPU registers", OAMADDR, Values
// during rendering and OAMADDR precautions; "PPU OAM", Internal operation).
void spriteDot(PpuState& state) noexcept {
  const std::uint16_t dot = state.dot;
  const bool picture = state.scanline <= kLastPictureLine;
  if (dot == 0) {
    if (state.scanline <= kPostRenderScanline) state.oamBus = state.secondaryOam[0];
    return;
  }
  if (dot <= 64) {
    if (!picture) return;
    if (dot == 1) state.sprite0Next = false;
    state.oamBus = 0xFF;
    if ((dot & 1u) == 0) state.secondaryOam[static_cast<std::size_t>((dot >> 1) - 1u)] = 0xFF;
    return;
  }
  if (dot <= 256) {
    if (!picture) return;
    if ((dot & 1u) != 0) {
      if (dot == 65) {
        state.secondaryAddress = 0;
        state.copyLeft = 0;
        state.evaluationDone = false;
        if (state.oamAddr >= 8u) {
          const std::size_t row = state.oamAddr & 0xF8u;
          for (std::size_t byte = 0; byte < 8; ++byte) state.oam[byte] = state.oam[row + byte];
        }
      }
      state.oamBus = oamByte(state, state.oamAddr);
    } else {
      evaluateEven(state);
    }
    return;
  }
  if (dot <= 320) {
    state.oamAddr = 0;
    const std::size_t slot = spriteSlot(dot);
    const unsigned step = (dot - 257u) & 7u;
    if (dot == 257) state.sprite0Current = state.sprite0Next;
    state.oamBus = state.secondaryOam[slot * 4 + (step < 3u ? step : 3u)];
    if (step == 2) state.spriteUnits[slot].attribute = state.oamBus;
    if (step == 3) state.spriteUnits[slot].x = state.oamBus;
    return;
  }
  state.oamBus = state.secondaryOam[0];
}

// The chip's work on a dot with rendering enabled, answering the pixel selected on it, 0
// when none is. On scanlines 0-239 and 261: dots 2-257 and 322-337 shift the background's
// registers, the picture's scanlines first selecting the pixel of the position two dots
// behind on dots 2-257, from the background's registers and the sprites' units through
// the multiplexer, and the background's registers reload after the shift on every eighth
// dot from 9 to 257 and on dots 329 and 337; then `v`'s move for the dot and the dot's
// half of an access, the move falling after the latch has captured an address's low
// eight and before the high six lines and a read are taken; then the sprites' work on
// OAM. Dot 0 of scanlines 0-240 drives the low pattern byte's address and reads nothing.
// `skipped` is the dot an odd frame reaches from dot 339 of scanline 261, which makes
// the read dot 340 would have made in place of dot 0's address (NESdev wiki, "PPU
// rendering", Pre-render scanline, Cycle 0, Cycles 1-256 and Cycles 257-320, and its
// frame timing diagram).
[[nodiscard]] std::uint8_t renderDot(PpuState& state, const PpuInputs& inputs, bool skipped) {
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
      spriteDot(state);
    }
    return 0;
  }
  const bool preRender = scanline == kPreRenderScanline;
  if (scanline > kLastPictureLine && !preRender) return 0;
  std::uint8_t selected = 0;
  if ((dot >= 2 && dot <= 257) || (dot >= 322 && dot <= 337)) {
    if (!preRender && dot <= 257) selected = selectPixel(state, dot - 2u);
    shiftBackground(state);
    if ((dot & 7u) == 1) reloadBackground(state);
  }
  const Fetch fetch = fetchOn(dot);
  const auto low = static_cast<std::uint8_t>(fetchAddress(state, fetch) & 0xFFu);
  moveV(state, dot, preRender);
  if (fetch != Fetch::None) {
    if ((dot & 1u) != 0) {
      addressDot(state, inputs, fetch, low);
    } else {
      readDot(state, inputs, fetch);
    }
  }
  spriteDot(state);
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
  // The line is held for far longer than a $2007 access takes to its strobe dot.
  state.dataAccess = {};
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
    default:
      // NESdev wiki, "PPU registers", PPUDATA: the write lands at the current address
      // and the address steps, on the chip's own dots (dataAccessDot).
      takeDataAccess(state, PpuDataAccessKind::Write, value, inputs.accessCycle);
      return;
  }
}

std::uint8_t ppuRead(PpuState& state, std::uint8_t port, const PpuInputs& inputs) {
  switch (port) {
    case 2: {
      // NESdev wiki, "NMI", Operation: the flag is answered and then cleared; "PPU
      // registers", PPUSTATUS: the sprite 0 hit and overflow flags are answered in bits
      // 6 and 5 and left; "PPU scrolling", $2002 read: the toggle is cleared; "Open bus
      // behavior", PPU open bus: bits 7-5 of the I/O bus are driven and its low five
      // bits are answered as they stand.
      const auto value = static_cast<std::uint8_t>(
          (state.vblank ? 0x80u : 0u) | (state.sprite0Hit ? 0x40u : 0u) |
          (state.spriteOverflow ? 0x20u : 0u) | (state.ioLatch & 0x1Fu));
      state.vblank = false;
      state.w = false;
      driveBus(state, value, 0xE0, inputs);
      return value;
    }
    case 4:
      // NESdev wiki, "PPU registers", OAMDATA: a read answers the byte at the OAM
      // address and does not step it, and while the chip renders it exposes the chip's
      // own accesses to OAM; "PPU OAM", Byte 2 - Attributes: a sprite's byte 2 answers
      // five bits; "Open bus behavior", PPU open bus: the byte answered is driven onto
      // the I/O bus whole.
      driveBus(state, oamAnswer(state), 0xFF, inputs);
      return state.ioLatch;
    case 7: {
      const auto address = static_cast<std::uint16_t>(state.v & kAddressMask);
      std::uint8_t value;
      if (address >= kPaletteBase) {
        // NESdev wiki, "PPU registers", Reading palette RAM: the entry is answered at
        // once under the I/O bus's top two bits, which stay as they are.
        value = static_cast<std::uint8_t>((state.ioLatch & 0xC0u) | paletteRead(state, address));
        driveBus(state, value, 0x3F, inputs);
      } else {
        // NESdev wiki, "PPU registers", The PPUDATA read buffer: the buffer is answered.
        value = state.readBuffer;
        driveBus(state, value, 0xFF, inputs);
      }
      // The read the chip makes, which refills the buffer and steps the address, comes
      // on its own dots (dataAccessDot).
      takeDataAccess(state, PpuDataAccessKind::Read, 0, inputs.accessCycle);
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
      return static_cast<std::uint8_t>(
          (state.vblank ? 0x80u : 0u) | (state.sprite0Hit ? 0x40u : 0u) |
          (state.spriteOverflow ? 0x20u : 0u) | (state.ioLatch & 0x1Fu));
    case 4:
      return oamAnswer(state);
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
  // step; the flags and the warm-up at dot 1; the rendered dot, which selects the pixel
  // of the position two dots behind from the background's registers and the sprites'
  // units, shifts, reloads, moves `v`, makes its half of an access and does the sprites'
  // work on OAM; the dots of the $2007 accesses the chip holds, and the lines following
  // `v` on a dot with no access; the picture's position, drawn from the pixel selected
  // two dots ago, and that pixel's successors moved a place; the I/O bus's decay.
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
      // NESdev wiki, "PPU registers", PPUSTATUS: the three flags are cleared on dot 1 of
      // the pre-render scanline.
      state.vblank = false;
      state.sprite0Hit = false;
      state.spriteOverflow = false;
      state.warmingUp = false;
    }
  }
  // NESdev wiki, "PPU registers", PPUMASK, Rendering control: rendering is the chip
  // fetching memory and drawing, and the frame counter runs the rendered dot while it
  // reads rendering enabled.
  const std::uint8_t selected =
      renderingAtTheCounter(state) ? renderDot(state, inputs, skipped) : std::uint8_t{0};
  // The $2007 accesses the chip holds take their dots, and on a dot with no access of the
  // fetch circuit's and no strobe the lines follow `v` (NESdev wiki, "PPU rendering", PPU
  // address bus contents; Visual 2C02: the lines carry `v` on dot 0 of the pre-render
  // line).
  const bool fetching =
      renderingAtTheCounter(state) &&
      (state.scanline <= kLastPictureLine ||
       (state.scanline == kPreRenderScanline && state.dot != 0) ||
       (state.scanline == kPostRenderScanline && state.dot == 0));
  const bool strobed = dataAccessDot(state, inputs);
  if (!fetching && !strobed) followAddress(state, inputs);
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
