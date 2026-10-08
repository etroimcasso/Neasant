#include "nescore/cpu/cpu6502.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

// Every addressing mode and control-flow sequence of the 6502, one instruction at a
// time: the cycle count, every bus access with its address and kind, and the
// registers and memory the instruction leaves. Each case names the section of
// 64doc its trace is read from; the status-byte cases also name the NESdev wiki
// page. Every program is placed at $0200 with S at $FD unless the case says
// otherwise.

namespace {

using nescore::Cpu6502;
using nescore::Cpu6502State;
using nescore::CycleKind;
using nescore::kCpuFlagN;
using nescore::kCpuFlagZ;
using nescore::cpu_vectors::Access;
using nescore::cpu_vectors::RecordingBus;
using enum nescore::CycleKind;

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
    if (step.value >= 0) EXPECT_EQ(trace[cycle].value, step.value);
    ++cycle;
  }
}

// ---- read, write and read-modify-write, by mode ---------------------------------

TEST(CpuModes, ImmediateReadIsTwoCycles) {
  // 64doc, "Immediate addressing".
  auto bus = busWith({{0x0200, 0xA9}, {0x0201, 0x42}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch}, {0x0201, OperandFetch}});
  EXPECT_EQ(cpu.state().a, 0x42);
  EXPECT_EQ(cpu.state().pc, 0x0202);
}

TEST(CpuModes, ZeroPageReadIsThreeCycles) {
  // 64doc, "Zero page addressing".
  auto bus = busWith({{0x0200, 0xA5}, {0x0201, 0x10}, {0x0010, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  EXPECT_EQ(cpu.stepInstruction(bus), 3u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch}, {0x0201, OperandFetch}, {0x0010, DataRead}});
  EXPECT_EQ(cpu.state().a, 0x7F);
}

TEST(CpuModes, ZeroPageWriteIsThreeCycles) {
  // 64doc, "Zero page addressing".
  auto bus = busWith({{0x0200, 0x85}, {0x0201, 0x10}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x5A});
  EXPECT_EQ(cpu.stepInstruction(bus), 3u);
  expectTrace(bus.trace,
              {{0x0200, OpcodeFetch}, {0x0201, OperandFetch}, {0x0010, DataWrite, 0x5A}});
  EXPECT_EQ(bus.mem[0x0010], 0x5A);
}

TEST(CpuModes, ZeroPageReadModifyWriteWritesTwice) {
  // 64doc, "Zero page addressing"; "6510 features": a read-modify-write writes the
  // unmodified value, then the modified one.
  auto bus = busWith({{0x0200, 0xE6}, {0x0201, 0x10}, {0x0010, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  EXPECT_EQ(cpu.stepInstruction(bus), 5u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0010, RmwRead, 0x7F},
                          {0x0010, RmwDummyWrite, 0x7F},
                          {0x0010, RmwWrite, 0x80}});
  EXPECT_EQ(bus.mem[0x0010], 0x80);
  EXPECT_NE(cpu.state().p & kCpuFlagN, 0);
}

TEST(CpuModes, ZeroPageIndexedReadsTheUnindexedAddressFirstAndWrapsInPageZero) {
  // 64doc, "Zero page indexed addressing": the base address is read and discarded
  // while the index is added, and the sum stays in page zero.
  auto bus = busWith({{0x0200, 0xB5}, {0x0201, 0xF0}, {0x0010, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 4u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x00F0, DummyRead},
                          {0x0010, DataRead}});
  EXPECT_EQ(cpu.state().a, 0x7F);
}

TEST(CpuModes, ZeroPageIndexedWriteIsFourCycles) {
  // 64doc, "Zero page indexed addressing". STX zero page,Y.
  auto bus = busWith({{0x0200, 0x96}, {0x0201, 0xF0}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x5A, .y = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 4u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x00F0, DummyRead},
                          {0x0010, DataWrite, 0x5A}});
}

TEST(CpuModes, ZeroPageIndexedReadModifyWriteIsSixCycles) {
  // 64doc, "Zero page indexed addressing".
  auto bus = busWith({{0x0200, 0xF6}, {0x0201, 0xF0}, {0x0010, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x00F0, DummyRead},
                          {0x0010, RmwRead},
                          {0x0010, RmwDummyWrite, 0x7F},
                          {0x0010, RmwWrite, 0x80}});
}

TEST(CpuModes, AbsoluteReadIsFourCycles) {
  // 64doc, "Absolute addressing".
  auto bus = busWith({{0x0200, 0xAD}, {0x0201, 0x34}, {0x0202, 0x12}, {0x1234, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  EXPECT_EQ(cpu.stepInstruction(bus), 4u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x1234, DataRead}});
  EXPECT_EQ(cpu.state().a, 0x7F);
}

TEST(CpuModes, AbsoluteWriteIsFourCycles) {
  // 64doc, "Absolute addressing".
  auto bus = busWith({{0x0200, 0x8D}, {0x0201, 0x34}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x5A});
  EXPECT_EQ(cpu.stepInstruction(bus), 4u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x1234, DataWrite, 0x5A}});
}

TEST(CpuModes, AbsoluteReadModifyWriteIsSixCycles) {
  // 64doc, "Absolute addressing".
  auto bus = busWith({{0x0200, 0xEE}, {0x0201, 0x34}, {0x0202, 0x12}, {0x1234, 0xFF}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x1234, RmwRead, 0xFF},
                          {0x1234, RmwDummyWrite, 0xFF},
                          {0x1234, RmwWrite, 0x00}});
  EXPECT_NE(cpu.state().p & kCpuFlagZ, 0);
}

TEST(CpuModes, AbsoluteIndexedReadWithinThePageIsFourCycles) {
  // 64doc, "Absolute indexed addressing".
  auto bus = busWith({{0x0200, 0xBD}, {0x0201, 0xF0}, {0x0202, 0x12}, {0x12F5, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x05});
  EXPECT_EQ(cpu.stepInstruction(bus), 4u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x12F5, DataRead}});
  EXPECT_EQ(cpu.state().a, 0x7F);
}

TEST(CpuModes, AbsoluteIndexedReadAcrossAPageReadsTheWrongPageFirst) {
  // 64doc, "Absolute indexed addressing"; "6510 features": across a page the first
  // read lands one page low. LDA absolute,Y.
  auto bus = busWith({{0x0200, 0xB9}, {0x0201, 0xF0}, {0x0202, 0x12}, {0x1310, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .y = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 5u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x1210, DummyRead},
                          {0x1310, DataRead}});
  EXPECT_EQ(cpu.state().a, 0x7F);
}

TEST(CpuModes, AbsoluteIndexedWriteReadsFirstWithinThePage) {
  // 64doc, "Absolute indexed addressing": a write reads the effective address
  // before it writes, whether or not a page is crossed.
  auto bus = busWith({{0x0200, 0x9D}, {0x0201, 0xF0}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x5A, .x = 0x05});
  EXPECT_EQ(cpu.stepInstruction(bus), 5u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x12F5, DummyRead},
                          {0x12F5, DataWrite, 0x5A}});
}

TEST(CpuModes, AbsoluteIndexedWriteReadsTheWrongPageFirstAcrossAPage) {
  // 64doc, "Absolute indexed addressing". STA absolute,Y.
  auto bus = busWith({{0x0200, 0x99}, {0x0201, 0xF0}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x5A, .y = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 5u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x1210, DummyRead},
                          {0x1310, DataWrite, 0x5A}});
  EXPECT_EQ(bus.mem[0x1210], 0x00);
}

TEST(CpuModes, AbsoluteIndexedReadModifyWriteRereadsWithinThePage) {
  // 64doc, "Absolute indexed addressing": a read-modify-write reads the effective
  // address, then reads it again as its own first cycle.
  auto bus = busWith({{0x0200, 0xFE}, {0x0201, 0xF0}, {0x0202, 0x12}, {0x12F5, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x05});
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x12F5, DummyRead},
                          {0x12F5, RmwRead},
                          {0x12F5, RmwDummyWrite, 0x7F},
                          {0x12F5, RmwWrite, 0x80}});
}

TEST(CpuModes, AbsoluteIndexedReadModifyWriteAcrossAPageIsSevenCycles) {
  // 64doc, "Absolute indexed addressing".
  auto bus = busWith({{0x0200, 0xFE}, {0x0201, 0xF0}, {0x0202, 0x12}, {0x1310, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x1210, DummyRead},
                          {0x1310, RmwRead},
                          {0x1310, RmwDummyWrite},
                          {0x1310, RmwWrite, 0x80}});
}

TEST(CpuModes, AbsoluteIndexedYReadModifyWriteRunsTheSameSequence) {
  // 64doc, "Absolute indexed addressing". SLO absolute,Y, an unofficial opcode
  // that shifts the byte left.
  auto bus = busWith({{0x0200, 0x1B}, {0x0201, 0xF0}, {0x0202, 0x12}, {0x1310, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .y = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x1210, DummyRead},
                          {0x1310, RmwRead},
                          {0x1310, RmwDummyWrite},
                          {0x1310, RmwWrite, 0xFE}});
}

TEST(CpuModes, IndexedIndirectReadsTheUnindexedPointerFirst) {
  // 64doc, "Indexed indirect addressing".
  auto bus = busWith({{0x0200, 0xA1}, {0x0201, 0xF0}, {0x0010, 0x34}, {0x0011, 0x12},
                      {0x1234, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x00F0, DummyRead},
                          {0x0010, DataRead},
                          {0x0011, DataRead},
                          {0x1234, DataRead}});
  EXPECT_EQ(cpu.state().a, 0x7F);
}

TEST(CpuModes, IndexedIndirectPointerWrapsInPageZero) {
  // 64doc, "6510 features": the pointer's high byte is read from $00 when its low
  // byte is at $FF.
  auto bus = busWith({{0x0200, 0xA1}, {0x0201, 0xFF}, {0x00FF, 0x34}, {0x0000, 0x12},
                      {0x1234, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x00});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x00FF, DummyRead},
                          {0x00FF, DataRead},
                          {0x0000, DataRead},
                          {0x1234, DataRead}});
}

TEST(CpuModes, IndexedIndirectWriteIsSixCycles) {
  // 64doc, "Indexed indirect addressing".
  auto bus = busWith({{0x0200, 0x81}, {0x0201, 0xF0}, {0x0010, 0x34}, {0x0011, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x5A, .x = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x00F0, DummyRead},
                          {0x0010, DataRead},
                          {0x0011, DataRead},
                          {0x1234, DataWrite, 0x5A}});
}

TEST(CpuModes, IndexedIndirectReadModifyWriteIsEightCycles) {
  // 64doc, "Indexed indirect addressing". SLO (zero page,X), an unofficial opcode
  // that shifts the byte left.
  auto bus = busWith({{0x0200, 0x03}, {0x0201, 0xF0}, {0x0010, 0x34}, {0x0011, 0x12},
                      {0x1234, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .x = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 8u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x00F0, DummyRead},
                          {0x0010, DataRead},
                          {0x0011, DataRead},
                          {0x1234, RmwRead},
                          {0x1234, RmwDummyWrite, 0x7F},
                          {0x1234, RmwWrite, 0xFE}});
}

TEST(CpuModes, IndirectIndexedReadWithinThePageIsFiveCycles) {
  // 64doc, "Indirect indexed addressing".
  auto bus = busWith({{0x0200, 0xB1}, {0x0201, 0x10}, {0x0010, 0xF0}, {0x0011, 0x12},
                      {0x12F5, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .y = 0x05});
  EXPECT_EQ(cpu.stepInstruction(bus), 5u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0010, DataRead},
                          {0x0011, DataRead},
                          {0x12F5, DataRead}});
  EXPECT_EQ(cpu.state().a, 0x7F);
}

TEST(CpuModes, IndirectIndexedReadAcrossAPageReadsTheWrongPageFirst) {
  // 64doc, "Indirect indexed addressing"; "6510 features".
  auto bus = busWith({{0x0200, 0xB1}, {0x0201, 0x10}, {0x0010, 0xF0}, {0x0011, 0x12},
                      {0x1310, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .y = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0010, DataRead},
                          {0x0011, DataRead},
                          {0x1210, DummyRead},
                          {0x1310, DataRead}});
  EXPECT_EQ(cpu.state().a, 0x7F);
}

TEST(CpuModes, IndirectIndexedPointerWrapsInPageZero) {
  // 64doc, "6510 features": LDA ($FF),Y reads the base address from $FF and $00.
  auto bus = busWith({{0x0200, 0xB1}, {0x0201, 0xFF}, {0x00FF, 0x34}, {0x0000, 0x12},
                      {0x1234, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .y = 0x00});
  EXPECT_EQ(cpu.stepInstruction(bus), 5u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x00FF, DataRead},
                          {0x0000, DataRead},
                          {0x1234, DataRead}});
}

TEST(CpuModes, IndirectIndexedWriteAlwaysReadsFirst) {
  // 64doc, "Indirect indexed addressing".
  auto bus = busWith({{0x0200, 0x91}, {0x0201, 0x10}, {0x0010, 0xF0}, {0x0011, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x5A, .y = 0x05});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0010, DataRead},
                          {0x0011, DataRead},
                          {0x12F5, DummyRead},
                          {0x12F5, DataWrite, 0x5A}});
}

TEST(CpuModes, IndirectIndexedReadModifyWriteIsEightCycles) {
  // 64doc, "Indirect indexed addressing". SLO (zero page),Y, an unofficial opcode
  // that shifts the byte left.
  auto bus = busWith({{0x0200, 0x13}, {0x0201, 0x10}, {0x0010, 0xF0}, {0x0011, 0x12},
                      {0x1310, 0x7F}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .y = 0x20});
  EXPECT_EQ(cpu.stepInstruction(bus), 8u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0010, DataRead},
                          {0x0011, DataRead},
                          {0x1210, DummyRead},
                          {0x1310, RmwRead},
                          {0x1310, RmwDummyWrite},
                          {0x1310, RmwWrite, 0xFE}});
}

// ---- branches --------------------------------------------------------------------

TEST(CpuModes, BranchNotTakenIsTwoCycles) {
  // 64doc, "Relative addressing".
  auto bus = busWith({{0x0200, 0xD0}, {0x0201, 0x10}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .p = kCpuFlagZ});
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch}, {0x0201, OperandFetch}});
  EXPECT_EQ(cpu.state().pc, 0x0202);
}

TEST(CpuModes, BranchTakenWithinThePageIsThreeCycles) {
  // 64doc, "Relative addressing".
  auto bus = busWith({{0x0200, 0xD0}, {0x0201, 0x10}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .p = 0x00});
  EXPECT_EQ(cpu.stepInstruction(bus), 3u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch}, {0x0201, OperandFetch}, {0x0202, DummyRead}});
  EXPECT_EQ(cpu.state().pc, 0x0212);
}

TEST(CpuModes, BranchTakenForwardAcrossAPageReadsTheWrongPageFirst) {
  // 64doc, "Relative addressing"; "6510 features": across a page the processor
  // first reads the target's offset in the old page.
  auto bus = busWith({{0x02F0, 0xD0}, {0x02F1, 0x20}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x02F0, .s = 0xFD, .p = 0x00});
  EXPECT_EQ(cpu.stepInstruction(bus), 4u);
  expectTrace(bus.trace, {{0x02F0, OpcodeFetch},
                          {0x02F1, OperandFetch},
                          {0x02F2, DummyRead},
                          {0x0212, DummyRead}});
  EXPECT_EQ(cpu.state().pc, 0x0312);
}

TEST(CpuModes, BranchTakenBackwardAcrossAPageReadsTheWrongPageFirst) {
  // 64doc, "Relative addressing".
  auto bus = busWith({{0x0200, 0xD0}, {0x0201, 0xF0}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .p = 0x00});
  EXPECT_EQ(cpu.stepInstruction(bus), 4u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, DummyRead},
                          {0x02F2, DummyRead}});
  EXPECT_EQ(cpu.state().pc, 0x01F2);
}

// ---- jumps, calls, returns and the stack -----------------------------------------

TEST(CpuModes, JumpAbsoluteIsThreeCycles) {
  // 64doc, "Absolute addressing", JMP.
  auto bus = busWith({{0x0200, 0x4C}, {0x0201, 0x34}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  EXPECT_EQ(cpu.stepInstruction(bus), 3u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch}, {0x0201, OperandFetch}, {0x0202, OperandFetch}});
  EXPECT_EQ(cpu.state().pc, 0x1234);
}

TEST(CpuModes, JumpIndirectReadsBothBytesFromTheSamePage) {
  // 64doc, "Absolute indirect addressing (JMP)"; "6510 features": JMP ($12FF)
  // reads its high byte from $1200.
  auto bus = busWith({{0x0200, 0x6C}, {0x0201, 0xFF}, {0x0202, 0x12}, {0x12FF, 0x34},
                      {0x1200, 0x56}, {0x1300, 0x99}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  EXPECT_EQ(cpu.stepInstruction(bus), 5u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x12FF, DataRead},
                          {0x1200, DataRead}});
  EXPECT_EQ(cpu.state().pc, 0x5634);
}

TEST(CpuModes, JsrPushesTheAddressOfItsLastByte) {
  // 64doc, "Instructions accessing the stack", JSR.
  auto bus = busWith({{0x0200, 0x20}, {0x0201, 0x34}, {0x0202, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x01FD, DummyRead},
                          {0x01FD, StackPush, 0x02},
                          {0x01FC, StackPush, 0x02},
                          {0x0202, OperandFetch}});
  EXPECT_EQ(cpu.state().pc, 0x1234);
  EXPECT_EQ(cpu.state().s, 0xFB);
}

TEST(CpuModes, RtsPullsAndStepsPastTheReturnAddress) {
  // 64doc, "Instructions accessing the stack", RTS.
  auto bus = busWith({{0x0200, 0x60}, {0x01FC, 0x02}, {0x01FD, 0x02}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFB});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, DummyRead},
                          {0x01FB, DummyRead},
                          {0x01FC, StackPull},
                          {0x01FD, StackPull},
                          {0x0202, DummyRead}});
  EXPECT_EQ(cpu.state().pc, 0x0203);
  EXPECT_EQ(cpu.state().s, 0xFD);
}

TEST(CpuModes, RtiPullsStatusThenTheProgramCounter) {
  // 64doc, "Instructions accessing the stack", RTI; NESdev wiki, "Instruction
  // reference", RTI: bits 4 and 5 of the pulled byte are ignored.
  auto bus = busWith({{0x0200, 0x40}, {0x01FB, 0xC3}, {0x01FC, 0x34}, {0x01FD, 0x12}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFA, .p = 0x00});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, DummyRead},
                          {0x01FA, DummyRead},
                          {0x01FB, StackPull},
                          {0x01FC, StackPull},
                          {0x01FD, StackPull}});
  EXPECT_EQ(cpu.state().pc, 0x1234);
  EXPECT_EQ(cpu.state().s, 0xFD);
  EXPECT_EQ(cpu.state().p, 0xC3);
}

TEST(CpuModes, PushIsThreeCycles) {
  // 64doc, "Instructions accessing the stack", PHA, PHP.
  auto bus = busWith({{0x0200, 0x48}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x5A});
  EXPECT_EQ(cpu.stepInstruction(bus), 3u);
  expectTrace(bus.trace,
              {{0x0200, OpcodeFetch}, {0x0201, DummyRead}, {0x01FD, StackPush, 0x5A}});
  EXPECT_EQ(cpu.state().s, 0xFC);
}

TEST(CpuModes, PhpPushesBitsFourAndFiveSet) {
  // 64doc, "Instructions accessing the stack", PHA, PHP; NESdev wiki, "Status
  // flags", The B flag.
  auto bus = busWith({{0x0200, 0x08}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .p = 0x00});
  EXPECT_EQ(cpu.stepInstruction(bus), 3u);
  expectTrace(bus.trace,
              {{0x0200, OpcodeFetch}, {0x0201, DummyRead}, {0x01FD, StackPush, 0x30}});
  EXPECT_EQ(cpu.state().p, 0x00);
}

TEST(CpuModes, PullIsFourCycles) {
  // 64doc, "Instructions accessing the stack", PLA, PLP.
  auto bus = busWith({{0x0200, 0x68}, {0x01FD, 0x80}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFC});
  EXPECT_EQ(cpu.stepInstruction(bus), 4u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, DummyRead},
                          {0x01FC, DummyRead},
                          {0x01FD, StackPull}});
  EXPECT_EQ(cpu.state().a, 0x80);
  EXPECT_NE(cpu.state().p & kCpuFlagN, 0);
  EXPECT_EQ(cpu.state().s, 0xFD);
}

TEST(CpuModes, PlpLeavesBitsFourAndFiveAsTheyWere) {
  // 64doc, "Instructions accessing the stack", PLA, PLP; NESdev wiki, "Instruction
  // reference", PLP: bits 4 and 5 of the pulled byte are ignored.
  auto allSet = busWith({{0x0200, 0x28}, {0x01FD, 0xFF}});
  Cpu6502 fromClear(Cpu6502State{.pc = 0x0200, .s = 0xFC, .p = 0x00});
  EXPECT_EQ(fromClear.stepInstruction(allSet), 4u);
  EXPECT_EQ(fromClear.state().p, 0xCF);

  auto allClear = busWith({{0x0200, 0x28}, {0x01FD, 0x00}});
  Cpu6502 fromSet(Cpu6502State{.pc = 0x0200, .s = 0xFC, .p = 0x30});
  EXPECT_EQ(fromSet.stepInstruction(allClear), 4u);
  EXPECT_EQ(fromSet.state().p, 0x30);
}

}  // namespace
