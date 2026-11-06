cmake_minimum_required(VERSION 3.25)
project(TextScope VERSION 0.1.2 LANGUAGES CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(TEXTSCOPE_BUILD_TESTS "Build tests" ON)
option(TEXTSCOPE_BUILD_BENCHMARKS "Build the headless benchmark" ON)
option(TEXTSCOPE_BUILD_EXAMPLE_PLUGIN "Build the streaming parser example" OFF)
option(TEXTSCOPE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(TEXTSCOPE_BUILD_FUZZER "Build the Clang libFuzzer filter parser harness" OFF)
option(TEXTSCOPE_BUNDLE_RUNTIME "Deploy private runtime dependencies for standalone packages" ON)
option(TEXTSCOPE_INSTALL_EXAMPLE_PLUGIN "Install the example parser when built" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent Core5Compat)
find_package(PCRE2 CONFIG QUIET)
if(TARGET PCRE2::16BIT)
    set(TEXTSCOPE_PCRE2_TARGET PCRE2::16BIT)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-16)
    set(TEXTSCOPE_PCRE2_TARGET PkgConfig::PCRE2)
endif()

add_library(textscope_options INTERFACE)
target_compile_definitions(textscope_options INTERFACE TEXTSCOPE_VERSION="${PROJECT_VERSION}")
target_compile_options(textscope_options INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->)
if(TEXTSCOPE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(textscope_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(textscope_options INTERFACE -fsanitize=address,undefined)
endif()

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
add_library(textscope_core ${CORE_SOURCES} src/core/core.hpp)
target_include_directories(textscope_core PUBLIC src)
target_link_libraries(textscope_core PUBLIC Qt6::Core Qt6::Gui Qt6::Core5Compat
    PRIVATE ${TEXTSCOPE_PCRE2_TARGET} textscope_options)

add_library(textscope_persistence src/persistence/persistence.cpp src/persistence/persistence.hpp)
target_link_libraries(textscope_persistence PUBLIC textscope_core PRIVATE textscope_options)

add_library(textscope_sources src/sources/plugins.cpp src/sources/plugins.hpp src/sources/parserplugin.hpp)
target_link_libraries(textscope_sources PUBLIC textscope_core Qt6::Widgets PRIVATE textscope_options)

file(GLOB UI_SOURCES CONFIGURE_DEPENDS src/ui/*.cpp)
add_library(textscope_ui ${UI_SOURCES} src/ui/ui.hpp)
target_link_libraries(textscope_ui PUBLIC textscope_core Qt6::Widgets Qt6::Concurrent PRIVATE textscope_options)

qt_add_executable(textscope MACOSX_BUNDLE
    src/app/main.cpp src/app/session.cpp src/app/session.hpp resources/resources.qrc)
target_link_libraries(textscope PRIVATE textscope_ui textscope_persistence textscope_sources textscope_options)
set_target_properties(textscope PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.deepak-rawat.textscope"
    MACOSX_BUNDLE_BUNDLE_NAME "textscope"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in"
    WIN32_EXECUTABLE TRUE)

add_executable(tscope src/app/launcher.cpp)
target_link_libraries(tscope PRIVATE Qt6::Core textscope_options)
set_target_properties(tscope PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
if(APPLE)
    set_target_properties(tscope PROPERTIES INSTALL_RPATH "@loader_path/../Frameworks")
elseif(UNIX)
    set_target_properties(tscope PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()

if(APPLE)
    target_sources(textscope PRIVATE resources/icons/textscope.icns)
    set_source_files_properties(resources/icons/textscope.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(textscope PROPERTIES MACOSX_BUNDLE_ICON_FILE "textscope.icns")
elseif(WIN32)
    enable_language(RC)
    set(TEXTSCOPE_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/textscope.ico")
    configure_file(packaging/textscope.rc.in "${CMAKE_BINARY_DIR}/textscope.rc" @ONLY)
    set_source_files_properties("${CMAKE_BINARY_DIR}/textscope.rc" PROPERTIES
        OBJECT_DEPENDS "${TEXTSCOPE_WINDOWS_ICON}")
    target_sources(textscope PRIVATE "${CMAKE_BINARY_DIR}/textscope.rc")
endif()

if(TEXTSCOPE_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(launcher_fixture tests/fixtures/launcher_fixture.cpp)
    target_link_libraries(launcher_fixture PRIVATE Qt6::Core textscope_options)
    file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS tests/*_test.cpp)
    foreach(test_source IN LISTS TEST_SOURCES)
        get_filename_component(test_name "${test_source}" NAME_WE)
        add_executable(${test_name} "${test_source}")
        if(test_name STREQUAL "app_test" OR test_name STREQUAL "performance_test")
            target_sources(${test_name} PRIVATE src/app/session.cpp src/app/session.hpp resources/resources.qrc)
        endif()
        if(test_name STREQUAL "app_test")
            add_dependencies(app_test textscope)
            target_compile_definitions(app_test PRIVATE
                TEXTSCOPE_TEST_EXECUTABLE="$<TARGET_FILE:textscope>")
        endif()
        target_link_libraries(${test_name} PRIVATE textscope_ui textscope_persistence textscope_sources Qt6::Test textscope_options)
        if(test_name STREQUAL "core_test")
            target_link_libraries(core_test PRIVATE ${TEXTSCOPE_PCRE2_TARGET})
        endif()
        if(test_name STREQUAL "launcher_test")
            add_dependencies(launcher_test tscope launcher_fixture)
            target_compile_definitions(launcher_test PRIVATE
                TEXTSCOPE_TEST_LAUNCHER="$<TARGET_FILE:tscope>"
                TEXTSCOPE_TEST_LAUNCHER_FIXTURE="$<TARGET_FILE:launcher_fixture>")
            set_target_properties(launcher_test PROPERTIES WIN32_EXECUTABLE FALSE)
        endif()
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-results")
        add_test(NAME ${test_name} COMMAND ${test_name}
            -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/test-results/${test_name}.txt,txt")
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_TO_CONSOLE=1")
        if(test_name STREQUAL "performance_test")
            set_tests_properties(${test_name} PROPERTIES LABELS "performance" TIMEOUT 90)
        endif()
    endforeach()
    add_test(NAME packaging COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/packaging-tests"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/packaging/packaging_test.cmake")
    if(NOT TEXTSCOPE_BUNDLE_RUNTIME AND NOT TEXTSCOPE_INSTALL_EXAMPLE_PLUGIN)
        add_test(NAME managed_install COMMAND "${CMAKE_COMMAND}"
            "-DBINARY=${CMAKE_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
            "-DBINDIR=${CMAKE_INSTALL_BINDIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/packaging/managed_install_test.cmake")
    endif()
endif()

if(TEXTSCOPE_BUILD_BENCHMARKS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmark.cpp")
    add_executable(textscope_benchmark benchmarks/benchmark.cpp)
    target_link_libraries(textscope_benchmark PRIVATE textscope_core textscope_options)
    if(WIN32)
        target_link_libraries(textscope_benchmark PRIVATE psapi)
    endif()
endif()

if(TEXTSCOPE_BUILD_FUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "The parser fuzz harness requires Clang's libFuzzer.")
    endif()
    add_executable(textscope_filter_fuzz tests/fuzz/filter_fuzz.cpp src/persistence/persistence.cpp ${CORE_SOURCES})
    target_include_directories(textscope_filter_fuzz PRIVATE src)
    target_link_libraries(textscope_filter_fuzz PRIVATE Qt6::Core Qt6::Gui Qt6::Core5Compat ${TEXTSCOPE_PCRE2_TARGET})
    target_compile_options(textscope_filter_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(textscope_filter_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

if(TEXTSCOPE_BUILD_EXAMPLE_PLUGIN)
    qt_add_plugin(textscope_demo_parser CLASS_NAME DemoParser
        plugins/demo/demo.cpp plugins/demo/demo.json)
    target_link_libraries(textscope_demo_parser PRIVATE textscope_sources textscope_options)
    set_target_properties(textscope_demo_parser PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
    if(TEXTSCOPE_INSTALL_EXAMPLE_PLUGIN AND APPLE)
        install(TARGETS textscope_demo_parser LIBRARY DESTINATION textscope.app/Contents/PlugIns/textscope)
    elseif(TEXTSCOPE_INSTALL_EXAMPLE_PLUGIN)
        install(TARGETS textscope_demo_parser
            LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins)
    endif()
    if(TARGET plugin_test)
        add_dependencies(plugin_test textscope_demo_parser)
        target_compile_definitions(plugin_test PRIVATE
            TEXTSCOPE_TEST_PLUGIN_DIRECTORY="$<TARGET_FILE_DIR:textscope_demo_parser>")
    endif()
endif()

install(TARGETS textscope BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(APPLE)
    install(TARGETS tscope RUNTIME DESTINATION textscope.app/Contents/MacOS)
    install(DIRECTORY resources/licenses DESTINATION textscope.app/Contents/Resources)
    install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION textscope.app/Contents/Resources)
    install(DIRECTORY docs DESTINATION textscope.app/Contents/Resources)
else()
    install(TARGETS tscope RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TEXTSCOPE_BUNDLE_RUNTIME AND APPLE)
    find_program(TEXTSCOPE_MACDEPLOYQT macdeployqt HINTS "${Qt6_DIR}/../../../bin" REQUIRED)
    find_program(TEXTSCOPE_QMAKE_EXECUTABLE NAMES qmake6 qmake HINTS "${Qt6_DIR}/../../../bin" REQUIRED)
    execute_process(COMMAND "${TEXTSCOPE_QMAKE_EXECUTABLE}" -query QT_INSTALL_PLUGINS
        OUTPUT_VARIABLE TEXTSCOPE_QT_PLUGIN_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${TEXTSCOPE_QMAKE_EXECUTABLE}" -query QT_INSTALL_DATA
        OUTPUT_VARIABLE TEXTSCOPE_QT_DATA_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    find_program(TEXTSCOPE_BREW brew)
    set(TEXTSCOPE_HOMEBREW_PREFIX "")
    if(TEXTSCOPE_BREW)
        execute_process(COMMAND "${TEXTSCOPE_BREW}" --prefix OUTPUT_VARIABLE TEXTSCOPE_HOMEBREW_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    endif()
    configure_file(packaging/DeployMac.cmake.in "${CMAKE_BINARY_DIR}/DeployMac.cmake" @ONLY)
    install(SCRIPT "${CMAKE_BINARY_DIR}/DeployMac.cmake")
elseif(TEXTSCOPE_BUNDLE_RUNTIME)
    qt_generate_deploy_app_script(TARGET textscope OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT "${deploy_script}")
    if(WIN32 AND MINGW)
        get_filename_component(TEXTSCOPE_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_program(TEXTSCOPE_OBJDUMP objdump HINTS "${TEXTSCOPE_COMPILER_BIN}" REQUIRED)
        configure_file(packaging/DeployUcrt.cmake.in "${CMAKE_BINARY_DIR}/DeployUcrt.cmake" @ONLY)
        install(SCRIPT "${CMAKE_BINARY_DIR}/DeployUcrt.cmake")
    endif()
endif()
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/textscope)
install(DIRECTORY resources/licenses docs DESTINATION ${CMAKE_INSTALL_DATADIR}/textscope)
install(FILES packaging/textscope.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/icons/textscope.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps)
install(FILES resources/icons/textscope.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

set(CPACK_PACKAGE_NAME "TextScope")
set(CPACK_PACKAGE_VENDOR "TextScope contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
# Source distributions use the checked, Git-scoped staging in PrepareSource.cmake.
set(CPACK_SOURCE_GENERATOR "")
if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
