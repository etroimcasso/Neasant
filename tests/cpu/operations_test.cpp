#include "neasant/cpu/cpu6502.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

// The official operations of the 6502: the value each one leaves and the flags it
// sets, one instruction at a time through the public surface. The accumulator
// operations run in their immediate form, the read-modify-write operations on the
// byte at $0010, BIT on the byte at $0010. Each case names the MCS6500 programming
// manual's worked example or statement it is read from, or the NESdev wiki page.
// The whole status byte is compared, so a flag that moves without being named
// fails the case.

namespace {

using neasant::Cpu6502;
using neasant::Cpu6502State;
using neasant::kCpuFlagC;
using neasant::kCpuFlagD;
using neasant::kCpuFlagN;
using neasant::kCpuFlagV;
using neasant::kCpuFlagZ;
using neasant::cpu_vectors::RecordingBus;

constexpr std::uint16_t kCell = 0x0010;

RecordingBus busWith(std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> ram) {
  RecordingBus bus;
  for (const auto& [address, value] : ram) bus.mem[address] = value;
  return bus;
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

constexpr std::uint8_t kAdcImmediate = 0x69;
constexpr std::uint8_t kSbcImmediate = 0xE9;
constexpr std::uint8_t kCmpImmediate = 0xC9;
constexpr std::uint8_t kCpxImmediate = 0xE0;
constexpr std::uint8_t kCpyImmediate = 0xC0;
constexpr std::uint8_t kBitZeroPage = 0x24;
constexpr std::uint8_t kAndImmediate = 0x29;
constexpr std::uint8_t kOraImmediate = 0x09;
constexpr std::uint8_t kEorImmediate = 0x49;
constexpr std::uint8_t kLdaImmediate = 0xA9;
constexpr std::uint8_t kLdxImmediate = 0xA2;
constexpr std::uint8_t kLdyImmediate = 0xA0;
constexpr std::uint8_t kAslZeroPage = 0x06;
constexpr std::uint8_t kLsrZeroPage = 0x46;
constexpr std::uint8_t kRolZeroPage = 0x26;
constexpr std::uint8_t kRorZeroPage = 0x66;
constexpr std::uint8_t kIncZeroPage = 0xE6;
constexpr std::uint8_t kDecZeroPage = 0xC6;

// ---- ADC and SBC ------------------------------------------------------------------

TEST(CpuOperations, AdcTwoPositivesWithinRange) {
  // MCS6500 programming manual, Example 2.6: +5 plus +7 is +12; carry and overflow
  // stay clear.
  const Result r = run(kAdcImmediate, 0x07, {.a = 0x05});
  EXPECT_EQ(r.state.a, 0x0C);
  expectStatus(r.state, 0);
}

TEST(CpuOperations, AdcTwoPositivesSetsOverflow) {
  // MCS6500 programming manual, Example 2.7: +127 plus +2 sets bit 7 and overflow.
  const Result r = run(kAdcImmediate, 0x02, {.a = 0x7F});
  EXPECT_EQ(r.state.a, 0x81);
  expectStatus(r.state, kCpuFlagV | kCpuFlagN);
}

TEST(CpuOperations, AdcMixedSignsPositiveSetsCarry) {
  // MCS6500 programming manual, Example 2.8: +5 plus -3 is +2 with carry set and
  // overflow clear.
  const Result r = run(kAdcImmediate, 0xFD, {.a = 0x05});
  EXPECT_EQ(r.state.a, 0x02);
  expectStatus(r.state, kCpuFlagC);
}

TEST(CpuOperations, AdcMixedSignsNegativeClearsCarry) {
  // MCS6500 programming manual, Example 2.9: +5 plus -7 is -2 with carry and
  // overflow clear.
  const Result r = run(kAdcImmediate, 0xF9, {.a = 0x05});
  EXPECT_EQ(r.state.a, 0xFE);
  expectStatus(r.state, kCpuFlagN);
}

TEST(CpuOperations, AdcTwoNegativesWithinRange) {
  // MCS6500 programming manual, Example 2.10: -5 plus -7 is -12 with carry set and
  // overflow clear.
  const Result r = run(kAdcImmediate, 0xF9, {.a = 0xFB});
  EXPECT_EQ(r.state.a, 0xF4);
  expectStatus(r.state, kCpuFlagC | kCpuFlagN);
}

TEST(CpuOperations, AdcTwoNegativesSetsOverflow) {
  // MCS6500 programming manual, Example 2.11: -66 plus -65 leaves $7D with carry
  // and overflow set.
  const Result r = run(kAdcImmediate, 0xBF, {.a = 0xBE});
  EXPECT_EQ(r.state.a, 0x7D);
  expectStatus(r.state, kCpuFlagC | kCpuFlagV);
}

TEST(CpuOperations, AdcAddsTheCarryIn) {
  // MCS6500 programming manual, 2.2.1: ADC is A + M + C; Examples 2.1 and 2.2 add
  // the carry into bit 0.
  const Result r = run(kAdcImmediate, 0x07, {.a = 0x05, .p = kCpuFlagC});
  EXPECT_EQ(r.state.a, 0x0D);
  expectStatus(r.state, 0);
}

TEST(CpuOperations, SbcWithoutBorrowKeepsCarry) {
  // MCS6500 programming manual, Example 2.13: 5 minus 3 with carry set is +2, and
  // the carry stays set because no borrow was needed.
  const Result r = run(kSbcImmediate, 0x03, {.a = 0x05, .p = kCpuFlagC});
  EXPECT_EQ(r.state.a, 0x02);
  expectStatus(r.state, kCpuFlagC);
}

TEST(CpuOperations, SbcWithBorrowClearsCarry) {
  // MCS6500 programming manual, Example 2.14: 5 minus 6 with carry set is -1, and
  // the carry clears to show the borrow.
  const Result r = run(kSbcImmediate, 0x06, {.a = 0x05, .p = kCpuFlagC});
  EXPECT_EQ(r.state.a, 0xFF);
  expectStatus(r.state, kCpuFlagN);
}

TEST(CpuOperations, SbcWithCarryClearBorrowsOne) {
  // MCS6500 programming manual, 2.2.2: "The microprocessor adds the carry flag to
  // the complemented memory data", so a clear carry takes one more away.
  const Result r = run(kSbcImmediate, 0x03, {.a = 0x05});
  EXPECT_EQ(r.state.a, 0x01);
  expectStatus(r.state, kCpuFlagC);
}

TEST(CpuOperations, AdcIgnoresTheDecimalFlag) {
  // NESdev wiki, "CPU": the 2A03 lacks decimal mode; "Status flags", D: the flag
  // has no effect. $09 plus $01 is the binary $0A.
  const Result r = run(kAdcImmediate, 0x01, {.a = 0x09, .p = kCpuFlagD});
  EXPECT_EQ(r.state.a, 0x0A);
  expectStatus(r.state, kCpuFlagD);
}

TEST(CpuOperations, SbcIgnoresTheDecimalFlag) {
  // NESdev wiki, "CPU"; "Status flags", D. $10 minus $01 is the binary $0F.
  const Result r = run(kSbcImmediate, 0x01, {.a = 0x10, .p = kCpuFlagD | kCpuFlagC});
  EXPECT_EQ(r.state.a, 0x0F);
  expectStatus(r.state, kCpuFlagD | kCpuFlagC);
}

// ---- compares and BIT ---------------------------------------------------------------

TEST(CpuOperations, CmpEqualSetsZeroAndCarry) {
  // MCS6500 programming manual, 4.2.1, the results of a compare: accumulator equal
  // to memory sets Z and C and resets N; the accumulator is not affected.
  const Result r = run(kCmpImmediate, 0x40, {.a = 0x40});
  EXPECT_EQ(r.state.a, 0x40);
  expectStatus(r.state, kCpuFlagZ | kCpuFlagC);
}

TEST(CpuOperations, CmpLessClearsCarry) {
  // MCS6500 programming manual, 4.2.1: accumulator less than memory resets C and Z;
  // N is bit 7 of the difference, here $F0.
  const Result r = run(kCmpImmediate, 0x40, {.a = 0x30});
  EXPECT_EQ(r.state.a, 0x30);
  expectStatus(r.state, kCpuFlagN);
}

TEST(CpuOperations, CmpGreaterSetsCarry) {
  // MCS6500 programming manual, 4.2.1: accumulator greater than memory sets C and
  // resets Z; N is bit 7 of the difference, here $10.
  const Result r = run(kCmpImmediate, 0x40, {.a = 0x50});
  EXPECT_EQ(r.state.a, 0x50);
  expectStatus(r.state, kCpuFlagC);
}

TEST(CpuOperations, CpxAndCpyCompareTheIndexRegisters) {
  // MCS6500 programming manual, 7.8: X equal to memory sets Z and C. 7.9: Y less
  // than memory clears C, and bit 7 of the difference sets N. Neither register
  // changes.
  const Result cpx = run(kCpxImmediate, 0x10, {.x = 0x10});
  EXPECT_EQ(cpx.state.x, 0x10);
  expectStatus(cpx.state, kCpuFlagZ | kCpuFlagC);

  const Result cpy = run(kCpyImmediate, 0x01, {.y = 0x00});
  EXPECT_EQ(cpy.state.y, 0x00);
  expectStatus(cpy.state, kCpuFlagN);
}

TEST(CpuOperations, BitLoadsBitsSevenAndSixIntoNAndV) {
  // MCS6500 programming manual, 4.2.2.1: N is bit 7 and V bit 6 of the memory
  // tested, Z is set when the AND of memory and accumulator is zero, and the
  // accumulator is not affected. NESdev wiki, "Status flags", V and N.
  const Result nonzero = run(kBitZeroPage, 0x10, {.a = 0xFF}, 0xC0);
  EXPECT_EQ(nonzero.state.a, 0xFF);
  expectStatus(nonzero.state, kCpuFlagN | kCpuFlagV);

  const Result zero = run(kBitZeroPage, 0x10, {.a = 0x01}, 0xC0);
  EXPECT_EQ(zero.state.a, 0x01);
  expectStatus(zero.state, kCpuFlagN | kCpuFlagV | kCpuFlagZ);
}

// ---- logical operations and loads ---------------------------------------------------

TEST(CpuOperations, AndOraEorSetNAndZFromTheResult) {
  // MCS6500 programming manual, 2.2.4.1 to 2.2.4.3: each sets Z when the result is
  // zero and N from its bit 7. NESdev wiki, "Instruction reference", AND, ORA, EOR.
  const Result andOp = run(kAndImmediate, 0x0F, {.a = 0xF0});
  EXPECT_EQ(andOp.state.a, 0x00);
  expectStatus(andOp.state, kCpuFlagZ);

  const Result oraOp = run(kOraImmediate, 0x80, {.a = 0x00});
  EXPECT_EQ(oraOp.state.a, 0x80);
  expectStatus(oraOp.state, kCpuFlagN);

  const Result eorOp = run(kEorImmediate, 0xFF, {.a = 0xFF});
  EXPECT_EQ(eorOp.state.a, 0x00);
  expectStatus(eorOp.state, kCpuFlagZ);
}

TEST(CpuOperations, LoadsSetNAndZFromTheValue) {
  // NESdev wiki, "Instruction reference", LDA, LDX, LDY: Z when the value is zero, N
  // from its bit 7.
  const Result lda = run(kLdaImmediate, 0x00, {.a = 0x55});
  EXPECT_EQ(lda.state.a, 0x00);
  expectStatus(lda.state, kCpuFlagZ);

  const Result ldx = run(kLdxImmediate, 0x80, {});
  EXPECT_EQ(ldx.state.x, 0x80);
  expectStatus(ldx.state, kCpuFlagN);

  const Result ldy = run(kLdyImmediate, 0x01, {});
  EXPECT_EQ(ldy.state.y, 0x01);
  expectStatus(ldy.state, 0);
}

// ---- shifts, rotates, INC and DEC -------------------------------------------------

TEST(CpuOperations, AslMovesBitSevenIntoCarry) {
  // MCS6500 programming manual, 10.2: bit 0 becomes 0, input bit 7 goes to the
  // carry, N is result bit 7.
  const Result r = run(kAslZeroPage, 0x10, {}, 0x81);
  EXPECT_EQ(r.cell, 0x02);
  expectStatus(r.state, kCpuFlagC);
}

TEST(CpuOperations, LsrMovesBitZeroIntoCarryAndClearsN) {
  // MCS6500 programming manual, 10.1: bit 7 becomes 0, input bit 0 goes to the
  // carry, and N is always reset.
  const Result r = run(kLsrZeroPage, 0x10, {.p = kCpuFlagN}, 0x81);
  EXPECT_EQ(r.cell, 0x40);
  expectStatus(r.state, kCpuFlagC);

  const Result zero = run(kLsrZeroPage, 0x10, {}, 0x01);
  EXPECT_EQ(zero.cell, 0x00);
  expectStatus(zero.state, kCpuFlagZ | kCpuFlagC);
}

TEST(CpuOperations, RolRotatesThroughCarry) {
  // MCS6500 programming manual, 10.3: the input carry goes to bit 0 and input bit 7
  // to the carry.
  const Result r = run(kRolZeroPage, 0x10, {.p = kCpuFlagC}, 0x80);
  EXPECT_EQ(r.cell, 0x01);
  expectStatus(r.state, kCpuFlagC);
}

TEST(CpuOperations, RorRotatesThroughCarry) {
  // MCS6500 programming manual, 10.4: the input carry goes to bit 7, input bit 0 to
  // the carry, and N equals the input carry.
  const Result r = run(kRorZeroPage, 0x10, {.p = kCpuFlagC}, 0x01);
  EXPECT_EQ(r.cell, 0x80);
  expectStatus(r.state, kCpuFlagC | kCpuFlagN);
}

TEST(CpuOperations, IncAndDecMoveOnlyNAndZ) {
  // MCS6500 programming manual, 10.7 and 10.8: N and Z follow the result; carry and
  // overflow are not affected.
  const Result inc = run(kIncZeroPage, 0x10, {.p = kCpuFlagC}, 0xFF);
  EXPECT_EQ(inc.cell, 0x00);
  expectStatus(inc.state, kCpuFlagZ | kCpuFlagC);

  const Result dec = run(kDecZeroPage, 0x10, {}, 0x00);
  EXPECT_EQ(dec.cell, 0xFF);
  expectStatus(dec.state, kCpuFlagN);
}

}  // namespace
