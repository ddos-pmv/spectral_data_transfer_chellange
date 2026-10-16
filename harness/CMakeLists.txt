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

# Бинарники всегда лежат в harness/bin -- на этот путь жёстко рассчитаны все
# scripts/*.sh (RECEIVER_BIN=bin/dpdk_reciever и т.п.), независимо от того, из
# какого каталога собиралось (build/, build-pgo/, ...).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Глобальные флаги оптимизации для всех исполняемых файлов
add_compile_options(-O3 -march=native -g -fno-omit-frame-pointer -Wall -Wextra)

# Глобальные флаги диагностики unrolling в зависимости от компилятора
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Успехи, пропуски и подробный анализ причин для Clang
    add_compile_options(-Rpass=loop-unroll -Rpass-missed=loop-unroll -Rpass-analysis=loop-unroll)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Абсолютно вся информация по оптимизации циклов для GCC
    add_compile_options(-fopt-info-loop-all)
endif()

find_package(Threads REQUIRED)
# shm_open/shm_unlink; на части современных систем уже часть libc и отдельной
# библиотеки не имеет -- линкуем, только если find_library её нашла.
find_library(RT_LIBRARY rt)

# Один бинарник харнесса: include/ в путях поиска, -lpthread (-lrt, если есть)
# -- то же самое, что раньше было LDLIBS в Makefile.
function(add_harness_binary name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE include)
    target_link_libraries(${name} PRIVATE Threads::Threads)
    if(RT_LIBRARY)
        target_link_libraries(${name} PRIVATE ${RT_LIBRARY})
    endif()
endfunction()

# --- Бинарники, не требующие DPDK -- собираются всегда ---

add_harness_binary(producer src/producer.cpp)
add_harness_binary(consumer src/consumer.cpp)
add_harness_binary(sender   src/sender.cpp)
add_harness_binary(receiver src/receiver.cpp)

# Смещение часов между хостами -- четыре метки, обычные сокеты, DPDK не нужен.
add_harness_binary(clock_probe src/clock_probe.cpp)

# --- DPDK-транспорт -- опционально ---
#
# На машине без libdpdk (и даже без самого pkg-config) пять бинарников выше
# всё равно должны собраться как рабочий UDP-baseline, а не свалить всю
# конфигурацию -- поэтому find_package(PkgConfig) без REQUIRED.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(DPDK IMPORTED_TARGET libdpdk)
endif()

if(DPDK_FOUND)
    message(STATUS "DPDK ${DPDK_VERSION} found, building DPDK transport")

    function(add_dpdk_binary name)
        add_harness_binary(${name} ${ARGN})
        # Динамическая линковка намеренно: драйверы (net_af_xdp, net_ena)
        # живут отдельными .so и подгружаются EAL'ом в рантайме, в бинарь не
        # попадают -- pkg-config сам отдаёт нужные -march=native и
        # -include rte_config.h.
        target_link_libraries(${name} PRIVATE PkgConfig::DPDK)
    endfunction()

    add_dpdk_binary(dpdk_probe src/dpdk_probe.cpp)

    # Основная пара -- на ней сняты все числа в RESULTS.md.
    add_dpdk_binary(dpdk_sender   src/dpdk_sender.cpp)
    add_dpdk_binary(dpdk_reciever src/dpdk_reciever.cpp)

    # Вторая пара существует ради одного опыта: размазать получателя по
    # нескольким TX-очередям (dpdk_sender_mq) и починить возникающую
    # перестановку окном (dpdk_reciever_ro). Отдельными бинарниками, чтобы
    # основной путь остался нетронутым побайтово.
    add_dpdk_binary(dpdk_sender_mq   src/dpdk_sender_mq.cpp)
    add_dpdk_binary(dpdk_reciever_ro src/dpdk_reciever_ro.cpp)
else()
    message(STATUS "DPDK not found, skipping DPDK transport targets")
endif()
