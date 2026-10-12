#include "vector_harness.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace neasant::cpu_vectors {
namespace {

// A recursive-descent reader over the vector format: a top-level array of objects,
// each { name, initial, final, cycles }.
struct Parser {
  const char* p;
  const char* end;

  [[noreturn]] void fail(const char* message) const {
    throw std::runtime_error(std::string("vector JSON: ") + message);
  }

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }
  char cur() {
    ws();
    if (p >= end) fail("unexpected end of input");
    return *p;
  }
  void want(char c) {
    if (cur() != c) fail("unexpected character");
    ++p;
  }
  bool accept(char c) {
    if (p < end && cur() == c) {
      ++p;
      return true;
    }
    return false;
  }

  std::string str() {
    want('"');
    std::string s;
    while (true) {
      if (p >= end) fail("unterminated string");
      const char c = *p++;
      if (c == '"') break;
      if (c == '\\') {
        if (p >= end) fail("dangling escape");
        const char e = *p++;
        switch (e) {
          case '"': s += '"'; break;
          case '\\': s += '\\'; break;
          case '/': s += '/'; break;
          case 'n': s += '\n'; break;
          case 't': s += '\t'; break;
          case 'r': s += '\r'; break;
          default: fail("unsupported escape");
        }
      } else {
        s += c;
      }
    }
    return s;
  }

  // A non-negative integer no wider than `max`.
  unsigned long integer(unsigned long max) {
    ws();
    if (p >= end || std::isdigit(static_cast<unsigned char>(*p)) == 0) {
      fail("expected a non-negative integer");
    }
    unsigned long v = 0;
    while (p < end && std::isdigit(static_cast<unsigned char>(*p)) != 0) {
      const auto digit = static_cast<unsigned long>(*p - '0');
      if (v > (max - digit) / 10) fail("integer out of range for its field");
      v = v * 10 + digit;
      ++p;
    }
    return v;
  }
  std::uint16_t word() { return static_cast<std::uint16_t>(integer(0xFFFF)); }
  std::uint8_t byte() { return static_cast<std::uint8_t>(integer(0xFF)); }

  // Skips a value under a key the reader does not map.
  void skipValue() {
    const char c = cur();
    if (c == '{') {
      want('{');
      if (!accept('}')) {
        do {
          str();
          want(':');
          skipValue();
        } while (accept(','));
        want('}');
      }
    } else if (c == '[') {
      want('[');
      if (!accept(']')) {
        do {
          skipValue();
        } while (accept(','));
        want(']');
      }
    } else if (c == '"') {
      str();
    } else {
      integer(~0ul);
    }
  }

  std::vector<std::pair<std::uint16_t, std::uint8_t>> ramPairs() {
    std::vector<std::pair<std::uint16_t, std::uint8_t>> pairs;
    want('[');
    if (!accept(']')) {
      do {
        want('[');
        const std::uint16_t address = word();
        want(',');
        const std::uint8_t value = byte();
        want(']');
        pairs.emplace_back(address, value);
      } while (accept(','));
      want(']');
    }
    return pairs;
  }

  std::vector<CycleTrace> cycleTraces() {
    std::vector<CycleTrace> traces;
    want('[');
    if (!accept(']')) {
      do {
        CycleTrace trace;
        want('[');
        trace.address = word();
        want(',');
        trace.value = byte();
        want(',');
        const std::string direction = str();
        if (direction == "write") {
          trace.write = true;
        } else if (direction != "read") {
          fail("cycle direction is neither \"read\" nor \"write\"");
        }
        want(']');
        traces.push_back(trace);
      } while (accept(','));
      want(']');
    }
    return traces;
  }

  RegState regs() {
    RegState r;
    want('{');
    if (!accept('}')) {
      do {
        const std::string key = str();
        want(':');
        if (key == "pc") {
          r.pc = word();
        } else if (key == "s") {
          r.s = byte();
        } else if (key == "a") {
          r.a = byte();
        } else if (key == "x") {
          r.x = byte();
        } else if (key == "y") {
          r.y = byte();
        } else if (key == "p") {
          r.p = byte();
        } else if (key == "ram") {
          r.ram = ramPairs();
        } else {
          skipValue();
        }
      } while (accept(','));
      want('}');
    }
    return r;
  }

  VectorCase testCase() {
    VectorCase c;
    want('{');
    if (!accept('}')) {
      do {
        const std::string key = str();
        want(':');
        if (key == "name") {
          c.name = str();
        } else if (key == "initial") {
          c.initial = regs();
        } else if (key == "final") {
          c.final_ = regs();
        } else if (key == "cycles") {
          c.cycles = cycleTraces();
        } else {
          skipValue();
        }
      } while (accept(','));
      want('}');
    }
    return c;
  }

  std::vector<VectorCase> file() {
    std::vector<VectorCase> cases;
    want('[');
    if (!accept(']')) {
      do {
        cases.push_back(testCase());
      } while (accept(','));
      want(']');
    }
    ws();
    if (p != end) fail("trailing content after the top-level array");
    return cases;
  }
};

}  // namespace

bool isWrite(CycleKind kind) noexcept {
  return kind == CycleKind::DataWrite || kind == CycleKind::RmwDummyWrite ||
         kind == CycleKind::RmwWrite || kind == CycleKind::StackPush;
}

std::vector<VectorCase> parseVectorFile(const std::string& text) {
  Parser parser{text.data(), text.data() + text.size()};
  return parser.file();
}

std::optional<std::string> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace neasant::cpu_vectors
