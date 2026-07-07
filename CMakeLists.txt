cmake_minimum_required(VERSION 3.16)
project(pano-blend LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4 REQUIRED
    COMPONENTS core imgproc imgcodecs stitching)
find_package(TIFF REQUIRED)

set(PANOBLEND_SOURCES src/main.cpp src/colors.cpp src/seam.cpp src/blend.cpp src/tiff_io.cpp)

# Version comes solely from `git describe` ("unknown" fallback), regenerated on
# every build by cmake/GenerateVersion.cmake so incremental builds never embed
# a stale hash. It produces generated/version.h and, on Windows, the
# VERSIONINFO resource (product name, version, copyright, origin URL — visible
# in Explorer > Properties > Details) with numeric fields parsed from the tag.
find_package(Git QUIET)
set(PANOBLEND_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/version.h)
set(version_args
    -DSRC_DIR=${CMAKE_SOURCE_DIR}
    -DOUT_HEADER=${PANOBLEND_VERSION_HEADER}
    -DGIT_EXECUTABLE=${GIT_EXECUTABLE})
set(version_byproducts ${PANOBLEND_VERSION_HEADER})
if(WIN32)
    list(APPEND version_args
        -DRC_IN=${CMAKE_SOURCE_DIR}/pano-blend.rc.in
        -DRC_OUT=${CMAKE_BINARY_DIR}/generated/pano-blend.rc)
    list(APPEND version_byproducts ${CMAKE_BINARY_DIR}/generated/pano-blend.rc)
    list(APPEND PANOBLEND_SOURCES ${CMAKE_BINARY_DIR}/generated/pano-blend.rc)
endif()
add_custom_target(generate_version
    COMMAND ${CMAKE_COMMAND} ${version_args}
            -P ${CMAKE_SOURCE_DIR}/cmake/GenerateVersion.cmake
    BYPRODUCTS ${version_byproducts}
    COMMENT "Refreshing version from git describe")

add_executable(pano-blend ${PANOBLEND_SOURCES})
add_dependencies(pano-blend generate_version)
target_include_directories(pano-blend PRIVATE ${OpenCV_INCLUDE_DIRS} src
                           ${CMAKE_BINARY_DIR}/generated)
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
