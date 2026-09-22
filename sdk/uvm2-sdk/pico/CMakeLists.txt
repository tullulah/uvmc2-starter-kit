# Entry point for the pico-sdk UVM2 build. The substance is in ../uvm2_pico.cmake;
# this exists because CMake needs a project directory and pico_sdk_import.cmake
# has to sit next to the CMakeLists that includes it.
cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(UVM2_SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
include(${UVM2_SDK_DIR}/uvm2_pico.cmake)
