cmake_minimum_required(VERSION 3.20)
project(bsvx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

file(GLOB BSVX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/impl/*.cpp")

add_library(bsvx SHARED ${BSVX_SOURCES})
target_include_directories(bsvx
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/src/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/impl")
target_compile_definitions(bsvx PRIVATE TOML_HEADER_ONLY=0 BSVX_ASSETS_BUILD_DLL)
set_target_properties(bsvx PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

set_source_files_properties(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/impl/toml_impl.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/impl/toml.hpp"
  PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-literal-operator"
)

message(STATUS "Writing .clangd to ${CMAKE_CURRENT_SOURCE_DIR}/.clangd")
file(RELATIVE_PATH CLANGD_BUILD_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/.clangd.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/.clangd"
  @ONLY
)

option(BSVX_BUILD_TESTS "Build the fixture-driven test runner" ON)
if(BSVX_BUILD_TESTS)
    add_executable(bsvx_tests "${CMAKE_CURRENT_SOURCE_DIR}/test/tests.cpp")
    target_link_libraries(bsvx_tests PRIVATE bsvx)
    target_compile_definitions(bsvx_tests PRIVATE BSVX_ASSETS_USE_DLL)
endif()
