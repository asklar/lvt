cmake_minimum_required(VERSION 3.20)
project(lvt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(wil CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(lvt
    src/main.cpp
    src/target.cpp
    src/framework_detector.cpp
    src/tree_builder.cpp
    src/json_serializer.cpp
    src/screenshot.cpp
    src/providers/win32_provider.cpp
    src/providers/comctl_provider.cpp
    src/providers/xaml_provider.cpp
    src/providers/winui3_provider.cpp
)

target_include_directories(lvt PRIVATE src)

target_link_libraries(lvt PRIVATE
    WIL::WIL
    nlohmann_json::nlohmann_json
    dwmapi
    d3d11
    dxgi
    windowsapp
    windowscodecs
)

target_compile_definitions(lvt PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINRT_LEAN_AND_MEAN
)
