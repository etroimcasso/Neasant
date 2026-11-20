#include "wav_writer.h"

#include <cstddef>

namespace neasant::render {

namespace {

constexpr std::uint16_t kChannels = 1;
constexpr std::uint16_t kBitsPerSample = 16;
constexpr std::uint32_t kBytesPerFrame = kChannels * (kBitsPerSample / 8);

// Appends a 16-bit value, low byte first.
void putU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFF));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

// Appends a 32-bit value, low byte first.
void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
  }
}

// Writes the four ASCII bytes of a chunk tag; the literal's closing NUL is left out.
void putTag(std::vector<std::uint8_t>& out, const char (&tag)[5]) {
  for (std::size_t i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>(tag[i]));
  }
}

}  // namespace

std::vector<std::uint8_t> writeWav(std::span<const std::int16_t> samples,
                                   std::uint32_t sampleRate) {
  const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples.size()) * kBytesPerFrame;

  std::vector<std::uint8_t> out;
  out.reserve(44 + std::size_t{dataBytes});

  putTag(out, "RIFF");
  putU32(out, 36 + dataBytes);  // the size of everything after this field
  putTag(out, "WAVE");

  putTag(out, "fmt ");
  putU32(out, 16);  // the PCM fmt chunk's body size
  putU16(out, 1);  // the audio format: PCM
  putU16(out, kChannels);
  putU32(out, sampleRate);
  putU32(out, sampleRate * kBytesPerFrame);  // the byte rate
  putU16(out, static_cast<std::uint16_t>(kBytesPerFrame));  // the block align
  putU16(out, kBitsPerSample);

  putTag(out, "data");
  putU32(out, dataBytes);
  for (const std::int16_t sample : samples) {
    putU16(out, static_cast<std::uint16_t>(sample));
  }
  return out;
}

}  // namespace neasant::render
