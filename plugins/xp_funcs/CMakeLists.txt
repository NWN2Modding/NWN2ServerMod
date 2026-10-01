add_library(xp_funcs SHARED
    XpFuncs.cpp
    ${NWN2_PLUGIN_SUPPORT})

target_link_libraries(xp_funcs PRIVATE nwn2_plugin_sdk)

set_target_properties(xp_funcs PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
