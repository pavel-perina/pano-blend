cmake_minimum_required(VERSION 3.16)
project(blend CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4 REQUIRED
    COMPONENTS core imgproc imgcodecs)

add_executable(blend main.cpp)
target_include_directories(blend PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(blend PRIVATE ${OpenCV_LIBS})
