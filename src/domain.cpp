// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/domain.hpp"

#include <array>

namespace experiment_fabric {
namespace {

template <typename Enum, std::size_t N>
struct NameTable {
  std::array<std::pair<Enum, std::string_view>, N> entries;

  [[nodiscard]] std::string_view lookup(Enum value) const noexcept {
    for (const auto& entry : entries) {
      if (entry.first == value) {
        return entry.second;
      }
    }
    return "UNKNOWN";
  }

  [[nodiscard]] std::optional<Enum> reverse(std::string_view name) const noexcept {
    for (const auto& entry : entries) {
      if (entry.second == name) {
        return entry.first;
      }
    }
    return std::nullopt;
  }
};

constexpr NameTable<TrialState, 11> kTrialStates{{{
    {TrialState::CREATED, "CREATED"},
    {TrialState::READY, "READY"},
    {TrialState::ASSIGNED, "ASSIGNED"},
    {TrialState::RUNNING, "RUNNING"},
    {TrialState::OBSERVING, "OBSERVING"},
    {TrialState::COMPLETED, "COMPLETED"},
    {TrialState::FAILED, "FAILED"},
    {TrialState::CANCELLED, "CANCELLED"},
    {TrialState::INVALID, "INVALID"},
    {TrialState::SUPERSEDED, "SUPERSEDED"},
    {TrialState::REVALIDATION_REQUIRED, "REVALIDATION_REQUIRED"},
}}};

constexpr NameTable<ExperimentState, 5> kExperimentStates{{{
    {ExperimentState::OPEN, "OPEN"},
    {ExperimentState::FINALIZED, "FINALIZED"},
    {ExperimentState::CANCELLED, "CANCELLED"},
    {ExperimentState::SUPERSEDED, "SUPERSEDED"},
    {ExperimentState::RECOVERING, "RECOVERING"},
}}};

constexpr NameTable<BranchState, 4> kBranchStates{{{
    {BranchState::ACTIVE, "ACTIVE"},
    {BranchState::RETIRED, "RETIRED"},
    {BranchState::INVALID, "INVALID"},
    {BranchState::SUPERSEDED, "SUPERSEDED"},
}}};

constexpr NameTable<AttemptState, 8> kAttemptStates{{{
    {AttemptState::ISSUED, "ISSUED"},
    {AttemptState::RUNNING, "RUNNING"},
    {AttemptState::PUBLISHED, "PUBLISHED"},
    {AttemptState::COMPLETED, "COMPLETED"},
    {AttemptState::FAILED, "FAILED"},
    {AttemptState::CANCELLED, "CANCELLED"},
    {AttemptState::ABANDONED, "ABANDONED"},
    {AttemptState::REVALIDATION_REQUIRED, "REVALIDATION_REQUIRED"},
}}};

constexpr NameTable<BranchRole, 2> kBranchRoles{{{
    {BranchRole::BASELINE, "BASELINE"},
    {BranchRole::CANDIDATE, "CANDIDATE"},
}}};

constexpr NameTable<MetricKind, 4> kMetricKinds{{{
    {MetricKind::REAL, "REAL"},
    {MetricKind::INTEGER, "INTEGER"},
    {MetricKind::BOOLEAN, "BOOLEAN"},
    {MetricKind::CATEGORICAL, "CATEGORICAL"},
}}};

constexpr NameTable<MetricDirection, 4> kMetricDirections{{{
    {MetricDirection::MINIMIZE, "MINIMIZE"},
    {MetricDirection::MAXIMIZE, "MAXIMIZE"},
    {MetricDirection::TARGET, "TARGET"},
    {MetricDirection::INFORMATIONAL, "INFORMATIONAL"},
}}};

constexpr NameTable<AggregationRule, 7> kAggregationRules{{{
    {AggregationRule::NONE, "NONE"},
    {AggregationRule::MEAN, "MEAN"},
    {AggregationRule::MEDIAN, "MEDIAN"},
    {AggregationRule::MIN, "MIN"},
    {AggregationRule::MAX, "MAX"},
    {AggregationRule::SUM, "SUM"},
    {AggregationRule::MAJORITY, "MAJORITY"},
}}};

constexpr NameTable<ObservationValidity, 4> kObservationValidities{{{
    {ObservationValidity::VALID, "VALID"},
    {ObservationValidity::INVALID, "INVALID"},
    {ObservationValidity::MISSING, "MISSING"},
    {ObservationValidity::UNSUPPORTED, "UNSUPPORTED"},
}}};

constexpr NameTable<ComparisonOp, 7> kComparisonOps{{{
    {ComparisonOp::GREATER_THAN, "GREATER_THAN"},
    {ComparisonOp::LESS_THAN, "LESS_THAN"},
    {ComparisonOp::MIN_IMPROVEMENT, "MIN_IMPROVEMENT"},
    {ComparisonOp::MAX_REGRESSION_TOLERANCE, "MAX_REGRESSION_TOLERANCE"},
    {ComparisonOp::TARGET_RANGE, "TARGET_RANGE"},
    {ComparisonOp::MAJORITY_WINS, "MAJORITY_WINS"},
    {ComparisonOp::REPORT_ONLY, "REPORT_ONLY"},
}}};

constexpr NameTable<DecisionOutcome, 7> kDecisionOutcomes{{{
    {DecisionOutcome::ACCEPT, "ACCEPT"},
    {DecisionOutcome::REJECT, "REJECT"},
    {DecisionOutcome::INCONCLUSIVE, "INCONCLUSIVE"},
    {DecisionOutcome::INSUFFICIENT_EVIDENCE, "INSUFFICIENT_EVIDENCE"},
    {DecisionOutcome::INVALID, "INVALID"},
    {DecisionOutcome::CANCELLED, "CANCELLED"},
    {DecisionOutcome::SUPERSEDED, "SUPERSEDED"},
}}};

constexpr NameTable<FailureKind, 11> kFailureKinds{{{
    {FailureKind::NONE, "NONE"},
    {FailureKind::EXECUTION_FAILURE, "EXECUTION_FAILURE"},
    {FailureKind::INVALID_OBSERVATION, "INVALID_OBSERVATION"},
    {FailureKind::PRODUCER_LOSS, "PRODUCER_LOSS"},
    {FailureKind::TRANSPORT_FAILURE, "TRANSPORT_FAILURE"},
    {FailureKind::CANCELLATION, "CANCELLATION"},
    {FailureKind::POLICY_REJECTION, "POLICY_REJECTION"},
    {FailureKind::INSUFFICIENT_EVIDENCE, "INSUFFICIENT_EVIDENCE"},
    {FailureKind::PERSISTENCE_FAILURE, "PERSISTENCE_FAILURE"},
    {FailureKind::EXPERIMENT_INVALIDATION, "EXPERIMENT_INVALIDATION"},
    {FailureKind::LIMIT_EXCEEDED, "LIMIT_EXCEEDED"},
}}};

constexpr NameTable<ReproducibilityStatus, 4> kReproducibilityStatuses{{{
    {ReproducibilityStatus::REPRODUCIBLE, "REPRODUCIBLE"},
    {ReproducibilityStatus::PARTIALLY_REPRODUCIBLE, "PARTIALLY_REPRODUCIBLE"},
    {ReproducibilityStatus::NOT_REPRODUCIBLE, "NOT_REPRODUCIBLE"},
    {ReproducibilityStatus::UNKNOWN, "UNKNOWN"},
}}};

constexpr NameTable<SeedPolicy, 4> kSeedPolicies{{{
    {SeedPolicy::FIXED, "FIXED"},
    {SeedPolicy::DERIVED_PER_TRIAL, "DERIVED_PER_TRIAL"},
    {SeedPolicy::PRODUCER_SUPPLIED, "PRODUCER_SUPPLIED"},
    {SeedPolicy::NONE, "NONE"},
}}};

constexpr NameTable<RetryPolicy, 3> kRetryPolicies{{{
    {RetryPolicy::FRESH_ATTEMPT_SAME_TRIAL, "FRESH_ATTEMPT_SAME_TRIAL"},
    {RetryPolicy::NEW_TRIAL_IDENTITY, "NEW_TRIAL_IDENTITY"},
    {RetryPolicy::NO_RETRY, "NO_RETRY"},
}}};

constexpr NameTable<PartialFailurePolicy, 3> kPartialFailurePolicies{{{
    {PartialFailurePolicy::TOLERATE_IF_EVIDENCE_SUFFICIENT, "TOLERATE_IF_EVIDENCE_SUFFICIENT"},
    {PartialFailurePolicy::INVALIDATE_BRANCH, "INVALIDATE_BRANCH"},
    {PartialFailurePolicy::RETRY_ALLOWED, "RETRY_ALLOWED"},
}}};

constexpr NameTable<LateEvidencePolicy, 2> kLateEvidencePolicies{{{
    {LateEvidencePolicy::REJECT_AND_RECORD, "REJECT_AND_RECORD"},
    {LateEvidencePolicy::RETAIN_HISTORICAL, "RETAIN_HISTORICAL"},
}}};

constexpr NameTable<Provenance, 3> kProvenances{{{
    {Provenance::REAL, "REAL"},
    {Provenance::SYNTHETIC, "SYNTHETIC"},
    {Provenance::UNSUPPORTED, "UNSUPPORTED"},
}}};

constexpr NameTable<ParameterKind, 4> kParameterKinds{{{
    {ParameterKind::TEXT, "TEXT"},
    {ParameterKind::INTEGER, "INTEGER"},
    {ParameterKind::REAL, "REAL"},
    {ParameterKind::BOOLEAN, "BOOLEAN"},
}}};

}  // namespace

bool is_terminal(TrialState state) noexcept {
  switch (state) {
    case TrialState::COMPLETED:
    case TrialState::FAILED:
    case TrialState::CANCELLED:
    case TrialState::INVALID:
    case TrialState::SUPERSEDED:
      return true;
    case TrialState::CREATED:
    case TrialState::READY:
    case TrialState::ASSIGNED:
    case TrialState::RUNNING:
    case TrialState::OBSERVING:
    case TrialState::REVALIDATION_REQUIRED:
      return false;
  }
  return true;
}

bool is_trial_closed(TrialState state) noexcept { return is_terminal(state); }

bool is_experiment_mutable(ExperimentState state) noexcept { return state == ExperimentState::OPEN; }

bool is_attempt_terminal(AttemptState state) noexcept {
  switch (state) {
    case AttemptState::COMPLETED:
    case AttemptState::FAILED:
    case AttemptState::CANCELLED:
    case AttemptState::ABANDONED:
      return true;
    case AttemptState::ISSUED:
    case AttemptState::RUNNING:
    case AttemptState::PUBLISHED:
    case AttemptState::REVALIDATION_REQUIRED:
      return false;
  }
  return true;
}

bool is_branch_eligible(BranchState state) noexcept { return state == BranchState::ACTIVE; }

bool is_valid_transition(TrialState from, TrialState to) noexcept {
  if (from == to) {
    return false;
  }
  if (is_terminal(from)) {
    // Terminal states only move to SUPERSEDED-style audit markers, and only
    // from a terminal state that is itself not SUPERSEDED.
    return to == TrialState::SUPERSEDED && from != TrialState::SUPERSEDED;
  }
  switch (from) {
    case TrialState::CREATED:
      return to == TrialState::READY || to == TrialState::CANCELLED || to == TrialState::INVALID ||
             to == TrialState::SUPERSEDED || to == TrialState::REVALIDATION_REQUIRED;
    case TrialState::READY:
      return to == TrialState::ASSIGNED || to == TrialState::CANCELLED || to == TrialState::INVALID ||
             to == TrialState::SUPERSEDED || to == TrialState::REVALIDATION_REQUIRED;
    case TrialState::ASSIGNED:
      return to == TrialState::RUNNING || to == TrialState::OBSERVING || to == TrialState::READY ||
             to == TrialState::FAILED ||
             to == TrialState::CANCELLED || to == TrialState::INVALID || to == TrialState::SUPERSEDED ||
             to == TrialState::REVALIDATION_REQUIRED;
    case TrialState::RUNNING:
      return to == TrialState::OBSERVING || to == TrialState::FAILED || to == TrialState::CANCELLED ||
             to == TrialState::INVALID || to == TrialState::SUPERSEDED || to == TrialState::REVALIDATION_REQUIRED;
    case TrialState::OBSERVING:
      return to == TrialState::COMPLETED || to == TrialState::FAILED || to == TrialState::CANCELLED ||
             to == TrialState::INVALID || to == TrialState::SUPERSEDED || to == TrialState::REVALIDATION_REQUIRED;
    case TrialState::REVALIDATION_REQUIRED:
      // Recovered dynamic state may only be resolved conservatively.
      return to == TrialState::READY || to == TrialState::FAILED || to == TrialState::CANCELLED ||
             to == TrialState::INVALID || to == TrialState::SUPERSEDED;
    default:
      return false;
  }
}

bool is_valid_transition(AttemptState from, AttemptState to) noexcept {
  if (from == to) {
    return false;
  }
  if (is_attempt_terminal(from)) {
    return false;
  }
  switch (from) {
    case AttemptState::ISSUED:
      return to == AttemptState::RUNNING || to == AttemptState::PUBLISHED || to == AttemptState::COMPLETED ||
             to == AttemptState::FAILED || to == AttemptState::CANCELLED || to == AttemptState::ABANDONED ||
             to == AttemptState::REVALIDATION_REQUIRED;
    case AttemptState::RUNNING:
      return to == AttemptState::PUBLISHED || to == AttemptState::COMPLETED || to == AttemptState::FAILED ||
             to == AttemptState::CANCELLED || to == AttemptState::ABANDONED ||
             to == AttemptState::REVALIDATION_REQUIRED;
    case AttemptState::PUBLISHED:
      return to == AttemptState::COMPLETED || to == AttemptState::FAILED || to == AttemptState::CANCELLED ||
             to == AttemptState::ABANDONED || to == AttemptState::REVALIDATION_REQUIRED;
    case AttemptState::REVALIDATION_REQUIRED:
      return to == AttemptState::FAILED || to == AttemptState::CANCELLED || to == AttemptState::ABANDONED;
    default:
      return false;
  }
}

bool is_valid_transition(ExperimentState from, ExperimentState to) noexcept {
  if (from == to) {
    return false;
  }
  switch (from) {
    case ExperimentState::OPEN:
      return to == ExperimentState::FINALIZED || to == ExperimentState::CANCELLED ||
             to == ExperimentState::SUPERSEDED || to == ExperimentState::RECOVERING;
    case ExperimentState::RECOVERING:
      return to == ExperimentState::OPEN || to == ExperimentState::CANCELLED || to == ExperimentState::SUPERSEDED;
    case ExperimentState::FINALIZED:
    case ExperimentState::CANCELLED:
    case ExperimentState::SUPERSEDED:
      return to == ExperimentState::SUPERSEDED && from != ExperimentState::SUPERSEDED;
  }
  return false;
}

bool is_valid_transition(BranchState from, BranchState to) noexcept {
  if (from == to) {
    return false;
  }
  switch (from) {
    case BranchState::ACTIVE:
      return to == BranchState::RETIRED || to == BranchState::INVALID || to == BranchState::SUPERSEDED;
    case BranchState::RETIRED:
      return to == BranchState::ACTIVE || to == BranchState::SUPERSEDED;
    case BranchState::INVALID:
      return to == BranchState::SUPERSEDED;
    case BranchState::SUPERSEDED:
      return false;
  }
  return false;
}

Status invalid_transition_status(std::string_view what, std::string_view from, std::string_view to) {
  std::string detail;
  detail.append(what);
  detail.append(": invalid transition ");
  detail.append(from);
  detail.append(" -> ");
  detail.append(to);
  return make_error(ErrorCode::INVALID_TRANSITION, ErrorStage::LIFECYCLE, std::move(detail));
}

std::string_view to_string(TrialState state) noexcept { return kTrialStates.lookup(state); }
std::string_view to_string(ExperimentState state) noexcept { return kExperimentStates.lookup(state); }
std::string_view to_string(BranchState state) noexcept { return kBranchStates.lookup(state); }
std::string_view to_string(AttemptState state) noexcept { return kAttemptStates.lookup(state); }
std::string_view to_string(BranchRole role) noexcept { return kBranchRoles.lookup(role); }
std::string_view to_string(MetricKind kind) noexcept { return kMetricKinds.lookup(kind); }
std::string_view to_string(MetricDirection direction) noexcept { return kMetricDirections.lookup(direction); }
std::string_view to_string(AggregationRule rule) noexcept { return kAggregationRules.lookup(rule); }
std::string_view to_string(ObservationValidity validity) noexcept { return kObservationValidities.lookup(validity); }
std::string_view to_string(ComparisonOp op) noexcept { return kComparisonOps.lookup(op); }
std::string_view to_string(DecisionOutcome outcome) noexcept { return kDecisionOutcomes.lookup(outcome); }
std::string_view to_string(FailureKind kind) noexcept { return kFailureKinds.lookup(kind); }
std::string_view to_string(ReproducibilityStatus status) noexcept { return kReproducibilityStatuses.lookup(status); }
std::string_view to_string(SeedPolicy policy) noexcept { return kSeedPolicies.lookup(policy); }
std::string_view to_string(RetryPolicy policy) noexcept { return kRetryPolicies.lookup(policy); }
std::string_view to_string(PartialFailurePolicy policy) noexcept { return kPartialFailurePolicies.lookup(policy); }
std::string_view to_string(LateEvidencePolicy policy) noexcept { return kLateEvidencePolicies.lookup(policy); }
std::string_view to_string(Provenance provenance) noexcept { return kProvenances.lookup(provenance); }
std::string_view to_string(ParameterKind kind) noexcept { return kParameterKinds.lookup(kind); }

std::optional<TrialState> parse_trial_state(std::string_view name) noexcept { return kTrialStates.reverse(name); }
std::optional<ExperimentState> parse_experiment_state(std::string_view name) noexcept { return kExperimentStates.reverse(name); }
std::optional<BranchState> parse_branch_state(std::string_view name) noexcept { return kBranchStates.reverse(name); }
std::optional<AttemptState> parse_attempt_state(std::string_view name) noexcept { return kAttemptStates.reverse(name); }
std::optional<BranchRole> parse_branch_role(std::string_view name) noexcept { return kBranchRoles.reverse(name); }
std::optional<MetricKind> parse_metric_kind(std::string_view name) noexcept { return kMetricKinds.reverse(name); }
std::optional<MetricDirection> parse_metric_direction(std::string_view name) noexcept { return kMetricDirections.reverse(name); }
std::optional<AggregationRule> parse_aggregation_rule(std::string_view name) noexcept { return kAggregationRules.reverse(name); }
std::optional<ObservationValidity> parse_observation_validity(std::string_view name) noexcept { return kObservationValidities.reverse(name); }
std::optional<ComparisonOp> parse_comparison_op(std::string_view name) noexcept { return kComparisonOps.reverse(name); }
std::optional<DecisionOutcome> parse_decision_outcome(std::string_view name) noexcept { return kDecisionOutcomes.reverse(name); }
std::optional<FailureKind> parse_failure_kind(std::string_view name) noexcept { return kFailureKinds.reverse(name); }
std::optional<ReproducibilityStatus> parse_reproducibility_status(std::string_view name) noexcept { return kReproducibilityStatuses.reverse(name); }
std::optional<SeedPolicy> parse_seed_policy(std::string_view name) noexcept { return kSeedPolicies.reverse(name); }
std::optional<RetryPolicy> parse_retry_policy(std::string_view name) noexcept { return kRetryPolicies.reverse(name); }
std::optional<PartialFailurePolicy> parse_partial_failure_policy(std::string_view name) noexcept { return kPartialFailurePolicies.reverse(name); }
std::optional<LateEvidencePolicy> parse_late_evidence_policy(std::string_view name) noexcept { return kLateEvidencePolicies.reverse(name); }
std::optional<Provenance> parse_provenance(std::string_view name) noexcept { return kProvenances.reverse(name); }
std::optional<ParameterKind> parse_parameter_kind(std::string_view name) noexcept { return kParameterKinds.reverse(name); }

}  // namespace experiment_fabric
