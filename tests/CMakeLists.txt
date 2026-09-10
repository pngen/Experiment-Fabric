# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

set(EF_TEST_SOURCES
  test_support.cpp
  test_identity.cpp
  test_hash.cpp
  test_domain.cpp
  test_experiment.cpp
  test_metrics.cpp
  test_comparison.cpp
  test_explanation.cpp
  test_authority.cpp
  test_cancellation.cpp
  test_rollback.cpp
  test_lineage_reproducibility.cpp
  test_persistence.cpp
  test_protocol.cpp
  test_property.cpp
  test_concurrency.cpp
  test_adversarial.cpp
  test_multiprocess.cpp
)

add_executable(ef_tests ${EF_TEST_SOURCES})
target_link_libraries(ef_tests PRIVATE experiment_fabric ef_scenarios)
ef_apply_project_options(ef_tests)
target_include_directories(ef_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Executable locations are passed as compile definitions so that the
# multi-process proof drives the real installed binaries rather than an
# in-process approximation.
target_compile_definitions(ef_tests PRIVATE
  EF_COORDINATOR_EXE="$<TARGET_FILE:ef_coordinator>"
  EF_WORKER_EXE="$<TARGET_FILE:ef_worker>"
  EF_CTL_EXE="$<TARGET_FILE:ef_ctl>"
)
add_dependencies(ef_tests ef_coordinator ef_worker ef_ctl)

# Tests run plainly: no timeout control of any kind is applied, because a
# hanging test is a defect to diagnose rather than something to abandon.
add_test(NAME ef_tests COMMAND ef_tests)

if(EXPERIMENT_FABRIC_ENABLE_ASAN)
  add_test(NAME ef_tests_asan_core COMMAND ef_tests --filter=authority)
  add_test(NAME ef_tests_asan_persistence COMMAND ef_tests --filter=persistence)
  add_test(NAME ef_tests_asan_protocol COMMAND ef_tests --filter=protocol)
  add_test(NAME ef_tests_asan_rollback COMMAND ef_tests --filter=rollback)
endif()
