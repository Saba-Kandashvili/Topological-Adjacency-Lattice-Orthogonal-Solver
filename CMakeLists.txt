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

file(GLOB CGSME_SOURCES
    "generator.c"
    "cgsme_debug.c"
    "cgsme_utils.c"
    "cgsme_noise.c"
    "cgsme_topology.c"
    "cgsme_solver.c"
    "tinycthread/tinycthread.c"
)

add_library(${PROJECT_NAME} SHARED ${CGSME_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tinycthread"
)

# Auto-export every public symbol from the DLL even though the sources
# have no __declspec(dllexport). Works for MSVC and MinGW.
set_target_properties(${PROJECT_NAME} PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

# Threading:
# - Windows: tinycthread's Win32 backend needs no extra thread library, so
#   skip find_package(Threads) entirely. Linking it on MinGW would pull in
#   libwinpthread-1.dll unless special-cased, which is one more thing to
#   get wrong, so just don't link it.
# - Everywhere else (Linux, macOS): pthread is actually required.
if (NOT WIN32)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
endif()

# Fully static MinGW toolchain runtime -> no dependency on
# libgcc_s_seh-1.dll / libwinpthread-1.dll / libstdc++-6.dll at load time.
if (MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE
        -static
        -static-libgcc
    )
endif()
