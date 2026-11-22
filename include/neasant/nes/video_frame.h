#pragma once

// The picture the machine hands to a host that watches it: one value a dot, 256 across
// and 240 down, as the PPU resolves it, and the observer the machine hands each picture
// to. A dot's value is the chip's own output for that position, a palette entry and the
// three emphasis bits, and a host decodes the pair into a colour with a palette table of
// its own; no colour is made here.

#include "neasant/nes/clock.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace neasant {

// The picture region: 256 dots across and 240 lines down. The border around it, which
// the chip drives too, is not part of the picture (NESdev wiki, "PPU rendering", Drawing
// overview).
inline constexpr unsigned kPictureWidth = 256;
inline constexpr unsigned kPictureHeight = 240;
inline constexpr std::size_t kPictureDots = 61440;

// A dot's value, as the chip drives its video output for that position: bits 5-0 are
// the palette entry, with greyscale applied, and bits 8-6 are $2001's three emphasis
// bits, bit 6 red, bit 7 green and bit 8 blue; bits 15-9 are 0 (NESdev wiki, "PPU
// registers", PPUMASK, Color control; "PPU palettes"). dotEntry and dotEmphasis take
// the two apart.
[[nodiscard]] constexpr std::uint8_t dotEntry(std::uint16_t dot) noexcept {
  return static_cast<std::uint8_t>(dot & 0x3Fu);
}
// The emphasis bits of a dot: bit 0 red, bit 1 green, bit 2 blue.
[[nodiscard]] constexpr std::uint8_t dotEmphasis(std::uint16_t dot) noexcept {
  return static_cast<std::uint8_t>((dot >> 6) & 0x07u);
}

// One finished picture. `dots` is row-major from the top line, kPictureDots long,
// `width` across and `height` down: the machine's own storage, valid for the call, so a
// host that keeps a picture copies it. `region` is the machine's, and `odd` is the
// frame's parity: true on an odd frame, whose last dot the chip skips while rendering
// is enabled (NESdev wiki, "PPU frame timing", Even/Odd Frames). A decoder of the
// composite signal needs the parity, since the skipped dot moves the colour carrier's
// phase (NESdev wiki, "NTSC video", Color Artifacts).
struct VideoFrame {
  std::span<const std::uint16_t> dots;
  unsigned width = kPictureWidth;
  unsigned height = kPictureHeight;
  Region region = Region::Ntsc;
  bool odd = false;
};

// What a machine tells about the picture: each one the chip finishes, whole.
class FrameObserver {
 public:
  virtual ~FrameObserver() = default;

  // A picture the chip has finished, from inside step() or run(), at the end of the
  // machine cycle in which the picture's vertical blank begins, after the cycle has
  // closed and before the cycle that can take the frame's NMI: a pad a host sets inside
  // the report is the one the frame's handler reads. It may throw, on
  // SaveObserver::changed's terms: the cycle is accounted for, the machine is coherent
  // at its end, the report is not made again, and a run() it interrupts has accounted
  // for its whole budget.
  virtual void frame(const VideoFrame& frame) = 0;
};

}  // namespace neasant
