cmake_minimum_required(VERSION 3.20)
project(lvt_package_consumer LANGUAGES CXX)

# Stands in for a third-party project consuming lvt. Deliberately a separate
# CMake project built against an *installed* lvt, so it exercises the exported
# package rather than the build tree: if the install rules, lvtConfig.cmake or
# the public headers regress, this stops compiling.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lvt CONFIG REQUIRED)

add_executable(package_consumer main.cpp)
target_link_libraries(package_consumer PRIVATE lvt::core)

message(STATUS "lvt tools dir: ${LVT_TOOLS_DIR}")
message(STATUS "lvt TAP DLLs:  ${LVT_TAP_DLLS}")

lvt_copy_tap_dlls(package_consumer)
