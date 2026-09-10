// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/canonical.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <set>

namespace experiment_fabric {
namespace canonical {
namespace {

void write_tag(Sha256& hasher, std::string_view tag) {
  hasher.update_u32(static_cast<std::uint32_t>(tag.size()));
  hasher.update(tag);
}

void write_text(Sha256& hasher, std::string_view text) { hasher.update_length_prefixed(text); }

void write_real(Sha256& hasher, double value) {
  const std::string encoded = encode_real(value);
  write_text(hasher, encoded);
}

void write_metric_value(Sha256& hasher, const MetricValue& value) {
  hasher.update_u8(static_cast<std::uint8_t>(value.kind));
  switch (value.kind) {
    case MetricKind::REAL:
      write_real(hasher, value.real);
      break;
    case MetricKind::INTEGER:
      hasher.update_u64(static_cast<std::uint64_t>(value.integer));
      break;
    case MetricKind::BOOLEAN:
      hasher.update_bool(value.boolean);
      break;
    case MetricKind::CATEGORICAL:
      write_text(hasher, value.category);
      break;
  }
}

}  // namespace

std::string encode_real(double value) {
  if (std::isnan(value)) {
    return "nan";
  }
  if (std::isinf(value)) {
    return value > 0 ? "inf" : "-inf";
  }
  if (value == 0.0) {
    // Normalises negative zero so that -0.0 and 0.0 digest identically.
    return "0";
  }
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
  return std::string(buffer, result.ptr);
}

void write(Sha256& hasher, std::string_view tag, const ParameterAssignment& value) {
  write_tag(hasher, tag);
  write_text(hasher, value.key);
  hasher.update_u8(static_cast<std::uint8_t>(value.kind));
  write_text(hasher, value.value);
}

void write(Sha256& hasher, std::string_view tag, const MetricDefinition& value) {
  write_tag(hasher, tag);
  hasher.update_u64(value.id.value());
  write_text(hasher, value.name);
  write_text(hasher, value.unit);
  hasher.update_u8(static_cast<std::uint8_t>(value.kind));
  hasher.update_u8(static_cast<std::uint8_t>(value.direction));
  hasher.update_u8(static_cast<std::uint8_t>(value.aggregation));
  hasher.update_bool(value.required);
  hasher.update_bool(value.accepts_non_finite);
  hasher.update_bool(value.lower_bound.has_value());
  if (value.lower_bound.has_value()) {
    write_real(hasher, *value.lower_bound);
  }
  hasher.update_bool(value.upper_bound.has_value());
  if (value.upper_bound.has_value()) {
    write_real(hasher, *value.upper_bound);
  }
  hasher.update_bool(value.target_low.has_value());
  if (value.target_low.has_value()) {
    write_real(hasher, *value.target_low);
  }
  hasher.update_bool(value.target_high.has_value());
  if (value.target_high.has_value()) {
    write_real(hasher, *value.target_high);
  }
  hasher.update_u32(value.weight);
  hasher.update_u32(value.priority);
  std::vector<std::string> categories = value.categories;
  std::sort(categories.begin(), categories.end());
  hasher.update_u32(static_cast<std::uint32_t>(categories.size()));
  for (const std::string& category : categories) {
    write_text(hasher, category);
  }
}

void write(Sha256& hasher, std::string_view tag, const ComparisonRule& value) {
  write_tag(hasher, tag);
  hasher.update_u64(value.metric.value());
  hasher.update_u8(static_cast<std::uint8_t>(value.op));
  write_real(hasher, value.threshold);
  write_real(hasher, value.target_low);
  write_real(hasher, value.target_high);
  hasher.update_u32(value.priority);
}

void write(Sha256& hasher, std::string_view tag, const ComparisonPolicy& value) {
  write_tag(hasher, tag);
  hasher.update_u64(value.id.value());
  hasher.update_u64(value.generation.value());
  hasher.update_bool(value.require_baseline);
  hasher.update_u32(value.min_completed_trials_per_branch);
  hasher.update_u32(value.min_valid_observations_per_required_metric);
  hasher.update_bool(value.reject_on_any_invalid_observation);
  hasher.update_bool(value.require_environment_match);
  hasher.update_u32(value.majority_wins_min_basis_points);
  hasher.update_bool(value.use_weighted_score);
  write_real(hasher, value.accept_score_threshold);
  std::vector<ComparisonRule> rules = value.rules;
  std::sort(rules.begin(), rules.end(), [](const ComparisonRule& lhs, const ComparisonRule& rhs) {
    if (lhs.priority != rhs.priority) {
      return lhs.priority < rhs.priority;
    }
    return lhs.metric < rhs.metric;
  });
  hasher.update_u32(static_cast<std::uint32_t>(rules.size()));
  for (const ComparisonRule& rule : rules) {
    write(hasher, "rule", rule);
  }
}

void write(Sha256& hasher, std::string_view tag, const BranchDefinition& value) {
  write_tag(hasher, tag);
  hasher.update_u64(value.id.value());
  write_text(hasher, value.name);
  hasher.update_u8(static_cast<std::uint8_t>(value.role));
  hasher.update_u64(value.parent.value());
  hasher.update_u64(value.generation.value());
  hasher.update_u8(static_cast<std::uint8_t>(value.state));
  std::vector<ParameterAssignment> parameters = normalize_parameters(value.parameters);
  hasher.update_u32(static_cast<std::uint32_t>(parameters.size()));
  for (const ParameterAssignment& parameter : parameters) {
    write(hasher, "parameter", parameter);
  }
  hasher.update_bool(value.input_set_present);
  if (value.input_set_present) {
    hasher.update_u64(value.input_set.value());
  }
  hasher.update_bool(value.environment_present);
  if (value.environment_present) {
    hasher.update_u64(value.environment.value());
  }
  write_text(hasher, value.environment_fingerprint);
  hasher.update_bool(value.seed.has_value());
  if (value.seed.has_value()) {
    hasher.update_u64(*value.seed);
  }
  hasher.update_u32(value.planned_trials);
  hasher.update_bool(value.forked);
}

void write(Sha256& hasher, std::string_view tag, const BranchEvidence& value) {
  write_tag(hasher, tag);
  hasher.update_u64(value.branch.value());
  hasher.update_u64(value.generation.value());
  hasher.update_u8(static_cast<std::uint8_t>(value.role));
  hasher.update_u8(static_cast<std::uint8_t>(value.state));
  hasher.update_u32(value.planned_trials);
  hasher.update_u32(value.completed_trials);
  hasher.update_u32(value.failed_trials);
  hasher.update_u32(value.cancelled_trials);
  hasher.update_u32(value.invalid_trials);
  hasher.update_u32(value.superseded_trials);
  hasher.update_u32(value.open_trials);
  hasher.update_u32(static_cast<std::uint32_t>(value.aggregates.size()));
  for (const MetricAggregate& aggregate : value.aggregates) {
    hasher.update_u64(aggregate.metric.value());
    hasher.update_u8(static_cast<std::uint8_t>(aggregate.rule));
    hasher.update_bool(aggregate.present);
    write_real(hasher, aggregate.value);
    write_text(hasher, aggregate.category);
    hasher.update_u32(aggregate.valid_count);
    hasher.update_u32(aggregate.invalid_count);
    hasher.update_u32(aggregate.missing_count);
    hasher.update_u32(aggregate.unsupported_count);
    hasher.update_u32(static_cast<std::uint32_t>(aggregate.raw_values.size()));
    for (const double raw : aggregate.raw_values) {
      write_real(hasher, raw);
    }
  }
}

void write(Sha256& hasher, std::string_view tag, const Observation& value) {
  write_tag(hasher, tag);
  hasher.update_u64(value.id.value());
  hasher.update_u64(value.experiment.value());
  hasher.update_u64(value.generation.value());
  hasher.update_u64(value.branch.value());
  hasher.update_u64(value.branch_generation.value());
  hasher.update_u64(value.trial.value());
  hasher.update_u64(value.attempt.value());
  hasher.update_u64(value.metric.value());
  hasher.update_u64(value.producer.value());
  hasher.update_u8(static_cast<std::uint8_t>(value.validity));
  write_metric_value(hasher, value.value);
  write_text(hasher, value.detail);
}

void write(Sha256& hasher, std::string_view tag, const TrialRecord& value) {
  write_tag(hasher, tag);
  hasher.update_u64(value.id.value());
  hasher.update_u64(value.branch.value());
  hasher.update_u64(value.branch_generation.value());
  hasher.update_u64(value.generation.value());
  hasher.update_u8(static_cast<std::uint8_t>(value.state));
  hasher.update_u64(value.authoritative_attempt.value());
  hasher.update_u32(value.committed_attempts);
  hasher.update_u8(static_cast<std::uint8_t>(value.failure_kind));
}

void write(Sha256& hasher, std::string_view tag, const Decision& value) {
  write_tag(hasher, tag);
  hasher.update_u64(value.id.value());
  hasher.update_u64(value.experiment.value());
  hasher.update_u64(value.generation.value());
  hasher.update_u64(value.candidate.value());
  hasher.update_u8(static_cast<std::uint8_t>(value.outcome));
  write_text(hasher, value.reason);
  for (const ConstraintResult& constraint : value.constraints) {
    write_text(hasher, constraint.name);
    hasher.update_bool(constraint.passed);
    write_text(hasher, constraint.detail);
  }
  for (const ComparisonFactor& factor : value.factors) {
    hasher.update_u64(factor.metric.value());
    hasher.update_u8(static_cast<std::uint8_t>(factor.op));
    hasher.update_u32(factor.priority);
    write_real(hasher, factor.threshold);
    hasher.update_bool(factor.has_values);
    write_real(hasher, factor.baseline_value);
    write_real(hasher, factor.candidate_value);
    write_real(hasher, factor.delta);
    hasher.update_bool(factor.satisfied);
    hasher.update_bool(factor.decisive);
  }
}

bool has_duplicate_parameter_key(const std::vector<ParameterAssignment>& parameters) {
  std::set<std::string> keys;
  for (const ParameterAssignment& parameter : parameters) {
    if (!keys.insert(parameter.key).second) {
      return true;
    }
  }
  return false;
}

std::vector<ParameterAssignment> normalize_parameters(const std::vector<ParameterAssignment>& parameters) {
  std::vector<ParameterAssignment> normalized = parameters;
  std::stable_sort(normalized.begin(), normalized.end(), parameter_less);
  return normalized;
}

Sha256::Digest definition_digest(const ExperimentDefinition& definition) {
  Sha256 hasher;
  write_tag(hasher, "experiment-definition");
  hasher.update_u32(1);  // encoding revision
  hasher.update_u64(definition.id.value());
  hasher.update_u64(definition.generation.value());
  hasher.update_u64(definition.hypothesis.value());
  hasher.update_u64(definition.hypothesis_revision.value());
  write_text(hasher, definition.name);

  std::vector<BranchDefinition> branches = definition.branches;
  std::sort(branches.begin(), branches.end(), [](const BranchDefinition& lhs, const BranchDefinition& rhs) {
    return lhs.id < rhs.id;
  });
  hasher.update_u32(static_cast<std::uint32_t>(branches.size()));
  for (const BranchDefinition& branch : branches) {
    write(hasher, "branch", branch);
  }

  std::vector<MetricDefinition> metrics = definition.metrics;
  std::sort(metrics.begin(), metrics.end(), [](const MetricDefinition& lhs, const MetricDefinition& rhs) {
    return lhs.id < rhs.id;
  });
  hasher.update_u32(static_cast<std::uint32_t>(metrics.size()));
  for (const MetricDefinition& metric : metrics) {
    write(hasher, "metric", metric);
  }

  write(hasher, "policy", definition.policy);
  hasher.update_u8(static_cast<std::uint8_t>(definition.seed_policy));
  hasher.update_bool(definition.experiment_seed.has_value());
  if (definition.experiment_seed.has_value()) {
    hasher.update_u64(*definition.experiment_seed);
  }
  hasher.update_u8(static_cast<std::uint8_t>(definition.retry_policy));
  hasher.update_u32(definition.max_attempts_per_trial);
  hasher.update_u8(static_cast<std::uint8_t>(definition.partial_failure_policy));
  hasher.update_u8(static_cast<std::uint8_t>(definition.late_evidence_policy));
  const std::vector<ParameterAssignment> parameters = normalize_parameters(definition.parameters);
  hasher.update_u32(static_cast<std::uint32_t>(parameters.size()));
  for (const ParameterAssignment& parameter : parameters) {
    write(hasher, "parameter", parameter);
  }
  write_text(hasher, definition.provenance);
  return hasher.finalize();
}

Sha256::Digest branch_digest(const BranchDefinition& branch) {
  Sha256 hasher;
  write(hasher, "branch-definition", branch);
  return hasher.finalize();
}

Sha256::Digest policy_digest(const ComparisonPolicy& policy) {
  Sha256 hasher;
  write(hasher, "comparison-policy", policy);
  return hasher.finalize();
}

Sha256::Digest hypothesis_digest(const Hypothesis& hypothesis) {
  Sha256 hasher;
  write_tag(hasher, "hypothesis");
  hasher.update_u64(hypothesis.id.value());
  hasher.update_u64(hypothesis.revision.value());
  write_text(hasher, hypothesis.statement);
  hasher.update_bool(hypothesis.expected_direction.has_value());
  if (hypothesis.expected_direction.has_value()) {
    hasher.update_u8(static_cast<std::uint8_t>(*hypothesis.expected_direction));
  }
  hasher.update_u64(hypothesis.expected_metric.value());
  hasher.update_u64(hypothesis.parent.value());
  hasher.update_u64(hypothesis.parent_revision.value());
  write_text(hasher, hypothesis.provenance);
  return hasher.finalize();
}

Sha256::Digest evidence_digest(const std::vector<BranchEvidence>& evidence,
                               const ExperimentDefinition& definition) {
  Sha256 hasher;
  write_tag(hasher, "experiment-evidence");
  hasher.update_u64(definition.id.value());
  hasher.update_u64(definition.generation.value());
  hasher.update_u64(definition.hypothesis_revision.value());
  for (const BranchEvidence& branch_evidence : evidence) {
    write(hasher, "branch-evidence", branch_evidence);
  }
  return hasher.finalize();
}

}  // namespace canonical
}  // namespace experiment_fabric
