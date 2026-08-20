cmake_minimum_required(VERSION 3.15)
project(harness VERSION 1.0 LANGUAGES CXX)

# Задаем Release по умолчанию, если тип сборки не указан
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Глобальные настройки стандарта C++
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Глобальные флаги оптимизации для всех исполняемых файлов
add_compile_options(-O3)
add_compile_options(-march=native)
add_compile_options(-g)

# Глобальные флаги диагностики unrolling в зависимости от компилятора
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Успехи, пропуски и подробный анализ причин для Clang
    add_compile_options(-Rpass=loop-unroll -Rpass-missed=loop-unroll -Rpass-analysis=loop-unroll)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Абсолютно вся информация по оптимизации циклов для GCC
    add_compile_options(-fopt-info-loop-all)
endif()

# --- Определение исполняемых файлов ---

# 1. Producer
add_executable(producer src/producer.cpp)
target_include_directories(producer PRIVATE include)

# 2. Consumer
add_executable(consumer src/consumer.cpp)
target_include_directories(consumer PRIVATE include)

# 3. Sender
add_executable(sender src/sender.cpp)
target_include_directories(sender PRIVATE include)

# 4. Receiver
add_executable(receiver src/receiver.cpp)
target_include_directories(receiver PRIVATE include)



find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK IMPORTED_TARGET libdpdk)

if (DPDK_FOUND)
    message(STATUS "DPDK ${DPDK_VERSION} found, building DPDK transport")

    add_executable(dpdk_probe src/dpdk_probe.cpp)
    target_include_directories(dpdk_probe PRIVATE include)
    target_link_libraries(dpdk_probe PRIVATE PkgConfig::DPDK)


    add_executable(dpdk_sender src/dpdk_sender.cpp)
    target_include_directories(dpdk_sender PRIVATE include)
    target_link_libraries(dpdk_sender PRIVATE PkgConfig::DPDK)

    add_executable(dpdk_reciever src/dpdk_reciever.cpp)
    target_include_directories(dpdk_reciever PRIVATE include)
    target_link_libraries(dpdk_reciever PRIVATE PkgConfig::DPDK)
else()
    message(STATUS "DPDK not found, skipping DPDK transport targets")
endif()
