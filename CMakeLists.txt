cmake_minimum_required(VERSION 3.20)
project(bsvx VERSION 4.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

# CMake defines this itself from 3.21 on; the manual form keeps the 3.20 floor.
if(NOT DEFINED PROJECT_IS_TOP_LEVEL)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(PROJECT_IS_TOP_LEVEL ON)
    else()
        set(PROJECT_IS_TOP_LEVEL OFF)
    endif()
endif()

# A host that links bsvx into one loadable module of its own -- a GDExtension, a DCC plugin --
# wants a single file to ship, not a module plus a sibling .so it has to teach the loader about.
# Building the library into that module is what STATIC is for; the flat C ABI is unchanged either
# way, since a static bsvx simply resolves bsvx_* at link time instead of through the loader.
option(BSVX_BUILD_SHARED "Build bsvx as a shared library (OFF builds a static archive)" ON)
if(BSVX_BUILD_SHARED)
    set(BSVX_LIBRARY_TYPE SHARED)
else()
    set(BSVX_LIBRARY_TYPE STATIC)
endif()

file(GLOB BSVX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/impl/*.cpp")

add_library(bsvx ${BSVX_LIBRARY_TYPE} ${BSVX_SOURCES})
add_library(bsvx::bsvx ALIAS bsvx)

target_include_directories(bsvx
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/impl")
target_compile_definitions(bsvx PRIVATE TOML_HEADER_ONLY=0)
if(BSVX_BUILD_SHARED)
    # Only meaningful on Windows, where it turns BSVX_API into __declspec(dllexport). A static
    # bsvx must not carry it: consumers include the same header without BSVX_ASSETS_USE_DLL, and
    # an exported-but-not-imported symbol is a link error rather than a warning.
    target_compile_definitions(bsvx PRIVATE BSVX_ASSETS_BUILD_DLL)
endif()
set_target_properties(bsvx PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # SOVERSION tracks BSVX_ABI_VERSION: a host that dlopen()s the library checks
    # bsvx_abi_version() at runtime, and the loader checks this at link time.
    VERSION   ${PROJECT_VERSION}
    SOVERSION 4)

# A host that loads this library into an existing process -- Blender, a DCC tool, a game engine --
# may already have an older libstdc++ mapped in. Linking ours statically keeps the two from
# fighting, which otherwise shows up as a crash at import time rather than a diagnosable error.
option(BSVX_STATIC_CXX_RUNTIME "Link libstdc++/libgcc statically (recommended for redistributable plugin builds)" OFF)
if(BSVX_STATIC_CXX_RUNTIME AND NOT MSVC)
    target_link_options(bsvx PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Vendored toml++ uses `operator"" _toml`, whose space before the suffix is deprecated in C++23.
# The warning is not actionable in third-party code, so it is silenced for those two files only.
#
# MSVC must be excluded, and not merely because it does not need it: cl translates -W into its own
# /W warning-*level* flag and then rejects "no-deprecated-literal-operator" as a non-numeric level
# (command line error D8021), which fails the build outright rather than being ignored.
if(NOT MSVC)
    set_source_files_properties(
      "${CMAKE_CURRENT_SOURCE_DIR}/src/impl/toml_impl.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/impl/toml.hpp"
      PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-literal-operator"
    )
endif()

if(MSVC)
    # The test suite and this library both carry UTF-8 source (u8"" literals with non-ASCII text).
    # Without /utf-8 cl decodes the source in the active code page, which mangles them and emits
    # C4819. /utf-8 also sets the execution charset, which is what the "paths are UTF-8" contract
    # assumes.
    target_compile_options(bsvx PRIVATE /utf-8)
    # toml++ is large enough to exhaust the default object-file section limit.
    target_compile_options(bsvx PRIVATE /bigobj)
    # Without this cl reports __cplusplus as 199711L regardless of /std, which makes the value
    # bsvx_build_info() prints a lie and breaks any feature test that reads it.
    target_compile_options(bsvx PRIVATE /Zc:__cplusplus)
endif()

# Skipped in a subproject build: the generated .clangd points at CMAKE_BINARY_DIR, which then
# belongs to the superproject, and writing it would repoint this repo's tooling at a build tree
# that only exists while the superproject is configured.
if(PROJECT_IS_TOP_LEVEL)
    message(STATUS "Writing .clangd to ${CMAKE_CURRENT_SOURCE_DIR}/.clangd")
    file(RELATIVE_PATH CLANGD_BUILD_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
    )

    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/.clangd.in"
      "${CMAKE_CURRENT_SOURCE_DIR}/.clangd"
      @ONLY
    )
endif()

option(BSVX_BUILD_TESTS "Build the fixture-driven test runner" ${PROJECT_IS_TOP_LEVEL})
if(BSVX_BUILD_TESTS)
    add_executable(bsvx_tests "${CMAKE_CURRENT_SOURCE_DIR}/test/tests.cpp")
    target_link_libraries(bsvx_tests PRIVATE bsvx)
    if(BSVX_BUILD_SHARED)
        target_compile_definitions(bsvx_tests PRIVATE BSVX_ASSETS_USE_DLL)
    endif()
    if(MSVC)
        # test_non_ascii_paths has UTF-8 literals in the source; see above.
        target_compile_options(bsvx_tests PRIVATE /utf-8)
    endif()

    enable_testing()
    add_test(NAME bsvx_tests COMMAND bsvx_tests WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Off by default: it is a measurement tool, not part of the library or of what CI gates on, and it
# writes multi-megabyte fixtures to disk when it runs.
option(BSVX_BUILD_BENCH "Build the .vox comparison benchmark" OFF)
if(BSVX_BUILD_BENCH)
    add_executable(bench_vox "${CMAKE_CURRENT_SOURCE_DIR}/bench/bench_vox.cpp")
    target_link_libraries(bench_vox PRIVATE bsvx)
    # bvx_anatomy.hpp reads the on-disk structures through the library's own layout headers, which
    # live in src/impl's include path alongside the public ones.
    target_include_directories(bench_vox PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/bench"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/impl")
    if(BSVX_BUILD_SHARED)
        target_compile_definitions(bench_vox PRIVATE BSVX_ASSETS_USE_DLL)
    endif()
endif()

# ------------------------------------------------------------------------------------------------
# Install / package
# ------------------------------------------------------------------------------------------------

# A superproject installs its own module, not this one -- and the export set below would collide
# with whatever export set the superproject defines.
if(PROJECT_IS_TOP_LEVEL)

# Only bsvx_dll.h is a stable boundary; the rest are the C++ internals and are installed alongside
# it for consumers that link the library into their own binary.
install(TARGETS bsvx
    EXPORT bsvxTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.h")

install(EXPORT bsvxTargets
    FILE bsvxTargets.cmake
    NAMESPACE bsvx::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/bsvx")

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/bsvxConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bsvxConfig.cmake"
"include(\"\${CMAKE_CURRENT_LIST_DIR}/bsvxTargets.cmake\")\n")

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/bsvxConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/bsvxConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/bsvx")

endif() # PROJECT_IS_TOP_LEVEL

# ------------------------------------------------------------------------------------------------
# Python binding: stage the built shared library where the package expects to find it
# ------------------------------------------------------------------------------------------------

# The ctypes binding loads a shared library, so there is nothing to stage from a static build.
option(BSVX_STAGE_PYTHON "Copy the built library into python/bsvx/bin/<platform>/ after building" ON)
if(BSVX_STAGE_PYTHON AND BSVX_BUILD_SHARED AND PROJECT_IS_TOP_LEVEL)
    if(WIN32)
        set(BSVX_PY_PLATFORM "windows_x86_64")
    elseif(APPLE)
        set(BSVX_PY_PLATFORM "macos_universal")
    else()
        set(BSVX_PY_PLATFORM "linux_${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    set(BSVX_PY_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/python/bsvx/bin/${BSVX_PY_PLATFORM}")
    # Staged under the unversioned name the ctypes loader looks for -- an add-on ships one file per
    # platform, not a symlink farm.
    set(BSVX_PY_LIB_NAME "${CMAKE_SHARED_LIBRARY_PREFIX}bsvx${CMAKE_SHARED_LIBRARY_SUFFIX}")
    add_custom_command(TARGET bsvx POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${BSVX_PY_BIN_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:bsvx>" "${BSVX_PY_BIN_DIR}/${BSVX_PY_LIB_NAME}"
        COMMENT "Staging ${BSVX_PY_LIB_NAME} into python/bsvx/bin/${BSVX_PY_PLATFORM}")
endif()
