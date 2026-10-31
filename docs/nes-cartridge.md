# The cartridge

`cartridge.h` reads a cartridge image as a value: the header it carries, in one of two formats,
read byte by byte into a struct that keeps the bytes as written beside what they mean; the PRG ROM
and CHR ROM copied out of it; and the sizes a host's overrides settle. `board.h` builds the board
the header names from that value, and the board answers the console's accesses on the pins
declared here, each CPU cycle and each PPU access as it happens. NROM is the one board.

Each function works on values alone. A tool that wants the header reads it through
`parseCartridgeHeader` with no machine at all, and [the machine](nes-machine.md) builds its
cartridge and its board through the same functions, so the two never disagree about where a byte
is.

The status table in the [README](../README.md) carries the boards as a row.

## Contents

- [The surface](#the-surface)
- [The header](#the-header)
  - [iNES](#ines)
  - [NES 2.0](#nes-20)
  - [Archaic headers](#archaic-headers)
- [The cartridge value](#the-cartridge-value)
  - [The overrides](#the-overrides)
  - [What is refused](#what-is-refused)
- [The pins](#the-pins)
- [The board](#the-board)
- [NROM](#nrom)
  - [Where an address lands](#where-an-address-lands)
  - [Mirroring](#mirroring)
- [Gotchas](#gotchas)
- [What remains open](#what-remains-open)
- [See also](#see-also)
- [Where to look](#where-to-look)
- [Sources](#sources)

## The surface

Everything lives in `neasant`. The header is read into a value, the cartridge is built from an
image, the board is built from the cartridge, and the pins are two structs the board reads and
drives. The examples on this page build their images with one helper:

```cpp
#include "neasant/nes/board.h"
#include "neasant/nes/cartridge.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

// An image: the sixteen header bytes given, then 16 KB of PRG ROM and 8 KB of CHR ROM, every
// byte zero.
std::vector<std::uint8_t> imageWithHeader(std::initializer_list<std::uint8_t> header) {
  std::vector<std::uint8_t> image(header);
  image.resize(16 + 16384 + 8192);
  return image;
}
```

Every later example on this page uses this `imageWithHeader` and needs no further `#include`.

`HeaderFormat` says which of the two formats an image carries (NESdev wiki, "iNES", Variant
comparison; "NES 2.0", Identification). `Mirroring` says how the board wires CIRAM A10, which
decides which of the console's two nametable kilobytes an address reaches: `Horizontal` takes PPU
A11, so the two nametables sit one above the other, and `Vertical` takes PPU A10, so they sit side
by side. The names are the mirroring's, and the arrangement is the other way round: horizontal
mirroring stacks the nametables vertically. `FourScreen` is a board with its own nametable RAM,
which answers instead of the console's; a header's two layout bits name the first two, and a board
or a host's override names the third (NESdev wiki, "Mirroring", Nametable Mirroring; "Cartridge
connector", CIRAM A10). `ConsoleType` is the console an image is for (NESdev wiki, "NES 2.0", byte
7; "iNES", Flags 7). `Timing` is the CPU and PPU timing an image expects: an iNES header names NTSC
or PAL, and a NES 2.0 header also names a game that adjusts itself to either, and the Dendy
(NESdev wiki, "NES 2.0", CPU/PPU Timing; "iNES", Flags 9).

```cpp
enum class HeaderFormat : std::uint8_t { Ines, Nes2 };

enum class Mirroring : std::uint8_t { Horizontal, Vertical, FourScreen };

enum class ConsoleType : std::uint8_t { Nes, VsSystem, Playchoice10, Extended };

enum class Timing : std::uint8_t { Ntsc, Pal, Multiple, Dendy };

inline constexpr std::size_t kHeaderBytes = 16;
inline constexpr std::size_t kTrainerBytes = 512;
```

## The header

An image is a header followed by its PRG ROM and CHR ROM; the header is sixteen bytes, `NES` and
`$1A` first, in one of two formats (NESdev wiki, "iNES", iNES file format; "NES 2.0",
Identification). `parseCartridgeHeader` reads it into `CartridgeHeader`, which keeps the bytes as
written beside what they mean. Under iNES the sizes are in units of 16 KB and 8 KB, PRG RAM is
8 KB unless byte 8 says otherwise, and a CHR ROM size of zero means 8 KB of CHR RAM. Under NES 2.0
the sizes carry four more bits each, with an exponent form for odd sizes, every RAM has its own
shift count, and the console type, the timing and the expansion device are stated.

```cpp
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
```

`parseCartridgeHeader` answers nothing for a span shorter than a header or one that does not open
with `NES` and `$1A`. Because its check of the sizes reads the whole span, the sixteen header bytes
handed in alone never read as NES 2.0.

```cpp
[[nodiscard]] std::optional<CartridgeHeader> parseCartridgeHeader(
    std::span<const std::uint8_t> image) noexcept;
```

The format is decided in this order (NESdev wiki, "iNES", Variant comparison; "NES 2.0",
Identification):

1. Byte 7's bits 3–2 are `%10` and the PRG and CHR sizes the header declares, with the trainer
   and the header, fit inside the span: NES 2.0.
2. Byte 7's bits 3–2 are `%01`: archaic iNES.
3. Byte 7's bits 3–2 are `%00` and bytes 12–15 are all zero: iNES.
4. Otherwise: archaic iNES.

### iNES

An iNES header's sixteen bytes:

| Byte | Field |
|---|---|
| 0–3 | `NES` and `$1A` |
| 4 | PRG ROM in 16 KB units |
| 5 | CHR ROM in 8 KB units; 0 means the board carries 8 KB of CHR RAM |
| 6 | bit 0 the mirroring, 0 horizontal and 1 vertical; bit 1 a battery behind the PRG RAM; bit 2 a trainer; bit 3 the alternative nametable layout; bits 7–4 the mapper's low nibble |
| 7 | bit 0 a Vs. System image; bit 1 a PlayChoice-10 image; bits 3–2 the format; bits 7–4 the mapper's high nibble |
| 8 | PRG RAM in 8 KB units; 0 means 8 KB |
| 9 | bit 0 the timing, 0 NTSC and 1 PAL |
| 10–15 | padding; byte 10 is unofficial and is not read |

(NESdev wiki, "iNES", iNES file format, Flags 6, Flags 7, Flags 8, Flags 9.) The PRG RAM is
reported as battery-backed when flags 6 bit 1 is set and as volatile otherwise, 8 KB either way
when byte 8 is 0. A 32 KB image on mapper 33 with vertical mirroring and a battery:

```cpp
std::vector<std::uint8_t> image = imageWithHeader({
    0x4E, 0x45, 0x53, 0x1A,  // "NES" and $1A
    2,                       // byte 4: PRG ROM in 16 KB units
    1,                       // byte 5: CHR ROM in 8 KB units
    0x13,                    // flags 6: vertical mirroring, a battery, mapper 33's low nibble
    0x20,                    // flags 7: mapper 33's high nibble; bits 3-2 clear, so iNES
    0, 0, 0, 0, 0, 0, 0, 0,  // bytes 8-15: 8 KB of PRG RAM by default, NTSC, padding
});
image.resize(16 + 32768 + 8192);
const std::optional<neasant::CartridgeHeader> parsed = neasant::parseCartridgeHeader(image);
// parsed->format == neasant::HeaderFormat::Ines and parsed->archaic == false
// parsed->prgRomBytes == 32768 and parsed->chrRomBytes == 8192
// parsed->mapper == 33
// parsed->mirroring == neasant::Mirroring::Vertical
// parsed->battery == true
// parsed->prgNvramBytes == 8192 and parsed->prgRamBytes == 0: the battery makes the 8 KB persist
// parsed->chrRamBytes == 0
// parsed->bytes[6] == 0x13: the bytes as written
```

### NES 2.0

A NES 2.0 header keeps bytes 0–6 as iNES has them and gives bytes 7–15 fields of their own:

| Byte | Field |
|---|---|
| 0–6 | as iNES |
| 7 | bits 1–0 the console: 0 an NES or Famicom, 1 a Vs. System, 2 a PlayChoice-10, 3 an extended console; bits 3–2 `%10`, the identifier; bits 7–4 the mapper's middle nibble |
| 8 | bits 3–0 the mapper's high nibble; bits 7–4 the submapper |
| 9 | bits 3–0 the PRG ROM size's high nibble; bits 7–4 the CHR ROM size's. A nibble of `$F` names the exponent form: the low byte is `EEEEEEMM` and the size is 2^E × (MM × 2 + 1) bytes |
| 10 | bits 3–0 the PRG RAM's shift count; bits 7–4 the battery-backed PRG RAM's; a count of 0 is none, else 64 << count bytes |
| 11 | the same for CHR RAM and battery-backed CHR RAM |
| 12 | bits 1–0 the timing: 0 NTSC, 1 PAL, 2 a game that adjusts to either, 3 Dendy |
| 13 | the Vs. System's or extended console's type, as written |
| 14 | bits 1–0 the count of ROMs after the CHR area |
| 15 | bits 6–0 the default expansion device |

(NESdev wiki, "NES 2.0", Identification, Header, PRG-ROM Area, CHR-ROM Area, PRG-(NV)RAM,
CHR-(NV)RAM, CPU/PPU Timing.) A NES 2.0 header declares every RAM, so one that declares no CHR ROM
and no CHR RAM carries none, where an iNES header's 0 means 8 KB of CHR RAM. The size clause of
the detection procedure reads the whole span, so the sixteen header bytes handed in alone never
read as NES 2.0:

```cpp
std::vector<std::uint8_t> image = imageWithHeader({
    0x4E, 0x45, 0x53, 0x1A,
    1, 1,   // 16 KB of PRG ROM, 8 KB of CHR ROM
    0x02,   // flags 6: horizontal mirroring, a battery, mapper 0
    0x08,   // flags 7: bits 3-2 are %10, the NES 2.0 identifier; bits 1-0 an NES or Famicom
    0x00,   // byte 8: the mapper's high nibble 0, submapper 0
    0x00,   // byte 9: the high nibbles of the two sizes
    0x70,   // byte 10: 64 << 7 = 8 KB of battery-backed PRG RAM, no volatile PRG RAM
    0x00,   // byte 11: no CHR RAM
    0x01,   // byte 12: PAL timing
    0, 0, 0,
});
const std::optional<neasant::CartridgeHeader> parsed = neasant::parseCartridgeHeader(image);
// parsed->format == neasant::HeaderFormat::Nes2
// parsed->prgNvramBytes == 8192 and parsed->prgRamBytes == 0
// parsed->chrRamBytes == 0: a NES 2.0 header declares every RAM, and this one declares none
// parsed->timing == neasant::Timing::Pal and parsed->battery == true

const std::optional<neasant::CartridgeHeader> alone =
    neasant::parseCartridgeHeader(std::span(image).first(16));
// alone->format == neasant::HeaderFormat::Ines and alone->archaic == true: the sizes a NES 2.0
// header declares must fit inside the span, and sixteen bytes hold no image
```

### Archaic headers

A header that is neither NES 2.0 nor the third step's iNES is archaic: bytes 7–15 carry a
signature or padding rather than fields, so they are not read. The mapper is byte 6's nibble
alone, the console the NES, the PRG RAM 8 KB and the timing NTSC, and `archaic` on the header
says so (NESdev wiki, "iNES", the "DiskDude!" paragraph and Variant comparison).

```cpp
std::vector<std::uint8_t> image = imageWithHeader({
    0x4E, 0x45, 0x53, 0x1A, 1, 1,
    0x40,                                                   // flags 6: mapper 4's low nibble
    0x44, 0x69, 0x73, 0x6B, 0x44, 0x75, 0x64, 0x65, 0x21,   // bytes 7-15: "DiskDude!"
});
const std::optional<neasant::CartridgeHeader> parsed = neasant::parseCartridgeHeader(image);
// parsed->archaic == true: byte 7's bits 3-2 are not clear and bytes 12-15 are not all zero
// parsed->mapper == 4: byte 6's nibble alone
// parsed->prgRamBytes == 8192, parsed->consoleType == neasant::ConsoleType::Nes and
// parsed->timing == neasant::Timing::Ntsc: the tail is not read
```

## The cartridge value

`makeCartridge` builds a `Cartridge` from an image: its header, the PRG ROM and CHR ROM copied out
of the image in that order, and the values the board is built from once the overrides are applied.
The two RAM sizes sum the volatile and battery-backed halves, and `battery` says whether the
machine reports the PRG RAM as the save. Bytes after the CHR area are not part of the cartridge
(NESdev wiki, "iNES", iNES file format; "NES 2.0", Miscellaneous ROM Area).

```cpp
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
```

`makeCartridge` throws `std::invalid_argument`, naming what is supported instead, for an image it
cannot read ([What is refused](#what-is-refused)).

```cpp
[[nodiscard]] Cartridge makeCartridge(std::span<const std::uint8_t> image,
                                      const CartridgeOverrides& overrides = {});
```

An image with its first and last PRG bytes and its first CHR byte marked, and one byte past its
CHR area:

```cpp
std::vector<std::uint8_t> image =
    imageWithHeader({0x4E, 0x45, 0x53, 0x1A, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
image[16] = 0xA1;          // the first PRG byte
image[16 + 16383] = 0xA2;  // the last
image[16 + 16384] = 0xC1;  // the first CHR byte
image.push_back(0xFF);     // a byte past the CHR area is not part of the cartridge
const neasant::Cartridge cartridge = neasant::makeCartridge(image);
// cartridge.prgRom.size() == 16384, cartridge.prgRom[0] == 0xA1 and [16383] == 0xA2
// cartridge.chrRom.size() == 8192 and cartridge.chrRom[0] == 0xC1
// cartridge.mapper == 0 and cartridge.mirroring == neasant::Mirroring::Horizontal
// cartridge.prgRamBytes == 8192, cartridge.chrRamBytes == 0 and cartridge.battery == false
// cartridge.header.format == neasant::HeaderFormat::Ines
```

### The overrides

A host states what it knows over the header: the mapper, the mirroring, the two RAM sizes, 0 for
none, and the battery flag. A field left empty takes the header's value, and the header on the
cartridge is kept as read. A mirroring the host names replaces the header's two layout bits, so
the alternative-nametable bit is dropped with it.

```cpp
struct CartridgeOverrides {
  std::optional<std::uint16_t> mapper{};
  std::optional<Mirroring> mirroring{};
  std::optional<std::size_t> prgRamBytes{};
  std::optional<std::size_t> chrRamBytes{};
  std::optional<bool> battery{};
};
```

An image naming mapper 4 and the alternative-nametable bit, built as NROM with vertical mirroring,
no PRG RAM and a battery:

```cpp
// flags 6 $48: mapper 4's low nibble and the alternative-nametable bit
std::vector<std::uint8_t> image =
    imageWithHeader({0x4E, 0x45, 0x53, 0x1A, 1, 1, 0x48, 0, 0, 0, 0, 0, 0, 0, 0, 0});
const neasant::Cartridge overridden = neasant::makeCartridge(
    image, neasant::CartridgeOverrides{.mapper = std::uint16_t{0},
                                       .mirroring = neasant::Mirroring::Vertical,
                                       .prgRamBytes = std::size_t{0},
                                       .battery = true});
// overridden.mapper == 0 and overridden.header.mapper == 4: the header is kept as read
// overridden.mirroring == neasant::Mirroring::Vertical
// overridden.alternativeNametables == false: a mirroring the host names replaces both bits
// overridden.prgRamBytes == 0 and overridden.chrRamBytes == 0
// overridden.battery == true
```

### What is refused

`makeCartridge` and `boardFor` throw `std::invalid_argument` naming what is supported instead:

| | Message |
|---|---|
| an image shorter than a header | `an image of 15 bytes holds no 16-byte header` |
| an image without the identifier | `the image carries no NES header` |
| a trainer | `a trainer is not supported` |
| a Vs. System image | `a Vs. System image is not supported; NES and Famicom images are` |
| a PlayChoice-10 image | `a Playchoice 10 image is not supported; NES and Famicom images are` |
| an extended-console image | `an extended-console image is not supported; NES and Famicom images are` |
| an image shorter than its header's sizes | `the image is 16384 bytes short of the header's sizes` |
| a mapper other than 0 | `mapper 4 is not built; NROM (0) is` |
| PRG ROM other than 8, 16 or 32 KB | `NROM carries 8, 16 or 32 KB of PRG ROM; the header declares 49152 bytes` |
| CHR other than 8 KB of ROM or of RAM | `NROM carries 8 KB of CHR, ROM or RAM; the header declares 16384 bytes of CHR ROM and 0 of CHR RAM` |
| PRG RAM above 8 KB or not a power of two | `NROM's PRG RAM window holds at most 8 KB in a power-of-two size; the header declares 16384 bytes` |
| four-screen mirroring | `NROM has no four-screen RAM` |
| the alternative-nametable bit with no mirroring override | `NROM has no alternative nametable layout; a mirroring in the config overrides the header's bits` |

A message carries the figure the header or the image holds; the figures above are one case each.
An image whose header declares 32 KB of PRG ROM over 16 KB of bytes:

```cpp
std::vector<std::uint8_t> image =
    imageWithHeader({0x4E, 0x45, 0x53, 0x1A, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
try {
  const neasant::Cartridge cartridge = neasant::makeCartridge(image);  // 32 KB declared
} catch (const std::invalid_argument& refusal) {
  // refusal.what() is "the image is 16384 bytes short of the header's sizes"
}
```

## The pins

The board sees the connector's pins and nothing else. On the CPU side it is presented once per
CPU cycle with the address and R/W, stable for the whole period M2 is high, and the data as the
bus carries it at M2's fall. On a write the console drives the data lines; on a read the board
drives them when it decodes the address and leaves them alone when it does not. `/ROMSEL` is the
console's NAND of M2 and A15, low for `$8000-$FFFF` while the address is stable. On the PPU side
the board is presented once per PPU access with the 14-bit address, `/RD` or `/WR`, and the data
lines. It drives the two CIRAM lines on every presentation, `CIRAM /CE` enabling the console's
2 KB of nametable RAM and `CIRAM A10` selecting which kilobyte, and drives the data lines on a
read it answers (NESdev wiki, "Cartridge connector", Signal descriptions).

```cpp
struct CpuPins {
  std::uint16_t address = 0;  // A0-A15 as the CPU drives them; bit 15 is what /ROMSEL decodes
  std::uint8_t data = 0;      // D0-D7
  bool write = false;         // R/W low
  // /ROMSEL low: the console NANDs M2 with A15, so the line is low for $8000-$FFFF while
  // the address is stable (NESdev wiki, "Cartridge connector", Signal descriptions).
  [[nodiscard]] constexpr bool romSelect() const noexcept { return (address & 0x8000u) != 0; }
};

struct PpuPins {
  std::uint16_t address = 0;  // PA0-PA13; /A13 is bit 13 inverted
  std::uint8_t data = 0;      // PD0-PD7
  bool read = false;          // /RD low
  bool write = false;         // /WR low
  bool ciramA10 = false;      // which kilobyte of the console's 2 KB a nametable access reaches
  bool ciramEnable = false;   // /CIRAM CE low: the console's 2 KB answers this access
};
```

An NROM board answering a fetch, an address it leaves alone, a store to its PRG RAM, a pattern
table read and a nametable access:

```cpp
std::vector<std::uint8_t> image =
    imageWithHeader({0x4E, 0x45, 0x53, 0x1A, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
image[16] = 0xA1;
const neasant::Cartridge cartridge = neasant::makeCartridge(image);
neasant::Board board = neasant::boardFor(cartridge);
std::vector<std::uint8_t> prgRam(cartridge.prgRamBytes);
std::vector<std::uint8_t> chrRam(cartridge.chrRamBytes);  // empty: the image carries CHR ROM

neasant::CpuPins fetch{.address = 0x8000, .data = 0x5A};  // a read: the lines float at $5A
const bool decoded = neasant::boardCpuCycle(board, cartridge, fetch, prgRam);
// decoded == true and fetch.data == 0xA1: the PRG ROM's first byte
neasant::CpuPins below{.address = 0x5000, .data = 0x5A};
const bool nobody = neasant::boardCpuCycle(board, cartridge, below, prgRam);
// nobody == false and below.data == 0x5A: nothing on this board decodes $5000
neasant::CpuPins store{.address = 0x6000, .data = 0x33, .write = true};
neasant::boardCpuCycle(board, cartridge, store, prgRam);
// prgRam[0] == 0x33
neasant::PpuPins tile{.address = 0x0010, .data = 0x5A, .read = true};
const bool answered = neasant::boardPpuAccess(board, cartridge, tile, chrRam);
// answered == true and tile.data == 0x00: CHR ROM's byte $10; tile.ciramEnable == false
neasant::PpuPins name{.address = 0x2400, .data = 0x5A, .read = true};
const bool console = neasant::boardPpuAccess(board, cartridge, name, chrRam);
// console == false: the board leaves a nametable access to the console's RAM
// name.ciramEnable == true and name.ciramA10 == false: $2400 reaches the first kilobyte
// under horizontal mirroring
```

## The board

Every board is a struct holding its own state, and `Board` is the variant over them. `boardFor`
builds the board a cartridge's mapper number names, and refuses a mapper that is not built, or a
cartridge whose sizes or layout the named board does not have. The three dispatch functions
present one CPU cycle or one PPU access to the board held, and answer the mirroring the board
drives. A board is added by declaring its struct and its three functions in its own header and
naming it in the variant.

```cpp
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
```

The console reaches a board only through the pins.

## NROM

NROM is mapper 0, the board with no mapping logic: 16 KB of PRG ROM repeated across
`$8000-$FFFF`, or 32 KB, or 8 KB four times; 8 KB of CHR, ROM or RAM, over the pattern tables;
the mirroring fixed by a solder pad; and a Family BASIC board's PRG RAM at `$6000-$7FFF`, repeated
to fill the window. A write to the ROM changes nothing (NESdev wiki, "NROM", Banks, Solder pad
config, Registers and Variants; "Mirroring", Memory Mirroring). `Nrom` holds no state.

```cpp
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
```

### Where an address lands

Each address the board is presented with lands here:

| Address | On NROM |
|---|---|
| CPU `$4020-$5FFF` | nothing: the data lines keep their byte |
| CPU `$6000-$7FFF` | PRG RAM, repeated to fill the window, when the cartridge has any; nothing otherwise |
| CPU `$8000-$BFFF` | the first 16 KB of PRG ROM |
| CPU `$C000-$FFFF` | the last 16 KB, or the first again on a 16 KB cartridge; an 8 KB image repeats four times |
| PPU `$0000-$1FFF` | CHR ROM, or CHR RAM when the header declares no CHR ROM |
| PPU `$2000-$3FFF` | the console's nametable RAM: `CIRAM /CE` enabled, `CIRAM A10` from PPU A11 under horizontal mirroring and PPU A10 under vertical |

(NESdev wiki, "NROM", Banks; "2A03", 2A03 register map; "Cartridge connector", Signal
descriptions.) A 16 KB cartridge read
at both halves of the ROM range:

```cpp
std::vector<std::uint8_t> image =
    imageWithHeader({0x4E, 0x45, 0x53, 0x1A, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
image[16] = 0x11;
const neasant::Cartridge cartridge = neasant::makeCartridge(image);
neasant::Board board = neasant::boardFor(cartridge);
std::vector<std::uint8_t> prgRam(cartridge.prgRamBytes);
neasant::CpuPins low{.address = 0x8000};
neasant::CpuPins high{.address = 0xC000};
neasant::boardCpuCycle(board, cartridge, low, prgRam);
neasant::boardCpuCycle(board, cartridge, high, prgRam);
// low.data == 0x11 and high.data == 0x11: 16 KB of PRG ROM repeats across $8000-$FFFF
// neasant::boardMirroring(board, cartridge) == neasant::Mirroring::Horizontal
```

### Mirroring

The board wires `CIRAM A10` to PPU A11 for horizontal mirroring, so `$2000` and `$2400` reach the
first kilobyte and `$2800` and `$2C00` the second, the two nametables one above the other; or to
PPU A10 for vertical mirroring, so `$2000` and `$2800` reach the first and `$2400` and `$2C00` the
second, side by side. `$3000-$3EFF` repeats `$2000-$2EFF` through the same lines (NESdev wiki,
"Mirroring", Nametable Mirroring; "Cartridge connector", CIRAM A10).

| PPU address | Horizontal mirroring | Vertical mirroring |
|---|---|---|
| `$2000-$23FF` | the first kilobyte | the first kilobyte |
| `$2400-$27FF` | the first | the second |
| `$2800-$2BFF` | the second | the first |
| `$2C00-$2FFF` | the second | the second |

(NESdev wiki, "Mirroring", Nametable Mirroring.) A cartridge with vertical mirroring:

```cpp
// flags 6 bit 0 set: vertical mirroring
std::vector<std::uint8_t> image =
    imageWithHeader({0x4E, 0x45, 0x53, 0x1A, 1, 1, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0});
const neasant::Cartridge cartridge = neasant::makeCartridge(image);
neasant::Board board = neasant::boardFor(cartridge);
std::vector<std::uint8_t> chrRam;
neasant::PpuPins left{.address = 0x2000, .read = true};
neasant::PpuPins right{.address = 0x2400, .read = true};
neasant::boardPpuAccess(board, cartridge, left, chrRam);
neasant::boardPpuAccess(board, cartridge, right, chrRam);
// left.ciramA10 == false and right.ciramA10 == true: under vertical mirroring CIRAM A10 is
// PPU A10, so $2000 and $2400 reach the two kilobytes
```

## Gotchas

- **Horizontal mirroring is the vertical arrangement.** The "Mirroring" page's two vocabularies
  invert each other; `Mirroring` names the mirroring, and horizontal takes PPU A11.
- **Sixteen bytes alone never read as NES 2.0.** The detection procedure's size clause reads the
  whole span; hand the parser the image.
- **No CHR ROM means CHR RAM under iNES and nothing under NES 2.0.** Byte 5 at 0 is 8 KB of CHR
  RAM in an iNES header; a NES 2.0 header carries what byte 11 says.
- **Byte 8 at 0 is 8 KB of PRG RAM, battery or not.** A cartridge without the battery flag still
  has the window; `prgRamBytes = 0` in the overrides removes it.
- **A signed tail makes a header archaic.** Only byte 6's nibble names the mapper; the override
  reaches the rest.
- **The overrides change the cartridge, not the header.** `cartridge.header` is the header as
  read; `cartridge.mapper` and the rest are what the board is built from.
- **A mirroring override drops the alternative-nametable bit.** The bit's meaning is the
  mapper's, and NROM refuses it unless a mirroring is named.
- **Bytes after the CHR area are dropped.** A NES 2.0 miscellaneous ROM area is counted and not
  kept.
- **The cartridge's RAM sizes sum both halves.** `prgRamBytes` is the volatile and the
  battery-backed PRG RAM together; `battery` says whether the whole window is the save.
- **The data lines are the bus's on a read the board leaves alone.** `CpuPins::data` goes in
  holding the open-bus byte and comes back untouched when `boardCpuCycle` answers false.
- **A PPU access drives the CIRAM lines whether or not the board answers it.** A board answering
  false has still set `ciramEnable` and `ciramA10`.

## What remains open

What the code does at each point below is fixed and tested; what the hardware does is not settled
by the documents the cartridge is built from.

1. **Flags 6 bit 3 on mapper 0.** The "iNES" and "NES 2.0" pages say the bit's meaning varies by
   mapper and name none for mapper 0; `boardFor` refuses a cartridge that sets it unless the
   config names the mirroring.
2. **NROM-368.** The "NROM" page names it and gives no layout; `boardFor` accepts 8, 16 and 32 KB
   of PRG ROM.
3. **Family BASIC's RAM.** The "NROM" page gives 2 KB, 4 KB on one revision, of battery-backed PRG
   RAM, and 8 KB under the iNES battery flag as the usual mapping. The iNES byte-8 rule gives 8 KB
   and a NES 2.0 shift count the exact size, and the window repeats a smaller RAM to fill it.
4. **The trainer.** Both header pages place 512 bytes at `$7000`; `makeCartridge` refuses an
   image that carries one.
5. **Bus conflicts.** The "NROM" page lists them as present and irrelevant to software; a write to
   the ROM changes nothing and drives nothing back.
6. **Single-screen and four-screen.** The "Mirroring" page has single-screen as a board's latch or
   a fixed level and four screens as a board's own RAM; no header names the first, and NROM
   refuses the second.
7. **The other consoles.** A Vs. System, PlayChoice-10 or extended-console image is refused; the
   NES and Famicom are built.

## See also

- [The NES machine](nes-machine.md) — the bus that reaches the board at `$4020-$FFFF` and the
  save it keeps.
- [The PPU](ppu.md) — the accesses the board sees on its PPU side.

## Where to look

- `include/neasant/nes/cartridge.h` — the header, the cartridge, the overrides, the pins.
- `include/neasant/nes/board.h` — the board variant, `boardFor` and the dispatch.
- `include/neasant/mappers/nrom.h` — NROM.
- `src/nes/cartridge.cpp` — the header reader and `makeCartridge`'s refusals.
- `src/nes/board.cpp` — `boardFor`'s refusals and the dispatch.
- `src/mappers/nrom.cpp` — NROM on both pin sets.
- `tests/nes/cartridge_test.cpp` — the suite.

## Sources

- **NESdev wiki** — the pages "iNES", "NES 2.0", "NROM", "Mirroring", "Cartridge connector" and
  "2A03", at `https://www.nesdev.org/wiki/`.
