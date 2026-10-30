#include "neasant/nes/nes.h"
#include "rom_harness.h"
#include "authored_image.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifndef NEASANT_NES_TEST_ROMS
#define NEASANT_NES_TEST_ROMS ""
#endif

// The CPU proven on the self-checking test programs: each ROM of the eight suites booted
// on the machine, run to its report and required to pass, with what it printed shown when
// it does not. The NEASANT_NES_TEST_ROMS build option names a clone of
// christopherpow/nes-test-roms; with it unset every case skips naming the option, and with
// a file absent that case skips naming the file. NEASANT_REQUIRE_NES_TEST_ROMS=1 turns
// either skip into a failure, so a run that means to prove the programs cannot report
// green while running none. A program that needs hardware the machine does not hold
// registers and skips naming what it needs; its file is still checked. One case apart from
// the programs checks the constant the machine builds its CPU with.

namespace {

using neasant::Cpu6502;
using neasant::Nes;
using neasant::NesConfig;
using neasant::powerOn;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::loadRom;
using neasant::nes_tests::placeProgram;
using neasant::nes_tests::Report;
using neasant::nes_tests::RomResult;
using neasant::nes_tests::runUntilResult;

// The guard, in frames: 30 seconds at the console's rate. The longest of these programs
// reports within seven seconds; instr_timing's readme says it takes about 25.
constexpr std::uint64_t kGuardFrames = 1800;

std::string romsDir() { return NEASANT_NES_TEST_ROMS; }

bool romsRequired() {
  const char* raw = std::getenv("NEASANT_REQUIRE_NES_TEST_ROMS");
  return raw != nullptr && std::string(raw) == "1";
}

using Bytes = std::vector<std::uint8_t>;

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
// header, runs it to its report and requires a pass.
void expectPass(const std::string& relative, Report report,
                std::optional<std::uint16_t> mapper = std::nullopt) {
  std::optional<Bytes> image;
  loadOrSkip(relative, image);
  if (!image) return;
  Nes machine(NesConfig{.rom = *image, .mapper = mapper});
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

// The constant the CPU's ANE and LXA OR into the accumulator is the config's: $FF by
// default, which blargg's instruction test measures on the 2A03, and the documents' $EE
// when stated (64doc, "6510/8502 Undocumented Commands"; No More Secrets, "'Magic
// Constant' group").
TEST(UnstableConstant, TheMachineBuildsItsCoreWithTheConfigsValue) {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  const Bytes program{0xA9, 0x00, 0xA2, 0x55, 0xAB, 0xFF};
  placeProgram(image, 0, program);
  Nes measured(NesConfig{.rom = image});
  Nes documented(NesConfig{.rom = image, .unstableConstant = 0xEE});
  EXPECT_EQ(measured.unstableConstant(), 0xFF);
  EXPECT_EQ(documented.unstableConstant(), 0xEE);
  for (int cycle = 0; cycle < 13; ++cycle) {
    measured.step();
    documented.step();
  }
  EXPECT_EQ(measured.state().cpu.a, 0xFF);
  EXPECT_EQ(measured.state().cpu.x, 0xFF);
  EXPECT_EQ(documented.state().cpu.a, 0xEE);
  EXPECT_EQ(documented.state().cpu.x, 0xEE);
  EXPECT_EQ(Cpu6502(powerOn()).unstableConstant(), 0xEE);
  EXPECT_EQ(Cpu6502(powerOn(), 0xFF).unstableConstant(), 0xFF);
}

// Dummy reads of $2002 and its mirrors by LDA and STA in the indexed modes (its readme).
// The header names mapper 3 with one 8 KB bank of CHR, which no bank select can move, so
// the machine runs the image as NROM. The program reports on the screen.
TEST(CpuDummyReads, CpuDummyReads) {
  expectPass("cpu_dummy_reads/cpu_dummy_reads.nes", Report::Screen, std::uint16_t{0});
}

// The two writes of every read-modify-write opcode, seen through $2004's auto-increment
// (its readme; the OAM version, which the readme says holds on emulators and not on the
// console).
TEST(CpuDummyWrites, CpuDummyWritesOam) {
  expectPass("cpu_dummy_writes/cpu_dummy_writes_oam.nes", Report::Memory);
}

// The two writes seen through $2006 and $2007, after the PPU's open bus is checked port by
// port (its readme).
TEST(CpuDummyWrites, CpuDummyWritesPpumem) {
  expectPass("cpu_dummy_writes/cpu_dummy_writes_ppumem.nes", Report::Memory);
}

// Code executed from $4000-$40FF, where every write-only port and the unused range answer
// the open bus (its readme).
TEST(CpuExecSpace, TestCpuExecSpaceApu) {
  expectPass("cpu_exec_space/test_cpu_exec_space_apu.nes", Report::Memory);
}

// Code executed from the PPU's ports through JSR, JMP, RTS, RTI and BRK, each one-byte
// opcode's dummy fetch of the next byte read back through $2006 (its readme).
TEST(CpuExecSpace, TestCpuExecSpacePpuio) {
  expectPass("cpu_exec_space/test_cpu_exec_space_ppuio.nes", Report::Memory);
}

// The multi-test build: 256 KB of PRG on mapper 1 (its readme, "Multi-tests").
TEST(InstrTestV5, AllInstrs) {
  skipNeeding("instr_test-v5/all_instrs.nes",
              "mapper 1, which is not built; its sub-tests run from rom_singles");
}

TEST(InstrTestV5, OfficialOnly) {
  skipNeeding("instr_test-v5/official_only.nes",
              "mapper 1, which is not built; its sub-tests run from rom_singles");
}

// The shell's own ground: interrupts stay off, PHP, PLP, PHA, JSR and RTS (its readme and
// source).
TEST(InstrTestV5, Singles01Basics) {
  expectPass("instr_test-v5/rom_singles/01-basics.nes", Report::Memory);
}

// Every implied and accumulator-mode instruction over the suite's table of register and
// flag values, by checksum (its readme, "Internal operation").
TEST(InstrTestV5, Singles02Implied) {
  expectPass("instr_test-v5/rom_singles/02-implied.nes", Report::Memory);
}

// Every immediate-mode instruction, the unofficial ones included (its readme). ATX #n is
// the core's LXA, whose constant the machine states (NesConfig::unstableConstant).
TEST(InstrTestV5, Singles03Immediate) {
  expectPass("instr_test-v5/rom_singles/03-immediate.nes", Report::Memory);
}

// Every zero-page instruction, by checksum (its readme).
TEST(InstrTestV5, Singles04ZeroPage) {
  expectPass("instr_test-v5/rom_singles/04-zero_page.nes", Report::Memory);
}

// Every zero-page indexed instruction, with the index wrapping inside the page (its
// readme).
TEST(InstrTestV5, Singles05ZpXy) {
  expectPass("instr_test-v5/rom_singles/05-zp_xy.nes", Report::Memory);
}

// Every absolute instruction, by checksum (its readme).
TEST(InstrTestV5, Singles06Absolute) {
  expectPass("instr_test-v5/rom_singles/06-absolute.nes", Report::Memory);
}

// Every absolute indexed instruction, the stores through the high byte among them (its
// readme).
TEST(InstrTestV5, Singles07AbsXy) {
  expectPass("instr_test-v5/rom_singles/07-abs_xy.nes", Report::Memory);
}

// Every (zp,X) instruction, with the pointer wrapping inside the page (its readme).
TEST(InstrTestV5, Singles08IndX) {
  expectPass("instr_test-v5/rom_singles/08-ind_x.nes", Report::Memory);
}

// Every (zp),Y instruction (its readme).
TEST(InstrTestV5, Singles09IndY) {
  expectPass("instr_test-v5/rom_singles/09-ind_y.nes", Report::Memory);
}

// The eight branches over every flag value (its readme).
TEST(InstrTestV5, Singles10Branches) {
  expectPass("instr_test-v5/rom_singles/10-branches.nes", Report::Memory);
}

// PHA, PHP, PLA, PLP, TXS and TSX with S at the page's ends (its readme).
TEST(InstrTestV5, Singles11Stack) {
  expectPass("instr_test-v5/rom_singles/11-stack.nes", Report::Memory);
}

// JMP and JSR (its readme).
TEST(InstrTestV5, Singles12JmpJsr) {
  expectPass("instr_test-v5/rom_singles/12-jmp_jsr.nes", Report::Memory);
}

// RTS (its readme).
TEST(InstrTestV5, Singles13Rts) {
  expectPass("instr_test-v5/rom_singles/13-rts.nes", Report::Memory);
}

// RTI (its readme).
TEST(InstrTestV5, Singles14Rti) {
  expectPass("instr_test-v5/rom_singles/14-rti.nes", Report::Memory);
}

// BRK, the bytes it pushes checked on the stack (its readme).
TEST(InstrTestV5, Singles15Brk) {
  expectPass("instr_test-v5/rom_singles/15-brk.nes", Report::Memory);
}

// JMP ($6FF) taking its high byte from $600, RTI's return address, BRK's pushed status and
// return address (its source).
TEST(InstrTestV5, Singles16Special) {
  expectPass("instr_test-v5/rom_singles/16-special.nes", Report::Memory);
}

// The multi-test build, on mapper 1 (its readme, "Multi-tests").
TEST(InstrTiming, InstrTiming) {
  skipNeeding("instr_timing/instr_timing.nes",
              "mapper 1, which is not built; its sub-tests run from rom_singles");
}

// Every instruction timed against the APU's length counter (its readme, "Requirements" and
// "Internal operation").
TEST(InstrTiming, Singles1InstrTiming) {
  skipNeeding("instr_timing/rom_singles/1-instr_timing.nes",
              "the APU's length counter, which is not built");
}

// The branches timed against the APU's length counter in eight situations (its readme).
TEST(InstrTiming, Singles2BranchTiming) {
  skipNeeding("instr_timing/rom_singles/2-branch_timing.nes",
              "the APU's length counter, which is not built");
}

// The multi-test build, on mapper 1 (its readme, "Multi-tests").
TEST(InstrMisc, InstrMisc) {
  skipNeeding("instr_misc/instr_misc.nes",
              "mapper 1, which is not built; its sub-tests run from rom_singles");
}

// STA abs,X and LDA abs,X wrapping from $FFFF to $0000 (its readme).
TEST(InstrMisc, Singles01AbsXWrap) {
  expectPass("instr_misc/rom_singles/01-abs_x_wrap.nes", Report::Memory);
}

// A branch across the ends of the address space (its readme).
TEST(InstrMisc, Singles02BranchWrap) {
  expectPass("instr_misc/rom_singles/02-branch_wrap.nes", Report::Memory);
}

// Dummy reads of $2002 by LDA, STA and ROL in the indexed modes, one frame after a vertical
// blank (its readme).
TEST(InstrMisc, Singles03DummyReads) {
  expectPass("instr_misc/rom_singles/03-dummy_reads.nes", Report::Memory);
}

// Dummy reads of every instruction that makes one, seen through the frame interrupt flag
// (its readme).
TEST(InstrMisc, Singles04DummyReadsApu) {
  skipNeeding("instr_misc/rom_singles/04-dummy_reads_apu.nes",
              "the APU's frame interrupt flag in $4015, which is not built");
}

// RAM kept across the reset line, which the program asks for and the harness gives (its
// readme).
TEST(CpuReset, RamAfterReset) {
  expectPass("cpu_reset/ram_after_reset.nes", Report::Memory);
}

// A, X, Y, P and S at power-on, and across the reset line I set, S down by three and the
// stack untouched (its readme, "Expected behavior").
TEST(CpuReset, Registers) {
  expectPass("cpu_reset/registers.nes", Report::Memory);
}

// Branch timing basics and the NMI period they are measured against (its readme). The
// program reports on the screen.
TEST(BranchTimingTests, Rom1BranchBasics) {
  expectPass("branch_timing_tests/1.Branch_Basics.nes", Report::Screen);
}

// Backward branches across page ends (its readme).
TEST(BranchTimingTests, Rom2BackwardBranch) {
  expectPass("branch_timing_tests/2.Backward_Branch.nes", Report::Screen);
}

// Forward branches across page ends (its readme).
TEST(BranchTimingTests, Rom3ForwardBranch) {
  expectPass("branch_timing_tests/3.Forward_Branch.nes", Report::Screen);
}
