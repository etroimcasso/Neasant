#include "neasant/nes/board.h"
#include "neasant/nes/cartridge.h"
#include "neasant/nes/nes.h"
#include "neasant/ppu/ppu.h"
#include "authored_image.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

// The PPU without its picture: the eight ports with their latches, values and side
// effects; the scroll file; the port at $2007 over the chip's address space, the
// console's nametable RAM, the palette and OAM; the read buffer; the warm-up window;
// the frame counter with its flag and its odd-frame skip; the NMI output and the cycle
// the CPU takes the interrupt on; a read of $2002 against the flag's dot under each
// alignment; power-on and reset. A case that checks a hardware rule names the document
// and section it is read from; the cases on what a peek answers check the chip's own
// surface.

namespace {

using neasant::Board;
using neasant::boardFor;
using neasant::Cartridge;
using neasant::Cpu6502State;
using neasant::CpuSequence;
using neasant::makeCartridge;
using neasant::Mirroring;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::PpuInputs;
using neasant::ppuNmiAsserted;
using neasant::ppuPeekRegister;
using neasant::ppuRead;
using neasant::ppuReset;
using neasant::PpuState;
using neasant::ppuTickDot;
using neasant::ppuWrite;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::placeProgram;
using neasant::nes_tests::prgAt;

using Bytes = std::vector<std::uint8_t>;
using Program = std::initializer_list<std::uint8_t>;

constexpr std::uint8_t kNop = 0xEA;

// JMP $8000
constexpr Program kJmpLoop = {0x4C, 0x00, 0x80};
// LDA $2002; JMP $8003
constexpr Program kReadStatus = {0xAD, 0x02, 0x20, 0x4C, 0x03, 0x80};
// LDA #$80; STA $2000; LDA #$00; STA $2000; LDA #$80; STA $2000; JMP $800F
constexpr Program kToggleNmi = {0xA9, 0x80, 0x8D, 0x00, 0x20, 0xA9, 0x00, 0x8D, 0x00,
                                0x20, 0xA9, 0x80, 0x8D, 0x00, 0x20, 0x4C, 0x0F, 0x80};
// LDA #$80; STA $2000; LDA #$1E; STA $2001; JMP $8000: fifteen cycles a loop
constexpr Program kWriteControlAndMask = {0xA9, 0x80, 0x8D, 0x00, 0x20, 0xA9, 0x1E,
                                          0x8D, 0x01, 0x20, 0x4C, 0x00, 0x80};
// LDA #$AA; STA $2005; JMP $8005: one write to the bus, then nothing
constexpr Program kDriveBusOnce = {0xA9, 0xAA, 0x8D, 0x05, 0x20, 0x4C, 0x05, 0x80};

// A bare chip over an NROM board: 16 KB of PRG ROM, 8 KB of CHR ROM or of CHR RAM, the
// mirroring named, and the warm-up window closed as it is after the first pre-render
// line. A case sets the CHR ROM bytes it reads through `cartridge.chrRom`.
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

// A 16 KB NROM image with 8 KB of CHR ROM: `program` at $8000, the reset and IRQ
// vectors pointing there, the NMI vector at $9000 and an RTI there.
Bytes imageWith(Program program) {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  placeProgram(image, 0, std::span<const std::uint8_t>(program.begin(), program.size()));
  prgAt(image, 0x1000) = 0x40;  // RTI at $9000
  prgAt(image, 0x3FFA) = 0x00;  // the NMI vector: $9000
  prgAt(image, 0x3FFB) = 0x90;
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

// ---- PpuPorts ----

TEST(PpuPorts, AWriteLoadsTheBusAndAWriteOnlyPortAnswersIt) {
  // NESdev wiki, "Open bus behavior", PPU open bus; "PPU registers", MMIO registers.
  Chip chip;
  chip.write(5, 0xAA);
  EXPECT_EQ(chip.state.ioLatch, 0xAA);
  EXPECT_EQ(chip.read(0), 0xAA);
  EXPECT_EQ(chip.read(1), 0xAA);
  EXPECT_EQ(chip.read(3), 0xAA);
  EXPECT_EQ(chip.read(5), 0xAA);
  EXPECT_EQ(chip.read(6), 0xAA);
  chip.write(2, 0x55);
  EXPECT_EQ(chip.state.ioLatch, 0x55);
  EXPECT_EQ(chip.read(6), 0x55);
}

TEST(PpuPorts, ControlTakesTheNametableBitsAndDrivesTheNmiOutput) {
  // NESdev wiki, "PPU scrolling", $2000 write; "NMI", Operation.
  Chip chip;
  chip.write(0, 0x83);
  EXPECT_EQ(chip.state.ctrl, 0x83);
  EXPECT_EQ(chip.state.t, 0x0C00);
  EXPECT_FALSE(ppuNmiAsserted(chip.state));
  chip.state.vblank = true;
  EXPECT_TRUE(ppuNmiAsserted(chip.state));
  chip.write(0, 0x05);
  EXPECT_EQ(chip.state.ctrl, 0x05);
  EXPECT_EQ(chip.state.t, 0x0400);
  EXPECT_FALSE(ppuNmiAsserted(chip.state));
  chip.write(0, 0x00);
  EXPECT_EQ(chip.state.t, 0x0000);
}

TEST(PpuPorts, ScrollAndAddressShareTheToggle) {
  // NESdev wiki, "PPU scrolling", Summary: the page's worked sequence.
  Chip chip;
  chip.write(0, 0x00);
  EXPECT_EQ(chip.state.t, 0x0000);
  chip.read(2);
  EXPECT_FALSE(chip.state.w);
  chip.write(5, 0x7D);
  EXPECT_EQ(chip.state.t, 0x000F);
  EXPECT_EQ(chip.state.x, 5);
  EXPECT_TRUE(chip.state.w);
  chip.write(5, 0x5E);
  EXPECT_EQ(chip.state.t, 0x616F);
  EXPECT_FALSE(chip.state.w);
  EXPECT_EQ(chip.state.v, 0x0000);
  chip.write(6, 0x3D);
  EXPECT_EQ(chip.state.t, 0x3D6F);
  EXPECT_TRUE(chip.state.w);
  EXPECT_EQ(chip.state.v, 0x0000);
  chip.write(6, 0xF0);
  EXPECT_EQ(chip.state.t, 0x3DF0);
  EXPECT_EQ(chip.state.v, 0x3DF0);
  EXPECT_FALSE(chip.state.w);
  EXPECT_EQ(chip.state.x, 5);
}

TEST(PpuPorts, AStatusReadClearsTheToggle) {
  // NESdev wiki, "PPU scrolling", $2002 read and $2005 first write.
  Chip chip;
  chip.write(5, 0x12);
  EXPECT_TRUE(chip.state.w);
  EXPECT_EQ(chip.state.t, 0x0002);
  EXPECT_EQ(chip.state.x, 2);
  chip.read(2);
  EXPECT_FALSE(chip.state.w);
  chip.write(5, 0x34);
  EXPECT_EQ(chip.state.t, 0x0006);
  EXPECT_EQ(chip.state.x, 4);
  EXPECT_TRUE(chip.state.w);
}

TEST(PpuPorts, AddressWritesAreTheHighByteThenTheLowWithBitFourteenCleared) {
  // NESdev wiki, "PPU scrolling", $2006 first and second write; "PPU registers",
  // PPUADDR.
  Chip chip;
  chip.write(6, 0xFF);
  EXPECT_EQ(chip.state.t, 0x3F00);
  EXPECT_TRUE(chip.state.w);
  EXPECT_EQ(chip.state.v, 0x0000);
  chip.write(6, 0xFF);
  EXPECT_EQ(chip.state.t, 0x3FFF);
  EXPECT_EQ(chip.state.v, 0x3FFF);
  EXPECT_FALSE(chip.state.w);
  chip.state.t = 0x7FFF;
  chip.write(6, 0x21);
  EXPECT_EQ(chip.state.t, 0x21FF);
  chip.write(6, 0x08);
  EXPECT_EQ(chip.state.t, 0x2108);
  EXPECT_EQ(chip.state.v, 0x2108);
}

TEST(PpuPorts, DataWritesLandInNametableRamThroughTheBoardsLines) {
  // NESdev wiki, "PPU memory map", Hardware mapping; "Cartridge connector", Signal
  // descriptions: CIRAM /CE and CIRAM A10; "PPU registers", PPUDATA.
  Chip horizontal(Mirroring::Horizontal);
  horizontal.address(0x2000);
  horizontal.write(7, 0x11);
  EXPECT_EQ(horizontal.state.ciram[0], 0x11);
  EXPECT_EQ(horizontal.state.v, 0x2001);
  horizontal.address(0x2400);
  horizontal.write(7, 0x22);
  EXPECT_EQ(horizontal.state.ciram[0], 0x22);
  horizontal.address(0x2800);
  horizontal.write(7, 0x33);
  EXPECT_EQ(horizontal.state.ciram[0x400], 0x33);
  horizontal.address(0x2C00);
  horizontal.write(7, 0x44);
  EXPECT_EQ(horizontal.state.ciram[0x400], 0x44);
  EXPECT_EQ(horizontal.state.ciram[0], 0x22);
  horizontal.address(0x23FF);
  horizontal.write(7, 0x55);
  EXPECT_EQ(horizontal.state.ciram[0x3FF], 0x55);

  Chip vertical(Mirroring::Vertical);
  vertical.address(0x2000);
  vertical.write(7, 0x11);
  EXPECT_EQ(vertical.state.ciram[0], 0x11);
  vertical.address(0x2400);
  vertical.write(7, 0x22);
  EXPECT_EQ(vertical.state.ciram[0x400], 0x22);
  EXPECT_EQ(vertical.state.ciram[0], 0x11);
  vertical.address(0x2800);
  vertical.write(7, 0x33);
  EXPECT_EQ(vertical.state.ciram[0], 0x33);
  vertical.address(0x2C00);
  vertical.write(7, 0x44);
  EXPECT_EQ(vertical.state.ciram[0x400], 0x44);
}

TEST(PpuPorts, TheRangeAboveTheNametablesRepeatsThem) {
  // NESdev wiki, "PPU memory map", Hardware mapping: $3000-$3EFF repeats $2000-$2EFF.
  Chip chip;
  chip.address(0x3000);
  chip.write(7, 0x55);
  EXPECT_EQ(chip.state.ciram[0], 0x55);
  chip.address(0x3EFF);
  chip.write(7, 0x66);
  EXPECT_EQ(chip.state.ciram[0x6FF], 0x66);
  chip.address(0x2EFF);
  chip.read(7);
  EXPECT_EQ(chip.read(7), 0x66);
}

TEST(PpuPorts, DataWritesReachChrRamAndNeverChrRom) {
  // NESdev wiki, "NROM", Banks; "PPU registers", PPUDATA.
  Chip ram(Mirroring::Horizontal, true);
  ram.address(0x0123);
  ram.write(7, 0x5A);
  EXPECT_EQ(ram.chrRam[0x123], 0x5A);
  EXPECT_EQ(ram.state.v, 0x0124);
  Chip rom;
  rom.address(0x0123);
  rom.write(7, 0x5A);
  EXPECT_EQ(rom.cartridge.chrRom[0x123], 0);
}

TEST(PpuPorts, TheReadBufferLagsOneRead) {
  // NESdev wiki, "PPU registers", The PPUDATA read buffer; "Open bus behavior", PPU
  // open bus.
  Chip chip;
  chip.cartridge.chrRom[0x10] = 0xA1;
  chip.cartridge.chrRom[0x11] = 0xB2;
  chip.cartridge.chrRom[0x12] = 0xC3;
  chip.address(0x0010);
  EXPECT_EQ(chip.read(7), 0x00);
  EXPECT_EQ(chip.state.readBuffer, 0xA1);
  EXPECT_EQ(chip.state.v, 0x0011);
  EXPECT_EQ(chip.read(7), 0xA1);
  EXPECT_EQ(chip.state.readBuffer, 0xB2);
  EXPECT_EQ(chip.read(7), 0xB2);
  EXPECT_EQ(chip.state.readBuffer, 0xC3);
  EXPECT_EQ(chip.state.ioLatch, 0xB2);
  EXPECT_EQ(chip.state.v, 0x0013);
}

TEST(PpuPorts, TheAddressStepsByOneOrThirtyTwo) {
  // NESdev wiki, "PPU registers", PPUCTRL and PPUDATA; "PPU scrolling", PPU internal
  // registers: v is 15 bits.
  Chip chip;
  chip.write(0, 0x04);
  chip.address(0x2000);
  chip.write(7, 0x00);
  EXPECT_EQ(chip.state.v, 0x2020);
  chip.write(7, 0x00);
  EXPECT_EQ(chip.state.v, 0x2040);
  chip.write(0, 0x00);
  chip.write(7, 0x00);
  EXPECT_EQ(chip.state.v, 0x2041);
  chip.state.v = 0x7FFF;
  chip.read(7);
  EXPECT_EQ(chip.state.v, 0x0000);
}

TEST(PpuPorts, PaletteReadsAnswerAtOnceUnderTheBusesTopBits) {
  // NESdev wiki, "PPU registers", Reading palette RAM and Color control; "PPU memory
  // map": the entries repeat through $3FFF.
  Chip chip;
  chip.state.palette[0x05] = 0x2C;
  chip.state.ciram[0x705] = 0x77;  // the byte underneath $3F05 under horizontal mirroring
  chip.address(0x3F05);
  chip.write(3, 0xC3);  // the bus's top two bits set; the OAM address is not read here
  EXPECT_EQ(chip.read(7), 0xEC);
  EXPECT_EQ(chip.state.readBuffer, 0x77);
  EXPECT_EQ(chip.state.v, 0x3F06);
  EXPECT_EQ(chip.state.ioLatch, 0xEC);
  chip.address(0x3F25);
  chip.write(3, 0x00);
  EXPECT_EQ(chip.read(7), 0x2C);
  chip.write(1, 0x01);
  chip.address(0x3F05);
  chip.write(3, 0xC3);
  EXPECT_EQ(chip.read(7), 0xE0);
  EXPECT_EQ(chip.state.palette[0x05], 0x2C);
}

TEST(PpuPorts, PaletteWritesKeepSixBitsAndRepeatThroughTheRange) {
  // NESdev wiki, "PPU registers", Reading palette RAM: six-bit entries; "PPU memory
  // map": $3F20-$3FFF repeats $3F00-$3F1F.
  Chip chip;
  chip.address(0x3F00);
  chip.write(7, 0xFF);
  EXPECT_EQ(chip.state.palette[0x00], 0x3F);
  EXPECT_EQ(chip.state.v, 0x3F01);
  chip.address(0x3FFF);
  chip.write(7, 0x12);
  EXPECT_EQ(chip.state.palette[0x1F], 0x12);
  chip.address(0x3F3E);
  chip.write(7, 0x21);
  EXPECT_EQ(chip.state.palette[0x1E], 0x21);
  EXPECT_EQ(chip.state.ciram[0x700], 0);
}

TEST(PpuPorts, PaletteWritesLandInTheirAliases) {
  // NESdev wiki, "PPU palettes", Palette RAM: entry 0 of each sprite palette is the
  // storage of entry 0 of the background palette beside it, and the whole is repeated
  // through $3FFF.
  Chip chip;
  chip.address(0x3F10);
  chip.write(7, 0x21);
  chip.address(0x3F14);
  chip.write(7, 0x22);
  chip.address(0x3F18);
  chip.write(7, 0x23);
  chip.address(0x3F1C);
  chip.write(7, 0x24);
  chip.address(0x3F11);
  chip.write(7, 0x31);
  EXPECT_EQ(chip.state.palette[0x00], 0x21);
  EXPECT_EQ(chip.state.palette[0x04], 0x22);
  EXPECT_EQ(chip.state.palette[0x08], 0x23);
  EXPECT_EQ(chip.state.palette[0x0C], 0x24);
  EXPECT_EQ(chip.state.palette[0x10], 0x00);
  EXPECT_EQ(chip.state.palette[0x11], 0x31);
  chip.address(0x3F00);
  chip.write(3, 0x00);
  EXPECT_EQ(chip.read(7), 0x21);
  chip.address(0x3F0C);
  chip.write(3, 0x00);
  EXPECT_EQ(chip.read(7), 0x24);
  chip.address(0x3F04);
  chip.write(7, 0x05);
  chip.address(0x3F14);
  chip.write(3, 0x00);
  EXPECT_EQ(chip.read(7), 0x05);
  chip.address(0x3F30);
  chip.write(7, 0x26);
  EXPECT_EQ(chip.state.palette[0x00], 0x26);
}

TEST(PpuPorts, TheBusDecaysBitByBitAfterTheCount) {
  // NESdev forum, "Tests for ACE speedruns that rely on PPU's internal open bus"; NESdev
  // wiki, "Open bus behavior", PPU open bus: a read of $2002 drives bits 7-5 alone.
  Chip chip;
  chip.inputs.busDecayDots = 10;
  const auto tick = [&chip](int dots) {
    for (int i = 0; i < dots; ++i) ppuTickDot(chip.state, false);
  };
  chip.write(5, 0xFF);
  EXPECT_EQ(chip.state.ioLatchDecay[0], 10u);
  EXPECT_EQ(chip.state.ioLatchDecay[7], 10u);
  tick(9);
  EXPECT_EQ(chip.read(0), 0xFF);
  tick(1);
  EXPECT_EQ(chip.read(0), 0x00);
  EXPECT_EQ(chip.state.ioLatchDecay[0], 0u);

  chip.write(3, 0xFF);
  tick(5);
  chip.state.vblank = true;
  EXPECT_EQ(chip.read(2), 0x9F);
  EXPECT_EQ(chip.state.ioLatchDecay[7], 10u);
  EXPECT_EQ(chip.state.ioLatchDecay[4], 5u);
  tick(5);
  EXPECT_EQ(chip.read(0), 0x80);
  tick(5);
  EXPECT_EQ(chip.read(0), 0x00);

  chip.write(5, 0xAA);
  tick(5);
  EXPECT_EQ(chip.read(0), 0xAA);  // a read of a write-only port drives nothing
  tick(5);
  EXPECT_EQ(chip.read(0), 0x00);

  chip.inputs.busDecayDots = 0;
  chip.write(5, 0x5A);
  tick(1000);
  EXPECT_EQ(chip.read(0), 0x5A);
}

TEST(PpuPorts, OamWritesStepTheAddressAndReadsDoNot) {
  // NESdev wiki, "PPU registers", OAMADDR and OAMDATA.
  Chip chip;
  chip.write(3, 0x10);
  chip.write(4, 0xAA);
  EXPECT_EQ(chip.state.oam[0x10], 0xAA);
  EXPECT_EQ(chip.state.oamAddr, 0x11);
  chip.write(4, 0xBB);
  EXPECT_EQ(chip.state.oam[0x11], 0xBB);
  EXPECT_EQ(chip.state.oamAddr, 0x12);
  chip.write(3, 0x10);
  EXPECT_EQ(chip.read(4), 0xAA);
  EXPECT_EQ(chip.state.oamAddr, 0x10);
  EXPECT_EQ(chip.state.ioLatch, 0xAA);
  EXPECT_EQ(chip.read(4), 0xAA);
  chip.write(3, 0xFF);
  chip.write(4, 0xCC);
  EXPECT_EQ(chip.state.oam[0xFF], 0xCC);
  EXPECT_EQ(chip.state.oamAddr, 0x00);
}

TEST(PpuPorts, AStatusReadLoadsTheBusesTopThreeBits) {
  // NESdev wiki, "Open bus behavior", PPU open bus; "NMI", Operation; "PPU scrolling",
  // $2002 read.
  Chip chip;
  chip.write(3, 0x5F);
  chip.state.vblank = true;
  chip.state.w = true;
  EXPECT_EQ(chip.read(2), 0x9F);
  EXPECT_FALSE(chip.state.vblank);
  EXPECT_FALSE(chip.state.w);
  EXPECT_EQ(chip.state.ioLatch, 0x9F);
  EXPECT_EQ(chip.read(2), 0x1F);
  EXPECT_EQ(chip.state.ioLatch, 0x1F);
}

TEST(PpuPorts, TheWarmUpIgnoresFourWritesAndHoldsTheReadBuffer) {
  // NESdev wiki, "PPU power up state", the notes on the internal reset signal; "PPU
  // registers", the opening paragraph.
  Chip chip;
  chip.state.warmingUp = true;
  chip.write(0, 0x80);
  EXPECT_EQ(chip.state.ctrl, 0);
  EXPECT_EQ(chip.state.t, 0);
  EXPECT_EQ(chip.state.ioLatch, 0x80);
  chip.write(1, 0x1E);
  EXPECT_EQ(chip.state.mask, 0);
  chip.write(5, 0x7D);
  EXPECT_EQ(chip.state.t, 0);
  EXPECT_EQ(chip.state.x, 0);
  EXPECT_FALSE(chip.state.w);
  chip.write(6, 0x21);
  EXPECT_EQ(chip.state.t, 0);
  EXPECT_EQ(chip.state.v, 0);
  EXPECT_FALSE(chip.state.w);
  chip.write(3, 0x10);
  EXPECT_EQ(chip.state.oamAddr, 0x10);
  chip.write(4, 0x99);
  EXPECT_EQ(chip.state.oam[0x10], 0x99);
  chip.cartridge.chrRom[0x10] = 0xA1;
  chip.state.v = 0x0010;
  EXPECT_EQ(chip.read(7), 0);
  EXPECT_EQ(chip.state.readBuffer, 0);
  EXPECT_EQ(chip.state.v, 0x0011);
  EXPECT_EQ(chip.read(7), 0);
  chip.state.v = 0x2000;
  chip.write(7, 0x42);
  EXPECT_EQ(chip.state.ciram[0], 0x42);
  chip.state.warmingUp = false;
  chip.write(0, 0x80);
  EXPECT_EQ(chip.state.ctrl, 0x80);
  chip.state.v = 0x0010;
  EXPECT_EQ(chip.read(7), 0);
  EXPECT_EQ(chip.state.readBuffer, 0xA1);
}

TEST(PpuPorts, PeekMovesNothing) {
  Chip chip;
  chip.cartridge.chrRom[0x10] = 0xA1;
  chip.state.vblank = true;
  chip.state.w = true;
  chip.state.ioLatch = 0x1A;
  chip.state.oamAddr = 3;
  chip.state.oam[3] = 0x77;
  chip.state.v = 0x0010;
  chip.state.readBuffer = 0x33;
  EXPECT_EQ(ppuPeekRegister(chip.state, 2), 0x9A);
  EXPECT_TRUE(chip.state.vblank);
  EXPECT_TRUE(chip.state.w);
  EXPECT_EQ(chip.state.ioLatch, 0x1A);
  EXPECT_EQ(ppuPeekRegister(chip.state, 4), 0x77);
  EXPECT_EQ(chip.state.oamAddr, 3);
  EXPECT_EQ(ppuPeekRegister(chip.state, 7), 0x33);
  EXPECT_EQ(chip.state.v, 0x0010);
  EXPECT_EQ(chip.state.readBuffer, 0x33);
  EXPECT_EQ(ppuPeekRegister(chip.state, 0), 0x1A);
  EXPECT_EQ(ppuPeekRegister(chip.state, 6), 0x1A);
  chip.state.palette[0x05] = 0x2C;
  chip.state.v = 0x3F05;
  chip.state.ioLatch = 0xC3;
  EXPECT_EQ(ppuPeekRegister(chip.state, 7), 0xEC);
  EXPECT_EQ(chip.state.v, 0x3F05);
}

TEST(PpuPorts, ResetKeepsWhatItKeeps) {
  // NESdev wiki, "PPU power up state", Initial Register Values, the After Reset column
  // and the note on the internal reset signal.
  Chip chip;
  PpuState& p = chip.state;
  p.ctrl = 0x83;
  p.mask = 0x1E;
  p.vblank = true;
  p.oamAddr = 0x12;
  p.v = 0x2108;
  p.t = 0x3DF0;
  p.x = 5;
  p.w = true;
  p.readBuffer = 0x44;
  p.ioLatch = 0x55;
  p.warmingUp = false;
  p.dot = 100;
  p.scanline = 50;
  p.odd = true;
  p.ciram[0] = 1;
  p.palette[0] = 2;
  p.oam[0] = 3;
  ppuReset(p);
  EXPECT_EQ(p.ctrl, 0);
  EXPECT_EQ(p.mask, 0);
  EXPECT_TRUE(p.vblank);
  EXPECT_EQ(p.oamAddr, 0x12);
  EXPECT_EQ(p.v, 0x2108);
  EXPECT_EQ(p.t, 0);
  EXPECT_EQ(p.x, 0);
  EXPECT_FALSE(p.w);
  EXPECT_EQ(p.readBuffer, 0);
  EXPECT_EQ(p.ioLatch, 0x55);
  EXPECT_TRUE(p.warmingUp);
  EXPECT_EQ(p.dot, 0);
  EXPECT_EQ(p.scanline, 0);
  EXPECT_FALSE(p.odd);
  EXPECT_EQ(p.ciram[0], 1);
  EXPECT_EQ(p.palette[0], 2);
  EXPECT_EQ(p.oam[0], 3);
}

// ---- PpuFrame ----

TEST(PpuFrame, TheFlagSetsAtDot1OfScanline241AndClearsAtDot1OfScanline261) {
  // NESdev wiki, "NMI", Operation; "PPU frame timing", VBL Flag Timing: 6820 dots
  // apart; "PPU power up state": the clear ends the warm-up.
  PpuState p;
  const auto tick = [&p](int dots) {
    for (int i = 0; i < dots; ++i) ppuTickDot(p, false);
  };
  tick(82181);
  EXPECT_EQ(p.scanline, 241);
  EXPECT_EQ(p.dot, 0);
  EXPECT_FALSE(p.vblank);
  EXPECT_FALSE(ppuTickDot(p, false));
  EXPECT_EQ(p.dot, 1);
  EXPECT_TRUE(p.vblank);
  EXPECT_TRUE(p.warmingUp);
  tick(6819);
  EXPECT_EQ(p.scanline, 261);
  EXPECT_EQ(p.dot, 0);
  EXPECT_TRUE(p.vblank);
  EXPECT_FALSE(ppuTickDot(p, false));
  EXPECT_EQ(p.dot, 1);
  EXPECT_FALSE(p.vblank);
  EXPECT_FALSE(p.warmingUp);
  tick(339);
  EXPECT_EQ(p.scanline, 261);
  EXPECT_EQ(p.dot, 340);
  EXPECT_TRUE(ppuTickDot(p, false));
  EXPECT_EQ(p.scanline, 0);
  EXPECT_EQ(p.dot, 0);
  EXPECT_TRUE(p.odd);
}

TEST(PpuFrame, AStatusReadUnderWayBlocksTheSet) {
  // NESdev wiki, "PPU frame timing", VBL Flag Timing: a read one dot before the set
  // reads the flag clear and the flag is never set that frame.
  PpuState p;
  for (int i = 0; i < 82181; ++i) ppuTickDot(p, false);
  EXPECT_FALSE(ppuTickDot(p, true));
  EXPECT_EQ(p.scanline, 241);
  EXPECT_EQ(p.dot, 1);
  EXPECT_FALSE(p.vblank);
  for (int i = 0; i < 6820; ++i) ppuTickDot(p, false);
  EXPECT_EQ(p.scanline, 261);
  EXPECT_EQ(p.dot, 1);
  EXPECT_FALSE(p.vblank);
}

TEST(PpuFrame, AnOddFrameSkipsADotWithRenderingEnabled) {
  // NESdev wiki, "PPU frame timing", Even/Odd Frames; "PPU registers", PPUMASK,
  // Rendering control: either enable bit enables rendering.
  PpuState p;
  p.warmingUp = false;
  const auto frame = [&p] {
    int dots = 1;
    while (!ppuTickDot(p, false)) ++dots;
    return dots;
  };
  p.mask = 0x08;
  EXPECT_EQ(frame(), 89342);
  EXPECT_TRUE(p.odd);
  EXPECT_EQ(frame(), 89341);
  EXPECT_FALSE(p.odd);
  EXPECT_EQ(frame(), 89342);
  p.mask = 0x00;
  EXPECT_EQ(frame(), 89342);
  EXPECT_FALSE(p.odd);
  p.mask = 0x10;
  EXPECT_EQ(frame(), 89342);
  EXPECT_EQ(frame(), 89341);
  p.mask = 0x18;
  EXPECT_EQ(frame(), 89342);
  EXPECT_EQ(frame(), 89341);
}

TEST(PpuFrame, TheMachineSetsAndClearsTheFlagOnTheDocumentedCycles) {
  // NESdev wiki, "NMI", Operation; "PPU frame timing", VBL Flag Timing and CPU-PPU
  // Clock Alignment; "PPU power up state".
  const Bytes image = imageWith(kJmpLoop);
  for (std::uint8_t a = 0; a < 4; ++a) {
    Nes m(NesConfig{.rom = image, .alignment = a, .vblankAtPowerOn = false});
    EXPECT_EQ(m.run(27393u * 12u), 27393u * 12u);
    EXPECT_FALSE(m.state().ppu.vblank);
    EXPECT_TRUE(m.state().ppu.warmingUp);
    EXPECT_EQ(m.step(), 12u);
    EXPECT_TRUE(m.state().ppu.vblank);
    EXPECT_FALSE(m.state().cpu.nmiPending);
    EXPECT_EQ(m.state().master, 27394u * 12u);
    EXPECT_EQ(m.run((29667u - 27394u) * 12u), (29667u - 27394u) * 12u);
    EXPECT_TRUE(m.state().ppu.vblank);
    EXPECT_TRUE(m.state().ppu.warmingUp);
    EXPECT_EQ(m.step(), 12u);
    EXPECT_FALSE(m.state().ppu.vblank);
    EXPECT_FALSE(m.state().ppu.warmingUp);
    EXPECT_EQ(m.state().master, 29668u * 12u);
  }
}

TEST(PpuFrame, TheWarmUpEndsAtTheFirstPreRenderLine) {
  // NESdev wiki, "PPU power up state"; "PPU registers", the opening paragraph.
  const Bytes image = imageWith(kWriteControlAndMask);
  Nes m(NesConfig{.rom = image});
  EXPECT_EQ(m.run(29667u * 12u), 29667u * 12u);
  EXPECT_EQ(m.state().ppu.ctrl, 0);
  EXPECT_EQ(m.state().ppu.mask, 0);
  EXPECT_TRUE(m.state().ppu.warmingUp);
  EXPECT_EQ(m.run(15u * 12u), 15u * 12u);
  EXPECT_EQ(m.state().ppu.ctrl, 0x80);
  EXPECT_EQ(m.state().ppu.mask, 0x1E);
  EXPECT_FALSE(m.state().ppu.warmingUp);
}

TEST(PpuFrame, TheMachineDecaysTheBusAfterTheConfiguredMasterCycles) {
  // NESdev forum, "Tests for ACE speedruns that rely on PPU's internal open bus". The
  // write lands in cycle 12 after the dots at master 144 and 148; 480 master cycles are
  // 120 dots, the 120th of which is the dot at master 628, in cycle 52, whose last dot
  // at 632 is the 121st.
  const Bytes image = imageWith(kDriveBusOnce);
  Nes m(NesConfig{.rom = image, .ppuOpenBusDecay = 480});
  EXPECT_EQ(m.ppuOpenBusDecay(), 480u);
  stepCycles(m, 52);
  EXPECT_EQ(m.state().ppu.ioLatch, 0xAA);
  EXPECT_EQ(m.state().ppu.ioLatchDecay[0], 2u);
  m.step();
  EXPECT_EQ(m.state().ppu.ioLatch, 0x00);
  EXPECT_EQ(m.state().ppu.ioLatchDecay[0], 0u);

  Nes d(NesConfig{.rom = image});
  EXPECT_EQ(d.ppuOpenBusDecay(), 460800u);
  stepCycles(d, 53);
  EXPECT_EQ(d.state().ppu.ioLatch, 0xAA);
  EXPECT_EQ(d.state().ppu.ioLatchDecay[0], 115200u - 121u);
}

TEST(PpuFrame, TheFrameEndsInTheCycleTheCounterWrapsIn) {
  // NESdev wiki, "PPU frame timing", Even/Odd Frames and CPU-PPU Clock Alignment.
  const Bytes image = imageWith(kJmpLoop);
  for (std::uint8_t a = 0; a < 4; ++a) {
    Nes m(NesConfig{.rom = image, .alignment = a});
    EXPECT_EQ(m.run(29780u * 12u), 29780u * 12u);
    EXPECT_EQ(m.state().ppu.scanline, 261);
    EXPECT_EQ(m.state().ppu.dot, 339);
    EXPECT_FALSE(m.state().ppu.odd);
    EXPECT_EQ(m.step(), 12u);
    EXPECT_EQ(m.state().ppu.scanline, 0);
    EXPECT_EQ(m.state().ppu.dot, 1);
    EXPECT_TRUE(m.state().ppu.odd);
  }
}

// ---- PpuNmi ----

TEST(PpuNmi, TheOutputFollowsTheFlagAndTheEnableBit) {
  // NESdev wiki, "NMI", Operation.
  PpuState p;
  EXPECT_FALSE(ppuNmiAsserted(p));
  p.vblank = true;
  EXPECT_FALSE(ppuNmiAsserted(p));
  p.ctrl = 0x80;
  EXPECT_TRUE(ppuNmiAsserted(p));
  p.vblank = false;
  EXPECT_FALSE(ppuNmiAsserted(p));
  p.ctrl = 0x7F;
  p.vblank = true;
  EXPECT_FALSE(ppuNmiAsserted(p));
}

TEST(PpuNmi, TheInterruptIsTakenAfterTheInstructionTheFlagSetsIn) {
  // NESdev wiki, "NMI", Operation; "CPU interrupts", Detailed interrupt behavior; "PPU
  // frame timing", VBL Flag Timing.
  const Bytes image = imageWith(kJmpLoop);
  for (const std::uint8_t a : std::initializer_list<std::uint8_t>{0, 3}) {
    Nes m = prepared(image, a, [](NesState& s) { s.ppu.ctrl = 0x80; });
    const Cpu6502State& cpu = m.state().cpu;
    EXPECT_EQ(m.run((27393u - 7u) * 12u), (27393u - 7u) * 12u);
    EXPECT_EQ(m.state().master, 27393u * 12u);
    EXPECT_FALSE(m.state().ppu.vblank);
    EXPECT_FALSE(cpu.nmiPending);
    EXPECT_EQ(cpu.tcu, 2);
    EXPECT_EQ(m.step(), 12u);  // the flag sets in a late dot of the JMP's last cycle
    EXPECT_TRUE(m.state().ppu.vblank);
    EXPECT_TRUE(cpu.nmiLine);
    EXPECT_TRUE(cpu.nmiPending);
    EXPECT_EQ(cpu.tcu, 0);
    EXPECT_TRUE(cpu.servicing == CpuSequence::None);
    stepCycles(m, 3);  // the next JMP runs whole; its polls find the request
    EXPECT_EQ(cpu.tcu, 0);
    EXPECT_EQ(cpu.pc, 0x8000);
    EXPECT_TRUE(cpu.polled == CpuSequence::Nmi);
    EXPECT_TRUE(cpu.servicing == CpuSequence::None);
    EXPECT_EQ(m.step(), 12u);  // the sequence begins
    EXPECT_TRUE(cpu.servicing == CpuSequence::Nmi);
    EXPECT_FALSE(cpu.nmiPending);
    EXPECT_EQ(cpu.tcu, 1);
    stepCycles(m, 6);
    EXPECT_EQ(cpu.pc, 0x9000);
    EXPECT_EQ(cpu.tcu, 0);
    EXPECT_EQ(cpu.s, 0xFA);
    EXPECT_TRUE(cpu.servicing == CpuSequence::None);
    EXPECT_EQ(m.state().master, 27404u * 12u);
  }
}

TEST(PpuNmi, TogglingTheEnableInsideVblankPullsTheLineAgain) {
  // NESdev wiki, "NMI", Operation; "PPU registers", Vblank NMI.
  const Bytes image = imageWith(kToggleNmi);
  Nes m = prepared(image, 0, [](NesState& s) {
    s.ppu.vblank = true;
    s.ppu.warmingUp = false;
    s.ppu.scanline = 242;
    s.ppu.dot = 0;
  });
  const Cpu6502State& cpu = m.state().cpu;
  stepCycles(m, 6);  // LDA #$80; STA $2000, the write in the last cycle
  EXPECT_EQ(m.state().ppu.ctrl, 0x80);
  EXPECT_TRUE(cpu.nmiLine);
  EXPECT_TRUE(cpu.nmiPending);
  stepCycles(m, 9);  // LDA #$00, then the sequence
  EXPECT_EQ(cpu.pc, 0x9000);
  EXPECT_EQ(cpu.tcu, 0);
  EXPECT_FALSE(cpu.nmiPending);
  EXPECT_TRUE(cpu.nmiLine);
  stepCycles(m, 10);  // RTI, then STA $2000 with A zero
  EXPECT_EQ(cpu.pc, 0x800A);
  EXPECT_EQ(m.state().ppu.ctrl, 0);
  EXPECT_FALSE(cpu.nmiLine);
  EXPECT_FALSE(cpu.nmiPending);
  EXPECT_TRUE(m.state().ppu.vblank);
  stepCycles(m, 6);  // LDA #$80; STA $2000 again
  EXPECT_TRUE(cpu.nmiLine);
  EXPECT_TRUE(cpu.nmiPending);
  stepCycles(m, 10);  // JMP, then the sequence
  EXPECT_EQ(cpu.pc, 0x9000);
  EXPECT_EQ(cpu.s, 0xFA);
  EXPECT_EQ(m.state().master, 48u * 12u);
}

TEST(PpuNmi, TheStatusReadRaceUnderEachAlignment) {
  // NESdev wiki, "PPU frame timing", VBL Flag Timing and CPU-PPU Clock Alignment;
  // "NMI", Race condition; "PPU registers", Vblank flag; "CPU pinout", M2: the read
  // falls while M2 is high, from four and a half master cycles into the cycle. The read
  // of $2002 is cycle 10 from power-on; the flag's dot is placed k dots after the state
  // is restored, so it ticks at master 80 + a + 4k: in cycle 9's late dots for k = 9,
  // before the read for k = 10 and, under alignment 0 alone, for k = 11, inside the
  // read for k = 12 and for k = 11 under the other alignments, and in cycle 11 for
  // k = 13.
  const Bytes image = imageWith(kReadStatus);
  for (std::uint8_t a = 0; a < 4; ++a) {
    for (int k = 9; k <= 13; ++k) {
      Nes m = prepared(image, a, [k](NesState& s) {
        s.ppu.ctrl = 0x80;
        s.ppu.scanline = 240;
        s.ppu.dot = static_cast<std::uint16_t>(342 - k);
      });
      const Cpu6502State& cpu = m.state().cpu;
      stepCycles(m, 3);  // the opcode and the two operand bytes
      EXPECT_EQ(m.state().ppu.vblank, k == 9);
      EXPECT_EQ(cpu.nmiPending, k == 9);
      EXPECT_EQ(m.step(), 12u);  // the read
      const bool seen = k <= 10 || (k == 11 && a == 0);
      EXPECT_EQ(cpu.a, seen ? 0x80 : 0x00);
      EXPECT_EQ(m.state().ppu.ioLatch, seen ? 0x80 : 0x00);
      EXPECT_FALSE(m.state().ppu.vblank);
      EXPECT_FALSE(cpu.nmiLine);
      EXPECT_EQ(cpu.nmiPending, k == 9);
      EXPECT_EQ(m.step(), 12u);  // the next instruction's first cycle
      if (k == 9) {
        EXPECT_TRUE(cpu.servicing == CpuSequence::Nmi);
        EXPECT_FALSE(cpu.nmiPending);
      } else if (k == 13) {
        EXPECT_TRUE(m.state().ppu.vblank);
        EXPECT_TRUE(cpu.nmiLine);
        EXPECT_TRUE(cpu.nmiPending);
      } else {
        EXPECT_FALSE(m.state().ppu.vblank);
        EXPECT_FALSE(cpu.nmiPending);
        EXPECT_TRUE(cpu.servicing == CpuSequence::None);
      }
      m.run(2000u * 12u);
      EXPECT_EQ(m.state().ppu.vblank, k == 13);
    }
  }
}

// ---- PpuPowerOn ----

TEST(PpuPowerOn, PowerOnIsTheDocumentedState) {
  // NESdev wiki, "PPU power up state", Initial Register Values, the At Power column and
  // the notes; "PPU registers", Reading palette RAM: six-bit entries.
  const Bytes image = imageWith({kNop});
  const std::array<std::uint8_t, 3> seed{0x12, 0x34, 0x56};
  const Nes m(NesConfig{.rom = image, .alignment = 2, .ramSeed = seed});
  const PpuState& p = m.state().ppu;
  EXPECT_EQ(p.ctrl, 0);
  EXPECT_EQ(p.mask, 0);
  EXPECT_TRUE(p.vblank);
  EXPECT_EQ(p.oamAddr, 0);
  EXPECT_EQ(p.v, 0);
  EXPECT_EQ(p.t, 0);
  EXPECT_EQ(p.x, 0);
  EXPECT_FALSE(p.w);
  EXPECT_EQ(p.readBuffer, 0);
  EXPECT_EQ(p.ioLatch, 0);
  EXPECT_TRUE(p.warmingUp);
  EXPECT_EQ(p.dot, 0);
  EXPECT_EQ(p.scanline, 0);
  EXPECT_FALSE(p.odd);
  EXPECT_EQ(p.palette[0], 0x12);
  EXPECT_EQ(p.palette[1], 0x34);
  EXPECT_EQ(p.palette[2], 0x16);  // $56 kept to six bits
  EXPECT_EQ(p.palette[31], 0x34);  // 31 mod 3 is 1
  EXPECT_EQ(p.oam[0], 0x12);
  EXPECT_EQ(p.oam[1], 0x34);
  EXPECT_EQ(p.oam[255], 0x12);  // 255 mod 3 is 0

  const Nes z(NesConfig{.rom = image, .vblankAtPowerOn = false});
  const PpuState& zp = z.state().ppu;
  EXPECT_FALSE(zp.vblank);
  const auto zero = [](std::uint8_t byte) { return byte == 0; };
  EXPECT_TRUE(std::all_of(zp.palette.begin(), zp.palette.end(), zero));
  EXPECT_TRUE(std::all_of(zp.oam.begin(), zp.oam.end(), zero));
}

TEST(PpuPowerOn, ResetResetsThePpuWithTheCpu) {
  // NESdev wiki, "PPU power up state", Initial Register Values, the After Reset column,
  // and the note that a front-loading console's Reset button resets both chips.
  const Bytes image = imageWith(kJmpLoop);
  Nes m = prepared(image, 1, [](NesState& s) {
    s.ppu.ctrl = 0x83;
    s.ppu.mask = 0x1E;
    s.ppu.vblank = true;
    s.ppu.oamAddr = 0x12;
    s.ppu.v = 0x2108;
    s.ppu.t = 0x3DF0;
    s.ppu.x = 5;
    s.ppu.w = true;
    s.ppu.readBuffer = 0x44;
    s.ppu.ioLatch = 0x55;
    s.ppu.warmingUp = false;
    s.ppu.scanline = 100;
    s.ppu.dot = 200;
    s.ppu.odd = true;
  });
  EXPECT_EQ(m.run(1000u * 12u), 1000u * 12u);
  const std::uint64_t before = m.state().master;
  m.reset();
  const PpuState& p = m.state().ppu;
  EXPECT_EQ(p.ctrl, 0);
  EXPECT_EQ(p.mask, 0);
  EXPECT_TRUE(p.vblank);
  EXPECT_EQ(p.oamAddr, 0x12);
  EXPECT_EQ(p.v, 0x2108);
  EXPECT_EQ(p.t, 0);
  EXPECT_EQ(p.x, 0);
  EXPECT_FALSE(p.w);
  EXPECT_EQ(p.readBuffer, 0);
  EXPECT_EQ(p.ioLatch, 0x55);
  EXPECT_TRUE(p.warmingUp);
  EXPECT_EQ(p.dot, 0);
  EXPECT_EQ(p.scanline, 0);
  EXPECT_FALSE(p.odd);
  EXPECT_EQ(m.state().master, before);
  stepCycles(m, 7);
  EXPECT_EQ(p.dot, 21);
  EXPECT_EQ(p.scanline, 0);
  EXPECT_EQ(m.state().cpu.pc, 0x8000);
  EXPECT_EQ(m.state().cpu.s, 0xFA);
}

}  // namespace
