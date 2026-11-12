# LAST LIGHT - Environmental & World Intelligence (standalone build)
CXX      ?= g++
CXXSTD   ?= -std=c++17
WARN     := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion \
            -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference \
            -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wcast-align
OPT      ?= -O2
CXXFLAGS ?= $(CXXSTD) $(WARN) $(OPT)
BUILD    := build
AIS_DIR  ?= ../acoustic_intelligence
ENV_DIR  ?= ../environmental_systems

CORE_SRC := $(wildcard src/*.cpp)
DEP_SRC  := $(wildcard $(AIS_DIR)/src/*.cpp) $(wildcard $(ENV_DIR)/src/*.cpp) $(ENV_DIR)/bridge/AisAcousticBridge.cpp
INT_SRC  := integration/WorldIntelligence.cpp
INC      := -Iinclude -Iintegration -I$(AIS_DIR)/include -I$(ENV_DIR)/include -I$(ENV_DIR)/bridge
UNIT     := tests/test_main.cpp tests/test_weather.cpp tests/test_hazards.cpp tests/test_squad.cpp
HDRS     := $(wildcard include/llworld/*.h integration/*.h tests/*.h $(AIS_DIR)/include/ais/*.h $(ENV_DIR)/include/llenv/*.h $(ENV_DIR)/bridge/*.h)
DATA     := -DWI_DATA_DIR='"$(CURDIR)/data"'

.PHONY: all test unit demo sanitize ue-compat clean

all: $(BUILD)/wi_unit_tests $(BUILD)/wi_tests $(BUILD)/world_demo

# Core-only tests: prove the three subsystems build without AIS / power grid.
$(BUILD)/wi_unit_tests: $(CORE_SRC) $(UNIT) $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -Iinclude -Itests $(CORE_SRC) $(UNIT) -o $@

$(BUILD)/wi_tests: $(CORE_SRC) $(DEP_SRC) $(INT_SRC) $(wildcard tests/*.cpp) $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) -Itests $(DATA) $(CORE_SRC) $(DEP_SRC) $(INT_SRC) $(wildcard tests/*.cpp) -o $@

$(BUILD)/world_demo: $(CORE_SRC) $(DEP_SRC) $(INT_SRC) examples/world_demo.cpp $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $(DATA) $(CORE_SRC) $(DEP_SRC) $(INT_SRC) examples/world_demo.cpp -o $@

unit: $(BUILD)/wi_unit_tests
	./$(BUILD)/wi_unit_tests

test: $(BUILD)/wi_unit_tests $(BUILD)/wi_tests
	./$(BUILD)/wi_unit_tests
	./$(BUILD)/wi_tests

demo: $(BUILD)/world_demo
	./$(BUILD)/world_demo

sanitize:
	$(MAKE) BUILD=build-asan OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" test

ue-compat:
	@mkdir -p build-uecompat
	for f in $(CORE_SRC) $(INT_SRC); do $(CXX) $(CXXSTD) $(WARN) -O2 -fno-exceptions -fno-rtti $(INC) -c $$f -o build-uecompat/$$(basename $$f).o || exit 1; done
	@echo "core + integration compile with -fno-exceptions -fno-rtti"

clean:
	rm -rf build build-asan build-uecompat
