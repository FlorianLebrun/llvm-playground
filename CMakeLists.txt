cmake_minimum_required(VERSION 3.16)

project(LLVMPlayground
  LANGUAGES CXX CSharp
  VERSION 1.0.0
)
set(CMAKE_CXX_STANDARD 17)

include(.tools/sources.cmake)

add_subdirectory(llvm)
add_subdirectory(sample)
