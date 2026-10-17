#pragma once

// The board a cartridge is on, as one value. Every board is a struct holding its own
// state, and `Board` is the variant over them; a board is added by declaring its struct
// and its three functions in its own header and naming it here. The console reaches a
// board only through the pins (cartridge.h): each CPU cycle and each PPU access is
// presented as it happens, and the board answers on the lines.

#include "neasant/mappers/nrom.h"
#include "neasant/nes/cartridge.h"

#include <cstdint>
#include <span>
#include <variant>

namespace neasant {

using Board = std::variant<Nrom>;

// The board a cartridge's mapper number names, ready to answer. Throws
// std::invalid_argument for a mapper that is not built, and for a cartridge whose sizes
// or layout the named board does not have, naming what it does have.
[[nodiscard]] Board boardFor(const Cartridge& cartridge);

// One CPU cycle on the pins, dispatched to the board held. True when the board decoded
// the address; false leaves the console's bus as it was.
bool boardCpuCycle(Board& board, const Cartridge& cartridge, CpuPins& pins,
                   std::span<std::uint8_t> prgRam);

// One PPU access on the pins, dispatched likewise. The board drives the CIRAM lines
// every time; true when it answered the access itself.
bool boardPpuAccess(Board& board, const Cartridge& cartridge, PpuPins& pins,
                    std::span<std::uint8_t> chrRam);

// The mirroring the board drives on CIRAM A10, for a host that asks by name.
[[nodiscard]] Mirroring boardMirroring(const Board& board, const Cartridge& cartridge) noexcept;

}  // namespace neasant
