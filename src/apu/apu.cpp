#include "neasant/apu/apu.h"

#include <cstddef>

namespace neasant {

namespace {

// The value a write to a channel's fourth register loads, by bits 7-3 (NESdev wiki, "APU
// Length Counter", Length table).
constexpr std::array<std::uint8_t, 32> kLengthTable{
    10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
    12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};

// The steps in CPU cycles from the sequence's origin: the frame counter's APU-cycle counts
// doubled, the quarter- and half-frame steps one CPU cycle on: 3728.5, 7456.5, 11185.5,
// 14914, 14914.5 and 14915 APU cycles in the 4-step mode, 18640.5 and 18641 in the 5-step
// (NESdev wiki, "APU Frame Counter").
constexpr std::uint16_t kQuarterOne = 7457;
constexpr std::uint16_t kQuarterAndHalfTwo = 14913;
constexpr std::uint16_t kQuarterThree = 22371;
constexpr std::uint16_t kFourStepFlagFirst = 29828;
constexpr std::uint16_t kFourStepLast = 29829;
constexpr std::uint16_t kFourStepEnd = 29830;
constexpr std::uint16_t kFiveStepLast = 37281;
constexpr std::uint16_t kFiveStepEnd = 37282;

// The ticks from a write to $4017 to the restart, the write's own tick counted: the effects
// come 3 CPU cycles after a write during an APU cycle and 4 after one between APU cycles
// (NESdev wiki, "APU Frame Counter", Control register).
constexpr std::uint8_t kRestartAfterPut = 3;
constexpr std::uint8_t kRestartAfterGet = 4;

// The half of the APU cycle that is a put (NESdev wiki, "DMA", Cadence).
constexpr std::uint8_t kPutHalf = 1;

// The half-frame clock on the four length counters: each above 0 and not halted as the
// previous tick left its halt bit counts down, and a load written in the same cycle is
// dropped (NESdev wiki, "APU Length Counter", Clocking; blargg's 2005 frame counter readme,
// Length Halt and Length Reload).
void clockHalf(ApuState& state) noexcept {
  for (ApuLengthCounter& length : state.length) {
    if (length.counter > 0 && !length.haltLatched) {
      --length.counter;
      length.reloadPending = false;
    }
  }
}

// The sequence's restart: the count to 0 and the mode last written to $4017; in the 5-step
// mode the quarter- and half-frame units are clocked at once (NESdev wiki, "APU", Frame
// Counter; "APU Frame Counter", Control register). Answers whether the units were clocked.
bool restart(ApuState& state) noexcept {
  state.frameCycle = 0;
  state.fiveStep = state.pendingFiveStep;
  if (!state.fiveStep) return false;
  clockHalf(state);
  return true;
}

}  // namespace

void apuReset(ApuState& state) noexcept {
  // NESdev wiki, "APU", Status: reset has the effect of writing $00 to $4015. "CPU power up
  // state", APU, the After Reset column: $4017 unchanged; blargg's apu_reset readme: the
  // last value written to $4017 is written again.
  for (ApuLengthCounter& length : state.length) {
    length.enabled = false;
    length.counter = 0;
    length.reloadPending = false;
  }
  state.frameInterrupt = false;
  state.frameCycle = 0;
  state.restartDelay = 1;
}

void apuWrite(ApuState& state, std::uint16_t address, std::uint8_t value,
              std::uint8_t apuPhase) noexcept {
  // NESdev wiki, "APU Length Counter": the halt bits and the load registers, one channel
  // every four addresses.
  const auto channel = static_cast<std::size_t>((address - 0x4000u) >> 2);
  switch (address) {
    case 0x4000:
    case 0x4004:
    case 0x400C:
      state.length[channel].halt = (value & 0x20u) != 0;
      break;
    case 0x4008:
      state.length[channel].halt = (value & 0x80u) != 0;
      break;
    case 0x4003:
    case 0x4007:
    case 0x400B:
    case 0x400F: {
      // NESdev wiki, "APU Length Counter", Clocking: a load while the channel is disabled
      // is ignored.
      ApuLengthCounter& length = state.length[channel];
      if (length.enabled) {
        length.reloadPending = true;
        length.reloadValue = kLengthTable[value >> 3];
      }
      break;
    }
    case 0x4015:
      // NESdev wiki, "APU", Status: a cleared enable bit silences its channel and halts
      // its length counter at 0; "APU Length Counter", Clocking: setting it has no
      // immediate effect.
      for (std::size_t i = 0; i < state.length.size(); ++i) {
        ApuLengthCounter& length = state.length[i];
        length.enabled = (value & (1u << i)) != 0;
        if (!length.enabled) {
          length.counter = 0;
          length.reloadPending = false;
        }
      }
      break;
    case 0x4017:
      // NESdev wiki, "APU Frame Counter", Control register: bit 7 the mode, bit 6 the
      // interrupt inhibit, whose setting clears the flag; the counter is reset 3 or 4 CPU
      // cycles after the write by the half of the APU cycle it lands in.
      state.inhibit = (value & 0x40u) != 0;
      if (state.inhibit) state.frameInterrupt = false;
      state.pendingFiveStep = (value & 0x80u) != 0;
      state.restartDelay = apuPhase == kPutHalf ? kRestartAfterPut : kRestartAfterGet;
      break;
    default:
      break;
  }
}

std::uint8_t apuPeekStatus(const ApuState& state, std::uint8_t openBus) noexcept {
  // NESdev wiki, "APU", Status: IF-D NT21, bit 5 open bus.
  auto value = static_cast<std::uint8_t>(openBus & 0x20u);
  if (state.frameInterrupt) value = static_cast<std::uint8_t>(value | 0x40u);
  for (std::size_t i = 0; i < state.length.size(); ++i) {
    if (state.length[i].counter > 0) value = static_cast<std::uint8_t>(value | (1u << i));
  }
  return value;
}

std::uint8_t apuReadStatus(ApuState& state, std::uint8_t openBus) noexcept {
  const std::uint8_t value = apuPeekStatus(state, openBus);
  // NESdev wiki, "APU", Status: reading clears the frame interrupt flag.
  state.frameInterrupt = false;
  return value;
}

void apuTickCycle(ApuState& state) noexcept {
  state.irqLine = state.frameInterrupt && !state.inhibit;
  // The quarter-frame clock, which the envelopes and the linear counter take.
  [[maybe_unused]] bool quarter = false;
  if (state.restartDelay > 0 && --state.restartDelay == 0) {
    quarter = restart(state);
  } else {
    ++state.frameCycle;
  }
  const std::uint16_t n = state.frameCycle;
  if (!state.fiveStep) {
    if (n == kQuarterOne || n == kQuarterAndHalfTwo || n == kQuarterThree ||
        n == kFourStepLast) {
      quarter = true;
    }
    if (n == kQuarterAndHalfTwo || n == kFourStepLast) clockHalf(state);
    // NESdev wiki, "APU Frame Counter", the $4017.7 = 0 table: the flag is set on the
    // sequence's last three cycles while the interrupt inhibit flag is clear.
    if ((n == kFourStepFlagFirst || n == kFourStepLast || n == kFourStepEnd) &&
        !state.inhibit) {
      state.frameInterrupt = true;
    }
    if (n == kFourStepEnd) state.frameCycle = 0;
  } else {
    if (n == kQuarterOne || n == kQuarterAndHalfTwo || n == kQuarterThree ||
        n == kFiveStepLast) {
      quarter = true;
    }
    if (n == kQuarterAndHalfTwo || n == kFiveStepLast) clockHalf(state);
    if (n == kFiveStepEnd) state.frameCycle = 0;
  }
  for (ApuLengthCounter& length : state.length) {
    if (length.reloadPending) {
      length.counter = length.reloadValue;
      length.reloadPending = false;
    }
    length.haltLatched = length.halt;
  }
}

DmcFetch apuDmcWantsFetch(const ApuState&) noexcept {
  // The DMC holds no reader, so no fetch is ever waiting.
  return DmcFetch::None;
}

void apuDmcDeliver(ApuState&, std::uint8_t) noexcept {
  // The DMC holds no sample buffer, so the byte is dropped.
}

}  // namespace neasant
