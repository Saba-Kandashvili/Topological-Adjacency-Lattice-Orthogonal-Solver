cmake_minimum_required(VERSION 3.15)
project(TALOS C)

# needed for CMAKE_MSVC_RUNTIME_LIBRARY
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# static msvc runtime so no vcruntime dll dependency
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# override with -DTALOS_TILESET_FILE=/path/to/custom.json
set(TALOS_TILESET_FILE "${CMAKE_CURRENT_SOURCE_DIR}/tileset.json"
    CACHE FILEPATH "Path to the tileset JSON ruleset baked into C at build time")
get_filename_component(TALOS_TILESET_ABS "${TALOS_TILESET_FILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

option(TALOS_ENABLE_SOFT "Include runtime JSON (--soft) API in the shared library" ON)
option(TALOS_BUILD_CLI "Build the standalone debug_gen CLI executable" OFF)
option(TALOS_BUILD_TESTS "Build the verification and benchmark test runner" OFF)

# build-time tileset codegen
set(TALOS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(TALOS_BAKED_HEADER "${TALOS_GENERATED_DIR}/talos_tileset_baked.h")
file(MAKE_DIRECTORY "${TALOS_GENERATED_DIR}")

# rebake if tileset path changed
if(DEFINED TALOS_BAKED_SOURCE AND NOT TALOS_BAKED_SOURCE STREQUAL TALOS_TILESET_ABS)
    file(REMOVE "${TALOS_BAKED_HEADER}")
endif()
set(TALOS_BAKED_SOURCE "${TALOS_TILESET_ABS}" CACHE INTERNAL "Last baked tileset JSON path")

if(CMAKE_CROSSCOMPILING OR (WIN32 AND NOT CMAKE_HOST_WIN32))
    if(NOT TALOS_CODEGEN_EXECUTABLE)
        find_program(TALOS_CODEGEN_EXECUTABLE talos_codegen)
    endif()
    if(TALOS_CODEGEN_EXECUTABLE)
        set(TALOS_CODEGEN_CMD "${TALOS_CODEGEN_EXECUTABLE}" "${TALOS_TILESET_ABS}" "${TALOS_BAKED_HEADER}")
        set(TALOS_CODEGEN_DEPS "")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/talos_tileset_baked.h")
        message(STATUS "Cross-compiling without host talos_codegen: using pre-baked talos_tileset_baked.h")
        set(TALOS_CODEGEN_CMD "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/talos_tileset_baked.h" "${TALOS_BAKED_HEADER}")
        set(TALOS_CODEGEN_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/talos_tileset_baked.h")
    else()
        message(FATAL_ERROR
            "Cross-compiling requires either pre-baked talos_tileset_baked.h or "
            "-DTALOS_CODEGEN_EXECUTABLE=/path/to/talos_codegen")
    endif()
else()
    add_executable(talos_codegen EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_SOURCE_DIR}/talos_codegen.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/talos_ruleset.c"
    )
    set_target_properties(talos_codegen PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/codegen_tool"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/codegen_tool"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/codegen_tool"
    )
    target_include_directories(talos_codegen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT WIN32)
        target_compile_definitions(talos_codegen PRIVATE _GNU_SOURCE)
    endif()
    if(MSVC)
        target_compile_definitions(talos_codegen PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set(TALOS_CODEGEN_CMD $<TARGET_FILE:talos_codegen> "${TALOS_TILESET_ABS}" "${TALOS_BAKED_HEADER}")
    set(TALOS_CODEGEN_DEPS talos_codegen)
endif()

add_custom_command(
    OUTPUT "${TALOS_BAKED_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${TALOS_GENERATED_DIR}"
    COMMAND ${TALOS_CODEGEN_CMD}
    DEPENDS
        ${TALOS_CODEGEN_DEPS}
        "${TALOS_TILESET_ABS}"
        "${CMAKE_CURRENT_SOURCE_DIR}/talos_codegen.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/talos_ruleset.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/talos_ruleset.h"
    COMMENT "Baking tileset ruleset (${TALOS_TILESET_ABS}) -> talos_tileset_baked.h"
    VERBATIM
)

add_custom_target(talos_bake DEPENDS "${TALOS_BAKED_HEADER}")

# shared lib (.dll / .so / .dylib)
set(TALOS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/generator.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/talos_debug.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/talos_utils.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/talos_noise.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/talos_topology.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/talos_solver.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tinycthread/tinycthread.c"
)

set(TALOS_LIB_SOURCES ${TALOS_SOURCES})
if(TALOS_ENABLE_SOFT)
    list(APPEND TALOS_LIB_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/talos_ruleset.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/talos_soft.c"
    )
endif()

add_library(${PROJECT_NAME} SHARED ${TALOS_LIB_SOURCES} "${TALOS_BAKED_HEADER}")
add_dependencies(${PROJECT_NAME} talos_bake)

# put generated header dir first so old headers in repo root dont shadow it
target_include_directories(${PROJECT_NAME} BEFORE PUBLIC "${TALOS_GENERATED_DIR}")
target_include_directories(${PROJECT_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tinycthread"
)
target_compile_definitions(${PROJECT_NAME} PUBLIC TALOS_USE_CMAKE_BAKED)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TALOS_BUILD_DLL)
endif()
if(NOT WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _GNU_SOURCE)
endif()
if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# export all symbols on windows without __declspec(dllexport)
set_target_properties(${PROJECT_NAME} PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

# windows uses win32 threads directly; linux/macos need pthread + libm
if(NOT WIN32)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

    find_library(TALOS_MATH_LIBRARY m)
    if(TALOS_MATH_LIBRARY)
        target_link_libraries(${PROJECT_NAME} PRIVATE "${TALOS_MATH_LIBRARY}")
    endif()
endif()

# static mingw runtime so no extra dlls needed
if(MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE
        -static
        -static-libgcc
    )
endif()

# optional debug_gen cli
if(TALOS_BUILD_CLI)
    add_executable(debug_gen
        "${CMAKE_CURRENT_SOURCE_DIR}/debug/main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/talos_ruleset.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/talos_soft.c"
        ${TALOS_SOURCES}
        "${TALOS_BAKED_HEADER}"
    )
    add_dependencies(debug_gen talos_bake)
    target_include_directories(debug_gen BEFORE PRIVATE "${TALOS_GENERATED_DIR}")
    target_include_directories(debug_gen PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tinycthread"
    )
    target_compile_definitions(debug_gen PRIVATE TALOS_USE_CMAKE_BAKED TALOS_DEBUG)
    if(NOT WIN32)
        target_compile_definitions(debug_gen PRIVATE _GNU_SOURCE)
        target_link_libraries(debug_gen PRIVATE Threads::Threads)
        if(TALOS_MATH_LIBRARY)
            target_link_libraries(debug_gen PRIVATE "${TALOS_MATH_LIBRARY}")
        endif()
    endif()
    if(MSVC)
        target_compile_definitions(debug_gen PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(MINGW)
        target_link_options(debug_gen PRIVATE -static -static-libgcc)
    endif()
endif()

# optional test & benchmark suite
if(TALOS_BUILD_TESTS)
    enable_testing()
    add_executable(talos_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_talos.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/talos_ruleset.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/talos_soft.c"
        ${TALOS_SOURCES}
        "${TALOS_BAKED_HEADER}"
    )
    add_dependencies(talos_tests talos_bake)
    target_include_directories(talos_tests BEFORE PRIVATE "${TALOS_GENERATED_DIR}")
    target_include_directories(talos_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tinycthread"
    )
    target_compile_definitions(talos_tests PRIVATE TALOS_USE_CMAKE_BAKED)
    if(NOT WIN32)
        target_compile_definitions(talos_tests PRIVATE _GNU_SOURCE)
        target_link_libraries(talos_tests PRIVATE Threads::Threads)
        if(TALOS_MATH_LIBRARY)
            target_link_libraries(talos_tests PRIVATE "${TALOS_MATH_LIBRARY}")
        endif()
    endif()
    if(MSVC)
        target_compile_definitions(talos_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(MINGW)
        target_link_options(talos_tests PRIVATE -static -static-libgcc)
    endif()
    add_test(NAME talos_suite COMMAND talos_tests --quick WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
