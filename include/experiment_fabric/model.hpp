// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_MODEL_HPP
#define EXPERIMENT_FABRIC_MODEL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "experiment_fabric/domain.hpp"
#include "experiment_fabric/hash.hpp"
#include "experiment_fabric/identity.hpp"
#include "experiment_fabric/limits.hpp"

namespace experiment_fabric {

/// \file
/// The governed experiment object model.
///
/// These structures are the durable systems object that Experiment Fabric
/// governs. They are domain-neutral: no scientific, statistical or
/// machine-learning assumption is baked in.

/// A canonical key/value parameter assignment. The textual form is normalised
/// so that two equal assignments always produce equal canonical encodings.
struct ParameterAssignment {
  std::string key;
  ParameterKind kind = ParameterKind::TEXT;
  std::string value;  ///< canonical text form of the value

  friend bool operator==(const ParameterAssignment&, const ParameterAssignment&) = default;
};

/// Builds an assignment from typed inputs, normalising the text form.
[[nodiscard]] ParameterAssignment make_text_parameter(std::string key, std::string value);
[[nodiscard]] ParameterAssignment make_integer_parameter(std::string key, std::int64_t value);
[[nodiscard]] ParameterAssignment make_real_parameter(std::string key, double value);
[[nodiscard]] ParameterAssignment make_boolean_parameter(std::string key, bool value);

/// Canonical ordering for parameter lists.
[[nodiscard]] bool parameter_less(const ParameterAssignment& lhs, const ParameterAssignment& rhs) noexcept;

// ---------------------------------------------------------------------------
// Hypothesis
// ---------------------------------------------------------------------------

/// An explicit, revisioned hypothesis.
struct Hypothesis {
  HypothesisId id;
  HypothesisRevision revision;
  std::string statement;
  /// Direction in which the hypothesis predicts the metric will move. Absent
  /// when the hypothesis does not predict a direction.
  std::optional<MetricDirection> expected_direction;
  /// Metric the hypothesis concerns. Null when not metric specific.
  MetricId expected_metric;
  /// Creation lineage: the hypothesis this one was derived from.
  HypothesisId parent;
  HypothesisRevision parent_revision;
  bool superseded = false;
  HypothesisRevision superseded_by_revision;
  std::string provenance;
  std::uint64_t created_sequence = 0;
};

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

/// Typed metric definition.
struct MetricDefinition {
  MetricId id;
  std::string name;
  std::string unit;
  MetricKind kind = MetricKind::REAL;
  MetricDirection direction = MetricDirection::MINIMIZE;
  AggregationRule aggregation = AggregationRule::MEAN;
  bool required = true;
  /// Whether NaN and infinities are meaningful for this metric. When false
  /// (the default) a non-finite value is an INVALID observation, never a
  /// silently accepted number.
  bool accepts_non_finite = false;
  std::optional<double> lower_bound;
  std::optional<double> upper_bound;
  std::optional<double> target_low;
  std::optional<double> target_high;
  /// Weight used only by policies that explicitly opt into weighted scoring.
  std::uint32_t weight = 1;
  /// Lexicographic priority. Lower values are evaluated first.
  std::uint32_t priority = 0;
  /// Permitted categories for CATEGORICAL metrics. Empty means unrestricted.
  std::vector<std::string> categories;
};

/// A typed metric value. Exactly one payload member is meaningful, selected by
/// \c kind. An absent value is represented by \c ObservationValidity, never by
/// a sentinel numeric value.
struct MetricValue {
  MetricKind kind = MetricKind::REAL;
  double real = 0.0;
  std::int64_t integer = 0;
  bool boolean = false;
  std::string category;
};

[[nodiscard]] bool is_finite_value(const MetricValue& value) noexcept;

// ---------------------------------------------------------------------------
// Comparison policy
// ---------------------------------------------------------------------------

/// One deterministic comparison rule.
struct ComparisonRule {
  MetricId metric;
  ComparisonOp op = ComparisonOp::MIN_IMPROVEMENT;
  /// Improvement / tolerance magnitude in the metric unit.
  double threshold = 0.0;
  double target_low = 0.0;
  double target_high = 0.0;
  /// Lexicographic priority. Lower values are decisive first.
  std::uint32_t priority = 0;
};

/// Deterministic comparison policy. Exactly one methodology is never assumed:
/// every rule is explicit, and statistical confidence is only claimed when a
/// statistical component is actually configured and implemented.
struct ComparisonPolicy {
  PolicyId id;
  PolicyGeneration generation;
  bool require_baseline = true;
  std::uint32_t min_completed_trials_per_branch = 1;
  std::uint32_t min_valid_observations_per_required_metric = 1;
  bool reject_on_any_invalid_observation = false;
  bool require_environment_match = true;
  /// Fraction of paired trial-level wins required by MAJORITY_WINS, in basis
  /// points (0..10000).
  std::uint32_t majority_wins_min_basis_points = 5000;
  /// When true the policy computes an explicitly configured weighted score from
  /// metric weights. When false no score is computed at all.
  bool use_weighted_score = false;
  double accept_score_threshold = 0.0;
  std::vector<ComparisonRule> rules;
};

// ---------------------------------------------------------------------------
// Branch and experiment definition
// ---------------------------------------------------------------------------

/// One branch inside an experiment generation.
struct BranchDefinition {
  BranchId id;
  std::string name;
  BranchRole role = BranchRole::CANDIDATE;
  BranchId parent;
  BranchGeneration generation;
  BranchState state = BranchState::ACTIVE;
  std::vector<ParameterAssignment> parameters;
  InputSetId input_set;
  bool input_set_present = false;
  EnvironmentId environment;
  bool environment_present = false;
  std::string environment_fingerprint;
  std::optional<std::uint64_t> seed;
  std::uint32_t planned_trials = 1;
  std::uint64_t created_sequence = 0;
  /// Set when this branch was created by forking another branch.
  bool forked = false;
};

/// The canonical experiment definition for one generation.
struct ExperimentDefinition {
  ExperimentId id;
  ExperimentGeneration generation;
  HypothesisId hypothesis;
  HypothesisRevision hypothesis_revision;
  std::string name;
  std::vector<BranchDefinition> branches;
  std::vector<MetricDefinition> metrics;
  ComparisonPolicy policy;
  SeedPolicy seed_policy = SeedPolicy::DERIVED_PER_TRIAL;
  std::optional<std::uint64_t> experiment_seed;
  RetryPolicy retry_policy = RetryPolicy::FRESH_ATTEMPT_SAME_TRIAL;
  std::uint32_t max_attempts_per_trial = 2;
  PartialFailurePolicy partial_failure_policy = PartialFailurePolicy::TOLERATE_IF_EVIDENCE_SUFFICIENT;
  LateEvidencePolicy late_evidence_policy = LateEvidencePolicy::REJECT_AND_RECORD;
  std::vector<ParameterAssignment> parameters;
  std::string provenance;
  std::uint64_t created_sequence = 0;
};

// ---------------------------------------------------------------------------
// Trials, attempts
// ---------------------------------------------------------------------------

/// A logical trial. Attempts are fenced beneath it; exactly one attempt may
/// commit the authoritative logical result.
struct TrialRecord {
  TrialId id;
  ExperimentId experiment;
  ExperimentGeneration generation;
  BranchId branch;
  BranchGeneration branch_generation;
  HypothesisId hypothesis;
  HypothesisRevision hypothesis_revision;
  PolicyId policy;
  PolicyGeneration policy_generation;
  TrialState state = TrialState::CREATED;
  TrialAttemptId authoritative_attempt;
  TrialAttemptNumber next_attempt_number = TrialAttemptNumber::from_value(1);
  std::uint32_t attempt_count = 0;
  std::uint32_t committed_attempts = 0;
  FailureKind failure_kind = FailureKind::NONE;
  std::string failure_detail;
  std::optional<std::uint64_t> seed;
  InputSetId input_set;
  bool input_set_present = false;
  EnvironmentId environment;
  bool environment_present = false;
  std::string environment_fingerprint;
  std::vector<ArtifactId> artifacts;
  /// Opaque producer payload carried from trial creation to the executing
  /// worker. Experiment Fabric never interprets it.
  std::string payload;
  std::uint64_t created_sequence = 0;
  std::uint64_t updated_sequence = 0;
};

/// One execution attempt of a logical trial.
struct AttemptRecord {
  TrialAttemptId id;
  TrialAttemptNumber number;
  TrialId trial;
  ExperimentId experiment;
  ExperimentGeneration generation;
  BranchId branch;
  BranchGeneration branch_generation;
  WorkerId worker;
  WorkerBootId boot;
  CoordinatorEpoch epoch;
  ProducerId producer;
  AttemptState state = AttemptState::ISSUED;
  FailureKind failure_kind = FailureKind::NONE;
  std::string failure_detail;
  bool committed = false;
  std::uint64_t issued_sequence = 0;
  std::uint64_t updated_sequence = 0;
};

// ---------------------------------------------------------------------------
// Observations and artifacts
// ---------------------------------------------------------------------------

/// One metric observation emitted by one attempt.
struct Observation {
  ObservationId id;
  ExperimentId experiment;
  ExperimentGeneration generation;
  BranchId branch;
  BranchGeneration branch_generation;
  TrialId trial;
  TrialAttemptId attempt;
  MetricId metric;
  ProducerId producer;
  WorkerId worker;
  WorkerBootId boot;
  CoordinatorEpoch epoch;
  ObservationValidity validity = ObservationValidity::VALID;
  MetricValue value;
  std::string detail;
  std::uint64_t sequence = 0;
};

/// A bounded artifact reference produced by a trial attempt.
///
/// Experiment Fabric records that an artifact exists and which attempt produced
/// it. It never marks an artifact as production trusted.
struct ArtifactRecord {
  ArtifactId id;
  ExperimentId experiment;
  ExperimentGeneration generation;
  BranchId branch;
  TrialId trial;
  TrialAttemptId attempt;
  std::string category;
  std::string locator;
  Sha256::Digest content_digest{};
  bool digest_present = false;
  bool size_present = false;
  std::uint64_t size_bytes = 0;
  std::string provenance;
  /// False once the producing generation has been superseded.
  bool current = true;
  std::uint64_t sequence = 0;
};

// ---------------------------------------------------------------------------
// Evidence views
// ---------------------------------------------------------------------------

/// Deterministic aggregate of one metric over one branch.
struct MetricAggregate {
  MetricId metric;
  AggregationRule rule = AggregationRule::MEAN;
  bool present = false;
  double value = 0.0;
  std::string category;
  std::uint32_t valid_count = 0;
  std::uint32_t invalid_count = 0;
  std::uint32_t missing_count = 0;
  std::uint32_t unsupported_count = 0;
  /// Sorted trial-level raw values used to build the aggregate.
  std::vector<double> raw_values;
  /// For MAJORITY_WINS pairing: trial -> candidate value ordering.
  std::vector<std::string> trial_order;
};

/// Per-branch evidence summary computed from current authoritative state.
struct BranchEvidence {
  BranchId branch;
  BranchGeneration generation;
  BranchRole role = BranchRole::CANDIDATE;
  BranchState state = BranchState::ACTIVE;
  std::uint32_t planned_trials = 0;
  std::uint32_t completed_trials = 0;
  std::uint32_t failed_trials = 0;
  std::uint32_t cancelled_trials = 0;
  std::uint32_t invalid_trials = 0;
  std::uint32_t superseded_trials = 0;
  std::uint32_t open_trials = 0;
  std::vector<MetricAggregate> aggregates;
};

/// One rejected piece of evidence, retained for auditability.
struct RejectedEvidence {
  std::string kind;   ///< "observation" or "artifact"
  std::uint64_t identifier = 0;
  ErrorCode reason = ErrorCode::OK;
  std::string detail;
};

/// One deterministic comparison factor.
struct ComparisonFactor {
  MetricId metric;
  ComparisonOp op = ComparisonOp::MIN_IMPROVEMENT;
  std::uint32_t priority = 0;
  double threshold = 0.0;
  bool has_values = false;
  double baseline_value = 0.0;
  double candidate_value = 0.0;
  double delta = 0.0;
  bool satisfied = false;
  bool decisive = false;
  std::string detail;
};

/// Result of applying hard evidence constraints.
struct ConstraintResult {
  std::string name;
  bool passed = false;
  std::string detail;
};

/// An authoritative governed decision.
struct Decision {
  DecisionId id;
  ExperimentId experiment;
  ExperimentGeneration generation;
  HypothesisId hypothesis;
  HypothesisRevision hypothesis_revision;
  PolicyId policy;
  PolicyGeneration policy_generation;
  std::vector<BranchId> compared_branches;
  BranchId baseline;
  bool baseline_present = false;
  BranchId candidate;
  DecisionOutcome outcome = DecisionOutcome::INSUFFICIENT_EVIDENCE;
  FailureKind reason_kind = FailureKind::INSUFFICIENT_EVIDENCE;
  std::string reason;
  std::vector<ConstraintResult> constraints;
  std::vector<ComparisonFactor> factors;
  std::vector<BranchEvidence> evidence;
  std::vector<RejectedEvidence> rejected_evidence;
  std::string tie_break_rule;
  bool tied = false;
  Sha256::Digest canonical_state_digest{};
  std::uint64_t sequence = 0;
};

/// Authority of one branch at the moment a rollback point was recorded.
struct BranchAuthoritySnapshot {
  BranchId branch;
  BranchGeneration generation;
  BranchState state = BranchState::ACTIVE;
};

/// A restorable authoritative historical point.
///
/// A rollback point records what was authoritative, not merely which generation
/// existed, so that a rollback can restore an earlier authoritative state
/// without discarding the history that came after it.
struct RollbackPoint {
  RollbackPointId id;
  ExperimentId experiment;
  /// The authoritative generation this point records.
  ExperimentGeneration generation;
  /// Set when this point was created by a rollback; names the generation the
  /// rollback restored from. Zero for points created by authoritative
  /// finalization.
  ExperimentGeneration restored_from;
  DecisionId decision;
  bool decision_present = false;
  Sha256::Digest state_digest{};
  std::vector<BranchAuthoritySnapshot> authority;
  std::uint64_t sequence = 0;
};

// ---------------------------------------------------------------------------
// Reproducibility
// ---------------------------------------------------------------------------

/// Reproducibility evidence for one branch of one experiment generation.
struct ReproducibilityRecord {
  ExperimentId experiment;
  ExperimentGeneration generation;
  HypothesisId hypothesis;
  HypothesisRevision hypothesis_revision;
  BranchId branch;
  BranchGeneration branch_generation;
  PolicyId policy;
  PolicyGeneration policy_generation;
  Sha256::Digest definition_digest{};
  Sha256::Digest branch_definition_digest{};
  Sha256::Digest policy_digest{};
  InputSetId input_set;
  bool input_set_present = false;
  Sha256::Digest input_set_digest{};
  bool input_set_digest_present = false;
  EnvironmentId environment;
  bool environment_present = false;
  std::string environment_fingerprint;
  std::string executable_identity;
  std::string producer_identity;
  std::optional<std::uint64_t> seed;
  std::vector<ParameterAssignment> parameters;
  std::vector<ArtifactId> artifacts;
  std::uint32_t observation_count = 0;
  /// Materials the runtime knows are needed but does not have. Sorted.
  std::vector<std::string> missing_materials;
  ReproducibilityStatus status = ReproducibilityStatus::UNKNOWN;
};

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_MODEL_HPP
