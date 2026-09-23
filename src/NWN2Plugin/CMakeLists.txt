# Headers only. SamplePlugin compiles Data.cpp/Logger.cpp into itself, which is
# the right model for an SDK: plugin authors link no loader library at all.
add_library(NWN2Plugin INTERFACE)
target_include_directories(NWN2Plugin INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})