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

// The level each duty reads at each step of the sequencer (NESdev wiki, "APU Pulse", Duty
// Cycle Sequences, the output waveform).
constexpr std::array<std::array<std::uint8_t, 8>, 4> kDutyWaveform{{
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1},
}};

// The triangle's 32 levels in the order the sequencer reads them (NESdev wiki, "APU
// Triangle", Sequencer operation).
constexpr std::array<std::uint8_t, 32> kTriangleSequence{
    15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1,  0,
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15};

// The CPU cycles between two clocks of the noise's shift register, by bits 3-0 of $400E
// (NESdev wiki, "APU Noise", the NTSC column).
constexpr std::array<std::uint16_t, 16> kNoisePeriod{
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};

// The CPU cycles between two clocks of the DMC's output unit, by bits 3-0 of $4010
// (NESdev wiki, "APU DMC", Pitch table, the NTSC column).
constexpr std::array<std::uint16_t, 16> kDmcRate{
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54};

// The DMC's level steps by 2 within 0 to 127: up while it is below 126, down while it is
// above 1 (NESdev wiki, "APU DMC", Output unit).
constexpr std::uint8_t kDmcLevelCeiling = 126;
constexpr std::uint8_t kDmcLevelFloor = 1;

// A pulse is silent while its period is below 8 or its sweep's target is above $7FF
// (NESdev wiki, "APU Pulse", Sequencer operation and Sweep unit operation).
constexpr std::uint16_t kPeriodFloor = 8;
constexpr int kPeriodCeiling = 0x7FF;

// The mixer's two curves in whole numbers (NESdev wiki, "APU Mixer"): the pulses' 95.88 /
// (8128 / n + 100) is 9588 n / (100 (8128 + 100 n)), and the other group's 159.79 / (1 / s
// + 100), with s the sum of triangle / 8227, noise / 12241 and dmc / 22638 over their
// common denominator, is 15979 N / (100 (D + 100 N)), N the three levels weighted by the
// other two divisors' product and D the product of all three. Scaled by 32 767, the
// largest numerator, every channel at its top, is below 2 to the 64th.
constexpr std::uint64_t kPulseNumerator = 9588;
constexpr std::uint64_t kPulseDivisor = 8128;
constexpr std::uint64_t kTndNumerator = 15979;
constexpr std::uint64_t kTriangleWeight = 12241ull * 22638ull;
constexpr std::uint64_t kNoiseWeight = 8227ull * 22638ull;
constexpr std::uint64_t kDmcWeight = 8227ull * 12241ull;
constexpr std::uint64_t kTndDivisor = 8227ull * 12241ull * 22638ull;

// The level the curves' 1.0 is scaled to, which is also the most a sample holds.
constexpr std::uint64_t kFullScale = 32767;

// The output stage's two filters (NESdev wiki, "APU Mixer": a first-order high-pass at
// 90 Hz and a first-order low-pass at 14 kHz). A first-order filter of corner f, given a
// level held for one CPU cycle, moves 1 - e^(-2 pi f / F) of the way to it, F the CPU
// clock's 236 250 000 / 132 Hz. Each step is that part of 16 777 216, rounded to the
// nearest: 5300 stands for a corner of 90.00002 Hz and 804 639 for one of 13 999.997 Hz.
constexpr int kFilterShift = 24;
constexpr std::int64_t kFilterOne = std::int64_t{1} << kFilterShift;
constexpr std::int64_t kHighPassStep = 5300;
constexpr std::int64_t kLowPassStep = 804639;

// The decimator's units (NESdev wiki, "CPU", Frequencies): the master clock is
// 236 250 000 / 11 Hz and a CPU cycle twelve of its cycles, 132 / 236 250 000 of a
// second, so in units of 1 / (236 250 000 times the rate) of a second a cycle is 132 times
// the rate and a sample 236 250 000.
constexpr std::uint32_t kSamplePeriod = 236250000;
constexpr std::uint32_t kCyclePeriodPerHertz = 132;

// The triangle's and the noise's places among the length counters, $4015's bit order.
constexpr std::size_t kTriangleChannel = 2;
constexpr std::size_t kNoiseChannel = 3;

// NESdev wiki, "APU Envelope": the constant volume flag selects the volume's source and
// nothing else.
std::uint8_t envelopeVolume(const ApuEnvelope& envelope) noexcept {
  return envelope.constant ? envelope.volume : envelope.decay;
}

// NESdev wiki, "APU Envelope": the quarter-frame clock restarts an envelope whose start
// flag is set and clocks the divider of any other; a divider clocked at 0 reloads and
// clocks the decay level, which counts down to 0 and returns to 15 only while the loop
// flag is set.
void clockEnvelope(ApuEnvelope& envelope, bool loop) noexcept {
  if (envelope.start) {
    envelope.start = false;
    envelope.decay = 15;
    envelope.divider = envelope.volume;
  } else if (envelope.divider > 0) {
    --envelope.divider;
  } else {
    envelope.divider = envelope.volume;
    if (envelope.decay > 0) {
      --envelope.decay;
    } else if (loop) {
      envelope.decay = 15;
    }
  }
}

// NESdev wiki, "APU Pulse", Sweep unit operation: the change is the period shifted right
// by the shift count; negated, pulse 1 adds its ones' complement and pulse 2 its two's,
// and a sum below 0 is 0.
int sweepTarget(const ApuPulse& pulse, bool onesComplement) noexcept {
  const int period = pulse.period;
  const int change = period >> pulse.sweep.shift;
  if (!pulse.sweep.negate) return period + change;
  const int target = period - change - (onesComplement ? 1 : 0);
  return target < 0 ? 0 : target;
}

// NESdev wiki, "APU Pulse", Mixer output: a period below 8 and a target above $7FF each
// silence the channel, the second whether or not the sweep is enabled.
bool pulseSilenced(const ApuPulse& pulse, bool onesComplement) noexcept {
  return pulse.period < kPeriodFloor || sweepTarget(pulse, onesComplement) > kPeriodCeiling;
}

// NESdev wiki, "APU Pulse", Sweep unit operation: the period takes the target when the
// divider is at 0, the sweep is enabled, the shift is above 0 and the channel is not
// silenced; the divider then reloads when it is at 0 or the reload flag is set, and
// counts down otherwise. 2A03 technical reference, Sweep unit: the period is rewritten
// only while the channel's length counter is above 0, as Visual 2A03 shows, the divider
// counting whatever the counter holds. blargg's NES APU reference, Sweep Unit: a channel
// its period or its target silences keeps its period.
void clockSweep(ApuPulse& pulse, bool onesComplement, bool lengthAboveZero) noexcept {
  ApuSweep& sweep = pulse.sweep;
  if (sweep.divider == 0 && sweep.enabled && sweep.shift > 0 && lengthAboveZero &&
      !pulseSilenced(pulse, onesComplement)) {
    pulse.period = static_cast<std::uint16_t>(sweepTarget(pulse, onesComplement));
  }
  if (sweep.divider == 0 || sweep.reload) {
    sweep.divider = sweep.period;
    sweep.reload = false;
  } else {
    --sweep.divider;
  }
}

// The quarter-frame clock: the three envelopes, each looping on its channel's halt bit,
// and the triangle's linear counter, which reloads while its reload flag is set and
// counts down otherwise; the flag then clears unless the halt bit, the counter's control
// flag, is set (NESdev wiki, "APU Envelope"; "APU Triangle", Linear counter control).
void clockQuarter(ApuState& state) noexcept {
  clockEnvelope(state.pulse[0].envelope, state.length[0].halt);
  clockEnvelope(state.pulse[1].envelope, state.length[1].halt);
  clockEnvelope(state.noise.envelope, state.length[kNoiseChannel].halt);
  ApuTriangle& triangle = state.triangle;
  if (triangle.linearReload) {
    triangle.linear = triangle.linearLoad;
  } else if (triangle.linear > 0) {
    --triangle.linear;
  }
  if (!state.length[kTriangleChannel].halt) triangle.linearReload = false;
}

// Each pulse's sweep is clocked first, reading its length counter as the clock finds it
// (NESdev wiki, "APU Pulse", Sweep unit operation; Visual 2A03). The half-frame clock on
// the four length counters: each above 0 and not halted as the previous tick left its
// halt bit counts down, and a load written in the same cycle is dropped (NESdev wiki,
// "APU Length Counter", Clocking; blargg's 2005 frame counter readme, Length Halt and
// Length Reload).
void clockHalf(ApuState& state) noexcept {
  clockSweep(state.pulse[0], true, state.length[0].counter > 0);
  clockSweep(state.pulse[1], false, state.length[1].counter > 0);
  for (ApuLengthCounter& length : state.length) {
    if (length.counter > 0 && !length.haltLatched) {
      --length.counter;
      length.reloadPending = false;
    }
  }
}

// The level each channel's DAC takes (NESdev wiki, "APU Pulse", Mixer output; "APU
// Triangle", Sequencer operation; "APU Noise", Operation).
void refreshOutputs(ApuState& state) noexcept {
  for (std::size_t i = 0; i < state.pulse.size(); ++i) {
    ApuPulse& pulse = state.pulse[i];
    const bool high = kDutyWaveform[pulse.duty][pulse.step] != 0;
    const bool sounding = high && state.length[i].counter > 0 && !pulseSilenced(pulse, i == 0);
    pulse.output = sounding ? envelopeVolume(pulse.envelope) : std::uint8_t{0};
  }
  state.triangle.output = kTriangleSequence[state.triangle.step];
  ApuNoise& noise = state.noise;
  const bool sounding = (noise.shift & 1u) == 0 && state.length[kNoiseChannel].counter > 0;
  noise.output = sounding ? envelopeVolume(noise.envelope) : std::uint8_t{0};
}

// The sequence's restart: the count to 0 and the mode last written to $4017 (NESdev wiki,
// "APU Frame Counter", Control register).
void restart(ApuState& state) noexcept {
  state.frameCycle = 0;
  state.fiveStep = state.pendingFiveStep;
}

// The reader's address and length from $4012 and $4013 (NESdev wiki, "APU DMC", Sample
// address and Sample length).
void restartSample(ApuDmc& dmc) noexcept {
  dmc.address = static_cast<std::uint16_t>(0xC000u + 64u * dmc.sampleAddress);
  dmc.bytesRemaining = static_cast<std::uint16_t>(16u * dmc.sampleLength + 1u);
}

// One clock of the DMC's output unit (NESdev wiki, "APU DMC", Output unit; blargg's NES
// APU reference, Output Unit): the level steps by the bit shifted out unless the silence
// flag is set; after the eighth bit a new cycle takes the buffered byte, emptying the
// buffer and asking the reader for the next, or sets the silence flag when there is
// none. A reader whose sample stopped one or two ticks ago still asks, and the DMA unit
// abandons the fetch after its halt cycle (NESdev wiki, "DMA", Bugs).
void clockDmcOutput(ApuDmc& dmc) noexcept {
  if (!dmc.silence) {
    if ((dmc.shift & 1u) != 0) {
      if (dmc.output < kDmcLevelCeiling) dmc.output = static_cast<std::uint8_t>(dmc.output + 2);
    } else if (dmc.output > kDmcLevelFloor) {
      dmc.output = static_cast<std::uint8_t>(dmc.output - 2);
    }
  }
  dmc.shift = static_cast<std::uint8_t>(dmc.shift >> 1);
  if (dmc.bitsRemaining > 1) {
    --dmc.bitsRemaining;
    return;
  }
  dmc.bitsRemaining = 8;
  if (!dmc.bufferFull) {
    dmc.silence = true;
    return;
  }
  dmc.shift = dmc.buffer;
  dmc.bufferFull = false;
  dmc.silence = false;
  if (dmc.bytesRemaining > 0 || dmc.stopAge == 1 || dmc.stopAge == 2) {
    dmc.fetch = DmcFetch::Reload;
  }
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
  // "CPU power up state", APU, the After Reset column: the triangle's sequencer is at its
  // first step after reset; "APU Noise", Operation: reset does not affect the shift
  // register.
  state.triangle.step = 0;
  // NESdev wiki, "APU", Status: the write of $00 stops the DMC's sample and clears its
  // interrupt flag; "CPU power up state", APU, the After Reset column: $4011 keeps its low
  // bit.
  state.dmc.bytesRemaining = 0;
  state.dmc.fetch = DmcFetch::None;
  state.dmc.interrupt = false;
  state.dmc.output = static_cast<std::uint8_t>(state.dmc.output & 1u);
  refreshOutputs(state);
}

void apuWrite(ApuState& state, std::uint16_t address, std::uint8_t value,
              std::uint8_t apuPhase) noexcept {
  // One channel every four addresses: the register's place in its channel decides what it
  // holds (NESdev wiki, "APU", Registers).
  const auto channel = static_cast<std::size_t>((address - 0x4000u) >> 2);
  switch (address) {
    case 0x4000:
    case 0x4004: {
      ApuPulse& pulse = state.pulse[channel];
      pulse.duty = static_cast<std::uint8_t>(value >> 6);
      pulse.envelope.constant = (value & 0x10u) != 0;
      pulse.envelope.volume = static_cast<std::uint8_t>(value & 0x0Fu);
      state.length[channel].halt = (value & 0x20u) != 0;
      break;
    }
    case 0x400C:
      state.noise.envelope.constant = (value & 0x10u) != 0;
      state.noise.envelope.volume = static_cast<std::uint8_t>(value & 0x0Fu);
      state.length[channel].halt = (value & 0x20u) != 0;
      break;
    case 0x4001:
    case 0x4005: {
      // NESdev wiki, "APU Pulse", Sweep control: the write sets the reload flag and nothing
      // takes effect until a half-frame clock.
      ApuSweep& sweep = state.pulse[channel].sweep;
      sweep.enabled = (value & 0x80u) != 0;
      sweep.period = static_cast<std::uint8_t>((value >> 4) & 0x07u);
      sweep.negate = (value & 0x08u) != 0;
      sweep.shift = static_cast<std::uint8_t>(value & 0x07u);
      sweep.reload = true;
      break;
    }
    case 0x4002:
    case 0x4006: {
      ApuPulse& pulse = state.pulse[channel];
      pulse.period = static_cast<std::uint16_t>((pulse.period & 0x700u) | value);
      break;
    }
    case 0x4008:
      state.triangle.linearLoad = static_cast<std::uint8_t>(value & 0x7Fu);
      state.length[channel].halt = (value & 0x80u) != 0;
      break;
    case 0x400A:
      state.triangle.period = static_cast<std::uint16_t>((state.triangle.period & 0x700u) | value);
      break;
    case 0x400E:
      state.noise.mode = (value & 0x80u) != 0;
      state.noise.periodIndex = static_cast<std::uint8_t>(value & 0x0Fu);
      break;
    case 0x4003:
    case 0x4007:
    case 0x400B:
    case 0x400F: {
      // NESdev wiki, "APU Length Counter", the load registers' side effects: the envelope is
      // restarted, a pulse's sequencer is reset, the triangle's linear counter reload flag
      // is set; none waits on the channel's enable bit.
      const auto high = static_cast<std::uint16_t>((value & 0x07u) << 8);
      if (address == 0x4003 || address == 0x4007) {
        ApuPulse& pulse = state.pulse[channel];
        pulse.period = static_cast<std::uint16_t>(high | (pulse.period & 0xFFu));
        pulse.step = 0;
        pulse.envelope.start = true;
      } else if (address == 0x400B) {
        state.triangle.period = static_cast<std::uint16_t>(high | (state.triangle.period & 0xFFu));
        state.triangle.linearReload = true;
      } else {
        state.noise.envelope.start = true;
      }
      // NESdev wiki, "APU Length Counter", Clocking: a load while the channel is disabled
      // is ignored.
      ApuLengthCounter& length = state.length[channel];
      if (length.enabled) {
        length.reloadPending = true;
        length.reloadValue = kLengthTable[value >> 3];
      }
      break;
    }
    case 0x4010: {
      // NESdev wiki, "APU DMC", Flags and sample playback rate; blargg's NES APU reference,
      // DMC: a write with the interrupt bit clear clears the flag.
      ApuDmc& dmc = state.dmc;
      dmc.irqEnable = (value & 0x80u) != 0;
      dmc.loop = (value & 0x40u) != 0;
      dmc.rateIndex = static_cast<std::uint8_t>(value & 0x0Fu);
      if (!dmc.irqEnable) dmc.interrupt = false;
      break;
    }
    case 0x4011:
      // NESdev wiki, "APU DMC", Direct counter load.
      state.dmc.output = static_cast<std::uint8_t>(value & 0x7Fu);
      break;
    case 0x4012:
      state.dmc.sampleAddress = value;
      break;
    case 0x4013:
      state.dmc.sampleLength = value;
      break;
    case 0x4015: {
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
      // NESdev wiki, "APU DMC", Sample playback control: bit 4 clear sets the bytes
      // remaining to 0; bit 4 set restarts the sample only when they are 0, and the first
      // byte is fetched at once only when the buffer is empty ("DMA", DMC DMA); the write
      // clears the interrupt flag.
      ApuDmc& dmc = state.dmc;
      if ((value & 0x10u) == 0) {
        if (dmc.bytesRemaining > 0) {
          dmc.bytesRemaining = 0;
          dmc.stopAge = 0;
        }
      } else if (dmc.bytesRemaining == 0) {
        restartSample(dmc);
        if (!dmc.bufferFull) dmc.fetch = DmcFetch::Load;
      }
      dmc.interrupt = false;
      break;
    }
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
  if (state.dmc.interrupt) value = static_cast<std::uint8_t>(value | 0x80u);
  if (state.frameInterrupt) value = static_cast<std::uint8_t>(value | 0x40u);
  if (state.dmc.bytesRemaining > 0) value = static_cast<std::uint8_t>(value | 0x10u);
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

void apuTickCycle(ApuState& state, std::uint8_t apuPhase) noexcept {
  state.irqLine = (state.frameInterrupt && !state.inhibit) || state.dmc.interrupt;
  // A fetch the reader asked for lapses once its sample has stopped: the DMA unit drops
  // one it has not yet halted for and abandons one it has (NESdev wiki, "DMA", Bugs).
  if (state.dmc.fetch != DmcFetch::None && state.dmc.bytesRemaining == 0) {
    state.dmc.fetch = DmcFetch::None;
  }
  bool quarter = false;
  bool half = false;
  bool restarted = false;
  if (state.restartDelay > 0) {
    --state.restartDelay;
    // NESdev wiki, "APU", Frame Counter: a write with bit 7 set clocks the quarter- and
    // half-frame units 2 or 3 cycles after the write, the tick before the restart (Visual
    // 2A03).
    if (state.restartDelay == 1 && state.pendingFiveStep) {
      quarter = true;
      half = true;
    }
    if (state.restartDelay == 0) {
      restart(state);
      restarted = true;
    }
  }
  if (!restarted) ++state.frameCycle;
  const std::uint16_t n = state.frameCycle;
  if (!state.fiveStep) {
    if (n == kQuarterOne || n == kQuarterAndHalfTwo || n == kQuarterThree ||
        n == kFourStepLast) {
      quarter = true;
    }
    if (n == kQuarterAndHalfTwo || n == kFourStepLast) half = true;
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
    if (n == kQuarterAndHalfTwo || n == kFiveStepLast) half = true;
    if (n == kFiveStepEnd) state.frameCycle = 0;
  }
  if (quarter) clockQuarter(state);
  if (half) clockHalf(state);
  for (ApuLengthCounter& length : state.length) {
    if (length.reloadPending) {
      length.counter = length.reloadValue;
      length.reloadPending = false;
    }
    length.haltLatched = length.halt;
  }

  // NESdev wiki, "APU", Glossary: the triangle's timer is clocked on every CPU cycle and
  // the pulses' and the noise's on every second one. Visual 2A03 has the pulses'
  // sequencers and the noise's shift register change as a put cycle begins, which is the
  // tick of the get cycle before it. A timer at 0 reloads with its period and clocks what
  // it drives ("APU Pulse" and "APU Triangle", Sequencer operation); the triangle's
  // sequencer takes the clock only while its linear counter and its length counter are
  // both above 0.
  ApuTriangle& triangle = state.triangle;
  if (triangle.timer > 0) {
    --triangle.timer;
  } else {
    triangle.timer = triangle.period;
    if (triangle.linear > 0 && state.length[kTriangleChannel].counter > 0) {
      triangle.step = static_cast<std::uint8_t>((triangle.step + 1) % kTriangleSequence.size());
    }
  }
  if (apuPhase != kPutHalf) {
    for (ApuPulse& pulse : state.pulse) {
      if (pulse.timer > 0) {
        --pulse.timer;
      } else {
        pulse.timer = pulse.period;
        pulse.step = static_cast<std::uint8_t>((pulse.step + 1) % 8);
      }
    }
    ApuNoise& noise = state.noise;
    if (noise.timer > 0) {
      --noise.timer;
    } else {
      // The table counts CPU cycles and the timer APU cycles: half the entry, less one for
      // the count through 0.
      noise.timer = static_cast<std::uint16_t>(kNoisePeriod[noise.periodIndex] / 2 - 1);
      // NESdev wiki, "APU Noise", Operation: the feedback is bit 0 exclusive-ORed with bit 6
      // in the periodic mode and with bit 1 otherwise; the register shifts right and takes
      // the feedback in bit 14.
      const unsigned tap = noise.mode ? 6u : 1u;
      const unsigned feedback = (noise.shift ^ (noise.shift >> tap)) & 1u;
      noise.shift = static_cast<std::uint16_t>((noise.shift >> 1) | (feedback << 14));
    }
    // NESdev wiki, "APU", Glossary: the DMC's timer is clocked every second CPU cycle;
    // Visual 2A03: its output unit changes as a put cycle begins. The table counts CPU
    // cycles and the timer APU cycles: half the entry, less one for the count through 0.
    ApuDmc& dmc = state.dmc;
    if (dmc.timer > 0) {
      --dmc.timer;
    } else {
      dmc.timer = static_cast<std::uint16_t>(kDmcRate[dmc.rateIndex] / 2 - 1);
      clockDmcOutput(dmc);
    }
  }
  if (state.dmc.stopAge < 3) ++state.dmc.stopAge;
  refreshOutputs(state);
}

void apuDmcDeliver(ApuState& state, std::uint8_t byte) noexcept {
  // NESdev wiki, "APU DMC", Memory reader: the address wraps from $FFFF to $8000; at the
  // last byte the sample restarts with the loop flag set and otherwise ends, setting the
  // interrupt flag with the interrupt enabled.
  ApuDmc& dmc = state.dmc;
  dmc.buffer = byte;
  dmc.bufferFull = true;
  dmc.fetch = DmcFetch::None;
  dmc.address = dmc.address == 0xFFFFu ? std::uint16_t{0x8000}
                                       : static_cast<std::uint16_t>(dmc.address + 1u);
  if (dmc.bytesRemaining == 0) return;
  --dmc.bytesRemaining;
  if (dmc.bytesRemaining > 0) return;
  if (dmc.loop) {
    restartSample(dmc);
  } else {
    dmc.stopAge = 0;
    if (dmc.irqEnable) dmc.interrupt = true;
  }
}

std::int16_t apuMix(const ApuState& state, std::int16_t expansion) noexcept {
  // NESdev wiki, "APU Mixer": output = pulse_out + tnd_out, each group 0 when its channels
  // are all 0, which the whole-number form gives without a division by 0. Half the divisor
  // is added before the division, so each value rounds to the nearest.
  const std::uint64_t pulses =
      std::uint64_t{state.pulse[0].output} + std::uint64_t{state.pulse[1].output};
  const std::uint64_t pulseDivisor = 100 * (kPulseDivisor + 100 * pulses);
  const std::uint64_t pulseLevel =
      (kFullScale * kPulseNumerator * pulses + pulseDivisor / 2) / pulseDivisor;
  const std::uint64_t weighted = std::uint64_t{state.triangle.output} * kTriangleWeight +
                                 std::uint64_t{state.noise.output} * kNoiseWeight +
                                 std::uint64_t{state.dmc.output} * kDmcWeight;
  const std::uint64_t tndDivisor = 100 * (kTndDivisor + 100 * weighted);
  const std::uint64_t tndLevel =
      (kFullScale * kTndNumerator * weighted + tndDivisor / 2) / tndDivisor;
  // The chip's own level is 0 or above, so the sum with the cartridge's stays within a
  // sample's range below and is held at its top.
  const int sum = static_cast<int>(pulseLevel + tndLevel) + expansion;
  return static_cast<std::int16_t>(sum > static_cast<int>(kFullScale)
                                       ? static_cast<int>(kFullScale)
                                       : sum);
}

std::int16_t apuFilter(ApuState& state, std::int16_t level) noexcept {
  ApuFilter& filter = state.filter;
  // Each filter moves its step's part of the way to the level it is given. The shift
  // rounds down, which leaves a filter at rest within a 5000th of a level of the one it
  // is given, so the rounded answer to a standing level is exactly 0.
  filter.highPass +=
      ((std::int64_t{level} * kFilterOne - filter.highPass) * kHighPassStep) >> kFilterShift;
  const std::int64_t passed =
      std::int64_t{level} - ((filter.highPass + kFilterOne / 2) >> kFilterShift);
  filter.lowPass += ((passed * kFilterOne - filter.lowPass) * kLowPassStep) >> kFilterShift;
  const std::int64_t out = (filter.lowPass + kFilterOne / 2) >> kFilterShift;
  // A cartridge's level can swing the mixer's by more than a sample holds; the answer is
  // held within one.
  if (out > 32767) return 32767;
  if (out < -32768) return -32768;
  return static_cast<std::int16_t>(out);
}

std::optional<std::int16_t> apuDecimate(ApuState& state, const ApuInputs& inputs) noexcept {
  if (inputs.audioRate == 0) return std::nullopt;
  ApuDecimator& decimator = state.decimator;
  decimator.sum += apuFilter(state, apuMix(state, inputs.expansion));
  ++decimator.count;
  decimator.elapsed += kCyclePeriodPerHertz * inputs.audioRate;
  if (decimator.elapsed < kSamplePeriod) return std::nullopt;
  decimator.elapsed -= kSamplePeriod;
  const auto sample = static_cast<std::int16_t>(decimator.sum / decimator.count);
  decimator.sum = 0;
  decimator.count = 0;
  return sample;
}

}  // namespace neasant
