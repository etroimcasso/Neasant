#pragma once

// The 2A03's APU over a state value: the frame counter with its two sequences, its
// quarter- and half-frame clocks and its interrupt flag; the four length counters the
// half-frame clock counts down, with their table, their halt bits and the enable bits
// $4015 holds; the two pulse channels with their envelopes, sweeps, timers and duty
// sequencers, the triangle with its linear counter and its 32-step sequencer, the noise
// with its envelope and its shift register, and the DMC with its memory reader, its
// sample buffer, its output unit and its interrupt flag, each channel answering the
// level its DAC takes; the status register's read and write; the IRQ line the two
// interrupt flags drive; power-on and reset; the seam the DMC's sample fetch takes
// through the machine's DMA unit; the mixer, which makes one output level of the five;
// and the decimator, which averages that level down to the samples a host drains.

#include <array>
#include <cstdint>
#include <optional>

namespace neasant {

// One channel's length counter (NESdev wiki, "APU Length Counter"). The counter is
// loaded from the table by a write to the channel's fourth register while the channel is
// enabled, counts down on each half-frame clock while it is above 0 and not halted, and
// silences the channel at 0.
struct ApuLengthCounter {
  // The channel's enable bit as last written to $4015: cleared, the counter is held at 0
  // and a load is ignored (NESdev wiki, "APU Length Counter", Clocking).
  bool enabled = false;
  // The halt bit as last written: bit 5 of $4000, $4004 or $400C, bit 7 of $4008. The
  // same bit is the envelope's loop flag on a pulse and the noise, and the linear
  // counter's control flag on the triangle.
  bool halt = false;
  // The halt bit the half-frame clock consults: `halt` as it stood at the end of the
  // previous tick, so a halt written in the clock's own cycle does not hold it and one
  // written the cycle before does (blargg's 2005 frame counter readme, Length Halt).
  bool haltLatched = false;
  std::uint8_t counter = 0;
  // A load written this cycle, applied by the cycle's tick after its clocks: dropped if
  // that tick's half-frame clock counted the counter down, loaded otherwise (blargg's
  // 2005 frame counter readme, Length Reload).
  bool reloadPending = false;
  std::uint8_t reloadValue = 0;
  [[nodiscard]] bool operator==(const ApuLengthCounter&) const noexcept = default;
};

// A volume envelope, one each for pulse 1, pulse 2 and the noise (NESdev wiki, "APU
// Envelope"). The quarter-frame clock runs it: with the start flag set it loads the
// decay level with 15 and the divider with the volume parameter; otherwise it counts the
// divider down, and a divider clocked at 0 reloads and counts the decay level down, or
// returns it from 0 to 15 while the channel's halt bit, the envelope's loop flag, is set.
struct ApuEnvelope {
  // Bit 4 of $4000, $4004 or $400C: set, the channel's volume is `volume`; clear, it is
  // the decay level.
  bool constant = false;
  // Bits 3-0 of the same register: the constant volume, and the divider's reload value,
  // so the decay level steps every `volume` + 1 quarter frames.
  std::uint8_t volume = 0;
  // Set by a write to the channel's fourth register; the next quarter-frame clock
  // restarts the envelope and clears it.
  bool start = false;
  std::uint8_t divider = 0;
  // The decay level, 15 down to 0. It runs whichever volume is selected.
  std::uint8_t decay = 0;
  [[nodiscard]] bool operator==(const ApuEnvelope&) const noexcept = default;
};

// A pulse channel's sweep unit (NESdev wiki, "APU Pulse", Sweep unit operation). Its
// target is the channel's period plus the period shifted right by `shift`, or minus it
// when `negate` is set; pulse 1 then subtracts one more, adding the ones' complement
// where pulse 2 adds the two's. A target above $7FF silences the channel whether or not
// the sweep is enabled. The half-frame clock sets the period to the target when the
// divider is at 0, the sweep is enabled, the shift is above 0, the channel's length
// counter is above 0 and neither the period nor the target silences the channel; it then
// reloads the divider when it is at 0 or `reload` is set, and counts it down otherwise,
// whatever the length counter holds (2A03 technical reference, Sweep unit; Visual 2A03).
struct ApuSweep {
  // Bit 7 of $4001 or $4005.
  bool enabled = false;
  // Bits 6-4: the divider's reload value, so the period is rewritten every `period` + 1
  // half frames.
  std::uint8_t period = 0;
  // Bit 3: set, the target is below the period and the pitch rises.
  bool negate = false;
  // Bits 2-0.
  std::uint8_t shift = 0;
  // Set by a write to $4001 or $4005; the next half-frame clock reloads the divider and
  // clears it.
  bool reload = false;
  std::uint8_t divider = 0;
  [[nodiscard]] bool operator==(const ApuSweep&) const noexcept = default;
};

// A pulse channel (NESdev wiki, "APU Pulse"): a timer that counts once an APU cycle and
// steps an 8-step duty sequencer every `period` + 1 counts, an envelope, a sweep, and
// the length counter ApuState holds for it.
struct ApuPulse {
  // Bits 7-6 of $4000 or $4004: the waveform the sequencer reads, high for one step of
  // eight, two, four, or all but two.
  std::uint8_t duty = 0;
  ApuEnvelope envelope{};
  ApuSweep sweep{};
  // The timer's 11-bit period: $4002 or $4006 its low eight bits, bits 2-0 of $4003 or
  // $4007 its high three. The sweep rewrites it. A period below 8 silences the channel.
  std::uint16_t period = 0;
  // The timer's count, down to 0 and back to `period`.
  std::uint16_t timer = 0;
  // The sequencer's position in the waveform, 0 to 7. A write to $4003 or $4007 returns
  // it to 0; the timer's count is left as it is.
  std::uint8_t step = 0;
  // The level the channel's DAC takes, 0 to 15: the envelope's volume while the
  // waveform is high, the length counter is above 0 and neither the period nor the
  // sweep's target silences the channel, and 0 otherwise.
  std::uint8_t output = 0;
  [[nodiscard]] bool operator==(const ApuPulse&) const noexcept = default;
};

// The triangle channel (NESdev wiki, "APU Triangle"): a timer that counts every CPU
// cycle and steps a 32-step sequencer every `period` + 1 counts while the linear counter
// and the length counter are both above 0. A counter at 0 stops the sequencer where it
// stands, and the channel keeps answering that step's level.
struct ApuTriangle {
  // Bits 6-0 of $4008: the value the linear counter reloads with.
  std::uint8_t linearLoad = 0;
  // Set by a write to $400B. While it is set the quarter-frame clock reloads the linear
  // counter; the clock then clears it unless the channel's halt bit, the linear
  // counter's control flag, is set.
  bool linearReload = false;
  // The linear counter: the quarter-frame clock counts it down while it is above 0 and
  // `linearReload` is clear.
  std::uint8_t linear = 0;
  // The timer's 11-bit period: $400A its low eight bits, bits 2-0 of $400B its high
  // three.
  std::uint16_t period = 0;
  std::uint16_t timer = 0;
  // The sequencer's position, 0 to 31: the levels 15 down to 0, then 0 up to 15. Reset
  // returns it to 0.
  std::uint8_t step = 0;
  // The level the channel's DAC takes, 0 to 15: the sequence's value at `step`, 15 at
  // power-on.
  std::uint8_t output = 15;
  [[nodiscard]] bool operator==(const ApuTriangle&) const noexcept = default;
};

// The noise channel (NESdev wiki, "APU Noise"): a timer that clocks a 15-bit shift
// register every so many CPU cycles as the period table gives, and an envelope. Each
// clock shifts the register right and puts the exclusive OR of bit 0 and bit 1, or of
// bit 0 and bit 6 in the periodic mode, in bit 14.
struct ApuNoise {
  ApuEnvelope envelope{};
  // Bit 7 of $400E: set, the feedback takes bit 6 and the sequence is 93 or 31 steps
  // long; clear, it takes bit 1 and the sequence is 32 767 steps long.
  bool mode = false;
  // Bits 3-0 of $400E: the entry of the period table the timer reloads with.
  std::uint8_t periodIndex = 0;
  // The timer's count, in APU cycles.
  std::uint16_t timer = 0;
  // The shift register, 1 at power-on. Reset leaves it as it is.
  std::uint16_t shift = 1;
  // The level the channel's DAC takes, 0 to 15: the envelope's volume while bit 0 of the
  // shift register is clear and the length counter is above 0, and 0 otherwise.
  std::uint8_t output = 0;
  [[nodiscard]] bool operator==(const ApuNoise&) const noexcept = default;
};

// What the DMC's memory reader is waiting to fetch: nothing, the first byte of a sample
// after a $4015 write started it with the buffer empty, or the next byte after the output
// unit emptied the buffer (NESdev wiki, "DMA", DMC DMA).
enum class DmcFetch : std::uint8_t { None, Load, Reload };

// The delta modulation channel (NESdev wiki, "APU DMC"): a memory reader that fetches a
// sample's bytes one at a time through the machine's DMA unit, a one-byte sample buffer,
// an output unit that shifts the buffered byte out a bit a clock and steps a 7-bit
// counter by 2 up for a 1 and down for a 0, a timer clocking it at one of sixteen rates,
// and the interrupt flag the fetch that ends a sample sets.
struct ApuDmc {
  // Bit 7 of $4010: set, the fetch that ends a sample sets the interrupt flag; a write
  // clearing it clears the flag.
  bool irqEnable = false;
  // Bit 6 of $4010: set, a sample that ends restarts from $4012 and $4013 and sets no
  // flag.
  bool loop = false;
  // Bits 3-0 of $4010: the entry of the rate table, the CPU cycles between two clocks of
  // the output unit.
  std::uint8_t rateIndex = 0;
  // $4012 as written: a sample starts at $C000 plus 64 times it.
  std::uint8_t sampleAddress = 0;
  // $4013 as written: a sample is 16 times it plus 1 bytes long.
  std::uint8_t sampleLength = 0;
  // The reader's address, the next byte to fetch; it steps from $FFFF to $8000.
  std::uint16_t address = 0;
  // The reader's bytes remaining, bit 4 of a $4015 read while above 0. A $4015 write
  // with bit 4 set restarts the sample when it is 0 and a write with bit 4 clear sets it
  // to 0, the buffered byte still playing out.
  std::uint16_t bytesRemaining = 0;
  // The sample buffer: the byte the DMA unit delivered last, and whether the output unit
  // has yet to take it.
  std::uint8_t buffer = 0;
  bool bufferFull = false;
  // The fetch the reader is waiting for, which the machine's DMA unit carries out.
  DmcFetch fetch = DmcFetch::None;
  // Ticks since the sample stopped, by a $4015 write or by its last fetch, held at 3. A
  // stop one or two ticks before the output unit empties the buffer still asks for a
  // fetch, which the DMA unit abandons after its halt cycle (NESdev wiki, "DMA", Bugs).
  std::uint8_t stopAge = 3;
  // The output unit's shift register, the bits left in it, 8 down to 1, and the silence
  // flag, set when a byte's cycle began with the buffer empty.
  std::uint8_t shift = 0;
  std::uint8_t bitsRemaining = 8;
  bool silence = true;
  // The timer's count, in APU cycles.
  std::uint16_t timer = 0;
  // The 7-bit counter, the level the channel's DAC takes: bits 6-0 of $4011 load it, and
  // each clock with the silence flag clear steps it by 2, up for a 1 bit while it is
  // below 126 and down for a 0 while it is above 1.
  std::uint8_t output = 0;
  // The interrupt flag, bit 7 of $4015: set by the fetch that ends a sample while bit 7
  // of $4010 is set, cleared by any write to $4015 and by a $4010 write with bit 7
  // clear.
  bool interrupt = false;
  [[nodiscard]] bool operator==(const ApuDmc&) const noexcept = default;
};

// Where the chip's output stands in the sample under way (apuDecimate). Time is counted
// in units of 1 / (236 250 000 times the output rate) of a second: a CPU cycle is 132
// times the rate of them and a sample is 236 250 000 (NESdev wiki, "CPU", Frequencies:
// the CPU clock is 236 250 000 / 132 Hz).
struct ApuDecimator {
  // The time run since the last sample was made, in those units; below 236 250 000
  // between calls.
  std::uint32_t elapsed = 0;
  // The output level summed over the CPU cycles since the last sample, and the count of
  // those cycles.
  std::int64_t sum = 0;
  std::uint32_t count = 0;
  [[nodiscard]] bool operator==(const ApuDecimator&) const noexcept = default;
};

// What the chip's output reaches and does not hold: the rate its level is decimated to,
// in samples a second, from the machine's config, 0 making no samples; and the level the
// cartridge's sound hardware adds to the chip's, in a sample's units, 0 for a cartridge
// with none.
struct ApuInputs {
  std::uint32_t audioRate = 0;
  std::int16_t expansion = 0;
};

// The chip's whole mutable state as a value: snapshot by copy, restore by assignment.
// The defaults are the power-on state (NESdev wiki, "CPU power up state", APU): every
// register 0 and every channel disabled with its counter at 0, the triangle at its first
// step, the noise's shift register 1, the DMC silent with its level 0, mode 0 with the
// interrupt enabled, the flag clear, and the sequence restarting at the first machine
// cycle's tick, which puts its origin nine cycles before the first instruction, the
// figure blargg's apu_reset programs measure as usual; the output's first sample not yet
// begun.
struct ApuState {
  // Pulse 1, pulse 2, the triangle and the noise, in $4015's bit order.
  std::array<ApuLengthCounter, 4> length{};
  // Pulse 1 and pulse 2.
  std::array<ApuPulse, 2> pulse{};
  ApuTriangle triangle{};
  ApuNoise noise{};
  ApuDmc dmc{};
  // The mode the sequencer runs in: false the 4-step sequence, true the 5-step (NESdev
  // wiki, "APU Frame Counter").
  bool fiveStep = false;
  // $4017 bit 7 as last written, which the next restart makes the mode.
  bool pendingFiveStep = false;
  // $4017 bit 6 as last written: set, the frame interrupt flag is cleared and never set.
  bool inhibit = false;
  // The frame interrupt flag, bit 6 of $4015: set by the three consecutive ticks that end
  // the 4-step sequence, cleared by a read of $4015 and by a write to $4017 with bit 6 set
  // (NESdev wiki, "APU Frame Counter"; "APU", Status).
  bool frameInterrupt = false;
  // The IRQ line as the CPU samples it: the frame interrupt flag with the inhibit clear,
  // or the DMC's interrupt flag, as they stood when the current tick began. A set
  // reaches the line one cycle after a read of $4015 sees it (NESdev wiki, "CPU interrupts", Detailed interrupt behavior: the level
  // detector samples the line during the second half of a cycle and raises its signal the
  // cycle after).
  bool irqLine = false;
  // CPU cycles since the sequence's origin, as the last tick left it: the steps fall at
  // 7457, 14913, 22371 and 29829 or 37281, and the count returns to 0 at 29830 (4-step)
  // or 37282 (5-step), the APU-cycle counts of the frame counter's table doubled, the
  // quarter- and half-frame steps one cycle on (NESdev wiki, "APU Frame Counter").
  std::uint16_t frameCycle = 0;
  // Ticks until the sequence restarts, 0 when none is due: a write to $4017 sets 3 on a
  // put cycle and 4 on a get, so the origin is two or three cycles after the write, and
  // the restart takes the pending mode. A write with bit 7 set clocks the quarter- and
  // half-frame units on the tick before the restart (NESdev wiki, "APU", Frame Counter;
  // "APU Frame Counter", Control register; Visual 2A03). Until the restart the counter
  // keeps stepping in its old mode.
  std::uint8_t restartDelay = 1;
  // The output's place in its current sample. Reset leaves it as it is.
  ApuDecimator decimator{};
  [[nodiscard]] bool operator==(const ApuState&) const noexcept = default;
};

// The chip after the reset line: every channel disabled with its counter at 0, the DMC's
// sample stopped with its interrupt flag clear and its level keeping its low bit, the
// frame interrupt flag clear, the sequence restarting at the next tick in the mode last
// written to $4017, the triangle's sequencer at its first step, and the pulses and the
// noise silent. The inhibit bit, every halt bit, every channel register, every timer,
// envelope and sweep, the noise's shift register, the DMC's buffer and output unit and
// the decimator are kept (NESdev wiki, "CPU power up state", APU, the After Reset column;
// "APU", Status; "APU Noise", Operation).
void apuReset(ApuState& state) noexcept;

// A CPU write to `address`, one of $4000-$4013, $4015 or $4017, in a cycle whose APU
// half is `apuPhase`, 0 a get and 1 a put. $4000, $4004 and $400C take their channel's
// duty, halt bit, constant-volume bit and volume; $4001 and $4005 the sweep's fields,
// setting its reload flag; $4002, $4006 and $400A the low eight bits of a timer's
// period; $4003, $4007 and $400B the high three, and $400E the noise's mode and period
// index. $4010 takes the DMC's interrupt enable, loop flag and rate, clearing the
// interrupt flag when the enable is written clear; $4011 its level; $4012 and $4013 its
// sample address and length. $4008 takes the triangle's halt bit and the linear
// counter's reload value. A write to a fourth register, $4003, $4007, $400B or $400F,
// also loads the channel's length counter from the table at the cycle's tick while the
// channel is enabled, and whether or not it is enabled restarts a pulse's sequencer and
// its envelope, sets the triangle's linear reload flag, or restarts the noise's
// envelope. $4015 sets the four enables, clearing the counter of each channel it
// disables; its bit 4 clear stops the DMC's sample and set restarts one whose bytes
// remaining are 0, asking for the first byte at once when the buffer is empty, and the
// write clears the DMC's interrupt flag; $4017 sets the inhibit bit, clearing the flag
// when it is set, and schedules the sequence's restart two cycles on from a put cycle
// and three from a get. Every other address is dropped.
void apuWrite(ApuState& state, std::uint16_t address, std::uint8_t value,
              std::uint8_t apuPhase) noexcept;

// A CPU read of $4015: bit 7 the DMC's interrupt flag, bit 6 the frame interrupt flag,
// bit 4 whether the DMC's bytes remaining are above 0, bits 3-0 whether each length
// counter is above 0, bit 5 the open bus's, and the frame interrupt flag is then cleared
// (NESdev wiki, "APU", Status). A set the same cycle's tick makes is not cleared by it,
// since the tick runs after the access; the DMC's flag is not cleared by a read.
[[nodiscard]] std::uint8_t apuReadStatus(ApuState& state, std::uint8_t openBus) noexcept;

// What a read of $4015 would answer, with nothing cleared.
[[nodiscard]] std::uint8_t apuPeekStatus(const ApuState& state, std::uint8_t openBus) noexcept;

// One CPU cycle, in the half of the APU cycle `apuPhase` names, 0 a get and 1 a put:
// samples the IRQ line from the two interrupt flags, lets a DMC fetch request lapse once
// its bytes remaining are 0, counts the restart delay down and restarts the sequence
// when it reaches 0, otherwise steps the counter and runs the step that falls on it, the
// quarter-frame clock on the envelopes and the linear counter and the half-frame clock
// on the sweeps and the length counters; applies each channel's pending load and latches
// its halt bit; counts the triangle's timer, and on a get cycle the pulses', the noise's
// and the DMC's, so that their sequencer and shift register change as a put cycle
// begins (Visual 2A03); and leaves each channel's output as the tick's end finds it.
void apuTickCycle(ApuState& state, std::uint8_t apuPhase) noexcept;

// The chip's IRQ output, true while it is pulled low: the line as the last tick sampled
// it.
[[nodiscard]] constexpr bool apuIrqAsserted(const ApuState& state) noexcept {
  return state.irqLine;
}

// The fetch the DMC's reader is waiting to make, which the machine's DMA unit carries
// out on the CPU's RDY line from the reader's address: a load from the cycle after the
// $4015 write that started the sample, a reload from the cycle after the tick in which
// the output unit emptied the buffer. The request stands until the byte is delivered,
// and lapses at the next tick once the bytes remaining are 0 (NESdev wiki, "DMA", DMC
// DMA and Bugs).
[[nodiscard]] constexpr DmcFetch apuDmcWantsFetch(const ApuState& state) noexcept {
  return state.dmc.fetch;
}

// Puts the byte the DMA unit fetched into the sample buffer, steps the reader's address,
// wrapping from $FFFF to $8000, and counts its bytes remaining down; at 0 the sample
// restarts from $4012 and $4013 while $4010 bit 6 is set, and otherwise ends, setting the
// interrupt flag while $4010 bit 7 is set (NESdev wiki, "APU DMC", Memory reader).
void apuDmcDeliver(ApuState& state, std::uint8_t byte) noexcept;

// The level the chip's output takes, 0 to 32 767, from the level each channel's DAC
// takes. The two pulses go through one curve, 95.88 / (8128 / (pulse 1 + pulse 2) + 100),
// and the triangle, the noise and the DMC through another, 159.79 / (1 / (triangle /
// 8227 + noise / 12241 + dmc / 22638) + 100), a group whose channels all stand at 0
// adding 0. The two curves together run from 0.0 to 1.0; each one's value is scaled by
// 32 767 and rounded to the nearest whole number, a half upward, and the two are added
// (NESdev wiki, "APU Mixer"). Each curve flattens as its group's levels rise, so the
// DMC's level sets how far a step of the triangle or the noise moves the output.
// `expansion` is added to the sum, which is held at 32 767, the most a sample holds. The
// level is the DACs' own: 0 with every channel at 0, and the console's output filters
// are left to the host.
[[nodiscard]] std::int16_t apuMix(const ApuState& state, std::int16_t expansion) noexcept;

// One CPU cycle of the chip's output, called after the cycle's tick: adds the level
// apuMix answers to the sample under way and moves the decimator on by the cycle, 132
// times the rate; when that completes a sample's 236 250 000 it answers the sample, the
// sum divided by the count of cycles it spans with the fraction dropped, and begins the
// next with the time left over. A sample is thus the mean of the level over the cycles
// it spans, 37 or 38 of them at 48 000 Hz, and the first N cycles from power-on make the
// whole part of N times the rate times 132 / 236 250 000 samples. A rate of 0 answers
// nothing and leaves the state as it is; the rate is at most the CPU clock's 1 789 772,
// a sample a cycle.
[[nodiscard]] std::optional<std::int16_t> apuDecimate(ApuState& state,
                                                      const ApuInputs& inputs) noexcept;

}  // namespace neasant
