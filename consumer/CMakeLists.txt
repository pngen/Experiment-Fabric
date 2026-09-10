# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Independent downstream consumer.
#
# This project is deliberately kept outside the Experiment Fabric source and
# build trees. It consumes only the installed CMake package and the public
# headers, so it proves that the exported package is usable by a foreign project
# rather than by an in-tree example.

cmake_minimum_required(VERSION 3.25)

project(ExperimentFabricConsumer
        VERSION 1.0.0
        DESCRIPTION "Independent downstream consumer of the Experiment Fabric package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /W4 /WX)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

find_package(ExperimentFabric 1.0 REQUIRED)

add_executable(ef_consumer main.cpp)
target_link_libraries(ef_consumer PRIVATE experiment_fabric::experiment_fabric)
