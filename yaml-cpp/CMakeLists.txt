set(YAML_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../src/yaml-cpp-0.9.0)
set(YAML_SRC  ${YAML_ROOT}/src)

add_library(yaml-cpp STATIC
    ${YAML_SRC}/binary.cpp          ${YAML_SRC}/convert.cpp
    ${YAML_SRC}/depthguard.cpp      ${YAML_SRC}/directives.cpp
    ${YAML_SRC}/emit.cpp            ${YAML_SRC}/emitfromevents.cpp
    ${YAML_SRC}/emitter.cpp         ${YAML_SRC}/emitterstate.cpp
    ${YAML_SRC}/emitterutils.cpp    ${YAML_SRC}/exceptions.cpp
    ${YAML_SRC}/exp.cpp             ${YAML_SRC}/fptostring.cpp
    ${YAML_SRC}/memory.cpp          ${YAML_SRC}/node.cpp
    ${YAML_SRC}/node_data.cpp       ${YAML_SRC}/nodebuilder.cpp
    ${YAML_SRC}/nodeevents.cpp      ${YAML_SRC}/null.cpp
    ${YAML_SRC}/ostream_wrapper.cpp ${YAML_SRC}/parse.cpp
    ${YAML_SRC}/parser.cpp          ${YAML_SRC}/regex_yaml.cpp
    ${YAML_SRC}/scanner.cpp         ${YAML_SRC}/scanscalar.cpp
    ${YAML_SRC}/scantag.cpp         ${YAML_SRC}/scantoken.cpp
    ${YAML_SRC}/simplekey.cpp       ${YAML_SRC}/singledocparser.cpp
    ${YAML_SRC}/stream.cpp          ${YAML_SRC}/tag.cpp
    ${YAML_SRC}/contrib/graphbuilder.cpp
    ${YAML_SRC}/contrib/graphbuilderadapter.cpp
)

target_include_directories(yaml-cpp
    PUBLIC  ${YAML_ROOT}/include
    PRIVATE ${YAML_SRC})

# Consumers must see this too, or the headers declare dllimport.
target_compile_definitions(yaml-cpp PUBLIC YAML_CPP_STATIC_DEFINE)

target_compile_options(yaml-cpp PRIVATE /w)