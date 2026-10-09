find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

qt_add_executable(YadroPlayer
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    resources.qrc
)

target_link_libraries(YadroPlayer PRIVATE Qt6::Widgets PkgConfig::GST)
target_compile_definitions(YadroPlayer PRIVATE
    YADROVAD_PLUGIN_BUILD_DIR="${YADROVAD_PLUGIN_BUILD_DIR}")
target_compile_options(YadroPlayer PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)

# Плеер ищет плагин сам (см. main.cpp), но собирать его нужно вместе с плеером
add_dependencies(YadroPlayer gstyadrovad)

install(TARGETS YadroPlayer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
