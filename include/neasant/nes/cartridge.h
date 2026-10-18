#pragma once

// A cartridge as a value: the header an image carries, the PRG and CHR it holds, and
// the pins a board sees.
//
// An image is a header followed by its PRG ROM and CHR ROM. The header is one of two
// formats, iNES and NES 2.0, and is read byte by byte into a struct that keeps the bytes
// as written beside what they mean. A cartridge is that header plus the two areas copied
// out of the image and the sizes a host's overrides settle. A board (board.h) is built
// from a cartridge and answers the console's accesses on the pins declared here.
//
// Nothing here runs anything. A tool that wants the header and never a machine reads it
// through parseCartridgeHeader alone.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace neasant {

// Which header format an image carries (NESdev wiki, "iNES", Variant comparison;
// "NES 2.0", Identification).
enum class HeaderFormat : std::uint8_t { Ines, Nes2 };

// How the board wires CIRAM A10, which decides which of the console's two nametable
// kilobytes an address reaches (NESdev wiki, "Mirroring", Nametable Mirroring;
// "Cartridge connector", CIRAM A10). Horizontal mirroring takes PPU A11, so the two
// nametables sit one above the other; vertical mirroring takes PPU A10, so they sit side
// by side. Four-screen is a board with its own nametable RAM, which answers instead of
// the console's; a header's two layout bits name the first two, and a board or a host's
// override names the third.
enum class Mirroring : std::uint8_t { Horizontal, Vertical, FourScreen };

// The console an image is for (NESdev wiki, "NES 2.0", byte 7; "iNES", Flags 7).
enum class ConsoleType : std::uint8_t { Nes, VsSystem, Playchoice10, Extended };

// The CPU and PPU timing an image expects (NESdev wiki, "NES 2.0", CPU/PPU Timing; "iNES",
// Flags 9). An iNES header names NTSC or PAL; a NES 2.0 header also names a game that
// adjusts itself to either, and the Dendy.
enum class Timing : std::uint8_t { Ntsc, Pal, Multiple, Dendy };

inline constexpr std::size_t kHeaderBytes = 16;
inline constexpr std::size_t kTrainerBytes = 512;

// What a header says: the sixteen bytes as written, and each field read from them. Under
// iNES the sizes are in units of 16 KB and 8 KB, PRG RAM is 8 KB unless byte 8 says
// otherwise, and a CHR ROM size of zero means 8 KB of CHR RAM. Under NES 2.0 the sizes
// carry four more bits each with an exponent form for odd sizes, every RAM has its own
// shift count, and the console type, the timing and the expansion device are stated.
struct CartridgeHeader {
  std::array<std::uint8_t, kHeaderBytes> bytes{};  // as written
  HeaderFormat format = HeaderFormat::Ines;
  // An iNES header whose bytes 7-15 are a signature or padding rather than fields: byte
  // 7's bits 3-2 are %01, or bytes 12-15 are not all zero (NESdev wiki, "iNES", the
  // "DiskDude!" rule and the detection procedure). Those bytes are then unread: the mapper
  // is byte 6's nibble alone, the console is the NES and the timing NTSC.
  bool archaic = false;
  std::size_t prgRomBytes = 0;
  std::size_t chrRomBytes = 0;          // 0 under iNES means the board carries CHR RAM
  std::uint16_t mapper = 0;             // 8 bits under iNES, 12 under NES 2.0
  std::uint8_t submapper = 0;           // NES 2.0 only; 0 otherwise
  Mirroring mirroring = Mirroring::Horizontal;  // flags 6 bit 0: Horizontal or Vertical
  bool alternativeNametables = false;   // flags 6 bit 3, whose meaning is the mapper's
  bool battery = false;                 // flags 6 bit 1: the PRG RAM, or other memory, persists
  bool trainer = false;                 // flags 6 bit 2: 512 bytes sit between the header and the PRG ROM
  std::size_t prgRamBytes = 0;          // volatile PRG RAM
  std::size_t prgNvramBytes = 0;        // battery-backed PRG RAM or EEPROM
  std::size_t chrRamBytes = 0;          // volatile CHR RAM
  std::size_t chrNvramBytes = 0;        // battery-backed CHR RAM
  ConsoleType consoleType = ConsoleType::Nes;
  std::uint8_t consoleDetail = 0;       // NES 2.0 byte 13 as written: the Vs. System or extended console type
  Timing timing = Timing::Ntsc;
  std::uint8_t miscRoms = 0;            // NES 2.0 byte 14: ROMs after the CHR area
  std::uint8_t defaultExpansionDevice = 0;  // NES 2.0 byte 15
  [[nodiscard]] bool operator==(const CartridgeHeader&) const noexcept = default;
};

// The header an image begins with, or nothing when the span is shorter than a header or
// does not open with "NES" and $1A. The format is decided as the detection procedure
// says (NESdev wiki, "iNES", Variant comparison): NES 2.0 when byte 7 carries its
// identifier and the sizes it declares fit inside `image`; iNES otherwise, archaic when
// byte 7 or the tail says so. Because the size check reads the whole span, the sixteen
// header bytes handed in alone never read as NES 2.0.
[[nodiscard]] std::optional<CartridgeHeader> parseCartridgeHeader(
    std::span<const std::uint8_t> image) noexcept;

// What a host states over the header: the mapper, the mirroring, the two RAM sizes (0
// for none) and the battery flag. A field left empty takes the header's value.
struct CartridgeOverrides {
  std::optional<std::uint16_t> mapper{};
  std::optional<Mirroring> mirroring{};
  std::optional<std::size_t> prgRamBytes{};
  std::optional<std::size_t> chrRamBytes{};
  std::optional<bool> battery{};
};

// A cartridge: its header, its two ROM areas copied out of the image, and the values the
// board is built from once the overrides are applied. The two RAM sizes are the volatile
// and battery-backed halves summed; `battery` says whether the machine reports the PRG
// RAM as the save.
struct Cartridge {
  CartridgeHeader header;
  std::vector<std::uint8_t> prgRom;
  std::vector<std::uint8_t> chrRom;      // empty when the header declares none
  std::uint16_t mapper = 0;
  Mirroring mirroring = Mirroring::Horizontal;
  bool alternativeNametables = false;    // the header's bit; false once an override names the mirroring
  std::size_t prgRamBytes = 0;
  std::size_t chrRamBytes = 0;
  bool battery = false;
  [[nodiscard]] bool operator==(const Cartridge&) const noexcept = default;
};

// Builds a cartridge from an image. Throws std::invalid_argument, naming what is
// supported instead, for an image too short to hold a header or without one, a trainer,
// a console other than the NES and Famicom, or an image shorter than the sizes its header
// declares. Bytes after the CHR area are not part of the cartridge.
[[nodiscard]] Cartridge makeCartridge(std::span<const std::uint8_t> image,
                                      const CartridgeOverrides& overrides = {});

// The CPU side of the connector as the board sees it, once per CPU cycle: the address
// and R/W are stable for the whole period M2 is high, and the data is what the bus
// carries at M2's fall (NESdev wiki, "Cartridge connector", Signal descriptions). On a
// write the console drives `data`; on a read the board drives it when it decodes the
// address and leaves it alone when it does not.
struct CpuPins {
  std::uint16_t address = 0;  // A0-A15 as the CPU drives them; bit 15 is what /ROMSEL decodes
  std::uint8_t data = 0;      // D0-D7
  bool write = false;         // R/W low
  // /ROMSEL low: the console NANDs M2 with A15, so the line is low for $8000-$FFFF while
  // the address is stable (NESdev wiki, "Cartridge connector", Signal descriptions).
  [[nodiscard]] constexpr bool romSelect() const noexcept { return (address & 0x8000u) != 0; }
};

// The PPU side of the connector as the board sees it, once per PPU access. The board
// drives the two CIRAM lines on every presentation and `data` on a read it answers
// (NESdev wiki, "Cartridge connector", Signal descriptions).
struct PpuPins {
  std::uint16_t address = 0;  // PA0-PA13; /A13 is bit 13 inverted
  std::uint8_t data = 0;      // PD0-PD7
  bool read = false;          // /RD low
  bool write = false;         // /WR low
  bool ciramA10 = false;      // which kilobyte of the console's 2 KB a nametable access reaches
  bool ciramEnable = false;   // /CIRAM CE low: the console's 2 KB answers this access
};

}  // namespace neasant
