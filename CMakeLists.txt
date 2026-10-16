cmake_minimum_required(VERSION 3.16)

project(tizen-steam-link VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (EMSCRIPTEN)
    # Every object in a pthreads build needs shared memory and atomics enabled.
    add_compile_options(-s USE_PTHREADS=1)
    # Emscripten's toolchain file updates static archives in place ("emar rc"), so the object
    # of a source removed from a target stays in the archive and can win at link time. That
    # is how ihslib's fcntl-calling UDP backend ended up on the TV. Start every archive from
    # scratch, as CMake does by default elsewhere.
    # The toolchain sets the whole rule (CMAKE_<LANG>_CREATE_STATIC_LIBRARY), which takes
    # precedence over the ARCHIVE_* variables.
    foreach (lang C CXX)
        set(CMAKE_${lang}_CREATE_STATIC_LIBRARY
                "${CMAKE_COMMAND} -E rm -f <TARGET> && <CMAKE_AR> rc <TARGET> <LINK_FLAGS> <OBJECTS>")
    endforeach ()
else ()
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
endif ()

include(cmake/deps.cmake)
include(cmake/ihslib.cmake)

add_subdirectory(src/core)

if (EMSCRIPTEN)
    add_subdirectory(src/tizen)
else ()
    add_subdirectory(src/desktop)
    enable_testing()
    add_subdirectory(tests)
endif ()
