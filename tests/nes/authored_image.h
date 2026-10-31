#pragma once

// Test support for authored cartridge images: a header of either format from the fields
// a case names, an image of that header with zeroed PRG and CHR and its vectors set, and
// access to a byte of either area. No image file is read; every test builds its own.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace neasant::nes_tests {

// Builds a 16-byte iNES header from the fields a test names; every field has the
// value an all-zero header carries, so a case sets only what it exercises.
struct InesFields {
  std::uint8_t prgUnits = 1;             // byte 4: PRG ROM in 16 KB units
  std::uint8_t chrUnits = 1;             // byte 5: CHR ROM in 8 KB units; 0 for CHR RAM
  std::uint8_t mapper = 0;               // split across flags 6 and 7
  bool verticalMirroring = false;        // flags 6 bit 0
  bool battery = false;                  // flags 6 bit 1
  bool trainer = false;                  // flags 6 bit 2
  bool alternativeNametables = false;    // flags 6 bit 3
  bool vsSystem = false;                 // flags 7 bit 0
  bool playchoice10 = false;             // flags 7 bit 1
  std::uint8_t format = 0;               // flags 7 bits 3-2, as written
  std::uint8_t prgRamUnits = 0;          // byte 8: PRG RAM in 8 KB units
  bool pal = false;                      // byte 9 bit 0
  std::array<std::uint8_t, 6> tail{};    // bytes 10-15, as written
};

[[nodiscard]] inline std::array<std::uint8_t, 16> inesHeader(const InesFields& f) {
  std::array<std::uint8_t, 16> header{0x4E, 0x45, 0x53, 0x1A};
  header[4] = f.prgUnits;
  header[5] = f.chrUnits;
  header[6] = static_cast<std::uint8_t>((f.mapper & 0x0F) << 4 | f.alternativeNametables << 3 |
                                        f.trainer << 2 | f.battery << 1 |
                                        static_cast<int>(f.verticalMirroring));
  header[7] = static_cast<std::uint8_t>((f.mapper & 0xF0) | (f.format & 0x03) << 2 |
                                        f.playchoice10 << 1 | static_cast<int>(f.vsSystem));
  header[8] = f.prgRamUnits;
  header[9] = f.pal ? 1 : 0;
  for (std::size_t i = 0; i < f.tail.size(); ++i) header[10 + i] = f.tail[i];
  return header;
}

// Builds a 16-byte NES 2.0 header; the size fields are the 12-bit values as written so
// a case states the exponent form directly.
struct Nes2Fields {
  std::uint16_t prgSize = 1;             // 12 bits: byte 4 and byte 9's low nibble
  std::uint16_t chrSize = 1;             // 12 bits: byte 5 and byte 9's high nibble
  std::uint16_t mapper = 0;              // 12 bits across flags 6, 7 and byte 8
  std::uint8_t submapper = 0;            // byte 8's high nibble
  bool verticalMirroring = false;
  bool battery = false;
  bool trainer = false;
  bool alternativeNametables = false;
  std::uint8_t consoleType = 0;          // flags 7 bits 1-0
  std::uint8_t prgRamShift = 0;          // byte 10 low nibble
  std::uint8_t prgNvramShift = 0;        // byte 10 high nibble
  std::uint8_t chrRamShift = 0;          // byte 11 low nibble
  std::uint8_t chrNvramShift = 0;        // byte 11 high nibble
  std::uint8_t timing = 0;               // byte 12 bits 1-0
  std::uint8_t consoleDetail = 0;        // byte 13
  std::uint8_t miscRoms = 0;             // byte 14 bits 1-0
  std::uint8_t expansionDevice = 0;      // byte 15 bits 6-0
};

[[nodiscard]] inline std::array<std::uint8_t, 16> nes2Header(const Nes2Fields& f) {
  std::array<std::uint8_t, 16> header{0x4E, 0x45, 0x53, 0x1A};
  header[4] = static_cast<std::uint8_t>(f.prgSize & 0xFF);
  header[5] = static_cast<std::uint8_t>(f.chrSize & 0xFF);
  header[6] = static_cast<std::uint8_t>((f.mapper & 0x0F) << 4 | f.alternativeNametables << 3 |
                                        f.trainer << 2 | f.battery << 1 |
                                        static_cast<int>(f.verticalMirroring));
  header[7] = static_cast<std::uint8_t>((f.mapper & 0xF0) | 0x08 | (f.consoleType & 0x03));
  header[8] = static_cast<std::uint8_t>(f.submapper << 4 | (f.mapper >> 8 & 0x0F));
  header[9] = static_cast<std::uint8_t>((f.chrSize >> 8 & 0x0F) << 4 | (f.prgSize >> 8 & 0x0F));
  header[10] = static_cast<std::uint8_t>(f.prgNvramShift << 4 | (f.prgRamShift & 0x0F));
  header[11] = static_cast<std::uint8_t>(f.chrNvramShift << 4 | (f.chrRamShift & 0x0F));
  header[12] = static_cast<std::uint8_t>(f.timing & 0x03);
  header[13] = f.consoleDetail;
  header[14] = static_cast<std::uint8_t>(f.miscRoms & 0x03);
  header[15] = static_cast<std::uint8_t>(f.expansionDevice & 0x7F);
  return header;
}

// The header followed by `prgBytes` of PRG ROM and `chrBytes` of CHR ROM, all zero but
// the three vectors at the top of the PRG area: NMI, reset and IRQ each pointing at
// `entry`. A trainer is never authored.
[[nodiscard]] inline std::vector<std::uint8_t> authoredImage(
    std::span<const std::uint8_t, 16> header, std::size_t prgBytes, std::size_t chrBytes,
    std::uint16_t entry = 0x8000) {
  std::vector<std::uint8_t> image(header.begin(), header.end());
  image.resize(header.size() + prgBytes + chrBytes);
  if (prgBytes >= 8192) {
    const std::size_t vectors = header.size() + prgBytes - 6;
    for (std::size_t slot = 0; slot < 3; ++slot) {
      image[vectors + slot * 2] = static_cast<std::uint8_t>(entry & 0xFF);
      image[vectors + slot * 2 + 1] = static_cast<std::uint8_t>(entry >> 8);
    }
  }
  return image;
}

// The byte at an offset into the PRG area, or into the CHR area, of an authored image.
[[nodiscard]] inline std::uint8_t& prgAt(std::vector<std::uint8_t>& image, std::size_t offset) {
  return image[16 + offset];
}
[[nodiscard]] inline std::uint8_t& chrAt(std::vector<std::uint8_t>& image, std::size_t prgBytes,
                                         std::size_t offset) {
  return image[16 + prgBytes + offset];
}

// Writes `bytes` into the PRG area starting at `offset`.
inline void placeProgram(std::vector<std::uint8_t>& image, std::size_t offset,
                         std::span<const std::uint8_t> bytes) {
  for (std::size_t i = 0; i < bytes.size(); ++i) prgAt(image, offset + i) = bytes[i];
}

}  // namespace neasant::nes_tests
