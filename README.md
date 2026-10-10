# nescore

A clean-room, MIT-licensed implementation of the NES — cycle-accurate, deterministic, and
embeddable.

The name `nescore` is a placeholder. It is used consistently for the namespace, the header
directory and the library target so that the eventual name is a single rename.

## Contents

- [Why this exists](#why-this-exists)
- [Design commitments](#design-commitments)
- [What is built](#what-is-built)
- [Getting started](#getting-started)
- [Documentation](#documentation)
- [License](#license)

## Why this exists

There is no MIT-compatible, cycle-accurate, embeddable NES core. The established emulators are
exceptional work, but their licenses keep them out of permissively-licensed engines, tools and
commercial products. This project fills that gap: a NES implementation you can vendor into
anything, under MIT, with clean provenance.

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

The CPU is built and documented; the rest of the machine follows it. This table gains a row's
link and description as each component lands.

| Component | Status |
|---|---|
| [6502 CPU core](docs/6502-cpu.md) | **complete** — 256 opcodes, cycle-stepped, every cycle checked against the SingleStepTests nes6502 vectors; the reset, interrupt and RDY-halt sequences cycle by cycle |
| Machine (bus, RAM, cartridge boundary, controllers) | **not started** |
| PPU | **not started** |
| APU | **not started** |
| Mappers | **not started** |
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
add_subdirectory(nescore)
target_link_libraries(your_target PRIVATE nescore::nescore)
```

### The test data

Third-party reference data is never vendored; the tests that use it register and skip, with a
visible reason, until they are pointed at a local copy.

| Option | Points at |
|---|---|
| `NESCORE_NES6502_VECTORS` | the SingleStepTests `65x02/nes6502/v1` directory |

```
cmake -B build -DNESCORE_NES6502_VECTORS=/path/to/65x02/nes6502/v1
```

`NESCORE_BUILD_TESTS` turns the suite on or off explicitly.

## Documentation

[docs/README.md](docs/README.md) indexes every page. A page per component is added as the
component lands.

## License

MIT — see [LICENSE](LICENSE).
