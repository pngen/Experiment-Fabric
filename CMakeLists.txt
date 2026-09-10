# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

cmake_minimum_required(VERSION 3.25)

project(ExperimentFabric
        VERSION 1.0.0
        DESCRIPTION "Vendor-neutral C++20 runtime for governing autonomous experiments"
        LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(EXPERIMENT_FABRIC_BUILD_TOOLS "Build the coordinator, worker and inspection executables" ON)
option(EXPERIMENT_FABRIC_BUILD_EXAMPLES "Build the reference experiment examples" ON)
option(EXPERIMENT_FABRIC_BUILD_TESTS "Build the test suite" ON)
option(EXPERIMENT_FABRIC_BUILD_BENCHMARKS "Build the benchmark executable" ON)
option(EXPERIMENT_FABRIC_ENABLE_CUDA "Build the CUDA-backed reference trial example when a CUDA toolkit is present" ON)
option(EXPERIMENT_FABRIC_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(EXPERIMENT_FABRIC_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings and the optimisation policy.
#
# The flags are held in variables and applied per target rather than through a
# directory property or an interface library: a directory property would also be
# handed to the CUDA compiler, while an interface library would have to be
# exported alongside the runtime and would then leak the project's own warning
# policy into every downstream consumer.

set(EF_PROJECT_COMPILE_OPTIONS "")
set(EF_PROJECT_COMPILE_DEFINITIONS "")

if(MSVC)
  # C++20 conformance and a strict warning set that is required to build.
  list(APPEND EF_PROJECT_COMPILE_OPTIONS
    /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /W4 /MP)
  if(EXPERIMENT_FABRIC_WARNINGS_AS_ERRORS)
    list(APPEND EF_PROJECT_COMPILE_OPTIONS /WX)
  endif()
  list(APPEND EF_PROJECT_COMPILE_DEFINITIONS
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  # Release builds must not silently degrade to a debug heap.
  list(APPEND EF_PROJECT_COMPILE_OPTIONS $<$<CONFIG:Release>:/O2>)
else()
  list(APPEND EF_PROJECT_COMPILE_OPTIONS
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(EXPERIMENT_FABRIC_WARNINGS_AS_ERRORS)
    list(APPEND EF_PROJECT_COMPILE_OPTIONS -Werror)
  endif()
endif()

if(EXPERIMENT_FABRIC_ENABLE_ASAN)
  if(MSVC)
    list(APPEND EF_PROJECT_COMPILE_OPTIONS /fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO)
  else()
    list(APPEND EF_PROJECT_COMPILE_OPTIONS -fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
  endif()
endif()

# Applies the project's warning and optimisation policy to a C++ target.
function(ef_apply_project_options target)
  target_compile_options(${target} PRIVATE ${EF_PROJECT_COMPILE_OPTIONS})
  target_compile_definitions(${target} PRIVATE ${EF_PROJECT_COMPILE_DEFINITIONS})
endfunction()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------

add_library(experiment_fabric
  src/authority.cpp
  src/canonical.cpp
  src/client.cpp
  src/coordinator.cpp
  src/coordinator_authority.cpp
  src/coordinator_decision.cpp
  src/coordinator_query.cpp
  src/coordinator_server.cpp
  src/domain.cpp
  src/error.cpp
  src/hash.cpp
  src/inspect.cpp
  src/limits.cpp
  src/model.cpp
  src/persistence.cpp
  src/protocol.cpp
  src/state.cpp
  src/transport.cpp
  src/worker.cpp
)

add_library(experiment_fabric::experiment_fabric ALIAS experiment_fabric)
ef_apply_project_options(experiment_fabric)

target_include_directories(experiment_fabric
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_features(experiment_fabric PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(experiment_fabric PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(experiment_fabric PRIVATE ws2_32)
endif()

set_target_properties(experiment_fabric PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------

# Reference experiment catalogue shared by the tools, examples and tests.
# It is not installed: it is a reference artefact, not part of the runtime.
add_library(ef_scenarios STATIC apps/scenarios.cpp)
ef_apply_project_options(ef_scenarios)
target_link_libraries(ef_scenarios PUBLIC experiment_fabric)
target_include_directories(ef_scenarios PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/apps")

if(EXPERIMENT_FABRIC_BUILD_TOOLS)
  add_executable(ef_coordinator apps/ef_coordinator.cpp)
  target_link_libraries(ef_coordinator PRIVATE experiment_fabric ef_scenarios)
  ef_apply_project_options(ef_coordinator)

  add_executable(ef_worker apps/ef_worker.cpp)
  target_link_libraries(ef_worker PRIVATE experiment_fabric ef_scenarios)
  ef_apply_project_options(ef_worker)

  add_executable(ef_ctl apps/ef_ctl.cpp)
  target_link_libraries(ef_ctl PRIVATE experiment_fabric)
  ef_apply_project_options(ef_ctl)
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------

install(TARGETS experiment_fabric
  EXPORT ExperimentFabricTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(EXPERIMENT_FABRIC_BUILD_TOOLS)
  install(TARGETS ef_coordinator ef_worker ef_ctl
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ExperimentFabricTargets
  FILE ExperimentFabricTargets.cmake
  NAMESPACE experiment_fabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ExperimentFabric
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExperimentFabricConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/ExperimentFabricConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ExperimentFabric
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ExperimentFabricConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/ExperimentFabricConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/ExperimentFabricConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ExperimentFabric
)

# ---------------------------------------------------------------------------
# Tests, examples, benchmarks, CUDA
# ---------------------------------------------------------------------------

if(EXPERIMENT_FABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(EXPERIMENT_FABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Benchmarks of completed operations
# ---------------------------------------------------------------------------

if(EXPERIMENT_FABRIC_BUILD_BENCHMARKS)
  add_executable(ef_bench benchmarks/ef_bench.cpp)
  target_link_libraries(ef_bench PRIVATE experiment_fabric ef_scenarios)
  ef_apply_project_options(ef_bench)
endif()

if(EXPERIMENT_FABRIC_ENABLE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    add_subdirectory(cuda)
  else()
    message(STATUS "Experiment Fabric: no CUDA toolkit found; the CUDA reference trial is not built (UNSUPPORTED)")
  endif()
endif()
