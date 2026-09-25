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
    src/file_service.cpp
    src/http.cpp
    src/http_response.cpp
    src/main.cpp
    src/router.cpp
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

add_executable(file_service_test
    tests/file_service_test.cpp
    src/file_service.cpp
)

target_compile_features(file_service_test PRIVATE cxx_std_17)
target_include_directories(file_service_test PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)
set_target_properties(file_service_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_options(file_service_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(NAME file_service_test COMMAND file_service_test)


add_executable(streaming_upload_test
    tests/streaming_upload_test.cpp
    src/connection.cpp
    src/file_service.cpp
    src/http.cpp
    src/http_response.cpp
    src/router.cpp
)

target_compile_features(streaming_upload_test PRIVATE cxx_std_17)
target_include_directories(streaming_upload_test PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)
set_target_properties(streaming_upload_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_options(streaming_upload_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(NAME streaming_upload_test COMMAND streaming_upload_test)


add_executable(http_response_test
    tests/http_response_test.cpp
    src/connection.cpp
    src/file_service.cpp
    src/http.cpp
    src/http_response.cpp
    src/router.cpp
)

target_compile_features(http_response_test PRIVATE cxx_std_17)
target_include_directories(http_response_test PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)
set_target_properties(http_response_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_options(http_response_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(NAME http_response_test COMMAND http_response_test)

add_executable(get_streaming_test
    tests/get_streaming_test.cpp
    src/connection.cpp
    src/file_service.cpp
    src/http.cpp
    src/http_response.cpp
    src/router.cpp
)

target_compile_features(get_streaming_test PRIVATE cxx_std_17)
target_include_directories(get_streaming_test PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)
set_target_properties(get_streaming_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_options(get_streaming_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(NAME get_streaming_test COMMAND get_streaming_test)
