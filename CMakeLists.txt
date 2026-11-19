cmake_minimum_required(VERSION 3.15)
project(CGSME C)

# needed for CMAKE_MSVC_RUNTIME_LIBRARY
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# static msvc runtime so no vcruntime dll dependency
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# override with -DCGSME_TILESET_FILE=/path/to/custom.json
set(CGSME_TILESET_FILE "${CMAKE_CURRENT_SOURCE_DIR}/tileset.json"
    CACHE FILEPATH "Path to the tileset JSON ruleset baked into C at build time")
get_filename_component(CGSME_TILESET_ABS "${CGSME_TILESET_FILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

option(CGSME_BUILD_CLI "Build the standalone debug_gen CLI executable" OFF)

# build-time tileset codegen
set(CGSME_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CGSME_BAKED_HEADER "${CGSME_GENERATED_DIR}/cgsme_tileset_baked.h")
file(MAKE_DIRECTORY "${CGSME_GENERATED_DIR}")

# rebake if tileset path changed
if(DEFINED CGSME_BAKED_SOURCE AND NOT CGSME_BAKED_SOURCE STREQUAL CGSME_TILESET_ABS)
    file(REMOVE "${CGSME_BAKED_HEADER}")
endif()
set(CGSME_BAKED_SOURCE "${CGSME_TILESET_ABS}" CACHE INTERNAL "Last baked tileset JSON path")

if(CMAKE_CROSSCOMPILING OR (WIN32 AND NOT CMAKE_HOST_WIN32))
    if(NOT CGSME_CODEGEN_EXECUTABLE)
        find_program(CGSME_CODEGEN_EXECUTABLE cgsme_codegen)
    endif()
    if(CGSME_CODEGEN_EXECUTABLE)
        set(CGSME_CODEGEN_CMD "${CGSME_CODEGEN_EXECUTABLE}" "${CGSME_TILESET_ABS}" "${CGSME_BAKED_HEADER}")
        set(CGSME_CODEGEN_DEPS "")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_tileset_baked.h")
        message(STATUS "Cross-compiling without host cgsme_codegen: using pre-baked cgsme_tileset_baked.h")
        set(CGSME_CODEGEN_CMD "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_tileset_baked.h" "${CGSME_BAKED_HEADER}")
        set(CGSME_CODEGEN_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_tileset_baked.h")
    else()
        message(FATAL_ERROR
            "Cross-compiling requires either pre-baked cgsme_tileset_baked.h or "
            "-DCGSME_CODEGEN_EXECUTABLE=/path/to/cgsme_codegen")
    endif()
else()
    add_executable(cgsme_codegen EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_codegen.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_ruleset.c"
    )
    set_target_properties(cgsme_codegen PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/codegen_tool"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/codegen_tool"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/codegen_tool"
    )
    target_include_directories(cgsme_codegen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT WIN32)
        target_compile_definitions(cgsme_codegen PRIVATE _GNU_SOURCE)
    endif()
    if(MSVC)
        target_compile_definitions(cgsme_codegen PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set(CGSME_CODEGEN_CMD $<TARGET_FILE:cgsme_codegen> "${CGSME_TILESET_ABS}" "${CGSME_BAKED_HEADER}")
    set(CGSME_CODEGEN_DEPS cgsme_codegen)
endif()

add_custom_command(
    OUTPUT "${CGSME_BAKED_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CGSME_GENERATED_DIR}"
    COMMAND ${CGSME_CODEGEN_CMD}
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

# shared lib (.dll / .so / .dylib)
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

# put generated header dir first so old headers in repo root dont shadow it
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

# export all symbols on windows without __declspec(dllexport)
set_target_properties(${PROJECT_NAME} PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

# windows uses win32 threads directly; linux/macos need pthread + libm
if(NOT WIN32)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

    find_library(CGSME_MATH_LIBRARY m)
    if(CGSME_MATH_LIBRARY)
        target_link_libraries(${PROJECT_NAME} PRIVATE "${CGSME_MATH_LIBRARY}")
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
if(CGSME_BUILD_CLI)
    add_executable(debug_gen
        "${CMAKE_CURRENT_SOURCE_DIR}/debug/main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_ruleset.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/cgsme_soft.c"
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
