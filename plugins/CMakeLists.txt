cmake_minimum_required(VERSION 3.25)
# C as well as CXX: the SQLite amalgamation is C, and without it enabled CMake cannot work out
# how to link that target.
project(nwnx_ports C CXX)

# NWNX4 plugins ported to this loader's C ABI.
#
# They build independently of the loader itself: NWN2Plugin is header-only and a plugin links no
# loader library, so a port needs the SDK include directory and two support translation units and
# nothing else. Configuring this directory on its own is a supported way to build only the plugins.
#
# LOADER_SOURCE defaults to this repository's own src, and can be pointed elsewhere to build these
# against a different checkout.
set(LOADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    CACHE PATH "src directory of a NWN2ServerMod checkout")

if(NOT EXISTS "${LOADER_SOURCE}/NWN2Plugin/Plugin.hpp")
    message(FATAL_ERROR
        "LOADER_SOURCE does not look like a NWN2ServerMod src directory: ${LOADER_SOURCE}")
endif()

# Matches the loader's own settings. /MT because the engine process is not going to have the
# dynamic CRT the plugin wants, and the plugin owns everything it allocates anyway.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(nwn2_plugin_sdk INTERFACE)
target_include_directories(nwn2_plugin_sdk INTERFACE "${LOADER_SOURCE}/NWN2Plugin")

# Every port compiles these into itself, the same way SamplePlugin does.
set(NWN2_PLUGIN_SUPPORT
    "${LOADER_SOURCE}/NWN2Plugin/Data.cpp"
    "${LOADER_SOURCE}/NWN2Plugin/Logger.cpp")

# Yaml.h in the SDK is a wrapper around yaml-cpp, so a port that reads a config needs the library
# itself. The loader checkout already builds it; reuse that rather than keeping a second copy.
if(TARGET yaml-cpp)
    # Already built, because this is a sub-build of the whole repository.
elseif(EXISTS "${LOADER_SOURCE}/../yaml-cpp/CMakeLists.txt")
    add_subdirectory("${LOADER_SOURCE}/../yaml-cpp" "${CMAKE_BINARY_DIR}/yaml-cpp")
else()
    message(FATAL_ERROR
        "yaml-cpp was not found next to ${LOADER_SOURCE}; a port that reads a config cannot build")
endif()

add_subdirectory(xp_time)
add_subdirectory(xp_srvadmin)
add_subdirectory(xp_sqlite)
add_subdirectory(xp_funcs)
add_subdirectory(xp_objectattributes)

# xp_profiler attaches its own detour, so it needs Detours. The loader checkout already builds it;
# reuse that rather than keeping a second copy, the same way yaml-cpp is handled above.
if(TARGET Detours)
    # Already built by the parent, as above.
    add_subdirectory(xp_profiler)
    add_subdirectory(xp_pickpocket)
elseif(EXISTS "${LOADER_SOURCE}/../Detours/CMakeLists.txt")
    add_subdirectory("${LOADER_SOURCE}/../Detours" "${CMAKE_BINARY_DIR}/Detours")
    add_subdirectory(xp_profiler)
    add_subdirectory(xp_pickpocket)
else()
    message(STATUS "xp_profiler: SKIPPED - Detours was not found next to ${LOADER_SOURCE}")
endif()

# xp_mysql needs MariaDB Connector/C, which - unlike SQLite - is a real library rather than a file
# that can be vendored. It is the only port with a prerequisite, so it is skipped rather than
# fatal when the connector is absent: the other four still build with no setup at all.
#
# Provide it either way:
#   vcpkg install libmariadb:x64-windows-static
#     then configure with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#   or point -DMARIADB_CONNECTOR_ROOT=<prefix> at any existing connector, which is what a build
#     using a distro or mingw package will do.
#
# Building the connector from source here was tried and abandoned: its bundled zlib predates
# CMake 4, and several of its sources include windows.h ahead of winsock2.h, which clang-cl
# rejects. vcpkg carries the patches for both, which is why NWNX4 uses it too.
set(MARIADB_CONNECTOR_ROOT "" CACHE PATH
    "An existing MariaDB Connector/C prefix, if not using vcpkg.")

set(_mariadb_found FALSE)

if(MARIADB_CONNECTOR_ROOT)
    add_library(mariadb_client INTERFACE)
    target_include_directories(mariadb_client INTERFACE
        "${MARIADB_CONNECTOR_ROOT}/include"
        "${MARIADB_CONNECTOR_ROOT}/include/mariadb"
        "${MARIADB_CONNECTOR_ROOT}/include/mysql")
    target_link_directories(mariadb_client INTERFACE "${MARIADB_CONNECTOR_ROOT}/lib")
    target_link_libraries(mariadb_client INTERFACE mariadbclient ws2_32 shlwapi crypt32 bcrypt secur32)
    set(_mariadb_found TRUE)
    message(STATUS "xp_mysql: using the connector at ${MARIADB_CONNECTOR_ROOT}")
else()
    find_package(unofficial-libmariadb CONFIG QUIET)
    if(unofficial-libmariadb_FOUND)
        add_library(mariadb_client INTERFACE)
        target_link_libraries(mariadb_client INTERFACE unofficial::libmariadb)
        set(_mariadb_found TRUE)
        message(STATUS "xp_mysql: using the connector from vcpkg")
    endif()
endif()

if(_mariadb_found)
    add_subdirectory(xp_mysql)
else()
    message(STATUS
        "xp_mysql: SKIPPED - no MariaDB Connector/C found. "
        "Use the vcpkg toolchain with libmariadb installed, or set MARIADB_CONNECTOR_ROOT.")
endif()
