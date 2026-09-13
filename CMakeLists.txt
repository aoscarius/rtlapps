cmake_minimum_required(VERSION 3.16)
project(rtlapps LANGUAGES C CXX)

include(ExternalProject)

set(TARGET_APPTV "rtltv")
set(TARGET_APPRADIO "rtlradio")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

ExternalProject_Add(
    build_rtllib
    SOURCE_DIR              "${CMAKE_CURRENT_SOURCE_DIR}/rtllib"
    BINARY_DIR              "${CMAKE_CURRENT_SOURCE_DIR}/build/rtllib"
    DOWNLOAD_COMMAND        ""
    UPDATE_COMMAND          ""
    INSTALL_COMMAND         "" 
    CMAKE_ARGS              -DCMAKE_BUILD_TYPE=Release
                            -DINSTALL_UDEV_RULES=ON
                            -DCMAKE_C_FLAGS=-march=native\ -O3
    USES_TERMINAL_CONFIGURE ON
    USES_TERMINAL_BUILD     ON
)

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
add_dependencies(${TARGET_APPTV} build_rtllib)
add_executable(${TARGET_APPRADIO}
    rtlradio/rtlradio.c
)
add_dependencies(${TARGET_APPRADIO} build_rtllib)

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
    COMMAND $<TARGET_FILE:${TARGET_APPTV}> -P -c 4 -r 2400000 -g 95 -L 306 -s 0.68 --play-file pongontv.iq
    DEPENDS ${TARGET_APPTV}
    COMMENT "Starting ${TARGET_APPTV}..."
)

add_custom_target(run_${TARGET_APPRADIO}
    COMMAND $<TARGET_FILE:${TARGET_APPRADIO}> -f 97.9M -g auto -v 0.5 -a | paplay --raw --rate=48000 --format=s16le --channels=1 --latency-msec=100
    DEPENDS ${TARGET_APPRADIO}
    COMMENT "Starting ${TARGET_APPRADIO}..."
)

add_custom_target(install_rtllib
    COMMAND sudo ${CMAKE_COMMAND} --install "${CMAKE_CURRENT_SOURCE_DIR}/build/rtllib"
    DEPENDS build_rtllib
    COMMENT "Install rtllib on your system with root..."
    USES_TERMINAL
)
