add_library(NWN2Shared STATIC
    Config.cpp
    Data.cpp
    Handle.cpp
    Logger.cpp
    MemoryMap.cpp)

target_link_libraries(NWN2Shared PUBLIC yaml-cpp PRIVATE nwn2_flags)
target_include_directories(NWN2Shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(NWN2Shared PUBLIC yaml-cpp)