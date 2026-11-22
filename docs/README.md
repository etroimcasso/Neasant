# Neasant documentation

These pages describe what is built and how to use it. Every value in them is derived from public
hardware documentation and validated against it. Where the published documentation is incomplete,
ambiguous or wrong, a `*-behavior.md` page beside the component's page records what the hardware
does instead and the measurement that settles each case.

The [project README](../README.md) carries the status table — what is built, what is partial,
and what is unstarted.

## Pages

One row per page; each page lands with its component:

| Page | Covers |
|---|---|
| [6502-cpu.md](6502-cpu.md) | The 2A03's CPU core — its bus and state, stepping it a cycle or an instruction at a time, where every cycle of every addressing mode goes, the interrupt, reset and halt sequences, the unofficial opcodes, and the vector suite |
| [nes-machine.md](nes-machine.md) | The NES machine around the core — building it from a config value, the memory map and open bus, where the APU meets the console and the frames a host drains, stepping it by the cycle or by a master-cycle budget, the reset line, the clock, the controller ports, OAM DMA and the DMC's fetch, the bus, save and frame observers, snapshot and restore, and the test programs it runs |
| [nes-cartridge.md](nes-cartridge.md) | The cartridge as a value — the iNES and NES 2.0 headers byte by byte, the cartridge built from an image with a host's overrides, the pins a board sees, the board registry, and NROM |
| [ppu.md](ppu.md) | The PPU's register file and picture — the eight ports with their latches and side effects, the I/O bus and its decay, the scroll file, the chip's address space and the palette, the frame counter with its flag and odd-frame skip, the background's accesses, scroll and shift registers, the picture and what a dot holds, the NMI output and the $2002 race, the warm-up, power-on and reset, and the test programs it passes |
| [apu.md](apu.md) | The APU over a state value — the registers, the frame counter with its two sequences and its interrupt, the status register, the length counters, envelopes and sweeps, the pulse, triangle and noise channels, the DMC with its sample fetch, the mixer, the console's output filters and the decimator, power-on and reset, and the test programs it passes |
| [apu-behavior.md](apu-behavior.md) | APU behavior and provenance — the cases where the published documentation is silent or at odds with itself, each with the documents' own words and what settles it, and what remains open |

## Reading these pages

Each page opens with what its subject is and what it is for, then the surface, then worked
examples, then the gotchas worth knowing before they cost an afternoon. A page describes what the
code does now; where a subject is modelled in part, the page says so in place.
