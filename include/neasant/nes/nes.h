#pragma once

// The NES as a machine around the 6502: the master clock and its budget, the bus as the
// console decodes it, the 2 KB of work RAM, the open bus, the PPU with its ports, its
// memories, its frame and its NMI output, the APU's registers as the bytes written to
// them, the two controller ports with the pads plugged into them, the OAM DMA unit on
// the CPU's RDY line, and the cartridge on its board behind the pins.
//
// A machine is built from one config value and is at power-on from then: the CPU's
// reset sequence is pending, every RAM holds the seed, and the first seven cycles run
// the sequence to the reset vector. step() runs one machine cycle; run() spends a
// master-cycle budget in whole machine cycles and carries the overshoot, so run(a) then
// run(b) lands where run(a + b) does. One machine cycle is one CPU cycle, twelve master
// cycles: the PPU runs every dot whose master cycle falls inside it, one every four,
// the cycle's one bus access lands after the dots that fall before M2 rises, and the
// CPU samples the PPU's NMI output as the cycle ends. The access is the CPU's, or the
// OAM DMA unit's get or put while a transfer holds the bus.
//
// The whole mutable machine is one state value: state() answers it by reference,
// restore() replaces it, and a machine is copied whole. The image, the region and the
// alignment are the machine's fixed identity and are not in the state. A host that
// wants to see the bus rather than the state sets a bus observer, told every access as
// it settles; a host keeping a save sets a save observer, told when the cartridge's
// battery-backed PRG RAM has changed. Both are the host's objects, outside the state.

#include "neasant/cpu/cpu6502.h"
#include "neasant/nes/board.h"
#include "neasant/nes/cartridge.h"
#include "neasant/nes/clock.h"
#include "neasant/ppu/ppu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace neasant {

// How a machine is built. Every field has a default; the image is copied in, so the
// span need not outlive the call. Construction throws std::invalid_argument, naming
// what is supported instead, for a region other than NTSC, an alignment above 3, an
// apuPhase above 1, and for everything makeCartridge and boardFor refuse.
struct NesConfig {
  std::span<const std::uint8_t> rom{};  // an iNES or NES 2.0 image, header included
  Region region = Region::Ntsc;         // NTSC; PAL and Dendy are refused
  // The master cycles from the start of the first CPU cycle to the PPU's first dot, 0
  // to 3, which the hardware settles by chance at power-on (NESdev wiki, "PPU frame
  // timing", CPU-PPU Clock Alignment); every later dot is four master cycles on.
  std::uint8_t alignment = 0;
  // Which half of the APU cycle the first CPU cycle falls in, 0 a get and 1 a put,
  // which the hardware settles by chance at power-on (NESdev wiki, "DMA", Summary and
  // Cadence).
  std::uint8_t apuPhase = 0;
  // Whether the PPU's vertical-blank flag is set at power-on, which the hardware
  // settles by chance; the flag is usually found set (NESdev wiki, "PPU power up
  // state", Initial Register Values and the note on the VBL flag).
  bool vblankAtPowerOn = true;
  // Master cycles a bit of the PPU's I/O bus keeps the value an access drove onto it
  // before it decays to zero; 0 keeps it for ever. The hardware's time varies with the
  // chip and its warmth (NESdev forum, "Tests for ACE speedruns that rely on PPU's
  // internal open bus"); the default is the thread's reading for an NTSC NES at
  // power-on, $A00 counts of 15 CPU cycles, 38 400 CPU cycles.
  std::uint32_t ppuOpenBusDecay = 460800;
  // The pattern every RAM is filled with at power-on, repeated from its start; empty
  // fills with $00. The hardware leaves RAM unreliable at power-on (NESdev wiki, "CPU
  // power up state", RAM contents) and the PPU's memories unspecified (NESdev wiki,
  // "PPU power up state"). A palette entry keeps six bits of its byte.
  std::span<const std::uint8_t> ramSeed{};
  // Overrides on the header, each taken when set (CartridgeOverrides in cartridge.h).
  std::optional<std::uint16_t> mapper{};
  std::optional<Mirroring> mirroring{};
  std::optional<std::size_t> prgRamBytes{};  // 0 for none
  std::optional<std::size_t> chrRamBytes{};  // 0 for none
  std::optional<bool> battery{};
};

// A standard controller's eight buttons, in the order the pad shifts them out on its
// port's data line (NESdev wiki, "Standard controller", Report). The set is the
// controller's, so it is fixed: a controller with other buttons is a different
// controller, not a longer list.
enum class Button : std::uint8_t { A, B, Select, Start, Up, Down, Left, Right };
inline constexpr std::size_t kButtonCount = 8;

// Every button, in that order, for a caller walking the set.
[[nodiscard]] std::span<const Button> buttons() noexcept;

// What a button is called, "a", "select", "left", or an empty view for a value past
// the last button; and the button a name stands for, or nothing for a word that names
// none, read in any case. This is the one place the eight are spelled: a tool that
// reads a button out of a file and a host that prints one both ask here, so no two of
// them can disagree.
[[nodiscard]] std::string_view buttonName(Button button) noexcept;
[[nodiscard]] std::optional<Button> buttonFromName(std::string_view name) noexcept;

// A standard controller's eight buttons as a value: true is pressed. A pad is presented
// to the machine with Nes::setJoypad and held in its state; the program reads it
// through its port's register one button a read, after strobing the ports (NESdev
// wiki, "Controller reading"). A pad with nothing pressed is not an empty port:
// setJoypad takes an optional, and nothing at all is a socket with no controller in
// it, which a program can tell apart from this.
struct Joypad {
  bool a = false;
  bool b = false;
  bool select = false;
  bool start = false;
  bool up = false;
  bool down = false;
  bool left = false;
  bool right = false;

  // One button by name, read and written, so a caller working from the Button set
  // does not repeat the eight fields to reach them.
  [[nodiscard]] bool holds(Button button) const noexcept;
  void hold(Button button, bool pressed) noexcept;

  // The eight bits the pad shifts out: bit 0 is A, the first bit on the line, up to
  // bit 7 for Right. A set bit is a pressed button, which is how the port reads it
  // (NESdev wiki, "Controller reading", $4016 / $4017 Read).
  [[nodiscard]] std::uint8_t bits() const noexcept;

  [[nodiscard]] bool operator==(const Joypad&) const noexcept = default;
};

// The two controller ports on the console's front.
enum class JoypadPort : std::uint8_t { One, Two };

// Who made a bus access. The CPU drives the bus on every cycle of its own, the cycles
// it is halted on included; the OAM DMA unit makes its gets and puts while the CPU is
// held off the bus (NESdev wiki, "DMA", Behavior). DmcDma names the DMC's sample
// fetch; the DMC is not built, so no access carries it.
enum class AccessSource : std::uint8_t { Cpu, OamDma, DmcDma };

// One access as the machine made it: the address driven, the byte that crossed the bus
// (what the bus answered for a read, what the source drove for a write), which way it
// went, what the cycle was for as the CPU core drives it (CycleKind, cpu6502.h), and
// who made it. The OAM DMA unit's accesses are plain data reads and writes; a read the
// halted CPU repeats carries CycleKind::Halted.
struct BusAccess {
  std::uint16_t address = 0;
  std::uint8_t value = 0;
  bool write = false;
  CycleKind kind = CycleKind::DataRead;
  AccessSource source = AccessSource::Cpu;
  [[nodiscard]] bool operator==(const BusAccess&) const noexcept = default;
};

// What a machine tells about every access it makes, one each machine cycle. The 6502
// drives the bus on every cycle, so the accesses from one opcode fetch to the next are
// that instruction's cycles, together with any an OAM DMA transfer puts among them: the
// reads the halted CPU repeats, of kind CycleKind::Halted, and the unit's gets and puts,
// of source AccessSource::OamDma. From the stream a host derives which addresses were
// read and written, in what order, with what byte, and by whom.
class BusObserver {
 public:
  virtual ~BusObserver() = default;

  // An access that crossed the bus, after its value is settled: a register's read side
  // effect has happened when the read is told. Inside the report state() holds the
  // memories and the registers as the access left them and the CPU as the cycle began.
  // The report is made inside the machine cycle, between the access and the dots that
  // follow it, so it does not throw: a throw would leave the cycle half run.
  virtual void access(const BusAccess& access) = 0;
};

// What a machine tells about the cartridge's battery-backed PRG RAM, the save: that it
// has changed, and what it holds now. The machine says when and hands over the bytes;
// where a save is kept, and whether it is written at all, is the host's.
class SaveObserver {
 public:
  virtual ~SaveObserver() = default;

  // The save differs from the bytes the machine last handed a save observer, or from the
  // save as the machine was built when none has been handed yet, and `save` is the whole
  // of it as it now stands, the size the cartridge declares. Told at the end of the
  // machine cycle in which a frame ended, however many stores the frame made, and once
  // by a restore() that leaves the save other than as last handed; not at all while the
  // bytes are as last handed. The span is the machine's own storage and is valid for
  // the call.
  //
  // A report arrives from inside step() or run(), after the cycle has closed and with
  // state() coherent at its end, or from inside restore(), with the state in place. It
  // may throw: the call returns through the throw with the machine so, and the report
  // is not made again. A run() interrupted this way has accounted for its whole
  // budget, so the next run() spends the remainder before its own and run(a) then
  // run(b) still lands where run(a + b) does.
  virtual void changed(std::span<const std::uint8_t> save) = 0;
};

// Where the OAM DMA unit is in a transfer (NESdev wiki, "DMA", Behavior and OAM DMA).
enum class DmaStage : std::uint8_t {
  Idle,  // no transfer
  Halt,  // a write to $4014 landed: RDY is held low until a read cycle halts the CPU
  Get,   // the next get cycle reads a byte of the page; a put cycle before it is an alignment cycle
  Put,   // this cycle writes the byte read to $2004
};

// The OAM DMA unit's state.
struct Dma {
  // The page a transfer copies from: the byte most recently written to $4014 (NESdev
  // wiki, "DMA", OAM DMA).
  std::uint8_t page = 0;
  DmaStage stage = DmaStage::Idle;
  std::uint8_t index = 0;  // the byte of the page the next get reads
  std::uint8_t byte = 0;   // the byte the last get read, carried to its put
  // Whether the DMC has a sample byte to fetch, whose get takes precedence over the OAM
  // unit's and pauses it (NESdev wiki, "DMA", DMC DMA during OAM DMA). The DMC is not
  // built, so nothing sets it.
  bool dmcRequested = false;
  [[nodiscard]] bool operator==(const Dma&) const noexcept = default;
};

// The whole mutable machine as a value: snapshot by copy, restore by assignment. The
// image, the region and the alignment are fixed identity and live in the machine, so
// restore() replaces everything here and keeps the cartridge in place.
struct NesState {
  Cpu6502State cpu{};
  // The 2 KB of work RAM at $0000-$07FF, repeated through $1FFF.
  std::array<std::uint8_t, 2048> ram{};
  // The last byte the CPU's data bus carried, which a read nothing drives answers
  // (NESdev wiki, "Open bus behavior", CPU open bus).
  std::uint8_t mdr = 0;
  std::uint64_t master = 0;  // the free-running master-cycle counter
  // The master-cycle point the calls so far have accounted for: run() moves it on by
  // its budget as the call begins and step() by the twelve master cycles it ran. master
  // - consumed is the overshoot the next run() is credited; inside a run(), and after
  // one a save report threw out of, consumed stands ahead of master by what the budget
  // still owes.
  std::uint64_t consumed = 0;
  // The master cycle the PPU's next dot falls on: the alignment at power-on, then four
  // more for every dot (NESdev wiki, "PPU frame timing", CPU-PPU Clock Alignment: the
  // PPU divides the master clock by four, and a CPU cycle begins 0 to 3 master cycles
  // before its nearest following dot).
  std::uint64_t dotClock = 0;
  // Which half of the APU cycle the next machine cycle is, 0 a get and 1 a put; it
  // toggles every machine cycle (NESdev wiki, "DMA", Cadence).
  std::uint8_t apuPhase = 0;
  // $4000-$4017 as last written, for the registers the APU holds: $4000-$4013, $4015
  // and $4017. The bytes at $4014 and $4016 are never written: those addresses are the
  // OAM DMA page and the controller strobe, held below.
  std::array<std::uint8_t, 24> apuRegs{};
  PpuState ppu{};
  // The cartridge's PRG RAM and CHR RAM, each sized at construction from the header or
  // the config and never resized; the PRG RAM is the save when the cartridge has a
  // battery.
  std::vector<std::uint8_t> prgRam{};
  std::vector<std::uint8_t> chrRam{};
  Board board{};  // the board's own state
  std::array<std::optional<Joypad>, 2> pads{};  // what is plugged into each port
  // $4016 bit 0 as last written: the controllers' strobe line (NESdev wiki, "Controller
  // reading", $4016 Write). While it is high each pad's shift register fills from the
  // buttons held, and the write that drops it leaves the buttons held at that moment.
  bool strobe = false;
  // Per port, the pad's shift register: bit 0 is the bit a read of the port's register
  // answers, and each clock of the port moves the next bit down and a 1 in at the top,
  // so from the ninth read on the pad answers 1 (NESdev wiki, "Standard controller",
  // Report and Hardware; "Controller reading", Clock timing). An empty socket's register
  // holds 0.
  std::array<std::uint8_t, 2> shift{};
  // Per port, whether its clock line is low: a read of the port's register was the
  // previous cycle's access. The line is low through a read of the register and stays
  // low across adjacent cycles that read it again, so one contiguous run of reads is
  // one clock; it rises, and the pad shifts, on the first access that is not such a
  // read (NESdev wiki, "Controller reading", Clock timing; "DMA", Register conflicts).
  std::array<bool, 2> clockLow{};
  Dma dma{};
  [[nodiscard]] bool operator==(const NesState&) const noexcept = default;
};

class Nes {
 public:
  // Builds the machine at power-on: the CPU with its reset sequence pending, every RAM
  // holding the seed, the APU's registers zero (NESdev wiki, "CPU power up state",
  // APU), the open bus zero, the PPU with its registers at their power-on values, its
  // vertical-blank flag as the config says, its warm-up window open and its counter at
  // the first dot (NESdev wiki, "PPU power up state"), the ports empty with their
  // registers clear, the OAM DMA unit idle, no observer set, and the master counter at
  // zero. The first seven cycles run the reset sequence to the reset vector. Throws
  // std::invalid_argument as NesConfig says. A machine is copied and moved whole: the
  // state, the image, the board and the observer pointers go with it.
  explicit Nes(NesConfig config);

  // The whole machine as a value. state() answers the machine's own state without
  // copying it, coherent between calls at any cycle, mid-instruction included.
  // restore() replaces the mutable machine and resumes exactly there, keeping the
  // image, the region, the alignment and the observers. It throws
  // std::invalid_argument, and leaves the machine as it was, for a state whose PRG RAM
  // or CHR RAM is another size than this machine's or whose board is another kind:
  // those are the cartridge's, fixed for the machine's life, so such a state is a
  // snapshot of another cartridge. A restore that leaves the save other than as the
  // save observer last received it is reported to that observer, with the state in
  // place.
  [[nodiscard]] const NesState& state() const noexcept { return state_; }
  void restore(const NesState& state);

  // The console's reset line, pulled and let go, which resets the CPU and the PPU
  // together as a front-loading console's does (NESdev wiki, "PPU power up state"). The
  // CPU's reset sequence is pending again and runs on the next cycles, keeping A, X, Y
  // and every flag but I, and a jammed core is released (NESdev wiki, "CPU power up
  // state", CPU). A transfer the OAM DMA unit has under way ends there and the CPU is
  // released (NESdev wiki, "CPU pinout", /RST). The APU's status register goes to 0 and
  // the DMC's direct load keeps its low bit; every other APU register keeps its byte
  // (NESdev wiki, "CPU power up state", APU; "APU", Status). The PPU is as ppuReset
  // leaves it: its counter back at the top of the picture and its warm-up window open,
  // its flag, its address and its memories kept. Work RAM, PRG RAM, CHR RAM, the open
  // bus, the strobe, the shift registers, the pads, the DMA page, the master counters
  // and the observers keep what they hold. Call it between step() and run() calls.
  void reset();

  // The region the machine was built for, the CPU-PPU alignment it was built with and
  // the I/O bus decay it was built with, in master cycles, all fixed for its life like
  // the image.
  [[nodiscard]] Region region() const noexcept { return region_; }
  [[nodiscard]] std::uint8_t alignment() const noexcept { return alignment_; }
  [[nodiscard]] std::uint32_t ppuOpenBusDecay() const noexcept { return ppuOpenBusDecay_; }

  // Runs one machine cycle, one CPU cycle, and returns its master cycles, twelve: the
  // PPU runs the dots that fall inside the cycle, one bus access lands among them, and
  // the CPU samples the PPU's NMI output as the cycle ends. The access is the CPU's;
  // while the OAM DMA unit holds the bus it is the unit's get or put, and on the unit's
  // halt and alignment cycles the halted CPU repeats its read. A jammed core repeats
  // its idle read. A fresh machine's first seven cycles are the reset sequence. The
  // overshoot run() carries is unchanged. A cycle that ends a frame is followed by the
  // save observer's report, which may throw (SaveObserver) after the cycle and its
  // twelve master cycles are accounted for.
  std::uint64_t step();

  // Runs whole machine cycles until the machine stands at least `budget` master cycles
  // past the point the previous call reported, and returns the master cycles the
  // machine advanced in this call. A budget that ends inside a machine cycle finishes
  // the cycle; the overshoot is credited to the next call, so run(a) then run(b) lands
  // where run(a + b) does, and run(0) runs nothing. The budget is accounted for as the
  // call begins: a save observer's report made inside the call may throw (SaveObserver),
  // and the cycles not yet run are then owed to the next call, which spends them first.
  std::uint64_t run(std::uint64_t budget);

  // The controller in a port: a pad, or nothing, which is how the machine starts. The
  // pad is part of the state: a snapshot carries it and restore() puts it back. The
  // program sees a change when the port's register next fills from the buttons: at
  // every read while the strobe is high, and at the write that drops it.
  void setJoypad(JoypadPort port, std::optional<Joypad> pad) noexcept;
  [[nodiscard]] const std::optional<Joypad>& joypad(JoypadPort port) const noexcept;

  // The observer told every access the machine makes (BusObserver, above), or none,
  // which is how the machine starts. It is the host's object and outlives every step
  // it is set for; it is not part of the state, so a snapshot does not carry it,
  // restore() leaves it in place and a copied machine points at the same one. With
  // none set an access costs one test, and the machine runs exactly as it does with
  // one.
  void setObserver(BusObserver* observer) noexcept { observer_ = observer; }
  [[nodiscard]] BusObserver* observer() const noexcept { return observer_; }

  // The observer told that the save has changed (SaveObserver, above), or none, which
  // is how the machine starts; the host's object on the bus observer's terms. The
  // bytes a report is measured against are the save as the machine was built, until a
  // report hands the save over, whichever observer received it; so an observer set
  // after the program has run is told, at the next frame's end, of everything stored
  // since the last report, or since power-on when none has been made. With none set the
  // save is never compared, and a cartridge without a battery never reports.
  void setSaveObserver(SaveObserver* observer) noexcept { saveObserver_ = observer; }
  [[nodiscard]] SaveObserver* saveObserver() const noexcept { return saveObserver_; }

  // The save's size: the PRG RAM's on a cartridge with a battery, 0 on one without. A
  // save observer is handed this many bytes.
  [[nodiscard]] std::size_t saveDataSize() const noexcept;

 private:
  // The CPU's view of the console: the two members the core's bus concept names, each
  // one access on the machine's bus made in the CPU's name.
  struct Bus {
    Nes& m;
    std::uint8_t read(std::uint16_t address, CycleKind kind) {
      return m.busRead(address, kind, AccessSource::Cpu);
    }
    void write(std::uint16_t address, std::uint8_t value, CycleKind kind) {
      m.busWrite(address, value, kind, AccessSource::Cpu);
    }
  };

  // What an address reaches, as the console decodes it (NESdev wiki, "CPU memory map";
  // "2A03", 2A03 register map). Every read and every write resolves through decode()
  // and nothing else.
  enum class Target : std::uint8_t {
    WorkRam,    // $0000-$1FFF
    Ppu,        // $2000-$3FFF, the eight ports repeating every eight bytes
    Apu,        // $4000-$4013
    OamDma,     // $4014
    ApuStatus,  // $4015
    JoypadOne,  // $4016: the strobe on a write, port one on a read
    JoypadTwo,  // $4017: the frame counter on a write, port two on a read
    Unused,     // $4018-$401F, nothing on a retail console
    Cartridge,  // $4020-$FFFF, the board's
  };
  [[nodiscard]] static Target decode(std::uint16_t address) noexcept;

  // One access each way, made by `source`. Every read but $4015's, and every write,
  // leaves its byte on the open bus. Every access moves the controller ports' clock
  // lines first and is told to the bus observer last.
  std::uint8_t busRead(std::uint16_t address, CycleKind kind, AccessSource source);
  void busWrite(std::uint16_t address, std::uint8_t value, CycleKind kind, AccessSource source);
  // Tells the bus observer one access, when one is set.
  void observe(std::uint16_t address, std::uint8_t value, bool write, CycleKind kind,
               AccessSource source);

  // The controller ports' clock lines for one access: `reading` is the port whose
  // register the access reads, if any. That port's line falls or stays low; every
  // other port's line rises if it was low, which clocks its pad.
  void clockJoypads(std::optional<std::size_t> reading) noexcept;
  // One clock of a port's pad: the register shifts, or fills from the buttons while the
  // strobe is high.
  void clockJoypad(std::size_t port) noexcept;
  // Fills a port's register from its pad's buttons, or with 0 for an empty socket.
  void fillJoypad(std::size_t port) noexcept;
  // The bit a read of a port's register answers on its first data line.
  [[nodiscard]] std::uint8_t joypadBit(std::size_t port) noexcept;

  // One machine cycle: the dots that fall before M2 rises, the cycle's one bus access,
  // the dots that fall after, the NMI line, then the counters. Answers whether one of
  // its dots ended a frame.
  bool machineCycle();
  // The cycle's one bus access: the OAM DMA unit's get or put while a transfer holds
  // the bus, otherwise the CPU's.
  void busCycle();
  // The CPU's cycle on the bus, halted or not.
  void cpuCycle();

  // Tells the save observer the save, when one is set and the save differs from the
  // bytes it last received, and keeps those bytes as what it received. Made after a
  // cycle that ended a frame and by restore().
  void reportSave();

  // The chip's inputs as this machine supplies them: its board and cartridge, its CHR
  // RAM, and the I/O bus decay in whole dots.
  [[nodiscard]] PpuInputs ppuInputs() noexcept;

  NesState state_;
  Cpu6502 cpu_;
  Cartridge cartridge_;
  Region region_;
  std::uint8_t alignment_;
  std::uint32_t ppuOpenBusDecay_;
  BusObserver* observer_ = nullptr;
  SaveObserver* saveObserver_ = nullptr;
  // The save as the save observer last received it: the seed at construction, then
  // what each report hands over. Sized with the PRG RAM on a cartridge with a battery,
  // empty on one without; the host's view of the save, not part of the state.
  std::vector<std::uint8_t> saveReference_;
  // Whether the cycle under way is reading $2002, which the dots that tick after the
  // read are told. Meaningful inside one machineCycle call and not part of the state.
  bool statusRead_ = false;
};

}  // namespace neasant
