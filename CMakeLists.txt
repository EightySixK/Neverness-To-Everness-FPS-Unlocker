cmake_minimum_required(VERSION 3.15)
project(NTE-FPS-Unlocker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(version_proxy SHARED src/version_proxy.cpp version.def)
set_target_properties(version_proxy PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
    SUFFIX ".dll"
)
