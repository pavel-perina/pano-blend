# Building on Windows

## Prerequisites
- Visual Studio 2022 or newer (with C++ workload)
- CMake 3.16+
- Conan 2.x (`pip install conan`)

## Conan profile

MSVC 2026 example, change version to 194 for VS2022

```ini
##[settings]
arch=x86_64
build_type=Release
compiler=msvc
compiler.cppstd=23
compiler.runtime=dynamic
compiler.version=195
os=Windows

[conf]
tools.cmake.cmaketoolchain:generator=Ninja
```

## Build conan packages

In power shell:

```powershell
conan install . --profile:all ./conan_profile_msvc2026 --build=missing -s build_type=Debug
conan install . --profile:all ./conan_profile_msvc2026 --build=missing -s build_type=RelWithDebInfo
```


> At this point you should open folder in Visual Studio. Next step is manual build

## Set up environment for build

Using old command line interface, VS2022 Professional

```bat
rem The following one is crucisal for x64 build instead of x86 build, but it does not work in powershell
"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
```

In power shell, VS2026 Community

```powershell
Import-Module "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "C:\Program Files\Microsoft Visual Studio\18\Community" -Arch amd64
# cd c:/devel/pano-blend
```

## Build project
```powershell
# List cmake presets (optional)
cmake --list-presets
# Prepare/configure CMakeCache
cmake --preset conan-relwithdebinfo
# Build
cmake --build --preset conan-relwithdebinfo
```
