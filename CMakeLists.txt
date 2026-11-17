cmake_minimum_required(VERSION 3.16)
project(xsap VERSION 3.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)
include(GNUInstallDirs)

add_executable(xsap
    src/main.cpp
    src/XsapApp.cpp
    src/HelpWindow.cpp
    src/Model.cpp
    src/AudioPlayer.cpp
    src/core/SapFft.cpp
    src/core/SapSpectrogram.cpp
    src/core/SapSpectrogramCache.cpp
    src/core/SapWaveFile.cpp
    src/core/SapParameterFile.cpp
)

target_include_directories(xsap PRIVATE src)
target_link_libraries(xsap PRIVATE PkgConfig::GTKMM Threads::Threads ${CMAKE_DL_LIBS})
target_compile_definitions(xsap PRIVATE
    XSAP_INSTALL_ASSET_DIR="${CMAKE_INSTALL_FULL_DATADIR}/xsap/assets"
    XSAP_VERSION="${PROJECT_VERSION}"
)

target_compile_options(xsap PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

install(TARGETS xsap RUNTIME DESTINATION bin)
install(DIRECTORY assets/ DESTINATION share/xsap/assets)
install(
    FILES data/org.tianwang.xsap.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)
install(
    FILES assets/icons/xsap.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/48x48/apps
    RENAME org.tianwang.xsap.png
)

add_custom_command(TARGET xsap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:xsap>/assets"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(xsap_model_tests
        tests/ModelTests.cpp
        src/Model.cpp
        src/core/SapFft.cpp
    src/core/SapSpectrogram.cpp
    src/core/SapSpectrogramCache.cpp
        src/core/SapWaveFile.cpp
        src/core/SapParameterFile.cpp
    )
    target_include_directories(xsap_model_tests PRIVATE src)
    target_compile_options(xsap_model_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    )
    add_test(NAME xsap_model_tests COMMAND xsap_model_tests)

    add_executable(xsap_fft_bench
        tests/FftBench.cpp
        src/core/SapFft.cpp
    src/core/SapSpectrogram.cpp
    src/core/SapSpectrogramCache.cpp
    )
    target_include_directories(xsap_fft_bench PRIVATE src)
    target_compile_options(xsap_fft_bench PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    )
endif()
