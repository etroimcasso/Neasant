# The WAV writer as a library, so nes_render and the suite link one copy of it. tools/ is
# on its public include path, so its header is included as render/wav_writer.h.
add_library(neasant_render STATIC wav_writer.cpp)
target_include_directories(neasant_render PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/tools>
)
target_compile_features(neasant_render PUBLIC cxx_std_20)
target_compile_options(neasant_render PRIVATE ${NEASANT_WARNING_FLAGS})

# nes_render: a cartridge run from power-on and its sound written as a WAV file.
add_executable(nes_render nes_render_main.cpp)
target_link_libraries(nes_render PRIVATE neasant_render neasant::neasant)
target_compile_options(nes_render PRIVATE ${NEASANT_WARNING_FLAGS})

# The command is linked lean: code nothing references is dropped, and outside a Debug
# build the symbol table is left out. MSVC's Release link does both unasked.
if(NOT MSVC)
  target_compile_options(neasant_render PRIVATE -ffunction-sections -fdata-sections)
  target_compile_options(nes_render PRIVATE -ffunction-sections -fdata-sections)
endif()
if(APPLE)
  target_link_options(nes_render PRIVATE
    -Wl,-dead_strip $<$<NOT:$<CONFIG:Debug>>:-Wl,-x>)
elseif(NOT MSVC)
  target_link_options(nes_render PRIVATE
    -Wl,--gc-sections $<$<NOT:$<CONFIG:Debug>>:-Wl,-s>)
endif()
