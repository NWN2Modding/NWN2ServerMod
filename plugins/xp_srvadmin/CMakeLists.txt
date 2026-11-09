add_library(xp_srvadmin SHARED
    XpSrvAdmin.cpp
    ${NWN2_PLUGIN_SUPPORT})

target_link_libraries(xp_srvadmin PRIVATE nwn2_plugin_sdk yaml-cpp)

set_target_properties(xp_srvadmin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

configure_file(xp_srvadmin.yml "${CMAKE_BINARY_DIR}/bin/xp_srvadmin.yml.sample" COPYONLY)
