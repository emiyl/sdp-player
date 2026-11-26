cmake_minimum_required(VERSION 3.16)
project(sdp-sdl LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(LIBAVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(LIBAVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(LIBAVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(LIBSWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(LIBSWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

add_executable(sdp-sdl
    src/main.c
    src/gui.c
    src/network.c
    src/media.c
)

target_include_directories(sdp-sdl PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${LIBAVFORMAT_INCLUDE_DIRS}
    ${LIBAVCODEC_INCLUDE_DIRS}
    ${LIBAVUTIL_INCLUDE_DIRS}
    ${LIBSWSCALE_INCLUDE_DIRS}
    ${LIBSWRESAMPLE_INCLUDE_DIRS}
)

target_link_libraries(sdp-sdl PRIVATE
    PkgConfig::SDL2
    PkgConfig::LIBAVFORMAT
    PkgConfig::LIBAVCODEC
    PkgConfig::LIBAVUTIL
    PkgConfig::LIBSWSCALE
    PkgConfig::LIBSWRESAMPLE
)
