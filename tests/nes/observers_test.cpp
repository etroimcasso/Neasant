#include "neasant/nes/nes.h"
#include "authored_image.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

// The observers, the save and determinism: the bus observer told every access as it
// settles, with its address, byte, direction, kind and source, over the reset sequence
// and an instruction mix, a register read, open bus and an OAM DMA transfer; the save
// observer told at the end of a frame in which the battery-backed PRG RAM changed and
// after a restore that changes it, never for a cartridge without a battery, with the
// terms a report throws on; the observers as the host's objects outside the state; and
// two machines built alike, run differently and restored one from the other
// mid-instruction, equal state by state and access by access. A case that checks a
// hardware rule names the document and section it is read from; the rest check the
// machine's own surface.

namespace {

using neasant::AccessSource;
using neasant::BusAccess;
using neasant::BusObserver;
using neasant::buttons;
using neasant::Joypad;
using neasant::JoypadPort;
using neasant::kCpuFlagN;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::SaveObserver;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::placeProgram;
using enum neasant::CycleKind;

using Bytes = std::vector<std::uint8_t>;
using Program = std::initializer_list<std::uint8_t>;

constexpr std::uint8_t kNop = 0xEA;

// One frame with rendering off, in master cycles: 341 x 262 dots x 4 (NESdev wiki, "PPU
// frame timing", Even/Odd Frames).
constexpr std::uint64_t kFrame = 357368;
// The master cycle at which the machine cycle holding the first frame's end closes: the
// end falls at master 357364 under alignment 0, inside the cycle that runs 357360-357372.
constexpr std::uint64_t kFirstFrameEnd = 357372;
// The same for the second frame: its end at 714732, the first master cycle of its cycle.
constexpr std::uint64_t kSecondFrameEnd = 714744;

// A 16 KB NROM image with 8 KB of CHR ROM, `program` at $8000 and every vector pointing
// there, with or without a battery behind its 8 KB of PRG RAM.
Bytes imageWith(std::span<const std::uint8_t> program, bool battery = false) {
  Bytes image = authoredImage(inesHeader({.battery = battery}), 16384, 8192);
  placeProgram(image, 0, program);
  return image;
}

// A machine over that image, built with the defaults but for the APU half the first
// cycle falls in.
Nes machineWith(const Bytes& program, bool battery = false, std::uint8_t apuPhase = 0) {
  const Bytes image = imageWith(program, battery);
  return Nes(NesConfig{.rom = image, .apuPhase = apuPhase});
}
Nes machineWith(Program program, bool battery = false, std::uint8_t apuPhase = 0) {
  return machineWith(Bytes(program), battery, apuPhase);
}

// Steps `cycles` machine cycles.
void stepCycles(Nes& machine, int cycles) {
  for (int i = 0; i < cycles; ++i) machine.step();
}

// Fills page 2 of work RAM with byte i ^ `key`, through a snapshot.
void fillPageTwo(Nes& machine, std::uint8_t key) {
  NesState s = machine.state();
  for (std::size_t i = 0; i < 256; ++i) {
    s.ram[0x0200 | i] = static_cast<std::uint8_t>(i ^ key);
  }
  machine.restore(s);
}

// A pad with the buttons whose bits are set in `bits`, bit 0 A through bit 7 Right.
Joypad padFromBits(std::uint8_t bits) {
  Joypad pad;
  for (std::size_t i = 0; i < buttons().size(); ++i) {
    pad.hold(buttons()[i], ((bits >> i) & 1u) != 0);
  }
  return pad;
}

// Every access, in order.
struct Log final : BusObserver {
  std::vector<BusAccess> accesses;
  void access(const BusAccess& access) override { accesses.push_back(access); }
};

// Every save report, with the machine's counters as they stood inside it.
struct Keeper final : SaveObserver {
  explicit Keeper(const Nes& machine) : m(machine) {}
  const Nes& m;
  int reports = 0;
  Bytes last;
  std::uint64_t masterAt = 0;
  std::uint64_t consumedAt = 0;
  void changed(std::span<const std::uint8_t> save) override {
    ++reports;
    last.assign(save.begin(), save.end());
    masterAt = m.state().master;
    consumedAt = m.state().consumed;
  }
};

// A host whose keeping of the save fails.
struct Thrower final : SaveObserver {
  int reports = 0;
  void changed(std::span<const std::uint8_t>) override {
    ++reports;
    throw std::runtime_error("the host could not keep the save");
  }
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

// ---- NesObserver ----

TEST(NesObserver, NoneIsSetAtPowerOnAndNeitherIsState) {
  Nes m = machineWith({kNop});
  EXPECT_EQ(m.observer(), nullptr);
  EXPECT_EQ(m.saveObserver(), nullptr);
  Log log;
  Keeper keeper(m);
  const NesState before = m.state();
  m.setObserver(&log);
  m.setSaveObserver(&keeper);
  EXPECT_EQ(m.observer(), &log);
  EXPECT_EQ(m.saveObserver(), &keeper);
  EXPECT_TRUE(m.state() == before);
  const Nes copy = m;
  EXPECT_EQ(copy.observer(), &log);
  EXPECT_EQ(copy.saveObserver(), &keeper);
  m.step();
  EXPECT_EQ(log.accesses.size(), 1u);
  m.restore(before);
  EXPECT_EQ(m.observer(), &log);
  EXPECT_EQ(m.saveObserver(), &keeper);
  m.setObserver(nullptr);
  m.setSaveObserver(nullptr);
  EXPECT_EQ(m.observer(), nullptr);
  EXPECT_EQ(m.saveObserver(), nullptr);
  m.step();
  EXPECT_EQ(log.accesses.size(), 1u);
  EXPECT_EQ(keeper.reports, 0);
}

TEST(NesObserver, EveryAccessIsToldInOrderWithItsKindAndByte) {
  // NESdev wiki, "CPU power up state", CPU: the reset sequence's seven cycles, S from
  // $00 to $FD; 64doc, "Immediate addressing", "Absolute addressing" and "Instructions
  // accessing the stack": the cycles of each instruction; "Open bus behavior", CPU open
  // bus: $4018 answers the last byte read; "APU", Status: bit 5 of $4015 is open bus and
  // the rest 0. The kinds are the core's (cpu6502.h).
  // LDA #$5A; STA $0200; INC $0200; PHA; PLA; LDA $4018; LDA $4015
  Nes m = machineWith({0xA9, 0x5A, 0x8D, 0x00, 0x02, 0xEE, 0x00, 0x02, 0x48, 0x68, 0xAD, 0x18,
                       0x40, 0xAD, 0x15, 0x40});
  Log log;
  m.setObserver(&log);
  stepCycles(m, 34);
  ASSERT_EQ(log.accesses.size(), 34u);
  const std::array<BusAccess, 34> expected{{
      {.address = 0x0000, .value = 0x00, .kind = DummyRead},
      {.address = 0x0000, .value = 0x00, .kind = DummyRead},
      {.address = 0x0100, .value = 0x00, .kind = DummyRead},
      {.address = 0x01FF, .value = 0x00, .kind = DummyRead},
      {.address = 0x01FE, .value = 0x00, .kind = DummyRead},
      {.address = 0xFFFC, .value = 0x00, .kind = VectorRead},
      {.address = 0xFFFD, .value = 0x80, .kind = VectorRead},
      {.address = 0x8000, .value = 0xA9, .kind = OpcodeFetch},
      {.address = 0x8001, .value = 0x5A, .kind = OperandFetch},
      {.address = 0x8002, .value = 0x8D, .kind = OpcodeFetch},
      {.address = 0x8003, .value = 0x00, .kind = OperandFetch},
      {.address = 0x8004, .value = 0x02, .kind = OperandFetch},
      {.address = 0x0200, .value = 0x5A, .write = true, .kind = DataWrite},
      {.address = 0x8005, .value = 0xEE, .kind = OpcodeFetch},
      {.address = 0x8006, .value = 0x00, .kind = OperandFetch},
      {.address = 0x8007, .value = 0x02, .kind = OperandFetch},
      {.address = 0x0200, .value = 0x5A, .kind = RmwRead},
      {.address = 0x0200, .value = 0x5A, .write = true, .kind = RmwDummyWrite},
      {.address = 0x0200, .value = 0x5B, .write = true, .kind = RmwWrite},
      {.address = 0x8008, .value = 0x48, .kind = OpcodeFetch},
      {.address = 0x8009, .value = 0x68, .kind = DummyRead},
      {.address = 0x01FD, .value = 0x5A, .write = true, .kind = StackPush},
      {.address = 0x8009, .value = 0x68, .kind = OpcodeFetch},
      {.address = 0x800A, .value = 0xAD, .kind = DummyRead},
      {.address = 0x01FC, .value = 0x00, .kind = DummyRead},
      {.address = 0x01FD, .value = 0x5A, .kind = StackPull},
      {.address = 0x800A, .value = 0xAD, .kind = OpcodeFetch},
      {.address = 0x800B, .value = 0x18, .kind = OperandFetch},
      {.address = 0x800C, .value = 0x40, .kind = OperandFetch},
      {.address = 0x4018, .value = 0x40, .kind = DataRead},
      {.address = 0x800D, .value = 0xAD, .kind = OpcodeFetch},
      {.address = 0x800E, .value = 0x15, .kind = OperandFetch},
      {.address = 0x800F, .value = 0x40, .kind = OperandFetch},
      {.address = 0x4015, .value = 0x00, .kind = DataRead},
  }};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expectAccess(log.accesses, i, expected[i]);
  }
  EXPECT_EQ(m.state().cpu.a, 0x00);
  EXPECT_EQ(m.state().cpu.s, 0xFD);
  EXPECT_EQ(m.state().ram[0x200], 0x5B);
  EXPECT_EQ(m.state().mdr, 0x40);  // the $4015 read left it
}

TEST(NesObserver, ARegisterReadIsToldAfterItsSideEffect) {
  // NESdev wiki, "PPU registers", PPUSTATUS: a read clears the vertical-blank flag, which
  // is set at power-on by default ("PPU power up state"). Inside the report the flag is
  // already clear, the byte told carries it set, and the CPU is as the cycle began.
  // BIT $2002
  struct Watcher final : BusObserver {
    explicit Watcher(const Nes& machine) : m(machine) {}
    const Nes& m;
    std::optional<bool> vblankAtRead;
    std::uint8_t told = 0;
    std::uint8_t tcuAtRead = 0;
    void access(const BusAccess& access) override {
      if (access.address != 0x2002) return;
      vblankAtRead = m.state().ppu.vblank;
      told = access.value;
      tcuAtRead = m.state().cpu.tcu;
    }
  };
  Nes m = machineWith({0x2C, 0x02, 0x20, kNop});
  Watcher watcher(m);
  m.setObserver(&watcher);
  stepCycles(m, 11);
  ASSERT_TRUE(watcher.vblankAtRead.has_value());
  EXPECT_FALSE(*watcher.vblankAtRead);
  EXPECT_EQ(watcher.told, 0x80);
  EXPECT_EQ(watcher.tcuAtRead, 3);  // the operand fetches done, the read not yet mirrored
  EXPECT_NE(m.state().cpu.p & kCpuFlagN, 0);
  EXPECT_FALSE(m.state().ppu.vblank);
}

TEST(NesObserver, TheOamDmaUnitsAccessesCarryItsSource) {
  // NESdev wiki, "DMA", Behavior, OAM DMA and Register conflicts: the halt cycle and the
  // alignment cycle are the CPU's read repeated, then 256 get/put pairs are the unit's,
  // then the CPU performs the read it was halted on. The write to $4014 is the 13th
  // access; with the halt on a put cycle the pairs follow it at once, with the halt on a
  // get cycle an alignment cycle comes between.
  // LDA #$02; STA $4014; NOP; NOP
  const Bytes program{0xA9, 0x02, 0x8D, 0x14, 0x40, kNop, kNop};
  Nes put = machineWith(program);
  fillPageTwo(put, 0x5A);
  Log putLog;
  put.setObserver(&putLog);
  stepCycles(put, 527);
  ASSERT_EQ(putLog.accesses.size(), 527u);
  expectAccess(putLog.accesses, 12,
               {.address = 0x4014, .value = 0x02, .write = true, .kind = DataWrite});
  expectAccess(putLog.accesses, 13, {.address = 0x8005, .value = kNop, .kind = Halted});
  for (std::size_t k = 0; k < 256; ++k) {
    const auto byte = static_cast<std::uint8_t>(k ^ 0x5A);
    expectAccess(putLog.accesses, 14 + 2 * k,
                 {.address = static_cast<std::uint16_t>(0x0200 + k),
                  .value = byte,
                  .kind = DataRead,
                  .source = AccessSource::OamDma});
    expectAccess(putLog.accesses, 15 + 2 * k,
                 {.address = 0x2004,
                  .value = byte,
                  .write = true,
                  .kind = DataWrite,
                  .source = AccessSource::OamDma});
  }
  expectAccess(putLog.accesses, 526, {.address = 0x8005, .value = kNop, .kind = OpcodeFetch});

  Nes get = machineWith(program, false, 1);
  fillPageTwo(get, 0x5A);
  Log getLog;
  get.setObserver(&getLog);
  stepCycles(get, 528);
  ASSERT_EQ(getLog.accesses.size(), 528u);
  expectAccess(getLog.accesses, 13, {.address = 0x8005, .value = kNop, .kind = Halted});
  expectAccess(getLog.accesses, 14, {.address = 0x8005, .value = kNop, .kind = Halted});
  expectAccess(getLog.accesses, 15,
               {.address = 0x0200,
                .value = 0x5A,
                .kind = DataRead,
                .source = AccessSource::OamDma});
  expectAccess(getLog.accesses, 526,
               {.address = 0x2004,
                .value = 0xA5,
                .write = true,
                .kind = DataWrite,
                .source = AccessSource::OamDma});
  expectAccess(getLog.accesses, 527, {.address = 0x8005, .value = kNop, .kind = OpcodeFetch});
}

TEST(NesObserver, UnsetTheMachineRunsTheSame) {
  Nes watched = machineWith({kNop});
  Nes alone = machineWith({kNop});
  Log log;
  watched.setObserver(&log);
  EXPECT_EQ(watched.run(10000), 10008u);
  EXPECT_EQ(alone.run(10000), 10008u);
  EXPECT_TRUE(watched.state() == alone.state());
  EXPECT_EQ(log.accesses.size(), 834u);  // one access a cycle
}

// ---- NesSave ----

TEST(NesSave, TheSaveIsThePrgRamOfACartridgeWithABattery) {
  // NESdev wiki, "iNES", Flags 6: bit 1 is the battery; "NROM", Banks: 8 KB of PRG RAM.
  EXPECT_EQ(machineWith({kNop}, true).saveDataSize(), 8192u);
  EXPECT_EQ(machineWith({kNop}).saveDataSize(), 0u);
  const Bytes plain = imageWith(Bytes{kNop});
  const Bytes battery = imageWith(Bytes{kNop}, true);
  EXPECT_EQ(Nes(NesConfig{.rom = plain, .battery = true}).saveDataSize(), 8192u);
  EXPECT_EQ(Nes(NesConfig{.rom = battery, .prgRamBytes = 0}).saveDataSize(), 0u);
  EXPECT_EQ(Nes(NesConfig{.rom = battery, .prgRamBytes = 2048}).saveDataSize(), 2048u);
  EXPECT_EQ(Nes(NesConfig{.rom = battery, .battery = false}).saveDataSize(), 0u);
}

TEST(NesSave, ToldAtTheEndOfTheFrameThatChangedIt) {
  // NESdev wiki, "PPU frame timing", Even/Odd Frames: a frame is 89 342 dots; the first
  // frame's end falls in the machine cycle that closes at master 357 372 and the
  // second's in the one that closes at 714 744. The program stores once and then jumps
  // to itself, so the second frame changes nothing and is not told.
  // LDA #$33; STA $6000; JMP $8005
  const Bytes program{0xA9, 0x33, 0x8D, 0x00, 0x60, 0x4C, 0x05, 0x80};
  Nes a = machineWith(program, true);
  Keeper keeperA(a);
  a.setSaveObserver(&keeperA);
  EXPECT_EQ(a.run(kFrame), kFirstFrameEnd);
  EXPECT_EQ(keeperA.reports, 1);
  EXPECT_EQ(keeperA.masterAt, kFirstFrameEnd);
  EXPECT_EQ(keeperA.consumedAt, kFrame);
  ASSERT_EQ(keeperA.last.size(), 8192u);
  EXPECT_EQ(keeperA.last[0], 0x33);
  EXPECT_EQ(keeperA.last[1], 0x00);
  EXPECT_EQ(a.run(kFrame), kFrame + 4);
  EXPECT_EQ(a.state().master, kSecondFrameEnd);
  EXPECT_EQ(keeperA.reports, 1);

  // An observer set after the first frame is told at the second's end of the store.
  Nes b = machineWith(program, true);
  Keeper keeperB(b);
  b.run(kFrame);
  b.setSaveObserver(&keeperB);
  EXPECT_EQ(keeperB.reports, 0);
  b.run(kFrame);
  EXPECT_EQ(keeperB.reports, 1);
  EXPECT_EQ(keeperB.masterAt, kSecondFrameEnd);
  EXPECT_EQ(keeperB.last[0], 0x33);
}

TEST(NesSave, ToldOnceAFrameHoweverManyStoresLanded) {
  // NESdev wiki, "PPU frame timing", Even/Odd Frames. The loop changes the byte every
  // nine cycles; one report a frame, with the save as it then stands, from run() and
  // from the step() that closes the frame's cycle.
  // INC $6000; JMP $8000
  Nes m = machineWith({0xEE, 0x00, 0x60, 0x4C, 0x00, 0x80}, true);
  Keeper keeper(m);
  m.setSaveObserver(&keeper);
  stepCycles(m, 29780);
  EXPECT_EQ(keeper.reports, 0);
  EXPECT_EQ(m.step(), 12u);
  EXPECT_EQ(keeper.reports, 1);
  EXPECT_EQ(keeper.masterAt, kFirstFrameEnd);
  EXPECT_EQ(keeper.consumedAt, kFirstFrameEnd);
  EXPECT_EQ(keeper.last[0], m.state().prgRam[0]);
  EXPECT_NE(keeper.last[0], 0x00);
  m.run(kFrame - 4);
  EXPECT_EQ(m.state().master, kSecondFrameEnd);
  EXPECT_EQ(keeper.reports, 2);
  EXPECT_EQ(keeper.masterAt, kSecondFrameEnd);
  EXPECT_EQ(keeper.last[0], m.state().prgRam[0]);
}

TEST(NesSave, NotToldForACartridgeWithoutABattery) {
  // NESdev wiki, "iNES", Flags 6: without the battery bit the PRG RAM is not a save.
  // INC $6000; JMP $8000
  Nes m = machineWith({0xEE, 0x00, 0x60, 0x4C, 0x00, 0x80});
  Keeper keeper(m);
  m.setSaveObserver(&keeper);
  m.run(2 * kFrame);
  EXPECT_NE(m.state().prgRam[0], 0x00);
  EXPECT_EQ(keeper.reports, 0);
  NesState s = m.state();
  s.prgRam[0] = 0x99;
  m.restore(s);
  EXPECT_EQ(keeper.reports, 0);
}

TEST(NesSave, ToldAfterARestoreThatChangesIt) {
  Nes m = machineWith({kNop}, true);
  Keeper keeper(m);
  m.setSaveObserver(&keeper);
  NesState s = m.state();
  s.prgRam[5] = 0x77;
  m.restore(s);
  EXPECT_EQ(keeper.reports, 1);
  EXPECT_EQ(keeper.last[5], 0x77);
  EXPECT_EQ(keeper.masterAt, 0u);
  m.restore(s);
  EXPECT_EQ(keeper.reports, 1);
  s.prgRam[5] = 0x78;
  m.restore(s);
  EXPECT_EQ(keeper.reports, 2);
  EXPECT_EQ(keeper.last[5], 0x78);
  m.run(kFrame);
  EXPECT_EQ(keeper.reports, 2);

  // The measure is the save as last handed, not the save before the restore: a restore
  // made with no observer set is told when one is, at the next restore or frame's end.
  Nes n = machineWith({kNop}, true);
  Keeper late(n);
  NesState t = n.state();
  t.prgRam[0] = 0x01;
  n.restore(t);
  n.setSaveObserver(&late);
  EXPECT_EQ(late.reports, 0);
  n.restore(t);
  EXPECT_EQ(late.reports, 1);
  EXPECT_EQ(late.last[0], 0x01);
  n.run(kFrame);
  EXPECT_EQ(late.reports, 1);
}

TEST(NesSave, AReportMayThrowAndTheRestOfTheBudgetIsOwed) {
  // The first frame's report throws out of a run() of a frame and 1200 more master
  // cycles: the machine stands at the end of the frame's cycle, the budget is accounted
  // for whole, the next run() spends the 1200 owed, and the report is not made again.
  // LDA #$33; STA $6000; JMP $8005
  const Bytes program{0xA9, 0x33, 0x8D, 0x00, 0x60, 0x4C, 0x05, 0x80};
  Nes a = machineWith(program, true);
  Thrower thrower;
  a.setSaveObserver(&thrower);
  EXPECT_THROW(a.run(kFrame + 1200), std::runtime_error);
  EXPECT_EQ(thrower.reports, 1);
  EXPECT_EQ(a.state().master, kFirstFrameEnd);
  EXPECT_EQ(a.state().consumed, kFrame + 1200);
  EXPECT_EQ(a.state().prgRam[0], 0x33);
  EXPECT_EQ(a.run(0), 1200u);
  EXPECT_EQ(a.state().master, kFirstFrameEnd + 1200);
  EXPECT_EQ(thrower.reports, 1);
  EXPECT_EQ(a.run(kFrame), kFrame + 4);
  EXPECT_EQ(a.state().master, 2 * kFrame + 1200 + 8);
  EXPECT_EQ(thrower.reports, 1);

  // Out of step(): the cycle and its twelve master cycles are accounted for first.
  Nes b = machineWith(program, true);
  Thrower stepped;
  b.setSaveObserver(&stepped);
  stepCycles(b, 29780);
  EXPECT_THROW(b.step(), std::runtime_error);
  EXPECT_EQ(stepped.reports, 1);
  EXPECT_EQ(b.state().master, kFirstFrameEnd);
  EXPECT_EQ(b.state().consumed, kFirstFrameEnd);
  EXPECT_EQ(b.step(), 12u);
  EXPECT_EQ(b.state().master, kFirstFrameEnd + 12);
  EXPECT_EQ(stepped.reports, 1);
}

// ---- NesDeterminism ----

TEST(NesDeterminism, TwoMachinesRunDifferentlyAgreeEveryFrameAndAfterARestore) {
  // The program strobes the pads, reads each one bit by bit into $10 and $11, writes
  // them and a frame count to the nametable through $2006 and $2007, stores the count
  // in the save, starts an OAM DMA transfer from page 2 and waits for the vertical-blank
  // flag. Both machines are handed the same pads before every frame; one runs a frame
  // at a time, the other ten thousand cycles stepped and the rest of the frame run.
  const Bytes program{
      0xA9, 0x01, 0x8D, 0x16, 0x40,        // LDA #$01; STA $4016
      0xA9, 0x00, 0x8D, 0x16, 0x40,        // LDA #$00; STA $4016
      0xA2, 0x08,                          // LDX #$08
      0xAD, 0x16, 0x40, 0x4A, 0x66, 0x10,  // LDA $4016; LSR A; ROR $10
      0xCA, 0xD0, 0xF7,                    // DEX; BNE $800C
      0xA2, 0x08,                          // LDX #$08
      0xAD, 0x17, 0x40, 0x4A, 0x66, 0x11,  // LDA $4017; LSR A; ROR $11
      0xCA, 0xD0, 0xF7,                    // DEX; BNE $8017
      0xA9, 0x20, 0x8D, 0x06, 0x20,        // LDA #$20; STA $2006
      0xA9, 0x00, 0x8D, 0x06, 0x20,        // LDA #$00; STA $2006
      0xA5, 0x10, 0x8D, 0x07, 0x20,        // LDA $10; STA $2007
      0xA5, 0x11, 0x8D, 0x07, 0x20,        // LDA $11; STA $2007
      0xE6, 0x12, 0xA5, 0x12,              // INC $12; LDA $12
      0x8D, 0x07, 0x20, 0x8D, 0x00, 0x60,  // STA $2007; STA $6000
      0xA9, 0x02, 0x8D, 0x14, 0x40,        // LDA #$02; STA $4014
      0x2C, 0x02, 0x20, 0x10, 0xFB,        // BIT $2002; BPL $8043
      0x4C, 0x00, 0x80,                    // JMP $8000
  };
  Nes a = machineWith(program, true);
  Nes b = machineWith(program, true);
  ASSERT_TRUE(a.state() == b.state());
  for (int frame = 0; frame < 60; ++frame) {
    const auto one = static_cast<std::uint8_t>(frame * 0x35 + 0x11);
    const auto two = static_cast<std::uint8_t>(frame * 0x5B);
    for (Nes* m : {&a, &b}) {
      m->setJoypad(JoypadPort::One, padFromBits(one));
      m->setJoypad(JoypadPort::Two, padFromBits(two));
    }
    a.run(kFrame);
    stepCycles(b, 10000);
    b.run(kFrame - 120000);
    ASSERT_TRUE(a.state() == b.state()) << "frame " << frame;
  }
  EXPECT_EQ(a.state().master, 60 * kFrame);
  EXPECT_NE(a.state().ram[0x12], 0x00);
  EXPECT_EQ(a.state().prgRam[0], a.state().ram[0x12]);

  // The second restored from the first mid-instruction; then one frame each, every
  // access and every save report compared.
  do {
    a.step();
  } while (a.state().cpu.tcu == 0);
  b.restore(a.state());
  Log logA;
  Log logB;
  Keeper keeperA(a);
  Keeper keeperB(b);
  a.setObserver(&logA);
  b.setObserver(&logB);
  a.setSaveObserver(&keeperA);
  b.setSaveObserver(&keeperB);
  a.run(kFrame);
  stepCycles(b, 10000);
  b.run(kFrame - 120000);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(logA.accesses.size(), 29781u);
  EXPECT_TRUE(logA.accesses == logB.accesses);
  EXPECT_EQ(keeperA.reports, 1);
  EXPECT_EQ(keeperB.reports, 1);
  EXPECT_TRUE(keeperA.last == keeperB.last);
}

}  // namespace
