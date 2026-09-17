add_library(SamplePlugin SHARED
    SamplePlugin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../NWN2Plugin/Data.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../NWN2Plugin/Logger.cpp)

target_link_libraries(SamplePlugin PRIVATE NWN2Plugin yaml-cpp nwn2_flags)