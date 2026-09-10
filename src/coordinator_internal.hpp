// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_SRC_COORDINATOR_INTERNAL_HPP
#define EXPERIMENT_FABRIC_SRC_COORDINATOR_INTERNAL_HPP

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>

#include "experiment_fabric/canonical.hpp"
#include "experiment_fabric/coordinator.hpp"
#include "experiment_fabric/state.hpp"

namespace experiment_fabric {

/// Live registration of one worker process incarnation.
///
/// This structure is deliberately *not* persisted: it represents process
/// authority that a restarted coordinator must never inherit.
struct LiveWorker {
  WorkerId worker;
  WorkerBootId boot;
  ProducerId producer;
  std::string endpoint;
  std::string fingerprint;
  std::uint32_t active_assignments = 0;
  std::uint32_t total_assignments = 0;
  std::uint32_t max_concurrent_assignments = 1;
  bool revoked = false;
  FailureKind revocation_kind = FailureKind::NONE;
  std::uint64_t registered_sequence = 0;
};

/// Outcome of validating one authority envelope against live coordinator state.
struct EnvelopeCheck {
  AuthorityVerdict verdict;
  ExperimentRecord* record = nullptr;
  TrialRecord* trial = nullptr;
  AttemptRecord* attempt = nullptr;
  BranchDefinition* branch = nullptr;
};

/// Owns the coordinator's server-side thread pool. Defined in
/// coordinator_server.cpp so that the coordinator core does not depend on the
/// transport implementation.
class CoordinatorServerRuntime;

struct Coordinator::Impl {
  Impl(Config config_in, Limits limits_in) : config(std::move(config_in)), limits(limits_in) {}
  /// Out-of-line so that the incomplete server runtime type can be held.
  ~Impl();

  Config config;
  Limits limits;

  /// Single state lock. Ordering: this is the only coordinator lock acquired
  /// before any other, and it is never held across network I/O, file I/O, or an
  /// observer callback. Worker liveness and durable state are guarded together
  /// so that an authority decision is always made against one coherent view.
  mutable std::shared_mutex mutex;

  CoordinatorState state;
  CoordinatorEpoch epoch;
  bool admitting_work = true;
  bool persistence_degraded = false;

  std::map<std::pair<std::uint64_t, std::uint64_t>, LiveWorker> live_workers;
  std::map<ExperimentId, std::map<ObservationId, std::size_t>> observation_index;
  std::map<ExperimentId, std::map<ArtifactId, std::size_t>> artifact_index;

  /// Owned raw pointer rather than std::unique_ptr so that this header does not
  /// need the complete server-runtime type. Deleted in ~Impl.
  CoordinatorServerRuntime* server = nullptr;
  std::atomic<bool> server_active{false};

  /// Shutdown rendezvous. A supervising process blocks on this condition
  /// variable, so no polling loop and no timed wait is required anywhere.
  mutable std::mutex shutdown_mutex;
  mutable std::condition_variable shutdown_signal;
  bool shutdown_flagged = false;

  // ---- identity allocation (caller must hold the state lock) ----
  ExperimentId alloc_experiment_id();
  HypothesisId alloc_hypothesis_id();
  BranchId alloc_branch_id();
  TrialId alloc_trial_id();
  TrialAttemptId alloc_attempt_id();
  ObservationId alloc_observation_id();
  ArtifactId alloc_artifact_id();
  DecisionId alloc_decision_id();
  RollbackPointId alloc_rollback_id();
  std::uint64_t next_sequence();
  std::uint64_t current_sequence() const;

  ExperimentRecord* find_experiment(ExperimentId id);
  const ExperimentRecord* find_experiment(ExperimentId id) const;
  BranchDefinition* find_branch(ExperimentRecord& record, BranchId id);
  const BranchDefinition* find_branch(const ExperimentRecord& record, BranchId id) const;
  const MetricDefinition* find_metric(const ExperimentRecord& record, MetricId id) const;

  LiveWorker* find_live_worker(WorkerId worker, WorkerBootId boot);

  /// Validates a complete authority envelope against live coordinator state.
  /// Caller must hold the state lock.
  EnvelopeCheck check_envelope(const AuthorityEnvelope& envelope);
  void rebuild_indexes();
  void log(ExperimentRecord& record, std::string line);

  /// Serialises and durably commits the whole state. Called with the state lock
  /// held so that a mutation is never observable in memory before it is durable.
  Status persist_locked();
  Status persist_locked(bool required);

  /// Computes current authoritative evidence for every branch of an experiment.
  std::vector<BranchEvidence> compute_evidence(const ExperimentRecord& record) const;

  /// Applies hard evidence constraints. Returns the constraint list; the
  /// caller derives the decision outcome.
  void apply_constraints(const ExperimentRecord& record, const BranchDefinition& candidate,
                         const BranchDefinition* baseline, const std::vector<BranchEvidence>& evidence,
                         std::vector<ConstraintResult>& constraints, DecisionOutcome& outcome,
                         FailureKind& reason_kind, std::string& reason) const;

  /// Builds a complete decision for a candidate branch.
  Decision build_decision(const ExperimentRecord& record, BranchId candidate, DecisionId id,
                          std::uint64_t sequence) const;

  /// Counts committed, current authoritative trials of a branch.
  std::vector<const TrialRecord*> authoritative_trials(const ExperimentRecord& record, BranchId branch) const;

  void abandon_stale_live_work(ExperimentRecord& record, ExperimentGeneration generation, std::uint64_t sequence);
  void notify_event(const std::string& event) const;
  void notify_decision(const Decision& decision) const;
};

/// Deterministically derives a per-trial seed.
[[nodiscard]] std::uint64_t derive_trial_seed(std::uint64_t experiment_seed, std::uint64_t ordinal) noexcept;

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_SRC_COORDINATOR_INTERNAL_HPP
