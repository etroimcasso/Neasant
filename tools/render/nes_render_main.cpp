// nes_render: boots a cartridge image on the NES machine, runs it from power-on for a
// requested length and writes the APU's output as a WAV file, one channel of 16-bit
// samples.
//
//   nes_render <in.nes> (--seconds N | --samples N) [--rate HZ] -o <out.wav>
//   nes_render --help
//
// The cartridge's own code drives the APU, so a render carries what the program plays
// with both controller ports empty. Rendering starts at power-on and runs forward
// verbatim, with no warm-up, no skip and no fade: the opening samples are the console's
// output before the program touches a channel, which rests at 0. N samples at a rate take
// N times the master clock's rate over the sample rate in master cycles, rounded up, and
// the run ends on the machine cycle that completes sample N, so the machine queues
// exactly N.
// The samples are the console's output (takeFrames in nes.h); the rate is 48000 unless
// --rate names another, and the machine refuses one above its CPU clock.
//
// The command exits 0 with the file written; 1 when the image cannot be opened, the
// machine refuses it or the output cannot be written, saying which on stderr; and 2,
// with the usage, on arguments it cannot read. --help prints every argument with what
// it means and exits 0.

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "neasant/nes/clock.h"
#include "neasant/nes/nes.h"
#include "wav_writer.h"

namespace {

constexpr std::uint32_t kDefaultRate = 48000;

// The command line in one line, as the usage and the help both open.
void usageLine(std::ostream& out, const char* prog) {
  out << "usage: " << prog
      << " <in.nes> (--seconds N | --samples N) [--rate HZ] -o <out.wav>\n";
}

[[noreturn]] void usage(const char* prog) {
  usageLine(std::cerr, prog);
  std::cerr << "try '" << prog << " --help' for the arguments\n";
  std::exit(2);
}

// Every argument with what it means, the run it makes and the exit statuses.
void help(const char* prog) {
  usageLine(std::cout, prog);
  std::cout
      << "\n"
         "Runs a cartridge image on the NES machine from power-on and writes what its APU\n"
         "plays as a WAV file, one channel of 16-bit samples.\n"
         "\n"
         "  <in.nes>       the cartridge image, header included\n"
         "  --seconds N    the length to render, in whole seconds\n"
         "  --samples N    the length to render, in samples\n"
         "  --rate HZ      the sample rate, 48000 without it; the CPU clock's rate at most\n"
         "  -o <out.wav>   the file to write\n"
         "  -h, --help     print this help and exit\n"
         "  --             every argument after it is the image, whatever it begins with\n"
         "\n"
         "Exactly one of --seconds and --samples gives the length. The render starts at\n"
         "power-on and runs for exactly that length, with no warm-up, skip or fade. Both\n"
         "controller ports are empty and no button is pressed.\n"
         "\n"
         "Exit status: 0 with the file written; 1 when the image cannot be opened, the\n"
         "machine refuses it or the output cannot be written; 2 on arguments it cannot\n"
         "read.\n"
         "\n"
         "Example:\n"
         "  "
      << prog << " game.nes --seconds 30 -o game.wav\n";
}

// The whole number `text` spells in decimal digits, or nothing for any other text.
std::optional<std::uint64_t> wholeNumber(std::string_view text) {
  std::uint64_t value = 0;
  const char* const end = text.data() + text.size();
  const auto [ptr, error] = std::from_chars(text.data(), end, value);
  if (error != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  std::string inPath;
  std::string outPath;
  std::optional<std::uint64_t> seconds;
  std::optional<std::uint64_t> samples;
  std::optional<std::uint64_t> rate;
  bool operandsOnly = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    // The argument after the option, or the usage when there is none.
    auto next = [&]() -> std::string_view {
      if (i + 1 >= argc) {
        std::cerr << arg << " needs a value\n";
        usage(argv[0]);
      }
      return argv[++i];
    };
    // The option's value read as a whole number into `into`, once.
    auto number = [&](std::optional<std::uint64_t>& into) {
      if (into) {
        std::cerr << arg << " is given twice\n";
        usage(argv[0]);
      }
      into = wholeNumber(next());
      if (!into) {
        std::cerr << arg << " needs a whole number\n";
        usage(argv[0]);
      }
    };
    if (operandsOnly) {
      if (!inPath.empty()) {
        usage(argv[0]);
      }
      inPath = arg;
    } else if (arg == "-h" || arg == "--help") {
      help(argv[0]);
      return 0;
    } else if (arg == "--") {
      operandsOnly = true;
    } else if (arg == "--seconds") {
      number(seconds);
    } else if (arg == "--samples") {
      number(samples);
    } else if (arg == "--rate") {
      number(rate);
    } else if (arg == "-o") {
      if (!outPath.empty()) {
        std::cerr << "-o is given twice\n";
        usage(argv[0]);
      }
      outPath = next();
    } else if (arg.starts_with('-')) {
      std::cerr << "unknown option " << arg << "\n";
      usage(argv[0]);
    } else if (inPath.empty()) {
      inPath = arg;
    } else {
      usage(argv[0]);
    }
  }
  if (inPath.empty() || outPath.empty() || seconds.has_value() == samples.has_value()) {
    usage(argv[0]);
  }

  const std::uint64_t hertz = rate.value_or(kDefaultRate);
  if (hertz == 0 || hertz > std::numeric_limits<std::uint32_t>::max()) {
    std::cerr << "--rate needs a rate from 1 to 4294967295\n";
    usage(argv[0]);
  }

  // A length in seconds is that many times the rate in samples; either way the count is
  // held to what one WAV file carries.
  const std::uint64_t most = neasant::render::kMaxWavSamples;
  if ((seconds && *seconds > most / hertz) || (samples && *samples > most)) {
    std::cerr << "a WAV file holds at most " << most << " samples\n";
    usage(argv[0]);
  }
  const std::uint64_t count = seconds ? *seconds * hertz : *samples;

  std::ifstream in(inPath, std::ios::binary);
  if (!in) {
    std::cerr << "cannot open " << inPath << "\n";
    return 1;
  }
  const std::vector<std::uint8_t> image{std::istreambuf_iterator<char>(in),
                                        std::istreambuf_iterator<char>()};

  std::vector<std::int16_t> frames;
  try {
    neasant::Nes machine{
        neasant::NesConfig{.rom = image, .audioRate = static_cast<std::uint32_t>(hertz)}};
    // The master clock is hertzNumerator / hertzDenominator Hz, so `count` samples at
    // `hertz` take count * hertzNumerator / (hertzDenominator * hertz) master cycles.
    const neasant::ConsoleClock clock = neasant::consoleClock(machine.region());
    const std::uint64_t divisor = clock.hertzDenominator * hertz;
    machine.run((count * clock.hertzNumerator + divisor - 1) / divisor);
    frames = machine.takeFrames();
  } catch (const std::invalid_argument& refusal) {
    std::cerr << "cannot render " << inPath << ": " << refusal.what() << "\n";
    return 1;
  }

  const std::vector<std::uint8_t> wav =
      neasant::render::writeWav(frames, static_cast<std::uint32_t>(hertz));
  std::ofstream out(outPath, std::ios::binary);
  if (out) {
    out.write(reinterpret_cast<const char*>(wav.data()),
              static_cast<std::streamsize>(wav.size()));
    out.flush();
  }
  if (!out.good()) {
    std::cerr << "cannot write " << outPath << "\n";
    return 1;
  }
  return 0;
}
