#include "neasant/cpu/cpu6502.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

// The 6502's interrupt lines and its halt: when an NMI edge or an IRQ level is
// taken, the poll's timing through ordinary instructions, branches and the flag
// instructions, the vector a sequence takes when an NMI arrives inside it, and the
// DMA units' hold on the CPU through the RDY line, counted in cycles. Each case
// names the NESdev wiki page and section, the 64doc section, or the MCS6500
// manual section it is read from.

namespace {

using neasant::Cpu6502;
using neasant::Cpu6502State;
using neasant::CpuSequence;
using neasant::CycleKind;
using neasant::kCpuFlagI;
using neasant::kCpuFlagZ;
using neasant::cpu_vectors::Access;
using neasant::cpu_vectors::RecordingBus;
using enum neasant::CycleKind;

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

void expectTrace(const std::vector<Access>& trace, const std::vector<Step>& steps) {
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

constexpr std::uint8_t kNop = 0xEA;
constexpr std::uint16_t kNmiVector = 0xFFFA;
constexpr std::uint16_t kIrqVector = 0xFFFE;

void seed(RecordingBus& bus,
          std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> ram) {
  for (const auto& [address, value] : ram) bus.mem[address] = value;
}

// Every case's memory: the NMI vector pointing at $9000 and the IRQ/BRK vector at
// $A000, four NOPs at each handler, then the case's own bytes.
template <typename Bus>
Bus busFor(std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> program) {
  Bus bus;
  seed(bus, {{0xFFFA, 0x00}, {0xFFFB, 0x90}, {0xFFFE, 0x00}, {0xFFFF, 0xA0},
             {0x9000, kNop}, {0x9001, kNop}, {0x9002, kNop}, {0x9003, kNop},
             {0xA000, kNop}, {0xA001, kNop}, {0xA002, kNop}, {0xA003, kNop}});
  seed(bus, program);
  return bus;
}

// The seven accesses of the sequence NMI or IRQ runs from a boundary at `pc` with
// the stack pointer at `s`: two discarded reads of `pc`, the pushes of PCH, PCL and
// `status` at `$0100 + s` and the two bytes below it, and the two vector reads
// (NESdev wiki, "CPU interrupts", IRQ and NMI tick-by-tick execution). A case's
// first sequence starts from $FD; one taken inside a handler starts three lower.
std::vector<Step> sequence(std::uint16_t pc, std::uint16_t vector, std::uint8_t status,
                           std::uint8_t s = 0xFD) {
  const auto stack = [s](int below) {
    return static_cast<std::uint16_t>(0x0100 | static_cast<std::uint8_t>(s - below));
  };
  return {{pc, DummyRead},
          {pc, DummyRead},
          {stack(0), StackPush, pc >> 8},
          {stack(1), StackPush, pc & 0xFF},
          {stack(2), StackPush, status},
          {vector, VectorRead},
          {static_cast<std::uint16_t>(vector + 1), VectorRead}};
}

std::size_t haltedCount(const std::vector<Access>& trace) {
  return static_cast<std::size_t>(std::count_if(
      trace.begin(), trace.end(), [](const Access& a) { return a.kind == Halted; }));
}

void expectHaltedAt(const std::vector<Access>& trace, std::uint16_t address) {
  for (const Access& a : trace) {
    if (a.kind == Halted) {
      EXPECT_EQ(a.address, address);
    }
  }
}

// A recording bus that drives a line from inside an access, as the machine's bus
// does: RDY dropped by a write to one address, IRQ released or NMI asserted by a
// read of another.
struct LineBus : RecordingBus {
  Cpu6502* cpu = nullptr;
  std::optional<std::uint16_t> dropRdyOnWriteTo;
  std::optional<std::uint16_t> releaseIrqOnReadOf;
  std::optional<std::uint16_t> assertNmiOnReadOf;

  std::uint8_t read(std::uint16_t address, CycleKind kind) {
    const std::uint8_t value = RecordingBus::read(address, kind);
    if (releaseIrqOnReadOf == address) cpu->setIrqLine(false);
    if (assertNmiOnReadOf == address) cpu->setNmiLine(true);
    return value;
  }
  void write(std::uint16_t address, std::uint8_t value, CycleKind kind) {
    RecordingBus::write(address, value, kind);
    if (dropRdyOnWriteTo == address) cpu->setRdyLine(false);
  }
};

// Plays the 2A03's two DMA units on the core's Halted reads (NESdev wiki, "DMA":
// Summary, Cadence, Behavior, OAM DMA, DMC DMA). The bus alternates get and put
// cycles, one per core access. A transfer holds the CPU while `active`, and each
// Halted read is one cycle of it: the halt cycle, the DMC's dummy cycle, an
// alignment cycle when this parity does not permit the next access, and the gets
// and puts. The count of Halted reads is therefore the number of cycles the DMA
// held the CPU. The core's accesses are recorded; the DMA's own are not.
struct DmaBus : RecordingBus {
  enum class Unit { None, Oam, Dmc };
  enum class Stage { Halt, Dummy, Transfer };

  bool put = false;     // whether the cycle under way is a put cycle
  bool active = false;  // a transfer holds the CPU
  Unit unit = Unit::None;
  Stage stage = Stage::Halt;
  std::uint8_t page = 0;                // the OAM source page
  int index = 0;                        // the OAM byte in flight
  bool haveByte = false;                // a byte was got and awaits its put
  std::uint8_t byte = 0;
  std::array<std::uint8_t, 256> oam{};  // what the puts to $2004 deliver
  std::uint16_t dmcAddress = 0;
  std::uint8_t dmcSample = 0;

  // Schedules a DMC fetch. The test drops RDY itself, as the machine will.
  void requestDmc(std::uint16_t address) {
    unit = Unit::Dmc;
    dmcAddress = address;
    stage = Stage::Halt;
    active = true;
  }

  std::uint8_t read(std::uint16_t address, CycleKind kind) {
    if (kind == Halted && active) dmaCycle();
    const std::uint8_t value = RecordingBus::read(address, kind);
    put = !put;
    return value;
  }
  void write(std::uint16_t address, std::uint8_t value, CycleKind kind) {
    RecordingBus::write(address, value, kind);
    if (address == 0x4014) {  // OAMDMA: the page most recently written is the source
      unit = Unit::Oam;
      page = value;
      stage = Stage::Halt;
      index = 0;
      haveByte = false;
      active = true;
    }
    put = !put;
  }

 private:
  // One cycle of the transfer under way, played on a Halted read.
  void dmaCycle() {
    switch (stage) {
      case Stage::Halt:  // the halt cycle: no access
        stage = unit == Unit::Dmc ? Stage::Dummy : Stage::Transfer;
        return;
      case Stage::Dummy:  // the DMC's dummy cycle: no access
        stage = Stage::Transfer;
        return;
      case Stage::Transfer:
        break;
    }
    if (unit == Unit::Dmc) {
      if (put) return;  // an alignment cycle: the get waits for a get cycle
      dmcSample = mem[dmcAddress];
      active = false;
      return;
    }
    if (!put && !haveByte) {  // a get
      byte = mem[static_cast<std::uint16_t>((page << 8) | index)];
      haveByte = true;
    } else if (put && haveByte) {  // a put
      oam[static_cast<std::size_t>(index)] = byte;
      haveByte = false;
      if (++index == 256) active = false;
    }
    // Otherwise an alignment cycle: a put cycle with nothing to put.
  }
};

// The machine's part between cycles: the RDY line follows the DMA unit.
void stepMachine(Cpu6502& cpu, DmaBus& bus) {
  cpu.stepCycle(bus);
  cpu.setRdyLine(!bus.active);
}

// Steps until the transfer releases the CPU; fails rather than hangs.
void runTransfer(Cpu6502& cpu, DmaBus& bus) {
  for (int i = 0; i < 600 && bus.active; ++i) stepMachine(cpu, bus);
  ASSERT_FALSE(bus.active);
}

TEST(CpuLines, NmiIsTakenAfterTheInstructionItArrivesIn) {
  // NESdev wiki, "CPU interrupts", IRQ and NMI tick-by-tick execution and Detailed
  // interrupt behavior; NESdev wiki, "NMI", Operation; MCS6500 hardware manual
  // 1.4.1.2.9.
  auto bus = busFor<RecordingBus>({{0x0200, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  cpu.setNmiLine(true);
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch}, {0x0201, DummyRead}});
  EXPECT_TRUE(cpu.state().nmiPending);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x0201, kNmiVector, 0x20));
  EXPECT_EQ(cpu.state().pc, 0x9000);
  EXPECT_EQ(cpu.state().s, 0xFA);
  EXPECT_NE(cpu.state().p & kCpuFlagI, 0);
  EXPECT_FALSE(cpu.state().nmiPending);
  EXPECT_TRUE(cpu.state().nmiLine);
  EXPECT_EQ(cpu.state().servicing, CpuSequence::None);
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(CpuLines, AHeldNmiLineIsTakenOnce) {
  // NESdev wiki, "CPU interrupts", Detailed interrupt behavior; MCS6500 hardware
  // manual 1.4.1.2.9 and 1.3.2.6.
  auto bus = busFor<RecordingBus>({{0x0200, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  cpu.setNmiLine(true);
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch}, {0x0201, DummyRead}});
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x0201, kNmiVector, 0x20));
  EXPECT_TRUE(cpu.state().nmiLine);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0x9000, OpcodeFetch}, {0x9001, DummyRead}});
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_EQ(cpu.state().servicing, CpuSequence::None);
  EXPECT_FALSE(cpu.state().nmiPending);
  cpu.setNmiLine(false);
  cpu.setNmiLine(true);
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x9003, kNmiVector, 0x24, 0xFA));
}

TEST(CpuLines, AnNmiAssertedAfterAnInstructionsLastCycleWaitsForTheNext) {
  // NESdev wiki, "CPU interrupts", Detailed interrupt behavior.
  {
    auto bus = busFor<RecordingBus>({{0x0200, kNop}, {0x0201, kNop}, {0x0202, kNop}});
    Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
    cpu.stepCycle(bus);
    cpu.setNmiLine(true);
    EXPECT_EQ(cpu.stepInstruction(bus), 1u);
    bus.trace.clear();
    EXPECT_EQ(cpu.stepInstruction(bus), 7u);
    expectTrace(bus.trace, sequence(0x0201, kNmiVector, 0x20));
  }
  {
    auto bus = busFor<RecordingBus>({{0x0200, kNop}, {0x0201, kNop}, {0x0202, kNop}});
    Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
    EXPECT_EQ(cpu.stepInstruction(bus), 2u);
    cpu.setNmiLine(true);
    bus.trace.clear();
    EXPECT_EQ(cpu.stepInstruction(bus), 2u);
    expectTrace(bus.trace, {{0x0201, OpcodeFetch}, {0x0202, DummyRead}});
    bus.trace.clear();
    EXPECT_EQ(cpu.stepInstruction(bus), 7u);
    expectTrace(bus.trace, sequence(0x0202, kNmiVector, 0x20));
  }
}

TEST(CpuLines, IrqIsTakenWhileIIsClearAndPushesBClear) {
  // NESdev wiki, "CPU interrupts", IRQ and NMI tick-by-tick execution; NESdev wiki,
  // "Status flags", The B flag; 64doc, "Interrupts"; MCS6500 hardware manual
  // 1.4.1.2.10.
  auto bus = busFor<RecordingBus>({{0x0200, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .irqLine = true});
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x0201, kIrqVector, 0x20));
  EXPECT_EQ(cpu.state().pc, 0xA000);
  EXPECT_EQ(cpu.state().s, 0xFA);
  EXPECT_NE(cpu.state().p & kCpuFlagI, 0);
  EXPECT_TRUE(cpu.state().irqLine);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0xA000, OpcodeFetch}, {0xA001, DummyRead}});
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_EQ(cpu.state().servicing, CpuSequence::None);
}

TEST(CpuLines, IrqIsIgnoredWhileIIsSet) {
  // 64doc, "Interrupts"; MCS6500 hardware manual 1.4.1.2.10.
  auto bus = busFor<RecordingBus>({{0x0200, kNop}, {0x0201, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .p = kCpuFlagI, .irqLine = true});
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0x0201, OpcodeFetch}, {0x0202, DummyRead}});
  EXPECT_EQ(cpu.state().servicing, CpuSequence::None);
  EXPECT_EQ(cpu.state().polled, CpuSequence::None);
}

TEST(CpuLines, IrqIsSampledAtTheEndOfTheSecondToLastCycle) {
  // NESdev wiki, "CPU interrupts", Detailed interrupt behavior.
  const std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> program = {
      {0x0200, 0xAD}, {0x0201, 0x34}, {0x0202, 0x12}, {0x0203, kNop}, {0x1234, 0x7F}};
  {
    auto bus = busFor<RecordingBus>(program);
    Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .irqLine = true});
    cpu.stepCycle(bus);
    cpu.stepCycle(bus);
    cpu.setIrqLine(false);
    EXPECT_EQ(cpu.stepInstruction(bus), 2u);
    bus.trace.clear();
    EXPECT_EQ(cpu.stepInstruction(bus), 2u);
    expectTrace(bus.trace, {{0x0203, OpcodeFetch}, {0x0204, DummyRead}});
    EXPECT_EQ(cpu.state().servicing, CpuSequence::None);
  }
  {
    auto bus = busFor<RecordingBus>(program);
    Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
    cpu.stepCycle(bus);
    cpu.stepCycle(bus);
    cpu.stepCycle(bus);
    cpu.setIrqLine(true);
    EXPECT_EQ(cpu.stepInstruction(bus), 1u);
    EXPECT_EQ(cpu.state().a, 0x7F);
    bus.trace.clear();
    EXPECT_EQ(cpu.stepInstruction(bus), 7u);
    expectTrace(bus.trace, sequence(0x0203, kIrqVector, 0x20));
  }
  {
    auto bus = busFor<LineBus>(program);
    bus.releaseIrqOnReadOf = 0x1234;
    Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .irqLine = true});
    bus.cpu = &cpu;
    EXPECT_EQ(cpu.stepInstruction(bus), 4u);
    EXPECT_FALSE(cpu.state().irqLine);
    bus.trace.clear();
    EXPECT_EQ(cpu.stepInstruction(bus), 7u);
    expectTrace(bus.trace, sequence(0x0203, kIrqVector, 0x20));
  }
}

TEST(CpuLines, NmiTakesPrecedenceOverAPendingIrq) {
  // NESdev wiki, "CPU interrupts", Detailed interrupt behavior; MCS6500 programming
  // manual 9.10; MCS6500 hardware manual Figure 1.14, events 9 and 12.
  auto bus = busFor<RecordingBus>({{0x0200, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .irqLine = true});
  cpu.setNmiLine(true);
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x0201, kNmiVector, 0x20));
  EXPECT_EQ(cpu.state().pc, 0x9000);
  EXPECT_NE(cpu.state().p & kCpuFlagI, 0);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0x9000, OpcodeFetch}, {0x9001, DummyRead}});
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_EQ(cpu.state().servicing, CpuSequence::None);
}

TEST(CpuLines, TheSequencesDoNotPollSoOneHandlerInstructionRuns) {
  // NESdev wiki, "CPU interrupts", Detailed interrupt behavior.
  auto bus = busFor<RecordingBus>({{0x0200, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .irqLine = true});
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  for (int i = 0; i < 5; ++i) cpu.stepCycle(bus);
  cpu.setNmiLine(true);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0xFFFE, VectorRead}, {0xFFFF, VectorRead}});
  EXPECT_EQ(cpu.state().pc, 0xA000);
  EXPECT_TRUE(cpu.state().nmiPending);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0xA000, OpcodeFetch}, {0xA001, DummyRead}});
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0xA001, kNmiVector, 0x24, 0xFA));
}

TEST(CpuLines, RtiRestoresIBeforeItsLastPollSoAPendingIrqIsTakenAtOnce) {
  // NESdev wiki, "CPU interrupts", Delayed IRQ response after CLI, SEI, and PLP;
  // MCS6500 programming manual Example 9.3.
  auto bus = busFor<RecordingBus>(
      {{0x0200, 0x40}, {0x0300, kNop}, {0x01FB, 0x00}, {0x01FC, 0x00}, {0x01FD, 0x03}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFA, .p = kCpuFlagI, .irqLine = true});
  EXPECT_EQ(cpu.stepInstruction(bus), 6u);
  EXPECT_EQ(cpu.state().p, 0x00);
  EXPECT_EQ(cpu.state().pc, 0x0300);
  EXPECT_EQ(cpu.state().s, 0xFD);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x0300, kIrqVector, 0x20));
}

TEST(CpuLines, CliDelaysAnIrqByOneInstruction) {
  // NESdev wiki, "CPU interrupts", Delayed IRQ response after CLI, SEI, and PLP.
  auto bus = busFor<RecordingBus>({{0x0200, 0x58}, {0x0201, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .p = kCpuFlagI, .irqLine = true});
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_EQ(cpu.state().p, 0x00);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0x0201, OpcodeFetch}, {0x0202, DummyRead}});
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x0202, kIrqVector, 0x20));
}

TEST(CpuLines, SeiDoesNotStopAnIrqPolledBeforeIt) {
  // NESdev wiki, "CPU interrupts", Delayed IRQ response after CLI, SEI, and PLP.
  auto bus = busFor<RecordingBus>({{0x0200, 0x78}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .irqLine = true});
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_EQ(cpu.state().p, 0x04);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x0201, kIrqVector, 0x24));
}

TEST(CpuLines, PlpDelaysAnIrqByOneInstruction) {
  // NESdev wiki, "CPU interrupts", Delayed IRQ response after CLI, SEI, and PLP.
  auto bus = busFor<RecordingBus>({{0x0200, 0x28}, {0x0201, kNop}, {0x01FD, 0x00}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFC, .p = kCpuFlagI, .irqLine = true});
  EXPECT_EQ(cpu.stepInstruction(bus), 4u);
  EXPECT_EQ(cpu.state().p, 0x00);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0x0201, OpcodeFetch}, {0x0202, DummyRead}});
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x0202, kIrqVector, 0x20));
}

TEST(CpuLines, ATakenBranchDoesNotPollBeforeItsThirdCycle) {
  // NESdev wiki, "CPU interrupts", Branch instructions and interrupts.
  const std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> program = {
      {0x0200, 0xD0}, {0x0201, 0x10}, {0x0212, kNop}, {0x0213, kNop}};
  {
    auto bus = busFor<RecordingBus>(program);
    Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
    cpu.stepCycle(bus);
    cpu.setIrqLine(true);
    EXPECT_EQ(cpu.stepInstruction(bus), 2u);
    EXPECT_EQ(cpu.state().pc, 0x0212);
    bus.trace.clear();
    EXPECT_EQ(cpu.stepInstruction(bus), 7u);
    expectTrace(bus.trace, sequence(0x0212, kIrqVector, 0x20));
  }
  {
    auto bus = busFor<RecordingBus>(program);
    Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
    cpu.stepCycle(bus);
    cpu.stepCycle(bus);
    cpu.setIrqLine(true);
    EXPECT_EQ(cpu.stepInstruction(bus), 1u);
    EXPECT_EQ(cpu.state().pc, 0x0212);
    bus.trace.clear();
    EXPECT_EQ(cpu.stepInstruction(bus), 2u);
    expectTrace(bus.trace, {{0x0212, OpcodeFetch}, {0x0213, DummyRead}});
    bus.trace.clear();
    EXPECT_EQ(cpu.stepInstruction(bus), 7u);
    expectTrace(bus.trace, sequence(0x0213, kIrqVector, 0x20));
  }
}

TEST(CpuLines, ANotTakenBranchPollsLikeATwoCycleInstruction) {
  // NESdev wiki, "CPU interrupts", Branch instructions and interrupts.
  auto bus = busFor<RecordingBus>({{0x0200, 0xD0}, {0x0201, 0x10}, {0x0202, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .p = kCpuFlagZ});
  cpu.stepCycle(bus);
  cpu.setIrqLine(true);
  EXPECT_EQ(cpu.stepInstruction(bus), 1u);
  EXPECT_EQ(cpu.state().pc, 0x0202);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x0202, kIrqVector, 0x22));
}

TEST(CpuLines, ABranchAcrossAPagePollsBeforeItsFixUpCycle) {
  // NESdev wiki, "CPU interrupts", Branch instructions and interrupts; 64doc,
  // "Relative addressing".
  const std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> program = {
      {0x02F0, 0xD0}, {0x02F1, 0x20}, {0x0312, kNop}};
  {
    auto bus = busFor<RecordingBus>(program);
    Cpu6502 cpu(Cpu6502State{.pc = 0x02F0, .s = 0xFD});
    for (int i = 0; i < 3; ++i) cpu.stepCycle(bus);
    cpu.setIrqLine(true);
    EXPECT_EQ(cpu.stepInstruction(bus), 1u);
    EXPECT_EQ(cpu.state().pc, 0x0312);
    bus.trace.clear();
    EXPECT_EQ(cpu.stepInstruction(bus), 7u);
    expectTrace(bus.trace, sequence(0x0312, kIrqVector, 0x20));
  }
  {
    auto bus = busFor<RecordingBus>(program);
    Cpu6502 cpu(Cpu6502State{.pc = 0x02F0, .s = 0xFD});
    cpu.stepCycle(bus);
    cpu.stepCycle(bus);
    cpu.setIrqLine(true);
    EXPECT_EQ(cpu.stepInstruction(bus), 2u);
    EXPECT_EQ(cpu.state().pc, 0x0312);
    bus.trace.clear();
    EXPECT_EQ(cpu.stepInstruction(bus), 7u);
    expectTrace(bus.trace, sequence(0x0312, kIrqVector, 0x20));
  }
}

TEST(CpuLines, AnIrqSeenOnlyAtABranchsFirstPollIsStillTaken) {
  // NESdev wiki, "CPU interrupts", Branch instructions and interrupts.
  auto bus = busFor<RecordingBus>({{0x02F0, 0xD0}, {0x02F1, 0x20}, {0x0312, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x02F0, .s = 0xFD});
  cpu.stepCycle(bus);
  cpu.setIrqLine(true);
  cpu.stepCycle(bus);
  cpu.setIrqLine(false);
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_FALSE(cpu.state().irqLine);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x0312, kIrqVector, 0x20));
}

TEST(CpuLines, AnNmiInBrksFirstFourCyclesTakesTheNmiVectorWithBSet) {
  // NESdev wiki, "CPU interrupts", Interrupt hijacking; 64doc, "Interrupts";
  // MCS6500 programming manual 9.10.
  auto bus = busFor<RecordingBus>({{0x0200, 0x00}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  for (int i = 0; i < 4; ++i) cpu.stepCycle(bus);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, DummyRead},
                          {0x01FD, StackPush, 0x02},
                          {0x01FC, StackPush, 0x02}});
  cpu.setNmiLine(true);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 3u);
  expectTrace(bus.trace,
              {{0x01FB, StackPush, 0x30}, {0xFFFA, VectorRead}, {0xFFFB, VectorRead}});
  EXPECT_EQ(cpu.state().pc, 0x9000);
  EXPECT_NE(cpu.state().p & kCpuFlagI, 0);
  EXPECT_FALSE(cpu.state().nmiPending);
  EXPECT_EQ(cpu.state().servicing, CpuSequence::None);
  EXPECT_TRUE(cpu.atInstructionBoundary());
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_EQ(cpu.state().servicing, CpuSequence::None);
}

TEST(CpuLines, AnNmiAfterBrksFourthCycleWaitsForTheHandlersFirstInstruction) {
  // NESdev wiki, "CPU interrupts", Interrupt hijacking and Detailed interrupt
  // behavior.
  auto bus = busFor<RecordingBus>({{0x0200, 0x00}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  for (int i = 0; i < 5; ++i) cpu.stepCycle(bus);
  expectAccess(bus.trace.back(), 0x01FB, StackPush);
  EXPECT_EQ(bus.trace.back().value, 0x30);
  cpu.setNmiLine(true);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0xFFFE, VectorRead}, {0xFFFF, VectorRead}});
  EXPECT_EQ(cpu.state().pc, 0xA000);
  EXPECT_TRUE(cpu.state().nmiPending);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  expectTrace(bus.trace, {{0xA000, OpcodeFetch}, {0xA001, DummyRead}});
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0xA001, kNmiVector, 0x24, 0xFA));
}

TEST(CpuLines, AnNmiInAnIrqSequencesFirstFourCyclesTakesTheNmiVector) {
  // NESdev wiki, "CPU interrupts", Interrupt hijacking; MCS6500 programming manual
  // 9.10; MCS6500 hardware manual Figure 1.14.
  auto bus = busFor<RecordingBus>({{0x0200, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .irqLine = true});
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  bus.trace.clear();
  for (int i = 0; i < 4; ++i) cpu.stepCycle(bus);
  expectTrace(bus.trace, {{0x0201, DummyRead},
                          {0x0201, DummyRead},
                          {0x01FD, StackPush, 0x02},
                          {0x01FC, StackPush, 0x01}});
  cpu.setNmiLine(true);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 3u);
  expectTrace(bus.trace,
              {{0x01FB, StackPush, 0x20}, {0xFFFA, VectorRead}, {0xFFFB, VectorRead}});
  EXPECT_EQ(cpu.state().pc, 0x9000);
  EXPECT_NE(cpu.state().p & kCpuFlagI, 0);
  EXPECT_FALSE(cpu.state().nmiPending);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_EQ(cpu.state().servicing, CpuSequence::None);
}

TEST(CpuLines, AnNmiDuringAHaltIsTakenAfterTheHaltedInstruction) {
  // NESdev wiki, "DMA", Behavior; MCS6500 hardware manual 1.4.1.2.8.
  auto bus = busFor<RecordingBus>({{0x0200, kNop}, {0x0201, kNop}});
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  cpu.stepCycle(bus);
  cpu.setRdyLine(false);
  bus.trace.clear();
  cpu.stepCycle(bus);
  cpu.stepCycle(bus);
  expectTrace(bus.trace, {{0x0201, Halted}, {0x0201, Halted}});
  cpu.setNmiLine(true);
  cpu.stepCycle(bus);
  expectAccess(bus.trace.back(), 0x0201, Halted);
  EXPECT_TRUE(cpu.state().nmiPending);
  EXPECT_EQ(cpu.state().tcu, 1);
  cpu.setRdyLine(true);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 1u);
  expectTrace(bus.trace, {{0x0201, DummyRead}});
  EXPECT_TRUE(cpu.atInstructionBoundary());
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x0201, kNmiVector, 0x20));
}

TEST(CpuLines, AnNmiAssertedInsideAHaltedReadIsKept) {
  // NESdev wiki, "DMA", Behavior; MCS6500 hardware manual 1.4.1.2.8; NESdev wiki,
  // "CPU interrupts", Detailed interrupt behavior.
  auto bus = busFor<LineBus>({{0x0200, kNop}, {0x0201, kNop}});
  bus.assertNmiOnReadOf = 0x0201;
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  bus.cpu = &cpu;
  cpu.stepCycle(bus);
  cpu.setRdyLine(false);
  bus.trace.clear();
  cpu.stepCycle(bus);
  expectTrace(bus.trace, {{0x0201, Halted}});
  EXPECT_TRUE(cpu.state().nmiPending);
  EXPECT_TRUE(cpu.state().nmiLine);
  EXPECT_EQ(cpu.state().tcu, 1);
  cpu.setRdyLine(true);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 1u);
  expectTrace(bus.trace, {{0x0201, DummyRead}});
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 7u);
  expectTrace(bus.trace, sequence(0x0201, kNmiVector, 0x20));
}

TEST(CpuLines, StepInstructionStopsWhenACycleLeavesRdyLow) {
  // 64doc, "6510 features"; MCS6500 hardware manual 1.4.1.2.8; NESdev wiki, "DMA",
  // Behavior.
  auto bus = busFor<LineBus>(
      {{0x0200, 0xEE}, {0x0201, 0x14}, {0x0202, 0x40}, {0x0203, kNop}, {0x4014, 0x01}});
  bus.dropRdyOnWriteTo = 0x4014;
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  bus.cpu = &cpu;
  EXPECT_EQ(cpu.stepInstruction(bus), 5u);
  expectTrace(bus.trace, {{0x0200, OpcodeFetch},
                          {0x0201, OperandFetch},
                          {0x0202, OperandFetch},
                          {0x4014, RmwRead, 0x01},
                          {0x4014, RmwDummyWrite, 0x01}});
  EXPECT_EQ(cpu.state().tcu, 5);
  EXPECT_FALSE(cpu.atInstructionBoundary());
  EXPECT_FALSE(cpu.state().rdyLine);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 1u);
  expectTrace(bus.trace, {{0x4014, RmwWrite, 0x02}});
  EXPECT_TRUE(cpu.atInstructionBoundary());
  EXPECT_EQ(bus.mem[0x4014], 0x02);
  bus.trace.clear();
  EXPECT_EQ(cpu.stepInstruction(bus), 1u);
  expectTrace(bus.trace, {{0x0203, Halted}});
  EXPECT_EQ(cpu.state().tcu, 0);
}

TEST(CpuLines, OamDmaHaltingOnAPutCycleTakes513Cycles) {
  // NESdev wiki, "DMA", Summary, Behavior and OAM DMA.
  auto bus = busFor<DmaBus>(
      {{0x0200, 0x8D}, {0x0201, 0x14}, {0x0202, 0x40}, {0x0203, kNop}, {0x0204, kNop}});
  for (int i = 0; i < 256; ++i) {
    bus.mem[static_cast<std::size_t>(0x0300 + i)] = static_cast<std::uint8_t>(i ^ 0x5A);
  }
  bus.put = true;
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x03});
  for (int i = 0; i < 4; ++i) stepMachine(cpu, bus);
  expectAccess(bus.trace.back(), 0x4014, DataWrite);
  EXPECT_EQ(bus.trace.back().value, 0x03);
  EXPECT_TRUE(bus.active);
  EXPECT_FALSE(cpu.state().rdyLine);
  bus.trace.clear();
  runTransfer(cpu, bus);
  EXPECT_EQ(haltedCount(bus.trace), 513u);
  expectHaltedAt(bus.trace, 0x0203);
  for (int i = 0; i < 256; ++i) EXPECT_EQ(bus.oam[static_cast<std::size_t>(i)], i ^ 0x5A);
  EXPECT_TRUE(cpu.state().rdyLine);
  bus.trace.clear();
  stepMachine(cpu, bus);
  stepMachine(cpu, bus);
  expectTrace(bus.trace, {{0x0203, OpcodeFetch}, {0x0204, DummyRead}});
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(CpuLines, OamDmaHaltingOnAGetCycleTakes514Cycles) {
  // NESdev wiki, "DMA", Summary, Behavior and OAM DMA.
  auto bus = busFor<DmaBus>(
      {{0x0200, 0x8D}, {0x0201, 0x14}, {0x0202, 0x40}, {0x0203, kNop}, {0x0204, kNop}});
  for (int i = 0; i < 256; ++i) {
    bus.mem[static_cast<std::size_t>(0x0300 + i)] = static_cast<std::uint8_t>(i ^ 0x5A);
  }
  bus.put = false;
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .a = 0x03});
  for (int i = 0; i < 4; ++i) stepMachine(cpu, bus);
  expectAccess(bus.trace.back(), 0x4014, DataWrite);
  EXPECT_EQ(bus.trace.back().value, 0x03);
  EXPECT_TRUE(bus.active);
  EXPECT_FALSE(cpu.state().rdyLine);
  bus.trace.clear();
  runTransfer(cpu, bus);
  EXPECT_EQ(haltedCount(bus.trace), 514u);
  expectHaltedAt(bus.trace, 0x0203);
  for (int i = 0; i < 256; ++i) EXPECT_EQ(bus.oam[static_cast<std::size_t>(i)], i ^ 0x5A);
  EXPECT_TRUE(cpu.state().rdyLine);
  bus.trace.clear();
  stepMachine(cpu, bus);
  stepMachine(cpu, bus);
  expectTrace(bus.trace, {{0x0203, OpcodeFetch}, {0x0204, DummyRead}});
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(CpuLines, OamDmaWaitsForAWriteCycleAndCopiesTheSecondPageWritten) {
  // NESdev wiki, "DMA", Behavior and OAM DMA.
  auto bus = busFor<DmaBus>({{0x0200, 0xEE},
                             {0x0201, 0x14},
                             {0x0202, 0x40},
                             {0x0203, kNop},
                             {0x0204, kNop},
                             {0x4014, 0x03}});
  for (int i = 0; i < 256; ++i) {
    bus.mem[static_cast<std::size_t>(0x0300 + i)] = static_cast<std::uint8_t>(i ^ 0x5A);
    bus.mem[static_cast<std::size_t>(0x0400 + i)] = static_cast<std::uint8_t>(i ^ 0xA5);
  }
  bus.put = true;
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  for (int i = 0; i < 5; ++i) stepMachine(cpu, bus);
  expectAccess(bus.trace.back(), 0x4014, RmwDummyWrite);
  EXPECT_EQ(bus.trace.back().value, 0x03);
  EXPECT_TRUE(bus.active);
  EXPECT_FALSE(cpu.state().rdyLine);
  bus.trace.clear();
  stepMachine(cpu, bus);
  expectTrace(bus.trace, {{0x4014, RmwWrite, 0x04}});
  EXPECT_TRUE(bus.active);
  bus.trace.clear();
  runTransfer(cpu, bus);
  EXPECT_EQ(haltedCount(bus.trace), 513u);
  expectHaltedAt(bus.trace, 0x0203);
  for (int i = 0; i < 256; ++i) EXPECT_EQ(bus.oam[static_cast<std::size_t>(i)], i ^ 0xA5);
  bus.trace.clear();
  stepMachine(cpu, bus);
  stepMachine(cpu, bus);
  expectTrace(bus.trace, {{0x0203, OpcodeFetch}, {0x0204, DummyRead}});
}

TEST(CpuLines, DmcDmaHaltingOnAGetCycleTakes3Cycles) {
  // NESdev wiki, "DMA", Summary and DMC DMA.
  auto bus = busFor<DmaBus>({{0x0200, kNop}, {0x0201, kNop}, {0xC123, 0x7E}});
  bus.put = true;
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  stepMachine(cpu, bus);
  bus.requestDmc(0xC123);
  cpu.setRdyLine(false);
  bus.trace.clear();
  runTransfer(cpu, bus);
  EXPECT_EQ(haltedCount(bus.trace), 3u);
  expectHaltedAt(bus.trace, 0x0201);
  EXPECT_EQ(bus.dmcSample, 0x7E);
  bus.trace.clear();
  stepMachine(cpu, bus);
  expectTrace(bus.trace, {{0x0201, DummyRead}});
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(CpuLines, DmcDmaHaltingOnAPutCycleTakes4Cycles) {
  // NESdev wiki, "DMA", Summary and DMC DMA.
  auto bus = busFor<DmaBus>({{0x0200, kNop}, {0x0201, kNop}, {0xC123, 0x7E}});
  bus.put = false;
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD});
  stepMachine(cpu, bus);
  bus.requestDmc(0xC123);
  cpu.setRdyLine(false);
  bus.trace.clear();
  runTransfer(cpu, bus);
  EXPECT_EQ(haltedCount(bus.trace), 4u);
  expectHaltedAt(bus.trace, 0x0201);
  EXPECT_EQ(bus.dmcSample, 0x7E);
  bus.trace.clear();
  stepMachine(cpu, bus);
  expectTrace(bus.trace, {{0x0201, DummyRead}});
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(CpuLines, DmcDmaWaitsThroughAnInterruptsThreePushes) {
  // 64doc, "6510 features"; NESdev wiki, "DMA", Behavior; MCS6500 hardware manual
  // 1.4.1.2.8.
  auto bus = busFor<DmaBus>({{0x0200, kNop}, {0xC123, 0x7E}});
  bus.put = true;
  Cpu6502 cpu(Cpu6502State{.pc = 0x0200, .s = 0xFD, .irqLine = true});
  stepMachine(cpu, bus);
  stepMachine(cpu, bus);
  stepMachine(cpu, bus);
  stepMachine(cpu, bus);
  bus.requestDmc(0xC123);
  cpu.setRdyLine(false);
  bus.trace.clear();
  for (int i = 0; i < 3; ++i) stepMachine(cpu, bus);
  expectTrace(bus.trace, {{0x01FD, StackPush, 0x02},
                          {0x01FC, StackPush, 0x01},
                          {0x01FB, StackPush, 0x20}});
  EXPECT_EQ(cpu.state().s, 0xFA);
  EXPECT_TRUE(bus.active);
  EXPECT_FALSE(cpu.state().rdyLine);
  bus.trace.clear();
  runTransfer(cpu, bus);
  EXPECT_EQ(haltedCount(bus.trace), 3u);
  expectHaltedAt(bus.trace, 0xFFFE);
  EXPECT_EQ(bus.dmcSample, 0x7E);
  bus.trace.clear();
  stepMachine(cpu, bus);
  stepMachine(cpu, bus);
  expectTrace(bus.trace, {{0xFFFE, VectorRead}, {0xFFFF, VectorRead}});
  EXPECT_EQ(cpu.state().pc, 0xA000);
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

}  // namespace
