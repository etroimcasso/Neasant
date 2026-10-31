#pragma once

// NROM, mapper 0: the board with no mapping logic. Its PRG ROM is 16 KB, repeated across
// $8000-$FFFF, or 32 KB; its CHR is 8 KB of ROM or RAM over the pattern tables; its
// nametable mirroring is fixed by a solder pad; a Family BASIC board adds PRG RAM at
// $6000-$7FFF (NESdev wiki, "NROM"). It holds no state of its own.

#include "neasant/nes/cartridge.h"

#include <cstdint>
#include <span>

namespace neasant {

struct Nrom {
  [[nodiscard]] bool operator==(const Nrom&) const noexcept = default;
};

// The CPU's access on the pins. Answers true when the board decodes the address: a read
// of $8000-$FFFF drives the PRG ROM byte, a read of $6000-$7FFF drives the PRG RAM byte
// when the board has RAM, a write to that RAM lands, and a write to the ROM changes
// nothing. Every other address is nobody's on this board and answers false with `data`
// untouched.
bool cpuCycle(Nrom&, const Cartridge& cartridge, CpuPins& pins, std::span<std::uint8_t> prgRam);

// The PPU's access on the pins. Drives the CIRAM lines on every presentation — enabled
// for $2000-$3FFF, A10 from PPU A11 under horizontal mirroring and PPU A10 under vertical —
// and answers the pattern tables, $0000-$1FFF, from the CHR ROM or RAM: true for those
// addresses, false above them.
bool ppuAccess(Nrom&, const Cartridge& cartridge, PpuPins& pins, std::span<std::uint8_t> chrRam);

// The mirroring the board is wired for.
[[nodiscard]] Mirroring mirroring(const Nrom&, const Cartridge& cartridge) noexcept;

}  // namespace neasant
