#include "neasant/nes/clock.h"
#include "neasant/nes/nes.h"
#include "authored_image.h"
#include "render/wav_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The render tool: the WAV writer driven with samples set by hand, every byte of the
// header written out from the format's fields; and a cartridge rendered as nes_render
// renders it, an authored program on a machine at power-on run for the master cycles its
// samples take, with the count, the bytes and the beep's period derived from the master
// clock, the rate and the registers the program writes. A cycle below is a machine cycle
// counted from power-on, whose first seven cycles are the reset sequence.

namespace {

using neasant::consoleClock;
using neasant::Nes;
using neasant::NesConfig;
using neasant::Region;
using neasant::nes_tests::authoredImage;
using neasant::nes_tests::inesHeader;
using neasant::nes_tests::placeProgram;
using neasant::render::writeWav;

using Bytes = std::vector<std::uint8_t>;
using Frames = std::vector<std::int16_t>;

// The 16-bit value at an offset, low byte first.
std::uint16_t readU16(const Bytes& bytes, std::size_t at) {
  return static_cast<std::uint16_t>(bytes[at] | bytes[at + 1] << 8);
}

// The 32-bit value at an offset, low byte first.
std::uint32_t readU32(const Bytes& bytes, std::size_t at) {
  return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8 |
         static_cast<std::uint32_t>(bytes[at + 2]) << 16 |
         static_cast<std::uint32_t>(bytes[at + 3]) << 24;
}

// The four characters of the chunk tag at an offset.
std::string tagAt(const Bytes& bytes, std::size_t at) {
  std::string tag;
  for (std::size_t i = 0; i < 4; ++i) tag.push_back(static_cast<char>(bytes[at + i]));
  return tag;
}

// The file's first 44 bytes: the RIFF, fmt and data chunks' headers.
Bytes headerOf(const Bytes& wav) {
  return Bytes(wav.begin(), wav.begin() + 44);
}

// Pulse 1 at a 50% duty, constant volume 15 and a period of $06F: the sequencer steps
// every 2 * ($06F + 1) = 224 CPU cycles and its eight steps make a wave every 1792
// (NESdev wiki, "APU Pulse").
// LDA #$01; STA $4015; LDA #$BF; STA $4000; LDA #$7F; STA $4001; LDA #$6F; STA $4002;
// LDA #$00; STA $4003; JMP $8019
const Bytes kBeepProgram{0xA9, 0x01, 0x8D, 0x15, 0x40, 0xA9, 0xBF, 0x8D, 0x00, 0x40,
                         0xA9, 0x7F, 0x8D, 0x01, 0x40, 0xA9, 0x6F, 0x8D, 0x02, 0x40,
                         0xA9, 0x00, 0x8D, 0x03, 0x40, 0x4C, 0x19, 0x80};

// A 16 KB NROM image with 8 KB of CHR ROM, the beep's program at $8000 and every vector
// there.
Bytes beepImage() {
  Bytes image = authoredImage(inesHeader({}), 16384, 8192);
  placeProgram(image, 0, kBeepProgram);
  return image;
}

// The master cycles `samples` samples at `rate` take, rounded up: the master clock is
// hertzNumerator / hertzDenominator Hz (NESdev wiki, "CPU", Frequencies).
std::uint64_t budgetFor(std::uint64_t samples, std::uint32_t rate) {
  const neasant::ConsoleClock clock = consoleClock(Region::Ntsc);
  const std::uint64_t divisor = clock.hertzDenominator * rate;
  return (samples * clock.hertzNumerator + divisor - 1) / divisor;
}

// Renders `samples` samples of `image` at `rate` as nes_render does: a machine built from
// the image at power-on, run for the master cycles the samples take and drained once.
Frames render(const Bytes& image, std::uint64_t samples, std::uint32_t rate) {
  Nes machine(NesConfig{.rom = image, .audioRate = rate});
  machine.run(budgetFor(samples, rate));
  return machine.takeFrames();
}

// The samples of a WAV file's data chunk.
Frames samplesOf(const Bytes& wav) {
  Frames samples;
  for (std::size_t at = 44; at + 1 < wav.size(); at += 2) {
    samples.push_back(static_cast<std::int16_t>(readU16(wav, at)));
  }
  return samples;
}

// ---- The WAV writer ----

TEST(WavWriter, CarriesTheRiffAndWaveTags) {
  const Bytes wav = writeWav(Frames{1, 2, 3, 4}, 48000);
  EXPECT_EQ(tagAt(wav, 0), "RIFF");
  EXPECT_EQ(tagAt(wav, 8), "WAVE");
}

TEST(WavWriter, DeclaresPcmMono16InTheFmtChunk) {
  const Bytes wav = writeWav(Frames{0}, 48000);
  EXPECT_EQ(tagAt(wav, 12), "fmt ");
  EXPECT_EQ(readU32(wav, 16), 16u);  // the PCM fmt chunk's body size
  EXPECT_EQ(readU16(wav, 20), 1u);   // the audio format: PCM
  EXPECT_EQ(readU16(wav, 22), 1u);   // channels
  EXPECT_EQ(readU16(wav, 34), 16u);  // bits a sample
}

TEST(WavWriter, WritesTheSampleRateAndItsDerivedFields) {
  const Bytes at48000 = writeWav(Frames{0}, 48000);
  EXPECT_EQ(readU32(at48000, 24), 48000u);  // the sample rate
  EXPECT_EQ(readU32(at48000, 28), 96000u);  // the byte rate: the rate times the block align
  EXPECT_EQ(readU16(at48000, 32), 2u);      // the block align: one channel of two bytes

  const Bytes at44100 = writeWav(Frames{0}, 44100);
  EXPECT_EQ(readU32(at44100, 24), 44100u);  // the sample rate
  EXPECT_EQ(readU32(at44100, 28), 88200u);  // the byte rate
  EXPECT_EQ(readU16(at44100, 32), 2u);      // the block align
}

TEST(WavWriter, SizesTheDataAndRiffChunksToTheSampleCount) {
  const Bytes wav = writeWav(Frames(10, 0), 48000);
  EXPECT_EQ(tagAt(wav, 36), "data");
  EXPECT_EQ(readU32(wav, 40), 20u);  // the data chunk's body: two bytes a sample
  EXPECT_EQ(readU32(wav, 4), 56u);   // the RIFF chunk's size: 36 and the data's body
  EXPECT_EQ(wav.size(), 64u);        // the whole file: 44 and the data's body
}

TEST(WavWriter, WritesEachSampleLowByteFirstInOrder) {
  const Bytes wav = writeWav(Frames{0x1234, -1, -32768, 32767}, 48000);
  ASSERT_EQ(wav.size(), 52u);
  EXPECT_EQ(Bytes(wav.begin() + 44, wav.end()),
            (Bytes{0x34, 0x12, 0xFF, 0xFF, 0x00, 0x80, 0xFF, 0x7F}));
}

TEST(WavWriter, TheHeaderIsFortyFourBytesForNoSamplesAndForSome) {
  // No samples at 48 000 Hz: a RIFF size of 36, a rate of $0000BB80, a byte rate of
  // $00017700 and a data chunk of length 0.
  const Bytes empty = writeWav(Frames{}, 48000);
  EXPECT_EQ(empty, (Bytes{'R',  'I',  'F',  'F',  0x24, 0x00, 0x00, 0x00, 'W',  'A',  'V',
                          'E',  'f',  'm',  't',  ' ',  0x10, 0x00, 0x00, 0x00, 0x01, 0x00,
                          0x01, 0x00, 0x80, 0xBB, 0x00, 0x00, 0x00, 0x77, 0x01, 0x00, 0x02,
                          0x00, 0x10, 0x00, 'd',  'a',  't',  'a',  0x00, 0x00, 0x00, 0x00}));

  // Three samples at 44 100 Hz: a RIFF size of 42, a rate of $0000AC44, a byte rate of
  // $00015888 and a data chunk of six bytes.
  const Bytes three = writeWav(Frames{1, 2, 3}, 44100);
  ASSERT_EQ(three.size(), 50u);
  EXPECT_EQ(headerOf(three),
            (Bytes{'R',  'I',  'F',  'F',  0x2A, 0x00, 0x00, 0x00, 'W',  'A',  'V',
                   'E',  'f',  'm',  't',  ' ',  0x10, 0x00, 0x00, 0x00, 0x01, 0x00,
                   0x01, 0x00, 0x44, 0xAC, 0x00, 0x00, 0x88, 0x58, 0x01, 0x00, 0x02,
                   0x00, 0x10, 0x00, 'd',  'a',  't',  'a',  0x06, 0x00, 0x00, 0x00}));
}

// ---- A cartridge rendered ----

// The budget is the master cycles the samples take, rounded up, and a run of it from
// power-on ends on the machine cycle that completes the last sample, at any rate.
TEST(NesRender, TheRunQueuesExactlyTheSamplesAskedFor) {
  // 236 250 000 / 528 000 = 447.4 master cycles, rounded up: 38 machine cycles, the cycle
  // the first sample completes on.
  EXPECT_EQ(budgetFor(1, 48000), 448u);
  // The master clock's 21 477 272.7 cycles a second, rounded up.
  EXPECT_EQ(budgetFor(48000, 48000), 21477273u);

  const Bytes image = beepImage();
  EXPECT_EQ(render(image, 1, 48000).size(), 1u);
  EXPECT_EQ(render(image, 4800, 48000).size(), 4800u);
  EXPECT_EQ(render(image, 48000, 48000).size(), 48000u);
  EXPECT_EQ(render(image, 44100, 44100).size(), 44100u);
  // 1000 samples at the CPU clock take 1000.0004 cycles: a budget rounded down runs 1000
  // cycles and queues 999.
  EXPECT_EQ(render(image, 1000, 1789772).size(), 1000u);
  EXPECT_EQ(render(image, 3, 1).size(), 3u);
}

TEST(NesRender, ZeroSamplesProduceABareHeader) {
  EXPECT_EQ(budgetFor(0, 48000), 0u);
  EXPECT_EQ(writeWav(render(beepImage(), 0, 48000), 48000).size(), 44u);
}

// The run is the same from the same image and budget, so its file is too. The file's
// samples range from -2689, a low half of the beep's wave once the output stage has
// centred it, to 4731, the wave's first high half before it has (apuFilter in apu.h).
TEST(NesRender, SameImageAndBudgetProduceIdenticalBytes) {
  const Bytes image = beepImage();
  const Bytes first = writeWav(render(image, 4800, 48000), 48000);
  const Bytes second = writeWav(render(image, 4800, 48000), 48000);
  EXPECT_EQ(first.size(), 9644u);  // 44 and two bytes for each of 4800 samples
  EXPECT_EQ(first, second);

  const Frames samples = samplesOf(first);
  ASSERT_FALSE(samples.empty());
  EXPECT_EQ(*std::min_element(samples.begin(), samples.end()), -2689);
  EXPECT_EQ(*std::max_element(samples.begin(), samples.end()), 4731);
}

// The write to $4003 lands in cycle 36 and returns the sequencer to its first step, low
// at this duty; the timer, reloaded with $06F at cycle 30's tick, steps the sequencer 224
// cycles later, so the waveform rises at cycles 254 + 1792 k. The output opens at 0, so
// the first rise starts from 0 and crosses nothing; the wave first rises through 0 at its
// second, cycle 2046, which falls late in sample 54: sample 55 is the first at or above 0.
// The second's last rise, cycle 1 788 670, falls in sample 47 970, which the low-pass's
// climb leaves below 0, so it reads at 47 971: 998 rises. A sample at 48 000 Hz takes
// 39375 / 1056 CPU cycles.
TEST(NesRender, AnAuthoredBeepHasThePeriodItsRegistersSet) {
  const Frames samples = samplesOf(writeWav(render(beepImage(), 48000, 48000), 48000));
  ASSERT_EQ(samples.size(), 48000u);
  const std::int16_t least = *std::min_element(samples.begin(), samples.end());
  const std::int16_t greatest = *std::max_element(samples.begin(), samples.end());
  EXPECT_EQ(least, -2689);
  EXPECT_EQ(greatest, 4731);

  // A rise is an index whose sample is at or above 0 and whose predecessor is below it.
  std::vector<std::int64_t> rises;
  for (std::size_t i = 1; i < samples.size(); ++i) {
    if (samples[i] >= 0 && samples[i - 1] < 0) {
      rises.push_back(static_cast<std::int64_t>(i));
    }
  }
  ASSERT_EQ(rises.size(), 998u);
  EXPECT_EQ(rises.front(), 55);
  EXPECT_EQ(rises.back(), 47971);

  // The distance in samples times the cycles a sample takes, over the waves, rounded to
  // the nearest cycle: 16 * ($06F + 1).
  const std::int64_t waves = static_cast<std::int64_t>(rises.size()) - 1;
  const std::int64_t first = rises.front();
  const std::int64_t last = rises.back();
  EXPECT_EQ((2 * (last - first) * 39375 + 1056 * waves) / (2 * 1056 * waves), 1792);
}

}  // namespace
