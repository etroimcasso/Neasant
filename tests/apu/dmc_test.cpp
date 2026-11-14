#include "neasant/apu/apu.h"
#include "neasant/nes/clock.h"
#include "neasant/nes/nes.h"
#include "rom_harness.h"
#include "authored_image.h"

#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef NEASANT_NES_TEST_ROMS
#define NEASANT_NES_TEST_ROMS ""
#endif
#ifndef NEASANT_DMC_PROGRAMS_DIR
#define NEASANT_DMC_PROGRAMS_DIR ""
#endif

// The DMC driven through apu.h a cycle at a time, then on the machine with every bus access
// watched, every expected cycle derived from the DMC and DMA pages, the Visual 2A03
// readings and blargg's measurements; and blargg's and Fiskbit's DMC programs booted on
// the machine and run to their report, read out of the state value or counted through the
// bus observer. A cycle below is a tick index counted from a chip's first tick, or a
// machine cycle counted from power-on, whose first seven cycles are the reset sequence.
// The NEASANT_NES_TEST_ROMS and NEASANT_DMC_PROGRAMS_DIR options, the skips and their
// require variables are as in the frame counter's test.
//
// At power-on the DMC's timer is 0 and its output unit has eight clocks to go, so its
// first clock falls at the first get tick at rate 0's period and, with rate 0 kept, the
// eighth, the first cycle start, at tick 2996; on a machine whose program writes rate F
// at cycle 12 the clocks fall at 0, 428, 482 and every 54 after, the eighth at 752. The
// chip's own first clock comes wherever its timer stood at power-on.

namespace {

using neasant::AccessSource;
using neasant::ApuState;
using neasant::apuDmcDeliver;
using neasant::apuDmcWantsFetch;
using neasant::apuPeekStatus;
using neasant::apuReadStatus;
using neasant::apuReset;
using neasant::apuTickCycle;
using neasant::apuWrite;
using neasant::BusAccess;
using neasant::BusObserver;
using neasant::consoleClock;
using neasant::DmcFetch;
using neasant::DmcStage;
using neasant::Joypad;
using neasant::JoypadPort;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::Region;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::loadRom;
using neasant::nes_tests::placeProgram;
using neasant::nes_tests::prgAt;
using neasant::nes_tests::Report;
using neasant::nes_tests::RomResult;
using neasant::nes_tests::runUntilResult;
using neasant::nes_tests::screenText;
using enum neasant::CycleKind;

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint8_t kGet = 0;
constexpr std::uint8_t kPut = 1;

// ---- The chip ----

// The chip driven as the machine drives it: a write lands in the current cycle's half, a
// tick ends the cycle in that half, and the half then alternates. `ticks` counts the ticks
// run, so the next tick's index.
struct Chip {
  ApuState state{};
  std::uint8_t half;
  long ticks = 0;

  explicit Chip(std::uint8_t first = kGet) : half(first) {}

  void write(std::uint16_t address, std::uint8_t value) {
    apuWrite(state, address, value, half);
  }

  void tick(long n = 1) {
    for (long i = 0; i < n; ++i) {
      apuTickCycle(state, half);
      half = static_cast<std::uint8_t>(half ^ 1u);
      ++ticks;
    }
  }

  // Ticks until tick `k` has run.
  void through(long k) { tick(k + 1 - ticks); }

  void deliver(std::uint8_t byte) { apuDmcDeliver(state, byte); }
};

// ---- The machine ----

// Every access, in order: the index is the machine cycle.
struct Log final : BusObserver {
  std::vector<BusAccess> accesses;
  void access(const BusAccess& access) override { accesses.push_back(access); }
};

// Checks one access against what the machine is expected to have told.
void expectAccess(const std::vector<BusAccess>& accesses, std::size_t index,
                  const BusAccess& expected) {
  ASSERT_LT(index, accesses.size());
  const BusAccess& access = accesses[index];
  EXPECT_EQ(access.address, expected.address) << "access " << index;
  EXPECT_EQ(access.value, expected.value) << "access " << index;
  EXPECT_EQ(access.write, expected.write) << "access " << index;
  EXPECT_EQ(access.kind, expected.kind) << "access " << index;
  EXPECT_EQ(access.source, expected.source) << "access " << index;
}

// The halted reads among the accesses from index `from` up to `to`.
int countHalted(const std::vector<BusAccess>& accesses, std::size_t from, std::size_t to) {
  int count = 0;
  for (std::size_t i = from; i < to && i < accesses.size(); ++i) {
    if (accesses[i].kind == Halted) ++count;
  }
  return count;
}

// The first opcode fetch after access `after`, or the log's size when none follows.
std::size_t nextOpcodeFetch(const std::vector<BusAccess>& accesses, std::size_t after) {
  for (std::size_t i = after + 1; i < accesses.size(); ++i) {
    if (accesses[i].kind == OpcodeFetch) return i;
  }
  return accesses.size();
}

// The sample bytes' place in the image's PRG area, and their count.
constexpr std::size_t kSampleOffset = 0x3F00;
constexpr std::size_t kSampleBytes = 64;

// A 16 KB NROM image with 8 KB of CHR ROM, `program` at $8000, the sample bytes
// $A5 $E1 $1D … at offset $3F00, which $4012 = $FC names through the mirror at $FF00, and
// every vector pointing at $8000.
Bytes imageWith(const Bytes& program) {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  placeProgram(image, 0, program);
  for (std::size_t i = 0; i < kSampleBytes; ++i) {
    prgAt(image, kSampleOffset + i) = static_cast<std::uint8_t>(0xA5u + 0x3Cu * i);
  }
  return image;
}

// A machine over `image`, built with the defaults but for the APU half the first cycle
// falls in.
Nes machineOver(const Bytes& image, std::uint8_t apuPhase = 0) {
  return Nes(NesConfig{.rom = image, .apuPhase = apuPhase});
}

Bytes nops(std::size_t n) { return Bytes(n, 0xEA); }

Bytes join(std::initializer_list<Bytes> parts) {
  Bytes joined;
  for (const Bytes& part : parts) joined.insert(joined.end(), part.begin(), part.end());
  return joined;
}

// `program` followed by a jump to itself, so a long program never runs off its end.
Bytes looped(Bytes program) {
  const auto at = static_cast<std::uint16_t>(0x8000u + program.size());
  program.insert(program.end(), {0x4C, static_cast<std::uint8_t>(at & 0xFFu),
                                 static_cast<std::uint8_t>(at >> 8)});
  return program;
}

void stepCycles(Nes& machine, long cycles) {
  for (long i = 0; i < cycles; ++i) machine.step();
}

// The DMC set up and started: the $4015 write lands in machine cycle 30, an even cycle,
// which is a get under the default apuPhase and a put under 1.
// LDA #rate; STA $4010; LDA #$FC; STA $4012; LDA #length; STA $4013; LDA #$10; STA $4015
Bytes dmcStart(std::uint8_t rate, std::uint8_t length) {
  return {0xA9, rate, 0x8D, 0x10, 0x40, 0xA9, 0xFC,   0x8D, 0x12, 0x40,
          0xA9, length, 0x8D, 0x13, 0x40, 0xA9, 0x10, 0x8D, 0x15, 0x40};
}

// A loop of reads, so a halt attempt never finds a write cycle.
// LDA $00; NOP; JMP at
Bytes readLoop(std::uint16_t at) {
  return {0xA5, 0x00, 0xEA, 0x4C, static_cast<std::uint8_t>(at & 0xFFu),
          static_cast<std::uint8_t>(at >> 8)};
}

// ---- The programs ----

// The guard, in frames: 30 seconds at the console's rate.
constexpr std::uint64_t kGuardFrames = 1800;

// One frame with rendering off, in master cycles.
constexpr std::uint64_t kFrame = consoleClock(Region::Ntsc).masterCyclesPerFrame;

bool required(const char* variable) {
  const char* raw = std::getenv(variable);
  return raw != nullptr && std::string(raw) == "1";
}

// Loads `relative` from `dir`, which the cache variable `option` sets, into `image`, or
// leaves it empty after skipping or failing the case: the case skips naming the option
// when it is unset and the file when it is absent, and either is a failure instead when
// the environment variable `require` is 1.
void loadOrSkip(const std::string& dir, const char* option, const char* require,
                const std::string& relative, std::optional<Bytes>& image) {
  image.reset();
  if (dir.empty()) {
    if (required(require)) {
      ADD_FAILURE() << relative << ": " << option << " is unset and " << require << " is 1";
      return;
    }
    GTEST_SKIP() << relative << ": set " << option << " to run it";
  }
  const std::string path = dir + "/" + relative;
  image = loadRom(path);
  if (!image) {
    if (required(require)) {
      ADD_FAILURE() << path << " is absent and " << require << " is 1";
      return;
    }
    GTEST_SKIP() << path << " is absent";
  }
}

// The same for a file of the nes-test-roms clone.
void loadOrSkip(const std::string& relative, std::optional<Bytes>& image) {
  loadOrSkip(NEASANT_NES_TEST_ROMS, "NEASANT_NES_TEST_ROMS", "NEASANT_REQUIRE_NES_TEST_ROMS",
             relative, image);
}

// The same for a file under NEASANT_DMC_PROGRAMS_DIR.
void loadDmcProgramOrSkip(const std::string& relative, std::optional<Bytes>& image) {
  loadOrSkip(NEASANT_DMC_PROGRAMS_DIR, "NEASANT_DMC_PROGRAMS_DIR",
             "NEASANT_REQUIRE_DMC_PROGRAMS", relative, image);
}

// Boots the ROM on a machine built with the defaults, with a standard pad holding nothing
// in both ports when `pads` is set, runs it to its report and requires a pass.
void expectPass(const std::string& relative, Report report, bool pads = false) {
  std::optional<Bytes> image;
  loadOrSkip(relative, image);
  if (!image) return;
  Nes machine(NesConfig{.rom = *image});
  if (pads) {
    machine.setJoypad(JoypadPort::One, Joypad{});
    machine.setJoypad(JoypadPort::Two, Joypad{});
  }
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

// Registers a program no readme, source or filed document describes, looked for as above:
// what it reports cannot be read as a pass or a failure until a description is found.
void skipUndescribed(const std::string& relative) {
  std::optional<Bytes> image;
  loadOrSkip(relative, image);
  if (!image) return;
  GTEST_SKIP() << relative
               << " has no readme or source in the clone, and no filed document describes it";
}

// The CRC a program of blargg's read4 set prints as its last line, booted with its first
// cycle in the APU half `half`, read off the screen once a line holds exactly eight
// hexadecimal digits after its leading spaces (`print_crc` in its `crc.s`); empty if none
// appears within 120 frames, and nothing at all when the case was skipped or failed for
// the file.
std::optional<std::string> screenCrc(const std::string& relative, std::uint8_t half) {
  std::optional<Bytes> image;
  loadOrSkip(relative, image);
  if (!image) return std::nullopt;
  Nes machine(NesConfig{.rom = *image, .apuPhase = half});
  for (int frame = 0; frame < 120; ++frame) {
    machine.run(kFrame);
    std::istringstream lines(screenText(machine.state()));
    std::string line;
    while (std::getline(lines, line)) {
      // The program prints the CRC indented by a column.
      line.erase(0, line.find_first_not_of(' '));
      if (line.size() != 8) continue;
      bool hex = true;
      for (const char c : line) {
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0) hex = false;
      }
      if (hex) return line;
    }
  }
  return std::string{};
}

// What a run of one of Fiskbit's programs measured: each greyscale region's width in
// machine cycles with the half of the APU cycle its $4015 write fell on (0 a get, 1 a put),
// or 2 plus the half of its $4014 write when it made none, or -1 when it made neither; the
// test number the program holds at $F2; and its eight joypad-two counters at $E0-$E7.
struct GreyscaleRun {
  std::vector<std::pair<int, long>> regions;
  std::uint8_t test = 0;
  std::array<std::uint8_t, 8> counters{};
};

// Measures each greyscale region as the cycles between the $2001 write that sets bit 0
// and the next $2001 write, with the half of the $4015 and $4014 writes inside it.
struct GreyscaleMeter final : BusObserver {
  const Nes* machine = nullptr;
  bool recording = false;
  long cycle = 0;
  std::optional<long> on;
  bool onRecorded = false;
  int dmcHalf = -1;
  int oamHalf = -1;
  std::vector<std::pair<int, long>> regions;

  void access(const BusAccess& access) override {
    const long now = cycle++;
    if (!access.write) return;
    const int half = machine->state().apuPhase;
    if (access.address == 0x4015) dmcHalf = half;
    if (access.address == 0x4014) oamHalf = half;
    if (access.address != 0x2001) return;
    if (on && onRecorded) {
      regions.emplace_back(dmcHalf >= 0 ? dmcHalf : oamHalf >= 0 ? 2 + oamHalf : -1, now - *on);
    }
    on.reset();
    if ((access.value & 0x01u) != 0) {
      on = now;
      onRecorded = recording;
      dmcHalf = -1;
      oamHalf = -1;
    }
  }
};

// Fiskbit's program synchronized for forty frames, its test selected by pressing A once a
// frame per step, and the greyscale region of each of `frames` frames measured as the
// machine cycles between the $2001 write that turns it on and the write that turns it off,
// with the half of the APU cycle the test's $4015 write fell on ($4014's for the OAM-only
// test). Both ports hold a pad: the program's read loops wait for the ninth read of each
// to answer 1.
std::optional<GreyscaleRun> fiskbit(const std::string& relative, int test, int frames) {
  std::optional<Bytes> image;
  loadDmcProgramOrSkip(relative, image);
  if (!image) return std::nullopt;
  Nes machine(NesConfig{.rom = *image});
  GreyscaleMeter meter;
  meter.machine = &machine;
  machine.setObserver(&meter);
  machine.setJoypad(JoypadPort::One, Joypad{});
  machine.setJoypad(JoypadPort::Two, Joypad{});
  for (int frame = 0; frame < 40; ++frame) machine.run(kFrame);
  for (int press = 0; press < test; ++press) {
    machine.setJoypad(JoypadPort::One, Joypad{.a = true});
    machine.run(kFrame);
    machine.setJoypad(JoypadPort::One, Joypad{});
    machine.run(kFrame);
  }
  machine.run(kFrame);
  meter.recording = true;
  for (int frame = 0; frame < frames; ++frame) machine.run(kFrame);
  machine.setObserver(nullptr);
  GreyscaleRun run;
  run.regions = meter.regions;
  run.test = machine.state().ram[0xF2];
  for (std::size_t i = 0; i < run.counters.size(); ++i) run.counters[i] = machine.state().ram[0xE0 + i];
  return run;
}

// The widths Fiskbit's readme measures for test `test` on an RP2A03H, by the half of the
// test's $4015 write, as the machine gives them: a figure under -1 holds for any half.
std::map<int, long> expectedWidths(int test) {
  switch (test) {
    case 0x0: return {{-1, 13}};
    case 0x1: return {{kGet, 16}, {kPut, 16}};
    case 0x2: return {{kGet, 16}, {kPut, 16}};
    case 0x3: return {{kGet, 16}, {kPut, 17}};
    case 0x4: return {{kGet, 17}, {kPut, 16}};
    case 0x5: return {{kGet, 8}, {kPut, 11}};
    case 0x6: return {{2 + kGet, 525}, {2 + kPut, 526}};
    case 0x7: return {{kGet, 527}, {kPut, 528}};
    case 0x8: return {{kGet, 17}, {kPut, 17}};
    case 0x9: return {{kGet, 17}, {kPut, 17}};
    case 0xA: return {{kGet, 17}, {kPut, 17}};
    case 0xB: return {{kGet, 17}, {kPut, 18}};
    default: return {};
  }
}

// Runs test `test` of `relative` for sixteen frames and checks its widths and counters.
void expectFiskbit(const std::string& relative, int test) {
  const std::optional<GreyscaleRun> run = fiskbit(relative, test, 16);
  if (!run) return;
  EXPECT_EQ(run->test, test);
  const std::map<int, long> widths = expectedWidths(test);
  if (widths.empty()) {
    EXPECT_TRUE(run->regions.empty()) << run->regions.size() << " regions";
  } else {
    ASSERT_EQ(run->regions.size(), 16u);
    for (const auto& [half, width] : run->regions) {
      const auto any = widths.find(-1);
      const auto byHalf = widths.find(half);
      ASSERT_TRUE(any != widths.end() || byHalf != widths.end()) << "half " << half;
      EXPECT_EQ(width, (byHalf != widths.end() ? byHalf : any)->second) << "half " << half;
    }
  }
  for (std::size_t i = 0; i < run->counters.size(); ++i) {
    const int expected = test == 0xC && i == 0 ? 10 : 0;
    EXPECT_EQ(run->counters[i], expected) << "$E" << i;
  }
}

constexpr const char* kStartTestV2 = "dmc_dma_start_test_v2/dmc_dma_start_test_v2.nes";
constexpr const char* kStartTestV1 = "dmc_dma_start_test/dmc_dma_start_test.nes";

}  // namespace

// ---- DmcRegisters ----

TEST(DmcRegisters, TheFourRegistersTakeTheirBits) {
  // NESdev wiki, "APU DMC", Registers: $4010 IL-- RRRR, $4011 the level's seven bits,
  // $4012 and $4013 as written. blargg's NES APU reference, DMC: a $4010 write with the
  // interrupt bit clear clears the flag.
  Chip chip;
  chip.write(0x4010, 0xCF);
  EXPECT_TRUE(chip.state.dmc.irqEnable);
  EXPECT_TRUE(chip.state.dmc.loop);
  EXPECT_EQ(chip.state.dmc.rateIndex, 15);
  chip.write(0x4011, 0xFF);
  EXPECT_EQ(chip.state.dmc.output, 0x7F);
  chip.write(0x4012, 0xAB);
  chip.write(0x4013, 0xCD);
  EXPECT_EQ(chip.state.dmc.sampleAddress, 0xAB);
  EXPECT_EQ(chip.state.dmc.sampleLength, 0xCD);
  chip.state.dmc.interrupt = true;
  chip.write(0x4010, 0x45);
  EXPECT_FALSE(chip.state.dmc.irqEnable);
  EXPECT_TRUE(chip.state.dmc.loop);
  EXPECT_EQ(chip.state.dmc.rateIndex, 5);
  EXPECT_FALSE(chip.state.dmc.interrupt);
  chip.state.dmc.interrupt = true;
  chip.write(0x4010, 0x80);
  EXPECT_TRUE(chip.state.dmc.interrupt);
}

TEST(DmcRegisters, AStartLoadsTheReaderAndAsksForALoad) {
  // NESdev wiki, "APU DMC", Sample playback control: bit 4 set restarts the sample from
  // $C000 + 64 x $4012 for 16 x $4013 + 1 bytes only when the bytes remaining are 0; bit 4
  // clear sets them to 0. "DMA", DMC DMA: the first byte is fetched at once when the buffer
  // is empty; "DMA", Bugs: a request whose sample stopped lapses.
  Chip chip;
  chip.write(0x4012, 0x02);
  chip.write(0x4013, 0x01);
  chip.write(0x4015, 0x10);
  EXPECT_EQ(chip.state.dmc.address, 0xC080);
  EXPECT_EQ(chip.state.dmc.bytesRemaining, 17);
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::Load);
  EXPECT_EQ(apuPeekStatus(chip.state, 0) & 0x10u, 0x10u);
  chip.write(0x4012, 0x03);
  chip.write(0x4013, 0x00);
  chip.write(0x4015, 0x10);
  EXPECT_EQ(chip.state.dmc.address, 0xC080);
  EXPECT_EQ(chip.state.dmc.bytesRemaining, 17);
  chip.write(0x4015, 0x00);
  EXPECT_EQ(chip.state.dmc.bytesRemaining, 0);
  EXPECT_EQ(apuPeekStatus(chip.state, 0) & 0x10u, 0u);
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::Load);
  chip.tick();
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::None);
  chip.write(0x4015, 0x10);
  EXPECT_EQ(chip.state.dmc.address, 0xC0C0);
  EXPECT_EQ(chip.state.dmc.bytesRemaining, 1);
}

TEST(DmcRegisters, AStartWithTheBufferFullWaitsForTheOutputUnit) {
  // NESdev wiki, "APU DMC", Memory reader: a byte is fetched whenever the buffer is empty
  // and bytes remain, so a start with the buffer full fetches nothing until the output unit
  // takes the buffered byte, at the eighth clock after power-on: rate 0's clocks at ticks
  // 0, 428 … 2996.
  Chip chip;
  chip.deliver(0x55);
  chip.write(0x4015, 0x10);
  EXPECT_EQ(chip.state.dmc.bytesRemaining, 1);
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::None);
  chip.through(2995);
  EXPECT_TRUE(chip.state.dmc.bufferFull);
  EXPECT_TRUE(chip.state.dmc.silence);
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::None);
  chip.through(2996);
  EXPECT_FALSE(chip.state.dmc.bufferFull);
  EXPECT_FALSE(chip.state.dmc.silence);
  EXPECT_EQ(chip.state.dmc.shift, 0x55);
  EXPECT_EQ(chip.state.dmc.bitsRemaining, 8);
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::Reload);
  chip.tick(500);
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::Reload);
}

// ---- DmcOutput ----

TEST(DmcOutput, TheLevelStepsByTwoAndStopsAtTheEnds) {
  // NESdev wiki, "APU DMC", Output unit: each clock steps the level by 2, up for a 1 bit
  // while it is below 126 and down for a 0 while it is above 1, unless the silence flag is
  // set; a cycle begins after the eighth bit, taking the buffer or setting the flag. Rate
  // F clocks every 54 ticks from tick 0; the eighth clock after power-on is tick 378.
  Chip chip;
  chip.write(0x4010, 0x0F);
  chip.write(0x4011, 0x7C);
  chip.deliver(0xFF);
  chip.through(377);
  EXPECT_EQ(chip.state.dmc.bitsRemaining, 1);
  EXPECT_EQ(chip.state.dmc.output, 124);
  chip.through(378);
  EXPECT_EQ(chip.state.dmc.shift, 0xFF);
  EXPECT_EQ(chip.state.dmc.output, 124);
  chip.through(432);
  EXPECT_EQ(chip.state.dmc.output, 126);
  chip.through(486);
  EXPECT_EQ(chip.state.dmc.output, 126);
  chip.deliver(0x00);
  chip.through(810);
  EXPECT_EQ(chip.state.dmc.shift, 0x00);
  EXPECT_EQ(chip.state.dmc.output, 126);
  chip.through(864);
  EXPECT_EQ(chip.state.dmc.output, 124);
  chip.through(1242);
  EXPECT_EQ(chip.state.dmc.output, 110);
  EXPECT_TRUE(chip.state.dmc.silence);
  chip.through(1673);
  EXPECT_EQ(chip.state.dmc.output, 110);
  chip.write(0x4011, 0x03);
  chip.deliver(0x00);
  chip.through(2105);
  EXPECT_EQ(chip.state.dmc.output, 1);
  chip.through(2537);
  EXPECT_EQ(chip.state.dmc.output, 1);
  chip.write(0x4011, 0x02);
  chip.deliver(0x00);
  chip.through(2969);
  EXPECT_EQ(chip.state.dmc.output, 0);
}

TEST(DmcOutput, TheRateTableSetsTheClockSpacing) {
  // NESdev wiki, "APU DMC", Pitch table, the NTSC column: the CPU cycles between two clocks
  // of the output unit.
  constexpr std::array<long, 16> kRates{428, 380, 340, 320, 286, 254, 226, 214,
                                        190, 160, 142, 128, 106, 84,  72,  54};
  for (std::size_t index = 0; index < kRates.size(); ++index) {
    Chip chip;
    chip.write(0x4010, static_cast<std::uint8_t>(index));
    chip.through(0);
    EXPECT_EQ(chip.state.dmc.bitsRemaining, 7) << "rate " << index;
    chip.through(kRates[index] - 1);
    EXPECT_EQ(chip.state.dmc.bitsRemaining, 7) << "rate " << index;
    chip.through(kRates[index]);
    EXPECT_EQ(chip.state.dmc.bitsRemaining, 6) << "rate " << index;
  }
}

TEST(DmcOutput, TheTimerCountsOnGetTicks) {
  // NESdev wiki, "APU", Glossary: the DMC's timer is clocked every second CPU cycle; Visual
  // 2A03: its output unit changes as a put cycle begins, the tick of the get cycle before.
  Chip chip(kPut);
  chip.through(0);
  EXPECT_EQ(chip.state.dmc.bitsRemaining, 8);
  chip.through(1);
  EXPECT_EQ(chip.state.dmc.bitsRemaining, 7);
}

// ---- DmcReader ----

TEST(DmcReader, ADeliveryStepsTheAddressAndCountsDownAndTheLoopRestarts) {
  // NESdev wiki, "APU DMC", Memory reader: a delivery fills the buffer, steps the address
  // and counts the bytes down; at 0 the sample restarts with the loop flag set and
  // otherwise ends, setting the interrupt flag with the interrupt enabled.
  Chip chip;
  chip.write(0x4010, 0xCF);
  chip.write(0x4012, 0x00);
  chip.write(0x4013, 0x00);
  chip.write(0x4015, 0x10);
  EXPECT_EQ(chip.state.dmc.bytesRemaining, 1);
  chip.deliver(0x12);
  EXPECT_EQ(chip.state.dmc.bytesRemaining, 1);
  EXPECT_EQ(chip.state.dmc.address, 0xC000);
  EXPECT_FALSE(chip.state.dmc.interrupt);
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::None);
  EXPECT_EQ(chip.state.dmc.buffer, 0x12);
  chip.write(0x4010, 0x8F);
  chip.deliver(0x34);
  EXPECT_EQ(chip.state.dmc.bytesRemaining, 0);
  EXPECT_EQ(chip.state.dmc.address, 0xC001);
  EXPECT_TRUE(chip.state.dmc.interrupt);
  EXPECT_EQ(chip.state.dmc.stopAge, 0);
}

TEST(DmcReader, TheAddressWrapsToEightThousand) {
  // NESdev wiki, "APU DMC", Memory reader: the address wraps from $FFFF to $8000; 2A03
  // technical reference, $4012.
  Chip chip;
  chip.write(0x4012, 0xFF);
  chip.write(0x4013, 0x04);
  chip.write(0x4015, 0x10);
  EXPECT_EQ(chip.state.dmc.address, 0xFFC0);
  EXPECT_EQ(chip.state.dmc.bytesRemaining, 65);
  for (int i = 0; i < 63; ++i) chip.deliver(0x00);
  EXPECT_EQ(chip.state.dmc.address, 0xFFFF);
  EXPECT_EQ(chip.state.dmc.bytesRemaining, 2);
  chip.deliver(0x00);
  EXPECT_EQ(chip.state.dmc.address, 0x8000);
  EXPECT_EQ(chip.state.dmc.bytesRemaining, 1);
}

TEST(DmcReader, TheFlagClearsByAStatusWriteOrByClearingBitSevenAndNotByARead) {
  // NESdev wiki, "APU", Status: a read clears the frame interrupt flag but not the DMC's;
  // "APU DMC", Sample playback control: a $4015 write clears it; blargg's NES APU
  // reference, DMC: a $4010 write with bit 7 clear clears it; 7-dmc_basics, tests 10-12.
  Chip chip;
  chip.write(0x4010, 0x8F);
  chip.write(0x4013, 0x00);
  chip.write(0x4015, 0x10);
  chip.deliver(0x00);
  EXPECT_TRUE(chip.state.dmc.interrupt);
  EXPECT_EQ(apuReadStatus(chip.state, 0) & 0x90u, 0x80u);
  EXPECT_TRUE(chip.state.dmc.interrupt);
  chip.write(0x4015, 0x00);
  EXPECT_FALSE(chip.state.dmc.interrupt);
  chip.write(0x4015, 0x10);
  chip.deliver(0x00);
  EXPECT_TRUE(chip.state.dmc.interrupt);
  chip.write(0x4010, 0x0F);
  EXPECT_FALSE(chip.state.dmc.interrupt);
  chip.write(0x4010, 0x8F);
  EXPECT_FALSE(chip.state.dmc.interrupt);
}

TEST(DmcReader, TheIrqLineFollowsTheFlagAtTheNextTick) {
  // NESdev wiki, "APU DMC", Memory reader: the IRQ line is asserted while the flag is set;
  // the line is sampled at the top of each tick, after the cycle's access.
  Chip chip;
  chip.write(0x4010, 0x8F);
  chip.write(0x4013, 0x00);
  chip.write(0x4015, 0x10);
  chip.tick(10);
  chip.deliver(0x00);
  EXPECT_TRUE(chip.state.dmc.interrupt);
  EXPECT_FALSE(chip.state.irqLine);
  chip.tick();
  EXPECT_TRUE(chip.state.irqLine);
  chip.write(0x4015, 0x00);
  EXPECT_TRUE(chip.state.irqLine);
  chip.tick();
  EXPECT_FALSE(chip.state.irqLine);
}

TEST(DmcReader, AStopOneOrTwoTicksBeforeACycleStartAsksOnceAndTheRequestLapses) {
  // NESdev wiki, "DMA", Bugs: playback stopped during the APU cycle before a reload DMA
  // would schedule still starts the DMA. The buffered byte is taken at tick 2996.
  for (const long before : {0L, 1L, 2L, 3L}) {
    Chip chip;
    chip.deliver(0x00);
    chip.write(0x4015, 0x10);
    chip.tick(2996 - before);
    chip.write(0x4015, 0x00);
    chip.through(2995);
    EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::None) << "before " << before;
    chip.through(2996);
    const DmcFetch asked = before == 1 || before == 2 ? DmcFetch::Reload : DmcFetch::None;
    EXPECT_EQ(apuDmcWantsFetch(chip.state), asked) << "before " << before;
    chip.through(2997);
    EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::None) << "before " << before;
  }
}

TEST(DmcReader, ALastFetchTwoTicksBeforeACycleStartAsksOnce) {
  // NESdev wiki, "DMA", Bugs: the implicit case, the sample's last fetch two ticks before
  // the output unit takes the buffered byte.
  Chip chip;
  chip.write(0x4010, 0x00);
  chip.write(0x4013, 0x00);
  chip.write(0x4015, 0x10);
  chip.tick(2994);
  chip.deliver(0x00);
  chip.through(2994);
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::None);
  chip.through(2995);
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::None);
  chip.through(2996);
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::Reload);
  chip.through(2997);
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::None);
}

TEST(DmcReader, ResetStopsTheSampleAndKeepsTheRegistersAndTheLevelsLowBit) {
  // NESdev wiki, "APU", Status: reset writes $00 to $4015; "CPU power up state", APU, the
  // After Reset column: $4011 keeps its low bit and the other registers are unmodified.
  Chip chip;
  chip.write(0x4010, 0xCF);
  chip.write(0x4011, 0x7F);
  chip.write(0x4012, 0x12);
  chip.write(0x4013, 0x01);
  chip.write(0x4015, 0x10);
  chip.deliver(0x99);
  chip.state.dmc.interrupt = true;
  apuReset(chip.state);
  EXPECT_EQ(chip.state.dmc.bytesRemaining, 0);
  EXPECT_FALSE(chip.state.dmc.interrupt);
  EXPECT_EQ(chip.state.dmc.output, 1);
  EXPECT_EQ(apuDmcWantsFetch(chip.state), DmcFetch::None);
  EXPECT_TRUE(chip.state.dmc.irqEnable);
  EXPECT_TRUE(chip.state.dmc.loop);
  EXPECT_EQ(chip.state.dmc.rateIndex, 15);
  EXPECT_EQ(chip.state.dmc.sampleAddress, 0x12);
  EXPECT_EQ(chip.state.dmc.sampleLength, 0x01);
  EXPECT_TRUE(chip.state.dmc.bufferFull);
  EXPECT_EQ(chip.state.dmc.buffer, 0x99);
}

// ---- DmcMachine ----
//
// The sample's first byte $A5 is at $FF00, its second $E1 at $FF01, its seventeenth $65 at
// $FF10.

TEST(DmcMachine, ALoadHaltsOnTheThirdCycleAfterAWriteOnAPutAndTheFourthAfterAGet) {
  // NESdev wiki, "DMA", DMC DMA: a load halts the CPU on the get cycle of the second APU
  // cycle after the $4015 write, then a dummy cycle, then the get; Visual 2A03: RDY falls in
  // the third cycle after a write on a put and the fourth after one on a get.
  const Bytes image = imageWith(join({dmcStart(0x0F, 0x00), readLoop(0x8014)}));
  {
    Nes m = machineOver(image, kPut);
    Log log;
    m.setObserver(&log);
    stepCycles(m, 40);
    expectAccess(log.accesses, 30,
                 {.address = 0x4015, .value = 0x10, .write = true, .kind = DataWrite});
    expectAccess(log.accesses, 32, {.address = 0x8015, .value = 0x00, .kind = OperandFetch});
    expectAccess(log.accesses, 33, {.address = 0x0000, .value = 0x00, .kind = Halted});
    expectAccess(log.accesses, 34, {.address = 0x0000, .value = 0x00, .kind = Halted});
    expectAccess(log.accesses, 35, {.address = 0xFF00, .value = 0xA5, .kind = DataRead,
                                    .source = AccessSource::DmcDma});
    expectAccess(log.accesses, 36, {.address = 0x0000, .value = 0x00, .kind = DataRead});
    expectAccess(log.accesses, 37, {.address = 0x8016, .value = 0xEA, .kind = OpcodeFetch});
    const NesState& s = m.state();
    EXPECT_EQ(s.dma.dmcStage, DmcStage::Idle);
    EXPECT_TRUE(s.cpu.rdyLine);
    EXPECT_TRUE(s.apu.dmc.bufferFull);
    EXPECT_EQ(s.apu.dmc.buffer, 0xA5);
    EXPECT_EQ(s.apu.dmc.bytesRemaining, 0);
    EXPECT_EQ(s.apu.dmc.address, 0xFF01);
  }
  {
    Nes m = machineOver(image, kGet);
    Log log;
    m.setObserver(&log);
    stepCycles(m, 40);
    expectAccess(log.accesses, 33, {.address = 0x0000, .value = 0x00, .kind = DataRead});
    expectAccess(log.accesses, 34, {.address = 0x8016, .value = 0xEA, .kind = Halted});
    expectAccess(log.accesses, 35, {.address = 0x8016, .value = 0xEA, .kind = Halted});
    expectAccess(log.accesses, 36, {.address = 0xFF00, .value = 0xA5, .kind = DataRead,
                                    .source = AccessSource::DmcDma});
    expectAccess(log.accesses, 37, {.address = 0x8016, .value = 0xEA, .kind = OpcodeFetch});
  }
}

TEST(DmcMachine, AWriteInTheWayDelaysTheHaltAndTheFetchTakesFourCycles) {
  // NESdev wiki, "DMA", DMC DMA, "Load DMA (delayed 1 cycle)": a write on the halt cycle
  // moves the halt to the next cycle, a put, so an alignment cycle follows the dummy.
  // STX $0000, its write at 34
  const Bytes image =
      imageWith(join({dmcStart(0x0F, 0x00), {0x8E, 0x00, 0x00}, readLoop(0x8017)}));
  Nes m = machineOver(image);
  Log log;
  m.setObserver(&log);
  stepCycles(m, 40);
  expectAccess(log.accesses, 34,
               {.address = 0x0000, .value = 0x00, .write = true, .kind = DataWrite});
  for (std::size_t i = 35; i <= 37; ++i) {
    expectAccess(log.accesses, i, {.address = 0x8017, .value = 0xA5, .kind = Halted});
  }
  expectAccess(log.accesses, 38, {.address = 0xFF00, .value = 0xA5, .kind = DataRead,
                                  .source = AccessSource::DmcDma});
  expectAccess(log.accesses, 39, {.address = 0x8017, .value = 0xA5, .kind = OpcodeFetch});
}

TEST(DmcMachine, AReloadHaltsOnAPutTheCycleAfterTheOutputUnitEmptiesTheBufferAndTakesFour) {
  // NESdev wiki, "DMA", DMC DMA: reloads halt on a put cycle, then the dummy, an alignment
  // cycle and the get; Visual 2A03: RDY falls in the put cycle in which the shift register
  // loads. The clocks fall at 428 and every 54 after, the first cycle start at 752, the
  // reloads at 756 + 432k.
  const Bytes image = imageWith(join({dmcStart(0x0F, 0x01), readLoop(0x8014)}));
  Nes m = machineOver(image);
  Log log;
  m.setObserver(&log);
  stepCycles(m, 780);
  EXPECT_EQ(m.state().apu.dmc.bytesRemaining, 15);
  EXPECT_EQ(m.state().apu.dmc.address, 0xFF02);
  EXPECT_EQ(countHalted(log.accesses, 37, 753), 0);
  for (std::size_t i = 753; i <= 755; ++i) {
    ASSERT_LT(i, log.accesses.size());
    EXPECT_EQ(log.accesses[i].kind, Halted) << "access " << i;
    EXPECT_EQ(log.accesses[i].address, log.accesses[753].address) << "access " << i;
  }
  expectAccess(log.accesses, 756, {.address = 0xFF01, .value = 0xE1, .kind = DataRead,
                                   .source = AccessSource::DmcDma});
  EXPECT_EQ(countHalted(log.accesses, 757, 780), 0);
  stepCycles(m, 7236 - 780);
  EXPECT_EQ(m.state().apu.dmc.bytesRemaining, 1);
  m.step();
  EXPECT_EQ(m.state().apu.dmc.bytesRemaining, 0);
  EXPECT_FALSE(m.state().apu.dmc.interrupt);
  expectAccess(log.accesses, 7236, {.address = 0xFF10, .value = 0x65, .kind = DataRead,
                                    .source = AccessSource::DmcDma});
}

TEST(DmcMachine, TheInterruptReachesTheCpuTheCycleAfterTheFetch) {
  // NESdev wiki, "APU DMC", Memory reader: the fetch that ends the sample sets the flag,
  // which asserts the IRQ line; "CPU interrupts", Detailed interrupt behavior: the CPU
  // takes it at its next poll. CLI; the DMC started with the interrupt enabled, the write
  // at 32 and the fetch at 38; the handler at $9000 counts the interrupt and clears the
  // flag. The NOP halted at 36 ends at 40, and the interrupt sequence runs 41-47.
  // INC $10; LDA #$0F; STA $4010; RTI
  Bytes image = imageWith(join({{0x58}, dmcStart(0x8F, 0x00), readLoop(0x8015)}));
  prgAt(image, 0x3FFE) = 0x00;
  prgAt(image, 0x3FFF) = 0x90;
  placeProgram(image, 0x1000, Bytes{0xE6, 0x10, 0xA9, 0x0F, 0x8D, 0x10, 0x40, 0x40});
  Nes m = machineOver(image);
  Log log;
  m.setObserver(&log);
  stepCycles(m, 39);
  expectAccess(log.accesses, 32,
               {.address = 0x4015, .value = 0x10, .write = true, .kind = DataWrite});
  expectAccess(log.accesses, 38, {.address = 0xFF00, .value = 0xA5, .kind = DataRead,
                                  .source = AccessSource::DmcDma});
  EXPECT_TRUE(m.state().apu.dmc.interrupt);
  EXPECT_TRUE(m.state().apu.irqLine);
  EXPECT_TRUE(m.state().cpu.irqLine);
  stepCycles(m, 30);
  EXPECT_EQ(m.state().ram[0x10], 1);
  EXPECT_FALSE(m.state().apu.dmc.interrupt);
  expectAccess(log.accesses, 46, {.address = 0xFFFE, .value = 0x00, .kind = VectorRead});
}

TEST(DmcMachine, AFetchInsideAnOamTransferCostsTwoCyclesAndOneOrThreeAtItsEnd) {
  // NESdev wiki, "DMA", DMC DMA during OAM DMA: a DMC DMA in the middle of an OAM transfer
  // costs 2 cycles; scheduled for the second-to-last put, 1; for the last put, 3. blargg's
  // sprdma_and_dmc_dma post's second table. The $4014 write lands at 39 + 2n, the load's
  // three cycles shifting the stream; with a 17-byte sample the reload halts at 2997, with
  // a one-byte sample there is none.
  struct Case {
    std::size_t nopCount;
    std::size_t write;
    std::size_t withReload;
    std::size_t withoutReload;
  };
  for (const Case c : {Case{1279, 2597, 3114, 3112}, Case{1223, 2485, 3001, 3000},
                       Case{1222, 2483, 3001, 2998}}) {
    std::array<std::size_t, 2> fetched{};
    for (const std::uint8_t length : {std::uint8_t{1}, std::uint8_t{0}}) {
      // LDA #$02; STA $4014; NOP; NOP
      const Bytes image = imageWith(looped(join({dmcStart(0x00, length), nops(c.nopCount),
                                                 {0xA9, 0x02, 0x8D, 0x14, 0x40, 0xEA, 0xEA}})));
      Nes m = machineOver(image);
      Log log;
      m.setObserver(&log);
      stepCycles(m, 3200);
      expectAccess(log.accesses, c.write,
                   {.address = 0x4014, .value = 0x02, .write = true, .kind = DataWrite});
      fetched[length == 1 ? 0 : 1] = nextOpcodeFetch(log.accesses, c.write);
    }
    EXPECT_EQ(fetched[0], c.withReload) << c.nopCount << " NOPs";
    EXPECT_EQ(fetched[1], c.withoutReload) << c.nopCount << " NOPs";
  }
}

TEST(DmcMachine, AStopTwoCyclesBeforeACycleStartHaltsOnceUnlessAWriteIsInTheWay) {
  // NESdev wiki, "DMA", Bugs, "Explicit-stop aborted DMA": a stop in the APU cycle before a
  // reload would schedule starts the DMA, which is aborted after a single cycle; a write in
  // the way of its halt brings none. The output unit takes the buffered byte at 2996.
  // LDA $00; LDA #$00; STA $4015, its write at 2994
  const Bytes stopAndRead = {0xA5, 0x00, 0xA9, 0x00, 0x8D, 0x15, 0x40};
  {
    // LDA $00
    const Bytes image = imageWith(
        looped(join({dmcStart(0x00, 0x01), nops(1476), stopAndRead, {0xA5, 0x00}, nops(16)})));
    Nes m = machineOver(image);
    Log log;
    m.setObserver(&log);
    stepCycles(m, 3010);
    expectAccess(log.accesses, 2994,
                 {.address = 0x4015, .value = 0x00, .write = true, .kind = DataWrite});
    EXPECT_EQ(countHalted(log.accesses, 2995, 2997), 0);
    expectAccess(log.accesses, 2997, {.address = 0x0000, .value = 0x00, .kind = Halted});
    expectAccess(log.accesses, 2998, {.address = 0x0000, .value = 0x00, .kind = DataRead});
    EXPECT_EQ(countHalted(log.accesses, 2998, 3010), 0);
    EXPECT_EQ(m.state().dma.dmcStage, DmcStage::Idle);
  }
  {
    // STX $00, its write at 2997
    const Bytes image = imageWith(
        looped(join({dmcStart(0x00, 0x01), nops(1476), stopAndRead, {0x86, 0x00}, nops(16)})));
    Nes m = machineOver(image);
    Log log;
    m.setObserver(&log);
    stepCycles(m, 3010);
    expectAccess(log.accesses, 2997,
                 {.address = 0x0000, .value = 0x00, .write = true, .kind = DataWrite});
    EXPECT_EQ(countHalted(log.accesses, 2994, 3010), 0);
  }
  {
    // LDA #$00; STA $4015, its write at 2993; LDA $00
    const Bytes image = imageWith(looped(join(
        {dmcStart(0x00, 0x01), nops(1477), {0xA9, 0x00, 0x8D, 0x15, 0x40, 0xA5, 0x00}, nops(16)})));
    Nes m = machineOver(image);
    Log log;
    m.setObserver(&log);
    stepCycles(m, 3010);
    expectAccess(log.accesses, 2993,
                 {.address = 0x4015, .value = 0x00, .write = true, .kind = DataWrite});
    EXPECT_EQ(countHalted(log.accesses, 2994, 3010), 0);
  }
}

TEST(DmcMachine, ALastFetchTwoCyclesBeforeACycleStartHaltsOnce) {
  // NESdev wiki, "DMA", Bugs, "Implicit-stop aborted DMA": the write that begins a one-byte
  // sample, here at 2988, a get, brings its load at 2992-2994 and the aborted reload's halt
  // at w + 9, when the output unit takes the byte at 2996.
  // LDA #$00; STA $4010; LDA #$FC; STA $4012; LDA #$00; STA $4013; NOP x 1479;
  // LDA #$10; STA $4015; NOP x 4
  const Bytes image = imageWith(looped(join({{0xA9, 0x00, 0x8D, 0x10, 0x40, 0xA9, 0xFC, 0x8D,
                                              0x12, 0x40, 0xA9, 0x00, 0x8D, 0x13, 0x40},
                                             nops(1479),
                                             {0xA9, 0x10, 0x8D, 0x15, 0x40},
                                             nops(4)})));
  Nes m = machineOver(image);
  Log log;
  m.setObserver(&log);
  stepCycles(m, 3010);
  expectAccess(log.accesses, 2988,
               {.address = 0x4015, .value = 0x10, .write = true, .kind = DataWrite});
  EXPECT_EQ(log.accesses[2992].kind, Halted);
  EXPECT_EQ(log.accesses[2993].kind, Halted);
  expectAccess(log.accesses, 2994, {.address = 0xFF00, .value = 0xA5, .kind = DataRead,
                                    .source = AccessSource::DmcDma});
  EXPECT_EQ(countHalted(log.accesses, 2995, 2997), 0);
  EXPECT_EQ(log.accesses[2997].kind, Halted);
  EXPECT_EQ(countHalted(log.accesses, 2998, 3010), 0);
}

TEST(DmcMachine, ResetEndsAFetchAndASnapshotCarriesOne) {
  // NESdev wiki, "CPU pinout", /RST: a DMA under way ends and the CPU is released; "CPU
  // power up state", APU, the After Reset column. The state value carries the fetch's
  // stage, so a machine restored mid-fetch runs on as the original does.
  const Bytes image = imageWith(join({dmcStart(0x0F, 0x01), readLoop(0x8014)}));
  Nes m = machineOver(image, kPut);
  stepCycles(m, 34);
  EXPECT_EQ(m.state().dma.dmcStage, DmcStage::Dummy);
  EXPECT_FALSE(m.state().cpu.rdyLine);
  Nes restored = machineOver(image, kPut);
  restored.restore(m.state());
  Nes third = m;
  stepCycles(m, 1000);
  stepCycles(restored, 1000);
  EXPECT_TRUE(m.state() == restored.state());
  EXPECT_EQ(m.state().apu.dmc.bytesRemaining, 15);
  NesState s = third.state();
  s.apu.dmc.output = 0x7F;
  third.restore(s);
  third.reset();
  EXPECT_EQ(third.state().dma.dmcStage, DmcStage::Idle);
  EXPECT_TRUE(third.state().cpu.rdyLine);
  EXPECT_EQ(third.state().apu.dmc.bytesRemaining, 0);
  EXPECT_EQ(third.state().apu.dmc.output, 1);
  EXPECT_EQ(apuDmcWantsFetch(third.state().apu), DmcFetch::None);
}

// ---- The programs ----

// The DMC's registers, reader, output unit, status bits and interrupt, nineteen tests (the
// apu_test readme and 7-dmc_basics.s); 26 frames.
TEST(ApuTest, Singles7DmcBasics) {
  expectPass("apu_test/rom_singles/7-dmc_basics.nes", Report::Memory);
}

// The sixteen rates, each measured against the CPU (8-dmc_rates.s); 29 frames.
TEST(ApuTest, Singles8DmcRates) {
  expectPass("apu_test/rom_singles/8-dmc_rates.nes", Report::Memory);
}

// "DMC DMA during $2007 read causes 2-3 extra $2007 reads before real read", its two CRCs
// 159A7A8F and 5E3DF9C4 by alignment (its readme). The machine makes three extra reads under
// every alignment and both halves, printing 5E3DF9C4; 20 frames.
TEST(DmcDmaDuringRead4, Dma2007Read) {
  const std::optional<std::string> get = screenCrc("dmc_dma_during_read4/dma_2007_read.nes", kGet);
  if (!get) return;
  EXPECT_EQ(*get, "5E3DF9C4");
  const std::optional<std::string> put = screenCrc("dmc_dma_during_read4/dma_2007_read.nes", kPut);
  if (!put) return;
  EXPECT_EQ(*put, "5E3DF9C4");
}

// "DMC DMA during $2007 write has no effect" (its readme); 24 frames.
TEST(DmcDmaDuringRead4, Dma2007Write) {
  expectPass("dmc_dma_during_read4/dma_2007_write.nes", Report::Screen);
}

// "DMC DMA during $4016 read causes extra $4016 read", printing 08 08 07 08 08 (its readme).
// The pad its `end` routine reads to its ninth bit, which answers 1; 12 frames.
TEST(DmcDmaDuringRead4, Dma4016Read) {
  expectPass("dmc_dma_during_read4/dma_4016_read.nes", Report::Screen, true);
}

// "Double read of $2007 sometimes ignores extra read, and puts odd things into buffer", its
// four CRCs by alignment (its readme), with no DMA at all; the machine prints D84F6815, its
// $2007 read on adjacent cycles two full reads.
TEST(DmcDmaDuringRead4, Double2007Read) {
  skipNeeding("dmc_dma_during_read4/double_2007_read.nes",
              "the PPU's read of $2007 on adjacent cycles, which is not built");
}

// "Read of $2007 just before write behaves normally" (its readme); 12 frames.
TEST(DmcDmaDuringRead4, ReadWrite2007) {
  expectPass("dmc_dma_during_read4/read_write_2007.nes", Report::Screen);
}

// "DMC DMA adds 4 cycles normally, 3 if it lands on a CPU write, 2 if it lands on the $4014
// write or during OAM DMA, 1 if on the next-to-next-to-last DMA cycle, 3 if on the last DMA
// cycle", the program verifying its table (blargg's post of 2010-06-08, "DMA operation in
// APU"); 138 frames.
TEST(SprdmaAndDmcDma, SprdmaAndDmcDma) {
  expectPass("sprdma_and_dmc_dma/sprdma_and_dmc_dma.nes", Report::Screen);
}

// The same over a 512-cycle OAM transfer's alignment (the same post); 135 frames.
TEST(SprdmaAndDmcDma, SprdmaAndDmcDma512) {
  expectPass("sprdma_and_dmc_dma/sprdma_and_dmc_dma_512.nes", Report::Screen);
}

// Its NMI handler turns rendering on and waits for the sprite 0 hit before it starts the
// sample and counts the interrupts (its README.txt and reset.s).
TEST(Dpcmletterbox, Dpcmletterbox) {
  std::optional<Bytes> image;
  loadDmcProgramOrSkip("dpcmletterbox/dpcmletterbox.nes", image);
  if (!image) return;
  GTEST_SKIP() << "dpcmletterbox/dpcmletterbox.nes needs the sprite 0 hit its NMI handler "
                  "waits for, which is not built";
}

// The four of dmc_tests carry no text, readme or source.
TEST(DmcTests, BufferRetained) { skipUndescribed("dmc_tests/buffer_retained.nes"); }
TEST(DmcTests, Latency) { skipUndescribed("dmc_tests/latency.nes"); }
TEST(DmcTests, Status) { skipUndescribed("dmc_tests/status.nes"); }
TEST(DmcTests, StatusIrq) { skipUndescribed("dmc_tests/status_irq.nes"); }

// Fiskbit's dmc_dma_start_test_v2: the greyscale region of each test, which the
// $4015-initiated DMC DMA lengthens, at the widths his readme measures on an RP2A03H, the
// "solid" figure for each half of the write; test C's joypad-two counter one bit a frame
// on one half; D and E no bit deleted.
TEST(DmcDmaStartTestV2, Test0) { expectFiskbit(kStartTestV2, 0x0); }
TEST(DmcDmaStartTestV2, Test1) { expectFiskbit(kStartTestV2, 0x1); }
TEST(DmcDmaStartTestV2, Test2) { expectFiskbit(kStartTestV2, 0x2); }
TEST(DmcDmaStartTestV2, Test3) { expectFiskbit(kStartTestV2, 0x3); }
TEST(DmcDmaStartTestV2, Test4) { expectFiskbit(kStartTestV2, 0x4); }
TEST(DmcDmaStartTestV2, Test5) { expectFiskbit(kStartTestV2, 0x5); }
TEST(DmcDmaStartTestV2, Test6) { expectFiskbit(kStartTestV2, 0x6); }
TEST(DmcDmaStartTestV2, Test7) { expectFiskbit(kStartTestV2, 0x7); }
TEST(DmcDmaStartTestV2, Test8) { expectFiskbit(kStartTestV2, 0x8); }
TEST(DmcDmaStartTestV2, Test9) { expectFiskbit(kStartTestV2, 0x9); }
TEST(DmcDmaStartTestV2, TestA) { expectFiskbit(kStartTestV2, 0xA); }
TEST(DmcDmaStartTestV2, TestB) { expectFiskbit(kStartTestV2, 0xB); }
TEST(DmcDmaStartTestV2, TestC) { expectFiskbit(kStartTestV2, 0xC); }
TEST(DmcDmaStartTestV2, TestD) { expectFiskbit(kStartTestV2, 0xD); }
TEST(DmcDmaStartTestV2, TestE) { expectFiskbit(kStartTestV2, 0xE); }

// Fiskbit's dmc_dma_start_test, tests 0-5 as the second version's.
TEST(DmcDmaStartTest, Test0) { expectFiskbit(kStartTestV1, 0x0); }
TEST(DmcDmaStartTest, Test1) { expectFiskbit(kStartTestV1, 0x1); }
TEST(DmcDmaStartTest, Test2) { expectFiskbit(kStartTestV1, 0x2); }
TEST(DmcDmaStartTest, Test3) { expectFiskbit(kStartTestV1, 0x3); }
TEST(DmcDmaStartTest, Test4) { expectFiskbit(kStartTestV1, 0x4); }
TEST(DmcDmaStartTest, Test5) { expectFiskbit(kStartTestV1, 0x5); }
