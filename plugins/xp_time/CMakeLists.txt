add_library(xp_time SHARED
    XpTime.cpp
    ${NWN2_PLUGIN_SUPPORT})

target_link_libraries(xp_time PRIVATE nwn2_plugin_sdk)

set_target_properties(xp_time PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
