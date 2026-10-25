cmake_minimum_required(VERSION 3.20)
project(psprecomp_runtime CXX C)

# VFPU: PPSSPP's interpreter vendored in src/vfpu_ppsspp (see its README.md); psp_vfpu_ops.cpp is the
# API the recompiled code calls. Used by the runtime and by the host VFPU tests.
set(PSP_VFPU_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/psp_vfpu_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/psp_vfpu_ops.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vfpu_ppsspp/psp_vfpu_support.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vfpu_ppsspp/Core/MIPS/InterpreterVFPU.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vfpu_ppsspp/Core/MIPS/MIPSVFPUUtils.cpp
)
set_source_files_properties(${PSP_VFPU_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-fno-strict-aliasing;-w"
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/vfpu_ppsspp")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find SDL2 via pkg-config (Homebrew installs pkg-config file)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
# FreeType renders the sceLibFont (sceFont*) HLE glyphs
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)

# Savedata-dialog fonts (OFL: Cinzel + Crimson Text) are embedded in the binary.
set(PSP_UI_FONTS_CPP ${CMAKE_CURRENT_BINARY_DIR}/gen/psp_ui_fonts.cpp)
file(GLOB PSP_UI_FONT_FILES ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.ttf)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PSP_UI_FONT_FILES}
             ${CMAKE_CURRENT_SOURCE_DIR}/assets/embed_fonts.cmake)
execute_process(COMMAND ${CMAKE_COMMAND}
    -DFONT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts -DOUT=${PSP_UI_FONTS_CPP}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/assets/embed_fonts.cmake)

# Optional: libavcodec for ATRAC3/ATRAC3+ decoding (sceAtrac HLE). Without it
# the sceAtrac HLE keeps its bookkeeping but decodes to silence.
pkg_check_modules(AVCODEC IMPORTED_TARGET libavcodec libavutil)

# Find OpenGL
find_package(OpenGL REQUIRED)

# Find zlib (system zlib on macOS; pkg-config-discoverable on Linux)
find_package(ZLIB REQUIRED)

# ── Recompiled game output directory (issue #47 Phase 1) ────────────────────
# The runtime is game-agnostic: it builds against ANY `psprecomp recompile`
# output directory. Default is the in-repo layout (../output). Another output dir:
#   cmake -B build -S runtime -DPSPRECOMP_OUTPUT_DIR=/abs/path/output
set(PSPRECOMP_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../output" CACHE PATH
    "Recompiled game output directory (psprecomp recompile -o <dir>)")
get_filename_component(PSPRECOMP_OUTPUT_DIR "${PSPRECOMP_OUTPUT_DIR}" ABSOLUTE)
message(STATUS "psprecomp: output dir = ${PSPRECOMP_OUTPUT_DIR}")

if(NOT EXISTS "${PSPRECOMP_OUTPUT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "PSPRECOMP_OUTPUT_DIR does not contain a recompiled game "
        "(${PSPRECOMP_OUTPUT_DIR}/CMakeLists.txt not found).\n"
        "Generate it first:\n"
        "    PSPRECOMP_CROSS_MID=1 cargo run --release -- recompile "
        "<analysis.json> -o ${PSPRECOMP_OUTPUT_DIR}\n"
        "or point at an existing output:\n"
        "    cmake ... -DPSPRECOMP_OUTPUT_DIR=/abs/path/to/output")
endif()

# ── Build fingerprint staleness check (issue #36) ───────────────────────────
# output/ is gitignored; building against a stale output/ after an emitter
# change silently invalidates all conclusions. At configure time, recompute
# the emitter-source content hash and compare with output/fingerprint.json
# (written by `psprecomp recompile`). Recipe: crates/psp-cli/src/fingerprint.rs
# and runtime/cmake/check_fingerprint.py (kept in sync; recipe version 1).
option(PSPRECOMP_ALLOW_STALE
    "Allow configuring against a stale output/ (fingerprint mismatch becomes a warning)" OFF)

set(PSPRECOMP_REGEN_CMD
    "PSPRECOMP_CROSS_MID=1 cargo run --release -- recompile analysis.json --config games/<id>/game.toml -o ${PSPRECOMP_OUTPUT_DIR}")

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_fingerprint.py
            --fingerprint ${PSPRECOMP_OUTPUT_DIR}/fingerprint.json
            --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/..
        RESULT_VARIABLE PSPRECOMP_FP_RESULT
        OUTPUT_VARIABLE PSPRECOMP_FP_OUTPUT
        ERROR_VARIABLE PSPRECOMP_FP_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(PSPRECOMP_FP_RESULT EQUAL 0)
        message(STATUS "${PSPRECOMP_FP_OUTPUT}")
    elseif(PSPRECOMP_FP_RESULT EQUAL 2)
        if(PSPRECOMP_ALLOW_STALE)
            message(WARNING
                "BUILDING AGAINST STALE output/ (PSPRECOMP_ALLOW_STALE=ON):\n"
                "${PSPRECOMP_FP_OUTPUT}\n"
                "Runtime behavior will NOT reflect the current emitter sources.")
        else()
            message(FATAL_ERROR
                "output/ is stale relative to the emitter sources — run:\n"
                "    ${PSPRECOMP_REGEN_CMD}\n"
                "${PSPRECOMP_FP_OUTPUT}\n"
                "(override with -DPSPRECOMP_ALLOW_STALE=ON at your own risk)")
        endif()
    elseif(PSPRECOMP_FP_RESULT EQUAL 3)
        message(WARNING
            "output/fingerprint.json not found (output/ predates issue #36). "
            "Staleness cannot be checked — regenerate to enable it:\n"
            "    ${PSPRECOMP_REGEN_CMD}")
    else()
        message(WARNING
            "Fingerprint check could not verify output/ freshness "
            "(exit ${PSPRECOMP_FP_RESULT}):\n"
            "${PSPRECOMP_FP_OUTPUT}${PSPRECOMP_FP_ERROR}")
    endif()
else()
    message(WARNING
        "Python3 not found — skipping the output/ staleness check (issue #36).")
endif()

# Build the recompiler output as a subdirectory.
# The generated CMakeLists.txt produces lib{module_name}_recomp.a and the
# stable alias psp::recomp that the runtime links (issue #47 Phase 1).
add_subdirectory(${PSPRECOMP_OUTPUT_DIR} ${CMAKE_CURRENT_BINARY_DIR}/recomp_build)

if(NOT TARGET psp::recomp)
    message(FATAL_ERROR
        "${PSPRECOMP_OUTPUT_DIR} does not define the psp::recomp ALIAS — "
        "it was generated by a psprecomp older than issue #47 Phase 1. "
        "Regenerate it:\n    ${PSPRECOMP_REGEN_CMD}")
endif()

# Runtime sources
set(RUNTIME_SOURCES
    src/main.cpp
    src/psp_memory.cpp
    src/psp_dispatch.cpp
    src/psp_fn_watch.cpp
    src/psp_scheduler.cpp
    src/psp_render_queue.cpp
    src/psp_event_loop.cpp
    src/psp_ge.cpp
    src/psp_ge_vertex.cpp
    src/psp_ge_texture.cpp
    src/psp_tex_pack.cpp
    src/psp_fx_manifest.cpp
    src/psp_fx_shader.cpp
    src/psp_fep_log.cpp
    src/psp_ge_shader.cpp
    src/psp_ge_draw.cpp
    src/psp_ge_viewport.cpp
    src/psp_ge_test.cpp
    src/hle/psp_hle_dispatch.cpp
    src/hle/psp_hle_kernel_thread.cpp
    src/hle/psp_hle_kernel_memory.cpp
    src/hle/psp_hle_kernel_sema.cpp
    src/hle/psp_hle_kernel_mutex.cpp
    src/hle/psp_hle_kernel_lwmutex.cpp
    src/hle/psp_hle_kernel_eventflag.cpp
    src/hle/psp_hle_io.cpp
    src/hle/psp_hle_display.cpp
    src/hle/psp_hle_ge.cpp
    src/hle/psp_hle_power.cpp
    src/hle/psp_hle_ctrl.cpp
    src/hle/psp_hle_utility.cpp
    src/hle/psp_savedata_crypto.cpp
    src/hle/psp_savedata_dialog.cpp
    ${PSP_UI_FONTS_CPP}
    src/hle/psp_hle_font.cpp
    src/hle/psp_hle_sas.cpp
    src/hle/psp_hle_atrac.cpp
    src/hle/psp_hle_mpeg.cpp
    src/hle/psp_audio_sink.cpp
    ${PSP_VFPU_SOURCES}
    src/glad.c
    src/psp_debug_socket.cpp
    src/psp_gl_stats.cpp
)

# ── Game-module selection (issues #46/#47 Phase 4) ──────────────────────────
# Everything address-keyed or title-specific lives in games/<id>/runtime/,
# compiled in only when selected here (exactly one strong psp_game_module()
# definition per build). Default "dissidia"; "none" builds the pure generic
# runtime with ZERO game hooks compiled in or registered.
#   cmake -B runtime/build -S runtime                       # Dissidia (default)
#   cmake -B runtime/build-none -S runtime -DPSPRECOMP_GAME=none   # generic
set(PSPRECOMP_GAME "dissidia" CACHE STRING
    "games/<id> module to compile in ('none' = generic build, zero game hooks)")
if(PSPRECOMP_GAME STREQUAL "none" OR PSPRECOMP_GAME STREQUAL "")
    list(APPEND RUNTIME_SOURCES src/psp_game_default.cpp)
    message(STATUS "psprecomp: game module = (none — generic build)")
else()
    # CONFIGURE_DEPENDS: newly added hook files are picked up without a manual
    # reconfigure (plan risk R7 — same trap class as the batch_*.cpp glob).
    file(GLOB PSP_GAME_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/../games/${PSPRECOMP_GAME}/runtime/*.cpp)
    if(NOT PSP_GAME_SOURCES)
        message(FATAL_ERROR
            "games/${PSPRECOMP_GAME}/runtime/ has no sources — unknown game id "
            "'${PSPRECOMP_GAME}' (use -DPSPRECOMP_GAME=none for a generic build).")
    endif()
    list(APPEND RUNTIME_SOURCES ${PSP_GAME_SOURCES})
    # Game-private headers (games/<id>/runtime/*.h) resolve only when the game module is compiled in — the
    # generic build cannot even include them by construction.
    set(PSP_GAME_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../games/${PSPRECOMP_GAME}/runtime)
    message(STATUS "psprecomp: game module = ${PSPRECOMP_GAME}")
endif()

add_executable(psprecomp_runtime ${RUNTIME_SOURCES})

target_include_directories(psprecomp_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h
    ${PSPRECOMP_OUTPUT_DIR}          # funcs.h
    ${SDL2_INCLUDE_DIRS}
    ${PSP_GAME_INCLUDE_DIR}          # game-private headers (empty when generic)
)

# ── Code overlays (optional) ─────────────────────────────────────────────────
# A game that swaps code overlays at runtime passes the directories of its
# extra recompiled function sets (one per overlay, produced with a symbol
# prefix so they can coexist — see games/dissidia/tools/build_overlay.py).
# Each becomes its own static library of batch_*.cpp + dispatch.cpp +
# mid_entries.cpp; the game module registers a resolver that picks between
# them (psp_dispatch_set_overlay_resolver).
set(PSPRECOMP_OVERLAY_DIRS "" CACHE STRING
    "Semicolon-separated overlay output dirs (prefixed recompiled function sets)")
set(PSP_OVERLAY_LIBS "")
foreach(ovl_dir ${PSPRECOMP_OVERLAY_DIRS})
    get_filename_component(ovl_dir "${ovl_dir}" ABSOLUTE)
    get_filename_component(ovl_name "${ovl_dir}" NAME)
    file(GLOB OVL_SOURCES CONFIGURE_DEPENDS "${ovl_dir}/generated/batch_*.cpp")
    list(APPEND OVL_SOURCES "${ovl_dir}/dispatch.cpp" "${ovl_dir}/mid_entries.cpp")
    # A --module set (a PRX the runtime loads itself) also carries its data
    # sections and import-stub table, renamed under its prefix.
    if(EXISTS "${ovl_dir}/overlay.txt")
        file(READ "${ovl_dir}/overlay.txt" OVL_META)
        if(OVL_META MATCHES "^0 ")
            list(APPEND OVL_SOURCES "${ovl_dir}/data_sections.cpp" "${ovl_dir}/syscall_table.cpp")
        endif()
    endif()
    add_library(psp_overlay_${ovl_name} STATIC ${OVL_SOURCES})
    # The overlay's own funcs.h first; recomp.h and friends from the main output.
    target_include_directories(psp_overlay_${ovl_name} PRIVATE
        "${ovl_dir}" "${PSPRECOMP_OUTPUT_DIR}/include" "${PSPRECOMP_OUTPUT_DIR}")
    target_compile_options(psp_overlay_${ovl_name} PRIVATE -O2 -fno-lto -fno-strict-aliasing)
    list(APPEND PSP_OVERLAY_LIBS psp_overlay_${ovl_name})
    message(STATUS "psprecomp: overlay set ${ovl_name} from ${ovl_dir}")
endforeach()

if(AVCODEC_FOUND)
    message(STATUS "psprecomp: libavcodec found (${AVCODEC_VERSION}) -> ATRAC3/ATRAC3+ + sceMpeg (H.264/ATRAC3+) decoding enabled")
    target_compile_definitions(psprecomp_runtime PRIVATE PSPRECOMP_HAVE_AVCODEC)
    target_link_libraries(psprecomp_runtime PRIVATE PkgConfig::AVCODEC)
else()
    message(STATUS "psprecomp: libavcodec NOT found -> sceAtrac decodes silence")
endif()

target_link_libraries(psprecomp_runtime PRIVATE
    ${PSP_OVERLAY_LIBS}
    psp::recomp
    PkgConfig::SDL2
    PkgConfig::FREETYPE
    OpenGL::GL
    ZLIB::ZLIB
    pthread
)

# Suppress OpenGL deprecation warnings on macOS
target_compile_definitions(psprecomp_runtime PRIVATE
    GL_SILENCE_DEPRECATION
)

target_compile_options(psprecomp_runtime PRIVATE
    $<IF:$<CONFIG:Debug>,-O0,-O2>
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# VFPU unit test target (links only VFPU sources, no SDL2/GL dependency)
set(VFPU_TEST_SOURCES
    tests/test_vfpu.cpp
    ${PSP_VFPU_SOURCES}
)

add_executable(test_vfpu ${VFPU_TEST_SOURCES})

target_include_directories(test_vfpu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h
)

target_compile_options(test_vfpu PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_compile_definitions(test_vfpu PRIVATE
    GL_SILENCE_DEPRECATION
)

enable_testing()
add_test(NAME vfpu_tests COMMAND test_vfpu)

# atan2 (FUN_089A457C) host test; also runnable standalone via tests/run_vfpu_atan2.sh
add_executable(test_vfpu_atan2 tests/test_vfpu_atan2.cpp
    ${PSP_VFPU_SOURCES})
target_include_directories(test_vfpu_atan2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${PSPRECOMP_OUTPUT_DIR}/include)
add_test(NAME vfpu_atan2 COMMAND test_vfpu_atan2)

# Savedata crypto / list dialog / sceUtilitySavedata HLE host tests (no
# SDL2/GL; also runnable standalone via tests/run_savedata_*.sh).
add_executable(test_savedata_crypto tests/test_savedata_crypto.cpp src/hle/psp_savedata_crypto.cpp)
add_test(NAME savedata_crypto COMMAND test_savedata_crypto)
add_executable(test_savedata_dialog tests/test_savedata_dialog.cpp src/hle/psp_savedata_dialog.cpp ${PSP_UI_FONTS_CPP})
target_link_libraries(test_savedata_dialog PRIVATE PkgConfig::FREETYPE)
add_test(NAME savedata_dialog COMMAND test_savedata_dialog)
add_executable(test_savedata_hle tests/test_savedata_hle.cpp
    src/hle/psp_savedata_crypto.cpp src/hle/psp_savedata_dialog.cpp ${PSP_UI_FONTS_CPP})
target_include_directories(test_savedata_hle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${PSPRECOMP_OUTPUT_DIR}/include)
target_link_libraries(test_savedata_hle PRIVATE pthread PkgConfig::FREETYPE)
add_test(NAME savedata_hle COMMAND test_savedata_hle)

# Debug socket v2 unit test target (issue #35). Links ONLY the socket
# layer -- the I/S commands reach game state through the PspDebugHooks
# seams, so no SDL2/GL/recomp dependency is needed.
set(DEBUG_SOCKET_TEST_SOURCES
    tests/test_debug_socket.cpp
    src/psp_debug_socket.cpp
)

add_executable(test_debug_socket ${DEBUG_SOCKET_TEST_SOURCES})

target_include_directories(test_debug_socket PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(test_debug_socket PRIVATE
    pthread
)

target_compile_options(test_debug_socket PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME debug_socket_tests COMMAND test_debug_socket)

# Refer*Status layout unit test target. The header under
# test (hle/psp_hle_refer_info.h) is pure layout/encoding logic, so the
# test links nothing beyond its own translation unit (test_vfpu convention:
# no SDL2/GL).
add_executable(test_refer_info tests/test_refer_info.cpp)

target_include_directories(test_refer_info PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h (psp_mem_read/write)
)

target_compile_options(test_refer_info PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME refer_info_tests COMMAND test_refer_info)

# Host-heap allocator unit tests: psp_hle_kernel_memory.cpp is included into the test (file-static
# heap state) and built once per stand-in recomp_game_config.h — no host-heap keys (the historical
# 0x0B000000 cap) and Dissidia's two-region config. The stand-in dir comes first on the
# include path so it shadows the generated header.
foreach(kmem_cfg default two_region)
    set(kmem_tgt test_kmem_${kmem_cfg})
    add_executable(${kmem_tgt} tests/test_kmem.cpp)
    target_include_directories(${kmem_tgt} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/kmem_cfg_${kmem_cfg}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PSPRECOMP_OUTPUT_DIR}/include
    )
    target_compile_options(${kmem_tgt} PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    if(kmem_cfg STREQUAL "two_region")
        target_compile_definitions(${kmem_tgt} PRIVATE KMEM_TEST_TWO_REGION=1)
    endif()
    add_test(NAME kmem_${kmem_cfg} COMMAND ${kmem_tgt})
endforeach()

# GE vertex transform unit test target. Links the real
# psp_ge_vertex.cpp and calls ge_transform_vertices directly to verify
# through-mode UV texel->[0,1] normalization. No SDL2/GL/scheduler deps
# (the function is pure float math); only needs recomp.h for the TU.
add_executable(test_ge_vertex
    tests/test_ge_vertex.cpp
    src/psp_ge_vertex.cpp
)

target_include_directories(test_ge_vertex PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h (psp_mem_read/write)
)

target_compile_options(test_ge_vertex PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME ge_vertex_tests COMMAND test_ge_vertex)

# GE viewport/depth-range mapping unit test (issue #23). Pure math: links
# psp_ge_viewport.cpp only -- no SDL2/GL/scheduler deps.
add_executable(test_ge_viewport
    tests/test_ge_viewport.cpp
    src/psp_ge_viewport.cpp
)

target_include_directories(test_ge_viewport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(test_ge_viewport PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME ge_viewport_tests COMMAND test_ge_viewport)

# FX shader overrides (FX_SHADERS.md): manifest parsing, hash matching, effect
# ages, compile-failure fallback (no GL), plus a check of the shipped manifest.
add_executable(test_fx_shader
    tests/test_fx_shader.cpp
    src/psp_fx_manifest.cpp
)
target_include_directories(test_fx_shader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_fx_shader PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
# With glslangValidator (brew install glslang / apt install glslang-tools) every
# shipped shader is also compiled by the Khronos reference compiler, which is as
# strict as Apple's GL (Mesa accepts things the Mac rejects).
find_program(GLSLANG_VALIDATOR glslangValidator)
if(GLSLANG_VALIDATOR)
    add_test(NAME fx_shader_tests COMMAND test_fx_shader ${CMAKE_CURRENT_SOURCE_DIR}/../work/mods/fx_shaders
             --glslang ${GLSLANG_VALIDATOR})
else()
    message(STATUS "psprecomp: glslangValidator not found - fx_shader_tests lint the shaders without compiling them")
    add_test(NAME fx_shader_tests COMMAND test_fx_shader ${CMAKE_CURRENT_SOURCE_DIR}/../work/mods/fx_shaders)
endif()

# PSPRECOMP_FEP_LOG measuring (psp_fep_log.h): the pure half, no GL / guest memory.
add_executable(test_fep_log
    tests/test_fep_log.cpp
    src/psp_fep_log.cpp
)
target_include_directories(test_fep_log PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(test_fep_log PRIVATE PSP_FEP_LOG_PURE)
target_compile_options(test_fep_log PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME fep_log_tests COMMAND test_fep_log)

