#pragma once

// The 2A03's APU over a state value: the frame counter with its two sequences, its
// quarter- and half-frame clocks and its interrupt flag; the four length counters the
// half-frame clock counts down, with their table, their halt bits and the enable bits
// $4015 holds; the two pulse channels with their envelopes, sweeps, timers and duty
// sequencers, the triangle with its linear counter and its 32-step sequencer, and the
// noise with its envelope and its shift register, each answering the level its DAC
// takes; the status register's read and write; the IRQ line the frame interrupt drives;
// power-on and reset; and the seam the DMC's sample fetch takes through the machine's
// DMA unit. The DMC's reader and output unit, the mixer and the decimator are absent: a
// write to $4010-$4013 reaches the bus and is dropped, and the status register's DMC
// bits read 0.

#include <array>
#include <cstdint>

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

// The chip's whole mutable state as a value: snapshot by copy, restore by assignment.
// The defaults are the power-on state (NESdev wiki, "CPU power up state", APU): every
// register 0 and every channel disabled with its counter at 0, the triangle at its first
// step, the noise's shift register 1, mode 0 with the interrupt enabled, the flag clear,
// and the sequence restarting at the first machine cycle's tick, which puts its origin
// nine cycles before the first instruction, the figure blargg's apu_reset programs
// measure as usual.
struct ApuState {
  // Pulse 1, pulse 2, the triangle and the noise, in $4015's bit order.
  std::array<ApuLengthCounter, 4> length{};
  // Pulse 1 and pulse 2.
  std::array<ApuPulse, 2> pulse{};
  ApuTriangle triangle{};
  ApuNoise noise{};
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
  // The IRQ line as the CPU samples it: the flag with the inhibit clear, as they stood
  // when the current tick began. A set reaches the line one cycle after a read of $4015
  // sees it (NESdev wiki, "CPU interrupts", Detailed interrupt behavior: the level
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
  [[nodiscard]] bool operator==(const ApuState&) const noexcept = default;
};

// What the DMC's memory reader is waiting to fetch: nothing, the first byte of a sample
// after a $4015 write started it, or the next byte of one in progress (NESdev wiki,
// "DMA", DMC DMA).
enum class DmcFetch : std::uint8_t { None, Load, Reload };

// The chip after the reset line: every channel disabled with its counter at 0, the frame
// interrupt flag clear, the sequence restarting at the next tick in the mode last written
// to $4017, the triangle's sequencer at its first step, and the pulses and the noise
// silent. The inhibit bit, every halt bit, every channel register, every timer, envelope
// and sweep, and the noise's shift register are kept (NESdev wiki, "CPU power up state",
// APU, the After Reset column; "APU", Status; "APU Noise", Operation).
void apuReset(ApuState& state) noexcept;

// A CPU write to `address`, one of $4000-$4013, $4015 or $4017, in a cycle whose APU
// half is `apuPhase`, 0 a get and 1 a put. $4000, $4004 and $400C take their channel's
// duty, halt bit, constant-volume bit and volume; $4001 and $4005 the sweep's fields,
// setting its reload flag; $4002, $4006 and $400A the low eight bits of a timer's
// period; $4003, $4007 and $400B the high three, and $400E the noise's mode and period
// index. $4008 takes the triangle's halt bit and the linear counter's reload value. A
// write to a fourth register, $4003, $4007, $400B or $400F, also loads the channel's
// length counter from the table at the cycle's tick while the channel is enabled, and
// whether or not it is enabled restarts a pulse's sequencer and its envelope, sets the
// triangle's linear reload flag, or restarts the noise's envelope. $4015 sets the four
// enables, clearing the counter of each channel it disables; $4017 sets the inhibit bit,
// clearing the flag when it is set, and schedules the sequence's restart two cycles on
// from a put cycle and three from a get. Every other address is dropped.
void apuWrite(ApuState& state, std::uint16_t address, std::uint8_t value,
              std::uint8_t apuPhase) noexcept;

// A CPU read of $4015: bit 6 the frame interrupt flag, bits 3-0 whether each length
// counter is above 0, bit 5 the open bus's, bits 7 and 4 0, and the flag is then
// cleared (NESdev wiki, "APU", Status). A set the same cycle's tick makes is not cleared
// by it, since the tick runs after the access.
[[nodiscard]] std::uint8_t apuReadStatus(ApuState& state, std::uint8_t openBus) noexcept;

// What a read of $4015 would answer, with nothing cleared.
[[nodiscard]] std::uint8_t apuPeekStatus(const ApuState& state, std::uint8_t openBus) noexcept;

// One CPU cycle, in the half of the APU cycle `apuPhase` names, 0 a get and 1 a put:
// samples the IRQ line from the flag, counts the restart delay down and restarts the
// sequence when it reaches 0, otherwise steps the counter and runs the step that falls on
// it, the quarter-frame clock on the envelopes and the linear counter and the half-frame
// clock on the sweeps and the length counters; applies each channel's pending load and
// latches its halt bit; counts the triangle's timer, and on a get cycle the pulses' and
// the noise's, so that their sequencer and shift register change as a put cycle begins
// (Visual 2A03); and leaves each channel's output as the tick's end finds it.
void apuTickCycle(ApuState& state, std::uint8_t apuPhase) noexcept;

// The chip's IRQ output, true while it is pulled low: the line as the last tick sampled
// it.
[[nodiscard]] constexpr bool apuIrqAsserted(const ApuState& state) noexcept {
  return state.irqLine;
}

// The fetch the DMC's reader is waiting to make, which the machine's DMA unit carries
// out on the CPU's RDY line; DmcFetch::None while the DMC holds no reader.
[[nodiscard]] DmcFetch apuDmcWantsFetch(const ApuState& state) noexcept;

// Puts a byte the DMA unit fetched into the DMC's sample buffer; with no buffer built the
// byte is dropped.
void apuDmcDeliver(ApuState& state, std::uint8_t byte) noexcept;

}  // namespace neasant
