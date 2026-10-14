#include "neasant/cpu/cpu6502.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef NEASANT_NES6502_VECTORS
#define NEASANT_NES6502_VECTORS ""
#endif

// The core against the SingleStepTests nes6502 vector set: every case of every opcode
// file, compared cycle by cycle and on the final state. The NEASANT_NES6502_VECTORS
// build option names the directory. Two environment variables shape a run:
// NEASANT_REQUIRE_NES6502_VECTORS=1 turns a missing vector path into a failure, and
// NEASANT_NES6502_CASE_CAP=N runs at most N cases per file.

namespace {

using neasant::Cpu6502;
using neasant::Cpu6502State;
using neasant::cpu_vectors::Access;
using neasant::cpu_vectors::CycleTrace;
using neasant::cpu_vectors::isWrite;
using neasant::cpu_vectors::parseVectorFile;
using neasant::cpu_vectors::readFile;
using neasant::cpu_vectors::RecordingBus;
using neasant::cpu_vectors::VectorCase;

std::string vectorsDir() { return NEASANT_NES6502_VECTORS; }

bool vectorsRequired() {
  const char* raw = std::getenv("NEASANT_REQUIRE_NES6502_VECTORS");
  return raw != nullptr && std::string(raw) == "1";
}

// Zero (unset) runs every case; a positive value runs at most that many per file.
std::size_t caseCap() {
  const char* raw = std::getenv("NEASANT_NES6502_CASE_CAP");
  if (raw == nullptr) return 0;
  const long v = std::strtol(raw, nullptr, 10);
  return v > 0 ? static_cast<std::size_t>(v) : 0;
}

std::string opcodeFile(int opcode) {
  char name[16];
  std::snprintf(name, sizeof name, "/%02x.json", opcode);
  return vectorsDir() + name;
}

// The JAM opcodes. Their cases record eleven cycles of a halted CPU, so the core ends
// jammed rather than on an instruction boundary, and the program counter they record
// is one short of the chip's, which steps past the byte after the opcode.
bool isJam(int opcode) {
  switch (opcode) {
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
    case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
      return true;
    default:
      return false;
  }
}

std::string triple(std::uint16_t address, std::uint8_t value, bool write) {
  std::ostringstream out;
  out << '[' << address << ", " << int{value} << ", " << (write ? "write" : "read") << ']';
  return out.str();
}

// Runs one case on a bus holding only the case's initial cells, and checks the trace,
// the registers and every address the core touched. The bus is left zeroed again.
void runCase(int opcode, const VectorCase& c, RecordingBus& bus) {
  for (const auto& [address, value] : c.initial.ram) bus.mem[address] = value;

  Cpu6502 cpu(Cpu6502State{.pc = c.initial.pc,
                           .s = c.initial.s,
                           .a = c.initial.a,
                           .x = c.initial.x,
                           .y = c.initial.y,
                           .p = c.initial.p});
  for (std::size_t i = 0; i < c.cycles.size(); ++i) cpu.stepCycle(bus);

  // Every cycle, as recorded and as made.
  const std::vector<Access>& got = bus.trace;
  EXPECT_EQ(got.size(), c.cycles.size()) << c.name << ": bus accesses for " << c.cycles.size()
                                         << " cycles";
  if (got.size() == c.cycles.size()) {
    for (std::size_t i = 0; i < c.cycles.size(); ++i) {
      const CycleTrace& want = c.cycles[i];
      const bool gotWrite = isWrite(got[i].kind);
      const std::string context = c.name + ": cycle " + std::to_string(i) + ", expected " +
                                  triple(want.address, want.value, want.write) + ", got " +
                                  triple(got[i].address, got[i].value, gotWrite);
      EXPECT_EQ(got[i].address, want.address) << context;
      EXPECT_EQ(int{got[i].value}, int{want.value}) << context;
      EXPECT_EQ(gotWrite, want.write) << context;
    }
  }

  // The registers.
  const Cpu6502State& s = cpu.state();
  if (!isJam(opcode)) {
    EXPECT_EQ(int{s.pc}, int{c.final_.pc}) << c.name << ": pc";
  }
  EXPECT_EQ(int{s.s}, int{c.final_.s}) << c.name << ": s";
  EXPECT_EQ(int{s.a}, int{c.final_.a}) << c.name << ": a";
  EXPECT_EQ(int{s.x}, int{c.final_.x}) << c.name << ": x";
  EXPECT_EQ(int{s.y}, int{c.final_.y}) << c.name << ": y";
  EXPECT_EQ(int{s.p}, int{c.final_.p}) << c.name << ": p";

  // Memory: every final cell; every initial cell the final list does not repeat.
  std::unordered_set<std::uint16_t> listed;
  for (const auto& [address, value] : c.final_.ram) {
    EXPECT_EQ(int{bus.mem[address]}, int{value}) << c.name << ": ram " << address;
    listed.insert(address);
  }
  for (const auto& [address, value] : c.initial.ram) {
    if (listed.insert(address).second) {
      EXPECT_EQ(int{bus.mem[address]}, int{value}) << c.name << ": unchanged ram " << address;
    }
  }
  // The set's README: an address in neither list must not be accessed.
  for (const Access& access : got) {
    EXPECT_NE(listed.count(access.address), 0u)
        << c.name << ": accessed unlisted address " << access.address;
  }

  if (isJam(opcode)) {
    EXPECT_TRUE(s.jammed) << c.name << ": the core is not jammed";
  } else {
    EXPECT_TRUE(cpu.atInstructionBoundary())
        << c.name << ": the instruction had not finished after " << c.cycles.size()
        << " cycles";
  }

  for (const Access& access : got) bus.mem[access.address] = 0;
  for (const auto& [address, value] : c.initial.ram) bus.mem[address] = 0;
  bus.trace.clear();
}

class Cpu6502Vectors : public ::testing::TestWithParam<int> {};

// A file stops at its first failing case, so a red file reports one case in full.
TEST_P(Cpu6502Vectors, MatchesTraceAndFinalState) {
  const int opcode = GetParam();
  if (vectorsDir().empty()) {
    if (vectorsRequired()) {
      FAIL() << "NEASANT_NES6502_VECTORS is empty and NEASANT_REQUIRE_NES6502_VECTORS=1 "
                "requires the vector set; configure with -DNEASANT_NES6502_VECTORS "
                "pointing at the SingleStepTests 65x02 nes6502/v1 directory.";
    }
    GTEST_SKIP() << "NEASANT_NES6502_VECTORS is unset; point it at the SingleStepTests "
                    "65x02 nes6502/v1 directory to run the vectors.";
  }

  const std::string path = opcodeFile(opcode);
  const auto text = readFile(path);
  ASSERT_TRUE(text.has_value()) << "cannot open vector file: " << path;
  const std::vector<VectorCase> cases = parseVectorFile(*text);
  ASSERT_FALSE(cases.empty()) << "no cases in " << path;

  const std::size_t cap = caseCap();
  auto bus = std::make_unique<RecordingBus>();
  std::size_t ran = 0;
  for (const VectorCase& c : cases) {
    if (cap != 0 && ran >= cap) {
      std::printf("[case cap] %s: ran %zu of %zu cases\n", path.c_str(), ran, cases.size());
      break;
    }
    ++ran;
    runCase(opcode, c, *bus);
    if (HasFailure()) return;
  }
}

INSTANTIATE_TEST_SUITE_P(AllOpcodes, Cpu6502Vectors, ::testing::Range(0, 256),
                         [](const ::testing::TestParamInfo<int>& parameter) {
                           char label[8];
                           std::snprintf(label, sizeof label, "op%02X", parameter.param);
                           return std::string(label);
                         });

// ---- the reader ------------------------------------------------------------------

// The example case from the set's nes6502/README.md, verbatim.
const char* kReadmeExample = R"([{
	"name": "b1 71 8b",
	"initial": {
		"pc": 9023,
		"s": 240,
		"a": 47,
		"x": 162,
		"y": 170,
		"p": 170,
		"ram": [
			[9023, 177],
			[9024, 113],
			[9025, 139],
			[113, 169],
			[114, 89],
			[22867, 214],
			[23123, 37]
		]
	},
	"final": {
		"pc": 9025,
		"s": 240,
		"a": 37,
		"x": 162,
		"y": 170,
		"p": 40,
		"ram": [
			[113, 169],
			[114, 89],
			[9023, 177],
			[9024, 113],
			[9025, 139],
			[22867, 214],
			[23123, 37]
		]
	},
	"cycles": [
		[9023, 177, "read"],
		[9024, 113, "read"],
		[113, 169, "read"],
		[114, 89, "read"],
		[22867, 214, "read"],
		[23123, 37, "read"]
	]
}])";

TEST(Cpu6502VectorHarness, ReadsTheReadmeExample) {
  const auto cases = parseVectorFile(kReadmeExample);
  ASSERT_EQ(cases.size(), 1u);
  const VectorCase& c = cases.front();
  EXPECT_EQ(c.name, "b1 71 8b");

  EXPECT_EQ(c.initial.pc, 9023);
  EXPECT_EQ(c.initial.s, 240);
  EXPECT_EQ(c.initial.a, 47);
  EXPECT_EQ(c.initial.x, 162);
  EXPECT_EQ(c.initial.y, 170);
  EXPECT_EQ(c.initial.p, 170);
  ASSERT_EQ(c.initial.ram.size(), 7u);
  EXPECT_EQ(c.initial.ram[0].first, 9023);
  EXPECT_EQ(c.initial.ram[0].second, 177);
  EXPECT_EQ(c.initial.ram[6].first, 23123);
  EXPECT_EQ(c.initial.ram[6].second, 37);

  EXPECT_EQ(c.final_.pc, 9025);
  EXPECT_EQ(c.final_.s, 240);
  EXPECT_EQ(c.final_.a, 37);
  EXPECT_EQ(c.final_.x, 162);
  EXPECT_EQ(c.final_.y, 170);
  EXPECT_EQ(c.final_.p, 40);
  ASSERT_EQ(c.final_.ram.size(), 7u);
  EXPECT_EQ(c.final_.ram[0].first, 113);
  EXPECT_EQ(c.final_.ram[0].second, 169);
  EXPECT_EQ(c.final_.ram[5].first, 22867);
  EXPECT_EQ(c.final_.ram[5].second, 214);

  ASSERT_EQ(c.cycles.size(), 6u);
  const std::uint16_t addresses[] = {9023, 9024, 113, 114, 22867, 23123};
  const std::uint8_t values[] = {177, 113, 169, 89, 214, 37};
  for (std::size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(c.cycles[i].address, addresses[i]) << "cycle " << i;
    EXPECT_EQ(c.cycles[i].value, values[i]) << "cycle " << i;
    EXPECT_FALSE(c.cycles[i].write) << "cycle " << i;
  }
}

TEST(Cpu6502VectorHarness, ReadsAnEmptyTopLevelArray) {
  EXPECT_TRUE(parseVectorFile("[]").empty());
}

TEST(Cpu6502VectorHarness, ThrowsOnMalformedInput) {
  EXPECT_THROW((void)parseVectorFile("not json"), std::runtime_error);
  EXPECT_THROW((void)parseVectorFile(R"([{ "name": }])"), std::runtime_error);
  EXPECT_THROW((void)parseVectorFile(R"([{ "initial": { "pc": null } }])"), std::runtime_error);
  EXPECT_THROW((void)parseVectorFile(R"([{ "initial": { "pc": true } }])"), std::runtime_error);
  EXPECT_THROW((void)parseVectorFile(R"([{ "initial": { "a": -1 } }])"), std::runtime_error);
  EXPECT_THROW((void)parseVectorFile(R"([{ "initial": { "a": 256 } }])"), std::runtime_error);
  EXPECT_THROW((void)parseVectorFile(R"([{ "cycles": [[1, 2, "fetch"]] }])"), std::runtime_error);
  EXPECT_THROW((void)parseVectorFile("[] trailing"), std::runtime_error);
}

TEST(Cpu6502VectorHarness, ReadsAWriteRowsDirection) {
  const auto cases =
      parseVectorFile(R"([{ "cycles": [[511, 7, "write"], [512, 8, "read"]] }])");
  ASSERT_EQ(cases.size(), 1u);
  ASSERT_EQ(cases.front().cycles.size(), 2u);
  EXPECT_TRUE(cases.front().cycles[0].write);
  EXPECT_EQ(cases.front().cycles[0].address, 511);
  EXPECT_EQ(cases.front().cycles[0].value, 7);
  EXPECT_FALSE(cases.front().cycles[1].write);
}

}  // namespace
