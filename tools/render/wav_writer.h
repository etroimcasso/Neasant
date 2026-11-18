#pragma once

// The WAV writer: encodes samples as a canonical PCM WAV file, one channel of 16-bit
// signed samples. The file is a 44-byte header, its RIFF, fmt and data chunks, followed by
// the samples in order, each little-endian. The bytes are the samples: nothing is
// resampled, dithered or scaled, and no metadata chunk is added. The rate given is
// written into the header.

#include <cstdint>
#include <span>
#include <vector>

namespace neasant::render {

// The most samples one file holds: a WAV's sizes are 32-bit, and the RIFF chunk's counts
// the 36 header bytes after it with the two bytes of every sample.
inline constexpr std::uint64_t kMaxWavSamples = 2147483629;

// Encodes `samples` as a PCM WAV at `sampleRate` and returns the whole file as bytes,
// header and data; the caller writes them. An empty span gives the 44-byte header alone,
// its data chunk of length 0. The caller keeps the span within kMaxWavSamples.
[[nodiscard]] std::vector<std::uint8_t> writeWav(std::span<const std::int16_t> samples,
                                                 std::uint32_t sampleRate);

}  // namespace neasant::render
