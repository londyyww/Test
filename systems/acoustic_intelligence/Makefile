# LAST LIGHT - Acoustic Intelligence System (standalone build)
CXX      ?= g++
CXXSTD   ?= -std=c++17
WARN     := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion \
            -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference \
            -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wcast-align
OPT      ?= -O2
CXXFLAGS ?= $(CXXSTD) $(WARN) $(OPT)
INC      := -Iinclude
BUILD    := build

LIB_SRC  := $(wildcard src/*.cpp)
TEST_SRC := $(wildcard tests/*.cpp)
LIB_OBJ  := $(patsubst src/%.cpp,$(BUILD)/src/%.o,$(LIB_SRC))

.PHONY: all test demo sanitize ue-compat clean

all: $(BUILD)/ais_tests $(BUILD)/ais_demo

$(BUILD)/src/%.o: src/%.cpp $(wildcard include/ais/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

$(BUILD)/libais.a: $(LIB_OBJ)
	ar rcs $@ $^

$(BUILD)/ais_tests: $(TEST_SRC) $(BUILD)/libais.a $(wildcard tests/*.h)
	$(CXX) $(CXXFLAGS) $(INC) -Itests -DAIS_DATA_DIR='"$(CURDIR)/data"' $(TEST_SRC) $(BUILD)/libais.a -o $@

$(BUILD)/ais_demo: examples/acoustic_demo.cpp $(BUILD)/libais.a
	$(CXX) $(CXXFLAGS) $(INC) $< $(BUILD)/libais.a -o $@

test: $(BUILD)/ais_tests
	./$(BUILD)/ais_tests

demo: $(BUILD)/ais_demo
	./$(BUILD)/ais_demo

# Address + undefined-behaviour sanitizers over the full test suite.
sanitize:
	$(MAKE) BUILD=build-asan OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" test

# Core library built the way Unreal compiles modules: no exceptions, no RTTI.
ue-compat:
	@mkdir -p build-uecompat
	for f in $(LIB_SRC); do $(CXX) $(CXXSTD) $(WARN) -O2 -fno-exceptions -fno-rtti $(INC) -c $$f -o build-uecompat/$$(basename $$f).o || exit 1; done
	@echo "core compiles with -fno-exceptions -fno-rtti"

clean:
	rm -rf build build-asan build-uecompat
