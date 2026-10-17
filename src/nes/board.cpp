#include "neasant/nes/board.h"

#include <stdexcept>
#include <string>

namespace neasant {

Board boardFor(const Cartridge& cartridge) {
  // Mapper 0 and what NROM can carry (NESdev wiki, "NROM": the board table and Banks).
  if (cartridge.mapper != 0) {
    throw std::invalid_argument("mapper " + std::to_string(cartridge.mapper) +
                                " is not built; NROM (0) is");
  }
  const std::size_t prgRom = cartridge.prgRom.size();
  if (prgRom != 8192 && prgRom != 16384 && prgRom != 32768) {
    throw std::invalid_argument("NROM carries 8, 16 or 32 KB of PRG ROM; the header declares " +
                                std::to_string(prgRom) + " bytes");
  }
  const std::size_t chrRom = cartridge.chrRom.size();
  const std::size_t chrRam = cartridge.chrRamBytes;
  if (chrRom + chrRam != 8192 || (chrRom != 0 && chrRam != 0)) {
    throw std::invalid_argument("NROM carries 8 KB of CHR, ROM or RAM; the header declares " +
                                std::to_string(chrRom) + " bytes of CHR ROM and " +
                                std::to_string(chrRam) + " of CHR RAM");
  }
  const std::size_t prgRam = cartridge.prgRamBytes;
  if (prgRam > 8192 || (prgRam & (prgRam - 1)) != 0) {
    throw std::invalid_argument(
        "NROM's PRG RAM window holds at most 8 KB in a power-of-two size; the header "
        "declares " +
        std::to_string(prgRam) + " bytes");
  }
  if (cartridge.mirroring == Mirroring::FourScreen) {
    throw std::invalid_argument("NROM has no four-screen RAM");
  }
  if (cartridge.alternativeNametables) {
    throw std::invalid_argument(
        "NROM has no alternative nametable layout; a mirroring in the config overrides the "
        "header's bits");
  }
  return Board{Nrom{}};
}

bool boardCpuCycle(Board& board, const Cartridge& cartridge, CpuPins& pins,
                   std::span<std::uint8_t> prgRam) {
  return std::visit([&](auto& held) { return cpuCycle(held, cartridge, pins, prgRam); }, board);
}

bool boardPpuAccess(Board& board, const Cartridge& cartridge, PpuPins& pins,
                    std::span<std::uint8_t> chrRam) {
  return std::visit([&](auto& held) { return ppuAccess(held, cartridge, pins, chrRam); }, board);
}

Mirroring boardMirroring(const Board& board, const Cartridge& cartridge) noexcept {
  return std::visit([&](const auto& held) { return mirroring(held, cartridge); }, board);
}

}  // namespace neasant
