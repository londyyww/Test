cmake_minimum_required(VERSION 3.16)
project(LastLightWorldSimulation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(WI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../world_intelligence CACHE PATH "World intelligence (PR #3) source")
set(AIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../acoustic_intelligence CACHE PATH "Acoustic Intelligence System source")
set(ENV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../environmental_systems CACHE PATH "Environmental power grid source")

if(MSVC)
  set(WS_WARNINGS /W4 /WX /permissive-)
else()
  set(WS_WARNINGS -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
      -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference
      -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wcast-align)
endif()
set(WS_DATA_DEF WS_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# Portable core: loot, containers, world simulation, content loader.
# Reuses only the llworld INI parser and shared types.
file(GLOB WS_CORE CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(llws_core STATIC ${WS_CORE} ${WI_DIR}/src/Ini.cpp)
target_include_directories(llws_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${WI_DIR}/include)
target_compile_options(llws_core PRIVATE ${WS_WARNINGS})

# Existing environmental stack (PR #1-#3) for the integration host.
file(GLOB WI_SRC CONFIGURE_DEPENDS ${WI_DIR}/src/*.cpp)
list(REMOVE_ITEM WI_SRC ${WI_DIR}/src/Ini.cpp)
file(GLOB AIS_SRC CONFIGURE_DEPENDS ${AIS_DIR}/src/*.cpp)
file(GLOB ENV_SRC CONFIGURE_DEPENDS ${ENV_DIR}/src/*.cpp)
add_library(llws_stack STATIC ${WI_SRC} ${WI_DIR}/integration/WorldIntelligence.cpp ${AIS_SRC} ${ENV_SRC} ${ENV_DIR}/bridge/AisAcousticBridge.cpp)
target_include_directories(llws_stack PUBLIC ${WI_DIR}/integration ${AIS_DIR}/include ${ENV_DIR}/include ${ENV_DIR}/bridge)
target_link_libraries(llws_stack PUBLIC llws_core)
target_compile_options(llws_stack PRIVATE ${WS_WARNINGS})

add_library(llws_integration STATIC ${CMAKE_CURRENT_SOURCE_DIR}/integration/MissionWorldHost.cpp)
target_include_directories(llws_integration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/integration)
target_link_libraries(llws_integration PUBLIC llws_stack)
target_compile_options(llws_integration PRIVATE ${WS_WARNINGS})

set(WS_UNIT_TESTS tests/test_main.cpp tests/test_loot.cpp tests/test_containers.cpp tests/test_sim.cpp tests/test_config.cpp tests/test_adversarial.cpp)
add_executable(ws_unit_tests ${WS_UNIT_TESTS})
target_include_directories(ws_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(ws_unit_tests PRIVATE ${WS_DATA_DEF})
target_compile_options(ws_unit_tests PRIVATE ${WS_WARNINGS})
target_link_libraries(ws_unit_tests PRIVATE llws_core)

file(GLOB WS_ALL_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(ws_tests ${WS_ALL_TESTS})
target_include_directories(ws_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(ws_tests PRIVATE ${WS_DATA_DEF})
target_compile_options(ws_tests PRIVATE ${WS_WARNINGS})
target_link_libraries(ws_tests PRIVATE llws_integration)

add_executable(scenarios examples/scenarios.cpp)
target_include_directories(scenarios PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(scenarios PRIVATE ${WS_DATA_DEF})
target_compile_options(scenarios PRIVATE ${WS_WARNINGS})
target_link_libraries(scenarios PRIVATE llws_integration)

add_executable(benchmark examples/benchmark.cpp)
target_include_directories(benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(benchmark PRIVATE ${WS_DATA_DEF})
target_compile_options(benchmark PRIVATE ${WS_WARNINGS})
target_link_libraries(benchmark PRIVATE llws_core)

enable_testing()
add_test(NAME ws_unit_tests COMMAND ws_unit_tests)
add_test(NAME ws_tests COMMAND ws_tests)
