add_library(gstyadrovad MODULE gst_yadro_vad.cpp gst_yadro_vad.h)

target_link_libraries(gstyadrovad PRIVATE PkgConfig::GST fvad)
target_compile_definitions(gstyadrovad PRIVATE PACKAGE="yadrovad")
target_compile_options(gstyadrovad PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wno-unused-parameter>)

set_target_properties(gstyadrovad PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${YADROVAD_PLUGIN_BUILD_DIR})

install(TARGETS gstyadrovad
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0)
