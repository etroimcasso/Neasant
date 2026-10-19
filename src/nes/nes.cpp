#include "neasant/nes/nes.h"

#include <stdexcept>
#include <string>

namespace neasant {

namespace {

// The NTSC dividers: the CPU takes one cycle every twelve master cycles and the PPU one
// dot every four (NESdev wiki, "CPU", Frequencies; "PPU frame timing", CPU-PPU Clock
// Alignment).
constexpr std::uint64_t kMasterCyclesPerCpuCycle = 12;
constexpr std::uint64_t kMasterCyclesPerDot = 4;

// Fills a RAM from the seed, repeated from its start; an empty seed fills with $00.
void fillFromSeed(std::span<std::uint8_t> ram, std::span<const std::uint8_t> seed) {
  for (std::size_t i = 0; i < ram.size(); ++i) {
    ram[i] = seed.empty() ? std::uint8_t{0} : seed[i % seed.size()];
  }
}

}  // namespace

Nes::Nes(NesConfig config)
    : cpu_(powerOn()), region_(config.region), alignment_(config.alignment) {
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

std::uint8_t Nes::busRead(std::uint16_t address, CycleKind) {
  std::uint8_t value = state_.mdr;
  switch (decode(address)) {
    case Target::WorkRam:
      // NESdev wiki, "CPU memory map": 2 KB at $0000-$07FF, repeated through $1FFF.
      value = state_.ram[address & 0x07FFu];
      break;
    case Target::Ppu: {
      // NESdev wiki, "CPU memory map": the eight ports repeat every eight bytes.
      PpuInputs inputs{state_.board, cartridge_, state_.chrRam};
      value = ppuRead(state_.ppu, static_cast<std::uint8_t>(address & 0x0007u), inputs);
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
      // and open bus: bits 7-5 are open bus; the five data lines answer 0, the value of
      // a line with nothing on it.
      value = static_cast<std::uint8_t>(state_.mdr & 0xE0u);
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
  switch (decode(address)) {
    case Target::WorkRam:
      state_.ram[address & 0x07FFu] = value;
      break;
    case Target::Ppu: {
      PpuInputs inputs{state_.board, cartridge_, state_.chrRam};
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
      // written to $4014.
      state_.dma.page = value;
      break;
    case Target::JoypadOne:
      // NESdev wiki, "Controller reading", $4016 Write: bit 0 is latched and held as the
      // controllers' strobe.
      state_.strobe = (value & 0x01u) != 0;
      break;
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

void Nes::machineCycle() {
  // The dots whose master cycle falls inside this CPU cycle, then the CPU's one access.
  const std::uint64_t end = state_.master + kMasterCyclesPerCpuCycle;
  while (state_.dotClock < end) {
    ppuTickDot(state_.ppu);
    state_.dotClock += kMasterCyclesPerDot;
  }
  Bus bus{*this};
  cpu_.stepCycle(bus);
  state_.master = end;
  state_.apuPhase = static_cast<std::uint8_t>(state_.apuPhase ^ 1u);
  state_.cpu = cpu_.state();
}

}  // namespace neasant
