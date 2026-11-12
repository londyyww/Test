cmake_minimum_required(VERSION 3.16)
project(LastLightAcousticIntelligence LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  set(AIS_WARNINGS /W4 /WX /permissive-)
else()
  set(AIS_WARNINGS -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
      -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference
      -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wcast-align)
endif()

file(GLOB AIS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(ais STATIC ${AIS_SOURCES})
target_include_directories(ais PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(ais PRIVATE ${AIS_WARNINGS})

file(GLOB AIS_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(ais_tests ${AIS_TEST_SOURCES})
target_include_directories(ais_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(ais_tests PRIVATE AIS_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(ais_tests PRIVATE ${AIS_WARNINGS})
target_link_libraries(ais_tests PRIVATE ais)

add_executable(ais_demo ${CMAKE_CURRENT_SOURCE_DIR}/examples/acoustic_demo.cpp)
target_compile_options(ais_demo PRIVATE ${AIS_WARNINGS})
target_link_libraries(ais_demo PRIVATE ais)

enable_testing()
add_test(NAME ais_tests COMMAND ais_tests)
