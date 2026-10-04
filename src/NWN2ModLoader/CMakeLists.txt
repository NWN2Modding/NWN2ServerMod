add_library(NWN2ModLoader SHARED
    CExoString.cpp
    DataBlock.cpp
    FastModuleLoad.cpp
    Main.cpp
    NWN2Mod.cpp
    PEPattern.cpp
    PluginManager.cpp
    VirtualMachineCommands.cpp)

target_link_libraries(NWN2ModLoader PRIVATE
    NWN2Shared
    NWN2Plugin
    Detours
    psapi
    nwn2_flags)