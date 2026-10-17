#include "neasant/mappers/nrom.h"

namespace neasant {

bool cpuCycle(Nrom&, const Cartridge& cartridge, CpuPins& pins, std::span<std::uint8_t> prgRam) {
  const auto address = static_cast<std::size_t>(pins.address);
  if (pins.romSelect()) {
    // NESdev wiki, "NROM", Registers: no mapping logic, so a write to the ROM changes nothing.
    if (pins.write) return true;
    // NESdev wiki, "NROM", Banks.
    pins.data = cartridge.prgRom[address & (cartridge.prgRom.size() - 1)];
    return true;
  }
  if (address >= 0x6000 && !prgRam.empty()) {
    // NESdev wiki, "NROM", Banks.
    std::uint8_t& byte = prgRam[(address - 0x6000) & (prgRam.size() - 1)];
    if (pins.write) {
      byte = pins.data;
    } else {
      pins.data = byte;
    }
    return true;
  }
  return false;
}

bool ppuAccess(Nrom&, const Cartridge& cartridge, PpuPins& pins, std::span<std::uint8_t> chrRam) {
  const auto address = static_cast<std::size_t>(pins.address);
  // NESdev wiki, "Cartridge connector", Signal descriptions: CIRAM /CE from PPU /A13.
  pins.ciramEnable = (address & 0x2000) != 0;
  // NESdev wiki, "Mirroring", Horizontal and Vertical: CIRAM A10 from PPU A11 or PPU A10.
  const std::size_t line = cartridge.mirroring == Mirroring::Horizontal ? 0x0800 : 0x0400;
  pins.ciramA10 = (address & line) != 0;
  if (address >= 0x2000) return false;
  // NESdev wiki, "NROM", Banks.
  const std::size_t index = address & 0x1FFF;
  if (!cartridge.chrRom.empty()) {
    if (pins.read) pins.data = cartridge.chrRom[index];
  } else if (pins.write) {
    chrRam[index] = pins.data;
  } else if (pins.read) {
    pins.data = chrRam[index];
  }
  return true;
}

Mirroring mirroring(const Nrom&, const Cartridge& cartridge) noexcept {
  return cartridge.mirroring;
}

}  // namespace neasant
