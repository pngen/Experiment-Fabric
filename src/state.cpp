// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/state.hpp"

#include <algorithm>
#include <set>
#include <string>

#include "experiment_fabric/canonical.hpp"
#include "experiment_fabric/version.hpp"

namespace experiment_fabric {
namespace {

Status corrupt(std::string reason) {
  return make_error(ErrorCode::CORRUPT_PERSISTENCE, ErrorStage::PERSISTENCE, std::move(reason));
}

Status invalid(std::string reason) {
  return make_error(ErrorCode::INVALID_ARGUMENT, ErrorStage::VALIDATION, std::move(reason));
}

Status limits_error(std::string reason) {
  return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS, std::move(reason));
}

/// Highest generation value the runtime will accept from persisted state.
constexpr std::uint64_t kMaxGeneration = 1ull << 32;

/// Walks branch parentage and rejects cycles and impossible parentage.
Status validate_branch_lineage(const ExperimentDefinition& definition, const char* context) {
  std::set<BranchId> known;
  for (const BranchDefinition& branch : definition.branches) {
    if (!branch.id.valid()) {
      return corrupt(std::string(context) + ": null branch identity");
    }
    if (!known.insert(branch.id).second) {
      return corrupt(std::string(context) + ": duplicate branch identity");
    }
  }
  std::set<BranchId> baseline_seen;
  for (const BranchDefinition& branch : definition.branches) {
    if (branch.role == BranchRole::BASELINE) {
      baseline_seen.insert(branch.id);
    }
    if (branch.parent.valid() && known.find(branch.parent) == known.end()) {
      std::string reason = context;
      reason.append(": branch parent does not exist");
      return corrupt(std::move(reason));
    }
    if (branch.parent == branch.id) {
      return corrupt(std::string(context) + ": branch is its own parent");
    }
  }
  if (definition.policy.require_baseline && baseline_seen.empty()) {
    return corrupt(std::string(context) + ": policy requires a baseline branch but none is designated");
  }
  // Cycle detection: follow each chain with a bounded depth.
  for (const BranchDefinition& branch : definition.branches) {
    std::set<BranchId> seen;
    BranchId cursor = branch.id;
    std::uint32_t depth = 0;
    while (cursor.valid()) {
      if (!seen.insert(cursor).second) {
        return corrupt(std::string(context) + ": branch lineage contains a cycle");
      }
      if (++depth > 4096) {
        return corrupt(std::string(context) + ": branch lineage exceeds depth bound");
      }
      BranchId next;
      const auto iterator = std::find_if(definition.branches.begin(), definition.branches.end(),
                                         [cursor](const BranchDefinition& candidate) {
                                           return candidate.id == cursor;
                                         });
      if (iterator == definition.branches.end()) {
        break;
      }
      next = iterator->parent;
      cursor = next;
    }
  }
  return Status::success();
}

Status validate_metrics(const ExperimentDefinition& definition, const Limits& limits, const char* context) {
  if (definition.metrics.size() > limits.max_metrics_per_experiment) {
    return limits_error(std::string(context) + ": too many metric definitions");
  }
  std::set<MetricId> ids;
  std::set<std::string> names;
  for (const MetricDefinition& metric : definition.metrics) {
    if (!metric.id.valid()) {
      return corrupt(std::string(context) + ": null metric identity");
    }
    if (!ids.insert(metric.id).second) {
      return corrupt(std::string(context) + ": duplicate metric identity");
    }
    if (metric.name.empty() || metric.name.size() > limits.max_name_length) {
      return invalid(std::string(context) + ": metric name is empty or too long");
    }
    if (!names.insert(metric.name).second) {
      return invalid(std::string(context) + ": duplicate metric name");
    }
    if (metric.kind == MetricKind::CATEGORICAL && metric.aggregation != AggregationRule::MAJORITY &&
        metric.aggregation != AggregationRule::NONE) {
      return invalid(std::string(context) + ": categorical metric requires MAJORITY or NONE aggregation");
    }
    if ((metric.kind == MetricKind::REAL || metric.kind == MetricKind::INTEGER) &&
        metric.aggregation == AggregationRule::MAJORITY) {
      return invalid(std::string(context) + ": numeric metric cannot use MAJORITY aggregation");
    }
    if (metric.lower_bound.has_value() && metric.upper_bound.has_value() &&
        *metric.lower_bound > *metric.upper_bound) {
      return invalid(std::string(context) + ": metric validity bounds are inverted");
    }
    if (metric.target_low.has_value() && metric.target_high.has_value() &&
        *metric.target_low > *metric.target_high) {
      return invalid(std::string(context) + ": metric target range is inverted");
    }
  }
  if (definition.policy.rules.size() > limits.max_comparison_rules) {
    return limits_error(std::string(context) + ": too many comparison rules");
  }
  for (const ComparisonRule& rule : definition.policy.rules) {
    if (ids.find(rule.metric) == ids.end()) {
      return corrupt(std::string(context) + ": comparison rule references an unknown metric");
    }
    if (rule.op == ComparisonOp::TARGET_RANGE && rule.target_low > rule.target_high) {
      return invalid(std::string(context) + ": comparison target range is inverted");
    }
    if (rule.op == ComparisonOp::MAJORITY_WINS && (rule.threshold < 0.0 || rule.threshold > 1.0)) {
      return invalid(std::string(context) + ": majority-wins threshold must be a fraction in [0,1]");
    }
  }
  if (definition.policy.use_weighted_score) {
    for (const MetricDefinition& metric : definition.metrics) {
      if (metric.required && metric.direction == MetricDirection::INFORMATIONAL) {
        return invalid(std::string(context) +
                       ": weighted scoring requires a directional required metric");
      }
    }
  }
  if (definition.policy.majority_wins_min_basis_points > 10000) {
    return invalid(std::string(context) + ": majority-wins basis points out of range");
  }
  return Status::success();
}

Status validate_record(const ExperimentRecord& record, const Limits& limits, ExperimentId id) {
  const std::string context = "experiment " + id.to_string();
  const ExperimentDefinition& definition = record.definition;
  if (definition.id != id) {
    return corrupt(context + ": definition identity does not match its record key");
  }
  if (!definition.generation.valid() || definition.generation.value() > kMaxGeneration) {
    return corrupt(context + ": experiment generation is out of range");
  }
  if (!record.hypothesis.id.valid() || !record.hypothesis.revision.valid()) {
    return corrupt(context + ": hypothesis identity or revision is null");
  }
  if (record.hypothesis.id != definition.hypothesis) {
    return corrupt(context + ": definition hypothesis does not match the current hypothesis");
  }
  if (record.hypothesis.revision != definition.hypothesis_revision) {
    return corrupt(context + ": definition hypothesis revision does not match the current hypothesis");
  }
  if (definition.branches.size() > limits.max_branches_per_experiment) {
    return limits_error(context + ": too many branch definitions");
  }
  if (definition.branches.empty()) {
    return invalid(context + ": experiment has no branches");
  }
  {
    const Status status = validate_branch_lineage(definition, context.c_str());
    if (!status.ok()) {
      return status;
    }
  }
  {
    const Status status = validate_metrics(definition, limits, context.c_str());
    if (!status.ok()) {
      return status;
    }
  }
  if (canonical::has_duplicate_parameter_key(definition.parameters)) {
    return invalid(context + ": duplicate experiment parameter key");
  }

  std::map<BranchId, const BranchDefinition*> branches;
  for (const BranchDefinition& branch : definition.branches) {
    branches.emplace(branch.id, &branch);
    if (canonical::has_duplicate_parameter_key(branch.parameters)) {
      return invalid(context + ": duplicate branch parameter key");
    }
    if (branch.planned_trials == 0 || branch.planned_trials > limits.max_trials_per_branch) {
      return limits_error(context + ": branch planned trial count is out of range");
    }
  }

  std::set<MetricId> metric_ids;
  for (const MetricDefinition& metric : definition.metrics) {
    metric_ids.insert(metric.id);
  }

  if (record.trials.size() > branches.size() * static_cast<std::size_t>(limits.max_trials_per_branch)) {
    return limits_error(context + ": too many trials");
  }
  std::uint64_t committed_per_trial = 0;
  for (const auto& entry : record.trials) {
    const TrialRecord& trial = entry.second;
    if (trial.id != entry.first) {
      return corrupt(context + ": trial key does not match its identity");
    }
    if (branches.find(trial.branch) == branches.end()) {
      return corrupt(context + ": trial references an unknown branch");
    }
    // Historical trials may carry either an older or a newer generation than
    // the current one: a rollback restores an earlier authoritative generation
    // while later history is preserved. Only absurd values are corrupt.
    if (trial.generation.value() == 0 || trial.generation.value() > kMaxGeneration) {
      return corrupt(context + ": trial references an out-of-range experiment generation");
    }
    if (trial.hypothesis != definition.hypothesis) {
      return corrupt(context + ": trial references an unknown hypothesis");
    }
    if (trial.attempt_count > limits.max_attempts_per_trial) {
      return limits_error(context + ": trial attempt count exceeds the bound");
    }
    if (trial.committed_attempts > 1) {
      return corrupt(context + ": trial records more than one authoritative completion");
    }
    committed_per_trial += trial.committed_attempts;
  }

  if (record.attempts.size() > record.trials.size() * static_cast<std::size_t>(limits.max_attempts_per_trial)) {
    return limits_error(context + ": too many attempts");
  }
  std::uint64_t committed_attempts = 0;
  for (const auto& entry : record.attempts) {
    const AttemptRecord& attempt = entry.second;
    if (attempt.id != entry.first) {
      return corrupt(context + ": attempt key does not match its identity");
    }
    if (record.trials.find(attempt.trial) == record.trials.end()) {
      return corrupt(context + ": attempt references an unknown trial");
    }
    if (!attempt.number.valid()) {
      return corrupt(context + ": attempt number must be non-zero");
    }
    if (attempt.number.value() > limits.max_attempts_per_trial) {
      return limits_error(context + ": attempt number exceeds the configured bound");
    }
    if (attempt.committed) {
      ++committed_attempts;
    }
  }
  if (committed_attempts != committed_per_trial) {
    return corrupt(context + ": committed attempt count disagrees with trial completion count");
  }

  for (const auto& trial_entry : record.trials) {
    const TrialRecord& trial = trial_entry.second;
    if (trial.authoritative_attempt.valid() &&
        record.attempts.find(trial.authoritative_attempt) == record.attempts.end()) {
      return corrupt(context + ": trial authority references an unknown attempt");
    }
  }

  if (record.observations.size() > limits.max_observations_per_experiment) {
    return limits_error(context + ": too many observations");
  }
  if (record.artifacts.size() > limits.max_artifacts_per_experiment) {
    return limits_error(context + ": too many artifacts");
  }
  if (record.decisions.size() > limits.max_decisions_per_experiment) {
    return limits_error(context + ": too many decisions");
  }
  if (record.rollback_points.size() > limits.max_decisions_per_experiment) {
    return limits_error(context + ": too many rollback points");
  }

  std::set<ObservationId> observation_ids;
  std::map<TrialId, std::map<MetricId, std::uint32_t>> valid_per_trial_metric;
  for (const Observation& observation : record.observations) {
    if (!observation.id.valid()) {
      return corrupt(context + ": null observation identity");
    }
    if (!observation_ids.insert(observation.id).second) {
      return corrupt(context + ": duplicate observation identity");
    }
    if (record.trials.find(observation.trial) == record.trials.end()) {
      return corrupt(context + ": observation references an unknown trial");
    }
    if (record.attempts.find(observation.attempt) == record.attempts.end()) {
      return corrupt(context + ": observation references an unknown attempt");
    }
    if (metric_ids.find(observation.metric) == metric_ids.end()) {
      return corrupt(context + ": observation references an unknown metric");
    }
    if (!observation.producer.valid()) {
      return corrupt(context + ": observation has a null producer identity");
    }
    if (observation.generation.value() == 0 || observation.generation.value() > kMaxGeneration) {
      return corrupt(context + ": observation references an out-of-range experiment generation");
    }
    if (observation.validity == ObservationValidity::VALID) {
      ++valid_per_trial_metric[observation.trial][observation.metric];
    }
  }
  for (const auto& trial_entry : valid_per_trial_metric) {
    for (const auto& metric_entry : trial_entry.second) {
      if (metric_entry.second > 1) {
        return corrupt(context + ": duplicate valid observation for one metric on one trial");
      }
    }
  }

  std::set<ArtifactId> artifact_ids;
  for (const ArtifactRecord& artifact : record.artifacts) {
    if (!artifact.id.valid()) {
      return corrupt(context + ": null artifact identity");
    }
    if (!artifact_ids.insert(artifact.id).second) {
      return corrupt(context + ": duplicate artifact identity");
    }
    if (record.trials.find(artifact.trial) == record.trials.end()) {
      return corrupt(context + ": artifact references an unknown trial");
    }
    if (record.attempts.find(artifact.attempt) == record.attempts.end()) {
      return corrupt(context + ": artifact references an unknown attempt");
    }
    if (artifact.locator.empty() || artifact.locator.size() > limits.max_locator_length) {
      return invalid(context + ": artifact locator is empty or too long");
    }
  }

  for (const auto& trial_entry : record.trials) {
    for (const ArtifactId& artifact : trial_entry.second.artifacts) {
      if (artifact_ids.find(artifact) == artifact_ids.end()) {
        return corrupt(context + ": trial references an unknown artifact");
      }
    }
  }

  std::set<DecisionId> decision_ids;
  for (const Decision& decision : record.decisions) {
    if (!decision.id.valid()) {
      return corrupt(context + ": null decision identity");
    }
    if (!decision_ids.insert(decision.id).second) {
      return corrupt(context + ": duplicate decision identity");
    }
    if (decision.generation.value() == 0 || decision.generation.value() > kMaxGeneration) {
      return corrupt(context + ": decision references an out-of-range experiment generation");
    }
    if (branches.find(decision.candidate) == branches.end()) {
      return corrupt(context + ": decision references an unknown candidate branch");
    }
    if (decision.baseline_present && branches.find(decision.baseline) == branches.end()) {
      return corrupt(context + ": decision references an unknown baseline branch");
    }
  }

  std::set<RollbackPointId> rollback_ids;
  std::uint64_t previous_sequence = 0;
  for (const RollbackPoint& point : record.rollback_points) {
    if (!point.id.valid()) {
      return corrupt(context + ": null rollback point identity");
    }
    if (!rollback_ids.insert(point.id).second) {
      return corrupt(context + ": duplicate rollback point identity");
    }
    if (point.generation.value() == 0 || point.generation.value() > kMaxGeneration) {
      return corrupt(context + ": rollback point references an out-of-range experiment generation");
    }
    // Rollback points are ordered by the sequence at which they were recorded,
    // not by generation: a rollback restores an earlier generation and is
    // therefore legitimately recorded after points naming higher generations.
    if (point.sequence <= previous_sequence) {
      return corrupt(context + ": rollback points are not ordered by sequence");
    }
    if (point.decision_present && decision_ids.find(point.decision) == decision_ids.end()) {
      return corrupt(context + ": rollback point references an unknown decision");
    }
    for (const BranchAuthoritySnapshot& snapshot : point.authority) {
      if (!snapshot.branch.valid() || !snapshot.generation.valid()) {
        return corrupt(context + ": rollback point carries a null branch authority snapshot");
      }
      if (branches.find(snapshot.branch) == branches.end()) {
        return corrupt(context + ": rollback point references an unknown branch");
      }
    }
    previous_sequence = point.sequence;
  }
  return Status::success();
}

}  // namespace

Status validate_experiment_record(const ExperimentRecord& record, const Limits& limits, ExperimentId id) {
  return validate_record(record, limits, id);
}

std::uint64_t CoordinatorState::total_record_count() const noexcept {
  std::uint64_t total = workers.size();
  for (const auto& entry : experiments) {
    const ExperimentRecord& record = entry.second;
    total += 1;
    total += record.trials.size();
    total += record.attempts.size();
    total += record.observations.size();
    total += record.artifacts.size();
    total += record.decisions.size();
    total += record.rollback_points.size();
    total += record.rejected_evidence.size();
    total += record.hypothesis_history.size();
    total += record.lifecycle_log.size();
  }
  return total;
}

Status CoordinatorState::validate(const Limits& limits) const {
  if (format_version != kPersistenceFormatVersion) {
    return corrupt("state format version mismatch");
  }
  if (experiments.size() > limits.max_experiments) {
    return limits_error("state holds more experiments than the configured bound");
  }
  if (total_record_count() > limits.max_persisted_records) {
    return limits_error("state holds more records than the configured bound");
  }
  std::set<std::pair<std::uint64_t, std::uint64_t>> incarnations;
  for (const WorkerIncarnation& incarnation : workers) {
    if (!incarnation.worker.valid() || !incarnation.boot.valid() || !incarnation.epoch.valid()) {
      return corrupt("worker incarnation contains a null identity");
    }
    if (!incarnations.emplace(incarnation.worker.value(), incarnation.boot.value()).second) {
      return corrupt("duplicate worker boot incarnation");
    }
  }
  for (const auto& entry : experiments) {
    if (!entry.first.valid()) {
      return corrupt("null experiment record key");
    }
    const Status status = validate_record(entry.second, limits, entry.first);
    if (!status.ok()) {
      return status;
    }
  }

  // Watermarks must dominate every identity actually present.
  std::uint64_t max_experiment = 0;
  std::uint64_t max_hypothesis = 0;
  std::uint64_t max_branch = 0;
  std::uint64_t max_trial = 0;
  std::uint64_t max_attempt = 0;
  std::uint64_t max_observation = 0;
  std::uint64_t max_artifact = 0;
  std::uint64_t max_decision = 0;
  std::uint64_t max_rollback = 0;
  for (const auto& entry : experiments) {
    const ExperimentRecord& record = entry.second;
    max_experiment = std::max(max_experiment, entry.first.value());
    max_hypothesis = std::max({max_hypothesis, record.hypothesis.id.value(), record.definition.hypothesis.value()});
    for (const Hypothesis& hypothesis : record.hypothesis_history) {
      max_hypothesis = std::max(max_hypothesis, hypothesis.id.value());
    }
    for (const BranchDefinition& branch : record.definition.branches) {
      max_branch = std::max(max_branch, branch.id.value());
    }
    for (const auto& trial : record.trials) {
      max_trial = std::max(max_trial, trial.first.value());
    }
    for (const auto& attempt : record.attempts) {
      max_attempt = std::max(max_attempt, attempt.first.value());
    }
    for (const Observation& observation : record.observations) {
      max_observation = std::max(max_observation, observation.id.value());
    }
    for (const ArtifactRecord& artifact : record.artifacts) {
      max_artifact = std::max(max_artifact, artifact.id.value());
    }
    for (const Decision& decision : record.decisions) {
      max_decision = std::max(max_decision, decision.id.value());
    }
    for (const RollbackPoint& point : record.rollback_points) {
      max_rollback = std::max(max_rollback, point.id.value());
    }
  }
  if (id_watermark_experiment < max_experiment || id_watermark_hypothesis < max_hypothesis ||
      id_watermark_branch < max_branch || id_watermark_trial < max_trial ||
      id_watermark_attempt < max_attempt || id_watermark_observation < max_observation ||
      id_watermark_artifact < max_artifact || id_watermark_decision < max_decision ||
      id_watermark_rollback < max_rollback) {
    return corrupt("identity watermark does not dominate persisted identities");
  }
  return Status::success();
}

Sha256::Digest CoordinatorState::experiment_digest(ExperimentId id) const {
  Sha256 hasher;
  hasher.update_length_prefixed("experiment-state-v1");
  const auto iterator = experiments.find(id);
  if (iterator == experiments.end()) {
    hasher.update_length_prefixed("absent");
    return hasher.finalize();
  }
  const ExperimentRecord& record = iterator->second;
  const Sha256::Digest definition = canonical::definition_digest(record.definition);
  hasher.update(definition);
  const Sha256::Digest hypothesis = canonical::hypothesis_digest(record.hypothesis);
  hasher.update(hypothesis);
  hasher.update_u64(static_cast<std::uint64_t>(record.state));
  for (const auto& entry : record.trials) {
    canonical::write(hasher, "trial", entry.second);
  }
  for (const auto& entry : record.attempts) {
    hasher.update_u64(entry.first.value());
    hasher.update_u8(static_cast<std::uint8_t>(entry.second.state));
    hasher.update_bool(entry.second.committed);
  }
  for (const Observation& observation : record.observations) {
    canonical::write(hasher, "observation", observation);
  }
  for (const ArtifactRecord& artifact : record.artifacts) {
    hasher.update_u64(artifact.id.value());
    hasher.update_bool(artifact.current);
    hasher.update(artifact.content_digest);
  }
  for (const Decision& decision : record.decisions) {
    hasher.update_u64(decision.id.value());
    hasher.update_u8(static_cast<std::uint8_t>(decision.outcome));
    hasher.update(decision.canonical_state_digest);
  }
  return hasher.finalize();
}

}  // namespace experiment_fabric
