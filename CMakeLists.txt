cmake_minimum_required(VERSION 3.15)
project(CGSME C)

# CMAKE_MSVC_RUNTIME_LIBRARY is ignored unless CMP0091 is NEW. Requiring
# CMake >= 3.15 makes that the default, but set it explicitly to be safe.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Statically link the MSVC runtime -> no dependency on vcruntimeXXX.dll /
# msvcpXXX.dll at load time.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Configurable tileset ruleset (override with -DCGSME_TILESET_FILE=/path/to/custom.json)
set(CGSME_TILESET_FILE "${CMAKE_CURRENT_SOURCE_DIR}/tileset.json"
    CACHE FILEPATH "Path to the tileset JSON ruleset baked into C at build time")
get_filename_component(CGSME_TILESET_ABS "${CGSME_TILESET_FILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

option(CGSME_BUILD_CLI "Build the standalone debug_gen CLI executable" OFF)

# ---------------------------------------------------------------------------
# 1. Build-Time Tileset Codegen (tileset.json -> generated/cgsme_tileset_baked.h)
# ---------------------------------------------------------------------------
set(CGSME_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CGSME_BAKED_HEADER "${CGSME_GENERATED_DIR}/cgsme_tileset_baked.h")
file(MAKE_DIRECTORY "${CGSME_GENERATED_DIR}")

# Force a clean rebake if CGSME_TILESET_FILE was switched to a different JSON path
if(DEFINED CGSME_BAKED_SOURCE AND NOT CGSME_BAKED_SOURCE STREQUAL CGSME_TILESET_ABS)
    file(REMOVE "${CGSME_BAKED_HEADER}")
endif()
set(CGSME_BAKED_SOURCE "${CGSME_TILESET_ABS}" CACHE INTERNAL "Last baked tileset JSON path")

if(CMAKE_CROSSCOMPILING)
    if(NOT CGSME_CODEGEN_EXECUTABLE)
        find_program(CGSME_CODEGEN_EXECUTABLE cgsme_codegen)
    endif()
    if(NOT CGSME_CODEGEN_EXECUTABLE)
        message(FATAL_ERROR
            "Cross-compiling requires a host cgsme_codegen executable. "
            "Build it natively and pass -DCGSME_CODEGEN_EXECUTABLE=/path/to/cgsme_codegen")
    endif()
    set(CGSME_CODEGEN_CMD "${CGSME_CODEGEN_EXECUTABLE}")
    set(CGSME_CODEGEN_DEPS "")
else()
    add_executable(cgsme_codegen
        "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_codegen.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_ruleset.c"
    )
    target_include_directories(cgsme_codegen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT WIN32)
        target_compile_definitions(cgsme_codegen PRIVATE _GNU_SOURCE)
    endif()
    if(MSVC)
        target_compile_definitions(cgsme_codegen PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set(CGSME_CODEGEN_CMD $<TARGET_FILE:cgsme_codegen>)
    set(CGSME_CODEGEN_DEPS cgsme_codegen)
endif()

add_custom_command(
    OUTPUT "${CGSME_BAKED_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CGSME_GENERATED_DIR}"
    COMMAND ${CGSME_CODEGEN_CMD} "${CGSME_TILESET_ABS}" "${CGSME_BAKED_HEADER}"
    DEPENDS
        ${CGSME_CODEGEN_DEPS}
        "${CGSME_TILESET_ABS}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_codegen.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_ruleset.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_ruleset.h"
    COMMENT "Baking tileset ruleset (${CGSME_TILESET_ABS}) -> cgsme_tileset_baked.h"
    VERBATIM
)

add_custom_target(cgsme_bake DEPENDS "${CGSME_BAKED_HEADER}")

# ---------------------------------------------------------------------------
# 2. CGSME Shared Library (.dll / .so / .dylib)
# ---------------------------------------------------------------------------
set(CGSME_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/generator.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_debug.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_utils.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_noise.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_topology.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_solver.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tinycthread/tinycthread.c"
)

add_library(${PROJECT_NAME} SHARED ${CGSME_SOURCES} "${CGSME_BAKED_HEADER}")
add_dependencies(${PROJECT_NAME} cgsme_bake)

# Put generated header dir BEFORE source dir and force angle-bracket lookup via
# CGSME_USE_CMAKE_BAKED so stale headers in the repo root never shadow it.
target_include_directories(${PROJECT_NAME} BEFORE PUBLIC "${CGSME_GENERATED_DIR}")
target_include_directories(${PROJECT_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tinycthread"
)
target_compile_definitions(${PROJECT_NAME} PUBLIC CGSME_USE_CMAKE_BAKED)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE cgsme_BUILD_DLL)
endif()
if(NOT WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _GNU_SOURCE)
endif()
if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Auto-export every public symbol from the DLL even though the sources
# have no __declspec(dllexport). Works for MSVC and MinGW.
set_target_properties(${PROJECT_NAME} PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

# Threading and math:
# - Windows: tinycthread's Win32 backend needs no extra thread library, so
#   skip find_package(Threads) entirely. Linking it on MinGW would pull in
#   libwinpthread-1.dll unless special-cased, which is one more thing to
#   get wrong, so just don't link it.
# - Everywhere else (Linux, macOS): pthread + libm (when present) are required.
if(NOT WIN32)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

    find_library(CGSME_MATH_LIBRARY m)
    if(CGSME_MATH_LIBRARY)
        target_link_libraries(${PROJECT_NAME} PRIVATE "${CGSME_MATH_LIBRARY}")
    endif()
endif()

# Fully static MinGW toolchain runtime -> no dependency on
# libgcc_s_seh-1.dll / libwinpthread-1.dll / libstdc++-6.dll at load time.
if(MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE
        -static
        -static-libgcc
    )
endif()

# ---------------------------------------------------------------------------
# 3. Optional Standalone CLI Runner (debug_gen)
# ---------------------------------------------------------------------------
if(CGSME_BUILD_CLI)
    add_executable(debug_gen
        "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
        ${CGSME_SOURCES}
        "${CGSME_BAKED_HEADER}"
    )
    add_dependencies(debug_gen cgsme_bake)
    target_include_directories(debug_gen BEFORE PRIVATE "${CGSME_GENERATED_DIR}")
    target_include_directories(debug_gen PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tinycthread"
    )
    target_compile_definitions(debug_gen PRIVATE CGSME_USE_CMAKE_BAKED cgsme_DEBUG)
    if(NOT WIN32)
        target_compile_definitions(debug_gen PRIVATE _GNU_SOURCE)
        target_link_libraries(debug_gen PRIVATE Threads::Threads)
        if(CGSME_MATH_LIBRARY)
            target_link_libraries(debug_gen PRIVATE "${CGSME_MATH_LIBRARY}")
        endif()
    endif()
    if(MSVC)
        target_compile_definitions(debug_gen PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(MINGW)
        target_link_options(debug_gen PRIVATE -static -static-libgcc)
    endif()
endif()
