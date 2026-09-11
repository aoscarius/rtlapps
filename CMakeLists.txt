cmake_minimum_required(VERSION 3.16)
project(rtlapps LANGUAGES C CXX)

set(TARGET_APPTV "rtltv")
set(TARGET_APPRADIO "rtlradio")

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

add_executable(${TARGET_APPTV}
    rtltv/main.cpp
    rtltv/display/crt_display.cpp
    rtltv/display/osd_menu.cpp
)
add_executable(${TARGET_APPRADIO}
    rtlradio/rtlradio.c
)

target_include_directories(${TARGET_APPTV} PRIVATE rtltv)
target_include_directories(${TARGET_APPRADIO} PRIVATE rtlradio)

if(TARGET SDL2::SDL2)
    target_link_libraries(${TARGET_APPTV} PRIVATE SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(${TARGET_APPTV} PRIVATE SDL2::SDL2-static)
else()
    # Legacy find-module style (no imported target provided).
    target_include_directories(${TARGET_APPTV} PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(${TARGET_APPTV} PRIVATE ${SDL2_LIBRARIES})
endif()

target_link_libraries(${TARGET_APPTV} PRIVATE RTLSDR::rtlsdr Threads::Threads)
target_link_libraries(${TARGET_APPRADIO} PRIVATE RTLSDR::rtlsdr m Threads::Threads)

target_compile_options(${TARGET_APPTV} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O2 -Wall -Wextra>
)
target_compile_options(${TARGET_APPRADIO} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O2 -Wall -Wextra>
)

message(STATUS "SDL2 include dirs:  ${SDL2_INCLUDE_DIRS}")
message(STATUS "RTLSDR include dir: ${RTLSDR_INCLUDE_DIR}")
message(STATUS "RTLSDR library:     ${RTLSDR_LIBRARY}")

install(TARGETS ${TARGET_APPTV} RUNTIME DESTINATION bin)
install(TARGETS ${TARGET_APPRADIO} RUNTIME DESTINATION bin)

add_custom_target(run_${TARGET_APPTV}
    COMMAND $<TARGET_FILE:${TARGET_APPTV}>
    DEPENDS ${TARGET_APPTV}
    COMMENT "Starting ${TARGET_APPTV}..."
)

add_custom_target(run_${TARGET_APPRADIO}
    COMMAND $<TARGET_FILE:${TARGET_APPRADIO}> -f 92.6M -v 2.0 -g 280 -v 0.5 | paplay --raw --rate=48000 --format=s16le --channels=1 --latency-msec=100
    DEPENDS ${TARGET_APPRADIO}
    COMMENT "Starting ${TARGET_APPRADIO}..."
)