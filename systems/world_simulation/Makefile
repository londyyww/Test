# LAST LIGHT - World Simulation & Scavenging (standalone build)
CXX      ?= g++
CXXSTD   ?= -std=c++17
WARN     := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion \
            -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference \
            -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wcast-align
OPT      ?= -O2
CXXFLAGS ?= $(CXXSTD) $(WARN) $(OPT)
BUILD    := build
WI_DIR   ?= ../world_intelligence
AIS_DIR  ?= ../acoustic_intelligence
ENV_DIR  ?= ../environmental_systems

# Portable core: loot + containers + world simulation + content loader.
# It reuses only the llworld INI parser and shared types (header + Ini.cpp).
CORE_SRC := $(wildcard src/*.cpp)
CORE_DEP := $(WI_DIR)/src/Ini.cpp
CORE_INC := -Iinclude -I$(WI_DIR)/include
# Full stack: existing systems from PR #1-#3 plus the integration host.
STACK_SRC := $(wildcard $(WI_DIR)/src/*.cpp) $(WI_DIR)/integration/WorldIntelligence.cpp \
             $(wildcard $(AIS_DIR)/src/*.cpp) $(wildcard $(ENV_DIR)/src/*.cpp) $(ENV_DIR)/bridge/AisAcousticBridge.cpp
INT_SRC  := integration/MissionWorldHost.cpp
FULL_INC := $(CORE_INC) -Iintegration -I$(WI_DIR)/integration -I$(AIS_DIR)/include -I$(ENV_DIR)/include -I$(ENV_DIR)/bridge
UNIT     := tests/test_main.cpp tests/test_loot.cpp tests/test_containers.cpp tests/test_sim.cpp tests/test_config.cpp tests/test_adversarial.cpp
HDRS     := $(wildcard include/*/*.h integration/*.h tests/*.h tests/fixtures/*.h)
DATA     := -DWS_DATA_DIR='"$(CURDIR)/data"'

.PHONY: all test unit scenarios bench sanitize ue-compat regress clean

all: $(BUILD)/ws_unit_tests $(BUILD)/ws_tests $(BUILD)/scenarios $(BUILD)/benchmark

# Core-only tests: prove the portable core builds without AIS / power grid / Unreal.
$(BUILD)/ws_unit_tests: $(CORE_SRC) $(UNIT) $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(CORE_INC) -Itests $(DATA) $(CORE_SRC) $(CORE_DEP) $(UNIT) -o $@

$(BUILD)/ws_tests: $(CORE_SRC) $(INT_SRC) $(wildcard tests/*.cpp) $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(FULL_INC) -Itests $(DATA) $(CORE_SRC) $(STACK_SRC) $(INT_SRC) $(wildcard tests/*.cpp) -o $@

$(BUILD)/scenarios: $(CORE_SRC) $(INT_SRC) examples/scenarios.cpp $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(FULL_INC) -Itests $(DATA) $(CORE_SRC) $(STACK_SRC) $(INT_SRC) examples/scenarios.cpp -o $@

$(BUILD)/benchmark: $(CORE_SRC) examples/benchmark.cpp $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXSTD) $(WARN) -O2 $(CORE_INC) -Itests $(DATA) $(CORE_SRC) $(CORE_DEP) examples/benchmark.cpp -o $@

unit: $(BUILD)/ws_unit_tests
	./$(BUILD)/ws_unit_tests

test: $(BUILD)/ws_unit_tests $(BUILD)/ws_tests
	./$(BUILD)/ws_unit_tests
	./$(BUILD)/ws_tests

scenarios: $(BUILD)/scenarios
	./$(BUILD)/scenarios

bench: $(BUILD)/benchmark
	./$(BUILD)/benchmark

sanitize:
	$(MAKE) BUILD=build-asan OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" test

# Unreal builds without exceptions and RTTI by default.
ue-compat:
	@mkdir -p build-uecompat
	for f in $(CORE_SRC) $(INT_SRC); do $(CXX) $(CXXSTD) $(WARN) -O2 -fno-exceptions -fno-rtti $(FULL_INC) -c $$f -o build-uecompat/$$(basename $$f).o || exit 1; done
	@echo "core + integration compile with -fno-exceptions -fno-rtti"

# Previous suites (PR #1-#3) must keep passing.
regress:
	$(MAKE) -C $(AIS_DIR) test
	$(MAKE) -C $(ENV_DIR) test
	$(MAKE) -C $(WI_DIR) test

clean:
	rm -rf build build-asan build-uecompat
