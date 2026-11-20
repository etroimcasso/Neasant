#include "neasant/apu/apu.h"
#include "neasant/nes/clock.h"
#include "neasant/nes/nes.h"
#include "authored_image.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// The console's output stage: apuFilter driven a cycle at a time with levels set by hand
// on a stage set to rest, every expected answer the two filters' whole-number steps
// applied cycle by cycle, with the figure each corner frequency gives beside it; the
// level the stage holds at power-on; and the machine, silent from power-on until a
// program moves a level, with the stage in a snapshot, across a reset and at a rate of 0.
// A cycle below is a call counted from a chip's first, or a machine cycle counted from
// power-on. The steps are 5300 and 804 639 of 16 777 216 (apu.cpp); a time constant is
// 3165.5 cycles for the high-pass and 20.85 for the low-pass.

namespace {

using neasant::ApuFilter;
using neasant::ApuState;
using neasant::apuFilter;
using neasant::apuMix;
using neasant::apuReset;
using neasant::consoleClock;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::Region;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::placeProgram;

using Bytes = std::vector<std::uint8_t>;
using Frames = std::vector<std::int16_t>;

// ---- The chip ----

// A chip whose output stage is at rest, both filters at 0, so a level given to it is a
// step from 0.
ApuState atRest() {
  ApuState state{};
  state.filter = ApuFilter{.highPass = 0, .lowPass = 0};
  return state;
}

// Gives the stage `level` for `cycles` cycles and answers its last answer.
std::int16_t held(ApuState& state, std::int16_t level, long cycles) {
  std::int16_t answer = 0;
  for (long cycle = 0; cycle < cycles; ++cycle) answer = apuFilter(state, level);
  return answer;
}

// ---- The machine ----

// One picture with rendering off, in master cycles.
constexpr std::uint64_t kFrame = consoleClock(Region::Ntsc).masterCyclesPerFrame;

// A program that writes no register: the mixer's level stays 8074, the triangle's 15
// alone.
// JMP $8000
const Bytes kIdleProgram{0x4C, 0x00, 0x80};

// The DMC's level loaded with 127: the write lands in machine cycle 12 and takes the
// mixer's level from 8074 to 22 325.
// LDA #$7F; STA $4011; JMP $8005
const Bytes kLevelProgram{0xA9, 0x7F, 0x8D, 0x11, 0x40, 0x4C, 0x05, 0x80};

// A 16 KB NROM image with 8 KB of CHR ROM, the program at $8000 and every vector there.
Bytes idleImage() {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  placeProgram(image, 0, kIdleProgram);
  return image;
}

// A 16 KB NROM image with 8 KB of CHR ROM, the program at $8000 and every vector there.
Bytes levelImage() {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  placeProgram(image, 0, kLevelProgram);
  return image;
}

void stepCycles(Nes& machine, long cycles) {
  for (long i = 0; i < cycles; ++i) machine.step();
}

// ---- ApuFilter ----

// At power-on the high-pass holds the mixer's level, 8074 with the triangle at its first
// step (NESdev wiki, "CPU power up state", APU; "APU Mixer"), so the stage answers 0
// while that level stands.
TEST(ApuFilter, PowerOnHoldsTheLevelTheDacsStartAt) {
  ApuState state{};
  EXPECT_EQ(apuMix(state, 0), 8074);
  EXPECT_EQ(state.filter.highPass, 135459241984);
  EXPECT_EQ(state.filter.lowPass, 0);
  for (int cycle = 1; cycle <= 100; ++cycle) {
    ASSERT_EQ(apuFilter(state, 8074), 0) << "cycle " << cycle;
  }
  EXPECT_EQ(state.filter, ApuFilter{});
}

// From rest, one cycle at 16 384: the high-pass holds 16 384 times its step, 5 of a level
// rounded, and the low-pass 16 379 times its own.
TEST(ApuFilter, OneCycleMovesEachFilterByItsStep) {
  ApuState state = atRest();
  EXPECT_EQ(apuFilter(state, 16384), 786);
  EXPECT_EQ(state.filter.highPass, 86835200);
  EXPECT_EQ(state.filter.lowPass, 13179182181);
}

// A step of 16 384 from rest: the low-pass rises with a time constant of 21 cycles and
// the high-pass takes the level away with one of 3165, so the answer is 16 384 / e, less
// the low-pass's lag, after 3165.
TEST(ApuFilter, AStepPassesAndTheHighPassTakesItAway) {
  ApuState state = atRest();
  EXPECT_EQ(held(state, 16384, 1), 786) << "cycle 1";
  EXPECT_EQ(held(state, 16384, 1), 1533) << "cycle 2";
  EXPECT_EQ(held(state, 16384, 1), 2245) << "cycle 3";
  EXPECT_EQ(held(state, 16384, 17), 10214) << "cycle 20";
  EXPECT_EQ(held(state, 16384, 80), 15854) << "cycle 100";
  EXPECT_EQ(held(state, 16384, 3065), 6065) << "cycle 3165";
  EXPECT_EQ(held(state, 16384, 3165), 2231) << "cycle 6330";
  EXPECT_EQ(held(state, 16384, 13670), 30) << "cycle 20000";
}

// A level that stands is taken away whole: the high-pass stops within its step's reach of
// it, 3165 units of a 16 777 216th, and the rounded answer is 0 from then on.
TEST(ApuFilter, AStandingLevelSettlesToZero) {
  ApuState state = atRest();
  EXPECT_EQ(held(state, 16384, 32921), 1) << "cycle 32921";
  for (long cycle = 32922; cycle <= 100000; ++cycle) {
    ASSERT_EQ(apuFilter(state, 16384), 0) << "cycle " << cycle;
  }
  EXPECT_EQ(state.filter.highPass, 274877903779);
  EXPECT_EQ(state.filter.lowPass, 0);
}

// The level falling back to 0 passes as the same step downward, and the high-pass,
// closing from above, reaches 0 itself.
TEST(ApuFilter, AFallMirrorsARise) {
  ApuState state = atRest();
  ASSERT_EQ(held(state, 16384, 100000), 0);
  EXPECT_EQ(held(state, 0, 1), -786) << "cycle 1";
  EXPECT_EQ(held(state, 0, 1), -1533) << "cycle 2";
  EXPECT_EQ(held(state, 0, 1), -2245) << "cycle 3";
  EXPECT_EQ(held(state, 0, 3162), -6065) << "cycle 3165";
  EXPECT_EQ(held(state, 0, 96835), 0) << "cycle 100000";
  EXPECT_EQ(state.filter.highPass, 0);
}

// The beep's wave, a step of 4895 every 896 cycles: the high-pass centres it and lets each
// half sag by a quarter, from 2792 to 2104, and the low-pass rounds each edge over its
// first cycles.
TEST(ApuFilter, ASquareWaveSagsBetweenItsEdges) {
  ApuState state = atRest();
  for (int wave = 1; wave <= 39; ++wave) {
    held(state, 4895, 896);
    held(state, 0, 896);
  }
  EXPECT_EQ(held(state, 4895, 1), -1881) << "cycle 1";
  EXPECT_EQ(held(state, 4895, 99), 2686) << "cycle 100";
  EXPECT_EQ(held(state, 4895, 796), 2117) << "cycle 896";
  EXPECT_EQ(held(state, 0, 1), 1881) << "cycle 897";
  EXPECT_EQ(held(state, 0, 99), -2686) << "cycle 996";
  EXPECT_EQ(held(state, 0, 796), -2117) << "cycle 1792";
}

// A cartridge's level can take the mixer's from one end of a sample's range to the other,
// a step of 65 535: the stage's answer is held at the range's ends.
TEST(ApuFilter, TheAnswerIsHeldWithinASample) {
  ApuState rising = atRest();
  ASSERT_EQ(held(rising, -32768, 100000), 0);
  EXPECT_EQ(held(rising, 32767, 1), 3142) << "cycle 1";
  EXPECT_EQ(held(rising, 32767, 13), 32516) << "cycle 14";
  for (int cycle = 15; cycle <= 200; ++cycle) {
    ASSERT_EQ(apuFilter(rising, 32767), 32767) << "cycle " << cycle;
  }

  ApuState falling = atRest();
  ASSERT_EQ(held(falling, 32767, 100000), 0);
  EXPECT_EQ(held(falling, -32768, 14), -32516) << "cycle 14";
  EXPECT_EQ(held(falling, -32768, 1), -32768) << "cycle 15";
}

// The stage's capacitors stand outside the chip, so the reset line leaves both filters
// where they are.
TEST(ApuFilter, ResetKeepsTheStage) {
  ApuState state = atRest();
  held(state, 18817, 20);
  EXPECT_EQ(state.filter.highPass, 1988627333);
  EXPECT_EQ(state.filter.lowPass, 196808321944);
  const ApuFilter before = state.filter;
  apuReset(state);
  EXPECT_EQ(state.filter, before);
}

// ---- FilterMachine ----

// The mixer's level is 8074 from the first cycle, the triangle at its first step, and the
// stage is settled on it at power-on: a program that writes no register is silent.
TEST(FilterMachine, AnIdleMachineIsSilentFromPowerOn) {
  const Bytes image = idleImage();
  Nes machine(NesConfig{.rom = image});
  machine.run(2 * kFrame);
  const Frames frames = machine.takeFrames();
  ASSERT_EQ(frames.size(), 1597u);
  for (std::size_t frame = 0; frame < frames.size(); ++frame) {
    ASSERT_EQ(frames[frame], 0) << "frame " << frame;
  }
  EXPECT_EQ(machine.state().apu.filter, ApuFilter{});
}

// The two filters are in the state value: a machine restored from a snapshot taken while
// a step is fading makes the frames that follow it.
TEST(FilterMachine, ASnapshotCarriesTheStage) {
  const Bytes image = levelImage();
  Nes machine(NesConfig{.rom = image});
  stepCycles(machine, 1000);
  const NesState snapshot = machine.state();
  EXPECT_EQ(snapshot.apu.filter.highPass, 199568879180);
  EXPECT_EQ(snapshot.apu.filter.lowPass, 176086889384);
  const Frames opening = machine.takeFrames();
  ASSERT_EQ(opening.size(), 26u);
  EXPECT_EQ(opening[0], 4366);
  EXPECT_EQ(opening[1], 12342);

  Nes restored(NesConfig{.rom = image});
  restored.restore(snapshot);
  stepCycles(machine, 5000);
  stepCycles(restored, 5000);
  const Frames following = machine.takeFrames();
  ASSERT_EQ(following.size(), 134u);
  EXPECT_EQ(following.front(), 10532);
  EXPECT_EQ(following.back(), 2198);
  EXPECT_EQ(restored.takeFrames(), following);
}

// The reset button leaves the output stage as it is, a fading step included.
TEST(FilterMachine, ResetLeavesTheStageWhereItStands) {
  const Bytes image = levelImage();
  Nes machine(NesConfig{.rom = image});
  stepCycles(machine, 1000);
  const ApuFilter before = machine.state().apu.filter;
  EXPECT_NE(before, ApuFilter{});
  machine.reset();
  EXPECT_EQ(machine.state().apu.filter, before);
}

// A machine built with an audioRate of 0 has no output, and its stage stays as power-on
// set it whatever the program writes.
TEST(FilterMachine, ARateOfZeroLeavesTheStageAsPowerOnSetIt) {
  const Bytes image = levelImage();
  Nes machine(NesConfig{.rom = image, .audioRate = 0});
  machine.run(kFrame);
  EXPECT_EQ(machine.state().apu.filter, ApuFilter{});
}

}  // namespace
