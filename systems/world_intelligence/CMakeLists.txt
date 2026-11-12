cmake_minimum_required(VERSION 3.16)
project(LastLightWorldIntelligence LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(AIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../acoustic_intelligence CACHE PATH "Acoustic Intelligence System source")
set(ENV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../environmental_systems CACHE PATH "Environmental power grid source")

if(MSVC)
  set(WI_WARNINGS /W4 /WX /permissive-)
else()
  set(WI_WARNINGS -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
      -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference
      -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wcast-align)
endif()

# Portable core: weather, hazards, squads. No AIS / power-grid dependency.
file(GLOB WI_CORE CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(llworld STATIC ${WI_CORE})
target_include_directories(llworld PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(llworld PRIVATE ${WI_WARNINGS})

# Existing systems (PR #1 and #2) and the integration layer.
file(GLOB AIS_SRC CONFIGURE_DEPENDS ${AIS_DIR}/src/*.cpp)
file(GLOB ENV_SRC CONFIGURE_DEPENDS ${ENV_DIR}/src/*.cpp)
add_library(wi_deps STATIC ${AIS_SRC} ${ENV_SRC} ${ENV_DIR}/bridge/AisAcousticBridge.cpp)
target_include_directories(wi_deps PUBLIC ${AIS_DIR}/include ${ENV_DIR}/include ${ENV_DIR}/bridge)
target_compile_options(wi_deps PRIVATE ${WI_WARNINGS})

add_library(llworld_integration STATIC ${CMAKE_CURRENT_SOURCE_DIR}/integration/WorldIntelligence.cpp)
target_include_directories(llworld_integration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/integration)
target_link_libraries(llworld_integration PUBLIC llworld wi_deps)
target_compile_options(llworld_integration PRIVATE ${WI_WARNINGS})

set(WI_UNIT_TESTS tests/test_main.cpp tests/test_weather.cpp tests/test_hazards.cpp tests/test_squad.cpp)
add_executable(wi_unit_tests ${WI_UNIT_TESTS})
target_include_directories(wi_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_options(wi_unit_tests PRIVATE ${WI_WARNINGS})
target_link_libraries(wi_unit_tests PRIVATE llworld)

file(GLOB WI_ALL_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(wi_tests ${WI_ALL_TESTS})
target_include_directories(wi_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(wi_tests PRIVATE WI_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(wi_tests PRIVATE ${WI_WARNINGS})
target_link_libraries(wi_tests PRIVATE llworld_integration)

add_executable(world_demo examples/world_demo.cpp)
target_compile_definitions(world_demo PRIVATE WI_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(world_demo PRIVATE ${WI_WARNINGS})
target_link_libraries(world_demo PRIVATE llworld_integration)

enable_testing()
add_test(NAME wi_unit_tests COMMAND wi_unit_tests)
add_test(NAME wi_tests COMMAND wi_tests)
