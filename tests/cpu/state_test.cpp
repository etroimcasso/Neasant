#include "nescore/cpu/cpu6502.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

// The 6502 core's state value, its bus concept, cycle stepping, the power-on and
// reset sequence, the RDY halt and the jammed core. Each expectation names the
// document it is read from: the NESdev wiki pages "CPU power up state", "CPU
// interrupts", "DMA" and "Status flags", and 64doc.

namespace {

using nescore::Cpu6502;
using nescore::Cpu6502State;
using nescore::CpuBus;
using nescore::CpuSequence;
using nescore::CycleKind;
using nescore::kCpuFlagC;
using nescore::kCpuFlagD;
using nescore::kCpuFlagI;
using nescore::kCpuFlagN;
using nescore::kCpuFlagU;
using nescore::kCpuFlagV;
using nescore::kCpuFlagZ;
using nescore::powerOn;

// A flat 64 KiB memory that records every access the core makes.
struct Access {
  std::uint16_t address;
  std::uint8_t value;
  CycleKind kind;
};

struct TraceBus {
  std::array<std::uint8_t, 65536> mem{};
  std::vector<Access> trace;

  std::uint8_t read(std::uint16_t address, CycleKind kind) {
    trace.push_back({address, mem[address], kind});
    return mem[address];
  }
  void write(std::uint16_t address, std::uint8_t value, CycleKind kind) {
    mem[address] = value;
    trace.push_back({address, value, kind});
  }
};

// A type with a read member and no write member.
struct ReadOnlyBus {
  std::uint8_t read(std::uint16_t, CycleKind) { return 0; }
};

TraceBus busWith(std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> ram) {
  TraceBus bus;
  for (const auto& [address, value] : ram) bus.mem[address] = value;
  return bus;
}

void expectAccess(const Access& access, std::uint16_t address, CycleKind kind) {
  EXPECT_EQ(access.address, address);
  EXPECT_EQ(access.kind, kind);
}

bool isWrite(CycleKind kind) {
  return kind == CycleKind::DataWrite || kind == CycleKind::RmwDummyWrite ||
         kind == CycleKind::RmwWrite || kind == CycleKind::StackPush;
}

constexpr std::uint8_t kNop = 0xEA;

TEST(CpuState, StateIsAValue) {
  const Cpu6502State original{.pc = 0x1234, .s = 0xFD, .a = 0x56, .x = 0x78, .y = 0x9A,
                              .p = kCpuFlagI, .ir = 0xEA, .tcu = 1};
  Cpu6502State copy = original;
  EXPECT_EQ(copy, original);
  copy.tcu = 0;
  EXPECT_NE(copy, original);

  Cpu6502 cpu;
  cpu.restore(original);
  EXPECT_EQ(cpu.state(), original);
}

TEST(CpuState, ConceptAcceptsAFlatBusAndRejectsAReadOnlyType) {
  static_assert(CpuBus<TraceBus>);
  static_assert(!CpuBus<ReadOnlyBus>);
}

TEST(CpuState, PowerOnMatchesTheDocumentedRegisters) {
  // NESdev wiki, "CPU power up state", CPU: A, X, Y zero; S $00 before the reset
  // sequence's three decrements; I set, every other flag clear.
  const Cpu6502State state = powerOn();
  EXPECT_EQ(state.a, 0);
  EXPECT_EQ(state.x, 0);
  EXPECT_EQ(state.y, 0);
  EXPECT_EQ(state.s, 0x00);
  EXPECT_EQ(state.p, kCpuFlagI);
  EXPECT_EQ(state.servicing, CpuSequence::Reset);
  EXPECT_EQ(state.tcu, 0);
  EXPECT_TRUE(state.rdyLine);
  EXPECT_FALSE(state.jammed);
  EXPECT_TRUE(Cpu6502(state).atInstructionBoundary());
}

TEST(CpuState, TheResetSequenceTakesSevenCyclesWithItsKinds) {
  // NESdev wiki, "CPU interrupts", IRQ and NMI tick-by-tick execution: a reset runs
  // the interrupt sequence with its writes suppressed, taking S down three times
  // with memory untouched, then reads the vector. "CPU power up state": S ends at
  // $FD and I is set.
  auto bus = busWith({{0xFFFC, 0x00}, {0xFFFD, 0x80}});
  Cpu6502 cpu(powerOn());
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  ASSERT_EQ(bus.trace.size(), 7u);
  expectAccess(bus.trace[0], 0x0000, CycleKind::DummyRead);
  expectAccess(bus.trace[1], 0x0000, CycleKind::DummyRead);
  expectAccess(bus.trace[2], 0x0100, CycleKind::DummyRead);
  expectAccess(bus.trace[3], 0x01FF, CycleKind::DummyRead);
  expectAccess(bus.trace[4], 0x01FE, CycleKind::DummyRead);
  expectAccess(bus.trace[5], 0xFFFC, CycleKind::VectorRead);
  expectAccess(bus.trace[6], 0xFFFD, CycleKind::VectorRead);
  for (const Access& access : bus.trace) EXPECT_FALSE(isWrite(access.kind));
  EXPECT_EQ(cpu.state().pc, 0x8000);
  EXPECT_EQ(cpu.state().s, 0xFD);
  EXPECT_NE(cpu.state().p & kCpuFlagI, 0);
  EXPECT_EQ(cpu.state().servicing, CpuSequence::None);
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(CpuState, ResetOnARunningCoreKeepsTheRegistersItFound) {
  // NESdev wiki, "CPU power up state", CPU: after a reset A, X, Y, C, Z, D, V and N
  // are unchanged, S -= 3 and I is 1. 64doc, "6510 features": all registers except
  // the program counter remain unmodified after -RESET.
  const std::uint8_t flags = kCpuFlagN | kCpuFlagV | kCpuFlagD | kCpuFlagZ | kCpuFlagC;
  auto bus = busWith({{0xFFFC, 0x34}, {0xFFFD, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x4000, .s = 0x80, .a = 0x11, .x = 0x22, .y = 0x33,
                           .p = flags, .jammed = true});
  cpu.reset();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  EXPECT_EQ(cpu.state().a, 0x11);
  EXPECT_EQ(cpu.state().x, 0x22);
  EXPECT_EQ(cpu.state().y, 0x33);
  EXPECT_EQ(cpu.state().p, flags | kCpuFlagI);
  EXPECT_EQ(cpu.state().s, 0x7D);
  EXPECT_EQ(cpu.state().pc, 0x1234);
  EXPECT_FALSE(cpu.state().jammed);
}

TEST(CpuState, NopIsTwoCyclesWithItsKinds) {
  // 64doc, "Accumulator or implied addressing": fetch the opcode and increment PC,
  // then read the next instruction byte and throw it away.
  auto bus = busWith({{0x0200, kNop}});
  const Cpu6502State before{.pc = 0x0200, .s = 0xFD, .a = 0x01, .x = 0x02, .y = 0x03,
                            .p = kCpuFlagI | kCpuFlagC};
  Cpu6502 cpu(before);
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  ASSERT_EQ(bus.trace.size(), 2u);
  expectAccess(bus.trace[0], 0x0200, CycleKind::OpcodeFetch);
  expectAccess(bus.trace[1], 0x0201, CycleKind::DummyRead);
  EXPECT_EQ(cpu.state().pc, 0x0201);
  EXPECT_EQ(cpu.state().s, before.s);
  EXPECT_EQ(cpu.state().a, before.a);
  EXPECT_EQ(cpu.state().x, before.x);
  EXPECT_EQ(cpu.state().y, before.y);
  EXPECT_EQ(cpu.state().p, before.p);
}

TEST(CpuState, RdyLowOnNopsSecondCycleRepeatsThatReadAsHalted) {
  // NESdev wiki, "DMA", Behavior: while RDY is deasserted the core repeats the read
  // cycle it was halted on, making no forward progress, and performs that read
  // when released.
  auto bus = busWith({{0x0200, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  cpu.stepCycle(bus);  // the opcode fetch
  cpu.setRdyLine(false);
  Cpu6502State held = cpu.state();
  held.rdyHeld = true;  // the one mark a held cycle leaves
  for (int i = 0; i < 3; ++i) cpu.stepCycle(bus);
  ASSERT_EQ(bus.trace.size(), 4u);
  for (std::size_t i = 1; i < 4; ++i) expectAccess(bus.trace[i], 0x0201, CycleKind::Halted);
  EXPECT_EQ(cpu.state().tcu, 1);
  EXPECT_EQ(cpu.state(), held);

  cpu.setRdyLine(true);
  cpu.stepCycle(bus);
  ASSERT_EQ(bus.trace.size(), 5u);
  expectAccess(bus.trace[4], 0x0201, CycleKind::DummyRead);
  EXPECT_TRUE(cpu.atInstructionBoundary());
  EXPECT_FALSE(cpu.state().rdyHeld);
}

TEST(CpuState, RdyLowHoldsTheOpcodeFetch) {
  // NESdev wiki, "DMA", Behavior: the opcode fetch is a read cycle and halts like
  // any other.
  auto bus = busWith({{0x0200, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .ir = 0x18});
  cpu.setRdyLine(false);
  Cpu6502State held = cpu.state();
  held.rdyHeld = true;  // the one mark a held cycle leaves
  cpu.stepCycle(bus);
  ASSERT_EQ(bus.trace.size(), 1u);
  expectAccess(bus.trace[0], 0x0200, CycleKind::Halted);
  EXPECT_EQ(cpu.state().ir, 0x18);
  EXPECT_EQ(cpu.state().tcu, 0);
  EXPECT_EQ(cpu.stepInstruction(bus), 1u);
  EXPECT_EQ(cpu.state(), held);
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(CpuState, AWriteCycleProceedsWhileRdyIsLow) {
  // NESdev wiki, "DMA", Behavior: the CPU allows a halt only on read cycles and
  // ignores it while writing. 64doc, "6510 features": -RDY is ignored during
  // writes. An IRQ taken after a NOP gives three consecutive writes to halt across.
  auto bus = busWith({{0x0200, kNop}, {0xFFFE, 0x00}, {0xFFFF, 0x90}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .p = 0x00, .irqLine = true});
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);  // the NOP
  cpu.stepCycle(bus);
  cpu.stepCycle(bus);
  ASSERT_EQ(bus.trace.size(), 4u);
  expectAccess(bus.trace[2], 0x0201, CycleKind::DummyRead);
  expectAccess(bus.trace[3], 0x0201, CycleKind::DummyRead);

  cpu.setRdyLine(false);
  for (int i = 0; i < 3; ++i) cpu.stepCycle(bus);
  ASSERT_EQ(bus.trace.size(), 7u);
  expectAccess(bus.trace[4], 0x01FD, CycleKind::StackPush);
  expectAccess(bus.trace[5], 0x01FC, CycleKind::StackPush);
  expectAccess(bus.trace[6], 0x01FB, CycleKind::StackPush);
  EXPECT_EQ(cpu.state().s, 0xFA);
  EXPECT_EQ(bus.mem[0x01FB], kCpuFlagU);  // NESdev wiki, "Status flags", The B flag

  const std::uint8_t tcu = cpu.state().tcu;
  cpu.stepCycle(bus);
  ASSERT_EQ(bus.trace.size(), 8u);
  expectAccess(bus.trace[7], 0xFFFE, CycleKind::Halted);
  EXPECT_EQ(cpu.state().tcu, tcu);

  cpu.setRdyLine(true);
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectAccess(bus.trace[8], 0xFFFE, CycleKind::VectorRead);
  expectAccess(bus.trace[9], 0xFFFF, CycleKind::VectorRead);
  EXPECT_EQ(cpu.state().pc, 0x9000);
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(CpuState, StepInstructionMidSequenceFinishesIt) {
  auto nopBus = busWith({{0x0200, kNop}});
  Cpu6502 nop(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  nop.stepCycle(nopBus);
  EXPECT_EQ(nop.stepInstruction(nopBus), 1u);
  EXPECT_TRUE(nop.atInstructionBoundary());

  TraceBus resetBus;
  Cpu6502 reset(powerOn());
  for (int i = 0; i < 3; ++i) reset.stepCycle(resetBus);
  EXPECT_EQ(reset.stepInstruction(resetBus), 4u);
  EXPECT_TRUE(reset.atInstructionBoundary());
}

TEST(CpuState, RestoreMidSequenceResumesOnTheSameCycle) {
  const TraceBus start = busWith({{0xFFFC, 0x00}, {0xFFFD, 0xC0}});
  TraceBus firstBus = start;
  Cpu6502 first(powerOn());
  for (int i = 0; i < 4; ++i) first.stepCycle(firstBus);

  TraceBus secondBus = start;
  Cpu6502 second;
  second.restore(first.state());

  first.stepInstruction(firstBus);
  second.stepInstruction(secondBus);
  ASSERT_EQ(firstBus.trace.size(), 4u + secondBus.trace.size());
  for (std::size_t i = 0; i < secondBus.trace.size(); ++i) {
    expectAccess(secondBus.trace[i], firstBus.trace[4 + i].address, firstBus.trace[4 + i].kind);
  }
  EXPECT_EQ(first.state(), second.state());
}

TEST(CpuState, AJammedCoreRepeatsADummyReadOfItsLatchedAddress) {
  auto bus = busWith({{0xFFFC, 0x00}, {0xFFFD, 0x80}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0201, .s = 0xFD, .tcu = 3, .ea = 0xFFFF, .jammed = true});
  for (int i = 0; i < 3; ++i) cpu.stepCycle(bus);
  ASSERT_EQ(bus.trace.size(), 3u);
  for (const Access& access : bus.trace) expectAccess(access, 0xFFFF, CycleKind::DummyRead);
  EXPECT_EQ(cpu.state().tcu, 3);
  EXPECT_EQ(cpu.stepInstruction(bus), 1u);

  cpu.reset();
  EXPECT_FALSE(cpu.state().jammed);
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  EXPECT_EQ(cpu.state().pc, 0x8000);
}

}  // namespace
