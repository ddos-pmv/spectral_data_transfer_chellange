cmake_minimum_required(VERSION 3.15)
project(harness VERSION 1.0 LANGUAGES CXX)

# An unset CMAKE_BUILD_TYPE builds without optimisation, silently -- and then
# the benchmark measures the wrong binary.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(producer
        src/producer.cpp
)

target_include_directories(producer PRIVATE include)
target_compile_features(producer PRIVATE cxx_std_17)

add_executable(consumer
        src/consumer.cpp
)

target_include_directories(consumer PRIVATE include)
target_compile_features(consumer PRIVATE cxx_std_17)