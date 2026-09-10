// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_STATE_HPP
#define EXPERIMENT_FABRIC_STATE_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "experiment_fabric/domain.hpp"
#include "experiment_fabric/hash.hpp"
#include "experiment_fabric/identity.hpp"
#include "experiment_fabric/limits.hpp"
#include "experiment_fabric/model.hpp"

namespace experiment_fabric {

/// \file
/// The durable coordinator state model.
///
/// Everything in this header is persisted. Nothing in this header represents
/// live process authority: \c CoordinatorEpoch and \c WorkerBootId are minted
/// at process start and are deliberately absent from the durable model except as
/// historical record on attempts and observations.

/// One worker incarnation recorded for audit. Historical only.
struct WorkerIncarnation {
  WorkerId worker;
  WorkerBootId boot;
  CoordinatorEpoch epoch;
  ProducerId producer;
  std::string endpoint;
  std::string fingerprint;
  std::uint64_t registered_sequence = 0;
  bool revoked = false;
  FailureKind revocation_kind = FailureKind::NONE;
  std::uint64_t revoked_sequence = 0;
};

/// Durable record for one experiment.
struct ExperimentRecord {
  ExperimentDefinition definition;
  Hypothesis hypothesis;
  std::vector<Hypothesis> hypothesis_history;
  ExperimentState state = ExperimentState::OPEN;
  std::uint64_t revision_sequence = 0;
  std::uint64_t created_sequence = 0;

  std::map<TrialId, TrialRecord> trials;
  std::map<TrialAttemptId, AttemptRecord> attempts;
  std::vector<Observation> observations;
  std::vector<ArtifactRecord> artifacts;
  std::vector<Decision> decisions;
  std::vector<RollbackPoint> rollback_points;
  std::vector<RejectedEvidence> rejected_evidence;
  /// Ordered record of experiment-level lifecycle events for auditability.
  std::vector<std::string> lifecycle_log;
};

/// The complete durable state owned by one coordinator.
struct CoordinatorState {
  std::uint32_t format_version = 0;
  /// Highest coordinator epoch ever minted. Guarantees epochs strictly increase
  /// across independent process restarts even when the clock is unreliable.
  std::uint64_t last_epoch = 0;
  std::uint64_t last_sequence = 0;
  std::map<ExperimentId, ExperimentRecord> experiments;
  std::vector<WorkerIncarnation> workers;
  std::uint64_t next_worker_registration_sequence = 0;

  /// Watermarks for identity allocators so restored state never re-issues an
  /// identity that historical records already used.
  std::uint64_t id_watermark_experiment = 0;
  std::uint64_t id_watermark_hypothesis = 0;
  std::uint64_t id_watermark_branch = 0;
  std::uint64_t id_watermark_metric = 0;
  std::uint64_t id_watermark_policy = 0;
  std::uint64_t id_watermark_trial = 0;
  std::uint64_t id_watermark_attempt = 0;
  std::uint64_t id_watermark_observation = 0;
  std::uint64_t id_watermark_artifact = 0;
  std::uint64_t id_watermark_decision = 0;
  std::uint64_t id_watermark_rollback = 0;

  /// Validates the entire durable state: identity uniqueness, cross references,
  /// branch acyclicity, bounded collections, and generation coherence.
  [[nodiscard]] Status validate(const Limits& limits) const;

  /// Counts every persisted record. Used to bound decoding.
  [[nodiscard]] std::uint64_t total_record_count() const noexcept;

  /// Deterministic digest of one experiment's authoritative durable content.
  [[nodiscard]] Sha256::Digest experiment_digest(ExperimentId id) const;
};

/// Validates one experiment record in isolation: identity uniqueness, cross
/// references, branch acyclicity, generation coherence and bounded collections.
///
/// Mutations validate the record they changed rather than the whole state, so
/// that a mutation costs O(changed record) instead of O(all experiments). The
/// complete-state check remains available through \c CoordinatorState::validate
/// and runs on every load.
[[nodiscard]] Status validate_experiment_record(const ExperimentRecord& record, const Limits& limits,
                                                ExperimentId id);

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_STATE_HPP
