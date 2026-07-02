cmake_minimum_required(VERSION 3.16)
project(pano-blend VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4 REQUIRED
    COMPONENTS core imgproc imgcodecs stitching)
find_package(TIFF REQUIRED)

# Version string from `git describe`; falls back to "unknown" when git or the
# .git directory is unavailable (e.g. a source-archive build). Evaluated at
# configure time.
set(PANOBLEND_VERSION "unknown")
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE PANOBLEND_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
message(STATUS "pano-blend version: ${PANOBLEND_VERSION}")

set(PANOBLEND_SOURCES src/main.cpp src/colors.cpp src/seam.cpp src/blend.cpp src/tiff_io.cpp)

# Windows: embed a VERSIONINFO resource so the .exe carries product name,
# version, copyright and origin URL (visible in Explorer > Properties > Details).
# Only compiled on WIN32; the Linux build is untouched.
if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/pano-blend.rc.in
                   ${CMAKE_BINARY_DIR}/pano-blend.rc @ONLY)
    list(APPEND PANOBLEND_SOURCES ${CMAKE_BINARY_DIR}/pano-blend.rc)
endif()

add_executable(pano-blend ${PANOBLEND_SOURCES})
target_include_directories(pano-blend PRIVATE ${OpenCV_INCLUDE_DIRS} src)
target_compile_definitions(pano-blend PRIVATE PANOBLEND_VERSION="${PANOBLEND_VERSION}")
target_link_libraries(pano-blend PRIVATE ${OpenCV_LIBS} TIFF::TIFF)

add_executable(tag_tiff tools/tag_tiff.c)
target_link_libraries(tag_tiff PRIVATE TIFF::TIFF)

enable_testing()
find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(test_input tests/test_input.cpp src/tiff_io.cpp)
target_include_directories(test_input PRIVATE ${OpenCV_INCLUDE_DIRS} src)
target_link_libraries(test_input PRIVATE GTest::gtest_main ${OpenCV_LIBS} TIFF::TIFF)
target_compile_definitions(test_input PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test-data")
gtest_discover_tests(test_input)
