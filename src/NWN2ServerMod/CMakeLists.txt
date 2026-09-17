add_executable(NWN2ServerMod
    Main.cpp
    NWN2Server.cpp)

# SubSystem was Windows in the .vcxproj - no console window.
set_target_properties(NWN2ServerMod PROPERTIES WIN32_EXECUTABLE TRUE)

target_link_libraries(NWN2ServerMod PRIVATE NWN2Shared nwn2_flags)