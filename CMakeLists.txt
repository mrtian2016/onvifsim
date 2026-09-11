cmake_minimum_required(VERSION 3.21)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Version)

project(onvifsim
    VERSION ${ONVIFSIM_VERSION_SHORT}
    DESCRIPTION "ONVIF camera simulator"
    HOMEPAGE_URL "https://github.com/mrtian2016/onvifsim"
    LANGUAGES CXX)

option(BUILD_GUI "Build the Qt Widgets GUI executable" ON)
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_AUDIO_PLAYBACK "Enable local talkback playback (requires QtMultimedia)" OFF)
option(ENABLE_WERROR "Treat warnings as errors (CI)" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# Qt 6.2 is the floor on purpose: distro Qt (Ubuntu 22.04 / Debian 12) must work.
# Do not use APIs introduced after 6.2 (no qt_standard_project_setup, no QHttpServer).
set(ONVIFSIM_QT_COMPONENTS Core Network Gui)
if(BUILD_GUI)
    list(APPEND ONVIFSIM_QT_COMPONENTS Widgets)
endif()
if(BUILD_TESTS)
    list(APPEND ONVIFSIM_QT_COMPONENTS Test)
endif()
if(ENABLE_AUDIO_PLAYBACK)
    list(APPEND ONVIFSIM_QT_COMPONENTS Multimedia)
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS ${ONVIFSIM_QT_COMPONENTS})
find_package(Qt6 6.2 QUIET COMPONENTS LinguistTools)

include(CompilerWarnings)

add_subdirectory(src)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()

message(STATUS "onvifsim ${ONVIFSIM_VERSION_FULL}")
message(STATUS "  Qt              ${Qt6_VERSION}")
message(STATUS "  GUI             ${BUILD_GUI}")
message(STATUS "  tests           ${BUILD_TESTS}")
message(STATUS "  audio playback  ${ENABLE_AUDIO_PLAYBACK}")
