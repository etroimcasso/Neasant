#include "neasant/nes/board.h"
#include "neasant/nes/cartridge.h"
#include "neasant/nes/nes.h"
#include "neasant/nes/video_frame.h"
#include "neasant/ppu/ppu.h"
#include "authored_image.h"
#include "rom_harness.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifndef NEASANT_NES_TEST_ROMS
#define NEASANT_NES_TEST_ROMS ""
#endif

// The sprites. On a bare chip: secondary OAM cleared on dots 1-64; the evaluation of a
// line dot by dot, the sprites in range copied in OAM's order, the first found marked as
// sprite 0, the height from $2000, a Y below the line or at $FF never in range, the ninth
// sprite setting the overflow flag on its dot, the walk the search takes diagonally
// through the bytes after it and its end at sprite 63, the OAM address as the
// evaluation's pointer and held at 0 on dots 257-320, the 2C02G's eight-byte copy when
// evaluation begins with the address at 8 or more; the output units loaded from secondary
// OAM on dots 257-320, the pattern address of an 8x8 and an 8x16 sprite with each flip, a
// slot without a sprite for the line loaded transparent, and every slot so on the
// pre-render line; the units counting down and shifting out eight pixels, the frontmost
// opaque unit winning, the priority multiplexer, sprite 0 hit on the selection's dot and
// the conditions it needs, the two flags cleared at dot 1 of the pre-render line and
// answered by $2002, $2004 answering the OAM bus while the chip renders, and rendering
// disabled moving nothing. On the machine: a sprite where OAM puts it a line low, the
// flips, an 8x16 sprite, the sprites' order and priority against the background, the left
// column, nine on a line, no sprite on the first line, the hit's cycle, a program placing
// a sprite through the ports, and two machines equal watched and unwatched. Then blargg's
// sprite_hit_tests and sprite_overflow_tests, each run to its report. A case that checks
// a hardware rule names the document and section it is read from.

namespace {

using neasant::Board;
using neasant::boardFor;
using neasant::Cartridge;
using neasant::kPictureDots;
using neasant::kPictureWidth;
using neasant::makeCartridge;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::PpuDotEvent;
using neasant::PpuInputs;
using neasant::ppuPeekRegister;
using neasant::ppuRead;
using neasant::PpuState;
using neasant::ppuTickDot;
using neasant::ppuWrite;
using neasant::SpriteUnit;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::chrAt;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::loadRom;
using neasant::nes_tests::PictureKeeper;
using neasant::nes_tests::placeProgram;
using neasant::nes_tests::Report;
using neasant::nes_tests::RomResult;
using neasant::nes_tests::runUntilResult;

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
// entry 4p + c of palette RAM holds kColour[p][c - 1] for the background's four palettes
// and kSpriteColour[p][c - 1] for the sprites'.
constexpr std::uint8_t kBackdrop = 0x0F;
constexpr std::array<std::array<std::uint8_t, 3>, 4> kColour{{
    {0x01, 0x02, 0x03},
    {0x15, 0x16, 0x17},
    {0x29, 0x2A, 0x2B},
    {0x31, 0x32, 0x33},
}};
constexpr std::array<std::array<std::uint8_t, 3>, 4> kSpriteColour{{
    {0x04, 0x05, 0x06},
    {0x18, 0x19, 0x1A},
    {0x24, 0x25, 0x26},
    {0x34, 0x35, 0x36},
}};

// Fills palette RAM: the backdrop, the four background palettes and the four sprite
// palettes.
void setColours(PpuState& state) {
  state.palette[0] = kBackdrop;
  for (std::size_t p = 0; p < 4; ++p) {
    for (std::size_t c = 1; c <= 3; ++c) {
      state.palette[4 * p + c] = kColour[p][c - 1];
      state.palette[0x10 + 4 * p + c] = kSpriteColour[p][c - 1];
    }
  }
}

// Puts sprite `n`'s four bytes into OAM.
void setSprite(std::array<std::uint8_t, 256>& oam, std::size_t n, std::uint8_t y,
               std::uint8_t tile, std::uint8_t attribute, std::uint8_t x) {
  oam[4 * n] = y;
  oam[4 * n + 1] = tile;
  oam[4 * n + 2] = attribute;
  oam[4 * n + 3] = x;
}

// Puts a sprite's four bytes into slot `slot` of secondary OAM.
void setSlot(PpuState& state, std::size_t slot, std::uint8_t y, std::uint8_t tile,
             std::uint8_t attribute, std::uint8_t x) {
  state.secondaryOam[4 * slot] = y;
  state.secondaryOam[4 * slot + 1] = tile;
  state.secondaryOam[4 * slot + 2] = attribute;
  state.secondaryOam[4 * slot + 3] = x;
}

// A bare chip over an NROM board with 16 KB of PRG ROM and 8 KB of CHR RAM, which a case
// writes its patterns into; its warm-up window closed as it is after the first pre-render
// line, the background and the sprites shown with their left columns, every sprite in OAM
// hidden at Y = $F8, holding a picture the chip writes into, every entry blank until it
// does.
struct Chip {
  Bytes image;
  Cartridge cartridge;
  Board board;
  std::vector<std::uint8_t> chrRam;
  std::vector<std::uint16_t> picture = std::vector<std::uint16_t>(kPictureDots, kBlank);
  PpuState state;
  PpuInputs inputs;

  Chip()
      : image(authoredImage(inesHeader({.chrUnits = 0}), 16384, 0)),
        cartridge(makeCartridge(image)),
        board(boardFor(cartridge)),
        chrRam(cartridge.chrRamBytes),
        inputs{board, cartridge, chrRam, 115200u, picture} {
    state.warmingUp = false;
    state.mask = 0x1E;
    state.oam.fill(0xF8);
    setColours(state);
  }

  void write(std::uint8_t port, std::uint8_t value) { ppuWrite(state, port, value, inputs); }
  std::uint8_t read(std::uint8_t port) { return ppuRead(state, port, inputs); }
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

// The tiles the machine's cases draw with, in the pattern table at $0000: tile 1 every
// pixel 1, tile 2 every pixel 2, tile 3 every pixel 3, tile 4 its top row alone pixel 1,
// tile 5 its top left pixel alone pixel 1, and tile $FF, the tile a slot no sprite fills
// names, every pixel 1; and in the table at $1000 tile 1 every pixel 2. A tile is sixteen
// bytes, a row's low bits in the first eight and its high bits in the second (NESdev
// wiki, "PPU pattern tables").
void placePatterns(Bytes& image) {
  for (std::size_t i = 0; i < 8; ++i) {
    chrAt(image, 16384, 0x0010 + i) = 0xFF;
    chrAt(image, 16384, 0x0028 + i) = 0xFF;
    chrAt(image, 16384, 0x0030 + i) = 0xFF;
    chrAt(image, 16384, 0x0038 + i) = 0xFF;
    chrAt(image, 16384, 0x1018 + i) = 0xFF;
    chrAt(image, 16384, 0x0FF0 + i) = 0xFF;
  }
  chrAt(image, 16384, 0x0040) = 0xFF;
  chrAt(image, 16384, 0x0050) = 0x80;
}

// A 16 KB NROM image with the patterns in 8 KB of CHR ROM, `program` at $8000 and every
// vector pointing there, under horizontal mirroring.
Bytes imageWith(Program program) {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  placeProgram(image, 0, std::span<const std::uint8_t>(program.begin(), program.size()));
  placePatterns(image);
  return image;
}

// Steps `cycles` machine cycles.
void stepCycles(Nes& machine, int cycles) {
  for (int i = 0; i < cycles; ++i) machine.step();
}

// A machine over `image` with the flag clear at power-on, stepped through its reset
// sequence, its PPU then given the colours, the warm-up window closed, the background and
// the sprites shown with their left columns and every sprite hidden at Y = $F8, changed
// further by `edit`, and restored.
template <typename Edit>
Nes prepared(const Bytes& image, Edit edit) {
  Nes machine(NesConfig{.rom = image, .vblankAtPowerOn = false});
  stepCycles(machine, 7);
  NesState state = machine.state();
  state.ppu.warmingUp = false;
  state.ppu.mask = 0x1E;
  state.ppu.oam.fill(0xF8);
  setColours(state.ppu);
  edit(state.ppu);
  machine.restore(state);
  return machine;
}

// The picture the machine draws of what `edit` set up: the second frame's, the first one
// begun after the set-up.
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

// Whether `x`, `y` is inside the eight-by-eight square whose top left is `left`, `top`.
bool inSquare(std::size_t x, std::size_t y, std::size_t left, std::size_t top) {
  return x >= left && x < left + 8 && y >= top && y < top + 8;
}

// The guard, in frames: 60 seconds at the console's rate.
constexpr std::uint64_t kGuardFrames = 3600;

std::string romsDir() { return NEASANT_NES_TEST_ROMS; }

bool romsRequired() {
  const char* raw = std::getenv("NEASANT_REQUIRE_NES_TEST_ROMS");
  return raw != nullptr && std::string(raw) == "1";
}

// Loads the file into `image`, or leaves it empty after skipping or failing the case:
// the case skips naming the option when it is unset and the file when it is absent, and
// either is a failure instead when the ROMs are required.
void loadOrSkip(const std::string& relative, std::optional<Bytes>& image) {
  image.reset();
  if (romsDir().empty()) {
    if (romsRequired()) {
      ADD_FAILURE() << relative
                    << ": NEASANT_NES_TEST_ROMS is unset and the ROMs are required";
      return;
    }
    GTEST_SKIP() << relative
                 << ": set NEASANT_NES_TEST_ROMS to a clone of nes-test-roms to run it";
  }
  const std::string path = romsDir() + "/" + relative;
  image = loadRom(path);
  if (!image) {
    if (romsRequired()) {
      ADD_FAILURE() << path << " is absent and the ROMs are required";
      return;
    }
    GTEST_SKIP() << path << " is absent";
  }
}

// Boots the ROM on a machine built with the defaults and runs it to its report on the
// screen, requiring a pass.
void expectPass(const std::string& relative) {
  std::optional<Bytes> image;
  loadOrSkip(relative, image);
  if (!image) return;
  Nes machine(NesConfig{.rom = *image});
  const RomResult result = runUntilResult(machine, Report::Screen, kGuardFrames);
  EXPECT_TRUE(result.reported) << relative << " did not report within " << kGuardFrames
                               << " frames\n--- text ---\n"
                               << result.text << "\n------------";
  if (!result.reported) return;
  EXPECT_TRUE(result.passed) << relative << " reports a failure after " << result.frames
                             << " frames\n--- text ---\n"
                             << result.text << "\n------------";
}

// ---- The bare chip -----------------------------------------------------------------

// Dots 1-64 of a picture line clear secondary OAM to $FF: each odd dot reads $FF onto the
// OAM bus and each even dot writes it to the next byte, and dot 1 clears the flag that
// marks sprite 0 for the next line (NESdev wiki, "PPU sprite evaluation", Details; "PPU
// OAM", Internal operation).
TEST(PpuSprites, SecondaryOamIsClearedOnDots1To64) {
  Chip chip;
  chip.state.secondaryOam.fill(0x55);
  chip.state.sprite0Next = true;
  chip.state.oamBus = 0x12;
  chip.at(10, 0);
  chip.tick();  // dot 1
  EXPECT_EQ(chip.state.oamBus, 0xFF);
  EXPECT_FALSE(chip.state.sprite0Next);
  EXPECT_EQ(chip.state.secondaryOam[0], 0x55);
  chip.tick();  // dot 2
  EXPECT_EQ(chip.state.secondaryOam[0], 0xFF);
  EXPECT_EQ(chip.state.secondaryOam[1], 0x55);
  chip.tickTo(63);
  EXPECT_EQ(chip.state.secondaryOam[30], 0xFF);
  EXPECT_EQ(chip.state.secondaryOam[31], 0x55);
  chip.tick();  // dot 64
  EXPECT_EQ(chip.state.secondaryOam[31], 0xFF);
  EXPECT_EQ(chip.state.oamBus, 0xFF);
}

// From dot 65 the evaluation reads OAM at the OAM address on each odd dot and acts on
// each even one: a Y in range is copied to the next free byte of secondary OAM and the
// sprite's three other bytes follow it, two dots each; a Y out of range is written to the
// free byte too and the pointer moves to the next sprite (NESdev wiki, "PPU sprite
// evaluation", Details, steps 1 to 2).
TEST(PpuSprites, TheEvaluationReadsOnOddDotsAndCopiesOnEven) {
  Chip chip;
  setSprite(chip.state.oam, 0, 10, 0x4E, 0xFF, 0x42);
  setSprite(chip.state.oam, 1, 0x80, 0x11, 0x22, 0x33);
  chip.at(10, 0);
  chip.tickTo(64);
  EXPECT_EQ(chip.state.oamAddr, 0);
  chip.tick();  // dot 65
  EXPECT_EQ(chip.state.oamBus, 10);
  EXPECT_EQ(chip.state.secondaryOam[0], 0xFF);
  EXPECT_EQ(chip.state.oamAddr, 0);
  chip.tick();  // dot 66
  EXPECT_EQ(chip.state.secondaryOam[0], 10);
  EXPECT_EQ(chip.state.secondaryAddress, 1);
  EXPECT_EQ(chip.state.copyLeft, 3);
  EXPECT_EQ(chip.state.oamAddr, 1);
  chip.tick();  // dot 67
  EXPECT_EQ(chip.state.oamBus, 0x4E);
  chip.tick();  // dot 68
  EXPECT_EQ(chip.state.secondaryOam[1], 0x4E);
  chip.tick();  // dot 69
  EXPECT_EQ(chip.state.oamBus, 0xE3);  // byte 2 as its five bits
  chip.tick();  // dot 70
  EXPECT_EQ(chip.state.secondaryOam[2], 0xE3);
  chip.tickTo(72);
  EXPECT_EQ(chip.state.secondaryOam[3], 0x42);
  EXPECT_EQ(chip.state.secondaryAddress, 4);
  EXPECT_EQ(chip.state.copyLeft, 0);
  EXPECT_EQ(chip.state.oamAddr, 4);
  chip.tick();  // dot 73
  EXPECT_EQ(chip.state.oamBus, 0x80);
  chip.tick();  // dot 74: written though out of range
  EXPECT_EQ(chip.state.secondaryOam[4], 0x80);
  EXPECT_EQ(chip.state.secondaryAddress, 4);
  EXPECT_EQ(chip.state.oamAddr, 8);
}

// The sprites in range are copied in OAM's order, eight at most; the free byte after the
// last one holds the last Y looked at, sprite 63's, and the bytes after it $FF (NESdev
// wiki, "PPU sprite evaluation", Details: the first empty slot holds sprite 63's Y and
// then three $FF bytes).
TEST(PpuSprites, TheSpritesInRangeAreCopiedInOrder) {
  Chip chip;
  setSprite(chip.state.oam, 3, 10, 0x11, 0x01, 0x21);
  setSprite(chip.state.oam, 5, 5, 0x22, 0x02, 0x32);
  setSprite(chip.state.oam, 7, 200, 0x33, 0x03, 0x43);
  setSprite(chip.state.oam, 63, 0xF0, 0x44, 0x04, 0x54);
  chip.at(10, 0);
  chip.tickTo(256);
  // Sprites 3 and 5 in range, rows 0 and 5; sprite 7 out; sprite 63's Y in the free byte.
  constexpr std::array<std::uint8_t, 12> kExpected{10,   0x11, 0x01, 0x21, 5,    0x22,
                                                   0x02, 0x32, 0xF0, 0xFF, 0xFF, 0xFF};
  for (std::size_t i = 0; i < kExpected.size(); ++i) {
    EXPECT_EQ(chip.state.secondaryOam[i], kExpected[i]) << "byte " << i;
  }
  EXPECT_EQ(chip.state.secondaryAddress, 8);
  EXPECT_TRUE(chip.state.evaluationDone);
  EXPECT_FALSE(chip.state.spriteOverflow);
}

// A sprite is in range when the line less its Y is one of its rows, 0-7, or 0-15 with
// $2000 bit 5 set; a sprite below the line, or one at Y = $FF, is never in range, and one
// at Y = 239 is on scanline 239 (NESdev wiki, "PPU sprite evaluation", Details; "PPU
// OAM", Byte 0 - Y position; "PPU registers", PPUCTRL).
TEST(PpuSprites, TheHeightAndTheLineDecideTheRange) {
  // Whether scanline `scanline`'s evaluation finds sprite 0 at `y` with $2000 at `ctrl`.
  const auto found = [](std::uint16_t scanline, std::uint8_t y, std::uint8_t ctrl) {
    Chip chip;
    chip.state.ctrl = ctrl;
    chip.state.oam[0] = y;
    chip.at(scanline, 64);
    chip.tickTo(256);
    return chip.state.secondaryAddress == 4;
  };
  EXPECT_TRUE(found(10, 10, 0));     // row 0
  EXPECT_TRUE(found(17, 10, 0));     // row 7
  EXPECT_FALSE(found(18, 10, 0));    // row 8
  EXPECT_TRUE(found(25, 10, 0x20));  // row 15
  EXPECT_FALSE(found(26, 10, 0x20));
  EXPECT_FALSE(found(9, 10, 0));  // below the line
  EXPECT_FALSE(found(0, 0xFF, 0));
  EXPECT_FALSE(found(6, 0xFF, 0));
  EXPECT_TRUE(found(239, 239, 0));
  EXPECT_FALSE(found(239, 240, 0));
}

// The ninth sprite in range sets the overflow flag, on the even dot its Y is acted on:
// eight sprites take 64 dots and the ninth's Y is read on dot 129 and acted on at dot
// 130. Its three other bytes are then read and the evaluation is done; nothing more is
// written to secondary OAM (NESdev wiki, "PPU sprite evaluation", Details, steps 2c and
// 3a; "PPU registers", PPUSTATUS, Sprite overflow flag).
TEST(PpuSprites, TheNinthSpriteSetsTheOverflowFlagOnItsDot) {
  Chip chip;
  for (std::size_t n = 0; n < 9; ++n) setSprite(chip.state.oam, n, 10, 1, 0, 0);
  chip.at(10, 0);
  chip.tickTo(129);
  EXPECT_FALSE(chip.state.spriteOverflow);
  EXPECT_EQ(chip.state.secondaryAddress, 32);
  EXPECT_EQ(chip.state.oamAddr, 32);  // eight sprites, four pairs each, dots 66-128
  chip.tick();                        // dot 130
  EXPECT_TRUE(chip.state.spriteOverflow);
  EXPECT_EQ(chip.state.copyLeft, 3);
  EXPECT_EQ(chip.state.oamAddr, 33);
  chip.tickTo(136);  // the three bytes read on dots 132, 134 and 136
  EXPECT_EQ(chip.state.oamAddr, 36);
  EXPECT_TRUE(chip.state.evaluationDone);
  chip.tickTo(256);
  EXPECT_EQ(chip.state.secondaryOam[31], 0);  // nothing written after the eighth
  EXPECT_EQ(chip.state.oamAddr, 20);  // sixty even dots from 138 to 256, four each, wrapping
}

// Once eight are found and the next sprite's Y is out of range, the search steps to the
// next sprite and the next byte at once, so it reads the second byte of the sprite after,
// the third of the one after that, and so on round the four, and a byte in range by that
// reading sets the flag. A byte 2 is read as its five bits, so its bits 4-2 cannot put it
// in range and its bit 5 can keep it out (NESdev wiki, "PPU sprite evaluation", Sprite
// overflow bug; "PPU OAM", Byte 2 - Attributes; blargg's sprite_overflow_tests,
// 4.Obscure).
TEST(PpuSprites, TheSearchAfterEightReadsTheBytesDiagonally) {
  // Sprites 0-7 on scanline 3, byte `byte` of sprite `sprite` set to `value`: whether the
  // line's evaluation sets the flag. Sprite 8's Y, $F8, is out of range, and the search
  // steps to sprite 9's byte 1, then 10's byte 2, 11's byte 3, 12's byte 0, 13's byte 1.
  const auto overflows = [](std::size_t sprite, std::size_t byte, std::uint8_t value) {
    Chip chip;
    for (std::size_t n = 0; n < 8; ++n) chip.state.oam[4 * n] = 3;
    chip.state.oam[4 * sprite + byte] = value;
    chip.at(3, 0);
    chip.tickTo(256);
    return chip.state.spriteOverflow;
  };
  EXPECT_FALSE(overflows(9, 0, 3));  // never read
  EXPECT_TRUE(overflows(9, 1, 3));
  EXPECT_TRUE(overflows(10, 2, 3));
  EXPECT_TRUE(overflows(10, 2, 0x1C));   // read as 0, row 3
  EXPECT_FALSE(overflows(10, 2, 0x20));  // 32, below the line
  EXPECT_TRUE(overflows(11, 3, 3));
  EXPECT_TRUE(overflows(12, 0, 3));
  EXPECT_TRUE(overflows(13, 1, 3));
  EXPECT_FALSE(overflows(13, 0, 3));
}

// The search ends at sprite 63 without wrapping into sprite 0's other bytes, and once
// every sprite is looked at the evaluation reads a Y every other dot and copies nothing
// (NESdev wiki, "PPU sprite evaluation", Details, steps 3b and 4; blargg's
// sprite_overflow_tests, 4.Obscure, its seventh test).
TEST(PpuSprites, TheSearchStopsAtSprite63) {
  Chip walked;
  for (std::size_t n = 2; n <= 9; ++n) walked.state.oam[4 * n] = 10;
  setSprite(walked.state.oam, 0, 0xF8, 10, 10, 10);
  walked.at(10, 0);
  walked.tickTo(256);
  EXPECT_FALSE(walked.state.spriteOverflow);
  EXPECT_TRUE(walked.state.evaluationDone);

  Chip plain;
  plain.at(10, 0);
  plain.tickTo(190);
  EXPECT_FALSE(plain.state.evaluationDone);
  EXPECT_EQ(plain.state.oamAddr, 252);  // sprites 0-62, dots 66-190
  plain.tickTo(192);
  EXPECT_TRUE(plain.state.evaluationDone);
  EXPECT_EQ(plain.state.oamAddr, 0);  // sprite 63 and the wrap
  plain.tickTo(256);
  EXPECT_EQ(plain.state.oamAddr, 128);           // 32 even dots from 194, four each
  EXPECT_EQ(plain.state.secondaryOam[0], 0xF8);  // sprite 63's Y, written on dot 192
  EXPECT_EQ(plain.state.secondaryOam[1], 0xFF);
}

// The first sprite a line's evaluation looks at marks sprite 0 for the next line when it
// is in range, on dot 66; dot 257 copies the mark into the flag for the line being drawn,
// and dot 1 of the next line clears the first again (NESdev wiki, "PPU OAM", Internal
// operation: the two flags).
TEST(PpuSprites, TheSprite0FlagsArePerLine) {
  Chip chip;
  chip.state.oam[0] = 10;
  chip.state.oam[4] = 10;
  chip.at(10, 0);
  chip.tickTo(65);
  EXPECT_FALSE(chip.state.sprite0Next);
  chip.tick();  // dot 66
  EXPECT_TRUE(chip.state.sprite0Next);
  EXPECT_FALSE(chip.state.sprite0Current);
  chip.tickTo(256);
  EXPECT_FALSE(chip.state.sprite0Current);
  chip.tick();  // dot 257
  EXPECT_TRUE(chip.state.sprite0Current);
  chip.tickTo(340);
  chip.tick();  // dot 0 of scanline 11
  chip.tick();  // dot 1
  EXPECT_EQ(chip.state.scanline, 11);
  EXPECT_FALSE(chip.state.sprite0Next);
  EXPECT_TRUE(chip.state.sprite0Current);

  Chip second;
  second.state.oam[4] = 10;
  second.at(10, 0);
  second.tickTo(256);
  EXPECT_FALSE(second.state.sprite0Next);
  EXPECT_EQ(second.state.secondaryAddress, 4);
}

// The OAM address is the evaluation's pointer: the evaluation begins at it, so the sprite
// there is treated as sprite 0, and a 2C02G copies the eight-byte row the address is in
// over OAM's first eight bytes as evaluation begins with the address at 8 or more (NESdev
// wiki, "PPU registers", OAMADDR, Values during rendering and OAMADDR precautions; "PPU
// sprite evaluation", Notes).
TEST(PpuSprites, TheEvaluationBeginsAtTheOamAddress) {
  Chip chip;
  for (std::size_t i = 0; i < 8; ++i) {
    chip.state.oam[0x10 + i] = static_cast<std::uint8_t>(0xA0 + i);
  }
  setSprite(chip.state.oam, 5, 10, 0x55, 0, 0);  // over $14-$17
  chip.state.oamAddr = 0x14;
  chip.at(10, 64);
  chip.tick();  // dot 65
  constexpr std::array<std::uint8_t, 8> kRow{0xA0, 0xA1, 0xA2, 0xA3, 10, 0x55, 0, 0};
  for (std::size_t i = 0; i < kRow.size(); ++i) {
    EXPECT_EQ(chip.state.oam[i], kRow[i]) << "byte " << i;  // the row at $10
  }
  EXPECT_EQ(chip.state.oamBus, 10);  // the byte at $14
  chip.tick();                       // dot 66
  EXPECT_TRUE(chip.state.sprite0Next);  // the sprite at the address is sprite 0
  EXPECT_EQ(chip.state.oamAddr, 0x15);

  Chip low;
  low.state.oam[0] = 0x77;
  low.state.oamAddr = 5;
  low.at(10, 64);
  low.tick();  // dot 65
  EXPECT_EQ(low.state.oam[0], 0x77);
  EXPECT_EQ(low.state.oamBus, 0xF8);
}

// The OAM address is held at 0 on each of dots 257-320 of scanlines 0-239 and 261, and
// left alone on other dots and with rendering disabled (NESdev wiki, "PPU registers",
// OAMADDR, Values during rendering).
TEST(PpuSprites, TheOamAddressIsHeldAt0OnDots257To320) {
  for (const std::uint16_t scanline : {std::uint16_t{10}, std::uint16_t{261}}) {
    Chip chip;
    chip.at(scanline, 256);
    chip.state.oamAddr = 0x77;
    chip.tick();  // dot 257
    EXPECT_EQ(chip.state.oamAddr, 0) << "scanline " << scanline;
    chip.state.oamAddr = 0x77;
    chip.tickTo(320);
    EXPECT_EQ(chip.state.oamAddr, 0) << "scanline " << scanline;
    chip.state.oamAddr = 0x77;
    chip.tick();  // dot 321
    EXPECT_EQ(chip.state.oamAddr, 0x77) << "scanline " << scanline;
  }
  Chip after;
  after.at(240, 256);
  after.state.oamAddr = 0x77;
  after.tickTo(320);
  EXPECT_EQ(after.state.oamAddr, 0x77);

  Chip disabled;
  disabled.state.mask = 0;
  disabled.at(10, 256);
  disabled.state.oamAddr = 0x77;
  disabled.tickTo(320);
  EXPECT_EQ(disabled.state.oamAddr, 0x77);
}

// The pre-render line clears nothing and evaluates nothing: through its dots 1-256
// secondary OAM, the OAM address and the OAM bus keep what they hold, and its dots
// 257-320 load the units from secondary OAM as a picture line's do (NESdev wiki, "PPU
// sprite evaluation", Notes; "PPU rendering", Pre-render scanline).
TEST(PpuSprites, ThePreRenderLineClearsAndEvaluatesNothing) {
  Chip chip;
  chip.state.secondaryOam.fill(0x5A);
  chip.state.oamAddr = 0x44;
  chip.state.oamBus = 0x33;
  chip.at(261, 0);
  chip.tickTo(256);
  for (std::size_t i = 0; i < chip.state.secondaryOam.size(); ++i) {
    EXPECT_EQ(chip.state.secondaryOam[i], 0x5A) << "byte " << i;
  }
  EXPECT_EQ(chip.state.oamAddr, 0x44);
  EXPECT_EQ(chip.state.oamBus, 0x33);
  chip.tickTo(260);
  EXPECT_EQ(chip.state.oamAddr, 0);
  EXPECT_EQ(chip.state.oamBus, 0x5A);
  EXPECT_EQ(chip.state.spriteUnits[0].x, 0x5A);
}

// On dots 257-320 each eight reads its slot of secondary OAM onto the OAM bus, the four
// bytes and then the X four times; the attribute enters the unit on the third dot and the
// X its counter on the fourth; the fifth to eighth fetch the row through the board, the
// low byte then the high, into the unit's registers with the leftmost pixel at bit 0
// (NESdev wiki, "PPU sprite evaluation", Details, Cycles 257-320; "PPU rendering", Cycles
// 257-320; "PPU pattern tables", Addressing).
TEST(PpuSprites, TheUnitsLoadFromSecondaryOamOnDots257To320) {
  Chip chip;
  setSlot(chip.state, 0, 7, 0x4E, 0x03, 0x42);  // row 10 - 7 = 3 on scanline 10
  setSlot(chip.state, 1, 0xF8, 0x11, 0x22, 0x33);
  chip.chrRam[0x04E3] = 0x41;
  chip.chrRam[0x04EB] = 0x16;
  chip.state.spriteUnits[0].x = 0x99;
  chip.at(10, 256);
  chip.tick();  // dot 257
  EXPECT_EQ(chip.state.oamBus, 7);
  chip.tick();  // dot 258
  EXPECT_EQ(chip.state.oamBus, 0x4E);
  chip.tick();  // dot 259
  EXPECT_EQ(chip.state.oamBus, 0x03);
  EXPECT_EQ(chip.state.spriteUnits[0].attribute, 0x03);
  EXPECT_EQ(chip.state.spriteUnits[0].x, 0x98);  // stepped once by dot 257's position 255
  chip.tick();                                   // dot 260
  EXPECT_EQ(chip.state.oamBus, 0x42);
  EXPECT_EQ(chip.state.spriteUnits[0].x, 0x42);
  chip.tick();  // dot 261
  EXPECT_EQ(chip.state.oamBus, 0x42);
  EXPECT_EQ(chip.state.addressPins, 0x04E3);
  EXPECT_EQ(chip.state.addressLatch, 0xE3);
  chip.tick();  // dot 262
  EXPECT_EQ(chip.state.spriteUnits[0].pattern[0], 0x82);  // $41 reversed
  chip.tick();  // dot 263
  EXPECT_EQ(chip.state.addressPins, 0x04EB);
  chip.tick();  // dot 264
  EXPECT_EQ(chip.state.oamBus, 0x42);
  EXPECT_EQ(chip.state.spriteUnits[0].pattern[1], 0x68);  // $16 reversed
  chip.tick();  // dot 265
  EXPECT_EQ(chip.state.oamBus, 0xF8);
  chip.tickTo(268);
  EXPECT_EQ(chip.state.spriteUnits[1].attribute, 0x22);
  EXPECT_EQ(chip.state.spriteUnits[1].x, 0x33);
}

// A horizontally flipped sprite loads its row as read, the rightmost pixel first; one
// flipped vertically takes its rows from the bottom (NESdev wiki, "PPU OAM", Byte 2 -
// Attributes; Brad Taylor's NTSC 2C02 technical reference, Details of object pattern
// fetch & render).
TEST(PpuSprites, TheFlipsReverseTheRowAndTheRows) {
  // The low byte's address on dot 261 of scanline 10 and the unit's low register after
  // dot 264, for slot 0 at Y 7, row 3, with `attribute`.
  const auto loaded = [](std::uint8_t attribute) {
    Chip chip;
    setSlot(chip.state, 0, 7, 0x4E, attribute, 0);
    chip.chrRam[0x04E3] = 0x41;
    chip.chrRam[0x04E4] = 0x1C;
    chip.at(10, 256);
    chip.tickTo(261);
    const std::uint16_t address = chip.state.addressPins;
    chip.tickTo(264);
    return std::pair<std::uint16_t, std::uint8_t>{address, chip.state.spriteUnits[0].pattern[0]};
  };
  EXPECT_EQ(loaded(0x00), (std::pair<std::uint16_t, std::uint8_t>{0x04E3, 0x82}));
  EXPECT_EQ(loaded(0x40), (std::pair<std::uint16_t, std::uint8_t>{0x04E3, 0x41}));
  // Row 3 ^ 7 = 4, $1C reversed.
  EXPECT_EQ(loaded(0x80), (std::pair<std::uint16_t, std::uint8_t>{0x04E4, 0x38}));
  EXPECT_EQ(loaded(0xC0), (std::pair<std::uint16_t, std::uint8_t>{0x04E4, 0x1C}));
}

// $2000 bit 3 selects the half of the pattern table 8x8 sprites are read from (NESdev
// wiki, "PPU pattern tables", Addressing; "PPU registers", PPUCTRL).
TEST(PpuSprites, Bit3Of2000SelectsTheSpritesPatternTable) {
  Chip chip;
  chip.state.ctrl = 0x08;
  setSlot(chip.state, 0, 7, 0x4E, 0, 0);
  chip.chrRam[0x14E3] = 0x81;
  chip.at(10, 256);
  chip.tickTo(261);
  EXPECT_EQ(chip.state.addressPins, 0x14E3);
  chip.tickTo(264);
  EXPECT_EQ(chip.state.spriteUnits[0].pattern[0], 0x81);  // $81 reversed is $81
}

// An 8x16 sprite is two tiles, the even one on top and the one after it below, in the
// half of the pattern table bit 0 of its tile byte selects; flipped vertically, the odd
// tile is on top and each is read from the bottom (NESdev wiki, "PPU OAM", Byte 1 -
// Tile/index and Byte 2 - Attributes; "PPU pattern tables", Addressing).
TEST(PpuSprites, An8x16SpriteTakesItsRowsFromTwoTiles) {
  // The address on dot `dot` of `scanline` for an 8x16 sprite at Y 7 in slot 0.
  const auto address = [](std::uint16_t scanline, std::uint8_t tile, std::uint8_t attribute,
                          std::uint16_t dot = 261) {
    Chip chip;
    chip.state.ctrl = 0x20;
    setSlot(chip.state, 0, 7, tile, attribute, 0);
    chip.at(scanline, 256);
    chip.tickTo(dot);
    return chip.state.addressPins;
  };
  EXPECT_EQ(address(10, 0x4F, 0), 0x14E3);     // row 3, the even tile $4E in $1000
  EXPECT_EQ(address(18, 0x4F, 0), 0x14F3);     // row 11, tile $4F row 3
  EXPECT_EQ(address(22, 0x4F, 0), 0x14F7);     // row 15
  EXPECT_EQ(address(10, 0x4F, 0x80), 0x14F4);  // row 3 ^ 15 = 12, tile $4F row 4
  EXPECT_EQ(address(22, 0x4F, 0x80), 0x14E0);  // row 0
  EXPECT_EQ(address(10, 0x4E, 0), 0x04E3);     // the even tile's bit 0 clear
  EXPECT_EQ(address(22, 0x4E, 0, 263), 0x04FF);  // tile $4F row 7, the high byte
}

// A slot whose Y is not in range for the line, the $FF of a slot no sprite filled or a
// sprite found for another line, has its address driven, tile $FF's row for the empty
// slot, and its unit loaded with registers of 0 (NESdev wiki, "PPU rendering", Cycles
// 257-320: dummy fetches to tile $FF, the data discarded and the sprites loaded
// transparent).
TEST(PpuSprites, ASlotOutOfRangeLoadsTransparentRegisters) {
  Chip chip;
  std::fill(chip.chrRam.begin(), chip.chrRam.end(), std::uint8_t{0xFF});
  chip.state.secondaryOam.fill(0xFF);
  setSlot(chip.state, 1, 20, 0x11, 0, 0);  // below the line
  setSlot(chip.state, 2, 3, 0x11, 0, 0);   // row 7
  chip.state.spriteUnits[0].pattern = {0x12, 0x34};
  chip.at(10, 256);
  chip.tickTo(261);
  EXPECT_EQ(chip.state.addressPins, 0x0FF4);  // tile $FF, row (10 - 255) & 7 = 3 flipped to 4
  chip.tickTo(264);
  EXPECT_EQ(chip.state.spriteUnits[0].pattern, (std::array<std::uint8_t, 2>{0, 0}));
  EXPECT_EQ(chip.state.spriteUnits[0].x, 0xFF);
  chip.tickTo(272);
  EXPECT_EQ(chip.state.spriteUnits[1].pattern[0], 0);
  chip.tickTo(280);
  EXPECT_EQ(chip.state.spriteUnits[2].pattern[0], 0xFF);
}

// The pre-render line loads every unit from the secondary OAM scanline 239 left, with no
// slot in range: the line counts as -1, so neither a sprite found for line 240 nor the
// $FF of an empty slot has a row there, and no sprite shows on the first line of the
// picture (NESdev wiki, "PPU rendering", Pre-render scanline (-1 or 261); "PPU sprite
// evaluation", Notes; "PPU OAM", Byte 0 - Y position).
TEST(PpuSprites, ThePreRenderLineLoadsEverySlotTransparent) {
  Chip chip;
  std::fill(chip.chrRam.begin(), chip.chrRam.end(), std::uint8_t{0xFF});
  setSlot(chip.state, 0, 232, 0x11, 0, 0);
  setSlot(chip.state, 1, 239, 0x11, 0, 0);
  setSlot(chip.state, 2, 0, 0x11, 0, 0);
  setSlot(chip.state, 3, 0xFF, 0xFF, 0xFF, 0xFF);
  for (SpriteUnit& unit : chip.state.spriteUnits) unit.pattern = {0x12, 0x34};
  chip.at(261, 256);
  chip.tickTo(320);
  for (std::size_t slot = 0; slot < chip.state.spriteUnits.size(); ++slot) {
    EXPECT_EQ(chip.state.spriteUnits[slot].pattern, (std::array<std::uint8_t, 2>{0, 0}))
        << "slot " << slot;
  }

  Chip last;
  std::fill(last.chrRam.begin(), last.chrRam.end(), std::uint8_t{0xFF});
  setSlot(last.state, 0, 232, 0x11, 0, 0);
  last.at(239, 256);
  last.tickTo(264);
  EXPECT_EQ(last.state.spriteUnits[0].pattern[0], 0xFF);  // row 7
}

// A unit counts its X down a position at a time and, at 0, shifts out eight pixels, the
// leftmost first, then nothing: a unit at X = 3 covers positions 3-10 (NESdev wiki, "PPU
// rendering", Picture region; Brad Taylor's NTSC 2C02 technical reference, Details of
// object pattern fetch & render).
TEST(PpuSprites, AUnitCountsDownAndShiftsOutEightPixels) {
  Chip chip;
  chip.state.spriteUnits[0] = SpriteUnit{.pattern = {0xFF, 0x00}, .attribute = 0, .x = 3};
  chip.at(10, 1);
  chip.tick();  // dot 2
  EXPECT_EQ(chip.state.spriteUnits[0].x, 2);
  chip.tickTo(4);
  EXPECT_EQ(chip.state.spriteUnits[0].x, 0);
  EXPECT_EQ(chip.state.spriteUnits[0].pattern[0], 0xFF);
  chip.tick();  // dot 5, position 3
  EXPECT_EQ(chip.state.spriteUnits[0].pattern[0], 0x7F);
  EXPECT_EQ(chip.state.pixelPipeline[0], 0x11);
  chip.tickTo(20);
  for (std::size_t x = 0; x < 16; ++x) {
    const std::uint16_t want = x >= 3 && x <= 10 ? kSpriteColour[0][0] : kBackdrop;
    EXPECT_EQ(chip.dotAt(x, 10), want) << "position " << x;
  }
  EXPECT_EQ(chip.state.spriteUnits[0].pattern[0], 0);
}

// A unit's pixel is its palette over its pattern, a sprite palette's entry, and a pattern
// of 0 is transparent whatever the palette (NESdev wiki, "PPU rendering", Picture region;
// "PPU palettes", Palette RAM).
TEST(PpuSprites, AUnitsPixelIsItsPaletteOverItsPattern) {
  // Position 0 of line 10 drawn from unit 0 at X = 0 with `low` and `high` in the bit
  // each register shifts out first.
  const auto drawn = [](std::uint8_t low, std::uint8_t high, std::uint8_t attribute) {
    Chip chip;
    chip.state.spriteUnits[0] = SpriteUnit{.pattern = {low, high}, .attribute = attribute, .x = 0};
    chip.at(10, 1);
    chip.tickTo(4);
    return chip.dotAt(0, 10);
  };
  EXPECT_EQ(drawn(1, 0, 0), 0x04);
  EXPECT_EQ(drawn(0, 1, 0), 0x05);
  EXPECT_EQ(drawn(1, 1, 0), 0x06);
  EXPECT_EQ(drawn(1, 1, 1), 0x1A);
  EXPECT_EQ(drawn(0, 1, 2), 0x25);
  EXPECT_EQ(drawn(1, 0, 3), 0x34);
  EXPECT_EQ(drawn(0, 0, 3), kBackdrop);
}

// The lowest unit with an opaque pixel wins among the sprites, whatever the units'
// priority bits (NESdev wiki, "PPU sprite priority", Detailed internals of the sprite
// priority quirk; "PPU OAM", Sprite overlapping).
TEST(PpuSprites, TheLowestUnitWithAnOpaquePixelWins) {
  Chip chip;
  chip.state.spriteUnits[0] = SpriteUnit{.pattern = {0xFF, 0x00}, .attribute = 0, .x = 4};
  chip.state.spriteUnits[1] = SpriteUnit{.pattern = {0x00, 0xFF}, .attribute = 1, .x = 0};
  chip.state.spriteUnits[2] = SpriteUnit{.pattern = {0x0F, 0x00}, .attribute = 2, .x = 4};
  chip.at(10, 1);
  chip.tickTo(20);
  for (std::size_t x = 0; x < 16; ++x) {
    std::uint16_t want = kBackdrop;
    if (x <= 3) want = kSpriteColour[1][1];
    if (x >= 4 && x <= 11) want = kSpriteColour[0][0];  // unit 0 over units 1 and 2
    EXPECT_EQ(chip.dotAt(x, 10), want) << "position " << x;
  }
}

// The priority multiplexer: both transparent gives the backdrop, one opaque gives that
// one, both opaque gives the sprite unless its priority bit puts it behind the
// background, and a sprite behind the background still hides the sprites behind it
// (NESdev wiki, "PPU rendering", Picture region, the priority multiplexer decision table;
// "PPU sprite priority").
TEST(PpuSprites, ThePriorityMultiplexer) {
  // Position 0 of line 10 with a background pixel of 1 or none, unit 0's pixel of 2 in
  // palette 0, in front or behind, or none, and unit 1's pixel of 3 in palette 1 or none.
  const auto drawn = [](bool background, bool unit0, bool behind, bool unit1) {
    Chip chip;
    chip.state.patternShift[0] = static_cast<std::uint16_t>(background ? 1 : 0);
    if (unit0) {
      const auto attribute = static_cast<std::uint8_t>(behind ? 0x20 : 0);
      chip.state.spriteUnits[0] = SpriteUnit{.pattern = {0, 1}, .attribute = attribute, .x = 0};
    }
    if (unit1) chip.state.spriteUnits[1] = SpriteUnit{.pattern = {1, 1}, .attribute = 1, .x = 0};
    chip.at(10, 1);
    chip.tickTo(4);
    return chip.dotAt(0, 10);
  };
  EXPECT_EQ(drawn(false, false, false, false), kBackdrop);
  EXPECT_EQ(drawn(false, true, false, false), 0x05);
  EXPECT_EQ(drawn(false, true, true, false), 0x05);
  EXPECT_EQ(drawn(true, false, false, false), 0x01);
  EXPECT_EQ(drawn(true, true, false, false), 0x05);
  EXPECT_EQ(drawn(true, true, true, false), 0x01);
  EXPECT_EQ(drawn(true, true, true, true), 0x01);  // unit 0 wins among the sprites, behind
  EXPECT_EQ(drawn(false, false, false, true), 0x1A);
}

// Sprite 0 hit is set on the dot the chip selects the first position where the first
// unit's pixel and the background's are both opaque with sprite 0 in the unit: position 5
// on dot 7, two dots before the position reaches the picture (NESdev wiki, "PPU OAM",
// Sprite 0 hits; "PPU rendering", Cycles 1-256, and its frame timing diagram's notes:
// setting sprite 0 hit has a 2-dot delay where pixel output has a 4-dot delay).
TEST(PpuSprites, Sprite0HitIsSetOnTheSelectionsDot) {
  Chip chip;
  chip.state.patternShift[0] = 0xFFFF;
  chip.state.spriteUnits[0] = SpriteUnit{.pattern = {1, 0}, .attribute = 0, .x = 5};
  chip.state.sprite0Current = true;
  chip.at(10, 1);
  chip.tickTo(6);
  EXPECT_FALSE(chip.state.sprite0Hit);
  chip.tick();  // dot 7, position 5
  EXPECT_TRUE(chip.state.sprite0Hit);
  EXPECT_EQ(chip.dotAt(5, 10), kBlank);
  chip.tickTo(9);
  EXPECT_EQ(chip.dotAt(5, 10), 0x04);
}

// Sprite 0 hit needs the background's pixel and the first unit's both opaque, by their
// pattern bits alone, with sprite 0 marked for the line and both layers shown; the
// sprite's priority and the palettes do not matter, and another unit's pixel never hits
// (NESdev wiki, "PPU OAM", Sprite 0 hits; "PPU registers", PPUSTATUS, Sprite 0 hit flag;
// blargg's sprite_hit_tests, 01.basics).
TEST(PpuSprites, Sprite0HitNeedsBothLayersAndTheMark) {
  // Whether dots 1-30 of scanline 10 set the flag, with the background's high register
  // at `background`, $2001 at `mask` and unit `unit` covering positions 2-9, inside the
  // register's first eight.
  const auto hits = [](std::uint16_t background, std::uint8_t mask, std::size_t unit,
                       std::uint8_t attribute, bool marked) {
    Chip chip;
    chip.state.mask = mask;
    chip.state.patternShift[1] = background;
    chip.state.spriteUnits[unit] =
        SpriteUnit{.pattern = {0xFF, 0x00}, .attribute = attribute, .x = 2};
    chip.state.sprite0Current = marked;
    chip.at(10, 1);
    chip.tickTo(30);
    return chip.state.sprite0Hit;
  };
  EXPECT_TRUE(hits(0xFFFF, 0x1E, 0, 0, true));
  EXPECT_TRUE(hits(0xFFFF, 0x1E, 0, 0x23, true));  // behind, palette 3
  EXPECT_FALSE(hits(0, 0x1E, 0, 0, true));
  EXPECT_FALSE(hits(0xFFFF, 0x16, 0, 0, true));  // $2001 bit 3 clear
  EXPECT_FALSE(hits(0xFFFF, 0x0E, 0, 0, true));  // $2001 bit 4 clear
  EXPECT_FALSE(hits(0xFFFF, 0x1E, 0, 0, false));
  EXPECT_FALSE(hits(0xFFFF, 0x1E, 1, 0, true));
}

// Sprite 0 hit is never set at position 255, nor in the eight leftmost positions while
// either layer is hidden there, where the hidden layer's pixel is transparent (NESdev
// wiki, "PPU OAM", Sprite 0 hits; "PPU registers", PPUSTATUS, Sprite 0 hit flag).
TEST(PpuSprites, Sprite0HitSkipsPosition255AndTheHiddenColumn) {
  // Whether a line sets the flag with unit 0 at `x` shifting out `pattern`, $2001 at
  // `mask`, and the background opaque at every position.
  const auto hits = [](std::uint8_t x, std::uint8_t pattern, std::uint8_t mask) {
    Chip chip;
    chip.state.mask = mask;
    chip.state.spriteUnits[0] = SpriteUnit{.pattern = {pattern, 0}, .attribute = 0, .x = x};
    chip.state.sprite0Current = true;
    chip.at(10, 1);
    for (int dot = 2; dot <= 257; ++dot) {
      chip.state.patternShift[0] = 0xFFFF;
      chip.tick();
    }
    return chip.state.sprite0Hit;
  };
  EXPECT_TRUE(hits(0, 0xFF, 0x1E));
  EXPECT_TRUE(hits(7, 0xFF, 0x1E));
  EXPECT_FALSE(hits(0, 0xFF, 0x1A));  // the sprites hidden on 0-7
  EXPECT_FALSE(hits(0, 0xFF, 0x1C));  // the background hidden on 0-7
  EXPECT_FALSE(hits(7, 0x01, 0x1A));  // one pixel, at 7
  EXPECT_TRUE(hits(7, 0x02, 0x1A));   // at 8
  EXPECT_TRUE(hits(8, 0x01, 0x18));
  EXPECT_FALSE(hits(255, 0x01, 0x1E));
  EXPECT_FALSE(hits(248, 0x80, 0x1E));  // the pixel at 255
  EXPECT_TRUE(hits(248, 0x40, 0x1E));   // at 254
  EXPECT_TRUE(hits(254, 0x01, 0x1E));
}

// The two flags clear at dot 1 of scanline 261 and at no other dot; a read of $2002
// answers them and leaves them (NESdev wiki, "PPU registers", PPUSTATUS; blargg's
// sprite_overflow_tests, 1.Basics).
TEST(PpuSprites, TheFlagsClearAtDot1OfThePreRenderLine) {
  Chip chip;
  chip.state.sprite0Hit = true;
  chip.state.spriteOverflow = true;
  chip.at(241, 0);
  chip.tick();  // dot 1
  EXPECT_TRUE(chip.state.sprite0Hit);
  EXPECT_TRUE(chip.state.spriteOverflow);
  EXPECT_EQ(chip.read(2) & 0x60, 0x60);
  EXPECT_TRUE(chip.state.sprite0Hit);
  EXPECT_TRUE(chip.state.spriteOverflow);
  chip.state.mask = 0;
  chip.at(261, 0);
  chip.tick();  // dot 1
  EXPECT_FALSE(chip.state.sprite0Hit);
  EXPECT_FALSE(chip.state.spriteOverflow);
}

// $2002 answers the sprite 0 hit flag in bit 6 and the overflow flag in bit 5 over the
// bus's low five bits, drives bits 7-5 of the bus with what it answered, and a peek
// answers the same (NESdev wiki, "PPU registers", PPUSTATUS; "Open bus behavior", PPU
// open bus).
TEST(PpuSprites, TheStatusReadAnswersTheTwoFlags) {
  Chip chip;
  chip.write(3, 0x1F);  // the bus
  chip.state.sprite0Hit = true;
  EXPECT_EQ(ppuPeekRegister(chip.state, 2), 0x5F);
  EXPECT_EQ(chip.read(2), 0x5F);
  EXPECT_EQ(chip.state.ioLatch, 0x5F);
  chip.state.spriteOverflow = true;
  chip.state.vblank = true;
  EXPECT_EQ(ppuPeekRegister(chip.state, 2), 0xFF);
  EXPECT_EQ(chip.read(2), 0xFF);
  EXPECT_FALSE(chip.state.vblank);
  EXPECT_TRUE(chip.state.sprite0Hit);
  EXPECT_TRUE(chip.state.spriteOverflow);
  EXPECT_EQ(chip.read(2), 0x7F);
}

// A read of $2004 while the chip renders answers the byte on the OAM bus: $FF during the
// clear, the byte the evaluation read, and the slot bytes of dots 257-320; with rendering
// disabled, or outside the rendered lines, it answers the byte at the OAM address (NESdev
// wiki, "PPU registers", OAMDATA; "PPU sprite evaluation", Details).
TEST(PpuSprites, A2004ReadWhileRenderingAnswersTheOamBus) {
  Chip chip;
  setSprite(chip.state.oam, 0, 10, 0x4E, 0x03, 0x42);
  chip.state.oam[7] = 0x77;
  chip.state.oamAddr = 7;
  chip.at(10, 0);
  chip.tickTo(30);
  EXPECT_EQ(chip.read(4), 0xFF);
  EXPECT_EQ(ppuPeekRegister(chip.state, 4), 0xFF);
  chip.state.oamAddr = 0;
  chip.tickTo(67);
  EXPECT_EQ(chip.read(4), 0x4E);  // the tile just read
  chip.tickTo(258);
  EXPECT_EQ(chip.read(4), 0x4E);  // slot 0's tile
  chip.tickTo(260);
  EXPECT_EQ(chip.read(4), 0x42);
  chip.tickTo(330);
  EXPECT_EQ(chip.read(4), 10);  // secondary OAM's first byte

  Chip blank;
  blank.state.oam[7] = 0x77;
  blank.state.oamAddr = 7;
  blank.at(240, 100);
  blank.tick();
  EXPECT_EQ(blank.read(4), 0x77);

  Chip disabled;
  disabled.state.oam[7] = 0x77;
  disabled.state.oamAddr = 7;
  disabled.state.mask = 0;
  disabled.at(10, 100);
  disabled.tick();
  EXPECT_EQ(disabled.read(4), 0x77);
  EXPECT_EQ(ppuPeekRegister(disabled.state, 4), 0x77);
}

// With rendering disabled nothing of the sprites moves: secondary OAM, the evaluation's
// place, the OAM bus, the flags, the units and the OAM address keep their values through
// a line (NESdev wiki, "PPU registers", PPUMASK, Rendering control).
TEST(PpuSprites, WithRenderingDisabledNothingOfTheSpritesMoves) {
  Chip chip;
  chip.state.mask = 0;
  chip.state.secondaryOam.fill(0x5A);
  chip.state.secondaryAddress = 9;
  chip.state.oamBus = 0x33;
  chip.state.copyLeft = 2;
  chip.state.evaluationDone = true;
  chip.state.sprite0Next = true;
  chip.state.sprite0Current = true;
  chip.state.oamAddr = 0x44;
  chip.state.spriteUnits[3] = SpriteUnit{.pattern = {0x12, 0x34}, .attribute = 0x56, .x = 7};
  const PpuState before = chip.state;
  chip.at(10, 0);
  chip.tickTo(340);
  EXPECT_EQ(chip.state.secondaryOam, before.secondaryOam);
  EXPECT_EQ(chip.state.secondaryAddress, 9);
  EXPECT_EQ(chip.state.oamBus, 0x33);
  EXPECT_EQ(chip.state.copyLeft, 2);
  EXPECT_TRUE(chip.state.evaluationDone);
  EXPECT_TRUE(chip.state.sprite0Next);
  EXPECT_TRUE(chip.state.sprite0Current);
  EXPECT_EQ(chip.state.oamAddr, 0x44);
  EXPECT_EQ(chip.state.spriteUnits, before.spriteUnits);
  EXPECT_FALSE(chip.state.sprite0Hit);
}

// ---- The machine -------------------------------------------------------------------

// A sprite shows where OAM puts it, one line below its Y: sprite 0 at Y = 20, X = 30 with
// tile 1 covers positions 30-37 of lines 21-28 in its palette's first colour (NESdev
// wiki, "PPU OAM", Byte 0 - Y position and Byte 3 - X position).
TEST(PpuMachineSprites, ASpriteShowsWhereOamPutsItALineLow) {
  const Bytes image = imageWith(kJmpLoop);
  const auto dots =
      pictureOf(image, [](PpuState& ppu) { setSprite(ppu.oam, 0, 20, 1, 0x01, 30); });
  expectPicture(dots, [](std::size_t x, std::size_t y) -> std::uint16_t {
    return inSquare(x, y, 30, 21) ? kSpriteColour[1][0] : kBackdrop;
  });
}

// The flips mirror the tile within its square: tile 5's one pixel, top left, is at the
// top right flipped horizontally, at the bottom left flipped vertically, and at the
// bottom right flipped both ways (NESdev wiki, "PPU OAM", Byte 2 - Attributes).
TEST(PpuMachineSprites, TheFlipsMirrorTheTile) {
  const Bytes image = imageWith(kJmpLoop);
  const auto expectAt = [&image](std::uint8_t attribute, std::size_t px, std::size_t py) {
    const auto dots = pictureOf(
        image, [attribute](PpuState& ppu) { setSprite(ppu.oam, 0, 40, 5, attribute, 60); });
    expectPicture(dots, [px, py](std::size_t x, std::size_t y) -> std::uint16_t {
      return x == px && y == py ? kSpriteColour[0][0] : kBackdrop;
    });
  };
  expectAt(0x00, 60, 41);
  expectAt(0x40, 67, 41);
  expectAt(0x80, 60, 48);
  expectAt(0xC0, 67, 48);
}

// With $2000 bit 5 set a sprite is 8x16: tile byte 2 draws tile 2 on its top eight lines
// and tile 3 on its bottom eight, and flipped vertically tile 3 on top, each tile from
// the bottom (NESdev wiki, "PPU OAM", Byte 1 - Tile/index and Byte 2 - Attributes).
TEST(PpuMachineSprites, An8x16SpriteDrawsTwoTiles) {
  const Bytes image = imageWith(kJmpLoop);
  const auto tall = pictureOf(image, [](PpuState& ppu) {
    ppu.ctrl = 0x20;
    setSprite(ppu.oam, 0, 100, 2, 0, 50);
  });
  expectPicture(tall, [](std::size_t x, std::size_t y) -> std::uint16_t {
    if (inSquare(x, y, 50, 101)) return kSpriteColour[0][1];
    if (inSquare(x, y, 50, 109)) return kSpriteColour[0][2];
    return kBackdrop;
  });
  const auto flipped = pictureOf(image, [](PpuState& ppu) {
    ppu.ctrl = 0x20;
    setSprite(ppu.oam, 0, 100, 2, 0x80, 50);
  });
  expectPicture(flipped, [](std::size_t x, std::size_t y) -> std::uint16_t {
    if (inSquare(x, y, 50, 101)) return kSpriteColour[0][2];
    if (inSquare(x, y, 50, 109)) return kSpriteColour[0][1];
    return kBackdrop;
  });
  const auto small = pictureOf(image, [](PpuState& ppu) { setSprite(ppu.oam, 0, 100, 2, 0, 50); });
  expectPicture(small, [](std::size_t x, std::size_t y) -> std::uint16_t {
    return inSquare(x, y, 50, 101) ? kSpriteColour[0][1] : kBackdrop;
  });
}

// Sprites layer by their OAM index, the lower in front, and a sprite behind the
// background shows only where the background is transparent, hiding the sprites behind it
// even there the background covers it (NESdev wiki, "PPU sprite priority"; "PPU OAM",
// Sprite overlapping).
TEST(PpuMachineSprites, SpritesLayerByIndexAndPriority) {
  const Bytes image = imageWith(kJmpLoop);
  const auto dots = pictureOf(image, [](PpuState& ppu) {
    ppu.ciram[2 * 32 + 5] = 1;  // positions 40-47, lines 16-31
    ppu.ciram[3 * 32 + 5] = 1;
    setSprite(ppu.oam, 0, 19, 2, 0x20, 40);  // behind
    setSprite(ppu.oam, 1, 19, 3, 0x01, 44);
    setSprite(ppu.oam, 2, 39, 2, 0x02, 40);
    setSprite(ppu.oam, 3, 39, 3, 0x03, 40);
  });
  expectPicture(dots, [](std::size_t x, std::size_t y) -> std::uint16_t {
    // Sprite 0 is behind the background in the square at 40, 20 and hides sprite 1
    // where they overlap at 44-47; sprite 1 shows at 48-51; sprite 2 is over sprite 3.
    if (inSquare(x, y, 40, 40)) return kSpriteColour[2][1];
    if (x >= 48 && x <= 51 && y >= 20 && y <= 27) return kSpriteColour[1][2];
    if (x >= 40 && x <= 47 && y >= 16 && y <= 31) return kColour[0][0];
    return kBackdrop;
  });
}

// With $2001 bit 2 clear the sprites are hidden in the picture's eight leftmost
// positions, and the background's bit 1 hides the background alone (NESdev wiki, "PPU
// registers", PPUMASK).
TEST(PpuMachineSprites, TheLeftColumnHidesSpritesWithoutBit2) {
  const Bytes image = imageWith(kJmpLoop);
  const auto hidden = pictureOf(image, [](PpuState& ppu) {
    ppu.mask = 0x1A;
    setSprite(ppu.oam, 0, 10, 1, 0, 4);
  });
  expectPicture(hidden, [](std::size_t x, std::size_t y) -> std::uint16_t {
    return x >= 8 && x <= 11 && y >= 11 && y <= 18 ? kSpriteColour[0][0] : kBackdrop;
  });
  const auto shown = pictureOf(image, [](PpuState& ppu) {
    ppu.mask = 0x1C;
    setSprite(ppu.oam, 0, 10, 1, 0, 4);
  });
  expectPicture(shown, [](std::size_t x, std::size_t y) -> std::uint16_t {
    return inSquare(x, y, 4, 11) ? kSpriteColour[0][0] : kBackdrop;
  });
}

// Nine sprites on one line: the first eight draw and the ninth does not, and the overflow
// flag stands from that line's evaluation until dot 1 of the pre-render line (NESdev
// wiki, "PPU sprite evaluation", Overview; "PPU registers", PPUSTATUS).
TEST(PpuMachineSprites, NineSpritesOnALineDrawEightAndSetTheOverflowFlag) {
  const Bytes image = imageWith(kJmpLoop);
  const auto nine = [](PpuState& ppu) {
    for (std::size_t n = 0; n < 9; ++n) {
      setSprite(ppu.oam, n, 50, 1, 0, static_cast<std::uint8_t>(10 * n));
    }
  };
  const auto dots = pictureOf(image, nine);
  expectPicture(dots, [](std::size_t x, std::size_t y) -> std::uint16_t {
    // Eight sprites with two-position gaps; sprite 8, at 80, is not drawn.
    return y >= 51 && y <= 58 && x < 78 && x % 10 < 8 ? kSpriteColour[0][0] : kBackdrop;
  });
  Nes machine = prepared(image, nine);
  machine.run(kFrame + kFrame / 2);
  EXPECT_TRUE(machine.state().ppu.spriteOverflow);
  EXPECT_FALSE(machine.state().ppu.sprite0Hit);
  machine.run(kFrame / 2);
  // Cleared at dot 1 of scanline 261, and scanline 50 not yet reached.
  EXPECT_FALSE(machine.state().ppu.spriteOverflow);
}

// No sprite shows on the first line of the picture: a sprite at Y = 0 begins on line 1,
// and sprites at Y = 232 to 255, in range for scanline 239's evaluation or none, draw
// nothing at the top, nor does tile $FF from the slots no sprite fills (NESdev wiki, "PPU
// OAM", Byte 0 - Y position; "PPU sprite evaluation", Notes; "PPU rendering", Pre-render
// scanline).
TEST(PpuMachineSprites, NoSpriteShowsOnTheFirstLine) {
  const Bytes image = imageWith(kJmpLoop);
  const auto dots = pictureOf(image, [](PpuState& ppu) {
    setSprite(ppu.oam, 0, 0, 1, 0, 0);
    setSprite(ppu.oam, 1, 232, 1, 0, 20);
    setSprite(ppu.oam, 2, 239, 1, 0, 40);
    setSprite(ppu.oam, 3, 255, 1, 0, 60);
  });
  // Tile $FF is every pixel 1 in this image: line 0 and position 255 stay the backdrop.
  expectPicture(dots, [](std::size_t x, std::size_t y) -> std::uint16_t {
    if (inSquare(x, y, 0, 1)) return kSpriteColour[0][0];
    if (x >= 20 && x <= 27 && y >= 233 && y <= 239) return kSpriteColour[0][0];
    return kBackdrop;
  });
}

// On the machine the hit is set in the cycle that holds the selection's dot: sprite 0
// over a background tile at position 32 of line 21 hits on dot 34 of scanline 21, so the
// flag is clear while the counter stands before that dot and set once it has passed it
// (NESdev wiki, "PPU OAM", Sprite 0 hits; "PPU rendering", Cycles 1-256).
TEST(PpuMachineSprites, TheHitIsSetInTheCycleOfTheSelectionsDot) {
  const Bytes image = imageWith(kJmpLoop);
  Nes machine = prepared(image, [](PpuState& ppu) {
    ppu.ciram[2 * 32 + 4] = 1;  // positions 32-39, lines 16-23
    setSprite(ppu.oam, 0, 20, 1, 0, 32);
  });
  machine.run(kFrame);
  bool reached = false;
  for (int cycle = 0; cycle < 30000 && !reached; ++cycle) {
    const PpuState& ppu = machine.state().ppu;
    const bool before = ppu.scanline < 21 || (ppu.scanline == 21 && ppu.dot < 34);
    if (before) {
      ASSERT_FALSE(ppu.sprite0Hit) << "scanline " << ppu.scanline << ", dot " << ppu.dot;
      machine.step();
      continue;
    }
    reached = true;
    EXPECT_TRUE(ppu.sprite0Hit);
    EXPECT_EQ(ppu.scanline, 21);
    EXPECT_GE(ppu.dot, 34);
    EXPECT_LE(ppu.dot, 36);
  }
  EXPECT_TRUE(reached);
}

// A program places a sprite through the ports: it waits two vertical blanks, writes the
// palette, a sprite's four bytes through $2003 and $2004 and the mask, and the picture
// after shows the sprite where the program put it.
TEST(PpuMachineSprites, AProgramPlacesASpriteThroughThePorts) {
  // BIT $2002; BPL -5; BIT $2002; BPL -5; LDA #$3F; STA $2006; LDA #$00; STA $2006;
  // LDA #$0F; STA $2007; LDA #$3F; STA $2006; LDA #$11; STA $2006; LDA #$21; STA $2007;
  // LDA #$00; STA $2003; LDA #$2F; STA $2004; LDA #$01; STA $2004; LDA #$00; STA $2004;
  // LDA #$50; STA $2004; LDA #$1E; STA $2001; JMP self
  const Bytes image = imageWith(
      {0x2C, 0x02, 0x20, 0x10, 0xFB, 0x2C, 0x02, 0x20, 0x10, 0xFB, 0xA9, 0x3F, 0x8D, 0x06,
       0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20, 0xA9, 0x0F, 0x8D, 0x07, 0x20, 0xA9, 0x3F, 0x8D,
       0x06, 0x20, 0xA9, 0x11, 0x8D, 0x06, 0x20, 0xA9, 0x21, 0x8D, 0x07, 0x20, 0xA9, 0x00,
       0x8D, 0x03, 0x20, 0xA9, 0x2F, 0x8D, 0x04, 0x20, 0xA9, 0x01, 0x8D, 0x04, 0x20, 0xA9,
       0x00, 0x8D, 0x04, 0x20, 0xA9, 0x50, 0x8D, 0x04, 0x20, 0xA9, 0x1E, 0x8D, 0x01, 0x20,
       0x4C, 0x46, 0x80});
  Nes machine(NesConfig{.rom = image, .vblankAtPowerOn = false});
  PictureKeeper keeper;
  machine.setFrameObserver(&keeper);
  machine.run(4 * kFrame);
  machine.setFrameObserver(nullptr);
  EXPECT_EQ(keeper.pictures, 4u);
  EXPECT_EQ(machine.state().ppu.mask, 0x1E);
  // Y $2F plus one.
  expectPicture(keeper.dots, [](std::size_t x, std::size_t y) -> std::uint16_t {
    return inSquare(x, y, 0x50, 0x30) ? 0x21 : 0x0F;
  });
}

// A machine nobody watches evaluates and draws its sprites exactly as a watched one, and
// a machine restored from another's state inside a picture goes on exactly as it does.
TEST(PpuMachineSprites, WatchedUnwatchedAndRestoredMachinesRunTheSame) {
  const Bytes image = imageWith(kJmpLoop);
  const auto edit = [](PpuState& ppu) {
    for (std::size_t tile = 0; tile < 960; ++tile) {
      ppu.ciram[tile] = static_cast<std::uint8_t>(tile % 5);
    }
    for (std::size_t n = 0; n < 64; ++n) {
      const auto attribute = static_cast<std::uint8_t>((n % 4) | ((n & 8) != 0 ? 0x20u : 0u) |
                                                       ((n & 16) != 0 ? 0xC0u : 0u));
      setSprite(ppu.oam, n, static_cast<std::uint8_t>(3 * n),
                static_cast<std::uint8_t>(1 + n % 5), attribute,
                static_cast<std::uint8_t>(7 * n));
    }
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
  bool spriteDrawn = false;
  for (const std::uint16_t dot : keeper.dots) {
    spriteDrawn = spriteDrawn || (dot >= kSpriteColour[0][0] && dot <= kSpriteColour[0][2]);
  }
  EXPECT_TRUE(spriteDrawn);
}

// ---- SpriteHitTests ----

// "Tests basic sprite 0 hit behavior (nothing timing related)" (its readme).
TEST(SpriteHitTests, Rom01Basics) { expectPass("sprite_hit_tests_2005.10.05/01.basics.nes"); }

// "Tests alignment of sprite hit with background" (its readme).
TEST(SpriteHitTests, Rom02Alignment) {
  expectPass("sprite_hit_tests_2005.10.05/02.alignment.nes");
}

// "Tests sprite 0 hit using a sprite with a single pixel set, for each of the four
// corners" (its readme).
TEST(SpriteHitTests, Rom03Corners) { expectPass("sprite_hit_tests_2005.10.05/03.corners.nes"); }

// "Tests sprite 0 hit for single pixel sprite and background" (its readme).
TEST(SpriteHitTests, Rom04Flip) { expectPass("sprite_hit_tests_2005.10.05/04.flip.nes"); }

// "Tests sprite 0 hit with regard to clipping of left 8 pixels of screen" (its readme).
TEST(SpriteHitTests, Rom05LeftClip) {
  expectPass("sprite_hit_tests_2005.10.05/05.left_clip.nes");
}

// "Tests sprite 0 hit with regard to column 255 (ignored) and off right edge of screen"
// (its readme).
TEST(SpriteHitTests, Rom06RightEdge) {
  expectPass("sprite_hit_tests_2005.10.05/06.right_edge.nes");
}

// "Tests sprite 0 hit with regard to bottom of screen" (its readme).
TEST(SpriteHitTests, Rom07ScreenBottom) {
  expectPass("sprite_hit_tests_2005.10.05/07.screen_bottom.nes");
}

// "Tests basic sprite 0 hit double-height operation" (its readme).
TEST(SpriteHitTests, Rom08DoubleHeight) {
  expectPass("sprite_hit_tests_2005.10.05/08.double_height.nes");
}

// "Tests sprite 0 hit timing to within 12 or so PPU clocks" (its readme).
TEST(SpriteHitTests, Rom09TimingBasics) {
  expectPass("sprite_hit_tests_2005.10.05/09.timing_basics.nes");
}

// "Tests sprite 0 hit timing for which pixel it first reports hit on" (its readme).
TEST(SpriteHitTests, Rom10TimingOrder) {
  expectPass("sprite_hit_tests_2005.10.05/10.timing_order.nes");
}

// "Tests sprite 0 hit timing for which pixel it first reports hit on when some pixels are
// under clip, or at or beyond right edge" (its readme).
TEST(SpriteHitTests, Rom11EdgeTiming) {
  expectPass("sprite_hit_tests_2005.10.05/11.edge_timing.nes");
}

// ---- SpriteOverflowTests ----

// "Tests basic operation of sprite overflow flag" (its readme).
TEST(SpriteOverflowTests, Rom1Basics) { expectPass("sprite_overflow_tests/1.Basics.nes"); }

// "Tests more detailed operation" (its readme).
TEST(SpriteOverflowTests, Rom2Details) { expectPass("sprite_overflow_tests/2.Details.nes"); }

// "Tests timing of sprite overflow flag. The tests fail if timing is off by more than a
// CPU clock or two" (its readme).
TEST(SpriteOverflowTests, Rom3Timing) { expectPass("sprite_overflow_tests/3.Timing.nes"); }

// "Tests the pathological behavior when 8 sprites are on a scanline and the one just
// after the 8th is not on the scanline" (its readme).
TEST(SpriteOverflowTests, Rom4Obscure) { expectPass("sprite_overflow_tests/4.Obscure.nes"); }

// "Tests things that an emulator with predictive overflow flag handling is likely to get
// wrong" (its readme).
TEST(SpriteOverflowTests, Rom5Emulator) { expectPass("sprite_overflow_tests/5.Emulator.nes"); }

}  // namespace
