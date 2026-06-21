project(CGSME)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(GLOB CGSME_SOURCES
    "generator.c"
    "cgsme_debug.c"
    "cgsme_utils.c"
    "cgsme_noise.c"
    "cgsme_topology.c"
    "cgsme_solver.c"
)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CGSME_SOURCES
        "tinycthread/tinycthread.c"
    )
endif()

add_library(${PROJECT_NAME} SHARED ${CGSME_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC 
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tinycthread"
)

# Proper cross-platform threads (static where possible)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
