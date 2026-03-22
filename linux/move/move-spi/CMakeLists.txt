cmake_minimum_required(VERSION 3.10)
project(ablspi C)

add_library(ablspi STATIC src/ablspi.c)

target_include_directories(ablspi PUBLIC include)

target_compile_options(
    ablspi
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Werror
)
