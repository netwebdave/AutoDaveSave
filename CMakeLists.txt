cmake_minimum_required(VERSION 3.20)

project(AutoDaveSavePlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# One DLL plugin target
add_library(plugin SHARED
    src/plugin.cpp
)

target_include_directories(plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Put outputs in build/out
set_target_properties(plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
)

# Windows convenience: exports symbols automatically
if (WIN32)
    set_target_properties(plugin PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
