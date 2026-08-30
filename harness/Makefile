# Standalone build for the fan-out benchmark harness. No external dependencies
# beyond a C++17 compiler and librt (POSIX shared memory).
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -march=native -g -fno-omit-frame-pointer -Wall -Wextra -Iinclude
LDLIBS   ?= -lrt -lpthread

BIN := bin
BINARIES := $(BIN)/producer $(BIN)/consumer $(BIN)/sender $(BIN)/receiver \
            $(BIN)/clock_probe

# DPDK-транспорт собирается, только если в системе есть libdpdk: на машине без
# него `make` должен по-прежнему давать рабочий UDP-baseline, а не падать.
# pkg-config сам отдаёт нужный -march=native и -include rte_config.h.
DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk 2>/dev/null)
DPDK_LIBS   := $(shell pkg-config --libs libdpdk 2>/dev/null)
ifneq ($(DPDK_LIBS),)
DPDK_BINARIES := $(BIN)/dpdk_sender $(BIN)/dpdk_reciever $(BIN)/dpdk_probe
endif

.PHONY: all clean test

all: $(BINARIES) $(DPDK_BINARIES)

$(BIN):
	mkdir -p $(BIN)

$(BIN)/sender: src/sender.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

$(BIN)/receiver: src/receiver.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

$(BIN)/producer: src/producer.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

$(BIN)/consumer: src/consumer.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

# Смещение часов между хостами: четыре метки, круг и разворот считаются каждый
# на своих часах, поэтому величина не зависит от их согласования. Обычные
# сокеты, DPDK не нужен -- собирается всегда.
$(BIN)/clock_probe: src/clock_probe.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

# Динамическая линковка намеренно: драйверы (net_af_xdp, net_ena) живут
# отдельными .so и подгружаются EAL'ом в рантайме, в бинарь не попадают.
$(BIN)/dpdk_%: src/dpdk_%.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $(DPDK_CFLAGS) $< -o $@ $(DPDK_LIBS) $(LDLIBS)

test: $(BIN)/test_harness
	./$(BIN)/test_harness

$(BIN)/test_harness: test/test_harness.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -rf $(BIN)
