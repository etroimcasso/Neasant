#include "neasant/apu/apu.h"
#include "neasant/nes/nes.h"
#include "authored_image.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <tuple>
#include <vector>

// The APU's pulse, triangle and noise channels driven through apu.h a cycle at a time, a
// write landing in a cycle's access and the tick following in the same half, with every
// expected level, step and count derived from the channels' pages; then two programs on
// the machine. A cycle below is a tick index counted from a chip's first tick, which is a
// get cycle unless the case builds the chip on a put, so the get cycles are the even ones.

namespace {

using neasant::ApuState;
using neasant::apuPeekStatus;
using neasant::apuReset;
using neasant::apuTickCycle;
using neasant::apuWrite;
using neasant::Nes;
using neasant::NesConfig;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::placeProgram;

using Bytes = std::vector<std::uint8_t>;
using Program = std::initializer_list<std::uint8_t>;

constexpr std::uint8_t kGet = 0;
constexpr std::uint8_t kPut = 1;

// The chip driven as the machine drives it: a write lands in the current cycle's half, a
// tick ends the cycle in that half, and the half then alternates.
struct Chip {
  ApuState state{};
  std::uint8_t half;

  explicit Chip(std::uint8_t first = kGet) : half(first) {}

  void write(std::uint16_t address, std::uint8_t value) {
    apuWrite(state, address, value, half);
  }

  void tick(long n = 1) {
    for (long i = 0; i < n; ++i) {
      apuTickCycle(state, half);
      half = static_cast<std::uint8_t>(half ^ 1u);
    }
  }

  // One quarter-frame and one half-frame clock: $4017 = $80 restarts the 5-step sequence,
  // which clocks both the tick before its restart, on the third tick from a get cycle and
  // the second from a put (NESdev wiki, "APU Frame Counter", Control register).
  void clockBoth() {
    write(0x4017, 0x80);
    tick(4);
  }
};

// Ticks `chip` until tick `k` has run, `done` counting the ticks already run.
void throughTick(Chip& chip, long& done, long k) {
  chip.tick(k + 1 - done);
  done = k + 1;
}

// Pulse 1 enabled with `settings` in $4000, the sweep's negate flag set so that no target
// is above $7FF (NESdev wiki, "APU Pulse", Sweep unit operation), the period given and the
// length counter loading 254.
Chip pulseOne(std::uint16_t period, std::uint8_t settings, std::uint8_t first = kGet) {
  Chip chip(first);
  chip.write(0x4015, 0x01);
  chip.write(0x4000, settings);
  chip.write(0x4001, 0x08);
  chip.write(0x4002, static_cast<std::uint8_t>(period & 0xFFu));
  chip.write(0x4003, static_cast<std::uint8_t>(0x08u | (period >> 8)));
  return chip;
}

// Both pulses enabled at one period with one sweep setting, a 50% duty and constant
// volume 15.
Chip twoPulses(std::uint16_t period, std::uint8_t sweep) {
  Chip chip;
  chip.write(0x4015, 0x03);
  for (const std::uint16_t base : {std::uint16_t{0x4000}, std::uint16_t{0x4004}}) {
    chip.write(base, 0xBF);
    chip.write(static_cast<std::uint16_t>(base + 1), sweep);
    chip.write(static_cast<std::uint16_t>(base + 2), static_cast<std::uint8_t>(period & 0xFFu));
    chip.write(static_cast<std::uint16_t>(base + 3), static_cast<std::uint8_t>(0x08u | (period >> 8)));
  }
  return chip;
}

// The triangle enabled with `linear` in $4008, the period given, its length counter
// loading 254 and its reload flag set.
Chip triangleAt(std::uint8_t period, std::uint8_t linear) {
  Chip chip;
  chip.write(0x4015, 0x04);
  chip.write(0x4008, linear);
  chip.write(0x400A, period);
  chip.write(0x400B, 0x08);
  return chip;
}

// Pulse 1's envelope restarting with a divider of 1, its sweep halving the period at every
// half-frame clock, and the linear counter reloading 127 once: one unit for each clock to
// move.
Chip clockedUnits(std::uint8_t mode) {
  Chip chip(kPut);
  chip.write(0x4015, 0x05);
  chip.write(0x4000, 0x00);
  chip.write(0x4001, 0x89);
  chip.write(0x4002, 0x00);
  chip.write(0x4003, 0x0C);
  chip.write(0x4008, 0x7F);
  chip.write(0x400B, 0x08);
  chip.write(0x4017, mode);
  return chip;
}

using Units = std::tuple<int, int, int>;

// Pulse 1's decay level, the linear counter and pulse 1's period.
Units unitsOf(const Chip& chip) {
  return {chip.state.pulse[0].envelope.decay, chip.state.triangle.linear,
          chip.state.pulse[0].period};
}

// The highest level pulse `index` answers over `ticks` ticks.
int pulsePeak(Chip& chip, std::size_t index, long ticks) {
  int peak = 0;
  for (long i = 0; i < ticks; ++i) {
    chip.tick();
    peak = std::max(peak, static_cast<int>(chip.state.pulse[index].output));
  }
  return peak;
}

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

// ---- Pulse ----

TEST(Pulse, TheTimerStepsTheSequencerEveryPeriodPlusOneApuCycles) {
  // NESdev wiki, "APU Pulse", Sequencer operation: the timer counts t down to 0 once an APU
  // cycle and clocks the sequencer going from 0 to t; a waveform takes 16 * (t + 1) CPU
  // cycles. A 50% duty at constant volume 15 and a period of 8: a step lasts 18 cycles.
  Chip c = pulseOne(8, 0xBF);
  const auto& pulse = c.state.pulse[0];
  long done = 0;
  EXPECT_EQ(pulse.step, 0);
  throughTick(c, done, 0);
  EXPECT_EQ(c.state.length[0].counter, 254);
  EXPECT_EQ(pulse.output, 15);
  EXPECT_EQ(pulse.step, 1);
  EXPECT_EQ(pulse.timer, 8);
  throughTick(c, done, 17);
  EXPECT_EQ(pulse.step, 1);
  throughTick(c, done, 18);
  EXPECT_EQ(pulse.step, 2);
  throughTick(c, done, 71);
  EXPECT_EQ(pulse.output, 15);
  throughTick(c, done, 72);
  EXPECT_EQ(pulse.output, 0);
  EXPECT_EQ(pulse.step, 5);
  throughTick(c, done, 143);
  EXPECT_EQ(pulse.output, 0);
  EXPECT_EQ(pulse.step, 0);
  throughTick(c, done, 144);
  EXPECT_EQ(pulse.output, 15);
  EXPECT_EQ(pulse.step, 1);
}

TEST(Pulse, TheTimersCountOnGetCyclesAlone) {
  // Visual 2A03: the pulse's and the noise's units change as a put cycle begins, on the
  // tick of the get cycle before it. A chip whose first cycle is a put steps one cycle
  // later throughout.
  Chip c = pulseOne(8, 0xBF, kPut);
  const auto& pulse = c.state.pulse[0];
  long done = 0;
  throughTick(c, done, 0);
  EXPECT_EQ(pulse.output, 0);
  EXPECT_EQ(pulse.step, 0);
  throughTick(c, done, 1);
  EXPECT_EQ(pulse.output, 15);
  EXPECT_EQ(pulse.step, 1);
  throughTick(c, done, 72);
  EXPECT_EQ(pulse.output, 15);
  throughTick(c, done, 73);
  EXPECT_EQ(pulse.output, 0);

  // Every tick a put: neither timer counts.
  Chip puts = pulseOne(8, 0xBF);
  for (int i = 0; i < 100; ++i) apuTickCycle(puts.state, kPut);
  EXPECT_EQ(puts.state.pulse[0].step, 0);
  EXPECT_EQ(puts.state.pulse[0].timer, 0);
  EXPECT_EQ(puts.state.noise.shift, 1);
}

TEST(Pulse, EachDutyReadsItsWaveform) {
  // NESdev wiki, "APU Pulse", Duty Cycle Sequences, the output waveform.
  constexpr std::array<std::array<int, 8>, 4> kRows{{
      {0, 1, 0, 0, 0, 0, 0, 0},
      {0, 1, 1, 0, 0, 0, 0, 0},
      {0, 1, 1, 1, 1, 0, 0, 0},
      {1, 0, 0, 1, 1, 1, 1, 1},
  }};
  constexpr std::array<long, 8> kTicks{0, 1, 19, 37, 55, 73, 91, 109};
  for (std::size_t duty = 0; duty < kRows.size(); ++duty) {
    Chip c = pulseOne(8, static_cast<std::uint8_t>((duty << 6) | 0x3Fu), kPut);
    long done = 0;
    for (std::size_t i = 0; i < kTicks.size(); ++i) {
      throughTick(c, done, kTicks[i]);
      EXPECT_EQ(c.state.pulse[0].output, kRows[duty][i] * 15) << duty << " " << i;
    }
  }
}

TEST(Pulse, AWriteToTheFourthRegisterReturnsTheSequencerToItsFirstStepAndLeavesTheTimer) {
  // NESdev wiki, "APU Pulse": writing $4003 resets the sequencer to its first value;
  // blargg's NES APU reference: changing a divider's period does not affect its count.
  Chip c = pulseOne(8, 0xBF);
  c.tick(40);
  EXPECT_EQ(c.state.pulse[0].step, 3);
  EXPECT_EQ(c.state.pulse[0].timer, 7);
  c.write(0x4003, 0x08);
  EXPECT_EQ(c.state.pulse[0].step, 0);
  EXPECT_EQ(c.state.pulse[0].timer, 7);
  EXPECT_TRUE(c.state.pulse[0].envelope.start);
  EXPECT_EQ(c.state.pulse[0].period, 8);
  c.tick();
  EXPECT_EQ(c.state.pulse[0].output, 0);
}

TEST(Pulse, APeriodBelowEightIsSilent) {
  // NESdev wiki, "APU Pulse", Sequencer operation: a period t < 8 silences the channel.
  Chip seven = pulseOne(7, 0xBF);
  EXPECT_EQ(pulsePeak(seven, 0, 200), 0);
  EXPECT_NE(seven.state.pulse[0].step, 0);
  Chip eight = pulseOne(8, 0xBF);
  EXPECT_EQ(pulsePeak(eight, 0, 200), 15);
}

TEST(Pulse, PulseTwoAnswersAtItsOwnRegisters) {
  // NESdev wiki, "APU Pulse", Registers: $4004-$4007 are pulse 2's.
  Chip c;
  c.write(0x4015, 0x02);
  c.write(0x4004, 0xBF);
  c.write(0x4005, 0x08);
  c.write(0x4006, 0x08);
  c.write(0x4007, 0x08);
  c.tick(2);
  EXPECT_EQ(c.state.pulse[1].output, 15);
  EXPECT_EQ(c.state.pulse[1].period, 8);
  EXPECT_EQ(c.state.pulse[1].duty, 2);
  EXPECT_TRUE(c.state.pulse[1].sweep.negate);
  EXPECT_EQ(c.state.pulse[0].output, 0);
  EXPECT_EQ(c.state.pulse[0].period, 0);
  c.clockBoth();
  EXPECT_EQ(c.state.pulse[1].envelope.decay, 15);
  EXPECT_EQ(c.state.pulse[0].envelope.decay, 0);
}

TEST(Pulse, ALengthCounterAtZeroSilencesItAndAnEnableRestoresNothing) {
  // NESdev wiki, "APU Length Counter", Clocking; "APU Pulse", Mixer output.
  Chip c = pulseOne(8, 0xBF);
  c.tick(2);
  EXPECT_EQ(c.state.pulse[0].output, 15);
  c.write(0x4015, 0x00);
  EXPECT_EQ(c.state.length[0].counter, 0);
  EXPECT_EQ(pulsePeak(c, 0, 144), 0);
  c.write(0x4015, 0x01);
  EXPECT_EQ(pulsePeak(c, 0, 144), 0);
  EXPECT_EQ(c.state.length[0].counter, 0);
  c.write(0x4003, 0x08);
  EXPECT_EQ(pulsePeak(c, 0, 144), 15);
}

// ---- Envelope ----

TEST(Envelope, AStartLoadsFifteenAndTheDividerCountsTheVolumePlusOneClocks) {
  // NESdev wiki, "APU Envelope": a parameter of 2 counts the divider 2, 1, 0 and the fourth
  // clock reloads it and takes one from the level, three clocks a level.
  Chip c = pulseOne(8, 0x02);
  const auto& envelope = c.state.pulse[0].envelope;
  EXPECT_TRUE(envelope.start);
  EXPECT_EQ(envelope.decay, 0);
  c.clockBoth();
  EXPECT_FALSE(envelope.start);
  EXPECT_EQ(envelope.decay, 15);
  EXPECT_EQ(envelope.divider, 2);
  for (int n = 2; n <= 46; ++n) {
    c.clockBoth();
    EXPECT_EQ(envelope.decay, 15 - (n - 1) / 3) << n;
  }
  for (int n = 47; n <= 49; ++n) {
    c.clockBoth();
    EXPECT_EQ(envelope.decay, 0) << n;
  }
}

TEST(Envelope, TheLoopFlagReturnsTheDecayToFifteen) {
  // NESdev wiki, "APU Envelope": otherwise if the loop flag is set, the decay level
  // counter is loaded with 15.
  Chip c = pulseOne(8, 0x20);
  const auto& envelope = c.state.pulse[0].envelope;
  for (int n = 1; n <= 16; ++n) {
    c.clockBoth();
    EXPECT_EQ(envelope.decay, 16 - n) << n;
  }
  c.clockBoth();
  EXPECT_EQ(envelope.decay, 15);
  c.clockBoth();
  EXPECT_EQ(envelope.decay, 14);
}

TEST(Envelope, ConstantVolumeIsTheParameterWhileTheDecayRuns) {
  // NESdev wiki, "APU Envelope": the decay level is still updated when constant volume is
  // selected. A 50% duty at constant volume 1 and a step 4096 cycles long.
  Chip c = pulseOne(0x7FF, 0x91);
  c.tick(2);
  EXPECT_EQ(c.state.pulse[0].output, 1);
  for (int n = 0; n < 5; ++n) c.clockBoth();
  EXPECT_EQ(c.state.pulse[0].envelope.decay, 13);
  EXPECT_EQ(c.state.pulse[0].output, 1);
  c.write(0x4000, 0x81);
  c.tick();
  EXPECT_EQ(c.state.pulse[0].output, 13);
}

TEST(Envelope, AWriteToTheFourthRegisterRestartsIt) {
  // NESdev wiki, "APU Envelope": a write to the fourth register sets the start flag.
  Chip c = pulseOne(8, 0x00);
  for (int n = 0; n < 4; ++n) c.clockBoth();
  EXPECT_EQ(c.state.pulse[0].envelope.decay, 12);
  c.write(0x4003, 0x08);
  EXPECT_EQ(c.state.pulse[0].envelope.decay, 12);
  c.clockBoth();
  EXPECT_EQ(c.state.pulse[0].envelope.decay, 15);
}

// ---- Sweep ----

TEST(Sweep, PulseOneSubtractsOneMoreThanPulseTwo) {
  // NESdev wiki, "APU Pulse", Sweep unit operation: making 20 negative produces a change
  // amount of -21 on pulse 1 and -20 on pulse 2.
  Chip c = twoPulses(80, 0x8A);
  c.clockBoth();
  EXPECT_EQ(c.state.pulse[0].period, 59);
  EXPECT_EQ(c.state.pulse[1].period, 60);
  c.clockBoth();
  EXPECT_EQ(c.state.pulse[0].period, 44);
  EXPECT_EQ(c.state.pulse[1].period, 45);
}

TEST(Sweep, WithNegateClearTheTargetAdds) {
  // NESdev wiki, "APU Pulse", Sweep unit operation: the target is the period plus the
  // change.
  Chip c = twoPulses(80, 0x82);
  c.clockBoth();
  EXPECT_EQ(c.state.pulse[0].period, 100);
  EXPECT_EQ(c.state.pulse[1].period, 100);
  c.clockBoth();
  EXPECT_EQ(c.state.pulse[0].period, 125);
  EXPECT_EQ(c.state.pulse[1].period, 125);
}

TEST(Sweep, TheDividerCountsThePeriodPlusOneHalfFramesAndAWriteReloadsIt) {
  // NESdev wiki, "APU Pulse", Sweep unit operation, the half-frame clock's two steps. The
  // divider is 0 at power-on, so clock 1 updates and reloads it to 2; clock 4 updates
  // next. A write between clocks 5 and 6 makes clock 6 reload without updating.
  Chip c = twoPulses(256, 0xA9);
  constexpr std::array<int, 5> kFirst{128, 128, 128, 64, 64};
  for (std::size_t i = 0; i < kFirst.size(); ++i) {
    c.clockBoth();
    EXPECT_EQ(c.state.pulse[1].period, kFirst[i]) << i + 1;
  }
  EXPECT_EQ(c.state.pulse[1].sweep.divider, 1);
  c.write(0x4005, 0xA9);
  EXPECT_TRUE(c.state.pulse[1].sweep.reload);
  constexpr std::array<int, 4> kSecond{64, 64, 64, 32};
  for (std::size_t i = 0; i < kSecond.size(); ++i) {
    c.clockBoth();
    EXPECT_EQ(c.state.pulse[1].period, kSecond[i]) << i + 6;
  }
  EXPECT_FALSE(c.state.pulse[1].sweep.reload);
}

TEST(Sweep, ATargetAboveTheCeilingSilencesWithTheSweepDisabled) {
  // NESdev wiki, "APU Pulse", Sweep unit operation: a target above $7FF silences the
  // channel even if the sweep unit is not enabled.
  Chip c = pulseOne(0x400, 0xBF);
  c.write(0x4001, 0x00);
  c.tick(2);
  EXPECT_EQ(c.state.pulse[0].step, 1);
  EXPECT_EQ(c.state.pulse[0].output, 0);
  c.write(0x4001, 0x08);
  c.tick();
  EXPECT_EQ(c.state.pulse[0].output, 15);

  Chip below = pulseOne(0x3FF, 0xBF);
  below.write(0x4001, 0x00);
  below.tick(2);
  EXPECT_EQ(below.state.pulse[0].output, 15);
}

TEST(Sweep, ADisabledSweepASilencedChannelAndAZeroShiftLeaveThePeriod) {
  // NESdev wiki, "APU Pulse", Sweep unit operation, step 1's conditions; blargg's NES APU
  // reference, Sweep Unit.
  struct Setting {
    std::uint16_t period;
    std::uint8_t sweep;
  };
  for (const Setting s : {Setting{256, 0x09}, Setting{0x600, 0x81}, Setting{256, 0x88}}) {
    Chip c = twoPulses(s.period, s.sweep);
    for (int n = 0; n < 3; ++n) c.clockBoth();
    EXPECT_EQ(c.state.pulse[0].period, s.period) << static_cast<int>(s.sweep);
    EXPECT_EQ(c.state.pulse[1].period, s.period) << static_cast<int>(s.sweep);
  }
}

TEST(Sweep, ItStopsBelowThePeriodFloor) {
  // blargg's NES APU reference, Sweep Unit: with the period below 8 the sweep unit does
  // not change it.
  Chip c = twoPulses(8, 0x89);
  c.clockBoth();
  EXPECT_EQ(c.state.pulse[0].period, 3);
  EXPECT_EQ(c.state.pulse[1].period, 4);
  for (int n = 0; n < 3; ++n) c.clockBoth();
  EXPECT_EQ(c.state.pulse[0].period, 3);
  EXPECT_EQ(c.state.pulse[1].period, 4);
  EXPECT_EQ(pulsePeak(c, 1, 200), 0);
}

TEST(Sweep, ItWaitsOnTheLengthCounterAndItsDividerDoesNot) {
  // 2A03 technical reference, Sweep unit: the period is rewritten only while the length
  // counter is above 0; Visual 2A03 shows it, the divider counting either way.
  Chip c = twoPulses(0x100, 0x81);
  c.write(0x4015, 0x01);
  c.clockBoth();
  c.clockBoth();
  EXPECT_EQ(c.state.pulse[0].period, 0x240);
  EXPECT_EQ(c.state.pulse[1].period, 0x100);

  // A counter of 2, the halt bit clear: the second update comes in the clock that empties
  // the counter.
  Chip two = twoPulses(0x100, 0x81);
  two.write(0x4000, 0x9F);
  two.write(0x4003, 0x19);
  constexpr std::array<int, 4> kPeriods{0x180, 0x240, 0x240, 0x240};
  constexpr std::array<int, 4> kCounters{1, 0, 0, 0};
  for (std::size_t i = 0; i < kPeriods.size(); ++i) {
    two.clockBoth();
    EXPECT_EQ(two.state.pulse[0].period, kPeriods[i]) << i + 1;
    EXPECT_EQ(two.state.length[0].counter, kCounters[i]) << i + 1;
  }

  Chip silent = twoPulses(0x100, 0xA1);
  silent.write(0x4015, 0x00);
  constexpr std::array<int, 4> kDividers{2, 1, 0, 2};
  for (std::size_t i = 0; i < kDividers.size(); ++i) {
    silent.clockBoth();
    EXPECT_EQ(silent.state.pulse[0].sweep.divider, kDividers[i]) << i + 1;
    EXPECT_EQ(silent.state.pulse[0].period, 0x100) << i + 1;
  }
}

// ---- FrameClocks ----

TEST(FrameClocks, TheFourStepSequenceClocksTheEnvelopesAndTheLinearCounterFourTimesAndTheSweepsTwice) {
  // NESdev wiki, "APU Frame Counter", the 4-step table: quarter frames at steps 1 to 4,
  // half frames at 2 and 4. The write on a put puts the origin at tick 2.
  Chip c = clockedUnits(0x00);
  long done = 0;
  throughTick(c, done, 7458);
  EXPECT_EQ(unitsOf(c), Units(0, 0, 1024));
  throughTick(c, done, 7459);
  EXPECT_EQ(unitsOf(c), Units(15, 127, 1024));
  throughTick(c, done, 14914);
  EXPECT_EQ(unitsOf(c), Units(15, 127, 1024));
  throughTick(c, done, 14915);
  EXPECT_EQ(unitsOf(c), Units(14, 126, 511));
  throughTick(c, done, 22372);
  EXPECT_EQ(unitsOf(c), Units(14, 126, 511));
  throughTick(c, done, 22373);
  EXPECT_EQ(unitsOf(c), Units(13, 125, 511));
  throughTick(c, done, 29830);
  EXPECT_EQ(unitsOf(c), Units(13, 125, 511));
  throughTick(c, done, 29831);
  EXPECT_EQ(unitsOf(c), Units(12, 124, 255));
}

TEST(FrameClocks, TheFiveStepSequenceClocksThemTheCycleBeforeItsRestartAndSkipsItsFourthStep) {
  // NESdev wiki, "APU Frame Counter", the 5-step table; "APU", Frame Counter: the write
  // clocks both units 2 or 3 cycles after it, the cycle before the restart.
  Chip c = clockedUnits(0x80);
  long done = 0;
  throughTick(c, done, 0);
  EXPECT_EQ(unitsOf(c), Units(0, 0, 1024));
  throughTick(c, done, 1);
  EXPECT_EQ(unitsOf(c), Units(15, 127, 511));
  throughTick(c, done, 7459);
  EXPECT_EQ(unitsOf(c), Units(14, 126, 511));
  throughTick(c, done, 14915);
  EXPECT_EQ(unitsOf(c), Units(13, 125, 255));
  throughTick(c, done, 22373);
  EXPECT_EQ(unitsOf(c), Units(12, 124, 255));
  throughTick(c, done, 37282);
  EXPECT_EQ(unitsOf(c), Units(12, 124, 255));
  throughTick(c, done, 37283);
  EXPECT_EQ(unitsOf(c), Units(11, 123, 127));
}

// ---- Triangle ----

TEST(Triangle, TheSequenceRunsFifteenToZeroAndBack) {
  // NESdev wiki, "APU Triangle", Sequencer operation: a clock every period + 1 CPU
  // cycles. The timer reloads to 3 at tick 0 with the sequencer standing and counts 2, 1,
  // 0; the write's quarter-frame clock loads the linear counter at tick 2.
  constexpr std::array<int, 32> kSequence{15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,
                                          4,  3,  2,  1,  0,  0,  1,  2,  3,  4,  5,
                                          6,  7,  8,  9,  10, 11, 12, 13, 14, 15};
  Chip c = triangleAt(3, 0xFF);
  c.write(0x4017, 0x80);
  long done = 0;
  throughTick(c, done, 3);
  EXPECT_EQ(c.state.triangle.linear, 127);
  EXPECT_EQ(c.state.triangle.step, 0);
  EXPECT_EQ(c.state.triangle.output, 15);
  for (long k = 1; k <= 33; ++k) {
    const int expected = kSequence[static_cast<std::size_t>(k % 32)];
    throughTick(c, done, 4 * k);
    EXPECT_EQ(c.state.triangle.output, expected) << k;
    throughTick(c, done, 4 * k + 3);
    EXPECT_EQ(c.state.triangle.output, expected) << k;
  }
}

TEST(Triangle, ALinearCounterAtZeroStopsTheSequencerWhereItStands) {
  // NESdev wiki, "APU Triangle": the sequencer is clocked only while the linear counter
  // and the length counter are both above 0; "APU", Status: the status of the linear
  // counter is irrelevant to bit 2.
  Chip c = triangleAt(3, 0x02);
  c.clockBoth();  // ticks 0-3
  EXPECT_EQ(c.state.triangle.linear, 2);
  EXPECT_FALSE(c.state.triangle.linearReload);
  EXPECT_EQ(c.state.triangle.step, 0);
  c.tick(8);  // 4-11
  EXPECT_EQ(c.state.triangle.step, 2);
  EXPECT_EQ(c.state.triangle.output, 13);
  c.clockBoth();  // 12-15
  EXPECT_EQ(c.state.triangle.linear, 1);
  EXPECT_EQ(c.state.triangle.step, 3);
  c.clockBoth();  // 16-19
  EXPECT_EQ(c.state.triangle.linear, 0);
  EXPECT_EQ(c.state.triangle.step, 4);
  EXPECT_EQ(c.state.triangle.output, 11);
  c.tick(40);  // 20-59
  EXPECT_EQ(c.state.triangle.step, 4);
  EXPECT_EQ(c.state.triangle.output, 11);
  EXPECT_EQ(c.state.triangle.timer, 0);
  EXPECT_EQ(c.state.length[2].counter, 251);
  EXPECT_EQ(apuPeekStatus(c.state, 0) & 0x04, 0x04);
}

TEST(Triangle, TheControlFlagKeepsTheReloadFlagSet) {
  // NESdev wiki, "APU Triangle", the quarter-frame clock's two actions.
  Chip c = triangleAt(3, 0x82);
  for (int n = 0; n < 3; ++n) {
    c.clockBoth();
    EXPECT_EQ(c.state.triangle.linear, 2) << n;
    EXPECT_TRUE(c.state.triangle.linearReload) << n;
  }
  c.write(0x4008, 0x02);
  c.clockBoth();
  EXPECT_EQ(c.state.triangle.linear, 2);
  EXPECT_FALSE(c.state.triangle.linearReload);
  c.clockBoth();
  EXPECT_EQ(c.state.triangle.linear, 1);
  c.clockBoth();
  EXPECT_EQ(c.state.triangle.linear, 0);
  c.clockBoth();
  EXPECT_EQ(c.state.triangle.linear, 0);

  // No write to $400B, no reload.
  Chip unset;
  unset.write(0x4015, 0x04);
  unset.write(0x4008, 0x7F);
  unset.clockBoth();
  EXPECT_EQ(unset.state.triangle.linear, 0);
}

TEST(Triangle, ALengthCounterAtZeroStopsItAndItKeepsItsLevel) {
  // NESdev wiki, "APU": silencing the triangle merely halts it; it continues to output its
  // last value rather than 0.
  Chip c = triangleAt(3, 0xFF);
  c.write(0x4017, 0x80);
  c.tick(21);
  EXPECT_EQ(c.state.triangle.step, 5);
  EXPECT_EQ(c.state.triangle.output, 10);
  c.write(0x4015, 0x00);
  c.tick(40);
  EXPECT_EQ(c.state.triangle.step, 5);
  EXPECT_EQ(c.state.triangle.output, 10);
  EXPECT_EQ(apuPeekStatus(c.state, 0) & 0x04, 0x00);
}

TEST(Triangle, APeriodOfZeroStepsEveryCycleFromTheTickThatLoadsTheLinearCounter) {
  // NESdev wiki, "APU Triangle": the channel can generate ultrasonic frequencies. The
  // write's clock loads the linear counter before the same tick's timer.
  Chip c = triangleAt(0, 0xFF);
  c.write(0x4017, 0x80);
  long done = 0;
  throughTick(c, done, 1);
  EXPECT_EQ(c.state.triangle.step, 0);
  throughTick(c, done, 2);
  EXPECT_EQ(c.state.triangle.step, 1);
  EXPECT_EQ(c.state.triangle.output, 14);
  throughTick(c, done, 33);
  EXPECT_EQ(c.state.triangle.step, 0);
  EXPECT_EQ(c.state.triangle.output, 15);
}

TEST(Triangle, PowerOnAndResetLeaveItAtItsFirstStep) {
  // NESdev wiki, "CPU power up state", APU, the After Reset column: triangle phase 0,
  // output 15; blargg's apu_mixer shell: at power the DAC starts at 15.
  Chip fresh;
  EXPECT_EQ(fresh.state.triangle.step, 0);
  EXPECT_EQ(fresh.state.triangle.output, 15);
  fresh.tick(100);
  EXPECT_EQ(fresh.state.triangle.output, 15);

  Chip c = triangleAt(3, 0xFF);
  c.write(0x4017, 0x80);
  c.tick(21);
  EXPECT_EQ(c.state.triangle.output, 10);
  apuReset(c.state);
  EXPECT_EQ(c.state.triangle.step, 0);
  EXPECT_EQ(c.state.triangle.output, 15);
  EXPECT_EQ(c.state.triangle.period, 3);
  EXPECT_EQ(c.state.triangle.linearLoad, 127);
  EXPECT_EQ(c.state.triangle.linear, 127);
}

// ---- Noise ----

TEST(Noise, TheShiftRegisterStartsAtOneAndTakesTheExclusiveOrOfBitsZeroAndOne) {
  // NESdev wiki, "APU Noise", Operation: 1 at power-on; the feedback is bit 0 exclusive-ORed
  // with bit 1; the sequence is 32 767 steps long. Period index 0 clocks every 4 cycles,
  // the first on tick 0.
  constexpr std::array<std::uint16_t, 17> kShifts{
      0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200, 0x0100, 0x0080, 0x0040,
      0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x4001, 0x6000, 0x3000};
  Chip c;
  EXPECT_EQ(c.state.noise.shift, 0x0001);
  long done = 0;
  for (long n = 0; n < static_cast<long>(kShifts.size()); ++n) {
    throughTick(c, done, 4 * n);
    EXPECT_EQ(c.state.noise.shift, kShifts[static_cast<std::size_t>(n)]) << n;
    throughTick(c, done, 4 * n + 3);
    EXPECT_EQ(c.state.noise.shift, kShifts[static_cast<std::size_t>(n)]) << n;
  }

  Chip full;
  long clocks = 0;
  do {
    full.tick(4);
    ++clocks;
  } while (full.state.noise.shift != 0x0001 && clocks < 40000);
  EXPECT_EQ(clocks, 32767);
}

TEST(Noise, ThePeriodicModeTapsBitSix) {
  // NESdev wiki, "APU Noise", Operation: bit 6 in the periodic mode; the sequence is 93 or
  // 31 steps long.
  constexpr std::array<std::uint16_t, 11> kShifts{0x4000, 0x2000, 0x1000, 0x0800,
                                                  0x0400, 0x0200, 0x0100, 0x0080,
                                                  0x0040, 0x4020, 0x2010};
  Chip c;
  c.write(0x400E, 0x80);
  long done = 0;
  for (long n = 0; n < static_cast<long>(kShifts.size()); ++n) {
    throughTick(c, done, 4 * n);
    EXPECT_EQ(c.state.noise.shift, kShifts[static_cast<std::size_t>(n)]) << n;
  }

  Chip full;
  full.write(0x400E, 0x80);
  long clocks = 0;
  do {
    full.tick(4);
    ++clocks;
  } while (full.state.noise.shift != 0x0001 && clocks < 200);
  EXPECT_EQ(clocks, 93);
}

TEST(Noise, ThePeriodTableGivesTheCyclesBetweenShifts) {
  // NESdev wiki, "APU Noise", the NTSC column: the CPU cycles between two clocks.
  constexpr std::array<long, 16> kPeriods{4,   8,   16,  32,  64,  96,   128,  160,
                                          202, 254, 380, 508, 762, 1016, 2034, 4068};
  for (std::size_t i = 0; i < kPeriods.size(); ++i) {
    Chip c;
    c.write(0x400E, static_cast<std::uint8_t>(i));
    long done = 0;
    throughTick(c, done, 0);
    EXPECT_EQ(c.state.noise.shift, 0x4000) << i;
    throughTick(c, done, kPeriods[i] - 1);
    EXPECT_EQ(c.state.noise.shift, 0x4000) << i;
    throughTick(c, done, kPeriods[i]);
    EXPECT_EQ(c.state.noise.shift, 0x2000) << i;
  }
}

TEST(Noise, ItAnswersItsVolumeWhileBitZeroIsClearAndItsLengthCounterIsAboveZero) {
  // NESdev wiki, "APU Noise": the mixer receives the envelope's volume except when bit 0
  // of the shift register is set or the length counter is zero.
  Chip c(kPut);
  c.write(0x4015, 0x08);
  c.write(0x400C, 0x1A);
  c.write(0x400F, 0x08);
  long done = 0;
  throughTick(c, done, 0);
  EXPECT_EQ(c.state.length[3].counter, 254);
  EXPECT_EQ(c.state.noise.output, 0);
  throughTick(c, done, 1);
  EXPECT_EQ(c.state.noise.output, 10);
  throughTick(c, done, 56);
  EXPECT_EQ(c.state.noise.output, 10);
  throughTick(c, done, 57);
  EXPECT_EQ(c.state.noise.shift, 0x4001);
  EXPECT_EQ(c.state.noise.output, 0);
  throughTick(c, done, 61);
  EXPECT_EQ(c.state.noise.output, 10);
  c.write(0x4015, 0x00);
  c.tick();
  EXPECT_EQ(c.state.noise.output, 0);
}

TEST(Noise, ItsEnvelopeRestartsOnAWriteToItsFourthRegister) {
  // NESdev wiki, "APU Noise": writing $400F also restarts the envelope.
  Chip c;
  c.write(0x4015, 0x08);
  c.write(0x400C, 0x00);
  c.write(0x400F, 0x08);
  EXPECT_TRUE(c.state.noise.envelope.start);
  c.clockBoth();
  EXPECT_EQ(c.state.noise.envelope.decay, 15);
  c.clockBoth();
  EXPECT_EQ(c.state.noise.envelope.decay, 14);
  EXPECT_EQ(c.state.noise.shift & 0x0001, 0);
  EXPECT_EQ(c.state.noise.output, 14);
  c.write(0x400F, 0x08);
  c.clockBoth();
  EXPECT_EQ(c.state.noise.envelope.decay, 15);
}

// ---- Channels ----

TEST(Channels, ResetKeepsTheRegistersAndTheShiftRegisterAndSilencesThePulsesAndTheNoise) {
  // NESdev wiki, "CPU power up state", APU, the After Reset column; "APU Noise", Operation:
  // reset does not affect the shift register.
  Chip c = pulseOne(8, 0xBF);
  c.write(0x4015, 0x09);
  c.write(0x400C, 0x1A);
  c.write(0x400E, 0x85);
  c.write(0x400F, 0x08);
  c.tick(2);
  EXPECT_EQ(c.state.pulse[0].output, 15);
  EXPECT_EQ(c.state.noise.output, 10);
  const std::uint16_t shift = c.state.noise.shift;
  apuReset(c.state);
  EXPECT_EQ(c.state.pulse[0].output, 0);
  EXPECT_EQ(c.state.noise.output, 0);
  EXPECT_EQ(c.state.noise.shift, shift);
  EXPECT_TRUE(c.state.noise.mode);
  EXPECT_EQ(c.state.noise.periodIndex, 5);
  EXPECT_EQ(c.state.noise.envelope.volume, 10);
  const auto& pulse = c.state.pulse[0];
  EXPECT_EQ(pulse.period, 8);
  EXPECT_EQ(pulse.duty, 2);
  EXPECT_EQ(pulse.step, 1);
  EXPECT_EQ(pulse.timer, 8);
  EXPECT_TRUE(pulse.envelope.constant);
  EXPECT_EQ(pulse.envelope.volume, 15);
  EXPECT_TRUE(pulse.sweep.negate);
}

TEST(Channels, ADisabledChannelTakesItsRegistersAndLoadsNoLengthCounter) {
  // A fourth register's side effects wait on no enable bit; the length counter's load
  // does (NESdev wiki, "APU Length Counter", Clocking; blargg's apu_mixer shell loads the
  // linear counter with the triangle disabled).
  Chip c;
  c.write(0x4008, 0x04);
  c.write(0x400B, 0x09);
  c.write(0x4003, 0x0A);
  c.write(0x400F, 0x08);
  EXPECT_TRUE(c.state.triangle.linearReload);
  EXPECT_EQ(c.state.triangle.period, 0x100);
  EXPECT_EQ(c.state.pulse[0].period, 0x200);
  EXPECT_TRUE(c.state.pulse[0].envelope.start);
  EXPECT_TRUE(c.state.noise.envelope.start);
  c.clockBoth();
  EXPECT_EQ(c.state.triangle.linear, 4);
  EXPECT_EQ(c.state.pulse[0].envelope.decay, 15);
  for (const auto& length : c.state.length) EXPECT_EQ(length.counter, 0);
}

// ---- Machine ----

TEST(Machine, TheMachineGivesTheTickItsCyclesHalf) {
  // Cycles 0 to 6 are the reset sequence; the writes land in cycles 12, 18, 24, 28 and 32.
  // Under the defaults an even cycle is a get: the timer reloads with 8 in cycle 28, the
  // write in cycle 32 returns the sequencer to step 0, and the timer counts 7 at cycle 30
  // down to 0 at cycle 44 and reloads at 46.
  // LDA #$01; STA $4015; LDA #$BF; STA $4000; LDA #$08; STA $4001; STA $4002; STA $4003;
  // JMP $8015
  const Bytes image = imageWith({0xA9, 0x01, 0x8D, 0x15, 0x40, 0xA9, 0xBF, 0x8D,
                                 0x00, 0x40, 0xA9, 0x08, 0x8D, 0x01, 0x40, 0x8D,
                                 0x02, 0x40, 0x8D, 0x03, 0x40, 0x4C, 0x15, 0x80});
  Nes m(NesConfig{.rom = image});
  stepCycles(m, 46);
  EXPECT_EQ(m.state().apu.pulse[0].step, 0);
  EXPECT_EQ(m.state().apu.pulse[0].output, 0);
  EXPECT_EQ(m.state().apu.pulse[0].timer, 0);
  stepCycles(m, 1);  // 47
  EXPECT_EQ(m.state().apu.pulse[0].step, 1);
  EXPECT_EQ(m.state().apu.pulse[0].output, 15);
  stepCycles(m, 118 - 47);
  EXPECT_EQ(m.state().apu.pulse[0].output, 15);
  stepCycles(m, 1);  // 119
  EXPECT_EQ(m.state().apu.pulse[0].output, 0);
  EXPECT_EQ(m.state().apu.pulse[0].step, 5);

  // With the odd cycles the gets, the reload with 8 comes at cycle 29 and the next at 47.
  Nes odd(NesConfig{.rom = image, .apuPhase = 1});
  stepCycles(odd, 47);
  EXPECT_EQ(odd.state().apu.pulse[0].step, 0);
  EXPECT_EQ(odd.state().apu.pulse[0].output, 0);
  stepCycles(odd, 1);  // 48
  EXPECT_EQ(odd.state().apu.pulse[0].step, 1);
  EXPECT_EQ(odd.state().apu.pulse[0].output, 15);
}

TEST(Machine, ASnapshotCarriesTheChannels) {
  // Every channel enabled at a period of $1FD, the envelopes at a parameter of 5, pulse 1
  // sweeping down and pulse 2 up with a divider period of 1 and a shift of 2, the linear
  // counter loading 69. By step 40 000 the sequence has made five quarter-frame clocks and
  // two half-frame clocks; by 45 000, the half-frame clock at 44 743.
  // LDA #$0F; STA $4015; LDA #$45; STA $4000; STA $4004; STA $400C; STA $4008; LDA #$9A;
  // STA $4001; LDA #$92; STA $4005; LDA #$FD; STA $4002; STA $4006; STA $400A; LDA #$85;
  // STA $400E; LDA #$09; STA $4003; STA $4007; STA $400B; STA $400F; JMP $803B
  const Bytes image = imageWith(
      {0xA9, 0x0F, 0x8D, 0x15, 0x40, 0xA9, 0x45, 0x8D, 0x00, 0x40, 0x8D, 0x04, 0x40, 0x8D, 0x0C,
       0x40, 0x8D, 0x08, 0x40, 0xA9, 0x9A, 0x8D, 0x01, 0x40, 0xA9, 0x92, 0x8D, 0x05, 0x40, 0xA9,
       0xFD, 0x8D, 0x02, 0x40, 0x8D, 0x06, 0x40, 0x8D, 0x0A, 0x40, 0xA9, 0x85, 0x8D, 0x0E, 0x40,
       0xA9, 0x09, 0x8D, 0x03, 0x40, 0x8D, 0x07, 0x40, 0x8D, 0x0B, 0x40, 0x8D, 0x0F, 0x40, 0x4C,
       0x3B, 0x80});
  Nes a(NesConfig{.rom = image});
  Nes b(NesConfig{.rom = image});
  stepCycles(a, 40000);
  const auto& apu = a.state().apu;
  EXPECT_EQ(apu.pulse[0].period, 381);
  EXPECT_EQ(apu.pulse[1].period, 636);
  EXPECT_EQ(apu.pulse[0].envelope.decay, 15);
  EXPECT_EQ(apu.pulse[0].envelope.divider, 1);
  EXPECT_EQ(apu.noise.envelope.divider, 1);
  EXPECT_EQ(apu.triangle.linear, 65);
  for (const auto& length : apu.length) EXPECT_EQ(length.counter, 252);
  b.restore(a.state());
  EXPECT_TRUE(a.state().apu == b.state().apu);
  stepCycles(a, 5000);
  stepCycles(b, 5000);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.state().apu.pulse[0].period, 285);
  EXPECT_EQ(a.state().apu.pulse[1].period, 795);
  EXPECT_EQ(a.state().apu.triangle.linear, 64);
  for (const auto& length : a.state().apu.length) EXPECT_EQ(length.counter, 251);
}

}  // namespace
