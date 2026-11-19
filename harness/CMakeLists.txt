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

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Глобальные флаги оптимизации для всех исполняемых файлов
add_compile_options(-O3 -march=native -g -fno-omit-frame-pointer -Wall -Wextra)

# Глобальные флаги диагностики unrolling в зависимости от компилятора
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Rpass=loop-unroll -Rpass-missed=loop-unroll -Rpass-analysis=loop-unroll)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fopt-info-loop-all)
endif()

include(CheckIPOSupported)

find_package(Threads REQUIRED)
find_library(RT_LIBRARY rt)

function(add_harness_binary name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE include)
    target_link_libraries(${name} PRIVATE Threads::Threads)
    if(RT_LIBRARY)
        target_link_libraries(${name} PRIVATE ${RT_LIBRARY})
    endif()


    # lto setup
    check_ipo_supported(RESULT LTO_SUPPORTED)
    if (LTO_SUPPORTED)
        set_property(TARGET ${name} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif ()
endfunction()

# --- Бинарники, не требующие DPDK -- собираются всегда ---
add_harness_binary(producer src/producer.cpp)
add_harness_binary(consumer src/consumer.cpp)
add_harness_binary(sender   src/sender.cpp)
add_harness_binary(receiver src/receiver.cpp)

# Смещение часов между хостами -- четыре метки, обычные сокеты, DPDK не нужен.
add_harness_binary(clock_probe src/clock_probe.cpp)

# --- DPDK-транспорт -- опционально ---
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(DPDK IMPORTED_TARGET libdpdk)
endif()

if(DPDK_FOUND)
    message(STATUS "DPDK ${DPDK_VERSION} found, building DPDK transport")

    function(add_dpdk_binary name)
        add_harness_binary(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE PkgConfig::DPDK)
    endfunction()

    add_dpdk_binary(dpdk_probe src/dpdk_probe.cpp)

    # Основная пара -- на ней сняты все числа в RESULTS.md.
    add_dpdk_binary(dpdk_sender   src/dpdk_sender.cpp)
    add_dpdk_binary(dpdk_reciever src/dpdk_reciever.cpp)

#    add_dpdk_binary(dpdk_sender_mq   src/dpdk_sender_mq.cpp)
#    add_dpdk_binary(dpdk_reciever_ro src/dpdk_reciever_ro.cpp)
else()
    message(STATUS "DPDK not found, skipping DPDK transport targets")
endif()
