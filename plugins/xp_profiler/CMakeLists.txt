add_library(xp_profiler SHARED
    XpProfiler.cpp
    ${NWN2_PLUGIN_SUPPORT})

# This plugin owns its own hook rather than asking the loader for one. Detours chains correctly
# when more than one module attaches to the same function, so a plugin linking its own copy is
# safe - see SCRIPT_HOOK_FINDINGS.md.
target_link_libraries(xp_profiler PRIVATE nwn2_plugin_sdk Detours yaml-cpp)

set_target_properties(xp_profiler PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

configure_file(xp_profiler.yml "${CMAKE_BINARY_DIR}/bin/xp_profiler.yml.sample" COPYONLY)
