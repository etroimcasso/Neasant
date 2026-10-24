#include "neasant/nes/nes.h"

#include <cctype>
#include <stdexcept>
#include <string>

namespace neasant {

namespace {

// The NTSC dividers: the CPU takes one cycle every twelve master cycles and the PPU one
// dot every four (NESdev wiki, "CPU", Frequencies; "PPU frame timing", CPU-PPU Clock
// Alignment).
constexpr std::uint64_t kMasterCyclesPerCpuCycle = 12;
constexpr std::uint64_t kMasterCyclesPerDot = 4;

// Where the CPU's access falls among the cycle's dots. M2 is low for the first 9/24 of
// the cycle and high for the rest, four and a half master cycles in (NESdev wiki, "CPU
// pinout", M2: a duty cycle of 15/24, and CPU cycles begin where M2 goes low), and the
// console decodes the PPU's ports while M2 is high (NESdev wiki, "Open bus behavior",
// Electrical basis), so a dot at master offset 4 falls before the access and one at
// offset 5 after it: the dots under five master cycles into the cycle tick first.
constexpr std::uint64_t kMasterCyclesToM2Rise = 5;

// The half of the APU cycle a DMA unit may read in (NESdev wiki, "DMA", Cadence).
constexpr std::uint8_t kGetHalf = 0;

// The port the OAM DMA unit writes each byte to (NESdev wiki, "DMA", OAM DMA).
constexpr std::uint16_t kOamDataPort = 0x2004;

// The buttons in shift order, and their names.
constexpr std::array<Button, kButtonCount> kButtons{Button::A,  Button::B,    Button::Select,
                                                    Button::Start, Button::Up, Button::Down,
                                                    Button::Left, Button::Right};
constexpr std::array<std::string_view, kButtonCount> kButtonNames{
    "a", "b", "select", "start", "up", "down", "left", "right"};

// Whether `name` spells `button` in any case.
bool spells(std::string_view name, std::string_view button) noexcept {
  if (name.size() != button.size()) return false;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const int lowered = std::tolower(static_cast<unsigned char>(name[i]));
    if (lowered != static_cast<unsigned char>(button[i])) return false;
  }
  return true;
}

// Fills a RAM from the seed, repeated from its start; an empty seed fills with $00.
void fillFromSeed(std::span<std::uint8_t> ram, std::span<const std::uint8_t> seed) {
  for (std::size_t i = 0; i < ram.size(); ++i) {
    ram[i] = seed.empty() ? std::uint8_t{0} : seed[i % seed.size()];
  }
}

}  // namespace

std::span<const Button> buttons() noexcept { return kButtons; }

std::string_view buttonName(Button button) noexcept {
  const auto index = static_cast<std::size_t>(button);
  return index < kButtonCount ? kButtonNames[index] : std::string_view{};
}

std::optional<Button> buttonFromName(std::string_view name) noexcept {
  for (std::size_t i = 0; i < kButtonCount; ++i) {
    if (spells(name, kButtonNames[i])) return kButtons[i];
  }
  return std::nullopt;
}

bool Joypad::holds(Button button) const noexcept {
  switch (button) {
    case Button::A: return a;
    case Button::B: return b;
    case Button::Select: return select;
    case Button::Start: return start;
    case Button::Up: return up;
    case Button::Down: return down;
    case Button::Left: return left;
    case Button::Right: return right;
  }
  return false;
}

void Joypad::hold(Button button, bool pressed) noexcept {
  switch (button) {
    case Button::A: a = pressed; return;
    case Button::B: b = pressed; return;
    case Button::Select: select = pressed; return;
    case Button::Start: start = pressed; return;
    case Button::Up: up = pressed; return;
    case Button::Down: down = pressed; return;
    case Button::Left: left = pressed; return;
    case Button::Right: right = pressed; return;
  }
}

std::uint8_t Joypad::bits() const noexcept {
  std::uint8_t value = 0;
  for (std::size_t i = 0; i < kButtonCount; ++i) {
    if (holds(kButtons[i])) value = static_cast<std::uint8_t>(value | (1u << i));
  }
  return value;
}

Nes::Nes(NesConfig config)
    : cpu_(powerOn()),
      region_(config.region),
      alignment_(config.alignment),
      ppuOpenBusDecay_(config.ppuOpenBusDecay) {
  if (config.region != Region::Ntsc) {
    throw std::invalid_argument("PAL and Dendy are not yet selectable; NTSC is");
  }
  if (config.alignment > 3) {
    throw std::invalid_argument("an alignment of " +
                                std::to_string(static_cast<unsigned>(config.alignment)) +
                                " is not one of 0 to 3");
  }
  if (config.apuPhase > 1) {
    throw std::invalid_argument("an apuPhase of " +
                                std::to_string(static_cast<unsigned>(config.apuPhase)) +
                                " is not 0 (a get) or 1 (a put)");
  }
  cartridge_ = makeCartridge(config.rom, CartridgeOverrides{.mapper = config.mapper,
                                                             .mirroring = config.mirroring,
                                                             .prgRamBytes = config.prgRamBytes,
                                                             .chrRamBytes = config.chrRamBytes,
                                                             .battery = config.battery});
  state_.board = boardFor(cartridge_);
  state_.prgRam.resize(cartridge_.prgRamBytes);
  state_.chrRam.resize(cartridge_.chrRamBytes);
  fillFromSeed(state_.ram, config.ramSeed);
  fillFromSeed(state_.prgRam, config.ramSeed);
  fillFromSeed(state_.chrRam, config.ramSeed);
  fillFromSeed(state_.ppu.ciram, config.ramSeed);
  fillFromSeed(state_.ppu.oam, config.ramSeed);
  // NESdev wiki, "PPU power up state": the palette is unspecified at power-on; "PPU
  // registers", Reading palette RAM: an entry is six bits.
  fillFromSeed(state_.ppu.palette, config.ramSeed);
  for (std::uint8_t& entry : state_.ppu.palette) {
    entry = static_cast<std::uint8_t>(entry & 0x3Fu);
  }
  // NESdev wiki, "PPU power up state", the note on the VBL flag: random at power-on.
  state_.ppu.vblank = config.vblankAtPowerOn;
  state_.apuPhase = config.apuPhase;
  // NESdev wiki, "PPU frame timing", CPU-PPU Clock Alignment: the first CPU cycle begins 0
  // to 3 master cycles before the PPU's nearest following dot.
  state_.dotClock = config.alignment;
  state_.cpu = cpu_.state();
}

void Nes::restore(const NesState& state) {
  if (state.prgRam.size() != state_.prgRam.size()) {
    throw std::invalid_argument("a snapshot of another cartridge: its PRG RAM is " +
                                std::to_string(state.prgRam.size()) +
                                " bytes and this machine's is " +
                                std::to_string(state_.prgRam.size()));
  }
  if (state.chrRam.size() != state_.chrRam.size()) {
    throw std::invalid_argument("a snapshot of another cartridge: its CHR RAM is " +
                                std::to_string(state.chrRam.size()) +
                                " bytes and this machine's is " +
                                std::to_string(state_.chrRam.size()));
  }
  if (state.board.index() != state_.board.index()) {
    throw std::invalid_argument(
        "a snapshot of another cartridge: its board is not this machine's");
  }
  state_ = state;
  cpu_.restore(state.cpu);
}

void Nes::reset() {
  cpu_.reset();
  // NESdev wiki, "CPU power up state", APU, the after-reset column; "APU", Status: a
  // reset has the effect of writing $00 to $4015, and the DMC's direct load keeps its
  // low bit. Every other register keeps its byte.
  state_.apuRegs[0x15] = 0;
  state_.apuRegs[0x11] = static_cast<std::uint8_t>(state_.apuRegs[0x11] & 0x01u);
  // NESdev wiki, "CPU pinout", /RST: the line holds the CPU in reset with its pins
  // released, and the CPU starts at the reset vector when it is let go, so a transfer
  // under way ends here and the halted read is not resumed; the page latch keeps its
  // byte.
  state_.dma.stage = DmaStage::Idle;
  state_.dma.index = 0;
  cpu_.setRdyLine(true);
  // NESdev wiki, "PPU power up state": a front-loading console's reset line resets the
  // PPU with the CPU.
  ppuReset(state_.ppu);
  state_.cpu = cpu_.state();
}

std::uint64_t Nes::step() {
  machineCycle();
  state_.consumed += kMasterCyclesPerCpuCycle;
  return kMasterCyclesPerCpuCycle;
}

std::uint64_t Nes::run(std::uint64_t budget) {
  const std::uint64_t start = state_.master;
  const std::uint64_t target = state_.consumed + budget;
  while (state_.master < target) machineCycle();
  state_.consumed = target;
  return state_.master - start;
}

void Nes::setJoypad(JoypadPort port, std::optional<Joypad> pad) noexcept {
  state_.pads[static_cast<std::size_t>(port)] = pad;
}

const std::optional<Joypad>& Nes::joypad(JoypadPort port) const noexcept {
  return state_.pads[static_cast<std::size_t>(port)];
}

PpuInputs Nes::ppuInputs() noexcept {
  return PpuInputs{state_.board, cartridge_, state_.chrRam,
                   static_cast<std::uint32_t>(ppuOpenBusDecay_ / kMasterCyclesPerDot)};
}

Nes::Target Nes::decode(std::uint16_t address) noexcept {
  // NESdev wiki, "CPU memory map", the table; "2A03", 2A03 register map.
  if (address < 0x2000) return Target::WorkRam;
  if (address < 0x4000) return Target::Ppu;
  if (address < 0x4014) return Target::Apu;
  if (address == 0x4014) return Target::OamDma;
  if (address == 0x4015) return Target::ApuStatus;
  if (address == 0x4016) return Target::JoypadOne;
  if (address == 0x4017) return Target::JoypadTwo;
  if (address < 0x4020) return Target::Unused;
  return Target::Cartridge;
}

void Nes::clockJoypads(std::optional<std::size_t> reading) noexcept {
  // NESdev wiki, "Controller reading", Clock timing: a port's clock line is low only
  // while its register is read; the pad shifts when the line rises. "DMA", Register
  // conflicts: the line stays low across adjacent cycles that read the same register,
  // so the pad sees one clock for each contiguous run of reads.
  for (std::size_t port = 0; port < 2; ++port) {
    if (reading == port) {
      state_.clockLow[port] = true;
    } else if (state_.clockLow[port]) {
      clockJoypad(port);
      state_.clockLow[port] = false;
    }
  }
}

void Nes::clockJoypad(std::size_t port) noexcept {
  // NESdev wiki, "Standard controller", Input ($4016 write): while the strobe is high
  // the shift registers are continuously reloaded from the button states, so a clock
  // reloads. Hardware: the 4021's serial input is grounded, so each clock shifts in a
  // bit that reads as 1 once the eight buttons are out. An empty socket has no register
  // to shift: its byte is kept at 0.
  if (state_.strobe || !state_.pads[port]) {
    fillJoypad(port);
    return;
  }
  state_.shift[port] = static_cast<std::uint8_t>((state_.shift[port] >> 1) | 0x80u);
}

void Nes::fillJoypad(std::size_t port) noexcept {
  const std::optional<Joypad>& pad = state_.pads[port];
  state_.shift[port] = pad ? pad->bits() : std::uint8_t{0};
}

std::uint8_t Nes::joypadBit(std::size_t port) noexcept {
  // NESdev wiki, "Standard controller", Output ($4016/$4017 read): with no controller
  // connected the status bit reads 0; Input ($4016 write): while the strobe is high a
  // read keeps returning the current state of the first button.
  if (!state_.pads[port]) return 0;
  if (state_.strobe) fillJoypad(port);
  return static_cast<std::uint8_t>(state_.shift[port] & 0x01u);
}

std::uint8_t Nes::busRead(std::uint16_t address, CycleKind) {
  const Target target = decode(address);
  clockJoypads(target == Target::JoypadOne   ? std::optional<std::size_t>{0}
               : target == Target::JoypadTwo ? std::optional<std::size_t>{1}
                                             : std::nullopt);
  std::uint8_t value = state_.mdr;
  switch (target) {
    case Target::WorkRam:
      // NESdev wiki, "CPU memory map": 2 KB at $0000-$07FF, repeated through $1FFF.
      value = state_.ram[address & 0x07FFu];
      break;
    case Target::Ppu: {
      // NESdev wiki, "CPU memory map": the eight ports repeat every eight bytes.
      const auto port = static_cast<std::uint8_t>(address & 0x0007u);
      const PpuInputs inputs = ppuInputs();
      value = ppuRead(state_.ppu, port, inputs);
      statusRead_ = port == 2;
      break;
    }
    case Target::ApuStatus:
      // NESdev wiki, "APU", Status: bit 5 is open bus, and the other bits report
      // counters and flags this machine does not hold; the external data bus is
      // disconnected for the read, so the open bus keeps the value of the last cycle
      // that did not read $4015.
      return static_cast<std::uint8_t>(state_.mdr & 0x20u);
    case Target::JoypadOne:
    case Target::JoypadTwo:
      // NESdev wiki, "Controller reading", $4016 / $4017 Read and Unconnected data lines
      // and open bus: bit 0 is the port's first data line, the standard controller's;
      // bits 4-1 are lines nothing drives on a front-loading console's ports, 0; bits
      // 7-5 are open bus.
      value = static_cast<std::uint8_t>((state_.mdr & 0xE0u) |
                                        joypadBit(target == Target::JoypadTwo ? 1 : 0));
      break;
    case Target::Apu:
    case Target::OamDma:
    case Target::Unused:
      // NESdev wiki, "2A03", 2A03 register map: the write-only registers read as open
      // bus, and $4018-$401F does nothing on a retail console.
      break;
    case Target::Cartridge: {
      // NESdev wiki, "2A03", 2A03 register map: $4020-$FFFF is the cartridge's. The data
      // lines float at the open-bus byte; a board that decodes the address drives them.
      CpuPins pins{.address = address, .data = state_.mdr};
      boardCpuCycle(state_.board, cartridge_, pins, state_.prgRam);
      value = pins.data;
      break;
    }
  }
  // NESdev wiki, "Open bus behavior", CPU open bus: the last byte read.
  state_.mdr = value;
  return value;
}

void Nes::busWrite(std::uint16_t address, std::uint8_t value, CycleKind) {
  // NESdev wiki, "Controller reading", Clock timing: a write is not a read of a port's
  // register, so it raises every clock line.
  clockJoypads(std::nullopt);
  switch (decode(address)) {
    case Target::WorkRam:
      state_.ram[address & 0x07FFu] = value;
      break;
    case Target::Ppu: {
      const PpuInputs inputs = ppuInputs();
      ppuWrite(state_.ppu, static_cast<std::uint8_t>(address & 0x0007u), value, inputs);
      break;
    }
    case Target::Apu:
    case Target::ApuStatus:
    case Target::JoypadTwo:
      // The APU's registers are held as the bytes written to them (NESdev wiki, "2A03",
      // 2A03 register map; "APU", Registers).
      state_.apuRegs[address & 0x001Fu] = value;
      break;
    case Target::OamDma:
      // NESdev wiki, "DMA", OAM DMA: a transfer copies from the page most recently
      // written to $4014, and the unit is scheduled to halt the CPU on the first cycle
      // after the write.
      state_.dma.page = value;
      state_.dma.stage = DmaStage::Halt;
      break;
    case Target::JoypadOne: {
      // NESdev wiki, "Controller reading", $4016 Write: bit 0 is latched and held as the
      // controllers' strobe; while it is high each pad's register fills from the buttons
      // held, and the write that drops it leaves them held. Bits 2-1 drive the
      // expansion port and are not held.
      const bool wasHigh = state_.strobe;
      state_.strobe = (value & 0x01u) != 0;
      if (wasHigh || state_.strobe) {
        fillJoypad(0);
        fillJoypad(1);
      }
      break;
    }
    case Target::Unused:
      break;
    case Target::Cartridge: {
      CpuPins pins{.address = address, .data = value, .write = true};
      boardCpuCycle(state_.board, cartridge_, pins, state_.prgRam);
      break;
    }
  }
  // NESdev wiki, "Open bus behavior", Electrical basis: a value placed on the data bus
  // floats there to be read again.
  state_.mdr = value;
}

void Nes::cpuCycle() {
  Bus bus{*this};
  cpu_.stepCycle(bus);
}

void Nes::busCycle() {
  Dma& dma = state_.dma;
  switch (dma.stage) {
    case DmaStage::Idle:
      cpuCycle();
      return;
    case DmaStage::Halt:
      // NESdev wiki, "DMA", Behavior: the unit halts the CPU through RDY, which the CPU
      // allows on a read cycle only; a write proceeds and the unit tries again on the
      // next cycle. The halt cycle is the read the CPU then repeats.
      cpu_.setRdyLine(false);
      cpuCycle();
      if (cpu_.state().rdyHeld) dma.stage = DmaStage::Get;
      return;
    case DmaStage::Get:
      // NESdev wiki, "DMA", Cadence and OAM DMA: the unit reads on a get cycle only, so
      // a put cycle before the read is an alignment cycle, on which the halted CPU
      // repeats its read (Register conflicts). The read goes through the bus as the
      // CPU's would.
      if (state_.apuPhase != kGetHalf) {
        cpuCycle();
        return;
      }
      dma.byte = busRead(static_cast<std::uint16_t>((dma.page << 8) | dma.index),
                         CycleKind::DataRead);
      dma.stage = DmaStage::Put;
      return;
    case DmaStage::Put:
      // NESdev wiki, "DMA", OAM DMA: the byte is written to $2004 on the put cycle that
      // follows its get; after the 256th the CPU is released and performs the read it
      // was halted on.
      busWrite(kOamDataPort, dma.byte, CycleKind::DataWrite);
      if (dma.index == 255) {
        dma.index = 0;
        dma.stage = DmaStage::Idle;
        cpu_.setRdyLine(true);
      } else {
        ++dma.index;
        dma.stage = DmaStage::Get;
      }
      return;
  }
}

void Nes::machineCycle() {
  // The dots whose master cycle falls before M2 rises, then the cycle's one access,
  // then the dots that fall after it, which a read of $2002 under way is told (NESdev
  // wiki, "PPU frame timing", VBL Flag Timing), then the NMI output as the CPU samples
  // it at the cycle's end (NESdev wiki, "CPU interrupts", Detailed interrupt behavior:
  // the edge detector polls the line during the second half of each cycle).
  const std::uint64_t access = state_.master + kMasterCyclesToM2Rise;
  const std::uint64_t end = state_.master + kMasterCyclesPerCpuCycle;
  while (state_.dotClock < access) {
    ppuTickDot(state_.ppu, false);
    state_.dotClock += kMasterCyclesPerDot;
  }
  statusRead_ = false;
  busCycle();
  while (state_.dotClock < end) {
    ppuTickDot(state_.ppu, statusRead_);
    state_.dotClock += kMasterCyclesPerDot;
  }
  cpu_.setNmiLine(ppuNmiAsserted(state_.ppu));
  state_.master = end;
  state_.apuPhase = static_cast<std::uint8_t>(state_.apuPhase ^ 1u);
  state_.cpu = cpu_.state();
}

}  // namespace neasant
