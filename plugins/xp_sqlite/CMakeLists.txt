# SQLite is compiled as its own static library rather than as part of the plugin, so the plugin's
# warning settings do not apply to 240k lines of someone else's C and so it only rebuilds when the
# amalgamation is actually replaced.
add_library(sqlite3 STATIC lib/sqlite3.c)

target_include_directories(sqlite3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/lib")

# A server plugin needs none of SQLite's shell-oriented extras. Omitting them keeps the DLL
# smaller and reduces the surface that has to be trusted.
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_COLUMN_METADATA   # sqlite3_column_name on a statement that returned no rows
    SQLITE_OMIT_LOAD_EXTENSION      # nothing should be loading .so/.dll into the server
    SQLITE_OMIT_DEPRECATED
    SQLITE_DQS=0                    # a bare "string" is an error, not a silently-accepted literal
    SQLITE_THREADSAFE=1)

add_library(xp_sqlite SHARED
    XpSqlite.cpp
    ${NWN2_PLUGIN_SUPPORT})

target_link_libraries(xp_sqlite PRIVATE nwn2_plugin_sdk sqlite3 yaml-cpp)

set_target_properties(xp_sqlite PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# The sample config travels with the DLL, since the plugin looks for it next to itself.
configure_file(xp_sqlite.yml "${CMAKE_BINARY_DIR}/bin/xp_sqlite.yml.sample" COPYONLY)
