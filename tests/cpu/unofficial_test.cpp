#include "neasant/cpu/cpu6502.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

// The unofficial opcodes of the 6502, one instruction at a time through the public
// surface: the value each operation leaves and the flags it sets, the bytes the
// stores write and where they land, and the halt a JAM opcode brings. The
// read-modify-write combinations run on the byte at $0010 and the accumulator
// operations in their immediate form. Each case names the NESdev wiki page and
// section or the 64doc section it is read from. The whole status byte is compared,
// so a flag that moves without being named fails the case.

namespace {

using neasant::Cpu6502;
using neasant::Cpu6502State;
using neasant::CycleKind;
using neasant::kCpuFlagC;
using neasant::kCpuFlagD;
using neasant::kCpuFlagN;
using neasant::kCpuFlagV;
using neasant::kCpuFlagZ;
using neasant::cpu_vectors::Access;
using neasant::cpu_vectors::RecordingBus;
using enum neasant::CycleKind;

constexpr std::uint16_t kCell = 0x0010;

RecordingBus busWith(std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> ram) {
  RecordingBus bus;
  for (const auto& [address, value] : ram) bus.mem[address] = value;
  return bus;
}

void expectAccess(const Access& access, std::uint16_t address, CycleKind kind) {
  EXPECT_EQ(access.address, address);
  EXPECT_EQ(access.kind, kind);
}

// One expected bus access; a negative value leaves the byte unchecked.
struct Step {
  std::uint16_t address;
  CycleKind kind;
  int value = -1;
};

void expectTrace(const std::vector<Access>& trace, std::initializer_list<Step> steps) {
  ASSERT_EQ(trace.size(), steps.size());
  std::size_t cycle = 0;
  for (const Step& step : steps) {
    SCOPED_TRACE(::testing::Message() << "cycle " << cycle + 1);
    expectAccess(trace[cycle], step.address, step.kind);
    if (step.value >= 0) {
      EXPECT_EQ(trace[cycle].value, step.value);
    }
    ++cycle;
  }
}

// What one instruction leaves: the registers and the byte at $0010.
struct Result {
  Cpu6502State state;
  std::uint8_t cell;
};

// Places the two bytes of an instruction at $0200 and `cell` at $0010, runs that
// instruction from `start` with PC at $0200 and S at $FD, and returns the result.
Result run(std::uint8_t opcode, std::uint8_t operand, Cpu6502State start,
           std::uint8_t cell = 0) {
  auto bus = busWith({{0x0200, opcode}, {0x0201, operand}, {kCell, cell}});
  start.pc = 0x0200;
  start.s = 0xFD;
  Cpu6502 cpu(start);
  cpu.stepInstruction(bus);
  return {cpu.state(), bus.mem[kCell]};
}

void expectStatus(const Cpu6502State& state, std::uint8_t expected) {
  EXPECT_EQ(static_cast<int>(state.p), static_cast<int>(expected));
}

constexpr std::uint8_t kSloZeroPage = 0x07;
constexpr std::uint8_t kRlaZeroPage = 0x27;
constexpr std::uint8_t kSreZeroPage = 0x47;
constexpr std::uint8_t kRraZeroPage = 0x67;
constexpr std::uint8_t kDcpZeroPage = 0xC7;
constexpr std::uint8_t kIscZeroPage = 0xE7;
constexpr std::uint8_t kSaxZeroPage = 0x87;
constexpr std::uint8_t kLaxZeroPage = 0xA7;
constexpr std::uint8_t kAncImmediate = 0x0B;
constexpr std::uint8_t kAncImmediateSecond = 0x2B;
constexpr std::uint8_t kAlrImmediate = 0x4B;
constexpr std::uint8_t kArrImmediate = 0x6B;
constexpr std::uint8_t kSbxImmediate = 0xCB;
constexpr std::uint8_t kAneImmediate = 0x8B;
constexpr std::uint8_t kLxaImmediate = 0xAB;

TEST(CpuUnofficial, SloShiftsTheByteLeftAndOrsItIntoA) {
  // NESdev wiki, "Programming with unofficial opcodes", RMW instructions, SLO.
  const Result r = run(kSloZeroPage, 0x10, {.a = 0x01}, 0x81);
  EXPECT_EQ(r.cell, 0x02);
  EXPECT_EQ(r.state.a, 0x03);
  expectStatus(r.state, kCpuFlagC);
}

TEST(CpuUnofficial, RlaRotatesTheByteLeftAndAndsItIntoA) {
  // NESdev wiki, "Programming with unofficial opcodes", RMW instructions, RLA.
  const Result r = run(kRlaZeroPage, 0x10, {.a = 0xFF, .p = kCpuFlagC}, 0x40);
  EXPECT_EQ(r.cell, 0x81);
  EXPECT_EQ(r.state.a, 0x81);
  expectStatus(r.state, kCpuFlagN);
}

TEST(CpuUnofficial, SreShiftsTheByteRightAndEorsItIntoA) {
  // NESdev wiki, "Programming with unofficial opcodes", RMW instructions, SRE.
  const Result r = run(kSreZeroPage, 0x10, {.a = 0xFF}, 0x03);
  EXPECT_EQ(r.cell, 0x01);
  EXPECT_EQ(r.state.a, 0xFE);
  expectStatus(r.state, kCpuFlagN | kCpuFlagC);
}

TEST(CpuUnofficial, RraRotatesTheByteRightAndAddsItToA) {
  // NESdev wiki, "Programming with unofficial opcodes", RMW instructions, RRA; NOPs
  // (ARR, ISC and RRA affect V).
  const Result a = run(kRraZeroPage, 0x10, {.a = 0x10, .p = kCpuFlagC}, 0x03);
  EXPECT_EQ(a.cell, 0x81);
  EXPECT_EQ(a.state.a, 0x92);
  expectStatus(a.state, kCpuFlagN);

  const Result b = run(kRraZeroPage, 0x10, {.a = 0x7F}, 0x02);
  EXPECT_EQ(b.cell, 0x01);
  EXPECT_EQ(b.state.a, 0x80);
  expectStatus(b.state, kCpuFlagN | kCpuFlagV);
}

TEST(CpuUnofficial, DcpDecrementsTheByteAndComparesAWithIt) {
  // NESdev wiki, "Programming with unofficial opcodes", RMW instructions, DCP, with
  // its LDA #$FF then DCP check for a decrement that underflows.
  const Result a = run(kDcpZeroPage, 0x10, {.a = 0x40}, 0x41);
  EXPECT_EQ(a.cell, 0x40);
  EXPECT_EQ(a.state.a, 0x40);
  expectStatus(a.state, kCpuFlagZ | kCpuFlagC);

  const Result b = run(kDcpZeroPage, 0x10, {.a = 0xFF}, 0x00);
  EXPECT_EQ(b.cell, 0xFF);
  EXPECT_EQ(b.state.a, 0xFF);
  expectStatus(b.state, kCpuFlagZ | kCpuFlagC);

  const Result c = run(kDcpZeroPage, 0x10, {.a = 0x10}, 0x21);
  EXPECT_EQ(c.cell, 0x20);
  EXPECT_EQ(c.state.a, 0x10);
  expectStatus(c.state, kCpuFlagN);
}

TEST(CpuUnofficial, IscIncrementsTheByteAndSubtractsItFromA) {
  // NESdev wiki, "Programming with unofficial opcodes", RMW instructions, ISC; NOPs
  // (ARR, ISC and RRA affect V).
  const Result a = run(kIscZeroPage, 0x10, {.a = 0x10, .p = kCpuFlagC}, 0x04);
  EXPECT_EQ(a.cell, 0x05);
  EXPECT_EQ(a.state.a, 0x0B);
  expectStatus(a.state, kCpuFlagC);

  const Result b = run(kIscZeroPage, 0x10, {.a = 0x80, .p = kCpuFlagC}, 0x00);
  EXPECT_EQ(b.cell, 0x01);
  EXPECT_EQ(b.state.a, 0x7F);
  expectStatus(b.state, kCpuFlagV | kCpuFlagC);
}

TEST(CpuUnofficial, SaxStoresAAndXAndMovesNoFlag) {
  // NESdev wiki, "Programming with unofficial opcodes", Combined operations, SAX;
  // 64doc, "6510/8502 Undocumented Commands" ("stores the result of (A & X)").
  const auto all = static_cast<std::uint8_t>(kCpuFlagN | kCpuFlagV | kCpuFlagZ | kCpuFlagC);
  const Result r = run(kSaxZeroPage, 0x10, {.a = 0xF0, .x = 0x3C, .p = all});
  EXPECT_EQ(r.cell, 0x30);
  EXPECT_EQ(r.state.a, 0xF0);
  EXPECT_EQ(r.state.x, 0x3C);
  expectStatus(r.state, all);
}

TEST(CpuUnofficial, LaxLoadsAAndXTogether) {
  // NESdev wiki, "Programming with unofficial opcodes", Combined operations, LAX;
  // 64doc, "Zero page indexed addressing" (LAX among the read instructions).
  const Result a = run(kLaxZeroPage, 0x10, {}, 0x80);
  EXPECT_EQ(a.state.a, 0x80);
  EXPECT_EQ(a.state.x, 0x80);
  expectStatus(a.state, kCpuFlagN);

  auto bus = busWith({{0x0200, 0xB7}, {0x0201, 0xF0}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x55, .x = 0x55, .y = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 4u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x00F0, DummyRead},
                          {0x0010, DataRead}});
  EXPECT_EQ(cpu.state().a, 0x00);
  EXPECT_EQ(cpu.state().x, 0x00);
  expectStatus(cpu.state(), kCpuFlagZ);
}

TEST(CpuUnofficial, AncAndsAndCopiesBitSevenIntoCarry) {
  // NESdev wiki, "Programming with unofficial opcodes", Combined operations, ANC
  // ("ANC #$00 acts like LDA #$00 followed by CLC").
  const Result a = run(kAncImmediate, 0x80, {.a = 0xFF});
  EXPECT_EQ(a.state.a, 0x80);
  expectStatus(a.state, kCpuFlagN | kCpuFlagC);

  const Result b = run(kAncImmediateSecond, 0x7F, {.a = 0xFF, .p = kCpuFlagC});
  EXPECT_EQ(b.state.a, 0x7F);
  expectStatus(b.state, 0);

  const Result c = run(kAncImmediate, 0x00, {.a = 0x55, .p = kCpuFlagC});
  EXPECT_EQ(c.state.a, 0x00);
  expectStatus(c.state, kCpuFlagZ);
}

TEST(CpuUnofficial, AlrAndsThenShiftsARight) {
  // NESdev wiki, "Programming with unofficial opcodes", Combined operations, ALR
  // ("ALR #$FE acts like LSR followed by CLC").
  const Result a = run(kAlrImmediate, 0x03, {.a = 0xFF});
  EXPECT_EQ(a.state.a, 0x01);
  expectStatus(a.state, kCpuFlagC);

  const Result b = run(kAlrImmediate, 0xFE, {.a = 0x03, .p = kCpuFlagC});
  EXPECT_EQ(b.state.a, 0x01);
  expectStatus(b.state, 0);
}

TEST(CpuUnofficial, ArrTakesCarryFromBitSixAndOverflowFromBitsSixAndFive) {
  // NESdev wiki, "Programming with unofficial opcodes", Combined operations, ARR;
  // 64doc, "6510/8502 Undocumented Commands", "$6B ARR", the binary-mode paragraph.
  const Result a = run(kArrImmediate, 0xFF, {.a = 0xC0});
  EXPECT_EQ(a.state.a, 0x60);
  expectStatus(a.state, kCpuFlagC);

  const Result b = run(kArrImmediate, 0xFF, {.a = 0x80});
  EXPECT_EQ(b.state.a, 0x40);
  expectStatus(b.state, kCpuFlagV | kCpuFlagC);

  const Result c = run(kArrImmediate, 0xFF, {.a = 0x40, .p = kCpuFlagC});
  EXPECT_EQ(c.state.a, 0xA0);
  expectStatus(c.state, kCpuFlagN | kCpuFlagV);

  const Result d = run(kArrImmediate, 0xFF, {.a = 0x00});
  EXPECT_EQ(d.state.a, 0x00);
  expectStatus(d.state, kCpuFlagZ);
}

TEST(CpuUnofficial, ArrIgnoresTheDecimalFlag) {
  // NESdev wiki, "CPU": the 2A03 lacks decimal mode. 64doc's decimal routine leaves
  // $C0 for these inputs, so the case tells the two apart.
  const Result r = run(kArrImmediate, 0xFF, {.a = 0xC0, .p = kCpuFlagD});
  EXPECT_EQ(r.state.a, 0x60);
  expectStatus(r.state, kCpuFlagD | kCpuFlagC);
}

TEST(CpuUnofficial, SbxSubtractsFromAAndXIntoXWithNoBorrowIn) {
  // NESdev wiki, "Programming with unofficial opcodes", Combined operations, AXS
  // ("TXA AXS #$FC could step to the next OAM entry"); 64doc, "6510/8502
  // Undocumented Commands", "$CB SBX" (carry in ignored, V not affected).
  const Result a = run(kSbxImmediate, 0x02, {.a = 0x0F, .x = 0xF3, .p = kCpuFlagV});
  EXPECT_EQ(a.state.x, 0x01);
  EXPECT_EQ(a.state.a, 0x0F);
  expectStatus(a.state, kCpuFlagV | kCpuFlagC);

  const Result b = run(kSbxImmediate, 0x05, {.a = 0xFF, .x = 0x03, .p = kCpuFlagC});
  EXPECT_EQ(b.state.x, 0xFE);
  expectStatus(b.state, kCpuFlagN);

  const Result c = run(kSbxImmediate, 0xFC, {.a = 0x10, .x = 0x10});
  EXPECT_EQ(c.state.x, 0x14);
  expectStatus(c.state, 0);
}

TEST(CpuUnofficial, AneOrsAConstantIntoAThenAndsXAndTheOperand) {
  // 64doc, "6510/8502 Undocumented Commands", ANE: A = (A | #$EE) & X & #byte.
  const Result a = run(kAneImmediate, 0xFF, {.a = 0x00, .x = 0xFF});
  EXPECT_EQ(a.state.a, 0xEE);
  EXPECT_EQ(a.state.x, 0xFF);
  expectStatus(a.state, kCpuFlagN);

  const Result b = run(kAneImmediate, 0x3C, {.a = 0xFF, .x = 0x0F});
  EXPECT_EQ(b.state.a, 0x0C);
  EXPECT_EQ(b.state.x, 0x0F);
  expectStatus(b.state, 0);

  const Result c = run(kAneImmediate, 0xFF, {.a = 0x00, .x = 0x00});
  EXPECT_EQ(c.state.a, 0x00);
  expectStatus(c.state, kCpuFlagZ);
}

TEST(CpuUnofficial, ShxStoresXAndedWithTheHighByteOfItsAddressPlusOne) {
  // 64doc, "6510/8502 Undocumented Commands", SHX; NESdev wiki, "Programming with
  // unofficial opcodes", Unimplemented addressing modes, SHX; 64doc, "Absolute
  // indexed addressing", write instructions.
  auto bus = busWith({{0x0200, 0x9E}, {0x0201, 0x00}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0xFF, .y = 0x05});
  EXPECT_EQ(cpu.stepInstruction(bus), 5u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x1205, DummyRead},
                          {0x1205, DataWrite, 0x13}});
  EXPECT_EQ(bus.mem[0x1205], 0x13);
  EXPECT_EQ(cpu.state().x, 0xFF);
  expectStatus(cpu.state(), 0);
}

TEST(CpuUnofficial, ShyStoresYAndedWithTheHighByteOfItsAddressPlusOne) {
  // 64doc, "6510/8502 Undocumented Commands", SHY; NESdev wiki, "Programming with
  // unofficial opcodes", Unimplemented addressing modes, SHY.
  auto bus = busWith({{0x0200, 0x9C}, {0x0201, 0x00}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x05, .y = 0xFF});
  EXPECT_EQ(cpu.stepInstruction(bus), 5u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x1205, DummyRead},
                          {0x1205, DataWrite, 0x13}});
  EXPECT_EQ(bus.mem[0x1205], 0x13);
  EXPECT_EQ(cpu.state().y, 0xFF);
}

TEST(CpuUnofficial, ShaStoresAAndXAndedWithTheHighByteOfItsAddressPlusOne) {
  // 64doc, "6510/8502 Undocumented Commands", SHA ($1E & $17 = $16, $16 & $13 =
  // $12); 64doc, "Indirect indexed addressing", write instructions.
  auto absolute = busWith({{0x0200, 0x9F}, {0x0201, 0x00}, {0x0202, 0x12}});
  Cpu6502 a(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x1E, .x = 0x17, .y = 0x05});
  EXPECT_EQ(a.stepInstruction(absolute), 5u);
  ASSERT_EQ(absolute.trace.size(), 5u);
  expectAccess(absolute.trace[4], 0x1205, DataWrite);
  EXPECT_EQ(absolute.trace[4].value, 0x12);
  EXPECT_EQ(a.state().a, 0x1E);
  EXPECT_EQ(a.state().x, 0x17);

  auto indirect = busWith({{0x0200, 0x93}, {0x0201, 0x10}, {0x0010, 0x00}, {0x0011, 0x12}});
  Cpu6502 b(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x1E, .x = 0x17, .y = 0x05});
  EXPECT_EQ(b.stepInstruction(indirect), 6u);
  expectTrace(indirect.trace, {{0x0200, OpcodeFetch},
                               {0x0201, OperandFetch},
                               {0x0010, DataRead},
                               {0x0011, DataRead},
                               {0x1205, DummyRead},
                               {0x1205, DataWrite, 0x12}});
  EXPECT_EQ(b.state().a, 0x1E);
  EXPECT_EQ(b.state().x, 0x17);
}

TEST(CpuUnofficial, TasLoadsSWithAAndXAndStoresItLikeSha) {
  // 64doc, "6510/8502 Undocumented Commands", SHS ("SHA and TXS, where X is
  // replaced by (A & X)").
  auto bus = busWith({{0x0200, 0x9B}, {0x0201, 0x00}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x1E, .x = 0x17, .y = 0x05});
  EXPECT_EQ(cpu.stepInstruction(bus), 5u);
  ASSERT_EQ(bus.trace.size(), 5u);
  expectAccess(bus.trace[4], 0x1205, DataWrite);
  EXPECT_EQ(bus.trace[4].value, 0x12);
  EXPECT_EQ(cpu.state().s, 0x16);
  EXPECT_EQ(cpu.state().a, 0x1E);
  EXPECT_EQ(cpu.state().x, 0x17);
  expectStatus(cpu.state(), 0);
}

TEST(CpuUnofficial, AHighByteStoreAcrossAPageWritesToThePageItsValueNames) {
  // 64doc, "6510/8502 Undocumented Commands", the note ("The value to be stored is
  // copied also to ADDR_HI if page boundary is crossed"); NESdev wiki, "Programming
  // with unofficial opcodes", Unimplemented addressing modes ("the high byte of the
  // computed address is ANDed with X").
  auto shx = busWith({{0x0200, 0x9E}, {0x0201, 0xF0}, {0x0202, 0x12}});
  Cpu6502 a(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x03, .y = 0x20});
  EXPECT_EQ(a.stepInstruction(shx), 5u);
  ASSERT_EQ(shx.trace.size(), 5u);
  expectAccess(shx.trace[3], 0x1210, DummyRead);
  expectAccess(shx.trace[4], 0x0310, DataWrite);
  EXPECT_EQ(shx.trace[4].value, 0x03);
  EXPECT_EQ(shx.mem[0x0310], 0x03);
  EXPECT_EQ(shx.mem[0x1310], 0x00);

  auto shy = busWith({{0x0200, 0x9C}, {0x0201, 0xF0}, {0x0202, 0x12}});
  Cpu6502 b(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x20, .y = 0xFF});
  EXPECT_EQ(b.stepInstruction(shy), 5u);
  ASSERT_EQ(shy.trace.size(), 5u);
  expectAccess(shy.trace[3], 0x1210, DummyRead);
  expectAccess(shy.trace[4], 0x1310, DataWrite);
  EXPECT_EQ(shy.trace[4].value, 0x13);

  auto sha = busWith({{0x0200, 0x93}, {0x0201, 0x10}, {0x0010, 0xF0}, {0x0011, 0x12}});
  Cpu6502 c(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0xFF, .x = 0x11, .y = 0x20});
  EXPECT_EQ(c.stepInstruction(sha), 6u);
  ASSERT_EQ(sha.trace.size(), 6u);
  expectAccess(sha.trace[4], 0x1210, DummyRead);
  expectAccess(sha.trace[5], 0x1110, DataWrite);
  EXPECT_EQ(sha.trace[5].value, 0x11);
  EXPECT_EQ(sha.mem[0x1310], 0x00);

  auto tas = busWith({{0x0200, 0x9B}, {0x0201, 0xF0}, {0x0202, 0x12}});
  Cpu6502 d(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x0F, .x = 0xFF, .y = 0x20});
  EXPECT_EQ(d.stepInstruction(tas), 5u);
  ASSERT_EQ(tas.trace.size(), 5u);
  expectAccess(tas.trace[3], 0x1210, DummyRead);
  expectAccess(tas.trace[4], 0x0310, DataWrite);
  EXPECT_EQ(tas.trace[4].value, 0x03);
  EXPECT_EQ(d.state().s, 0x0F);
}

TEST(CpuUnofficial, ShxHeldOnItsFourthCycleStoresXUnmasked) {
  // NESdev wiki, "Programming with unofficial opcodes", Unimplemented addressing
  // modes, SHX ("Unless interrupted by DMC DMA on the 4th clock… data written is
  // ANDed with (high byte of literal address +1)"); NESdev wiki, "DMA", Behavior
  // (the held read repeats).
  auto bus = busWith({{0x0200, 0x9E}, {0x0201, 0x00}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0xFF, .y = 0x05});
  for (int i = 0; i < 3; ++i) cpu.stepCycle(bus);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch}, {0x0201, OperandFetch}, {0x0202, OperandFetch}});
  cpu.setRdyLine(false);
  bus.trace.clear();
  for (int i = 0; i < 2; ++i) cpu.stepCycle(bus);
  expectTrace(bus.trace, {{0x1205, Halted}, {0x1205, Halted}});
  EXPECT_TRUE(cpu.state().rdyHeld);
  EXPECT_EQ(cpu.state().tcu, 3);
  cpu.setRdyLine(true);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0x1205, DummyRead}, {0x1205, DataWrite, 0xFF}});
  EXPECT_EQ(bus.mem[0x1205], 0xFF);
  EXPECT_FALSE(cpu.state().rdyHeld);
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(CpuUnofficial, ShyHeldOnItsFourthCycleStoresYUnmasked) {
  // NESdev wiki, "Programming with unofficial opcodes", Unimplemented addressing
  // modes, SHY ("Exact same as SHX a, Y but swap X and Y").
  auto bus = busWith({{0x0200, 0x9C}, {0x0201, 0x00}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x05, .y = 0xFF});
  for (int i = 0; i < 3; ++i) cpu.stepCycle(bus);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch}, {0x0201, OperandFetch}, {0x0202, OperandFetch}});
  cpu.setRdyLine(false);
  bus.trace.clear();
  for (int i = 0; i < 2; ++i) cpu.stepCycle(bus);
  expectTrace(bus.trace, {{0x1205, Halted}, {0x1205, Halted}});
  EXPECT_TRUE(cpu.state().rdyHeld);
  EXPECT_EQ(cpu.state().tcu, 3);
  cpu.setRdyLine(true);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0x1205, DummyRead}, {0x1205, DataWrite, 0xFF}});
  EXPECT_EQ(bus.mem[0x1205], 0xFF);
  EXPECT_FALSE(cpu.state().rdyHeld);
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(CpuUnofficial, AHoldOnAnotherCycleKeepsTheMask) {
  // NESdev wiki, "Programming with unofficial opcodes", Unimplemented addressing
  // modes ("RDY goes low between fetching the high byte of the address and the
  // dummy read" is the one window).
  auto bus = busWith({{0x0200, 0x9E}, {0x0201, 0x00}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0xFF, .y = 0x05});
  for (int i = 0; i < 2; ++i) cpu.stepCycle(bus);
  cpu.setRdyLine(false);
  bus.trace.clear();
  cpu.stepCycle(bus);
  expectTrace(bus.trace, {{0x0202, Halted}});
  cpu.setRdyLine(true);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 3u);
  expectTrace(bus.trace,
              {{0x0202, OperandFetch}, {0x1205, DummyRead}, {0x1205, DataWrite, 0x13}});
  EXPECT_EQ(bus.mem[0x1205], 0x13);
}

TEST(CpuUnofficial, AHeldShxAcrossAPageWritesToThePageTheMaskedValueNames) {
  // NESdev wiki, "Programming with unofficial opcodes", Unimplemented addressing
  // modes ("the high byte of the computed address is ANDed with X regardless of
  // what RDY does": $0F & $13 = $03); 64doc, "6510/8502 Undocumented Commands", the
  // note.
  auto bus = busWith({{0x0200, 0x9E}, {0x0201, 0xF0}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x0F, .y = 0x20});
  for (int i = 0; i < 3; ++i) cpu.stepCycle(bus);
  cpu.setRdyLine(false);
  bus.trace.clear();
  cpu.stepCycle(bus);
  expectTrace(bus.trace, {{0x1210, Halted}});
  cpu.setRdyLine(true);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0x1210, DummyRead}, {0x0310, DataWrite, 0x0F}});
  EXPECT_EQ(bus.mem[0x0310], 0x0F);
  EXPECT_EQ(bus.mem[0x1310], 0x00);
}

TEST(CpuUnofficial, TheHoldSurvivesASnapshot) {
  // The state value carries the hold, so a core restored from a snapshot taken
  // during it stores what the held core would; NESdev wiki, "Programming with
  // unofficial opcodes", Unimplemented addressing modes, for the byte.
  auto bus = busWith({{0x0200, 0x9E}, {0x0201, 0x00}, {0x0202, 0x12}});
  auto second = busWith({{0x0200, 0x9E}, {0x0201, 0x00}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0xFF, .y = 0x05});
  for (int i = 0; i < 3; ++i) cpu.stepCycle(bus);
  cpu.setRdyLine(false);
  for (int i = 0; i < 2; ++i) cpu.stepCycle(bus);
  expectAccess(bus.trace[3], 0x1205, Halted);
  expectAccess(bus.trace[4], 0x1205, Halted);
  Cpu6502 copy;
  copy.restore(cpu.state());
  copy.setRdyLine(true);
  EXPECT_EQ(copy.stepInstruction(second), 2u);
  expectTrace(second.trace, {{0x1205, DummyRead}, {0x1205, DataWrite, 0xFF}});
  EXPECT_EQ(second.mem[0x1205], 0xFF);
}

TEST(CpuUnofficial, LxaOrsTheConstantIntoAThenAndsTheOperandIntoAAndX) {
  // No More Secrets, "LAX #imm (ATX, LXA, OAL, ANX)": the formula, and its "read the
  // 'magic constant'", "load A and X with same value" and "clear A and X" examples.
  const Result a = run(kLxaImmediate, 0xFF, {.a = 0x00, .x = 0x55});
  EXPECT_EQ(a.state.a, 0xEE);
  EXPECT_EQ(a.state.x, 0xEE);
  expectStatus(a.state, kCpuFlagN);

  const Result b = run(kLxaImmediate, 0x0F, {.a = 0xFF, .x = 0x55});
  EXPECT_EQ(b.state.a, 0x0F);
  EXPECT_EQ(b.state.x, 0x0F);
  expectStatus(b.state, 0);

  const Result c = run(kLxaImmediate, 0x00, {.a = 0x55, .x = 0x55});
  EXPECT_EQ(c.state.a, 0x00);
  EXPECT_EQ(c.state.x, 0x00);
  expectStatus(c.state, kCpuFlagZ);
}

TEST(CpuUnofficial, LasAndsTheOperandIntoSAndLoadsAAndXWithIt) {
  // No More Secrets, "LAS (LAR)": the operation, "N and Z are set as expected by a
  // load instruction", and its example with SP at $F7 and a page of $0F giving $07;
  // 64doc, "Absolute indexed addressing", read instructions (as LAE).
  auto within = busWith({{0x0200, 0xBB}, {0x0201, 0x00}, {0x0202, 0x12}, {0x1205, 0x3C}});
  Cpu6502 a(Cpu6502State{.pc = 0x0200, .s = 0xF7, .y = 0x05});
  EXPECT_EQ(a.stepInstruction(within), 4u);
  expectTrace(within.trace, {{0x0200, OpcodeFetch},
                             {0x0201, OperandFetch},
                             {0x0202, OperandFetch},
                             {0x1205, DataRead}});
  EXPECT_EQ(a.state().a, 0x34);
  EXPECT_EQ(a.state().x, 0x34);
  EXPECT_EQ(a.state().s, 0x34);
  expectStatus(a.state(), 0);

  auto across = busWith({{0x0200, 0xBB}, {0x0201, 0xF0}, {0x0202, 0x12}, {0x1310, 0x0F}});
  Cpu6502 b(Cpu6502State{.pc = 0x0200, .s = 0xF7, .y = 0x20});
  EXPECT_EQ(b.stepInstruction(across), 5u);
  ASSERT_EQ(across.trace.size(), 5u);
  expectAccess(across.trace[3], 0x1210, DummyRead);
  expectAccess(across.trace[4], 0x1310, DataRead);
  EXPECT_EQ(b.state().a, 0x07);
  EXPECT_EQ(b.state().x, 0x07);
  EXPECT_EQ(b.state().s, 0x07);

  auto negative = busWith({{0x0200, 0xBB}, {0x0201, 0x00}, {0x0202, 0x12}, {0x1205, 0x80}});
  Cpu6502 c(Cpu6502State{.pc = 0x0200, .s = 0xFF, .y = 0x05});
  c.stepInstruction(negative);
  EXPECT_EQ(c.state().a, 0x80);
  EXPECT_EQ(c.state().x, 0x80);
  EXPECT_EQ(c.state().s, 0x80);
  expectStatus(c.state(), kCpuFlagN);
}

TEST(CpuUnofficial, JamHaltsTheCoreUntilReset) {
  // No More Secrets, "JAM (KIL, HLT, CIM, CRP)" (the byte after the opcode is
  // fetched, the buses settle on all ones, only a reset restarts, no register or
  // flag changes); NESdev wiki, "CPU unofficial opcodes"; 64doc, "Different CPU
  // types"; Visual6502, the simulation of opcode $02 (the two $FFFE reads and the
  // program counter at $0002).
  auto bus = busWith({{0x0200, 0x02}, {0x0201, 0xEA}, {0xFFFC, 0x00}, {0xFFFD, 0x80},
                      {0xFFFE, 0x11}, {0xFFFF, 0x22}});
  Cpu6502 cpu(Cpu6502State{
      .pc = 0x0200, .s = 0xFD, .a = 0x11, .x = 0x22, .y = 0x33, .p = kCpuFlagC});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, DummyRead},
                          {0xFFFF, DummyRead, 0x22},
                          {0xFFFE, DummyRead, 0x11},
                          {0xFFFE, DummyRead, 0x11},
                          {0xFFFF, DummyRead, 0x22}});
  EXPECT_TRUE(cpu.state().jammed);
  EXPECT_FALSE(cpu.atInstructionBoundary());
  EXPECT_EQ(cpu.state().pc, 0x0202);

  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 1u);
  expectTrace(bus.trace, {{0xFFFF, DummyRead}});
  EXPECT_EQ(cpu.stepInstruction(bus), 1u);
  EXPECT_EQ(cpu.state().a, 0x11);
  EXPECT_EQ(cpu.state().x, 0x22);
  EXPECT_EQ(cpu.state().y, 0x33);
  EXPECT_EQ(cpu.state().s, 0xFD);
  expectStatus(cpu.state(), kCpuFlagC);

  cpu.reset();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  EXPECT_EQ(cpu.state().pc, 0x8000);
  EXPECT_FALSE(cpu.state().jammed);
  EXPECT_EQ(cpu.state().a, 0x11);
  EXPECT_EQ(cpu.state().x, 0x22);
  EXPECT_EQ(cpu.state().y, 0x33);
  EXPECT_EQ(cpu.state().s, 0xFA);
  EXPECT_NE(cpu.state().p & neasant::kCpuFlagI, 0);
}

TEST(CpuUnofficial, EveryJamOpcodeJams) {
  // No More Secrets, "JAM (KIL, HLT, CIM, CRP)" (the twelve opcodes); 64doc's
  // matrix legend (t).
  for (const std::uint8_t opcode : std::initializer_list<std::uint8_t>{
           0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x92, 0xB2, 0xD2, 0xF2}) {
    SCOPED_TRACE(::testing::Message() << "opcode " << int{opcode});
    auto bus = busWith({{0x0200, opcode}});
    Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
    EXPECT_EQ(cpu.stepInstruction(bus), 6u);
    EXPECT_TRUE(cpu.state().jammed);
    EXPECT_EQ(cpu.state().pc, 0x0202);
  }
}

TEST(CpuUnofficial, AnInterruptDoesNotReleaseAJammedCore) {
  // No More Secrets, "JAM (KIL, HLT, CIM, CRP)" ("No hardware interrupts will
  // execute either. Only a reset will restart execution"); 64doc, "Different CPU
  // types" ("terminate only by reset").
  auto bus = busWith({{0x0200, 0x02}, {0x0201, 0xEA}, {0xFFFC, 0x00}, {0xFFFD, 0x80},
                      {0xFFFE, 0x11}, {0xFFFF, 0x22}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .p = 0x00, .irqLine = true});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  EXPECT_TRUE(cpu.state().jammed);
  cpu.setNmiLine(true);
  bus.trace.clear();
  for (int i = 0; i < 3; ++i) cpu.stepCycle(bus);
  expectTrace(bus.trace, {{0xFFFF, DummyRead}, {0xFFFF, DummyRead}, {0xFFFF, DummyRead}});
  EXPECT_TRUE(cpu.state().jammed);
  EXPECT_EQ(cpu.state().servicing, neasant::CpuSequence::None);
  EXPECT_EQ(cpu.state().pc, 0x0202);
}

TEST(CpuUnofficial, ShaAndTasHeldOnTheirPageFixCycleStoreUnmasked) {
  // No More Secrets, "'unstable address high byte' group" (cycle N is the fourth
  // for $9F and $9B, the fifth for $93; with DMA in cycle N the value written is
  // Value), "SHA (AXA, AHX, TEA)" and "TAS (XAS, SHS)", Instabilities.
  auto absolute = busWith({{0x0200, 0x9F}, {0x0201, 0x00}, {0x0202, 0x12}});
  Cpu6502 a(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x1E, .x = 0x17, .y = 0x05});
  for (int i = 0; i < 3; ++i) a.stepCycle(absolute);
  a.setRdyLine(false);
  absolute.trace.clear();
  a.stepCycle(absolute);
  expectTrace(absolute.trace, {{0x1205, Halted}});
  a.setRdyLine(true);
  absolute.trace.clear();
  EXPECT_EQ(a.stepInstruction(absolute), 2u);
  expectTrace(absolute.trace, {{0x1205, DummyRead}, {0x1205, DataWrite, 0x16}});
  EXPECT_EQ(absolute.mem[0x1205], 0x16);

  auto indirect = busWith({{0x0200, 0x93}, {0x0201, 0x10}, {0x0010, 0x00}, {0x0011, 0x12}});
  Cpu6502 b(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x1E, .x = 0x17, .y = 0x05});
  for (int i = 0; i < 4; ++i) b.stepCycle(indirect);
  b.setRdyLine(false);
  indirect.trace.clear();
  b.stepCycle(indirect);
  expectTrace(indirect.trace, {{0x1205, Halted}});
  b.setRdyLine(true);
  indirect.trace.clear();
  EXPECT_EQ(b.stepInstruction(indirect), 2u);
  expectTrace(indirect.trace, {{0x1205, DummyRead}, {0x1205, DataWrite, 0x16}});

  auto tas = busWith({{0x0200, 0x9B}, {0x0201, 0x00}, {0x0202, 0x12}});
  Cpu6502 c(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x1E, .x = 0x17, .y = 0x05});
  for (int i = 0; i < 3; ++i) c.stepCycle(tas);
  c.setRdyLine(false);
  tas.trace.clear();
  c.stepCycle(tas);
  expectTrace(tas.trace, {{0x1205, Halted}});
  c.setRdyLine(true);
  tas.trace.clear();
  EXPECT_EQ(c.stepInstruction(tas), 2u);
  expectTrace(tas.trace, {{0x1205, DummyRead}, {0x1205, DataWrite, 0x16}});
  EXPECT_EQ(tas.mem[0x1205], 0x16);
  EXPECT_EQ(c.state().s, 0x16);
}

}  // namespace
