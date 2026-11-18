# Render

`nes_render` runs a cartridge on the [NES machine](../../docs/nes-machine.md) from power-on
and writes what its APU plays as a WAV file. Beside it is the file-format piece it uses, the
WAV writer.

## Contents

- [`nes_render`](#nes_render)
- [Rendering in code](#rendering-in-code)
- [The WAV writer](#the-wav-writer)
- [See also](#see-also)

## `nes_render`

```
nes_render <in.nes> (--seconds N | --samples N) [--rate HZ] -o <out.wav>
```

Exactly one of `--seconds` and `--samples` gives the length and `-o` names the output;
`--rate` names the sample rate, 48000 without it. The input is a `.nes` file, header included
([the cartridge](../../docs/nes-cartridge.md)). The machine is built from it at power-on with
both controller ports empty and run forward for exactly the requested length, with no warm-up,
no skip and no fade. No button is pressed, so a program that waits for one plays what it
plays while it waits.

`nes_render --help`, or `-h`, prints every argument with what it means and exits 0. An
argument that begins with `-` and is none of the options is refused, and `--` ends the
options, so an image whose name begins with `-` follows it.

```
nes_render game.nes --seconds 30 -o game.wav
```

The output is a canonical PCM WAV, 16-bit signed, one channel: the machine's frames written
sample for sample, with no resampling, dither or gain. Each sample is the console's output,
the mixer's level through the NES's output filters, averaged over the CPU cycles it spans
([The APU](../../docs/nes-machine.md#the-apu)). The output rests at 0: a file opens at 0,
and a level that stands is taken away within 20 ms.

The command exits 0 with the file written. It exits 1, saying which on stderr, when the
image cannot be opened, when the machine refuses it (a board the machine does not build, a
rate above the CPU clock's 1 789 772) or when the output cannot be written. It exits 2, with
the usage and a line naming `--help`, on arguments it cannot read: an option it does not
have, a length missing, given twice or given both ways, a value that is not a whole number, a
rate of 0, or a length past the 2 147 483 629 samples one WAV file holds.

## Rendering in code

The command is this sequence over the public headers. With `image` holding the file's bytes:

```cpp
#include "neasant/nes/clock.h"
#include "neasant/nes/nes.h"
#include "render/wav_writer.h"

const std::uint32_t rate = 48000;
const std::uint64_t samples = 2 * rate;  // two seconds

neasant::Nes machine{neasant::NesConfig{.rom = image, .audioRate = rate}};
// The master clock is hertzNumerator / hertzDenominator Hz: the cycles `samples` take,
// rounded up.
const neasant::ConsoleClock clock = neasant::consoleClock(machine.region());
const std::uint64_t divisor = clock.hertzDenominator * rate;
machine.run((samples * clock.hertzNumerator + divisor - 1) / divisor);
const std::vector<std::int16_t> frames = machine.takeFrames();
// frames.size() == samples: the run ends on the machine cycle that completes the last one

const std::vector<std::uint8_t> wav = neasant::render::writeWav(frames, rate);
// wav.size() == 44 + 2 * samples: a whole file, for the caller to write
```

Construction throws `std::invalid_argument`, naming what the machine builds instead, for an
image or a rate it refuses; the command prints that text and exits 1.

## The WAV writer

```cpp
#include "render/wav_writer.h"

std::vector<std::uint8_t> file = neasant::render::writeWav(frames, 48000);
```

Returns the whole file as bytes: a 44-byte header and the samples in order, each 16-bit
little-endian signed. The caller writes them. An empty span gives the header alone, with a
data chunk of length 0. A WAV's sizes are 32-bit, so one file holds at most
`neasant::render::kMaxWavSamples` samples, 2 147 483 629, and the caller keeps to that.

The library target is `neasant_render`; `tools/` is on its public include path.

## See also

- [docs/nes-machine.md](../../docs/nes-machine.md): the machine a cartridge is run on, its
  mixer and the frames a host drains.
- [docs/nes-cartridge.md](../../docs/nes-cartridge.md): the image and its header.
- [tools/README.md](../README.md): every tool and how they build.
