// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_DOMAIN_HPP
#define EXPERIMENT_FABRIC_DOMAIN_HPP

#include <cstdint>
#include <optional>
#include <string_view>

#include "experiment_fabric/error.hpp"

namespace experiment_fabric {

/// \file
/// Domain enumerations and the explicit lifecycle state machines they drive.
///
/// Enumerations cross persistence and transport boundaries, so every value is
/// assigned an explicit stable number and every decoder must reject out-of-range
/// values rather than clamping them.

// ---------------------------------------------------------------------------
// Lifecycle states
// ---------------------------------------------------------------------------

/// Kind of a canonical parameter value. Parameter values cross persistence and
/// transport boundaries, so their textual form is canonicalised.
enum class ParameterKind : std::uint8_t {
  TEXT = 0,
  INTEGER = 1,
  REAL = 2,
  BOOLEAN = 3,
};

[[nodiscard]] std::string_view to_string(ParameterKind kind) noexcept;
[[nodiscard]] std::optional<ParameterKind> parse_parameter_kind(std::string_view name) noexcept;

/// Lifecycle of a logical trial. A logical trial spans one or more attempts.
enum class TrialState : std::uint8_t {
  CREATED = 0,
  READY = 1,
  ASSIGNED = 2,
  RUNNING = 3,
  OBSERVING = 4,
  COMPLETED = 5,
  FAILED = 6,
  CANCELLED = 7,
  INVALID = 8,
  SUPERSEDED = 9,
  /// Recovered dynamic state that must be reconciled before it can advance.
  REVALIDATION_REQUIRED = 10,
};

/// Lifecycle of a persisted experiment.
enum class ExperimentState : std::uint8_t {
  OPEN = 0,
  FINALIZED = 1,
  CANCELLED = 2,
  SUPERSEDED = 3,
  /// Loaded from persistence with live process authority that must be
  /// reacquired before further authoritative mutation.
  RECOVERING = 4,
};

/// Lifecycle of a branch.
enum class BranchState : std::uint8_t {
  ACTIVE = 0,
  RETIRED = 1,
  INVALID = 2,
  SUPERSEDED = 3,
};

/// Role of a branch inside one experiment generation.
enum class BranchRole : std::uint8_t {
  BASELINE = 0,
  CANDIDATE = 1,
};

/// Whether an attempt is still able to commit authoritative evidence.
enum class AttemptState : std::uint8_t {
  ISSUED = 0,
  RUNNING = 1,
  PUBLISHED = 2,
  COMPLETED = 3,
  FAILED = 4,
  CANCELLED = 5,
  ABANDONED = 6,
  REVALIDATION_REQUIRED = 7,
};

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

/// Storage form of a metric value.
enum class MetricKind : std::uint8_t {
  REAL = 0,
  INTEGER = 1,
  BOOLEAN = 2,
  CATEGORICAL = 3,
};

/// Which direction is better for a metric.
enum class MetricDirection : std::uint8_t {
  MINIMIZE = 0,
  MAXIMIZE = 1,
  TARGET = 2,
  INFORMATIONAL = 3,
};

/// How raw trial observations become branch evidence.
enum class AggregationRule : std::uint8_t {
  /// No aggregation; only the raw observation set is evidence.
  NONE = 0,
  MEAN = 1,
  MEDIAN = 2,
  MIN = 3,
  MAX = 4,
  SUM = 5,
  /// Most frequent category; ties are broken by lowest canonical category text.
  MAJORITY = 6,
};

/// Validity of a single observation. These states are never collapsed into one
/// another: MISSING is not zero, UNSUPPORTED is not INVALID, and INVALID is not
/// FAILED.
enum class ObservationValidity : std::uint8_t {
  VALID = 0,
  INVALID = 1,
  MISSING = 2,
  UNSUPPORTED = 3,
};

// ---------------------------------------------------------------------------
// Comparison and decision
// ---------------------------------------------------------------------------

/// One deterministic comparison primitive.
enum class ComparisonOp : std::uint8_t {
  /// Candidate aggregate must be greater than baseline by c threshold.
  GREATER_THAN = 0,
  /// Candidate aggregate must be less than baseline by c threshold.
  LESS_THAN = 1,
  /// Candidate aggregate must be greater than baseline by at least c threshold.
  MIN_IMPROVEMENT = 2,
  /// Candidate aggregate must not be worse than baseline by more than c threshold.
  MAX_REGRESSION_TOLERANCE = 3,
  /// Candidate aggregate must lie inside [low, high].
  TARGET_RANGE = 4,
  /// At least c threshold fraction (0..1) of paired trials must favour the candidate.
  MAJORITY_WINS = 5,
  /// Informational: reported but never decisive.
  REPORT_ONLY = 6,
};

/// Outcome of a governed comparison.
enum class DecisionOutcome : std::uint8_t {
  ACCEPT = 0,
  REJECT = 1,
  INCONCLUSIVE = 2,
  INSUFFICIENT_EVIDENCE = 3,
  INVALID = 4,
  CANCELLED = 5,
  SUPERSEDED = 6,
};

/// Typed classification of why an attempt or trial did not produce usable
/// evidence. Internally distinct; never collapsed into a single FAILED bucket.
enum class FailureKind : std::uint8_t {
  NONE = 0,
  EXECUTION_FAILURE = 1,
  INVALID_OBSERVATION = 2,
  PRODUCER_LOSS = 3,
  TRANSPORT_FAILURE = 4,
  CANCELLATION = 5,
  POLICY_REJECTION = 6,
  INSUFFICIENT_EVIDENCE = 7,
  PERSISTENCE_FAILURE = 8,
  EXPERIMENT_INVALIDATION = 9,
  LIMIT_EXCEEDED = 10,
};

/// Explicit reproducibility classification.
enum class ReproducibilityStatus : std::uint8_t {
  REPRODUCIBLE = 0,
  PARTIALLY_REPRODUCIBLE = 1,
  NOT_REPRODUCIBLE = 2,
  UNKNOWN = 3,
};

/// Seed policy for trial parameter generation.
enum class SeedPolicy : std::uint8_t {
  /// Every trial uses the experiment-level seed verbatim.
  FIXED = 0,
  /// Trial seed is derived deterministically from (experiment seed, trial ordinal).
  DERIVED_PER_TRIAL = 1,
  /// Producer supplies the seed; stored but not derived.
  PRODUCER_SUPPLIED = 2,
  /// No seed is required for this experiment.
  NONE = 3,
};

/// How a retry relates to the logical trial identity.
enum class RetryPolicy : std::uint8_t {
  /// Retry preserves the logical TrialId and issues a fresh TrialAttemptId.
  FRESH_ATTEMPT_SAME_TRIAL = 0,
  /// Retry allocates a brand new logical TrialId.
  NEW_TRIAL_IDENTITY = 1,
  /// No retry is permitted.
  NO_RETRY = 2,
};

/// What happens to the experiment when a trial fails.
enum class PartialFailurePolicy : std::uint8_t {
  /// The branch can still be decided from the remaining valid trials.
  TOLERATE_IF_EVIDENCE_SUFFICIENT = 0,
  /// Any terminal failure makes the branch undecidable.
  INVALIDATE_BRANCH = 1,
  /// The trial may be retried; the failure is recorded but not fatal.
  RETRY_ALLOWED = 2,
};

/// What happens to evidence that arrives after cancellation.
enum class LateEvidencePolicy : std::uint8_t {
  /// Late evidence is rejected and recorded only in the rejection log.
  REJECT_AND_RECORD = 0,
  /// Late evidence is retained as historical, never as current authority.
  RETAIN_HISTORICAL = 1,
};

/// Provenance labels required for every environment-dependent claim.
enum class Provenance : std::uint8_t {
  REAL = 0,
  SYNTHETIC = 1,
  UNSUPPORTED = 2,
};

// ---------------------------------------------------------------------------
// State machine helpers
// ---------------------------------------------------------------------------

/// True when no further transition may leave the state.
[[nodiscard]] bool is_terminal(TrialState state) noexcept;

/// True when the trial state can no longer be advanced by an attempt.
[[nodiscard]] bool is_trial_closed(TrialState state) noexcept;

/// True when the experiment accepts new authoritative mutation.
[[nodiscard]] bool is_experiment_mutable(ExperimentState state) noexcept;

/// True when the attempt can no longer publish authoritative evidence.
[[nodiscard]] bool is_attempt_terminal(AttemptState state) noexcept;

/// True when the branch may receive authoritative new trial evidence.
[[nodiscard]] bool is_branch_eligible(BranchState state) noexcept;

/// Validates a trial lifecycle transition. Terminal states accept no outgoing
/// transition; REVALIDATION_REQUIRED may only move to a conservative state.
[[nodiscard]] bool is_valid_transition(TrialState from, TrialState to) noexcept;

/// Validates an attempt transition.
[[nodiscard]] bool is_valid_transition(AttemptState from, AttemptState to) noexcept;

/// Validates an experiment transition.
[[nodiscard]] bool is_valid_transition(ExperimentState from, ExperimentState to) noexcept;

/// Validates a branch transition.
[[nodiscard]] bool is_valid_transition(BranchState from, BranchState to) noexcept;

/// Returns the status describing an invalid transition.
[[nodiscard]] Status invalid_transition_status(std::string_view what, std::string_view from,
                                               std::string_view to);

// ---- Stable names used by persistence, transport and the inspection CLI ----

[[nodiscard]] std::string_view to_string(TrialState state) noexcept;
[[nodiscard]] std::string_view to_string(ExperimentState state) noexcept;
[[nodiscard]] std::string_view to_string(BranchState state) noexcept;
[[nodiscard]] std::string_view to_string(AttemptState state) noexcept;
[[nodiscard]] std::string_view to_string(BranchRole role) noexcept;
[[nodiscard]] std::string_view to_string(MetricKind kind) noexcept;
[[nodiscard]] std::string_view to_string(MetricDirection direction) noexcept;
[[nodiscard]] std::string_view to_string(AggregationRule rule) noexcept;
[[nodiscard]] std::string_view to_string(ObservationValidity validity) noexcept;
[[nodiscard]] std::string_view to_string(ComparisonOp op) noexcept;
[[nodiscard]] std::string_view to_string(DecisionOutcome outcome) noexcept;
[[nodiscard]] std::string_view to_string(FailureKind kind) noexcept;
[[nodiscard]] std::string_view to_string(ReproducibilityStatus status) noexcept;
[[nodiscard]] std::string_view to_string(SeedPolicy policy) noexcept;
[[nodiscard]] std::string_view to_string(RetryPolicy policy) noexcept;
[[nodiscard]] std::string_view to_string(PartialFailurePolicy policy) noexcept;
[[nodiscard]] std::string_view to_string(LateEvidencePolicy policy) noexcept;
[[nodiscard]] std::string_view to_string(Provenance provenance) noexcept;

// ---- Strict parsing. Returns nullopt for unknown names. ----

[[nodiscard]] std::optional<TrialState> parse_trial_state(std::string_view name) noexcept;
[[nodiscard]] std::optional<ExperimentState> parse_experiment_state(std::string_view name) noexcept;
[[nodiscard]] std::optional<BranchState> parse_branch_state(std::string_view name) noexcept;
[[nodiscard]] std::optional<AttemptState> parse_attempt_state(std::string_view name) noexcept;
[[nodiscard]] std::optional<BranchRole> parse_branch_role(std::string_view name) noexcept;
[[nodiscard]] std::optional<MetricKind> parse_metric_kind(std::string_view name) noexcept;
[[nodiscard]] std::optional<MetricDirection> parse_metric_direction(std::string_view name) noexcept;
[[nodiscard]] std::optional<AggregationRule> parse_aggregation_rule(std::string_view name) noexcept;
[[nodiscard]] std::optional<ObservationValidity> parse_observation_validity(std::string_view name) noexcept;
[[nodiscard]] std::optional<ComparisonOp> parse_comparison_op(std::string_view name) noexcept;
[[nodiscard]] std::optional<DecisionOutcome> parse_decision_outcome(std::string_view name) noexcept;
[[nodiscard]] std::optional<FailureKind> parse_failure_kind(std::string_view name) noexcept;
[[nodiscard]] std::optional<ReproducibilityStatus> parse_reproducibility_status(std::string_view name) noexcept;
[[nodiscard]] std::optional<SeedPolicy> parse_seed_policy(std::string_view name) noexcept;
[[nodiscard]] std::optional<RetryPolicy> parse_retry_policy(std::string_view name) noexcept;
[[nodiscard]] std::optional<PartialFailurePolicy> parse_partial_failure_policy(std::string_view name) noexcept;
[[nodiscard]] std::optional<LateEvidencePolicy> parse_late_evidence_policy(std::string_view name) noexcept;
[[nodiscard]] std::optional<Provenance> parse_provenance(std::string_view name) noexcept;

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_DOMAIN_HPP
