# Standalone build for the fan-out benchmark harness. No external dependencies
# beyond a C++17 compiler and librt (POSIX shared memory).
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -g -fno-omit-frame-pointer -Wall -Wextra -Iinclude
LDLIBS   ?= -lrt -lpthread

BIN := bin
BINARIES := $(BIN)/producer $(BIN)/consumer $(BIN)/sender $(BIN)/receiver

.PHONY: all clean test

all: $(BINARIES)

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

test: $(BIN)/test_harness
	./$(BIN)/test_harness

$(BIN)/test_harness: test/test_harness.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -rf $(BIN)
