add_library(xp_objectattributes SHARED
    XpObjectAttributes.cpp
    ${NWN2_PLUGIN_SUPPORT})

target_link_libraries(xp_objectattributes PRIVATE nwn2_plugin_sdk yaml-cpp)

set_target_properties(xp_objectattributes PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

configure_file(xp_objectattributes.yml
               "${CMAKE_BINARY_DIR}/bin/xp_objectattributes.yml.sample" COPYONLY)
