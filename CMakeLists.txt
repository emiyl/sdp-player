cmake_minimum_required(VERSION 3.16)
project(sdp-player LANGUAGES C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(SDL_MAIN_HANDLED=1)

find_package(PkgConfig REQUIRED)

set(ENV{PKG_CONFIG_ARGN} "--static")

if(NOT WIN32)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(LIBAVFORMAT REQUIRED IMPORTED_TARGET libavformat)
    pkg_check_modules(LIBAVCODEC REQUIRED IMPORTED_TARGET libavcodec)
    pkg_check_modules(LIBAVUTIL REQUIRED IMPORTED_TARGET libavutil)
    pkg_check_modules(LIBSWSCALE REQUIRED IMPORTED_TARGET libswscale)
    pkg_check_modules(LIBSWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
else()
    find_package(SDL2 CONFIG REQUIRED)
    find_package(FFmpeg CONFIG REQUIRED)
endif()

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

if(WIN32)
    target_link_libraries(sdp-player-core PUBLIC
        SDL2::SDL2-static
        avformat
        avcodec
        avutil
        swscale
        swresample
    )
else()
    target_link_libraries(sdp-player-core PUBLIC
        PkgConfig::SDL2
        PkgConfig::LIBAVFORMAT
        PkgConfig::LIBAVCODEC
        PkgConfig::LIBAVUTIL
        PkgConfig::LIBSWSCALE
        PkgConfig::LIBSWRESAMPLE
    )
endif()

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

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(WIN32)
    target_link_options(sdp-player PRIVATE -static -static-libgcc)
elseif(UNIX AND NOT APPLE)
endif()

if(WIN32)
    set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_TARGET_TRIPLET}" CACHE STRING "")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(APPLE)
    target_link_libraries(sdp-player-core PUBLIC
        "-framework CoreAudio"
        "-framework AudioUnit"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework Security"
    )
elseif(UNIX)
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
