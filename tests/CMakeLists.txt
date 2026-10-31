# The suite. GoogleTest is fetched at a pinned release; nothing else is vendored.
include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.18.0
  GIT_SHALLOW    TRUE
)
# Keep gtest's own settings from bleeding into the parent cache.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Every test executable is built the same way: linked against the library and
# gtest_main, compiled under the library's warning flags, and registered test by
# test when the suite runs.
function(neasant_add_test target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE neasant::neasant GTest::gtest_main)
  target_compile_options(${target} PRIVATE ${NEASANT_WARNING_FLAGS})
  if(MSVC)
    # The tests read their environment through std::getenv, which MSVC's runtime marks
    # as unsafe in favour of its own _dupenv_s.
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST)
endfunction()

# One test executable per area of the library, so a failure names the area it is
# in.

# The library's version string.
neasant_add_test(version_test version_test.cpp)

# The 6502 core: its state value, bus concept, cycle stepping, power-on and reset
# sequence, and RDY halt.
neasant_add_test(cpu_state_test cpu/state_test.cpp)

# The 6502 core against the SingleStepTests nes6502 vector set, one test per opcode
# file, and the vector reader. An empty NEASANT_NES6502_VECTORS skips each opcode
# with the variable named.
neasant_add_test(cpu_vectors_test cpu/vectors_test.cpp cpu/vector_harness.cpp)
target_compile_definitions(cpu_vectors_test PRIVATE
  NEASANT_NES6502_VECTORS="${NEASANT_NES6502_VECTORS}")

# The 6502 core's addressing modes and control-flow sequences, one instruction at a
# time: cycle counts, every bus access with its kind, and the results. The recording
# bus is the vector harness's.
neasant_add_test(cpu_modes_test cpu/modes_test.cpp cpu/vector_harness.cpp)

# The 6502 core's official operations: the value each leaves and the flags it sets,
# checked against the programming manual's worked examples. The recording bus is the
# vector harness's.
neasant_add_test(cpu_operations_test cpu/operations_test.cpp cpu/vector_harness.cpp)

# The 6502 core's interrupt lines and its halt: when an NMI edge or an IRQ level is taken,
# the poll's timing, the vector a sequence takes when an NMI arrives inside it, and the DMA
# units' hold on the CPU through the RDY line. The recording bus is the vector harness's.
neasant_add_test(cpu_lines_test cpu/lines_test.cpp cpu/vector_harness.cpp)

# The 6502 core's unofficial opcodes: the combined operations' results and flags, the
# stores whose value passes through the high byte of their address, and the halt a JAM
# opcode brings. The recording bus is the vector harness's.
neasant_add_test(cpu_unofficial_test cpu/unofficial_test.cpp cpu/vector_harness.cpp)

# The cartridge as a value: the two header formats byte by byte, the image sliced and
# the overrides applied, the refusals, and NROM answering on the CPU and PPU pins.
neasant_add_test(nes_cartridge_test nes/cartridge_test.cpp)

# The machine around the core: construction from a config value and its refusals, one
# CPU cycle a step with the reset sequence over the first seven, the master-cycle budget
# and its overshoot, the bus decode over work RAM, the cartridge, open bus, the PPU's
# latch and the APU's bytes, the reset line, snapshot and restore, and the clock values.
neasant_add_test(nes_machine_test nes/machine_test.cpp)

# The PPU without its picture: the eight ports with their latches, values and side
# effects, the scroll file, the port at $2007 over the chip's address space and the
# console's nametable RAM, the palette and OAM, the read buffer, the warm-up window,
# the frame counter with its flag and its odd-frame skip, the NMI output and the cycle
# the CPU takes the interrupt on, a read of $2002 against the flag's dot under each
# alignment, power-on and reset. The authored-image helpers are the machine tests'.
neasant_add_test(ppu_registers_test ppu/registers_test.cpp)
target_include_directories(ppu_registers_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/nes)

# The controller ports and OAM DMA: the strobe and the registers it fills, a pad read
# bit by bit through its port's clock line, a run of reads as one clock, an empty
# socket, the open-bus bits above the lines; the OAM DMA unit halting the CPU through
# RDY for 513 or 514 cycles, its gets and puts through the one bus, the halted read on
# its no-operation cycles, the halt waiting for a write cycle, a reset ending a
# transfer, a snapshot carrying one, and an NMI arriving inside one.
neasant_add_test(nes_input_dma_test nes/input_dma_test.cpp)

# The observers, the save and determinism: the bus observer told every access with its
# byte, kind and source over an instruction mix, a register read and an OAM DMA transfer;
# the save observer told at the end of a frame that changed the battery-backed PRG RAM
# and after a restore that changes it, with the terms a report throws on; two machines
# built alike, run differently and restored one from the other, equal state by state and
# access by access.
neasant_add_test(nes_observers_test nes/observers_test.cpp)

# The CPU proven on the self-checking test programs: each ROM booted on the machine, run
# frame by frame to its report, and the report read out of the state value, the status
# byte and text at $6000 for the programs that write them and the console's nametable for
# the ones that print. An empty NEASANT_NES_TEST_ROMS skips each case with the variable
# named; a program that needs hardware the machine does not hold skips naming it.
neasant_add_test(nes_blargg_cpu_test nes/blargg_cpu_test.cpp nes/rom_harness.cpp)
target_compile_definitions(nes_blargg_cpu_test PRIVATE
  NEASANT_NES_TEST_ROMS="${NEASANT_NES_TEST_ROMS}")
