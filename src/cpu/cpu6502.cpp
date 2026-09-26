#include "nescore/cpu/cpu6502.h"

#include <array>
#include <cstddef>

namespace nescore {

namespace {

// Every opcode's mnemonic, addressing mode and access class, from the opcode matrix
// in 64doc ("6510 Instructions by Addressing Modes") checked against the NESdev
// wiki's matrix ("CPU unofficial opcodes"). The unofficial names follow 64doc except
// ISC, ALR and TAS, which follow the wiki. `unstable` marks ANE and LXA, whose result
// includes a chip-dependent constant, and SHA, SHX, SHY and TAS, whose stored value
// depends on a page cross (64doc, "6510/8502 Undocumented Commands").
// Other names in use: ISB for ISC, ASR for ALR, SHS for TAS, XAA for ANE, AXS for
// SBX, AHX for SHA, STP for JAM, LAX #imm for LXA.
constexpr std::array<Opcode, 256> kOpcodeTable{{
    // $00
    {Mnemonic::Brk, AddressingMode::Implied, AccessClass::Break},                           // $00
    {Mnemonic::Ora, AddressingMode::IndexedIndirect, AccessClass::Read},                    // $01
    {Mnemonic::Jam, AddressingMode::Implied, AccessClass::Jam, kUnofficial},                // $02
    {Mnemonic::Slo, AddressingMode::IndexedIndirect, AccessClass::ReadModifyWrite, kUnofficial},// $03
    {Mnemonic::Nop, AddressingMode::ZeroPage, AccessClass::Read, kUnofficial},              // $04
    {Mnemonic::Ora, AddressingMode::ZeroPage, AccessClass::Read},                           // $05
    {Mnemonic::Asl, AddressingMode::ZeroPage, AccessClass::ReadModifyWrite},                // $06
    {Mnemonic::Slo, AddressingMode::ZeroPage, AccessClass::ReadModifyWrite, kUnofficial},   // $07
    {Mnemonic::Php, AddressingMode::Implied, AccessClass::Push},                            // $08
    {Mnemonic::Ora, AddressingMode::Immediate, AccessClass::Read},                          // $09
    {Mnemonic::Asl, AddressingMode::Accumulator, AccessClass::Implied},                     // $0A
    {Mnemonic::Anc, AddressingMode::Immediate, AccessClass::Read, kUnofficial},             // $0B
    {Mnemonic::Nop, AddressingMode::Absolute, AccessClass::Read, kUnofficial},              // $0C
    {Mnemonic::Ora, AddressingMode::Absolute, AccessClass::Read},                           // $0D
    {Mnemonic::Asl, AddressingMode::Absolute, AccessClass::ReadModifyWrite},                // $0E
    {Mnemonic::Slo, AddressingMode::Absolute, AccessClass::ReadModifyWrite, kUnofficial},   // $0F
    // $10
    {Mnemonic::Bpl, AddressingMode::Relative, AccessClass::Branch},                         // $10
    {Mnemonic::Ora, AddressingMode::IndirectIndexed, AccessClass::Read},                    // $11
    {Mnemonic::Jam, AddressingMode::Implied, AccessClass::Jam, kUnofficial},                // $12
    {Mnemonic::Slo, AddressingMode::IndirectIndexed, AccessClass::ReadModifyWrite, kUnofficial},// $13
    {Mnemonic::Nop, AddressingMode::ZeroPageX, AccessClass::Read, kUnofficial},             // $14
    {Mnemonic::Ora, AddressingMode::ZeroPageX, AccessClass::Read},                          // $15
    {Mnemonic::Asl, AddressingMode::ZeroPageX, AccessClass::ReadModifyWrite},               // $16
    {Mnemonic::Slo, AddressingMode::ZeroPageX, AccessClass::ReadModifyWrite, kUnofficial},  // $17
    {Mnemonic::Clc, AddressingMode::Implied, AccessClass::Implied},                         // $18
    {Mnemonic::Ora, AddressingMode::AbsoluteY, AccessClass::Read},                          // $19
    {Mnemonic::Nop, AddressingMode::Implied, AccessClass::Implied, kUnofficial},            // $1A
    {Mnemonic::Slo, AddressingMode::AbsoluteY, AccessClass::ReadModifyWrite, kUnofficial},  // $1B
    {Mnemonic::Nop, AddressingMode::AbsoluteX, AccessClass::Read, kUnofficial},             // $1C
    {Mnemonic::Ora, AddressingMode::AbsoluteX, AccessClass::Read},                          // $1D
    {Mnemonic::Asl, AddressingMode::AbsoluteX, AccessClass::ReadModifyWrite},               // $1E
    {Mnemonic::Slo, AddressingMode::AbsoluteX, AccessClass::ReadModifyWrite, kUnofficial},  // $1F
    // $20
    {Mnemonic::Jsr, AddressingMode::Absolute, AccessClass::Call},                           // $20
    {Mnemonic::And, AddressingMode::IndexedIndirect, AccessClass::Read},                    // $21
    {Mnemonic::Jam, AddressingMode::Implied, AccessClass::Jam, kUnofficial},                // $22
    {Mnemonic::Rla, AddressingMode::IndexedIndirect, AccessClass::ReadModifyWrite, kUnofficial},// $23
    {Mnemonic::Bit, AddressingMode::ZeroPage, AccessClass::Read},                           // $24
    {Mnemonic::And, AddressingMode::ZeroPage, AccessClass::Read},                           // $25
    {Mnemonic::Rol, AddressingMode::ZeroPage, AccessClass::ReadModifyWrite},                // $26
    {Mnemonic::Rla, AddressingMode::ZeroPage, AccessClass::ReadModifyWrite, kUnofficial},   // $27
    {Mnemonic::Plp, AddressingMode::Implied, AccessClass::Pull},                            // $28
    {Mnemonic::And, AddressingMode::Immediate, AccessClass::Read},                          // $29
    {Mnemonic::Rol, AddressingMode::Accumulator, AccessClass::Implied},                     // $2A
    {Mnemonic::Anc, AddressingMode::Immediate, AccessClass::Read, kUnofficial},             // $2B
    {Mnemonic::Bit, AddressingMode::Absolute, AccessClass::Read},                           // $2C
    {Mnemonic::And, AddressingMode::Absolute, AccessClass::Read},                           // $2D
    {Mnemonic::Rol, AddressingMode::Absolute, AccessClass::ReadModifyWrite},                // $2E
    {Mnemonic::Rla, AddressingMode::Absolute, AccessClass::ReadModifyWrite, kUnofficial},   // $2F
    // $30
    {Mnemonic::Bmi, AddressingMode::Relative, AccessClass::Branch},                         // $30
    {Mnemonic::And, AddressingMode::IndirectIndexed, AccessClass::Read},                    // $31
    {Mnemonic::Jam, AddressingMode::Implied, AccessClass::Jam, kUnofficial},                // $32
    {Mnemonic::Rla, AddressingMode::IndirectIndexed, AccessClass::ReadModifyWrite, kUnofficial},// $33
    {Mnemonic::Nop, AddressingMode::ZeroPageX, AccessClass::Read, kUnofficial},             // $34
    {Mnemonic::And, AddressingMode::ZeroPageX, AccessClass::Read},                          // $35
    {Mnemonic::Rol, AddressingMode::ZeroPageX, AccessClass::ReadModifyWrite},               // $36
    {Mnemonic::Rla, AddressingMode::ZeroPageX, AccessClass::ReadModifyWrite, kUnofficial},  // $37
    {Mnemonic::Sec, AddressingMode::Implied, AccessClass::Implied},                         // $38
    {Mnemonic::And, AddressingMode::AbsoluteY, AccessClass::Read},                          // $39
    {Mnemonic::Nop, AddressingMode::Implied, AccessClass::Implied, kUnofficial},            // $3A
    {Mnemonic::Rla, AddressingMode::AbsoluteY, AccessClass::ReadModifyWrite, kUnofficial},  // $3B
    {Mnemonic::Nop, AddressingMode::AbsoluteX, AccessClass::Read, kUnofficial},             // $3C
    {Mnemonic::And, AddressingMode::AbsoluteX, AccessClass::Read},                          // $3D
    {Mnemonic::Rol, AddressingMode::AbsoluteX, AccessClass::ReadModifyWrite},               // $3E
    {Mnemonic::Rla, AddressingMode::AbsoluteX, AccessClass::ReadModifyWrite, kUnofficial},  // $3F
    // $40
    {Mnemonic::Rti, AddressingMode::Implied, AccessClass::ReturnFromInterrupt},             // $40
    {Mnemonic::Eor, AddressingMode::IndexedIndirect, AccessClass::Read},                    // $41
    {Mnemonic::Jam, AddressingMode::Implied, AccessClass::Jam, kUnofficial},                // $42
    {Mnemonic::Sre, AddressingMode::IndexedIndirect, AccessClass::ReadModifyWrite, kUnofficial},// $43
    {Mnemonic::Nop, AddressingMode::ZeroPage, AccessClass::Read, kUnofficial},              // $44
    {Mnemonic::Eor, AddressingMode::ZeroPage, AccessClass::Read},                           // $45
    {Mnemonic::Lsr, AddressingMode::ZeroPage, AccessClass::ReadModifyWrite},                // $46
    {Mnemonic::Sre, AddressingMode::ZeroPage, AccessClass::ReadModifyWrite, kUnofficial},   // $47
    {Mnemonic::Pha, AddressingMode::Implied, AccessClass::Push},                            // $48
    {Mnemonic::Eor, AddressingMode::Immediate, AccessClass::Read},                          // $49
    {Mnemonic::Lsr, AddressingMode::Accumulator, AccessClass::Implied},                     // $4A
    {Mnemonic::Alr, AddressingMode::Immediate, AccessClass::Read, kUnofficial},             // $4B
    {Mnemonic::Jmp, AddressingMode::Absolute, AccessClass::Jump},                           // $4C
    {Mnemonic::Eor, AddressingMode::Absolute, AccessClass::Read},                           // $4D
    {Mnemonic::Lsr, AddressingMode::Absolute, AccessClass::ReadModifyWrite},                // $4E
    {Mnemonic::Sre, AddressingMode::Absolute, AccessClass::ReadModifyWrite, kUnofficial},   // $4F
    // $50
    {Mnemonic::Bvc, AddressingMode::Relative, AccessClass::Branch},                         // $50
    {Mnemonic::Eor, AddressingMode::IndirectIndexed, AccessClass::Read},                    // $51
    {Mnemonic::Jam, AddressingMode::Implied, AccessClass::Jam, kUnofficial},                // $52
    {Mnemonic::Sre, AddressingMode::IndirectIndexed, AccessClass::ReadModifyWrite, kUnofficial},// $53
    {Mnemonic::Nop, AddressingMode::ZeroPageX, AccessClass::Read, kUnofficial},             // $54
    {Mnemonic::Eor, AddressingMode::ZeroPageX, AccessClass::Read},                          // $55
    {Mnemonic::Lsr, AddressingMode::ZeroPageX, AccessClass::ReadModifyWrite},               // $56
    {Mnemonic::Sre, AddressingMode::ZeroPageX, AccessClass::ReadModifyWrite, kUnofficial},  // $57
    {Mnemonic::Cli, AddressingMode::Implied, AccessClass::Implied},                         // $58
    {Mnemonic::Eor, AddressingMode::AbsoluteY, AccessClass::Read},                          // $59
    {Mnemonic::Nop, AddressingMode::Implied, AccessClass::Implied, kUnofficial},            // $5A
    {Mnemonic::Sre, AddressingMode::AbsoluteY, AccessClass::ReadModifyWrite, kUnofficial},  // $5B
    {Mnemonic::Nop, AddressingMode::AbsoluteX, AccessClass::Read, kUnofficial},             // $5C
    {Mnemonic::Eor, AddressingMode::AbsoluteX, AccessClass::Read},                          // $5D
    {Mnemonic::Lsr, AddressingMode::AbsoluteX, AccessClass::ReadModifyWrite},               // $5E
    {Mnemonic::Sre, AddressingMode::AbsoluteX, AccessClass::ReadModifyWrite, kUnofficial},  // $5F
    // $60
    {Mnemonic::Rts, AddressingMode::Implied, AccessClass::Return},                          // $60
    {Mnemonic::Adc, AddressingMode::IndexedIndirect, AccessClass::Read},                    // $61
    {Mnemonic::Jam, AddressingMode::Implied, AccessClass::Jam, kUnofficial},                // $62
    {Mnemonic::Rra, AddressingMode::IndexedIndirect, AccessClass::ReadModifyWrite, kUnofficial},// $63
    {Mnemonic::Nop, AddressingMode::ZeroPage, AccessClass::Read, kUnofficial},              // $64
    {Mnemonic::Adc, AddressingMode::ZeroPage, AccessClass::Read},                           // $65
    {Mnemonic::Ror, AddressingMode::ZeroPage, AccessClass::ReadModifyWrite},                // $66
    {Mnemonic::Rra, AddressingMode::ZeroPage, AccessClass::ReadModifyWrite, kUnofficial},   // $67
    {Mnemonic::Pla, AddressingMode::Implied, AccessClass::Pull},                            // $68
    {Mnemonic::Adc, AddressingMode::Immediate, AccessClass::Read},                          // $69
    {Mnemonic::Ror, AddressingMode::Accumulator, AccessClass::Implied},                     // $6A
    {Mnemonic::Arr, AddressingMode::Immediate, AccessClass::Read, kUnofficial},             // $6B
    {Mnemonic::Jmp, AddressingMode::Indirect, AccessClass::Jump},                           // $6C
    {Mnemonic::Adc, AddressingMode::Absolute, AccessClass::Read},                           // $6D
    {Mnemonic::Ror, AddressingMode::Absolute, AccessClass::ReadModifyWrite},                // $6E
    {Mnemonic::Rra, AddressingMode::Absolute, AccessClass::ReadModifyWrite, kUnofficial},   // $6F
    // $70
    {Mnemonic::Bvs, AddressingMode::Relative, AccessClass::Branch},                         // $70
    {Mnemonic::Adc, AddressingMode::IndirectIndexed, AccessClass::Read},                    // $71
    {Mnemonic::Jam, AddressingMode::Implied, AccessClass::Jam, kUnofficial},                // $72
    {Mnemonic::Rra, AddressingMode::IndirectIndexed, AccessClass::ReadModifyWrite, kUnofficial},// $73
    {Mnemonic::Nop, AddressingMode::ZeroPageX, AccessClass::Read, kUnofficial},             // $74
    {Mnemonic::Adc, AddressingMode::ZeroPageX, AccessClass::Read},                          // $75
    {Mnemonic::Ror, AddressingMode::ZeroPageX, AccessClass::ReadModifyWrite},               // $76
    {Mnemonic::Rra, AddressingMode::ZeroPageX, AccessClass::ReadModifyWrite, kUnofficial},  // $77
    {Mnemonic::Sei, AddressingMode::Implied, AccessClass::Implied},                         // $78
    {Mnemonic::Adc, AddressingMode::AbsoluteY, AccessClass::Read},                          // $79
    {Mnemonic::Nop, AddressingMode::Implied, AccessClass::Implied, kUnofficial},            // $7A
    {Mnemonic::Rra, AddressingMode::AbsoluteY, AccessClass::ReadModifyWrite, kUnofficial},  // $7B
    {Mnemonic::Nop, AddressingMode::AbsoluteX, AccessClass::Read, kUnofficial},             // $7C
    {Mnemonic::Adc, AddressingMode::AbsoluteX, AccessClass::Read},                          // $7D
    {Mnemonic::Ror, AddressingMode::AbsoluteX, AccessClass::ReadModifyWrite},               // $7E
    {Mnemonic::Rra, AddressingMode::AbsoluteX, AccessClass::ReadModifyWrite, kUnofficial},  // $7F
    // $80
    {Mnemonic::Nop, AddressingMode::Immediate, AccessClass::Read, kUnofficial},             // $80
    {Mnemonic::Sta, AddressingMode::IndexedIndirect, AccessClass::Write},                   // $81
    {Mnemonic::Nop, AddressingMode::Immediate, AccessClass::Read, kUnofficial},             // $82
    {Mnemonic::Sax, AddressingMode::IndexedIndirect, AccessClass::Write, kUnofficial},      // $83
    {Mnemonic::Sty, AddressingMode::ZeroPage, AccessClass::Write},                          // $84
    {Mnemonic::Sta, AddressingMode::ZeroPage, AccessClass::Write},                          // $85
    {Mnemonic::Stx, AddressingMode::ZeroPage, AccessClass::Write},                          // $86
    {Mnemonic::Sax, AddressingMode::ZeroPage, AccessClass::Write, kUnofficial},             // $87
    {Mnemonic::Dey, AddressingMode::Implied, AccessClass::Implied},                         // $88
    {Mnemonic::Nop, AddressingMode::Immediate, AccessClass::Read, kUnofficial},             // $89
    {Mnemonic::Txa, AddressingMode::Implied, AccessClass::Implied},                         // $8A
    {Mnemonic::Ane, AddressingMode::Immediate, AccessClass::Read, kUnofficial, kUnstable},  // $8B
    {Mnemonic::Sty, AddressingMode::Absolute, AccessClass::Write},                          // $8C
    {Mnemonic::Sta, AddressingMode::Absolute, AccessClass::Write},                          // $8D
    {Mnemonic::Stx, AddressingMode::Absolute, AccessClass::Write},                          // $8E
    {Mnemonic::Sax, AddressingMode::Absolute, AccessClass::Write, kUnofficial},             // $8F
    // $90
    {Mnemonic::Bcc, AddressingMode::Relative, AccessClass::Branch},                         // $90
    {Mnemonic::Sta, AddressingMode::IndirectIndexed, AccessClass::Write},                   // $91
    {Mnemonic::Jam, AddressingMode::Implied, AccessClass::Jam, kUnofficial},                // $92
    {Mnemonic::Sha, AddressingMode::IndirectIndexed, AccessClass::Write, kUnofficial, kUnstable},// $93
    {Mnemonic::Sty, AddressingMode::ZeroPageX, AccessClass::Write},                         // $94
    {Mnemonic::Sta, AddressingMode::ZeroPageX, AccessClass::Write},                         // $95
    {Mnemonic::Stx, AddressingMode::ZeroPageY, AccessClass::Write},                         // $96
    {Mnemonic::Sax, AddressingMode::ZeroPageY, AccessClass::Write, kUnofficial},            // $97
    {Mnemonic::Tya, AddressingMode::Implied, AccessClass::Implied},                         // $98
    {Mnemonic::Sta, AddressingMode::AbsoluteY, AccessClass::Write},                         // $99
    {Mnemonic::Txs, AddressingMode::Implied, AccessClass::Implied},                         // $9A
    {Mnemonic::Tas, AddressingMode::AbsoluteY, AccessClass::Write, kUnofficial, kUnstable}, // $9B
    {Mnemonic::Shy, AddressingMode::AbsoluteX, AccessClass::Write, kUnofficial, kUnstable}, // $9C
    {Mnemonic::Sta, AddressingMode::AbsoluteX, AccessClass::Write},                         // $9D
    {Mnemonic::Shx, AddressingMode::AbsoluteY, AccessClass::Write, kUnofficial, kUnstable}, // $9E
    {Mnemonic::Sha, AddressingMode::AbsoluteY, AccessClass::Write, kUnofficial, kUnstable}, // $9F
    // $A0
    {Mnemonic::Ldy, AddressingMode::Immediate, AccessClass::Read},                          // $A0
    {Mnemonic::Lda, AddressingMode::IndexedIndirect, AccessClass::Read},                    // $A1
    {Mnemonic::Ldx, AddressingMode::Immediate, AccessClass::Read},                          // $A2
    {Mnemonic::Lax, AddressingMode::IndexedIndirect, AccessClass::Read, kUnofficial},       // $A3
    {Mnemonic::Ldy, AddressingMode::ZeroPage, AccessClass::Read},                           // $A4
    {Mnemonic::Lda, AddressingMode::ZeroPage, AccessClass::Read},                           // $A5
    {Mnemonic::Ldx, AddressingMode::ZeroPage, AccessClass::Read},                           // $A6
    {Mnemonic::Lax, AddressingMode::ZeroPage, AccessClass::Read, kUnofficial},              // $A7
    {Mnemonic::Tay, AddressingMode::Implied, AccessClass::Implied},                         // $A8
    {Mnemonic::Lda, AddressingMode::Immediate, AccessClass::Read},                          // $A9
    {Mnemonic::Tax, AddressingMode::Implied, AccessClass::Implied},                         // $AA
    {Mnemonic::Lxa, AddressingMode::Immediate, AccessClass::Read, kUnofficial, kUnstable},  // $AB
    {Mnemonic::Ldy, AddressingMode::Absolute, AccessClass::Read},                           // $AC
    {Mnemonic::Lda, AddressingMode::Absolute, AccessClass::Read},                           // $AD
    {Mnemonic::Ldx, AddressingMode::Absolute, AccessClass::Read},                           // $AE
    {Mnemonic::Lax, AddressingMode::Absolute, AccessClass::Read, kUnofficial},              // $AF
    // $B0
    {Mnemonic::Bcs, AddressingMode::Relative, AccessClass::Branch},                         // $B0
    {Mnemonic::Lda, AddressingMode::IndirectIndexed, AccessClass::Read},                    // $B1
    {Mnemonic::Jam, AddressingMode::Implied, AccessClass::Jam, kUnofficial},                // $B2
    {Mnemonic::Lax, AddressingMode::IndirectIndexed, AccessClass::Read, kUnofficial},       // $B3
    {Mnemonic::Ldy, AddressingMode::ZeroPageX, AccessClass::Read},                          // $B4
    {Mnemonic::Lda, AddressingMode::ZeroPageX, AccessClass::Read},                          // $B5
    {Mnemonic::Ldx, AddressingMode::ZeroPageY, AccessClass::Read},                          // $B6
    {Mnemonic::Lax, AddressingMode::ZeroPageY, AccessClass::Read, kUnofficial},             // $B7
    {Mnemonic::Clv, AddressingMode::Implied, AccessClass::Implied},                         // $B8
    {Mnemonic::Lda, AddressingMode::AbsoluteY, AccessClass::Read},                          // $B9
    {Mnemonic::Tsx, AddressingMode::Implied, AccessClass::Implied},                         // $BA
    {Mnemonic::Las, AddressingMode::AbsoluteY, AccessClass::Read, kUnofficial},             // $BB
    {Mnemonic::Ldy, AddressingMode::AbsoluteX, AccessClass::Read},                          // $BC
    {Mnemonic::Lda, AddressingMode::AbsoluteX, AccessClass::Read},                          // $BD
    {Mnemonic::Ldx, AddressingMode::AbsoluteY, AccessClass::Read},                          // $BE
    {Mnemonic::Lax, AddressingMode::AbsoluteY, AccessClass::Read, kUnofficial},             // $BF
    // $C0
    {Mnemonic::Cpy, AddressingMode::Immediate, AccessClass::Read},                          // $C0
    {Mnemonic::Cmp, AddressingMode::IndexedIndirect, AccessClass::Read},                    // $C1
    {Mnemonic::Nop, AddressingMode::Immediate, AccessClass::Read, kUnofficial},             // $C2
    {Mnemonic::Dcp, AddressingMode::IndexedIndirect, AccessClass::ReadModifyWrite, kUnofficial},// $C3
    {Mnemonic::Cpy, AddressingMode::ZeroPage, AccessClass::Read},                           // $C4
    {Mnemonic::Cmp, AddressingMode::ZeroPage, AccessClass::Read},                           // $C5
    {Mnemonic::Dec, AddressingMode::ZeroPage, AccessClass::ReadModifyWrite},                // $C6
    {Mnemonic::Dcp, AddressingMode::ZeroPage, AccessClass::ReadModifyWrite, kUnofficial},   // $C7
    {Mnemonic::Iny, AddressingMode::Implied, AccessClass::Implied},                         // $C8
    {Mnemonic::Cmp, AddressingMode::Immediate, AccessClass::Read},                          // $C9
    {Mnemonic::Dex, AddressingMode::Implied, AccessClass::Implied},                         // $CA
    {Mnemonic::Sbx, AddressingMode::Immediate, AccessClass::Read, kUnofficial},             // $CB
    {Mnemonic::Cpy, AddressingMode::Absolute, AccessClass::Read},                           // $CC
    {Mnemonic::Cmp, AddressingMode::Absolute, AccessClass::Read},                           // $CD
    {Mnemonic::Dec, AddressingMode::Absolute, AccessClass::ReadModifyWrite},                // $CE
    {Mnemonic::Dcp, AddressingMode::Absolute, AccessClass::ReadModifyWrite, kUnofficial},   // $CF
    // $D0
    {Mnemonic::Bne, AddressingMode::Relative, AccessClass::Branch},                         // $D0
    {Mnemonic::Cmp, AddressingMode::IndirectIndexed, AccessClass::Read},                    // $D1
    {Mnemonic::Jam, AddressingMode::Implied, AccessClass::Jam, kUnofficial},                // $D2
    {Mnemonic::Dcp, AddressingMode::IndirectIndexed, AccessClass::ReadModifyWrite, kUnofficial},// $D3
    {Mnemonic::Nop, AddressingMode::ZeroPageX, AccessClass::Read, kUnofficial},             // $D4
    {Mnemonic::Cmp, AddressingMode::ZeroPageX, AccessClass::Read},                          // $D5
    {Mnemonic::Dec, AddressingMode::ZeroPageX, AccessClass::ReadModifyWrite},               // $D6
    {Mnemonic::Dcp, AddressingMode::ZeroPageX, AccessClass::ReadModifyWrite, kUnofficial},  // $D7
    {Mnemonic::Cld, AddressingMode::Implied, AccessClass::Implied},                         // $D8
    {Mnemonic::Cmp, AddressingMode::AbsoluteY, AccessClass::Read},                          // $D9
    {Mnemonic::Nop, AddressingMode::Implied, AccessClass::Implied, kUnofficial},            // $DA
    {Mnemonic::Dcp, AddressingMode::AbsoluteY, AccessClass::ReadModifyWrite, kUnofficial},  // $DB
    {Mnemonic::Nop, AddressingMode::AbsoluteX, AccessClass::Read, kUnofficial},             // $DC
    {Mnemonic::Cmp, AddressingMode::AbsoluteX, AccessClass::Read},                          // $DD
    {Mnemonic::Dec, AddressingMode::AbsoluteX, AccessClass::ReadModifyWrite},               // $DE
    {Mnemonic::Dcp, AddressingMode::AbsoluteX, AccessClass::ReadModifyWrite, kUnofficial},  // $DF
    // $E0
    {Mnemonic::Cpx, AddressingMode::Immediate, AccessClass::Read},                          // $E0
    {Mnemonic::Sbc, AddressingMode::IndexedIndirect, AccessClass::Read},                    // $E1
    {Mnemonic::Nop, AddressingMode::Immediate, AccessClass::Read, kUnofficial},             // $E2
    {Mnemonic::Isc, AddressingMode::IndexedIndirect, AccessClass::ReadModifyWrite, kUnofficial},// $E3
    {Mnemonic::Cpx, AddressingMode::ZeroPage, AccessClass::Read},                           // $E4
    {Mnemonic::Sbc, AddressingMode::ZeroPage, AccessClass::Read},                           // $E5
    {Mnemonic::Inc, AddressingMode::ZeroPage, AccessClass::ReadModifyWrite},                // $E6
    {Mnemonic::Isc, AddressingMode::ZeroPage, AccessClass::ReadModifyWrite, kUnofficial},   // $E7
    {Mnemonic::Inx, AddressingMode::Implied, AccessClass::Implied},                         // $E8
    {Mnemonic::Sbc, AddressingMode::Immediate, AccessClass::Read},                          // $E9
    {Mnemonic::Nop, AddressingMode::Implied, AccessClass::Implied},                         // $EA
    {Mnemonic::Sbc, AddressingMode::Immediate, AccessClass::Read, kUnofficial},             // $EB
    {Mnemonic::Cpx, AddressingMode::Absolute, AccessClass::Read},                           // $EC
    {Mnemonic::Sbc, AddressingMode::Absolute, AccessClass::Read},                           // $ED
    {Mnemonic::Inc, AddressingMode::Absolute, AccessClass::ReadModifyWrite},                // $EE
    {Mnemonic::Isc, AddressingMode::Absolute, AccessClass::ReadModifyWrite, kUnofficial},   // $EF
    // $F0
    {Mnemonic::Beq, AddressingMode::Relative, AccessClass::Branch},                         // $F0
    {Mnemonic::Sbc, AddressingMode::IndirectIndexed, AccessClass::Read},                    // $F1
    {Mnemonic::Jam, AddressingMode::Implied, AccessClass::Jam, kUnofficial},                // $F2
    {Mnemonic::Isc, AddressingMode::IndirectIndexed, AccessClass::ReadModifyWrite, kUnofficial},// $F3
    {Mnemonic::Nop, AddressingMode::ZeroPageX, AccessClass::Read, kUnofficial},             // $F4
    {Mnemonic::Sbc, AddressingMode::ZeroPageX, AccessClass::Read},                          // $F5
    {Mnemonic::Inc, AddressingMode::ZeroPageX, AccessClass::ReadModifyWrite},               // $F6
    {Mnemonic::Isc, AddressingMode::ZeroPageX, AccessClass::ReadModifyWrite, kUnofficial},  // $F7
    {Mnemonic::Sed, AddressingMode::Implied, AccessClass::Implied},                         // $F8
    {Mnemonic::Sbc, AddressingMode::AbsoluteY, AccessClass::Read},                          // $F9
    {Mnemonic::Nop, AddressingMode::Implied, AccessClass::Implied, kUnofficial},            // $FA
    {Mnemonic::Isc, AddressingMode::AbsoluteY, AccessClass::ReadModifyWrite, kUnofficial},  // $FB
    {Mnemonic::Nop, AddressingMode::AbsoluteX, AccessClass::Read, kUnofficial},             // $FC
    {Mnemonic::Sbc, AddressingMode::AbsoluteX, AccessClass::Read},                          // $FD
    {Mnemonic::Inc, AddressingMode::AbsoluteX, AccessClass::ReadModifyWrite},               // $FE
    {Mnemonic::Isc, AddressingMode::AbsoluteX, AccessClass::ReadModifyWrite, kUnofficial},  // $FF
}};

// The mnemonic spellings, in the order of the Mnemonic enumeration.
constexpr std::array<std::string_view, 76> kMnemonicNames{{
    "ADC", "ALR", "ANC", "AND", "ANE", "ARR", "ASL", "BCC", "BCS", "BEQ", "BIT", "BMI",
    "BNE", "BPL", "BRK", "BVC", "BVS", "CLC", "CLD", "CLI", "CLV", "CMP", "CPX", "CPY",
    "DCP", "DEC", "DEX", "DEY", "EOR", "INC", "INX", "INY", "ISC", "JAM", "JMP", "JSR",
    "LAS", "LAX", "LDA", "LDX", "LDY", "LSR", "LXA", "NOP", "ORA", "PHA", "PHP", "PLA",
    "PLP", "RLA", "ROL", "ROR", "RRA", "RTI", "RTS", "SAX", "SBC", "SBX", "SEC", "SED",
    "SEI", "SHA", "SHX", "SHY", "SLO", "SRE", "STA", "STX", "STY", "TAS", "TAX", "TAY",
    "TSX", "TXA", "TXS", "TYA",
}};

static_assert(kMnemonicNames.size() == static_cast<std::size_t>(Mnemonic::Tya) + 1,
              "one spelling per mnemonic");

// Whether every read, write and read-modify-write row names a mode with an address
// to reach: immediate, zero page, absolute, or one of their indexed and indirect
// forms.
constexpr bool addressedRowsHaveAddressedModes() {
  for (const Opcode& opcode : kOpcodeTable) {
    const bool addressed = opcode.access == AccessClass::Read ||
                           opcode.access == AccessClass::Write ||
                           opcode.access == AccessClass::ReadModifyWrite;
    const bool modeless = opcode.mode == AddressingMode::Implied ||
                          opcode.mode == AddressingMode::Accumulator ||
                          opcode.mode == AddressingMode::Relative ||
                          opcode.mode == AddressingMode::Indirect;
    if (addressed && modeless) return false;
  }
  return true;
}

static_assert(addressedRowsHaveAddressedModes(),
              "a read, write or read-modify-write row names a mode with an address");

}  // namespace

const Opcode& opcodeAt(std::uint8_t code) noexcept { return kOpcodeTable[code]; }

std::string_view mnemonicName(Mnemonic mnemonic) noexcept {
  return kMnemonicNames[static_cast<std::size_t>(mnemonic)];
}

void Cpu6502::applyImplied(Mnemonic mnemonic) noexcept {
  switch (mnemonic) {
    // The transfers set N and Z from the value moved, except TXS, which sets no flag.
    case Mnemonic::Tax: state_.x = state_.a; setNZ(state_.x); break;
    case Mnemonic::Tay: state_.y = state_.a; setNZ(state_.y); break;
    case Mnemonic::Tsx: state_.x = state_.s; setNZ(state_.x); break;
    case Mnemonic::Txa: state_.a = state_.x; setNZ(state_.a); break;
    case Mnemonic::Tya: state_.a = state_.y; setNZ(state_.a); break;
    case Mnemonic::Txs: state_.s = state_.x; break;

    // The index steps set N and Z.
    case Mnemonic::Inx: state_.x = incOp(state_.x); break;
    case Mnemonic::Iny: state_.y = incOp(state_.y); break;
    case Mnemonic::Dex: state_.x = decOp(state_.x); break;
    case Mnemonic::Dey: state_.y = decOp(state_.y); break;

    // The accumulator forms of the shifts and rotates.
    case Mnemonic::Asl: state_.a = aslOp(state_.a); break;
    case Mnemonic::Lsr: state_.a = lsrOp(state_.a); break;
    case Mnemonic::Rol: state_.a = rolOp(state_.a); break;
    case Mnemonic::Ror: state_.a = rorOp(state_.a); break;

    // The flag instructions. CLI and SEI change I on this, their last cycle, after
    // the cycle's interrupt poll, so the change is seen by the next instruction's
    // polls (NESdev wiki, "CPU interrupts", Delayed IRQ response after CLI, SEI,
    // and PLP).
    case Mnemonic::Clc: setCarry(false); break;
    case Mnemonic::Sec: setCarry(true); break;
    case Mnemonic::Cli: state_.p = static_cast<std::uint8_t>(state_.p & ~kCpuFlagI); break;
    case Mnemonic::Sei: state_.p = static_cast<std::uint8_t>(state_.p | kCpuFlagI); break;
    case Mnemonic::Cld: state_.p = static_cast<std::uint8_t>(state_.p & ~kCpuFlagD); break;
    case Mnemonic::Sed: state_.p = static_cast<std::uint8_t>(state_.p | kCpuFlagD); break;
    case Mnemonic::Clv: setOverflow(false); break;

    // NOP, and every other mnemonic, which the implied class does not carry.
    default: break;
  }
}

// The loads (MCS6500 programming manual, 2.1.1, 7.0 and 7.1), ADC and SBC (2.2.1 and
// 2.2.2; SBC adds the complement of the operand with the carry in), the bitwise
// operators (2.2.4), the compares (4.2.1, 7.8 and 7.9) and BIT (4.2.2.1).
void Cpu6502::applyRead(Mnemonic mnemonic, std::uint8_t value) noexcept {
  switch (mnemonic) {
    case Mnemonic::Lda: state_.a = value; setNZ(state_.a); break;
    case Mnemonic::Ldx: state_.x = value; setNZ(state_.x); break;
    case Mnemonic::Ldy: state_.y = value; setNZ(state_.y); break;
    case Mnemonic::And: andOp(value); break;
    case Mnemonic::Ora: oraOp(value); break;
    case Mnemonic::Eor: eorOp(value); break;
    case Mnemonic::Adc: addWithCarry(value, false); break;
    case Mnemonic::Sbc: addWithCarry(value, true); break;
    case Mnemonic::Cmp: compareWith(state_.a, value); break;
    case Mnemonic::Cpx: compareWith(state_.x, value); break;
    case Mnemonic::Cpy: compareWith(state_.y, value); break;
    case Mnemonic::Bit: bitOp(value); break;
    case Mnemonic::Nop: break;
    // The unofficial read operations are not in this file; they load nothing.
    default: break;
  }
}

// The shifts and rotates (MCS6500 programming manual, 10.1 to 10.4) and INC and DEC
// (10.7 and 10.8).
std::uint8_t Cpu6502::modify(Mnemonic mnemonic, std::uint8_t value) noexcept {
  switch (mnemonic) {
    case Mnemonic::Asl: return aslOp(value);
    case Mnemonic::Lsr: return lsrOp(value);
    case Mnemonic::Rol: return rolOp(value);
    case Mnemonic::Ror: return rorOp(value);
    case Mnemonic::Inc: return incOp(value);
    case Mnemonic::Dec: return decOp(value);
    // The unofficial read-modify-write operations are not in this file; they write the value back unchanged.
    default: return value;
  }
}

// STA, STX and STY store a register unchanged and set no flag (MCS6500 programming
// manual, 2.1.2, 7.2 and 7.3).
std::uint8_t Cpu6502::storeValue(Mnemonic mnemonic) const noexcept {
  switch (mnemonic) {
    case Mnemonic::Sta: return state_.a;
    case Mnemonic::Stx: return state_.x;
    case Mnemonic::Sty: return state_.y;
    // The unofficial stores are not in this file; they write $00.
    default: return 0;
  }
}

// Each branch tests one flag, taken when the flag is clear or when it is set
// (NESdev wiki, "Instruction reference", BCC through BVS).
bool Cpu6502::branchTaken(Mnemonic mnemonic) const noexcept {
  switch (mnemonic) {
    case Mnemonic::Bpl: return (state_.p & kCpuFlagN) == 0;
    case Mnemonic::Bmi: return (state_.p & kCpuFlagN) != 0;
    case Mnemonic::Bvc: return (state_.p & kCpuFlagV) == 0;
    case Mnemonic::Bvs: return (state_.p & kCpuFlagV) != 0;
    case Mnemonic::Bcc: return (state_.p & kCpuFlagC) == 0;
    case Mnemonic::Bcs: return (state_.p & kCpuFlagC) != 0;
    case Mnemonic::Bne: return (state_.p & kCpuFlagZ) == 0;
    case Mnemonic::Beq: return (state_.p & kCpuFlagZ) != 0;
    default: return false;
  }
}

}  // namespace nescore
