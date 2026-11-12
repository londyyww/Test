cmake_minimum_required(VERSION 3.16)
project(LastLightEnvironmentalSystems LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(AIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../acoustic_intelligence CACHE PATH "Acoustic Intelligence System source")

if(MSVC)
  set(ENV_WARNINGS /W4 /WX /permissive-)
else()
  set(ENV_WARNINGS -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
      -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference
      -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wcast-align)
endif()

# Core: no dependency on AIS.
file(GLOB ENV_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(llenv STATIC ${ENV_SOURCES})
target_include_directories(llenv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(llenv PRIVATE ${ENV_WARNINGS})

# AIS + bridge.
file(GLOB AIS_SOURCES CONFIGURE_DEPENDS ${AIS_DIR}/src/*.cpp)
add_library(ais_core STATIC ${AIS_SOURCES})
target_include_directories(ais_core PUBLIC ${AIS_DIR}/include)
target_compile_options(ais_core PRIVATE ${ENV_WARNINGS})

add_library(llenv_ais_bridge STATIC ${CMAKE_CURRENT_SOURCE_DIR}/bridge/AisAcousticBridge.cpp)
target_include_directories(llenv_ais_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/bridge)
target_link_libraries(llenv_ais_bridge PUBLIC llenv ais_core)
target_compile_options(llenv_ais_bridge PRIVATE ${ENV_WARNINGS})

file(GLOB ENV_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(env_tests ${ENV_TEST_SOURCES})
target_include_directories(env_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(env_tests PRIVATE ENV_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(env_tests PRIVATE ${ENV_WARNINGS})
target_link_libraries(env_tests PRIVATE llenv_ais_bridge)

add_executable(facility_demo ${CMAKE_CURRENT_SOURCE_DIR}/examples/facility_demo.cpp)
target_compile_definitions(facility_demo PRIVATE ENV_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(facility_demo PRIVATE ${ENV_WARNINGS})
target_link_libraries(facility_demo PRIVATE llenv_ais_bridge)

enable_testing()
add_test(NAME env_tests COMMAND env_tests)
