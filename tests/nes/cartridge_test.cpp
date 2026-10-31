#include "neasant/nes/board.h"
#include "neasant/nes/cartridge.h"
#include "authored_image.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The cartridge as a value: both header formats read byte by byte, the image sliced into
// a cartridge with a host's overrides applied, every refusal by its message, and NROM on
// the pins. Each case names the NESdev wiki page and section it is read from: "iNES",
// "NES 2.0", "NROM", "Mirroring" and "Cartridge connector".

namespace {

using neasant::Board;
using neasant::boardCpuCycle;
using neasant::boardFor;
using neasant::boardMirroring;
using neasant::boardPpuAccess;
using neasant::Cartridge;
using neasant::CartridgeHeader;
using neasant::CartridgeOverrides;
using neasant::ConsoleType;
using neasant::CpuPins;
using neasant::HeaderFormat;
using neasant::makeCartridge;
using neasant::Mirroring;
using neasant::Nrom;
using neasant::parseCartridgeHeader;
using neasant::PpuPins;
using neasant::Timing;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::chrAt;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::nes2Header;
using neasant::nes_tests::prgAt;

using Bytes = std::vector<std::uint8_t>;
using Header = std::array<std::uint8_t, 16>;
using CpuAnswer = std::pair<bool, std::uint8_t>;

// The header parsed from an image of it with the PRG and CHR sizes named.
CartridgeHeader parsedFrom(const Header& header, std::size_t prgBytes, std::size_t chrBytes) {
  const std::optional<CartridgeHeader> parsed =
      parseCartridgeHeader(authoredImage(header, prgBytes, chrBytes));
  EXPECT_TRUE(parsed.has_value());
  return parsed.value_or(CartridgeHeader{});
}

// The header parsed from its sixteen bytes alone.
CartridgeHeader parsedAlone(const Header& header) {
  const std::optional<CartridgeHeader> parsed = parseCartridgeHeader(header);
  EXPECT_TRUE(parsed.has_value());
  return parsed.value_or(CartridgeHeader{});
}

Cartridge cartridgeFrom(const Header& header, std::size_t prgBytes, std::size_t chrBytes,
                        const CartridgeOverrides& overrides = {}) {
  const Bytes image = authoredImage(header, prgBytes, chrBytes);
  return makeCartridge(image, overrides);
}

// The message makeCartridge refuses an image with.
std::string cartridgeRefusal(std::span<const std::uint8_t> image) {
  try {
    (void)makeCartridge(image);
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  return "(accepted)";
}

// The message boardFor refuses a cartridge with.
std::string boardRefusal(const Cartridge& cartridge) {
  try {
    (void)boardFor(cartridge);
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  return "(accepted)";
}

// A CPU read on the pins with the data lines holding $5A beforehand, so a read the
// board leaves alone shows it.
CpuAnswer readCpu(Board& board, const Cartridge& cartridge, std::uint16_t address,
                  std::span<std::uint8_t> prgRam) {
  CpuPins pins{.address = address, .data = 0x5A};
  const bool decoded = boardCpuCycle(board, cartridge, pins, prgRam);
  return {decoded, pins.data};
}

bool writeCpu(Board& board, const Cartridge& cartridge, std::uint16_t address,
              std::uint8_t value, std::span<std::uint8_t> prgRam) {
  CpuPins pins{.address = address, .data = value, .write = true};
  return boardCpuCycle(board, cartridge, pins, prgRam);
}

std::pair<bool, PpuPins> readPpu(Board& board, const Cartridge& cartridge,
                                 std::uint16_t address, std::span<std::uint8_t> chrRam) {
  PpuPins pins{.address = address, .data = 0x5A, .read = true};
  const bool answered = boardPpuAccess(board, cartridge, pins, chrRam);
  return {answered, pins};
}

std::pair<bool, PpuPins> writePpu(Board& board, const Cartridge& cartridge,
                                  std::uint16_t address, std::uint8_t value,
                                  std::span<std::uint8_t> chrRam) {
  PpuPins pins{.address = address, .data = value, .write = true};
  const bool answered = boardPpuAccess(board, cartridge, pins, chrRam);
  return {answered, pins};
}

// ---- HeaderParse ----

TEST(HeaderParse, ShorterThanAHeaderIsNone) {
  // NESdev wiki, "iNES", iNES file format: the header is 16 bytes.
  const Header header = inesHeader({});
  EXPECT_FALSE(parseCartridgeHeader(std::span(header).first(15)).has_value());
  EXPECT_TRUE(parseCartridgeHeader(header).has_value());
}

TEST(HeaderParse, MissingIdentifierIsNone) {
  // NESdev wiki, "iNES", bytes 0-3; "NES 2.0", Identification: "NES" followed by $1A.
  Header noEof = inesHeader({});
  noEof[3] = 0x00;
  Header lowercase = inesHeader({});
  lowercase[0] = 'n';
  EXPECT_FALSE(parseCartridgeHeader(noEof).has_value());
  EXPECT_FALSE(parseCartridgeHeader(lowercase).has_value());
}

TEST(HeaderParse, InesSizesAndFlags) {
  // NESdev wiki, "iNES", Flags 6, Flags 7 and Flags 8.
  const CartridgeHeader h = parsedFrom(inesHeader({.prgUnits = 2, .chrUnits = 1, .mapper = 33,
                                                   .verticalMirroring = true, .battery = true}),
                                       32768, 8192);
  EXPECT_EQ(h.format, HeaderFormat::Ines);
  EXPECT_FALSE(h.archaic);
  EXPECT_EQ(h.prgRomBytes, 32768u);
  EXPECT_EQ(h.chrRomBytes, 8192u);
  EXPECT_EQ(h.mapper, 33);
  EXPECT_EQ(h.submapper, 0);
  EXPECT_EQ(h.mirroring, Mirroring::Vertical);
  EXPECT_TRUE(h.battery);
  EXPECT_FALSE(h.trainer);
  EXPECT_FALSE(h.alternativeNametables);
  EXPECT_EQ(h.prgRamBytes, 0u);
  EXPECT_EQ(h.prgNvramBytes, 8192u);
  EXPECT_EQ(h.chrRamBytes, 0u);
  EXPECT_EQ(h.consoleType, ConsoleType::Nes);
  EXPECT_EQ(h.timing, Timing::Ntsc);
  EXPECT_EQ(h.bytes[6], 0x13);
  EXPECT_EQ(h.bytes[7], 0x20);
}

TEST(HeaderParse, InesChrZeroIsChrRam) {
  // NESdev wiki, "iNES", byte 5: "value 0 means the board uses CHR RAM"; "NROM", Variants.
  const CartridgeHeader h = parsedFrom(inesHeader({.chrUnits = 0}), 16384, 0);
  EXPECT_EQ(h.chrRomBytes, 0u);
  EXPECT_EQ(h.chrRamBytes, 8192u);
  EXPECT_EQ(h.chrNvramBytes, 0u);
}

TEST(HeaderParse, InesPrgRamUnits) {
  // NESdev wiki, "iNES", Flags 8: PRG RAM in 8 KB units, "Value 0 infers 8 KB".
  const CartridgeHeader two = parsedAlone(inesHeader({.prgRamUnits = 2}));
  EXPECT_EQ(two.prgRamBytes, 16384u);
  EXPECT_EQ(two.prgNvramBytes, 0u);
  const CartridgeHeader zero = parsedAlone(inesHeader({}));
  EXPECT_EQ(zero.prgRamBytes, 8192u);
  EXPECT_EQ(zero.prgNvramBytes, 0u);
}

TEST(HeaderParse, InesTrainerConsoleAndTiming) {
  // NESdev wiki, "iNES", Flags 6 bit 2, Flags 7 bits 0 and 1, Flags 9 bit 0.
  EXPECT_TRUE(parsedAlone(inesHeader({.trainer = true})).trainer);
  EXPECT_EQ(parsedAlone(inesHeader({.vsSystem = true})).consoleType, ConsoleType::VsSystem);
  EXPECT_EQ(parsedAlone(inesHeader({.playchoice10 = true})).consoleType,
            ConsoleType::Playchoice10);
  EXPECT_EQ(parsedAlone(inesHeader({.pal = true})).timing, Timing::Pal);
}

TEST(HeaderParse, InesAlternativeNametables) {
  // NESdev wiki, "iNES", Flags 6 bit 3: "Alternative nametable layout".
  const CartridgeHeader h = parsedAlone(inesHeader({.alternativeNametables = true}));
  EXPECT_TRUE(h.alternativeNametables);
  EXPECT_EQ(h.mirroring, Mirroring::Horizontal);
}

TEST(HeaderParse, DiskDudeTailIsArchaic) {
  // NESdev wiki, "iNES", the "DiskDude!" paragraph and Variant comparison: bytes 7-15
  // carry a signature, so only byte 6's nibble names the mapper.
  Header header = inesHeader({.mapper = 0x40});
  constexpr std::array<std::uint8_t, 9> kSignature{0x44, 0x69, 0x73, 0x6B, 0x44,
                                                   0x75, 0x64, 0x65, 0x21};
  for (std::size_t i = 0; i < kSignature.size(); ++i) header[7 + i] = kSignature[i];
  const CartridgeHeader h = parsedAlone(header);
  EXPECT_EQ(h.format, HeaderFormat::Ines);
  EXPECT_TRUE(h.archaic);
  EXPECT_EQ(h.mapper, 0);
  EXPECT_EQ(h.consoleType, ConsoleType::Nes);
  EXPECT_EQ(h.timing, Timing::Ntsc);
  EXPECT_EQ(h.prgRamBytes, 8192u);
}

TEST(HeaderParse, NonZeroTailIsArchaic) {
  // NESdev wiki, "iNES", Variant comparison, step 3: iNES needs bytes 12-15 all zero.
  Header header = inesHeader({.mapper = 0x21});
  header[15] = 1;
  const CartridgeHeader h = parsedAlone(header);
  EXPECT_TRUE(h.archaic);
  EXPECT_EQ(h.mapper, 1);
}

TEST(HeaderParse, Byte7BitsAreArchaic) {
  // NESdev wiki, "iNES", Variant comparison, steps 2 and 4.
  const CartridgeHeader one = parsedAlone(inesHeader({.mapper = 0x21, .format = 1}));
  EXPECT_TRUE(one.archaic);
  EXPECT_EQ(one.mapper, 1);
  EXPECT_TRUE(parsedAlone(inesHeader({.format = 3})).archaic);
}

TEST(HeaderParse, Nes2Identifier) {
  // NESdev wiki, "NES 2.0", Identification: byte 7 AND $0C is $08.
  const CartridgeHeader h = parsedFrom(nes2Header({}), 16384, 8192);
  EXPECT_EQ(h.format, HeaderFormat::Nes2);
  EXPECT_FALSE(h.archaic);
  EXPECT_EQ(h.bytes[7] & 0x0C, 0x08);
}

TEST(HeaderParse, Nes2MapperAndSubmapper) {
  // NESdev wiki, "NES 2.0", bytes 6-8: the mapper's twelve bits and the submapper.
  const CartridgeHeader h =
      parsedFrom(nes2Header({.mapper = 0x214, .submapper = 5}), 16384, 8192);
  EXPECT_EQ(h.mapper, 532);
  EXPECT_EQ(h.submapper, 5);
  EXPECT_EQ(h.bytes[8], 0x52);
}

TEST(HeaderParse, Nes2SimpleSizes) {
  // NESdev wiki, "NES 2.0", PRG-ROM Area and CHR-ROM Area: twelve bits of units.
  const CartridgeHeader h =
      parsedFrom(nes2Header({.prgSize = 0x134, .chrSize = 0x202}), 5046272, 4210688);
  EXPECT_EQ(h.prgRomBytes, 5046272u);
  EXPECT_EQ(h.chrRomBytes, 4210688u);
  EXPECT_EQ(h.bytes[9], 0x21);
}

TEST(HeaderParse, Nes2ExponentSizes) {
  // NESdev wiki, "NES 2.0", PRG-ROM Area: 2^E * (MM*2+1); "NROM", Variants (Galaxian).
  const CartridgeHeader h =
      parsedFrom(nes2Header({.prgSize = 0xF34, .chrSize = 0xF39}), 8192, 49152);
  EXPECT_EQ(h.prgRomBytes, 8192u);
  EXPECT_EQ(h.chrRomBytes, 49152u);
}

TEST(HeaderParse, Nes2RamShifts) {
  // NESdev wiki, "NES 2.0", bytes 10 and 11: 64 << shift; CHR-(NV)RAM: no CHR ROM does
  // not imply CHR RAM.
  const CartridgeHeader h = parsedFrom(
      nes2Header({.prgRamShift = 7, .prgNvramShift = 9, .chrRamShift = 7}), 16384, 8192);
  EXPECT_EQ(h.prgRamBytes, 8192u);
  EXPECT_EQ(h.prgNvramBytes, 32768u);
  EXPECT_EQ(h.chrRamBytes, 8192u);
  EXPECT_EQ(h.chrNvramBytes, 0u);
  const CartridgeHeader noChr = parsedFrom(nes2Header({.chrSize = 0}), 16384, 0);
  EXPECT_EQ(noChr.chrRomBytes, 0u);
  EXPECT_EQ(noChr.chrRamBytes, 0u);
}

TEST(HeaderParse, Nes2NeedsTheImageToFit) {
  // NESdev wiki, "iNES", Variant comparison, step 1: the declared sizes must fit.
  const Header header = nes2Header({.prgSize = 2});
  const CartridgeHeader shortImage = parsedFrom(header, 16384, 8192);
  EXPECT_EQ(shortImage.format, HeaderFormat::Ines);
  EXPECT_TRUE(shortImage.archaic);
  const CartridgeHeader alone = parsedAlone(header);
  EXPECT_EQ(alone.format, HeaderFormat::Ines);
  EXPECT_TRUE(alone.archaic);
}

TEST(HeaderParse, Nes2ConsoleTimingAndTail) {
  // NESdev wiki, "NES 2.0", byte 7 and bytes 12-15.
  const CartridgeHeader vs = parsedFrom(nes2Header({.consoleType = 1, .timing = 1,
                                                    .consoleDetail = 0x12, .miscRoms = 2,
                                                    .expansionDevice = 0x23}),
                                        16384, 8192);
  EXPECT_EQ(vs.consoleType, ConsoleType::VsSystem);
  EXPECT_EQ(vs.timing, Timing::Pal);
  EXPECT_EQ(vs.consoleDetail, 0x12);
  EXPECT_EQ(vs.miscRoms, 2);
  EXPECT_EQ(vs.defaultExpansionDevice, 0x23);
  const CartridgeHeader extended =
      parsedFrom(nes2Header({.consoleType = 3, .timing = 3}), 16384, 8192);
  EXPECT_EQ(extended.consoleType, ConsoleType::Extended);
  EXPECT_EQ(extended.timing, Timing::Dendy);
  EXPECT_EQ(parsedFrom(nes2Header({.timing = 2}), 16384, 8192).timing, Timing::Multiple);
}

TEST(HeaderParse, Nes2SizesNoImageHoldsAreArchaic) {
  // NESdev wiki, "NES 2.0", PRG-ROM Area and CHR-ROM Area; "iNES", Variant comparison,
  // step 1. Each exponent-form size is 2^63 bytes, so the two together pass 64 bits; the
  // header does not fit and reads as archaic iNES, which makeCartridge refuses by size.
  const Header header = nes2Header({.prgSize = 0xFFC, .chrSize = 0xFFC});
  const CartridgeHeader h = parsedAlone(header);
  EXPECT_EQ(h.format, HeaderFormat::Ines);
  EXPECT_TRUE(h.archaic);
  EXPECT_EQ(cartridgeRefusal(header), "the image is 6193152 bytes short of the header's sizes");
}

// ---- CartridgeValue ----

TEST(CartridgeValue, SlicesPrgAndChr) {
  // NESdev wiki, "iNES", iNES file format; "NES 2.0", Miscellaneous ROM Area: bytes after
  // the CHR area are not part of it.
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  prgAt(image, 0) = 0xA1;
  prgAt(image, 16383) = 0xA2;
  chrAt(image, 16384, 0) = 0xC1;
  chrAt(image, 16384, 8191) = 0xC2;
  image.insert(image.end(), std::size_t{128}, std::uint8_t{0xFF});
  const Cartridge c = makeCartridge(image);
  EXPECT_EQ(c.prgRom.size(), 16384u);
  EXPECT_EQ(c.prgRom[0], 0xA1);
  EXPECT_EQ(c.prgRom[16383], 0xA2);
  EXPECT_EQ(c.chrRom.size(), 8192u);
  EXPECT_EQ(c.chrRom[0], 0xC1);
  EXPECT_EQ(c.chrRom[8191], 0xC2);
  EXPECT_EQ(c.mapper, 0);
  EXPECT_EQ(c.mirroring, Mirroring::Horizontal);
  EXPECT_EQ(c.prgRamBytes, 8192u);
  EXPECT_EQ(c.chrRamBytes, 0u);
  EXPECT_FALSE(c.battery);
  EXPECT_EQ(c.header.format, HeaderFormat::Ines);
}

TEST(CartridgeValue, Refusals) {
  // NESdev wiki, "iNES", Flags 6 and Flags 7; "NES 2.0", Trainer Area and byte 7.
  const Header plain = inesHeader({});
  EXPECT_EQ(cartridgeRefusal(std::span(plain).first(15)),
            "an image of 15 bytes holds no 16-byte header");
  Bytes noHeader = authoredImage(plain, 16384, 8192);
  noHeader[3] = 0x00;
  EXPECT_EQ(cartridgeRefusal(noHeader), "the image carries no NES header");
  EXPECT_EQ(cartridgeRefusal(authoredImage(inesHeader({.trainer = true}), 16384, 8192)),
            "a trainer is not supported");
  EXPECT_EQ(cartridgeRefusal(authoredImage(inesHeader({.vsSystem = true}), 16384, 8192)),
            "a Vs. System image is not supported; NES and Famicom images are");
  EXPECT_EQ(cartridgeRefusal(authoredImage(inesHeader({.playchoice10 = true}), 16384, 8192)),
            "a Playchoice 10 image is not supported; NES and Famicom images are");
  EXPECT_EQ(cartridgeRefusal(authoredImage(nes2Header({.consoleType = 3}), 16384, 8192)),
            "an extended-console image is not supported; NES and Famicom images are");
  EXPECT_EQ(cartridgeRefusal(authoredImage(inesHeader({.prgUnits = 2}), 16384, 8192)),
            "the image is 16384 bytes short of the header's sizes");
}

TEST(CartridgeValue, OverridesReplaceTheHeader) {
  // NESdev wiki, "iNES", Flags 6: the host's values stand over the header's.
  const Cartridge c = cartridgeFrom(inesHeader({.mapper = 4, .alternativeNametables = true}),
                                    16384, 8192,
                                    {.mapper = std::uint16_t{0},
                                     .mirroring = Mirroring::Vertical,
                                     .prgRamBytes = std::size_t{0},
                                     .chrRamBytes = std::size_t{0},
                                     .battery = true});
  EXPECT_EQ(c.mapper, 0);
  EXPECT_EQ(c.mirroring, Mirroring::Vertical);
  EXPECT_FALSE(c.alternativeNametables);
  EXPECT_EQ(c.prgRamBytes, 0u);
  EXPECT_EQ(c.chrRamBytes, 0u);
  EXPECT_TRUE(c.battery);
  EXPECT_EQ(c.header.mapper, 4);
  EXPECT_TRUE(c.header.alternativeNametables);
}

TEST(CartridgeValue, RamTotalsSumBothHalves) {
  // NESdev wiki, "NES 2.0", byte 10; "NROM", Variants: one PRG RAM chip.
  const Cartridge c = cartridgeFrom(
      nes2Header({.battery = true, .prgRamShift = 5, .prgNvramShift = 5}), 16384, 8192);
  EXPECT_EQ(c.prgRamBytes, 4096u);
  EXPECT_TRUE(c.battery);
}

// ---- NromBoard ----

TEST(NromBoard, BoardForRefusals) {
  // NESdev wiki, "NROM", the board table and Banks.
  EXPECT_EQ(boardRefusal(cartridgeFrom(inesHeader({.mapper = 4}), 16384, 8192)),
            "mapper 4 is not built; NROM (0) is");
  EXPECT_EQ(boardRefusal(cartridgeFrom(inesHeader({.prgUnits = 3}), 49152, 8192)),
            "NROM carries 8, 16 or 32 KB of PRG ROM; the header declares 49152 bytes");
  EXPECT_EQ(boardRefusal(cartridgeFrom(inesHeader({.prgUnits = 0}), 0, 8192)),
            "NROM carries 8, 16 or 32 KB of PRG ROM; the header declares 0 bytes");
  EXPECT_EQ(boardRefusal(cartridgeFrom(nes2Header({.chrSize = 2}), 16384, 16384)),
            "NROM carries 8 KB of CHR, ROM or RAM; the header declares 16384 bytes of CHR "
            "ROM and 0 of CHR RAM");
  EXPECT_EQ(boardRefusal(cartridgeFrom(nes2Header({.chrSize = 0}), 16384, 0)),
            "NROM carries 8 KB of CHR, ROM or RAM; the header declares 0 bytes of CHR ROM "
            "and 0 of CHR RAM");
  EXPECT_EQ(boardRefusal(cartridgeFrom(inesHeader({}), 16384, 8192,
                                       {.chrRamBytes = std::size_t{8192}})),
            "NROM carries 8 KB of CHR, ROM or RAM; the header declares 8192 bytes of CHR "
            "ROM and 8192 of CHR RAM");
  EXPECT_EQ(boardRefusal(cartridgeFrom(inesHeader({.prgRamUnits = 2}), 16384, 8192)),
            "NROM's PRG RAM window holds at most 8 KB in a power-of-two size; the header "
            "declares 16384 bytes");
  EXPECT_EQ(boardRefusal(cartridgeFrom(inesHeader({}), 16384, 8192,
                                       {.prgRamBytes = std::size_t{3000}})),
            "NROM's PRG RAM window holds at most 8 KB in a power-of-two size; the header "
            "declares 3000 bytes");
  EXPECT_EQ(boardRefusal(cartridgeFrom(inesHeader({}), 16384, 8192,
                                       {.mirroring = Mirroring::FourScreen})),
            "NROM has no four-screen RAM");
  EXPECT_EQ(boardRefusal(cartridgeFrom(inesHeader({.alternativeNametables = true}), 16384,
                                       8192)),
            "NROM has no alternative nametable layout; a mirroring in the config overrides "
            "the header's bits");
}

TEST(NromBoard, BoardForAcceptsEveryNromShape) {
  // NESdev wiki, "NROM", the board table and Variants.
  const std::vector<Cartridge> shapes{
      cartridgeFrom(inesHeader({}), 16384, 8192),
      cartridgeFrom(inesHeader({.prgUnits = 2}), 32768, 8192),
      cartridgeFrom(nes2Header({.prgSize = 0xF34}), 8192, 8192),
      cartridgeFrom(inesHeader({.chrUnits = 0}), 16384, 0),
      cartridgeFrom(nes2Header({.prgRamShift = 5}), 16384, 8192),
      cartridgeFrom(inesHeader({.alternativeNametables = true}), 16384, 8192,
                    {.mirroring = Mirroring::Horizontal}),
  };
  for (const Cartridge& c : shapes) {
    EXPECT_TRUE(std::holds_alternative<Nrom>(boardFor(c)));
  }
}

TEST(NromBoard, PrgRom32K) {
  // NESdev wiki, "NROM", Banks: the first 16 KB at $8000, the last at $C000.
  Bytes image = authoredImage(inesHeader({.prgUnits = 2}), 32768, 8192);
  prgAt(image, 0) = 0x01;
  prgAt(image, 16383) = 0x02;
  prgAt(image, 16384) = 0x03;
  prgAt(image, 32767) = 0x04;
  const Cartridge c = makeCartridge(image);
  Board board = boardFor(c);
  Bytes prgRam(8192);
  EXPECT_EQ(readCpu(board, c, 0x8000, prgRam), (CpuAnswer{true, 0x01}));
  EXPECT_EQ(readCpu(board, c, 0xBFFF, prgRam), (CpuAnswer{true, 0x02}));
  EXPECT_EQ(readCpu(board, c, 0xC000, prgRam), (CpuAnswer{true, 0x03}));
  EXPECT_EQ(readCpu(board, c, 0xFFFF, prgRam), (CpuAnswer{true, 0x04}));
}

TEST(NromBoard, PrgRom16KRepeats) {
  // NESdev wiki, "NROM", Banks; "Mirroring", Memory Mirroring: NROM-128's 16 KB at both
  // $8000 and $C000.
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  prgAt(image, 0) = 0x11;
  const Cartridge c = makeCartridge(image);
  Board board = boardFor(c);
  Bytes prgRam(8192);
  EXPECT_EQ(readCpu(board, c, 0xC000, prgRam), (CpuAnswer{true, 0x11}));
  EXPECT_EQ(readCpu(board, c, 0x8000, prgRam), (CpuAnswer{true, 0x11}));
  EXPECT_EQ(readCpu(board, c, 0xFFFC, prgRam), (CpuAnswer{true, 0x00}));
  EXPECT_EQ(readCpu(board, c, 0xFFFD, prgRam), (CpuAnswer{true, 0x80}));
}

TEST(NromBoard, PrgRom8KRepeatsFourTimes) {
  // NESdev wiki, "NROM", Variants (Galaxian); "Mirroring", Memory Mirroring.
  Bytes image = authoredImage(nes2Header({.prgSize = 0xF34}), 8192, 8192);
  prgAt(image, 0) = 0x21;
  const Cartridge c = makeCartridge(image);
  Board board = boardFor(c);
  Bytes prgRam;
  for (const std::uint16_t address :
       std::initializer_list<std::uint16_t>{0x8000, 0xA000, 0xC000, 0xE000}) {
    EXPECT_EQ(readCpu(board, c, address, prgRam), (CpuAnswer{true, 0x21}));
  }
}

TEST(NromBoard, WriteToPrgRomChangesNothing) {
  // NESdev wiki, "NROM", Registers: no mapping logic.
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  prgAt(image, 0) = 0x31;
  const Cartridge c = makeCartridge(image);
  Board board = boardFor(c);
  Bytes prgRam(8192);
  EXPECT_TRUE(writeCpu(board, c, 0x8000, 0x99, prgRam));
  EXPECT_EQ(c.prgRom[0], 0x31);
  EXPECT_EQ(readCpu(board, c, 0x8000, prgRam), (CpuAnswer{true, 0x31}));
}

TEST(NromBoard, OpenBusBelowTheRom) {
  // NESdev wiki, "2A03", 2A03 register map: $4020-$FFFF is the cartridge's; "NROM", Banks:
  // nothing below $8000 without the RAM.
  const Cartridge c =
      cartridgeFrom(inesHeader({}), 16384, 8192, {.prgRamBytes = std::size_t{0}});
  Board board = boardFor(c);
  Bytes prgRam;
  for (const std::uint16_t address :
       std::initializer_list<std::uint16_t>{0x4020, 0x5FFF, 0x6000, 0x7FFF}) {
    EXPECT_EQ(readCpu(board, c, address, prgRam), (CpuAnswer{false, 0x5A}));
  }
}

TEST(NromBoard, PrgRamWindowAndItsMirrors) {
  // NESdev wiki, "NROM", Banks: PRG RAM "mirrored as necessary to fill entire 8 KiB window".
  const Cartridge full = cartridgeFrom(inesHeader({}), 16384, 8192);
  Board fullBoard = boardFor(full);
  Bytes fullRam(full.prgRamBytes);
  EXPECT_TRUE(writeCpu(fullBoard, full, 0x6000, 0x41, fullRam));
  EXPECT_TRUE(writeCpu(fullBoard, full, 0x7FFF, 0x42, fullRam));
  EXPECT_EQ(readCpu(fullBoard, full, 0x6000, fullRam), (CpuAnswer{true, 0x41}));
  EXPECT_EQ(readCpu(fullBoard, full, 0x7FFF, fullRam), (CpuAnswer{true, 0x42}));
  EXPECT_EQ(fullRam[0], 0x41);
  EXPECT_EQ(fullRam[8191], 0x42);

  const Cartridge small = cartridgeFrom(nes2Header({.prgRamShift = 5}), 16384, 8192);
  Board smallBoard = boardFor(small);
  Bytes smallRam(small.prgRamBytes);
  ASSERT_EQ(smallRam.size(), 2048u);
  EXPECT_TRUE(writeCpu(smallBoard, small, 0x6000, 0x43, smallRam));
  EXPECT_EQ(readCpu(smallBoard, small, 0x6800, smallRam), (CpuAnswer{true, 0x43}));
  EXPECT_EQ(readCpu(smallBoard, small, 0x7000, smallRam), (CpuAnswer{true, 0x43}));
  EXPECT_TRUE(writeCpu(smallBoard, small, 0x67FF, 0x44, smallRam));
  EXPECT_EQ(readCpu(smallBoard, small, 0x7FFF, smallRam), (CpuAnswer{true, 0x44}));
}

TEST(NromBoard, ChrRomOnThePpuPins) {
  // NESdev wiki, "NROM", Banks; "Cartridge connector", PPU A13 to the CHR chip's /CE.
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  chrAt(image, 16384, 0) = 0xC1;
  chrAt(image, 16384, 8191) = 0xC2;
  const Cartridge c = makeCartridge(image);
  Board board = boardFor(c);
  Bytes chrRam;
  const auto [first, firstPins] = readPpu(board, c, 0x0000, chrRam);
  EXPECT_TRUE(first);
  EXPECT_EQ(firstPins.data, 0xC1);
  EXPECT_FALSE(firstPins.ciramEnable);
  EXPECT_EQ(readPpu(board, c, 0x1FFF, chrRam).second.data, 0xC2);
  EXPECT_TRUE(writePpu(board, c, 0x0000, 0x99, chrRam).first);
  EXPECT_EQ(c.chrRom[0], 0xC1);
}

TEST(NromBoard, ChrRamOnThePpuPins) {
  // NESdev wiki, "NROM", Variants; "iNES", byte 5: CHR RAM when the CHR ROM size is 0.
  const Cartridge c = cartridgeFrom(inesHeader({.chrUnits = 0}), 16384, 0);
  Board board = boardFor(c);
  Bytes chrRam(c.chrRamBytes);
  EXPECT_TRUE(writePpu(board, c, 0x0123, 0x77, chrRam).first);
  const auto [answered, pins] = readPpu(board, c, 0x0123, chrRam);
  EXPECT_TRUE(answered);
  EXPECT_EQ(pins.data, 0x77);
  EXPECT_EQ(chrRam[0x123], 0x77);
}

TEST(NromBoard, CiramLinesUnderHorizontalMirroring) {
  // NESdev wiki, "Mirroring", Horizontal: CIRAM A10 = PPU A11; "Cartridge connector",
  // CIRAM A10 and CIRAM /CE.
  const Cartridge c = cartridgeFrom(inesHeader({}), 16384, 8192);
  Board board = boardFor(c);
  Bytes chrRam;
  const auto [answered, pins] = readPpu(board, c, 0x2000, chrRam);
  EXPECT_FALSE(answered);
  EXPECT_TRUE(pins.ciramEnable);
  EXPECT_FALSE(pins.ciramA10);
  EXPECT_EQ(pins.data, 0x5A);
  EXPECT_FALSE(readPpu(board, c, 0x2400, chrRam).second.ciramA10);
  EXPECT_TRUE(readPpu(board, c, 0x2800, chrRam).second.ciramA10);
  EXPECT_TRUE(readPpu(board, c, 0x2C00, chrRam).second.ciramA10);
  EXPECT_TRUE(readPpu(board, c, 0x3000, chrRam).second.ciramEnable);
  EXPECT_FALSE(readPpu(board, c, 0x1000, chrRam).second.ciramEnable);
}

TEST(NromBoard, CiramLinesUnderVerticalMirroring) {
  // NESdev wiki, "Mirroring", Vertical: CIRAM A10 = PPU A10; "iNES", Flags 6 bit 0.
  const Cartridge c = cartridgeFrom(inesHeader({.verticalMirroring = true}), 16384, 8192);
  Board board = boardFor(c);
  Bytes chrRam;
  const std::array<std::pair<std::uint16_t, bool>, 4> expected{
      {{0x2000, false}, {0x2400, true}, {0x2800, false}, {0x2C00, true}}};
  for (const auto& [address, a10] : expected) {
    const auto [answered, pins] = readPpu(board, c, address, chrRam);
    EXPECT_FALSE(answered);
    EXPECT_TRUE(pins.ciramEnable);
    EXPECT_EQ(pins.ciramA10, a10);
  }
}

TEST(NromBoard, MirroringAnswers) {
  // NESdev wiki, "NROM", Solder pad config: the mirroring is fixed by the board's wiring.
  const Cartridge horizontal = cartridgeFrom(inesHeader({}), 16384, 8192);
  const Cartridge vertical = cartridgeFrom(inesHeader({.verticalMirroring = true}), 16384, 8192);
  const Cartridge overridden =
      cartridgeFrom(inesHeader({}), 16384, 8192, {.mirroring = Mirroring::Vertical});
  EXPECT_EQ(boardMirroring(boardFor(horizontal), horizontal), Mirroring::Horizontal);
  EXPECT_EQ(boardMirroring(boardFor(vertical), vertical), Mirroring::Vertical);
  EXPECT_EQ(boardMirroring(boardFor(overridden), overridden), Mirroring::Vertical);
}

TEST(NromBoard, BoardIsAValue) {
  const Cartridge c = cartridgeFrom(inesHeader({}), 16384, 8192);
  const Board a = boardFor(c);
  const Board b = a;
  EXPECT_EQ(a, b);
  EXPECT_TRUE(std::holds_alternative<Nrom>(b));
  EXPECT_EQ(Nrom{}, Nrom{});
}

}  // namespace
