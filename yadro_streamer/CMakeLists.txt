cmake_minimum_required(VERSION 3.10)
project(yadro_streamer)

set(CMAKE_CXX_STANDARD 17)

# Ищем GStreamer
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0)


# Плагин VAD (Для Qt)
file(GLOB_RECURSE VAD_SOURCES "src/libfvad/src/*.c")
add_library(gstyadrovad SHARED src/gst_yadro_vad.cpp ${VAD_SOURCES})

target_include_directories(gstyadrovad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/libfvad/include)
target_include_directories(gstyadrovad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/libfvad/src)
target_link_libraries(gstyadrovad PRIVATE PkgConfig::GST)


add_executable(vad_tester src/vad_tester.cpp)
target_link_libraries(vad_tester PRIVATE PkgConfig::GST)