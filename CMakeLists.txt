cmake_minimum_required(VERSION 3.24)

project(neasant VERSION 0.0.0 LANGUAGES CXX)

# ── Top-level detection ─────────────────────────────────────────────────────
# The suite builds only when this is the top-level project; a parent that
# vendors the library gets the library alone.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(NEASANT_IS_TOP_LEVEL ON)
else()
  set(NEASANT_IS_TOP_LEVEL OFF)
endif()

option(NEASANT_BUILD_TESTS "Build the test suite" ${NEASANT_IS_TOP_LEVEL})

# Third-party reference data is never vendored. Each set is a path option the
# tests read; an unset path registers the test and skips it with a reason.
set(NEASANT_NES6502_VECTORS "" CACHE PATH
    "Directory holding the SingleStepTests 65x02/nes6502/v1 vector files")

set(NEASANT_NES_TEST_ROMS "" CACHE PATH
    "Directory holding a clone of christopherpow/nes-test-roms")

# ── Build type ──────────────────────────────────────────────────────────────
# A build is optimized by default. Debug is an explicit, temporary opt-in.
if(NEASANT_IS_TOP_LEVEL AND NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Release Debug RelWithDebInfo MinSizeRel)
endif()

if(NEASANT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

  find_program(CCACHE_PROGRAM ccache)
  if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  endif()
endif()

# ── Warning flags ───────────────────────────────────────────────────────────
# One list for the library and for every test executable, so a header compiles
# without a warning wherever its templates are instantiated.
if(MSVC)
  set(NEASANT_WARNING_FLAGS /W4 /permissive- /Zc:__cplusplus)
else()
  set(NEASANT_WARNING_FLAGS
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

# ── The library ─────────────────────────────────────────────────────────────
add_library(neasant STATIC
  src/version.cpp
  src/cpu/cpu6502.cpp
  src/nes/cartridge.cpp
  src/nes/board.cpp
  src/mappers/nrom.cpp
  src/ppu/ppu.cpp
  src/nes/nes.cpp
)
add_library(neasant::neasant ALIAS neasant)

target_include_directories(neasant PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_compile_features(neasant PUBLIC cxx_std_20)
target_compile_options(neasant PRIVATE ${NEASANT_WARNING_FLAGS})

if(NOT MSVC)
  # Each function and datum in its own section so a consumer's final link
  # can drop what it never references.
  target_compile_options(neasant PRIVATE -ffunction-sections -fdata-sections)
endif()

# ── Tests ───────────────────────────────────────────────────────────────────
if(NEASANT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
