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

## Reading these pages

Each page opens with what its subject is and what it is for, then the surface, then worked
examples, then the gotchas worth knowing before they cost an afternoon. A page describes what the
code does now; where a subject is modelled but not yet complete, the page says so in place.
