cmake_minimum_required(VERSION 3.16)

project(OSMParser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THIRD_PARTY_ROOT "${CMAKE_CURRENT_LIST_DIR}/third_party")
set(PBF_PARSER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../pbf-parser")
set(HTTPLIB_INCLUDE_DIR "${THIRD_PARTY_ROOT}/cpp-httplib")

find_package(Threads REQUIRED)

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/include/build_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/build_config.h"
    @ONLY
)

find_path(LIBOSMIUM_INCLUDE_DIR
    NAMES osmium/io/any_input.hpp
    HINTS
        "${THIRD_PARTY_ROOT}/libosmium/include"
        "${PBF_PARSER_ROOT}/libosmium/include"
        "/usr/include"
        "/usr/local/include")

find_path(PROTOZERO_INCLUDE_DIR
    NAMES protozero/pbf_reader.hpp
    HINTS
        "${THIRD_PARTY_ROOT}/protozero/include"
        "${PBF_PARSER_ROOT}/protozero/include"
        "/usr/include"
        "/usr/local/include")

if(NOT LIBOSMIUM_INCLUDE_DIR)
    message(FATAL_ERROR "Could not find libosmium headers. Set LIBOSMIUM_INCLUDE_DIR or install libosmium.")
endif()

if(NOT PROTOZERO_INCLUDE_DIR)
    message(FATAL_ERROR "Could not find protozero headers. Set PROTOZERO_INCLUDE_DIR or provide protozero.")
endif()

if(NOT EXISTS "${HTTPLIB_INCLUDE_DIR}/httplib.h")
    message(FATAL_ERROR "Could not find cpp-httplib header at ${HTTPLIB_INCLUDE_DIR}/httplib.h.")
endif()

add_executable(osm_parser
    src/main.cpp
    src/geocode_index.cpp
    src/parser.cpp
    src/server.cpp
)

target_include_directories(osm_parser PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_include_directories(osm_parser SYSTEM PRIVATE
    "${LIBOSMIUM_INCLUDE_DIR}"
    "${PROTOZERO_INCLUDE_DIR}"
    "${HTTPLIB_INCLUDE_DIR}"
)

if(WIN32)
    target_link_libraries(osm_parser PRIVATE ws2_32 psapi)
endif()

target_link_libraries(osm_parser PRIVATE Threads::Threads z bz2 expat)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(osm_parser PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()
