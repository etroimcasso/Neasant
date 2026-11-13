#include "neasant/apu/apu.h"
#include "neasant/nes/nes.h"
#include "rom_harness.h"
#include "authored_image.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef NEASANT_NES_TEST_ROMS
#define NEASANT_NES_TEST_ROMS ""
#endif

// The APU's frame counter, length counters and status, first driven alone through apu.h a
// tick at a time with every expected cycle derived from the frame counter's table and
// blargg's measurements, then on the machine; and blargg's APU, reset, frame-counter and
// interrupt programs booted on the machine and run to their report. The
// NEASANT_NES_TEST_ROMS option, the skips and NEASANT_REQUIRE_NES_TEST_ROMS are as in the
// CPU programs' test.
//
// A cycle below is a tick index counted from a write: the write lands in the access of
// cycle 0, before that cycle's tick, so "after tick k" is after k + 1 ticks. A write on a
// put cycle puts the sequence's origin at tick 2 and one on a get at tick 3.

namespace {

using neasant::ApuState;
using neasant::apuPeekStatus;
using neasant::apuReadStatus;
using neasant::apuReset;
using neasant::apuTickCycle;
using neasant::apuWrite;
using neasant::CpuSequence;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::loadRom;
using neasant::nes_tests::placeProgram;
using neasant::nes_tests::Report;
using neasant::nes_tests::RomResult;
using neasant::nes_tests::runUntilResult;

using Bytes = std::vector<std::uint8_t>;
using Program = std::initializer_list<std::uint8_t>;

constexpr std::uint8_t kGet = 0;
constexpr std::uint8_t kPut = 1;

// Ticks the chip `n` times, each as a get cycle: the half a tick is given moves the
// pulses' and the noise's timers alone, which these cases do not read.
void tickN(ApuState& state, long n) {
  for (long i = 0; i < n; ++i) apuTickCycle(state, kGet);
}

// The power-on chip with pulse 1 enabled and its counter loaded from table entry
// `value >> 3`, after the first tick, which is the power-on restart and clocks nothing in
// the 4-step mode.
ApuState pulseLoaded(std::uint8_t value) {
  ApuState state;
  apuWrite(state, 0x4015, 0x01, kGet);
  apuWrite(state, 0x4003, value, kGet);
  apuTickCycle(state, kGet);
  return state;
}

// ---- The machine ----

// A 16 KB NROM image with 8 KB of CHR ROM, `program` at $8000 and every vector pointing
// there.
Bytes imageWith(Program program) {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  placeProgram(image, 0, std::span<const std::uint8_t>(program.begin(), program.size()));
  return image;
}

void stepCycles(Nes& machine, long cycles) {
  for (long i = 0; i < cycles; ++i) machine.step();
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

// LDA #$00; STA $4017; CLI; JMP $8006: the write lands in machine cycle 12, cycles 0-6
// being the reset sequence, 7-8 the LDA and 9-12 the STA.
constexpr Program kIrqProgram{0xA9, 0x00, 0x8D, 0x17, 0x40, 0x58, 0x4C, 0x06, 0x80};

// ---- The programs ----

// The guard, in frames: 30 seconds at the console's rate. The longest of these programs
// reports within seven seconds.
constexpr std::uint64_t kGuardFrames = 1800;

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

// Boots the ROM on a machine built with the defaults, or with `mapper` stated over the
// header and the CPU-PPU alignment `alignment`, runs it to its report and requires a
// pass.
void expectPass(const std::string& relative, Report report,
                std::optional<std::uint16_t> mapper = std::nullopt,
                std::uint8_t alignment = 0) {
  std::optional<Bytes> image;
  loadOrSkip(relative, image);
  if (!image) return;
  Nes machine(NesConfig{.rom = *image, .alignment = alignment, .mapper = mapper});
  const RomResult result = runUntilResult(machine, report, kGuardFrames);
  EXPECT_TRUE(result.reported) << relative << " did not report within " << kGuardFrames
                               << " frames\n--- text ---\n"
                               << result.text << "\n------------";
  if (!result.reported) return;
  EXPECT_TRUE(result.passed) << relative << " reports a failure after " << result.frames
                             << " frames\n--- text ---\n"
                             << result.text << "\n------------";
}

// Registers a program the machine cannot run: the file is looked for as above, so a
// required clone that lacks it fails, and the case then skips naming what it needs.
void skipNeeding(const std::string& relative, const std::string& needs) {
  std::optional<Bytes> image;
  loadOrSkip(relative, image);
  if (!image) return;
  GTEST_SKIP() << relative << " needs " << needs;
}

}  // namespace

// ---- FrameCounter ----

TEST(FrameCounter, FourStepFromAWriteOnAPutClocksAndSetsTheFlagAtBlarggsCycles) {
  // NESdev wiki, "APU Frame Counter", the opening and the $4017.7 = 0 table: the
  // half-frame clocks at 7456.5 and 14914.5 APU cycles from an origin two cycles after a
  // write during an APU cycle ("Control register": 3 CPU cycles after the write cycle),
  // then every 14915 APU cycles; the flag on the last three cycles. blargg's 2005 frame
  // counter readme, Mode 0 Timing: the length clocked at 14915 and 29831, the flag at
  // 29830-29832. "CPU interrupts", Detailed interrupt behavior: the line a cycle behind.
  ApuState s = pulseLoaded(0x08);  // entry 1, 254
  apuWrite(s, 0x4017, 0x00, kPut);
  tickN(s, 14915);  // ticks 0-14914
  EXPECT_EQ(s.length[0].counter, 254);
  tickN(s, 1);
  EXPECT_EQ(s.length[0].counter, 253);
  tickN(s, 29829 - 14915);  // through 29829
  EXPECT_FALSE(s.frameInterrupt);
  tickN(s, 1);  // 29830
  EXPECT_TRUE(s.frameInterrupt);
  EXPECT_FALSE(s.irqLine);
  EXPECT_EQ(s.length[0].counter, 253);
  tickN(s, 1);  // 29831
  EXPECT_TRUE(s.irqLine);
  EXPECT_EQ(s.length[0].counter, 252);
  tickN(s, 44744 - 29831);
  EXPECT_EQ(s.length[0].counter, 252);
  tickN(s, 1);  // 44745
  EXPECT_EQ(s.length[0].counter, 251);
}

TEST(FrameCounter, FourStepFromAWriteOnAGetIsOneCycleLater) {
  // NESdev wiki, "APU Frame Counter", Control register: the effects occur 4 CPU cycles
  // after a write between APU cycles, one later than after a write during one.
  ApuState s = pulseLoaded(0x08);
  apuWrite(s, 0x4017, 0x00, kGet);
  tickN(s, 14916);  // through 14915
  EXPECT_EQ(s.length[0].counter, 254);
  tickN(s, 1);  // 14916
  EXPECT_EQ(s.length[0].counter, 253);
  tickN(s, 29831 - 14917);  // through 29830
  EXPECT_FALSE(s.frameInterrupt);
  tickN(s, 1);  // 29831
  EXPECT_TRUE(s.frameInterrupt);
}

TEST(FrameCounter, TheFlagIsSetOnThreeTicksAndAReadInTheirCyclesClearsOnlyThatTicksPredecessor) {
  // NESdev wiki, "APU", Status: "If an interrupt flag was set at the same moment of the
  // read, it will read back as 1 but it will not be cleared"; blargg's 2005 readme,
  // 07.irq_flag_timing tests 4 and 5. A read in a cycle precedes that cycle's tick, so a
  // read in the second or third of the flag's cycles clears it and the tick sets it
  // again, and a read in the cycle after the third clears it for good. The IRQ line is
  // sampled from the flag after the cycle's access, so a read every cycle keeps it low.
  ApuState s = pulseLoaded(0x08);
  apuWrite(s, 0x4017, 0x00, kPut);
  tickN(s, 29831);  // through 29830
  ASSERT_TRUE(s.frameInterrupt);
  EXPECT_EQ(apuReadStatus(s, 0) & 0x40, 0x40);
  EXPECT_FALSE(s.frameInterrupt);
  tickN(s, 1);  // 29831
  EXPECT_TRUE(s.frameInterrupt);
  EXPECT_FALSE(s.irqLine);
  EXPECT_EQ(apuReadStatus(s, 0) & 0x40, 0x40);
  tickN(s, 1);  // 29832
  EXPECT_TRUE(s.frameInterrupt);
  EXPECT_FALSE(s.irqLine);
  EXPECT_EQ(apuReadStatus(s, 0) & 0x40, 0x40);
  tickN(s, 1);  // 29833
  EXPECT_FALSE(s.frameInterrupt);
  EXPECT_FALSE(s.irqLine);
  EXPECT_EQ(apuReadStatus(s, 0) & 0x40, 0x00);
}

TEST(FrameCounter, FiveStepFromAWriteOnAPutClocksTheCycleBeforeItsRestartAndNeverSetsTheFlag) {
  // NESdev wiki, "APU Frame Counter", the $4017.7 = 1 table and Control register: the
  // half-frame clocks at 7456.5 and 18640.5 APU cycles, both units clocked the cycle
  // before the restart, two cycles after the write ("APU", Frame Counter), the flag never
  // set; blargg's 2005 readme, Mode 1 Timing: length at 1, 14915 and 37283, the next pass
  // 37282 on.
  ApuState s = pulseLoaded(0x08);
  apuWrite(s, 0x4017, 0x80, kPut);
  tickN(s, 1);  // tick 0
  EXPECT_EQ(s.length[0].counter, 254);
  tickN(s, 1);  // 1, the cycle before the restart
  EXPECT_EQ(s.length[0].counter, 253);
  tickN(s, 14915 - 1);
  EXPECT_EQ(s.length[0].counter, 252);
  tickN(s, 37283 - 14915);
  EXPECT_EQ(s.length[0].counter, 251);
  tickN(s, 52197 - 37283);
  EXPECT_EQ(s.length[0].counter, 250);
  tickN(s, 74565 - 52197);
  EXPECT_EQ(s.length[0].counter, 249);
  for (long tick = 74566; tick < 80000; ++tick) {
    apuTickCycle(s, kGet);
    EXPECT_FALSE(s.frameInterrupt);
    EXPECT_FALSE(s.irqLine);
  }
  EXPECT_FALSE(s.frameInterrupt);
}

TEST(FrameCounter, AWriteWithBit7SetOnAGetClocksAtTickTwo) {
  // NESdev wiki, "APU", Frame Counter: 2 or 3 cycles after the write.
  ApuState s = pulseLoaded(0x08);
  apuWrite(s, 0x4017, 0x80, kGet);
  tickN(s, 2);  // through 1
  EXPECT_EQ(s.length[0].counter, 254);
  tickN(s, 1);  // 2
  EXPECT_EQ(s.length[0].counter, 253);
}

TEST(FrameCounter, InhibitClearsTheFlagAtOnceAndAModeWriteKeepsIt) {
  // NESdev wiki, "APU Frame Counter": the flag is acknowledged by writing $4017 with the
  // interrupt inhibit flag set; apu_test's readme, 3-irq_flag tests 6 and 7.
  for (const std::uint8_t value : std::initializer_list<std::uint8_t>{0x00, 0x80}) {
    ApuState s;
    s.frameInterrupt = true;
    apuWrite(s, 0x4017, value, kPut);
    EXPECT_TRUE(s.frameInterrupt) << static_cast<int>(value);
  }
  for (const std::uint8_t value : std::initializer_list<std::uint8_t>{0x40, 0xC0}) {
    ApuState s;
    s.frameInterrupt = true;
    apuWrite(s, 0x4017, value, kPut);
    EXPECT_FALSE(s.frameInterrupt) << static_cast<int>(value);
    EXPECT_TRUE(s.inhibit);
  }
}

TEST(FrameCounter, TheOldSequenceStepsUntilTheRestart) {
  // blargg's 2005 frame counter readme: a write to $4017 runs the current mode a few
  // clocks before switching to the new one. A second write on a put, in the cycle of the
  // old sequence's first flag tick, leaves that tick and the next to set the flag; the
  // restart two cycles on brings the count to 0 and the flag is next set 29828 ticks
  // after it.
  ApuState s;
  apuWrite(s, 0x4017, 0x00, kPut);
  tickN(s, 29830);  // ticks 0-29829
  EXPECT_FALSE(s.frameInterrupt);
  apuWrite(s, 0x4017, 0x00, kPut);
  tickN(s, 1);  // 29830
  EXPECT_TRUE(s.frameInterrupt);
  s.frameInterrupt = false;
  tickN(s, 1);  // 29831
  EXPECT_TRUE(s.frameInterrupt);
  tickN(s, 1);  // 29832, the restart
  EXPECT_EQ(s.frameCycle, 0);
  EXPECT_EQ(s.restartDelay, 0);
  EXPECT_EQ(apuReadStatus(s, 0) & 0x40, 0x40);
  tickN(s, 59659 - 29832);  // through 59659
  EXPECT_FALSE(s.frameInterrupt);
  tickN(s, 1);  // 59660
  EXPECT_TRUE(s.frameInterrupt);
}

TEST(FrameCounter, PowerOnIsASequenceRestartedAtTheFirstTick) {
  // NESdev wiki, "CPU power up state", APU; blargg's apu_reset readme, 4017_timing: at
  // power it is as if $00 were written to $4017 before execution, nine cycles before
  // the first instruction, which begins at machine cycle 7.
  EXPECT_TRUE(ApuState{} == NesState{}.apu);
  ApuState s;
  EXPECT_EQ(s.restartDelay, 1);
  apuWrite(s, 0x4015, 0x01, kGet);
  apuWrite(s, 0x4003, 0x08, kGet);
  tickN(s, 1);  // 0
  EXPECT_EQ(s.restartDelay, 0);
  EXPECT_EQ(s.length[0].counter, 254);
  tickN(s, 14913);  // through 14913
  EXPECT_EQ(s.length[0].counter, 253);
  tickN(s, 29827 - 14913);  // through 29827
  EXPECT_FALSE(s.frameInterrupt);
  tickN(s, 1);  // 29828
  EXPECT_TRUE(s.frameInterrupt);
  tickN(s, 1);  // 29829
  EXPECT_EQ(s.length[0].counter, 252);
}

TEST(FrameCounter, ResetKeepsTheModeAndTheHaltsAndClearsTheFlagAndTheCounters) {
  // NESdev wiki, "CPU power up state", APU, the After Reset column: $4015 cleared, $4017
  // unchanged; blargg's apu_reset readme, 4017_written and len_ctrs_enabled.
  ApuState s;
  apuWrite(s, 0x4015, 0x0F, kGet);
  for (const std::uint16_t address :
       std::initializer_list<std::uint16_t>{0x4003, 0x4007, 0x400B, 0x400F}) {
    apuWrite(s, address, 0x08, kGet);
  }
  apuWrite(s, 0x4000, 0x30, kGet);
  apuWrite(s, 0x4017, 0xC0, kPut);
  tickN(s, 5);
  s.frameInterrupt = true;
  apuReset(s);
  EXPECT_FALSE(s.frameInterrupt);
  for (const auto& length : s.length) {
    EXPECT_FALSE(length.enabled);
    EXPECT_EQ(length.counter, 0);
  }
  EXPECT_TRUE(s.length[0].halt);
  EXPECT_TRUE(s.inhibit);
  EXPECT_TRUE(s.fiveStep);
  EXPECT_EQ(s.restartDelay, 1);
  // A reset's restart makes no clock, and the load lands.
  apuWrite(s, 0x4015, 0x01, kGet);
  apuWrite(s, 0x4003, 0x08, kGet);
  tickN(s, 1);
  EXPECT_EQ(s.length[0].counter, 254);

  // The other order: a load written in the cycle of the write's clock loses to it
  // (blargg's 2005 frame counter readme, Length Reload).
  ApuState t = pulseLoaded(0x08);
  apuWrite(t, 0x4000, 0x10, kGet);
  apuWrite(t, 0x4017, 0x80, kPut);
  tickN(t, 1);  // tick 0
  EXPECT_EQ(t.length[0].counter, 254);
  apuWrite(t, 0x4003, 0x08, kGet);
  tickN(t, 1);  // 1, the write's clock
  EXPECT_EQ(t.length[0].counter, 253);
}

// ---- LengthCounter ----

TEST(LengthCounter, LoadsFromTheTableAtTheTickWhileEnabled) {
  // NESdev wiki, "APU Length Counter", Clocking and the register table.
  ApuState s;
  apuWrite(s, 0x4003, 0x18, kGet);
  tickN(s, 1);
  EXPECT_EQ(s.length[0].counter, 0);
  apuWrite(s, 0x4015, 0x01, kGet);
  apuWrite(s, 0x4003, 0x18, kGet);
  EXPECT_EQ(s.length[0].counter, 0);
  tickN(s, 1);
  EXPECT_EQ(s.length[0].counter, 2);
  EXPECT_EQ(apuPeekStatus(s, 0) & 0x01, 0x01);
}

TEST(LengthCounter, TheTableHasThirtyTwoEntries) {
  // NESdev wiki, "APU Length Counter", Length table.
  constexpr std::array<std::uint8_t, 32> kTable{
      10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
      12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
  for (std::size_t index = 0; index < kTable.size(); ++index) {
    ApuState s;
    apuWrite(s, 0x4015, 0x01, kGet);
    apuWrite(s, 0x4003, static_cast<std::uint8_t>(index << 3), kGet);
    tickN(s, 1);
    EXPECT_EQ(s.length[0].counter, kTable[index]) << index;
  }
}

TEST(LengthCounter, ClearingTheEnableZeroesItAtOnceAndSettingItRestoresNothing) {
  // NESdev wiki, "APU Length Counter", Clocking: the counter is forced to 0, and there is
  // no immediate effect when enabled is set.
  ApuState s = pulseLoaded(0x18);
  ASSERT_EQ(s.length[0].counter, 2);
  apuWrite(s, 0x4015, 0x00, kGet);
  EXPECT_EQ(s.length[0].counter, 0);
  apuWrite(s, 0x4015, 0x01, kGet);
  tickN(s, 1);
  EXPECT_EQ(s.length[0].counter, 0);
  apuWrite(s, 0x4003, 0x18, kGet);
  tickN(s, 1);
  EXPECT_EQ(s.length[0].counter, 2);
}

TEST(LengthCounter, AHaltWrittenInTheClocksCycleDoesNotHoldIt) {
  // blargg's 2005 frame counter readme, Length Halt: a halt written at 14915 lets the
  // clock count.
  ApuState s;
  apuWrite(s, 0x4015, 0x01, kGet);
  apuWrite(s, 0x4000, 0x10, kGet);
  apuWrite(s, 0x4003, 0x18, kGet);
  tickN(s, 1);
  apuWrite(s, 0x4017, 0x00, kPut);
  tickN(s, 14915);  // ticks 0-14914
  apuWrite(s, 0x4000, 0x30, kGet);
  tickN(s, 1);  // 14915
  EXPECT_EQ(s.length[0].counter, 1);
}

TEST(LengthCounter, AHaltWrittenTheCycleBeforeHoldsIt) {
  // blargg's 2005 frame counter readme, Length Halt: a halt written at 14914 holds the
  // clock at 14915.
  ApuState s;
  apuWrite(s, 0x4015, 0x01, kGet);
  apuWrite(s, 0x4000, 0x10, kGet);
  apuWrite(s, 0x4003, 0x18, kGet);
  tickN(s, 1);
  apuWrite(s, 0x4017, 0x00, kPut);
  tickN(s, 14914);  // ticks 0-14913
  apuWrite(s, 0x4000, 0x30, kGet);
  tickN(s, 2);  // 14914 and 14915
  EXPECT_EQ(s.length[0].counter, 2);
}

TEST(LengthCounter, AReloadInTheClocksCycleIsIgnoredWhileCounting) {
  // blargg's 2005 frame counter readme, Length Reload: ignored when written during
  // length clocking with the counter above 0.
  ApuState s = pulseLoaded(0x38);  // 6
  apuWrite(s, 0x4017, 0x00, kPut);
  tickN(s, 14915);  // ticks 0-14914
  apuWrite(s, 0x4003, 0x18, kGet);
  tickN(s, 1);  // 14915
  EXPECT_EQ(s.length[0].counter, 5);
}

TEST(LengthCounter, AReloadInTheClocksCycleLandsAtZero) {
  // blargg's 2005 frame counter readme, Length Reload: a reload during length clocking
  // with the counter at 0 works normally.
  ApuState s = pulseLoaded(0x38);
  apuWrite(s, 0x4015, 0x00, kGet);
  apuWrite(s, 0x4015, 0x01, kGet);
  apuWrite(s, 0x4017, 0x00, kPut);
  tickN(s, 14915);
  apuWrite(s, 0x4003, 0x18, kGet);
  tickN(s, 1);  // 14915
  EXPECT_EQ(s.length[0].counter, 2);
}

TEST(LengthCounter, AReloadTheCycleBeforeTheClockLandsAndIsClocked) {
  // blargg's 2005 frame counter readme, Length Reload: a reload just before the length
  // clock works normally.
  ApuState s = pulseLoaded(0x38);
  apuWrite(s, 0x4017, 0x00, kPut);
  tickN(s, 14914);  // ticks 0-14913
  apuWrite(s, 0x4003, 0x18, kGet);
  tickN(s, 2);  // 14914 and 14915
  EXPECT_EQ(s.length[0].counter, 1);
}

TEST(LengthCounter, EachChannelHasItsHaltBitLoadRegisterAndStatusBit) {
  // NESdev wiki, "APU Length Counter", the register table: bit 5 of $4000, $4004 and
  // $400C and bit 7 of $4008 halt; $4003, $4007, $400B and $400F load.
  ApuState s;
  apuWrite(s, 0x4015, 0x0F, kGet);
  apuWrite(s, 0x4004, 0x20, kGet);
  apuWrite(s, 0x4008, 0x80, kGet);
  apuWrite(s, 0x400C, 0x20, kGet);
  apuWrite(s, 0x4000, 0xDF, kGet);
  EXPECT_FALSE(s.length[0].halt);
  EXPECT_TRUE(s.length[1].halt);
  EXPECT_TRUE(s.length[2].halt);
  EXPECT_TRUE(s.length[3].halt);
  apuWrite(s, 0x4007, 0x08, kGet);
  apuWrite(s, 0x400B, 0x08, kGet);
  apuWrite(s, 0x400F, 0x08, kGet);
  tickN(s, 1);
  EXPECT_EQ(s.length[0].counter, 0);
  EXPECT_EQ(s.length[1].counter, 254);
  EXPECT_EQ(s.length[2].counter, 254);
  EXPECT_EQ(s.length[3].counter, 254);
  EXPECT_EQ(apuPeekStatus(s, 0xFF), 0x2E);
}

// ---- Status ----

TEST(Status, TheReadAnswersTheFlagAndTheCountersOverBitFiveAndClearsTheFlag) {
  // NESdev wiki, "APU", Status: IF-D NT21, bit 5 open bus; reading clears the frame
  // interrupt flag.
  ApuState s = pulseLoaded(0x18);
  s.frameInterrupt = true;
  EXPECT_EQ(apuPeekStatus(s, 0x00), 0x41);
  EXPECT_TRUE(s.frameInterrupt);
  EXPECT_EQ(apuReadStatus(s, 0x20), 0x61);
  EXPECT_FALSE(s.frameInterrupt);
  EXPECT_EQ(apuReadStatus(s, 0xDF), 0x01);
}

// ---- Machine ----

TEST(Machine, TheMachineTicksTheApuAfterTheAccessAndDrivesTheLineACycleLater) {
  // NESdev wiki, "APU Frame Counter", Control register; "CPU interrupts", Detailed
  // interrupt behavior: the poll before an instruction's last cycle decides. The write
  // lands on a get, so the origin is the tick of cycle 15 and the flag is set by the
  // tick of 15 + 29828; the line follows a cycle on and the CPU's level a cycle after
  // that; the JMP whose last cycle is 29846 polls before it and the interrupt sequence
  // begins at 29847, pushing three bytes and loading the vector, $8000.
  const Bytes image = imageWith(kIrqProgram);
  Nes m(NesConfig{.rom = image});
  stepCycles(m, 29843);
  EXPECT_FALSE(m.state().apu.frameInterrupt);
  stepCycles(m, 1);  // 29844
  EXPECT_TRUE(m.state().apu.frameInterrupt);
  EXPECT_FALSE(m.state().cpu.irqLine);
  stepCycles(m, 1);  // 29845
  EXPECT_TRUE(m.state().cpu.irqLine);
  stepCycles(m, 2);  // 29847
  EXPECT_EQ(m.state().cpu.servicing, CpuSequence::None);
  stepCycles(m, 1);  // 29848
  EXPECT_EQ(m.state().cpu.servicing, CpuSequence::Irq);
  stepCycles(m, 6);  // 29854
  EXPECT_EQ(m.state().cpu.pc, 0x8000);
  EXPECT_EQ(m.state().cpu.s, 0xFA);

  // A write on a put restarts a cycle sooner.
  Nes put(NesConfig{.rom = image, .apuPhase = 1});
  stepCycles(put, 29842);
  EXPECT_FALSE(put.state().apu.frameInterrupt);
  stepCycles(put, 1);  // 29843
  EXPECT_TRUE(put.state().apu.frameInterrupt);
}

TEST(Machine, PowerOnThroughTheMachine) {
  // NESdev wiki, "CPU power up state", APU; "APU", Status: the sequence restarted at the
  // first cycle's tick sets the flag at the ticks of 29828-29830, and the loop's reads
  // see it and clear it. LDA $4015; STA $00; JMP $8000.
  const Bytes image = imageWith({0xAD, 0x15, 0x40, 0x85, 0x00, 0x4C, 0x00, 0x80});
  Nes m(NesConfig{.rom = image});
  stepCycles(m, 29828);
  EXPECT_FALSE(m.state().apu.frameInterrupt);
  stepCycles(m, 1);  // 29829
  EXPECT_TRUE(m.state().apu.frameInterrupt);
  stepCycles(m, 29840 - 29829);
  EXPECT_EQ(m.state().ram[0], 0x40);
  stepCycles(m, 1);  // 29841
  EXPECT_FALSE(m.state().apu.frameInterrupt);
  for (long cycle = 29841; cycle < 29852; ++cycle) {
    m.step();
    EXPECT_FALSE(m.state().apu.frameInterrupt) << cycle;
  }
}

TEST(Machine, ResetThroughTheMachine) {
  // NESdev wiki, "CPU power up state", APU, the After Reset column; blargg's apu_reset
  // readme, 4017_written: the mode and the inhibit as last written, the channels
  // disabled, the sequence restarted at the first tick after the reset.
  // LDA #$C0; STA $4017; LDA #$0F; STA $4015; LDA #$08; STA $4003; JMP $800F
  const Bytes image = imageWith({0xA9, 0xC0, 0x8D, 0x17, 0x40, 0xA9, 0x0F, 0x8D, 0x15, 0x40,
                                 0xA9, 0x08, 0x8D, 0x03, 0x40, 0x4C, 0x0F, 0x80});
  Nes m(NesConfig{.rom = image});
  stepCycles(m, 60);
  ASSERT_EQ(m.state().apu.length[0].counter, 254);
  m.reset();
  stepCycles(m, 7);
  const ApuState& a = m.state().apu;
  EXPECT_TRUE(a.fiveStep);
  EXPECT_TRUE(a.inhibit);
  for (const auto& length : a.length) {
    EXPECT_FALSE(length.enabled);
    EXPECT_EQ(length.counter, 0);
  }
  EXPECT_FALSE(a.frameInterrupt);
  EXPECT_EQ(a.restartDelay, 0);
  EXPECT_EQ(a.frameCycle, 6);
}

TEST(Machine, ASnapshotCarriesTheApu) {
  const Bytes image = imageWith(kIrqProgram);
  Nes a(NesConfig{.rom = image});
  Nes b(NesConfig{.rom = image});
  stepCycles(a, 30000);
  b.restore(a.state());
  EXPECT_TRUE(a.state().apu == b.state().apu);
  stepCycles(a, 1000);
  stepCycles(b, 1000);
  EXPECT_TRUE(a.state().apu == b.state().apu);
  EXPECT_TRUE(a.state() == b.state());
}

TEST(Machine, AudioRateIsConfigAndRefusedAboveTheCpuClock) {
  // NESdev wiki, "CPU", Frequencies: the CPU clock is 236 250 000 / 132 Hz, 1 789 772.7.
  const Bytes image = imageWith({0xEA});
  EXPECT_EQ(Nes(NesConfig{.rom = image}).audioRate(), 48000u);
  EXPECT_EQ(Nes(NesConfig{.rom = image, .audioRate = 1789772}).audioRate(), 1789772u);
  EXPECT_EQ(buildRefusal({.rom = image, .audioRate = 1789773}),
            "an audioRate of 1789773 is above the CPU clock; 1789772 is the most");
  EXPECT_EQ(Nes(NesConfig{.rom = image, .audioRate = 0}).audioRate(), 0u);
}

// ---- apu_test ----

// The multi-test build, on mapper 1 (its readme).
TEST(ApuTest, ApuTest) {
  skipNeeding("apu_test/apu_test.nes",
              "mapper 1, which is not built; its sub-tests run from rom_singles");
}

// "Tests length counter operation for the four main channels" (its readme). Reports in
// 22 frames.
TEST(ApuTest, Singles1LenCtr) {
  expectPass("apu_test/rom_singles/1-len_ctr.nes", Report::Memory);
}

// "Verifies all length table entries" (its readme). Reports in 17 frames.
TEST(ApuTest, Singles2LenTable) {
  expectPass("apu_test/rom_singles/2-len_table.nes", Report::Memory);
}

// "Verifies basic operation of frame irq flag" (its readme). Reports in 21 frames.
TEST(ApuTest, Singles3IrqFlag) {
  expectPass("apu_test/rom_singles/3-irq_flag.nes", Report::Memory);
}

// "Tests for APU clock jitter. Also tests basic timing of frame irq flag since it's needed
// to determine jitter" (its readme). Reports in 20 frames.
TEST(ApuTest, Singles4Jitter) {
  expectPass("apu_test/rom_singles/4-jitter.nes", Report::Memory);
}

// "Verifies timing of length counter clocks in both modes" (its readme). Reports in 115
// frames.
TEST(ApuTest, Singles5LenTiming) {
  expectPass("apu_test/rom_singles/5-len_timing.nes", Report::Memory);
}

// "Frame interrupt flag is set three times in a row 29831 clocks after writing $00 to
// $4017" (its readme). Reports in 23 frames.
TEST(ApuTest, Singles6IrqFlagTiming) {
  expectPass("apu_test/rom_singles/6-irq_flag_timing.nes", Report::Memory);
}

// ---- apu_reset ----
//
// Each program asks for the reset button and the harness gives it seven frames after the
// request (rom_harness.h).

// "At power and reset, $4015 is cleared" (its readme). Reports in 33 frames.
TEST(ApuReset, Rom4015Cleared) {
  expectPass("apu_reset/4015_cleared.nes", Report::Memory);
}

// "At power, it is as if $00 were written to $4017, then a 9-12 clock delay, then
// execution from address in reset vector" (its readme); the program prints the delay, 9.
// Reports in 47 frames.
TEST(ApuReset, Rom4017Timing) {
  expectPass("apu_reset/4017_timing.nes", Report::Memory);
}

// "At power, $4017 = $00. At reset, $4017 mode is unchanged" (its readme). Reports in 61
// frames.
TEST(ApuReset, Rom4017Written) {
  expectPass("apu_reset/4017_written.nes", Report::Memory);
}

// "At power and reset, IRQ flag is clear" (its readme). Reports in 31 frames.
TEST(ApuReset, RomIrqFlagCleared) {
  expectPass("apu_reset/irq_flag_cleared.nes", Report::Memory);
}

// "At power and reset, length counters are enabled" (its readme). Reports in 36 frames.
TEST(ApuReset, RomLenCtrsEnabled) {
  expectPass("apu_reset/len_ctrs_enabled.nes", Report::Memory);
}

// The frame counter, the status and the length counters working at power-on and after
// reset, read with the DMC's two status bits (its readme and source).
TEST(ApuReset, RomWorksImmediately) {
  skipNeeding("apu_reset/works_immediately.nes",
              "the DMC's active bit and interrupt flag in $4015, which are not built");
}

// ---- blargg_apu_2005.07.30 ----
//
// Each program prints its result code on the screen, $01 a pass (its tests.txt).

// "Tests basic length counter operation" (its tests.txt). Reports in 23 frames.
TEST(BlarggApu2005, Rom01LenCtr) {
  expectPass("blargg_apu_2005.07.30/01.len_ctr.nes", Report::Screen);
}

// "Tests all length table entries" (its tests.txt). Reports in 9 frames.
TEST(BlarggApu2005, Rom02LenTable) {
  expectPass("blargg_apu_2005.07.30/02.len_table.nes", Report::Screen);
}

// "Tests basic operation of frame irq flag" (its tests.txt). Reports in 15 frames.
TEST(BlarggApu2005, Rom03IrqFlag) {
  expectPass("blargg_apu_2005.07.30/03.irq_flag.nes", Report::Screen);
}

// "Tests for APU clock jitter. Also tests basic timing of frame irq flag since it's needed
// to determine jitter" (its tests.txt). Reports in 14 frames.
TEST(BlarggApu2005, Rom04ClockJitter) {
  expectPass("blargg_apu_2005.07.30/04.clock_jitter.nes", Report::Screen);
}

// "Tests length counter timing in mode 0" (its tests.txt). Reports in 20 frames.
TEST(BlarggApu2005, Rom05LenTimingMode0) {
  expectPass("blargg_apu_2005.07.30/05.len_timing_mode0.nes", Report::Screen);
}

// "Tests length counter timing in mode 1" (its tests.txt). Reports in 21 frames.
TEST(BlarggApu2005, Rom06LenTimingMode1) {
  expectPass("blargg_apu_2005.07.30/06.len_timing_mode1.nes", Report::Screen);
}

// "Frame interrupt flag is set three times in a row 29831 clocks after writing $4017 with
// $00" (its tests.txt). Reports in 16 frames.
TEST(BlarggApu2005, Rom07IrqFlagTiming) {
  expectPass("blargg_apu_2005.07.30/07.irq_flag_timing.nes", Report::Screen);
}

// "IRQ handler is invoked at minimum 29833 clocks after writing $00 to $4017" (its
// tests.txt). Reports in 14 frames.
TEST(BlarggApu2005, Rom08IrqTiming) {
  expectPass("blargg_apu_2005.07.30/08.irq_timing.nes", Report::Screen);
}

// "After reset or power-up, APU acts as if $4017 were written with $00 from 9 to 12 clocks
// before first instruction begins" (its tests.txt). Reports in 9 frames.
TEST(BlarggApu2005, Rom09ResetTiming) {
  expectPass("blargg_apu_2005.07.30/09.reset_timing.nes", Report::Screen);
}

// "Changes to length counter halt occur after clocking length, not before" (its
// tests.txt). Reports in 14 frames.
TEST(BlarggApu2005, Rom10LenHaltTiming) {
  expectPass("blargg_apu_2005.07.30/10.len_halt_timing.nes", Report::Screen);
}

// "Write to length counter reload should be ignored when made during length counter
// clocking and the length counter is not zero" (its tests.txt). Reports in 14 frames.
TEST(BlarggApu2005, Rom11LenReloadTiming) {
  expectPass("blargg_apu_2005.07.30/11.len_reload_timing.nes", Report::Screen);
}

// ---- cpu_interrupts_v2 ----

// The multi-test build, on mapper 1 (its readme).
TEST(CpuInterrupts, CpuInterrupts) {
  skipNeeding("cpu_interrupts_v2/cpu_interrupts.nes",
              "mapper 1, which is not built; its sub-tests run from rom_singles");
}

// "Tests the delay in CLI taking effect, and some basic aspects of IRQ handling and the
// APU frame IRQ" (its readme). Reports in 18 frames.
TEST(CpuInterrupts, Singles1CliLatency) {
  expectPass("cpu_interrupts_v2/rom_singles/1-cli_latency.nes", Report::Memory);
}

// NMI interrupting BRK (its readme), which "occasionally fails on NES due to PPU-CPU
// synchronization": it passes under alignment 0, the default, and fails under 1, 2 and 3.
// Reports in 106 frames.
TEST(CpuInterrupts, Singles2NmiAndBrk) {
  expectPass("cpu_interrupts_v2/rom_singles/2-nmi_and_brk.nes", Report::Memory);
}

// "NMI behavior when it interrupts IRQ vectoring" (its readme). Reports in 126 frames.
TEST(CpuInterrupts, Singles3NmiAndIrq) {
  expectPass("cpu_interrupts_v2/rom_singles/3-nmi_and_irq.nes", Report::Memory);
}

// "Has IRQ occur at various times around sprite DMA" (its readme). Reports in 71 frames.
TEST(CpuInterrupts, Singles4IrqAndDma) {
  expectPass("cpu_interrupts_v2/rom_singles/4-irq_and_dma.nes", Report::Memory);
}

// "A taken non-page-crossing branch ignores IRQ during its last clock, so that next
// instruction executes before the IRQ" (its readme). Reports in 380 frames.
TEST(CpuInterrupts, Singles5BranchDelaysIrq) {
  expectPass("cpu_interrupts_v2/rom_singles/5-branch_delays_irq.nes", Report::Memory);
}
