set(DETOURS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src/Detours-4.0.1/src)

add_library(Detours STATIC
    ${DETOURS_SRC}/creatwth.cpp
    ${DETOURS_SRC}/detours.cpp
    ${DETOURS_SRC}/disasm.cpp
    ${DETOURS_SRC}/disolarm.cpp
    ${DETOURS_SRC}/disolarm64.cpp
    ${DETOURS_SRC}/disolia64.cpp
    ${DETOURS_SRC}/disolx64.cpp
    ${DETOURS_SRC}/disolx86.cpp
    ${DETOURS_SRC}/image.cpp
    ${DETOURS_SRC}/modules.cpp
    # uimports.cpp is deliberately NOT listed: creatwth.cpp #includes it twice
    # (lines 340 and 356). Compiling it separately gives duplicate symbols.
)

target_include_directories(Detours PUBLIC ${DETOURS_SRC})

# Vendored third-party code; do not hold it to /W4.
target_compile_options(Detours PRIVATE /w)