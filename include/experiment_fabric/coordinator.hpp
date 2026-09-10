// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_COORDINATOR_HPP
#define EXPERIMENT_FABRIC_COORDINATOR_HPP

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "experiment_fabric/authority.hpp"
#include "experiment_fabric/domain.hpp"
#include "experiment_fabric/error.hpp"
#include "experiment_fabric/identity.hpp"
#include "experiment_fabric/limits.hpp"
#include "experiment_fabric/model.hpp"
#include "experiment_fabric/persistence.hpp"
#include "experiment_fabric/protocol.hpp"
#include "experiment_fabric/state.hpp"
#include "experiment_fabric/transport.hpp"

namespace experiment_fabric {

/// \file
/// The governance core.
///
/// The coordinator owns one coherent durable state and one live process
/// authority. Every authoritative mutation validates against a single
/// generation, commits under one lock, and publishes events only after the lock
/// is released.
///
/// Lock ordering (strict, lowest acquisition first):
///   1. Coordinator::state_mutex_   (shared for reads, unique for mutations)
///   2. Coordinator::workers_mutex_ (worker registry and connection table)
/// No state lock is ever held while network I/O, persistence I/O, or an
/// observer callback runs.

// ---------------------------------------------------------------------------
// Specification objects
// ---------------------------------------------------------------------------

/// Declarative metric specification. Names are resolved to MetricId at
/// definition time; the coordinator never lets a caller choose identity values.
struct MetricSpec {
  std::string name;
  std::string unit;
  MetricKind kind = MetricKind::REAL;
  MetricDirection direction = MetricDirection::MINIMIZE;
  AggregationRule aggregation = AggregationRule::MEAN;
  bool required = true;
  bool accepts_non_finite = false;
  std::optional<double> lower_bound;
  std::optional<double> upper_bound;
  std::optional<double> target_low;
  std::optional<double> target_high;
  std::uint32_t weight = 1;
  std::uint32_t priority = 0;
  std::vector<std::string> categories;
};

/// Declarative branch specification.
struct BranchSpec {
  std::string name;
  BranchRole role = BranchRole::CANDIDATE;
  /// Name of an existing branch to fork from. Null creates a root branch.
  std::optional<std::string> fork_from;
  std::vector<ParameterAssignment> parameters;
  std::optional<std::string> input_set;
  std::optional<std::string> environment;
  std::string environment_fingerprint;
  std::optional<std::uint64_t> seed;
  std::uint32_t planned_trials = 1;
};

/// Declarative comparison rule, referencing metrics by name.
struct ComparisonRuleSpec {
  std::string metric;
  ComparisonOp op = ComparisonOp::MIN_IMPROVEMENT;
  double threshold = 0.0;
  double target_low = 0.0;
  double target_high = 0.0;
  std::uint32_t priority = 0;
};

/// Complete declarative experiment specification.
struct ExperimentSpec {
  std::string name;
  std::string hypothesis_statement;
  std::optional<MetricDirection> expected_direction;
  std::optional<std::string> expected_metric;
  std::string provenance;

  std::vector<BranchSpec> branches;
  std::vector<MetricSpec> metrics;

  bool require_baseline = true;
  std::uint32_t min_completed_trials_per_branch = 1;
  std::uint32_t min_valid_observations_per_required_metric = 1;
  bool reject_on_any_invalid_observation = false;
  bool require_environment_match = true;
  std::uint32_t majority_wins_min_basis_points = 5000;
  bool use_weighted_score = false;
  double accept_score_threshold = 0.0;
  std::vector<ComparisonRuleSpec> rules;

  SeedPolicy seed_policy = SeedPolicy::DERIVED_PER_TRIAL;
  std::optional<std::uint64_t> experiment_seed;
  RetryPolicy retry_policy = RetryPolicy::FRESH_ATTEMPT_SAME_TRIAL;
  std::uint32_t max_attempts_per_trial = 2;
  PartialFailurePolicy partial_failure_policy = PartialFailurePolicy::TOLERATE_IF_EVIDENCE_SUFFICIENT;
  LateEvidencePolicy late_evidence_policy = LateEvidencePolicy::REJECT_AND_RECORD;

  std::vector<ParameterAssignment> parameters;
};

/// Declarative hypothesis revision request.
struct HypothesisRevisionSpec {
  std::string statement;
  std::optional<MetricDirection> expected_direction;
  /// Null when the revision concerns no specific metric.
  MetricId expected_metric;
  std::string provenance;
};

/// Deterministically derives the seed of one trial from the experiment seed and
/// the trial identity. Exposed so that a reproducibility record can be checked
/// independently of the runtime that produced it.
[[nodiscard]] EF_API std::uint64_t derive_trial_seed(std::uint64_t experiment_seed,
                                                     std::uint64_t ordinal) noexcept;

// ---------------------------------------------------------------------------
// Read-only snapshots
// ---------------------------------------------------------------------------

/// Trials with their attempt history, ordered by TrialId.
struct TrialSnapshot {
  TrialRecord trial;
  std::vector<AttemptRecord> attempts;
};

/// A complete, internally consistent read-only view of one experiment.
struct ExperimentSnapshot {
  ExperimentDefinition definition;
  Hypothesis hypothesis;
  std::vector<Hypothesis> hypothesis_history;
  ExperimentState state = ExperimentState::OPEN;
  CoordinatorEpoch producing_epoch;
  std::uint64_t revision_sequence = 0;
  std::vector<TrialSnapshot> trials;
  std::vector<Observation> observations;
  std::vector<ArtifactRecord> artifacts;
  std::vector<Decision> decisions;
  std::vector<RollbackPoint> rollback_points;
  std::vector<RejectedEvidence> rejected_evidence;
  std::vector<std::string> lifecycle_log;
  Sha256::Digest state_digest{};
};

/// Observer notified after authoritative mutations commit. Callbacks run with
/// no coordinator lock held, so they may safely re-enter read-only APIs.
class CoordinatorObserver {
 public:
  virtual ~CoordinatorObserver() = default;
  virtual void on_event(const std::string& event) { (void)event; }
  virtual void on_decision(const Decision& decision) { (void)decision; }
};

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------

class EF_API Coordinator {
 public:
  struct Config {
    Limits limits{};
    std::filesystem::path state_path;
    std::string bind_address = "127.0.0.1";
    std::uint16_t listen_port = 0;
    bool persist_on_mutation = true;
    /// 0 means derive from the durable epoch counter and the process clock.
    std::uint64_t epoch_seed = 0;
    std::uint32_t max_connection_threads = 16;
    CoordinatorObserver* observer = nullptr;
  };

  /// Creates a coordinator. Durable state is loaded from \c config.state_path
  /// when the file exists, otherwise a fresh state is created. Loaded dynamic
  /// authority is never adopted: workers must re-register under the new epoch.
  [[nodiscard]] static Result<std::unique_ptr<Coordinator>> create(Config config);

  ~Coordinator();
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  // ---- Server ----------------------------------------------------------
  /// Starts the framed TCP control plane on the configured loopback address.
  [[nodiscard]] Status start_server();
  /// Stops admitting work, closes the listener and joins connection threads.
  [[nodiscard]] Status stop_server();

  /// Requests an orderly stop. Called by the control plane when a client sends
  /// SHUTDOWN, and by a supervising process. No new authority is granted
  /// afterwards.
  void request_shutdown();

  /// Blocks until a shutdown has been requested. Used by a supervising process
  /// so that it never has to poll.
  void wait_for_shutdown_request();

  [[nodiscard]] bool shutdown_requested() const noexcept;
  [[nodiscard]] bool server_running() const noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept;

  // ---- Process authority ----------------------------------------------
  [[nodiscard]] CoordinatorEpoch epoch() const noexcept;
  [[nodiscard]] std::uint64_t sequence() const noexcept;

  // ---- Definition ------------------------------------------------------
  [[nodiscard]] Result<ExperimentId> create_experiment(const ExperimentSpec& spec);
  [[nodiscard]] Result<HypothesisRevision> revise_hypothesis(ExperimentId experiment,
                                                             const HypothesisRevisionSpec& spec);
  [[nodiscard]] Result<BranchId> fork_branch(ExperimentId experiment, BranchId parent, const BranchSpec& spec);
  [[nodiscard]] Result<BranchId> retire_branch(ExperimentId experiment, BranchId branch, std::string reason);
  [[nodiscard]] Result<TrialId> create_trial(ExperimentId experiment, BranchId branch, std::string payload);

  // ---- Evidence --------------------------------------------------------
  [[nodiscard]] Status publish_observation(const PublishObservationRequest& request);
  [[nodiscard]] Status publish_artifact(const PublishArtifactRequest& request);
  [[nodiscard]] Status commit_trial(const CommitTrialRequest& request);
  [[nodiscard]] Status fail_trial(const FailTrialRequest& request);

  // ---- Worker lifecycle ------------------------------------------------
  [[nodiscard]] Result<RegisterWorkerReply> register_worker(const RegisterWorkerRequest& request);
  [[nodiscard]] Status revoke_worker(WorkerId worker, WorkerBootId boot, FailureKind kind, std::string reason);
  [[nodiscard]] Result<ClaimTrialReply> claim_trials(const ClaimTrialRequest& request);
  [[nodiscard]] Status heartbeat(const HeartbeatRequest& request);
  [[nodiscard]] Result<std::vector<std::string>> worker_lines() const;

  // ---- Cancellation, decision, rollback --------------------------------
  [[nodiscard]] Status cancel_trial(TrialId trial, std::string reason);
  [[nodiscard]] Status cancel_experiment(ExperimentId experiment, std::string reason);
  [[nodiscard]] Result<Decision> evaluate(ExperimentId experiment, BranchId candidate) const;
  [[nodiscard]] Result<Decision> finalize_experiment(ExperimentId experiment, BranchId candidate,
                                                     std::string reason);
  [[nodiscard]] Status rollback(ExperimentId experiment, ExperimentGeneration target, std::string reason);

  // ---- Queries ---------------------------------------------------------
  [[nodiscard]] Result<std::vector<ExperimentId>> list_experiments() const;
  [[nodiscard]] Result<ExperimentSnapshot> snapshot(ExperimentId experiment) const;
  [[nodiscard]] Result<ReproducibilityRecord> reproducibility(ExperimentId experiment, BranchId branch) const;
  [[nodiscard]] Result<std::vector<std::string>> query_lines(QueryKind kind, const std::string& argument) const;
  [[nodiscard]] Result<std::string> explain(ExperimentId experiment, BranchId candidate) const;

  // ---- Recovery --------------------------------------------------------
  /// Marks every persisted dynamic authority as requiring revalidation, then
  /// resolves it conservatively. Called automatically during creation.
  [[nodiscard]] Status reconcile_recovered_state();

  // ---- Persistence -----------------------------------------------------
  [[nodiscard]] Status save();
  [[nodiscard]] Status validate_state() const;
  [[nodiscard]] Result<std::string> state_digest_hex() const;
  [[nodiscard]] const Limits& limits() const noexcept;

 private:
  explicit Coordinator(Config config);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_COORDINATOR_HPP
