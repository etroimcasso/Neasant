#include "neasant/nes/clock.h"
#include "neasant/nes/nes.h"
#include "authored_image.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// The machine around the core: built from a config value at power-on and refusing what
// it cannot build; one CPU cycle a step, with the reset sequence over the first seven;
// the master-cycle budget with its overshoot carried; the bus decode over work RAM, the
// cartridge, open bus, the PPU's ports and the APU's registers; the reset line; snapshot
// and restore; and the clock values. A case that checks a hardware rule names the
// document and section it is read from; the cases on two machines built alike, the
// budget, snapshot and restore, and the pads check the machine's own surface.

namespace {

using neasant::consoleClock;
using neasant::Cpu6502State;
using neasant::FrameDeadline;
using neasant::Joypad;
using neasant::JoypadPort;
using neasant::kCpuFlagI;
using neasant::Mirroring;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::Nrom;
using neasant::powerOn;
using neasant::Region;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::placeProgram;
using neasant::nes_tests::prgAt;

using Bytes = std::vector<std::uint8_t>;
using Program = std::initializer_list<std::uint8_t>;

constexpr std::uint8_t kNop = 0xEA;

// A 16 KB NROM image with 8 KB of CHR ROM, `program` at $8000 and every vector pointing
// there.
Bytes imageWith(Program program) {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  placeProgram(image, 0, std::span<const std::uint8_t>(program.begin(), program.size()));
  return image;
}

// A machine over that image, built with the defaults.
Nes machineWith(Program program) {
  const Bytes image = imageWith(program);
  return Nes(NesConfig{.rom = image});
}

// Steps `cycles` machine cycles.
void stepCycles(Nes& machine, int cycles) {
  for (int i = 0; i < cycles; ++i) machine.step();
}

// The message construction refuses a config with.
std::string buildRefusal(const NesConfig& config) {
  try {
    (void)Nes(config);
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  return "(accepted)";
}

// The message restore refuses a state with.
std::string restoreRefusal(Nes& machine, const NesState& state) {
  try {
    machine.restore(state);
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  return "(accepted)";
}

// ---- NesBuild ----

TEST(NesBuild, RefusesARegionAlignmentOrPhaseItCannotBuild) {
  // NESdev wiki, "CPU", Frequencies; "PPU frame timing", CPU-PPU Clock Alignment; "DMA",
  // Summary.
  const Bytes image = imageWith({kNop});
  EXPECT_EQ(buildRefusal({.rom = image, .region = Region::Pal}),
            "PAL and Dendy are not yet selectable; NTSC is");
  EXPECT_EQ(buildRefusal({.rom = image, .region = Region::Dendy}),
            "PAL and Dendy are not yet selectable; NTSC is");
  EXPECT_EQ(buildRefusal({.rom = image, .alignment = 4}),
            "an alignment of 4 is not one of 0 to 3");
  EXPECT_EQ(buildRefusal({.rom = image, .apuPhase = 2}),
            "an apuPhase of 2 is not 0 (a get) or 1 (a put)");
  // The config's own fields are checked before the image is read.
  EXPECT_EQ(buildRefusal({.rom = image, .region = Region::Pal, .mapper = 4}),
            "PAL and Dendy are not yet selectable; NTSC is");
  EXPECT_EQ(buildRefusal({.alignment = 4}), "an alignment of 4 is not one of 0 to 3");
  const Nes m(NesConfig{.rom = image, .alignment = 3, .apuPhase = 1});
  EXPECT_EQ(m.alignment(), 3);
  EXPECT_EQ(m.state().apuPhase, 1);
}

TEST(NesBuild, RefusesWhatTheCartridgeAndBoardRefuse) {
  // NESdev wiki, "iNES", iNES file format; "NROM", the board table.
  const Bytes image = imageWith({kNop});
  const Bytes fifteen(image.begin(), image.begin() + 15);
  EXPECT_EQ(buildRefusal(NesConfig{}), "an image of 0 bytes holds no 16-byte header");
  EXPECT_EQ(buildRefusal({.rom = fifteen}), "an image of 15 bytes holds no 16-byte header");
  EXPECT_EQ(buildRefusal({.rom = image, .mapper = 4}), "mapper 4 is not built; NROM (0) is");
  EXPECT_EQ(buildRefusal({.rom = image, .mirroring = Mirroring::FourScreen}),
            "NROM has no four-screen RAM");
}

TEST(NesBuild, PowerOnIsTheDocumentedState) {
  // NESdev wiki, "CPU power up state", CPU and APU; "DMA", Cadence; "iNES", Flags 8.
  const Bytes image = imageWith({kNop});
  const Nes m(NesConfig{.rom = image, .alignment = 3, .apuPhase = 1});
  const NesState& s = m.state();
  EXPECT_TRUE(s.cpu == powerOn());
  EXPECT_EQ(s.mdr, 0);
  EXPECT_EQ(s.master, 0u);
  EXPECT_EQ(s.consumed, 0u);
  EXPECT_EQ(s.dotClock, 3u);
  EXPECT_EQ(s.apuPhase, 1);
  EXPECT_TRUE(std::all_of(s.apuRegs.begin(), s.apuRegs.end(),
                          [](std::uint8_t byte) { return byte == 0; }));
  EXPECT_EQ(s.ppu.ioLatch, 0);
  EXPECT_EQ(s.ppu.dot, 0);
  EXPECT_EQ(s.ppu.scanline, 0);
  EXPECT_FALSE(s.ppu.odd);
  EXPECT_FALSE(s.pads[0].has_value());
  EXPECT_FALSE(s.pads[1].has_value());
  EXPECT_FALSE(s.strobe);
  EXPECT_EQ(s.dma.page, 0);
  EXPECT_EQ(s.prgRam.size(), 8192u);
  EXPECT_EQ(s.chrRam.size(), 0u);
  EXPECT_TRUE(std::holds_alternative<Nrom>(s.board));
  EXPECT_EQ(m.region(), Region::Ntsc);
  EXPECT_EQ(m.alignment(), 3);
}

TEST(NesBuild, TheSeedFillsEveryRam) {
  // NESdev wiki, "CPU power up state", RAM contents; "iNES", byte 5.
  const Bytes image = authoredImage(inesHeader({.chrUnits = 0}), 16384, 0);
  const std::array<std::uint8_t, 3> seed{0x12, 0x34, 0x56};
  const Nes m(NesConfig{.rom = image, .ramSeed = seed});
  const NesState& s = m.state();
  EXPECT_EQ(s.ram[0], 0x12);
  EXPECT_EQ(s.ram[1], 0x34);
  EXPECT_EQ(s.ram[2], 0x56);
  EXPECT_EQ(s.ram[3], 0x12);
  EXPECT_EQ(s.ram[2047], 0x34);  // 2047 mod 3 is 1
  EXPECT_EQ(s.prgRam[0], 0x12);
  EXPECT_EQ(s.prgRam[8191], 0x34);  // 8191 mod 3 is 1
  EXPECT_EQ(s.chrRam.size(), 8192u);
  EXPECT_EQ(s.chrRam[0], 0x12);
  EXPECT_EQ(s.chrRam[8191], 0x34);
  EXPECT_EQ(s.ppu.ciram[0], 0x12);
  EXPECT_EQ(s.ppu.ciram[2047], 0x34);

  const Nes z(NesConfig{.rom = image});
  const NesState& zs = z.state();
  const auto zero = [](std::uint8_t byte) { return byte == 0; };
  EXPECT_TRUE(std::all_of(zs.ram.begin(), zs.ram.end(), zero));
  EXPECT_TRUE(std::all_of(zs.prgRam.begin(), zs.prgRam.end(), zero));
  EXPECT_TRUE(std::all_of(zs.chrRam.begin(), zs.chrRam.end(), zero));
  EXPECT_TRUE(std::all_of(zs.ppu.ciram.begin(), zs.ppu.ciram.end(), zero));
}

TEST(NesBuild, TwoMachinesFromOneConfigAreEqual) {
  const Bytes image = imageWith({0x4C, 0x00, 0x80});  // JMP $8000
  Nes a(NesConfig{.rom = image});
  Nes b(NesConfig{.rom = image});
  EXPECT_TRUE(a.state() == b.state());
  a.run(1000);
  b.run(1000);
  EXPECT_TRUE(a.state() == b.state());
  Nes c = a;
  EXPECT_TRUE(c.state() == a.state());
  c.run(777);
  a.run(777);
  EXPECT_TRUE(c.state() == a.state());
}

// ---- NesStep ----

TEST(NesStep, TheFirstSevenCyclesAreTheResetSequence) {
  // NESdev wiki, "CPU power up state", CPU; "CPU memory map", the vectors.
  Nes m = machineWith({kNop});
  const Cpu6502State& cpu = m.state().cpu;
  EXPECT_EQ(m.step(), 12u);
  EXPECT_EQ(cpu.tcu, 1);
  EXPECT_EQ(m.state().master, 12u);
  EXPECT_EQ(m.state().consumed, 12u);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(m.step(), 12u);
  }
  EXPECT_EQ(cpu.pc, 0);
  EXPECT_EQ(cpu.tcu, 6);
  EXPECT_EQ(m.step(), 12u);
  EXPECT_EQ(cpu.pc, 0x8000);
  EXPECT_EQ(cpu.s, 0xFD);
  EXPECT_NE(cpu.p & kCpuFlagI, 0);
  EXPECT_EQ(cpu.a, 0);
  EXPECT_EQ(cpu.tcu, 0);
  EXPECT_EQ(m.state().mdr, 0x80);  // the vector's high byte, the last read
  EXPECT_EQ(m.state().master, 84u);
  EXPECT_EQ(m.state().consumed, 84u);
}

TEST(NesStep, AStepIsOneCpuCycleOfTwelveMasterCycles) {
  // NESdev wiki, "CPU", Frequencies; "DMA", Cadence; 64doc, "Accumulator or implied
  // addressing".
  const Bytes image = imageWith({kNop, kNop});
  Nes m(NesConfig{.rom = image});
  const Cpu6502State& cpu = m.state().cpu;
  stepCycles(m, 7);
  EXPECT_EQ(m.step(), 12u);
  EXPECT_EQ(cpu.tcu, 1);
  EXPECT_EQ(cpu.ir, 0xEA);
  EXPECT_EQ(cpu.pc, 0x8001);  // the opcode fetch
  EXPECT_EQ(m.step(), 12u);
  EXPECT_EQ(cpu.tcu, 0);  // the discarded read that ends the NOP
  EXPECT_EQ(m.state().master, 108u);

  Nes one(NesConfig{.rom = image, .apuPhase = 1});
  Nes zero(NesConfig{.rom = image});
  stepCycles(one, 7);
  EXPECT_EQ(one.state().apuPhase, 0);  // seven toggles
  stepCycles(one, 2);
  EXPECT_EQ(one.state().apuPhase, 0);
  stepCycles(zero, 7);
  EXPECT_EQ(zero.state().apuPhase, 1);
}

TEST(NesStep, RunCarriesTheOvershoot) {
  const Bytes image = imageWith({0x4C, 0x00, 0x80});  // JMP $8000
  Nes a(NesConfig{.rom = image});
  Nes b(NesConfig{.rom = image});
  EXPECT_EQ(a.run(100), 108u);
  EXPECT_EQ(a.run(100), 96u);
  EXPECT_EQ(b.run(200), 204u);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.state().master, 204u);
  EXPECT_EQ(a.state().consumed, 200u);
  // Seven reset cycles, three jumps and the first cycle of the fourth.
  EXPECT_EQ(a.state().cpu.tcu, 1);
}

TEST(NesStep, RunOfZeroRunsNothing) {
  Nes m = machineWith({0x4C, 0x00, 0x80});  // JMP $8000
  const Nes fresh = m;
  EXPECT_EQ(m.run(0), 0u);
  EXPECT_TRUE(m.state() == fresh.state());
  EXPECT_EQ(m.run(100), 108u);
  EXPECT_EQ(m.run(0), 0u);
  EXPECT_EQ(m.state().master, 108u);
  EXPECT_EQ(m.state().consumed, 100u);
}

TEST(NesStep, StepKeepsTheOvershootRunCarries) {
  Nes m = machineWith({kNop, kNop, kNop});
  const Cpu6502State& cpu = m.state().cpu;
  EXPECT_EQ(m.run(90), 96u);
  EXPECT_EQ(cpu.tcu, 1);
  EXPECT_EQ(cpu.ir, 0xEA);
  EXPECT_EQ(m.state().consumed, 90u);
  EXPECT_EQ(m.step(), 12u);
  EXPECT_EQ(cpu.pc, 0x8001);
  EXPECT_EQ(cpu.tcu, 0);
  EXPECT_EQ(m.state().master, 108u);
  EXPECT_EQ(m.state().consumed, 102u);
  EXPECT_EQ(m.run(6), 0u);
  EXPECT_EQ(m.state().consumed, 108u);
  EXPECT_EQ(m.run(1), 12u);
  EXPECT_EQ(m.state().master, 120u);
  EXPECT_EQ(m.state().consumed, 109u);
}

TEST(NesStep, AJammedCoreIdlesOneCycleAStepUntilReset) {
  // NESdev wiki, "CPU unofficial opcodes", JAM; "CPU power up state", CPU.
  Nes m = machineWith({0x02});
  const Cpu6502State& cpu = m.state().cpu;
  stepCycles(m, 7);
  stepCycles(m, 5);
  EXPECT_FALSE(cpu.jammed);
  EXPECT_EQ(cpu.tcu, 5);
  EXPECT_EQ(cpu.pc, 0x8002);
  EXPECT_EQ(m.step(), 12u);
  EXPECT_TRUE(cpu.jammed);  // the JAM's sixth cycle sets the halt
  EXPECT_EQ(m.step(), 12u);
  EXPECT_TRUE(cpu.jammed);
  EXPECT_EQ(m.state().master, 168u);
  m.reset();
  stepCycles(m, 7);
  EXPECT_EQ(cpu.pc, 0x8000);
  EXPECT_EQ(cpu.s, 0xFA);
  EXPECT_FALSE(cpu.jammed);
  EXPECT_EQ(m.state().master, 252u);
}

TEST(NesStep, AHaltedReadReachesTheBus) {
  // NESdev wiki, "DMA", Behavior and Register conflicts.
  Nes m = machineWith({kNop, kNop});
  const Cpu6502State& cpu = m.state().cpu;
  stepCycles(m, 7);
  NesState s = m.state();
  s.cpu.rdyLine = false;
  m.restore(s);
  EXPECT_EQ(m.step(), 12u);
  EXPECT_EQ(m.state().mdr, 0xEA);  // the halted opcode fetch of $8000 reached the cartridge
  EXPECT_EQ(cpu.pc, 0x8000);
  EXPECT_EQ(cpu.tcu, 0);
  EXPECT_TRUE(cpu.rdyHeld);
  EXPECT_EQ(m.state().master, 96u);
  EXPECT_EQ(m.step(), 12u);
  EXPECT_EQ(m.state().mdr, 0xEA);
  EXPECT_EQ(cpu.pc, 0x8000);
  EXPECT_EQ(m.state().master, 108u);

  s = m.state();
  s.cpu.rdyLine = true;
  m.restore(s);
  EXPECT_EQ(m.step(), 12u);
  EXPECT_EQ(cpu.pc, 0x8001);  // the fetch made for real
  EXPECT_EQ(cpu.tcu, 1);
  EXPECT_FALSE(cpu.rdyHeld);
}

TEST(NesStep, TheDotsCountTheFrame) {
  // NESdev wiki, "PPU frame timing", Even/Odd Frames and CPU-PPU Clock Alignment.
  const Bytes image = imageWith({0x4C, 0x00, 0x80});  // JMP $8000
  Nes m(NesConfig{.rom = image});
  stepCycles(m, 7);
  EXPECT_EQ(m.state().ppu.dot, 21);
  EXPECT_EQ(m.state().ppu.scanline, 0);
  EXPECT_FALSE(m.state().ppu.odd);
  EXPECT_EQ(m.state().dotClock, 84u);

  // The dots at master cycles 3, 7, ... 83 fall inside the first seven cycles.
  Nes c(NesConfig{.rom = image, .alignment = 3});
  stepCycles(c, 7);
  EXPECT_EQ(c.state().ppu.dot, 21);
  EXPECT_EQ(c.state().dotClock, 87u);

  Nes a(NesConfig{.rom = image});
  EXPECT_EQ(a.run(1368), 1368u);
  EXPECT_EQ(a.state().ppu.dot, 1);
  EXPECT_EQ(a.state().ppu.scanline, 1);

  Nes b(NesConfig{.rom = image});
  EXPECT_EQ(b.run(357368), 357372u);
  EXPECT_EQ(b.state().ppu.dot, 1);
  EXPECT_EQ(b.state().ppu.scanline, 0);
  EXPECT_TRUE(b.state().ppu.odd);
  EXPECT_EQ(b.run(357368), 357372u);
  EXPECT_EQ(b.state().ppu.dot, 2);
  EXPECT_EQ(b.state().ppu.scanline, 0);
  EXPECT_FALSE(b.state().ppu.odd);
  EXPECT_EQ(b.state().master, 714744u);
}

// ---- NesBus ----

TEST(NesBus, WorkRamRepeatsThroughTheFirstEightKilobytes) {
  // NESdev wiki, "CPU memory map", the table.
  // LDA #$5A; STA $0801; STA $1FFF; LDA #$00; LDA $1001
  Nes m = machineWith({0xA9, 0x5A, 0x8D, 0x01, 0x08, 0x8D, 0xFF, 0x1F, 0xA9, 0x00, 0xAD, 0x01,
                       0x10});
  stepCycles(m, 23);
  EXPECT_EQ(m.state().ram[1], 0x5A);
  EXPECT_EQ(m.state().ram[0x7FF], 0x5A);
  EXPECT_EQ(m.state().ram[0], 0);
  EXPECT_EQ(m.state().cpu.a, 0x5A);
}

TEST(NesBus, ThePrgRomRepeatsAndIsNeverWritten) {
  // NESdev wiki, "NROM", Banks and Registers; "Mirroring", Memory Mirroring.
  // LDA $FFF0; LDX $BFF0; LDY #$99; STY $8000; LDA $8000
  Bytes image = imageWith({0xAD, 0xF0, 0xFF, 0xAE, 0xF0, 0xBF, 0xA0, 0x99, 0x8C, 0x00, 0x80,
                           0xAD, 0x00, 0x80});
  prgAt(image, 0x3FF0) = 0x77;
  Nes m(NesConfig{.rom = image});
  stepCycles(m, 25);
  EXPECT_EQ(m.state().cpu.x, 0x77);
  EXPECT_EQ(m.state().cpu.y, 0x99);
  EXPECT_EQ(m.state().cpu.a, 0xAD);  // the opcode byte at $8000, unchanged by the store
}

TEST(NesBus, ThePrgRamWindowIsTheCartridges) {
  // NESdev wiki, "NROM", Banks; "iNES", Flags 8.
  // LDA #$33; STA $6000; LDA #$44; STA $7FFF; LDX $6000; LDY $7FFF
  Nes m = machineWith({0xA9, 0x33, 0x8D, 0x00, 0x60, 0xA9, 0x44, 0x8D, 0xFF, 0x7F, 0xAE, 0x00,
                       0x60, 0xAC, 0xFF, 0x7F});
  stepCycles(m, 27);
  EXPECT_EQ(m.state().prgRam[0], 0x33);
  EXPECT_EQ(m.state().prgRam[8191], 0x44);
  EXPECT_EQ(m.state().cpu.x, 0x33);
  EXPECT_EQ(m.state().cpu.y, 0x44);
}

TEST(NesBus, AnUndrivenReadAnswersTheLastByteRead) {
  // NESdev wiki, "Open bus behavior", CPU open bus; "2A03", 2A03 register map; "CPU memory
  // map"; "NROM", Banks. Each read answers the high byte of its address, the last byte read
  // before it.
  // LDA $4018; STA $10; LDA $5FFF; STA $11; LDA $4000; STA $12; LDA $401F; STA $13;
  // LDA $7FFF; STA $14; LDA $4014; STA $15
  const Bytes image = imageWith({0xAD, 0x18, 0x40, 0x85, 0x10, 0xAD, 0xFF, 0x5F, 0x85, 0x11,
                                 0xAD, 0x00, 0x40, 0x85, 0x12, 0xAD, 0x1F, 0x40, 0x85, 0x13,
                                 0xAD, 0xFF, 0x7F, 0x85, 0x14, 0xAD, 0x14, 0x40, 0x85, 0x15});
  Nes m(NesConfig{.rom = image, .prgRamBytes = 0});
  stepCycles(m, 49);
  const NesState& s = m.state();
  EXPECT_EQ(s.ram[0x10], 0x40);
  EXPECT_EQ(s.ram[0x11], 0x5F);
  EXPECT_EQ(s.ram[0x12], 0x40);
  EXPECT_EQ(s.ram[0x13], 0x40);
  EXPECT_EQ(s.ram[0x14], 0x7F);
  EXPECT_EQ(s.ram[0x15], 0x40);
}

TEST(NesBus, AnIndirectIndexedReadOfOpenBusAnswersThePointersHighByte) {
  // NESdev wiki, "Open bus behavior", Indirect indexed instruction example: cycle 5 reads
  // $732B and cycle 6 $742B, both answering the pointer's high byte.
  // LDA #$FA; STA $04; LDA #$73; STA $05; LDY #$31; LDA ($04),Y
  const Bytes image =
      imageWith({0xA9, 0xFA, 0x85, 0x04, 0xA9, 0x73, 0x85, 0x05, 0xA0, 0x31, 0xB1, 0x04});
  Nes m(NesConfig{.rom = image, .prgRamBytes = 0});
  stepCycles(m, 25);
  EXPECT_EQ(m.state().cpu.a, 0x73);
  EXPECT_EQ(m.state().mdr, 0x73);
}

TEST(NesBus, ThePpuPortsLoadAndAnswerOneLatch) {
  // NESdev wiki, "Open bus behavior", PPU open bus; "CPU memory map", the table; "PPU
  // registers", PPUSTATUS and The PPUDATA read buffer; "PPU power up state". The mirror
  // $3F02 is $2002, answering the vertical-blank flag, set at power-on by default, over
  // the latch's low five bits; $2007 answers the read buffer, held at zero through the
  // warm-up, and loads it into the latch.
  // LDA #$AA; STA $2005; LDA $2001; LDX $4018; LDY $3F02; LDA #$BB; STA $2002; LDA $2007
  Nes m = machineWith({0xA9, 0xAA, 0x8D, 0x05, 0x20, 0xAD, 0x01, 0x20, 0xAE, 0x18, 0x40, 0xAC,
                       0x02, 0x3F, 0xA9, 0xBB, 0x8D, 0x02, 0x20, 0xAD, 0x07, 0x20});
  stepCycles(m, 35);
  EXPECT_EQ(m.state().cpu.x, 0x40);  // the CPU's open bus, another latch
  EXPECT_EQ(m.state().cpu.y, 0x8A);  // $80 | ($AA & $1F)
  EXPECT_EQ(m.state().cpu.a, 0x00);  // the write to $2002 loaded $BB; $2007 answers 0
  EXPECT_EQ(m.state().ppu.ioLatch, 0x00);
}

TEST(NesBus, TheApuStatusReadLeavesTheBusAlone) {
  // NESdev wiki, "APU", Status; "Open bus behavior", PPU open bus. The indexed read crosses
  // a page, so its fourth cycle reads $3F15, a PPU port answering $FF, and its fifth $4015.
  // LDA #$FF; STA $2000; LDX #$FF; LDA $3F16,X
  Nes m = machineWith({0xA9, 0xFF, 0x8D, 0x00, 0x20, 0xA2, 0xFF, 0xBD, 0x16, 0x3F});
  stepCycles(m, 20);
  EXPECT_EQ(m.state().cpu.a, 0x20);  // bit 5 from the bus, every other bit 0
  EXPECT_EQ(m.state().mdr, 0xFF);    // the $4015 read left it
  EXPECT_EQ(m.state().ppu.ioLatch, 0xFF);
}

TEST(NesBus, TheApuRegistersHoldTheirBytes) {
  // NESdev wiki, "2A03", 2A03 register map; "APU", Registers; "DMA", OAM DMA; "Controller
  // reading", $4016 Write. The write to $4014 starts a transfer that holds the CPU for
  // 513 cycles before the next instruction's opcode fetch.
  // LDA #$C0; STA $4017; LDA #$0F; STA $4015; LDA #$02; STA $4014; LDA #$01; STA $4016;
  // LDA #$7F; STA $4011; STA $4000
  Nes m = machineWith({0xA9, 0xC0, 0x8D, 0x17, 0x40, 0xA9, 0x0F, 0x8D, 0x15, 0x40, 0xA9, 0x02,
                       0x8D, 0x14, 0x40, 0xA9, 0x01, 0x8D, 0x16, 0x40, 0xA9, 0x7F, 0x8D, 0x11,
                       0x40, 0x8D, 0x00, 0x40});
  stepCycles(m, 554);
  const NesState& s = m.state();
  EXPECT_EQ(s.apuRegs[0x17], 0xC0);
  EXPECT_EQ(s.apuRegs[0x15], 0x0F);
  EXPECT_EQ(s.apuRegs[0x11], 0x7F);
  EXPECT_EQ(s.apuRegs[0x00], 0x7F);
  EXPECT_EQ(s.apuRegs[0x14], 0);
  EXPECT_EQ(s.apuRegs[0x16], 0);
  EXPECT_EQ(s.dma.page, 2);
  EXPECT_TRUE(s.strobe);
}

TEST(NesBus, TheControllerPortsAnswerOpenBusAboveFiveZeroLines) {
  // NESdev wiki, "Controller reading", $4016 / $4017 Read and Unconnected data lines and
  // open bus; "Open bus behavior", CPU open bus; "PPU registers", The PPUDATA read
  // buffer. Each indexed read's page-crossing cycle reads a PPU port before the port
  // register: the first reads $3F16, a write-only port answering the latch, $FF; the
  // second reads $3F17, which is $2007, answering the read buffer, zero through the
  // warm-up.
  // LDA #$FF; STA $2000; LDX #$FF; LDA $3F17,X; STA $10; LDA $3F18,X
  Nes m = machineWith({0xA9, 0xFF, 0x8D, 0x00, 0x20, 0xA2, 0xFF, 0xBD, 0x17, 0x3F, 0x85, 0x10,
                       0xBD, 0x18, 0x3F});
  stepCycles(m, 28);
  EXPECT_EQ(m.state().ram[0x10], 0xE0);  // $4016
  EXPECT_EQ(m.state().cpu.a, 0x00);      // $4017
  EXPECT_EQ(m.state().mdr, 0x00);
}

// ---- NesReset ----

TEST(NesReset, ResetKeepsWhatItKeeps) {
  // NESdev wiki, "CPU power up state", CPU and APU; "APU", Status.
  // LDA #$55; STA $0000; LDA #$0F; STA $4015; LDA #$C0; STA $4017; LDA #$7F; STA $4011;
  // STA $4000; STA $2000; STA $4016; LDX #$12; LDY #$34
  Nes m = machineWith({0xA9, 0x55, 0x8D, 0x00, 0x00, 0xA9, 0x0F, 0x8D, 0x15, 0x40, 0xA9, 0xC0,
                       0x8D, 0x17, 0x40, 0xA9, 0x7F, 0x8D, 0x11, 0x40, 0x8D, 0x00, 0x40, 0x8D,
                       0x00, 0x20, 0x8D, 0x16, 0x40, 0xA2, 0x12, 0xA0, 0x34});
  stepCycles(m, 47);
  EXPECT_EQ(m.state().master, 564u);
  m.reset();
  for (int i = 0; i < 7; ++i) {
    EXPECT_EQ(m.step(), 12u);
  }
  const NesState& s = m.state();
  EXPECT_EQ(s.cpu.pc, 0x8000);
  EXPECT_EQ(s.cpu.s, 0xFA);
  EXPECT_EQ(s.cpu.a, 0x7F);
  EXPECT_EQ(s.cpu.x, 0x12);
  EXPECT_EQ(s.cpu.y, 0x34);
  EXPECT_NE(s.cpu.p & kCpuFlagI, 0);
  EXPECT_EQ(s.apuRegs[0x15], 0);
  EXPECT_EQ(s.apuRegs[0x17], 0xC0);
  EXPECT_EQ(s.apuRegs[0x11], 0x01);
  EXPECT_EQ(s.apuRegs[0x00], 0x7F);
  EXPECT_EQ(s.ram[0], 0x55);
  EXPECT_EQ(s.ppu.ioLatch, 0x7F);
  EXPECT_TRUE(s.strobe);
  EXPECT_EQ(s.mdr, 0x80);
  EXPECT_EQ(s.master, 648u);
  EXPECT_EQ(s.consumed, 648u);
  EXPECT_EQ(s.apuPhase, 0);  // 54 cycles
}

// ---- NesSnapshot ----

TEST(NesSnapshot, RestoreResumesExactlyThere) {
  const Bytes image = imageWith({0x4C, 0x00, 0x80});  // JMP $8000
  Nes a(NesConfig{.rom = image});
  a.run(90);
  const NesState save = a.state();
  EXPECT_EQ(save.cpu.tcu, 1);
  a.run(5000);
  a.restore(save);
  Nes b(NesConfig{.rom = image});
  b.restore(save);
  EXPECT_TRUE(a.state() == save);
  EXPECT_TRUE(b.state() == save);
  EXPECT_EQ(a.step(), 12u);
  EXPECT_EQ(a.state().cpu.tcu, 2);
  EXPECT_EQ(b.step(), 12u);
  EXPECT_TRUE(a.state() == b.state());
  a.step();
  b.step();
  EXPECT_EQ(a.state().cpu.pc, 0x8000);
  EXPECT_EQ(a.state().cpu.tcu, 0);  // the jump's third cycle
  EXPECT_TRUE(a.state() == b.state());
  a.run(1000);
  b.run(1000);
  EXPECT_TRUE(a.state() == b.state());
}

TEST(NesSnapshot, RestoreRefusesAnotherCartridgesSnapshot) {
  Nes m = machineWith({kNop});
  const NesState before = m.state();
  NesState s = before;
  s.prgRam.clear();
  NesState t = before;
  t.chrRam.resize(16);
  EXPECT_EQ(restoreRefusal(m, s),
            "a snapshot of another cartridge: its PRG RAM is 0 bytes and this machine's is 8192");
  EXPECT_TRUE(m.state() == before);
  EXPECT_EQ(restoreRefusal(m, t),
            "a snapshot of another cartridge: its CHR RAM is 16 bytes and this machine's is 0");
  EXPECT_TRUE(m.state() == before);
}

TEST(NesSnapshot, PadsAreState) {
  Nes m = machineWith({kNop});
  m.setJoypad(JoypadPort::One, Joypad{.a = true, .start = true});
  ASSERT_TRUE(m.joypad(JoypadPort::One).has_value());
  EXPECT_TRUE(m.joypad(JoypadPort::One)->a);
  EXPECT_TRUE(m.joypad(JoypadPort::One)->start);
  EXPECT_FALSE(m.joypad(JoypadPort::One)->b);
  EXPECT_FALSE(m.joypad(JoypadPort::Two).has_value());
  EXPECT_TRUE(m.state().pads[0] == (Joypad{.a = true, .start = true}));
  const Nes c = m;
  EXPECT_TRUE(c.joypad(JoypadPort::One) == m.joypad(JoypadPort::One));
  NesState s = m.state();
  s.pads[0].reset();
  m.restore(s);
  EXPECT_FALSE(m.joypad(JoypadPort::One).has_value());
  m.setJoypad(JoypadPort::Two, Joypad{});
  EXPECT_TRUE(m.joypad(JoypadPort::Two) == Joypad{});
  m.setJoypad(JoypadPort::Two, std::nullopt);
  EXPECT_FALSE(m.joypad(JoypadPort::Two).has_value());
}

// ---- NesClock ----

TEST(NesClock, ConsoleClockValues) {
  // NESdev wiki, "CPU", Frequencies, and "PPU frame timing", Even/Odd Frames; "2A03
  // technical reference", CLK.
  const auto ntsc = consoleClock(Region::Ntsc);
  EXPECT_EQ(ntsc.masterCyclesPerFrame, 357368u);
  EXPECT_EQ(ntsc.hertzNumerator, 236250000u);
  EXPECT_EQ(ntsc.hertzDenominator, 11u);
  for (const Region region : {Region::Pal, Region::Dendy}) {
    const auto clock = consoleClock(region);
    EXPECT_EQ(clock.masterCyclesPerFrame, 0u);
    EXPECT_EQ(clock.hertzNumerator, 53203425u);
    EXPECT_EQ(clock.hertzDenominator, 2u);
  }
}

TEST(NesClock, FrameDeadlineValues) {
  // NESdev wiki, "CPU", Frequencies; "PPU frame timing", Even/Odd Frames.
  const FrameDeadline ntsc(Region::Ntsc);
  const FrameDeadline pal(Region::Pal);
  const FrameDeadline dendy(Region::Dendy);
  EXPECT_EQ(ntsc.owedAt(0), 0u);
  EXPECT_EQ(ntsc.owedAt(1), 16639356u);
  // 60 x 16 639 356 + floor(60 x 145 000 000 / 236 250 000) = 998 361 360 + 36.
  EXPECT_EQ(ntsc.owedAt(60), 998361396u);
  EXPECT_EQ(ntsc.owedAt(4693), 78088500588u);
  EXPECT_EQ(ntsc.wholeNanos(), 16639356u);
  EXPECT_EQ(pal.owedAt(1), 0u);
  EXPECT_EQ(pal.owedAt(1000), 0u);
  EXPECT_EQ(pal.wholeNanos(), 0u);
  EXPECT_EQ(dendy.owedAt(1), 0u);
}

}  // namespace
