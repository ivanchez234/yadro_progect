pkg_check_modules(GST_CHECK REQUIRED IMPORTED_TARGET gstreamer-check-1.0)

add_executable(test_yadrovad test_yadrovad.cpp)
target_link_libraries(test_yadrovad PRIVATE PkgConfig::GST PkgConfig::GST_CHECK)
add_dependencies(test_yadrovad gstyadrovad)

set(YADROVAD_TESTS
    all_frames_processed
    three_phrases_survive
    pts_contiguous
    discont_only_on_splices
    hangover_frames
    silence_only
    input_discont_drops_partial_frame
    flush_and_segment
    property_change_while_streaming)

foreach(name IN LISTS YADROVAD_TESTS)
    add_test(NAME yadrovad.${name} COMMAND test_yadrovad ${name})
    set_tests_properties(yadrovad.${name} PROPERTIES
        TIMEOUT 60
        # Только наш плагин и свой реестр: тест не зависит от системных настроек
        ENVIRONMENT "GST_PLUGIN_PATH=${YADROVAD_PLUGIN_BUILD_DIR};GST_REGISTRY=${CMAKE_CURRENT_BINARY_DIR}/registry.bin;GST_REGISTRY_FORK=no")
endforeach()

# Сквозной тест через настоящий конвейер (нужны wavparse и playbin из
# gstreamer1.0-plugins-good/base и Python 3)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME e2e.cli_wav
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e_cli.py $<TARGET_FILE:yadrovad_cli>)
    set_tests_properties(e2e.cli_wav PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "GST_PLUGIN_PATH=${YADROVAD_PLUGIN_BUILD_DIR}")
endif()
