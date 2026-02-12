cmake_minimum_required(VERSION 3.20)

project(sinnet VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This project currently supports Linux only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
