cmake_minimum_required(VERSION 3.16)
project(sdp-player LANGUAGES C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(LIBAVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(LIBAVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(LIBAVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(LIBSWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(LIBSWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

add_library(sdp-player-core
    src/player.c
    src/network.c
    src/media.c
    src/profile.c
)

target_include_directories(sdp-player-core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/sdp-player>
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SDL2_INCLUDE_DIRS}
    ${LIBAVFORMAT_INCLUDE_DIRS}
    ${LIBAVCODEC_INCLUDE_DIRS}
    ${LIBAVUTIL_INCLUDE_DIRS}
    ${LIBSWSCALE_INCLUDE_DIRS}
    ${LIBSWRESAMPLE_INCLUDE_DIRS}
)

target_link_libraries(sdp-player-core PUBLIC
    PkgConfig::SDL2
    PkgConfig::LIBAVFORMAT
    PkgConfig::LIBAVCODEC
    PkgConfig::LIBAVUTIL
    PkgConfig::LIBSWSCALE
    PkgConfig::LIBSWRESAMPLE
)

add_executable(sdp-player
    src/main.c
    src/gui.c
)

target_include_directories(sdp-player PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sdp-player PRIVATE
    sdp-player-core
)

if(WIN32)
    if(MINGW)
        target_link_options(sdp-player PRIVATE -mconsole)
    endif()
endif()

set_target_properties(sdp-player-core PROPERTIES
    OUTPUT_NAME sdp-player-core
)

install(TARGETS sdp-player-core sdp-player
    EXPORT sdp-playerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sdp-player
)

install(EXPORT sdp-playerTargets
    FILE sdp-playerTargets.cmake
    NAMESPACE sdp-player::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sdp-player
)
