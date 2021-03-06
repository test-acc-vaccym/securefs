cmake_minimum_required(VERSION 2.8.12 FATAL_ERROR)

option(PORTABLE_BUILD "Build for the general architecture instead of this cpu" OFF)

project(securefs)

set (CMAKE_FIND_FRAMEWORK NEVER)

if (UNIX)
    find_path(FUSE_INCLUDE_DIR fuse.h PATHS /usr/local/include PATH_SUFFIXES osxfuse)
    if (APPLE)
        find_library(FUSE_LIBRARIES osxfuse PATHS /usr/local/lib)
    else()
        find_library(FUSE_LIBRARIES fuse PATHS /usr/local/lib)
    endif()
    include_directories(${FUSE_INCLUDE_DIR})
    link_libraries(${FUSE_LIBRARIES})
    add_compile_options(-Wall -Wextra -Wno-unknown-pragmas -std=gnu++11)

    if (NOT PORTABLE_BUILD)
        add_compile_options(-march=native -mtune=native)
    endif ()

    if (APPLE)
        link_libraries(-Wl,-dead_strip)
    else ()
        add_compile_options(-pthread)
        link_libraries(-pthread)
    endif ()
    link_libraries(${CMAKE_DL_LIBS})
else ()
    add_definitions(-DNOMINMAX=1)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS=1)
    add_definitions(-D__STDC__=1)
    if (NOT WINFSP_PREFIX)
        message("WINFSP_PREFIX not set, fallback to default value")
        set(WINFSP_PREFIX "C:/Program Files (x86)/WinFsp")
    endif ()
    if (${CMAKE_SIZEOF_VOID_P} EQUAL 8)
        set(ARCH x64)
    else ()
        set(ARCH x86)
    endif ()
    set(FUSE_INCLUDE_DIR ${WINFSP_PREFIX}/inc/fuse)
    include_directories(${WINFSP_PREFIX}/inc)
    include_directories(${FUSE_INCLUDE_DIR})
    link_libraries(${WINFSP_PREFIX}/lib/winfsp-${ARCH}.lib)
    link_libraries(-DELAYLOAD:winfsp-${ARCH}.dll)
    link_libraries(delayimp.lib)
    add_compile_options(/MP)
endif ()

add_definitions(-D_REENTRANT -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=28)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

include_directories(sources)
set(EXTERNAL_DIR external)
include_directories(${EXTERNAL_DIR})

set(CRYPTOPP_DIR ${EXTERNAL_DIR}/cryptopp)
add_subdirectory(${CRYPTOPP_DIR})
link_libraries(cryptopp-static)

file(GLOB SOURCES sources/*.cpp sources/*.h ${EXTERNAL_DIR}/*.h ${EXTERNAL_DIR}/*.hpp ${EXTERNAL_DIR}/*.cpp)
file(GLOB TEST_SOURCES test/*.cpp)
add_library(securefs-static STATIC ${SOURCES})
link_libraries(securefs-static)

add_executable(securefs main.cpp)
add_executable(securefs_test ${TEST_SOURCES})

if (UNIX)
    set (CMAKE_REQUIRED_FLAGS "-std=gnu++11")
endif()

include(CheckCXXSourceRuns)
CHECK_CXX_SOURCE_RUNS("int main() { thread_local int i = 0; return i; }" HAS_THREAD_LOCAL)

CHECK_CXX_SOURCE_RUNS("
#include <time.h>

int main() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return 0;
}
" HAS_CLOCK_GETTIME)

CHECK_CXX_SOURCE_RUNS("
#include <unistd.h>
#include <sys/stat.h>

int main() {
    futimens(-1, nullptr);
    return 0;
}
" HAS_FUTIMENS)

CHECK_CXX_SOURCE_RUNS("
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

int main() {
    utimensat(-1, nullptr, nullptr, 0);
    return 0;
}
" HAS_UTIMENSAT)

configure_file(sources/securefs_config.in securefs_config.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

enable_testing()
add_test(NAME sec_test COMMAND securefs_test)
add_test(NAME simple_test COMMAND /usr/bin/env build_config_type=${CMAKE_BUILD_TYPE} /usr/bin/python ${PROJECT_SOURCE_DIR}/test/simple_test.py)

install(TARGETS securefs DESTINATION bin)
