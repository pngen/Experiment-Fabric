// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/limits.hpp"

namespace experiment_fabric {

const Limits& Limits::defaults() noexcept {
  static const Limits limits{};
  return limits;
}

bool Limits::is_coherent(const Limits& limits) noexcept {
  if (limits.max_experiments == 0 || limits.max_branches_per_experiment == 0 ||
      limits.max_metrics_per_experiment == 0 || limits.max_trials_per_branch == 0 ||
      limits.max_attempts_per_trial == 0 || limits.max_observations_per_trial == 0) {
    return false;
  }
  if (limits.max_frame_payload_bytes < 64) {
    return false;
  }
  if (limits.max_name_length == 0 || limits.max_name_length > (1u << 20)) {
    return false;
  }
  if (limits.max_explanation_lines == 0 || limits.max_explanation_bytes == 0) {
    return false;
  }
  if (limits.max_state_file_bytes == 0 || limits.max_persisted_records == 0) {
    return false;
  }
  if (limits.max_lineage_depth == 0 || limits.max_lineage_depth > (1u << 20)) {
    return false;
  }
  if (limits.max_comparison_rules == 0 || limits.max_comparison_rules > (1u << 16)) {
    return false;
  }
  return true;
}

}  // namespace experiment_fabric
