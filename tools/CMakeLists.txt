# Консольный проигрыватель: проверка плагина без GUI
add_executable(yadrovad_cli yadrovad_cli.cpp)
target_link_libraries(yadrovad_cli PRIVATE PkgConfig::GST)
target_compile_definitions(yadrovad_cli PRIVATE
    YADROVAD_PLUGIN_BUILD_DIR="${YADROVAD_PLUGIN_BUILD_DIR}")
