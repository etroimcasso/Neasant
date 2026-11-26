#include "neasant/nes/board.h"
#include "neasant/nes/cartridge.h"
#include "neasant/nes/nes.h"
#include "neasant/ppu/ppu.h"
#include "authored_image.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

// The pins to the board. On a bare chip: the address lines following `v` while the chip
// makes no access, the console's latch keeping its byte; a $2007 read and a $2007 write
// taken in the dot under way and made on the chip's own dots, the address on the third
// and the strobe on the fifth, with the read buffer and `v` moving there; the palette's
// write driving its address and no strobe and its read reading the byte underneath; two
// reads or two writes three dots apart, the first's strobe a dot early and the second
// dropped when the access falls in the dot's last master cycle; a read and a write three
// dots apart keeping their dots; an access as a dot begins counting a dot more; the
// warm-up holding the buffer at zero; reset dropping an access on its way; the lines
// carrying `v` on the pre-render line's dot 0 and after the post-render line's; address
// line 13 rising 43 times across a rendered line with the latch capturing on dot 0; and
// the pre-render line's sprite fetches taking 261 for the scanline. On the machine: the
// master cycle of the dot an access falls on, from the alignment, read through two reads
// on adjacent cycles, and a snapshot carrying an access on its way. A case that checks a
// hardware rule names the document and section it is read from.

namespace {

using neasant::Board;
using neasant::boardFor;
using neasant::Cartridge;
using neasant::makeCartridge;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::PpuDataAccess;
using neasant::PpuDataAccessKind;
using neasant::PpuDotEvent;
using neasant::PpuInputs;
using neasant::ppuRead;
using neasant::ppuReset;
using neasant::PpuState;
using neasant::ppuTickDot;
using neasant::ppuWrite;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::placeProgram;

using Bytes = std::vector<std::uint8_t>;
using Program = std::initializer_list<std::uint8_t>;

constexpr PpuDataAccess kNoAccess{};

// A bare chip over an NROM board with 16 KB of PRG ROM and 8 KB of CHR ROM or of CHR RAM
// under horizontal mirroring, its warm-up window closed as it is after the first
// pre-render line; its accesses fall on the dot's master cycle 1 until a case says
// otherwise.
struct Chip {
  Bytes image;
  Cartridge cartridge;
  Board board;
  std::vector<std::uint8_t> chrRam;
  PpuState state;
  PpuInputs inputs;

  explicit Chip(bool withChrRam = false)
      : image(authoredImage(
            inesHeader({.chrUnits = withChrRam ? std::uint8_t{0} : std::uint8_t{1}}), 16384,
            withChrRam ? 0 : 8192)),
        cartridge(makeCartridge(image)),
        board(boardFor(cartridge)),
        chrRam(cartridge.chrRamBytes),
        inputs{board, cartridge, chrRam, 115200u} {
    state.warmingUp = false;
  }

  void write(std::uint8_t port, std::uint8_t value) { ppuWrite(state, port, value, inputs); }
  std::uint8_t read(std::uint8_t port) { return ppuRead(state, port, inputs); }
  PpuDotEvent tick() { return ppuTickDot(state, false, inputs); }
  void tick(int dots) {
    for (int dot = 0; dot < dots; ++dot) tick();
  }
  // Sets the current address through the two writes to $2006.
  void address(std::uint16_t v) {
    write(6, static_cast<std::uint8_t>(v >> 8));
    write(6, static_cast<std::uint8_t>(v & 0xFFu));
  }
  [[nodiscard]] PpuDataAccess& held(std::size_t slot = 0) { return state.dataAccess[slot]; }
  // Puts the counter at `scanline`, `dot`: the next tick is the dot after it.
  void at(std::uint16_t scanline, std::uint16_t dot) {
    state.scanline = scanline;
    state.dot = dot;
  }
};

// A $2007 access as the chip holds it.
PpuDataAccess access(PpuDataAccessKind kind, std::uint8_t data, std::uint8_t toAddress,
                     std::uint8_t toStrobe) {
  return PpuDataAccess{
      .kind = kind, .data = data, .dotsToAddress = toAddress, .dotsToStrobe = toStrobe};
}

constexpr auto kRead = PpuDataAccessKind::Read;
constexpr auto kWrite = PpuDataAccessKind::Write;

// Puts a sprite's four bytes into slot `slot` of secondary OAM.
void setSlot(PpuState& state, std::size_t slot, std::uint8_t y, std::uint8_t tile,
             std::uint8_t attribute, std::uint8_t x) {
  state.secondaryOam[4 * slot] = y;
  state.secondaryOam[4 * slot + 1] = tile;
  state.secondaryOam[4 * slot + 2] = attribute;
  state.secondaryOam[4 * slot + 3] = x;
}

// A 16 KB NROM image with 8 KB of CHR RAM, `program` at $8000 and every vector pointing
// there.
Bytes imageWith(Program program) {
  Bytes image = authoredImage(inesHeader({.chrUnits = 0}), 16384, 0);
  placeProgram(image, 0, std::span<const std::uint8_t>(program.begin(), program.size()));
  return image;
}

// Steps `cycles` machine cycles.
void stepCycles(Nes& machine, int cycles) {
  for (int i = 0; i < cycles; ++i) machine.step();
}

// A machine over `image` built with `alignment`, stepped through its reset sequence and
// its PPU's warm-up window closed through a restore.
Nes prepared(const Bytes& image, std::uint8_t alignment) {
  Nes machine(NesConfig{.rom = image, .alignment = alignment});
  stepCycles(machine, 7);
  NesState state = machine.state();
  state.ppu.warmingUp = false;
  machine.restore(state);
  return machine;
}

// Fills seven bytes of the pattern table with $11 to $77 through $2007, sets the address
// to 1, primes the buffer, then reads $2007 on two adjacent cycles with LDA $20F7,X and
// four more times, keeping each value at $00-$04.
constexpr Program kDoubleRead = {
    0xA9, 0x00, 0x8D, 0x06, 0x20, 0x8D, 0x06, 0x20,  // LDA #$00; STA $2006; STA $2006
    0xA9, 0x11, 0x8D, 0x07, 0x20,                    // LDA #$11; STA $2007
    0xA9, 0x22, 0x8D, 0x07, 0x20,                    // LDA #$22; STA $2007
    0xA9, 0x33, 0x8D, 0x07, 0x20,                    // LDA #$33; STA $2007
    0xA9, 0x44, 0x8D, 0x07, 0x20,                    // LDA #$44; STA $2007
    0xA9, 0x55, 0x8D, 0x07, 0x20,                    // LDA #$55; STA $2007
    0xA9, 0x66, 0x8D, 0x07, 0x20,                    // LDA #$66; STA $2007
    0xA9, 0x77, 0x8D, 0x07, 0x20,                    // LDA #$77; STA $2007
    0xA9, 0x00, 0x8D, 0x06, 0x20,                    // LDA #$00; STA $2006
    0xA9, 0x01, 0x8D, 0x06, 0x20,                    // LDA #$01; STA $2006
    0xAD, 0x07, 0x20,                                // LDA $2007
    0xA2, 0x10,                                      // LDX #$10
    0xBD, 0xF7, 0x20,                                // LDA $20F7,X
    0x85, 0x00,                                      // STA $00
    0xAD, 0x07, 0x20, 0x85, 0x01,                    // LDA $2007; STA $01
    0xAD, 0x07, 0x20, 0x85, 0x02,                    // LDA $2007; STA $02
    0xAD, 0x07, 0x20, 0x85, 0x03,                    // LDA $2007; STA $03
    0xAD, 0x07, 0x20, 0x85, 0x04,                    // LDA $2007; STA $04
    0x4C, 0x53, 0x80,                                // JMP $8053
};

// LDA #$00; STA $2006; STA $2006; LDA #$5A; STA $2007; JMP $800D
constexpr Program kWriteOnce = {0xA9, 0x00, 0x8D, 0x06, 0x20, 0x8D, 0x06, 0x20,
                                0xA9, 0x5A, 0x8D, 0x07, 0x20, 0x4C, 0x0D, 0x80};

}  // namespace

// NESdev wiki, "PPU rendering", PPU address bus contents: with rendering disabled the
// address bus carries v; "PPU pinout", Signal descriptions: ALE latches an access's low
// byte. Visual 2C02: the lines take a written v on the next dot with no ALE.
TEST(PpuBus, TheLinesFollowVWhileTheChipMakesNoAccessAndTheLatchKeepsItsByte) {
  Chip chip;
  chip.state.addressLatch = 0xEE;
  chip.address(0x2A5C);
  EXPECT_EQ(chip.state.addressPins, 0);
  chip.tick();
  EXPECT_EQ(chip.state.addressPins, 0x2A5C);
  EXPECT_EQ(chip.state.addressLatch, 0xEE);
  chip.state.v = 0x7FFF;
  chip.tick();
  EXPECT_EQ(chip.state.addressPins, 0x3FFF);
  EXPECT_EQ(chip.state.addressLatch, 0xEE);
  chip.at(241, 100);
  chip.state.v = 0x1234;
  chip.tick();
  EXPECT_EQ(chip.state.addressPins, 0x1234);
  EXPECT_EQ(chip.held(), kNoAccess);
}

// NESdev wiki, "PPU registers", The PPUDATA read buffer; "PPU rendering", PPU address bus
// contents. Visual 2C02: ALE three dots after the port select falls, /RD two dots after
// that, the buffer loading and v stepping on the /RD dot.
TEST(PpuBus, AReadIsTakenInTheDotUnderWayAndMadeOnItsThirdAndFifthDots) {
  Chip chip;
  chip.cartridge.chrRom[0x10] = 0xA1;
  chip.address(0x0010);
  chip.tick();
  chip.state.addressLatch = 0xEE;
  chip.state.readBuffer = 0x5A;
  EXPECT_EQ(chip.read(7), 0x5A);
  EXPECT_EQ(chip.held(), access(kRead, 0, 3, 5));
  EXPECT_EQ(chip.state.readBuffer, 0x5A);
  EXPECT_EQ(chip.state.v, 0x0010);
  chip.tick();
  EXPECT_EQ(chip.held(), access(kRead, 0, 2, 4));
  EXPECT_EQ(chip.state.addressLatch, 0xEE);
  chip.tick();
  EXPECT_EQ(chip.held(), access(kRead, 0, 1, 3));
  chip.tick();  // the address dot
  EXPECT_EQ(chip.held(), access(kRead, 0, 0, 2));
  EXPECT_EQ(chip.state.addressPins, 0x0010);
  EXPECT_EQ(chip.state.addressLatch, 0x10);
  chip.tick();
  EXPECT_EQ(chip.held(), access(kRead, 0, 0, 1));
  EXPECT_EQ(chip.state.readBuffer, 0x5A);
  EXPECT_EQ(chip.state.v, 0x0010);
  chip.tick();  // the strobe dot
  EXPECT_EQ(chip.held(), kNoAccess);
  EXPECT_EQ(chip.state.readBuffer, 0xA1);
  EXPECT_EQ(chip.state.v, 0x0011);
  EXPECT_EQ(chip.state.addressPins, 0x0010);
  chip.tick();
  EXPECT_EQ(chip.state.addressPins, 0x0011);
  EXPECT_EQ(chip.state.addressLatch, 0x10);
}

// NESdev wiki, "PPU registers", PPUDATA; "NROM", Banks; "PPU memory map", Hardware
// mapping. Visual 2C02: /WR across the fourth and fifth dots after the select, the byte
// on the lines and v stepping on the fifth.
TEST(PpuBus, AWriteLandsOnItsStrobeDotAndStepsV) {
  Chip chip(true);
  chip.address(0x0123);
  chip.tick();
  chip.write(7, 0x5A);
  EXPECT_EQ(chip.state.ioLatch, 0x5A);
  EXPECT_EQ(chip.held(), access(kWrite, 0x5A, 3, 5));
  chip.tick(4);
  EXPECT_EQ(chip.chrRam[0x123], 0);
  EXPECT_EQ(chip.state.v, 0x0123);
  EXPECT_EQ(chip.state.addressLatch, 0x23);
  chip.tick();
  EXPECT_EQ(chip.chrRam[0x123], 0x5A);
  EXPECT_EQ(chip.state.v, 0x0124);
  EXPECT_EQ(chip.held(), kNoAccess);
  chip.address(0x2000);
  chip.tick();
  chip.write(7, 0x77);
  chip.tick(4);
  EXPECT_EQ(chip.state.ciram[0], 0);
  chip.tick();
  EXPECT_EQ(chip.state.ciram[0], 0x77);
  EXPECT_EQ(chip.state.v, 0x2001);
}

// NESdev wiki, "PPU pinout", Signal descriptions: a write to the palette range does not
// assert /WR; "PPU registers", Reading palette RAM: the read underneath fills the buffer;
// "PPU palettes", Palette RAM. Visual 2C02: ALE with the palette address, /WR high
// throughout a palette write, /RD low at the palette address on a read.
TEST(PpuBus, APaletteWriteDrivesItsAddressAndNoStrobeAndAPaletteReadReadsUnderneath) {
  Chip chip;
  chip.address(0x3F05);
  chip.tick();
  chip.state.addressLatch = 0xEE;
  chip.write(7, 0x3C);
  chip.tick(3);
  EXPECT_EQ(chip.state.addressPins, 0x3F05);
  EXPECT_EQ(chip.state.addressLatch, 0x05);
  EXPECT_EQ(chip.state.palette[5], 0);
  chip.tick(2);
  EXPECT_EQ(chip.state.palette[5], 0x3C);
  EXPECT_EQ(chip.state.ciram[0x705], 0);  // no strobe reached the console's RAM
  EXPECT_EQ(chip.state.v, 0x3F06);
  chip.state.palette[6] = 0x2C;
  chip.state.ciram[0x706] = 0x77;  // the byte underneath $3F06 under horizontal mirroring
  chip.state.ioLatch = 0xC3;
  chip.state.readBuffer = 0x5A;
  EXPECT_EQ(chip.read(7), 0xEC);
  EXPECT_EQ(chip.state.readBuffer, 0x5A);
  chip.tick(5);
  EXPECT_EQ(chip.state.readBuffer, 0x77);
  EXPECT_EQ(chip.state.palette[6], 0x2C);
  EXPECT_EQ(chip.state.v, 0x3F07);
}

// Visual 2C02: a second read whose select falls in the first's ALE dot brings the first's
// /RD to the next dot and follows three and five dots after its own select; blargg's
// double_2007_read: the second read answers the buffer as it stood.
TEST(PpuBus, TwoReadsThreeDotsApartMakeTwoAccessesTheFirstADotEarly) {
  Chip chip;
  chip.cartridge.chrRom[0x10] = 0xA1;
  chip.cartridge.chrRom[0x11] = 0xB2;
  chip.cartridge.chrRom[0x12] = 0xC3;
  chip.address(0x0010);
  chip.tick();
  static_cast<void>(chip.read(7));
  chip.tick(5);
  EXPECT_EQ(chip.state.readBuffer, 0xA1);
  EXPECT_EQ(chip.state.v, 0x0011);
  EXPECT_EQ(chip.read(7), 0xA1);
  chip.tick(3);
  EXPECT_EQ(chip.held(0), access(kRead, 0, 0, 2));
  EXPECT_EQ(chip.read(7), 0xA1);
  EXPECT_EQ(chip.held(0), access(kRead, 0, 0, 1));
  EXPECT_EQ(chip.held(1), access(kRead, 0, 3, 5));
  chip.tick();
  EXPECT_EQ(chip.state.readBuffer, 0xB2);
  EXPECT_EQ(chip.state.v, 0x0012);
  EXPECT_EQ(chip.held(0), kNoAccess);
  chip.tick(2);
  EXPECT_EQ(chip.held(1), access(kRead, 0, 0, 2));
  EXPECT_EQ(chip.state.addressPins, 0x0012);
  EXPECT_EQ(chip.state.addressLatch, 0x12);
  chip.tick(2);
  EXPECT_EQ(chip.state.readBuffer, 0xC3);
  EXPECT_EQ(chip.state.v, 0x0013);
  EXPECT_EQ(chip.held(1), kNoAccess);
}

// Visual 2C02: a second read whose select falls in the third master cycle of the first's
// ALE dot makes no access; blargg's double_2007_read: "sometimes ignores extra read".
TEST(PpuBus, TheSecondOfTwoReadsIsDroppedOnTheDotsLastMasterCycle) {
  Chip chip;
  chip.cartridge.chrRom[0x10] = 0xA1;
  chip.cartridge.chrRom[0x11] = 0xB2;
  chip.cartridge.chrRom[0x12] = 0xC3;
  chip.address(0x0010);
  chip.tick();
  static_cast<void>(chip.read(7));
  chip.tick(5);
  EXPECT_EQ(chip.state.readBuffer, 0xA1);
  EXPECT_EQ(chip.state.v, 0x0011);
  chip.inputs.accessCycle = 3;
  EXPECT_EQ(chip.read(7), 0xA1);
  EXPECT_EQ(chip.held(0), access(kRead, 0, 3, 5));
  chip.tick(3);
  EXPECT_EQ(chip.read(7), 0xA1);
  EXPECT_EQ(chip.held(0), access(kRead, 0, 0, 2));
  EXPECT_EQ(chip.held(1), kNoAccess);
  chip.tick(2);
  EXPECT_EQ(chip.state.readBuffer, 0xB2);
  EXPECT_EQ(chip.state.v, 0x0012);
  chip.tick(5);
  EXPECT_EQ(chip.state.readBuffer, 0xB2);
  EXPECT_EQ(chip.state.v, 0x0012);
  EXPECT_EQ(chip.held(0), kNoAccess);
}

// Visual 2C02: a select falling in a dot's last master cycle is taken in the next dot,
// its ALE and /RD a dot later than otherwise, and a second read in the same relation to
// the first's ALE dot still brings the first's /RD a dot nearer.
TEST(PpuBus, AnAccessOnTheDotsFirstMasterCycleCountsOneDotMore) {
  Chip chip;
  chip.cartridge.chrRom[0x10] = 0xA1;
  chip.cartridge.chrRom[0x11] = 0xB2;
  chip.address(0x0010);
  chip.tick();
  chip.state.addressLatch = 0xEE;
  chip.inputs.accessCycle = 0;
  static_cast<void>(chip.read(7));
  EXPECT_EQ(chip.held(0), access(kRead, 0, 4, 6));
  chip.tick(3);
  EXPECT_EQ(chip.state.addressLatch, 0xEE);
  chip.tick();
  EXPECT_EQ(chip.state.addressLatch, 0x10);
  chip.tick();
  EXPECT_EQ(chip.state.readBuffer, 0);
  chip.tick();
  EXPECT_EQ(chip.state.readBuffer, 0xA1);
  EXPECT_EQ(chip.state.v, 0x0011);
  static_cast<void>(chip.read(7));
  chip.tick(3);
  EXPECT_EQ(chip.held(0), access(kRead, 0, 1, 3));
  EXPECT_EQ(chip.read(7), 0xA1);
  EXPECT_EQ(chip.held(0), access(kRead, 0, 1, 2));
  EXPECT_EQ(chip.held(1), access(kRead, 0, 4, 6));
  chip.tick();
  EXPECT_EQ(chip.held(0), access(kRead, 0, 0, 1));
  EXPECT_EQ(chip.state.addressLatch, 0x11);
  chip.tick();
  EXPECT_EQ(chip.state.readBuffer, 0xB2);
  EXPECT_EQ(chip.state.v, 0x0012);
  EXPECT_EQ(chip.held(0), kNoAccess);
  EXPECT_EQ(chip.held(1), access(kRead, 0, 2, 4));
}

// Visual 2C02: a read and a write on adjacent CPU cycles, in either order, each make
// their access three and five dots after their own select.
TEST(PpuBus, AReadAndAWriteThreeDotsApartKeepTheirDots) {
  Chip chip(true);
  chip.chrRam[0x10] = 0xA1;
  chip.chrRam[0x11] = 0xB2;
  chip.address(0x0010);
  chip.tick();
  static_cast<void>(chip.read(7));
  chip.tick(5);
  EXPECT_EQ(chip.read(7), 0xA1);
  chip.tick(3);
  chip.write(7, 0x99);
  EXPECT_EQ(chip.held(0), access(kRead, 0, 0, 2));
  EXPECT_EQ(chip.held(1), access(kWrite, 0x99, 3, 5));
  chip.tick(2);
  EXPECT_EQ(chip.state.readBuffer, 0xB2);
  EXPECT_EQ(chip.state.v, 0x0012);
  EXPECT_EQ(chip.held(0), kNoAccess);
  chip.tick(3);
  EXPECT_EQ(chip.chrRam[0x12], 0x99);
  EXPECT_EQ(chip.state.v, 0x0013);
  EXPECT_EQ(chip.held(1), kNoAccess);
  chip.write(7, 0x77);
  chip.tick(3);
  EXPECT_EQ(chip.read(7), 0xB2);
  EXPECT_EQ(chip.held(0), access(kWrite, 0x77, 0, 2));
  EXPECT_EQ(chip.held(1), access(kRead, 0, 3, 5));
  chip.tick(2);
  EXPECT_EQ(chip.chrRam[0x13], 0x77);
  EXPECT_EQ(chip.state.v, 0x0014);
  chip.tick(3);
  EXPECT_EQ(chip.state.readBuffer, 0);
  EXPECT_EQ(chip.state.v, 0x0015);
}

// Visual 2C02: two writes on adjacent CPU cycles, the first's /WR a dot early and the
// second on its own dots, or the second dropped when its select falls in the third master
// cycle of the first's ALE dot.
TEST(PpuBus, TwoWritesThreeDotsApartFollowTheReadsRule) {
  Chip chip(true);
  chip.address(0x0020);
  chip.tick();
  chip.write(7, 0x11);
  chip.tick(3);
  chip.write(7, 0x22);
  EXPECT_EQ(chip.held(0), access(kWrite, 0x11, 0, 1));
  EXPECT_EQ(chip.held(1), access(kWrite, 0x22, 3, 5));
  chip.tick();
  EXPECT_EQ(chip.chrRam[0x20], 0x11);
  EXPECT_EQ(chip.state.v, 0x0021);
  chip.tick(4);
  EXPECT_EQ(chip.chrRam[0x21], 0x22);
  EXPECT_EQ(chip.state.v, 0x0022);
  chip.inputs.accessCycle = 3;
  chip.write(7, 0x33);
  chip.tick(3);
  chip.write(7, 0x44);
  EXPECT_EQ(chip.held(1), kNoAccess);
  chip.tick(2);
  EXPECT_EQ(chip.chrRam[0x22], 0x33);
  EXPECT_EQ(chip.state.v, 0x0023);
  chip.tick(5);
  EXPECT_EQ(chip.chrRam[0x23], 0);
  EXPECT_EQ(chip.state.v, 0x0023);
}

// NESdev wiki, "PPU power up state", the notes on the internal reset signal: the read
// buffer is held cleared while it stands; the address steps all the same.
TEST(PpuBus, TheWarmUpHoldsTheBufferAtZeroOnTheStrobeDot) {
  Chip chip;
  chip.cartridge.chrRom[0x10] = 0xA1;
  chip.cartridge.chrRom[0x11] = 0xB2;
  chip.state.warmingUp = true;
  chip.state.v = 0x0010;
  EXPECT_EQ(chip.read(7), 0);
  chip.tick(5);
  EXPECT_EQ(chip.state.readBuffer, 0);
  EXPECT_EQ(chip.state.v, 0x0011);
  chip.state.warmingUp = false;
  EXPECT_EQ(chip.read(7), 0);
  chip.tick(5);
  EXPECT_EQ(chip.state.readBuffer, 0xB2);
  EXPECT_EQ(chip.state.v, 0x0012);
}

// NESdev wiki, "PPU pinout", Signal descriptions: /RST clears the read buffer; the line
// is held for far longer than an access takes.
TEST(PpuBus, ResetDropsAnAccessOnItsWay) {
  Chip chip;
  chip.cartridge.chrRom[0x10] = 0xA1;
  chip.state.v = 0x0010;
  static_cast<void>(chip.read(7));
  EXPECT_EQ(chip.held().kind, kRead);
  ppuReset(chip.state);
  EXPECT_EQ(chip.held(), kNoAccess);
  chip.tick(6);
  EXPECT_EQ(chip.state.readBuffer, 0);
  EXPECT_EQ(chip.state.v, 0x0010);
}

// NESdev wiki, "PPU rendering", PPU address bus contents and Cycle 0, and its frame
// timing diagram: dot 0 of scanlines 0-240 carries the low pattern byte's address and dot
// 0 of the pre-render line nothing of the kind. Visual 2C02: the lines carry v on dot 0
// of the pre-render line with no ALE.
TEST(PpuBus, TheLinesCarryVOnThePreRenderLinesDot0AndAfterThePostRenderLinesDot0) {
  Chip chip;
  chip.state.mask = 0x1E;
  chip.state.v = 0x73A2;
  chip.state.addressPins = 0x1234;
  chip.state.addressLatch = 0xEE;
  chip.at(260, 340);
  chip.tick();  // dot 0 of scanline 261
  EXPECT_EQ(chip.state.addressPins, 0x33A2);
  EXPECT_EQ(chip.state.addressLatch, 0xEE);
  chip.tick();  // dot 1
  EXPECT_EQ(chip.state.addressPins, 0x23A2);
  EXPECT_EQ(chip.state.addressLatch, 0xA2);

  Chip post;
  post.state.mask = 0x1E;
  post.state.v = 0x1234;
  post.state.nametableLatch = 0x4E;
  post.at(239, 340);
  post.tick();  // dot 0 of scanline 240: tile $4E, fine Y 1, the table's low half
  EXPECT_EQ(post.state.addressPins, 0x04E1);
  EXPECT_EQ(post.state.addressLatch, 0xE1);
  post.tick();  // dot 1
  EXPECT_EQ(post.state.addressPins, 0x1234);
  EXPECT_EQ(post.state.addressLatch, 0xE1);
}

// NESdev wiki, "PPU rendering", Line-by-line timing and Cycle 0; Brad Taylor's NTSC 2C02
// technical reference, The MMC3's scanline counter, counts 42. Visual 2C02: 171 ALE
// pulses and 43 rises of address line 13 across a rendered line, the first ALE on dot 0
// with the low pattern byte's address.
TEST(PpuBus, AddressLineThirteenRisesFortyThreeTimesALineAndTheLatchCapturesOnDot0) {
  Chip chip;
  chip.state.mask = 0x1E;
  chip.state.v = 0x0005;
  chip.state.nametableLatch = 0x4E;
  chip.state.addressPins = 0x2000;
  chip.state.addressLatch = 0xEE;
  chip.at(9, 340);
  chip.tick();  // dot 0 of scanline 10
  EXPECT_EQ(chip.state.addressPins, 0x04E0);
  EXPECT_EQ(chip.state.addressLatch, 0xE0);
  // 32 nametable fetches on dots 1-249 after a pattern address, 8 on dots 257-313 after
  // the sprites' pattern fetches, 2 at 321 and 329, and 1 at 337.
  int rises = 0;
  for (int dot = 1; dot <= 340; ++dot) {
    const bool before = (chip.state.addressPins & 0x2000u) != 0;
    chip.tick();
    if (!before && (chip.state.addressPins & 0x2000u) != 0) ++rises;
  }
  EXPECT_EQ(chip.state.dot, 340);
  EXPECT_EQ(rises, 43);
}

// Visual 2C02: on the pre-render line the slots the clear left at $FF and the slot
// holding the last Y looked at, $F0, drive tile $FF with the rows 261 less Y gives,
// flipped by the $FF attribute: $0FF2 and $0FFA for the $F0 slot, $0FF1 and $0FF9 for the
// others. NESdev wiki, "PPU OAM", Byte 2 - Attributes: bit 7 flips vertically.
TEST(PpuBus, ThePreRenderLinesSpriteFetchesTake261ForTheScanline) {
  Chip chip;
  chip.state.mask = 0x1E;
  setSlot(chip.state, 0, 0xF0, 0xFF, 0xFF, 0xFF);
  for (std::size_t slot = 1; slot < 8; ++slot) setSlot(chip.state, slot, 0xFF, 0xFF, 0xFF, 0xFF);
  chip.at(261, 256);
  chip.tick(5);  // dot 261
  EXPECT_EQ(chip.state.addressPins, 0x0FF2);
  chip.tick(2);  // dot 263
  EXPECT_EQ(chip.state.addressPins, 0x0FFA);
  chip.tick(6);  // dot 269
  EXPECT_EQ(chip.state.addressPins, 0x0FF1);
  chip.tick(2);  // dot 271
  EXPECT_EQ(chip.state.addressPins, 0x0FF9);
  EXPECT_EQ(chip.state.spriteUnits[0].pattern, (std::array<std::uint8_t, 2>{0, 0}));
}

// NESdev wiki, "PPU frame timing", CPU-PPU Clock Alignment; "CPU pinout", M2. Visual 2C02
// and blargg's double_2007_read: two reads on adjacent cycles both made under three
// alignments, 22 44 55 66 77, and the second dropped under the fourth, 22 33 44 55 66.
TEST(NesBus, TheMachineSetsAnAccesssCycleByItsAlignment) {
  const Bytes image = imageWith(kDoubleRead);
  for (std::uint8_t alignment = 0; alignment < 4; ++alignment) {
    Nes machine = prepared(image, alignment);
    stepCycles(machine, 130);
    const std::array<std::uint8_t, 5> expected =
        alignment == 2 ? std::array<std::uint8_t, 5>{0x22, 0x33, 0x44, 0x55, 0x66}
                       : std::array<std::uint8_t, 5>{0x22, 0x44, 0x55, 0x66, 0x77};
    for (std::size_t index = 0; index < expected.size(); ++index) {
      EXPECT_EQ(machine.state().ram[index], expected[index])
          << "alignment " << int{alignment} << ", byte " << index;
    }
  }
}

// The machine is one value: a $2007 write taken and not yet made rides in the state and
// lands after a restore as it would have.
TEST(NesBus, ASnapshotCarriesAnAccessOnItsWay) {
  const Bytes image = imageWith(kWriteOnce);
  Nes a = prepared(image, 0);
  stepCycles(a, 16);  // the write's cycle is the sixteenth and its strobe dot two cycles on
  EXPECT_EQ(a.state().ppu.dataAccess[0].kind, kWrite);
  EXPECT_EQ(a.state().chrRam[0], 0);
  Nes b(NesConfig{.rom = image});
  b.restore(a.state());
  stepCycles(a, 3);
  stepCycles(b, 3);
  EXPECT_EQ(a.state().chrRam[0], 0x5A);
  EXPECT_EQ(a.state().ppu.dataAccess[0], kNoAccess);
  EXPECT_EQ(a.state(), b.state());
}
