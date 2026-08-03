cmake_minimum_required(VERSION 3.20)
project(nes-emu C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# raylib
find_package(raylib QUIET)
if (NOT raylib_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 6.0
        GIT_SHALLOW 1
    )
    FetchContent_MakeAvailable(raylib)
endif()

add_executable(nes-emu src/main.c)
target_link_libraries(nes-emu PRIVATE raylib)