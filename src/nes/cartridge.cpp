#include "neasant/nes/cartridge.h"

#include <stdexcept>
#include <string>

namespace neasant {

namespace {

// NESdev wiki, "iNES", iNES file format; "NES 2.0", Identification: "NES" and $1A.
bool hasIdentifier(std::span<const std::uint8_t> image) noexcept {
  return image.size() >= kHeaderBytes && image[0] == 0x4E && image[1] == 0x45 &&
         image[2] == 0x53 && image[3] == 0x1A;
}

// A NES 2.0 ROM size: the twelve bits as a count of units, or, when the high nibble is
// $F, the exponent-multiplier form 2^E * (MM*2+1) held in the low byte (NESdev wiki,
// "NES 2.0", PRG-ROM Area and CHR-ROM Area).
std::size_t nes2RomSize(std::uint8_t low, std::uint8_t highNibble, std::size_t unit) noexcept {
  if (highNibble != 0x0F) {
    return (static_cast<std::size_t>(highNibble) << 8 | static_cast<std::size_t>(low)) * unit;
  }
  const auto exponent = static_cast<unsigned>(low >> 2);
  const auto multiplier = static_cast<std::size_t>(low & 0x03) * 2 + 1;
  return (std::size_t{1} << exponent) * multiplier;
}

// A NES 2.0 RAM size from one shift-count nibble: none for zero, else 64 << count
// (NESdev wiki, "NES 2.0", bytes 10 and 11).
std::size_t nes2RamSize(unsigned count) noexcept {
  return count == 0 ? 0 : std::size_t{64} << count;
}

// NESdev wiki, "iNES", Variant comparison, step 1: the sizes a NES 2.0 header declares fit
// inside the image. Each size is compared against the bytes that remain, so no sum of
// sizes wraps.
bool fitsInside(std::size_t imageBytes, std::size_t before, std::size_t prgRomBytes,
                std::size_t chrRomBytes) noexcept {
  if (before > imageBytes) return false;
  const std::size_t remaining = imageBytes - before;
  return prgRomBytes <= remaining && chrRomBytes <= remaining - prgRomBytes;
}

}  // namespace

std::optional<CartridgeHeader> parseCartridgeHeader(std::span<const std::uint8_t> image) noexcept {
  if (!hasIdentifier(image)) return std::nullopt;

  CartridgeHeader header;
  for (std::size_t i = 0; i < kHeaderBytes; ++i) header.bytes[i] = image[i];
  const std::array<std::uint8_t, kHeaderBytes>& b = header.bytes;

  // NESdev wiki, "iNES", Flags 6; "Mirroring", Horizontal and Vertical.
  header.mirroring = (b[6] & 0x01) != 0 ? Mirroring::Vertical : Mirroring::Horizontal;
  header.battery = (b[6] & 0x02) != 0;
  header.trainer = (b[6] & 0x04) != 0;
  header.alternativeNametables = (b[6] & 0x08) != 0;

  // NESdev wiki, "NES 2.0", Identification; "iNES", Variant comparison, step 1.
  const std::size_t nes2Prg =
      nes2RomSize(b[4], static_cast<std::uint8_t>(b[9] & 0x0F), 16384);
  const std::size_t nes2Chr = nes2RomSize(b[5], static_cast<std::uint8_t>(b[9] >> 4), 8192);
  const std::size_t before = kHeaderBytes + (header.trainer ? kTrainerBytes : 0);
  if ((b[7] & 0x0C) == 0x08 && fitsInside(image.size(), before, nes2Prg, nes2Chr)) {
    header.format = HeaderFormat::Nes2;
    header.prgRomBytes = nes2Prg;
    header.chrRomBytes = nes2Chr;
    // NESdev wiki, "NES 2.0", byte 8.
    header.mapper =
        static_cast<std::uint16_t>((b[6] >> 4) | (b[7] & 0xF0) | (b[8] & 0x0F) << 8);
    header.submapper = static_cast<std::uint8_t>(b[8] >> 4);
    // NESdev wiki, "NES 2.0", bytes 10 and 11, PRG-(NV)RAM and CHR-(NV)RAM.
    header.prgRamBytes = nes2RamSize(static_cast<unsigned>(b[10] & 0x0F));
    header.prgNvramBytes = nes2RamSize(static_cast<unsigned>(b[10] >> 4));
    header.chrRamBytes = nes2RamSize(static_cast<unsigned>(b[11] & 0x0F));
    header.chrNvramBytes = nes2RamSize(static_cast<unsigned>(b[11] >> 4));
    // NESdev wiki, "NES 2.0", byte 7, bytes 12-15, CPU/PPU Timing.
    header.consoleType = static_cast<ConsoleType>(b[7] & 0x03);
    header.consoleDetail = b[13];
    header.timing = static_cast<Timing>(b[12] & 0x03);
    header.miscRoms = static_cast<std::uint8_t>(b[14] & 0x03);
    header.defaultExpansionDevice = static_cast<std::uint8_t>(b[15] & 0x7F);
    return header;
  }

  // NESdev wiki, "iNES", Variant comparison, steps 2-4, and the "DiskDude!" paragraph.
  header.archaic = (b[7] & 0x0C) != 0 || b[12] != 0 || b[13] != 0 || b[14] != 0 || b[15] != 0;
  // NESdev wiki, "iNES", the header's bytes 4 and 5.
  header.prgRomBytes = std::size_t{b[4]} * 16384;
  header.chrRomBytes = std::size_t{b[5]} * 8192;
  // NESdev wiki, "iNES", Flags 6 and Flags 7.
  header.mapper = header.archaic ? static_cast<std::uint16_t>(b[6] >> 4)
                                 : static_cast<std::uint16_t>((b[6] >> 4) | (b[7] & 0xF0));
  // NESdev wiki, "iNES", Flags 8: "Value 0 infers 8 KB for compatibility".
  const std::size_t units = header.archaic ? 0 : std::size_t{b[8]};
  const std::size_t prgRam = (units == 0 ? 1 : units) * 8192;
  if (header.battery) {
    header.prgNvramBytes = prgRam;
  } else {
    header.prgRamBytes = prgRam;
  }
  // NESdev wiki, "iNES", byte 5: a CHR ROM size of 0 means the board uses CHR RAM.
  header.chrRamBytes = header.chrRomBytes == 0 ? 8192 : 0;
  if (!header.archaic) {
    // NESdev wiki, "iNES", Flags 7 and Flags 9.
    if ((b[7] & 0x01) != 0) {
      header.consoleType = ConsoleType::VsSystem;
    } else if ((b[7] & 0x02) != 0) {
      header.consoleType = ConsoleType::Playchoice10;
    }
    header.timing = (b[9] & 0x01) != 0 ? Timing::Pal : Timing::Ntsc;
  }
  return header;
}

Cartridge makeCartridge(std::span<const std::uint8_t> image, const CartridgeOverrides& overrides) {
  if (image.size() < kHeaderBytes) {
    throw std::invalid_argument("an image of " + std::to_string(image.size()) +
                                " bytes holds no 16-byte header");
  }
  const std::optional<CartridgeHeader> parsed = parseCartridgeHeader(image);
  if (!parsed) throw std::invalid_argument("the image carries no NES header");
  const CartridgeHeader& header = *parsed;

  // NESdev wiki, "iNES", Flags 6 and Flags 7; "NES 2.0", Trainer Area and byte 7.
  if (header.trainer) throw std::invalid_argument("a trainer is not supported");
  switch (header.consoleType) {
    case ConsoleType::Nes:
      break;
    case ConsoleType::VsSystem:
      throw std::invalid_argument(
          "a Vs. System image is not supported; NES and Famicom images are");
    case ConsoleType::Playchoice10:
      throw std::invalid_argument(
          "a Playchoice 10 image is not supported; NES and Famicom images are");
    case ConsoleType::Extended:
      throw std::invalid_argument(
          "an extended-console image is not supported; NES and Famicom images are");
  }
  const std::size_t declared = kHeaderBytes + header.prgRomBytes + header.chrRomBytes;
  if (declared > image.size()) {
    throw std::invalid_argument("the image is " + std::to_string(declared - image.size()) +
                                " bytes short of the header's sizes");
  }

  Cartridge cartridge;
  cartridge.header = header;
  // NESdev wiki, "iNES", iNES file format: the PRG ROM follows the header and the CHR ROM
  // follows the PRG ROM.
  const std::span<const std::uint8_t> prg = image.subspan(kHeaderBytes, header.prgRomBytes);
  const std::span<const std::uint8_t> chr =
      image.subspan(kHeaderBytes + header.prgRomBytes, header.chrRomBytes);
  cartridge.prgRom.assign(prg.begin(), prg.end());
  cartridge.chrRom.assign(chr.begin(), chr.end());
  cartridge.mapper = overrides.mapper.value_or(header.mapper);
  cartridge.mirroring = overrides.mirroring.value_or(header.mirroring);
  cartridge.alternativeNametables = !overrides.mirroring && header.alternativeNametables;
  cartridge.prgRamBytes =
      overrides.prgRamBytes.value_or(header.prgRamBytes + header.prgNvramBytes);
  cartridge.chrRamBytes =
      overrides.chrRamBytes.value_or(header.chrRamBytes + header.chrNvramBytes);
  cartridge.battery = overrides.battery.value_or(header.battery);
  return cartridge;
}

}  // namespace neasant
