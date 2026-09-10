// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_LIMITS_HPP
#define EXPERIMENT_FABRIC_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace experiment_fabric {

/// ile
/// Hard bounds enforced by the runtime.
///
/// Every bound is checked *before* allocation or iteration. Remote size fields
/// and persisted counts are treated as untrusted: a declared length is never
/// used directly as an allocation size.

struct Limits {
  // ---- Governance shape -------------------------------------------------
  std::uint32_t max_experiments = 4096;
  std::uint32_t max_branches_per_experiment = 256;
  std::uint32_t max_metrics_per_experiment = 128;
  std::uint32_t max_trials_per_branch = 4096;
  std::uint32_t max_attempts_per_trial = 8;
  std::uint32_t max_observations_per_trial = 1024;
  std::uint32_t max_artifacts_per_attempt = 256;
  std::uint32_t max_observations_per_experiment = 1u << 20;
  std::uint32_t max_artifacts_per_experiment = 1u << 16;
  std::uint32_t max_decisions_per_experiment = 1024;
  std::uint32_t max_parameters_per_branch = 256;
  std::uint32_t max_comparison_rules = 256;
  std::uint32_t max_lineage_depth = 1024;

  // ---- Text -------------------------------------------------------------
  std::uint32_t max_name_length = 128;
  std::uint32_t max_unit_length = 32;
  std::uint32_t max_statement_length = 4096;
  std::uint32_t max_payload_text_length = 4096;
  std::uint32_t max_locator_length = 1024;
  std::uint32_t max_provenance_length = 1024;

  // ---- Explanation ------------------------------------------------------
  std::uint32_t max_explanation_lines = 4096;
  std::uint32_t max_explanation_bytes = 1u << 20;

  // ---- Persistence ------------------------------------------------------
  std::uint64_t max_state_file_bytes = 1ull << 30;  // 1 GiB
  std::uint32_t max_persisted_records = 1u << 22;
  std::uint32_t max_persisted_string_bytes = 1u << 20;

  // ---- Transport --------------------------------------------------------
  std::uint32_t max_frame_payload_bytes = 1u << 20;  // 1 MiB
  std::uint32_t max_connections = 128;
  std::uint32_t max_inflight_frames_per_connection = 64;
  std::uint32_t max_batch_items = 4096;

  // ---- Workers ----------------------------------------------------------
  std::uint32_t max_registered_workers = 256;
  std::uint32_t max_concurrent_assignments_per_worker = 64;
  std::uint32_t max_worker_threads = 64;

  /// Returns the single process-wide default limits instance.
  [[nodiscard]] static const Limits& defaults() noexcept;

  /// Validates that a limits instance is internally coherent.
  [[nodiscard]] static bool is_coherent(const Limits& limits) noexcept;
};

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_LIMITS_HPP
