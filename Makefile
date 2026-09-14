CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pthread -Iinclude
LDFLAGS  := -pthread

BIN_DIR := bin

.PHONY: all clean

all: $(BIN_DIR)/servidor $(BIN_DIR)/cliente

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/servidor: src/servidor/main.cpp src/ipc.cpp include/protocol.h include/ipc.h | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) src/servidor/main.cpp src/ipc.cpp -o $@ $(LDFLAGS)

$(BIN_DIR)/cliente: src/cliente/main.cpp src/ipc.cpp include/protocol.h include/ipc.h | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) src/cliente/main.cpp src/ipc.cpp -o $@ $(LDFLAGS)

clean:
	rm -rf $(BIN_DIR)
	rm -f log.txt
	rm -f /tmp/db_req /tmp/db_resp
