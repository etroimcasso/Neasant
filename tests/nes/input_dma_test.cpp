#include "neasant/nes/nes.h"
#include "authored_image.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// The controller ports and the OAM DMA unit: the strobe and the registers it fills, a
// pad read bit by bit through its port's clock line, the run of reads that is one
// clock, an empty socket, the open-bus bits above the lines; and OAM DMA halting the
// CPU through RDY for 513 or 514 cycles by the half of the APU cycle it lands on, the
// halted read repeating on its no-operation cycles, the halt waiting for a write cycle
// and copying the second page written, the bytes landing at the OAM address through
// the one bus, a reset ending a transfer, a snapshot carrying one, and an NMI arriving
// inside one. A case that checks a hardware rule names the document and section it is
// read from; the cases on the button names, power-on, the registers as state and the
// snapshot check the machine's own surface.

namespace {

using neasant::Button;
using neasant::buttonFromName;
using neasant::buttonName;
using neasant::buttons;
using neasant::Dma;
using neasant::DmaStage;
using neasant::Joypad;
using neasant::JoypadPort;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::placeProgram;

using Bytes = std::vector<std::uint8_t>;
using Program = std::initializer_list<std::uint8_t>;

constexpr std::uint8_t kNop = 0xEA;

// A 16 KB NROM image with 8 KB of CHR ROM, `program` at $8000 and every vector pointing
// there.
Bytes imageWith(std::span<const std::uint8_t> program) {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  placeProgram(image, 0, program);
  return image;
}

// A machine over that image, built with the defaults but for the APU half the first
// cycle falls in.
Nes machineWith(const Bytes& program, std::uint8_t apuPhase = 0) {
  const Bytes image = imageWith(program);
  return Nes(NesConfig{.rom = image, .apuPhase = apuPhase});
}
Nes machineWith(Program program, std::uint8_t apuPhase = 0) {
  return machineWith(Bytes(program), apuPhase);
}

// Steps `cycles` machine cycles.
void stepCycles(Nes& machine, int cycles) {
  for (int i = 0; i < cycles; ++i) machine.step();
}

// The strobe: LDA #$01; STA $4016; LDA #$00; STA $4016. Twelve cycles.
constexpr std::array<std::uint8_t, 10> kStrobe{0xA9, 0x01, 0x8D, 0x16, 0x40,
                                               0xA9, 0x00, 0x8D, 0x16, 0x40};

// `count` reads of the register at $40xx, each stored at `at` upward in the zero page:
// LDA abs; STA zp. Seven cycles a read, and the store's opcode fetch is the access
// that raises the port's clock line.
Bytes readsOf(std::uint8_t low, int count, std::uint8_t at = 0x10) {
  Bytes bytes;
  for (int i = 0; i < count; ++i) {
    bytes.insert(bytes.end(), {0xAD, low, 0x40, 0x85, static_cast<std::uint8_t>(at + i)});
  }
  return bytes;
}

// `parts` joined into one program.
Bytes join(std::initializer_list<std::span<const std::uint8_t>> parts) {
  Bytes bytes;
  for (const auto part : parts) bytes.insert(bytes.end(), part.begin(), part.end());
  return bytes;
}

// Fills page `page` of work RAM, or of PRG RAM for pages $60-$7F, with byte i ^ `key`,
// through a snapshot.
void fillPage(Nes& machine, std::uint8_t page, std::uint8_t key) {
  NesState s = machine.state();
  for (std::size_t i = 0; i < 256; ++i) {
    const auto byte = static_cast<std::uint8_t>(i ^ key);
    if (page < 0x08) {
      s.ram[(static_cast<std::size_t>(page) << 8) | i] = byte;
    } else {
      s.prgRam[((static_cast<std::size_t>(page) << 8) | i) - 0x6000] = byte;
    }
  }
  machine.restore(s);
}

// Whether OAM holds byte i ^ `key` at every i.
bool oamHolds(const NesState& s, std::uint8_t key) {
  for (std::size_t i = 0; i < 256; ++i) {
    if (s.ppu.oam[i] != static_cast<std::uint8_t>(i ^ key)) return false;
  }
  return true;
}

// A pad with the named buttons pressed.
Joypad padOf(std::initializer_list<Button> pressed) {
  Joypad pad;
  for (const Button button : pressed) pad.hold(button, true);
  return pad;
}

// ---- NesButtons ----

TEST(NesButtons, NamesAndBitsFollowTheShiftOrder) {
  // NESdev wiki, "Standard controller", Report; "Controller reading", $4016 / $4017 Read.
  ASSERT_EQ(buttons().size(), 8u);
  EXPECT_EQ(buttons()[0], Button::A);
  EXPECT_EQ(buttons()[1], Button::B);
  EXPECT_EQ(buttons()[2], Button::Select);
  EXPECT_EQ(buttons()[3], Button::Start);
  EXPECT_EQ(buttons()[4], Button::Up);
  EXPECT_EQ(buttons()[5], Button::Down);
  EXPECT_EQ(buttons()[6], Button::Left);
  EXPECT_EQ(buttons()[7], Button::Right);
  EXPECT_EQ(buttonName(Button::A), "a");
  EXPECT_EQ(buttonName(Button::Select), "select");
  EXPECT_EQ(buttonName(Button::Right), "right");
  EXPECT_EQ(buttonName(static_cast<Button>(8)), "");
  EXPECT_EQ(buttonFromName("start"), Button::Start);
  EXPECT_EQ(buttonFromName("Start"), Button::Start);
  EXPECT_EQ(buttonFromName("DOWN"), Button::Down);
  EXPECT_EQ(buttonFromName("x"), std::nullopt);
  EXPECT_EQ(buttonFromName(""), std::nullopt);
  EXPECT_EQ(buttonFromName("selec"), std::nullopt);
  Joypad pad;
  pad.hold(Button::Left, true);
  EXPECT_TRUE(pad.left);
  EXPECT_TRUE(pad.holds(Button::Left));
  EXPECT_FALSE(pad.holds(Button::Right));
  EXPECT_EQ(pad.bits(), 0x40);
  pad.hold(Button::Left, false);
  EXPECT_TRUE(pad == Joypad{});
  EXPECT_EQ(Joypad{}.bits(), 0x00);
  EXPECT_EQ((Joypad{.a = true, .right = true}).bits(), 0x81);
  EXPECT_EQ(padOf({Button::A, Button::Select, Button::Up, Button::Left}).bits(), 0x55);
  EXPECT_EQ(padOf({Button::B, Button::Start, Button::Down, Button::Right}).bits(), 0xAA);
}

// ---- NesJoypad ----

TEST(NesJoypad, PowerOnLeavesThePortsEmptyAndTheUnitIdle) {
  const Nes m = machineWith({kNop});
  const NesState& s = m.state();
  EXPECT_FALSE(s.pads[0].has_value());
  EXPECT_FALSE(s.pads[1].has_value());
  EXPECT_FALSE(s.strobe);
  EXPECT_EQ(s.shift[0], 0);
  EXPECT_EQ(s.shift[1], 0);
  EXPECT_FALSE(s.clockLow[0]);
  EXPECT_FALSE(s.clockLow[1]);
  EXPECT_TRUE(s.dma == Dma{});
  EXPECT_EQ(s.dma.stage, DmaStage::Idle);
  EXPECT_FALSE(s.dma.dmcRequested);
}

TEST(NesJoypad, TheStrobeIsBitZeroOfAWriteAndFillsTheRegisterWhileHigh) {
  // NESdev wiki, "Controller reading", $4016 Write: the low bits are latched and held;
  // writing 1 fills the register from the buttons held, writing 0 returns it to serial
  // mode. "Standard controller", Input ($4016 write): while the strobe is high a read
  // keeps returning the current state of the first button, A.
  // LDA #$FF; STA $4016; LDA $4016; STA $10; LDA $4016; STA $11; LDA #$FE; STA $4016;
  // LDA $4016; STA $12; LDA $4016; STA $13
  Nes m = machineWith({0xA9, 0xFF, 0x8D, 0x16, 0x40, 0xAD, 0x16, 0x40, 0x85, 0x10, 0xAD,
                       0x16, 0x40, 0x85, 0x11, 0xA9, 0xFE, 0x8D, 0x16, 0x40, 0xAD, 0x16,
                       0x40, 0x85, 0x12, 0xAD, 0x16, 0x40, 0x85, 0x13});
  m.setJoypad(JoypadPort::One, Joypad{.a = true});
  stepCycles(m, 13);
  EXPECT_TRUE(m.state().strobe);
  EXPECT_EQ(m.state().shift[0], 0x01);  // filled at the write
  stepCycles(m, 7);
  EXPECT_EQ(m.state().ram[0x10], 0x41);  // A, over the operand's high byte $40
  m.setJoypad(JoypadPort::One, Joypad{.b = true});
  stepCycles(m, 7);
  EXPECT_EQ(m.state().ram[0x11], 0x40);  // the register refilled at the read: A is up
  EXPECT_EQ(m.state().shift[0], 0x02);
  stepCycles(m, 6);
  EXPECT_FALSE(m.state().strobe);        // bit 0 of $FE
  EXPECT_EQ(m.state().shift[0], 0x02);   // held at the fall
  stepCycles(m, 7);
  EXPECT_EQ(m.state().ram[0x12], 0x40);  // A, from the held register
  EXPECT_EQ(m.state().shift[0], 0x81);   // clocked by the store's opcode fetch
  stepCycles(m, 7);
  EXPECT_EQ(m.state().ram[0x13], 0x41);  // B
}

TEST(NesJoypad, APadIsReadBitByBitInShiftOrder) {
  // NESdev wiki, "Standard controller", Report and Output ($4016/$4017 read): A, B, Select,
  // Start, Up, Down, Left, Right, one bit a read through D0, 1 if pressed.
  Nes m = machineWith(join({kStrobe, readsOf(0x16, 8)}));
  m.setJoypad(JoypadPort::One, padOf({Button::A, Button::Select, Button::Up, Button::Left}));
  stepCycles(m, 75);
  const NesState& s = m.state();
  EXPECT_EQ(s.ram[0x10], 0x41);  // A
  EXPECT_EQ(s.ram[0x11], 0x40);  // B
  EXPECT_EQ(s.ram[0x12], 0x41);  // Select
  EXPECT_EQ(s.ram[0x13], 0x40);  // Start
  EXPECT_EQ(s.ram[0x14], 0x41);  // Up
  EXPECT_EQ(s.ram[0x15], 0x40);  // Down
  EXPECT_EQ(s.ram[0x16], 0x41);  // Left
  EXPECT_EQ(s.ram[0x17], 0x40);  // Right
  EXPECT_EQ(s.shift[0], 0xFF);   // eight clocks shifted eight ones in
}

TEST(NesJoypad, ReadsPastTheEighthAnswerOne) {
  // NESdev wiki, "Standard controller", Report: after 8 bits are read, all subsequent bits
  // report 1; Hardware: the serial input is grounded and read back inverted.
  const Bytes program = join({kStrobe, readsOf(0x16, 12)});
  Nes none = machineWith(program);
  none.setJoypad(JoypadPort::One, Joypad{});
  stepCycles(none, 103);
  for (std::size_t i = 0x10; i < 0x18; ++i) {
    EXPECT_EQ(none.state().ram[i], 0x40) << i;
  }
  for (std::size_t i = 0x18; i < 0x1C; ++i) {
    EXPECT_EQ(none.state().ram[i], 0x41) << i;
  }
  Nes all = machineWith(program);
  all.setJoypad(JoypadPort::One, padOf({Button::A, Button::B, Button::Select, Button::Start,
                                        Button::Up, Button::Down, Button::Left, Button::Right}));
  stepCycles(all, 103);
  for (std::size_t i = 0x10; i < 0x1C; ++i) {
    EXPECT_EQ(all.state().ram[i], 0x41) << i;
  }
}

TEST(NesJoypad, AnEmptySocketAnswersZeroOnEveryBit) {
  // NESdev wiki, "Controller reading", Unconnected data lines and open bus: D0 reads 0
  // if not connected, on both ports of a front-loading console; "Standard controller",
  // Output ($4016/$4017 read): with no controller connected the status bit reports 0.
  Nes m = machineWith(join({kStrobe, readsOf(0x16, 5), readsOf(0x17, 5, 0x15)}));
  stepCycles(m, 89);
  const NesState& s = m.state();
  for (std::size_t i = 0x10; i < 0x1A; ++i) {
    EXPECT_EQ(s.ram[i], 0x40) << i;
  }
  EXPECT_EQ(s.shift[0], 0);
  EXPECT_EQ(s.shift[1], 0);
}

TEST(NesJoypad, PortTwoIsReadAtFourZeroOneSevenAndClocksAlone) {
  // NESdev wiki, "Controller reading", $4016 / $4017 Read: $4016 reads port 1 only and
  // $4017 port 2 only; Clock timing: each port has its own clock line.
  Nes m = machineWith(join({kStrobe, readsOf(0x17, 8), readsOf(0x16, 1, 0x18)}));
  m.setJoypad(JoypadPort::One, Joypad{.a = true});
  m.setJoypad(JoypadPort::Two, padOf({Button::B, Button::Start, Button::Down, Button::Right}));
  stepCycles(m, 82);
  const NesState& s = m.state();
  EXPECT_EQ(s.ram[0x10], 0x40);  // A
  EXPECT_EQ(s.ram[0x11], 0x41);  // B
  EXPECT_EQ(s.ram[0x12], 0x40);
  EXPECT_EQ(s.ram[0x13], 0x41);  // Start
  EXPECT_EQ(s.ram[0x14], 0x40);
  EXPECT_EQ(s.ram[0x15], 0x41);  // Down
  EXPECT_EQ(s.ram[0x16], 0x40);
  EXPECT_EQ(s.ram[0x17], 0x41);  // Right
  EXPECT_EQ(s.ram[0x18], 0x41);  // port one's A, its register unclocked by port two's reads
  EXPECT_EQ(s.shift[1], 0xFF);
  EXPECT_EQ(s.shift[0], 0x80);   // clocked once, by the last store's opcode fetch
}

TEST(NesJoypad, AContiguousRunOfReadsIsOneClock) {
  // NESdev wiki, "DMA", Register conflicts: the output enables stay asserted across
  // adjacent cycles reading the same joypad register, so the controller sees a single
  // read for each contiguous set; "Controller reading", Clock timing. The run here is
  // the read of $4016 repeated while RDY is held low, then made for real.
  Nes m = machineWith(join({kStrobe, readsOf(0x16, 2)}));
  m.setJoypad(JoypadPort::One, padOf({Button::A, Button::Select, Button::Up, Button::Left}));
  stepCycles(m, 22);  // the strobe, then the read's first three cycles
  EXPECT_EQ(m.state().cpu.tcu, 3);
  NesState s = m.state();
  s.cpu.rdyLine = false;
  m.restore(s);
  for (int i = 0; i < 3; ++i) {
    m.step();
    EXPECT_TRUE(m.state().cpu.rdyHeld);
    EXPECT_EQ(m.state().cpu.tcu, 3);
    EXPECT_EQ(m.state().cpu.pc, 0x800D);
    EXPECT_EQ(m.state().mdr, 0x41);  // the halted read answered A
    EXPECT_TRUE(m.state().clockLow[0]);
    EXPECT_EQ(m.state().shift[0], 0x55);
  }
  s = m.state();
  s.cpu.rdyLine = true;
  m.restore(s);
  m.step();  // the read, for real
  EXPECT_EQ(m.state().cpu.a, 0x41);
  EXPECT_EQ(m.state().cpu.tcu, 0);
  EXPECT_EQ(m.state().shift[0], 0x55);
  EXPECT_TRUE(m.state().clockLow[0]);
  stepCycles(m, 3);  // STA $10: its opcode fetch raises the line, one clock for four reads
  EXPECT_EQ(m.state().ram[0x10], 0x41);
  EXPECT_EQ(m.state().shift[0], 0xAA);
  EXPECT_FALSE(m.state().clockLow[0]);
  stepCycles(m, 7);
  EXPECT_EQ(m.state().ram[0x11], 0x40);  // B
}

TEST(NesJoypad, TheOpenBusBitsRideAboveTheLines) {
  // NESdev wiki, "Controller reading", $4016 / $4017 Read and Unconnected data lines and
  // open bus: bits 7-5 are open bus; "Open bus behavior", CPU open bus. Each indexed
  // read crosses a page, so its fourth cycle reads a PPU port before the port register:
  // $3F17 is $2007, answering the read buffer, and $3F16 is $2006, answering the I/O
  // bus; both are set to $FF through a snapshot so the byte on the bus has bits 7-5 set.
  // LDX #$FF; LDA $3F18,X; STA $10; LDA $3F17,X; STA $11
  Nes m = machineWith(join({kStrobe, Bytes{0xA2, 0xFF, 0xBD, 0x18, 0x3F, 0x85, 0x10, 0xBD,
                                           0x17, 0x3F, 0x85, 0x11}}));
  NesState s = m.state();
  s.ppu.ioLatch = 0xFF;
  s.ppu.readBuffer = 0xFF;
  m.restore(s);
  m.setJoypad(JoypadPort::Two, Joypad{.a = true});
  stepCycles(m, 37);
  EXPECT_EQ(m.state().ram[0x10], 0xE1);  // $4017: port two's A under the bus's $FF
  EXPECT_EQ(m.state().ram[0x11], 0xE0);  // $4016: an empty socket
  EXPECT_EQ(m.state().mdr, 0xE0);
}

TEST(NesJoypad, TheStrobesFallHoldsTheButtonsOfThatMoment) {
  // NESdev wiki, "Controller reading", $4016 Write: writing 1 fills the register from the
  // buttons currently held; writing 0 returns it to serial mode, waiting to be read out.
  Nes m = machineWith(join({kStrobe, readsOf(0x16, 2)}));
  m.setJoypad(JoypadPort::One, Joypad{.a = true});
  stepCycles(m, 13);  // LDA #$01; STA $4016
  EXPECT_EQ(m.state().shift[0], 0x01);
  m.setJoypad(JoypadPort::One, Joypad{.b = true});
  stepCycles(m, 6);  // LDA #$00; STA $4016
  EXPECT_FALSE(m.state().strobe);
  EXPECT_EQ(m.state().shift[0], 0x02);
  m.setJoypad(JoypadPort::One, Joypad{.select = true});
  stepCycles(m, 14);
  EXPECT_EQ(m.state().ram[0x10], 0x40);  // A, as held at the fall
  EXPECT_EQ(m.state().ram[0x11], 0x41);  // B, as held at the fall
}

TEST(NesJoypad, TheShiftRegistersAreState) {
  Nes m = machineWith(join({kStrobe, readsOf(0x16, 5)}));
  m.setJoypad(JoypadPort::One, padOf({Button::A, Button::Select, Button::Up, Button::Left}));
  stepCycles(m, 40);  // the strobe and three reads
  const NesState save = m.state();
  EXPECT_EQ(save.shift[0], 0xEA);
  EXPECT_FALSE(save.clockLow[0]);
  stepCycles(m, 14);
  EXPECT_EQ(m.state().ram[0x13], 0x40);  // Start
  EXPECT_EQ(m.state().ram[0x14], 0x41);  // Up
  const NesState after = m.state();
  m.restore(save);
  stepCycles(m, 14);
  EXPECT_TRUE(m.state() == after);
}

// ---- NesOamDma ----

TEST(NesOamDma, OamDmaHaltingOnAPutCycleTakes513Cycles) {
  // NESdev wiki, "DMA", Summary, Cadence, Behavior and OAM DMA: the halt on the first
  // cycle after the write, then 256 get/put pairs, 513 cycles when the first get needs
  // no alignment; the CPU then performs the read it was halted on.
  // LDA #$02; STA $4014; NOP; NOP
  Nes m = machineWith({0xA9, 0x02, 0x8D, 0x14, 0x40, kNop, kNop});
  fillPage(m, 0x02, 0x5A);
  stepCycles(m, 13);
  const NesState& s = m.state();
  EXPECT_EQ(s.dma.page, 0x02);
  EXPECT_EQ(s.dma.stage, DmaStage::Halt);
  EXPECT_EQ(s.cpu.pc, 0x8005);
  EXPECT_TRUE(s.cpu.rdyLine);
  EXPECT_EQ(s.apuPhase, 1);  // the halt cycle is a put
  EXPECT_EQ(s.master, 156u);
  m.step();  // the halt cycle: the opcode fetch, repeated
  EXPECT_FALSE(s.cpu.rdyLine);
  EXPECT_TRUE(s.cpu.rdyHeld);
  EXPECT_EQ(s.cpu.pc, 0x8005);
  EXPECT_EQ(s.cpu.tcu, 0);
  EXPECT_EQ(s.mdr, kNop);
  EXPECT_EQ(s.dma.stage, DmaStage::Get);
  m.step();  // the first get
  EXPECT_EQ(s.dma.byte, 0x5A);
  EXPECT_EQ(s.dma.stage, DmaStage::Put);
  EXPECT_EQ(s.mdr, 0x5A);
  m.step();  // the first put
  EXPECT_EQ(s.ppu.oam[0], 0x5A);
  EXPECT_EQ(s.ppu.oamAddr, 1);
  EXPECT_EQ(s.dma.index, 1);
  EXPECT_EQ(s.dma.stage, DmaStage::Get);
  stepCycles(m, 510);
  EXPECT_EQ(s.dma.stage, DmaStage::Idle);
  EXPECT_EQ(s.dma.index, 0);
  EXPECT_TRUE(s.cpu.rdyLine);
  EXPECT_EQ(s.cpu.pc, 0x8005);
  EXPECT_EQ(s.cpu.tcu, 0);
  EXPECT_TRUE(oamHolds(s, 0x5A));
  EXPECT_EQ(s.ppu.oamAddr, 0);
  EXPECT_EQ(s.ppu.ioLatch, 0xA5);  // the last put drove the PPU's bus
  EXPECT_EQ(s.mdr, 0xA5);
  EXPECT_EQ(s.master, 6312u);  // 526 cycles: 13 before the halt and 513 held
  m.step();  // the opcode fetch, for real
  EXPECT_EQ(s.cpu.tcu, 1);
  EXPECT_EQ(s.cpu.ir, kNop);
  EXPECT_EQ(s.cpu.pc, 0x8006);
  EXPECT_FALSE(s.cpu.rdyHeld);
}

TEST(NesOamDma, OamDmaHaltingOnAGetCycleTakes514Cycles) {
  // NESdev wiki, "DMA", Summary, Cadence, Behavior and OAM DMA: a put cycle after the
  // halt cycle is an alignment cycle, 514 in all.
  Nes m = machineWith({0xA9, 0x02, 0x8D, 0x14, 0x40, kNop, kNop}, 1);
  fillPage(m, 0x02, 0x5A);
  stepCycles(m, 13);
  const NesState& s = m.state();
  EXPECT_EQ(s.apuPhase, 0);  // the halt cycle is a get
  m.step();  // the halt cycle
  EXPECT_EQ(s.dma.stage, DmaStage::Get);
  EXPECT_EQ(s.mdr, kNop);
  m.step();  // the alignment cycle
  EXPECT_EQ(s.dma.stage, DmaStage::Get);
  EXPECT_EQ(s.dma.index, 0);
  EXPECT_TRUE(s.cpu.rdyHeld);
  EXPECT_EQ(s.cpu.pc, 0x8005);
  EXPECT_EQ(s.mdr, kNop);
  EXPECT_EQ(s.master, 180u);
  stepCycles(m, 512);
  EXPECT_EQ(s.dma.stage, DmaStage::Idle);
  EXPECT_TRUE(s.cpu.rdyLine);
  EXPECT_TRUE(oamHolds(s, 0x5A));
  EXPECT_EQ(s.master, 6324u);  // 527 cycles: 13 before the halt and 514 held
  m.step();
  EXPECT_EQ(s.cpu.tcu, 1);
  EXPECT_EQ(s.cpu.pc, 0x8006);
}

TEST(NesOamDma, TheHaltedReadRepeatsOnEveryNoOperationCycle) {
  // NESdev wiki, "DMA", Behavior and Register conflicts: while halted the CPU repeats
  // the read it was halted on during every no-operation DMA cycle, and the read is
  // externally visible. The open bus is cleared through a snapshot before each such
  // cycle, and the read puts the opcode back on it.
  Nes m = machineWith({0xA9, 0x02, 0x8D, 0x14, 0x40, kNop, kNop}, 1);
  fillPage(m, 0x02, 0x5A);
  stepCycles(m, 13);
  NesState s = m.state();
  s.mdr = 0x00;
  m.restore(s);
  m.step();  // the halt cycle
  EXPECT_EQ(m.state().mdr, kNop);
  s = m.state();
  s.mdr = 0x00;
  m.restore(s);
  m.step();  // the alignment cycle
  EXPECT_EQ(m.state().mdr, kNop);
  EXPECT_EQ(m.state().cpu.pc, 0x8005);
  EXPECT_EQ(m.state().cpu.tcu, 0);
  EXPECT_EQ(m.state().dma.stage, DmaStage::Get);
  m.step();  // the first get
  EXPECT_EQ(m.state().mdr, 0x5A);
}

TEST(NesOamDma, OamDmaWaitsForAWriteCycleAndCopiesTheSecondPageWritten) {
  // NESdev wiki, "DMA", Behavior and OAM DMA: the halt fails on a write cycle and is
  // tried again next cycle; a read-modify-write of $4014 performs its second write
  // before the CPU can be halted, and the copy is from the second page written. The
  // read of $4014 answers open bus, the operand's high byte $40, so the pages are $40
  // and $41; page $41 is open bus too ("Open bus behavior", CPU open bus), so every get
  // answers the byte the halted read left, the opcode after the instruction.
  // INC $4014; NOP; NOP
  Nes m = machineWith({0xEE, 0x14, 0x40, kNop, kNop});
  stepCycles(m, 12);
  const NesState& s = m.state();
  EXPECT_EQ(s.dma.page, 0x40);  // the first write
  EXPECT_EQ(s.dma.stage, DmaStage::Halt);
  EXPECT_TRUE(s.cpu.rdyLine);
  EXPECT_EQ(s.cpu.tcu, 5);
  m.step();  // the second write proceeds under RDY low
  EXPECT_EQ(s.dma.page, 0x41);
  EXPECT_EQ(s.dma.stage, DmaStage::Halt);
  EXPECT_FALSE(s.cpu.rdyLine);
  EXPECT_FALSE(s.cpu.rdyHeld);
  EXPECT_EQ(s.cpu.tcu, 0);
  EXPECT_EQ(s.cpu.pc, 0x8003);
  EXPECT_EQ(s.mdr, 0x41);
  m.step();  // the halt cycle
  EXPECT_TRUE(s.cpu.rdyHeld);
  EXPECT_EQ(s.dma.stage, DmaStage::Get);
  EXPECT_EQ(s.mdr, kNop);
  stepCycles(m, 512);
  EXPECT_EQ(s.dma.stage, DmaStage::Idle);
  for (std::size_t i = 0; i < 256; ++i) {
    EXPECT_EQ(s.ppu.oam[i], kNop) << i;
  }
  EXPECT_EQ(s.master, 6312u);
  m.step();
  EXPECT_EQ(s.cpu.tcu, 1);
  EXPECT_EQ(s.cpu.pc, 0x8004);
}

TEST(NesOamDma, OamDmaLandsAtTheOamAddressAndWraps) {
  // NESdev wiki, "DMA", OAM DMA: 256 bytes copied via OAMDATA; "PPU registers", OAMDATA:
  // each write lands at OAMADDR and increments it.
  // LDA #$10; STA $2003; LDA #$02; STA $4014; NOP
  Nes m = machineWith({0xA9, 0x10, 0x8D, 0x03, 0x20, 0xA9, 0x02, 0x8D, 0x14, 0x40, kNop});
  fillPage(m, 0x02, 0x5A);
  stepCycles(m, 19);
  EXPECT_EQ(m.state().ppu.oamAddr, 0x10);
  EXPECT_EQ(m.state().dma.stage, DmaStage::Halt);
  stepCycles(m, 513);
  const NesState& s = m.state();
  EXPECT_EQ(s.dma.stage, DmaStage::Idle);
  EXPECT_EQ(s.ppu.oam[0x10], 0x5A);  // the page's byte 0
  EXPECT_EQ(s.ppu.oam[0xFF], 0xB5);  // byte $EF
  EXPECT_EQ(s.ppu.oam[0x00], 0xAA);  // byte $F0
  EXPECT_EQ(s.ppu.oam[0x0F], 0xA5);  // byte $FF
  EXPECT_EQ(s.ppu.oamAddr, 0x10);
}

TEST(NesOamDma, ResetEndsATransferAndReleasesTheCpu) {
  // NESdev wiki, "CPU pinout", /RST; "CPU power up state", CPU.
  Nes m = machineWith({0xA9, 0x02, 0x8D, 0x14, 0x40, kNop, kNop});
  fillPage(m, 0x02, 0x5A);
  stepCycles(m, 114);  // the halt and fifty pairs
  const NesState& s = m.state();
  EXPECT_EQ(s.dma.stage, DmaStage::Get);
  EXPECT_EQ(s.dma.index, 50);
  EXPECT_EQ(s.ppu.oam[49], 0x6B);  // 49 ^ $5A
  EXPECT_EQ(s.ppu.oam[50], 0x00);
  EXPECT_FALSE(s.cpu.rdyLine);
  m.reset();
  EXPECT_EQ(s.dma.stage, DmaStage::Idle);
  EXPECT_EQ(s.dma.index, 0);
  EXPECT_EQ(s.dma.page, 0x02);
  EXPECT_TRUE(s.cpu.rdyLine);
  stepCycles(m, 7);
  EXPECT_EQ(s.cpu.pc, 0x8000);
  EXPECT_EQ(s.cpu.s, 0xFA);
  EXPECT_EQ(s.cpu.tcu, 0);
  EXPECT_EQ(s.ppu.oam[49], 0x6B);
  EXPECT_EQ(s.ppu.oam[50], 0x00);
  EXPECT_EQ(s.master, 1452u);
}

TEST(NesOamDma, ASnapshotMidTransferResumesIt) {
  const Bytes program{0xA9, 0x02, 0x8D, 0x14, 0x40, kNop, kNop};
  Nes a = machineWith(program);
  fillPage(a, 0x02, 0x5A);
  stepCycles(a, 114);
  const NesState save = a.state();
  EXPECT_EQ(save.dma.stage, DmaStage::Get);
  EXPECT_EQ(save.dma.index, 50);
  EXPECT_FALSE(save.cpu.rdyLine);
  Nes b = machineWith(program);
  b.restore(save);
  stepCycles(a, 412);  // the 206 pairs left
  stepCycles(b, 412);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.state().dma.stage, DmaStage::Idle);
  EXPECT_TRUE(oamHolds(a.state(), 0x5A));
  EXPECT_TRUE(a.state().cpu.rdyLine);
  EXPECT_EQ(a.state().master, 6312u);
}

TEST(NesOamDma, AnNmiArrivingDuringTheTransferIsTakenAfterTheHaltedInstruction) {
  // NESdev wiki, "DMA", Behavior: while halted the core makes no forward progress nor
  // handles interrupts; "CPU interrupts", Detailed interrupt behavior: the edge detector
  // polls the line every cycle, and the request is taken at the next instruction
  // boundary. The PPU's output is asserted through a snapshot once the CPU is halted.
  Nes m = machineWith({0xA9, 0x02, 0x8D, 0x14, 0x40, kNop, kNop});
  fillPage(m, 0x02, 0x5A);
  stepCycles(m, 14);  // through the halt cycle
  NesState s = m.state();
  s.ppu.ctrl = 0x80;
  s.ppu.vblank = true;
  m.restore(s);
  stepCycles(m, 512);
  EXPECT_EQ(m.state().dma.stage, DmaStage::Idle);
  EXPECT_TRUE(m.state().cpu.nmiLine);
  EXPECT_TRUE(m.state().cpu.nmiPending);
  stepCycles(m, 2);  // the NOP the CPU was halted on
  EXPECT_EQ(m.state().cpu.pc, 0x8006);
  EXPECT_EQ(m.state().cpu.tcu, 0);
  stepCycles(m, 7);  // the NMI sequence
  EXPECT_EQ(m.state().cpu.pc, 0x8000);  // the vector points at the entry
  EXPECT_EQ(m.state().cpu.s, 0xFA);
  EXPECT_FALSE(m.state().cpu.nmiPending);
}

TEST(NesOamDma, OamDmaReadsAnyPageTheBusDecodes) {
  // NESdev wiki, "DMA", OAM DMA: the page is any CPU page; "NROM", Banks: $6000-$7FFF is
  // the PRG RAM and $8000-$FFFF the PRG ROM. The second transfer's halt cycle is a get,
  // so it takes 514 cycles.
  // LDA #$60; STA $4014; LDA #$80; STA $4014; NOP
  Nes m = machineWith({0xA9, 0x60, 0x8D, 0x14, 0x40, 0xA9, 0x80, 0x8D, 0x14, 0x40, kNop});
  fillPage(m, 0x60, 0xC3);
  stepCycles(m, 526);
  const NesState& s = m.state();
  EXPECT_EQ(s.dma.stage, DmaStage::Idle);
  EXPECT_TRUE(oamHolds(s, 0xC3));
  stepCycles(m, 520);
  EXPECT_EQ(s.dma.stage, DmaStage::Idle);
  const std::array<std::uint8_t, 11> program{0xA9, 0x60, 0x8D, 0x14, 0x40, 0xA9,
                                             0x80, 0x8D, 0x14, 0x40, kNop};
  for (std::size_t i = 0; i < program.size(); ++i) {
    EXPECT_EQ(s.ppu.oam[i], program[i]) << i;
  }
  for (std::size_t i = program.size(); i < 256; ++i) {
    EXPECT_EQ(s.ppu.oam[i], 0x00) << i;
  }
  EXPECT_EQ(s.master, 12552u);  // 1046 cycles
}

}  // namespace
