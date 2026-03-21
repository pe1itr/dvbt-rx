cmake_minimum_required(VERSION 3.16)
project(rbdvbt_qpsk_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(rbdvbt_qpsk_probe
    src/main.cpp
)

target_compile_options(rbdvbt_qpsk_probe PRIVATE -O2 -Wall -Wextra -Wpedantic)
