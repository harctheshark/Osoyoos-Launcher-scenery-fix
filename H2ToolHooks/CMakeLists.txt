cmake_minimum_required(VERSION 3.25)

project(H2ToolHooks VERSION 1.0)

set(SOURCES
    src/dllmain.cpp
    src/H2ToolHooks.cpp
    src/PatternScanner.cpp
)

add_library(H2ToolHooks SHARED ${SOURCES})

target_compile_definitions(H2ToolHooks PRIVATE
    WIN32_LEAN_AND_MEAN
    H2TOOLHOOKS_EXPORTS
    _WINDOWS
    _USRDLL
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

target_compile_options(H2ToolHooks PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:
        -Wall
        -Wno-c++98-compat
        -Wno-c++98-compat-pedantic
        -Wno-padded
        /EHsc
        $<$<CONFIG:Release>:-O2 -flto>
    >
)

target_link_options(H2ToolHooks PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:
        /MANIFESTUAC:NO
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
    >
)

target_include_directories(H2ToolHooks PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

set_target_properties(H2ToolHooks PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/x86/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/x86/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/x86/$<CONFIG>"
)

target_link_libraries(H2ToolHooks PRIVATE
    psapi
    kernel32
)
