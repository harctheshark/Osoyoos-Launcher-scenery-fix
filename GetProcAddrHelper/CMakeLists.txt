cmake_minimum_required(VERSION 3.25)

project(GetProcAddrHelper VERSION 1.0)

add_executable(GetProcAddrHelper src/GetProcAddrHelper.cpp)

target_compile_definitions(GetProcAddrHelper PRIVATE
    WIN32_LEAN_AND_MEAN
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

target_compile_options(GetProcAddrHelper PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:
        -Wall
        -Wno-c++98-compat
        -Wno-c++98-compat-pedantic
        -Wno-padded
        /GS-
        $<$<CONFIG:Release>:-O2 -flto>
    >
)

target_link_options(GetProcAddrHelper PRIVATE
    /NODEFAULTLIB
    /SUBSYSTEM:WINDOWS
    $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
)

set_target_properties(GetProcAddrHelper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/x86/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/x86/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/x86/$<CONFIG>"
)

target_link_libraries(GetProcAddrHelper PRIVATE
    kernel32
    shell32
)
