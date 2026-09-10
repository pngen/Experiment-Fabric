# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

set(EF_EXAMPLES
  example_latency_comparison
  example_throughput_memory_tradeoff
  example_branch_fork
  example_retry_after_failure
  example_cancelled_late_evidence
  example_rollback
  example_worker_loss
  example_coordinator_restart
  example_insufficient_evidence
  example_reproducibility
  example_lineage
)

foreach(example IN LISTS EF_EXAMPLES)
  add_executable(${example} ${example}.cpp)
  target_link_libraries(${example} PRIVATE experiment_fabric ef_scenarios)
  target_include_directories(${example} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()
