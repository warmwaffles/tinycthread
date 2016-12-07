cmake_minimum_required(VERSION 3.2 FATAL_ERROR)

project(tinycthread C)

set(CMAKE_C_FLAGS_DEBUG "-std=c11 -Wall -Wextra -g")
set(CMAKE_C_FLAGS_RELEASE "-std=c11 -Wall -Wextra -O3 -flto")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif(NOT CMAKE_BUILD_TYPE)

include(FindPkgConfig)
find_package(Threads REQUIRED)

if (WIN32)
    set(SOURCES src/win/tinycthread.c)
else()
    set(SOURCES src/posix/tinycthread.c)
endif()

include_directories(include)

add_library(tinycthread STATIC ${SOURCES})
target_link_libraries(tinycthread ${CMAKE_THREAD_LIBS_INIT})

target_include_directories(tinycthread PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    PRIVATE src)

export(TARGETS tinycthread FILE tinycthread_config.cmake)
