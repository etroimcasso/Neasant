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

# One test executable per area of the library, so a failure names the area it is
# in.

# The library's version string.
add_executable(version_test version_test.cpp)
target_link_libraries(version_test PRIVATE nescore::nescore GTest::gtest_main)
gtest_discover_tests(version_test DISCOVERY_MODE PRE_TEST)

# The 6502 core: its state value, bus concept, cycle stepping, power-on and reset
# sequence, and RDY halt.
add_executable(cpu_state_test cpu/state_test.cpp)
target_link_libraries(cpu_state_test PRIVATE nescore::nescore GTest::gtest_main)
gtest_discover_tests(cpu_state_test DISCOVERY_MODE PRE_TEST)

# The 6502 core against the SingleStepTests nes6502 vector set, one test per opcode
# file, and the vector reader. An empty NESCORE_NES6502_VECTORS skips each opcode
# with the variable named.
add_executable(cpu_vectors_test cpu/vectors_test.cpp cpu/vector_harness.cpp)
target_link_libraries(cpu_vectors_test PRIVATE nescore::nescore GTest::gtest_main)
target_compile_definitions(cpu_vectors_test PRIVATE
  NESCORE_NES6502_VECTORS="${NESCORE_NES6502_VECTORS}")
gtest_discover_tests(cpu_vectors_test DISCOVERY_MODE PRE_TEST)

# The 6502 core's addressing modes and control-flow sequences, one instruction at a
# time: cycle counts, every bus access with its kind, and the results. The recording
# bus is the vector harness's.
add_executable(cpu_modes_test cpu/modes_test.cpp cpu/vector_harness.cpp)
target_link_libraries(cpu_modes_test PRIVATE nescore::nescore GTest::gtest_main)
gtest_discover_tests(cpu_modes_test DISCOVERY_MODE PRE_TEST)
