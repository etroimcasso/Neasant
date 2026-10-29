#include "rom_harness.h"

#include "neasant/nes/clock.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <span>

namespace neasant::nes_tests {

namespace {

// One frame with rendering off (NESdev wiki, "PPU frame timing", Even/Odd Frames).
constexpr std::uint64_t kFrame = consoleClock(Region::Ntsc).masterCyclesPerFrame;

// The frames between a program's request for the reset button and the reset: the
// programs ask for a delay "of at least 100 msec" (the suites' readmes, "Output at
// $6000"); six frames are 99.8 ms at the console's rate, seven 116.5.
constexpr std::uint64_t kResetDelayFrames = 7;

// The memory report's bytes (the suites' readmes, "Output at $6000"; text_out.s in each
// suite's source writes $61 where the readmes print $G1).
constexpr std::size_t kStatus = 0;
constexpr std::array<std::uint8_t, 3> kSignature{0xDE, 0xB0, 0x61};
constexpr std::size_t kText = 4;
constexpr std::uint8_t kRunning = 0x80;
constexpr std::uint8_t kResetRequested = 0x81;

// The words the two older consoles print for their result (cpu_dummy_reads's readme:
// "Passed", "Failed", "Error <code>"; branch_timing_tests's validation.a: "PASSED",
// "FAILED: #", "INTERNAL ERROR").
constexpr std::array<const char*, 2> kPassWords{"Passed", "PASSED"};
constexpr std::array<const char*, 4> kFailWords{"Failed", "FAILED", "Error", "INTERNAL"};

// The text as the program wrote it; a byte outside the printable range, the colour codes
// of two suites among them, shows as a dot.
std::string memoryText(const NesState& state) {
  std::string text;
  for (std::size_t i = kText; i < state.prgRam.size() && state.prgRam[i] != 0; ++i) {
    const std::uint8_t byte = state.prgRam[i];
    const bool printable = byte == 10 || (byte >= 32 && byte <= 126);
    text += printable ? static_cast<char>(byte) : '.';
  }
  return text;
}

bool containsAny(const std::string& text, std::span<const char* const> words) {
  for (const char* word : words) {
    if (text.find(word) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> loadRom(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>());
}

std::string screenText(const NesState& state) {
  std::string text;
  // Both consoles print at the nametable at $2000 and both boards put it in the first
  // kilobyte of the console's RAM (NESdev wiki, "Mirroring", Nametable Mirroring: CIRAM
  // A10 from PPU A11 or PPU A10). The fonts place each glyph at the tile of its character
  // code (cpu_dummy_reads's console.s loads ascii.chr; branch_timing_tests's console.a
  // loads 59 glyphs from tile 32).
  for (std::size_t row = 0; row < 30; ++row) {
    for (std::size_t column = 0; column < 32; ++column) {
      const std::uint8_t tile = state.ppu.ciram[row * 32 + column];
      if (tile >= 32 && tile <= 126) {
        text += static_cast<char>(tile);
      } else if (tile == 0) {
        text += ' ';
      } else {
        text += '.';
      }
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    text += '\n';
  }
  return text;
}

RomResult runUntilResult(Nes& machine, Report report, std::uint64_t guardFrames) {
  RomResult result;
  std::uint64_t resetIn = 0;
  bool awaitingRunning = false;
  while (result.frames < guardFrames) {
    machine.run(kFrame);
    ++result.frames;
    const NesState& state = machine.state();

    if (report == Report::Screen) {
      result.text = screenText(state);
      if (containsAny(result.text, kPassWords)) {
        result.reported = true;
        result.passed = true;
        return result;
      }
      if (containsAny(result.text, kFailWords)) {
        result.reported = true;
        return result;
      }
      continue;
    }

    // The program writes $80 and then the signature before anything else, so a status is
    // read only once the signature stands (text_out.s, init_text_out).
    const auto& ram = state.prgRam;
    if (ram.size() <= kText || ram[1] != kSignature[0] || ram[2] != kSignature[1] ||
        ram[3] != kSignature[2]) {
      continue;
    }
    const std::uint8_t status = ram[kStatus];
    result.text = memoryText(state);

    // The reset is given after the delay the program asks for; the status keeps $81 until
    // the program's shell has run again and written $80, so a request is honoured once.
    if (resetIn != 0) {
      --resetIn;
      if (resetIn == 0) {
        machine.reset();
        awaitingRunning = true;
      }
      continue;
    }
    if (awaitingRunning) {
      if (status == kResetRequested) continue;
      awaitingRunning = false;
    }
    if (status == kResetRequested) {
      resetIn = kResetDelayFrames;
      continue;
    }
    if (status < kRunning) {
      result.reported = true;
      result.passed = status == 0;
      return result;
    }
  }
  return result;
}

}  // namespace neasant::nes_tests
