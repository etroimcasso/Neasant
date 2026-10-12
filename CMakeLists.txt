cmake_minimum_required(VERSION 3.24)

project(nescore VERSION 0.0.0 LANGUAGES CXX)

# ── Top-level detection ─────────────────────────────────────────────────────
# The suite builds only when this is the top-level project; a parent that
# vendors the library gets the library alone.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(NESCORE_IS_TOP_LEVEL ON)
else()
  set(NESCORE_IS_TOP_LEVEL OFF)
endif()

option(NESCORE_BUILD_TESTS "Build the test suite" ${NESCORE_IS_TOP_LEVEL})

# Third-party reference data is never vendored. Each set is a path option the
# tests read; an unset path registers the test and skips it with a reason.
set(NESCORE_NES6502_VECTORS "" CACHE PATH
    "Directory holding the SingleStepTests 65x02/nes6502/v1 vector files")

# ── Build type ──────────────────────────────────────────────────────────────
# A build is optimized by default. Debug is an explicit, temporary opt-in.
if(NESCORE_IS_TOP_LEVEL AND NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Release Debug RelWithDebInfo MinSizeRel)
endif()

if(NESCORE_IS_TOP_LEVEL)
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

# ── The library ─────────────────────────────────────────────────────────────
add_library(nescore STATIC
  src/version.cpp
)
add_library(nescore::nescore ALIAS nescore)

target_include_directories(nescore PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_compile_features(nescore PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(nescore PRIVATE /W4 /permissive- /Zc:__cplusplus)
else()
  target_compile_options(nescore PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    # Each function and datum in its own section so a consumer's final link
    # can drop what it never references.
    -ffunction-sections -fdata-sections
  )
endif()

# ── Tests ───────────────────────────────────────────────────────────────────
if(NESCORE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
