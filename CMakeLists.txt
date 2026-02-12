cmake_minimum_required(VERSION 3.20)

project(sinnet VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This project currently supports Linux only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
option(SINNET_FETCH_GTEST "Fetch GoogleTest when not found via find_package" ON)

add_library(sinnet
    src/EventLoop.cpp
)
add_library(sinnet::sinnet ALIAS sinnet)

target_include_directories(sinnet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(sinnet PUBLIC cxx_std_17)

add_executable(sinnet_example
    examples/main.cpp
)
target_link_libraries(sinnet_example PRIVATE sinnet::sinnet)
set_target_properties(sinnet_example PROPERTIES OUTPUT_NAME example)

if(BUILD_TESTING)
    find_package(GTest CONFIG QUIET)

    if(NOT GTest_FOUND AND SINNET_FETCH_GTEST)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(sinnet_tests
        tests/EventLoopTests.cpp
    )
    target_link_libraries(sinnet_tests
        PRIVATE
            sinnet::sinnet
            GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(sinnet_tests)
endif()
