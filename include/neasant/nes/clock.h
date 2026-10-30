#pragma once

// The console's clock as values: the region, the master clock as the exact ratio it is
// with the master cycles one frame takes, and when each frame of a run is owed.

#include <cstdint>

namespace neasant {

// The console's video standard, which sets the master clock and the dividers the CPU
// and the PPU run at. A machine is built for one region and keeps it for its life. The
// machine builds NTSC; Pal and Dendy are named so a host can state them, and are
// refused at construction.
enum class Region : std::uint8_t { Ntsc, Pal, Dendy };

// What a console of a region runs at: its master clock as an exact ratio, and the
// master cycles one frame takes. NTSC's clock is 236 250 000 / 11 Hz, six times the
// colour subcarrier's 315 / 88 MHz (NESdev wiki, "CPU", Frequencies; "2A03 technical
// reference", CLK), and its frame is 341 dots by 262 scanlines at four master cycles
// a dot, 357 368, the length of a frame with rendering disabled (NESdev wiki, "PPU
// frame timing", Even/Odd Frames and CPU-PPU Clock Alignment). PAL and Dendy share a
// clock of 53 203 425 / 2 Hz, six times 4 433 618.75 Hz (NESdev wiki, "CPU",
// Frequencies); their frames are not stated and read as 0.
struct ConsoleClock {
  std::uint64_t masterCyclesPerFrame = 0;
  std::uint64_t hertzNumerator = 0;
  std::uint64_t hertzDenominator = 1;
};

[[nodiscard]] constexpr ConsoleClock consoleClock(Region region) noexcept {
  switch (region) {
    case Region::Pal:
    case Region::Dendy:
      return ConsoleClock{
          .masterCyclesPerFrame = 0u, .hertzNumerator = 53203425u, .hertzDenominator = 2u};
    case Region::Ntsc:
      break;
  }
  return ConsoleClock{
      .masterCyclesPerFrame = 357368u, .hertzNumerator = 236250000u, .hertzDenominator = 11u};
}

// When each frame of a run is owed, in nanoseconds from the run's start, at the
// console's own rate: for a host holding a run to it. The host asks when the next
// frame is due and waits however it waits; this reads no clock and never waits. A host
// with a clock of its own spends cycle budgets instead.
//
// The frame count is multiplied by the whole nanoseconds of one frame and by the
// fraction left over separately: the single product of the count and the interval's
// numerator passes 64 bits inside a couple of minutes, and a deadline that has wrapped
// sits behind the clock. Split, every deadline is exact past a hundred billion frames,
// sixty years at the console's rate.
class FrameDeadline {
 public:
  explicit constexpr FrameDeadline(Region region) noexcept
      : divisor_(consoleClock(region).hertzNumerator),
        whole_(nanosNumerator(region) / consoleClock(region).hertzNumerator),
        remainder_(nanosNumerator(region) % consoleClock(region).hertzNumerator) {}

  // The nanosecond the Nth frame of the run is owed at.
  [[nodiscard]] constexpr std::uint64_t owedAt(std::uint64_t frames) const noexcept {
    return frames * whole_ + frames * remainder_ / divisor_;
  }

  // One frame's interval in whole nanoseconds, the fraction left over dropped.
  [[nodiscard]] constexpr std::uint64_t wholeNanos() const noexcept { return whole_; }

 private:
  static constexpr std::uint64_t nanosNumerator(Region region) noexcept {
    const ConsoleClock clock = consoleClock(region);
    return 1000000000ull * clock.masterCyclesPerFrame * clock.hertzDenominator;
  }

  std::uint64_t divisor_;
  std::uint64_t whole_;
  std::uint64_t remainder_;
};

// NTSC's first frame and the frame one past the point a single product passes 64
// bits, worked out by hand, so a rearrangement that loses the fraction or wraps does
// not compile; PAL and Dendy have no frame length stated, so their deadlines are 0.
static_assert(FrameDeadline(Region::Ntsc).owedAt(1u) == 16639356ull);
static_assert(FrameDeadline(Region::Ntsc).owedAt(4693u) == 78088500588ull);
static_assert(FrameDeadline(Region::Pal).owedAt(1u) == 0ull);
static_assert(FrameDeadline(Region::Dendy).owedAt(1u) == 0ull);

}  // namespace neasant
