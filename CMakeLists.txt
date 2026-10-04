cmake_minimum_required(VERSION 3.25)
project(NWN2ServerMod LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR
    "NWN2ServerMod targets Windows. Cross-compile from Linux with the clang-cl "
    "toolchain file and an xwin sysroot.")
endif()

# NWN2Server64.exe is 64-bit. A 32-bit loader DLL can never be injected into
# it, and a Ninja build silently uses whatever cl.exe is on PATH - so catch a
# 32-bit developer environment here rather than at runtime.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR
    "32-bit build detected.\n"
    "  Compiler: ${CMAKE_CXX_COMPILER}\n"
    "NWN2ServerMod is x64 only. Open an x64 developer shell, delete the build "
    "directory, and re-run cmake --preset.")
endif()

# Matches the .vcxproj files: MultiThreaded / MultiThreadedDebug (/MT, /MTd).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# clang-cl leaves exceptions off by default, and vendored yaml-cpp throws.
add_compile_options(/EHsc)

add_compile_definitions(UNICODE _UNICODE)

# Strict settings for OUR code only. Vendored Detours and yaml-cpp predate
# these: Detours' goto-based cleanup is ill-formed under /permissive- and
# C++23, and it builds upstream with neither.
add_library(nwn2_flags INTERFACE)
target_compile_features(nwn2_flags INTERFACE cxx_std_23)
target_compile_options(nwn2_flags INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_subdirectory(Detours)
add_subdirectory(yaml-cpp)
add_subdirectory(src/NWN2Plugin)
add_subdirectory(src/NWN2Shared)
add_subdirectory(src/NWN2ModLoader)
add_subdirectory(src/NWN2ServerMod)
add_subdirectory(src/SamplePlugin)