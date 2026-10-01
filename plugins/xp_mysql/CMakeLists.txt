# The connector is located by ports/CMakeLists.txt, which only adds this directory once it has
# found one. See there for how to provide it.

add_library(xp_mysql SHARED
    XpMySql.cpp
    ${NWN2_PLUGIN_SUPPORT})

target_link_libraries(xp_mysql PRIVATE nwn2_plugin_sdk mariadb_client yaml-cpp)

set_target_properties(xp_mysql PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

configure_file(xp_mysql.yml "${CMAKE_BINARY_DIR}/bin/xp_mysql.yml.sample" COPYONLY)
