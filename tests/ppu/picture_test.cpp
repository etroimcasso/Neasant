#include "neasant/nes/board.h"
#include "neasant/nes/cartridge.h"
#include "neasant/nes/nes.h"
#include "neasant/nes/video_frame.h"
#include "neasant/ppu/ppu.h"
#include "authored_image.h"
#include "rom_harness.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <vector>

// The picture and its observer. On a bare chip: the dot each position is resolved on,
// four behind the counter; the picture's 240 lines and nothing past them; the three dots
// the chip marks; the skip's frame end; with rendering disabled the backdrop or the entry
// `v` points at, greyscale and each emphasis bit; with rendering enabled the backdrop
// alone; an entry and the mask changed mid-line; and the chip the same with no picture
// held. On the machine: the observer as the host's object outside the state; the report
// in the cycle the vertical blank begins, before the frame's NMI, with a pad set inside
// it read by the handler; an observer set inside a picture handed the next; a set, a
// restore and a reset dropping the picture in progress; a throw with the budget owed;
// two machines equal watched and unwatched, with the parity; a copy with its own
// picture; and whole pictures as the chip drew them. A case that checks a hardware rule
// names the document and section it is read from; the rest check the machine's own
// surface.

namespace {

using neasant::Board;
using neasant::boardFor;
using neasant::Cartridge;
using neasant::CpuSequence;
using neasant::dotEmphasis;
using neasant::dotEntry;
using neasant::FrameObserver;
using neasant::Joypad;
using neasant::JoypadPort;
using neasant::kPictureDots;
using neasant::kPictureHeight;
using neasant::kPictureWidth;
using neasant::makeCartridge;
using neasant::Mirroring;
using neasant::Nes;
using neasant::NesConfig;
using neasant::NesState;
using neasant::PpuDotEvent;
using neasant::PpuInputs;
using neasant::PpuState;
using neasant::ppuTickDot;
using neasant::ppuWrite;
using neasant::Region;
using neasant::VideoFrame;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::PictureKeeper;
using neasant::nes_tests::placeProgram;
using neasant::nes_tests::prgAt;

using Bytes = std::vector<std::uint8_t>;
using Program = std::initializer_list<std::uint8_t>;

// A value no dot takes, bits 15-9 of a dot being 0: what a picture's entries hold before
// the chip writes them.
constexpr std::uint16_t kBlank = 0xFFFF;

// One frame with rendering off, in master cycles: 341 x 262 dots x 4 (NESdev wiki, "PPU
// frame timing", Even/Odd Frames).
constexpr std::uint64_t kFrame = 357368;
// The master cycle at which the machine cycle holding the first frame's dot 1 of scanline
// 241 closes: the dot is the 82182nd tick after power-on, at master 328 724 under
// alignment 0 and up to three later under the others, inside the cycle that runs
// 328 716-328 728.
constexpr std::uint64_t kFirstVblankCycleEnd = 328728;
// The same for the second frame: its tick at master 686 092 to 686 095, inside the cycle
// that runs 686 088-686 100.
constexpr std::uint64_t kSecondVblankCycleEnd = 686100;

// JMP $8000
constexpr Program kJmpLoop = {0x4C, 0x00, 0x80};

// A bare chip over an NROM board with 16 KB of PRG ROM and 8 KB of CHR ROM, its warm-up
// window closed as it is after the first pre-render line, holding a picture the chip
// writes into, every entry blank until it does.
struct Chip {
  Bytes image;
  Cartridge cartridge;
  Board board;
  std::vector<std::uint8_t> chrRam;
  std::vector<std::uint16_t> picture = std::vector<std::uint16_t>(kPictureDots, kBlank);
  PpuState state;
  PpuInputs inputs;

  Chip()
      : image(authoredImage(inesHeader({}), 16384, 8192)),
        cartridge(makeCartridge(image)),
        board(boardFor(cartridge)),
        chrRam(cartridge.chrRamBytes),
        inputs{board, cartridge, chrRam, 115200u, picture} {
    state.warmingUp = false;
  }

  void write(std::uint8_t port, std::uint8_t value) { ppuWrite(state, port, value, inputs); }
  PpuDotEvent tick() { return ppuTickDot(state, false, inputs); }
  // Ticks until the counter stands at `scanline`, `dot`.
  void tickTo(std::uint16_t scanline, std::uint16_t dot) {
    while (!(state.scanline == scanline && state.dot == dot)) tick();
  }
  // How many entries the chip has written.
  [[nodiscard]] std::size_t written() const {
    return static_cast<std::size_t>(
        std::count_if(picture.begin(), picture.end(), [](std::uint16_t d) { return d != kBlank; }));
  }
  // The value the chip resolves for position 6 of line 0 with `v` as given: the counter
  // put at dot 9 of scanline 0 and ticked once, to dot 10.
  std::uint16_t valueWith(std::uint16_t v) {
    state.scanline = 0;
    state.dot = 9;
    state.v = v;
    tick();
    return picture[6];
  }
};

// A 16 KB NROM image with 8 KB of CHR ROM: `program` at $8000, the reset and IRQ
// vectors pointing there, the NMI vector at $9000 and `handler` there, an RTI by default.
Bytes imageWith(Program program, Program handler = {0x40}) {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  placeProgram(image, 0, std::span<const std::uint8_t>(program.begin(), program.size()));
  placeProgram(image, 0x1000, std::span<const std::uint8_t>(handler.begin(), handler.size()));
  prgAt(image, 0x3FFA) = 0x00;  // the NMI vector: $9000
  prgAt(image, 0x3FFB) = 0x90;
  return image;
}

// Steps `cycles` machine cycles.
void stepCycles(Nes& machine, int cycles) {
  for (int i = 0; i < cycles; ++i) machine.step();
}

// A machine over `image` built with `alignment` and the flag clear at power-on, stepped
// through its reset sequence, its state then changed by `edit` and restored.
template <typename Edit>
Nes prepared(const Bytes& image, std::uint8_t alignment, Edit edit) {
  Nes machine(NesConfig{.rom = image, .alignment = alignment, .vblankAtPowerOn = false});
  stepCycles(machine, 7);
  NesState state = machine.state();
  edit(state);
  machine.restore(state);
  return machine;
}

// Every report, with the machine as it stood inside it.
struct Watcher final : FrameObserver {
  explicit Watcher(Nes& machine) : m(machine) {}
  Nes& m;
  int reports = 0;
  std::uint64_t masterAt = 0;
  std::uint64_t consumedAt = 0;
  bool vblankAt = false;
  bool nmiPendingAt = false;
  CpuSequence servicingAt = CpuSequence::None;
  std::uint16_t pcAt = 0;
  VideoFrame last;
  std::vector<std::uint16_t> dots;
  Joypad padToSet;
  bool setsPad = false;
  void frame(const VideoFrame& frame) override {
    ++reports;
    masterAt = m.state().master;
    consumedAt = m.state().consumed;
    vblankAt = m.state().ppu.vblank;
    nmiPendingAt = m.state().cpu.nmiPending;
    servicingAt = m.state().cpu.servicing;
    pcAt = m.state().cpu.pc;
    last = frame;
    dots.assign(frame.dots.begin(), frame.dots.end());
    if (setsPad) m.setJoypad(JoypadPort::One, padToSet);
  }
};

// A host whose keeping of the picture fails.
struct Thrower final : FrameObserver {
  int reports = 0;
  void frame(const VideoFrame&) override {
    ++reports;
    throw std::runtime_error("the host could not keep the picture");
  }
};

// Whether `dots` is a whole picture, kPictureDots long, every entry of it `value`.
bool allAre(std::span<const std::uint16_t> dots, std::uint16_t value) {
  return dots.size() == kPictureDots &&
         std::all_of(dots.begin(), dots.end(), [value](std::uint16_t d) { return d == value; });
}

// ---- PpuPicture: the bare chip ----

// Position x of line y is resolved on dot x + 4 of scanline y, the chip's output running
// four dots behind its counter (NESdev wiki, "PPU rendering", Cycles 1-256: the first
// pixel is output during cycle 4; its frame timing diagram's notes: actual pixel output
// has a 4-dot delay). Dots 0-3 and 260-340 resolve no position.
TEST(PpuPicture, APositionIsResolvedOnTheFourthDotAfterItsOwn) {
  Chip chip;
  chip.state.palette[0] = 0x21;
  for (int i = 0; i < 3; ++i) EXPECT_EQ(chip.tick(), PpuDotEvent::None) << "tick " << i;
  EXPECT_EQ(chip.state.dot, 3);
  EXPECT_EQ(chip.picture[0], kBlank);
  EXPECT_EQ(chip.tick(), PpuDotEvent::PictureBegun);
  EXPECT_EQ(chip.state.dot, 4);
  EXPECT_EQ(chip.picture[0], 0x21);
  EXPECT_EQ(chip.picture[1], kBlank);
  EXPECT_EQ(chip.tick(), PpuDotEvent::None);
  EXPECT_EQ(chip.picture[1], 0x21);
  chip.tickTo(0, 259);
  EXPECT_EQ(chip.picture[255], 0x21);
  EXPECT_EQ(chip.picture[256], kBlank);
  EXPECT_EQ(chip.written(), 256u);
  chip.tickTo(1, 3);
  EXPECT_EQ(chip.written(), 256u);
  EXPECT_EQ(chip.tick(), PpuDotEvent::None);
  EXPECT_EQ(chip.picture[256], 0x21);
  EXPECT_EQ(chip.written(), 257u);
}

// Lines 0-239 are the picture; the last position is resolved on dot 259 of scanline 239,
// and the post-render line, the vertical blank and the pre-render line resolve none: a
// picture with a line's worth of entries past the region is left blank there (NESdev
// wiki, "PPU rendering", Drawing overview and Line-by-line timing).
TEST(PpuPicture, TheLastLineIsLine239AndNothingIsStoredAfterIt) {
  Chip chip;
  chip.picture.resize(kPictureDots + kPictureWidth, kBlank);
  chip.inputs.picture = chip.picture;
  chip.state.palette[0] = 0x05;
  chip.tickTo(239, 258);
  EXPECT_EQ(chip.picture[kPictureDots - 1], kBlank);
  EXPECT_EQ(chip.written(), kPictureDots - 1);
  EXPECT_EQ(chip.tick(), PpuDotEvent::None);
  EXPECT_EQ(chip.picture[kPictureDots - 1], 0x05);
  EXPECT_EQ(chip.written(), kPictureDots);
  const std::vector<std::uint16_t> complete = chip.picture;
  chip.state.palette[0] = 0x30;
  chip.tickTo(261, 340);
  EXPECT_EQ(chip.tick(), PpuDotEvent::FrameEnded);
  EXPECT_TRUE(chip.picture == complete);
  EXPECT_EQ(chip.picture[kPictureDots], kBlank);
  chip.tickTo(0, 4);
  EXPECT_EQ(chip.picture[0], 0x30);
}

// The three dots the chip marks: dot 4 of scanline 0 begins the picture, dot 1 of scanline
// 241 begins the vertical blank, whether or not a read of $2002 keeps the flag from
// setting, and the dot that wraps the counter ends the frame; every other dot is none
// (NESdev wiki, "PPU rendering", Cycles 1-256 and Vertical blanking lines; "PPU frame
// timing", VBL Flag Timing).
TEST(PpuPicture, TheThreeMarkedDots) {
  Chip chip;
  int begun = 0;
  int vblank = 0;
  int ended = 0;
  int none = 0;
  for (int tick = 1; tick <= 89342; ++tick) {
    switch (chip.tick()) {
      case PpuDotEvent::PictureBegun:
        ++begun;
        EXPECT_EQ(tick, 4);
        break;
      case PpuDotEvent::VerticalBlank:
        ++vblank;
        EXPECT_EQ(tick, 82182);
        EXPECT_EQ(chip.state.scanline, 241);
        EXPECT_EQ(chip.state.dot, 1);
        EXPECT_TRUE(chip.state.vblank);
        break;
      case PpuDotEvent::FrameEnded:
        ++ended;
        EXPECT_EQ(tick, 89342);
        break;
      case PpuDotEvent::None:
        ++none;
        break;
    }
  }
  EXPECT_EQ(begun, 1);
  EXPECT_EQ(vblank, 1);
  EXPECT_EQ(ended, 1);
  EXPECT_EQ(none, 89339);

  Chip read;
  read.tickTo(241, 0);
  EXPECT_EQ(ppuTickDot(read.state, true, read.inputs), PpuDotEvent::VerticalBlank);
  EXPECT_FALSE(read.state.vblank);
}

// An odd frame with rendering enabled ends on the dot that leaves scanline 261 from dot
// 339 (NESdev wiki, "PPU frame timing", Even/Odd Frames).
TEST(PpuPicture, TheSkipEndsTheFrameToo) {
  Chip chip;
  chip.state.mask = 0x08;
  chip.state.odd = true;
  chip.state.scanline = 261;
  chip.state.dot = 338;
  EXPECT_EQ(chip.tick(), PpuDotEvent::None);
  EXPECT_EQ(chip.state.dot, 339);
  EXPECT_EQ(chip.tick(), PpuDotEvent::FrameEnded);
  EXPECT_EQ(chip.state.scanline, 0);
  EXPECT_EQ(chip.state.dot, 0);
  EXPECT_FALSE(chip.state.odd);
}

// With rendering disabled every position is the backdrop, palette entry 0, or, while the
// low 14 bits of `v` are in $3F00-$3FFF, the entry they name through the palette's
// aliases (NESdev wiki, "PPU rendering", Rendering disabled; "PPU palettes", Backdrop
// override and Palette RAM).
TEST(PpuPicture, WithRenderingDisabledTheBackdropOrTheEntryVPointsAt) {
  Chip chip;
  chip.state.palette[0x00] = 0x21;
  chip.state.palette[0x04] = 0x2A;
  chip.state.palette[0x05] = 0x16;
  chip.state.palette[0x1D] = 0x3C;
  EXPECT_EQ(chip.valueWith(0x2000), 0x21);
  EXPECT_EQ(chip.valueWith(0x0000), 0x21);
  EXPECT_EQ(chip.valueWith(0x3EFF), 0x21);
  EXPECT_EQ(chip.valueWith(0x3F05), 0x16);
  EXPECT_EQ(chip.valueWith(0x3F14), 0x2A);  // $3F14 is the storage of $3F04
  EXPECT_EQ(chip.valueWith(0x3F10), 0x21);  // $3F10 is the storage of $3F00
  EXPECT_EQ(chip.valueWith(0x3F25), 0x16);  // the 32 entries repeat
  EXPECT_EQ(chip.valueWith(0x3FFD), 0x3C);
  EXPECT_EQ(chip.valueWith(0x7F05), 0x16);  // the low 14 bits of v
}

// Greyscale keeps the entry's grey column: the entry ANDed with $30 while $2001 bit 0 is
// set, with the backdrop and with the entry `v` points at alike (NESdev wiki, "PPU
// registers", PPUMASK, Color control).
TEST(PpuPicture, GreyscaleKeepsTheGreyColumnOfTheEntry) {
  Chip chip;
  chip.state.palette[0x00] = 0x16;
  chip.state.palette[0x05] = 0x27;
  chip.state.mask = 0x01;
  EXPECT_EQ(chip.valueWith(0x0000), 0x10);
  chip.state.palette[0x00] = 0x3C;
  EXPECT_EQ(chip.valueWith(0x0000), 0x30);
  chip.state.palette[0x00] = 0x0F;
  EXPECT_EQ(chip.valueWith(0x0000), 0x00);
  EXPECT_EQ(chip.valueWith(0x3F05), 0x20);
  chip.state.mask = 0x00;
  EXPECT_EQ(chip.valueWith(0x0000), 0x0F);
  EXPECT_EQ(chip.valueWith(0x3F05), 0x27);
}

// $2001 bits 7-5 ride above the entry as bits 8-6 of the dot, bit 6 red, bit 7 green and
// bit 8 blue, on their own and with greyscale; bits 4-1 of $2001 reach no dot (NESdev
// wiki, "PPU registers", PPUMASK).
TEST(PpuPicture, EachEmphasisBitRidesAboveTheEntry) {
  Chip chip;
  chip.state.palette[0x00] = 0x16;
  chip.state.mask = 0x20;
  EXPECT_EQ(chip.valueWith(0x0000), 0x56);
  EXPECT_EQ(dotEmphasis(0x56), 1);
  chip.state.mask = 0x40;
  EXPECT_EQ(chip.valueWith(0x0000), 0x96);
  EXPECT_EQ(dotEmphasis(0x96), 2);
  chip.state.mask = 0x80;
  EXPECT_EQ(chip.valueWith(0x0000), 0x116);
  EXPECT_EQ(dotEmphasis(0x116), 4);
  chip.state.mask = 0xE0;
  EXPECT_EQ(chip.valueWith(0x0000), 0x1D6);
  EXPECT_EQ(dotEntry(0x1D6), 0x16);
  EXPECT_EQ(dotEmphasis(0x1D6), 7);
  chip.state.mask = 0xE1;
  EXPECT_EQ(chip.valueWith(0x0000), 0x1D0);
  chip.state.mask = 0x1E;
  EXPECT_EQ(chip.valueWith(0x0000), 0x16);
  EXPECT_EQ(dotEmphasis(0x16), 0);
}

// With rendering enabled, either bit, every position is the backdrop and `v` names no
// entry: no tile is fetched, so every pixel is transparent, and the override belongs to
// rendering disabled (NESdev wiki, "PPU rendering", Drawing overview and Rendering
// disabled; "PPU palettes", Backdrop override).
TEST(PpuPicture, WithRenderingEnabledEveryPositionIsTheBackdropAndVDoesNotOverride) {
  Chip chip;
  chip.state.palette[0x00] = 0x21;
  chip.state.palette[0x05] = 0x16;
  chip.state.mask = 0x08;
  EXPECT_EQ(chip.valueWith(0x3F05), 0x21);
  chip.state.mask = 0x10;
  EXPECT_EQ(chip.valueWith(0x3F05), 0x21);
  chip.state.mask = 0x18;
  EXPECT_EQ(chip.valueWith(0x3F05), 0x21);
  chip.state.mask = 0x00;
  EXPECT_EQ(chip.valueWith(0x3F05), 0x16);
}

// An entry or $2001 changed between two dots shows from the next dot's position: the
// chip reads the palette and the mask on the dot it resolves a position (NESdev wiki,
// "PPU rendering", Rendering disabled: the chip continuously draws the colour at the
// address; "PPU registers", PPUMASK, Color control).
TEST(PpuPicture, AnEntryOrTheMaskChangedMidLineShowsFromTheNextDot) {
  Chip chip;
  chip.state.palette[0x00] = 0x21;
  chip.tickTo(0, 100);
  EXPECT_EQ(chip.picture[96], 0x21);
  chip.state.palette[0x00] = 0x0A;
  EXPECT_EQ(chip.tick(), PpuDotEvent::None);
  EXPECT_EQ(chip.picture[96], 0x21);
  EXPECT_EQ(chip.picture[97], 0x0A);
  chip.write(1, 0x01);  // greyscale, read by the next dot
  chip.tick();
  EXPECT_EQ(chip.picture[98], 0x00);
  chip.write(1, 0x40);  // green emphasis
  chip.tick();
  EXPECT_EQ(chip.picture[99], 0x8A);
}

// With no picture held the chip resolves every dot and stores none, and its state and
// its answers are what they are with one; a picture shorter than the region takes the
// entries it holds.
TEST(PpuPicture, NothingIsStoredWithoutThePictureAndTheChipRunsTheSame) {
  Chip held;
  Chip empty;
  Chip part;
  empty.inputs.picture = {};
  part.inputs.picture = std::span<std::uint16_t>(part.picture).first(10);
  for (Chip* chip : {&held, &empty, &part}) {
    chip->state.palette[0x00] = 0x21;
    chip->state.v = 0x3F05;
    chip->state.palette[0x05] = 0x16;
  }
  for (int tick = 1; tick <= 89342; ++tick) {
    const PpuDotEvent a = held.tick();
    const PpuDotEvent b = empty.tick();
    const PpuDotEvent c = part.tick();
    ASSERT_EQ(a, b) << "tick " << tick;
    ASSERT_EQ(a, c) << "tick " << tick;
  }
  EXPECT_TRUE(held.state == empty.state);
  EXPECT_TRUE(held.state == part.state);
  EXPECT_EQ(held.written(), kPictureDots);
  EXPECT_EQ(held.picture[0], 0x16);
  EXPECT_EQ(empty.written(), 0u);
  EXPECT_EQ(part.written(), 10u);
  EXPECT_EQ(part.picture[9], 0x16);
  EXPECT_EQ(part.picture[10], kBlank);
}

// ---- PpuMachinePicture ----

TEST(PpuMachinePicture, NoneIsSetAtPowerOnAndItIsNotState) {
  const Bytes image = imageWith(kJmpLoop);
  Nes m(NesConfig{.rom = image});
  EXPECT_EQ(m.frameObserver(), nullptr);
  PictureKeeper keeper;
  const NesState before = m.state();
  m.setFrameObserver(&keeper);
  EXPECT_EQ(m.frameObserver(), &keeper);
  EXPECT_TRUE(m.state() == before);
  const Nes copy = m;
  EXPECT_EQ(copy.frameObserver(), &keeper);
  m.step();
  EXPECT_TRUE(m.state() != before);
  m.restore(before);
  EXPECT_EQ(m.frameObserver(), &keeper);
  m.setFrameObserver(nullptr);
  EXPECT_EQ(m.frameObserver(), nullptr);
  m.run(2 * kFrame);
  EXPECT_EQ(keeper.pictures, 0u);
}

// The picture is handed over at the end of the machine cycle in which dot 1 of scanline
// 241 fell, under every alignment: the 82182nd dot after power-on, in the cycle that
// closes at master 328 728, and a frame later in the one that closes at 686 100 (NESdev
// wiki, "PPU rendering", Vertical blanking lines; "PPU frame timing", CPU-PPU Clock
// Alignment). Its fields are the picture's size, the machine's region and the frame's
// parity.
TEST(PpuMachinePicture, ThePictureIsHandedOverInTheCycleTheVerticalBlankBegins) {
  EXPECT_EQ(kPictureDots, kPictureWidth * kPictureHeight);
  const Bytes image = imageWith(kJmpLoop);
  for (std::uint8_t a = 0; a < 4; ++a) {
    Nes m(NesConfig{.rom = image, .alignment = a, .vblankAtPowerOn = false});
    Watcher watcher(m);
    m.setFrameObserver(&watcher);
    EXPECT_EQ(m.run(27393u * 12u), 27393u * 12u);
    EXPECT_EQ(watcher.reports, 0) << "alignment " << int{a};
    EXPECT_EQ(m.step(), 12u);
    EXPECT_EQ(watcher.reports, 1) << "alignment " << int{a};
    EXPECT_EQ(watcher.masterAt, kFirstVblankCycleEnd);
    EXPECT_EQ(watcher.consumedAt, kFirstVblankCycleEnd);
    EXPECT_TRUE(watcher.vblankAt);
    EXPECT_EQ(m.state().ppu.scanline, 241);
    EXPECT_EQ(watcher.last.width, kPictureWidth);
    EXPECT_EQ(watcher.last.height, kPictureHeight);
    EXPECT_EQ(watcher.last.region, Region::Ntsc);
    EXPECT_FALSE(watcher.last.odd);
    EXPECT_EQ(watcher.dots.size(), kPictureDots);
    EXPECT_TRUE(allAre(watcher.dots, 0x00));  // the palette holds the seed, $00
    // The budget ends at master 686 096, inside the cycle the second report follows.
    EXPECT_EQ(m.run(kFrame), kFrame + 4);
    EXPECT_EQ(watcher.reports, 2) << "alignment " << int{a};
    EXPECT_EQ(watcher.masterAt, kSecondVblankCycleEnd);
    EXPECT_EQ(m.state().master, kSecondVblankCycleEnd);
    EXPECT_TRUE(watcher.last.odd);
  }
}

// The report comes before the cycle that takes the frame's NMI: inside it the edge is
// latched and no sequence has begun, and a pad set inside it is the pad the handler
// reads (NESdev wiki, "CPU interrupts", Detailed interrupt behavior; "Controller
// reading"). The handler strobes the ports, reads port one once and stores the byte.
TEST(PpuMachinePicture, TheReportComesBeforeTheFrameNmiAndAPadSetInsideItIsRead) {
  // LDA #$01; STA $4016; LDA #$00; STA $4016; LDA $4016; STA $10; RTI
  const Bytes image = imageWith(kJmpLoop, {0xA9, 0x01, 0x8D, 0x16, 0x40, 0xA9, 0x00, 0x8D, 0x16,
                                           0x40, 0xAD, 0x16, 0x40, 0x85, 0x10, 0x40});
  Nes m = prepared(image, 0, [](NesState& s) { s.ppu.ctrl = 0x80; });
  Watcher watcher(m);
  watcher.setsPad = true;
  watcher.padToSet = Joypad{.a = true};
  m.setFrameObserver(&watcher);
  // The restore dropped the first picture; the second is reported.
  m.run(2 * kFrame - 84);
  EXPECT_EQ(watcher.reports, 1);
  EXPECT_EQ(watcher.masterAt, kSecondVblankCycleEnd);
  EXPECT_TRUE(watcher.nmiPendingAt);
  EXPECT_EQ(watcher.servicingAt, CpuSequence::None);
  EXPECT_EQ(m.state().ram[0x10], 0x41);  // A pressed, over the operand's high byte $40
  EXPECT_EQ(m.state().cpu.servicing, CpuSequence::None);
  EXPECT_GE(m.state().cpu.pc, 0x8000);  // back in the loop, the handler done
  EXPECT_LE(m.state().cpu.pc, 0x8002);
}

// An observer set inside a picture is handed the next one, begun after the set.
TEST(PpuMachinePicture, AnObserverSetInsideAPictureIsHandedTheNext) {
  const Bytes image = imageWith(kJmpLoop);
  Nes m(NesConfig{.rom = image});
  stepCycles(m, 1000);  // dot 3000 of the first picture
  Watcher watcher(m);
  m.setFrameObserver(&watcher);
  m.run(kFrame - 12000);
  EXPECT_EQ(watcher.reports, 0);
  m.run(kFrame);
  EXPECT_EQ(watcher.reports, 1);
  EXPECT_EQ(watcher.masterAt, kSecondVblankCycleEnd);
}

// A set of the observer, the same pointer included, a restore and a reset each drop the
// picture in progress, and the first picture begun after each is reported.
TEST(PpuMachinePicture, SettingRestoringAndResettingDropThePictureInProgress) {
  const Bytes image = imageWith(kJmpLoop);

  Nes set(NesConfig{.rom = image});
  Watcher setWatcher(set);
  set.setFrameObserver(&setWatcher);
  stepCycles(set, 1000);
  set.setFrameObserver(&setWatcher);
  set.run(kFrame - 12000);
  EXPECT_EQ(setWatcher.reports, 0);
  set.run(kFrame);
  EXPECT_EQ(setWatcher.reports, 1);
  EXPECT_EQ(setWatcher.masterAt, kSecondVblankCycleEnd);

  Nes restored(NesConfig{.rom = image});
  Watcher restoredWatcher(restored);
  restored.setFrameObserver(&restoredWatcher);
  stepCycles(restored, 1000);
  const NesState inside = restored.state();
  stepCycles(restored, 500);
  restored.restore(inside);
  restored.run(kFrame - 12000);
  EXPECT_EQ(restoredWatcher.reports, 0);
  restored.run(kFrame);
  EXPECT_EQ(restoredWatcher.reports, 1);
  EXPECT_EQ(restoredWatcher.masterAt, kSecondVblankCycleEnd);
  // Restored to before its first dot, the picture is whole: the counter is back at
  // power-on and its dot 1 of scanline 241 falls where the first one did.
  const NesState powerOn = Nes(NesConfig{.rom = image}).state();
  restored.restore(powerOn);
  restored.run(kFrame);
  EXPECT_EQ(restoredWatcher.reports, 2);
  EXPECT_EQ(restoredWatcher.masterAt, kFirstVblankCycleEnd);

  // After a reset at master 84 the counter restarts: its dot 1 of scanline 241 is the
  // 82182nd tick on, at master 84 + 4 x 82181 = 328 808, inside the cycle that closes at
  // 328 812.
  Nes reset(NesConfig{.rom = image});
  Watcher resetWatcher(reset);
  reset.setFrameObserver(&resetWatcher);
  stepCycles(reset, 7);
  reset.reset();
  reset.run(kFrame);
  EXPECT_EQ(resetWatcher.reports, 1);
  EXPECT_EQ(resetWatcher.masterAt, 328812u);
}

// A report may throw: the machine stands at the end of the vertical blank's cycle, the
// budget is accounted for whole, the next run() spends what is owed, and the report is
// not made again; out of step(), the cycle and its twelve master cycles are accounted
// for first.
TEST(PpuMachinePicture, AReportMayThrowAndTheRestOfTheBudgetIsOwed) {
  const Bytes image = imageWith(kJmpLoop);
  Nes a(NesConfig{.rom = image});
  Thrower thrower;
  a.setFrameObserver(&thrower);
  EXPECT_THROW(a.run(kFrame + 1200), std::runtime_error);
  EXPECT_EQ(thrower.reports, 1);
  EXPECT_EQ(a.state().master, kFirstVblankCycleEnd);
  EXPECT_EQ(a.state().consumed, kFrame + 1200);
  // The budget's 358 568 falls inside a cycle, which run(0) finishes: 358 572.
  EXPECT_EQ(a.run(0), 358572u - kFirstVblankCycleEnd);
  EXPECT_EQ(a.state().master, 358572u);
  EXPECT_EQ(thrower.reports, 1);
  EXPECT_THROW(a.run(kFrame), std::runtime_error);
  EXPECT_EQ(thrower.reports, 2);
  EXPECT_EQ(a.state().master, kSecondVblankCycleEnd);

  Nes b(NesConfig{.rom = image});
  Thrower stepped;
  b.setFrameObserver(&stepped);
  stepCycles(b, 27393);
  EXPECT_THROW(b.step(), std::runtime_error);
  EXPECT_EQ(stepped.reports, 1);
  EXPECT_EQ(b.state().master, kFirstVblankCycleEnd);
  EXPECT_EQ(b.state().consumed, kFirstVblankCycleEnd);
  EXPECT_EQ(b.step(), 12u);
  EXPECT_EQ(b.state().master, kFirstVblankCycleEnd + 12);
  EXPECT_EQ(stepped.reports, 1);
}

// A machine nobody watches runs exactly as a watched one: equal states after every
// frame; and each picture's parity is its frame's, which toggles every frame with
// rendering disabled too (NESdev wiki, "PPU frame timing", Even/Odd Frames).
TEST(PpuMachinePicture, UnsetTheMachineRunsTheSameAndTheParityIsTheFrames) {
  const Bytes image = imageWith(kJmpLoop);
  Nes watched(NesConfig{.rom = image});
  Nes alone(NesConfig{.rom = image});
  PictureKeeper keeper;
  watched.setFrameObserver(&keeper);
  for (int frame = 0; frame < 4; ++frame) {
    watched.run(kFrame);
    stepCycles(alone, 10000);
    alone.run(kFrame - 120000);
    ASSERT_TRUE(watched.state() == alone.state()) << "frame " << frame;
    EXPECT_EQ(keeper.pictures, static_cast<std::uint64_t>(frame + 1));
    EXPECT_EQ(keeper.odd, frame % 2 == 1) << "frame " << frame;
  }
}

// A copy of a machine points at the same observer and carries its own picture, whole as
// far as the original's was: both report, and the two pictures are equal.
TEST(PpuMachinePicture, ACopyCarriesTheObserverAndItsOwnPicture) {
  const Bytes image = imageWith(kJmpLoop);
  Nes m = prepared(image, 0, [](NesState& s) {
    s.ppu.warmingUp = false;
    s.ppu.palette[0] = 0x21;
  });
  PictureKeeper keeper;
  m.setFrameObserver(&keeper);
  m.run(kFrame);  // the picture begun after the restore is under way
  Nes copy = m;
  EXPECT_EQ(copy.frameObserver(), &keeper);
  m.run(kFrame);
  EXPECT_EQ(keeper.pictures, 1u);
  const std::vector<std::uint16_t> first = keeper.dots;
  copy.run(kFrame);
  EXPECT_EQ(keeper.pictures, 2u);
  EXPECT_TRUE(keeper.dots == first);
  EXPECT_TRUE(allAre(first, 0x21));
}

// A whole picture as the chip drew it: the backdrop, the entry `v` points at, greyscale
// and emphasis over every position; and a palette written during one vertical blank
// shows whole in the next picture, never part of one (NESdev wiki, "PPU rendering",
// Rendering disabled; "PPU palettes", Backdrop override; "PPU registers", PPUMASK).
TEST(PpuMachinePicture, ThePictureHoldsWhatTheChipDrewForTheFrame) {
  const Bytes image = imageWith(kJmpLoop);
  const auto pictureWith = [&image](std::uint8_t mask, std::uint16_t v) {
    Nes m = prepared(image, 0, [mask, v](NesState& s) {
      s.ppu.warmingUp = false;
      s.ppu.palette[0x00] = 0x21;
      s.ppu.palette[0x05] = 0x16;
      s.ppu.mask = mask;
      s.ppu.v = v;
    });
    PictureKeeper keeper;
    m.setFrameObserver(&keeper);
    m.run(2 * kFrame);
    EXPECT_EQ(keeper.pictures, 1u);
    return keeper.dots;
  };
  EXPECT_TRUE(allAre(pictureWith(0x00, 0x0000), 0x21));
  EXPECT_TRUE(allAre(pictureWith(0x00, 0x3F05), 0x16));
  EXPECT_TRUE(allAre(pictureWith(0x01, 0x3F05), 0x10));
  EXPECT_TRUE(allAre(pictureWith(0xE0, 0x0000), 0x1E1));
  EXPECT_TRUE(allAre(pictureWith(0x08, 0x3F05), 0x21));

  // The program waits for the flag twice, so its palette writes land in a vertical blank
  // after the warm-up: $3F00 = $16, then v back to $0000.
  // BIT $2002; BPL -5; BIT $2002; BPL -5; LDA #$3F; STA $2006; LDA #$00; STA $2006;
  // LDA #$16; STA $2007; LDA #$00; STA $2006; STA $2006; JMP self
  const Bytes writer = imageWith({0x2C, 0x02, 0x20, 0x10, 0xFB, 0x2C, 0x02, 0x20, 0x10, 0xFB,
                                  0xA9, 0x3F, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20,
                                  0xA9, 0x16, 0x8D, 0x07, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20,
                                  0x8D, 0x06, 0x20, 0x4C, 0x21, 0x80});
  struct Every final : FrameObserver {
    std::vector<std::vector<std::uint16_t>> pictures;
    void frame(const VideoFrame& frame) override {
      pictures.emplace_back(frame.dots.begin(), frame.dots.end());
    }
  };
  Nes m(NesConfig{.rom = writer, .vblankAtPowerOn = false});
  Every every;
  m.setFrameObserver(&every);
  m.run(4 * kFrame);
  ASSERT_EQ(every.pictures.size(), 4u);
  EXPECT_TRUE(allAre(every.pictures[0], 0x00));
  EXPECT_TRUE(allAre(every.pictures[3], 0x16));
  for (const std::vector<std::uint16_t>& picture : every.pictures) {
    EXPECT_TRUE(allAre(picture, 0x00) || allAre(picture, 0x16));
  }
  EXPECT_EQ(m.state().ppu.palette[0], 0x16);
  EXPECT_EQ(m.state().ppu.v, 0x0000);
}

}  // namespace
