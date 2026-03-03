cmake_minimum_required(VERSION 3.16)
project(blend CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4 REQUIRED
    COMPONENTS core imgproc imgcodecs)

add_executable(blend src/main.cpp src/colors.cpp src/seam.cpp)
target_include_directories(blend PRIVATE ${OpenCV_INCLUDE_DIRS} src)
target_link_libraries(blend PRIVATE ${OpenCV_LIBS})

enable_testing()
find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(test_input tests/test_input.cpp)
target_include_directories(test_input PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(test_input PRIVATE GTest::gtest_main ${OpenCV_LIBS})
target_compile_definitions(test_input PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test-data")
gtest_discover_tests(test_input)
