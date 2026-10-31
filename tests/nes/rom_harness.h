#pragma once

// Test support for the self-checking test programs: a ROM file read whole, a machine
// run frame by frame until the program reports, and the report read out of the
// machine's state value. The programs report in one of two ways. The newer ones write
// a status byte at $6000, the bytes $DE $B0 $61 at $6001-$6003 and their text from
// $6004 (the suites' readmes, "Output at $6000"), which the cartridge's PRG RAM holds;
// the older ones print their result on the screen (the readmes of cpu_dummy_reads and
// branch_timing_tests), which the console's nametable RAM holds.

#include "neasant/nes/nes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neasant::nes_tests {

// The bytes of a file, or nothing for a file that cannot be opened.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> loadRom(const std::string& path);

// How a program reports. Memory: the status byte at $6000, read only once the bytes
// $DE $B0 $61 stand at $6001-$6003; $80 while the program runs, $81 while it asks for
// the reset button, below $80 its result, 0 a pass and any other value the failure's
// code; the text from $6004 up to a zero byte. Screen: the text the program prints to
// the nametable at $2000, one character code a tile; "Passed" or "PASSED" is a pass,
// "Failed", "FAILED", "Error" or "INTERNAL" a failure.
enum class Report : std::uint8_t { Memory, Screen };

// What a run found: whether the program reported before the guard ran out, whether it
// reported a pass, the text it printed as far as it got, and the frames run.
struct RomResult {
  bool reported = false;
  bool passed = false;
  std::string text;
  std::uint64_t frames = 0;
};

// Runs the machine one frame at a time, 357 368 master cycles each, until the program
// reports or `guardFrames` have run. A program that asks for the reset button gets the
// reset line seven frames after it asks, 116.5 ms at the console's rate, and runs on;
// a second request counts only after the program has reported itself running again.
[[nodiscard]] RomResult runUntilResult(Nes& machine, Report report, std::uint64_t guardFrames);

// The text on the console's screen: the 32 by 30 tiles of the nametable at $2000, each
// tile index read as its character code, a row ending at its last character that is
// not a space, rows joined by newlines.
[[nodiscard]] std::string screenText(const NesState& state);

}  // namespace neasant::nes_tests
