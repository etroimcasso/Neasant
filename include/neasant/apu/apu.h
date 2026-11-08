#pragma once

// The 2A03's APU over a state value: the frame counter with its two sequences, its
// quarter- and half-frame clocks and its interrupt flag; the four length counters the
// half-frame clock counts down, with their table, their halt bits and the enable bits
// $4015 holds; the status register's read and write; the IRQ line the frame interrupt
// drives; power-on and reset; and the seam the DMC's sample fetch takes through the
// machine's DMA unit. The channels' timers, sequencers, envelopes, sweeps, linear
// counter, shift register and DMC reader, the mixer and the decimator are absent: a
// write to their registers reaches the bus and is dropped, and the status register's
// DMC bits read 0.

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
  // The halt bit as last written: bit 5 of $4000, $4004 or $400C, bit 7 of $4008.
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

// The chip's whole mutable state as a value: snapshot by copy, restore by assignment.
// The defaults are the power-on state (NESdev wiki, "CPU power up state", APU): every
// channel disabled with its counter at 0, mode 0 with the interrupt enabled, the flag
// clear, and the sequence restarting at the first machine cycle's tick, which puts its
// origin nine cycles before the first instruction, the figure blargg's apu_reset
// programs measure as usual.
struct ApuState {
  // Pulse 1, pulse 2, the triangle and the noise, in $4015's bit order.
  std::array<ApuLengthCounter, 4> length{};
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
  // put cycle and 4 on a get, so the origin is two or three cycles after the write; the
  // restart takes the pending mode and, in the 5-step mode, clocks the quarter- and
  // half-frame units at once (NESdev wiki, "APU", Frame Counter; "APU Frame Counter",
  // Control register). Until it comes the counter keeps stepping in its old mode.
  std::uint8_t restartDelay = 1;
  [[nodiscard]] bool operator==(const ApuState&) const noexcept = default;
};

// What the DMC's memory reader is waiting to fetch: nothing, the first byte of a sample
// after a $4015 write started it, or the next byte of one in progress (NESdev wiki,
// "DMA", DMC DMA).
enum class DmcFetch : std::uint8_t { None, Load, Reload };

// The chip after the reset line: every channel disabled with its counter at 0, the frame
// interrupt flag clear, the sequence restarting at the next tick in the mode last written
// to $4017, the inhibit bit and every halt bit kept (NESdev wiki, "CPU power up state",
// APU, the After Reset column; "APU", Status).
void apuReset(ApuState& state) noexcept;

// A CPU write to `address`, one of $4000-$4013, $4015 or $4017, in a cycle whose APU
// half is `apuPhase`, 0 a get and 1 a put. $4000, $4004, $4008 and $400C take their halt
// bit; $4003, $4007, $400B and $400F load their length counter from the table at the
// cycle's tick while the channel is enabled; $4015 sets the four enables, clearing the
// counter of each channel it disables; $4017 sets the inhibit bit, clearing the flag
// when it is set, and schedules the sequence's restart two cycles on from a put cycle
// and three from a get. Every other address is dropped.
void apuWrite(ApuState& state, std::uint16_t address, std::uint8_t value,
              std::uint8_t apuPhase) noexcept;

// A CPU read of $4015: bit 6 the frame interrupt flag, bits 3-0 whether each length
// counter is above 0, bit 5 the open bus's, bits 7 and 4 0, and the flag is then
// cleared (NESdev wiki, "APU", Status). A set the same cycle's tick makes is not cleared
// by it, since the tick runs after the access.
[[nodiscard]] std::uint8_t apuReadStatus(ApuState& state, std::uint8_t openBus) noexcept;

// What a read of $4015 would answer, with nothing cleared.
[[nodiscard]] std::uint8_t apuPeekStatus(const ApuState& state, std::uint8_t openBus) noexcept;

// One CPU cycle: samples the IRQ line from the flag, counts the restart delay down and
// restarts the sequence when it reaches 0, otherwise steps the counter and runs the step
// that falls on it; then applies each channel's pending load and latches its halt bit.
void apuTickCycle(ApuState& state) noexcept;

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
