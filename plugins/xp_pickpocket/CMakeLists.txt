add_library(xp_pickpocket SHARED
    XpPickpocket.cpp
    ${NWN2_PLUGIN_SUPPORT})

# Owns its own hook, like xp_profiler. See SCRIPT_HOOK_FINDINGS.md for why that is safe.
target_link_libraries(xp_pickpocket PRIVATE nwn2_plugin_sdk Detours yaml-cpp)

set_target_properties(xp_pickpocket PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

configure_file(xp_pickpocket.yml "${CMAKE_BINARY_DIR}/bin/xp_pickpocket.yml.sample" COPYONLY)
