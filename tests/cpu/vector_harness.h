#pragma once

// Test support for the SingleStepTests nes6502 vector set: a flat 64 KiB bus that
// records every access, the parsed shape of one case, and a reader for the
// per-opcode files. The reader understands exactly the format the set's README
// describes (objects, arrays, non-negative integers and strings), so the suite
// takes no JSON library.

#include "nescore/cpu/cpu6502.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nescore::cpu_vectors {

// One bus access as the core made it.
struct Access {
  std::uint16_t address;
  std::uint8_t value;
  CycleKind kind;
};

// Whether a cycle of this kind drives a write. Every other kind is a read.
[[nodiscard]] bool isWrite(CycleKind kind) noexcept;

// The whole 64 KiB address space as RAM, which is what the vector set assumes.
// Cells a case does not list read zero. Every access is appended to `trace`.
struct RecordingBus {
  std::array<std::uint8_t, 65536> mem{};
  std::vector<Access> trace;

  std::uint8_t read(std::uint16_t address, CycleKind kind) {
    trace.push_back({address, mem[address], kind});
    return mem[address];
  }
  void write(std::uint16_t address, std::uint8_t value, CycleKind kind) {
    mem[address] = value;
    trace.push_back({address, value, kind});
  }
};

// One cycle as the vectors record it: `[address, value, "read" | "write"]`.
struct CycleTrace {
  std::uint16_t address = 0;
  std::uint8_t value = 0;
  bool write = false;
};

// One side of a case: the registers and the RAM cells the case lists.
struct RegState {
  std::uint16_t pc = 0;
  std::uint8_t s = 0;
  std::uint8_t a = 0;
  std::uint8_t x = 0;
  std::uint8_t y = 0;
  std::uint8_t p = 0;
  std::vector<std::pair<std::uint16_t, std::uint8_t>> ram;
};

// One case: the state before, the state after, and every cycle of the instruction.
// The length of `cycles` is the instruction's cycle count.
struct VectorCase {
  std::string name;
  RegState initial;
  RegState final_;
  std::vector<CycleTrace> cycles;
};

// Parses every case in one file's text. Throws std::runtime_error on input outside
// the format: a null, true or false literal, a negative number, a number too wide
// for its field, a cycle direction other than "read" or "write", or broken syntax.
[[nodiscard]] std::vector<VectorCase> parseVectorFile(const std::string& text);

// Reads a whole file; nullopt if it cannot be opened.
[[nodiscard]] std::optional<std::string> readFile(const std::string& path);

}  // namespace nescore::cpu_vectors
