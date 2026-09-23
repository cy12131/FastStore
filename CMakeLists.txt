cmake_minimum_required(VERSION 3.16)

project(FastStore VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "FastStore currently supports Linux only")
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_executable(faststore
    src/connection.cpp
    src/http.cpp
    src/main.cpp
    src/server.cpp
)

target_compile_features(faststore PRIVATE cxx_std_17)
target_include_directories(faststore PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)
set_target_properties(faststore PROPERTIES
    CXX_EXTENSIONS OFF
)

target_compile_options(faststore PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_definitions(faststore PRIVATE
    FASTSTORE_DATA_DIRECTORY="${PROJECT_SOURCE_DIR}/data"
)

enable_testing()

add_executable(http_parser_test
    tests/http_parser_test.cpp
    src/http.cpp
)

target_compile_features(http_parser_test PRIVATE cxx_std_17)
target_include_directories(http_parser_test PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)
set_target_properties(http_parser_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_options(http_parser_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(NAME http_parser_test COMMAND http_parser_test)
