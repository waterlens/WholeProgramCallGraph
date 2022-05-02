cmake_minimum_required(VERSION 3.17)
project(WholeProgramCallGraph)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM REQUIRED CONFIG)
find_package(fmt)

message(STATUS "LLVM_DIR = ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
add_executable(WholeProgramCallGraph main.cpp CallGraph.h CallGraph.cpp)
explicit_llvm_config(WholeProgramCallGraph core support irreader)
target_link_libraries(WholeProgramCallGraph PRIVATE fmt::fmt)