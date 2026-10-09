cmake_minimum_required(VERSION 3.21)
project(drizzy_renderer VERSION 0.2.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "drizzy_renderer targets Windows (Direct3D 11 / 12) only.")
endif()

option(DRIZZY_BUILD_D3D11          "Build the Direct3D 11 backend"                     ON)
option(DRIZZY_BUILD_D3D12          "Build the Direct3D 12 backend"                     ON)
option(DRIZZY_EMBED_DEFAULT_FONT   "Embed Inter Regular for FontAtlas::AddFontDefault" ON)
option(DRIZZY_BUILD_SAMPLES        "Build the sandbox and the overlay DLL"             ${PROJECT_IS_TOP_LEVEL})
# Whole-program optimization: the UI, draw list and font code live in separate files and call each other per widget;
# link-time code generation lets those calls inline. A DLL linking the libraries links with /LTCG (MSVC restarts the
# link that way on its own when it meets /GL objects, with a note suggesting the flag).
option(DRIZZY_ENABLE_LTO           "Link-time code generation in optimized builds"     ON)
# AVX2 code generation. Faster vector code, but the program then needs a CPU from 2013 or later (Intel Haswell / AMD
# Excavator); an older CPU crashes with an illegal instruction. Only enable it when every player's machine has AVX2.
option(DRIZZY_ENABLE_AVX2          "Compile for AVX2 (/arch:AVX2)"                     OFF)
# Encrypt the library's own string literals (theme/color names, format strings, widget ids) with a per-string XOR
# keystream generated at compile time, decoded on use into a buffer that is wiped as soon as it leaves scope. The
# plaintext never appears in the binary image, so `strings` and at-rest memory scans come up empty. This is the
# default; turn it off to keep the plaintext readable while debugging. (Obfuscation, not security: the key ships in
# the binary, so a debugger on the decode site still recovers the text.)
option(DRIZZY_ENCRYPT_STRINGS      "Encrypt string literals in memory (decode-on-use)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(DRIZZY_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT DRIZZY_LTO_SUPPORTED OUTPUT DRIZZY_LTO_ERROR LANGUAGES CXX)
  if(DRIZZY_LTO_SUPPORTED)
    # Applies to every target defined below (Debug stays unoptimized).
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
  else()
    message(STATUS "drizzy: link-time optimization unavailable: ${DRIZZY_LTO_ERROR}")
  endif()
endif()

include(cmake/DrizzyShaders.cmake)

# Warning, conformance and optimization flags shared by every drizzy target. Optimized builds add whole-program data
# optimization (/Gw); /fp:fast is deliberately not used, since culling relies on NaN comparisons behaving as IEEE says.
add_library(drizzy_options INTERFACE)
target_compile_options(drizzy_options INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8 /Zc:__cplusplus /MP>
  $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/Gw>
  $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<BOOL:${DRIZZY_ENABLE_AVX2}>>:/arch:AVX2>)
# In-memory string encryption, on by default. Defined on every target so a table encoded in one translation unit is
# decoded with the same setting everywhere.
target_compile_definitions(drizzy_options INTERFACE
  DRIZZY_ENCRYPT_STRINGS=$<IF:$<BOOL:${DRIZZY_ENCRYPT_STRINGS}>,1,0>)

set(DRIZZY_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${DRIZZY_GENERATED_DIR}")

# ---------------------------------------------------------------------------------------------------------------------
# drizzy_core: shapes, text and draw lists. No OS or graphics API code. Third-party code (stb_truetype, msdfgen) is
# compiled in, so consumers link a single library and ship no extra DLLs.
# ---------------------------------------------------------------------------------------------------------------------
file(GLOB DRIZZY_MSDFGEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/msdfgen/core/*.cpp")
add_library(drizzy_msdfgen OBJECT ${DRIZZY_MSDFGEN_SOURCES})
target_include_directories(drizzy_msdfgen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/msdfgen")
# MSDFGEN_PUBLIC (empty) replaces the msdfgen-config.h its own build would generate.
target_compile_definitions(drizzy_msdfgen PUBLIC MSDFGEN_PUBLIC= MSDFGEN_USE_CPP11)
target_compile_options(drizzy_msdfgen PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W0 /MP>)

add_library(drizzy_core STATIC
  include/drizzy/core.h
  include/drizzy/draw_list.h
  include/drizzy/font.h
  include/drizzy/ui.h
  include/drizzy/debug_draw.h
  include/drizzy/overlay_common.h
  src/draw_list.cpp
  src/font_atlas.cpp
  src/debug_draw.cpp
  src/text.cpp
  src/text_layout.h
  src/ui/ui_internal.h
  src/ui/ui_core.cpp
  src/ui/ui_widgets.cpp
  src/ui/ui_color.cpp
  src/ui/ui_menus.cpp
  src/ui/ui_input_text.cpp
  src/ui/ui_style.cpp
  src/ui/ui_tables.cpp
  src/ui/ui_extras.cpp
  $<TARGET_OBJECTS:drizzy_msdfgen>)
add_library(drizzy::core ALIAS drizzy_core)
target_include_directories(drizzy_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/msdfgen")
target_compile_definitions(drizzy_core PRIVATE MSDFGEN_PUBLIC= MSDFGEN_USE_CPP11)
target_link_libraries(drizzy_core PRIVATE drizzy_options)

# Build-time helper that embeds binary files (fonts) as byte arrays.
if(DRIZZY_EMBED_DEFAULT_FONT OR DRIZZY_BUILD_SAMPLES)
  add_executable(drizzy_bin2c tools/bin2c.cpp)
endif()

if(DRIZZY_EMBED_DEFAULT_FONT)
  set(_font "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf")
  add_custom_command(
    OUTPUT "${DRIZZY_GENERATED_DIR}/default_font.cpp"
    COMMAND drizzy_bin2c "${_font}" "${DRIZZY_GENERATED_DIR}/default_font.cpp" kDrizzyDefaultFont
    DEPENDS drizzy_bin2c "${_font}"
    COMMENT "Embedding Inter-Regular.ttf"
    VERBATIM)
  target_sources(drizzy_core PRIVATE "${DRIZZY_GENERATED_DIR}/default_font.cpp")
  target_compile_definitions(drizzy_core PRIVATE DRIZZY_HAS_DEFAULT_FONT)
endif()

# ---------------------------------------------------------------------------------------------------------------------
# drizzy_d3d11: Direct3D 11 backend (shader model 5.0 via fxc).
# ---------------------------------------------------------------------------------------------------------------------
if(DRIZZY_BUILD_D3D11)
  drizzy_compile_shader(
    SOURCE src/shaders/prim.hlsl ENTRY VSMain PROFILE vs_5_0 VARIABLE g_PrimVS
    OUTPUT "${DRIZZY_GENERATED_DIR}/prim_vs_d3d11.h")
  drizzy_compile_shader(
    SOURCE src/shaders/prim.hlsl ENTRY PSMain PROFILE ps_5_0 VARIABLE g_PrimPS
    OUTPUT "${DRIZZY_GENERATED_DIR}/prim_ps_d3d11.h")

  add_library(drizzy_d3d11 STATIC
    include/drizzy/backend_d3d11.h
    include/drizzy/overlay_d3d11.h
    src/backends/d3d11/renderer_d3d11.cpp
    src/overlay/overlay_d3d11.cpp
    src/shaders/prim.hlsl
    "${DRIZZY_GENERATED_DIR}/prim_vs_d3d11.h"
    "${DRIZZY_GENERATED_DIR}/prim_ps_d3d11.h")
  add_library(drizzy::d3d11 ALIAS drizzy_d3d11)
  target_include_directories(drizzy_d3d11 PRIVATE "${DRIZZY_GENERATED_DIR}")
  target_link_libraries(drizzy_d3d11 PUBLIC drizzy_core d3d11 dxgi PRIVATE drizzy_options)
endif()

# ---------------------------------------------------------------------------------------------------------------------
# drizzy_d3d12: Direct3D 12 backend (shader model 6.0 via dxc).
# ---------------------------------------------------------------------------------------------------------------------
if(DRIZZY_BUILD_D3D12)
  drizzy_compile_shader(
    SOURCE src/shaders/prim.hlsl ENTRY VSMain PROFILE vs_6_0 VARIABLE g_PrimVS12
    OUTPUT "${DRIZZY_GENERATED_DIR}/prim_vs_d3d12.h")
  drizzy_compile_shader(
    SOURCE src/shaders/prim.hlsl ENTRY PSMain PROFILE ps_6_0 VARIABLE g_PrimPS12
    OUTPUT "${DRIZZY_GENERATED_DIR}/prim_ps_d3d12.h")

  add_library(drizzy_d3d12 STATIC
    include/drizzy/backend_d3d12.h
    include/drizzy/overlay_d3d12.h
    src/backends/d3d12/renderer_d3d12.cpp
    src/overlay/overlay_d3d12.cpp
    src/shaders/prim.hlsl
    "${DRIZZY_GENERATED_DIR}/prim_vs_d3d12.h"
    "${DRIZZY_GENERATED_DIR}/prim_ps_d3d12.h")
  add_library(drizzy::d3d12 ALIAS drizzy_d3d12)
  target_include_directories(drizzy_d3d12 PRIVATE "${DRIZZY_GENERATED_DIR}")
  target_link_libraries(drizzy_d3d12 PUBLIC drizzy_core d3d12 dxgi PRIVATE drizzy_options)
endif()

set_source_files_properties(src/shaders/prim.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)

# ---------------------------------------------------------------------------------------------------------------------
# Samples
# ---------------------------------------------------------------------------------------------------------------------
if(DRIZZY_BUILD_SAMPLES AND DRIZZY_BUILD_D3D11 AND DRIZZY_BUILD_D3D12)
  # The fonts the samples offer besides Inter (all SIL Open Font License; see assets/fonts/*-LICENSE.txt), embedded
  # so the overlay DLL needs no files next to it.
  set(_demo_fonts Monocraft PressStart2P-Regular JetBrainsMono-Regular Orbitron-Variable Rajdhani-Medium
                  VarelaRound-Regular)
  set(_demo_font_sources)
  foreach(_name IN LISTS _demo_fonts)
    string(MAKE_C_IDENTIFIER "kDemoFont_${_name}" _symbol)
    set(_ttf "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/${_name}.ttf")
    set(_out "${DRIZZY_GENERATED_DIR}/demo_font_${_name}.cpp")
    add_custom_command(
      OUTPUT "${_out}"
      COMMAND drizzy_bin2c "${_ttf}" "${_out}" ${_symbol}
      DEPENDS drizzy_bin2c "${_ttf}"
      COMMENT "Embedding ${_name}.ttf"
      VERBATIM)
    list(APPEND _demo_font_sources "${_out}")
  endforeach()

  # Sample code shared by the sandbox and the overlay DLL: the widget gallery, its animated backdrops and fonts.
  add_library(drizzy_demo_common STATIC
    samples/common/demo_gallery.h
    samples/common/demo_gallery.cpp
    samples/common/demo_backdrops.h
    samples/common/demo_backdrops.cpp
    samples/common/demo_fonts.h
    samples/common/demo_fonts.cpp
    ${_demo_font_sources})
  target_include_directories(drizzy_demo_common PUBLIC samples/common)
  target_link_libraries(drizzy_demo_common PUBLIC drizzy_core PRIVATE drizzy_options)
  # Like the samples' own code: an application's code, not part of the library's whole-program optimization.
  set_property(TARGET drizzy_demo_common PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)

  add_executable(drizzy_sandbox
    samples/sandbox/main.cpp
    samples/sandbox/host.h
    samples/sandbox/host_d3d11.cpp
    samples/sandbox/host_d3d12.cpp
    samples/sandbox/scenes.h
    samples/sandbox/scenes.cpp
    samples/sandbox/ui_demo.h
    samples/sandbox/ui_demo.cpp
    samples/common/keybinds.h
    samples/common/keybinds.cpp
    samples/sandbox/png_writer.h)
  target_compile_definitions(drizzy_sandbox PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_include_directories(drizzy_sandbox PRIVATE samples/common)
  target_link_libraries(drizzy_sandbox PRIVATE drizzy_demo_common drizzy_d3d11 drizzy_d3d12 dxgi drizzy_options)
  set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT drizzy_sandbox)
  # The sandbox's own code (the benchmark scenes) is compiled without /GL, like a game DLL that just links the
  # libraries: the libraries are still optimized together at link time, but benchmark numbers are not skewed by
  # link-time inlining reshaping the benchmark's loops (it measurably slowed the text stress scene).
  set_property(TARGET drizzy_sandbox PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)

  # Injectable overlay DLL: hooks a game's swap chain (VMT) and draws the gallery, on D3D11 and D3D12 with one build.
  # drizzy itself installs no hooks; this sample does. drizzy_overlay_dll_test is a stand-in game that loads the DLL.
  add_library(drizzy_overlay_dll SHARED
    samples/overlay_dll/overlay_dll.cpp
    samples/overlay_dll/vmt_hook.h)
  target_compile_definitions(drizzy_overlay_dll PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_include_directories(drizzy_overlay_dll PRIVATE samples/common)
  target_link_libraries(drizzy_overlay_dll PRIVATE drizzy_demo_common drizzy_d3d11 drizzy_d3d12 dxgi d3d11 d3d12
                                                   drizzy_options)
  # A game DLL is linked into a process it does not control: its own code is not whole-program-optimized with the host.
  set_property(TARGET drizzy_overlay_dll PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)

  add_executable(drizzy_overlay_dll_test samples/overlay_dll/inject_test.cpp samples/sandbox/png_writer.h)
  target_compile_definitions(drizzy_overlay_dll_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_include_directories(drizzy_overlay_dll_test PRIVATE samples/sandbox)
  target_link_libraries(drizzy_overlay_dll_test PRIVATE dxgi d3d11 d3d12 drizzy_options)
  add_dependencies(drizzy_overlay_dll_test drizzy_overlay_dll)  # the test loads the DLL at runtime
endif()
