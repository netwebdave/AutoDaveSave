cmake_minimum_required(VERSION 3.20)

project(AutoDaveSave LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build one DLL (Windows) or shared library (other platforms)
add_library(AutoDaveSave SHARED
    AudoDaveSave.cpp
)

# Optional: use module definition file for exports (MSVC)
if (MSVC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Exports.def")
    set_target_properties(AutoDaveSave PROPERTIES
        LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/Exports.def"
    )
endif()

# Put output artifacts in build temp folder under /out
set_target_properties(AutoDaveSave PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"  # .dll
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"  # .lib import libs or non-Windows shared libs
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"  # static/import libs
)
