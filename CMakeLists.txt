cmake_minimum_required(VERSION 3.16)
project(pong_tv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Our own Find modules (RTLSDR has none upstream) live in cmake/.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(Threads REQUIRED)

# SDL2 ships its own CMake package config on most platforms (SDL2Config.cmake
# via libsdl2-dev / brew / vcpkg), exposing an SDL2::SDL2 target. Some older
# distro packages only provide sdl2-config.cmake with SDL2_INCLUDE_DIRS /
# SDL2_LIBRARIES variables instead of a target -- handle both.
find_package(SDL2 REQUIRED)

# librtlsdr: our custom module (see cmake/FindRTLSDR.cmake) tries
# pkg-config first, then a manual header/lib search.
find_package(RTLSDR REQUIRED)

add_executable(pong_tv
    src/main.cpp
    src/display/crt_display.cpp
    src/display/osd_menu.cpp
)

target_include_directories(pong_tv PRIVATE src)

if(TARGET SDL2::SDL2)
    target_link_libraries(pong_tv PRIVATE SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(pong_tv PRIVATE SDL2::SDL2-static)
else()
    # Legacy find-module style (no imported target provided).
    target_include_directories(pong_tv PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(pong_tv PRIVATE ${SDL2_LIBRARIES})
endif()

target_link_libraries(pong_tv PRIVATE RTLSDR::rtlsdr Threads::Threads)

target_compile_options(pong_tv PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O2 -Wall -Wextra>
)

message(STATUS "pong_tv: SDL2 include dirs: ${SDL2_INCLUDE_DIRS}")
message(STATUS "pong_tv: RTLSDR include dir: ${RTLSDR_INCLUDE_DIR}")
message(STATUS "pong_tv: RTLSDR library:     ${RTLSDR_LIBRARY}")

install(TARGETS pong_tv RUNTIME DESTINATION bin)
