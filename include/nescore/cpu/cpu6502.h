#pragma once

// The CPU of the 2A03: a cycle-stepped 6502 whose ADC and SBC are binary.
//
// The core is cycle-stepped: stepCycle() executes exactly one CPU cycle, and every
// cycle is one bus access. The 6502 drives the bus on every cycle, so a cycle is
// either a read or a write, each tagged with a CycleKind naming what it is for. A
// machine that prices memory by region, lends the bus to a DMA unit part-way through
// an instruction, or watches a single access needs that grain; so does a test that
// compares the core against a per-cycle recording of the chip.
//
// Instruction progress is part of the state value: an instruction register, a cycle
// index within the instruction, and a few scratch fields. state() and restore() are
// legal at any cycle, mid-instruction included, and a snapshot is a plain copy of
// the struct.
//
// stepInstruction() runs cycles to the next instruction boundary and returns how
// many it took: the cycles the instruction executed, counted as they run.
//
// The core runs over an abstract bus (the CpuBus concept): a flat test memory and
// the full machine both satisfy it, and the machine binds its own bus type with no
// indirection. Reads can have side effects on the real system, so the core issues
// every documented access, the discarded ones included.
//
// Power-on and reset are sequences the core steps. powerOn() answers the state the
// chip holds at power-on with the reset sequence pending; reset() makes the same
// sequence pending on a running core. The sequence's seven cycles pass through the
// bus like any others.

#include <concepts>
#include <cstdint>
#include <string_view>

namespace nescore {

// What a bus access is for. A bus prices a cycle by the region it reaches and the
// purpose it serves, an observer names it, and a watch tells an instruction fetch
// from a data load.
enum class CycleKind : std::uint8_t {
  OpcodeFetch,   // the first byte of an instruction
  OperandFetch,  // an instruction's operand bytes, read at the program counter
  DataRead,      // the byte an instruction loads, or a byte of a pointer an indirect mode reads
  DataWrite,     // the byte an instruction stores
  // A read whose value is discarded: the second cycle of an implied instruction,
  // the wrong-page read of an indexed access, a branch's fix-up read, the idle
  // reads of the stack instructions, the reads that open a hardware sequence.
  DummyRead,
  RmwRead,        // the read of a read-modify-write
  RmwDummyWrite,  // the unmodified value written back by a read-modify-write
  RmwWrite,       // the modified value
  StackPush,      // a write to page one at the stack pointer
  StackPull,      // a read from page one at the stack pointer
  VectorRead,     // a byte of the reset, NMI or IRQ/BRK vector
  Halted,         // a read repeated while the RDY line is low
};

// The 6502's view of the system: a 16-bit address space with 8-bit data. A
// conforming bus answers a read of any address and accepts a write to any address,
// each told what the access is for. Every cycle the core runs calls exactly one of
// the two, once, so a count of bus calls is a count of cycles.
template <typename B>
concept CpuBus = requires(B bus, std::uint16_t address, std::uint8_t value,
                          CycleKind kind) {
  { bus.read(address, kind) } -> std::same_as<std::uint8_t>;
  bus.write(address, value, kind);
};

// A hardware sequence: the seven cycles that save the program counter and status
// and load a vector. It has no opcode, so the state names which one is running.
// BRK runs the same cycles as an instruction and is not one of these.
enum class CpuSequence : std::uint8_t { None, Reset, Nmi, Irq };

// Processor-status masks. The chip holds six flags; bits 4 and 5 have no flag
// behind them and matter only in the byte a push writes (NESdev wiki, "Status
// flags", Flags and The B flag).
inline constexpr std::uint8_t kCpuFlagN = 0x80;  // negative: bit 7 of a result
inline constexpr std::uint8_t kCpuFlagV = 0x40;  // signed overflow
inline constexpr std::uint8_t kCpuFlagU = 0x20;  // bit 5: always pushed as 1
inline constexpr std::uint8_t kCpuFlagB = 0x10;  // bit 4: pushed as 1 by BRK and PHP, 0 by NMI and IRQ
// Decimal: held, pushed and pulled, and never honored by ADC or SBC, since the
// 2A03 lacks decimal mode (NESdev wiki, "CPU").
inline constexpr std::uint8_t kCpuFlagD = 0x08;
inline constexpr std::uint8_t kCpuFlagI = 0x04;  // interrupt disable
inline constexpr std::uint8_t kCpuFlagZ = 0x02;  // zero
inline constexpr std::uint8_t kCpuFlagC = 0x01;  // carry

// The whole CPU as a value: snapshot by copy, restore by assignment. `p` is a plain
// byte; the core reads and writes only N, V, D, I, Z and C in it and carries bits 4
// and 5 as found, so a byte a host restores round-trips.
struct Cpu6502State {
  std::uint16_t pc = 0;  // program counter
  std::uint8_t s = 0;    // stack pointer, an offset into page one
  std::uint8_t a = 0;    // accumulator
  std::uint8_t x = 0;    // X index register
  std::uint8_t y = 0;    // Y index register
  std::uint8_t p = 0;    // processor status

  // How far into an instruction the core is. These carry no architectural meaning;
  // they are the progress a running instruction has made, held as values so a
  // snapshot taken mid-instruction restores to the same cycle.
  std::uint8_t ir = 0;       // the opcode being executed
  std::uint8_t tcu = 0;      // cycle index within it; 0 means the next cycle fetches
  std::uint16_t ea = 0;      // effective-address latch
  std::uint8_t ptr = 0;      // pointer latch
  std::uint8_t data = 0;     // data latch
  bool pageCross = false;    // an index addition carried past a page

  // The input lines and what the core has latched from them.
  bool nmiLine = false;     // the NMI line's level, true while asserted
  bool nmiPending = false;  // latched by the NMI line becoming asserted; cleared when taken
  bool irqLine = false;     // the IRQ line's level, true while asserted
  bool rdyLine = true;      // the RDY line; false halts the core on its next read
  // The sequence the last interrupt poll asked for, taken at the next boundary.
  CpuSequence polled = CpuSequence::None;
  // The hardware sequence in progress, or pending at a boundary.
  CpuSequence servicing = CpuSequence::None;
  bool jammed = false;  // halted by a JAM opcode until reset

  [[nodiscard]] bool operator==(const Cpu6502State&) const noexcept = default;
};

// ---- the instruction set -----------------------------------------------------
// One table spells the whole instruction set: for each of the 256 opcodes, its
// mnemonic, its addressing mode and its access class (what the instruction does
// with the bus once its address is known), and whether it is unofficial or
// unstable. The cycle sequences select on the mode and the class; cycle counts fall
// out of the sequences.

enum class Mnemonic : std::uint8_t {
  Adc, Alr, Anc, And, Ane, Arr, Asl, Bcc, Bcs, Beq, Bit, Bmi, Bne, Bpl, Brk, Bvc, Bvs, Clc,
  Cld, Cli, Clv, Cmp, Cpx, Cpy, Dcp, Dec, Dex, Dey, Eor, Inc, Inx, Iny, Isc, Jam, Jmp, Jsr,
  Las, Lax, Lda, Ldx, Ldy, Lsr, Lxa, Nop, Ora, Pha, Php, Pla, Plp, Rla, Rol, Ror, Rra, Rti,
  Rts, Sax, Sbc, Sbx, Sec, Sed, Sei, Sha, Shx, Shy, Slo, Sre, Sta, Stx, Sty, Tas, Tax, Tay,
  Tsx, Txa, Txs, Tya,
};

enum class AddressingMode : std::uint8_t {
  Implied,
  Accumulator,
  Immediate,
  ZeroPage,
  ZeroPageX,
  ZeroPageY,
  Absolute,
  AbsoluteX,
  AbsoluteY,
  IndexedIndirect,  // (zp,X)
  IndirectIndexed,  // (zp),Y
  Relative,
  Indirect,  // JMP (abs)
};

enum class AccessClass : std::uint8_t {
  Implied,              // registers only; the accumulator shifts included
  Read,                 // loads an operand
  Write,                // stores a value
  ReadModifyWrite,      // reads, writes the value back, writes the result
  Branch,               // a relative branch
  Jump,                 // JMP
  Call,                 // JSR
  Return,               // RTS
  ReturnFromInterrupt,  // RTI
  Push,                 // PHA, PHP
  Pull,                 // PLA, PLP
  Break,                // BRK
  Jam,                  // halts the core until reset
};

struct Opcode {
  Mnemonic mnemonic;
  AddressingMode mode;
  AccessClass access;
  bool unofficial = false;  // outside the documented instruction set
  bool unstable = false;    // the result depends on analog behavior of the chip
};

// Spellings for the table's optional fields, so a row reads as what it marks.
inline constexpr bool kUnofficial = true;
inline constexpr bool kUnstable = true;

// The table row for an opcode.
[[nodiscard]] const Opcode& opcodeAt(std::uint8_t code) noexcept;

// The mnemonic in upper case: "ADC" through "TYA".
[[nodiscard]] std::string_view mnemonicName(Mnemonic mnemonic) noexcept;

// ---- power-on -----------------------------------------------------------------

// The state the chip holds at power-on, with the reset sequence pending: A, X and Y
// zero, S $00 before the sequence's three decrements take it to $FD, I set and every
// other flag clear (NESdev wiki, "CPU power up state", CPU). The two reads that open
// the sequence are at the program counter, which is $0000 here. The first seven
// cycles stepped are the reset sequence, which ends at the reset vector.
[[nodiscard]] constexpr Cpu6502State powerOn() noexcept {
  return Cpu6502State{.s = 0x00, .p = kCpuFlagI, .servicing = CpuSequence::Reset};
}

// ---- the core -------------------------------------------------------------------

class Cpu6502 {
 public:
  // A default-constructed core holds a default state and fetches from $0000 when
  // stepped. A host that wants a power-on builds from powerOn().
  Cpu6502() = default;
  explicit Cpu6502(Cpu6502State state) : state_(state) {}

  [[nodiscard]] const Cpu6502State& state() const noexcept { return state_; }
  void restore(Cpu6502State state) noexcept { state_ = state; }

  // Executes exactly one CPU cycle: one read or one write through the bus. While
  // the RDY line is low a read cycle is repeated as a Halted read of the same
  // address and the core makes no progress; a write cycle proceeds. A jammed core
  // repeats a discarded read of its latched address.
  template <CpuBus B>
  void stepCycle(B& bus);

  // Runs cycles until the next instruction boundary and returns how many it took.
  // Called mid-instruction it finishes the instruction in progress; at a boundary
  // with a sequence due it runs that sequence. With the RDY line low or the core
  // jammed no boundary is reachable, so it steps one cycle and returns 1.
  template <CpuBus B>
  std::uint32_t stepInstruction(B& bus);

  // Whether the core sits between instructions: the point at which an instruction
  // or a hardware sequence begins, and the state a completed one leaves behind.
  [[nodiscard]] bool atInstructionBoundary() const noexcept { return state_.tcu == 0; }

  // Makes the reset sequence pending: the next cycle begins it, abandoning any
  // instruction in progress, and a jammed core is released. A, X, Y and every flag
  // but I keep their values; the sequence takes S down by three and sets I (NESdev
  // wiki, "CPU power up state", CPU; 64doc, "6510 features"). The lines and the
  // latched NMI are left as they stand.
  void reset() noexcept {
    state_.servicing = CpuSequence::Reset;
    state_.tcu = 0;
    state_.polled = CpuSequence::None;
    state_.jammed = false;
  }

  // The input lines, driven by the machine; `asserted` is true while the active-low
  // pin is held low. NMI is edge-sensitive: the line becoming asserted latches a
  // request that stays pending until it is taken. IRQ is level-sensitive: the core
  // samples the level, so a source holds it until acknowledged (NESdev wiki, "CPU
  // interrupts", Detailed interrupt behavior). RDY low halts the core on its next
  // read cycle (NESdev wiki, "DMA", Behavior).
  void setNmiLine(bool asserted) noexcept {
    if (asserted && !state_.nmiLine) state_.nmiPending = true;
    state_.nmiLine = asserted;
  }
  void setIrqLine(bool asserted) noexcept { state_.irqLine = asserted; }
  void setRdyLine(bool asserted) noexcept { state_.rdyLine = asserted; }

 private:
  // ---- flags --------------------------------------------------------------
  void setNZ(std::uint8_t v) noexcept {
    state_.p = static_cast<std::uint8_t>((state_.p & ~(kCpuFlagN | kCpuFlagZ)) |
                                         (v & kCpuFlagN) | (v == 0 ? kCpuFlagZ : 0));
  }
  void setCarry(bool carry) noexcept {
    state_.p = static_cast<std::uint8_t>(carry ? (state_.p | kCpuFlagC)
                                               : (state_.p & ~kCpuFlagC));
  }
  void setOverflow(bool v) noexcept {
    state_.p = static_cast<std::uint8_t>(v ? (state_.p | kCpuFlagV)
                                           : (state_.p & ~kCpuFlagV));
  }
  void setN(bool n) noexcept {
    state_.p = static_cast<std::uint8_t>(n ? (state_.p | kCpuFlagN)
                                           : (state_.p & ~kCpuFlagN));
  }
  void setZ(bool z) noexcept {
    state_.p = static_cast<std::uint8_t>(z ? (state_.p | kCpuFlagZ)
                                           : (state_.p & ~kCpuFlagZ));
  }

  // ---- arithmetic and logic -----------------------------------------------
  // ADC and SBC share one binary adder: SBC adds the ones-complement of the operand
  // with the same carry in (A - M - !C is A + ~M + C), so the borrow, the carry and
  // the signed overflow fall out of the same addition. The D flag is ignored; the
  // 2A03 has no decimal mode (NESdev wiki, "CPU").
  void addWithCarry(std::uint8_t operand, bool subtract) noexcept {
    const unsigned a = state_.a;
    const unsigned m = subtract ? (operand ^ 0xFFu) : unsigned{operand};
    const unsigned sum = a + m + ((state_.p & kCpuFlagC) ? 1u : 0u);
    const auto result = static_cast<std::uint8_t>(sum);
    setOverflow(((~(a ^ m) & (a ^ unsigned{result})) & 0x80u) != 0);
    setCarry(sum > 0xFFu);
    state_.a = result;
    setNZ(result);
  }

  // CMP, CPX and CPY subtract the operand from a register without carry in and
  // without storing: N and Z from the difference, C set when the register is at or
  // above the operand.
  void compareWith(std::uint8_t reg, std::uint8_t operand) noexcept {
    setNZ(static_cast<std::uint8_t>(reg - operand));
    setCarry(reg >= operand);
  }

  // The bitwise operators combine the operand into the accumulator and set N and Z
  // from the result.
  void andOp(std::uint8_t operand) noexcept {
    state_.a = static_cast<std::uint8_t>(state_.a & operand);
    setNZ(state_.a);
  }
  void oraOp(std::uint8_t operand) noexcept {
    state_.a = static_cast<std::uint8_t>(state_.a | operand);
    setNZ(state_.a);
  }
  void eorOp(std::uint8_t operand) noexcept {
    state_.a = static_cast<std::uint8_t>(state_.a ^ operand);
    setNZ(state_.a);
  }

  // BIT sets Z from the AND of the accumulator and the operand, and N and V from
  // bits 7 and 6 of the operand itself. The accumulator is unchanged.
  void bitOp(std::uint8_t operand) noexcept {
    setZ((state_.a & operand) == 0);
    setN((operand & 0x80u) != 0);
    setOverflow((operand & 0x40u) != 0);
  }

  // ---- shifts, rotates, increments ----------------------------------------
  // Each returns the modified value and sets the flags it owns; the caller writes
  // the result back, to the accumulator or to memory. N and Z come from the result;
  // the shifts and rotates also move the carry.
  [[nodiscard]] std::uint8_t aslOp(std::uint8_t v) noexcept {
    setCarry((v & 0x80u) != 0);
    const auto r = static_cast<std::uint8_t>(v << 1);
    setNZ(r);
    return r;
  }
  // LSR shifts a zero into bit 7, so it always clears N.
  [[nodiscard]] std::uint8_t lsrOp(std::uint8_t v) noexcept {
    setCarry((v & 0x01u) != 0);
    const auto r = static_cast<std::uint8_t>(v >> 1);
    setNZ(r);
    return r;
  }
  // ROL and ROR rotate through the carry: the old carry fills the vacated bit and
  // the bit shifted out becomes the new carry.
  [[nodiscard]] std::uint8_t rolOp(std::uint8_t v) noexcept {
    const unsigned carryIn = (state_.p & kCpuFlagC) ? 1u : 0u;
    setCarry((v & 0x80u) != 0);
    const auto r = static_cast<std::uint8_t>((unsigned{v} << 1) | carryIn);
    setNZ(r);
    return r;
  }
  [[nodiscard]] std::uint8_t rorOp(std::uint8_t v) noexcept {
    const unsigned carryIn = (state_.p & kCpuFlagC) ? 0x80u : 0u;
    setCarry((v & 0x01u) != 0);
    const auto r = static_cast<std::uint8_t>((unsigned{v} >> 1) | carryIn);
    setNZ(r);
    return r;
  }
  // INC and DEC step a value by one; they move only N and Z.
  [[nodiscard]] std::uint8_t incOp(std::uint8_t v) noexcept {
    const auto r = static_cast<std::uint8_t>(v + 1);
    setNZ(r);
    return r;
  }
  [[nodiscard]] std::uint8_t decOp(std::uint8_t v) noexcept {
    const auto r = static_cast<std::uint8_t>(v - 1);
    setNZ(r);
    return r;
  }

  // Applies an instruction of the implied class on its second cycle: the
  // transfers, the index steps, the accumulator shifts, the flag instructions and
  // NOP (64doc, "Accumulator or implied addressing").
  void applyImplied(Mnemonic mnemonic) noexcept;

  // Applies a read-class operation to the byte its last cycle read: the loads, the
  // bitwise operators, ADC and SBC, the compares, BIT and NOP.
  void applyRead(Mnemonic mnemonic, std::uint8_t value) noexcept;

  // Answers the result a read-modify-write operation writes for the byte it read,
  // setting the flags the operation owns.
  [[nodiscard]] std::uint8_t modify(Mnemonic mnemonic, std::uint8_t value) noexcept;

  // The byte a write-class instruction stores.
  [[nodiscard]] std::uint8_t storeValue(Mnemonic mnemonic) const noexcept;

  // Whether a branch's condition holds in the current flags.
  [[nodiscard]] bool branchTaken(Mnemonic mnemonic) const noexcept;

  // The index register an indexed mode adds: Y for the zero page and absolute Y
  // forms, X for the others.
  [[nodiscard]] std::uint8_t indexFor(AddressingMode mode) const noexcept {
    return (mode == AddressingMode::ZeroPageY || mode == AddressingMode::AbsoluteY) ? state_.y
                                                                                     : state_.x;
  }

  // ---- reaching the bus ---------------------------------------------------
  // Every read and write the core makes passes through these two. A read while the
  // RDY line is low is issued as a Halted read of the same address and marks the
  // cycle halted; stepCycle then undoes everything else the cycle did.
  template <CpuBus B>
  std::uint8_t busRead(B& bus, std::uint16_t address, CycleKind kind) {
    if (!state_.rdyLine) {
      halted_ = true;
      return bus.read(address, CycleKind::Halted);
    }
    return bus.read(address, kind);
  }
  template <CpuBus B>
  void busWrite(B& bus, std::uint16_t address, std::uint8_t value, CycleKind kind) {
    bus.write(address, value, kind);
  }

  // ---- stack --------------------------------------------------------------
  // The stack lives in page one and the pointer wraps inside it. A push writes at
  // the pointer and then steps it down; a pull steps it up and then reads.
  template <CpuBus B>
  void pushByte(B& bus, std::uint8_t value) {
    busWrite(bus, stackAddress(), value, CycleKind::StackPush);
    state_.s = static_cast<std::uint8_t>(state_.s - 1);
  }
  template <CpuBus B>
  std::uint8_t pullByte(B& bus) {
    state_.s = static_cast<std::uint8_t>(state_.s + 1);
    return busRead(bus, stackAddress(), CycleKind::StackPull);
  }
  [[nodiscard]] std::uint16_t stackAddress() const noexcept {
    return static_cast<std::uint16_t>(0x0100u | state_.s);
  }

  // ---- the cycle engine ---------------------------------------------------
  // Samples the interrupt inputs into `polled`: a latched NMI first, then an
  // asserted IRQ while I is clear (NESdev wiki, "CPU interrupts", Detailed
  // interrupt behavior). It runs at the start of every cycle of an instruction and
  // never during a hardware sequence, so the poll that decides an instruction's end
  // is the one at the start of its last cycle: the lines as they stood at the end
  // of the cycle before it.
  void poll() noexcept {
    if (state_.nmiPending) {
      state_.polled = CpuSequence::Nmi;
    } else if (state_.irqLine && (state_.p & kCpuFlagI) == 0) {
      state_.polled = CpuSequence::Irq;
    } else {
      state_.polled = CpuSequence::None;
    }
  }

  // One cycle with RDY already accounted for: a jammed cycle, the first cycle at a
  // boundary, or the next cycle of the instruction or sequence in progress.
  template <CpuBus B>
  void runCycle(B& bus);

  // Executes the cycle `tcu` names of the instruction in the instruction register
  // and answers whether that cycle was its last.
  template <CpuBus B>
  bool executeCycle(B& bus);

  // Executes the cycle `tcu` names of the seven-cycle sequence shared by reset,
  // NMI, IRQ and BRK, and answers whether it was the last.
  template <CpuBus B>
  bool executeSequenceCycle(B& bus);

  // ---- the instruction sequences ------------------------------------------
  // One function per addressing mode and per control-flow class. Each executes the
  // cycle `tcu` names, from the second cycle on, and answers whether it was the
  // instruction's last.

  // Reads the byte at the program counter as an operand and steps past it.
  template <CpuBus B>
  std::uint8_t fetchOperand(B& bus);

  // The read, write and read-modify-write classes, by addressing mode.
  template <CpuBus B>
  bool executeAddressedCycle(B& bus, const Opcode& opcode);
  template <CpuBus B>
  bool executeImmediateCycle(B& bus, const Opcode& opcode);
  template <CpuBus B>
  bool executeZeroPageCycle(B& bus, const Opcode& opcode);
  template <CpuBus B>
  bool executeZeroPageIndexedCycle(B& bus, const Opcode& opcode);
  template <CpuBus B>
  bool executeAbsoluteCycle(B& bus, const Opcode& opcode);
  template <CpuBus B>
  bool executeAbsoluteIndexedCycle(B& bus, const Opcode& opcode);
  template <CpuBus B>
  bool executeIndexedIndirectCycle(B& bus, const Opcode& opcode);
  template <CpuBus B>
  bool executeIndirectIndexedCycle(B& bus, const Opcode& opcode);

  // The cycle after an index addition into `ea`, in the absolute indexed and
  // indirect indexed modes.
  template <CpuBus B>
  bool pageFixCycle(B& bus, const Opcode& opcode);

  // The cycles at the effective address once it is known; `step` counts them
  // from 0.
  template <CpuBus B>
  bool addressedTailCycle(B& bus, const Opcode& opcode, int step);
  template <CpuBus B>
  bool finishRead(B& bus, const Opcode& opcode);
  template <CpuBus B>
  bool finishWrite(B& bus, const Opcode& opcode);
  template <CpuBus B>
  bool rmwTailCycle(B& bus, const Opcode& opcode, int step);

  // The control-flow classes.
  template <CpuBus B>
  bool executeBranchCycle(B& bus, const Opcode& opcode);
  template <CpuBus B>
  bool executeJumpCycle(B& bus, const Opcode& opcode);
  template <CpuBus B>
  bool executeCallCycle(B& bus);
  template <CpuBus B>
  bool executeReturnCycle(B& bus);
  template <CpuBus B>
  bool executeReturnFromInterruptCycle(B& bus);
  template <CpuBus B>
  bool executePushCycle(B& bus, const Opcode& opcode);
  template <CpuBus B>
  bool executePullCycle(B& bus, const Opcode& opcode);

  // Loads a status byte pulled by PLP or RTI. Bits 4 and 5 keep the values they
  // had (NESdev wiki, "Instruction reference", PLP and RTI).
  void pullStatus(std::uint8_t pulled) noexcept {
    constexpr std::uint8_t kKept = kCpuFlagU | kCpuFlagB;
    state_.p = static_cast<std::uint8_t>((pulled & ~kKept) | (state_.p & kKept));
  }

  // Where the running sequence reads its vector (NESdev wiki, "CPU interrupts",
  // Interrupt hijacking).
  [[nodiscard]] std::uint16_t vectorAddress() const noexcept {
    switch (state_.servicing) {
      case CpuSequence::Reset: return 0xFFFC;
      case CpuSequence::Nmi: return 0xFFFA;
      case CpuSequence::Irq:
      case CpuSequence::None: break;
    }
    return 0xFFFE;  // IRQ and BRK
  }

  Cpu6502State state_{};
  // Whether the read of the cycle under way was halted by RDY. Meaningful only
  // inside one stepCycle call; nothing between cycles reads it.
  bool halted_ = false;
};

// ---- the cycle engine, defined ------------------------------------------------

template <CpuBus B>
void Cpu6502::stepCycle(B& bus) {
  if (state_.rdyLine) {
    runCycle(bus);
    return;
  }
  const Cpu6502State before = state_;
  halted_ = false;
  runCycle(bus);
  // A write cycle proceeds regardless of RDY (NESdev wiki, "DMA", Behavior; 64doc,
  // "6510 features": -RDY is ignored during writes).
  if (!halted_) return;
  // A read cycle under RDY low did not happen: everything the cycle changed is put
  // back, except the line inputs, which the bus may have driven during the call.
  // The next cycle repeats the same read (NESdev wiki, "DMA", Behavior).
  const bool nmiLine = state_.nmiLine;
  const bool nmiPending = state_.nmiPending;
  const bool irqLine = state_.irqLine;
  const bool rdyLine = state_.rdyLine;
  state_ = before;
  state_.nmiLine = nmiLine;
  state_.nmiPending = nmiPending;
  state_.irqLine = irqLine;
  state_.rdyLine = rdyLine;
}

template <CpuBus B>
std::uint32_t Cpu6502::stepInstruction(B& bus) {
  if (!state_.rdyLine || state_.jammed) {
    stepCycle(bus);
    return 1;
  }
  std::uint32_t cycles = 0;
  if (atInstructionBoundary()) {
    stepCycle(bus);  // the opcode fetch, or the first cycle of a sequence
    cycles = 1;
  }
  while (!atInstructionBoundary()) {
    stepCycle(bus);
    ++cycles;
  }
  return cycles;
}

template <CpuBus B>
void Cpu6502::runCycle(B& bus) {
  // A jammed core makes no progress; it repeats a discarded read of the address it
  // latched until reset.
  if (state_.jammed) {
    busRead(bus, state_.ea, CycleKind::DummyRead);
    return;
  }

  if (atInstructionBoundary()) {
    // A pending reset runs first; then a sequence the last poll asked for; then the
    // next instruction.
    if (state_.servicing == CpuSequence::None && state_.polled != CpuSequence::None) {
      state_.servicing = state_.polled;
      state_.polled = CpuSequence::None;
      if (state_.servicing == CpuSequence::Nmi) state_.nmiPending = false;
    }
    if (state_.servicing != CpuSequence::None) {
      executeSequenceCycle(bus);
      state_.tcu = 1;
      return;
    }
    poll();
    state_.ir = busRead(bus, state_.pc, CycleKind::OpcodeFetch);
    state_.pc = static_cast<std::uint16_t>(state_.pc + 1);
    state_.tcu = 1;
    return;
  }

  bool last;
  if (state_.servicing != CpuSequence::None) {
    last = executeSequenceCycle(bus);
  } else {
    poll();
    last = executeCycle(bus);
  }
  state_.tcu = last ? std::uint8_t{0} : static_cast<std::uint8_t>(state_.tcu + 1);
}

template <CpuBus B>
bool Cpu6502::executeCycle(B& bus) {
  const Opcode& opcode = opcodeAt(state_.ir);
  switch (opcode.access) {
    case AccessClass::Implied:
      // 64doc, "Accumulator or implied addressing": the second cycle reads the byte
      // after the opcode and discards it; the program counter stays.
      busRead(bus, state_.pc, CycleKind::DummyRead);
      applyImplied(opcode.mnemonic);
      return true;
    case AccessClass::Read:
    case AccessClass::Write:
    case AccessClass::ReadModifyWrite:
      return executeAddressedCycle(bus, opcode);
    case AccessClass::Branch:
      return executeBranchCycle(bus, opcode);
    case AccessClass::Jump:
      return executeJumpCycle(bus, opcode);
    case AccessClass::Call:
      return executeCallCycle(bus);
    case AccessClass::Return:
      return executeReturnCycle(bus);
    case AccessClass::ReturnFromInterrupt:
      return executeReturnFromInterruptCycle(bus);
    case AccessClass::Push:
      return executePushCycle(bus, opcode);
    case AccessClass::Pull:
      return executePullCycle(bus, opcode);
    case AccessClass::Break:
      return executeSequenceCycle(bus);
    case AccessClass::Jam:
      // An opcode whose sequence is not in this file runs the two-cycle implied
      // shape and changes no register.
      busRead(bus, state_.pc, CycleKind::DummyRead);
      return true;
  }
  return true;
}

// The sequence shared by reset, NMI, IRQ and BRK. Cycle 1 is the boundary cycle and
// cycles 2 to 7 run here with `tcu` 1 to 6. BRK's first cycle is its opcode fetch;
// its second reads the byte after it; both advance the program counter. A hardware
// sequence reads the program counter twice without advancing it, so the address
// saved is the instruction it interrupted. The three stack cycles push the return
// address and the status, with B set for BRK and clear for NMI and IRQ; a reset
// makes them reads, taking S down by three with memory untouched. The vector's two
// bytes come last, and I is set with the first of them (NESdev wiki, "CPU
// interrupts", Interrupt hijacking and IRQ and NMI tick-by-tick execution; NESdev
// wiki, "CPU power up state", References; 64doc, "Instructions accessing the stack",
// BRK; NESdev wiki, "Status flags", The B flag).
template <CpuBus B>
bool Cpu6502::executeSequenceCycle(B& bus) {
  const bool isBreak = state_.servicing == CpuSequence::None;
  const bool isReset = state_.servicing == CpuSequence::Reset;
  switch (state_.tcu) {
    case 0:  // a hardware sequence's first cycle
      busRead(bus, state_.pc, CycleKind::DummyRead);
      return false;
    case 1:
      busRead(bus, state_.pc, CycleKind::DummyRead);
      if (isBreak) state_.pc = static_cast<std::uint16_t>(state_.pc + 1);
      return false;
    case 2:
    case 3: {
      const auto byte = static_cast<std::uint8_t>(state_.tcu == 2 ? state_.pc >> 8 : state_.pc);
      if (isReset) {
        busRead(bus, stackAddress(), CycleKind::DummyRead);
        state_.s = static_cast<std::uint8_t>(state_.s - 1);
      } else {
        pushByte(bus, byte);
      }
      return false;
    }
    case 4:
      if (isReset) {
        busRead(bus, stackAddress(), CycleKind::DummyRead);
        state_.s = static_cast<std::uint8_t>(state_.s - 1);
      } else if (isBreak) {
        pushByte(bus, static_cast<std::uint8_t>(state_.p | kCpuFlagU | kCpuFlagB));
      } else {
        pushByte(bus, static_cast<std::uint8_t>((state_.p | kCpuFlagU) & ~kCpuFlagB));
      }
      return false;
    case 5:
      state_.data = busRead(bus, vectorAddress(), CycleKind::VectorRead);
      state_.p = static_cast<std::uint8_t>(state_.p | kCpuFlagI);
      return false;
    default: {
      const std::uint8_t high =
          busRead(bus, static_cast<std::uint16_t>(vectorAddress() + 1), CycleKind::VectorRead);
      state_.pc = static_cast<std::uint16_t>(state_.data | (high << 8));
      state_.servicing = CpuSequence::None;
      state_.polled = CpuSequence::None;
      return true;
    }
  }
}

// ---- the instruction sequences, defined ---------------------------------------

template <CpuBus B>
std::uint8_t Cpu6502::fetchOperand(B& bus) {
  const std::uint8_t value = busRead(bus, state_.pc, CycleKind::OperandFetch);
  state_.pc = static_cast<std::uint16_t>(state_.pc + 1);
  return value;
}

template <CpuBus B>
bool Cpu6502::executeAddressedCycle(B& bus, const Opcode& opcode) {
  switch (opcode.mode) {
    case AddressingMode::Immediate:
      return executeImmediateCycle(bus, opcode);
    case AddressingMode::ZeroPage:
      return executeZeroPageCycle(bus, opcode);
    case AddressingMode::ZeroPageX:
    case AddressingMode::ZeroPageY:
      return executeZeroPageIndexedCycle(bus, opcode);
    case AddressingMode::Absolute:
      return executeAbsoluteCycle(bus, opcode);
    case AddressingMode::AbsoluteX:
    case AddressingMode::AbsoluteY:
      return executeAbsoluteIndexedCycle(bus, opcode);
    case AddressingMode::IndexedIndirect:
      return executeIndexedIndirectCycle(bus, opcode);
    case AddressingMode::IndirectIndexed:
      return executeIndirectIndexedCycle(bus, opcode);
    case AddressingMode::Implied:
    case AddressingMode::Accumulator:
    case AddressingMode::Relative:
    case AddressingMode::Indirect:
      // The opcode table pairs the read, write and read-modify-write classes only
      // with the modes above, which the table's definition checks at compile time.
      break;
  }
  busRead(bus, state_.pc, CycleKind::DummyRead);
  return true;
}

// 64doc, "Immediate addressing".
template <CpuBus B>
bool Cpu6502::executeImmediateCycle(B& bus, const Opcode& opcode) {
  applyRead(opcode.mnemonic, fetchOperand(bus));
  return true;
}

// 64doc, "Zero page addressing".
template <CpuBus B>
bool Cpu6502::executeZeroPageCycle(B& bus, const Opcode& opcode) {
  if (state_.tcu == 1) {
    state_.ea = fetchOperand(bus);
    return false;
  }
  return addressedTailCycle(bus, opcode, state_.tcu - 2);
}

// 64doc, "Zero page indexed addressing".
template <CpuBus B>
bool Cpu6502::executeZeroPageIndexedCycle(B& bus, const Opcode& opcode) {
  switch (state_.tcu) {
    case 1:
      state_.ea = fetchOperand(bus);
      return false;
    case 2:
      busRead(bus, state_.ea, CycleKind::DummyRead);
      state_.ea = static_cast<std::uint8_t>(state_.ea + indexFor(opcode.mode));
      return false;
    default:
      return addressedTailCycle(bus, opcode, state_.tcu - 3);
  }
}

// 64doc, "Absolute addressing".
template <CpuBus B>
bool Cpu6502::executeAbsoluteCycle(B& bus, const Opcode& opcode) {
  switch (state_.tcu) {
    case 1:
      state_.ea = fetchOperand(bus);
      return false;
    case 2:
      state_.ea = static_cast<std::uint16_t>(state_.ea | (fetchOperand(bus) << 8));
      return false;
    default:
      return addressedTailCycle(bus, opcode, state_.tcu - 3);
  }
}

// 64doc, "Absolute indexed addressing".
template <CpuBus B>
bool Cpu6502::executeAbsoluteIndexedCycle(B& bus, const Opcode& opcode) {
  switch (state_.tcu) {
    case 1:
      state_.ea = fetchOperand(bus);
      return false;
    case 2: {
      const unsigned low = state_.ea + unsigned{indexFor(opcode.mode)};
      state_.ea = static_cast<std::uint16_t>((fetchOperand(bus) << 8) | (low & 0xFFu));
      state_.pageCross = low > 0xFFu;
      return false;
    }
    case 3:
      return pageFixCycle(bus, opcode);
    default:
      return addressedTailCycle(bus, opcode, state_.tcu - 4);
  }
}

// 64doc, "Indexed indirect addressing".
template <CpuBus B>
bool Cpu6502::executeIndexedIndirectCycle(B& bus, const Opcode& opcode) {
  switch (state_.tcu) {
    case 1:
      state_.ptr = fetchOperand(bus);
      return false;
    case 2:
      busRead(bus, state_.ptr, CycleKind::DummyRead);
      state_.ptr = static_cast<std::uint8_t>(state_.ptr + state_.x);
      return false;
    case 3:
      state_.ea = busRead(bus, state_.ptr, CycleKind::DataRead);
      return false;
    case 4: {
      const auto high = busRead(bus, static_cast<std::uint8_t>(state_.ptr + 1), CycleKind::DataRead);
      state_.ea = static_cast<std::uint16_t>(state_.ea | (high << 8));
      return false;
    }
    default:
      return addressedTailCycle(bus, opcode, state_.tcu - 5);
  }
}

// 64doc, "Indirect indexed addressing".
template <CpuBus B>
bool Cpu6502::executeIndirectIndexedCycle(B& bus, const Opcode& opcode) {
  switch (state_.tcu) {
    case 1:
      state_.ptr = fetchOperand(bus);
      return false;
    case 2:
      state_.ea = busRead(bus, state_.ptr, CycleKind::DataRead);
      return false;
    case 3: {
      const auto high = busRead(bus, static_cast<std::uint8_t>(state_.ptr + 1), CycleKind::DataRead);
      const unsigned low = state_.ea + unsigned{state_.y};
      state_.ea = static_cast<std::uint16_t>((high << 8) | (low & 0xFFu));
      state_.pageCross = low > 0xFFu;
      return false;
    }
    case 4:
      return pageFixCycle(bus, opcode);
    default:
      return addressedTailCycle(bus, opcode, state_.tcu - 5);
  }
}

// The index was added to the low byte alone, so `ea` is in the base address's
// page. A read that stayed in that page reads its operand here and ends. Otherwise
// the cycle reads `ea` and discards it, and a carry moves `ea` to the next page:
// across a page the read lands one page low, and a write or read-modify-write
// makes this read whether or not the page changed (64doc, "Absolute indexed
// addressing" and "Indirect indexed addressing"; 64doc, "6510 features").
template <CpuBus B>
bool Cpu6502::pageFixCycle(B& bus, const Opcode& opcode) {
  if (opcode.access == AccessClass::Read && !state_.pageCross) return finishRead(bus, opcode);
  busRead(bus, state_.ea, CycleKind::DummyRead);
  if (state_.pageCross) state_.ea = static_cast<std::uint16_t>(state_.ea + 0x100);
  return false;
}

template <CpuBus B>
bool Cpu6502::addressedTailCycle(B& bus, const Opcode& opcode, int step) {
  switch (opcode.access) {
    case AccessClass::Read:
      return finishRead(bus, opcode);
    case AccessClass::Write:
      return finishWrite(bus, opcode);
    default:
      return rmwTailCycle(bus, opcode, step);
  }
}

// A read-class instruction's last cycle reads its operand at `ea` and applies the
// operation.
template <CpuBus B>
bool Cpu6502::finishRead(B& bus, const Opcode& opcode) {
  applyRead(opcode.mnemonic, busRead(bus, state_.ea, CycleKind::DataRead));
  return true;
}

// A write-class instruction's last cycle stores its value at `ea`.
template <CpuBus B>
bool Cpu6502::finishWrite(B& bus, const Opcode& opcode) {
  busWrite(bus, state_.ea, storeValue(opcode.mnemonic), CycleKind::DataWrite);
  return true;
}

// A read-modify-write instruction reads `ea`, writes the value back unmodified
// while the operation runs, then writes the result (64doc, "6510 features").
template <CpuBus B>
bool Cpu6502::rmwTailCycle(B& bus, const Opcode& opcode, int step) {
  switch (step) {
    case 0:
      state_.data = busRead(bus, state_.ea, CycleKind::RmwRead);
      return false;
    case 1:
      busWrite(bus, state_.ea, state_.data, CycleKind::RmwDummyWrite);
      state_.data = modify(opcode.mnemonic, state_.data);
      return false;
    default:
      busWrite(bus, state_.ea, state_.data, CycleKind::RmwWrite);
      return true;
  }
}

// 64doc, "Relative addressing".
template <CpuBus B>
bool Cpu6502::executeBranchCycle(B& bus, const Opcode& opcode) {
  switch (state_.tcu) {
    case 1:
      state_.data = fetchOperand(bus);
      return !branchTaken(opcode.mnemonic);
    case 2: {
      busRead(bus, state_.pc, CycleKind::DummyRead);
      state_.ea = static_cast<std::uint16_t>(state_.pc + static_cast<std::int8_t>(state_.data));
      state_.pageCross = ((state_.ea ^ state_.pc) & 0xFF00u) != 0;
      state_.pc = static_cast<std::uint16_t>((state_.pc & 0xFF00u) | (state_.ea & 0x00FFu));
      return !state_.pageCross;
    }
    default:
      // A branch across a page reads the target's low byte in the old page, then
      // takes the target.
      busRead(bus, state_.pc, CycleKind::DummyRead);
      state_.pc = state_.ea;
      return true;
  }
}

// JMP absolute (64doc, "Absolute addressing", JMP) and JMP indirect (64doc,
// "Absolute indirect addressing (JMP)"). The indirect form reads the pointer's
// second byte from the same page as its first (64doc, "6510 features").
template <CpuBus B>
bool Cpu6502::executeJumpCycle(B& bus, const Opcode& opcode) {
  if (opcode.mode != AddressingMode::Indirect) {
    if (state_.tcu == 1) {
      state_.ea = fetchOperand(bus);
      return false;
    }
    const auto high = busRead(bus, state_.pc, CycleKind::OperandFetch);
    state_.pc = static_cast<std::uint16_t>(state_.ea | (high << 8));
    return true;
  }
  switch (state_.tcu) {
    case 1:
      state_.ea = fetchOperand(bus);
      return false;
    case 2:
      state_.ea = static_cast<std::uint16_t>(state_.ea | (fetchOperand(bus) << 8));
      return false;
    case 3:
      state_.data = busRead(bus, state_.ea, CycleKind::DataRead);
      return false;
    default: {
      const auto next = static_cast<std::uint16_t>((state_.ea & 0xFF00u) |
                                                   ((state_.ea + 1u) & 0x00FFu));
      const auto high = busRead(bus, next, CycleKind::DataRead);
      state_.pc = static_cast<std::uint16_t>(state_.data | (high << 8));
      return true;
    }
  }
}

// JSR pushes the address of its own last byte: the program counter after the low
// byte is fetched, before the high byte is (64doc, "Instructions accessing the
// stack", JSR).
template <CpuBus B>
bool Cpu6502::executeCallCycle(B& bus) {
  switch (state_.tcu) {
    case 1:
      state_.data = fetchOperand(bus);
      return false;
    case 2:
      busRead(bus, stackAddress(), CycleKind::DummyRead);
      return false;
    case 3:
      pushByte(bus, static_cast<std::uint8_t>(state_.pc >> 8));
      return false;
    case 4:
      pushByte(bus, static_cast<std::uint8_t>(state_.pc));
      return false;
    default: {
      const auto high = busRead(bus, state_.pc, CycleKind::OperandFetch);
      state_.pc = static_cast<std::uint16_t>(state_.data | (high << 8));
      return true;
    }
  }
}

// RTS pulls the address JSR pushed and steps past it on its last cycle (64doc,
// "Instructions accessing the stack", RTS).
template <CpuBus B>
bool Cpu6502::executeReturnCycle(B& bus) {
  switch (state_.tcu) {
    case 1:
      busRead(bus, state_.pc, CycleKind::DummyRead);
      return false;
    case 2:
      busRead(bus, stackAddress(), CycleKind::DummyRead);
      return false;
    case 3:
      state_.data = pullByte(bus);
      return false;
    case 4: {
      const auto high = pullByte(bus);
      state_.pc = static_cast<std::uint16_t>(state_.data | (high << 8));
      return false;
    }
    default:
      busRead(bus, state_.pc, CycleKind::DummyRead);
      state_.pc = static_cast<std::uint16_t>(state_.pc + 1);
      return true;
  }
}

// RTI pulls the status, then the program counter, and resumes at that address
// (64doc, "Instructions accessing the stack", RTI).
template <CpuBus B>
bool Cpu6502::executeReturnFromInterruptCycle(B& bus) {
  switch (state_.tcu) {
    case 1:
      busRead(bus, state_.pc, CycleKind::DummyRead);
      return false;
    case 2:
      busRead(bus, stackAddress(), CycleKind::DummyRead);
      return false;
    case 3:
      pullStatus(pullByte(bus));
      return false;
    case 4:
      state_.data = pullByte(bus);
      return false;
    default: {
      const auto high = pullByte(bus);
      state_.pc = static_cast<std::uint16_t>(state_.data | (high << 8));
      return true;
    }
  }
}

// PHA pushes A. PHP pushes P with bits 4 and 5 set (64doc, "Instructions accessing
// the stack", PHA, PHP; NESdev wiki, "Status flags", The B flag).
template <CpuBus B>
bool Cpu6502::executePushCycle(B& bus, const Opcode& opcode) {
  if (state_.tcu == 1) {
    busRead(bus, state_.pc, CycleKind::DummyRead);
    return false;
  }
  pushByte(bus, opcode.mnemonic == Mnemonic::Pha
                    ? state_.a
                    : static_cast<std::uint8_t>(state_.p | kCpuFlagU | kCpuFlagB));
  return true;
}

// PLA loads A and sets N and Z from it. PLP loads P, bits 4 and 5 excepted (64doc,
// "Instructions accessing the stack", PLA, PLP).
template <CpuBus B>
bool Cpu6502::executePullCycle(B& bus, const Opcode& opcode) {
  switch (state_.tcu) {
    case 1:
      busRead(bus, state_.pc, CycleKind::DummyRead);
      return false;
    case 2:
      busRead(bus, stackAddress(), CycleKind::DummyRead);
      return false;
    default: {
      const std::uint8_t value = pullByte(bus);
      if (opcode.mnemonic == Mnemonic::Pla) {
        state_.a = value;
        setNZ(state_.a);
      } else {
        pullStatus(value);
      }
      return true;
    }
  }
}

}  // namespace nescore
