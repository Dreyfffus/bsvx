cmake_minimum_required(VERSION 3.22)
project(bsvx_godot LANGUAGES CXX)

# ------------------------------------------------------------------------------------------------
# bsvx itself
# ------------------------------------------------------------------------------------------------
#
# The library is consumed through its own CMakeLists rather than by globbing its sources: that file
# already carries the MSVC flags this code needs (/utf-8, /bigobj, /Zc:__cplusplus) and the
# non-MSVC-only warning suppression for the vendored toml++. A second, hand-copied build definition
# drifts from the first, and the first thing it loses is exactly those flags.
#
# STATIC because a GDExtension should be one file to ship. The flat C ABI behaves identically; it
# simply resolves at link time instead of through the loader, which also removes the rpath dance a
# sibling shared library would need.
set(BSVX_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BSVX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BSVX_STAGE_PYTHON OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." bsvx_core)

# ------------------------------------------------------------------------------------------------
# godot-cpp -- the only submodule this integration has
# ------------------------------------------------------------------------------------------------

set(GODOT_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/godot-cpp")
if(NOT EXISTS "${GODOT_CPP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "godot-cpp is missing. Run:\n"
        "  git submodule update --init --recursive integrations/godot/extern/godot-cpp")
endif()

add_subdirectory("${GODOT_CPP_DIR}" godot-cpp)

# ------------------------------------------------------------------------------------------------
# The extension
# ------------------------------------------------------------------------------------------------

add_library(bsvx_godot SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bsvx_gd_vfs.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bsvx_world_resource.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/register_types.cpp")

target_link_libraries(bsvx_godot PRIVATE godot-cpp bsvx)
target_include_directories(bsvx_godot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# godot-cpp publishes the naming convention Godot's .gdextension files expect as a property on its
# own target, so the two cannot disagree about what the built file is called.
get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)
set(BSVX_GODOT_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/project/addons/bsvx/bin")

set_target_properties(bsvx_godot PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
    BUILD_RPATH_USE_ORIGIN ON
    PREFIX "lib"
    OUTPUT_NAME "bsvx_godot${GODOTCPP_SUFFIX}"
    # Wrapping these in a generator expression stops the MSVC generator appending a per-config
    # directory, which would put the binary somewhere the .gdextension does not look.
    LIBRARY_OUTPUT_DIRECTORY "$<1:${BSVX_GODOT_BIN_DIR}>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${BSVX_GODOT_BIN_DIR}>"
    PDB_OUTPUT_DIRECTORY "$<1:${BSVX_GODOT_BIN_DIR}>")

if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
    # Godot loads macOS extensions out of a .framework directory, not a bare dylib.
    set_target_properties(bsvx_godot PROPERTIES
        SUFFIX ""
        LIBRARY_OUTPUT_DIRECTORY "$<1:${BSVX_GODOT_BIN_DIR}/libbsvx_godot.macos.${GODOTCPP_TARGET}.framework>")
endif()

message(STATUS "bsvx_godot -> ${BSVX_GODOT_BIN_DIR}")
