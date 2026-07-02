# Building on Linux

## Prerequisites

* git
* gcc or clang compiler
* CMake
* `sudo zypper in libtiff-devel opencv-devel gtest` (openSUSE Tumbleweed)
* `sudo dnf in libtiff-devel opencv-devel gtest-devel` (Fedora 44)

## Build instructions (dynamic linking)

* clone git repository

```sh
# Change directory
cd pano-blend
# CMake configure
# (optionally -DCMAKE_BUILD_TYPE=Release|RelWithDebInfo|Debug -DCMAKE_CXX_COMPILER=g++|clang++|clang++-22 -DCMAKE_C_COMPILER=gcc|clang))
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
# CMake build (use `build` directory)
cmake --build build
# Test, prints usage
build/blend
```

`Release` (`-O3 -DNDEBUG`) is the default for a fast, small binary. Use
`RelWithDebInfo` (`-O2 -g`) when you want optimized code that still carries
debug symbols — handy for profiling.

