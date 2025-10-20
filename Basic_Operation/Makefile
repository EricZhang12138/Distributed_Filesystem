# Compiler and flags
CXX := g++
CXXFLAGS := -std=c++17 -Wall

# Directories
SERVER_DIR := server
CLIENT_DIR := client
PROTO_DIR := protos
# All generated files will go into the build directory
BUILD_DIR := build

# Protobuf and gRPC compiler
PROTOC := protoc
GRPC_CPP_PLUGIN_PATH ?= $(shell which grpc_cpp_plugin)
GRPC_PLUGIN := --plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN_PATH)

# Source and Generated Files
PROTO_FILE := $(PROTO_DIR)/afs_operation.proto
# Prepend build directory to all generated file paths
PROTO_GEN_CC := $(BUILD_DIR)/afs_operation.pb.cc $(BUILD_DIR)/afs_operation.grpc.pb.cc
PROTO_GEN_H := $(PROTO_GEN_CC:.cc=.h)
PROTO_GEN_O := $(PROTO_GEN_CC:.cc=.o)

# Server files
SERVER_SRC := $(SERVER_DIR)/afs_operation.cpp
SERVER_OBJ := $(BUILD_DIR)/afs_operation.o
SERVER_EXEC := afs_server

# Client files
CLIENT_SRC := $(CLIENT_DIR)/client_interface.cpp
CLIENT_OBJ := $(BUILD_DIR)/client_interface.o
CLIENT_EXEC := afs_client

# pkg-config settings for linking
# We add -I$(BUILD_DIR) so the compiler can find the generated headers
LDFLAGS := $(shell pkg-config --libs grpc++ protobuf)
CXXFLAGS += $(shell pkg-config --cflags grpc++ protobuf) -I$(BUILD_DIR)

# --- TARGETS ---

# Default target
all: $(SERVER_EXEC) $(CLIENT_EXEC)

# Rule to create the server executable
$(SERVER_EXEC): $(SERVER_OBJ) $(PROTO_GEN_O)
	@echo "Linking server executable: $@"
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Rule to create the client executable
$(CLIENT_EXEC): $(CLIENT_OBJ) $(PROTO_GEN_O)
	@echo "Linking client executable: $@"
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Rule to generate C++ code from .proto file into the build directory
# This rule depends on the build directory existing (order-only prerequisite)
$(PROTO_GEN_CC) $(PROTO_GEN_H): $(PROTO_FILE) | $(BUILD_DIR)
	@echo "Generating C++ from $<"
	$(PROTOC) --cpp_out=$(BUILD_DIR) --grpc_out=$(BUILD_DIR) $(GRPC_PLUGIN) -I$(PROTO_DIR) $(PROTO_FILE)

# --- COMPILATION RULES ---

# Rule to compile the server source file
$(SERVER_OBJ): $(SERVER_SRC) $(PROTO_GEN_H)
	@echo "Compiling server source: $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule to compile the client source file
$(CLIENT_OBJ): $(CLIENT_SRC) $(PROTO_GEN_H)
	@echo "Compiling client source: $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule to compile generated .pb.cc files
$(BUILD_DIR)/%.pb.o: $(BUILD_DIR)/%.pb.cc
	@echo "Compiling generated protobuf file: $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule to compile generated .grpc.pb.cc files
$(BUILD_DIR)/%.grpc.pb.o: $(BUILD_DIR)/%.grpc.pb.cc
	@echo "Compiling generated gRPC file: $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# --- UTILITY RULES ---

# Rule to create the build directory
$(BUILD_DIR):
	@echo "Creating build directory: $@"
	mkdir -p $(BUILD_DIR)

# Clean up generated files
clean:
	@echo "Cleaning up..."
	rm -f $(SERVER_EXEC) $(CLIENT_EXEC)
	rm -rf $(BUILD_DIR)

# Phony targets
.PHONY: all clean