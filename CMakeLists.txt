cmake_minimum_required(VERSION 3.16)
project(yadrovad VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(YADROVAD_BUILD_PLAYER "Build the Qt 6 player" ON)
option(YADROVAD_BUILD_TESTS  "Build the plugin tests" ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.20 gstreamer-base-1.0>=1.20)

# Плагин кладём в отдельную папку сборки: GST_PLUGIN_PATH=build/gst-plugins
set(YADROVAD_PLUGIN_BUILD_DIR ${CMAKE_BINARY_DIR}/gst-plugins)

# libfvad (WebRTC VAD) — сторонний код, без изменений
add_subdirectory(third_party/libfvad/src EXCLUDE_FROM_ALL)

add_subdirectory(plugin)
add_subdirectory(tools)

if(YADROVAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(YADROVAD_BUILD_PLAYER)
    add_subdirectory(player)
endif()
