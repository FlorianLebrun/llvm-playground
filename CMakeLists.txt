cmake_minimum_required(VERSION 3.16)
include(./cmake-toolkit.cmake)

project(LLVMPlayground
  LANGUAGES CXX CSharp
  VERSION 1.0.0
)
set(CMAKE_CXX_STANDARD 17)

# Find used package
find_package(llvm-jit REQUIRED PATHS "${CMAKE_SOURCE_DIR}/node_modules/llvm-jit")

# Add own library
add_subdirectory(sample)
