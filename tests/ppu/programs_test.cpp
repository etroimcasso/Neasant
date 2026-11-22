#include "neasant/nes/board.h"
#include "neasant/nes/cartridge.h"
#include "neasant/nes/nes.h"
#include "neasant/ppu/ppu.h"
#include "authored_image.h"
#include "rom_harness.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifndef NEASANT_NES_TEST_ROMS
#define NEASANT_NES_TEST_ROMS ""
#endif

// The PPU's register file on the self-checking test programs: blargg's ppu_vbl_nmi,
// vbl_nmi_timing, his 2005 PPU tests, ppu_open_bus, oam_read and oam_stress booted on the
// machine and run to their report, with the two rules they measure beside them as hand
// cases: a sprite's byte 2 read back as five bits, and the dot a write to $2001 reaches
// the odd frame's skip on. The NEASANT_NES_TEST_ROMS option, the skips and
// NEASANT_REQUIRE_NES_TEST_ROMS are as in the CPU programs' test. The programs that time
// the vertical-blank flag and the NMI to one PPU clock pass under alignment 0, the
// config's default; their readmes say the console fails some of them after some resets.

namespace {

using neasant::Board;
using neasant::boardFor;
using neasant::Cartridge;
using neasant::makeCartridge;
using neasant::Mirroring;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::PpuDotEvent;
using neasant::PpuInputs;
using neasant::ppuPeekRegister;
using neasant::ppuRead;
using neasant::ppuReset;
using neasant::PpuState;
using neasant::ppuTickDot;
using neasant::ppuWrite;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::loadRom;
using neasant::nes_tests::placeProgram;
using neasant::nes_tests::Report;
using neasant::nes_tests::RomResult;
using neasant::nes_tests::runUntilResult;

using Bytes = std::vector<std::uint8_t>;
using Program = std::initializer_list<std::uint8_t>;

// A bare chip over an NROM board with 16 KB of PRG ROM and 8 KB of CHR ROM, its warm-up
// window closed as it is after the first pre-render line.
struct Chip {
  Bytes image;
  Cartridge cartridge;
  Board board;
  std::vector<std::uint8_t> chrRam;
  PpuState state;
  PpuInputs inputs;

  explicit Chip(Mirroring mirroring = Mirroring::Horizontal, bool withChrRam = false)
      : image(authoredImage(inesHeader({.chrUnits = withChrRam ? std::uint8_t{0} : std::uint8_t{1},
                                        .verticalMirroring = mirroring == Mirroring::Vertical}),
                            16384, withChrRam ? 0 : 8192)),
        cartridge(makeCartridge(image)),
        board(boardFor(cartridge)),
        chrRam(cartridge.chrRamBytes),
        inputs{board, cartridge, chrRam, 115200u} {
    state.warmingUp = false;
  }

  void write(std::uint8_t port, std::uint8_t value) { ppuWrite(state, port, value, inputs); }
  std::uint8_t read(std::uint8_t port) { return ppuRead(state, port, inputs); }

  // Sets the current address through the two writes to $2006.
  void address(std::uint16_t v) {
    write(6, static_cast<std::uint8_t>(v >> 8));
    write(6, static_cast<std::uint8_t>(v & 0xFFu));
  }
};

// A 16 KB NROM image with 8 KB of CHR ROM, `program` at $8000 and every vector pointing
// there.
Bytes imageWith(Program program) {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  placeProgram(image, 0, std::span<const std::uint8_t>(program.begin(), program.size()));
  return image;
}

// Steps `cycles` machine cycles.
void stepCycles(Nes& machine, int cycles) {
  for (int i = 0; i < cycles; ++i) machine.step();
}

// A machine over `image` built with `alignment` and the flag clear at power-on, stepped
// through its reset sequence, its state then changed by `edit` and restored.
template <typename Edit>
Nes prepared(const Bytes& image, std::uint8_t alignment, Edit edit) {
  Nes machine(NesConfig{.rom = image, .alignment = alignment, .vblankAtPowerOn = false});
  stepCycles(machine, 7);
  NesState state = machine.state();
  edit(state);
  machine.restore(state);
  return machine;
}

// The guard, in frames: 60 seconds at the console's rate. oam_stress's readme gives about
// 30 seconds for its run, the longest here.
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

// Registers a program the machine cannot run: the file is looked for as above, so a
// required clone that lacks it fails, and the case then skips naming what it needs.
void skipNeeding(const std::string& relative, const std::string& needs) {
  std::optional<Bytes> image;
  loadOrSkip(relative, image);
  if (!image) return;
  GTEST_SKIP() << relative << " needs " << needs;
}

// Boots the ROM on a machine built with the defaults, or with the CPU-PPU alignment
// `alignment` and the power-on pattern `ramSeed`, and runs it to its report.
std::optional<RomResult> runProgram(const std::string& relative, Report report,
                                    std::uint8_t alignment = 0,
                                    std::span<const std::uint8_t> ramSeed = {}) {
  std::optional<Bytes> image;
  loadOrSkip(relative, image);
  if (!image) return std::nullopt;
  Nes machine(NesConfig{.rom = *image, .alignment = alignment, .ramSeed = ramSeed});
  return runUntilResult(machine, report, kGuardFrames);
}

// Runs the ROM as runProgram does and requires a pass.
void expectPass(const std::string& relative, Report report, std::uint8_t alignment = 0,
                std::span<const std::uint8_t> ramSeed = {}) {
  const std::optional<RomResult> result = runProgram(relative, report, alignment, ramSeed);
  if (!result) return;
  EXPECT_TRUE(result->reported) << relative << " did not report within " << kGuardFrames
                                << " frames\n--- text ---\n"
                                << result->text << "\n------------";
  if (!result->reported) return;
  EXPECT_TRUE(result->passed) << relative << " reports a failure after " << result->frames
                              << " frames\n--- text ---\n"
                              << result->text << "\n------------";
}

// The palette power_up_palette expects at power-on: the table in its source, which its
// readme says holds the values of its author's console.
constexpr std::array<std::uint8_t, 32> kPowerUpPalette{
    0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0D, 0x08, 0x10, 0x08,
    0x24, 0x00, 0x00, 0x04, 0x2C, 0x09, 0x01, 0x34, 0x03, 0x00, 0x04,
    0x00, 0x14, 0x08, 0x3A, 0x00, 0x02, 0x00, 0x20, 0x2C, 0x08};

}  // namespace

// ---- PpuOam ----

// A sprite's byte 2 is five bits in the chip: a read of $2004 answers bits 7-5 and 1-0
// and the array keeps the byte as written (NESdev wiki, "PPU OAM", Byte 2 - Attributes).
// The read steps nothing (NESdev wiki, "PPU registers", OAMDATA).
TEST(PpuOam, ASpritesByteTwoReadsBackFiveBits) {
  Chip chip;
  chip.write(3, 0x02);
  chip.write(4, 0xFF);
  EXPECT_EQ(chip.state.oam[2], 0xFF);
  EXPECT_EQ(chip.state.oamAddr, 3);
  chip.write(3, 0x02);
  EXPECT_EQ(chip.read(4), 0xE3);
  EXPECT_EQ(chip.state.oamAddr, 2);
  EXPECT_EQ(chip.state.oam[2], 0xFF);
}

// Bytes 0, 1 and 3 of a sprite read back whole, and byte 2 of the last sprite as five
// bits (NESdev wiki, "PPU OAM", Byte 2 - Attributes; sprite_ram's fifth test writes $FF at
// address 3 and requires an answer other than $E3).
TEST(PpuOam, TheOtherThreeBytesReadBackWholeAndEverySpritesByteTwoAsFive) {
  Chip chip;
  chip.write(3, 0x00);
  for (int i = 0; i < 4; ++i) chip.write(4, 0xFF);
  for (const std::uint8_t address : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{3}}) {
    chip.write(3, address);
    EXPECT_EQ(chip.read(4), 0xFF) << "address " << int{address};
  }
  // $FE is the last sprite's byte 2 and $1C is the three bits alone.
  chip.write(3, 0xFE);
  chip.write(4, 0x1C);
  chip.write(3, 0xFE);
  EXPECT_EQ(chip.read(4), 0x00);
}

// The read drives all eight bits of the I/O bus with the five it answered, bits 4-2 as 0,
// and recharges each (ppu_open_bus's readme and its eleventh test: "Reading third byte of
// a sprite from $2004 should refresh all bits of decay value"; NESdev wiki, "Open bus
// behavior", PPU open bus).
TEST(PpuOam, TheReadDrivesTheWholeBusWithTheFiveBits) {
  Chip chip;
  chip.write(3, 0x06);
  chip.write(4, 0xFF);
  chip.write(3, 0x06);
  chip.write(2, 0xFF);  // the bus holds $FF, every count 115 200
  for (int i = 0; i < 10; ++i) ppuTickDot(chip.state, false, chip.inputs);  // every count 115 190
  EXPECT_EQ(chip.read(4), 0xE3);
  EXPECT_EQ(chip.state.ioLatch, 0xE3);
  for (std::size_t bit = 0; bit < 8; ++bit) {
    EXPECT_EQ(chip.state.ioLatchDecay[bit], 115200u) << "bit " << bit;
  }
  EXPECT_EQ(chip.read(0), 0xE3);
}

// A peek answers what the read would and moves nothing.
TEST(PpuOam, APeekAnswersTheFiveBitsAndMovesNothing) {
  PpuState state;
  state.oamAddr = 0x0A;
  state.oam[0x0A] = 0xFF;
  state.ioLatch = 0x55;
  EXPECT_EQ(ppuPeekRegister(state, 4), 0xE3);
  EXPECT_EQ(state.oamAddr, 0x0A);
  EXPECT_EQ(state.ioLatch, 0x55);
  EXPECT_EQ(state.oam[0x0A], 0xFF);
}

TEST(PpuOam, TheMachineReadsASpritesByteTwoThroughThePort) {
  // LDA #$02; STA $2003; LDA #$FF; STA $2004; LDA #$02; STA $2003; LDA $2004; STA $00;
  // JMP $8014: cycles 0-6 are the reset sequence and the store into $00 is cycle 31.
  const Bytes image = imageWith({0xA9, 0x02, 0x8D, 0x03, 0x20, 0xA9, 0xFF, 0x8D,
                                 0x04, 0x20, 0xA9, 0x02, 0x8D, 0x03, 0x20, 0xAD,
                                 0x04, 0x20, 0x85, 0x00, 0x4C, 0x14, 0x80});
  Nes machine(NesConfig{.rom = image});
  stepCycles(machine, 32);
  EXPECT_EQ(machine.state().ram[0], 0xE3);
  EXPECT_EQ(machine.state().cpu.a, 0xE3);
  EXPECT_EQ(machine.state().ppu.oam[2], 0xFF);
  EXPECT_EQ(machine.state().ppu.oamAddr, 2);
}

// ---- PpuSkip ----

// A write that enables the background while the counter stands at dot 337 of an odd
// frame's scanline 261 is read at dot 339, its third dot, and the dot is skipped (NESdev
// wiki, "PPU frame timing", Even/Odd Frames; "PPU registers", PPUMASK, Rendering control;
// 10-even_odd_timing's first result, 08).
TEST(PpuSkip, TheBackgroundEnabledThreeDotsBeforeTheSkipIsRead) {
  Chip chip;
  chip.state.odd = true;
  chip.state.scanline = 261;
  chip.state.dot = 337;
  chip.write(1, 0x08);
  EXPECT_EQ(chip.state.maskLag, 3);
  EXPECT_EQ(chip.state.maskBefore, 0x00);
  EXPECT_EQ(chip.state.mask, 0x08);
  EXPECT_EQ(ppuTickDot(chip.state, false, chip.inputs), PpuDotEvent::None);
  EXPECT_EQ(chip.state.dot, 338);
  EXPECT_EQ(ppuTickDot(chip.state, false, chip.inputs), PpuDotEvent::None);
  EXPECT_EQ(chip.state.dot, 339);
  EXPECT_EQ(ppuTickDot(chip.state, false, chip.inputs), PpuDotEvent::FrameEnded);
  EXPECT_EQ(chip.state.scanline, 0);
  EXPECT_EQ(chip.state.dot, 0);
  EXPECT_FALSE(chip.state.odd);
  EXPECT_EQ(chip.state.maskLag, 0);
}

// The same write one dot later is short of its third dot at dot 339, and the line runs
// its 341 dots (10-even_odd_timing's second result, 08).
TEST(PpuSkip, TheBackgroundEnabledTwoDotsBeforeTheSkipIsNotRead) {
  Chip chip;
  chip.state.odd = true;
  chip.state.scanline = 261;
  chip.state.dot = 338;
  chip.write(1, 0x08);
  EXPECT_EQ(ppuTickDot(chip.state, false, chip.inputs), PpuDotEvent::None);
  EXPECT_EQ(chip.state.dot, 339);
  EXPECT_EQ(ppuTickDot(chip.state, false, chip.inputs), PpuDotEvent::None);
  EXPECT_EQ(chip.state.scanline, 261);
  EXPECT_EQ(chip.state.dot, 340);
  EXPECT_EQ(ppuTickDot(chip.state, false, chip.inputs), PpuDotEvent::FrameEnded);
  EXPECT_EQ(chip.state.scanline, 0);
  EXPECT_EQ(chip.state.dot, 0);
}

// A write that disables the background at dot 337 is read at dot 339 and the frame keeps
// its dot (10-even_odd_timing's third result, 09).
TEST(PpuSkip, TheBackgroundDisabledThreeDotsBeforeTheSkipIsRead) {
  Chip chip;
  chip.state.odd = true;
  chip.state.scanline = 261;
  chip.state.dot = 337;
  chip.state.mask = 0x08;
  chip.write(1, 0x00);
  EXPECT_EQ(chip.state.maskBefore, 0x08);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(ppuTickDot(chip.state, false, chip.inputs), PpuDotEvent::None) << "tick " << i;
  }
  EXPECT_EQ(chip.state.scanline, 261);
  EXPECT_EQ(chip.state.dot, 340);
  EXPECT_EQ(ppuTickDot(chip.state, false, chip.inputs), PpuDotEvent::FrameEnded);
  EXPECT_EQ(chip.state.scanline, 0);
  EXPECT_EQ(chip.state.dot, 0);
}

// The same write one dot later leaves the skip to the bits that stood before it
// (10-even_odd_timing's fourth result, 07).
TEST(PpuSkip, TheBackgroundDisabledTwoDotsBeforeTheSkipIsNotRead) {
  Chip chip;
  chip.state.odd = true;
  chip.state.scanline = 261;
  chip.state.dot = 338;
  chip.state.mask = 0x08;
  chip.write(1, 0x00);
  EXPECT_EQ(ppuTickDot(chip.state, false, chip.inputs), PpuDotEvent::None);
  EXPECT_EQ(chip.state.dot, 339);
  EXPECT_EQ(ppuTickDot(chip.state, false, chip.inputs), PpuDotEvent::FrameEnded);
  EXPECT_EQ(chip.state.scanline, 0);
  EXPECT_EQ(chip.state.dot, 0);
  EXPECT_FALSE(chip.state.odd);
}

// The lag holds the two rendering bits and runs three dots on any line; a write to
// another port leaves it alone.
TEST(PpuSkip, TheLagHoldsTheTwoRenderingBitsForThreeDots) {
  Chip chip;
  chip.state.mask = 0xFF;
  chip.write(1, 0x01);
  EXPECT_EQ(chip.state.maskBefore, 0x18);
  EXPECT_EQ(chip.state.mask, 0x01);
  EXPECT_EQ(chip.state.maskLag, 3);
  chip.write(0, 0x00);
  EXPECT_EQ(chip.state.maskLag, 3);
  for (const std::uint8_t left : {std::uint8_t{2}, std::uint8_t{1}, std::uint8_t{0}}) {
    ppuTickDot(chip.state, false, chip.inputs);
    EXPECT_EQ(chip.state.maskLag, left);
    EXPECT_EQ(chip.state.maskBefore, 0x18);
  }
}

// A write to $2001 inside the warm-up window is ignored whole (NESdev wiki, "PPU power up
// state").
TEST(PpuSkip, AWriteInTheWarmUpStartsNoLag) {
  Chip chip;
  chip.state.warmingUp = true;
  chip.write(1, 0x08);
  EXPECT_EQ(chip.state.mask, 0);
  EXPECT_EQ(chip.state.maskLag, 0);
  EXPECT_EQ(chip.state.maskBefore, 0);
  EXPECT_EQ(chip.state.ioLatch, 0x08);
}

// The reset line clears $2001 and the lag with it (NESdev wiki, "PPU power up state", the
// note on the internal reset signal).
TEST(PpuSkip, ResetDropsAWriteStillOnItsWay) {
  Chip chip;
  chip.state.mask = 0x18;
  chip.write(1, 0x00);
  ppuTickDot(chip.state, false, chip.inputs);
  EXPECT_EQ(chip.state.maskLag, 2);
  EXPECT_EQ(chip.state.maskBefore, 0x18);
  ppuReset(chip.state);
  EXPECT_EQ(chip.state.mask, 0);
  EXPECT_EQ(chip.state.maskBefore, 0);
  EXPECT_EQ(chip.state.maskLag, 0);
}

// A second write inside a lag keeps the bits the counter was reading and counts three
// dots again. The CPU cannot make it, as three dots pass between two of its accesses;
// the case fixes what a state built by hand does.
TEST(PpuSkip, ASecondWriteInsideALagKeepsTheBitsTheCounterWasReading) {
  Chip chip;
  chip.write(1, 0x08);
  EXPECT_EQ(chip.state.maskBefore, 0x00);
  EXPECT_EQ(chip.state.maskLag, 3);
  ppuTickDot(chip.state, false, chip.inputs);
  EXPECT_EQ(chip.state.maskLag, 2);
  chip.write(1, 0x10);
  EXPECT_EQ(chip.state.maskBefore, 0x00);
  EXPECT_EQ(chip.state.maskLag, 3);
  EXPECT_EQ(chip.state.mask, 0x10);
}

// LDA #$08; STA $2001; JMP $8005 on a machine restored after its reset sequence with the
// counter at dot `from` of an odd frame's scanline 261. The write's access is in machine
// cycle 12, and seven cycles after the restore the counter stands at the dot each row
// names: under alignment 0 seventeen dots tick before the access and four after it by the
// end of cycle 13, under the others sixteen and five, and the skip follows the write when
// dot 339 is the third or a later dot after the access (NESdev wiki, "PPU frame timing",
// Even/Odd Frames and CPU-PPU Clock Alignment; "PPU registers", PPUMASK, Rendering
// control).
TEST(PpuSkip, TheMachinePutsTheSkipOnTheWritesThirdDot) {
  struct Row {
    std::uint8_t alignment;
    std::uint16_t from;
    std::uint16_t dot;
  };
  constexpr std::array<Row, 16> kRows{{
      {0, 319, 0}, {0, 320, 1}, {0, 321, 1}, {0, 322, 2},
      {1, 320, 1}, {1, 321, 2}, {1, 322, 2}, {1, 323, 3},
      {2, 320, 1}, {2, 321, 2}, {2, 322, 2}, {2, 323, 3},
      {3, 320, 1}, {3, 321, 2}, {3, 322, 2}, {3, 323, 3},
  }};
  const Bytes image = imageWith({0xA9, 0x08, 0x8D, 0x01, 0x20, 0x4C, 0x05, 0x80});
  for (const Row& row : kRows) {
    Nes machine = prepared(image, row.alignment, [&](NesState& s) {
      s.ppu.warmingUp = false;
      s.ppu.odd = true;
      s.ppu.scanline = 261;
      s.ppu.dot = row.from;
    });
    stepCycles(machine, 7);
    const PpuState& ppu = machine.state().ppu;
    EXPECT_EQ(ppu.scanline, 0) << "alignment " << int{row.alignment} << ", from " << row.from;
    EXPECT_FALSE(ppu.odd) << "alignment " << int{row.alignment} << ", from " << row.from;
    EXPECT_EQ(ppu.mask, 0x08) << "alignment " << int{row.alignment} << ", from " << row.from;
    EXPECT_EQ(ppu.maskLag, 0) << "alignment " << int{row.alignment} << ", from " << row.from;
    EXPECT_EQ(ppu.dot, row.dot) << "alignment " << int{row.alignment} << ", from " << row.from;
  }
}

// ---- PpuVblNmi ----

// The multi-test build, on mapper 1 (its readme, "Multi-tests").
TEST(PpuVblNmi, PpuVblNmi) {
  skipNeeding("ppu_vbl_nmi/ppu_vbl_nmi.nes",
              "mapper 1, which is not built; its sub-tests run from rom_singles");
}

// "Tests basic VBL operation and VBL period" (its readme).
TEST(PpuVblNmi, Singles01VblBasics) {
  expectPass("ppu_vbl_nmi/rom_singles/01-vbl_basics.nes", Report::Memory);
}

// "Verifies time VBL flag is set" (its readme): the table of nine rows under alignment 0.
TEST(PpuVblNmi, Singles02VblSetTime) {
  expectPass("ppu_vbl_nmi/rom_singles/02-vbl_set_time.nes", Report::Memory);
}

// The same program under alignments 1 to 3 prints "03 - -", the row its readme gives
// "after some resets", and reports a failure by its checksum.
TEST(PpuVblNmi, Singles02VblSetTimeUnderTheOtherAlignments) {
  for (const std::uint8_t alignment : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3}}) {
    const std::optional<RomResult> result =
        runProgram("ppu_vbl_nmi/rom_singles/02-vbl_set_time.nes", Report::Memory, alignment);
    if (!result) return;
    EXPECT_TRUE(result->reported) << "alignment " << int{alignment};
    EXPECT_FALSE(result->passed) << "alignment " << int{alignment};
    EXPECT_NE(result->text.find("03 - -"), std::string::npos)
        << "alignment " << int{alignment} << "\n--- text ---\n"
        << result->text << "\n------------";
  }
}

// "Tests time VBL flag is cleared" (its readme).
TEST(PpuVblNmi, Singles03VblClearTime) {
  expectPass("ppu_vbl_nmi/rom_singles/03-vbl_clear_time.nes", Report::Memory);
}

// "Tests immediate NMI behavior when enabling while VBL flag is already set" (its readme).
TEST(PpuVblNmi, Singles04NmiControl) {
  expectPass("ppu_vbl_nmi/rom_singles/04-nmi_control.nes", Report::Memory);
}

// "Tests NMI timing" (its readme): the instruction the interrupt follows, a PPU clock
// later each row.
TEST(PpuVblNmi, Singles05NmiTiming) {
  expectPass("ppu_vbl_nmi/rom_singles/05-nmi_timing.nes", Report::Memory);
}

// "Tests behavior when $2002 is read near time VBL flag is set" (its readme).
TEST(PpuVblNmi, Singles06Suppression) {
  expectPass("ppu_vbl_nmi/rom_singles/06-suppression.nes", Report::Memory);
}

// "Tests NMI occurrence when enabled near time VBL flag is cleared" (its readme).
TEST(PpuVblNmi, Singles07NmiOnTiming) {
  expectPass("ppu_vbl_nmi/rom_singles/07-nmi_on_timing.nes", Report::Memory);
}

// "Tests NMI occurrence when disabled near time VBL flag is set" (its readme).
TEST(PpuVblNmi, Singles08NmiOffTiming) {
  expectPass("ppu_vbl_nmi/rom_singles/08-nmi_off_timing.nes", Report::Memory);
}

// "Tests clock skipped on every other PPU frame when BG rendering is enabled" (its
// readme).
TEST(PpuVblNmi, Singles09EvenOddFrames) {
  expectPass("ppu_vbl_nmi/rom_singles/09-even_odd_frames.nes", Report::Memory);
}

// "Tests timing of skipped clock every other frame when BG is enabled" (its readme):
// output 08 08 09 07.
TEST(PpuVblNmi, Singles10EvenOddTiming) {
  expectPass("ppu_vbl_nmi/rom_singles/10-even_odd_timing.nes", Report::Memory);
}

// ---- VblNmiTiming ----

// "Tests basic VBL flag operation and general timing of PPU frames" (its readme).
TEST(VblNmiTiming, Rom1FrameBasics) {
  expectPass("vbl_nmi_timing/1.frame_basics.nes", Report::Screen);
}

// "Tests timing of VBL being set, and special case where reading VBL flag as it would be
// set causes it to not be set for that frame" (its readme).
TEST(VblNmiTiming, Rom2VblTiming) {
  expectPass("vbl_nmi_timing/2.vbl_timing.nes", Report::Screen);
}

// "Test clock skipped when BG is enabled on odd PPU frames" (its readme).
TEST(VblNmiTiming, Rom3EvenOddFrames) {
  expectPass("vbl_nmi_timing/3.even_odd_frames.nes", Report::Screen);
}

// "Tests timing of VBL flag clearing" (its readme).
TEST(VblNmiTiming, Rom4VblClearTiming) {
  expectPass("vbl_nmi_timing/4.vbl_clear_timing.nes", Report::Screen);
}

// "Tests timing of NMI suppression when reading VBL flag just as it's set, and that this
// doesn't occur when reading one clock before or after" (its readme).
TEST(VblNmiTiming, Rom5NmiSuppression) {
  expectPass("vbl_nmi_timing/5.nmi_suppression.nes", Report::Screen);
}

// "Tests NMI occurrence when disabling NMI just as VBL flag is set, and just after" (its
// readme).
TEST(VblNmiTiming, Rom6NmiDisable) {
  expectPass("vbl_nmi_timing/6.nmi_disable.nes", Report::Screen);
}

// "Tests timing of NMI and immediate occurrence when enabled with VBL flag already set"
// (its readme).
TEST(VblNmiTiming, Rom7NmiTiming) {
  expectPass("vbl_nmi_timing/7.nmi_timing.nes", Report::Screen);
}

// ---- BlarggPpu2005 ----

// "PPU palette RAM read/write and mirroring test" (its readme).
TEST(BlarggPpu2005, RomPaletteRam) {
  expectPass("blargg_ppu_tests_2005.09.15b/palette_ram.nes", Report::Screen);
}

// "Reports whether initial values in palette at power-up match those that my NES has"
// (its readme): the machine is built with that table as its power-on pattern, which
// reaches the palette.
TEST(BlarggPpu2005, RomPowerUpPalette) {
  expectPass("blargg_ppu_tests_2005.09.15b/power_up_palette.nes", Report::Screen, 0,
             kPowerUpPalette);
}

// "Tests sprite RAM access via $2003, $2004, and $4014" (its readme).
TEST(BlarggPpu2005, RomSpriteRam) {
  expectPass("blargg_ppu_tests_2005.09.15b/sprite_ram.nes", Report::Screen);
}

// The vertical-blank flag cleared "around 2270 CPU clocks after NMI occurs" (its readme).
TEST(BlarggPpu2005, RomVblClearTime) {
  expectPass("blargg_ppu_tests_2005.09.15b/vbl_clear_time.nes", Report::Screen);
}

// "Tests PPU VRAM read/write and internal read buffer operation" (its readme).
TEST(BlarggPpu2005, RomVramAccess) {
  expectPass("blargg_ppu_tests_2005.09.15b/vram_access.nes", Report::Screen);
}

// ---- PpuOpenBus, OamRead, OamStress, PpuReadBuffer ----

// "Tests behavior when reading from open-bus PPU bits/registers, those bits that aren't
// otherwise defined" (its readme). Its longest wait for a bit to decay is one second, and
// the config's default serves it.
TEST(PpuOpenBus, PpuOpenBus) {
  expectPass("ppu_open_bus/ppu_open_bus.nes", Report::Memory);
}

// "Tests OAM reading ($2004), being sure it reads the byte from OAM at the current address
// in $2003" (its readme).
TEST(OamRead, OamRead) { expectPass("oam_read/oam_read.nes", Report::Memory); }

// "Thoroughly tests OAM address ($2003) and read/write ($2004)" (its readme), which says
// the console passes it on one of its four power-on synchronizations.
TEST(OamStress, OamStress) { expectPass("oam_stress/oam_stress.nes", Report::Memory); }

// Bisqwit's and blargg's read-buffer test pack, on mapper 3 with four banks of CHR (its
// header).
TEST(PpuReadBuffer, TestPpuReadBuffer) {
  skipNeeding("ppu_read_buffer/test_ppu_read_buffer.nes",
              "mapper 3 with four banks of CHR, which is not built");
}
