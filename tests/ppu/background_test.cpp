#include "neasant/nes/board.h"
#include "neasant/nes/cartridge.h"
#include "neasant/nes/nes.h"
#include "neasant/nes/video_frame.h"
#include "neasant/ppu/ppu.h"
#include "authored_image.h"
#include "rom_harness.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

// The background. On a bare chip: a tile's four accesses, two dots each, with the address
// lines, the console's latch and the four fetch latches dot by dot; the pattern table's
// half; the kilobyte of nametable RAM the board selects; a whole line's accesses in
// eights; the nametable fetches of dots 257-320 and 337-340, the first of a pair kept and
// the second dropped, with the address the first one after dot 256 mixes; dot 0's address
// and the read an odd frame's skip leaves in its place; `v`'s steps and copies; the shift
// registers' shifts and reloads; the pixel the fine X scroll picks, its two dots to the
// picture, the palette beside it, the hidden background and the hidden left column;
// rendering disabled, and a write to $2001 reaching all of it on its third dot. On the
// machine: a tile where the nametable puts it, an attribute byte's four quarters, each
// fine X and each fine Y, a scroll across both nametable seams, coarse Y past 29, the
// left column, the pattern table's half, sprites alone, a program drawing through the
// ports, and two machines equal watched and unwatched. A case that checks a hardware rule
// names the document and section it is read from.

namespace {

using neasant::Board;
using neasant::boardFor;
using neasant::boardPpuAccess;
using neasant::Cartridge;
using neasant::kPictureDots;
using neasant::kPictureWidth;
using neasant::makeCartridge;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::PpuDotEvent;
using neasant::PpuInputs;
using neasant::PpuPins;
using neasant::PpuState;
using neasant::ppuTickDot;
using neasant::ppuWrite;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::chrAt;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::PictureKeeper;
using neasant::nes_tests::placeProgram;

using Bytes = std::vector<std::uint8_t>;
using Program = std::initializer_list<std::uint8_t>;

// A value no dot takes, bits 15-9 of a dot being 0: what a picture's entries hold before
// the chip writes them.
constexpr std::uint16_t kBlank = 0xFFFF;

// One frame with rendering off, in master cycles: 341 x 262 dots x 4 (NESdev wiki, "PPU
// frame timing", Even/Odd Frames).
constexpr std::uint64_t kFrame = 357368;

// JMP $8000
constexpr Program kJmpLoop = {0x4C, 0x00, 0x80};

// The backdrop every case draws over, and the colour of each palette's three entries:
// entry 4p + c of palette RAM holds kColour[p][c - 1].
constexpr std::uint8_t kBackdrop = 0x0F;
constexpr std::array<std::array<std::uint8_t, 3>, 4> kColour{{
    {0x01, 0x02, 0x03},
    {0x15, 0x16, 0x17},
    {0x29, 0x2A, 0x2B},
    {0x31, 0x32, 0x33},
}};

// Fills the background's half of palette RAM: the backdrop and the four palettes.
void setColours(PpuState& state) {
  state.palette[0] = kBackdrop;
  for (std::size_t p = 0; p < 4; ++p) {
    for (std::size_t c = 1; c <= 3; ++c) state.palette[4 * p + c] = kColour[p][c - 1];
  }
}

// A bare chip over an NROM board with 16 KB of PRG ROM and 8 KB of CHR RAM, which a case
// writes its patterns into; its warm-up window closed as it is after the first pre-render
// line, the background shown with its left column, holding a picture the chip writes
// into, every entry blank until it does.
struct Chip {
  Bytes image;
  Cartridge cartridge;
  Board board;
  std::vector<std::uint8_t> chrRam;
  std::vector<std::uint16_t> picture = std::vector<std::uint16_t>(kPictureDots, kBlank);
  PpuState state;
  PpuInputs inputs;

  explicit Chip(bool verticalMirroring = false)
      : image(authoredImage(
            inesHeader({.chrUnits = 0, .verticalMirroring = verticalMirroring}), 16384, 0)),
        cartridge(makeCartridge(image)),
        board(boardFor(cartridge)),
        chrRam(cartridge.chrRamBytes),
        inputs{board, cartridge, chrRam, 115200u, picture} {
    state.warmingUp = false;
    state.mask = 0x0A;
    setColours(state);
  }

  void write(std::uint8_t port, std::uint8_t value) { ppuWrite(state, port, value, inputs); }
  PpuDotEvent tick() { return ppuTickDot(state, false, inputs); }
  // Puts the counter at `scanline`, `dot`: the next tick is the dot after it.
  void at(std::uint16_t scanline, std::uint16_t dot) {
    state.scanline = scanline;
    state.dot = dot;
  }
  // Ticks until the counter stands at `dot` of the scanline it is on.
  void tickTo(std::uint16_t dot) {
    while (state.dot != dot) tick();
  }
  // The picture's entry for position `x` of line `y`.
  [[nodiscard]] std::uint16_t dotAt(std::size_t x, std::size_t y) const {
    return picture[y * kPictureWidth + x];
  }
};

// The four tiles the machine's cases draw with, in the pattern table at $0000, and one in
// the table at $1000: tile 1 every pixel 1, tile 2 every pixel 2, tile 3 every pixel 3,
// tile 4 its top row alone pixel 1; and at $1000 tile 1 every pixel 2. A tile is sixteen
// bytes, a row's low bits in the first eight and its high bits in the second (NESdev
// wiki, "PPU pattern tables").
void placePatterns(Bytes& image) {
  for (std::size_t i = 0; i < 8; ++i) {
    chrAt(image, 16384, 0x0010 + i) = 0xFF;
    chrAt(image, 16384, 0x0028 + i) = 0xFF;
    chrAt(image, 16384, 0x0030 + i) = 0xFF;
    chrAt(image, 16384, 0x0038 + i) = 0xFF;
    chrAt(image, 16384, 0x1018 + i) = 0xFF;
  }
  chrAt(image, 16384, 0x0040) = 0xFF;
}

// A 16 KB NROM image with the patterns in 8 KB of CHR ROM, `program` at $8000 and every
// vector pointing there; `verticalMirroring` puts $2000 and $2400 on the two kilobytes of
// nametable RAM, and without it $2000 and $2800 are.
Bytes imageWith(Program program, bool verticalMirroring = false) {
  Bytes image =
      authoredImage(inesHeader({.verticalMirroring = verticalMirroring}), 16384, 8192);
  placeProgram(image, 0, std::span<const std::uint8_t>(program.begin(), program.size()));
  placePatterns(image);
  return image;
}

// Steps `cycles` machine cycles.
void stepCycles(Nes& machine, int cycles) {
  for (int i = 0; i < cycles; ++i) machine.step();
}

// A machine over `image` with the flag clear at power-on, stepped through its reset
// sequence, its PPU then given the colours, the warm-up window closed and the background
// shown with its left column, changed further by `edit`, and restored.
template <typename Edit>
Nes prepared(const Bytes& image, Edit edit) {
  Nes machine(NesConfig{.rom = image, .vblankAtPowerOn = false});
  stepCycles(machine, 7);
  NesState state = machine.state();
  state.ppu.warmingUp = false;
  state.ppu.mask = 0x0A;
  setColours(state.ppu);
  edit(state.ppu);
  machine.restore(state);
  return machine;
}

// The picture the machine draws of what `edit` set up: the second frame's, the first one
// begun after the set-up, whose scroll the pre-render line took from `t`.
template <typename Edit>
std::vector<std::uint16_t> pictureOf(const Bytes& image, Edit edit) {
  Nes machine = prepared(image, edit);
  PictureKeeper keeper;
  machine.setFrameObserver(&keeper);
  machine.run(2 * kFrame);
  EXPECT_EQ(keeper.pictures, 1u);
  machine.setFrameObserver(nullptr);
  return keeper.dots;
}

// Checks every dot of `dots` against `expected(x, y)`, naming the first that differs.
template <typename Expected>
void expectPicture(const std::vector<std::uint16_t>& dots, Expected expected) {
  ASSERT_EQ(dots.size(), kPictureDots);
  for (std::size_t y = 0; y < kPictureDots / kPictureWidth; ++y) {
    for (std::size_t x = 0; x < kPictureWidth; ++x) {
      const std::uint16_t want = expected(x, y);
      const std::uint16_t got = dots[y * kPictureWidth + x];
      if (got != want) {
        ADD_FAILURE() << "position " << x << " of line " << y << " is " << got << ", expected "
                      << want;
        return;
      }
    }
  }
}

// The access a dot's address is for, read off the lines: a nametable or an attribute
// address has line 13 high, an attribute address with bits 9-6 set too, and a pattern
// address has line 13 low, its bit 3 the high byte's.
char accessOf(std::uint16_t pins) {
  if ((pins & 0x2000u) != 0) return (pins & 0x03C0u) == 0x03C0u ? 'A' : 'N';
  return (pins & 0x0008u) != 0 ? 'H' : 'L';
}

// ---- The bare chip -----------------------------------------------------------------

// A tile is four accesses of two dots each: the address on the first dot, on all fourteen
// lines with the low eight captured by the console's latch, and the read on the second,
// its byte kept in the access's latch. The nametable byte names the tile, the attribute
// byte leaves the two bits of the tile's quarter, and the two pattern bytes are the
// tile's row at `v`'s fine Y, the high byte eight above the low (NESdev wiki, "PPU
// rendering", Cycles 1-256 and PPU address bus contents; "PPU scrolling", Tile and
// attribute fetching; "PPU pattern tables", Addressing; "PPU attribute tables").
TEST(PpuBackground, ATileIsFourAccessesOfTwoDotsEach) {
  Chip chip;
  chip.state.v = 0x54A7;
  chip.state.ciram[0x0A7] = 0x4E;
  chip.state.ciram[0x3C9] = 0xB4;
  chip.chrRam[0x04E5] = 0x41;
  chip.chrRam[0x04ED] = 0x16;
  chip.at(10, 0);
  chip.tick();  // dot 1
  EXPECT_EQ(chip.state.addressPins, 0x24A7);
  EXPECT_EQ(chip.state.addressLatch, 0xA7);
  EXPECT_EQ(chip.state.nametableLatch, 0);
  chip.tick();  // dot 2
  EXPECT_EQ(chip.state.addressPins, 0x24A7);
  EXPECT_EQ(chip.state.nametableLatch, 0x4E);
  chip.tick();  // dot 3
  EXPECT_EQ(chip.state.addressPins, 0x27C9);
  EXPECT_EQ(chip.state.addressLatch, 0xC9);
  EXPECT_EQ(chip.state.attributeLatch, 0);
  chip.tick();  // dot 4
  EXPECT_EQ(chip.state.attributeLatch, 1);
  chip.tick();  // dot 5
  EXPECT_EQ(chip.state.addressPins, 0x04E5);
  EXPECT_EQ(chip.state.addressLatch, 0xE5);
  EXPECT_EQ(chip.state.patternLatch[0], 0);
  chip.tick();  // dot 6
  EXPECT_EQ(chip.state.patternLatch[0], 0x41);
  chip.tick();  // dot 7
  EXPECT_EQ(chip.state.addressPins, 0x04ED);
  EXPECT_EQ(chip.state.addressLatch, 0xED);
  EXPECT_EQ(chip.state.patternLatch[1], 0);
  EXPECT_EQ(chip.state.v, 0x54A7);
  chip.tick();  // dot 8
  EXPECT_EQ(chip.state.patternLatch[1], 0x16);
  EXPECT_EQ(chip.state.v, 0x54A8);
}

// $2000 bit 4 selects the half of the pattern table the background's tiles are read from:
// $1000 with the bit set (NESdev wiki, "PPU pattern tables", Addressing; "PPU registers",
// PPUCTRL).
TEST(PpuBackground, Bit4Of2000SelectsThePatternTablesHalf) {
  Chip chip;
  chip.state.ctrl = 0x10;
  chip.state.v = 0x54A7;
  chip.state.ciram[0x0A7] = 0x4E;
  chip.chrRam[0x04E5] = 0x41;
  chip.chrRam[0x14E5] = 0x99;
  chip.at(10, 0);
  chip.tickTo(5);
  EXPECT_EQ(chip.state.addressPins, 0x14E5);
  chip.tick();  // dot 6
  EXPECT_EQ(chip.state.patternLatch[0], 0x99);
}

// A nametable fetch reaches the kilobyte of the console's RAM the board selects: under
// horizontal mirroring $2400 is the kilobyte $2000 is and $2800 the other, and under
// vertical mirroring the reverse (NESdev wiki, "PPU nametables", Mirroring; "Cartridge
// connector", Signal descriptions: CIRAM A10).
TEST(PpuBackground, ANametableFetchReachesTheKilobyteTheBoardSelects) {
  const auto fetched = [](bool verticalMirroring, std::uint16_t v) {
    Chip chip(verticalMirroring);
    chip.state.ciram[0x005] = 0x11;
    chip.state.ciram[0x405] = 0x22;
    chip.state.v = v;
    chip.at(10, 0);
    chip.tickTo(2);
    return chip.state.nametableLatch;
  };
  EXPECT_EQ(fetched(false, 0x0005), 0x11);
  EXPECT_EQ(fetched(false, 0x0405), 0x11);
  EXPECT_EQ(fetched(false, 0x0805), 0x22);
  EXPECT_EQ(fetched(false, 0x0C05), 0x22);
  EXPECT_EQ(fetched(true, 0x0005), 0x11);
  EXPECT_EQ(fetched(true, 0x0805), 0x11);
  EXPECT_EQ(fetched(true, 0x0405), 0x22);
  EXPECT_EQ(fetched(true, 0x0C05), 0x22);
}

// The board is presented an address alone on a fetch's first dot, with neither strobe
// low: it drives the CIRAM lines for the address and reads and writes nothing, the data
// lines and its memory left as they were (NESdev wiki, "Cartridge connector", Signal
// descriptions; "PPU pinout", Signal descriptions).
TEST(PpuBackground, AnAddressAloneMovesTheCiramLinesAndNothingElse) {
  Chip chip(true);
  chip.chrRam[0x0123] = 0x77;
  PpuPins pattern{.address = 0x0123, .data = 0x5A};
  EXPECT_TRUE(boardPpuAccess(chip.board, chip.cartridge, pattern, chip.chrRam));
  EXPECT_EQ(pattern.data, 0x5A);
  EXPECT_EQ(chip.chrRam[0x0123], 0x77);
  EXPECT_FALSE(pattern.ciramEnable);
  PpuPins nametable{.address = 0x2400, .data = 0x5A};
  EXPECT_FALSE(boardPpuAccess(chip.board, chip.cartridge, nametable, chip.chrRam));
  EXPECT_EQ(nametable.data, 0x5A);
  EXPECT_TRUE(nametable.ciramEnable);
  EXPECT_TRUE(nametable.ciramA10);
}

// A line's accesses run in eights from dot 1: 32 tiles on dots 1-256, each a nametable,
// an attribute, a low pattern and a high pattern access; on dots 257-320 two nametable
// accesses open each eight and its other four dots fetch a sprite's two pattern bytes;
// two more tiles on dots 321-336; two nametable accesses on dots 337-340; and the next
// line's dot 0 drives a low pattern address (NESdev wiki, "PPU rendering", Line-by-line
// timing, and its frame timing diagram).
TEST(PpuBackground, ALineIsAccessesInEightsFromDot1) {
  Chip chip;
  chip.at(10, 0);
  std::string accesses;
  for (int dot = 1; dot <= 340; ++dot) {
    chip.tick();
    accesses += accessOf(chip.state.addressPins);
  }
  std::string expected;
  for (int tile = 0; tile < 32; ++tile) expected += "NNAALLHH";
  for (int eight = 0; eight < 8; ++eight) expected += "NNNNLLHH";
  expected += "NNAALLHHNNAALLHHNNNN";
  EXPECT_EQ(accesses, expected);
  EXPECT_EQ(chip.state.v, 0x1002);
  chip.tick();
  EXPECT_EQ(chip.state.scanline, 11);
  EXPECT_EQ(accessOf(chip.state.addressPins), 'L');
}

// On dots 257-320 each eight opens with two nametable fetches. The first one after dot
// 256 drives the low eight lines as `v` stood when the dot began and the high six as the
// dot's copy of the horizontal bits leaves it; its byte is kept. The second reads the
// tile `v` now points at and its byte is dropped. The four dots after them fetch the
// slot's sprite pattern bytes, here a slot of zeros on line 10, row 2 of tile 0, and
// leave the nametable byte as it was (NESdev wiki, "PPU rendering", Cycles 257-320, and
// its frame timing diagram's legend: unused nametable fetches update the nametable byte
// register, ignored ones do not).
TEST(PpuBackground, TheNametableFetchesOfDots257To320) {
  Chip chip;
  chip.state.v = 0x0B5A;
  chip.state.t = 0x0405;
  chip.state.ciram[0x75A] = 0x77;
  chip.state.ciram[0x745] = 0x88;
  chip.at(10, 256);
  chip.tick();  // dot 257
  EXPECT_EQ(chip.state.v, 0x0F45);
  EXPECT_EQ(chip.state.addressPins, 0x2F5A);
  EXPECT_EQ(chip.state.addressLatch, 0x5A);
  chip.tick();  // dot 258
  EXPECT_EQ(chip.state.nametableLatch, 0x77);
  chip.tick();  // dot 259
  EXPECT_EQ(chip.state.addressPins, 0x2F45);
  EXPECT_EQ(chip.state.addressLatch, 0x45);
  chip.tick();  // dot 260
  EXPECT_EQ(chip.state.nametableLatch, 0x77);
  chip.state.ciram[0x745] = 0x99;
  chip.tick();  // dot 261
  EXPECT_EQ(chip.state.addressPins, 0x0002);
  EXPECT_EQ(chip.state.addressLatch, 0x02);
  chip.tick();  // dot 262
  EXPECT_EQ(chip.state.nametableLatch, 0x77);
  chip.tick();  // dot 263
  EXPECT_EQ(chip.state.addressPins, 0x000A);
  EXPECT_EQ(chip.state.addressLatch, 0x0A);
  chip.tick();  // dot 264
  EXPECT_EQ(chip.state.nametableLatch, 0x77);
  chip.tick();  // dot 265
  EXPECT_EQ(chip.state.addressPins, 0x2F45);
  chip.tick();  // dot 266
  EXPECT_EQ(chip.state.nametableLatch, 0x99);
  EXPECT_EQ(chip.state.v, 0x0F45);
}

// Dots 337-340 are two nametable fetches of the tile the next line's dot 1 fetches again:
// the first byte is kept and the second dropped. Dot 0 then drives the address of the low
// pattern byte of the tile the kept byte names, on all fourteen lines, and reads nothing
// (NESdev wiki, "PPU rendering", Cycles 337-340 and Cycle 0, and its frame timing
// diagram's legend).
TEST(PpuBackground, TheTwoFetchesOfDots337To340AndDot0sAddress) {
  Chip chip;
  chip.state.v = 0x3005;
  chip.state.ciram[0x005] = 0x31;
  chip.chrRam[0x0313] = 0xAA;
  chip.at(10, 336);
  chip.tick();  // dot 337
  EXPECT_EQ(chip.state.addressPins, 0x2005);
  chip.tick();  // dot 338
  EXPECT_EQ(chip.state.nametableLatch, 0x31);
  chip.state.ciram[0x005] = 0x32;
  chip.tick();  // dot 339
  EXPECT_EQ(chip.state.addressPins, 0x2005);
  chip.tick();  // dot 340
  EXPECT_EQ(chip.state.nametableLatch, 0x31);
  EXPECT_EQ(chip.tick(), PpuDotEvent::None);  // dot 0 of scanline 11
  EXPECT_EQ(chip.state.scanline, 11);
  EXPECT_EQ(chip.state.addressPins, 0x0313);
  EXPECT_EQ(chip.state.addressLatch, 0x13);
  EXPECT_EQ(chip.state.patternLatch[0], 0);
  chip.tick();  // dot 1
  EXPECT_EQ(chip.state.addressPins, 0x2005);
  EXPECT_EQ(chip.state.addressLatch, 0x05);
}

// Dot 0 drives the pattern address on scanlines 0-240: after each line of the picture,
// the last included, and after the pre-render line on a frame that does not skip. The
// vertical blank's lines and the pre-render line's own dot 0 make no access, and the
// lines carry `v` (NESdev wiki, "PPU rendering", Cycle 0, Pre-render scanline and PPU
// address bus contents, and its frame timing diagram).
TEST(PpuBackground, Dot0DrivesItsAddressOnScanlines0To240) {
  const auto driven = [](std::uint16_t scanline) {
    Chip chip;
    chip.state.ctrl = 0x10;
    chip.state.v = 0x3000;
    chip.state.nametableLatch = 0x31;
    chip.at(scanline, 340);
    chip.tick();
    return chip.state.addressPins;
  };
  EXPECT_EQ(driven(238), 0x1313);
  EXPECT_EQ(driven(239), 0x1313);
  EXPECT_EQ(driven(240), 0x3000);
  EXPECT_EQ(driven(260), 0x3000);
  EXPECT_EQ(driven(261), 0x1313);
}

// On an odd frame the dot the counter reaches from dot 339 of scanline 261 makes the read
// dot 340 would have made, the second half of the dropped nametable fetch, and dot 0's
// address is not driven (NESdev wiki, "PPU rendering", Pre-render scanline: the jump
// replaces the idle tick at the beginning of the first visible scanline with the last
// tick of the last dummy nametable fetch).
TEST(PpuBackground, TheDotAnOddFrameSkipsToMakesTheReadOfDot340) {
  const auto toDot339 = [](Chip& chip, bool odd) {
    chip.state.odd = odd;
    chip.state.v = 0x0005;
    chip.state.nametableLatch = 0x99;
    chip.at(261, 338);
    chip.tick();  // dot 339
    EXPECT_EQ(chip.state.addressPins, 0x2005);
    chip.state.v = 0x0105;
  };
  Chip even;
  toDot339(even, false);
  EXPECT_EQ(even.tick(), PpuDotEvent::None);  // dot 340
  EXPECT_EQ(even.state.addressPins, 0x2105);
  EXPECT_EQ(even.tick(), PpuDotEvent::FrameEnded);  // dot 0 of scanline 0
  EXPECT_EQ(even.state.addressPins, 0x0990);
  EXPECT_EQ(even.state.addressLatch, 0x90);

  Chip odd;
  toDot339(odd, true);
  EXPECT_EQ(odd.tick(), PpuDotEvent::FrameEnded);
  EXPECT_EQ(odd.state.dot, 0);
  EXPECT_EQ(odd.state.scanline, 0);
  EXPECT_EQ(odd.state.addressPins, 0x2105);
  EXPECT_EQ(odd.state.addressLatch, 0x05);
  EXPECT_EQ(odd.state.nametableLatch, 0x99);
}

// A read is made at the high six lines as the chip drives them on the read's dot, over
// the low eight the latch captured on the dot before: `v` changed between the two dots
// moves the high six and not the low eight (NESdev wiki, "PPU rendering", its notes:
// reads come from an address composed of the latched low 8 bits and the current high 6
// bits).
TEST(PpuBackground, AReadIsAtTheHighSixLinesOverTheLatch) {
  Chip chip;
  chip.state.ciram[0x005] = 0x11;
  chip.state.ciram[0x006] = 0x33;
  chip.state.ciram[0x105] = 0x22;
  chip.state.v = 0x0005;
  chip.at(10, 0);
  chip.tick();  // dot 1
  EXPECT_EQ(chip.state.addressPins, 0x2005);
  chip.state.v = 0x0105;
  chip.tick();  // dot 2
  EXPECT_EQ(chip.state.addressPins, 0x2105);
  EXPECT_EQ(chip.state.addressLatch, 0x05);
  EXPECT_EQ(chip.state.nametableLatch, 0x22);

  chip.state.v = 0x0005;
  chip.at(10, 0);
  chip.tick();  // dot 1
  chip.state.v = 0x0006;
  chip.tick();  // dot 2
  EXPECT_EQ(chip.state.addressPins, 0x2005);
  EXPECT_EQ(chip.state.nametableLatch, 0x11);
}

// `v`'s move on a dot falls before the dot's read: the high six lines of a read on a dot
// of the pre-render line's vertical copy are the address's as the copy leaves `v`, over
// the low eight the latch captured the dot before (NESdev wiki, "PPU rendering", Cycles
// 257-320: the first garbage nametable fetch has its upper six bits already reloaded; its
// notes: reads come from the current high 6 bits).
TEST(PpuBackground, VsMoveOnADotComesBeforeItsRead) {
  Chip chip;
  chip.state.ciram[0x005] = 0x11;
  chip.state.ciram[0x405] = 0x22;
  chip.state.v = 0x0005;
  chip.state.t = 0x0005;
  chip.at(261, 280);
  chip.tick();  // dot 281
  EXPECT_EQ(chip.state.addressPins, 0x2005);
  chip.state.t = 0x0805;
  chip.tick();  // dot 282
  EXPECT_EQ(chip.state.v, 0x0805);
  EXPECT_EQ(chip.state.addressPins, 0x2805);
  EXPECT_EQ(chip.state.nametableLatch, 0x22);
}

// The background runs on scanlines 0-239 and 261: dot 1 of each drives a nametable
// address, and dot 1 of the post-render line and of the vertical blank's lines makes no
// access, the lines carrying `v` (NESdev wiki, "PPU rendering", Line-by-line timing and
// PPU address bus contents).
TEST(PpuBackground, TheBackgroundRunsOnScanlines0To239And261) {
  const auto driven = [](std::uint16_t scanline) {
    Chip chip;
    chip.state.v = 0x0005;
    chip.at(scanline, 0);
    chip.tick();
    return chip.state.addressPins;
  };
  EXPECT_EQ(driven(0), 0x2005);
  EXPECT_EQ(driven(239), 0x2005);
  EXPECT_EQ(driven(261), 0x2005);
  EXPECT_EQ(driven(240), 0x0005);
  EXPECT_EQ(driven(241), 0x0005);
  EXPECT_EQ(driven(260), 0x0005);
}

// `v` steps its coarse X on every eighth dot from 8 to 256 and on dots 328 and 336, and
// on no other: coarse X counts to 31 and its overflow switches the horizontal nametable
// (NESdev wiki, "PPU scrolling", Between dot 328 of a scanline, and 256 of the next
// scanline, and Coarse X increment).
TEST(PpuBackground, CoarseXStepsOnEveryEighthDot) {
  Chip chip;
  chip.state.v = 0x001E;
  chip.at(10, 0);
  chip.tickTo(7);
  EXPECT_EQ(chip.state.v, 0x001E);
  chip.tick();  // dot 8
  EXPECT_EQ(chip.state.v, 0x001F);
  chip.tickTo(15);
  EXPECT_EQ(chip.state.v, 0x001F);
  chip.tick();  // dot 16
  EXPECT_EQ(chip.state.v, 0x0400);
  chip.tickTo(24);
  EXPECT_EQ(chip.state.v, 0x0401);

  chip.state.v = 0x041E;
  chip.state.t = 0x041E;
  chip.at(10, 257);
  chip.tickTo(327);
  EXPECT_EQ(chip.state.v, 0x041E);
  chip.tick();  // dot 328
  EXPECT_EQ(chip.state.v, 0x041F);
  chip.tickTo(335);
  EXPECT_EQ(chip.state.v, 0x041F);
  chip.tick();  // dot 336
  EXPECT_EQ(chip.state.v, 0x0000);
  chip.tickTo(340);
  EXPECT_EQ(chip.state.v, 0x0000);
}

// `v` steps its Y on dot 256: fine Y counts to 7 and overflows into coarse Y, which goes
// from 29 to 0 switching the vertical nametable and from 31 to 0 without switching it;
// the same dot steps coarse X (NESdev wiki, "PPU scrolling", At dot 256 of each scanline
// and Y increment).
TEST(PpuBackground, YStepsOnDot256) {
  const auto stepped = [](std::uint16_t v) {
    Chip chip;
    chip.state.v = v;
    chip.at(10, 254);
    chip.tick();  // dot 255
    EXPECT_EQ(chip.state.v, v);
    chip.tick();  // dot 256
    return chip.state.v;
  };
  EXPECT_EQ(stepped(0x1005), 0x2006);
  EXPECT_EQ(stepped(0x70A5), 0x00C6);
  EXPECT_EQ(stepped(0x73A5), 0x0806);
  EXPECT_EQ(stepped(0x73C5), 0x03E6);
  EXPECT_EQ(stepped(0x73E5), 0x0006);
  EXPECT_EQ(stepped(0x7BA5), 0x0006);
}

// On dot 257 `v` takes `t`'s horizontal bits, coarse X and bit 10, and keeps the rest
// (NESdev wiki, "PPU scrolling", At dot 257 of each scanline).
TEST(PpuBackground, VTakesTsHorizontalBitsOnDot257) {
  const auto copied = [](std::uint16_t v, std::uint16_t t) {
    Chip chip;
    chip.state.v = v;
    chip.state.t = t;
    chip.at(10, 256);
    chip.tick();
    return chip.state.v;
  };
  EXPECT_EQ(copied(0x0000, 0x7FFF), 0x041F);
  EXPECT_EQ(copied(0x7FFF, 0x0000), 0x7BE0);
  EXPECT_EQ(copied(0x2ABC, 0x1543), 0x2EA3);
}

// On each of dots 280-304 of the pre-render line `v` takes `t`'s vertical bits, fine Y,
// coarse Y and bit 11; a picture line's dots 280-304 copy nothing (NESdev wiki, "PPU
// scrolling", During dots 280 to 304 of the pre-render scanline).
TEST(PpuBackground, ThePreRenderLineTakesTsVerticalBitsOnDots280To304) {
  Chip chip;
  chip.state.v = 0x041F;
  chip.state.t = 0x7FFF;
  chip.at(261, 278);
  chip.tick();  // dot 279
  EXPECT_EQ(chip.state.v, 0x041F);
  chip.tick();  // dot 280
  EXPECT_EQ(chip.state.v, 0x7FFF);
  chip.tickTo(303);
  chip.state.t = 0x0000;
  chip.tick();  // dot 304
  EXPECT_EQ(chip.state.v, 0x041F);
  chip.state.t = 0x7FFF;
  chip.tick();  // dot 305
  EXPECT_EQ(chip.state.v, 0x041F);

  chip.state.v = 0x041F;
  chip.at(10, 278);
  while (chip.state.dot != 306) {
    chip.tick();
    EXPECT_EQ(chip.state.v, 0x041F) << "dot " << chip.state.dot;
  }
}

// The shift registers shift on every dot from 2 to 257, each bit a place toward bit 0:
// the low pattern register takes a 0 at bit 15, the high one a 1, and each attribute
// register its 1-bit latch at bit 7. On dot 9 and every eighth dot after it, after that
// dot's shift, the pattern latches enter the high eight bits, the tile's leftmost pixel
// at bit 8, and the attribute latch fills the 1-bit latches (NESdev wiki, "PPU
// rendering", Picture region and its notes, and Cycles 1-256: the shifters shift for the
// first time on cycle 2 and are reloaded during ticks 9, 17, 25, ..., 257).
TEST(PpuBackground, TheShiftRegistersShiftFromDot2AndReloadOnDot9) {
  Chip chip;
  chip.state.ciram[0x000] = 0x4E;
  chip.state.ciram[0x3C0] = 0x02;
  chip.chrRam[0x04E0] = 0x41;
  chip.chrRam[0x04E8] = 0x16;
  chip.state.patternShift = {0x1234, 0x00F0};
  chip.state.attributeShift = {0xFF, 0x00};
  chip.state.attributeFeed = 1;
  chip.at(10, 0);
  chip.tick();  // dot 1
  EXPECT_EQ(chip.state.patternShift[0], 0x1234);
  EXPECT_EQ(chip.state.patternShift[1], 0x00F0);
  chip.tick();  // dot 2
  EXPECT_EQ(chip.state.patternShift[0], 0x091A);
  EXPECT_EQ(chip.state.patternShift[1], 0x8078);
  EXPECT_EQ(chip.state.attributeShift[0], 0xFF);
  EXPECT_EQ(chip.state.attributeShift[1], 0x00);
  chip.tickTo(8);
  EXPECT_EQ(chip.state.patternShift[0], 0x0024);
  EXPECT_EQ(chip.state.patternShift[1], 0xFE01);
  EXPECT_EQ(chip.state.attributeFeed, 1);
  chip.tick();  // dot 9
  EXPECT_EQ(chip.state.patternShift[0], 0x8212);
  EXPECT_EQ(chip.state.patternShift[1], 0x6800);
  EXPECT_EQ(chip.state.attributeFeed, 2);
  EXPECT_EQ(chip.state.attributeShift[0], 0xFF);
  EXPECT_EQ(chip.state.attributeShift[1], 0x00);
  chip.tick();  // dot 10
  EXPECT_EQ(chip.state.patternShift[0], 0x4109);
  EXPECT_EQ(chip.state.patternShift[1], 0xB400);
  EXPECT_EQ(chip.state.attributeShift[0], 0x7F);
  EXPECT_EQ(chip.state.attributeShift[1], 0x80);
  chip.tickTo(257);
  const auto pattern = chip.state.patternShift;
  const auto attribute = chip.state.attributeShift;
  chip.tickTo(321);
  EXPECT_EQ(chip.state.patternShift, pattern);
  EXPECT_EQ(chip.state.attributeShift, attribute);
}

// Dots 322-337 shift, and dots 329 and 337 reload: the next line's first two tiles stand
// in the registers when it begins, the first in the low eight bits and the second in the
// high eight, the first tile's palette through the attribute registers and the second's
// in the 1-bit latches (NESdev wiki, "PPU rendering", Cycles 321-336, and Cycles 1-256:
// at the beginning of each scanline the data for the first two tiles is already loaded
// into the shift registers).
TEST(PpuBackground, TheNextLinesFirstTwoTilesAreLoadedOnDots329And337) {
  Chip chip;
  chip.state.v = 0x0001;
  chip.state.ciram[0x001] = 0x4E;
  chip.state.ciram[0x002] = 0x4F;
  chip.state.ciram[0x3C0] = 0x09;
  chip.chrRam[0x04E0] = 0x41;
  chip.chrRam[0x04E8] = 0x16;
  chip.chrRam[0x04F0] = 0xF0;
  chip.chrRam[0x04F8] = 0x0F;
  chip.at(10, 320);
  chip.tickTo(328);
  EXPECT_EQ(chip.state.patternShift[0], 0x0000);
  EXPECT_EQ(chip.state.patternShift[1], 0xFE00);
  chip.tick();  // dot 329
  EXPECT_EQ(chip.state.patternShift[0], 0x8200);
  EXPECT_EQ(chip.state.patternShift[1], 0x6800);
  EXPECT_EQ(chip.state.attributeFeed, 1);
  chip.tickTo(337);
  EXPECT_EQ(chip.state.patternShift[0], 0x0F82);
  EXPECT_EQ(chip.state.patternShift[1], 0xF068);
  EXPECT_EQ(chip.state.attributeShift[0], 0xFF);
  EXPECT_EQ(chip.state.attributeShift[1], 0x00);
  EXPECT_EQ(chip.state.attributeFeed, 2);
  EXPECT_EQ(chip.state.v, 0x0003);
  const PpuState loaded = chip.state;
  chip.tickTo(340);
  chip.tick();  // dot 0 of scanline 11
  chip.tick();  // dot 1
  EXPECT_EQ(chip.state.scanline, 11);
  EXPECT_EQ(chip.state.patternShift, loaded.patternShift);
  EXPECT_EQ(chip.state.attributeShift, loaded.attributeShift);
  EXPECT_EQ(chip.state.attributeFeed, loaded.attributeFeed);
}

// A position's pixel is selected two dots behind the counter and reaches the picture four
// behind it: position 0 is selected on dot 2, before that dot's shift, and stored on dot
// 4 (NESdev wiki, "PPU rendering", Cycles 1-256: sprite 0 hit acts as if the image starts
// at cycle 2, the cycle the shifters first shift on, and the first pixel is output during
// cycle 4).
TEST(PpuBackground, APixelIsSelectedTwoDotsBehindAndDrawnFourBehind) {
  Chip chip;
  chip.at(10, 1);
  chip.state.patternShift[0] = 0x0001;
  chip.tick();  // dot 2
  EXPECT_EQ(chip.state.pixelPipeline[0], 1);
  EXPECT_EQ(chip.state.pixelPipeline[1], 0);
  EXPECT_EQ(chip.state.patternShift[0], 0);
  EXPECT_EQ(chip.dotAt(0, 10), kBlank);
  chip.tick();  // dot 3
  EXPECT_EQ(chip.state.pixelPipeline[0], 0);
  EXPECT_EQ(chip.state.pixelPipeline[1], 1);
  EXPECT_EQ(chip.dotAt(0, 10), kBlank);
  chip.tick();  // dot 4
  EXPECT_EQ(chip.dotAt(0, 10), kColour[0][0]);
  EXPECT_EQ(chip.state.pixelPipeline[1], 0);
  chip.tick();  // dot 5
  EXPECT_EQ(chip.dotAt(1, 10), kBackdrop);
}

// The fine X scroll picks the bit of each register a pixel is read from, bit `x`: a pixel
// standing at bit 7 is position 7 with a fine X of 0 and position 0 with a fine X of 7
// (NESdev wiki, "PPU rendering", Picture region; "PPU scrolling", PPU internal
// registers).
TEST(PpuBackground, TheFineXScrollPicksTheBit) {
  for (std::uint8_t x = 0; x < 8; ++x) {
    Chip chip;
    chip.state.x = x;
    chip.at(10, 1);
    chip.state.patternShift[0] = 0x0080;
    chip.tickTo(13);
    for (std::size_t position = 0; position < 8; ++position) {
      const std::uint16_t expected = position == 7u - x ? kColour[0][0] : kBackdrop;
      EXPECT_EQ(chip.dotAt(position, 10), expected) << "fine X " << int{x} << ", position "
                                                    << position;
    }
  }
}

// A pixel is the palette's two bits over the pattern's two, an address into palette RAM;
// a pattern of 0 is transparent and shows the backdrop whatever its palette (NESdev wiki,
// "PPU rendering", Drawing overview; "PPU palettes", Palette RAM).
TEST(PpuBackground, APixelIsThePalettesBitsOverThePatterns) {
  const auto drawn = [](std::uint16_t low, std::uint16_t high, std::uint8_t paletteLow,
                        std::uint8_t paletteHigh) {
    Chip chip;
    chip.at(10, 1);
    chip.state.patternShift = {low, high};
    chip.state.attributeShift = {paletteLow, paletteHigh};
    chip.tickTo(4);
    return chip.dotAt(0, 10);
  };
  EXPECT_EQ(drawn(1, 0, 0, 0), kColour[0][0]);
  EXPECT_EQ(drawn(0, 1, 0, 0), kColour[0][1]);
  EXPECT_EQ(drawn(1, 1, 0, 0), kColour[0][2]);
  EXPECT_EQ(drawn(1, 1, 1, 0), kColour[1][2]);
  EXPECT_EQ(drawn(0, 1, 0, 1), kColour[2][1]);
  EXPECT_EQ(drawn(1, 0, 1, 1), kColour[3][0]);
  EXPECT_EQ(drawn(0, 0, 1, 1), kBackdrop);
}

// With $2001 bit 3 clear the background is transparent, the chip fetching and shifting
// all the same for the sprites' sake; with bit 1 clear it is transparent in the picture's
// eight leftmost positions (NESdev wiki, "PPU registers", PPUMASK, Rendering control).
TEST(PpuBackground, TheBackgroundHiddenIsTransparent) {
  const auto run = [](Chip& chip, std::uint8_t mask) {
    chip.state.mask = mask;
    chip.at(10, 1);
    chip.state.patternShift[0] = 0xFFFF;
    chip.tickTo(24);
  };
  Chip shown;
  run(shown, 0x0A);
  Chip noLeftColumn;
  run(noLeftColumn, 0x08);
  Chip hidden;
  run(hidden, 0x10);
  for (std::size_t position = 0; position < 20; ++position) {
    const bool pixel = position < 16;
    EXPECT_EQ(shown.dotAt(position, 10), pixel ? kColour[0][0] : kBackdrop) << position;
    EXPECT_EQ(noLeftColumn.dotAt(position, 10),
              pixel && position >= 8 ? kColour[0][0] : kBackdrop)
        << position;
    EXPECT_EQ(hidden.dotAt(position, 10), kBackdrop) << position;
  }
  for (const Chip* other : {&noLeftColumn, &hidden}) {
    EXPECT_EQ(other->state.patternShift[0], shown.state.patternShift[0]);
    EXPECT_EQ(other->state.v, shown.state.v);
    EXPECT_EQ(other->state.addressPins, shown.state.addressPins);
  }
}

// The pre-render line fetches, shifts and moves `v` as a picture line does and selects no
// pixel (NESdev wiki, "PPU rendering", Pre-render scanline).
TEST(PpuBackground, ThePreRenderLineSelectsNoPixel) {
  Chip chip;
  chip.at(261, 1);
  chip.state.patternShift[0] = 0xFFFF;
  for (int dot = 2; dot <= 12; ++dot) {
    chip.tick();
    EXPECT_EQ(chip.state.pixelPipeline[0], 0) << "dot " << dot;
  }
  EXPECT_EQ(chip.state.patternShift[0], 0x001F);
  EXPECT_EQ(chip.state.v, 0x0001);
  EXPECT_EQ(chip.state.addressPins, 0x23C0);
}

// With rendering disabled nothing of the background moves: no access is made, no register
// shifts, and `v` keeps its value; the pixels on their way to the picture give way to
// transparent ones (NESdev wiki, "PPU registers", PPUMASK, Rendering control: when both
// components are disabled the rendering process stops).
TEST(PpuBackground, WithRenderingDisabledNothingOfTheBackgroundMoves) {
  for (const std::uint16_t scanline : {std::uint16_t{10}, std::uint16_t{261}}) {
    Chip chip;
    chip.state.mask = 0;
    chip.state.v = 0x1234;
    chip.state.t = 0x4321;
    chip.state.nametableLatch = 0x11;
    chip.state.attributeLatch = 2;
    chip.state.patternLatch = {0x33, 0x44};
    chip.state.patternShift = {0x5566, 0x7788};
    chip.state.attributeShift = {0x99, 0xAA};
    chip.state.attributeFeed = 1;
    chip.state.pixelPipeline = {5, 6};
    chip.state.addressPins = 0x2BCD;
    chip.state.addressLatch = 0xCD;
    chip.at(scanline, 0);
    const PpuState before = chip.state;
    chip.tick();
    EXPECT_EQ(chip.state.pixelPipeline[0], 0) << scanline;
    EXPECT_EQ(chip.state.pixelPipeline[1], 5) << scanline;
    chip.tickTo(340);
    EXPECT_EQ(chip.state.v, before.v) << scanline;
    EXPECT_EQ(chip.state.t, before.t) << scanline;
    EXPECT_EQ(chip.state.nametableLatch, before.nametableLatch) << scanline;
    EXPECT_EQ(chip.state.attributeLatch, before.attributeLatch) << scanline;
    EXPECT_EQ(chip.state.patternLatch, before.patternLatch) << scanline;
    EXPECT_EQ(chip.state.patternShift, before.patternShift) << scanline;
    EXPECT_EQ(chip.state.attributeShift, before.attributeShift) << scanline;
    EXPECT_EQ(chip.state.attributeFeed, before.attributeFeed) << scanline;
    EXPECT_EQ(chip.state.addressPins, before.v & 0x3FFFu) << scanline;  // the lines carry v
    EXPECT_EQ(chip.state.addressLatch, before.addressLatch) << scanline;
    EXPECT_EQ(chip.state.pixelPipeline[0], 0) << scanline;
    EXPECT_EQ(chip.state.pixelPipeline[1], 0) << scanline;
  }
}

// A write to $2001 reaches the background as it reaches the odd-frame skip, on the third
// dot after its access: rendering enabled two dots before a step of coarse X is not read
// by it and enabled three dots before is, and rendering disabled likewise (NESdev wiki,
// "PPU registers", PPUMASK, Rendering control: toggling rendering takes effect
// approximately 3-4 dots after the write; blargg's ppu_vbl_nmi, 10-even_odd_timing, for
// the count).
TEST(PpuBackground, AWriteTo2001ReachesTheBackgroundOnItsThirdDot) {
  const auto vAfter = [](std::uint8_t before, std::uint8_t value, std::uint16_t writtenAt) {
    Chip chip;
    chip.state.mask = before;
    chip.at(10, writtenAt);
    chip.write(1, value);
    chip.tickTo(104);
    return chip.state.v;
  };
  EXPECT_EQ(vAfter(0x00, 0x08, 101), 0x0001);
  EXPECT_EQ(vAfter(0x00, 0x08, 102), 0x0000);
  EXPECT_EQ(vAfter(0x08, 0x00, 101), 0x0000);
  EXPECT_EQ(vAfter(0x08, 0x00, 102), 0x0001);
}

// ---- The machine ---------------------------------------------------------------------

// A tile shows where the nametable puts it: tile 3 at column 3 of row 2 covers positions
// 24-31 of lines 16-23, and the backdrop is everywhere a pixel is transparent. The
// picture's scroll is `t`'s, whatever `v` held before the pre-render line (NESdev wiki,
// "PPU nametables"; "PPU scrolling", During dots 280 to 304 of the pre-render scanline).
TEST(PpuMachineBackground, ATileShowsWhereTheNametablePutsIt) {
  const auto dots = pictureOf(imageWith(kJmpLoop), [](PpuState& ppu) {
    ppu.ciram[2 * 32 + 3] = 3;
    ppu.v = 0x7FFF;
  });
  expectPicture(dots, [](std::size_t x, std::size_t y) -> std::uint16_t {
    return x >= 24 && x <= 31 && y >= 16 && y <= 23 ? kColour[0][2] : kBackdrop;
  });
}

// An attribute byte gives a palette to each quarter of its sixteen tiles, two tiles by
// two: bits 1-0 the top left, 3-2 the top right, 5-4 the bottom left, 7-6 the bottom
// right (NESdev wiki, "PPU attribute tables").
TEST(PpuMachineBackground, AnAttributeByteColoursItsFourQuarters) {
  const auto dots = pictureOf(imageWith(kJmpLoop), [](PpuState& ppu) {
    for (std::size_t row = 0; row < 4; ++row) {
      for (std::size_t column = 0; column < 4; ++column) ppu.ciram[row * 32 + column] = 1;
    }
    ppu.ciram[0x3C0] = 0xE4;
  });
  expectPicture(dots, [](std::size_t x, std::size_t y) -> std::uint16_t {
    if (x >= 32 || y >= 32) return kBackdrop;
    return kColour[(y / 16) * 2 + x / 16][0];
  });
}

// The fine X scroll slides the background left a position for each count, and coarse X a
// tile: a tile at column 1 covers positions 8 - x to 15 - x, and positions 0-7 with a
// coarse X of 1 (NESdev wiki, "PPU scrolling", $2005 first write).
TEST(PpuMachineBackground, EachFineXSlidesTheBackgroundAPosition) {
  const Bytes image = imageWith(kJmpLoop);
  for (std::uint8_t fineX = 0; fineX < 8; ++fineX) {
    const auto dots = pictureOf(image, [fineX](PpuState& ppu) {
      ppu.ciram[1] = 3;
      ppu.x = fineX;
    });
    expectPicture(dots, [fineX](std::size_t x, std::size_t y) -> std::uint16_t {
      return x + fineX >= 8 && x + fineX <= 15 && y < 8 ? kColour[0][2] : kBackdrop;
    });
  }
  const auto coarse = pictureOf(image, [](PpuState& ppu) {
    ppu.ciram[1] = 3;
    ppu.t = 0x0001;
  });
  expectPicture(coarse, [](std::size_t x, std::size_t y) -> std::uint16_t {
    return x < 8 && y < 8 ? kColour[0][2] : kBackdrop;
  });
}

// The picture's first line is the row of the nametable `t`'s Y scroll names: tile 4, its
// top row alone drawn, at row 1 shows on line 8 - y with a fine Y of y, and on line 0
// with a coarse Y of 1 (NESdev wiki, "PPU scrolling", $2005 second write; "PPU pattern
// tables", Addressing).
TEST(PpuMachineBackground, EachFineYStartsThePictureARowLower) {
  const Bytes image = imageWith(kJmpLoop);
  for (std::uint16_t fineY = 0; fineY < 8; ++fineY) {
    const auto dots = pictureOf(image, [fineY](PpuState& ppu) {
      ppu.ciram[32] = 4;
      ppu.t = static_cast<std::uint16_t>(fineY << 12);
    });
    expectPicture(dots, [fineY](std::size_t x, std::size_t y) -> std::uint16_t {
      return x < 8 && y == 8u - fineY ? kColour[0][0] : kBackdrop;
    });
  }
  const auto coarse = pictureOf(image, [](PpuState& ppu) {
    ppu.ciram[32] = 4;
    ppu.t = 0x0020;
  });
  expectPicture(coarse, [](std::size_t x, std::size_t y) -> std::uint16_t {
    return x < 8 && y == 0 ? kColour[0][0] : kBackdrop;
  });
}

// A scroll crosses both nametable seams: past column 31 the line goes on in the nametable
// beside, and past row 29 the picture goes on in the nametable below (NESdev wiki, "PPU
// scrolling", Coarse X increment and Y increment).
TEST(PpuMachineBackground, AScrollCrossesBothNametableSeams) {
  const auto across = pictureOf(imageWith(kJmpLoop, true), [](PpuState& ppu) {
    ppu.ciram[0x01F] = 1;
    ppu.ciram[0x400] = 2;
    ppu.t = 0x001F;
  });
  expectPicture(across, [](std::size_t x, std::size_t y) -> std::uint16_t {
    if (y >= 8 || x >= 16) return kBackdrop;
    return x < 8 ? kColour[0][0] : kColour[0][1];
  });
  const auto down = pictureOf(imageWith(kJmpLoop), [](PpuState& ppu) {
    ppu.ciram[29 * 32] = 1;
    ppu.ciram[0x400] = 2;
    ppu.t = 0x03A0;
  });
  expectPicture(down, [](std::size_t x, std::size_t y) -> std::uint16_t {
    if (x >= 8 || y >= 16) return kBackdrop;
    return y < 8 ? kColour[0][0] : kColour[0][1];
  });
}

// A coarse Y of 30 or 31 reads the attribute table's bytes as tiles, and stepping from 31
// goes to row 0 of the same nametable: the byte at $23C0 is the tile at column 0 of row
// 30, coloured by the attribute byte its own address selects, $23F8, and sixteen lines
// down the picture is at row 0 of $2000, where that same byte at $23C0 is the attribute
// (NESdev wiki, "PPU scrolling", Y increment).
TEST(PpuMachineBackground, CoarseYPast29ReadsAttributesAsTilesAndKeepsTheNametable) {
  const auto dots = pictureOf(imageWith(kJmpLoop), [](PpuState& ppu) {
    ppu.ciram[0x3C0] = 1;
    ppu.ciram[0x000] = 2;
    ppu.ciram[0x400] = 3;
    ppu.t = 0x03C0;
  });
  expectPicture(dots, [](std::size_t x, std::size_t y) -> std::uint16_t {
    if (x >= 8) return kBackdrop;
    if (y < 8) return kColour[0][0];
    if (y >= 16 && y < 24) return kColour[1][1];
    return kBackdrop;
  });
}

// With $2001 bit 1 clear the background is hidden in the picture's eight leftmost
// positions, which show the backdrop (NESdev wiki, "PPU registers", PPUMASK, Rendering
// control).
TEST(PpuMachineBackground, TheLeftColumnIsHiddenWithoutBit1Of2001) {
  const Bytes image = imageWith(kJmpLoop);
  const auto rowOfThrees = [](PpuState& ppu) {
    for (std::size_t column = 0; column < 32; ++column) ppu.ciram[column] = 3;
  };
  const auto hidden = pictureOf(image, [&rowOfThrees](PpuState& ppu) {
    rowOfThrees(ppu);
    ppu.mask = 0x08;
  });
  expectPicture(hidden, [](std::size_t x, std::size_t y) -> std::uint16_t {
    return y < 8 && x >= 8 ? kColour[0][2] : kBackdrop;
  });
  const auto shown = pictureOf(image, rowOfThrees);
  expectPicture(shown, [](std::size_t, std::size_t y) -> std::uint16_t {
    return y < 8 ? kColour[0][2] : kBackdrop;
  });
}

// $2000 bit 4 selects the pattern table the background is drawn from: tile 1 is every
// pixel 1 in the table at $0000 and every pixel 2 in the one at $1000 (NESdev wiki, "PPU
// pattern tables", Addressing).
TEST(PpuMachineBackground, Bit4Of2000SelectsTheBackgroundsPatternTable) {
  const Bytes image = imageWith(kJmpLoop);
  const auto low = pictureOf(image, [](PpuState& ppu) { ppu.ciram[0] = 1; });
  expectPicture(low, [](std::size_t x, std::size_t y) -> std::uint16_t {
    return x < 8 && y < 8 ? kColour[0][0] : kBackdrop;
  });
  const auto high = pictureOf(image, [](PpuState& ppu) {
    ppu.ciram[0] = 1;
    ppu.ctrl = 0x10;
  });
  expectPicture(high, [](std::size_t x, std::size_t y) -> std::uint16_t {
    return x < 8 && y < 8 ? kColour[0][1] : kBackdrop;
  });
}

// With the sprites enabled alone rendering runs and the background is transparent: every
// position is the backdrop, and `v` moves as it does with the background shown (NESdev
// wiki, "PPU registers", PPUMASK, Rendering control).
TEST(PpuMachineBackground, WithTheSpritesAloneEveryPositionIsTheBackdrop) {
  const Bytes image = imageWith(kJmpLoop);
  const auto everyTileThree = [](PpuState& ppu) {
    for (std::size_t tile = 0; tile < 960; ++tile) ppu.ciram[tile] = 3;
  };
  const auto dots = pictureOf(image, [&everyTileThree](PpuState& ppu) {
    everyTileThree(ppu);
    ppu.mask = 0x10;
  });
  expectPicture(dots, [](std::size_t, std::size_t) -> std::uint16_t { return kBackdrop; });
  Nes sprites = prepared(image, [&everyTileThree](PpuState& ppu) {
    everyTileThree(ppu);
    ppu.mask = 0x10;
  });
  Nes background = prepared(image, everyTileThree);
  sprites.run(kFrame);
  background.run(kFrame);
  EXPECT_EQ(sprites.state().ppu.v, background.state().ppu.v);
  EXPECT_NE(sprites.state().ppu.v, 0);
}

// A program draws through the ports: it waits two vertical blanks, writes a backdrop and
// a colour to palette RAM and a tile to the nametable through $2006 and $2007, sets the
// scroll through $2000 and $2005, and enables the background through $2001; the picture
// after shows the tile where the program put it.
TEST(PpuMachineBackground, AProgramDrawsThroughThePorts) {
  // BIT $2002; BPL -5; BIT $2002; BPL -5; LDA #$3F; STA $2006; LDA #$00; STA $2006;
  // LDA #$0F; STA $2007; LDA #$21; STA $2007; LDA #$20; STA $2006; LDA #$21; STA $2006;
  // LDA #$01; STA $2007; LDA #$00; STA $2000; STA $2005; STA $2005; LDA #$0A; STA $2001;
  // JMP self
  const Bytes image = imageWith(
      {0x2C, 0x02, 0x20, 0x10, 0xFB, 0x2C, 0x02, 0x20, 0x10, 0xFB, 0xA9, 0x3F, 0x8D, 0x06,
       0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20, 0xA9, 0x0F, 0x8D, 0x07, 0x20, 0xA9, 0x21, 0x8D,
       0x07, 0x20, 0xA9, 0x20, 0x8D, 0x06, 0x20, 0xA9, 0x21, 0x8D, 0x06, 0x20, 0xA9, 0x01,
       0x8D, 0x07, 0x20, 0xA9, 0x00, 0x8D, 0x00, 0x20, 0x8D, 0x05, 0x20, 0x8D, 0x05, 0x20,
       0xA9, 0x0A, 0x8D, 0x01, 0x20, 0x4C, 0x3D, 0x80});
  Nes machine(NesConfig{.rom = image, .vblankAtPowerOn = false});
  PictureKeeper keeper;
  machine.setFrameObserver(&keeper);
  machine.run(4 * kFrame);
  machine.setFrameObserver(nullptr);
  EXPECT_EQ(keeper.pictures, 4u);
  EXPECT_EQ(machine.state().ppu.mask, 0x0A);
  expectPicture(keeper.dots, [](std::size_t x, std::size_t y) -> std::uint16_t {
    return x >= 8 && x <= 15 && y >= 8 && y <= 15 ? 0x21 : 0x0F;
  });
}

// A machine nobody watches draws exactly as a watched one, and a machine restored from
// another's state inside a picture goes on exactly as it does: equal states after every
// frame, and equal pictures after the restore.
TEST(PpuMachineBackground, WatchedUnwatchedAndRestoredMachinesRunTheSame) {
  const Bytes image = imageWith(kJmpLoop, true);
  const auto edit = [](PpuState& ppu) {
    for (std::size_t tile = 0; tile < ppu.ciram.size(); ++tile) {
      ppu.ciram[tile] = static_cast<std::uint8_t>(tile % 5);
    }
    ppu.t = 0x2ABC;
    ppu.x = 3;
  };
  Nes watched = prepared(image, edit);
  Nes alone = prepared(image, edit);
  PictureKeeper keeper;
  watched.setFrameObserver(&keeper);
  for (int round = 0; round < 4; ++round) {
    watched.run(kFrame);
    stepCycles(alone, 10000);
    alone.run(kFrame - 120000);
    EXPECT_TRUE(watched.state() == alone.state()) << "round " << round;
  }
  EXPECT_EQ(keeper.pictures, 3u);

  stepCycles(watched, 5000);
  alone.restore(watched.state());
  PictureKeeper second;
  alone.setFrameObserver(&second);
  watched.setFrameObserver(&keeper);
  watched.run(2 * kFrame);
  alone.run(2 * kFrame);
  watched.setFrameObserver(nullptr);
  alone.setFrameObserver(nullptr);
  EXPECT_TRUE(watched.state() == alone.state());
  ASSERT_EQ(keeper.dots.size(), kPictureDots);
  EXPECT_EQ(second.dots, keeper.dots);
  bool drawn = false;
  for (const std::uint16_t dot : keeper.dots) drawn = drawn || dot != kBackdrop;
  EXPECT_TRUE(drawn);
}

}  // namespace
