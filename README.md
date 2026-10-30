# Neasant

A clean-room, MIT-licensed implementation of the NES — cycle-accurate, deterministic, and
embeddable. Pronounced "NESn't," like Peasant with an N.

## Contents

- [Design commitments](#design-commitments)
- [What is built](#what-is-built)
- [Getting started](#getting-started)
- [Documentation](#documentation)
- [License](#license)

## Design commitments

- **Clean-room.** Implemented from public hardware documentation, published hardware
  measurements and self-checking test programs only. No emulator source is consulted, and no
  copyrighted bytes (game ROMs, the Famicom Disk System BIOS) are ever included.
- **Cycle-accurate.** The machine advances in master clock cycles. Every CPU bus access, every
  PPU dot and every APU tick lands on the master cycle the hardware puts it on, and the
  cartridge sees the same pins the hardware drives.
- **Deterministic and steppable.** Components advance by an externally supplied cycle budget —
  no wall clock, no threads inside the core. Everything the hardware leaves to chance at
  power-on (the CPU/PPU clock alignment, the contents of RAM) is a documented value the host
  chooses. The same starting state and the same inputs produce the same bytes, every run;
  whole-machine state snapshots and restores as a value.
- **Embeddable.** A C++20 static library with a declarative API: no UI, no audio device, no file
  formats required at the boundary beyond the cartridge header. The host owns time, I/O, the
  picture and the output samples.
- **Accurate.** Behavior is held to public hardware documentation and validated against real
  program output. Accuracy is never traded away for API convenience.

## What is built

The CPU, the machine around it and the cartridge are built and documented; the PPU holds its
register file and frame timing with no picture, and NROM is the one board. This table gains a
row's link and description as each component lands.

| Component | Status |
|---|---|
| [6502 CPU core](docs/6502-cpu.md) | **complete** — 256 opcodes, cycle-stepped, every cycle checked against the SingleStepTests nes6502 vectors; the reset, interrupt and RDY-halt sequences cycle by cycle |
| [Machine (bus, RAM, cartridge boundary, controllers)](docs/nes-machine.md) | **complete** — the master-cycle budget, the console's bus and open bus, the two controller ports, OAM DMA, the bus and save observers, snapshot and restore; the CPU's self-checking test programs pass on it |
| [PPU](docs/ppu.md) | **in progress** — the register file, the frame's timing and the NMI, with no picture |
| APU | **not started** |
| [Mappers](docs/nes-cartridge.md) | **in progress** — the cartridge as a value, the pin-level board boundary, NROM |
| Peripherals | **not started** |
| Player | **not started** |
| PAL and Dendy variants | **not started** |

## Getting started

### Building

Requires a C++20 toolchain and CMake 3.24 or later. A build is optimized by default.

```
cmake -B build
cmake --build build
ctest --test-dir build
```

### Embedding the library

Vendor the repository (a git submodule works) and link the library target. The test suite is
off when the library is built inside a parent project.

```cmake
add_subdirectory(neasant)
target_link_libraries(your_target PRIVATE neasant::neasant)
```

### The test data

Third-party reference data is never vendored; the tests that use it register and skip, with a
visible reason, until they are pointed at a local copy.

| Option | Points at |
|---|---|
| `NEASANT_NES6502_VECTORS` | the SingleStepTests `65x02/nes6502/v1` directory |
| `NEASANT_NES_TEST_ROMS` | a clone of `christopherpow/nes-test-roms`, the self-checking NES test programs |

```
cmake -B build -DNEASANT_NES6502_VECTORS=/path/to/65x02/nes6502/v1 \
               -DNEASANT_NES_TEST_ROMS=/path/to/nes-test-roms
```

`NEASANT_BUILD_TESTS` turns the suite on or off explicitly.

## Documentation

[docs/README.md](docs/README.md) indexes every page. A page per component is added as the
component lands.

## License

MIT — see [LICENSE](LICENSE).
