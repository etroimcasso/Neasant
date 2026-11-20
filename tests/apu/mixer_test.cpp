#include "neasant/apu/apu.h"
#include "neasant/nes/clock.h"
#include "neasant/nes/nes.h"
#include "rom_harness.h"
#include "authored_image.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifndef NEASANT_NES_TEST_ROMS
#define NEASANT_NES_TEST_ROMS ""
#endif

// The APU's output: the mixer driven through apu.h with each channel's level set by hand,
// every expected level the mixer page's formula scaled by 32 767 and rounded; the
// decimator driven a cycle at a time, every expected cycle derived from the CPU clock and
// the rate and every mean from the output stage's answers over the cycles (apuFilter; the
// filter's test derives those); the machine with its frames drained both ways; and
// blargg's mixer programs booted on the machine and measured in the frames they make. A
// cycle below is a call counted from a chip's first, or a machine cycle counted from
// power-on, whose first seven cycles are the reset sequence; a picture is 357 368 master
// cycles. The NEASANT_NES_TEST_ROMS option, the skips and the require variable are as in
// the frame counter's test.

namespace {

using neasant::ApuDecimator;
using neasant::ApuInputs;
using neasant::ApuState;
using neasant::apuDecimate;
using neasant::apuMix;
using neasant::apuReset;
using neasant::consoleClock;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::Region;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::loadRom;
using neasant::nes_tests::placeProgram;
using neasant::nes_tests::Report;
using neasant::nes_tests::RomResult;
using neasant::nes_tests::runUntilResult;

using Bytes = std::vector<std::uint8_t>;
using Frames = std::vector<std::int16_t>;

// ---- The chip ----

// A chip with every channel's level set by hand and its output stage set to rest, both
// filters at 0. The triangle, which stands at 15 at power-on, is at the level the case
// names.
ApuState levels(std::uint8_t pulse1, std::uint8_t pulse2, std::uint8_t triangle,
                std::uint8_t noise, std::uint8_t dmc) {
  ApuState state{};
  state.filter = neasant::ApuFilter{.highPass = 0, .lowPass = 0};
  state.pulse[0].output = pulse1;
  state.pulse[1].output = pulse2;
  state.triangle.output = triangle;
  state.noise.output = noise;
  state.dmc.output = dmc;
  return state;
}

constexpr ApuInputs kAt48000{.audioRate = 48000, .expansion = 0};

// ---- The machine ----

// One picture with rendering off, in master cycles.
constexpr std::uint64_t kFrame = consoleClock(Region::Ntsc).masterCyclesPerFrame;

// A 16 KB NROM image with 8 KB of CHR ROM, `program` at $8000 and every vector there.
Bytes imageWith(const Bytes& program) {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  placeProgram(image, 0, program);
  return image;
}

void stepCycles(Nes& machine, long cycles) {
  for (long i = 0; i < cycles; ++i) machine.step();
}

// The DMC's level loaded with 127: the write lands in machine cycle 12.
// LDA #$7F; STA $4011; JMP $8005
const Bytes kLevelProgram{0xA9, 0x7F, 0x8D, 0x11, 0x40, 0x4C, 0x05, 0x80};

// Pulse 1 at the duty whose first step is high, constant volume 15 and a period of $0FF:
// the write to $4003 lands in machine cycle 28, loads the length counter at that cycle's
// tick and returns the sequencer to its first step, so the channel's level is 15 from that
// tick on.
// LDA #$01; STA $4015; LDA #$FF; STA $4000; STA $4002; LDA #$08; STA $4003; JMP $8012
const Bytes kPulseProgram{0xA9, 0x01, 0x8D, 0x15, 0x40, 0xA9, 0xFF, 0x8D, 0x00, 0x40, 0x8D,
                          0x02, 0x40, 0xA9, 0x08, 0x8D, 0x03, 0x40, 0x4C, 0x12, 0x80};

// The mixer programs' beep (vol_shell.inc, short_tone): pulse 1 at a 50% duty, constant
// volume 15 and a period of $06F, 1792 CPU cycles a wave.
// LDA #$01; STA $4015; LDA #$BF; STA $4000; LDA #$7F; STA $4001; LDA #$6F; STA $4002;
// LDA #$00; STA $4003; JMP $8019
const Bytes kBeepProgram{0xA9, 0x01, 0x8D, 0x15, 0x40, 0xA9, 0xBF, 0x8D, 0x00, 0x40,
                         0xA9, 0x7F, 0x8D, 0x01, 0x40, 0xA9, 0x6F, 0x8D, 0x02, 0x40,
                         0xA9, 0x00, 0x8D, 0x03, 0x40, 0x4C, 0x19, 0x80};

// ---- The measure ----

// The beep's period and every test tone's, in CPU cycles: pulse 1 at a period of $06F
// steps its eight-step waveform every 224 cycles (NESdev wiki, "APU Pulse"), dmc.s,
// square.s and noise.s toggle their channel against the DMC every 896, and triangle.s
// runs the triangle's 32 steps at 56 cycles each.
constexpr std::int64_t kToneCycles = 1792;

// What the beep adds to the output while its waveform is high: pulse 1 at volume 15
// (NESdev wiki, "APU Mixer").
constexpr std::int64_t kBeepStep = 4895;

// What one picture's frames measure, in whole numbers. `energy` is the sum of the squares
// of each frame's distance from the picture's mean, which is left out so that a level the
// output stage is still taking away adds nothing.
// `tone` is the step of a square wave of the beep's period in the picture: each frame's
// distance from the mean is added or subtracted by the half of the period its last CPU
// cycle falls in, and again a quarter period on, and twice the two sums' magnitudes over
// the count is the step, wherever the wave's edges fall.
struct FrameMeasure {
  std::int64_t count = 0;
  std::int64_t energy = 0;
  std::int64_t tone = 0;
};

// `first` is the index of the picture's first frame counted from power-on: frame i ends at
// CPU cycle (39375 (i + 1) + 1055) / 1056, the fraction dropped (apuDecimate at
// 48 000 Hz).
FrameMeasure measure(const Frames& frames, std::int64_t first) {
  FrameMeasure result;
  result.count = static_cast<std::int64_t>(frames.size());
  if (result.count == 0) return result;
  std::int64_t sum = 0;
  for (const std::int16_t frame : frames) sum += frame;
  const std::int64_t mean = sum / result.count;
  std::int64_t aligned = 0;
  std::int64_t quarterOn = 0;
  for (std::size_t k = 0; k < frames.size(); ++k) {
    const std::int64_t distance = frames[k] - mean;
    result.energy += distance * distance;
    const std::int64_t index = first + static_cast<std::int64_t>(k);
    const std::int64_t cycle = (39375 * (index + 1) + 1055) / 1056;
    aligned += cycle % kToneCycles < kToneCycles / 2 ? distance : -distance;
    quarterOn +=
        (cycle + kToneCycles / 4) % kToneCycles < kToneCycles / 2 ? distance : -distance;
  }
  result.tone = 2 * (std::llabs(aligned) + std::llabs(quarterOn)) / result.count;
  return result;
}

// The largest whole number whose square is at most `value`.
std::int64_t wholeRoot(std::int64_t value) {
  auto root = static_cast<std::int64_t>(std::sqrt(static_cast<double>(value)));
  while (root * root > value) --root;
  while ((root + 1) * (root + 1) <= value) ++root;
  return root;
}

// A run of pictures, by the index of its first and its last, counted from the first
// picture a machine runs.
struct Run {
  std::size_t first = 0;
  std::size_t last = 0;
  bool operator==(const Run&) const = default;
};

// The runs of pictures that hold a beep: those whose tone is at least half the beep's
// step.
std::vector<Run> beepRuns(const std::vector<FrameMeasure>& pictures) {
  std::vector<Run> runs;
  bool inRun = false;
  for (std::size_t p = 0; p < pictures.size(); ++p) {
    const bool beep = 2 * pictures[p].tone >= kBeepStep;
    if (beep && !inRun) runs.push_back(Run{.first = p, .last = p});
    if (beep) runs.back().last = p;
    inRun = beep;
  }
  return runs;
}

struct Totals {
  std::int64_t count = 0;
  std::int64_t energy = 0;
  std::int64_t tone = 0;
  std::int64_t pictures = 0;

  void add(const FrameMeasure& picture) {
    count += picture.count;
    energy += picture.energy;
    tone += picture.tone;
    ++pictures;
  }
};

// ---- The programs ----

// The guard, in pictures: 30 seconds at the console's rate.
constexpr std::size_t kGuardPictures = 1800;

bool required(const char* variable) {
  const char* raw = std::getenv(variable);
  return raw != nullptr && std::string(raw) == "1";
}

// Loads `relative` from the nes-test-roms clone into `image`, or leaves it empty after
// skipping or failing the case: the case skips naming NEASANT_NES_TEST_ROMS when it is
// unset and the file when it is absent, and either is a failure instead when
// NEASANT_REQUIRE_NES_TEST_ROMS is 1.
void loadOrSkip(const std::string& relative, std::optional<Bytes>& image) {
  image.reset();
  const std::string dir = NEASANT_NES_TEST_ROMS;
  if (dir.empty()) {
    if (required("NEASANT_REQUIRE_NES_TEST_ROMS")) {
      ADD_FAILURE() << relative
                    << ": NEASANT_NES_TEST_ROMS is unset and NEASANT_REQUIRE_NES_TEST_ROMS is 1";
      return;
    }
    GTEST_SKIP() << relative << ": set NEASANT_NES_TEST_ROMS to run it";
  }
  const std::string path = dir + "/" + relative;
  image = loadRom(path);
  if (!image) {
    if (required("NEASANT_REQUIRE_NES_TEST_ROMS")) {
      ADD_FAILURE() << path << " is absent and NEASANT_REQUIRE_NES_TEST_ROMS is 1";
      return;
    }
    GTEST_SKIP() << path << " is absent";
  }
}

// Where a mixer program's two beeps fall and the picture its report comes in, each counted
// from the first picture the machine runs, and the level and the tone between the beeps in
// thousandths of the beeps' own.
struct Heard {
  Run firstBeep;
  Run secondBeep;
  std::size_t reportedAt = 0;
  std::int64_t levelPerMille = 0;
  std::int64_t tonePerMille = 0;
};

// Boots the ROM on a machine built with the defaults and runs it a picture at a time to
// its report, draining and measuring each picture (runUntilResult with a guard of one
// picture). Requires the report and its pass, the picture it comes in, exactly two beeps
// and where `expected` has them, and the level and the tone between them in thousandths
// of the beeps' own. The pictures inside a beep leave out its first and last, which the
// beep fills in part; the pictures between leave out the one after the first beep and the
// one before the second. The level is the whole root of 1 000 000 times the energy over
// the count between, over the energy over the count inside; the tone is 1000 times the
// tone over the pictures between, over the tone over the pictures inside; every division
// whole.
void expectHeard(const std::string& relative, const Heard& expected) {
  std::optional<Bytes> image;
  loadOrSkip(relative, image);
  if (!image) return;
  Nes machine(NesConfig{.rom = *image});
  std::vector<FrameMeasure> pictures;
  std::int64_t first = 0;
  RomResult result;
  while (pictures.size() < kGuardPictures) {
    result = runUntilResult(machine, Report::Memory, 1);
    const Frames frames = machine.takeFrames();
    pictures.push_back(measure(frames, first));
    first += static_cast<std::int64_t>(frames.size());
    if (result.reported) break;
  }
  ASSERT_TRUE(result.reported) << relative << " did not report within " << kGuardPictures
                               << " pictures";
  EXPECT_TRUE(result.passed) << relative << " reports a failure\n--- text ---\n"
                             << result.text << "\n------------";
  EXPECT_EQ(pictures.size() - 1, expected.reportedAt) << relative << ": the report";
  const std::vector<Run> runs = beepRuns(pictures);
  ASSERT_EQ(runs.size(), 2u) << relative;
  EXPECT_EQ(runs[0], expected.firstBeep) << relative << ": the first beep";
  EXPECT_EQ(runs[1], expected.secondBeep) << relative << ": the second beep";
  Totals during;
  Totals between;
  for (const Run& run : runs) {
    for (std::size_t p = run.first + 1; p < run.last; ++p) during.add(pictures[p]);
  }
  for (std::size_t p = runs[0].last + 2; p + 2 <= runs[1].first; ++p) {
    between.add(pictures[p]);
  }
  ASSERT_GT(during.pictures, 0) << relative;
  ASSERT_GT(between.pictures, 0) << relative;
  const std::int64_t level = wholeRoot(1'000'000 * (between.energy / between.count) /
                                       (during.energy / during.count));
  const std::int64_t tone =
      1000 * (between.tone / between.pictures) / (during.tone / during.pictures);
  EXPECT_EQ(level, expected.levelPerMille) << relative << ": the level between the beeps";
  EXPECT_EQ(tone, expected.tonePerMille) << relative << ": the tone between the beeps";
}

// ---- ApuMix ----

// NESdev wiki, "APU Mixer": a group whose channels are all 0 is 0, and the two curves run
// from 0.0 to 1.0, every channel at its top 8470 from the pulses and 24 297 from the rest.
// At power-on the triangle stands at its first step, level 15, and the rest at 0.
TEST(ApuMix, SilenceIsZeroAndEveryChannelAtItsTopIsFullScale) {
  EXPECT_EQ(apuMix(levels(0, 0, 0, 0, 0), 0), 0);
  EXPECT_EQ(apuMix(levels(15, 15, 15, 15, 127), 0), 32767);
  EXPECT_EQ(apuMix(ApuState{}, 0), 8074);
}

// NESdev wiki, "APU Mixer", pulse_out: 95.88 / (8128 / (pulse1 + pulse2) + 100), scaled by
// 32 767 and rounded, depends on the two levels' sum alone.
TEST(ApuMix, TheTwoPulsesGoThroughOneCurve) {
  constexpr std::array<std::int16_t, 31> kByTotal{
      0,    382,  754,  1118, 1474, 1821, 2160, 2491, 2815, 3132, 3442,
      3745, 4042, 4332, 4616, 4895, 5167, 5434, 5696, 5953, 6204, 6450,
      6692, 6929, 7162, 7390, 7614, 7834, 8050, 8262, 8470};
  for (std::uint8_t total = 0; total <= 30; ++total) {
    const auto low = static_cast<std::uint8_t>(total < 15 ? total : 15);
    const auto rest = static_cast<std::uint8_t>(total - low);
    EXPECT_EQ(apuMix(levels(low, rest, 0, 0, 0), 0), kByTotal[total]) << "total " << +total;
    EXPECT_EQ(apuMix(levels(rest, low, 0, 0, 0), 0), kByTotal[total]) << "total " << +total;
  }
  EXPECT_EQ(apuMix(levels(7, 8, 0, 0, 0), 0), 4895);
}

// NESdev wiki, "APU Mixer", tnd_out: one channel alone is 159.79 / (divisor / level + 100),
// the triangle's divisor 8227, the noise's 12241 and the DMC's 22638.
TEST(ApuMix, TheTriangleTheNoiseAndTheDmcEachAlone) {
  EXPECT_EQ(apuMix(levels(0, 0, 1, 0, 0), 0), 629);
  EXPECT_EQ(apuMix(levels(0, 0, 8, 0, 0), 0), 4640);
  EXPECT_EQ(apuMix(levels(0, 0, 15, 0, 0), 0), 8074);
  EXPECT_EQ(apuMix(levels(0, 0, 0, 1, 0), 0), 424);
  EXPECT_EQ(apuMix(levels(0, 0, 0, 8, 0), 0), 3212);
  EXPECT_EQ(apuMix(levels(0, 0, 0, 15, 0), 0), 5716);
  EXPECT_EQ(apuMix(levels(0, 0, 0, 0, 1), 0), 230);
  EXPECT_EQ(apuMix(levels(0, 0, 0, 0, 64), 0), 11540);
  EXPECT_EQ(apuMix(levels(0, 0, 0, 0, 127), 0), 18817);
}

// NESdev wiki, "APU Mixer", tnd_out: the three channels sum inside one curve, so together
// they make less than their levels alone added.
TEST(ApuMix, TheThreeShareOneCurve) {
  EXPECT_EQ(apuMix(levels(0, 0, 15, 15, 0), 0), 12233);
  EXPECT_LT(apuMix(levels(0, 0, 15, 15, 0), 0), 8074 + 5716);
  EXPECT_EQ(apuMix(levels(0, 0, 7, 3, 40), 0), 11653);
  EXPECT_EQ(apuMix(levels(0, 0, 15, 15, 127), 0), 24297);
}

// NESdev wiki, "APU Mixer": games use the DMC's level as a crude volume control for the
// triangle and the noise.
TEST(ApuMix, TheDmcsLevelSetsHowFarTheTriangleAndTheNoiseStep) {
  EXPECT_EQ(apuMix(levels(0, 0, 15, 0, 0), 0) - apuMix(levels(0, 0, 0, 0, 0), 0), 8074);
  EXPECT_EQ(apuMix(levels(0, 0, 15, 0, 127), 0), 22325);
  EXPECT_EQ(apuMix(levels(0, 0, 15, 0, 127), 0) - apuMix(levels(0, 0, 0, 0, 127), 0), 3508);
  EXPECT_EQ(apuMix(levels(0, 0, 0, 15, 0), 0) - apuMix(levels(0, 0, 0, 0, 0), 0), 5716);
  EXPECT_EQ(apuMix(levels(0, 0, 0, 15, 127), 0), 21258);
  EXPECT_EQ(apuMix(levels(0, 0, 0, 15, 127), 0) - apuMix(levels(0, 0, 0, 0, 127), 0), 2441);
}

// NESdev wiki, "APU Mixer": output = pulse_out + tnd_out. dmc.s pairs a DMC level of 82
// with 127 for a pulse at volume 15, and the curve's rise between them is the pulse's
// level.
TEST(ApuMix, TheTwoGroupsAdd) {
  EXPECT_EQ(apuMix(levels(15, 0, 0, 0, 127), 0), 4895 + 18817);
  EXPECT_EQ(apuMix(levels(0, 0, 0, 0, 82), 0), 13922);
  EXPECT_EQ(apuMix(levels(15, 0, 0, 0, 0), 0), 4895);
  EXPECT_EQ(apuMix(levels(0, 0, 0, 0, 127), 0) - apuMix(levels(0, 0, 0, 0, 82), 0),
            apuMix(levels(15, 0, 0, 0, 0), 0));
}

// The cartridge's level is added to the chip's, and the sum is held at 32 767, the most a
// sample holds; the chip's own level is 0 or above, so the sum reaches -32 768 only from
// the cartridge.
TEST(ApuMix, TheExpansionLevelAddsAndTheSumIsHeldAtTheTop) {
  const ApuState silence = levels(0, 0, 0, 0, 0);
  const ApuState full = levels(15, 15, 15, 15, 127);
  EXPECT_EQ(apuMix(silence, 1000), 1000);
  EXPECT_EQ(apuMix(silence, -1000), -1000);
  EXPECT_EQ(apuMix(levels(15, 0, 0, 0, 0), -5000), -105);
  EXPECT_EQ(apuMix(full, 1), 32767);
  EXPECT_EQ(apuMix(full, -1), 32766);
  EXPECT_EQ(apuMix(silence, -32768), -32768);
}

// ---- ApuDecimate ----

// NESdev wiki, "CPU", Frequencies: a CPU cycle is 132 / 236 250 000 of a second, so at
// 48 000 Hz a sample is 39375 / 1056 cycles and the first N cycles make the whole part of
// 1056 N / 39375 samples; after 39 375 cycles the count is whole again.
TEST(ApuDecimate, AtFortyEightThousandASampleSpans37Or38Cycles) {
  ApuState state = levels(0, 0, 0, 0, 0);
  std::vector<long> answered;
  for (long call = 1; call <= 39375; ++call) {
    if (apuDecimate(state, kAt48000)) answered.push_back(call);
  }
  ASSERT_EQ(answered.size(), 1056u);
  EXPECT_EQ(std::vector<long>(answered.begin(), answered.begin() + 10),
            (std::vector<long>{38, 75, 112, 150, 187, 224, 262, 299, 336, 373}));
  EXPECT_EQ(state.decimator, ApuDecimator{});
}

// A sample is the mean of the output stage's answers over the cycles it spans, the
// fraction dropped. The DMC at 127 is 18 817 at the mixer (NESdev wiki, "APU Mixer");
// from rest the stage answers one cycle at it and 37 at 0 with a mean of 415, the next 37
// at it with 10 372 as its low-pass rises, and 19 more at it then 18 at 0 with 14 008.
TEST(ApuDecimate, ASampleIsTheMeanOfTheLevelsItSpans) {
  ApuState state = levels(0, 0, 0, 0, 127);
  EXPECT_FALSE(apuDecimate(state, kAt48000)) << "call 1";
  state.dmc.output = 0;
  for (long call = 2; call <= 37; ++call) {
    EXPECT_FALSE(apuDecimate(state, kAt48000)) << "call " << call;
  }
  EXPECT_EQ(apuDecimate(state, kAt48000), std::optional<std::int16_t>(415)) << "call 38";
  state.dmc.output = 127;
  for (long call = 39; call <= 74; ++call) {
    EXPECT_FALSE(apuDecimate(state, kAt48000)) << "call " << call;
  }
  EXPECT_EQ(apuDecimate(state, kAt48000), std::optional<std::int16_t>(10372)) << "call 75";
  for (long call = 76; call <= 94; ++call) {
    EXPECT_FALSE(apuDecimate(state, kAt48000)) << "call " << call;
  }
  state.dmc.output = 0;
  for (long call = 95; call <= 111; ++call) {
    EXPECT_FALSE(apuDecimate(state, kAt48000)) << "call " << call;
  }
  EXPECT_EQ(apuDecimate(state, kAt48000), std::optional<std::int16_t>(14008)) << "call 112";
}

// At the CPU clock's 1 789 772 a cycle is 236 249 904 units of the 236 250 000 a sample
// takes (NESdev wiki, "CPU", Frequencies), so the first sample spans two cycles and every
// later one a single cycle, the time left over shrinking by 96 units a sample. The stage
// answers 902 and 859 to the DMC's 18 817 then 0, a mean of 880, and 1370 to a level of
// 11 540 after them.
TEST(ApuDecimate, AtTheHighestRateASampleSpansTwoCyclesThenOne) {
  constexpr ApuInputs kHighest{.audioRate = 1789772, .expansion = 0};
  ApuState state = levels(0, 0, 0, 0, 127);
  EXPECT_FALSE(apuDecimate(state, kHighest));
  EXPECT_EQ(state.decimator.elapsed, 236249904u);
  state.dmc.output = 0;
  EXPECT_EQ(apuDecimate(state, kHighest), std::optional<std::int16_t>(880));
  EXPECT_EQ(state.decimator.elapsed, 236249808u);
  state.dmc.output = 64;
  EXPECT_EQ(apuDecimate(state, kHighest), std::optional<std::int16_t>(1370));
}

// A rate of 0 makes no sample and leaves the state as it is.
TEST(ApuDecimate, ARateOfZeroAnswersNothingAndLeavesTheState) {
  ApuState state = levels(15, 15, 15, 15, 127);
  const ApuState before = state;
  for (int call = 1; call <= 100; ++call) {
    EXPECT_FALSE(apuDecimate(state, ApuInputs{})) << "call " << call;
  }
  EXPECT_EQ(state, before);
}

// The cartridge's level is part of every cycle's level, and so of what the output stage
// is given: its answers to -700 held from rest have a mean of -389 over the first
// sample's 38 cycles.
TEST(ApuDecimate, TheExpansionLevelIsMixedIntoEverySample) {
  ApuState state = levels(0, 0, 0, 0, 0);
  constexpr ApuInputs kWithExpansion{.audioRate = 48000, .expansion = -700};
  for (int call = 1; call <= 37; ++call) {
    EXPECT_FALSE(apuDecimate(state, kWithExpansion)) << "call " << call;
  }
  EXPECT_EQ(apuDecimate(state, kWithExpansion), std::optional<std::int16_t>(-389));
}

// The decimator is the core's sampling of the chip's output and no register of the chip,
// so the reset line leaves it where it stands: 20 cycles of 6 336 000 units each at
// 48 000 Hz, the output stage's answers to the DMC's 18 817 summed over them.
TEST(ApuDecimate, ResetKeepsTheDecimator) {
  ApuState state = levels(0, 0, 0, 0, 127);
  for (int call = 1; call <= 20; ++call) {
    EXPECT_FALSE(apuDecimate(state, kAt48000)) << "call " << call;
  }
  EXPECT_EQ(state.decimator.elapsed, 126720000u);
  EXPECT_EQ(state.decimator.sum, 142232);
  EXPECT_EQ(state.decimator.count, 20u);
  const ApuDecimator before = state.decimator;
  apuReset(state);
  EXPECT_EQ(state.decimator, before);
}

// ---- MixerMachine ----

// The mixer's level is 8074 for machine cycles 0 to 11, the triangle's level 15 alone at
// power-on, and 22 325 from the write in cycle 12, with the DMC's level 127 (NESdev wiki,
// "APU DMC": $4011 loads the counter directly; "APU Mixer"). The output stage is settled
// on the 8074 at power-on, so those twelve cycles answer 0 and the write is a step of
// 14 251: the first frame, cycles 0 to 37, is 4366 as the low-pass rises on it, and the
// second is 12 342.
TEST(MixerMachine, AWriteToTheDmcsLevelPassesAsAStep) {
  const Bytes image = imageWith(kLevelProgram);
  Nes machine(NesConfig{.rom = image});
  stepCycles(machine, 37);
  EXPECT_TRUE(machine.takeFrames().empty());
  machine.step();
  EXPECT_EQ(machine.takeFrames(), (Frames{4366}));
  stepCycles(machine, 37);
  EXPECT_EQ(machine.takeFrames(), (Frames{12342}));
  EXPECT_TRUE(machine.takeFrames().empty());
}

// The output takes the level the cycle's tick leaves: the $4003 write in cycle 28 loads
// the length counter at that cycle's tick, so pulse 1's level is 15 from cycle 28 on and
// the first frame holds 28 cycles at 8074, which the output stage answers with 0, and
// ten at 12 969, a step of 4895: the frame is 294, and the second, 37 cycles at 12 969, is
// 3526. The pulse's timer, reloaded
// with 255 at cycle 22's tick, next steps the sequencer at cycle 534 (NESdev wiki, "APU
// Pulse"; "APU Length Counter").
TEST(MixerMachine, TheOutputTakesTheLevelTheCyclesTickLeaves) {
  const Bytes image = imageWith(kPulseProgram);
  Nes machine(NesConfig{.rom = image});
  stepCycles(machine, 28);
  EXPECT_EQ(machine.state().apu.pulse[0].output, 0);
  machine.step();
  EXPECT_EQ(machine.state().apu.pulse[0].output, 15);
  stepCycles(machine, 9);
  EXPECT_EQ(machine.takeFrames(), (Frames{294}));
  stepCycles(machine, 37);
  EXPECT_EQ(machine.takeFrames(), (Frames{3526}));
}

// A picture's 357 368 master cycles are 29 780 and two thirds CPU cycles (NESdev wiki,
// "CPU", Frequencies), the overshoot carried from one run to the next, so the pictures make
// 798 or 799 frames at 48 000 Hz as the whole part of 1056 N / 39375 steps.
TEST(MixerMachine, APictureIs798Or799FramesAtFortyEightThousand) {
  const Bytes image = imageWith(kLevelProgram);
  Nes machine(NesConfig{.rom = image});
  std::vector<std::size_t> sizes;
  for (int picture = 0; picture < 9; ++picture) {
    machine.run(kFrame);
    sizes.push_back(machine.takeFrames().size());
  }
  EXPECT_EQ(sizes, (std::vector<std::size_t>{798, 799, 799, 798, 799, 799, 798, 799, 799}));
}

// A machine built with an audioRate of 0 makes no frame and leaves the decimator at rest;
// the span form then writes nothing.
TEST(MixerMachine, ARateOfZeroQueuesNothing) {
  const Bytes image = imageWith(kBeepProgram);
  Nes machine(NesConfig{.rom = image, .audioRate = 0});
  machine.run(kFrame);
  EXPECT_TRUE(machine.takeFrames().empty());
  EXPECT_EQ(machine.state().apu.decimator, ApuDecimator{});
  std::array<std::int16_t, 4> storage{1, 2, 3, 4};
  EXPECT_EQ(machine.takeFrames(storage), 0u);
  EXPECT_EQ(storage, (std::array<std::int16_t, 4>{1, 2, 3, 4}));
}

// The frames depend on the cycles run and not on how the runs are cut or when they are
// drained, and a machine restored from a snapshot makes the frames that follow it, the
// decimator being in the state.
TEST(MixerMachine, TwoMachinesRunDifferentlyDrainEqualFrames) {
  const Bytes image = imageWith(kBeepProgram);
  Nes a(NesConfig{.rom = image});
  Frames joined;
  std::optional<NesState> afterFirst;
  for (int picture = 0; picture < 3; ++picture) {
    a.run(kFrame);
    const Frames frames = a.takeFrames();
    joined.insert(joined.end(), frames.begin(), frames.end());
    if (picture == 0) afterFirst = a.state();
  }
  ASSERT_EQ(joined.size(), 2396u);

  Nes b(NesConfig{.rom = image});
  stepCycles(b, 1000);
  b.run(3 * kFrame - 12000);
  const Frames once = b.takeFrames();
  EXPECT_EQ(once, joined);
  std::int16_t least = joined.front();
  std::int16_t greatest = joined.front();
  for (const std::int16_t frame : joined) {
    least = frame < least ? frame : least;
    greatest = frame > greatest ? frame : greatest;
  }
  // The greatest frame is the tenth, the beep's first high half before the output stage
  // has centred the wave; the least is a low half once it has.
  EXPECT_EQ(least, -2689);
  EXPECT_EQ(greatest, 4731);

  Nes c(NesConfig{.rom = image});
  c.restore(*afterFirst);
  c.run(2 * kFrame);
  EXPECT_EQ(c.takeFrames(), Frames(joined.begin() + 798, joined.end()));
}

// The span form writes as many frames as the caller's storage holds, oldest first, keeps
// the rest queued in order, and leaves the storage past what it wrote as it was.
TEST(MixerMachine, TheSpanFormFillsTheCallersStorageAndKeepsTheRest) {
  const Bytes image = imageWith(kBeepProgram);
  Nes machine(NesConfig{.rom = image});
  Nes twin(NesConfig{.rom = image});
  machine.run(kFrame);
  twin.run(kFrame);
  const Frames all = twin.takeFrames();
  ASSERT_EQ(all.size(), 798u);

  EXPECT_EQ(machine.takeFrames(std::span<std::int16_t>{}), 0u);
  Frames first(300);
  EXPECT_EQ(machine.takeFrames(first), 300u);
  EXPECT_EQ(first, Frames(all.begin(), all.begin() + 300));
  Frames second(300);
  EXPECT_EQ(machine.takeFrames(second), 300u);
  EXPECT_EQ(second, Frames(all.begin() + 300, all.begin() + 600));
  Frames rest(1000, -1);
  EXPECT_EQ(machine.takeFrames(rest), 198u);
  EXPECT_EQ(Frames(rest.begin(), rest.begin() + 198), Frames(all.begin() + 600, all.end()));
  EXPECT_EQ(rest[198], -1);
  EXPECT_EQ(machine.takeFrames(rest), 0u);
}

// Frames are output, outside the state: a restore and a reset discard those not yet
// drained, and the reset keeps the decimator's place, so the next picture makes 799.
TEST(MixerMachine, RestoreAndResetDiscardTheFramesPending) {
  const Bytes image = imageWith(kBeepProgram);
  Nes machine(NesConfig{.rom = image});
  machine.run(kFrame);
  const NesState snapshot = machine.state();
  machine.restore(snapshot);
  EXPECT_TRUE(machine.takeFrames().empty());
  machine.run(kFrame);
  const ApuDecimator before = machine.state().apu.decimator;
  machine.reset();
  EXPECT_TRUE(machine.takeFrames().empty());
  EXPECT_EQ(machine.state().apu.decimator, before);
  machine.run(kFrame);
  EXPECT_EQ(machine.takeFrames().size(), 799u);
}

// A machine is copied whole, the frames not yet drained with it.
TEST(MixerMachine, ACopiedMachineCarriesTheFramesPending) {
  const Bytes image = imageWith(kBeepProgram);
  Nes machine(NesConfig{.rom = image});
  machine.run(kFrame);
  Nes copy = machine;
  const Frames fromCopy = copy.takeFrames();
  EXPECT_EQ(fromCopy.size(), 798u);
  EXPECT_EQ(fromCopy, machine.takeFrames());
}

// The measure on a beep alone: a square wave of step 4895 which the output stage centres
// and lets sag between its edges, less the samples that span an edge, two in every 48 or
// so, which blend its two levels.
TEST(MixerMachine, TheMeasureReadsAnAuthoredBeepsStepAndLevel) {
  const Bytes image = imageWith(kBeepProgram);
  Nes machine(NesConfig{.rom = image});
  Totals totals;
  std::int64_t first = 0;
  for (int picture = 0; picture < 19; ++picture) {
    machine.run(kFrame);
    const Frames frames = machine.takeFrames();
    const FrameMeasure measured = measure(frames, first);
    first += static_cast<std::int64_t>(frames.size());
    if (picture > 0) totals.add(measured);
  }
  EXPECT_EQ(wholeRoot(totals.energy / totals.count), 2370);
  EXPECT_EQ(totals.tone / totals.pictures, 4951);
}

// ---- The programs ----

// apu_mixer's readme: "All tests beep, play a test sound, then beep again. For all but the
// noise test, there should be near silence between the beeps." square.s plays both pulses
// at every total against the DMC, which cancels them.
TEST(ApuMixer, Square) {
  expectHeard("apu_mixer/square.nes", {{219, 236}, {928, 945}, 970, 22, 10});
}

// apu_mixer's readme: "near silence between the beeps". triangle.s plays the triangle over
// its range of levels against the DMC, which cancels it.
TEST(ApuMixer, Triangle) {
  expectHeard("apu_mixer/triangle.nes", {{219, 236}, {565, 583}, 606, 15, 7});
}

// apu_mixer's readme: "For the noise test, noise will fade in and out. There shouldn't be
// any noticeable tone". noise.s plays the noise at each volume, its level toggled against
// the DMC.
TEST(ApuMixer, Noise) {
  expectHeard("apu_mixer/noise.nes", {{221, 238}, {1119, 1136}, 1159, 130, 13});
}

// apu_mixer's readme: "near silence between the beeps". dmc.s plays pulse 1 at volume 15
// against pairs of DMC levels over its range, which cancel it.
TEST(ApuMixer, Dmc) {
  expectHeard("apu_mixer/dmc.nes", {{219, 236}, {679, 696}, 719, 18, 8});
}

}  // namespace
