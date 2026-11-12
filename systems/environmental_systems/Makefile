# LAST LIGHT - Environmental Interaction & Power Grid (standalone build)
CXX      ?= g++
CXXSTD   ?= -std=c++17
WARN     := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion \
            -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference \
            -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wcast-align
OPT      ?= -O2
CXXFLAGS ?= $(CXXSTD) $(WARN) $(OPT)
BUILD    := build
AIS_DIR  ?= ../acoustic_intelligence

INC      := -Iinclude
AIS_INC  := -I$(AIS_DIR)/include -Ibridge

LIB_SRC  := $(wildcard src/*.cpp)
AIS_SRC  := $(wildcard $(AIS_DIR)/src/*.cpp)
TEST_SRC := $(wildcard tests/*.cpp)
HDRS     := $(wildcard include/llenv/*.h) $(wildcard bridge/*.h) $(wildcard tests/*.h) $(wildcard $(AIS_DIR)/include/ais/*.h)

.PHONY: all test demo sanitize ue-compat clean

all: $(BUILD)/env_tests $(BUILD)/facility_demo

$(BUILD)/env_tests: $(LIB_SRC) $(AIS_SRC) bridge/AisAcousticBridge.cpp $(TEST_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $(AIS_INC) -Itests -DENV_DATA_DIR='"$(CURDIR)/data"' \
	    $(LIB_SRC) $(AIS_SRC) bridge/AisAcousticBridge.cpp $(TEST_SRC) -o $@

$(BUILD)/facility_demo: $(LIB_SRC) $(AIS_SRC) bridge/AisAcousticBridge.cpp examples/facility_demo.cpp $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $(AIS_INC) -DENV_DATA_DIR='"$(CURDIR)/data"' \
	    $(LIB_SRC) $(AIS_SRC) bridge/AisAcousticBridge.cpp examples/facility_demo.cpp -o $@

test: $(BUILD)/env_tests
	./$(BUILD)/env_tests

demo: $(BUILD)/facility_demo
	./$(BUILD)/facility_demo

sanitize:
	$(MAKE) BUILD=build-asan OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" test

# Core (without the AIS bridge) the way Unreal compiles modules.
ue-compat:
	@mkdir -p build-uecompat
	for f in $(LIB_SRC); do $(CXX) $(CXXSTD) $(WARN) -O2 -fno-exceptions -fno-rtti $(INC) -c $$f -o build-uecompat/$$(basename $$f).o || exit 1; done
	$(CXX) $(CXXSTD) $(WARN) -O2 -fno-exceptions -fno-rtti $(INC) $(AIS_INC) -c bridge/AisAcousticBridge.cpp -o build-uecompat/bridge.o
	@echo "core + bridge compile with -fno-exceptions -fno-rtti"

clean:
	rm -rf build build-asan build-uecompat
